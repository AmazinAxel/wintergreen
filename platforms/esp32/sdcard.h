#pragma once

// SD card support

#include <cstdio>

#include "esp_vfs_fat.h"

#define SD_MOUNT "/sdcard"
#define SD_MAX_FILES 4


#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

#define SD_CS GPIO_NUM_12
static sdmmc_card_t* sd_card_ = nullptr;

inline bool sd_init() {
  // CS pin: default-high so the SD card stays deselected until we talk to it
  gpio_set_direction(SD_CS, GPIO_MODE_OUTPUT);
  gpio_set_level(SD_CS, 1);

  sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
  dev_cfg.gpio_cs = SD_CS;
  dev_cfg.host_id = SPI2_HOST;

  sdspi_dev_handle_t handle{};
  esp_err_t err = sdspi_host_init_device(&dev_cfg, &handle);
  if (err != ESP_OK) {
    return false;
  }

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = handle;
  // 20 MHz is the ceiling — 40 and 26.7 were both tested and fail. See CLAUDE.md.
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;

  esp_vfs_fat_mount_config_t mnt{};
  mnt.format_if_mount_failed = false;
  mnt.max_files = SD_MAX_FILES;
  mnt.allocation_unit_size = 8 * 1024;

  err = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &dev_cfg, &mnt, &sd_card_);
  if (err != ESP_OK) {
    return false;
  }

  return true;
}

inline bool sd_mounted() {
  return sd_card_ != nullptr;
}
