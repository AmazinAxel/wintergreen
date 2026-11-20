#pragma once

// WGB — Wintergreen Book. Built on a computer, read directly by the device:
// every page of every layout already laid out as a display list, and images
// already rasterised to the 1-bit bitmaps the panel wants.
//
// File layout (all values little-endian):
//
//   [Header 32 bytes]
//   [Image bitmaps]
//   [Image ref table — image_count × 12 bytes]
//   [Metadata blob   — title, author, language strings]
//   [Page section    — per layout: chapter_count × u32 chapter start page,
//                      then page_count[L] × 14-byte page records]
//   [Display lists   — one per page record, where the record points]
//
// **Pages are fully baked.** `books` lays every page out on the build machine
// with TextLayout and the device's own font bundle, for each of the kWgbLayouts
// (font size × orientation), and stores the result: every line's baseline,
// every word's x, font, style and text, every image slice and rule. A page turn
// on the device reads one record and one display list and blits it. The device
// has no layout engine, no hyphenation and no paragraph model.
//
// The bake works from an intermediate file of the same shape with a paragraph
// stream between the header and the image bitmaps (WgbWriter's output, page
// section empty); WgbPaginator lays it out and writes the final file without
// the stream. Only the intermediate is ever read with WgbParagraphs.h.
//
// The coupling is total and unchecked (no magic, no version): **changing the
// font bundle, TextLayout, page geometry or hyphenation means rebaking the
// library in the same commit.** A stale book still opens and draws the pages it
// was baked with; a font bundle change draws them with the wrong glyphs.

#include <cstdint>
#include <cstring>

namespace wintergreen {

// ---------------------------------------------------------------------------
// Layouts
// ---------------------------------------------------------------------------
//
// Layout index = font_size_idx * 2 + (landscape ? 1 : 0). Must match the number
// of sizes tools/make_font.py builds into the bundle (ReaderSettings).
static constexpr uint8_t kWgbFontSizes = 2;
static constexpr uint8_t kWgbLayouts = kWgbFontSizes * 2;
inline constexpr uint8_t wgb_layout_index(uint8_t font_size_idx, bool landscape) {
  return static_cast<uint8_t>(font_size_idx * 2 + (landscape ? 1 : 0));
}

// ---------------------------------------------------------------------------
// Header (32 bytes, fixed)
// ---------------------------------------------------------------------------
//
// No magic and no version. WgbReader::open instead requires the image ref table
// to end exactly at meta_offset and the page section to end exactly at the end
// of the file, which a stale or non-WGB file fails.
struct WgbHeader {
  uint16_t image_count;
  uint16_t chapter_count;
  uint32_t image_offset;  // file offset of image ref table
  uint32_t meta_offset;   // file offset of metadata blob
  uint32_t pages_offset;  // file offset of the page section
  uint32_t page_count[kWgbLayouts];
};
static_assert(sizeof(WgbHeader) == 32, "WgbHeader must be 32 bytes");

// In the intermediate file, paragraph records start right after the header; in
// the final file the image bitmaps do.
static constexpr uint32_t kWgbFirstParagraph = sizeof(WgbHeader);
static constexpr size_t kWgbParaHeaderSize = 5;  // type(1) + body size(4)
static constexpr uint16_t kWgbMaxUnitParagraphs = 0x7FFF;

// ---------------------------------------------------------------------------
// Page record (14 bytes)
// ---------------------------------------------------------------------------
//
//   para_offset   intermediate-file offset of the page's first paragraph record
//   offset        line index into it (text) or pixel row (image)
//   text_offset   byte offset into that paragraph's text
//   dl_offset     file offset of the page's display list
//   dl_size       its length in bytes, at most kWgbMaxPageBytes
//
// The first three are the page's start position, nothing more: no paragraph is
// ever read through them. (para_offset, offset, text_offset) orders every
// layout's pages the same way, so a font or rotation change, and book.pos
// (para_offset + text_offset), map through it.
struct WgbPage {
  uint32_t para_offset = 0;
  uint16_t offset = 0;
  uint16_t text_offset = 0;
  uint32_t dl_offset = 0;
  uint16_t dl_size = 0;
};
static constexpr size_t kWgbPageSize = 14;

// ---------------------------------------------------------------------------
// Display list (one per page)
// ---------------------------------------------------------------------------
//
//   u16 item_count, then items, each starting with a u8 tag. Coordinates are
//   logical page pixels in the layout's rotation, padding included.
//
//   kWgbItemLine   u16 baseline_y, u8 word_count, then per word:
//                    u8 dx: x minus the previous word's x (0 before the
//                      line's first word); kWgbAbsX: u16 absolute x follows
//                    u8 len in bits 0-6 (kWgbLongWord: u16 len follows),
//                      bit 7 (kWgbFlagsFollow): u8 flags follows
//                    [u16 len], [u8 flags], len bytes of glyph indices
//                      into the word's font and style (wgb_read_glyph:
//                      < 0x80 one byte, else two big-endian with the top
//                      bit set; kWgbNoGlyph = no glyph, default advance).
//                      Resolved at bake time, U+FFFD fallback included, so
//                      the device never decodes UTF-8 or searches ranges.
//                  A word without a flags byte keeps the previous word's
//                  flags, across lines; a page starts at flags 0.
//                  flags: bits 0-1 FontStyle, bits 2-4 font index in the
//                  bundle (resolved from size_pct at bake time), bits 5-6
//                  VerticalAlign (the device shifts the baseline by 20% of that
//                  font's y_advance, as the layout engine's draw did)
//   kWgbItemRule   u16 x, y, w, h — a 2 px rule centred in the h-high slot
//   kWgbItemImage  u16 key, x, y, w, full_h, src_y, clip_h — rows
//                  [src_y, src_y + clip_h) of a w × full_h stored bitmap
//
// Items are stored in draw order: lines, then rules, then images, so images
// paint over anything they overlap, exactly as before.
static constexpr uint8_t kWgbItemLine = 0;
static constexpr uint8_t kWgbItemRule = 1;
static constexpr uint8_t kWgbItemImage = 2;
static constexpr uint8_t kWgbAbsX = 0xFF;
static constexpr uint8_t kWgbLongWord = 0x7F;
static constexpr uint8_t kWgbFlagsFollow = 0x80;
static constexpr uint16_t kWgbNoGlyph = 0x7FFF;

// One glyph index of a word; -1 for kWgbNoGlyph (or a truncated pair).
inline int wgb_read_glyph(const uint8_t*& p, const uint8_t* end) {
  const uint8_t b0 = *p++;
  if (b0 < 0x80)
    return b0;
  if (p >= end)
    return -1;
  const int v = ((b0 & 0x7F) << 8) | *p++;
  return v == kWgbNoGlyph ? -1 : v;
}
static constexpr size_t kWgbRuleItemSize = 1 + 8;
static constexpr size_t kWgbImageItemSize = 1 + 14;

// Largest display list the bake accepts, and the size of the device's one
// page buffer. paginate_book_test prints the maximum: 879 B over two novels
// (2026-11-11), so this is ~4x headroom for dense pages.
static constexpr size_t kWgbMaxPageBytes = 4 * 1024;

// Where one reviewed chapter begins, as the converter hands it to the
// paginator: spine-file unit and paragraph index within it. Not stored; the
// page section stores the page each chapter starts on, per layout.
struct WgbChapterStart {
  uint16_t unit;
  uint16_t para;
};

// ---------------------------------------------------------------------------
// Image raster box
// ---------------------------------------------------------------------------
//
// Images are stored already scaled and dithered to fit this box, in portrait
// panel pixels: full panel width, and the page height less its vertical padding
// (see ReaderSettings and TextLayout's promoted-image clamp). The converter
// fits to it and the device blits the result 1:1 — scale_image() never enlarges,
// so a landscape page centres the stored bitmap rather than stretching it.
//
// Changing either number means re-converting the library; nothing on the device
// can recover detail that was thrown away on the build machine.
static constexpr uint16_t kWgbImageBoxW = 480;
static constexpr uint16_t kWgbImageBoxH = 772;

// ---------------------------------------------------------------------------
// Image reference entry (12 bytes each)
// ---------------------------------------------------------------------------

// Images are stored as raw 1-bit bitmaps, already scaled and dithered by the
// converter to fit kWgbImageBoxW x kWgbImageBoxH. width/height are the stored
// bitmap's dimensions — which are also the dimensions it is drawn at, since
// scale_image() never enlarges — and the data is (width+7)/8 bytes per row,
// MSB first, bit clear = ink.
//
// They used to be the original JPEG/PNG bytes, decoded and dithered on the
// device on first view and memoised to the SD card. Rasterising on the build
// machine instead cut book size by about 70%, removed the JPEG and PNG decoders
// from the firmware entirely, made an image page turn as fast as a text one, and
// stopped the reader writing image caches to the card.
//
// Followed directly by a portrait copy in 8-row bands (wgb_image_bands_bytes):
// band b is `width` bytes, byte c = column c's pixels in rows 8b..8b+7, top
// MSB (padding rows white). In portrait a column is a panel row, so each band
// is a byte copy per column instead of a bit transpose, and a page's slice of
// the image is still one contiguous read. Landscape reads the rows.
struct WgbImageRef {
  uint32_t data_offset;  // offset of the bitmap within this WGB
  uint32_t data_size;    // (width+7)/8 * height + wgb_image_bands_bytes
  uint16_t width;
  uint16_t height;
};
static_assert(sizeof(WgbImageRef) == 12, "WgbImageRef must be 12 bytes");

inline uint32_t wgb_image_rows_bytes(uint32_t w, uint32_t h) {
  return (w + 7) / 8 * h;
}
inline uint32_t wgb_image_bands_bytes(uint32_t w, uint32_t h) {
  return (h + 7) / 8 * w;
}

// ---------------------------------------------------------------------------
// Paragraph type tags (match ParagraphType enum values)
// ---------------------------------------------------------------------------

static constexpr uint8_t kWgbParaText = 0;
static constexpr uint8_t kWgbParaImage = 1;
static constexpr uint8_t kWgbParaHr = 2;
static constexpr uint8_t kWgbParaPageBreak = 3;

// ---------------------------------------------------------------------------
// Sentinel values for optional fields
// ---------------------------------------------------------------------------

static constexpr uint8_t kWgbAlignDefault = 0xFF;
static constexpr int16_t kWgbIndentNone = 0x7FFF;
static constexpr uint16_t kWgbSpacingDefault = 0xFFFF;

// Text bytes above which the converter splits a paragraph into several.
//
// Originally a device RAM bound (a paragraph was the device's indivisible layout
// unit, and one oversized paragraph aborted the reader with the clicker up).
// Layout now runs only at bake time, so it is no longer load-bearing for RAM;
// it stays because the cut is invisible and it keeps every page start's
// text_offset far inside its u16.
//
// Splitting happens at a <br> where one exists, and otherwise between sentences.
// Both are places a line break could already fall, so the rendered result is
// unchanged — see write_split_text_paragraph_ and write_sentence_split_paragraph_.
static constexpr size_t kSplitParagraphBytes = 700;
static constexpr uint16_t kWgbNoImage = 0xFFFF;
static constexpr uint8_t kWgbHrWidthDefault = 0xFF;

// ---------------------------------------------------------------------------
// Little-endian serialization helpers
// ---------------------------------------------------------------------------

inline void wgb_write_u16(uint8_t* dst, uint16_t v) {
  dst[0] = static_cast<uint8_t>(v);
  dst[1] = static_cast<uint8_t>(v >> 8);
}
inline void wgb_write_i16(uint8_t* dst, int16_t v) {
  wgb_write_u16(dst, static_cast<uint16_t>(v));
}
inline void wgb_write_u32(uint8_t* dst, uint32_t v) {
  dst[0] = static_cast<uint8_t>(v);
  dst[1] = static_cast<uint8_t>(v >> 8);
  dst[2] = static_cast<uint8_t>(v >> 16);
  dst[3] = static_cast<uint8_t>(v >> 24);
}

inline uint8_t wgb_read_u8(const uint8_t* src) {
  return src[0];
}
inline uint16_t wgb_read_u16(const uint8_t* src) {
  return static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8);
}
inline int16_t wgb_read_i16(const uint8_t* src) {
  return static_cast<int16_t>(wgb_read_u16(src));
}
inline uint32_t wgb_read_u32(const uint8_t* src) {
  return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) | (static_cast<uint32_t>(src[2]) << 16) |
         (static_cast<uint32_t>(src[3]) << 24);
}

}  // namespace wintergreen
