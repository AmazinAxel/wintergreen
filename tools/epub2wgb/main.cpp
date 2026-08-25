// epub2wgb — host-side EPUB → WGB converter.
//
// Runs the exact same pipeline the firmware used to run on-device
// (Book::open + convert_epub_to_wgb_streaming), so the output is byte-identical
// to what the reader would have produced for itself. Conversion on the ESP32 is
// slow; doing it here means the device only ever reads.
//
// Build and run via tools/convert-books.sh.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "wintergreen/content/Book.h"
#include "wintergreen/content/wgb/WgbConverter.h"
#include "wintergreen/content/wgb/WgbReader.h"

namespace fs = std::filesystem;
using namespace wintergreen;

// The device hands the converter two ~48 KB display buffers as scratch so it
// never touches the heap. Here we just allocate them.
static constexpr size_t kScratch = 48000;

// stderr is shared by the worker threads; only whole lines are written under it.
// The per-book progress percentage is suppressed when there is more than one
// worker — interleaved carriage returns are unreadable.
static std::mutex g_report_mu;
static bool g_progress = true;

static bool convert_one(const fs::path& epub, const fs::path& out_dir, bool verbose) {
  Book book;
  std::vector<uint8_t> work(kScratch), xml(kScratch);

  const EpubError err = book.open(epub.string().c_str(), work.data(), xml.data());
  if (err != EpubError::Ok || book.chapter_count() == 0) {
    std::lock_guard<std::mutex> lk(g_report_mu);
    std::fprintf(stderr, "  FAIL  %s: open error %d, %u chapters\n", epub.filename().string().c_str(),
                 static_cast<int>(err), static_cast<unsigned>(book.chapter_count()));
    return false;
  }

  // One directory per book, named after the EPUB stem. The reader finds
  // <dir>/book.wgb; keeping each book in its own folder leaves room for the
  // cover and image data to sit alongside it.
  const fs::path book_dir = out_dir / epub.stem();
  std::error_code ec;
  fs::create_directories(book_dir, ec);
  const fs::path wgb = book_dir / "book.wgb";

  int last_pct = -1;
  const bool ok = convert_epub_to_wgb_streaming(
      book, wgb.string().c_str(), work.data(), xml.data(),
      [&](int done, int total) {
        if (!verbose || !g_progress || total <= 0)
          return;
        const int pct = done * 100 / total;
        if (pct != last_pct) {
          last_pct = pct;
          std::fprintf(stderr, "\r  %-40.40s %3d%%", epub.filename().string().c_str(), pct);
          std::fflush(stderr);
        }
      });
  book.close();

  if (verbose && g_progress)
    std::fprintf(stderr, "\r");

  if (!ok) {
    std::lock_guard<std::mutex> lk(g_report_mu);
    std::fprintf(stderr, "  FAIL  %s: conversion failed\n", epub.filename().string().c_str());
    return false;
  }

  // Read it back the way the device will, so a file that cannot be opened is
  // reported here rather than as a book that silently refuses to load.
  WgbReader reader;
  if (!reader.open(wgb.string().c_str())) {
    std::lock_guard<std::mutex> lk(g_report_mu);
    std::fprintf(stderr, "  FAIL  %s: output does not reopen\n", epub.filename().string().c_str());
    return false;
  }
  const auto sz = fs::file_size(wgb, ec);
  {
    std::lock_guard<std::mutex> lk(g_report_mu);
    std::fprintf(stderr, "  ok    %-40.40s  %u ch, %u img, %.1f KB  \"%s\"\n", epub.filename().string().c_str(),
                 static_cast<unsigned>(reader.chapter_count()), static_cast<unsigned>(reader.image_count()),
                 static_cast<double>(sz) / 1024.0, reader.metadata().title.c_str());
  }
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
                   "usage: epub2wgb [-o OUTDIR] [-q] <file.epub | directory> ...\n"
                   "  Writes OUTDIR/<book name>/book.wgb for each input EPUB.\n");
      return 0;
    } else {
      inputs.push_back(a);
    }
  }

  if (inputs.empty()) {
    std::fprintf(stderr, "epub2wgb: no input. Try --help.\n");
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

  // Convert books in parallel. Each conversion owns its Book, its two 48 KB
  // scratch buffers and its own output directory, and the pipeline holds no
  // mutable global state, so the only thing that needs serialising is the
  // reporting. Worth doing for a library on a Raspberry Pi, where a single core
  // is slow but there are four of them.
  unsigned workers = std::thread::hardware_concurrency();
  if (workers == 0)
    workers = 1;
  if (workers > epubs.size())
    workers = static_cast<unsigned>(epubs.size());

  g_progress = (workers == 1);

  std::atomic<size_t> next{0};
  std::atomic<int> failed{0};
  std::vector<std::thread> pool;
  pool.reserve(workers);
  for (unsigned w = 0; w < workers; ++w) {
    pool.emplace_back([&] {
      for (;;) {
        const size_t i = next.fetch_add(1);
        if (i >= epubs.size())
          return;
        if (!convert_one(epubs[i], out_dir, verbose))
          failed.fetch_add(1);
      }
    });
  }
  for (auto& t : pool)
    t.join();

  std::fprintf(stderr, "\n%zu converted, %d failed -> %s\n", epubs.size() - failed.load(), failed.load(),
               fs::absolute(out_dir).string().c_str());
  return failed.load() == 0 ? 0 : 1;
}
