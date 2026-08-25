#include "QuickmenuScreen.h"

#include <climits>
#include <cstdio>
#include <cstring>

#include "../Application.h"
#include "../content/BookIndex.h"

namespace wintergreen {
constexpr uint8_t ReaderSettings::kNumFontSizePresets;
constexpr const char* ReaderSettings::kFontSizeNames[];

// ---------------------------------------------------------------------------

void QuickmenuScreen::populate(const TableOfContents& toc, uint16_t current_chapter, uint16_t current_para,
                                   const std::string& fallback_title, int book_progress_pct, int chapter_progress_pct,
                                   uint16_t chapter_count) {
  toc_ = &toc;
  chapter_count_ = chapter_count;
  book_title_ = fallback_title;
  chapter_title_ = fallback_title;
  int best_match = -1;
  for (size_t i = 0; i < toc_->entries.size(); ++i) {
    if (toc_->entries[i].file_idx < current_chapter ||
        (toc_->entries[i].file_idx == current_chapter && toc_->entries[i].para_index <= current_para)) {
      best_match = static_cast<int>(i);
    }
  }
  if (best_match >= 0) {
    chapter_title_ = toc_->entries[best_match].label.to_string(toc_->pool);
  }

  book_progress_pct_ = book_progress_pct;
  chapter_progress_pct_ = chapter_progress_pct;
  has_pending_ = false;
}

std::vector<std::string> QuickmenuScreen::wrap_(const BitmapFont& f, const std::string& text, int first_w,
                                                    int rest_w, int max_lines) {
  std::vector<std::string> out;
  if (text.empty() || !f.valid() || max_lines <= 0)
    return out;

  std::vector<std::string> words;
  {
    std::string w;
    for (size_t i = 0; i <= text.size(); ++i) {
      if (i == text.size() || text[i] == ' ') {
        if (!w.empty()) words.push_back(w);
        w.clear();
      } else {
        w += text[i];
      }
    }
  }
  if (words.empty())
    return out;

  auto width = [&f](const std::string& s) {
    return static_cast<int>(f.word_width(s.c_str(), s.size(), FontStyle::Regular));
  };

  size_t wi = 0;
  while (wi < words.size() && static_cast<int>(out.size()) < max_lines) {
    const int avail = out.empty() ? first_w : rest_w;
    std::string line = words[wi++];
    while (wi < words.size()) {
      std::string cand = line + ' ' + words[wi];
      if (width(cand) > avail) break;
      line = std::move(cand);
      ++wi;
    }
    out.push_back(std::move(line));
  }

  // Anything that did not fit is ellipsised onto the last line.
  if (wi < words.size() && !out.empty()) {
    static const char kEll[] = "...";
    const int avail = (out.size() == 1 ? first_w : rest_w) - static_cast<int>(f.word_width(kEll, 3, FontStyle::Regular));
    std::string& last = out.back();
    while (!last.empty() && width(last) > avail)
      last.erase(last.size() - 1);
    last += kEll;
  }
  return out;
}

int QuickmenuScreen::header_h_() const {
  int h = header_top_();
  h += static_cast<int>(title_lines_.size()) * title_font_().y_advance();
  if (!chapter_lines_.empty())
    h += kBlockGap + static_cast<int>(chapter_lines_.size()) * chapter_font_().y_advance();
  // The rule under the header is drawn with the shared draw_separator_(), so it
  // occupies exactly kSeparatorH like every other hairline in the tree — half
  // above the line, half below.
  //
  // kBlockGap on top of that, because kSeparatorH/2 is the whole gap between the
  // rule and the last line of the header, whereas the settings/chapters hairline
  // has a row's kRowPad on each side of it. Without this the header text sits
  // hard against its rule while every other divider in the tree breathes. Must
  // match draw_all_.
  return h + kBlockGap + kSeparatorH;
}

// Build a "Label: Value" string into a fixed buffer.
static const char* fmt_setting(char* buf, size_t bufsz, const char* label, const char* value) {
  snprintf(buf, bufsz, "%s: %s", label, value);
  return buf;
}

std::string_view QuickmenuScreen::get_item_subtitle(int index) const {
  std::string_view label = ListMenuScreen::get_item_label(index);
  const auto pos = label.find(": ");
  if (pos == std::string_view::npos) return {};
  subtitle_buf_ = label.substr(pos + 2);
  return subtitle_buf_;
}

std::string QuickmenuScreen::wintergreen_header_left() const {
  return "reading";
}

void QuickmenuScreen::start(DrawBuffer& buf, IRuntime& runtime) {
  buf_ = &buf;
  runtime_ = &runtime;
  // Capture current selection before the base class calls on_start(), which
  // rebuilds the list.
  prev_selected_ = selected_index();
  ListMenuScreen::start(buf, runtime);
}

void QuickmenuScreen::on_start() {
  // Use the list/menu rotation rather than inheriting whatever rotation the
  // reader view was using.
  set_buf_rotation_(Rotation::Deg90);

  // The header is drawn entirely by this screen's own draw_all_; the base
  // class's card header (title_/subtitle_) is not used. The author was dropped —
  // it is on the home screen and the book list, and says nothing here.
  title_ = nullptr;
  title2_ = nullptr;
  subtitle_.clear();
  subtitle2_.clear();

  // Percentages sit at the right-hand end of the title and chapter lines rather
  // than on a row of their own.
  {
    char b[16];
    snprintf(b, sizeof(b), "%d%%", book_progress_pct_);
    book_pct_buf_ = b;
    snprintf(b, sizeof(b), "%d%%", chapter_progress_pct_);
    chapter_pct_buf_ = b;
  }
  {
    // Each percentage is set in the same font as the text it sits beside, so
    // the pair reads as one line rather than as a label and an annotation.
    const BitmapFont& tfont = title_font_();
    const BitmapFont& cfont = chapter_font_();
    const int avail = (buf_ ? buf_->width() : 0) - kLM - kRM;
    const int bp_w = tfont.valid() ? tfont.word_width(book_pct_buf_.c_str(), book_pct_buf_.size(), FontStyle::Regular) : 0;
    const int cp_w = cfont.valid() ? cfont.word_width(chapter_pct_buf_.c_str(), chapter_pct_buf_.size(), FontStyle::Regular) : 0;
    title_lines_ = wrap_(tfont, book_title_, avail - bp_w - kPctGap, avail, 2);
    chapter_lines_ = wrap_(cfont, chapter_title_, avail - cp_w - kPctGap, avail, 3);
  }

  clear_items();
  idx_font_size_ = idx_reader_rotate_display_ = -1;
  first_chapter_ = -1;

  char tmp[40];

  // Item 0 *is* the header block — the book title, the chapter and their two
  // percentages. It carries no label of its own and is never drawn by the list
  // loop below; draw_all_ paints it at the top of the panel and inverts the
  // whole block when it is selected. Selecting it returns to the book, so the
  // thing you are reading is both the first thing the menu shows and the
  // default action. Its row height is 0 everywhere the list measures itself.
  add_item("");

  // Flat list, no section headers: Page Turner, Orientation, Font Size, then
  // Chapters. The clicker leads because it is the only row whose value changes
  // on its own and the only one you press twice in a session.
  //
  // The clicker row exists only when a MAC is configured — on every other build
  // clicker_state() is Unavailable and there is no row to select, rather than a
  // control that can never do anything.
  idx_clicker_ = -1;
  clicker_shown_ = runtime_ ? runtime_->clicker_state() : ClickerState::Unavailable;
  if (clicker_shown_ != ClickerState::Unavailable) {
    idx_clicker_ = count();
    // Three things: "Connecting", the clicker's battery percentage once it
    // is connected, or "Disconnected". Every failure reads as Disconnected —
    // the row is a switch, not a status console, and they all mean the same
    // thing to the user: press it again.
    //
    // The percentage is captured once on connect and held, so it never changes
    // under you mid-book. A clicker with no battery service reports nothing, so
    // that case says Connected instead.
    char val[16];
    const uint8_t pct = runtime_ ? runtime_->clicker_battery_pct() : 0;
    clicker_pct_shown_ = pct;
    if (clicker_shown_ == ClickerState::Connecting)
      snprintf(val, sizeof(val), "Connecting");
    else if (clicker_shown_ == ClickerState::Connected && pct > 0)
      snprintf(val, sizeof(val), "%u%%", static_cast<unsigned>(pct));
    else if (clicker_shown_ == ClickerState::Connected)
      snprintf(val, sizeof(val), "Connected");
    else
      snprintf(val, sizeof(val), "Disconnected");
    add_item(fmt_setting(tmp, sizeof(tmp), "Page Turner", val));
  }

  if (settings_) {
    idx_reader_rotate_display_ = count();
    add_item(fmt_setting(tmp, sizeof(tmp), "Orientation", rotation_label(app_ ? app_->rotate_reader() : 0)));

    idx_font_size_ = count();
    if (app_ && app_->font_manager() && app_->font_manager()->valid()) {
      auto* fonts = app_->font_manager()->font_set();
      int sz = 0;
      if (fonts && settings_->font_size_idx < fonts->num_fonts()) {
        auto* f = fonts->get_font(settings_->font_size_idx);
        if (f) {
          sz = f->nominal_size();
          if (sz == 0)
            sz = f->y_advance();
        }
      }
      char val[16];
      if (sz > 0)
        snprintf(val, sizeof(val), "%d", sz);
      else
        snprintf(val, sizeof(val), "Unknown");
      add_item(fmt_setting(tmp, sizeof(tmp), "Font Size", val));
    } else {
      add_item(fmt_setting(tmp, sizeof(tmp), "Font Size", ReaderSettings::kFontSizeNames[settings_->font_size_idx]));
    }
  }

  // Chapters, inline below the settings. Nested TOC entries keep their depth as
  // an indent; a book with no TOC falls back to numbered spine chapters.
  const bool has_toc = toc_ && !toc_->entries.empty();
  if (has_toc) {
    first_chapter_ = count();
    for (const auto& entry : toc_->entries)
      add_item_view(entry.label.view(toc_->pool), entry.depth);
  } else if (chapter_count_ > 1) { // chapter generation
    first_chapter_ = count();
    char cbuf[20];
    for (uint16_t i = 0; i < chapter_count_; ++i) {
      snprintf(cbuf, sizeof(cbuf), "Chapter %u", static_cast<unsigned>(i + 1));
      add_item(cbuf);
    }
  }

  // Restore selection. The item list is now fixed for a given book, so no
  // shift correction is needed.
  int sel = prev_selected_;
  int max_sel = count() - 1;
  if (max_sel >= 0) {
    if (sel < 0)
      sel = 0;
    if (sel > max_sel)
      sel = max_sel;
  }
  set_selected(sel);
}

void QuickmenuScreen::refresh_items_(int restore_selection) {
  prev_selected_ = restore_selection;
  on_start();  // on_start() calls set_selected().
}

int QuickmenuScreen::get_visible_count_(int H, int scroll_off) const {
  int available_h = H - header_h_();
  // The settings/chapters hairline takes a row-sized bite out of the list.
  if (first_chapter_ > 0)
    available_h -= kSeparatorH;
  int h = 0, cnt = 0;
  const int n = count();
  for (int i = scroll_off; i < n; ++i) {
    // The header item occupies the header, not a list row, so it costs no
    // height here — otherwise the list would think it had one row fewer than
    // it draws and the last chapter would be unreachable.
    const int rh = (i == kIdxBack) ? 0 : row_h_();
    if (h + rh > available_h) break;
    h += rh;
    cnt++;
  }
  return cnt;
}

void QuickmenuScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct) const {
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);

  if (!ui_font_.valid() || !subtitle_font_.valid()) return;

  (void)battery_pct;  // no battery here — see kHeaderTop
  const BitmapFont& tfont = title_font_();
  const BitmapFont& cfont = chapter_font_();
  int y = header_top_();

  // The header block is item kIdxBack. When it is selected the whole block —
  // title, chapter and both percentages — is inverted, exactly as a list row is,
  // and Confirm returns to the book. The fill runs from the top of the panel to
  // the rule, so the block reads as one target rather than as text that happens
  // to be highlighted.
  const bool head_sel = (selected() == kIdxBack);
  if (head_sel)
    // Up to the rule, which draw_separator_ places at the midpoint of the
    // kSeparatorH gap the header block ends with.
    buf.fill_rect(0, 0, W, header_h_() - kSeparatorH / 2, false);

  // ── Book title, with the book percentage at the right of its first line ──
  for (size_t i = 0; i < title_lines_.size(); ++i) {
    const std::string& line = title_lines_[i];
    buf.draw_text_proportional(kLM, y + tfont.baseline(), line.c_str(), line.size(), tfont, head_sel);
    if (i == 0) {
      const int pw = tfont.word_width(book_pct_buf_.c_str(), book_pct_buf_.size(), FontStyle::Regular);
      buf.draw_text_proportional(W - kRM - pw, y + tfont.baseline(), book_pct_buf_.c_str(), book_pct_buf_.size(), tfont,
                                 head_sel);
    }
    y += tfont.y_advance();
  }

  // ── Chapter, word-wrapped, with the chapter percentage on its first line ──
  if (!chapter_lines_.empty()) {
    y += kBlockGap;
    for (size_t i = 0; i < chapter_lines_.size(); ++i) {
      const std::string& line = chapter_lines_[i];
      buf.draw_text_proportional(kLM, y + cfont.baseline(), line.c_str(), line.size(), cfont, head_sel);
      if (i == 0) {
        const int pw = cfont.word_width(chapter_pct_buf_.c_str(), chapter_pct_buf_.size(), FontStyle::Regular);
        buf.draw_text_proportional(W - kRM - pw, y + cfont.baseline(), chapter_pct_buf_.c_str(),
                                   chapter_pct_buf_.size(), cfont, head_sel);
      }
      y += cfont.y_advance();
    }
  }

  // Same helper, same geometry as every other hairline — see header_h_().
  y += kBlockGap;
  draw_separator_(buf, W, y);
  y += kSeparatorH;  // must match header_h_()

  // ── Item list ────────────────────────────────────────────────────────────
  const int n = count();
  // Compute total content height to decide whether scrolling is needed. The
  // header item is not one of these rows.
  const int total_h = (n - 1) * row_h_();
  const int so = (total_h <= H - y) ? 0 : scroll_offset();

  for (int i = (so > kIdxBack ? so : kIdxBack + 1); i < n && y < H; ++i) {
    const bool sel = (i == selected());

    const std::string_view label = get_item_label(i);
    const bool is_chapter = (first_chapter_ >= 0 && i >= first_chapter_);

    // Hairline between the settings and the chapters. It sits in its own gap
    // rather than inside a row, so a selected first chapter cannot paint over it.
    if (i == first_chapter_ && i > so) {
      draw_separator_(buf, W, y);
      y += kSeparatorH;
    }

    // Only a settings row is "Label: Value". Chapter titles legitimately contain
    // ": " ("Chapter 1: The Beginning") and must never be split into columns.
    std::string_view display_label = label;
    std::string_view display_value;
    if (!is_chapter) {
      const auto pos = label.find(": ");
      if (pos != std::string_view::npos) {
        display_label = label.substr(0, pos);
        display_value = label.substr(pos + 2);
      }
    }

    if (sel)
      buf.fill_rect(0, y, W, row_h_(), false);

    const int text_y = y + (row_h_() - ui_font_.y_advance()) / 2 + ui_font_.baseline();
    const int text_x = kLM + (is_chapter ? get_item_indent(i) * 12 : 0);
    buf.draw_text_proportional(text_x, text_y, display_label.data(), display_label.size(), ui_font_, sel);

    if (!display_value.empty()) {
      const int vw = ui_font_.word_width(display_value.data(), display_value.size(), FontStyle::Regular);
      buf.draw_text_proportional(W - kRM - vw, text_y, display_value.data(), display_value.size(), ui_font_, sel);
    }

    y += row_h_();
  }
}

void QuickmenuScreen::on_select(int index) {
  // The header block: back to the book, with no chapter jump pending.
  if (index == kIdxBack) {
    app_->pop_screen();
    return;
  }

  if (first_chapter_ >= 0 && index >= first_chapter_) {
    const int ch = index - first_chapter_;
    if (toc_ && !toc_->entries.empty()) {
      pending_chapter_ = toc_->entries[ch].file_idx;
      pending_para_index_ = toc_->entries[ch].para_index;
    } else {
      pending_chapter_ = static_cast<uint16_t>(ch);
      pending_para_index_ = 0;
    }
    has_pending_ = true;
    app_->pop_screen();
    return;
  }

  if (!settings_)
    return;

  if (index == idx_font_size_) {
    uint8_t max_idx = ReaderSettings::kNumFontSizePresets;
    if (app_ && app_->font_manager() && app_->font_manager()->valid()) {
      auto* fonts = app_->font_manager()->font_set();
      if (fonts && fonts->num_fonts() > 0)
        max_idx = static_cast<uint8_t>(fonts->num_fonts());
    }
    settings_->font_size_idx = static_cast<uint8_t>((settings_->font_size_idx + 1) % max_idx);
    refresh_items_(index);
    return;
  }
  if (index == idx_reader_rotate_display_) {
    if (app_) {
      uint8_t v = static_cast<uint8_t>(app_->rotate_reader() == 1 ? 0 : 1);
      app_->set_rotate_reader(v);
      refresh_items_(index);
    }
    return;
  }
  if (index == idx_clicker_) {
    if (runtime_) {
      // Free the resident book index first, synchronously. The BLE stack starts
      // allocating the moment toggle_clicker() spawns its worker, and on this
      // device the two do not fit at once — a connect used to fail with the
      // host task unable to start (11 KB free), or succeed and then abort the
      // reader on its next page layout.
      if (runtime_->clicker_state() != ClickerState::Connected && app_)
        app_->release_ram_for_radio();
      // Returns immediately: connecting takes seconds. The row redraws as
      // "Connecting", and update() below repaints it when the outcome lands.
      runtime_->toggle_clicker();
      refresh_items_(index);
    }
    return;
  }
  return;
}

}  // namespace wintergreen
