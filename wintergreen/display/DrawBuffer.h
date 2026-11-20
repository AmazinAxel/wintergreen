#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../content/BitmapFont.h"
#include "../content/wgb/WgbPageView.h"

namespace wintergreen {

// Deg90 is portrait (every list, and the reader's default); Deg0 is the reader's
// landscape.
enum class Rotation { Deg0 = 0, Deg90 = 90 };

// What one panel pixel shows. A normal update can produce White, Gray and Black
// (one of the four waveform slots must be HOLD, see epd.h); Light exists only in
// a full refresh, where every pixel is driven anyway (the sleep frame).
//
// Two bit planes carry it everywhere (fonts, images, covers, the strip scratch):
// `hi` = level bit 1, `lo` = level bit 0. White 00, Light 01, Gray 10, Black 11,
// so black ink composites with OR, and a glyph's black plane is inside its gray
// plane.
enum class Level : uint8_t { White = 0, Light = 1, Gray = 2, Black = 3 };

// How text meets what is under it. Black: the glyph's gray and black pixels are
// added (OR). White: the reverse, for text on a black fill (a selected row): the
// glyph's black core clears to white and its antialiasing edge to gray.
enum class Ink : uint8_t { Black = 0, White = 1 };

// Physical screen constants.
struct DisplayFrame {
  // 7 + 793 = 800: the panel's hidden columns are all at the leading edge, not
  // split across both. This said 10 + 786 once, leaving 4 columns that no
  // drawing code could reach — they showed as a white bar along the top in
  // portrait, most visible against a dark cover. Determined on hardware:
  // dropping the offset to 7 moved the bar wholesale to the trailing edge rather
  // than splitting it, so nothing is hidden there.
  static constexpr int kPhysicalWidth = 793;  // visible app-space width
  static constexpr int kPanelOffsetX = 7;     // hidden columns at the leading edge of the 800px panel
  static constexpr int kPanelWidth = 800;
  static constexpr int kPhysicalHeight = 480;
  static constexpr int kStride = kPanelWidth / 8;  // 100 bytes per plane row
  static constexpr std::size_t kPixelBytes = static_cast<std::size_t>(kStride) * kPhysicalHeight;  // 48000, one plane
  // A whole 4-level frame (sleep cover, wordmark): per physical row, the hi
  // plane's 100 bytes then the lo plane's.
  static constexpr std::size_t kFrameBytes = 2 * kPixelBytes;  // 96000
};

enum class UpdateKind : uint8_t {
  Turn,  // page turn, menus: HOLD / to-white / to-gray / to-black per pixel
  Full,  // boot paint, sleep: every pixel cleared, then set to its level
};

// The panel driver, as the strip renderer sees it. EInkDisplay (epd.h) on the
// device; a simulated controller in the host tests.
//
// An update is begin_update, any number of write_window calls, then fire.
// Controller RAM keeps what was written between updates, which is what lets an
// update write only the windows that changed (DrawBuffer tracks what RAM may
// still hold, see dirty_).
class IDisplay {
 public:
  virtual ~IDisplay() = default;

  // Wait out any running waveform, wake the controller, load the LUT for `kind`.
  virtual void begin_update(UpdateKind kind) = 0;
  // Block until at most one window transfer is in flight, so the strip buffer
  // the one before it used may be refilled.
  virtual void await_strip_slot() = 0;
  // Queue a window of controller RAM: physical rows [row0, row0 + rows), plane
  // bytes [xb0, xb0 + xbytes) of each. `bw` and `red` are rows x xbytes, packed;
  // they must stay untouched until await_strip_slot() has been called twice
  // more, or finish_writes(). For a Turn update each pixel's 2 bits are its slot
  // (RED << 1 | BW); for Full they are its level.
  virtual void write_window(int row0, int rows, int xb0, int xbytes, const uint8_t* bw, const uint8_t* red) = 0;
  // Block until every queued write is on the controller: the bus (shared with
  // the SD card) is free.
  virtual void finish_writes() = 0;
  // Run the waveform. Turn returns at once (the panel stays busy); Full waits,
  // and powers the panel down at the end when `turn_off`.
  virtual void fire(UpdateKind kind, bool turn_off) = 0;

  virtual void deep_sleep() {}
  // True while a waveform runs.
  virtual bool is_busy() const { return false; }
  // Block until any outstanding waveform has finished. The SD card shares SPI2
  // with the panel and traffic during a waveform corrupts it: call before SD.
  virtual void wait_idle() {}
};

// Pixels that live outside RAM — on the card (covers, reader images, the sleep
// cover) or in flash (the wordmark) — read while a refresh renders, a strip at a
// time, so they never need a buffer of their own.
class PixelSource {
 public:
  // Read `n` bytes at `offset` of item `item` into `dst`. False leaves those
  // pixels white.
  virtual bool read(uint32_t item, uint32_t offset, uint8_t* dst, size_t n) = 0;
  // After a refresh that read from it: close whatever read() opened.
  virtual void release() {}

 protected:
  ~PixelSource() = default;
};

// How a source's pixels are laid out (all two planes, bit set = ink).
enum class PixelLayout : uint8_t {
  Cols,   // portrait: column c = ceil(h/8) hi bytes then ceil(h/8) lo bytes, top MSB
  Rows,   // landscape: row r = ceil(w/8) hi bytes then ceil(w/8) lo bytes, left MSB
  Frame,  // a whole panel frame: physical row = 100 hi bytes then 100 lo bytes
};

// Everything drawn, as a draw list rather than pixels.
//
// There is no framebuffer. Screens draw through the calls below exactly as
// before; each call is recorded into the current list (cur_). refresh() then
// knows two lists — what is on the glass (prev_) and what should be — and:
//
//   1. matches them op by op; only the bounding boxes of ops that differ can
//      have changed pixels (damage);
//   2. walks the panel in strips of kStripRows physical rows; inside the damage
//      it replays both lists into two small scratch strips, compares them, and
//      gives every changed pixel its target slot (to-white / to-gray / to-black)
//      and every other pixel HOLD;
//   3. writes only those windows (plus whatever controller RAM may still hold a
//      drive slot from the last update) and fires one waveform.
//
// So a page turn is one panel update with all of its grays already in it, a
// cursor move renders and sends two rows, and an unchanged screen costs nothing.
// See docs/display.md "Draw lists and strips".
//
// Every buffer is claimed once at construction (heap, see buf_), ~26 KB in all.
class DrawBuffer {
 public:
  // Logical portrait dimensions.
  static constexpr int kWidth = DisplayFrame::kPhysicalHeight;
  static constexpr int kHeight = DisplayFrame::kPhysicalWidth;

  // Strip geometry. 16 rows: 30 strips, 3,200 B per two-plane scratch strip.
  static constexpr int kStripRows = 16;
  static constexpr int kStrips = DisplayFrame::kPhysicalHeight / kStripRows;
  static_assert(DisplayFrame::kPhysicalHeight % kStripRows == 0, "strips must tile the panel");
  static constexpr size_t kPlaneBytes = static_cast<size_t>(kStripRows) * DisplayFrame::kStride;
  static constexpr size_t kStripBytes = 2 * kPlaneBytes;
  // One draw list. Holds the ops of a screen and, at its tail, the reader's
  // display list (or the page parked under the quick menu). Sized from the
  // largest real screens (tools/tests/drawlist_test.cpp prints them).
  static constexpr size_t kArenaBytes = 6 * 1024;
  // Card/flash pixels for one strip. A strip that would need more is processed
  // in fewer rows at a time.
  static constexpr size_t kFetchBytes = 4096;
  // Damage intervals kept per strip before neighbours merge.
  static constexpr int kMaxIntervals = 4;

  explicit DrawBuffer(IDisplay& display);
  ~DrawBuffer();
  DrawBuffer(const DrawBuffer&) = delete;
  DrawBuffer& operator=(const DrawBuffer&) = delete;

  IDisplay& display() { return display_; }
  const IDisplay& display() const { return display_; }

  // Logical rotation of every draw call. The panel is always written in
  // physical coordinates; the transform is applied when a call is recorded.
  void set_rotation(Rotation r) { rotation_ = r; }
  Rotation rotation() const { return rotation_; }
  int width() const { return rotation_ == Rotation::Deg0 ? DisplayFrame::kPhysicalWidth : kWidth; }
  int height() const { return rotation_ == Rotation::Deg0 ? DisplayFrame::kPhysicalHeight : kHeight; }

  // -- Drawing (logical coordinates) -----------------------------------------

  // Start a new screen: the list restarts with the whole panel at `level`.
  // The tail (reader display list, parked page) is kept.
  void fill(Level level = Level::White);
  // A rectangle at an exact level.
  void fill_rect(int x, int y, int w, int h, Level level);
  // A `t`-pixel frame growing inwards.
  void draw_outline(int x, int y, int w, int h, int t, Level level);
  // Diamond (a square on its corner) centred on (cx, cy) with radius r,
  // antialiased. t = 0: filled; otherwise an outline t pixels thick.
  void draw_diamond(int cx, int cy, int r, int t = 0);

  // UTF-8 text with its baseline at `baseline_y`; returns the x after it.
  int draw_text(int x, int baseline_y, const char* text, size_t len, const BitmapFont& font, Ink ink = Ink::Black,
                FontStyle style = FontStyle::Regular);
  int draw_text(int x, int baseline_y, const char* text, const BitmapFont& font, Ink ink = Ink::Black,
                FontStyle style = FontStyle::Regular) {
    return draw_text(x, baseline_y, text, text ? strlen(text) : 0, font, ink, style);
  }

  // One baked line (WgbFormat.h). Its words must lie in the tail (claim_tail),
  // which is where the reader reads the page's display list.
  void draw_baked_line(const WgbLine& line, const uint8_t* end, const BitmapFontSet& fonts);

  // Pixels from a source, w x h, top-left (x, y); only source rows
  // [keep_lo, keep_hi) are drawn. Cols needs portrait, Rows landscape.
  // (src, item, gen, base) identify the pixels: equal identity = equal pixels,
  // so bump `gen` whenever the bytes behind an item may have changed.
  void blit(PixelSource* src, uint32_t item, uint32_t gen, uint32_t base, PixelLayout layout, int x, int y, int w,
            int h, int keep_lo = 0, int keep_hi = 0x7FFF);

  // -- The tail ---------------------------------------------------------------
  //
  // Bytes at the end of the list being recorded, kept across fill(): the
  // reader reads a page's display list straight into it and records lines that
  // point there. `tag` says what it holds (0 = nothing), so a page already
  // there — staged after the last waveform — is not read again. Claiming
  // starts a new screen: the list's ops are dropped, fill() comes next.
  uint8_t* claim_tail(size_t n, uint32_t tag);
  uint32_t tail_tag() const { return cur().tail_tag; }
  const uint8_t* tail_data() const { return cur().base + kArenaBytes - cur().tail_len; }
  size_t tail_size() const { return cur().tail_len; }

  // -- The quick menu's snapshot ------------------------------------------------
  //
  // save_snapshot() parks what is on the glass (the reader's page: ops and
  // display list) at the tail of the lists the overlay records into; no extra
  // buffer. restore_snapshot() makes it the current list again, ready for
  // refresh() — no card read, no redraw. False when it was dropped to make
  // room (the overlay then needed the whole list) or never taken.
  void save_snapshot();
  bool restore_snapshot();
  bool has_snapshot() const { return parked_; }
  void drop_snapshot();

  // -- Display operations -----------------------------------------------------

  // One normal update from what is on the glass to the current list. Returns at
  // once; the waveform runs on (every panel entry waits it out first).
  void refresh();
  // Every pixel driven, four levels (boot paint, sleep). Waits for the end.
  void full_refresh(bool turn_off = false);
  void set_sunlight_fading_fix(bool v) { sunlight_fading_fix_ = v; }

  // The wordmark, a Frame mapped from the `sleep` partition (font_partition.h).
  void set_sleep_image(const uint8_t* data, size_t size);
  // Full refresh of a whole Frame (the wordmark, or a book's cover_sleep.bin)
  // and panel power-down. The wordmark variant returns false without an image.
  void show_frame(PixelSource* src, uint32_t item);
  bool show_sleep_image();

  // Block until the panel has finished any outstanding refresh. Call before SD
  // access: the card shares SPI2 with the display.
  void wait_panel_idle() { display_.wait_idle(); }
  void deep_sleep() { display_.deep_sleep(); }

  // -- Introspection (host tests) ----------------------------------------------
  struct Stats {
    uint32_t ops = 0, ops_bytes = 0, tail_bytes = 0;
    bool overflowed = false;
  };
  // The list being recorded, or (glass) the one on the panel.
  Stats current_stats(bool glass = false) const;
  // Rasterise the current list into a full 2-plane frame (kFrameBytes, the
  // Frame layout), through the same strip code. Host previews and tests.
  void render_current(uint8_t* frame);

 private:
  struct Arena {
    uint8_t* base = nullptr;
    size_t used = 0;          // op records, from base
    size_t tail_len = 0;      // bytes at the end
    uint32_t tail_tag = 0;
    size_t parked_ops = 0;    // when parked: op bytes at the start of the tail block
    bool overflowed = false;
  };
  struct Interval {
    uint8_t lo, hi;  // plane bytes [lo, hi)
  };
  struct Fetch {
    uint32_t op;           // record offset in cur_
    const uint8_t* data;   // first unit fetched
    int first;             // first unit (column, row) it holds
  };

  IDisplay& display_;
  Rotation rotation_ = Rotation::Deg90;
  bool sunlight_fading_fix_ = true;

  // **Heap, not BSS.** As member arrays the old framebuffers were .bss, which
  // lands in the one ~50 KB contiguous DRAM pool the BT controller must
  // allocate from ("esp_bt_controller_init -4"). On the heap the allocator can
  // place them elsewhere. One block, claimed at construction, never freed until
  // the destructor (which only the host tests reach).
  uint8_t* mem_ = nullptr;
  Arena arenas_[2];
  int cur_ = 0;           // arenas_[cur_] records; the other is on the glass
  bool cur_started_ = false;  // fill() since the last refresh
  bool prev_valid_ = false;   // the glass shows prev(); false = unknown (boot)
  bool parked_ = false;
  uint8_t* scratch_new_ = nullptr;   // kStripBytes: hi plane then lo plane
  uint8_t* scratch_old_[2] = {};     // alternate; each becomes its strip's send buffer
  uint8_t* fetch_ = nullptr;         // kFetchBytes
  int send_slot_ = 0;

  // Controller RAM per physical row: plane bytes [lo, hi) may hold a non-HOLD
  // slot from the last update. Every update rewrites at least this, or the
  // controller would drive those pixels again (it drives the whole panel from
  // RAM every time).
  uint8_t dirty_lo_[DisplayFrame::kPhysicalHeight] = {};
  uint8_t dirty_hi_[DisplayFrame::kPhysicalHeight] = {};

  // Damage per strip for the refresh in progress.
  Interval iv_[kStrips][kMaxIntervals] = {};
  uint8_t iv_n_[kStrips] = {};
  // Old pixels nobody can reproduce (an image no longer drawn): physical rects
  // where every pixel is driven to its new level.
  struct Rect {
    int16_t r0, r1, b0, b1;
  };
  static constexpr int kMaxUnknown = 8;
  Rect unknown_[kMaxUnknown] = {};
  int unknown_n_ = 0;
  bool all_unknown_ = false;

  // Wordmark mapping.
  const uint8_t* sleep_img_ = nullptr;
  size_t sleep_img_size_ = 0;
  class MemorySource;
  MemorySource* sleep_src_ = nullptr;

  Arena& cur() { return arenas_[cur_]; }
  const Arena& cur() const { return arenas_[cur_]; }
  Arena& prev() { return arenas_[cur_ ^ 1]; }

  // Recording.
  uint8_t* begin_op_(size_t payload);
  void ensure_started_();
  void phys_rect_(int x, int y, int w, int h, int& r0, int& r1, int& b0, int& b1) const;

  // Refresh machinery (DrawBuffer.cpp).
  void match_();
  void add_damage_(int r0, int r1, int b0, int b1);
  void add_unknown_(int r0, int r1, int b0, int b1);
  void run_(UpdateKind kind, bool turn_off);
  bool fetch_chunk_(int ra, int rb, Fetch* fetches, int& nf);
  void render_(const Arena& a, bool old, int ra, int rb, int xa, int xb, uint8_t* planes, const Fetch* fetches,
               int nf);
  void swap_lists_();
  void release_sources_();
};

}  // namespace wintergreen
