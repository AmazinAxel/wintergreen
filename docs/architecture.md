# Architecture and screens

Part of the Wintergreen notes; the entry point is `CLAUDE.md`.

## Architecture

`Application` owns every screen as a by-value member and a `ScreenManager` stack.
Screens never push directly; they call `push_screen`/`replace_screen`/`pop_screen`,
which queue a `ScreenId` applied after `update()` returns. `screen_for_(ScreenId)` maps
the enum to the member.

Stack: `HomeScreen` → `MainMenu` (all books) → `ReaderScreen` → `QuickmenuScreen`. Only
these four. Back on the home screen opens the book list.

**Opening a book from the list *replaces* the list.** `MainMenu::on_select` calls
`replace_screen(Reader)`, so the stack is `HomeScreen → ReaderScreen` and Back from the
reader lands on the home carousel rather than back in the list. The carousel is sorted
most-recently-opened first and `record_book_opened()` has already bumped this book to
the top. It also drops the list's `entries_` immediately.

`ScreenManager::replace()` is **not** `pop()` + `push()`: `pop()` resumes whatever is
underneath, so the old sequence had `HomeScreen` rebuild its carousel, re-read the index
and decode a cover — all discarded one line later when the push paused it again.

**The root screen depends on whether the library is empty.** `Application::start()` loads
the index and pushes `MainMenu` instead of `HomeScreen` when there are no books: an empty
carousel has nothing to show, while the list is where Sync lives. Two consequences:

- `MainMenu::on_back()` does nothing when `app_->at_root()` — no home screen underneath.
- `MainMenu::on_select()` *pushes* the reader at root instead of replacing, so Back from
  a book returns to the list rather than emptying the stack.

`ScreenManager::pop()` refuses to remove the last screen. An empty stack makes `top()`
null and `Application::update` then silently stops processing input —
indistinguishable from a freeze.

**The carousel falls back to the library when nothing has been opened.**
`HomeScreen::on_start()` picks the five most recently opened; if none have been (a fresh
card, or wiped settings) it shows the alphabetically first books instead, ordered with
the shared `ci_less()` so "first" means the same thing as in the book list's never-opened
group. A carousel with nothing at all draws only the battery.

### HomeScreen

A **carousel of the five most recently opened books**: battery percentage top right, one
large cover inside a 2 px frame held 10 px clear of it, title (up to two lines) and
author centred beneath, and a row of diamonds for position (the selected one carries a
second, larger outline diamond). Only title lines actually drawn advance the cursor, so
the author never floats under a reserved blank line; the diamonds are anchored to the
bottom of the panel.

Up/Down are **swapped** here (`swap_updown_`, applied to the state forwarded to the base
class): the carousel runs left-to-right, so the top of the side rocker and the bottom
front key advance it. Select opens the book, **Back opens the full book list** — which is
why there is no Recent Books screen. It derives from `ListMenuScreen` purely for the
item/selection/navigation machinery; every pixel comes from its own `draw_all_`.

- The selected cover is loaded on demand **from inside `draw_all_`** (hence the `mutable`
  cover members). Loading from `update()` would draw one frame with the previous cover
  and then redraw — two e-ink refreshes per press.
- **The cover lives in the spare framebuffer, not on the heap** (`claim_spare_cache(this)`,
  trusted while `spare_cache_valid(cover_gen_)`). A heap copy was ~26 KB resident for as
  long as the home screen was. The cache owner is part of the claim, so the book list's
  thumbnails and this cover invalidate each other instead of trusting overwritten bytes.
  The spare is never released (radios included, since 2026-11-11), so the cover shows
  with the clicker connected and through a sync. There is still exactly **one** cover —
  a second, preloaded one crashed the device in the layout era; reopened 2026-11-12 and
  waiting on heap numbers (two covers do not fit the 48,000 B spare; it needs its own
  fixed 25 KB buffer). See `docs/history.md` "Still open". `cover_slot_` is what stops the card being re-read every frame,
  including for a book with no cover file: no cover with a valid `cover_slot_` is a real
  state, not a miss. A cover that was loaded and then lost its spare is reloaded.
- `on_start()` clears `cover_slot_` because it rebuilds `slots_` from the index — a sync,
  a sync repoints every index without necessarily changing one.
- The cover is **`cover_home.bin`**, rendered by the converter to fit the box
  (`kHomeCoverW` × `kHomeCoverH` = 424x486 in `content/CoverPaths.h`) with its blank top
  and bottom rows already trimmed (`Book::write_cover_bin`), so a load is a header and
  one `fread` into the spare. **There is no scaler on the device** and no fallback
  file: a missing or oversized cover draws the placeholder frame. `cover_sleep.bin` is
  not a substitute — it is composed for the panel with bars baked in. The two constants
  mirror `draw_all_`'s layout maths; if the layout moves they drift and every cover
  shows the placeholder until the library is re-converted.
- The cover read calls `wait_panel_idle()` first: the card shares SPI2 with the panel.

### QuickmenuScreen

**Closes itself when the clicker connects.** `update()` watches `clicker_state()`; a
Connecting → Connected change while the menu is up calls `on_back()`, returning to the
book (snapshot fast path).

A flat list: header block, Font Size, Orientation, Clicker (only when a MAC is
configured), then the chapter grid. There is no separate Chapters page — selecting a
chapter sets `pending_page()` (the chapter's baked start page in the current layout) and
pops one screen; `ReaderScreen::resume()` consumes it.

**Chapter grid.** Two columns of boxed numbers (`1`, `2`, ...), nothing else; the header's
chapter line reads "Chapter N of M". Each chapter is still one list item, in grid order, so
the inherited linear navigation is the grid's: down / front Button2 steps right, then wraps
to the next row's left cell; up reverses. Every cell has a 1 px box, the selected one a
2 px frame (`kCellSelW`, same as the home cover frame, shared `draw_outline()` in
`ListMenuScreen.h`), never an inverted fill — fewer pixels flip per move, so less ghosting.
Settings rows keep the inverted selection. Scrolling is item-indexed (base class), but the
grid always draws and counts whole rows from the row holding `scroll_offset()`
(`first_grid_row_()`); `centred_offset_` and `draw_all_` must agree on that.

**Item 0 *is* the header block** — book title, chapter and their two percentages.
Selecting it pops back to the book. It is **not** the default selection: a power-button
tap opens on the first setting row (the Page Turner when a clicker is configured), the
front select key on the chapter being read (`set_open_on_chapter()`, from
`Application::confirm_from_power()`). `refresh_items_()` bypasses `start()` and keeps the
cursor. It is a real list item rather than a special
case, so navigation, wrap-around and `on_select` dispatch work without changes. What it
is *not* is a row: `draw_all_` paints it at the top and inverts the whole block when
`selected() == kIdxBack`, and the list loop starts at `kIdxBack + 1`. Two places must
agree that it costs no height, or the last chapter becomes unreachable:
`centred_offset_` gives index `kIdxBack` no line, and `draw_all_`'s `total_h`
starts from `(settings_end - 1) * row_h_()`.

Its header is drawn entirely by its own `draw_all_` — the base class's card header is
unused, so `title_`/`subtitle_` are left empty. Book title, current chapter, rule; there
is **no battery** (it is an overlay on the book, and the reader underneath shows none).
Both title and chapter are word wrapped (`wrap_`, into spans of the space-collapsed
title: no allocation per open; "..." drawn at the line's end cursor, identical because the
UI fonts have no kerning) and each carries its
percentage right-aligned on its **first** line, set in the *same font as the text beside
it* so the pair reads as one line. The chapter is a size down (`chapter_font_()` =
`section_font_`). `header_h_()` measures that block from the wrapped lines computed in
`on_start()`, and `centred_offset_()` calls it — the two used to re-derive the height
independently, which is how scroll bugs get in.

A hairline divides settings from chapters, drawn in a `kSeparatorH` gap of its own. Row height is `row_h_()` = the list font's `y_advance()` + padding, not a
constant: the list is set in the 32 px header face and the old fixed 28 px row was
shorter than a single line.

The book title comes from `ReaderScreen::display_title_()`, not `wgb_.metadata().title`:
a converted EPUB with no usable metadata title yields the literal string `"none"`, and
`BookIndex` already substitutes the folder name. Without the same substitution here the
quick menu said "none" for a book the home screen named correctly.

### MainMenu

**Row 0 of the book list is a `Sync` action, not a book** — when sync is compiled in
(`has_sync_`, from `sync_state() != Unavailable` at `start()`; without `WG_WIFI_SYNC` the
row would be a button that does nothing, so it is absent and books start at row 0). Row 1
is its hairline (pushed into `separators_` like any other), and books start at row 2 — so
`entries_index_for()` subtracts `sync_rows_()` *on top of* the separator shift, and
`visual_for_entries()` skips both kinds of non-book row. Every derived separator position
in `populate_list_()` carries the same `first_book` offset (2 or 0); getting that wrong
shows up as the wrong book opening, not as a drawing glitch.

**One repaint at the end of a sync, with everything final** (`MainMenu::update`). The
worker bumps the index generation just before it sets its end state, so the list used to
paint the new books under "Syncing" and then refresh again for the label. While the state
is Working, generation changes wait; the frame that sees the end state repopulates and
draws books plus label together (full refresh if books changed, else partial). It also
keeps the UI thread from reading the index while the worker is still mutating it.

Entries reference `BookIndex`'s pool for path, title and author (`path_of_()`); only hidden
books own strings. A `std::string` path per book was up to 250 allocations per list build.

Titles are drawn whole: the old `"Label: Value"` split (from long-gone settings screens)
also cut book titles at their first `": "` whenever the row had an author.

`run_sync_()` calls `IRuntime::start_sync()` and returns immediately. Its label reports
progress (`Sync` / `Syncing...` / `Synced`), read from `sync_shown_` rather than the
runtime because `get_item_label` is called per row draw. `MainMenu::update()` repaints
when that state changes: the index `generation()` check above it only covers books
*arriving*, so a sync that finds nothing new would leave the row on "Syncing..." forever.
Idle and Failed both read `Sync`.

`select_first_book_()` moves the cursor off Sync onto the first book when the screen
opens. Called from `on_start()` and from `update()`'s scan branch — deliberately **not**
from `populate_list_()`, which also runs on an in-place refresh (a sync) where
moving the cursor under the user would be wrong. With no books there is no hairline and
Sync is the only row.

**Hidden books are a mode of `MainMenu`, not a screen.** A 1 s back long-press **on the
home screen** opens the list with the books under `<books_dir>/.hidden/` at the top, above
their own hairline (`set_show_hidden()` before the push). That gesture is the only way to
see them, and `MainMenu::stop()` clears the flag.

**The hold fires while the button is still down**, the moment `kHiddenHoldMs` elapses —
waiting for the release made the device look dead for the length of the hold. Safe *here*
because presses are rising edges only, so `MainMenu` receives no Button0 event from a
button already down when it started, and `back_consumed_` makes the eventual release a
no-op. The plain book list still opens on release, since a tap cannot be distinguished
any earlier.

The gesture lives on the home screen rather than in the list because the *tap* resolves
on **release**, and only a release-time push lands on a screen whose buttons are up. It
was first written in `MainMenu`, which pushed on the *press*: the pushed screen started
with Button0 still down, its own release handler fired, and the list vanished the moment
the user let go. `HomeScreen` also swallows the first hold after every `start()`
(`back_ignore_`), because `MainMenu` pops on the back press and hands the home screen a
button that is still down. **Any screen pushing another from a button press inherits
this problem.**

Four things the hidden list depends on:

- `.hidden` books are **not in `BookIndex`** (its scan skips dot-directories), so
  `MainMenu::scan_hidden_()` walks the folder itself and takes title/author from
  `.hidden/.wintergreen-index`, opening only WGBs not in it (rewritten when the hidden
  set changed). Those entries carry owned `title_own`/`author_own` strings rather than
  `StringRef`s, because there is no pool entry to reference. The scan runs only when the
  gesture first asks for it.
- `on_select` skips `record_book_opened()` for a hidden entry — a hidden book must never
  reach the recents carousel.
- `separators_` holds **visual** indices, so the hidden divider shifts the
  recently-opened/never-opened one below it by a row. `populate_list_()` inserts the
  hidden group after sorting, so it keeps its own alphabetical order.
- Hidden books are `.hidden/<book>/book.wgb` like any other. The old `HiddenBooksMenu`
  scanned for `*.epub`, which stopped matching anything the moment EPUB support was
  removed.

### ListMenuScreen

**`Button::Undo`** (`Input.h`, index 7, press-only, never in `current`) is injected only
by the clicker's hold (`docs/bluetooth.md`): it means "revert the step the last press
made, without drawing; an action follows". Here: `undo_selected_` (cursor before the last
press move) is restored. In `ReaderScreen`: `last_step_` (+1/-1 of the last turn that
moved, 0 if none, so a press at the book's first page undoes nothing) is reverted with
`prev_page_()`/`next_page_()` and `pixels_stale_` stops `pause()` snapshotting the
page the glass still shows; a turn still pending in the same batch is just cancelled.
Screens that do not handle it ignore it.

**Back no longer flushes a pending move before `on_back()`.** It used to draw + refresh
first "in case on_back stays", which cost a whole refresh before leaving the screen when
an Undo preceded it. Now `on_back()` runs first; if it stays, `moved` still draws.

Base class for every list screen: item storage, navigation, hold-repeat, scrolling and
the battery header. Its own draw pass is **one layout**, the book list's; `HomeScreen` and
`QuickmenuScreen` override `draw_all_` and draw everything themselves. The other layouts
it once carried — the `plain_list_` centred header, the book-details card selected by a
non-empty `subtitle_`, centred single-line rows, `draw_bottom_`'s footer — had no screen
left using them and are gone, along with the 14 px UI font only they used.

- Two-line rows (title + subtitle, full-width divider, right-hand column). The right
  column is the reading percentage and sits on the
  **subtitle** baseline, bottom right beside the author, so the title gets the full row
  width. The **last** row draws no divider, and neither does a row whose next row is a
  section hairline, or the two rules land a few pixels apart and read as a double line.
  MainMenu adds a **cover thumbnail** at the far right of each book row (`prepare_thumbs`
  / `get_item_thumb`) and book rows reserve **two title lines** (`detail_title_lines_`;
  the title wraps at a word, a one-line title leaves the second blank) so the cover gets
  a 76x115 box. The file is `cover_list.bin`, rendered by the converter to fit
  `kListCoverW`x`kListCoverH`, so a load is a header and one `fread` straight into the
  slot. Indexed books read their slot from the thumbnail pack (`.wintergreen-thumbs`,
  `BookIndex.h`), opened once per draw and closed by `end_thumbs()`; hidden books and
  any slot whose key does not match fall back to the book's own file. **The device has no cover scaler** — a book without the file draws an empty
  frame. Covers load **during the draw that first shows the row**, so a page and its
  covers arrive in one refresh; nothing is read ahead (a prefetch of rows that might
  never be shown was built and removed as wasted card reads and CPU). The cache lives
  in the spare framebuffer (`DrawBuffer::claim_spare_cache`, validity by
  `spare_cache_gen()`): no allocation, up to 40 slots, keyed by path, surviving list
  visits until the reader or a radio claims the spare — with a radio up
  the column is absent. The first version used its own `malloc` gated on 40 KB of
  contiguous heap and showed nothing on hardware: the heap is too fragmented for that
  gate. A load re-checks the spare after its last allocation, because the OOM handler
  releases it from inside any allocation.

**There is one separator, defined once.** `kSeparatorH` (14 px) and `draw_separator_()`
are the only hairline geometry in the tree, and every list uses them, including
`QuickmenuScreen`, which draws its own list but inherits both. Before that each site
carried its own `kSepH` — 8, 12 and 14.

Row height is per-row, not per-list: `wintergreen_slot_h_(index)` returns the one-line
height where `is_single_line_row(index)` is true (MainMenu's Sync action) and the
two-line height otherwise. `centred_offset_` asks per index too, so scrolling
stays consistent with what is drawn.

No screen draws button labels, nav-arrow glyphs or a battery icon: the labelled `Back |
Select` / `Down | Up` boxes and the ◀▶▲▼ hint row were removed. Don't add them back —
the buttons are fixed and the panel is small. Nor any empty-state text: an empty list is
blank.

**The header is battery-only.** No wordmark on any screen, no rule under the header. Its
height is `header_h_()` = `battery_row_h_() + kHeaderBottomGap`, used by both the draw
and the scroll maths. It used to be measured off the 41 px header face while the
percentage is drawn in `section_font_` (31 px), so the list started 10 px too low.

Every screen draws the percentage through `draw_battery_`, and `HomeScreen` — which
otherwise shares no drawing code — calls it too, sizing its own header from
`battery_row_h_()`. The position is a property of the base class rather than of each
screen on purpose: when the carousel had its own copy of the maths it drifted by a few
pixels in both axes, visible as a jump when moving between screens. Don't reintroduce a
per-screen `W - pad - pw`.

Drawing is `draw_battery_` → `draw_list_`. `centred_offset_` and `draw_list_` both
measure rows through `wintergreen_slot_h_`; keep it that way, divergence causes
scroll bugs.

### Sorting — never use std::sort here

`std::sort` and `std::stable_sort` are templates on both the iterator *and* the
comparator, so every call site gets its own copy of introsort, insertion sort, `__rotate`
and `__merge_adaptive`. Three call sites over two element types cost **~34 KB of flash**
— more than every screen's drawing code put together — to order lists that are at most
`MAX_BOOKS` (250) long and built once per screen entry. They are gone:

- `MainMenu` has one file-static `stable_sort_indices_()`: a bottom-up merge sort over a
  `uint16_t` permutation, with the comparator as a **plain function pointer** so there is
  exactly one copy in the image no matter how many callers appear. `iota_indices_()`
  builds the identity permutation and `apply_permutation_()` reorders afterwards. Sorting
  indices rather than elements also means `BookEntry`'s four `std::string`s move once
  instead of O(n log n) times.
- `HomeScreen::on_start` does not sort at all. It runs a five-pass top-K selection over
  `BookIndex::entries()`, so it never materialises path/title/author for books it is
  about to discard.

If you need another ordering, reuse `stable_sort_indices_` (move it to a shared header if
a second file needs it). Do not reach for `<algorithm>`.
