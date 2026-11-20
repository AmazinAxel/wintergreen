#include "DrawBuffer.h"

#include <algorithm>
#include <climits>
#include <cstdlib>

namespace wintergreen {

namespace {

constexpr int kStride = DisplayFrame::kStride;
constexpr int kPhysW = DisplayFrame::kPhysicalWidth;
constexpr int kPhysH = DisplayFrame::kPhysicalHeight;
constexpr int kOffX = DisplayFrame::kPanelOffsetX;
constexpr int kPanelBits = DisplayFrame::kPanelWidth;
constexpr int kFrameRowBytes = 2 * kStride;

// ---------------------------------------------------------------------------
// Op records
// ---------------------------------------------------------------------------
//
// [OpHdr][payload], 4-byte aligned, back to back from the arena's start. The
// header carries the op's physical bounding box (rows [r0, r1), panel bits
// [b0, b1), hidden columns included), so a strip rejects it in O(1), and a
// content hash for matching the two lists. Payloads are copied in and out with
// memcpy: on the host their pointers would be misaligned.

enum OpKind : uint8_t { kFill, kRect, kText, kLine, kBlit, kDiamond };

struct OpHdr {
  uint8_t kind;
  uint8_t mode;   // Level (Fill, Rect), Ink (Text), PixelLayout | kTwoLevel (Blit)
  uint16_t size;  // record bytes
  int16_t r0, r1, b0, b1;
  uint32_t hash;
  uint16_t partner;  // refresh only: matched op's record offset / 4 in the other list
  uint8_t rot;       // 1 = Deg90
  uint8_t matched;   // refresh only
};
static_assert(sizeof(OpHdr) == 20, "OpHdr layout");
constexpr uint8_t kTwoLevel = 0x80;  // Blit mode bit; the low bits are the PixelLayout
constexpr uint16_t kNoPartner = 0xFFFF;

struct TextP {
  const BitmapFont* font;
  const uint8_t* font_data;  // identity: equal data draws equal pixels
  int16_t baseline;
  uint16_t n;  // glyphs that follow: {u16 index, i16 pen x}
  uint8_t style;
  uint8_t pad[3];
};
struct TextGlyph {
  uint16_t idx;
  int16_t x;
};

// A baked line. The words stay in the tail (the page's display list); the op
// holds where (distance from the arena's end, so a parked copy still resolves)
// and, per word, its ink's right edge, so a strip skips words it cannot touch.
struct LineP {
  const BitmapFontSet* fonts;
  uint16_t words_off;  // arena end - first word
  uint16_t end_off;    // arena end - list end
  uint16_t len;        // bytes of the line's words
  int16_t baseline;
  uint8_t word_count;
  uint8_t flags;
  uint16_t pad;
  // then word_count x i16 ink right edge (logical x, exclusive)
};

struct BlitP {
  PixelSource* src;
  uint32_t item, gen, base;
  int16_t x, y, w, h, keep_lo, keep_hi;
};

struct DiamondP {
  int16_t cx, cy, r, t;
};

template <typename T>
inline T load(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

inline uint32_t fnv(uint32_t h, const void* p, size_t n) {
  const auto* b = static_cast<const uint8_t*>(p);
  while (n--) {
    h ^= *b++;
    h *= 16777619u;
  }
  return h;
}

inline uint32_t hdr_hash(const OpHdr& h) {
  const int16_t box[4] = {h.r0, h.r1, h.b0, h.b1};
  uint32_t v = fnv(2166136261u, &h.kind, 2);
  v = fnv(v, box, sizeof(box));
  return fnv(v, &h.rot, 1);
}

inline OpHdr* hdr_at(uint8_t* base, size_t off) {
  return reinterpret_cast<OpHdr*>(base + off);
}
inline const OpHdr* hdr_at(const uint8_t* base, size_t off) {
  return reinterpret_cast<const OpHdr*>(base + off);
}

// ---------------------------------------------------------------------------
// Bit rows
// ---------------------------------------------------------------------------

enum class BlitOp { Copy, Or, AndNot };

template <BlitOp Op>
inline void apply_(uint8_t& d, uint8_t bits, uint8_t mask) {
  if constexpr (Op == BlitOp::Copy)
    d = static_cast<uint8_t>((d & ~mask) | (bits & mask));
  else if constexpr (Op == BlitOp::Or)
    d |= static_cast<uint8_t>(bits & mask);
  else
    d &= static_cast<uint8_t>(~(bits & mask));
}

// One packed source row onto one scratch row, a byte at a time, shifted to the
// target's bit offset. Source bits [lo, hi) of an n-bit row land at row bits
// bit0 + lo ... ([lo, hi) already clipped to the window).
template <BlitOp Op>
inline void put_row_(uint8_t* row, const uint8_t* s, int lo, int hi, int n, int bit0) {
  if (lo == 0 && hi == n && bit0 >= 0) {
    // Unclipped (most glyphs): one shift for the whole row, a mask only on the
    // last byte.
    const int sh = bit0 & 7;
    uint8_t* d = row + (bit0 >> 3);
    const int last = (n - 1) >> 3;
    for (int j = 0; j <= last; ++j, ++d) {
      const auto mask = static_cast<uint8_t>(j == last ? 0xFFu << ((8 - (n & 7)) & 7) : 0xFFu);
      apply_<Op>(d[0], static_cast<uint8_t>(s[j] >> sh), static_cast<uint8_t>(mask >> sh));
      // Only touch the next byte when bits land there: past the row's last
      // byte it is the next row.
      const auto m_lo = static_cast<uint8_t>(mask << (8 - sh));
      if (sh && m_lo)
        apply_<Op>(d[1], static_cast<uint8_t>(s[j] << (8 - sh)), m_lo);
    }
    return;
  }
  for (int j = lo >> 3; j <= (hi - 1) >> 3; ++j) {
    const int a = std::max(lo, 8 * j) - 8 * j;
    const int b = std::min(hi, 8 * j + 8) - 8 * j;
    const auto mask = static_cast<uint8_t>((0xFFu >> a) & (0xFFu << (8 - b)));
    const int start = bit0 + 8 * j;
    const int d = start >> 3;
    const int sh = start & 7;
    // Every unmasked bit lands inside the window, so a non-zero mask is also
    // the bounds check: the byte either side of a clipped edge is never touched.
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

// Set or clear bits [x1, x2) of a row.
inline void span_(uint8_t* row, int x1, int x2, bool set) {
  if (x1 >= x2)
    return;
  const int bx1 = x1 >> 3;
  const int bx2 = (x2 - 1) >> 3;
  const auto lmask = static_cast<uint8_t>(0xFF >> (x1 & 7));
  const auto rmask = static_cast<uint8_t>(0xFF << (7 - ((x2 - 1) & 7)));
  auto put = [&](int i, uint8_t m) {
    if (set)
      row[i] |= m;
    else
      row[i] &= static_cast<uint8_t>(~m);
  };
  if (bx1 == bx2) {
    put(bx1, static_cast<uint8_t>(lmask & rmask));
    return;
  }
  put(bx1, lmask);
  if (bx2 > bx1 + 1)
    std::memset(row + bx1 + 1, set ? 0xFF : 0x00, static_cast<size_t>(bx2 - bx1 - 1));
  put(bx2, rmask);
}

// Where one replay draws: physical rows [ra, rb), panel bits [w0, w1), into a
// scratch strip whose row 0 is physical row `row0`.
struct Clip {
  int ra, rb, w0, w1;
  int row0;
  uint8_t* hi;
  uint8_t* lo;
  uint8_t* hi_row(int r) const { return hi + static_cast<size_t>(r - row0) * kStride; }
  uint8_t* lo_row(int r) const { return lo + static_cast<size_t>(r - row0) * kStride; }
  // Logical x range the clip covers, given the rotation (portrait: physical
  // row R is logical column kPhysH - 1 - R).
  int lx0(bool portrait) const { return portrait ? kPhysH - rb : w0 - kOffX; }
  int lx1(bool portrait) const { return portrait ? kPhysH - ra : w1 - kOffX; }
};

// One source row (both planes) through the clip.
template <BlitOp OpHi, BlitOp OpLo>
inline void put2_(const Clip& c, int R, const uint8_t* s_hi, const uint8_t* s_lo, int lo, int hi, int n, int bit0) {
  // Never into the hidden columns: logical y < 0 (portrait) or x < 0 (landscape).
  lo = std::max(lo, std::max(c.w0, kOffX) - bit0);
  hi = std::min(hi, c.w1 - bit0);
  if (lo >= hi)
    return;
  put_row_<OpHi>(c.hi_row(R), s_hi, lo, hi, n, bit0);
  put_row_<OpLo>(c.lo_row(R), s_lo, lo, hi, n, bit0);
}

// Gray pixels of physical row R, bits [x1, x2) (clipped like put2_), become a
// 1 px checkerboard of black and white: two levels, the same mean tone.
inline void two_level_(const Clip& c, int R, int x1, int x2) {
  x1 = std::max(x1, std::max(c.w0, kOffX));
  x2 = std::min(x2, c.w1);
  if (x1 >= x2)
    return;
  uint8_t* h = c.hi_row(R);
  uint8_t* l = c.lo_row(R);
  const auto pat = static_cast<uint8_t>((R & 1) ? 0xAA : 0x55);
  for (int j = x1 >> 3; j <= (x2 - 1) >> 3; ++j) {
    const int a = std::max(x1, 8 * j) - 8 * j, b = std::min(x2, 8 * j + 8) - 8 * j;
    const auto m = static_cast<uint8_t>((0xFFu >> a) & (0xFFu << (8 - b)));
    const auto g = static_cast<uint8_t>(h[j] & ~l[j] & m);
    l[j] = static_cast<uint8_t>(l[j] | (g & pat));
    h[j] = static_cast<uint8_t>((h[j] & ~g) | (g & pat));
  }
}

// Black ink adds (OR both planes); white ink takes away: the glyph's black core
// clears gray (hi), its whole coverage clears black (lo) — black under an AA edge
// becomes gray, under the core white.
template <BlitOp OpHi, BlitOp OpLo, bool Swap>
inline void glyph_rows_(const Clip& c, bool portrait, const GlyphData& g, int gx, int gy) {
  const int w = g.bitmap_width, h = g.bitmap_height;
  if (portrait) {
    const size_t cs = static_cast<size_t>((h + 7) / 8);
    const uint8_t* hiP = g.cols;
    const uint8_t* loP = g.cols + cs * w;
    const int c_lo = std::max(0, kPhysH - gx - c.rb);
    const int c_hi = std::min(w, kPhysH - gx - c.ra);
    for (int col = c_lo; col < c_hi; ++col) {
      const uint8_t* a = hiP + col * cs;
      const uint8_t* b = loP + col * cs;
      put2_<OpHi, OpLo>(c, kPhysH - 1 - gx - col, Swap ? b : a, Swap ? a : b, 0, h, h, gy + kOffX);
    }
  } else {
    const size_t rs = static_cast<size_t>((w + 7) / 8);
    const uint8_t* hiP = g.bits;
    const uint8_t* loP = g.bits + rs * h;
    const int r_lo = std::max(0, c.ra - gy);
    const int r_hi = std::min(h, c.rb - gy);
    for (int r = r_lo; r < r_hi; ++r) {
      const uint8_t* a = hiP + r * rs;
      const uint8_t* b = loP + r * rs;
      put2_<OpHi, OpLo>(c, gy + r, Swap ? b : a, Swap ? a : b, 0, w, w, gx + kOffX);
    }
  }
}

inline void glyph_(const Clip& c, bool portrait, const GlyphData& g, int gx, int gy, Ink ink) {
  if (!g.bits || !g.cols)
    return;
  if (ink == Ink::Black)
    glyph_rows_<BlitOp::Or, BlitOp::Or, false>(c, portrait, g, gx, gy);
  else
    // hi &= ~g.lo, lo &= ~g.hi: pass the planes swapped.
    glyph_rows_<BlitOp::AndNot, BlitOp::AndNot, true>(c, portrait, g, gx, gy);
}

// Does the glyph's box meet the clip's logical x range?
inline bool glyph_in_x_(const Clip& c, bool portrait, int gx, int w) {
  return gx + w > c.lx0(portrait) && gx < c.lx1(portrait);
}

// Coverage of a diamond pixel in sixteenths (4x4 samples), in eighths of a
// pixel so it stays integer. Centred on pixel (cx, cy)'s centre; the edge runs
// through the centres of the pixels at |dx| + |dy| = r, so the staircase the
// 1-bit rows drew becomes a run of half-covered (gray) pixels.
inline int diamond_cov_(int px, int py, int cx, int cy, int r, int t) {
  int n = 0;
  const int outer = 8 * r, inner = outer - 8 * t;
  for (int j = 0; j < 4; ++j)
    for (int i = 0; i < 4; ++i) {
      const int dx = std::abs(8 * (px - cx) + 2 * i + 1 - 4);
      const int dy = std::abs(8 * (py - cy) + 2 * j + 1 - 4);
      const int d = dx + dy;
      if (d <= outer && (t == 0 || d > inner))
        ++n;
    }
  return n;
}

// Same thresholds as tools/make_font.py, in sixteenths: 64/255 and 168/255.
constexpr int kCovGray = 4;
constexpr int kCovBlack = 11;

inline size_t align4(size_t n) {
  return (n + 3) & ~static_cast<size_t>(3);
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

class DrawBuffer::MemorySource final : public PixelSource {
 public:
  const uint8_t* data = nullptr;
  size_t size = 0;
  bool read(uint32_t, uint32_t offset, uint8_t* dst, size_t n) override {
    if (!data || offset > size || n > size - offset)
      return false;
    std::memcpy(dst, data + offset, n);
    return true;
  }
};

namespace {
// One allocation: two arenas, the new-scratch strip, two old/send strips (+4
// for the RED plane's alignment), the fetch buffer.
constexpr size_t kSendBytes = DrawBuffer::kStripBytes + 4;
// match_() keeps both lists' record offsets in the fetch buffer.
static_assert(DrawBuffer::kArenaBytes / sizeof(OpHdr) <= DrawBuffer::kFetchBytes / 4, "fetch buffer holds the match");
constexpr size_t kMemBytes = 2 * DrawBuffer::kArenaBytes + DrawBuffer::kStripBytes + 2 * kSendBytes +
                             DrawBuffer::kFetchBytes;
}  // namespace

DrawBuffer::DrawBuffer(IDisplay& display) : display_(display) {
  mem_ = static_cast<uint8_t*>(std::malloc(kMemBytes));
  sleep_src_ = new MemorySource();
  if (!mem_)
    return;
  uint8_t* p = mem_;
  arenas_[0].base = p;
  p += kArenaBytes;
  arenas_[1].base = p;
  p += kArenaBytes;
  scratch_new_ = p;
  p += kStripBytes;
  scratch_old_[0] = p;
  p += kSendBytes;
  scratch_old_[1] = p;
  p += kSendBytes;
  fetch_ = p;
}

DrawBuffer::~DrawBuffer() {
  std::free(mem_);
  delete sleep_src_;
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

// A draw without fill() continues the screen on the glass, as drawing into a
// framebuffer did: the current list starts as a copy of it.
void DrawBuffer::ensure_started_() {
  if (cur_started_)
    return;
  cur_started_ = true;
  Arena& c = cur();
  const Arena& p = prev();
  parked_ = false;
  c.overflowed = p.overflowed || !prev_valid_;
  std::memcpy(c.base, p.base, p.used);
  c.used = p.used;
  std::memcpy(c.base + kArenaBytes - p.tail_len, p.base + kArenaBytes - p.tail_len, p.tail_len);
  c.tail_len = p.tail_len;
  c.tail_tag = p.tail_tag;
}

uint8_t* DrawBuffer::begin_op_(size_t payload) {
  if (!mem_)
    return nullptr;
  ensure_started_();
  const size_t size = align4(sizeof(OpHdr) + payload);
  Arena& a = cur();
  if (a.used + size > kArenaBytes - a.tail_len && parked_)
    drop_snapshot();  // the overlay needs the room; the page is re-read on close
  if (a.used + size > kArenaBytes - a.tail_len) {
    a.overflowed = true;
    return nullptr;
  }
  uint8_t* rec = a.base + a.used;
  a.used += size;
  OpHdr h{};
  h.size = static_cast<uint16_t>(size);
  h.partner = kNoPartner;
  h.rot = rotation_ == Rotation::Deg90 ? 1 : 0;
  std::memcpy(rec, &h, sizeof(h));
  return rec;
}

void DrawBuffer::phys_rect_(int x, int y, int w, int h, int& r0, int& r1, int& b0, int& b1) const {
  if (rotation_ == Rotation::Deg0) {
    r0 = y;
    r1 = y + h;
    b0 = x + kOffX;
    b1 = x + w + kOffX;
  } else {
    r0 = kPhysH - x - w;
    r1 = kPhysH - x;
    b0 = y + kOffX;
    b1 = y + h + kOffX;
  }
  r0 = std::max(r0, 0);
  r1 = std::min(r1, kPhysH);
  b0 = std::max(b0, kOffX);
  b1 = std::min(b1, kPanelBits);
}

namespace {
inline void finish_hdr_(uint8_t* rec, uint8_t kind, uint8_t mode, int r0, int r1, int b0, int b1, uint32_t payload_hash) {
  OpHdr h;
  std::memcpy(&h, rec, sizeof(h));
  h.kind = kind;
  h.mode = mode;
  h.r0 = static_cast<int16_t>(r0);
  h.r1 = static_cast<int16_t>(r1);
  h.b0 = static_cast<int16_t>(b0);
  h.b1 = static_cast<int16_t>(b1);
  h.hash = payload_hash ^ hdr_hash(h);
  std::memcpy(rec, &h, sizeof(h));
}
}  // namespace

void DrawBuffer::fill(Level level) {
  cur_started_ = true;
  Arena& a = cur();
  a.used = 0;
  a.overflowed = false;
  uint8_t* rec = begin_op_(0);
  if (rec)
    finish_hdr_(rec, kFill, static_cast<uint8_t>(level), 0, kPhysH, 0, kPanelBits, 0);
}

void DrawBuffer::fill_rect(int x, int y, int w, int h, Level level) {
  if (w <= 0 || h <= 0)
    return;
  int r0, r1, b0, b1;
  phys_rect_(x, y, w, h, r0, r1, b0, b1);
  if (r0 >= r1 || b0 >= b1)
    return;
  uint8_t* rec = begin_op_(0);
  if (rec)
    finish_hdr_(rec, kRect, static_cast<uint8_t>(level), r0, r1, b0, b1, 0);
}

void DrawBuffer::draw_outline(int x, int y, int w, int h, int t, Level level) {
  if (w <= 0 || h <= 0)
    return;
  t = std::min({t, w, h});
  fill_rect(x, y, w, t, level);
  fill_rect(x, y + h - t, w, t, level);
  fill_rect(x, y + t, t, h - 2 * t, level);
  fill_rect(x + w - t, y + t, t, h - 2 * t, level);
}

void DrawBuffer::draw_diamond(int cx, int cy, int r, int t) {
  int r0, r1, b0, b1;
  phys_rect_(cx - r - 1, cy - r - 1, 2 * r + 3, 2 * r + 3, r0, r1, b0, b1);
  if (r0 >= r1 || b0 >= b1)
    return;
  uint8_t* rec = begin_op_(sizeof(DiamondP));
  if (!rec)
    return;
  const DiamondP d{static_cast<int16_t>(cx), static_cast<int16_t>(cy), static_cast<int16_t>(r), static_cast<int16_t>(t)};
  std::memcpy(rec + sizeof(OpHdr), &d, sizeof(d));
  finish_hdr_(rec, kDiamond, 0, r0, r1, b0, b1, fnv(2166136261u, &d, sizeof(d)));
}

int DrawBuffer::draw_text(int x, int baseline_y, const char* text, size_t len, const BitmapFont& font, Ink ink,
                          FontStyle style) {
  if (!text || len == 0 || !font.valid())
    return x;
  const BitmapFont::Styled f = font.styled(style);
  // Resolve once, at record time: glyph index and pen x per drawn glyph, so a
  // strip replays without UTF-8 decoding, range searches or kerning. Room for
  // one glyph per codepoint; the record shrinks to the drawn ones below.
  size_t cps = 0;
  for (size_t k = 0; k < len; ++k)
    cps += (static_cast<uint8_t>(text[k]) & 0xC0) != 0x80;
  uint8_t* rec = begin_op_(sizeof(TextP) + cps * sizeof(TextGlyph));
  size_t drawn = 0;
  int cursor_q = x * 4;
  int prev_idx = -1;
  int lx0 = INT_MAX, lx1 = INT_MIN, ly0 = INT_MAX, ly1 = INT_MIN;
  uint8_t* out = rec ? rec + sizeof(OpHdr) + sizeof(TextP) : nullptr;
  const char* p = text;
  const char* e = text + len;
  uint32_t hash = 2166136261u;
  while (p < e) {
    const int idx = f.glyph_index(BitmapFont::decode_utf8(p, e));
    cursor_q += f.kerning_q(prev_idx, idx);
    const GlyphData g = f.glyph_data_at(idx);
    const int pen = (cursor_q + 2) / 4;
    if (g.bits && out) {
      const TextGlyph tg{static_cast<uint16_t>(idx), static_cast<int16_t>(pen)};
      std::memcpy(out, &tg, sizeof(tg));
      hash = fnv(hash, &tg, sizeof(tg));
      out += sizeof(tg);
      ++drawn;
      lx0 = std::min(lx0, pen + g.x_offset);
      lx1 = std::max(lx1, pen + g.x_offset + g.bitmap_width);
      ly0 = std::min(ly0, baseline_y + g.y_offset);
      ly1 = std::max(ly1, baseline_y + g.y_offset + g.bitmap_height);
    }
    cursor_q += g.advance_width;
    cursor_q = ((cursor_q + 2) / 4) * 4;  // whole pixels per glyph, as word_width() measures
    prev_idx = idx;
  }
  if (!rec)
    return cursor_q / 4;
  // Shrink to what was drawn (the record is the last one).
  OpHdr hd = load<OpHdr>(rec);
  cur().used -= hd.size;
  int r0 = 0, r1 = 0, b0 = 0, b1 = 0;
  if (drawn)
    phys_rect_(lx0, ly0, lx1 - lx0, ly1 - ly0, r0, r1, b0, b1);
  if (r0 >= r1 || b0 >= b1)
    return cursor_q / 4;  // nothing drawn, or entirely off the panel
  hd.size = static_cast<uint16_t>(align4(sizeof(OpHdr) + sizeof(TextP) + drawn * sizeof(TextGlyph)));
  std::memcpy(rec, &hd, sizeof(hd));
  cur().used += hd.size;
  const TextP tp{&font, font.data(), static_cast<int16_t>(baseline_y), static_cast<uint16_t>(drawn),
                 static_cast<uint8_t>(style), {}};
  std::memcpy(rec + sizeof(OpHdr), &tp, sizeof(tp));
  hash = fnv(hash, &tp.font_data, sizeof(tp.font_data));
  hash = fnv(hash, &tp.baseline, sizeof(tp.baseline));
  hash = fnv(hash, &tp.style, 1);
  finish_hdr_(rec, kText, static_cast<uint8_t>(ink), r0, r1, b0, b1, hash);
  return cursor_q / 4;
}

namespace {
inline int valign_shift_(const BitmapFont* f, uint8_t valign) {
  if (valign == static_cast<uint8_t>(VerticalAlign::Super))
    return -static_cast<int>(f->y_advance(100)) * 20 / 100;
  if (valign == static_cast<uint8_t>(VerticalAlign::Sub))
    return static_cast<int>(f->y_advance(100)) * 20 / 100;
  return 0;
}
}  // namespace

void DrawBuffer::draw_baked_line(const WgbLine& line, const uint8_t* end, const BitmapFontSet& fonts) {
  if (!mem_ || line.word_count == 0 || !line.words)
    return;
  ensure_started_();
  const Arena& a = cur();
  const uint8_t* aend = a.base + kArenaBytes;
  const uint8_t* tail = aend - a.tail_len;
  if (line.words < tail || end > aend || line.words >= end)
    return;  // not in the tail: the op could not outlive the caller's buffer
  uint8_t* rec = begin_op_(sizeof(LineP) + 2u * line.word_count);
  if (!rec)
    return;
  int16_t* xe = reinterpret_cast<int16_t*>(rec + sizeof(OpHdr) + sizeof(LineP));
  int lx0 = INT_MAX, lx1 = INT_MIN, ly0 = INT_MAX, ly1 = INT_MIN;
  WgbWordIter it(line, end);
  WgbWord w;
  int n = 0;
  while (n < line.word_count && it.next(w)) {
    int right = w.x;
    const BitmapFont* f = fonts.get_font(w.font);
    if (f && f->valid()) {
      const int base = line.baseline_y + valign_shift_(f, w.valign);
      const BitmapFont::Styled sf = f->styled(static_cast<FontStyle>(w.style));
      int cursor_q = w.x * 4;
      int prev_idx = -1;
      const uint8_t* p = w.glyphs;
      const uint8_t* e = w.glyphs + w.len;
      while (p < e) {
        const int idx = wgb_read_glyph(p, e);
        cursor_q += sf.kerning_q(prev_idx, idx);
        const GlyphData g = sf.glyph_data_at(idx);
        if (g.bits) {
          const int gx = (cursor_q + 2) / 4 + g.x_offset;
          lx0 = std::min(lx0, gx);
          lx1 = std::max(lx1, gx + g.bitmap_width);
          right = std::max(right, gx + g.bitmap_width);
          ly0 = std::min(ly0, base + g.y_offset);
          ly1 = std::max(ly1, base + g.y_offset + g.bitmap_height);
        }
        cursor_q += g.advance_width;
        cursor_q = ((cursor_q + 2) / 4) * 4;
        prev_idx = idx;
      }
    }
    const int16_t r16 = static_cast<int16_t>(right);
    std::memcpy(xe + n, &r16, sizeof(r16));
    ++n;
  }
  int r0 = 0, r1 = 0, b0 = 0, b1 = 0;
  if (lx0 < lx1)
    phys_rect_(lx0, ly0, lx1 - lx0, ly1 - ly0, r0, r1, b0, b1);
  if (r0 >= r1 || b0 >= b1) {
    cur().used -= load<OpHdr>(rec).size;
    return;
  }
  const LineP lp{&fonts,
                 static_cast<uint16_t>(aend - line.words),
                 static_cast<uint16_t>(aend - end),
                 static_cast<uint16_t>(it.pos() - line.words),
                 static_cast<int16_t>(line.baseline_y),
                 static_cast<uint8_t>(n),
                 line.flags,
                 0};
  std::memcpy(rec + sizeof(OpHdr), &lp, sizeof(lp));
  uint32_t hash = fnv(2166136261u, line.words, lp.len);
  hash = fnv(hash, &lp.fonts, sizeof(lp.fonts));
  hash = fnv(hash, &lp.baseline, sizeof(lp.baseline));
  hash = fnv(hash, &lp.flags, 2);
  finish_hdr_(rec, kLine, 0, r0, r1, b0, b1, hash);
}

void DrawBuffer::blit(PixelSource* src, uint32_t item, uint32_t gen, uint32_t base, PixelLayout layout, int x, int y,
                      int w, int h, int keep_lo, int keep_hi, bool two_level) {
  if (!src || w <= 0 || h <= 0)
    return;
  keep_lo = std::max(keep_lo, 0);
  keep_hi = std::min(keep_hi, h);
  if (keep_lo >= keep_hi)
    return;
  int r0, r1, b0, b1;
  if (layout == PixelLayout::Frame) {
    x = y = 0;
    r0 = 0;
    r1 = kPhysH;
    b0 = 0;
    b1 = kPanelBits;
  } else {
    // A layout made for the other orientation would need a transpose; the
    // converter never produces one.
    if ((layout == PixelLayout::Cols) != (rotation_ == Rotation::Deg90))
      return;
    phys_rect_(x, y + keep_lo, w, keep_hi - keep_lo, r0, r1, b0, b1);
  }
  if (r0 >= r1 || b0 >= b1)
    return;
  uint8_t* rec = begin_op_(sizeof(BlitP));
  if (!rec)
    return;
  const BlitP bp{src,
                 item,
                 gen,
                 base,
                 static_cast<int16_t>(x),
                 static_cast<int16_t>(y),
                 static_cast<int16_t>(w),
                 static_cast<int16_t>(h),
                 static_cast<int16_t>(keep_lo),
                 static_cast<int16_t>(keep_hi)};
  std::memcpy(rec + sizeof(OpHdr), &bp, sizeof(bp));
  const auto mode = static_cast<uint8_t>(static_cast<uint8_t>(layout) | (two_level ? kTwoLevel : 0));
  finish_hdr_(rec, kBlit, mode, r0, r1, b0, b1, fnv(2166136261u, &bp, sizeof(bp)));
}

// ---------------------------------------------------------------------------
// Tail and snapshot
// ---------------------------------------------------------------------------

uint8_t* DrawBuffer::claim_tail(size_t n, uint32_t tag) {
  if (!mem_)
    return nullptr;
  if (parked_)
    drop_snapshot();
  // A new screen starts here (fill() follows): what the list held is stale.
  Arena& a = cur();
  a.used = 0;
  a.overflowed = false;
  cur_started_ = false;
  if (n > kArenaBytes) {
    a.tail_len = 0;
    a.tail_tag = 0;
    return nullptr;
  }
  a.tail_len = n;
  a.tail_tag = tag;
  return a.base + kArenaBytes - n;
}

void DrawBuffer::save_snapshot() {
  drop_snapshot();
  const Arena& p = prev();
  Arena& c = cur();
  const size_t n = p.used + p.tail_len;
  if (!mem_ || !prev_valid_ || p.overflowed || n > kArenaBytes)
    return;
  if (!cur_started_)
    c.used = 0;
  if (c.used > kArenaBytes - n)
    return;
  // [ops][tail] ending at the arena's end: the tail keeps its distance from the
  // end, so the line ops' offsets still resolve.
  uint8_t* dst = c.base + kArenaBytes - n;
  std::memcpy(dst, p.base, p.used);
  std::memcpy(dst + p.used, p.base + kArenaBytes - p.tail_len, p.tail_len);
  c.tail_len = n;
  c.tail_tag = p.tail_tag;
  c.parked_ops = p.used;
  parked_ = true;
}

bool DrawBuffer::restore_snapshot() {
  if (!parked_)
    return false;
  Arena& c = cur();
  const uint8_t* src = c.base + kArenaBytes - c.tail_len;
  std::memmove(c.base, src, c.parked_ops);
  c.used = c.parked_ops;
  c.tail_len -= c.parked_ops;
  c.parked_ops = 0;
  c.overflowed = false;
  parked_ = false;
  cur_started_ = true;
  return true;
}

void DrawBuffer::drop_snapshot() {
  if (!parked_)
    return;
  parked_ = false;
  Arena& c = cur();
  c.tail_len = 0;
  c.tail_tag = 0;
  c.parked_ops = 0;
}

// ---------------------------------------------------------------------------
// Matching and damage
// ---------------------------------------------------------------------------

namespace {

bool same_op_(const uint8_t* ab, const OpHdr& a, const uint8_t* bb, const OpHdr& b) {
  if (a.hash != b.hash || a.kind != b.kind || a.mode != b.mode || a.size != b.size || a.r0 != b.r0 ||
      a.r1 != b.r1 || a.b0 != b.b0 || a.b1 != b.b1 || a.rot != b.rot)
    return false;
  const uint8_t* pa = reinterpret_cast<const uint8_t*>(&a) + sizeof(OpHdr);
  const uint8_t* pb = reinterpret_cast<const uint8_t*>(&b) + sizeof(OpHdr);
  switch (a.kind) {
    case kFill:
    case kRect:
      return true;
    case kText: {
      const TextP ta = load<TextP>(pa), tb = load<TextP>(pb);
      return ta.font_data == tb.font_data && ta.baseline == tb.baseline && ta.n == tb.n && ta.style == tb.style &&
             std::memcmp(pa + sizeof(TextP), pb + sizeof(TextP), ta.n * sizeof(TextGlyph)) == 0;
    }
    case kLine: {
      const LineP la = load<LineP>(pa), lb = load<LineP>(pb);
      if (la.fonts != lb.fonts || la.baseline != lb.baseline || la.flags != lb.flags ||
          la.word_count != lb.word_count || la.len != lb.len)
        return false;
      const uint8_t* wa = ab + DrawBuffer::kArenaBytes - la.words_off;
      const uint8_t* wb = bb + DrawBuffer::kArenaBytes - lb.words_off;
      return std::memcmp(wa, wb, la.len) == 0;
    }
    default:
      return std::memcmp(pa, pb, static_cast<size_t>(a.size) - sizeof(OpHdr)) == 0;
  }
}

}  // namespace

void DrawBuffer::add_damage_(int r0, int r1, int b0, int b1) {
  r0 = std::max(r0, 0);
  r1 = std::min(r1, kPhysH);
  const int x0 = std::max(b0, 0) >> 3;
  const int x1 = std::min((b1 + 7) >> 3, kStride);
  if (r0 >= r1 || x0 >= x1)
    return;
  for (int s = r0 / kStripRows; s <= (r1 - 1) / kStripRows; ++s) {
    Interval* iv = iv_[s];
    uint8_t& n = iv_n_[s];
    int lo = x0, hi = x1;
    // Merge everything it touches.
    for (int i = 0; i < n;) {
      if (iv[i].hi >= lo && iv[i].lo <= hi) {
        lo = std::min<int>(lo, iv[i].lo);
        hi = std::max<int>(hi, iv[i].hi);
        iv[i] = iv[--n];
      } else {
        ++i;
      }
    }
    if (n == kMaxIntervals) {
      // Full: fold the new one into its nearest neighbour.
      int best = 0, gap = INT_MAX;
      for (int i = 0; i < n; ++i) {
        const int g = iv[i].lo >= hi ? iv[i].lo - hi : lo - iv[i].hi;
        if (g < gap) {
          gap = g;
          best = i;
        }
      }
      lo = std::min<int>(lo, iv[best].lo);
      hi = std::max<int>(hi, iv[best].hi);
      iv[best] = iv[--n];
      // The widened one may now touch others.
      for (int i = 0; i < n;) {
        if (iv[i].hi >= lo && iv[i].lo <= hi) {
          lo = std::min<int>(lo, iv[i].lo);
          hi = std::max<int>(hi, iv[i].hi);
          iv[i] = iv[--n];
        } else {
          ++i;
        }
      }
    }
    iv[n++] = Interval{static_cast<uint8_t>(lo), static_cast<uint8_t>(hi)};
  }
}

void DrawBuffer::add_unknown_(int r0, int r1, int b0, int b1) {
  add_damage_(r0, r1, b0, b1);
  if (unknown_n_ == kMaxUnknown) {
    all_unknown_ = true;
    return;
  }
  unknown_[unknown_n_++] = Rect{static_cast<int16_t>(r0), static_cast<int16_t>(r1), static_cast<int16_t>(b0),
                                static_cast<int16_t>(b1)};
}

// Pair the glass's ops with the new ones as a common subsequence (equal ops in
// the same relative order). A pixel no unpaired op covers is drawn by the same
// ops in the same order in both lists, so it cannot have changed: only unpaired
// ops' boxes are damage. Greedy with a bounded look-ahead: a worse pairing only
// costs damage, never correctness.
void DrawBuffer::match_() {
  for (int s = 0; s < kStrips; ++s)
    iv_n_[s] = 0;
  unknown_n_ = 0;
  all_unknown_ = false;

  Arena& P = prev();
  Arena& C = cur();
  // Offsets / 4 of each list's records, in the fetch buffer (free until the
  // strips start).
  uint16_t* po = reinterpret_cast<uint16_t*>(fetch_);
  uint16_t* co = po + kFetchBytes / 4;
  const size_t cap = kFetchBytes / 4;
  size_t np = 0, nc = 0;
  for (size_t off = 0; off < P.used && np < cap; off += hdr_at(P.base, off)->size) {
    OpHdr* h = hdr_at(P.base, off);
    h->matched = 0;
    h->partner = kNoPartner;
    po[np++] = static_cast<uint16_t>(off / 4);
  }
  for (size_t off = 0; off < C.used && nc < cap; off += hdr_at(C.base, off)->size) {
    OpHdr* h = hdr_at(C.base, off);
    h->matched = 0;
    h->partner = kNoPartner;
    co[nc++] = static_cast<uint16_t>(off / 4);
  }

  if (!prev_valid_ || P.overflowed || C.overflowed) {
    all_unknown_ = true;
    add_damage_(0, kPhysH, 0, kPanelBits);
    return;
  }

  auto ph = [&](size_t i) { return hdr_at(P.base, static_cast<size_t>(po[i]) * 4); };
  auto ch = [&](size_t j) { return hdr_at(C.base, static_cast<size_t>(co[j]) * 4); };
  auto pair = [&](size_t i, size_t j) {
    ph(i)->matched = 1;
    ch(j)->matched = 1;
    ph(i)->partner = co[j];
  };
  static constexpr size_t kLook = 32;
  size_t i = 0, j = 0;
  while (i < np && j < nc) {
    if (same_op_(P.base, *ph(i), C.base, *ch(j))) {
      pair(i, j);
      ++i;
      ++j;
      continue;
    }
    size_t a = 0, b = 0;
    for (size_t k = 1; k <= kLook && j + k < nc; ++k)
      if (same_op_(P.base, *ph(i), C.base, *ch(j + k))) {
        a = k;
        break;
      }
    for (size_t k = 1; k <= kLook && i + k < np; ++k)
      if (same_op_(P.base, *ph(i + k), C.base, *ch(j))) {
        b = k;
        break;
      }
    if (a && (!b || a <= b))
      j += a;  // cur ops j .. j+a-1 are new
    else if (b)
      i += b;  // prev ops i .. i+b-1 are gone
    else {
      ++i;
      ++j;
    }
  }

  for (size_t k = 0; k < nc; ++k) {
    const OpHdr* h = ch(k);
    if (!h->matched)
      add_damage_(h->r0, h->r1, h->b0, h->b1);
  }
  for (size_t k = 0; k < np; ++k) {
    const OpHdr* h = ph(k);
    if (h->matched)
      continue;
    // A gone image cannot be replayed (its file is not read again): what it
    // showed is unknown, so its pixels are all driven to their new level.
    if (h->kind == kBlit)
      add_unknown_(h->r0, h->r1, h->b0, h->b1);
    else
      add_damage_(h->r0, h->r1, h->b0, h->b1);
  }
}

// ---------------------------------------------------------------------------
// Strips
// ---------------------------------------------------------------------------

// Read every pixel source the current list needs for rows [ra, rb) into the
// fetch buffer. False when it would not fit: the caller takes fewer rows.
bool DrawBuffer::fetch_chunk_(int ra, int rb, Fetch* fetches, int& nf) {
  static constexpr int kMaxFetch = 16;
  nf = 0;
  const Arena& C = cur();
  const int s = ra / kStripRows;
  size_t used = 0;
  bool bus_free = false;
  for (size_t off = 0; off < C.used; off += hdr_at(C.base, off)->size) {
    const OpHdr* h = hdr_at(C.base, off);
    if (h->kind != kBlit || h->r1 <= ra || h->r0 >= rb)
      continue;
    bool hit = all_unknown_;
    for (int k = 0; k < iv_n_[s] && !hit; ++k)
      hit = h->b1 > 8 * iv_[s][k].lo && h->b0 < 8 * iv_[s][k].hi;
    if (!hit)
      continue;
    const BlitP bp = load<BlitP>(reinterpret_cast<const uint8_t*>(h) + sizeof(OpHdr));
    const auto layout = static_cast<PixelLayout>(h->mode & ~kTwoLevel);
    int first = 0, count = 0;
    size_t unit = 0;
    if (layout == PixelLayout::Cols) {
      unit = 2 * static_cast<size_t>((bp.h + 7) / 8);
      first = std::max(0, kPhysH - bp.x - rb);
      const int last = std::min<int>(bp.w, kPhysH - bp.x - ra);
      count = last - first;
    } else if (layout == PixelLayout::Rows) {
      unit = 2 * static_cast<size_t>((bp.w + 7) / 8);
      first = std::max<int>({0, ra - bp.y, bp.keep_lo});
      const int last = std::min<int>({bp.h, rb - bp.y, bp.keep_hi});
      count = last - first;
    } else {
      unit = kFrameRowBytes;
      first = ra;
      count = rb - ra;
    }
    if (count <= 0)
      continue;
    const size_t n = unit * static_cast<size_t>(count);
    if (nf == kMaxFetch || used + n > kFetchBytes)
      return false;
    if (!bus_free) {
      display_.finish_writes();  // the card shares the bus with queued panel writes
      bus_free = true;
    }
    uint8_t* dst = fetch_ + used;
    if (!bp.src->read(bp.item, bp.base + static_cast<uint32_t>(unit * first), dst, n))
      std::memset(dst, 0, n);
    fetches[nf++] = Fetch{static_cast<uint32_t>(off), dst, first};
    used += n;
  }
  return true;
}

// Replay list `a` into `planes` (hi plane, then lo plane kPlaneBytes later) for
// physical rows [ra, rb) and plane bytes [xa, xb). The scratch row 0 is the
// strip's first row (ra rounded down to the strip).
void DrawBuffer::render_(const Arena& a, bool old, int ra, int rb, int xa, int xb, uint8_t* planes,
                         const Fetch* fetches, int nf) {
  const int row0 = (ra / kStripRows) * kStripRows;
  Clip c{ra, rb, 8 * xa, 8 * xb, row0, planes, planes + kPlaneBytes};
  for (size_t off = 0; off < a.used; off += hdr_at(a.base, off)->size) {
    const OpHdr* h = hdr_at(a.base, off);
    if (h->r1 <= ra || h->r0 >= rb || h->b1 <= c.w0 || h->b0 >= c.w1)
      continue;
    const uint8_t* pl = reinterpret_cast<const uint8_t*>(h) + sizeof(OpHdr);
    const bool portrait = h->rot != 0;
    switch (h->kind) {
      case kFill:
      case kRect: {
        const int r_lo = std::max<int>(h->r0, ra), r_hi = std::min<int>(h->r1, rb);
        const int x1 = std::max<int>(h->b0, c.w0), x2 = std::min<int>(h->b1, c.w1);
        const bool sh = (h->mode & 2) != 0, sl = (h->mode & 1) != 0;
        for (int r = r_lo; r < r_hi; ++r) {
          span_(c.hi_row(r), x1, x2, sh);
          span_(c.lo_row(r), x1, x2, sl);
        }
        break;
      }
      case kText: {
        const TextP tp = load<TextP>(pl);
        const BitmapFont::Styled f = tp.font->styled(static_cast<FontStyle>(tp.style));
        const uint8_t* gp = pl + sizeof(TextP);
        const int X0 = c.lx0(portrait), X1 = c.lx1(portrait);
        for (uint16_t k = 0; k < tp.n; ++k, gp += sizeof(TextGlyph)) {
          const TextGlyph tg = load<TextGlyph>(gp);
          const GlyphData g = f.glyph_data_at(tg.idx);
          const int gx = tg.x + g.x_offset;
          if (gx >= X1 || gx + g.bitmap_width <= X0)
            continue;
          glyph_(c, portrait, g, gx, tp.baseline + g.y_offset, static_cast<Ink>(h->mode));
        }
        break;
      }
      case kLine: {
        const LineP lp = load<LineP>(pl);
        const uint8_t* aend = a.base + kArenaBytes;
        WgbLine line;
        line.baseline_y = static_cast<uint16_t>(lp.baseline);
        line.word_count = lp.word_count;
        line.flags = lp.flags;
        line.words = aend - lp.words_off;
        WgbWordIter it(line, aend - lp.end_off);
        WgbWord w;
        const uint8_t* xe = pl + sizeof(LineP);
        const int X0 = c.lx0(portrait), X1 = c.lx1(portrait);
        for (int n = 0; it.next(w); ++n) {
          const int right = load<int16_t>(xe + 2 * n);
          const BitmapFont* f = lp.fonts->get_font(w.font);
          if (right <= X0 || !f || !f->valid() || w.x + f->min_x_offset() >= X1)
            continue;
          const int base = lp.baseline + valign_shift_(f, w.valign);
          const BitmapFont::Styled sf = f->styled(static_cast<FontStyle>(w.style));
          int cursor_q = w.x * 4;
          int prev_idx = -1;
          const uint8_t* p = w.glyphs;
          const uint8_t* e = w.glyphs + w.len;
          while (p < e) {
            const int idx = wgb_read_glyph(p, e);
            cursor_q += sf.kerning_q(prev_idx, idx);
            const int pen = (cursor_q + 2) / 4;
            // Glyphs right of here cannot reach back into the clip (their ink
            // starts at least min_x_offset left of their pen).
            if (pen + f->min_x_offset() >= X1)
              break;
            const GlyphData g = sf.glyph_data_at(idx);
            const int gx = pen + g.x_offset;
            if (g.bits && gx + g.bitmap_width > X0)
              glyph_(c, portrait, g, gx, base + g.y_offset, Ink::Black);
            cursor_q += g.advance_width;
            cursor_q = ((cursor_q + 2) / 4) * 4;
            prev_idx = idx;
          }
        }
        break;
      }
      case kBlit: {
        // Old pixels of an image come only from the fetch of its identical
        // partner in the new list; an unpaired one is unknown (never re-read).
        const uint32_t want = old ? (h->partner == kNoPartner ? UINT32_MAX : 4u * h->partner)
                                  : static_cast<uint32_t>(off);
        const Fetch* fe = nullptr;
        for (int k = 0; k < nf; ++k)
          if (fetches[k].op == want)
            fe = &fetches[k];
        if (!fe)
          break;
        const BlitP bp = load<BlitP>(pl);
        const auto layout = static_cast<PixelLayout>(h->mode & ~kTwoLevel);
        const bool two = (h->mode & kTwoLevel) != 0;
        if (layout == PixelLayout::Cols) {
          const size_t cb = static_cast<size_t>((bp.h + 7) / 8);
          const int c_lo = std::max(0, kPhysH - bp.x - rb);
          const int c_hi = std::min<int>(bp.w, kPhysH - bp.x - ra);
          for (int col = c_lo; col < c_hi; ++col) {
            const uint8_t* s = fe->data + (col - fe->first) * 2 * cb;
            const int R = kPhysH - 1 - bp.x - col;
            put2_<BlitOp::Copy, BlitOp::Copy>(c, R, s, s + cb, bp.keep_lo, bp.keep_hi, bp.h, bp.y + kOffX);
            if (two)
              two_level_(c, R, bp.y + kOffX + bp.keep_lo, bp.y + kOffX + bp.keep_hi);
          }
        } else if (layout == PixelLayout::Rows) {
          const size_t rbytes = static_cast<size_t>((bp.w + 7) / 8);
          const int r_lo = std::max<int>({0, ra - bp.y, bp.keep_lo});
          const int r_hi = std::min<int>({bp.h, rb - bp.y, bp.keep_hi});
          for (int r = r_lo; r < r_hi; ++r) {
            const uint8_t* s = fe->data + (r - fe->first) * 2 * rbytes;
            put2_<BlitOp::Copy, BlitOp::Copy>(c, bp.y + r, s, s + rbytes, 0, bp.w, bp.w, bp.x + kOffX);
            if (two)
              two_level_(c, bp.y + r, bp.x + kOffX, bp.x + kOffX + bp.w);
          }
        } else {
          for (int r = ra; r < rb; ++r) {
            const uint8_t* s = fe->data + (r - fe->first) * kFrameRowBytes;
            std::memcpy(c.hi_row(r) + xa, s + xa, static_cast<size_t>(xb - xa));
            std::memcpy(c.lo_row(r) + xa, s + kStride + xa, static_cast<size_t>(xb - xa));
          }
        }
        break;
      }
      case kDiamond: {
        const DiamondP d = load<DiamondP>(pl);
        const int r_lo = std::max<int>(h->r0, ra), r_hi = std::min<int>(h->r1, rb);
        const int x1 = std::max<int>(h->b0, c.w0), x2 = std::min<int>(h->b1, c.w1);
        for (int r = r_lo; r < r_hi; ++r)
          for (int bit = x1; bit < x2; ++bit) {
            const int lx = portrait ? kPhysH - 1 - r : bit - kOffX;
            const int ly = portrait ? bit - kOffX : r;
            const int cov = diamond_cov_(lx, ly, d.cx, d.cy, d.r, d.t);
            if (cov < kCovGray)
              continue;
            const auto m = static_cast<uint8_t>(0x80u >> (bit & 7));
            c.hi_row(r)[bit >> 3] |= m;
            if (cov >= kCovBlack)
              c.lo_row(r)[bit >> 3] |= m;
          }
        break;
      }
      default:
        break;
    }
  }
}

void DrawBuffer::release_sources_() {
  // Each source used by the list just shown, once.
  PixelSource* seen[8];
  int n = 0;
  const Arena& C = cur();
  for (size_t off = 0; off < C.used; off += hdr_at(C.base, off)->size) {
    const OpHdr* h = hdr_at(C.base, off);
    if (h->kind != kBlit)
      continue;
    PixelSource* s = load<BlitP>(reinterpret_cast<const uint8_t*>(h) + sizeof(OpHdr)).src;
    bool dup = false;
    for (int k = 0; k < n; ++k)
      dup = dup || seen[k] == s;
    if (dup)
      continue;
    s->release();
    if (n < 8)
      seen[n++] = s;
  }
}

// The list just shown becomes the glass; the other one is free to record the
// next screen. A parked page moves along to the new list's tail.
void DrawBuffer::swap_lists_() {
  Arena& was = cur();
  Arena& next = prev();
  prev_valid_ = true;
  next.used = 0;
  next.overflowed = false;
  if (parked_) {
    std::memcpy(next.base + kArenaBytes - was.tail_len, was.base + kArenaBytes - was.tail_len, was.tail_len);
    next.tail_len = was.tail_len;
    next.tail_tag = was.tail_tag;
    next.parked_ops = was.parked_ops;
  } else {
    next.tail_len = 0;
    next.tail_tag = 0;
    next.parked_ops = 0;
  }
  cur_ ^= 1;
  cur_started_ = false;
}

void DrawBuffer::run_(UpdateKind kind, bool turn_off) {
  const bool full = kind == UpdateKind::Full;
  if (full) {
    for (int s = 0; s < kStrips; ++s) {
      iv_[s][0] = Interval{0, static_cast<uint8_t>(kStride)};
      iv_n_[s] = 1;
    }
    unknown_n_ = 0;
    all_unknown_ = false;
  } else {
    match_();
  }

  bool begun = false;
  bool drive = false;
  Fetch fetches[16];
  for (int s = 0; s < kStrips; ++s) {
    const int s0 = s * kStripRows, s1 = s0 + kStripRows;
    int dlo = kStride, dhi = 0;
    for (int r = s0; r < s1; ++r)
      if (dirty_lo_[r] < dirty_hi_[r]) {
        dlo = std::min<int>(dlo, dirty_lo_[r]);
        dhi = std::max<int>(dhi, dirty_hi_[r]);
      }
    if (iv_n_[s] == 0 && dlo >= dhi)
      continue;
    int ilo = kStride, ihi = 0;
    for (int k = 0; k < iv_n_[s]; ++k) {
      ilo = std::min<int>(ilo, iv_[s][k].lo);
      ihi = std::max<int>(ihi, iv_[s][k].hi);
    }
    const int wa = std::min(ilo, dlo), wb = std::max(ihi, dhi);
    const int wn = wb - wa;

    int rows = kStripRows;
    for (int ra = s0; ra < s1;) {
      int rb = std::min(s1, ra + rows);
      if (!begun) {
        display_.begin_update(kind);  // waits out the last waveform: the card is free from here
        begun = true;
      }
      int nf = 0;
      while (!fetch_chunk_(ra, rb, fetches, nf) && rb - ra > 1) {
        rows = std::max(1, rows / 2);
        rb = std::min(s1, ra + rows);
      }
      const int n = rb - ra;
      const int off = ra - s0;  // scratch row of ra

      display_.await_strip_slot();
      uint8_t* oldp = scratch_old_[send_slot_];
      uint8_t* nh = scratch_new_;
      uint8_t* nl = scratch_new_ + kPlaneBytes;
      // New: zero (HOLD for a turn) across the window, rendered in the damage.
      for (int r = 0; r < n; ++r) {
        std::memset(nh + (off + r) * kStride + wa, 0, static_cast<size_t>(wn));
        std::memset(nl + (off + r) * kStride + wa, 0, static_cast<size_t>(wn));
      }
      for (int k = 0; k < iv_n_[s]; ++k) {
        const int xa = iv_[s][k].lo, xb = iv_[s][k].hi;
        render_(cur(), false, ra, rb, xa, xb, scratch_new_, fetches, nf);
        if (full)
          continue;
        uint8_t* oh = oldp;
        uint8_t* ol = oldp + kPlaneBytes;
        for (int r = 0; r < n; ++r) {
          std::memset(oh + (off + r) * kStride + xa, 0, static_cast<size_t>(xb - xa));
          std::memset(ol + (off + r) * kStride + xa, 0, static_cast<size_t>(xb - xa));
        }
        render_(prev(), true, ra, rb, xa, xb, oldp, fetches, nf);
        // Slots, in place over the new planes: hi <- BW, lo <- RED.
        //   HOLD 00 where old == new; else to-white 01, to-gray 10, to-black 11
        //   (RED << 1 | BW), i.e. RED = changed & new_hi, BW = changed & (~new_hi | new_lo).
        //   Black that stays black inside the damage counts as changed: it is
        //   re-driven, as the stock X4 Pro waveform does (held black fades within
        //   minutes, so a glyph half re-driven reads uneven; panel_lut.h).
        for (int r = 0; r < n; ++r) {
          const int R = ra + r;
          uint8_t* ph = nh + (off + r) * kStride;
          uint8_t* pl = nl + (off + r) * kStride;
          const uint8_t* qh = oh + (off + r) * kStride;
          const uint8_t* ql = ol + (off + r) * kStride;
          int ulo = 0, uhi = 0;  // unknown bytes on this row: one span is enough
          if (all_unknown_) {
            ulo = xa;
            uhi = xb;
          } else {
            for (int u = 0; u < unknown_n_; ++u)
              if (R >= unknown_[u].r0 && R < unknown_[u].r1) {
                const int a0 = unknown_[u].b0 >> 3, a1 = (unknown_[u].b1 + 7) >> 3;
                if (uhi <= ulo) {
                  ulo = a0;
                  uhi = a1;
                } else {
                  ulo = std::min(ulo, a0);
                  uhi = std::max(uhi, a1);
                }
              }
          }
          for (int x = xa; x < xb; ++x) {
            const uint8_t h = ph[x], l = pl[x];
            uint8_t ch = static_cast<uint8_t>((h ^ qh[x]) | (l ^ ql[x]) | (l & ql[x]));
            if (x >= ulo && x < uhi)
              ch = 0xFF;
            pl[x] = static_cast<uint8_t>(ch & h);
            ph[x] = static_cast<uint8_t>(ch & (~h | l));
          }
        }
      }

      // Pack the window: BW plane then RED (4-aligned for the DMA).
      const size_t plane = static_cast<size_t>(n) * wn;
      uint8_t* bw = oldp;
      uint8_t* red = oldp + align4(plane);
      for (int r = 0; r < n; ++r) {
        const uint8_t* sh = nh + (off + r) * kStride + wa;
        const uint8_t* sl = nl + (off + r) * kStride + wa;
        // Full: the RAM bits are the level, RED = hi and BW = lo. Turn: the
        // slots computed above, BW in hi and RED in lo.
        std::memcpy(bw + r * wn, full ? sl : sh, static_cast<size_t>(wn));
        std::memcpy(red + r * wn, full ? sh : sl, static_cast<size_t>(wn));
        int lo = wn, hi = 0;
        for (int x = 0; x < wn; ++x)
          if (bw[r * wn + x] | red[r * wn + x]) {
            lo = std::min(lo, x);
            hi = x + 1;
          }
        if (lo < hi) {
          dirty_lo_[ra + r] = static_cast<uint8_t>(wa + lo);
          dirty_hi_[ra + r] = static_cast<uint8_t>(wa + hi);
          drive = true;
        } else {
          dirty_lo_[ra + r] = dirty_hi_[ra + r] = 0;
        }
      }
      display_.write_window(ra, n, wa, wn, bw, red);
      send_slot_ ^= 1;
      ra = rb;
    }
  }
  if (begun) {
    display_.finish_writes();
    if (drive || full)
      display_.fire(kind, turn_off);
  }
  release_sources_();
  swap_lists_();
}

void DrawBuffer::refresh() {
  if (!mem_ || !cur_started_)
    return;  // nothing drawn since the glass was last updated
  run_(UpdateKind::Turn, false);
}

void DrawBuffer::full_refresh(bool turn_off) {
  if (!mem_)
    return;
  ensure_started_();
  run_(UpdateKind::Full, turn_off || sunlight_fading_fix_);
}

void DrawBuffer::set_sleep_image(const uint8_t* data, size_t size) {
  sleep_img_ = data;
  sleep_img_size_ = size;
  if (sleep_src_) {
    sleep_src_->data = data;
    sleep_src_->size = size;
  }
}

void DrawBuffer::show_frame(PixelSource* src, uint32_t item) {
  set_rotation(Rotation::Deg90);
  fill(Level::White);
  blit(src, item, 0, 0, PixelLayout::Frame, 0, 0, kPanelBits, kPhysH);
  full_refresh(true);
}

bool DrawBuffer::show_sleep_image() {
  if (!sleep_img_ || sleep_img_size_ != DisplayFrame::kFrameBytes)
    return false;
  show_frame(sleep_src_, 0);
  return true;
}

// ---------------------------------------------------------------------------
// Host introspection
// ---------------------------------------------------------------------------

DrawBuffer::Stats DrawBuffer::current_stats(bool glass) const {
  Stats s;
  const Arena& a = arenas_[glass ? cur_ ^ 1 : cur_];
  for (size_t off = 0; off < a.used; off += hdr_at(a.base, off)->size)
    ++s.ops;
  s.ops_bytes = static_cast<uint32_t>(a.used);
  s.tail_bytes = static_cast<uint32_t>(a.tail_len);
  s.overflowed = a.overflowed;
  return s;
}

void DrawBuffer::render_current(uint8_t* frame) {
  if (!mem_)
    return;
  ensure_started_();
  Fetch fetches[16];
  const bool saved_all = all_unknown_;
  all_unknown_ = true;  // fetch everything the strip touches
  for (int s = 0; s < kStrips; ++s) {
    const int s0 = s * kStripRows, s1 = s0 + kStripRows;
    int rows = kStripRows;
    for (int ra = s0; ra < s1;) {
      int rb = std::min(s1, ra + rows);
      int nf = 0;
      while (!fetch_chunk_(ra, rb, fetches, nf) && rb - ra > 1) {
        rows = std::max(1, rows / 2);
        rb = std::min(s1, ra + rows);
      }
      std::memset(scratch_new_, 0, kStripBytes);
      render_(cur(), false, ra, rb, 0, kStride, scratch_new_, fetches, nf);
      for (int r = ra; r < rb; ++r) {
        std::memcpy(frame + static_cast<size_t>(r) * kFrameRowBytes, scratch_new_ + (r - s0) * kStride, kStride);
        std::memcpy(frame + static_cast<size_t>(r) * kFrameRowBytes + kStride,
                    scratch_new_ + kPlaneBytes + (r - s0) * kStride, kStride);
      }
      ra = rb;
    }
  }
  all_unknown_ = saved_all;
  release_sources_();
}

}  // namespace wintergreen
