// convert_epubs <book.epub> ...
//
// Test driver for run.sh: bakes each EPUB into ./<slug>/ the way `books` does,
// with the proposed chapter list as is. No prompts, no upload, the input is
// not touched.

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "review.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  int failed = 0;
  const fs::path tmp = fs::temp_directory_path() / ("convert_epubs." + std::to_string(::getpid()));
  fs::create_directories(tmp);
  for (int i = 1; i < argc; ++i) {
    books::Review r;
    const std::string err = r.open(argv[i]);
    if (!err.empty()) {
      std::fprintf(stderr, "  FAIL  %s: %s\n", argv[i], err.c_str());
      ++failed;
      continue;
    }
    r.propose_chapters();
    const std::string epub = (tmp / "book.epub").string();
    std::string summary;
    const std::string name = r.write_epub(epub, true) ? r.bake(epub, ".", summary) : std::string();
    if (name.empty()) {
      std::fprintf(stderr, "  FAIL  %s: %s\n", argv[i], summary.empty() ? "write failed" : summary.c_str());
      ++failed;
      continue;
    }
    std::fprintf(stderr, "  ok    %s: %s\n", name.c_str(), summary.c_str());
  }
  std::error_code ec;
  fs::remove_all(tmp, ec);
  return failed ? 1 : 0;
}
