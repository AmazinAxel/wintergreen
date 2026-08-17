#include "Application.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "HeapLog.h"
#include "content/CoverPaths.h"
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
  menu_.set_app(this);
  reader_.set_app(this);
  reader_options_.set_app(this);
  hidden_books_.set_app(this);


  // Set up settings file path if data_dir_ is set
  if (data_dir_)
    settings_path_ = std::string(data_dir_) + "/settings";

  load_settings_();

  // Apply persisted menu font size to all list screens.
  ListMenuScreen::set_font_size(kMenuFontSize);

  buf.set_rotation(rotation_from_setting(rotate_display())); // rotaton

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
      auto_open_book(pending_book_path_.c_str(), buf, runtime);
    } else {
      MR_LOGI("app", "skipping auto-open (no valid font) — starting from book list");
    }
    pending_book_path_.clear();
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
// Draw `book_path`'s cover as the sleep screen. Returns false when the book has
// no full-res cover, in which case the caller falls back to the wordmark image.
static bool show_book_cover_sleep_(DrawBuffer& buf, const char* data_dir, const std::string& book_path) {
  if (!data_dir || book_path.empty()) return false;

  // Load the pre-extracted full-res 1-bit cover for the sleep screen.
  const std::string cpath = cover_sleep_bin_path(book_path.c_str(), data_dir);
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
  // The reader now persists position only in stop(), so if a child screen (the
  // quick menu) is on top it must be stopped too or the session is lost.
  IScreen* top = screen_mgr_.top();
  if (top)
    top->stop();
  if (top != &reader_ && screen_mgr_.contains(&reader_))
    reader_.stop();
  save_settings_();

  // Reset rotation before drawing the sleep screen — it is always portrait.
  buf.set_rotation(Rotation::Deg90);

  // The cover of the most recently opened book is the default sleep screen.
  // The embedded wintergreen image is kept as the fallback, and is what a
  // power-button long-press should show once that gesture is wired up.
  // The cover shows only while a book is actually open, and never for a hidden
  // one — the sleep screen is on display while the device sits around, so a
  // hidden book must not be advertised by it. Anything else gets the wordmark.
  std::string cover_book;
  if (!wordmark_image && screen_mgr_.contains(&reader_) && reader_.has_path()) {
    const std::string p = reader_.get_path();
    if (p.find("/.hidden/") == std::string::npos)
      cover_book = p;
  }

  bool shown = false;
  if (!cover_book.empty())
    shown = show_book_cover_sleep_(buf, data_dir_, cover_book);
  if (!shown)
    shown = buf.show_sleep_image_embedded(show_sleep_text());

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

  // Power button: hold to sleep, tap to select. The tap resolves on release —
  // the only way to tell it apart from a hold — and is forwarded to the screen
  // as a synthetic Button1 (Confirm) press, so screens need no power handling.
  ButtonState fwd = buttons_;
  if (buttons_.is_pressed(Button::Power)) {
    power_armed_ = true;
    power_hold_ms_ = 0;
  } else if (power_armed_ && buttons_.is_down(Button::Power)) {
    power_hold_ms_ += dt_ms;
    if (power_hold_ms_ >= power_hold_sleep_ms()) {
      power_armed_ = false;
      do_sleep_(buf);
      return;
    }
  } else if (power_armed_) {
    power_armed_ = false;
    const uint8_t confirm = static_cast<uint8_t>(Button::Button1);
    fwd.pressed_latch |= static_cast<uint8_t>(1u << confirm);
    if (fwd.press_history_count < ButtonState::kMaxPressHistory)
      fwd.press_history[fwd.press_history_count++] = confirm;
  }
  // Screens never see the raw power key.
  fwd.pressed_latch &= static_cast<uint8_t>(~(1u << static_cast<uint8_t>(Button::Power)));
  fwd.current &= static_cast<uint8_t>(~(1u << static_cast<uint8_t>(Button::Power)));
  {
    uint8_t n = 0;
    for (uint8_t i = 0; i < fwd.press_history_count; ++i)
      if (static_cast<Button>(fwd.press_history[i]) != Button::Power)
        fwd.press_history[n++] = fwd.press_history[i];
    fwd.press_history_count = n;
  }

  IScreen* top = screen_mgr_.top();
  if (top) {
    top->update(fwd, buf, runtime);

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
    case ScreenId::HiddenBooks:
      return &hidden_books_;
    case ScreenId::LyraExt:
      return &lyra_ext_;
    default:
      return nullptr;
  }
}
void wintergreen::Application::save_settings_() {
  if (settings_path_.empty())
    return;

  ReaderScreen* reader = &reader_;
  const bool reader_active = screen_mgr_.contains(reader);

  std::string out;
  char line[600];
  if (reader_active && reader->has_path() && reader->get_path().find("/.hidden/") == std::string::npos) {
    std::snprintf(line, sizeof(line), "book_path=%s\n", reader->get_path().c_str());
    out += line;
  }
  const ReaderSettings& rs = reader->reader_settings();
  std::snprintf(line, sizeof(line), "font_size=%u\n", static_cast<unsigned>(rs.font_size_idx));
  out += line;
  std::snprintf(line, sizeof(line), "rotate_reader=%u\n", static_cast<unsigned>(rotate_reader_));
  out += line;

  if (out == settings_written_) // dont write to file if nothing changed
    return;

  const std::string tmp_path = settings_path_ + ".tmp";
  FILE* f = std::fopen(tmp_path.c_str(), "w");
  if (!f)
    return;
  const bool wrote = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  if (std::fclose(f) != 0 || !wrote) {
    std::remove(tmp_path.c_str());
    return;
  }

  // Write-to-temp + rename is not a backup — no copy of the old file is kept.
  // It is what stops fopen(path, "w") truncating the real file to zero bytes
  // before writing a byte, on a device that saves during auto-sleep.
  //
  // The remove() is required, not tidiness: FatFs f_rename returns FR_EXIST
  // when the destination exists, and ESP-IDF's VFS passes it straight through
  // without unlinking first — so unlike POSIX, rename() here does NOT replace.
  // Without this every save after the first silently failed and left a .tmp.
  std::remove(settings_path_.c_str());
  if (std::rename(tmp_path.c_str(), settings_path_.c_str()) == 0)
    settings_written_ = out;
}


void wintergreen::Application::record_book_opened(const std::string& path) {
  BookIndex::instance().mark_opened(path);
  if (data_dir_) {
    std::string index_path = std::string(data_dir_) + "/book_index.dat";
    BookIndex::instance().save(index_path);
  }
  save_settings_();
}
void wintergreen::Application::load_settings_() {
  if (settings_path_.empty())
    return;
  FILE* f = std::fopen(settings_path_.c_str(), "r");
  if (!f) {
    // Power was lost between the remove() and the rename() in save_settings_.
    // The .tmp is a complete file, so prefer it to starting from defaults.
    const std::string tmp_path = settings_path_ + ".tmp";
    f = std::fopen(tmp_path.c_str(), "r");
    if (f)
      MR_LOGI("app", "settings: recovered from %s", tmp_path.c_str());
  }
  if (!f)
    return;

  char line[512];
  std::string last_book_path;
  ReaderSettings& rs = reader_.reader_settings();

  while (std::fgets(line, sizeof(line), f)) {
    // Strip trailing newline
    char* nl = std::strchr(line, '\n');
    if (nl)
      *nl = 0;

    char sval[512];
    unsigned uval = 0;
    if (std::sscanf(line, "book_path=%511[^\n]", sval) == 1)
      last_book_path = sval;
    else if (std::sscanf(line, "font_size=%u", &uval) == 1)
      rs.font_size_idx = uval < kMaxFontSizes ? static_cast<uint8_t>(uval) : 1;
    else if (std::sscanf(line, "rotate_reader=%u", &uval) == 1)
      rotate_reader_ = static_cast<uint8_t>(uval <= 3 ? uval : 0);
  }
  std::fclose(f);

  MR_LOGI("app", "Loaded settings: font_size=%u", rs.font_size_idx);


  // Store the book to auto-open; actual push happens in start() after buf is ready.
  pending_book_path_ = last_book_path;
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
