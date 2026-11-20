#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../Input.h"
#include "../content/StringPool.h"
#include "../display/DrawBuffer.h"
#include "../content/CoverPaths.h"
#include "ListMenuScreen.h"

namespace wintergreen {


// The book list: every book on the card, a Sync row on top, and the hidden
// books when the home screen's long-press asks for them.
class MainMenu final : public ListMenuScreen, public PixelSource {
 public:
  MainMenu() = default;

  void set_books_dir(const char* dir) {
    books_dir_ = dir;
  }

  // The full path of the currently highlighted entry (even if not yet opened).
  std::string current_book_path() const {
    const int idx = entries_index_for(selected());
    if (idx >= 0 && idx < static_cast<int>(entries_.size()))
      return std::string(path_of_(entries_[idx]));
    return {};
  }

  // Reveal the books under .hidden/ at the top of the list. Set by the home
  // screen's back long-press before it pushes this screen; cleared by stop(),
  // so it never survives leaving the list.
  void set_show_hidden(bool v) {
    show_hidden_ = v;
  }

  const char* books_dir() const {
    return books_dir_;
  }

  std::string_view get_item_label(int index) const override;
  std::string_view get_item_subtitle(int index) const override;
  std::string_view get_item_right(int index) const override;
  bool is_separator(int index) const override;
  // The Sync action never has an author line under it.
  bool is_single_line_row(int index) const override { return is_action_row_(index); }
  int count() const override;
  int prepare_thumbs(int box_h) const override;
  bool get_item_thumb(int index, Thumb& out) const override;
  // PixelSource: a thumbnail's columns, from the pack or the book's own file.
  bool read(uint32_t item, uint32_t offset, uint8_t* dst, size_t n) override;
  void release() override;

  void start(DrawBuffer& buf, IRuntime& runtime) override {
    buf_ = &buf;
    sync_runtime_ = &runtime;  // the first draw happens inside start(), before update()
    // Compiled out (no WG_WIFI_SYNC) the row would be a button that does nothing.
    has_sync_ = runtime.sync_state() != SyncState::Unavailable;
    ListMenuScreen::start(buf, runtime);
  }

  void stop() override;

  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

 protected:
  void on_start() override;
  void on_select(int index) override;
  void on_back() override;

 private:
  const char* books_dir_ = nullptr;
  std::string initial_selection_;   // path to pre-select after scan
  DrawBuffer* buf_ = nullptr;
  // Cached BookIndex::generation() value from the last populate_list_(). When
  // update() detects a mismatch, the index was mutated externally (a sync
  // while this screen is showing) and we refresh
  // the list in place without requiring the user to navigate away and back.
  uint64_t cached_generation_ = 0;

  // Indexed books reference BookIndex's pool for all three strings: a list
  // build used to allocate a std::string path per book, up to MAX_BOOKS of them.
  struct BookEntry {
    StringRef path_ref;
    StringRef title_ref;
    StringRef author_ref;
    uint32_t last_open_order = 0;
    uint8_t progress_pct = 0;
    int index_pos = -1;  // position in BookIndex::entries(): its thumbnail pack slot
    // Books under .hidden/ are not in BookIndex — there is no pool to reference,
    // so they carry their own strings and are read straight from the WGB.
    bool hidden = false;
    std::string path_own;
    std::string title_own;
    std::string author_own;
  };
  std::vector<BookEntry> entries_;
  mutable std::string right_buf_;

  std::string_view path_of_(const BookEntry& e) const;

  // Hairline divider positions, as visual indices.
  std::vector<int> separators_;


  // Number of separators with visual_index strictly less than v.
  int seps_before_(int v) const {
    int cnt = 0;
    for (const int s : separators_)
      if (s < v) cnt++;
    return cnt;
  }

  // With sync compiled in, row 0 is the Sync action and row 1 the hairline
  // under it; books start at 2. Both are visual-only, so every entries_ index
  // shifts by one on top of the separator shift.
  bool has_sync_ = false;
  int sync_rows_() const {
    return has_sync_ ? 1 : 0;
  }
  bool is_action_row_(int visual) const {
    return has_sync_ && visual == 0;
  }

  int entries_index_for(int visual) const {
    if (is_action_row_(visual)) return -1;
    return visual - seps_before_(visual) - sync_rows_();
  }

  int visual_for_entries(int real) const {
    int r = 0;
    for (int v = 0; v < count(); ++v) {
      if (is_separator(v) || is_action_row_(v)) continue;
      if (r == real) return v;
      ++r;
    }
    return count() - 1;
  }

  void scan_directory_();
  void populate_list_();
  // Land on the first book rather than on Sync when the screen opens. Called
  // after populate_list_() from the two paths that build the list on entry, so
  // an in-place refresh (sync) does not move the cursor.
  void select_first_book_();
  // NAS sync: hands off to IRuntime::start_sync() and returns.
  void run_sync_();
  // Reads <books_dir>/.hidden/ into hidden_. Only called when the gesture asks
  // for it, so the card is not walked on every visit to this screen.
  void scan_hidden_();

  std::vector<BookEntry> hidden_;
  bool show_hidden_ = false;

  // ── Cover thumbnails ──────────────────────────────────────────────────────
  // cover_list.bin, which the converter renders to fit the row's box
  // (kListCoverW x kListCoverH). There is no thumbnail cache: a row records a
  // blit of the file, and the refresh reads it a strip at a time — only when
  // the row is new on the glass or something over it changed (a cursor move
  // past it). What is kept is each book's header answer (where, how big, or
  // no cover), so a redraw opens nothing at record time.
  static constexpr int kThumbSlotsMax = 40;
  struct ThumbSlot {
    uint32_t key = 0;     // FNV-1a of the book path
    bool valid = false;
    bool packed = false;  // in the pack at pack_offset, else the book's own file
    uint32_t used = 0;    // last-use stamp, for eviction
    uint32_t pack_offset = 0;
    std::string_view path;  // the book (pool or hidden entry), for its own file
    uint8_t w = 0, h = 0;   // 0 = the book has no usable cover
  };
  mutable ThumbSlot thumb_slots_[kThumbSlotsMax];
  mutable uint32_t thumb_clock_ = 0;
  // Bumped when a book may have changed under its path (a sync): part of every
  // blit's identity, so an old cover is never taken for the new one.
  mutable uint32_t thumb_epoch_ = 0;
  // Open while a refresh reads (release() closes them).
  FILE* pack_ = nullptr;
  bool pack_tried_ = false;
  FILE* own_ = nullptr;
  int own_slot_ = -1;
  void load_thumb_(std::string_view book_path, int index_pos, ThumbSlot& slot) const;
  void forget_thumbs_() const;

  // Last sync state painted, so the Sync row repaints when it changes.
  SyncState sync_shown_ = SyncState::Unavailable;
  // Cached in update(): on_select() gets no runtime and the base class keeps
  // its own pointer private.
  IRuntime* sync_runtime_ = nullptr;

};

}  // namespace wintergreen
