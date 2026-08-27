#pragma once

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../ContentModel.h"
#include "../IParagraphSource.h"
#include "../TextLayout.h"
#include "WgbFormat.h"

namespace wintergreen {

// Reads an WGB file.  Loads the chapter table and image refs into RAM on
// open(), then provides paragraph loading by file offset.  Paragraphs are
// linked (prev/next offsets) so they can be traversed sequentially.
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

  // Counts
  uint32_t paragraph_count() const {
    return header_.paragraph_count;
  }
  uint16_t chapter_count() const {
    return header_.chapter_count;
  }
  uint16_t image_count() const {
    return header_.image_count;
  }

  // Chapter navigation
  uint32_t chapter_para_table_offset(uint16_t chapter_idx) const;
  uint16_t chapter_paragraph_count(uint16_t chapter_idx) const;
  uint32_t chapter_char_count(uint16_t chapter_idx) const;

  // Sum of char_count across all chapters (0 if not stored, i.e. old WGB file).
  uint64_t total_char_count() const;

  // Load a single paragraph at a given file offset.
  // On success, fills `out` and sets ok = true.
  struct LoadResult {
    bool ok = false;
  };
  LoadResult load_paragraph(uint32_t file_offset, Paragraph& out);

  // Image references
  const WgbImageRef& image_ref(uint16_t index) const {
    return images_[index];
  }

  // Metadata
  const EpubMetadata& metadata() const {
    return metadata_;
  }
  // The open file handle, shared with ReaderScreen::draw_image_ so drawing a
  // figure does not reopen the book. Every read here seeks first, so a borrowed
  // read cannot disturb this class's position.
  FILE* file() const {
    return f_;
  }

  const TableOfContents& toc() const {
    return toc_;
  }

  // Returns true and sets para_idx if found; returns false if not found.

 private:
  FILE* f_ = nullptr;
  WgbHeader header_{};
  std::vector<WgbChapterEntry> chapters_;
  std::vector<WgbImageRef> images_;
  EpubMetadata metadata_;
  TableOfContents toc_;

  bool read_bytes(void* buf, size_t size);
  bool read_at(uint32_t offset, void* buf, size_t size);
  std::string read_string();
  bool deserialize_text(const uint8_t* data, size_t size, Paragraph& out);

  friend class WgbChapterSource;
};

// ---------------------------------------------------------------------------
// IParagraphSource backed by WgbReader — loads paragraphs on demand
// for a single chapter, with a sliding-window cache.
//
// On construction, scans the chapter's linked list to build a local
// vector of file offsets (~4 bytes per paragraph).  This replaces the
// old global paragraph index that was stored in the WGB file.
//
// Only kWindowSize paragraphs are kept in memory at once. When a
// paragraph outside the window is requested, the window slides to
// center on the new access point, evicting old entries.
// ---------------------------------------------------------------------------

class WgbChapterSource : public IParagraphSource {
 public:
  static constexpr size_t kWindowSize = 32;

  WgbChapterSource(WgbReader& reader, uint16_t chapter_idx) : reader_(reader) {
    uint16_t count = reader.chapter_paragraph_count(chapter_idx);
    if (count == 0)
      return;

    // Bulk-read the descriptor table: one seek + one fread of N×8 bytes.
    uint32_t table_off = reader.chapter_para_table_offset(chapter_idx);
    if (table_off == 0)
      return;

    offsets_.resize(count);
    para_char_offsets_.resize(count);

    // One fread for the whole descriptor table, parsed from memory. Reading the
    // 8-byte entries one at a time cost a FATFS + SPI round trip per paragraph,
    // and a chapter can have hundreds — it dominated chapter-load time, which is
    // on the path for every book open and every chapter jump. The staging buffer
    // is freed before layout begins, so the peak is 8 bytes per paragraph on top
    // of the two vectors that are kept anyway.
    {
      std::vector<uint8_t> table(static_cast<size_t>(count) * 8);
      fseek(reader.f_, static_cast<long>(table_off), SEEK_SET);
      if (fread(table.data(), 1, table.size(), reader.f_) != table.size()) {
        offsets_.clear();
        para_char_offsets_.clear();
        return;
      }
      for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* p = table.data() + static_cast<size_t>(i) * 8;
        offsets_[i] = wgb_read_u32(p);
        para_char_offsets_[i] = wgb_read_u32(p + 4);
      }
    }
    total_chars_ = reader.chapter_char_count(chapter_idx);

    size_t actual = count < kWindowSize ? count : kWindowSize;
    slots_.resize(actual);
    slot_index_.resize(actual, UINT32_MAX);
  }

  size_t paragraph_count() const override {
    return offsets_.size();
  }

  // Cumulative char offset just before paragraph `index` (0-based).
  // Returns total_chars() when index >= paragraph_count (= chars in whole chapter).
  uint32_t char_before_para(size_t index) const {
    if (index >= para_char_offsets_.size())
      return total_chars_;
    return para_char_offsets_[index];
  }

  // Total chars in this chapter (sum of all run text lengths).
  uint32_t total_chars() const {
    return total_chars_;
  }

  // Shrink the live window while a radio holds the heap, freeing the text and
  // run vectors of every slot above the limit.
  //
  // **Only safe between pages**, never mid-layout: LayoutWord::text points into
  // these slots, so dropping one under a laid-out page leaves dangling
  // pointers. It is called from Application::release_ram_for_radio(), which
  // also clears the page and paragraph caches that hold those pointers — the
  // three have to happen together or not at all.
  //
  // The vectors themselves are not resized: that would move the Paragraph
  // objects and dangle the same pointers by another route. Only the contents
  // above the limit are released, and find_slot_ stops handing them out.
  // **Do not add a way to shrink this window. It was tried and it corrupts the
  // displayed page.** Freeing slots above a limit — even keeping the current
  // paragraph and its neighbours — produced text drawn from freed memory:
  // "?  I   is mos   astonish" in place of the real line, consistently wrong
  // rather than intermittently.
  //
  // Every LayoutWord::text in the laid-out page points into these slots, and so
  // do the entries of TextLayout's paragraph cache. A page can span more
  // paragraphs than any "keep the current one" heuristic predicts, and the
  // caches outlive the page that built them, so there is no safe subset to
  // free while a book is open. The window is freed wholesale by load_chapter_()
  // and that is the only safe point.

  const Paragraph& paragraph(size_t index) const override {
    // Check if already in window
    for (size_t i = 0; i < slot_index_.size(); ++i) {
      if (slot_index_[i] == static_cast<uint32_t>(index))
        return slots_[i];
    }

    // Not in window — find a free slot or evict the furthest entry
    size_t slot = find_slot_(index);
    slot_index_[slot] = static_cast<uint32_t>(index);
    if (!reader_.load_paragraph(offsets_[index], slots_[slot]).ok)
      slots_[slot] = Paragraph{};
    return slots_[slot];
  }

 private:
  WgbReader& reader_;
  std::vector<uint32_t> offsets_;            // file offsets of each paragraph
  std::vector<uint32_t> para_char_offsets_;  // cumulative chars before each paragraph
  uint32_t total_chars_ = 0;                 // total chars in this chapter
  mutable std::vector<Paragraph> slots_;
  mutable std::vector<uint32_t> slot_index_;  // UINT32_MAX = empty
  // Slots find_slot_ may hand out; the whole window normally, fewer while a

  // Find best slot: prefer empty, then evict the one furthest from `index`
  size_t find_slot_(size_t index) const {
    size_t best = 0;
    uint32_t best_dist = 0;
    for (size_t i = 0; i < slot_index_.size(); ++i) {
      if (slot_index_[i] == UINT32_MAX)
        return i;  // empty slot
      uint32_t d = (slot_index_[i] > index) ? static_cast<uint32_t>(slot_index_[i] - index)
                                            : static_cast<uint32_t>(index - slot_index_[i]);
      if (d > best_dist) {
        best_dist = d;
        best = i;
      }
    }
    return best;
  }
};

// ---------------------------------------------------------------------------
// Build an ImageSizeQuery that resolves image dimensions from an WGB file.
// Fast path: width/height stored in WgbImageRef (from HTML attributes).
// Slow path: stream the image header from the EPUB local file entry.
// Scales to fit max_w preserving aspect ratio. Results are cached.
// Mirrors the logic in ReaderScreen::resolve_image_size_().
// ---------------------------------------------------------------------------
// Image dimensions, straight out of the image table.
//
// The table is authoritative: the converter rasterised every image to the size
// it will be drawn at, so there is nothing to scale and nothing to sniff. This
// used to fall back to streaming the embedded JPEG/PNG header to recover
// dimensions the EPUB markup had not declared, and then scale them to the page —
// all of which now happens on the build machine.
inline ImageSizeQuery make_image_size_query(const WgbReader& wgb) {
  const WgbReader* r = &wgb;
  return [r](uint16_t key, uint16_t& w, uint16_t& h) -> bool {
    if (key >= r->image_count())
      return false;
    const auto& ref = r->image_ref(key);
    w = ref.width;
    h = ref.height;
    return w != 0 && h != 0;
  };
}

}  // namespace wintergreen
