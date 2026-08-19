#pragma once

#include <string>

namespace wintergreen {

// Where a book's cover bitmaps live.
//
// For a converted book (`<book>/book.mrb`) they sit beside it, written by
// tools/epub2mrb — the device cannot generate them, since there is no EPUB on
// the card. Deriving a path from the file stem would be wrong as well as
// useless: every converted book is literally named book.mrb.
//
// The `<data_dir>/cache/<stem>/` form is kept only for the EPUB conversion
// path, which no longer exists on-device but still runs in the host converter.
std::string cover_bin_path(const char* book_path, const char* data_dir);
std::string cover_sleep_bin_path(const char* book_path, const char* data_dir);

}  // namespace wintergreen
