#pragma once

#include <cstdint>
#include <string>

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
    std::string home_path;   // exact-fit, blitted 1:1 (see kHomeCoverW in CoverPaths.h)
  };

  BookSlot slots_[kMaxBooks];
  int num_books_ = 0;

  // Cover of the selected book only — loaded on demand from draw_all_(), so it
  // is ready in the same frame the selection changes.
  //
  // It lives in DrawBuffer's spare framebuffer (claim_spare_cache), not on the
  // heap: a cover is ~26 KB, and a heap copy of it sat resident for as long as
  // the home screen did. Trusted only while spare_cache_valid(cover_gen_) — the
  // book list's thumbnails, the reader, the sleep path, a radio and the OOM
  // handler can all take the spare, and the next draw then reloads. With a radio
  // up there is no spare and the placeholder frame is drawn instead.
  //
  // There is exactly ONE cover, deliberately: preloading the neighbour was tried
  // and removed. See "Investigated and rejected" in docs/history.md.
  mutable const uint8_t* cover_bits_ = nullptr;
  mutable uint32_t cover_gen_ = 0;
  mutable uint16_t cover_w_ = 0, cover_h_ = 0;
  mutable int cover_slot_ = -1;  // slot the cover belongs to; -1 = nothing loaded

  // Wall time, not frames — see "Input and holds" in docs/power-and-input.md.
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

  // Reads the slot's cover_home.bin into the spare. The converter renders it to
  // fit the box (kHomeCoverW x kHomeCoverH), so this is a header and one fread —
  // no scaling on the device. A file larger than the box, a book without one,
  // or no spare to read into leaves no cover and draws the placeholder frame.
  void load_cover_(DrawBuffer& buf, int slot_idx, int box_w, int box_h) const;
  bool have_cover_(const DrawBuffer& buf) const {
    return cover_bits_ && cover_w_ > 0 && buf.spare_cache_valid(cover_gen_);
  }

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
