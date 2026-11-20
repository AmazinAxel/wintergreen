// A simulated SSD1677 for the host tests: controller RAM (BW and RED planes),
// and the glass, updated by the two waveforms the way epd.h programs them.
//
//   Turn: per pixel slot RED << 1 | BW — 00 HOLD (glass unchanged), 01 to
//         white, 10 to gray, 11 to black.
//   Full: the glass takes the level RED << 1 | BW (white, light, gray, black).
//
// `copy_after_turn` models a controller that copies BW into RED after an update
// (the OTP fast path did something like it; unknown for a custom LUT): the
// renderer must stay correct either way.
#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

#include "wintergreen/display/DrawBuffer.h"

namespace simpanel {

using namespace wintergreen;

constexpr int kRows = DisplayFrame::kPhysicalHeight;
constexpr int kBits = DisplayFrame::kPanelWidth;
constexpr int kStride = DisplayFrame::kStride;

struct SimPanel : IDisplay {
  std::vector<uint8_t> bw = std::vector<uint8_t>(DisplayFrame::kPixelBytes, 0);
  std::vector<uint8_t> red = std::vector<uint8_t>(DisplayFrame::kPixelBytes, 0);
  std::vector<uint8_t> glass = std::vector<uint8_t>(static_cast<size_t>(kRows) * kBits, 0);  // Level per pixel
  bool copy_after_turn = false;
  bool in_update = false;
  int inflight = 0;
  // Counters.
  int fires = 0, full_fires = 0, updates = 0;
  size_t bytes = 0;
  // Pixels the last Turn drove although the glass already showed the target
  // (black excepted: DrawBuffer re-drives it).
  long needless = 0;
  // Pixels the last Turn drove at all.
  long driven = 0;

  void begin_update(UpdateKind) override {
    // An update with nothing to drive is begun and never fired.
    in_update = true;
    ++updates;
  }
  void await_strip_slot() override {
    assert(in_update);
    if (inflight > 1)
      inflight = 1;
  }
  void write_window(int row0, int rows, int xb0, int xbytes, const uint8_t* b, const uint8_t* r) override {
    assert(in_update);
    assert(row0 >= 0 && rows > 0 && row0 + rows <= kRows);
    assert(xb0 >= 0 && xbytes > 0 && xb0 + xbytes <= kStride);
    // The renderer may hold at most two windows in flight.
    assert(inflight < 2);
    ++inflight;
    for (int y = 0; y < rows; ++y) {
      std::memcpy(&bw[static_cast<size_t>(row0 + y) * kStride + xb0], b + y * xbytes, xbytes);
      std::memcpy(&red[static_cast<size_t>(row0 + y) * kStride + xb0], r + y * xbytes, xbytes);
    }
    bytes += 2u * rows * xbytes;
  }
  void finish_writes() override { inflight = 0; }
  void fire(UpdateKind kind, bool) override {
    assert(in_update && inflight == 0);
    in_update = false;
    ++fires;
    needless = driven = 0;
    for (int y = 0; y < kRows; ++y)
      for (int x = 0; x < kBits; ++x) {
        const size_t i = static_cast<size_t>(y) * kStride + x / 8;
        const int m = 0x80 >> (x & 7);
        const int s = ((red[i] & m) ? 2 : 0) | ((bw[i] & m) ? 1 : 0);
        uint8_t& g = glass[static_cast<size_t>(y) * kBits + x];
        if (kind == UpdateKind::Full) {
          g = static_cast<uint8_t>(s);
          continue;
        }
        if (s == 0)
          continue;
        const uint8_t target = s == 1 ? 0 : s == 2 ? 2 : 3;
        ++driven;
        if (g == target && target != 3)  // black is re-driven on purpose
          ++needless;
        g = target;
      }
    if (kind == UpdateKind::Full)
      ++full_fires;
    if (copy_after_turn && kind == UpdateKind::Turn)
      red = bw;
  }
};

// Levels of a 2-plane Frame (DrawBuffer::render_current) at each panel pixel.
inline std::vector<uint8_t> frame_levels(const std::vector<uint8_t>& frame) {
  std::vector<uint8_t> out(static_cast<size_t>(kRows) * kBits);
  for (int y = 0; y < kRows; ++y)
    for (int x = 0; x < kBits; ++x) {
      const uint8_t* row = &frame[static_cast<size_t>(y) * 2 * kStride];
      const int m = 0x80 >> (x & 7);
      out[static_cast<size_t>(y) * kBits + x] =
          static_cast<uint8_t>(((row[x / 8] & m) ? 2 : 0) | ((row[kStride + x / 8] & m) ? 1 : 0));
    }
  return out;
}

inline std::vector<uint8_t> render_levels(DrawBuffer& buf) {
  std::vector<uint8_t> frame(DisplayFrame::kFrameBytes);
  buf.render_current(frame.data());
  return frame_levels(frame);
}

// A PixelSource over bytes in memory.
struct MemSource : PixelSource {
  std::vector<std::vector<uint8_t>> items;
  int reads = 0;
  bool read(uint32_t item, uint32_t offset, uint8_t* dst, size_t n) override {
    ++reads;
    if (item >= items.size() || offset + n > items[item].size())
      return false;
    std::memcpy(dst, items[item].data() + offset, n);
    return true;
  }
};

}  // namespace simpanel
