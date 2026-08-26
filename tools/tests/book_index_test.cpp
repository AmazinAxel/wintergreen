// Round-trip check for BookIndex::load after the reserve change.
#include "content/BookIndex.h"
#include <cassert>
#include <cstdio>
#include <string>
using namespace wintergreen;

static void write_idx(const char* p, int n) {
  FILE* f = fopen(p, "wb");
  fprintf(f, "#wintergreen-index v%u\n", INDEX_FORMAT_VERSION);
  for (int i = 0; i < n; ++i)
    fprintf(f, "/sdcard/book %d/book.wgb|Title %d|Author %d|%d|%d\n", i, i, i, i, i % 101);
  fclose(f);
}

int main() {
  // Empty file: header only, no entries, must not crash on the size estimate.
  write_idx("/tmp/i0", 0);
  assert(BookIndex::instance().load("/tmp/i0"));
  assert(BookIndex::instance().entries().empty());

  // Sizes either side of the bytes/64 estimator, incl. over MAX_BOOKS.
  for (int n : {1, 3, 63, 64, 250, 300}) {
    write_idx("/tmp/i1", n);
    auto& ix = BookIndex::instance();
    assert(ix.load("/tmp/i1"));
    const int want = n > MAX_BOOKS ? MAX_BOOKS : n;
    assert((int)ix.entries().size() == want);
    for (int i = 0; i < want; ++i) {
      const auto& e = ix.entries()[i];
      assert(e.path.view(ix.pool()) == "/sdcard/book " + std::to_string(i) + "/book.wgb");
      assert(e.title.view(ix.pool()) == "Title " + std::to_string(i));
      assert(e.author.view(ix.pool()) == "Author " + std::to_string(i));
      assert(e.last_open_order == (uint32_t)i);
      assert(e.progress_pct == (uint8_t)(i % 101));
    }
  }

  // Missing file must still report failure, not a stale index.
  assert(!BookIndex::instance().load("/tmp/does-not-exist"));
  printf("BookIndex load round-trip OK\n");
}
