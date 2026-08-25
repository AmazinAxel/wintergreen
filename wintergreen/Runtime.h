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
// Three states, and the UI shows two: "Disconnected" or the clicker's battery
// percentage. Connecting is only distinct so a second press during a connect is
// ignored rather than queued; it reads as Disconnected on screen, because to the
// user every not-connected state means the same thing — press it again.
//
// Failure detail deliberately does not appear here. It did while the feature
// was being brought up; see "The failure codes are gone" in CLAUDE.md.
enum class ClickerState : uint8_t {
  Unavailable = 0,
  Disconnected,
  Connecting,
  Connected,
};

// NAS sync, shown on the book list's Sync row. Same collapse as above: Idle and
// Failed both display as "Sync", since a failure and a fresh start call for the
// same action.
enum class SyncState : uint8_t {
  Unavailable = 0,
  Idle,
  Working,
  Done,
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

  // Clicker battery percentage, 0 when it has not reported one. Sampled once on
  // connect and then held: a number that drifts while you read is worse than a
  // slightly stale one.
  virtual uint8_t clicker_battery_pct() const {
    return 0;
  }

  // ── NAS book sync ─────────────────────────────────────────────────────────
  //
  // Like the clicker, the UI collapses this to what the user can act on: the
  // row reads "Sync" for both Idle and Failed, because the only useful response
  // to either is to press it again.
  virtual SyncState sync_state() const {
    return SyncState::Unavailable;
  }
  // Returns immediately; a sync takes seconds and runs on its own task. The
  // caller watches sync_state() for the outcome.
  virtual void start_sync() {}

  // TEMPORARY bring-up diagnostics: stage of the last sync failure and the free
  // internal RAM at that moment, shown on the Sync row. Remove both once the
  // sync is confirmed working on hardware — the row is a switch, not a status
  // console, exactly as with the clicker.
  virtual uint8_t sync_fail_stage() const {
    return 0;
  }
  virtual uint32_t sync_fail_heap_kb() const {
    return 0;
  }
};
}  // namespace wintergreen
