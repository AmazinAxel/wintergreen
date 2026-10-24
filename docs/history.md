# Investigated, rejected and removed

Part of the Wintergreen notes; the entry point is `CLAUDE.md`.

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
