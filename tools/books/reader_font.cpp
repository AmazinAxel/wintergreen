#include "reader_font.h"

#include <cstdint>
#include <vector>

#include "miniz.h"
#include "wintergreen/FontManager.h"

#ifndef WG_READER_FONT
#error "WG_READER_FONT must name resources/Libron.bin"
#endif

// resources/Libron.bin, verbatim: [uint32 uncompressed size][zlib stream].
asm(".section .rodata\n"
    ".global wg_reader_font_blob\n"
    ".global wg_reader_font_blob_end\n"
    ".balign 16\n"
    "wg_reader_font_blob:\n"
    ".incbin \"" WG_READER_FONT "\"\n"
    "wg_reader_font_blob_end:\n"
    ".previous\n");
extern "C" const uint8_t wg_reader_font_blob[];
extern "C" const uint8_t wg_reader_font_blob_end[];

namespace books {

const wintergreen::BitmapFontSet* reader_fonts() {
  // Inflated once and kept for the process: the fonts point into it.
  static std::vector<uint8_t> data;
  static wintergreen::FontManager mgr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    const size_t n = static_cast<size_t>(wg_reader_font_blob_end - wg_reader_font_blob);
    if (n > 4) {
      const uint32_t raw = wg_reader_font_blob[0] | (wg_reader_font_blob[1] << 8) | (wg_reader_font_blob[2] << 16) |
                           (static_cast<uint32_t>(wg_reader_font_blob[3]) << 24);
      data.resize(raw);
      mz_ulong out_len = raw;
      if (mz_uncompress(data.data(), &out_len, wg_reader_font_blob + 4, static_cast<mz_ulong>(n - 4)) == MZ_OK &&
          out_len == raw)
        mgr.load_bundle(data.data(), data.size());
    }
  }
  return mgr.font_set();
}

}  // namespace books
