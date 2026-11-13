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
class MainMenu final : public ListMenuScreen {
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
  void end_thumbs() const override;

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
  // (kListCoverW x kListCoverH): a load is a header and one fread straight into
  // the slot, no scaling. Loaded the first time a row is drawn, during that draw,
  // so a page and its covers arrive in one refresh; nothing is read ahead. A
  // book without the file (no cover, or converted before it existed) draws an
  // empty frame.
  //
  // Slots live in DrawBuffer's spare framebuffer (claim_spare_cache): no
  // allocation, keyed by path, surviving list visits until the reader, the sleep
  // path or a radio claims the spare.
  static constexpr int kThumbSlotsMax = 40;
  static constexpr int kThumbMaxW = 96;
  struct ThumbSlot {
    uint32_t key = 0;     // FNV-1a of the book path
    bool valid = false;
    uint32_t used = 0;    // last-use stamp, for eviction
    uint8_t w = 0, h = 0; // 0 = the book has no usable cover
  };
  mutable uint8_t* thumb_bits_ = nullptr;  // trust only while spare_cache_valid(thumb_gen_)
  mutable uint32_t thumb_gen_ = 0;
  mutable int thumb_w_ = kListCoverW, thumb_h_ = kListCoverH;  // the box every slot is sized for
  mutable int thumb_slots_n_ = 0;
  mutable ThumbSlot thumb_slots_[kThumbSlotsMax];
  mutable uint32_t thumb_clock_ = 0;
  size_t thumb_slot_bytes_() const { return cover_cols_bytes(thumb_w_, thumb_h_); }
  // The thumbnail pack, open only for the duration of one draw (end_thumbs).
  mutable FILE* pack_ = nullptr;
  mutable bool pack_tried_ = false;
  bool load_packed_thumb_(int index_pos, uint32_t key, uint8_t* dst, ThumbSlot& slot) const;
  // Claims (or re-validates) the cache for a w x h box; false when the spare is
  // unavailable. Changing the box empties every slot.
  bool thumb_cache_ready_(int w, int h) const;
  ThumbSlot* find_thumb_(uint32_t key) const;
  uint8_t* thumb_slot_bits_(const ThumbSlot& s) const {
    return thumb_bits_ + static_cast<size_t>(&s - thumb_slots_) * thumb_slot_bytes_();
  }
  void load_thumb_(std::string_view book_path, int index_pos, uint8_t* dst, ThumbSlot& slot) const;
  void forget_thumbs_() const;  // a book may have changed under its path

  // Last sync state painted, so the Sync row repaints when it changes.
  SyncState sync_shown_ = SyncState::Unavailable;
  // Cached in update(): on_select() gets no runtime and the base class keeps
  // its own pointer private.
  IRuntime* sync_runtime_ = nullptr;

};

}  // namespace wintergreen
