// A page drawn from its display list (DrawBuffer::draw_baked_line and
// ReaderScreen's rule draw) is pixel-identical to the same page drawn straight
// from the layout engine's PageContent, the way the device drew before pages
// were fully baked. Random words in every font, style and vertical alignment,
// plus rules, in both rotations, with the real reader font.
//
// Run with tools/tests/run.sh.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "content/PageGeometry.h"
#include "content/wgb/WgbPageView.h"
#include "content/wgb/WgbPaginator.h"
#include "display/DrawBuffer.h"
#include "reader_font.h"

using namespace wintergreen;

namespace {

struct CaptureDisplay : IDisplay {
  const uint8_t* last = nullptr;
  void full_refresh(const uint8_t* pixels, const uint8_t*, RefreshMode, bool) override { last = pixels; }
  void partial_refresh(const uint8_t* pixels) override { last = pixels; }
};

std::vector<uint8_t> snapshot(DrawBuffer& buf, CaptureDisplay& disp) {
  buf.refresh();
  assert(disp.last);
  return std::vector<uint8_t>(disp.last, disp.last + DrawBuffer::kBufSize);
}

// The pre-bake draw: ReaderScreen::draw_page_ + DrawBuffer::draw_layout_line.
void draw_reference(DrawBuffer& buf, const PageContent& page, const BitmapFontSet& fonts) {
  buf.fill(true);
  for (const auto& ci : page.items) {
    const PageTextItem* item = std::get_if<PageTextItem>(&ci);
    if (!item)
      continue;
    const int baseline_y = static_cast<int>(item->y_offset) + item->baseline;
    for (const LayoutWord& w : item->line.words) {
      if (w.len == 0)
        continue;
      const BitmapFont* f = fonts.get(w.size_pct);
      int b = baseline_y;
      if (w.vertical_align == VerticalAlign::Super)
        b -= static_cast<int>(fonts.y_advance(w.size_pct)) * 20 / 100;
      else if (w.vertical_align == VerticalAlign::Sub)
        b += static_cast<int>(fonts.y_advance(w.size_pct)) * 20 / 100;
      buf.draw_text_proportional(kReaderHPadding + w.x, b, w.text, w.len, *f, false, w.style);
    }
  }
  for (const auto& ci : page.items) {
    const PageHrItem* h = std::get_if<PageHrItem>(&ci);
    if (!h)
      continue;
    const int hr_y = static_cast<int>(h->y_offset) + static_cast<int>(h->height) / 2;
    buf.fill_rect(static_cast<int>(h->x_offset), hr_y - 1, static_cast<int>(h->width), 2, false);
  }
}

// ReaderScreen::draw_page_ without images.
void draw_baked(DrawBuffer& buf, const std::vector<uint8_t>& dl, const BitmapFontSet& fonts) {
  buf.fill(true);
  WgbPageView v(dl.data(), dl.size());
  uint8_t tag;
  while (v.peek(tag)) {
    if (tag == kWgbItemLine) {
      WgbLine line;
      assert(v.read_line(line));
      buf.draw_baked_line(line, v.end(), fonts, false);
    } else {
      WgbRule r;
      assert(v.read_rule(r));
      buf.fill_rect(r.x, r.y + r.h / 2 - 1, r.w, 2, false);
    }
  }
  assert(v.at_end());
}

}  // namespace

int main() {
  const BitmapFontSet* fonts = books::reader_fonts();
  assert(fonts && fonts->num_fonts() >= 2);
  CaptureDisplay disp;
  DrawBuffer buf(disp);
  std::srand(99);

  static const char* kWords[] = {"The",   "quick", "\xE2\x80\x9C" "brown\xE2\x80\x9D", "fox",  "-", "AV",
                                 "Wa.",   "jumps", "caf\xC3\xA9",                       "over", "x", "lazy,"};
  // A long word takes the u16 length escape.
  std::string long_word(300, 'm');
  const uint8_t sizes[] = {100, 80, 120, 60, 150};

  for (int iter = 0; iter < 200; ++iter) {
    const bool landscape = iter & 1;
    buf.set_rotation(landscape ? Rotation::Deg0 : Rotation::Deg90);
    BitmapFontSet fset = *fonts;
    fset.set_base_size_index((iter >> 1) & 1);

    PageContent pc;
    const int lines = 1 + std::rand() % 20;
    std::vector<std::vector<LayoutWord>> words(static_cast<size_t>(lines));
    for (int l = 0; l < lines; ++l) {
      // Large first x and gaps take the absolute-x escape; plain lines leave
      // the flags byte out and inherit the previous word's (across lines too).
      int x = std::rand() % 8 == 0 ? 300 : std::rand() % 20;
      const bool plain = std::rand() % 2;
      const int n = std::rand() % 12;
      for (int k = 0; k < n; ++k) {
        const bool lw = std::rand() % 50 == 0;
        const char* t = lw ? long_word.c_str() : kWords[std::rand() % (sizeof(kWords) / sizeof(*kWords))];
        LayoutWord w{t,
                     static_cast<uint16_t>(lw ? long_word.size() : std::strlen(t)),
                     static_cast<uint16_t>(x),
                     plain ? FontStyle::Regular : static_cast<FontStyle>(std::rand() % 4),
                     plain ? uint8_t{100} : sizes[std::rand() % 5],
                     plain ? VerticalAlign::Baseline : static_cast<VerticalAlign>(std::rand() % 3),
                     false};
        words[l].push_back(w);
        x += std::rand() % 10 == 0 ? 254 + std::rand() % 200 : 10 + std::rand() % 60;
      }
      if (std::rand() % 8 == 0)
        words[l].push_back(LayoutWord{"", 0, 5, FontStyle::Regular, 100, VerticalAlign::Baseline, false});
    }
    for (int l = 0; l < lines; ++l) {
      PageTextItem ti{};
      ti.line.words.data_ = words[l].data();
      ti.line.words.size_ = static_cast<uint16_t>(words[l].size());
      ti.y_offset = static_cast<uint16_t>(4 + l * 36);
      ti.height = 36;
      ti.baseline = 28;
      pc.items.emplace_back(ti);
      if (std::rand() % 6 == 0)
        pc.items.emplace_back(PageHrItem{static_cast<uint16_t>(std::rand() % 100), static_cast<uint16_t>(l * 36),
                                         static_cast<uint16_t>(50 + std::rand() % 300), 36});
    }

    std::vector<uint8_t> dl;
    std::string err;
    if (!serialize_wgb_page(pc, fset, 0, dl, err)) {
      std::fprintf(stderr, "iter %d: %s\n", iter, err.c_str());
      return 1;
    }
    draw_reference(buf, pc, fset);
    const std::vector<uint8_t> want = snapshot(buf, disp);
    draw_baked(buf, dl, fset);
    const std::vector<uint8_t> got = snapshot(buf, disp);
    if (want != got) {
      std::fprintf(stderr, "iter %d: baked page differs from the layout draw\n", iter);
      return 1;
    }
  }
  std::printf("ok\n");
  return 0;
}
