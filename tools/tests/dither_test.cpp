// The converter's blue-noise quantiser (ImageDecoder.h): flat input at a
// level's own tone stays exactly that level, every flat tone in between keeps
// its mean (within a few percent over one 64 x 64 tile), only the two levels
// around a tone appear, light only in 4-level output, and the packers put each
// level's bits in the right plane (black inside gray).
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include "content/ImageDecoder.h"

using namespace wintergreen;

namespace {

float level_lum(int l) {
  return l == 0 ? 1.f : l == 1 ? kLightLevel : l == 2 ? kGrayLevel : 0.f;
}

}  // namespace

int main() {
  for (int levels : {3, 4}) {
    // Exact levels stay exact.
    for (int l : {0, 1, 2, 3}) {
      if (l == 1 && levels == 3)
        continue;
      const auto g = static_cast<uint8_t>(std::lround(level_lum(l) * 255.f));
      for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
          assert(quantise_level(g, x, y, levels) == l);
    }
    // Every tone: mean preserved, only neighbouring levels used.
    for (int g = 0; g < 256; ++g) {
      double sum = 0;
      int used[4] = {};
      for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
          const int l = quantise_level(static_cast<uint8_t>(g), x, y, levels);
          ++used[l];
          sum += level_lum(l);
        }
      assert(levels == 4 || used[1] == 0);
      int kinds = 0;
      for (int u : used)
        kinds += u > 0;
      assert(kinds <= 2);
      const double mean = sum / (64 * 64);
      if (std::fabs(mean - g / 255.0) > 0.01) {
        std::fprintf(stderr, "levels %d gray %d: mean %.3f\n", levels, g, mean);
        return 1;
      }
    }
  }
  // Packers: planes agree with quantise_level, lo inside hi.
  const int w = 37, h = 23;
  std::vector<uint8_t> gray(w * h);
  for (int i = 0; i < w * h; ++i)
    gray[i] = static_cast<uint8_t>((i * 97) & 0xFF);
  std::vector<uint8_t> rows(2 * ((w + 7) / 8) * h), cols(2 * ((h + 7) / 8) * w);
  pack_rows(gray.data(), w, h, 3, rows.data());
  pack_cols(gray.data(), w, h, 3, cols.data());
  const int rb = (w + 7) / 8, cb = (h + 7) / 8;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const int l = quantise_level(gray[y * w + x], x, y, 3);
      const int rh = (rows[2 * rb * y + x / 8] >> (7 - x % 8)) & 1, rl = (rows[2 * rb * y + rb + x / 8] >> (7 - x % 8)) & 1;
      const int ch = (cols[2 * cb * x + y / 8] >> (7 - y % 8)) & 1, cl = (cols[2 * cb * x + cb + y / 8] >> (7 - y % 8)) & 1;
      assert(((rh << 1) | rl) == l && ((ch << 1) | cl) == l);
      assert(!rl || rh);
    }
  std::printf("ok\n");
  return 0;
}
