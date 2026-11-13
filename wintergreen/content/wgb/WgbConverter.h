#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../BitmapFont.h"
#include "../Book.h"
#include "WgbWriter.h"

namespace wintergreen {

// One reviewed chapter, as decided by `books`. Chapters are listed in reading
// order and together cover the whole spine: each owns everything from its start
// (spine file `spine_idx`, at the element with id `fragment`, or the file's
// first byte when `fragment` is empty) up to the next entry's start.
//
// A dropped entry (`keep` false) is still listed: its start is where dropped
// content begins. Nothing it owns reaches the WGB, even when it shares an XHTML
// file with a kept chapter. The first entry must start at spine file 0.
//
// There are no labels: the device shows kept entry i as the number i + 1.
struct PlannedChapter {
  uint16_t spine_idx = 0;
  std::string fragment;
  bool keep = true;
};

// Convert an opened EPUB book to WGB format using the streaming path.
// The Book must already be open().  Writes output to `output_path`.
// The chapters written are the kept entries of `plan`; the EPUB's own NCX is
// ignored.
// Spine files with no kept content are left out entirely.
// Optional work_buf/xml_buf avoid heap allocation for the decompression
// buffers (pass nullptr to allocate from heap).
// Optional progress_cb is called after each spine file with (done, total).
//
// `fonts` is the device's reader font bundle (all sizes): every page is baked
// with it. `err` says why on failure, when there is more to say than "failed".
bool convert_epub_to_wgb_streaming(Book& book, const char* output_path, const std::vector<PlannedChapter>& plan,
                                   const BitmapFontSet& fonts, std::string& err,
                                   uint8_t* work_buf = nullptr, uint8_t* xml_buf = nullptr,
                                   std::function<void(int, int)> progress_cb = nullptr);

}  // namespace wintergreen
