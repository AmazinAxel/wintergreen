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
// Prints the largest display list, which is what sets kWgbMaxPageBytes.
//
// Run with tools/tests/run.sh.
#include <cstdio>
#include <vector>

#include "content/wgb/WgbPageView.h"
#include "content/wgb/WgbReader.h"

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
  size_t max_dl = 0;
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

  std::printf("pages=%u/%u/%u/%u chapters=%u blank=%u max_dl=%zu failures=%d\n", wgb.page_count(0),
              wgb.page_count(1), wgb.page_count(2), wgb.page_count(3), wgb.chapter_count(), blank, max_dl,
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
