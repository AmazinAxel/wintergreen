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

// FNV-1a of a book path: the thumbnail pack's per-slot check and the book
// list's thumbnail cache key.
inline uint32_t book_path_key(std::string_view path) {
  uint32_t key = 2166136261u;
  for (const char c : path)
    key = (key ^ static_cast<uint8_t>(c)) * 16777619u;
  return key;
}

// Thumbnail pack: every indexed book's cover_list.bin in one file beside the
// index, so the book list opens one file per draw instead of one per row (each
// open walks the card's root directory, which holds every book folder).
//
//   slot i (kThumbPackSlot bytes) = BookIndex entry i at the last save:
//     u32 book_path_key, u16 w, u16 h (0 = no cover), then
//     cover_cols_bytes(w, h) bytes as in cover_list.bin, padded.
//
// A cache only: written by save() when the set of books changed, never on the
// page-turn or reader-close path; a slot whose key does not match its book is
// ignored and the book's own cover_list.bin read instead.
std::string thumb_pack_path(const std::string& index_file);
size_t thumb_pack_slot_bytes();

// Case-insensitive title ordering, shared so the book list and the home carousel
// sort never-opened books identically.
bool ci_less(std::string_view a, std::string_view b);

// Name of the folder holding `path` ("…/the odyssey/book.wgb" → "the odyssey"),
// or "" when the file has no folder of its own.
std::string book_folder_name(const std::string& path);

// The title to show: the WGB's own, unless it is empty or the converter's
// literal "none" (an EPUB with no usable metadata), in which case the folder
// name — which the converter takes from the original filename. Every screen
// goes through this so none of them can disagree about a book's name.
std::string book_title_fallback(const std::string& title, const std::string& path);

// Title (via book_title_fallback) and author from the WGB header. False when
// the book does not open.
bool read_book_meta(const std::string& path, std::string& title, std::string& author);

class BookIndex {
 public:
  static BookIndex& instance();

  // True when the path is an already-converted book (no EPUB needed).
  static bool is_wgb_path(const char* path);

  bool load(const std::string& index_file);
  bool save(const std::string& index_file) const;

  // Recursively scan root_dir for .wgb books and rebuild the index.
  void build_index(const std::string& root_dir);

  const std::vector<BookIndexEntry>& entries() const { return entries_; }
  const StringPool& pool() const { return pool_; }

  // Bumped on every logical mutation (build_index, sync). MainMenu caches this to detect external changes. load() does not
  // bump.
  uint64_t generation() const { return generation_; }

  // For callers that batch several in-memory add_entry/remove_entry calls and
  // then save once — the sync does this to avoid rewriting the whole index per
  // book. Those mutators only set dirty_, so without this the screens never
  // notice and a freshly synced book does not appear until you navigate away
  // and back.
  void bump_generation() { ++generation_; }

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


 private:
  std::vector<BookIndexEntry> entries_;
  StringPool pool_;
  uint64_t generation_ = 0;
  // Set by any mutator that actually changes stored data; cleared by save() and
  // load(). Guards against rewriting an identical file — see save().
  mutable bool dirty_ = false;
  // Set when the set or order of entries changed (add, remove, rescan): the
  // thumbnail pack's slots no longer line up. Not set by progress or open order.
  mutable bool thumbs_dirty_ = false;
  // Books added since the pack was last written: their slots are read from
  // their own cover_list.bin, never copied from the old pack (a re-synced book
  // keeps its path but may bring a new cover).
  mutable std::vector<uint32_t> fresh_keys_;
  void save_thumbs_(const std::string& index_file) const;
  BookIndex() = default;
};

}  // namespace wintergreen
