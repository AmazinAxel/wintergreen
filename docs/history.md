# Investigated, rejected and removed

Part of the Wintergreen notes; the entry point is `CLAUDE.md`.

## Still open

Moved here from `PLAN.md` (deleted 2026-11-12). Owner's call unless noted.

- **Neighbouring carousel cover preload (reopened 2026-11-12, not built).** Two covers
  (2 × 25,387 B) do not fit the 48,000 B spare, so it needs a fixed 25 KB buffer claimed
  at boot. Waits for the device heap numbers (`H min`) to show the room.
- **Re-baked books never re-sync.** Sync matches by folder name only; a re-bake under the
  same slug is never offered again. Bug. Any WGB format change (2026-11-12: compact word
  records) needs the card's books replaced by hand until this is fixed.
- **Sync re-download on size mismatch** (old item 1b). Touches the NAS side.
- **Latency floor not reached** for backward turn, quick-menu open, home ↔ library and
  chapter jump: 10–30 ms render + card reads above input + ~10 ms SPI + ~300 ms waveform.
  Each extra pre-draw needs another 48 KB buffer; likely not beside Wi-Fi.
- **Built 2026-11-12, none device-tested yet:** column-major glyphs, covers in panel
  order, glyph ids in display lists, allocation-free quick-menu wrap, thumbnail pack,
  allocation-free home slots, hidden-book title cache, unclipped fast path in
  `put_row_`, reader images with a portrait band copy, binary index file (format only:
  `BookIndex` API, sort, sync and hidden handling unchanged). Not baked: display-order
  records and pre-cut titles (sort and `fit_prefix` are sub-ms; would change the API).
- B1 light-sleep experiment, B2 `min_freq_mhz = 40`: both contradict CLAUDE.md "never
  break" rows.
- B3 deep-sleep pin holds: removed 2026-11-12 (drain rose; see docs/power-and-input.md).
- Ammeter only: B8 current with the clicker connected.
- R8: NimBLE host stack high-water mark never read (task not found by that name).
- Not flashed: final sync build (47 KB spare as the download buffer; expect ~300 KB/s).
- Never device-tested: sync started while the clicker is still connecting (code review
  2026-11-12 found and fixed a race: `begin_sync()` now aborts and waits for
  `connect_task` to exit); clicker paired elsewhere then back (fixed: stale bond deleted
  even when the clicker hangs up, then one fresh re-pair); image-page pre-draw after idle; `stage_next_page_` ordering on
  quick-menu resume; final-file rewrite in `WgbPaginator` (tmp + remove + rename) on the
  owner's machine.
- Seen once, never again: 20/20 clicker presses dropped (queue full) in one run; first
  big sync leaving a 0-byte `book.wgb.tmp` (`docs/sync.md` "Open").
- README line 12 still says the NAS converts books (`books` converts locally).

(Image-page pre-draw, formerly open here, is done: drawn on the first frame after the
waveform, see `docs/display.md`.)

### Investigated and rejected

- **Resume state in RTC memory (2026-11-12).** Would draw the first page on wake without
  opening `.wintergreen`, `book.pos` or the book, but keeping RTC memory powered adds
  deep-sleep current. Owner: nothing that costs battery.
- **Baking the quick-menu title wrap into the WGB (2026-11-12).** The wrap depends on the
  percentage's width at runtime; baking would mean reserving "100%" (a title could wrap
  earlier than today). Made allocation-free at runtime instead, same pixels.

- **Text stored once, shared by the 4 layouts (2026-11-12).** Measured on Dune: word text
  is 48% of the file (17.5 MB, ×4), so sharing it saves ~36%. Rejected for compact word
  records instead: sharing needs a second card read on every page not pre-staged
  (backward turn, chapter jump, open), against a one-time sync gain.

- **Shorter page-turn waveform (custom fast LUT)** — offered 2026-11-11 as the only lever
  left on turn/menu/hold latency (~150-200 ms vs ~300 ms, at a ghosting/contrast cost
  judged by eye). Owner declined; not built. Offered again 2026-11-12 for menu cursor moves
  only (fast clicker taps batch into one ~430 ms frame and look skipped; measured: 26 presses,
  0 dropped, queue peak 5, longest wait 428 ms). Declined again.

- **Fast wake paint** (2026-11-12) — wake measured at 131 ms of software, then ~1565 ms of
  Half refresh (`M boot` 1696). Replacing it with the page-turn refresh would save ~1.1 s
  per wake at the cost of sleep-cover ghosting. Owner kept Half.

- **Preloading the neighbouring carousel cover** (reopened by the owner 2026-11-12, see
  "Still open"; the history below is why the first two attempts failed). Built, flashed, and it **ran the device out of
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

  A heap-free version (the neighbour pre-read into the spare framebuffer) was built
  and **removed by choice**: it read a 25 KB cover the user might never move to, on
  every pause. With the device doing no scaling a cover is one `fread`, so the
  speculation bought little. Don't re-add read-ahead of any cover.
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

**`esp_hidh` (2026-11-11).** Its discovery and CCCD writes ran before encryption and
mapped nothing, its INPUT events never fired, and its connect failed on every fresh boot
(measured, 78 ms in). Replaced by raw NimBLE (`docs/bluetooth.md`); with it went the 6 KB
event task and the forced `ROLE_PERIPHERAL` / `GATT_SERVER` / `HID_SERVICE` keys.

**USB detection (2026-11-11, owner's call).** `usb_serial_jtag_is_connected()` gated three
things: skipping the wake hold, skipping the low-battery boot gate, and `keep_awake()`
while plugged in. All gone. Consequence accepted: flash within `kAutoSleepMinutes` of a
wake or start the upload and hold power; a flat cell refuses boot even on USB until the
charger lifts it (`CLAUDE.md` "Low-battery cutoff"). The reader's low-battery bar
latches until the next boot instead of clearing on USB.

**Clicker release-edge page turn (2026-11-11).** Replaced by press-edge turn + Undo on
hold (`docs/bluetooth.md`).

**Device-side layout and every radio RAM workaround (2026-11-11).** Pages became display
lists baked by `books`, so `TextLayout`, hyphenation, `WgbPageSource` / paragraph parsing
and the paragraph cache left the firmware, and with them: the OOM reserve +
`set_new_handler` rescue, `largest_free_block` guards and cache caps in the reader,
`Application::release_ram_for_radio` / `restore_ram_after_radio`,
`BookIndex::release_memory`, the clicker and sync releasing DrawBuffer's spare (which hid
covers and thumbnails), `IRuntime::clicker_holds_ram` / `largest_free_block_bytes`,
`layout_memory_test`, and the sync's controller deinit that left the clicker unavailable
until reboot. The reasoning they were built on is kept in `docs/bluetooth.md` (layout-era
sections).

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

**The /optimize pass (2026-10-25)**, dead or duplicated code only unless noted:
`Rotation::Deg180`/`Deg270` and every draw branch for them; the gray-plane plumbing
(`GrayPlane`, `GlyphData::gray_*`, `has_grayscale`) and `IFont::underline_*`;
`ListMenuScreen`'s plain header, book-details card, centred rows and footer, with
`title_`/`title2_`/`subtitle_`/`subtitle2_`/`plain_list_`/`detail_list_` and the 14 px
`ui_font_small.h`; `IScreen::name()`; the reader's `FixedFont` fallback and
"Failed to open book" path (no font = the book does not open); every empty-state string;
`verify_ota()` (rollback is disabled, so nothing is ever PENDING_VERIFY); serial `SIMG` and
`Z` (nothing reads `/sdcard/sleep/`); the BSSID/channel NVS hint; `stb_image.h` and the
buffer `decode_image()` / `floyd_steinberg_dither` it served. The glyph and blit code moved
out of `DrawBuffer.h` into `DrawBuffer.cpp`, where it had been cloned into four TUs, and
both rotations now blit a byte at a time.
Second pass (2026-10-26): serial `L` reads the index file only (it read `BookIndex` from the
serial task, a data race); `Application::font_manager()` and `kFontSizeNames`; the reserved
chapter-table words and the device-side TOC `fragment`; make_font.py's 5-level gray path
(`--mono` now a no-op). The home cover moved from a heap vector into the spare framebuffer.
