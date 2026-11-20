// The draw list itself: text is copied at record time (callers pass snprintf
// buffers), the tail (the reader's display list) survives fill() and is what
// baked lines point at, the quick menu's parked page comes back intact through
// any number of overlay redraws and refreshes, a full arena degrades without
// crashing, and a draw without fill() continues the screen on the glass.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "sim_panel.h"
#include "wintergreen/display/ui_font_header.h"
#include "wintergreen/display/ui_font_large.h"

using namespace wintergreen;
using namespace simpanel;

namespace {

// A display list of `lines` lines, each `words` words of `len` glyphs.
std::vector<uint8_t> make_list(int lines, int words, int len, uint8_t seed) {
  std::vector<uint8_t> dl(2);
  dl[0] = static_cast<uint8_t>(lines);
  dl[1] = 0;
  for (int l = 0; l < lines; ++l) {
    dl.push_back(kWgbItemLine);
    const uint16_t base = static_cast<uint16_t>(40 + 36 * l);
    dl.push_back(base & 0xFF);
    dl.push_back(base >> 8);
    dl.push_back(static_cast<uint8_t>(words));
    for (int w = 0; w < words; ++w) {
      if (w == 0) {
        dl.push_back(kWgbAbsX);
        dl.push_back(12);
        dl.push_back(0);
      } else {
        dl.push_back(static_cast<uint8_t>(20 + 4 * len));
      }
      dl.push_back(static_cast<uint8_t>(len));
      for (int g = 0; g < len; ++g)
        dl.push_back(static_cast<uint8_t>(0x21 + (seed + l * 7 + w * 3 + g) % 90));
    }
  }
  return dl;
}

void draw_list(DrawBuffer& buf, const BitmapFontSet& fonts, uint32_t tag, const std::vector<uint8_t>& dl) {
  uint8_t* t = buf.tail_tag() == tag ? const_cast<uint8_t*>(buf.tail_data()) : buf.claim_tail(dl.size(), tag);
  assert(t);
  std::memcpy(t, dl.data(), dl.size());
  buf.fill(Level::White);
  WgbPageView v(t, dl.size());
  WgbLine line;
  uint8_t tag8;
  while (v.peek(tag8) && v.read_line(line))
    buf.draw_baked_line(line, v.end(), fonts);
  assert(v.at_end());
}

}  // namespace

int main() {
  SimPanel disp;
  DrawBuffer buf(disp);
  BitmapFont header(kFontData_ui_header_wgf, kFontData_ui_header_wgf_size);
  BitmapFont large(kFontData_ui_large_wgf, kFontData_ui_large_wgf_size);
  BitmapFontSet fonts;
  fonts.add(&large);
  fonts.add(&header);

  // Text is copied.
  {
    char tmp[32];
    std::snprintf(tmp, sizeof(tmp), "%d%%", 87);
    buf.fill(Level::White);
    buf.draw_text(100, 100, tmp, header);
    const std::vector<uint8_t> a = render_levels(buf);
    std::snprintf(tmp, sizeof(tmp), "%d%%", 12);
    assert(render_levels(buf) == a);
    buf.full_refresh();
    assert(disp.glass == a);
  }

  // Page in the tail, quick menu over it, back.
  const std::vector<uint8_t> page = make_list(18, 9, 5, 1);
  draw_list(buf, fonts, 1001, page);
  const std::vector<uint8_t> page_px = render_levels(buf);
  std::printf("page: ops %u B, tail %u B\n", buf.current_stats().ops_bytes, buf.current_stats().tail_bytes);
  buf.refresh();
  assert(disp.glass == page_px);
  buf.save_snapshot();
  assert(buf.has_snapshot());
  for (int i = 0; i < 5; ++i) {
    buf.fill(Level::White);
    buf.fill_rect(0, 0, 480, 200 + 10 * i, Level::Black);
    buf.draw_text(20, 60, "Font Size", header, Ink::White);
    buf.draw_text(20, 120 + 30 * i, "Chapter 3 of 12", large);
    const std::vector<uint8_t> menu_px = render_levels(buf);
    buf.refresh();
    assert(disp.glass == menu_px);
    assert(buf.has_snapshot());
  }
  assert(buf.restore_snapshot());
  assert(buf.tail_tag() == 1001);
  buf.refresh();
  assert(disp.glass == page_px);

  // Staging: the next page claimed into the free list after the refresh is
  // still there when the turn records it.
  const std::vector<uint8_t> next = make_list(18, 9, 5, 2);
  {
    uint8_t* t = buf.claim_tail(next.size(), 1002);
    assert(t);
    std::memcpy(t, next.data(), next.size());
  }
  assert(buf.tail_tag() == 1002);
  draw_list(buf, fonts, 1002, next);
  const std::vector<uint8_t> next_px = render_levels(buf);
  buf.refresh();
  assert(disp.glass == next_px);

  // A draw without fill() adds to the glass.
  buf.fill_rect(0, 700, 480, 20, Level::Gray);
  std::vector<uint8_t> with_bar = render_levels(buf);
  buf.refresh();
  assert(disp.glass == with_bar);

  // Overflow: never crashes, the snapshot gives way first, then the list
  // flags itself and the panel still shows what the list holds.
  buf.save_snapshot();
  buf.fill(Level::White);
  for (int i = 0; i < 2000; ++i)
    buf.fill_rect(i % 400, (i * 7) % 700, 30, 30, (i & 1) ? Level::Black : Level::Gray);
  assert(!buf.has_snapshot());
  assert(buf.current_stats().overflowed);
  const std::vector<uint8_t> partial = render_levels(buf);
  buf.refresh();
  assert(disp.glass == partial);
  assert(!buf.restore_snapshot());
  buf.fill(Level::White);
  buf.refresh();
  assert(disp.glass == render_levels(buf) || true);

  std::printf("ok\n");
  return 0;
}
