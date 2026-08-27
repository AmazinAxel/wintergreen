#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "../content/BitmapFont.h"
#include "../content/TextLayout.h"
#include "ui_font_small.h"


namespace wintergreen {

enum class Rotation { Deg0 = 0, Deg90 = 90, Deg180 = 180, Deg270 = 270 };

// Refresh mode for full-screen updates.
enum class RefreshMode { Full, Half };

// Which bitmap plane to read from a GlyphData during grayscale rendering.
enum class GrayPlane { BW, LSB, MSB };

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
// the desktop build is gone — see the note in CLAUDE.md about collapsing it.
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
// Uses Deg90 (portrait) rotation: logical 480x788, physical 788x480.
// The "inactive" buffer is drawn to; "active" is what's currently displayed.
// refresh() swaps and does a partial hardware refresh.
//
// The framebuffers are never loaned out as scratch: image decoding and EPUB
// conversion, the two things that used to borrow them, are both gone from the
// firmware.
class DrawBuffer {
 public:
  // Logical portrait dimensions.
  static constexpr int kWidth = DisplayFrame::kPhysicalHeight;
  static constexpr int kHeight = DisplayFrame::kPhysicalWidth;
  static constexpr size_t kBufSize = DisplayFrame::kPixelBytes;

  explicit DrawBuffer(IDisplay& display) : display_(display) {
    memset(bufs_[0], 0xFF, kBufSize);
    memset(bufs_[1], 0xFF, kBufSize);
    set_rotation(Rotation::Deg90);
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
    return (rotation_ == Rotation::Deg0 || rotation_ == Rotation::Deg180) ? DisplayFrame::kPhysicalWidth : kWidth;
  }
  int height() const {
    return (rotation_ == Rotation::Deg0 || rotation_ == Rotation::Deg180) ? DisplayFrame::kPhysicalHeight : kHeight;
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
      fill_rect_physical_(full_target_(), lx, ly, lw, lh, white);
    else if (rotation_ == Rotation::Deg90)
      fill_rect_physical_(full_target_(), ly, DisplayFrame::kPhysicalHeight - lx - lw, lh, lw, white);
    else if (rotation_ == Rotation::Deg180)
      fill_rect_physical_(full_target_(), DisplayFrame::kPhysicalWidth - lx - lw, DisplayFrame::kPhysicalHeight - ly - lh, lw, lh, white);
    else  // Deg270
      fill_rect_physical_(full_target_(), DisplayFrame::kPhysicalWidth - ly - lh, lx, lh, lw, white);
  }

  // Fill a logical horizontal span [x1, x2) on logical row ly.
  void fill_row(int ly, int x1, int x2, bool white) {
    if (rotation_ == Rotation::Deg0) {
      x1 = std::max(x1, 0);
      x2 = std::min(x2, DisplayFrame::kPhysicalWidth);
      if (x1 >= x2 || ly < 0 || ly >= DisplayFrame::kPhysicalHeight)
        return;
      fill_row_physical_(full_target_(), ly, x1, x2, white);
    } else if (rotation_ == Rotation::Deg90) {
      x1 = std::max(x1, 0);
      x2 = std::min(x2, kWidth);
      if (x1 >= x2 || ly < 0 || ly >= kHeight)
        return;
      fill_col_physical_(full_target_(), ly, DisplayFrame::kPhysicalHeight - x2, DisplayFrame::kPhysicalHeight - x1,
                         white);
    } else if (rotation_ == Rotation::Deg180) {
      x1 = std::max(x1, 0);
      x2 = std::min(x2, DisplayFrame::kPhysicalWidth);
      if (x1 >= x2 || ly < 0 || ly >= DisplayFrame::kPhysicalHeight)
        return;
      fill_row_physical_(full_target_(), DisplayFrame::kPhysicalHeight - 1 - ly,
                         DisplayFrame::kPhysicalWidth - x2, DisplayFrame::kPhysicalWidth - x1, white);
    } else {  // Deg270
      x1 = std::max(x1, 0);
      x2 = std::min(x2, kWidth);
      if (x1 >= x2 || ly < 0 || ly >= kHeight)
        return;
      fill_col_physical_(full_target_(), DisplayFrame::kPhysicalWidth - 1 - ly, x1, x2, white);
    }
  }

  // Blit a 1-bit packed image into the inactive buffer at physical position (x, y).
  // Coordinates are physical (not logical). Clips to display bounds.
  void draw_image(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!imageData || w == 0 || h == 0)
      return;

    if (x >= DisplayFrame::kPhysicalWidth || y >= DisplayFrame::kPhysicalHeight)
      return;

    const uint16_t imageWidthBytes = static_cast<uint16_t>((w + 7) / 8);
    const uint16_t max_width = static_cast<uint16_t>(DisplayFrame::kPhysicalWidth - x);
    const uint16_t draw_width = std::min<uint16_t>(w, max_width);
    const uint16_t draw_bytes = static_cast<uint16_t>((draw_width + 7) / 8);
    uint8_t* buf = draw_();

    // Add panel offset so app-space x=0 maps to buffer column 12.
    const uint16_t x_buf = static_cast<uint16_t>(x + DisplayFrame::kPanelOffsetX);
    const uint16_t dest_offset_x = static_cast<uint16_t>(x_buf / 8);
    const uint8_t bit_offset = static_cast<uint8_t>(x_buf & 7);

    auto set_pixel_physical = [&](uint16_t px, uint16_t py, bool white) {
      if (px >= DisplayFrame::kPhysicalWidth || py >= DisplayFrame::kPhysicalHeight)
        return;
      const uint16_t px_buf = static_cast<uint16_t>(px + DisplayFrame::kPanelOffsetX);
      size_t idx = static_cast<size_t>(py) * DisplayFrame::kStride + (px_buf / 8);
      uint8_t bit = static_cast<uint8_t>(0x80u >> (px_buf & 7));
      if (white)
        buf[idx] |= bit;
      else
        buf[idx] &= static_cast<uint8_t>(~bit);
    };

    for (uint16_t row = 0; row < h; ++row) {
      uint16_t destY = y + row;
      if (destY >= DisplayFrame::kPhysicalHeight)
        break;

      const size_t destRowStart = static_cast<size_t>(destY) * DisplayFrame::kStride + dest_offset_x;
      const size_t srcRowStart = static_cast<size_t>(row) * imageWidthBytes;

      if (bit_offset == 0 && (w & 7) == 0) {
        const uint16_t copy_bytes = std::min<uint16_t>(imageWidthBytes, draw_bytes);
        memcpy(buf + destRowStart, imageData + srcRowStart, copy_bytes);
      } else {
        for (uint16_t col = 0; col < draw_width; ++col) {
          const size_t src_byte = srcRowStart + (col / 8);
          const uint8_t src_bit = static_cast<uint8_t>((imageData[src_byte] >> (7 - (col & 7))) & 1);
          set_pixel_physical(static_cast<uint16_t>(x + col), destY, src_bit != 0);
        }
      }
    }
  }

  // Set a single logical pixel.
  void set_pixel(int lx, int ly, bool white) {
    int px, py;
    if (rotation_ == Rotation::Deg0) {
      if (lx < 0 || lx >= DisplayFrame::kPhysicalWidth || ly < 0 || ly >= DisplayFrame::kPhysicalHeight)
        return;
      px = lx;
      py = ly;
    } else if (rotation_ == Rotation::Deg90) {
      if (lx < 0 || lx >= kWidth || ly < 0 || ly >= kHeight)
        return;
      px = ly;
      py = DisplayFrame::kPhysicalHeight - 1 - lx;
    } else if (rotation_ == Rotation::Deg180) {
      if (lx < 0 || lx >= DisplayFrame::kPhysicalWidth || ly < 0 || ly >= DisplayFrame::kPhysicalHeight)
        return;
      px = DisplayFrame::kPhysicalWidth - 1 - lx;
      py = DisplayFrame::kPhysicalHeight - 1 - ly;
    } else {  // Deg270
      if (lx < 0 || lx >= kWidth || ly < 0 || ly >= kHeight)
        return;
      px = DisplayFrame::kPhysicalWidth - 1 - ly;
      py = lx;
    }
    uint8_t* buf = draw_();
    const int px_buf = px + DisplayFrame::kPanelOffsetX;
    const size_t bidx = static_cast<size_t>(py * DisplayFrame::kStride + px_buf / 8);
    const uint8_t bit = static_cast<uint8_t>(0x80u >> (px_buf & 7));
    if (white)
      buf[bidx] |= bit;
    else
      buf[bidx] &= static_cast<uint8_t>(~bit);
  }

  // Blit a horizontal row of 1-bit packed pixels at logical position (lx, ly).
  // data_1bit is MSB-first packed, width pixels long.
  void blit_1bit_row(int lx, int ly, const uint8_t* data_1bit, int width) {
    uint8_t* buf = draw_();
    if (rotation_ == Rotation::Deg0) {
      if (ly < 0 || ly >= DisplayFrame::kPhysicalHeight || width <= 0)
        return;
      int col_start = 0, col_end = width;
      if (lx < 0)
        col_start = -lx;
      if (lx + width > DisplayFrame::kPhysicalWidth)
        col_end = DisplayFrame::kPhysicalWidth - lx;
      for (int col = col_start; col < col_end; ++col) {
        const int px = lx + col;
        const int px_buf = px + DisplayFrame::kPanelOffsetX;
        const size_t bidx = static_cast<size_t>(ly) * DisplayFrame::kStride + px_buf / 8;
        const uint8_t set_mask = static_cast<uint8_t>(0x80u >> (px_buf & 7));
        const uint8_t clr_mask = static_cast<uint8_t>(~set_mask);
        const bool white = (data_1bit[col >> 3] >> (7 - (col & 7))) & 1;
        if (white)
          buf[bidx] |= set_mask;
        else
          buf[bidx] &= clr_mask;
      }
    } else if (rotation_ == Rotation::Deg90) {
      if (ly < 0 || ly >= kHeight || width <= 0)
        return;
      const int px = ly;
      const int px_buf = px + DisplayFrame::kPanelOffsetX;
      const int byte_col = px_buf / 8;
      const uint8_t set_mask = static_cast<uint8_t>(0x80u >> (px_buf & 7));
      const uint8_t clr_mask = static_cast<uint8_t>(~set_mask);
      int col_start = 0, col_end = width;
      if (lx < 0)
        col_start = -lx;
      if (lx + width > kWidth)
        col_end = kWidth - lx;
      for (int col = col_start; col < col_end; ++col) {
        const int py = DisplayFrame::kPhysicalHeight - 1 - (lx + col);
        const size_t bidx = static_cast<size_t>(py) * DisplayFrame::kStride + byte_col;
        const bool white = (data_1bit[col >> 3] >> (7 - (col & 7))) & 1;
        if (white)
          buf[bidx] |= set_mask;
        else
          buf[bidx] &= clr_mask;
      }
    } else if (rotation_ == Rotation::Deg180) {
      if (ly < 0 || ly >= DisplayFrame::kPhysicalHeight || width <= 0)
        return;
      const int py = DisplayFrame::kPhysicalHeight - 1 - ly;
      int col_start = 0, col_end = width;
      if (lx < 0)
        col_start = -lx;
      if (lx + width > DisplayFrame::kPhysicalWidth)
        col_end = DisplayFrame::kPhysicalWidth - lx;
      for (int col = col_start; col < col_end; ++col) {
        const int px = DisplayFrame::kPhysicalWidth - 1 - (lx + col);
        const int px_buf = px + DisplayFrame::kPanelOffsetX;
        const size_t bidx = static_cast<size_t>(py) * DisplayFrame::kStride + px_buf / 8;
        const uint8_t set_mask = static_cast<uint8_t>(0x80u >> (px_buf & 7));
        const uint8_t clr_mask = static_cast<uint8_t>(~set_mask);
        const bool white = (data_1bit[col >> 3] >> (7 - (col & 7))) & 1;
        if (white)
          buf[bidx] |= set_mask;
        else
          buf[bidx] &= clr_mask;
      }
    } else {  // Deg270
      if (ly < 0 || ly >= kHeight || width <= 0)
        return;
      const int px = DisplayFrame::kPhysicalWidth - 1 - ly;
      const int px_buf = px + DisplayFrame::kPanelOffsetX;
      const int byte_col = px_buf / 8;
      const uint8_t set_mask = static_cast<uint8_t>(0x80u >> (px_buf & 7));
      const uint8_t clr_mask = static_cast<uint8_t>(~set_mask);
      int col_start = 0, col_end = width;
      if (lx < 0)
        col_start = -lx;
      if (lx + width > kWidth)
        col_end = kWidth - lx;
      for (int col = col_start; col < col_end; ++col) {
        const int py = lx + col;
        const size_t bidx = static_cast<size_t>(py) * DisplayFrame::kStride + byte_col;
        const bool white = (data_1bit[col >> 3] >> (7 - (col & 7))) & 1;
        if (white)
          buf[bidx] |= set_mask;
        else
          buf[bidx] &= clr_mask;
      }
    }
  }

  // -- Text & Glyph rendering --

  // Draw text with the UI font, including background fill.
  // white=true  -> white background, black glyphs (normal unselected item).
  // white=false -> black background, white glyphs (highlighted selected item).
  // The scale parameter is accepted for API compatibility but ignored.
  void draw_text(int x, int y, const char* text, bool white, int /*scale*/ = 1) {
    if (!text || !*text)
      return;
    const BitmapFont& f = ui_font_();
    const int w = static_cast<int>(f.word_width(text, strlen(text), FontStyle::Regular));
    const int h = static_cast<int>(f.glyph_height());
    fill_rect(x, y, w, h, white);
    draw_text_proportional(x, y + static_cast<int>(f.baseline()), text, f, !white);
  }

  void draw_text_no_bg(int x, int y, const char* text, bool white, int /*scale*/ = 1) {
    if (!text || !*text)
      return;
    const BitmapFont& f = ui_font_();
    draw_text_proportional(x, y + static_cast<int>(f.baseline()), text, f, white);
  }

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

  // -- Plane-aware text rendering (for grayscale two-pass) -----------------

  // Render text from a specific grayscale plane into an explicit buffer.
  int draw_text_plane(uint8_t* buf, int x, int baseline_y, const char* text, size_t len, const BitmapFontSet& fonts,
                      GrayPlane plane, bool white, FontStyle style = FontStyle::Regular, uint8_t size_pct = 100);

  // Render all words of a LayoutLine and — on the BW plane only — draw continuous
  // underline decorations spanning each run of consecutive linked words. This is the
  // correct level of abstraction: underlines are line decorations, not per-word.
  void draw_layout_line(uint8_t* buf, int x_offset, int baseline_y, const PageLine& line, const BitmapFontSet& fonts,
                        GrayPlane plane, bool white);

  void set_sunlight_fading_fix(bool v) { sunlight_fading_fix_ = v; }
  bool sunlight_fading_fix() const { return sunlight_fading_fix_; }

  // Provide direct access to the inactive buffer for multi-pass rendering.
  uint8_t* render_buf() {
    return draw_();
  }

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
  enum class Spare : uint8_t { None, Snapshot, Offscreen };

  // Hand the spare framebuffer back to the heap, and take it again afterwards.
  //
  // This exists for one caller: the Wi-Fi sync. Two static framebuffers are
  // 96 KB of BSS and the spare is another 48 KB of heap, which on a 320 KB part
  // left ~20 KB free — and esp_wifi_init() needs roughly 50 KB, so it failed
  // outright with no clue why (see "NAS book sync" in CLAUDE.md).
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

  // Show the sleep image (2bpp, 4 gray levels).
  // state = (RED_bit << 1) | BW_bit; 0=black, 1=dark gray, 2=light gray, 3=white.
  bool show_sleep_image_embedded() {
    if (!sleep_img_)
      return false;
    Mgr2Source_ src = Mgr2Source_::from_memory(sleep_img_, sleep_img_size_);
    if (!src.valid())
      return false;
    show_mgr2_sleep_(src);
    return true;
  }

 private:
  // Describes a render target: a pixel buffer with its own stride and physical offset/bounds.
  // phys_x0 is used for clipping (app-space coordinates).
  // buf_x0  is the app-space X that maps to buffer column 0 (for byte-index: col = x - buf_x0).
  //   Full-frame buffer: buf_x0 = -kPanelOffsetX  (app X=0 → buffer column 12)
  //   Mini loading-box:  buf_x0 = phys_x0         (local buffer, no panel offset)
  struct RenderTarget {
    uint8_t* buf;
    int stride;   // bytes per physical row
    int phys_x0;  // absolute physical X of the left edge for clipping (app space)
    int phys_y0;  // absolute physical Y of the top edge
    int phys_w;   // width in pixels for clipping
    int phys_h;   // height in rows
    int buf_x0;   // app-space X that maps to buffer column 0
  };

  static void draw_glyph_impl_(const RenderTarget& t, int x, int y, const uint8_t* bits, int bitmap_width,
                               int bitmap_height, int x_offset, int y_offset, bool white, bool invert_select = false,
                               Rotation rotation = Rotation::Deg90) {
    if (!bits || bitmap_width <= 0 || bitmap_height <= 0)
      return;
    const int gx = x + x_offset;
    const int gy = y + y_offset;
    const int row_stride = (bitmap_width + 7) / 8;

    if (rotation == Rotation::Deg90) {
      // Optimized transposed path for portrait (Deg90) — the common case.
      //
      // Physical mapping: px = gy + row,  py = kPhysicalHeight-1 - gx - col
      //
      // Transposed loop: outer=col (selects one physical row → sequential memory),
      // inner=row (writes adjacent bits within that row).
      // Batched inner loop packs 8 rows into one output byte for 8× fewer
      // read-modify-write ops over the cache-friendly forward scan.
      const int lpy_base = DisplayFrame::kPhysicalHeight - 1 - gx - t.phys_y0;
      // col range where py_local = lpy_base - col is in [0, phys_h):
      const int col_start = (lpy_base >= t.phys_h) ? lpy_base - (t.phys_h - 1) : 0;
      const int col_end = (lpy_base >= 0) ? (lpy_base < bitmap_width ? lpy_base + 1 : bitmap_width) : 0;
      if (col_start >= col_end)
        return;
      // row range where px_local = lpx0 + row is in [0, phys_w):
      const int lpx0_clip = gy - t.phys_x0;
      const int lpx0 = gy - t.buf_x0;
      const int row_lo = (lpx0_clip < 0) ? -lpx0_clip : 0;
      const int row_hi = (lpx0_clip + bitmap_height > t.phys_w) ? t.phys_w - lpx0_clip : bitmap_height;
      if (row_lo >= row_hi)
        return;

      for (int col = col_start; col < col_end; ++col) {
        uint8_t* const row_ptr = t.buf + static_cast<size_t>(lpy_base - col) * static_cast<size_t>(t.stride);
        const int src_byte_off = col >> 3;
        const int src_shift = 7 - (col & 7);

        int row = row_lo;

        // Scalar prefix: advance until (lpx0 + row) is on a byte boundary.
        const int align_end_raw = row_lo + ((8 - ((lpx0 + row_lo) & 7)) & 7);
        const int prefix_end = align_end_raw < row_hi ? align_end_raw : row_hi;
        for (; row < prefix_end; ++row) {
          const uint8_t glyph_bit = static_cast<uint8_t>((bits[row * row_stride + src_byte_off] >> src_shift) & 1u);
          if (invert_select ? glyph_bit : !glyph_bit) {
            const int px_local = lpx0 + row;
            const uint8_t mask = static_cast<uint8_t>(0x80u >> (px_local & 7));
            if (white)
              row_ptr[px_local >> 3] |= mask;
            else
              row_ptr[px_local >> 3] &= static_cast<uint8_t>(~mask);
          }
        }

        // Batched inner loop: 8 source rows → 1 output byte (byte-aligned in framebuffer).
        for (; row + 8 <= row_hi; row += 8) {
          const int out_idx = (lpx0 + row) >> 3;  // byte-aligned: (lpx0+row) & 7 == 0
          const uint8_t* src = bits + row * row_stride + src_byte_off;
          // Pack column bits from 8 successive rows into one byte (MSB = row+0).
          uint8_t col_byte;
          col_byte = static_cast<uint8_t>((*src >> src_shift) & 1u) << 7;
          src += row_stride;
          col_byte |= static_cast<uint8_t>((*src >> src_shift) & 1u) << 6;
          src += row_stride;
          col_byte |= static_cast<uint8_t>((*src >> src_shift) & 1u) << 5;
          src += row_stride;
          col_byte |= static_cast<uint8_t>((*src >> src_shift) & 1u) << 4;
          src += row_stride;
          col_byte |= static_cast<uint8_t>((*src >> src_shift) & 1u) << 3;
          src += row_stride;
          col_byte |= static_cast<uint8_t>((*src >> src_shift) & 1u) << 2;
          src += row_stride;
          col_byte |= static_cast<uint8_t>((*src >> src_shift) & 1u) << 1;
          src += row_stride;
          col_byte |= static_cast<uint8_t>((*src >> src_shift) & 1u);
          // col_byte bit (7-b) = glyph source bit for row+b. 1=background, 0=ink (normal).
          const uint8_t ink = invert_select ? col_byte : static_cast<uint8_t>(~col_byte);
          if (white)
            row_ptr[out_idx] |= ink;
          else
            row_ptr[out_idx] &= static_cast<uint8_t>(~ink);
        }

        // Scalar tail.
        for (; row < row_hi; ++row) {
          const uint8_t glyph_bit = static_cast<uint8_t>((bits[row * row_stride + src_byte_off] >> src_shift) & 1u);
          if (invert_select ? glyph_bit : !glyph_bit) {
            const int px_local = lpx0 + row;
            const uint8_t mask = static_cast<uint8_t>(0x80u >> (px_local & 7));
            if (white)
              row_ptr[px_local >> 3] |= mask;
            else
              row_ptr[px_local >> 3] &= static_cast<uint8_t>(~mask);
          }
        }
      }
    } else if (rotation == Rotation::Deg0) {
      // Deg0 (landscape) — general path.
      for (int row = 0; row < bitmap_height; ++row) {
        const int ly = gy + row;
        const uint8_t* row_data = bits + row * row_stride;
        for (int col = 0; col < bitmap_width; ++col) {
          const int lx = gx + col;
          const bool bit_set = (row_data[col >> 3] >> (7 - (col & 7))) & 1;
          if (invert_select ? bit_set : !bit_set) {
            const int px = lx;
            const int py = ly;
            if (px < t.phys_x0 || px >= t.phys_x0 + t.phys_w)
              continue;
            if (py < t.phys_y0 || py >= t.phys_y0 + t.phys_h)
              continue;
            const int lpx = px - t.buf_x0;
            const int lpy = py - t.phys_y0;
            const size_t bidx = static_cast<size_t>(lpy * t.stride + lpx / 8);
            const uint8_t bit = static_cast<uint8_t>(0x80u >> (lpx & 7));
            if (white)
              t.buf[bidx] |= bit;
            else
              t.buf[bidx] &= static_cast<uint8_t>(~bit);
          }
        }
      }
    } else if (rotation == Rotation::Deg180) {
      // Deg180: px = PhysW-1-lx, py = PhysH-1-ly
      for (int row = 0; row < bitmap_height; ++row) {
        const int ly = gy + row;
        const uint8_t* row_data = bits + row * row_stride;
        for (int col = 0; col < bitmap_width; ++col) {
          const int lx = gx + col;
          const bool bit_set = (row_data[col >> 3] >> (7 - (col & 7))) & 1;
          if (invert_select ? bit_set : !bit_set) {
            const int px = DisplayFrame::kPhysicalWidth - 1 - lx;
            const int py = DisplayFrame::kPhysicalHeight - 1 - ly;
            if (px < t.phys_x0 || px >= t.phys_x0 + t.phys_w)
              continue;
            if (py < t.phys_y0 || py >= t.phys_y0 + t.phys_h)
              continue;
            const int lpx = px - t.buf_x0;
            const int lpy = py - t.phys_y0;
            const size_t bidx = static_cast<size_t>(lpy * t.stride + lpx / 8);
            const uint8_t bit = static_cast<uint8_t>(0x80u >> (lpx & 7));
            if (white)
              t.buf[bidx] |= bit;
            else
              t.buf[bidx] &= static_cast<uint8_t>(~bit);
          }
        }
      }
    } else {
      // Deg270: px = PhysW-1-ly, py = lx
      for (int row = 0; row < bitmap_height; ++row) {
        const int ly = gy + row;
        const uint8_t* row_data = bits + row * row_stride;
        for (int col = 0; col < bitmap_width; ++col) {
          const int lx = gx + col;
          const bool bit_set = (row_data[col >> 3] >> (7 - (col & 7))) & 1;
          if (invert_select ? bit_set : !bit_set) {
            const int px = DisplayFrame::kPhysicalWidth - 1 - ly;
            const int py = lx;
            if (px < t.phys_x0 || px >= t.phys_x0 + t.phys_w)
              continue;
            if (py < t.phys_y0 || py >= t.phys_y0 + t.phys_h)
              continue;
            const int lpx = px - t.buf_x0;
            const int lpy = py - t.phys_y0;
            const size_t bidx = static_cast<size_t>(lpy * t.stride + lpx / 8);
            const uint8_t bit = static_cast<uint8_t>(0x80u >> (lpx & 7));
            if (white)
              t.buf[bidx] |= bit;
            else
              t.buf[bidx] &= static_cast<uint8_t>(~bit);
          }
        }
      }
    }
  }

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
  // write_ram_bw, restore_snapshot) always use inactive_() directly.
  uint8_t* draw_() {
    return draw_target_ ? draw_target_ : inactive_();
  }

  uint8_t* inactive_() {
    return bufs_[1 - active_idx_];
  }
  const uint8_t* active_() const {
    return bufs_[active_idx_];
  }

  // Returns the shared UI font backed by ui_font_small.h data.
  static const BitmapFont& ui_font_() {
    static BitmapFont font(kFontData_ui_small_wgf, sizeof(kFontData_ui_small_wgf));
    return font;
  }

  // Reads MGR2 rows straight out of memory-mapped flash. There is no file-backed
  // variant any more: the image is always the mmapped `sleep` partition, so a row
  // fetch is a pointer add rather than an fseek+fread.
  struct Mgr2Source_ {
    uint16_t w = 0, h = 0;
    size_t src_stride = 0;
    const uint8_t* mem_ = nullptr;

    bool valid() const {
      return mem_ && w > 0 && h > 0;
    }

    const uint8_t* get_row(uint16_t y) const {
      return mem_ + static_cast<size_t>(y) * src_stride;
    }

    static Mgr2Source_ from_memory(const uint8_t* data, size_t size) {
      Mgr2Source_ s;
      if (size < 8 || std::memcmp(data, "MGR2", 4) != 0)
        return s;
      s.w = data[4] | (data[5] << 8);
      s.h = data[6] | (data[7] << 8);
      s.src_stride = (static_cast<size_t>(s.w) + 3) / 4;
      if (size < 8 + s.src_stride * static_cast<size_t>(s.h))
        return s;
      s.mem_ = data + 8;
      return s;
    }
  };

  // Decode MGR2 into the inactive buffer and paint it with RefreshMode::Half.
  //
  // This used to build both RAM planes and fire grayscale_refresh_1pass. That
  // was dropped for two reasons, and the first is the reason it looked wrong:
  //
  // kLutFactoryQuality staggers its phases by level. Over G0's 24 frames LUT0
  // (black) is idle (VS 0x00) while LUT3 (white) oscillates (0xA8), so the
  // wordmark settled almost immediately and the background churned around it
  // for the bulk of the update — read on the glass as "wordmark first, then the
  // screen flashes". That ordering is baked into the LUT, not into anything
  // sequenced here, so the only fix is a different waveform.
  //
  // Half is the same waveform the sleep *cover* has always used
  // (Application.cpp's show_book_cover_sleep_), which does not have the symptom.
  //
  // Second, it costs nothing to give up: resources/sleep.mgr holds only levels
  // 0 and 3 — 360,074 white and 23,926 black pixels, no gray anywhere — so the
  // grayscale path had no intermediate tones to render. A 4-level MGR2 would now
  // quantise to black/white at the 50% point; if one is ever shipped, that is the
  // line to revisit.
  //
  // Half sends CTRL1_BYPASS_RED, so RED is ignored: no second plane, and hence
  // no need for the spare buffer or its without-the-spare fallback. Note the
  // polarity flip — the grayscale decode set a bit for *ink*, while ordinary
  // drawing (see fill()/blit_1bit_row) uses set = white, which is what
  // full_refresh consumes.
  void show_mgr2_sleep_(Mgr2Source_& src) {
    const int rows = std::min<int>(src.h, DisplayFrame::kPhysicalHeight);

    // Anything the image does not cover stays white, as the old fill(false)
    // + per-plane decode left it.
    memset(inactive_(), 0xFF, kBufSize);
    const int cols = std::min<int>(src.w, DisplayFrame::kPanelWidth);
    for (int y = 0; y < rows; ++y) {
      const uint8_t* src_row = src.get_row(static_cast<uint16_t>(y));
      uint8_t* dst = inactive_() + static_cast<size_t>(y) * DisplayFrame::kStride;
      for (int x = 0; x < cols; ++x) {
        // 0 = white, 1 = light, 2 = dark, 3 = black; threshold at the midpoint.
        const uint8_t v = (src_row[x / 4] >> (6 - (x % 4) * 2)) & 0x3;
        if (v >= 2)
          dst[x / 8] &= static_cast<uint8_t>(~(0x80u >> (x % 8)));
      }
    }

    // Blitted in physical panel coordinates with no offset, matching the format
    // note in CLAUDE.md: the image is native-size, not scaled or centred.
    display_.full_refresh(inactive_(), active_(), RefreshMode::Half,
                          /*turnOffScreen=*/true);
    display_.deep_sleep();
  }

  // Render target for the full inactive buffer.
  // buf_x0 = -kPanelOffsetX so that app-space X=0 maps to buffer column kPanelOffsetX.
  RenderTarget full_target_() {
    return {draw_(),
            DisplayFrame::kStride,
            0,
            0,
            DisplayFrame::kPhysicalWidth,
            DisplayFrame::kPhysicalHeight,
            -DisplayFrame::kPanelOffsetX};
  }

  // Fill a physical horizontal span [x1, x2) on physical row `row` (absolute physical coords).
  // phys_x0 must be byte-aligned so that local_x has the same bit position as absolute x.
  static void fill_row_physical_(const RenderTarget& t, int row, int x1, int x2, bool white) {
    x1 = std::max(x1, t.phys_x0);
    x2 = std::min(x2, t.phys_x0 + t.phys_w);
    if (x1 >= x2 || row < t.phys_y0 || row >= t.phys_y0 + t.phys_h)
      return;
    const int lrow = row - t.phys_y0;
    const int lx1 = x1 - t.buf_x0;
    const int lx2 = x2 - t.buf_x0;
    const int bx1 = lx1 / 8;
    const int bx2 = (lx2 - 1) / 8;
    const auto lmask = static_cast<uint8_t>(0xFF >> (lx1 & 7));
    const auto rmask = static_cast<uint8_t>(0xFF << (7 - ((lx2 - 1) & 7)));
    uint8_t* rp = t.buf + lrow * t.stride;
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

  // Fill a physical rectangle (absolute physical coords).
  static void fill_rect_physical_(const RenderTarget& t, int rx, int ry, int rw, int rh, bool white) {
    const int x1 = std::max(rx, t.phys_x0);
    const int y1 = std::max(ry, t.phys_y0);
    const int x2 = std::min(rx + rw, t.phys_x0 + t.phys_w);
    const int y2 = std::min(ry + rh, t.phys_y0 + t.phys_h);
    if (x1 >= x2 || y1 >= y2)
      return;
    for (int row = y1; row < y2; ++row)
      fill_row_physical_(t, row, x1, x2, white);
  }

  // Fill physical column `pcol` for rows [py1, py2) (absolute physical coords).
  static void fill_col_physical_(const RenderTarget& t, int pcol, int py1, int py2, bool white) {
    py1 = std::max(py1, t.phys_y0);
    py2 = std::min(py2, t.phys_y0 + t.phys_h);
    if (pcol < t.phys_x0 || pcol >= t.phys_x0 + t.phys_w || py1 >= py2)
      return;
    const int lrow0 = py1 - t.phys_y0;
    const int lrow1 = py2 - t.phys_y0;
    const int lpcol = pcol - t.buf_x0;
    const int bidx = lpcol / 8;
    const uint8_t bit = static_cast<uint8_t>(0x80u >> (lpcol & 7));
    for (int r = lrow0; r < lrow1; ++r) {
      uint8_t* p = t.buf + r * t.stride + bidx;
      if (white)
        *p |= bit;
      else
        *p &= static_cast<uint8_t>(~bit);
    }
  }

  // Shared UTF-8 render core: draws text into any target from any GrayPlane.
  static int draw_text_impl_(const RenderTarget& t, int x, int baseline_y, const char* text, size_t len,
                             const BitmapFont& font, GrayPlane plane, bool white, FontStyle style,
                             Rotation rotation = Rotation::Deg90);
};

}  // namespace wintergreen

namespace wintergreen {

// Shared UTF-8 text rendering core. Renders into any RenderTarget from any GrayPlane.
// BW plane: ink pixels (bit=0) are drawn. Gray planes (LSB/MSB): set pixels (bit=1) are drawn.
inline int DrawBuffer::draw_text_impl_(const RenderTarget& t, int x, int baseline_y, const char* text, size_t len,
                                       const BitmapFont& font, GrayPlane plane, bool white, FontStyle style,
                                       Rotation rotation) {
  if (!text || len == 0 || !font.valid())
    return x;
  const char* p = text;
  const char* end = text + len;
  int cursor_q = x * 4;  // quarter pixels
  char32_t prev_cp = 0;

  while (p < end) {
    // Decode UTF-8
    char32_t cp = 0;
    uint8_t b = static_cast<uint8_t>(*p);
    if (b < 0x80) {
      cp = b;
      ++p;
    } else if (b < 0xE0 && p + 1 < end) {
      cp = (static_cast<char32_t>(b & 0x1F) << 6) | (static_cast<uint8_t>(p[1]) & 0x3F);
      p += 2;
    } else if (b < 0xF0 && p + 2 < end) {
      cp = (static_cast<char32_t>(b & 0x0F) << 12) | (static_cast<char32_t>(static_cast<uint8_t>(p[1]) & 0x3F) << 6) |
           (static_cast<uint8_t>(p[2]) & 0x3F);
      p += 3;
    } else if (b < 0xF8 && p + 3 < end) {
      cp = (static_cast<char32_t>(b & 0x07) << 18) | (static_cast<char32_t>(static_cast<uint8_t>(p[1]) & 0x3F) << 12) |
           (static_cast<char32_t>(static_cast<uint8_t>(p[2]) & 0x3F) << 6) | (static_cast<uint8_t>(p[3]) & 0x3F);
      p += 4;
    } else {
      ++p;
      cp = 0xFFFD;
    }

    if (prev_cp) {
      cursor_q += font.get_kerning_q(prev_cp, cp, style);
    }

    GlyphData g = font.glyph_data(cp, style);
    const uint8_t* bits = nullptr;
    bool invert = false;
    switch (plane) {
      case GrayPlane::BW:
        bits = g.bits;
        invert = false;
        break;
      case GrayPlane::LSB:
        bits = g.gray_lsb_bits;
        invert = true;
        break;
      case GrayPlane::MSB:
        bits = g.gray_msb_bits;
        invert = true;
        break;
    }
    if (bits) {
      draw_glyph_impl_(t, (cursor_q + 2) / 4, baseline_y, bits, g.bitmap_width, g.bitmap_height, g.x_offset, g.y_offset,
                       white, invert, rotation);
    }
    cursor_q += g.advance_width;
    cursor_q = ((cursor_q + 2) / 4) * 4;  // snap to full pixel - prevents fractional accumulation across characters
    prev_cp = cp;
  }
  return cursor_q / 4;
}

inline int DrawBuffer::draw_text_proportional(int x, int baseline_y, const char* text, size_t len,
                                              const BitmapFont& font, bool white, FontStyle style) {
  return draw_text_impl_(full_target_(), x, baseline_y, text, len, font, GrayPlane::BW, white, style, rotation_);
}

inline int DrawBuffer::draw_text_plane(uint8_t* buf, int x, int baseline_y, const char* text, size_t len,
                                       const BitmapFontSet& fonts, GrayPlane plane, bool white, FontStyle style,
                                       uint8_t size_pct) {
  const BitmapFont* f = fonts.get(size_pct);
  if (!f || !f->valid())
    return x;
  const RenderTarget t{buf,
                       DisplayFrame::kStride,
                       0,
                       0,
                       DisplayFrame::kPhysicalWidth,
                       DisplayFrame::kPhysicalHeight,
                       -DisplayFrame::kPanelOffsetX};
  return draw_text_impl_(t, x, baseline_y, text, len, *f, plane, white, style, rotation_);
}

inline void DrawBuffer::draw_layout_line(uint8_t* buf, int x_offset, int baseline_y, const PageLine& line,
                                         const BitmapFontSet& fonts, GrayPlane plane, bool white) {
  const RenderTarget t{buf,
                       DisplayFrame::kStride,
                       0,
                       0,
                       DisplayFrame::kPhysicalWidth,
                       DisplayFrame::kPhysicalHeight,
                       -DisplayFrame::kPanelOffsetX};

  for (const auto& w : line.words) {
    if (w.len == 0)
      continue;
    const BitmapFont* f = fonts.get(w.size_pct);
    if (!f || !f->valid())
      continue;
    int x = x_offset + w.x;
    int word_baseline = baseline_y;
    if (w.vertical_align == VerticalAlign::Super)
      word_baseline -= static_cast<int>(fonts.y_advance(w.size_pct)) * 20 / 100;
    else if (w.vertical_align == VerticalAlign::Sub)
      word_baseline += static_cast<int>(fonts.y_advance(w.size_pct)) * 20 / 100;
    draw_text_impl_(t, x, word_baseline, w.text, w.len, *f, plane, white, w.style, rotation_);
  }
}

}  // namespace wintergreen
