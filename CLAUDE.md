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

Flash is QIO (`CONFIG_ESPTOOLPY_FLASHMODE_QIO` in `sdkconfig.defaults`). The image
header still reports `dio` — that is by design, the bootloader upgrades itself to
quad mode during init. Don't "fix" it. If a board won't boot, switch to
`CONFIG_ESPTOOLPY_FLASHMODE_DIO`.

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

`wintergreen_config.h` **in the project root** is the only place with tunable
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
