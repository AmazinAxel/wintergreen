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
  inline constexpr uint32_t kPowerHoldSleepMs = 400;
}
```

If you use a white Xteink in the sun, I recommend enabling the fading fix, but it can cause more full screen refreshes. Auto sleep should be set to as low a value as comfortably possible. I personally recommend 1-2 minutes, depending on your reading speed.

The prebuilt binaries ship with the default config from above! If you want to modify those values, you'd have to build it from scratch.

## Some design choices

This is a VERY opinionated custom firmware! I want people to flash this firmware and start reading without a second thought and without any sort of setup needed. That being said, I have made some very sensible design choices and I'm listing them and their reasons here:

| What | Why |
| No progress bar/battery when reading! | It's distracting. You should focus on the book, not things about the book or the device. If you want to see this information, it's just a short button press away. |
| Iosevka & Atkinson Hyperlegible ONLY | Don't take |
| English only | This cfw is made only for English and some special characters. This firmware will not be internationalized or support other languages. |
| No stats | You don't *really* need stats. It's a nice-to-have but also unnecessary and distracting. There is no other menus than the homescreen and book menu, and I want to keep it that way! |
| Just chapter control | There is no link support. It keeps book builds faster and this a mostly unused feature for many books. Chapters get first class support! |
