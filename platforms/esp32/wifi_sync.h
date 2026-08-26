#pragma once

// NAS book sync over HTTP.
//
// Pulls newly converted books off the homelab, pushes reading positions, and
// retires books read to 100%. One button, one round trip, radio off the moment
// it is done.
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
//    nothing writes zero bytes: the index save is dirty-guarded, and a .pos is
//    rewritten only where the server was strictly further along.
//
// Compiled out entirely unless WG_WIFI_SYNC is defined, the same way
// WG_BLUETOOTH_PAGE_TURNER gates NimBLE.

#include <cstdint>

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
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wintergreen/Application.h"
#include "wintergreen/content/BookIndex.h"
#include "wintergreen/content/CoverPaths.h"
#include "wintergreen/content/wgb/WgbReader.h"
#include "wintergreen/display/DrawBuffer.h"

namespace wg_sync {

inline volatile SyncState g_state = SyncState::Idle;
inline volatile bool g_busy = false;

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
// One chunk of a book download. 4 KB is a FATFS cluster and a comfortable TCP
// read; the point is that a 1 MB book is never held in RAM.
inline constexpr int kChunk = 4096;

inline int g_retries = 0;

// ── Cached association state ───────────────────────────────────────────────
//
// In NVS rather than the settings file: this is device-local network state, not
// a user setting, and the settings file is deliberately three keys.
//
// The BSSID and channel skip a full scan across every channel on reconnect,
// which is 1-2 s of radio at full draw. The IP skips mDNS, which is multicast
// and slower still. All three are hints — any failure falls back to the slow
// path and re-caches.

inline constexpr const char* kNvsNamespace = "wgsync";

struct NetHint {
  uint8_t bssid[6] = {};
  uint8_t channel = 0;
  uint32_t ip = 0;
  bool valid = false;
};

inline NetHint load_hint_() {
  NetHint h;
  nvs_handle_t nvs;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK)
    return h;
  size_t len = sizeof(h.bssid);
  const bool got_bssid = nvs_get_blob(nvs, "bssid", h.bssid, &len) == ESP_OK && len == sizeof(h.bssid);
  nvs_get_u8(nvs, "chan", &h.channel);
  nvs_get_u32(nvs, "ip", &h.ip);
  nvs_close(nvs);
  h.valid = got_bssid;
  return h;
}

inline void save_hint_(const NetHint& h) {
  nvs_handle_t nvs;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK)
    return;
  nvs_set_blob(nvs, "bssid", h.bssid, sizeof(h.bssid));
  nvs_set_u8(nvs, "chan", h.channel);
  nvs_set_u32(nvs, "ip", h.ip);
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
    ++g_retries;
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    g_retries = 0;
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
// The response has three known keys holding arrays of strings and a flat object
// of 4-element integer arrays. A JSON library for that is a dependency to avoid;
// these are ~40 lines and only ever see output from our own server.

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

struct PosEntry {
  std::string dir;
  uint32_t v[4] = {};
};

// {"name":[a,b,c,d], ...}
inline std::vector<PosEntry> parse_pos_object_(const std::string& s, size_t pos) {
  std::vector<PosEntry> out;
  if (pos == std::string::npos)
    return out;
  const size_t open = s.find('{', pos);
  if (open == std::string::npos)
    return out;
  size_t i = open + 1;
  while (i < s.size() && s[i] != '}') {
    if (s[i] != '"') { ++i; continue; }
    PosEntry e;
    for (++i; i < s.size() && s[i] != '"'; ++i) {
      if (s[i] == '\\' && i + 1 < s.size())
        ++i;
      e.dir.push_back(s[i]);
    }
    ++i;
    const size_t lb = s.find('[', i);
    if (lb == std::string::npos)
      break;
    int n = 0;
    i = lb + 1;
    for (; i < s.size() && s[i] != ']' && n < 4; ++i) {
      if (s[i] == ' ' || s[i] == ',')
        continue;
      e.v[n++] = static_cast<uint32_t>(std::strtoul(s.c_str() + i, nullptr, 10));
      while (i < s.size() && s[i] != ',' && s[i] != ']')
        ++i;
      --i;
    }
    const size_t rb = s.find(']', i);
    i = (rb == std::string::npos) ? s.size() : rb + 1;
    if (n == 4)
      out.push_back(std::move(e));
  }
  return out;
}

// ── Reading position files ─────────────────────────────────────────────────
//
// Same four numbers and the same format ReaderScreen::save_position_ writes, so
// the two paths cannot drift apart.

inline bool read_pos_(const std::string& path, uint32_t out[4]) {
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f)
    return false;
  unsigned v[4] = {};
  const int n = std::fscanf(f, "%u %u %u %u", &v[0], &v[1], &v[2], &v[3]);
  std::fclose(f);
  for (int i = 0; i < 4; ++i)
    out[i] = v[i];
  return n >= 3;
}

inline bool write_pos_(const std::string& path, const uint32_t v[4]) {
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f)
    return false;
  std::fprintf(f, "%u %u %u %u\n", static_cast<unsigned>(v[0]), static_cast<unsigned>(v[1]),
               static_cast<unsigned>(v[2]), static_cast<unsigned>(v[3]));
  return std::fclose(f) == 0;
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

  std::vector<char> buf(kChunk);
  bool ok = true;
  int64_t written = 0;
  int r;
  while ((r = esp_http_client_read(g_http, buf.data(), kChunk)) > 0) {
    if (std::fwrite(buf.data(), 1, static_cast<size_t>(r), f) != static_cast<size_t>(r)) {
      ok = false;
      break;
    }
    written += r;
  }
  if (r < 0)
    ok = false;
  if (std::fclose(f) != 0)
    ok = false;
  esp_http_client_close(g_http);

  // **A 200 is not proof this is the file we asked for.** The server's router
  // falls through to an HTML page for any unmatched path, so a routing mistake
  // arrives as 200 + Content-Length + a body — and the first version wrote that
  // dashboard HTML to the card as book.wgb, four times, identically sized.
  // Insisting the body match the declared length catches a truncated transfer;
  // the reader's WGB2 magic check catches the rest.
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

// The last path segment: BookIndex stores full paths, the protocol keys on the
// book's directory name.
inline std::string dir_of_(const std::string& book_path) {
  const size_t last = book_path.find_last_of('/');
  // No directory component ("book.wgb"), or the file sits at the root
  // ("/book.wgb") so there is no book folder to name. Callers skip an empty
  // result; relying on `last - 1` wrapping to npos here would work by accident.
  if (last == std::string::npos || last == 0)
    return {};
  const size_t prev = book_path.find_last_of('/', last - 1);
  const size_t start = (prev == std::string::npos) ? 0 : prev + 1;
  return book_path.substr(start, last - start);
}

// ── The sync itself ────────────────────────────────────────────────────────

inline bool run_sync_() {
  using wintergreen::BookIndex;

  const char* books_dir = g_app->main_menu()->books_dir();
  // data_dir_ is not used directly any more (cover/pos paths derive from the
  // book), but index_path() is built from it, so a null root is still fatal.
  if (!books_dir || !g_app->data_dir_)
    return false;

  auto& index = BookIndex::instance();
  if (index.entries().empty())
    index.load(g_app->index_path());

  // One pass over the index builds all three request fields. Each book's
  // position comes from its own .pos, the same file the reader reads.
  std::string req = "{\"have\":[";
  std::string pos_json = "\"pos\":{";
  std::string done_json = "\"done\":[";
  bool first_have = true, first_pos = true, first_done = true;

  for (const auto& e : index.entries()) {
    const std::string path(e.path.view(index.pool()));
    const std::string dir = dir_of_(path);
    if (dir.empty())
      continue;

    if (!first_have)
      req.push_back(',');
    first_have = false;
    append_escaped_(req, dir);

    uint32_t p[4];
    if (read_pos_(wintergreen::book_pos_path(path.c_str()), p)) {
      if (!first_pos)
        pos_json.push_back(',');
      first_pos = false;
      append_escaped_(pos_json, dir);
      char nums[64];
      std::snprintf(nums, sizeof(nums), ":[%lu,%lu,%lu,%lu]", static_cast<unsigned long>(p[0]),
                    static_cast<unsigned long>(p[1]), static_cast<unsigned long>(p[2]),
                    static_cast<unsigned long>(p[3]));
      pos_json += nums;
    }

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
  req += pos_json + "},";
  req += done_json + "]}";

  std::string resp;
  if (!http_post_(req, resp))
    return false;

  const std::vector<std::string> get = parse_string_array_(resp, find_key_(resp, "get"));
  const std::vector<std::string> del = parse_string_array_(resp, find_key_(resp, "delete"));
  const std::vector<PosEntry> newpos = parse_pos_object_(resp, find_key_(resp, "pos"));

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
    // and every consumer already falls back.
    for (const char* cover : {"cover.bin", "cover_home.bin", "cover_sleep.bin"})
      http_get_file_("/booksync/" + url_dir + "/" + cover, book_dir + "/" + cover);

    // Index it in memory only. index_file() would save the whole ~30 KB index
    // per book; one save at the end covers every mutation.
    //
    // **A book that does not open is never indexed.** WgbReader::open checks the
    // WGB2 magic, so this is the backstop that catches anything the transfer
    // checks missed. The first version ignored the result and indexed the book
    // anyway with the folder name as its title — so a bad download appeared in
    // the library looking perfectly normal and simply failed to open. Deleting
    // it here means the next sync re-offers it, since the device stops claiming
    // to have it.
    std::string title, author;
    bool readable = false;
    {
      wintergreen::WgbReader r;
      if (r.open(wgb.c_str())) {
        readable = true;
        title = r.metadata().title;
        author = r.metadata().author.value_or("");
        r.close();
      }
    }
    if (!readable) {
      std::remove(wgb.c_str());
      for (const char* cover : {"cover.bin", "cover_home.bin", "cover_sleep.bin"})
        std::remove((book_dir + "/" + cover).c_str());
      ::rmdir(book_dir.c_str());
      continue;
    }
    if (title.empty() || title == "none")
      title = dir;
    index.remove_entry(wgb);
    index.add_entry(wgb, title, author);
    index_changed = true;
  }

  // 2. Adopt the positions the server sent: the ones it is strictly ahead on,
  // plus every book downloaded above. Runs *after* the downloads so those book
  // directories already exist — a restored device resumes where it left off
  // instead of starting every book at page one.
  //
  // The server only sends what actually moved, so every write here is a file
  // that genuinely changed.
  const std::string open_book = g_app->reader() ? g_app->reader()->get_path() : std::string();
  for (const PosEntry& e : newpos) {
    const std::string wgb = std::string(books_dir) + "/" + e.dir + "/book.wgb";
    // Never touch the open book: ReaderScreen::stop() writes .pos on close and
    // would put the stale in-memory position straight back over this.
    if (!open_book.empty() && open_book == wgb)
      continue;
    if (g_buf)
      g_buf->wait_panel_idle();
    write_pos_(wintergreen::book_pos_path(wgb.c_str()), e.v);
  }

  // 3. Remove what the server confirmed it has recorded.
  for (const std::string& dir : del) {
    const std::string book_dir = std::string(books_dir) + "/" + dir;
    if (g_buf)
      g_buf->wait_panel_idle();
    for (const char* f : {"book.wgb", "book.pos", "cover.bin", "cover_home.bin", "cover_sleep.bin"})
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

// ── Temporary failure diagnostics ──────────────────────────────────────────
//
// Every failure here looks identical from the UI: the row flicks back to
// "Sync". That is the right end state (see the clicker's "failure codes are
// gone" note in CLAUDE.md) but it is useless while bringing the feature up, so
// the stage is recorded and shown after "Sync" until this is confirmed working.
//
// **Remove this whole block once sync is proven on hardware**, exactly as the
// clicker's FailStage was removed.
enum class FailStage : uint8_t {
  None = 0,
  NvsInit,
  NetifInit,
  EventGroup,
  NetifCreate,
  WifiInit,     // the likely one: internal RAM
  HandlerReg,
  SetMode,
  SetConfig,
  WifiStart,
  ConnectTimeout,  // associated never happened — wrong password, wrong SSID, out of range
  Resolve,
  HttpInit,
  Post,
};
inline volatile FailStage g_fail = FailStage::None;
inline volatile uint32_t g_fail_heap_kb = 0;

inline bool fail_(FailStage s) {
  g_fail = s;
  g_fail_heap_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
  return false;
}

inline bool bring_up_wifi_(WifiGuard& guard) {
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs = nvs_flash_init();
  }
  if (nvs != ESP_OK)
    return fail_(FailStage::NvsInit);

  if (esp_netif_init() != ESP_OK)
    return fail_(FailStage::NetifInit);
  // Both are idempotent-ish: already-created is not an error worth failing on,
  // since a second sync in the same boot re-enters here.
  esp_event_loop_create_default();

  g_wifi_events = xEventGroupCreate();
  if (!g_wifi_events)
    return fail_(FailStage::EventGroup);

  guard.netif = esp_netif_create_default_wifi_sta();
  if (!guard.netif)
    return fail_(FailStage::NetifCreate);

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&cfg) != ESP_OK)
    return fail_(FailStage::WifiInit);

  if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_, nullptr,
                                          &guard.any_wifi) != ESP_OK ||
      esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_, nullptr,
                                          &guard.got_ip) != ESP_OK)
    return fail_(FailStage::HandlerReg);

  wifi_config_t wc = {};
  std::snprintf(reinterpret_cast<char*>(wc.sta.ssid), sizeof(wc.sta.ssid), "%s", wintergreen::config::kWifiName);
  std::snprintf(reinterpret_cast<char*>(wc.sta.password), sizeof(wc.sta.password), "%s",
                wintergreen::config::kWifiPassword);

  // **The BSSID/channel pin is not applied on the first attempt.**
  //
  // It was, and it is a trap: a pinned BSSID that has gone stale (the AP moved
  // channel, or you are near a different one in a mesh) fails to associate, and
  // the retry in wifi_event_ just calls esp_wifi_connect() again against the
  // *same* pinned config — so every retry fails identically and the sync dies
  // without ever trying a normal scan. Saving 1-2 s of scan is not worth a
  // failure mode that looks exactly like a wrong password.
  //
  // The hint is still cached and still used, but only as a *fallback* ordering
  // hint by esp_wifi's own scan, which is where it belongs.

  g_retries = 0;
  if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK)
    return fail_(FailStage::SetMode);
  // Credentials live in the firmware, so there is nothing to persist — and this
  // avoids an NVS write on every sync.
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK)
    return fail_(FailStage::SetConfig);
  if (esp_wifi_start() != ESP_OK)
    return fail_(FailStage::WifiStart);

  // Power save OFF, deliberately. It saves current while associated and *idle*,
  // which this workload never is: connect, transfer flat out, leave. MIN_MODEM
  // would park the radio between DTIM beacons and add latency to every round
  // trip, extending total radio-on time — the opposite of the goal.
  esp_wifi_set_ps(WIFI_PS_NONE);

  // Wait for the *timeout*, not for the retry counter. An association failure
  // can come back in milliseconds (bad password, AP busy), so a small retry
  // budget is spent almost instantly and the sync gives up while the radio has
  // barely tried — user-visible as "Syncing..." flicking straight back to
  // "Sync". kFailedBit is therefore not waited on here: wifi_event_ keeps
  // reconnecting for as long as this timeout allows.
  const EventBits_t bits = xEventGroupWaitBits(g_wifi_events, kConnectedBit, pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(kConnectTimeoutMs));
  if (!(bits & kConnectedBit))
    return fail_(FailStage::ConnectTimeout);

  // Cache what actually worked, for resolve_server_ and as a scan hint.
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    NetHint h = load_hint_();
    std::memcpy(h.bssid, ap.bssid, 6);
    h.channel = ap.primary;
    h.valid = true;
    save_hint_(h);
  }
  return true;
}

// Resolve the server, preferring the cached address.
//
// **No mDNS.** `esp_mdns` is a managed component, not part of IDF, so using it
// would mean a component-manager fetch at build time — a real dependency for
// something the cache covers in the steady state. Instead: cached IP first,
// then lwIP's own resolver (which works whenever the router serves DNS for its
// DHCP leases, as most do), and `syncServer` may simply be written as a literal
// IP to skip the question entirely.
inline bool resolve_server_(std::string& url_out) {
  const NetHint hint = load_hint_();
  if (hint.ip) {
    esp_ip4_addr_t a;
    a.addr = hint.ip;
    char ip[16];
    esp_ip4addr_ntoa(&a, ip, sizeof(ip));
    url_out = std::string("http://") + ip;
    return true;
  }

  // A literal IP in the config needs no lookup at all.
  ip_addr_t parsed;
  if (ipaddr_aton(wintergreen::config::syncServer, &parsed) && IP_IS_V4(&parsed)) {
    url_out = std::string("http://") + wintergreen::config::syncServer;
    NetHint h = hint;
    h.ip = ip_2_ip4(&parsed)->addr;
    save_hint_(h);
    return true;
  }

  // IPv4 only: lwIP is built without IPv6 here, and the panel has no use for a
  // AAAA record anyway.
  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  if (getaddrinfo(wintergreen::config::syncServer, "80", &hints, &res) != 0 || !res)
    return false;

  const uint32_t addr = reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
  freeaddrinfo(res);

  esp_ip4_addr_t a;
  a.addr = addr;
  char ip[16];
  esp_ip4addr_ntoa(&a, ip, sizeof(ip));
  url_out = std::string("http://") + ip;

  NetHint h = hint;
  h.ip = addr;
  save_hint_(h);
  return true;
}

inline void sync_task(void*) {
  bool ok = false;
  {
    WifiGuard guard;  // radio down on every path out of this scope
    if (!bring_up_wifi_(guard)) {
      // g_fail stays where bring_up_wifi_ left it
    } else if (!resolve_server_(g_base_url)) {
      g_fail = FailStage::Resolve;
    } else {
      esp_http_client_config_t hc = {};
      hc.url = g_base_url.c_str();
      hc.timeout_ms = kHttpTimeoutMs;
      // One handle, kept alive across the POST and every GET: four files per
      // book then cost one TCP handshake rather than four.
      hc.keep_alive_enable = true;
      g_http = esp_http_client_init(&hc);
      if (!g_http) {
        g_fail = FailStage::HttpInit;
      } else {
        ok = run_sync_();
        if (!ok && g_fail == FailStage::None)
          g_fail = FailStage::Post;
        esp_http_client_cleanup(g_http);
        g_http = nullptr;
      }
    }
  }

  // After the WifiGuard has run, so the radio's memory is back first.
  g_buf->reacquire_spare();

  g_state = ok ? SyncState::Done : SyncState::Failed;
  g_keep_awake = false;
  g_busy = false;
  vTaskDelete(nullptr);
}

// ── Public API ─────────────────────────────────────────────────────────────

inline SyncState state() {
  return g_state;
}

// TEMPORARY — see FailStage. Stage of the last failure, and free internal RAM
// at that moment. Remove along with FailStage once sync is confirmed.
inline uint8_t fail_stage() {
  return static_cast<uint8_t>(g_fail);
}
inline uint32_t fail_heap_kb() {
  return g_fail_heap_kb;
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

  // Both radios cannot fit. The clicker goes down first, and stays down for the
  // session unless its quick-menu row is pressed again.
  //
  // release_for_wifi(), not radio_off(): the latter disables the controller but
  // leaves it initialised and holding its buffers, which left esp_wifi_init()
  // ~18 KB short of the ~50 KB it wants. The sync then failed immediately
  // ("Fail 5, 18k") *and* left the heap too full to open a book afterwards —
  // WgbReader::open aborted allocating an 8 KB StringPool chunk for the TOC.
  wg_clicker::release_for_wifi();

  // **The book index is deliberately NOT released here.**
  //
  // It was, and it did two bad things at once. `MainMenu::entries_` holds
  // `StringRef`s into the index's `StringPool`, and this screen is *on display*
  // when Sync is pressed — so dropping the pool left every title and author
  // resolving to an empty `string_view` (`StringPool::get` walks an empty chunk
  // list and returns `{}`). The list stayed navigable and kept showing reading
  // percentages, because those are `uint8_t` copies, which made it look like a
  // drawing bug rather than a dangling reference.
  //
  // And it bought nothing: `run_sync_()` reloads the index from disk as its
  // first act, so the memory came straight back before Wi-Fi ever started.
  //
  // The clicker's own release (QuickmenuScreen) is unaffected — that runs from
  // a screen that holds no `StringRef`s.

  // **This is what actually makes room for the radio.** Measured on hardware:
  // esp_wifi_init() failed with 20 KB of internal RAM free, because two static
  // framebuffers are 96 KB of BSS and the spare is another 48 KB of heap. Wi-Fi
  // wants ~50 KB. Handing the spare back covers it.
  //
  // Costs nothing but latency while the sync runs: without the spare a quick
  // menu dismiss re-renders and a page turn re-draws, and the sync screen is
  // doing neither.
  g_buf->release_spare();

  // 6 KB: the worker runs TLS-free HTTP, JSON scanning and FATFS writes.
  if (xTaskCreate(sync_task, "wg_sync", 6144, nullptr, 4, nullptr) != pdPASS) {
    g_buf->reacquire_spare();  // the task that would have restored it never ran
    g_state = SyncState::Failed;
    g_keep_awake = false;
    g_busy = false;
  }
}

// Called every UI frame. Holds off auto-sleep while a sync is in flight —
// deep-sleeping mid-transfer would cut the radio with files half written.
inline void poll() {
  if (g_keep_awake && g_app)
    g_app->keep_awake();
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
inline uint8_t fail_stage() {
  return 0;
}
inline uint32_t fail_heap_kb() {
  return 0;
}
inline void bind(wintergreen::Application&, wintergreen::DrawBuffer&) {}
inline void start() {}
inline void poll() {}

}  // namespace wg_sync

#endif  // WG_WIFI_SYNC
