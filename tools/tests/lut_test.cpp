// The waveform tables epd.h uploads (platforms/esp32/panel_lut.h): 105 bytes in
// the controller's layout, HOLD really idle, the turn's targets driving at once
// as the stock X4 Pro text waveform (no reset pass before the new page),
// temperature bands that only lengthen, clamp, and do not flap, and the
// vendor's voltages untouched.
#include <cassert>
#include <cstdio>
#include <initializer_list>

#include "platforms/esp32/panel_lut.h"

using namespace panel_lut;

namespace {

// Phase code of LUT `n`, group `g`, phase `ph` (0..3 = A..D).
int vs(const uint8_t* lut, int n, int g, int ph) {
  return (lut[kVsOffset + n * 10 + g] >> (6 - 2 * ph)) & 3;
}
int tp(const uint8_t* lut, int g, int ph) {
  return lut[kTpOffset + g * 5 + ph];
}

// The drive sequence LUT n produces, frame by frame.
struct Seq {
  int len = 0;
  int code[2048];
};
Seq run(const uint8_t* lut, int n) {
  Seq s;
  for (int g = 0; g < 10; ++g)
    for (int rep = 0; rep <= lut[kTpOffset + g * 5 + 4]; ++rep)  // RP: run RP + 1 times
      for (int ph = 0; ph < 4; ++ph)
        for (int f = 0; f < tp(lut, g, ph); ++f)
          s.code[s.len++] = vs(lut, n, g, ph);
  return s;
}

// Frames from the last non-VSS frame to the end (0 = ends driving).
int idle_tail(const Seq& s) {
  int k = 0;
  while (k < s.len && s.code[s.len - 1 - k] == kVsHold)
    ++k;
  return k;
}
int count(const Seq& s, int code, int from = 0) {
  int n = 0;
  for (int i = from; i < s.len; ++i)
    n += s.code[i] == code;
  return n;
}

}  // namespace

int main() {
  assert(kVsBlack != kVsWhite && kVsBlack != kVsHold && kVsWhite != kVsHold);
  // Voltages are the vendor's.
  assert(kGateVoltage == 0x17 && kSourceVoltage[0] == 0x41 && kSourceVoltage[1] == 0xA8 &&
         kSourceVoltage[2] == 0x32 && kVcom == 0x30);

  for (Band b : {Band::Warm, Band::Cool, Band::Cold}) {
    const Timing t = timing(b);
    uint8_t turn[kLutBytes];
    build_turn(turn, t);
    for (int i = 0; i < 10; ++i)
      assert(turn[kVsOffset + 40 + i] == 0);  // VCOM LUT4 idle
    for (int i = 0; i < 5; ++i)
      assert(turn[kFrOffset + i] == kFrameRate);

    // Turn: LUT0 HOLD never drives; the targets are the stock text waveform's
    // rows, frame for frame, all driving from the start (no reset pass).
    const Seq hold = run(turn, 0), w = run(turn, 1), g = run(turn, 2), k = run(turn, 3);
    assert(count(hold, kVsBlack) == 0 && count(hold, kVsWhite) == 0);
    assert(w.len == t.turn_len() && g.len == w.len && k.len == w.len);
    for (int i = 0; i < w.len; ++i) {
      const bool wi = i >= t.white_start && i < t.white_start + t.white;
      assert(w.code[i] == (wi ? kVsWhite : kVsHold));
      assert(k.code[i] == (i < t.black ? kVsBlack : kVsHold));
      // Gray = to-black's drive (erases what was there), then pulled back.
      assert(g.code[i] == (i < t.black ? kVsBlack : i < t.black + t.gray_back ? kVsWhite : kVsHold));
    }
  }

  // The waveform picked on hardware: 13 frames at FR 0x33 (stock text halved), gray 2 back.
  assert(timing(Band::Warm).turn_len() == 13 && kFrameRate == 0x33 && kGrayBack == 2);
  assert(timing(Band::Warm).black + timing(Band::Warm).gray_back <= timing(Band::Warm).turn_len());

  // Bands only lengthen, and clamp to a byte.
  assert(timing(Band::Cool).turn_len() > timing(Band::Warm).turn_len() &&
         timing(Band::Cold).turn_len() > timing(Band::Cool).turn_len());
  assert(scaled_(250, Band::Cold) == 255);

  // Hysteresis: hovering at a boundary does not flip.
  Band b = Band::Warm;
  for (float c : {21.f, 19.5f, 18.5f, 19.f, 20.5f, 21.5f})
    b = band_for(c, b);
  assert(b == Band::Warm);
  b = band_for(17.f, b);
  assert(b == Band::Cool);
  b = band_for(21.f, b);
  assert(b == Band::Cool);
  b = band_for(22.5f, b);
  assert(b == Band::Warm);
  assert(band_for(5.f, Band::Warm) == Band::Cold);
  assert(band_for(25.f, Band::Cold) == Band::Warm);
  std::printf("ok\n");
  return 0;
}
