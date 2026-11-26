// The reader bundle in the two-plane format (the UI headers stay 1-bit and are
// covered by blit_test): every glyph's black plane inside its gray plane, and
// its column-major copy exactly the transpose of its rows (GrayPage's portrait
// path draws only the copy, landscape only the rows).
#include <cassert>
#include <cstdio>

#include "content/BitmapFont.h"
#include "reader_font.h"

using namespace wintergreen;

static int check(const BitmapFont& f, const char* name) {
  assert(f.valid());
  int glyphs = 0;
  for (int s = 0; s < 4; ++s) {
    const BitmapFont::Styled st = f.styled(static_cast<FontStyle>(s));
    for (int idx = 0; idx < f.num_glyphs(); ++idx) {
      const GlyphData g = st.glyph_data_at(idx);
      if (!g.bits)
        continue;
      assert(g.cols);
      ++glyphs;
      const int w = g.bitmap_width, h = g.bitmap_height;
      const int rs = (w + 7) / 8, cs = (h + 7) / 8;
      for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c) {
          const int hi = (g.bits[r * rs + c / 8] >> (7 - c % 8)) & 1;
          const int lo = (g.bits[rs * h + r * rs + c / 8] >> (7 - c % 8)) & 1;
          const int chi = (g.cols[c * cs + r / 8] >> (7 - r % 8)) & 1;
          const int clo = (g.cols[cs * w + c * cs + r / 8] >> (7 - r % 8)) & 1;
          if ((lo && !hi) || chi != hi || clo != lo) {
            std::fprintf(stderr, "%s style %d glyph %d bad at %d,%d\n", name, s, idx, c, r);
            return 1;
          }
        }
    }
  }
  std::printf("%s: %d glyphs\n", name, glyphs);
  return 0;
}

int main() {
  int bad = 0;
  const BitmapFontSet* set = books::reader_fonts();
  assert(set && set->num_fonts() == 2);
  for (int i = 0; i < set->num_fonts(); ++i) {
    assert(set->get_font(i)->planes() == 2);
    bad |= check(*set->get_font(i), "reader");
  }
  if (!bad)
    std::printf("ok\n");
  return bad;
}
