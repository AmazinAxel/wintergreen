#pragma once

#include <cstdint>
#include <optional>

#include "Input.h"

namespace wintergreen {

// State of the optional BLE page-turner clicker.
//
// Unavailable is the normal case: no MAC in config::kBluetoothPageTurner, or a
// platform with no radio. The quick menu hides its row entirely rather than
// showing a control that can never do anything.
//
// There is deliberately no state that survives a sleep. Deep-sleep wake is a
// full boot, so the device always comes up Disconnected with the radio cold —
// the radio is by far the largest current draw available to this firmware, and
// nothing turns it on except the user.
// NotFound and Failed are both "off" — the radio is down and toggling starts a
// fresh attempt. They exist because the two failures need completely different
// things from the user (switch the clicker on / unpair it from whatever else
// holds it), and this firmware has no log to say which happened.
enum class ClickerState : uint8_t {
  Unavailable = 0,
  Disconnected,
  Connecting,
  Connected,
  NotFound,
  Failed,
};

class IRuntime {
 public:
  virtual ~IRuntime() = default;

  // Measured duration of the last frame. Never the nominal target: Application
  // accumulates this into the auto-sleep countdown, and HoldRepeat uses it as
  // dt, so a constant here makes both drift whenever a frame overruns.
  virtual uint32_t frame_time_ms() const = 0;
  virtual void wait_next_frame() = 0;

  // Read battery level (0-100 percentage).
  // Returns empty optional if the platform does not have a battery.
  virtual std::optional<uint8_t> battery_percentage() const = 0;

  virtual int battery_millivolts() const {
    return 0; // cant be read/no battery/uncalibrated ADC
  }

  // Pin the CPU at its maximum frequency while the UI is actually working to keep refreshes fast
  virtual void set_performance_hold(bool) {}

  // ── BLE page-turner clicker ───────────────────────────────────────────────
  virtual ClickerState clicker_state() const {
    return ClickerState::Unavailable;
  }
  // Turn the radio on and connect when off; disconnect and power the radio down
  // when on. Returns immediately — connecting takes seconds and must not block
  // the UI — so the caller watches clicker_state() for the outcome.
  virtual void toggle_clicker() {}
  // Platform-specific reason code for the last failure, 0 when there is none.
  // Shown beside "Failed" in the quick menu — with no logging anywhere in this
  // firmware, that row is the only channel a diagnosis can travel down.
  virtual int clicker_status_code() const {
    return 0;
  }

  // Why the device last restarted, when that was *abnormal* — a panic, a
  // watchdog or a brownout — and 0 for the ordinary causes (power-on, deep-sleep
  // wake, a flash). Shown in the quick menu beside the clicker row.
  //
  // A device that resets has no way to tell anyone why: the panic text goes to a
  // console this firmware does not enable, and the reset wipes any state that
  // could have recorded it. esp_reset_reason() survives the reset, so it is the
  // one thing that can be reported afterwards. Distinguishing a brownout from a
  // stack overflow matters enormously and is otherwise pure guesswork.
  virtual int last_reset_reason() const {
    return 0;
  }
};
}  // namespace wintergreen
