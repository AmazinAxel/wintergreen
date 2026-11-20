#pragma once

// EInkDisplay driver for ESP-IDF (SSD1677): custom LUTs (panel_lut.h), strip
// windows queued over DMA, temperature bands from the C3's own sensor.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/temperature_sensor.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "panel_lut.h"
#include "wintergreen/display/DrawBuffer.h"

// ---- Pin assignments ----
#define EPD_SCLK GPIO_NUM_8
#define EPD_MOSI GPIO_NUM_10
#define EPD_CS GPIO_NUM_21
#define EPD_DC GPIO_NUM_4
#define EPD_RST GPIO_NUM_5
#define EPD_BUSY GPIO_NUM_6

// ---- SSD1677 command definitions ----
#define CMD_SOFT_RESET 0x12
#define CMD_BOOSTER_SOFT_START 0x0C
#define CMD_DRIVER_OUTPUT_CONTROL 0x01
#define CMD_BORDER_WAVEFORM 0x3C
#define CMD_TEMP_SENSOR_CONTROL 0x18
#define CMD_DATA_ENTRY_MODE 0x11
#define CMD_SET_RAM_X_RANGE 0x44
#define CMD_SET_RAM_Y_RANGE 0x45
#define CMD_SET_RAM_X_COUNTER 0x4E
#define CMD_SET_RAM_Y_COUNTER 0x4F
#define CMD_WRITE_RAM_BW 0x24
#define CMD_WRITE_RAM_RED 0x26
#define CMD_DISPLAY_UPDATE_CTRL1 0x21
#define CMD_DISPLAY_UPDATE_CTRL2 0x22
#define CMD_MASTER_ACTIVATION 0x20
#define CTRL1_NORMAL 0x00
#define CMD_GATE_VOLTAGE 0x03
#define CMD_SOURCE_VOLTAGE 0x04
#define CMD_WRITE_VCOM 0x2C
#define CMD_WRITE_LUT 0x32
#define CMD_DEEP_SLEEP 0x10

class EInkDisplay : public wintergreen::IDisplay {
 public:
  static constexpr uint16_t DISPLAY_WIDTH = wintergreen::DisplayFrame::kPanelWidth;  // 800 (full panel)
  static constexpr uint16_t DISPLAY_HEIGHT = wintergreen::DisplayFrame::kPhysicalHeight;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = wintergreen::DisplayFrame::kStride;
  static constexpr uint32_t BUFFER_SIZE = wintergreen::DisplayFrame::kPixelBytes;

  bool isScreenOn = false;

  // Given by the BUSY falling edge (end of a waveform). waitWhileBusy() blocks
  // on it instead of polling the pin once a tick: at FREERTOS_HZ=100 a poll
  // saw the end of a waveform up to 10 ms late, and every frame chained behind
  // a waveform (a menu after a page turn, a turn during a turn) paid it.
  SemaphoreHandle_t busy_done_ = nullptr;
  static void busy_isr_(void* arg) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(static_cast<EInkDisplay*>(arg)->busy_done_, &woken);
    if (woken)
      portYIELD_FROM_ISR();
  }
  bool inDeepSleep_ = false;
  // When begin() let go of RST; finish_begin() waits out the rest of the settle.
  static constexpr uint32_t kResetSettleMs = 20;
  uint32_t reset_released_ms_ = 0;

  bool is_busy() const override {
    return gpio_get_level(EPD_BUSY) == 1;
  }

  void wait_idle() override {
    waitWhileBusy();
  }

  spi_device_handle_t spi_;

  void begin() {
    // GPIO outputs: DC, RST (CS is the SPI peripheral's, see below)
    gpio_config_t out_cfg{};
    out_cfg.pin_bit_mask = (1ULL << EPD_DC) | (1ULL << EPD_RST);
    out_cfg.mode = GPIO_MODE_OUTPUT;
    out_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    out_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&out_cfg);

    // GPIO input: BUSY
    gpio_config_t in_cfg{};
    in_cfg.pin_bit_mask = (1ULL << EPD_BUSY);
    in_cfg.mode = GPIO_MODE_INPUT;
    in_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    in_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    in_cfg.intr_type = GPIO_INTR_NEGEDGE;
    gpio_config(&in_cfg);
    // Flags 0: not IRAM-safe, so the interrupt is held off during flash writes
    // (NVS, OTA) instead of running from disabled cache; the waiter re-checks
    // the pin, so a late edge costs nothing.
    busy_done_ = xSemaphoreCreateBinary();
    gpio_install_isr_service(0);
    gpio_isr_handler_add(EPD_BUSY, busy_isr_, this);

    gpio_set_level(EPD_DC, 1);

    spi_bus_config_t bus{};
    bus.mosi_io_num = EPD_MOSI;
    bus.miso_io_num = GPIO_NUM_7;  // shared with SD card
    bus.sclk_io_num = EPD_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);

    // CS by the peripheral and DC from each transaction's pre-callback, so a
    // whole strip (window commands, both planes) is queued at once and the CPU
    // renders the next strip while DMA sends this one.
    spi_device_interface_config_t dev{};
    dev.clock_speed_hz = 40 * 1000 * 1000;
    dev.mode = 0;
    dev.spics_io_num = EPD_CS;
    dev.queue_size = 2 * kTxPerWindow;
    dev.pre_cb = dc_pre_cb_;
    spi_bus_add_device(SPI2_HOST, &dev, &spi_);

    // The C3's die sensor: chip temperature, a few degrees above ambient.
    // Installed once; enabled only around each reading.
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &temp_) != ESP_OK)
      temp_ = nullptr;

    // Reset pulse only. The panel needs ~20 ms after RST rises before it takes
    // commands, and main.cpp spends that mounting the SD card (same bus, but
    // the panel ignores SPI with CS high) instead of sleeping through it. Every
    // wake is a boot, so this is on the path of every wake.
    gpio_set_level(EPD_RST, 1);
    delay(20);
    gpio_set_level(EPD_RST, 0);
    delay(2);
    gpio_set_level(EPD_RST, 1);
    reset_released_ms_ = millis();
  }

  // Second half of begin(): wait out whatever is left of the post-reset settle,
  // then configure the controller. No RAM clear: the first thing drawn is
  // Application::start()'s full_refresh(), which writes both planes over the
  // whole panel.
  void finish_begin() {
    const uint32_t since = millis() - reset_released_ms_;
    if (since < kResetSettleMs)
      delay(kResetSettleMs - since);
    initDisplayController();
  }

  // ---- wintergreen::IDisplay ----

  void begin_update(wintergreen::UpdateKind kind) override {
    wakeIfNeeded();
    waitWhileBusy();
    load_lut_(kind);
    sendCommandData(CMD_DATA_ENTRY_MODE, {0x01});  // X inc, Y dec
  }

  void await_strip_slot() override {
    while (windows_ > 1)
      drain_oldest_();
  }

  // One window of both RAM planes, queued: the range, the counters, BW, the
  // counters again, RED. Buffer row 0 is the window's first physical row; the
  // controller's Y runs the other way (data entry Y decrement), as the old
  // whole-frame write did.
  void write_window(int row0, int rows, int xb0, int xbytes, const uint8_t* bw, const uint8_t* red) override {
    await_strip_slot();
    spi_transaction_t* t = ring_[ring_slot_];
    int n = 0;
    const uint16_t x = static_cast<uint16_t>(xb0 * 8);
    const uint16_t x_end = static_cast<uint16_t>(x + xbytes * 8 - 1);
    const uint16_t y = static_cast<uint16_t>(DISPLAY_HEIGHT - row0 - rows);
    const uint16_t y_end = static_cast<uint16_t>(DISPLAY_HEIGHT - row0 - 1);
    auto cmd = [&](uint8_t c) { small_(t[n++], 0, {c}); };
    auto counters = [&]() {
      cmd(CMD_SET_RAM_X_COUNTER);
      small_(t[n++], 1, {lo8_(x), hi8_(x)});
      cmd(CMD_SET_RAM_Y_COUNTER);
      small_(t[n++], 1, {lo8_(y_end), hi8_(y_end)});
    };
    cmd(CMD_SET_RAM_X_RANGE);
    small_(t[n++], 1, {lo8_(x), hi8_(x), lo8_(x_end), hi8_(x_end)});
    cmd(CMD_SET_RAM_Y_RANGE);
    small_(t[n++], 1, {lo8_(y_end), hi8_(y_end), lo8_(y), hi8_(y)});
    counters();
    cmd(CMD_WRITE_RAM_BW);
    bulk_(t[n++], bw, static_cast<size_t>(rows) * xbytes);
    counters();
    cmd(CMD_WRITE_RAM_RED);
    bulk_(t[n++], red, static_cast<size_t>(rows) * xbytes);
    for (int k = 0; k < n; ++k)
      spi_device_queue_trans(spi_, &t[k], portMAX_DELAY);
    ring_n_[ring_slot_] = n;
    ring_slot_ ^= 1;
    ++windows_;
  }

  void finish_writes() override {
    while (windows_ > 0)
      drain_oldest_();
  }

  // Run the loaded LUT over the whole panel. CTRL1 0x00: RED is used (it is
  // half of every pixel's state). CTRL2 0x0C = DISPLAY_START + mode 2 and
  // **no LUT_LOAD**: LUT_LOAD would replace our table from OTP.
  //
  // Turn returns while the panel is still busy: every entry point opens with
  // wakeIfNeeded(); waitWhileBusy(), so the next one collects it. Full waits —
  // its callers send more (deep sleep) or power down at its end.
  void fire(wintergreen::UpdateKind kind, bool turn_off) override {
    finish_writes();
    sendCommandData(CMD_DISPLAY_UPDATE_CTRL1, {CTRL1_NORMAL});
    uint8_t mode = 0x0C;
    if (!isScreenOn) {
      isScreenOn = true;
      mode |= 0xC0;  // CLOCK_ON + ANALOG_ON (power up for the first update)
    }
    if (turn_off) {
      isScreenOn = false;
      mode |= 0x03;  // ANALOG_OFF_PHASE + CLOCK_OFF at the end of the waveform
    }
    sendCommandData(CMD_DISPLAY_UPDATE_CTRL2, {mode});
    sendCommand(CMD_MASTER_ACTIVATION);
    if (kind == wintergreen::UpdateKind::Full || turn_off)
      waitWhileBusy();
  }

  // ---------------------------------------------------------------------------
  // There is deliberately NO idle rails-off here. Do not add one back.
  //
  // It shipped: after Application::kPanelIdleOffMs (5 s) of no input the app
  // sent ANALOG_OFF_PHASE | CLOCK_OFF (0x03) with MASTER_ACTIVATION and no
  // DISPLAY_START, on the reasoning that e-ink is bistable so the page stays on
  // the glass, and that the next refresh would OR in CLOCK_ON | ANALOG_ON and
  // bring the pumps back by itself.
  //
  // On hardware the *first* page turn after the rails went down did nothing
  // visible — the old page stayed — and the turn after that drew correctly. So
  // the waveform runs before the analog rails have actually come up, and one
  // page turn is silently lost.
  //
  // The rails are still powered down where it is free: fire()'s turn_off, at
  // the end of a full refresh, which is what the sleep image uses.
  // ---------------------------------------------------------------------------

  void deep_sleep() override {
    // A turn may have left a waveform running; CMD_DEEP_SLEEP mid-update aborts
    // it and leaves a half-drawn panel on screen for the whole sleep.
    finish_writes();
    waitWhileBusy();
    sendCommand(CMD_DEEP_SLEEP);
    sendData(0x03);
    isScreenOn = false;
    inDeepSleep_ = true;
  }

 private:
  // ---- Transactions ----
  //
  // Every transaction carries its DC level in `user`; the pre-callback sets the
  // pin just before the peripheral asserts CS.
  static void dc_pre_cb_(spi_transaction_t* t) {
    gpio_set_level(EPD_DC, reinterpret_cast<uintptr_t>(t->user) ? 1 : 0);
  }
  static uint8_t lo8_(uint16_t v) {
    return static_cast<uint8_t>(v & 0xFF);
  }
  static uint8_t hi8_(uint16_t v) {
    return static_cast<uint8_t>(v >> 8);
  }
  static void small_(spi_transaction_t& t, int dc, std::initializer_list<uint8_t> bytes) {
    t = spi_transaction_t{};
    t.length = bytes.size() * 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.user = reinterpret_cast<void*>(static_cast<uintptr_t>(dc));
    std::copy(bytes.begin(), bytes.end(), t.tx_data);
  }
  static void bulk_(spi_transaction_t& t, const uint8_t* data, size_t n) {
    t = spi_transaction_t{};
    t.length = n * 8;
    t.tx_buffer = data;
    t.user = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
  }

  // Two windows of transactions in flight at most (DrawBuffer's two strip
  // buffers). The descriptors live here until their results are collected.
  static constexpr int kTxPerWindow = 16;
  spi_transaction_t ring_[2][kTxPerWindow] = {};
  int ring_n_[2] = {};
  int ring_slot_ = 0;
  int windows_ = 0;  // queued, results not yet collected

  void drain_oldest_() {
    // Results come back in queue order, so the oldest window's are next.
    const int slot = ring_slot_ ^ (windows_ == 2 ? 0 : 1);
    spi_transaction_t* done = nullptr;
    for (int k = 0; k < ring_n_[slot]; ++k)
      spi_device_get_trans_result(spi_, &done, portMAX_DELAY);
    ring_n_[slot] = 0;
    --windows_;
  }

  // ---- LUT ----

  enum class Loaded : uint8_t { None, Turn, Full };
  Loaded loaded_ = Loaded::None;  // reset by every controller init
  panel_lut::Band band_ = panel_lut::Band::Warm;
  panel_lut::Band loaded_band_ = panel_lut::Band::Warm;
  temperature_sensor_handle_t temp_ = nullptr;

  // Pick the band from the die temperature, then upload the table for `kind`
  // unless it is already loaded for that band. Upload order as the vendor's:
  // LUT (0x32, 105 B), gate voltage, source voltages, VCOM — after any OTP
  // load (there is none now) and after every controller init.
  void load_lut_(wintergreen::UpdateKind kind) {
    float c = 0;
    if (temp_ && temperature_sensor_enable(temp_) == ESP_OK) {
      if (temperature_sensor_get_celsius(temp_, &c) == ESP_OK)
        band_ = panel_lut::band_for(c, band_);
      temperature_sensor_disable(temp_);
    }
    const Loaded want = kind == wintergreen::UpdateKind::Full ? Loaded::Full : Loaded::Turn;
    if (loaded_ == want && loaded_band_ == band_)
      return;
    alignas(4) uint8_t lut[panel_lut::kLutBytes];
    if (want == Loaded::Full)
      panel_lut::build_full(lut, panel_lut::timing(band_));
    else
      panel_lut::build_turn(lut, panel_lut::timing(band_));
    sendCommand(CMD_WRITE_LUT);
    sendData(lut, sizeof(lut));
    sendCommandData(CMD_GATE_VOLTAGE, {panel_lut::kGateVoltage});
    sendCommandData(CMD_SOURCE_VOLTAGE,
                    {panel_lut::kSourceVoltage[0], panel_lut::kSourceVoltage[1], panel_lut::kSourceVoltage[2]});
    sendCommandData(CMD_WRITE_VCOM, {panel_lut::kVcom});
    loaded_ = want;
    loaded_band_ = band_;
  }

  // Exit deep sleep via hardware reset + controller re-init.
  void wakeIfNeeded() {
    if (!inDeepSleep_)
      return;
    gpio_set_level(EPD_RST, 1);
    delay(10);
    gpio_set_level(EPD_RST, 0);
    delay(20);
    gpio_set_level(EPD_RST, 1);
    delay(200);
    waitWhileBusy();
    initDisplayController();
    inDeepSleep_ = false;
  }

  static uint32_t millis() {
    return (uint32_t)(esp_timer_get_time() / 1000);
  }
  static void delay(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
  }

  // Polled transfers, only with nothing queued (begin_update, fire, deep sleep).
  void sendCommand(uint8_t command) {
    spi_transaction_t t;
    small_(t, 0, {command});
    spi_device_polling_transmit(spi_, &t);
  }

  void sendData(uint8_t data) {
    spi_transaction_t t;
    small_(t, 1, {data});
    spi_device_polling_transmit(spi_, &t);
  }

  // The LUT: 105 bytes from the stack, polled (less than an interrupt round
  // trip).
  void sendData(const uint8_t* data, uint32_t length) {
    spi_transaction_t t;
    bulk_(t, data, length);
    spi_device_polling_transmit(spi_, &t);
  }

  // A command and its (at most four) parameter bytes: two transactions.
  void sendCommandData(uint8_t command, std::initializer_list<uint8_t> data) {
    sendCommand(command);
    spi_transaction_t t;
    small_(t, 1, data);
    spi_device_polling_transmit(spi_, &t);
  }

  void waitWhileBusy() {
    uint32_t start = millis();
    while (gpio_get_level(EPD_BUSY) == 1) {
      // Woken by the falling edge; a stale give from an earlier waveform only
      // costs one more pin check.
      xSemaphoreTake(busy_done_, pdMS_TO_TICKS(10000));
      if (millis() - start > 10000) {
        break;
      }
    }
  }

  // The vendor init. CMD_TEMP_SENSOR_CONTROL 0x80 (internal sensor) stays
  // although nothing loads an OTP waveform any more (no LUT_LOAD, no
  // TEMP_LOAD): harmless, and a vendor init is not a place to remove things
  // that "should" be no-ops. Every init forgets the uploaded LUT.
  void initDisplayController() {
    sendCommand(CMD_SOFT_RESET);
    waitWhileBusy();
    loaded_ = Loaded::None;

    sendCommandData(CMD_TEMP_SENSOR_CONTROL, {0x80});
    sendCommand(CMD_BOOSTER_SOFT_START);
    sendData(0xAE);
    sendData(0xC7);
    sendData(0xC3);
    sendData(0xC0);
    sendData(0x40);

    sendCommandData(CMD_DRIVER_OUTPUT_CONTROL,
                    {static_cast<uint8_t>((DISPLAY_HEIGHT - 1) % 256), static_cast<uint8_t>((DISPLAY_HEIGHT - 1) / 256),
                     0x02});
    sendCommandData(CMD_BORDER_WAVEFORM, {0x01});
  }
};
