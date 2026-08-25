#pragma once

// WGF (Microreader Bitmap Font) binary format — version 4.
//
// Each WGF file contains one font at one pixel size, with up to 4 styles
// (Regular + optional Bold, Italic, BoldItalic) and optional 2-bit grayscale
// data for antialiased rendering on e-ink displays.
//
// Layout (single-style, style_flags=0):
//   [WgfHeader]                     40 bytes
//   [WgfRange × num_ranges]         8 bytes each
//   [WgfGlyph × num_glyphs]        10 bytes each
//   [BW bitmap data]               variable (1-bit packed, MSB-first)
//   [Gray LSB bitmap data]         variable (same size as BW; absent if gray_lsb_offset=0)
//   [Gray MSB bitmap data]         variable (same size as BW; absent if gray_msb_offset=0)
//
// Grayscale: each glyph has 3 bitmap sections (BW, LSB, MSB) all sharing the
// same per-glyph bitmap_offset. The offset is relative to bitmap_data_offset
// for BW, gray_lsb_offset for LSB, gray_msb_offset for MSB.
// 2-bit gray value = (MSB << 1) | LSB:
//   0 = white, 1 = light gray, 2 = gray, 3 = dark gray.
//
// Multi-style (style_flags != 0): additional style sections are appended
// between the Regular glyph table and the bitmap data. The header fields
// bold_offset / italic_offset / bold_italic_offset point to WgfStyleSection
// structs (4 bytes each) followed by their own WgfRange[] + WgfGlyph[] tables.
// All bitmap_offset values are relative to bitmap_data_offset (shared pool).
//
// Multiple sizes (Small/Normal/Large) are separate WGF files loaded into
// a BitmapFontSet at runtime.
//
// Glyph lookup: binary-search ranges by codepoint → index into glyph table.
// Bitmap data: each glyph has ceil(bitmap_width/8) × bitmap_height bytes.
// Bit polarity: 1 = white, 0 = black (bit CLEAR = draw pixel).

#include <cstddef>
#include <cstdint>

namespace wintergreen {

// The magic is the version: an incompatible layout change changes these bytes,
// so a stale file is rejected by the existing check rather than misparsed by a
// reader trusting a version number it does not understand.
static constexpr uint32_t kWgfMagic = 0x31464757;  // "WGF1" little-endian

#pragma pack(push, 1)

// File header — 49 bytes, all fields little-endian.
struct WgfHeader {
  uint32_t magic;               //  0: must be kWgfMagic
  uint8_t glyph_height;         //  4: maximum rendered glyph height (px)
  uint8_t baseline;             //  5: distance from line top to baseline (px)
  uint8_t y_advance;            //  6: line height / vertical advance (px)
  uint8_t default_advance;      //  7: fallback xAdvance for missing glyphs
  uint8_t style_flags;          //  8: bitmask of present styles (bit0=Regular, bit1=Bold, bit2=Italic, bit3=BoldItalic)
  uint16_t num_ranges;          //  9: number of WgfRange entries (Regular style)
  uint16_t num_glyphs;          // 11: total glyph count (Regular style)
  uint16_t nominal_size;        // 13: nominal pixel size requested at generation
  uint32_t kerning_length;      // 15: byte length of kerning data (Regular style)
  uint32_t bitmap_data_offset;  // 19: byte offset from file start to BW bitmap data (shared by all styles)
  uint32_t bold_offset;         // 23: file offset to Bold WgfStyleSection (0 = absent)
  uint32_t italic_offset;       // 27: file offset to Italic WgfStyleSection (0 = absent)
  uint32_t bold_italic_offset;  // 31: file offset to BoldItalic WgfStyleSection (0 = absent)
  uint32_t kerning_offset;      // 35: file offset to Regular style kerning data
  uint32_t gray_lsb_offset;     // 39: file offset to grayscale LSB bitmap data (0 = absent)
  uint32_t gray_msb_offset;     // 43: file offset to grayscale MSB bitmap data (0 = absent)
  int8_t underline_pos;         // 47: distance below baseline to underline top (px, positive = below)
  uint8_t underline_thickness;  // 48: underline height in pixels (>= 1)
};
static_assert(sizeof(WgfHeader) == 49, "WgfHeader must be 49 bytes");

// Style section preamble — appears at bold_offset / italic_offset / bold_italic_offset.
// Immediately followed by WgfRange[num_ranges] then WgfGlyph[num_glyphs] then WgfClassKerning data block.
struct WgfStyleSection {
  uint16_t num_ranges;
  uint16_t num_glyphs;
  uint32_t kerning_length;
};
static_assert(sizeof(WgfStyleSection) == 8, "WgfStyleSection must be 8 bytes");

// Unicode range — maps a contiguous codepoint block to a slice of the glyph table.
// 8 bytes each.
struct WgfRange {
  uint32_t first_codepoint;    // first Unicode codepoint in this range
  uint16_t count;              // number of consecutive codepoints
  uint16_t glyph_table_start;  // index of the first glyph in the glyph table
};
static_assert(sizeof(WgfRange) == 8, "WgfRange must be 8 bytes");

// Per-glyph metrics + bitmap location — 10 bytes each.
struct WgfGlyph {
  uint32_t bitmap_offset;  // byte offset into bitmap data section
  uint8_t advance_width;   // horizontal advance (cursor movement)
  uint8_t bitmap_width;    // bitmap width in pixels (0 for space-like glyphs)
  uint8_t bitmap_height;   // bitmap height in pixels
  int8_t x_offset;         // horizontal offset from cursor to bitmap left edge
  int8_t y_offset;         // vertical offset from baseline to bitmap top (negative = above)
  uint8_t reserved;
};
static_assert(sizeof(WgfGlyph) == 10, "WgfGlyph must be 10 bytes");

// Class kerning metadata block (Format 3).
// The binary kerning section is laid out as follows:
//   uint8_t num_l_classes_minus_1;
//   uint8_t num_r_classes_minus_1;
//   uint8_t l_class_map[num_glyphs];
//   uint8_t r_class_map[num_glyphs];
//   int8_t  matrix[num_l_classes * num_r_classes];
//
struct WgfClassKerning {
  uint8_t num_l_classes_minus_1;
  uint8_t num_r_classes_minus_1;
  // Followed by variable length arrays:
  // uint8_t l_class_map[num_glyphs];
  // uint8_t r_class_map[num_glyphs];
  // int8_t  matrix[ (num_l_classes_minus_1+1) * (num_r_classes_minus_1+1) ];
};

#pragma pack(pop)

}  // namespace wintergreen
