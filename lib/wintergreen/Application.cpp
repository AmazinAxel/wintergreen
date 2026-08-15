#include "Application.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "HeapLog.h"
#include "content/Book.h"
#include "content/BookIndex.h"
#include "content/ImageDecoder.h"
#include "screens/ListMenuScreen.h"

#ifdef ESP_PLATFORM
#include <dirent.h>
#include <sys/stat.h>

#include "esp_random.h"
#else
#include <filesystem>
#endif

#define WINTERGREEN_VERSION "0.1"

#ifndef ESP_PLATFORM
namespace fs = std::filesystem;
#endif

namespace wintergreen {

void Application::start(DrawBuffer& buf, IRuntime& runtime) {
  ticks_ = 0;
  uptime_ms_ = 0;
  buttons_ = ButtonState{};
  started_ = true;
  running_ = true;

#ifdef ESP_PLATFORM
  std::srand(esp_random());
#else
  std::srand(static_cast<unsigned>(std::time(nullptr)));
#endif

  if (reader_font_)
    reader_.set_fonts(reader_font_);

  lyra_ext_.set_app(this);
  recent_books_.set_app(this);
  menu_.set_app(this);
  reader_.set_app(this);
  reader_options_.set_app(this);
  chapter_select_.set_app(this);
  links_screen_.set_app(this);
  hidden_books_.set_app(this);


  // Set up settings file path if data_dir_ is set
  if (data_dir_)
    settings_path_ = std::string(data_dir_) + "/settings";

  // Load settings first so initial_selection_ and reader settings are ready
  // before the menu's on_start() (directory scan + selection restore) runs.
  load_settings_();

  // Apply persisted menu font size to all list screens.
  ListMenuScreen::set_font_size(kMenuFontSize);

  // Apply persisted display rotation.
  buf.set_rotation(rotation_from_setting(rotate_display()));

  // Apply persisted image toggle.
  images_enabled = show_reader_images();

  screen_mgr_.push(&lyra_ext_, buf, runtime);

  // Don't auto-open books from the hidden folder — they're meant to stay private.
  if (!pending_book_path_.empty() && pending_book_path_.find("/.hidden/") != std::string::npos)
    pending_book_path_.clear();

  // Auto-open last book if one was active at shutdown — but only if the font
  // is valid. cache_only=true tells the reader not to convert if the MRB is
  // missing; it will pop back to the book list instead of blocking the UI.
  if (!pending_book_path_.empty()) {
    MR_LOGI("app", "auto-open: '%s'", pending_book_path_.c_str());
    if (reader_font_ && reader_font_->valid()) {
      reader_.set_cache_only(true);
      auto_open_book(pending_book_path_.c_str(), buf, runtime);
    } else {
      MR_LOGI("app", "skipping auto-open (no valid font) — starting from book list");
    }
    pending_book_path_.clear();
  }

  pending_screen_.clear();

  // Update last seen version silently (What's New screen removed).
  if (last_seen_version_ != WINTERGREEN_VERSION) {
    last_seen_version_ = WINTERGREEN_VERSION;
    save_settings_();
  }

  buf.full_refresh();
}

void Application::auto_open_book(const char* epub_path, DrawBuffer& buf, IRuntime& runtime) {
  reader_.set_path(epub_path);
  if (reader_font_)
    reader_.set_fonts(reader_font_);

  screen_mgr_.push(&reader_, buf, runtime);
}

// Scale-to-fit the most recent book's cover onto the sleep screen (white letterbox).
// Returns true and does a full refresh if successful.
static bool show_book_cover_sleep_(DrawBuffer& buf, const char* data_dir) {
  if (!data_dir) return false;

  // Find most recently opened book in the index.
  std::string best_path;
  uint32_t best_order = 0;
  BookIndex::instance().load(std::string(data_dir) + "/book_index.dat");
  const StringPool& pool = BookIndex::instance().pool();
  for (const auto& e : BookIndex::instance().entries()) {
    if (e.last_open_order > best_order) {
      best_order = e.last_open_order;
      best_path  = e.path.to_string(pool);
    }
  }
  if (best_path.empty()) return false;

  // Load the pre-extracted full-res 1-bit cover for the sleep screen.
  const std::string cpath = cover_sleep_bin_path(best_path.c_str(), data_dir);
  FILE* f = std::fopen(cpath.c_str(), "rb");
  if (!f) return false;

  uint16_t hdr[2] = {};
  if (std::fread(hdr, 2, 2, f) != 2) { std::fclose(f); return false; }
  const int cw = static_cast<int>(hdr[0]);
  const int ch = static_cast<int>(hdr[1]);
  const size_t stride  = (static_cast<size_t>(cw) + 7) / 8;
  const size_t data_sz = stride * static_cast<size_t>(ch);
  if (cw <= 0 || ch <= 0 || data_sz == 0 || data_sz > 65536) { std::fclose(f); return false; }

  std::vector<uint8_t> data(data_sz);
  const bool ok = (std::fread(data.data(), 1, data_sz, f) == data_sz);
  std::fclose(f);
  if (!ok) return false;

  // Scale-to-fit within W×H, preserving aspect ratio, white letterbox.
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);  // white background

  int dst_w = W;
  int dst_h = W * ch / cw;
  if (dst_h > H) {
    dst_h = H;
    dst_w = H * cw / ch;
  }
  const int ox = (W - dst_w) / 2;
  const int oy = (H - dst_h) / 2;

  // Nearest-neighbour blit row-by-row.
  uint8_t row_buf[80];
  if ((dst_w + 7) / 8 > static_cast<int>(sizeof(row_buf))) return false;
  for (int dy = 0; dy < dst_h; ++dy) {
    const int sy = dy * ch / dst_h;
    const uint8_t* src_row = data.data() + static_cast<size_t>(sy) * stride;
    std::memset(row_buf, 0xFF, sizeof(row_buf));  // 1 = white
    for (int dx = 0; dx < dst_w; ++dx) {
      const int sx  = dx * cw / dst_w;
      const int bit = (src_row[sx >> 3] >> (7 - (sx & 7))) & 1;
      if (bit == 0)  // 0 = black pixel
        row_buf[dx >> 3] &= static_cast<uint8_t>(~(0x80u >> (dx & 7)));
    }
    buf.blit_1bit_row(ox, oy + dy, row_buf, dst_w);
  }

  buf.full_refresh(RefreshMode::Full, /*turnOffScreen=*/true);
  buf.deep_sleep();
  return true;
}

void Application::do_sleep_(DrawBuffer& buf, bool wordmark_image) {
  // Stop the active screen so it can save state (e.g. reading position).
  if (IScreen* top = screen_mgr_.top())
    top->stop();
  save_settings_();

  // Reset rotation before drawing the sleep screen — it is always portrait.
  buf.set_rotation(Rotation::Deg90);

  // The cover of the most recently opened book is the default sleep screen.
  // The embedded wintergreen image is kept as the fallback, and is what a
  // power-button long-press should show once that gesture is wired up.
  bool shown = false;
  if (!wordmark_image)
    shown = show_book_cover_sleep_(buf, data_dir_);
  if (!shown)
    shown = buf.show_sleep_image_embedded(0, show_sleep_text());

  MR_LOGI("sleep", "sleep screen shown: %d (wordmark=%d)", (int)shown, (int)wordmark_image);
  if (!shown)
    buf.deep_sleep();

  running_ = false;
}

void Application::update(const ButtonState& buttons, uint32_t dt_ms, DrawBuffer& buf, IRuntime& runtime) {
  if (!started_)
    start(buf, runtime);
  if (!running_)
    return;

  ++ticks_;
  uptime_ms_ += dt_ms;
  buttons_ = buttons;
  buf.set_sunlight_fading_fix(sunlight_fading_fix());

  // Inactivity / auto-sleep tracking
  if (buttons_.current != 0 || buttons_.pressed_latch != 0) {
    inactivity_ms_ = 0;
  } else {
    inactivity_ms_ += dt_ms;
    if (sleep_timeout_min() > 0) {
      const uint32_t timeout_ms = static_cast<uint32_t>(sleep_timeout_min()) * 60u * 1000u;
      if (inactivity_ms_ >= timeout_ms) {
        MR_LOGI("app", "auto-sleep after %u ms idle", inactivity_ms_);
        do_sleep_(buf);
        return;
      }
    }
  }

  if (buttons_.is_pressed(Button::Power)) {
    do_sleep_(buf);
    return;
  }

  IScreen* top = screen_mgr_.top();
  if (top) {
    top->update(buttons_, buf, runtime);

    if (!font_warning_shown_ && font_manager_ && font_manager_->any_corrupt()) {
      font_warning_shown_ = true;
      MR_LOGI("app", "font glyphs out of bounds — font file may be corrupt");
    }

    // Process pending navigation (queued by screens via push_screen/replace_screen).
    if (pending_replace_ != ScreenId::None) {
      ScreenId id = pending_replace_;
      pending_replace_ = ScreenId::None;
      screen_mgr_.pop(buf, runtime);
      screen_mgr_.push(screen_for_(id), buf, runtime);
      buf.refresh();
    } else if (pending_push_ != ScreenId::None) {
      ScreenId id = pending_push_;
      pending_push_ = ScreenId::None;
      screen_mgr_.push(screen_for_(id), buf, runtime);
      buf.refresh();
    } else if (pending_pop_count_ > 0) {
      int count = pending_pop_count_;
      pending_pop_count_ = 0;
      if (top == &reader_ || top == &reader_options_)
        save_settings_();
      screen_mgr_.pop(count, buf, runtime);
      buf.refresh();
    }
  }
}  // namespace wintergreen

IScreen* wintergreen::Application::screen_for_(ScreenId id) {
  switch (id) {
    case ScreenId::MainMenu:
      return &menu_;
    case ScreenId::Reader:
      return &reader_;
    case ScreenId::ReaderOptions:
      return &reader_options_;
    case ScreenId::ChapterSelect:
      return &chapter_select_;
    case ScreenId::Links:
      return &links_screen_;
    case ScreenId::HiddenBooks:
      return &hidden_books_;
    case ScreenId::LyraExt:
      return &lyra_ext_;
    case ScreenId::RecentBooks:
      return &recent_books_;
    default:
      return nullptr;
  }
}
void wintergreen::Application::save_settings_() {
  if (settings_path_.empty())
    return;
  const std::string tmp_path = settings_path_ + ".tmp";
  FILE* f = std::fopen(tmp_path.c_str(), "w");
  if (!f)
    return;

  // Version tag
  std::fprintf(f, "v=1\n");

  // Last screen / book — treat reader-is-anywhere-in-stack as "reader" so
  // shutting down from ReaderOptionsScreen still boots back into the reader.
  ReaderScreen* reader = &reader_;
  const bool reader_active = screen_mgr_.contains(reader);

  std::fprintf(f, "screen=%s\n", reader_active ? "reader" : "menu");

  if (reader_active && reader->has_path() && reader->get_path().find("/.hidden/") == std::string::npos)
    std::fprintf(f, "book_path=%s\n", reader->get_path().c_str());

  // Last book-list selection: prefer the currently highlighted entry so
  // power-off while browsing still saves position; fall back to last opened.
  const std::string& sel =
      !menu_.current_book_path().empty() ? menu_.current_book_path() : menu_.last_selected_book_path();
  if (!sel.empty())
    std::fprintf(f, "book_sel=%s\n", sel.c_str());

  // Reader display settings
  const ReaderSettings& rs = reader->reader_settings();
  std::fprintf(f, "align_override=%u\n", static_cast<unsigned>(rs.align_override));
  std::fprintf(f, "padding_h=%u\n", static_cast<unsigned>(rs.padding_h_idx));
  std::fprintf(f, "padding_v=%u\n", static_cast<unsigned>(rs.padding_v_idx));
  std::fprintf(f, "spacing_override=%u\n", static_cast<unsigned>(rs.spacing_override));
  std::fprintf(f, "progress=%u\n", static_cast<unsigned>(rs.progress_style));
  std::fprintf(f, "progress_scope=%u\n", static_cast<unsigned>(rs.progress_scope));
  std::fprintf(f, "override_pub_fonts=%u\n", rs.override_publisher_fonts ? 1u : 0u);
  std::fprintf(f, "hyphenation=%u\n", rs.hyphenation_enabled ? 1u : 0u);
  std::fprintf(f, "font_size=%u\n", static_cast<unsigned>(rs.font_size_idx));

  std::fprintf(f, "open_counter=%u\n", static_cast<unsigned>(open_counter_));
  std::fprintf(f, "rotate_reader=%u\n", static_cast<unsigned>(rotate_reader_));

  if (!last_seen_version_.empty())
    std::fprintf(f, "last_version=%s\n", last_seen_version_.c_str());

  if (std::fclose(f) != 0) {
    std::remove(tmp_path.c_str());
    return;
  }

  // Atomic replace: the old file stays intact until the new one is complete, so
  // losing power mid-save can't truncate the reading position. No backup copies
  // — rename() already guarantees we never observe a half-written file.
  std::rename(tmp_path.c_str(), settings_path_.c_str());
}


void wintergreen::Application::update_book_read_time(const std::string& path, uint64_t ms,
                                                     uint32_t times_opened, uint32_t page_turns,
                                                     int progress_pct, uint64_t time_left_ms,
                                                     uint16_t chapter_count, uint32_t total_chars) {
  if (!data_dir_) return;
  const std::string index_path = std::string(data_dir_) + "/book_index.dat";
  BookIndex::instance().update_reading_stats(path, ms, times_opened, page_turns,
      static_cast<uint16_t>(progress_pct), chapter_count, time_left_ms, index_path, total_chars);
}

void wintergreen::Application::record_book_opened(const std::string& path) {
  BookIndex::instance().set_last_opened(path, ++open_counter_);
  if (data_dir_) {
    std::string index_path = std::string(data_dir_) + "/book_index.dat";
    BookIndex::instance().save(index_path);
  }
  save_settings_();
}
void Application::ensure_cover_bin(const std::string& epub_path,
                                    uint8_t* scratch1, uint8_t* scratch2,
                                    size_t scratch_size) {
  if (!data_dir_) return;
  const std::string cpath  = cover_bin_path(epub_path.c_str(), data_dir_);
  const std::string spath  = cover_sleep_bin_path(epub_path.c_str(), data_dir_);

  // cover.bin: small thumbnail (≤160×240). Stale if width > 200 (old 480px extract).
  bool need_thumb = true;
  FILE* chk = std::fopen(cpath.c_str(), "rb");
  if (chk) {
    uint16_t hdr[2] = {};
    need_thumb = (std::fread(hdr, 2, 2, chk) != 2 || hdr[0] > 200);
    std::fclose(chk);
  }

  // cover_sleep.bin: full-res (≤480×786). Stale if missing or width < 400.
  bool need_sleep = true;
  FILE* schk = std::fopen(spath.c_str(), "rb");
  if (schk) {
    uint16_t hdr[2] = {};
    need_sleep = (std::fread(hdr, 2, 2, schk) != 2 || hdr[0] < 400);
    std::fclose(schk);
  }

  if (!need_thumb && !need_sleep) return;

  // Create data_dir/cache/ and data_dir/cache/STEM/ before writing.
  const std::string cache_base = std::string(data_dir_) + "/cache";
  const size_t last_slash = cpath.rfind('/');
  const std::string stem_dir = (last_slash != std::string::npos) ? cpath.substr(0, last_slash) : cache_base;
#ifdef ESP_PLATFORM
  mkdir(cache_base.c_str(), 0775);
  mkdir(stem_dir.c_str(), 0775);
#else
  try { fs::create_directories(stem_dir); } catch (...) {}
#endif

  Book book;
  if (book.open(epub_path.c_str(), scratch1, scratch2) != EpubError::Ok) return;
  if (need_thumb) book.write_cover_bin(cpath.c_str(),  160, 240, scratch1, scratch_size);
  if (need_sleep) book.write_cover_bin(spath.c_str(),  480, 786, scratch1, scratch_size);
}

void wintergreen::Application::load_settings_() {
  if (settings_path_.empty())
    return;
  FILE* f = std::fopen(settings_path_.c_str(), "r");
  if (!f)
    return;

  char line[512];
  std::string last_screen, last_book_path, book_sel;
  ReaderSettings& rs = reader_.reader_settings();

  while (std::fgets(line, sizeof(line), f)) {
    // Strip trailing newline
    char* nl = std::strchr(line, '\n');
    if (nl)
      *nl = 0;

    char sval[512];
    unsigned uval = 0;
    if (std::sscanf(line, "screen=%511s", sval) == 1)
      last_screen = sval;
    else if (std::sscanf(line, "book_path=%511[^\n]", sval) == 1)
      last_book_path = sval;
    else if (std::sscanf(line, "book_sel=%511[^\n]", sval) == 1)
      book_sel = sval;
    else if (std::sscanf(line, "align_override=%u", &uval) == 1)
      rs.align_override =
          uval < ReaderSettings::kNumAlignPresets ? static_cast<AlignOverride>(uval) : AlignOverride::Book;
    else if (std::sscanf(line, "justify=%u", &uval) == 1)  // Backwards compatibility
      rs.align_override = uval != 0 ? AlignOverride::Justify : AlignOverride::Left;
    else if (std::sscanf(line, "padding_h=%u", &uval) == 1)
      rs.padding_h_idx = uval < ReaderSettings::kNumPresets ? static_cast<uint8_t>(uval) : 1;
    else if (std::sscanf(line, "padding_v=%u", &uval) == 1)
      rs.padding_v_idx = uval < ReaderSettings::kNumPresets ? static_cast<uint8_t>(uval) : 1;
    else if (std::sscanf(line, "spacing_override=%u", &uval) == 1)
      rs.spacing_override = uval < ReaderSettings::kNumSpacingPresets ? static_cast<SpacingOverride>(uval)
                                                                      : SpacingOverride::Spacing_1_0x;
    else if (std::sscanf(line, "line_spacing=%u", &uval) == 1)  // Backwards compatibility
      rs.spacing_override = SpacingOverride::Book;
    else if (std::sscanf(line, "progress=%u", &uval) == 1)
      rs.progress_style = uval <= 2 ? static_cast<ProgressStyle>(uval) : ProgressStyle::Bar;
    else if (std::sscanf(line, "progress_scope=%u", &uval) == 1)
      rs.progress_scope = uval <= 1 ? static_cast<ProgressScope>(uval) : ProgressScope::Book;
    else if (std::sscanf(line, "override_pub_fonts=%u", &uval) == 1)
      rs.override_publisher_fonts = (uval != 0);
    else if (std::sscanf(line, "hyphenation=%u", &uval) == 1)
      rs.hyphenation_enabled = (uval != 0);
    else if (std::sscanf(line, "font_size=%u", &uval) == 1)
      rs.font_size_idx = uval < kMaxFontSizes ? static_cast<uint8_t>(uval) : 1;
    else if (std::sscanf(line, "open_counter=%u", &uval) == 1)
      open_counter_ = uval;
    else if (std::sscanf(line, "rotate_reader=%u", &uval) == 1)
      rotate_reader_ = static_cast<uint8_t>(uval <= 3 ? uval : 0);
    else if (std::sscanf(line, "last_version=%511[^\n]", sval) == 1)
      last_seen_version_ = sval;
  }
  std::fclose(f);

  MR_LOGI("app", "Loaded settings: align=%u ph=%u pv=%u ls=%u prog=%u sel=%s", static_cast<unsigned>(rs.align_override),
          rs.padding_h_idx, rs.padding_v_idx, static_cast<unsigned>(rs.spacing_override),
          static_cast<unsigned>(rs.progress_style), book_sel.c_str());

  // Restore book list selection highlight
  if (!book_sel.empty())
    menu_.set_initial_selection(book_sel.c_str());


  // Store the book to auto-open; actual push happens in start() after buf is ready.
  if (last_screen == "reader" && !last_book_path.empty())
    pending_book_path_ = last_book_path;

  pending_screen_ = last_screen;
}

bool Application::running() const {
  return running_;
}
uint64_t Application::tick_count() const {
  return ticks_;
}
uint32_t Application::uptime_ms() const {
  return uptime_ms_;
}

}  // namespace wintergreen
