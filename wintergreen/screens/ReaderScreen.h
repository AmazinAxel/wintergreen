#pragma once

#include <cstdlib>
#include <vector>

#include "../Input.h"
#include "../content/BitmapFont.h"
#include "../content/PageGeometry.h"
#include "../content/wgb/WgbReader.h"
#include "../display/DrawBuffer.h"
#include "IScreen.h"
#include "QuickmenuScreen.h"

namespace wintergreen {

// The WGB page viewer.
//
// **Pages are fully baked** (see WgbFormat.h): the book carries every page of
// every layout as a display list, so the reader's position is just a page index
// into the current layout. Forward and backward are ±1, a chapter is a page,
// progress is page / page_count. A page turn records the display list (read
// into DrawBuffer's tail, usually staged there after the last waveform) as
// draw ops; nothing on this path allocates.
class ReaderScreen final : public IScreen, public PixelSource {
 public:
  // The current book path
  const std::string& get_path() const {
    return path_;
  }
  ReaderScreen() = default;

  void set_path(std::string wgb_path) {
    path_ = std::move(wgb_path);
  }
  bool has_path() const {
    return !path_.empty();
  }

  // The reader font set (one face, several sizes). Font data must outlive this
  // screen. Without it no book opens: there is no fallback face.
  void set_fonts(const BitmapFontSet* fonts) {
    ext_font_set_ = fonts;
  }

  void start(DrawBuffer& buf, IRuntime& runtime) override;
  void stop() override;
  // pause(): freeze session timer while a child screen (options/chapter) is active.
  void pause() override;
  // resume(): return from a child screen — handle any pending navigation, then re-render.
  void resume(DrawBuffer& buf, IRuntime& runtime) override;
  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

  // PixelSource: image pixels from the open book.
  bool read(uint32_t item, uint32_t offset, uint8_t* dst, size_t n) override;

 private:
  const BitmapFontSet* ext_font_set_ = nullptr;  // from set_fonts()
  std::string path_;
  std::string pos_path_;       // <book dir>/book.pos, beside the book itself
  DrawBuffer* buf_ = nullptr;  // set in start(), cleared in stop()

  WgbReader wgb_;
  uint8_t layout_ = 0;  // wgb_layout_index of page_
  uint32_t page_ = 0;   // index into layout_'s pages

  // Read page `index` of `layout`'s display list into DrawBuffer's tail
  // (nothing when it is already there).
  bool load_page_(DrawBuffer& buf, uint8_t layout, uint32_t index);
  // Read page_ + 1's list into the free list's tail — see the definition.
  void stage_next_page_(DrawBuffer& buf);
  // A page went up: stage the next one on the first frame the panel is idle.
  bool stage_pending_ = false;

  // Identity of this opening's images (DrawBuffer::blit): book path key mixed
  // with an open counter.
  uint32_t image_gen_ = 0;
  uint32_t opens_ = 0;

  HoldRepeat hold_next_;
  HoldRepeat hold_prev_;

  bool open_ok_ = false;

  // Low-battery reminder bar, drawn along the bottom of a reading page only.
  // Sampled from the runtime at the entry points that have one and cached here.
  //
  // Latches at or below the threshold until the next boot — see
  // sample_battery_().
  static constexpr int kLowBatteryBarPct = 15;
  bool low_battery_ = false;
  void sample_battery_(IRuntime& runtime);

  // Direction of the last turn that moved (+1/-1), 0 when the last batch moved
  // nothing. Button::Undo reverts exactly that step, without drawing.
  int8_t last_step_ = 0;
  // The glass shows a page that Undo has since left, so resume() must not put
  // the parked snapshot back.
  bool pixels_stale_ = false;

  // Guards the resume() fast path — see pause().
  bool snapshot_ok_ = false;
  uint8_t snapshot_font_size_ = 0;
  uint8_t snapshot_rotation_ = 0;

  // Reader options menu pushed when user presses Button1.
  ReaderSettings reader_settings_;  // user-adjustable settings, mutated by quickmenu_

  // The book's title as it should be shown. Falls back to the containing folder
  // name when the WGB has no usable title — a converted EPUB whose metadata said
  // nothing yields the literal "none", which is what the book list already
  // substitutes for, so the two screens must not disagree.
  std::string display_title_() const;

  // The layout the current font size and rotation call for.
  uint8_t wanted_layout_(const DrawBuffer& buf) const;
  // Move page_ into the wanted layout after a font-size or rotation change: the
  // page holding the same text position.
  void sync_layout_(const DrawBuffer& buf);

  // Record a stored image (or the slice on this page) — see the definition.
  bool draw_image_(DrawBuffer& buf, const WgbImage& img);
  void render_page_(DrawBuffer& buf);
  // Record the display list in DrawBuffer's tail (`have_list` false: blank).
  void draw_page_(DrawBuffer& buf, bool have_list);
  bool next_page_();
  bool prev_page_();

  // book.pos holds the layout-independent position of page_'s start:
  // (paragraph record offset, text offset). Last value written or read, so an
  // unchanged position is not rewritten.
  struct PosKey {
    uint32_t para_offset = 0;
    uint16_t text_offset = 0;
    bool operator==(const PosKey& o) const {
      return para_offset == o.para_offset && text_offset == o.text_offset;
    }
  };
  PosKey pos_written_{};
  bool pos_written_valid_ = false;
  void save_position_();
  bool load_position_(PosKey& out);

 public:
  // Access to user-adjustable display settings (read/write by Application for persistence).
  ReaderSettings& reader_settings() {
    return reader_settings_;
  }
  const ReaderSettings& reader_settings() const {
    return reader_settings_;
  }

  // Whole-book progress, 0-100, by page; the last page is 100.
  int progress_pct() const {
    const uint32_t n = wgb_.page_count(layout_);
    if (n == 0)
      return 0;
    if (page_ + 1 >= n)
      return 100;
    return static_cast<int>(static_cast<uint64_t>(page_) * 100u / n);
  }

 private:
  // Progress within the chapter holding page_, by page; its last page is 100.
  int chapter_progress_pct_(const std::vector<uint32_t>& chapter_pages) const;
};

}  // namespace wintergreen
