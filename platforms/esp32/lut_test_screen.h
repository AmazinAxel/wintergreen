#pragma once

// TEMPORARY waveform comparison screen (2026-11-19). Compiled only with
// WG_LUT_TEST; delete with the define once a turn waveform is chosen.
//
// Five columns, one candidate page-turn waveform each, numbered by the black
// squares at the top (1..5). After one full refresh (pattern A everywhere),
// every column is toggled A -> B -> A ... 11 times, each column by its own
// update with its own LUT while the others hold. The screen ends on B:
//
//   zone 1 (top)     thin horizontal strokes   (A had vertical ones: any
//                                               vertical lines = ghosting)
//   zone 2           solid gray                (A was black), beside a 1 px
//                                              checkerboard: the 50% reference
//   zone 3           solid gray                (A was vertical text strokes)
//
// Round 2 (after round 1 on hardware: strip 1 best on text, but old text showed
// through new gray; strips 2 and 4 too slow to read with): 1 = current, 2..5 =
// current with gray driven black first (as to-black), then pulled back by
// 1..4 white frames. All 28 frames.
//   zone 4 (bottom)  white with sparse strokes (A was solid gray)
//
// Then the device deep-sleeps; the power button reruns the test.

#include <cstdint>
#include <cstring>

#include "epd.h"
#include "panel_lut.h"

namespace wg_lut_test {

inline constexpr int kCols = 5;
inline constexpr int kColPx = 160;
inline constexpr int kColBytes = kColPx / 8;
inline constexpr int kRows = EInkDisplay::DISPLAY_HEIGHT;  // 480 physical rows
inline constexpr int kStrip = 16;
inline constexpr int kToggles = 11;

// Levels: 0 white, 1 light, 2 gray, 3 black.
inline int level_(int col, bool b, int x, int y) {
  if (x < 4 || x >= kColPx - 4)
    return 0;
  if (y < 24) {  // label: col + 1 squares
    const bool sq = x >= 8 && (x - 8) / 20 <= col && (x - 8) % 20 < 14 && y >= 5 && y < 19;
    return sq ? 3 : 0;
  }
  const int zone = (y - 24) / 114;
  if ((y - 24) % 114 < 3)
    return 0;  // white gap between zones
  switch (zone) {
    case 0:
      return b ? (y % 8 < 2 ? 3 : 0) : (x % 8 < 2 ? 3 : 0);
    case 1:
      if (x >= 100)
        return ((x + y) & 1) ? 3 : 0;  // 50% reference, the same in A and B
      return b ? 2 : 3;
    case 2:
      return b ? 2 : (x % 6 < 2 ? 3 : 0);
    default:
      return b ? (x % 16 < 2 ? 3 : 0) : 2;
  }
}

inline void set_bit_(uint8_t* p, int x) {
  p[x >> 3] |= static_cast<uint8_t>(0x80 >> (x & 7));
}

// Candidates, slots HOLD / to-white / to-gray / to-black.
inline void candidate_(int col, uint8_t (&out)[panel_lut::kLutBytes]) {
  using namespace panel_lut;
  constexpr uint8_t W = kVsWhite, B = kVsBlack;
  if (col == 0) {  // 1: round 1's strip 1 (X4 Pro stock text waveform, 28 frames FR 0x33)
    const Seg_ stock[] = {{1, 4, 24, W}, {2, 0, 6, W}, {2, 6, 4, B}, {3, 3, 17, B}};
    build_(out, stock, 4);
    return;
  }
  // 2..5: current, gray = to-black's drive, then `col` white frames back.
  const Seg_ segs[] = {
      {1, kWhiteStart, kToWhite, W},
      {2, 0, kToBlack, B},
      {2, kToBlack, col, W},
      {3, 0, kToBlack, B},
  };
  build_(out, segs, 4);
}

// One window of column `col` (all columns when col < 0): Full levels, turn
// slots for `from` -> `to`, or zeros (HOLD) when `zero`.
inline void write_(EInkDisplay& epd, int col, bool full, bool from, bool to, bool zero) {
  static uint8_t bw[kStrip * 100], red[kStrip * 100];
  const int c0 = col < 0 ? 0 : col, c1 = col < 0 ? kCols : col + 1;
  const int xbytes = (c1 - c0) * kColBytes;
  for (int r0 = 0; r0 < kRows; r0 += kStrip) {
    std::memset(bw, 0, sizeof(bw));
    std::memset(red, 0, sizeof(red));
    if (!zero)
      for (int r = 0; r < kStrip; ++r)
        for (int c = c0; c < c1; ++c)
          for (int x = 0; x < kColPx; ++x) {
            const int px = (c - c0) * kColPx + x;
            uint8_t* pb = bw + r * xbytes;
            uint8_t* pr = red + r * xbytes;
            const int n = level_(c, to, x, r0 + r);
            int s;  // RED << 1 | BW
            if (full) {
              s = n;
            } else {
              const int o = level_(c, from, x, r0 + r);
              s = (o == n && n != 3) ? 0 : n == 0 ? 1 : n == 2 ? 2 : 3;
            }
            if (s & 1)
              set_bit_(pb, px);
            if (s & 2)
              set_bit_(pr, px);
          }
    epd.write_window(r0, kStrip, c0 * kColBytes, xbytes, bw, red);
    epd.finish_writes();
  }
}

inline void run(EInkDisplay& epd) {
  epd.begin_update(wintergreen::UpdateKind::Full);
  write_(epd, -1, true, false, false, false);
  epd.fire(wintergreen::UpdateKind::Full, false);
  write_(epd, -1, false, false, false, true);  // Full left levels in RAM: all HOLD

  bool on_b = false;
  for (int t = 0; t < kToggles; ++t) {
    for (int c = 0; c < kCols; ++c) {
      uint8_t lut[panel_lut::kLutBytes];
      candidate_(c, lut);
      epd.begin_update(wintergreen::UpdateKind::Turn);
      epd.upload_lut(lut);
      write_(epd, c, false, on_b, !on_b, false);
      epd.fire(wintergreen::UpdateKind::Turn, false);
      epd.wait_idle();
      write_(epd, c, false, false, false, true);  // back to HOLD before the next column fires
    }
    on_b = !on_b;
  }
}

}  // namespace wg_lut_test
