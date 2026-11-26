#pragma once

// Gray reader pages: the one place the custom Turn LUT (platforms/esp32/panel_lut.h)
// runs. Everything else on the device stays 1-bit on the panel's OTP waveforms.
//
// A reading page has three levels — white, gray (antialiasing), black — from the
// two-plane reader font (hi = gray or black, lo = black, set = ink). While a
// gray page is up the glass is held as those two planes in DrawBuffer's two
// framebuffers (hi in the draw buffer, lo in the spare), which the reader has
// no other use for. A turn renders only the new page, from its baked display
// list, one strip of kStripRows physical rows at a time, compares it with the
// planes, gives every pixel its Turn slot (state = RED << 1 | BW) and writes the
// new levels back:
//
//   00 HOLD       unchanged
//   01 to-white   became white
//   10 to-gray    became gray
//   11 to-black   black inside a strip that changed (re-driven, as the stock
//                 waveform does)
//
// A strip with no change sends nothing once the controller's RAM for it holds
// HOLD (dirty_), and an update that changes nothing fires nothing. Windows are
// queued: the CPU renders strip k + 1 while DMA sends strip k.
//
// What the glass holds (Glass):
//   Frame    the 1-bit frame in the draw buffer, put up by an OTP update (list,
//            home, quick menu): old levels come from it, exactly.
//   Page     a gray page: the planes, while planes_ok_ (any 1-bit draw or spare
//            claim spends them; then every pixel is driven).
//   Unknown  anything else (boot): every pixel is driven.
//
// Leaving a gray page for an OTP update needs RED rewritten first (handoff()):
// the OTP fast waveform reads RED as the previous 1-bit frame and leaves pixels
// with BW == RED alone, but after a gray update RED holds slots. By then the
// next screen has drawn over the planes, so the handoff replays the kept display
// list (kept_) instead. Gray pixels get RED = the opposite of the new pixel, so
// the OTP waveform drives them either way; image areas are not re-read and are
// driven the same way.
//
// Images in pages stay 1-bit (WgbFormat.h); each strip reads its part of each
// image from the card (one contiguous read: rows in landscape, columns in
// portrait).

#include <cstddef>
#include <cstdint>

#include "../content/BitmapFont.h"
#include "../content/wgb/WgbPageView.h"

namespace wintergreen {

class IDisplay;
enum class Rotation;

// A page's image pixels, read during the update (card: the panel is idle and
// the queued windows have drained).
class GrayImageSource {
 public:
  virtual ~GrayImageSource() = default;
  // Copy [lo, hi) of `img`'s stored bitmap into dst (WgbFormat.h): Deg0 = rows
  // of the full image, (w + 7) / 8 bytes each; Deg90 = columns, (full_h + 7) / 8
  // bytes each. Bit clear = ink. False = draw nothing there (white).
  virtual bool read_image(const WgbImage& img, Rotation rot, int lo, int hi, uint8_t* dst, size_t cap) = 0;
};

class GrayPage {
 public:
  static constexpr int kStripRows = 16;
  static constexpr int kStrips = 30;  // 480 physical rows (static_assert in GrayPage.cpp)

  enum class Glass : uint8_t { Unknown, Frame, Page };

  GrayPage();
  ~GrayPage();
  GrayPage(const GrayPage&) = delete;
  GrayPage& operator=(const GrayPage&) = delete;

  // The page the next refresh puts up. `list` must stay valid until then; it is
  // copied when the update fires. `bar_h` > 0 draws the low-battery bar.
  void set(const uint8_t* list, size_t len, const BitmapFontSet* fonts, Rotation rot, int bar_h,
           GrayImageSource* images);
  bool pending() const {
    return pending_;
  }
  void drop_pending() {
    pending_ = false;
  }

  Glass glass() const {
    return glass_;
  }
  // After an OTP update (Frame) or anything unknown: the controller RAM holds
  // no HOLD state anywhere.
  void set_glass(Glass g);
  // The framebuffers holding the planes were drawn into or lent.
  void invalidate_planes() {
    planes_ok_ = false;
  }

  // Fire the pending page with the Turn LUT. `hi` is the draw buffer: the
  // 1-bit frame on the glass when glass() == Frame, the hi plane when Page, and
  // the new hi plane afterwards. `lo` is the spare (null: lent, then every
  // pixel of the next turn is driven).
  void show(IDisplay& d, uint8_t* hi, uint8_t* lo);

  // Before the first OTP update after a gray page: write RED from the kept page
  // against `new_frame` (the 1-bit frame about to go up). No-op unless Page.
  void handoff(IDisplay& d, const uint8_t* new_frame);

 private:
  struct Page {
    const uint8_t* list = nullptr;
    size_t len = 0;
    const BitmapFontSet* fonts = nullptr;
    Rotation rot{};
    int bar_h = 0;
  };

  Page next_;
  GrayImageSource* images_ = nullptr;
  bool pending_ = false;
  Glass glass_ = Glass::Unknown;
  bool planes_ok_ = false;
  bool dirty_[kStrips];  // the strip's controller RAM may hold a non-HOLD slot

  Page kept_;  // the page on the glass; list points into kept_buf_
  uint8_t* kept_buf_;

  // Scratch, one strip each: the new planes, an image read, two send pairs.
  uint8_t* new_hi_;
  uint8_t* new_lo_;
  uint8_t* img_;
  uint8_t* send_[2][2];

  // A page's items, decoded once per update rather than once per strip (each
  // strip used to re-parse the whole list, every word of every line).
  struct Item {
    WgbLine line;
    union {
      WgbRule rule;
      WgbImage img;
    };
    uint8_t tag;
  };
  static constexpr int kMaxItems = 64;  // a page past this is parsed per strip
  Item* items_;
  int n_items_ = 0;
  const Page* indexed_ = nullptr;  // the page items_ holds; null = none
  void index_(const Page& p);
  static bool decode_(WgbPageView& v, Item& it);

  // Replay `p` into one strip (rows [r0, r0 + rows)). With `d`, images are read
  // from images_ (after d->finish_writes()); without, their area is set in `unk`.
  void render_(const Page& p, int r0, int rows, uint8_t* hi, uint8_t* lo, uint8_t* unk, IDisplay* d) const;
};

}  // namespace wintergreen
