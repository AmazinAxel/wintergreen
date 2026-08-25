#pragma once

// Raw asset partitions, memory-mapped for zero-RAM, XIP-speed access.
//
// `font` and `sleep` (see default_16MB.csv) are NOT filesystems. Each holds one
// file, written verbatim by esptool at flash time from tools/generate_assets.py.
// Nothing here parses a header, checks a CRC, erases flash or decompresses: the
// build machine does all of that, and the device only calls esp_partition_mmap.
//
// That is the whole reason the old provisioning path is gone. It used to inflate
// a zlib-compressed bundle from the app image into this partition on the first
// boot after every firmware update — ~15 s of erase-and-write behind an
// "Installing fonts..." progress bar, with the font occupying flash twice.

#include <cstddef>
#include <cstdint>

#include "esp_partition.h"

// One mmapped raw partition. `data` stays valid for the life of the process;
// nothing unmaps, because both assets are needed until power-off.
struct RawPartition {
  const uint8_t* data = nullptr;
  size_t size = 0;

  bool map(const char* name) {
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, name);
    if (!part)
      return false;
    esp_partition_mmap_handle_t handle;
    const void* mapped = nullptr;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &mapped, &handle) != ESP_OK)
      return false;
    data = static_cast<const uint8_t*>(mapped);
    size = part->size;
    return true;
  }
};
