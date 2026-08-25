# Wintergreen

Firmware for the Xteink X4 e-reader (ESP32-C3, 16 MB flash, e-ink panel). A fork of
Nous, itself a fork of Microreader. The design goal is a reader with no
configuration surface: everything is fixed at compile time so the device boots
straight into a book.

## Build & flash

```
pio run                            # build (default env: esp32c3)
pio run -t upload                  # build + flash over USB
pio device monitor                 # serial log, 115200
```

There is exactly one env. The `esp32c3-dev` variant — `-DWG_NO_EMBED_FONT`, which
dropped the reader font from the firmware for faster uploads — is **gone**, and
is moot besides: no asset is embedded in the app image any more (see "Assets are
flashed, not embedded").

The board enumerates as `/dev/ttyACM0` on Linux (VID:PID `303A:1001` — the C3's
native USB JTAG/serial peripheral, not an external UART bridge). `upload_port` is
deliberately **not** set in `platformio.ini` so PlatformIO auto-detects; don't
hardcode a port, it breaks cross-platform flashing.

**The desktop build is gone.** `platforms/desktop/` no longer exists; the only
remaining host build is `tools/epub2wgb`, and it lists its sources
**explicitly** in `tools/epub2wgb/CMakeLists.txt`, as does
`platforms/esp32/CMakeLists.txt` for the firmware. Both must be edited when a
core `.cpp` is added or removed, or CMake fails at generate time with an error
naming `idf_component_register` rather than anything you touched.
`wintergreen/Loop.cpp` is the last remnant of the desktop entry point; its
`run_loop()` is gone and `main.cpp` drives `run_loop_iteration()` itself.

`ESP_PLATFORM` is **not** a QEMU leftover — it is ESP-IDF's own macro, defined
for any ESP32 target build, and it is the seam between firmware and the host
converter (`tools/epub2wgb`). It stays. The QEMU flag was `QEMU_BUILD`, below.

The `esp32c3-qemu` env is gone, along with every `QEMU_BUILD` conditional. Two
things that removal broke, both worth knowing about:

- Deleting `#ifndef QEMU_BUILD` … `#endif` around the ADC setup in `input.h` took
  **the body with it**, leaving a constructor that configures the power GPIO and
  nothing else. No `adc_oneshot_new_unit`, no `esp_timer_start_periodic`, so
  `sample()` never ran, `debounced_` stayed 0 and **no button worked at all** —
  including power. It builds and boots perfectly.
- The `[env:esp32c3-qemu]` header was removed but its keys were not, so
  `build_flags = -Ilib -DQEMU_BUILD` and a `sdkconfig.qemu` reference were left
  orphaned inside the (since removed) dev env. That eventually surfaced as an
  `InvalidProjectConfError` about a duplicate `build_flags`.

Flash is **DIO at 80 MHz** (`sdkconfig.defaults`). QIO was tried and **bricks the
boot** on the X4 — the header byte looks the same either way (IDF deliberately
writes `dio` for QIO images), but the bootloader's quad-mode self-upgrade during
init never completes on this board. DIO@80 MHz gives 20 MB/s, identical to
QIO@40 MHz. `board_build.flash_mode` in `platformio.ini` is inert under the espidf
framework; only the sdkconfig option matters. Don't re-enable QIO without a board
in hand to test on.

The SD clock (`platforms/esp32/sdcard.h`) comes off the 80 MHz APB through an
integer divider, so the only rates available are 80 / 40 / 26.7 / 20 MHz.
**40 MHz does not work** — the card fails to read. The e-ink panel does run the
same SPI2 bus at 40 MHz, but that path is write-only (CLK+MOSI); SD reads need the
MISO round trip, and a 12.5 ns half-period is under the 14 ns output delay the SD
spec already grants the card. Host/board timing, not card quality — a better card
does not fix it, and the C3 has no SDMMC peripheral to fall back to.

**26.67 MHz does not work either** (tested on hardware — the book list comes up
empty). 20 MHz is the ceiling; there is no rung between 20 and 26.7. Don't spend
another flash cycle on the SD clock, and don't reach for the card's `input_delay`
either — IDF doesn't expose it through `sdspi_device_config_t`, so it would mean
patching the framework in-place, which is exactly what `patch_ffconf.py` was
deleted for.

### Idle power

The energy budget is lopsided and worth internalising before optimizing anything:
deep sleep is ~10 µA, CPU awake is ~15 mA at 80 MHz and ~22 mA at 160 MHz, and an
e-ink refresh is a sub-second burst. **One second awake costs more than half an
hour asleep.** Battery life is therefore almost entirely "what fraction of the
time is the CPU awake, and at what clock" — flash mode, SD clock and panel
tuning move the third decimal place.

**Automatic light sleep is not usable on this device — it was tried and removed.**
Note this is specifically about **tickless idle**, not the whole PM subsystem;
`CONFIG_PM_ENABLE` on its own is still available (see DFS below). Don't
reintroduce `CONFIG_FREERTOS_USE_TICKLESS_IDLE` without reading this:

- Tickless idle force-enables `PM_SLP_DISABLE_GPIO` via `ESP_SLEEP_GPIO_RESET_WORKAROUND`
  (a C3 errata fix — an ESD pulse on a sleeping pin can reset the chip) using a
  Kconfig `select`, which **cannot** be overridden from `sdkconfig.defaults`.
- The buttons are ADC resistor ladders on GPIO1/GPIO2 (`input.h`).
  `gpio_sleep_sel_dis()` does **not** rescue them: those pads sit in analog mode,
  so what light sleep destroys is SAR ADC state, not pin configuration.
- Symptom if it comes back: buttons work for about a second after boot, then stop
  until USB is attached. USB suppresses light sleep, so the device looks healthy
  on the bench and is unusable on battery.
- It also only engages when button sampling is >= 30 ms, flatly at odds with
  responsive input.

Battery life comes from reaching **deep** sleep (microamps) rather than light
sleep (milliamps). The lever is `kAutoSleepMinutes` in `WintergreenConfig.h`,
traded against deep-sleep wake being a full boot.

Because wake **is** a full boot, everything on the boot path runs on every wake,
and cheap wake is what makes a short `kAutoSleepMinutes` tolerable. Hence the
"Boot speed" block in `sdkconfig.defaults`: `BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP`
(skips re-hashing the whole image; cold boot still validates),
`BOOTLOADER_LOG_LEVEL_NONE` (~78 ms of UART per boot), and `RTC_CLK_CAL_CYCLES=256`
(down from 1024; calibration precision only matters for *timer* wake, and this
device wakes on GPIO). There is no app logging at all — see "No logging" — so
`LOG_DEFAULT_LEVEL_NONE` is belt and braces rather than the mechanism.

**Dynamic frequency scaling is now live** — see "Dynamic frequency scaling
(live)" below for the configuration and the three invariants it depends on.
`CONFIG_PM_ENABLE` with `light_sleep_enable = false` does not trip the errata
`select` above, so it does not break the buttons.

Side-rocker paging is **orientation-dependent**; the front row is not. The rocker
is one physical control whose ends swap places when the device is turned, so
`ReaderScreen::update()` picks the side-rocker mapping off `landscape_hold`:

|            | Up (rocker top) | Down (rocker bottom) |
|------------|-----------------|----------------------|
| Portrait   | next            | previous             |
| Landscape  | previous        | next                 |

The front buttons are fixed to the panel and never change: Button2 is next,
Button3 is previous, in both orientations.

Portrait is `Deg90` **or** `Deg270` and landscape is `Deg0` **or** `Deg180` — the
reversed orientations are the same physical hold, so a check written as
`rotation() == Deg0` leaves flipped-landscape behaving like portrait. That bug
was present in the reader's layout padding and is fixed.

**The reversed orientations are no longer reachable.** `rotate_reader` is 0
(portrait, `Deg90`) or 1 (landscape, `Deg0`); `rotation_from_setting` and
`rotation_label` collapsed from four cases to two, "Reader Display" toggles
rather than cycles, and both the setter and the settings loader clamp anything
else to 0. The `Rotation` enum keeps all four values — the panel driver needs
them — so the paragraph above still applies to any new orientation check.

Input timing is chosen for responsiveness: 5 ms button sampling (`input.h`) under
a 25 ms UI frame (`main.cpp`), so a press lands on the next frame. Screens repaint
only on state changes, so frame rate does not drive panel traffic.

**Never measure a hold in frames.** Frame duration is not a constant — it swings
with how much a screen rendered and whether the panel was mid-refresh — so a frame
count means different wall time in different builds. Use `runtime.frame_time_ms()`,
which returns the *measured* last frame, and compare against milliseconds. Both
frame-counted holds in the tree were wrong the moment `partial_refresh()` stopped
blocking and frames went from hundreds of ms to ~25:

- the reader's and list's page/row repeat fired once **per frame** with no delay, so
  a normal tap registered as two presses. They now use `HoldRepeat` (`Input.h`),
  driven by `config::kHoldDelayMs` / `kHoldRepeatMs` / `kHoldAccelStep`.

`HoldRepeat::tick()` may return more than one repeat when a frame ran long, so a
slow render cannot throttle the repeat rate — but it is capped at `kMaxPerTick`,
because cashing in a multi-second stall as a burst reads as a freeze then a jump.

The menu's acceleration step is read into a local before its inner loop. Written as
`for (i = 0; i < ++hold_reps_up_; ++i)` the increment sits in the loop *condition*
and is re-evaluated every pass, so the bound outruns `i` and the loop never
terminates — that shipped once and hung the device on any button hold. The step is
also capped at the list length; beyond that it is spinning around a list it has
already crossed.
- the hidden-books long-press was `15` frames, commented "≈3s at typical e-ink
  frame rate" — i.e. calibrated to refresh-blocked frames. It became ~0.4 s. It is
  milliseconds now, and (on `HomeScreen`, where it moved) it is the tree's only
  remaining hold gesture outside `Application`'s power button.

`Application`'s power-button hold was always ms-based (`power_hold_ms_ += dt_ms`)
and was unaffected — that is the pattern to copy.

### Refresh latency

Almost all of a page turn or a menu keystroke is the e-ink waveform, not the CPU.
Layout and draw are tens of milliseconds; the waveform is hundreds. Three things
exploit that, and the first two have **invariants that are easy to break**.

`partial_refresh()` passes `false`, so it fires `CMD_MASTER_ACTIVATION` and returns
`partial_refresh()` passes `false`, so it fires `CMD_MASTER_ACTIVATION` and returns
while the panel is still updating. The app then lays out and draws the *next* frame
during the waveform instead of after it. This is safe only because **every**
`EInkDisplay` entry point opens with `wakeIfNeeded(); waitWhileBusy();` — that
prologue is what collects the outstanding refresh. Do not add a panel operation
without it. Two consequences that are load-bearing:

- Anything that sends SPI right after firing must keep `wait = true`, or the command
  lands mid-waveform and corrupts the update. That means `full_refresh`, the one-pass
  grayscale path, and anything with `turnOffScreen` (the screen powers down at the
  *end* of the waveform). `setCustomLUT_()` and `deep_sleep()` therefore start with
  their own `waitWhileBusy()` — `deep_sleep()` mid-update leaves a half-drawn panel
  on screen for the entire sleep, which is the most visible way to get this wrong.
- **The SD card shares SPI2 with the panel** and concurrent traffic corrupts an
  update in flight (this is why `main.cpp` already defers index ops on
  `epd.is_busy()`). Any new SD access must call `DrawBuffer::wait_panel_idle()`
  first. The existing ones are `draw_image_`, `ReaderScreen::stop()`'s
  `.pos` write, `do_sleep_()`, and the settings save on reader pop. Text-only pages
  never touch the card and keep the full overlap.

**Region refresh cannot be used to update "just the changed rows", and this was
tried and reverted.** `setRamArea()` bounds where a RAM *write* lands; it does not
bound the refresh. `refreshDisplay()` drives the **whole panel** from BW RAM, and a
partial update merely *looks* localized because unchanged pixels have BW RAM == RED
RAM and receive a no-op waveform. So writing only a sub-rectangle leaves the rest of
BW RAM holding the last *fully written* frame, and the panel faithfully redraws that
stale frame everywhere outside the window.

The symptom, when `ListMenuScreen` used a diff-and-region-refresh helper for cursor
movement: previously-selected rows stayed lit, the reader page showed through behind
the menu, and the screen alternated between correct and stale as the periodic
full-refresh corrected it. Keeping BW RAM in sync would mean writing all 48 KB
anyway, and the waveform is panel-wide regardless — there is no saving to be had.

`partial_refresh_region()` remains correct for its **one** caller, `show_loading()`,
which satisfies the precondition: during conversion the rest of the screen is static
*and* BW RAM already matches it. That narrow validity is why it looked like a general
primitive and is not one.

**`ReaderScreen::page_cache_`** holds one laid-out page. `prev_page_()` has to run a
full `layout_backward()` just to learn where the previous page starts; it used to
throw the result away and `render_page_()` laid the identical page out again, so
every backward turn cost two layouts. The key is chapter + position + `PageOptions`
+ font size index, checked **after** `resolve_stable_position()` (which can move
`page_pos_`). `set_font`/`set_options` wipe `TextLayout`'s paragraph cache wholesale,
so a stale entry doesn't merely draw the wrong page — it points at freed line
vectors. `LayoutWord::text` points into `WgbChapterSource`'s 32-slot paragraph
window rather than into `PageContent`, which is why the cached page is **moved**
into `page_` (never copied — only one may be live) and why `load_chapter_()` and
`stop()` drop it explicitly: rebuilding the source frees every slot it points into,
and no key comparison can detect that.

Auto-sleep never fires while USB is connected — `main.cpp` calls `keep_awake()`
every iteration when `usb_serial_jtag_is_connected()`, which zeroes
`inactivity_ms_`. Deliberate, but it means the timeout can only be tested on
battery. `Esp32Runtime::frame_time_ms()` returns the **measured** duration of the
last frame, not the nominal target; `Application` accumulates it into the
auto-sleep countdown, so returning the constant made that clock drift whenever a
frame overran.

**`Application::update` reads its `buttons` parameter directly — there is no
`buttons_` member any more, and that is deliberate.** The member existed only to
be copied from the parameter on the first line of `update()` and read a few lines
later. When an edit removed that one copy, `buttons_` stayed default-constructed
forever: every button on the device went dead, power included, so the reader
could not be opened, paged, or even put to sleep — and the firmware built and
booted perfectly, showing the home screen. Deleting the member makes that state
unrepresentable. Don't reintroduce it.

The power button (GPIO3, the only non-ADC key) is handled entirely in
`Application::update`, not by any screen. Held for `kPowerHoldSleepMs` it sleeps;
released sooner it is forwarded to the top screen as a synthetic `Button1`
(Confirm) press, so "select" and "open the quick menu" come for free. Three
things that look incidental are load-bearing:

- It arms only on a **rising edge**. The press that wakes the device is cleared
  by `input.clear_button(Button::Power)` in `main.cpp`, so a wake hold can never
  read as a hold-to-sleep or a select.
- The tap necessarily resolves on **release** — that is the only way to tell it
  apart from a hold.
- `main.cpp` waits for GPIO3 to go HIGH before `esp_deep_sleep_start()`. Wake is
  level-triggered on LOW, and hold-to-sleep leaves the button down, so without
  that wait the device wakes instantly and (being a non-`ESP_RST_POWERON` reset)
  skips the hold gate in `verify_wakeup_press()` — an on/off loop.

Beware `pdMS_TO_TICKS(1)`: at `FREERTOS_HZ=100` it evaluates to **0 ticks**, and
`vTaskDelay(0)` only yields rather than blocking, so any wait loop using it spins
the CPU flat out. `epd.h`'s `waitWhileBusy` had this bug and now uses a literal
`vTaskDelay(1)`. Check for it before adding any new poll loop.

If a build ever seems to ignore an `sdkconfig.defaults` change, delete the
generated `sdkconfig.esp32c3` — IDF keeps existing values and does not re-apply
defaults over them. That file is generated per env and is **not** committed;
`sdkconfig.defaults` is the checked-in source of truth.

Deleting those is sometimes **not enough**: `memory.ld` and `sections.ld` under
`.pio/build/<env>/` are generated from sdkconfig too and are not always
regenerated. Enabling `BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` failed to link
with `region 'rtc_reserved_seg' overflowed by 16 bytes` purely because
`memory.ld` still carried the old `len = (0 + (24))` while the config correctly
said `CONFIG_BOOTLOADER_RESERVE_RTC_SIZE 0x10`. If a config change produces a
linker-script-shaped error, `rm -rf .pio/build/<env>` before believing it.

`CONFIG_FATFS_CODEPAGE_437` matters: the dynamic codepage links every OEM table,
including the four CJK ones, for 478 KB of flash. Long filenames go through the
UTF-8 LFN path regardless; the codepage only affects 8.3 short names.

UI font arrays in `display/ui_font_*.h` are `inline constexpr`, not
`static constexpr` — at namespace scope in a header, `static` gives each including
TU its own copy (the small font was linked 4×, wasting 96 KB).

Toolchain note: this is a Nix machine with no system `python3` on PATH. Build
scripts run under PlatformIO's bundled interpreter. For one-off scripting use
the interpreter from the `pio` wrapper's shebang chain (currently
`/nix/store/…-python3-3.14.6/bin/python3.14`); `perl` is not installed either.

### Dynamic frequency scaling (live)

`CONFIG_PM_ENABLE=y` with `max_freq_mhz = 160`, `min_freq_mhz = 80`,
`light_sleep_enable = false`, configured in `Esp32Runtime::init_pm_()`. The core
idles at 80 MHz — ~15 mA against ~22 mA — which is where nearly a whole reading
session is spent, since the UI loop wakes every 25 ms, finds nothing to do and
returns to WFI.

Three things hold this together; breaking any one of them undoes it or worse:

- **`light_sleep_enable` must stay false.** This is the tickless-idle errata
  described under "Idle power", not a `PM_ENABLE` problem. `PM_ENABLE` on its own
  does *not* select `PM_SLP_DISABLE_GPIO`, so the ADC-ladder buttons are safe.
  Never add `CONFIG_FREERTOS_USE_TICKLESS_IDLE`.
- **`min_freq_mhz` is 80, not 40.** APB follows the CPU clock on the C3, and the
  SD-over-SPI timing has no margin left at 20 MHz as it is.
- **Layout and drawing take no PM lock of their own.** They are pure CPU with no
  driver involved, so without an explicit lock a page turn would run at the idle
  clock — up to 2x slower, which is a net regression. `IRuntime::set_performance_hold`
  wraps an `ESP_PM_CPU_FREQ_MAX` lock, and `Application::update` raises it on any
  button activity or pending screen transition and drops it after `kPerfHoldMs`
  (750 ms) of quiet. That window comfortably outlasts a page turn, and the
  acquire/release are skipped when the state is unchanged.

### Page-turn latency

Everything CPU-side now happens off the critical path, leaving the ~300 ms e-ink
waveform as the floor:

- **The next page is laid out *and drawn* during the waveform**, so a forward turn
  is a memcpy plus an SPI write. **Returning from the quick menu is a memcpy too.**
  Both use the same buffer — see "The spare framebuffer" — including the two cases
  the pre-draw deliberately skips.
- **Bulk SPI is interrupt-driven, not polled.** `EInkDisplay::sendData` uses
  `spi_device_transmit` for chunks >= 1 KB from DMA-capable memory and
  `spi_device_polling_transmit` for everything else. A framebuffer is 48,000 bytes
  — ~10 ms per plane at 40 MHz, and `full_refresh` writes two — and polling spun
  the CPU for all of it. The transfer was already DMA; only the waiting changed.
  Small writes stay polled deliberately: a 105-byte LUT costs less than the
  interrupt round trip, **and the LUT tables live in rodata, which is not
  DMA-capable** — hence the `esp_ptr_dma_capable()` guard rather than a size test
  alone.
- **Chapter load reads its descriptor table in one `fread`.**
  `WgbChapterSource`'s constructor used to read the N x 8-byte entries one at a
  time, a FATFS + SPI round trip per paragraph, and a chapter can have hundreds.
  It now reads the table into a staging vector and parses from memory. This is on
  the path for every book open and every chapter jump.
- **Images are never decoded on the device** — see "Content pipeline". An image
  page turns at the speed of a text page.
- **The book index stays resident** for the whole session. `MainMenu::stop()` no
  longer calls `clear_entries()` (the method is gone), and both `MainMenu` and
  `HomeScreen` load from disk only when `entries()` is empty — i.e. once, on a
  cold boot. Every mutation path updates the in-memory copy and saves, so a
  non-empty index is authoritative. It is ~30 KB against ~165 KB of free heap,
  and dropping it meant re-reading and re-parsing the index file off the SD
  card on entry to the home screen, on entry to the list, and again when the
  reader wrote its progress.

### The spare framebuffer

`DrawBuffer` carries a third 48 KB buffer serving two jobs that never overlap in
time, tracked by `spare_use_`:

- **Snapshot** — a copy of the displayed frame. `ReaderScreen::pause()` takes it
  when the quick menu opens; `resume()` puts it back, so dismissing the menu is a
  memcpy instead of a full layout and glyph draw.
- **Offscreen** — where `prerender_next_page_()` *draws* the next page while the
  panel is still running the current page's waveform. `take_predrawn_()` then
  commits it, so a forward turn is a memcpy plus an SPI write: no layout, no
  blitting.

They share storage because a reader is either in the quick menu or turning pages,
never both. **Claiming it for one purpose silently invalidates the other**, and
that is safe only because both consumers re-check: `resume()` tests
`has_snapshot()`, `take_predrawn_()` tests `has_offscreen()`, and either falling
false just means rendering normally.

`begin_offscreen()` redirects `draw_()` — the target every drawing helper resolves
through — at the spare; `end_offscreen()` publishes it and `commit_offscreen()`
moves it into the inactive buffer for the caller's refresh. The commit paths
(`refresh`, `full_refresh`,
`write_ram_bw`, `restore_snapshot`, the MGR2 sleep blit) use `inactive_()`
directly and are unaffected.

Two things `prerender_next_page_()` must get right:

- **It restores `page_` and `page_pos_` afterwards.** `render_page_()` works on
  those, so the next page is made current for the duration of the draw and then
  put back. `page_` keeps only positional metadata either way — its
  `LayoutWord::text` pointers into the 32-slot paragraph window die the moment
  laying out the next page slides it.
- **It skips the draw for a page containing images.** `draw_image_()` reads the
  book off the SD card, which shares SPI2 with the panel, so it drains the
  waveform first — and doing that here would block the UI loop for the couple of
  hundred milliseconds the page just shown is still painting. Image pages get
  their layout cached and nothing more.

`take_predrawn_()` keys on chapter, the **pre-resolve** request position (what
`next_page_()` produces, so no `resolve_stable_position()` call is needed to
compare), font size and the full `PageOptions`. Any mismatch renders normally.

### Low-battery cutoff

**Live, at 3200 mV.** The threshold is `wintergreen::kLowBatteryCutoffMv` in
`Application.h` — deliberately **not** in `WintergreenConfig.h`, because it is a
property of the cell and the board's brownout margin rather than a preference,
and a wrong value makes the device look bricked. The header carries the three
numbers that bound the choice (protection IC ~2500, permanent-damage onset ~3000,
discharge-curve zero 3300).

It shipped once with three faults that together made the
device look bricked, so the guards below are not optional decoration — read them
before touching this. Everything here rests on
`Esp32Runtime::battery_millivolts()` being right; if the on-screen percentage ever
stops tracking reality, suspect this first.

Below the cutoff the device saves state, shows the wordmark and deep-sleeps, and
`verify_battery()` in `main.cpp` refuses to boot again until it has been charged.
Deep-discharging a Li-ion cell below ~3.0 V costs it permanent capacity, and deep
sleep keeps drawing ~10 µA afterwards. The cell almost certainly has a hardware
protection IC, but that cuts off around 2.5 V, well past where the damage starts.

Five guards, all load-bearing. **The first three exist because their absence is
what bricked the device**, and the failure is not subtle: the reader boots, draws
one frame, and goes straight back to the sleep screen, over and over, with the
USB Serial/JTAG peripheral powered down between attempts so esptool cannot get a
word in.

1. **`usb_attached()` must be a real check.** It read GPIO20 (U0RXD) and called a
   high level "USB connected" — true only behind an external UART bridge. The X4
   uses the C3's **native** USB, so GPIO20 never goes high and the answer was
   always "no host": the boot gate fired *while plugged in*, which is exactly the
   case that was supposed to be the escape hatch. It now asks
   `usb_serial_jtag_is_connected()`, which is valid before the driver is
   installed.
2. **The low reading must persist for wall-clock time**, `kLowBatteryHoldMs`
   (4 s), not a frame count. The first version wanted five frames — about 125 ms,
   *shorter than one e-ink refresh*, which is the biggest current spike the device
   has and sags the rail while it runs. See "Never measure a hold in frames".
3. **It never samples while the panel is busy** (`display().is_busy()`), for the
   same reason.
4. **A zero reading means "cannot measure"** — no battery, or an uncalibrated ADC —
   and so does anything below `kImplausibleMv` (2500). `battery_millivolts()`
   returns 0 rather than a raw ADC count when calibration is unavailable,
   precisely so a wrong voltage cannot trip the cutoff.
5. **The boot check runs before `epd.begin()`**, so a refused boot never lights the
   panel or mounts the card.

Note the default threshold that shipped, 3300 mV, is *0%* on the discharge
polynomial in `battery_percentage()` — the cutoff sat exactly at empty with no
margin, so an ordinary sag crossed it. Pick a value below the curve's zero, not at
it.

**Recovering a device that is cutting off:** leave it on USB. The charger IC runs
independently of the SoC, so the cell charges whether or not the firmware is
awake. To flash one that is still sleeping instantly, start the upload and then
hold the power button: deep-sleep wake is level-triggered on GPIO3 LOW, and a
wake is a non-`ESP_RST_POWERON` reset, so `verify_wakeup_press()` returns
immediately and the device boot-loops while the button is held — enumerating USB
on each pass and giving esptool's connect retry a window.

### Panel rails — the idle power-down is gone, don't re-add it

`EInkDisplay::power_down_rails()`, `Application::kPanelIdleOffMs` (5 s),
`panel_rails_off_` and `DrawBuffer`'s wrapper are all **deleted**. The idea was
sound and the implementation is not recoverable; read this before trying again.

It switched the analog charge pumps off after 5 s of no input, on the reasoning
that e-ink is bistable so the page stays on the glass, that this is exactly the
state a session spends most of its time in, and that the next refresh would see
`isScreenOn == false` and OR in `CLOCK_ON | ANALOG_ON` by itself. It sent the
same `ANALOG_OFF_PHASE | CLOCK_OFF` bits `refreshDisplay()` uses for
`turnOffScreen` but with no `DISPLAY_START`, so no waveform ran — and unlike
`deep_sleep()` it kept controller RAM, which the differential fast-refresh
waveform needs.

**On hardware it silently swallowed one page turn.** With the rails down, the
next `partial_refresh()` ran its waveform before the analog rails were actually
up: the old page stayed on the glass, and only the turn *after* that drew. The
user-visible bug is "wait five seconds, press forward, nothing happens; go back
and forward again and it works".

Two reasons not to retry. Those two CTRL2 bits are marked "best guess" in
`epd.h` and are only *documented* in combination with a refresh, so the
shutdown-only sequence was never on firm ground. And the fix — a settling delay,
or a separate power-up activation before the waveform — would land on **the
first page turn after a pause**, which is the common case, paying the whole
saving back as latency in the one place this firmware is tuned not to have any.

The rails are still powered down where it costs nothing: `refreshDisplay()`'s
`turnOffScreen` path at the end of a full refresh, which is what the sleep image
uses.

Auto-sleep is now the only idle timer left — `kGhostFlushIdleMs` went with the
periodic flush and `kPanelIdleOffMs` with this.

Temperature compensation is a separate mechanism and is **not** implied by
`initDisplayController()` selecting the internal sensor — see "Temperature
compensation" below, and do not touch the `CMD_WRITE_TEMP 0x5A` in the
half-refresh path before reading it.

### The half-refresh temperature write — do not remove it

`refreshDisplay()`'s `EPD_HALF_REFRESH` case sends `CMD_WRITE_TEMP` (0x1A) with
`0x5A` before setting the mode byte. It looks redundant, because
`initDisplayController()` selects the controller's *internal* temperature sensor
(`CMD_TEMP_SENSOR_CONTROL` = 0x80) and a manual write to the temperature register
should then have no effect.

It was removed on exactly that reasoning, and the result was a device that ran a
waveform — one black flash — and then displayed nothing at all, ever. `0xD4`
includes **LUT_LOAD**, and the OTP waveform it loads is selected via the
temperature register; without the write the panel comes up with no usable LUT.

What made it costly to find: **`Application::start()` ends with `buf.full_refresh()`,
whose default is `RefreshMode::Half`**. So the one path that was broken is the one
that paints the very first screen, and every screen after it looked broken too.

Two lessons that generalise beyond this line:

- A vendor init sequence is not a place to remove things that "should" be no-ops.
  The datasheet does not describe what the OTP LUT loader actually keys off.
- "The code reached this point" is not "the panel showed something". A boot trace
  reporting that `app.start()` returned says nothing about whether anything is on
  the glass, and reading it as success sent the search after dynamic frequency
  scaling and the battery instead.
### Panel health

A run of fast partial waveforms is **not DC-balanced**: each leaves a little
residual charge in the microcapsules, which shows up as ghosting and, over a long
enough run, as permanent contrast loss. Every e-reader periodically clears this
with a full refresh — the second-long flash where the panel inverts.

**Wintergreen has no periodic flush, and this was arrived at the hard way.**
Three versions shipped:

1. **Inline.** `DrawBuffer::refresh()` promoted every 24th call to a full
   refresh. The flash landed on whichever page turn happened to be the 24th.
2. **Deferred to a short idle** (2500 ms), on the reasoning that a page turn is
   over by then. It is not: 2.5 s after the last press you are *reading the
   page*, and a flash there is worse than one during the turn, because it
   interrupts rather than delays.
3. **Removed.** `partials_since_full_`, `flush_pending()`, `flush_ghosting()`,
   `Application::kGhostFlushIdleMs` and `config::kFullRefreshEveryNUpdates` are
   all gone.

Removing it is safe **because deep-sleep wake is a full boot**. The counter never
survived sleep anyway, and every session is already bracketed by two full-panel
waveforms:

| When | What runs |
|---|---|
| going to sleep | the sleep image — `full_refresh(RefreshMode::Full)` for a book cover, or `grayscale_refresh_1pass` for the wordmark |
| waking | `Application::start()`'s `full_refresh()` (Half) |

With `kAutoSleepMinutes = 1` that is a full-panel waveform every time the device
is put down and picked up — more often than a Kindle, which flashes every few
pages *while you read*. The in-session flush was insuring against a risk the
sleep cycle had already covered, and it is the only mechanism here the user can
actually see.

**What is genuinely uncovered:** a session long enough to never idle for a whole
minute. Page turns every 45 s keep `inactivity_ms_` below the auto-sleep timeout
indefinitely, so a 30-minute stretch is ~40 partial updates with no full refresh
in the middle. That is within normal e-reader practice (Kobo exposes 1–100 pages
*and* "never"), and the page-turn waveform is the vendor's OTP fast LUT rather
than a hand-rolled table — `custom_lut_active_` is false for page turns, so
`EPD_FAST_REFRESH` sets LUT_LOAD and uses the waveform the panel shipped with.
Expect faint ghosting to become visible late in a long session; expect it to be
gone after the next sleep.

The canary is visible ghosting that *survives* a sleep/wake cycle. Nothing in the
current design would produce that, and if it appears, the fix is to reintroduce a
counter in `DrawBuffer::refresh()` and call `full_refresh(RefreshMode::Full)`
from wherever the interruption is acceptable — accepting that there is no
threshold which both protects the panel mid-session and stays out of the way.

Temperature compensation is a separate mechanism and is **not** implied by
`initDisplayController()` selecting the internal sensor — see "Temperature
compensation" below, and do not touch the `CMD_WRITE_TEMP 0x5A` in the
half-refresh path before reading it.

**Temperature compensation is live**, and the mechanism is worth understanding
because it also explains the `0x5A` disaster above.

`initDisplayController()` selects the controller's *internal* temperature sensor
(`CMD_TEMP_SENSOR_CONTROL` = 0x80). That only says which sensor to use; it does
not say when to read it. The reading happens when **TEMP_LOAD (CTRL2 bit 5,
0x20)** is set in the display update sequence, and that bit was **never set
anywhere in this driver**. So the controller never sampled its own sensor: the
temperature register only ever held what the host had written into it, which was
the fixed `0x5A` in the half-refresh path. Every waveform the device ever ran was
selected for one unchanging notional temperature.

That is also the real explanation for the failure recorded above. Removing the
`0x5A` write did not break "a magic vendor incantation" — with TEMP_LOAD never
set, that write was the *only* thing putting a valid value in the register, so
removing it left the OTP LUT loader keying off whatever a soft reset leaves, and
it loaded no usable waveform.

`refreshDisplay()` ORs TEMP_LOAD into **one** path:

- **`EPD_FULL_REFRESH` (0x34 → 0x54)**. The periodic DC-balancing flush and the
  sleep image — the updates that actually recondition the panel, and the ones
  nobody is waiting on.
- **`EPD_FAST_REFRESH` is excluded** because it runs the custom LUT, a fixed
  table no temperature can select between, and it is the page-turn hot path.
- **`EPD_HALF_REFRESH` is excluded for a UX reason, not a technical one**, and it
  was tried the other way first. Half is the boot paint (`Application::start()`
  ends with `full_refresh()`, default Half), and since deep-sleep wake is a full
  boot with `kAutoSleepMinutes = 1`, that waveform runs *every time the device is
  picked up*. With TEMP_LOAD it got visibly longer and flashed noticeably more:
  `0x5A` corresponds to a much warmer panel than a room, and e-ink waveforms
  lengthen and invert more as they get colder. The panel was right; several times
  an hour it was intolerable to look at. Any under-driving it leaves is cleared by
  the next full-refresh flush.

If a boot paint ever *needs* to be temperature-correct, the fix is to OR
`kTempLoad` into the Half case — not to change what Full does.

Two safety properties, both deliberate:

- **The `0x5A` write stays.** It is the seed the controller falls back to, so a
  sensor that does not answer leaves a usable value in the register rather than
  whatever the reset left, and turning the feature off restores the previous
  behaviour exactly.
- **`kTempLoad` in `refreshDisplay()` is the one line to flip.** Setting it to
  `0x00` restores the previous fixed-temperature behaviour exactly, which matters
  because this is the sequence that once produced a panel which flashed and then
  showed nothing. It is a local constant rather than a config option: whether the
  controller implements CTRL2 bit 5 is a property of the hardware, not a
  preference.

Why it matters: driving a cold panel with a warm panel's waveform under-develops
the pixels, which reads as ghosting and washed-out contrast — the same symptoms
the periodic full refresh exists to correct. A reader used outdoors in winter is
the case this fixes.

**The one thing still not possible: reading the temperature back.** `CMD_READ_TEMP`
would need MISO from the panel, and the panel is wired write-only (CLK+MOSI) —
the SD card is the only device on SPI2 with a read path. So the firmware cannot
log or display the panel temperature, only tell the controller to act on it.

### SD card wear

Writes are already rare — the reading position moved out of the page-turn path

long ago — and every remaining one is now deduplicated:
| File | Written | Guard |
|---|---|---|
| `.wintergreen` (settings) | reader/quickmenu pop, sleep | byte-compare against `settings_written_` |
| `.wintergreen-index` | reader close, library scan, serial mutations | `BookIndex::dirty_` |
| `<book dir>/book.pos` | reader close | `ReaderScreen::pos_written_` |

**A whole reading session can now write nothing at all.** Open a book you were
already on, read, and sleep: `mark_opened` does not move the order, `set_progress`
does not move the percentage, so the index stays clean; the position is written
only if it moved; and settings only if a byte changed.

Four changes made that concrete:

- **`BookIndex` has a dirty flag.** `save()` returns immediately when clean, and
  `mark_opened` / `set_progress` set it only when the stored value actually
  moves.
- **One index write per session, not two.** `record_book_opened()` used to save
  the index *and* settings; the index goes out once, from
  `record_book_progress()` when the reader closes.
- **Nothing is written on book open.** `record_book_opened()` used to save
  settings there so a reboot resumed the book. But every ordinary way of putting
  the device down routes through `do_sleep_()`, which saves settings anyway, so
  that write only mattered after a power loss — and it cost an SD write on every
  single book open to cover it. `.pos` is written at close for the same reason,
  so an unexpected power loss already resumes at the position you started from;
  now it resumes at the previous *book* too.
- **Settings are saved *after* the pop, not before.** `save_settings_()` writes
  `book_path` only while the reader is on the stack, so saving first persisted
  "resume this book" for a book the user had just closed, and then had to be
  rewritten without it at the next sleep. Two writes and a wrong intermediate
  state, now one write with the right contents.

The index is also **kept resident** (see `MainMenu::stop`), so none of the three
screens that need it re-read and re-parse the index off the card.

**Don't add a write to the page-turn path.** That is the one thing that would
change the wear profile by an order of magnitude, and it is why `.pos` is written
from `ReaderScreen::stop()` alone.
## Configuration

`WintergreenConfig.h` **in the project root** is the only place with tunable
values. The project root is on the include path for both platform builds.

| Constant | Default | Notes |
|---|---|---|
| `WG_BLUETOOTH_PAGE_TURNER` | *(commented out)* | A `#define`, not a constant — see "Bluetooth page-turner" for why it has to be. The clicker's MAC, `"AA:BB:CC:DD:EE:FF"`. Undefined keeps the whole NimBLE stack out of the build. |
| `kWifiName` / `kWifiPassword` / `syncServer` | `""` | **Nothing reads these yet** — no Wi-Fi subsystem exists. Placeholders for the planned NAS sync; the one consumer-to-be is `MainMenu::run_sync_()`. |
| `kSunlightFadingFix` | `false` | Powers the panel off at the end of every *full* refresh, which restores contrast lost to sunlight. There are no periodic full refreshes to trigger it (see "Panel health"), so in practice this affects the sleep image and the boot paint, and never a partial refresh — those never pass `turnOffScreen`. Read in `Application::start`. |
| `kAutoSleepMinutes` | `1` | Read in `Application::update`. |
| `kPowerHoldSleepMs` | `kHoldDelayMs` (250) | Power-button hold before sleeping; a shorter press acts as Select. Defined *as* `kHoldDelayMs` rather than as its own number, so every button on the device splits tap from hold at the same moment. Read in `Application::update`. |
| `kHoldDelayMs` | `250` | How long a nav button must be held before it auto-repeats. Below ~250 ms a tap starts reading as two presses — the press itself already counts once — 250 is just above it. |
| `kHoldRepeatMs` | `200` | Gap between repeats once they start; lower is faster. |
| `kHoldAccelStep` | `0` | Extra list entries added per repeat in menus, so a long hold accelerates. `0` = constant one entry per repeat. It shipped as `0.9` on an `int` field, which truncates to `0` — the acceleration had silently never been on. Make the field a float if fractional steps are ever wanted. The reader ignores it either way: page turns never accelerate. |

Everything except `WG_BLUETOOTH_PAGE_TURNER` is an `inline constexpr`; that one
is a macro because only the preprocessor can keep a whole IDF stack out of the
image.

The file must open with `#include <cstdint>` — it declares `uint8_t`/`uint32_t`
constants and was relying on `Input.h` having included it first. It is
**gitignored**, because it is where the Wi-Fi password will live, so a fresh
clone does not build until the block in the README is pasted into place. That
block is the canonical default set and must be kept in step with this table.

Everything else is hardcoded **at the call site**. `Application` used to expose
each one as a `static constexpr` member function returning a literal
(`rotate_display()`, `show_sleep_text()`, `list_align()`, `menu_font_size()`,
`invert_menu_buttons()`, `invert_bottom_paging()`, `invert_side_buttons()`);
those are all gone, along with the branches they selected between. Don't
reintroduce that indirection layer:

- lists are always portrait — `buf.set_rotation(Rotation::Deg90)` directly — and
  the menu font is always the 32 px header face, so `ListMenuScreen`'s static
  `font_size_idx_` / `set_font_size()` / `font_size()` / `apply_ui_font()` and
  its `list_align_` (always centred) are gone too
- no sleep caption; `DrawBuffer::show_sleep_image_embedded()` lost its
  `show_text` parameter and `show_sleep_image(path)` had no callers at all. The
  sleep screen shows the cover of the
  book **currently open**, and only when it is not in `/.hidden/` — a hidden book
  must not be advertised by a screen that sits on display while the device is
  idle. Everything else, including sleeping from the home screen, gets the
  embedded wordmark image.
- battery is a header percentage, never an icon. The converted-book marker is
  gone — every book on the card is an WGB, so the trailing middle dot marked
  every row and said nothing (`show_converted_indicator()` and `wgb_exists` went
  with it)
- images always render: the `images_enabled` global is gone, not defaulted
- single reader font (Atkinson Hyperlegible, in the `font` partition), no picker
- book list is title-only, sorted most-recently-opened first with a hairline
  divider before never-opened books
- fixed page geometry: margins at `ReaderSettings::h_padding()` / `v_padding()`,
  and alignment, line spacing and font sizes all deferring to the book's own CSS

`ReaderSettings` is down to **one** live value — `font_size_idx` — plus
`rotate_reader_` on `Application`. Those two, and reading position, are the only
genuine runtime state; everything else about the page is compile-time.
Hyphenation is always on (English patterns; unknown language tags fall back to
none). The in-book quick menu is a flat list: the header block, Font Size,
Orientation, Clicker (only when a MAC is configured), then Chapters when the
book has a TOC.

### The quick menu's header is a row

`QuickmenuScreen`'s item 0 **is** the header block — book title, chapter and
their two percentages. Selecting it pops back to the book, and it is the default
selection, so opening the quick menu and pressing Confirm is a no-op round trip.

It is a real list item rather than a special case bolted onto the base class, so
navigation, wrap-around and `on_select` dispatch all work without changes. What
it is *not* is a row: `draw_all_` paints it at the top of the panel and inverts
the whole block (top of panel down to the rule) when `selected() == kIdxBack`,
and the list loop starts at `kIdxBack + 1`. Two places must agree that it costs
no height, or the last chapter becomes unreachable:

- `get_visible_count_` gives index `kIdxBack` a row height of 0.
- `draw_all_`'s `total_h` is `(n - 1) * row_h_()`.

## Repository map

The tree was flattened in `18fa42b`: the core library moved from `lib/wintergreen/`
to **`wintergreen/`** at the repo root, `platforms/desktop/` was deleted, and two
screens were renamed (`LyraExtScreen` → `HomeScreen`, `ReaderOptionsScreen` →
`QuickmenuScreen`). Anything still saying `lib/` is stale.

```
WintergreenConfig.h        the only tunables (gitignored — see Configuration)
CMakeLists.txt             IDF project stub; the real source list is platforms/esp32/
platformio.ini             one env, esp32c3; pre-build hook tools/generate_assets.py
sdkconfig.defaults         checked-in IDF config; sdkconfig.esp32c3 is generated
resources/                 AtkinsonHyperlegible.bin (reader font), sleep.mgr (wordmark)

platforms/esp32/           firmware entry point + every hardware driver, header-only
  main.cpp                 app_main: boot gate, init order, the while() loop
  epd.h                    EInkDisplay — SPI2 panel driver, LUTs, refresh modes
  input.h                  Esp32InputSource — 5 ms ADC-ladder sampler, press ring
  runtime.h                Esp32Runtime — frame pacing, battery percentage
  sdcard.h                 sd_init() — SDSPI on the same SPI2 bus, 20 MHz
  serial_communication.h   USB-JTAG command protocol (see below)
  bluetooth_clicker.h      BLE HID host for a page-turner; compiles away by default
  font_manager.h           ESP32 FontManager: mmaps the font partition
  font_partition.h         RawPartition — find + esp_partition_mmap, nothing else
  default_16MB.csv         partition table
  miniz_wrapper.cpp        compiles third_party/miniz as C++

wintergreen/               portable core
  Application.{h,cpp}      screen ownership, ScreenId, settings I/O, sleep screen
  Loop.{h,cpp}             run_loop_iteration() — poll, update, pace the frame
  ScreenManager.h          8-deep screen stack, start/stop/pause/resume
  Input.h                  Button, ButtonState, HoldRepeat
  Runtime.h                IRuntime
  FontManager.h            portable base: WGFS bundle parse into BitmapFontSet
  screens/
    IScreen.h              screen interface
    ListMenuScreen.{h,cpp} base list: header, rows, scroll, hold-repeat  (825 L)
    HomeScreen.{h,cpp}     recents carousel; overrides draw_all_ entirely (509 L)
    MainMenu.{h,cpp}       book list + Sync row + hidden-books mode      (342 L)
    ReaderScreen.{h,cpp}   the reader: paging, images, .pos, page cache  (858 L)
    QuickmenuScreen.{h,cpp} in-book menu + inline TOC                    (372 L)
  display/
    DrawBuffer.h           IDisplay iface + double buffer + all drawing (1224 L)
    ui_font_{small,large,header}.h   Iosevka Slab Medium, 14/24/32 px
  content/
    BitmapFont{,Format}.h  WGF1 glyph reader / format doc
    ContentModel.h         Paragraph/Run/TextStyle; kMaxFontSizes = 8
    TextLayout.{h,cpp}     line breaking, pagination, page cache        (1569 L)
    IParagraphSource.h     the interface TextLayout pages over
    BookIndex.{h,cpp}      singleton index + StringPool, .wintergreen-index
    StringPool.h           StringRef arena backing BookIndex
    CoverPaths.{h,cpp}     cover_*.bin + book.pos paths, kHomeCover/kSleepCover
    wgb/WgbFormat.h        on-disk WGB layout (magic WGB2, no version field)
    wgb/WgbReader.{h,cpp}  device read path
    wgb/WgbWriter.{h,cpp}  host-only, converter output
    wgb/WgbConverter.*     host-only, EPUB → WGB
    Book/EpubParser/CssParser/XmlReader/HtmlEntities   host-only (see Removed)
    ZipReader.{h,cpp}      still in firmware — reads embedded images
    ImageDecoder/JpegDecoder/PngDecoder                in firmware
    stb_image.h            vendored, used by the host converter only
    hyphenation/           Liang, English patterns only (27 KB trie)

tools/
  generate_assets.py       pre-build hook: builds the font/sleep flash images
  make_font.py             TTF → WGF1 / WGFS; --mono is mandatory here
  check_font.py            validates a bundle and ASCII-renders a sample
  make_sleep_image.py      PNG → MGR2
  convert-books.sh         nix-shell wrapper around tools/epub2wgb
  epub2wgb/                host converter (its own CMakeLists, explicit sources)
```

### Where behaviour actually lives

| Question | Read |
|---|---|
| Boot order, deep-sleep entry, serial op dispatch | `platforms/esp32/main.cpp` |
| Power button, auto-sleep, screen transitions, settings file | `Application::update` / `save_settings_` |
| What a list looks like and how it scrolls | `ListMenuScreen::draw_list_` + `get_visible_count_` |
| Page turn latency | `EInkDisplay::partial_refresh` + `ReaderScreen::render_page_` |
| Why a button does what it does | `input.h` thresholds → `Application::update` → the screen's `update()` |
| Why a cover looks the way it does | `HomeScreen::load_cover_`, `Application::show_book_cover_sleep_` |

### Bluetooth page-turner

A BLE HID-over-GATT **host** for a clicker, in
`platforms/esp32/bluetooth_clicker.h`. Off by default, off at every boot, and
off after any failure — the radio is the largest current draw this firmware can
switch on, tens of mA against the ~15 mA the CPU idles at.

**The switch is a `#define`, not an `inline constexpr`, and that is deliberate.**
`WG_BLUETOOTH_PAGE_TURNER` in `WintergreenConfig.h` holds the clicker's MAC;
leaving it commented out is what keeps the NimBLE stack out of the build, and
only the preprocessor can do that. It is the one setting in that file which is
not a constant. A first attempt used `#if config::kBluetoothPageTurner[0] != '\0'`
— `#if` cannot evaluate a C++ constant, and the error names the `::`, not the
config.

| Build | pio flash | pio RAM |
|---|---|---|
| no BT keys at all | 430,790 | 159,820 |
| BT keys, MAC undefined (**default**) | 434,580 | 160,216 |
| MAC defined | 728,888 | 164,128 |

So the sdkconfig keys cost **3.8 KB of flash and 396 bytes of RAM** in a build
that will never use them, and the stack itself is ~294 KB of flash — plus tens
of KB of *heap*, but only while the radio is actually on. app0 is 6.4 MB, so
none of this is capacity pressure.

**Three sdkconfig keys look wrong and are not.** IDF guards `nimble_hidh.c` —
the HID **host** — with `CONFIG_BT_NIMBLE_HID_SERVICE`, which is the HID *device*
service option, and that option lives in a Kconfig menu gated on
`BT_NIMBLE_GATT_SERVER`, which depends on `BT_NIMBLE_ROLE_PERIPHERAL`. So a
central-only device cannot compile the host without nominally enabling the
peripheral role. Nothing ever advertises. Drop any of the three and the link
fails with `undefined reference to esp_ble_hidh_init` — note this is a *link*
error naming an IDF file, with nothing pointing at the config.

Design, and why each part is the way it is:

- **It scans before connecting, and must.** Connecting straight to the
  configured MAC does not work in general, for two independent reasons. A BLE
  address does not carry its own *type* (public / random), and guessing wrong
  costs 30 s because `esp_hidh` calls `ble_gap_connect` with that timeout and
  blocks on it. And HID devices routinely use a **resolvable private address**,
  which rotates every ~15 minutes — `(addr[5] & 0xC0) == 0x40` identifies one —
  so a MAC read off a laptop an hour ago is not that device's address any more.
  An 8 s active scan yields the address *and* type that are true now. The
  configured MAC is therefore a filter, not a destination; if nothing matches
  it, anything advertising the HID service UUID (0x1812) is taken instead, which
  is what makes a rotating address work at all.
- **No reconnect.** A disconnect, a refused connection or one that never
  completes all drop straight back to Disconnected. A radio hunting for a device
  that is not there is precisely the leak this avoids.
- **No persistence.** Deep-sleep wake is a full boot, so the device always comes
  up with the radio cold. With `kAutoSleepMinutes = 1` that means re-enabling
  the clicker after any pause long enough to sleep — the deliberate trade, since
  turning pages resets `inactivity_ms_` and a session in progress never sleeps.
  If that becomes annoying the fix is a `bluetooth=1` key in settings and an
  auto-connect in `Application::start()`, at the cost of the radio coming up on
  a boot the user did not ask it to.
- **Everything blocking runs on a worker task.** `esp_hidh_dev_open()` blocks for
  the length of a connection attempt and the teardown calls block too, so
  `toggle()` spawns a one-shot task and returns. `g_busy` makes a second toggle
  during a transition a no-op rather than a queued one.
- **The HID callback never tears its own stack down.** It sets
  `g_teardown_request`; `wg_clicker::poll()`, called from the main loop, spawns
  the teardown task. Deinitialising a stack from inside its own event callback
  deadlocks.
- **NVS is initialised lazily**, in `bringup_stack()`, because the bond store is
  its only user in this firmware — a device with the radio off never pays for it.

Input arrives as **edges only**: `g_clicker_buttons` is merged by
`Esp32InputSource::poll_buttons` exactly the way `g_serial_buttons` is, and
nothing sets `current`, so a clicker press can never look held and drive
`HoldRepeat`. Reports are matched permissively — scan the payload for Left/Right
Arrow (0x50/0x4F) or Page Up/Down (0x4B/0x4E) rather than parsing the device's
report map, since `esp_hidh` has already done discovery and subscription and
clickers disagree about layout. A `g_last_key` transition check stops a held key
repeating.

Left advances and right goes back — **the opposite of a presentation remote's
convention**, and intentional. Injected as Button2/Button3, which are "next" and
"previous" in the reader in *both* orientations (the side rocker swaps, the
front row does not).

**Four things are required together, and each fails silently on its own.** The
first version had all four wrong and produced exactly one symptom — "Connecting"
for a minute, then "Disconnected" — with no way to tell them apart:

1. **The MAC must be byte-reversed.** NimBLE's `ble_addr_t.val` is
   little-endian: `val[5]` is the byte written leftmost in a MAC, which is why
   `ble_gap.c` tests `addr[5] & 0xC0`. `esp_hidh_dev_open()` memcpy's the array
   straight into `ble_addr_t.val`. `parse_mac()` fills `out[5 - i]` for this
   reason. Reversed, the stack cheerfully hunts for an address that does not
   exist and times out looking exactly like a clicker that is switched off.
2. **`CONFIG_BT_NIMBLE_GATTC_AUTO_PAIR=y`.** `esp_hidh`'s NimBLE path never
   calls `ble_gap_security_initiate`, and the HID characteristics need an
   encrypted link. This is the switch that makes `ble_gattc` retry a read
   through pairing when the peer answers `INSUFFICIENT_ENC`/`AUTHEN`. Without
   it the link comes up and then reads nothing.
3. **`ble_hs_cfg.sm_*` must be set in `bringup_stack()`.** NimBLE defaults
   `BLE_SM_BONDING` and *both* key-distribution masks to `(0)`, so pairing
   exchanges no keys and there is no bond. The ID key (IRK) is what later lets
   the stack recognise a rotating private address.
4. **The scan**, above.

**Still untested against a real clicker.** Both build paths link and the four
requirements above are now met, but nothing here has met the hardware. The
likeliest remaining adjustment is the keycode set.

#### Reading a failure

The quick-menu row is the **only diagnostic channel this firmware has** — there
is no log anywhere (see "No logging"), `esp_hidh` stores the reason a connect
failed in `dev->status` and frees `dev` before returning NULL, and every
`ESP_LOGE` inside NimBLE is compiled out. So the row carries the reason itself.

**Not found** means the scan ran and saw nothing: the clicker is off, asleep, or
still bonded to another host. **Failed N** is everything else, where N is
`stage * 1000 + gap_status`:

| Stage | Meaning |
|---|---|
| 1 | `WG_BLUETOOTH_PAGE_TURNER` is not a MAC |
| 2 | NVS (bond storage) unavailable |
| 3 | `esp_bt_controller_init` — almost always heap |
| 4 | `esp_bt_controller_enable` |
| 5 | `esp_nimble_init` |
| 6 | `esp_hidh_init` |
| 7 | `esp_nimble_enable` |
| 8 | host never synced with the controller |
| 9 | `ble_gap_disc` / `ble_hs_id_infer_auto` refused — scan could not start |
| 10 | found it; the GAP connect failed. `gap_status` says why |
| 11 | refused before allocating: free internal RAM below `kMinHeapKb` (24). Reports free KB |

**`kMinHeapKb` is a floor for the hopeless case, not an estimate of what BLE
needs.** It shipped at 64 — above what the device actually had free — and
refused every attempt on hardware that had been connecting successfully. A
threshold set at the real requirement rejects attempts that would have worked,
which is worse than the wedge it prevents.

Stage 8 is special-cased: it reports **free internal heap in KB** rather than a
GAP status, because NimBLE allocates from internal RAM only
(`MEM_ALLOC_MODE_INTERNAL`) and `esp_nimble_enable` **ignores
`xTaskCreatePinnedToCore`'s return value and always reports ESP_OK** — so a host
task that could not be allocated is indistinguishable from one that started and
never synced. `8045` means the sync timed out with 45 KB free.

`gap_status` is a NimBLE `BLE_HS_*` code from `host/ble_hs.h`: 0x02 `EALREADY`,
0x03 `EBUSY`, 0x07 `ENOMEM`, 0x0D `ETIMEOUT`, 0x0E `EDONE`. Values ≥ 0x200 are
`BLE_HS_HCI_ERR(x)` — a controller or peer rejection carrying an HCI error, e.g.
0x3E connection-establishment-failed, 0x08 connection-timeout.

It is captured by a **global GAP event listener**
(`ble_gap_event_listener_register`), which sees every GAP event regardless of
which callback owns the connection, so it can record the status without
displacing `esp_hidh`'s own handler. It is registered once, in
`bringup_stack()`, and never unregistered — the stack it belongs to is never
torn down either.

Stages 1–7 and 9 report `gap_status` 0, and stage 9 is deliberately **not**
collapsed into "Not found": a scan that could not start is a fault here, and
showing it as "nothing was advertising" sends the user to inspect a clicker that
was never the problem.

#### The reset-reason readout

The same row also shows `rst<N>` after an **abnormal** restart — panic (4),
interrupt WDT (5), task WDT (6), WDT (7) or brownout (9) — from
`IRuntime::last_reset_reason()`. Ordinary causes (power-on, deep-sleep wake, a
flash) report 0 and print nothing.

A device that resets otherwise has no way at all to say why: the panic text goes
to a console this firmware does not enable, and the reset wipes anything that
could have recorded it. `esp_reset_reason()` is the one thing that survives, and
distinguishing a brownout from a stack overflow decides which fix is even
plausible.

#### Stack sizes are not the defaults, deliberately

`CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` is **6144**, not IDF's 4096, and
`esp_hidh_config_t::event_stack_size` is 6144 rather than the example's 4096.
The NimBLE host task runs GATT discovery, the security manager, *and* — because
`ble_store_config` persists bonds — **NVS writes**, which are stack-hungry.
Bonding completes a second or two after a HID connect, which is precisely when a
device that resets shortly after connecting would do it.

#### Never use `p->input.data`

`nimble_hidh.c` allocates one block holding the event struct followed by the
report payload, points `input.data` at the payload, posts the block to the event
loop — which **copies** it — and then **frees the original**. The pointer that
arrives in the callback therefore refers to freed heap.

The copy has the payload at a known offset, so `hidh_event_cb` recomputes it as
`(const uint8_t*)p + sizeof(esp_hidh_event_data_t)`. The same applies to
`feature.data` if that event is ever handled.

#### The stack comes up once per boot and is NEVER torn down

`bringup_stack()` runs each init at most once, guarded by its **own** flag —
`g_ctrl_up` / `g_nimble_up` / `g_hidh_up` / `g_host_up`. Toggling the clicker off
calls `disconnect_only()` — cancel any scan, `esp_hidh_dev_close()`, back to
Disconnected — and leaves the controller, NimBLE and esp_hidh initialised for
the rest of the session.

**One flag for the whole function is not enough**, and the difference is the
same panic: a bringup that reached `esp_nimble_init()` and then failed at the
sync step left a single end-of-function flag false, so the next attempt re-ran
the one call that cannot be repeated. Per-step flags make a retry resume rather
than restart.

**Do not add a deinit path back.** Three attempts were made and the third is
this one; the first two each shipped a distinct bug:

1. **Unconditional teardown** → a bringup that died at the sync step still called
   `esp_hidh_deinit()` on a stack whose host had never started, leaving
   esp_hidh's `event_loop_handle` non-null. `esp_hidh_init()` returns `ESP_FAIL`
   ("Already initialised") when it is, so: *first attempt after a boot fails at
   stage 8, every attempt after that fails at stage 6, permanently, until a
   power cycle.* The second failure masked the first entirely.
2. **Per-stage tracked teardown** → fixed that, and then crashed:

   ```
   assert failed: ble_hs_init at ble_hs.c:928
     esp_nimble_init  (nimble_port.c:200)
     wg_clicker::bringup_stack()
   ```

   Line 928 is `SYSINIT_PANIC_ASSERT(rc == 0)` on the `os_mempool_init` of
   `ble_hs_hci_ev_pool`. With `CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC=y` — which is
   set here — `ble_hs_ctx` and its `hci_os_event_buf` are heap-allocated behind
   an `if (!ble_hs_ctx)` guard, and `esp_nimble_deinit()` does not restore those
   pointers. A second `esp_nimble_init()` therefore builds the pool on freed
   state and panics. **Cycling the NimBLE stack is not supported by this IDF,
   and it fails by crashing rather than by returning an error.**

IDF's own examples init the stack once and never deinit; this now matches.

**What it costs:** idle BLE current persists after toggling off, until the
device sleeps. Acceptable only because deep-sleep wake is a full boot and
`kAutoSleepMinutes` is 1 — putting the device down clears the whole stack within
a minute, and the radio is only ever up during a session where the user asked
for it.

A stage-8 failure is no longer terminal: the retry skips straight to
`esp_nimble_enable` and waits for sync again, because the steps below it are
already marked done.

#### Heap is the binding constraint, confirmed twice

The second confirmation is the more useful one, because it names the victim:

```
abort() ... std::bad_alloc
  operator new
  std::vector<LayoutWord>::_M_range_insert
  TextLayout::assemble_page   (TextLayout.cpp:827)
  ReaderScreen::prev_page_    (ReaderScreen.cpp:693)
```

**No BLE frame anywhere in it.** The radio simply took the internal RAM the
reader needed to lay out a page, and with exceptions disabled `bad_alloc` is an
`abort()` — a whole-device restart mid-read, reported afterwards as `rst4`.

So the failure is not in the clicker code and cannot be fixed there. Either BLE
gets smaller or the reader's peak allocation does.

To stop sizing this blind, the quick-menu row shows **free internal RAM in KB
while connected** (`Connected 34k`), via a negative `clicker_status_code()`.
That is the number that decides whether the radio and the reader coexist, and
there is nowhere else it can be printed.



A `Failed 8003` on a device that had connected minutes earlier means the sync
timed out with **3 KB of internal RAM free**. BLE's allocations all come from
`MALLOC_CAP_INTERNAL` (`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL`), on a build
that already holds ~160 KB statically out of 320 KB. It is not a stack-size
problem, and raising stacks makes it worse.

Everything below is sized for this device rather than left at IDF's defaults,
which assume a multi-peripheral hub:

| Key | Default | Here | Why |
|---|---|---|---|
| `BT_CTRL_BLE_MAX_ACT` | 6 | **2** | one scan + one connection; each slot carries its own controller buffers, and this is the largest single saving |
| `BT_CTRL_SCAN_DUPL_CACHE_SIZE` | 100 | 10 | only live during the few-second connect scan |
| `BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT` | 24 | 8 | 8-byte reports, not a data link |
| `BT_NIMBLE_MSYS_1_BLOCK_COUNT` | 12 | 6 | |
| `BT_NIMBLE_MSYS_2_BLOCK_COUNT` | 24 | 8 | |
| `BT_NIMBLE_TRANSPORT_EVT_COUNT` | 30 | 12 | |
| `BT_NIMBLE_MAX_BONDS` | 3 | 2 | |

`bringup_stack()` also **refuses to start below `kMinHeapKb`**, reporting free KB
as `Failed 11xxx`. That converts the worst failure mode — half initialise, time
out at stage 8, and leave a session that cannot retry — into a clean refusal
that names the number.

**Any early bail-out from `bringup_stack()` reaches `disconnect_only()`**, via
`connect_task`'s `if (!ok)`. That is why `disconnect_only()` guards
`ble_gap_disc_cancel()` behind `g_host_up`: calling it before `esp_nimble_init()`
dereferences a null `ble_hs_ctx` inside `ble_hs_is_enabled()` and takes a **load
fault, not an error return**. The heap check above introduced exactly this — the
device rebooted the instant the row was selected, before "Connecting" was drawn.
Any new pre-NimBLE failure stage inherits the same hazard.

### Serial protocol

`platforms/esp32/serial_communication.h` runs a FreeRTOS task (`serial_rx`, 8 KB
stack) over USB-JTAG with 2 KB tx / 4 KB rx driver buffers — about 14 KB of RAM,
allocated only once a host is attached (see below).

**It is a file-management channel only; there is no debug console.** Every log
line in the tree was deleted (see "No logging" below), along with the `'S'` heap
query, the `'Q'` state dump, the `'G'` flash benchmark and the `'P'` render
benchmark, so the device never emits unsolicited output and the host never has
to filter it. Live commands are `A` (dir list), `B` (inject buttons),
`K` (mkdir), `L` (list books), `N` (rename), `O` (open book),
`R` (recursive delete), `T` (read file), `W` (upload), `Z` (clear
`/sdcard/sleep/`). Frame magics are `EPUB`, `SIMG` and `CMND`; the `0xDEADBEEF`
LUT frame is gone with the grayscale tuning path.

Add / Rename / Remove do not execute in the receiver task: they are handed to
`main.cpp` through the single-slot `g_index_op` SPSC slot and applied there, so
they can be deferred while the reader owns the scratch buffers or the panel is
mid-refresh.

**The receiver task starts lazily.** `serial_start_if_connected()` is called from
the main loop the first time `usb_serial_jtag_is_connected()` returns true, and
is a no-op afterwards. On battery none of the ~14 KB is ever allocated and the
task never exists to wake the CPU on its 50 ms read timeout.
`usb_serial_jtag_is_connected()` reads the peripheral's state directly and does
not need the driver installed, so polling it is free — the loop already did, to
suppress auto-sleep.

### No logging

There are **no log statements anywhere in the tree** — no `ESP_LOGx`, no
`MR_LOGI`, no `HEAP_LOG`, and `wintergreen/HeapLog.h` is deleted. Don't
reintroduce them casually, and if you add one temporarily, remove it again.

Two things worth knowing before you assume this bought flash:

- With `CONFIG_LOG_DEFAULT_LEVEL_NONE` the macros already expanded to nothing —
  the format strings were **not** in the image (verified by searching
  `firmware.bin` for them). The removal is a source-cleanliness change, and the
  measurable savings came from the *diagnostic commands* and the timing
  scaffolding that fed the logs, not from the log calls themselves.
- Removing a log whose call was the entire body of an unbraced `if`/`else`
  silently re-points that branch at the next statement. That happened in nine
  places here and only two were caught by the compiler. If you ever strip logs
  in bulk again, scan the *original* source for a control statement whose next
  non-blank line is a log macro, rather than trusting warnings.

`esp_log_level_set()` calls are gone too: the upload paths used to silence
logging for the duration of a binary transfer so a stray `I` byte could not be
read as an ACK. With nothing logging, there is nothing to silence.


### Assets are flashed, not embedded

Neither the reader font nor the sleep image is in the app image. Both are raw
partitions declared in `platforms/esp32/default_16MB.csv`:

| Partition | Offset | Size | Holds |
|---|---|---|---|
| `font` | `0xC90000` | 512 KB | the inflated WGFS v2 bundle, 428,289 B |
| `sleep` | `0xD10000` | 128 KB | `resources/sleep.mgr` verbatim, 96,008 B |

`tools/generate_assets.py` runs pre-build. It inflates
`resources/AtkinsonHyperlegible.bin` (which is `[uint32 raw size][zlib]`) **on the
build machine**, copies `sleep.mgr` unchanged, writes both to `$BUILD_DIR`, checks
each against its partition size, and appends them to `FLASH_EXTRA_IMAGES` so
`pio run -t upload` writes all four images in one esptool invocation. It rewrites
an image only when the bytes change.

At runtime `RawPartition::map()` (`platforms/esp32/font_partition.h`) is the whole
story: `esp_partition_find_first` + `esp_partition_mmap`, no header, no CRC, no
copy, no inflate. `FontManager::init()` parses the WGFS table straight out of the
mapping; `main.cpp` hands the sleep mapping to `DrawBuffer::set_sleep_image()`.

This replaced a provisioning path that inflated the font out of an embedded ASTS
blob into the partition on the first boot after every firmware update — roughly
15 s of flash erase-and-write behind an "Installing fonts..." progress bar, with
the font stored twice. Gone with it: `asset_blob.{h,cpp}`, `assets_embedded.S`,
`tools/build_assets.py`, `FontPartition::provision_embedded` /
`needs_provisioning` / `write` / `invalidate` / `fits_partition` /
`provision_uncompressed_file`, `FontManager::ensure_ready`,
`DrawBuffer::show_loading` / `render_loading_box_` / `mini_target_` and the whole
`kLoad*` / `kBar*` geometry block, and `IDisplay::partial_refresh_region` with its
`EInkDisplay` override.

**Adding an asset means editing three places in step**: the CSV, `PARTITIONS` in
`generate_assets.py`, and the `map()` call site. The Python asserts the image fits,
so a mismatch fails the build rather than the device.

Note the font partition holds the **inflated** bundle: 428 KB, not the 122 KB
compressed form the old blob carried. Flash is not scarce and the device does no
work — that trade is the point.

### Measured size budget

`.pio/build/esp32c3/firmware.elf`, current tree. app0 is 6.4 MB so there is no
capacity pressure — this is about upload time and instruction-cache pressure.

| Section | Now | Before the 2026-08-19 pass |
|---|---|---|
| image (`firmware.bin`) | 442,656 | 745,712 |
| `.flash.text` | 311,504 | 403,550 |
| `.flash.rodata` | 93,480 | 313,256 |
| `.iram0.text` | 26,580 | 25,546 |
| `.dram0.bss` | 157,200 | 109,816 |

"Now" is the default build — `WG_BLUETOOTH_PAGE_TURNER` undefined. Defining it
adds ~294 KB of flash and ~3.9 KB of static RAM; see "Bluetooth page-turner".

Where `.flash.text` actually goes, by symbol prefix (approximate — weak symbols
and inlining blur the edges, and ~245 KB of it is not ours):

| Area | Bytes |
|---|---|
| IDF + picolibc + C++ runtime | ~245,000 |
| screens (`ListMenuScreen`, `HomeScreen`, `MainMenu`, `Quickmenu`, `Reader`) | 51,476 |
| book format + index (`Wgb*`, `BookIndex`, `CoverPaths`) | 17,228 |
| drivers (`EInkDisplay`, input, runtime) | 15,276 |
| `DrawBuffer` + drawing | 14,356 |
| app shell (`Application`, `ScreenManager`) | 9,606 |
| text layout + font parsing | ~11,000 |

And inside the IDF share, the pieces big enough to argue with:

| | Bytes | Removable? |
|---|---|---|
| FatFs + VFS-FAT | 16,814 | no — the card is FAT32 |
| SD (`sdspi`/`sdmmc`) | 14,814 | no |
| C++ runtime + STL | 14,422 | only by writing less STL |
| SPI master driver | 13,750 | no |
| power mgmt + sleep + clocks | 12,998 | no — this is what DFS and deep sleep run on |
| FreeRTOS | 11,204 | no |
| picolibc stdio/string/malloc | 10,578 | ~5 KB of it is `__d_vfprintf`/`__d_vfscanf` |
| mbedtls SHA | 7,896 | no — image validation |
| heap allocator | 7,630 | no |
| VFS core | 6,230 | partially, and not worth it |
| `f_mkfs` | 2,360 | unreachable, but referenced unconditionally |
| timezone/`localtime_r`/`mktime` | ~2,700 | no — `vfs_fat.c` uses it for FAT timestamps |

The last two are the shape of everything left: reachable only through IDF code we
do not control, so removing them means patching the framework in place — which is
exactly what `tools/patch_ffconf.py` was deleted for.

BSS is up 47 KB on purpose: `DrawBuffer`'s third framebuffer, carrying both the
quick-menu snapshot and the pre-drawn next page — see "The spare framebuffer".
Static use is 159,820 of 327,680, leaving ~165 KB of heap.

rodata fell by 220 KB because the reader font and sleep image left the app image
for their own flash partitions. What remains is dominated by the three UI fonts
(54,138) and the hyphenation trie (26,943) — together 87% of it. Both could move
to a partition too if the image size ever mattered; it currently does not.

`.flash.text` fell by 86 KB, largest first: ~34 KB of `std::sort` /
`std::stable_sort` template instantiations (see "Sorting"), ~24 KB of image
decoders unlinked when images became pre-rasterised (`JpegDecoder`, `PngDecoder`,
`ImageDecoder`, `ZipReader`, `stb_image`, miniz), ~4 KB of triplicated cover-path
bodies, ~3 KB of mbedtls SHA-512 and ~2 KB of `esp_mprot_set_prot` unlinked by
config changes, and the rest from the removed benchmarks, grayscale LUT tables,
font provisioning and diagnostic commands.

Re-measure rather than trusting these:

```
~/.platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-nm \
  --print-size --size-sort --radix=d .pio/build/esp32c3/firmware.elf | tail -40
~/.platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-size -A \
  .pio/build/esp32c3/firmware.elf
```

**`-Os` was measured and rejected.** `CONFIG_COMPILER_OPTIMIZATION_SIZE` takes
`.flash.text` from 346,482 to 243,666 — 30% smaller — and the image to 374,816.
It is *not* the setting in use: this device is tuned for page-turn latency, flash
is abundant, and the reader's hot loops (`draw_glyph_impl_`'s bit blitting,
`word_width`'s per-character scan) are exactly the code `-Os` stops unrolling and
inlining. The theory that a smaller hot path wins back more from the 16 KB
I-cache than it loses is only worth revisiting **with a device and a stopwatch**;
the size number alone does not settle it. `CONFIG_COMPILER_OPTIMIZATION_PERF`
(-O2) stays.

## Architecture

`Application` (`wintergreen/Application.{h,cpp}`) owns every screen as a
by-value member and a `ScreenManager` stack. Screens never push directly; they
call `push_screen`/`replace_screen`/`pop_screen`, which queue a `ScreenId` that
the app applies after `update()` returns. `screen_for_(ScreenId)` maps the enum
to the member.

Screen stack: `HomeScreen` (home) → `MainMenu` (all books) → `ReaderScreen`
→ `QuickmenuScreen` (quick menu). There are only these four screens.

Back on the home screen opens the book list. See "The root screen depends on
whether the library is empty" below for the fresh-card case.

**Opening a book from the list *replaces* the list.** `MainMenu::on_select` calls
`replace_screen(Reader)`, not `push_screen`, so the stack goes
`HomeScreen → ReaderScreen` and Back from the reader lands on the home carousel
rather than back in the list. That is the intended flow: the carousel is sorted
most-recently-opened first and `record_book_opened()` has already bumped this
book to the top, so Back shows the book you were just reading. It also drops the
list's `entries_` immediately.

`ScreenManager::replace()` exists for this and is **not** `pop()` + `push()`:
`pop()` resumes whatever is underneath, so the old sequence had `HomeScreen`
rebuild its carousel, re-read the book index and decode a cover — all discarded
one line later when the push paused it again.


**The root screen depends on whether the library is empty.** `Application::start()`
loads the book index (both screens load it only when empty, so this costs nothing
extra) and pushes `MainMenu` instead of `HomeScreen` when there are no books at
all: an empty carousel has nothing to show and its only useful action is "Back for
all books", while the list is also where Sync lives — the one thing that can fix an
empty library. Two consequences:

- `MainMenu::on_back()` does nothing when `app_->at_root()`, since there is no home
  screen underneath to return to.
- `MainMenu::on_select()` *pushes* the reader at root instead of replacing, so
  Back from a book returns to the list rather than emptying the stack.

`ScreenManager::pop()` also refuses to remove the last screen outright. An empty
stack makes `top()` null, and `Application::update` then silently stops processing
input — indistinguishable from a freeze.

**The carousel falls back to the library when nothing has been opened.**
`HomeScreen::on_start()` picks the five most recently opened books; if none have
been (a fresh card, or wiped settings) it shows the alphabetically first books
instead, ordered with the shared `ci_less()` so "first" means the same thing here
as in the book list's never-opened group. The "No books on the card" panel is now
only a safety net — `Application::start()` routes a genuinely empty library to the
list.
**Row 0 of the book list is a `Sync` action, not a book.** Row 1 is its hairline
(pushed into `separators_` like any other), and books start at row 2 — so
`entries_index_for()` subtracts one *on top of* the separator shift, and
`visual_for_entries()` skips both kinds of non-book row. Every derived separator
position in `populate_list_()` carries the same `kFirstBook = 2` offset; getting
that wrong shows up as the wrong book opening, not as a drawing glitch.

The row does nothing yet: `run_sync_()` is an empty hook, since no Wi-Fi
subsystem exists (`kWifiSsid`/`kWifiPassword` are still placeholders). It is the
single entry point for the planned NAS sync.

`select_first_book_()` moves the cursor off Sync onto the first book when the
screen opens. It is called from `on_start()` and from `update()`'s scan branch —
deliberately **not** from `populate_list_()`, which also runs on an in-place
refresh (a serial upload) where moving the cursor under the user would be wrong.
With no books at all there is no hairline and Sync is the only row.

**Hidden books are a mode of `MainMenu`, not a screen.** A 1 s back long-press
**on the home screen** opens the list with the books under `<books_dir>/.hidden/`
at the top, above their own hairline divider (`set_show_hidden()` before the
push). That gesture is the only way to see them, and `MainMenu::stop()` clears
the flag, so leaving the list means performing it again.

**The hold fires while the button is still down**, the moment `kHiddenHoldMs`
elapses — waiting for the release made the device look dead for the length of the
hold. That is safe *here* because presses are rising edges only (`input.h` queues
them on the edge), so `MainMenu` receives no Button0 event from a button that was
already down when it started, and `back_consumed_` makes the eventual release a
no-op. The plain book list still opens on release, since a tap cannot be
distinguished from a hold any earlier.

The gesture lives on the home screen rather than in the list because the *tap*
resolves on **release**, and only a release-time push lands on a screen whose
buttons are up. It was first written in `MainMenu`, which pushed on the *press*: the pushed
screen started with Button0 still physically down, its own release handler fired,
and the list vanished the moment the user let go — it stayed visible only while
the button was held. `HomeScreen` also swallows the first hold after every
`start()` (`back_ignore_`), because `MainMenu` pops on the back press and hands
the home screen a button that is still down; without that, the release would read
as a fresh tap and reopen the list. **Any screen pushing another from a button
press inherits this problem.**

Four things the hidden list itself depends on:

- `.hidden` books are **not in `BookIndex`** (its scan skips dot-directories),
  so `MainMenu::scan_hidden_()` walks the folder itself and reads title/author
  straight from each WGB. Those entries carry owned `title_own`/`author_own`
  strings rather than `StringRef`s, because there is no pool entry to reference.
  The scan runs only when the gesture first asks for it.
- `on_select` skips `record_book_opened()` for a hidden entry. `mark_opened()`
  would be a no-op anyway, but the point is that a hidden book must never reach
  the recents carousel on the home screen.
- `separators_` holds **visual** indices, so the hidden divider shifts the
  recently-opened/never-opened one below it by a row. `populate_list_()` inserts
  the hidden group after sorting, so it keeps its own alphabetical order.
- Hidden books are `.hidden/<book>/book.wgb` like any other. The old
  `HiddenBooksMenu` scanned for `*.epub`, which stopped matching anything the
  moment EPUB support was removed — it was listing an empty folder.

The home screen is a **carousel of the five most recently opened books**, not a
list: battery percentage top right, one large cover inside a 2 px frame held
10 px clear of it, title (up to two lines) and author centred directly beneath
it, and a row of diamonds for position (the selected one carries a second,
larger outline diamond). Only title lines actually drawn advance the cursor, so
the author never floats under a reserved blank line; the diamonds are anchored
to the bottom of the panel so they don't move with it.

Up/Down are **swapped** on this screen (`swap_updown_`, applied to the state
forwarded to the base class): the carousel runs left-to-right, so the top of the
side rocker and the bottom front key have to advance it, which is the opposite of
what a vertical list wants. Select opens the book, and **Back opens the full book list** —
which is why there is no longer a Recent Books screen (`RecentBooksScreen` is
deleted, along with `ScreenId::RecentBooks`). It still derives from
`ListMenuScreen` purely for the item/selection/navigation machinery; every pixel
comes from its own `draw_all_` override, so the base header, footer and list
passes never run.

Two things about that screen worth knowing:

- The selected book's cover is loaded on demand **from inside `draw_all_`**
  (hence the `mutable` cover members). Loading it from `update()` instead would
  draw one frame with the previous book’s cover and then redraw — two e-ink
  refreshes per press.
- The cover comes from **`cover_home.bin`**, which the converter renders to fit
  the box exactly (`kHomeCoverW` × `kHomeCoverH` = 424x479 in
  `content/CoverPaths.h`), so the scaler below collapses to a 1:1 copy and the
  image is dithered exactly once, from the original grayscale. `cover.bin`
  (160x240) is the only fallback, and it can only be pixel-doubled.
  **`cover_sleep.bin` is not a fallback here** — it is composed for the panel
  with letterbox bars baked in, so blitting it into the carousel box would draw
  the bars too.
  Those two constants mirror `draw_all_`'s layout maths; if the layout moves
  they drift, the fallback path takes over and quality quietly regresses until
  the library is re-converted. Nothing breaks.
- Two scaling paths, and the distinction matters. **Upscaling is whole-number
  only**: pixel-doubling keeps the 1-bit dither intact where a fractional
  nearest-neighbour scale moirés it. **Downscaling is a box filter plus a 4x4
  ordered dither** — the average of a cell is a coverage fraction, and a hard
  50% threshold would throw away every tone between paper and ink. It streams
  the source a row at a time with one accumulator row. Nothing the converter
  writes actually reaches the downscale path any more (`cover_home.bin` is
  exactly the box and `cover.bin` is smaller); it is kept for the case where the
  layout box shrinks below `kHomeCoverW`/`kHomeCoverH` before a library is
  re-converted.
- All-white rows are **trimmed off the top and bottom** afterwards. Covers often
  carry a white band above and below the artwork, and with the frame drawn a
  fixed distance from the bitmap those bands read as lopsided padding. Columns
  are left alone — the cover is fitted to the box width, so side bands are rare.
- The cover read calls `wait_panel_idle()` first: the card shares SPI2 with the
  panel and this is a much longer read than the old thumbnail.

The sleep screen is `show_book_cover_sleep_` in `Application.cpp`, and there is
**nothing left in it but a blit**. `cover_sleep.bin` is composed by
`Book::write_sleep_cover_bin` at exactly `kSleepCoverW` × `kSleepCoverH`
(480x786 — see below; **not** 800) with the artwork already trimmed, scaled, centred
and its letterbox bars already filled, so the device streams it a row at a time
straight onto the panel — no 48 KB buffer, no scaling, no tone measurement. A
file of any other size is from an older conversion and is **ignored**; the
wordmark shows instead until the library is re-converted.

What the converter does, and why each step is there:

- It decodes with `scale_to_fill`, so a small cover is scaled up from the
  *grayscale* and dithered once, rather than dithered small and pixel-replicated.
- It **crops fully blank rows and columns**. Publishers bake white bands into
  cover artwork, and stretching those to the panel only makes them bigger.
- It then **stretches the artwork to the full panel — aspect ratio is not
  preserved.** This is deliberate. Fitting inside the panel leaves letterbox
  bars, and on a 1-bit panel there is no good colour for them: white bars under a
  dark cover look like a bug. The alternative shipped briefly — measure the ink
  coverage of the artwork edges each bar abuts and fill the bars to match — and
  is **gone**, along with `edge_coverage_` and `fill_band_` in `Application.cpp`.
  Covers sit close enough to the panel's 480:786 that filling it outright reads
  as a full-bleed image, and there is no bar geometry left to get wrong.

Nothing is cropped off the artwork; it is stretched, not filled by cropping.

**The panel's app-space height is 786, not 800.** `DisplayFrame::kPhysicalWidth`
is 786 — the glass has hidden rows top and bottom — so in portrait
`DrawBuffer::height()` is 786 and `width()` is 480. Both cover constants are
derived from that, and both were once written against 800: `kSleepCoverH` by 14
px, which made *every* sleep cover fail the exact-size check and fall back to the
wordmark, and `kHomeCoverH` by the same 14, which silently put every home cover
through the box-filter-and-re-dither path it exists to avoid. `Application.cpp`
now `static_assert`s the sleep constants against `DrawBuffer::kWidth`/`kHeight`,
so that half can no longer drift; the home box still depends on font metrics and
has to be re-derived by hand if the layout moves.

There is no separate Chapters page: `QuickmenuScreen` lists the book's TOC
inline, below Font Size and Reader, and scrolls. Selecting a chapter sets
`pending_chapter()` / `pending_para_index()` and pops one screen;
`ReaderScreen::resume()` consumes them. Two things that merge broke and had to be
handled: chapter titles legitimately contain `": "` ("Chapter 1: The Beginning"),
so the `Label: Value` split is gated to non-chapter rows or the title gets torn
into two columns; and nested TOC entries carry a depth that must still indent,
via `ListMenuScreen::get_item_indent()`.

Its header is drawn entirely by its own `draw_all_` — the base class's card
header is unused, so `title_`/`subtitle_` are left empty. It is book title,
current chapter, rule; there is **no battery** (it is an overlay on the book, and
the reader underneath shows none either). Both the title and the chapter are word
wrapped (`wrap_`, 2 and 3 lines) and each carries its percentage right-aligned on
its **first** line, set in the *same font as the text beside it* so the pair reads
as one line. The chapter is a size down from the title (`chapter_font_()` =
`section_font_`), like a subtitle. The old `Book X% · Chapter Y%` row is gone, as
is the author (it is on the home screen and the book list already). `header_h_()`
measures that block from the wrapped lines computed in `on_start()`, and
`get_visible_count_()` calls it — the two used to re-derive the height
independently, which is exactly how scroll bugs get in.

A hairline divides the settings from the chapters, drawn in a `kSeparatorH` gap
of its own between the two rather than inside the first chapter’s row — a selected row
fills its whole rect, and a rule drawn inside it would be painted over. Row
height is `row_h_()` = the list font's `y_advance()` + padding, not a constant:
the list is set in the 32 px header face and the old fixed 28 px row was shorter
than a single line of it.

The book title comes from `ReaderScreen::display_title_()`, not
`wgb_.metadata().title`: a converted EPUB with no usable metadata title yields
the literal string `"none"`, and `BookIndex` already substitutes the containing
folder name for it. Without the same substitution here the quick menu said
"none" for a book the home screen named correctly.


### Sorting — never use std::sort here

`std::sort` and `std::stable_sort` are templates on both the iterator *and* the
comparator, so every call site gets its own copy of introsort, insertion sort,
`__rotate` and `__merge_adaptive`. Three call sites over two element types cost
**~34 KB of flash** — more than every screen's drawing code put together — to
order lists that are at most `MAX_BOOKS` (250) long and are built once per screen
entry. They are gone:

- `MainMenu` has one file-static `stable_sort_indices_()`: a bottom-up merge sort
  over a `uint16_t` permutation, with the comparator as a **plain function
  pointer** so there is exactly one copy in the image no matter how many callers
  appear. `iota_indices_()` builds the identity permutation and
  `apply_permutation_()` reorders the vector afterwards. Sorting indices rather
  than elements also means `BookEntry`'s four `std::string`s move once instead of
  O(n log n) times.
- `HomeScreen::on_start` does not sort at all. It runs a five-pass top-K
  selection straight over `BookIndex::entries()`, so it never materialises
  path/title/author for books it is about to discard. The old version built a
  `std::vector<Raw>` — a `uint32_t` plus six `std::string`s per entry — for every
  book ever opened and then truncated it to five.

If you need another ordering, reuse `stable_sort_indices_` (move it to a shared
header if a second file needs it). Do not reach for `<algorithm>`.
### ListMenuScreen

Base class for every list screen, in `screens/ListMenuScreen.{h,cpp}`. There is
**one** visual theme (formerly "Lyra Extended"); the `MenuTheme` enum and the
Chronicle/Minimal/Stele/Codex/Lyra variants are gone. Two per-instance booleans
select the remaining layout variations:

- `detail_list_` — two-line rows (title + subtitle, full-width divider, right-hand
  column). Set by MainMenu. The right-hand column is the reading percentage and
  sits on the **subtitle** baseline, bottom right beside the author, so the title
  gets the full row width. The **last** row draws no divider — there is nothing
  below it to divide from — and neither does a row whose next row is a section
  hairline, or the two rules land a few pixels apart and read as a double line.
- `plain_list_` — centred-title header instead of the battery status bar. Was set by
  ChapterSelectScreen.

**There is one separator, defined once.** `ListMenuScreen::kSeparatorH` (14 px)
and `draw_separator_()` are the only hairline geometry in the tree, and every
list uses them: the book list, the book-details list, the plain centred list and
`QuickmenuScreen`, which draws its own list but inherits both. Before that
each site carried its own `kSepH` — 8, 12 and 14 — so the same divider sat
tighter in the book list than in the quick menu. Don't re-introduce a local one.

Row height is per-row, not per-list: `wintergreen_slot_h_(index)` returns the
one-line height for a row where `is_single_line_row(index)` is true (MainMenu's
Sync action, which never has an author beneath it) and the two-line height
otherwise. `wintergreen_visible_from_` asks per index too, so scrolling stays
consistent with what is drawn.

No screen draws button labels, nav-arrow glyphs or a battery icon: the labelled
`Back | Select` / `Down | Up` boxes and the ◀▶▲▼ hint row were removed, and
`draw_bottom_` now only draws the book-details footer. Don't add them back — the
buttons are fixed and the panel is small. Battery appears once, as `NN%` in the
header (`draw_header_`, and `HomeScreen`'s own header).

Both false = standard header + centred single-line rows. A non-empty `subtitle_`
switches the header into the book-details card and is checked before either flag.

**The header is battery-only.** There is no "wintergreen" wordmark on any screen
and no rule under the header — the book-details card drew the wordmark in a bar
of its own and that is gone too. The card's footer no longer repeats the battery
either. `header_override_`, which swapped the wordmark per screen, went with the
wordmark.

The header's height is `battery_row_h_() + kHeaderBottomGap` in **both**
`draw_header_` and `compute_header_h_`. It used to be measured off
`header_font_` (41 px) while the percentage is drawn in `section_font_` (31 px),
so the list started 10 px lower than the text needed and the gap above the first
row looked lopsided against the tight one above the battery.

Every screen draws the percentage through `ListMenuScreen::draw_battery_`, and
`HomeScreen` — which otherwise shares no drawing code with the base — calls it
too, sizing its own header from `battery_row_h_()`. The position is a property of
the base class rather than of each screen on purpose: when the home carousel had
its own copy of the maths it drifted from the list's by a few pixels in both
axes, which is plainly visible as a jump when moving between them. Don't
reintroduce a per-screen `W - pad - pw`.

Drawing is four passes: `draw_header_` → `draw_bottom_` → `draw_list_`, with
`compute_header_h_` mirroring the header maths for scroll calculations.
`get_visible_count_` must stay consistent with `draw_list_`'s branch selection —
they are separate functions keyed on the same conditions, and divergence causes
scroll bugs.

### Content pipeline

EPUB → WGB (a preprocessed binary layout format) → rendered pages.

**Images are rasterised by the converter, not decoded on the device.**
`WgbImageRef` is `{data_offset, data_size, width, height}` (12 bytes) addressing a
raw 1-bit bitmap inside the WGB: `(width+7)/8` bytes per row, MSB first, bit clear
= ink. `data_size` is always exactly `(width+7)/8 * height`, which is worth
asserting if you ever touch the writer.

The converter decodes the source JPEG/PNG, scales it to fit
`kWgbImageBoxW × kWgbImageBoxH` (480 × 772 — the portrait panel width and the page
height less its vertical padding) and dithers it, all on the build machine. The
device seeks and blits rows. Consequences worth knowing:

- **`width`/`height` in the table are the drawn dimensions**, not the source's,
  because `scale_image()` never enlarges (see "Refresh latency"). In portrait the
  blit is 1:1; a landscape page, whose content area is wider, centres the stored
  bitmap rather than stretching a dithered image with no detail to recover.
- **The image table beats the EPUB's declared width/height.**
  `resolve_inline_image()` asks `make_image_size_query()` first and falls back to
  `attr_width`/`attr_height` only when there is no raster — markup dimensions no
  longer match what is stored.
- **`ReaderScreen::draw_image_` reads through `WgbReader::file()`**, the handle the
  book is already open on, so a page with three figures does not open the book
  three times. Every `WgbReader` read seeks first, so borrowing the handle cannot
  disturb its position.
- There is **no image cache on the card**. `img_<key>_<w>x<h>.bin` is gone, and
  with it the largest writer the reader had.

What this bought, measured on The Hobbit: image bytes 2,077,395 → 423,652 (−80%),
the file 2.71 MB → 1.06 MB (−61%), and `JpegDecoder`, `PngDecoder`,
`ImageDecoder`, `ZipReader`, `stb_image` and miniz all left the firmware build
(~24 KB of flash, plus their heap and scratch use). An image page now turns at
the speed of a text page.

`WgbReader::open()` reads the chapter and image tables with **one `fread` each**
and parses from memory, and `WgbChapterSource`'s constructor does the same for the
per-chapter paragraph descriptor table. One `fread` per entry costs a FATFS + SPI
round trip apiece and a long book has hundreds of each, so those loops dominated
open time. The spine-filename and anchor tables are **gone from the format**:
both existed only to resolve hyperlinks, which are no longer parsed. Dropping
them also removed the anchor `.tmp` file `WgbWriter` streamed during every
conversion, and the header's `anchor_offset` with it (28 → 24 bytes).

`content/wgb/WgbConverter` does the conversion; `WgbReader` is the read path.
There is no on-device conversion at all, so `tools/convert-books.sh <epub-dir>
<out-dir>` is the only way a book gets made. It builds and runs `tools/epub2wgb`,
which reopens each output with `WgbReader` — the same code the device runs — so a
file that would not load is reported at conversion time.

Two things about the converter that matter on a slow machine (it is meant to run
on a Raspberry Pi):

- **It converts books in parallel**, one per hardware thread. Each conversion owns
  its `Book`, its two 48 KB scratch buffers and its own output directory, and the
  pipeline holds no mutable global state, so only the reporting needs a mutex.
  Output is byte-identical to the single-threaded result. The per-book progress
  percentage is suppressed when there is more than one worker.
- **The script only rebuilds when the binary is stale.** It used to run `cmake
  configure` + `cmake --build` on every invocation. Converting three books takes
  0.15 s; a cold compile of the converter is ~18 s here and minutes on a Pi, so
  the build, not the conversion, was what you waited for. `needs_build()` compares
  the binary's mtime against every source it is built from.

The converter links only the content pipeline. `WgbConverter` includes
`DrawBuffer.h` purely for the `kWidth`/`kHeight` constants, so no display or SDL
code is involved.

Output is one folder per book:

```
<out>/<book name>/book.wgb
<out>/<book name>/cover.bin        # 160x240, book list
<out>/<book name>/cover_home.bin   # fitted to 424x479, home carousel
<out>/<book name>/cover_sleep.bin  # 480x786, composed for the panel (bars included)
```

The three cover files are written only when the EPUB has a cover. The
`book.wgb` filename is not arbitrary — `convert_epub_to_wgb_streaming()` looks
for the literal string `book.wgb` in the output path and writes the covers
beside it, so renaming the output breaks cover extraction silently.

**Finding the cover in the EPUB is three fallbacks deep**, and it has to be.
`EpubParser` used to accept only `<meta name="cover" content="ID">` in the OPF
metadata, which EPUB 3 does not require and plenty of EPUB 2 files never wrote —
those books produced *no* cover files at all, so the carousel drew a placeholder
outline and the sleep screen showed the wordmark, with nothing anywhere saying
why. It now falls back, in order, to a manifest item whose `properties` contains
`cover-image` (the EPUB 3 spelling), one whose `id` is literally `cover`, and one
whose href basename starts with `cover`. All three are restricted to image media
types, and a meta-declared cover always wins.

Note the fallbacks can only find what the book has: a "cover" that is really a
rendered title page of body text dithers to a near-blank panel, and that is the
file doing its job, not a decode failure.

`WgbConverter` is still compiled into the firmware and is still the fallback when
an **EPUB** is opened without a cached WGB.


### File formats: WGB and WGF

Two binary formats, both **magic-as-version**: there is no version field in
either. An incompatible layout change changes the four magic bytes, so a stale
file is rejected by the existing magic check rather than misparsed by a reader
trusting a version number it does not understand. Bump the digit when the layout
changes, and re-convert.

| | Magic | Holds | Defined in |
|---|---|---|---|
| Book | `WGB2` | one book: paragraphs, chapter/image tables, metadata, TOC, image bitmaps | `content/wgb/WgbFormat.h` |
| Font | `WGF1` | one font at one pixel size, up to four styles | `content/WgfFormat.h` |
| Font set | `WGFS` | a count, a size table, and N `WGF1` blobs | `tools/make_font.py` `pack_bundle()` |

They were MRB/MBF4/FNTS. The rename came with three simplifications, all of which
mean any file produced before it must be re-converted:

- `WgbHeader` lost its `version` and `flags` fields (32 → 28 bytes) and later its
  `anchor_offset` (28 → 24). The magic went to `WGB2` with that second change.
- `WgfHeader` lost its `version` byte: 50 → 49 bytes. **Every absolute offset
  stored in that header shifted down by one** — `bitmap_data_offset`, the three
  style offsets, `kerning_offset` and the two gray-plane offsets. Per-glyph
  `bitmap_offset` is relative to `bitmap_data_offset`, so it did not move. Getting
  this wrong produces a font that parses and renders garbage, which is why
  `tools/check_font.py` renders a sample string rather than just checking sizes.
- The `WGFS` bundle lost the version byte and the 32-byte font name. The name was
  never read — there is one reader font and it is found by *partition*, not by
  name — so `--name` is now a diagnostic label only.

`resources/AtkinsonHyperlegible.bin` is a `WGFS` bundle behind a
`[uint32 uncompressed size][zlib stream]` wrapper; `tools/generate_assets.py`
inflates it on the build machine. The three `display/ui_font_*.h` headers are bare
`WGF1` blobs emitted as C arrays (`kFontData_ui_*_wgf`).

Regenerating either is `tools/make_font.py` (needs freetype-py; see "Fonts").
Validate with `tools/check_font.py`, which parses the way `BitmapFont.h` does and
ASCII-art renders a string — a structurally broken font renders blank on device
with no error.
### Books on the card

Two shapes are recognised, both found by `BookIndex`'s recursive scan (which
skips dot-directories, so the state files and any `.hidden/` folder are never
picked up as books):

- `<book name>/book.wgb` — a converted book, opened **in place**. Metadata comes
  from the WGB itself, no EPUB parse. Three covers sit beside it: `cover.bin`
  (160x240, book list), `cover_home.bin` (fitted to `kHomeCoverW`×`kHomeCoverH`,
  home carousel) and `cover_sleep.bin` (480x786, composed for the panel). **All** must be
  written by the converter — the device cannot generate any of them for a
  converted book, because there is no EPUB on the card to extract from. Each is
  optional at read time: every consumer falls back to a smaller one.
  `book.pos` is written beside them by the device when the reader closes.
- `<name>.epub` — a source, converted on demand into
  `.wintergreen/cache/<stem>/book.wgb` with covers alongside it.

A third: the "converted" marker in `MainMenu` looked for `cache/<stem>/book.wgb`,
and every converted book has the stem `book` — so they all probed the same
nonexistent path and every one displayed as *not* converted. The marker has since
been removed outright (see Configuration), taking the per-book `fopen` with it.

Two traps here, both hit during the switch to converted books. `cover_bin_path()`
derives from the file *stem*, and every converted book is literally named
`book.wgb`, so all of them mapped to `cache/book/cover.bin` and collided — it now
returns the WGB's sibling instead. And `ensure_cover_bin()` must return early for
an WGB: there is no EPUB to extract from, so whatever the converter shipped is all
there is.

A third, from the MRB → WGB rename: **that "is this a converted book" test is a
literal extension comparison**, and `CoverPaths.cpp`'s copy was still matching
`.mrb`. Every book therefore took the EPUB-cache branch and every cover path
resolved to `<data_dir>/cache/book/cover*.bin` — one nonexistent path shared by
the whole library. No cover appeared anywhere, on the home carousel or the sleep
screen, and nothing logged a thing: a missing cover file is a supported state
everywhere it is read. If covers vanish library-wide, check `is_wgb_` first.

`BookIndex` is a singleton holding the scanned book list plus reading stats,
backed by a `StringPool`. `MainMenu` rescans whenever the loaded index is
**empty**, not just when it fails to parse: a valid-but-empty index is
indistinguishable from "never scanned", and treating it as authoritative meant
books added to the card were never discovered — the empty index kept loading
successfully and nothing ever triggered a scan. It has a `generation()` counter — MainMenu polls it so
serial uploads/deletes refresh the visible list without navigating away.

### Fonts

Two fonts, both compiled from TTF by `tools/make_font.py`, and **no way to add a
third** — the serial `FONT` partition upload and `SDFN` (`/sdcard/fonts/`)
sideload paths were deliberately removed, along with the `'Y'` (clear SD fonts)
and `'F'` (invalidate font) serial commands.

- **Reader: Atkinson Hyperlegible**, 5 sizes (20/24/28/32/36 px), all four
  styles, built `--mono` (see below). Lives in the `font` flash partition,
  written by esptool from `resources/AtkinsonHyperlegible.bin` at upload time and
  mmapped at boot — it is not in the app image and is never copied or
  decompressed on the device. A font change takes effect on the next upload.

`ReaderSettings::kFontSizeNames` and `kNumFontSizePresets` **must** mirror the
`--sizes` list passed to `make_font.py`, in order. They had drifted to eight
entries reading 20/24/26/28/30/32/34/36 against a five-size bundle, so the quick
menu labelled the third preset "26" while rendering 28, and the fourth "28" while
rendering 32 — every size above the second was mislabelled. Indices 5–7 were
unreachable only because `QuickmenuScreen::on_select` clamps the cycle to
`font_set()->num_fonts()`; `Application::load_settings_` had no such clamp and
now checks `kNumFontSizePresets` rather than the array capacity `kMaxFontSizes`.

- **UI: Iosevka Slab Medium**, built `--mono`, one size per header in
  `display/ui_font_*.h` (14/24/32 px → small/large/header). There is no medium
  *size*; that header was unused and is gone. It was Bold until the weight was
  dropped — Bold at 14 px thresholds into near-solid blocks, and the mono
  hinting is what keeps Medium's thinner stems from breaking up. Dropping the
  gray planes roughly halved each header (small: 20,332 → 10,099 bytes).

WGF1 format is documented in `content/WgfFormat.h`, but three things that
matter are **not** in that header and were recovered by decoding the old shipped
Literata — `make_font.py`'s docstring is the real reference:

- `MbfGlyph::advance_width` is in **quarter-pixels**, not pixels.
- Antialiasing is **5** levels, not 4, encoded across the three planes as
  `(bw, msb, lsb)`: white `(1,0,0)`, light `(1,0,1)`, mid `(0,1,0)`, dark
  `(0,1,1)`, black `(0,0,0)`. The BW plane draws where its bit is **clear**; the
  gray planes draw where theirs is **set**. So `(msb,lsb)` is a lightening
  modifier, not a magnitude.
- The reader font asset is `[uint32 uncompressed size][zlib stream]` wrapping an
  WGFS v2 bundle: `[WGFS][num][ver=2][pad:2][name:32][num × uint32][WGF1...]`.

The gray planes are **optional** — `gray_lsb_offset` / `gray_msb_offset` of 0
mean absent, which is what `has_grayscale()` keys off. The reader font is now
built that way, via `make_font.py --mono`: `FT_LOAD_TARGET_MONO`, no gray planes.

That followed from the reader drawing only `GrayPlane::BW` (see "Removed"). The
BW plane is not a 50% threshold — `LEVEL_BITS` puts level 2 (37.5% coverage) in
the ink plane, so BW-only text was fattened by a ring of barely-covered edge
pixels, and `FT_LOAD_TARGET_LIGHT` left stems straddling pixel boundaries, so
that ring was uneven from glyph to glyph. Mono hinting snaps stems to the grid
instead: same total advance widths, but uniform stems and clean curves. It also
cut the asset from 296 KB to 122 KB compressed, since two of the three bitmap
pools are gone.

Don't "fix" the AA path to match — the 5-level encoding and its 37.5% threshold
are correct *for grayscale output*, where the lightening pass pulls those edge
pixels back. They only misbehave when the gray planes are thrown away.

`tools/check_font.py` parses a bundle or bare WGF1 exactly the way `BitmapFont.h`
does and ASCII-art renders a sample string. Run it after regenerating a font —
a structurally broken font renders blank on device with no error. Its header line
reports `gray=yes|no`, which is the quick check that `--mono` took effect.

The UI fonts are built `--mono` too, so no `ui_font_*.h` carries the two bitmap
pools nothing reads. Regenerating one is a single command per header, e.g.

```
nix-shell -p 'python3.withPackages(ps: [ps.freetype-py ps.fonttools])' --run '
  python3 tools/make_font.py header --mono \
    --regular …/SGr-IosevkaSlab-Medium.ttc:0 \
    --name ui_small --size 14 --symbol ui_small \
    --out wintergreen/display/ui_font_small.h'
```

The TTC face index matters: face **0** is Iosevka Slab, and the rest are the
Extended and Oblique cuts.

Regenerating (needs freetype-py + fontTools, absent from the PlatformIO
interpreter, hence nix-shell):

```
nix-shell -p 'python3.withPackages(ps: [ps.freetype-py ps.fonttools])' --run '
  python3 tools/make_font.py bundle --name AtkinsonHyperlegible --mono \
    --sizes 20,24,28,32,36 --line-height 150 \
    --regular …-Regular.ttf --bold …-Bold.ttf \
    --italic …-Italic.ttf --bold-italic …-BoldItalic.ttf \
    --out resources/AtkinsonHyperlegible.bin'
```

`--line-height` matters: the reader multiplies `y_advance` by the user's setting,
so a font's native leading decides how that setting feels. Atkinson's own metrics
are 1.25× the em where Literata's were 1.55×, which reads cramped; 150% restores
it. The extra leading is split above and below the text rather than all landing
under the baseline.

There is no longer an asset name to keep in sync: `generate_assets.py` reads
`resources/AtkinsonHyperlegible.bin` by path and writes it to the `font`
partition, and `FontManager` finds that partition by name (`"font"`, matching
`default_16MB.csv`). Renaming the resource means editing one line of Python.

Glyph coverage is per-font and sparse. A font that parses fine can still be
missing the glyphs for a given string, in which case it renders blank rather than
falling back — check `glyph_data(cp).bits != nullptr` before assuming a font can
render text. (This bit the old brand wordmark font: it contained only `n`, `o`,
`u`, `s`.) Both fonts cover the same 527 codepoints — ASCII, Latin-1 + Latin
Ext-A, General Punctuation/Currency — but Atkinson genuinely lacks 224 of them
(most of Latin Ext-A and General Punctuation). `make_font.py`'s `SUBSTITUTES`
table maps the ones that would actually hurt onto near-equivalents it does have
(U+2010 hyphen → ASCII `-`, and so on); the rest are blank. Extend that table
rather than accepting a blank glyph. There is currently **no wintergreen logotype font** — the wordmark is
drawn in the regular UI font. Dropping in a real cut means adding a brand font
header and using it in `ListMenuScreen::draw_header_`.
### Persistence

**Nothing on the card is a directory the firmware made.** State is two hidden
files at the card root plus one file per book:

| What | Where | Written |
|---|---|---|
| settings | `/sdcard/.wintergreen` | sleep, reader/quickmenu pop |
| book index | `/sdcard/.wintergreen-index` | reader close, library scan, serial mutations |
| reading position | `<book dir>/book.pos` | reader close |

There is no `.wintergreen/` folder any more, and `main.cpp` no longer `mkdir`s
anything at boot. Two consequences that were the point of the change:

- **The position file needs no key.** It used to be
  `.wintergreen/<8 hex digits>.pos`, the digits an FNV-1a hash of title + author
  + language, which gave a directory of opaque filenames, put two books one hash
  collision away from sharing a bookmark, and silently reset your place if a
  publisher edited the metadata. `book_pos_path()` (in `content/CoverPaths.cpp`,
  beside the cover paths) returns the book's own folder, so the bookmark travels
  with the book and is deleted with it. `make_book_key()` and `fnv1a_32` are gone.
- **Settings and index stay two files, deliberately.** They are written on
  completely different schedules — settings is a few dozen bytes on every sleep,
  the index is ~30 KB once a session — so folding them together would rewrite the
  index every time the font size changed.

The serial `'C'` command went with the directory. It cleared
`/sdcard/.wintergreen/cache/` — the on-device EPUB conversion cache, a feature
removed some time ago — and, when the directory was missing, **created it**. So
the one command still touching that path existed only to re-make the folder this
change deletes.

`Application::save_settings_` / `load_settings_` write a flat `key=value` file.
Three keys, and nothing that can be derived from elsewhere: `book_path`,
`font_size`, `rotate_reader`.

`book_path` is written **only while the reader is in the stack**, so its mere
presence means "resume this book on boot" — that is what a separate
`screen=reader|menu` key used to say, which is why that key is gone.

The book-list cursor is deliberately **not** persisted. `MainMenu::stop()` keeps
it in `initial_selection_` so navigating away and back within a session restores
it, but deep-sleep wake is a full boot, so after an auto-sleep the cursor starts
at row 0. That is nearly free because the list is already sorted
most-recently-opened-first — row 0 *is* the book you last read.

Removed and worth not re-adding: `v=1` (written, never parsed — the loader
ignores unknown keys, so evolving the format never needed a version tag),
`screen` (redundant, above), `last_version` (its What's New screen is long gone;
all it did was force one extra SD write per firmware update), and `open_counter`.

That last one was also a latent bug. It was a monotonic counter kept in
*settings* but consumed as `last_open_order` in the *index* — two files that
could drift. Losing or resetting settings reset the counter to 0, so newly opened
books sorted *below* older ones in the book list and on the home screen until it
climbed back. `BookIndex::mark_opened()` now derives the next order from
`max(last_open_order)` over the entries themselves, so the counter cannot
disagree with the data it orders. It is O(books) per book open, against an
operation that already reads and reparses the whole index.

`cache/` is created **on demand** by the EPUB conversion path, which exists only
in the host converter now — a library of converted books never produces it, so an
empty directory never appears on the card to puzzle over later.

The position file is four space-separated numbers: chapter, paragraph, offset,
text offset. Older files appended progress percent, time-left estimate and
chapter count, which were written on every save but **never read back**; three
more held reading statistics, which no longer exist at all.
the same values by `update_book_read_time()`.

It is written **only from `ReaderScreen::stop()`**. It used to be written after
every page turn, which is an SD write per page. Two things make the sparser write
safe, and both must hold: auto-sleep and the power button both route through
`Application::do_sleep_()`, which stops the screen; and `do_sleep_()` explicitly
stops the reader when a child screen (the quick menu) is on top, because
`screen_mgr_.top()->stop()` alone would miss it and silently lose the session.
It also skips the write when the position has not moved since it was loaded or
last written (`pos_written_`), so opening a book and backing straight out, or
paging forward and back to where you started, costs the card nothing. See "SD
card wear" for the other two files' guards.

**`rename()` on this platform does not replace an existing file.** ESP-IDF's
`vfs_fat_rename` calls FatFs `f_rename` directly with no unlink, and `f_rename`
returns `FR_EXIST` when the destination exists — unlike POSIX. Both save paths
therefore `remove()` the target immediately before renaming. Without that, only
the *first* save of a file ever succeeded: every later one failed silently at the
rename and left a stale `.tmp` behind, so settings, reading position and the book
index all stopped persisting after their initial write. If you see a `.tmp`
sitting next to its target on the card, that is the symptom.

The remove opens a brief window where neither file exists, so `load_settings_()`
falls back to reading `settings.tmp` when `settings` is missing — the `.tmp` is
always a complete file.

Both the settings file and the book index write to a `.tmp` and then `rename()`
over the target. **The `.tmp` is not a backup** — no copy of the old file is ever
made, and there are no `.bak.N` rotations (deliberately removed). It exists purely
to make the replace atomic. Don't reintroduce a direct `fopen(path, "w")` on
either file: `"w"` truncates the real file to zero bytes *before* writing a single
byte, and settings are saved on every auto-sleep — precisely when the battery is
most likely to give out, on a device with no low-battery cutoff. The rename
guarantees you observe either the complete old file or the complete new one.
Staging the content in RAM does not help: the truncate happens at `fopen`, and on
FAT32 the data, FAT-chain and directory-entry updates are separate writes anyway.

`save_settings_()` composes the file into a `std::string` and compares it against
`settings_written_`, the bytes of the last successful write, skipping the SD write
when nothing changed. That matters because it is called on every sleep, every book
open and every reader pop, and usually has identical content. `settings_written_`
starts empty, so the first save after boot always writes — which conveniently
normalises a settings file left over from an older firmware with removed keys.

## Conventions

- Two-space indent, `snake_case_` trailing underscore on private members,
  `k`-prefixed constants, `Deg90` = portrait.
- Screens are members of `Application`, not heap-allocated; `free_items_storage()`
  in `stop()` releases list RAM while keeping cursor position.
- Hyphenation ships **English patterns only** (`content/hyphenation/Liang/`); the
  other language tries were deleted and `HyphenationLang` has just `None` and
  `English`. Unknown language tags fall back to no hyphenation.
- The README's attribution to upstream Nous/Microreader is intentional — keep it.

## Planned — investigated, not yet done

Everything that was on this list has shipped and been confirmed working on the
device: dynamic frequency scaling, de-embedding the font and sleep image, the
sort collapse, lazy serial startup, async bulk SPI, next-page pre-layout *and*
pre-draw, the quick-menu frame snapshot, the resident book index, the deferred
ghost-clearing full refresh, panel temperature compensation, SD write dedup, the
state-file flattening (no `.wintergreen/` directory), the WGB/WGF rename with
magic-as-version, pre-rasterised images (and the removal of every image decoder
from the firmware), parallel conversion, the low-battery cutoff, and the idle
panel-rails power-down.

Two things remain, both needing a device and a measurement rather than an opinion:

1. **Measure `-Os` against `-O2` with a stopwatch.** `-Os` is 30% smaller code
   (see "Measured size budget") and the theory that a smaller hot path wins back
   more from the 16 KB I-cache than it loses is untested. Only a device settles it.
2. **Drop the unused OTA half of the flash.** `app1` (6.4 MB) and `otadata` are
   reserved for an OTA path that does not exist. **Deliberately not done** — it
   changes the partition table under an already-flashed device and needs a full
   erase, so it is the owner's call.

### Smaller, still open

- **Pre-draw backwards.** `prerender_next_page_()` only runs after a *forward*
  turn. The symmetric version is **not** the same win, and the reason is worth
  writing down before someone tries again: `prev_page_()` already runs
  `layout_backward()` itself and caches the result, so a backward pre-draw would
  save only the *glyph blit*, not the layout — and to save the layout too it
  would need a second, backward-keyed page cache on top of a second spare
  framebuffer, since the one spare is already shared between the quick-menu
  snapshot and the forward pre-draw. That is 48 KB of BSS and a third
  invalidation path for the rarer direction.
- **Preload the neighbouring carousel cover.** `HomeScreen::load_cover_` reads
  the selected book's cover off the card inside `draw_all_`. Preloading the
  next one during the waveform would shave the SD read and scale — tens of
  milliseconds off an operation whose floor is a ~300 ms waveform — at the cost
  of another ~25 KB buffer and a cache to invalidate. Measure before building it.
- **Pre-draw skips image pages.** `draw_image_()` must drain the waveform before
  touching the card, so drawing an image page speculatively would block the UI
  loop for the couple of hundred milliseconds the current page is still painting.
  A second spare buffer, or an SD read that does not need the panel idle, would
  lift the restriction.
- **`__d_vfprintf` / `__d_vfscanf` are ~5 KB** of double-capable formatting,
  pulled in by the tree's `snprintf`/`sscanf`/`fprintf`. `CONFIG_NEWLIB_NANO_FORMAT`
  no longer exists under picolibc, so the only route is hand-rolled integer
  formatting at every call site — and IDF's own panic path may keep them anyway.
- **`f_mkfs` is 2,360 bytes** and unreachable (`format_if_mount_failed = false`),
  but `esp_vfs_fat_sdspi_mount` references it unconditionally, so dropping it
  means patching IDF — exactly what `patch_ffconf.py` was deleted for.
- **The UI fonts and hyphenation trie are 81 KB of rodata** and could move to a
  flash partition like the reader font did. mmapped flash and rodata are both XIP
  through the same cache, so it would be free at runtime — but the app image is
  not under pressure, so it has not been done.
- **`settings` is written on book open** purely so a reboot resumes that book.
  Dropping it would remove one SD write per session at the cost of booting to the
  home screen after a power loss — and `.pos` is only written at close anyway, so
  the resume would land on the previous position regardless.

### Investigated and rejected

- **Precomputing hyphenation into the WGB.** The obvious "do it on the host
  instead" candidate, and it does not pay: `find_hyphen_break` is called only when
  a word overflows a line end — roughly once per *line*, not per word, so about 30
  Liang trie walks per page. That is microseconds. Baking break points into the
  format would save the 27 KB trie and nothing measurable in time.
- **Collapsing `IDisplay` into `EInkDisplay`.** Only a handful of virtual calls
  per page turn, each followed by tens of milliseconds of SPI and hundreds of
  waveform. Nanoseconds against that, and it would force `DrawBuffer.h` to include
  `epd.h` when it still has to compile on the host for `tools/epub2wgb`.
- **Sampling buttons every 10 ms instead of 5 ms.** Would halve the 200 Hz timer
  wakeups, but the CPU is *never* asleep between them — there is no light sleep —
  so it saves a fraction of a milliamp out of 15, while adding up to 10 ms to
  every press on a device whose headline feature is responsiveness.
- **Compressing `sleep.mgr`** (96,008 -> 4,682 with zlib). Moot: the image left
  the app image entirely for its own flash partition, so it costs no image bytes,
  and uncompressed means the sleep path is a straight mmap with no inflate.
- **A faster page-turn waveform.** There isn't one. Page turns already use
  `EPD_FAST_REFRESH`, the shortest waveform the controller has; `EPD_HALF_REFRESH`
  and `EPD_FULL_REFRESH` are both slower and exist for ghost clearing. The
  waveform is the floor on page-turn time, and every CPU-side optimization here is
  shaving the tens-of-milliseconds prefix off a several-hundred-millisecond
  operation.

### Closed — don't re-investigate

Instruction cache size (fixed in C3 silicon, not configurable as it is on the
S3), QIO flash mode, SD clock above 20 MHz, tickless idle, region-only refresh
for menu cursor movement, and `CONFIG_NEWLIB_NANO_FORMAT` (the option no longer
exists under picolibc). See "Build & flash", "Idle power" and "Refresh latency".

Also settled: three `sdkconfig.defaults` keys that IDF silently ignored because
`espressif32 @ ^7.0.1` renamed or removed them — `CONFIG_NEWLIB_NANO_FORMAT`,
`CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH` (`CONFIG_FREERTOS_IN_IRAM` already
defaults off, same outcome), `CONFIG_FREERTOS_DEBUG_OCDAWARE` and
`CONFIG_SPI_MASTER_ISR_IN_IRAM`. They are gone from the file, with a comment
explaining why, along with `CONFIG_FREERTOS_ISR_STACKSIZE=1024`, which the
generated config clamped straight back to 1536. **An unknown key in
`sdkconfig.defaults` is ignored, not an error** — after any IDF bump, diff
`sdkconfig.defaults` against the generated `sdkconfig.esp32c3` to check every
line still takes effect.

`CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=n` and `CONFIG_VFS_SUPPORT_SELECT=n` are now
set: neither was reachable, and they were costing `esp_mprot_set_prot` (~2 KB)
plus PMS programming on every boot, and the VFS select shim respectively.

### Battery health

Distinct from battery *life*. The discharge side is covered by the low-battery
cutoff, live at 3200 mV — see that section. Deep-discharging a Li-ion cell below
~3.0 V costs it permanent capacity, and deep sleep keeps drawing ~10 µA
afterwards. The cell almost certainly has a hardware protection IC, but that cuts
off around 2.5 V, well past where the damage starts, which is why the firmware
does not rely on it.

Charge-side health (not holding at 4.2 V, thermal limits) is handled by the
charger IC and is **not** reachable from firmware.

## Removed (don't resurrect without asking)

Settings menu, theme picker, Stats / GlobalStats / WhatsNew / Alert / ConvertAll
screens, the bouncing-ball and grayscale demos, BMP sleep-image conversion,
sleep-image cycling, SD-card font selection, and the on-device System tab
(clear cache, rebuild index, rebuild SPIFFS, OTA partition switch).

**`RecentBooksScreen` and `HiddenBooksMenu`**, with `ScreenId::RecentBooks` and
`ScreenId::HiddenBooks`. Recents became the home carousel and hidden books became
a `MainMenu` mode — see Architecture for both. Remember that both
`platforms/*/CMakeLists.txt` list core `.cpp` files **explicitly**, so deleting a
screen means editing them too.

**Antialiased (grayscale) text in the reader.** `ReaderScreen::apply_grayscale_`,
the `grayscale_pending_` / `grayscale_active_` state and the `revert_grayscale`
calls are gone; a page turn is now a single `buf.refresh()` and `render_text_`
only ever draws `GrayPlane::BW`.

It was removed because it was **visible as motion**. `kLutGrayscale`'s four
waveforms are indexed by (MSB, LSB), and white and black both land on LUT0 =
*do nothing* — the gray levels are only reachable by first driving the pixel to
black and then partially pulling it back. So every page turn was three physical
panel updates: revert-to-BW, a BW-only refresh in which the font's *mid* and
*dark* AA levels appear **solid black** (the BW plane draws wherever its bit is
clear), and finally the grayscale pass lightening exactly those edge pixels.
The middle frame read as bolder text that then visibly thinned — text appearing
to move after the page had already changed. That intermediate is inherent to the
LUT, not a sequencing bug, so no amount of reordering fixes it.

Text is consequently a hard threshold at the 50% AA level (mid/dark/black are
ink, light is paper) and reads slightly heavier than the old settled state. The
alternative, if the weight ever becomes a problem, is the one-pass
`kLutFactoryQuality` LUT already used for sleep images: it drives each pixel to
an absolute level with no prior-state dependency, so a page could land in final
form in a single update — at the cost of squashing the font's 5 AA levels into
the panel's 4 and composing the two RAM planes by hand. Note its RAM polarity is
**inverted** relative to normal drawing (state `(RED<<1|BW)` = 00 is white, per
`make_sleep_image.py` and the enum comment above the table; the per-row comments
inside `kLutFactoryQuality` say the opposite and are wrong).

**The multi-pass grayscale machinery is now gone too**, not merely unreachable.
`kLutGrayscale`, `kLutGrayscaleRevert`, `EInkDisplay::grayscale_refresh`,
`revert_grayscale`, `grayscale_revert_`, `set_grayscale_lut` /
`set_grayscale_revert_lut` / their `clear_*` pairs, the two 112-byte
`custom_grayscale_*_lut_` buffers, `in_grayscale_mode_` and
`DrawBuffer::show_grayscale_image` have all been deleted, along with the
`0xDEADBEEF` serial LUT-upload frame that was the only reason to keep them
tunable. `IDisplay` lost `grayscale_refresh`, `revert_grayscale`,
`in_grayscale_mode` and `set_rotation` with them, and `partial_refresh` now takes
one argument — its `prev_pixels` parameter existed solely for the grayscale
revert path.

The one-pass path (`grayscale_refresh_1pass` + `kLutFactoryQuality`) is still
live: it is what draws the sleep image, and it remains the escape hatch if reader
text ever needs to be lighter than the current hard threshold.

**Hyperlinks, entirely.** `LinksScreen`, the quick menu's "Links (n)" item, the
reader's nav-history stack (the back/stay gesture that returned from a followed
link), `Run::href`, `LayoutWord::href`, the `<a href>` tracking in `EpubParser`,
the href write path in `WgbWriter`, and the link-underline rendering in
`DrawBuffer`'s line renderer (now `draw_layout_line`). An `<a>` element now contributes only its text.

The last remnant went with the `WGB2` bump: `WgbReader` used to read an href
length and **skip** those bytes when a run's flag bit `0x02` was set, as a
compatibility shim for caches written while links still existed. No file the
current writer produces can set that bit, and the magic change makes older ones
unreadable anyway, so the branch is gone too. Bit `0x02` is now free.

**Every page-geometry setting except font size.** H-Margin, V-Margin, Alignment,
Line spacing and Publisher Sizes are gone — along with `AlignOverride`,
`SpacingOverride`, their preset/name tables, the persisted `align_override`,
`padding_h`, `padding_v`, `spacing_override` and `override_pub_fonts` keys, and
the matching `align_override` / `override_publisher_fonts` /
`line_height_multiplier_percent` fields on `LayoutOptions` and `PageOptions`.
The layout engine now always takes alignment and line height from the book's CSS
(`para.alignment`, `para.text.line_height_pct`) and always honours publisher font
sizes. Where the book specifies no alignment, body text is **justified** — the
`value_or` default in `layout_para_lines`. `para.alignment` is a genuine optional
all the way through: `EpubParser` only fills it when CSS said something, and WGB
round-trips the empty state through the `kWgbAlignDefault` (`0xFF`) sentinel, so
changing that default does not require re-converting cached books. `ReaderScreen::make_page_opts()` is fully determined by the page size, and
`ReaderSettings::progress_bottom()` was renamed `bottom_margin()` now that it has
nothing to do with a progress indicator. `ReaderScreen`'s `kPaddingRight` and
`kPaddingBottom` went with them — note that dead `static constexpr` class members
produce no compiler warning, so they have to be found by grep, not by build.

**The hyphenation toggle** (the feature stays, always on) and, with it, the last
caller of `ListMenuScreen::add_separator()`. That helper and its parallel
`std::vector<bool> separators_` are gone; the base `is_separator()` now just
returns false. `MainMenu` is unaffected — it has its **own** `separators_` member
(`vector<pair<int,string>>`) and overrides `is_separator()` to mark the hairline
divider it derives from its entry list, which also means the base member had been
shadowed all along. `QuickmenuScreen`'s separator-drawing branches went too,
since that screen can no longer contain one.

**`ChapterSelectScreen`**, merged into the quick menu (see Architecture). Note
both `platforms/*/CMakeLists.txt` list core `.cpp` files **explicitly** — deleting
a screen without editing both fails the build at CMake generate time, not compile
time, with a "Cannot find source file" that names the wrong call site.

**On-device EPUB conversion, and with it EPUB support entirely.** A book is a
`.wgb`; `BookIndex` does not recognise `.epub` at all, so an unconverted book is
not listed rather than listed-and-broken. `ReaderScreen` opens the WGB or shows
an error — there is no fallback and no "Converting…" progress screen. The
`Book`, `EpubParser`, `CssParser`, `XmlReader`, `HtmlEntities` and `WgbConverter`
translation units are **out of the firmware build** (they remain in the repo for
`tools/epub2wgb`), as are the `X`/`I`/`D` serial benchmark commands that were
their last callers. `Application::ensure_cover_bin()` went too: with no EPUB
there is nothing to extract from. Together that is ~95 KB of flash.

`ZipReader` **stays** in the firmware despite EPUBs being gone — it is how
embedded images are read, as stored entries pointing into the WGB. `WgbWriter`
does **not**: its only caller is `WgbConverter`, so it was dropped from
`platforms/esp32/CMakeLists.txt` and is now host-only alongside it.

Because those TUs are host-only, every `#ifdef ESP_PLATFORM` inside them was
unreachable, and they have been flattened to the host branch. Gone with them:
`WgbConverter`'s three `benchmark_*` functions (~250 lines, no callers since the
`X`/`I`/`D` commands went), all the `esp_timer` sub-stage instrumentation in
`convert_epub_to_wgb_streaming` and `parse_xhtml_events` / `parse_xhtml_body`, the
`heap_caps_get_largest_free_block` reserve-capping and capacity-triggered
`flush_run()` in `EpubParser`'s run accumulator, and `CssCache::low_memory()`
(which was `esp_get_free_heap_size() < 24 KB` on device and a constant `false`
off it, so eviction is now driven by `over_budget` alone). The never-defined
`WINTERGREEN_DIAG_STREAMING` blocks went too. None of this changes converter
output — the host build never compiled any of it.

**Font sideloading**, both paths: the serial `FONT` upload (raw write to the
spiffs partition) and `SDFN` uploads to `/sdcard/fonts/`, plus the `'Y'` and
`'F'` serial commands and `Application::invalidate_font()`. The two compiled-in
fonts are the only fonts the device can have.

**exFAT**, along with `tools/patch_ffconf.py` which enabled it. SD cards are
FAT32, 16 GB or smaller — that is the supported configuration, and an exFAT card
will simply fail to mount. exFAT cost 7,892 bytes of flash and required patching
`ffconf.h` inside the ESP-IDF package on every build; ESP-IDF ships it disabled
because it is patent-encumbered, so there is no Kconfig option for it.

**All reading statistics.** Read time, times opened, page turns and estimated
time left are gone from the reader, the `.pos` file, `BookIndexEntry`, the index
file format, and both places they were displayed (the book list's right-hand
column and the quick menu's "2h 15m read" line). With them went
`Application::update_book_read_time()`, `BookIndex::update_reading_stats()`, the
reading-time accumulator (`tick_activity_`, `kActivityWindowMs`,
`last_activity_ms_`) and `ListMenuScreen::subtitle3_`.

`.pos` is now four numbers: chapter, paragraph, offset, text offset. The index
line is `path|title|author|last_open_order|progress_pct`. Both readers tolerate
the old shorter and longer forms — a missing field reads as 0 and extra ones are
not consumed, so `INDEX_FORMAT_VERSION` did not need bumping.

`progress_pct` is the one exception to "no statistics in the index", and it is
there for cost, not sentiment: the book list shows a percentage per row, and
deriving it live would mean opening every WGB *and* every `.pos` on the card
every time the list is built. `ReaderScreen::stop()` hands it to
`Application::record_book_progress()`, which reloads the index first —
`MainMenu::pause()` (the default, = `stop()`) clears the in-memory entries
whenever the reader is pushed, so saving without reloading would truncate the
file, and it re-clears afterwards to leave that state as it found it.

What stayed: `progress_pct()` and the `total_chars` / `char_before_para()`
machinery behind it. That is computed live from the reading position, not
accumulated over time, so it is not a statistic in the sense meant here.
`last_open_order` also stays — it drives the most-recently-read sort.

**The reading progress indicator**, entirely — bar, percentage, `ProgressStyle`,
`ProgressScope`, their quick-menu entries and their persisted settings keys.
`ReaderScreen::draw_bottom_` is gone; nothing is drawn in the reader's bottom
margin now. `progress_pct()` / `chapter_progress_pct()` **stay** — they feed the
book-details card and reading stats, not the page.

The embedded wintergreen sleep image is intentionally **kept**:
`do_sleep_(buf, wordmark_image = true)` shows it instead of the book cover, ready
for a power-button long-press gesture that is not wired up yet.


**Debug and benchmark surface, entirely** (2026-08-19). Every log statement and
`wintergreen/HeapLog.h` itself (see "No logging"); the serial `'S'` heap query,
`'Q'` state dump, `'G'` flash benchmark and `'P'` render benchmark;
`ReaderScreen::bench_render` and `FontPartition::bench_flash`;
`Application::top_screen_name()` and the `g_top_screen_name` copy that ran every
frame; the `esp_timer` scaffolding that fed the removed timing logs in
`render_page_`, `PngDecoder`, `JpegDecoder` and `provision_embedded`; and the
`esp_log_level_set()` calls that muted logging during binary transfers.

**Dead `IRuntime` and `DrawBuffer` surface.** `IRuntime::step_mode()`,
`consume_step()`, `yield()` and `should_continue()` were never overridden to
anything but a constant, so the step-mode branch in `Loop.cpp` and the
`runtime.should_continue()` test in `main.cpp` were dead; all four are gone.
`Loop.cpp::run_loop()` went with the desktop build. On `DrawBuffer`:
`write_ram_bw()`/`write_ram_red()` (the no-arg wrappers — the sleep path calls
`display_.` directly), `grayscale_refresh()`, `revert_grayscale()`,
`draw_circle()`, `draw_text_centered()` and `set_rotation_transform()` (which had
become identical to `set_rotation` once driver-side rotation was removed).

**`BookIndex::is_book_path`.** It was byte-identical to `is_wgb_path` — its doc
comment still claimed `.epub` — and every caller now uses `is_wgb_path`.

**`build_index`'s first pass.** It walked the whole card recursively once just to
count books into a `total` that only a compiled-out log ever read, then walked it
again to index them. One tree walk now. Its `iterate_books` helper is templated on
the callback instead of taking `std::function`, which was costing a heap
allocation and an indirect call per book.

**Font provisioning and the "Installing fonts..." progress screen** (2026-08-19).
The font is flashed straight into its own partition now — see "Assets are flashed,
not embedded" for the full list of what went with it, which includes the entire
asset-blob mechanism, five unused `FontPartition` methods, `FontManager::ensure_ready`,
`DrawBuffer::show_loading` / `render_loading_box_` / `mini_target_` and the
`kLoad*`/`kBar*` geometry, and `IDisplay::partial_refresh_region`.

Two smaller things went at the same time: `FontManager::any_corrupt()` and
`Application::font_warning_shown_`, which after their log line was deleted did
nothing but scan eight fonts once per frame and set a flag nobody read; and
`FontManager::load_bundle()`, which lost its last caller with the desktop build.

**The 3-second post-flash boot delay.** `app_main` used to `vTaskDelay(3000)` on
`ESP_RST_SW` so a serial monitor had time to attach. There is no serial monitor
output any more, so every flash-and-run is three seconds shorter.

**Layout instrumentation.** `g_layout_hyph_us`, `g_layout_metrics_us`,
`g_layout_para_us`, `g_layout_cache_misses` and the `MR_TRACE` / `MR_LAYOUT_TRACE`
facility. The first two bracketed **every `word_width()` call** with
`esp_timer_get_time()` — two hardware timer reads per word, thousands per page —
and all of them became write-only when the log that printed them was deleted.

**The scratch-buffer loan.** `DrawBuffer::scratch_buf1()`/`scratch_buf2()`/
`reset_after_scratch()` handed both framebuffers out to on-device EPUB conversion
and image decoding. Both are gone, so nothing borrowed them any more — and the
three surviving `reset_after_scratch()` calls were actively harmful, blanking both
buffers and marking the displayed frame stale after a serial index op that never
touched them. `BookIndex::build_index()` and `index_file()` lost the `DrawBuffer&`
they only ever `(void)`-cast, and `DrawBuffer::active_valid_` went with them: with
nothing invalidating it, it was always true.

### Sleep images (MGR2)

`resources/sleep.mgr`, flashed verbatim into the `sleep` partition and read
straight out of the mapping by `DrawBuffer::Mgr2Source_` — there is no longer a
file-backed variant, so fetching a row is a pointer add. A deliberately trivial
format:

```
"MGR2"   4 bytes
width    uint16 LE      height   uint16 LE
pixels   2 bits each, row-major, stride = ceil(width / 4), MSB-first:
         value = (row[x / 4] >> (6 - (x % 4) * 2)) & 3
         0 = white, 1 = light, 2 = dark, 3 = black
```

Only 4 levels exist because the panel shows it with a one-pass grayscale refresh
that takes bit 0 of each value into BW RAM and bit 1 into RED RAM. The image is
blitted at native size into physical panel coordinates with **no scaling and no
centring**, so it must be exactly 800x480 (landscape).

Make one with `tools/make_sleep_image.py` (needs Pillow):

```
nix-shell -p 'python3.withPackages(ps: [ps.pillow])' --run \
  'python3 tools/make_sleep_image.py cover.png resources/sleep.mgr'
```

It fits and letterboxes onto an 800x480 white field by default. `--dither` gives
Floyd-Steinberg for photographs; plain 4-level quantisation (the default) is
better for line art, logos and text.
