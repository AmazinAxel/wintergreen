# Wintergreen

Firmware for the Xteink X4 e-reader (ESP32-C3, 16 MB flash, e-ink panel). A fork of
Nous, itself a fork of Microreader. The design goal is a reader with no
configuration surface: everything is fixed at compile time so the device boots
straight into a book.

## Build & flash

```
pio run                            # build (default env: esp32c3)
pio run -t upload                  # build + flash over USB
pio run -e esp32c3-dev -t upload   # ~940 KB image, no embedded reader font
pio device monitor                 # serial log, 115200
```

`esp32c3-dev` sets `-DWG_NO_EMBED_FONT`, which drops Literata (726 KB) from the
asset blob for fast iteration. The device keeps using whatever is already in the
font partition, so **flash a full `esp32c3` build at least once** on a new board
first. Both envs write the same `platforms/esp32/assets.bin`, regenerated
pre-build, so its contents reflect whichever env you built last.

The board enumerates as `/dev/ttyACM0` on Linux (VID:PID `303A:1001` — the C3's
native USB JTAG/serial peripheral, not an external UART bridge). `upload_port` is
deliberately **not** set in `platformio.ini` so PlatformIO auto-detects; don't
hardcode a port, it breaks cross-platform flashing.

There is a desktop build under `platforms/desktop/` — its CMakeLists must be kept
in sync when adding or removing core `.cpp` files, as must
`tools/epub2mrb/CMakeLists.txt`. All three list sources **explicitly**, so a
deleted file fails at CMake generate time with an error naming
`idf_component_register` rather than anything you touched.

`ESP_PLATFORM` is **not** a QEMU leftover — it is ESP-IDF's own macro, defined
for any ESP32 target build, and it is the seam between firmware and the two host
builds (`tools/epub2mrb`, `platforms/desktop`). It stays. The QEMU flag was
`QEMU_BUILD`, below.

The `esp32c3-qemu` env is gone, along with every `QEMU_BUILD` conditional. Two
things that removal broke, both worth knowing about:

- Deleting `#ifndef QEMU_BUILD` … `#endif` around the ADC setup in `input.h` took
  **the body with it**, leaving a constructor that configures the power GPIO and
  nothing else. No `adc_oneshot_new_unit`, no `esp_timer_start_periodic`, so
  `sample()` never ran, `debounced_` stayed 0 and **no button worked at all** —
  including power. It builds and boots perfectly.
- The `[env:esp32c3-qemu]` header was removed but its keys were not, so
  `build_flags = -Ilib -DQEMU_BUILD` and a `sdkconfig.qemu` reference were left
  orphaned inside `[env:esp32c3-dev]`. That eventually surfaced as an
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
sleep (milliamps). The lever is `kAutoSleepMinutes` in `wintergreenConfig.h`,
traded against deep-sleep wake being a full boot.

Because wake **is** a full boot, everything on the boot path runs on every wake,
and cheap wake is what makes a short `kAutoSleepMinutes` tolerable. Hence the
"Boot speed" block in `sdkconfig.defaults`: `BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP`
(skips re-hashing the whole image; cold boot still validates),
`BOOTLOADER_LOG_LEVEL_NONE` (~78 ms of UART per boot), and `RTC_CLK_CAL_CYCLES=256`
(down from 1024; calibration precision only matters for *timer* wake, and this
device wakes on GPIO). App logging is silenced via `LOG_DEFAULT_LEVEL_NONE` but
deliberately still **compiled in** — flash is not scarce (~4.7 MB free in app0)
and `esp_log_level_set()` can re-enable any tag at runtime.

**Dynamic frequency scaling is still on the table and untried.** `CONFIG_PM_ENABLE`
with `light_sleep_enable = false` does not trip the errata `select` above, so it
does not break the buttons. Two caveats if it gets implemented:

- Use `min_freq_mhz = 80`, not 40, so APB never drops below 80 MHz and the
  already-marginal SD timing is left alone.
- Page rendering is pure CPU with no driver involved, so **nothing acquires a PM
  lock on its behalf** and it would run at the idle frequency — page turns up to
  2× slower. It needs an explicit `ESP_PM_CPU_FREQ_MAX` lock held across the
  render/page-turn path and released when returning to the idle poll. Plain
  `esp_pm_configure` without that lock is a regression, not a win.

Side-rocker paging is **orientation-dependent**; the front row is not. The rocker
is one physical control whose ends swap places when the device is turned, so
`ReaderScreen::update()` flips `inv_side` in landscape:

|            | Up (rocker top) | Down (rocker bottom) |
|------------|-----------------|----------------------|
| Portrait   | next            | previous             |
| Landscape  | previous        | next                 |

`Application::invert_side_buttons()` is the portrait mapping; landscape is always
its inverse. The front buttons are fixed to the panel and never change.

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
  milliseconds now, and (on `LyraExtScreen`, where it moved) it is the tree's only
  remaining hold gesture outside `Application`'s power button.

`Application`'s power-button hold was always ms-based (`power_hold_ms_ += dt_ms`)
and was unaffected — that is the pattern to copy.

### Refresh latency

Almost all of a page turn or a menu keystroke is the e-ink waveform, not the CPU.
Layout and draw are tens of milliseconds; the waveform is hundreds. Three things
exploit that, and the first two have **invariants that are easy to break**.

**`partial_refresh()` does not block.** `refreshDisplay()` takes a `wait` flag and
`partial_refresh()` passes `false`, so it fires `CMD_MASTER_ACTIVATION` and returns
while the panel is still updating. The app then lays out and draws the *next* frame
during the waveform instead of after it. This is safe only because **every**
`EInkDisplay` entry point opens with `wakeIfNeeded(); waitWhileBusy();` — that
prologue is what collects the outstanding refresh. Do not add a panel operation
without it. Two consequences that are load-bearing:

- Anything that sends SPI right after firing must keep `wait = true`, or the command
  lands mid-waveform and corrupts the update. That means `full_refresh`, both
  grayscale paths, and anything with `turnOffScreen` (the screen powers down at the
  *end* of the waveform). `setCustomLUT_()` and `deep_sleep()` therefore start with
  their own `waitWhileBusy()` — `deep_sleep()` mid-update leaves a half-drawn panel
  on screen for the entire sleep, which is the most visible way to get this wrong.
- **The SD card shares SPI2 with the panel** and concurrent traffic corrupts an
  update in flight (this is why `main.cpp` already defers index ops on
  `epd.is_busy()`). Any new SD access must call `DrawBuffer::wait_panel_idle()`
  first. The existing ones are `decode_image_to_buffer_`, `ReaderScreen::stop()`'s
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
vectors. `LayoutWord::text` points into `MrbChapterSource`'s 32-slot paragraph
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
defaults over them. Note there is one per env, so `sdkconfig.esp32c3-dev` needs
deleting too or the dev image silently keeps the old settings.

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

## Configuration

`wintergreenConfig.h` **in the project root** is the only place with tunable
values. The project root is on the include path for both platform builds.

| Constant | Default | Notes |
|---|---|---|
| `kWifiSsid` / `kWifiPassword` | `""` | **Nothing reads these yet** — no Wi-Fi subsystem exists. Placeholders for the planned NAS sync. |
| `kSunlightFadingFix` | `false` | Periodic full flush to fight sunlight contrast loss. Read by `Application::sunlight_fading_fix()`. |
| `kAutoSleepMinutes` | `1` | Read by `Application::sleep_timeout_min()`. |
| `kPowerHoldSleepMs` | `800` | Power-button hold before sleeping; a shorter press acts as Select. Read by `Application::power_hold_sleep_ms()`. |
| `kHoldDelayMs` | `350` | How long a nav button must be held before it auto-repeats. Below ~250 ms a tap starts reading as two presses — the press itself already counts once. |
| `kHoldRepeatMs` | `175` | Gap between repeats once they start; lower is faster. |
| `kHoldAccelStep` | `1` | Extra list entries added per repeat in menus, so a long hold accelerates. `0` = constant one entry per repeat. The reader ignores it — page turns never accelerate. |

Everything else is hardcoded. `Application` exposes them as `static constexpr`
member functions so call sites read unchanged:

- portrait lists (`rotate_display()`), X-Large menu font (`kMenuFontSize = 3`)
- no sleep caption (`show_sleep_text()`); the sleep screen shows the cover of the
  book **currently open**, and only when it is not in `/.hidden/` — a hidden book
  must not be advertised by a screen that sits on display while the device is
  idle. Everything else, including sleeping from the home screen, gets the
  embedded wordmark image.
- battery is a header percentage, never an icon. The converted-book marker is
  gone — every book on the card is an MRB, so the trailing middle dot marked
  every row and said nothing (`show_converted_indicator()` and `mrb_exists` went
  with it)
- images always render: the `images_enabled` global is gone, not defaulted
- default button mapping (`invert_bottom_paging()` is `true` — that is the default)
- single reader font (Atkinson Hyperlegible, `FontManager::kFontAsset`), no picker
- book list is title-only, sorted most-recently-opened first with a hairline
  divider before never-opened books
- fixed page geometry: margins at `ReaderSettings::h_padding()` / `v_padding()`,
  and alignment, line spacing and font sizes all deferring to the book's own CSS

`ReaderSettings` is down to **one** live value — `font_size_idx` — plus
`rotate_reader_` on `Application`. Those two, and reading position, are the only
genuine runtime state; everything else about the page is compile-time.
Hyphenation is always on (English patterns; unknown language tags fall back to
none). The in-book quick menu is a flat, header-less list: Font Size, Reader
Display, then Chapters when the book has a TOC.

## Architecture

`Application` (`lib/wintergreen/Application.{h,cpp}`) owns every screen as a
by-value member and a `ScreenManager` stack. Screens never push directly; they
call `push_screen`/`replace_screen`/`pop_screen`, which queue a `ScreenId` that
the app applies after `update()` returns. `screen_for_(ScreenId)` maps the enum
to the member.

Screen stack: `LyraExtScreen` (home) → `MainMenu` (all books) → `ReaderScreen`
→ `ReaderOptionsScreen` (quick menu). There are only these four screens.

Back on the home screen opens the book list; back in the list returns home.

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
the button was held. `LyraExtScreen` also swallows the first hold after every
`start()` (`back_ignore_`), because `MainMenu` pops on the back press and hands
the home screen a button that is still down; without that, the release would read
as a fresh tap and reopen the list. **Any screen pushing another from a button
press inherits this problem.**

Four things the hidden list itself depends on:

- `.hidden` books are **not in `BookIndex`** (its scan skips dot-directories),
  so `MainMenu::scan_hidden_()` walks the folder itself and reads title/author
  straight from each MRB. Those entries carry owned `title_own`/`author_own`
  strings rather than `StringRef`s, because there is no pool entry to reference.
  The scan runs only when the gesture first asks for it.
- `on_select` skips `record_book_opened()` for a hidden entry. `mark_opened()`
  would be a no-op anyway, but the point is that a hidden book must never reach
  the recents carousel on the home screen.
- `separators_` holds **visual** indices, so the hidden divider shifts the
  recently-opened/never-opened one below it by a row. `populate_list_()` inserts
  the hidden group after sorting, so it keeps its own alphabetical order.
- Hidden books are `.hidden/<book>/book.mrb` like any other. The old
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
  draw one frame with the previous book's cover and then redraw — two e-ink
  refreshes per press.
- The cover comes from **`cover_sleep.bin`** (up to 480x786), not the 160x240
  `cover.bin` the book list uses — the box is ~450x550, so the thumbnail could
  only ever be pixel-doubled into it. `cover.bin` is the fallback when the
  full-res file is missing.
- Two scaling paths, and the distinction matters. **Upscaling is whole-number
  only**: pixel-doubling keeps the 1-bit dither intact where a fractional
  nearest-neighbour scale moirés it. **Downscaling is a box filter plus a 4x4
  ordered dither** — the average of a cell is a coverage fraction, and a hard
  50% threshold would throw away every tone between paper and ink. It streams
  the source a row at a time (a 480x786 cover is 47 KB) with one accumulator row.
- All-white rows are **trimmed off the top and bottom** afterwards. Covers often
  carry a white band above and below the artwork, and with the frame drawn a
  fixed distance from the bitmap those bands read as lopsided padding. Columns
  are left alone — the cover is fitted to the box width, so side bands are rare.
- The cover read calls `wait_panel_idle()` first: the card shares SPI2 with the
  panel and this is a much longer read than the old thumbnail.

There is no separate Chapters page: `ReaderOptionsScreen` lists the book's TOC
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

A hairline divides the settings from the chapters, drawn in a `kSepH` gap of its
own between the two rather than inside the first chapter's row — a selected row
fills its whole rect, and a rule drawn inside it would be painted over. Row
height is `row_h_()` = the list font's `y_advance()` + padding, not a constant:
the list is set in the 32 px header face and the old fixed 28 px row was shorter
than a single line of it.

The book title comes from `ReaderScreen::display_title_()`, not
`mrb_.metadata().title`: a converted EPUB with no usable metadata title yields
the literal string `"none"`, and `BookIndex` already substitutes the containing
folder name for it. Without the same substitution here the quick menu said
"none" for a book the home screen named correctly.

### ListMenuScreen

Base class for every list screen, in `screens/ListMenuScreen.{h,cpp}`. There is
**one** visual theme (formerly "Lyra Extended"); the `MenuTheme` enum and the
Chronicle/Minimal/Stele/Codex/Lyra variants are gone. Two per-instance booleans
select the remaining layout variations:

- `detail_list_` — two-line rows (title + subtitle, full-width divider, right-hand
  column). Set by MainMenu. The right-hand column is the reading percentage and
  sits on the **subtitle** baseline, bottom right beside the author, so the title
  gets the full row width. The **last** row draws no divider — there is nothing
  below it to divide from.
- `plain_list_` — centred-title header instead of the battery status bar. Set by
  ChapterSelectScreen.

No screen draws button labels, nav-arrow glyphs or a battery icon: the labelled
`Back | Select` / `Down | Up` boxes and the ◀▶▲▼ hint row were removed, and
`draw_bottom_` now only draws the book-details footer. Don't add them back — the
buttons are fixed and the panel is small. Battery appears once, as `NN%` in the
header (`draw_header_`, and `LyraExtScreen`'s own header).

Both false = standard header + centred single-line rows. A non-empty `subtitle_`
switches the header into the book-details card and is checked before either flag.

**The header is battery-only.** There is no "wintergreen" wordmark on any screen
and no rule under the header — the book-details card drew the wordmark in a bar
of its own and that is gone too. The card's footer no longer repeats the battery
either. `header_override_`, which swapped the wordmark per screen, went with the
wordmark.

Every screen draws the percentage through `ListMenuScreen::draw_battery_`, and
`LyraExtScreen` — which otherwise shares no drawing code with the base — calls it
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

EPUB → MRB (a preprocessed binary layout format) → rendered pages.

**MRB v12 embeds its images.** `MrbImageRef` is `{data_offset, data_size, width,
height}` (12 bytes) addressing a blob section inside the MRB itself, written by
the converter. Before v12 the offset pointed into the *EPUB's ZIP*, so a book
copied to the card without its EPUB drew a blank box for every figure.

The bytes go in **verbatim** — already-compressed JPEG/PNG, not re-deflated.
That is the whole optimisation: reading an image is a seek plus a read, with no
ZIP central-directory walk, no local-header parse and no inflate pass, none of
which bought anything for data that is already compressed. Both read paths
describe the blob as a `ZipEntry` with `compression = 0` so the existing
streaming decoder works unchanged, and leave `name` empty so format detection
falls back to sniffing the magic bytes:

- `ReaderScreen::decode_image_to_buffer_` for drawing, and
- `make_image_size_query()` in `MrbReader.h` for lazily resolving dimensions the
  EPUB markup did not declare. Both open `mrb_path_`, **not** `path_` — for an
  MRB book they are the same file, but that is a coincidence worth not relying on.

Decoded, dithered results still memoise to `img_<key>_<w>x<h>.bin`, which for a
converted book lands in the book's own folder next to `book.mrb`.

`MrbReader::open()` reads the chapter and image tables with **one `fread` each**
and parses from memory. One `fread` per entry costs a FATFS + SPI round trip
apiece and a long book has hundreds of chapters, so that loop dominated open
time. It also no longer reads the spine-filename or anchor tables: both existed
only to resolve hyperlinks, which are no longer parsed. Dropping the spine table
removes a `std::string` per chapter from the heap for as long as a book is open.
`MrbWriter` still emits both sections — harmless, and cheaper to strip when the
format next changes. Nothing after the TOC is read sequentially, so ignoring them
is safe.
`content/mrb/MrbConverter` does the conversion; `MrbReader` is the read path.
Conversion on-device is slow, so books are converted on a computer.
`tools/convert-books.sh <epub-dir> <out-dir>` builds and runs `tools/epub2mrb`,
which calls the same `Book::open` + `convert_epub_to_mrb_streaming` the firmware
does, so the output is identical to what the device would have produced. It needs
cmake and a C++17 compiler; on NixOS the script fetches both through `nix-shell`
automatically, which is also how to build it here — there is no system cmake.

The converter links only the content pipeline. `MrbConverter` includes
`DrawBuffer.h` purely for the `kWidth`/`kHeight` constants, so no display or SDL
code is involved.

Output is one folder per book:

```
<out>/<book name>/book.mrb
<out>/<book name>/cover.bin    # when the EPUB has a cover
```

The `book.mrb` filename is not arbitrary — `convert_epub_to_mrb_streaming()`
looks for the literal string `book.mrb` in the output path and writes
`cover.bin` beside it, so renaming the output breaks cover extraction silently.

`MrbConverter` is still compiled into the firmware and is still the fallback when
an **EPUB** is opened without a cached MRB.

### Books on the card

Two shapes are recognised, both found by `BookIndex`'s recursive scan (which
skips dot-directories, so `.wintergreen/cache/` is never picked up):

- `<book name>/book.mrb` — a converted book, opened **in place**. Metadata comes
  from the MRB itself, no EPUB parse. Covers are siblings: `cover.bin` (160x240,
  book list and home screen) and `cover_sleep.bin` (480x786, sleep screen).
  **Both** must be written by the converter — the device cannot generate either
  for a converted book, because there is no EPUB on the card to extract from.
- `<name>.epub` — a source, converted on demand into
  `.wintergreen/cache/<stem>/book.mrb` with covers alongside it.

A third: the "converted" marker in `MainMenu` looked for `cache/<stem>/book.mrb`,
and every converted book has the stem `book` — so they all probed the same
nonexistent path and every one displayed as *not* converted. The marker has since
been removed outright (see Configuration), taking the per-book `fopen` with it.

Two traps here, both hit during the switch to converted books. `cover_bin_path()`
derives from the file *stem*, and every converted book is literally named
`book.mrb`, so all of them mapped to `cache/book/cover.bin` and collided — it now
returns the MRB's sibling instead. And `ensure_cover_bin()` must return early for
an MRB: there is no EPUB to extract from, so whatever the converter shipped is all
there is.

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
  styles, built `--mono` (see below). Lives in the appended asset blob
  (`platforms/esp32/asset_blob.*`) and is provisioned into a flash partition on
  first boot after a firmware update, keyed on CRC. **Only the `esp32c3` env
  ships it** — a font change is invisible until a full build is flashed, because
  `esp32c3-dev` filters the asset out and the device keeps the partition it has.
- **UI: Iosevka Slab Medium**, built `--mono`, one size per header in
  `display/ui_font_*.h` (14/24/32 px → small/large/header). There is no medium
  *size*; that header was unused and is gone. It was Bold until the weight was
  dropped — Bold at 14 px thresholds into near-solid blocks, and the mono
  hinting is what keeps Medium's thinner stems from breaking up. Dropping the
  gray planes roughly halved each header (small: 20,332 → 10,099 bytes).

MBF4 format is documented in `content/BitmapFontFormat.h`, but three things that
matter are **not** in that header and were recovered by decoding the old shipped
Literata — `make_font.py`'s docstring is the real reference:

- `MbfGlyph::advance_width` is in **quarter-pixels**, not pixels.
- Antialiasing is **5** levels, not 4, encoded across the three planes as
  `(bw, msb, lsb)`: white `(1,0,0)`, light `(1,0,1)`, mid `(0,1,0)`, dark
  `(0,1,1)`, black `(0,0,0)`. The BW plane draws where its bit is **clear**; the
  gray planes draw where theirs is **set**. So `(msb,lsb)` is a lightening
  modifier, not a magnitude.
- The reader font asset is `[uint32 uncompressed size][zlib stream]` wrapping an
  FNTS v2 bundle: `[FNTS][num][ver=2][pad:2][name:32][num × uint32][MBF4...]`.

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

`tools/check_font.py` parses a bundle or bare MBF4 exactly the way `BitmapFont.h`
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
    --out lib/wintergreen/display/ui_font_small.h'
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

`build_assets.FONT_ASSET` is the single source of the asset's name and must match
`kFontAsset` in `platforms/esp32/font_manager.h`. `WG_NO_EMBED_FONT` filters on
that constant — when the name was hardcoded in two places, renaming the font
silently defeated the filter and the dev image quietly grew by the whole font.

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

`Application::save_settings_` / `load_settings_` write a flat `key=value` file at
`<data_dir>/settings`. Three keys, and nothing that can be derived from
elsewhere: `book_path`, `font_size`, `rotate_reader`.

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
*settings* but consumed as `last_open_order` in *book_index.dat* — two files that
could drift. Losing or resetting settings reset the counter to 0, so newly opened
books sorted *below* older ones in the book list and on the home screen until it
climbed back. `BookIndex::mark_opened()` now derives the next order from
`max(last_open_order)` over the entries themselves, so the counter cannot
disagree with the data it orders. It is O(books) per book open, against an
operation that already reads and reparses the whole index.

Only `.wintergreen/` itself is created at boot. `data/` is gone entirely, and
`cache/` is created **on demand** by the EPUB conversion path — a library of
converted books never produces it, so an empty directory never appears on the
card to puzzle over later. `cache/` holds nothing but EPUB machinery: the
converted `book.mrb`, its covers, and decoded-image memos. Converted books keep
all of that in their own folder instead.

Reading position lives in `<data_dir>/<book_key>.pos` — flat alongside `settings`
and `book_index.dat`. Seven space-separated numbers: chapter, paragraph, offset,
text offset, times opened, reading ms, page turns. Three further fields used to be
appended — progress percent, time-left estimate, chapter count — and were written
on every save but **never read back**; `BookIndex` already stores them, fed from
the same values by `update_book_read_time()`.

It is written **only from `ReaderScreen::stop()`**. It used to be written after
every page turn, which is an SD write per page. Two things make the sparser write
safe, and both must hold: auto-sleep and the power button both route through
`Application::do_sleep_()`, which stops the screen; and `do_sleep_()` explicitly
stops the reader when a child screen (the quick menu) is on top, because
`screen_mgr_.top()->stop()` alone would miss it and silently lose the session.

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

Ordered by expected payoff. Each of these has been analysed; none is speculative.

1. **Dynamic frequency scaling.** `CONFIG_PM_ENABLE` with
   `light_sleep_enable = false`, `min_freq_mhz = 80`, `max_freq_mhz = 160`. Worth
   ~30% of the awake current in the reading-a-page state, which is where most
   awake time goes. **Must** ship with an `ESP_PM_CPU_FREQ_MAX` lock held across
   the render/page-turn path — see the caveats under "Idle power"; without the
   lock, page turns get up to 2× slower and it is a net regression. This does
   *not* trip the tickless-idle errata that broke the buttons.
2. **Stop embedding the reader font in the app image.** `_binary_assets_bin_start`
   is ~821 KB — **52% of the entire firmware**. It is then copied into a flash
   partition on first boot, so the font occupies flash twice. Flashing the font
   partition directly (once, like `esp32c3-dev` assumes) and dropping the blob
   from the default build would roughly halve the image and every upload. Costs
   a provisioning step on a virgin board.
3. **Low-battery cutoff.** There is currently none — `battery_percentage()` is
   display-only and the device runs until brownout. See "Battery health" below.
4. **Pre-laying-out the *next* page** after a turn, so the forward path skips
   layout entirely. The cache and its invalidation already exist (see "Refresh
   latency" — `page_cache_` / `take_cached_page_`); all that is missing is a
   `prerender_next_page_()` call after `buf.refresh()`, predicting the next
   position with the same image-snapback logic `next_page_()` uses. **Measure
   first** with the `'P'` `RenderBench` serial command; not worth the complexity
   if render is already fast.
5. **Try `-Os` against `-O2`.** Counterintuitively `-Os` can be *faster* here:
   code executes from flash through a fixed 16 KB cache, so a smaller hot path
   can win more from cache hits than it loses to less aggressive optimization.
   Purely an empirical question — measure with `RenderBench`.

Closed, don't re-investigate: instruction cache size (fixed in C3 silicon, not
configurable as it is on the S3), QIO flash mode, SD clock above 20 MHz,
tickless idle, and region-only refresh for menu cursor movement. See "Build &
flash", "Idle power" and "Refresh latency".

### Battery health

Distinct from battery *life*. Nothing in the firmware protects the cell:
`battery_percentage()` (`runtime.h`) is read only to draw the indicator, and the
device runs until the brownout detector trips. Deep-discharging a Li-ion cell
below ~3.0 V causes permanent capacity loss, and deep sleep keeps drawing ~10 µA
afterwards, taking it lower. A soft cutoff — save state and force deep sleep at a
voltage floor, refuse to wake until charged — is the missing piece. The cell
almost certainly has a hardware protection IC, but that cuts off around 2.5 V,
well past where the damage starts.

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

Left in place, now unreachable from the reader: `kLutGrayscale`,
`kLutGrayscaleRevert`, `EInkDisplay::grayscale_refresh`, `revert_grayscale`,
`DrawBuffer::show_grayscale_image` (no callers at all), and the serial LUT-upload
commands in `main.cpp` that tune them. The one-pass path
(`grayscale_refresh_1pass`) is still live — it is what draws the sleep image.

**Hyperlinks, entirely.** `LinksScreen`, the quick menu's "Links (n)" item, the
reader's nav-history stack (the back/stay gesture that returned from a followed
link), `Run::href`, `LayoutWord::href`, the `<a href>` tracking in `EpubParser`,
the href write path in `MrbWriter`, and the link-underline rendering in
`DrawBuffer::draw_line_`. An `<a>` element now contributes only its text.

One deliberate remnant: `MrbReader` still reads the href length and **skips**
those bytes when a run's flag bit `0x02` is set. MRB caches written before this
change still contain hrefs, and without the skip every run after one would be
misparsed. Don't delete that branch — it is a compatibility shim, not dead code.

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
all the way through: `EpubParser` only fills it when CSS said something, and MRB
round-trips the empty state through the `kMrbAlignDefault` (`0xFF`) sentinel, so
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
shadowed all along. `ReaderOptionsScreen`'s separator-drawing branches went too,
since that screen can no longer contain one.

**`ChapterSelectScreen`**, merged into the quick menu (see Architecture). Note
both `platforms/*/CMakeLists.txt` list core `.cpp` files **explicitly** — deleting
a screen without editing both fails the build at CMake generate time, not compile
time, with a "Cannot find source file" that names the wrong call site.

**On-device EPUB conversion, and with it EPUB support entirely.** A book is a
`.mrb`; `BookIndex` does not recognise `.epub` at all, so an unconverted book is
not listed rather than listed-and-broken. `ReaderScreen` opens the MRB or shows
an error — there is no fallback and no "Converting…" progress screen. The
`Book`, `EpubParser`, `CssParser`, `XmlReader`, `HtmlEntities` and `MrbConverter`
translation units are **out of the firmware build** (they remain in the repo for
`tools/epub2mrb`), as are the `X`/`I`/`D` serial benchmark commands that were
their last callers. `Application::ensure_cover_bin()` went too: with no EPUB
there is nothing to extract from. Together that is ~95 KB of flash.

`ZipReader` **stays** in the firmware despite EPUBs being gone — it is how
embedded images are read, as stored entries pointing into the MRB. `MrbWriter`
does **not**: its only caller is `MrbConverter`, so it was dropped from
`platforms/esp32/CMakeLists.txt` and is now host-only alongside it.

Because those TUs are host-only, every `#ifdef ESP_PLATFORM` inside them was
unreachable, and they have been flattened to the host branch. Gone with them:
`MrbConverter`'s three `benchmark_*` functions (~250 lines, no callers since the
`X`/`I`/`D` commands went), all the `esp_timer` sub-stage instrumentation in
`convert_epub_to_mrb_streaming` and `parse_xhtml_streaming`, the
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
deriving it live would mean opening every MRB *and* every `.pos` on the card
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

### Sleep images (MGR2)

`resources/sleep.mgr`, shipped in the asset blob. A deliberately trivial format
read by `DrawBuffer::Mgr2Source_`:

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
