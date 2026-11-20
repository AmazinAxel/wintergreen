# Display: refresh latency, paging and panel health

Part of the Wintergreen notes; the entry point is `CLAUDE.md`.

## Refresh latency

Almost all of a page turn or menu keystroke is the e-ink waveform, not the CPU. Layout
and draw are tens of ms; the waveform is hundreds.

`partial_refresh()` passes `false`, firing `CMD_MASTER_ACTIVATION` and returning while
the panel is still updating, so the app lays out and draws the *next* frame during the
waveform. Safe only because **every** `EInkDisplay` entry point opens with
`wakeIfNeeded(); waitWhileBusy();`. Do not add a panel operation without it.

- Anything that sends SPI right after firing must keep `wait = true`, or the command
  lands mid-waveform and corrupts the update: `full_refresh`, the one-pass grayscale
  path, and anything with `turnOffScreen` (the screen powers down at the *end* of the
  waveform). `deep_sleep()` therefore starts with its own `waitWhileBusy()` — mid-update
  it leaves a half-drawn panel on screen for the entire sleep.
- **The SD card shares SPI2 with the panel** and concurrent traffic corrupts an update
  in flight (why `main.cpp` defers index ops on `epd.is_busy()`). Any new SD access must
  call `DrawBuffer::wait_panel_idle()` first. Existing ones: `draw_image_`,
  `ReaderScreen::stop()`'s `.pos` write, `do_sleep_()`, the settings save on reader pop,
  every sync write, `ReaderScreen::load_page_` (display lists; see "Pages are fully baked").

**Region refresh cannot update "just the changed rows" — tried and reverted.**
`setRamArea()` bounds where a RAM *write* lands, not the refresh. `refreshDisplay()`
drives the **whole panel** from BW RAM; a partial update merely *looks* localized
because unchanged pixels have BW RAM == RED RAM and get a no-op waveform. Writing only
a sub-rectangle leaves the rest of BW RAM holding the last *fully written* frame, and
the panel redraws that stale frame everywhere outside the window. Symptom when
`ListMenuScreen` tried it for cursor movement: previously-selected rows stayed lit and
the reader page showed through behind the menu. `partial_refresh_region()` remains
correct for its **one** caller, `show_loading()`, where the rest of the screen is
static *and* BW RAM already matches it.

### Pages are fully baked; the device never lays out

`books` lays out every page of every layout (2 font sizes × 2 orientations) on the build
machine and stores one 14-byte record plus one **display list** per page (`WgbFormat.h`,
`WgbPaginator.cpp`): line baselines, word x / font / style / valign / text, rules, image
slices, in draw order. The reader's position is a page index: forward/back are ±1, a
chapter jump is a page, progress is page / page_count. A page turn reads the record and
the list (≤ `kWgbMaxPageBytes`, measured max 879 B) into `ReaderScreen::page_buf_` and
draws it with `DrawBuffer::draw_baked_line`; nothing allocates. `baked_line_test` proves
the draw pixel-identical to the PageContent draw it replaced.

**Display lists come off the card, so the read follows the SPI2 rule.** `load_page_`
waits for the panel before reading. To keep the pre-draw overlap, `stage_next_page_`
reads page + 1's list **before** `refresh()` (panel idle, a few ms), and
`prerender_next_page_` draws it during the waveform from RAM. A page with an image needs
the card to draw, so it is pre-drawn on the first frame after the waveform ends
(`predraw_after_idle_`; the loop runs frames while the panel is busy), still well before
the next turn.

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

Everything CPU-side is off the critical path, leaving the ~300 ms waveform as the
floor:

- **`waitWhileBusy()` is event-driven** (2026-11-11). A BUSY falling-edge GPIO interrupt
  (`busy_isr_`, ISR service flags 0, so held off during flash writes rather than run from
  disabled cache) gives `busy_done_`; the waiter re-checks the pin, so a stale give costs
  one check. The old one-tick poll saw a waveform end up to 10 ms late (`FREERTOS_HZ=100`),
  paid by every frame chained behind a waveform: a menu after a page turn, a turn during
  a turn. The 10 s dead-panel guard stays.
- **Two chained waveforms are the floor for anything that follows a page turn.** A
  waveform cannot be aborted, so a clicker hold (page turned on press, menu at 100 ms)
  shows its menu after the rest of the turn's ~300 ms plus the menu's own ~300 ms: the
  ~0.5-0.6 s the owner noticed. Below that needs a shorter waveform LUT (more ghosting,
  weaker black, by eye); offered 2026-11-11 and **declined by the owner**.

- **The next page is laid out *and drawn* during the waveform**, so a forward turn is a
  memcpy plus an SPI write. Returning from the quick menu is a memcpy too. Both use the
  spare buffer.
- **Bulk SPI is interrupt-driven, not polled.** `EInkDisplay::sendData` uses
  `spi_device_transmit` for chunks >= 1 KB from DMA-capable memory,
  `spi_device_polling_transmit` otherwise. A framebuffer is 48,000 bytes (~10 ms per
  plane at 40 MHz, and `full_refresh` writes two) and polling spun the CPU for all of
  it. Small writes stay polled deliberately: a 105-byte LUT costs less than the
  interrupt round trip, **and LUT tables live in rodata, which is not DMA-capable** —
  hence the `esp_ptr_dma_capable()` guard rather than a size test alone.
- **No chapter load.** A page turn reads one 14-byte page record and that page's display
  list; a book open or font/rotation change is a binary search over records on the card.
  No paragraph or descriptor table is ever resident.
- **Images are never decoded on the device.** An image page turns at the speed of text.
- **Nothing walks the chapter.** Opening a book lays out nothing; pages are baked.
- **Drawing is byte copies (2026-11-12).** Display lists hold glyph indices (no UTF-8
  decode, no range search); portrait glyphs, covers and reader images have baked
  column-major / band copies (`blit_cols_`, no bit transpose); unclipped rows skip the
  per-byte clip math (`put_row_` fast path). See docs/content.md.
- **The book index stays resident** for the session. Both `MainMenu` and `HomeScreen`
  load from disk only when `entries()` is empty — once, on a cold boot. Every mutation
  updates the in-memory copy and saves, so a non-empty index is authoritative. ~30 KB
  against ~165 KB free heap (binary file since 2026-11-12, no text parse), and dropping it
  meant re-reading and re-parsing on entry to
  the home screen, on entry to the list, and again when the reader wrote progress.
- **Label truncation is linear, not quadratic.** `fit_prefix()` in `ListMenuScreen.h` is
  the one place that measures how much of a string fits a width; it accumulates
  `char_width` per codepoint. Every list screen used to inline a loop calling
  `word_width(text, fit + cb)`, re-measuring the whole prefix each step — O(n²) per
  label, on every row of every repaint. See "Text truncation" under Conventions.

### The spare framebuffer

`DrawBuffer` carries a third 48 KB buffer serving two jobs that never overlap, tracked
by `spare_use_`:

- **Snapshot** — a copy of the displayed frame. `ReaderScreen::pause()` takes it when
  the quick menu opens; `resume()` puts it back. Both are pointer swaps between the
  draw buffer and the spare, no memcpy.
- **Offscreen** — where `prerender_next_page_()` draws the next page while the panel is
  still running the current waveform. `take_predrawn_()` commits it.

Two more users, each displacing whatever it held: **Cache** (`claim_spare_cache`, the
book list's thumbnails and the home cover, generation-checked) and **Lent**
(`lend_spare()` / `return_spare()`, 2026-11-12): the sync's download buffer, handed out
and taken back by `wg_sync::poll()` on the UI thread. While lent (`spare_lent_`) every
other claim refuses (cache returns null, snapshot and pre-draw skip), so no screen can
write into a buffer another task is filling. The sleep cover no longer borrows it
(`borrow_spare_scratch()` removed 2026-11-12): `cover_sleep.bin` is the panel's frame and
is read straight into the draw buffer.

They share storage because a reader is either in the quick menu or turning pages, never
both. **Claiming it for one purpose silently invalidates the other**, safe only because
both consumers re-check: `resume()` tests `has_snapshot()`, `take_predrawn_()` tests
`has_offscreen()`, and either falling false just means rendering normally.

`begin_offscreen()` redirects `draw_()` at the spare; `end_offscreen()` publishes it and
`commit_offscreen()` swaps it into the draw buffer. The commit paths (`refresh`,
`full_refresh`, `restore_snapshot`, the MGR2 sleep blit) use `buf_` directly.

There is one draw buffer, not a front/back pair. `refresh()` sends it and does not
flip: the controller keeps the pixels, the SPI write is synchronous, and every draw
starts from `fill()` or a whole-buffer swap, so nothing reads the previous frame. Between
refreshes the draw buffer is the frame on the glass. The second host copy (48 KB) was
removed because nothing read it; `RefreshMode::Full`, the only mode that takes the old
frame from the host, has no caller, and one added later must keep its own copy.

Two things `prerender_next_page_()` must get right:

- **It restores `page_` and `page_pos_` afterwards.** `render_page_()` works on those,
  so the next page is made current for the draw and then put back. `page_` keeps only
  positional metadata either way — its `LayoutWord::text` pointers into the paragraph
  window die the moment laying out the next page slides it.
- **It skips the draw for a page containing images.** `draw_image_()` reads the card,
  which shares SPI2 with the panel, so it drains the waveform first — blocking the UI
  loop for the couple of hundred ms the current page is still painting. Image pages get
  their layout cached and nothing more.

`take_predrawn_()` keys on chapter, the **pre-resolve** request position (what
`next_page_()` produces, so no `resolve_stable_position()` call is needed to compare),
font size and the full `PageOptions`. Any mismatch renders normally.

### Panel health

A run of fast partial waveforms is **not DC-balanced**: each leaves residual charge in
the microcapsules, showing as ghosting and, over a long run, permanent contrast loss.

**Wintergreen has no periodic flush, arrived at the hard way.** Three versions shipped:
inline (every 24th `refresh()` promoted to full — the flash landed on whichever page
turn happened to be 24th); deferred to a 2500 ms idle (2.5 s after the last press you
are *reading the page*, and a flash there interrupts rather than delays); removed.
`partials_since_full_`, `flush_pending()`, `flush_ghosting()`,
`Application::kGhostFlushIdleMs` and `config::kFullRefreshEveryNUpdates` are gone.

Removing it is safe **because deep-sleep wake is a full boot**. The counter never
survived sleep anyway, and every session is bracketed by two full-panel waveforms:

| When | What runs |
|---|---|
| going to sleep | the sleep image — `full_refresh(RefreshMode::Half)`, cover or wordmark |
| waking | `Application::start()`'s `full_refresh()` (Half) |

With `kAutoSleepMinutes = 1` that is a full-panel waveform every time the device is put
down and picked up — more often than a Kindle, which flashes every few pages *while you
read*.

**What is genuinely uncovered:** a session that never idles for a whole minute. Page
turns every 45 s keep `inactivity_ms_` below the timeout indefinitely, so 30 minutes is
~40 partial updates with no full refresh. That is within normal e-reader practice (Kobo
exposes 1–100 pages *and* "never"), and the page-turn waveform is the vendor's OTP fast
LUT. Expect faint ghosting late in a long session; expect it gone after the next sleep.

The canary is ghosting that *survives* a sleep/wake cycle. Nothing in the current design
would produce that; if it appears, the fix is a counter in `DrawBuffer::refresh()`
calling `full_refresh(RefreshMode::Full)` — accepting that no threshold both protects
the panel mid-session and stays out of the way.

### The half-refresh temperature write — do not remove it

`refreshDisplay()`'s `EPD_HALF_REFRESH` case sends `CMD_WRITE_TEMP` (0x1A) with `0x5A`
before setting the mode byte. It looks redundant, because `initDisplayController()`
selects the controller's *internal* sensor (`CMD_TEMP_SENSOR_CONTROL` = 0x80) and a
manual write should then have no effect.

It was removed on exactly that reasoning, and the result was a device that ran one black
flash and then displayed nothing at all, ever. `0xD4` includes **LUT_LOAD**, and the OTP
waveform it loads is selected via the temperature register; without the write the panel
comes up with no usable LUT. What made it costly to find: **`Application::start()` ends
with `buf.full_refresh()`, whose default is `RefreshMode::Half`** — so the one broken
path is the one that paints the very first screen.

Two lessons that generalise: a vendor init sequence is not a place to remove things that
"should" be no-ops; and "the code reached this point" is not "the panel showed
something".

**Temperature compensation is live**, and the mechanism explains the `0x5A` disaster.
Selecting the sensor only says *which* sensor, not when to read it. The reading happens
when **TEMP_LOAD (CTRL2 bit 5, 0x20)** is set in the update sequence, and that bit was
**never set anywhere in this driver** — so the controller never sampled its own sensor
and the register only ever held the fixed `0x5A`.

`refreshDisplay()` ORs TEMP_LOAD into **one** path:

- **`EPD_FULL_REFRESH` (0x34 → 0x54)** — the sleep image and any DC-balancing flush, the
  updates that recondition the panel and that nobody waits on.
- **`EPD_FAST_REFRESH` is excluded** — it runs the custom LUT, a fixed table no
  temperature can select between, and it is the page-turn hot path.
- **`EPD_HALF_REFRESH` is excluded for a UX reason**, tried the other way first. Half is
  the boot paint, which with `kAutoSleepMinutes = 1` runs *every time the device is
  picked up*. With TEMP_LOAD it got visibly longer and flashed more: `0x5A` corresponds
  to a much warmer panel than a room, and e-ink waveforms lengthen as they get colder.
  Several times an hour it was intolerable. Any under-driving is cleared by the next
  full refresh.

Two deliberate safety properties: **the `0x5A` write stays** as the seed the controller
falls back to, so a sensor that does not answer leaves a usable value; and **`kTempLoad`
is the one line to flip** (`0x00` restores the previous fixed-temperature behaviour).
It is a local constant rather than a config option: whether the controller implements
CTRL2 bit 5 is a property of the hardware, not a preference.

**Still not possible: reading the temperature back.** `CMD_READ_TEMP` would need MISO
from the panel, and the panel is wired write-only.

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

The rails are still powered down where it costs nothing: `refreshDisplay()`'s
`turnOffScreen` path at the end of a full refresh, which is what the sleep image uses.
