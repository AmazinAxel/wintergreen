# Display: refresh latency, paging and panel health

Part of the Wintergreen notes; the entry point is `CLAUDE.md`.

## One update per change, three levels, no framebuffer (2026-11-19)

Every screen change is **one** panel update, and when it lands every gray is already in
it; nothing moves afterwards. Normal updates (page turns, menus) show **white, gray and
black**; full refreshes (boot paint, sleep cover, wordmark) add **light**. That is the
hardware maximum without a second pass: each pixel picks one of four waveforms per update
by its two RAM bits (BW 0x24, RED 0x26), and leaving unchanged pixels alone needs one of
the four to be HOLD, which leaves three targets. Only an update that drives every pixel
anyway can spend all four on levels. The old multi-pass AA is why this matters: it showed
text black and then thinned it after the page had changed (docs/history.md); any design
needing a second update is rejected.

### Draw lists and strips

`DrawBuffer` keeps no pixels. Screens draw through it as before (`fill`, `fill_rect`,
`draw_text`, `draw_baked_line`, `blit`, `draw_diamond`, `draw_outline`) and each call is
**recorded** into a draw list (`cur_`); the list on the glass is `prev_`. `refresh()`:

1. **Matches** the two lists op by op (equal ops in the same relative order; greedy with a
   32-op look-ahead, `match_()`). A pixel no unpaired op covers is drawn by the same ops
   in the same order in both lists, so it cannot have changed: only unpaired ops' bounding
   boxes are **damage**, kept per strip as up to 4 byte intervals.
2. Walks the panel in **strips of 16 physical rows** (30 strips). Inside the damage only,
   it replays both lists into two scratch strips (two bit planes each, `hi` = gray or
   black, `lo` = black, the same layout everywhere: fonts, images, covers), compares them
   and gives each changed pixel its slot — RED = changed & new_hi, BW = changed &
   (~new_hi | new_lo): `00` HOLD, `01` to-white, `10` to-gray, `11` to-black.
3. Writes only the **window** each strip needs: the damage, unioned with whatever
   controller RAM may still hold a drive slot from the last update (`dirty_lo_/hi_`, per
   physical row; the controller drives the whole panel from RAM every update, so stale
   slots would re-fire). Unchanged strips with clean RAM are not sent at all, and an update
   with nothing to drive fires no waveform.

So a page turn renders and sends the text area (two planes, ~19 ms at 40 MHz for a full
panel; that doubling over 1-bit is inherent to gray), a list cursor move ~40 KB (measured
in `tools/tests/preview.cpp`), and an unchanged redraw nothing.

The strip being sent is DMA'd (`write_window`, queued transactions with the DC level in a
pre-callback and the SPI peripheral's CS) while the CPU renders the next one into the
other send buffer; `IDisplay::await_strip_slot()` keeps at most one window in flight
behind it.

RAM, one heap block claimed at construction (`kMemBytes`, ~26 KB): two 6 KB lists, the
new strip (3.2 KB), two old/send strips (3.2 KB each; the old planes become the packed send
buffer once diffed), a 4 KB fetch buffer. It replaced two 48 KB framebuffers and the
reader's 4 KB page buffer. Heap, not BSS: the BT controller allocates from the one ~50 KB
contiguous DRAM pool that .bss lands in (`esp_bt_controller_init -4`).

Ops and what they cost per strip:

| Op | Payload | Notes |
|---|---|---|
| Fill / Rect | none (bbox is the rect) | exact level |
| Text | glyph index + pen x per drawn glyph (4 B) | resolved at record time: no UTF-8 decode, range search or kerning per strip; the text bytes are not kept, so callers may reuse their buffers |
| Line | offset of the words in the tail + ink right edge per word | the reader's baked line; words left of the strip are skipped |
| Blit | source, item, gen, base offset, geometry, row keep | pixels from a `PixelSource` (card or flash) |
| Diamond | centre, radius, thickness | 4x4 supersampled, so its edges come out gray |

**Pixel sources are read only inside a refresh, a strip at a time, into the fetch
buffer** (`fetch_chunk_`; a strip whose sources would not fit is processed in fewer rows).
There is no cover or image cache: an unchanged cover is matched and never read; a changed
one is read once. Old pixels of a source come only from the fetch of an identical op in
the new list; an image that left the screen is **unknown** (never re-read) and its area is
driven to the new levels. The bus is drained (`finish_writes()`) before every fetch: the
card shares SPI2.

**The tail.** The reader reads a page's display list straight into the end of the list
being recorded (`claim_tail`, tagged by layout + page) and records line ops pointing into
it. After each waveform it **stages** the next page's list into the free list's tail
(`ReaderScreen::stage_next_page_`, first frame the panel is idle), so a forward turn reads
nothing on the press path. The quick menu's **snapshot** parks the page's ops and list at
the tail of the lists the menu records into (`save_snapshot`; moved along on each swap,
~1 µs memcpy) and `restore_snapshot()` makes it the current list again: closing the menu
reads nothing and draws nothing. If the menu needs the room the park is dropped and the
page is re-read.

An overflowing list never crashes: it is flagged, the next update drives everything to what
the list holds. `paginate_book_test` and the preview print the real sizes (largest page
1.6 KB with its list; quick menu with the parked page 4.3 KB).

`tools/tests/diff_test.cpp` runs random evolving screens through a simulated controller
(`sim_panel.h`) and checks after every update that the glass equals the list, that no pixel
was driven to the level it already showed (except black, re-driven on purpose), and that both hold if the controller copies BW
into RED after an update (unknown for a custom LUT).

## Refresh latency

Almost all of a page turn or menu keystroke is the e-ink waveform, not the CPU. Recording a
screen is tens of microseconds to a few ms; the strip render rides behind the SPI send;
the waveform is hundreds of ms.

`refresh()` fires `CMD_MASTER_ACTIVATION` and returns while the panel is still updating.
Safe only because **every** `EInkDisplay` entry point opens with
`wakeIfNeeded(); waitWhileBusy();` (`begin_update` does). Do not add a panel operation
without it.

- Anything that sends SPI right after firing must wait: `fire(Full, …)` waits, and so does
  anything with `turn_off` (the screen powers down at the *end* of the waveform).
  `deep_sleep()` therefore starts with `finish_writes(); waitWhileBusy();` — mid-update it
  leaves a half-drawn panel on screen for the entire sleep.
- **The SD card shares SPI2 with the panel** and traffic during a waveform corrupts it (why
  `main.cpp` defers index ops on `epd.is_busy()`). Any new SD access must call
  `DrawBuffer::wait_panel_idle()` first. Existing ones: `ReaderScreen::load_page_` and
  `draw_image_` (the image table), `stop()`'s `.pos` write, `do_sleep_()`, the settings
  save on reader pop, cover/thumbnail headers at record time, every sync write. Pixel
  sources read inside a refresh are already past `begin_update`'s wait.

**Region refresh cannot update "just the changed rows" by writing a window alone — tried
and reverted (1-bit era).** `setRamArea()` bounds where a RAM *write* lands, not the
refresh; the controller drives the **whole panel** from RAM. The old OTP partial looked
localized only because unchanged pixels had BW == RED. Writing a sub-rectangle left the
rest of RAM holding the last fully written frame, redrawn everywhere outside the window
(previously-selected rows stayed lit). The strip renderer writes windows safely because
every pixel outside them is known to hold HOLD (`00`): HOLD is the zero state and
`dirty_` makes every window cover whatever RAM may still hold non-zero.

### Pages are fully baked; the device never lays out

`books` lays out every page of every layout (2 font sizes × 2 orientations) on the build
machine and stores one 14-byte record plus one **display list** per page (`WgbFormat.h`,
`WgbPaginator.cpp`): line baselines, word x / font / style / valign / text, rules, image
slices, in draw order. The reader's position is a page index: forward/back are ±1, a
chapter jump is a page, progress is page / page_count. A page turn reads the record and
the list (≤ `kWgbMaxPageBytes` = 2 KB, measured max 642 B) into the draw list's tail and
records it; nothing allocates. `baked_line_test` proves the line ops render
pixel-identical to the PageContent draw they replaced.

**Display lists come off the card, so the read follows the SPI2 rule.** `load_page_` waits
for the panel before reading; `stage_next_page_` does the forward read on the first frame
after the waveform, so a forward turn finds its list already in place.

**Rebake rule.** Font bundle, `TextLayout`, `PageGeometry.h` or hyphenation change =
rebake the library in the same commit. No version field: a stale book still opens and
draws what it was baked with.

Two bugs the per-page source exposed (layout era, still in host `TextLayout`), both fixed:
- `leading_spacer` was granted to source paragraph 0. With a source per page that was
  every page's first paragraph, shifting its collect-index space by one against the
  page before it (a line repeated or skipped). Now only when the source
  `starts_unit()` (record top bit).
- A page boundary's `text_offset` indexed `lines[]` with the collect index, which also
  counts the spacer and promoted-image rows, so pages starting mid-paragraph often got
  0. `text_offset_at()` maps it; the offset orders pages across layouts (`find_page`,
  `book.pos`).

### History: backward paging (removed with baked pages)

Everything below describes the runtime backward walk that baked pages replaced; kept
for why the device no longer does it.

### Forward and backward must use the same line-fitting rule

**Both walks pass `require_full_height`.** Forward used to accept a line whose
*baseline* fitted and carry the descender into the next line's budget via
`pending_desc`; backward had no carry and gave its bottommost line a free descender
never repaid. Backward fitted more lines than forward and **ran past the page box on
30% of turns** (bottom 780 against 753 available on The Hobbit) — the same text landed
a line or two higher depending on which direction the reader arrived from. One rule for
both costs at most one line per page, one that was overhanging anyway.

**What remains is structural.** A page ending at a PageBreak or at end-of-chapter can
legitimately be reached from several starts, and backward picks the fullest rather than
the one the reader came from, skipping text at 1-2 places per book. `prev_page_()`
steps back one further page for an anchor and walks forward, keeping the last page that
ends at or before the current start: **every skip gone across four books**, for one
extra `layout_backward` and at most three boundary collections.

**Forward-then-back is now exact.** `PageHistory` (16 turns, ~260 bytes inside
`ReaderScreen`, no heap) records each forward turn; a backward turn whose current page is
the destination of the last recorded turn simply lays out the page it came from. The
history is keyed on `TextLayout::epoch()`, which bumps on any font/options/source change,
and is cleared on chapter load, so it can only ever retrace pages laid out the same way.
The anchor walk below is now only the *cold* path — after a jump, a bookmark resume, or
more than 16 turns back — and both it and `next_page_()` live in `TextLayout`
(`predecessor_start`, `next_page_start`) so `paginate_book_test` runs the device's code.

On the cold path a page ending at a PageBreak or before an image that did not fit still
has several valid starts. Measured with the test's 480x793 geometry: 11 one-line and 71
cross-paragraph start differences over 794 pages of an illustrated *Pride and Prejudice*
(165 images), 2 one-line offsets each on Alice, Frankenstein and the Odyssey. Every one
of those pages runs exactly up to the page it came from — nothing skipped or repeated.
Anchoring deeper would cut the illustrated-book count but drags more paragraphs through
`WgbChapterSource`'s window, the memory the next paragraph is about.

A handful of one-line offsets survive (25 / 5 / 2 / 2 over 520 / 579 / 170 / 640
pages). Closing those needs a chapter-wide forward chain, **built and removed**: it
worked, but walking the chapter drags the 32-slot paragraph window across it and peaked
around **31 KB**, reintroducing the heap abort with the clicker resident and putting a
per-chapter walk on the book-open path. Gating on free heap was not enough — the check
happens before the walk, not during it. Two other approaches that do *not* work:

- **A round-trip gate** ("does forward from backward's answer land back here?"). It
  does — backward's answer is a valid page ending in the right place, just not the one
  the reader came from, so the gate never fires.
- **Gating on how many paragraphs the answers differ by.** Distributions overlap
  completely: genuine skips span 4-12 paragraphs, correct turns 0-22.

### Page-turn latency

Everything CPU-side is off the critical path or rides behind the SPI send, leaving the
waveform as the floor:

- **`waitWhileBusy()` is event-driven** (2026-11-11). A BUSY falling-edge GPIO interrupt
  (`busy_isr_`, ISR service flags 0, so held off during flash writes rather than run from
  disabled cache) gives `busy_done_`; the waiter re-checks the pin, so a stale give costs
  one check. The old one-tick poll saw a waveform end up to 10 ms late (`FREERTOS_HZ=100`),
  paid by every frame chained behind a waveform. The 10 s dead-panel guard stays.
- **Two chained waveforms are the floor for anything that follows a page turn.** A
  waveform cannot be aborted, so a clicker hold (page turned on press, menu at 100 ms)
  shows its menu after the rest of the turn plus the menu's own waveform. The turn LUT's
  length is now ours (`panel_lut.h` D); shortening it was declined twice in the OTP era
  (docs/history.md) — tune for solid blacks first.
- **Forward turns read nothing on the press path** (staged list), and **closing the quick
  menu reads and draws nothing** (parked snapshot). The pre-draw (`prerender_next_page_`,
  `take_predrawn_`, `predraw_after_idle_`) and the snapshot framebuffer are gone: rendering
  is no longer the bottleneck, the SPI send is, and it starts at once.
- **Bulk SPI is DMA, queued, interrupt-driven.** A whole strip window (range, counters, BW,
  counters, RED: 16 transactions) is queued at once; the CPU renders the next strip
  meanwhile. Small polled transfers only with nothing queued (LUT upload in
  `begin_update`, `fire`, deep sleep).
- **No chapter load.** A page turn reads one 14-byte page record and that page's display
  list; a book open or font/rotation change is a binary search over records on the card.
- **Images are never decoded on the device.** An image page streams its rows (landscape)
  or columns (portrait) straight from the WGB, a strip at a time.
- **Drawing is byte copies.** Display lists hold glyph indices (no UTF-8 decode, no range
  search); portrait glyphs, covers and reader images have baked column-major copies (no bit
  transpose); unclipped rows skip the per-byte clip math (`put_row_` fast path). See
  docs/content.md.
- **The book index stays resident** for the session (~30 KB, binary). Both `MainMenu` and
  `HomeScreen` load from disk only when `entries()` is empty — once, on a cold boot.
- **Label truncation is linear, not quadratic.** `fit_prefix()` in `ListMenuScreen.h` is
  the one place that measures how much of a string fits a width. See "Text truncation"
  under Conventions.
- Measure on the device: temporary timing from `refresh()` start to MASTER_ACTIVATION
  (then remove it: no logging).

### Waveforms (custom LUTs, vendor voltages)

`platforms/esp32/panel_lut.h` builds both tables at runtime from named frame counts;
`EInkDisplay::load_lut_` uploads one (0x32 105 B, 0x03 gate, 0x04 source, 0x2C VCOM) only
when the needed table or temperature band differs from the loaded one, and after every
controller init (`loaded_ = None`). CTRL1 0x00 (RED used), CTRL2 0x0C (+0xC0 power-up,
+0x03 power-down), **never LUT_LOAD** (it would replace the table from OTP).

| Table | Slots | Shape |
|---|---|---|
| Turn | HOLD / to-white / to-gray / to-black | all at once: to-black VSH1 17 (frames 0–17), to-white VSL 24 (1–25), to-gray VSH1 17 then VSL 4 (0–21); FR 0x33, 25 frames |
| Full | white / light / gray / black | the X4 OEM factory 4-gray table verbatim (`kFactoryFull`, 50 frames, FR 0x22), phases lengthened in cold bands |

- **Voltages are the vendor's** (gate 0x17, source 0x41 0xA8 0x32: Good Display's 4-gray
  table, identical in Adafruit's hardware-verified ThinkInk 4.26" grayscale driver). They
  are the electrical safety envelope; only timings are tuned.
- **VCOM 0x30** (X4 OEM). 0x14, the X4 Pro firmware's pairing for the stock text
  waveform, was tried 2026-11-19: no visible difference to ghosting or dark covers.
- **Full = X4 OEM factory table** (2026-11-19). Our own full table (clear, then 4 / 2
  frames of black from white for gray / light) turned light covers near-black: the OEM
  grays drive toward black and pull back with 2–4 white frames, so short black pulses
  from white land far darker than the 0.5 / 0.75 covers are dithered for. Turn gray and
  full gray are no longer one recipe.
- **Comparison screen** (`platforms/esp32/lut_test_screen.h`, off; `#define WG_LUT_TEST`
  at the top of `main.cpp` boots into it): five candidate turn LUTs side by side, 11
  toggles each over text, black→gray (beside a 1 px checkerboard = 50%), text→gray and
  gray→white zones, then deep sleep. One flash compares five waveforms; use it instead
  of a guess per flash.
- **Slot order** = RED << 1 | BW (SSD1677 datasheet Table 6-5; FreeInk's "RED × 2 + BW").
- **Polarity in one place**: `kVsBlack = 01` (VSH1), `kVsWhite = 10` (VSL); the stock text
  waveform's black row is VSH1. An inverted panel = swap those two.
- **Turn black and white are the stock firmware's** (`kX4ProTextSsd1677`, firmware
  26110201, from Free-Ink/freeink-sdk PR #185, ~600 ms measured on an X4). Its rows:
  black, to-white, gray, white-rest (VSL 2 nudge); ours put HOLD where the nudge was.
- **Turn gray = to-black's drive, then 4 white frames back** (comparison screen,
  2026-11-19). The stock gray (VSL 6, VSH1 4) let old text show through new gray, and was
  darker than 50%. Driving to black first erases the old level; of 1–4 frames back, 4
  matched the checkerboard. Covers and images in normal updates use this gray.
- **No reset pass** (removed 2026-11-19): a 15-frame white reset with to-black waiting
  read as "one pass to undo, another to do", slow (35 frames at FR 0x22).
- **Black re-driven** inside the damage (`DrawBuffer::refresh` slot loop), as the stock
  waveform does: FreeInk measured held black fading within minutes (27.9 vs 13.1
  re-driven), so a glyph half held, half re-driven reads uneven weight.
- **Rejected on hardware (2026-11-19)**: gray erased 24 frames with every slot ending on
  frame 28, plus gray re-driven in the damage (slower to appear, gray AA flashed on every
  update, ghosting no better); the OEM full recipes per slot (50 frames) and a
  black-then-white flash for every driven pixel (58): both too slow to read with.
- **Gray partial updates are not a vendor feature**: Good Display and Waveshare only
  offer B/W partial; e-reader controllers clear gray ghosting with GC16 (every pixel in the
  region driven black and white before the target).
- **Menu covers are two-level** (owner's call, 2026-11-19): home carousel and book-list
  thumbnails blit with `two_level`, gray → 1 px black/white checkerboard, so scrolling has
  no gray dipping black before it settles. Reader images, text AA, frames and the sleep
  cover keep gray.
- **Open (2026-11-19)**: real-use check of the picked turn LUT (ghosting over a reading
  session) and of the OEM full table on sleep covers (were near-black).
- **Full right-aligned**: every level finishes on the last frame. The old vendor 4-gray
  one-pass LUT staggered its phases and read as "wordmark first, then the screen flashes".
- **Not DC-balanced on turns** (neither was the OTP fast waveform); the owner chose no
  visible motion over in-update counter-pulses. Sleep and boot full refreshes clear.
- **Temperature bands**: the C3's on-chip sensor (`temperature_sensor`, enabled only
  around the reading in `begin_update`): ≥ 20 °C ×1, 10–20 °C ×1.25, < 10 °C ×1.5 on
  every frame count; ±2 °C hysteresis. Custom LUTs lose the controller's OTP temperature
  compensation; this replaces it. Chip temperature reads a few degrees above ambient.
- `tools/tests/lut_test.cpp` checks layout, HOLD idle, the turn rows frame for frame,
  Full right alignment, shared gray, bands, clamping and the voltages.

### Waveform tuning (device day)

Turn timings are the stock waveform's; PL = 2 and C = 24 are ours, unverified. Constants in `panel_lut.h`; gray/light reflectance targets in
`ImageDecoder.h` (`kGrayLevel` 0.5, `kLightLevel` 0.75) and `tools/make_sleep_image.py`.

1. Panel shows anything? Inverted → swap `kVsBlack` / `kVsWhite`. Nothing at all → suspect
   the custom-LUT path first (CTRL2 value, upload after `waitWhileBusy()`).
2. Blacks solid, whites clean on a page turn? Adjust `kToBlack` / `kToWhite`.
3. Gray pleasant, and identical between the boot paint and after a turn? Adjust
   `kGrayBlack`, and `kLight` (PL) for the sleep cover's light level. Then set `kGrayLevel` /
   `kLightLevel` to what the levels actually reflect, regenerate the wordmark and rebake
   (covers and images are dithered against them).
4. Turn duration: temporary timing around `refresh()` and BUSY, then remove.
5. Ghosting after ~50 turns; gone after a sleep/wake? (the canary in "Panel health").
6. Cold test (fridge) for the temperature bands.

### Panel health

A run of turn waveforms is **not DC-balanced**: each leaves residual charge in the
microcapsules, showing as ghosting and, over a long run, permanent contrast loss.

**Wintergreen has no periodic flush, arrived at the hard way.** Three versions shipped:
inline (every 24th refresh promoted to full — the flash landed on whichever page turn
happened to be 24th); deferred to a 2500 ms idle (2.5 s after the last press you are
*reading the page*, and a flash there interrupts rather than delays); removed.

Removing it is safe **because deep-sleep wake is a full boot**, and every session is
bracketed by two full-panel waveforms:

| When | What runs |
|---|---|
| going to sleep | the sleep frame — `show_frame()`, Full LUT, cover or wordmark |
| waking | `Application::start()`'s `full_refresh()` (Full LUT) |

With `kAutoSleepMinutes = 1` that is a full-panel waveform every time the device is put
down and picked up.

**What is genuinely uncovered:** a session that never idles for a whole minute. Page
turns every 45 s keep `inactivity_ms_` below the timeout indefinitely, so 30 minutes is
~40 turns with no full refresh. Expect faint ghosting late in a long session; expect it
gone after the next sleep.

The canary is ghosting that *survives* a sleep/wake cycle. If it appears, the fix is a
counter in `DrawBuffer::refresh()` calling `full_refresh()` — accepting that no threshold
both protects the panel mid-session and stays out of the way.

### The OTP waveforms are gone (and why the 0x5A lesson no longer bites)

Until 2026-11-19 every update used the controller's OTP waveforms: fast partial (CTRL2
0x1C), the boot paint "Half" (0xD4 + a `CMD_WRITE_TEMP 0x5A` seed) and full (0x34 +
TEMP_LOAD). Removing the 0x5A write once produced a panel that ran one black flash and then
showed nothing: 0xD4 includes **LUT_LOAD**, and the OTP waveform it loads is selected via the
temperature register. **Nothing sets LUT_LOAD now** — both tables are ours, uploaded after
every init — so the OTP selection and the temperature register no longer matter.
`CMD_TEMP_SENSOR_CONTROL 0x80` stays in the init (harmless; a vendor init is not a place to
remove things that "should" be no-ops). The lesson that generalises: "the code reached this
point" is not "the panel showed something" — if a flashed build shows nothing, the
custom-LUT path is the first suspect.

**Still not possible: reading the temperature back** from the controller (`CMD_READ_TEMP`
needs MISO; the panel is wired write-only). The C3's own sensor stands in.

### Panel rails — the idle power-down is gone, don't re-add it

`EInkDisplay::power_down_rails()`, `Application::kPanelIdleOffMs` (5 s),
`panel_rails_off_` and `DrawBuffer`'s wrapper are all **deleted**. It switched the analog
charge pumps off after 5 s of no input, reasoning that e-ink is bistable and the next
refresh would see `isScreenOn == false` and OR in `CLOCK_ON | ANALOG_ON` itself.

**On hardware it silently swallowed one page turn.** With the rails down the next
`partial_refresh()` ran its waveform before the rails were up: the old page stayed on
the glass and only the turn *after* that drew. User-visible as "wait five seconds, press
forward, nothing happens".

Two reasons not to retry: those two CTRL2 bits are marked "best guess" in `epd.h` and
are only *documented* in combination with a refresh; and the fix (a settling delay, or a
separate power-up activation) would land on **the first page turn after a pause**, the
common case, paying the whole saving back as latency.

The rails are still powered down where it costs nothing: `fire()`'s
`turn_off` at the end of a full refresh, which is what the sleep frame uses.
