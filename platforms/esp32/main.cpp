#include <cstdio>

#include "driver/gpio.h"
#include "epd.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "font_manager.h"
#include "input.h"
#include "WintergreenConfig.h"
#include "wintergreen/Application.h"
#include "wintergreen/Input.h"
#include "wintergreen/Runtime.h"
#include "wintergreen/display/DrawBuffer.h"
#include "runtime.h"
#include "sdcard.h"

// Every deep-sleep entry goes through here. The ROM bootloader prints its banner
// (chip, reset reason, boot mode, entry point — a few hundred characters at
// 115200 baud) on every reset, and deep-sleep wake is a reset: tens of
// milliseconds of each wake spent on UART output nobody reads. This suppresses
// it for the next wake only, which is why it is set on each entry. A cold power-on
// still prints, so a device on the bench still says what it is.
//
// Wake is the power button, level-triggered on LOW.
static constexpr gpio_num_t kPowerPin = GPIO_NUM_3;

// No pin is held through deep sleep. Firmware from 2026-11-11 held SD CS
// (GPIO12), panel CS and panel RST high; deep-sleep drain rose with it (GPIO12
// sits in the VDD_SPI pad group). Released first thing in app_main so a device
// that went to sleep under that firmware gets its pins back: a held CS cannot
// be driven.
static constexpr gpio_num_t kOldSleepHoldPins[] = {GPIO_NUM_12, EPD_CS, EPD_RST};

static void release_sleep_holds_() {
  gpio_deep_sleep_hold_dis();
  for (gpio_num_t p : kOldSleepHoldPins)
    gpio_hold_dis(p);
}

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

// On a clean power-on, require the power button to be held for at least
// kPowerWakeupMs milliseconds before allowing boot. USB makes no difference.
// A brief accidental touch goes back to sleep immediately without any display
// activity, just like the original wintergreen firmware.
// Exception: software resets (e.g. after esptool flash) boot immediately.
static constexpr uint32_t kPowerWakeupMs = 50;

static void verify_wakeup_press() {
  // Only require a hold check on a clean power-on.
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
// running, so a cut-off device left on USB recovers on its own. USB does not
// bypass this check: flashing a refused device means starting the upload and
// holding power (each wake boot-loops through here, enumerating USB). It runs
// before epd.begin(), so a refused boot never lights the panel.
//
// A reading of zero (cannot measure) or an implausibly low one is treated as
// "fine": stranding a device because it cannot read its own battery would be
// worse than the discharge it is trying to prevent.
static void verify_battery(const Esp32Runtime& runtime) {
  if (wintergreen::kLowBatteryCutoffMv == 0)
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

// No out-of-memory rescue. Every long-lived buffer (both framebuffers, the
// reader's page buffer, the book index) is claimed at boot and kept; a page
// turn allocates nothing; the radios come and go beside them without anything
// being handed back. See docs/history.md "OOM rescue".
extern "C" void app_main(void) {
  release_sleep_holds_();
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

  app.start(buf, runtime);

  // Discard the power-button press that woke us from deep sleep.
  input.clear_button(wintergreen::Button::Power);

  while (app.running()) {
    // Carry out a BLE teardown the HID callback asked for. Compiles to nothing
    // unless a clicker MAC is configured; see bluetooth_clicker.h for why the
    // callback cannot tear its own stack down.
    wg_clicker::poll();

    // Hold off auto-sleep while a sync is in flight (a multi-book transfer
    // easily outlasts kAutoSleepMinutes, and sleeping mid-download would cut
    // the radio with files half written), and reconnect the clicker once a
    // sync that dropped it has finished.
    wg_sync::poll();

    // Every button is dead while a sync runs, power included (owner's call): no
    // sleeping or navigating away mid-download. The sync always ends on its own
    // (HTTP and Wi-Fi timeouts).
    wintergreen::ButtonState buttons = input.poll_buttons();
    if (wg_sync::active())
      buttons = {};
    app.update(buttons, runtime.frame_time_ms(), buf, runtime);

    // Sleep as long as nothing is due. A radio in transition keeps the frame
    // cadence (its rows repaint as state changes); a clicker key down shortens
    // the wait to exactly its hold deadline.
    uint32_t budget = app.idle_wait_ms(buf);
    if (wg_sync::active() || wg_clicker::busy())
      budget = 0;
    const uint32_t hold = wg_clicker::hold_wait_ms();
    if (hold < (budget ? budget : runtime.target_frame_ms()))
      budget = hold;
    runtime.wait_next_frame(budget);
  }

  // Hold-to-sleep leaves the power button still down.
  wait_power_release_();
  deep_sleep_now_();
}
