#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

#include "../Input.h"
#include "../content/StringPool.h"
#include "../display/DrawBuffer.h"
#include "ListMenuScreen.h"

namespace wintergreen {


// Main screen — lists EPUB books from a directory.
// Button1 = open book, Button0 = settings.
class MainMenu final : public ListMenuScreen {
 public:
  MainMenu() = default;

  void set_books_dir(const char* dir) {
    books_dir_ = dir;
  }

  // Restore the book list selection to the entry matching this path.
  // Call before start(); applied after directory scan.
  void set_initial_selection(const char* path) {
    initial_selection_ = path ? path : "";
  }

  // The full path of the currently highlighted entry (even if not yet opened).
  const std::string& current_book_path() const {
    int idx = entries_index_for(selected());
    if (idx >= 0 && idx < static_cast<int>(entries_.size()))
      return entries_[idx].path;
    static const std::string kEmpty;
    return kEmpty;
  }

  // Reveal the books under .hidden/ at the top of the list. Set by the home
  // screen's back long-press before it pushes this screen; cleared by stop(),
  // so it never survives leaving the list.
  void set_show_hidden(bool v) {
    show_hidden_ = v;
  }

  bool has_books_dir() const {
    return books_dir_ != nullptr;
  }

  const char* books_dir() const {
    return books_dir_;
  }

  const char* name() const override {
    return "Books";
  }



  void set_app(Application* app) {
    app_ = app;
  }

  std::string_view get_item_label(int index) const override;
  std::string_view get_item_subtitle(int index) const override;
  std::string_view get_item_right(int index) const override;
  std::string wintergreen_header_left() const override;
  bool is_separator(int index) const override;
  // The Sync action never has an author line under it.
  bool is_single_line_row(int index) const override { return is_action_row_(index); }
  int count() const override;

  void start(DrawBuffer& buf, IRuntime& runtime) override {
    buf_ = &buf;
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
  // update() detects a mismatch, the index was mutated externally (e.g. by a
  // serial upload/delete/rename while this screen is showing) and we refresh
  // the list in place without requiring the user to navigate away and back.
  uint64_t cached_generation_ = 0;

  struct BookEntry {
    std::string path;
    StringRef title_ref;
    StringRef author_ref;
    uint32_t last_open_order = 0;
    uint8_t progress_pct = 0;
    // Books under .hidden/ are not in BookIndex — there is no pool to reference,
    // so they carry their own metadata and are read straight from the WGB.
    bool hidden = false;
    std::string title_own;
    std::string author_own;
  };
  std::vector<BookEntry> entries_;
  mutable std::string subtitle_buf_;
  mutable std::string right_buf_;

  // Labeled section separators: {visual_index, label}.
  // Inserted by populate_list_() for LastOpened sort ("Recents", "All Books").
  std::vector<std::pair<int, std::string>> separators_;


  // Number of separators with visual_index strictly less than v.
  int seps_before_(int v) const {
    int cnt = 0;
    for (const auto& s : separators_)
      if (s.first < v) cnt++;
    return cnt;
  }

  // Row 0 is the Sync action, row 1 the hairline under it; books start at 2.
  // Both are visual-only, so every entries_ index shifts by one on top of the
  // separator shift.
  static constexpr int kSyncRow = 0;
  static bool is_action_row_(int visual) {
    return visual == kSyncRow;
  }

  int entries_index_for(int visual) const {
    if (is_action_row_(visual)) return -1;
    return visual - seps_before_(visual) - 1;
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
  // an in-place refresh (serial upload) does not move the cursor.
  void select_first_book_();
  // NAS sync, the one thing kWifiSsid/kWifiPassword are reserved for. No Wi-Fi
  // subsystem exists yet, so this is the single hook that will start it.
  void run_sync_();
  // Reads <books_dir>/.hidden/ into hidden_. Only called when the gesture asks
  // for it, so the card is not walked on every visit to this screen.
  void scan_hidden_();

  std::vector<BookEntry> hidden_;
  bool show_hidden_ = false;

  // Last sync state painted, so the Sync row repaints when it changes.
  SyncState sync_shown_ = SyncState::Unavailable;
  // Cached in update(): on_select() gets no runtime and the base class keeps
  // its own pointer private.
  IRuntime* sync_runtime_ = nullptr;

};

}  // namespace wintergreen
