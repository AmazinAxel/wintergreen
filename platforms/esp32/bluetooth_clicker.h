#pragma once

// BLE page-turner clicker — HID-over-GATT host, off by default.
//
// The radio is the largest current draw this firmware can switch on: tens of
// milliamps against the ~15 mA the CPU idles at, on a device whose whole power
// budget is "what fraction of the time is the CPU awake". So the design is
// aggressively conservative about ever having it powered:
//
//  - **Compiled out entirely** unless WG_BLUETOOTH_PAGE_TURNER is defined. The
//    linker then drops the whole NimBLE stack, and the quick menu never shows
//    the row. This is the default, and a build with no MAC configured is byte
//    for byte the build that existed before the feature.
//  - **Off at every boot.** There is no persisted "was connected" flag. Deep
//    sleep wake is a full boot, so putting the device down and picking it up
//    always comes back with the radio cold. Nothing turns it on but the user.
//  - **Idle after any failure.** A refused connection, a connection that never
//    completes, or a clicker that walks out of range all drop straight back to
//    Disconnected. There is no retry loop and no reconnect timer — a radio
//    scanning for a device that is not there is exactly the battery leak this
//    avoids.
//
// Note "off" above means *before the first connect of a boot* and *after the
// device sleeps*, which is the state it spends nearly all its time in. Once the
// stack has been brought up it stays up for the rest of the session; see "The
// stack comes up once per boot" for the panic that forced that.
//
// There is no pairing screen. Turning it on scans for a few seconds, connects
// to the clicker, bonds, and that is the whole interaction.
//
// **It scans rather than connecting straight to the configured MAC**, and that
// is load-bearing rather than a nicety — see the scan block below. A BLE
// address does not carry its own type, and HID devices routinely advertise a
// resolvable private address that rotates every ~15 minutes, so the address in
// the config is a *filter*, not a destination.
//
// Threading: esp_hidh_dev_open() blocks for the length of a connection attempt,
// and the teardown calls block too, so both run on a short-lived worker task
// rather than on the UI loop. state_ is the only thing shared with the UI, and
// the UI only ever reads it. The HID event callback runs on esp_hidh's own task
// and never tears the stack down from inside itself — it raises
// teardown_request_ and lets poll() do it, because deinitialising a stack from
// within its own event callback deadlocks.

#include <cstdint>
#include <cstdio>

#include "WintergreenConfig.h"
#include "wintergreen/Input.h"
#include "wintergreen/Runtime.h"

namespace wg_clicker {

// Presses injected by the clicker, merged by Esp32InputSource::poll_buttons in
// exactly the way serial-injected presses are. A bitmask of Button indices.
inline volatile uint8_t g_clicker_buttons = 0;

// HID keyboard usage IDs (HID Usage Table, keyboard page 0x07).
//
// The mapping is the one asked for and is deliberately the opposite of a
// presentation remote's convention: **left advances**, right goes back. Swap
// these two lines to flip it. Page Up / Page Down are accepted alongside the
// arrows because clickers disagree about which pair they send, and a clicker
// that sends both is not a case worth distinguishing.
inline constexpr uint8_t kKeyRightArrow = 0x4F;
inline constexpr uint8_t kKeyLeftArrow = 0x50;

// Button2 is "next page" and Button3 is "previous page" in the reader, in both
// orientations (unlike the side rocker, which swaps — see CLAUDE.md). On any
// other screen they are list navigation, which is a sane thing for a clicker to
// do rather than a special case worth writing.
inline constexpr uint8_t kBtnNext = static_cast<uint8_t>(wintergreen::Button::Button2);
inline constexpr uint8_t kBtnPrev = static_cast<uint8_t>(wintergreen::Button::Button3);

}  // namespace wg_clicker

#ifdef WG_BLUETOOTH_PAGE_TURNER

#include "esp_bt.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_hidh.h"
#include "esp_hidh_nimble.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

extern "C" void ble_store_config_init(void);

namespace wg_clicker {

inline volatile wintergreen::ClickerState g_state = wintergreen::ClickerState::Disconnected;
// Set by the HID event callback, acted on by poll() from the UI loop.
inline volatile bool g_teardown_request = false;
// True while a worker task owns the stack; a second toggle is ignored rather
// than queued, so mashing the row cannot start two connects.
inline volatile bool g_busy = false;

inline esp_hidh_dev_t* g_dev = nullptr;
// Last keycode seen in an input report, so a held key does not repeat and a
// release report (all zeros) is not read as a press.
inline uint8_t g_last_key = 0;

// Which step failed. Reported as the number after "Failed" in the quick menu,
// so a failure names its own stage instead of collapsing every cause into one
// word. Without this, "could not allocate the BLE controller" and "the peer
// refused the connection" look identical from the outside — which is exactly
// where this feature got stuck.
enum FailStage : int {
  kFailNone = 0,
  kFailMac = 1,        // WG_BLUETOOTH_PAGE_TURNER is not a MAC
  kFailNvs = 2,        // bond storage unavailable
  kFailCtrlInit = 3,   // esp_bt_controller_init — usually heap
  kFailCtrlEnable = 4,
  kFailNimbleInit = 5,
  kFailHidhInit = 6,
  kFailNimbleEnable = 7,
  kFailSync = 8,       // host never synced with the controller
  kFailScanStart = 9,  // ble_gap_disc / ble_hs_id_infer_auto refused
  kFailOpen = 10,      // found it, GAP connect failed — see g_last_gap_status
  kFailHeap = 11,      // not enough internal RAM to bring the radio up
};

// Internal RAM the stack needs before it is worth starting, in KB. The
// controller, the NimBLE mempools, the host task and esp_hidh's event task come
// to roughly this much, all of it MALLOC_CAP_INTERNAL.
//
// Checking first turns "half-initialise, fail at stage 8, and leave the session
// unable to try again" into a clean, retryable refusal that names the number.
//
// Deliberately well below what the stack actually wants. This is a floor for
// the hopeless case (a stage-8 failure reported 3 KB free), not an estimate of
// the requirement — a threshold set at the real figure refuses attempts that
// would have succeeded, which is a worse failure than the one it prevents.
inline constexpr size_t kMinHeapKb = 24;
inline volatile int g_fail_stage = kFailNone;

// ── The stack comes up once per boot and is never torn down ────────────────
//
// **Do not add a deinit path back.** Cycling the NimBLE stack is not supported
// by this IDF, and it fails by crashing rather than by returning an error:
//
//   assert failed: ble_hs_init at ble_hs.c:928
//     esp_nimble_init  (nimble_port.c:200)
//     wg_clicker::bringup_stack()
//
// Line 928 is `SYSINIT_PANIC_ASSERT(rc == 0)` on the `os_mempool_init` of
// `ble_hs_hci_ev_pool`. With `CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC=y` (which is
// set here) `ble_hs_ctx` and its `hci_os_event_buf` are heap-allocated and
// guarded by `if (!ble_hs_ctx)`, and `esp_nimble_deinit()` does not restore
// those pointers — so a second `esp_nimble_init()` builds the pool on freed
// state and panics.
//
// Two earlier attempts at "tear it down properly" produced two different
// bugs — a permanent stage-6 lockout, then this panic — before it became clear
// the whole approach was wrong. IDF's own examples init the stack once at boot
// and never deinit; this now matches that.
//
// A third variant of the same panic came from guarding bringup with a single
// "fully up" flag set at the end: a bringup that reached esp_nimble_init() and
// then failed at the sync step left that flag false, so the retry re-ran the
// one call that cannot be repeated. Hence a flag per step below.
//
// **What this costs.** Toggling the clicker off closes the connection and stops
// all radio activity, but the controller stays initialised, so idle BLE current
// remains until the device sleeps. That is acceptable here precisely because
// deep-sleep wake is a full boot: `kAutoSleepMinutes` is 1, so putting the
// device down clears the whole stack within a minute, and the radio is only
// ever up during an awake session in which the user explicitly asked for it.
inline bool g_ctrl_up = false;    // esp_bt_controller_init + enable
inline bool g_nimble_up = false;  // esp_nimble_init — the one that panics twice
inline bool g_hidh_up = false;    // esp_hidh_init
inline bool g_host_up = false;    // esp_nimble_enable + host synced

// ── MAC parsing ────────────────────────────────────────────────────────────
// "AA:BB:CC:DD:EE:FF" → 6 bytes, **reversed**.
//
// NimBLE's ble_addr_t.val is little-endian: val[5] is the most significant
// byte, the one a MAC is written leftmost. ble_gap.c's own address-type checks
// read `addr[5] & 0xC0`, which is the giveaway. esp_hidh_dev_open() memcpy's
// this array straight into ble_addr_t.val, so it must already be in that order.
//
// Getting this wrong is silent: the stack happily tries to connect to the
// byte-reversed address, finds nothing, and times out after 30 seconds looking
// exactly like a clicker that is switched off.
//
// Anything unparseable is treated as unconfigured, so a typo fails to connect
// rather than reaching some other device.
inline bool parse_mac(const char* s, uint8_t out[6]) {
  unsigned v[6];
  if (std::sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
    return false;
  for (int i = 0; i < 6; ++i) {
    if (v[i] > 0xFF)
      return false;
    out[5 - i] = static_cast<uint8_t>(v[i]);
  }
  return true;
}

inline void queue_press(uint8_t btn) {
  g_clicker_buttons = static_cast<uint8_t>(g_clicker_buttons | (1u << btn));
}

// Map one HID input report onto a page turn.
//
// The report is scanned for a keycode rather than parsed against the device's
// report map: a clicker sends one key at a time, the keycodes we care about are
// implausible as modifier or report-ID bytes, and a permissive scan works
// across the boot-protocol and report-protocol layouts that clickers disagree
// about. esp_hidh has already done the hard part (discovery, report map,
// notification subscription); this only has to recognise four numbers.
inline void handle_report(const uint8_t* data, uint16_t len) {
  uint8_t found = 0;
  for (uint16_t i = 0; i < len; ++i) {
    const uint8_t k = data[i];
    if (k == kKeyLeftArrow || k == kKeyRightArrow) {
      found = k;
      break;
    }
  }
  // Fire on the transition only: a key held down re-notifies, and the release
  // report is all zeros.
  if (found != 0 && found != g_last_key)
    queue_press((found == kKeyLeftArrow) ? kBtnNext : kBtnPrev);
  g_last_key = found;
}

inline void hidh_event_cb(void* handler_args, esp_event_base_t base, int32_t id, void* event_data) {
  (void)handler_args;
  (void)base;
  auto* p = static_cast<esp_hidh_event_data_t*>(event_data);
  switch (static_cast<esp_hidh_event_t>(id)) {
    case ESP_HIDH_OPEN_EVENT:
      if (p->open.dev) {
        g_dev = p->open.dev;
        g_state = wintergreen::ClickerState::Connected;
      }
      break;
    case ESP_HIDH_INPUT_EVENT:
      // **Do not use p->input.data.** nimble_hidh.c allocates one block holding
      // the event struct followed by the payload, points input.data at the
      // payload, posts the block to the event loop — which *copies* it — and
      // then frees the original. So the pointer that arrives here refers to
      // freed heap. The copy has the payload immediately after the struct, at a
      // known offset, so recompute it rather than trusting the pointer.
      if (g_state == wintergreen::ClickerState::Connected) {
        const auto* payload = reinterpret_cast<const uint8_t*>(p) + sizeof(esp_hidh_event_data_t);
        handle_report(payload, p->input.length);
      }
      break;
    case ESP_HIDH_CLOSE_EVENT:
      // The device must be freed from here; nothing else may.
      if (p->close.dev) {
        esp_hidh_dev_free(p->close.dev);
        if (p->close.dev == g_dev)
          g_dev = nullptr;
      }
      // The clicker went away. Do not reconnect — power the radio down.
      g_teardown_request = true;
      break;
    default:
      break;
  }
}

// ── Scan ───────────────────────────────────────────────────────────────────
// Connecting straight to the configured address does not work in general, and
// this is not a shortcut around a slow path — it is the only correct way in.
//
// Two reasons. A BLE address carries a *type* (public / random) that the
// address alone cannot tell you, and guessing costs 30 s per wrong guess
// because ble_gap_connect blocks for its whole timeout. And many HID devices
// advertise a **resolvable private address**, which rotates every ~15 minutes:
// `(addr[5] & 0xC0) == 0x40` identifies one, and the MAC a laptop showed you an
// hour ago is simply not that device's address any more.
//
// So: scan, take the address and type that are true *now*, then connect to
// exactly that. A device that is switched off or not advertising is discovered
// in seconds instead of two 30 s timeouts.

inline constexpr uint16_t kHidServiceUuid = 0x1812;
// Kept short on purpose. Many clickers advertise in a burst after a keypress
// and then go quiet, so every second spent scanning is a second in which the
// device we just found may stop being connectable. A MAC match cancels the scan
// immediately; only the HID-service fallback waits out the full window, because
// a later MAC match should still win over some other HID device in the room.
inline constexpr int32_t kScanMs = 5000;

inline uint8_t g_want_mac[6] = {};
inline ble_addr_t g_target{};
inline volatile bool g_have_target = false;   // exact MAC match
inline volatile bool g_have_hid = false;      // fallback: something advertising HID
inline ble_addr_t g_hid_candidate{};
inline volatile bool g_scan_done = false;

inline bool adv_has_hid_service(const struct ble_gap_disc_desc* d) {
  struct ble_hs_adv_fields f;
  if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0)
    return false;
  for (int i = 0; i < f.num_uuids16; ++i) {
    if (ble_uuid_u16(&f.uuids16[i].u) == kHidServiceUuid)
      return true;
  }
  return false;
}

inline int scan_cb(struct ble_gap_event* ev, void* arg) {
  (void)arg;
  switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
      if (memcmp(ev->disc.addr.val, g_want_mac, 6) == 0) {
        g_target = ev->disc.addr;  // address *and* the type it really uses
        g_have_target = true;
        ble_gap_disc_cancel();
        g_scan_done = true;
      } else if (!g_have_hid && adv_has_hid_service(&ev->disc)) {
        g_hid_candidate = ev->disc.addr;
        g_have_hid = true;
      }
      break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
      g_scan_done = true;
      break;
    default:
      break;
  }
  return 0;
}

// Returns true and fills g_target when something worth connecting to was found.
inline bool scan_for_clicker() {
  g_have_target = false;
  g_have_hid = false;
  g_scan_done = false;

  uint8_t own_addr_type = 0;
  if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
    g_fail_stage = kFailScanStart;
    return false;
  }

  struct ble_gap_disc_params params = {};
  params.passive = 0;   // active: some clickers put the HID UUID in the scan response
  params.filter_duplicates = 1;

  if (ble_gap_disc(own_addr_type, kScanMs, &params, scan_cb, nullptr) != 0) {
    g_fail_stage = kFailScanStart;
    return false;
  }

  for (int waited = 0; !g_scan_done && waited < kScanMs + 2000; waited += 50)
    vTaskDelay(pdMS_TO_TICKS(50));

  // The scan must be fully stopped before connecting — ble_gap_connect returns
  // BLE_HS_EBUSY while discovery is active, and the cancel issued from inside
  // the discovery callback is not necessarily complete by the time this task
  // wakes. Poll rather than assume.
  ble_gap_disc_cancel();
  for (int waited = 0; ble_gap_disc_active() && waited < 1000; waited += 20)
    vTaskDelay(pdMS_TO_TICKS(20));

  if (g_have_target)
    return true;
  // Nothing at the configured address, but something is advertising as a HID
  // device. That is almost always the clicker behind a rotated private address,
  // and connecting to it is what makes the feature work at all for such a
  // device. It bonds on connect, so this is a one-time detour.
  if (g_have_hid) {
    g_target = g_hid_candidate;
    return true;
  }
  return false;
}

// ── Failure reason ─────────────────────────────────────────────────────────
// esp_hidh swallows the reason a connection failed: esp_ble_hidh_dev_open()
// stores it in dev->status and then frees dev before returning NULL, and every
// ESP_LOGE inside NimBLE is compiled out here (see "No logging"). So a failed
// connect is indistinguishable from any other failed connect — which is exactly
// the position this feature was stuck in.
//
// A global GAP event listener sees every GAP event regardless of which callback
// owns the connection, so it can record the status without interfering with
// esp_hidh's own handler. The code is shown in the quick-menu row, which is the
// only diagnostic channel this firmware has.
//
// Common values (host/ble_hs.h): 0x02 BLE_HS_EALREADY, 0x03 BLE_HS_EBUSY,
// 0x07 BLE_HS_ENOMEM, 0x0D BLE_HS_ETIMEOUT, 0x0E BLE_HS_EDONE. Anything above
// 0x200 is BLE_HS_HCI_ERR(x) — a controller/peer rejection, where x is the HCI
// error: 0x3E connection-establishment-failed, 0x08 connection-timeout,
// 0x13/0x16 remote-terminated.
inline volatile int g_last_gap_status = 0;
inline struct ble_gap_event_listener g_gap_listener;
inline bool g_gap_listener_registered = false;


inline int gap_listener_cb(struct ble_gap_event* ev, void* arg) {
  (void)arg;
  if (ev->type == BLE_GAP_EVENT_CONNECT && ev->connect.status != 0)
    g_last_gap_status = ev->connect.status;
  else if (ev->type == BLE_GAP_EVENT_DISCONNECT)
    g_last_gap_status = ev->disconnect.reason;
  return 0;
}

inline void host_task(void* param) {
  (void)param;
  nimble_port_run();  // returns only after nimble_port_stop()
  nimble_port_freertos_deinit();
}

// ── Bring-up / disconnect ──────────────────────────────────────────────────
// Both run on the worker task, because both block.

// Drop the connection and stop radio activity, leaving the stack initialised.
//
// This is deliberately **not** a teardown — see "The stack comes up once per
// boot" above for the panic that made it one. Only what can be undone safely is
// undone; the rest stays up until the device next sleeps, which is a full boot.
inline void disconnect_only() {
  // Only touch NimBLE once the host is actually running. This is reached from
  // the failure path of bringup_stack() too, which can bail out *before*
  // esp_nimble_init() — and ble_gap_disc_cancel() then dereferences a null
  // ble_hs_ctx inside ble_hs_is_enabled(), which is a load fault, not an error
  // return. Symptom: selecting the row reboots the device instantly, before
  // "Connecting" is ever drawn.
  if (g_host_up)
    ble_gap_disc_cancel();  // no-op when not scanning
  if (g_dev) {
    esp_hidh_dev_close(g_dev);
    g_dev = nullptr;
  }
  g_last_key = 0;
  g_state = wintergreen::ClickerState::Disconnected;
}

inline bool bringup_stack() {
  // Each step guarded separately, not one flag at the end.
  //
  // A single "fully up" flag is not enough and the difference is a panic: a
  // bringup that got as far as esp_nimble_init() and then failed at the sync
  // step would leave the flag false, so the next attempt re-ran
  // esp_nimble_init() — the one call that cannot be repeated. Every step below
  // therefore records its own success and is skipped on a retry.

  // Refuse before allocating anything if there is plainly not enough internal
  // RAM. Reports free KB, so "Failed 11xxx" says how far short it was.
  if (!g_ctrl_up) {
    const size_t free_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    if (free_kb < kMinHeapKb) {
      g_fail_stage = kFailHeap;
      g_last_gap_status = static_cast<int>(free_kb);
      return false;
    }
  }

  // NVS is initialised here rather than at boot: it backs the bond store and
  // nothing else in this firmware uses it, so a device with the radio off never
  // pays for it. Safe to call on every connect — a second nvs_flash_init() on an
  // already-initialised partition returns ESP_OK.
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs = nvs_flash_init();
  }
  if (nvs != ESP_OK) {
    g_fail_stage = kFailNvs;
    return false;
  }

  if (!g_ctrl_up) {
    // Hands the (nonexistent, on a C3) Classic BT allocation back to the heap.
    // Harmless where there is none, and the IDF example does it before init.
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&cfg) != ESP_OK) {
      g_fail_stage = kFailCtrlInit;
      return false;
    }
    if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) {
      g_fail_stage = kFailCtrlEnable;
      return false;
    }
    g_ctrl_up = true;
  }

  if (!g_nimble_up) {
    if (esp_nimble_init() != ESP_OK) {
      g_fail_stage = kFailNimbleInit;
      return false;
    }
    g_nimble_up = true;
  }

  if (!g_hidh_up) {
    esp_hidh_config_t hidh_cfg = {};
    hidh_cfg.callback = hidh_event_cb;
    // esp_hidh dispatches OPEN/INPUT/CLOSE on this task; 4096 is the example's
    // value and leaves little room once a report map is being walked.
    hidh_cfg.event_stack_size = 6144;
    hidh_cfg.callback_arg = nullptr;
    if (esp_hidh_init(&hidh_cfg) != ESP_OK) {
      g_fail_stage = kFailHidhInit;
      return false;
    }
    g_hidh_up = true;
  }

  // Bonding keys live in the nvs partition; without this the clicker has to
  // re-pair on every connect and some refuse to. store_status_cb handles a full
  // bond store by evicting the oldest entry — without it, pairing fails outright
  // once the store fills.
  ble_store_config_init();
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

  // Security manager. **None of this is optional** — every field below defaults
  // to 0 in NimBLE (MYNEWT_VAL_BLE_SM_BONDING and both KEY_DIST are `(0)`), and
  // the HID service's characteristics require an encrypted link, so leaving the
  // defaults produces a device that connects at the link layer and then reads
  // nothing.
  //
  //  - Just Works pairing: a clicker has no display and no keypad, so
  //    NO_INPUT_OUTPUT is the only honest capability and MITM protection is not
  //    achievable. This is a page-turner, not a bank.
  //  - Bonding on, with keys distributed both ways, so the pairing survives a
  //    reboot (they land in NVS via ble_store_config_init above).
  //  - **The ID key matters more than usual here.** It carries the peer's IRK,
  //    which is what lets the stack recognise a device whose advertised address
  //    rotates — see the scan block for why that is the normal case.
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  if (!g_gap_listener_registered &&
      ble_gap_event_listener_register(&g_gap_listener, gap_listener_cb, nullptr) == 0)
    g_gap_listener_registered = true;
  g_last_gap_status = 0;
  // esp_nimble_enable takes the host task as void* (it is handed straight to
  // nimble_port_freertos_init), hence the cast.
  // esp_nimble_enable ignores xTaskCreatePinnedToCore's return value and always
  // reports ESP_OK, so a host task that could not be created is indistinguishable
  // here from one that was — it shows up as a sync timeout below instead.
  if (!g_host_up && esp_nimble_enable(reinterpret_cast<void*>(host_task)) != ESP_OK) {
    g_fail_stage = kFailNimbleEnable;
    return false;
  }

  // Wait for the host to sync with the controller before opening anything.
  // 10 s rather than the 2 s this started with: the wait costs nothing when
  // sync is quick (which it normally is, ~100 ms), and a timeout that is merely
  // *tight* is the most misleading failure available — it looks exactly like a
  // stack that cannot start at all.
  for (int i = 0; i < 1000 && !ble_hs_synced(); ++i)
    vTaskDelay(pdMS_TO_TICKS(10));
  if (ble_hs_synced()) {
    g_host_up = true;
    return true;
  }
  g_fail_stage = kFailSync;
  // NimBLE allocates from internal RAM only (MEM_ALLOC_MODE_INTERNAL), and the
  // host task's stack is the last thing bringup asks for — a silent
  // xTaskCreate failure there is a prime suspect for never syncing. Report free
  // internal heap in KB so the next attempt says whether that is it.
  //
  // Note the stack stays half-up after this: there is no safe way to unwind it
  // (see above), so a failure here is terminal for the session. It has not been
  // observed since the sync wait went from 2 s to 10 s.
  g_last_gap_status = static_cast<int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
  return false;
}

inline void connect_task(void* param) {
  (void)param;
  g_fail_stage = kFailNone;
  g_last_gap_status = 0;

  uint8_t mac[6];
  bool ok = true;
  if (!parse_mac(WG_BLUETOOTH_PAGE_TURNER, mac)) {
    g_fail_stage = kFailMac;
    ok = false;
  }
  if (ok)
    ok = bringup_stack();  // sets g_fail_stage itself

  wintergreen::ClickerState outcome = wintergreen::ClickerState::Failed;

  if (ok) {
    memcpy(g_want_mac, mac, 6);
    if (!scan_for_clicker()) {
      // A scan that ran and found nothing is "Not found": the clicker is off,
      // asleep, or already bound to something else. A scan that could not be
      // *started* is a different animal and keeps its Failed stage — collapsing
      // the two would send the user off looking at the clicker when the fault
      // is here.
      if (g_fail_stage == kFailNone)
        outcome = wintergreen::ClickerState::NotFound;
      ok = false;
    }
  }

  if (ok) {
    // Connect to the address and type the scan just observed, not to what the
    // config guessed. esp_ble_hidh_dev_open returns non-null as soon as the
    // link is up; service discovery happens after and cannot fail this call.
    esp_hidh_dev_t* dev = esp_hidh_dev_open(g_target.val, ESP_HID_TRANSPORT_BLE, g_target.type);
    ok = (dev != nullptr);
    if (ok) {
      g_dev = dev;
      outcome = wintergreen::ClickerState::Connected;
    } else {
      g_fail_stage = kFailOpen;
    }
  }

  // Anything short of a live connection drops back to idle. No retry — a radio
  // hunting for a device that is not there is the battery leak this avoids.
  if (!ok)
    disconnect_only();  // sets Disconnected; the reason overwrites it below
  g_state = outcome;

  g_busy = false;
  vTaskDelete(nullptr);
}

// ── Public API ─────────────────────────────────────────────────────────────

inline wintergreen::ClickerState state() {
  return g_state;
}

// Failure detail, packed as `stage * 1000 + gap_status`, so one number in the
// quick-menu row identifies both which step failed and, for a GAP failure, why.
// 0 means no failure recorded. See FailStage and g_last_gap_status above.
//
// Example: 10013 = stage 10 (GAP connect) with status 0x0D (BLE_HS_ETIMEOUT) —
// the peer was found by the scan but never answered the connect.
inline int status_code() {
  const int stage = g_fail_stage;
  // While connected there is no failure to report, so the row carries free
  // internal RAM in KB instead — negated to keep it unambiguous. This is the
  // one number that decides whether the radio and the reader can coexist, and
  // there is no log to print it to. A reader page layout needs low tens of KB;
  // if this sits near zero, the next allocation the reader makes is the crash.
  if (stage == kFailNone) {
    if (g_state == wintergreen::ClickerState::Connected)
      return -static_cast<int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    return 0;
  }
  int gap = g_last_gap_status;
  if (gap < 0 || gap > 999)
    gap = 999;
  return stage * 1000 + gap;
}

inline void toggle() {
  if (g_busy)
    return;  // a transition is already in flight
  g_busy = true;
  // NotFound and Failed are off states: toggling from either retries.
  if (g_state != wintergreen::ClickerState::Connected) {
    g_state = wintergreen::ClickerState::Connecting;
    g_teardown_request = false;
    // 6 KB: esp_hidh_dev_open runs GATT discovery and report-map parsing on
    // this stack. It is freed the moment the connect finishes either way.
    if (xTaskCreate(connect_task, "wg_bt_up", 6144, nullptr, 4, nullptr) != pdPASS) {
      g_state = wintergreen::ClickerState::Disconnected;
      g_busy = false;
    }
  } else {
    // Direct: disconnect_only() is just ble_gap_terminate plus bookkeeping and
    // does not block. It needed a task back when it also tore the stack down.
    disconnect_only();
    g_busy = false;
  }
}

// Called every UI frame. Carries out the disconnect the HID callback asked for
// — the callback runs on esp_hidh's event task and must not close the device
// it is currently dispatching an event for.
inline void poll() {
  if (!g_teardown_request || g_busy)
    return;
  g_teardown_request = false;
  disconnect_only();
}

}  // namespace wg_clicker

#else  // !WG_BLUETOOTH_PAGE_TURNER

namespace wg_clicker {
inline wintergreen::ClickerState state() {
  return wintergreen::ClickerState::Unavailable;
}
inline int status_code() {
  return 0;
}
inline void toggle() {}
inline void poll() {}
}  // namespace wg_clicker

#endif  // WG_BLUETOOTH_PAGE_TURNER
