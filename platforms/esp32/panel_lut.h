#pragma once

// SSD1677 waveform tables for the GDEQ0426T82, built at runtime from a few
// named frame counts. No IDF dependency: tools/tests/lut_test.cpp builds the
// same tables on the host.
//
// One update drives each pixel by the waveform its two RAM bits select
// (state = RED << 1 | BW, LUT0..LUT3). Two tables:
//
//   Turn (page turns, menus). State = slot. LUT0 HOLD (no drive: unchanged
//   pixels are never touched), LUT1 to-white, LUT2 to-gray, LUT3 to-black. One
//   group, two phases, every target ending on the same final frame, so all of a
//   page's levels land together and nothing moves after it has landed.
//
//     HOLD      VSS        VSS
//     to-white  white      white       (D - P frames, then P frames)
//     to-gray   white      black       saturate white, then P frames of black
//     to-black  black      black
//
//   Full (boot paint, sleep). State = level. Group 0 clears every pixel (black
//   for C frames, then white for C: the flash, balanced); group 1 sets it,
//   right-aligned: black D frames, gray P, light PL, white nothing. Gray is "P
//   frames of black from white" in both tables, so it is the same shade.
//
// Not DC-balanced on turns (the vendor's OTP fast waveform was not either):
// the owner chose "no visible motion" over in-update counter-pulses. The
// sleep/boot full refreshes clear residual charge, as before.
//
// Everything below the voltages is a guess until the device says otherwise;
// docs/display.md "Waveform tuning" is the checklist.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace panel_lut {

// VS codes (2 bits per phase): 00 VSS, 01 VSH1, 10 VSL, 11 VSH2. Which of VSH1
// / VSL pulls toward black is inferred from the vendor 4-gray table (its black
// state ends on VSH1) — **the one place to swap if the panel comes up
// inverted.**
inline constexpr uint8_t kVsHold = 0b00;
inline constexpr uint8_t kVsBlack = 0b01;
inline constexpr uint8_t kVsWhite = 0b10;

// The vendor's voltages for this panel (Good Display's 4-gray table, identical
// in Adafruit's hardware-verified ThinkInk_426_Grayscale4_GDEQ). They are the
// electrical safety envelope: never tune these, only the timings.
inline constexpr uint8_t kGateVoltage = 0x17;                      // 0x03: VGH
inline constexpr uint8_t kSourceVoltage[3] = {0x41, 0xA8, 0x32};   // 0x04: VSH1, VSH2, VSL
inline constexpr uint8_t kVcom = 0x30;                             // 0x2C

// Frame rate byte (each nibble one group pair), from the vendor table.
inline constexpr uint8_t kFrameRate = 0x22;

// Frame counts at room temperature.
inline constexpr int kSaturate = 20;  // D: frames to drive a pixel fully white or black
inline constexpr int kGray = 5;       // P: black frames from white to the gray level
inline constexpr int kLight = 2;      // PL: black frames from white to the light level (Full only)
inline constexpr int kClear = 20;     // C: each half of the full refresh's clear

inline constexpr size_t kLutBytes = 105;  // 0x32: 5 x 10 VS, 10 x 5 TP/RP, 5 FR

struct Timing {
  int d, p, pl, c;
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
  return Timing{scaled_(kSaturate, band), scaled_(kGray, band), scaled_(kLight, band), scaled_(kClear, band)};
}

inline uint8_t vs_(uint8_t a, uint8_t b = kVsHold, uint8_t c = kVsHold, uint8_t d = kVsHold) {
  return static_cast<uint8_t>((a << 6) | (b << 4) | (c << 2) | d);
}

// Layout offsets inside the 105 bytes.
inline constexpr size_t kVsOffset = 0;    // LUTn group g at n * 10 + g
inline constexpr size_t kTpOffset = 50;   // group g at 50 + g * 5: TPA TPB TPC TPD RP
inline constexpr size_t kFrOffset = 100;  // 5 bytes

inline void set_tp_(uint8_t* out, int group, int a, int b, int c = 0, int d = 0) {
  uint8_t* g = out + kTpOffset + group * 5;
  g[0] = static_cast<uint8_t>(a);
  g[1] = static_cast<uint8_t>(b);
  g[2] = static_cast<uint8_t>(c);
  g[3] = static_cast<uint8_t>(d);
  g[4] = 0;  // repeat count 0 = run once
}

inline void finish_(uint8_t* out) {
  for (int i = 0; i < 5; ++i)
    out[kFrOffset + i] = kFrameRate;
}

inline void build_turn(uint8_t (&out)[kLutBytes], const Timing& t) {
  std::memset(out, 0, kLutBytes);
  out[kVsOffset + 0 * 10] = vs_(kVsHold, kVsHold);    // LUT0 HOLD
  out[kVsOffset + 1 * 10] = vs_(kVsWhite, kVsWhite);  // LUT1 to-white
  out[kVsOffset + 2 * 10] = vs_(kVsWhite, kVsBlack);  // LUT2 to-gray
  out[kVsOffset + 3 * 10] = vs_(kVsBlack, kVsBlack);  // LUT3 to-black
  set_tp_(out, 0, t.d - t.p, t.p);
  finish_(out);
}

inline void build_full(uint8_t (&out)[kLutBytes], const Timing& t) {
  std::memset(out, 0, kLutBytes);
  for (int n = 0; n < 4; ++n)
    out[kVsOffset + n * 10 + 0] = vs_(kVsBlack, kVsWhite);  // clear: black then white
  out[kVsOffset + 0 * 10 + 1] = vs_(kVsHold, kVsHold, kVsHold);     // white
  out[kVsOffset + 1 * 10 + 1] = vs_(kVsHold, kVsHold, kVsBlack);    // light
  out[kVsOffset + 2 * 10 + 1] = vs_(kVsHold, kVsBlack, kVsBlack);   // gray
  out[kVsOffset + 3 * 10 + 1] = vs_(kVsBlack, kVsBlack, kVsBlack);  // black
  set_tp_(out, 0, t.c, t.c);
  set_tp_(out, 1, t.d - t.p, t.p - t.pl, t.pl);
  finish_(out);
}

}  // namespace panel_lut
