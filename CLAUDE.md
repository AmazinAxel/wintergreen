# Wintergreen

Firmware for the Xteink X4 e-reader (ESP32-C3, 16 MB flash, e-ink panel). A fork of
Nous, itself a fork of Microreader. The design goal is a reader with no
configuration surface: everything is fixed at compile time so the device boots
straight into a book.

## Build & flash

```
pio run                       # build (default env: esp32c3)
pio run -t upload             # build + flash over USB
pio run -e esp32c3-dev -t upload   # ~940 KB image, no embedded reader font
pio device monitor            # serial log, 115200
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
in sync when adding or removing core `.cpp` files. The `esp32c3-qemu` env is
currently **broken**: it references `sdkconfig.qemu`, which does not exist, and
`tools/run_qemu.py` is gone too. Either restore both or delete the env.

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

Input timing is chosen for responsiveness: 5 ms button sampling (`input.h`) under
a 25 ms UI frame (`main.cpp`), so a press lands on the next frame. Screens repaint
only on state changes, so frame rate does not drive panel traffic.

Auto-sleep never fires while USB is connected — `main.cpp` calls `keep_awake()`
every iteration when `usb_serial_jtag_is_connected()`, which zeroes
`inactivity_ms_`. Deliberate, but it means the timeout can only be tested on
battery. `Esp32Runtime::frame_time_ms()` returns the **measured** duration of the
last frame, not the nominal target; `Application` accumulates it into the
auto-sleep countdown, so returning the constant made that clock drift whenever a
frame overran.

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
| `kAutoSleepMinutes` | `3` | Intended values 2, 3 or 5. Read by `Application::sleep_timeout_min()`. |

Everything else is hardcoded. `Application` exposes them as `static constexpr`
member functions so call sites read unchanged:

- portrait lists (`rotate_display()`), X-Large menu font (`kMenuFontSize = 3`)
- no sleep caption (`show_sleep_text()`), sleep screen = most recent book's cover
- book images always on, nav arrows on, converted-book marker on, battery icon
- default button mapping (`invert_bottom_paging()` is `true` — that is the default)
- single reader font (Literata, `FontManager::kFontAsset`), no picker
- book list is title-only, sorted most-recently-opened first with a hairline
  divider before never-opened books

`rotate_reader_` is the one genuinely runtime setting — landscape reading is
reachable from the in-book quick menu and persists.

## Architecture

`Application` (`lib/wintergreen/Application.{h,cpp}`) owns every screen as a
by-value member and a `ScreenManager` stack. Screens never push directly; they
call `push_screen`/`replace_screen`/`pop_screen`, which queue a `ScreenId` that
the app applies after `update()` returns. `screen_for_(ScreenId)` maps the enum
to the member.

Screen stack: `LyraExtScreen` (home) → `MainMenu` (all books) / `RecentBooksScreen`
→ `ReaderScreen` → `ReaderOptionsScreen` (quick menu) → `ChapterSelectScreen` /
`LinksScreen`. `HiddenBooksMenu` is reached by a ~3 s back long-press on MainMenu.

### ListMenuScreen

Base class for every list screen, in `screens/ListMenuScreen.{h,cpp}`. There is
**one** visual theme (formerly "Lyra Extended"); the `MenuTheme` enum and the
Chronicle/Minimal/Stele/Codex/Lyra variants are gone. Two per-instance booleans
select the remaining layout variations:

- `detail_list_` — two-line rows (title + subtitle, full-width divider, right-hand
  column). Set by MainMenu, RecentBooksScreen, HiddenBooksMenu.
- `plain_list_` — centred-title header and battery/nav-glyph footer instead of the
  wordmark status bar and labelled nav boxes. Set by the reader overlays
  (ChapterSelectScreen, LinksScreen).

Both false = standard wordmark header + centred single-line rows. A non-empty
`subtitle_` switches the header into the book-details card and is checked before
either flag. `header_override_` replaces the "wintergreen" wordmark per screen
(HiddenBooksMenu uses "secret").

Drawing is four passes: `draw_header_` → `draw_bottom_` → `draw_list_`, with
`compute_header_h_` mirroring the header maths for scroll calculations.
`get_visible_count_` must stay consistent with `draw_list_`'s branch selection —
they are separate functions keyed on the same conditions, and divergence causes
scroll bugs.

### Content pipeline

EPUB → MRB (a preprocessed binary layout format) → rendered pages.
`content/mrb/MrbConverter` does the conversion; `MrbReader` is the read path.
Conversion on-device is slow, so the intent is to convert on a computer and copy
`.mrb` files to the SD card — but `MrbConverter` is still compiled in and is
still used as a fallback when a book is opened without a cached MRB.

`BookIndex` is a singleton holding the scanned book list plus reading stats,
backed by a `StringPool`. It has a `generation()` counter — MainMenu polls it so
serial uploads/deletes refresh the visible list without navigating away.

### Fonts

MBF4 bitmap format, documented in `content/BitmapFontFormat.h`. UI fonts are
embedded headers in `display/ui_font_*.h`; the reader font lives in the appended
asset blob (`platforms/esp32/asset_blob.*`) and is provisioned into a flash
partition on first boot after a firmware update, keyed on CRC.

Glyph coverage is per-font and sparse. A font that parses fine can still be
missing the glyphs for a given string, in which case it renders blank rather than
falling back — check `glyph_data(cp).bits != nullptr` before assuming a font can
render text. (This bit the old brand wordmark font: it contained only `n`, `o`,
`u`, `s`.) There is currently **no wintergreen logotype font** — the wordmark is
drawn in the regular UI font. Dropping in a real cut means adding a brand font
header and using it in `ListMenuScreen::draw_header_`.

### Persistence

`Application::save_settings_` / `load_settings_` write a flat `key=value` file at
`<data_dir>/settings`. It holds only genuine runtime state: last screen, book
path, list selection, reader display settings, open counter, `rotate_reader`,
last seen version.

Both the settings file and the book index write to a `.tmp` and then `rename()`
over the target. There are **no** `.bak.N` rotations — deliberately removed; the
atomic rename is what protects against a power cut mid-write, and the backups
were redundant clutter. Don't reintroduce a direct `fopen(path, "w")` on either
file: that truncates immediately and a power loss would lose reading position.

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

1. **`kAutoSleepMinutes` 3 → 1** (`wintergreen_config.h`). The single largest
   battery lever, now that the boot-speed work has made deep-sleep wake cheap.
   Gated only on eyeballing wake time on hardware.
2. **Dynamic frequency scaling.** `CONFIG_PM_ENABLE` with
   `light_sleep_enable = false`, `min_freq_mhz = 80`, `max_freq_mhz = 160`. Worth
   ~30% of the awake current in the reading-a-page state, which is where most
   awake time goes. **Must** ship with an `ESP_PM_CPU_FREQ_MAX` lock held across
   the render/page-turn path — see the caveats under "Idle power"; without the
   lock, page turns get up to 2× slower and it is a net regression. This does
   *not* trip the tickless-idle errata that broke the buttons.
3. **Stop embedding the reader font in the app image.** `_binary_assets_bin_start`
   is ~821 KB — **52% of the entire firmware**. It is then copied into a flash
   partition on first boot, so the font occupies flash twice. Flashing the font
   partition directly (once, like `esp32c3-dev` assumes) and dropping the blob
   from the default build would roughly halve the image and every upload. Costs
   a provisioning step on a virgin board.
4. **Low-battery cutoff.** There is currently none — `battery_percentage()` is
   display-only and the device runs until brownout. See "Battery health" below.
5. **Page pre-rendering** during idle, to make page turns feel instant. Measure
   with the existing `RenderBench` serial command first; not worth the complexity
   if render is already fast.
6. **Try `-Os` against `-O2`.** Counterintuitively `-Os` can be *faster* here:
   code executes from flash through a fixed 16 KB cache, so a smaller hot path
   can win more from cache hits than it loses to less aggressive optimization.
   Purely an empirical question — measure with `RenderBench`.

Closed, don't re-investigate: instruction cache size (fixed in C3 silicon, not
configurable as it is on the S3), QIO flash mode, SD clock above 20 MHz,
tickless idle. See "Build & flash" and "Idle power".

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

**exFAT**, along with `tools/patch_ffconf.py` which enabled it. SD cards are
FAT32, 16 GB or smaller — that is the supported configuration, and an exFAT card
will simply fail to mount. exFAT cost 7,892 bytes of flash and required patching
`ffconf.h` inside the ESP-IDF package on every build; ESP-IDF ships it disabled
because it is patent-encumbered, so there is no Kconfig option for it.

The embedded wintergreen sleep image is intentionally **kept**:
`do_sleep_(buf, wordmark_image = true)` shows it instead of the book cover, ready
for a power-button long-press gesture that is not wired up yet.
