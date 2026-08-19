#include "MrbConverter.h"

#include <cstring>

#include "../../display/DrawBuffer.h"
#include "../Book.h"
#include "../EpubParser.h"
#include "../ZipReader.h"

namespace wintergreen {

// Shared image-key remapping used by both converter paths.
namespace {
struct ImageMapping {
  uint16_t zip_key;
  uint16_t mrb_idx;
};

uint16_t get_or_add_image(MrbWriter& writer, std::vector<ImageMapping>& image_map, uint16_t zip_key,
                          uint32_t local_offset, uint16_t w, uint16_t h) {
  bool caller_has_size = (w != 0 || h != 0);
  for (const auto& m : image_map) {
    if (m.zip_key == zip_key && writer.image_size_known(m.mrb_idx) == caller_has_size)
      return m.mrb_idx;
  }
  uint16_t idx = writer.add_image_ref(local_offset, w, h);
  image_map.push_back({zip_key, idx});
  return idx;
}

void remap_paragraph_images(Paragraph& para, MrbWriter& writer, std::vector<ImageMapping>& image_map,
                            const ZipReader& zip) {
  if (para.type == ParagraphType::Image) {
    uint32_t offset = zip.entry(para.image.key).local_header_offset;
    para.image.key =
        get_or_add_image(writer, image_map, para.image.key, offset, para.image.attr_width, para.image.attr_height);
  }
  if (para.type == ParagraphType::Text && para.text.inline_image.has_value()) {
    auto& img = *para.text.inline_image;
    uint32_t offset = zip.entry(img.key).local_header_offset;
    img.key = get_or_add_image(writer, image_map, img.key, offset, img.attr_width, img.attr_height);
  }
}

// Split a large text paragraph into chunks to keep the serialized body small.
// Budget is the estimated on-disk size: 18 (para header) + runs×12 (run headers) + text bytes.
// Keeping them small avoids large contiguous heap allocations at read time on ESP32-C3.
// Annotated books can have thousands of tiny/empty inline spans — the 12-byte run header
// dominates even when total text bytes are small.
static constexpr size_t kMaxSerializedBody = 8192;  // max estimated serialized size per chunk
static constexpr size_t kParaHeaderBytes = 18;
static constexpr size_t kRunHeaderBytes = 12;

// Returns estimated serialized size for a text paragraph.
static size_t estimated_body_size(const TextParagraph& text) {
  size_t sz = kParaHeaderBytes;
  for (const auto& run : text.runs)
    sz += kRunHeaderBytes + run.text.size();
  return sz;
}

bool write_split_paragraph(MrbWriter& writer, Paragraph& para) {
  if (para.type != ParagraphType::Text)
    return writer.write_paragraph(para);

  const size_t est = estimated_body_size(para.text);

  if (est <= kMaxSerializedBody)
    return writer.write_paragraph(para);

  // Write the paragraph as a series of chunks, each within kMaxSerializedBody bytes.
  // We track slice indices into para.text.runs — no intermediate vector is allocated.
  // Only the first chunk gets indent, spacing_before, and inline_image.
  const auto& runs = para.text.runs;
  const size_t total_runs = runs.size();

  // Shared metadata for all chunks (no runs vector — passed as pointer slices).
  TextParagraph meta;
  meta.alignment = para.text.alignment;
  meta.line_height_pct = para.text.line_height_pct;
  meta.indent = para.text.indent;
  meta.inline_image = para.text.inline_image;
  const uint16_t first_spacing = para.spacing_before.value_or(kMrbSpacingDefault);

  size_t chunk_start = 0;
  size_t chunk_est = kParaHeaderBytes;
  bool is_first = true;
  bool ok = true;

  auto flush_range = [&](size_t start, size_t end) {
    if (start == end)
      return;
    if (!writer.write_text_paragraph(meta, is_first ? first_spacing : kMrbSpacingDefault, runs.data() + start,
                                     end - start))
      ok = false;
    if (is_first) {
      is_first = false;
      meta.indent.reset();
      meta.inline_image.reset();
    }
    chunk_est = kParaHeaderBytes;
    chunk_start = end;
  };

  for (size_t i = 0; i < total_runs; ++i) {
    const Run& run = runs[i];
    const size_t run_cost = kRunHeaderBytes + run.text.size();

    // If a single run exceeds the budget on its own, flush whatever we have first,
    // then emit it as its own chunk (rare — would require >8KB of text in one run).
    if (run_cost >= kMaxSerializedBody) {
      flush_range(chunk_start, i);
      chunk_est += run_cost;
      continue;
    }

    // Would this run overflow the current chunk? Flush first.
    if (i > chunk_start && chunk_est + run_cost > kMaxSerializedBody)
      flush_range(chunk_start, i);

    chunk_est += run_cost;
  }

  flush_range(chunk_start, total_runs);
  return ok;
}

}  // namespace

bool convert_epub_to_mrb_streaming(Book& book, const char* output_path, uint8_t* work_buf, uint8_t* xml_buf,
                                   std::function<void(int, int)> progress_cb) {
  MrbWriter writer;
  if (!writer.open(output_path)) {
    return false;
  }

  // On ESP32, caller always passes pre-allocated buffers. On desktop (tests etc.)
  // allocate here so the leaf parse_chapter_streaming never has to.
  static constexpr size_t kWorkBufSize = ZipEntryInput::kDecompSize + ZipEntryInput::kDictSize + 2048;
  static constexpr size_t kXmlBufSize = 16384;
  std::unique_ptr<uint8_t[]> owned_work;
  std::unique_ptr<uint8_t[]> owned_xml;
  if (!work_buf) {
    owned_work = std::make_unique<uint8_t[]>(kWorkBufSize);
    work_buf = owned_work.get();
  }
  if (!xml_buf) {
    owned_xml = std::make_unique<uint8_t[]>(kXmlBufSize);
    xml_buf = owned_xml.get();
  }

  std::vector<ImageMapping> image_map;
  const auto& zip = book.epub().zip();

  // --- Fragment → para_index resolution setup ---
  // Build a working copy of the TOC early so we can resolve fragment anchors.
  struct FragmentNeed {
    uint16_t zip_file_idx;  // zip entry index of the XHTML file (raw, before spine remapping)
    std::string fragment;   // the id value to locate
    size_t toc_entry_idx;   // index into toc_work.entries to fill in
  };
  // Use a reference — moving the TOC would invalidate StringRef::pool pointers.
  TableOfContents& toc_work = book.toc();

  std::vector<FragmentNeed> fragment_needs;
  fragment_needs.reserve(toc_work.entries.size());
  for (size_t i = 0; i < toc_work.entries.size(); ++i) {
    auto frag = toc_work.entries[i].fragment.to_string(toc_work.pool);
    if (!frag.empty()) {
      fragment_needs.push_back({toc_work.entries[i].file_idx, std::string(frag), i});
    }
  }

  // Context for the streaming paragraph + ID sinks.
  struct SinkCtx {
    MrbWriter* writer;
    std::vector<ImageMapping>* image_map;
    const ZipReader* zip;
    bool error;
    // Fragment resolution (nullptr if no fragments need resolving)
    std::vector<FragmentNeed>* fragment_needs;
    TableOfContents* toc_work;
    uint16_t current_zip_file_idx;
    uint16_t current_chapter_idx;
    // (anchors are written directly to the MrbWriter as they arrive)
  };
  SinkCtx ctx{};
  ctx.writer = &writer;
  ctx.image_map = &image_map;
  ctx.zip = &zip;
  ctx.error = false;
  ctx.fragment_needs = &fragment_needs;
  ctx.toc_work = &toc_work;
  ctx.current_zip_file_idx = 0;
  ctx.current_chapter_idx = 0;

  // ID sink: always active.
  // Resolves TOC fragment anchors AND collects all id→para mappings for runtime link navigation.
  IdSink id_sink = [](void* raw_ctx, const char* id_p, size_t id_len, uint32_t para_idx) {
    auto& c = *static_cast<SinkCtx*>(raw_ctx);
    // TOC fragment resolution.
    for (auto& need : *c.fragment_needs) {
      if (need.zip_file_idx == c.current_zip_file_idx && need.fragment.size() == id_len &&
          std::memcmp(need.fragment.data(), id_p, id_len) == 0) {
        auto& entry = c.toc_work->entries[need.toc_entry_idx];
        if (entry.para_index == 0)  // only record first match per entry
          entry.para_index = static_cast<uint16_t>(para_idx < 0xFFFFu ? para_idx : 0xFFFFu);
      }
    }
    // Anchor collection: write directly to MRB to avoid buffering all anchors in RAM.
    if (id_len > 0 && id_len <= 255 && para_idx < 0xFFFFu)
      c.writer->add_anchor(c.current_chapter_idx, static_cast<uint16_t>(para_idx), id_p, id_len);
  };

  // Non-text paragraph sink: remap image keys, split large text paragraphs, and write to MRB.
  auto sink = [](void* raw_ctx, Paragraph&& para) {
    auto& c = *static_cast<SinkCtx*>(raw_ctx);
    if (c.error)
      return;

    remap_paragraph_images(para, *c.writer, *c.image_map, *c.zip);

    bool write_ok = write_split_paragraph(*c.writer, para);
    if (!write_ok) {
      c.error = true;
    }
  };

  for (size_t ci = 0; ci < book.chapter_count(); ++ci) {
    writer.begin_chapter();

    ctx.current_zip_file_idx = static_cast<uint16_t>(book.epub().spine()[ci].file_idx);
    ctx.current_chapter_idx = static_cast<uint16_t>(ci);
    book.load_chapter_streaming(ci, sink, &ctx, work_buf, xml_buf, id_sink, &ctx);
    if (ctx.error) {
      return false;
    }

    writer.end_chapter();

    if (progress_cb)
      progress_cb(static_cast<int>(ci + 1), static_cast<int>(book.chapter_count()));

  }

  // Remap TOC file_idx (zip entry index) → spine index so ReaderScreen::load_chapter_() works.
  // toc_work already has para_index values filled in from fragment anchor resolution.
  const auto& spine = book.epub().spine();
  for (auto& entry : toc_work.entries) {
    for (size_t si = 0; si < spine.size(); ++si) {
      if (spine[si].file_idx == entry.file_idx) {
        entry.file_idx = static_cast<uint16_t>(si);
        break;
      }
    }
  }

  // Build spine filename table: base filename of each spine item for href resolution at runtime.
  std::vector<std::string> spine_files;
  spine_files.reserve(spine.size());
  for (const auto& si : spine) {
    std::string_view entry_name = zip.entry(si.file_idx).name;
    auto slash_pos = entry_name.rfind('/');
    std::string basename =
        (slash_pos != std::string_view::npos) ? std::string(entry_name.substr(slash_pos + 1)) : std::string(entry_name);
    spine_files.push_back(std::move(basename));
  }

  // --- Embed the image bytes -------------------------------------------------
  // v12: the MRB carries its images, so a converted book needs no EPUB at read
  // time. The bytes go in verbatim — they are already JPEG/PNG, so re-deflating
  // them would cost size and force an inflate pass on the device for nothing.
  {
    std::vector<uint8_t> img_bytes;
    for (const auto& m : image_map) {
      if (writer.image_has_data(m.mrb_idx))
        continue;  // the same EPUB entry can back several image refs
      if (m.zip_key >= zip.entry_count())
        continue;
      const ZipEntry& e = zip.entry(m.zip_key);
      img_bytes.clear();
      const ZipError err = zip.extract(book.file(), e, img_bytes);
      if (err != ZipError::Ok || img_bytes.empty())
        continue;  // leave data_size 0; the reader renders blank for that image
      writer.set_image_data(m.mrb_idx, std::move(img_bytes));
      img_bytes = std::vector<uint8_t>();
    }
  }

  bool ok = writer.finish(book.metadata(), toc_work, spine_files);
  writer.close();  // explicit close so fclose() happens before we return

  // Extract both covers alongside the MRB if the EPUB has one. The device
  // cannot generate these itself for a converted book — there is no EPUB on the
  // card — so anything not written here is simply absent.
  //   cover.bin        160x240, book list and home screen
  //   cover_sleep.bin  480x786, sleep screen
  if (ok) {
    const std::string base(output_path);
    const size_t pos = base.rfind("book.mrb");
    if (pos != std::string::npos) {
      const size_t work_sz = work_buf ? (ZipEntryInput::kDecompSize + ZipEntryInput::kDictSize + 1024) : 0;
      std::string p = base;
      p.replace(pos, 8, "cover.bin");
      book.write_cover_bin(p.c_str(), 160, 240, work_buf, work_sz);
      p = base;
      p.replace(pos, 8, "cover_sleep.bin");
      book.write_cover_bin(p.c_str(), 480, 786, work_buf, work_sz);
    }
  }

  return ok;
}

}  // namespace wintergreen
