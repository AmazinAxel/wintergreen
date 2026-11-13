# Wintergreen

Xteink X4 e-reader firmware (ESP32-C3, 16 MB flash, e-ink). Fork of Nous ← Microreader. Goal: zero config surface; all fixed at compile time, boots straight into book.

Mostly negative knowledge: hardware-tested failures. "Don't" = cost a debug session. Reopening needs a device, not an argument.

## Notes index (read matching `docs/` file before touching area)
| File | Covers | Never break |
|---|---|---|
| `docs/power-and-input.md` | idle power, DFS, input sampling, holds, power button | no tickless idle / light sleep (kills ADC buttons on battery); `min_freq_mhz` stays 80; never measure hold in frames; first down frame of a hold adds no dt (idle loop: dt = wait before press); async state change → `ui_wake()` or it shows at next deadline (≤1 s); `pdMS_TO_TICKS(1)` = 0 ticks; wait GPIO3 HIGH before deep sleep; sleep pin holds released first in `app_main` |
| `docs/display.md` | refresh latency, paging, spare framebuffer, panel health | every `EInkDisplay` entry starts `wakeIfNeeded(); waitWhileBusy();` (BUSY falling-edge ISR, not tick poll); SD shares SPI2 → `wait_panel_idle()` before card access; keep half-refresh `0x5A` temp write; no idle rail power-down; device never lays out: page = one baked display list read into `ReaderScreen::page_buf_` (`stage_next_page_` reads next one before refresh, never under a waveform) |
| `docs/architecture.md` | screens, stack, home/list/quick menu, sorting | `ScreenManager::pop()` keeps last screen; screens pushed from press inherit held button; no `std::sort` (34 KB) |
| `docs/bluetooth.md` | BLE clicker host, heap | raw NimBLE, no esp_hidh (removed): encrypt, then own CCCD writes, keys = notifications in `conn_cb`; `L2CAP_UPDATE_REQ` has null `self_params` (edit `peer_params`); waits on events only (event group); NimBLE never torn down; never `esp_bt_controller_deinit` (clicker gone till reboot); nothing freed for radios (index, page buffer stay; spare only lent to sync downloads, see docs/sync.md) |
| `docs/sync.md` | NAS sync over HTTP | radio teardown scope-bound (`WifiGuard`); never call saving `BookIndex` mutators per book; downloads → `.tmp`, must match Content-Length, resume via Range; all buttons dead while syncing; download serial (writer task measured no faster, single core); clicker dropped via `begin_sync()`, reconnected by `wg_sync::poll()` |
| `docs/content.md` | EPUB→WGB, WGB/WGF formats, books on card, fonts, size budget | no magic/version in any format (WGB, WGF, WGFS, MGR2, index); WGF layout change = regenerate assets same commit; font bundle / `TextLayout` / `PageGeometry.h` / hyphenation change = rebake library same commit (pages fully baked to display lists, unchecked); covers beside literal `book.wgb`; font presets mirror `make_font.py --sizes` |
| `docs/history.md` | open, rejected, removed | don't resurrect without asking |

## Build / flash
```
pio run                 # only env: esp32c3
pio run -t upload
pio device monitor      # 115200
```
- `/dev/ttyACM0`, `303A:1001` (native USB JTAG/serial). `upload_port` unset on purpose (autodetect).
- No desktop build. Only host build = `tools/books`. It and `platforms/esp32/CMakeLists.txt` list core sources explicitly: add/delete core `.cpp` → edit both, else CMake fails at generate naming `idf_component_register`. Exception: `display/DrawBuffer.cpp` is firmware-only (converter includes `DrawBuffer.h` for constants, never draws).
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

Guards. First two: absence bricked device (boot, one frame, sleep screen, loop; USB Serial/JTAG off between, esptool can't connect).
1. No USB bypass (removed 2026-11-11, owner's call; no `usb_serial_jtag` read anywhere). Old bypass read GPIO20, always "no host" on native USB. Plugged-in flat cell refuses boot until charger lifts it past cutoff.
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
| `.wintergreen-index` | reader close, library scan, sync | `BookIndex::dirty_` |
| `<book dir>/book.pos` | reader close | `ReaderScreen::pos_written_` |
| `.wintergreen-thumbs` | index save after add/remove (sync, scan) | `BookIndex::thumbs_dirty_` |

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
| `WG_BLUETOOTH_PAGE_TURNER` | defined | `#define`, clicker MAC `"AA:BB:CC:DD:EE:FF"`. Undefined = no NimBLE, no quick-menu row. |
| `WG_WIFI_SYNC` | defined | `#define`. Undefined drops Wi-Fi, lwIP, `esp_http_client` and the book list's Sync row. |
| `kWifiName` / `kWifiPassword` / `syncServer` | `""` | Read by `platforms/esp32/wifi_sync.h`. `syncServer`: literal IP, router-DNS name, or `.local` (lwIP one-shot mDNS query, `CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES`; no `esp_mdns`). Resolved IP cached in NVS per name; failed sync re-resolves once. |
| `kSunlightFadingFix` | `false` | Panel off after every full refresh (restores sunlight contrast). No periodic full refreshes → affects sleep image + boot paint only. `Application::start`. |
| `kAutoSleepMinutes` | `1` | `Application::update`. |
| `kResumeOnePageBack` | `false` | Fresh open steps back one page (re-shows last page read). `ReaderScreen::resume_one_page_back_`. |
| `kHoldDelayMs` | `250` | Hold before nav auto-repeat; also power hold before sleep. < ~250 ms tap reads as two. |
| `kBluetoothHoldDelayMs` | `100` | Clicker hold → Undo + Back/Confirm (page already turned on press). Tap held longer reads as hold. `bluetooth_clicker.h`. |
| `kHoldRepeatMs` | `100` | Repeat gap. |
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
- No placeholder/empty-state/error text anywhere ("No items", "No books", "Failed to open book" all gone). Empty = blank; failed book open pops silently.
- Two rotations only: `Rotation::Deg0` (reader landscape) / `Deg90`. Reversed ones and their draw branches gone.
- Fixed geometry: `ReaderSettings::h_padding()`/`v_padding()`; alignment, spacing, sizes from book CSS.

Runtime state: `ReaderSettings::font_size_idx`, `Application::rotate_reader_`, reading position. Hyphenation always on at bake time (English; unknown tag → none); device has none.

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
    ReaderScreen.{h,cpp}   page index, draws baked display lists, images, .pos, pre-draw
    QuickmenuScreen.{h,cpp} in-book menu + numbered chapter grid
  display/
    DrawBuffer.h           IDisplay + draw buffer + fills/spare (host-safe constants)
    DrawBuffer.cpp         text + 1-bit blit (byte-packed, both rotations; column-major/band paths for portrait); firmware CMake only
    ui_font_{large,header}.h   Iosevka Slab Medium 24/32 px
  content/
    BitmapFont{,Format}.h  WGF1 reader / format doc
    ContentModel.h         Paragraph/Run/TextStyle; kMaxFontSizes = 8
    TextLayout.{h,cpp}     host-only: line breaking, page fitting, next_page_start
    PageGeometry.h         host-only: reader page options the paginator bakes with
    IParagraphSource.h     host-only: what TextLayout pages over
    BookIndex.{h,cpp}      singleton index + StringPool, binary .wintergreen-index, thumbnail pack
    StringPool.h           StringRef arena
    CoverPaths.{h,cpp}     cover_*.bin + book.pos paths, kHomeCover/kSleepCover
    wgb/WgbFormat.h        on-disk WGB (no magic, no version field), page records, display lists
    wgb/WgbReader.{h,cpp}  device read path: header, page records, display lists, image refs on demand
    wgb/WgbPageView.h      parses one display list in place (device draw, bake check, tests)
    wgb/WgbParagraphs.*    host-only: paragraph stream of the bake's intermediate file, WgbPageSource
    wgb/WgbPaginator.*     host-only: lays out the 4 layouts, writes final file (no paragraph stream)
    wgb/WgbWriter.{h,cpp}  host-only
    wgb/WgbConverter.*     host-only EPUB → WGB
    Book/EpubParser/CssParser/XmlReader/HtmlEntities   host-only
    ZipReader.{h,cpp}      host-only
    ImageDecoder/JpegDecoder/PngDecoder   host-only (streaming; no stb_image)
    hyphenation/           host-only: Liang, English only (27 KB trie)

docs/                      deep-dive notes (index above)

tools/
  generate_assets.py       pre-build: font/sleep flash images
  make_font.py             TTF → WGF1 / WGFS; always hinted 1-bit (--mono accepted, no-op)
  wgf_columns.py           append column-major glyph copy to WGFs (Libron.bin, ui_font_*.h); idempotent
  check_font.py            validate bundle, ASCII-render sample
  make_sleep_image.py      PNG → MGR2
  merge-bin.sh             single flash-at-0x0 release image
  books/                   `books` (flake package): review EPUBs in ~/Downloads/books (title/author/cover sixel/chapters), rewrite in place (wg.ncx, dropped = class="wgdrop"), bake, tar | ssh to homelab. Own CMakeLists, explicit sources; no flags
  tests/                   host checks; run.sh
```

### Where behaviour lives
| Question | Read |
|---|---|
| Boot order, deep-sleep entry, main loop | `platforms/esp32/main.cpp` |
| Power button, auto-sleep, transitions, settings file | `Application::update` / `save_settings_` |
| List look + scroll | `ListMenuScreen::draw_list_` + `get_visible_count_` |
| Page turn latency | `EInkDisplay::partial_refresh` + `ReaderScreen::render_page_` |
| Button behavior | `input.h` thresholds → `Application::update` → screen `update()` |
| Cover look | `HomeScreen::load_cover_`, `Application::show_book_cover_sleep_` |

## USB = flashing only
- No serial protocol, no USB driver install, no console. `serial_communication.h` (upload/delete/rename/open/inject commands, `g_index_op` handoff) removed 2026-11-10; don't re-add. Books arrive only by Wi-Fi sync.
- No USB detection at all (removed 2026-11-11): auto-sleep and low-battery cutoff run while plugged. Flash within `kAutoSleepMinutes` of waking, or start upload and hold power. `keep_awake()` = sync only.

## No logging
- No log statements anywhere (`ESP_LOGx`, `MR_LOGI`, `HEAP_LOG`; `wintergreen/HeapLog.h` deleted). Temporary ones must be removed.
- With `CONFIG_LOG_DEFAULT_LEVEL_NONE` macros were already empty (format strings not in `firmware.bin`); savings came from removed diagnostic commands + timing scaffolding.
- Removing a log that was sole body of unbraced `if`/`else` re-points branch to next statement: 9 places, compiler caught 2. Bulk strip → scan original source for control statement followed by log macro.

## Assets flashed, not embedded
Raw partitions in `platforms/esp32/default_16MB.csv`:

| Partition | Offset | Size | Holds |
|---|---|---|---|
| `font` | `0xC90000` | 1 MB | inflated WGFS (Libron) incl. column-major copy, 573,486 B |
| `sleep` | `0xD90000` | 128 KB | `resources/sleep.mgr` verbatim, 96,004 B |

- `tools/generate_assets.py` pre-build: inflates `resources/Libron.bin` (`[uint32 raw size][zlib]`) on build machine, copies `sleep.mgr`, writes to `$BUILD_DIR`, checks partition size, appends to `FLASH_EXTRA_IMAGES` (one esptool run, 4 images). Rewrites only on byte change.
- Runtime: `RawPartition::map()` (`font_partition.h`) = `esp_partition_find_first` + `esp_partition_mmap`; no header/CRC/copy/inflate. `FontManager::init()` parses WGFS from mapping; `main.cpp` passes sleep mapping to `DrawBuffer::set_sleep_image()`.
- Replaced first-boot provisioning (~15 s "Installing fonts...", font stored twice).
- New asset = edit 3 in step: CSV, `PARTITIONS` in `generate_assets.py`, `map()` call site. Python asserts fit.
- Font partition holds inflated 574 KB, not compressed. Intentional.
- Every WGF (Libron bundle, `ui_font_*.h`) carries each glyph twice: row-major + column-major (`col_bitmap_offset`/`col_index_offset`, appended by `tools/wgf_columns.py`, idempotent, derived from existing bitmaps so pixels never change). Portrait glyphs = `blit_cols_` byte copy, no transpose. `make_font.py` calls `add_columns()`; hand-edited font without it still draws (transpose path).

## Persistence
No firmware-made dirs on card. Three hidden root files, one in `.hidden/`, + one per book:

| What | Where | Written |
|---|---|---|
| settings | `/sdcard/.wintergreen` | sleep, reader/quickmenu pop |
| index | `/sdcard/.wintergreen-index` | reader close, library scan, sync |
| hidden titles | `/sdcard/.hidden/.wintergreen-index` | hidden reveal, only when hidden set changed |
| thumbnail pack | `/sdcard/.wintergreen-thumbs` | index save after books added/removed (sync, scan) only |
| position | `<book dir>/book.pos` | reader close |

- Index is binary (`BookIndex.cpp` `kIndexRecordBytes`): `[u16 count][count × {u16 path/title/author len, u32 last_open_order, u8 pct}][strings]`. Sizes must sum to file size, else `load()` false → list rescans (old text index after update = one rescan). Strings may now contain `|`.
- Hidden titles (`MainMenu::scan_hidden_`): text `path|title|author` cache; folder walk still runs (finds hand-added/removed books); only uncached WGBs opened. Book replaced in place under same path keeps old title until set changes.
- Thumbnail pack (`BookIndex.h`): all `cover_list.bin` in index order, one slot each, keyed by path FNV. Book list opens it once per draw (`end_thumbs()` closes), not one root-dir walk per row. Cache only: key mismatch or missing pack → book's own `cover_list.bin`. Never written on reader close (`thumbs_dirty_` only from add/remove/rescan). Old slots copied; books added since last pack (`fresh_keys_`) re-read from their file (re-synced cover). Missing after firmware update until next sync with changes.

- No `.wintergreen/` folder; `main.cpp` mkdirs nothing.
- Position needs no key: was `.wintergreen/<fnv1a>.pos` (opaque names, collision risk, reset on metadata edit). `book_pos_path()` = book's folder; travels/deletes with book. `make_book_key()`, `fnv1a_32` gone.
- Settings + index stay two files: settings tiny every sleep, index ~30 KB once per session.
- `save_settings_`/`load_settings_`: flat `key=value`, three keys: `book_path`, `font_size`, `rotate_reader`. `book_path` written only while reader on stack; presence = resume on boot.
- Book-list cursor not persisted: `MainMenu::stop()` keeps `initial_selection_` in-session; deep-sleep wake = full boot; row 0 is last-read anyway.
- Removed, don't re-add: `v=1`, `screen`, `last_version`, `open_counter`. `open_counter` was a bug (settings counter feeding index `last_open_order`; drift reset sorting). `BookIndex::mark_opened()` now uses `max(last_open_order)` in same pass.
- `.pos` = two numbers: page start's paragraph record offset + text offset (layout-independent; `find_page` maps to current font/rotation). Only from `ReaderScreen::stop()`. Safe because auto-sleep + power both route via `Application::do_sleep_()`, which stops the screen, and explicitly stops reader when quick menu on top (`screen_mgr_.top()->stop()` alone misses it). Skipped if unmoved (`pos_written_`).
- `rename()` doesn't replace here: `vfs_fat_rename` → `f_rename` → `FR_EXIST` if dest exists. Both save paths `remove()` target right before rename. Without it only first save worked; later failed silently leaving `.tmp`. `.tmp` beside target on card = this symptom.
- Remove→rename window: `load_settings_()` falls back to `settings.tmp` (always complete).
- `.tmp` + `rename()` is atomic replace, not backup. No `.bak.N` (removed). Never direct `fopen(path, "w")` on either file (truncates before write; settings saved on auto-sleep = low battery). RAM staging doesn't help (truncate at `fopen`; FAT data/chain/dirent separate writes).
- `save_settings_()` builds `std::string`, compares `settings_written_`, skips if same (called every sleep/open/pop). Starts empty → first save after boot always writes (normalises old files).

## Conventions
- Two-space indent, `snake_case_` private members, `k` constants, `Deg90` = portrait.
- Screens are `Application` members, not heap; `free_items_storage()` in `stop()` frees list RAM, keeps cursor.
- Hyphenation English only, host-only (`content/hyphenation/Liang/`); `HyphenationLang` = `None`, `English`.
- Open work: `docs/history.md` "Still open".
- README attribution to Nous/Microreader intentional; keep.

### Text truncation
`fit_prefix()` (`screens/ListMenuScreen.h`) = only place measuring how much string fits a width. Old inlined loops were:
- O(n²): re-measured prefix via `word_width(text, fit + cb)` each step. `fit_prefix` sums `char_width` per codepoint.
- Unbounded: `while (*p)` on `string_view`s into `StringPool` (not NUL-terminated; see docs/content.md "Books on the card").

Per-char sum slightly wider than `word_width()` (which applies class kerning, mostly negative) → cuts ≤1 char early, never overruns. Need exact kerned width → `word_width` once on final byte count (draw path does).

### Tests
Host-side, no framework, one `main()` + asserts each, ASan/UBSan. `tools/tests/run.sh` builds + runs all; pass books (`.wgb`, `.epub`, or dir) for the page-section test (EPUBs baked by sanitized `convert_epubs`, the books pipeline without prompts). No system compiler:
```
nix shell nixpkgs#gcc nixpkgs#cmake nixpkgs#gnumake -c tools/tests/run.sh ~/books
```

| Test | Covers |
|---|---|
| `json_scan_test.cpp` | sync reply scanner, every truncation of a valid reply |
| `baked_line_test.cpp` | display-list draw pixel-identical to the PageContent draw it replaced: every font/style/valign, long words, rules, both rotations, real reader font |
| `paginate_book_test.cpp` | final book: every display list reads, fits `kWgbMaxPageBytes`, parses to its end, valid image keys, no blank page, records advance, `find_page` round trip, chapter pages ordered; prints max list size |
| `book_index_test.cpp` | index parse/save round trip; thumbnail pack built only on add/remove, slots copied, re-added book re-read |
| `blank_para_test.cpp` | empty paragraphs |
| `stretch_lut_test.cpp` | cover scaler stretch tables |
| `blit_test.cpp` | `DrawBuffer::blit_1bit` / `blit_1bit_cols` + glyph ink (column-major in portrait) vs per-pixel reference, both rotations, all clipping |
| `labels_test.cpp` | `books` keep/drop heuristics (`tidy`, matter/junk, `key`, `slug`); labels raw, no chapter-number parsing |
