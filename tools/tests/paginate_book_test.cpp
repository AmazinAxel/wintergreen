// Paginate a real .wgb with the device's own layout engine and assert the
// invariants that matter on hardware:
//
//   * every page makes forward progress (no stall), and pagination terminates
//   * no page is blank when the chapter still has text to show
//   * paging forward then back retraces the exact pages (PageHistory)
//   * a cold backward turn (no history: after a jump or a resume) shows a page
//     that runs exactly up to the page it came from — nothing skipped, nothing
//     repeated across the boundary
//
// Forward steps and the backward resolution call the same TextLayout functions
// ReaderScreen does (next_page_start, predecessor_start), so this cannot drift
// from the device. A cold backward turn may still pick a different start than
// forward reading did — a page ending at a PageBreak or an image that did not
// fit has several valid starts — so those are counted, not failed.
//
// This is the harness for the Odyssey blank-page report — it runs the same
// TextLayout over the same file the device reads, under ASan/UBSan.
//
// Run (or use tools/tests/run.sh):
//   nix-shell -p gcc --run \
//     'g++ -std=c++17 -I . -I wintergreen -fsanitize=address,undefined \
//        -o /tmp/pb tools/tests/paginate_book_test.cpp \
//        wintergreen/content/TextLayout.cpp \
//        wintergreen/content/wgb/WgbReader.cpp \
//        wintergreen/content/hyphenation/Hyphenation.cpp && /tmp/pb <book.wgb>'
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "content/Font.h"
#include "content/PageHistory.h"
#include "content/TextLayout.h"
#include "content/wgb/WgbReader.h"

using namespace wintergreen;

// ReaderScreen::kPrevPageProbeLimit.
static constexpr int kProbeLimit = 64;

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

  // The device's portrait geometry, close enough for pagination shape.
  FixedFont font(8, 32);
  PageOptions opts;
  opts.width = 480;
  opts.height = 793;
  opts.padding_left = opts.padding_right = 20;
  opts.padding_top = opts.padding_bottom = 20;
  opts.para_spacing = 0;

  size_t total_pages = 0, blank_pages = 0, stalls = 0;
  size_t history_misses = 0, seams = 0, line_off = 0, para_off = 0;

  for (uint16_t ch = 0; ch < wgb.chapter_count(); ++ch) {
    WgbChapterSource src(wgb, ch);
    TextLayout layout;
    layout.set_source(src);
    layout.set_font(font);
    layout.set_options(opts);

    // Forward, exactly as ReaderScreen::next_page_ + render_page_ do it.
    std::vector<PagePosition> starts;
    PageHistory history;
    PagePosition pos{0, 0, 0};
    int guard = 0;
    while (true) {
      if (++guard > 5000) {
        std::fprintf(stderr, "ch %u: pagination did not terminate\n", ch);
        return 1;
      }
      PagePosition resolved = layout.resolve_stable_position(pos);
      layout.set_position(resolved);
      PageContent pc = layout.layout();
      starts.push_back(resolved);
      ++total_pages;

      // Blank = nothing drawn at all. A page of only an image is not blank —
      // every book here opens on a full-page cover, which is exactly that.
      if (pc.items.empty())
        ++blank_pages;

      if (pc.at_chapter_end)
        break;
      const PagePosition next = layout.next_page_start(resolved, pc.end);
      if (!(resolved < next)) {
        std::fprintf(stderr, "ch %u: page stalled at para %u off %u\n", ch, resolved.paragraph, resolved.offset);
        ++stalls;
        break;
      }
      history.push(layout.epoch(), resolved, next);
      pos = next;
    }

    // Back again with the history: must retrace the forward pages exactly, for
    // as many turns as the ring holds.
    const size_t retrace = std::min<size_t>(PageHistory::kCapacity, starts.size() - 1);
    for (size_t k = 1; k <= retrace; ++k) {
      const size_t i = starts.size() - k;
      PagePosition got;
      if (!history.pop_to(layout.epoch(), starts[i], &got) || !(got == starts[i - 1])) {
        if (history_misses < 8)
          std::fprintf(stderr, "ch %u: history did not retrace (%u,%u)\n", ch, starts[i].paragraph,
                       starts[i].offset);
        ++history_misses;
        break;
      }
    }

    // Cold backward turns from every page, as ReaderScreen::prev_page_ does
    // without history.
    for (size_t i = starts.size(); i-- > 1;) {
      const PagePosition cur = starts[i];
      const PagePosition end = layout.snap_to_image_end(cur);
      layout.set_position(end);
      PageContent shown = layout.layout_backward();
      const PagePosition got = layout.predecessor_start(cur, shown.start, kProbeLimit);
      if (!(got == shown.start)) {
        layout.set_position(got);
        shown = layout.layout();
      }

      // The page shown must lead straight back to the one we left.
      const bool seamless =
          got == shown.start && (shown.end == end || layout.next_page_start(got, shown.end) == cur);
      if (!seamless) {
        if (seams < 8)
          std::fprintf(stderr, "ch %u: back from (%u,%u) shows (%u,%u)..(%u,%u), not up to it\n", ch,
                       cur.paragraph, cur.offset, got.paragraph, got.offset, shown.end.paragraph, shown.end.offset);
        ++seams;
      }

      if (!(got == starts[i - 1])) {
        if (got.paragraph == starts[i - 1].paragraph)
          ++line_off;
        else
          ++para_off;
      }
    }
  }

  std::printf("chapters=%u pages=%zu blank=%zu stalls=%zu history_misses=%zu seams=%zu "
              "cold_line_off=%zu cold_para_off=%zu\n",
              wgb.chapter_count(), total_pages, blank_pages, stalls, history_misses, seams, line_off, para_off);
  assert(stalls == 0 && "a page consumed nothing");
  assert(blank_pages == 0 && "a page rendered nothing at all");
  assert(history_misses == 0 && "paging back did not retrace the pages read forward");
  assert(seams == 0 && "a backward page does not run up to the page it came from");
  return (stalls || blank_pages || history_misses || seams) ? 1 : 0;
}
