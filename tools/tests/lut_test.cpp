// The waveform tables epd.h uploads (platforms/esp32/panel_lut.h): 105 bytes in
// the controller's layout, HOLD really idle, every target of a table ending on
// the same frame (nothing settles early and then watches the rest move), the
// gray pulse shared by both tables so boot paint and page turns show one gray,
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
  for (int g = 0; g < 10; ++g) {
    assert(lut[kTpOffset + g * 5 + 4] == 0);  // no repeats
    for (int ph = 0; ph < 4; ++ph)
      for (int f = 0; f < tp(lut, g, ph); ++f)
        s.code[s.len++] = vs(lut, n, g, ph);
  }
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
    assert(t.d >= t.p && t.p >= t.pl && t.pl >= 1 && t.c >= 1 && t.d <= 255 && t.c <= 255);
    uint8_t turn[kLutBytes], full[kLutBytes];
    build_turn(turn, t);
    build_full(full, t);
    for (int i = 0; i < 10; ++i) {
      assert(turn[kVsOffset + 40 + i] == 0);  // VCOM LUT4 idle
      assert(full[kVsOffset + 40 + i] == 0);
    }
    for (int i = 0; i < 5; ++i)
      assert(turn[kFrOffset + i] == kFrameRate && full[kFrOffset + i] == kFrameRate);

    // Turn: LUT0 HOLD never drives; the three targets all end on the last frame.
    const Seq hold = run(turn, 0), w = run(turn, 1), g = run(turn, 2), k = run(turn, 3);
    assert(count(hold, kVsBlack) == 0 && count(hold, kVsWhite) == 0);
    assert(idle_tail(w) == 0 && idle_tail(g) == 0 && idle_tail(k) == 0);
    assert(w.len == g.len && g.len == k.len && w.len == t.d);
    assert(count(w, kVsWhite) == t.d && count(k, kVsBlack) == t.d);
    // to-gray: saturate white, then exactly P frames of black at the end.
    assert(count(g, kVsBlack) == t.p && count(g, kVsBlack, g.len - t.p) == t.p);
    assert(count(g, kVsWhite) == t.d - t.p);

    // Full: clear is balanced and identical for every level; the set phase is
    // right-aligned: black D, gray P, light PL, white nothing.
    const Seq f[4] = {run(full, 0), run(full, 1), run(full, 2), run(full, 3)};
    for (int n = 0; n < 4; ++n) {
      assert(f[n].len == 2 * t.c + t.d);
      assert(count(f[n], kVsWhite) == t.c);
      for (int i = 0; i < t.c; ++i)
        assert(f[n].code[i] == kVsBlack && f[n].code[t.c + i] == kVsWhite);
    }
    const int set = 2 * t.c;
    assert(count(f[0], kVsBlack, set) == 0);
    assert(count(f[1], kVsBlack, set) == t.pl && idle_tail(f[1]) == 0);
    assert(count(f[2], kVsBlack, set) == t.p && idle_tail(f[2]) == 0);
    assert(count(f[3], kVsBlack, set) == t.d && idle_tail(f[3]) == 0);
    // Same gray: P black frames from white in both tables.
    assert(count(f[2], kVsBlack, set) == count(g, kVsBlack));
  }

  // Bands only lengthen, and clamp to a byte.
  assert(timing(Band::Cool).d > timing(Band::Warm).d && timing(Band::Cold).d > timing(Band::Cool).d);
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
