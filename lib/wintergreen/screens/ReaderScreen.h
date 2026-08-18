#pragma once

#include <functional>
#include <memory>

#include "../Input.h"
#include "../content/BitmapFont.h"
#include "../content/TextLayout.h"
#include "../content/mrb/MrbReader.h"
#include "../display/DrawBuffer.h"
#include "IScreen.h"
#include "ReaderOptionsScreen.h"

namespace wintergreen {

// Simple EPUB page viewer.
// Renders text using the 8Ã—8 bitmap font scaled 2Ã— (16Ã—16 glyphs).
// Button2 = next page, Button3 = prev page, Button0 = back to menu.
// Button1 = open chapter list (if TOC available).
class ReaderScreen final : public IScreen {
 public:
  // Get the current book path
  std::string get_path() {
    return path_;
  }
  ReaderScreen() = default;
  explicit ReaderScreen(std::string epub_path) : path_(std::move(epub_path)) {}

  void set_path(std::string epub_path) {
    path_ = std::move(epub_path);
  }
  bool has_path() const {
    return !path_.empty();
  }
  void set_data_dir(std::string dir) {
    data_dir_ = std::move(dir);
  }

  // Set the proportional bitmap font for rendering. If null, falls back to
  // the builtin 8Ã—8 bitmap font at 2Ã— scale. The font data must outlive
  // this screen.
  void set_font(const BitmapFont* font) {}

  // Set the full font set (Small/Normal/Large). Font data must outlive this screen.
  void set_fonts(const BitmapFontSet* fonts) {
    ext_font_set_ = fonts;
  }

  // Export helpers.
  bool render_current_page(DrawBuffer& buf);
  bool next_page_and_render(DrawBuffer& buf);
  bool is_open_ok() const;

  // Render benchmark: calls render_page_ `iterations` times on the current page
  // and logs timing stats (per-iteration + summary). ESP32-only; no-op on desktop.
  void bench_render(DrawBuffer& buf, int iterations = 100);
  size_t current_chapter_index() const;

  // Test accessors — expose internal state so tests can drive the real screen
  // without duplicating its logic.
  const MrbReader& test_mrb() const {
    return mrb_;
  }

  const char* name() const override {
    return "Reader";
  }

  void start(DrawBuffer& buf, IRuntime& runtime) override;
  void stop() override;
  // pause(): freeze session timer while a child screen (options/chapter) is active.
  void pause() override;
  // resume(): return from a child screen — handle any pending navigation, then re-render.
  void resume(DrawBuffer& buf, IRuntime& runtime) override;
  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

  // Layout constants â€” exposed so tests and tools can build matching PageOptions.
  static constexpr int kScale = 2;
  static constexpr int kGlyphW = 8;
  static constexpr int kGlyphH = 8;
  static constexpr int kPaddingTop = 0;
  static constexpr int kPaddingLeft = 12;
  static constexpr int kParaSpacing = 8;

  // Build the fixed fallback font used when no proportional font is loaded.
  static FixedFont make_fixed_font() {
    return FixedFont(kGlyphW * kScale, kGlyphH * kScale + 4);
  }

  // Build PageOptions matching the reader's layout configuration. Margins are
  // fixed (see ReaderSettings), so this is fully determined by the page size.
  static PageOptions make_page_opts(int width = DrawBuffer::kWidth, int height = DrawBuffer::kHeight) {
    PageOptions opts(static_cast<uint16_t>(width), static_cast<uint16_t>(height), kPaddingTop, kParaSpacing);
    opts.padding_right = ReaderSettings::h_padding();
    opts.padding_left = ReaderSettings::h_padding();
    opts.padding_top = static_cast<uint16_t>(kPaddingTop + ReaderSettings::v_padding());
    opts.padding_bottom = ReaderSettings::bottom_margin();
    opts.center_text = true;
    return opts;
  }

 private:
  BitmapFontSet font_set_;                       // owned set (for single-font set_font() path)
  const BitmapFontSet* ext_font_set_ = nullptr;  // external set (from set_fonts())
  std::string path_;
  std::string data_dir_;
  std::string book_cache_dir_;
  std::string mrb_path_;
  std::string pos_path_;       // path to .pos bookmark: <data_dir>/data/<book_key>.pos
  std::string book_key_;       // sanitized title (content-derived), drives .pos filename
  DrawBuffer* buf_ = nullptr;  // set in start(), cleared in stop()
  MrbReader mrb_;
  std::unique_ptr<MrbChapterSource> chapter_src_;
  size_t chapter_idx_ = 0;
  TextLayout layout_engine_;
  PagePosition page_pos_;
  PageContent page_;

  // One laid-out page held ahead of use. prev_page_() has to run a full
  // layout_backward() just to learn where the previous page starts; without this it
  // threw the result away and render_page_() laid the identical page out again, so
  // every backward turn cost two layouts.
  //
  // The key must match on all of chapter/position/geometry: set_font() and
  // set_options() invalidate TextLayout's paragraph cache wholesale, so a stale entry
  // does not merely draw the wrong page, it points at freed line vectors.
  struct LaidOutPageCache {
    bool valid = false;
    size_t chapter_idx = 0;
    PagePosition pos;
    PageOptions opts;
    uint8_t font_size_idx = 0;
    PageContent page;
  };
  LaidOutPageCache page_cache_;
  PageOptions last_opts_;  // geometry of the most recent render, stamped onto the cache

  HoldRepeat hold_next_;
  HoldRepeat hold_prev_;

  bool open_ok_ = false;
  bool buf_was_touched_ = false;


  // Reader options menu â€” pushed when user presses Button1.
  // Prep (set_settings + populate) happens before calling app_->push_screen(ReaderOptions).
  ReaderSettings reader_settings_;  // user-adjustable settings, mutated by reader_options_

  // Saved position (survives stop()) so we can restore after chapter select cancel.
  size_t saved_chapter_idx_ = 0;
  PagePosition saved_page_pos_;

  ImageSizeQuery image_size_fn_;

  // Returns the filename stem of path_ (no directory, no extension).
  std::string book_stem_() const;

  // The book's title as it should be shown. Falls back to the containing folder
  // name when the MRB has no usable title — a converted EPUB whose metadata said
  // nothing yields the literal "none", which is what the book list already
  // substitutes for, so the two screens must not disagree.
  std::string display_title_() const;

  bool decode_image_to_buffer_(uint16_t img_key, uint32_t offset, uint32_t size, DrawBuffer& buf, int dest_x,
                               int dest_y, uint16_t max_w, uint16_t max_h, uint16_t src_y = 0, uint16_t clip_h = 0);
  void render_page_(DrawBuffer& buf);
  // Bottom margin below the text block (nothing is drawn there).
  static uint16_t bottom_padding_(bool landscape);
  void render_text_(DrawBuffer& buf, const BitmapFontSet& fset, int left_padding);
  bool next_page_();
  bool prev_page_();
  void load_chapter_(size_t idx);
  // Stash a page the caller already laid out, keyed on its own start position.
  void cache_page_(PageContent&& pc);
  // Move the cached page into page_ if it matches pos under the current geometry.
  bool take_cached_page_(const PagePosition& pos, const PageOptions& opts);
  void save_position_();
  void load_position_();

 public:
  bool is_open() const { return open_ok_; }

  std::string book_title() const { return mrb_.metadata().title; }
  uint16_t chapter_index() const { return static_cast<uint16_t>(chapter_idx_); }
  uint16_t chapter_count() const { return mrb_.chapter_count(); }

  // Access to user-adjustable display settings (read/write by Application for persistence).
  ReaderSettings& reader_settings() {
    return reader_settings_;
  }
  const ReaderSettings& reader_settings() const {
    return reader_settings_;
  }

  // Returns progress percentage 0-100 based on read characters (whole book)
  int progress_pct() const {
    if (mrb_.paragraph_count() == 0)
      return 0;
    const bool is_last_chapter = chapter_idx_ + 1 >= mrb_.chapter_count();
    if (page_.at_chapter_end && is_last_chapter)
      return 100;
    const uint64_t total_chars = mrb_.total_char_count();
    uint64_t chars_before = 0;
    for (size_t i = 0; i < chapter_idx_; ++i)
      chars_before += mrb_.chapter_char_count(static_cast<uint16_t>(i));
    const uint64_t cur =
        chars_before + (chapter_src_ ? chapter_src_->char_before_para(page_pos_.paragraph) : 0) + page_pos_.text_offset;
    return total_chars > 0 ? static_cast<int>(cur * 100u / total_chars) : 0;
  }

  // Returns progress percentage 0-100 within the current chapter
  int chapter_progress_pct() const {
    if (mrb_.paragraph_count() == 0)
      return 0;
    if (page_.at_chapter_end)
      return 100;
    const uint64_t chapter_chars = mrb_.chapter_char_count(static_cast<uint16_t>(chapter_idx_));
    const uint64_t cur =
        (chapter_src_ ? chapter_src_->char_before_para(page_pos_.paragraph) : 0) + page_pos_.text_offset;
    return chapter_chars > 0 ? static_cast<int>(cur * 100u / chapter_chars) : 0;
  }
};

}  // namespace wintergreen
