#include <cstdio>
#include <new>  // set_new_handler, see reclaim_on_oom_

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "epd.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "font_manager.h"
#include "input.h"
#include "WintergreenConfig.h"
#include "wintergreen/Application.h"
#include "wintergreen/Input.h"
#include "wintergreen/Runtime.h"
#include "wintergreen/content/BookIndex.h"
#include "wintergreen/display/DrawBuffer.h"
#include "runtime.h"
#include "sdcard.h"
#include "serial_communication.h"

// Every deep-sleep entry goes through here. The ROM bootloader prints its banner
// (chip, reset reason, boot mode, entry point — a few hundred characters at
// 115200 baud) on every reset, and deep-sleep wake is a reset: tens of
// milliseconds of each wake spent on UART output nobody reads. This suppresses
// it for the next wake only, which is why it is set on each entry. A cold power-on
// still prints, so a device on the bench still says what it is.
//
// Wake is the power button, level-triggered on LOW.
static constexpr gpio_num_t kPowerPin = GPIO_NUM_3;

[[noreturn]] static void deep_sleep_now_() {
  esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << kPowerPin, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_disable_rom_logging();
  esp_deep_sleep_start();
}

// The wake source is level-triggered on LOW, so sleeping with the power button
// still down would wake straight back up. Bounded, in case the pin is stuck.
static void wait_power_release_() {
  for (uint32_t waited = 0; waited < 5000 && gpio_get_level(kPowerPin) == 0; waited += 10)
    vTaskDelay(pdMS_TO_TICKS(10));
}

// When the device boots on battery (no USB), require the power button to be
// held for at least kPowerWakeupMs milliseconds before allowing boot.
// A brief accidental touch goes back to sleep immediately without any display
// activity, just like the original wintergreen firmware.
// Exception: software resets (e.g. after esptool flash) boot immediately.
static constexpr uint32_t kPowerWakeupMs = 50;

// True when a USB host is attached.
//
// This asks the USB Serial/JTAG peripheral directly. It used to read GPIO20
// (U0RXD) and call a high level "USB connected", which is only true behind an
// external UART bridge — the X4 uses the C3's *native* USB, so GPIO20 never goes
// high and the check always answered "no host". That was harmless while it only
// decided whether to require a wake-hold, and became a device-bricking bug the
// moment the battery cutoff used it to decide whether it was safe to sleep.
//
// Safe to call before usb_serial_jtag_driver_install(); it reads peripheral
// state, not driver state.
static bool usb_attached() {
  return usb_serial_jtag_is_connected();
}

static void verify_wakeup_press() {
  // If USB is connected, boot immediately.
  if (usb_attached())
    return;

  // Only require a hold check on a clean power-on (battery, no USB).
  // Crashes, panics, watchdog resets, SW resets — all boot immediately.
  if (esp_reset_reason() != ESP_RST_POWERON) {
    return;
  }

  gpio_config_t cfg{};
  cfg.pin_bit_mask = (1ULL << kPowerPin);
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&cfg);

  // Wait up to 2× the threshold; if the button isn't held long enough, sleep.
  const uint32_t deadline_ms = kPowerWakeupMs * 2;
  uint32_t held_ms = 0;
  for (uint32_t elapsed = 0; elapsed < deadline_ms; elapsed += 10) {
    vTaskDelay(pdMS_TO_TICKS(10));
    if (gpio_get_level(kPowerPin) == 0) {
      held_ms += 10;
      if (held_ms >= kPowerWakeupMs)
        return;  // confirmed long press — boot normally
    } else {
      held_ms = 0;  // button released, reset counter
    }
  }

  // Short press — go back to sleep; wake again on power button press.
  deep_sleep_now_();
}

// Refuse to come up on a flat cell.
//
// Application::update puts the device to sleep once the battery has been below
// kLowBatteryCutoffMv for a few seconds, and this is what stops the next power
// button press bringing it straight back — booting far enough to power the panel
// and mount the card would take the cell lower still.
//
// Charging is the way out. The charger IC works whether or not the SoC is
// running, so a cut-off device left on USB recovers on its own; and this check
// is skipped outright while a host is attached, so plugging in always gives a
// bootable device to flash. It runs before epd.begin(), so a refused boot never
// lights the panel.
//
// A reading of zero (cannot measure) or an implausibly low one is treated as
// "fine": stranding a device because it cannot read its own battery would be
// worse than the discharge it is trying to prevent.
static void verify_battery(const Esp32Runtime& runtime) {
  if (wintergreen::kLowBatteryCutoffMv == 0 || usb_attached())
    return;
  static constexpr int kImplausibleMv = 2500;
  // Eight back-to-back reads, averaged. These used to be 25 ms apart, keeping the
  // worst — 175 ms of sleeping on every wake, guarding against load transients
  // that cannot happen here: this runs before the panel or the card is powered,
  // so the rail is as quiet as it will ever be. Averaging still flattens ADC noise.
  int sum = 0;
  for (int i = 0; i < 8; ++i) {
    const int mv = runtime.battery_millivolts();
    if (mv < kImplausibleMv)
      return;
    sum += mv;
  }
  if (sum / 8 >= wintergreen::kLowBatteryCutoffMv)
    return;
  deep_sleep_now_();
}

// Last resort before an allocation failure becomes a reboot.
//
// With exceptions disabled a std::bad_alloc is an abort(), so every OOM in the
// reader was a full restart mid-book. This runs *before* operator new throws
// and hands back the two largest reclaimable blocks in the system — the spare
// framebuffer and the book index — giving the allocation a second chance.
//
// It is not a substitute for the caches being sized correctly; it is the net
// under them, for the page whose layout happens to land on a bad heap. If it
// frees nothing (already released, or the index is dirty) the throw proceeds
// exactly as before, so the worst case is unchanged.
static wintergreen::DrawBuffer* g_oom_buf = nullptr;

// **The reserve is what makes the rescue work with a radio up.**
//
// The handler used to free only the spare framebuffer and the book index. Both
// are already gone by the time the clicker is connected — that is exactly what
// release_ram_for_radio() does — so the handler ran, freed *nothing*, and the
// abort proceeded. Every clicker-connected crash in this reader had a live
// new_handler that could not help.
//
// This block exists solely to be freed here. It is claimed once at boot, is
// never read or written, and is large enough for the worst cold page layout
// measured over four books (24,066 B on the Odyssey) plus margin. Releasing it
// turns the one allocation that would have aborted into a slow page turn, and
// the reader keeps running in the reduced state its own guards already handle.
static void* g_oom_reserve = nullptr;
// 12 KB, not the 28 KB first tried. The reserve does not have to cover a whole
// page layout — the reader's own guards drop their caches long before this
// fires, and every failure seen on hardware has been a *small* allocation on a
// fragmented heap (192, 256, 456, 1212, 1416 bytes). What the rescue has to
// supply is one clean extent big enough for the request that lost the race,
// which is far less than a page.
//
// Staying small also matters because this competes with the radios for the
// single ~50 KB internal DRAM pool: 28 KB of it was over half, and that is what
// stopped the clicker connecting at all.
static constexpr size_t kOomReserveBytes = 12 * 1024;

// **The radio needs this block back, and holding it broke the clicker.**
//
// The BT controller wants a contiguous allocation out of the single ~50 KB
// internal DRAM pool this device has (see the note in bluetooth_clicker.h).
// Reserving 28 KB of that pool at boot (the first size tried) left too little
// for it: the connect
// worker could not start, toggle() cleared g_state back to Disconnected, and the
// row looked like the button had done nothing — it never even said "Connecting".
//
// So the reserve is given back on the way up, the same way DrawBuffer's spare
// is, and re-claimed by the frame loop once the radio is down again and the
// heap has room. wg_clicker calls this through the hook below.
void wg_release_oom_reserve() {
  if (g_oom_reserve) {
    heap_caps_free(g_oom_reserve);
    g_oom_reserve = nullptr;
  }
}

static void reclaim_on_oom_() {
  // **Must disarm itself.** operator new calls the handler in a loop and only
  // stops when the handler frees nothing *and* clears itself — a handler that
  // returns without freeing spins forever. Clearing first means one rescue
  // attempt per re-arm, which app_main does at each frame boundary.
  std::set_new_handler(nullptr);
  // Reserve first: it is the only one guaranteed to still be holding memory.
  if (g_oom_reserve) {
    heap_caps_free(g_oom_reserve);  // IDF documents free() as equivalent; explicit for clarity
    g_oom_reserve = nullptr;
  }
  if (g_oom_buf)
    g_oom_buf->release_spare();
  wintergreen::BookIndex::instance().release_memory();
}

extern "C" void app_main(void) {
  // Claimed before anything else allocates, so it is guaranteed to exist and to
  // come out of internal RAM. Never read; it is a rope for reclaim_on_oom_.
  g_oom_reserve = heap_caps_malloc(kOomReserveBytes, MALLOC_CAP_INTERNAL);
  std::set_new_handler(reclaim_on_oom_);
  verify_wakeup_press();

  static Esp32InputSource input;
  static EInkDisplay epd;
  // 25 ms UI frame (40 Hz) — see docs/power-and-input.md "Idle power" for the timing rationale.
  static Esp32Runtime runtime(25, input.get_adc_handle());
  // The constructor configures DFS with nothing holding the clock up, so the
  // whole boot — card mount, index, book open, layout, the first frame's SPI —
  // ran at the 80 MHz idle clock. Every wake is a boot. Application::update
  // drops this hold after its usual idle interval.
  runtime.set_performance_hold(true);
  static wintergreen::Application app;
  static wintergreen::DrawBuffer buf(epd);
  g_oom_buf = &buf;  // the spare is the largest block the OOM rescue can free

  // Before anything touches the panel or the card.
  verify_battery(runtime);

  // The sleep image is a raw partition written by esptool, so this is an mmap
  // and nothing else — no copy, no inflate, no SD access on the sleep path.
  static RawPartition sleep_part;
  if (sleep_part.map("sleep"))
    buf.set_sleep_image(sleep_part.data, sleep_part.size);

  epd.begin();

  // Mount the SD card (shares SPI2 with the display). Without it there is no
  // library, no settings and no index — every screen would be an empty shell —
  // so refuse to boot the same way a flat battery does: wordmark, then sleep.
  // The panel is settling from its reset pulse meanwhile; see EInkDisplay::begin.
  const bool sd_ok = sd_init();
  epd.finish_begin();
  if (!sd_ok) {
    buf.show_sleep_image_embedded();
    buf.deep_sleep();
    wait_power_release_();
    deep_sleep_now_();
  }
  // State lives in two dotfiles at the card root — see set_state_root.
  app.set_books_dir("/sdcard");
  app.set_state_root("/sdcard");

  static FontManager font_mgr(app);
  font_mgr.init();  // hands the reader font to the app

  // The runtime's start_sync() reaches neither of these, and a sync needs both:
  // the app for the book index and card paths, the buffer to stay off SPI2
  // while the panel is mid-waveform.
  wg_sync::bind(app, buf);

  // The clicker borrows the same spare framebuffer, but only when the heap is
  // too tight for the BT controller to allocate beside it.
  wg_clicker::bind(buf);

  app.start(buf, runtime);

  // Discard the power-button press that woke us from deep sleep.
  input.clear_button(wintergreen::Button::Power);

  while (app.running()) {
    // A host is attached: bring the serial receiver up (once) and hold off
    // auto-sleep for as long as it stays attached.
    if (usb_serial_jtag_is_connected()) {
      serial_start_if_connected();
      app.keep_awake();
    }

    // Process pending index mutation (upload via EPUB magic or 'W' command,
    // delete via 'R', rename via 'N'). Single-slot SPSC queue: receiver task
    // is the producer, this loop is the consumer.
    //
    // Deferral rules:
    //   (A) Add/Rename open the book to read its metadata and may reload the
    //       ~30 KB index — defer while the reader is the top screen, which is
    //       using the heap for its own caches.
    //   (B) ALL ops do SD card I/O (fopen/fprintf in save/load) which shares
    //       SPI2_HOST with the display. Defer when the EPD hardware is
    //       mid-refresh. This prevents SPI contention that corrupts the
    //       display during SD card writes.
    if (g_index_op != SerialIndexOp::None) {
      const SerialIndexOp op = g_index_op;
      std::atomic_thread_fence(std::memory_order_acquire);
      const bool opens_book = (op == SerialIndexOp::Add || op == SerialIndexOp::Rename);
      const bool defer = (opens_book && app.is_reader_active()) || epd.is_busy();

      if (defer) {
        // Leave the slot occupied; retry next iteration. The main loop
        // continues to run (UI updates, serial commands) between retries.
      } else {
        // Copy paths to locals BEFORE clearing the slot — minimizes the window
        // in which a new op would be dropped.
        char path_a[256];
        char path_b[256];
        strncpy(path_a, g_index_path_a, sizeof(path_a) - 1);
        path_a[sizeof(path_a) - 1] = '\0';
        strncpy(path_b, g_index_path_b, sizeof(path_b) - 1);
        path_b[sizeof(path_b) - 1] = '\0';
        std::atomic_thread_fence(std::memory_order_release);  // copies above happen first
        g_index_op = SerialIndexOp::None;  // free slot before processing

        const std::string& index_path = app.index_path();
        switch (op) {
          case SerialIndexOp::Add:
            (void)wintergreen::BookIndex::instance().index_file(path_a, index_path);
            break;
          case SerialIndexOp::Remove:
            wintergreen::BookIndex::instance().remove_path(path_a, index_path);
            break;
          case SerialIndexOp::Rename:
            // Fast path: in-place rename preserves metadata + last_open_order.
            // Fallback: src wasn't indexed → index dst fresh (extracts metadata).
            if (!wintergreen::BookIndex::instance().rename_in_place(path_a, path_b, index_path)) {
              (void)wintergreen::BookIndex::instance().index_file(path_b, index_path);
            }
            break;
          default:
            break;
        }
      }
    }

    // Dispatch serial path commands.
    {
      const char* cmd_path = nullptr;
      if (serial_cmd_take(&cmd_path) == SerialCmdType::Open) {
        app.auto_open_book(cmd_path, buf, runtime);
        // auto_open_book pushes the Reader and renders the page into the
        // inactive buffer, but does not commit it. We must refresh here to
        // show the book page on the display (the Application::start() path
        // has its own buf.full_refresh() after auto_open_book returns).
        buf.refresh();
      }
    }

    // Skip UI update during upload: prevents display SPI (SPI2_HOST) from
    // contending with SD-card fwrite() (also SPI2_HOST).
    if (g_upload_in_progress) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Carry out a BLE teardown the HID callback asked for. Compiles to nothing
    // unless a clicker MAC is configured; see bluetooth_clicker.h for why the
    // callback cannot tear its own stack down.
    wg_clicker::g_in_reader = app.is_reader_active();
    wg_clicker::poll();
    // Re-arm the out-of-memory rescue once per frame. It disarms itself when it
    // fires, so without this it would rescue exactly one allocation per boot.
    //
    // Re-claim the reserve too, but only when the heap has recovered enough that
    // taking it back cannot itself cause the shortage it exists to prevent.
    // Without this the rope is single-use: the first rescue spends it and every
    // later page turn is unprotected again.
    //
    // **Never while a radio holds RAM.** The reserve comes out of the same ~50 KB
    // internal pool the BT controller needs a contiguous block from, so taking it
    // back mid-session would starve the stack that is already up — and taking it
    // at all is what stopped the clicker connecting in the first place.
    //
    // The re-claim is gated on the reserve being spent: in the steady state —
    // every frame of an ordinary reading session — that is one pointer test
    // instead of a heap free-list walk 40 times a second. The re-arm is *not*
    // gated on it, because the handler disarms itself on any rescue and its
    // other two levers (the spare framebuffer, the book index) can fire while
    // the reserve is still held.
    if (!g_oom_reserve && !runtime.clicker_holds_ram() &&
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) > kOomReserveBytes + 16 * 1024)
      g_oom_reserve = heap_caps_malloc(kOomReserveBytes, MALLOC_CAP_INTERNAL);
    std::set_new_handler(reclaim_on_oom_);

    // Hold off auto-sleep while a sync is in flight: a multi-book transfer
    // easily outlasts kAutoSleepMinutes, and sleeping mid-download would cut
    // the radio with files half written.
    wg_sync::poll();

    const wintergreen::ButtonState buttons = input.poll_buttons();
    app.update(buttons, runtime.frame_time_ms(), buf, runtime);
    runtime.wait_next_frame();
  }

  // Hold-to-sleep leaves the power button still down.
  wait_power_release_();
  deep_sleep_now_();
}
