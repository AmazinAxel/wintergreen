#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "ListMenuScreen.h"

namespace wintergreen {

// Home screen: a carousel of the most recently opened books.
//
// One book at a time — large cover, title and author, and a row of dots showing
// where it sits among the recents. Up/Down move between them, Select opens the
// book, Back opens the full book list (there is no separate Recents screen), and
// a long Back press opens Hidden Books.
class HomeScreen final : public ListMenuScreen, public PixelSource {
 public:
  HomeScreen() = default;

  void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const override;

  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

  // PixelSource: the cover of slot `item`, streamed while the panel refreshes.
  bool read(uint32_t item, uint32_t offset, uint8_t* dst, size_t n) override;
  void release() override;


 protected:
  void on_start() override;
  void on_select(int index) override;
  // Back never reaches the base class — update() resolves it on release.
  void on_back() override {}

 private:
  static constexpr int kMaxBooks = 5;

  // Views into the resident BookIndex pool, not copies: a home visit allocates
  // nothing. The pool is append-only between load()/build_index(), and this
  // screen re-runs on_start() (resume() = start()) every time it comes back on
  // top, after any of those.
  struct BookSlot {
    std::string_view path;
    std::string_view title;
    std::string_view author;
  };

  BookSlot slots_[kMaxBooks];
  int num_books_ = 0;

  // Cover of the selected book: its header, read on demand from draw_all_()
  // so the frame that changes the selection shows it. The pixels are never
  // held — no cover buffer at all: the refresh streams them from the card, and
  // only when the cover is new on the glass (an unchanged one is not re-read).
  //
  // There is exactly ONE cover, deliberately: preloading the neighbour was tried
  // and removed. See "Investigated and rejected" in docs/history.md.
  mutable uint16_t cover_w_ = 0, cover_h_ = 0;
  mutable int cover_slot_ = -1;  // slot the header belongs to; -1 = nothing loaded
  FILE* file_ = nullptr;         // open while a refresh reads the cover
  int file_slot_ = -1;

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

  // Reads the slot's cover_home.bin header. The converter renders it to fit the
  // box (kHomeCoverW x kHomeCoverH) — no scaling on the device. A file larger
  // than the box, or a book without one, draws the placeholder frame.
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
