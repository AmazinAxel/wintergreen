#include "BookIndex.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <queue>
#include <string>
#include <vector>

#include "../display/DrawBuffer.h"
#include "wgb/WgbReader.h"

#ifdef ESP_PLATFORM
#include <dirent.h>
#else
#include <filesystem>
namespace fs = std::filesystem;
#endif

namespace wintergreen {

bool ci_less(std::string_view a, std::string_view b) {
  size_t min_len = std::min(a.size(), b.size());
#ifdef _WIN32
  int cmp = _strnicmp(a.data(), b.data(), min_len);
#else
  int cmp = strncasecmp(a.data(), b.data(), min_len);
#endif
  if (cmp != 0) return cmp < 0;
  return a.size() < b.size();
}

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

// A book is a .wgb produced by tools/epub2wgb. EPUBs are not recognised: the
// device has no converter, so listing one would only offer a book that cannot
// be opened.
static bool has_book_extension(const char* name, size_t name_len) {
  return ends_with_ci(name, name_len, ".wgb", 4);
}

// Name of the directory containing `path`, used as a title fallback.
static std::string folder_name_of(const std::string& path) {
  const size_t last = path.find_last_of('/');
  if (last == std::string::npos || last == 0)
    return path;
  const size_t prev = path.find_last_of('/', last - 1);
  return path.substr(prev == std::string::npos ? 0 : prev + 1, last - (prev == std::string::npos ? 0 : prev + 1));
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
  return true;
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
  // Sized from the file rather than MAX_BOOKS: a 3-book library should not take
  // 4 KB for 250 slots. One line is a path, a title, an author and two numbers,
  // so bytes/64 is a floor on the entry count that never over-reserves for a
  // real index. Off by a little is harmless — this only has to stop the
  // reallocation chain, not predict exactly.
  if (std::fseek(f, 0, SEEK_END) == 0) {
    const long bytes = std::ftell(f);
    if (bytes > 0) {
      const size_t est = static_cast<size_t>(bytes) / 64 + 1;
      entries_.reserve(est > MAX_BOOKS ? MAX_BOOKS : est);
      pool_.reserve(static_cast<size_t>(bytes));
    }
  }
  std::rewind(f);

  char line[1024];
  bool first_line = true;
  bool needs_rebuild = false;
  while (std::fgets(line, sizeof(line), f) && static_cast<int>(entries_.size()) < MAX_BOOKS) {
    if (first_line) {
      first_line = false;
      if (std::strncmp(line, "#wintergreen-index v", 20) == 0) {
        uint32_t v = static_cast<uint32_t>(std::strtoul(line + 20, nullptr, 10));
        if (v != INDEX_FORMAT_VERSION)
          needs_rebuild = true;
        continue;
      } else {
        needs_rebuild = true;
      }
    }

    // Remove newline
    size_t len = std::strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
      line[len - 1] = '\0';
      len--;
    }
    if (len > 0 && line[len - 1] == '\r') {
      line[len - 1] = '\0';
      len--;
    }

    // Format: path|title|author[|last_open_order[|progress_pct]]
    char* sep1 = std::strchr(line, '|');
    if (!sep1)
      continue;
    *sep1 = '\0';
    char* sep2 = std::strchr(sep1 + 1, '|');
    if (!sep2)
      continue;
    *sep2 = '\0';

    // Optional field 4: last_open_order. Older files appended reading statistics
    // after it; those are no longer tracked, and strtoul stops at the separator
    // so the extra columns are simply ignored.
    uint32_t order = 0;
    uint32_t pct = 0;
    char* sep3 = std::strchr(sep2 + 1, '|');
    if (sep3) {
      *sep3 = '\0';
      order = static_cast<uint32_t>(std::strtoul(sep3 + 1, nullptr, 10));
      // Optional field 5: cached reading percentage. Absent in files written by
      // older firmware, which simply read back as 0.
      char* sep4 = std::strchr(sep3 + 1, '|');
      if (sep4)
        pct = static_cast<uint32_t>(std::strtoul(sep4 + 1, nullptr, 10));
    }
    if (add_entry(line, sep1 + 1, sep2 + 1, order))
      entries_.back().progress_pct = static_cast<uint8_t>(pct > 100 ? 100 : pct);
  }

  std::fclose(f);
  dirty_ = false;
  if (needs_rebuild)
    return false;
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

  std::fprintf(f, "#wintergreen-index v%lu\n", (unsigned long)INDEX_FORMAT_VERSION);

  for (const auto& entry : entries_) {
    auto path_v = entry.path.view(pool_);
    auto title_v = entry.title.view(pool_);
    auto author_v = entry.author.view(pool_);
    std::fprintf(f, "%.*s|%.*s|%.*s|%u|%u\n",
                 static_cast<int>(path_v.size()), path_v.data(),
                 static_cast<int>(title_v.size()), title_v.data(),
                 static_cast<int>(author_v.size()), author_v.data(),
                 static_cast<unsigned>(entry.last_open_order),
                 static_cast<unsigned>(entry.progress_pct));
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
  return true;
}


void BookIndex::ensure_loaded_(const std::string& index_path) {
  if (!entries_.empty()) return;
  // entries_ is empty (e.g. MainMenu was stopped). Reload from disk so the
  // upcoming mutation merges with the existing on-disk state instead of
  // truncating the file. If the file doesn't exist, load() returns false and
  // entries_ stays empty — the caller's save() will then create it fresh.
  load(index_path);
}

bool BookIndex::index_file(const std::string& path, const std::string& index_path) {
  std::string title, author;
  {
    WgbReader r;
    if (!r.open(path.c_str())) {
      return false;
    }
    title = r.metadata().title;
    author = r.metadata().author.value_or("");
    r.close();
    // A book whose EPUB had no usable title would otherwise show as an empty
    // row; fall back to the folder name, which the converter takes from the
    // original filename.
    if (title.empty() || title == "none")
      title = folder_name_of(path);
  }
  ensure_loaded_(index_path);
  remove_entry(path);
  add_entry(path, title, author);
  ++generation_;
  return save(index_path);
}

bool BookIndex::remove_path(const std::string& path, const std::string& index_path) {
  ensure_loaded_(index_path);
  auto it = std::find_if(entries_.begin(), entries_.end(),
                         [&](const BookIndexEntry& e) { return e.path.view(pool_) == path; });
  if (it == entries_.end())
    return true;  // not indexed — nothing to do, no save needed
  entries_.erase(it);
  dirty_ = true;
  ++generation_;
  return save(index_path);
}

bool BookIndex::rename_in_place(const std::string& src, const std::string& dst, const std::string& index_path) {
  ensure_loaded_(index_path);
  auto it = std::find_if(entries_.begin(), entries_.end(),
                         [&](const BookIndexEntry& e) { return e.path.view(pool_) == src; });
  if (it == entries_.end())
    return false;  // src not indexed — caller may fall back to index_file(dst)
  it->path = pool_.add(dst);
  dirty_ = true;
  ++generation_;
  return save(index_path);
}

void BookIndex::remove_entry(std::string_view path) {
  auto it = std::find_if(entries_.begin(), entries_.end(),
                         [&](const BookIndexEntry& e) { return e.path.view(pool_) == path; });
  if (it != entries_.end()) {
    entries_.erase(it);
    dirty_ = true;
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
  int done = 0;

  auto process_path = [&](const std::string& path) {
    if (static_cast<int>(entries_.size()) >= MAX_BOOKS)
      return;

    std::string title, author;
    bool ok = false;
    {
      WgbReader r;
      if (r.open(path.c_str())) {
        title = r.metadata().title;
        author = r.metadata().author.value_or("");
        r.close();
        if (title.empty() || title == "none")
          title = folder_name_of(path);
        ok = true;
      }
    }

    if (ok) {
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
    done++;
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

#ifdef ESP_PLATFORM
      DIR* dir = opendir(current_dir.c_str());
      if (!dir) continue;
      struct dirent* ent;
      while ((ent = readdir(dir)) != nullptr) {
        std::string fullpath = current_dir + "/" + ent->d_name;
        visit(ent->d_name, ent->d_type == DT_DIR, fullpath);
      }
      closedir(dir);
#else
      try {
        for (const auto& entry : fs::directory_iterator(current_dir, fs::directory_options::skip_permission_denied)) {
          std::string fullpath = entry.path().string();
          for (char& c : fullpath) if (c == '\\') c = '/';
          std::string fname = entry.path().filename().string();
          visit(fname.c_str(), entry.is_directory(), fullpath);
        }
      } catch (...) {}
#endif
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
