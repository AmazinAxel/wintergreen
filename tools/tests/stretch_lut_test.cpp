// Self-check for build_stretch_lut. Build:
//   c++ -std=c++17 -I. -fsanitize=address,undefined \
//     tools/tests/stretch_lut_test.cpp wintergreen/content/ImageDecoder.cpp -o /tmp/slt
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "wintergreen/content/ImageDecoder.h"

using wintergreen::build_stretch_lut;

// Fill bins[lo..hi] uniformly.
static void band(uint32_t hist[256], int lo, int hi, uint32_t per_bin) {
  std::memset(hist, 0, sizeof(uint32_t) * 256);
  for (int i = lo; i <= hi; ++i) hist[i] = per_bin;
}

int main() {
  uint32_t hist[256];
  uint8_t lut[256];

  // Mid-tone-only image (the alice case): must stretch, and must map the
  // measured endpoints to full black and full white.
  band(hist, 77, 153, 100);
  assert(build_stretch_lut(hist, 0.005, lut));
  assert(lut[77] == 0);
  assert(lut[153] == 255);
  assert(lut[115] > 100 && lut[115] < 155);  // midpoint stays mid
  // Monotonic.
  for (int i = 1; i < 256; ++i) assert(lut[i] >= lut[i - 1]);

  // Already full-range: declines, leaving the caller on the identity path.
  band(hist, 0, 255, 100);
  assert(!build_stretch_lut(hist, 0.005, lut));

  // Nearly full range still declines (lo<16 && hi>239).
  band(hist, 10, 245, 100);
  assert(!build_stretch_lut(hist, 0.005, lut));

  // Empty histogram: no division by zero, declines.
  std::memset(hist, 0, sizeof(hist));
  assert(!build_stretch_lut(hist, 0.005, lut));

  // Single occupied bin: hi == lo, declines rather than dividing by a zero span.
  std::memset(hist, 0, sizeof(hist));
  hist[128] = 5000;
  assert(!build_stretch_lut(hist, 0.005, lut));

  // Clipping: a few stray pixels at each extreme must not pin the range open.
  band(hist, 100, 140, 1000);
  hist[0] = 1;
  hist[255] = 1;
  assert(build_stretch_lut(hist, 0.005, lut));
  assert(lut[100] == 0 && lut[140] == 255);

  // clip_frac 0 keeps the outliers, so the range spans and it declines.
  band(hist, 100, 140, 1000);
  hist[0] = 1;
  hist[255] = 1;
  assert(!build_stretch_lut(hist, 0.0, lut));

  std::printf("stretch_lut_test: ok\n");
  return 0;
}
