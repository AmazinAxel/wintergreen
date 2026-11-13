// Round-trip check for BookIndex::load after the reserve change.
#include "content/BookIndex.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "content/CoverPaths.h"
using namespace wintergreen;

// The binary index, written by hand (not by save()) so the format itself is
// what is tested: [u16 count][count × {u16 lens[3], u32 order, u8 pct}][strings].
static void write_bin(const char* p, int n, bool with_order) {
  FILE* f = fopen(p, "wb");
  const uint8_t hdr[2] = {uint8_t(n), uint8_t(n >> 8)};
  fwrite(hdr, 1, 2, f);
  auto s = [](const char* fmt, int i) { return std::string(fmt) + std::to_string(i); };
  for (int i = 0; i < n; ++i) {
    const std::string strs[3] = {"/sdcard/book " + std::to_string(i) + "/book.wgb", s("Title ", i), s("Author ", i)};
    uint8_t r[11];
    for (int k = 0; k < 3; ++k) {
      r[2 * k] = uint8_t(strs[k].size());
      r[2 * k + 1] = uint8_t(strs[k].size() >> 8);
    }
    const uint32_t order = with_order ? uint32_t(i) : 0;
    for (int b = 0; b < 4; ++b)
      r[6 + b] = uint8_t(order >> (8 * b));
    r[10] = with_order ? uint8_t(i % 101) : 0;
    fwrite(r, 1, 11, f);
  }
  for (int i = 0; i < n; ++i) {
    const std::string strs[3] = {"/sdcard/book " + std::to_string(i) + "/book.wgb", s("Title ", i), s("Author ", i)};
    for (const auto& x : strs)
      fwrite(x.data(), 1, x.size(), f);
  }
  fclose(f);
}

static void write_idx(const char* p, int n) {
  write_bin(p, n, true);
}

static void write_idx0(const char* p, int n) {
  write_bin(p, n, false);
}

int main() {
  // Empty file: header only, no entries, must not crash on the size estimate.
  write_idx("/tmp/i0", 0);
  assert(BookIndex::instance().load("/tmp/i0"));
  assert(BookIndex::instance().entries().empty());

  // Over MAX_BOOKS is not a file save() writes: rejected whole.
  write_idx("/tmp/i1", 300);
  assert(!BookIndex::instance().load("/tmp/i1"));
  // A text index from older firmware, or a truncated file: rejected, not misread.
  {
    FILE* f = fopen("/tmp/i1", "wb");
    fprintf(f, "/sdcard/a/book.wgb|A|B|1|2\n");
    fclose(f);
    assert(!BookIndex::instance().load("/tmp/i1"));
    write_idx("/tmp/i1", 5);
    std::filesystem::resize_file("/tmp/i1", std::filesystem::file_size("/tmp/i1") - 1);
    assert(!BookIndex::instance().load("/tmp/i1"));
    assert(BookIndex::instance().entries().empty());
  }

  // Load, then save() and load again: the same entries both times.
  for (int n : {1, 3, 63, 64, 250}) {
    write_idx("/tmp/i1", n);
    auto& ix = BookIndex::instance();
    assert(ix.load("/tmp/i1"));
    ix.set_progress("/sdcard/book 0/book.wgb", 77);  // dirty, so save() writes
    ix.set_progress("/sdcard/book 0/book.wgb", 0);
    assert(ix.save("/tmp/i1"));
    assert(ix.load("/tmp/i1"));
    const int want = n;
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

  // Thumbnail pack: built on add/remove saves only, slots in entry order, old
  // slots copied, a re-added book re-read from its own file.
  {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "wg_pack_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string idx = (dir / ".wintergreen-index").string();
    const std::string pack = thumb_pack_path(idx);
    auto book = [&](int i) { return (dir / ("b" + std::to_string(i)) / "book.wgb").string(); };
    auto write_cover = [&](int i, uint8_t fill) {
      fs::create_directories(dir / ("b" + std::to_string(i)));
      FILE* f = fopen((dir / ("b" + std::to_string(i)) / "cover_list.bin").c_str(), "wb");
      const uint16_t wh[2] = {10, 20};
      fwrite(wh, 2, 2, f);
      std::vector<uint8_t> body(cover_cols_bytes(10, 20), fill);
      fwrite(body.data(), 1, body.size(), f);
      fclose(f);
    };
    auto slot = [&](int i, uint32_t& key, int& w, uint8_t& first) {
      FILE* f = fopen(pack.c_str(), "rb");
      assert(f);
      std::vector<uint8_t> s(thumb_pack_slot_bytes());
      fseek(f, static_cast<long>(i * s.size()), SEEK_SET);
      const bool ok = fread(s.data(), 1, s.size(), f) == s.size();
      fclose(f);
      assert(ok);
      key = s[0] | (s[1] << 8) | (s[2] << 16) | (uint32_t(s[3]) << 24);
      w = s[4] | (s[5] << 8);
      first = s[8];
    };
    write_cover(0, 0x11);
    write_cover(1, 0x22);
    fs::create_directories(dir / "b2");  // no cover
    write_idx0(idx.c_str(), 0);
    auto& ix = BookIndex::instance();
    assert(ix.load(idx));
    for (int i = 0; i < 3; ++i)
      ix.add_entry(book(i), "T", "A");
    assert(ix.save(idx));
    uint32_t k;
    int w;
    uint8_t first;
    slot(0, k, w, first);
    assert(k == book_path_key(book(0)) && w == 10 && first == 0x11);
    slot(2, k, w, first);
    assert(k == book_path_key(book(2)) && w == 0);

    // Progress only: the pack is not touched.
    fs::remove(pack);
    ix.set_progress(book(0), 5);
    assert(ix.save(idx));
    assert(!fs::exists(pack));

    // Re-sync of book 1 with a new cover; book 0's slot comes from the old pack
    // even though its file changed (proves the copy), book 1's from its file.
    ix.remove_entry(book(2));
    assert(ix.save(idx));  // rebuilds a full pack
    write_cover(0, 0x33);
    write_cover(1, 0x44);
    ix.remove_entry(book(1));
    ix.add_entry(book(1), "T", "A");
    assert(ix.save(idx));
    slot(0, k, w, first);
    assert(k == book_path_key(book(0)) && first == 0x11);
    slot(1, k, w, first);
    assert(k == book_path_key(book(1)) && first == 0x44);
    fs::remove_all(dir);
  }

  printf("BookIndex load round-trip OK\n");
}
