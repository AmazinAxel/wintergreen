# Power, input and the power button

Part of the Wintergreen notes; the entry point is `CLAUDE.md`.

## Idle power

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

More wake-path rules, each worth a measurable slice of every wake: **the boot holds
the 160 MHz PM lock** from `main.cpp` (the runtime's constructor configures DFS, so
without it the whole boot ran at 80 MHz); **`verify_battery()` does not sleep** between
its eight samples (it used to spend 175 ms); **the ROM banner is suppressed** —
`deep_sleep_now_()` calls `esp_deep_sleep_disable_rom_logging()` on every entry, a
few hundred UART characters per wake; **the panel's post-reset settle overlaps the
card mount** (`EInkDisplay::begin()` / `finish_begin()`), and its boot-time RAM clear
is gone because `start()`'s `full_refresh()` writes both planes anyway; and
**resuming into a book does not build the home screen** — `ScreenManager::push_inactive`
puts it under the reader unstarted, and the index is read by `record_book_progress()`
(or the home screen's own resume) only when something needs it. The reader then
pre-draws the next page on its first frame (`prerender_pending_`, also set on book open
and quick-menu return), so the first turn after waking is a memcpy. Going to sleep,
`do_sleep_()` puts the sleep screen up **before** stopping the reader and saving, so
those SD writes are not latency; the panel's `deep_sleep()` is sent last.

**The CPU never sleeps between frames.** `wait_next_frame()` `vTaskDelay`s to a 25 ms
cadence, so the core wakes 40×/s at ~15 mA. With no light sleep available the only
lever is reaching deep sleep sooner. A longer frame period saves a fraction of a
milliamp and costs input latency — measured and rejected.

## Dynamic frequency scaling (live)

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
  drops it after `kPerfHoldMs` (300 ms) of quiet. Acquire/release are skipped when
  unchanged.

## Input and holds

Side-rocker paging is **orientation-dependent**; the front row is not. The rocker is
one physical control whose ends swap places when the device is turned, so
`ReaderScreen::update()` picks the mapping off `landscape_hold`:

|            | Up (rocker top) | Down (rocker bottom) |
|------------|-----------------|----------------------|
| Portrait   | next            | previous             |
| Landscape  | previous        | next                 |

Front buttons are fixed to the panel: Button2 next, Button3 previous, both
orientations.

`Rotation` has two values: `Deg90` (portrait) and `Deg0` (landscape). The reversed
orientations were the same physical hold and are gone from the enum along with every
drawing branch for them, so `rotation() == Deg0` is a complete landscape test.

5 ms button sampling (`input.h`) under a 25 ms UI frame (`main.cpp`). **An input edge
ends the frame wait early** (`ui_wake.h`: the sampler and the clicker give a binary
semaphore that `wait_next_frame()` takes with the frame timeout), so a press is handled
within a sample or two instead of waiting out the rest of the frame — free in power,
since the loop wakes no more often than before when nothing is pressed. A semaphore,
not a task notification, because IDF drivers the UI task calls may block on its
notification slot. Screens repaint only on state changes, so frame rate does not drive
panel traffic.

**Card writes are kept off transitions.** A reader close (`record_book_progress`) and a
pop from the reader stack set `persist_pending_` instead of writing the index and
settings; `Application::update` flushes them once input has been still for
`kPersistQuietMs` (400) and the panel is idle, and `do_sleep_()` flushes whatever is
left. Anything that reloads the index from disk must call `flush_persist()` first —
`MainMenu::run_sync_` does.

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

## The power button

GPIO3, the only non-ADC key, handled entirely in `Application::update`, not by any
screen. Held for `kHoldDelayMs` it sleeps; released sooner it is forwarded to the
top screen as a synthetic `Button1` (Confirm), so "select" and "open the quick menu"
come for free. `power_confirm_` marks that frame's Confirm as the power key so the
reader can open its quick menu on the top setting for it and on the current chapter
for the front select key. **There is no double tap anywhere** — one was built here and
removed as unintuitive; don't add one (it also forces a wait on every single tap).
Three load-bearing details:

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
