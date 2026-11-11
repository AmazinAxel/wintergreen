# Plan: bake everything, device only reads and blits

Handoff for a fresh session. Read `CLAUDE.md` and the `docs/` file for each area first;
every "never break" rule there still holds. Delete this file once the plan is done or
moved into `docs/`.

## Goal

The X4 reads a file and copies pixels. Everything else happens on the PC in `books`:
which text goes on which page, line breaks, hyphenation, word and glyph positions,
chapter structure and labels, page counts, and refresh mode. Why:

- **Battery: no gain by itself** (corrected 2026-11-09, see "Whole-project audit").
  The CPU never sleeps between frames and `kPerfHoldMs` pins 160 MHz for a fixed
  300 ms after every press, so layout taking 30 ms or 3 ms draws the same charge.
  Baking only pays in battery once the reader can sleep between turns (audit, B1).
- **RAM and stability.** `TextLayout`'s 24 KB layout ceiling and the clicker OOM risk
  disappear with it.
- **Flash.** `TextLayout`, `PageHistory`, the page cache, the backward page search,
  the hyphenation trie (27 KB) and language detection all leave the firmware.
- **Latency.** Back turns, chapter jumps and size changes become table lookups. Forward
  turns are already a memcpy plus SPI write (`docs/display.md` "Page-turn latency").

Fixed at bake time: font Libron, **sizes 28 and 32 px**, rotations `Deg0` (landscape)
and `Deg90` (portrait). That gives 4 layouts per book. Changing any of these means
rebuilding the whole library, which is accepted. Images already work this way
(`kWgbImageBoxW/H`).

## State at handoff

Done in the previous session, uncommitted, verified as noted:

- `preprocess.fish` (packaged as `books` in `flake.nix`). Interactive review of
  `~/Downloads/books/*.epub`: title, author, cover (chafa 1-bit preview, replace with
  jpg/png), chapter keep/drop/rename. Writes `wg.ncx` with "N: Name" labels and
  rewrites the EPUB in place. Converts in a temp dir, then tars over ssh to
  `alec@alechomelab:/media/books/.incoming/` and `mv`s into `/media/books/<slug>/`.
  - Run end to end on Gutenberg 11, 84 and 1342 with a fake `ssh`; never against the
    real host.
  - Its chapter heuristics (`chapnum`, `matter_re`, `junk_re`, `subtitle`, `tidy`,
    `slug`) are the reference behaviour for the C++ port: tested cases are listed below.
- `tools/epub2wgb/main.cpp`: no flags; writes `./<epub stem>/`. `convert-books.sh` and
  `tools/tests/run.sh` adapted.
- `QuickmenuScreen.cpp`: on-device chapter-label rewriting removed (shows TOC labels
  verbatim). `pio run` succeeds.
- snowflake (`~/Projects/snowflake`, the homelab flake, not borealis):
  - `books.nix` and `convertBooks.fish` deleted.
  - `booksync.js`: books live at `/media/books/<dir>/`; finished books move to
    `/media/books/.read/<dir>/`; dot dirs are rejected.
  - `modules/desktop.nix` installs `books`.
  - `nix eval` checked with wintergreen overridden to the local checkout.
- Deploy still pending:
  1. Push wintergreen.
  2. `nix flake update wintergreen` in snowflake.
  3. On the Pi, move old books into place:
     `mv /media/books/.compiled/* /media/books/ && rmdir /media/books/.compiled && rm -f /media/books/*.epub`

The working tree also holds many other uncommitted changes from parallel work (WGB no
longer has a magic; see `WgbFormat.h`). Don't revert them.

User requirements behind all of this (from the session, not written elsewhere):

- Hyper-minimal, no fallbacks anywhere. Every book has reviewed chapters, labelled
  "N: Name" (or the verbatim matter label such as "Prologue", "Letter 1").
- `books` reads `~/Downloads/books`. The reviewed EPUB overwrites the original there.
  The WGB is never kept on the PC.
- Upload goes straight to the homelab. The homelab never receives an EPUB and never
  converts anything.
- Finished books: the device still deletes them and never re-downloads them. The
  homelab keeps the binaries in `/media/books/.read/<dir>/` instead of deleting them.

Facts found during the session that the C++ port must respect:

- The converter reads **only NCX** (`EpubParser.cpp` `parse_opf`/`parse_ncx`). EPUB3
  nav-only books used to get no TOC. That's why `books` always writes `wg.ncx`, until
  step 2 replaces the TOC with the chapter table.
- A root-level NCX resolves its `src` against the **OPF dir**, not the zip root (bug
  in `parse_opf`: `ncx_dir` starts as `root_dir_`). `wg.ncx` is therefore written
  beside the OPF, with srcs relative to it (`../` for files outside the OPF dir).
- NCX `src` is never URL-decoded by the converter. `wg.ncx` writes decoded zip names.
- `wg.ncx` is spine-referenced: the OPF's other NCX items are deleted, and
  `<spine toc="wgncx">` is set, because a `toc` attr that doesn't match the NCX id
  disables the NCX.
- A custom cover is added as manifest item `wgcover` with `properties="cover-image"`
  and `<meta name="cover" content="wgcover"/>`. The meta always wins in the converter.
- Dropping a chapter removes its spine itemrefs. A spine file is removed only when
  every entry starting in it (or the entry it continues) is dropped.
- Already-reviewed EPUBs are detected by a `wg.ncx` entry. With no arguments they're
  skipped; named files are reviewed again. Re-review must be idempotent: its own
  "N: Name" labels parse back to the same thing.
- Upload: `tar -C <out> -cf - <dirs> | ssh alec@alechomelab "mkdir -p
  /media/books/.incoming && tar -C … -xf - && chmod -R a+rX … && rm -rf '<dst>' && mv …"`.
  - The key is authorized via snowflake `hosts/common.nix`.
  - The remote login shell may be fish, so the command uses only syntax valid in both
    sh and fish (`&&`, single quotes).
  - On failure, the WGBs are lost (temp dir), so the tool prints the reviewed EPUBs to
    rerun by name.
- Known limitation: re-uploading a book under the same dir replaces it on the server,
  but a device that already has that dir won't re-download it (`have[]`).
- `docs/sync.md` mentions `booksync.test.js`, which doesn't exist in snowflake.

How things were verified (reuse these):

- Test EPUBs: Project Gutenberg 11 (Alice), 84 (Frankenstein, number-only chapters,
  Letters), 1342 (Pride and Prejudice: 164 images, caption-then-"CHAPTER N." labels,
  no Contents entry), from `https://www.gutenberg.org/ebooks/<n>.epub3.images`, kept in
  their own scratch dir.
  - Interactive input piped on stdin.
  - A fake `ssh` script first on `PATH` that discards stdin.
- Firmware: `nix print-dev-env > <scratch>/dev.sh; . <scratch>/dev.sh; pio run`.
- Snowflake: `nix eval --no-write-lock-file --override-input wintergreen
  git+file:///home/alec/Projects/wintergreen
  .#nixosConfigurations.<host>.config.<path>`. New files need `git add -N`.

## Step 1: `books` in C++ (replaces preprocess.fish)

- New host tool, for example `tools/books/`, sharing the core like `tools/epub2wgb`.
  Its own CMakeLists lists sources explicitly.
  - The flake's `books` package builds it instead of wrapping fish.
  - `epub2wgb` either becomes a library entry inside `books` or stays as a thin
    batch front end. Either way it takes no flags.
- No runtime deps: miniz (vendored) for unzip and zip, the existing `XmlReader`/
  `EpubParser` for OPF/NCX/nav/XHTML. ssh is still exec'd for upload, using the same
  tar | ssh `.incoming` → `mv` protocol and `slug()` rules (must pass `booksync.js`
  `safeName`).
- TUI: raw termios, line editor with a prefilled default (title, author), chapter list
  with toggle `N` / `N-M`, rename `r N name`, enter = done.
  - Cover preview as sixel (the terminal is foot) from the **actual** converter output
    (`cover_sleep.bin`), not an approximation.
  - "(front pages)" must be dropped or named before continuing: no unnamed chapter
    exists in the new model.
- Chapter source order: NCX → EPUB3 nav (`epub:type="toc"`) → h1-h3 per spine file.
  Then the heuristics, ported from `preprocess.fish`:
  - entries before a "Contents" entry, and entries containing the book title
    (letters/digits only), are front matter;
  - `matter_re` entries stay unnumbered; `junk_re` entries are dropped by default;
  - shallowest non-matter depth = chapter depth; deeper entries are dropped;
  - number parse: digits, canonical Roman (≤200 unless after "Chapter"), number words
    and ordinals, "Chapter the Third", `CHAPTERXXVII`, caption-then-"CHAPTER III."
    (caption discarded);
  - a bare Roman or word numeral counts only after "Chapter" or before punctuation or
    the end ("I Meet a Stranger" and "One Day" stay titles);
  - number-only chapters take a name only from the heading carrying that number or the
    sibling heading right after it, otherwise they stay "N";
  - ALL-CAPS names become Title Case.
- Tested label → result cases:
  `CHAPTER I. Down the Rabbit-Hole`→`1: Down the Rabbit-Hole`, `Ch. 3: Name`→`3: Name`,
  `Chapter Twenty-One`→`21`, `XII`→`12`, `Seven: Name`→`7: Name`, `07 - The End`→`7: The End`,
  `He rode a black horse. CHAPTER III.`→`3`, `Chapter the Third. Mice`→`3: Mice`,
  `Chapterhouse Dune`, `I Meet a Stranger`, `Mi. something` → unnumbered.
  Port these into a host test.
- Also in the TUI: drop all images or pick which to keep (illustrated books are mostly
  image bytes).
- Reviewed EPUB is still saved in place in `~/Downloads/books` (it's the source for
  rebuilds). WGB is never kept locally. Add a "rebuild all" mode that re-bakes every
  reviewed EPUB without prompting, for when font, sizes or geometry change.

## Step 2: new WGB, fully baked

Chapter = reviewed TOC entry. No spine chapters, no TOC blob, no `file_idx` /
`para_index` / `depth`, and no "Chapter %u" fallback on the device. All values are
little-endian; counts and deltas use small variable-length integers (varints).

Layout:

1. **Header.** Counts and offsets of every table below. No magic, no version, no
   layout key, no stale-file handling: per the user, an old-format or mismatched book
   will never exist on a device. Don't add checks for it.
2. **Text stream**, stored once for all 4 layouts.
   - Words as glyph ids (1 byte per char for English via a per-book glyph map),
     separated by word boundaries. The map resolves to final Libron glyph indices
     at bake time, so the reader never decodes UTF-8 or runs
     `find_glyph_index_in`'s range search.
   - **No hyphenation points in the text.** A layout only needs the breaks it
     actually uses, and those go in its line table (below). Marking every possible
     break costs bytes in every block and hurts deflate, for points no layout reads.
   - Split into independently deflated ~4 KB blocks, plus a block offset table.
   - Device unpacks with ROM `tinfl`. Confirmed present: `tinfl_decompress` is
     exported in `esp_rom/esp32c3/ld/esp32c3.rom.ld`, so inflate costs no flash.
   - It does cost RAM: a `tinfl_decompressor` is about 11 KB (three Huffman tables).
     Allocate it once per book open, or make it a static member; never per page.
     Inflate each block straight into its 4 KB output buffer with
     `TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF` so no 32 KB dictionary is needed.
   - Compression buys card bytes and sync time only. Read time is a wash (4 KB raw
     at 20 MHz is about as long as 1.6 KB plus inflate). Prototype step 2 of "Order"
     uncompressed, then add deflate.
3. **Per layout** (4 of them):
   - **Line table:** per line, one byte: the word count (which is the delta to the
     next line's first word) plus an ends-in-hyphen flag. A hyphenated line adds one
     byte for the split offset inside the word. About 1.1 bytes per line instead of
     3, so the four layouts take ~55 KB, not ~150 KB.
     - The gap width is not stored. It is `(box width - sum of advances) / gaps`,
       one division per line, about 30 per page, and the advances are summed for
       drawing anyway. This is the rule at the end of this section. The host and
       device must use the same integer arithmetic; the round-trip test checks it.
     - Style and size switches are control codes in the text stream, not line flags.
     - Everything that is not justified prose (last lines, indents, alignment,
       headings, rules, images, vertical centring and spreading) is not
       specified here yet, and the gap rule above has an exception. See F2 in
       the audit.
   - **Page table:** per page, the first line index, chapter index, character offset
     (for size switches and %), and a refresh-mode flag (the converter decides full vs
     partial: image pages, ghosting budget).
   - **Chapter jump table:** chapter → page.
4. **Kerning: not in the file.** Kerning is a property of the font, and the font's
   class table is already mapped from flash. Glyph x within a word = running sum of
   advance plus the pair's table entry: one lookup per glyph, ~1,500 per page, well
   under 0.1 ms. Storing it per book would repeat font data in every WGB.
5. **Chapter table:** final label strings, shown verbatim. Optionally pre-wrapped and
   pre-shortened for the quick menu header and rows.
6. **Images:** 1-bit, stored once at the largest box and centred in either orientation.
   Run-length coded. Ordered dither (compresses far better) vs error diffusion (looks
   better) is a per-book choice in the TUI.
7. **Metadata:** title, author, total pages per layout.

Don't bake values cheaper to compute than to store: page "N / M" and percentages are one
integer print from the page index.

Size target: plain 400-page novel ≈ 200 KB of compressed text + 55 KB of layouts +
40 KB of tables ≈ **0.3 MB** (estimate; the baker should print the real split per
book), smaller than today. A naive per-page glyph list would be
~1.4 MB per layout (5.6 MB per book), so don't do that. Full page bitmaps (48 KB per page)
were rejected for size.

Estimated device cost per page (estimates, **measure on hardware first**; prototype a
reader for one layout before committing the format):

| Work | Est. |
|---|---|
| glyph advance adds, ~1,500 glyphs | ~0.2 ms |
| varint/table decode | µs |
| ROM inflate, 1-2 blocks | ~1-2 ms |
| RLE image, full page | ~1-3 ms |
| SD read saved by smaller data | −~1 ms |

All of it runs during the ~300 ms panel refresh (prerender), so forward turns add
nothing; back turns and jumps add ~1-3 ms.

## Step 3: device side

- `ReaderScreen`:
  - page = (layout, page index); next/prev = ±1;
  - size switch = binary search on the character offset in the other layout's page table;
  - chapter jump = table lookup;
  - quick menu lists the chapter table verbatim, with the current chapter coming from the
    page table.
- Hard rules for the new reader (from the audit below):
  - **No heap allocation on the page-turn path.** Line, page and text-block buffers
    are fixed-size and allocated once at book open. Every clicker crash on record
    was a `std::vector` growing inside a turn; with no allocation there, the OOM
    rescue and its 12 KB reserve in `main.cpp` can be deleted.
  - **The card is read only while the panel is idle, never between a press and the
    SPI write.** The card shares SPI2 with the panel, so a read during a waveform
    waits the waveform out. Keep a read-ahead window in RAM (the blocks for the
    next two pages) and top it up when BUSY drops. Then the pre-draw during the
    waveform never needs the card.
  - **Image pages and chapter starts are pre-drawn at panel idle** instead of being
    skipped. Today `prerender_next_page_()` returns early for both, so every turn
    onto an image page or a new chapter waits for the previous waveform and then
    reads the card. The reader sits idle for tens of seconds after each waveform;
    do the read then.
  - Use `open`/`read`/`lseek`, not `fopen`: stdio mallocs a `FILE` buffer per open
    on top of FatFs's own sector cache.
- `book.pos`: one page index per layout (or one character offset), written only from
  `ReaderScreen::stop()`. The SD wear rules in `CLAUDE.md` still apply.
- Delete: `TextLayout` (on device), `PageHistory`, the page cache, `predecessor_start`,
  the paragraph window, the hyphenation trie and `detect_language`, and the "Chapter %u"
  fallback. Remove them from `platforms/esp32/CMakeLists.txt`.
  - `TextLayout` itself stays host-side: `books` runs it with the real font to produce
    the line and page tables.
- Font: rebuild Libron with `make_font.py --sizes 28,32` (currently 32,36; see
  `docs/content.md`). Keep `QuickmenuScreen::kNumFontSizePresets` / `kFontSizeNames` and
  the baker's size list in step.
- Header shows battery % live (the only live value) plus the baked chapter title.
- Book list and home: sync can carry each book's display row (shortened title as
  glyph runs, page count) so the device stops scanning the card and stops running
  `fit_prefix` / `ellipsize` per redraw. Optional, lower priority.
- Static UI chrome (quick menu settings rows, header frame) can become flash bitmaps;
  selection = invert a rectangle. Optional.

## Tests to keep or replace

- `paginate_book_test`, `layout_memory_test`: move to the host baker (every page of
  every layout, no stall, no blank page; page table monotonic; chapter jump pages
  valid).
- New: round trip, where the baked book read back by the device reader draws the same
  glyph positions as host `TextLayout` (extends `blit_test`'s reference approach).
- New: heuristics test with the label cases above.
- `json_scan_test`, `book_index_test`, `blit_test`, `stretch_lut_test` unchanged.

## Is this the floor?

For the file format and per-page CPU, yes: one add per glyph, a table lookup per turn
and a block inflate. For the whole device, no. The audit below lists what is left,
and its "Floor check" states for each axis where the floor is, what this plan
reaches, and which gap is left on purpose.

- **Panel refresh (~300 ms)** is the latency floor. Only waveform/LUT work moves
  it; see `docs/display.md`, which is hardware-tested territory.
- **Size:** an English-tuned dictionary or entropy coder beats deflate by maybe 10-15%.
  Not worth it over the ROM inflate.

## Whole-project audit (2026-11-09)

Scope: `sdkconfig.defaults`, the generated `sdkconfig.esp32c3`, the linker map of the
2026-11-08 build, `main.cpp`, `runtime.h`, `input.h`, `epd.h`, `DrawBuffer.h`,
`Application::update`, the reader's pre-draw, and `docs/`. Nothing was changed or
flashed. Each item says whether it is **measured** (map or config), **read** (from
the code) or **needs a device**. Items the docs already closed are not repeated.

A second pass the same day added `bluetooth_clicker.h`, `wifi_sync.h` (start,
teardown and buffers; not the JSON and index half of `run_sync_`),
`serial_communication.h` (start-up and the receiver loop; not the command handlers),
`DrawBuffer.cpp`, `BitmapFont.h`, the `update()` and draw entry of every screen,
`WgbReader`, the `TextLayout.h` output structures, the IDF linker fragment for
Wi-Fi, `phy_init.c`, and `docs/bluetooth.md`, `sync.md`, `content.md` and
`architecture.md`. Its findings are R8-R13, B6-B8, S1-S5, F1-F7, and corrections
to R1, R2, P1 and P4.

A third pass (2026-11-10) read `sdcard.h`, `font_manager.h`, `run_sync_`, the
snowflake `booksync.js`, `TextLayout.cpp`'s line, spacing and page-assembly code
(lines 153-440 by keyword, 717-1000 in full), `ContentModel.h`, the Kconfig text
of every `*_IN_IRAM` option that is set, NimBLE's default connection parameters,
and a keyword outline of every screen's draw path, `BookIndex.cpp` and the serial
handlers. It re-checked R10 and S3 against the code; both stand.

A fourth pass (2026-11-10) read what was left of the code that survives steps 1-3:
`ScreenManager.h`, `Input.h`, `IScreen.h`, `ui_wake.h`, `font_partition.h`,
`FontManager.h`, `CoverPaths.cpp`, `platformio.ini`, the firmware `CMakeLists.txt`,
the partition table, the rest of `bluetooth_clicker.h`, `wifi_sync.h` and
`Application.cpp`, the start, stop, update and draw bodies of every screen, the
serial command handlers, and the BT controller's low-power-clock Kconfig. It
corrected B7, added to R3, B1, B3, B8, S1, S5, P1 and Latency, and wrote the
Floor check.

What no amount of reading closes: every millisecond and milliamp figure in this
audit is an estimate until the measurements in "Suggested order" are taken. They
are labelled where they appear.

Not read, with the reason: `TextLayout.cpp`'s pagination collectors (lines
1000-1741; step 3 deletes them), the host converter, `tools/*.py` and
`preprocess.fish` (steps 1 and 2 replace them), `PageHistory.h` and the
hyphenation code (step 3 deletes them), the parsing bodies of `BookIndex.cpp` (P1
replaces them), the panel LUT tables (the ~300 ms figure and the claim that only
LUT work moves it rest on `docs/display.md`, not on this audit), and the NimBLE,
controller, Wi-Fi and PHY binary blobs.

Short answer: latency for a forward text turn with the built-in buttons is within
about 10 ms of the floor; with the clicker it is not (Latency). RAM and battery are
not. About 88 KB of RAM is recoverable against the ~13 KB free with the clicker up,
up to 19 KB more if R9 passes its device test, and the reader draws ~15 mA for the
whole time a page is on screen.

### RAM (this is what crashes the clicker)

Measured from the map: `.iram0.text` 75,256 B, `.dram0.data` 10,376, `.dram0.bss`
26,824. On the C3 code in IRAM comes out of the same SRAM as the heap.

- **R1. One of the three framebuffers is redundant: 48 KB.** Read.
  `EInkDisplay::partial_refresh()` writes BW RAM and returns; the controller keeps
  the pixels, and nothing reads the host's "active" copy afterwards. `active_()`
  has three readers: `save_snapshot()`, and the `prev` argument of two
  `full_refresh` calls, which `epd.h` ignores unless the mode is `RefreshMode::Full`.
  No caller passes `Full`.
  - Target: one draw buffer plus the spare. `refresh()` sends the draw buffer and
    does not flip. Opening the quick menu swaps the two pointers (the page becomes
    the snapshot, the menu draws in the other), so the 48 KB memcpy in
    `save_snapshot`, `restore_snapshot`, `commit_offscreen` and `full_refresh` goes
    too.
  - Checked (read): no screen relies on the inactive buffer holding the frame
    before last, and none draws incrementally. Every draw path starts from
    `buf.fill(true)` (`ListMenuScreen.cpp:95`, `HomeScreen.cpp:307`,
    `QuickmenuScreen.cpp:306`, `ReaderScreen.cpp:617`) or from a whole-buffer
    `memcpy` (`restore_snapshot`, `commit_offscreen`). The SPI write in
    `partial_refresh` is synchronous, so the buffer is free to draw into as soon
    as the call returns; only the waveform runs on. The fast refresh never sends
    RED RAM from the host. R1 is safe as written.
  - After this change the draw buffer holds the displayed frame, so a list cursor
    move could redraw two rows instead of the whole list.
  - One thing R1 gives up: `RefreshMode::Full` takes the previous frame from the
    host for RED RAM. No caller uses it. If one ever does, it must keep its own
    copy.
  - The spare has five users, not two: quick-menu snapshot, reader pre-draw, list
    thumbnails (up to 40 in `MainMenu`), the home cover, and scratch for the sleep
    cover. While a radio holds the spare there are no thumbnails and no home
    cover, and the sleep cover falls back to 793 one-row reads. R1 ends that for
    the clicker if the clicker then fits without taking the spare (first device
    measurement in "Suggested order").
  - This is the largest single lever and needs no hardware knowledge. It also lets
    the pre-draw survive with the clicker up; today the spare is given to the radio
    and every clicker turn pays layout and draw before the SPI write.
- **R2. Wi-Fi code held in RAM: 19,724 B.** Measured: `wifi0iram` 8,206,
  `wifirxiram` 3,910, `wifislprxiram` 7,608 (libpp 15,324, libnet80211 4,400). It is
  resident whether or not a sync ever runs. Set `CONFIG_ESP_WIFI_IRAM_OPT=n` and
  `CONFIG_ESP_WIFI_RX_IRAM_OPT=n`, delete `sdkconfig.esp32c3`, rebuild, and confirm
  in the map that all three section groups left IRAM. Cost: somewhat slower sync
  throughput. Needs one sync run on the device.
  - Those two options are the right ones and are enough. Read from
    `esp_wifi/linker.lf`: `wifi0iram` follows `ESP_WIFI_IRAM_OPT`, `wifirxiram`
    follows `ESP_WIFI_RX_IRAM_OPT`, and `wifislprxiram` is placed in IRAM when
    `ESP_WIFI_RX_IRAM_OPT || ESP_WIFI_SLP_IRAM_OPT`. `SLP_IRAM_OPT` is already
    off, so the 7,608 B are there only because of `RX_IRAM_OPT`. `wifislpiram`
    (1,706) and `wifiextrairam` (5,408) are already in flash.
- **R3. Main task stack is 16 KB for a reason that no longer exists.** Read:
  `sdkconfig.defaults` justifies it with "EPUB parsing (ZIP + XML + CSS)", which is
  host-only now. Measure with `uxTaskGetStackHighWaterMark` over a session
  (temporary code), then size it to the peak plus margin. Likely 6-8 KB back. Step 3
  removes `TextLayout` from the device and lowers the peak again.
  - Inputs to the new size, read from `Application.cpp`: `save_settings_` keeps
    `line[600]` on this stack and `load_settings_` keeps `line[512]` plus
    `sval[512]`, for a file whose longest line is one book path. `save_settings_`
    runs under `do_sleep_`, so it sits on top of the deepest UI frames. Take the
    high-water mark across a sleep, not only across page turns.
- **R4. The 12 KB OOM reserve** goes away with the no-allocation rule in step 3.
- **R5. `CONFIG_FATFS_LFN_HEAP=y` mallocs and frees 512 B on every `fopen`, `stat`
  and `rename`.** Config, read. `docs/bluetooth.md` says this heap fails by
  fragmenting. `CONFIG_FATFS_LFN_STACK=y` puts that buffer on the caller's stack.
  Check the sync task's 6 KB stack has the room.
- **R6. Wi-Fi static data, about 17 KB** (libnet80211 8,867, libpp 3,946, lwip 1,952,
  wpa_supplicant 1,587). Measured. Inherent to linking Wi-Fi; only undefining
  `WG_WIFI_SYNC` removes it. Recorded so the cost of sync is known, not as a task.
- **R7. Budget for the new format's `tinfl` state, about 11 KB** (step 2 above).
- **R8. The BLE stacks were sized by argument, not measured.** Read: three stacks of
  6,144 B. The NimBLE host task and the `esp_hidh` event task are resident once the
  clicker is used (12.3 KB); the `wg_bt_up` connect worker is transient. The
  comments justify the size by the NVS write at bonding. No high-water mark is
  recorded anywhere. Measure all three with `uxTaskGetStackHighWaterMark` after a
  connect that bonds, in the same temporary build as R3, and add `wg_sync` (also
  6,144). Estimate: 2-4 KB back in total; do not guess the split.
- **R9. The BT controller holds 19,006 B of IRAM, the largest single archive.**
  Measured (`libbtdm_app.a`; next are libpp 16,438, esp_hw_support 7,792).
  `CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y` moves the controller's code to flash; its
  Kconfig text says "over 20K". It is resident whether or not the clicker is ever
  used. Needs a device: the Kconfig warns of lower BT performance and of trouble
  while flash is being erased or written. The only flash writes here are NVS: the
  bond store right after the first connect, the PHY calibration blob, and the
  sync IP cache. Test: forget the bond, pair, then connect and page 20 times;
  the clicker must bond, reconnect and page with no missed presses.
- **R10. Once the clicker has been used, its RAM stays held until deep sleep.**
  Read: NimBLE is never torn down (closed, correct), so `holds_ram()` is true for
  the rest of the boot even after the clicker is switched off. The OOM reserve is
  not taken back and the paragraph cache stays capped. With a 1-minute auto-sleep
  this is short-lived. Recorded so nobody chases it; R1 and R2 are the fix.
- **R11. Serial with no host costs 768 B and nothing else.** Measured and read:
  three 256-byte static path buffers (`g_cmd_path`, `g_index_path_a`,
  `g_index_path_b`) are in BSS always. No task, no driver, no heap and no wakeups
  exist until a host attaches; the per-frame check is one register read. This
  confirms `CLAUDE.md`. Not stated there: once a host has attached, the task
  (8 KB stack), the driver (~6 KB) and its 50 ms wakeups stay until the next deep
  sleep, even after unplugging. B1 must treat "serial started" as "no light
  sleep", not "USB attached now". `handle_serial_cmd` declares about 5 KB of
  local buffers (`line[1024]`, `out[1060]`, `tchunk[2048]` and smaller ones)
  inside that 8 KB stack; if the stack is ever cut, measure it first.
- **R12. Opening a chapter in today's reader spikes the heap.** Read:
  `WgbChapterSource` reads a table of 8 B per paragraph into a temporary vector and
  keeps two 4-byte arrays per paragraph for the life of the chapter. A
  3,000-paragraph chapter is 24 KB resident with a 24 KB temporary beside it.
  Step 3 removes this; until then it is one more reason the clicker runs out.
- **R13. Smaller IRAM residents: 2 KB is free, the rest is not on offer.**
  Measured: esp_hw_support 7,792, spi_flash 4,520, phy 4,408, freertos 2,982,
  esp_ringbuf 1,998, esp_libc 1,894, esp_system 1,290, esp_timer 1,240. Read
  from each option's Kconfig:
  - `CONFIG_RINGBUF_PLACE_ISR_FUNCTIONS_INTO_FLASH=y` moves the 1,998 B. Its only
    stated conflict is a driver whose interrupt runs from IRAM. The one ring
    buffer user here is the USB-serial driver, whose interrupt is not registered
    as IRAM, and `UART_ISR_IN_IRAM` is off. Safe by reading; a config line. Test:
    one serial upload.
  - `ESP_TIMER_IN_IRAM`, `LIBC_MISC_IN_IRAM`, `ESP_SYSTEM_IN_IRAM`,
    `ESP_INTR_IN_IRAM`, `LOG_IN_IRAM`, `ESP_ROM_PRINT_IN_IRAM` and
    `SPI_FLASH_PLACE_FUNCTIONS_IN_IRAM` can only be changed when
    `SPI_FLASH_AUTO_SUSPEND` is on, which needs a flash chip that supports
    suspend. It is off here. Not available.
  - `RTC_CLK_FUNC_IN_IRAM`, `RTC_TIME_FUNC_IN_IRAM`,
    `ESP_PERIPH_CTRL_FUNC_IN_IRAM` and `ESP_REGI2C_CTRL_FUNC_IN_IRAM` are free to
    change and cover part of esp_hw_support's 7,792 B. With `CONFIG_PM_ENABLE=y`
    the frequency switch calls clock code from interrupt context, and I did not
    trace whether that can happen while an NVS write has the cache off. Leave
    them unless a later heap measurement says the bytes are needed.
  - `ESP_PHY_IRAM_OPT` only affects the C2, so the phy bytes stay.

Do R1 and R2 before step 3. They make the clicker safe with today's reader.

### Battery

`docs/power-and-input.md` is right that battery is the fraction of time awake. What
it does not say outright: while a page is being read the device is awake the whole
time, at ~15 mA. Nothing in this plan or the tree changes that. Everything else
(frame rate, sampler rate, perf hold, layout time) is under 1% of it.

- **B1. Manual light sleep between page turns. Needs a device.** The closed item is
  *automatic* light sleep through tickless idle, whose Kconfig `select` forces
  `PM_SLP_DISABLE_GPIO`. Calling `esp_light_sleep_start()` from the UI loop does not
  go through that option and is not in `docs/history.md`.
  - When: reader on top, panel idle, nothing pending to persist, no USB host, no
    radio up, no button down.
  - Wake: GPIO3 low, plus a timer every 20-25 ms to sample the ladders (call
    `sample()` directly; the `esp_timer` sampler does not run while asleep).
  - The ladders cannot wake the chip themselves. Read from `input.h`: only Button3
    and Down pull their pin to 0 V; the others sit at roughly 1.2 V, 1.8 V, 2.1 V
    and 2.8 V, which is not a digital low. The same fact rules out deep sleep
    between pages with the page left on the glass.
  - The open question is whether the SAR ADC reads correctly after a manual light
    sleep. If not, try re-creating the oneshot unit on wake and measure what that
    costs.
  - Estimate, not measured: ~15 mA falls to the order of 1 mA while reading, at up
    to 20 ms extra input latency. With the clicker connected it does not apply.
  - The forward-turn buttons can wake the chip by themselves. Read: the two
    buttons that reach 0 V are the ones the reader pages forward with (Button3
    always, Down when held landscape, `ReaderScreen.cpp:257-260`), and the ladders
    are GPIO1 and GPIO2. A low-level GPIO wake on both pins makes a forward press
    cost one light-sleep wake, about a millisecond (estimate), with no sampling
    delay. The timer then only serves back, menu and previous-page. Needs a
    device: whether the pad's digital input can stay enabled beside the ADC
    channel, and which of the mid-level buttons also read as low. If some do they
    wake the chip too, which is harmless, because a wake only starts the sampler.
  - The sampling timer is most of what is left. Each timer wake is about 1 ms
    awake (estimate), so a 20-25 ms period is 0.6-0.75 mA of the ~1 mA. At 50 ms
    it is half that and at 100 ms a quarter, and the cost is that much latency on
    back, menu and previous-page presses only, once the forward buttons wake by
    GPIO. Owner picks the period after measuring.
  - The waveform is awake time too. After step 3 the pre-draw takes 10-30 ms of
    the ~300 ms and the CPU then waits for BUSY at ~15 mA. Sleep through the rest
    with the BUSY pin as one more wake source. That is about 4 mA·s per turn
    against 30-60 mA·s for a page held 30-60 s at 1 mA: 7-13% of what B1 leaves,
    and a larger share as the dwell current falls (estimate). Needs a device: the
    panel's CS, DC and RST pads must hold their levels through the sleep.
  - This is the floor for this chip. The C3 has no ULP core and no analog wake
    source, so a ladder button that does not reach a digital low can only be
    seen by waking the CPU to sample.
  - Prerequisite: B4.
- **B2. `min_freq_mhz = 40`. Needs a device, and contradicts a "never break" rule.**
  The rule's reason is SD timing when APB drops. IDF's SPI master takes an
  `ESP_PM_APB_FREQ_MAX` lock around transactions, so APB should be back at 80 MHz
  whenever the card or panel is clocked. That is my reading of IDF, not a
  measurement. If the rule came from a failed hardware test, leave it closed. If it
  came from reasoning, one test with an ammeter and a card soak settles it; the PLL
  being off is worth several mA of the 15.
- **B3. Whole-device deep-sleep current has no measurement in the docs.** Read: no
  `gpio_hold_en` or `gpio_deep_sleep_hold_en` anywhere, so the SPI chip-selects
  float while asleep, and the card stays powered. Nothing unmounts the card or
  puts it in its idle state before sleep either (`sdcard.h` has no teardown;
  the sleep path is `buf.deep_sleep()` then `esp_deep_sleep_start()`). A microSD
  card in standby is typically 100-250 µA by its datasheet, which would dwarf
  the SoC. "~10 µA" is the SoC's figure.
  Measure at the battery. Above ~50 µA, hold SD and panel CS high through sleep and
  measure again. At an hour of reading a day the device sleeps 23 hours, so 300 µA
  there would be a third of the daily budget.
  - The same card standby current is paid for every minute of reading. Against
    15 mA it is noise; against B1's ~1 mA it is 10-25% (estimate from the
    datasheet range). Firmware cannot remove it: an SPI-mode card has no sleep
    command and nothing in the tree switches card power. Whether the board can
    is a schematic question.
- **B4. The UI loop polls at 40 Hz.** Read: `wait_next_frame()` already wakes on
  input through `g_ui_wake`; the 25 ms timeout only serves timers (auto-sleep, perf
  hold, persist quiet time, the 1 s battery poll, hold-repeat). Block until the
  nearest of those deadlines instead. Worth microamps today; needed by B1 to know
  how long to sleep.
  - Two more per-frame polls to convert, read from `bluetooth_clicker.h` and
    `main.cpp`: `wg_clicker::poll()` compares a timestamp for the clicker's
    tap/hold split and checks `g_disconnect_request`; the first becomes a
    deadline, the second a wake of `g_ui_wake` from the BLE callback. The OOM
    reserve check calls `heap_caps_get_largest_free_block()`, which walks the
    free list, on every frame while the reserve is spent and no clicker holds
    RAM, which is the whole length of a sync. Run it once when the sync ends.
  - Otherwise the screens are already idle when nothing changes (read): every
    `update()` only compares flags and ticks hold-repeat. Nothing measures or
    draws without a change.
- **B5. Wake paint as a fast partial. Needs a device and eyes.** Every wake runs a
  Half refresh. The sleep image on the glass is known (cover or wordmark), so wake
  could load it as the previous frame and run the fast waveform to the page. The
  sleep-entry Half refresh stays, which keeps one full-panel waveform per cycle.
  Risk: cover ghosting under text. `docs/display.md` "Panel health" applies.
- `kPerfHoldMs`: after step 3, hold 160 MHz only around the render call. Under 1%.
- **B6. Full RF calibration does not run on every wake.** Read. No radio code runs
  at boot at all: `main.cpp` only calls `wg_sync::bind()` and `wg_clicker::bind()`,
  and the PHY is enabled only when the clicker row or Sync is pressed. On a boot
  that is a deep-sleep wake, `esp_phy/src/phy_init.c` overrides the configured
  mode with `PHY_RF_CAL_NONE` and loads the stored blob from NVS. Every normal
  boot here is a deep-sleep wake. The full calibration that
  `CONFIG_ESP_PHY_RF_CAL_FULL=y` asks for runs on the first radio use after a
  power-on, a reset or a flash, or when the blob is missing. Estimate: 100-200 ms
  on those boots only. Nothing to do.
- **B7. The clicker connect path polls, but only while connecting.** Read:
  `bringup_stack()` waits for `ble_hs_synced()` in 10 ms steps for up to 10 s, and
  the scan is 5 s at 100% duty (`kScanMs`, `itvl = window = 96`), watched in
  10 ms steps for up to 7 s. Bounded and user-triggered, so not a battery item. It breaks the
  "event-driven, no polling" rule; the host sync callback and the scan-complete
  event can give a semaphore instead. Low priority.
- **B8. Current with the clicker connected has never been measured.** The link is
  held for the whole reading session, and B1 does not apply while it is up.
  Read: `esp_hidh` connects with `ble_gap_connect(..., NULL, ...)`, so NimBLE's
  defaults apply: interval 30-50 ms, latency 0, supervision timeout 2.56 s.
  Neither `bluetooth_clicker.h` nor `nimble_hidh.c` handles a parameter-update
  request, so if the clicker asks for other values the host's default answer
  decides. As the central, the device keys its radio every interval whether or
  not a button is pressed. Controller modem sleep is already on
  (`CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1`, main crystal as the sleep clock), so
  between events only the radio rests; the CPU does not. Measure at the battery
  with a page on screen and the clicker idle. If it is more than 1-2 mA above
  the baseline, ask for a 100 ms interval after connect with
  `ble_gap_update_params`; that adds up to 100 ms to a click, so measure the
  saving before taking the latency.
  - The link cannot be carried through light sleep on this board by
    configuration. Read from the controller's Kconfig:
    `BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP` depends on
    `FREERTOS_USE_TICKLESS_IDLE`, which is closed. The external 32 kHz option
    needs a crystal, and the config runs the slow clock from the internal RC
    (`CONFIG_RTC_CLK_SRC_INT_RC=y`), so presumably the board has none. The
    internal RC option's own help text says not to use it while connected (over
    500 ppm). So with the clicker connected the awake current is the floor, and
    only hardware changes it.

### Latency

- Forward text turn: at the floor (pre-drawn, memcpy, DMA SPI, waveform).
- Not at the floor today, all fixed by step 3 with the rules added there: turns
  onto image pages, the first page of each chapter, cold backward turns, and every
  turn with the clicker connected (no spare, so no pre-draw).
- Menus: draw overlaps the previous waveform already. Two stalls are left, both the
  same shape. A home carousel step waits out the waveform before its 25 KB cover
  read. A book-list step onto a row whose thumbnail is not yet in the 40-slot
  cache does the same: `get_item_thumb` loads inside the draw, and `load_thumb_`
  calls `wait_panel_idle()` before its `fopen`. Opening the list pays one file
  open per visible row. Read-ahead of covers was removed by choice
  (`docs/history.md`: "Don't re-add read-ahead of any cover"); not reopened. That
  entry records a judgement ("bought little"), not a failed hardware test. P1
  has a way to cut the list-open cost that is not read-ahead.
- Wake to page: boot, card mount, book open, Half refresh. Step 3 shortens book
  open; B5 is the only lever on the refresh.
- **Pre-drawing menus or the previous page is not worth doing.** A draw is tens of
  milliseconds and already overlaps the previous waveform; the waveform is ~300 ms
  and cannot be skipped. A pre-drawn frame saves only the part of the draw that
  outlasts the waveform, which for a menu is nothing. Each extra prediction costs
  one 48 KB buffer, which is the RAM R1 exists to give back. The forward page is
  the one prediction that pays, because the reader spends minutes on a page and
  the next press is almost always forward. Closed by reasoning; the timing in
  P4 would be the measurement if anyone doubts it.
- Every repaint sends the whole 48 KB frame over SPI (read, `epd.h`). A windowed
  RAM write for a cursor move would save about 10 ms of ~300 (estimate).
  `docs/display.md` records that it was tried and reverted (stale BW RAM outside
  the window). Closed.
- Input side, read from `input.h`: a 5 ms sampler and a 5 ms debounce, so a
  built-in button is acted on within about 10 ms. The debounce stays. A ladder
  press passes through the other buttons' voltages on its way down, so the first
  sample cannot be trusted (my reasoning, not a test).
- **The last 10 ms of a forward turn is the SPI write, and it can be moved off the
  press. Needs a device; optional.** While idle, write the pre-drawn page into
  the panel's BW RAM. The press then sends only the activation command. BW RAM
  would hold a frame that is not on the glass until the press; a back turn, a
  menu or sleep rewrites the whole plane, as they do now. That is the same kind
  of state the reverted region write got wrong, so try it last and by eye. It is
  3% of a turn, under what a reader notices, and nothing else in the plan
  depends on it.
- **A clicker turn waits for the key to come back up.** Read
  (`bluetooth_clicker.h`, `on_report` and `poll()`): a tap is queued on release,
  because a key held past `kHoldDelayMs` is a different action (left hold is
  Back, right hold is Confirm). Every clicker turn therefore starts 80-150 ms
  late (estimate: the length of a button press), which is more than every other
  latency item in this audit together. On top of that the report waits for the
  next connection event, 0-50 ms at the default interval (B8), and there is no
  pre-draw until S4. Acting on the press removes the 80-150 ms and gives up the
  two hold actions on the clicker. Owner's call. The built-in buttons act on
  press and are not affected.
- Sleep press to dark, read from `Application::do_sleep_`: wait for the panel,
  paint the cover (one Half refresh), stop the reader (`book.pos`, then the index
  load and save described under "Flash and card"), save settings, flush, panel
  deep sleep. The paint is needed. The index pass is the removable part.

### Sync and the clicker

Read from `wifi_sync.h`, `bluetooth_clicker.h`, `docs/sync.md` and
`docs/bluetooth.md`.

- **S1. Radio-on time for a sync is not a battery lever.** It is scan, WPA
  handshake, DHCP, one POST, then the files, on one kept-alive connection, with
  teardown bound to `WifiGuard`. Fixed overhead before the first byte is 2-5 s
  (estimate, not measured). A static lease or a pinned channel would cut a second
  or so; pinning is closed by `docs/sync.md` and a sync is rare and user-started.
  Buffers: `wg_sync` stack 6,144, one 4,096-byte `std::vector` allocated per
  downloaded file, a 512-byte reply buffer on the stack, TCP window 5,760. The
  per-file vector should be one allocation per sync, since this heap fails by
  fragmenting. Small. `http_post_` grows its reply `std::string` in 512-byte
  appends; reserve once.
  - A sync leaves two tasks behind until deep sleep. Read: `WifiGuard` undoes
    `esp_wifi_init()` and destroys the netif, but `esp_netif_init()` and
    `esp_event_loop_create_default()` (`wifi_sync.h:485-489`) have no
    counterpart, and IDF cannot stop the lwIP task anyway. The lwIP task
    (3,072 B stack) and the event task (2,048 B stack) stay, about 6-7 KB with
    their queues (estimate). Every put-down is a deep sleep, so it lasts one
    session. It matters in one place: the heap the clicker finds after a sync
    (S2 stage 1, S3) is lower than the heap before it. Deleting the default
    event loop in `WifiGuard` gives back the smaller half.
- **S2. Whether BLE can stay up during a sync is open. Needs a device.** The
  shared radio does not rule it out: `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y` is
  already set and the C3 time-slices one radio between both stacks. What forces
  BLE down today is heap. `esp_wifi_init()` wants ~50 KB of internal RAM against
  13-20 KB free, and deinitialising the controller is what frees the last ~12 KB.
  After R1 (+48 KB) and R2 (+19.7 KB) the sum should fit (estimate). The test,
  in two stages, both after R1 and R2 are in:
  1. Change the sync to call `radio_off()` without the controller deinit. Record
     `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` just before
     `esp_wifi_init()` and just before `esp_http_client_init()`. Sync three
     books. Then reconnect the clicker without a reboot. Pass: sync completes,
     the clicker reconnects, and the smaller of the two recorded blocks is
     above 16 KB.
  2. Leave the clicker connected through the sync. Older IDF releases refused
     `WIFI_PS_NONE` while BT was enabled and demanded modem sleep. I searched
     IDF 6.1's `esp_wifi` sources and the strings of `libnet80211.a`, `libpp.a`
     and `libcoexist.a` for that check and did not find it, so it is probably
     gone, but only the run proves it. Try `WIFI_PS_NONE` first; if
     `esp_wifi_set_ps()` or `esp_wifi_start()` returns an error or the link
     drops, switch to `WIFI_PS_MIN_MODEM`. Sync the same three books. Pass: the
     clicker still pages afterwards and sync time is within 1.5× of stage 1.
  If stage 1 fails on heap, stop: BLE stays down for sync and the docs are right.
  If stage 1 passes and stage 2 fails, keep stage 1; it removes S3.
- **S3. Any sync disables the clicker until the next boot, even if the clicker was
  never on.** Read: `wg_sync::start()` always calls `release_for_wifi()`, which
  sets `g_deep_teardown`, and `radio_off()` then deinitialises the controller and
  sets `g_ctrl_dead` without checking that the controller was ever initialised.
  `state()` reports `Unavailable` from then on. The deinit is needed when BLE was
  up; when it was not, there is nothing to free and the door closes for nothing.
  Fix: take the deinit path only when `g_ctrl_inited`. `docs/sync.md` says the
  clicker "is re-armed by pressing its quick-menu row again", which is no longer
  what the code does. Re-checked at `bluetooth_clicker.h:531-548`. Device check
  after the fix: boot, sync, then connect the clicker in the same session.
- **S5. Download writes do not wait for the panel, and `docs/display.md` says they
  do.** Read: `http_get_file_` calls `wait_panel_idle()` once, before `fopen`.
  The `fwrite` of each 4 KB chunk that follows does not, and a book takes
  seconds. The list stays navigable during a sync, so a cursor move starts a
  waveform while the sync task is writing the card. `docs/display.md` states
  that card traffic during a waveform corrupts the update and lists "every sync
  write" among the guarded paths. Either the rule is stricter than the hardware
  needs or this is a live violation that nobody has hit because nobody scrolls
  during a sync. Needs a device: sync three books and move the cursor
  throughout; look for a damaged frame. If it shows, the fix is not another
  `wait_panel_idle()` per chunk (the UI task can start a refresh between the
  wait and the write) but one lock taken by `refresh()` for the length of the
  waveform and by the sync task around each write.
  - The serial task is half-guarded (read, `serial_communication.h` and
    `main.cpp:356`). File transfers in either direction set
    `g_upload_in_progress`, and the main loop skips all UI work while it is set,
    in 10 ms `vTaskDelay` steps. The flag is set after the `fopen` and does not
    wait out a waveform already running. `R` (recursive delete), `L`, `A`, `K`
    and `N` touch the card from the serial task with no guard at all. Host
    attached only. If the S5 test shows damage, the same lock covers these.
- **S4. The clicker gives up the spare unconditionally.** Read: `bringup_stack()`
  calls `g_buf->release_spare()` with no heap test (the old `kMinHeapKb` floor was
  removed on purpose). After R1 and R2, measure free heap with the clicker up and
  the spare kept. If the largest block stays above the 12 KB reserve plus one
  page layout (24 KB ceiling in `layout_memory_test`), stop releasing it.

### Flash and card

- Image is 1,240,128 B in a 6.4 MB slot. Not at the floor and nothing depends on it:
  wake skips image validation, so size costs upload time only. Largest pieces
  (measured, `.flash.text`): mbedtls PSA crypto 116,826, NimBLE host 114,762,
  net80211 102,324, lwIP 65,316, wpa_supplicant 62,436, BT controller 58,896.
  `CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=y` could go if the router is WPA2; owner's call,
  no runtime gain.
- Step 3 removes `TextLayout` (18,988 B text) and the hyphenation trie (26,970 B).
- Unused partitions: `app1` (6.4 MB), `otadata`, `coredump` (64 KB,
  `ESP_COREDUMP_ENABLE_TO_NONE`). Harmless; changing the table needs a full erase.
- Card: after baking, a book is ~0.4 MB plus its covers, which stay raw 1-bit (home
  25,387 B, sleep 48,000 B) so the device does one read and no decode. Keep.
- `.wintergreen-index` is rewritten whole (~30 KB) once per session to change one
  book's order and percentage. Fixed-width binary records updated in place would be
  one sector. Low priority; fits with the optional display-row item in step 3.
  - It is also read whole for that one change. Read: a wake that resumes
    straight into a book skips the index (`index_deferred_`), and
    `record_book_progress()` then loads and parses all ~30 KB at reader close,
    which is normally the sleep path, to update one entry. That is about 60
    sector reads plus the parse, and about 60 sector writes when the percentage
    moved: 0.1-0.5 s awake per sleep (estimate) and a 30 KB heap peak at the
    worst moment. With the radio holding RAM the load is skipped and the
    progress is dropped. In-place records make this one sector read and one
    sector write with no heap.
- `book.pos` is written with a plain `fopen(path, "w")` (`ReaderScreen.cpp:801`),
  the pattern `CLAUDE.md` forbids for the settings and index files because the
  file is truncated before the write. A power cut inside that window loses the
  position and the book reopens at its start. It is four numbers and one
  sector, so the window is short; not a floor item, recorded because the rule's
  reasoning applies. P3's RTC copy does not help on a cold power-on.

### What the device still computes, and what baking can remove

Read from the code on 2026-11-09. Ordered by how much each is worth. None of these
is measured; time one before building it.

- **P1. The book index is text, parsed and sorted on the device.** `BookIndex::load`
  runs `fgets` and `strchr` per line into a `StringPool`; `MainMenu` merge-sorts an
  index permutation on every list build (`stable_sort_indices_`, two call sites);
  `HomeScreen::on_start` does not sort but makes repeated passes over the index
  to pick the most recent few; every row runs `word_width`, `fit_prefix` or
  `ellipsize` on every repaint.
  - Target: fixed-width binary records, stored in display order, each holding the
    title already cut to fit. Loading is one `read` into a fixed array: no parse, no
    pool, no sort, no measuring, no allocation.
  - The baker makes the cut title with the UI font's metrics. The list row's width
    depends on the percentage drawn at its right, so reserve the width of "100%".
    The home screen centres its title at a different width, so it needs its own.
  - The sync reply or the book's metadata carries the record, so the device never
    opens a WGB to learn its title. Read from `booksync.js`: the reply is two
    arrays of directory names and the server knows no titles. Keep it that way.
    Have `books` write the record as a small file beside `book.wgb`; the device
    downloads it like a cover and appends it to the index.
  - List thumbnails are one `fopen` per visible row today
    (`<book dir>/cover_list.bin`, 76×115, 1,150 B). Each open walks three
    directories with long names, about 10-15 single-sector reads (estimate), so
    opening the list costs 70-100 ms of card time (estimate) before the first
    paint. If the thumbnail is stored in, or in a file parallel to, the
    fixed-width record, the visible rows are one seek and one read of ~8 KB.
    This is the same data read on demand, not read-ahead, so it does not reopen
    the closed cover item. Measure list-open time first.
  - What the list holds in RAM today, read from `MainMenu.h`: one `BookEntry` per
    book with three `StringRef`s, the order, the percentage and three
    `std::string`s that are only used for hidden books. That is about 90-100 B a
    book (estimate from the struct), 72 B of it empty strings, on top of the
    loaded index. With records in display order the list needs no per-book
    array: it reads the visible rows.
  - `HomeScreen::on_start` copies path, title, author and cover path into
    `std::string`s for each of up to five slots, about twenty small allocations
    per home visit. `scan_hidden_` walks `.hidden` through a
    `std::queue<std::string>` and opens every hidden book's WGB to read its
    title. Both go away with the record file: the home slots point into the
    records and a hidden book has a record like any other.
  - Gain: list and home open time, and ~30 KB of index shrinks to the records.
- **P2. Covers are stored upright and rotated on the device.** Three per book: home
  424×486, list thumbnail 76×115, sleep 480×793. The sleep path reads the file into
  the spare and then calls `blit_1bit` under `Deg90`.
  - Target: store each in the panel's own byte order. The sleep cover becomes a
    complete 48,000-byte frame, read straight into the draw buffer with no scratch
    buffer, no blit and no size header. Home and thumbnail blits become row copies
    if the baker aligns them to a byte boundary on the panel's row axis.
- **P3. State across sleep is re-read from the card on every wake.** Nothing uses
  RTC memory, which survives deep sleep (about 8 KB on the C3). Keep the settings,
  book path, page index and the resume page's text block and line entries there.
  - On a deep-sleep wake the first page can then be drawn without opening
    `.wintergreen`, `book.pos` or the book. The card files stay the source of truth
    and are written at sleep exactly as now; a cold power-on reads them as today.
  - Measure `sd_init()` and book-open time first. If the mount is already hidden
    behind the panel's reset settle, the gain is only the file opens.
- **P4. Glyph blit per rotation: the two loops do not cost the same.** Read.
  `blit_deg0_` works a source byte at a time: one mask and at most two
  `apply_` calls per byte, so about 8 pixels per step. `blit_deg90_` has to
  transpose: for each output byte it gathers 8 source rows one bit at a time
  (`v |= ((*p >> sh) & 1u) << (7 - k)`, stepping `p` by the stride), and the
  head and tail of each column go a pixel at a time.
  - Estimate, not measured: `Deg90` is 2.5-3× `Deg0`, roughly 20-30 ms against
    ~10 ms for a 1,500-glyph page at 160 MHz. Time `render_page_` in both
    rotations before acting.
  - It matters only for turns that are not pre-drawn, and for every list and
    menu repaint (lists are always `Deg90`), where it already hides under the
    previous waveform.
  - If the measurement says it is worth it, store each glyph a second time in
    column-major order so `Deg90` becomes the same byte copy. The font grows from
    354,585 B to about 650-710 KB (estimate) and still fits the 1 MB partition.
  - Per-glyph work before the blit (read, `BitmapFont.h`): UTF-8 decode, a binary
    search over the WGF range table, a bounds check, and for kerning two
    class-map byte loads and one matrix load. Small beside the blit. Step 2's
    glyph ids remove the decode and the search; the kerning loads stay, as step 2
    item 4 intends. `draw_layout_line` also calls `fonts.get(size_pct)` once per
    word; the baked reader should switch font only at a size control code.
- **P5. Glyph clipping.** Baked positions are inside the page box by construction,
  so the reader can use a blit with no clip tests. Small.
- **P6. Quick menu rows and header** re-measure and re-cut labels on each repaint.
  Covered by "pre-wrapped and pre-shortened" in step 2 item 5; make it required
  rather than optional so `ellipsize` leaves the reader stack. Read: on every
  quick-menu open `QuickmenuScreen::wrap_` splits the book title and the chapter
  title into a `std::vector<std::string>` of words and builds candidate lines by
  string concatenation, two or three times. That is dozens of small allocations
  in the heap that fails by fragmenting, at the moment the clicker row is used.
  The baked header needs the book title wrapped too (two lines, in the header
  font and in its smaller fallback), not only the chapter labels.
- Left alone on purpose: the battery polynomial (one evaluation per header draw),
  page "N / M" and percentages (one integer print), settings parsing (three keys).

### Baked format: what reading today's reader adds to step 2

Read from `WgbReader.cpp`, `TextLayout.h`, the FatFs and sdmmc sources and the
config. None measured.

- **F1. Card reads per page today, and the read shape the new reader should use.**
  Today each paragraph is an `fseek` plus an `fread` through stdio (picolibc,
  128-byte buffer) and FatFs's 512-byte per-file cache. A page touching 4-6
  paragraphs is about that many single-sector commands, 2-6 ms in total
  (estimate). The new reader should read whole sectors at sector-aligned file
  offsets straight into a heap buffer, with `read()` or `f_read`, not `fread`.
  Two reasons, both read from source: FatFs passes a multi-sector request to the
  driver as one command only when the request covers whole sectors, and
  `sdmmc_read_sectors` falls back to one block at a time through a bounce buffer
  when the destination is not word-aligned DMA-capable memory. So read the
  sector span that covers the block and index into it. Padding each block to a
  512-byte file offset in the baker would do the same but costs ~256 B per block
  on average, about 16% of a ~1.6 KB compressed block, so do not pad.
- **F2. The one-byte line entry does not say where anything except justified prose
  goes.** Today's layout output carries, per item: x, y offset, height and
  baseline; whether a word continues the previous one with no gap; paragraph
  indent, alignment and margins; super/subscript; rules (`PageHrItem`); images
  with a crop offset when one is cut across pages (`y_crop`, `full_height`);
  inline images; and a vertical centring offset for the page. Step 2 names only
  style and size control codes. The format needs these, all decided by the baker:
  - a paragraph-start control code in the text stream carrying alignment, first
    line indent, left and right margin, and space above;
  - a "ragged" bit in the line byte. Today a line is left unjustified in three
    cases: the last line of a paragraph, a line ended by `<br>`, and a line
    whose gaps would exceed `line_width / 8`, which gets fixed gaps of two space
    widths instead (`justify_words`). The third case means the gap rule in step
    2 item 3 is wrong as written: it is not always
    `(box width - advances) / gaps`. One bit decided by the baker covers all
    three and keeps the cap out of the device. The byte then holds a 6-bit word
    count, the hyphen flag and the ragged bit; the baker asserts no line exceeds
    62 pieces;
  - the hyphen flag means "draw a hyphen glyph at the end of this line". Today's
    code adds none when the split falls after a hyphen already in the word; the
    baker clears the flag there and still stores the split offset;
  - a rule for the gap count: gaps are counted from boundary codes in the stream,
    not `words - 1`, because style changes inside a word split it into pieces
    with no gap between;
  - line height from the current size code, with the baker inserting an explicit
    advance where a heading or a mixed-size line differs;
  - one escape value of the line byte (count 63) meaning "non-text item
    follows": image id, x, y, crop offset and height, or a rule's x, y, width
    and height. Count 0 is not free: a whitespace-only paragraph yields a blank
    line that takes its height and draws nothing;
  - the first line's y in the page table, which carries vertical centring;
  - vertical spreading. Today `spread_text_items` runs on every page: when the
    last line falls short of the bottom by less than two line heights, the slack
    is shared out over the gaps between lines, weighted 1 inside a paragraph, 4
    between paragraphs and 8 next to an image or rule. So a line's y is not
    `first y + n × line height`. Store one slack byte per page in the page table
    and have the device run the same weighted split in integers (it knows the
    paragraph starts from the control codes), or store a y delta per line at
    about one more byte per line (+50 KB per book over four layouts). The slack
    byte is consistent with the "don't store what is cheap to compute" rule;
  - smaller cases the baker must resolve and the test must cover: inline left
    and right margins on a run inside a paragraph, a negative first-line indent
    combined with a run margin, a small image floated at the start of a
    paragraph's first line (drawn at the left padding, bottom on the baseline),
    an inline image promoted to a block, a rule's width as a percentage of the
    column (default one third, centred), forced page breaks, a paragraph's own
    `line_height_pct`, and super/subscript at ±20% of the line advance.
  The layouts differ in all of these, so everything that is geometry must live in
  the per-layout tables and only what is common to all four in the text stream.
  Margins and indents scale with font size, so store them in the stream as the
  CSS value and resolve per layout with the same integer rule on both sides, or
  move them to the line table. Decide in the prototype; the round-trip test
  must cover a book with headings, a blockquote, a rule and a split image.
- **F3. Size switch needs a search key that is resident.** A switch maps the
  current character offset to a page in the other layout. With the page table
  stored as one byte per page plus an absolute entry every 16 pages ("WGB size"),
  keep the absolute entries of the active layout in RAM, about 250 B for a
  400-page book, and read the target layout's entries once at the switch. No
  per-page array in RAM.
- **F4. Chapter labels on demand.** Today `open()` builds `std::string` labels for
  every TOC entry and keeps them. The baked reader needs one label for the header
  and a screenful for the quick menu. Read them from the chapter table when
  drawn; keep only the chapter → page table resident (2 B per chapter per
  layout).
- **F5. One file handle for the life of the book.** Each `fopen` allocates a
  512-byte FatFs buffer, the 512-byte LFN buffer (R5) and a stdio buffer, and
  walks the directory. Open `book.wgb` once at book open and keep it; covers are
  the only other opens and happen outside the page-turn path.
- **F6. Write the reader's RAM as a fixed budget in step 3.** `tinfl` state
  ~11 KB, one inflated block (4-16 KB by the "WGB size" choice), one compressed
  block being read ahead (about half that), the F3 anchors and the chapter →
  page table. For 4 KB blocks that is about 18 KB, allocated once at book open.
  This replaces today's paragraph window, offset tables (R12) and layout
  allocations, and it is what R4 relies on.
- **F7. The one-byte glyph id needs an escape.** Step 2 says "1 byte per char for
  English via a per-book glyph map". A book with more than about 250 distinct
  glyphs (accented names, Greek, a few CJK characters; today's layout has a CJK
  line-break path, `is_cjk_break_char`) must still bake. Reserve the top ids of
  the byte range for control codes and for a two-byte escape to the full glyph
  index, and have the baker put the most frequent glyphs in the one-byte map.
  A break between two CJK glyphs is a boundary with no gap, the same case as a
  style change inside a word in the gap-count rule above.

### WGB size

Applied to step 2 above: hyphenation points out of the text, one-byte line entries,
no kerning section. Together about 100 KB off a 400-page novel with no cost to the
reader. Still open, each settled by a host measurement in the baker:

- **Block size.** Independent 4 KB deflate blocks lose ratio against larger ones.
  With the read-ahead rule in step 3, blocks are inflated while the panel is idle,
  so 8 or 16 KB blocks add no latency. They add RAM equal to the block size. Print
  the book's size at 4, 8 and 16 KB and choose.
- **Images through the same inflate instead of RLE.** One codec on the device, and
  LZ77 does well on ordered dither. Compare both on the test books.
- **Page table as lines-per-page (one byte) with an absolute entry every 16 pages.**
  Saves a few KB.
- **Covers stay raw.** They are ~74 KB per book, a fifth of a baked novel, but
  dithered photos deflate poorly and the home cover is read on a carousel step,
  where inflate time shows. Not free, so not done.

### Stale comments found on the way

- `sdkconfig.defaults`: main stack "for EPUB parsing"; FASTSEEK "~29% faster
  conversion"; the logging block says logs are "still compiled in", but
  `CONFIG_LOG_MAXIMUM_LEVEL=0` compiles them out.
- `DrawBuffer.h` `release_spare()` comment: "Two static framebuffers are 96 KB of
  BSS". All three are `malloc`ed.
- `docs/content.md` "Measured size budget": `.dram0.bss` 122,760 is now 26,824.
- `docs/content.md`, same table: `.iram0.text` 69,872 is now 75,256.
- `docs/content.md` covers: the sleep cover is described as read "a row at a time"
  with "no 48 KB buffer" at 480×786; the code does one read into the spare and
  blits, at 480×793, and reads by rows only when the spare is released. The home
  cover is given as 424×479; `CoverPaths.h` says 424×486. It also names
  `draw_glyph_impl_`, which no longer exists.
- `docs/bluetooth.md`: still describes the `kMinHeapKb` 24 KB floor, which the
  code removed; says the spare is never released, but `bringup_stack()` releases
  it; gives the OOM reserve as 28 KB against `kOomReserveBytes` = 12 KB; and
  says the magic "stays WGB2" where WGB has no magic.
- `docs/sync.md`: `bufs_[2]` as "96,036 bytes of BSS" (all three buffers are
  heap); the clicker re-arm sentence (S3).
- `sdkconfig.defaults`: "Full RF calibration on every PHY enable" (B6).
- `docs/display.md`: "every sync write" waits for the panel (S5).
- `wifi_sync.h` `run_sync_`: "a missing one draws a placeholder". Nothing draws
  a placeholder.
- `HomeScreen.cpp` `on_start`: names the index `book_index.dat`; it is
  `.wintergreen-index`.
- No rule in `docs/` or `CLAUDE.md` contradicts an R, B, P, S or F item beyond
  what B1 and B2 already flag. `-Os` and the SD clock are closed by hardware
  tests and were not looked at.

### Floor check (2026-11-10)

For each axis: the floor on this hardware, what the plan reaches when every item
above is done, and the gap that is left with the reason. Figures are estimates
unless the item they point to says measured.

- **Forward turn, built-in buttons.** Floor: the time to be sure of the press plus
  the waveform. The plan reaches about 10 ms input, about 10 ms SPI and ~300 ms
  waveform on every forward turn (step 3 extends it to image pages and chapter
  starts, S4 to clicker sessions). Gap: the 10 ms SPI write, removable by the BW
  RAM preload under Latency. Optional, last.
- **Forward turn, clicker.** Floor: the same plus one connection interval. The
  plan leaves the release wait (80-150 ms) in place because removing it removes
  the hold actions. This is the largest gap left anywhere and it is a product
  decision, not an engineering one. The interval is a straight trade against
  B8's current and both ends are listed there.
- **Backward turn, menu step, quick-menu return.** 10-30 ms of render above the
  floor after step 3. Closing it means a second predicted frame at 48 KB.
  Accepted (Latency). The home carousel's cover stall also stays: the preload
  that would remove it was closed by a hardware out-of-memory in
  `docs/history.md`. That test predates R1 and R2, which change its premise; it
  is the owner's to reopen.
- **Wake to page.** Floor: ROM and bootloader, panel reset, one render, one
  waveform. Image validation on wake is already skipped (measured, config). P3
  keeps the card out of the first paint, step 3 makes the render 10-30 ms, B5
  makes the waveform a fast one. No further gap found by reading. Boot time
  itself has never been measured; added to measurement 1.
- **Battery, reading, no clicker.** Floor: light sleep between input samples and
  through the waveform, with the forward buttons waking by GPIO. B1 with its
  sub-items is that floor for a chip with no ULP core. What remains is the
  card's standby current (B3), which is hardware.
- **Battery, reading, clicker connected.** The awake current is the floor (B8).
  B2 and the B8 interval are the only levers. Hardware otherwise.
- **Battery, asleep.** Floor: SoC deep sleep plus what the card and panel draw.
  B3 measures it and holds the chip-selects. Per sleep cycle the removable work
  is the index pass (Flash and card); the cover paint is needed. After B1 the
  right `kAutoSleepMinutes` changes: a page left on the glass costs ~1 mA
  instead of ~15, so compare one minute of that with the measured energy of a
  sleep paint plus a wake. Owner's tunable.
- **RAM.** After R1 there are two 48 KB buffers, the one drawn into and the
  spare. The spare is the one knob left between RAM and latency: after step 3
  it buys 10-30 ms per forward turn and per quick-menu return. Keep it while
  the heap allows (S4). The floor is no host framebuffer at all: render in
  bands of 3-6 KB straight into the panel's BW RAM, which with the preload
  above also serves as the pre-draw buffer. Considered and not proposed. RAM
  stops binding after R1, R2 and R9, it needs the preload to pass its device
  test, and it needs a second render path, because in portrait a panel row
  crosses every text line and every menu row. If measurement 3 still shows the
  heap binding with the clicker up, this is the next step, not more trimming.
- **CPU per page.** Floor: one block inflate, one blit per glyph, one 48 KB SPI
  write. Step 3 with P4 and P5 is that. The only design that does less CPU is
  baked pixels, and it loses: a page is 48 KB raw and several KB deflated
  against about 1 KB of baked text, once per size and rotation, and reading
  and inflating it costs more than the blits it saves (estimate).
- **CPU, idle.** Floor: nothing runs without an event or a deadline. B4 is that.
  The deadlines, read from `Application::update` and the screens, are all
  `dt_ms` accumulators today: perf hold, the 1 s battery poll, inactivity,
  power hold, persist quiet time, hold-repeat and the home screen's back hold.
- **Flash size.** Nothing depends on it. `platforms/esp32/CMakeLists.txt` lists
  `wear_levelling` in `REQUIRES` and nothing calls it (read; not checked in the
  map). Harmless.

Left above the floor after everything, on purpose: the clicker release wait
(owner), 10 ms of SPI per forward turn unless the preload is tried, 10-30 ms on
backward turns and menu steps, the carousel cover stall, the card's standby
current, and the awake current while the clicker is connected.

### Suggested order

R1, R2, R5 (no device knowledge needed, R2 and R5 are config lines), then R3 and B3
(one measurement each), then B4 and the B1 experiment, then the baking steps below.
S3 is a one-line guard and can go with R1. The ring-buffer line in R13 can go
with R2.

Device measurements the plan depends on, in the order to take them:

1. One temporary instrumented build of today's code, removed afterwards: stack
   high-water marks for the main task (R3), the NimBLE host, `esp_hidh` event,
   `wg_bt_up` and `wg_sync` tasks after a bonding connect and a sync (R8); free
   and largest internal block in the reader with the clicker up; `render_page_`
   time in both rotations and card time per page (P4, F1); `sd_init()` and
   book-open time (P3); book-list open time, split into card and draw (P1);
   time from the sleep press to `esp_deep_sleep_start()` (index load and save);
   time from the wake press to the first panel SPI byte (Floor check).
   In the same session, with no build change: scroll the list during a
   three-book sync and watch for a damaged frame (S5).
2. Ammeter only, no build: deep-sleep current at the battery (B3); current with
   a page on screen, with and without the clicker connected (B8).
3. After R1 and R2: free and largest internal block with the clicker up and the
   spare kept (S4), one sync for throughput (R2), one serial upload (R13), and
   sync followed by a clicker connect in the same boot (S3), with the largest
   internal block recorded between the two (S1, what a sync leaves behind).
4. `CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y` pairing and paging soak (R9).
5. The sync test in S2, stage 1 then stage 2.
6. B4, then the B1 light-sleep experiment in three parts (does the ADC read after
   a manual light sleep; do GPIO1 and GPIO2 wake the chip on Button3 and Down;
   does the panel finish a waveform with the CPU asleep), then B2 with a card
   soak, then B5 by eye.
7. Step 2 prototype timings: uncompressed, then deflate at 4, 8 and 16 KB.
8. Last and optional: the BW RAM preload (Latency), by eye for a stale frame and
   by timing for the 10 ms.

## Order

1. C++ `books` + chapter table.
2. Prototype the baked reader for one layout on device; measure.
3. Full format (4 layouts, compressed text, images).
4. Device deletions.
5. Library rebuild + re-sync.

Keep `CLAUDE.md` and `docs/` in step as each part lands.
