#include <cstdio>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "epd.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "font_manager.h"
#include "input.h"
#include "WintergreenConfig.h"
#include "wintergreen/Application.h"
#include "wintergreen/Loop.h"
#include "wintergreen/content/BookIndex.h"
#include "wintergreen/display/DrawBuffer.h"
#include "runtime.h"
#include "sdcard.h"
#include "serial_communication.h"

static void verify_ota() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state;
  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_ota_mark_app_valid_cancel_rollback();
    }
  }
}

// When the device boots on battery (no USB), require the power button to be
// held for at least kPowerWakeupMs milliseconds before allowing boot.
// A brief accidental touch goes back to sleep immediately without any display
// activity, just like the original wintergreen firmware.
// Exception: software resets (e.g. after esptool flash) boot immediately.
static constexpr gpio_num_t kPowerPin = GPIO_NUM_3;
static constexpr uint32_t kPowerWakeupMs = 250;

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
  esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << kPowerPin, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
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
  int worst = 0;
  for (int i = 0; i < 8; ++i) {
    const int mv = runtime.battery_millivolts();
    if (mv < kImplausibleMv)
      return;
    if (worst == 0 || mv < worst)
      worst = mv;
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  if (worst >= wintergreen::kLowBatteryCutoffMv)
    return;
  esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << kPowerPin, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

extern "C" void app_main(void) {
  verify_ota();
  verify_wakeup_press();

  static Esp32InputSource input;
  static EInkDisplay epd;
  // 25 ms UI frame (40 Hz) — see CLAUDE.md "Idle power" for the timing rationale.
  static Esp32Runtime runtime(25, input.get_adc_handle());
  static wintergreen::Application app;
  static wintergreen::DrawBuffer buf(epd);

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
  if (!sd_init()) {
    buf.show_sleep_image_embedded();
    // Wake is level-triggered on LOW; sleeping with the button still down would
    // wake us straight back up.
    for (uint32_t waited = 0; waited < 5000 && gpio_get_level(kPowerPin) == 0; waited += 10)
      vTaskDelay(pdMS_TO_TICKS(10));
    esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << kPowerPin, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
  }
  // State lives in two dotfiles at the card root — see set_state_root.
  app.set_books_dir("/sdcard");
  app.set_state_root("/sdcard");

  static FontManager font_mgr(app);
  font_mgr.init();
  app.set_font_manager(&font_mgr);

  // The runtime's start_sync() reaches neither of these, and a sync needs both:
  // the app for the book index and card paths, the buffer to stay off SPI2
  // while the panel is mid-waveform.
  wg_sync::bind(app, buf);

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
    //   (A) Add/Rename ops use scratch buffers (Book::open) — defer when
    //       Reader is the top screen (it owns those buffers for rendering).
    //   (C) ALL ops do SD card I/O (fopen/fprintf in save/load) which shares
    //       SPI2_HOST with the display. Defer when the EPD hardware is
    //       mid-refresh (DMA reading from framebuffer). This prevents SPI
    //       contention that corrupts the display during SD card writes.
    if (g_index_op != SerialIndexOp::None) {
      const SerialIndexOp op = g_index_op;
      const bool needs_scratch = (op == SerialIndexOp::Add || op == SerialIndexOp::Rename);
      const bool reader_active = app.is_reader_active();
      const bool epd_busy = epd.is_busy();
      const bool defer = (needs_scratch && reader_active) || epd_busy;

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
    wg_clicker::poll();

    // Hold off auto-sleep while a sync is in flight: a multi-book transfer
    // easily outlasts kAutoSleepMinutes, and sleeping mid-download would cut
    // the radio with files half written.
    wg_sync::poll();

    wintergreen::run_loop_iteration(app, buf, input, runtime);
  }

  // Hold-to-sleep leaves the power button still down, and the wake source is
  // level-triggered on LOW — sleeping now would wake instantly. Wait for the
  // release (bounded, in case the pin is stuck).
  for (uint32_t waited = 0; waited < 5000 && gpio_get_level(kPowerPin) == 0; waited += 10)
    vTaskDelay(pdMS_TO_TICKS(10));

  // Enter deep sleep; wake on power button press (active LOW, GPIO 3).
  esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << kPowerPin, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}
