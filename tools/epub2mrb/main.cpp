// epub2mrb — host-side EPUB → MRB converter.
//
// Runs the exact same pipeline the firmware used to run on-device
// (Book::open + convert_epub_to_mrb_streaming), so the output is byte-identical
// to what the reader would have produced for itself. Conversion on the ESP32 is
// slow; doing it here means the device only ever reads.
//
// Build and run via tools/convert-books.sh.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "wintergreen/content/Book.h"
#include "wintergreen/content/mrb/MrbConverter.h"
#include "wintergreen/content/mrb/MrbReader.h"

namespace fs = std::filesystem;
using namespace wintergreen;

// The device hands the converter two ~48 KB display buffers as scratch so it
// never touches the heap. Here we just allocate them.
static constexpr size_t kScratch = 48000;


// Panel geometry in portrait. The sleep screen blits its cover with a
// nearest-neighbour scale-to-fit; emitting a cover that is already exactly
// panel-sized makes that a 1:1 copy, so the only resampling happens here on a
// real computer instead of twice on the device.
static constexpr uint16_t kPanelW = 480;
static constexpr uint16_t kPanelH = 786;

// cover_*.bin is [u16 w][u16 h][1bpp rows, MSB first, 1 = white].
// Re-emit `path` centred on a white kPanelW x kPanelH field.
static bool pad_cover_to_panel(const fs::path& path) {
  std::vector<uint8_t> in;
  {
    FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f)
      return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 4) {
      std::fclose(f);
      return false;
    }
    in.resize(static_cast<size_t>(n));
    const bool ok = std::fread(in.data(), 1, in.size(), f) == in.size();
    std::fclose(f);
    if (!ok)
      return false;
  }

  const uint16_t w = static_cast<uint16_t>(in[0] | (in[1] << 8));
  const uint16_t h = static_cast<uint16_t>(in[2] | (in[3] << 8));
  if (w == 0 || h == 0 || w > kPanelW || h > kPanelH)
    return false;                     // already panel-sized, or unexpected
  if (w == kPanelW && h == kPanelH)
    return true;

  const size_t src_stride = (w + 7) / 8;
  if (in.size() < 4 + src_stride * h)
    return false;

  const size_t dst_stride = (kPanelW + 7) / 8;
  std::vector<uint8_t> out(4 + dst_stride * kPanelH, 0xFF);  // 1 = white
  out[0] = kPanelW & 0xFF; out[1] = kPanelW >> 8;
  out[2] = kPanelH & 0xFF; out[3] = kPanelH >> 8;

  const int ox = (kPanelW - w) / 2;
  const int oy = (kPanelH - h) / 2;
  for (uint16_t y = 0; y < h; ++y) {
    const uint8_t* srow = in.data() + 4 + static_cast<size_t>(y) * src_stride;
    uint8_t* drow = out.data() + 4 + static_cast<size_t>(oy + y) * dst_stride;
    for (uint16_t x = 0; x < w; ++x) {
      if (((srow[x >> 3] >> (7 - (x & 7))) & 1) == 0) {   // black pixel
        const int dx = ox + x;
        drow[dx >> 3] &= static_cast<uint8_t>(~(0x80u >> (dx & 7)));
      }
    }
  }

  FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f)
    return false;
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

static bool convert_one(const fs::path& epub, const fs::path& out_dir, bool verbose) {
  Book book;
  std::vector<uint8_t> work(kScratch), xml(kScratch);

  const EpubError err = book.open(epub.string().c_str(), work.data(), xml.data());
  if (err != EpubError::Ok || book.chapter_count() == 0) {
    std::fprintf(stderr, "  FAIL  %s: open error %d, %u chapters\n", epub.filename().string().c_str(),
                 static_cast<int>(err), static_cast<unsigned>(book.chapter_count()));
    return false;
  }

  // One directory per book, named after the EPUB stem. The reader finds
  // <dir>/book.mrb; keeping each book in its own folder leaves room for the
  // cover and image data to sit alongside it.
  const fs::path book_dir = out_dir / epub.stem();
  std::error_code ec;
  fs::create_directories(book_dir, ec);
  const fs::path mrb = book_dir / "book.mrb";

  int last_pct = -1;
  const bool ok = convert_epub_to_mrb_streaming(
      book, mrb.string().c_str(), work.data(), xml.data(),
      [&](int done, int total) {
        if (!verbose || total <= 0)
          return;
        const int pct = done * 100 / total;
        if (pct != last_pct) {
          last_pct = pct;
          std::fprintf(stderr, "\r  %-40.40s %3d%%", epub.filename().string().c_str(), pct);
          std::fflush(stderr);
        }
      });
  book.close();

  if (verbose)
    std::fprintf(stderr, "\r");

  if (!ok) {
    std::fprintf(stderr, "  FAIL  %s: conversion failed\n", epub.filename().string().c_str());
    return false;
  }

  // The sleep cover is letterboxed to the exact panel size here so the device
  // never has to rescale it.
  const fs::path sleep_cover = book_dir / "cover_sleep.bin";
  if (fs::exists(sleep_cover, ec))
    pad_cover_to_panel(sleep_cover);

  // Read it back the way the device will, so a file that cannot be opened is
  // reported here rather than as a book that silently refuses to load.
  MrbReader reader;
  if (!reader.open(mrb.string().c_str())) {
    std::fprintf(stderr, "  FAIL  %s: output does not reopen\n", epub.filename().string().c_str());
    return false;
  }
  const auto sz = fs::file_size(mrb, ec);
  std::fprintf(stderr, "  ok    %-40.40s  %u ch, %u img, %.1f KB  \"%s\"\n", epub.filename().string().c_str(),
               static_cast<unsigned>(reader.chapter_count()), static_cast<unsigned>(reader.image_count()),
               static_cast<double>(sz) / 1024.0, reader.metadata().title.c_str());
  reader.close();
  return true;
}

int main(int argc, char** argv) {
  std::vector<fs::path> inputs;
  fs::path out_dir = ".";
  bool verbose = true;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-o" && i + 1 < argc) {
      out_dir = argv[++i];
    } else if (a == "-q") {
      verbose = false;
    } else if (a == "-h" || a == "--help") {
      std::fprintf(stderr,
                   "usage: epub2mrb [-o OUTDIR] [-q] <file.epub | directory> ...\n"
                   "  Writes OUTDIR/<book name>/book.mrb for each input EPUB.\n");
      return 0;
    } else {
      inputs.push_back(a);
    }
  }

  if (inputs.empty()) {
    std::fprintf(stderr, "epub2mrb: no input. Try --help.\n");
    return 2;
  }

  std::vector<fs::path> epubs;
  for (const auto& in : inputs) {
    std::error_code ec;
    if (fs::is_directory(in, ec)) {
      for (const auto& e : fs::directory_iterator(in, ec)) {
        std::string ext = e.path().extension().string();
        for (char& c : ext)
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".epub")
          epubs.push_back(e.path());
      }
    } else {
      epubs.push_back(in);
    }
  }
  std::sort(epubs.begin(), epubs.end());

  std::error_code ec;
  fs::create_directories(out_dir, ec);

  int failed = 0;
  for (const auto& e : epubs)
    if (!convert_one(e, out_dir, verbose))
      ++failed;

  std::fprintf(stderr, "\n%zu converted, %d failed -> %s\n", epubs.size() - failed, failed,
               fs::absolute(out_dir).string().c_str());
  return failed == 0 ? 0 : 1;
}
