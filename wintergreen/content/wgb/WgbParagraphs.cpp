#include "WgbParagraphs.h"

namespace wintergreen {

WgbParagraphs::WgbParagraphs(WgbReader& reader) : reader_(reader) {
  images_.resize(reader.image_count());
  for (uint16_t i = 0; i < reader.image_count(); ++i)
    if (!reader.image_ref(i, images_[i]))
      images_[i] = WgbImageRef{};
}

bool WgbParagraphs::load_paragraph(uint32_t file_offset, Paragraph& out, uint32_t* next_offset) {
  FILE* f = reader_.file();
  if (!f || file_offset < kWgbFirstParagraph || std::fseek(f, static_cast<long>(file_offset), SEEK_SET) != 0)
    return false;

  uint8_t hdr[kWgbParaHeaderSize];
  if (std::fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
    return false;
  const uint8_t type = hdr[0];
  const uint32_t data_size = wgb_read_u32(hdr + 1);
  if (data_size > 65536)
    return false;

  body_.resize(data_size);
  const bool ok =
      (data_size == 0 || std::fread(body_.data(), 1, data_size, f) == data_size) && parse_paragraph_(type, out);
  if (ok && next_offset)
    *next_offset = file_offset + static_cast<uint32_t>(kWgbParaHeaderSize) + data_size;
  return ok;
}

bool WgbParagraphs::parse_paragraph_(uint8_t type, Paragraph& out) {
  const size_t data_size = body_.size();
  const uint8_t* body = body_.data();
  switch (type) {
    case kWgbParaText:
      return deserialize_text(body, data_size, out);

    case kWgbParaImage: {
      out = Paragraph{};
      out.type = ParagraphType::Image;
      if (data_size >= 4) {
        const uint16_t key = wgb_read_u16(body);
        uint16_t w = 0, h = 0;
        if (key < images_.size()) {
          w = images_[key].width;
          h = images_[key].height;
        }
        out.image = ImageRef{key, w, h};
        const uint16_t sp = wgb_read_u16(body + 2);
        if (sp != kWgbSpacingDefault)
          out.spacing_before = sp;
      }
      return true;
    }

    case kWgbParaHr: {
      out = Paragraph::make_hr();
      if (data_size >= 2) {
        const uint16_t sp = wgb_read_u16(body);
        if (sp != kWgbSpacingDefault)
          out.spacing_before = sp;
      }
      if (data_size >= 3) {
        const uint8_t w = wgb_read_u8(body + 2);
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

bool WgbParagraphs::deserialize_text(const uint8_t* data, size_t size, Paragraph& out) {
  out = Paragraph{};
  out.type = ParagraphType::Text;

  if (size < 14)  // minimum: header fields + run_count
    return false;

  size_t pos = 0;

  const uint8_t align_val = data[pos++];
  if (align_val != kWgbAlignDefault)
    out.text.alignment = static_cast<Alignment>(align_val);

  const int16_t indent_val = wgb_read_i16(data + pos);
  pos += 2;
  if (indent_val != kWgbIndentNone)
    out.text.indent = indent_val;

  const uint16_t spacing = wgb_read_u16(data + pos);
  pos += 2;
  if (spacing != kWgbSpacingDefault)
    out.spacing_before = spacing;

  out.text.line_height_pct = data[pos++];

  const uint16_t img_key = wgb_read_u16(data + pos);
  pos += 2;
  uint16_t img_w = wgb_read_u16(data + pos);
  pos += 2;
  uint16_t img_h = wgb_read_u16(data + pos);
  pos += 2;
  if (img_key != kWgbNoImage) {
    // Fall back to the image ref table if inline dimensions are unknown.
    if ((img_w == 0 || img_h == 0) && img_key < images_.size()) {
      img_w = images_[img_key].width;
      img_h = images_[img_key].height;
    }
    out.text.inline_image = ImageRef{img_key, img_w, img_h};
  }

  if (pos + 2 > size)
    return false;
  const uint16_t run_count = wgb_read_u16(data + pos);
  pos += 2;

  out.text.runs.resize(run_count);
  for (uint16_t i = 0; i < run_count; ++i) {
    if (pos + 12 > size)
      return false;

    Run& run = out.text.runs[i];
    run.style = static_cast<FontStyle>(data[pos++]);
    run.size_pct = data[pos++];
    run.vertical_align = static_cast<VerticalAlign>(data[pos++]);
    const uint8_t flags = data[pos++];
    run.breaking = (flags & 0x01) != 0;

    run.margin_left = wgb_read_u16(data + pos);
    pos += 2;
    run.margin_right = wgb_read_u16(data + pos);
    pos += 2;

    const uint32_t text_len = wgb_read_u32(data + pos);
    pos += 4;

    if (pos + text_len > size)
      return false;
    run.text.assign(reinterpret_cast<const char*>(data + pos), text_len);
    pos += text_len;
  }

  return true;
}

}  // namespace wintergreen
