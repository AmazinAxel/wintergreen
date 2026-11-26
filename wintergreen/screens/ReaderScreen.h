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
// progress is page / page_count. A page turn takes the page's display list (read
// ahead after the last waveform, or read now) and hands it to DrawBuffer's gray
// path (GrayPage.h), which renders it strip by strip on the Turn LUT; nothing on
// this path allocates.
class ReaderScreen final : public IScreen, private GrayImageSource {
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

 private:
  const BitmapFontSet* ext_font_set_ = nullptr;  // from set_fonts()
  std::string path_;
  std::string pos_path_;       // <book dir>/book.pos, beside the book itself
  DrawBuffer* buf_ = nullptr;  // set in start(), cleared in stop()

  WgbReader wgb_;
  uint8_t layout_ = 0;  // wgb_layout_index of page_
  uint32_t page_ = 0;   // index into layout_'s pages

  // Three display-list buffers, claimed at boot (with the screen) and never
  // freed: the page being shown (rendered straight out of it; GrayPage copies it
  // once up) and its two neighbours, read ahead after each waveform. A turn
  // either way onto a read-ahead page swaps pointers; the slot then holds the
  // page just left, which is the new page's other neighbour.
  uint8_t* page_buf_ = static_cast<uint8_t*>(std::malloc(kWgbMaxPageBytes));
  size_t page_len_ = 0;
  // Which page a buffer holds.
  struct Loaded {
    bool valid = false;
    uint8_t layout = 0;
    uint32_t index = 0;
    bool is(uint8_t l, uint32_t i) const {
      return valid && layout == l && index == i;
    }
  };
  Loaded loaded_;
  struct Ahead {
    uint8_t* buf = static_cast<uint8_t*>(std::malloc(kWgbMaxPageBytes));
    size_t len = 0;
    Loaded at;
  };
  Ahead ahead_[2];
  // Put page `index` of `layout`'s display list in page_buf_.
  bool load_page_(DrawBuffer& buf, uint8_t layout, uint32_t index);
  // Read page_ + 1 and page_ - 1 into ahead_ (panel idle only).
  void prefetch_();

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
  void sync_layout_(DrawBuffer& buf);

  // GrayImageSource: one strip's part of a stored 1-bit image — see the definition.
  bool read_image(const WgbImage& img, Rotation rot, int lo, int hi, uint8_t* dst, size_t cap) override;
  // The last image table entry read_image() fetched (per open book).
  uint32_t ref_key_ = UINT32_MAX;
  WgbImageRef ref_{};
  // Load page_ and set it as the next gray page.
  void render_page_(DrawBuffer& buf);
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
