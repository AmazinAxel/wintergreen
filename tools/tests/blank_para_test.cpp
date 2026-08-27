// Self-check for pagination across whitespace-only paragraphs.
//
// `<p> </p>` has a non-empty run holding only spaces, so it is not
// text_runs_empty, and layout_para_lines returns *zero* lines for it. Before the
// fix, collect_text returned nullopt for such a paragraph: it produced no item
// and consumed no height, so a run of them left the page never filling. On
// hardware that was blank and half-blank pages, a backward walk whose start
// position could not move past them, and a saved .pos that reloaded straight
// back into the stuck state.
//
// The invariant this pins: a page must always make forward progress, and
// forward then backward must return to where it started.
//
// Run:
//   nix-shell -p gcc --run \
//     'g++ -std=c++17 -I wintergreen -fsanitize=address,undefined \
//        -o /tmp/bp tools/tests/blank_para_test.cpp \
//        wintergreen/content/TextLayout.cpp \
//        wintergreen/content/hyphenation/Hyphenation.cpp && /tmp/bp'
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "content/Font.h"
#include "content/IParagraphSource.h"
#include "content/TextLayout.h"

using namespace wintergreen;

namespace {

Paragraph text_para(const std::string& s) {
  Paragraph p;
  p.type = ParagraphType::Text;
  Run r;
  r.text = s;
  p.text.runs.push_back(r);
  return p;
}

struct VecSource : IParagraphSource {
  std::vector<Paragraph> paras;
  size_t paragraph_count() const override {
    return paras.size();
  }
  const Paragraph& paragraph(size_t i) const override {
    return paras[i];
  }
};

// A page height of 5 lines at the FixedFont's 16 px advance.
constexpr uint16_t kLineH = 16;
constexpr uint16_t kPageH = kLineH * 5;

PageOptions page_opts() {
  PageOptions o;
  o.width = 200;
  o.height = kPageH;
  o.padding_left = o.padding_right = o.padding_top = o.padding_bottom = 0;
  o.para_spacing = 0;
  return o;
}

}  // namespace

int main() {
  FixedFont font(8, kLineH);

  // A body of text with a cluster of whitespace-only paragraphs in the middle —
  // the shape a <br>-split section produces, and the one that wedged the reader.
  VecSource src;
  for (int i = 0; i < 4; ++i)
    src.paras.push_back(text_para("alpha bravo charlie delta echo foxtrot"));
  for (int i = 0; i < 6; ++i)
    src.paras.push_back(text_para(" "));  // whitespace only: zero lines
  for (int i = 0; i < 4; ++i)
    src.paras.push_back(text_para("golf hotel india juliett kilo lima"));

  TextLayout layout;
  layout.set_source(src);
  layout.set_font(font);
  layout.set_options(page_opts());

  // --- Forward: every page must advance, and the book must terminate. ---
  std::vector<PagePosition> starts;
  PagePosition pos{0, 0, 0};
  int guard = 0;
  while (true) {
    assert(++guard < 500 && "pagination did not terminate");
    layout.set_position(pos);
    PageContent pc = layout.layout();
    starts.push_back(pos);

    // The blank-paragraph bug showed up exactly here: end == start meant the
    // page consumed nothing at all.
    assert(!(pc.end == pos) && "page made no forward progress");

    if (pc.at_chapter_end)
      break;
    pos = pc.end;
  }
  assert(starts.size() > 1 && "expected several pages");

  // --- Backward: stepping back from each page start returns the previous one. ---
  for (size_t i = starts.size(); i-- > 1;) {
    layout.set_position(starts[i]);
    PageContent back = layout.layout_backward();
    assert(back.start == starts[i - 1] && "backward page did not match forward page");
  }

  // --- A page made only of blank paragraphs still terminates and advances. ---
  VecSource blanks;
  for (int i = 0; i < 40; ++i)
    blanks.paras.push_back(text_para("   "));
  TextLayout bl;
  bl.set_source(blanks);
  bl.set_font(font);
  bl.set_options(page_opts());
  PagePosition bp{0, 0, 0};
  guard = 0;
  while (true) {
    assert(++guard < 500 && "all-blank pagination did not terminate");
    bl.set_position(bp);
    PageContent pc = bl.layout();
    assert(!(pc.end == bp) && "all-blank page made no forward progress");
    if (pc.at_chapter_end)
      break;
    bp = pc.end;
  }

  // --- Chapter shapes that occur at the Odyssey's prologue boundaries. ---
  // A short chapter that is nothing but whitespace paragraphs is what a
  // "prologue" separator page converts to, and it must still terminate and
  // report at_chapter_end so ReaderScreen advances to the next chapter.
  {
    VecSource ws;
    for (int i = 0; i < 3; ++i)
      ws.paras.push_back(text_para(" "));
    TextLayout t;
    t.set_source(ws);
    t.set_font(font);
    t.set_options(page_opts());
    t.set_position(PagePosition{0, 0, 0});
    PageContent pc = t.layout();
    assert(pc.at_chapter_end && "all-whitespace chapter must end, not stall");
  }

  // An empty chapter (no paragraphs at all) must not hang or index anything.
  {
    VecSource none;
    TextLayout t;
    t.set_source(none);
    t.set_font(font);
    t.set_options(page_opts());
    t.set_position(PagePosition{0, 0, 0});
    PageContent pc = t.layout();
    assert(pc.at_chapter_end && "empty chapter must report chapter end");
    PageContent back = t.layout_backward();
    assert(back.items.empty() && "empty chapter has no backward page");
  }

  // A single whitespace paragraph followed by real text: the blank must not
  // swallow the text or leave the page unfillable.
  {
    VecSource mix;
    mix.paras.push_back(text_para(" "));
    mix.paras.push_back(text_para("mike november oscar papa quebec romeo sierra"));
    TextLayout t;
    t.set_source(mix);
    t.set_font(font);
    t.set_options(page_opts());
    t.set_position(PagePosition{0, 0, 0});
    PageContent pc = t.layout();
    bool has_text = false;
    for (const auto& ci : pc.items)
      if (std::get_if<PageTextItem>(&ci))
        has_text = true;
    assert(has_text && "text after a blank paragraph must still render");
  }

  std::printf("ok: %zu pages, blank paragraphs paginate\n", starts.size());
  return 0;
}
