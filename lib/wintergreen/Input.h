#pragma once

#include <cstdint>

#include "WintergreenConfig.h"

namespace wintergreen {

enum class Button : uint8_t { Button0 = 0, Button1 = 1, Button2 = 2, Button3 = 3, Up = 4, Down = 5, Power = 6 };

struct ButtonState {
  static constexpr uint8_t kButtonCount = 7;
  static constexpr uint8_t kMaxPressHistory = 16;

  uint8_t current = 0;        // Instantaneous state at last sample
  uint8_t pressed_latch = 0;  // Accumulated rising edges since last poll (backward compat)

  // Ordered history of button presses since the last poll (oldest first).
  // Stores button indices (cast to Button). Capped at kMaxPressHistory entries;
  // excess events are silently dropped.
  uint8_t press_history[kMaxPressHistory] = {};
  uint8_t press_history_count = 0;

  bool is_down(Button button) const {
    const uint8_t mask = static_cast<uint8_t>(1u << static_cast<uint8_t>(button));
    return (current & mask) != 0;
  }

  bool is_pressed(Button button) const {
    const uint8_t mask = static_cast<uint8_t>(1u << static_cast<uint8_t>(button));
    return (pressed_latch & mask) != 0;
  }

  // Consume the next press event in arrival order. Returns false when exhausted.
  // The read cursor is mutable so this works on const ButtonState references.
  bool next_press(Button& out) const {
    if (press_history_pos_ >= press_history_count)
      return false;
    out = static_cast<Button>(press_history[press_history_pos_++]);
    return true;
  }

 private:
  mutable uint8_t press_history_pos_ = 0;
};

// Auto-repeat for a held button. Timings come from config::kHoldDelayMs and
// config::kHoldRepeatMs in WintergreenConfig.h.
//
// Repeats are driven by elapsed time, never by frame count. Frame duration varies
// with how much rendering a screen did and whether the panel was refreshing, so
// counting frames ties repeat speed to render speed: when refreshes stopped
// blocking, frames got ~10x shorter and a plain tap started registering as two
// presses. Feed this runtime.frame_time_ms() (the measured last frame).
struct HoldRepeat {
  static constexpr uint32_t kMaxPerTick = 4;

  uint32_t held_ms = 0;
  uint32_t fired = 0;  // repeats already emitted during this hold

  // True when the button is not currently held.
  bool idle() const {
    return held_ms == 0;
  }

  // Advance one frame. Returns how many repeats to apply now (0 on a tap).
  // May return >1 after a long frame, so a slow render cannot throttle the rate.
  int tick(bool down, uint32_t dt_ms) {
    if (!down) {
      held_ms = 0;
      fired = 0;
      return 0;
    }
    held_ms += dt_ms;
    if (held_ms < config::kHoldDelayMs)
      return 0;
    constexpr uint32_t kInterval = config::kHoldRepeatMs > 0 ? config::kHoldRepeatMs : 1;
    const uint32_t due = 1 + (held_ms - config::kHoldDelayMs) / kInterval;
    if (due <= fired)
      return 0;
    uint32_t n = due - fired;
    fired = due;  // skip any backlog rather than replaying it
    // A frame that ran long (image page, SD access) must not cash in as a burst of
    // repeats — that reads as a freeze followed by a jump.
    return static_cast<int>(n < kMaxPerTick ? n : kMaxPerTick);
  }
};

class IInputSource {
 public:
  virtual ~IInputSource() = default;
  virtual ButtonState poll_buttons() = 0;
};

}  // namespace wintergreen
