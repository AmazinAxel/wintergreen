#include "WgbReader.h"

#include <cstring>

namespace wintergreen {

bool WgbReader::open(const char* path) {
  close();
  f_ = fopen(path, "rb");
  if (!f_)
    return false;

  if (!read_bytes(&header_, sizeof(header_))) {
    close();
    return false;
  }
  // Stands in for a magic: the image ref table always ends at the metadata
  // blob, the page tables follow the metadata, and the last page's display list
  // ends exactly at the end of the file (the tables do, when there are no pages:
  // the bake's intermediate file).
  uint64_t tables_end = header_.pages_offset;
  int last_layout = -1;
  for (uint8_t l = 0; l < kWgbLayouts; ++l) {
    tables_end += static_cast<uint64_t>(header_.chapter_count) * 4u +
                  static_cast<uint64_t>(header_.page_count[l]) * kWgbPageSize;
    if (header_.page_count[l] > 0)
      last_layout = l;
  }
  long file_size = -1;
  if (fseek(f_, 0, SEEK_END) == 0)
    file_size = ftell(f_);
  bool ok = header_.image_count <= 4096 && header_.image_offset >= kWgbFirstParagraph &&
            header_.meta_offset == header_.image_offset + header_.image_count * 12u &&
            header_.pages_offset >= header_.meta_offset && file_size >= 0;
  if (ok && last_layout < 0) {
    ok = tables_end == static_cast<uint64_t>(file_size);
  } else if (ok) {
    WgbPage last;
    ok = page(static_cast<uint8_t>(last_layout), header_.page_count[last_layout] - 1, last) &&
         last.dl_offset >= tables_end &&
         static_cast<uint64_t>(last.dl_offset) + last.dl_size == static_cast<uint64_t>(file_size);
  }
  if (!ok) {
    close();
    return false;
  }

  fseek(f_, static_cast<long>(header_.meta_offset), SEEK_SET);
  metadata_.title = read_string();
  std::string author = read_string();
  if (!author.empty())
    metadata_.author = std::move(author);
  std::string lang = read_string();
  if (!lang.empty())
    metadata_.language = std::move(lang);

  return true;
}

void WgbReader::close() {
  if (f_) {
    fclose(f_);
    f_ = nullptr;
  }
  header_ = {};
  metadata_ = {};
}

uint32_t WgbReader::layout_base_(uint8_t layout) const {
  uint32_t base = header_.pages_offset;
  for (uint8_t l = 0; l < layout; ++l)
    base += static_cast<uint32_t>(header_.chapter_count) * 4u + header_.page_count[l] * kWgbPageSize;
  return base;
}

bool WgbReader::read_at_(uint32_t at, void* buf, size_t size) {
  return f_ && fseek(f_, static_cast<long>(at), SEEK_SET) == 0 && read_bytes(buf, size);
}

bool WgbReader::page(uint8_t layout, uint32_t index, WgbPage& out) {
  if (!f_ || layout >= kWgbLayouts || index >= header_.page_count[layout])
    return false;
  uint8_t b[kWgbPageSize];
  if (!read_at_(layout_base_(layout) + header_.chapter_count * 4u + index * kWgbPageSize, b, sizeof(b)))
    return false;
  out.para_offset = wgb_read_u32(b);
  out.offset = wgb_read_u16(b + 4);
  out.text_offset = wgb_read_u16(b + 6);
  out.dl_offset = wgb_read_u32(b + 8);
  out.dl_size = wgb_read_u16(b + 12);
  return true;
}

bool WgbReader::read_page(const WgbPage& rec, uint8_t* buf, size_t cap) {
  return rec.dl_size <= cap && read_at_(rec.dl_offset, buf, rec.dl_size);
}

uint32_t WgbReader::find_page(uint8_t layout, uint32_t para_offset, uint16_t text_offset) {
  // Last page whose start is at or before the position: lo is always such a
  // page (or 0), hi always past one.
  uint32_t lo = 0, hi = page_count(layout);
  while (hi - lo > 1) {
    const uint32_t mid = lo + (hi - lo) / 2;
    WgbPage p;
    if (!page(layout, mid, p))
      return lo;
    if (p.para_offset < para_offset || (p.para_offset == para_offset && p.text_offset <= text_offset))
      lo = mid;
    else
      hi = mid;
  }
  return lo;
}

bool WgbReader::chapter_pages(uint8_t layout, std::vector<uint32_t>& out) {
  out.clear();
  if (!f_ || layout >= kWgbLayouts || header_.chapter_count == 0)
    return f_ != nullptr;
  if (fseek(f_, static_cast<long>(layout_base_(layout)), SEEK_SET) != 0)
    return false;
  out.resize(header_.chapter_count);
  for (uint16_t i = 0; i < header_.chapter_count; ++i) {
    uint8_t b[4];
    if (!read_bytes(b, sizeof(b))) {
      out.clear();
      return false;
    }
    out[i] = wgb_read_u32(b);
  }
  return true;
}

bool WgbReader::image_ref(uint16_t index, WgbImageRef& out) {
  uint8_t b[12];
  if (index >= header_.image_count || !read_at_(header_.image_offset + index * 12u, b, sizeof(b)))
    return false;
  out.data_offset = wgb_read_u32(b);
  out.data_size = wgb_read_u32(b + 4);
  out.width = wgb_read_u16(b + 8);
  out.height = wgb_read_u16(b + 10);
  return true;
}

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
