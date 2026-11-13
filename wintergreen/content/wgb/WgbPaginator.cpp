#include "WgbPaginator.h"

#include <cstdio>

#include "../PageGeometry.h"
#include "../hyphenation/Hyphenation.h"
#include "WgbPageView.h"
#include "WgbParagraphs.h"

namespace wintergreen {

namespace {

struct UnitPos {
  uint16_t unit;
  PagePosition pos;
};

bool before_or_at(const UnitPos& a, uint16_t unit, uint16_t para) {
  if (a.unit != unit)
    return a.unit < unit;
  return a.pos <= PagePosition{para, 0, 0};
}

void put_u16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
}

bool fits_u16(int v) {
  return v >= 0 && v <= 0xFFFF;
}

// One page as a display list (WgbFormat.h), in the order the device draws:
// lines, rules, images. Mirrors what ReaderScreen's draw did with the
// PageContent: words of zero length and images with no size or a key past the
// table were never drawn, so they are not stored.
bool serialize_page(const PageContent& pc, const BitmapFontSet& fset, uint16_t image_count, std::vector<uint8_t>& out,
                    std::string& err) {
  out.clear();
  out.resize(2);
  uint16_t items = 0;
  uint8_t cur_flags = 0;  // word flags in force; carried across lines
  std::vector<uint8_t> glyphs;

  for (const PageContentItem& ci : pc.items) {
    const PageTextItem* ti = std::get_if<PageTextItem>(&ci);
    if (!ti)
      continue;
    const int baseline = static_cast<int>(ti->y_offset) + ti->baseline;
    size_t nwords = 0;
    for (const LayoutWord& w : ti->line.words)
      nwords += w.len > 0 ? 1 : 0;
    if (nwords == 0)
      continue;
    if (nwords > 0xFF || !fits_u16(baseline)) {
      err = "line does not fit its display-list record";
      return false;
    }
    out.push_back(kWgbItemLine);
    put_u16(out, static_cast<uint16_t>(baseline));
    out.push_back(static_cast<uint8_t>(nwords));
    int prev_x = 0;
    for (const LayoutWord& w : ti->line.words) {
      if (w.len == 0)
        continue;
      const BitmapFont* f = fset.get(w.size_pct);
      int font = -1;
      for (int i = 0; i < fset.num_fonts(); ++i)
        if (fset.get_font(i) == f)
          font = i;
      const int x = static_cast<int>(kReaderHPadding) + w.x;
      if (font < 0 || font > 7 || !fits_u16(x)) {
        err = "word does not fit its display-list record";
        return false;
      }
      if (x >= prev_x && x - prev_x < kWgbAbsX) {
        out.push_back(static_cast<uint8_t>(x - prev_x));
      } else {
        out.push_back(kWgbAbsX);
        put_u16(out, static_cast<uint16_t>(x));
      }
      prev_x = x;
      const uint8_t flags = static_cast<uint8_t>((static_cast<uint8_t>(w.style) & 0x03) | (font << 2) |
                                                 ((static_cast<uint8_t>(w.vertical_align) & 0x03) << 5));
      // Glyph indices exactly as the device's UTF-8 draw resolved them: same
      // font object, same style fallback, U+FFFD included.
      glyphs.clear();
      const BitmapFont::Styled sf = f->styled(w.style);
      for (const char *p = w.text, *e = w.text + w.len; p < e;) {
        const int idx = sf.glyph_index(BitmapFont::decode_utf8(p, e));
        if (idx >= kWgbNoGlyph) {
          err = "glyph index does not fit its display-list record";
          return false;
        }
        const int v = idx < 0 ? kWgbNoGlyph : idx;
        if (v < 0x80) {
          glyphs.push_back(static_cast<uint8_t>(v));
        } else {
          glyphs.push_back(static_cast<uint8_t>(0x80 | (v >> 8)));
          glyphs.push_back(static_cast<uint8_t>(v));
        }
      }
      if (glyphs.size() > 0xFFFF) {
        err = "word does not fit its display-list record";
        return false;
      }
      const uint8_t new_flags = flags != cur_flags ? kWgbFlagsFollow : 0;
      if (glyphs.size() >= kWgbLongWord) {
        out.push_back(kWgbLongWord | new_flags);
        put_u16(out, static_cast<uint16_t>(glyphs.size()));
      } else {
        out.push_back(static_cast<uint8_t>(glyphs.size()) | new_flags);
      }
      if (new_flags) {
        out.push_back(flags);
        cur_flags = flags;
      }
      out.insert(out.end(), glyphs.begin(), glyphs.end());
    }
    ++items;
  }

  for (const PageContentItem& ci : pc.items) {
    const PageHrItem* h = std::get_if<PageHrItem>(&ci);
    if (!h)
      continue;
    out.push_back(kWgbItemRule);
    put_u16(out, h->x_offset);
    put_u16(out, h->y_offset);
    put_u16(out, h->width);
    put_u16(out, h->height);
    ++items;
  }

  auto image = [&](const PageImageItem& img) {
    if (img.width == 0 || img.height == 0 || img.key >= image_count)
      return;
    out.push_back(kWgbItemImage);
    put_u16(out, img.key);
    put_u16(out, img.x_offset);
    put_u16(out, img.y_offset);
    put_u16(out, img.width);
    put_u16(out, img.full_height > 0 ? img.full_height : img.height);
    put_u16(out, img.y_crop);
    put_u16(out, img.height);
    ++items;
  };
  for (const PageContentItem& ci : pc.items) {
    if (const PageImageItem* img = std::get_if<PageImageItem>(&ci))
      image(*img);
    else if (const PageTextItem* ti = std::get_if<PageTextItem>(&ci))
      if (ti->inline_image.has_value())
        image(*ti->inline_image);
  }

  wgb_write_u16(out.data(), items);
  if (out.size() > kWgbMaxPageBytes) {
    err = "page display list larger than kWgbMaxPageBytes";
    return false;
  }
  // Self-check: what the device will parse is exactly what was written.
  WgbPageView v(out.data(), out.size());
  while (v.skip()) {
  }
  if (!v.at_end()) {
    err = "page display list does not parse back";
    return false;
  }
  return true;
}

bool copy_range(FILE* in, FILE* out, uint32_t from, uint32_t size) {
  if (std::fseek(in, static_cast<long>(from), SEEK_SET) != 0)
    return false;
  uint8_t buf[16 * 1024];
  while (size > 0) {
    const size_t n = size < sizeof(buf) ? size : sizeof(buf);
    if (std::fread(buf, 1, n, in) != n || std::fwrite(buf, 1, n, out) != n)
      return false;
    size -= static_cast<uint32_t>(n);
  }
  return true;
}

}  // namespace

bool serialize_wgb_page(const PageContent& pc, const BitmapFontSet& fset, uint16_t image_count,
                        std::vector<uint8_t>& out, std::string& err) {
  return serialize_page(pc, fset, image_count, out, err);
}

bool paginate_wgb(const char* path, const std::vector<std::vector<uint32_t>>& units,
                  const std::vector<WgbChapterStart>& starts, const BitmapFontSet& fonts, std::string& err) {
  if (starts.size() > 0xFFFF) {
    err = "too many chapters";
    return false;
  }
  if (fonts.num_fonts() < kWgbFontSizes) {
    err = "reader font bundle has fewer sizes than the format";
    return false;
  }

  WgbReader reader;
  if (!reader.open(path)) {
    err = "unpaginated book does not open";
    return false;
  }
  WgbParagraphs paras(reader);
  const ImageSizeQuery size_fn = make_image_size_query(paras);
  const HyphenationLang lang = detect_language(reader.metadata().language);

  std::vector<WgbPage> pages[kWgbLayouts];
  std::vector<uint8_t> lists[kWgbLayouts];  // every page's display list, back to back
  std::vector<uint32_t> chapter_pages[kWgbLayouts];
  std::vector<uint8_t> dl;

  for (uint8_t l = 0; l < kWgbLayouts; ++l) {
    BitmapFontSet fset = fonts;
    fset.set_base_size_index(l / 2);
    TextLayout engine;
    engine.set_font(fset);
    engine.set_options(reader_page_options(l % 2 == 1));
    engine.set_image_size_fn(size_fn);
    engine.set_hyphenation_lang(lang);

    std::vector<UnitPos> where;
    for (size_t u = 0; u < units.size(); ++u) {
      const std::vector<uint32_t>& offs = units[u];
      if (offs.empty())
        continue;
      if (offs.size() > kWgbMaxUnitParagraphs || u > 0xFFFF) {
        err = "spine file too large to paginate";
        return false;
      }
      PagePosition pos{0, 0, 0};
      for (;;) {
        if (pos.text_offset > 0xFFFF) {
          err = "paragraph text offset does not fit a page record";
          return false;
        }
        WgbPageSource src(paras, offs[pos.paragraph], static_cast<uint16_t>(offs.size() - pos.paragraph),
                          pos.paragraph == 0);
        engine.set_source(src);
        const PagePosition local{0, pos.offset, pos.text_offset};
        engine.set_position(local);
        const PageContent pc = engine.layout();
        if (pc.items.empty() && pc.at_chapter_end)
          break;
        if (!serialize_page(pc, fset, reader.image_count(), dl, err))
          return false;
        pages[l].push_back(WgbPage{offs[pos.paragraph], pos.offset, static_cast<uint16_t>(pos.text_offset),
                                   static_cast<uint32_t>(lists[l].size()), static_cast<uint16_t>(dl.size())});
        lists[l].insert(lists[l].end(), dl.begin(), dl.end());
        where.push_back({static_cast<uint16_t>(u), pos});
        if (pc.at_chapter_end)
          break;
        const PagePosition next = engine.next_page_start(local, pc.end);
        if (next <= local) {
          err = "page would stall";
          return false;
        }
        pos = PagePosition{static_cast<uint16_t>(pos.paragraph + next.paragraph), next.offset, next.text_offset};
        if (pos.paragraph >= offs.size())
          break;
      }
    }
    if (pages[l].empty()) {
      err = "no pages";
      return false;
    }

    for (const WgbChapterStart& s : starts) {
      uint16_t unit = s.unit, para = s.para;
      if (unit < units.size() && para >= units[unit].size()) {
        ++unit;
        para = 0;
      }
      uint32_t idx = 0;
      for (uint32_t i = 0; i < where.size() && before_or_at(where[i], unit, para); ++i)
        idx = i;
      chapter_pages[l].push_back(idx);
    }
  }

  // --- Write the final file: no paragraph stream ------------------------------
  // The image bitmaps sit contiguously right after the paragraphs, so dropping
  // the stream moves everything from them to the end of the metadata down by
  // the same amount.
  WgbHeader hdr = reader.header();
  uint64_t blob_bytes = 0;
  std::vector<WgbImageRef> refs(hdr.image_count);
  for (uint16_t i = 0; i < hdr.image_count; ++i) {
    if (!reader.image_ref(i, refs[i])) {
      err = "reading the image table failed";
      return false;
    }
    blob_bytes += refs[i].data_size;
  }
  if (blob_bytes > hdr.image_offset - kWgbFirstParagraph) {
    err = "image bitmaps overlap the paragraph stream";
    return false;
  }
  const uint32_t blob_start = hdr.image_offset - static_cast<uint32_t>(blob_bytes);
  const uint32_t shift = blob_start - static_cast<uint32_t>(sizeof(WgbHeader));

  WgbHeader out_hdr = hdr;
  out_hdr.image_offset -= shift;
  out_hdr.meta_offset -= shift;
  out_hdr.pages_offset -= shift;
  out_hdr.chapter_count = static_cast<uint16_t>(starts.size());
  uint64_t lists_at = out_hdr.pages_offset;
  for (uint8_t l = 0; l < kWgbLayouts; ++l) {
    out_hdr.page_count[l] = static_cast<uint32_t>(pages[l].size());
    lists_at += starts.size() * 4u + pages[l].size() * kWgbPageSize;
  }
  uint64_t total = lists_at;
  for (uint8_t l = 0; l < kWgbLayouts; ++l)
    total += lists[l].size();
  if (total > 0xFFFFFFFFu) {
    err = "book larger than 4 GB";
    return false;
  }

  const std::string tmp = std::string(path) + ".tmp";
  FILE* out = std::fopen(tmp.c_str(), "wb");
  if (!out) {
    err = "cannot write the baked book";
    return false;
  }
  bool ok = std::fwrite(&out_hdr, sizeof(out_hdr), 1, out) == 1 &&
            copy_range(reader.file(), out, blob_start, static_cast<uint32_t>(blob_bytes));
  for (const WgbImageRef& r : refs) {
    uint8_t b[12];
    wgb_write_u32(b, r.data_size ? r.data_offset - shift : 0);
    wgb_write_u32(b + 4, r.data_size);
    wgb_write_u16(b + 8, r.width);
    wgb_write_u16(b + 10, r.height);
    ok = ok && std::fwrite(b, sizeof(b), 1, out) == 1;
  }
  ok = ok && copy_range(reader.file(), out, hdr.meta_offset, hdr.pages_offset - hdr.meta_offset);

  uint32_t list_base = static_cast<uint32_t>(lists_at);
  for (uint8_t l = 0; ok && l < kWgbLayouts; ++l) {
    for (uint32_t p : chapter_pages[l]) {
      uint8_t b[4];
      wgb_write_u32(b, p);
      ok = ok && std::fwrite(b, sizeof(b), 1, out) == 1;
    }
    for (const WgbPage& p : pages[l]) {
      uint8_t b[kWgbPageSize];
      wgb_write_u32(b, p.para_offset);
      wgb_write_u16(b + 4, p.offset);
      wgb_write_u16(b + 6, p.text_offset);
      wgb_write_u32(b + 8, list_base + p.dl_offset);
      wgb_write_u16(b + 12, p.dl_size);
      ok = ok && std::fwrite(b, sizeof(b), 1, out) == 1;
    }
    list_base += static_cast<uint32_t>(lists[l].size());
  }
  for (uint8_t l = 0; ok && l < kWgbLayouts; ++l)
    ok = std::fwrite(lists[l].data(), 1, lists[l].size(), out) == lists[l].size();
  ok = std::fclose(out) == 0 && ok;
  reader.close();
  ok = ok && std::remove(path) == 0 && std::rename(tmp.c_str(), path) == 0;
  if (!ok) {
    std::remove(tmp.c_str());
    err = "writing the baked book failed";
  }
  return ok;
}

}  // namespace wintergreen
