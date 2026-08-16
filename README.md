# Wintergreen

A highly focused lightweight e-book reader for the Xteink X4, whose goal is to optimize the reading experience and be as usable as possible

A fork of [Nous](https://github.com/unitreign/nous), which is a fork of [Microreader](https://github.com/CidVonHighwind/wintergreen)

Some goals:

- Syncs to a scripted NAS server
  - Because all books have to be converted to a binary format, the NAS server runs a script to convert all of them locally so that the Xteink can pull just the binary files and not the unconverted epubs. This saves space and makes syncing *much* faster.
  - All of the book reading progress is saved to a simple json file! And when a book is finished, it gets deleted and saved to a different json file listing all your finshed books!
- Bluetooth clicker support with auto pair (only supports reprogammable clickers with left/right arrow for page navigation)
- Nix-first building!!
- NO settings menu!! Everything is hardcoded for performance with very sane defaults so you don't have to think twice. Just flash and start reading!
- No unnecessary UI, stat tracking or other extra features. It's meant for very fast and productive reading for quick moments, and nothing else to distract you from that.
- The best possible performance and battery life possible from an X4! This is about as lightweight and optimized of a cfw as you'd get.

## Flashing this

**DISCLAIMER: You should not flash this on an locked device yet!!**

This project uses Platformio, so you can simply run `pio run -t upload` to build and upload the firmware to the device over USB!

Paste and fill the following settings into the `WintergreenConfig.h` file in your project root!

```c
#pragma once

namespace wintergreen::config {
  // Wifi syncing
  inline constexpr const char* kWifiSsid = "";
  inline constexpr const char* kWifiPassword = "";

  // General settings
  inline constexpr bool kSunlightFadingFix = false;
  inline constexpr uint8_t kAutoSleepMinutes = 1;
}
```

If you use a white Xteink in the sun, I recommend enabling the fading fix, but it can cause more full screen refreshes. Auto sleep should be set to as low a value as comfortably possible. I personally recommend 1-2 minutes, depending on your reading speed.