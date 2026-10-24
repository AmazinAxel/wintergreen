# Bluetooth page-turner

Part of the Wintergreen notes; the entry point is `CLAUDE.md`.

## Bluetooth page-turner

A BLE HID-over-GATT **host** for a clicker, in `platforms/esp32/bluetooth_clicker.h`. Off
by default, off at every boot, and off after any failure — the radio is the largest
current draw this firmware can switch on, tens of mA against ~15 mA idle.

| Build | pio flash | pio RAM |
|---|---|---|
| no BT keys at all | 430,790 | 159,820 |
| BT keys, MAC undefined (**default**) | 434,580 | 160,216 |
| MAC defined | 728,888 | 164,128 |

The sdkconfig keys cost **3.8 KB of flash and 396 bytes of RAM** in a build that will
never use them; the stack itself is ~294 KB of flash plus tens of KB of *heap* while the
radio is on. app0 is 6.4 MB, so none of this is capacity pressure.

**Three sdkconfig keys look wrong and are not.** IDF guards `nimble_hidh.c` — the HID
**host** — with `CONFIG_BT_NIMBLE_HID_SERVICE`, which is the HID *device* service option,
and that option lives in a menu gated on `BT_NIMBLE_GATT_SERVER`, which depends on
`BT_NIMBLE_ROLE_PERIPHERAL`. A central-only device cannot compile the host without
nominally enabling the peripheral role. Nothing ever advertises. Drop any of the three and
the link fails with `undefined reference to esp_ble_hidh_init` — a *link* error naming an
IDF file, with nothing pointing at the config.

Design:

- **It scans before connecting, and must.** Connecting straight to the configured MAC does
  not work, for two reasons. A BLE address does not carry its own *type* (public/random),
  and guessing wrong costs 30 s because `esp_hidh` calls `ble_gap_connect` with that
  timeout and blocks. And HID devices routinely use a **resolvable private address**,
  rotating every ~15 minutes — `(addr[5] & 0xC0) == 0x40` identifies one — so a MAC read
  an hour ago is not that device's address any more. An 8 s active scan yields the address
  *and* type that are true now. The configured MAC is a filter, not a destination; if
  nothing matches, anything advertising HID service UUID 0x1812 is taken instead, which is
  what makes a rotating address work.
- **No reconnect.** A disconnect, a refused connection or one that never completes all drop
  to Disconnected. A radio hunting for a device that is not there is the leak this avoids.
- **No persistence.** Deep-sleep wake is a full boot, so the device always comes up cold.
  With `kAutoSleepMinutes = 1` that means re-enabling after any pause long enough to
  sleep — the deliberate trade, since turning pages resets `inactivity_ms_`.
- **Everything blocking runs on a worker task.** `esp_hidh_dev_open()` blocks for the
  length of a connection attempt and teardown calls block too, so `toggle()` spawns a
  one-shot task and returns. `g_busy` makes a second toggle during a transition a no-op.
- **The HID callback never tears its own stack down.** It sets `g_teardown_request`;
  `wg_clicker::poll()` spawns the teardown task. Deinitialising a stack from inside its own
  event callback deadlocks.
- **NVS is initialised lazily**, in `bringup_stack()` — the bond store is its only user.

Input arrives as **edges only**: `g_clicker_buttons` is merged by
`Esp32InputSource::poll_buttons` the way `g_serial_buttons` is, and nothing sets
`current`, so a clicker press can never look held and drive `HoldRepeat`. Reports are
matched permissively — scan the payload for Left/Right Arrow (0x50/0x4F) or Page Up/Down
(0x4B/0x4E) rather than parsing the report map, since `esp_hidh` has already done
discovery and clickers disagree about layout. A `g_last_key` transition check stops a held
key repeating.

Left advances and right goes back — **the opposite of a presentation remote's
convention**, intentional. Injected as Button2/Button3, "next" and "previous" in the
reader in *both* orientations.

**Four things are required together, and each fails silently on its own.** The first
version had all four wrong and produced one symptom — "Connecting" for a minute, then
"Disconnected":

1. **The MAC must be byte-reversed.** NimBLE's `ble_addr_t.val` is little-endian: `val[5]`
   is the byte written leftmost in a MAC, which is why `ble_gap.c` tests `addr[5] & 0xC0`.
   `esp_hidh_dev_open()` memcpy's the array straight into `ble_addr_t.val`. `parse_mac()`
   fills `out[5 - i]`. Reversed, the stack hunts for an address that does not exist and
   times out looking exactly like a clicker that is switched off.
2. **`CONFIG_BT_NIMBLE_GATTC_AUTO_PAIR=y`.** `esp_hidh`'s NimBLE path never calls
   `ble_gap_security_initiate`, and HID characteristics need an encrypted link. This is
   what makes `ble_gattc` retry a read through pairing when the peer answers
   `INSUFFICIENT_ENC`/`AUTHEN`. Without it the link comes up and reads nothing.
3. **`ble_hs_cfg.sm_*` must be set in `bringup_stack()`.** NimBLE defaults `BLE_SM_BONDING`
   and *both* key-distribution masks to `(0)`, so pairing exchanges no keys and there is no
   bond. The ID key (IRK) is what later lets the stack recognise a rotating address.
4. **The scan**, above.

**Confirmed working on hardware**: connects reliably, pages turn, stays up.

### The failure codes are gone — here is what they found

The row briefly carried numeric stage codes (`Failed 8003`), a free-heap readout, a GAP
listener and an `esp_reset_reason()` display. All **removed** — it was scaffolding, and it
worked. Keep this record instead:

- `8003` / `8011` — sync timed out with 3 KB, then 11 KB, of internal RAM free. This
  established heap as the binding constraint. Note `esp_nimble_enable` **ignores
  `xTaskCreatePinnedToCore`'s return and always reports ESP_OK**, so a host task that could
  not be allocated is indistinguishable from one that started and never synced.
- The stage split proved a *first* failure could wedge the session.
- `rst4` plus a backtrace named the real bug: `std::bad_alloc` in
  `TextLayout::assemble_page`, not in any BLE code.

If a failure needs diagnosing again, the cheapest reconstruction is a `FailStage` enum set
at each bail-out in `bringup_stack()` and shown after "Failed". Do not leave it in.

`ClickerState` is Unavailable / Disconnected / Connecting / Connected. **Every failure
reads as Disconnected**: the row is a switch, not a status console, and each of those
states meant the same thing to the user (press it again).

### The row shows the clicker's own battery

`ESP_HIDH_BATTERY_EVENT` carries the peer's level, so once connected the row reads `Page
Turner: 89%`.

**That event is not enough and waiting for it shows nothing.** `nimble_hidh.c` posts it
*only* from a `notify_rx`, and most clickers notify only on change — so the row sat on
"Connected" for hours. `read_battery_once()` does an explicit `ble_gattc_read_by_uuid` of
the Battery Level characteristic (0x2A19) over the whole handle range, immediately after
connect and before the state goes Connected. One round trip, no discovery of our own,
bounded at 2 s so a clicker that ignores the read cannot hang the connect.

- **Captured once**, then left alone: a percentage that twitches while you read is worse
  than a slightly stale one. Cleared by `radio_off()`.
- **Not every clicker has a battery service.** No answer means the row says "Connected".
- `QuickmenuScreen::update` watches the percentage as well as the state, since a later
  notification can still update it.

**The read doubles as the liveness check.** `ESP_HIDH_OPEN_EVENT` with `status == ESP_OK`
is not sufficient: it has been seen arriving for a link that then delivers no reports,
leaving the row on "Connected" with a clicker whose buttons did nothing.
`read_battery_once()` returns false when the read could not be issued or never completed,
and `connect_task` treats that as a failed connect. A peer that will not answer a GATT
read will not send input reports either. This does **not** reject clickers without a
battery service: those still *answer* ("attribute not found"), so they connect and show
"Connected" with no percentage.

**Never find the connection with `ble_gap_conn_find_by_addr(&g_target)` alone.** That was
the underlying bug. A HID device typically advertises under a resolvable private address,
and once bonding completes the stack resolves it to the peer's *identity* address — so the
connection no longer matches the address the scan saw and the lookup returns `ENOTCONN` on
a live link. `live_conn_handle()` tries the scanned address first and otherwise walks the
handle range matching `peer_ota_addr` or `peer_id_addr`. Handles are controller-assigned,
**not** 0-based indices, so iterating `0..CONFIG_BT_NIMBLE_MAX_CONNECTIONS` does not work.

### Stack sizes are not the defaults, deliberately

`CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` is **6144**, not IDF's 4096, and
`esp_hidh_config_t::event_stack_size` is 6144 rather than 4096. The NimBLE host task runs
GATT discovery, the security manager, *and* — because `ble_store_config` persists bonds —
**NVS writes**, which are stack-hungry. Bonding completes a second or two after a HID
connect, which is precisely when a device that resets shortly after connecting would do it.

### Never use `p->input.data`

`nimble_hidh.c` allocates one block holding the event struct followed by the report
payload, points `input.data` at the payload, posts the block to the event loop — which
**copies** it — and then **frees the original**. The pointer arriving in the callback
refers to freed heap. The copy has the payload at a known offset, so `hidh_event_cb`
recomputes it as `(const uint8_t*)p + sizeof(esp_hidh_event_data_t)`. Same applies to
`feature.data` if that event is ever handled.

### The stack comes up once per boot and is NEVER torn down

`bringup_stack()` runs each init at most once, guarded by its **own** flag. Toggling off
calls `radio_off()`, which closes the link and disables the controller but leaves the
NimBLE host and esp_hidh initialised for the session.

**One flag for the whole function is not enough**: a bringup that reached
`esp_nimble_init()` and then failed at sync left a single end-of-function flag false, so
the next attempt re-ran the one call that cannot be repeated. Per-step flags make a retry
resume rather than restart.

**Do not add a deinit path back.** Three attempts were made; the first two each shipped a
distinct bug:

1. **Unconditional teardown** → a bringup that died at sync still called
   `esp_hidh_deinit()` on a stack whose host had never started, leaving esp_hidh's
   `event_loop_handle` non-null. `esp_hidh_init()` returns `ESP_FAIL` ("Already
   initialised") when it is, so: *first attempt after a boot fails at stage 8, every
   attempt after that fails at stage 6, permanently, until a power cycle.*
2. **Per-stage tracked teardown** → fixed that, then crashed:

   ```
   assert failed: ble_hs_init at ble_hs.c:928
     esp_nimble_init  (nimble_port.c:200)
     wg_clicker::bringup_stack()
   ```

   Line 928 is `SYSINIT_PANIC_ASSERT(rc == 0)` on the `os_mempool_init` of
   `ble_hs_hci_ev_pool`. With `CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC=y` — set here —
   `ble_hs_ctx` and its `hci_os_event_buf` are heap-allocated behind an `if (!ble_hs_ctx)`
   guard, and `esp_nimble_deinit()` does not restore those pointers. A second
   `esp_nimble_init()` builds the pool on freed state and panics. **Cycling the NimBLE
   stack is not supported by this IDF, and it fails by crashing rather than by returning
   an error.**

**The controller is a different matter, and this is the battery lever.**
`esp_bt_controller_init`/`deinit` cannot be cycled, but
**`esp_bt_controller_enable`/`disable` can** — that is the documented pair, and disabling
is what actually stops the radio drawing current. So the flags split:

| Flag | Lifetime |
|---|---|
| `g_ctrl_inited` | once per boot |
| `g_ctrl_enabled` | **cycles** — cleared by `radio_off()` |
| `g_nimble_up` / `g_hidh_up` / `g_host_task_started` | once per boot |
| `g_host_up` | cleared by `radio_off()`, re-waited on each bringup |

`radio_off()` closes the link, waits ~100 ms for the disconnect to reach the peer and the
CLOSE event to land, then disables the controller. Reached from every path: toggling off,
a failed connect, and `poll()` acting on the clicker being switched off mid-book.

`g_host_up` must be re-waited after a re-enable: the controller runs a fresh HCI reset when
it comes back, and no GAP call is legal until the host has synced again. The host task is
started once and never deleted — `esp_nimble_disable()` is a bare `vTaskDelete` and the
host is not restartable. A stage-8 failure is no longer terminal: the retry skips to
`esp_nimble_enable` and waits for sync again.

### Heap is the binding constraint, confirmed twice

The second confirmation names the victim:

```
abort() ... std::bad_alloc
  operator new
  std::vector<LayoutWord>::_M_range_insert
  TextLayout::assemble_page   (TextLayout.cpp:827)
  ReaderScreen::prev_page_    (ReaderScreen.cpp:693)
```

**No BLE frame anywhere in it.** The radio took the internal RAM the reader needed to lay
out a page, and with exceptions disabled `bad_alloc` is an `abort()` — a whole-device
restart mid-read, reported as `rst4`. The failure is not in the clicker code and cannot be
fixed there. Either BLE gets smaller or the reader's peak allocation does.

Measured on hardware: **~13 KB free with the radio up and a book open.** Survivable only
because the allocation *spike* below is gone.

**The reader's own allocation spike.** `build_page_items` grew `PageContent::word_pool` by
`insert()` with **no reserve**. A `std::vector` realloc holds the old and new blocks
simultaneously, so a page of a couple of thousand `LayoutWord` (12 bytes each) transiently
needed ~1.5-2x its final size. That spike is what threw `std::bad_alloc`; the radio only
made the headroom small enough to notice. It now does one exact reservation from a pre-pass
over the same cached paragraphs the main loop uses. `rev_items` / `items` in the collect
passes are reserved to 64 for the same reason. **Reserve before growing anything sized by
page content.** This is the highest-value memory fix in the tree and it is invisible until
the heap is tight.

`TextLayout::kCacheCapacity` is **8**, not 16. Each entry is a laid-out paragraph holding
per-line word vectors. A page spans a handful of paragraphs, so the hit rate is effectively
unchanged; below ~6 a backward turn would thrash, since it crosses a paragraph boundary by
definition.

**What the paragraph cache costs**, measured over every page of four books with
`tools/tests/layout_memory_test.cpp`'s instrumented allocator (worst case, the Odyssey):

| `cache_limit_` | worst page peak | resident (cache + page) |
|---|---|---|
| 8 (no radio) | 25,823 B | 73,002 B |
| 2 (radio up) | 27,097 B | 44,944 B |
| 1 | 28,051 B | 44,480 B |

So the cap `release_ram_for_radio()` applies buys **~28 KB resident for ~1 KB extra peak**.
**Do not lower it to 1**: resident stops improving while the peak keeps rising, because a
single slot thrashes on a page spanning two paragraphs. The switch is automatic, keyed on
`IRuntime::clicker_holds_ram()`, so caching runs at full size whenever no radio is up.

**An oversized paragraph is split by the converter, not handled on the device.** The
Odyssey's chapter 2 held a 3,672-byte paragraph that laid out to 116 lines / 683 words =
14,640 bytes resident, inside a page peak of 45,712 B, against ~13 KB free with the clicker
connected. Reading that page aborted the device reproducibly; the allocation that failed
was a **256-byte** `reserve`, i.e. the heap was gone.

A paragraph is the device's indivisible layout unit, so the fix is `kSplitParagraphBytes`
in `WgbFormat.h` and the two splitters in `WgbWriter`:

- `write_split_text_paragraph_` cuts at `<br>` runs where the paragraph has them, and
  **recurses into the sentence splitter** for any chunk still oversized.
- `write_sentence_split_paragraph_` handles running prose with no `<br>`. It cuts only
  after `.`/`!`/`?` (allowing a trailing quote or bracket) followed by whitespace, so the
  break lands where a line break could already fall. **Continuation chunks clear `indent`**
  and carry `kWgbSpacingDefault`, so no fresh first-line indent and no extra gap. A
  paragraph with no sentence boundary in range is left whole rather than cut mid-clause —
  visible breakage is worse than a large paragraph.

`kSplitParagraphBytes` is **700**, not the 1,500 first tried. The binding cost is not one
paragraph — it is `WgbChapterSource`'s **32-slot window**, which holds the raw text of 32
paragraphs at once and so multiplies the *average* paragraph size by 32. On the Odyssey
that window alone measured **26.8 KB**, more than the page and the paragraph cache
combined. The window cannot shrink: a page legitimately spans up to 24 paragraphs
(measured), and cutting it corrupts displayed text. Lowering the average is the only lever.

Verified invisible: concatenating all text from every paragraph, before and after, is
**byte-identical** on all four books. Page counts move by a handful (514 → 516, 629 → 639)
because a split adds a paragraph boundary, a legal break the layout was already free to
take.

| | before | after |
|---|---|---|
| per-page layout peak (Odyssey) | 25,823 B | **13,157 B** |
| total chapter + page peak, cache=2 (Odyssey) | 47,434 B | **32,890 B** |
| largest paragraph (Odyssey) | 3,672 B | 1,404 B |
| paragraphs over the limit | 25 | 5 |

The five remaining have no sentence boundary inside the window.

**Three bugs made the first attempt look like it had done nothing**, all producing the same
symptom — a re-converted book still holding oversized paragraphs:

- `WgbConverter::write_split_paragraph` called `write_text_paragraph` **directly**,
  bypassing `write_paragraph` and the size split entirely. It bounds the *serialized* size
  (run headers dominate in annotated books); the text-size bound lives one level up.
- `write_sentence_split_paragraph_` never flushed when the chunk was already full, so
  `room` went to 0, `sentence_cut_` found nothing, and the whole run was appended to an
  oversized chunk.
- Even with room > 0, a *scrap* of room is as bad as none: chapter 22's paragraph 31 is
  runs of 658 + 5 + 848 bytes, and the third could not be cut inside the 37 bytes left
  over. `sentence_cut_fits_` now tests whether a cut is possible before the run is
  considered, and flushes first if not.

Two consequences worth knowing:

- **`TOC para_index` counts *written* paragraphs, not source ones.** `EpubParser` numbers
  source paragraphs, so once a chapter splits anything the two diverge and every later
  chapter jump lands short. `WgbConverter`'s `id_sink` reads
  `WgbWriter::chapter_paragraph_count()` instead.
- **The magic stays `WGB2`.** The on-disk layout is unchanged — only paragraph granularity
  differs — so old files still open, keeping their oversized paragraphs and staying exposed
  to the abort until re-converted.

Two allocation bugs in `layout_para_lines` are also fixed, and they matter for any book:

- `lines` is reserved from the paragraph's byte count and the font's own `char_width('n')`,
  not a fixed 12. The old doubling chain 12 → 24 → 48 → 96 held both arrays *and*
  move-constructed every heap-owning `LayoutLine` per step. A guessed 6 px/char undershot
  on 930 of the Odyssey's paragraphs, so the estimate uses real metrics; it now undershoots
  twice in ~8,800 paragraphs.
- Every finished line's word vector is shrunk to its real size (in `flush_line`, which
  pairs with all four push sites), and `lines` itself is shrunk before it is returned.
  `reserve(16)` per line is held for the paragraph's whole life otherwise: 21.5 KB to store
  10.9 KB of words.

On their own those took the Odyssey's worst page from 45,712 B to 25,823 B — short of what
BLE leaves free, which is why the converter split is the actual fix. A per-paragraph **line
cap** was considered and **rejected**: it would silently truncate text.

### The OOM rescue, and why it needs its own reserve

`main.cpp` installs a `std::set_new_handler`. With exceptions disabled a `std::bad_alloc`
is an unconditional `abort()` — IDF's `__wrap___cxa_throw` is literally `abort()` — so the
handler is the *only* place an allocation failure can be intercepted. A guard checked
before allocating can always be overtaken by fragmentation between the check and the `new`;
the handler cannot.

**It must have something left to free, and that was the bug.** The handler originally
released the spare framebuffer and the book index. Both are already gone whenever the
clicker is connected — `release_ram_for_radio()` drops the index and `wg_clicker` drops the
spare — so the handler ran, freed **nothing**, and the abort proceeded. Every
clicker-connected crash had a live handler that could not help.

`g_oom_reserve` fixes that: **28 KB claimed at the top of `app_main` before anything else
allocates**, never read, existing solely to be freed by the handler. Sized from the worst
*cold* page layout measured over four books — 24,066 B on the Odyssey, with an emptied
paragraph cache, the state the reader's own guards force under pressure. Claiming it first
also means it is one clean extent.

Three properties that matter:

- **The handler frees the reserve first**, the only block guaranteed to still hold memory.
- **The reserve is re-claimed once per frame**, but only when the largest free block
  exceeds `kOomReserveBytes + 16 KB` — taking it back must not cause the shortage it exists
  to prevent. Without the re-claim the rope is single-use. The re-arm of the handler is
  *not* gated on the reserve being spent: the handler disarms itself on any rescue, and its
  other two levers can fire while the reserve is still held.
- **The reserve must be released before `xTaskCreate`, not inside the worker.** It comes
  out of the same single ~50 KB internal DRAM pool the BT controller needs a *contiguous*
  block from, and holding it broke the clicker outright. Releasing it in `bringup_stack()`
  was not enough — that runs *inside* the worker, so it cannot help the `xTaskCreate` that
  allocates the worker's own 6 KB stack. The release is in `toggle()`, immediately before
  the create.

  The symptom is worth recognising: `toggle()` sets Connecting, the create fails, and the
  state goes back to Disconnected **within the same call**, so the row never repaints as
  "Connecting" and the press looks like it did nothing. A row that stays on Disconnected
  with no intermediate state is a failed task creation, not a failed connect.

  `wg_clicker::holds_ram()` includes `g_busy` so the frame loop cannot re-claim the reserve
  in the window between `toggle()` freeing it and the worker allocating. `wg_sync::start()`
  releases it for the same reason.

**Anything new that reserves internal RAM up front inherits this.**

### The heap fails by fragmenting, not by filling

Every abort in this reader has been a small allocation failing with plenty of total free
heap: 256 bytes, 1,212 bytes, 1,416 bytes. Two things follow, both got wrong first.

**Guard on `largest_free_block_bytes()`, never `free_memory_bytes()`.**
`heap_caps_get_free_size` sums every free byte across a fragmented heap, and IDF's own
header warns that a single block of that size probably cannot be allocated. A 24 KB
*total-free* guard passed and the layout behind it aborted on a 1,416-byte request, on an
ordinary forward page turn. `heap_caps_get_largest_free_block` answers the question the
guard is actually asking. The number to clear is the largest *single* allocation one layout
makes — measured at **5,808 B** worst case — hence `kLayoutMinBlockBytes` = 12 KB and
`kPrerenderMinBlockBytes` = 20 KB.

**Allocation churn is worse than allocation size.** `shrink_to_fit()` on each laid-out line
and on the line vector reclaimed ~5 KB of slack per page and was removed again: it
reallocates (allocate exact, copy, free old), adding ~80 allocate/free pairs per page and
taking the page from **114 allocations to 197**. Many short-lived blocks of varying size is
the definition of heap churn, and churn produces the fragmentation above. 5 KB of headroom
is not worth 40% more traffic on a heap that fails this way. Peak went 13.2 KB → 18.6 KB,
both inside budget. Same reasoning is why `split_words` fills a caller-owned buffer reused
across every run of a paragraph instead of returning a fresh vector per run.

**A runtime guard backs this up.** `ReaderScreen::render_page_` checks
`largest_free_block_bytes()` against `kLayoutMinBlockBytes` immediately before laying out
and, if short, drops the page cache, the pre-drawn page and the paragraph cache and caps
the cache at one slot. Exceptions are off, so an allocation failure inside layout is an
`abort()` with nothing to catch — the check has to happen before allocating. The cost is a
slower page turn; the page still renders. It deliberately does **not** release
`DrawBuffer`'s spare: `render_page_` can be reached from inside an offscreen draw, and
freeing the buffer being drawn into is worse than the abort it would prevent.

**Every path that lays out needs the guard.** There are three, and missing one is not
theoretical — `prev_page_` was left unguarded and scrolling backwards with the clicker
connected aborted on a 1,212-byte allocation inside `split_words`:

| Path | Guard |
|---|---|
| `render_page_` | two stages — `kLayoutShedBlockBytes` (20 KB) drops the page cache and pre-drawn page; `kLayoutMinBlockBytes` (12 KB) also drops the paragraph cache |
| `prerender_next_page_` | `kPrerenderMinBlockBytes` (20 KB) — higher, because the *real* page after it still has to fit |
| `prev_page_` | `kLayoutMinBlockBytes`, and it also skips the forward-resync block entirely |

`prev_page_` is the hungriest: one `layout_backward`, then the resync does another plus up
to three `layout_end` calls and a final `layout()`. When the heap is tight it drops caches
*and* takes `layout_backward`'s answer unrefined, costing the occasional one-line offset
rather than a reboot.

**A forward turn holds two pages at once**, and that is the number to budget against.
`prerender_next_page_` lays out page N+1 while page N is still resident: measured worst case
**25 KB** across four books against ~13 KB for a single page. Two consequences, both bugs:

- Its guard at the top tests the heap *before* the speculative layout exists, so it says
  nothing about whether the subsequent *draw* will fit. It now re-checks after the layout
  and bails, leaving the layout cached — the next turn is a normal render.
- The draw calls `render_page_`, which runs its own guard. Left alone that guard would drop
  the very page cache the prerender just filled, forcing an immediate re-layout with *less*
  room than the check was made against. `in_prerender_draw_` suppresses it for the nested
  call only.

**`DrawBuffer`'s spare is on the heap, not in BSS.** Static RAM went 164,184 → 116,168 with
that one change. It is still allocated at construction and kept for the session — the point
is that the 48 KB sits in the pool the reader and radio draw from.

**It is deliberately never released, and neither are the reader's page caches.** Both were
wired to free on connect and that was wrong: they are what makes a page turn a memcpy
instead of a layout, and a clicker exists to turn pages. Only the book index is handed
back, because being without it while a book is open costs nothing.

**The index is handed back before the radio starts.** `Failed 8011` — the host task could
not be created with 11 KB free — showed the two do not fit alongside a resident index.
`QuickmenuScreen::on_select` calls `Application::release_ram_for_radio()` **synchronously,
immediately before** `toggle_clicker()`, freeing ~30 KB. It must be synchronous: bringup
allocates the moment the worker spawns, so a next-frame release is too late.

`BookIndex::release_memory()` swaps the vector rather than clearing it (a cleared vector
keeps its capacity) and refuses when `dirty_`, since a reload cannot recover unsaved
mutations. An empty index is the "never loaded" state everywhere, so nothing truncates the
file.

BLE's allocations all come from `MALLOC_CAP_INTERNAL`
(`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL`), on a build already holding ~160 KB statically
out of 320 KB. Not a stack-size problem, and raising stacks makes it worse. Every `BT_*`
buffer key in `sdkconfig.defaults` is sized down from IDF's defaults, which assume a
multi-peripheral hub rather than one clicker sending 8-byte reports — each commented in
place. `BT_CTRL_BLE_MAX_ACT` (6 → 2) is the largest single saving.

`bringup_stack()` **refuses to start below `kMinHeapKb` (24)**, turning the worst failure
mode — half initialise, time out at sync, leave a session that cannot retry — into a clean
refusal. It is a floor for the hopeless case, not an estimate of what BLE needs: it shipped
at 64, above what the device actually had free, and refused every attempt on hardware that
had been connecting fine.

**Any early bail-out from `bringup_stack()` reaches `radio_off()`**, via `connect_task`'s
`if (!ok)`. That is why `radio_off()` guards `ble_gap_disc_cancel()` behind `g_host_up`:
calling it before `esp_nimble_init()` dereferences a null `ble_hs_ctx` inside
`ble_hs_is_enabled()` and takes a **load fault, not an error return**. The heap check
introduced exactly this — the device rebooted the instant the row was selected, before
"Connecting" was drawn. Any new pre-NimBLE failure stage inherits the same hazard.
