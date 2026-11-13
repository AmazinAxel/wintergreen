#include "QuickmenuScreen.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

#include "../Application.h"
#include "../content/BookIndex.h"

namespace wintergreen {

// Left-aligned text, ellipsised when it does not fit in max_w.
//
// Truncation advances by whole UTF-8 sequences: cutting mid-sequence would feed
// draw_text_proportional a partial codepoint.
static void draw_text_ellipsised(DrawBuffer& buf, int x, int baseline, const char* text, size_t len,
                                 const BitmapFont& f, int max_w, bool invert) {
  if (len == 0 || max_w <= 0) return;
  char trunc[260];
  const std::string_view t = ellipsize(f, text, len, max_w, trunc);
  buf.draw_text_proportional(x, baseline, t.data(), t.size(), f, invert);
}

// ---------------------------------------------------------------------------

void QuickmenuScreen::populate(std::vector<uint32_t> chapter_pages, uint32_t current_page, const std::string& title,
                               int book_progress_pct, int chapter_progress_pct) {
  chapter_pages_ = std::move(chapter_pages);
  // Collapse runs of spaces and trim, in place: the wrap's lines are then
  // contiguous spans, exactly the word + ' ' + word strings it used to build.
  book_title_ = title;
  size_t n = 0;
  for (char c : book_title_)
    if (c != ' ' || (n > 0 && book_title_[n - 1] != ' '))
      book_title_[n++] = c;
  if (n > 0 && book_title_[n - 1] == ' ')
    --n;
  book_title_.resize(n);
  int best_match = -1;
  for (size_t i = 0; i < chapter_pages_.size(); ++i)
    if (chapter_pages_[i] <= current_page)
      best_match = static_cast<int>(i);
  // Before the first chapter's page the nearest chapter is the first.
  current_chapter_offset_ = std::max(best_match, 0);
  chapter_title_[0] = '\0';
  if (!chapter_pages_.empty())
    snprintf(chapter_title_, sizeof(chapter_title_), "Chapter %d of %u", current_chapter_offset_ + 1,
             static_cast<unsigned>(chapter_pages_.size()));

  book_progress_pct_ = book_progress_pct;
  chapter_progress_pct_ = chapter_progress_pct;
  has_pending_ = false;
}

int QuickmenuScreen::wrap_(const BitmapFont& f, std::string_view text, int first_w, int rest_w, int max_lines,
                           Line* out) {
  if (text.empty() || !f.valid() || max_lines <= 0 || text.size() > 0xFFFF)
    return 0;
  auto width = [&](size_t a, size_t b) {
    return static_cast<int>(f.word_width(text.data() + a, b - a, FontStyle::Regular));
  };
  auto word_end = [&](size_t i) {
    while (i < text.size() && text[i] != ' ')
      ++i;
    return i;
  };

  int n = 0;
  size_t pos = 0;  // start of the next word
  while (pos < text.size() && n < max_lines) {
    const int avail = n == 0 ? first_w : rest_w;
    size_t end = word_end(pos);
    while (end < text.size()) {
      const size_t cand = word_end(end + 1);
      if (width(pos, cand) > avail)
        break;
      end = cand;
    }
    out[n++] = Line{static_cast<uint16_t>(pos), static_cast<uint16_t>(end - pos), false};
    pos = end < text.size() ? end + 1 : end;
  }

  // Anything that did not fit is ellipsised onto the last line.
  if (pos < text.size() && n > 0) {
    static const char kEll[] = "...";
    const int avail = (n == 1 ? first_w : rest_w) - static_cast<int>(f.word_width(kEll, 3, FontStyle::Regular));
    Line& last = out[n - 1];
    while (last.len > 0 && width(last.off, last.off + last.len) > avail)
      --last.len;
    last.ell = true;
  }
  return n;
}

// The UI fonts carry no kerning, so "..." drawn at the line's end cursor is
// pixel-identical to drawing line + "..." as one string.
void QuickmenuScreen::draw_line_(DrawBuffer& buf, int x, int baseline, std::string_view text, const Line& l,
                                 const BitmapFont& f, bool invert) {
  x = buf.draw_text_proportional(x, baseline, text.data() + l.off, l.len, f, invert);
  if (l.ell)
    buf.draw_text_proportional(x, baseline, "...", 3, f, invert);
}

int QuickmenuScreen::header_content_h_() const {
  int h = title_n_ * title_font_().y_advance();
  if (chapter_n_ > 0)
    h += kBlockGap + chapter_n_ * chapter_font_().y_advance();
  return h;
}

int QuickmenuScreen::header_h_() const {
  // Fixed height: one full-size title line plus one chapter line, kHeaderTop
  // above and below (padding inside the highlight box, not around it). A title
  // needing two lines drops to the chapter face instead of growing the block;
  // draw_all_ centres the content, eating into that padding.
  const int h = 2 * kHeaderTop + ui_font_.y_advance() + kBlockGap + section_font_.y_advance();
  // The rule under the header is drawn with the shared draw_separator_(), so it
  // occupies exactly kSeparatorH like every other hairline in the tree — half
  // above the line, half below — and content butts against both ends of that
  // gap. Don't pad it further here or this divider reads twice as loose as the
  // rest. Must match draw_all_.
  return h + kSeparatorH;
}

// Build a "Label: Value" string into a fixed buffer.
static const char* fmt_setting(char* buf, size_t bufsz, const char* label, const char* value) {
  snprintf(buf, bufsz, "%s: %s", label, value);
  return buf;
}

void QuickmenuScreen::start(DrawBuffer& buf, IRuntime& runtime) {
  buf_ = &buf;
  runtime_ = &runtime;
  // Every opening starts afresh rather than where the cursor was last time: on
  // the top setting (the Page Turner when a clicker is configured) for a power
  // tap, on the current chapter for the front select key — on_start() applies
  // that once the list exists. refresh_items_() bypasses start(), so an in-place
  // rebuild keeps the cursor.
  prev_selected_ = kIdxBack + 1;
  ListMenuScreen::start(buf, runtime);
}

void QuickmenuScreen::on_start() {
  // Use the list/menu rotation rather than inheriting whatever rotation the
  // reader view was using.
  set_buf_rotation_(Rotation::Deg90);

  // The header is drawn entirely by this screen's own draw_all_. The author was
  // dropped — it is on the home screen and the book list, and says nothing here.

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
    title_small_ = false;
    title_n_ = wrap_(tfont, book_title_, avail - bp_w - kPctGap, avail, 2, title_lines_);
    if (title_n_ > 1) {
      // Too long for one full-size line: two lines of the smaller face, so the
      // header keeps its fixed height (see header_h_()).
      title_small_ = true;
      const BitmapFont& sfont = title_font_();
      const int sp_w = sfont.valid() ? sfont.word_width(book_pct_buf_.c_str(), book_pct_buf_.size(), FontStyle::Regular) : 0;
      title_n_ = wrap_(sfont, book_title_, avail - sp_w - kPctGap, avail, 2, title_lines_);
    }
    chapter_n_ = wrap_(cfont, chapter_title_, avail - cp_w - kPctGap, avail, 1, chapter_lines_);
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

    // The reader font set is always present here: without it no book opens, and
    // this menu only exists over an open book.
    idx_font_size_ = count();
    const BitmapFont* f = app_ ? app_->reader_font()->get_font(settings_->font_size_idx) : nullptr;
    char val[16] = "";
    if (f)
      snprintf(val, sizeof(val), "%d", f->nominal_size() ? f->nominal_size() : f->y_advance());
    add_item(fmt_setting(tmp, sizeof(tmp), "Font Size", val));
  }

  // Chapters, inline below the settings: one numbered cell per chapter start.
  // List order is grid order (left to right, then down), so the inherited
  // up/down navigation steps right and wraps to the next row unchanged.
  const int nch = static_cast<int>(chapter_pages_.size());
  if (nch > 0) {
    first_chapter_ = count();
    for (int i = 0; i < nch; ++i) {
      snprintf(tmp, sizeof(tmp), "%d", i + 1);
      add_item(tmp);
    }
  }
  current_chapter_item_ = -1;
  if (first_chapter_ >= 0 && current_chapter_offset_ >= 0 && first_chapter_ + current_chapter_offset_ < count())
    current_chapter_item_ = first_chapter_ + current_chapter_offset_;

  if (open_on_chapter_ && current_chapter_item_ >= 0)
    prev_selected_ = current_chapter_item_;
  open_on_chapter_ = false;
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
  const int settings_end = first_chapter_ >= 0 ? first_chapter_ : n;
  for (int i = scroll_off; i < settings_end; ++i) {
    // The header item occupies the header, not a list row, so it costs no
    // height here — otherwise the list would think it had one row fewer than
    // it draws and the last chapter would be unreachable.
    const int rh = (i == kIdxBack) ? 0 : row_h_();
    if (h + rh > available_h) return cnt;
    h += rh;
    cnt++;
  }
  if (first_chapter_ < 0)
    return cnt;
  // Grid: whole rows from the one holding scroll_off (draw_all_ starts there
  // too). Cells of that row left of scroll_off are drawn but not counted.
  const int rows = (n - first_chapter_ + kGridCols - 1) / kGridCols;
  for (int r = first_grid_row_(scroll_off); r < rows; ++r) {
    if (h + cell_h_() > available_h) break;
    h += cell_pitch_();
    const int row_first = first_chapter_ + r * kGridCols;
    const int row_end = std::min(row_first + kGridCols, n);
    cnt += row_end - std::max(row_first, scroll_off);
  }
  return cnt;
}

void QuickmenuScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct) const {
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);

  if (!ui_font_.valid()) return;

  (void)battery_pct;  // no battery here — see kHeaderTop
  const BitmapFont& tfont = title_font_();
  const BitmapFont& cfont = chapter_font_();
  int y = (header_h_() - kSeparatorH - header_content_h_()) / 2;

  // The header block is item kIdxBack. When it is selected the whole block —
  // title, chapter and both percentages — is inverted, exactly as a list row is,
  // and Confirm returns to the book. The fill runs from the top of the panel to
  // the *start* of the separator gap, so the block reads as one target rather
  // than as text that happens to be highlighted.
  const bool head_sel = (selected() == kIdxBack);
  if (head_sel)
    // Stops where the kSeparatorH gap begins. Running it to the rule instead
    // (header_h_() - kSeparatorH / 2) swallows the gap above the hairline, and
    // the separator loses its clearance on one side.
    buf.fill_rect(0, 0, W, header_h_() - kSeparatorH, false);

  // ── Book title, with the book percentage at the right of its first line ──
  for (int i = 0; i < title_n_; ++i) {
    draw_line_(buf, kLM, y + tfont.baseline(), book_title_, title_lines_[i], tfont, head_sel);
    if (i == 0) {
      const int pw = tfont.word_width(book_pct_buf_.c_str(), book_pct_buf_.size(), FontStyle::Regular);
      buf.draw_text_proportional(W - kRM - pw, y + tfont.baseline(), book_pct_buf_.c_str(), book_pct_buf_.size(), tfont,
                                 head_sel);
    }
    y += tfont.y_advance();
  }

  // ── Chapter, word-wrapped, with the chapter percentage on its first line ──
  if (chapter_n_ > 0) {
    y += kBlockGap;
    for (int i = 0; i < chapter_n_; ++i) {
      draw_line_(buf, kLM, y + cfont.baseline(), chapter_title_, chapter_lines_[i], cfont, head_sel);
      if (i == 0) {
        const int pw = cfont.word_width(chapter_pct_buf_.c_str(), chapter_pct_buf_.size(), FontStyle::Regular);
        buf.draw_text_proportional(W - kRM - pw, y + cfont.baseline(), chapter_pct_buf_.c_str(),
                                   chapter_pct_buf_.size(), cfont, head_sel);
      }
      y += cfont.y_advance();
    }
  }

  // Same helper, same geometry as every other hairline — see header_h_().
  y = header_h_() - kSeparatorH;
  draw_separator_(buf, W, y);
  y += kSeparatorH;  // must match header_h_()

  // ── Item list ────────────────────────────────────────────────────────────
  const int n = count();
  const int settings_end = first_chapter_ >= 0 ? first_chapter_ : n;
  const int grid_rows = first_chapter_ >= 0 ? (n - first_chapter_ + kGridCols - 1) / kGridCols : 0;
  // Compute total content height to decide whether scrolling is needed. The
  // header item is not one of these rows.
  int total_h = (settings_end - 1) * row_h_();
  if (grid_rows > 0)
    total_h += (first_chapter_ > 0 ? kSeparatorH : 0) + grid_rows * cell_pitch_() - kCellGap;
  const int so = (total_h <= H - y) ? 0 : scroll_offset();

  // Settings rows: "Label: Value", inverted when selected.
  for (int i = (so > kIdxBack ? so : kIdxBack + 1); i < settings_end && y < H; ++i) {
    const bool sel = (i == selected());
    const std::string_view label = get_item_label(i);
    std::string_view display_label = label;
    std::string_view display_value;
    const auto pos = label.find(": ");
    if (pos != std::string_view::npos) {
      display_label = label.substr(0, pos);
      display_value = label.substr(pos + 2);
    }

    if (sel)
      buf.fill_rect(0, y, W, row_h_(), false);

    const int text_y = y + (row_h_() - ui_font_.y_advance()) / 2 + ui_font_.baseline();
    const int text_x = kLM;

    // The value keeps its full width and the label gives way: a settings row
    // reads "Font Size: 28", and losing the 28 would defeat the row.
    int label_w = W - kRM - text_x;
    if (!display_value.empty()) {
      const int vw = ui_font_.word_width(display_value.data(), display_value.size(), FontStyle::Regular);
      buf.draw_text_proportional(W - kRM - vw, text_y, display_value.data(), display_value.size(), ui_font_, sel);
      label_w -= vw + kValueGap;
    }

    draw_text_ellipsised(buf, text_x, text_y, display_label.data(), display_label.size(), ui_font_, label_w, sel);

    y += row_h_();
  }

  if (grid_rows == 0)
    return;

  // Hairline between the settings and the chapters, in its own gap.
  if (so < first_chapter_) {
    draw_separator_(buf, W, y);
    y += kSeparatorH;
  }

  // ── Chapter grid ─────────────────────────────────────────────────────────
  const int cell_w = (W - kLM - kRM - (kGridCols - 1) * kCellGap) / kGridCols;
  for (int r = first_grid_row_(so); r < grid_rows && y + cell_h_() <= H; ++r) {
    for (int c = 0; c < kGridCols; ++c) {
      const int i = first_chapter_ + r * kGridCols + c;
      if (i >= n)
        break;
      const int x = kLM + c * (cell_w + kCellGap);
      draw_outline(buf, x, y, cell_w, cell_h_(), i == selected() ? kCellSelW : 1);
      const std::string_view label = get_item_label(i);
      const int tw = ui_font_.word_width(label.data(), label.size(), FontStyle::Regular);
      const int text_y = y + kCellPadV + ui_font_.baseline();
      buf.draw_text_proportional(x + (cell_w - tw) / 2, text_y, label.data(), label.size(), ui_font_, false);
    }
    y += cell_pitch_();
  }
}

void QuickmenuScreen::on_select(int index) {
  // The header block: back to the book, with no chapter jump pending.
  if (index == kIdxBack) {
    app_->pop_screen();
    return;
  }

  if (first_chapter_ >= 0 && index >= first_chapter_) {
    pending_page_ = chapter_pages_[index - first_chapter_];
    has_pending_ = true;
    app_->pop_screen();
    return;
  }

  if (!settings_)
    return;

  if (index == idx_font_size_) {
    uint8_t max_idx = ReaderSettings::kNumFontSizePresets;
    const int n = app_ ? app_->reader_font()->num_fonts() : 0;
    if (n > 0 && n < max_idx)
      max_idx = static_cast<uint8_t>(n);
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
