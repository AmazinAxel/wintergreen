// Peak heap for laying out one page, measured over every page of a real book.
//
// This is a budget test, not a correctness test. The device has ~165 KB of free
// heap with no radio and roughly 13 KB with the BLE clicker connected, and
// exceptions are off, so a std::bad_alloc inside layout is an abort() — the
// device reboots mid-book with no message. That is what "reading the Odyssey's
// first preface with the clicker attached reboots the device" was.
//
// The Odyssey's chapter 2 (PREFACE TO FIRST EDITION) holds the largest paragraph
// in the sample: 3672 bytes over 116 lines. Laying out the page containing it
// peaked at 45,712 bytes before the fixes below and 25,727 after:
//
//   * layout_para_lines() reserves `lines` from the paragraph's byte count and
//     the font's own average advance, instead of a fixed 12, so the vector does
//     not double 12 -> 24 -> 48 -> 96 while move-constructing a heap-owning
//     LayoutLine each time.
//   * every finished line's word vector is shrunk to its real size, instead of
//     keeping the reserve(16) it was built with. That reserve is 16 words per
//     line whether the line holds 3 or 12, and it is retained for the life of
//     the paragraph.
//
// Run:
//   nix-shell -p gcc --run \
//     'g++ -std=c++17 -O2 -I . -I wintergreen \
//        -o /tmp/lm tools/tests/layout_memory_test.cpp \
//        wintergreen/content/TextLayout.cpp \
//        wintergreen/content/wgb/WgbReader.cpp \
//        wintergreen/content/hyphenation/Hyphenation.cpp && /tmp/lm <book.wgb>'
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>

// Instrumented allocator. Must be defined before the headers that allocate.
namespace {
size_t g_live = 0;
size_t g_peak = 0;
}  // namespace

void* operator new(size_t n) {
  void* p = std::malloc(n + 16);
  if (!p)
    throw std::bad_alloc();
  *static_cast<size_t*>(p) = n;
  g_live += n;
  if (g_live > g_peak)
    g_peak = g_live;
  return static_cast<char*>(p) + 16;
}
void operator delete(void* p) noexcept {
  if (!p)
    return;
  char* b = static_cast<char*>(p) - 16;
  g_live -= *reinterpret_cast<size_t*>(b);
  std::free(b);
}
void operator delete(void* p, size_t) noexcept {
  operator delete(p);
}

#include "content/Font.h"
#include "content/TextLayout.h"
#include "content/wgb/WgbReader.h"

using namespace wintergreen;

namespace {

// Stands in for BitmapFont: widths vary per character, so line breaking lands
// where it does on the device. A FixedFont makes every line the same length and
// hides the estimate's error.
struct PropFont : IFont {
  uint16_t char_width(char32_t c, FontStyle, uint8_t) const override {
    if (c == 'i' || c == 'l' || c == '.' || c == ',' || c == '\'')
      return 5;
    if (c == 'm' || c == 'w' || c == 'M' || c == 'W')
      return 20;
    if (c == ' ')
      return 8;
    return 12;
  }
  uint16_t word_width(const char* t, size_t n, FontStyle s, uint8_t p) const override {
    uint16_t x = 0;
    for (size_t i = 0; i < n;) {
      unsigned char b = static_cast<unsigned char>(t[i]);
      size_t adv = b < 0x80 ? 1 : (b < 0xE0 ? 2 : (b < 0xF0 ? 3 : 4));
      x += char_width(b < 0x80 ? b : 'n', s, p);
      i += adv;
    }
    return x;
  }
  uint16_t y_advance(uint8_t) const override {
    return 32;
  }
  uint16_t baseline(uint8_t) const override {
    return 24;
  }
};

// Ceiling for one page's layout, over a book converted by the *current*
// converter.
//
// **This measures the delta inside one layout() call, not what the reader holds
// in total.** The bigger number is the chapter source's 32-paragraph window plus
// the paragraph cache plus the page, all live at once — 33 KB at worst on the
// Odyssey, against a heap the BLE stack has already taken its share of. That
// total is what actually aborts the device, and it is not what this asserts;
// this only catches a regression in the per-page allocation.
//
// **A book converted by an older converter will exceed this**, because it still
// holds the oversized paragraphs. The fix is in the converter, so the assertion
// is about converter output and an old file has to be re-converted to benefit.
constexpr size_t kMaxPageLayoutBytes = 24 * 1024;

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <book.wgb>\n", argv[0]);
    return 2;
  }

  WgbReader wgb;
  if (!wgb.open(argv[1])) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }

  PropFont font;
  PageOptions opts;
  opts.width = 480;
  opts.height = 793;
  opts.padding_left = opts.padding_right = 20;
  opts.padding_top = opts.padding_bottom = 20;
  opts.para_spacing = 0;

  size_t worst = 0;
  uint16_t worst_ch = 0;
  PagePosition worst_pos;

  for (uint16_t ch = 0; ch < wgb.chapter_count(); ++ch) {
    WgbChapterSource src(wgb, ch);
    TextLayout layout;
    layout.set_source(src);
    layout.set_font(font);
    layout.set_options(opts);

    PagePosition pos{0, 0, 0};
    for (int guard = 0; guard < 20000; ++guard) {
      PagePosition resolved = layout.resolve_stable_position(pos);
      layout.set_position(resolved);

      const size_t base = g_live;
      g_peak = g_live;
      PageContent pc = layout.layout();
      const size_t peak = g_peak - base;

      if (peak > worst) {
        worst = peak;
        worst_ch = ch;
        worst_pos = resolved;
      }

      if (pc.at_chapter_end)
        break;
      if (pc.end == resolved)
        break;
      pos = pc.end;
    }
  }

  std::printf("worst page layout peak = %zu B (ch %u, para %u off %u), budget %zu B\n", worst, worst_ch,
              worst_pos.paragraph, worst_pos.offset, kMaxPageLayoutBytes);
  assert(worst <= kMaxPageLayoutBytes && "one page's layout exceeded the heap budget");
  return worst <= kMaxPageLayoutBytes ? 0 : 1;
}
