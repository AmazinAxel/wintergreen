#pragma once

// Asset blob: read-only access to binary resources (fonts, sleep images)
// embedded directly in the firmware image via CMake EMBED_FILES.
// See tools/build_assets.py for the blob format.
//
// NOTE: assets are placed in DROM and consume MMU pages.  OTA validation
// requires ~17 free MMU pages; embedding ~2 MB of assets uses ~17 pages,
// leaving ~30 free.  This is sufficient but leaves less headroom than the
// old appended-blob approach.
//
// Call asset_blob::g_assets.init() once at startup before any consumer.

#include <cstddef>
#include <cstdint>

#include "esp_partition.h"

namespace asset_blob {

struct Entry {
  char name[32];
  uint32_t offset;  // byte offset within assets.bin from its start
  uint32_t length;
  uint32_t crc32;
};
static_assert(sizeof(Entry) == 44, "Entry layout must match build_assets.py");

class Blob {
 public:
  bool init();

  // Look up an asset by name.  Returns false if not found.
  // `data_offset` is the byte offset within assets.bin from its start.
  bool find(const char* name, uint32_t& data_offset, uint32_t& length, uint32_t& crc32) const;

  // Convenience: total CRC of the named asset (0 if not found).
  uint32_t crc(const char* name) const;
  uint32_t size(const char* name) const;

  // Returns a direct DROM pointer to the asset data.  handle_out is always
  // set to 0; unmap() is a no-op.  Returns nullptr if not found.
  const void* map(const char* name, size_t& size_out, esp_partition_mmap_handle_t& handle_out);
  void unmap(esp_partition_mmap_handle_t handle);

  // Copy raw bytes from an asset.
  bool read(const char* name, uint32_t offset, void* dst, size_t len) const;

 private:
  Entry* entries_ = nullptr;
  uint32_t count_ = 0;
};

extern Blob g_assets;

}  // namespace asset_blob
