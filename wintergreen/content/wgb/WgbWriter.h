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
//   w.finish(metadata);
//   paginate_wgb(path, w.units(), chapter_starts, fonts);
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

  // Emit an oversized paragraph with no <br> as several, cut at sentence
  // boundaries. The rendered result is unchanged: the cut lands between two
  // sentences and continuation chunks carry no spacing and no first-line indent.
  bool write_sentence_split_paragraph_(const TextParagraph& tp, uint16_t spacing);

  // Emit a <br>-delimited paragraph as several, so no single layout unit is
  // large enough to exhaust the device's heap. See the call site.
  bool write_split_text_paragraph_(const TextParagraph& tp, uint16_t spacing);

  // Paragraphs written into the current chapter so far, i.e. the index the next
  // paragraph will get.
  //
  // Chapter starts store a paragraph index, and the EPUB parser counts *source*
  // paragraphs while the splitters above make the writer emit more than one for
  // some of them. Resolving an anchor against the parser's count therefore
  // drifts by the number of splits before it, and a chapter jump lands in the
  // wrong place. WgbConverter's id_sink reads this instead.
  uint16_t chapter_paragraph_count() const {
    return in_chapter_ ? static_cast<uint16_t>(units_.back().size()) : 0;
  }

  // Call after writing all paragraphs for a chapter.
  void end_chapter();

  // Close a chapter that received no paragraphs without recording it, so the
  // book never holds an empty unit. Nothing was written for it.
  void cancel_chapter();

  // Chapters recorded so far, i.e. the index the open (or next) chapter gets.
  uint16_t chapter_count() const {
    return static_cast<uint16_t>(in_chapter_ ? units_.size() - 1 : units_.size());
  }

  // "Chapters" here are pagination units (one spine file each): a unit always
  // ends a page. Each is the file offsets of its paragraph records, kept in
  // memory for WgbPaginator; nothing about them is written to the file.
  const std::vector<std::vector<uint32_t>>& units() const {
    return units_;
  }

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

  // Finalize: write images, metadata and the header, with an empty page
  // section. WgbPaginator appends the pages afterwards. units() stays valid
  // until close().
  bool finish(const EpubMetadata& meta);

 private:
  BufferedFileWriter bw_;
  std::vector<std::vector<uint32_t>> units_;
  std::vector<WgbImageRef> images_;
  std::vector<std::vector<uint8_t>> image_data_;  // parallel to images_
  bool in_chapter_ = false;

  // Note the offset of the paragraph record about to be written.
  void record_paragraph_();

  // Write raw bytes.
  bool write_bytes(const void* data, size_t size);

  // Write a length-prefixed UTF-8 string (uint16 length + bytes).
  bool write_string(const std::string& s);
};

}  // namespace wintergreen
