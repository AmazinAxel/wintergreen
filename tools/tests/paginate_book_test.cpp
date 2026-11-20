// Check a baked .wgb's page section, every page of every layout:
//
//   * each display list reads, fits kWgbMaxPageBytes, parses to its exact end,
//     and names only images in the table
//   * no page is blank
//   * records strictly advance, and find_page() maps each record's own position
//     back to it (what font/rotation changes and book.pos rely on)
//   * chapter pages are in order and in range
//
// That each page ends where the next begins is guaranteed by construction (the
// paginator records the layout engine's own next_page_start), and that a
// display list draws what the layout engine laid out is baked_line_test's job.
//
// Prints the largest display list, which is what sets kWgbMaxPageBytes, and the
// most of a DrawBuffer list any page needs (ops + display list in the tail),
// recorded exactly as ReaderScreen records it; every page must fit, with room
// left for the quick menu to park it.
//
// Run with tools/tests/run.sh.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "content/wgb/WgbPageView.h"
#include "content/wgb/WgbReader.h"
#include "display/DrawBuffer.h"
#include "reader_font.h"

using namespace wintergreen;

namespace {

int g_failures = 0;

void fail(uint8_t l, uint32_t i, const char* what) {
  if (g_failures < 20)
    std::fprintf(stderr, "layout %u page %u: %s\n", l, i, what);
  ++g_failures;
}

bool key_less(const WgbPage& a, const WgbPage& b) {
  if (a.para_offset != b.para_offset)
    return a.para_offset < b.para_offset;
  if (a.text_offset != b.text_offset)
    return a.text_offset < b.text_offset;
  return a.offset < b.offset;
}

}  // namespace

struct NullDisplay : IDisplay {
  void begin_update(UpdateKind) override {}
  void await_strip_slot() override {}
  void write_window(int, int, int, int, const uint8_t*, const uint8_t*) override {}
  void finish_writes() override {}
  void fire(UpdateKind, bool) override {}
};
struct NoPixels : PixelSource {
  bool read(uint32_t, uint32_t, uint8_t*, size_t) override { return false; }
};

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

  std::vector<uint8_t> buf(kWgbMaxPageBytes);
  uint32_t blank = 0;
  size_t max_dl = 0, max_list = 0;
  const BitmapFontSet* fonts = books::reader_fonts();
  NullDisplay disp;
  DrawBuffer draw(disp);
  NoPixels nopix;
  for (uint16_t k = 0; k < wgb.image_count(); ++k) {
    WgbImageRef ref;
    if (!wgb.image_ref(k, ref))
      fail(0, k, "image ref unreadable");
    else if (ref.data_size != 0 && ref.data_size != wgb_image_bytes(ref.width, ref.height))
      fail(0, k, "image data is not the two-plane rows + cols copies");
  }
  for (uint8_t l = 0; l < kWgbLayouts; ++l) {
    const uint32_t n = wgb.page_count(l);
    if (n == 0)
      fail(l, 0, "no pages");
    WgbPage prev{};
    for (uint32_t i = 0; i < n; ++i) {
      WgbPage rec;
      if (!wgb.page(l, i, rec)) {
        fail(l, i, "record unreadable");
        break;
      }
      if (i > 0 && !key_less(prev, rec))
        fail(l, i, "record does not advance");
      prev = rec;
      if (rec.dl_size > max_dl)
        max_dl = rec.dl_size;
      if (!wgb.read_page(rec, buf.data(), buf.size())) {
        fail(l, i, "display list unreadable or larger than kWgbMaxPageBytes");
        continue;
      }
      WgbPageView v(buf.data(), rec.dl_size);
      uint8_t tag;
      uint32_t items = 0;
      while (v.peek(tag)) {
        if (tag == kWgbItemImage) {
          WgbImage img;
          if (v.read_image(img) && img.key >= wgb.image_count())
            fail(l, i, "image key past the table");
        } else {
          v.skip();
        }
        ++items;
      }
      if (!v.at_end())
        fail(l, i, "display list does not parse to its end");

      // Record it as ReaderScreen::draw_page_ does (rotation by layout).
      draw.set_rotation((l & 1) ? Rotation::Deg0 : Rotation::Deg90);
      uint8_t* tail = draw.claim_tail(rec.dl_size, 1 + i);
      if (!tail) {
        fail(l, i, "display list does not fit a draw list");
      } else {
        std::memcpy(tail, buf.data(), rec.dl_size);
        draw.fill(Level::White);
        WgbPageView dv(tail, rec.dl_size);
        while (dv.peek(tag)) {
          if (tag == kWgbItemLine) {
            WgbLine line;
            dv.read_line(line);
            draw.draw_baked_line(line, dv.end(), *fonts);
          } else if (tag == kWgbItemRule) {
            WgbRule r;
            dv.read_rule(r);
            draw.fill_rect(r.x, r.y + r.h / 2 - 1, r.w, 2, Level::Gray);
          } else {
            WgbImage img;
            dv.read_image(img);
            draw.blit(&nopix, img.key, 0, 0, (l & 1) ? PixelLayout::Rows : PixelLayout::Cols, img.x,
                      img.y - img.src_y, img.w, img.full_h, img.src_y, img.src_y + img.clip_h);
          }
        }
        draw.fill_rect(0, 0, 10, 10, Level::Gray);  // the low-battery bar
        const DrawBuffer::Stats st = draw.current_stats();
        if (st.overflowed)
          fail(l, i, "page overflows a draw list");
        max_list = std::max<size_t>(max_list, st.ops_bytes + st.tail_bytes);
      }
      if (items == 0) {
        ++blank;
        fail(l, i, "blank page");
      }
      // Several pages can share a text position (slices of one image); the
      // last of them is the one found, never an earlier page.
      if (wgb.find_page(l, rec.para_offset, rec.text_offset) < i)
        fail(l, i, "find_page lands before this page");
    }

    std::vector<uint32_t> chapters;
    if (!wgb.chapter_pages(l, chapters) || chapters.size() != wgb.chapter_count())
      fail(l, 0, "chapter pages unreadable");
    for (size_t c = 0; c < chapters.size(); ++c) {
      if (chapters[c] >= n || (c > 0 && chapters[c] < chapters[c - 1]))
        fail(l, chapters[c], "chapter page out of order or range");
    }
  }

  std::printf("pages=%u/%u/%u/%u chapters=%u blank=%u max_dl=%zu max_list=%zu/%zu failures=%d\n",
              wgb.page_count(0), wgb.page_count(1), wgb.page_count(2), wgb.page_count(3), wgb.chapter_count(), blank,
              max_dl, max_list, DrawBuffer::kArenaBytes, g_failures);
  return g_failures == 0 ? 0 : 1;
}
