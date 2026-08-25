#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <string_view>

#include "StringPool.h"

namespace wintergreen {


struct BookIndexEntry {
  StringRef path{};
  StringRef title{};
  StringRef author{};
  uint32_t last_open_order = 0;  // 0 = never opened; higher = more recently opened
  // Reading position as a percentage, cached here purely so the book list can
  // show it without opening every WGB and .pos on the card. Written when the
  // reader closes; 0 for a book that was never opened.
  uint8_t progress_pct = 0;
};

static constexpr int MAX_BOOKS = 250;
static constexpr uint32_t INDEX_FORMAT_VERSION = 2;

// Case-insensitive title ordering, shared so the book list and the home carousel
// sort never-opened books identically.
bool ci_less(std::string_view a, std::string_view b);

class BookIndex {
 public:
  static BookIndex& instance();

  // True when the path is an already-converted book (no EPUB needed).
  static bool is_wgb_path(const char* path);

  bool load(const std::string& index_file);
  bool save(const std::string& index_file) const;

  // Recursively scan root_dir for EPUBs and rebuild the index.
  void build_index(const std::string& root_dir);

  const std::vector<BookIndexEntry>& entries() const { return entries_; }
  const StringPool& pool() const { return pool_; }

  // Drop the in-memory index and its string pool, actually returning the memory
  // rather than just clearing (a cleared vector keeps its capacity). ~30 KB for
  // a full library.
  //
  // Safe because an empty index is the "never loaded" state everywhere: every
  // screen reloads from disk when entries() is empty, and every mutator calls
  // ensure_loaded_() first, so nothing can truncate the file by saving over it.
  // Only worth calling when something else needs the heap more — see
  // Application::update's BLE case.
  void release_memory() {
    if (dirty_)
      return;  // unsaved changes would be lost; a reload cannot recover them
    std::vector<BookIndexEntry>().swap(entries_);
    pool_ = StringPool{};
  }

  // Bumped on every logical mutation (index_file / remove_path / rename_in_place /
  // build_index). MainMenu caches this to detect external changes. load() does not
  // bump.
  uint64_t generation() const { return generation_; }

  // Returns false (no-op) if MAX_BOOKS has been reached.
  bool add_entry(std::string_view path, std::string_view title, std::string_view author,
                 uint32_t last_open_order = 0);

  // Updates in-memory entry only; call save() to persist.
  // Mark `path` as the most recently opened book. The order counter is derived
  // from the entries themselves rather than persisted separately, so it cannot
  // drift out of step with the index.
  void mark_opened(std::string_view path);

  // Cache the reading percentage shown in the book list. Call save() to persist.
  void set_progress(std::string_view path, int pct);

  // No-op if not found. Call save() to persist.
  void remove_entry(std::string_view path);

  // Open path as an EPUB, upsert its index entry, and save to index_path.
  // Reloads from disk first if entries_ is empty, to avoid truncating the .dat.
  bool index_file(const std::string& path, const std::string& index_path);

  // Remove path's entry and save to index_path. No-op (returns true) if not indexed.
  // Reloads from disk first if entries_ is empty.
  bool remove_path(const std::string& path, const std::string& index_path);

  // Rename src→dst in-place, preserving metadata. Returns false if src not indexed
  // (caller may fall back to index_file). Reloads from disk first if entries_ is empty.
  bool rename_in_place(const std::string& src, const std::string& dst, const std::string& index_path);


 private:
  std::vector<BookIndexEntry> entries_;
  StringPool pool_;
  uint64_t generation_ = 0;
  // Set by any mutator that actually changes stored data; cleared by save() and
  // load(). Guards against rewriting an identical file — see save().
  mutable bool dirty_ = false;
  BookIndex() = default;

  // Loads from index_path if entries_ is empty, preventing .dat truncation
  // when a mutation arrives after MainMenu::stop() cleared in-memory state.
  void ensure_loaded_(const std::string& index_path);
};

}  // namespace wintergreen
