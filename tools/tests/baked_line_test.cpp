// A page recorded from its display list (DrawBuffer::draw_baked_line line ops
// over the list in the tail, and ReaderScreen's rule draw) renders through the
// strip renderer pixel-identical to the same page drawn straight from the
// layout engine's PageContent as one text op per word, the way the device drew
// before pages were fully baked. Random words in every font, style and vertical
// alignment, plus rules, in both rotations, with the real reader font (two
// planes, three levels).
//
// Run with tools/tests/run.sh.
#include <algorithm>
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
#include "sim_panel.h"

using namespace wintergreen;

namespace {


// The pre-bake draw: ReaderScreen::draw_page_ + DrawBuffer::draw_layout_line.
//
// One text item (line) per call: a page of per-word text ops would not fit one
// draw list, so the expected page is composited from the lines (black ink is a
// max) with the rules (`line` = -1, drawn black here) laid over in gray.
void draw_reference(DrawBuffer& buf, const PageContent& page, const BitmapFontSet& fonts, int line) {
  buf.fill(Level::White);
  int n = -1;
  for (const auto& ci : page.items) {
    const PageTextItem* item = std::get_if<PageTextItem>(&ci);
    if (!item || ++n != line)
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
      buf.draw_text(kReaderHPadding + w.x, b, w.text, w.len, *f, Ink::Black, w.style);
    }
  }
  for (const auto& ci : page.items) {
    const PageHrItem* h = std::get_if<PageHrItem>(&ci);
    if (!h || line >= 0)
      continue;
    const int hr_y = static_cast<int>(h->y_offset) + static_cast<int>(h->height) / 2;
    buf.fill_rect(static_cast<int>(h->x_offset), hr_y - 1, static_cast<int>(h->width), 2, Level::Black);
  }
}

// ReaderScreen::draw_page_ without images.
void draw_baked(DrawBuffer& buf, const std::vector<uint8_t>& dl, const BitmapFontSet& fonts) {
  uint8_t* tail = buf.claim_tail(dl.size(), 7);
  assert(tail);
  std::memcpy(tail, dl.data(), dl.size());
  buf.fill(Level::White);
  WgbPageView v(tail, dl.size());
  uint8_t tag;
  while (v.peek(tag)) {
    if (tag == kWgbItemLine) {
      WgbLine line;
      assert(v.read_line(line));
      buf.draw_baked_line(line, v.end(), fonts);
    } else {
      WgbRule r;
      assert(v.read_rule(r));
      buf.fill_rect(r.x, r.y + r.h / 2 - 1, r.w, 2, Level::Gray);
    }
  }
  assert(v.at_end());
}

}  // namespace

int main() {
  const BitmapFontSet* fonts = books::reader_fonts();
  assert(fonts && fonts->num_fonts() >= 2);
  simpanel::SimPanel disp;
  DrawBuffer buf(disp);
  std::srand(99);

  static const char* kWords[] = {"The",   "quick", "\xE2\x80\x9C" "brown\xE2\x80\x9D", "fox",  "-", "AV",
                                 "Wa.",   "jumps", "caf\xC3\xA9",                       "over", "x", "lazy,"};
  // A long word takes the u16 length escape.
  std::string long_word(300, 'm');
  const uint8_t sizes[] = {100, 80, 120, 60, 150};

  int skipped = 0;
  for (int iter = 0; iter < 120; ++iter) {
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
      // Random pages can be denser than kWgbMaxPageBytes allows (real pages
      // are under a third of it); the bake refuses those, so skip them.
      if (err.find("kWgbMaxPageBytes") != std::string::npos) {
        ++skipped;
        continue;
      }
      std::fprintf(stderr, "iter %d: %s\n", iter, err.c_str());
      return 1;
    }
    std::vector<uint8_t> want(static_cast<size_t>(simpanel::kRows) * simpanel::kBits, 0);
    for (int l = 0; l < lines; ++l) {
      draw_reference(buf, pc, fset, l);
      assert(!buf.current_stats().overflowed);
      const std::vector<uint8_t> one = simpanel::render_levels(buf);
      for (size_t q = 0; q < want.size(); ++q)
        want[q] = std::max(want[q], one[q]);
    }
    draw_reference(buf, pc, fset, -1);
    const std::vector<uint8_t> rules = simpanel::render_levels(buf);
    for (size_t q = 0; q < want.size(); ++q)
      if (rules[q] == 3)
        want[q] = 2;
    // Keep the glass in step with the list (the reference screens above were
    // never refreshed).
    buf.fill(Level::White);
    buf.refresh();
    draw_baked(buf, dl, fset);
    const std::vector<uint8_t> got = simpanel::render_levels(buf);
    // And on the glass, through a turn after the reference.
    buf.refresh();
    if (disp.glass != got) {
      std::fprintf(stderr, "iter %d: glass differs from the baked page\n", iter);
      return 1;
    }
    if (want != got) {
      std::fprintf(stderr, "iter %d: baked page differs from the layout draw\n", iter);
      return 1;
    }
  }
  assert(skipped < 30);
  std::printf("ok (%d over the cap skipped)\n", skipped);
  return 0;
}
