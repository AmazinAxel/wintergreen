#include "HomeScreen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../Application.h"
#include "../content/CoverPaths.h"
#include "../content/BookIndex.h"
#include "../display/ui_font_large.h"

namespace wintergreen {

// Filled diamond (a square on its corner), one fill_rect per row. Hard edges —
// the panel is 1-bit, so anything rounded only reads as a ragged circle.
static void draw_diamond(DrawBuffer& buf, int cx, int cy, int r) {
  for (int dy = -r; dy <= r; ++dy) {
    const int span = r - std::abs(dy);
    buf.fill_rect(cx - span, cy + dy, 2 * span + 1, 1, false);
  }
}

// Just the border of that diamond, `t` pixels thick along each row.
static void draw_diamond_outline(DrawBuffer& buf, int cx, int cy, int r, int t) {
  for (int dy = -r; dy <= r; ++dy) {
    const int span = r - std::abs(dy);
    const int w = std::min(t, span + 1);
    if (2 * span + 1 <= 2 * w) {
      buf.fill_rect(cx - span, cy + dy, 2 * span + 1, 1, false);
    } else {
      buf.fill_rect(cx - span, cy + dy, w, 1, false);
      buf.fill_rect(cx + span - w + 1, cy + dy, w, 1, false);
    }
  }
}

// Longest prefix of [text, text+len) that fits in max_w, broken at a space when
// one is available. Returns the byte count to draw; *next is where the following
// line starts (leading space skipped).
static size_t fit_line(const BitmapFont& f, const char* text, size_t len, int max_w, size_t* next) {
  size_t fit = 0, last_space = 0;
  const char* p = text;
  while (fit < len) {
    const uint8_t b = static_cast<uint8_t>(*p);
    const size_t cb = b < 0x80 ? 1u : b < 0xE0 ? 2u : b < 0xF0 ? 3u : 4u;
    if (f.word_width(text, fit + cb, FontStyle::Regular) > max_w) break;
    if (b == ' ') last_space = fit;
    fit += cb;
    p += cb;
  }
  if (fit >= len) { *next = len; return len; }
  if (last_space > 0) { *next = last_space + 1; return last_space; }
  *next = fit;
  return fit;
}

// Centred text, truncated with an ellipsis when it cannot fit in max_w.
static void draw_centred(DrawBuffer& buf, int cx, int baseline, const char* text, size_t len,
                         const BitmapFont& f, int max_w) {
  if (len == 0) return;
  static const char kEll[] = "...";
  const int w = f.word_width(text, len, FontStyle::Regular);
  if (w <= max_w) {
    buf.draw_text_proportional(cx - w / 2, baseline, text, len, f, false);
    return;
  }
  const int budget = max_w - f.word_width(kEll, 3, FontStyle::Regular);
  size_t fit = 0;
  const char* p = text;
  while (fit < len) {
    const uint8_t b = static_cast<uint8_t>(*p);
    const size_t cb = b < 0x80 ? 1u : b < 0xE0 ? 2u : b < 0xF0 ? 3u : 4u;
    if (f.word_width(text, fit + cb, FontStyle::Regular) > budget) break;
    fit += cb;
    p += cb;
  }
  char trunc[260];
  const size_t cp = std::min<size_t>(fit, 256);
  std::memcpy(trunc, text, cp);
  std::memcpy(trunc + cp, kEll, 3);
  const int tw = f.word_width(trunc, cp + 3, FontStyle::Regular);
  buf.draw_text_proportional(cx - tw / 2, baseline, trunc, cp + 3, f, false);
}

// Border outline around a rectangle, `t` pixels thick, growing inwards.
static void draw_outline(DrawBuffer& buf, int x, int y, int w, int h, int t = 1) {
  if (w <= 0 || h <= 0) return;
  t = std::min(t, std::min(w, h));
  buf.fill_rect(x,         y,         w, t, false);
  buf.fill_rect(x,         y + h - t, w, t, false);
  buf.fill_rect(x,         y,         t, h, false);
  buf.fill_rect(x + w - t, y,         t, h, false);
}

// 4x4 ordered dither, used when an oversized cover is area-averaged down: the
// average is a coverage fraction, and a hard 50% threshold would throw away
// every tone between paper and ink.
static constexpr uint8_t kBayer4[4][4] = {
  { 0,  8,  2, 10},
  {12,  4, 14,  6},
  { 3, 11,  1,  9},
  {15,  7, 13,  5},
};

void HomeScreen::load_cover_(int i, int box_w, int box_h) const {
  cover_data_.clear();
  cover_w_ = cover_h_ = 0;
  cover_slot_ = i;
  if (i < 0 || i >= num_books_) return;

  // Prefer the full-res sleep cover (up to 480x786) over the 160x240 list
  // thumbnail: the box is several hundred pixels tall, so the thumbnail can
  // only be doubled into it, while the sleep cover carries enough detail to be
  // scaled down. Both files are the same format — w,h then 1bpp rows, bit
  // clear = ink.
  FILE* f = nullptr;
  if (!slots_[i].sleep_path.empty())
    f = std::fopen(slots_[i].sleep_path.c_str(), "rb");
  if (!f && !slots_[i].bin_path.empty())
    f = std::fopen(slots_[i].bin_path.c_str(), "rb");
  if (!f) return;
  uint16_t hdr[2] = {};
  if (std::fread(hdr, 2, 2, f) != 2) { std::fclose(f); return; }
  const int src_w = hdr[0], src_h = hdr[1];
  if (src_w <= 0 || src_h <= 0) { std::fclose(f); return; }

  int dst_w, dst_h;
  const bool downscale = src_w > box_w || src_h > box_h;
  if (!downscale) {
    // Upscale by a whole number only — pixel-doubling keeps the 1-bit dither
    // intact where a fractional nearest-neighbour scale would moiré it.
    const int k = std::max(1, std::min(box_w / src_w, box_h / src_h));
    dst_w = src_w * k;
    dst_h = src_h * k;
  } else {
    dst_w = box_w;
    dst_h = src_h * box_w / src_w;
    if (dst_h > box_h) { dst_h = box_h; dst_w = src_w * box_h / src_h; }
  }
  if (dst_w <= 0 || dst_h <= 0) { std::fclose(f); return; }

  const int src_stride = (src_w + 7) / 8;
  const int dst_stride = (dst_w + 7) / 8;
  cover_data_.assign(static_cast<size_t>(dst_stride) * dst_h, 0xFF);
  std::vector<uint8_t> src_row(src_stride);

  auto set_ink = [&](int dx, int dy) {
    cover_data_[static_cast<size_t>(dy) * dst_stride + (dx >> 3)] &=
        static_cast<uint8_t>(~(1u << (7 - (dx & 7))));
  };

  if (!downscale) {
    int prev_sy = -1;
    for (int dy = 0; dy < dst_h; ++dy) {
      const int sy = dy * src_h / dst_h;
      if (sy != prev_sy) {
        std::fseek(f, 4 + static_cast<long>(sy) * src_stride, SEEK_SET);
        if (std::fread(src_row.data(), 1, src_stride, f) != static_cast<size_t>(src_stride))
          { cover_data_.clear(); break; }
        prev_sy = sy;
      }
      for (int dx = 0; dx < dst_w; ++dx) {
        const int sx = dx * src_w / dst_w;
        if (!((src_row[sx >> 3] >> (7 - (sx & 7))) & 1))
          set_ink(dx, dy);
      }
    }
  } else {
    // Box filter: every source pixel lands in exactly one destination cell, so
    // the whole image is read once, sequentially, with a row of counters as the
    // only extra state.
    std::vector<uint16_t> col_cnt(dst_w, 0);
    for (int sx = 0; sx < src_w; ++sx)
      ++col_cnt[sx * dst_w / src_w];
    std::vector<uint32_t> acc(dst_w, 0);
    int cur_dy = 0, rows_in = 0;

    auto flush = [&](int dy) {
      if (rows_in == 0 || dy >= dst_h) return;
      for (int dx = 0; dx < dst_w; ++dx) {
        const uint32_t total = static_cast<uint32_t>(col_cnt[dx]) * rows_in;
        if (total == 0) continue;
        const uint32_t thr = 2u * kBayer4[dy & 3][dx & 3] + 1u;
        if (acc[dx] * 32u > total * thr)
          set_ink(dx, dy);
      }
    };

    for (int sy = 0; sy < src_h; ++sy) {
      if (std::fread(src_row.data(), 1, src_stride, f) != static_cast<size_t>(src_stride))
        { cover_data_.clear(); break; }
      const int dy = sy * dst_h / src_h;
      if (dy != cur_dy) {
        flush(cur_dy);
        cur_dy = dy;
        rows_in = 0;
        std::fill(acc.begin(), acc.end(), 0u);
      }
      for (int sx = 0; sx < src_w; ++sx)
        if (!((src_row[sx >> 3] >> (7 - (sx & 7))) & 1))
          ++acc[sx * dst_w / src_w];
      ++rows_in;
    }
    if (!cover_data_.empty())
      flush(cur_dy);
  }

  // Trim all-white rows off the top and bottom. Plenty of covers carry a white
  // band above and below the artwork, and with a frame drawn a fixed distance
  // from the bitmap those bands read as lopsided padding. Columns are left
  // alone: the cover is fitted to the box width, so side bands are rare.
  if (!cover_data_.empty()) {
    auto row_blank = [&](int row) {
      const uint8_t* r = cover_data_.data() + static_cast<size_t>(row) * dst_stride;
      for (int b = 0; b < dst_stride; ++b)
        if (r[b] != 0xFF) return false;  // tail bits past dst_w are never written
      return true;
    };
    int top = 0, bot = dst_h - 1;
    while (top <= bot && row_blank(top)) ++top;
    while (bot > top && row_blank(bot)) --bot;
    if (top <= bot && (top > 0 || bot < dst_h - 1)) {
      const size_t off = static_cast<size_t>(top) * dst_stride;
      const size_t keep = static_cast<size_t>(bot - top + 1) * dst_stride;
      std::memmove(cover_data_.data(), cover_data_.data() + off, keep);
      cover_data_.resize(keep);
      dst_h = bot - top + 1;
    }
  }

  if (!cover_data_.empty()) {
    cover_w_ = static_cast<uint16_t>(dst_w);
    cover_h_ = static_cast<uint16_t>(dst_h);
  }
  std::fclose(f);
}

void HomeScreen::on_start() {
  const Rotation rot = current_rotation_();
  if (rot == Rotation::Deg0 || rot == Rotation::Deg180)
    set_buf_rotation_(Rotation::Deg90);

  if (!author_font_.valid())
    author_font_.init(kFontData_ui_large_mbf, kFontData_ui_large_mbf_size);

  if (app_ && app_->data_dir_) {
    const std::string idx_path = std::string(app_->data_dir_) + "/book_index.dat";
    BookIndex::instance().load(idx_path);
  }

  back_ignore_ = true;
  back_was_down_ = false;
  back_hold_ms_ = 0;
  back_consumed_ = false;

  num_books_ = 0;
  for (int i = 0; i < kMaxBooks; ++i)
    slots_[i] = BookSlot{};
  cover_slot_ = -1;
  cover_data_.clear();
  cover_w_ = cover_h_ = 0;

  // TODO rename this to just normal homescreen

  struct Raw { uint32_t order; BookSlot s; };
  std::vector<Raw> raw;
  const StringPool& pool = BookIndex::instance().pool();
  for (const auto& e : BookIndex::instance().entries()) {
    if (e.last_open_order == 0) continue;
    raw.push_back({e.last_open_order, {
      e.path.to_string(pool),
      std::string(e.title.view(pool)),
      std::string(e.author.view(pool)),
      {}, {},
    }});
  }
  std::stable_sort(raw.begin(), raw.end(), [](const Raw& a, const Raw& b) {
    return a.order > b.order;
  });

  num_books_ = static_cast<int>(std::min(raw.size(), static_cast<size_t>(kMaxBooks)));

  clear_items();
  for (int i = 0; i < num_books_; ++i) {
    slots_[i] = std::move(raw[i].s);
    if (app_ && app_->data_dir_) {
      slots_[i].bin_path = cover_bin_path(slots_[i].path.c_str(), app_->data_dir_);
      slots_[i].sleep_path = cover_sleep_bin_path(slots_[i].path.c_str(), app_->data_dir_);
    }
    add_item(slots_[i].title);
  }

  // The book just closed is now first, and on a cold boot the first entry is the
  // one last read either way — always land on it.
  set_selected(0);
}

void HomeScreen::on_select(int index) {
  if (!app_ || index < 0 || index >= num_books_) return;
  app_->record_book_opened(slots_[index].path);
  app_->reader()->set_path(slots_[index].path.c_str());
  app_->push_screen(ScreenId::Reader);
}

// Back is handled here rather than through on_back(): a tap opens the book list
// and a ~3 s hold opens it with the hidden books revealed, and the two can only
// be told apart when the button comes back up.
//
// Resolving on release is also what keeps the pushed screen clean. MainMenu used
// to own this gesture, and because the push happened on the *press*, MainMenu
// started with Button0 still physically down — its own release handler then fired
// immediately and popped it. The list appeared only for as long as the button was
// held. Any screen that pushes another on a button press has this problem.
void HomeScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  ButtonState fwd = buttons;
  const bool back_down = buttons.is_down(Button::Button0);

  // Strip Button0 from what the base class sees, always: it must never fire
  // on_back(), and a press that arrived before this screen started is dropped
  // here too.
  fwd.pressed_latch &= ~(1u << static_cast<uint8_t>(Button::Button0));
  {
    uint8_t nc = 0;
    for (uint8_t i = 0; i < fwd.press_history_count; ++i)
      if (static_cast<Button>(fwd.press_history[i]) != Button::Button0)
        fwd.press_history[nc++] =
            static_cast<uint8_t>(swap_updown_(static_cast<Button>(fwd.press_history[i])));
    fwd.press_history_count = nc;
  }
  // The carousel runs left-to-right, so "up" has to advance it: the top of the
  // side rocker and the bottom front key both move to the next book, the
  // opposite of what a vertical list wants. Swapping the buttons here keeps the
  // base class's list navigation untouched.
  {
    uint8_t cur = 0;
    for (uint8_t i = 0; i < ButtonState::kButtonCount; ++i)
      if (fwd.current & (1u << i))
        cur |= static_cast<uint8_t>(1u << static_cast<uint8_t>(swap_updown_(static_cast<Button>(i))));
    fwd.current = cur;
  }

  if (back_ignore_) {
    if (!back_down)
      back_ignore_ = false;
    ListMenuScreen::update(fwd, buf, runtime);
    return;
  }

  if (back_down) {
    back_was_down_ = true;
    if (!back_consumed_) {
      back_hold_ms_ += runtime.frame_time_ms();
      // The hidden list opens as soon as the hold is long enough, without
      // waiting for the release. Safe because presses are rising edges only:
      // MainMenu sees no Button0 event from a button that was already down when
      // it started, so it cannot immediately pop itself.
      if (back_hold_ms_ >= kHiddenHoldMs) {
        back_consumed_ = true;
        if (app_) {
          app_->main_menu()->set_show_hidden(true);
          app_->push_screen(ScreenId::MainMenu);
        }
        return;
      }
    }
  } else if (back_was_down_) {
    back_was_down_ = false;
    back_hold_ms_ = 0;
    // A hold that already opened the list resolves here as nothing at all.
    if (back_consumed_) {
      back_consumed_ = false;
    } else if (app_) {
      app_->main_menu()->set_show_hidden(false);
      app_->push_screen(ScreenId::MainMenu);
    }
    return;
  }
  ListMenuScreen::update(fwd, buf, runtime);
}

void HomeScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct) const {
  if (!ui_font_.valid()) return;
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);

  static constexpr int kPad        = 16;
  static constexpr int kHeroGap    = 24; // header rule → cover box
  static constexpr int kTitleGap   = 26; // cover box → title baseline block
  static constexpr int kAuthorGap  = 6;
  static constexpr int kDotsGap    = 26;
  static constexpr int kBottomPad  = 28;
  static constexpr int kDotR       = 5;   // filled diamond, both states
  static constexpr int kDotRing    = 9;   // outline diamond around the selected one
  static constexpr int kDotRSmall  = 3;
  static constexpr int kDotStep    = 28;
  static constexpr int kFrameGap   = 10;  // white gap between cover and its frame
  static constexpr int kFrameW     = 2;   // frame thickness

  const BitmapFont& tf = ui_font_;                                         // title
  const BitmapFont& af = author_font_.valid() ? author_font_ : ui_font_;   // author

  // Header battery — drawn by the base class so its position is identical on
  // every screen. Its height is the base's too, not kHeaderY.
  draw_battery_(buf, W, battery_pct);
  int y = battery_row_h_() + kHeroGap;

  // ── Cover box ────────────────────────────────────────────────────────────
  // Everything below the cover sits at a fixed height so the dots don't hop
  // around as covers of different shapes come and go.
  const int dots_h    = 2 * kDotRing + 1;
  const int below_h   = kTitleGap + 2 * tf.y_advance() + kAuthorGap + af.y_advance()
                        + kDotsGap + dots_h + kBottomPad;
  const int box_y     = y;
  const int box_w     = W - 2 * kPad;
  const int box_h     = std::max(0, H - box_y - below_h);

  if (num_books_ == 0) {
    draw_outline(buf, kPad, box_y, box_w, box_h);
    const int mid = box_y + box_h / 2;
    static const char kEmpty[] = "No books opened yet";
    static const char kHint[]  = "Press Back for all books";
    draw_centred(buf, W / 2, mid, kEmpty, std::strlen(kEmpty), tf, box_w);
    draw_centred(buf, W / 2, mid + af.y_advance() + 8, kHint, std::strlen(kHint), af, box_w);
    return;
  }

  const int sel = std::min(std::max(selected(), 0), num_books_ - 1);
  // The frame sits outside the cover, so the cover itself gets the box less the
  // gap and the frame on both sides.
  const int inset      = kFrameGap + kFrameW;
  const int cover_boxw = std::max(0, box_w - 2 * inset);
  const int cover_boxh = std::max(0, box_h - 2 * inset);
  if (cover_slot_ != sel) {
    // The card shares SPI2 with the panel, and the full-res cover is a much
    // longer read than the old thumbnail — never overlap it with a refresh.
    buf.wait_panel_idle();
    load_cover_(sel, cover_boxw, cover_boxh);
  }

  if (cover_w_ > 0 && cover_h_ > 0) {
    const int cx = kPad + (box_w - cover_w_) / 2;
    const int cy = box_y + (box_h - cover_h_) / 2;
    const int stride = (cover_w_ + 7) / 8;
    for (int row = 0; row < cover_h_; ++row)
      buf.blit_1bit_row(cx, cy + row, cover_data_.data() + static_cast<size_t>(row) * stride, cover_w_);
    draw_outline(buf, cx - inset, cy - inset, cover_w_ + 2 * inset, cover_h_ + 2 * inset, kFrameW);
  } else {
    // No cover on the card — a placeholder box keeps the layout intact.
    const int ph_w = std::min(box_w, box_h * 2 / 3);
    draw_outline(buf, kPad + (box_w - ph_w) / 2, box_y, ph_w, box_h, kFrameW);
  }

  y = box_y + box_h + kTitleGap;

  // ── Title (up to two lines) ──────────────────────────────────────────────
  // Only the lines actually drawn advance y, so the author always sits directly
  // under the last line of the title rather than under a reserved second line.
  {
    const std::string& t = slots_[sel].title;
    size_t off = 0;
    for (int line = 0; line < 2; ++line) {
      if (off >= t.size()) break;
      const int baseline = y + tf.baseline();
      if (line == 1) {
        // Last line: whatever is left, ellipsised if it still overflows.
        draw_centred(buf, W / 2, baseline, t.data() + off, t.size() - off, tf, W - 2 * kPad);
        off = t.size();
      } else {
        size_t next = 0;
        const size_t n = fit_line(tf, t.data() + off, t.size() - off, W - 2 * kPad, &next);
        buf.draw_text_proportional(
            W / 2 - tf.word_width(t.data() + off, n, FontStyle::Regular) / 2,
            baseline, t.data() + off, n, tf, false);
        off += next;
      }
      y += tf.y_advance();
    }
  }

  // ── Author ───────────────────────────────────────────────────────────────
  y += kAuthorGap;
  const std::string& a = slots_[sel].author;
  if (!a.empty())
    draw_centred(buf, W / 2, y + af.baseline(), a.data(), a.size(), af, W - 2 * kPad);

  // ── Dots ─────────────────────────────────────────────────────────────────
  // Anchored to the bottom of the panel, not to the end of the text: the title
  // is one or two lines depending on the book, and the dots must not move with
  // it. below_h reserves room for the taller case, so this never collides.
  const int dots_cy = H - kBottomPad - dots_h + kDotRing;
  const int dots_x0 = W / 2 - (num_books_ - 1) * kDotStep / 2;
  for (int i = 0; i < num_books_; ++i) {
    const int dx = dots_x0 + i * kDotStep;
    if (i == sel) {
      draw_diamond(buf, dx, dots_cy, kDotR);
      draw_diamond_outline(buf, dx, dots_cy, kDotRing, kFrameW);
    } else {
      draw_diamond(buf, dx, dots_cy, kDotRSmall);
    }
  }
}

}  // namespace wintergreen
