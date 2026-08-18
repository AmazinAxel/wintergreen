#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../content/mrb/MrbReader.h"
#include "ListMenuScreen.h"

namespace wintergreen {

// ---------------------------------------------------------------------------
// ReaderSettings — user-adjustable reader preferences
// Stored in ReaderScreen; mutated inline by ReaderOptionsScreen.
// ---------------------------------------------------------------------------
// Only two things about the page are adjustable. Margins, alignment, line
// spacing and publisher font sizes are fixed: margins at what used to be the
// "Normal" preset, and the other three deferring to the book's own CSS.
struct ReaderSettings {
  uint8_t font_size_idx = 1;  // base font size preset index (1 = Normal/24px)

  static constexpr const char* kFontSizeNames[] = {"20", "24", "26", "28", "30", "32", "34", "36"};
  static constexpr uint8_t kNumFontSizePresets = 8;

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
//   app_->reader_options()->set_settings(&reader_settings_);
//   app_->reader_options()->populate(mrb_.toc(), chapter_idx, page_pos_.paragraph);
//   app_->push_screen(ScreenId::ReaderOptions);
class ReaderOptionsScreen final : public ListMenuScreen {
 public:
  ReaderOptionsScreen() = default;

  const char* name() const override {
    return "Options";
  }

  // Set the settings object to read/write (must outlive this screen).
  void set_settings(ReaderSettings* s) {
    settings_ = s;
  }

  // Populate before pushing. Pass toc (may be empty) and chapter_count from MRB.
  // The chapter list is appended below the settings when toc has entries OR
  // chapter_count > 1 (numbered fallback).
  void populate(const TableOfContents& toc, uint16_t current_chapter, uint16_t current_para,
                const std::string& fallback_title, int book_progress_pct, int chapter_progress_pct,
                uint16_t chapter_count = 0);

  void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const override;
  int get_visible_count_(int H, int scroll_off) const override;

  static constexpr int kRowH = 28;

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

  // Item indices (-1 = not shown).
  int idx_font_size_ = -1;
  int idx_rotate_display_ = -1;
  int idx_reader_rotate_display_ = -1;
  int first_chapter_ = -1;

  uint16_t pending_chapter_ = 0;
  uint16_t pending_para_index_ = 0;
  bool has_pending_ = false;

  void refresh_items_(int restore_selection);

  int prev_selected_ = 0;

  std::string book_title_;
  std::string book_title1_buf_;
  std::string book_title2_buf_;
  std::string chapter_title_;

  int book_progress_pct_ = 0;
  int chapter_progress_pct_ = 0;
  uint16_t chapter_count_ = 0;

  const TableOfContents* toc_ = nullptr;
};
}  // namespace wintergreen
