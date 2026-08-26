#pragma once

#include <functional>
#include <memory>

#include "../Input.h"
#include "../content/BitmapFont.h"
#include "../content/TextLayout.h"
#include "../content/wgb/WgbReader.h"
#include "../display/DrawBuffer.h"
#include "IScreen.h"
#include "QuickmenuScreen.h"

namespace wintergreen {

// Simple EPUB page viewer.
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
  // Set the proportional bitmap font for rendering. If null, falls back to
  // the builtin 8×8 bitmap font at 2× scale. The font data must outlive
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

  // Drop every cache that exists only to make a page turn fast: the laid-out
  // page and the layout engine's paragraph cache. Called when a radio comes up,
  // because both are large and both rebuild themselves on demand.
  //
  // The reader keeps working — a turn re-lays out instead of being a memcpy.
  // Without this a backward turn with BLE resident throws std::bad_alloc inside
  // assemble_page's word_pool reserve, and with exceptions off that is an
  // abort(): the device reboots mid-book.
  // True while a radio holds the heap, so load_chapter_() re-applies the caps
  // to a newly built chapter source.
  bool radio_ram_hold_ = false;

  void release_caches() {
    radio_ram_hold_ = true;
    // Order matters: both caches hold LayoutWord::text pointers into the
    // chapter source's paragraph slots, so they must be dropped *before* the
    // window shrinks under them.
    page_cache_ = LaidOutPageCache{};
    layout_engine_.release_cache_memory();
    layout_engine_.set_cache_limit(2);
    // page_ still points into the slot holding the current paragraph, so that
    // one is named as live and kept.
    if (chapter_src_)
      chapter_src_->set_window_limit(4, page_pos_.paragraph);
  }

  // Undo release_caches()'s cap once the radio is down. The caches themselves
  // refill on demand, so nothing needs restoring but the limit.
  void restore_caches() {
    radio_ram_hold_ = false;
    layout_engine_.set_cache_limit(TextLayout::kCacheCapacity);
    if (chapter_src_)
      chapter_src_->set_window_limit(WgbChapterSource::kWindowSize);
  }

  size_t current_chapter_index() const;

  // Test accessors — expose internal state so tests can drive the real screen
  // without duplicating its logic.
  const WgbReader& test_wgb() const {
    return wgb_;
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

  // Layout constants exposed so tests and tools can build matching PageOptions
  static constexpr int kScale = 2;
  static constexpr int kGlyphW = 8;
  static constexpr int kGlyphH = 8;
  static constexpr int kPaddingTop = 0;
  static constexpr int kPaddingLeft = 12;
  static constexpr int kParaSpacing = 8;

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
  std::string wgb_path_;
  std::string pos_path_;       // <book dir>/book.pos, beside the book itself
  DrawBuffer* buf_ = nullptr;  // set in start(), cleared in stop()
  WgbReader wgb_;
  std::unique_ptr<WgbChapterSource> chapter_src_;
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

  // Low-battery reminder bar, drawn along the bottom of a reading page only.
  // Sampled from the runtime at the entry points that have one and cached here,
  // because render_page_() is also reached from the pre-draw path, which has no
  // runtime. A stale sample only delays the bar by one page turn.
  //
  // Latches at or below the threshold and clears only on USB — see
  // sample_battery_().
  static constexpr int kLowBatteryBarPct = 15;
  static constexpr int kLowBatteryBarH = 5;
  bool low_battery_ = false;
  void sample_battery_(IRuntime& runtime);

  // Guards the resume() fast path — see pause().
  bool snapshot_ok_ = false;
  uint8_t snapshot_font_size_ = 0;
  uint8_t snapshot_rotation_ = 0;


  // Reader options menu pushed when user presses Button1.
  // Prep (set_settings + populate) happens before calling app_->push_screen(Quickmenu).
  ReaderSettings reader_settings_;  // user-adjustable settings, mutated by quickmenu_

  // Saved position (survives stop()) so we can restore after chapter select cancel.
  size_t saved_chapter_idx_ = 0;
  PagePosition saved_page_pos_;

  ImageSizeQuery image_size_fn_;

  // Returns the filename stem of path_ (no directory, no extension).
  std::string book_stem_() const;

  // The book's title as it should be shown. Falls back to the containing folder
  // name when the WGB has no usable title — a converted EPUB whose metadata said
  // nothing yields the literal "none", which is what the book list already
  // substitutes for, so the two screens must not disagree.
  std::string display_title_() const;

  // Blit a stored 1-bit image (or the slice on this page) — see the definition.
  bool draw_image_(uint32_t offset, uint32_t size, DrawBuffer& buf, int dest_x, int dest_y, uint16_t width,
                   uint16_t height, uint16_t src_y = 0, uint16_t clip_h = 0);
  void render_page_(DrawBuffer& buf);
  // Bottom margin below the text block (nothing is drawn there).
  static uint16_t bottom_padding_(bool landscape);
  void render_text_(DrawBuffer& buf, const BitmapFontSet& fset, int left_padding);
  bool next_page_();
  bool prev_page_();
  void load_chapter_(size_t idx);
  // Stash a page the caller already laid out, keyed on its own start position.
  void cache_page_(PageContent&& pc);
  // Lay out the following page during the e-ink waveform. See the comment on the
  // definition — it releases page_'s word storage, so nothing may read page_.items
  // after a render completes.
  void prerender_next_page_(DrawBuffer& buf);
  // Commit the pre-drawn page if it still matches; false = render normally.
  bool take_predrawn_(DrawBuffer& buf);

  // Metadata for the page sitting in DrawBuffer's offscreen buffer.
  struct PreDrawn {
    bool valid = false;
    size_t chapter_idx = 0;
    PagePosition request;  // what next_page_() will produce, before resolving
    PagePosition start;
    PagePosition end;
    bool at_chapter_end = false;
    PageOptions opts;
    uint8_t font_size_idx = 0;
  };
  PreDrawn predrawn_;

  // Move the cached page into page_
  bool take_cached_page_(const PagePosition& pos, const PageOptions& opts);
  // Last position actually written to .pos, so an unchanged position is not
  // rewritten. See save_position_().
  struct PosKey {
    size_t chapter = 0;
    uint16_t paragraph = 0;
    uint16_t offset = 0;
    uint32_t text_offset = 0;
    bool operator==(const PosKey& o) const {
      return chapter == o.chapter && paragraph == o.paragraph && offset == o.offset &&
             text_offset == o.text_offset;
    }
  };
  PosKey pos_written_{};
  bool pos_written_valid_ = false;
  void save_position_();
  void load_position_();

 public:
  bool is_open() const { return open_ok_; }

  std::string book_title() const { return wgb_.metadata().title; }
  uint16_t chapter_index() const { return static_cast<uint16_t>(chapter_idx_); }
  uint16_t chapter_count() const { return wgb_.chapter_count(); }

  // Access to user-adjustable display settings (read/write by Application for persistence).
  ReaderSettings& reader_settings() {
    return reader_settings_;
  }
  const ReaderSettings& reader_settings() const {
    return reader_settings_;
  }

  // Returns progress percentage 0-100 based on read characters (whole book)
  int progress_pct() const {
    if (wgb_.paragraph_count() == 0)
      return 0;
    const bool is_last_chapter = chapter_idx_ + 1 >= wgb_.chapter_count();
    if (page_.at_chapter_end && is_last_chapter)
      return 100;
    const uint64_t total_chars = wgb_.total_char_count();
    uint64_t chars_before = 0;
    for (size_t i = 0; i < chapter_idx_; ++i)
      chars_before += wgb_.chapter_char_count(static_cast<uint16_t>(i));
    const uint64_t cur =
        chars_before + (chapter_src_ ? chapter_src_->char_before_para(page_pos_.paragraph) : 0) + page_pos_.text_offset;
    return total_chars > 0 ? static_cast<int>(cur * 100u / total_chars) : 0;
  }

  // Returns progress percentage 0-100 within the current chapter
  int chapter_progress_pct() const {
    if (wgb_.paragraph_count() == 0)
      return 0;
    if (page_.at_chapter_end)
      return 100;
    const uint64_t chapter_chars = wgb_.chapter_char_count(static_cast<uint16_t>(chapter_idx_));
    const uint64_t cur =
        (chapter_src_ ? chapter_src_->char_before_para(page_pos_.paragraph) : 0) + page_pos_.text_offset;
    return chapter_chars > 0 ? static_cast<int>(cur * 100u / chapter_chars) : 0;
  }
};

} // namespace wintergreen
