#pragma once

#include <cstring>

#include "font_partition.h"
#include "wintergreen/Application.h"
#include "wintergreen/FontManager.h"

// ESP32 font manager. The reader font lives in the `font` partition, written by
// esptool at flash time and mmapped here — there is no provisioning step, no
// progress screen and no runtime decompression. See font_partition.h.
//
// Declare as `static FontManager font_mgr(app)` in app_main to keep the object
// in BSS rather than on the stack
class FontManager : public wintergreen::FontManager {
 public:
  explicit FontManager(wintergreen::Application& app) : app_(app) {}

  void init() {
    if (!font_part_.map("font"))
      return;
    load_fonts_();
    if (font_set_.valid())
      app_.set_reader_font(font_set());
  }

 private:
  // WGFS bundle: [WGFS:4][u8 count][pad:3][count × uint32 size][WGF1 blobs...]
  // The magic is the version — there is no version byte to check.
  void load_fonts_() {
    const uint8_t* d = font_part_.data;
    const size_t sz = font_part_.size;
    if (sz < 8 || memcmp(d, "WGFS", 4) != 0)
      return;

    uint8_t num = d[4];
    if (num > wintergreen::kMaxFontSizes)
      num = wintergreen::kMaxFontSizes;

    constexpr size_t kSizeTableOff = 8;
    uint32_t sizes[wintergreen::kMaxFontSizes] = {};
    for (int i = 0; i < num; i++) {
      const uint8_t* p = d + kSizeTableOff + i * 4;
      sizes[i] = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    }
    size_t off = kSizeTableOff + static_cast<size_t>(num) * 4;
    for (int i = 0; i < num; i++) {
      if (off + sizes[i] > sz)
        break;
      load_font(d + off, sizes[i]);
      off += sizes[i];
    }
  }

  wintergreen::Application& app_;
  RawPartition font_part_;
};
