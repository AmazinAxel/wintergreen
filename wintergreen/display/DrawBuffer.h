#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "../content/BitmapFont.h"
#include "../content/TextLayout.h"

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

// Double-buffered display with simple draw helpers.
//
// Uses Deg90 (portrait) rotation: logical 480x793, physical 793x480.
// The "inactive" buffer is drawn to; "active" is what's currently displayed.
// refresh() swaps and does a partial hardware refresh.
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
    memset(bufs_[0], 0xFF, kBufSize);
    memset(bufs_[1], 0xFF, kBufSize);
  }

  ~DrawBuffer() {
    std::free(bufs_[0]);
    std::free(bufs_[1]);
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

  // Fill the entire inactive buffer.
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

  // Render all words of a laid-out line, each in its own size and style.
  void draw_layout_line(int x_offset, int baseline_y, const PageLine& line, const BitmapFontSet& fonts, bool white);

  void set_sunlight_fading_fix(bool v) { sunlight_fading_fix_ = v; }

  // -- Display operations
  // --------------------------------------------------

  // Swap active<->inactive and push the new frame to the panel.
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
    display_.partial_refresh(inactive_());
    active_idx_ = 1 - active_idx_;
  }

  // Call full hardware refresh using the current inactive buffer, then sync both.
  // The active buffer is what is on the glass, which RefreshMode::Full needs as
  // its starting state.
  void full_refresh(RefreshMode mode = RefreshMode::Half, bool turnOffScreen = false) {
    display_.full_refresh(inactive_(), active_(), mode, turnOffScreen || sunlight_fading_fix_);
    memcpy(bufs_[active_idx_], bufs_[1 - active_idx_], kBufSize);
    active_idx_ = 1 - active_idx_;
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

  // Hand the spare framebuffer back to the heap, and take it again afterwards.
  //
  // This exists for one caller: the Wi-Fi sync. Two static framebuffers are
  // 96 KB of BSS and the spare is another 48 KB of heap, which on a 320 KB part
  // left ~20 KB free — and esp_wifi_init() needs roughly 50 KB, so it failed
  // outright with no clue why (see docs/sync.md).
  //
  // Safe because the spare is **purely an optimisation**: without it a quick
  // menu dismiss re-renders and a page turn re-draws, which is what happened
  // before it existed. Both claim paths already null-check it.
  //
  // spare_use_ must be reset here: has_snapshot()/has_offscreen() test only that
  // enum, so leaving it set would let restore_snapshot() memcpy from a freed
  // pointer.
  void release_spare() {
    std::free(spare_);
    spare_ = nullptr;
    spare_use_ = Spare::None;
    // draw_() resolves through draw_target_; if an offscreen draw was in
    // progress the target must go back to a real buffer.
    draw_target_ = nullptr;
  }

  // Best-effort: staying without it costs latency, not correctness.
  void reacquire_spare() {
    if (!spare_)
      spare_ = static_cast<uint8_t*>(std::malloc(kBufSize));
  }

  // Lend the spare as a plain scratch buffer, for a caller that needs kBufSize
  // bytes and is not drawing. Only used by the sleep path, which reads the whole
  // cover file in one fread instead of one per row: 793 rows is 793 FATFS + SPI
  // round trips on a 20 MHz card, and the spare is idle at sleep time.
  //
  // Whatever the spare held is destroyed, hence the invalidate — the snapshot and
  // the pre-drawn page are both meaningless once the device is going to sleep.
  // Returns null when the spare has been released (Wi-Fi sync); the caller must
  // then fall back to reading row by row.
  uint8_t* borrow_spare_scratch() {
    spare_use_ = Spare::None;
    return spare_;
  }

  // Lend the spare as a cache that may outlive the screen using it: the book
  // list keeps its cover thumbnails here. Nothing draws into it while the list
  // is up, so the reader's two uses never overlap it. Like them it is
  // invalidated by any other claim — a snapshot, a pre-draw, the sleep path, a
  // release.
  //
  // Validity is a generation, not just the enum: each time the spare *enters*
  // cache use its contents are garbage and the generation moves, so a cache that
  // was displaced and re-claimed cannot mistake its old slots for live ones. The
  // owner trusts what it wrote only while
  // spare_cache_valid(the generation it wrote under). Null when released.
  //
  // `owner` distinguishes the two caches that use it — the book list's
  // thumbnails and the home screen's cover — so one claiming it moves the
  // generation and invalidates the other, rather than both trusting slots the
  // other has overwritten.
  uint8_t* claim_spare_cache(const void* owner) {
    if (!spare_) {
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

  void save_snapshot() {
    if (!spare_)
      return;
    memcpy(spare_, active_(), kBufSize);
    spare_use_ = Spare::Snapshot;
  }

  bool has_snapshot() const {
    return spare_use_ == Spare::Snapshot;
  }

  // Copy the snapshot into the inactive buffer, ready for the caller's refresh().
  bool restore_snapshot() {
    if (spare_use_ != Spare::Snapshot)
      return false;
    memcpy(inactive_(), spare_, kBufSize);
    return true;
  }

  // Redirect every draw call into the spare buffer until end_offscreen(). The
  // displayed frame and the inactive buffer are both untouched meanwhile.
  // Returns false when there is no spare; the caller must then skip the
  // pre-draw entirely rather than drawing into the live buffer.
  bool begin_offscreen() {
    if (!spare_)
      return false;
    draw_target_ = spare_;
    spare_use_ = Spare::None;  // not usable until end_offscreen says so
    return true;
  }

  // Whether begin_offscreen() would succeed, without the side effects. Lets a
  // caller skip speculative work it could not use — the spare is absent
  // whenever a radio holds the heap.
  bool can_offscreen() const {
    return spare_ != nullptr;
  }

  void end_offscreen() {
    draw_target_ = nullptr;
    spare_use_ = Spare::Offscreen;
  }

  bool has_offscreen() const {
    return spare_use_ == Spare::Offscreen;
  }

  // Move the offscreen frame into the inactive buffer, ready for refresh().
  bool commit_offscreen() {
    if (spare_use_ != Spare::Offscreen)
      return false;
    memcpy(inactive_(), spare_, kBufSize);
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
  // **Heap, not BSS, and this is what makes the BLE controller fit.** As a
  // member array these were 96 KB of .bss, and .bss lands in the one ~50 KB
  // contiguous DRAM pool the BT controller must allocate from (the boot log's
  // "At 3FCB3490 len 0000CB70 (50 KiB): RAM" — the other ~123 KB is retention
  // RAM, which .bss does not reach). The controller therefore had nowhere to
  // go and reported "esp_bt_controller_init -4" / ESP_ERR_NO_MEM.
  //
  // On the heap the allocator is free to place them in the retention pool and
  // leave the scarce region for the radio. Nothing else changes: they are
  // allocated once at construction and never freed.
  uint8_t* bufs_[2] = {static_cast<uint8_t*>(std::malloc(kBufSize)),
                       static_cast<uint8_t*>(std::malloc(kBufSize))};
  // Heap for the same reason, but also released outright while a radio is up —
  // see release_spare().
  uint8_t* spare_ = static_cast<uint8_t*>(std::malloc(kBufSize));
  Spare spare_use_ = Spare::None;
  uint32_t spare_cache_gen_ = 0;  // see claim_spare_cache()
  const void* spare_owner_ = nullptr;
  // Non-null while begin_offscreen() is in effect; see draw_().
  uint8_t* draw_target_ = nullptr;
  int active_idx_ = 0;
  Rotation rotation_ = Rotation::Deg90;
  bool sunlight_fading_fix_ = true;

  // MGR2 sleep image, pointing into memory-mapped flash (set_sleep_image).
  const uint8_t* sleep_img_ = nullptr;
  size_t sleep_img_size_ = 0;

  // Where draw calls land: the inactive buffer normally, the spare while an
  // offscreen pass is in effect. Commit paths (refresh, full_refresh,
  // restore_snapshot) always use inactive_() directly.
  uint8_t* draw_() {
    return draw_target_ ? draw_target_ : inactive_();
  }

  uint8_t* inactive_() {
    return bufs_[1 - active_idx_];
  }
  const uint8_t* active_() const {
    return bufs_[active_idx_];
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
