#include "WgbConverter.h"

#include <cstring>

#include "../../display/DrawBuffer.h"
#include "../Book.h"
#include "../EpubParser.h"
#include "../ImageDecoder.h"
#include "../ZipReader.h"
#include "../CoverPaths.h"
#include "WgbPaginator.h"

namespace wintergreen {

// Shared image-key remapping used by both converter paths.
namespace {
struct ImageMapping {
  uint16_t zip_key;
  uint16_t wgb_idx;
};

uint16_t get_or_add_image(WgbWriter& writer, std::vector<ImageMapping>& image_map, uint16_t zip_key,
                          uint32_t local_offset, uint16_t w, uint16_t h) {
  bool caller_has_size = (w != 0 || h != 0);
  for (const auto& m : image_map) {
    if (m.zip_key == zip_key && writer.image_size_known(m.wgb_idx) == caller_has_size)
      return m.wgb_idx;
  }
  uint16_t idx = writer.add_image_ref(local_offset, w, h);
  image_map.push_back({zip_key, idx});
  return idx;
}

void remap_paragraph_images(Paragraph& para, WgbWriter& writer, std::vector<ImageMapping>& image_map,
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

bool write_split_paragraph(WgbWriter& writer, Paragraph& para) {
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
  const uint16_t first_spacing = para.spacing_before.value_or(kWgbSpacingDefault);

  size_t chunk_start = 0;
  size_t chunk_est = kParaHeaderBytes;
  bool is_first = true;
  bool ok = true;

  auto flush_range = [&](size_t start, size_t end) {
    if (start == end)
      return;
    // **Route through write_paragraph, not write_text_paragraph.** This splitter
    // bounds the *serialized* size (run headers dominate in annotated books);
    // WgbWriter::write_paragraph bounds the *text* size, which is what decides
    // whether the device can lay the paragraph out. Calling the low-level writer
    // here bypassed that entirely, so a chunk under kMaxSerializedBody but over
    // kSplitParagraphBytes reached the file whole — 25 of them in the Odyssey,
    // the largest 1,709 bytes, which is how oversized paragraphs survived a
    // re-convert that was supposed to remove them.
    Paragraph chunk;
    chunk.type = ParagraphType::Text;
    chunk.text = meta;
    chunk.text.runs.assign(runs.begin() + start, runs.begin() + end);
    chunk.spacing_before = is_first ? first_spacing : kWgbSpacingDefault;
    if (!writer.write_paragraph(chunk))
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

bool convert_epub_to_wgb_streaming(Book& book, const char* output_path, const std::vector<PlannedChapter>& plan,
                                   const BitmapFontSet& fonts, std::string& err,
                                   uint8_t* work_buf, uint8_t* xml_buf, std::function<void(int, int)> progress_cb) {
  if (plan.empty() || plan.front().spine_idx != 0)
    return false;
  WgbWriter writer;
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

  // --- Chapter plan: who owns each paragraph ---------------------------------
  // `cur` is the plan entry owning the content being parsed. It moves at the
  // start of a spine file (to the last entry starting there with no fragment)
  // and when the parser reports the id an entry starts at. Paragraphs owned by
  // a dropped entry are not written, and neither are their images.
  struct Start {
    bool resolved = false;
    uint16_t chapter = 0;  // written chapter index
    uint16_t para = 0;     // writer paragraph index within it
  };
  std::vector<Start> starts(plan.size());

  struct SinkCtx {
    WgbWriter* writer;
    std::vector<ImageMapping>* image_map;
    const ZipReader* zip;
    const std::vector<PlannedChapter>* plan;
    std::vector<Start>* starts;
    size_t cur;
    uint16_t spine_idx;
    bool error;
  };
  SinkCtx ctx{&writer, &image_map, &zip, &plan, &starts, 0, 0, false};

  auto mark_start = [](SinkCtx& c, size_t i) {
    c.cur = i;
    Start& s = (*c.starts)[i];
    if (!(*c.plan)[i].keep || s.resolved)
      return;
    s.resolved = true;
    s.chapter = c.writer->chapter_count();
    // **The writer's count, not the parser's.** The parser counts source
    // paragraphs; the writer splits oversized ones, so the two diverge after
    // the first split and a chapter jump would land short. The sink fires
    // between paragraphs, so this is the index the next paragraph gets.
    s.para = c.writer->chapter_paragraph_count();
  };

  IdSink id_sink = [](void* raw_ctx, const char* id_p, size_t id_len, uint32_t) {
    auto& c = *static_cast<SinkCtx*>(raw_ctx);
    const auto& p = *c.plan;
    for (size_t i = c.cur + 1; i < p.size() && p[i].spine_idx == c.spine_idx; ++i) {
      if (p[i].fragment.size() == id_len && std::memcmp(p[i].fragment.data(), id_p, id_len) == 0) {
        // Same rule as mark_start, which a captureless lambda can't call.
        c.cur = i;
        Start& s = (*c.starts)[i];
        if (p[i].keep && !s.resolved) {
          s.resolved = true;
          s.chapter = c.writer->chapter_count();
          s.para = c.writer->chapter_paragraph_count();
        }
        return;
      }
    }
  };

  auto sink = [](void* raw_ctx, Paragraph&& para) {
    auto& c = *static_cast<SinkCtx*>(raw_ctx);
    if (c.error || !(*c.plan)[c.cur].keep)
      return;
    remap_paragraph_images(para, *c.writer, *c.image_map, *c.zip);
    if (!write_split_paragraph(*c.writer, para))
      c.error = true;
  };

  // Plan entries [next, end) start in spine file si. The leading run with no
  // fragment starts at the file's first byte (plan[0] always does, whatever its
  // fragment); the rest start at their anchors. `books` orders them so.
  size_t next = 0;
  for (size_t si = 0; si < book.chapter_count(); ++si) {
    ctx.spine_idx = static_cast<uint16_t>(si);
    size_t end = next;
    while (end < plan.size() && plan[end].spine_idx == si)
      ++end;
    size_t lead = next;
    while (lead < end && (lead == 0 || plan[lead].fragment.empty()))
      ++lead;

    bool any_kept = plan[lead > next ? lead - 1 : ctx.cur].keep;
    for (size_t i = lead; i < end; ++i)
      any_kept = any_kept || plan[i].keep;

    if (any_kept) {
      writer.begin_chapter();
      for (size_t i = next; i < lead; ++i)
        mark_start(ctx, i);
      book.load_chapter_streaming(si, sink, &ctx, work_buf, xml_buf, id_sink, &ctx);
      if (ctx.error)
        return false;
      if (writer.chapter_paragraph_count() == 0)
        writer.cancel_chapter();  // starts recorded in it now name the next chapter
      else
        writer.end_chapter();
    }
    // The last entry starting here owns what follows, whether its anchor was
    // seen or not (a skipped file reports no anchors).
    if (end > next)
      ctx.cur = end - 1;
    next = end;

    if (progress_cb)
      progress_cb(static_cast<int>(si + 1), static_cast<int>(book.chapter_count()));
  }

  // Where each kept entry starts. No labels: the device numbers them.
  std::vector<WgbChapterStart> chapter_starts;
  const uint16_t written = writer.chapter_count();
  for (size_t i = 0; i < plan.size(); ++i) {
    const Start& s = starts[i];
    if (s.resolved && s.chapter < written)
      chapter_starts.push_back({s.chapter, s.para});  // unit, paragraph
  }
  if (written == 0)
    return false;


  // --- Rasterise the images --------------------------------------------------
  // Each image is decoded, scaled to fit the panel's portrait page box and
  // dithered to 1 bit *here*, and only that bitmap goes into the WGB. The device
  // then draws an image by reading rows and blitting them.
  //
  // This is the single biggest thing the format does for the reader. Storing the
  // original JPEG/PNG meant the device carried a JPEG decoder, a PNG decoder, an
  // inflate implementation and a ditherer; decoded a multi-megapixel image on a
  // 160 MHz core the first time a figure appeared; and wrote the result back to
  // the SD card so it would not have to do it twice. None of that exists now, and
  // a book is roughly 70% smaller because the source images were most of it.
  {
    for (const auto& m : image_map) {
      if (writer.image_has_data(m.wgb_idx))
        continue;  // the same EPUB entry can back several image refs
      if (m.zip_key >= zip.entry_count())
        continue;
      DecodedImage img;
      const ZipEntry& e = zip.entry(m.zip_key);
      const ImageError err = decode_image_from_entry(book.file(), e, kWgbImageBoxW, kWgbImageBoxH, img, work_buf,
                                                     work_buf ? (ZipEntryInput::kDecompSize + ZipEntryInput::kDictSize + 1024) : 0,
                                                     /*scale_to_fill=*/false);
      if (err != ImageError::Ok || img.data.empty() || img.width == 0 || img.height == 0)
        continue;  // leave data_size 0; the reader draws blank space for that image
      // The stored size is the drawn size: layout reads these dimensions back out
      // of the image table and scale_image() never enlarges.
      writer.update_image_size(m.wgb_idx, img.width, img.height);
      writer.set_image_data(m.wgb_idx, std::move(img.data));
    }
  }

  bool ok = writer.finish(book.metadata());
  const std::vector<std::vector<uint32_t>> units = writer.units();
  writer.close();  // explicit close so fclose() happens before paginating
  if (!ok)
    err = "writing the book failed";

  // --- Bake the pages ----------------------------------------------------------
  // Every page of every layout, laid out here with the device's own code and
  // font, so the device never paginates. See WgbFormat.h.
  ok = ok && paginate_wgb(output_path, units, chapter_starts, fonts, err);

  // Extract the covers alongside the WGB if the EPUB has one, each already at
  // the size its screen draws it: the device never scales a cover. The device
  // cannot generate these itself for a converted book — there is no EPUB on the
  // card — so anything not written here is simply absent.
  //   cover_list.bin   kListCoverW x kListCoverH, book list rows — read 1:1
  //   cover_home.bin   kHomeCoverW x kHomeCoverH, home carousel — sized to the
  //                    box so the device blits it 1:1 and dithers only once
  //   cover_sleep.bin  kSleepCoverW x kSleepCoverH, composed exactly as the
  //                    panel shows it (letterbox bars included)
  if (ok) {
    const std::string base(output_path);
    const size_t pos = base.rfind("book.wgb");
    if (pos != std::string::npos) {
      const size_t work_sz = work_buf ? (ZipEntryInput::kDecompSize + ZipEntryInput::kDictSize + 1024) : 0;
      std::string p = base;
      p.replace(pos, 8, "cover_list.bin");
      book.write_cover_bin(p.c_str(), kListCoverW, kListCoverH, work_buf, work_sz);
      p = base;
      p.replace(pos, 8, "cover_home.bin");
      book.write_cover_bin(p.c_str(), kHomeCoverW, kHomeCoverH, work_buf, work_sz);
      p = base;
      p.replace(pos, 8, "cover_sleep.bin");
      book.write_sleep_cover_bin(p.c_str(), kSleepCoverW, kSleepCoverH, work_buf, work_sz);
    }
  }

  return ok;
}

}  // namespace wintergreen
