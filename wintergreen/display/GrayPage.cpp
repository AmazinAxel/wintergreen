#include "GrayPage.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>

#include "DrawBuffer.h"

namespace wintergreen {

namespace {

constexpr int kStride = DisplayFrame::kStride;
constexpr int kPhysW = DisplayFrame::kPhysicalWidth;
constexpr int kPhysH = DisplayFrame::kPhysicalHeight;
constexpr int kOffX = DisplayFrame::kPanelOffsetX;
constexpr size_t kStripBytes = static_cast<size_t>(GrayPage::kStripRows) * kStride;
static_assert(GrayPage::kStrips * GrayPage::kStripRows == kPhysH, "strips tile the panel");

// OR source bits [lo, hi) of an MSB-first packed run into `row` at bit bit0 + i.
// `inv`: the source stores ink as 0 (1-bit images). Callers clip [lo, hi) so
// every landing bit is inside the row; a byte is only touched when bits land in
// it, so the bytes either side of a clipped edge are never read or written.
inline void or_bits_(uint8_t* row, const uint8_t* s, int lo, int hi, int bit0, bool inv) {
  for (int j = lo >> 3; j <= (hi - 1) >> 3; ++j) {
    const int a = std::max(lo, 8 * j) - 8 * j;
    const int b = std::min(hi, 8 * j + 8) - 8 * j;
    const auto mask = static_cast<uint8_t>((0xFFu >> a) & (0xFFu << (8 - b)));
    const auto v = static_cast<uint8_t>((inv ? ~s[j] : s[j]) & mask);
    if (!v)
      continue;
    const int start = bit0 + 8 * j;
    const int d = start >> 3;
    const int sh = start & 7;
    const auto h8 = static_cast<uint8_t>(v >> sh);
    if (h8)
      row[d] |= h8;
    if (sh) {
      const auto l8 = static_cast<uint8_t>(v << (8 - sh));
      if (l8)
        row[d + 1] |= l8;
    }
  }
}

// Set physical bits [x1, x2) of one strip row (x in app space): masked edge
// bytes, memset between.
inline void set_span_(uint8_t* row, int x1, int x2) {
  const int b1 = x1 + kOffX, b2 = x2 + kOffX;  // [b1, b2)
  const int j1 = b1 >> 3, j2 = (b2 - 1) >> 3;
  const auto lm = static_cast<uint8_t>(0xFFu >> (b1 & 7));
  const auto rm = static_cast<uint8_t>(0xFFu << (7 - ((b2 - 1) & 7)));
  if (j1 == j2) {
    row[j1] |= static_cast<uint8_t>(lm & rm);
    return;
  }
  row[j1] |= lm;
  std::memset(row + j1 + 1, 0xFF, static_cast<size_t>(j2 - j1 - 1));
  row[j2] |= rm;
}

struct Strip {
  int r0, r1;  // physical rows [r0, r1)
  uint8_t* hi;
  uint8_t* lo;
  uint8_t* unk;
  uint8_t* row(uint8_t* plane, int r) const {
    return plane + static_cast<size_t>(r - r0) * kStride;
  }
};

// Physical rectangle [x, x + w) x [y, y + h): ink into both planes, or into unk.
void rect_phys_(const Strip& s, int x, int y, int w, int h, bool to_unk) {
  const int x1 = std::max(x, 0), x2 = std::min(x + w, kPhysW);
  const int y1 = std::max(y, s.r0), y2 = std::min(y + h, s.r1);
  if (x1 >= x2)
    return;
  for (int r = y1; r < y2; ++r) {
    if (to_unk) {
      set_span_(s.row(s.unk, r), x1, x2);
    } else {
      set_span_(s.row(s.hi, r), x1, x2);
      set_span_(s.row(s.lo, r), x1, x2);
    }
  }
}

// Logical rectangle, as DrawBuffer::fill_rect maps it.
void rect_(const Strip& s, Rotation rot, int lx, int ly, int lw, int lh, bool to_unk) {
  if (rot == Rotation::Deg0)
    rect_phys_(s, lx, ly, lw, lh, to_unk);
  else
    rect_phys_(s, ly, kPhysH - lx - lw, lh, lw, to_unk);
}

// One two-plane glyph with its top-left at logical (gx, gy).
void glyph_(const Strip& s, Rotation rot, const GlyphData& g, int gx, int gy) {
  const int w = g.bitmap_width, h = g.bitmap_height;
  if (rot == Rotation::Deg0) {
    // Row r is physical row gy + r; pixel c lands at bit gx + c + kOffX.
    const int rb = (w + 7) / 8;
    const int r_lo = std::max(0, s.r0 - gy), r_hi = std::min(h, s.r1 - gy);
    const int c_lo = std::max(0, -gx), c_hi = std::min(w, kPhysW - gx);
    if (r_lo >= r_hi || c_lo >= c_hi)
      return;
    const uint8_t* lo_plane = g.bits + static_cast<size_t>(rb) * h;
    for (int r = r_lo; r < r_hi; ++r) {
      or_bits_(s.row(s.hi, gy + r), g.bits + static_cast<size_t>(r) * rb, c_lo, c_hi, gx + kOffX, false);
      or_bits_(s.row(s.lo, gy + r), lo_plane + static_cast<size_t>(r) * rb, c_lo, c_hi, gx + kOffX, false);
    }
    return;
  }
  // Portrait: logical column gx + c is physical row kPhysH - 1 - gx - c, its
  // pixels (logical y = gy + r) at bit gy + r + kOffX. The column copy holds
  // every hi column, then every lo column.
  if (!g.cols)
    return;
  const int cs = (h + 7) / 8;
  const int c_lo = std::max(0, kPhysH - gx - s.r1), c_hi = std::min(w, kPhysH - gx - s.r0);
  const int r_lo = std::max(0, -gy), r_hi = std::min(h, kPhysW - gy);
  if (c_lo >= c_hi || r_lo >= r_hi)
    return;
  const uint8_t* lo_cols = g.cols + static_cast<size_t>(cs) * w;
  for (int c = c_lo; c < c_hi; ++c) {
    const int pr = kPhysH - 1 - gx - c;
    or_bits_(s.row(s.hi, pr), g.cols + static_cast<size_t>(c) * cs, r_lo, r_hi, gy + kOffX, false);
    or_bits_(s.row(s.lo, pr), lo_cols + static_cast<size_t>(c) * cs, r_lo, r_hi, gy + kOffX, false);
  }
}

// A baked line (WgbFormat.h): ink rows skip whole lines in landscape, word ink
// columns skip words in portrait, pens are baked (no kerning here).
void line_(const Strip& s, Rotation rot, const WgbLine& line, const uint8_t* end, const BitmapFontSet& fonts) {
  if (rot == Rotation::Deg0 &&
      (line.baseline_y - line.ink_up >= s.r1 || line.baseline_y + line.ink_down <= s.r0))
    return;
  // Portrait: the strip is logical columns [x_min, x_max).
  const int x_min = kPhysH - s.r1, x_max = kPhysH - s.r0;
  WgbWordIter it(line, end);
  WgbWord w;
  while (it.next(w)) {
    if (w.ink_r == 0 || w.len == 0)
      continue;
    if (rot == Rotation::Deg90 && (w.x + w.ink_l >= x_max || w.x + w.ink_r <= x_min))
      continue;
    const BitmapFont* f = fonts.get_font(w.font);
    if (!f || !f->valid() || f->planes() != 2)
      continue;
    int baseline = line.baseline_y;
    if (w.valign == static_cast<uint8_t>(VerticalAlign::Super))
      baseline -= static_cast<int>(f->y_advance(100)) * 20 / 100;
    else if (w.valign == static_cast<uint8_t>(VerticalAlign::Sub))
      baseline += static_cast<int>(f->y_advance(100)) * 20 / 100;
    const BitmapFont::Styled sf = f->styled(static_cast<FontStyle>(w.style));
    const uint8_t* p = w.glyphs;
    const uint8_t* e = w.glyphs + w.len;
    int pen = w.x;
    for (bool first = true; p < e; first = false) {
      if (!first) {
        pen += static_cast<int8_t>(*p++);
        if (p >= e)
          break;
      }
      const GlyphData g = sf.glyph_data_at(wgb_read_glyph(p, e));
      if (g.bits)
        glyph_(s, rot, g, pen + g.x_offset, baseline + g.y_offset);
    }
  }
}

// The rows of an image this page shows: [first, last) of its full height.
void image_slice_(const WgbImage& img, int& first, int& last) {
  first = img.src_y;
  last = img.full_h;
  if (img.clip_h > 0 && first + img.clip_h < last)
    last = first + img.clip_h;
}

// This strip's part of a 1-bit image (bit clear = ink) into both planes, read
// from the card into `buf` (kStripBytes).
void image_(const Strip& s, Rotation rot, const WgbImage& img, GrayImageSource& src, uint8_t* buf) {
  int first, last;
  image_slice_(img, first, last);
  const int w = img.w;
  if (first >= last || w == 0)
    return;
  if (rot == Rotation::Deg0) {
    // Image row r is physical row img.y + r - first.
    const int rb = (w + 7) / 8;
    const int r_lo = std::max(first, s.r0 - img.y + first), r_hi = std::min(last, s.r1 - img.y + first);
    const int c_lo = std::max(0, -static_cast<int>(img.x)), c_hi = std::min(w, kPhysW - img.x);
    if (r_lo >= r_hi || c_lo >= c_hi || !src.read_image(img, rot, r_lo, r_hi, buf, kStripBytes))
      return;
    for (int r = r_lo; r < r_hi; ++r) {
      const uint8_t* row = buf + static_cast<size_t>(r - r_lo) * rb;
      or_bits_(s.row(s.hi, img.y + r - first), row, c_lo, c_hi, img.x + kOffX, true);
      or_bits_(s.row(s.lo, img.y + r - first), row, c_lo, c_hi, img.x + kOffX, true);
    }
    return;
  }
  // Portrait: column c is physical row kPhysH - 1 - img.x - c; its rows
  // [first, last) land at logical y = img.y + r - first.
  const int cs = (img.full_h + 7) / 8;
  const int c_lo = std::max(0, kPhysH - img.x - s.r1), c_hi = std::min(w, kPhysH - img.x - s.r0);
  const int r_lo = std::max(first, first - img.y), r_hi = std::min(last, kPhysW - img.y + first);
  if (c_lo >= c_hi || r_lo >= r_hi || !src.read_image(img, rot, c_lo, c_hi, buf, kStripBytes))
    return;
  const int bit0 = img.y - first + kOffX;
  for (int c = c_lo; c < c_hi; ++c) {
    const uint8_t* col = buf + static_cast<size_t>(c - c_lo) * cs;
    const int pr = kPhysH - 1 - img.x - c;
    or_bits_(s.row(s.hi, pr), col, r_lo, r_hi, bit0, true);
    or_bits_(s.row(s.lo, pr), col, r_lo, r_hi, bit0, true);
  }
}

uint8_t* alloc_(size_t n) {
  return static_cast<uint8_t*>(std::calloc(1, n));
}

}  // namespace

// Heap, claimed once at construction and never freed (as the framebuffers).
GrayPage::GrayPage()
    : kept_buf_(alloc_(kWgbMaxPageBytes)),
      new_hi_(alloc_(kStripBytes)),
      new_lo_(alloc_(kStripBytes)),
      img_(alloc_(kStripBytes)),
      send_{{alloc_(kStripBytes), alloc_(kStripBytes)}, {alloc_(kStripBytes), alloc_(kStripBytes)}},
      items_(static_cast<Item*>(std::calloc(kMaxItems, sizeof(Item)))) {
  kept_.list = kept_buf_;
  set_glass(Glass::Unknown);
}

GrayPage::~GrayPage() {
  std::free(items_);
  for (uint8_t* p : {kept_buf_, new_hi_, new_lo_, img_, send_[0][0], send_[0][1], send_[1][0], send_[1][1]})
    std::free(p);
}

void GrayPage::set(const uint8_t* list, size_t len, const BitmapFontSet* fonts, Rotation rot, int bar_h,
                   GrayImageSource* images) {
  next_ = Page{list, list ? std::min(len, kWgbMaxPageBytes) : 0, fonts, rot, bar_h};
  images_ = images;
  pending_ = true;
  indexed_ = nullptr;
}

void GrayPage::set_glass(Glass g) {
  glass_ = g;
  planes_ok_ = false;
  if (g != Glass::Page)
    std::fill(std::begin(dirty_), std::end(dirty_), true);
}

bool GrayPage::decode_(WgbPageView& v, Item& it) {
  if (!v.peek(it.tag))
    return false;
  if (it.tag == kWgbItemLine)
    return v.read_line(it.line);
  if (it.tag == kWgbItemRule)
    return v.read_rule(it.rule);
  return it.tag == kWgbItemImage && v.read_image(it.img);
}

// Decode `p` into items_ once, for every strip of this update. A page with more
// than kMaxItems items stays unindexed and is parsed per strip.
void GrayPage::index_(const Page& p) {
  indexed_ = nullptr;
  n_items_ = 0;
  if (!items_ || !p.fonts || p.len == 0)
    return;
  WgbPageView v(p.list, p.len);
  Item it;
  while (decode_(v, it)) {
    if (n_items_ == kMaxItems)
      return;
    items_[n_items_++] = it;
  }
  indexed_ = &p;
}

void GrayPage::render_(const Page& p, int r0, int rows, uint8_t* hi, uint8_t* lo, uint8_t* unk,
                       IDisplay* d) const {
  const size_t n = static_cast<size_t>(rows) * kStride;
  std::memset(hi, 0, n);
  std::memset(lo, 0, n);
  const Strip s{r0, r0 + rows, hi, lo, unk};
  if (p.fonts && p.len > 0) {
    const uint8_t* end = p.list + p.len;
    auto draw = [&](const Item& it) {
      if (it.tag == kWgbItemLine) {
        line_(s, p.rot, it.line, end, *p.fonts);
      } else if (it.tag == kWgbItemRule) {
        rect_(s, p.rot, it.rule.x, it.rule.y + it.rule.h / 2 - 1, it.rule.w, 2, false);
      } else if (d) {
        if (images_) {
          d->finish_writes();  // the card shares SPI2 with the queued windows
          image_(s, p.rot, it.img, *images_, img_);
        }
      } else if (unk) {
        int first, last;
        image_slice_(it.img, first, last);
        if (last > first)
          rect_(s, p.rot, it.img.x, it.img.y, it.img.w, last - first, true);
      }
    };
    if (indexed_ == &p) {
      for (int i = 0; i < n_items_; ++i)
        draw(items_[i]);
    } else {
      WgbPageView v(p.list, p.len);
      Item it;
      while (decode_(v, it))
        draw(it);
    }
  }
  // Charge reminder along the bottom edge, as the 1-bit page drew it.
  if (p.bar_h > 0) {
    const int W = p.rot == Rotation::Deg0 ? kPhysW : kPhysH;
    const int H = p.rot == Rotation::Deg0 ? kPhysH : kPhysW;
    rect_(s, p.rot, 0, H - p.bar_h, W, p.bar_h, false);
  }
}

void GrayPage::show(IDisplay& d, uint8_t* hi, uint8_t* lo) {
  if (!pending_)
    return;
  pending_ = false;
  if (!kept_buf_ || !new_hi_ || !new_lo_ || !img_ || !send_[0][0] || !send_[0][1] || !send_[1][0] ||
      !send_[1][1] || !hi)
    return;
  const Glass old = glass_;
  const bool from_planes = old == Glass::Page && planes_ok_ && lo;
  // The same page over itself: nothing to do.
  if (from_planes && next_.len == kept_.len && next_.fonts == kept_.fonts && next_.rot == kept_.rot &&
      next_.bar_h == kept_.bar_h && (next_.len == 0 || std::memcmp(next_.list, kept_.list, next_.len) == 0))
    return;

  d.gray_begin();  // waits the previous waveform out: the card is free below
  index_(next_);

  bool fired = false;
  int pair = 0;  // send pair for the next window; alternates per window sent, not per strip
  for (int k = 0; k < kStrips; ++k) {
    const int r0 = k * kStripRows;
    const size_t at = static_cast<size_t>(r0) * kStride;
    render_(next_, r0, kStripRows, new_hi_, new_lo_, nullptr, &d);

    // d0 = pixels whose level moved (every pixel over an unknown glass).
    uint8_t* bw = send_[pair][0];
    uint8_t* red = send_[pair][1];
    d.await_strip_slot();  // this pair's previous window has gone out
    uint8_t any = 0;
    for (size_t i = 0; i < kStripBytes; ++i) {
      uint8_t d0 = 0xFF;
      if (from_planes)
        d0 = static_cast<uint8_t>((hi[at + i] ^ new_hi_[i]) | (lo[at + i] ^ new_lo_[i]));
      else if (old == Glass::Frame)  // 1-bit frame, set = white: black is both planes
        d0 = static_cast<uint8_t>((~hi[at + i] ^ new_hi_[i]) | (~hi[at + i] ^ new_lo_[i]));
      red[i] = d0;
      any |= d0;
    }
    if (any) {
      // RED = changed & hi, BW = changed & (white | black); black re-driven
      // across a strip that changed.
      for (size_t i = 0; i < kStripBytes; ++i) {
        const uint8_t nh = new_hi_[i], nl = new_lo_[i];
        const auto changed = static_cast<uint8_t>(red[i] | nl);
        red[i] = static_cast<uint8_t>(changed & nh);
        bw[i] = static_cast<uint8_t>(changed & (~nh | nl));
      }
    } else if (dirty_[k]) {
      std::memset(red, 0, kStripBytes);  // back to HOLD
      std::memset(bw, 0, kStripBytes);
    }
    if (any || dirty_[k]) {
      d.write_window(r0, kStripRows, bw, red);
      pair ^= 1;
      fired = fired || any;
      dirty_[k] = any != 0;
    }
    // The glass's new levels, kept as the planes.
    std::memcpy(hi + at, new_hi_, kStripBytes);
    if (lo)
      std::memcpy(lo + at, new_lo_, kStripBytes);
  }
  if (fired)
    d.gray_fire();
  else
    d.finish_writes();

  // The glass now holds this page.
  if (next_.len > 0)
    std::memcpy(kept_buf_, next_.list, next_.len);
  kept_ = Page{kept_buf_, next_.len, next_.fonts, next_.rot, next_.bar_h};
  glass_ = Glass::Page;
  planes_ok_ = lo != nullptr;
  indexed_ = nullptr;
}

void GrayPage::handoff(IDisplay& d, const uint8_t* new_frame) {
  if (glass_ != Glass::Page || !new_frame || !kept_buf_ || !new_hi_ || !new_lo_ || !img_ || !send_[0][1] || !send_[1][1])
    return;
  uint8_t* unk = img_;  // no image reads here
  d.wait_idle();        // RAM is not written under a running waveform
  index_(kept_);
  // RED-only windows, queued like a turn's: strip k + 1 renders while strip k
  // goes out, alternating the send pairs' RED buffers.
  int pair = 0;
  for (int k = 0; k < kStrips; ++k) {
    const int r0 = k * kStripRows;
    std::memset(unk, 0, kStripBytes);
    render_(kept_, r0, kStripRows, new_hi_, new_lo_, unk, nullptr);
    uint8_t* red = send_[pair][1];
    d.await_strip_slot();
    // RED = the old frame as the OTP waveform reads it (set = white). Gray and
    // unknown pixels take the opposite of the new pixel, so they are driven.
    const uint8_t* f = new_frame + static_cast<size_t>(r0) * kStride;
    for (size_t i = 0; i < kStripBytes; ++i) {
      const auto white = static_cast<uint8_t>(~new_hi_[i] & ~unk[i]);
      const auto unsure = static_cast<uint8_t>((new_hi_[i] & ~new_lo_[i]) | unk[i]);
      red[i] = static_cast<uint8_t>(white | (unsure & ~f[i]));
    }
    d.write_window(r0, kStripRows, nullptr, red);
    pair ^= 1;
  }
  d.finish_writes();  // partial_refresh's polled writes follow
  indexed_ = nullptr;
}

}  // namespace wintergreen
