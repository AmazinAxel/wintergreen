#pragma once

// EInkDisplay driver for ESP-IDF (SSD1677).

#include <cstdint>
#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
#define CTRL1_BYPASS_RED 0x40
#define CMD_WRITE_TEMP 0x1A
#define CMD_AUTO_WRITE_BW_RAM 0x46
#define CMD_AUTO_WRITE_RED_RAM 0x47
#define CMD_DEEP_SLEEP 0x10

// ---- Refresh modes (internal) ----
enum EpdRefreshMode { EPD_FULL_REFRESH, EPD_HALF_REFRESH, EPD_FAST_REFRESH };

class EInkDisplay : public wintergreen::IDisplay {
 public:
  static constexpr uint16_t DISPLAY_WIDTH = wintergreen::DisplayFrame::kPanelWidth;  // 800 (full panel)
  static constexpr uint16_t DISPLAY_HEIGHT = wintergreen::DisplayFrame::kPhysicalHeight;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = wintergreen::DisplayFrame::kStride;
  static constexpr uint32_t BUFFER_SIZE = wintergreen::DisplayFrame::kPixelBytes;

  bool isScreenOn = false;
  bool inDeepSleep_ = false;
  // millis() at MASTER_ACTIVATION of a refresh that was fired without waiting;
  // 0 = nothing outstanding. Collected by waitWhileBusy().
  uint32_t pending_refresh_start_ = 0;

  bool is_busy() const override {
    return gpio_get_level(EPD_BUSY) == 1;
  }

  void wait_idle() override {
    waitWhileBusy();
  }

  spi_device_handle_t spi_;

  void begin() {
    // GPIO outputs: CS, DC, RST
    gpio_config_t out_cfg{};
    out_cfg.pin_bit_mask = (1ULL << EPD_CS) | (1ULL << EPD_DC) | (1ULL << EPD_RST);
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
    in_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&in_cfg);

    gpio_set_level(EPD_CS, 1);
    gpio_set_level(EPD_DC, 1);

    spi_bus_config_t bus{};
    bus.mosi_io_num = EPD_MOSI;
    bus.miso_io_num = GPIO_NUM_7;  // shared with SD card
    bus.sclk_io_num = EPD_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t dev{};
    dev.clock_speed_hz = 40 * 1000 * 1000;
    dev.mode = 0;
    dev.spics_io_num = -1;
    dev.queue_size = 2;  // one in flight + one queued for the async bulk path
    spi_bus_add_device(SPI2_HOST, &dev, &spi_);

    resetDisplay();
    initDisplayController(true);
  }

  // ---- wintergreen::IDisplay ----

  // RED RAM is the *previous* frame, and only EPD_FULL_REFRESH reads it.
  //
  // The full-refresh waveform is prior-state dependent: it drives each pixel from
  // its old level to its new one, and the level it starts from is what RED holds.
  // This used to write `pixels` to both planes with CTRL1_BYPASS_RED set, which
  // told the controller to ignore RED altogether — so those 48 KB went out over
  // SPI and were discarded, and the waveform fell back to the controller's own
  // notion of the previous frame. After a session of fast partials that notion is
  // stale, so pixels that should have been given a long white drive got a short
  // one and stayed grey.
  //
  // Visible as a sleep cover that comes up washed out in its light areas, only on
  // light covers (a dark one is mostly large transitions that get driven anyway)
  // and only sometimes, since it depends on what was last on the glass.
  //
  // Half keeps BYPASS_RED. It is the boot paint — Application::start()'s
  // full_refresh() defaults to it — and that sequence has already produced a
  // panel that ran a waveform and then displayed nothing for the rest of time
  // (see the EPD_HALF_REFRESH case in refreshDisplay). It draws from a cleared
  // panel on a cold boot, so it has no previous frame to honour and gains
  // nothing from one. Leave it alone.
  void full_refresh(const uint8_t* pixels, const uint8_t* prev, wintergreen::RefreshMode mode,
                    bool turnOffScreen) override {

    wakeIfNeeded();
    waitWhileBusy();
    const bool full = (mode != wintergreen::RefreshMode::Half);
    // Without a previous frame there is nothing better to start from than the
    // new one, which is the old behaviour.
    const uint8_t* red = (full && prev) ? prev : pixels;
    // Buffer covers all 800 columns; panel offset is baked into draw-function coordinates.
    setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    writeRamBuffer(CMD_WRITE_RAM_BW, pixels, BUFFER_SIZE);
    writeRamBuffer(CMD_WRITE_RAM_RED, red, BUFFER_SIZE);
    refreshDisplay(full ? EPD_FULL_REFRESH : EPD_HALF_REFRESH, turnOffScreen);
  }

  // Fire and return: the controller now holds the pixels in its own RAM, so both
  // host buffers are free and the app can lay out and draw the next frame while
  // the waveform runs. This is the whole page-turn/menu latency win.
  void partial_refresh(const uint8_t* new_pixels) override {
    wakeIfNeeded();
    waitWhileBusy();
    setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    writeRamBuffer(CMD_WRITE_RAM_BW, new_pixels, BUFFER_SIZE);
    refreshDisplay(EPD_FAST_REFRESH, /*turnOffScreen=*/false, /*wait=*/false);
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
  // the fast waveform runs before the analog rails have actually come up, and
  // one page turn is silently lost. The user-visible bug is "go forward, screen
  // does not change; go back and forward again and it does".
  //
  // Two reasons not to try again. The 0x03 shutdown-only sequence is unproven:
  // those two bits are marked "best guess" in refreshDisplay() and are only
  // documented in combination with a refresh. And the fix — a settling delay or
  // a power-up activation before the waveform — would land on the *first page
  // turn after a pause*, which is the common case, paying back the whole saving
  // as added latency exactly where this firmware is tuned not to have any.
  //
  // The rails are still powered down where it is free: refreshDisplay()'s
  // turnOffScreen path, at the end of a full refresh, which is what the sleep
  // image uses.
  // ---------------------------------------------------------------------------

  void deep_sleep() override {
    // partial_refresh() may have left a waveform running; CMD_DEEP_SLEEP mid-update
    // aborts it and leaves a half-drawn panel on screen for the whole sleep.
    waitWhileBusy();
    sendCommand(CMD_DEEP_SLEEP);
    sendData(0x03);
    isScreenOn = false;
    inDeepSleep_ = true;
  }

 private:
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
    waitWhileBusy("post-HWRESET");
    initDisplayController(false);
    inDeepSleep_ = false;
  }

  static uint32_t millis() {
    return (uint32_t)(esp_timer_get_time() / 1000);
  }
  static void delay(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
  }

  void sendCommand(uint8_t command) {
    gpio_set_level(EPD_DC, 0);
    gpio_set_level(EPD_CS, 0);
    spi_transaction_t t{};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = command;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level(EPD_CS, 1);
  }

  void sendData(uint8_t data) {
    gpio_set_level(EPD_DC, 1);
    gpio_set_level(EPD_CS, 0);
    spi_transaction_t t{};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = data;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level(EPD_CS, 1);
  }

  // Bulk pixel/LUT push.
  //
  // Bulk transfers wait on an interrupt rather than polling. A framebuffer plane
  // is 48,000 bytes — ~10 ms at 40 MHz, and full_refresh writes two — and polling
  // spun the CPU for all of it. The transfer is DMA either way; only the waiting
  // changed, so this is a battery optimisation rather than a speed one.
  //
  // Small writes always poll: a 105-byte LUT costs less than the interrupt round
  // trip, and LUT tables live in rodata, which is not DMA-capable — hence the
  // esp_ptr_dma_capable() guard rather than a plain size test.
  void sendData(const uint8_t* data, uint32_t length) {
    static constexpr size_t kChunk = 4092;
    static constexpr size_t kAsyncMin = 1024;
    const bool async = length >= kAsyncMin && esp_ptr_dma_capable(data);
    gpio_set_level(EPD_DC, 1);
    size_t offset = 0;
    while (offset < length) {
      const size_t chunk = (length - offset < kChunk) ? (length - offset) : kChunk;
      gpio_set_level(EPD_CS, 0);
      spi_transaction_t t{};
      t.length = chunk * 8;
      t.tx_buffer = data + offset;
      if (async)
        spi_device_transmit(spi_, &t);
      else
        spi_device_polling_transmit(spi_, &t);
      gpio_set_level(EPD_CS, 1);
      offset += chunk;
    }
  }

  void waitWhileBusy(const char* comment = nullptr) {
    uint32_t start = millis();
    while (gpio_get_level(EPD_BUSY) == 1) {
      vTaskDelay(1);  // 1 tick; pdMS_TO_TICKS(1) rounds to 0 and busy-spins
      if (millis() - start > 10000) {
        break;
      }
    }
    // A non-blocking refreshDisplay() left its waveform running; this is where it
    // gets collected, so report its true duration from the activation timestamp.
    if (pending_refresh_start_ != 0) {
      pending_refresh_start_ = 0;
    }
    if (comment) {
    }
  }

  void resetDisplay() {
    gpio_set_level(EPD_RST, 1);
    delay(20);
    gpio_set_level(EPD_RST, 0);
    delay(2);
    gpio_set_level(EPD_RST, 1);
    delay(20);
  }

  // wait=false fires the waveform and returns immediately, leaving the panel busy.
  // Safe because every public entry point opens with wakeIfNeeded()+waitWhileBusy(),
  // so the next panel operation collects the outstanding refresh before touching RAM.
  // Callers that send further SPI commands right afterwards (deep sleep, screen
  // power-off) must keep wait=true — a command mid-waveform aborts it.
  void refreshDisplay(EpdRefreshMode mode, bool turnOffScreen = false, bool wait = true) {
    sendCommand(CMD_DISPLAY_UPDATE_CTRL1);
    // Only EPD_HALF_REFRESH bypasses RED. Fast has always used it as the previous
    // frame, and Full now does too — that is what makes a full refresh drive from
    // the pixel's actual old level rather than from whatever the controller last
    // tracked. See full_refresh() for what the bypass cost on the sleep cover.
    sendData(mode == EPD_HALF_REFRESH ? CTRL1_BYPASS_RED : CTRL1_NORMAL);

    // best guess at display mode bits:
    // bit | hex | name                    | effect
    // ----+-----+--------------------------+-------------------------------------------
    // 7   | 80  | CLOCK_ON                | Start internal oscillator
    // 6   | 40  | ANALOG_ON               | Enable analog power rails (VGH/VGL drivers)
    // 5   | 20  | TEMP_LOAD               | Load temperature (internal or I2C)
    // 4   | 10  | LUT_LOAD                | Load waveform LUT
    // 3   | 08  | MODE_SELECT             | Mode 1/2
    // 2   | 04  | DISPLAY_START           | Run display
    // 1   | 02  | ANALOG_OFF_PHASE        | Shutdown step 1 (undocumented)
    // 0   | 01  | CLOCK_OFF               | Disable internal oscillator

    // TEMP_LOAD (0x20) makes the controller sample the sensor selected by
    // CMD_TEMP_SENSOR_CONTROL — the internal one, chosen in
    // initDisplayController() — into the temperature register, and the OTP
    // waveform is then picked for *that* reading.
    //
    // Without it the register only ever holds what the host last wrote, which is
    // the fixed 0x5A below: every waveform on this device ran as though the panel
    // were at one unchanging temperature. e-ink waveform timing is genuinely
    // temperature-dependent, and driving a cold panel with a warm panel's
    // waveform under-develops the pixels — which reads as ghosting and washed-out
    // contrast, the exact symptoms the periodic full refresh exists to fix.
    //
    // **Only EPD_FULL_REFRESH takes it**, and the other two are excluded for
    // different reasons. EPD_FAST_REFRESH runs the custom LUT — a fixed table no
    // temperature can select between — and is the page-turn hot path.
    // EPD_HALF_REFRESH is the boot paint, which on a device that deep-sleeps
    // after a minute runs every time it is picked up; a correct, colder, longer
    // waveform there was simply too visible. See the note in that case.
    //
    // What is left is exactly the set of updates that recondition the panel: the
    // periodic DC-balancing flush and the sleep image, neither of which anybody
    // is waiting on.
    //
    // The temperature cannot be read back: CMD_READ_TEMP needs MISO and the
    // panel is wired write-only. The controller acts on it; nothing can print it.
    //
    // If a future panel revision disagrees, setting this to 0x00 restores the
    // previous fixed-temperature behaviour exactly — the 0x5A seed write below
    // is kept for that reason. It is a constant rather than a config option
    // because it is a property of the controller, not a preference.
    static constexpr uint8_t kTempLoad = 0x20;

    uint8_t displayMode = 0x00;
    if (!isScreenOn) {
      isScreenOn = true;
      displayMode |= 0xC0;  // CLOCK_ON + ANALOG_ON (power up for first refresh)
    }

    if (turnOffScreen) {
      isScreenOn = false;
      displayMode |= 0x03;  // Set ANALOG_OFF_PHASE and CLOCK_OFF bits
    }

    switch (mode) {
      case EPD_FULL_REFRESH:
        displayMode |= 0x34 | kTempLoad;
        break;
      case EPD_HALF_REFRESH:
        // This temperature-register write is NOT redundant, however much it
        // looks it. initDisplayController() does select the internal sensor
        // (CMD_TEMP_SENSOR_CONTROL = 0x80), and on that basis this was removed
        // once as vestigial — the result was a panel that ran its waveform (a
        // black flash) and then showed nothing at all. 0xD4 includes LUT_LOAD,
        // and the OTP waveform this loads is selected by the temperature
        // register; without the write it does not come up with a usable LUT.
        // Application::start()'s full_refresh() defaults to Half, so this is the
        // path that draws the very first screen. Leave it alone.
        //
        // **This path deliberately does NOT take TEMP_LOAD**, and the reason is
        // what the user sees rather than what the panel wants.
        //
        // Half is the boot paint — Application::start() ends with
        // full_refresh(), whose default is Half — and because deep-sleep wake is
        // a full boot, that is the waveform that runs *every time the device is
        // picked up*. Letting the controller sample its sensor here made it
        // visibly longer: 0x5A corresponds to a much warmer panel than a room,
        // and e-ink waveforms get longer and invert more times as they get
        // colder. Correct, and far too distracting several times an hour.
        //
        // EPD_FULL_REFRESH keeps TEMP_LOAD, and that is where it earns its keep:
        // the periodic DC-balancing flush and the sleep image are the updates
        // that actually recondition the panel, and they happen when nobody is
        // waiting on them. Any under-driving this path leaves behind is cleared
        // by the next flush.
        sendCommand(CMD_WRITE_TEMP);
        sendData(0x5A);
        displayMode |= 0xD4;
        break;
      case EPD_FAST_REFRESH:
        // LUT_LOAD (bit 4) included: page turns run the panel's OTP fast
        // waveform, which is selected via the temperature register.
        displayMode |= 0x1C;
        break;
    }

    sendCommand(CMD_DISPLAY_UPDATE_CTRL2);
    sendData(displayMode);

    sendCommand(CMD_MASTER_ACTIVATION);

    // Powering the screen off happens at the end of the waveform, so those
    // refreshes can never be left outstanding.
    if (!wait && !turnOffScreen) {
      pending_refresh_start_ = millis();
      if (pending_refresh_start_ == 0)
        pending_refresh_start_ = 1;  // 0 is the "nothing pending" sentinel
      return;
    }
    waitWhileBusy();
  }

  void initDisplayController(bool clearBuffer) {
    sendCommand(CMD_SOFT_RESET);
    waitWhileBusy("CMD_SOFT_RESET");

    sendCommand(CMD_TEMP_SENSOR_CONTROL);
    sendData(0x80);

    sendCommand(CMD_BOOSTER_SOFT_START);
    sendData(0xAE);
    sendData(0xC7);
    sendData(0xC3);
    sendData(0xC0);
    sendData(0x40);

    sendCommand(CMD_DRIVER_OUTPUT_CONTROL);
    sendData((DISPLAY_HEIGHT - 1) % 256);
    sendData((DISPLAY_HEIGHT - 1) / 256);
    sendData(0x02);

    sendCommand(CMD_BORDER_WAVEFORM);
    sendData(0x01);

    if (clearBuffer) {
      clearDisplay();
    }

  }

  void clearDisplay() {
    sendCommand(CMD_AUTO_WRITE_BW_RAM);
    sendData(0x77);
    waitWhileBusy("CMD_AUTO_WRITE_BW_RAM");

    sendCommand(CMD_AUTO_WRITE_RED_RAM);
    sendData(0x77);
    waitWhileBusy("CMD_AUTO_WRITE_RED_RAM");
  }

  void setRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    y = DISPLAY_HEIGHT - y - h;

    sendCommand(CMD_DATA_ENTRY_MODE);
    sendData(0x01);  // X inc, Y dec

    sendCommand(CMD_SET_RAM_X_RANGE);
    sendData(x % 256);
    sendData(x / 256);
    sendData((x + w - 1) % 256);
    sendData((x + w - 1) / 256);

    sendCommand(CMD_SET_RAM_Y_RANGE);
    sendData((y + h - 1) % 256);
    sendData((y + h - 1) / 256);
    sendData(y % 256);
    sendData(y / 256);

    sendCommand(CMD_SET_RAM_X_COUNTER);
    sendData(x % 256);
    sendData(x / 256);

    sendCommand(CMD_SET_RAM_Y_COUNTER);
    sendData((y + h - 1) % 256);
    sendData((y + h - 1) / 256);
  }

  void writeRamBuffer(uint8_t ramBuffer, const uint8_t* data, uint32_t size) {
    sendCommand(ramBuffer);
    sendData(data, size);
  }

};
