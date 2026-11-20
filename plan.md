# Wintergreen grayscale rewrite — full handoff plan

Written 2026-11-19 at the end of a long design/interview session with the owner.
Everything below was decided with the owner unless it is listed under "Open
questions". Read the whole file before touching code. Read `CLAUDE.md` and the
`docs/` file matching each area (as CLAUDE.md requires) before editing it.

The owner will flash and rebake later; **no device is available during the
build session**. Everything must be written, compile-checked (`pio run`), and
host-tested (`tools/tests/run.sh`) as far as possible without hardware.

---

## 0. Revisions decided at the start of the build session (2026-11-19) — override the sections below

Answers to §11:
- Q1: selection = **black fill, white text, gray AA** (not gray fill). Secondary text
  (author, percentages, battery) **black with AA**, not gray. Hairlines, borders,
  scrollbar, outlines gray.
- Q2: low-battery bar gray. Q3: post-sync list redraw = normal update (no flash).
- Q4: strip height tunable within budget. Q5: Libron v0.25 release TTFs fetched.

Efficiency revisions (owner: as little RAM and CPU as possible, every navigation at
the waveform floor):
- **Damage culling.** `refresh()` matches `prev_` and `cur_` op by op; pixels outside
  the union of differing ops' bboxes are known equal → HOLD, never rendered. Per
  strip only the damaged physical x-range is replayed, diffed and sent (RAM window
  per strip, unioned with what that strip's controller RAM may still hold).
- **No cover cache pool.** Covers, thumbnails and reader images stream from the card
  inside changed areas only. A file op's old pixels come only from the fetch of an
  identical op in `cur_` (same source, same place); otherwise UNKNOWN. Old files are
  never re-read. Fetches happen per strip with the SPI bus idle (SD shares SPI2).
- **Sync buffer from heap** (16 KB chunk, measured 296 KB/s), no loan.
- **No `page_buf_`.** The display list is read straight into the draw-list arena.
  Forward staging: right after a waveform ends, the next page's list is read into the
  free arena (zero RAM). Quick-menu snapshot: the page's list is parked in the tail of
  whichever arena holds the menu (zero RAM, no card read on close).
- Target ~25 KB display RAM instead of 96,000 B.

## 1. Goal (owner's words, condensed)

- Use the display hardware as fully as possible across **everything**: reader,
  home, book list, quick menu, chapter grid, sleep screen, covers, the
  wintergreen wordmark.
- Text and graphics should look as smooth as the panel allows: as few
  noticeable hard pixels as possible ("minimalism looks better when the device
  doesn't have any hard pixels noticeable").
- **No additional passes/updates.** A page turn (or any screen change) is ONE
  panel update. When it lands, all grays are already there. Nothing changes
  after the page has landed (the owner compares the old behaviour to web CLS:
  text appeared, then its edges faded/changed — never again).
- Fast: page turns and menu moves stay at the latency floor. Light, optimized,
  tested, bug-free, healthy for the panel (no long-term ghosting), best
  day-to-day reading experience.
- This is not an experiment. **No compile-time switch back to 1-bit, no escape
  hatch.** This *is* Wintergreen from now on; restructure anything needed.
  Old code lives in git. (The `WG_BLUETOOTH_PAGE_TURNER` / `WG_WIFI_SYNC`
  defines stay — owner confirmed they are feature config, not escape hatches.)
- The owner explicitly approved resurrecting grayscale, which
  `docs/history.md` lists as removed ("don't resurrect without asking" — asked
  and approved 2026-11-19).

## 2. Hardware facts and research (why the design is what it is)

### 2.1 Panel and controller
- Panel: Good Display **GDEQ0426T82** (4.26", 800x480), controller **SSD1677**.
  `platforms/esp32/epd.h`. SPI2 at 40 MHz, write-only (no MISO from panel; the
  controller temperature register cannot be read back).
- Per update, each pixel's waveform is chosen by **2 bits**: one in BW RAM
  (0x24), one in RED RAM (0x26). That is 4 waveforms ("LUT0..LUT3") per update.
  **So one update can produce at most 4 distinct outcomes per pixel.** More
  gray levels (16 etc.) need multiple sequential updates — forbidden by the
  owner.
- Today's partial (page turn) uses the panel's built-in OTP fast waveform
  (`EPD_FAST_REFRESH`, CTRL2 `0x1C`): BW = new frame, RED = old frame (the
  controller tracks/copies it), LUT index = (old bit, new bit). 1-bit only.

### 2.2 Why 3 levels for normal updates, 4 for full refreshes
- To avoid touching unchanged pixels (no flicker, no needless drive, panel
  health), one of the 4 slots must be **HOLD** (no drive). That leaves 3
  targets: **to-white, to-gray, to-black**. Proof it can't be 4: HOLD + 4
  targets = 5 slots > 4.
- 4 targets (white/light/dark/black) are only possible when every pixel is
  driven regardless of its old state = a full-screen waveform. That is fine
  for **full refreshes** (boot paint, sleep screen) which flash anyway.
- Therefore: **normal updates (page turns, menus): 3 levels (white, gray,
  black). Full refreshes (boot, sleep image/cover): 4 levels (white, light,
  gray, black).** The "gray" of both must be the same physical shade (see
  waveform design §5: same pulse from white in both LUTs).
- Owner asked "what does 4 give over 3?" — two gray steps (smoother edges and
  photos), only where the whole panel is driven anyway. Decision: 4 only in
  full refreshes, 3 everywhere else. This is the hardware maximum without
  extra passes.

### 2.3 History that must not repeat
- `docs/history.md` "Antialiased (grayscale) text in the reader": the old
  multi-pass AA made every page turn 3 physical updates; mid/dark AA showed as
  solid black then visibly thinned after the page had changed. Owner hated it
  (the "CLS" effect). Root cause: `kLutGrayscale`'s white/black both on LUT0 =
  do nothing, gray only reachable by driving black then pulling back. **The new
  design is one update per change; any design that needs a second update is
  rejected.**
- `DrawBuffer.cpp` comment on `show_sleep_image_embedded()`: the old one-pass
  `kLutFactoryQuality` (vendor 4-gray LUT) staggered phases by level: black
  settled early while the white background churned for most of the update —
  seen as "wordmark first, then the screen flashes". Lesson: **right-align all
  target pulses so every level finishes in the same final frame** (§5).
- `docs/display.md`: never remove the `0x5A` temp write on the OTP Half path
  (that path disappears entirely now, see §5.5 — read that section before
  deleting it), event-driven BUSY wait, SD shares SPI2 (wait panel idle before
  card access), region refresh can't update only changed rows (the controller
  drives the whole panel from RAM; with our own HOLD slot this is now handled
  by writing HOLD everywhere unchanged — see §4.6 for the skip rule), no idle
  rail power-down, no periodic flush.

### 2.4 External research (2026-11-19)
- **Adafruit_EPD `ThinkInk_426_Grayscale4_GDEQ`** — hardware-verified 4-gray
  driver for this exact panel (also ported to Rust crate **epdsi** v0.6.0,
  confirmed on ESP32-C3). Source:
  https://raw.githubusercontent.com/adafruit/Adafruit_EPD/master/src/panels/ThinkInk_426_Grayscale4_GDEQ.h
  - Voltages: gate `0x17`; source `0x41, 0xA8, 0x32`; VCOM `0x30` —
    **identical to our old `kLutFactoryQuality`** (git: `git show
    312fb4c^:platforms/esp32/epd.h`, lines ~57-90). These are the vendor's
    values. **Keep them exactly; they are the electrical safety guarantee.**
  - LUT layout: 105 bytes to `0x32` = 5 VS blocks x 10 bytes (LUT0-3 + VCOM),
    10 groups x 5 bytes TP/RP (TPA, TPB, TPC, TPD, RP), 5 frame-rate bytes.
    Then `0x03` gate voltage (1 byte), `0x04` source voltage (3 bytes),
    `0x2C` VCOM (1 byte). Old `setCustomLUT_()` in that git revision shows
    the exact command sequence.
  - Adafruit's gray mode is **two passes** (OTP mono preclear, then custom LUT
    with CTRL2 `0xCF`, CTRL1 `0x00`). Not usable as-is (two passes), but it
    proves the custom-LUT path, voltages, and that the custom LUT must be
    re-uploaded after any OTP LUT load (OTP load overwrites LUT + voltages).
  - Their LUT bytes (for reference): VS LUT0 all 0; LUT1 `54 54 40`; LUT2
    `AA A0 A8`; LUT3 `A2 22 20`; TP groups 0-2 `01 01 01 01 00`; FR `8F` x5.
- **CrossMux** (github.com/ryokun6/crossmux, X3/X4 firmware): "renders the two
  grayscale planes in narrow strips and writes them directly to the display,
  instead of keeping two extra full-screen buffers in RAM." Same strip idea we
  adopt. (It doesn't document passes/LUTs.)
- CrossPoint Enhanced Reading Mod notes the stock first refresh under-drives
  (gray tint after power-on); they force a full waveform on boot — consistent
  with our full refresh at boot.

### 2.5 Waveform polarity (VS codes) — evidence, not certainty
VS 2-bit codes: `00` = VSS (no drive), `01` = VSH1, `10` = VSL, `11` = VSH2.
Conclusion used by this plan: **VSH1 (`01`) drives toward black, VSL (`10`)
toward white.** Evidence:
1. Our old `kLutFactoryQuality` showed the MGR2 sleep image correctly with the
   decode that set a RAM bit for *ink* (black = state 3 = LUT3). LUT3 ends with
   VSH1 pulses, LUT0 (white) ends with VSL → VSH1 = black, VSL = white.
2. In that table LUT2 ("dark") has less trailing VSL than LUT1 ("light") —
   consistent.
3. Adafruit's table is consistent with it given their mono-preclear baseline.
State index = `(RED_bit << 1) | BW_bit` (old `docs/history.md` note and
Adafruit's `{RED_bit, BW_bit}` agree on bit order).
**Put the polarity in exactly one place** (two named constants `kVsBlack =
0b01`, `kVsWhite = 0b10`) so a wrong guess is a one-line fix on device day.

### 2.6 RAM budget (binding constraint)
- `docs/bluetooth.md` §"RAM now": measured minimum free internal heap
  **44,604 B** (sync + clicker), largest block 38,912 B; pass mark 24 KB.
  That 44 KB is the entire budget for anything new. Two 2-bit full frames
  (192 KB) or even +48 KB are impossible. (I first proposed spending +96 KB and
  the owner agreed; that was wrong — corrected and re-decided below.)
- Large mallocs after boot fail on the fragmented heap (`docs/architecture.md`
  ~line 255: a 40 KB malloc showed nothing on hardware). **All big buffers are
  allocated once at boot**, as today.
- Today: `DrawBuffer::buf_` 48,000 + `spare_` 48,000 = 96,000 B on the heap.
  The new design must fit in **≤ 96,000 B** total for display + caches
  (target ~77 KB, freeing ~19 KB).

## 3. Decisions (all owner-approved)

| Topic | Decision |
|---|---|
| Levels | 3 (white/gray/black) for every normal update; 4 (white/light/gray/black) for full refreshes (boot, sleep). |
| Passes | Exactly one panel update per change. Never a second pass. |
| Conflict: smooth vs ghosting | **No visible motion wins.** No in-update counter-pulses for DC balance; leftover charge is cleared by the sleep/boot full refreshes (as today). |
| Text rendering | **Light hinting** (vertical metrics snapped: baseline, x-height; horizontal shapes smooth via gray) **plus slight stem darkening** so weight matches today's text. Applies to reader font and UI fonts. |
| Images, covers, sleep cover | **Blue-noise dither** over the available levels (3 for book images/home/list covers, 4 for the sleep cover and wordmark). |
| Escape hatch | None. Remove/never add compile-time fallbacks. Radio defines stay. |
| UI gray | Gray "everywhere we can": secondary text (author, percentages, battery), **gray fill selection with black text** (replaces inverted black bar), gray hairlines/dividers. AA on all UI text and shapes (e.g. the home diamond). See open question Q1 about legibility costs. |
| "Progress bars" | None exist (my mistake in the interview); the list shows percentages as text. |
| Wordmark | Source `~/Downloads/wintergreen-source.png` (960x1600 RGBA, black hand-drawn strokes: tree + "Wintergreen" + "Alec♡"). **Full screen, as drawn**: scale exactly 0.5 → 480x800 portrait; the 7 hidden panel rows fall at the top (white margin there). 4 levels. Copy the PNG into `resources/` (e.g. `resources/wintergreen-source.png`) so it's in the repo. |
| RAM architecture | **Draw-list design, no pixel framebuffers** (§4). Owner approved after explanation. |
| Temperature | Custom LUTs lose OTP temperature compensation. **Scale pulse lengths by ESP32-C3 on-chip temperature sensor in a few bands** (§5.4). |
| Safety | Vendor voltages unchanged (§2.4). Only timings are ours. Worst case = poor image until reflash; no electrical risk. |
| RAM spend | Must fit in today's 96,000 B (approved "spend RAM" was retracted when the 44 KB heap floor was found). |

## 4. Architecture: draw lists + strip renderer

### 4.1 Idea
The panel needs, per pixel, old level and new level to choose HOLD vs a
target. Storing the old frame as pixels costs 96 KB (2 bits) or 77 KB (3-level
packed) — too much alongside a new frame. Instead **remember screens as the
draw calls that produced them**:

1. A screen draws through the `DrawBuffer` API exactly as today (`fill`,
   `fill_rect`, `draw_text_proportional`, `draw_baked_line`, blits). Those calls
   are **recorded** into a draw list (`cur_`), not rasterised.
2. On `refresh()`, the panel is processed in **strips of physical rows**
   (proposal: 16 rows → 30 strips; tune). Per strip:
   - replay the **previous** list (`prev_`, what's on the glass) into scratch
     `old` (2 planes x 16 rows x 100 B = 3.2 KB),
   - replay `cur_` into scratch `new` (3.2 KB),
   - compute slots per pixel: `old == new` → HOLD, else to-white / to-gray /
     to-black,
   - write the strip's BW and RED slot planes to controller RAM
     (`setRamArea` window for that strip, `0x24` then reset counters, `0x26`)
     using async DMA from a send buffer while the CPU renders the next strip
     (double-buffered send buffers 2 x 3.2 KB).
3. Fire the turn LUT (one update). `prev_ = cur_` (swap).

Pages are already tiny baked display lists (≤ `kWgbMaxPageBytes`, measured max
879 B); menus are a few dozen calls. Replaying both is cheap (glyphs come from
flash-mapped fonts).

### 4.2 Performance (as explained to and accepted by the owner)
- Each update now sends **2 planes = 96,000 B** instead of 48,000 B: ~19 ms
  at 40 MHz instead of ~10 ms. **Inherent to gray** (both RAM planes carry
  slot bits); no precompute can remove it.
- Strip rendering (~10 ms CPU estimated for a text page, both lists) runs
  **while the previous strip is DMA'd**, so it hides behind the send. The
  next-page pre-draw (`prerender_next_page_`, `take_predrawn_`,
  `predraw_after_idle_`, `stage_next_page_`'s purpose) becomes unnecessary —
  rendering is no longer the bottleneck. Owner's concern: "would this undo
  the everything-precomputed ideology?" Answer given: mostly kept; the press
  starts the send immediately and rendering rides along; the floor rises
  ~10 ms only because gray needs two planes. **Measure on device**: add
  temporary timing (then remove — no logging rule) for refresh() start→
  MASTER_ACTIVATION.
- Battery: ~10 ms more CPU per press vs ~300 ms panel drive. Small.
- **Skip unchanged strips**: if a strip is all HOLD *and* controller RAM for
  that strip already holds all-HOLD (tracked per strip: `ram_clean_[30]`),
  don't send it. Menus (cursor moves) then send less than today. HOLD is
  encoded as state `00` (both planes 0) so that if the controller copies
  BW→RED after an update (it does something like that for OTP fast; unknown
  for custom LUT), a HOLD strip stays HOLD. Any other state may change under
  a copy, so `ram_clean_` is only true for strips last written all-zero.
  **Never skip a strip whose RAM may hold drive slots** — the controller
  drives the whole panel from RAM every update (docs/display.md "Region
  refresh" lesson).

### 4.3 RAM layout (fits in today's 96,000 B)
| Buffer | Size | Notes |
|---|---|---|
| draw list `prev_` | ~6 KB arena | what's on glass |
| draw list `cur_` | ~6 KB arena | being recorded; swapped after refresh |
| strip scratch old/new | 2 x 3.2 KB | 2 planes x 16 rows x 100 B |
| send buffers | 2 x 3.2 KB | DMA-capable (heap), BW+RED for a strip, double-buffered |
| cover cache pool | ~52 KB | replaces `claim_spare_cache`; holds the home cover (424 cols x ceil(486/8)=61 B x 2 planes = 51,728 B) **or** the book list's visible thumbnails (76 x ceil(115/8)=15 B x 2 = 2,280 B each) — never both (different screens) |
| **Total** | **~77 KB** | vs 96,000 B today → ~19 KB freed |

Exact arena size: size it from the worst case (largest display list copied in
+ largest menu) and prove it in a host test. All allocated once at boot (heap,
not BSS — see the `DrawBuffer.h` comment about the BT controller's DRAM pool).

### 4.4 Draw-list op set (proposal; design precisely in session)
Every op stores a precomputed **physical row range** (bbox) so per-strip replay
rejects it in O(1).
- `Fill(level)` — whole screen.
- `Rect(x, y, w, h, level)` — logical coords + rotation baked to physical.
- `Text(x, baseline, font*, style, ink, bytes...)` — **copy the UTF-8 bytes
  into the arena** (callers pass temporaries/`snprintf` buffers). Fonts are in
  flash/rodata (stable pointers).
- `Baked(line...)` — the reader page: copy the **whole display list** once into
  the arena (≤ `kWgbMaxPageBytes` 4 KB) and reference lines by offset;
  `page_buf_` may be overwritten before the list is retired (e.g. staging the
  next page). Resolve per-word bboxes at record time.
- `BlitCache(slot, gen, x, y, w, h)` — 2-plane column-major data in the cover
  cache pool; replay checks generation; stale → treat as UNKNOWN (§4.5).
- `BlitFile(source, x, y, w, h, keep rows)` — reader images read from the open
  WGB per strip (portrait strip = logical column range). Replay of a
  `BlitFile` op **from `prev_` yields UNKNOWN** (we don't re-read the old
  image; owner accepted: "its area is redriven like a normal change").
- `Frame(source)` — full-screen 2-plane frame streamed from flash/file (sleep
  image, sleep cover) for the full-refresh path.
- Ink modes: black ink (max-composite), white ink (for any remaining inverted
  drawing), gray ink (secondary text — see Q1).

Arena overflow must never crash: mark the list `overflowed`; an overflowed
`prev_` replays as UNKNOWN everywhere (every pixel driven to its new level,
like a non-HOLD update) and an overflowed `cur_` forces a full refresh. Host
test proves real screens never overflow.

### 4.5 UNKNOWN old pixels
Replaying `prev_` can produce UNKNOWN (stale cache generation, old image, a
list that overflowed, first update after boot is a full refresh anyway). Slot
rule: `old == UNKNOWN` → drive to the new level (to-white/to-gray/to-black).
Driving white→white or black→black is invisible (saturated); gray→gray via the
to-gray waveform passes through white briefly — acceptable because it only
happens where content just changed (image page left, cache evicted).

Implementation tip: carry UNKNOWN as a third scratch plane only when the strip
intersects an UNKNOWN-producing op (rare), to keep the common path 2 planes.

### 4.6 Frame encodings
- **Level** (2 bits, logical): 0 white, 1 light (full refresh only), 2 gray,
  3 black. Scratch planes: `hi` and `lo` bit planes per row (100 B each per
  physical row, same byte/bit layout as today's frame: `kStride` = 100,
  `kPanelOffsetX` = 7, MSB = leftmost). Two 1-bit planes let the existing
  `put_row_` / `blit_cols_` machinery be reused per plane.
- Compositing for black ink = OR into both planes (white 00 < gray 10 < black
  11 is a max under OR). Gray glyph pixel = set `hi` only.
- **Slots** (turn LUT state = `RED<<1 | BW`): `00` HOLD, `01` to-white, `10`
  to-gray, `11` to-black. (Choose any assignment, but keep HOLD = `00`, §4.2.)
- **Full LUT states**: `00` white, `01` light, `10` gray, `11` black (level =
  state).

### 4.7 What goes away
- `DrawBuffer::buf_`/`spare_`, the `Spare` enum, `save_snapshot`/
  `restore_snapshot`, `begin_offscreen`/`end_offscreen`/`commit_offscreen`,
  `frame()` (sleep cover read straight into the frame), `claim_spare_cache`
  (replaced by the cover cache pool with the same owner+generation semantics).
- `ReaderScreen` pre-draw machinery (`prerender_next_page_`, `PreDrawn`,
  `take_predrawn_`, `prerender_pending_`, `predraw_after_idle_`) and the
  snapshot fast path (`snapshot_ok_`, `snapshot_font_size_`,
  `snapshot_rotation_`). Quick menu close = record the page again from
  `page_buf_` (it's ~free) and refresh; the diff makes it a normal update.
  Keep `stage_next_page_` only if it still saves a card read on the critical
  path (it reads the next display list before the waveform — still useful
  because SD can't be read during a waveform; decide in session).
- OTP waveforms entirely (`EPD_FAST_REFRESH` OTP `0x1C`, `EPD_HALF_REFRESH`
  `0xD4` + `0x5A`, `EPD_FULL_REFRESH` `0x34|TEMP_LOAD`). See §5.5.
- `DrawBuffer::show_sleep_image_embedded()` 1-bit threshold decode.
- Sync's spare loan (`lend_spare`/`return_spare`, `wifi_sync.h` ~line 259-300,
  819): **lend the cover cache pool instead** (52 KB ≥ today's 48 KB loan,
  so `DownloadBuf` keeps whole sectors). While lent, cache ops replay as
  UNKNOWN / thumbnails absent, exactly as today's semantics.

### 4.8 Rotation and strips
Strips are always **physical row ranges** (natural controller RAM order; one
`setRamArea` per strip).
- Portrait (`Deg90`): physical row = logical column. Glyphs use the
  column-major copy (`blit_cols_`) — each glyph column is one physical row, so
  clipping to a strip = clipping columns. Covers are column-major too (a strip
  is a contiguous column range of the cache/file). Reader images: change the
  portrait copy from 8-row bands to **column-major** so a strip is one
  contiguous read (WGB format change; rebake accepted).
- Landscape (`Deg0`): physical row = logical row. Row-major glyphs/images,
  strip = row range, contiguous.

### 4.9 Full refresh path
`full_refresh()` (boot paint `Application::start`, sleep): replay `cur_` per
strip → write level planes directly as LUT states (no diff) → full LUT
(clear + set, §5.2), `prev_ = cur_`. Sleep image/cover: `Frame` source
streamed per strip. `kSunlightFadingFix` (panel off after full refresh) keeps
working via `turnOffScreen`.
`MainMenu.cpp` ~line 120 does a `full_refresh()` when a sync changed the book
list → see Q3.

## 5. Waveforms (blind design; tune on device)

All four tables built **at runtime** into a 105-byte RAM array from named
frame counts (so temperature scaling is arithmetic, §5.4), uploaded with the
old `setCustomLUT_` sequence (`0x32` 105 B, `0x03` 1 B, `0x04` 3 B, `0x2C`
1 B) only when the needed table differs from the loaded one. Small polled SPI
writes (LUT arrays aren't DMA-capable if in rodata; RAM arrays are fine).
CTRL1 = `0x00` (use RED). CTRL2 = `0x0C` (DISPLAY_START + MODE 2, **no
LUT_LOAD** — LUT_LOAD would overwrite our table from OTP), plus `0xC0` when
the analog is off, plus `0x03` for `turnOffScreen` — exactly as the old
custom path (`custom_lut_active_ ? 0x0C : 0x1C`).

### 5.1 Turn LUT (normal updates) — 3 targets + HOLD
Base saturation length **D** frames, gray pulse **P** frames, all targets
**end on the same final frame** (right-aligned):
| Slot | Phases (group 0: TPA = D−P, TPB = P) | VS byte |
|---|---|---|
| HOLD (`00`) | VSS, VSS | `0x00` |
| to-white (`01`) | VSL, VSL | `0xA0` |
| to-gray (`10`) | VSL (D−P), then VSH1 (P) | `0x90` |
| to-black (`11`) | VSH1, VSH1 | `0x50` |
VCOM LUT4 all `0x00`. Remaining groups zero.
- to-gray from black passes through white then lands gray; from white it stays
  white then lands gray; from gray it lightens then returns. Accepted (only on
  changed pixels).
- Not DC-balanced (same as today's OTP fast). Owner chose "no visible motion"
  over in-update balancing.
- Starting values (derived from the old factory table, which saturated in
  ~20 frames at FR `0x22`, and its ~5-frame light/dark offsets): **FR `0x22`,
  D = 20, P = 5.** Unverified; frame period at FR `0x22` unknown — measure
  BUSY duration on device and tune D toward the OTP fast turn (~300 ms) or
  shorter if blacks are solid.

### 5.2 Full LUT (boot, sleep) — 4 levels, clear then set
- Clear (all states): VSH1 for C frames, then VSL for C frames (balanced pair;
  this is the flash, same as today's full/half refreshes). Start C = 20.
- Set, right-aligned to the last frame: white = VSS; light = VSH1 for PL;
  gray = VSH1 for **P (same P as the turn LUT → same physical gray)**; black =
  VSH1 for D. Implement as one group with phases A = D−P, B = P−PL, C = PL:
  white `00 00 00`, light `00 00 01`, gray `00 01 01`, black `01 01 01`.
  Start PL = 2.
- Same vendor voltages.

### 5.3 Same gray in both LUTs
Gray in both is "from white, VSH1 for P frames". The turn LUT's to-gray first
saturates white (D−P frames), the full LUT's clear ends on white. Keep P shared
by construction (one constant).

### 5.4 Temperature bands
- ESP-IDF `driver/temperature_sensor.h` (ESP32-C3 on-chip; chip temp ≈ ambient
  + self-heating). Install once at boot, read before each refresh (cheap), or
  on a slower cadence — measure the cost.
- Bands (proposal): ≥ 20 °C x1.0; 10-20 °C x1.25; < 10 °C x1.5 on D, P, PL,
  C (clamp each TP to 255). Re-upload LUT only when the band changes.
  Hysteresis of ~2 °C to avoid flapping.
- Check `sdkconfig.defaults` for anything needed (the driver is part of IDF;
  no new component).

### 5.5 Removing the OTP paths — read first
`docs/display.md` "The half-refresh temperature write — do not remove it":
removing the `0x5A` write once produced a panel that showed nothing because
`0xD4` includes LUT_LOAD and the OTP LUT was selected via the temperature
register. In the new design **nothing sets LUT_LOAD**, so the OTP selection
no longer matters. Still: keep `CMD_TEMP_SENSOR_CONTROL 0x80` in init (harmless),
and treat "panel shows nothing after flash" as the first suspect being the
custom-LUT path (CTRL2 value, LUT upload order: upload LUT *before* writing
RAM? either is fine as long as it's after `waitWhileBusy()`). Update the doc
section to explain the OTP path is gone and why the old lesson no longer bites.
`wakeIfNeeded()` re-inits the controller after deep sleep: **the custom LUT
and voltages must be re-uploaded after every controller reset/init** (track a
`loaded_lut_ = None` reset in `initDisplayController()`).

### 5.6 Device tuning checklist (for the owner's flash day; put in docs)
1. Panel shows anything? If inverted: swap `kVsBlack`/`kVsWhite`.
2. Blacks solid, whites clean on a page turn? Adjust D.
3. Gray level pleasant and identical between boot paint and after a turn?
   Adjust P (and PL for the sleep cover's light level).
4. Turn duration (temporary timing, then remove).
5. Ghosting after ~50 turns; gone after sleep/wake? (canary in
   docs/display.md "Panel health").
6. Cold test (fridge) for temperature bands.

## 6. Content pipeline changes (rebake + regenerate accepted)

### 6.1 Fonts (WGF/WGFS, `tools/make_font.py`, `tools/wgf_columns.py`, `tools/check_font.py`, `wintergreen/content/WgfFormat.h`, `BitmapFont.h`)
- Render with FreeType **light hinting** (`FT_LOAD_TARGET_LIGHT`, 8-bit
  coverage, `FT_RENDER_MODE_NORMAL`) + **stem darkening** (FreeType
  `no-stem-darkening=false` for the autofitter/CFF, or an equivalent coverage
  gamma) so weight matches today's mono-hinted text.
- Quantise coverage to **3 levels** with two tunable thresholds (white/gray,
  gray/black). Store each glyph as **two 1-bit planes** (`hi` = gray-or-black,
  `lo` = black), both row-major and column-major (column copy as today via
  `wgf_columns.py`, now per plane). Docs/content.md documents the old 5-level
  `(bw, msb, lsb)` encoding and the "37.5% threshold" — replace with the new
  encoding; there's no magic/version, so the format changes in the same commit
  as all assets.
- Advances: light hinting gives unhinted fractional advances (already stored in
  quarter-pixels). The draw loop snaps the cursor to whole pixels per glyph
  (`DrawBuffer.cpp` `draw_glyphs_`); host `TextLayout` must measure the same
  way (it already shares `word_width`) → **rebake the library** (rule: font
  bundle change = rebake, same commit; owner will run `books`).
- Kerning: GPOS/kern class kerning already extracted and applied
  (`make_font.py` "Kerning" section) — keep.
- Reader font **Libron v0.25** (github.com/nicoverbruggen/libron), four
  styles (Regular/Bold/Italic/BoldItalic), sizes `32,36`, `--line-height 150`
  (docs/content.md command). Only a Regular web `libron.ttf` exists locally
  (`~/Projects/site/static/fonts/libron.ttf`) — **fetch the v0.25 release
  TTFs** (check the exact version matches; if not, ask the owner).
- UI fonts: **Iosevka Slab Medium** (SGr-IosevkaSlab, TTC face 0), sizes
  24 (`ui_large`) and 32 (`ui_header`), plus `ui_digits` 40 px digits-only
  (`--digits`). Not found in /nix/store; get via nixpkgs `iosevka-bin`
  variant `SGr-IosevkaSlab` (or the Iosevka release). Arrays stay `inline
  constexpr` (CLAUDE.md gotcha). Header sizes roughly double — flash is not
  scarce (app0 6.4 MB vs ~1.2 MB image).
- `--mono` flag: currently a no-op; remove it (no escape hatches) and update
  docs commands.
- Python deps: no system python3. Use e.g.
  `nix shell --impure --expr 'with import <nixpkgs> {}; python3.withPackages (ps: [ps.freetype-py ps.fonttools ps.pillow])' -c python3 tools/make_font.py ...`
  (owner's global rule: `nix shell`, never `nix-shell -p`). Update the docs
  commands that still say `nix-shell -p`.
- Size: today's font partition content 573,486 B (1 MB partition). Two planes
  ≈ 2x → ~1.15 MB. **Grow `font` to 2 MB**: `0xC90000` size `0x200000`; move
  `sleep` to `0xE90000` (128 KB). Free space exists up to coredump at
  `0xFF0000`. Edit the 3 places in step (CLAUDE.md "New asset = edit 3 in
  step": CSV, `PARTITIONS` in `tools/generate_assets.py`, `map()` call site).
- `resources/Libron.bin` stays `[u32 raw size][zlib]` wrapping the WGFS
  bundle; regenerate it.
- Review aid: make `tools/check_font.py` render a sample as a **3-level PNG**
  (simulated panel) for the owner to judge weight/thresholds without a device.

### 6.2 Book images (WGB) — `WgbFormat.h`, `WgbWriter`, `WgbConverter.cpp` (~291-336), `ImageDecoder`
- Replace the 1-bit dither with **blue-noise 3-level** quantisation (white,
  gray, black). Store 2 planes. Landscape copy row-major; **portrait copy
  column-major** (replacing 8-row bands, §4.8). Update `WgbImageRef`
  `data_size` math, `wgb_image_bands_bytes` → new helper, `ReaderScreen::
  draw_image_` read path, `paginate_book_test` checks.
- Blue-noise threshold map: generate once (void-and-cluster, e.g. 64x64) at
  build time or check in as a small table in the host converter (host-only;
  the device never dithers).
- Gray tone mapping: the panel's gray ≈ some reflectance between white and
  black; make the target luminance of "gray" (and "light" for 4-level) a named
  constant in the converter (start: gray 0.5, light 0.75) for device tuning.
- Keep the existing contrast stretch (`ImageDecoder.cpp` ~321).

### 6.3 Covers — `Book.cpp` (`write_cover_bin` ~86, `write_sleep_cover_bin` ~164), `CoverPaths.h`, `BookIndex` thumbnail pack, `tools/books/main.cpp` ~227
- `cover_list.bin` (76x115) and `cover_home.bin` (424x486): 3-level blue
  noise, `[u16 w][u16 h]` + w columns, each column = `hi` plane bytes then
  `lo` plane bytes (or plane-interleaved — choose what makes a strip one
  contiguous read). Thumbnail pack slot size (`thumb_pack_slot_bytes`) doubles.
- `cover_sleep.bin`: **4-level**, the panel frame as **per physical row
  `hi` (100 B) then `lo` (100 B)** = 96,000 B, so the full-refresh streamer
  reads one contiguous chunk per strip. Exact-size check stays (wrong size →
  wordmark). Update `CoverPaths.h` helpers/constants (`kSleepFrameBytes`).
- Keep the trim/stretch-to-panel behaviour and its documented reasons.

### 6.4 Sleep wordmark — `tools/make_sleep_image.py`, `resources/sleep.mgr`, `generate_assets.py`
- Replace MGR2 with the **same 96,000 B frame format as `cover_sleep.bin`**
  so one streamer serves both (no header, no magic — same build flashes it).
- Source `resources/wintergreen-source.png` (copy from Downloads). Composite
  RGBA over white, scale exactly 0.5 (960x1600 → 480x800, high-quality
  downsample = natural AA), map portrait → physical frame (top 7 rows land in
  the hidden `kPanelOffsetX` columns), quantise to 4 levels with blue noise
  (flat black/white areas stay exact; only AA edges get 4-level shading).
- `DrawBuffer::set_sleep_image` → feeds a `Frame` source.

## 7. Screens and UI (`wintergreen/screens/`, `Application.cpp`)
Read `docs/architecture.md` first.
- All drawing keeps going through `DrawBuffer`; signatures change from
  `bool white` to a level/ink enum where needed.
- `ListMenuScreen::draw_list_` (~line 152): selection = **gray fill, black
  text** (was inverted black bar). Thumbnail borders (~229-234) gray. Row
  hairlines/separators (~242, `ListMenuScreen.h` ~149) gray. Scrollbar (~254)
  gray.
- Secondary text gray: author/subtitle, right-column percentages (~158, ~207),
  battery % (`draw_battery_` ~94). See Q1.
- `MainMenu`: "hairline before never-opened" gray; thumbnails via cover cache
  pool (`thumb_bits_ = claim_spare_cache` ~498 → pool).
- `HomeScreen`: cover via pool (~89, ~368); the diamond (~19-38, "hard edges")
  drawn antialiased (gray edge pixels).
- `QuickmenuScreen`: selection rows (~408) and chapter grid cells (~447) gray
  fill + black text/digits; header separator gray.
- `ReaderScreen`: rules (~461) AA/gray as appropriate; low-battery bar (~485)
  gray (owner: "gray everywhere we can"); images via `BlitFile`.
- `Application::show_book_cover_sleep_` (~96) and wordmark → `Frame` source +
  full refresh with turnOffScreen.
- No placeholder/empty-state text anywhere (owner rule, memory
  `no-fallback-text`).

## 8. Files touched (checklist)
- `platforms/esp32/epd.h` — custom LUT builder/upload, strip RAM writes
  (window per strip, both planes), temperature bands, remove OTP paths,
  re-upload after init.
- `platforms/esp32/main.cpp` — sleep mapping → frame source, temp sensor init,
  allocation order (big buffers early).
- `platforms/esp32/default_16MB.csv`, `tools/generate_assets.py` — partitions.
- `platforms/esp32/wifi_sync.h` — lend cover pool instead of spare.
- `wintergreen/display/DrawBuffer.{h,cpp}` — rewritten: recorder, ops,
  strip replay, diff, cover pool, IDisplay interface change (strip-based send:
  e.g. `begin_update(mode)`, `send_strip(row0, rows, bw, red)`, `fire()`).
- `wintergreen/display/ui_font_*.h` — regenerated.
- `wintergreen/content/WgfFormat.h`, `BitmapFont.h` — 2-plane glyphs.
- `wintergreen/content/wgb/WgbFormat.h`, `WgbWriter.*`, `WgbConverter.cpp`,
  `ImageDecoder.*`, `Book.cpp`, `CoverPaths.{h,cpp}`, `BookIndex.{h,cpp}`.
- `wintergreen/screens/*` — as §7.
- `tools/make_font.py`, `tools/wgf_columns.py`, `tools/check_font.py`,
  `tools/make_sleep_image.py`, `tools/books/*` (cover writing),
  `resources/Libron.bin`, `resources/sleep.mgr` (→ new frame format; rename
  if clearer, e.g. `resources/sleep.frame`, and update all references).
- If a core `.cpp` is added/removed: edit **both**
  `platforms/esp32/CMakeLists.txt` and `tools/books/CMakeLists.txt` (and
  `tools/tests` build) — CLAUDE.md. `DrawBuffer.cpp` is firmware-only today;
  the tests must be able to build the renderer on host (add it to the test
  build only).
- Docs: `CLAUDE.md` (spare buffer, framebuffers, fonts "1-bit/mono", sleep
  image, Half refresh, OTP, tests table, assets table sizes/offsets, SD-wear
  unaffected), `docs/display.md` (refresh latency, pre-draw, spare, panel
  health, temp write, new LUT section + tuning checklist), `docs/content.md`
  (WGF encoding, images, covers, sleep image format, font commands),
  `docs/architecture.md` (cover in spare → pool, snapshot), `docs/sync.md`
  (loan), `docs/history.md` (grayscale resurrected 2026-11-19 with the
  one-pass/draw-list design and why it differs from the removed one; removed:
  pre-draw, snapshot, spare, OTP waveforms, mono fonts, MGR2).
- Memory file: none needed beyond the pointer to this plan.

## 9. Tests (host; `nix shell nixpkgs#gcc nixpkgs#cmake nixpkgs#gnumake -c tools/tests/run.sh ~/books`)
Update/replace:
- `blit_test` → 2-plane glyph/image/cover blits vs per-pixel reference, both
  rotations, all clipping, **strip clipping** (render by strips == render
  whole).
- `baked_line_test` → baked page via draw list replay == reference render
  (both rotations, every style/valign, real reader font).
- `paginate_book_test` → new image layout (column-major portrait copy),
  sizes, plus "display list fits the draw-list arena".
New:
- `drawlist_test`: record/replay equivalence; arena worst cases (largest
  page, largest menu) never overflow; overflow handling; text bytes copied
  (caller buffer reuse doesn't corrupt).
- `diff_test`: slot computation incl. UNKNOWN, HOLD = `00`, `ram_clean_`
  skip rule (never skip a strip whose RAM may hold drive slots), full-refresh
  level→state mapping.
- `lut_test`: built tables — right alignment (every non-HOLD target ends on
  the same frame), shared P between LUTs, temperature scaling and clamping,
  byte layout (105 B) and voltages unchanged.
- `frame_test`: sleep frame format round trip (converter writer ↔ device
  streamer), exact-size rule.
- `dither_test`: blue-noise quantiser — flat inputs at exact levels stay
  exact, mean tone preserved, 3- and 4-level.
- Font: `check_font.py` parses the new format; a host test that every glyph's
  planes are consistent (`lo` ⊆ `hi`).
- **Preview tool** (host): render real screens (a page from a baked book, the
  book list, home, quick menu, sleep cover, wordmark) through the same
  draw-list/strip code to PNGs simulating the panel levels, so the owner can
  review the look before flashing. Put outputs in the scratchpad, not the repo.
All tests: one `main()` + asserts, ASan/UBSan, no framework (CLAUDE.md).

## 10. Build / verification order (suggested phases)
1. Read: CLAUDE.md, docs/display.md, docs/content.md, docs/architecture.md,
   docs/history.md, docs/sync.md "loan", docs/bluetooth.md RAM section, and
   this plan. Ask the owner the **open questions** (§11) first.
2. Fonts: fetch TTFs, update `make_font.py`/`wgf_columns.py`/`check_font.py`,
   regenerate `Libron.bin` + UI headers, PNG previews for the owner.
3. Renderer core (`DrawBuffer` rewrite) + host tests green.
4. Driver (`epd.h`): LUT builder, strip send, temp bands. `lut_test` green.
5. Screens + UI gray + cover pool + sync loan.
6. Converter: images, covers, sleep cover, wordmark; `books` builds; tests
   with real books (`~/books` or EPUBs).
7. `pio run` clean (delete `sdkconfig.esp32c3` if sdkconfig.defaults changed;
   `rm -rf .pio/build/esp32c3` on linker-script-shaped errors). Check
   `riscv32-esp-elf-size` for static RAM vs the docs table.
8. Docs + CLAUDE.md updated in the same commit(s). Commit only when the owner
   asks (global rule).
9. Hand the owner: flash steps, rebake (`books`), and the §5.6 tuning
   checklist.

## 11. Open questions — ask the owner at the start of the build session
- **Q1 (legibility cost of 3 levels in UI):** with only white/gray/black in
  normal updates, (a) **gray secondary text can't be antialiased** (no level
  between gray and white) — it will show hard pixel stairs, against the "no
  hard pixels" goal; (b) **black text on a gray selection fill loses its AA**
  (its gray edge pixels equal the background), so the selected row's text is
  pixel-hard. Options: keep as chosen; or secondary text black (AA) and
  smaller/lighter weight instead of gray; or selection as a gray bar/edge
  marker with the row white. Owner chose gray secondary text + gray fill
  selection *before* this cost was known — confirm.
- **Q2:** Low-battery bar in the reader gray (default here) or black (it's a
  warning)?
- **Q3:** `MainMenu` full refresh (flash) after a sync changed the list: keep,
  or make it a normal update now that the old frame is known?
- **Q4:** Strip height (16 rows proposed) and the cover cache pool size —
  confirm the home cover is the only big cached item (are there other cached
  bitmaps worth keeping?).
- **Q5:** Libron TTF source/version if v0.25 isn't the one fetched.

## 12. Constraints reminder (from CLAUDE.md and the owner's global rules)
- No logging anywhere (temporary measurements must be removed).
- No `std::sort`. No placeholder/empty/error text. Event-driven only (no
  polling/delays/fallback timeouts) — the BUSY wait stays ISR-driven.
- Every `EInkDisplay` entry starts `wakeIfNeeded(); waitWhileBusy();`. SD
  access only with the panel idle (strip replay reads the card for images and
  covers — only between waveforms, i.e. inside `refresh()` before
  MASTER_ACTIVATION or after waiting).
- Never add a write to the page-turn path (SD wear). Settings/index/pos rules
  unchanged.
- No magic/version in any format; layout change = regenerate assets + rebake,
  same commit.
- `WintergreenConfig.h` is the only tunables file; waveform constants are
  hardware properties → local constants in `epd.h`, not config.
- Nix machine: no python3/perl/node; `bun -e`, `jq`, `awk`, or `nix shell`.
  Dev shell via `nix print-dev-env` into the scratchpad when needed. Big
  downloads to `~/.cache`. Never `nixos-rebuild`. Commit/push only when asked.
- Compile check: `pio run` for firmware; host tests as above.
