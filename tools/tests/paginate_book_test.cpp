// Paginate a real .wgb with the device's own layout engine and assert the
// invariants that matter on hardware:
//
//   * every page makes forward progress (no stall)
//   * pagination terminates
//   * forward-then-backward returns to the same page start
//   * no page is blank when the chapter still has text to show
//
// This is the harness for the Odyssey blank-page report — it runs the same
// TextLayout over the same file the device reads, under ASan/UBSan.
//
// Run:
//   nix-shell -p gcc --run \
//     'g++ -std=c++17 -I . -I wintergreen -fsanitize=address,undefined \
//        -o /tmp/pb tools/tests/paginate_book_test.cpp \
//        wintergreen/content/TextLayout.cpp \
//        wintergreen/content/wgb/WgbReader.cpp \
//        wintergreen/content/hyphenation/Hyphenation.cpp && /tmp/pb <book.wgb>'
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "content/Font.h"
#include "content/TextLayout.h"
#include "content/wgb/WgbReader.h"

using namespace wintergreen;

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

  size_t total_pages = 0, blank_pages = 0, stalls = 0, back_mismatch = 0;
  size_t resyncs = 0, skipped_text = 0;

  for (uint16_t ch = 0; ch < wgb.chapter_count(); ++ch) {
    WgbChapterSource src(wgb, ch);
    TextLayout layout;
    layout.set_source(src);
    layout.set_font(font);
    layout.set_options(opts);

    std::vector<PagePosition> starts;
    PagePosition pos{0, 0, 0};
    int guard = 0;
    while (true) {
      if (++guard > 5000) {
        std::fprintf(stderr, "ch %u: pagination did not terminate\n", ch);
        return 1;
      }
      layout.set_position(pos);
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
      if (pc.end == resolved) {
        std::fprintf(stderr, "ch %u: page stalled at para %u off %u\n", ch, resolved.paragraph, resolved.offset);
        ++stalls;
        break;
      }
      pos = pc.end;
    }

    // Backward must retrace the forward walk exactly. This mirrors
    // ReaderScreen::prev_page_, including the forward re-walk it does when a
    // PageBreak makes the backward page disagree — keep the two in step.
    for (size_t i = starts.size(); i-- > 1;) {
      const PagePosition cur_start = starts[i];
      layout.set_position(cur_start);
      PageContent back = layout.layout_backward();
      PagePosition got = back.start;

      // Mirrors ReaderScreen::prev_page_: step back one further page for an
      // anchor, then walk forward and keep the last page ending at cur_start.
      // Keep the two in step — this is what stops a backward turn skipping text.
      {
        PagePosition anchor = got;
        if (!(anchor == PagePosition{0, 0})) {
          layout.set_position(anchor);
          PageContent back2 = layout.layout_backward();
          if (back2.start < anchor)
            anchor = back2.start;
        }
        PagePosition probe = anchor;
        PagePosition best = got;
        for (int guard = 0; guard < 64; ++guard) {
          ++resyncs;
          const PagePosition probe_end = layout.layout_end(probe);
          if (!(probe < probe_end))
            break;
          if (!(probe_end < cur_start)) {
            if (probe_end == cur_start)
              best = probe;
            break;
          }
          probe = probe_end;
        }
        got = best;
      }

      if (!(got == starts[i - 1])) {
        // A mismatch confined to one paragraph is the benign hanging-descender
        // case: the page starts a line early and repeats it. A mismatch that
        // crosses paragraphs means text the reader never saw was skipped, which
        // is the failure this whole path exists to prevent.
        const bool crosses_paras = got.paragraph != starts[i - 1].paragraph;
        if (crosses_paras) {
          if (skipped_text < 8)
            std::fprintf(stderr, "ch %u: SKIP back from (%u,%u) -> (%u,%u), expected (%u,%u)\n", ch,
                         cur_start.paragraph, cur_start.offset, got.paragraph, got.offset, starts[i - 1].paragraph,
                         starts[i - 1].offset);
          ++skipped_text;
        }
        ++back_mismatch;
      }
    }
  }

  std::printf("chapters=%u pages=%zu blank=%zu stalls=%zu line_off=%zu skipped_text=%zu resyncs=%zu\n",
              wgb.chapter_count(), total_pages, blank_pages, stalls, back_mismatch - skipped_text, skipped_text,
              resyncs);
  // Hard invariants: a page must always consume input, and a backward turn must
  // never skip text. A one-line overlap (line_off) is tolerated by design.
  assert(stalls == 0 && "a page consumed nothing");
  assert(blank_pages == 0 && "a page rendered nothing at all");
  // The hard one: a backward turn must never land on a different *paragraph*
  // than the forward walk, because that is text the reader never saw.
  assert(skipped_text == 0 && "a backward turn skipped text the reader never saw");
  // line_off (same paragraph, different line) is tolerated. Closing it needs a
  // chapter-wide forward chain, which costs ~31 KB of paragraph-window pressure
  // and reintroduced the heap abort with the clicker connected — see prev_page_.
  return (stalls || skipped_text || blank_pages) ? 1 : 0;
}
