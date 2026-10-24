#pragma once

#include <cstdint>

#include "TextLayout.h"

namespace wintergreen {

// The last few forward page turns, so turning back lands on exactly the page
// the reader saw rather than on a reconstruction of it (see
// TextLayout::predecessor_start for why those can differ).
//
// Fixed-size and allocation-free: 16 turns is ~260 bytes inside ReaderScreen,
// and it never touches the heap the reader's layout lives on.
class PageHistory {
 public:
  static constexpr uint8_t kCapacity = 16;

  void clear() {
    len_ = 0;
  }

  // A forward turn from the page at `start` to `next`, laid out under `epoch`.
  void push(uint32_t epoch, PagePosition start, PagePosition next) {
    if (len_ > 0 && epoch != epoch_)
      len_ = 0;
    epoch_ = epoch;
    head_ = static_cast<uint8_t>((head_ + 1) % kCapacity);
    turns_[head_] = Turn{start, next};
    if (len_ < kCapacity)
      ++len_;
  }

  // If the most recent forward turn led to `cur` under the same `epoch`, pop it
  // and return the page it came from. Anything else (a jump, a font change, a
  // turn older than the ring) clears the history and returns false.
  bool pop_to(uint32_t epoch, PagePosition cur, PagePosition* start) {
    if (len_ == 0 || epoch != epoch_ || turns_[head_].next != cur) {
      len_ = 0;
      return false;
    }
    *start = turns_[head_].start;
    head_ = static_cast<uint8_t>((head_ + kCapacity - 1) % kCapacity);
    --len_;
    return true;
  }

 private:
  struct Turn {
    PagePosition start, next;
  };
  Turn turns_[kCapacity];
  uint32_t epoch_ = 0;
  uint8_t head_ = 0;
  uint8_t len_ = 0;
};

}  // namespace wintergreen
