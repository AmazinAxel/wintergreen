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

**mDNS is not used and should not be added.** `esp_mdns` is a managed component, not part of
IDF, so it would mean a component-manager fetch at build time. Resolution is: cached IP from
NVS → literal IP in config → `getaddrinfo`.

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
3. only then does the server unlink, and reply with `delete[]`
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

#### Internal RAM is the binding constraint, and the framebuffers are why

`esp_wifi_init()` wants **roughly 50 KB of `MALLOC_CAP_INTERNAL`**, and it does not crash
when it cannot get it — it returns an error, the worker bails, and the row flicks back to
`Sync` with nothing said. Measured during bring-up: **20 KB free**, so it failed every time.

`DrawBuffer` holds `bufs_[2][kBufSize]` — **96,036 bytes of BSS**, the largest symbol in the
image — plus `spare_`, another 48 KB on the heap. 144 KB of framebuffers on a 320 KB part.

So `wg_sync::start()` calls **`DrawBuffer::release_spare()`**, handing the 48 KB back and
taking it again when the sync ends (on every path, including a failed `xTaskCreate`). Safe
because the spare is purely an optimisation — without it a quick-menu dismiss re-renders and
a page turn re-draws, and the sync screen does neither. Both claim paths already null-check.

`release_spare()` must also reset `spare_use_` and `draw_target_`: `has_snapshot()` /
`has_offscreen()` test only that enum, so leaving it set would let `restore_snapshot()`
memcpy from a freed pointer.

**The book index is deliberately not released here**, unlike the clicker path.
`MainMenu::entries_` holds `StringRef`s into `BookIndex`'s `StringPool`, and that screen is
*on display* when Sync is pressed — dropping the pool left every title and author resolving
to an empty `string_view` while the list stayed navigable and kept showing reading
percentages (`uint8_t` copies). It read as a drawing glitch rather than a dangling
reference. It also bought nothing: `run_sync_()` reloads the index from disk as its first
act.

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
  a scan — indistinguishable from a wrong password. The hint is still cached for
  `resolve_server_`, but association always starts with a normal scan.

**BLE goes down first and does not come back on its own.** One radio, and ~13 KB free with
the clicker up leaves no room for Wi-Fi's ~40-50 KB. `start()` calls
`wg_clicker::radio_off()` and `Application::release_ram_for_radio()` **synchronously before
the worker task spawns**. The clicker is re-armed by pressing its quick-menu row again.

**Auto-sleep is held off while a sync runs.** `wg_sync::poll()` calls `keep_awake()` from the
main loop; a multi-book transfer easily outlasts `kAutoSleepMinutes` and sleeping mid-download
would cut the radio with files half written.

### The server side

In the flake repo, not here: `hosts/alechomelab/webserver/booksync.js` bolted onto the
existing bun server (no new service, no new port), plus `hosts/alechomelab/books.nix` — a
systemd `.path` unit watching `/media/books` that converts any new EPUB into
`.compiled/<stem>/` with `epub2wgb`, staged in a temp dir and moved into place so a
half-written book is never visible to a concurrent sync.

`epub2wgb` is packaged by **this** repo's `flake.nix` and consumed as
`inputs.wintergreen.packages.aarch64-linux.epub2wgb`. Its `CMakeLists.txt` has no
`install()` rule, so the derivation supplies its own `installPhase`.

Endpoints: `POST /booksync` (the whole negotiation) and `GET /booksync/<dir>/<file>` (one
book file). Book directory names are validated against `/^[A-Za-z0-9 ._-]+$/` and rejected
if they contain `..` — they are used as path segments.

One bug worth not repeating: **`DirectoryNotEmpty` is a level, not an edge.** It re-triggers
for as long as the watched directory has *anything* in it, so the service restarted the
instant it finished, forever, until systemd's rate limiter killed it with `start-limit-hit`
— **part-way through the loop**, so the alphabetically last book was never converted and
nothing said so. It is `PathChanged` now (fires once per upload), plus a `startLimitBurst`
backstop so a future retrigger loop fails loudly instead of spinning.

Book names contain spaces, so anything built on them must encode them.

Two host-side tests worth keeping green:
`hosts/alechomelab/webserver/booksync.test.js` (the negotiation, including the
confirm-before-unlink ordering) and `tools/tests/json_scan_test.cpp` (the device's
hand-rolled JSON scanner, under ASan/UBSan, including every truncation of a valid reply).
