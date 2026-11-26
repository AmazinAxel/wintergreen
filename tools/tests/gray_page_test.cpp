// Gray reader pages through DrawBuffer on a simulated controller (gray_sim.h):
// random sequences of page turns (both rotations, images, the bar), 1-bit OTP
// screens (partial and full) and repeats, from a garbage glass at boot. After
// every update the glass must be exactly what was meant: the page's three
// levels after a gray update, the 1-bit frame after an OTP one (so the handoff
// leaves no gray behind). A text-only turn from a known glass never drives a
// white or gray pixel to the level it already shows, and an unchanged page
// fires nothing.
//
// Run with tools/tests/run.sh.
#include <cstdio>

#include "gray_sim.h"
#include "reader_font.h"

using namespace wintergreen;

namespace {

bool has_image(const sim::TestPage& p) {
  for (const auto& ci : p.pc.items)
    if (std::holds_alternative<PageImageItem>(ci))
      return true;
  return false;
}

void random_frame(DrawBuffer& buf) {
  buf.set_rotation(Rotation::Deg90);
  buf.fill(true);
  for (int k = std::rand() % 20; k > 0; --k)
    buf.fill_rect(std::rand() % 500 - 10, std::rand() % 800 - 10, std::rand() % 200, std::rand() % 200,
                  std::rand() % 4 == 0);
}

}  // namespace

int main() {
  const BitmapFontSet* fonts = books::reader_fonts();
  assert(fonts && fonts->num_fonts() >= 2);
  std::srand(7);

  std::vector<std::unique_ptr<sim::TestPage>> pages;
  std::vector<sim::Levels> want;
  for (int i = 0; i < 16; ++i) {
    pages.push_back(sim::random_page(*fonts, i & 1 ? Rotation::Deg0 : Rotation::Deg90, /*images=*/i % 4 == 0));
    want.push_back(sim::reference(*pages.back(), *fonts));
  }

  sim::Panel panel;
  for (auto& g : panel.glass)
    g = static_cast<uint8_t>(std::rand() % 3);  // whatever the sleep screen left
  for (auto& b : panel.bw)
    b = static_cast<uint8_t>(std::rand());
  for (auto& b : panel.red)
    b = static_cast<uint8_t>(std::rand());
  DrawBuffer buf(panel);
  sim::FakeImages images;

  int last = -1;             // page on the glass, -1 = none
  bool known = false;        // glass known exactly (not the boot garbage)
  for (int step = 0; step < 400; ++step) {
    const int act = step == 0 ? 0 : std::rand() % 10;
    if (act <= 6) {
      const int i = act == 6 && last >= 0 ? last : std::rand() % static_cast<int>(pages.size());
      const sim::TestPage& p = *pages[i];
      const int turns = panel.turns, windows = panel.windows;
      const long redriven = panel.redriven;
      buf.set_rotation(p.rot);
      buf.gray().set(p.dl.data(), p.dl.size(), fonts, p.rot, p.bar_h, &images);
      if (step == 0)
        buf.full_refresh();  // boot into a book: Application::start's paint
      else
        buf.refresh();
      if (panel.glass != want[i]) {
        std::fprintf(stderr, "step %d: page %d not on the glass\n", step, i);
        return 1;
      }
      if (i == last && (panel.turns != turns || panel.windows != windows)) {
        std::fprintf(stderr, "step %d: unchanged page fired an update\n", step);
        return 1;
      }
      if (known && i != last && !has_image(p) && (last < 0 || !has_image(*pages[last])) &&
          panel.redriven != redriven) {
        std::fprintf(stderr, "step %d: %ld pixels re-driven to the level they showed\n", step,
                     panel.redriven - redriven);
        return 1;
      }
      last = i;
      known = true;
    } else {
      random_frame(buf);
      if (act == 9)
        buf.full_refresh();
      else
        buf.refresh();
      if (panel.glass != sim::frame_levels(panel.bw.data())) {
        std::fprintf(stderr, "step %d: OTP frame not on the glass (handoff)\n", step);
        return 1;
      }
      last = -1;
      known = true;
    }
  }
  std::printf("ok: %d turns, %d OTP updates, %d strip windows\n", panel.turns, panel.otp, panel.windows);
  return 0;
}
