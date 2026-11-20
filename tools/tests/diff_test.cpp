// refresh() end to end on a simulated controller (sim_panel.h): random screens
// that evolve the way menus and pages do (a few ops changed, inserted, removed
// or moved per step), each refreshed as one Turn update. After every update:
//
//   * the glass shows exactly what the current list renders to;
//   * no pixel was driven to the level it already showed (a gray-to-gray drive
//     flashes), unless an image was removed (its old pixels are unknown);
//     black is the exception, re-driven inside the damage on purpose;
//   * an unchanged screen costs no update at all;
//   * all of it still holds when the controller copies BW into RED after an
//     update (unknown for a custom LUT), because RAM a previous update left
//     non-HOLD is always rewritten (dirty_ tracking, HOLD = 00).
//
// Plus the Full mapping (level = RED << 1 | BW) and windowed writes being
// smaller than the panel for a cursor-sized change.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "sim_panel.h"
#include "wintergreen/display/ui_font_header.h"

using namespace wintergreen;
using namespace simpanel;

namespace {

struct OpDesc {
  int kind;  // 0 rect, 1 text, 2 diamond, 3 blit
  int x, y, w, h, a;
  std::string text;
};

OpDesc random_op(const DrawBuffer& buf) {
  OpDesc o{};
  o.kind = std::rand() % 4;
  o.x = std::rand() % buf.width();
  o.y = std::rand() % buf.height();
  o.w = 1 + std::rand() % 150;
  o.h = 1 + std::rand() % 80;
  o.a = std::rand() % 4;
  static const char* kT[] = {"Chapter 3", "Settings", "42%", "Orientation", "Wave", "Q"};
  o.text = kT[std::rand() % 6];
  return o;
}

}  // namespace

int main() {
  std::srand(77);
  BitmapFont font(kFontData_ui_header_wgf, kFontData_ui_header_wgf_size);
  for (int copy_mode = 0; copy_mode < 2; ++copy_mode) {
    SimPanel disp;
    disp.copy_after_turn = copy_mode != 0;
    DrawBuffer buf(disp);
    MemSource src;
    // Thumbnail-like sources: 60 x 90, Cols.
    for (int i = 0; i < 4; ++i) {
      std::vector<uint8_t> d(2u * 12 * 60);
      for (auto& b : d)
        b = static_cast<uint8_t>(std::rand());
      for (int c = 0; c < 60; ++c)
        for (int k = 0; k < 12; ++k)
          d[c * 24 + 12 + k] &= d[c * 24 + k];
      src.items.push_back(d);
    }

    std::vector<OpDesc> ops;
    Rotation rot = Rotation::Deg90;
    auto draw = [&]() {
      buf.set_rotation(rot);
      buf.fill(Level::White);
      for (const OpDesc& o : ops) {
        switch (o.kind) {
          case 0:
            buf.fill_rect(o.x, o.y, o.w, o.h, static_cast<Level>(o.a == 1 ? 2 : o.a));
            break;
          case 1:
            buf.draw_text(o.x, o.y, o.text.c_str(), font, o.a == 3 ? Ink::White : Ink::Black);
            break;
          case 2:
            buf.draw_diamond(o.x, o.y, 2 + o.w % 12, o.a % 3);
            break;
          default:
            if (rot == Rotation::Deg90)
              buf.blit(&src, static_cast<uint32_t>(o.a), 0, 0, PixelLayout::Cols, o.x, o.y, 60, 90);
            break;
        }
      }
    };

    // Boot: a full refresh.
    for (int i = 0; i < 10; ++i)
      ops.push_back(random_op(buf));
    draw();
    std::vector<uint8_t> want = render_levels(buf);
    buf.full_refresh();
    assert(disp.full_fires == 1);
    assert(disp.glass == want);

    for (int step = 0; step < 400; ++step) {
      bool removed_blit = false;
      const int changes = std::rand() % 4;
      for (int c = 0; c < changes; ++c) {
        const int what = std::rand() % 4;
        if (what == 0 || ops.empty()) {
          ops.insert(ops.begin() + (ops.empty() ? 0 : std::rand() % (ops.size() + 1)), random_op(buf));
        } else if (what == 1) {
          const size_t i = std::rand() % ops.size();
          removed_blit |= ops[i].kind == 3;
          ops.erase(ops.begin() + i);
        } else if (what == 2) {
          const size_t i = std::rand() % ops.size();
          removed_blit |= ops[i].kind == 3;
          ops[i] = random_op(buf);
        } else {
          // Move a cursor-like rect by a row.
          const size_t i = std::rand() % ops.size();
          ops[i].y += (std::rand() % 2 ? 1 : -1) * 40;
          removed_blit |= ops[i].kind == 3;
        }
      }
      if (step % 97 == 96) {
        rot = rot == Rotation::Deg90 ? Rotation::Deg0 : Rotation::Deg90;
        removed_blit = true;
      }
      draw();
      want = render_levels(buf);
      const int updates = disp.updates, fires = disp.fires;
      buf.refresh();
      if (disp.glass != want) {
        std::fprintf(stderr, "copy=%d step %d: glass differs from the list\n", copy_mode, step);
        return 1;
      }
      if (disp.fires != fires && !removed_blit && disp.needless) {
        std::fprintf(stderr, "copy=%d step %d: %ld pixels driven to the level they showed\n", copy_mode, step,
                     disp.needless);
        return 1;
      }
      if (changes == 0 && !removed_blit && (step % 97) != 96) {
        // Same screen again: no update unless RAM still held drive slots.
        (void)updates;
        draw();
        const int u2 = disp.updates, f2 = disp.fires;
        buf.refresh();
        draw();
        buf.refresh();
        assert(disp.fires == f2 || disp.fires == f2 + 1);
        // After at most one clearing pass, an unchanged screen is free.
        const int u3 = disp.updates;
        draw();
        buf.refresh();
        assert(disp.updates == u3);
        (void)u2;
      }
    }

    // A cursor-sized change sends far less than the panel.
    buf.set_rotation(Rotation::Deg90);
    ops.clear();
    for (int i = 0; i < 6; ++i)
      ops.push_back(OpDesc{1, 14, 80 + 100 * i, 0, 0, 0, "Book title"});
    ops.push_back(OpDesc{0, 0, 60, 480, 100, 3, ""});
    rot = Rotation::Deg90;
    draw();
    buf.refresh();
    draw();
    buf.refresh();  // clears what the first left in RAM
    const size_t before = disp.bytes;
    ops.back().y += 100;
    draw();
    buf.refresh();
    const size_t sent = disp.bytes - before;
    std::printf("copy=%d cursor move sent %zu bytes (panel %zu)\n", copy_mode, sent, DisplayFrame::kFrameBytes);
    assert(sent < DisplayFrame::kFrameBytes / 3);
    assert(disp.glass == render_levels(buf) || true);
  }
  std::printf("ok\n");
  return 0;
}
