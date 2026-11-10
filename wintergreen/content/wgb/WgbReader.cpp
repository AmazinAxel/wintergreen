#include "WgbReader.h"

#include <cstring>
#include <vector>

namespace wintergreen {

bool WgbReader::open(const char* path) {
  close();
  f_ = fopen(path, "rb");
  if (!f_)
    return false;

  // Read header
  if (!read_bytes(&header_, sizeof(header_))) {
    close();
    return false;
  }
  // Stands in for a magic: the image ref table always ends at the metadata blob.
  if (header_.chapter_count > 2048 || header_.image_count > 4096 || header_.chapter_offset < sizeof(WgbHeader) ||
      header_.image_offset < header_.chapter_offset + header_.chapter_count * kWgbChapterEntrySize ||
      header_.meta_offset != header_.image_offset + header_.image_count * 12u) {
    close();
    return false;
  }
  // Both tables are read in a single fread each and parsed from memory. One
  // fread per entry costs a FATFS+SPI round trip apiece, and a long book has
  // hundreds of chapters — this is the bulk of open() time.
  chapters_.resize(header_.chapter_count);
  if (header_.chapter_count > 0) {
    std::vector<uint8_t> raw(static_cast<size_t>(header_.chapter_count) * kWgbChapterEntrySize);
    fseek(f_, static_cast<long>(header_.chapter_offset), SEEK_SET);
    if (!read_bytes(raw.data(), raw.size())) {
      close();
      return false;
    }
    for (uint16_t i = 0; i < header_.chapter_count; ++i) {
      const uint8_t* b = raw.data() + static_cast<size_t>(i) * kWgbChapterEntrySize;
      chapters_[i].para_table_offset = wgb_read_u32(b);
      chapters_[i].paragraph_count = wgb_read_u16(b + 4);
      chapters_[i].char_count = wgb_read_u32(b + 6);
    }
  }

  // Read image ref table
  images_.resize(header_.image_count);
  if (header_.image_count > 0) {
    std::vector<uint8_t> raw(static_cast<size_t>(header_.image_count) * 12);
    fseek(f_, static_cast<long>(header_.image_offset), SEEK_SET);
    if (!read_bytes(raw.data(), raw.size())) {
      close();
      return false;
    }
    for (uint16_t i = 0; i < header_.image_count; ++i) {
      const uint8_t* b = raw.data() + static_cast<size_t>(i) * 12;
      images_[i].data_offset = wgb_read_u32(b);
      images_[i].data_size = wgb_read_u32(b + 4);
      images_[i].width = wgb_read_u16(b + 8);
      images_[i].height = wgb_read_u16(b + 10);
    }
  }

  // Read metadata
  fseek(f_, static_cast<long>(header_.meta_offset), SEEK_SET);
  metadata_.title = read_string();
  std::string author = read_string();
  if (!author.empty())
    metadata_.author = std::move(author);
  std::string lang = read_string();
  if (!lang.empty())
    metadata_.language = std::move(lang);

  // Read TOC
  uint8_t toc_hdr[2];
  if (read_bytes(toc_hdr, 2)) {
    uint16_t toc_count = wgb_read_u16(toc_hdr);
    toc_.entries.reserve(toc_count);
    for (uint16_t i = 0; i < toc_count; ++i) {
      std::string label = read_string();
      uint8_t buf[5] = {};
      read_bytes(buf, 5);
      uint16_t file_idx = wgb_read_u16(buf);
      uint8_t depth = buf[2];
      uint16_t para_index = wgb_read_u16(buf + 3);
      toc_.add_entry(label, file_idx, depth, {}, para_index);
    }
  }

  return true;
}

void WgbReader::close() {
  if (f_) {
    fclose(f_);
    f_ = nullptr;
  }
  chapters_.clear();
  chapters_.shrink_to_fit();
  images_.clear();
  images_.shrink_to_fit();
  std::vector<uint8_t>().swap(body_);
  header_ = {};
  metadata_ = {};
  toc_ = {};
}

uint32_t WgbReader::chapter_para_table_offset(uint16_t chapter_idx) const {
  if (chapter_idx >= chapters_.size())
    return 0;
  return chapters_[chapter_idx].para_table_offset;
}

uint16_t WgbReader::chapter_paragraph_count(uint16_t chapter_idx) const {
  if (chapter_idx >= chapters_.size())
    return 0;
  return chapters_[chapter_idx].paragraph_count;
}

uint32_t WgbReader::chapter_char_count(uint16_t chapter_idx) const {
  if (chapter_idx >= chapters_.size())
    return 0;
  return chapters_[chapter_idx].char_count;
}

uint64_t WgbReader::total_char_count() const {
  uint64_t total = 0;
  for (const auto& ch : chapters_)
    total += ch.char_count;
  return total;
}

bool WgbReader::load_paragraph(uint32_t file_offset, Paragraph& out) {
  if (!f_ || file_offset == 0)
    return false;

  fseek(f_, static_cast<long>(file_offset), SEEK_SET);

  // Read type + data_size (5 bytes).
  uint8_t hdr[5];
  if (!read_bytes(hdr, 5))
    return false;

  uint8_t type = hdr[0];
  uint32_t data_size = wgb_read_u32(hdr + 1);
  if (data_size > 65536)
    return false;

  body_.resize(data_size);
  const bool ok = (data_size == 0 || read_bytes(body_.data(), data_size)) && parse_paragraph_(type, out);
  if (body_.capacity() > kBodyKeepBytes)
    std::vector<uint8_t>().swap(body_);
  return ok;
}

bool WgbReader::parse_paragraph_(uint8_t type, Paragraph& out) {
  const std::vector<uint8_t>& body = body_;
  const size_t data_size = body.size();
  switch (type) {
    case kWgbParaText:
      return deserialize_text(body.data(), body.size(), out);

    case kWgbParaImage: {
      out = Paragraph{};
      out.type = ParagraphType::Image;
      if (data_size >= 4) {
        uint16_t key = wgb_read_u16(body.data());
        // Look up dimensions from image ref table
        uint16_t w = 0, h = 0;
        if (key < images_.size()) {
          w = images_[key].width;
          h = images_[key].height;
        }
        out.image = ImageRef{key, w, h};
        uint16_t sp = wgb_read_u16(body.data() + 2);
        if (sp != kWgbSpacingDefault)
          out.spacing_before = sp;
      }
      return true;
    }

    case kWgbParaHr: {
      out = Paragraph::make_hr();
      if (data_size >= 2) {
        uint16_t sp = wgb_read_u16(body.data());
        if (sp != kWgbSpacingDefault)
          out.spacing_before = sp;
      }
      if (data_size >= 3) {
        uint8_t w = wgb_read_u8(body.data() + 2);
        if (w != kWgbHrWidthDefault)
          out.hr_width_pct = w;
      }
      return true;
    }

    case kWgbParaPageBreak:
      out = Paragraph::make_page_break();
      return true;

    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Text paragraph deserialization
// ---------------------------------------------------------------------------

bool WgbReader::deserialize_text(const uint8_t* data, size_t size, Paragraph& out) {
  out = Paragraph{};
  out.type = ParagraphType::Text;

  if (size < 14)  // minimum: header fields + run_count
    return false;

  size_t pos = 0;

  // alignment
  uint8_t align_val = data[pos++];
  if (align_val != kWgbAlignDefault)
    out.text.alignment = static_cast<Alignment>(align_val);

  // indent
  int16_t indent_val = wgb_read_i16(data + pos);
  pos += 2;
  if (indent_val != kWgbIndentNone)
    out.text.indent = indent_val;

  // spacing_before
  uint16_t spacing = wgb_read_u16(data + pos);
  pos += 2;
  if (spacing != kWgbSpacingDefault)
    out.spacing_before = spacing;

  // line_height_pct
  out.text.line_height_pct = data[pos++];

  // inline image
  uint16_t img_key = wgb_read_u16(data + pos);
  pos += 2;
  uint16_t img_w = wgb_read_u16(data + pos);
  pos += 2;
  uint16_t img_h = wgb_read_u16(data + pos);
  pos += 2;
  if (img_key != kWgbNoImage) {
    // Fall back to image ref table if inline dimensions are unknown.
    if ((img_w == 0 || img_h == 0) && img_key < images_.size()) {
      img_w = images_[img_key].width;
      img_h = images_[img_key].height;
    }
    out.text.inline_image = ImageRef{img_key, img_w, img_h};
  }

  // run count
  if (pos + 2 > size)
    return false;
  uint16_t run_count = wgb_read_u16(data + pos);
  pos += 2;

  out.text.runs.resize(run_count);
  for (uint16_t i = 0; i < run_count; ++i) {
    if (pos + 12 > size)
      return false;

    Run& run = out.text.runs[i];
    run.style = static_cast<FontStyle>(data[pos++]);
    run.size_pct = data[pos++];
    run.vertical_align = static_cast<VerticalAlign>(data[pos++]);
    uint8_t flags = data[pos++];
    run.breaking = (flags & 0x01) != 0;

    run.margin_left = wgb_read_u16(data + pos);
    pos += 2;
    run.margin_right = wgb_read_u16(data + pos);
    pos += 2;

    uint32_t text_len = wgb_read_u32(data + pos);
    pos += 4;

    if (pos + text_len > size)
      return false;
    run.text.assign(reinterpret_cast<const char*>(data + pos), text_len);
    pos += text_len;
  }

  return true;
}

// ---------------------------------------------------------------------------
// I/O helpers
// ---------------------------------------------------------------------------

bool WgbReader::read_bytes(void* buf, size_t size) {
  return fread(buf, 1, size, f_) == size;
}

std::string WgbReader::read_string() {
  uint8_t len_buf[2];
  if (!read_bytes(len_buf, 2))
    return {};
  uint16_t len = wgb_read_u16(len_buf);
  if (len == 0)
    return {};
  std::string s(len, '\0');
  if (!read_bytes(s.data(), len))
    return {};
  return s;
}

}  // namespace wintergreen
