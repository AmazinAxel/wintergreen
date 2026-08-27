#include "WgbWriter.h"

#include <cerrno>
#include <cstring>

#ifdef ESP_PLATFORM
#endif

namespace wintergreen {

// ---------------------------------------------------------------------------
// BufferedFileWriter
// ---------------------------------------------------------------------------

bool BufferedFileWriter::open(const char* path) {
  close();
  f_ = fopen(path, "wb");
  if (!f_) {
    return false;
  }
  // Use unbuffered I/O: our BufferedFileWriter already batches writes into
  // a 4KB buffer, so a second stdio layer just wastes heap and can fail to
  // allocate its buffer on a fragmented ESP32 heap.
  setvbuf(f_, nullptr, _IONBF, 0);
  pos_ = 0;
  used_ = 0;
  return true;
}

void BufferedFileWriter::close() {
  if (f_) {
    flush();
    fclose(f_);
    f_ = nullptr;
  }
  pos_ = 0;
  used_ = 0;
}

bool BufferedFileWriter::flush() {
  if (used_ > 0) {
    if (fwrite(buf_, 1, used_, f_) != used_) {
      return false;
    }
    used_ = 0;
  }
  return true;
}

bool BufferedFileWriter::write(const void* data, size_t size) {
  const uint8_t* src = static_cast<const uint8_t*>(data);
  pos_ += static_cast<uint32_t>(size);
  // Fast path: fits in remaining buffer space.
  if (used_ + size <= kBufSize) {
    std::memcpy(buf_ + used_, src, size);
    used_ += size;
    return true;
  }
  // Flush current buffer.
  if (!flush())
    return false;
  // Large write: bypass buffer entirely.
  if (size >= kBufSize)
    return fwrite(src, 1, size, f_) == size;
  // Small write after flush: start fresh buffer.
  std::memcpy(buf_, src, size);
  used_ = size;
  return true;
}

bool BufferedFileWriter::seek(uint32_t offset) {
  if (!flush())
    return false;
  if (fseek(f_, static_cast<long>(offset), SEEK_SET) != 0)
    return false;
  pos_ = offset;
  return true;
}

// ---------------------------------------------------------------------------
// WgbWriter
// ---------------------------------------------------------------------------

bool WgbWriter::open(const char* path) {
  close();
  if (!bw_.open(path))
    return false;


  // Open a single temp file for all paragraph descriptors across the whole book.
  // Reused across chapters; chapter_desc_start_ tracks where each chapter begins.
  std::snprintf(desc_tmp_path_, sizeof(desc_tmp_path_), "%s.desc", path);
  desc_tmp_ = std::fopen(desc_tmp_path_, "w+b");
  chapter_desc_start_ = 0;
  if (!desc_tmp_) {
    close();
    return false;
  }

  // Write placeholder header (will be fixed up in finish()).
  WgbHeader hdr{};
  std::memcpy(hdr.magic, kWgbMagic, 4);
  if (!write_bytes(&hdr, sizeof(hdr))) {
    close();
    return false;
  }
  return true;
}

void WgbWriter::close() {
  bw_.close();
  if (desc_tmp_) {
    std::fclose(desc_tmp_);
    desc_tmp_ = nullptr;
    if (desc_tmp_path_[0])
      std::remove(desc_tmp_path_);
    desc_tmp_path_[0] = '\0';
  }
  chapter_desc_start_ = 0;
  paragraph_count_ = 0;
  chapters_.clear();
  images_.clear();
  image_data_.clear();
  in_chapter_ = false;
  chapter_para_count_ = 0;
  chapter_char_count_ = 0;
}

void WgbWriter::begin_chapter() {
  chapter_para_count_ = 0;
  chapter_char_count_ = 0;
  // Record where this chapter's descriptors start in desc_tmp_.
  if (desc_tmp_) {
    std::fseek(desc_tmp_, 0, SEEK_END);
    chapter_desc_start_ = static_cast<uint32_t>(std::ftell(desc_tmp_));
  }
  in_chapter_ = true;
}

void WgbWriter::end_chapter() {
  if (!in_chapter_)
    return;

  // Write the descriptor table by reading back from desc_tmp_.
  uint32_t table_offset = bw_.tell();
  if (desc_tmp_ && chapter_para_count_ > 0) {
    std::fseek(desc_tmp_, static_cast<long>(chapter_desc_start_), SEEK_SET);
    uint32_t remaining = chapter_para_count_ * 8u;
    uint8_t copy_buf[128];
    while (remaining > 0) {
      size_t want = remaining < sizeof(copy_buf) ? remaining : sizeof(copy_buf);
      size_t n = std::fread(copy_buf, 1, want, desc_tmp_);
      if (n == 0)
        break;
      write_bytes(copy_buf, n);
      remaining -= static_cast<uint32_t>(n);
    }
  }

  WgbChapterEntry entry{};
  entry.para_table_offset = table_offset;
  entry.reserved = 0;
  entry.paragraph_count = chapter_para_count_;
  entry.reserved1 = 0;
  entry.char_count = chapter_char_count_;
  chapters_.push_back(entry);
  in_chapter_ = false;
}

// Total text bytes across a paragraph's runs.
static size_t text_bytes_(const TextParagraph& tp) {
  size_t n = 0;
  for (const auto& r : tp.runs)
    n += r.text.size();
  return n;
}

// Whether there is a <br> to split on, and not on the very last run (splitting
// there would emit a trailing empty paragraph).
//
// Note `breaking` is a *flag on the run the break follows*, not a separate
// run — see EpubParser, which sets it on runs_.back(). So the cut goes after
// run i, and run i keeps its text.
// Whether a run range holds anything worth laying out.
//
// ASCII whitespace only. A UTF-8 space (U+00A0, U+2003, …) has bytes above
// 0x20 and so reads as content here, keeping the chunk — deliberately the safe
// direction: an extra near-empty paragraph is a cosmetic blank line, whereas
// dropping a chunk that did have text loses content.
static bool chunk_has_text_(const Run* runs, size_t count) {
  for (size_t i = 0; i < count; ++i)
    for (char c : runs[i].text)
      if (static_cast<unsigned char>(c) > 0x20)
        return true;
  return false;
}

static bool has_breaking_run_(const TextParagraph& tp) {
  for (size_t i = 0; i + 1 < tp.runs.size(); ++i)
    if (tp.runs[i].breaking)
      return true;
  return false;
}

// Emit one paragraph per <br>-delimited chunk. The break flag sits on the last
// run of each chunk, so the cut is *after* run i and that run keeps its text —
// nothing is dropped. Chunks share the original TextParagraph, so alignment and
// styling carry over, and only the first keeps `spacing`: the rest follow
// immediately, exactly as the lines of the single block did before.
bool WgbWriter::write_split_text_paragraph_(const TextParagraph& tp, uint16_t spacing) {
  size_t start = 0;
  bool first = true;
  for (size_t i = 0; i < tp.runs.size(); ++i) {
    const bool last = (i + 1 == tp.runs.size());
    if (!tp.runs[i].breaking && !last)
      continue;
    const size_t count = i + 1 - start;
    // Skip a chunk with no visible text. <br/> in these files is routinely
    // followed by indentation, so a naive split yields chunks holding only
    // spaces — and a paragraph that lays out to nothing is worse than useless
    // on the device: it collects as a zero-content item, so a page made of them
    // never fills and the reader gets blank pages it cannot page back out of.
    if (count > 0 && chunk_has_text_(tp.runs.data() + start, count)) {
      const uint16_t chunk_spacing = first ? spacing : kWgbSpacingDefault;
      // A <br>-delimited chunk can itself be oversized — the Odyssey's verse
      // quotation has a 1,165-byte stanza, and other books have worse. Recurse
      // into the sentence splitter so one long stanza is not left as a single
      // layout unit just because the paragraph happened to contain a <br>.
      size_t chunk_bytes = 0;
      for (size_t k = 0; k < count; ++k)
        chunk_bytes += tp.runs[start + k].text.size();

      if (chunk_bytes > kSplitParagraphBytes) {
        TextParagraph sub = tp;
        sub.runs.assign(tp.runs.begin() + start, tp.runs.begin() + start + count);
        if (!first)
          sub.indent = 0;
        if (!write_sentence_split_paragraph_(sub, chunk_spacing))
          return false;
      } else if (!write_text_paragraph(tp, chunk_spacing, tp.runs.data() + start, count)) {
        return false;
      }
      first = false;
    }
    start = i + 1;
  }
  return true;
}

// Byte offset just past a sentence end at or before `limit`, or 0 if there is
// none. A cut is only offered after '.', '!', '?' — optionally followed by a
// closing quote or bracket — and then whitespace, so the split always lands in
// the gap between two sentences where a line break was already possible.
//
// Returns an offset into `s`. Never returns an offset that would leave either
// side empty.
static size_t sentence_cut_(const std::string& s, size_t limit) {
  if (limit >= s.size())
    limit = s.size() - 1;
  for (size_t i = limit; i > 0; --i) {
    if (!(static_cast<unsigned char>(s[i]) <= 0x20))
      continue;  // cut point must be followed by whitespace
    // Walk back over any closing quotes/brackets to reach the punctuation.
    size_t j = i;
    while (j > 0) {
      const unsigned char c = static_cast<unsigned char>(s[j - 1]);
      // ASCII closers, plus the lead bytes of the curly quotes Gutenberg uses.
      if (c == '"' || c == '\'' || c == ')' || c == ']' || c >= 0x80) {
        --j;
        continue;
      }
      break;
    }
    if (j == 0)
      continue;
    const char p = s[j - 1];
    if (p == '.' || p == '!' || p == '?') {
      // Skip the whitespace so the next chunk starts on a word.
      size_t k = i;
      while (k < s.size() && static_cast<unsigned char>(s[k]) <= 0x20)
        ++k;
      if (k > 0 && k < s.size())
        return k;
    }
  }
  return 0;
}

// Whether sentence_cut_ would find a boundary in `s[off..off+room]`. Used to
// decide whether the current chunk should be flushed *before* a run is
// considered, so the run gets a full window to cut in rather than a scrap.
// Takes the string by reference and an offset so the tail is not copied just to
// answer the question.
static bool sentence_cut_fits_(const std::string& s, size_t off, size_t room) {
  if (off >= s.size() || room == 0)
    return false;
  const size_t avail = s.size() - off;
  size_t limit = room < avail ? room : avail - 1;
  for (size_t i = limit; i > 0; --i) {
    const size_t abs = off + i;
    if (!(static_cast<unsigned char>(s[abs]) <= 0x20))
      continue;
    size_t j = abs;
    while (j > off) {
      const unsigned char c = static_cast<unsigned char>(s[j - 1]);
      if (c == '"' || c == '\'' || c == ')' || c == ']' || c >= 0x80) {
        --j;
        continue;
      }
      break;
    }
    if (j == off)
      continue;
    const char p = s[j - 1];
    if (p == '.' || p == '!' || p == '?')
      return true;
  }
  return false;
}

// Last resort for an oversized paragraph with no <br> to split on: cut it at
// sentence boundaries instead.
//
// **This is the case that aborts the device**, and it is not hypothetical: the
// Odyssey's chapter 2 (PREFACE TO FIRST EDITION) holds a 3,672-byte paragraph of
// running prose. A paragraph is the indivisible unit of layout — TextLayout
// holds a LayoutWord per word and a vector per line, all resident together — and
// that one needs ~15 KB against roughly 13 KB free with the BLE clicker up. The
// allocation that failed on hardware was a 256-byte reserve; the heap was simply
// gone.
//
// Why this is invisible in the rendered result: the cut lands between two
// sentences, exactly where a line break could already fall, and every chunk
// after the first is written with `kWgbSpacingDefault` and the paragraph's own
// alignment, so no extra gap and no fresh indent appears. The one thing that
// *would* show is a first-line indent restarting mid-paragraph, which is why
// continuation chunks clear `indent`.
//
// A cut is only taken when a sentence boundary exists in the window; prose
// without one (a single enormous sentence) is left whole rather than broken
// mid-clause, since that would be visible. Such paragraphs do not occur in the
// sample, and leaving them intact keeps this transformation lossless.
bool WgbWriter::write_sentence_split_paragraph_(const TextParagraph& tp, uint16_t spacing) {
  // Only the text of the runs matters for the cut; styling is carried by
  // re-emitting the run with a shorter string, so runs keep their own style.
  std::vector<Run> pending;
  size_t pending_bytes = 0;
  bool first = true;
  bool wrote_any = false;

  auto flush = [&]() -> bool {
    if (pending.empty() || !chunk_has_text_(pending.data(), pending.size())) {
      pending.clear();
      pending_bytes = 0;
      return true;
    }
    TextParagraph chunk = tp;
    if (!first)
      chunk.indent = 0;  // a restarted first-line indent would be visible
    if (!write_text_paragraph(chunk, first ? spacing : kWgbSpacingDefault, pending.data(), pending.size()))
      return false;
    first = false;
    wrote_any = true;
    pending.clear();
    pending_bytes = 0;
    return true;
  };

  for (const auto& run : tp.runs) {
    size_t off = 0;
    while (off < run.text.size()) {
      // Flush before this run when the chunk is full, *or* when what is left of
      // the window is too small to cut in. Both leave `sentence_cut_` with no
      // usable boundary, and its answer to that is "take the whole remainder" —
      // which appends an entire run to an already-full chunk.
      //
      // That is what left 25 paragraphs over the limit in the Odyssey after a
      // re-convert. Chapter 22's paragraph 31 is three runs of 658 + 5 + 848
      // bytes: the first two filled the chunk to 663, leaving 37 bytes of room,
      // so the 848-byte run could not be cut and went in whole — 1,511 bytes out
      // of a 700-byte limit. Flushing first gives that run the full window.
      const size_t remaining_in_run = run.text.size() - off;
      if (pending_bytes > 0 && remaining_in_run > kSplitParagraphBytes - pending_bytes &&
          !sentence_cut_fits_(run.text, off, kSplitParagraphBytes - pending_bytes)) {
        if (!flush())
          return false;
      }

      const size_t room = kSplitParagraphBytes - pending_bytes;
      const size_t remaining = run.text.size() - off;

      if (remaining <= room) {
        Run part = run;
        part.text = run.text.substr(off);
        pending_bytes += part.text.size();
        pending.push_back(std::move(part));
        break;
      }

      // Need a cut inside this run. Look for a sentence end within the room we
      // have; if there is none, take the whole remainder rather than cutting
      // mid-sentence — an oversized paragraph is better than a visible break.
      const std::string tail = run.text.substr(off);
      const size_t cut = sentence_cut_(tail, room);
      if (cut == 0) {
        Run part = run;
        part.text = tail;
        pending_bytes += part.text.size();
        pending.push_back(std::move(part));
        break;
      }

      Run part = run;
      part.text = tail.substr(0, cut);
      pending.push_back(std::move(part));
      if (!flush())
        return false;
      off += cut;
    }
  }

  if (!flush())
    return false;
  // A paragraph that produced nothing (all whitespace) still has to reach the
  // file, or chapter paragraph indices shift under the TOC.
  if (!wrote_any)
    return write_text_paragraph(tp, spacing, tp.runs.data(), tp.runs.size());
  return true;
}

bool WgbWriter::write_paragraph(const Paragraph& para) {
  if (!bw_.is_open())
    return false;

  // Text paragraphs are handled entirely by write_text_paragraph (descriptor + counters included).
  if (para.type == ParagraphType::Text) {
    uint16_t spacing = para.spacing_before.value_or(kWgbSpacingDefault);

    // **Split very large paragraphs at <br> boundaries.**
    //
    // A paragraph is the indivisible unit of layout on the device: TextLayout
    // lays out all of it at once and holds a LayoutWord per word plus a vector
    // per line, all resident together. That is fine for prose, where the
    // largest paragraph in an ordinary book is ~2,300 characters, but Project
    // Gutenberg's Odyssey carries a 4,335-character `<p class="letter">` that
    // packs several quoted stanzas into one paragraph separated by <br/>. On a
    // device whose free heap is measured in tens of KB that single unit is
    // enough to abort the reader, reproducibly, at the same point in the book.
    //
    // A <br> is already a hard line break, so breaking the paragraph there
    // changes nothing about the rendered result — the only difference is that
    // the device lays out and holds one stanza at a time. Splitting anywhere
    // else would be visible (it would insert a paragraph break mid-sentence),
    // which is why an oversized paragraph with no <br> is left alone: there is
    // no lossless cut, and those are rare in practice.
    const size_t total = text_bytes_(para.text);
    if (total > kSplitParagraphBytes) {
      if (has_breaking_run_(para.text))
        return write_split_text_paragraph_(para.text, spacing);
      // No <br> to cut on. Fall back to sentence boundaries — the Odyssey's
      // first preface is running prose in one 3,672-byte paragraph, and that
      // single unit is what aborts the reader when the clicker holds the heap.
      return write_sentence_split_paragraph_(para.text, spacing);
    }

    return write_text_paragraph(para.text, spacing, para.text.runs.data(), para.text.runs.size());
  }

  // Record descriptor for non-text paragraphs: append {file_offset, char_offset} to desc_tmp_.
  if (desc_tmp_) {
    uint8_t desc[8];
    wgb_write_u32(desc, bw_.tell());
    wgb_write_u32(desc + 4, chapter_char_count_);
    std::fwrite(desc, 1, 8, desc_tmp_);
  }

  // Serialize and write: [type(1)][data_size(4)][data...] — no link header.
  switch (para.type) {
    case ParagraphType::Text:
      break;  // unreachable — handled above
    case ParagraphType::Image: {
      uint8_t buf[9];
      buf[0] = kWgbParaImage;
      wgb_write_u32(buf + 1, 4);
      wgb_write_u16(buf + 5, para.image.key);
      wgb_write_u16(buf + 7, para.spacing_before.value_or(kWgbSpacingDefault));
      if (!write_bytes(buf, 9))
        return false;
      break;
    }
    case ParagraphType::Hr: {
      uint8_t buf[8];
      buf[0] = kWgbParaHr;
      wgb_write_u32(buf + 1, 3);
      wgb_write_u16(buf + 5, para.spacing_before.value_or(kWgbSpacingDefault));
      buf[7] = para.hr_width_pct.value_or(kWgbHrWidthDefault);
      if (!write_bytes(buf, 8))
        return false;
      break;
    }
    case ParagraphType::PageBreak: {
      uint8_t buf[5];
      buf[0] = kWgbParaPageBreak;
      wgb_write_u32(buf + 1, 0);
      if (!write_bytes(buf, 5))
        return false;
      break;
    }
  }

  ++chapter_para_count_;
  ++paragraph_count_;
  return true;
}

uint16_t WgbWriter::add_image_ref(uint32_t local_header_offset, uint16_t width, uint16_t height) {
  uint16_t idx = static_cast<uint16_t>(images_.size());
  WgbImageRef ref{};
  (void)local_header_offset;  // v12: the EPUB offset is no longer persisted
  ref.width = width;
  ref.height = height;
  images_.push_back(ref);
  image_data_.emplace_back();
  return idx;
}

void WgbWriter::set_image_data(uint16_t idx, std::vector<uint8_t>&& data) {
  if (idx < image_data_.size())
    image_data_[idx] = std::move(data);
}

void WgbWriter::update_image_size(uint16_t idx, uint16_t width, uint16_t height) {
  if (idx < images_.size()) {
    images_[idx].width = width;
    images_[idx].height = height;
  }
}

bool WgbWriter::finish(const EpubMetadata& meta, const TableOfContents& toc) {
  if (!bw_.is_open())
    return false;

  // Close any open chapter.
  if (in_chapter_)
    end_chapter();

  // --- Write chapter table (16 bytes each) ---
  uint32_t chapter_offset = bw_.tell();
  for (const auto& ch : chapters_) {
    uint8_t buf[16];
    wgb_write_u32(buf, ch.para_table_offset);
    wgb_write_u32(buf + 4, ch.reserved);
    wgb_write_u16(buf + 8, ch.paragraph_count);
    wgb_write_u16(buf + 10, 0);
    wgb_write_u32(buf + 12, ch.char_count);
    if (!write_bytes(buf, 16))
      return false;
  }

  // --- Write image blob, then the ref table that points into it ---
  // Blob first so the refs can carry final offsets. The images are contiguous
  // and in index order, and stored verbatim: the device seeks straight to the
  // bytes with no inflate and no ZIP walk.
  for (size_t i = 0; i < images_.size(); ++i) {
    if (image_data_[i].empty()) {
      images_[i].data_offset = 0;
      images_[i].data_size = 0;
      continue;
    }
    images_[i].data_offset = bw_.tell();
    images_[i].data_size = static_cast<uint32_t>(image_data_[i].size());
    if (!write_bytes(image_data_[i].data(), image_data_[i].size()))
      return false;
    // Free as we go — a book can carry several MB of images and the ESP32
    // conversion path has no room to hold them all at once.
    std::vector<uint8_t>().swap(image_data_[i]);
  }

  uint32_t image_offset = bw_.tell();
  for (const auto& img : images_) {
    uint8_t buf[12];
    wgb_write_u32(buf, img.data_offset);
    wgb_write_u32(buf + 4, img.data_size);
    wgb_write_u16(buf + 8, img.width);
    wgb_write_u16(buf + 10, img.height);
    if (!write_bytes(buf, 12))
      return false;
  }

  // --- Write metadata blob ---
  uint32_t meta_offset = bw_.tell();
  write_string(meta.title);
  write_string(meta.author.value_or(""));
  write_string(meta.language.value_or(""));

  // --- Write TOC ---
  uint16_t toc_count = static_cast<uint16_t>(toc.entries.size());
  uint8_t toc_hdr[2];
  wgb_write_u16(toc_hdr, toc_count);
  write_bytes(toc_hdr, 2);
  for (const auto& entry : toc.entries) {
    auto lbl = entry.label.to_string(toc.pool);
    // Defensive: ensure label fits u16 length field
    if (lbl.size() > 0xFFFF) {
      return false;
    }
    if (!write_string(lbl)) {
      int e = errno;
      return false;
    }
    uint8_t buf[5];
    wgb_write_u16(buf, entry.file_idx);
    buf[2] = entry.depth;
    wgb_write_u16(buf + 3, entry.para_index);
    write_bytes(buf, 5);
  }


  // --- Fix up header ---
  WgbHeader hdr{};
  std::memcpy(hdr.magic, kWgbMagic, 4);
  hdr.paragraph_count = paragraph_count_;
  hdr.chapter_count = static_cast<uint16_t>(chapters_.size());
  hdr.image_count = static_cast<uint16_t>(images_.size());
  hdr.chapter_offset = chapter_offset;
  hdr.image_offset = image_offset;
  hdr.meta_offset = meta_offset;

  bw_.seek(0);
  if (!write_bytes(&hdr, sizeof(hdr)))
    return false;
  bw_.close();

  return true;
}

// ---------------------------------------------------------------------------
// write_text_paragraph  (streams directly to bw_ — no intermediate buffer)
// ---------------------------------------------------------------------------

bool WgbWriter::write_text_paragraph(const TextParagraph& meta, uint16_t spacing, const Run* runs, size_t run_count) {
  if (!bw_.is_open())
    return false;

  // Record descriptor.
  if (desc_tmp_) {
    uint8_t desc[8];
    wgb_write_u32(desc, bw_.tell());
    wgb_write_u32(desc + 4, chapter_char_count_);
    std::fwrite(desc, 1, 8, desc_tmp_);
  }

  // Count chars.
  for (size_t i = 0; i < run_count; ++i)
    chapter_char_count_ += static_cast<uint32_t>(runs[i].text.size());

  // Compute body size (pure arithmetic, no allocation).
  static constexpr size_t kBodyHdrSize = 18;  // alignment(1)+indent(2)+ml(2)+mr(2)+spacing(2)+lh(1)+img(6)+runcount(2)
  static constexpr size_t kRunHdrSize = 12;   // style(1)+size(1)+valign(1)+flags(1)+ml(2)+mr(2)+textlen(4)
  size_t body_size = kBodyHdrSize;
  for (size_t i = 0; i < run_count; ++i) {
    body_size += kRunHdrSize + runs[i].text.size();
  }

  // Write outer header: [type(1)][body_size(4)]
  uint8_t outer[5];
  outer[0] = kWgbParaText;
  wgb_write_u32(outer + 1, static_cast<uint32_t>(body_size));
  if (!write_bytes(outer, 5))
    return false;

  // Write paragraph body header (18 bytes) from stack.
  uint8_t hdr[18];
  uint8_t* p = hdr;
  *p++ = meta.alignment.has_value() ? static_cast<uint8_t>(*meta.alignment) : kWgbAlignDefault;
  wgb_write_i16(p, meta.indent.value_or(kWgbIndentNone));
  p += 2;
  wgb_write_u16(p, 0);
  p += 2;  // margin_left placeholder
  wgb_write_u16(p, 0);
  p += 2;  // margin_right placeholder
  wgb_write_u16(p, spacing);
  p += 2;
  *p++ = meta.line_height_pct;
  if (meta.inline_image.has_value()) {
    wgb_write_u16(p, meta.inline_image->key);
    p += 2;
    wgb_write_u16(p, meta.inline_image->attr_width);
    p += 2;
    wgb_write_u16(p, meta.inline_image->attr_height);
    p += 2;
  } else {
    wgb_write_u16(p, kWgbNoImage);
    p += 2;
    wgb_write_u16(p, 0);
    p += 2;
    wgb_write_u16(p, 0);
    p += 2;
  }
  wgb_write_u16(p, static_cast<uint16_t>(run_count));
  p += 2;
  if (!write_bytes(hdr, 18))
    return false;

  // Write each run using a 12-byte stack header.
  for (size_t i = 0; i < run_count; ++i) {
    const Run& run = runs[i];
    uint8_t rhdr[12];
    rhdr[0] = static_cast<uint8_t>(run.style);
    rhdr[1] = run.size_pct;
    rhdr[2] = static_cast<uint8_t>(run.vertical_align);
    // Bit 0x02 (href present) is never set: links are not parsed.
    rhdr[3] = run.breaking ? 0x01 : 0x00;
    wgb_write_u16(rhdr + 4, run.margin_left);
    wgb_write_u16(rhdr + 6, run.margin_right);
    wgb_write_u32(rhdr + 8, static_cast<uint32_t>(run.text.size()));
    if (!write_bytes(rhdr, 12))
      return false;
    if (!run.text.empty() && !write_bytes(run.text.data(), run.text.size()))
      return false;
  }

  ++chapter_para_count_;
  ++paragraph_count_;
  return true;
}

// ---------------------------------------------------------------------------
// I/O helpers
// ---------------------------------------------------------------------------

bool WgbWriter::write_bytes(const void* data, size_t size) {
  return bw_.write(data, size);
}

bool WgbWriter::write_string(const std::string& s) {
  uint8_t len_buf[2];
  wgb_write_u16(len_buf, static_cast<uint16_t>(s.size()));
  if (!write_bytes(len_buf, 2))
    return false;
  if (!s.empty() && !write_bytes(s.data(), s.size()))
    return false;
  return true;
}

}  // namespace wintergreen
