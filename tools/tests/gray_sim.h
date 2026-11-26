#pragma once

// Shared by baked_line_test and gray_page_test: random reader pages (layout
// PageContent + its display list), an independent per-pixel reference render of
// them in three levels, a fake image source, and a simulated controller that
// runs both the OTP and the Turn semantics on a glass of levels.

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "content/PageGeometry.h"
#include "content/wgb/WgbPageView.h"
#include "content/wgb/WgbPaginator.h"
#include "display/DrawBuffer.h"

namespace sim {

using namespace wintergreen;

constexpr int kCols = DisplayFrame::kPanelWidth;  // 800 bits per physical row
constexpr int kRows = DisplayFrame::kPhysicalHeight;
constexpr int kStride = DisplayFrame::kStride;
constexpr uint8_t kWhite = 0, kGray = 1, kBlack = 2;

using Levels = std::vector<uint8_t>;  // kRows x kCols, physical

inline Levels blank() {
  return Levels(static_cast<size_t>(kRows) * kCols, kWhite);
}

inline bool bit(const uint8_t* plane, int r, int b) {
  return (plane[static_cast<size_t>(r) * kStride + (b >> 3)] >> (7 - (b & 7))) & 1;
}

// 1-bit frame (set = white) to levels.
inline Levels frame_levels(const uint8_t* f) {
  Levels L = blank();
  for (int r = 0; r < kRows; ++r)
    for (int b = 0; b < kCols; ++b)
      L[static_cast<size_t>(r) * kCols + b] = bit(f, r, b) ? kWhite : kBlack;
  return L;
}

// Darken logical pixel (lx, ly) to `lv`, as DrawBuffer maps rotations.
inline void put(Levels& L, Rotation rot, int lx, int ly, uint8_t lv) {
  int r, b;
  if (rot == Rotation::Deg0) {
    if (lx < 0 || lx >= DisplayFrame::kPhysicalWidth || ly < 0 || ly >= kRows)
      return;
    r = ly;
    b = lx + DisplayFrame::kPanelOffsetX;
  } else {
    if (lx < 0 || lx >= kRows || ly < 0 || ly >= DisplayFrame::kPhysicalWidth)
      return;
    r = kRows - 1 - lx;
    b = ly + DisplayFrame::kPanelOffsetX;
  }
  uint8_t& p = L[static_cast<size_t>(r) * kCols + b];
  if (lv > p)
    p = lv;
}

inline void rect(Levels& L, Rotation rot, int x, int y, int w, int h, uint8_t lv) {
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i)
      put(L, rot, x + i, y + j, lv);
}

// Image pixels: a fixed function of (key, x, y) in the full image.
inline bool image_ink(int key, int x, int y) {
  return ((x * 7 + y * 13 + key * 5) % 11) < 4 || (x / 9 + y / 7 + key) % 5 == 0;
}

// The stored copies as WgbWriter lays them out: rows (landscape) and columns
// (portrait), bit clear = ink, padding white.
struct FakeImages : GrayImageSource {
  int reads = 0;
  bool read_image(const WgbImage& img, Rotation rot, int lo, int hi, uint8_t* dst, size_t cap) override {
    ++reads;
    const int w = img.w, h = img.full_h;
    const int unit = rot == Rotation::Deg90 ? (h + 7) / 8 : (w + 7) / 8;
    const size_t n = static_cast<size_t>(hi - lo) * unit;
    assert(lo >= 0 && hi > lo && hi <= (rot == Rotation::Deg90 ? w : h));
    if (n > cap)
      return false;
    std::memset(dst, 0xFF, n);
    for (int k = lo; k < hi; ++k)
      for (int j = 0; j < (rot == Rotation::Deg90 ? h : w); ++j) {
        const bool ink = rot == Rotation::Deg90 ? image_ink(img.key, k, j) : image_ink(img.key, j, k);
        if (ink)
          dst[static_cast<size_t>(k - lo) * unit + j / 8] &= static_cast<uint8_t>(~(0x80u >> (j & 7)));
      }
    return true;
  }
};

struct TestPage {
  Rotation rot = Rotation::Deg90;
  int bar_h = 0;
  std::vector<std::vector<LayoutWord>> words;
  PageContent pc;
  std::vector<uint8_t> dl;
};

// The three-level page as the reader should show it: two-plane glyphs drawn
// from their rows (the GrayPage portrait path uses the column copy), UTF-8
// decoded and kerned the way BitmapFont::word_width measures.
inline Levels reference(const TestPage& p, const BitmapFontSet& fonts) {
  Levels L = blank();
  for (const auto& ci : p.pc.items) {
    const PageTextItem* item = std::get_if<PageTextItem>(&ci);
    if (!item)
      continue;
    const int baseline_y = static_cast<int>(item->y_offset) + item->baseline;
    for (const LayoutWord& w : item->line.words) {
      if (w.len == 0)
        continue;
      const BitmapFont* f = fonts.get(w.size_pct);
      int base = baseline_y;
      if (w.vertical_align == VerticalAlign::Super)
        base -= static_cast<int>(fonts.y_advance(w.size_pct)) * 20 / 100;
      else if (w.vertical_align == VerticalAlign::Sub)
        base += static_cast<int>(fonts.y_advance(w.size_pct)) * 20 / 100;
      const BitmapFont::Styled sf = f->styled(w.style);
      int cursor_q = (kReaderHPadding + w.x) * 4;
      int prev = -1;
      for (const char *t = w.text, *e = w.text + w.len; t < e;) {
        const int idx = sf.glyph_index(BitmapFont::decode_utf8(t, e));
        cursor_q += sf.kerning_q(prev, idx);
        const GlyphData g = sf.glyph_data_at(idx);
        if (g.bits) {
          const int gx = (cursor_q + 2) / 4 + g.x_offset, gy = base + g.y_offset;
          const int rb = (g.bitmap_width + 7) / 8;
          for (int r = 0; r < g.bitmap_height; ++r)
            for (int c = 0; c < g.bitmap_width; ++c) {
              const size_t o = static_cast<size_t>(r) * rb + c / 8;
              const auto m = static_cast<uint8_t>(0x80u >> (c & 7));
              const bool lo = g.bits[static_cast<size_t>(rb) * g.bitmap_height + o] & m;
              const bool hi = g.bits[o] & m;
              if (hi || lo)
                put(L, p.rot, gx + c, gy + r, lo ? kBlack : kGray);
            }
        }
        cursor_q += g.advance_width;
        cursor_q = ((cursor_q + 2) / 4) * 4;
        prev = idx;
      }
    }
  }
  for (const auto& ci : p.pc.items) {
    if (const PageHrItem* h = std::get_if<PageHrItem>(&ci))
      rect(L, p.rot, h->x_offset, h->y_offset + h->height / 2 - 1, h->width, 2, kBlack);
    if (const PageImageItem* im = std::get_if<PageImageItem>(&ci)) {
      const int full = im->full_height > 0 ? im->full_height : im->height;
      for (int r = im->y_crop; r < im->y_crop + im->height && r < full; ++r)
        for (int c = 0; c < im->width; ++c)
          if (image_ink(im->key, c, r))
            put(L, p.rot, im->x_offset + c, im->y_offset + r - im->y_crop, kBlack);
    }
  }
  if (p.bar_h > 0) {
    const int W = p.rot == Rotation::Deg0 ? DisplayFrame::kPhysicalWidth : kRows;
    const int H = p.rot == Rotation::Deg0 ? kRows : DisplayFrame::kPhysicalWidth;
    rect(L, p.rot, 0, H - p.bar_h, W, p.bar_h, kBlack);
  }
  return L;
}

// A random page: words in every font, style and vertical alignment (long
// words, absolute-x escapes, empty words), rules, optionally images.
// `many`: also 70 rules, past GrayPage::kMaxItems (the per-strip parse path).
inline std::unique_ptr<TestPage> random_page(const BitmapFontSet& fset, Rotation rot, bool images, bool many = false) {
  static const char* kWords[] = {"The",   "quick", "\xE2\x80\x9C" "brown\xE2\x80\x9D", "fox",  "-", "AV",
                                 "Wa.",   "jumps", "caf\xC3\xA9",                       "over", "x", "lazy,"};
  static const std::string long_word(150, 'm');  // past the 127-byte length escape
  static const uint8_t sizes[] = {100, 80, 120, 60, 150};
  auto p = std::make_unique<TestPage>();
  p->rot = rot;
  p->bar_h = std::rand() % 5 == 0 ? kLowBatteryBarH : 0;
  const int lines = 1 + std::rand() % 20;
  p->words.resize(static_cast<size_t>(lines));
  for (int l = 0; l < lines; ++l) {
    int x = std::rand() % 8 == 0 ? 300 : std::rand() % 20;
    const bool plain = std::rand() % 2;
    const int n = std::rand() % 12;
    for (int k = 0; k < n; ++k) {
      const bool lw = std::rand() % 50 == 0;
      const char* t = lw ? long_word.c_str() : kWords[std::rand() % (sizeof(kWords) / sizeof(*kWords))];
      p->words[l].push_back(LayoutWord{t, static_cast<uint16_t>(lw ? long_word.size() : std::strlen(t)),
                                       static_cast<uint16_t>(x),
                                       plain ? FontStyle::Regular : static_cast<FontStyle>(std::rand() % 4),
                                       plain ? uint8_t{100} : sizes[std::rand() % 5],
                                       plain ? VerticalAlign::Baseline : static_cast<VerticalAlign>(std::rand() % 3),
                                       false});
      x += std::rand() % 10 == 0 ? 254 + std::rand() % 200 : 10 + std::rand() % 60;
    }
    if (std::rand() % 8 == 0)
      p->words[l].push_back(LayoutWord{"", 0, 5, FontStyle::Regular, 100, VerticalAlign::Baseline, false});
  }
  for (int l = 0; l < lines; ++l) {
    PageTextItem ti{};
    ti.line.words.data_ = p->words[l].data();
    ti.line.words.size_ = static_cast<uint16_t>(p->words[l].size());
    ti.y_offset = static_cast<uint16_t>(4 + l * 36);
    ti.height = 36;
    ti.baseline = 28;
    p->pc.items.emplace_back(ti);
    if (std::rand() % 6 == 0)
      p->pc.items.emplace_back(PageHrItem{static_cast<uint16_t>(std::rand() % 100), static_cast<uint16_t>(l * 36),
                                          static_cast<uint16_t>(50 + std::rand() % 300), 36});
  }
  for (int i = 0; many && i < 70; ++i)
    p->pc.items.emplace_back(PageHrItem{static_cast<uint16_t>(std::rand() % 400), static_cast<uint16_t>(i * 11),
                                        static_cast<uint16_t>(20 + std::rand() % 200), 8});
  const int n_img = images ? std::rand() % 3 : 0;
  for (int i = 0; i < n_img; ++i) {
    PageImageItem im{};
    im.key = static_cast<uint16_t>(i);
    im.width = static_cast<uint16_t>(1 + std::rand() % 300);
    im.full_height = static_cast<uint16_t>(1 + std::rand() % 300);
    im.y_crop = static_cast<uint16_t>(std::rand() % im.full_height);
    im.height = static_cast<uint16_t>(1 + std::rand() % (im.full_height - im.y_crop));
    im.x_offset = static_cast<uint16_t>(std::rand() % 500);
    im.y_offset = static_cast<uint16_t>(std::rand() % 600);
    p->pc.items.emplace_back(im);
  }
  std::string err;
  if (!serialize_wgb_page(p->pc, fset, 8, p->dl, err)) {
    std::fprintf(stderr, "serialize: %s\n", err.c_str());
    std::abort();
  }
  return p;
}

// The controller: two RAM planes and a glass of levels.
//   OTP fast (partial_refresh): BW replaces RAM; pixels with BW != RED go to
//     BW's level, the rest are left alone; RED then = BW.
//   OTP full/half: both planes = the frame, glass = the frame.
//   Turn (gray_fire): state = RED << 1 | BW per pixel: HOLD, white, gray, black.
struct Panel : IDisplay {
  std::vector<uint8_t> bw = std::vector<uint8_t>(DisplayFrame::kPixelBytes, 0xFF);
  std::vector<uint8_t> red = std::vector<uint8_t>(DisplayFrame::kPixelBytes, 0xFF);
  Levels glass = blank();
  int turns = 0, otp = 0, windows = 0;
  long redriven = 0;  // white or gray pixels driven to the level they showed

  // Queued windows are applied late, as DMA would send them: a buffer the
  // caller refills too early shows up as wrong pixels.
  struct Window {
    int row0, rows;
    const uint8_t* bw;
    const uint8_t* red;
  };
  std::vector<Window> queue;
  void apply_oldest() {
    const Window w = queue.front();
    queue.erase(queue.begin());
    const size_t at = static_cast<size_t>(w.row0) * kStride, n = static_cast<size_t>(w.rows) * kStride;
    if (w.bw)
      std::memcpy(bw.data() + at, w.bw, n);
    std::memcpy(red.data() + at, w.red, n);
  }
  void await_strip_slot() override {
    while (queue.size() > 1)
      apply_oldest();
  }
  void finish_writes() override {
    while (!queue.empty())
      apply_oldest();
  }
  void write_window(int row0, int rows, const uint8_t* b, const uint8_t* rd) override {
    await_strip_slot();
    queue.push_back(Window{row0, rows, b, rd});
    ++windows;
  }

  void full_refresh(const uint8_t* px, const uint8_t*, RefreshMode, bool) override {
    finish_writes();
    std::memcpy(bw.data(), px, bw.size());
    std::memcpy(red.data(), px, red.size());
    glass = frame_levels(px);
    ++otp;
  }
  void partial_refresh(const uint8_t* px) override {
    finish_writes();
    std::memcpy(bw.data(), px, bw.size());
    for (int r = 0; r < kRows; ++r)
      for (int b = 0; b < kCols; ++b)
        if (bit(bw.data(), r, b) != bit(red.data(), r, b))
          glass[static_cast<size_t>(r) * kCols + b] = bit(bw.data(), r, b) ? kWhite : kBlack;
    red = bw;
    ++otp;
  }
  void gray_fire() override {
    finish_writes();
    for (int r = 0; r < kRows; ++r)
      for (int b = 0; b < kCols; ++b) {
        const int s = bit(red.data(), r, b) << 1 | bit(bw.data(), r, b);
        uint8_t& g = glass[static_cast<size_t>(r) * kCols + b];
        if (s == 0)
          continue;
        const uint8_t to = s == 1 ? kWhite : s == 2 ? kGray : kBlack;
        if (to != kBlack && to == g)
          ++redriven;
        g = to;
      }
    ++turns;
  }
};

}  // namespace sim
