#pragma once

// Receives frames over USB Serial/JTAG and dispatches them.
//
// Frame types (matched by magic prefix):
//   "EPUB"      file upload   → /sdcard/books/
//   "CMND"      command frame → see handle_serial_cmd()
//
// File upload format (EPUB):
//   [4B] magic
//   [2B LE] filename length
//   [N B]   filename
//   [4B LE] payload size
//   [data]  in 2 KB chunks; each chunk ACKed with 0x06
//   [4B LE] CRC-32 of full payload
//   Response: "READY\n" → 0x06 per chunk → "OK\n" or "ERR:...\n"
//
// The receiver task is started lazily by serial_start_if_connected(), which the
// main loop calls the first time a USB host appears.

#include <atomic>
#include <dirent.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "wintergreen/content/BookIndex.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr uint8_t kEpubMagic[4] = {'E', 'P', 'U', 'B'};
static constexpr uint8_t kCmdMagic[4] = {'C', 'M', 'N', 'D'};
static constexpr uint8_t kAck = 0x06;           // flow-control ACK between chunks


// Button injection: OR'd into next poll_buttons before clearing.
volatile uint8_t g_serial_buttons = 0;

// Single-slot command queue: only one path command can be pending at a time.
// The serial receiver task writes path then sets type as the commit signal.
// The main loop reads type, dispatches, then clears to None.
enum class SerialCmdType : uint8_t {
  None = 0,
  Open
};
static char g_cmd_path[256];
static volatile SerialCmdType g_cmd_type = SerialCmdType::None;

// Single-slot SPSC queue for index mutations triggered by serial commands
// (upload via 'W' or the EPUB magic, delete via 'R', rename via 'N'). The
// receiver task is the producer; the main loop is the consumer. Single slot is
// enough because the host always waits for the "OK\n" response between
// operations, and the main loop dequeues before processing (so the slot is free
// quickly). If a second op arrives while the slot is still occupied, it is
// dropped — the file on SD is unchanged, only the index entry is missed until
// the next library scan.
//
// Memory ordering: producer writes path_a/path_b THEN sets g_index_op (commit).
// Consumer reads g_index_op, copies paths to locals, THEN clears g_index_op.
// Volatile is sufficient on ESP32 (32-bit atomic reads/writes) and matches
// the pattern already used by g_cmd_type/g_cmd_path above.
enum class SerialIndexOp : uint8_t { None, Add, Remove, Rename };
static volatile SerialIndexOp g_index_op = SerialIndexOp::None;
static char g_index_path_a[256];  // Add/Remove: the path. Rename: src.
static char g_index_path_b[256];  // Rename: dst.

inline void request_index_op(SerialIndexOp op, const char* a, const char* b = nullptr) {
  if (g_index_op != SerialIndexOp::None) {
    return;  // drop
  }
  if (a) {
    strncpy(g_index_path_a, a, sizeof(g_index_path_a) - 1);
    g_index_path_a[sizeof(g_index_path_a) - 1] = '\0';
  }
  if (b) {
    strncpy(g_index_path_b, b, sizeof(g_index_path_b) - 1);
    g_index_path_b[sizeof(g_index_path_b) - 1] = '\0';
  }
  // volatile orders only against other volatiles: without the fence the
  // compiler may sink the path stores below the commit.
  std::atomic_thread_fence(std::memory_order_release);
  g_index_op = op;  // commit
}

// Set while a chunked file transfer (EPUB, 'W', 'T') is in progress. The main
// loop skips app.update() when this is true to prevent display SPI traffic
// (SPI2_HOST) from contending with SD-card I/O (also SPI2_HOST).
static volatile bool g_upload_in_progress = false;


// Call from the main loop. Returns the command type and sets *path_out to the
// path string. Returns None (and leaves *path_out unchanged) if nothing pending.
// Clears the pending state before returning.
inline SerialCmdType serial_cmd_take(const char** path_out) {
  SerialCmdType t = g_cmd_type;
  if (t == SerialCmdType::None)
    return SerialCmdType::None;
  if (path_out)
    *path_out = g_cmd_path;
  g_cmd_type = SerialCmdType::None;
  return t;
}

// Read exactly `n` bytes with a timeout. Returns true on success.
static bool serial_read_exact(uint8_t* buf, size_t n, uint32_t timeout_ms) {
  size_t received = 0;
  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  while (received < n) {
    const TickType_t now = xTaskGetTickCount();
    if ((int32_t)(deadline - now) <= 0)
      return false;
    const int r = usb_serial_jtag_read_bytes(buf + received, n - received, deadline - now);
    if (r > 0)
      received += r;
  }
  return true;
}

static void serial_write(const char* msg) {
  usb_serial_jtag_write_bytes((const uint8_t*)msg, strlen(msg), pdMS_TO_TICKS(1000));
}

static void serial_write_raw(const uint8_t* buf, size_t n) {
  usb_serial_jtag_write_bytes(buf, n, pdMS_TO_TICKS(1000));
}

static uint32_t read_le32_(const uint8_t* b) {
  return b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

// Receive `size` bytes into `path`: "READY\n", then 2 KB chunks each ACKed with
// 0x06, then a CRC-32 trailer. On failure writes the ERR: line itself and
// removes the partial file; on success the caller sends "OK\n".
static bool receive_file_(const char* path, uint32_t size) {
  FILE* f = fopen(path, "wb");
  if (!f) {
    serial_write("ERR:fopen\n");
    return false;
  }
  serial_write("READY\n");
  g_upload_in_progress = true;

  const char* err = nullptr;
  uint32_t crc = 0;
  uint32_t remaining = size;
  uint8_t chunk[2048];
  while (remaining > 0) {
    const size_t want = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
    if (!serial_read_exact(chunk, want, 30000)) {
      err = "ERR:timeout\n";
      break;
    }
    fwrite(chunk, 1, want, f);
    crc = esp_rom_crc32_le(crc, chunk, want);
    remaining -= want;
    serial_write_raw(&kAck, 1);
  }
  fclose(f);
  if (!err) {
    uint8_t crc_buf[4];
    if (!serial_read_exact(crc_buf, 4, 2000))
      err = "ERR:crc_missing\n";
    else if (crc != read_le32_(crc_buf))
      err = "ERR:crc\n";
  }
  g_upload_in_progress = false;
  if (err) {
    remove(path);
    serial_write(err);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Handle an incoming upload to a specific directory (after magic matched).
// ---------------------------------------------------------------------------
static void handle_file_upload(const char* target_dir) {
  // Read filename length (2 bytes LE).
  uint8_t hdr[2];
  if (!serial_read_exact(hdr, 2, 2000)) {
    serial_write("ERR:header\n");
    return;
  }
  uint16_t name_len = hdr[0] | (hdr[1] << 8);
  if (name_len == 0 || name_len > 200) {
    serial_write("ERR:name_len\n");
    return;
  }

  // Read filename.
  char name[204];
  if (!serial_read_exact((uint8_t*)name, name_len, 2000)) {
    serial_write("ERR:name\n");
    return;
  }
  name[name_len] = '\0';

  // Read file size (4 bytes LE).
  uint8_t sz_buf[4];
  if (!serial_read_exact(sz_buf, 4, 2000)) {
    serial_write("ERR:size\n");
    return;
  }
  const uint32_t file_size = read_le32_(sz_buf);

  // Build path
  char path[256];
  snprintf(path, sizeof(path), "%s/%s", target_dir, name);
  mkdir(target_dir, 0775);

  if (!receive_file_(path, file_size))
    return;
  // Goes through request_index_op instead of touching BookIndex directly from
  // this receiver task. The main loop will call index_file(), which (via
  // ensure_loaded_) merges with the existing on-disk index.
  request_index_op(SerialIndexOp::Add, path);
  serial_write("OK\n");
}


// ---------------------------------------------------------------------------
// Handle a serial command (after "CMND" magic has been matched).
//
// This is a file-management channel only. There is no debug console: every log
// line, heap/state query and benchmark command was removed, so nothing here
// prints unsolicited output and the host never has to filter it.
//
// Sub-commands (1 byte after magic). Keep this list in step with the switch —
// it drifted once already, advertising commands that had been deleted.
//   'A' + 2B path_len + path  → dir listing: "DIR:<p>\n" + "d|name\n" /
//                                "f|name|size|mtime\n" lines + "END\n"
//   'B' + 1B mask             → inject button press(es)
//   'K' + 2B path_len + path  → mkdir
//   'L'                       → list books in /sdcard/books/ ("BOOKS:\n" … "END\n")
//   'N' + 2B src_len + src
//       + 2B dst_len + dst    → rename / move
//   'O' + 2B path_len + path  → open book
//   'R' + 2B path_len + path  → recursive delete
//   'T' + 2B path_len + path  → read file: "READY\n" + 4B size + [2KB chunks, 0x06 ACK each] + 4B CRC32
//   'W' + 2B path_len + path
//       + 4B size + data
//       + 4B CRC32            → write file (chunked + 0x06 ACKs)
// ---------------------------------------------------------------------------

// Read a 2-byte LE path length followed by the path bytes into g_cmd_path.
// Sends an ERR: response and returns false on any failure.
static bool read_cmd_path() {
  uint8_t len_buf[2];
  if (!serial_read_exact(len_buf, 2, 1000)) {
    serial_write("ERR:path_len\n");
    return false;
  }
  uint16_t path_len = len_buf[0] | (len_buf[1] << 8);
  if (path_len == 0 || path_len >= sizeof(g_cmd_path)) {
    serial_write("ERR:path_too_long\n");
    return false;
  }
  if (!serial_read_exact((uint8_t*)g_cmd_path, path_len, 2000)) {
    serial_write("ERR:path_read\n");
    return false;
  }
  g_cmd_path[path_len] = '\0';
  return true;
}

// Recursively delete a file or directory tree. Best-effort: deletes as much as
// possible, does not abort on partial failures.
static void remove_recursive(const char* path) {
  DIR* d = opendir(path);
  if (!d) {
    // Not a directory (or doesn't exist) — try plain remove.
    remove(path);
    return;
  }
  struct dirent* ent;
  char child[300];
  while ((ent = readdir(d)) != nullptr) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
    snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
    if (ent->d_type == DT_DIR) {
      remove_recursive(child);
    } else {
      remove(child);
    }
  }
  closedir(d);
  rmdir(path);
}

static void handle_serial_cmd() {
  uint8_t sub;
  if (!serial_read_exact(&sub, 1, 1000)) {
    return;
  }

  switch (sub) {
    case 'B': {
      uint8_t mask;
      if (!serial_read_exact(&mask, 1, 500)) {
        serial_write("ERR:btn_read\n");
        return;
      }
      g_serial_buttons |= mask;
      serial_write("OK\n");
      break;
    }
    case 'O': {
      if (!read_cmd_path())
        return;
      g_cmd_type = SerialCmdType::Open;
      serial_write("OK\n");
      break;
    }
    case 'L': {
      // Response format: "path|title|author|size|mtime\n" per book.
      //
      // Read from the index file, never from BookIndex::instance(): this is the
      // serial task, and the main loop mutates the in-memory index (sync, reader
      // close, the index ops above) without a lock. Every path/title/author
      // change is saved before its op completes, so the file is current for the
      // three fields this reports.
      serial_write("BOOKS:\n");
      FILE* fidx = fopen("/sdcard/.wintergreen-index", "r");
      if (fidx) {
        char line[1024];
        while (fgets(line, sizeof(line), fidx)) {
          size_t len = strlen(line);
          while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
          if (len == 0 || line[0] == '#')
            continue;
          // Keep only the first 3 fields: path|title|author.
          int fields = 0;
          char* path_end = nullptr;
          for (char* p = line; *p; ++p) {
            if (*p == '|') {
              ++fields;
              if (fields == 1) path_end = p;
              if (fields == 3) { *p = '\0'; break; }
            }
          }
          if (!path_end)
            continue;
          char fpath[256];
          snprintf(fpath, sizeof(fpath), "%.*s", static_cast<int>(path_end - line), line);
          struct stat lst = {};
          stat(fpath, &lst);
          char out[1060];
          snprintf(out, sizeof(out), "%s|%lu|%lu\n", line, (unsigned long)lst.st_size, (unsigned long)lst.st_mtime);
          serial_write(out);
        }
        fclose(fidx);
      }
      serial_write("END\n");
      break;
    }
    case 'R': {
      if (!read_cmd_path())
        return;
      remove_recursive(g_cmd_path);
      struct stat rm_st;
      if (stat(g_cmd_path, &rm_st) != 0) {
        // Schedule index update via the main loop instead of mutating BookIndex
        // here. This avoids a data race with MainMenu::update() (which reads
        // entries_ from the main loop) and ensures ensure_loaded_() runs in
        // the right thread. Non-book paths (e.g. cache files) are ignored.
        if (wintergreen::BookIndex::is_wgb_path(g_cmd_path)) {
          request_index_op(SerialIndexOp::Remove, g_cmd_path);
        }
        serial_write("OK\n");
      } else {
        serial_write("ERR:remove_failed\n");
      }
      break;
    }
    case 'A': {
      // Directory listing: responds with "DIR:<path>\n", then per-entry lines, then "END\n".
      // File entries: "f|name|size_bytes|mtime_unix\n"
      // Dir entries:  "d|name\n"
      if (!read_cmd_path())
        return;
      DIR* ldir = opendir(g_cmd_path);
      if (!ldir) {
        serial_write("ERR:opendir\n");
        return;
      }
      char lline[400];
      snprintf(lline, sizeof(lline), "DIR:%s\n", g_cmd_path);
      serial_write(lline);
      struct dirent* lent;
      while ((lent = readdir(ldir)) != nullptr) {
        if (strcmp(lent->d_name, ".") == 0 || strcmp(lent->d_name, "..") == 0)
          continue;
        char lfull[300];
        snprintf(lfull, sizeof(lfull), "%s/%s", g_cmd_path, lent->d_name);
        struct stat lst;
        bool lis_dir = (lent->d_type == DT_DIR);
        unsigned long lsize = 0;
        long lmtime = 0;
        if (stat(lfull, &lst) == 0) {
          lis_dir = S_ISDIR(lst.st_mode);
          lsize = (unsigned long)lst.st_size;
          lmtime = (long)lst.st_mtime;
        }
        if (lis_dir) {
          snprintf(lline, sizeof(lline), "d|%s\n", lent->d_name);
        } else {
          snprintf(lline, sizeof(lline), "f|%s|%lu|%ld\n", lent->d_name, lsize, lmtime);
        }
        serial_write(lline);
      }
      closedir(ldir);
      serial_write("END\n");
      break;
    }
    case 'W': {
      // Write file to arbitrary /sdcard/ path.
      // Format after 'W': 2-byte LE path_len + path + 4-byte LE size + [2KB chunks + 0x06 ACK] + 4-byte CRC32
      if (!read_cmd_path())
        return;
      if (strncmp(g_cmd_path, "/sdcard/", 8) != 0) {
        serial_write("ERR:invalid_path\n");
        return;
      }
      uint8_t wsz[4];
      if (!serial_read_exact(wsz, 4, 2000)) { serial_write("ERR:size\n"); return; }
      if (!receive_file_(g_cmd_path, read_le32_(wsz)))
        return;
      serial_write("OK\n");
      // A book written anywhere under /sdcard/ is indexed via the main loop, so
      // it appears in the list without a rescan.
      if (wintergreen::BookIndex::is_wgb_path(g_cmd_path)) {
        request_index_op(SerialIndexOp::Add, g_cmd_path);
      }
      break;
    }
    case 'K': {
      // Make directory: 'K' + 2-byte LE path_len + path
      if (!read_cmd_path())
        return;
      if (mkdir(g_cmd_path, 0775) == 0 || errno == EEXIST) {
        serial_write("OK\n");
      } else {
        serial_write("ERR:mkdir_failed\n");
      }
      break;
    }
    case 'N': {
      // Rename/move: 'N' + 2-byte LE src_len + src + 2-byte LE dst_len + dst
      if (!read_cmd_path())
        return;
      char nsrc[256];
      strncpy(nsrc, g_cmd_path, sizeof(nsrc) - 1);
      nsrc[sizeof(nsrc) - 1] = '\0';
      if (!read_cmd_path())
        return;
      if (rename(nsrc, g_cmd_path) == 0) {
        // Update the book index based on what changed. Three cases:
        //   src book + dst book  → Rename (preserves metadata + last_open_order)
        //   src book + dst non   → Remove (file is no longer a book)
        //   src non  + dst book  → Add    (file became a book, e.g. .tmp → .wgb)
        const bool src_is_book = wintergreen::BookIndex::is_wgb_path(nsrc);
        const bool dst_is_book = wintergreen::BookIndex::is_wgb_path(g_cmd_path);
        if (src_is_book && dst_is_book) {
          request_index_op(SerialIndexOp::Rename, nsrc, g_cmd_path);
        } else if (src_is_book) {
          request_index_op(SerialIndexOp::Remove, nsrc);
        } else if (dst_is_book) {
          request_index_op(SerialIndexOp::Add, g_cmd_path);
        }
        serial_write("OK\n");
      } else {
        serial_write("ERR:rename_failed\n");
      }
      break;
    }
    case 'T': {
      // Read file: host receives "READY\n" + 4B size LE + [2KB chunks, each
      // ACKed with 0x06 from the host] + 4B CRC32 LE.
      //
      // The ACK-per-chunk flow control mirrors the upload protocol ('W') and
      // prevents USB-CDC TX buffer overruns that cause data corruption or
      // transfer aborts on larger files. Without pacing, the device writes
      // faster than the host can drain the shared USB serial TX buffer, leading
      // to dropped bytes and CRC mismatches on the receiving end.
      if (!read_cmd_path())
        return;
      FILE* tf = fopen(g_cmd_path, "rb");
      if (!tf) {
        serial_write("ERR:fopen\n");
        return;
      }
      struct stat tst;
      fstat(fileno(tf), &tst);
      const uint32_t tfsize = (uint32_t)tst.st_size;
      serial_write("READY\n");
      // Pause UI updates for the transfer, same as the uploads: the card and
      // the panel share SPI2.
      g_upload_in_progress = true;
      uint8_t tszb[4] = {(uint8_t)tfsize, (uint8_t)(tfsize>>8), (uint8_t)(tfsize>>16), (uint8_t)(tfsize>>24)};
      serial_write_raw(tszb, 4);
      uint32_t tcrc = 0;
      uint8_t tchunk[2048];
      size_t tn;
      bool terror = false;
      uint8_t tack = 0;
      while ((tn = fread(tchunk, 1, sizeof(tchunk), tf)) > 0) {
        serial_write_raw(tchunk, tn);
        tcrc = esp_rom_crc32_le(tcrc, tchunk, tn);
        // Wait for the host's 0x06 ACK before sending the next chunk.
        if (!serial_read_exact(&tack, 1, 30000)) {
          terror = true;
          break;
        }
        if (tack != kAck) {
          terror = true;
          break;
        }
      }
      fclose(tf);
      g_upload_in_progress = false;
      if (terror) {
        break;
      }
      uint8_t tcrb[4] = {(uint8_t)tcrc, (uint8_t)(tcrc>>8), (uint8_t)(tcrc>>16), (uint8_t)(tcrc>>24)};
      serial_write_raw(tcrb, 4);
      break;
    }
    default:
      serial_write("ERR:unknown_cmd\n");
      break;
  }
}
// ---------------------------------------------------------------------------
static void serial_receiver_task(void* /*arg*/) {
  uint8_t epub_pos = 0;  // progress matching kEpubMagic
  uint8_t cmd_pos = 0;   // progress matching kCmdMagic

  while (true) {
    uint8_t byte;
    if (usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(50)) != 1)
      continue;

    // Match EPUB magic.
    if (byte == kEpubMagic[epub_pos]) {
      if (++epub_pos == 4) {
        epub_pos = 0;
        cmd_pos = 0;
        handle_file_upload("/sdcard/books");
        continue;
      }
    } else {
      epub_pos = (byte == kEpubMagic[0]) ? 1 : 0;
    }

    // Match CMND magic.
    if (byte == kCmdMagic[cmd_pos]) {
      if (++cmd_pos == 4) {
        epub_pos = 0;
        cmd_pos = 0;
        handle_serial_cmd();
        continue;
      }
    } else {
      cmd_pos = (byte == kCmdMagic[0]) ? 1 : 0;
    }
  }
}

// Call once from app_main before the main loop
// Start the receiver on first USB connect, not at boot.
//
// The driver buffers (2 KB tx + 4 KB rx) and the task stack (8 KB) are ~14 KB of
// RAM, and the task wakes the CPU every 50 ms on its read timeout. On battery
// none of that is ever used, so it is deferred until a host actually appears.
// usb_serial_jtag_is_connected() reads the peripheral's SOF state directly and
// does not need the driver installed, so the main loop can poll it for free — it
// already does, to suppress auto-sleep.
inline void serial_start_if_connected() {
  static bool started = false;
  if (started)
    return;
  started = true;
  usb_serial_jtag_driver_config_t cfg = {
    .tx_buffer_size = 2048,  // must be >= chunk size (2048) to send a full chunk in one call
    .rx_buffer_size = 4096,
  };
  usb_serial_jtag_driver_install(&cfg);
  usb_serial_jtag_vfs_register();
  xTaskCreate(serial_receiver_task, "serial_rx", 8192, nullptr, 3, nullptr);
}
