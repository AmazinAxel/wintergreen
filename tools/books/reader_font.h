#pragma once

// The device's reader font bundle, for baking pages. resources/Libron.bin is
// compiled into the binary (WG_READER_FONT, set by CMake), so a `books` built
// from a commit bakes with exactly the font that commit flashes.

#include "wintergreen/content/BitmapFont.h"

namespace books {

// Null if the embedded bundle does not inflate or parse.
const wintergreen::BitmapFontSet* reader_fonts();

}  // namespace books
