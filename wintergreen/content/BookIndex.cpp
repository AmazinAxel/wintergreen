#include "BookIndex.h"

#include <dirent.h>
#include <strings.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <queue>
#include <string>
#include <vector>

#include "CoverPaths.h"
#include "wgb/WgbReader.h"

namespace wintergreen {

bool ci_less(std::string_view a, std::string_view b) {
  size_t min_len = std::min(a.size(), b.size());
  int cmp = strncasecmp(a.data(), b.data(), min_len);
  if (cmp != 0) return cmp < 0;
  return a.size() < b.size();
}

// .wintergreen-index record: u16 path_len, u16 title_len, u16 author_len,
// u32 last_open_order, u8 progress_pct; little-endian. No magic, no version.
static constexpr size_t kIndexRecordBytes = 11;

BookIndex& BookIndex::instance() {
  static BookIndex instance;
  return instance;
}

// Case-insensitive suffix test.
static bool ends_with_ci(const char* name, size_t name_len, const char* ext, size_t ext_len) {
  if (name_len <= ext_len)
    return false;
  const char* p = name + name_len - ext_len;
  for (size_t i = 0; i < ext_len; ++i)
    if (std::tolower(static_cast<unsigned char>(p[i])) != ext[i])
      return false;
  return true;
}

// A book is a .wgb produced by `books` (tools/books). EPUBs are not recognised: the
// device has no converter, so listing one would only offer a book that cannot
// be opened.
static bool has_book_extension(const char* name, size_t name_len) {
  return ends_with_ci(name, name_len, ".wgb", 4);
}

std::string book_folder_name(const std::string& path) {
  const size_t last = path.find_last_of('/');
  if (last == std::string::npos || last == 0)
    return {};
  const size_t prev = path.find_last_of('/', last - 1);
  const size_t start = prev == std::string::npos ? 0 : prev + 1;
  return path.substr(start, last - start);
}

std::string book_title_fallback(const std::string& title, const std::string& path) {
  if (!title.empty() && title != "none")
    return title;
  std::string folder = book_folder_name(path);
  return folder.empty() ? path : folder;
}

bool read_book_meta(const std::string& path, std::string& title, std::string& author) {
  WgbReader r;
  if (!r.open(path.c_str()))
    return false;
  title = book_title_fallback(r.metadata().title, path);
  author = r.metadata().author.value_or("");
  return true;
}

bool BookIndex::is_wgb_path(const char* path) {
  if (!path)
    return false;
  const char* slash = std::strrchr(path, '/');
  const char* name = slash ? slash + 1 : path;
  return has_book_extension(name, std::strlen(name));
}

bool BookIndex::add_entry(std::string_view path, std::string_view title, std::string_view author,
                          uint32_t last_open_order) {
  if (static_cast<int>(entries_.size()) >= MAX_BOOKS)
    return false;
  BookIndexEntry entry;
  entry.path = pool_.add(path);
  entry.title = pool_.add(title);
  entry.author = pool_.add(author);
  entry.last_open_order = last_open_order;
  entries_.push_back(entry);
  dirty_ = true;
  thumbs_dirty_ = true;
  fresh_keys_.push_back(book_path_key(path));
  return true;
}

std::string thumb_pack_path(const std::string& index_file) {
  const size_t slash = index_file.find_last_of('/');
  return (slash == std::string::npos ? std::string() : index_file.substr(0, slash + 1)) + ".wintergreen-thumbs";
}

size_t thumb_pack_slot_bytes() {
  return 8 + cover_cols_bytes(kListCoverW, kListCoverH);
}

// Rebuild the pack in entry order. Slots of books already in the old pack are
// copied from it (one open, sequential-ish reads); only new books open their
// cover_list.bin. Same tmp + remove + rename as the index.
void BookIndex::save_thumbs_(const std::string& index_file) const {
  const std::string path = thumb_pack_path(index_file);
  const std::string tmp = path + ".tmp";
  const size_t slot_bytes = thumb_pack_slot_bytes();
  std::vector<uint8_t> slot(slot_bytes);

  FILE* old = std::fopen(path.c_str(), "rb");
  std::vector<uint32_t> old_keys;
  if (old) {
    uint8_t h[4];
    for (long at = 0; std::fseek(old, at, SEEK_SET) == 0 && std::fread(h, 1, 4, old) == 4;
         at += static_cast<long>(slot_bytes))
      old_keys.push_back(h[0] | (h[1] << 8) | (h[2] << 16) | (static_cast<uint32_t>(h[3]) << 24));
  }
  FILE* out = std::fopen(tmp.c_str(), "wb");
  if (!out) {
    if (old)
      std::fclose(old);
    return;
  }
  bool ok = true;
  for (const auto& e : entries_) {
    const std::string_view p = e.path.view(pool_);
    const uint32_t key = book_path_key(p);
    std::fill(slot.begin(), slot.end(), 0xFF);
    bool have = false;
    const bool fresh = std::find(fresh_keys_.begin(), fresh_keys_.end(), key) != fresh_keys_.end();
    for (size_t i = 0; old && !fresh && i < old_keys.size() && !have; ++i)
      if (old_keys[i] == key)
        have = std::fseek(old, static_cast<long>(i * slot_bytes), SEEK_SET) == 0 &&
               std::fread(slot.data(), 1, slot_bytes, old) == slot_bytes;
    if (!have) {
      uint16_t wh[2] = {0, 0};
      FILE* c = std::fopen(cover_list_bin_path(std::string(p).c_str()).c_str(), "rb");
      if (c) {
        if (std::fread(wh, 2, 2, c) != 2 || wh[0] == 0 || wh[1] == 0 || wh[0] > kListCoverW || wh[1] > kListCoverH ||
            std::fread(slot.data() + 8, 1, cover_cols_bytes(wh[0], wh[1]), c) != cover_cols_bytes(wh[0], wh[1]) ||
            std::fgetc(c) != EOF)
          wh[0] = wh[1] = 0;
        std::fclose(c);
      }
      std::memcpy(slot.data() + 4, &wh[0], 2);
      std::memcpy(slot.data() + 6, &wh[1], 2);
    }
    for (int b = 0; b < 4; ++b)
      slot[b] = static_cast<uint8_t>(key >> (8 * b));
    ok = ok && std::fwrite(slot.data(), 1, slot_bytes, out) == slot_bytes;
  }
  if (old)
    std::fclose(old);
  if (std::fclose(out) != 0 || !ok) {
    std::remove(tmp.c_str());
    return;
  }
  std::remove(path.c_str());
  if (std::rename(tmp.c_str(), path.c_str()) == 0) {
    thumbs_dirty_ = false;
    std::vector<uint32_t>().swap(fresh_keys_);
  }
}

bool BookIndex::load(const std::string& index_file) {
  FILE* f = std::fopen(index_file.c_str(), "rb");
  if (!f)
    return false;

  entries_.clear();
  pool_.reset();

  // Reserve before growing anything sized by file content. Without this the
  // entry vector reallocates its way up as it parses while the string pool is
  // also allocating chunks, and a vector realloc holds the old and new blocks at
  // once — the same transient spike that threw bad_alloc in build_page_items.
  //
  // Binary (see kIndexRecordBytes): the record table in one read, then each
  // string copied straight into the pool. No line splitting, no number
  // parsing. The sizes must add up to the file exactly, which a file in any
  // other shape fails; load() then reports no index and the caller rescans.
  long bytes = -1;
  if (std::fseek(f, 0, SEEK_END) == 0)
    bytes = std::ftell(f);
  std::rewind(f);
  uint8_t hdr[2];
  if (bytes < 2 || std::fread(hdr, 1, 2, f) != 2) {
    std::fclose(f);
    return false;
  }
  const size_t count = hdr[0] | (hdr[1] << 8);
  std::vector<uint8_t> recs(count * kIndexRecordBytes);
  size_t total = 2 + recs.size();
  bool ok = count <= MAX_BOOKS && std::fread(recs.data(), 1, recs.size(), f) == recs.size();
  for (size_t i = 0; ok && i < count; ++i) {
    const uint8_t* r = recs.data() + i * kIndexRecordBytes;
    total += static_cast<size_t>(r[0] | (r[1] << 8)) + (r[2] | (r[3] << 8)) + (r[4] | (r[5] << 8));
  }
  ok = ok && total == static_cast<size_t>(bytes);
  if (ok) {
    entries_.reserve(count);
    pool_.reserve(total);
  }
  // One stack buffer (the sync task loads this too), each string pooled as read.
  char s[1024];
  for (size_t i = 0; ok && i < count; ++i) {
    const uint8_t* r = recs.data() + i * kIndexRecordBytes;
    StringRef refs[3];
    for (int k = 0; k < 3 && ok; ++k) {
      const size_t len = r[2 * k] | (r[2 * k + 1] << 8);
      ok = len < sizeof(s) && std::fread(s, 1, len, f) == len;
      if (ok)
        refs[k] = pool_.add(std::string_view(s, len));
    }
    if (!ok)
      break;
    BookIndexEntry e;
    e.path = refs[0];
    e.title = refs[1];
    e.author = refs[2];
    e.last_open_order = r[6] | (r[7] << 8) | (r[8] << 16) | (static_cast<uint32_t>(r[9]) << 24);
    e.progress_pct = r[10] > 100 ? 100 : r[10];
    entries_.push_back(e);
  }
  std::fclose(f);
  if (!ok) {
    entries_.clear();
    pool_.reset();
    return false;
  }
  dirty_ = false;
  // The pack on the card was written with this index; nothing to rebuild.
  thumbs_dirty_ = false;
  std::vector<uint32_t>().swap(fresh_keys_);
  return true;
}

bool BookIndex::save(const std::string& index_file) const {
  // Rewriting an unchanged index is pure SD wear: the file is rewritten whole,
  // and a flash erase block is far larger than it. Every mutator sets dirty_ only
  // when a stored value actually moves, so re-opening a book you were already on
  // and closing it again writes nothing.
  if (!dirty_)
    return true;

  const std::string tmp_path = index_file + ".tmp";
  FILE* f = std::fopen(tmp_path.c_str(), "wb");
  if (!f)
    return false;

  // [u16 count][count records][strings: path, title, author per record].
  const uint8_t hdr[2] = {static_cast<uint8_t>(entries_.size()), static_cast<uint8_t>(entries_.size() >> 8)};
  bool ok = std::fwrite(hdr, 1, 2, f) == 2;
  for (const auto& entry : entries_) {
    uint8_t r[kIndexRecordBytes];
    const StringRef refs[3] = {entry.path, entry.title, entry.author};
    for (int k = 0; k < 3; ++k) {
      r[2 * k] = static_cast<uint8_t>(refs[k].len);
      r[2 * k + 1] = static_cast<uint8_t>(refs[k].len >> 8);
    }
    for (int b = 0; b < 4; ++b)
      r[6 + b] = static_cast<uint8_t>(entry.last_open_order >> (8 * b));
    r[10] = entry.progress_pct;
    ok = ok && std::fwrite(r, 1, sizeof(r), f) == sizeof(r);
  }
  for (const auto& entry : entries_)
    for (const StringRef& ref : {entry.path, entry.title, entry.author}) {
      const std::string_view v = ref.view(pool_);
      ok = ok && std::fwrite(v.data(), 1, v.size(), f) == v.size();
    }
  if (!ok) {
    std::fclose(f);
    std::remove(tmp_path.c_str());
    return false;
  }

  if (std::fclose(f) != 0) {
    std::remove(tmp_path.c_str());
    return false;
  }

  // Atomic replace — no backup copies. The index is rebuildable from the books
  // on the SD card, and rename() already prevents a half-written file.
  // See Application::save_settings_: FatFs rename does not replace an existing
  // destination, so it must be removed first.
  std::remove(index_file.c_str());
  if (std::rename(tmp_path.c_str(), index_file.c_str()) == 0)
    dirty_ = false;
  // Only when books came or went (sync, rescan): never on a reader close.
  if (thumbs_dirty_)
    save_thumbs_(index_file);
  return true;
}


void BookIndex::remove_entry(std::string_view path) {
  auto it = std::find_if(entries_.begin(), entries_.end(),
                         [&](const BookIndexEntry& e) { return e.path.view(pool_) == path; });
  if (it != entries_.end()) {
    entries_.erase(it);
    dirty_ = true;
    thumbs_dirty_ = true;
  }
}

void BookIndex::mark_opened(std::string_view path) {
  uint32_t max_order = 0;
  BookIndexEntry* found = nullptr;
  for (auto& entry : entries_) {
    max_order = std::max(max_order, entry.last_open_order);
    if (!found && entry.path.view(pool_) == path) found = &entry;
  }
  if (!found) return;
  // Already the most-recently-opened book: leave it alone so re-reading the
  // book you are already on does not dirty the index and force a save.
  if (found->last_open_order != max_order || max_order == 0) {
    found->last_open_order = max_order + 1;
    dirty_ = true;
  }
}
void BookIndex::set_progress(std::string_view path, int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  for (auto& entry : entries_) {
    if (entry.path.view(pool_) == path) {
      if (entry.progress_pct != static_cast<uint8_t>(pct)) {
        entry.progress_pct = static_cast<uint8_t>(pct);
        dirty_ = true;
      }
      return;
    }
  }
}

void BookIndex::build_index(const std::string& root_dir) {
  dirty_ = true;
  // Preserve open order across a rescan so the most-recently-read sort survives.
  struct OldStats {
    std::string key;
    uint32_t order;
    uint8_t pct;
  };
  std::vector<OldStats> old_stats;
  for (const auto& e : entries_)
    if (e.last_open_order > 0)
      old_stats.push_back({e.title.to_string(pool_) + '\x01' + e.author.to_string(pool_), e.last_open_order,
                           e.progress_pct});

  entries_.clear();
  pool_.reset();
  dirty_ = false;
  // Same reasoning as load(): the scan pushes an entry per book found while
  // WgbReader is holding its own chapter-table buffer, so let the vector settle
  // its allocation up front. The previous entry count is the best estimate
  // available before walking the card.
  if (!old_stats.empty())
    entries_.reserve(old_stats.size());
  // Process books as we find them to avoid storing all paths in memory.
  auto process_path = [&](const std::string& path) {
    if (static_cast<int>(entries_.size()) >= MAX_BOOKS)
      return;

    std::string title, author;
    if (read_book_meta(path, title, author)) {
      add_entry(path, title, author);
      const std::string key = title + '\x01' + author;
      for (const auto& old : old_stats) {
        if (old.key == key) {
          entries_.back().last_open_order = old.order;
          entries_.back().progress_pct = old.pct;
          break;
        }
      }
    }
  };

  // Single iterator helper: calls `cb(path)` for each book found
  // Templated on the callback rather than taking std::function: type erasure here
  // cost a heap allocation and an indirect call per book, and emitted the lambda
  // body as a separate 1.7 KB symbol.
  auto iterate_books = [&](auto&& cb) {
    std::queue<std::string> q;
    q.push(root_dir);
    while (!q.empty()) {
      std::string current_dir = std::move(q.front());
      q.pop();

      // Called for each entry in current_dir; shared dot-skip and .epub check.
      auto visit = [&](const char* name, bool is_dir, const std::string& fullpath) {
        if (name[0] == '.') return;
        if (is_dir) { q.push(fullpath); return; }
        if (has_book_extension(name, std::strlen(name)))
          cb(fullpath);
      };

      DIR* dir = opendir(current_dir.c_str());
      if (!dir) continue;
      struct dirent* ent;
      while ((ent = readdir(dir)) != nullptr) {
        std::string fullpath = current_dir + "/" + ent->d_name;
        visit(ent->d_name, ent->d_type == DT_DIR, fullpath);
      }
      closedir(dir);
    }
  };

  // One pass, not two. The old code walked the whole card first purely to count
  // books into `total` for a log line that is compiled out at
  // LOG_DEFAULT_LEVEL_NONE — a full recursive SD scan, over a bus shared with
  // the panel, for nothing.
  iterate_books(process_path);

  // build_index is a structural mutation — bump so observers (MainMenu) refresh
  // even if the call doesn't go through index_file/remove_path/rename_in_place.
  ++generation_;

}

}  // namespace wintergreen
