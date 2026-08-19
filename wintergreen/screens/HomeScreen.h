#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ListMenuScreen.h"

namespace wintergreen {

// Home screen: a carousel of the most recently opened books.
//
// One book at a time — large cover, title and author, and a row of dots showing
// where it sits among the recents. Up/Down move between them, Select opens the
// book, Back opens the full book list (there is no separate Recents screen), and
// a long Back press opens Hidden Books.
class HomeScreen final : public ListMenuScreen {
 public:
  HomeScreen() = default;

  const char* name() const override { return "Home"; }

  void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const override;
  int get_visible_count_(int H, int scroll_off) const override { return count(); }

  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;


 protected:
  void on_start() override;
  void on_select(int index) override;
  // Back never reaches the base class — update() resolves it on release.
  void on_back() override {}

 private:
  static constexpr int kMaxBooks = 5;

  struct BookSlot {
    std::string path;
    std::string title;
    std::string author;
    std::string bin_path;
    std::string sleep_path;
  };

  BookSlot slots_[kMaxBooks];
  int num_books_ = 0;

  // Cover of the selected book only — loaded on demand from draw_all_(), so it
  // is ready in the same frame the selection changes.
  mutable std::vector<uint8_t> cover_data_;
  mutable uint16_t cover_w_ = 0, cover_h_ = 0;
  mutable int cover_slot_ = -1;  // slot cover_data_ belongs to; -1 = nothing loaded

  BitmapFont author_font_;

  // Wall time, not frames — see the note in CLAUDE.md.
  //
  // The list opens the instant this elapses, with the button still down, so the
  // number is how long the screen sits there doing nothing visible. At 3 s that
  // read as "it waits for the release"; a tap is well under 300 ms, so 1 s is
  // still unambiguous.
  static constexpr uint32_t kHiddenHoldMs = 1000;
  uint32_t back_hold_ms_ = 0;
  bool back_was_down_    = false;
  // Set on every start: MainMenu pops on the back *press*, so this screen comes
  // back with the button still down. Without swallowing that first hold, its
  // release would read as a fresh tap and re-open the list immediately.
  bool back_ignore_      = true;
  // The hold fires the moment it crosses kHiddenHoldMs, while the button is
  // still down; this stops the release from also opening the list.
  bool back_consumed_    = false;

  // Scales the slot's cover into a box_w × box_h box, preserving aspect.
  void load_cover_(int slot_idx, int box_w, int box_h) const;

  // Up↔Down and the two front nav keys, swapped: see update().
  static Button swap_updown_(Button b) {
    switch (b) {
      case Button::Up:      return Button::Down;
      case Button::Down:    return Button::Up;
      case Button::Button2: return Button::Button3;
      case Button::Button3: return Button::Button2;
      default:              return b;
    }
  }
};

}  // namespace wintergreen
