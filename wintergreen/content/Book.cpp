#include "Book.h"

#include "CoverPaths.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <vector>


namespace wintergreen {

EpubError Book::open(const char* path, uint8_t* work_buf, uint8_t* xml_buf, bool parse_css_ncx) {
  close();  // release previous resources
  if (!file_.open(path))
    return EpubError::ZipError;
  file_open_ = true;

  // If caller didn't provide buffers (desktop / tests), allocate here.
  // Two separate allocations so heap fragmentation can't block us: the
  // 45KB work buf and 4KB xml buf fit individually even when no single
  // 49KB block is free.
  static constexpr size_t kWorkBufSize = ZipEntryInput::kDecompSize + ZipEntryInput::kDictSize + 1024;
  static constexpr size_t kXmlBufSize = 4096;
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

  return epub_.open(file_, work_buf, xml_buf, parse_css_ncx);
}

bool Book::open_zip_only(const char* path) {
  close();
  if (!file_.open(path))
    return false;
  file_open_ = true;
  auto err = epub_.open_zip_only(file_);
  if (err != EpubError::Ok) {
    close();
    return false;
  }
  return true;
}

void Book::close() {
  epub_.close();
  file_.close();
  file_open_ = false;
}

EpubError Book::load_chapter(size_t index, Chapter& out) {
  return epub_.parse_chapter(file_, index, out);
}

EpubError Book::load_chapter_streaming(size_t index, ParagraphSink sink, void* sink_ctx, uint8_t* work_buf,
                                       uint8_t* xml_buf, IdSink id_sink, void* id_sink_ctx) {
  return epub_.parse_chapter_streaming(file_, index, sink, sink_ctx, work_buf, xml_buf, id_sink, id_sink_ctx);
}

ImageError Book::decode_image(uint16_t entry_index, DecodedImage& out, uint16_t max_w, uint16_t max_h,
                              uint8_t* work_buf, size_t work_buf_size) {
  if (entry_index >= epub_.zip().entry_count())
    return ImageError::UnsupportedFormat;

  auto& entry = epub_.zip().entry(entry_index);
  return decode_image_from_entry(file_, entry, max_w, max_h, out, work_buf, work_buf_size, /*scale_to_fill=*/true);
}

ZipError Book::extract_entry(uint16_t entry_index, std::vector<uint8_t>& out) {
  if (entry_index >= epub_.zip().entry_count())
    return ZipError::InvalidData;
  auto& entry = epub_.zip().entry(entry_index);
  return epub_.zip().extract(file_, entry, out);
}

bool Book::read_image_size(uint16_t entry_index, uint16_t& w, uint16_t& h, uint8_t* work_buf, size_t work_size) {
  if (entry_index >= epub_.zip().entry_count()) {
    return false;
  }
  static constexpr size_t kWorkSize = ZipEntryInput::kDecompSize + ZipEntryInput::kDictSize + 1024;
  std::unique_ptr<uint8_t[]> owned;
  if (!work_buf || work_size < kWorkSize) {
    owned = std::make_unique<uint8_t[]>(kWorkSize);
    work_buf = owned.get();
  }
  ImageSizeStream stream;
  epub_.zip().extract_streaming(
      file_, epub_.zip().entry(entry_index),
      [](const uint8_t* d, size_t n, void* ud) -> bool { return !static_cast<ImageSizeStream*>(ud)->feed(d, n); },
      &stream, work_buf, kWorkSize);
  if (!stream.ok()) {
    return false;
  }
  w = stream.width();
  h = stream.height();
  return true;
}

bool Book::write_cover_bin(const char* cover_path, int max_w, int max_h,
                            uint8_t* work_buf, size_t work_buf_size) {
  const int idx = epub_.cover_zip_idx();
  if (idx < 0 || idx >= static_cast<int>(epub_.zip().entry_count()))
    return false;
  DecodedImage img;
  auto& entry = epub_.zip().entry(static_cast<uint16_t>(idx));
  auto err = decode_image_from_entry(file_, entry, max_w, max_h, img, work_buf, work_buf_size, /*scale_to_fill=*/false);
  if (err != ImageError::Ok || img.data.empty())
    return false;
  FILE* f = std::fopen(cover_path, "wb");
  if (!f)
    return false;
  uint16_t le[2] = {img.width, img.height};
  const bool ok = (std::fwrite(le, 2, 2, f) == 2) &&
                  (std::fwrite(img.data.data(), 1, img.data.size(), f) == img.data.size());
  std::fclose(f);
  if (!ok) { std::remove(cover_path); return false; }
  return true;
}

// Compose the sleep screen exactly as the panel will show it, so the device
// only has to blit it: trimmed, stretched to the full panel, dithered. None of
// it depends on anything known only at runtime — the panel size is fixed — so
// it happens here and the device gets a 1:1 blit of a W x H bitmap.
//
// The stretch is deliberate and is **not** aspect-preserving. Fitting the cover
// inside the panel leaves letterbox bars, and on a 1-bit panel there is no good
// colour for them: white bars on a dark cover look like a mistake, and the
// alternative (measuring the artwork's edge ink and filling the bars to match)
// was tried and is gone with this comment. Covers are close enough to the
// panel's aspect that filling it outright reads as a full-bleed image, which is
// what a sleep screen wants — and there is no bar geometry left to get wrong.
//
// **Never resample the decoder's output.** It returns a dithered 1-bit bitmap,
// and scaling that means picking whole pixels out of an error-diffused pattern:
// the dropped pixels are the ones carrying the diffused error, so the local
// average stops matching the original tone and the result bands and mottles.
// This function used to decode at W x H and then nearest-neighbour the trimmed
// sub-rectangle up to the panel, which is exactly that — and it silently
// contradicted the "dithered once" comment two lines above it.
//
// So the scale is pushed into the decoder, which scales from *grayscale* and
// dithers in the same pass (see dither_row's x_step). Two decodes: one to
// measure the trim box, one at dimensions inflated so the trimmed region lands
// at exactly W x H. The second decode's crop is then a pure copy, no resampling
// at any point. Decoding twice costs converter time only, and the converter
// already decodes this cover three times for the three sizes.
bool Book::write_sleep_cover_bin(const char* cover_path, int W, int H,
                                 uint8_t* work_buf, size_t work_buf_size) {
  const int idx = epub_.cover_zip_idx();
  if (idx < 0 || idx >= static_cast<int>(epub_.zip().entry_count()))
    return false;
  if (W <= 0 || H <= 0)
    return false;

  auto& entry = epub_.zip().entry(static_cast<uint16_t>(idx));

  // Pass 1: decode at panel size purely to locate the blank bands, and to
  // measure the source tone range. Only the trim box and the histogram are
  // taken from this; none of its pixels reach the output.
  DecodedImage probe;
  ToneHistogram hist;
  if (decode_image_from_entry(file_, entry, static_cast<uint16_t>(W), static_cast<uint16_t>(H), probe,
                              work_buf, work_buf_size, /*scale_to_fill=*/true, /*sink=*/nullptr,
                              /*pixel_sink=*/nullptr, /*tone_lut=*/nullptr, &hist) != ImageError::Ok)
    return false;

  // Contrast-stretch low-contrast covers. A cover whose tones all sit in the
  // middle (a soft watercolour, a faded scan) dithers to a uniform speckle with
  // no true black or paper white, and on a 1-bit panel in sunlight that reads as
  // flat gray. Measured on "alice in wonderland": 46.5% ink with zero rows at
  // either extreme, against ~95% (the hobbit) and ~10% (lord of the flies),
  // both of which look right on the device untouched. build_stretch_lut
  // declines when the image already spans the range, so those are unaffected.
  //
  // ponytail: 0.5% clip is a guess at how much of a cover may be specular
  // highlight or printer's black. If a stretched cover looks blown out, raise
  // it; if a faded one still looks flat, lower it.
  uint8_t lut[256];
  const bool stretch = build_stretch_lut(hist.bins, 0.005, lut);
  const ToneLut tone = stretch ? lut : nullptr;
  if (probe.data.empty() || probe.width == 0 || probe.height == 0)
    return false;

  const int cw = probe.width, ch = probe.height;
  const size_t pstride = probe.stride();
  const uint8_t* psrc = probe.data.data();
  auto pink = [&](int x, int y) {
    return ((psrc[static_cast<size_t>(y) * pstride + (x >> 3)] >> (7 - (x & 7))) & 1) == 0;
  };

  // Trim fully blank rows and columns. Publishers bake white bands into cover
  // artwork, and stretching those to the panel just makes them bigger.
  int x0 = 0, y0 = 0, x1 = cw, y1 = ch;
  {
    auto row_blank = [&](int y) {
      for (int x = 0; x < cw; ++x) if (pink(x, y)) return false;
      return true;
    };
    auto col_blank = [&](int x) {
      for (int y = y0; y < y1; ++y) if (pink(x, y)) return false;
      return true;
    };
    while (y0 < y1 - 1 && row_blank(y0)) ++y0;
    while (y1 > y0 + 1 && row_blank(y1 - 1)) --y1;
    while (x0 < x1 - 1 && col_blank(x0)) ++x0;
    while (x1 > x0 + 1 && col_blank(x1 - 1)) --x1;
  }
  const int keep_w = x1 - x0, keep_h = y1 - y0;
  if (keep_w <= 0 || keep_h <= 0)
    return false;

  const size_t out_stride = (static_cast<size_t>(W) + 7) / 8;
  std::vector<uint8_t> out;

  if (keep_w == cw && keep_h == ch && !stretch) {
    // Nothing to trim and no tone map: the probe is already the final image at
    // the right size. With a stretch the probe is unusable — it was dithered
    // from the raw tones — so fall through and decode again through the LUT.
    out = std::move(probe.data);
  } else if (keep_w == cw && keep_h == ch) {
    DecodedImage full;
    if (decode_image_from_entry(file_, entry, static_cast<uint16_t>(W), static_cast<uint16_t>(H), full,
                                work_buf, work_buf_size, /*scale_to_fill=*/true, /*sink=*/nullptr,
                                /*pixel_sink=*/nullptr, tone) != ImageError::Ok)
      return false;
    if (full.data.empty() || full.width != W || full.height != H)
      return false;
    out = std::move(full.data);
  } else {
    // Pass 2: inflate the decode so the kept region scales up to exactly W x H,
    // then take that region out of it. Rounding up keeps the crop inside the
    // decoded image; any excess is absorbed by clamping the origin below.
    const int dec_w = static_cast<int>((static_cast<int64_t>(W) * cw + keep_w - 1) / keep_w);
    const int dec_h = static_cast<int>((static_cast<int64_t>(H) * ch + keep_h - 1) / keep_h);
    if (dec_w <= 0 || dec_h <= 0 || dec_w > 0xFFFF || dec_h > 0xFFFF)
      return false;

    DecodedImage full;
    if (decode_image_from_entry(file_, entry, static_cast<uint16_t>(dec_w), static_cast<uint16_t>(dec_h), full,
                                work_buf, work_buf_size, /*scale_to_fill=*/true, /*sink=*/nullptr,
                                /*pixel_sink=*/nullptr, tone) != ImageError::Ok)
      return false;
    if (full.data.empty() || full.width < W || full.height < H)
      return false;

    // Map the trim origin into the inflated image, clamped so the W x H window
    // always lies inside it.
    int ox = static_cast<int>(static_cast<int64_t>(x0) * full.width / cw);
    int oy = static_cast<int>(static_cast<int64_t>(y0) * full.height / ch);
    if (ox > full.width - W) ox = full.width - W;
    if (oy > full.height - H) oy = full.height - H;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;

    const size_t fstride = full.stride();
    const uint8_t* fsrc = full.data.data();
    out.assign(out_stride * static_cast<size_t>(H), 0xFF);
    for (int dy = 0; dy < H; ++dy) {
      const uint8_t* srow = fsrc + static_cast<size_t>(oy + dy) * fstride;
      uint8_t* orow = out.data() + static_cast<size_t>(dy) * out_stride;
      for (int dx = 0; dx < W; ++dx) {
        const int sx = ox + dx;
        if (((srow[sx >> 3] >> (7 - (sx & 7))) & 1) == 0)
          orow[dx >> 3] &= static_cast<uint8_t>(~(0x80u >> (dx & 7)));
      }
    }
  }

  FILE* f = std::fopen(cover_path, "wb");
  if (!f)
    return false;
  const uint16_t le[2] = {static_cast<uint16_t>(W), static_cast<uint16_t>(H)};
  const bool ok = (std::fwrite(le, 2, 2, f) == 2) &&
                  (std::fwrite(out.data(), 1, out.size(), f) == out.size());
  std::fclose(f);
  if (!ok) { std::remove(cover_path); return false; }
  return true;
}

static std::string epub_stem_(const char* epub_path) {
  const char* p = epub_path;
  const char* slash = nullptr;
  for (const char* c = p; *c; ++c)
    if (*c == '/' || *c == '\\') slash = c;
  const char* name = slash ? slash + 1 : p;
  const char* dot = std::strrchr(name, '.');
  return dot ? std::string(name, static_cast<size_t>(dot - name)) : std::string(name);
}

}  // namespace wintergreen
