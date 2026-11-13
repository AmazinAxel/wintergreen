#include "DrawBuffer.h"

#include <climits>

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

// One packed source row onto one framebuffer row, a byte at a time, shifted to
// the target's bit offset. Source bits [lo, hi) of an n-bit row land at
// framebuffer bits bit0 + lo ... (bit0 may be negative; [lo, hi) is already
// clipped).
template <BlitOp Op>
inline void put_row_(uint8_t* row, const uint8_t* s, int lo, int hi, int n, int bit0) {
  if (lo == 0 && hi == n) {
    // Unclipped (every glyph of a baked page, most of any screen): bit0 >= 0,
    // one shift for the whole row, a mask only on the last byte.
    const int sh = bit0 & 7;
    uint8_t* d = row + (bit0 >> 3);
    const int last = (n - 1) >> 3;
    for (int j = 0; j <= last; ++j, ++d) {
      const auto mask = static_cast<uint8_t>(j == last ? 0xFFu << ((8 - (n & 7)) & 7) : 0xFFu);
      apply_<Op>(d[0], static_cast<uint8_t>(s[j] >> sh), static_cast<uint8_t>(mask >> sh));
      // Only touch the next byte when bits land there: past the row's last
      // byte it is the next row, or past the end of the buffer.
      const auto m_lo = static_cast<uint8_t>(mask << (8 - sh));
      if (sh && m_lo)
        apply_<Op>(d[1], static_cast<uint8_t>(s[j] << (8 - sh)), m_lo);
    }
    return;
  }
  for (int j = lo >> 3; j <= (hi - 1) >> 3; ++j) {
    // Bits of this source byte that are inside both the image and the panel.
    const int a = std::max(lo, 8 * j) - 8 * j;
    const int b = std::min(hi, 8 * j + 8) - 8 * j;
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

// Landscape: logical (x + c, y + r) is physical (x + c, y + r), so a source row
// is a framebuffer row. This used to visit every pixel through four bounds
// checks — a page of text in landscape was several hundred thousand of them.
template <BlitOp Op>
void blit_deg0_(uint8_t* buf, int x, int y, const uint8_t* src, int w, int h, size_t stride) {
  const int c_lo = std::max(0, -x);
  const int c_hi = std::min(w, kPhysW - x);
  const int r_lo = std::max(0, -y);
  const int r_hi = std::min(h, kPhysH - y);
  if (c_lo >= c_hi || r_lo >= r_hi)
    return;
  for (int r = r_lo; r < r_hi; ++r)
    put_row_<Op>(buf + static_cast<size_t>(y + r) * kStride, src + static_cast<size_t>(r) * stride, c_lo, c_hi, w,
                 x + kOffX);
}

// Portrait from column-major source (each logical column c is `cstride` bytes
// holding its h pixels top to bottom, MSB first). Logical column x + c is
// physical row kPhysH - 1 - x - c and its pixels run along that row, so each
// column is one put_row_: the same byte copy as landscape, no transpose. Fonts
// and the home/list covers are baked in this order for the portrait screens.
//
// Rows outside [keep_lo, keep_hi) are not drawn either (a reader image band
// that straddles the page's slice of the image).
template <BlitOp Op>
void blit_cols_(uint8_t* buf, int x, int y, const uint8_t* cols, int w, int h, size_t cstride, int keep_lo = 0,
                int keep_hi = INT_MAX) {
  const int c_lo = std::max(0, -x);
  const int c_hi = std::min(w, kPhysH - x);
  const int r_lo = std::max({0, -y, keep_lo});
  const int r_hi = std::min({h, kPhysW - y, keep_hi});
  if (c_lo >= c_hi || r_lo >= r_hi)
    return;
  for (int c = c_lo; c < c_hi; ++c)
    put_row_<Op>(buf + static_cast<size_t>(kPhysH - 1 - x - c) * kStride, cols + static_cast<size_t>(c) * cstride,
                 r_lo, r_hi, h, y + kOffX);
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

// Shared render core over glyph indices; `more()` says whether another glyph
// follows and `next()` yields its index. The previous glyph's index is carried
// for kerning, as BitmapFont::word_width() does.
template <typename More, typename Next>
int draw_glyphs_(uint8_t* buf, Rotation rot, int x, int baseline_y, const BitmapFont::Styled& f, bool white,
                 More more, Next next) {
  int cursor_q = x * 4;  // quarter pixels
  int prev_idx = -1;
  while (more()) {
    const int idx = next();
    cursor_q += f.kerning_q(prev_idx, idx);
    const GlyphData g = f.glyph_data_at(idx);
    if (g.bits) {
      const int gx = (cursor_q + 2) / 4 + g.x_offset;
      const int gy = baseline_y + g.y_offset;
      const size_t gstride = (g.bitmap_width + 7) / 8;
      if (rot == Rotation::Deg90 && g.cols) {
        const size_t cstride = (g.bitmap_height + 7) / 8;
        if (white)
          blit_cols_<BlitOp::InkWhite>(buf, gx, gy, g.cols, g.bitmap_width, g.bitmap_height, cstride);
        else
          blit_cols_<BlitOp::InkBlack>(buf, gx, gy, g.cols, g.bitmap_width, g.bitmap_height, cstride);
      } else if (white)
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

// UTF-8 text (UI strings): one range lookup per character.
int draw_text_impl_(uint8_t* buf, Rotation rot, int x, int baseline_y, const char* text, size_t len,
                    const BitmapFont& font, bool white, FontStyle style) {
  if (!text || len == 0 || !font.valid())
    return x;
  const char* p = text;
  const char* end = text + len;
  const BitmapFont::Styled f = font.styled(style);
  return draw_glyphs_(
      buf, rot, x, baseline_y, f, white, [&] { return p < end; },
      [&] { return f.glyph_index(BitmapFont::decode_utf8(p, end)); });
}

}  // namespace

void DrawBuffer::blit_1bit(int lx, int ly, const uint8_t* data, int w, int h, size_t stride) {
  if (!data || w <= 0 || h <= 0)
    return;
  blit_<BlitOp::Copy>(draw_(), rotation_, lx, ly, data, w, h, stride);
}

void DrawBuffer::blit_1bit_bands(int lx, int ly, const uint8_t* band, int w, int keep_lo, int keep_hi) {
  if (!band || w <= 0)
    return;
  if (rotation_ == Rotation::Deg90) {
    blit_cols_<BlitOp::Copy>(draw_(), lx, ly, band, w, 8, 1, keep_lo, keep_hi);
    return;
  }
  for (int c = 0; c < w; ++c)
    for (int r = std::max(0, keep_lo); r < std::min(8, keep_hi); ++r)
      fill_rect(lx + c, ly + r, 1, 1, (band[c] >> (7 - r)) & 1);
}

void DrawBuffer::blit_1bit_cols(int lx, int ly, const uint8_t* cols, int w, int h) {
  if (!cols || w <= 0 || h <= 0)
    return;
  const size_t cstride = static_cast<size_t>((h + 7) / 8);
  if (rotation_ == Rotation::Deg90) {
    blit_cols_<BlitOp::Copy>(draw_(), lx, ly, cols, w, h, cstride);
    return;
  }
  // Landscape never shows a cover; kept correct anyway, a pixel at a time.
  for (int c = 0; c < w; ++c)
    for (int r = 0; r < h; ++r) {
      const bool white = (cols[static_cast<size_t>(c) * cstride + (r >> 3)] >> (7 - (r & 7))) & 1;
      fill_rect(lx + c, ly + r, 1, 1, white);
    }
}

int DrawBuffer::draw_text_proportional(int x, int baseline_y, const char* text, size_t len, const BitmapFont& font,
                                       bool white, FontStyle style) {
  return draw_text_impl_(draw_(), rotation_, x, baseline_y, text, len, font, white, style);
}

void DrawBuffer::draw_baked_line(const WgbLine& line, const uint8_t* end, const BitmapFontSet& fonts, bool white) {
  uint8_t* buf = draw_();
  WgbWordIter it(line, end);
  WgbWord w;
  while (it.next(w)) {
    const BitmapFont* f = fonts.get_font(w.font);
    if (!f || !f->valid())
      continue;
    int word_baseline = line.baseline_y;
    if (w.valign == static_cast<uint8_t>(VerticalAlign::Super))
      word_baseline -= static_cast<int>(f->y_advance(100)) * 20 / 100;
    else if (w.valign == static_cast<uint8_t>(VerticalAlign::Sub))
      word_baseline += static_cast<int>(f->y_advance(100)) * 20 / 100;
    // Baked glyph indices (WgbFormat.h): no UTF-8 decode, no range search.
    const uint8_t* p = w.glyphs;
    const uint8_t* e = w.glyphs + w.len;
    draw_glyphs_(
        buf, rotation_, w.x, word_baseline, f->styled(static_cast<FontStyle>(w.style)), white,
        [&] { return p < e; }, [&] { return wgb_read_glyph(p, e); });
  }
}

// Decode MGR2 into the draw buffer and paint it with RefreshMode::Half.
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
  // [u16 w][u16 h][pixels]; no magic, the partition is written by the same build.
  if (!data || sleep_img_size_ < 4)
    return false;
  const int w = data[0] | (data[1] << 8);
  const int h = data[2] | (data[3] << 8);
  const size_t src_stride = (static_cast<size_t>(w) + 3) / 4;
  if (w == 0 || h == 0 || sleep_img_size_ < 4 + src_stride * static_cast<size_t>(h))
    return false;
  data += 4;

  const int rows = std::min(h, kPhysH);
  const int cols = std::min(w, DisplayFrame::kPanelWidth);
  // Anything the image does not cover stays white.
  memset(buf_, 0xFF, kBufSize);
  for (int y = 0; y < rows; ++y) {
    const uint8_t* src_row = data + static_cast<size_t>(y) * src_stride;
    uint8_t* dst = buf_ + static_cast<size_t>(y) * kStride;
    for (int x = 0; x < cols; ++x) {
      // 0 = white, 1 = light, 2 = dark, 3 = black; threshold at the midpoint.
      const uint8_t v = (src_row[x / 4] >> (6 - (x % 4) * 2)) & 0x3;
      if (v >= 2)
        dst[x / 8] &= static_cast<uint8_t>(~(0x80u >> (x % 8)));
    }
  }

  display_.full_refresh(buf_, nullptr, RefreshMode::Half, /*turnOffScreen=*/true);
  return true;
}

}  // namespace wintergreen
