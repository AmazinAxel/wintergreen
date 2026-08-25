#pragma once

#include <cstdint>
#include <optional>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/adc_types.h"
#include "wintergreen/Runtime.h"

#include "bluetooth_clicker.h"
#include "wifi_sync.h"

// Battery sense sits on GPIO0 = ADC1 channel 0, behind a 2:1 divider.
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0

class Esp32Runtime final : public wintergreen::IRuntime {
 public:
  explicit Esp32Runtime(uint32_t frame_time_ms, adc_oneshot_unit_handle_t adc_handle)
      : target_frame_ms_(frame_time_ms), last_frame_ms_(frame_time_ms), frame_start_ms_(0), adc1_handle_(adc_handle) {
    init_battery_adc();
    init_pm_();
  }

  ~Esp32Runtime() override {
    if (adc_cali_handle_) {
      adc_cali_delete_scheme_curve_fitting(adc_cali_handle_);
    }
  }


  // Measured duration of the last frame, not the target — Application uses it as
  // dt for the auto-sleep countdown, which drifts if the nominal value is returned.
  uint32_t frame_time_ms() const override {
    return last_frame_ms_;
  }

  void wait_next_frame() override {
    const uint32_t now = millis();
    if (frame_start_ms_ != 0) {
      const uint32_t elapsed = now - frame_start_ms_;
      if (elapsed < target_frame_ms_)
        vTaskDelay(pdMS_TO_TICKS(target_frame_ms_ - elapsed));
      else
        vTaskDelay(1);  // 1 tick; pdMS_TO_TICKS(1) rounds to 0 and only yields
    }
    const uint32_t frame_end = millis();
    if (frame_start_ms_ != 0)
      last_frame_ms_ = frame_end - frame_start_ms_;
    frame_start_ms_ = frame_end;
  }

  // Battery terminal voltage in millivolts, or 0 if it cannot be read.
  // Everything else derives from this: percentage for the header, and the
  // low-battery cutoff.
  int battery_millivolts() const override {
    if (!adc1_handle_)
      return 0;
    int adc_raw = 0;
    if (adc_oneshot_read(adc1_handle_, BATTERY_ADC_CHANNEL, &adc_raw) != ESP_OK)
      return 0;
    int voltage_mv = 0;
    if (adc_cali_handle_)
      adc_cali_raw_to_voltage(adc_cali_handle_, adc_raw, &voltage_mv);
    else
      return 0;  // uncalibrated: a wrong voltage here would strand the device
    return voltage_mv * 2;  // voltage divider
  }

  std::optional<uint8_t> battery_percentage() const override {
    const int mv_i = battery_millivolts();
    if (mv_i == 0)
      return std::nullopt;
    const int64_t mv = mv_i;

    // Percent = -144.9390·v³ + 1655.8629·v² - 6158.8520·v + 7501.3202, v in volts,
    // from LiPo discharge samples. Evaluated in int64 rather than double: the C3
    // has no FPU, so the original expression pulled the soft-double library into
    // flash and ran it on every redraw. Coefficients are scaled by 1e4 and v by
    // 1e3, so the sum carries 1e13; the worst-case term is ~3e17, well inside
    // int64. The result is bit-comparable to the float version at the one-percent
    // resolution it is displayed at.
    constexpr int64_t kScale = 10000000000000LL;  // 1e13
    const int64_t n = -1449390LL * mv * mv * mv
                    + 16558629000LL * mv * mv
                    - 61588520000000LL * mv
                    + 75013202000000000LL;
    int pct = static_cast<int>((n + kScale / 2) / kScale);  // round to nearest
    if (pct < 0)
      pct = 0;
    else if (pct > 100)
      pct = 100;

    // Hysteresis: only update the displayed value when the new reading differs
    // by at least kHysteresisPercent. Without it, voltage noise flickers the
    // indicator between adjacent percentages every time a screen redraws.
    const int shown = last_pct_.has_value() ? static_cast<int>(*last_pct_) : 0;
    const int delta = pct > shown ? pct - shown : shown - pct;
    if (!last_pct_.has_value() || delta >= kHysteresisPercent)
      last_pct_ = static_cast<uint8_t>(pct);
    return last_pct_;
  }


  // Dynamic frequency scaling. The lock is held only while the UI is working;
  // see IRuntime::set_performance_hold for why layout needs it explicitly.
  void set_performance_hold(bool on) override {
    if (!pm_lock_ || on == pm_held_)
      return;
    if (on)
      esp_pm_lock_acquire(pm_lock_);
    else
      esp_pm_lock_release(pm_lock_);
    pm_held_ = on;
  }

  // BLE page-turner clicker. Both are thin: the state machine and every BLE
  // call live in bluetooth_clicker.h, and compile away completely when no MAC
  // is configured.
  wintergreen::ClickerState clicker_state() const override {
    return wg_clicker::state();
  }
  void toggle_clicker() override {
    wg_clicker::toggle();
  }
  uint8_t clicker_battery_pct() const override {
    return wg_clicker::battery_pct();
  }

  // NAS sync. Thin for the same reason as the clicker above: everything lives
  // in wifi_sync.h and compiles away when WG_WIFI_SYNC is undefined.
  wintergreen::SyncState sync_state() const override {
    return wg_sync::state();
  }
  void start_sync() override {
    wg_sync::start();
  }
  // TEMPORARY — remove with wg_sync::FailStage.
  uint8_t sync_fail_stage() const override {
    return wg_sync::fail_stage();
  }
  uint32_t sync_fail_heap_kb() const override {
    return wg_sync::fail_heap_kb();
  }

 private:
  // Dynamic frequency scaling. Note this is the only thing in the tree whose
  // behaviour differs between USB-attached and battery, because the
  // USB Serial/JTAG driver holds a PM lock that pins the clock — so a DFS fault
  // is invisible on USB and appears only on battery. Test on battery.
  void init_pm_() {
    esp_pm_config_t cfg{};
    cfg.max_freq_mhz = 160;
    // Never below 80: APB follows the CPU clock on the C3, and the SD-over-SPI
    // timing has no margin left at 20 MHz as it is.
    cfg.min_freq_mhz = 80;
    // Automatic light sleep breaks the ADC-ladder buttons — see sdkconfig.defaults.
    cfg.light_sleep_enable = false;
    if (esp_pm_configure(&cfg) != ESP_OK)
      return;
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ui", &pm_lock_) != ESP_OK)
      pm_lock_ = nullptr;
  }

  void init_battery_adc() {
    // Configuration for ESP32-C3 ADC1 Channel 0 (GPIO0)
    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(adc1_handle_, BATTERY_ADC_CHANNEL, &config) != ESP_OK) {
      return;
    }

    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle_) != ESP_OK) {
      adc_cali_handle_ = nullptr;
    }
  }

  static uint32_t millis() {
    return (uint32_t)(esp_timer_get_time() / 1000);
  }

  // Only update the displayed battery percentage when the reading has moved
  // at least this many percentage points away from the last displayed value.
  static constexpr int kHysteresisPercent = 3;

  uint32_t target_frame_ms_;
  uint32_t last_frame_ms_;
  uint32_t frame_start_ms_;
  adc_oneshot_unit_handle_t adc1_handle_ = nullptr;
  adc_cali_handle_t adc_cali_handle_ = nullptr;
  esp_pm_lock_handle_t pm_lock_ = nullptr;
  bool pm_held_ = false;
  mutable std::optional<uint8_t> last_pct_;
};
