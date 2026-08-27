#pragma once

// WGB — Wintergreen Book. A pre-processed binary format built on a computer and
// read directly by the device: paragraphs with styling and text inline, and
// images already rasterised to the 1-bit bitmaps the panel wants.
//
// File layout (all values little-endian):
//
//   [Header 24 bytes]
//   [Paragraph data — variable, written sequentially]   ← no prev/next links
//   [Per-chapter descriptor tables — paragraph_count × 8 bytes each]
//   [Chapter table  — chapter_count  × 16 bytes]
//   [Image ref table— image_count    × 12 bytes]
//   [Metadata blob  — variable]
//   [TOC blob       — variable]
//
// Per-chapter descriptor table (written immediately after the chapter's
// last paragraph):
//   N × { file_offset(u32), char_offset(u32) }
// file_offset: absolute position of the paragraph record in the file.
// char_offset: cumulative UTF-8 bytes of text before this paragraph.
// This allows O(1) random access to any paragraph and accurate char-based
// progress without reading any paragraph data.

#include <cstdint>
#include <cstring>

namespace wintergreen {

// ---------------------------------------------------------------------------
// Magic
// ---------------------------------------------------------------------------
//
// There is no version field. The magic *is* the version: any incompatible change
// to the layout changes these four bytes, so a stale file is rejected by the
// existing magic check instead of being misparsed by a reader that trusted a
// version number it did not understand. Bump the digit when the layout changes.
// Deliberately *not* bumped for paragraph splitting. The on-disk layout is
// unchanged — splitting only alters how many paragraphs a chapter holds — so an
// older file parses correctly. It keeps the oversized paragraphs, so it stays
// exposed to the heap abort until it is re-converted, but it opens and reads.
static constexpr uint8_t kWgbMagic[4] = {'W', 'G', 'B', '2'};

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
// Header (24 bytes, fixed)
// ---------------------------------------------------------------------------

struct WgbHeader {
  uint8_t magic[4];  // "WGB2"
  uint32_t paragraph_count;
  uint16_t chapter_count;
  uint16_t image_count;
  uint32_t chapter_offset;  // file offset of chapter table
  uint32_t image_offset;    // file offset of image ref table
  uint32_t meta_offset;     // file offset of metadata blob
};
static_assert(sizeof(WgbHeader) == 24, "WgbHeader must be 24 bytes");

// ---------------------------------------------------------------------------
// Chapter table entry (16 bytes each)
// ---------------------------------------------------------------------------

struct WgbChapterEntry {
  uint32_t para_table_offset;  // file offset of the paragraph descriptor table for this chapter
  uint32_t reserved;           // unused
  uint16_t paragraph_count;
  uint16_t reserved1;
  uint32_t char_count;  // total UTF-8 bytes of text in this chapter
};
static_assert(sizeof(WgbChapterEntry) == 16, "WgbChapterEntry must be 16 bytes");

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
struct WgbImageRef {
  uint32_t data_offset;  // offset of the bitmap within this WGB
  uint32_t data_size;    // (width+7)/8 * height
  uint16_t width;
  uint16_t height;
};
static_assert(sizeof(WgbImageRef) == 12, "WgbImageRef must be 12 bytes");

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
// A paragraph is the device's indivisible layout unit — every word and line
// vector for it is resident at once — so one oversized paragraph can abort the
// reader on a device with tens of KB free. Project Gutenberg's Odyssey has a
// 3,672-byte paragraph in its first preface, and reading that page with the BLE
// clicker connected reproducibly aborted the device: the allocation that failed
// was a 256-byte reserve, i.e. the heap was simply gone.
//
// The device budget is what sets this, not the shape of the input. Laying out a
// paragraph costs roughly 4 bytes of heap per byte of text (a LayoutWord per
// word at 16 bytes, plus a LayoutLine and its word vector per line), and the
// clicker leaves about 13 KB free. 1,500 bytes therefore lays out in ~6 KB and
// leaves room for the rest of the page; measured over four books, the worst page
// peak falls from 25,823 B to under 11 KB.
//
// It also sits below ordinary prose's ceiling (~2,300 bytes across this repo's
// test books), so a long paragraph in a normal book is now split too. That is
// intended: the cut is invisible, and being under budget everywhere is worth
// more than leaving big-but-not-pathological paragraphs whole.
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

inline void wgb_write_u8(uint8_t* dst, uint8_t v) {
  dst[0] = v;
}
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
