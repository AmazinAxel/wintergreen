#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../content/wgb/WgbReader.h"
#include "ListMenuScreen.h"

namespace wintergreen {

// ---------------------------------------------------------------------------
// ReaderSettings — user-adjustable reader preferences
// Stored in ReaderScreen; mutated inline by QuickmenuScreen.
// ---------------------------------------------------------------------------
// Only two things about the page are adjustable. Margins, alignment, line
// spacing and publisher font sizes are fixed: margins at what used to be the
// "Normal" preset, and the other three deferring to the book's own CSS.
struct ReaderSettings {
  uint8_t font_size_idx = 0;  // base font size preset index (0 = 32px)

  // Must match the number of sizes tools/make_font.py builds into the bundle;
  // every book carries one baked page table per size and orientation.
  static constexpr uint8_t kNumFontSizePresets = kWgbFontSizes;
};

// In-reader options menu — shown when the user presses Button1 while reading.
// Populated by ReaderScreen before being pushed so it reflects the current
// reading context. Below the settings rows, the book's chapters as a two-column
// grid of numbered boxes (chapter start i is chapter i + 1; there are no names).
//
// Usage:
//   app_->quickmenu()->set_settings(&reader_settings_);
//   app_->quickmenu()->populate(std::move(chapter_pages), page, title, book_pct, chapter_pct);
//   app_->push_screen(ScreenId::Quickmenu);
class QuickmenuScreen final : public ListMenuScreen {
 public:
  QuickmenuScreen() = default;

  // Set the settings object to read/write (must outlive this screen).
  void set_settings(ReaderSettings* s) {
    settings_ = s;
  }

  // Populate before pushing. The chapter grid is appended below the settings.
  // `chapter_pages`: the page each chapter starts on, in the current layout.
  void populate(std::vector<uint32_t> chapter_pages, uint32_t current_page, const std::string& title,
                int book_progress_pct, int chapter_progress_pct);

  void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const override;
  int centred_offset_(int H, int& shift) const override;
  void draw_header_(DrawBuffer& buf, int W) const;

  // Row height follows the font rather than being a constant: the list font is
  // the 32 px header face, and the old fixed 28 px was shorter than a line of it.
  static constexpr int kRowPad = 12;
  int row_h_() const { return ui_font_.y_advance() + kRowPad; }
  static constexpr int kLM = 14, kRM = 14;
  // Minimum gap between an ellipsised label and its right-hand value.
  static constexpr int kValueGap = 12;
  static constexpr int kPctGap = 10;    // title text → its percentage
  static constexpr int kBlockGap = 4;   // between title, chapter and the rule
  // The settings/chapters hairline uses the shared ListMenuScreen geometry
  // (kSeparatorH / draw_separator_) so it matches the book list exactly.
  // No battery on this screen — it is an overlay on the book, not a top-level
  // screen, and the reader it sits over shows nothing either.
  static constexpr int kHeaderTop = 16;
  int header_top_() const { return kHeaderTop; }

  // Chapter grid: two columns of boxed numbers, kCellGap from the sides. Every cell has a
  // 1 px box; the selected one is filled and its number inverted, exactly as a
  // selected list row. The gap between cells (both ways) is half the side
  // margin, the hairline's clearance; the padding above and below the
  // number's ink is kLM.
  // Numbers are digit_font_ (40 px Iosevka Slab, digits only); the number is
  // centred on its ink (glyph_height), not on the line box.
  static constexpr int kGridCols = 2;
  static constexpr int kCellGap = kLM / 2;
  static constexpr int kCellPadV = kLM;
  int cell_h_() const { return digit_font_.glyph_height() + 2 * kCellPadV; }
  int cell_pitch_() const { return cell_h_() + kCellGap; }
  BitmapFont digit_font_;

  // The chapter is a subtitle to the book title: one step down in size, in the
  // same font the settings rows use.
  const BitmapFont& chapter_font_() const { return section_font_; }
  // A title too long for one line of ui_font_ is set in two lines of the
  // chapter face instead (title_small_, decided in on_start()).
  const BitmapFont& title_font_() const { return title_small_ ? section_font_ : ui_font_; }
  bool title_small_ = false;

  // ── Chapter jump ──────────────────────────────────────────────────────────
  // The chapter list lives in this screen; there is no separate Chapters page.
  // ReaderScreen::resume() consumes these after the menu pops.
  bool has_pending() const {
    return has_pending_;
  }
  // The page the chosen chapter starts on.
  uint32_t pending_page() const {
    return pending_page_;
  }
  void clear_pending() {
    has_pending_ = false;
  }

  // Where the next opening puts the cursor: the chapter being read (the front
  // select key) or the top setting (a power-button tap). Set before the push.
  void set_open_on_chapter(bool v) {
    open_on_chapter_ = v;
  }

  void stop() override {
    std::vector<uint32_t>().swap(chapter_pages_);
  }

  void pause() override {}

  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override {
    buf_ = &buf;
    runtime_ = &runtime;
    // The clicker connects asynchronously — several seconds, on its own task —
    // so the row that says "Connecting" has to be repainted when the outcome
    // arrives. This is the only thing on any screen that changes without a
    // button press, hence the explicit poll; everything else repaints on input.
    // The battery percentage arrives on its own notification a moment *after*
    // the connect completes, so watching the state alone would leave the row
    // reading "Connected" until the next keypress.
    if (idx_clicker_ >= 0) {
      const ClickerState now = runtime.clicker_state();
      const uint8_t pct = runtime.clicker_battery_pct();
      // A connect that succeeds while this menu is up returns to the book:
      // the next thing anyone does after pairing a page-turner is read.
      if (clicker_shown_ == ClickerState::Connecting && now == ClickerState::Connected) {
        on_back();
        return;
      }
      if (now != clicker_shown_ || pct != clicker_pct_shown_) {
        refresh_items_(selected());
        request_redraw();
      }
    }
    ListMenuScreen::update(buttons, buf, runtime);
  }

  void start(DrawBuffer& buf, IRuntime& runtime) override;

 protected:
  void on_start() override;
  void on_select(int index) override;

 private:
  ReaderSettings* settings_ = nullptr;
  DrawBuffer* buf_ = nullptr;
  IRuntime* runtime_ = nullptr;

  // What the Clicker row currently says, so update() can tell an asynchronous
  // change from a redraw it has already done.
  ClickerState clicker_shown_ = ClickerState::Unavailable;
  uint8_t clicker_pct_shown_ = 0;


  // Item 0 is the header block (book title + chapter + percentages), drawn by
  // draw_all_ rather than by the list loop and worth 0 rows to every height
  // calculation. Selecting it returns to the book.
  static constexpr int kIdxBack = 0;

  // Item indices (-1 = not shown).
  int idx_font_size_ = -1;
  int idx_reader_rotate_display_ = -1;
  int idx_clicker_ = -1;
  int first_chapter_ = -1;
  // List index of the chapter holding the reading position, or -1. Derived in
  // on_start() from current_chapter_offset_, which populate() sets: the last
  // chapter starting on or before the current page.
  int current_chapter_item_ = -1;
  int current_chapter_offset_ = -1;
  std::vector<uint32_t> chapter_pages_;
  bool open_on_chapter_ = false;

  uint32_t pending_page_ = 0;
  bool has_pending_ = false;

  void refresh_items_(int restore_selection);

  int prev_selected_ = 0;

  // Spaces collapsed to one and trimmed (populate), so every wrapped line is a
  // contiguous span of it.
  std::string book_title_;
  char chapter_title_[32] = {};
  // A wrapped line: bytes [off, off + len) of its text, then "..." when the
  // rest did not fit.
  struct Line {
    uint16_t off = 0, len = 0;
    bool ell = false;
  };
  // Wrapped at on_start() so draw_all_ and header_h_ measure the same block.
  // Spans, not strings: opening the menu allocates nothing.
  Line title_lines_[2];
  int title_n_ = 0;
  Line chapter_lines_[1];
  int chapter_n_ = 0;
  std::string book_pct_buf_;
  std::string chapter_pct_buf_;

  // Word-wraps `text` (single spaces, no leading/trailing) into at most
  // `max_lines` lines of `f`, ellipsising the last one if it still overflows.
  // `first_w` is the width available to the first line, which is narrower when
  // a percentage sits beside it. Returns the line count.
  static int wrap_(const BitmapFont& f, std::string_view text, int first_w, int rest_w, int max_lines, Line* out);
  // Draw one wrapped line; returns nothing, the caller advances y.
  static void draw_line_(DrawBuffer& buf, int x, int baseline, std::string_view text, const Line& l,
                         const BitmapFont& f, bool invert);

  // Pixels from the top of the panel to the rule above the item list.
  int header_h_() const;
  // Height of the title + chapter lines alone, centred inside header_h_().
  int header_content_h_() const;

  int book_progress_pct_ = 0;
  int chapter_progress_pct_ = 0;

  // Grid row (0-based) of the chapter grid that a draw starting at list index
  // `scroll_off` begins with: the row holding that cell, so a row is never split.
  int first_grid_row_(int scroll_off) const {
    return scroll_off > first_chapter_ ? (scroll_off - first_chapter_) / kGridCols : 0;
  }
};
}  // namespace wintergreen
