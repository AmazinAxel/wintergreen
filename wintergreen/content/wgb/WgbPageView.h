#pragma once

// Reads one page's display list (see WgbFormat.h) in place. No allocation: the
// view points into the caller's buffer, and every item is decoded as it is
// visited. Shared by the device's draw path, the bake's self-check and the
// host tests.

#include <cstddef>
#include <cstdint>

#include "WgbFormat.h"

namespace wintergreen {

struct WgbWord {
  uint16_t x;
  uint8_t style;   // FontStyle
  uint8_t font;    // index into the font bundle
  uint8_t valign;  // VerticalAlign
  const uint8_t* glyphs;  // glyph indices, wgb_read_glyph encoding
  uint16_t len;           // bytes at glyphs
};

struct WgbLine {
  uint16_t baseline_y = 0;
  uint8_t word_count = 0;
  uint8_t flags = 0;               // word flags in force at the first word
  const uint8_t* words = nullptr;  // first word header
};

struct WgbRule {
  uint16_t x, y, w, h;
};

struct WgbImage {
  uint16_t key, x, y, w, full_h, src_y, clip_h;
};

// Walks a line's words. next() is false once word_count words were read, or
// on a word that runs past `end`.
class WgbWordIter {
 public:
  WgbWordIter(const WgbLine& line, const uint8_t* end)
      : p_(line.words), end_(end), left_(line.word_count), flags_(line.flags) {}

  bool next(WgbWord& w) {
    if (left_ == 0 || !p_ || end_ - p_ < 2)
      return false;
    if (*p_ == kWgbAbsX) {
      if (end_ - p_ < 4)
        return false;
      x_ = wgb_read_u16(p_ + 1);
      p_ += 3;
    } else {
      x_ = static_cast<uint16_t>(x_ + *p_++);
    }
    const uint8_t lenb = *p_++;
    uint16_t len = lenb & kWgbLongWord;
    if (len == kWgbLongWord) {
      if (end_ - p_ < 2)
        return false;
      len = wgb_read_u16(p_);
      p_ += 2;
    }
    if (lenb & kWgbFlagsFollow) {
      if (end_ - p_ < 1)
        return false;
      flags_ = *p_++;
    }
    w.x = x_;
    w.style = flags_ & 0x03;
    w.font = (flags_ >> 2) & 0x07;
    w.valign = (flags_ >> 5) & 0x03;
    if (end_ - p_ < len)
      return false;
    w.glyphs = p_;
    w.len = len;
    p_ += len;
    --left_;
    return true;
  }
  // Just past the last word read.
  const uint8_t* pos() const {
    return p_;
  }
  // Flags in force after the last word read.
  uint8_t flags() const {
    return flags_;
  }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
  uint8_t left_;
  uint8_t flags_;
  uint16_t x_ = 0;
};

// Walks a page's items in draw order. Each next_*() reports the tag of the
// item at the cursor via tag(); the caller decodes it with the matching read_*()
// and the cursor moves past it. ok() goes false on any malformed byte, and
// stays false.
class WgbPageView {
 public:
  WgbPageView(const uint8_t* data, size_t size) : p_(data), end_(data + size) {
    if (size < 2) {
      ok_ = false;
      return;
    }
    left_ = wgb_read_u16(p_);
    p_ += 2;
  }

  bool ok() const {
    return ok_;
  }
  // True after every item was read and nothing is left over: a well-formed list.
  bool at_end() const {
    return ok_ && left_ == 0 && p_ == end_;
  }
  // Tag of the next item, or false when there are none left.
  bool peek(uint8_t& tag) const {
    if (!ok_ || left_ == 0 || p_ >= end_)
      return false;
    tag = *p_;
    return true;
  }

  bool read_line(WgbLine& out) {
    if (!take_(kWgbItemLine, 1 + 3))
      return false;
    out.baseline_y = wgb_read_u16(p_ + 1);
    out.word_count = p_[3];
    out.flags = flags_;
    out.words = p_ + 4;
    WgbWordIter it(out, end_);
    WgbWord w;
    for (uint8_t i = 0; i < out.word_count; ++i) {
      if (!it.next(w)) {
        ok_ = false;
        return false;
      }
    }
    flags_ = it.flags();
    p_ = it.pos();
    --left_;
    return true;
  }
  const uint8_t* end() const {
    return end_;
  }

  bool read_rule(WgbRule& out) {
    if (!take_(kWgbItemRule, kWgbRuleItemSize))
      return false;
    out = WgbRule{wgb_read_u16(p_ + 1), wgb_read_u16(p_ + 3), wgb_read_u16(p_ + 5), wgb_read_u16(p_ + 7)};
    p_ += kWgbRuleItemSize;
    --left_;
    return true;
  }

  bool read_image(WgbImage& out) {
    if (!take_(kWgbItemImage, kWgbImageItemSize))
      return false;
    out = WgbImage{wgb_read_u16(p_ + 1), wgb_read_u16(p_ + 3), wgb_read_u16(p_ + 5), wgb_read_u16(p_ + 7),
                   wgb_read_u16(p_ + 9), wgb_read_u16(p_ + 11), wgb_read_u16(p_ + 13)};
    p_ += kWgbImageItemSize;
    --left_;
    return true;
  }

  // Skip the item at the cursor, whatever it is.
  bool skip() {
    uint8_t tag;
    if (!peek(tag))
      return false;
    WgbLine l;
    WgbRule r;
    WgbImage i;
    switch (tag) {
      case kWgbItemLine:
        return read_line(l);
      case kWgbItemRule:
        return read_rule(r);
      case kWgbItemImage:
        return read_image(i);
      default:
        ok_ = false;
        return false;
    }
  }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
  uint16_t left_ = 0;
  uint8_t flags_ = 0;  // word flags carried from line to line
  bool ok_ = true;

  bool take_(uint8_t tag, size_t need) {
    if (!ok_ || left_ == 0 || static_cast<size_t>(end_ - p_) < need || *p_ != tag) {
      ok_ = false;
      return false;
    }
    return true;
  }
};

}  // namespace wintergreen
