# Wintergreen

A highly focused and lightweight epub reader for the Xteink X4 whose goal is to optimize the reading experience and be as usable as possible

Wintergreen offers first-class support for bluetooth page turners and a wireless book syncing

A fork of [Nous](https://github.com/unitreign/nous), which is a fork of [Microreader](https://github.com/CidVonHighwind/wintergreen)

Some goals:

- Syncs to a scripted NAS server
  - Because all books have to be converted to a binary format, the NAS server runs a script to convert all of them locally so that the Xteink can pull just the binary files and not the unconverted epubs. This saves space and makes syncing _much_ faster.
  - When a book is finished, a Sync deletes it and saves it to a different JSON file with all your finshed books!
- Bluetooth clicker support (only supports reprogammable clickers with left/right arrow for page navigation)
  - Highly optimized, works great (NOT a beta feature!) and supports back/select keys with page caching!
- Nix-first building!!
  - A Homelab setup works best with NixOS so you can convert your books and sync them locally
- NO settings menu!! Everything is hardcoded for performance with very sensible defaults so you don't have to think twice. Just flash and start reading!
  - If you want syncing or bluetooth support, you will HAVE to build from scratch
- No unnecessary UI, stat tracking or other extra features. It's meant for very fast and productive reading for quick moments, and nothing else to distract you from that.
  - The use cases here optimize for in-bed reading with a Bluetooth clicker and reading out-and-about with the built-in keys!
- The best possible performance and battery life possible from an X4! This is about as lightweight and optimized of a cfw as you'd get.
  - Everything is built around making things feel fast and we cache wherever possible!

## Media

<img alt="wintergreen" src="https://github.com/user-attachments/assets/7c8fbd3b-71ae-4996-803c-ef8ccab5c184"/>

<img alt="home-menu" src="https://github.com/user-attachments/assets/046d05b8-a048-4c46-ab09-ae8223e9bd27"/>

<img alt="homelab" src="https://github.com/user-attachments/assets/d38311b1-2a7b-42bc-a297-400a00fc4df7"/>

<img alt="quick-menu" src="https://github.com/user-attachments/assets/16840878-ff7c-4108-b73c-ba5b7d13cd04"/>

## Flashing this

**DISCLAIMER: You should not flash this on an locked device!! This firmware does _not_ offer network or SD card flashing, so you will be softlocked on this firmware if you flash it. No support is provided for flashng on a locked device, so please use a developer model!**

This project uses Platformio, so you can simply run `pio run -t upload` to build and upload the firmware to the device over USB!

Paste and fill the following settings into the `WintergreenConfig.h` file in your project root. These are what the default firmware come compiled with, you will definitely want to change these:

```c
#pragma once

#include <cstdint>
#define WG_BLUETOOTH_PAGE_TURNER "7B:70:25:80:D8:27" // you need a mac address like this one!
//#define WG_WIFI_SYNC // if you want wifi sync you need to uncomment this!

namespace wintergreen::config {
  inline constexpr const char* kWifiName = "";
  inline constexpr const char* kWifiPassword = "";
  inline constexpr const char* syncServer = "";

  inline constexpr bool kSunlightFadingFix = false;
  inline constexpr uint8_t kAutoSleepMinutes = 2;

  inline constexpr bool kResumeOnePageBack = true;
  inline constexpr uint32_t kHoldDelayMs = 300;
  inline constexpr uint32_t kHoldRepeatMs = 200;
  inline constexpr int kHoldAccelStep = 0;
}
```

If you use a white Xteink in the sun, I recommend enabling the fading fix, but it can cause more full screen refreshes. If you own a newer Xteink, you may not need the fix since I've used the device in the sun without needing to enable the option (which I have kept from nous). Auto sleep should be set to as low a value as comfortably possible. I personally recommend 1-2 minutes, depending on your reading speed.

The button timing should also be modified to your liking! Especially to scroll through a large library of books or chapters, I suggest lower values for repeating and hold. Be warned that below 200ms repeat, it can get a little disorientating since the e-ink panel can only refresh so fast! The defaults are very sensible.

<!-- pio run first!!

nix-shell -p 'python3.withPackages(ps: [ps.pyserial])' --run '
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32c3 merge_bin -o wintergreen.bin --flash_mode dio --flash_freq 80m --flash_size 16MB \
0x0      .pio/build/esp32c3/bootloader.bin \
0x8000   .pio/build/esp32c3/partitions.bin \
0x10000  .pio/build/esp32c3/firmware.bin \
0xc90000 .pio/build/esp32c3/font_partition.bin \
0xd10000 .pio/build/esp32c3/sleep_partition.bin'
-->

The prebuilt binaries ship with the default config from above! If you want to modify those values, you'd have to build it from scratch. If you choose to use the prebuilt binary, flash it with esptool (you cant use the crosspoint flasher):

```bash
pip install esptool
esptool.py --chip esp32c3 -p /dev/ttyACM0 write_flash 0x0 wintergreen.bin
```

The `wintergreen.bin` in this repository is from the latest git changes. You should use that over a release binary since it might have changes that you'd want!
