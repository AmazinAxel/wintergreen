// DrawBuffer::blit_1bit against a per-pixel reference, both rotations, with
// every kind of clipping. The byte-packing paths (shifted rows in landscape,
// 8-row transposes in portrait) are the ones that can go wrong a bit at a time.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "wintergreen/display/DrawBuffer.h"
#include "wintergreen/display/ui_font_header.h"

using namespace wintergreen;

struct CaptureDisplay : IDisplay {
  const uint8_t* last = nullptr;
  void full_refresh(const uint8_t* pixels, const uint8_t*, RefreshMode, bool) override { last = pixels; }
  void partial_refresh(const uint8_t* pixels) override { last = pixels; }
};

static bool ref_bit(const std::vector<uint8_t>& fb, int px, int py) {
  const int b = px + DisplayFrame::kPanelOffsetX;
  return (fb[static_cast<size_t>(py) * DisplayFrame::kStride + b / 8] >> (7 - (b & 7))) & 1;
}

static void ref_set(std::vector<uint8_t>& fb, int px, int py, bool white) {
  const int b = px + DisplayFrame::kPanelOffsetX;
  uint8_t& d = fb[static_cast<size_t>(py) * DisplayFrame::kStride + b / 8];
  const uint8_t m = static_cast<uint8_t>(0x80u >> (b & 7));
  d = white ? (d | m) : (d & ~m);
}

int main() {
  std::srand(1234);
  CaptureDisplay disp;
  DrawBuffer buf(disp);
  for (int iter = 0; iter < 4000; ++iter) {
    const Rotation rot = (iter & 1) ? Rotation::Deg90 : Rotation::Deg0;
    buf.set_rotation(rot);
    const bool white_bg = iter & 2;
    buf.fill(white_bg);
    std::vector<uint8_t> ref(DrawBuffer::kBufSize, white_bg ? 0xFF : 0x00);

    const int w = 1 + std::rand() % 90, h = 1 + std::rand() % 90;
    const size_t stride = (w + 7) / 8 + std::rand() % 3;
    std::vector<uint8_t> img(stride * h);
    for (auto& b : img) b = static_cast<uint8_t>(std::rand());
    const int x = std::rand() % (buf.width() + 2 * w) - w;
    const int y = std::rand() % (buf.height() + 2 * h) - h;
    buf.blit_1bit(x, y, img.data(), w, h, stride);

    for (int r = 0; r < h; ++r)
      for (int c = 0; c < w; ++c) {
        const int lx = x + c, ly = y + r;
        if (lx < 0 || ly < 0 || lx >= buf.width() || ly >= buf.height())
          continue;
        const bool white = (img[r * stride + c / 8] >> (7 - (c & 7))) & 1;
        if (rot == Rotation::Deg0)
          ref_set(ref, lx, ly, white);
        else
          ref_set(ref, ly, DisplayFrame::kPhysicalHeight - 1 - lx, white);
      }

    buf.refresh();
    const uint8_t* fb = disp.last;
    for (int py = 0; py < DisplayFrame::kPhysicalHeight; ++py)
      for (int px = 0; px < DisplayFrame::kPhysicalWidth; ++px) {
        const int b = px + DisplayFrame::kPanelOffsetX;
        const bool got = (fb[static_cast<size_t>(py) * DisplayFrame::kStride + b / 8] >> (7 - (b & 7))) & 1;
        if (got != ref_bit(ref, px, py)) {
          std::fprintf(stderr, "iter %d rot %d at (%d,%d): w=%d h=%d x=%d y=%d\n", iter, static_cast<int>(rot), px, py,
                       w, h, x, y);
          return 1;
        }
      }
    // Bytes outside the visible columns must never be written.
    for (int py = 0; py < DisplayFrame::kPhysicalHeight; ++py) {
      const uint8_t first = fb[static_cast<size_t>(py) * DisplayFrame::kStride];
      const uint8_t mask = static_cast<uint8_t>(0xFF << (8 - DisplayFrame::kPanelOffsetX));
      assert((first & mask) == ((white_bg ? 0xFF : 0x00) & mask));
    }
  }
  // Glyphs: ink only where the bitmap bit is clear, background untouched, in
  // both colours. Reference walks the same cursor rules per pixel.
  BitmapFont font(kFontData_ui_header_wgf, kFontData_ui_header_wgf_size);
  assert(font.valid());
  static const char kText[] = "Wintergreen, \xC3\xA9t\xC3\xA9 AV.";
  for (int iter = 0; iter < 400; ++iter) {
    const Rotation rot = (iter & 1) ? Rotation::Deg90 : Rotation::Deg0;
    const bool ink_white = iter & 2;
    buf.set_rotation(rot);
    buf.fill(!ink_white);
    std::vector<uint8_t> ref(DrawBuffer::kBufSize, ink_white ? 0x00 : 0xFF);
    const int x = std::rand() % (buf.width() + 100) - 80;
    const int base = std::rand() % (buf.height() + 60) - 20;
    buf.draw_text_proportional(x, base, kText, font, ink_white);

    const char* p = kText;
    const char* end = kText + sizeof(kText) - 1;
    int cursor_q = x * 4, prev = -1;
    const BitmapFont::Styled sf = font.styled(FontStyle::Regular);
    while (p < end) {
      const int idx = sf.glyph_index(BitmapFont::decode_utf8(p, end));
      cursor_q += sf.kerning_q(prev, idx);
      const GlyphData g = sf.glyph_data_at(idx);
      const int gx = (cursor_q + 2) / 4 + g.x_offset, gy = base + g.y_offset;
      const int gs = (g.bitmap_width + 7) / 8;
      for (int r = 0; g.bits && r < g.bitmap_height; ++r)
        for (int c = 0; c < g.bitmap_width; ++c) {
          if ((g.bits[r * gs + c / 8] >> (7 - (c & 7))) & 1)
            continue;  // background
          const int lx = gx + c, ly = gy + r;
          if (lx < 0 || ly < 0 || lx >= buf.width() || ly >= buf.height())
            continue;
          if (rot == Rotation::Deg0)
            ref_set(ref, lx, ly, ink_white);
          else
            ref_set(ref, ly, DisplayFrame::kPhysicalHeight - 1 - lx, ink_white);
        }
      cursor_q += g.advance_width;
      cursor_q = ((cursor_q + 2) / 4) * 4;
      prev = idx;
    }

    buf.refresh();
    for (int py = 0; py < DisplayFrame::kPhysicalHeight; ++py)
      for (int px = 0; px < DisplayFrame::kPhysicalWidth; ++px) {
        const int b = px + DisplayFrame::kPanelOffsetX;
        const bool got = (disp.last[static_cast<size_t>(py) * DisplayFrame::kStride + b / 8] >> (7 - (b & 7))) & 1;
        if (got != ref_bit(ref, px, py)) {
          std::fprintf(stderr, "text iter %d rot %d at (%d,%d) x=%d base=%d\n", iter, static_cast<int>(rot), px, py, x,
                       base);
          return 1;
        }
      }
  }
  std::puts("blit ok");
  return 0;
}
