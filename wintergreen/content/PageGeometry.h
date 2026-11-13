#pragma once

// The reader's page geometry, shared by the device (ReaderScreen) and the
// build machine (WgbPaginator), which bakes every page with it. Host-safe.
//
// **Any change here means rebaking the library** — see WgbFormat.h.

#include <cstdint>

#include "../display/DrawBuffer.h"
#include "TextLayout.h"

namespace wintergreen {

// Margins are fixed at what used to be the "Normal" preset; alignment, line
// spacing and publisher font sizes defer to the book's own CSS.
inline constexpr uint16_t kReaderHPadding = 12;
inline constexpr uint16_t kReaderVPadding = 4;
// Height of the low-battery bar ReaderScreen paints along the bottom edge.
inline constexpr uint16_t kLowBatteryBarH = 5;
// Deepest glyph extent below the baseline in the reader font (Libron 36 px,
// any style). The last baseline sits this far above the bar, so descenders
// never reach it. Font change = re-measure.
inline constexpr uint16_t kReaderMaxDescender = 10;
// Last baseline to panel edge: descender room plus the bar. Nothing else is
// drawn in it.
inline constexpr uint16_t kReaderBottomMargin = kReaderMaxDescender + kLowBatteryBarH;
inline constexpr uint16_t kReaderParaSpacing = 8;

// Landscape is Rotation::Deg0 (the panel's native 793 × 480); portrait Deg90.
inline PageOptions reader_page_options(bool landscape) {
  const int w = landscape ? DrawBuffer::kHeight : DrawBuffer::kWidth;
  const int h = landscape ? DrawBuffer::kWidth : DrawBuffer::kHeight;
  PageOptions opts(static_cast<uint16_t>(w), static_cast<uint16_t>(h), 0, kReaderParaSpacing);
  opts.padding_right = kReaderHPadding;
  opts.padding_left = kReaderHPadding;
  opts.padding_top = kReaderVPadding;
  opts.padding_bottom = static_cast<uint16_t>(kReaderBottomMargin + kReaderVPadding + (landscape ? 2 : 0));
  opts.center_text = true;
  return opts;
}

}  // namespace wintergreen
