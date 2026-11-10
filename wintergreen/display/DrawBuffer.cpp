#include "DrawBuffer.h"

namespace wintergreen {

namespace {

constexpr int kStride = DisplayFrame::kStride;
constexpr int kPhysW = DisplayFrame::kPhysicalWidth;
constexpr int kPhysH = DisplayFrame::kPhysicalHeight;
constexpr int kOffX = DisplayFrame::kPanelOffsetX;

// How a source bit meets the framebuffer. Copy writes both colours (images);
// the two ink modes paint only where the source bit is clear (glyphs, whose
// bitmaps store ink as 0) and leave the background alone.
enum class BlitOp { Copy, InkWhite, InkBlack };

template <BlitOp Op>
inline void apply_(uint8_t& d, uint8_t bits, uint8_t mask) {
  if constexpr (Op == BlitOp::Copy)
    d = static_cast<uint8_t>((d & ~mask) | (bits & mask));
  else if constexpr (Op == BlitOp::InkWhite)
    d |= static_cast<uint8_t>(~bits & mask);
  else
    d &= static_cast<uint8_t>(~(~bits & mask));
}

// Landscape: logical (x + c, y + r) is physical (x + c, y + r), so a source row
// is a framebuffer row and goes in a byte at a time, shifted to the target's bit
// offset. This used to visit every pixel through four bounds checks — a page of
// text in landscape was several hundred thousand of them.
template <BlitOp Op>
void blit_deg0_(uint8_t* buf, int x, int y, const uint8_t* src, int w, int h, size_t stride) {
  const int c_lo = std::max(0, -x);
  const int c_hi = std::min(w, kPhysW - x);
  const int r_lo = std::max(0, -y);
  const int r_hi = std::min(h, kPhysH - y);
  if (c_lo >= c_hi || r_lo >= r_hi)
    return;
  const int bit0 = x + kOffX;  // framebuffer bit of source column 0 (may be negative)
  for (int r = r_lo; r < r_hi; ++r) {
    uint8_t* row = buf + static_cast<size_t>(y + r) * kStride;
    const uint8_t* s = src + static_cast<size_t>(r) * stride;
    for (int j = c_lo >> 3; j <= (c_hi - 1) >> 3; ++j) {
      // Columns of this source byte that are inside both the glyph and the panel.
      const int a = std::max(c_lo, 8 * j) - 8 * j;
      const int b = std::min(c_hi, 8 * j + 8) - 8 * j;
      const auto mask = static_cast<uint8_t>((0xFFu >> a) & (0xFFu << (8 - b)));
      const int start = bit0 + 8 * j;
      const int d = start >> 3;
      const int sh = start & 7;
      // Every unmasked bit lands on the panel, so a non-zero mask is also the
      // bounds check: the byte either side of a clipped edge is never touched.
      const auto m_hi = static_cast<uint8_t>(mask >> sh);
      if (m_hi)
        apply_<Op>(row[d], static_cast<uint8_t>(s[j] >> sh), m_hi);
      if (sh) {
        const auto m_lo = static_cast<uint8_t>(mask << (8 - sh));
        if (m_lo)
          apply_<Op>(row[d + 1], static_cast<uint8_t>(s[j] << (8 - sh)), m_lo);
      }
    }
  }
}

// Portrait: logical (x + c, y + r) is physical (y + r, kPhysH - 1 - x - c). A
// source column is a framebuffer row, so the loop runs column-major and packs
// eight source rows into each output byte once it is byte-aligned.
template <BlitOp Op>
void blit_deg90_(uint8_t* buf, int x, int y, const uint8_t* src, int w, int h, size_t stride) {
  const int c_lo = std::max(0, -x);
  const int c_hi = std::min(w, kPhysH - x);
  const int r_lo = std::max(0, -y);
  const int r_hi = std::min(h, kPhysW - y);
  if (c_lo >= c_hi || r_lo >= r_hi)
    return;
  const int bx0 = y + kOffX;  // framebuffer bit of source row 0
  const int aligned = r_lo + ((8 - ((bx0 + r_lo) & 7)) & 7);
  for (int c = c_lo; c < c_hi; ++c) {
    uint8_t* row = buf + static_cast<size_t>(kPhysH - 1 - x - c) * kStride;
    const uint8_t* s = src + (c >> 3);
    const int sh = 7 - (c & 7);
    auto single = [&](int r) {
      const int bx = bx0 + r;
      const auto m = static_cast<uint8_t>(0x80u >> (bx & 7));
      apply_<Op>(row[bx >> 3], ((s[static_cast<size_t>(r) * stride] >> sh) & 1u) ? m : 0, m);
    };
    int r = r_lo;
    for (; r < r_hi && r < aligned; ++r)
      single(r);
    for (; r + 8 <= r_hi; r += 8) {
      const uint8_t* p = s + static_cast<size_t>(r) * stride;
      uint8_t v = 0;
      for (int k = 0; k < 8; ++k, p += stride)
        v |= static_cast<uint8_t>(((*p >> sh) & 1u) << (7 - k));
      apply_<Op>(row[(bx0 + r) >> 3], v, 0xFF);
    }
    for (; r < r_hi; ++r)
      single(r);
  }
}

template <BlitOp Op>
inline void blit_(uint8_t* buf, Rotation rot, int x, int y, const uint8_t* src, int w, int h, size_t stride) {
  if (rot == Rotation::Deg90)
    blit_deg90_<Op>(buf, x, y, src, w, h, stride);
  else
    blit_deg0_<Op>(buf, x, y, src, w, h, stride);
}

// Shared UTF-8 render core. One glyph lookup per character: the previous glyph's
// index is carried for kerning, as BitmapFont::word_width() does.
int draw_text_impl_(uint8_t* buf, Rotation rot, int x, int baseline_y, const char* text, size_t len,
                    const BitmapFont& font, bool white, FontStyle style) {
  if (!text || len == 0 || !font.valid())
    return x;
  const char* p = text;
  const char* end = text + len;
  int cursor_q = x * 4;  // quarter pixels
  int prev_idx = -1;
  const BitmapFont::Styled f = font.styled(style);

  while (p < end) {
    const int idx = f.glyph_index(BitmapFont::decode_utf8(p, end));
    cursor_q += f.kerning_q(prev_idx, idx);
    const GlyphData g = f.glyph_data_at(idx);
    if (g.bits) {
      const int gx = (cursor_q + 2) / 4 + g.x_offset;
      const int gy = baseline_y + g.y_offset;
      const size_t gstride = (g.bitmap_width + 7) / 8;
      if (white)
        blit_<BlitOp::InkWhite>(buf, rot, gx, gy, g.bits, g.bitmap_width, g.bitmap_height, gstride);
      else
        blit_<BlitOp::InkBlack>(buf, rot, gx, gy, g.bits, g.bitmap_width, g.bitmap_height, gstride);
    }
    cursor_q += g.advance_width;
    cursor_q = ((cursor_q + 2) / 4) * 4;  // snap to full pixel - prevents fractional accumulation across characters
    prev_idx = idx;
  }
  return cursor_q / 4;
}

}  // namespace

void DrawBuffer::blit_1bit(int lx, int ly, const uint8_t* data, int w, int h, size_t stride) {
  if (!data || w <= 0 || h <= 0)
    return;
  blit_<BlitOp::Copy>(draw_(), rotation_, lx, ly, data, w, h, stride);
}

int DrawBuffer::draw_text_proportional(int x, int baseline_y, const char* text, size_t len, const BitmapFont& font,
                                       bool white, FontStyle style) {
  return draw_text_impl_(draw_(), rotation_, x, baseline_y, text, len, font, white, style);
}

void DrawBuffer::draw_layout_line(int x_offset, int baseline_y, const PageLine& line, const BitmapFontSet& fonts,
                                  bool white) {
  uint8_t* buf = draw_();
  for (const auto& w : line.words) {
    if (w.len == 0)
      continue;
    const BitmapFont* f = fonts.get(w.size_pct);
    if (!f || !f->valid())
      continue;
    int word_baseline = baseline_y;
    if (w.vertical_align == VerticalAlign::Super)
      word_baseline -= static_cast<int>(fonts.y_advance(w.size_pct)) * 20 / 100;
    else if (w.vertical_align == VerticalAlign::Sub)
      word_baseline += static_cast<int>(fonts.y_advance(w.size_pct)) * 20 / 100;
    draw_text_impl_(buf, rotation_, x_offset + w.x, word_baseline, w.text, w.len, *f, white, w.style);
  }
}

// Decode MGR2 into the inactive buffer and paint it with RefreshMode::Half.
//
// This used to build both RAM planes and fire grayscale_refresh_1pass. That
// was dropped for two reasons, and the first is the reason it looked wrong:
//
// kLutFactoryQuality staggers its phases by level. Over G0's 24 frames LUT0
// (black) is idle (VS 0x00) while LUT3 (white) oscillates (0xA8), so the
// wordmark settled almost immediately and the background churned around it
// for the bulk of the update — read on the glass as "wordmark first, then the
// screen flashes". That ordering is baked into the LUT, not into anything
// sequenced here, so the only fix is a different waveform.
//
// Half is the same waveform the sleep *cover* has always used
// (Application.cpp's show_book_cover_sleep_), which does not have the symptom.
//
// Second, it costs nothing to give up: resources/sleep.mgr holds only levels
// 0 and 3 — 360,074 white and 23,926 black pixels, no gray anywhere — so the
// grayscale path had no intermediate tones to render. A 4-level MGR2 would now
// quantise to black/white at the 50% point; if one is ever shipped, that is the
// line to revisit.
//
// Half sends CTRL1_BYPASS_RED, so RED is ignored: no second plane, and hence
// no need for the spare buffer or its without-the-spare fallback. Note the
// polarity flip — the grayscale decode set a bit for *ink*, while ordinary
// drawing (see fill()/blit_1bit) uses set = white, which is what full_refresh
// consumes.
//
// Blitted in physical panel coordinates with no offset: the image is
// native-size, not scaled or centred.
bool DrawBuffer::show_sleep_image_embedded() {
  const uint8_t* data = sleep_img_;
  if (!data || sleep_img_size_ < 8 || std::memcmp(data, "MGR2", 4) != 0)
    return false;
  const int w = data[4] | (data[5] << 8);
  const int h = data[6] | (data[7] << 8);
  const size_t src_stride = (static_cast<size_t>(w) + 3) / 4;
  if (w == 0 || h == 0 || sleep_img_size_ < 8 + src_stride * static_cast<size_t>(h))
    return false;
  data += 8;

  const int rows = std::min(h, kPhysH);
  const int cols = std::min(w, DisplayFrame::kPanelWidth);
  // Anything the image does not cover stays white.
  memset(inactive_(), 0xFF, kBufSize);
  for (int y = 0; y < rows; ++y) {
    const uint8_t* src_row = data + static_cast<size_t>(y) * src_stride;
    uint8_t* dst = inactive_() + static_cast<size_t>(y) * kStride;
    for (int x = 0; x < cols; ++x) {
      // 0 = white, 1 = light, 2 = dark, 3 = black; threshold at the midpoint.
      const uint8_t v = (src_row[x / 4] >> (6 - (x % 4) * 2)) & 0x3;
      if (v >= 2)
        dst[x / 8] &= static_cast<uint8_t>(~(0x80u >> (x % 8)));
    }
  }

  display_.full_refresh(inactive_(), active_(), RefreshMode::Half, /*turnOffScreen=*/true);
  return true;
}

}  // namespace wintergreen
