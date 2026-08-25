#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "../ContentModel.h"
#include "WgbFormat.h"

namespace wintergreen {

// Batches small writes into a memory buffer (4 KB), flushing to the
// underlying FILE* only when full or on seek.  tell() is O(1).
class BufferedFileWriter {
 public:
  BufferedFileWriter() = default;
  ~BufferedFileWriter() {
    close();
  }
  BufferedFileWriter(const BufferedFileWriter&) = delete;
  BufferedFileWriter& operator=(const BufferedFileWriter&) = delete;

  bool open(const char* path);
  void close();
  bool write(const void* data, size_t size);
  bool seek(uint32_t offset);
  bool flush();
  uint32_t tell() const {
    return pos_;
  }
  bool is_open() const {
    return f_ != nullptr;
  }

 private:
  static constexpr size_t kBufSize = 4096;
  FILE* f_ = nullptr;
  uint32_t pos_ = 0;
  size_t used_ = 0;
  uint8_t buf_[kBufSize];
};

// Writes an WGB file sequentially.  Usage:
//
//   WgbWriter w;
//   w.open("book.wgb");
//   w.begin_chapter();
//   w.write_paragraph(para);
//   w.end_chapter();
//   w.finish(metadata, toc);
//
class WgbWriter {
 public:
  WgbWriter() = default;
  ~WgbWriter() {
    close();
  }

  WgbWriter(const WgbWriter&) = delete;
  WgbWriter& operator=(const WgbWriter&) = delete;

  bool open(const char* path);
  void close();

  // Call before writing paragraphs for a new chapter.
  void begin_chapter();

  // Write one paragraph.  Returns false on I/O error.
  bool write_paragraph(const Paragraph& para);

  // Write a text paragraph from a raw run slice — avoids building an intermediate
  // Paragraph with a heap-allocated runs vector.  Used by the split-write path.
  bool write_text_paragraph(const TextParagraph& meta, uint16_t spacing, const Run* runs, size_t run_count);

  // Call after writing all paragraphs for a chapter.
  void end_chapter();

  // Add an image reference. Returns the index to use in paragraph image refs.
  // The bytes themselves are attached later with set_image_data(), once the
  // whole EPUB entry has been read.
  uint16_t add_image_ref(uint32_t local_header_offset, uint16_t width, uint16_t height);

  // Store `size` bytes of already-compressed image data (JPEG/PNG exactly as it
  // appeared in the EPUB) for image `idx`, to be emitted into the blob section.
  void set_image_data(uint16_t idx, std::vector<uint8_t>&& data);
  bool image_has_data(uint16_t idx) const {
    return idx < image_data_.size() && !image_data_[idx].empty();
  }

  // Update the size of an existing image ref (used by WGB converter after
  // lazy resolution).  No-op if idx is out of range.
  void update_image_size(uint16_t idx, uint16_t width, uint16_t height);

  // Returns true if the image ref at idx has non-zero dimensions.
  bool image_size_known(uint16_t idx) const {
    return idx < images_.size() && (images_[idx].width != 0 || images_[idx].height != 0);
  }

  // Finalize: write index tables, metadata, TOC and fix up header.
  bool finish(const EpubMetadata& meta, const TableOfContents& toc);

 private:
  BufferedFileWriter bw_;
  uint32_t paragraph_count_ = 0;
  std::vector<WgbChapterEntry> chapters_;
  std::vector<WgbImageRef> images_;
  std::vector<std::vector<uint8_t>> image_data_;  // parallel to images_
  bool in_chapter_ = false;

  // Per-chapter state
  uint16_t chapter_para_count_ = 0;  // paragraph count in current chapter
  uint32_t chapter_char_count_ = 0;  // total text chars (bytes) in current chapter

  // Descriptor table: streamed to a single temp file for the whole book.
  // Each 8-byte entry {file_offset(u32), char_offset(u32)} is appended as paragraphs
  // are written. end_chapter() seeks back to chapter_desc_start_ and copies exactly
  // chapter_para_count_*8 bytes into the WGB. Zero heap allocation, one FD, unbounded.
  FILE* desc_tmp_ = nullptr;
  char desc_tmp_path_[260] = {};
  uint32_t chapter_desc_start_ = 0;  // byte offset in desc_tmp_ where current chapter begins

  // Anchor table: streamed directly to a temp file during conversion to avoid
  // large contiguous RAM allocation. Copied into the WGB at finish().

  // Write raw bytes.
  bool write_bytes(const void* data, size_t size);

  // Write a length-prefixed UTF-8 string (uint16 length + bytes).
  bool write_string(const std::string& s);
};

}  // namespace wintergreen
