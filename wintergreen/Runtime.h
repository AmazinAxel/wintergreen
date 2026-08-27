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

  // Whether the BLE stack is currently holding heap. **Not the same as being
  // connected**: the NimBLE host and esp_hidh are initialised once per boot and
  // never deinitialised (deinit panics on this IDF), so they keep their
  // allocation across a disconnect. The reader sizes its caches off this, so
  // that a disconnect does not restore a full working set on top of a stack
  // that still owns its memory.
  virtual bool clicker_holds_ram() const {
    return false;
  }

  // Free heap, for callers deciding whether to do optional work. The reader
  // uses it to skip its speculative next-page layout when the margin is too
  // thin — that layout is what tips a long-paragraph book into an abort.
  // UINT32_MAX on a host build, where the question does not arise.
  virtual uint32_t free_memory_bytes() const {
    return UINT32_MAX;
  }

  // Largest single allocation the heap can currently satisfy.
  //
  // **This, not free_memory_bytes(), is what a guard before allocating must
  // use.** IDF's own header says so: total free size says nothing about whether
  // one block of that size exists, because the heap fragments. Guarding on the
  // total let a page layout through with 24 KB "free" and then abort on a
  // 1,416-byte request — reproducibly, on an ordinary forward page turn.
  //
  // UINT32_MAX on a host build.
  virtual uint32_t largest_free_block_bytes() const {
    return UINT32_MAX;
  }

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
};
}  // namespace wintergreen
