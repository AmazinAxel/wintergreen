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
  std::string wintergreen_header_left() const override;
  bool is_separator(int index) const override;
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
  bool needs_scan_ = false;
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
    bool mrb_exists = false;
  };
  std::vector<BookEntry> entries_;
  mutable std::string label_buf_;
  mutable std::string subtitle_buf_;

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

  int entries_index_for(int visual) const {
    return visual - seps_before_(visual);
  }

  int visual_for_entries(int real) const {
    int r = 0, v = 0;
    while (v < count()) {
      while (v < count() && is_separator(v)) v++;
      if (v >= count()) break;
      if (r == real) return v;
      r++; v++;
    }
    return count() - 1;
  }

  void scan_directory_(DrawBuffer& buf);
  void populate_list_();

  // Back-button long-press state for hidden books gesture.
  // Frames held; on release, short=Settings, long=HiddenBooks.
  static constexpr int kHiddenHoldFrames = 15;  // ~3s at typical e-ink frame rate
  int back_hold_frames_ = 0;
  bool back_was_down_ = false;
};

}  // namespace wintergreen
