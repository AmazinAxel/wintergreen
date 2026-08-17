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
class LyraExtScreen final : public ListMenuScreen {
 public:
  LyraExtScreen() = default;

  const char* name() const override { return "Home"; }

  void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const override;
  int get_visible_count_(int H, int scroll_off) const override { return count(); }

  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

 protected:
  void on_start() override;
  void on_select(int index) override;
  void on_back() override {}

 private:
  static constexpr int kMaxBooks = 5;

  struct BookSlot {
    std::string path;
    std::string title;
    std::string author;
    std::string bin_path;
  };

  BookSlot slots_[kMaxBooks];
  int num_books_ = 0;

  // Cover of the selected book only — loaded on demand from draw_all_(), so it
  // is ready in the same frame the selection changes.
  mutable std::vector<uint8_t> cover_data_;
  mutable uint16_t cover_w_ = 0, cover_h_ = 0;
  mutable int cover_slot_ = -1;  // slot cover_data_ belongs to; -1 = nothing loaded

  BitmapFont author_font_;

  static constexpr int kHiddenHoldFrames = 60;  // ~1.5 s at a 25 ms frame
  int  back_hold_frames_ = 0;
  bool back_was_down_    = false;

  // Scales the slot's cover into a box_w × box_h box, preserving aspect.
  void load_cover_(int slot_idx, int box_w, int box_h) const;
};

}  // namespace wintergreen
