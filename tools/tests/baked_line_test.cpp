// A reader page drawn from its display list by GrayPage (strip by strip, the
// column copy in portrait) shows exactly the three levels of an independent
// per-pixel render of the layout engine's PageContent, the way the device drew
// before pages were baked: random words in every font, style and vertical
// alignment, rules, images (1-bit, read per strip), the low-battery bar, both
// rotations, the real two-plane reader font, baked pens and ink boxes. Drawn
// over an unknown glass, so every pixel is driven; the planes kept for the next
// turn must hold the same levels.
//
// Run with tools/tests/run.sh.
#include <cstdio>

#include "gray_sim.h"
#include "reader_font.h"

using namespace wintergreen;

int main() {
  const BitmapFontSet* fonts = books::reader_fonts();
  assert(fonts && fonts->num_fonts() >= 2);
  for (int i = 0; i < fonts->num_fonts(); ++i)
    assert(fonts->get_font(i)->planes() == 2);
  std::srand(99);
  std::vector<uint8_t> hi(DisplayFrame::kPixelBytes), lo(DisplayFrame::kPixelBytes);

  for (int iter = 0; iter < 200; ++iter) {
    const Rotation rot = iter & 1 ? Rotation::Deg0 : Rotation::Deg90;
    BitmapFontSet fset = *fonts;
    fset.set_base_size_index((iter >> 1) & 1);
    const auto page = sim::random_page(fset, rot, /*images=*/true, /*many=*/iter % 10 == 0);

    sim::Panel panel;
    sim::FakeImages images;
    GrayPage gp;
    gp.set(page->dl.data(), page->dl.size(), &fset, rot, page->bar_h, &images);
    gp.show(panel, hi.data(), lo.data());
    const sim::Levels want = sim::reference(*page, fset);
    if (panel.turns != 1 || panel.glass != want) {
      std::fprintf(stderr, "iter %d: gray page differs from the layout render\n", iter);
      return 1;
    }
    // The planes kept for the next turn hold the same levels.
    sim::Levels planes = sim::blank();
    for (int r = 0; r < sim::kRows; ++r)
      for (int b = 0; b < sim::kCols; ++b)
        planes[static_cast<size_t>(r) * sim::kCols + b] =
            sim::bit(lo.data(), r, b) ? sim::kBlack : sim::bit(hi.data(), r, b) ? sim::kGray : sim::kWhite;
    if (planes != want) {
      std::fprintf(stderr, "iter %d: kept planes differ from the glass\n", iter);
      return 1;
    }
  }
  std::printf("ok\n");
  return 0;
}
