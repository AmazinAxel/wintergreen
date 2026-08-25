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

  // Return the font set (use with app.set_reader_font()).
  BitmapFontSet* font_set() {
    return font_set_.valid() ? &font_set_ : nullptr;
  }

  bool valid() const {
    return font_set_.valid();
  }
 protected:
  BitmapFont prop_fonts_[kMaxFontSizes];
  BitmapFontSet font_set_;
  int num_fonts_ = 0;
};

}  // namespace wintergreen
