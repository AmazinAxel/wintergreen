# Wintergreen

Firmware for the Xteink X4 e-reader (ESP32-C3, 16 MB flash, e-ink panel). A fork of
Nous, itself a fork of Microreader. Design goal: a reader with no configuration
surface — everything fixed at compile time so the device boots straight into a book.

Entries below are mostly *negative* knowledge: things tried on hardware that failed.
Where one says "don't", it cost a debugging session to learn. Re-opening any of them
needs a device, not an argument.

## Build & flash

```
pio run                            # build (only env: esp32c3)
pio run -t upload                  # build + flash over USB
pio device monitor                 # serial log, 115200
```

Board enumerates as `/dev/ttyACM0` (VID:PID `303A:1001`, native USB JTAG/serial).
`upload_port` is deliberately unset so PlatformIO auto-detects.

**The desktop build is gone.** The only host build is `tools/epub2wgb`. Both it and
`platforms/esp32/CMakeLists.txt` list core sources **explicitly** — adding or deleting
a core `.cpp` means editing both, or CMake fails at generate time with an error naming
`idf_component_register` rather than anything you touched.

`ESP_PLATFORM` is ESP-IDF's own macro and is the seam between firmware and the host
converter. It stays.

Flash is **DIO at 80 MHz** (`sdkconfig.defaults`). QIO **bricks the boot** — the
bootloader's quad-mode self-upgrade never completes. DIO@80 gives 20 MB/s, same as
QIO@40. `board_build.flash_mode` in `platformio.ini` is inert under espidf; only the
sdkconfig option matters.

SD clock (`platforms/esp32/sdcard.h`) divides the 80 MHz APB, so only 80/40/26.7/20
exist. **40 MHz fails** — SD reads need the MISO round trip and a 12.5 ns half-period
is under the 14 ns output delay the spec grants the card (the panel runs 40 MHz fine
because it is write-only). **26.67 also fails** (book list comes up empty). 20 is the
ceiling. The card's `input_delay` is not reachable through `sdspi_device_config_t`, so
using it would mean patching IDF in place.

Build-system gotchas:

- If a build ignores an `sdkconfig.defaults` change, delete the generated
  `sdkconfig.esp32c3` — IDF keeps existing values and does not re-apply defaults. Not
  committed.
- `memory.ld` / `sections.ld` under `.pio/build/<env>/` are also generated and not
  always regenerated. On a linker-script-shaped error (`region 'rtc_reserved_seg'
  overflowed`), `rm -rf .pio/build/<env>` before believing it.
- `CONFIG_FATFS_CODEPAGE_437`: the dynamic codepage links every OEM table, four CJK
  ones included, for 478 KB. Long filenames use UTF-8 LFN regardless.
- UI font arrays in `display/ui_font_*.h` are `inline constexpr`, not `static` — at
  namespace scope in a header `static` gives each TU its own copy (the small font
  linked 4×, wasting 96 KB).
- Toolchain is a Nix machine with no system `python3` or `perl` on PATH; build scripts
  run under PlatformIO's bundled interpreter.

### Idle power

Deep sleep ~10 µA, CPU awake ~15 mA at 80 MHz / ~22 mA at 160 MHz, an e-ink refresh a
sub-second burst. **One second awake costs more than half an hour asleep.** Battery
life is almost entirely "what fraction of the time is the CPU awake, and at what
clock"; flash mode, SD clock and panel tuning move the third decimal place.

**Automatic light sleep is not usable here — tried and removed.** Specifically
*tickless idle*, not the whole PM subsystem. Don't reintroduce
`CONFIG_FREERTOS_USE_TICKLESS_IDLE`:

- It force-enables `PM_SLP_DISABLE_GPIO` via `ESP_SLEEP_GPIO_RESET_WORKAROUND` using a
  Kconfig `select`, which **cannot** be overridden from `sdkconfig.defaults`.
- The buttons are ADC resistor ladders on GPIO1/GPIO2 (`input.h`).
  `gpio_sleep_sel_dis()` does **not** rescue them: those pads sit in analog mode, so
  what light sleep destroys is SAR ADC state, not pin configuration.
- Symptom: buttons work for about a second after boot, then stop until USB is
  attached. USB suppresses light sleep, so the device looks healthy on the bench and
  is unusable on battery.
- It only engages when button sampling is >= 30 ms, at odds with responsive input.

Battery life comes from reaching **deep** sleep; the lever is `kAutoSleepMinutes`,
traded against deep-sleep wake being a full boot. Because wake **is** a full boot,
everything on the boot path runs on every wake — hence the "Boot speed" block in
`sdkconfig.defaults`: `BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` (cold boot still
validates), `BOOTLOADER_LOG_LEVEL_NONE` (~78 ms of UART per boot), and
`RTC_CLK_CAL_CYCLES=256` (precision only matters for *timer* wake; this device wakes
on GPIO).

**The CPU never sleeps between frames.** `wait_next_frame()` `vTaskDelay`s to a 25 ms
cadence, so the core wakes 40×/s at ~15 mA. With no light sleep available the only
lever is reaching deep sleep sooner. A longer frame period saves a fraction of a
milliamp and costs input latency — measured and rejected.

### Dynamic frequency scaling (live)

`CONFIG_PM_ENABLE=y`, `max_freq_mhz = 160`, `min_freq_mhz = 80`, `light_sleep_enable =
false`, configured in `Esp32Runtime::init_pm_()`. The core idles at 80 MHz, where
nearly a whole reading session is spent.

Three invariants; breaking any one undoes it or worse:

- **`light_sleep_enable` must stay false** — the tickless-idle errata above.
  `PM_ENABLE` alone does *not* select `PM_SLP_DISABLE_GPIO`, so the ADC buttons are
  safe.
- **`min_freq_mhz` is 80, not 40.** APB follows the CPU clock on the C3 and SD-over-SPI
  timing has no margin left at 20 MHz as it is.
- **Layout and drawing take no PM lock of their own.** Pure CPU, so without an explicit
  lock a page turn would run at the idle clock — up to 2× slower, a net regression.
  `IRuntime::set_performance_hold` wraps an `ESP_PM_CPU_FREQ_MAX` lock;
  `Application::update` raises it on button activity or a pending screen transition and
  drops it after `kPerfHoldMs` (750 ms) of quiet. Acquire/release are skipped when
  unchanged.

### Input and holds

Side-rocker paging is **orientation-dependent**; the front row is not. The rocker is
one physical control whose ends swap places when the device is turned, so
`ReaderScreen::update()` picks the mapping off `landscape_hold`:

|            | Up (rocker top) | Down (rocker bottom) |
|------------|-----------------|----------------------|
| Portrait   | next            | previous             |
| Landscape  | previous        | next                 |

Front buttons are fixed to the panel: Button2 next, Button3 previous, both
orientations.

Portrait is `Deg90` **or** `Deg270`, landscape `Deg0` **or** `Deg180` — the reversed
orientations are the same physical hold, so a check written as `rotation() == Deg0`
leaves flipped-landscape behaving like portrait. That bug was present in the reader's
layout padding and is fixed. The reversed orientations are no longer reachable
(`rotate_reader` is 0 or 1, and both setter and loader clamp), but the `Rotation` enum
keeps all four values for the panel driver, so the hazard applies to any new check.

5 ms button sampling (`input.h`) under a 25 ms UI frame (`main.cpp`), so a press lands
on the next frame. Screens repaint only on state changes, so frame rate does not drive
panel traffic.

**Never measure a hold in frames.** Frame duration swings with how much a screen
rendered and whether the panel was mid-refresh. Use `runtime.frame_time_ms()` (the
*measured* last frame) against milliseconds. Both frame-counted holds in the tree were
wrong the moment `partial_refresh()` stopped blocking and frames went from hundreds of
ms to ~25:

- the reader's and list's page/row repeat fired once **per frame** with no delay, so a
  tap registered as two presses. They now use `HoldRepeat` (`Input.h`), driven by
  `config::kHoldDelayMs` / `kHoldRepeatMs` / `kHoldAccelStep`.
- the hidden-books long-press was `15` frames, commented "≈3s at typical e-ink frame
  rate" — i.e. calibrated to refresh-blocked frames. It became ~0.4 s. It is
  milliseconds now, and on `HomeScreen` it is the tree's only hold gesture outside
  `Application`'s power button.

`HoldRepeat::tick()` may return more than one repeat when a frame ran long, capped at
`kMaxPerTick` — cashing in a multi-second stall as a burst reads as a freeze then a
jump.

The menu's acceleration step is read into a local before its inner loop. Written as
`for (i = 0; i < ++hold_reps_up_; ++i)` the increment sits in the loop *condition* and
is re-evaluated every pass, so the bound outruns `i` and the loop never terminates —
that shipped once and hung the device on any button hold. The step is also capped at
the list length.

Beware `pdMS_TO_TICKS(1)`: at `FREERTOS_HZ=100` it is **0 ticks**, and `vTaskDelay(0)`
only yields, so any wait loop using it spins the CPU flat out. `epd.h`'s
`waitWhileBusy` had this bug and now uses a literal `vTaskDelay(1)`.

### The power button

GPIO3, the only non-ADC key, handled entirely in `Application::update`, not by any
screen. Held for `kPowerHoldSleepMs` it sleeps; released sooner it is forwarded to the
top screen as a synthetic `Button1` (Confirm), so "select" and "open the quick menu"
come for free. Three load-bearing details:

- It arms only on a **rising edge**. The press that wakes the device is cleared by
  `input.clear_button(Button::Power)` in `main.cpp`, so a wake hold can never read as
  hold-to-sleep or select.
- The tap necessarily resolves on **release** — the only way to tell it from a hold.
- `main.cpp` waits for GPIO3 HIGH before `esp_deep_sleep_start()`. Wake is
  level-triggered on LOW and hold-to-sleep leaves the button down, so without that wait
  the device wakes instantly and (being a non-`ESP_RST_POWERON` reset) skips the hold
  gate in `verify_wakeup_press()` — an on/off loop.

**`Application::update` reads its `buttons` parameter directly — there is no `buttons_`
member, deliberately.** The member existed only to be copied from the parameter and
read a few lines later. When an edit removed that copy, `buttons_` stayed
default-constructed forever: every button died, power included, and the firmware built
and booted perfectly showing the home screen. Deleting the member makes that state
unrepresentable.

Auto-sleep never fires while USB is connected — `main.cpp` calls `keep_awake()` every
iteration when `usb_serial_jtag_is_connected()`. The timeout can only be tested on
battery. `Esp32Runtime::frame_time_ms()` returns the **measured** duration of the last
frame, not the nominal target; `Application` accumulates it into the auto-sleep
countdown, so returning the constant made that clock drift whenever a frame overran.

## Refresh latency

Almost all of a page turn or menu keystroke is the e-ink waveform, not the CPU. Layout
and draw are tens of ms; the waveform is hundreds.

`partial_refresh()` passes `false`, firing `CMD_MASTER_ACTIVATION` and returning while
the panel is still updating, so the app lays out and draws the *next* frame during the
waveform. Safe only because **every** `EInkDisplay` entry point opens with
`wakeIfNeeded(); waitWhileBusy();`. Do not add a panel operation without it.

- Anything that sends SPI right after firing must keep `wait = true`, or the command
  lands mid-waveform and corrupts the update: `full_refresh`, the one-pass grayscale
  path, and anything with `turnOffScreen` (the screen powers down at the *end* of the
  waveform). `deep_sleep()` therefore starts with its own `waitWhileBusy()` — mid-update
  it leaves a half-drawn panel on screen for the entire sleep.
- **The SD card shares SPI2 with the panel** and concurrent traffic corrupts an update
  in flight (why `main.cpp` defers index ops on `epd.is_busy()`). Any new SD access must
  call `DrawBuffer::wait_panel_idle()` first. Existing ones: `draw_image_`,
  `ReaderScreen::stop()`'s `.pos` write, `do_sleep_()`, the settings save on reader pop,
  every sync write. Text-only pages never touch the card and keep the full overlap.

**Region refresh cannot update "just the changed rows" — tried and reverted.**
`setRamArea()` bounds where a RAM *write* lands, not the refresh. `refreshDisplay()`
drives the **whole panel** from BW RAM; a partial update merely *looks* localized
because unchanged pixels have BW RAM == RED RAM and get a no-op waveform. Writing only
a sub-rectangle leaves the rest of BW RAM holding the last *fully written* frame, and
the panel redraws that stale frame everywhere outside the window. Symptom when
`ListMenuScreen` tried it for cursor movement: previously-selected rows stayed lit and
the reader page showed through behind the menu. `partial_refresh_region()` remains
correct for its **one** caller, `show_loading()`, where the rest of the screen is
static *and* BW RAM already matches it.

**`ReaderScreen::page_cache_`** holds one laid-out page, so `prev_page_()`'s
`layout_backward()` result is not thrown away for `render_page_()` to redo. Key is
chapter + position + `PageOptions` + font size index, checked **after**
`resolve_stable_position()` (which can move `page_pos_`). `set_font`/`set_options` wipe
`TextLayout`'s paragraph cache wholesale, so a stale entry points at freed line
vectors. `LayoutWord::text` points into `WgbChapterSource`'s 32-slot paragraph window
rather than into `PageContent`, which is why the cached page is **moved** into `page_`
(only one may be live) and why `load_chapter_()` and `stop()` drop it explicitly:
rebuilding the source frees every slot it points into and no key comparison detects
that.

### Forward and backward must use the same line-fitting rule

**Both walks pass `require_full_height`.** Forward used to accept a line whose
*baseline* fitted and carry the descender into the next line's budget via
`pending_desc`; backward had no carry and gave its bottommost line a free descender
never repaid. Backward fitted more lines than forward and **ran past the page box on
30% of turns** (bottom 780 against 753 available on The Hobbit) — the same text landed
a line or two higher depending on which direction the reader arrived from. One rule for
both costs at most one line per page, one that was overhanging anyway.

**What remains is structural.** A page ending at a PageBreak or at end-of-chapter can
legitimately be reached from several starts, and backward picks the fullest rather than
the one the reader came from, skipping text at 1-2 places per book. `prev_page_()`
steps back one further page for an anchor and walks forward, keeping the last page that
ends at or before the current start: **every skip gone across four books**, for one
extra `layout_backward` and at most three boundary collections.

A handful of one-line offsets survive (25 / 5 / 2 / 2 over 520 / 579 / 170 / 640
pages). Closing those needs a chapter-wide forward chain, **built and removed**: it
worked, but walking the chapter drags the 32-slot paragraph window across it and peaked
around **31 KB**, reintroducing the heap abort with the clicker resident and putting a
per-chapter walk on the book-open path. Gating on free heap was not enough — the check
happens before the walk, not during it. Two other approaches that do *not* work:

- **A round-trip gate** ("does forward from backward's answer land back here?"). It
  does — backward's answer is a valid page ending in the right place, just not the one
  the reader came from, so the gate never fires.
- **Gating on how many paragraphs the answers differ by.** Distributions overlap
  completely: genuine skips span 4-12 paragraphs, correct turns 0-22.

### Page-turn latency

Everything CPU-side is off the critical path, leaving the ~300 ms waveform as the
floor:

- **The next page is laid out *and drawn* during the waveform**, so a forward turn is a
  memcpy plus an SPI write. Returning from the quick menu is a memcpy too. Both use the
  spare buffer.
- **Bulk SPI is interrupt-driven, not polled.** `EInkDisplay::sendData` uses
  `spi_device_transmit` for chunks >= 1 KB from DMA-capable memory,
  `spi_device_polling_transmit` otherwise. A framebuffer is 48,000 bytes (~10 ms per
  plane at 40 MHz, and `full_refresh` writes two) and polling spun the CPU for all of
  it. Small writes stay polled deliberately: a 105-byte LUT costs less than the
  interrupt round trip, **and LUT tables live in rodata, which is not DMA-capable** —
  hence the `esp_ptr_dma_capable()` guard rather than a size test alone.
- **Chapter load reads its descriptor table in one `fread`.** One read per entry costs a
  FATFS + SPI round trip apiece, and a chapter can have hundreds. On the path for every
  book open and chapter jump.
- **Images are never decoded on the device.** An image page turns at the speed of text.
- **Nothing walks the chapter.** Opening a book lays out one page.
- **The book index stays resident** for the session. Both `MainMenu` and `HomeScreen`
  load from disk only when `entries()` is empty — once, on a cold boot. Every mutation
  updates the in-memory copy and saves, so a non-empty index is authoritative. ~30 KB
  against ~165 KB free heap, and dropping it meant re-reading and re-parsing on entry to
  the home screen, on entry to the list, and again when the reader wrote progress.
- **Label truncation is linear, not quadratic.** `fit_prefix()` in `ListMenuScreen.h` is
  the one place that measures how much of a string fits a width; it accumulates
  `char_width` per codepoint. Every list screen used to inline a loop calling
  `word_width(text, fit + cb)`, re-measuring the whole prefix each step — O(n²) per
  label, on every row of every repaint. See "Text truncation" under Conventions.

### The spare framebuffer

`DrawBuffer` carries a third 48 KB buffer serving two jobs that never overlap, tracked
by `spare_use_`:

- **Snapshot** — a copy of the displayed frame. `ReaderScreen::pause()` takes it when
  the quick menu opens; `resume()` puts it back, so dismissing the menu is a memcpy.
- **Offscreen** — where `prerender_next_page_()` draws the next page while the panel is
  still running the current waveform. `take_predrawn_()` commits it.

They share storage because a reader is either in the quick menu or turning pages, never
both. **Claiming it for one purpose silently invalidates the other**, safe only because
both consumers re-check: `resume()` tests `has_snapshot()`, `take_predrawn_()` tests
`has_offscreen()`, and either falling false just means rendering normally.

`begin_offscreen()` redirects `draw_()` at the spare; `end_offscreen()` publishes it and
`commit_offscreen()` moves it into the inactive buffer. The commit paths (`refresh`,
`full_refresh`, `restore_snapshot`, the MGR2 sleep blit) use `inactive_()` directly.

Two things `prerender_next_page_()` must get right:

- **It restores `page_` and `page_pos_` afterwards.** `render_page_()` works on those,
  so the next page is made current for the draw and then put back. `page_` keeps only
  positional metadata either way — its `LayoutWord::text` pointers into the paragraph
  window die the moment laying out the next page slides it.
- **It skips the draw for a page containing images.** `draw_image_()` reads the card,
  which shares SPI2 with the panel, so it drains the waveform first — blocking the UI
  loop for the couple of hundred ms the current page is still painting. Image pages get
  their layout cached and nothing more.

`take_predrawn_()` keys on chapter, the **pre-resolve** request position (what
`next_page_()` produces, so no `resolve_stable_position()` call is needed to compare),
font size and the full `PageOptions`. Any mismatch renders normally.

### Panel health

A run of fast partial waveforms is **not DC-balanced**: each leaves residual charge in
the microcapsules, showing as ghosting and, over a long run, permanent contrast loss.

**Wintergreen has no periodic flush, arrived at the hard way.** Three versions shipped:
inline (every 24th `refresh()` promoted to full — the flash landed on whichever page
turn happened to be 24th); deferred to a 2500 ms idle (2.5 s after the last press you
are *reading the page*, and a flash there interrupts rather than delays); removed.
`partials_since_full_`, `flush_pending()`, `flush_ghosting()`,
`Application::kGhostFlushIdleMs` and `config::kFullRefreshEveryNUpdates` are gone.

Removing it is safe **because deep-sleep wake is a full boot**. The counter never
survived sleep anyway, and every session is bracketed by two full-panel waveforms:

| When | What runs |
|---|---|
| going to sleep | the sleep image — `full_refresh(RefreshMode::Full)` for a book cover, `Half` for the wordmark |
| waking | `Application::start()`'s `full_refresh()` (Half) |

With `kAutoSleepMinutes = 1` that is a full-panel waveform every time the device is put
down and picked up — more often than a Kindle, which flashes every few pages *while you
read*.

**What is genuinely uncovered:** a session that never idles for a whole minute. Page
turns every 45 s keep `inactivity_ms_` below the timeout indefinitely, so 30 minutes is
~40 partial updates with no full refresh. That is within normal e-reader practice (Kobo
exposes 1–100 pages *and* "never"), and the page-turn waveform is the vendor's OTP fast
LUT. Expect faint ghosting late in a long session; expect it gone after the next sleep.

The canary is ghosting that *survives* a sleep/wake cycle. Nothing in the current design
would produce that; if it appears, the fix is a counter in `DrawBuffer::refresh()`
calling `full_refresh(RefreshMode::Full)` — accepting that no threshold both protects
the panel mid-session and stays out of the way.

### The half-refresh temperature write — do not remove it

`refreshDisplay()`'s `EPD_HALF_REFRESH` case sends `CMD_WRITE_TEMP` (0x1A) with `0x5A`
before setting the mode byte. It looks redundant, because `initDisplayController()`
selects the controller's *internal* sensor (`CMD_TEMP_SENSOR_CONTROL` = 0x80) and a
manual write should then have no effect.

It was removed on exactly that reasoning, and the result was a device that ran one black
flash and then displayed nothing at all, ever. `0xD4` includes **LUT_LOAD**, and the OTP
waveform it loads is selected via the temperature register; without the write the panel
comes up with no usable LUT. What made it costly to find: **`Application::start()` ends
with `buf.full_refresh()`, whose default is `RefreshMode::Half`** — so the one broken
path is the one that paints the very first screen.

Two lessons that generalise: a vendor init sequence is not a place to remove things that
"should" be no-ops; and "the code reached this point" is not "the panel showed
something".

**Temperature compensation is live**, and the mechanism explains the `0x5A` disaster.
Selecting the sensor only says *which* sensor, not when to read it. The reading happens
when **TEMP_LOAD (CTRL2 bit 5, 0x20)** is set in the update sequence, and that bit was
**never set anywhere in this driver** — so the controller never sampled its own sensor
and the register only ever held the fixed `0x5A`.

`refreshDisplay()` ORs TEMP_LOAD into **one** path:

- **`EPD_FULL_REFRESH` (0x34 → 0x54)** — the sleep image and any DC-balancing flush, the
  updates that recondition the panel and that nobody waits on.
- **`EPD_FAST_REFRESH` is excluded** — it runs the custom LUT, a fixed table no
  temperature can select between, and it is the page-turn hot path.
- **`EPD_HALF_REFRESH` is excluded for a UX reason**, tried the other way first. Half is
  the boot paint, which with `kAutoSleepMinutes = 1` runs *every time the device is
  picked up*. With TEMP_LOAD it got visibly longer and flashed more: `0x5A` corresponds
  to a much warmer panel than a room, and e-ink waveforms lengthen as they get colder.
  Several times an hour it was intolerable. Any under-driving is cleared by the next
  full refresh.

Two deliberate safety properties: **the `0x5A` write stays** as the seed the controller
falls back to, so a sensor that does not answer leaves a usable value; and **`kTempLoad`
is the one line to flip** (`0x00` restores the previous fixed-temperature behaviour).
It is a local constant rather than a config option: whether the controller implements
CTRL2 bit 5 is a property of the hardware, not a preference.

**Still not possible: reading the temperature back.** `CMD_READ_TEMP` would need MISO
from the panel, and the panel is wired write-only.

### Panel rails — the idle power-down is gone, don't re-add it

`EInkDisplay::power_down_rails()`, `Application::kPanelIdleOffMs` (5 s),
`panel_rails_off_` and `DrawBuffer`'s wrapper are all **deleted**. It switched the analog
charge pumps off after 5 s of no input, reasoning that e-ink is bistable and the next
refresh would see `isScreenOn == false` and OR in `CLOCK_ON | ANALOG_ON` itself.

**On hardware it silently swallowed one page turn.** With the rails down the next
`partial_refresh()` ran its waveform before the rails were up: the old page stayed on
the glass and only the turn *after* that drew. User-visible as "wait five seconds, press
forward, nothing happens".

Two reasons not to retry: those two CTRL2 bits are marked "best guess" in `epd.h` and
are only *documented* in combination with a refresh; and the fix (a settling delay, or a
separate power-up activation) would land on **the first page turn after a pause**, the
common case, paying the whole saving back as latency.

The rails are still powered down where it costs nothing: `refreshDisplay()`'s
`turnOffScreen` path at the end of a full refresh, which is what the sleep image uses.

## Low-battery cutoff

**Live, at 3200 mV.** `wintergreen::kLowBatteryCutoffMv` in `Application.h` —
deliberately **not** in `WintergreenConfig.h`, because it is a property of the cell and
the board's brownout margin rather than a preference, and a wrong value makes the device
look bricked. The header carries the three numbers that bound the choice (protection IC
~2500, permanent-damage onset ~3000, discharge-curve zero 3300).

Below the cutoff the device saves state, shows the wordmark and deep-sleeps, and
`verify_battery()` in `main.cpp` refuses to boot until charged. Everything rests on
`Esp32Runtime::battery_millivolts()` being right; if the on-screen percentage stops
tracking reality, suspect this first.

Five guards. **The first three exist because their absence bricked the device** — the
reader boots, draws one frame, and goes back to the sleep screen, over and over, with
USB Serial/JTAG powered down between attempts so esptool cannot get a word in.

1. **`usb_attached()` must be a real check.** It read GPIO20 (U0RXD) and called a high
   level "USB connected" — true only behind an external UART bridge. The X4 uses the
   C3's **native** USB, so GPIO20 never goes high and the answer was always "no host":
   the boot gate fired *while plugged in*, exactly the case meant to be the escape
   hatch. It now asks `usb_serial_jtag_is_connected()`, valid before the driver is
   installed.
2. **The low reading must persist for wall-clock time**, `kLowBatteryHoldMs` (4 s), not a
   frame count. The first version wanted five frames — ~125 ms, *shorter than one e-ink
   refresh*, which is the biggest current spike the device has.
3. **It never samples while the panel is busy** (`display().is_busy()`), same reason.
4. **A zero reading means "cannot measure"** — no battery, or an uncalibrated ADC — and
   so does anything below `kImplausibleMv` (2500). `battery_millivolts()` returns 0
   rather than a raw ADC count when calibration is unavailable, precisely so a wrong
   voltage cannot trip the cutoff.
5. **The boot check runs before `epd.begin()`**, so a refused boot never lights the panel
   or mounts the card.

The default that shipped, 3300 mV, is *0%* on the discharge polynomial in
`battery_percentage()` — the cutoff sat exactly at empty with no margin, so an ordinary
sag crossed it. Pick a value below the curve's zero, not at it.

**Recovering a device that is cutting off:** leave it on USB; the charger IC runs
independently of the SoC. To flash one that is sleeping instantly, start the upload and
hold the power button: wake is level-triggered on GPIO3 LOW and a wake is a
non-`ESP_RST_POWERON` reset, so `verify_wakeup_press()` returns immediately and the
device boot-loops while held — enumerating USB each pass and giving esptool's retry a
window.

Charge-side health (holding at 4.2 V, thermal limits) is handled by the charger IC and
is **not** reachable from firmware.

## SD card wear

Writes are rare — the reading position moved out of the page-turn path long ago — and
every remaining one is deduplicated:

| File | Written | Guard |
|---|---|---|
| `.wintergreen` (settings) | reader/quickmenu pop, sleep | byte-compare against `settings_written_` |
| `.wintergreen-index` | reader close, library scan, serial mutations | `BookIndex::dirty_` |
| `<book dir>/book.pos` | reader close | `ReaderScreen::pos_written_` |

**A whole reading session can write nothing at all.** Open a book you were already on,
read, sleep: `mark_opened` does not move the order, `set_progress` does not move the
percentage, so the index stays clean; position is written only if it moved; settings
only if a byte changed. Four changes made that concrete:

- **`BookIndex` has a dirty flag.** `save()` returns immediately when clean, and
  `mark_opened` / `set_progress` set it only when the stored value actually moves.
- **One index write per session, not two.** `record_book_opened()` used to save index
  *and* settings; the index goes out once, from `record_book_progress()` on reader close.
- **Nothing is written on book open.** Every ordinary way of putting the device down
  routes through `do_sleep_()`, which saves settings anyway, so that write only mattered
  after a power loss — and cost an SD write on every book open. `.pos` is written at
  close for the same reason, so a power loss already resumes at the position you started
  from, and now at the previous *book* too.
- **Settings are saved *after* the pop, not before.** `save_settings_()` writes
  `book_path` only while the reader is on the stack, so saving first persisted "resume
  this book" for a book just closed, then had to be rewritten at the next sleep.

**Don't add a write to the page-turn path.** That is the one thing that would change the
wear profile by an order of magnitude, and it is why `.pos` is written from
`ReaderScreen::stop()` alone.

## Configuration

`WintergreenConfig.h` **in the project root** is the only place with tunable values. The
root is on the include path for both platform builds.

| Constant | Default | Notes |
|---|---|---|
| `WG_BLUETOOTH_PAGE_TURNER` | *(commented out)* | A `#define`, not a constant. The clicker's MAC, `"AA:BB:CC:DD:EE:FF"`. Undefined keeps NimBLE out of the build. |
| `WG_WIFI_SYNC` | *(defined)* | A `#define`, same reason. Undefined drops Wi-Fi, lwIP and `esp_http_client` entirely. |
| `kWifiName` / `kWifiPassword` / `syncServer` | `""` | Read by `platforms/esp32/wifi_sync.h`. **`syncServer` must not be a `.local` name** — that is mDNS and there is no resolver in the firmware. Use a literal IP or a hostname your router serves DNS for. |
| `kSunlightFadingFix` | `false` | Powers the panel off at the end of every *full* refresh, restoring contrast lost to sunlight. There are no periodic full refreshes, so this affects the sleep image and boot paint only. Read in `Application::start`. |
| `kAutoSleepMinutes` | `1` | Read in `Application::update`. |
| `kPowerHoldSleepMs` | `kHoldDelayMs` (250) | Power-button hold before sleeping. Defined *as* `kHoldDelayMs` so every button splits tap from hold at the same moment. |
| `kHoldDelayMs` | `250` | Hold before a nav button auto-repeats. Below ~250 ms a tap reads as two presses. |
| `kHoldRepeatMs` | `200` | Gap between repeats once started. |
| `kHoldAccelStep` | `0` | Extra list entries per repeat. `0` = constant one per repeat. Shipped as `0.9` on an `int` field, truncating to `0` — acceleration had silently never been on. Make the field a float if fractional steps are wanted. The reader ignores it: page turns never accelerate. |

Everything except the two `#define`s is an `inline constexpr`; those are macros because
only the preprocessor can keep a whole IDF stack out of the image. A first attempt used
`#if config::kBluetoothPageTurner[0] != '\0'` — `#if` cannot evaluate a C++ constant,
and the error names the `::`, not the config.

The file must open with `#include <cstdint>`. It is **gitignored** (it holds the Wi-Fi
password), so a fresh clone does not build until the block in the README is pasted into
place. That block is the canonical default set and must be kept in step with this table.

Everything else is hardcoded **at the call site**. `Application` used to expose each as a
`static constexpr` member returning a literal (`rotate_display()`, `show_sleep_text()`,
`list_align()`, `menu_font_size()`, `invert_menu_buttons()`, `invert_bottom_paging()`,
`invert_side_buttons()`); those are gone with the branches they selected. Don't
reintroduce that indirection:

- lists are always portrait (`buf.set_rotation(Rotation::Deg90)`) and the menu font is
  always the 32 px header face, so `ListMenuScreen`'s static `font_size_idx_` /
  `set_font_size()` / `font_size()` / `apply_ui_font()` and its `list_align_` are gone
- no sleep caption. The sleep screen shows the cover of the book **currently open**, and
  only when it is not in `/.hidden/` — a hidden book must not be advertised by a screen
  that sits on display while idle. Everything else gets the embedded wordmark.
- battery is a header percentage, never an icon. The converted-book marker is gone —
  every book is a WGB, so the trailing dot marked every row and said nothing.
- images always render: the `images_enabled` global is gone, not defaulted
- single reader font (Atkinson Hyperlegible, in the `font` partition), no picker
- book list is title-only, sorted most-recently-opened first with a hairline before
  never-opened books
- fixed page geometry: margins at `ReaderSettings::h_padding()` / `v_padding()`, and
  alignment, line spacing and font sizes all deferring to the book's own CSS

`ReaderSettings` is down to **one** live value — `font_size_idx` — plus `rotate_reader_`
on `Application`. Those two and reading position are the only genuine runtime state.
Hyphenation is always on (English patterns; unknown language tags fall back to none).

## Repository map

The tree was flattened in `18fa42b`: the core library moved from `lib/wintergreen/` to
**`wintergreen/`** at the repo root, `platforms/desktop/` was deleted, and two screens
were renamed (`LyraExtScreen` → `HomeScreen`, `ReaderOptionsScreen` → `QuickmenuScreen`).
Anything still saying `lib/` is stale.

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
  wifi_sync.h              NAS book sync; #ifdef WG_WIFI_SYNC
  font_manager.h           ESP32 FontManager: mmaps the font partition
  font_partition.h         RawPartition — find + esp_partition_mmap, nothing else
  default_16MB.csv         partition table

wintergreen/               portable core
  Application.{h,cpp}      screen ownership, ScreenId, settings I/O, sleep screen
  ScreenManager.h          8-deep screen stack, start/stop/pause/resume
  Input.h                  Button, ButtonState, HoldRepeat
  Runtime.h                IRuntime
  FontManager.h            portable base: WGFS bundle parse into BitmapFontSet
  screens/
    IScreen.h              screen interface
    ListMenuScreen.{h,cpp} base list: header, rows, scroll, hold-repeat, fit_prefix
    HomeScreen.{h,cpp}     recents carousel; overrides draw_all_ entirely
    MainMenu.{h,cpp}       book list + Sync row + hidden-books mode
    ReaderScreen.{h,cpp}   the reader: paging, images, .pos, page cache
    QuickmenuScreen.{h,cpp} in-book menu + inline TOC
  display/
    DrawBuffer.h           IDisplay iface + double buffer + all drawing
    ui_font_{small,large,header}.h   Iosevka Slab Medium, 14/24/32 px
  content/
    BitmapFont{,Format}.h  WGF1 glyph reader / format doc
    ContentModel.h         Paragraph/Run/TextStyle; kMaxFontSizes = 8
    TextLayout.{h,cpp}     line breaking, pagination, page cache
    IParagraphSource.h     the interface TextLayout pages over
    BookIndex.{h,cpp}      singleton index + StringPool, .wintergreen-index
    StringPool.h           StringRef arena backing BookIndex
    CoverPaths.{h,cpp}     cover_*.bin + book.pos paths, kHomeCover/kSleepCover
    wgb/WgbFormat.h        on-disk WGB layout (magic WGB2, no version field)
    wgb/WgbReader.{h,cpp}  device read path
    wgb/WgbWriter.{h,cpp}  host-only, converter output
    wgb/WgbConverter.*     host-only, EPUB → WGB
    Book/EpubParser/CssParser/XmlReader/HtmlEntities   host-only (see Removed)
    ZipReader.{h,cpp}      host-only: reads the source EPUB during conversion
    ImageDecoder/JpegDecoder/PngDecoder/stb_image.h    host-only, converter rasteriser
    hyphenation/           Liang, English patterns only (27 KB trie)

tools/
  generate_assets.py       pre-build hook: builds the font/sleep flash images
  make_font.py             TTF → WGF1 / WGFS; --mono is mandatory here
  check_font.py            validates a bundle and ASCII-renders a sample
  make_sleep_image.py      PNG → MGR2
  convert-books.sh         nix-shell wrapper around tools/epub2wgb
  epub2wgb/                host converter (its own CMakeLists, explicit sources)
  tests/                   host-side checks; see Tests
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

## Architecture

`Application` owns every screen as a by-value member and a `ScreenManager` stack.
Screens never push directly; they call `push_screen`/`replace_screen`/`pop_screen`,
which queue a `ScreenId` applied after `update()` returns. `screen_for_(ScreenId)` maps
the enum to the member.

Stack: `HomeScreen` → `MainMenu` (all books) → `ReaderScreen` → `QuickmenuScreen`. Only
these four. Back on the home screen opens the book list.

**Opening a book from the list *replaces* the list.** `MainMenu::on_select` calls
`replace_screen(Reader)`, so the stack is `HomeScreen → ReaderScreen` and Back from the
reader lands on the home carousel rather than back in the list. The carousel is sorted
most-recently-opened first and `record_book_opened()` has already bumped this book to
the top. It also drops the list's `entries_` immediately.

`ScreenManager::replace()` is **not** `pop()` + `push()`: `pop()` resumes whatever is
underneath, so the old sequence had `HomeScreen` rebuild its carousel, re-read the index
and decode a cover — all discarded one line later when the push paused it again.

**The root screen depends on whether the library is empty.** `Application::start()` loads
the index and pushes `MainMenu` instead of `HomeScreen` when there are no books: an empty
carousel has nothing to show, while the list is where Sync lives. Two consequences:

- `MainMenu::on_back()` does nothing when `app_->at_root()` — no home screen underneath.
- `MainMenu::on_select()` *pushes* the reader at root instead of replacing, so Back from
  a book returns to the list rather than emptying the stack.

`ScreenManager::pop()` refuses to remove the last screen. An empty stack makes `top()`
null and `Application::update` then silently stops processing input —
indistinguishable from a freeze.

**The carousel falls back to the library when nothing has been opened.**
`HomeScreen::on_start()` picks the five most recently opened; if none have been (a fresh
card, or wiped settings) it shows the alphabetically first books instead, ordered with
the shared `ci_less()` so "first" means the same thing as in the book list's never-opened
group. The "No books on the card" panel is only a safety net.

### HomeScreen

A **carousel of the five most recently opened books**: battery percentage top right, one
large cover inside a 2 px frame held 10 px clear of it, title (up to two lines) and
author centred beneath, and a row of diamonds for position (the selected one carries a
second, larger outline diamond). Only title lines actually drawn advance the cursor, so
the author never floats under a reserved blank line; the diamonds are anchored to the
bottom of the panel.

Up/Down are **swapped** here (`swap_updown_`, applied to the state forwarded to the base
class): the carousel runs left-to-right, so the top of the side rocker and the bottom
front key advance it. Select opens the book, **Back opens the full book list** — which is
why there is no Recent Books screen. It derives from `ListMenuScreen` purely for the
item/selection/navigation machinery; every pixel comes from its own `draw_all_`.

- The selected cover is loaded on demand **from inside `draw_all_`** (hence the `mutable`
  cover members). Loading from `update()` would draw one frame with the previous cover
  and then redraw — two e-ink refreshes per press.
- **There is exactly one cover cached, and a second one crashes the device** — see
  "Investigated and rejected". `cover_slot_` is what stops the card being re-read every
  frame, including for a book with no cover file: empty `cover_data_` with a valid
  `cover_slot_` is a real state, not a miss.
- `on_start()` clears `cover_slot_` because it rebuilds `slots_` from the index — a sync,
  a serial upload or a delete repoints every index without necessarily changing one.
- The cover comes from **`cover_home.bin`**, rendered by the converter to fit the box
  exactly (`kHomeCoverW` × `kHomeCoverH` = 424x479 in `content/CoverPaths.h`), so the
  scaler collapses to a 1:1 copy and the image is dithered exactly once. `cover.bin`
  (160x240) is the only fallback and can only be pixel-doubled. **`cover_sleep.bin` is
  not a fallback here** — it is composed for the panel with letterbox bars baked in.
  Those two constants mirror `draw_all_`'s layout maths; if the layout moves they drift,
  the fallback takes over and quality quietly regresses until the library is
  re-converted.
- Two scaling paths. **Upscaling is whole-number only**: pixel-doubling keeps the 1-bit
  dither intact where a fractional nearest-neighbour scale moirés it. **Downscaling is a
  box filter plus a 4x4 ordered dither** — the average of a cell is a coverage fraction,
  and a hard 50% threshold would throw away every tone between paper and ink. Nothing the
  converter writes reaches the downscale path any more; it is kept for the case where the
  layout box shrinks below `kHomeCoverW`/`kHomeCoverH` before a re-conversion.
- All-white rows are **trimmed off the top and bottom**. Covers often carry a white band
  above and below the artwork, and with the frame at a fixed distance those read as
  lopsided padding. Columns are left alone.
- The cover read calls `wait_panel_idle()` first: the card shares SPI2 with the panel.

### QuickmenuScreen

A flat list: header block, Font Size, Orientation, Clicker (only when a MAC is
configured), then Chapters when the book has a TOC. There is no separate Chapters page —
selecting a chapter sets `pending_chapter()` / `pending_para_index()` and pops one screen;
`ReaderScreen::resume()` consumes them.

**Item 0 *is* the header block** — book title, chapter and their two percentages.
Selecting it pops back to the book, and it is the default selection, so opening the menu
and pressing Confirm is a no-op round trip. It is a real list item rather than a special
case, so navigation, wrap-around and `on_select` dispatch work without changes. What it
is *not* is a row: `draw_all_` paints it at the top and inverts the whole block when
`selected() == kIdxBack`, and the list loop starts at `kIdxBack + 1`. Two places must
agree that it costs no height, or the last chapter becomes unreachable:
`get_visible_count_` gives index `kIdxBack` a row height of 0, and `draw_all_`'s `total_h`
is `(n - 1) * row_h_()`.

Its header is drawn entirely by its own `draw_all_` — the base class's card header is
unused, so `title_`/`subtitle_` are left empty. Book title, current chapter, rule; there
is **no battery** (it is an overlay on the book, and the reader underneath shows none).
Both title and chapter are word wrapped (`wrap_`, 2 and 3 lines) and each carries its
percentage right-aligned on its **first** line, set in the *same font as the text beside
it* so the pair reads as one line. The chapter is a size down (`chapter_font_()` =
`section_font_`). `header_h_()` measures that block from the wrapped lines computed in
`on_start()`, and `get_visible_count_()` calls it — the two used to re-derive the height
independently, which is how scroll bugs get in.

A hairline divides settings from chapters, drawn in a `kSeparatorH` gap of its own rather
than inside the first chapter's row — a selected row fills its whole rect and would paint
over it. Row height is `row_h_()` = the list font's `y_advance()` + padding, not a
constant: the list is set in the 32 px header face and the old fixed 28 px row was
shorter than a single line.

Two things the TOC merge broke: chapter titles legitimately contain `": "` ("Chapter 1:
The Beginning"), so the `Label: Value` split is gated to non-chapter rows or the title is
torn into two columns; and nested TOC entries carry a depth that must still indent, via
`ListMenuScreen::get_item_indent()`.

The book title comes from `ReaderScreen::display_title_()`, not `wgb_.metadata().title`:
a converted EPUB with no usable metadata title yields the literal string `"none"`, and
`BookIndex` already substitutes the folder name. Without the same substitution here the
quick menu said "none" for a book the home screen named correctly.

### MainMenu

**Row 0 of the book list is a `Sync` action, not a book.** Row 1 is its hairline (pushed
into `separators_` like any other), and books start at row 2 — so `entries_index_for()`
subtracts one *on top of* the separator shift, and `visual_for_entries()` skips both
kinds of non-book row. Every derived separator position in `populate_list_()` carries the
same `kFirstBook = 2` offset; getting that wrong shows up as the wrong book opening, not
as a drawing glitch.

`run_sync_()` calls `IRuntime::start_sync()` and returns immediately. Its label reports
progress (`Sync` / `Syncing...` / `Synced`), read from `sync_shown_` rather than the
runtime because `get_item_label` is called per row draw. `MainMenu::update()` repaints
when that state changes: the index `generation()` check above it only covers books
*arriving*, so a sync that finds nothing new would leave the row on "Syncing..." forever.
Idle and Failed both read `Sync`.

`select_first_book_()` moves the cursor off Sync onto the first book when the screen
opens. Called from `on_start()` and from `update()`'s scan branch — deliberately **not**
from `populate_list_()`, which also runs on an in-place refresh (a serial upload) where
moving the cursor under the user would be wrong. With no books there is no hairline and
Sync is the only row.

**Hidden books are a mode of `MainMenu`, not a screen.** A 1 s back long-press **on the
home screen** opens the list with the books under `<books_dir>/.hidden/` at the top, above
their own hairline (`set_show_hidden()` before the push). That gesture is the only way to
see them, and `MainMenu::stop()` clears the flag.

**The hold fires while the button is still down**, the moment `kHiddenHoldMs` elapses —
waiting for the release made the device look dead for the length of the hold. Safe *here*
because presses are rising edges only, so `MainMenu` receives no Button0 event from a
button already down when it started, and `back_consumed_` makes the eventual release a
no-op. The plain book list still opens on release, since a tap cannot be distinguished
any earlier.

The gesture lives on the home screen rather than in the list because the *tap* resolves
on **release**, and only a release-time push lands on a screen whose buttons are up. It
was first written in `MainMenu`, which pushed on the *press*: the pushed screen started
with Button0 still down, its own release handler fired, and the list vanished the moment
the user let go. `HomeScreen` also swallows the first hold after every `start()`
(`back_ignore_`), because `MainMenu` pops on the back press and hands the home screen a
button that is still down. **Any screen pushing another from a button press inherits
this problem.**

Four things the hidden list depends on:

- `.hidden` books are **not in `BookIndex`** (its scan skips dot-directories), so
  `MainMenu::scan_hidden_()` walks the folder itself and reads title/author straight from
  each WGB. Those entries carry owned `title_own`/`author_own` strings rather than
  `StringRef`s, because there is no pool entry to reference. The scan runs only when the
  gesture first asks for it.
- `on_select` skips `record_book_opened()` for a hidden entry — a hidden book must never
  reach the recents carousel.
- `separators_` holds **visual** indices, so the hidden divider shifts the
  recently-opened/never-opened one below it by a row. `populate_list_()` inserts the
  hidden group after sorting, so it keeps its own alphabetical order.
- Hidden books are `.hidden/<book>/book.wgb` like any other. The old `HiddenBooksMenu`
  scanned for `*.epub`, which stopped matching anything the moment EPUB support was
  removed.

### ListMenuScreen

Base class for every list screen. There is **one** visual theme; the `MenuTheme` enum is
gone. Two per-instance booleans select the remaining variations:

- `detail_list_` — two-line rows (title + subtitle, full-width divider, right-hand
  column). Set by MainMenu. The right column is the reading percentage and sits on the
  **subtitle** baseline, bottom right beside the author, so the title gets the full row
  width. The **last** row draws no divider, and neither does a row whose next row is a
  section hairline, or the two rules land a few pixels apart and read as a double line.
- `plain_list_` — centred-title header instead of the battery status bar.

Both false = standard header + centred single-line rows. A non-empty `subtitle_` switches
the header into the book-details card and is checked before either flag.

**There is one separator, defined once.** `kSeparatorH` (14 px) and `draw_separator_()`
are the only hairline geometry in the tree, and every list uses them, including
`QuickmenuScreen`, which draws its own list but inherits both. Before that each site
carried its own `kSepH` — 8, 12 and 14.

Row height is per-row, not per-list: `wintergreen_slot_h_(index)` returns the one-line
height where `is_single_line_row(index)` is true (MainMenu's Sync action) and the
two-line height otherwise. `wintergreen_visible_from_` asks per index too, so scrolling
stays consistent with what is drawn.

No screen draws button labels, nav-arrow glyphs or a battery icon: the labelled `Back |
Select` / `Down | Up` boxes and the ◀▶▲▼ hint row were removed, and `draw_bottom_` only
draws the book-details footer. Don't add them back — the buttons are fixed and the panel
is small.

**The header is battery-only.** No wordmark on any screen, no rule under the header. Its
height is `battery_row_h_() + kHeaderBottomGap` in **both** `draw_header_` and
`compute_header_h_`. It used to be measured off `header_font_` (41 px) while the
percentage is drawn in `section_font_` (31 px), so the list started 10 px too low.

Every screen draws the percentage through `draw_battery_`, and `HomeScreen` — which
otherwise shares no drawing code — calls it too, sizing its own header from
`battery_row_h_()`. The position is a property of the base class rather than of each
screen on purpose: when the carousel had its own copy of the maths it drifted by a few
pixels in both axes, visible as a jump when moving between screens. Don't reintroduce a
per-screen `W - pad - pw`.

Drawing is `draw_header_` → `draw_bottom_` → `draw_list_`, with `compute_header_h_`
mirroring the header maths for scroll calculations. `get_visible_count_` must stay
consistent with `draw_list_`'s branch selection — separate functions keyed on the same
conditions, and divergence causes scroll bugs.

### Sorting — never use std::sort here

`std::sort` and `std::stable_sort` are templates on both the iterator *and* the
comparator, so every call site gets its own copy of introsort, insertion sort, `__rotate`
and `__merge_adaptive`. Three call sites over two element types cost **~34 KB of flash**
— more than every screen's drawing code put together — to order lists that are at most
`MAX_BOOKS` (250) long and built once per screen entry. They are gone:

- `MainMenu` has one file-static `stable_sort_indices_()`: a bottom-up merge sort over a
  `uint16_t` permutation, with the comparator as a **plain function pointer** so there is
  exactly one copy in the image no matter how many callers appear. `iota_indices_()`
  builds the identity permutation and `apply_permutation_()` reorders afterwards. Sorting
  indices rather than elements also means `BookEntry`'s four `std::string`s move once
  instead of O(n log n) times.
- `HomeScreen::on_start` does not sort at all. It runs a five-pass top-K selection over
  `BookIndex::entries()`, so it never materialises path/title/author for books it is
  about to discard.

If you need another ordering, reuse `stable_sort_indices_` (move it to a shared header if
a second file needs it). Do not reach for `<algorithm>`.

## Bluetooth page-turner

A BLE HID-over-GATT **host** for a clicker, in `platforms/esp32/bluetooth_clicker.h`. Off
by default, off at every boot, and off after any failure — the radio is the largest
current draw this firmware can switch on, tens of mA against ~15 mA idle.

| Build | pio flash | pio RAM |
|---|---|---|
| no BT keys at all | 430,790 | 159,820 |
| BT keys, MAC undefined (**default**) | 434,580 | 160,216 |
| MAC defined | 728,888 | 164,128 |

The sdkconfig keys cost **3.8 KB of flash and 396 bytes of RAM** in a build that will
never use them; the stack itself is ~294 KB of flash plus tens of KB of *heap* while the
radio is on. app0 is 6.4 MB, so none of this is capacity pressure.

**Three sdkconfig keys look wrong and are not.** IDF guards `nimble_hidh.c` — the HID
**host** — with `CONFIG_BT_NIMBLE_HID_SERVICE`, which is the HID *device* service option,
and that option lives in a menu gated on `BT_NIMBLE_GATT_SERVER`, which depends on
`BT_NIMBLE_ROLE_PERIPHERAL`. A central-only device cannot compile the host without
nominally enabling the peripheral role. Nothing ever advertises. Drop any of the three and
the link fails with `undefined reference to esp_ble_hidh_init` — a *link* error naming an
IDF file, with nothing pointing at the config.

Design:

- **It scans before connecting, and must.** Connecting straight to the configured MAC does
  not work, for two reasons. A BLE address does not carry its own *type* (public/random),
  and guessing wrong costs 30 s because `esp_hidh` calls `ble_gap_connect` with that
  timeout and blocks. And HID devices routinely use a **resolvable private address**,
  rotating every ~15 minutes — `(addr[5] & 0xC0) == 0x40` identifies one — so a MAC read
  an hour ago is not that device's address any more. An 8 s active scan yields the address
  *and* type that are true now. The configured MAC is a filter, not a destination; if
  nothing matches, anything advertising HID service UUID 0x1812 is taken instead, which is
  what makes a rotating address work.
- **No reconnect.** A disconnect, a refused connection or one that never completes all drop
  to Disconnected. A radio hunting for a device that is not there is the leak this avoids.
- **No persistence.** Deep-sleep wake is a full boot, so the device always comes up cold.
  With `kAutoSleepMinutes = 1` that means re-enabling after any pause long enough to
  sleep — the deliberate trade, since turning pages resets `inactivity_ms_`.
- **Everything blocking runs on a worker task.** `esp_hidh_dev_open()` blocks for the
  length of a connection attempt and teardown calls block too, so `toggle()` spawns a
  one-shot task and returns. `g_busy` makes a second toggle during a transition a no-op.
- **The HID callback never tears its own stack down.** It sets `g_teardown_request`;
  `wg_clicker::poll()` spawns the teardown task. Deinitialising a stack from inside its own
  event callback deadlocks.
- **NVS is initialised lazily**, in `bringup_stack()` — the bond store is its only user.

Input arrives as **edges only**: `g_clicker_buttons` is merged by
`Esp32InputSource::poll_buttons` the way `g_serial_buttons` is, and nothing sets
`current`, so a clicker press can never look held and drive `HoldRepeat`. Reports are
matched permissively — scan the payload for Left/Right Arrow (0x50/0x4F) or Page Up/Down
(0x4B/0x4E) rather than parsing the report map, since `esp_hidh` has already done
discovery and clickers disagree about layout. A `g_last_key` transition check stops a held
key repeating.

Left advances and right goes back — **the opposite of a presentation remote's
convention**, intentional. Injected as Button2/Button3, "next" and "previous" in the
reader in *both* orientations.

**Four things are required together, and each fails silently on its own.** The first
version had all four wrong and produced one symptom — "Connecting" for a minute, then
"Disconnected":

1. **The MAC must be byte-reversed.** NimBLE's `ble_addr_t.val` is little-endian: `val[5]`
   is the byte written leftmost in a MAC, which is why `ble_gap.c` tests `addr[5] & 0xC0`.
   `esp_hidh_dev_open()` memcpy's the array straight into `ble_addr_t.val`. `parse_mac()`
   fills `out[5 - i]`. Reversed, the stack hunts for an address that does not exist and
   times out looking exactly like a clicker that is switched off.
2. **`CONFIG_BT_NIMBLE_GATTC_AUTO_PAIR=y`.** `esp_hidh`'s NimBLE path never calls
   `ble_gap_security_initiate`, and HID characteristics need an encrypted link. This is
   what makes `ble_gattc` retry a read through pairing when the peer answers
   `INSUFFICIENT_ENC`/`AUTHEN`. Without it the link comes up and reads nothing.
3. **`ble_hs_cfg.sm_*` must be set in `bringup_stack()`.** NimBLE defaults `BLE_SM_BONDING`
   and *both* key-distribution masks to `(0)`, so pairing exchanges no keys and there is no
   bond. The ID key (IRK) is what later lets the stack recognise a rotating address.
4. **The scan**, above.

**Confirmed working on hardware**: connects reliably, pages turn, stays up.

### The failure codes are gone — here is what they found

The row briefly carried numeric stage codes (`Failed 8003`), a free-heap readout, a GAP
listener and an `esp_reset_reason()` display. All **removed** — it was scaffolding, and it
worked. Keep this record instead:

- `8003` / `8011` — sync timed out with 3 KB, then 11 KB, of internal RAM free. This
  established heap as the binding constraint. Note `esp_nimble_enable` **ignores
  `xTaskCreatePinnedToCore`'s return and always reports ESP_OK**, so a host task that could
  not be allocated is indistinguishable from one that started and never synced.
- The stage split proved a *first* failure could wedge the session.
- `rst4` plus a backtrace named the real bug: `std::bad_alloc` in
  `TextLayout::assemble_page`, not in any BLE code.

If a failure needs diagnosing again, the cheapest reconstruction is a `FailStage` enum set
at each bail-out in `bringup_stack()` and shown after "Failed". Do not leave it in.

`ClickerState` is Unavailable / Disconnected / Connecting / Connected. **Every failure
reads as Disconnected**: the row is a switch, not a status console, and each of those
states meant the same thing to the user (press it again).

### The row shows the clicker's own battery

`ESP_HIDH_BATTERY_EVENT` carries the peer's level, so once connected the row reads `Page
Turner: 89%`.

**That event is not enough and waiting for it shows nothing.** `nimble_hidh.c` posts it
*only* from a `notify_rx`, and most clickers notify only on change — so the row sat on
"Connected" for hours. `read_battery_once()` does an explicit `ble_gattc_read_by_uuid` of
the Battery Level characteristic (0x2A19) over the whole handle range, immediately after
connect and before the state goes Connected. One round trip, no discovery of our own,
bounded at 2 s so a clicker that ignores the read cannot hang the connect.

- **Captured once**, then left alone: a percentage that twitches while you read is worse
  than a slightly stale one. Cleared by `radio_off()`.
- **Not every clicker has a battery service.** No answer means the row says "Connected".
- `QuickmenuScreen::update` watches the percentage as well as the state, since a later
  notification can still update it.

**The read doubles as the liveness check.** `ESP_HIDH_OPEN_EVENT` with `status == ESP_OK`
is not sufficient: it has been seen arriving for a link that then delivers no reports,
leaving the row on "Connected" with a clicker whose buttons did nothing.
`read_battery_once()` returns false when the read could not be issued or never completed,
and `connect_task` treats that as a failed connect. A peer that will not answer a GATT
read will not send input reports either. This does **not** reject clickers without a
battery service: those still *answer* ("attribute not found"), so they connect and show
"Connected" with no percentage.

**Never find the connection with `ble_gap_conn_find_by_addr(&g_target)` alone.** That was
the underlying bug. A HID device typically advertises under a resolvable private address,
and once bonding completes the stack resolves it to the peer's *identity* address — so the
connection no longer matches the address the scan saw and the lookup returns `ENOTCONN` on
a live link. `live_conn_handle()` tries the scanned address first and otherwise walks the
handle range matching `peer_ota_addr` or `peer_id_addr`. Handles are controller-assigned,
**not** 0-based indices, so iterating `0..CONFIG_BT_NIMBLE_MAX_CONNECTIONS` does not work.

### Stack sizes are not the defaults, deliberately

`CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` is **6144**, not IDF's 4096, and
`esp_hidh_config_t::event_stack_size` is 6144 rather than 4096. The NimBLE host task runs
GATT discovery, the security manager, *and* — because `ble_store_config` persists bonds —
**NVS writes**, which are stack-hungry. Bonding completes a second or two after a HID
connect, which is precisely when a device that resets shortly after connecting would do it.

### Never use `p->input.data`

`nimble_hidh.c` allocates one block holding the event struct followed by the report
payload, points `input.data` at the payload, posts the block to the event loop — which
**copies** it — and then **frees the original**. The pointer arriving in the callback
refers to freed heap. The copy has the payload at a known offset, so `hidh_event_cb`
recomputes it as `(const uint8_t*)p + sizeof(esp_hidh_event_data_t)`. Same applies to
`feature.data` if that event is ever handled.

### The stack comes up once per boot and is NEVER torn down

`bringup_stack()` runs each init at most once, guarded by its **own** flag. Toggling off
calls `radio_off()`, which closes the link and disables the controller but leaves the
NimBLE host and esp_hidh initialised for the session.

**One flag for the whole function is not enough**: a bringup that reached
`esp_nimble_init()` and then failed at sync left a single end-of-function flag false, so
the next attempt re-ran the one call that cannot be repeated. Per-step flags make a retry
resume rather than restart.

**Do not add a deinit path back.** Three attempts were made; the first two each shipped a
distinct bug:

1. **Unconditional teardown** → a bringup that died at sync still called
   `esp_hidh_deinit()` on a stack whose host had never started, leaving esp_hidh's
   `event_loop_handle` non-null. `esp_hidh_init()` returns `ESP_FAIL` ("Already
   initialised") when it is, so: *first attempt after a boot fails at stage 8, every
   attempt after that fails at stage 6, permanently, until a power cycle.*
2. **Per-stage tracked teardown** → fixed that, then crashed:

   ```
   assert failed: ble_hs_init at ble_hs.c:928
     esp_nimble_init  (nimble_port.c:200)
     wg_clicker::bringup_stack()
   ```

   Line 928 is `SYSINIT_PANIC_ASSERT(rc == 0)` on the `os_mempool_init` of
   `ble_hs_hci_ev_pool`. With `CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC=y` — set here —
   `ble_hs_ctx` and its `hci_os_event_buf` are heap-allocated behind an `if (!ble_hs_ctx)`
   guard, and `esp_nimble_deinit()` does not restore those pointers. A second
   `esp_nimble_init()` builds the pool on freed state and panics. **Cycling the NimBLE
   stack is not supported by this IDF, and it fails by crashing rather than by returning
   an error.**

**The controller is a different matter, and this is the battery lever.**
`esp_bt_controller_init`/`deinit` cannot be cycled, but
**`esp_bt_controller_enable`/`disable` can** — that is the documented pair, and disabling
is what actually stops the radio drawing current. So the flags split:

| Flag | Lifetime |
|---|---|
| `g_ctrl_inited` | once per boot |
| `g_ctrl_enabled` | **cycles** — cleared by `radio_off()` |
| `g_nimble_up` / `g_hidh_up` / `g_host_task_started` | once per boot |
| `g_host_up` | cleared by `radio_off()`, re-waited on each bringup |

`radio_off()` closes the link, waits ~100 ms for the disconnect to reach the peer and the
CLOSE event to land, then disables the controller. Reached from every path: toggling off,
a failed connect, and `poll()` acting on the clicker being switched off mid-book.

`g_host_up` must be re-waited after a re-enable: the controller runs a fresh HCI reset when
it comes back, and no GAP call is legal until the host has synced again. The host task is
started once and never deleted — `esp_nimble_disable()` is a bare `vTaskDelete` and the
host is not restartable. A stage-8 failure is no longer terminal: the retry skips to
`esp_nimble_enable` and waits for sync again.

### Heap is the binding constraint, confirmed twice

The second confirmation names the victim:

```
abort() ... std::bad_alloc
  operator new
  std::vector<LayoutWord>::_M_range_insert
  TextLayout::assemble_page   (TextLayout.cpp:827)
  ReaderScreen::prev_page_    (ReaderScreen.cpp:693)
```

**No BLE frame anywhere in it.** The radio took the internal RAM the reader needed to lay
out a page, and with exceptions disabled `bad_alloc` is an `abort()` — a whole-device
restart mid-read, reported as `rst4`. The failure is not in the clicker code and cannot be
fixed there. Either BLE gets smaller or the reader's peak allocation does.

Measured on hardware: **~13 KB free with the radio up and a book open.** Survivable only
because the allocation *spike* below is gone.

**The reader's own allocation spike.** `build_page_items` grew `PageContent::word_pool` by
`insert()` with **no reserve**. A `std::vector` realloc holds the old and new blocks
simultaneously, so a page of a couple of thousand `LayoutWord` (12 bytes each) transiently
needed ~1.5-2x its final size. That spike is what threw `std::bad_alloc`; the radio only
made the headroom small enough to notice. It now does one exact reservation from a pre-pass
over the same cached paragraphs the main loop uses. `rev_items` / `items` in the collect
passes are reserved to 64 for the same reason. **Reserve before growing anything sized by
page content.** This is the highest-value memory fix in the tree and it is invisible until
the heap is tight.

`TextLayout::kCacheCapacity` is **8**, not 16. Each entry is a laid-out paragraph holding
per-line word vectors. A page spans a handful of paragraphs, so the hit rate is effectively
unchanged; below ~6 a backward turn would thrash, since it crosses a paragraph boundary by
definition.

**What the paragraph cache costs**, measured over every page of four books with
`tools/tests/layout_memory_test.cpp`'s instrumented allocator (worst case, the Odyssey):

| `cache_limit_` | worst page peak | resident (cache + page) |
|---|---|---|
| 8 (no radio) | 25,823 B | 73,002 B |
| 2 (radio up) | 27,097 B | 44,944 B |
| 1 | 28,051 B | 44,480 B |

So the cap `release_ram_for_radio()` applies buys **~28 KB resident for ~1 KB extra peak**.
**Do not lower it to 1**: resident stops improving while the peak keeps rising, because a
single slot thrashes on a page spanning two paragraphs. The switch is automatic, keyed on
`IRuntime::clicker_holds_ram()`, so caching runs at full size whenever no radio is up.

**An oversized paragraph is split by the converter, not handled on the device.** The
Odyssey's chapter 2 held a 3,672-byte paragraph that laid out to 116 lines / 683 words =
14,640 bytes resident, inside a page peak of 45,712 B, against ~13 KB free with the clicker
connected. Reading that page aborted the device reproducibly; the allocation that failed
was a **256-byte** `reserve`, i.e. the heap was gone.

A paragraph is the device's indivisible layout unit, so the fix is `kSplitParagraphBytes`
in `WgbFormat.h` and the two splitters in `WgbWriter`:

- `write_split_text_paragraph_` cuts at `<br>` runs where the paragraph has them, and
  **recurses into the sentence splitter** for any chunk still oversized.
- `write_sentence_split_paragraph_` handles running prose with no `<br>`. It cuts only
  after `.`/`!`/`?` (allowing a trailing quote or bracket) followed by whitespace, so the
  break lands where a line break could already fall. **Continuation chunks clear `indent`**
  and carry `kWgbSpacingDefault`, so no fresh first-line indent and no extra gap. A
  paragraph with no sentence boundary in range is left whole rather than cut mid-clause —
  visible breakage is worse than a large paragraph.

`kSplitParagraphBytes` is **700**, not the 1,500 first tried. The binding cost is not one
paragraph — it is `WgbChapterSource`'s **32-slot window**, which holds the raw text of 32
paragraphs at once and so multiplies the *average* paragraph size by 32. On the Odyssey
that window alone measured **26.8 KB**, more than the page and the paragraph cache
combined. The window cannot shrink: a page legitimately spans up to 24 paragraphs
(measured), and cutting it corrupts displayed text. Lowering the average is the only lever.

Verified invisible: concatenating all text from every paragraph, before and after, is
**byte-identical** on all four books. Page counts move by a handful (514 → 516, 629 → 639)
because a split adds a paragraph boundary, a legal break the layout was already free to
take.

| | before | after |
|---|---|---|
| per-page layout peak (Odyssey) | 25,823 B | **13,157 B** |
| total chapter + page peak, cache=2 (Odyssey) | 47,434 B | **32,890 B** |
| largest paragraph (Odyssey) | 3,672 B | 1,404 B |
| paragraphs over the limit | 25 | 5 |

The five remaining have no sentence boundary inside the window.

**Three bugs made the first attempt look like it had done nothing**, all producing the same
symptom — a re-converted book still holding oversized paragraphs:

- `WgbConverter::write_split_paragraph` called `write_text_paragraph` **directly**,
  bypassing `write_paragraph` and the size split entirely. It bounds the *serialized* size
  (run headers dominate in annotated books); the text-size bound lives one level up.
- `write_sentence_split_paragraph_` never flushed when the chunk was already full, so
  `room` went to 0, `sentence_cut_` found nothing, and the whole run was appended to an
  oversized chunk.
- Even with room > 0, a *scrap* of room is as bad as none: chapter 22's paragraph 31 is
  runs of 658 + 5 + 848 bytes, and the third could not be cut inside the 37 bytes left
  over. `sentence_cut_fits_` now tests whether a cut is possible before the run is
  considered, and flushes first if not.

Two consequences worth knowing:

- **`TOC para_index` counts *written* paragraphs, not source ones.** `EpubParser` numbers
  source paragraphs, so once a chapter splits anything the two diverge and every later
  chapter jump lands short. `WgbConverter`'s `id_sink` reads
  `WgbWriter::chapter_paragraph_count()` instead.
- **The magic stays `WGB2`.** The on-disk layout is unchanged — only paragraph granularity
  differs — so old files still open, keeping their oversized paragraphs and staying exposed
  to the abort until re-converted.

Two allocation bugs in `layout_para_lines` are also fixed, and they matter for any book:

- `lines` is reserved from the paragraph's byte count and the font's own `char_width('n')`,
  not a fixed 12. The old doubling chain 12 → 24 → 48 → 96 held both arrays *and*
  move-constructed every heap-owning `LayoutLine` per step. A guessed 6 px/char undershot
  on 930 of the Odyssey's paragraphs, so the estimate uses real metrics; it now undershoots
  twice in ~8,800 paragraphs.
- Every finished line's word vector is shrunk to its real size (in `flush_line`, which
  pairs with all four push sites), and `lines` itself is shrunk before it is returned.
  `reserve(16)` per line is held for the paragraph's whole life otherwise: 21.5 KB to store
  10.9 KB of words.

On their own those took the Odyssey's worst page from 45,712 B to 25,823 B — short of what
BLE leaves free, which is why the converter split is the actual fix. A per-paragraph **line
cap** was considered and **rejected**: it would silently truncate text.

### The OOM rescue, and why it needs its own reserve

`main.cpp` installs a `std::set_new_handler`. With exceptions disabled a `std::bad_alloc`
is an unconditional `abort()` — IDF's `__wrap___cxa_throw` is literally `abort()` — so the
handler is the *only* place an allocation failure can be intercepted. A guard checked
before allocating can always be overtaken by fragmentation between the check and the `new`;
the handler cannot.

**It must have something left to free, and that was the bug.** The handler originally
released the spare framebuffer and the book index. Both are already gone whenever the
clicker is connected — `release_ram_for_radio()` drops the index and `wg_clicker` drops the
spare — so the handler ran, freed **nothing**, and the abort proceeded. Every
clicker-connected crash had a live handler that could not help.

`g_oom_reserve` fixes that: **28 KB claimed at the top of `app_main` before anything else
allocates**, never read, existing solely to be freed by the handler. Sized from the worst
*cold* page layout measured over four books — 24,066 B on the Odyssey, with an emptied
paragraph cache, the state the reader's own guards force under pressure. Claiming it first
also means it is one clean extent.

Three properties that matter:

- **The handler frees the reserve first**, the only block guaranteed to still hold memory.
- **The reserve is re-claimed once per frame**, but only when the largest free block
  exceeds `kOomReserveBytes + 16 KB` — taking it back must not cause the shortage it exists
  to prevent. Without the re-claim the rope is single-use. The re-arm of the handler is
  *not* gated on the reserve being spent: the handler disarms itself on any rescue, and its
  other two levers can fire while the reserve is still held.
- **The reserve must be released before `xTaskCreate`, not inside the worker.** It comes
  out of the same single ~50 KB internal DRAM pool the BT controller needs a *contiguous*
  block from, and holding it broke the clicker outright. Releasing it in `bringup_stack()`
  was not enough — that runs *inside* the worker, so it cannot help the `xTaskCreate` that
  allocates the worker's own 6 KB stack. The release is in `toggle()`, immediately before
  the create.

  The symptom is worth recognising: `toggle()` sets Connecting, the create fails, and the
  state goes back to Disconnected **within the same call**, so the row never repaints as
  "Connecting" and the press looks like it did nothing. A row that stays on Disconnected
  with no intermediate state is a failed task creation, not a failed connect.

  `wg_clicker::holds_ram()` includes `g_busy` so the frame loop cannot re-claim the reserve
  in the window between `toggle()` freeing it and the worker allocating. `wg_sync::start()`
  releases it for the same reason.

**Anything new that reserves internal RAM up front inherits this.**

### The heap fails by fragmenting, not by filling

Every abort in this reader has been a small allocation failing with plenty of total free
heap: 256 bytes, 1,212 bytes, 1,416 bytes. Two things follow, both got wrong first.

**Guard on `largest_free_block_bytes()`, never `free_memory_bytes()`.**
`heap_caps_get_free_size` sums every free byte across a fragmented heap, and IDF's own
header warns that a single block of that size probably cannot be allocated. A 24 KB
*total-free* guard passed and the layout behind it aborted on a 1,416-byte request, on an
ordinary forward page turn. `heap_caps_get_largest_free_block` answers the question the
guard is actually asking. The number to clear is the largest *single* allocation one layout
makes — measured at **5,808 B** worst case — hence `kLayoutMinBlockBytes` = 12 KB and
`kPrerenderMinBlockBytes` = 20 KB.

**Allocation churn is worse than allocation size.** `shrink_to_fit()` on each laid-out line
and on the line vector reclaimed ~5 KB of slack per page and was removed again: it
reallocates (allocate exact, copy, free old), adding ~80 allocate/free pairs per page and
taking the page from **114 allocations to 197**. Many short-lived blocks of varying size is
the definition of heap churn, and churn produces the fragmentation above. 5 KB of headroom
is not worth 40% more traffic on a heap that fails this way. Peak went 13.2 KB → 18.6 KB,
both inside budget. Same reasoning is why `split_words` fills a caller-owned buffer reused
across every run of a paragraph instead of returning a fresh vector per run.

**A runtime guard backs this up.** `ReaderScreen::render_page_` checks
`largest_free_block_bytes()` against `kLayoutMinBlockBytes` immediately before laying out
and, if short, drops the page cache, the pre-drawn page and the paragraph cache and caps
the cache at one slot. Exceptions are off, so an allocation failure inside layout is an
`abort()` with nothing to catch — the check has to happen before allocating. The cost is a
slower page turn; the page still renders. It deliberately does **not** release
`DrawBuffer`'s spare: `render_page_` can be reached from inside an offscreen draw, and
freeing the buffer being drawn into is worse than the abort it would prevent.

**Every path that lays out needs the guard.** There are three, and missing one is not
theoretical — `prev_page_` was left unguarded and scrolling backwards with the clicker
connected aborted on a 1,212-byte allocation inside `split_words`:

| Path | Guard |
|---|---|
| `render_page_` | two stages — `kLayoutShedBlockBytes` (20 KB) drops the page cache and pre-drawn page; `kLayoutMinBlockBytes` (12 KB) also drops the paragraph cache |
| `prerender_next_page_` | `kPrerenderMinBlockBytes` (20 KB) — higher, because the *real* page after it still has to fit |
| `prev_page_` | `kLayoutMinBlockBytes`, and it also skips the forward-resync block entirely |

`prev_page_` is the hungriest: one `layout_backward`, then the resync does another plus up
to three `layout_end` calls and a final `layout()`. When the heap is tight it drops caches
*and* takes `layout_backward`'s answer unrefined, costing the occasional one-line offset
rather than a reboot.

**A forward turn holds two pages at once**, and that is the number to budget against.
`prerender_next_page_` lays out page N+1 while page N is still resident: measured worst case
**25 KB** across four books against ~13 KB for a single page. Two consequences, both bugs:

- Its guard at the top tests the heap *before* the speculative layout exists, so it says
  nothing about whether the subsequent *draw* will fit. It now re-checks after the layout
  and bails, leaving the layout cached — the next turn is a normal render.
- The draw calls `render_page_`, which runs its own guard. Left alone that guard would drop
  the very page cache the prerender just filled, forcing an immediate re-layout with *less*
  room than the check was made against. `in_prerender_draw_` suppresses it for the nested
  call only.

**`DrawBuffer`'s spare is on the heap, not in BSS.** Static RAM went 164,184 → 116,168 with
that one change. It is still allocated at construction and kept for the session — the point
is that the 48 KB sits in the pool the reader and radio draw from.

**It is deliberately never released, and neither are the reader's page caches.** Both were
wired to free on connect and that was wrong: they are what makes a page turn a memcpy
instead of a layout, and a clicker exists to turn pages. Only the book index is handed
back, because being without it while a book is open costs nothing.

**The index is handed back before the radio starts.** `Failed 8011` — the host task could
not be created with 11 KB free — showed the two do not fit alongside a resident index.
`QuickmenuScreen::on_select` calls `Application::release_ram_for_radio()` **synchronously,
immediately before** `toggle_clicker()`, freeing ~30 KB. It must be synchronous: bringup
allocates the moment the worker spawns, so a next-frame release is too late.

`BookIndex::release_memory()` swaps the vector rather than clearing it (a cleared vector
keeps its capacity) and refuses when `dirty_`, since a reload cannot recover unsaved
mutations. An empty index is the "never loaded" state everywhere, so nothing truncates the
file.

BLE's allocations all come from `MALLOC_CAP_INTERNAL`
(`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL`), on a build already holding ~160 KB statically
out of 320 KB. Not a stack-size problem, and raising stacks makes it worse. Every `BT_*`
buffer key in `sdkconfig.defaults` is sized down from IDF's defaults, which assume a
multi-peripheral hub rather than one clicker sending 8-byte reports — each commented in
place. `BT_CTRL_BLE_MAX_ACT` (6 → 2) is the largest single saving.

`bringup_stack()` **refuses to start below `kMinHeapKb` (24)**, turning the worst failure
mode — half initialise, time out at sync, leave a session that cannot retry — into a clean
refusal. It is a floor for the hopeless case, not an estimate of what BLE needs: it shipped
at 64, above what the device actually had free, and refused every attempt on hardware that
had been connecting fine.

**Any early bail-out from `bringup_stack()` reaches `radio_off()`**, via `connect_task`'s
`if (!ok)`. That is why `radio_off()` guards `ble_gap_disc_cancel()` behind `g_host_up`:
calling it before `esp_nimble_init()` dereferences a null `ble_hs_ctx` inside
`ble_hs_is_enabled()` and takes a **load fault, not an error return**. The heap check
introduced exactly this — the device rebooted the instant the row was selected, before
"Connecting" was drawn. Any new pre-NimBLE failure stage inherits the same hazard.

## NAS book sync

One button on the book list: pull newly converted books off the homelab and retire finished
books. `platforms/esp32/wifi_sync.h` (header-only, `#ifdef WG_WIFI_SYNC`), reached through
`IRuntime::start_sync()` / `sync_state()` so `MainMenu` stays portable.

| | Without | With | Δ |
|---|---|---|---|
| flash | 692,342 | 1,213,488 | **+521 KB** |
| static RAM | 115,576 | 132,920 | **+17.3 KB** |

`CONFIG_MBEDTLS_TLS_ENABLED=n` is part of that: `esp_http_client` drags in esp-tls and
mbedtls **even for a plain-HTTP build**, and turning the client half off saved 36 KB. Image
validation still needs the SHA code, so mbedtls does not leave entirely.

**HTTP, not SMB, a deliberate reversal of the original request.** There is no SMB client in
ESP-IDF. The choices were vendoring `libsmb2` (~60-80 KB, a third-party dependency) or
hand-rolling SMB3, which the homelab pins (`server min protocol = SMB3`) and which
therefore **signs every packet** — AES-CMAC with the radio held open, on a device where one
second awake costs more than half an hour asleep. SMB also has no "give me the diff"
primitive: it is a listing plus a stat per book. The Pi already runs bun on port 80, so one
POST carries the whole negotiation. Samba stays — it is still how EPUBs get *onto* the Pi.

**mDNS is not used and should not be added.** `esp_mdns` is a managed component, not part of
IDF, so it would mean a component-manager fetch at build time. Resolution is: cached IP from
NVS → literal IP in config → `getaddrinfo`.

### What a sync costs

| Network | |
|---|---|
| nothing changed | one POST + response, a few hundred bytes |
| N new books | + N × (`book.wgb` + up to 3 covers), **one** TCP connection |

One round trip decides everything (no per-file HEAD or stat), one kept-alive
`esp_http_client` handle covers the POST and every GET, and the reply carries only what the
device lacks.

| SD card | |
|---|---|
| nothing changed | **zero bytes** |
| N new books / deletions | the book files + **one** index save |

Guaranteed by two things, both of which have a tempting wrong version:

- **Never call `BookIndex::index_file()` / `remove_path()` / `rename_in_place()` here.**
  Each ends with its own `save()`, so one call per downloaded book rewrites the whole ~30 KB
  index per book. The sync uses the in-memory `remove_entry()` / `add_entry()`, which only
  set `dirty_`, and saves once at the end.
- **Downloads stream to `.tmp` then rename** (`remove()` first — FatFs `f_rename` does not
  replace). An interrupted download leaves a `.tmp`, never a truncated `book.wgb` that the
  reader would happily open.

### Reading positions are not synced — removed, don't re-add

`book.pos` is device-local. The sync carries `have` / `done` up and `get` / `delete` back,
nothing else. **The whole position half was built, shipped and deleted** because it was
buggy in practice and bought nothing: the reader is only ever open on one device, so there
is no second reader to merge with.

Gone with it: the wire `pos` object, `parse_pos_object_` / `PosEntry` / `read_pos_` /
`write_pos_` in `wifi_sync.h`, `state.pos` / `ahead()` / `dropPos()` / `handleDropped()` and
the `POST /booksync/dropped` route, and the `curl` call in `scripts/convertBooks.fish`.
`.sync.json` now holds only `finished`.

`progress_pct` still crosses the wire in exactly one place — `>= 100` as the "finished"
predicate in `done[]`, where the uint8 rounding that made it useless as a merge key cannot
matter because there is nothing past the end.

If it is ever wanted back: the merge key must be the four-number position tuple compared
lexicographically, **never `progress_pct`** (a uint8, so one percent spans four pages on a
400-page book and two genuinely different positions compare equal); the reply must carry
positions for books in `get` as well as `have` (a fresh card's `have` is empty, so iterating
it alone sent nothing and a restored device started every book at page one); the
currently-open book must be skipped, since `ReaderScreen::stop()` writes `.pos` on close and
would put the stale in-memory position back over it; and a re-converted book's stored
position must be dropped server-side, because the tuple indexes into a specific `book.wgb`.

### Finished books are deleted only after the server confirms

Ordering is the entire safety property:

1. device sends `done[]` (books at `progress_pct >= 100`)
2. server appends to `finished` in `.sync.json` and **fsyncs**
3. only then does the server unlink, and reply with `delete[]`
4. only on seeing `delete[]` does the device unlink

A crash at any step leaves the book present somewhere. `finished` is append-only and never
pruned — it is the permanent read history, deduplicated on `title|author`. A book at 100% is
re-sent every sync until one succeeds, which is idempotent server-side and cheaper than
tracking "already reported" state.

### The radio must be off when a sync ends, however it ends

Teardown is bound to **scope destruction** (`WifiGuard`), not to control flow, so a `return`
added later cannot leak it. Exceptions are off, so early returns are the only path that
matters. This is the requirement most likely to regress silently: a stuck radio is tens of
mA against a ~15 mA idle and is invisible except on a meter.

**Power save is deliberately OFF** (`WIFI_PS_NONE`). `MIN_MODEM` saves current while
associated and *idle*, which this workload never is — it connects, transfers flat out, and
leaves. Parking the radio between DTIM beacons adds latency to every round trip and
**extends** total radio-on time. Same reasoning keeps `AMPDU_RX` enabled. The energy model
is "finish and shut down", not "sip while connected".

#### Internal RAM is the binding constraint, and the framebuffers are why

`esp_wifi_init()` wants **roughly 50 KB of `MALLOC_CAP_INTERNAL`**, and it does not crash
when it cannot get it — it returns an error, the worker bails, and the row flicks back to
`Sync` with nothing said. Measured during bring-up: **20 KB free**, so it failed every time.

`DrawBuffer` holds `bufs_[2][kBufSize]` — **96,036 bytes of BSS**, the largest symbol in the
image — plus `spare_`, another 48 KB on the heap. 144 KB of framebuffers on a 320 KB part.

So `wg_sync::start()` calls **`DrawBuffer::release_spare()`**, handing the 48 KB back and
taking it again when the sync ends (on every path, including a failed `xTaskCreate`). Safe
because the spare is purely an optimisation — without it a quick-menu dismiss re-renders and
a page turn re-draws, and the sync screen does neither. Both claim paths already null-check.

`release_spare()` must also reset `spare_use_` and `draw_target_`: `has_snapshot()` /
`has_offscreen()` test only that enum, so leaving it set would let `restore_snapshot()`
memcpy from a freed pointer.

**The book index is deliberately not released here**, unlike the clicker path.
`MainMenu::entries_` holds `StringRef`s into `BookIndex`'s `StringPool`, and that screen is
*on display* when Sync is pressed — dropping the pool left every title and author resolving
to an empty `string_view` while the list stayed navigable and kept showing reading
percentages (`uint8_t` copies). It read as a drawing glitch rather than a dangling
reference. It also bought nothing: `run_sync_()` reloads the index from disk as its first
act.

#### A 200 is not proof you got the file — encode the URL

Book directory names contain spaces (`the odyssey`), and three things had to be wrong
together to produce a book that synced, appeared in the library, and would not open:

1. **The device sent a raw space in the request line**, not a valid URL. `url_escape_()`
   percent-encodes each path segment now.
2. **The server did not decode before validating.** `URL.pathname` keeps the `%20`, and
   `safeName()` rejects `%`, so every book file 404'd — and because the bun router falls
   through to the dashboard for unmatched paths, that 404 arrived as **200 + Content-Length
   + HTML**. `handleBookFile` decodes first, then validates, so an encoded `..%2f` still
   cannot slip a traversal through.
3. **Both ends accepted it.** The device checked only the status code, so it wrote the
   dashboard HTML to the card as `book.wgb` — all four files landed at an identical 9,486
   bytes, the tell. `http_get_file_()` now also requires the body to match the declared
   `Content-Length`.

The last link made it look like a reader bug rather than a transfer bug: `run_sync_()`
opened the downloaded WGB for its title, **ignored the failure**, and indexed it anyway with
the folder name as the title. A book that `WgbReader::open` rejects is now deleted and never
indexed, which also means the next sync re-offers it.

#### `add_entry` / `remove_entry` do not bump `generation()`

Deliberate — they are the in-memory mutators the sync batches so a dozen books cost one
index write — but nothing then tells the screens anything changed. `run_sync_()` calls
`BookIndex::bump_generation()` once, next to its single `save()`. Without it a freshly
synced book does not appear until you navigate away from the list and back.

#### Two connect bugs that both looked like "it fails instantly"

- **Never bound the connect by a retry count.** A rejected association comes back in
  *milliseconds*, so a budget of two retries was spent before the radio had meaningfully
  tried. `wifi_event_` reconnects for as long as `kConnectTimeoutMs` (20 s) allows, and that
  timeout is the only bound.
- **Never pin a cached BSSID/channel on the first attempt.** If the AP moved channel, every
  retry re-attempted the *same* pinned config and failed identically, never falling back to
  a scan — indistinguishable from a wrong password. The hint is still cached for
  `resolve_server_`, but association always starts with a normal scan.

**BLE goes down first and does not come back on its own.** One radio, and ~13 KB free with
the clicker up leaves no room for Wi-Fi's ~40-50 KB. `start()` calls
`wg_clicker::radio_off()` and `Application::release_ram_for_radio()` **synchronously before
the worker task spawns**. The clicker is re-armed by pressing its quick-menu row again.

**Auto-sleep is held off while a sync runs.** `wg_sync::poll()` calls `keep_awake()` from the
main loop; a multi-book transfer easily outlasts `kAutoSleepMinutes` and sleeping mid-download
would cut the radio with files half written.

### The server side

In the flake repo, not here: `hosts/alechomelab/webserver/booksync.js` bolted onto the
existing bun server (no new service, no new port), plus `hosts/alechomelab/books.nix` — a
systemd `.path` unit watching `/media/books` that converts any new EPUB into
`.compiled/<stem>/` with `epub2wgb`, staged in a temp dir and moved into place so a
half-written book is never visible to a concurrent sync.

`epub2wgb` is packaged by **this** repo's `flake.nix` and consumed as
`inputs.wintergreen.packages.aarch64-linux.epub2wgb`. Its `CMakeLists.txt` has no
`install()` rule, so the derivation supplies its own `installPhase`.

Endpoints: `POST /booksync` (the whole negotiation) and `GET /booksync/<dir>/<file>` (one
book file). Book directory names are validated against `/^[A-Za-z0-9 ._-]+$/` and rejected
if they contain `..` — they are used as path segments.

One bug worth not repeating: **`DirectoryNotEmpty` is a level, not an edge.** It re-triggers
for as long as the watched directory has *anything* in it, so the service restarted the
instant it finished, forever, until systemd's rate limiter killed it with `start-limit-hit`
— **part-way through the loop**, so the alphabetically last book was never converted and
nothing said so. It is `PathChanged` now (fires once per upload), plus a `startLimitBurst`
backstop so a future retrigger loop fails loudly instead of spinning.

Book names contain spaces, so anything built on them must encode them.

Two host-side tests worth keeping green:
`hosts/alechomelab/webserver/booksync.test.js` (the negotiation, including the
confirm-before-unlink ordering) and `tools/tests/json_scan_test.cpp` (the device's
hand-rolled JSON scanner, under ASan/UBSan, including every truncation of a valid reply).

## Serial protocol

`platforms/esp32/serial_communication.h` runs a FreeRTOS task (`serial_rx`, 8 KB stack) over
USB-JTAG with 2 KB tx / 4 KB rx driver buffers — about 14 KB of RAM, allocated only once a
host is attached.

**It is a file-management channel only; there is no debug console.** Live commands are `A`
(dir list), `B` (inject buttons), `K` (mkdir), `L` (list books), `N` (rename), `O` (open
book), `R` (recursive delete), `T` (read file), `W` (upload), `Z` (clear `/sdcard/sleep/`).
Frame magics are `EPUB`, `SIMG` and `CMND`.

Add / Rename / Remove do not execute in the receiver task: they are handed to `main.cpp`
through the single-slot `g_index_op` SPSC slot and applied there, so they can be deferred
while the reader owns the scratch buffers or the panel is mid-refresh.

**The receiver task starts lazily.** `serial_start_if_connected()` is called from the main
loop the first time `usb_serial_jtag_is_connected()` returns true, and is a no-op afterwards.
On battery none of the ~14 KB is ever allocated and the task never exists to wake the CPU on
its 50 ms read timeout. `usb_serial_jtag_is_connected()` reads the peripheral's state
directly and does not need the driver installed, so polling it is free.

## No logging

There are **no log statements anywhere in the tree** — no `ESP_LOGx`, no `MR_LOGI`, no
`HEAP_LOG`, and `wintergreen/HeapLog.h` is deleted. Don't reintroduce them casually, and if
you add one temporarily, remove it again.

Two things worth knowing before assuming this bought flash:

- With `CONFIG_LOG_DEFAULT_LEVEL_NONE` the macros already expanded to nothing — the format
  strings were **not** in the image (verified by searching `firmware.bin`). The removal is a
  source-cleanliness change; the measurable savings came from the *diagnostic commands* and
  the timing scaffolding that fed the logs.
- Removing a log whose call was the entire body of an unbraced `if`/`else` silently re-points
  that branch at the next statement. That happened in nine places here and only two were
  caught by the compiler. If you ever strip logs in bulk again, scan the *original* source
  for a control statement whose next non-blank line is a log macro, rather than trusting
  warnings.

## Assets are flashed, not embedded

Neither the reader font nor the sleep image is in the app image. Both are raw partitions
declared in `platforms/esp32/default_16MB.csv`:

| Partition | Offset | Size | Holds |
|---|---|---|---|
| `font` | `0xC90000` | 512 KB | the inflated WGFS v2 bundle, 428,289 B |
| `sleep` | `0xD10000` | 128 KB | `resources/sleep.mgr` verbatim, 96,008 B |

`tools/generate_assets.py` runs pre-build. It inflates `resources/AtkinsonHyperlegible.bin`
(which is `[uint32 raw size][zlib]`) **on the build machine**, copies `sleep.mgr` unchanged,
writes both to `$BUILD_DIR`, checks each against its partition size, and appends them to
`FLASH_EXTRA_IMAGES` so `pio run -t upload` writes all four images in one esptool
invocation. It rewrites an image only when the bytes change.

At runtime `RawPartition::map()` (`platforms/esp32/font_partition.h`) is the whole story:
`esp_partition_find_first` + `esp_partition_mmap`, no header, no CRC, no copy, no inflate.
`FontManager::init()` parses the WGFS table straight out of the mapping; `main.cpp` hands the
sleep mapping to `DrawBuffer::set_sleep_image()`.

This replaced a provisioning path that inflated the font out of an embedded ASTS blob into
the partition on the first boot after every firmware update — roughly 15 s of flash
erase-and-write behind an "Installing fonts..." progress bar, with the font stored twice.

**Adding an asset means editing three places in step**: the CSV, `PARTITIONS` in
`generate_assets.py`, and the `map()` call site. The Python asserts the image fits, so a
mismatch fails the build rather than the device.

The font partition holds the **inflated** bundle: 428 KB, not the 122 KB compressed form.
Flash is not scarce and the device does no work — that trade is the point.

## Measured size budget

app0 is 6.4 MB against a ~1.2 MB image, so **flash is not scarce** — size work here buys
upload time and instruction-cache pressure, nothing else. Both radios are opt-in and they
stack:

| Build | image | static RAM |
|---|---|---|
| neither radio | 430,790 | 159,820 |
| BLE only | 728,888 | 164,128 |
| **BLE + Wi-Fi sync** (the configured default) | **1,213,008** | **132,944** |

The RAM figure being *lower* than the BLE-only row is not an error: `DrawBuffer`'s spare
moved from BSS to the heap in between, taking 48 KB of static RAM with it.

Current sections, default build: `.flash.text` 988,598, `.flash.rodata` 143,624,
`.iram0.text` 69,872, `.dram0.bss` 122,760. Roughly 245 KB of `.flash.text` is IDF, picolibc
and the C++ runtime; ~51 KB is the five screens; everything else of ours is under 20 KB
apiece.

**Nothing left in the IDF share is worth arguing with.** FatFs, SD, SPI, power management,
FreeRTOS and the heap allocator are all load-bearing. The few genuinely dead pieces —
`f_mkfs` (2,360 B, unreachable but referenced unconditionally) and
`__d_vfprintf`/`__d_vfscanf` (~5 KB) — are reachable only through IDF code, so removing them
means patching the framework in place.

rodata is dominated by the three UI fonts (54,138) and the hyphenation trie (26,943) —
together 87% of it. Both could move to a flash partition like the reader font did (mmapped
flash and rodata are both XIP, so it would be free at runtime), but the image is not under
pressure.

Re-measure rather than trusting any of this:

```
~/.platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-size -A \
  .pio/build/esp32c3/firmware.elf
~/.platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-nm \
  --print-size --size-sort --radix=d .pio/build/esp32c3/firmware.elf | tail -40
```

**`-Os` was tried on hardware and rejected — don't re-open it.**
`CONFIG_COMPILER_OPTIMIZATION_SIZE` takes `.flash.text` from 346,482 to 243,666 (30%
smaller), and page turns came out the same or *slightly slower*, never faster. The theory was
that the C3's 16 KB instruction cache — fixed in silicon, not configurable as on the S3 —
would favour smaller hot loops staying resident. In practice `-Os` gives up the unrolling and
inlining that make `draw_glyph_impl_`'s bit blitting and `word_width`'s per-character scan
fast, and the cache does not pay it back. `CONFIG_COMPILER_OPTIMIZATION_PERF` (-O2) stays.

Note what the size number was worth here: nothing, for the reason in the first paragraph.
That is why it could never settle the question and why it took a device to.

## Content pipeline

EPUB → WGB (a preprocessed binary layout format) → rendered pages. There is no on-device
conversion at all, so `tools/convert-books.sh <epub-dir> <out-dir>` is the only way a book
gets made. It builds and runs `tools/epub2wgb`, which reopens each output with `WgbReader` —
the same code the device runs — so a file that would not load is reported at conversion time.

**Images are rasterised by the converter, not decoded on the device.** `WgbImageRef` is
`{data_offset, data_size, width, height}` (12 bytes) addressing a raw 1-bit bitmap inside the
WGB: `(width+7)/8` bytes per row, MSB first, bit clear = ink. `data_size` is always exactly
`(width+7)/8 * height`, worth asserting if you touch the writer.

The converter decodes the source JPEG/PNG, scales it to fit `kWgbImageBoxW × kWgbImageBoxH`
(480 × 772 — the portrait panel width and the page height less its vertical padding) and
dithers it, all on the build machine. The device seeks and blits rows. Consequences:

- **`width`/`height` in the table are the drawn dimensions**, not the source's, because
  `scale_image()` never enlarges. In portrait the blit is 1:1; a landscape page, whose
  content area is wider, centres the stored bitmap rather than stretching a dithered image
  with no detail to recover.
- **The image table beats the EPUB's declared width/height.** `resolve_inline_image()` asks
  `make_image_size_query()` first and falls back to `attr_width`/`attr_height` only when
  there is no raster.
- **`ReaderScreen::draw_image_` reads through `WgbReader::file()`**, the handle the book is
  already open on, so a page with three figures does not open the book three times. Every
  `WgbReader` read seeks first, so borrowing the handle cannot disturb its position.
- There is **no image cache on the card**.

Measured on The Hobbit: image bytes 2,077,395 → 423,652 (−80%), the file 2.71 MB → 1.06 MB
(−61%), and `JpegDecoder`, `PngDecoder`, `ImageDecoder`, `ZipReader`, `stb_image` and miniz
all left the firmware build (~24 KB of flash, plus their heap and scratch use). An image page
now turns at the speed of a text page.

`WgbReader::open()` reads the chapter and image tables with **one `fread` each** and parses
from memory, and `WgbChapterSource`'s constructor does the same for the per-chapter paragraph
descriptor table. One `fread` per entry costs a FATFS + SPI round trip apiece and a long book
has hundreds of each. The spine-filename and anchor tables are **gone from the format**: both
existed only to resolve hyperlinks. Dropping them also removed the anchor `.tmp` file
`WgbWriter` streamed during every conversion, and the header's `anchor_offset` with it (28 →
24 bytes).

Two things about the converter that matter on a slow machine (it is meant to run on a
Raspberry Pi):

- **It converts books in parallel**, one per hardware thread. Each conversion owns its `Book`,
  its two 48 KB scratch buffers and its own output directory, and the pipeline holds no
  mutable global state, so only the reporting needs a mutex. Output is byte-identical to the
  single-threaded result.
- **The script only rebuilds when the binary is stale.** Converting three books takes 0.15 s;
  a cold compile of the converter is ~18 s here and minutes on a Pi, so the build, not the
  conversion, was what you waited for. `needs_build()` compares the binary's mtime against
  every source it is built from.

The converter links only the content pipeline. `WgbConverter` includes `DrawBuffer.h` purely
for the `kWidth`/`kHeight` constants.

Output is one folder per book:

```
<out>/<book name>/book.wgb
<out>/<book name>/cover.bin        # 160x240, book list
<out>/<book name>/cover_home.bin   # fitted to 424x479, home carousel
<out>/<book name>/cover_sleep.bin  # 480x786, composed for the panel (bars included)
```

The three cover files are written only when the EPUB has a cover. The `book.wgb` filename is
not arbitrary — `convert_epub_to_wgb_streaming()` looks for the literal string `book.wgb` in
the output path and writes the covers beside it, so renaming the output breaks cover
extraction silently.

**Finding the cover in the EPUB is three fallbacks deep**, and it has to be. `EpubParser`
used to accept only `<meta name="cover" content="ID">` in the OPF metadata, which EPUB 3 does
not require and plenty of EPUB 2 files never wrote — those books produced *no* cover files at
all, so the carousel drew a placeholder outline and the sleep screen showed the wordmark,
with nothing saying why. It now falls back, in order, to a manifest item whose `properties`
contains `cover-image` (the EPUB 3 spelling), one whose `id` is literally `cover`, and one
whose href basename starts with `cover`. All three are restricted to image media types, and a
meta-declared cover always wins.

The fallbacks can only find what the book has: a "cover" that is really a rendered title page
of body text dithers to a near-blank panel, and that is the file doing its job, not a decode
failure.

### The sleep cover

`show_book_cover_sleep_` in `Application.cpp` has **nothing left in it but a blit**.
`cover_sleep.bin` is composed by `Book::write_sleep_cover_bin` at exactly `kSleepCoverW` ×
`kSleepCoverH` (480x786 — **not** 800) with the artwork already trimmed, scaled, centred and
its letterbox bars filled, so the device streams it a row at a time straight onto the panel —
no 48 KB buffer, no scaling, no tone measurement. A file of any other size is from an older
conversion and is **ignored**; the wordmark shows instead.

What the converter does, and why:

- It decodes with `scale_to_fill`, so a small cover is scaled up from the *grayscale* and
  dithered once, rather than dithered small and pixel-replicated.
- It **crops fully blank rows and columns**. Publishers bake white bands into cover artwork,
  and stretching those to the panel only makes them bigger.
- It then **stretches the artwork to the full panel — aspect ratio is not preserved.**
  Deliberate. Fitting inside the panel leaves letterbox bars, and on a 1-bit panel there is no
  good colour for them: white bars under a dark cover look like a bug. The alternative shipped
  briefly — measure the ink coverage of the artwork edges each bar abuts and fill the bars to
  match — and is **gone**, along with `edge_coverage_` and `fill_band_`. Covers sit close
  enough to the panel's 480:786 that filling it outright reads as full-bleed, and there is no
  bar geometry left to get wrong.

Nothing is cropped off the artwork; it is stretched, not filled by cropping.

**The panel's app-space height is 793, not 800.** `DisplayFrame::kPhysicalWidth` is 793 and
`kPanelOffsetX` is 7 — the glass hides 7 columns at the *leading edge only*, not split across
both — so in portrait `DrawBuffer::height()` is 793 and `width()` is 480. 7 + 793 = 800
exactly, so every panel column is reachable by drawing code.

This was 10 + 786 for a long time, which left **4 columns nothing could write**. They kept the
framebuffers' `0xFF` init and showed as a white bar along the top in portrait — invisible on
light content, obvious against a dark book cover, and visibly filling in during a full
refresh. Determined on hardware by dropping the offset to 7 alone: the bar moved *wholesale*
to the trailing edge rather than splitting in two, which proves the hidden columns are all at
one end.

The 480 axis (portrait width) is `kPhysicalHeight`, has no offset and no hidden pixels.

Both cover constants are derived from the height, and both were once written against 800:
`kSleepCoverH` by 14 px, which made *every* sleep cover fail the exact-size check and fall
back to the wordmark, and `kHomeCoverH` by the same 14, which silently put every home cover
through the box-filter-and-re-dither path it exists to avoid. `Application.cpp` now
`static_assert`s the sleep constants against `DrawBuffer::kWidth`/`kHeight`; the home box
still depends on font metrics and has to be re-derived by hand if the layout moves.

## File formats: WGB and WGF

Two binary formats, both **magic-as-version**: there is no version field in either. An
incompatible layout change changes the four magic bytes, so a stale file is rejected by the
existing magic check rather than misparsed by a reader trusting a version number it does not
understand. Bump the digit when the layout changes, and re-convert.

| | Magic | Holds | Defined in |
|---|---|---|---|
| Book | `WGB2` | one book: paragraphs, chapter/image tables, metadata, TOC, image bitmaps | `content/wgb/WgbFormat.h` |
| Font | `WGF1` | one font at one pixel size, up to four styles | `content/WgfFormat.h` |
| Font set | `WGFS` | a count, a size table, and N `WGF1` blobs | `tools/make_font.py` `pack_bundle()` |

They were MRB/MBF4/FNTS. The rename came with three simplifications, all of which mean any
file produced before it must be re-converted:

- `WgbHeader` lost its `version` and `flags` fields (32 → 28 bytes) and later its
  `anchor_offset` (28 → 24). The magic went to `WGB2` with that second change.
- `WgfHeader` lost its `version` byte: 50 → 49 bytes. **Every absolute offset stored in that
  header shifted down by one** — `bitmap_data_offset`, the three style offsets,
  `kerning_offset` and the two gray-plane offsets. Per-glyph `bitmap_offset` is relative to
  `bitmap_data_offset`, so it did not move. Getting this wrong produces a font that parses and
  renders garbage, which is why `tools/check_font.py` renders a sample string.
- The `WGFS` bundle lost the version byte and the 32-byte font name. The name was never read —
  there is one reader font, found by *partition*, not by name — so `--name` is a diagnostic
  label only.

`resources/AtkinsonHyperlegible.bin` is a `WGFS` bundle behind a `[uint32 uncompressed
size][zlib stream]` wrapper. The three `display/ui_font_*.h` headers are bare `WGF1` blobs
emitted as C arrays (`kFontData_ui_*_wgf`).

### Sleep images (MGR2)

`resources/sleep.mgr`, flashed verbatim into the `sleep` partition and read straight out of
the mapping by `DrawBuffer::Mgr2Source_` — there is no file-backed variant, so fetching a row
is a pointer add.

```
"MGR2"   4 bytes
width    uint16 LE      height   uint16 LE
pixels   2 bits each, row-major, stride = ceil(width / 4), MSB-first:
         value = (row[x / 4] >> (6 - (x % 4) * 2)) & 3
         0 = white, 1 = light, 2 = dark, 3 = black
```

Only 4 levels exist because the panel shows it with a one-pass grayscale refresh that takes
bit 0 of each value into BW RAM and bit 1 into RED RAM. The image is blitted at native size
into physical panel coordinates with **no scaling and no centring**, so it must be exactly
800x480 (landscape).

```
nix-shell -p 'python3.withPackages(ps: [ps.pillow])' --run \
  'python3 tools/make_sleep_image.py cover.png resources/sleep.mgr'
```

It fits and letterboxes onto an 800x480 white field by default. `--dither` gives
Floyd-Steinberg for photographs; plain 4-level quantisation (the default) is better for line
art, logos and text.

## Books on the card

Two shapes are recognised, both found by `BookIndex`'s recursive scan (which skips
dot-directories, so the state files and any `.hidden/` folder are never picked up as books):

- `<book name>/book.wgb` — a converted book, opened **in place**. Metadata comes from the WGB
  itself. Three covers sit beside it. **All** must be written by the converter — the device
  cannot generate any of them, because there is no EPUB on the card to extract from. Each is
  optional at read time: every consumer falls back to a smaller one. `book.pos` is written
  beside them when the reader closes.
- `<name>.epub` — a source, converted on demand into `.wintergreen/cache/<stem>/`.

Two traps, both hit during the switch to converted books. `cover_bin_path()` derives from the
file *stem*, and every converted book is literally named `book.wgb`, so all of them mapped to
`cache/book/cover.bin` and collided — it now returns the WGB's sibling instead. And
`ensure_cover_bin()` must return early for a WGB: there is no EPUB to extract from.

A third, from the MRB → WGB rename: **that "is this a converted book" test is a literal
extension comparison**, and `CoverPaths.cpp`'s copy was still matching `.mrb`. Every book
therefore took the EPUB-cache branch and every cover path resolved to one nonexistent shared
path. No cover appeared anywhere, and nothing logged a thing: a missing cover file is a
supported state everywhere it is read. **If covers vanish library-wide, check `is_wgb_`
first.**

`BookIndex` is a singleton holding the scanned book list plus reading stats, backed by a
`StringPool`. `MainMenu` rescans whenever the loaded index is **empty**, not just when it
fails to parse: a valid-but-empty index is indistinguishable from "never scanned", and
treating it as authoritative meant books added to the card were never discovered. It has a
`generation()` counter — MainMenu polls it so serial uploads/deletes refresh the visible list
without navigating away.

**`StringPool::get` is O(chunks), and callers must not treat it as free.** It walks the chunk
list to resolve an offset. Chunks are 8192 − 64 bytes (**deliberately not a power of two**:
`std::string::reserve(n)` allocates n + 1 for the NUL terminator, so 8192 asks for 8193 — one
byte past the block it would otherwise fit exactly, and on a fragmented heap that fails with
plenty free; opening the Odyssey aborted in `StringPool::add` with 30,828 bytes free and a
largest block of exactly 8,192). A 250-book library is a handful of chunks, so a per-row
`get()` is fine, but a per-*character* one is not.

**Views returned by `StringPool::get` are not NUL-terminated.** Strings are packed back to
back, so a loop bounded by `*p` rather than by the view's `size()` runs into the next book's
title. That was a real read-overrun in every list screen's label-truncation loop, confirmed
under ASan; `fit_prefix()` is bounded by length and is now the only place that scans a label.

## Fonts

Two fonts, both compiled from TTF by `tools/make_font.py`, and **no way to add a third** — the
serial `FONT` partition upload and `SDFN` (`/sdcard/fonts/`) sideload paths were deliberately
removed.

- **Reader: Atkinson Hyperlegible**, 5 sizes (20/24/28/32/36 px), all four styles, built
  `--mono`. Lives in the `font` flash partition, written by esptool at upload time and mmapped
  at boot. A font change takes effect on the next upload.
- **UI: Iosevka Slab Medium**, built `--mono`, one size per header in `display/ui_font_*.h`
  (14/24/32 px → small/large/header). There is no medium *size*. It was Bold until the weight
  was dropped — Bold at 14 px thresholds into near-solid blocks, and the mono hinting is what
  keeps Medium's thinner stems from breaking up. Dropping the gray planes roughly halved each
  header (small: 20,332 → 10,099 bytes).

`ReaderSettings::kFontSizeNames` and `kNumFontSizePresets` **must** mirror the `--sizes` list
passed to `make_font.py`, in order. They had drifted to eight entries reading
20/24/26/28/30/32/34/36 against a five-size bundle, so the quick menu labelled the third
preset "26" while rendering 28 — every size above the second was mislabelled. Indices 5–7 were
unreachable only because `QuickmenuScreen::on_select` clamps the cycle to
`font_set()->num_fonts()`; `Application::load_settings_` had no such clamp and now checks
`kNumFontSizePresets` rather than the array capacity `kMaxFontSizes`.

WGF1 format is documented in `content/WgfFormat.h`, but three things that matter are **not** in
that header and were recovered by decoding the old shipped Literata — `make_font.py`'s
docstring is the real reference:

- `MbfGlyph::advance_width` is in **quarter-pixels**, not pixels.
- Antialiasing is **5** levels, not 4, encoded across the three planes as `(bw, msb, lsb)`:
  white `(1,0,0)`, light `(1,0,1)`, mid `(0,1,0)`, dark `(0,1,1)`, black `(0,0,0)`. The BW
  plane draws where its bit is **clear**; the gray planes draw where theirs is **set**. So
  `(msb,lsb)` is a lightening modifier, not a magnitude.
- The reader font asset is `[uint32 uncompressed size][zlib stream]` wrapping a WGFS v2
  bundle: `[WGFS][num][ver=2][pad:2][name:32][num × uint32][WGF1...]`.

The gray planes are **optional** — `gray_lsb_offset` / `gray_msb_offset` of 0 mean absent,
which is what `has_grayscale()` keys off. The reader font is built that way, via `--mono`:
`FT_LOAD_TARGET_MONO`, no gray planes.

That followed from the reader drawing only `GrayPlane::BW`. The BW plane is not a 50%
threshold — `LEVEL_BITS` puts level 2 (37.5% coverage) in the ink plane, so BW-only text was
fattened by a ring of barely-covered edge pixels, and `FT_LOAD_TARGET_LIGHT` left stems
straddling pixel boundaries, so that ring was uneven from glyph to glyph. Mono hinting snaps
stems to the grid: same total advance widths, uniform stems, clean curves. It also cut the
asset from 296 KB to 122 KB compressed.

Don't "fix" the AA path to match — the 5-level encoding and its 37.5% threshold are correct
*for grayscale output*, where the lightening pass pulls those edge pixels back. They only
misbehave when the gray planes are thrown away.

`tools/check_font.py` parses a bundle or bare WGF1 exactly the way `BitmapFont.h` does and
ASCII-art renders a sample string. Run it after regenerating a font — a structurally broken
font renders blank on device with no error. Its header line reports `gray=yes|no`, the quick
check that `--mono` took effect.

Glyph coverage is per-font and sparse. A font that parses fine can still be missing the glyphs
for a given string, in which case it renders blank rather than falling back — check
`glyph_data(cp).bits != nullptr` before assuming a font can render text. (This bit the old
brand wordmark font: it contained only `n`, `o`, `u`, `s`.) Both fonts cover the same 527
codepoints, but Atkinson genuinely lacks 224 of them (most of Latin Ext-A and General
Punctuation). `make_font.py`'s `SUBSTITUTES` table maps the ones that would actually hurt onto
near-equivalents (U+2010 hyphen → ASCII `-`, and so on); the rest are blank. Extend that table
rather than accepting a blank glyph. There is **no wintergreen logotype font** — the wordmark
is drawn in the regular UI font.

Glyph lookup itself is a binary search over the WGF1 range table
(`BitmapFont::find_glyph_index_in`), so `char_width` is O(log ranges) and needs no cache.

Regenerating (needs freetype-py + fontTools, absent from the PlatformIO interpreter, hence
nix-shell):

```
nix-shell -p 'python3.withPackages(ps: [ps.freetype-py ps.fonttools])' --run '
  python3 tools/make_font.py bundle --name AtkinsonHyperlegible --mono \
    --sizes 20,24,28,32,36 --line-height 150 \
    --regular …-Regular.ttf --bold …-Bold.ttf \
    --italic …-Italic.ttf --bold-italic …-BoldItalic.ttf \
    --out resources/AtkinsonHyperlegible.bin'
```

`--line-height` matters: the reader multiplies `y_advance` by the user's setting, so a font's
native leading decides how that setting feels. Atkinson's own metrics are 1.25× the em where
Literata's were 1.55×, which reads cramped; 150% restores it. The extra leading is split above
and below the text rather than all landing under the baseline.

A UI font is one command per header, e.g.

```
nix-shell -p 'python3.withPackages(ps: [ps.freetype-py ps.fonttools])' --run '
  python3 tools/make_font.py header --mono \
    --regular …/SGr-IosevkaSlab-Medium.ttc:0 \
    --name ui_small --size 14 --symbol ui_small \
    --out wintergreen/display/ui_font_small.h'
```

The TTC face index matters: face **0** is Iosevka Slab, and the rest are the Extended and
Oblique cuts.

There is no asset name to keep in sync: `generate_assets.py` reads
`resources/AtkinsonHyperlegible.bin` by path, and `FontManager` finds that partition by name
(`"font"`, matching `default_16MB.csv`).

## Persistence

**Nothing on the card is a directory the firmware made.** State is two hidden files at the card
root plus one file per book:

| What | Where | Written |
|---|---|---|
| settings | `/sdcard/.wintergreen` | sleep, reader/quickmenu pop |
| book index | `/sdcard/.wintergreen-index` | reader close, library scan, serial mutations |
| reading position | `<book dir>/book.pos` | reader close |

There is no `.wintergreen/` folder any more, and `main.cpp` no longer `mkdir`s anything at
boot. Two consequences that were the point:

- **The position file needs no key.** It used to be `.wintergreen/<8 hex digits>.pos`, the
  digits an FNV-1a hash of title + author + language, which gave a directory of opaque
  filenames, put two books one hash collision away from sharing a bookmark, and silently reset
  your place if a publisher edited the metadata. `book_pos_path()` returns the book's own
  folder, so the bookmark travels with the book and is deleted with it. `make_book_key()` and
  `fnv1a_32` are gone.
- **Settings and index stay two files, deliberately.** They are written on completely different
  schedules — settings is a few dozen bytes on every sleep, the index is ~30 KB once a session
  — so folding them together would rewrite the index every time the font size changed.

`Application::save_settings_` / `load_settings_` write a flat `key=value` file. Three keys, and
nothing derivable from elsewhere: `book_path`, `font_size`, `rotate_reader`. `book_path` is
written **only while the reader is in the stack**, so its mere presence means "resume this book
on boot" — that is what a separate `screen=reader|menu` key used to say.

The book-list cursor is deliberately **not** persisted. `MainMenu::stop()` keeps it in
`initial_selection_` so navigating away and back within a session restores it, but deep-sleep
wake is a full boot. Nearly free because the list is already sorted most-recently-opened-first
— row 0 *is* the book you last read.

Removed and worth not re-adding: `v=1` (written, never parsed — the loader ignores unknown
keys), `screen`, `last_version` (its What's New screen is long gone; all it did was force one
extra SD write per firmware update), and `open_counter`.

That last one was also a latent bug. It was a monotonic counter kept in *settings* but consumed
as `last_open_order` in the *index* — two files that could drift. Losing or resetting settings
reset the counter to 0, so newly opened books sorted *below* older ones until it climbed back.
`BookIndex::mark_opened()` now derives the next order from `max(last_open_order)` over the
entries themselves, in the same single pass that finds the entry. O(books) per book open,
against an operation that already reads and reparses the whole index.

The position file is four space-separated numbers: chapter, paragraph, offset, text offset.
Written **only from `ReaderScreen::stop()`**. It used to be written after every page turn, an
SD write per page. Two things make the sparser write safe, and both must hold: auto-sleep and
the power button both route through `Application::do_sleep_()`, which stops the screen; and
`do_sleep_()` explicitly stops the reader when a child screen (the quick menu) is on top,
because `screen_mgr_.top()->stop()` alone would miss it and silently lose the session. It also
skips the write when the position has not moved since it was loaded or last written
(`pos_written_`).

**`rename()` on this platform does not replace an existing file.** ESP-IDF's `vfs_fat_rename`
calls FatFs `f_rename` directly with no unlink, and `f_rename` returns `FR_EXIST` when the
destination exists — unlike POSIX. Both save paths therefore `remove()` the target immediately
before renaming. Without that, only the *first* save of a file ever succeeded: every later one
failed silently at the rename and left a stale `.tmp` behind, so settings, reading position and
the book index all stopped persisting after their initial write. **If you see a `.tmp` sitting
next to its target on the card, that is the symptom.**

The remove opens a brief window where neither file exists, so `load_settings_()` falls back to
reading `settings.tmp` when `settings` is missing — the `.tmp` is always a complete file.

Both the settings file and the book index write to a `.tmp` and then `rename()` over the target.
**The `.tmp` is not a backup** — no copy of the old file is ever made, and there are no `.bak.N`
rotations (deliberately removed). It exists purely to make the replace atomic. Don't reintroduce
a direct `fopen(path, "w")` on either file: `"w"` truncates the real file to zero bytes *before*
writing a single byte, and settings are saved on every auto-sleep — precisely when the battery
is most likely to give out. Staging the content in RAM does not help: the truncate happens at
`fopen`, and on FAT32 the data, FAT-chain and directory-entry updates are separate writes
anyway.

`save_settings_()` composes the file into a `std::string` and compares it against
`settings_written_`, the bytes of the last successful write, skipping the SD write when nothing
changed. That matters because it is called on every sleep, every book open and every reader pop,
and usually has identical content. `settings_written_` starts empty, so the first save after
boot always writes — which conveniently normalises a settings file left over from an older
firmware with removed keys.

## Conventions

- Two-space indent, `snake_case_` trailing underscore on private members, `k`-prefixed
  constants, `Deg90` = portrait.
- Screens are members of `Application`, not heap-allocated; `free_items_storage()` in `stop()`
  releases list RAM while keeping cursor position.
- Hyphenation ships **English patterns only** (`content/hyphenation/Liang/`);
  `HyphenationLang` has just `None` and `English`. Unknown language tags fall back to none.
- The README's attribution to upstream Nous/Microreader is intentional — keep it.

### Text truncation

**`fit_prefix()` (`screens/ListMenuScreen.h`) is the only place that measures how much of a
string fits a width.** Every list screen used to inline the same loop, and all of them had the
same two defects:

- **O(n²).** The loop called `word_width(text, fit + cb)` per step, re-measuring the whole
  prefix from the start each time. `fit_prefix` accumulates `char_width` per codepoint instead.
- **Unbounded.** The loop ran `while (*p)`, but the strings are `string_view`s into a
  `StringPool` chunk and are not NUL-terminated (see Persistence).

The per-character sum is a hair wider than `word_width()` over the same bytes, because
`word_width` applies class kerning between adjacent glyphs. Kerning is negative far more often
than not, so `fit_prefix` cuts at most a character early and never overruns — the safe direction
for a truncation. If a caller ever needs the exact kerned width of what it decided to draw, it
should call `word_width` once on the final byte count, which is what the draw path already does.

### Tests

Host-side, no framework, each a single `main()` with asserts. Build under a Nix shell — there is
no system compiler on PATH:

```
nix-shell -p gcc --run 'g++ -std=c++17 -fsanitize=address,undefined -I. \
  tools/tests/<name>.cpp <deps> -o /tmp/t && /tmp/t'
```

| Test | Covers |
|---|---|
| `json_scan_test.cpp` | the sync reply scanner, including every truncation of a valid reply |
| `layout_memory_test.cpp` | asserts a 24 KB ceiling on one page's layout, via an instrumented `operator new` over every page of a book. Uses a *proportional* stand-in font, not `FixedFont`: a fixed-width font makes every line the same length and hides exactly the estimate error this guards |
| `paginate_book_test.cpp` | forward/backward page agreement over whole books |
| `book_index_test.cpp` | index parse/save round trip |
| `blank_para_test.cpp` | empty-paragraph handling |
| `stretch_lut_test.cpp` | the cover scaler's stretch tables |

## Still open

- **Pre-draw skips image pages.** `draw_image_()` must drain the waveform before touching the
  card, so a speculative draw would block the UI loop for the couple of hundred ms the current
  page is still painting. Needs a second spare buffer, or an SD read that does not need the
  panel idle. **Neither is available**: the measured ~13 KB free with the radio up has no room
  for another 48 KB framebuffer, and the SD/panel SPI2 sharing is a wiring fact. Do not re-open
  without one of those two changing.

### Investigated and rejected

- **Preloading the neighbouring carousel cover.** Built, flashed, and it **ran the device out of
  memory and restarted it** while cycling books on the home screen. The estimate of "~25 KB for
  a second cache" was right about the steady state and missed the spike:

  a cover at `kHomeCoverW`×`kHomeCoverH` is `(424+7)/8 * 479` = **25,387 bytes**, and
  `load_cover_` fills it with `cover_data_.assign(...)`. `assign` on a vector whose current size
  differs **reallocates**, holding the old and new blocks at once. So a prefetch has the live
  cover, the outgoing prefetch buffer and the incoming one resident together — **~74 KB
  transient** against ~165 KB free heap at best, and ~13 KB with the radio up.

  The `std::swap` promote does not rescue it: the buffer swapped in belongs to a cover of a
  *different* book at a different scaled size, so the next `assign` reallocates anyway. "A
  settled carousel stops allocating" was simply wrong.

  Same failure as `build_page_items` — a `std::vector` growing to a size driven by page content,
  with the realloc spike rather than the final size exhausting the heap.

  If attempted again it needs a **fixed-capacity** buffer sized once for the largest cover
  (`reserve` at construction, never reallocated) rather than a second `std::vector` — and even
  then the win is a prefetch, not waveform overlap: `wait_panel_idle()` *drains* the waveform
  before an SD read rather than running alongside it.
- **Precomputing hyphenation into the WGB.** `find_hyphen_break` is called only when a word
  overflows a line end — roughly once per *line*, so about 30 Liang trie walks per page.
  Microseconds. Baking break points into the format would save the 27 KB trie and nothing
  measurable in time.
- **Collapsing `IDisplay` into `EInkDisplay`.** Only a handful of virtual calls per page turn,
  each followed by tens of ms of SPI and hundreds of waveform. It would also force `DrawBuffer.h`
  to include `epd.h` when it still has to compile on the host for `tools/epub2wgb`.
- **Sampling buttons every 10 ms instead of 5 ms.** Would halve the 200 Hz timer wakeups, but the
  CPU is *never* asleep between them — there is no light sleep — so it saves a fraction of a
  milliamp out of 15 while adding up to 10 ms to every press.
- **Compressing `sleep.mgr`** (96,008 → 4,682 with zlib). Moot: the image left the app image for
  its own flash partition, so it costs no image bytes, and uncompressed means the sleep path is a
  straight mmap with no inflate.
- **A faster page-turn waveform.** There isn't one. Page turns already use `EPD_FAST_REFRESH`,
  the shortest waveform the controller has.
- **Pre-draw backwards.** `prev_page_()` already runs `layout_backward()` and caches the result,
  so a backward pre-draw saves only the glyph blit — and saving the layout too would need a
  second, backward-keyed page cache on top of a second spare framebuffer.
- **Dropping the unused OTA half of the flash.** `app1` (6.4 MB) and `otadata` serve an OTA path
  that does not exist, but removing them changes the partition table under an already-flashed
  device and needs a full erase — the owner's call.
- **Caching glyph indices or string widths.** Lookup is already a binary search over the range
  table and a label is measured a handful of times per repaint, all of them now linear. A cache
  would add heap churn, which is the thing this heap actually fails on.
- **Size-only, not worth doing alone** (app0 is 6.4 MB against a 1.2 MB image): the UI fonts and
  hyphenation trie are 81 KB of rodata that could move to a partition; `__d_vfprintf` /
  `__d_vfscanf` are ~5 KB that only hand-rolled integer formatting at every call site would
  remove, and IDF's panic path may keep them anyway; `f_mkfs` is 2,360 unreachable bytes that
  `esp_vfs_fat_sdspi_mount` references unconditionally.

### Closed — don't re-investigate

Instruction cache size (fixed in C3 silicon), QIO flash mode, SD clock above 20 MHz, tickless
idle, region-only refresh for menu cursor movement, `CONFIG_NEWLIB_NANO_FORMAT` (the option no
longer exists under picolibc), and **`-Os`** (tried on hardware: same or slightly slower page
turns). See "Build & flash", "Idle power" and "Refresh latency".

## Removed (don't resurrect without asking)

Only the reasons are kept — the symbol-by-symbol inventories are in git history.

Screens and settings: the settings menu, theme picker, Stats / GlobalStats / WhatsNew / Alert /
ConvertAll screens, the bouncing-ball and grayscale demos, BMP sleep-image conversion,
sleep-image cycling, SD-card font selection, and the on-device System tab. Also
`RecentBooksScreen`, `HiddenBooksMenu` and `ChapterSelectScreen` — recents became the home
carousel, hidden books a `MainMenu` mode, chapters an inline quick-menu list. **Deleting a screen
means editing `platforms/esp32/CMakeLists.txt` too**; the failure is at CMake generate time with
a "Cannot find source file" naming the wrong call site.

**Antialiased (grayscale) text in the reader**, and the whole multi-pass machinery behind it. It
was removed because it was **visible as motion**: `kLutGrayscale`'s waveforms put white and black
both on LUT0 = *do nothing*, so gray was only reachable by driving a pixel black and then
partially pulling it back. Every page turn became three physical updates, the middle one showing
the font's mid/dark AA levels as solid black — text that appeared to thicken and then visibly
thin after the page had already changed. That intermediate is inherent to the LUT, not a
sequencing bug, so no reordering fixes it.

Text is consequently a hard threshold at the 50% AA level and reads slightly heavier than the old
settled state. If that weight ever becomes a problem the escape hatch is the one-pass
`kLutFactoryQuality` LUT, which **has now been deleted along with `setCustomLUT_()` and
`custom_lut_active_`** — recover it from git history (`platforms/esp32/epd.h`). It drove each
pixel to an absolute level with no prior-state dependency, so a page could land in final form in
a single update, at the cost of squashing the font's 5 AA levels into the panel's 4 and composing
both RAM planes by hand. Its RAM polarity was **inverted** relative to normal drawing (state
`(RED<<1|BW)` = 00 is white); the per-row comments inside the table said the opposite and were
wrong.

**Hyperlinks, entirely** — `LinksScreen`, the nav-history stack, `href` on `Run` and `LayoutWord`,
and the underline rendering. An `<a>` now contributes only its text. WGB run-flag bit `0x02` was
its last remnant and is free.

**Every page-geometry setting except font size.** H-Margin, V-Margin, Alignment, Line spacing and
Publisher Sizes, with their overrides and persisted keys. Layout now always takes alignment and
line height from the book's CSS and always honours publisher font sizes; where the book specifies
nothing, body text is **justified**. `para.alignment` is a genuine optional throughout — WGB
round-trips the empty state through the `kWgbAlignDefault` (`0xFF`) sentinel, so changing that
default needs no re-conversion. Dead `static constexpr` class members produce no compiler warning,
so leftovers have to be found by grep.

**The hyphenation toggle** (the feature stays, always on), taking `ListMenuScreen::add_separator()`
and its `vector<bool> separators_` with it. `MainMenu` is unaffected — it has its own
`separators_` (`vector<pair<int,string>>`) and overrides `is_separator()`, which means the base
member had been shadowed all along.

**The reading progress indicator** — bar, percentage, styles, scopes and keys. Nothing is drawn in
the reader's bottom margin now. `progress_pct()` / `chapter_progress_pct()` **stay**; they feed
the book-details card.

**On-device EPUB conversion, and EPUB support entirely.** A book is a `.wgb`; `BookIndex` does not
recognise `.epub`, so an unconverted book is not listed rather than listed-and-broken. `Book`,
`EpubParser`, `CssParser`, `XmlReader`, `HtmlEntities`, `WgbConverter`, `ZipReader`,
`ImageDecoder`, `JpegDecoder`, `PngDecoder`, `stb_image` and miniz are **out of the firmware
build** but remain in the repo for `tools/epub2wgb` — ~95 KB of flash. `WgbWriter` is host-only
too. (`platforms/esp32/miniz_wrapper.cpp` is gone with them.)

Because those TUs are host-only, every `#ifdef ESP_PLATFORM` in them was unreachable and they are
flattened to the host branch — taking the `benchmark_*` functions, the `esp_timer`
instrumentation, `EpubParser`'s heap-driven reserve capping and `CssCache::low_memory()` (a
constant `false` off-device, so eviction is now driven by `over_budget` alone). Converter output
is unchanged.

**Font sideloading**, both the serial `FONT` upload and `SDFN` uploads to `/sdcard/fonts/`. The
two compiled-in fonts are the only fonts the device can have.

**exFAT**, with `tools/patch_ffconf.py` which enabled it. Cards are FAT32, 16 GB or smaller; an
exFAT card simply fails to mount. It cost 7,892 bytes and required patching `ffconf.h` inside the
IDF package on every build — ESP-IDF ships it disabled because it is patent-encumbered, so there
is no Kconfig option.

**All reading statistics** — read time, times opened, page turns, estimated time left — from the
reader, `.pos`, `BookIndexEntry`, the index format and both places they were displayed. `.pos` is
now four numbers and the index line is `path|title|author|last_open_order|progress_pct`. Both
readers tolerate older short and longer forms (a missing field reads as 0, extras are not
consumed), so `INDEX_FORMAT_VERSION` did not need bumping.

`progress_pct` is the one exception, and it is there for cost: the book list shows a percentage
per row, and deriving it live would mean opening every WGB *and* every `.pos` on the card each
time the list is built. `Application::record_book_progress()` reloads the index before saving —
`MainMenu::pause()` clears the in-memory entries whenever the reader is pushed, so saving without
reloading would truncate the file — and re-clears afterwards. `progress_pct()` itself and the
`total_chars` / `char_before_para()` machinery stay: computed live from position, not accumulated.
`last_open_order` stays; it drives the most-recently-read sort.

**Debug and benchmark surface, entirely.** Every log statement, `HeapLog.h`, the `'S'`/`'Q'`/`'G'`/
`'P'` serial commands, `bench_render`, `bench_flash`, `top_screen_name()`, and the layout
instrumentation — whose `g_layout_hyph_us` and `g_layout_metrics_us` bracketed **every
`word_width()` call** with `esp_timer_get_time()`, two hardware timer reads per word, thousands
per page.

**Dead `IRuntime` and `DrawBuffer` surface.** `step_mode()`, `consume_step()`, `yield()` and
`should_continue()` were never overridden to anything but a constant, so the branches testing them
were dead; `Loop.{h,cpp}` went with the desktop build entirely, its three-line
`run_loop_iteration()` inlined into `main.cpp`'s loop. On `DrawBuffer`: the no-arg `write_ram_*`
wrappers, `draw_circle()`, `draw_text_centered()` and `set_rotation_transform()`.

**The last of the grayscale plumbing**, orphaned when the MGR2 sleep blit moved to
`RefreshMode::Half`: `IDisplay::write_ram_bw` / `write_ram_red` / `grayscale_refresh_1pass` and
both `EInkDisplay` overrides, `DrawBuffer::sync_bw_ram()`, and `draw_text_plane()`. With them went
`kLutFactoryQuality`, `setCustomLUT_()`, `custom_lut_active_` (so `EPD_FAST_REFRESH` is a bare
`0x1C`) and the four `CMD_GATE_VOLTAGE` / `CMD_SOURCE_VOLTAGE` / `CMD_WRITE_VCOM` /
`CMD_WRITE_LUT` defines. `CMD_WRITE_RAM_RED` **stays** — `full_refresh` still writes both planes.

**`wg_sync::FailStage`**, the sync bring-up scaffolding: the 14-stage enum, `g_fail` /
`g_fail_heap_kb` / `fail_()`, `IRuntime::sync_fail_stage()` and `sync_fail_heap_kb()` with their
ESP32 overrides, and `MainMenu`'s `Sync (fail %u, %uk)` label with its `sync_label_buf_`. Every
bail-out in `bring_up_wifi_()` is a plain `return false` and the row reads `Sync` for both Idle
and Failed.

**`Application::tick_count()` / `uptime_ms()`** and the `ticks_` / `uptime_ms_` counters behind
them — incremented every frame, never read. `running()` stays. `ScreenManager::restart_top()` had
no callers either.

**The scratch-buffer loan** (`scratch_buf1()` / `scratch_buf2()` / `reset_after_scratch()`), which
handed both framebuffers to on-device conversion and image decoding. The three surviving
`reset_after_scratch()` calls were actively harmful — blanking both buffers and marking the frame
stale after serial index ops that never touched them. `DrawBuffer::active_valid_` went too: with
nothing invalidating it, it was always true.

**`BookIndex::is_book_path`** (byte-identical to `is_wgb_path`) and **`build_index`'s counting
pass**, which walked the whole card once just to produce a total that only a compiled-out log
read. One tree walk now, with `iterate_books` templated on its callback rather than taking a
`std::function`.

**Font provisioning and the "Installing fonts..." screen**, i.e. the entire asset-blob mechanism.
`IDisplay::partial_refresh_region` went with it.

**The 3-second post-flash boot delay** on `ESP_RST_SW`, which existed so a serial monitor could
attach. There is no serial monitor output any more.
