#pragma once

// Host-only: the paragraph stream of the bake's intermediate WGB (see
// WgbFormat.h). The device never reads paragraphs; it draws baked pages.

#include <memory>
#include <vector>

#include "../ContentModel.h"
#include "../IParagraphSource.h"
#include "../TextLayout.h"
#include "WgbReader.h"

namespace wintergreen {

class WgbParagraphs {
 public:
  // `reader` must be open on an intermediate file and outlive this.
  explicit WgbParagraphs(WgbReader& reader);

  // Load the paragraph record at `file_offset`. On success `next_offset`, if
  // given, is where the following record starts. False on any read or format
  // error.
  bool load_paragraph(uint32_t file_offset, Paragraph& out, uint32_t* next_offset = nullptr);

  uint16_t image_count() const {
    return static_cast<uint16_t>(images_.size());
  }
  const WgbImageRef& image(uint16_t index) const {
    return images_[index];
  }

 private:
  WgbReader& reader_;
  std::vector<WgbImageRef> images_;
  std::vector<uint8_t> body_;

  bool parse_paragraph_(uint8_t type, Paragraph& out);
  bool deserialize_text(const uint8_t* data, size_t size, Paragraph& out);
};

// The paragraphs one page is laid out from: `count` records starting at
// `first`, walked forward through their size fields as they are touched. A
// spine file always ends a page, so `count` runs to the end of the page's spine
// file. Every LayoutWord::text of a page laid out from this source points into
// it, so it must outlive that PageContent.
class WgbPageSource : public IParagraphSource {
 public:
  WgbPageSource(WgbParagraphs& paras, uint32_t first, uint16_t count, bool unit_start)
      : paras_(paras), count_(count), unit_start_(unit_start), next_offset_(first) {
    loaded_.reserve(8);
  }

  size_t paragraph_count() const override {
    return count_;
  }
  bool starts_unit() const override {
    return unit_start_;
  }

  const Paragraph& paragraph(size_t index) const override {
    static const Paragraph kEmpty{};
    if (index >= count_)
      return kEmpty;
    while (loaded_.size() <= index) {
      loaded_.push_back(std::make_unique<Paragraph>());
      // A record that does not load stays empty, and so does everything after
      // it: the walk has lost its place.
      if (next_offset_ == 0 || !paras_.load_paragraph(next_offset_, *loaded_.back(), &next_offset_)) {
        *loaded_.back() = Paragraph{};
        next_offset_ = 0;
      }
    }
    return *loaded_[index];
  }

 private:
  WgbParagraphs& paras_;
  uint16_t count_;
  bool unit_start_;
  mutable uint32_t next_offset_;
  mutable std::vector<std::unique_ptr<Paragraph>> loaded_;
};

// Image dimensions, straight out of the image table: the converter rasterised
// every image to the size it will be drawn at.
inline ImageSizeQuery make_image_size_query(const WgbParagraphs& paras) {
  const WgbParagraphs* p = &paras;
  return [p](uint16_t key, uint16_t& w, uint16_t& h) -> bool {
    if (key >= p->image_count())
      return false;
    w = p->image(key).width;
    h = p->image(key).height;
    return w != 0 && h != 0;
  };
}

}  // namespace wintergreen
