#pragma once

#include <functional>

#include "../Book.h"
#include "MrbWriter.h"

namespace wintergreen {

// Convert an opened EPUB book to MRB format using the streaming path.
// The Book must already be open().  Writes output to `output_path`.
// Uses ~37KB working memory per chapter. Safe for ESP32's limited RAM.
// Optional work_buf/xml_buf avoid heap allocation for the decompression
// buffers (pass nullptr to allocate from heap).
// Optional progress_cb is called after each chapter with (chapters_done, total_chapters).
bool convert_epub_to_mrb_streaming(Book& book, const char* output_path, uint8_t* work_buf = nullptr,
                                   uint8_t* xml_buf = nullptr, std::function<void(int, int)> progress_cb = nullptr);

}  // namespace wintergreen
