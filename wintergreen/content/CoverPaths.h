#pragma once

#include <string>

namespace wintergreen {

// Where a book's cover bitmaps live.
//
// For a converted book (`<book>/book.wgb`) they sit beside it, written by
// tools/epub2wgb — the device cannot generate them, since there is no EPUB on
// the card. Deriving a path from the file stem would be wrong as well as
// useless: every converted book is literally named book.wgb.
//
// The `<data_dir>/cache/<stem>/` form is kept only for the EPUB conversion
// path, which no longer exists on-device but still runs in the host converter.
std::string cover_bin_path(const char* book_path, const char* data_dir);
std::string cover_sleep_bin_path(const char* book_path, const char* data_dir);
std::string cover_home_bin_path(const char* book_path, const char* data_dir);

// Reading position, written beside the book as <book dir>/book.pos.
//
// It used to live in the state directory under an 8-hex-digit FNV hash of the
// book title, author and language, which meant a folder of opaque filenames,
// a hash collision away from two books sharing a bookmark, and a position that
// silently reset if a publisher edited the metadata. Beside the book it is
// obvious, deleted with the book, and needs no key at all.
std::string book_pos_path(const char* book_path, const char* data_dir);

// The home carousel's cover box, in panel pixels. The converter renders
// cover_home.bin to fit exactly this, so HomeScreen can blit it 1:1 instead of
// box-filtering and re-dithering an already-dithered cover — a second dither
// pass at a near-1:1 ratio is what made the home cover look coarse.
//
// Derived from HomeScreen::draw_all_'s layout. Note the panel's app-space
// height is **793**, not 800: DisplayFrame::kPhysicalWidth excludes the 7
// hidden columns at the leading edge of the glass. Getting that wrong is what
// made these too tall, which quietly put every home cover through the
// downscale path it exists to avoid.
//
//   battery_row_h_ = kBatteryPadY(10) + ui_large.y_advance(31)          =  41
//   box_y          = battery_row_h_ + kHeroGap(24)                      =  65
//   below_h        = 26 + 2*ui_header.y_advance(41) + 6 + 31 + 26 + 19 + 28
//                                                                       = 218
//   width  = 480 - 2*kPad(16) - 2*(kFrameGap(10) + kFrameW(2))          = 424
//   height = 793 - box_y - below_h - 2*(kFrameGap + kFrameW)            = 486
//
// If the layout moves these drift, the fallback path takes over and quality
// quietly regresses until the library is re-converted. Nothing breaks.
inline constexpr int kHomeCoverW = 424;
inline constexpr int kHomeCoverH = 486;

// The sleep screen, in panel pixels (portrait) — the whole visible panel.
// The converter composes cover_sleep.bin at exactly this size, artwork scaled
// to fit, centred, and the letterbox bars already filled, so
// Application::do_sleep_ blits it 1:1. A file of any other size is ignored and
// the wordmark shown instead; that is what an unconverted library looks like.
// Application.cpp static_asserts these against the panel — the first version of
// this said 800 and every cover silently failed the size check.
inline constexpr int kSleepCoverW = 480;
inline constexpr int kSleepCoverH = 793;

}  // namespace wintergreen
