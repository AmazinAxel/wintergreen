# Plan: bake everything, device only reads and blits

Handoff for a fresh session. Read `CLAUDE.md` and the `docs/` file for each area first;
every "never break" rule there still holds. Delete this file once the plan is done or
moved into `docs/`.

## Goal

The X4 reads a file and copies pixels. Everything else happens on the PC in `books`:
which text goes on which page, line breaks, hyphenation, word and glyph positions,
chapter structure and labels, page counts, and refresh mode. Why:

- **Battery.** CPU awake per page drops from tens of ms (layout) to ~2-5 ms.
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
     separated by word boundaries, with baked hyphenation points.
   - Split into independently deflated ~4 KB blocks, plus a block offset table.
   - Device unpacks with ROM `tinfl` (check `rom/miniz.h` in IDF for the C3; if it's
     missing, the vendored inflate costs ~10 KB of flash).
3. **Per layout** (4 of them):
   - **Line table:** per line, the starting word index (delta), one gap width (justified
     lines have even gaps), and flags (ends in hyphen, style or size switch). About
     3 bytes per line.
   - **Page table:** per page, the first line index, chapter index, character offset
     (for size switches and %), and a refresh-mode flag (the converter decides full vs
     partial: image pages, ghosting budget).
   - **Chapter jump table:** chapter → page.
4. **Kerning exceptions:** stored only where the pair adjustment isn't zero. Glyph x
   within a word = running sum of font advances (one add per glyph, no measuring).
5. **Chapter table:** final label strings, shown verbatim. Optionally pre-wrapped and
   pre-shortened for the quick menu header and rows.
6. **Images:** 1-bit, stored once at the largest box and centred in either orientation.
   Run-length coded. Ordered dither (compresses far better) vs error diffusion (looks
   better) is a per-book choice in the TUI.
7. **Metadata:** title, author, total pages per layout.

Don't bake values cheaper to compute than to store: page "N / M" and percentages are one
integer print from the page index.

Size target: plain 400-page novel ≈ 200 KB of compressed text + 150 KB of layouts +
40 KB of tables ≈ **0.4 MB**, smaller than today. A naive per-page glyph list would be
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

For the file format and the CPU, yes: the device does one add per glyph, a table
lookup per turn and a block inflate. Remaining levers, outside this plan:

- **Panel refresh (~300 ms)** is the real latency floor. Only waveform/LUT work moves
  it; see `docs/display.md`, which is hardware-tested territory.
- **SD wakeups:** read a whole compressed block run per card access instead of per page,
  so the card sleeps longer. Cheap; do it in step 3.
- **Size:** an English-tuned dictionary or entropy coder beats deflate by maybe 10-15%.
  Not worth it over the ROM inflate.

## Order

1. C++ `books` + chapter table.
2. Prototype the baked reader for one layout on device; measure.
3. Full format (4 layouts, compressed text, images).
4. Device deletions.
5. Library rebuild + re-sync.

Keep `CLAUDE.md` and `docs/` in step as each part lands.
