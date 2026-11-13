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
    load_bundle(font_part_.data, font_part_.size);
    if (font_set_.valid())
      app_.set_reader_font(font_set());
  }

 private:
  wintergreen::Application& app_;
  RawPartition font_part_;
};
