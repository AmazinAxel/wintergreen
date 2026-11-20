// The sleep frame: what the converter writes (pack_sleep_frame, cover_sleep.bin)
// and what tools/make_sleep_image.py wrote (resources/sleep.frame) both come
// out of DrawBuffer::show_frame — the device's streamer, a strip at a time —
// as exactly the levels they hold, on a simulated panel, in one full refresh
// that powers the panel down. sleep_frame_level reads the format back the way
// `books` previews it. A frame of the wrong size is not shown.
#include <cassert>
#include <cstdio>
#include <vector>

#include "content/CoverPaths.h"
#include "content/ImageDecoder.h"
#include "sim_panel.h"

using namespace wintergreen;
using namespace simpanel;

int main() {
  static_assert(kSleepFrameBytes == DisplayFrame::kFrameBytes, "frame size");
  // Converter side: a gradient with flat bands.
  std::vector<uint8_t> gray(static_cast<size_t>(kSleepCoverW) * kSleepCoverH);
  for (int y = 0; y < kSleepCoverH; ++y)
    for (int x = 0; x < kSleepCoverW; ++x)
      gray[static_cast<size_t>(y) * kSleepCoverW + x] = static_cast<uint8_t>(y < 100 ? 255 : y < 200 ? 0 : (x * 255) / kSleepCoverW);
  std::vector<uint8_t> frame(kSleepFrameBytes);
  pack_sleep_frame(gray.data(), frame.data());
  for (int y = 0; y < kSleepCoverH; ++y)
    for (int x = 0; x < kSleepCoverW; ++x)
      assert(sleep_frame_level(frame.data(), x, y) ==
             quantise_level(gray[static_cast<size_t>(y) * kSleepCoverW + x], x, y, 4));

  SimPanel disp;
  DrawBuffer buf(disp);
  MemSource src;
  src.items.push_back(frame);
  buf.show_frame(&src, 0);
  assert(disp.full_fires == 1);
  for (int y = 0; y < kSleepCoverH; ++y)
    for (int x = 0; x < kSleepCoverW; ++x) {
      const int row = DisplayFrame::kPhysicalHeight - 1 - x, bit = DisplayFrame::kPanelOffsetX + y;
      assert(disp.glass[static_cast<size_t>(row) * kBits + bit] == sleep_frame_level(frame.data(), x, y));
    }

  // The wordmark, as flashed.
  FILE* f = std::fopen("resources/sleep.frame", "rb");
  assert(f);
  std::vector<uint8_t> mark(kSleepFrameBytes + 1);
  const size_t n = std::fread(mark.data(), 1, mark.size(), f);
  std::fclose(f);
  assert(n == kSleepFrameBytes);
  mark.resize(n);
  buf.set_sleep_image(mark.data(), mark.size());
  assert(buf.show_sleep_image());
  assert(disp.glass == frame_levels(mark));
  int seen[4] = {};
  for (uint8_t l : disp.glass)
    ++seen[l];
  std::printf("wordmark levels: %d %d %d %d\n", seen[0], seen[1], seen[2], seen[3]);
  assert(seen[0] && seen[1] && seen[2] && seen[3]);

  // Wrong size: refused.
  buf.set_sleep_image(mark.data(), mark.size() - 1);
  assert(!buf.show_sleep_image());
  std::printf("ok\n");
  return 0;
}
