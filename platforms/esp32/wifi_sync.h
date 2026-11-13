#pragma once

// NAS book sync over HTTP.
//
// Pulls newly converted books off the homelab and retires books read to 100%.
// One button, one round trip, radio off the moment it is done.
//
// Reading positions are **not** synced. They stayed on the device.
//
// **HTTP, not SMB.** ESP-IDF ships no SMB client, and SMB3 (which the server
// pins) signs every packet — CPU time with the radio held open, on a device
// where one second awake costs more than half an hour asleep. The server
// already runs a web server, so a single POST carries the whole negotiation.
//
// Three things this file is built around:
//
//  - **The radio is off on every exit path.** Success, failure, timeout. The
//    teardown is bound to scope destruction (WifiGuard), not to control flow,
//    so a `return` added later cannot leak it.
//  - **Wi-Fi and BLE do not fit together.** The clicker is powered down before
//    the radio starts; it is re-armed by pressing its quick-menu row again.
//  - **Writes to the SD card are the scarce resource.** A sync that changes
//    nothing writes zero bytes: the index save is dirty-guarded.
//
// Compiled out entirely unless WG_WIFI_SYNC is defined, the same way
// WG_BLUETOOTH_PAGE_TURNER gates NimBLE.

#include <cstdint>

#include "ui_wake.h"
#include "wintergreen/Runtime.h"

namespace wg_sync {

// The state enum lives in Runtime.h, the portable seam the UI reads through —
// MainMenu cannot include this header.
using wintergreen::SyncState;

}  // namespace wg_sync

#ifdef WG_WIFI_SYNC

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "WintergreenConfig.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wintergreen/Application.h"
#include "wintergreen/content/BookIndex.h"
#include "wintergreen/display/DrawBuffer.h"

namespace wg_sync {

inline volatile SyncState g_state = SyncState::Idle;
inline volatile bool g_busy = false;
// UI-thread only: a sync was started and poll() has not yet handed the radio
// back to the clicker; whether the clicker was up when the sync took it.
inline bool g_end_pending = false;
inline bool g_reconnect_clicker = false;

// Set while the worker runs so the UI loop can hold off auto-sleep. A sync with
// several books to fetch easily outlasts kAutoSleepMinutes, and deep-sleeping
// mid-transfer would cut the radio with files half written.
inline volatile bool g_keep_awake = false;

inline wintergreen::Application* g_app = nullptr;
inline wintergreen::DrawBuffer* g_buf = nullptr;

inline EventGroupHandle_t g_wifi_events = nullptr;
inline constexpr int kConnectedBit = BIT0;

// Generous: a full scan, WPA2 handshake and DHCP lease on a busy 2.4 GHz band
// can take well over ten seconds, and the cost of waiting is a few seconds of
// radio rather than a failed sync the user has to notice and retry by hand.
inline constexpr int kConnectTimeoutMs = 20000;
inline constexpr int kHttpTimeoutMs = 8000;
// One chunk of a book download, read and written in series. Bigger chunks mean
// fewer, longer card writes (multi-block SD transfers) while the TCP window keeps
// the sender going; a book is still never held in RAM.
inline constexpr int kChunk = 16384;

// ── Cached server address ──────────────────────────────────────────────────
//
// In NVS rather than the settings file: this is device-local network state, not
// a user setting, and the settings file is deliberately three keys.
//
// The resolved IP skips the lookup — for a `.local` name that is an mDNS query,
// multicast and a second or so of radio. It is stored with the name it was
// resolved from, so editing syncServer never reuses the old server's address,
// and it is only a hint: a sync that fails against it re-resolves once and
// re-caches (see sync_task).

inline constexpr const char* kNvsNamespace = "wgsync";

inline uint32_t load_cached_ip_() {
  nvs_handle_t nvs;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK)
    return 0;
  uint32_t ip = 0;
  char host[64] = {};
  size_t len = sizeof(host);
  const bool ok = nvs_get_u32(nvs, "ip", &ip) == ESP_OK && nvs_get_str(nvs, "host", host, &len) == ESP_OK &&
                  std::strcmp(host, wintergreen::config::syncServer) == 0;
  nvs_close(nvs);
  return ok ? ip : 0;
}

// ip 0 forgets the cached address. Only called when it changes, so a sync
// against a good cache writes no NVS at all.
inline void save_cached_ip_(uint32_t ip) {
  nvs_handle_t nvs;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK)
    return;
  if (ip) {
    nvs_set_u32(nvs, "ip", ip);
    nvs_set_str(nvs, "host", wintergreen::config::syncServer);
  } else {
    nvs_erase_key(nvs, "ip");
  }
  nvs_commit(nvs);
  nvs_close(nvs);
}

// ── Wi-Fi bring-up ─────────────────────────────────────────────────────────

inline void wifi_event_(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    // Retry for as long as bring_up_wifi_'s timeout allows rather than counting
    // attempts. A rejected association returns in milliseconds, so a small
    // attempt budget is exhausted long before the radio has really tried; the
    // wall-clock timeout is the honest bound and it is the one the user feels.
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(g_wifi_events, kConnectedBit);
  }
}

// Everything the radio owns, released by destruction so that no early return
// can leak it. This is the hard requirement: the Wi-Fi stack must be down when
// a sync ends, however it ends.
struct WifiGuard {
  esp_netif_t* netif = nullptr;
  esp_event_handler_instance_t any_wifi = nullptr;
  esp_event_handler_instance_t got_ip = nullptr;

  ~WifiGuard() {
    if (any_wifi)
      esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, any_wifi);
    if (got_ip)
      esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip);
    esp_wifi_stop();
    esp_wifi_deinit();
    if (netif) {
      esp_netif_destroy_default_wifi(netif);
    }
    if (g_wifi_events) {
      vEventGroupDelete(g_wifi_events);
      g_wifi_events = nullptr;
    }
  }
};

// ── Tiny JSON scanning ─────────────────────────────────────────────────────
//
// The response has two known keys, each holding an array of strings. A JSON
// library for that is a dependency to avoid; this is ~30 lines and only ever
// sees output from our own server.

// Find `"key"` at the top level and return the offset just past its colon.
inline size_t find_key_(const std::string& s, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  const size_t k = s.find(needle);
  if (k == std::string::npos)
    return std::string::npos;
  const size_t colon = s.find(':', k + needle.size());
  return colon == std::string::npos ? std::string::npos : colon + 1;
}

// Strings out of the array starting at `pos`. Handles \" and \\ escapes only —
// the server never emits anything else in a book directory name.
inline std::vector<std::string> parse_string_array_(const std::string& s, size_t pos) {
  std::vector<std::string> out;
  if (pos == std::string::npos)
    return out;
  const size_t open = s.find('[', pos);
  if (open == std::string::npos)
    return out;
  size_t i = open + 1;
  while (i < s.size() && s[i] != ']') {
    if (s[i] != '"') { ++i; continue; }
    std::string item;
    for (++i; i < s.size() && s[i] != '"'; ++i) {
      if (s[i] == '\\' && i + 1 < s.size())
        ++i;
      item.push_back(s[i]);
    }
    ++i;  // closing quote
    out.push_back(std::move(item));
  }
  return out;
}

// ── HTTP ───────────────────────────────────────────────────────────────────

inline esp_http_client_handle_t g_http = nullptr;
inline std::string g_base_url;

// Collect a whole (small) response body. Only used for the POST, whose reply is
// a few hundred bytes; book downloads stream instead.
inline bool http_post_(const std::string& body, std::string& out) {
  esp_http_client_set_url(g_http, (g_base_url + "/booksync").c_str());
  esp_http_client_set_method(g_http, HTTP_METHOD_POST);
  esp_http_client_set_header(g_http, "Content-Type", "application/json");
  if (esp_http_client_open(g_http, static_cast<int>(body.size())) != ESP_OK)
    return false;
  bool ok = esp_http_client_write(g_http, body.data(), static_cast<int>(body.size())) ==
            static_cast<int>(body.size());
  if (ok)
    ok = esp_http_client_fetch_headers(g_http) >= 0 &&
         esp_http_client_get_status_code(g_http) == 200;
  if (ok) {
    char buf[512];
    int r;
    while ((r = esp_http_client_read(g_http, buf, sizeof(buf))) > 0)
      out.append(buf, static_cast<size_t>(r));
    ok = r >= 0;
  }
  esp_http_client_close(g_http);
  return ok;
}

// ── Download pipe: network fills one buffer while the card writes the other ──

// The spare framebuffer, lent by the UI thread for the length of a sync. It is
// 48 KB the sync would otherwise take from a heap that bottoms out at ~44 KB
// free during one, and while a sync runs nothing needs it: input is locked and
// the book list repaints only at the end (its thumbnails are re-read then).
// Lending and returning both happen in poll() on the UI thread, which owns the
// DrawBuffer; this task only asks and waits.
inline uint8_t* g_spare = nullptr;
inline volatile bool g_spare_want = false;
inline volatile bool g_spare_back = false;
inline SemaphoreHandle_t g_spare_ready = nullptr;

inline uint8_t* acquire_spare_() {
  if (g_spare)
    return g_spare;
  if (!g_spare_ready)
    g_spare_ready = xSemaphoreCreateBinary();
  if (!g_spare_ready)
    return nullptr;
  g_spare_want = true;
  ui_wake();
  xSemaphoreTake(g_spare_ready, portMAX_DELAY);
  return g_spare;  // null when there is no spare: DownloadBuf falls back to the heap
}

// One download buffer: the lent spare in whole sectors (92 × 512 B), or kChunk
// from the heap without one. Filled from the network, written in one fwrite,
// so the card sees few, large multi-block writes.
//
// **Serial on purpose.** A double buffer with a separate card-writer task was
// built and measured 2026-11-12 and gained nothing (docs/sync.md "Speed
// floor"): the C3 has one core, and both the Wi-Fi/TCP receive path and SDSPI
// (interrupt-driven per-sector and busy-poll transactions) need it, so the two
// took turns instead of overlapping.
struct DownloadBuf {
  uint8_t* data = nullptr;
  size_t cap = 0;
  uint8_t* heap = nullptr;

  bool open() {
    if (uint8_t* spare = acquire_spare_()) {
      data = spare;
      cap = wintergreen::DrawBuffer::kBufSize / 512 * 512;
      return true;
    }
    heap = static_cast<uint8_t*>(std::malloc(kChunk));
    data = heap;
    cap = kChunk;
    return heap != nullptr;
  }
  ~DownloadBuf() {
    std::free(heap);
  }
};

// Stream one file to the card. Written to .tmp and renamed, so an interrupted
// download can never leave a truncated book.wgb that the reader would accept.
//
// Returns false for a missing file too; covers are optional and a 404 is not an
// error to the caller.
inline bool http_get_file_(const std::string& url_path, const std::string& dest) {
  esp_http_client_set_url(g_http, (g_base_url + url_path).c_str());
  esp_http_client_set_method(g_http, HTTP_METHOD_GET);
  if (esp_http_client_open(g_http, 0) != ESP_OK)
    return false;
  const int64_t declared = esp_http_client_fetch_headers(g_http);
  if (declared < 0 || esp_http_client_get_status_code(g_http) != 200) {
    esp_http_client_close(g_http);
    return false;
  }

  const std::string tmp = dest + ".tmp";
  // The card shares SPI2 with the panel; a write during a waveform corrupts the
  // update in flight.
  if (g_buf)
    g_buf->wait_panel_idle();
  std::remove(tmp.c_str());
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) {
    esp_http_client_close(g_http);
    return false;
  }

  // **A stalled read resumes, it does not restart.** A 36 MB book takes minutes,
  // and over that long one 8 s gap with no data is likely: the first one
  // measured hit at 30 of 36 MB (ESP_ERR_HTTP_EAGAIN), and the whole book was
  // thrown away and re-offered, to fail the same way next time. On a read error
  // or an early close the connection is reopened with Range from the bytes
  // already received (the server answers 206; Bun.file supports it) and the same
  // file keeps growing. Gives up after kMaxResumes reopens in a row that brought
  // no new bytes.
  static constexpr int kMaxResumes = 5;
  DownloadBuf buf;
  if (!buf.open()) {
    std::fclose(f);
    esp_http_client_close(g_http);
    std::remove(tmp.c_str());
    return false;
  }
  bool ok = true;
  int resumes = 0;
  int64_t written = 0;  // received; on the card once flushed below
  int r = 0;
  size_t fill = 0;
  for (;;) {
    const int64_t before = written;
    while ((r = esp_http_client_read(g_http, reinterpret_cast<char*>(buf.data) + fill,
                                     static_cast<int>(buf.cap - fill))) > 0) {
      fill += static_cast<size_t>(r);
      written += r;
      if (fill == buf.cap) {
        if (std::fwrite(buf.data, 1, fill, f) != fill) {
          ok = false;
          break;
        }
        fill = 0;
      }
    }
    if (!ok)
      break;
    // Without a declared length, a clean close is the only end there is.
    if (declared <= 0 ? r == 0 : written >= declared)
      break;
    resumes = written > before ? 1 : resumes + 1;
    if (resumes > kMaxResumes) {
      ok = false;
      break;
    }
    esp_http_client_close(g_http);
    char range[32];
    std::snprintf(range, sizeof(range), "bytes=%lld-", static_cast<long long>(written));
    esp_http_client_set_header(g_http, "Range", range);
    int64_t rest = -1;
    if (esp_http_client_open(g_http, 0) == ESP_OK)
      rest = esp_http_client_fetch_headers(g_http);
    esp_http_client_delete_header(g_http, "Range");  // the handle goes on to the covers
    // 206 with exactly the missing tail, or nothing: a 200 here would be the
    // whole file again, appended after the part already written.
    if (rest != declared - written || esp_http_client_get_status_code(g_http) != 206) {
      ok = false;
      break;
    }
  }
  if (ok && fill && std::fwrite(buf.data, 1, fill, f) != fill)
    ok = false;
  if (std::fclose(f) != 0)
    ok = false;
  esp_http_client_close(g_http);

  // **A 200 is not proof this is the file we asked for.** The server's router
  // falls through to an HTML page for any unmatched path, so a routing mistake
  // arrives as 200 + Content-Length + a body — and the first version wrote that
  // dashboard HTML to the card as book.wgb, four times, identically sized.
  // Insisting the body match the declared length catches a truncated transfer;
  // the reader's header offset check catches the rest.
  if (ok && declared > 0 && written != declared)
    ok = false;
  if (ok && written == 0)
    ok = false;

  if (!ok) {
    std::remove(tmp.c_str());
    return false;
  }
  // FatFs f_rename does not replace, unlike POSIX — the target must go first.
  std::remove(dest.c_str());
  return std::rename(tmp.c_str(), dest.c_str()) == 0;
}

// Percent-encode one path segment. Book directory names contain spaces ("the
// odyssey"), and a raw space in a request line is not a valid URL — the server
// never matched the route and fell through to its HTML page.
//
// Only unreserved characters pass through (RFC 3986); everything else is
// escaped, which is safe because the server percent-decodes before validating.
inline std::string url_escape_(const std::string& s) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() + 8);
  for (const unsigned char c : s) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

// ── JSON emission ──────────────────────────────────────────────────────────

inline void append_escaped_(std::string& out, const std::string& s) {
  out.push_back('"');
  for (const char c : s) {
    if (c == '"' || c == '\\')
      out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
}

// ── The sync itself ────────────────────────────────────────────────────────

inline bool run_sync_() {
  using wintergreen::BookIndex;

  const char* books_dir = g_app->main_menu()->books_dir();
  // data_dir_ is not used directly any more, but index_path() is built from it,
  // so a null root is still fatal.
  if (!books_dir || !g_app->data_dir_)
    return false;

  auto& index = BookIndex::instance();
  if (index.entries().empty())
    index.load(g_app->index_path());

  // One pass over the index builds both request fields.
  std::string req = "{\"have\":[";
  std::string done_json = "\"done\":[";
  bool first_have = true, first_done = true;

  for (const auto& e : index.entries()) {
    const std::string path(e.path.view(index.pool()));
    // The protocol keys on the book's folder name; a book with none is skipped.
    const std::string dir = wintergreen::book_folder_name(path);
    if (dir.empty())
      continue;

    if (!first_have)
      req.push_back(',');
    first_have = false;
    append_escaped_(req, dir);

    // A finished book is only *eligible* for deletion; the server has to
    // confirm it recorded it before anything is unlinked.
    if (e.progress_pct >= 100) {
      if (!first_done)
        done_json.push_back(',');
      first_done = false;
      done_json += "{\"dir\":";
      append_escaped_(done_json, dir);
      done_json += ",\"title\":";
      append_escaped_(done_json, std::string(e.title.view(index.pool())));
      done_json += ",\"author\":";
      append_escaped_(done_json, std::string(e.author.view(index.pool())));
      done_json.push_back('}');
    }
  }
  req += "],";
  req += done_json + "]}";

  std::string resp;
  if (!http_post_(req, resp))
    return false;

  const std::vector<std::string> get = parse_string_array_(resp, find_key_(resp, "get"));
  const std::vector<std::string> del = parse_string_array_(resp, find_key_(resp, "delete"));

  bool index_changed = false;

  // 1. Download new books.
  for (const std::string& dir : get) {
    const std::string book_dir = std::string(books_dir) + "/" + dir;
    if (g_buf)
      g_buf->wait_panel_idle();
    ::mkdir(book_dir.c_str(), 0777);

    const std::string wgb = book_dir + "/book.wgb";
    const std::string url_dir = url_escape_(dir);
    if (!http_get_file_("/booksync/" + url_dir + "/book.wgb", wgb))
      continue;  // leave the rest of the sync running; retried next time

    // Covers are optional — an EPUB without one produces no cover files at all,
    // and a missing one draws a placeholder. cover.bin is no longer made; the
    // removal lists below still name it, so older books clean up fully.
    for (const char* cover : {"cover_list.bin", "cover_home.bin", "cover_sleep.bin"})
      http_get_file_("/booksync/" + url_dir + "/" + cover, book_dir + "/" + cover);

    // Index it in memory only. index_file() would save the whole ~30 KB index
    // per book; one save at the end covers every mutation.
    //
    // **A book that does not open is never indexed.** WgbReader::open checks the
    // header offsets, so this is the backstop that catches anything the transfer
    // checks missed. The first version ignored the result and indexed the book
    // anyway with the folder name as its title — so a bad download appeared in
    // the library looking perfectly normal and simply failed to open. Deleting
    // it here means the next sync re-offers it, since the device stops claiming
    // to have it.
    std::string title, author;
    if (!wintergreen::read_book_meta(wgb, title, author)) {
      std::remove(wgb.c_str());
      for (const char* cover : {"cover.bin", "cover_list.bin", "cover_home.bin", "cover_sleep.bin"})
        std::remove((book_dir + "/" + cover).c_str());
      ::rmdir(book_dir.c_str());
      continue;
    }
    index.remove_entry(wgb);
    index.add_entry(wgb, title, author);
    index_changed = true;
  }

  // 2. Remove what the server confirmed it has recorded.
  for (const std::string& dir : del) {
    const std::string book_dir = std::string(books_dir) + "/" + dir;
    if (g_buf)
      g_buf->wait_panel_idle();
    for (const char* f : {"book.wgb", "book.pos", "cover.bin", "cover_list.bin", "cover_home.bin", "cover_sleep.bin"})
      std::remove((book_dir + "/" + f).c_str());
    ::rmdir(book_dir.c_str());
    index.remove_entry(book_dir + "/book.wgb");
    index_changed = true;
  }

  // One save, dirty-guarded: a sync that changed nothing writes nothing.
  //
  // The generation bump is what makes MainMenu rebuild and repaint, so a book
  // that just arrived shows up without navigating away and back. add_entry and
  // remove_entry only set dirty_ — deliberately, so a batch of them costs one
  // index write — so nothing else here would tell the screens anything changed.
  if (index_changed) {
    index.save(g_app->index_path());
    index.bump_generation();
  }

  return true;
}

// ── Worker ─────────────────────────────────────────────────────────────────

inline bool bring_up_wifi_(WifiGuard& guard) {
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs = nvs_flash_init();
  }
  if (nvs != ESP_OK)
    return false;

  if (esp_netif_init() != ESP_OK)
    return false;
  // Both are idempotent-ish: already-created is not an error worth failing on,
  // since a second sync in the same boot re-enters here.
  esp_event_loop_create_default();

  g_wifi_events = xEventGroupCreate();
  if (!g_wifi_events)
    return false;

  guard.netif = esp_netif_create_default_wifi_sta();
  if (!guard.netif)
    return false;

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&cfg) != ESP_OK)
    return false;

  if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_, nullptr,
                                          &guard.any_wifi) != ESP_OK ||
      esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_, nullptr,
                                          &guard.got_ip) != ESP_OK)
    return false;

  wifi_config_t wc = {};
  std::snprintf(reinterpret_cast<char*>(wc.sta.ssid), sizeof(wc.sta.ssid), "%s", wintergreen::config::kWifiName);
  std::snprintf(reinterpret_cast<char*>(wc.sta.password), sizeof(wc.sta.password), "%s",
                wintergreen::config::kWifiPassword);

  // **No BSSID/channel pin.** It shipped once, and it is a trap: a pinned BSSID
  // that has gone stale (the AP moved channel, or you are near a different one
  // in a mesh) fails to associate, and the retry in wifi_event_ just calls
  // esp_wifi_connect() again against the *same* pinned config — so every retry
  // fails identically and the sync dies without ever trying a normal scan.
  // Saving 1-2 s of scan is not worth a failure mode that looks exactly like a
  // wrong password.

  if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK)
    return false;
  // Credentials live in the firmware, so there is nothing to persist — and this
  // avoids an NVS write on every sync.
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK)
    return false;
  if (esp_wifi_start() != ESP_OK)
    return false;

  // Power save OFF, deliberately. It saves current while associated and *idle*,
  // which this workload never is: connect, transfer flat out, leave. MIN_MODEM
  // would park the radio between DTIM beacons and add latency to every round
  // trip, extending total radio-on time — the opposite of the goal.
  esp_wifi_set_ps(WIFI_PS_NONE);

  // Wait for the *timeout*, not for the retry counter. An association failure
  // can come back in milliseconds (bad password, AP busy), so a small retry
  // budget is spent almost instantly and the sync gives up while the radio has
  // barely tried — user-visible as "Syncing..." flicking straight back to
  // "Sync". wifi_event_ keeps reconnecting for as long as this timeout allows.
  const EventBits_t bits = xEventGroupWaitBits(g_wifi_events, kConnectedBit, pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(kConnectTimeoutMs));
  return (bits & kConnectedBit) != 0;
}

// Resolve the server to "http://<ipv4>".
//
// **`.local` works, through lwIP rather than `esp_mdns`.** IDF builds lwIP with
// LWIP_DNS_SUPPORT_MDNS_QUERIES (pinned in sdkconfig.defaults), so getaddrinfo
// on a name ending in `.local` sends a one-shot multicast query to
// 224.0.0.251:5353 and takes the responder's unicast answer — Avahi on the Pi
// answers those. `esp_mdns` would be a managed component, a build-time fetch, for
// a responder this device does not need to be. Any other name goes to the DHCP
// DNS server, and a literal IP skips the question entirely.
//
// `use_cache` false forces a fresh lookup; the result is cached either way.
inline bool resolve_server_(std::string& url_out, bool use_cache, bool* from_cache) {
  *from_cache = false;
  uint32_t addr = 0;

  // A literal IP in the config needs no lookup and no cache.
  ip_addr_t parsed;
  if (ipaddr_aton(wintergreen::config::syncServer, &parsed) && IP_IS_V4(&parsed)) {
    addr = ip_2_ip4(&parsed)->addr;
  } else {
    const uint32_t cached = use_cache ? load_cached_ip_() : 0;
    if (cached) {
      addr = cached;
      *from_cache = true;
    } else {
      // IPv4 only: lwIP is built without IPv6 here.
      struct addrinfo hints = {};
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      struct addrinfo* res = nullptr;
      if (getaddrinfo(wintergreen::config::syncServer, "80", &hints, &res) != 0 || !res)
        return false;
      addr = reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
      freeaddrinfo(res);
      if (addr != load_cached_ip_())
        save_cached_ip_(addr);
    }
  }

  esp_ip4_addr_t a;
  a.addr = addr;
  char ip[16];
  esp_ip4addr_ntoa(&a, ip, sizeof(ip));
  url_out = std::string("http://") + ip;
  return true;
}

// One sync against g_base_url, on its own HTTP handle.
inline bool sync_once_() {
  esp_http_client_config_t hc = {};
  hc.url = g_base_url.c_str();
  hc.timeout_ms = kHttpTimeoutMs;
  // One handle, kept alive across the POST and every GET: four files per
  // book then cost one TCP handshake rather than four.
  hc.keep_alive_enable = true;
  g_http = esp_http_client_init(&hc);
  if (!g_http)
    return false;
  const bool ok = run_sync_();
  esp_http_client_cleanup(g_http);
  g_http = nullptr;
  return ok;
}

inline void sync_task(void*) {
  // Full CPU clock for the whole sync. The UI's performance hold lapses 300 ms
  // after the last press (and buttons are dead during a sync), so without this
  // lwIP, the HTTP client and FATFS all ran at the 80 MHz DFS floor.
  esp_pm_lock_handle_t cpu_lock = nullptr;
  if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "sync", &cpu_lock) == ESP_OK)
    esp_pm_lock_acquire(cpu_lock);

  bool ok = false;
  {
    WifiGuard guard;  // radio down on every path out of this scope
    bool from_cache = false;
    if (bring_up_wifi_(guard) && resolve_server_(g_base_url, true, &from_cache)) {
      ok = sync_once_();
      // **A cached address is a hint, not the answer.** It used to be trusted
      // forever, so once the server's DHCP lease moved every sync failed and
      // nothing ever looked the name up again. run_sync_ fails only when the
      // POST does — download errors are skipped — so retrying is safe.
      if (!ok && from_cache) {
        save_cached_ip_(0);
        if (resolve_server_(g_base_url, false, &from_cache))
          ok = sync_once_();
      }
    }
  }

  if (cpu_lock) {
    esp_pm_lock_release(cpu_lock);
    esp_pm_lock_delete(cpu_lock);
  }

  // The download buffers go back before the end state is visible: poll() on
  // the UI thread returns the spare and only then is the state set, so the
  // repaint that shows "Synced" has its thumbnails again.
  if (g_spare) {
    g_spare_back = true;
    ui_wake();
    xSemaphoreTake(g_spare_ready, portMAX_DELAY);  // poll() gives it once returned
  }
  g_state = ok ? SyncState::Done : SyncState::Failed;
  g_keep_awake = false;
  g_busy = false;
  ui_wake();  // the row shows the result now, not at the next idle deadline
  vTaskDelete(nullptr);
}

// ── Public API ─────────────────────────────────────────────────────────────

inline SyncState state() {
  return g_state;
}

// Wired once at boot from main.cpp. The runtime's start_sync() has no access to
// these, and a sync needs both: the app for the index and paths, the buffer to
// keep off SPI2 while the panel is mid-waveform.
inline void bind(wintergreen::Application& app, wintergreen::DrawBuffer& buf) {
  g_app = &app;
  g_buf = &buf;
}

inline void start() {
  if (g_busy || !g_app || !g_buf)
    return;
  g_busy = true;
  g_state = SyncState::Working;
  g_keep_awake = true;

  // One radio at a time: the clicker's link closes and its controller is
  // disabled, but its stack stays initialised (never deinit — that loses the
  // clicker until reboot), and poll() reconnects it when the sync ends.
  // Nothing is freed: the book index (whose strings the book list is showing)
  // and the reader's page buffer stay. The spare framebuffer is only *lent* to
  // the downloads (acquire_spare_), contents dropped, and comes back at the end.
  g_reconnect_clicker = wg_clicker::begin_sync();
  g_end_pending = true;

  // 6 KB: the worker runs TLS-free HTTP, JSON scanning and FATFS writes.
  if (xTaskCreate(sync_task, "wg_sync", 6144, nullptr, 4, nullptr) != pdPASS) {
    g_state = SyncState::Failed;
    g_keep_awake = false;
    g_busy = false;
  }
}

// Called every UI frame. Holds off auto-sleep while a sync is in flight —
// deep-sleeping mid-transfer would cut the radio with files half written —
// and, once the worker has finished (it ui_wake()s the loop), hands the radio
// back to the clicker, reconnecting it if the sync took it down.
inline void poll() {
  if (g_keep_awake && g_app)
    g_app->keep_awake();
  if (g_spare_want && g_buf) {
    g_spare_want = false;
    g_spare = g_buf->lend_spare();
    xSemaphoreGive(g_spare_ready);
  }
  if (g_spare_back && g_buf) {
    g_spare_back = false;
    g_spare = nullptr;
    g_buf->return_spare();
    xSemaphoreGive(g_spare_ready);
  }
  if (g_end_pending && !g_busy) {
    g_end_pending = false;
    wg_clicker::end_sync(g_reconnect_clicker);
    g_reconnect_clicker = false;
  }
}

// A sync in flight: the main loop keeps its frame cadence.
inline bool active() {
  return g_busy;
}

}  // namespace wg_sync

#else  // !WG_WIFI_SYNC

namespace wintergreen {
class Application;
class DrawBuffer;
}  // namespace wintergreen

namespace wg_sync {

inline SyncState state() {
  return SyncState::Unavailable;
}
inline void bind(wintergreen::Application&, wintergreen::DrawBuffer&) {}
inline void start() {}
inline void poll() {}
inline bool active() {
  return false;
}

}  // namespace wg_sync

#endif  // WG_WIFI_SYNC
