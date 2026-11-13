#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "../ContentModel.h"
#include "WgbFormat.h"

namespace wintergreen {

// Reads a WGB file. open() keeps only the header and the metadata in RAM; page
// records, display lists, image refs and chapter starts are read from the card
// when asked for, so nothing here grows with the book.
//
// Also opens the bake's intermediate file (empty page section), which is how
// WgbParagraphs and WgbPaginator get at it.
class WgbReader {
 public:
  WgbReader() = default;
  ~WgbReader() {
    close();
  }

  WgbReader(const WgbReader&) = delete;
  WgbReader& operator=(const WgbReader&) = delete;

  bool open(const char* path);
  void close();
  bool is_open() const {
    return f_ != nullptr;
  }

  const WgbHeader& header() const {
    return header_;
  }
  uint16_t image_count() const {
    return header_.image_count;
  }
  // Number of reviewed chapters (the quick menu's numbered grid).
  uint16_t chapter_count() const {
    return header_.chapter_count;
  }
  uint32_t page_count(uint8_t layout) const {
    return layout < kWgbLayouts ? header_.page_count[layout] : 0;
  }

  // Page record `index` of `layout`. False past the end or on a read error.
  bool page(uint8_t layout, uint32_t index, WgbPage& out);

  // Read the display list of `rec` into `buf` (capacity `cap`). False on a
  // read error or one larger than `cap`.
  bool read_page(const WgbPage& rec, uint8_t* buf, size_t cap);

  // The page of `layout` holding the reading position (para_offset,
  // text_offset): the last page that starts at or before it, by binary search
  // over the records on the card. 0 for a position before the first page.
  uint32_t find_page(uint8_t layout, uint32_t para_offset, uint16_t text_offset);

  // The page each chapter starts on in `layout`, chapter_count() entries.
  bool chapter_pages(uint8_t layout, std::vector<uint32_t>& out);

  // Image ref `index`, one 12-byte read. False past the end or on a read error.
  bool image_ref(uint16_t index, WgbImageRef& out);

  const EpubMetadata& metadata() const {
    return metadata_;
  }
  // The open file handle, shared with ReaderScreen::draw_image_ so drawing a
  // figure does not reopen the book. Every read here seeks first, so a borrowed
  // read cannot disturb this class's position.
  FILE* file() const {
    return f_;
  }

 private:
  FILE* f_ = nullptr;
  WgbHeader header_{};
  EpubMetadata metadata_;

  // File offset of `layout`'s chapter-start table; its page records follow.
  uint32_t layout_base_(uint8_t layout) const;
  bool read_at_(uint32_t at, void* buf, size_t size);
  bool read_bytes(void* buf, size_t size);
  std::string read_string();
};

}  // namespace wintergreen
