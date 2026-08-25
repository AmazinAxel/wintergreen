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

  // Must match the sizes tools/make_font.py builds into the bundle, in order.
  static constexpr const char* kFontSizeNames[] = {"32", "36"};
  static constexpr uint8_t kNumFontSizePresets = 2;

  static constexpr uint16_t h_padding() {
    return 12;
  }
  static constexpr uint16_t v_padding() {
    return 4;
  }
  // Breathing room above the panel edge. Nothing is drawn in it.
  static constexpr uint16_t bottom_margin() {
    return 6;
  }
};

// In-reader options menu — shown when the user presses Button1 while reading.
// Populated by ReaderScreen before being pushed so it reflects the current
// reading context (TOC availability).
//
// Usage:
//   app_->quickmenu()->set_settings(&reader_settings_);
//   app_->quickmenu()->populate(wgb_.toc(), chapter_idx, page_pos_.paragraph);
//   app_->push_screen(ScreenId::Quickmenu);
class QuickmenuScreen final : public ListMenuScreen {
 public:
  QuickmenuScreen() = default;

  const char* name() const override {
    return "Options";
  }

  // Set the settings object to read/write (must outlive this screen).
  void set_settings(ReaderSettings* s) {
    settings_ = s;
  }

  // Populate before pushing. Pass toc (may be empty) and chapter_count from WGB.
  // The chapter list is appended below the settings when toc has entries OR
  // chapter_count > 1 (numbered fallback).
  void populate(const TableOfContents& toc, uint16_t current_chapter, uint16_t current_para,
                const std::string& fallback_title, int book_progress_pct, int chapter_progress_pct,
                uint16_t chapter_count = 0);

  void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const override;
  int get_visible_count_(int H, int scroll_off) const override;

  // Row height follows the font rather than being a constant: the list font is
  // the 32 px header face, and the old fixed 28 px was shorter than a line of it.
  static constexpr int kRowPad = 12;
  int row_h_() const { return ui_font_.y_advance() + kRowPad; }
  static constexpr int kLM = 14, kRM = 14;
  static constexpr int kPctGap = 10;    // title text → its percentage
  static constexpr int kBlockGap = 10;  // between title, chapter and the rule
  // The settings/chapters hairline uses the shared ListMenuScreen geometry
  // (kSeparatorH / draw_separator_) so it matches the book list exactly.
  // No battery on this screen — it is an overlay on the book, not a top-level
  // screen, and the reader it sits over shows nothing either.
  static constexpr int kHeaderTop = 16;
  int header_top_() const { return kHeaderTop; }

  // The chapter is a subtitle to the book title: one step down in size, in the
  // same font the settings rows use.
  const BitmapFont& chapter_font_() const { return section_font_.valid() ? section_font_ : subtitle_font_; }
  const BitmapFont& title_font_() const { return header_font_.valid() ? header_font_ : ui_font_; }

  // ── Chapter jump ──────────────────────────────────────────────────────────
  // The chapter list lives in this screen; there is no separate Chapters page.
  // ReaderScreen::resume() consumes these after the menu pops.
  bool has_pending() const {
    return has_pending_;
  }
  uint16_t pending_chapter() const {
    return pending_chapter_;
  }
  uint16_t pending_para_index() const {
    return pending_para_index_;
  }
  void clear_pending() {
    has_pending_ = false;
  }

  void stop() override {
    toc_ = nullptr;
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
      if (now != clicker_shown_ || pct != clicker_pct_shown_) {
        refresh_items_(selected_index());
        request_redraw();
      }
    }
    ListMenuScreen::update(buttons, buf, runtime);
  }

  void start(DrawBuffer& buf, IRuntime& runtime) override;

  std::string_view get_item_subtitle(int index) const override;
  std::string wintergreen_header_left() const override;

 protected:
  void on_start() override;
  void on_select(int index) override;

 private:
  mutable std::string subtitle_buf_;
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

  uint16_t pending_chapter_ = 0;
  uint16_t pending_para_index_ = 0;
  bool has_pending_ = false;

  void refresh_items_(int restore_selection);

  int prev_selected_ = 0;

  std::string book_title_;
  std::string chapter_title_;
  // Wrapped at on_start() so draw_all_ and header_h_ measure the same block.
  std::vector<std::string> title_lines_;
  std::vector<std::string> chapter_lines_;
  std::string book_pct_buf_;
  std::string chapter_pct_buf_;

  // Word-wraps `text` into at most `max_lines` lines of `f`, ellipsising the
  // last one if it still overflows. `first_w` is the width available to the
  // first line, which is narrower when a percentage sits beside it.
  static std::vector<std::string> wrap_(const BitmapFont& f, const std::string& text, int first_w, int rest_w,
                                        int max_lines);

  // Pixels from the top of the panel to the rule above the item list.
  int header_h_() const;

  int book_progress_pct_ = 0;
  int chapter_progress_pct_ = 0;
  uint16_t chapter_count_ = 0;

  const TableOfContents* toc_ = nullptr;
};
}  // namespace wintergreen
