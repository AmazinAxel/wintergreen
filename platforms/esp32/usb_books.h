#pragma once

// Books over USB: the `books` tool writes baked book folders straight to the
// card through the native USB Serial/JTAG port, for when Wi-Fi sync is not an
// option. Upload only: no console, no commands, nothing read back.
//
// Session (host → device, device replies in quotes):
//   "WGBK"                                              → 'R'
//   per book:  'B' [u8 len][folder] [u8 file count]
//     per file:  [u8 len][name] [u32 LE size]           → 'A'
//                data in kChunk pieces, each            → 'A'
//     after the last file: indexed                      → 'O', unreadable book → 'X'
//   'E': index saved                                    → 'O'
//
// Flow control is the per-chunk 'A': the driver's ISR drops whatever does not
// fit its RX ring, so the host never has more than one chunk in flight and the
// ring holds a chunk plus slack.
//
// Event-driven: the receiver task blocks on the driver with no timeout until a
// host writes. Inside a session every read is bounded by kStallMs; that bound is
// the only way out of a session whose host died mid-file (killed `books`,
// unplugged cable), which would otherwise leave every button dead for good.
//
// Same rules as a Wi-Fi sync (wifi_sync.h): files land as .tmp and are renamed
// only once their declared size arrived; wait_panel_idle() before every card
// access (SD shares SPI2); the index is mutated in memory and saved once; all
// buttons are dead and auto-sleep is held off while a session runs.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "driver/usb_serial_jtag.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_wake.h"
#include "wintergreen/Application.h"
#include "wintergreen/content/BookIndex.h"
#include "wintergreen/display/DrawBuffer.h"
#include "wintergreen/screens/MainMenu.h"

namespace wg_usb {

inline constexpr uint8_t kMagic[4] = {'W', 'G', 'B', 'K'};
inline constexpr size_t kChunk = 4096;
inline constexpr uint32_t kStallMs = 5000;

inline wintergreen::Application* g_app = nullptr;
inline wintergreen::DrawBuffer* g_buf = nullptr;
inline volatile bool g_busy = false;

inline bool read_(void* dst, size_t n) {
  auto* p = static_cast<uint8_t*>(dst);
  while (n) {
    const int r = usb_serial_jtag_read_bytes(p, n, pdMS_TO_TICKS(kStallMs));
    if (r <= 0)
      return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

inline void reply_(char c) {
  usb_serial_jtag_write_bytes(&c, 1, pdMS_TO_TICKS(kStallMs));
}

// [u8 len][bytes]: one plain path component, never hidden or a parent.
inline bool read_name_(std::string& out) {
  uint8_t len = 0;
  if (!read_(&len, 1) || len == 0)
    return false;
  out.resize(len);
  if (!read_(out.data(), len))
    return false;
  return out[0] != '.' && out.find('/') == std::string::npos && out.find('\0') == std::string::npos;
}

// One file into dest via dest.tmp. False = the session is lost (stream out of
// step or host gone); the .tmp is removed.
inline bool receive_file_(const std::string& dest, uint8_t* chunk) {
  uint8_t sz[4];
  if (!read_(sz, 4))
    return false;
  uint32_t left = sz[0] | sz[1] << 8 | sz[2] << 16 | static_cast<uint32_t>(sz[3]) << 24;
  const std::string tmp = dest + ".tmp";
  g_buf->wait_panel_idle();
  std::remove(tmp.c_str());
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f)
    return false;
  reply_('A');
  bool ok = true;
  while (ok && left) {
    const size_t n = left < kChunk ? left : kChunk;
    ok = read_(chunk, n);
    if (ok) {
      g_buf->wait_panel_idle();
      ok = std::fwrite(chunk, 1, n, f) == n;
    }
    left -= ok ? n : 0;
    if (ok)
      reply_('A');
  }
  g_buf->wait_panel_idle();
  if (std::fclose(f) != 0)
    ok = false;
  if (!ok) {
    std::remove(tmp.c_str());
    return false;
  }
  // FatFs f_rename does not replace, unlike POSIX: the target must go first.
  std::remove(dest.c_str());
  return std::rename(tmp.c_str(), dest.c_str()) == 0;
}

// One 'B' record. False = the session is lost; a book that arrived whole but
// does not open is answered 'X' and the session goes on.
inline bool receive_book_(uint8_t* chunk, bool& index_changed) {
  using wintergreen::BookIndex;
  std::string dir, name;
  uint8_t files = 0;
  if (!read_name_(dir) || !read_(&files, 1))
    return false;
  const std::string book_dir = std::string(wintergreen::kCardRoot) + "/" + dir;
  g_buf->wait_panel_idle();
  ::mkdir(book_dir.c_str(), 0777);
  for (uint8_t i = 0; i < files; ++i)
    if (!read_name_(name) || !receive_file_(book_dir + "/" + name, chunk))
      return false;

  // Never index a book that does not open (see run_sync_ in wifi_sync.h).
  auto& index = BookIndex::instance();
  const std::string wgb = book_dir + "/book.wgb";
  std::string title, author;
  g_buf->wait_panel_idle();
  if (!wintergreen::read_book_meta(wgb, title, author)) {
    reply_('X');
    return true;
  }
  index.remove_entry(wgb);
  index.add_entry(wgb, title, author);
  index_changed = true;
  reply_('O');
  return true;
}

inline void session_() {
  using wintergreen::BookIndex;
  esp_pm_lock_handle_t cpu_lock = nullptr;
  if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "usb", &cpu_lock) == ESP_OK)
    esp_pm_lock_acquire(cpu_lock);

  auto& index = BookIndex::instance();
  g_buf->wait_panel_idle();
  if (index.entries().empty())
    index.load(g_app->index_path());

  bool index_changed = false;
  auto* chunk = static_cast<uint8_t*>(std::malloc(kChunk));
  if (chunk) {
    reply_('R');
    uint8_t cmd = 0;
    while (read_(&cmd, 1) && cmd == 'B' && receive_book_(chunk, index_changed)) {
    }
    std::free(chunk);
    // Books finished before a lost session are on the card: index them anyway.
    if (index_changed) {
      g_buf->wait_panel_idle();
      index.save(g_app->index_path());
      index.bump_generation();
    }
    if (cmd == 'E')
      reply_('O');
  }

  if (cpu_lock) {
    esp_pm_lock_release(cpu_lock);
    esp_pm_lock_delete(cpu_lock);
  }
}

inline void task_(void*) {
  size_t matched = 0;
  for (;;) {
    uint8_t b = 0;
    if (usb_serial_jtag_read_bytes(&b, 1, portMAX_DELAY) != 1)
      continue;
    matched = b == kMagic[matched] ? matched + 1 : (b == kMagic[0] ? 1 : 0);
    if (matched < sizeof(kMagic))
      continue;
    matched = 0;
    g_busy = true;
    ui_wake();  // poll() starts holding off sleep now
    session_();
    g_busy = false;
    ui_wake();  // the book list repaints with the new books now
  }
}

// Once at boot, after the card is mounted. The ring holds one chunk plus slack
// (the ISR moves 64-byte packets).
inline void start(wintergreen::Application& app, wintergreen::DrawBuffer& buf) {
  g_app = &app;
  g_buf = &buf;
  usb_serial_jtag_driver_config_t cfg = {};
  cfg.tx_buffer_size = 256;
  cfg.rx_buffer_size = kChunk + 1024;
  if (usb_serial_jtag_driver_install(&cfg) != ESP_OK)
    return;
  xTaskCreate(task_, "wg_usb", 4096, nullptr, 4, nullptr);
}

// Every UI frame: no auto-sleep or low-battery cutoff mid-file.
inline void poll() {
  if (g_busy && g_app)
    g_app->keep_awake();
}

inline bool active() {
  return g_busy;
}

}  // namespace wg_usb
