// Host preview: the real Application and screens, driven by simulated button
// presses over a card directory of baked books, through the real draw lists,
// strip renderer and diff, onto a simulated panel (sim_panel.h). Each step
// writes what the glass shows as a PGM in the panel's simulated tones (the
// same table as tools/check_font.py), portrait or landscape as held.
//
//   preview <card dir> <out dir>
//
// tools/tests/preview.sh builds it, bakes the card and converts to PNG.
// Also prints each screen's draw-list size (what sets DrawBuffer::kArenaBytes)
// and the bytes each update sent.
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include <filesystem>

#include "Application.h"
#include "content/CoverPaths.h"
#include "content/wgb/WgbPageView.h"
#include "content/wgb/WgbReader.h"
#include "reader_font.h"
#include "sim_panel.h"

using namespace wintergreen;

namespace {

constexpr uint8_t kTone[4] = {226, 178, 128, 34};

struct HostRuntime : IRuntime {
  uint32_t frame_time_ms() const override { return 25; }
  void wait_next_frame(uint32_t) override {}
  uint32_t target_frame_ms() const override { return 25; }
  std::optional<uint8_t> battery_percentage() const override { return 87; }
  int battery_millivolts() const override { return 3900; }
};

std::string g_out;
simpanel::SimPanel* g_disp = nullptr;
DrawBuffer* g_buf = nullptr;
size_t g_bytes = 0;

void save(const char* name, bool landscape = false) {
  const int W = landscape ? DisplayFrame::kPhysicalWidth : DrawBuffer::kWidth;
  const int H = landscape ? DisplayFrame::kPhysicalHeight : DrawBuffer::kHeight;
  std::vector<uint8_t> px(static_cast<size_t>(W) * H);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const int row = landscape ? y : DisplayFrame::kPhysicalHeight - 1 - x;
      const int bit = (landscape ? x : y) + DisplayFrame::kPanelOffsetX;
      px[static_cast<size_t>(y) * W + x] = kTone[g_disp->glass[static_cast<size_t>(row) * simpanel::kBits + bit]];
    }
  const std::string path = g_out + "/" + name + ".pgm";
  FILE* f = std::fopen(path.c_str(), "wb");
  std::fprintf(f, "P5\n%d %d\n255\n", W, H);
  std::fwrite(px.data(), 1, px.size(), f);
  std::fclose(f);
  const DrawBuffer::Stats st = g_buf->current_stats(/*glass=*/true);
  std::printf("%-22s updates=%d sent=%6zu B  list: %3u ops %5u B + tail %4u B%s\n", name, g_disp->updates,
              g_disp->bytes - g_bytes, st.ops, st.ops_bytes, st.tail_bytes, st.overflowed ? "  OVERFLOW" : "");
  g_bytes = g_disp->bytes;
}

ButtonState press(Button b) {
  ButtonState s;
  s.pressed_latch = static_cast<uint8_t>(1u << static_cast<uint8_t>(b));
  s.press_history[0] = static_cast<uint8_t>(b);
  s.press_history_count = 1;
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <card dir> <out dir>\n", argv[0]);
    return 2;
  }
  const std::string card = argv[1];
  g_out = argv[2];
  simpanel::SimPanel disp;
  DrawBuffer buf(disp);
  g_disp = &disp;
  g_buf = &buf;
  HostRuntime rt;
  Application app;
  app.set_books_dir(card.c_str());
  app.set_state_root(card.c_str());
  app.set_reader_font(books::reader_fonts());

  std::vector<uint8_t> mark(DisplayFrame::kFrameBytes);
  {
    FILE* f = std::fopen("resources/sleep.frame", "rb");
    assert(f && std::fread(mark.data(), 1, mark.size(), f) == mark.size());
    std::fclose(f);
  }
  buf.set_sleep_image(mark.data(), mark.size());

  auto step = [&](Button b) {
    app.update(press(b), 25, buf, rt);
    // Frames after the press: deferred work (staging, persistence).
    for (int i = 0; i < 4; ++i)
      app.update(ButtonState{}, 25, buf, rt);
  };

  app.start(buf, rt);
  save("01-list");
  step(Button::Down);
  save("02-list-cursor");
  step(Button::Down);
  save("03-list-cursor2");
  step(Button::Button1);  // open the selected book
  save("04-page");
  for (int i = 0; i < 6; ++i)
    step(Button::Button3);
  save("05-page-turned");
  step(Button::Button2);
  save("06-page-back");
  step(Button::Button1);  // quick menu
  save("07-quickmenu");
  for (int i = 0; i < 5; ++i)
    step(Button::Down);
  save("08-quickmenu-grid");
  step(Button::Button0);  // back to the page (parked snapshot)
  save("09-page-restored");
  step(Button::Button0);  // close the book: home
  save("10-home");
  step(Button::Down);
  save("11-home-next");
  // Sleep: power held, the book's cover.
  step(Button::Button1);  // open from home
  save("12-page-from-home");
  {
    ButtonState s = press(Button::Power);
    s.current = static_cast<uint8_t>(1u << static_cast<uint8_t>(Button::Power));
    app.update(s, 25, buf, rt);
    ButtonState held;
    held.current = s.current;
    for (int i = 0; i < 20 && app.running(); ++i)
      app.update(held, 25, buf, rt);
  }
  save("13-sleep-cover");
  buf.show_sleep_image();
  save("14-wordmark");

  // Wake into the book in landscape (the setting the quick menu writes).
  {
    FILE* f = std::fopen((card + "/.wintergreen").c_str(), "a");
    std::fputs("rotate_reader=1\n", f);
    std::fclose(f);
  }
  Application app2;
  app2.set_books_dir(card.c_str());
  app2.set_state_root(card.c_str());
  app2.set_reader_font(books::reader_fonts());
  app2.start(buf, rt);
  assert(app2.is_reader_active());
  save("15-landscape-page", true);
  for (int i = 0; i < 3; ++i) {
    app2.update(press(Button::Down), 25, buf, rt);
    for (int k = 0; k < 4; ++k)
      app2.update(ButtonState{}, 25, buf, rt);
  }
  save("16-landscape-turned", true);
  // Back from the book: the home carousel.
  app2.update(press(Button::Button0), 25, buf, rt);
  for (int k = 0; k < 4; ++k)
    app2.update(ButtonState{}, 25, buf, rt);
  save("17-home");
  app2.update(press(Button::Up), 25, buf, rt);
  for (int k = 0; k < 4; ++k)
    app2.update(ButtonState{}, 25, buf, rt);
  save("18-home-next");

  // Image pages: wake into the first book that has one, at its first image
  // page, in each orientation (book.pos + settings, as the device would).
  // The book with the most images, from a quarter of the way in.
  std::string best;
  uint16_t most = 0;
  for (const auto& e : std::filesystem::directory_iterator(card)) {
    WgbReader r;
    const std::string wgb = (e.path() / "book.wgb").string();
    if (r.open(wgb.c_str()) && r.image_count() > most) {
      most = r.image_count();
      best = wgb;
    }
  }
  for (int once = 0; once < 1 && most > 0; ++once) {
    const std::string wgb = best;
    WgbReader r;
    r.open(wgb.c_str());
    for (int rot = 0; rot < 2; ++rot) {
      const uint8_t layout = wgb_layout_index(0, rot == 1);
      std::vector<uint8_t> dl(kWgbMaxPageBytes);
      bool found = false;
      for (uint32_t i = 0; i < r.page_count(layout) && !found; ++i) {
        WgbPage rec;
        if (!r.page(layout, i, rec) || !r.read_page(rec, dl.data(), dl.size()))
          continue;
        WgbPageView v(dl.data(), rec.dl_size);
        uint8_t tag;
        while (v.peek(tag) && !found) {
          if (tag == kWgbItemImage && i > r.page_count(layout) / 4)
            found = true;
          v.skip();
        }
        if (!found)
          continue;
        FILE* f = std::fopen(book_pos_path(wgb.c_str()).c_str(), "w");
        std::fprintf(f, "%u %u\n", rec.para_offset, rec.text_offset);
        std::fclose(f);
        f = std::fopen((card + "/.wintergreen").c_str(), "w");
        std::fprintf(f, "book_path=%s\nfont_size=0\nrotate_reader=%d\n", wgb.c_str(), rot);
        std::fclose(f);
      }
      if (!found)
        continue;
      Application app3;
      app3.set_books_dir(card.c_str());
      app3.set_state_root(card.c_str());
      app3.set_reader_font(books::reader_fonts());
      app3.start(buf, rt);
      save(rot ? "20-image-page-landscape" : "19-image-page", rot == 1);
    }
  }
  return 0;
}
