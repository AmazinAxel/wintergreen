# Bluetooth page-turner

Part of the Wintergreen notes; the entry point is `CLAUDE.md`.

## Bluetooth page-turner

A BLE HID-over-GATT **host** for a clicker, in `platforms/esp32/bluetooth_clicker.h`, on
raw NimBLE (no `esp_hidh`, since 2026-11-11). Off at every boot and off after any
failure — the radio is the largest current draw this firmware can switch on, tens of mA
against ~15 mA idle. Compiled out entirely unless `WG_BLUETOOTH_PAGE_TURNER` is defined.

NimBLE is central + observer only: `ROLE_PERIPHERAL`, `ROLE_BROADCASTER` and
`GATT_SERVER` are explicitly `=n` (they default on). They were forced on only because
IDF gates `nimble_hidh.c` behind `CONFIG_BT_NIMBLE_HID_SERVICE`, a peripheral-menu
option; with `esp_hidh` gone that whole chain went (image 1,218,158 → 1,199,358 B).

### Connect sequence (all on the one-shot `wg_bt_up` task)

1. `bringup_stack()` (controller init/enable, NimBLE init once, wait for host sync).
2. **Bonded: no scan.** `bonded_target_()` takes the bond's identity address (configured
   MAC preferred when two bonds exist) as `BLE_ADDR_PUBLIC_ID` / `RANDOM_ID`, and
   `ble_gap_connect` goes straight to it with `kScanMs` as the timeout. The controller
   matches the clicker's rotating private address through its resolving list (NimBLE
   loads every bond's IRK at sync, `ble_hs_misc_restore_irks`, and enables resolution).
   Any failure except a toggle falls through to the scan.
3. Unbonded (or the direct connect failed): active scan, `kScanMs` = 5 s, 100% duty.
   Configured MAC wins and ends the scan; else the first device advertising HID 0x1812 is
   the fallback, taken only when the scan ends. Then `ble_gap_connect` to the address *and
   type* the scan saw, `kConnectMs` = 10 s.
4. Both connects use `conn_params_()` (interval 7.5-15 ms, see below) and callback
   `conn_cb`.
5. `encrypt_()`: `ble_gap_security_initiate` (bond → key refresh, none → Just Works).
6. `enable_key_notifications()`: **from the NVS handle cache when this clicker was seen
   before** (below), else discover HID services → descriptors; then write `01 00` to every
   CCCD (always, a few tens of ms at this interval).
7. `read_battery_once()`: read 0x2A19 over the whole handle range; liveness check.
8. Connected. `ui_wake()` so the row repaints at once; an open quick menu closes itself
   (`QuickmenuScreen::update`: Connecting → Connected calls `on_back()`), since the next
   thing after pairing a page-turner is reading.

Measured 2026-11-11 (ms since toggle, temporary quick-menu rows):

| | scan | link | encrypted | CCCDs | Connected |
|---|---|---|---|---|---|
| esp_hidh, toggle | 53 | — | — | 1773 | 1794 |
| esp_hidh, fresh boot | **fails at 78 ms, every time** | | | | |
| raw NimBLE, fresh boot | 170 | 407 | 657 | 1447 | 1470 |
| raw NimBLE, toggle | 1060 | 1268 | 1518 | 2311 | 2334 |

Scan time depends on when the clicker advertises. Post-scan, link → Connected went
~1741 → ~1280 ms. Descriptor discovery + CCCD writes (~800 ms) was then the largest part,
which is what the handle cache, the direct connect and the short interval remove.
`wg_bt_up` peak stack 1,736 B (4,408 B free of 6,144) on that build; it is **5,120** now
because the cache adds NVS reads/writes on this task (not re-measured).

Owner hardware test of the esp_hidh-free build (2026-11-11): connects first try on every
fresh boot (esp_hidh never did), toggle off/on, tap and hold in reader and menus, clicker
switched off → Disconnected, press during Connecting → Disconnected at once. "Never once
failed or glitched." After the direct connect + cache + interval build: "works great",
page turns feel faster. **Not measured:** the per-stage times of the direct/cached path
(the `D path` row was never read off), and the clicker-paired-elsewhere-then-back case.

### Handle cache (NVS `wg_bt` / `hid`)

`HidCache` = peer identity address, HID service ranges, CCCD handles (~56 B), written once
after a full discovery, keyed by `desc.peer_id_addr`. A reconnect loads it and goes
straight to the CCCD writes. Any refused write (handles moved, a different device) erases
the key and rediscovers once. The CCCD writes stay on every connect: a clicker that paired
elsewhere forgot its subscriptions.

### Connection interval, and the L2CAP crash

`conn_params_()`: interval **7.5-15 ms** (`kItvlMin` 6 / `kItvlMax` 12, units 1.25 ms),
latency 0 from our side, supervision 4 s, initiator scan 100% duty. A key report waits at
most one interval for its connection event, so this is the clicker's whole share of
page-turn latency, and every GATT round trip at connect costs about one interval. NimBLE's
default was 30-50 ms. Owner's call: lowest latency over radio current while connected.

`conn_cb` accepts a clicker's parameter-update request but caps `itvl_max` at
`kItvlMax` (and `itvl_min` to match); its slave latency and supervision timeout stay
(slave latency only lets an idle peripheral skip events; a key still goes out at the next).

**The two update events fill different fields, and getting it wrong rebooted the device on
every successful connect.** `BLE_GAP_EVENT_CONN_UPDATE_REQ` (LL procedure) hands the app
`self_params` to edit. `BLE_GAP_EVENT_L2CAP_UPDATE_REQ` leaves `self_params` **null** and
applies `peer_params` itself after the callback returns 0
(`ble_l2cap_sig.c`: `ble_gap_rx_l2cap_update_req` → `ble_gap_update_params(conn, &params)`),
a non-const object behind a const pointer, so the clamp writes through a `const_cast`.
Found with an RTC_NOINIT breadcrumb (no serial): last stage 9 (inside
`enable_key_notifications`, the clicker asks right after connecting), last GAP event 5
(`L2CAP_UPDATE_REQ`), reset reason 4 (panic). Off = no crash, which is why it looked like
"crashes only when it connects".

**Every wait is on an event, never a poll or our own timer.** Each procedure ends in a
callback by itself: scan → `DISC_COMPLETE` after `kScanMs`, connect → `CONNECT` with a
timeout status, GATT → error callback after ATT's 30 s, pairing → `ENC_CHANGE` after SM's
30 s. Callbacks set bit 1 of `g_evt` and the waiter re-checks its condition. It is an
**event group, not a semaphore**, so a sync calling `radio_off()` while `connect_task` is
mid-wait wakes both. A toggle during Connecting sets `g_disconnect_request` and the bit,
ending the attempt at once (hardware-checked).

### Things that each failed silently on their own

1. **The MAC is byte-reversed.** NimBLE's `ble_addr_t.val` is little-endian: `val[5]` is the
   leftmost MAC byte (`ble_gap.c` tests `addr[5] & 0xC0`). `parse_mac()` fills `out[5 - i]`.
   Wrong order looks exactly like a clicker that is switched off.
2. **It scans before connecting.** An address does not carry its type (public/random), and
   HID devices advertise a **resolvable private address** rotating every ~15 min
   (`(addr[5] & 0xC0) == 0x40`). The configured MAC is a filter, not a destination.
3. **`ble_hs_cfg.sm_*` must be set.** NimBLE defaults `sm_bonding` and both key-dist masks
   to 0: pairing exchanges no keys, no bond. The ID key (IRK) is what later recognises a
   rotating address. `CONFIG_BT_NIMBLE_GATTC_AUTO_PAIR=y` stays as a backstop.
4. **Encrypt before any CCCD write.** HID CCCDs need encryption and the clicker refuses an
   unencrypted write; nothing retries it. Measured under esp_hidh (which wrote them during
   open, pre-encryption): link up, encrypted, bonded, battery reads, **0 notifications**
   over dozens of presses. It had only worked because the clicker remembered
   "notifications on" from an old bond; pairing it with a PC wiped that.
5. **Keys are matched by HID service range, not characteristic.**
   `ble_gattc_disc_all_dscs` over a service range reports the range start as every
   descriptor's `chr_val_handle`, so CCCD → report cannot be mapped (70 notifications on
   handle 43 matched none). Any `NOTIFY_RX` inside an HID service range goes to
   `handle_report`, which scans for Left/Right Arrow (0x50/0x4F; Page Up/Down not matched).
6. **Never find the connection by address.** After bonding the stack resolves the RPA to
   the identity address, so `ble_gap_conn_find_by_addr(scanned)` returned `ENOTCONN` on a
   live link. `g_conn` comes from the `CONNECT` event and is cleared on `DISCONNECT`.

**Stale bond.** If the clicker paired elsewhere it no longer has our key: encryption with
our bond fails. `encrypt_()` deletes our side of the bond (peer address taken before the
attempt, since the clicker often hangs up right after rejecting the key) and pairs once
more; if the link is gone, `connect_task` scans, connects and pairs once over a fresh
link (`g_bond_deleted`; a peer hang-up sets `g_disconnect_request`, so deliberate aborts
use `g_user_abort`). Only a completed, failed encryption deletes the bond, never a link
lost mid-attempt. A clicker that starts pairing itself lands in `REPEAT_PAIRING`, which
deletes and returns `BLE_GAP_REPEAT_PAIRING_RETRY`.

**Sync during a connect.** `begin_sync()` sets the abort, signals, and waits on
`kEvTaskDone` until `connect_task` has exited before `radio_off()`. Turning the
controller off under a running connect used to strand the task (a cancelled scan sends
no event) and let `end_sync()` start a second one beside it.

### Behaviour

- **No reconnect, no persistence.** A disconnect, a refused or never-completing connection
  all drop to Disconnected with the radio off. Deep-sleep wake is a full boot, so the
  device always comes up cold.
- **The callback never tears its own stack down.** `conn_cb` runs on the NimBLE host task;
  a peer `DISCONNECT` sets `g_disconnect_request` and `poll()` (UI loop) calls
  `radio_off()`. `radio_off()`'s own terminate sets `g_closing` so it is not "gone".
- **NVS is initialised lazily**, in `bringup_stack()` — the bond store is its only user.
- Input arrives as **edges only** via `g_press_queue` (ring, so a burst of clicks is a
  burst of presses). **The page turns on the press edge**, the moment the report arrives
  (a release-edge turn added the whole press, ~80-150 ms, to every page). A key still down
  at `config::kBluetoothHoldDelayMs` (100, owner's setting; separate from the device
  buttons' `kHoldDelayMs` 250) queues `Button::Undo` then Back (left) / Confirm (right):
  the screen reverts the step the press made without drawing, then acts, so a hold
  neither loses a page nor selects the wrong row. Both or neither: queued only when
  `queue_room() >= 2`. The release is ignored. `hold_wait_ms()` tells the idle main loop
  exactly when the hold is due (`docs/power-and-input.md` "Idle loop"). A tap held longer
  than the threshold reads as a hold.
- **A hold takes ~0.5-0.6 s to show its menu, and that is the panel.** The press already
  started a ~300 ms waveform, which cannot be cut short; the menu's own ~300 ms waveform
  follows it. The threshold is not the bottleneck (`docs/display.md`).
- **One mapping everywhere: right = `Button3`, left = `Button2`** (`kBtnRight` /
  `kBtnLeft`). In the reader `Button3` is next page (`ReaderScreen` `logical_next_front`,
  both orientations); in lists right is up, left is down. A reader-only swap
  (`g_in_reader`) outlived the reader that needed it and turned pages backward; removed
  2026-11-12 after the owner caught it on device. Holds were never affected (left = Back,
  right = Confirm).
- **Fast taps are not lost, they batch** (measured 2026-11-12, temporary counters): 26
  taps → 52 reports, 26 press edges, 0 queue drops, 0 holds; queue peaked at 5, longest a
  press waited 428 ms (one waveform: the loop blocks in the next `refresh()` until the
  panel is idle). The list applies every queued press, so several taps inside one
  ~430 ms waveform land as one multi-row jump, which reads as "skipped". Only a shorter
  waveform changes that; owner declined it for menus too (`docs/history.md`). One earlier
  run counted 20 queue drops out of 20 presses and never reproduced.
- `ClickerState` is Unavailable / Disconnected / Connecting / Connected. **Every failure
  reads as Disconnected**: the row is a switch, not a status console.

### The row shows the clicker's own battery

`read_battery_once()` reads Battery Level (0x2A19) by UUID over the whole handle range, after
the CCCDs, before Connected. Captured once (a percentage that twitches while you read is
worse than a stale one), cleared by `radio_off()`. **Most clickers notify battery only on
change**, which is why waiting for a notification showed nothing for hours.

**The read doubles as the liveness check.** A link that came up has been seen delivering no
reports; a peer that will not answer a GATT read will not send them either, so no answer
fails the connect. A clicker with no battery service still *answers* ("attribute not
found") and shows "Connected" with no percentage. `QuickmenuScreen::update` watches the
percentage as well as the state.

### The failure codes are gone — here is what they found

Stage codes (`Failed 8003`), free-heap readouts and `esp_reset_reason()` were scaffolding,
removed. What they established:

- `8003` / `8011` — sync timed out with 3 KB, then 11 KB, of internal RAM free: heap is the
  binding constraint. `esp_nimble_enable` **ignores `xTaskCreatePinnedToCore`'s return and
  always reports ESP_OK**, so a host task that could not be allocated looks like a sync
  that never comes.
- A *first* failure could wedge the session.
- `rst4` plus a backtrace named the real bug: `std::bad_alloc` in
  `TextLayout::assemble_page`, not in any BLE code.

Cheapest reconstruction if needed again: a stage code set at each bail-out, shown as a
temporary quick-menu row. Do not leave it in.

### Stack sizes

`CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` is **6144**, not IDF's 4096: the host task runs the
security manager and — because `ble_store_config` persists bonds — **NVS writes**, which are
stack-hungry, a second or two after connect. `wg_bt_up` is 5,120 (measured peak 1,736 B
before the handle cache, which adds NVS on this task).
The `esp_hidh` event task (6 KB) no longer exists. The NimBLE host task's high-water mark
was never read (`xTaskGetHandle("nimble_host")` found nothing; R8 still open).
Main task: 13,624 B of 16,384 never touched (2026-11-12, a session with clicker, sync,
book opens), so `CONFIG_ESP_MAIN_TASK_STACK_SIZE` is now 8192; ran a sync with the clicker
connected without a fault.

### The stack comes up once per boot and is NEVER torn down

`bringup_stack()` runs each init at most once, guarded by its **own** flag. Toggling off
calls `radio_off()`, which closes the link and disables the controller but leaves the
NimBLE host initialised for the session.

**One flag for the whole function is not enough**: a bringup that reached
`esp_nimble_init()` and then failed at sync left a single end-of-function flag false, so
the next attempt re-ran the one call that cannot be repeated. Per-step flags make a retry
resume rather than restart.

**Do not add a deinit path back.** Three attempts were made; the first two each shipped a
distinct bug:

1. **Unconditional teardown** (esp_hidh era) → a bringup that died at sync still deinited
   a stack whose host had never started; every attempt after that failed permanently until
   a power cycle.
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
| `g_nimble_up` / `g_host_task_started` | once per boot |
| `g_host_up` | cleared by `radio_off()`, re-waited on each bringup |

`radio_off()` cancels scan and pending connect, terminates the link and waits for its own
`DISCONNECT` (the controller always delivers one), then disables the controller. Reached
from every path: toggling off, a failed connect, and `poll()` acting on the clicker being
switched off mid-book (hardware-checked: row goes to Disconnected).

`g_host_up` must be re-waited after a re-enable: the controller runs a fresh HCI reset when
it comes back, and no GAP call is legal until the host has synced again (`sync_cb` sets
`g_evt`). The host task is started once and never deleted — `esp_nimble_disable()` is a
bare `vTaskDelete` and the host is not restartable.

### RAM now: nothing is handed back for the radio (2026-11-11)

Pages are fully baked (`docs/content.md`), so the reader has no layout engine, no
paragraph cache and no per-page allocation: a page is one display list of a few hundred
bytes read into `ReaderScreen::page_buf_` (4 KB, claimed at boot). With that gone every
workaround below was deleted: the clicker no longer releases DrawBuffer's spare (covers,
thumbnails and the pre-drawn page all stay with the clicker up), the book index is never
dropped, there is no cache cap, no `largest_free_block` guard, no OOM reserve and no
`set_new_handler` rescue, and `IRuntime::clicker_holds_ram()` /
`largest_free_block_bytes()` are gone. Image 1,199,358 → 1,141,052 B.

**Measured on hardware 2026-11-12, passed.** With the clicker connected, a 3-book sync
(36 MB), the automatic reconnect after it, and page turns, the largest internal block never
fell below 38,912 B (lowest at `esp_http_client_init`). Total free internal heap bottomed at
44,604 B, during the sync. Pass mark was 24 KB. That 44 KB is the whole budget for anything
kept resident: since nothing is released for radios, every permanent buffer comes out of it.
If the controller reports `esp_bt_controller_init -4` or
`emi.c 164` again, that is a RAM budget regression: find the allocation, do not start
releasing buffers again. Next lever if it is short: initialise the controller at boot
(disabled, no RF current) so its contiguous block is claimed before anything else, or
`CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y` (~19 KB IRAM).

Everything from here to the end of this file is the history of the layout-era heap,
kept as negative knowledge. None of the mechanisms it describes exist any more.

### Heap is the binding constraint, confirmed twice (layout era, superseded)

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
  allocates the worker's own 4 KB stack. The release is in `toggle()`, immediately before
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
