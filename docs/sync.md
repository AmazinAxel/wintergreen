# NAS book sync

Part of the Wintergreen notes; the entry point is `CLAUDE.md`.

## NAS book sync

One button on the book list: pull newly converted books off the homelab and retire finished
books. `platforms/esp32/wifi_sync.h` (header-only, `#ifdef WG_WIFI_SYNC`), reached through
`IRuntime::start_sync()` / `sync_state()` so `MainMenu` stays portable.

| | Without | With | Δ |
|---|---|---|---|
| flash | 692,342 | 1,213,488 | **+521 KB** |
| static RAM | 115,576 | 132,920 | **+17.3 KB** |

`CONFIG_MBEDTLS_TLS_ENABLED=n` is part of that: `esp_http_client` drags in esp-tls and
mbedtls **even for a plain-HTTP build**, and turning the client half off saved 36 KB. Image
validation still needs the SHA code, so mbedtls does not leave entirely.

**HTTP, not SMB, a deliberate reversal of the original request.** There is no SMB client in
ESP-IDF. The choices were vendoring `libsmb2` (~60-80 KB, a third-party dependency) or
hand-rolling SMB3, which the homelab pins (`server min protocol = SMB3`) and which
therefore **signs every packet** — AES-CMAC with the radio held open, on a device where one
second awake costs more than half an hour asleep. SMB also has no "give me the diff"
primitive: it is a listing plus a stat per book. The Pi already runs bun on port 80, so one
POST carries the whole negotiation. Samba stays — it is still how EPUBs get *onto* the Pi.

**`.local` names resolve through lwIP, not `esp_mdns`.** `esp_mdns` is a managed
component, not part of IDF — a component-manager fetch at build time for a responder this
device does not need to be. lwIP's resolver already does the client half:
`CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES` (pinned in `sdkconfig.defaults`) makes `getaddrinfo`
send a one-shot query to 224.0.0.251:5353 for any name ending `.local`, and a legacy-unicast
responder (Avahi on the Pi) answers it directly. Not yet confirmed on hardware.

Resolution is: literal IP in config → IP cached in NVS (stored with the name it came from,
so editing `syncServer` never reuses the old address) → `getaddrinfo`. **The cache is a
hint.** It was once trusted forever: when the server's lease moved, every sync failed and
nothing ever looked the name up again. Now a sync that fails against a cached address drops
it, re-resolves and tries once more; `run_sync_` fails only on the POST, so the retry is
safe.

### What a sync costs

| Network | |
|---|---|
| nothing changed | one POST + response, a few hundred bytes |
| N new books | + N × (`book.wgb` + up to 3 covers), **one** TCP connection |

One round trip decides everything (no per-file HEAD or stat), one kept-alive
`esp_http_client` handle covers the POST and every GET, and the reply carries only what the
device lacks.

| SD card | |
|---|---|
| nothing changed | **zero bytes** |
| N new books / deletions | the book files + **one** index save |

Guaranteed by two things, both of which have a tempting wrong version:

- **Never call `BookIndex::index_file()` / `remove_path()` / `rename_in_place()` here.**
  Each ends with its own `save()`, so one call per downloaded book rewrites the whole ~30 KB
  index per book. The sync uses the in-memory `remove_entry()` / `add_entry()`, which only
  set `dirty_`, and saves once at the end.
- **Downloads stream to `.tmp` then rename** (`remove()` first — FatFs `f_rename` does not
  replace). An interrupted download leaves a `.tmp`, never a truncated `book.wgb` that the
  reader would happily open.

### Reading positions are not synced — removed, don't re-add

`book.pos` is device-local. The sync carries `have` / `done` up and `get` / `delete` back,
nothing else. **The whole position half was built, shipped and deleted** because it was
buggy in practice and bought nothing: the reader is only ever open on one device, so there
is no second reader to merge with.

Gone with it: the wire `pos` object, `parse_pos_object_` / `PosEntry` / `read_pos_` /
`write_pos_` in `wifi_sync.h`, `state.pos` / `ahead()` / `dropPos()` / `handleDropped()` and
the `POST /booksync/dropped` route, and the `curl` call in `scripts/convertBooks.fish`.
`.sync.json` now holds only `finished`.

`progress_pct` still crosses the wire in exactly one place — `>= 100` as the "finished"
predicate in `done[]`, where the uint8 rounding that made it useless as a merge key cannot
matter because there is nothing past the end.

If it is ever wanted back: the merge key must be the four-number position tuple compared
lexicographically, **never `progress_pct`** (a uint8, so one percent spans four pages on a
400-page book and two genuinely different positions compare equal); the reply must carry
positions for books in `get` as well as `have` (a fresh card's `have` is empty, so iterating
it alone sent nothing and a restored device started every book at page one); the
currently-open book must be skipped, since `ReaderScreen::stop()` writes `.pos` on close and
would put the stale in-memory position back over it; and a re-converted book's stored
position must be dropped server-side, because the tuple indexes into a specific `book.wgb`.

### Finished books are deleted only after the server confirms

Ordering is the entire safety property:

1. device sends `done[]` (books at `progress_pct >= 100`)
2. server appends to `finished` in `.sync.json` and **fsyncs**
3. only then does the server delete `/media/books/<dir>/` (WGB + covers; the EPUB backup
   in `.epubs/` stays), and reply with `delete[]`
4. only on seeing `delete[]` does the device unlink

A crash at any step leaves the book present somewhere. `finished` is append-only and never
pruned — it is the permanent read history, deduplicated on `title|author`. A book at 100% is
re-sent every sync until one succeeds, which is idempotent server-side and cheaper than
tracking "already reported" state.

### The radio must be off when a sync ends, however it ends

Teardown is bound to **scope destruction** (`WifiGuard`), not to control flow, so a `return`
added later cannot leak it. Exceptions are off, so early returns are the only path that
matters. This is the requirement most likely to regress silently: a stuck radio is tens of
mA against a ~15 mA idle and is invisible except on a meter.

**Power save is deliberately OFF** (`WIFI_PS_NONE`). `MIN_MODEM` saves current while
associated and *idle*, which this workload never is — it connects, transfers flat out, and
leaves. Parking the radio between DTIM beacons adds latency to every round trip and
**extends** total radio-on time. Same reasoning keeps `AMPDU_RX` enabled. The energy model
is "finish and shut down", not "sip while connected".

**Throughput settings (2026-11-12, unmeasured on device; was ~130 KB/s).** The worker holds
its own `ESP_PM_CPU_FREQ_MAX` lock: the UI performance hold lapses 300 ms after the last
press and buttons are dead during a sync, so it used to run at the 80 MHz DFS floor. TCP
window 16 × MSS (23,040, was 4 × MSS) with `TCP_RECVMBOX_SIZE` 18, 24 dynamic RX buffers,
BA window 12 (IDF `#error`s above 2 × static RX buffers; static stays 6 so `esp_wifi_init`
needs no more). 16 KB read/write chunk. Connect: no ARP probe of the offered address,
`LWIP_DHCP_RESTORE_LAST_IP` re-requests the last lease. Floor for big books is the card
(20 MHz SPI), since receive and `fwrite` still run in series; a writer task would overlap them.

#### Internal RAM: nothing is released for Wi-Fi any more

`esp_wifi_init()` wants **roughly 50 KB of `MALLOC_CAP_INTERNAL`**, and it does not crash
when it cannot get it — it returns an error, the worker bails, and the row flicks back to
`Sync` with nothing said. In the layout era that happened with 20 KB free, and the sync
released DrawBuffer's spare, the OOM reserve and the reader's caches, and deinitialised the
BT controller, to fit. Pages are now fully baked, the reader holds no layout memory, and
all of that is gone: the spare (covers, thumbnails) and the book index stay through a sync.
Not yet confirmed on hardware — `H wifi` / `H http` in the temporary quick-menu rows are
the largest block at the two points that used to fail (see `docs/bluetooth.md` "RAM now").

**Never release the book index from a sync.** `MainMenu::entries_` holds `StringRef`s into
`BookIndex`'s `StringPool` and the list is on display: dropping the pool blanked every title
while the list stayed navigable.

#### A 200 is not proof you got the file — encode the URL

Book directory names contain spaces (`the odyssey`), and three things had to be wrong
together to produce a book that synced, appeared in the library, and would not open:

1. **The device sent a raw space in the request line**, not a valid URL. `url_escape_()`
   percent-encodes each path segment now.
2. **The server did not decode before validating.** `URL.pathname` keeps the `%20`, and
   `safeName()` rejects `%`, so every book file 404'd — and because the bun router falls
   through to the dashboard for unmatched paths, that 404 arrived as **200 + Content-Length
   + HTML**. `handleBookFile` decodes first, then validates, so an encoded `..%2f` still
   cannot slip a traversal through.
3. **Both ends accepted it.** The device checked only the status code, so it wrote the
   dashboard HTML to the card as `book.wgb` — all four files landed at an identical 9,486
   bytes, the tell. `http_get_file_()` now also requires the body to match the declared
   `Content-Length`.

The last link made it look like a reader bug rather than a transfer bug: `run_sync_()`
opened the downloaded WGB for its title, **ignored the failure**, and indexed it anyway with
the folder name as the title. A book that `WgbReader::open` rejects is now deleted and never
indexed, which also means the next sync re-offers it.

#### `add_entry` / `remove_entry` do not bump `generation()`

Deliberate — they are the in-memory mutators the sync batches so a dozen books cost one
index write — but nothing then tells the screens anything changed. `run_sync_()` calls
`BookIndex::bump_generation()` once, next to its single `save()`. Without it a freshly
synced book does not appear until you navigate away from the list and back.

#### Two connect bugs that both looked like "it fails instantly"

- **Never bound the connect by a retry count.** A rejected association comes back in
  *milliseconds*, so a budget of two retries was spent before the radio had meaningfully
  tried. `wifi_event_` reconnects for as long as `kConnectTimeoutMs` (20 s) allows, and that
  timeout is the only bound.
- **Never pin a cached BSSID/channel on the first attempt.** If the AP moved channel, every
  retry re-attempted the *same* pinned config and failed identically, never falling back to
  a scan — indistinguishable from a wrong password. BSSID/channel are no longer cached at
  all (they were written to NVS every sync and never read); association always scans.

**The clicker steps aside for the sync and comes back by itself.** One radio at a time
(owner's choice over coexistence). `start()` calls `wg_clicker::begin_sync()`
**synchronously before the worker spawns**: it closes the link and *disables* the
controller (`radio_off()`), leaves the stack initialised, and returns whether the clicker
was Connected or Connecting. While the sync owns the radio `toggle()` is a no-op. When the
worker ends it `ui_wake()`s the loop, and `wg_sync::poll()` calls `end_sync(was_on)`, which
reconnects with an ordinary `toggle()`. **Never deinit the controller for a sync** — that
was the old way to make room, and it lost the clicker until reboot (the NimBLE host cannot
rebind; see `radio_off()`).

**Auto-sleep is held off while a sync runs.** `wg_sync::poll()` calls `keep_awake()` from the
main loop; a multi-book transfer easily outlasts `kAutoSleepMinutes` and sleeping mid-download
would cut the radio with files half written.

**All buttons are ignored while a sync runs, power included** (`main.cpp` empties the
`ButtonState` while `wg_sync::active()`). The low-battery cutoff stays suppressed too
(`keep_awake()`), so a sync always runs to its end. Timeouts bound it: 20 s Wi-Fi connect,
8 s per HTTP read.

**Speed floor: ~300 KB/s, serial on purpose** (`DownloadBuf`). Measured 2026-11-12, 4 MB books:

| Variant | Throughput | Split |
|---|---|---|
| 16 KB heap buffer, read then write | 296 KB/s | net 7.6 s + card 6.8 s + ~1.6 s overhead |
| double buffer, writer task same priority (4) | 244 KB/s | net 14.5 s: writer spin held the 10 ms tick |
| double buffer, writer one below (3) | 292 KB/s | net 6.5 s + waiting for buffer 5.5 s = serial |

The C3 has one core; the Wi-Fi/TCP receive path and SDSPI (interrupt-driven per-sector and
busy-poll transactions, `sdspi_host.c`) both need it, so overlap only interleaves them.
The writer task was removed. What stayed: the download fills the spare framebuffer, lent by
`wg_sync::poll()` (UI thread) via `DrawBuffer::lend_spare()`, 92 whole sectors per `fwrite`,
zero heap. While lent, every other spare claim refuses; the list thumbnails are re-read
after the sync, and the sync task waits for the spare to come back before setting Done so the
final repaint has them. Without a spare, `kChunk` from heap. Below this floor would take an
ESP-IDF SDSPI patch; the SD clock is already at its 20 MHz ceiling (CLAUDE.md).

**Downloads resume with Range** (`http_get_file_`). Measured 2026-11-12: a 36 MB book at
~130 KB/s hit one 8 s read timeout (`ESP_ERR_HTTP_EAGAIN`) at 30.2 of 36.1 MB after 234 s,
and the whole book was discarded. Now a read error or an early close reopens with
`Range: bytes=<written>-` and appends to the same `.tmp`. It accepts only a 206 whose length
is exactly the missing tail, and gives up after 5 reopens in a row that brought no bytes.

Open (2026-11-12): an earlier run of the same sync left a 0-byte `book.wgb.tmp`, no index,
and boots that showed only the wordmark until the card was pulled. Probably a crash
mid-download, not captured. The same session's `H min` (session minimum of the largest
internal block) read 1800 B. Where that happened is not known.

### The server side

In the flake repo (`snowflake`), not here: `hosts/alechomelab/webserver/booksync.js` bolted
onto the existing bun server (no new service, no new port). **The homelab never converts.**
`books` (`tools/books` in this repo, packaged by `flake.nix`, installed by snowflake's
`modules/desktop.nix`, no arguments) reviews every EPUB in `~/Downloads/books`, bakes
locally, and uploads over ssh in one tar stream (written by `books` itself): books into
`/media/books/.incoming/`, then `mv` into place, so a half-uploaded book is never visible
to a concurrent sync; reviewed EPUBs straight into `/media/books/.epubs/` as
`<Title> by <Author>.epub` (backup only, never served; the device fetches book files by
fixed name). Only after the upload succeeds are the uploaded EPUBs deleted from
`~/Downloads/books` (and the folder, if empty); nothing is kept on the desktop. To re-bake,
copy the backup EPUB back into `~/Downloads/books`: its `wg.ncx` restores the chapter picks.

Removed, don't re-add: `books.nix` (path unit converting on the Pi) and
`scripts/convertBooks.fish`. The `DirectoryNotEmpty` retrigger loop that unit once had is
moot with it.

Endpoints: `POST /booksync` (the whole negotiation) and `GET /booksync/<dir>/<file>` (one
book file). Book directory names are validated against `/^[A-Za-z0-9 ._-]+$/` and rejected
if they contain `..` or start with `.` — they are used as path segments, and dot dirs
(`.epubs`, `.incoming`) must never be listed or served. `books::slug` (`tools/books/labels.cpp`)
produces names that pass.

Book names contain spaces, so anything built on them must encode them.

Two host-side tests worth keeping green:
`hosts/alechomelab/webserver/booksync.test.js` (the negotiation, including the
confirm-before-unlink ordering) and `tools/tests/json_scan_test.cpp` (the device's
hand-rolled JSON scanner, under ASan/UBSan, including every truncation of a valid reply).
