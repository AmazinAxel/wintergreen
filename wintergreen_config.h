#pragma once

#include <cstdint>

namespace wintergreen {
namespace config {

// todo
inline constexpr const char* kWifiSsid     = "";
inline constexpr const char* kWifiPassword = "";

//inline constexpr bool kWifiEnabled = kWifiSsid[0] != '\0';

inline constexpr bool kSunlightFadingFix = false;

inline constexpr uint8_t kAutoSleepMinutes = 3;

}  // namespace config
}  // namespace wintergreen
