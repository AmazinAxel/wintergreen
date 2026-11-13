#pragma once

// BLE page-turner clicker — HID-over-GATT host, off by default.
//
// The radio is the largest current draw this firmware can switch on: tens of
// milliamps against the ~15 mA the CPU idles at. So:
//
//  - **Compiled out entirely** unless WG_BLUETOOTH_PAGE_TURNER is defined. The
//    linker then drops the whole NimBLE stack and the quick menu shows no row.
//  - **Off at every boot.** No persisted "was connected" flag. Deep-sleep wake
//    is a full boot, so picking the device up always comes back cold.
//  - **Radio powered down after any failure or disconnect.** A refused
//    connection, one that never completes, or a clicker switched off mid-book
//    all reach radio_off(), which disables the controller — not merely drops
//    the link. No retry loop, no reconnect timer.
//
// The NimBLE *host* stays initialised for the session (see "the stack is never
// torn down" — re-initialising it panics), but the **controller** is what draws
// current, and enable/disable is a supported cycle. So an idle session after
// toggling the clicker off costs nothing.
//
// **It scans rather than connecting straight to the configured MAC.** A BLE
// address does not carry its own type, and HID devices routinely advertise a
// resolvable private address that rotates every ~15 minutes, so the configured
// MAC is a *filter*, not a destination.

#include <cstdint>
#include <cstdio>

#include "esp_timer.h"
#include "ui_wake.h"

#include "WintergreenConfig.h"
#include "wintergreen/Input.h"
#include "wintergreen/Runtime.h"
#include "wintergreen/display/DrawBuffer.h"

namespace wg_clicker {

// Presses injected by the clicker, drained by Esp32InputSource::poll_buttons.
//
// **A ring buffer, not a bitmask.** A mask holds one bit per button, so several
// clicks arriving between two polls collapsed into a single press: clicking
// seven times in a second moved the list about three rows, one per UI frame
// that happened to contain a click. The internal buttons never had this — they
// queue every edge in press_queue_ — and this mirrors that, so a burst of
// clicks lands as a burst of presses in arrival order.
//
// Written from the HID callback and drained under Esp32InputSource's critical
// section. Single producer, single consumer, so head/tail need no locking of
// their own; a full queue drops the newest press rather than overwriting an
// unread one.
inline constexpr uint8_t kPressQueueSize = 16;
inline volatile uint8_t g_press_queue[kPressQueueSize] = {};
inline volatile uint8_t g_pq_head = 0;
inline volatile uint8_t g_pq_tail = 0;

inline uint8_t queue_room() {
  return static_cast<uint8_t>((g_pq_head + kPressQueueSize - g_pq_tail - 1) % kPressQueueSize);
}

inline void queue_press(uint8_t btn) {
  const uint8_t next = static_cast<uint8_t>((g_pq_tail + 1) % kPressQueueSize);
  if (next == g_pq_head)
    return;  // full: drop, never overwrite an unread press
  g_press_queue[g_pq_tail] = btn;
  g_pq_tail = next;
  ui_wake();
}

// HID keyboard usage IDs (HID Usage Table, keyboard page 0x07).
inline constexpr uint8_t kKeyRightArrow = 0x4F;
inline constexpr uint8_t kKeyLeftArrow = 0x50;

// One mapping everywhere: right = Button3, left = Button2. In the reader
// Button3 is next page (ReaderScreen: logical_next_front, both orientations);
// in lists left goes down and right up. There used to be a reader-only swap
// (g_in_reader) from when the reader read Button2 as next; it outlived that
// and turned pages backward (owner, on device, 2026-11-12).
inline constexpr uint8_t kBtnRight = static_cast<uint8_t>(wintergreen::Button::Button3);
inline constexpr uint8_t kBtnLeft = static_cast<uint8_t>(wintergreen::Button::Button2);

// Held past config::kBluetoothHoldDelayMs, right becomes Confirm and left
// becomes Back. The press already turned the page (or moved the cursor), so
// the hold queues Button::Undo first: the screen steps back without drawing,
// then acts. Separate from kHoldDelayMs (the device buttons); a tap held longer
// than it reads as a hold.
inline constexpr int64_t kClickerHoldMs = wintergreen::config::kBluetoothHoldDelayMs;
inline constexpr uint8_t kBtnConfirm = static_cast<uint8_t>(wintergreen::Button::Button1);
inline constexpr uint8_t kBtnBack = static_cast<uint8_t>(wintergreen::Button::Button0);

}  // namespace wg_clicker

#ifdef WG_BLUETOOTH_PAGE_TURNER

#include <cstring>

#include "esp_bt.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
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
#include "nvs.h"
#include "nvs_flash.h"

extern "C" void ble_store_config_init(void);

namespace wg_clicker {

inline volatile wintergreen::ClickerState g_state = wintergreen::ClickerState::Disconnected;
// Raised by the connection callback (link gone) or by toggle() during a
// connect (give up). Acted on by poll() from the UI loop, and ends every wait
// in connect_task.
inline volatile bool g_disconnect_request = false;
// A transition is in flight; a second toggle is ignored rather than queued.
inline volatile bool g_busy = false;

// The one connection, or BLE_HS_CONN_HANDLE_NONE. Owned by conn_cb: set on
// CONNECT, cleared on DISCONNECT. There is no address lookup: a HID device's
// resolvable private address resolves to its identity address once bonded, and
// matching connections by address is what once returned ENOTCONN on a live link.
inline volatile uint16_t g_conn = BLE_HS_CONN_HANDLE_NONE;
// 0 = connect pending, 1 = link up, -1 = connect failed.
inline volatile int8_t g_connect_result = 0;
// radio_off() is terminating the link itself, so its DISCONNECT is not "gone".
inline volatile bool g_closing = false;
inline volatile bool g_enc_done = false;

// Last keycode seen, so a held key does not repeat and an all-zero release
// report is not read as a press. g_press_us is when it went down, for the
// tap/hold split in handle_report.
inline uint8_t g_last_key = 0;
inline int64_t g_press_us = 0;
// Set once poll() has fired the hold, so the eventual release does not also
// inject a page turn.
inline volatile bool g_hold_fired = false;


// Clicker battery, 0 = unknown. Captured **once**, right after connecting, and
// then left alone so the quick menu shows a number that never twitches while
// reading. Cleared on disconnect; a sleep is a full boot and clears it anyway.
inline volatile uint8_t g_battery_pct = 0;

// ── Waiting ────────────────────────────────────────────────────────────────
//
// Every wait here is on an event, never a poll or a timer of our own. Each
// NimBLE procedure this file starts ends in a callback on its own: the scan
// after kScanMs (DISC_COMPLETE), a connect after kConnectMs (CONNECT with a
// timeout status), GATT procedures after ATT's 30 s (an error callback), and
// pairing after SM's 30 s (ENC_CHANGE). Each callback sets g_evt; the waiter
// re-checks its condition. A toggle during a connect sets it too, so pressing
// the row again always ends an attempt at once.
//
// An event group, not a semaphore: setting the bit wakes *every* waiter, so a
// sync calling radio_off() while connect_task is mid-wait cannot have its
// DISCONNECT consumed by the other task. A stray set (a late GATT error after
// a teardown) only costs the next waiter one extra condition check.
inline EventGroupHandle_t g_evt = nullptr;
// Bit 1: any condition changed (signal_). Bit 2: connect_task is exiting.
inline constexpr EventBits_t kEvTaskDone = 2;
// encrypt_() deleted a bond the clicker no longer holds (paired elsewhere since).
inline bool g_bond_deleted = false;
// The attempt was abandoned on purpose (row pressed again, or a sync), as
// opposed to g_disconnect_request alone, which a peer hang-up also sets.
inline volatile bool g_user_abort = false;

inline void signal_() {
  if (g_evt)
    xEventGroupSetBits(g_evt, 1);
}

// False when the attempt was abandoned (toggle, or the link dropped).
template <class Done>
inline bool wait_for_(Done done) {
  while (!done()) {
    if (g_disconnect_request || g_user_abort)
      return false;
    xEventGroupWaitBits(g_evt, 1, pdTRUE, pdFALSE, portMAX_DELAY);
  }
  return true;
}

// ── The stack is brought up once and NEVER torn down ───────────────────────
//
// **Do not add a deinit path.** Cycling NimBLE is unsupported by this IDF and
// fails by crashing, not by returning an error:
//
//   assert failed: ble_hs_init at ble_hs.c:928   (os_mempool_init of
//   ble_hs_hci_ev_pool)  <- esp_nimble_init <- bringup_stack
//
// With CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC=y, ble_hs_ctx and its
// hci_os_event_buf are heap-allocated behind an `if (!ble_hs_ctx)` guard, and
// esp_nimble_deinit() does not restore those pointers — so a second
// esp_nimble_init() builds the pool on freed state and panics.
//
// Each step keeps its **own** flag. A single flag set at the end is not enough:
// a bringup that reached esp_nimble_init() and then failed at sync left it
// false, so the retry re-ran the one call that cannot be repeated.
//
// Note the split between *inited* and *enabled* on the controller. Init/deinit
// is the pair that cannot be cycled; **enable/disable can**, and that is the one
// lever that genuinely powers the radio down mid-session — see radio_off().
inline bool g_ctrl_inited = false;   // esp_bt_controller_init — once, never undone
inline bool g_ctrl_enabled = false;  // esp_bt_controller_enable — cycles
inline bool g_nimble_up = false;     // esp_nimble_init + host config — once, panics if repeated
inline bool g_host_task_started = false;  // esp_nimble_enable — once; the task is never deleted
inline bool g_host_up = false;       // host synced with the controller; false again after radio_off

// The BT controller wants a contiguous block of internal DRAM, and a shortage
// shows as "esp_bt_controller_init -4" (ESP_ERR_NO_MEM) or, with slightly more
// room, "BLE assert emi.c 164" plus an interrupt-watchdog reboot. Nothing is
// handed back for it any more (the spare framebuffer, the book index and the
// reader caches used to be): with pages fully baked the reader holds no layout
// memory, so the heap has the room. If either symptom returns, that is a RAM
// budget regression, not a reason to start releasing buffers again.

// A Wi-Fi sync owns the one radio: toggle() does nothing until it ends.
inline bool g_sync_holds_radio = false;

// "AA:BB:CC:DD:EE:FF" -> 6 bytes, **reversed**. NimBLE's ble_addr_t.val is
// little-endian (ble_gap.c tests addr[5] & 0xC0). Reversed, the scan hunts an
// address that does not exist and looks exactly like a clicker switched off.
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

// Map one HID input report onto a button. Scanned for a keycode rather than
// parsed against the report map: a clicker sends one key at a time, and a
// permissive scan works across the boot- and report-protocol layouts clickers
// disagree about. Fed by conn_cb from raw NimBLE notifications.
//
// **The page turns on the press edge**, the moment the report arrives — a
// release-edge turn added the whole press duration to every page.
//
// The **hold** does not wait for the release: a held key sends no further
// reports, so poll() watches the clock and, at kClickerHoldMs, queues Undo
// (revert the turn the press made) and then Confirm/Back. The release is
// ignored either way.
inline void handle_report(const uint8_t* data, uint16_t len) {
  uint8_t found = 0;
  for (uint16_t i = 0; i < len; ++i) {
    if (data[i] == kKeyLeftArrow || data[i] == kKeyRightArrow) {
      found = data[i];
      break;
    }
  }
  if (found != 0 && found != g_last_key) {
    // Press edge: turn now, start the hold clock.
    g_press_us = esp_timer_get_time();
    g_hold_fired = false;
    queue_press(found == kKeyLeftArrow ? kBtnLeft : kBtnRight);
  }
  g_last_key = found;
}

// ── Scan ───────────────────────────────────────────────────────────────────
inline constexpr uint16_t kHidServiceUuid = 0x1812;
// Short on purpose: clickers advertise in a burst after a keypress and then go
// quiet, so every extra second risks the device we just found becoming
// unconnectable.
inline constexpr int32_t kScanMs = 5000;
// How long the controller tries to open the link to the address just seen.
inline constexpr int32_t kConnectMs = 10000;

// Connection interval 7.5-15 ms (units 1.25 ms), latency 0 from our side. A key
// report waits at most one interval for its connection event, so this is the
// clicker's whole share of page-turn latency. Also what makes discovery fast:
// every GATT round trip costs about one interval. Slave latency stays the
// clicker's choice (it only lets an idle peripheral skip events; a key still
// goes out at the next one).
inline constexpr uint16_t kItvlMin = 6;
inline constexpr uint16_t kItvlMax = 12;

inline struct ble_gap_conn_params conn_params_() {
  struct ble_gap_conn_params p = {};
  p.scan_itvl = 96;    // initiator scans at 100% duty, like scan_for_clicker
  p.scan_window = 96;
  p.itvl_min = kItvlMin;
  p.itvl_max = kItvlMax;
  p.latency = 0;
  p.supervision_timeout = 400;  // 4 s, units 10 ms
  return p;
}

inline uint8_t g_want_mac[6] = {};
inline uint8_t g_own_addr_type = 0;
inline ble_addr_t g_target{};
inline ble_addr_t g_hid_candidate{};
inline volatile bool g_have_target = false;
inline volatile bool g_have_hid = false;
inline volatile bool g_scan_done = false;

inline bool adv_has_hid_service(const struct ble_gap_disc_desc* d) {
  struct ble_hs_adv_fields f;
  if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0)
    return false;
  for (int i = 0; i < f.num_uuids16; ++i)
    if (ble_uuid_u16(&f.uuids16[i].u) == kHidServiceUuid)
      return true;
  return false;
}

inline int scan_cb(struct ble_gap_event* ev, void*) {
  if (ev->type == BLE_GAP_EVENT_DISC) {
    if (memcmp(ev->disc.addr.val, g_want_mac, 6) == 0) {
      g_target = ev->disc.addr;  // address *and* the type it really uses
      g_have_target = true;
      // Synchronous: discovery is stopped when this returns, so the connect
      // that follows never sees BLE_HS_EBUSY.
      ble_gap_disc_cancel();
      g_scan_done = true;
      signal_();
    } else if (!g_have_hid && adv_has_hid_service(&ev->disc)) {
      // Recorded as a fallback only — the scan deliberately runs to completion.
      // **Do not cancel here.** This is not "the clicker behind a rotated
      // address", it is the first device on the air advertising 0x1812, which
      // may be someone else's keyboard or mouse. Ending the scan on it means
      // never seeing the configured MAC that was about to advertise, and the
      // wrong peer connects, so the row says Connected with a clicker whose
      // buttons do nothing.
      g_hid_candidate = ev->disc.addr;
      g_have_hid = true;
    }
  } else if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
    g_scan_done = true;
    signal_();
  }
  return 0;
}

inline bool scan_for_clicker() {
  g_have_target = false;
  g_have_hid = false;
  g_scan_done = false;

  int rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
  if (rc != 0) {
    return false;
  }

  struct ble_gap_disc_params params = {};
  params.passive = 0;  // active: some clickers put the HID UUID in the scan response
  params.filter_duplicates = 1;
  // 100% duty: window == interval, so the radio listens continuously instead of
  // sampling. Left at 0 these take NimBLE's low-duty defaults, which can sit
  // between advertisements — and a clicker advertises in a short burst after a
  // keypress, so a missed burst costs the entire kScanMs. Units are 0.625 ms.
  params.itvl = 96;    // 60 ms
  params.window = 96;  // 60 ms
  rc = ble_gap_disc(g_own_addr_type, kScanMs, &params, scan_cb, nullptr);
  if (rc != 0) {
    return false;
  }

  if (!wait_for_([] { return g_scan_done; }))
    return false;

  if (g_have_target)
    return true;
  // Nothing at the configured address but something advertising as HID: almost
  // always the clicker behind a rotated private address. It bonds on connect,
  // so this is a one-time detour.
  if (g_have_hid) {
    g_target = g_hid_candidate;
    return true;
  }
  return false;
}

// ── Connection ─────────────────────────────────────────────────────────────
//
// What esp_hidh used to do, minus everything that did not help: its GATT
// discovery ran before the link was encrypted and mapped nothing, its CCCD
// writes were refused for the same reason, and its INPUT events therefore
// never fired (measured: 54 notifications, 0 INPUT events). Keys have come from
// raw notifications since; now the connect is ours too.

inline constexpr uint16_t kBatteryLevelChrUuid = 0x2A19;
inline constexpr uint16_t kCccdUuid = 0x2902;
inline constexpr int kMaxHidServices = 4;
inline constexpr int kMaxCccds = 16;
inline uint16_t g_hid_svc_range[kMaxHidServices][2] = {};
inline volatile int g_hid_svc_n = 0;
inline uint16_t g_cccd[kMaxCccds] = {};
inline volatile int g_cccd_n = 0;
inline volatile bool g_gatt_done = false;
inline volatile int g_gatt_status = 0;

// A notification from inside an HID service is a key report (battery lives in
// its own service, so it never matches). By range, not by characteristic:
// NimBLE's whole-range descriptor discovery reports the range start as every
// CCCD's "characteristic", so there is no per-report handle to match —
// measured, 70 notifications on handle 43 matched none. handle_report scans
// for the arrow keycodes, so a non-key HID report is simply ignored.
inline void on_notify_(const struct ble_gap_event* ev) {
  if (g_state != wintergreen::ClickerState::Connected)
    return;
  const uint16_t h = ev->notify_rx.attr_handle;
  for (int i = 0; i < g_hid_svc_n; ++i) {
    if (h < g_hid_svc_range[i][0] || h > g_hid_svc_range[i][1])
      continue;
    // A keyboard report is 8 bytes; anything longer is truncated, which only
    // drops keycodes past the 16th byte.
    uint8_t data[16];
    uint16_t len = 0;
    ble_hs_mbuf_to_flat(ev->notify_rx.om, data, sizeof data, &len);
    handle_report(data, len);
    return;
  }
}

// The connection's GAP callback. Runs on the NimBLE host task: it records and
// signals, and never tears anything down itself (poll() does, on the UI loop).
inline int conn_cb(struct ble_gap_event* ev, void*) {
  switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (ev->connect.status == 0) {
        g_conn = ev->connect.conn_handle;
        g_connect_result = 1;
      } else {
        g_connect_result = -1;
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      if (ev->disconnect.conn.conn_handle != g_conn)
        return 0;
      g_conn = BLE_HS_CONN_HANDLE_NONE;
      // The peer went away (or never finished connecting): power down rather
      // than reconnect. radio_off()'s own terminate is not "gone".
      if (!g_closing) {
        g_disconnect_request = true;
        ui_wake();  // poll() powers down; an idle loop would not look for seconds
      }
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      g_enc_done = true;
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      // The clicker forgot us (paired with another host) and is pairing
      // afresh while we still hold its old bond. Drop ours and let it.
      struct ble_gap_conn_desc desc;
      if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0)
        ble_store_util_delete_peer(&desc.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ: {
      // Accept the clicker's request but cap the interval, so it cannot trade
      // our page-turn latency for its battery. Its latency and supervision
      // timeout stay: both were valid at the longer interval, so still are.
      //
      // **The two events fill different fields.** CONN_UPDATE_REQ (LL
      // procedure) hands us self_params to edit; L2CAP_UPDATE_REQ leaves
      // self_params null and applies peer_params itself once we return 0 — a
      // non-const object behind a const pointer. Dereferencing self_params on
      // the L2CAP one panicked on every connect (hardware breadcrumb: event 5,
      // reset reason 4).
      struct ble_gap_upd_params* p =
          ev->type == BLE_GAP_EVENT_CONN_UPDATE_REQ
              ? ev->conn_update_req.self_params
              : const_cast<struct ble_gap_upd_params*>(ev->conn_update_req.peer_params);
      if (!p)
        return 0;
      if (p->itvl_max > kItvlMax)
        p->itvl_max = kItvlMax;
      if (p->itvl_min > p->itvl_max)
        p->itvl_min = p->itvl_max;
      return 0;
    }
    case BLE_GAP_EVENT_NOTIFY_RX:
      on_notify_(ev);
      return 0;
    default:
      return 0;
  }
  signal_();
  return 0;
}

inline bool link_up_() {
  return g_conn != BLE_HS_CONN_HANDLE_NONE;
}

inline bool link_encrypted_() {
  struct ble_gap_conn_desc desc;
  return ble_gap_conn_find(g_conn, &desc) == 0 && desc.sec_state.encrypted;
}

// Encrypt the link: with the stored bond if there is one (a key refresh), Just
// Works pairing otherwise. HID characteristics refuse an unencrypted client.
//
// **A stale bond gets one retry.** If the clicker paired elsewhere it no longer
// knows our key, encrypting with it fails, and every later attempt would fail
// the same way. Deleting our side of the bond and pairing again is what a user
// would otherwise do by hand. A clicker that starts pairing itself lands in
// REPEAT_PAIRING in conn_cb instead.
inline bool encrypt_() {
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (link_encrypted_())
      return true;
    // The peer's identity, taken while the link is certainly up: a clicker
    // that rejects our key often drops the link straight after, and looking
    // it up then fails, which used to leave the stale bond in place for good.
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(g_conn, &desc) != 0)
      return false;
    g_enc_done = false;
    const int rc = ble_gap_security_initiate(g_conn);
    // EALREADY: the clicker's own security request already started one.
    if (rc != 0 && rc != BLE_HS_EALREADY) {
      return false;
    }
    if (!wait_for_([] { return g_enc_done || !link_up_(); }))
      return false;
    if (link_encrypted_())
      return true;
    // Only an encryption that completed and failed means the key is stale; a
    // link lost mid-attempt (out of range) says nothing about the bond.
    if (!g_enc_done)
      return false;
    ble_store_util_delete_peer(&desc.peer_id_addr);
    g_bond_deleted = true;
    if (!link_up_())
      return false;  // connect_task pairs again over a fresh link
  }
  return false;
}

inline int hid_svc_cb(uint16_t, const struct ble_gatt_error* err, const struct ble_gatt_svc* svc, void*) {
  if (err->status == 0 && svc && g_hid_svc_n < kMaxHidServices) {
    g_hid_svc_range[g_hid_svc_n][0] = svc->start_handle;
    g_hid_svc_range[g_hid_svc_n][1] = svc->end_handle;
    g_hid_svc_n = g_hid_svc_n + 1;
  } else if (err->status != 0) {
    g_gatt_status = err->status == BLE_HS_EDONE ? 0 : err->status;
    g_gatt_done = true;
    signal_();
  }
  return 0;
}

inline int cccd_cb(uint16_t, const struct ble_gatt_error* err, uint16_t, const struct ble_gatt_dsc* dsc, void*) {
  if (err->status == 0 && dsc) {
    static const ble_uuid16_t cccd = BLE_UUID16_INIT(kCccdUuid);
    if (ble_uuid_cmp(&dsc->uuid.u, &cccd.u) == 0 && g_cccd_n < kMaxCccds) {
      g_cccd[g_cccd_n] = dsc->handle;
      g_cccd_n = g_cccd_n + 1;
    }
  } else if (err->status != 0) {
    g_gatt_status = err->status == BLE_HS_EDONE ? 0 : err->status;
    g_gatt_done = true;
    signal_();
  }
  return 0;
}

inline int cccd_write_cb(uint16_t, const struct ble_gatt_error* err, struct ble_gatt_attr*, void*) {
  g_gatt_status = err->status;
  g_gatt_done = true;
  signal_();
  return 0;
}

// Read-by-UUID calls back once per matching attribute and then once more with
// EDONE (or with the error, e.g. "attribute not found" on a clicker without a
// battery service). Only the final call ends the wait.
inline int battery_read_cb(uint16_t, const struct ble_gatt_error* err, struct ble_gatt_attr* attr, void*) {
  if (err->status == 0) {
    uint8_t level = 0;
    if (attr && attr->om && OS_MBUF_PKTLEN(attr->om) >= 1 &&
        ble_hs_mbuf_to_flat(attr->om, &level, sizeof(level), nullptr) == 0 && level > 0 && level <= 100 &&
        g_battery_pct == 0)
      g_battery_pct = level;
    return 0;
  }
  g_gatt_done = true;
  signal_();
  return 0;
}

// ── Handle cache ───────────────────────────────────────────────────────────
//
// Service and descriptor discovery is the slowest part of a connect (~800 ms
// measured) and its answer never changes for one clicker. So the HID service
// ranges and CCCD handles are kept in NVS, keyed by the peer's identity
// address, written once after a full discovery. A reconnect skips discovery
// and goes straight to the CCCD writes. Any write the clicker refuses (handles
// moved, a different device) drops the cache and rediscovers once.
//
// The CCCD writes themselves stay on every connect: a clicker that paired
// elsewhere forgot its subscriptions, and at a 7.5-15 ms interval the writes
// are a few tens of ms.
struct HidCache {
  uint8_t addr[6];
  uint8_t svc_n;
  uint8_t cccd_n;
  uint16_t svc[kMaxHidServices][2];
  uint16_t cccd[kMaxCccds];
};
inline constexpr const char* kNvsNs = "wg_bt";
inline constexpr const char* kNvsHid = "hid";

inline bool peer_id_(ble_addr_t* out) {
  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(g_conn, &desc) != 0)
    return false;
  *out = desc.peer_id_addr;
  return true;
}

inline bool load_cache_(const ble_addr_t& peer) {
  nvs_handle_t h;
  if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK)
    return false;
  HidCache c;
  size_t len = sizeof c;
  const bool got = nvs_get_blob(h, kNvsHid, &c, &len) == ESP_OK && len == sizeof c;
  nvs_close(h);
  if (!got || memcmp(c.addr, peer.val, 6) != 0 || c.svc_n == 0 || c.svc_n > kMaxHidServices ||
      c.cccd_n == 0 || c.cccd_n > kMaxCccds)
    return false;
  memcpy(g_hid_svc_range, c.svc, sizeof c.svc);
  g_hid_svc_n = c.svc_n;
  memcpy(g_cccd, c.cccd, sizeof c.cccd);
  g_cccd_n = c.cccd_n;
  return true;
}

// erase = forget the cache (it pointed at handles the clicker refused).
inline void store_cache_(const ble_addr_t& peer, bool erase) {
  nvs_handle_t h;
  if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK)
    return;
  if (erase) {
    nvs_erase_key(h, kNvsHid);
  } else {
    HidCache c = {};
    memcpy(c.addr, peer.val, 6);
    c.svc_n = static_cast<uint8_t>(g_hid_svc_n);
    c.cccd_n = static_cast<uint8_t>(g_cccd_n);
    memcpy(c.svc, g_hid_svc_range, sizeof c.svc);
    memcpy(c.cccd, g_cccd, sizeof c.cccd);
    nvs_set_blob(h, kNvsHid, &c, sizeof c);
  }
  nvs_commit(h);
  nvs_close(h);
}

// Wait for the GATT procedure just issued; true when it succeeded.
inline bool gatt_wait_() {
  return wait_for_([] { return g_gatt_done; }) && g_gatt_status == 0;
}

inline bool discover_hid_();

// Write "notifications on" to every known CCCD. Rewriting an already-enabled
// one is harmless, so there is no read first. True when all of them took.
inline bool write_cccds_() {
  static const uint8_t kNotifyOn[2] = {0x01, 0x00};
  for (int i = 0; i < g_cccd_n; ++i) {
    g_gatt_done = false;
    const int rc = ble_gattc_write_flat(g_conn, g_cccd[i], kNotifyOn, sizeof kNotifyOn, cccd_write_cb, nullptr);
    if (rc != 0 || !gatt_wait_()) {
      return false;
    }
  }
  return g_cccd_n > 0;
}

// Turn on every HID notification, from cached handles when this clicker has
// been seen before.
inline bool enable_key_notifications() {
  ble_addr_t peer{};
  const bool have_peer = peer_id_(&peer);
  if (have_peer && load_cache_(peer)) {
    if (write_cccds_())
      return true;
    if (g_disconnect_request || !link_up_())
      return false;
    store_cache_(peer, /*erase=*/true);
  }
  if (!discover_hid_())
    return false;
  if (!write_cccds_())
    return false;
  if (have_peer)
    store_cache_(peer, /*erase=*/false);
  return true;
}

inline bool discover_hid_() {
  const uint16_t conn = g_conn;
  g_hid_svc_n = 0;
  g_gatt_done = false;
  static const ble_uuid16_t hid = BLE_UUID16_INIT(kHidServiceUuid);
  int rc = ble_gattc_disc_svc_by_uuid(conn, &hid.u, hid_svc_cb, nullptr);
  if (rc != 0 || !gatt_wait_()) {
    return false;
  }

  g_cccd_n = 0;
  for (int s = 0; s < g_hid_svc_n; ++s) {
    g_gatt_done = false;
    rc = ble_gattc_disc_all_dscs(conn, g_hid_svc_range[s][0], g_hid_svc_range[s][1], cccd_cb, nullptr);
    if (rc != 0 || !gatt_wait_()) {
      return false;
    }
  }

  return true;
}

// The clicker's own battery, one read of 0x2A19 over the whole handle range.
// **Also the liveness check**: a peer that answers a GATT read will send
// reports; one that never answers fails the connect here. A clicker with no
// battery service still *answers* ("attribute not found") and connects with
// no percentage.
inline bool read_battery_once() {
  ble_uuid16_t uuid = BLE_UUID16_INIT(kBatteryLevelChrUuid);
  g_gatt_done = false;
  const int rc = ble_gattc_read_by_uuid(g_conn, 0x0001, 0xFFFF, &uuid.u, battery_read_cb, nullptr);
  if (rc != 0) {
    return false;
  }
  return wait_for_([] { return g_gatt_done; });
}

inline void host_task(void*) {
  nimble_port_run();  // returns only after nimble_port_stop()
  nimble_port_freertos_deinit();
}

inline void host_synced_() {
  signal_();
}

// Drop the connection **and power the radio down**.
//
// esp_bt_controller_disable() returns the controller to INITED, which is the
// documented, supported half of the cycle — unlike esp_nimble_deinit(), which
// panics on re-init and is why the host stack stays up forever. Disabling the
// controller is what actually stops the radio drawing current; closing the
// connection alone leaves it powered and idle, which was the battery leak.
//
// The NimBLE host stays initialised with a dead controller underneath it. That
// is fine because it is quiescent: nothing schedules GAP or GATT work while
// there is no connection and no scan. g_host_up drops so the next bringup waits
// for a fresh sync after re-enabling.
inline void radio_off() {
  // Only touch NimBLE once the host is running: this is also the failure path
  // of bringup_stack(), which can bail out before esp_nimble_init(), and
  // ble_gap_disc_cancel() then dereferences a null ble_hs_ctx — a load fault,
  // not an error return.
  if (g_host_up) {
    ble_gap_disc_cancel();
    if (ble_gap_conn_active())
      ble_gap_conn_cancel();
    // Let the disconnect reach the peer before the controller goes away under
    // it: wait for our own DISCONNECT, which the controller always delivers.
    g_closing = true;
    if (link_up_() && ble_gap_terminate(g_conn, BLE_ERR_REM_USER_CONN_TERM) == 0) {
      while (link_up_())
        xEventGroupWaitBits(g_evt, 1, pdTRUE, pdFALSE, portMAX_DELAY);
    }
    g_conn = BLE_HS_CONN_HANDLE_NONE;
    g_closing = false;
  }
  if (g_ctrl_enabled) {
    esp_bt_controller_disable();
    g_ctrl_enabled = false;
    g_host_up = false;  // resync on the next enable
  }
  // **Never esp_bt_controller_deinit().** It destroys the HCI transport the
  // NimBLE host is attached to, the host cannot be restarted on this IDF, and
  // a later init builds a transport the host never binds to: the clicker is
  // gone until reboot. A sync used to do exactly that to make room for Wi-Fi;
  // it now fits beside an initialised, disabled controller.

  g_last_key = 0;
  g_press_us = 0;
  g_hold_fired = false;
  g_battery_pct = 0;
  g_connect_result = 0;
  g_state = wintergreen::ClickerState::Disconnected;
}

inline bool bringup_stack() {
  if (!g_evt) {
    g_evt = xEventGroupCreate();
    if (!g_evt) {
      return false;
    }
  }

  // NVS backs the bond store and nothing else here uses it, so a device with
  // the radio off never pays for it.
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs = nvs_flash_init();
  }
  if (nvs != ESP_OK) {
    return false;
  }

  if (!g_ctrl_inited) {
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&cfg) != ESP_OK) {
      return false;
    }
    g_ctrl_inited = true;
  }

  // Separate from init, and re-run after every radio_off().
  if (!g_ctrl_enabled) {
    if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) {
      return false;
    }
    g_ctrl_enabled = true;
  }

  if (!g_nimble_up) {
    if (esp_nimble_init() != ESP_OK) {
      return false;
    }
    g_nimble_up = true;

    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sync_cb = host_synced_;

    // **None of this is optional.** Every sm_ field defaults to 0 in NimBLE
    // (BLE_SM_BONDING and both key-dist masks), and the HID characteristics
    // need an encrypted link — the defaults give a device that connects and
    // then reads nothing. Just Works, because a clicker has no display or
    // keypad. The ID key carries the peer's IRK, which is what lets the stack
    // recognise a rotating private address later.
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  }

  // The host task is started once and never deleted; esp_nimble_disable() is a
  // bare vTaskDelete and the host is not restartable. It simply idles on its
  // event queue while the controller is disabled.
  if (!g_host_task_started) {
    // esp_nimble_enable ignores xTaskCreatePinnedToCore's return and always
    // reports ESP_OK, so a host task that could not be allocated shows up as a
    // sync that never comes (press the row again to give up).
    if (esp_nimble_enable(reinterpret_cast<void*>(host_task)) != ESP_OK) {
      return false;
    }
    g_host_task_started = true;
  }

  // Waited for on every bringup, not just the first: after radio_off() the
  // controller was disabled underneath the host, and re-enabling it runs a
  // fresh HCI reset that has to complete before any GAP call is legal.
  // sync_cb fires on each sync and sets g_evt.
  if (!g_host_up) {
    if (!wait_for_([] { return ble_hs_synced() != 0; })) {
      return false;
    }
    g_host_up = true;
  }
  return true;
}

// The bonded clicker's identity address, as an identity type, so the
// controller matches its rotating private address through the resolving list
// (NimBLE loads every bond's IRK into it at sync). Prefers the configured MAC
// when two bonds exist.
inline bool bonded_target_(ble_addr_t* out) {
  ble_addr_t peers[2];
  int n = 0;
  if (ble_store_util_bonded_peers(peers, &n, 2) != 0 || n == 0)
    return false;
  int pick = 0;
  for (int i = 0; i < n; ++i)
    if (memcmp(peers[i].val, g_want_mac, 6) == 0)
      pick = i;
  *out = peers[pick];
  out->type = out->type == BLE_ADDR_RANDOM ? BLE_ADDR_RANDOM_ID : BLE_ADDR_PUBLIC_ID;
  return ble_hs_id_infer_auto(0, &g_own_addr_type) == 0;
}

// Open the link to g_target. Ends on CONNECT (success or the controller's own
// timeout) or on a toggle.
inline bool connect_(int32_t timeout_ms) {
  g_connect_result = 0;
  const struct ble_gap_conn_params params = conn_params_();
  const int rc = ble_gap_connect(g_own_addr_type, &g_target, timeout_ms, &params, conn_cb, nullptr);
  if (rc != 0) {
    return false;
  }
  return wait_for_([] { return g_connect_result != 0; }) && g_connect_result > 0;
}

inline void connect_task(void*) {
  uint8_t mac[6];
  bool ok = parse_mac(WG_BLUETOOTH_PAGE_TURNER, mac) && bringup_stack();

  if (ok)
    memcpy(g_want_mac, mac, 6);

  // Bonded: connect straight to the clicker, no scan. The initiator listens for
  // its advertising just as a scan would (same window), so kScanMs bounds it.
  // Falls back to the scan if that fails for any reason but a toggle — the
  // scan is what finds a clicker the resolving list does not.
  bool linked = false;
  if (ok && bonded_target_(&g_target)) {
    linked = connect_(kScanMs);
    if (!linked && g_disconnect_request)
      ok = false;
  }
  if (ok && !linked) {
    ok = scan_for_clicker();
    // Connect to the address and type the scan just observed, not to what the
    // config guessed.
    ok = ok && connect_(kConnectMs);
  }

  // **Connected means keys will arrive**: encrypted, every HID notification
  // on, and the peer answering a read. A link that cannot do all three cannot
  // turn pages.
  if (ok) {
    g_bond_deleted = false;
    ok = encrypt_();
    // Paired elsewhere since: encrypt_() dropped the stale bond, but the
    // clicker hung up over the failed encryption. Pair once more over a fresh
    // link rather than making the reader press the row again.
    if (!ok && g_bond_deleted && !g_user_abort && !link_up_()) {
      g_disconnect_request = false;  // set by that hang-up, not by the reader
      ok = scan_for_clicker() && connect_(kConnectMs) && encrypt_();
    }
  }
  if (ok)
    ok = enable_key_notifications();
  if (ok)
    ok = read_battery_once();

  // Every failure looks the same from here: back to Disconnected, radio off,
  // no retry (beyond the one re-pair above).
  if (ok) {
    g_state = wintergreen::ClickerState::Connected;
  } else {
    radio_off();
  }

  g_busy = false;
  if (g_evt)
    xEventGroupSetBits(g_evt, kEvTaskDone);  // begin_sync() may be waiting for this exit
  ui_wake();  // the row repaints Connected/Disconnected now, not at the next deadline
  vTaskDelete(nullptr);
}

// ── Public API ─────────────────────────────────────────────────────────────

// Take the clicker down for a Wi-Fi sync: link closed, controller disabled,
// stack left initialised (never deinit — see radio_off()). Returns whether it
// was up or coming up, so the sync can bring it back with end_sync().
inline bool begin_sync() {
  const bool was_on = g_state == wintergreen::ClickerState::Connected ||
                      g_state == wintergreen::ClickerState::Connecting;
  g_sync_holds_radio = true;
  // A connect in flight is abandoned and **waited out** before the radio goes:
  // turning the controller off under connect_task left it waiting forever (a
  // cancelled scan sends no event), and end_sync()'s reconnect then started a
  // second connect_task beside it on the same globals. The abort ends every
  // wait_for_ at once, so this is the time to unwind, not a timeout.
  if (g_busy && g_evt) {
    g_user_abort = true;
    g_disconnect_request = true;
    signal_();
    while (g_busy)
      xEventGroupWaitBits(g_evt, kEvTaskDone, pdTRUE, pdFALSE, portMAX_DELAY);
  }
  radio_off();
  g_busy = false;
  g_disconnect_request = false;
  return was_on;
}

// The sync is over. `reconnect` = what begin_sync() returned.
inline void toggle();
inline void end_sync(bool reconnect) {
  g_sync_holds_radio = false;
  if (reconnect)
    toggle();
}

// A connect in flight: the main loop keeps its frame cadence.
inline bool busy() {
  return g_busy;
}

// Milliseconds until a held key becomes a hold (at least 1), or UINT32_MAX
// when no key is down. The loop sleeps exactly that long instead of polling.
inline uint32_t hold_wait_ms() {
  if (g_last_key == 0 || g_hold_fired)
    return UINT32_MAX;
  const int64_t left_us = g_press_us + kClickerHoldMs * 1000 - esp_timer_get_time();
  return left_us <= 1000 ? 1 : static_cast<uint32_t>(left_us / 1000);
}

inline wintergreen::ClickerState state() {
  return g_state;
}

// Clicker battery percentage, or 0 when it has not reported one.
inline uint8_t battery_pct() {
  return g_battery_pct;
}

inline void toggle() {
  if (g_sync_holds_radio)
    return;
  // A press while a connect is still running means "give up", not "ignore me":
  // the request ends whatever the worker is waiting on, and it powers down.
  if (g_busy) {
    if (g_state == wintergreen::ClickerState::Connecting) {
      g_user_abort = true;
      g_disconnect_request = true;  // poll() tears down once the worker exits
      signal_();
    }
    return;
  }
  g_busy = true;
  // Anything not Connected is an off state: toggling retries.
  if (g_state != wintergreen::ClickerState::Connected) {
    g_state = wintergreen::ClickerState::Connecting;
    g_disconnect_request = false;
    g_user_abort = false;
    if (xTaskCreate(connect_task, "wg_bt_up", 5120, nullptr, 4, nullptr) != pdPASS) {
      g_state = wintergreen::ClickerState::Disconnected;
      g_busy = false;
    }
  } else {
    // Direct: radio_off() closes the link and disables the controller. It
    // waits only for the link's own DISCONNECT, short enough to run on the UI
    // thread and avoids a task for a one-shot teardown.
    radio_off();
    g_busy = false;
  }
}

// Called every UI frame. Fires a hold the moment it crosses the threshold, and
// carries out the disconnect conn_cb asked for — that callback runs on the
// NimBLE host task and must not tear down the stack dispatching it.
inline void poll() {
  // A held key sends no further reports, so handle_report never runs again
  // while it is down. The threshold has to be noticed here instead.
  if (g_last_key != 0 && !g_hold_fired && esp_timer_get_time() - g_press_us >= kClickerHoldMs * 1000) {
    g_hold_fired = true;
    // Both or neither: an action without its Undo would act one step off.
    if (queue_room() >= 2) {
      queue_press(static_cast<uint8_t>(wintergreen::Button::Undo));
      queue_press((g_last_key == kKeyLeftArrow) ? kBtnBack : kBtnConfirm);
    }
  }

  if (!g_disconnect_request || g_busy)
    return;
  g_disconnect_request = false;
  radio_off();
}

}  // namespace wg_clicker

#else  // !WG_BLUETOOTH_PAGE_TURNER

namespace wg_clicker {
inline wintergreen::ClickerState state() {
  return wintergreen::ClickerState::Unavailable;
}
inline uint8_t battery_pct() {
  return 0;
}
inline void toggle() {}
inline void poll() {}
inline bool busy() {
  return false;
}
inline uint32_t hold_wait_ms() {
  return UINT32_MAX;
}
inline bool begin_sync() {
  return false;
}
inline void end_sync(bool) {}
}  // namespace wg_clicker

#endif  // WG_BLUETOOTH_PAGE_TURNER
