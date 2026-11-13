#pragma once

// Host-only: bake the page section of a WGB. See WgbFormat.h.

#include <string>
#include <vector>

#include "../BitmapFont.h"
#include "../TextLayout.h"
#include "WgbFormat.h"

namespace wintergreen {

// Lay out every page of the intermediate WGB at `path` (written by
// WgbWriter::finish, page section empty) in each of the kWgbLayouts with
// TextLayout and `fonts`, store each page as a display list, and replace the
// file with the final book: no paragraph stream, page tables, display lists.
//
// `units` is WgbWriter::units(): the paragraph record offsets of each spine
// file, in order. A unit always ends a page. `starts` are the reviewed
// chapters; each becomes the page holding its first paragraph.
//
// False (and `err`) on a page that would stall, a field or page that does not
// fit its record, or an I/O error.
// One laid-out page as its display list (WgbFormat.h), checked to parse back
// and to fit kWgbMaxPageBytes. Exposed for tools/tests/baked_line_test.cpp.
bool serialize_wgb_page(const PageContent& pc, const BitmapFontSet& fset, uint16_t image_count,
                        std::vector<uint8_t>& out, std::string& err);

bool paginate_wgb(const char* path, const std::vector<std::vector<uint32_t>>& units,
                  const std::vector<WgbChapterStart>& starts, const BitmapFontSet& fonts, std::string& err);

}  // namespace wintergreen
