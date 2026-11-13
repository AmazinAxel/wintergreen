#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "../content/BitmapFont.h"
#include "../content/wgb/WgbPageView.h"

namespace wintergreen {

// Deg90 is portrait (every list, and the reader's default); Deg0 is the reader's
// landscape. The reversed orientations were dropped — see rotation_from_setting.
enum class Rotation { Deg0 = 0, Deg90 = 90 };

// Refresh mode for full-screen updates.
enum class RefreshMode { Full, Half };

// Physical screen constants and bit-packed pixel helpers (used internally by DrawBuffer).
struct DisplayFrame {
  // 7 + 793 = 800: the panel's hidden columns are all at the leading edge, not
  // split across both. This said 10 + 786 once, leaving 4 columns that no
  // drawing code could reach — they kept the buffers' 0xFF init and showed as a
  // white bar along the top in portrait, most visible against a dark cover.
  // Determined on hardware: dropping the offset to 7 moved the bar wholesale to
  // the trailing edge rather than splitting it, so nothing is hidden there.
  static constexpr int kPhysicalWidth = 793;  // visible app-space width
  static constexpr int kPanelOffsetX = 7;     // hidden columns at the leading edge of the 800px panel
  static constexpr int kPanelWidth = 800;
  static constexpr int kPhysicalHeight = 480;
  static constexpr int kStride = kPanelWidth / 8;  // 100 bytes/row (800 divisible by 8)
  static constexpr std::size_t kPixelBytes = static_cast<std::size_t>(kStride) * kPhysicalHeight;  // 48000
};
// Display driver interface. EInkDisplay is the only implementation left now that
// the desktop build is gone — see "Investigated and rejected" in docs/history.md.
class IDisplay {
 public:
  virtual ~IDisplay() = default;

  // Full physical refresh. `pixels` is the new frame; `prev` is the one currently
  // on the glass, or nullptr when it is not known.
  //
  // RefreshMode::Full drives each pixel from its actual old state to its new one,
  // which needs `prev` in RED RAM — see EInkDisplay::full_refresh. Half ignores
  // `prev` entirely and is the boot paint; don't change what it does.
  virtual void full_refresh(const uint8_t* pixels, const uint8_t* prev, RefreshMode mode,
                            bool turnOffScreen = false) = 0;

  // Partial refresh: new_pixels -> BW RAM, then fire the waveform without waiting.
  virtual void partial_refresh(const uint8_t* new_pixels) = 0;

  // Put the display controller into deep sleep (low-power mode).
  virtual void deep_sleep() {}

  // Returns true if the display hardware is currently busy refreshing.
  virtual bool is_busy() const {
    return false;
  }

  // Block until any outstanding refresh has finished. Refreshes are fired without
  // waiting, so callers that are about to use a bus the panel shares (SD on SPI2)
  // must drain first — concurrent SD traffic corrupts an in-flight update.
  virtual void wait_idle() {}
};

// Framebuffer with simple draw helpers.
//
// Uses Deg90 (portrait) rotation: logical 480x793, physical 793x480.
// One draw buffer: refresh() sends it to the panel and does not flip. The
// controller keeps the pixels in its own RAM and the SPI write is synchronous,
// so the buffer is free to draw into as soon as refresh() returns, and between
// refreshes it holds the frame on the glass. Every draw path starts from
// fill() or from a whole-buffer swap, so nothing reads the previous frame.
// A second host copy of the displayed frame (there used to be one) had no
// reader: the fast waveform never takes RED RAM from the host, and
// RefreshMode::Full, the only mode that would, has no caller. A future Full
// caller must keep its own copy of the old frame.
//
// The framebuffers are never loaned out as scratch: image decoding and EPUB
// conversion, the two things that used to borrow them, are both gone from the
// firmware.
//
// The text and bitmap routines live in DrawBuffer.cpp, firmware only. Defined
// inline here they were cloned into every screen's TU — four copies of the glyph
// loop in the image — and the host converter, which includes this header only
// for kWidth/kHeight, never calls them.
class DrawBuffer {
 public:
  // Logical portrait dimensions.
  static constexpr int kWidth = DisplayFrame::kPhysicalHeight;
  static constexpr int kHeight = DisplayFrame::kPhysicalWidth;
  static constexpr size_t kBufSize = DisplayFrame::kPixelBytes;

  explicit DrawBuffer(IDisplay& display) : display_(display) {
    memset(buf_, 0xFF, kBufSize);
  }

  ~DrawBuffer() {
    std::free(buf_);
    std::free(spare_);
  }

  // Owns a raw allocation; copying would double-free. There is exactly one
  // DrawBuffer and it lives for the whole session, so deleting these costs
  // nothing and makes the mistake unrepresentable.
  DrawBuffer(const DrawBuffer&) = delete;
  DrawBuffer& operator=(const DrawBuffer&) = delete;

  IDisplay& display() {
    return display_;
  }
  const IDisplay& display() const {
    return display_;
  }

  // Set the logical rotation used by every draw call. There is no driver-side
  // rotation: the panel is always written in physical coordinates and the
  // transform is applied here.
  void set_rotation(Rotation r) {
    rotation_ = r;
  }
  int width() const {
    return rotation_ == Rotation::Deg0 ? DisplayFrame::kPhysicalWidth : kWidth;
  }
  int height() const {
    return rotation_ == Rotation::Deg0 ? DisplayFrame::kPhysicalHeight : kHeight;
  }

  Rotation rotation() const {
    return rotation_;
  }

  // -- Draw helpers (logical coordinates)
  // ----------------------------------------

  // Fill the entire draw buffer.
  void fill(bool white = true) {
    memset(draw_(), white ? 0xFF : 0x00, kBufSize);
  }

  // Fill a logical rectangle.
  void fill_rect(int lx, int ly, int lw, int lh, bool white) {
    if (rotation_ == Rotation::Deg0)
      fill_rect_physical_(draw_(), lx, ly, lw, lh, white);
    else
      fill_rect_physical_(draw_(), ly, DisplayFrame::kPhysicalHeight - lx - lw, lh, lw, white);
  }

  // Blit a 1-bit image (MSB first, bit set = white, rows `stride` bytes apart)
  // with its top-left at logical (lx, ly). Clipped to the panel.
  //
  // Whole images rather than one row per call: in portrait a logical row is a
  // physical *column*, so per-row blitting could only ever set one bit at a
  // time. Given the rows together it packs eight of them into each output byte,
  // the same transpose the glyph path does.
  void blit_1bit(int lx, int ly, const uint8_t* data, int w, int h, size_t stride);

  // Same, from column-major data: logical column c is ceil(h / 8) bytes of its h
  // pixels, top first. In portrait each column is one physical row, so this is
  // a shifted byte copy. Home and list covers are baked this way.
  void blit_1bit_cols(int lx, int ly, const uint8_t* cols, int w, int h);

  // One 8-row band of a reader image (WgbFormat.h, portrait copy): w bytes, each
  // the band's 8 pixels of one column, top MSB. Top-left at (lx, ly); only band
  // rows [keep_lo, keep_hi) are drawn.
  void blit_1bit_bands(int lx, int ly, const uint8_t* band, int w, int keep_lo, int keep_hi);

  // The draw target as the panel's own frame (kStride bytes per physical row,
  // kBufSize bytes): a baked full-screen frame is read straight into it.
  uint8_t* frame() {
    return draw_();
  }

  // -- Text & Glyph rendering --

  // Draw proportional text using a BitmapFont. Cursor starts at (x, baseline_y)
  // where baseline_y is the Y position of the text baseline.
  // Returns the X position after the last character (cursor advance).
  int draw_text_proportional(int x, int baseline_y, const char* text, size_t len, const BitmapFont& font, bool white,
                             FontStyle style = FontStyle::Regular);

  // Convenience overload for null-terminated strings.
  int draw_text_proportional(int x, int baseline_y, const char* text, const BitmapFont& font, bool white,
                             FontStyle style = FontStyle::Regular) {
    return draw_text_proportional(x, baseline_y, text, text ? strlen(text) : 0, font, white, style);
  }

  // Render all words of a baked line (see WgbFormat.h), each in its own font
  // and style. `end` bounds the display list the line lives in.
  void draw_baked_line(const WgbLine& line, const uint8_t* end, const BitmapFontSet& fonts, bool white);

  void set_sunlight_fading_fix(bool v) { sunlight_fading_fix_ = v; }

  // -- Display operations
  // --------------------------------------------------

  // Push the draw buffer to the panel. No flip: see the class comment.
  //
  // **There is no periodic full-refresh flush**, and this is the third and final
  // position on that question. A run of fast partial waveforms is not
  // DC-balanced — each leaves a little residual charge, which accumulates as
  // ghosting — so the obvious design promotes every Nth update to a full
  // refresh, and that is what this used to do.
  //
  // It was removed because on this device the flush is redundant and the flash
  // is not. Deep-sleep wake is a full boot, so a session is bracketed by two
  // full-panel waveforms anyway: the sleep image on the way down and
  // Application::start()'s full_refresh() on the way up. With
  // kAutoSleepMinutes = 1 that happens every time the device is put down. The
  // in-session flush only ever added an interruption the sleep cycle had already
  // made unnecessary — inline it landed mid page turn, deferred it landed mid
  // *sentence*, and pushed out far enough not to be felt it never ran at all.
  //
  // Gone with it: partials_since_full_, flush_pending(), flush_ghosting(),
  // Application::kGhostFlushIdleMs and config::kFullRefreshEveryNUpdates. To
  // bring it back, count here and call full_refresh(RefreshMode::Full) from
  // wherever the interruption is acceptable — but read "Panel health" first.
  void refresh() {
    display_.partial_refresh(buf_);
  }

  // Full hardware refresh of the draw buffer. No previous frame is passed: no
  // host copy of it exists (class comment), and Half ignores it anyway.
  void full_refresh(RefreshMode mode = RefreshMode::Half, bool turnOffScreen = false) {
    display_.full_refresh(buf_, nullptr, mode, turnOffScreen || sunlight_fading_fix_);
  }


  // -- Spare buffer: snapshot and offscreen draw ---------------------------
  //
  // One extra 48 KB framebuffer serving two jobs that never overlap in time:
  //
  //   Snapshot   a copy of the displayed frame, so a screen overlaying another
  //              (the quick menu over the reader) can be dismissed without the
  //              screen underneath rebuilding its content.
  //   Offscreen  a place to draw the *next* page while the panel is still
  //              running the current page's waveform, so a forward turn becomes
  //              a memcpy plus an SPI write — no layout and no glyph blitting.
  //
  // They share storage because a reader is either in the quick menu or turning
  // pages, never both, and 48 KB is worth more as heap than as a second spare.
  // spare_use_ records which one is live; claiming it for one purpose silently
  // invalidates the other, and both consumers re-check validity before trusting
  // it (see ReaderScreen::resume and take_predrawn_).
  //
  // The framebuffers are never loaned out as scratch, so nothing else can
  // clobber this.
  enum class Spare : uint8_t { None, Snapshot, Offscreen, Cache };

  // The spare is claimed at construction and never given back, radios
  // included. The null checks below only cover a boot-time malloc failure.

  // Lend the spare to another task until return_spare(): the sync's download
  // buffers (wifi_sync.h). The caller is not on the UI thread, so every other
  // claim (cache, snapshot, pre-draw)
  // refuses while it is out, and the screens draw without it: the book list
  // shows no thumbnails until it comes back. Both calls happen on the UI thread
  // (wg_sync::poll), so the flag needs no lock. What the spare held is lost.
  uint8_t* lend_spare() {
    if (!spare_ || spare_lent_)
      return nullptr;
    spare_use_ = Spare::None;
    spare_lent_ = true;
    return spare_;
  }
  void return_spare() {
    spare_lent_ = false;
  }

  // Lend the spare as a cache that may outlive the screen using it: the book
  // list keeps its cover thumbnails here. Nothing draws into it while the list
  // is up, so the reader's two uses never overlap it. Like them it is
  // invalidated by any other claim — a snapshot, a pre-draw, a sync's loan.
  //
  // Validity is a generation, not just the enum: each time the spare *enters*
  // cache use its contents are garbage and the generation moves, so a cache that
  // was displaced and re-claimed cannot mistake its old slots for live ones. The
  // owner trusts what it wrote only while
  // spare_cache_valid(the generation it wrote under).
  //
  // `owner` distinguishes the two caches that use it — the book list's
  // thumbnails and the home screen's cover — so one claiming it moves the
  // generation and invalidates the other, rather than both trusting slots the
  // other has overwritten.
  uint8_t* claim_spare_cache(const void* owner) {
    if (!spare_ || spare_lent_) {
      spare_use_ = Spare::None;
      return nullptr;
    }
    if (spare_use_ != Spare::Cache || spare_owner_ != owner) {
      spare_use_ = Spare::Cache;
      spare_owner_ = owner;
      ++spare_cache_gen_;
    }
    return spare_;
  }
  uint32_t spare_cache_gen() const {
    return spare_cache_gen_;
  }
  bool spare_cache_valid(uint32_t gen) const {
    return spare_use_ == Spare::Cache && gen == spare_cache_gen_;
  }

  // The draw buffer holds the displayed frame, so a snapshot is a pointer
  // swap: the page becomes the spare and the overlay draws into what the spare
  // held (every draw starts with fill()). No 48 KB copy either way.
  void save_snapshot() {
    if (!spare_ || spare_lent_)
      return;
    std::swap(buf_, spare_);
    spare_use_ = Spare::Snapshot;
  }

  bool has_snapshot() const {
    return spare_use_ == Spare::Snapshot;
  }

  // Swap the snapshot back into the draw buffer, ready for the caller's
  // refresh(). The spare then holds the overlay's frame, which is garbage.
  bool restore_snapshot() {
    if (spare_use_ != Spare::Snapshot)
      return false;
    std::swap(buf_, spare_);
    spare_use_ = Spare::None;
    return true;
  }

  // Redirect every draw call into the spare buffer until end_offscreen(). The
  // draw buffer (the displayed frame) is untouched meanwhile.
  // Returns false when there is no spare; the caller must then skip the
  // pre-draw entirely rather than drawing into the live buffer.
  bool begin_offscreen() {
    if (!spare_ || spare_lent_)
      return false;
    draw_target_ = spare_;
    spare_use_ = Spare::None;  // not usable until end_offscreen says so
    return true;
  }

  void end_offscreen() {
    draw_target_ = nullptr;
    spare_use_ = Spare::Offscreen;
  }

  bool has_offscreen() const {
    return spare_use_ == Spare::Offscreen;
  }

  // Swap the offscreen frame into the draw buffer, ready for refresh().
  bool commit_offscreen() {
    if (spare_use_ != Spare::Offscreen)
      return false;
    std::swap(buf_, spare_);
    spare_use_ = Spare::None;
    return true;
  }

  // Block until the panel has finished any outstanding refresh. Call before SD
  // access: the card shares SPI2 with the display and concurrent traffic corrupts
  // an update in flight.
  void wait_panel_idle() {
    display_.wait_idle();
  }

  // Put the display into deep sleep (low-power mode). Call after a full refresh.
  void deep_sleep() {
    display_.deep_sleep();
  }


  // Point at the MGR2 sleep image. On device this is the mmapped `sleep`
  // partition (see font_partition.h) — no copy, no decompression, no file I/O
  // at sleep time, which matters because the SD card shares SPI2 with the panel.
  void set_sleep_image(const uint8_t* data, size_t size) {
    sleep_img_ = data;
    sleep_img_size_ = size;
  }

  // Show the sleep image (MGR2, thresholded to 1 bit). Leaves the panel awake;
  // the caller sends deep_sleep() once it is done with the card.
  bool show_sleep_image_embedded();

 private:
  IDisplay& display_;
  // **Heap, not BSS, and this is what makes the BLE controller fit.** As
  // member arrays the framebuffers were 96 KB of .bss, and .bss lands in the one ~50 KB
  // contiguous DRAM pool the BT controller must allocate from (the boot log's
  // "At 3FCB3490 len 0000CB70 (50 KiB): RAM" — the other ~123 KB is retention
  // RAM, which .bss does not reach). The controller therefore had nowhere to
  // go and reported "esp_bt_controller_init -4" / ESP_ERR_NO_MEM.
  //
  // On the heap the allocator is free to place them in the retention pool and
  // leave the scarce region for the radio. Allocated once at construction and
  // never freed.
  uint8_t* buf_ = static_cast<uint8_t*>(std::malloc(kBufSize));
  // Heap for the same reason, and likewise kept for the whole boot. Snapshot
  // and offscreen commit swap it with buf_, so neither pointer is tied to one
  // allocation.
  uint8_t* spare_ = static_cast<uint8_t*>(std::malloc(kBufSize));
  Spare spare_use_ = Spare::None;
  bool spare_lent_ = false;  // see lend_spare()
  uint32_t spare_cache_gen_ = 0;  // see claim_spare_cache()
  const void* spare_owner_ = nullptr;
  // Non-null while begin_offscreen() is in effect; see draw_().
  uint8_t* draw_target_ = nullptr;
  Rotation rotation_ = Rotation::Deg90;
  bool sunlight_fading_fix_ = true;

  // MGR2 sleep image, pointing into memory-mapped flash (set_sleep_image).
  const uint8_t* sleep_img_ = nullptr;
  size_t sleep_img_size_ = 0;

  // Where draw calls land: the draw buffer normally, the spare while an
  // offscreen pass is in effect. Commit paths (refresh, full_refresh,
  // restore_snapshot) always use buf_ directly.
  uint8_t* draw_() {
    return draw_target_ ? draw_target_ : buf_;
  }

  // Fill a physical horizontal span [x1, x2) on physical row `row`. Physical x
  // is app space: buffer column = x + kPanelOffsetX.
  static void fill_row_physical_(uint8_t* buf, int row, int x1, int x2, bool white) {
    const int lx1 = x1 + DisplayFrame::kPanelOffsetX;
    const int lx2 = x2 + DisplayFrame::kPanelOffsetX;
    const int bx1 = lx1 / 8;
    const int bx2 = (lx2 - 1) / 8;
    const auto lmask = static_cast<uint8_t>(0xFF >> (lx1 & 7));
    const auto rmask = static_cast<uint8_t>(0xFF << (7 - ((lx2 - 1) & 7)));
    uint8_t* rp = buf + row * DisplayFrame::kStride;
    if (bx1 == bx2) {
      const auto m = static_cast<uint8_t>(lmask & rmask);
      if (white)
        rp[bx1] |= m;
      else
        rp[bx1] &= static_cast<uint8_t>(~m);
    } else {
      if (white)
        rp[bx1] |= lmask;
      else
        rp[bx1] &= static_cast<uint8_t>(~lmask);
      if (bx2 > bx1 + 1)
        memset(rp + bx1 + 1, white ? 0xFF : 0x00, bx2 - bx1 - 1);
      if (white)
        rp[bx2] |= rmask;
      else
        rp[bx2] &= static_cast<uint8_t>(~rmask);
    }
  }

  // Fill a physical rectangle, clipped to the panel.
  static void fill_rect_physical_(uint8_t* buf, int rx, int ry, int rw, int rh, bool white) {
    const int x1 = std::max(rx, 0);
    const int y1 = std::max(ry, 0);
    const int x2 = std::min(rx + rw, DisplayFrame::kPhysicalWidth);
    const int y2 = std::min(ry + rh, DisplayFrame::kPhysicalHeight);
    if (x1 >= x2 || y1 >= y2)
      return;
    for (int row = y1; row < y2; ++row)
      fill_row_physical_(buf, row, x1, x2, white);
  }
};

}  // namespace wintergreen
