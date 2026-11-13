# Content pipeline, formats and fonts

Part of the Wintergreen notes; the entry point is `CLAUDE.md`.

## Content pipeline

EPUB → WGB (a preprocessed binary layout format) → rendered pages. There is no on-device
conversion at all, so `books` (`tools/books`, packaged by `flake.nix`) is the only way a
book gets made. It reviews each EPUB (title, author, cover, chapters), bakes it and reopens
the output with `WgbReader` — the same code the device runs — so a file that would not load
is reported at bake time.

**Chapters are the reviewed list, not the spine.** `convert_epub_to_wgb_streaming` takes a
`PlannedChapter` list (`WgbConverter.h`): each entry starts at a spine file or an anchor id
inside one and owns everything up to the next entry. Paragraphs (and images) owned by a
dropped entry are never written, even when they share an XHTML file with a kept chapter;
spine files with nothing kept are left out. The WGB stores, per layout, the page each
kept entry starts on; there are no labels, and the quick menu shows kept entry i as the
number i + 1. Names exist only in the review: `books` prints them raw from the EPUB
(whitespace collapsed, nothing renumbered or renamed). The EPUB's own NCX is ignored by the
bake. The reviewed EPUB carries the list as `wg.ncx`, dropped entries as `class="wgdrop"`,
so a re-review starts from it and can restore them.

**Pages are fully baked** (`WgbPaginator.cpp`, called by the converter): every page of all
four layouts, laid out with `TextLayout` and the reader font compiled into `books`
(`tools/books/reader_font.cpp`, `.incbin` of `resources/Libron.bin`), stored as a display
list (format in `WgbFormat.h`; parsed by `WgbPageView.h`). `WgbWriter` writes an
intermediate file with the paragraph stream; the paginator lays it out (through
`WgbParagraphs.h`'s `WgbPageSource`, host-only) and rewrites the file **without** the
stream: header, image bitmaps, image refs, metadata, page tables, display lists. The
device has no layout engine, hyphenation or paragraph parser. A spine file still always
ends a page. See docs/display.md "Pages are fully baked".

Size: text is stored once per layout, so a book is ~4× its text plus ~2 B per word per
layout (x as a 1-byte delta, flags only when they change; was 4 B). Word text is glyph
indices resolved at bake time (`wgb_read_glyph`, ASCII 1 B, never longer than UTF-8): the
device does no UTF-8 decode and no range search per glyph. Project Hail Mary:
4.8 MB (was 6.0), Dune omnibus 28.8 MB (was 36.1). Card and sync cost only; RAM per page
is one ≤ 4 KB buffer, largest list 705 B. Sharing the text across layouts was rejected
(`docs/history.md`).

**Rebake rule: font bundle, `TextLayout`, `PageGeometry.h` or hyphenation change =
rebake the library, same commit.** Unchecked (no version); a stale book draws what it
was baked with (a font bundle change draws it with the wrong glyph indices).

Tried and reverted on the way: making each kept entry one WGB chapter (as the load
unit) gave an omnibus an 8,513-paragraph chapter whose paragraph table alone was ~136 KB.
Baked pages removed the load unit instead.

**Images are rasterised by the converter, not decoded on the device.** `WgbImageRef` is
`{data_offset, data_size, width, height}` (12 bytes) addressing a raw 1-bit bitmap inside the
WGB: `(width+7)/8` bytes per row, MSB first, bit clear = ink, followed by a portrait copy in
8-row bands (`wgb_image_bands_bytes`: band b = `width` bytes, one per column, rows 8b..8b+7,
top MSB). `data_size` is exactly rows + bands (`WgbWriter::set_image_data` builds both). The
reader reads rows in landscape and bands in portrait (`blit_1bit_bands`, a byte copy per
column); a page's slice of an image is one contiguous read either way.

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
(−61%), and `JpegDecoder`, `PngDecoder`, `ImageDecoder`, `ZipReader` and miniz all left the
firmware build (~24 KB of flash, plus their heap and scratch use). `stb_image` has since left
the repo entirely: its buffer-decode path was never called by the converter, which streams
through its own decoders. An image page
now turns at the speed of a text page.

`WgbReader::open()` reads the image table with **one `fread`** and parses from memory (one
`fread` per entry costs a FATFS + SPI round trip apiece). Nothing else is resident: page
records, chapter pages and paragraphs are read when asked for. The spine-filename and anchor tables are **gone from the format**: both
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
<out>/<book name>/cover_list.bin   # fitted to 76x115, book list rows, column-major
<out>/<book name>/cover_home.bin   # fitted to 424x486, home carousel, column-major
<out>/<book name>/cover_sleep.bin  # the panel's own 48,000 B frame, no header
```

Layouts in `CoverPaths.h`. All three are baked for the portrait screens: list/home are
`[u16 w][u16 h]` then w columns of ceil(h/8) bytes (`DrawBuffer::blit_1bit_cols`, a byte
copy per column); the sleep cover is the physical frame, read straight into the draw buffer
(`DrawBuffer::frame()`), no scratch buffer and no blit.

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

`show_book_cover_sleep_` in `Application.cpp` has **nothing left in it but one `fread`**.
`cover_sleep.bin` is composed by `Book::write_sleep_cover_bin` at exactly `kSleepCoverW` ×
`kSleepCoverH` (480x793) with the artwork already trimmed, scaled and dithered, then laid
out as the panel's frame (`kSleepFrameBytes`), so the device reads it into the draw buffer —
no scratch buffer, no blit, no scaling. A file of any other size is **ignored**; the
wordmark shows instead.

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

**No file format has a magic or a version** (removed 2026-11-10: WGF `WGF1`, bundle `WGFS`,
sleep image `MGR2`, index header `#wintergreen-index v2`). Fonts and the sleep image are
built and flashed with the firmware, so a mismatched one cannot exist; a layout change means
regenerating them in the same commit. The index is binary now (`BookIndex.cpp`); a file whose
sizes do not add up (an older text index) fails `load()` and the list rescans the card.

WGB has no magic either. `WgbReader::open` requires the image ref table to end exactly at
`meta_offset`, which rejects a stale or non-WGB file (e.g. an HTML page saved by a bad sync).
Any WGB layout change means re-converting the library.

| | Name | Holds | Defined in |
|---|---|---|---|
| Book | WGB | one book: paragraph stream, image bitmaps + table, metadata, baked pages + chapter pages per layout | `content/wgb/WgbFormat.h` |
| Font | WGF | one font at one pixel size, up to four styles | `content/WgfFormat.h` |
| Font set | WGFS | `[u8 count][pad:3][count × u32 size]` then the WGF blobs | `tools/make_font.py` `pack_bundle()` |

The magic removal shifted every absolute offset in `WgfHeader` down by four (49 → 45 bytes),
the same trap as the version-byte removal below; the checked-in assets were converted
mechanically (drop 4 bytes, subtract 4 from each nonzero offset field), not regenerated.

They were MRB/MBF4/FNTS. The rename came with three simplifications, all of which mean any
file produced before it must be re-converted:

- `WgbHeader` lost its `version` and `flags` fields (32 → 28 bytes) and later its
  `anchor_offset` (28 → 24). The magic went to `WGB2` with that second change, and was
  later dropped with the unused fields (24 → 20; chapter entry 16 → 10; text paragraph
  body header 18 → 14).
- `WgfHeader` lost its `version` byte: 50 → 49 bytes. **Every absolute offset stored in that
  header shifted down by one** — `bitmap_data_offset`, the three style offsets,
  `kerning_offset` and the two gray-plane offsets. Per-glyph `bitmap_offset` is relative to
  `bitmap_data_offset`, so it did not move. Getting this wrong produces a font that parses and
  renders garbage, which is why `tools/check_font.py` renders a sample string.
- The `WGFS` bundle lost the version byte and the 32-byte font name. The name was never read —
  there is one reader font, found by *partition*, not by name — so `--name` is a diagnostic
  label only.

`resources/Libron.bin` is a `WGFS` bundle behind a `[uint32 uncompressed
size][zlib stream]` wrapper. The three `display/ui_font_*.h` headers are bare `WGF1` blobs
emitted as C arrays (`kFontData_ui_*_wgf`).

### Sleep images (MGR2)

`resources/sleep.mgr`, flashed verbatim into the `sleep` partition and read straight out of
the mapping by `DrawBuffer::Mgr2Source_` — there is no file-backed variant, so fetching a row
is a pointer add.

```
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
`generation()` counter — MainMenu polls it so a sync refreshes the visible list
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

- **Reader: Libron** (v0.25, github.com/nicoverbruggen/libron), 2 sizes (32/36 px), all four styles, built
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
- The reader font asset is `[uint32 uncompressed size][zlib stream]` wrapping a WGFS
  bundle: `[num][pad:3][num × uint32][WGF...]`.

The two old gray-plane header fields are now `col_bitmap_offset` / `col_index_offset`: a
column-major copy of every glyph plus a u32-per-glyph index (`WgfFormat.h`), appended by
`tools/wgf_columns.py` (from the existing bitmaps, so pixels never change; idempotent;
`make_font.py` calls it). Portrait text uses it (`blit_cols_`), landscape the row-major
bitmaps. Every font is built with `--mono`: `FT_LOAD_TARGET_MONO`, no gray planes.

That followed from the reader drawing only the BW plane. The BW plane is not a 50%
threshold — `LEVEL_BITS` puts level 2 (37.5% coverage) in the ink plane, so BW-only text was
fattened by a ring of barely-covered edge pixels, and `FT_LOAD_TARGET_LIGHT` left stems
straddling pixel boundaries, so that ring was uneven from glyph to glyph. Mono hinting snaps
stems to the grid: same total advance widths, uniform stems, clean curves. It also cut the
asset from 296 KB to 122 KB compressed.

Don't "fix" the AA path to match — the 5-level encoding and its 37.5% threshold are correct
*for grayscale output*, where the lightening pass pulls those edge pixels back. They only
misbehave when the gray planes are thrown away.

`tools/check_font.py` parses a bundle (compressed or raw) exactly the way `BitmapFont.h` does and
ASCII-art renders a sample string. Run it after regenerating a font — a structurally broken
font renders blank on device with no error. Its header line reports `gray=yes|no`, the quick
check that `--mono` took effect.

Glyph coverage is per-font and sparse. A font that parses fine can still be missing the glyphs
for a given string, in which case it renders blank rather than falling back — check
`glyph_data(cp).bits != nullptr` before assuming a font can render text. (This bit the old
brand wordmark font: it contained only `n`, `o`, `u`, `s`.) Both fonts cover the same 527
codepoints, but Libron genuinely lacks 167 of them (most of Latin Ext-A and General
Punctuation). `make_font.py`'s `SUBSTITUTES` table maps the ones that would actually hurt onto
near-equivalents (U+2010 hyphen → ASCII `-`, and so on); the rest are blank. Extend that table
rather than accepting a blank glyph. There is **no wintergreen logotype font** — the wordmark
is drawn in the regular UI font.

Glyph lookup itself is a binary search over the WGF1 range table
(`BitmapFont::find_glyph_index_in`), so `char_width` is O(log ranges) and needs no cache.
UI strings still go through it; reader pages do not (glyph indices are baked into the
display lists).

Regenerating (needs freetype-py + fontTools, absent from the PlatformIO interpreter, hence
nix-shell):

```
nix-shell -p 'python3.withPackages(ps: [ps.freetype-py ps.fonttools])' --run '
  python3 tools/make_font.py bundle --name Libron --mono \
    --sizes 32,36 --line-height 150 \
    --regular Libron-Regular.ttf --bold Libron-Bold.ttf \
    --italic Libron-Italic.ttf --bold-italic Libron-BoldItalic.ttf \
    --out resources/Libron.bin'
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
    --name ui_large --size 24 --symbol ui_large \
    --out wintergreen/display/ui_font_large.h'
```

The TTC face index matters: face **0** is Iosevka Slab, and the rest are the Extended and
Oblique cuts.

There is no asset name to keep in sync: `generate_assets.py` reads
`resources/Libron.bin` by path, and `FontManager` finds that partition by name
(`"font"`, matching `default_16MB.csv`).

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
