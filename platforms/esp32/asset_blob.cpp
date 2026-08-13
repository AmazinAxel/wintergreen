#include "asset_blob.h"

#include <cstdlib>
#include <cstring>

#include "esp_log.h"

// Assets are embedded directly into the firmware image via CMake EMBED_FILES.
// The linker exports these symbols pointing to the raw assets.bin data in DROM.
extern "C" const uint8_t _binary_assets_bin_start[];
extern "C" const uint8_t _binary_assets_bin_end[];

namespace asset_blob {

static constexpr const char* kTag = "asset";
static constexpr uint8_t kMagic[4] = {'A', 'S', 'T', 'S'};
static constexpr uint32_t kVersion = 1;
static constexpr size_t kHeaderFixed = 16;  // magic + version + count + total_size

Blob g_assets;

bool Blob::init() {
  const uint8_t* data = _binary_assets_bin_start;
  size_t data_len = static_cast<size_t>(_binary_assets_bin_end - _binary_assets_bin_start);

  if (data_len < kHeaderFixed) {
    ESP_LOGE(kTag, "embedded assets.bin too small (%u bytes)", static_cast<unsigned>(data_len));
    return false;
  }
  if (memcmp(data, kMagic, 4) != 0) {
    ESP_LOGE(kTag, "embedded assets.bin: bad magic");
    return false;
  }

  uint32_t version = static_cast<uint32_t>(data[4]) | (static_cast<uint32_t>(data[5]) << 8) |
                     (static_cast<uint32_t>(data[6]) << 16) | (static_cast<uint32_t>(data[7]) << 24);
  count_ = static_cast<uint32_t>(data[8]) | (static_cast<uint32_t>(data[9]) << 8) |
           (static_cast<uint32_t>(data[10]) << 16) | (static_cast<uint32_t>(data[11]) << 24);

  if (version != kVersion || count_ == 0 || count_ > 64) {
    ESP_LOGE(kTag, "bad manifest: version=%lu count=%lu", static_cast<unsigned long>(version),
             static_cast<unsigned long>(count_));
    return false;
  }

  size_t entry_bytes = count_ * sizeof(Entry);
  if (kHeaderFixed + entry_bytes > data_len) {
    ESP_LOGE(kTag, "embedded assets.bin truncated");
    return false;
  }

  entries_ = static_cast<Entry*>(malloc(entry_bytes));
  if (!entries_) {
    ESP_LOGE(kTag, "malloc(%u) failed", static_cast<unsigned>(entry_bytes));
    return false;
  }
  memcpy(entries_, data + kHeaderFixed, entry_bytes);

  ESP_LOGI(kTag, "embedded blob: %u entries, %u bytes total", static_cast<unsigned>(count_),
           static_cast<unsigned>(data_len));
  for (uint32_t i = 0; i < count_; i++) {
    char safe_name[33];
    memcpy(safe_name, entries_[i].name, 32);
    safe_name[32] = '\0';
    ESP_LOGI(kTag, "  %-24s off=0x%06lx len=%lu crc=0x%08lx", safe_name, static_cast<unsigned long>(entries_[i].offset),
             static_cast<unsigned long>(entries_[i].length), static_cast<unsigned long>(entries_[i].crc32));
  }
  return true;
}

bool Blob::find(const char* name, uint32_t& data_offset, uint32_t& length, uint32_t& crc32) const {
  if (!entries_)
    return false;
  for (uint32_t i = 0; i < count_; i++) {
    if (strncmp(entries_[i].name, name, 32) == 0) {
      data_offset = entries_[i].offset;  // byte offset within assets.bin from its start
      length = entries_[i].length;
      crc32 = entries_[i].crc32;
      return true;
    }
  }
  return false;
}

uint32_t Blob::crc(const char* name) const {
  uint32_t o, l, c;
  return find(name, o, l, c) ? c : 0u;
}

uint32_t Blob::size(const char* name) const {
  uint32_t o, l, c;
  return find(name, o, l, c) ? l : 0u;
}

const void* Blob::map(const char* name, size_t& size_out, esp_partition_mmap_handle_t& handle_out) {
  uint32_t off, len, crc;
  if (!find(name, off, len, crc))
    return nullptr;
  // Assets live in DROM — return a direct pointer, no mmap needed.
  handle_out = 0;
  size_out = len;
  return _binary_assets_bin_start + off;
}

void Blob::unmap(esp_partition_mmap_handle_t /*handle*/) {
  // No-op: embedded assets are always accessible in DROM.
}

bool Blob::read(const char* name, uint32_t offset, void* dst, size_t len) const {
  uint32_t off, total, crc;
  if (!find(name, off, total, crc))
    return false;
  if (offset > total || offset + len > total)
    return false;
  memcpy(dst, _binary_assets_bin_start + off + offset, len);
  return true;
}

}  // namespace asset_blob
