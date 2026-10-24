# Wintergreen

Xteink X4 e-reader firmware (ESP32-C3, 16 MB flash, e-ink). Fork of Nous ← Microreader. Goal: zero config surface; all fixed at compile time, boots straight into book.

Mostly negative knowledge: hardware-tested failures. "Don't" = cost a debug session. Reopening needs a device, not an argument.

## Notes index (read matching `docs/` file before touching area)
| File | Covers | Never break |
|---|---|---|
| `docs/power-and-input.md` | idle power, DFS, input sampling, holds, power button | no tickless idle / light sleep (kills ADC buttons on battery); `min_freq_mhz` stays 80; never measure hold in frames; `pdMS_TO_TICKS(1)` = 0 ticks; wait GPIO3 HIGH before deep sleep |
| `docs/display.md` | refresh latency, paging, spare framebuffer, panel health | every `EInkDisplay` entry starts `wakeIfNeeded(); waitWhileBusy();`; SD shares SPI2 → `wait_panel_idle()` before card access; keep half-refresh `0x5A` temp write; no idle rail power-down; fwd/back share `require_full_height` |
| `docs/architecture.md` | screens, stack, home/list/quick menu, sorting | `ScreenManager::pop()` keeps last screen; screens pushed from press inherit held button; no `std::sort` (34 KB) |
| `docs/bluetooth.md` | BLE clicker host, heap, OOM rescue | NimBLE never torn down; reserve before growing page-sized things; guard `largest_free_block_bytes()`, never total free; release OOM reserve before `xTaskCreate` |
| `docs/sync.md` | NAS sync over HTTP | radio teardown scope-bound (`WifiGuard`); never call saving `BookIndex` mutators per book; downloads → `.tmp`, must match Content-Length |
| `docs/content.md` | EPUB→WGB, WGB/WGF formats, books on card, fonts, size budget | formats magic-as-version; covers beside literal `book.wgb`; font presets mirror `make_font.py --sizes` |
| `docs/history.md` | open, rejected, removed | don't resurrect without asking |

## Build / flash
```
pio run                 # only env: esp32c3
pio run -t upload
pio device monitor      # 115200
```
- `/dev/ttyACM0`, `303A:1001` (native USB JTAG/serial). `upload_port` unset on purpose (autodetect).
- No desktop build. Only host build = `tools/epub2wgb`. It and `platforms/esp32/CMakeLists.txt` list core sources explicitly: add/delete core `.cpp` → edit both, else CMake fails at generate naming `idf_component_register`.
- `ESP_PLATFORM` (IDF macro) = firmware/host-converter seam. Stays.
- Flash DIO @ 80 MHz (`sdkconfig.defaults`). QIO bricks boot (bootloader quad self-upgrade never finishes). DIO@80 = QIO@40 = 20 MB/s. `board_build.flash_mode` inert under espidf.
- SD clock (`platforms/esp32/sdcard.h`) divides 80 MHz APB: 80/40/26.7/20 only. 40 fails (12.5 ns half-period < 14 ns card output delay; panel fine at 40, write-only). 26.67 fails (empty book list). 20 = ceiling. Card `input_delay` unreachable via `sdspi_device_config_t` (would need IDF patch).

Gotchas:
- `sdkconfig.defaults` change ignored → delete generated `sdkconfig.esp32c3` (not committed).
- `.pio/build/<env>/{memory,sections}.ld` generated, not always regenerated. Linker-script-shaped error (`region 'rtc_reserved_seg' overflowed`) → `rm -rf .pio/build/<env>` first.
- `CONFIG_FATFS_CODEPAGE_437`: dynamic codepage links all OEM tables (+4 CJK) = 478 KB. LFN UTF-8 regardless.
- `display/ui_font_*.h` arrays `inline constexpr`, not `static` (static in header = copy per TU; small font linked 4×, 96 KB).
- Nix machine, no system `python3`/`perl`; scripts run under PlatformIO's interpreter.

## Low-battery cutoff
- Live at 3200 mV: `wintergreen::kLowBatteryCutoffMv` in `Application.h`, NOT `WintergreenConfig.h` (cell/brownout property; wrong value looks bricked). Header documents bounds: protection IC ~2500, damage ~3000, curve zero 3300.
- Below: save state, wordmark, deep sleep; `verify_battery()` in `main.cpp` refuses boot until charged. Depends on `Esp32Runtime::battery_millivolts()`; on-screen % wrong → suspect it first.

Five guards. First three: absence bricked device (boot, one frame, sleep screen, loop; USB Serial/JTAG off between, esptool can't connect).
1. `usb_attached()` real check: old read GPIO20 (U0RXD) high = USB, only true with external UART bridge; X4 native USB → always "no host" → gate fired while plugged in. Now `usb_serial_jtag_is_connected()` (valid before driver install).
2. Low reading must persist wall-clock `kLowBatteryHoldMs` (4 s), not frames (5 frames ≈ 125 ms < one refresh = biggest spike).
3. Never sample while panel busy (`display().is_busy()`).
4. 0 = can't measure (no battery / uncalibrated ADC), also anything < `kImplausibleMv` (2500). `battery_millivolts()` returns 0, not raw count, without calibration.
5. Boot check before `epd.begin()`: refused boot never lights panel or mounts card.

- Shipped default 3300 = 0% on `battery_percentage()` polynomial; normal sag crossed it. Pick below curve zero.
- Recovery: leave on USB (charger IC independent of SoC). Flashing instantly-sleeping device: start upload, hold power. Wake level-triggered GPIO3 LOW, non-`ESP_RST_POWERON` → `verify_wakeup_press()` returns immediately → boot-loops while held, enumerating USB each pass → esptool retry window.
- Charge health (4.2 V hold, thermal) = charger IC; unreachable from firmware.

## SD wear
Writes rare, all deduplicated:

| File | Written | Guard |
|---|---|---|
| `.wintergreen` (settings) | reader/quickmenu pop, sleep | byte-compare vs `settings_written_` |
| `.wintergreen-index` | reader close, library scan, serial mutations | `BookIndex::dirty_` |
| `<book dir>/book.pos` | reader close | `ReaderScreen::pos_written_` |

A session can write nothing: same book, read, sleep → `mark_opened` / `set_progress` don't move values → index clean; pos only if moved; settings only if byte changed.
- `BookIndex` dirty flag: `save()` returns if clean; `mark_opened`/`set_progress` set it only on real change.
- One index write per session, from `record_book_progress()` on reader close (`record_book_opened()` no longer saves).
- Nothing written on book open: all put-downs route via `do_sleep_()` (saves settings); power loss resumes at start position + previous book.
- Settings saved after pop: `save_settings_()` writes `book_path` only while reader on stack; saving first persisted a just-closed book.
- Never add a write to page-turn path (10× wear). `.pos` only from `ReaderScreen::stop()`.

## Configuration
`WintergreenConfig.h` (root, on include path for both builds) = only tunables.

| Constant | Default | Notes |
|---|---|---|
| `WG_BLUETOOTH_PAGE_TURNER` | commented out | `#define`, clicker MAC `"AA:BB:CC:DD:EE:FF"`. Undefined = no NimBLE. |
| `WG_WIFI_SYNC` | defined | `#define`. Undefined drops Wi-Fi, lwIP, `esp_http_client`. |
| `kWifiName` / `kWifiPassword` / `syncServer` | `""` | Read by `platforms/esp32/wifi_sync.h`. `syncServer` never `.local` (mDNS, no resolver); literal IP or router-DNS name. |
| `kSunlightFadingFix` | `false` | Panel off after every full refresh (restores sunlight contrast). No periodic full refreshes → affects sleep image + boot paint only. `Application::start`. |
| `kAutoSleepMinutes` | `1` | `Application::update`. |
| `kPowerHoldSleepMs` | `kHoldDelayMs` (250) | Power hold before sleep; same tap/hold split for all buttons. |
| `kHoldDelayMs` | `250` | Hold before nav auto-repeat. < ~250 ms tap reads as two. |
| `kHoldRepeatMs` | `200` | Repeat gap. |
| `kHoldAccelStep` | `0` | Extra entries per repeat. Shipped `0.9` on `int` → 0, accel never on. Make float for fractions. Reader ignores (pages never accelerate). |

- All `inline constexpr` except the two `#define`s (only preprocessor can drop an IDF stack). `#if config::kBluetoothPageTurner[0] != '\0'` fails (`#if` can't eval C++; error names `::`).
- Must open with `#include <cstdint>`. Gitignored (Wi-Fi password); fresh clone won't build until README block pasted. README block = canonical defaults; keep in step with this table.

Everything else hardcoded at call site. Removed `Application` `static constexpr` getters (`rotate_display()`, `show_sleep_text()`, `list_align()`, `menu_font_size()`, `invert_menu_buttons()`, `invert_bottom_paging()`, `invert_side_buttons()`); don't reintroduce.
- Lists always portrait (`buf.set_rotation(Rotation::Deg90)`), menu font = 32 px header; `ListMenuScreen` `font_size_idx_`/`set_font_size()`/`font_size()`/`apply_ui_font()`/`list_align_` gone.
- No sleep caption. Sleep shows cover of currently open book unless in `/.hidden/` (hidden books never advertised); else embedded wordmark.
- Battery = header %, never icon. Converted-book marker gone (all books WGB).
- Images always render; `images_enabled` gone.
- One reader font (Libron, `font` partition), no picker.
- Book list title-only, most-recently-opened first, hairline before never-opened.
- Fixed geometry: `ReaderSettings::h_padding()`/`v_padding()`; alignment, spacing, sizes from book CSS.

Runtime state: `ReaderSettings::font_size_idx`, `Application::rotate_reader_`, reading position. Hyphenation always on (English; unknown tag → none).

## Repo map
Flattened in `18fa42b`: core `lib/wintergreen/` → `wintergreen/`, `platforms/desktop/` deleted, `LyraExtScreen` → `HomeScreen`, `ReaderOptionsScreen` → `QuickmenuScreen`. Anything saying `lib/` is stale.

```
WintergreenConfig.h        only tunables (gitignored)
CMakeLists.txt             IDF stub; real source list in platforms/esp32/
platformio.ini             env esp32c3; pre-build tools/generate_assets.py
sdkconfig.defaults         checked in; sdkconfig.esp32c3 generated
resources/                 Libron.bin (reader font), sleep.mgr (wordmark)

platforms/esp32/           entry + all drivers, header-only
  main.cpp                 app_main: boot gate, init order, while() loop
  epd.h                    EInkDisplay: SPI2 panel, LUTs, refresh modes
  input.h                  Esp32InputSource: 5 ms ADC-ladder sampler, press ring
  runtime.h                Esp32Runtime: frame pacing, battery %
  sdcard.h                 sd_init(): SDSPI on SPI2, 20 MHz
  serial_communication.h   USB-JTAG command protocol
  bluetooth_clicker.h      BLE HID host page-turner; compiled out by default
  wifi_sync.h              NAS sync; #ifdef WG_WIFI_SYNC
  font_manager.h           ESP32 FontManager: mmaps font partition
  font_partition.h         RawPartition: find + esp_partition_mmap
  default_16MB.csv         partition table

wintergreen/               portable core
  Application.{h,cpp}      screen ownership, ScreenId, settings I/O, sleep screen
  ScreenManager.h          8-deep stack, start/stop/pause/resume
  Input.h                  Button, ButtonState, HoldRepeat
  Runtime.h                IRuntime
  FontManager.h            WGFS bundle → BitmapFontSet
  screens/
    IScreen.h
    ListMenuScreen.{h,cpp} base list: header, rows, scroll, hold-repeat, fit_prefix
    HomeScreen.{h,cpp}     recents carousel; overrides draw_all_
    MainMenu.{h,cpp}       book list + Sync row + hidden-books mode
    ReaderScreen.{h,cpp}   paging, images, .pos, page cache
    QuickmenuScreen.{h,cpp} in-book menu + inline TOC
  display/
    DrawBuffer.h           IDisplay + double buffer + all drawing
    ui_font_{small,large,header}.h   Iosevka Slab Medium 14/24/32 px
  content/
    BitmapFont{,Format}.h  WGF1 reader / format doc
    ContentModel.h         Paragraph/Run/TextStyle; kMaxFontSizes = 8
    TextLayout.{h,cpp}     line breaking, pagination, page cache, next/prev steps
    PageHistory.h          ring of recent forward turns; back retraces exactly
    IParagraphSource.h     what TextLayout pages over
    BookIndex.{h,cpp}      singleton index + StringPool, .wintergreen-index
    StringPool.h           StringRef arena
    CoverPaths.{h,cpp}     cover_*.bin + book.pos paths, kHomeCover/kSleepCover
    wgb/WgbFormat.h        on-disk WGB (magic WGB2, no version field)
    wgb/WgbReader.{h,cpp}  device read path
    wgb/WgbWriter.{h,cpp}  host-only
    wgb/WgbConverter.*     host-only EPUB → WGB
    Book/EpubParser/CssParser/XmlReader/HtmlEntities   host-only
    ZipReader.{h,cpp}      host-only
    ImageDecoder/JpegDecoder/PngDecoder/stb_image.h    host-only
    hyphenation/           Liang, English only (27 KB trie)

docs/                      deep-dive notes (index above)

tools/
  generate_assets.py       pre-build: font/sleep flash images
  make_font.py             TTF → WGF1 / WGFS; --mono mandatory
  check_font.py            validate bundle, ASCII-render sample
  make_sleep_image.py      PNG → MGR2
  convert-books.sh         nix-shell wrapper for epub2wgb
  merge-bin.sh             single flash-at-0x0 release image
  epub2wgb/                host converter (own CMakeLists, explicit sources)
  tests/                   host checks; run.sh
```

### Where behaviour lives
| Question | Read |
|---|---|
| Boot order, deep-sleep entry, serial op dispatch | `platforms/esp32/main.cpp` |
| Power button, auto-sleep, transitions, settings file | `Application::update` / `save_settings_` |
| List look + scroll | `ListMenuScreen::draw_list_` + `get_visible_count_` |
| Page turn latency | `EInkDisplay::partial_refresh` + `ReaderScreen::render_page_` |
| Button behavior | `input.h` thresholds → `Application::update` → screen `update()` |
| Cover look | `HomeScreen::load_cover_`, `Application::show_book_cover_sleep_` |

## Serial protocol
- `platforms/esp32/serial_communication.h`: FreeRTOS task `serial_rx` (8 KB stack), USB-JTAG, 2 KB tx / 4 KB rx (~14 KB RAM, only once host attached).
- File management only; no debug console. Commands: `A` dir list, `B` inject buttons, `K` mkdir, `L` list books, `N` rename, `O` open book, `R` recursive delete, `T` read file, `W` upload, `Z` clear `/sdcard/sleep/`. Frame magics `EPUB`, `SIMG`, `CMND`.
- Add/Rename/Remove don't run in receiver: handed to `main.cpp` via single-slot SPSC `g_index_op`, deferred while reader owns scratch buffers or panel refreshing.
- Lazy start: `serial_start_if_connected()` from main loop first time `usb_serial_jtag_is_connected()` true; no-op after. On battery: never allocated, no 50 ms wakeups. Polling the check is free (reads peripheral, no driver).

## No logging
- No log statements anywhere (`ESP_LOGx`, `MR_LOGI`, `HEAP_LOG`; `wintergreen/HeapLog.h` deleted). Temporary ones must be removed.
- With `CONFIG_LOG_DEFAULT_LEVEL_NONE` macros were already empty (format strings not in `firmware.bin`); savings came from removed diagnostic commands + timing scaffolding.
- Removing a log that was sole body of unbraced `if`/`else` re-points branch to next statement: 9 places, compiler caught 2. Bulk strip → scan original source for control statement followed by log macro.

## Assets flashed, not embedded
Raw partitions in `platforms/esp32/default_16MB.csv`:

| Partition | Offset | Size | Holds |
|---|---|---|---|
| `font` | `0xC90000` | 1 MB | inflated WGFS (Libron), 354,585 B |
| `sleep` | `0xD90000` | 128 KB | `resources/sleep.mgr` verbatim, 96,008 B |

- `tools/generate_assets.py` pre-build: inflates `resources/Libron.bin` (`[uint32 raw size][zlib]`) on build machine, copies `sleep.mgr`, writes to `$BUILD_DIR`, checks partition size, appends to `FLASH_EXTRA_IMAGES` (one esptool run, 4 images). Rewrites only on byte change.
- Runtime: `RawPartition::map()` (`font_partition.h`) = `esp_partition_find_first` + `esp_partition_mmap`; no header/CRC/copy/inflate. `FontManager::init()` parses WGFS from mapping; `main.cpp` passes sleep mapping to `DrawBuffer::set_sleep_image()`.
- Replaced first-boot provisioning (~15 s "Installing fonts...", font stored twice).
- New asset = edit 3 in step: CSV, `PARTITIONS` in `generate_assets.py`, `map()` call site. Python asserts fit.
- Font partition holds inflated 355 KB, not 104 KB compressed. Intentional.

## Persistence
No firmware-made dirs on card. Two hidden root files + one per book:

| What | Where | Written |
|---|---|---|
| settings | `/sdcard/.wintergreen` | sleep, reader/quickmenu pop |
| index | `/sdcard/.wintergreen-index` | reader close, library scan, serial mutations |
| position | `<book dir>/book.pos` | reader close |

- No `.wintergreen/` folder; `main.cpp` mkdirs nothing.
- Position needs no key: was `.wintergreen/<fnv1a>.pos` (opaque names, collision risk, reset on metadata edit). `book_pos_path()` = book's folder; travels/deletes with book. `make_book_key()`, `fnv1a_32` gone.
- Settings + index stay two files: settings tiny every sleep, index ~30 KB once per session.
- `save_settings_`/`load_settings_`: flat `key=value`, three keys: `book_path`, `font_size`, `rotate_reader`. `book_path` written only while reader on stack; presence = resume on boot.
- Book-list cursor not persisted: `MainMenu::stop()` keeps `initial_selection_` in-session; deep-sleep wake = full boot; row 0 is last-read anyway.
- Removed, don't re-add: `v=1`, `screen`, `last_version`, `open_counter`. `open_counter` was a bug (settings counter feeding index `last_open_order`; drift reset sorting). `BookIndex::mark_opened()` now uses `max(last_open_order)` in same pass.
- `.pos` = four numbers: chapter, paragraph, offset, text offset. Only from `ReaderScreen::stop()`. Safe because auto-sleep + power both route via `Application::do_sleep_()`, which stops the screen, and explicitly stops reader when quick menu on top (`screen_mgr_.top()->stop()` alone misses it). Skipped if unmoved (`pos_written_`).
- `rename()` doesn't replace here: `vfs_fat_rename` → `f_rename` → `FR_EXIST` if dest exists. Both save paths `remove()` target right before rename. Without it only first save worked; later failed silently leaving `.tmp`. `.tmp` beside target on card = this symptom.
- Remove→rename window: `load_settings_()` falls back to `settings.tmp` (always complete).
- `.tmp` + `rename()` is atomic replace, not backup. No `.bak.N` (removed). Never direct `fopen(path, "w")` on either file (truncates before write; settings saved on auto-sleep = low battery). RAM staging doesn't help (truncate at `fopen`; FAT data/chain/dirent separate writes).
- `save_settings_()` builds `std::string`, compares `settings_written_`, skips if same (called every sleep/open/pop). Starts empty → first save after boot always writes (normalises old files).

## Conventions
- Two-space indent, `snake_case_` private members, `k` constants, `Deg90` = portrait.
- Screens are `Application` members, not heap; `free_items_storage()` in `stop()` frees list RAM, keeps cursor.
- Hyphenation English only (`content/hyphenation/Liang/`); `HyphenationLang` = `None`, `English`.
- README attribution to Nous/Microreader intentional; keep.

### Text truncation
`fit_prefix()` (`screens/ListMenuScreen.h`) = only place measuring how much string fits a width. Old inlined loops were:
- O(n²): re-measured prefix via `word_width(text, fit + cb)` each step. `fit_prefix` sums `char_width` per codepoint.
- Unbounded: `while (*p)` on `string_view`s into `StringPool` (not NUL-terminated; see docs/content.md "Books on the card").

Per-char sum slightly wider than `word_width()` (which applies class kerning, mostly negative) → cuts ≤1 char early, never overruns. Need exact kerned width → `word_width` once on final byte count (draw path does).

### Tests
Host-side, no framework, one `main()` + asserts each, ASan/UBSan. `tools/tests/run.sh` builds + runs all; pass books (`.wgb`, `.epub`, or dir) for pagination/layout-memory (EPUBs go through sanitized `epub2wgb`). No system compiler:
```
nix-shell -p gcc cmake gnumake --run 'tools/tests/run.sh ~/books'
```

| Test | Covers |
|---|---|
| `json_scan_test.cpp` | sync reply scanner, every truncation of a valid reply |
| `layout_memory_test.cpp` | 24 KB ceiling per page layout via instrumented `operator new`, every page. Proportional stand-in font, not `FixedFont` (fixed width hides estimate error) |
| `paginate_book_test.cpp` | whole-book paging with device steps (`next_page_start`, `predecessor_start`, `PageHistory`): no stall, no blank page, fwd-then-back retraces, cold backward reaches origin page. Cold-path start diffs counted, not failed |
| `book_index_test.cpp` | index parse/save round trip |
| `blank_para_test.cpp` | empty paragraphs |
| `stretch_lut_test.cpp` | cover scaler stretch tables |
