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

#include "WintergreenConfig.h"
#include "wintergreen/Input.h"
#include "wintergreen/Runtime.h"
#include "wintergreen/display/DrawBuffer.h"

namespace wg_clicker {

// Presses injected by the clicker, merged by Esp32InputSource::poll_buttons the
// same way serial-injected presses are. Bitmask of Button indices.
inline volatile uint8_t g_clicker_buttons = 0;

// HID keyboard usage IDs (HID Usage Table, keyboard page 0x07). Left advances,
// right goes back — deliberately the opposite of a presentation remote. Swap
// these two lines to flip it.
inline constexpr uint8_t kKeyRightArrow = 0x4F;
inline constexpr uint8_t kKeyLeftArrow = 0x50;

// Button2/Button3 are "next"/"previous" in the reader in both orientations
// (unlike the side rocker, which swaps). On other screens they are list
// navigation, which is a sane thing for a clicker to do.
inline constexpr uint8_t kBtnNext = static_cast<uint8_t>(wintergreen::Button::Button2);
inline constexpr uint8_t kBtnPrev = static_cast<uint8_t>(wintergreen::Button::Button3);

}  // namespace wg_clicker

#ifdef WG_BLUETOOTH_PAGE_TURNER

#include <cstring>

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
// Raised by the HID callback, acted on by poll() from the UI loop.
inline volatile bool g_disconnect_request = false;
// A transition is in flight; a second toggle is ignored rather than queued.
inline volatile bool g_busy = false;

inline esp_hidh_dev_t* g_dev = nullptr;
// Last keycode seen, so a held key does not repeat and an all-zero release
// report is not read as a press.
inline uint8_t g_last_key = 0;

// Clicker battery, 0 = unknown. Captured **once**, right after connecting, and
// then left alone so the quick menu shows a number that never twitches while
// reading. Cleared on disconnect; a sleep is a full boot and clears it anyway.
//
// **esp_hidh will not give you this.** nimble_hidh.c posts
// ESP_HIDH_BATTERY_EVENT only from a notify_rx, and most clickers notify only
// when the level *changes* — so waiting for that event shows nothing for hours.
// read_battery_once() does an explicit GATT read instead.
inline volatile uint8_t g_battery_pct = 0;

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
inline bool g_nimble_up = false;     // esp_nimble_init — once, panics if repeated
inline bool g_hidh_up = false;       // esp_hidh_init — once
inline bool g_host_task_started = false;  // esp_nimble_enable — once; the task is never deleted
inline bool g_host_up = false;       // host synced with the controller; false again after radio_off

// **There is deliberately no free-heap floor here any more.** kMinHeapKb
// existed to turn a hopeless attempt into a clean refusal, and in practice it
// did the opposite: it returned false without touching g_state, so the row sat
// on "Disconnected" and the device looked like it had ignored the button. Worse,
// whether it tripped depended on heap fragmentation, so the same press worked
// or did nothing depending on what had run before — which is exactly what made
// "run a sync first" look like a fix. A press now always attempts a connect.
//
// DrawBuffer's spare, released for the length of a connection attempt and taken
// back when the radio goes down. Set by bind(); null on a build that never
// calls it, and every use is guarded.
//
// **The clicker does not fit without this.** The BT controller wants a
// contiguous block of internal DRAM, and this device has one 50 KB pool of it
// (the boot log's "At 3FCB3490 len 0000CB70 (50 KiB): RAM"; the other ~123 KB
// is retention RAM). DrawBuffer's two framebuffers are 96 KB of BSS in that
// space and the spare is another 48 KB on the heap. Releasing the book index
// alone left the controller reporting, verbatim:
//
//   E BLE_INIT: Malloc failed
//   E BLE_INIT: esp_bt_controller_init -4      (ESP_ERR_NO_MEM)
//
// With slightly more free RAM the same shortage got one stage further and
// asserted inside the closed controller blob instead — "BLE assert emi.c 164"
// followed by an interrupt watchdog reboot from r_lld_core_init. Same cause,
// two symptoms; neither is a coexistence, modem-sleep or PHY-calibration
// problem, all of which were tried first and changed nothing.
//
// It is given up on **every** connect. Keeping it when the heap looked roomy
// was tried and made the failure depend on fragmentation rather than on
// anything the user did. Page turns fall back to re-rendering for as long as
// the clicker is connected, which is the documented behaviour without a spare
// and is the trade this device has to make: the clicker worked with the spare
// intact before Wi-Fi sync existed, and what changed is ~17 KB of Wi-Fi static
// RAM (g_cnxMgr, s_wifi_nvs, TxRxCxt, gWpaSm) that is present whether or not a
// sync ever runs.
inline wintergreen::DrawBuffer* g_buf = nullptr;
inline bool g_spare_released = false;

// Set by release_for_wifi() so the next radio_off() also deinits the
// controller. A flag rather than a parameter because radio_off() is reached
// from the HID callback path and from poll(), neither of which knows why.
inline bool g_deep_teardown = false;

// "AA:BB:CC:DD:EE:FF" -> 6 bytes, **reversed**. NimBLE's ble_addr_t.val is
// little-endian (ble_gap.c tests addr[5] & 0xC0) and esp_hidh_dev_open memcpy's
// this straight into it. Reversed, the stack hunts an address that does not
// exist and times out looking exactly like a clicker that is switched off.
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

// Map one HID input report onto a page turn. Scanned for a keycode rather than
// parsed against the report map: a clicker sends one key at a time, and a
// permissive scan works across the boot- and report-protocol layouts clickers
// disagree about. esp_hidh has already done discovery and subscription.
inline void handle_report(const uint8_t* data, uint16_t len) {
  uint8_t found = 0;
  for (uint16_t i = 0; i < len; ++i) {
    if (data[i] == kKeyLeftArrow || data[i] == kKeyRightArrow) {
      found = data[i];
      break;
    }
  }
  if (found != 0 && found != g_last_key) {
    const uint8_t btn = (found == kKeyLeftArrow) ? kBtnNext : kBtnPrev;
    g_clicker_buttons = static_cast<uint8_t>(g_clicker_buttons | (1u << btn));
  }
  g_last_key = found;
}

inline void hidh_event_cb(void*, esp_event_base_t, int32_t id, void* event_data) {
  auto* p = static_cast<esp_hidh_event_data_t*>(event_data);
  switch (static_cast<esp_hidh_event_t>(id)) {
    case ESP_HIDH_OPEN_EVENT:
      if (p->open.dev) {
        g_dev = p->open.dev;
        g_state = wintergreen::ClickerState::Connected;
      }
      break;
    case ESP_HIDH_INPUT_EVENT:
      // **Never use p->input.data.** nimble_hidh.c points it at a payload
      // inside a block it frees after esp_event_post_to *copies* the block, so
      // the pointer that arrives here is freed heap. The copy holds the payload
      // at a fixed offset, so recompute it.
      if (g_state == wintergreen::ClickerState::Connected)
        handle_report(reinterpret_cast<const uint8_t*>(p) + sizeof(esp_hidh_event_data_t), p->input.length);
      break;
    case ESP_HIDH_BATTERY_EVENT:
      // First reading only: the clicker re-notifies as its cell drains, and a
      // percentage that changes under the reader's eyes is worse than a stale
      // one. 0 is "not reported", so a device claiming 0% simply keeps showing
      // Connected.
      if (g_battery_pct == 0 && p->battery.level > 0 && p->battery.level <= 100)
        g_battery_pct = p->battery.level;
      break;
    case ESP_HIDH_CLOSE_EVENT:
      // The device must be freed here and nowhere else.
      if (p->close.dev) {
        esp_hidh_dev_free(p->close.dev);
        if (p->close.dev == g_dev)
          g_dev = nullptr;
      }
      g_disconnect_request = true;  // gone; power down rather than reconnect
      break;
    default:
      break;
  }
}

// ── Scan ───────────────────────────────────────────────────────────────────
inline constexpr uint16_t kHidServiceUuid = 0x1812;
// Short on purpose: clickers advertise in a burst after a keypress and then go
// quiet, so every extra second risks the device we just found becoming
// unconnectable.
inline constexpr int32_t kScanMs = 5000;

inline uint8_t g_want_mac[6] = {};
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
      ble_gap_disc_cancel();
      g_scan_done = true;
    } else if (!g_have_hid && adv_has_hid_service(&ev->disc)) {
      g_hid_candidate = ev->disc.addr;
      g_have_hid = true;
    }
  } else if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
    g_scan_done = true;
  }
  return 0;
}

inline bool scan_for_clicker() {
  g_have_target = false;
  g_have_hid = false;
  g_scan_done = false;

  uint8_t own_addr_type = 0;
  if (ble_hs_id_infer_auto(0, &own_addr_type) != 0)
    return false;

  struct ble_gap_disc_params params = {};
  params.passive = 0;  // active: some clickers put the HID UUID in the scan response
  params.filter_duplicates = 1;
  if (ble_gap_disc(own_addr_type, kScanMs, &params, scan_cb, nullptr) != 0)
    return false;

  for (int waited = 0; !g_scan_done && waited < kScanMs + 2000; waited += 50)
    vTaskDelay(pdMS_TO_TICKS(50));

  // Discovery must be fully stopped before connecting — ble_gap_connect returns
  // BLE_HS_EBUSY while it is active, and the cancel issued from the callback is
  // not necessarily complete when this task wakes.
  ble_gap_disc_cancel();
  for (int waited = 0; ble_gap_disc_active() && waited < 1000; waited += 20)
    vTaskDelay(pdMS_TO_TICKS(20));

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

// ── Battery ────────────────────────────────────────────────────────────────
// One explicit GATT read of the Battery Level characteristic (0x2A19) straight
// after connecting. ble_gattc_read_by_uuid searches the whole handle range, so
// no service discovery is needed on our side — esp_hidh has already walked the
// peer and this is a single round trip.
inline constexpr uint16_t kBatteryLevelChrUuid = 0x2A19;
inline volatile bool g_battery_read_done = false;

inline int battery_read_cb(uint16_t, const struct ble_gatt_error* err, struct ble_gatt_attr* attr, void*) {
  if (err && err->status == 0 && attr && attr->om && OS_MBUF_PKTLEN(attr->om) >= 1) {
    uint8_t level = 0;
    if (ble_hs_mbuf_to_flat(attr->om, &level, sizeof(level), nullptr) == 0 && level > 0 && level <= 100)
      g_battery_pct = level;
  }
  g_battery_read_done = true;
  return 0;
}

inline void read_battery_once() {
  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find_by_addr(&g_target, &desc) != 0)
    return;

  ble_uuid16_t uuid = BLE_UUID16_INIT(kBatteryLevelChrUuid);
  g_battery_read_done = false;
  if (ble_gattc_read_by_uuid(desc.conn_handle, 0x0001, 0xFFFF, &uuid.u, battery_read_cb, nullptr) != 0)
    return;
  // Bounded: a clicker with no battery service answers "attribute not found"
  // rather than hanging, but never block the connect on a peer that ignores us.
  for (int waited = 0; !g_battery_read_done && waited < 2000; waited += 20)
    vTaskDelay(pdMS_TO_TICKS(20));
}

inline void host_task(void*) {
  nimble_port_run();  // returns only after nimble_port_stop()
  nimble_port_freertos_deinit();
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
  if (g_host_up)
    ble_gap_disc_cancel();
  if (g_dev) {
    esp_hidh_dev_close(g_dev);
    g_dev = nullptr;
    // Let the disconnect reach the peer and the CLOSE event land before the
    // controller goes away underneath it.
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (g_ctrl_enabled) {
    esp_bt_controller_disable();
    g_ctrl_enabled = false;
    g_host_up = false;  // resync on the next enable
  }
  if (g_deep_teardown) {
    // Hands the controller's own allocation back, which disable() alone does
    // not: disable() returns it to INITED, still holding its buffers. Only
    // reached from release_for_wifi() — see there for why this is not the
    // default.
    esp_bt_controller_deinit();
    g_ctrl_inited = false;
    g_deep_teardown = false;
  }
  // Page turns go back to being a memcpy. Reached from every path out of the
  // radio — toggling off, a failed bringup (connect_task's `if (!ok)`) and
  // poll() acting on a disconnect — so the spare is never left released.
  // Guarded by g_spare_released because bringup only takes it under pressure.
  if (g_buf && g_spare_released) {
    g_buf->reacquire_spare();
    g_spare_released = false;
  }

  g_last_key = 0;
  g_battery_pct = 0;
  g_state = wintergreen::ClickerState::Disconnected;
}

inline bool bringup_stack() {

  // NVS backs the bond store and nothing else here uses it, so a device with
  // the radio off never pays for it.
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs = nvs_flash_init();
  }
  if (nvs != ESP_OK)
    return false;

  // **Unconditional.** A conditional release (only below some free-heap
  // threshold) was tried and is why a connect could refuse or die depending on
  // how fragmented the heap happened to be — a sync in between changed the
  // answer, which is what made this look like a Wi-Fi interaction. The radio
  // gets the 48 KB every time, so a connect attempt behaves the same on a
  // fresh boot as after an hour of reading.
  if (g_buf && !g_spare_released) {
    g_buf->release_spare();
    g_spare_released = true;
  }

  if (!g_ctrl_inited) {
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&cfg) != ESP_OK)
      return false;
    g_ctrl_inited = true;
  }

  // Separate from init, and re-run after every radio_off().
  if (!g_ctrl_enabled) {
    if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK)
      return false;
    g_ctrl_enabled = true;
  }

  if (!g_nimble_up) {
    if (esp_nimble_init() != ESP_OK)
      return false;
    g_nimble_up = true;
  }

  if (!g_hidh_up) {
    esp_hidh_config_t hidh_cfg = {};
    hidh_cfg.callback = hidh_event_cb;
    hidh_cfg.event_stack_size = 6144;  // dispatches OPEN/INPUT/CLOSE
    if (esp_hidh_init(&hidh_cfg) != ESP_OK)
      return false;
    g_hidh_up = true;

    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

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
    // reports ESP_OK, so a host task that could not be allocated shows up as
    // the sync timeout below rather than here.
    if (esp_nimble_enable(reinterpret_cast<void*>(host_task)) != ESP_OK)
      return false;
    g_host_task_started = true;
  }

  // Waited for on every bringup, not just the first: after radio_off() the
  // controller was disabled underneath the host, and re-enabling it runs a
  // fresh HCI reset that has to complete before any GAP call is legal.
  if (!g_host_up) {
    for (int i = 0; i < 1000 && !ble_hs_synced(); ++i)
      vTaskDelay(pdMS_TO_TICKS(10));
    if (!ble_hs_synced())
      return false;
    g_host_up = true;
  }
  return true;
}

inline void connect_task(void*) {
  uint8_t mac[6];
  bool ok = parse_mac(WG_BLUETOOTH_PAGE_TURNER, mac) && bringup_stack();

  if (ok) {
    memcpy(g_want_mac, mac, 6);
    ok = scan_for_clicker();
  }

  if (ok) {
    // Connect to the address and type the scan just observed, not to what the
    // config guessed. esp_ble_hidh_dev_open returns non-null as soon as the
    // link is up; discovery happens after and cannot fail this call.
    esp_hidh_dev_t* dev = esp_hidh_dev_open(g_target.val, ESP_HID_TRANSPORT_BLE, g_target.type);
    ok = (dev != nullptr);
    if (ok)
      g_dev = dev;
  }

  // Every failure looks the same from here: back to Disconnected, radio off,
  // no retry.
  if (ok) {
    read_battery_once();  // before Connected, so the row never shows a blank %
    g_state = wintergreen::ClickerState::Connected;
  } else {
    radio_off();
  }

  g_busy = false;
  vTaskDelete(nullptr);
}

// ── Public API ─────────────────────────────────────────────────────────────

// The spare framebuffer this hands to the radio for the length of a connection.
// Call once at startup; without it the clicker cannot allocate — see g_buf.
inline void bind(wintergreen::DrawBuffer& buf) {
  g_buf = &buf;
}

// Take the clicker down as far as it can go, for a Wi-Fi sync.
//
// radio_off() alone is not enough: it disables the controller but leaves it
// *initialised*, still holding its buffers, so esp_wifi_init() came up ~18 KB
// short of the ~50 KB it needs and the sync failed instantly. This also deinits
// the controller.
//
// What it deliberately does **not** touch is the NimBLE host and esp_hidh. With
// CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC=y, esp_nimble_deinit() leaves ble_hs_ctx
// dangling and the next esp_nimble_init() panics in os_mempool_init — cycling
// the host is not supported by this IDF and fails by crashing. They stay up and
// quiescent; bringup_stack() skips them on a reconnect and re-inits only the
// controller, so pressing the clicker row again after a sync works without a
// reboot.
inline void release_for_wifi() {
  g_deep_teardown = true;
  radio_off();
}

inline wintergreen::ClickerState state() {
  return g_state;
}

// Clicker battery percentage, or 0 when it has not reported one.
inline uint8_t battery_pct() {
  return g_battery_pct;
}

inline void toggle() {
  // A press while a connect is still running means "give up", not "ignore me".
  // Without this the row is inert for the whole attempt, and if the worker ever
  // fails to clear g_busy — an abort mid-discovery, a peer that vanishes with
  // no CLOSE event — the clicker is stuck reading Connected until a reboot,
  // with the row refusing to disconnect it.
  if (g_busy) {
    if (g_state == wintergreen::ClickerState::Connecting)
      g_disconnect_request = true;  // poll() tears down once the worker exits
    return;
  }
  g_busy = true;
  // Anything not Connected is an off state: toggling retries.
  if (g_state != wintergreen::ClickerState::Connected) {
    g_state = wintergreen::ClickerState::Connecting;
    g_disconnect_request = false;
    // 6 KB: esp_hidh_dev_open runs GATT discovery and report-map parsing on
    // this stack. Freed as soon as the connect finishes, either way.
    if (xTaskCreate(connect_task, "wg_bt_up", 6144, nullptr, 4, nullptr) != pdPASS) {
      g_state = wintergreen::ClickerState::Disconnected;
      g_busy = false;
    }
  } else {
    // Direct: radio_off() closes the link and disables the controller. It
    // sleeps ~100 ms letting the disconnect land, which is short enough to run
    // on the UI thread and avoids a task for a one-shot teardown.
    radio_off();
    g_busy = false;
  }
}

// Called every UI frame. Carries out the disconnect the HID callback asked for
// — that callback runs on esp_hidh's event task and must not close the device
// whose event it is dispatching.
inline void poll() {
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
inline void bind(wintergreen::DrawBuffer&) {}
inline void release_for_wifi() {}
}  // namespace wg_clicker

#endif  // WG_BLUETOOTH_PAGE_TURNER
