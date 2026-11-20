// The strip renderer against a per-pixel reference, both rotations, with every
// kind of clipping: fills and rects at every level, text in both inks (glyphs
// read from the row-major planes here, so the column-major copy the portrait
// path uses is checked against them), pixel sources in all three layouts with
// row keeps, and diamonds. DrawBuffer::render_current goes through the same
// strip code as refresh(), including the fetch splitting.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "sim_panel.h"
#include "wintergreen/display/ui_font_header.h"
#include "wintergreen/display/ui_font_large.h"

using namespace wintergreen;
using namespace simpanel;

namespace {

constexpr int kOffX = DisplayFrame::kPanelOffsetX;
constexpr int kPhysH = DisplayFrame::kPhysicalHeight;

struct Ref {
  std::vector<uint8_t> px = std::vector<uint8_t>(static_cast<size_t>(kRows) * kBits, 0);
  Rotation rot = Rotation::Deg90;
  int W() const { return rot == Rotation::Deg0 ? DisplayFrame::kPhysicalWidth : DrawBuffer::kWidth; }
  int H() const { return rot == Rotation::Deg0 ? kPhysH : DrawBuffer::kHeight; }
  uint8_t* at(int lx, int ly) {
    if (lx < 0 || ly < 0 || lx >= W() || ly >= H())
      return nullptr;
    const int row = rot == Rotation::Deg0 ? ly : kPhysH - 1 - lx;
    const int bit = (rot == Rotation::Deg0 ? lx : ly) + kOffX;
    return &px[static_cast<size_t>(row) * kBits + bit];
  }
  void fill(uint8_t l) { std::fill(px.begin(), px.end(), l); }
  void rect(int x, int y, int w, int h, uint8_t l) {
    for (int j = y; j < y + h; ++j)
      for (int i = x; i < x + w; ++i)
        if (uint8_t* p = at(i, j))
          *p = l;
  }
};

uint8_t plane_bit(const uint8_t* plane, int stride, int r, int c) {
  return (plane[r * stride + c / 8] >> (7 - (c & 7))) & 1;
}

// DrawBuffer::draw_text's pen loop, pixel by pixel.
int ref_text(Ref& ref, int x, int baseline, const char* text, const BitmapFont& font, Ink ink, FontStyle style) {
  const BitmapFont::Styled f = font.styled(style);
  const char* p = text;
  const char* e = text + std::strlen(text);
  int cursor_q = x * 4, prev = -1;
  while (p < e) {
    const int idx = f.glyph_index(BitmapFont::decode_utf8(p, e));
    cursor_q += f.kerning_q(prev, idx);
    const GlyphData g = f.glyph_data_at(idx);
    if (g.bits) {
      const int gx = (cursor_q + 2) / 4 + g.x_offset, gy = baseline + g.y_offset;
      const int rs = (g.bitmap_width + 7) / 8;
      const uint8_t* lo = g.bits + rs * g.bitmap_height;
      for (int r = 0; r < g.bitmap_height; ++r)
        for (int c = 0; c < g.bitmap_width; ++c) {
          uint8_t* d = ref.at(gx + c, gy + r);
          if (!d)
            continue;
          const int h = plane_bit(g.bits, rs, r, c), l = plane_bit(lo, rs, r, c);
          assert(!l || h);  // black inside gray
          if (ink == Ink::Black) {
            *d |= static_cast<uint8_t>((h << 1) | l);
          } else {
            // hi &= ~g.lo, lo &= ~g.hi
            if (l)
              *d &= 1;
            if (h)
              *d &= 2;
          }
        }
    }
    cursor_q += g.advance_width;
    cursor_q = ((cursor_q + 2) / 4) * 4;
    prev = idx;
  }
  return cursor_q / 4;
}

int ref_cov(int px, int py, int cx, int cy, int r, int t) {
  int n = 0;
  for (int j = 0; j < 4; ++j)
    for (int i = 0; i < 4; ++i) {
      const double dx = std::abs(px + (2 * i + 1) / 8.0 - (cx + 0.5));
      const double dy = std::abs(py + (2 * j + 1) / 8.0 - (cy + 0.5));
      const double d = dx + dy;
      if (d <= r && (t == 0 || d > r - t))
        ++n;
    }
  return n;
}

}  // namespace

int main() {
  std::srand(1234);
  SimPanel disp;
  DrawBuffer buf(disp);
  BitmapFont header(kFontData_ui_header_wgf, kFontData_ui_header_wgf_size);
  BitmapFont large(kFontData_ui_large_wgf, kFontData_ui_large_wgf_size);
  assert(header.valid() && large.valid());
  MemSource src;
  static const char* kTexts[] = {"Wave AVATAR", "fj. \xE2\x80\x9Cquick\xE2\x80\x9D", "87%", "Syncing...",
                                 "The Hobbit", "caf\xC3\xA9 0123456789"};

  for (int iter = 0; iter < 600; ++iter) {
    Ref ref;
    ref.rot = (iter & 1) ? Rotation::Deg90 : Rotation::Deg0;
    buf.set_rotation(ref.rot);
    const auto bg = static_cast<Level>(std::rand() % 4);
    buf.fill(bg);
    ref.fill(static_cast<uint8_t>(bg));
    src.items.clear();

    const int ops = 1 + std::rand() % 12;
    for (int k = 0; k < ops; ++k) {
      const int W = ref.W(), H = ref.H();
      switch (std::rand() % 5) {
        case 0: {
          const int w = 1 + std::rand() % 200, h = 1 + std::rand() % 200;
          const int x = std::rand() % (W + w) - w / 2, y = std::rand() % (H + h) - h / 2;
          const auto l = static_cast<Level>(std::rand() % 3 == 0 ? 2 : (std::rand() % 2) * 3);
          buf.fill_rect(x, y, w, h, l);
          ref.rect(x, y, w, h, static_cast<uint8_t>(l));
          break;
        }
        case 1: {
          const char* t = kTexts[std::rand() % 6];
          const BitmapFont& f = std::rand() % 2 ? header : large;
          const Ink ink = std::rand() % 3 == 0 ? Ink::White : Ink::Black;
          const int x = std::rand() % (W + 100) - 100, y = std::rand() % (H + 40) - 10;
          const int a = buf.draw_text(x, y, t, f, ink);
          const int b = ref_text(ref, x, y, t, f, ink, FontStyle::Regular);
          assert(a == b);
          break;
        }
        case 2: {
          // Cols in portrait, Rows in landscape, random keeps; both planes
          // random but black inside gray.
          const int w = 1 + std::rand() % 120, h = 1 + std::rand() % 120;
          const bool cols = ref.rot == Rotation::Deg90;
          const int ub = cols ? (h + 7) / 8 : (w + 7) / 8;
          const int units = cols ? w : h;
          std::vector<uint8_t> data(static_cast<size_t>(7) + 2u * ub * units);
          for (auto& b : data)
            b = static_cast<uint8_t>(std::rand());
          for (int u = 0; u < units; ++u)
            for (int i = 0; i < ub; ++i)
              data[7 + (2 * u + 1) * ub + i] &= data[7 + 2 * u * ub + i];
          src.items.push_back(data);
          const int x = std::rand() % (W + w) - w / 2, y = std::rand() % (H + h) - h / 2;
          int klo = std::rand() % h, khi = klo + 1 + std::rand() % (h - klo);
          if (std::rand() % 2) {
            klo = 0;
            khi = h;
          }
          buf.blit(&src, static_cast<uint32_t>(src.items.size() - 1), 0, 7,
                   cols ? PixelLayout::Cols : PixelLayout::Rows, x, y, w, h, klo, khi);
          for (int r = klo; r < khi; ++r)
            for (int c = 0; c < w; ++c) {
              uint8_t* d = ref.at(x + c, y + r);
              if (!d)
                continue;
              const uint8_t* u = data.data() + 7 + 2u * ub * (cols ? c : r);
              const int i = cols ? r : c;
              const int hb = (u[i / 8] >> (7 - (i & 7))) & 1, lb = (u[ub + i / 8] >> (7 - (i & 7))) & 1;
              *d = static_cast<uint8_t>((hb << 1) | lb);
            }
          break;
        }
        case 3: {
          const int r = 1 + std::rand() % 14, t = std::rand() % 3;
          const int cx = std::rand() % W, cy = std::rand() % H;
          buf.draw_diamond(cx, cy, r, t);
          for (int y = cy - r - 2; y <= cy + r + 2; ++y)
            for (int x = cx - r - 2; x <= cx + r + 2; ++x) {
              const int cov = ref_cov(x, y, cx, cy, r, t);
              uint8_t* d = ref.at(x, y);
              if (d && cov >= 4)
                *d |= cov >= 11 ? 3 : 2;
            }
          break;
        }
        default: {
          const int x = std::rand() % W, y = std::rand() % H, w = 1 + std::rand() % 60, h = 1 + std::rand() % 60;
          const int t = 1 + std::rand() % 4;
          buf.draw_outline(x, y, w, h, t, Level::Gray);
          const int tt = std::min({t, w, h});
          ref.rect(x, y, w, tt, 2);
          ref.rect(x, y + h - tt, w, tt, 2);
          ref.rect(x, y + tt, tt, h - 2 * tt, 2);
          ref.rect(x + w - tt, y + tt, tt, h - 2 * tt, 2);
          break;
        }
      }
    }
    // Frame layout occasionally: the whole panel from a source.
    if (iter % 50 == 49) {
      std::vector<uint8_t> frame(DisplayFrame::kFrameBytes);
      for (auto& b : frame)
        b = static_cast<uint8_t>(std::rand());
      src.items.push_back(frame);
      buf.fill(Level::White);
      buf.blit(&src, static_cast<uint32_t>(src.items.size() - 1), 0, 0, PixelLayout::Frame, 0, 0, kBits, kRows);
      ref.px = frame_levels(frame);
    }
    const std::vector<uint8_t> got = render_levels(buf);
    for (size_t i = 0; i < got.size(); ++i)
      if (got[i] != ref.px[i]) {
        std::fprintf(stderr, "iter %d: pixel row %zu bit %zu = %d, want %d\n", iter, i / kBits, i % kBits, got[i],
                     ref.px[i]);
        return 1;
      }
  }
  std::printf("ok\n");
  return 0;
}
