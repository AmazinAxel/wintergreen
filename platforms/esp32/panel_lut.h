#pragma once

// SSD1677 Turn waveform table for the GDEQ0426T82, built at runtime from a few
// named frame counts. No IDF dependency: tools/tests/lut_test.cpp builds the
// same table on the host. Everything but reader pages runs the OTP waveforms.
//
// One update drives each pixel by the waveform its two RAM bits select
// (state = RED << 1 | BW, LUT0..LUT3; SSD1677 datasheet Table 6-5).
//
//   Turn (gray reader pages). State = slot. LUT0 HOLD (no drive), LUT1
//   to-white, LUT2 to-gray, LUT3 to-black. All three drive at once, so the
//   old page fades out while the new one comes in (no reset pass first):
//
//     frame      0 1        9   11  13
//     to-white     | white 12 ......|
//     to-gray    | black 9 | white 2|
//     to-black   | black 9 |
//
//   (Halved from the stock text waveform below, 2026-11-25; frame counts at
//   kToWhite.)
//
//   Black and white are the Xteink X4 Pro stock firmware's one-pass text
//   waveform (kX4ProTextSsd1677, extracted by the FreeInk SDK,
//   Free-Ink/freeink-sdk PR #185: black VSH1 17, to-white VSL 24, FR 0x33,
//   measured ~600 ms on an X4). Its gray (VSL 6 then VSH1 4) let old text
//   show through new gray. Ours is the X4 OEM full table's trick: drive to
//   black like to-black (whatever was there is gone), then pull back with
//   white. Picked on hardware from 1..4 frames back (2026-11-19 comparison
//   screen): 4 matched a 1 px checkerboard (50%) and showed no old text;
//   2 since the halving.
//
//   The stock waveform re-drives every black pixel each update; GrayPage
//   does the same in changed strips (held black fades within minutes, so
//   glyphs half re-driven read uneven). Its white-rest "nudge" row is our
//   HOLD: held gray and black share that slot.
//
//   Rejected on hardware (2026-11-19): a white reset pass first (two-pass
//   feel); gray erased 24 white frames with gray re-driven in the damage
//   (slow, AA flashed every update); OEM full recipes per slot (50 frames) and
//   a black-then-white flash for every driven pixel (58): too slow to read.
//
// Turn unrolled into phases (10 groups x 4), no repeat counts.
//
// Not DC-balanced (neither is the stock waveform). The sleep/boot OTP
// full refreshes clear residual charge.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace panel_lut {

// VS codes (2 bits per phase): 00 VSS, 01 VSH1, 10 VSL, 11 VSH2. VSH1 pulls
// toward black (the stock text waveform's black row is VSH1) — **the one place
// to swap if the panel comes up inverted.**
inline constexpr uint8_t kVsHold = 0b00;
inline constexpr uint8_t kVsBlack = 0b01;
inline constexpr uint8_t kVsWhite = 0b10;

// The vendor's voltages for this panel (Good Display's 4-gray table, identical
// in Adafruit's hardware-verified ThinkInk_426_Grayscale4_GDEQ and the X4's OEM
// factory LUT). They are the electrical safety envelope: never tune these, only
// the timings.
inline constexpr uint8_t kGateVoltage = 0x17;                      // 0x03: VGH
inline constexpr uint8_t kSourceVoltage[3] = {0x41, 0xA8, 0x32};   // 0x04: VSH1, VSH2, VSL
// The X4 OEM value. 0x14 (the X4 Pro firmware's, paired with the stock text
// waveform) was tried 2026-11-19: no visible change to ghosting or darkness.
inline constexpr uint8_t kVcom = 0x30;                             // 0x2C

// Frame rate byte (each nibble one group pair), from the stock text waveform.
inline constexpr uint8_t kFrameRate = 0x33;

// Turn frame counts at room temperature: the stock text waveform (24 / 17 / 4,
// ~600 ms) halved, 13 frames ~310 ms at ~24 ms per frame (2026-11-25, owner's
// pick on hardware: "so good"). The stock 25 was the 2026-11-19 pick; 17 (~410 ms)
// was the middle step.
inline constexpr int kToWhite = 12;     // to-white: VSL
inline constexpr int kWhiteStart = 1;   // ... from this frame
inline constexpr int kToBlack = 9;      // to-black: VSH1 from frame 0
inline constexpr int kGrayBack = 2;     // to-gray: to-black's drive, then this many VSL frames

inline constexpr size_t kLutBytes = 105;  // 0x32: 5 x 10 VS, 10 x 5 TP/RP, 5 FR
inline constexpr int kMaxPhases = 40;     // 10 groups x 4 phases

struct Timing {
  int white, white_start, black, gray_back;
  int turn_len() const {
    const int w = white_start + white, g = black + gray_back;  // to-black ends first
    return w > g ? w : g;
  }
};

// Colder panels respond slower: the same pulses, longer. Bands from the C3's
// on-chip sensor (chip temperature, a few degrees above ambient).
enum class Band : uint8_t { Warm, Cool, Cold };
inline constexpr int kCoolBelowC = 20;
inline constexpr int kColdBelowC = 10;
inline constexpr int kHysteresisC = 2;

// Band for `celsius`, staying in `current` within kHysteresisC of a boundary so
// a reading hovering at it does not flip the LUT on every update.
inline Band band_for(float celsius, Band current) {
  auto raw = [](float t) {
    return t < kColdBelowC ? Band::Cold : t < kCoolBelowC ? Band::Cool : Band::Warm;
  };
  const Band b = raw(celsius);
  if (b == current)
    return b;
  // Leaving the current band needs the reading past the boundary by the margin.
  const Band lo = raw(celsius + kHysteresisC), hi = raw(celsius - kHysteresisC);
  if (lo == current || hi == current)
    return current;
  return b;
}

inline int scaled_(int frames, Band band) {
  // x1, x1.25, x1.5, rounded up, at most one TP byte.
  const int num = band == Band::Warm ? 4 : band == Band::Cool ? 5 : 6;
  const int v = (frames * num + 3) / 4;
  return v > 255 ? 255 : v;
}

inline Timing timing(Band band) {
  auto s = [band](int f) { return scaled_(f, band); };
  return Timing{s(kToWhite), s(kWhiteStart), s(kToBlack), s(kGrayBack)};
}

// Layout offsets inside the 105 bytes.
inline constexpr size_t kVsOffset = 0;    // LUTn group g at n * 10 + g
inline constexpr size_t kTpOffset = 50;   // group g at 50 + g * 5: TPA TPB TPC TPD RP
inline constexpr size_t kFrOffset = 100;  // 5 bytes

// One drive of LUT `lut`: `code` over frames [start, start + len).
struct Seg_ {
  int lut, start, len;
  uint8_t code;
};

// Unrolls segments into phases: one phase per run of frames between segment
// edges, each LUT driving whatever segment covers that run (else HOLD). RP
// stays 0 (run once).
inline void build_(uint8_t (&out)[kLutBytes], const Seg_* segs, int n, uint8_t frame_rate = kFrameRate) {
  std::memset(out, 0, kLutBytes);
  int edges[2 * 16], ne = 0;
  for (int k = 0; k < n && ne + 2 <= 32; ++k) {
    edges[ne++] = segs[k].start;
    edges[ne++] = segs[k].start + segs[k].len;
  }
  for (int a = 1; a < ne; ++a)  // insertion sort: a handful of edges
    for (int b = a; b > 0 && edges[b - 1] > edges[b]; --b) {
      const int t = edges[b];
      edges[b] = edges[b - 1];
      edges[b - 1] = t;
    }
  int i = 0;
  for (int e = 0; e + 1 < ne && i < kMaxPhases; ++e) {
    const int f0 = edges[e], f1 = edges[e + 1];
    if (f1 <= f0)
      continue;
    const int g = i / 4, shift = 6 - 2 * (i % 4);
    for (int k = 0; k < n; ++k)
      if (segs[k].start <= f0 && f1 <= segs[k].start + segs[k].len)
        out[kVsOffset + segs[k].lut * 10 + g] |= static_cast<uint8_t>(segs[k].code << shift);
    const int frames = f1 - f0;  // <= 255: every count is scaled_() to a byte and edges only split
    out[kTpOffset + g * 5 + i % 4] = static_cast<uint8_t>(frames > 255 ? 255 : frames);
    ++i;
  }
  for (int k = 0; k < 5; ++k)
    out[kFrOffset + k] = frame_rate;
}

inline void build_turn(uint8_t (&out)[kLutBytes], const Timing& t) {
  constexpr uint8_t W = kVsWhite, B = kVsBlack;
  const Seg_ segs[] = {
      {1, t.white_start, t.white, W},
      {2, 0, t.black, B},
      {2, t.black, t.gray_back, W},
      {3, 0, t.black, B},
  };
  build_(out, segs, 4);
}

}  // namespace panel_lut
