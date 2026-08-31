// Round-trip check for BookIndex::load after the reserve change.
#include "content/BookIndex.h"
#include <algorithm>
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

static void write_idx0(const char* p, int n) {
  FILE* f = fopen(p, "wb");
  fprintf(f, "#wintergreen-index v%u\n", INDEX_FORMAT_VERSION);
  for (int i = 0; i < n; ++i)
    fprintf(f, "/sdcard/book %d/book.wgb|Title %d|Author %d|0|0\n", i, i, i);
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

  {
    auto& ix = BookIndex::instance();
    write_idx("/tmp/i2", 5);
    assert(ix.load("/tmp/i2"));
    remove("/tmp/i2");
    assert(ix.save("/tmp/i2"));
    assert(fopen("/tmp/i2", "rb") == nullptr && "clean index must not be rewritten");

    ix.set_progress("/sdcard/book 0/book.wgb", 42);
    assert(ix.save("/tmp/i2"));
    FILE* f = fopen("/tmp/i2", "rb");
    assert(f && "dirty index must be written");
    fclose(f);
  }

  {
    auto& ix = BookIndex::instance();
    write_idx("/tmp/i3", 3);  // orders 0,1,2 — book 2 is the most recent
    assert(ix.load("/tmp/i3"));
    remove("/tmp/i3");
    ix.mark_opened("/sdcard/book 2/book.wgb");
    assert(ix.save("/tmp/i3"));
    assert(fopen("/tmp/i3", "rb") == nullptr && "re-opening the top book must write nothing");
    ix.mark_opened("/sdcard/book 0/book.wgb");
    uint32_t top = 0, second = 0;
    for (const auto& e : ix.entries())
      if (e.path.view(ix.pool()) == "/sdcard/book 0/book.wgb") top = e.last_open_order;
      else second = std::max(second, e.last_open_order);
    assert(top > second && "re-opened book must sort to the top");
    assert(ix.save("/tmp/i3"));
    assert(fopen("/tmp/i3", "rb") != nullptr);
  }

  {
    auto& ix = BookIndex::instance();
    write_idx0("/tmp/i4", 3);
    assert(ix.load("/tmp/i4"));
    ix.mark_opened("/sdcard/book 1/book.wgb");
    for (const auto& e : ix.entries())
      if (e.path.view(ix.pool()) == "/sdcard/book 1/book.wgb")
        assert(e.last_open_order != 0 && "first open must not stay at never-opened");
  }

  printf("BookIndex load round-trip OK\n");
}
