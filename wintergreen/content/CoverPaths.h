#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace wintergreen {

// Where a book's cover bitmaps live: beside the book, written by `books`
// — the device cannot generate them, since there is no EPUB on the card.
// Deriving a path from the file stem would be wrong as well as useless: every
// converted book is literally named book.wgb.
//
// These took a `data_dir` for a second `<data_dir>/cache/<stem>/` form used by
// on-device EPUB conversion. That path went with EPUB support: BookIndex only
// ever indexes .wgb, so the branch selecting it was unreachable. The host
// converter never called these at all — it string-replaces "book.wgb" in its
// own output path (WgbConverter.cpp).
std::string cover_sleep_bin_path(const char* book_path);
std::string cover_home_bin_path(const char* book_path);
std::string cover_list_bin_path(const char* book_path);

// Reading position, written beside the book as <book dir>/book.pos.
//
// It used to live in the state directory under an 8-hex-digit FNV hash of the
// book title, author and language, which meant a folder of opaque filenames,
// a hash collision away from two books sharing a bookmark, and a position that
// silently reset if a publisher edited the metadata. Beside the book it is
// obvious, deleted with the book, and needs no key at all.
std::string book_pos_path(const char* book_path);

// The home carousel's cover box, in panel pixels. The converter renders
// cover_home.bin to fit exactly this, and HomeScreen reads it 1:1 — the device
// has no scaler, so a cover is a header and one fread.
//
// Derived from HomeScreen::draw_all_'s layout. Note the panel's app-space
// height is **793**, not 800: DisplayFrame::kPhysicalWidth excludes the 7
// hidden columns at the leading edge of the glass. Getting that wrong is what
// made these too tall, which quietly put every home cover on a path it was
// never meant to take.
//
//   battery_row_h_ = kBatteryPadY(10) + ui_large.y_advance(31)          =  41
//   box_y          = battery_row_h_ + kHeroGap(24)                      =  65
//   below_h        = 26 + 2*ui_header.y_advance(41) + 6 + 31 + 26 + 19 + 28
//                                                                       = 218
//   width  = 480 - 2*kPad(16) - 2*(kFrameGap(10) + kFrameW(2))          = 424
//   height = 793 - box_y - below_h - 2*(kFrameGap + kFrameW)            = 486
//
// If the layout moves these drift and the screen shows its placeholder (the
// device will not scale a cover) until the library is re-converted.
inline constexpr int kHomeCoverW = 424;
inline constexpr int kHomeCoverH = 486;

// The book list's thumbnail box. The converter renders cover_list.bin to fit
// inside it, so MainMenu reads it straight into its cache with no scaling;
// a book without the file draws an empty frame. Derived from ListMenuScreen's
// two-title-line detail row:
//
//   slot   = kPadT(5) + 2*ui_header.y_advance(41) + kGap(3)
//            + ui_large.y_advance(31) + kPadB(6) + divider(1)              = 128
//   height = slot - divider(1) - 2*kThumbPad(6)                             = 115
//   width  = height * 2 / 3                                                 =  76
//
// Same drift rule as the home box: if the row moves, rows draw empty frames
// until the library is re-converted.
inline constexpr int kListCoverW = 76;
inline constexpr int kListCoverH = 115;

// The sleep screen, in panel pixels (portrait) — the whole visible panel.
// The converter composes cover_sleep.bin at exactly this size, artwork scaled
// to fit, centred, and the letterbox bars already filled, so
// Application::do_sleep_ blits it 1:1. A file of any other size is ignored and
// the wordmark shown instead; that is what an unconverted library looks like.
// Application.cpp static_asserts these against the panel — the first version of
// this said 800 and every cover silently failed the size check.
inline constexpr int kSleepCoverW = 480;
inline constexpr int kSleepCoverH = 793;

// On-card layouts, both baked for the portrait screens so the device copies
// bytes instead of transposing bits:
//
//   cover_list.bin, cover_home.bin: u16 w, u16 h, then w columns of
//     ceil(h / 8) bytes, top pixel MSB, bit set = white
//     (DrawBuffer::blit_1bit_cols).
//   cover_sleep.bin: the panel's own frame, kSleepFrameBytes, no header:
//     physical row kSleepFrameRows - 1 - x, bit kSleepFrameOffX + y holds
//     portrait pixel (x, y). Read straight into the draw buffer.
//
// Frame constants mirror DisplayFrame (Application.cpp static_asserts them);
// they live here so the host converter needs no display header.
inline constexpr int kSleepFrameStride = 100;
inline constexpr int kSleepFrameRows = 480;
inline constexpr int kSleepFrameOffX = 7;
inline constexpr size_t kSleepFrameBytes = static_cast<size_t>(kSleepFrameStride) * kSleepFrameRows;

inline constexpr size_t cover_cols_bytes(int w, int h) {
  return static_cast<size_t>((h + 7) / 8) * static_cast<size_t>(w);
}

// Portrait pixel (x, y) of a sleep frame: true = white.
inline bool sleep_frame_white(const uint8_t* frame, int x, int y) {
  const int bit = kSleepFrameOffX + y;
  return (frame[static_cast<size_t>(kSleepFrameRows - 1 - x) * kSleepFrameStride + (bit >> 3)] >> (7 - (bit & 7))) & 1;
}

}  // namespace wintergreen
