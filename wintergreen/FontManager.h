#pragma once

#include "content/BitmapFont.h"

namespace wintergreen {

class FontManager {
 public:
  // Init one font slot. The data pointer must outlive this object.
  void load_font(const uint8_t* data, size_t sz) {
    if (num_fonts_ >= kMaxFontSizes)
      return;
    int idx = num_fonts_++;
    prop_fonts_[idx].init(data, sz);
    font_set_.add(&prop_fonts_[idx]);
  }

  // Load every font of a WGFS bundle: [u8 count][pad:3][count × u32 size][WGF
  // blobs...]. No magic: the bundle comes from resources/ in the same build.
  // The data must outlive this object. Shared by the device (mmapped font
  // partition) and `books` (which bakes pages with the very same fonts).
  void load_bundle(const uint8_t* d, size_t sz) {
    if (sz < 4)
      return;
    uint8_t num = d[0];
    if (num > kMaxFontSizes)
      num = kMaxFontSizes;
    constexpr size_t kSizeTableOff = 4;
    if (kSizeTableOff + static_cast<size_t>(num) * 4 > sz)
      return;
    uint32_t sizes[kMaxFontSizes] = {};
    for (int i = 0; i < num; i++) {
      const uint8_t* p = d + kSizeTableOff + i * 4;
      sizes[i] = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }
    size_t off = kSizeTableOff + static_cast<size_t>(num) * 4;
    for (int i = 0; i < num; i++) {
      if (off + sizes[i] > sz)
        break;
      load_font(d + off, sizes[i]);
      off += sizes[i];
    }
  }

  // Return the font set (use with app.set_reader_font()).
  BitmapFontSet* font_set() {
    return font_set_.valid() ? &font_set_ : nullptr;
  }

 protected:
  BitmapFont prop_fonts_[kMaxFontSizes];
  BitmapFontSet font_set_;
  int num_fonts_ = 0;
};

}  // namespace wintergreen
