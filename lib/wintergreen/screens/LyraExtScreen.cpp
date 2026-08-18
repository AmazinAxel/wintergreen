#include "LyraExtScreen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../Application.h"
#include "../content/CoverPaths.h"
#include "../content/BookIndex.h"
#include "../display/ui_font_large.h"

namespace wintergreen {

// Filled disc, drawn as one fill_rect per row.
static void draw_dot(DrawBuffer& buf, int cx, int cy, int r) {
  for (int dy = -r; dy <= r; ++dy) {
    const int span = static_cast<int>(std::sqrt(static_cast<double>(r * r - dy * dy)));
    buf.fill_rect(cx - span, cy + dy, 2 * span + 1, 1, false);
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

// 1px border outline around a rectangle.
static void draw_outline(DrawBuffer& buf, int x, int y, int w, int h) {
  buf.fill_rect(x,         y,         w, 1, false);
  buf.fill_rect(x,         y + h - 1, w, 1, false);
  buf.fill_rect(x,         y,         1, h, false);
  buf.fill_rect(x + w - 1, y,         1, h, false);
}

void LyraExtScreen::load_cover_(int i, int box_w, int box_h) const {
  cover_data_.clear();
  cover_w_ = cover_h_ = 0;
  cover_slot_ = i;
  if (i < 0 || i >= num_books_ || slots_[i].bin_path.empty()) return;
  FILE* f = std::fopen(slots_[i].bin_path.c_str(), "rb");
  if (!f) return;
  uint16_t hdr[2] = {};
  if (std::fread(hdr, 2, 2, f) != 2) { std::fclose(f); return; }
  const int src_w = hdr[0], src_h = hdr[1];
  if (src_w <= 0 || src_h <= 0) { std::fclose(f); return; }

  // Upscale by a whole number only. The converter writes 160x240 covers, which
  // land on an exact 2x here, and pixel-doubling keeps the 1-bit dither intact
  // where a fractional nearest-neighbour scale would moiré it. Only an
  // oversized cover takes the fitting path.
  int dst_w, dst_h;
  if (src_w <= box_w && src_h <= box_h) {
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
  int prev_sy = -1;
  for (int dy = 0; dy < dst_h; ++dy) {
    const int sy = dy * src_h / dst_h;
    if (sy != prev_sy) {
      std::fseek(f, 4 + static_cast<long>(sy) * src_stride, SEEK_SET);
      if (std::fread(src_row.data(), 1, src_stride, f) != static_cast<size_t>(src_stride))
        { cover_data_.clear(); break; }
      prev_sy = sy;
    }
    uint8_t* dr = cover_data_.data() + static_cast<size_t>(dy) * dst_stride;
    for (int dx = 0; dx < dst_w; ++dx) {
      const int sx = dx * src_w / dst_w;
      if (!((src_row[sx >> 3] >> (7 - (sx & 7))) & 1))
        dr[dx >> 3] &= static_cast<uint8_t>(~(1u << (7 - (dx & 7))));
    }
  }
  if (!cover_data_.empty()) {
    cover_w_ = static_cast<uint16_t>(dst_w);
    cover_h_ = static_cast<uint16_t>(dst_h);
  }
  std::fclose(f);
}

void LyraExtScreen::on_start() {
  // Menus are portrait-only; the reader may leave the buffer in landscape.
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

  num_books_ = 0;
  for (int i = 0; i < kMaxBooks; ++i)
    slots_[i] = BookSlot{};
  cover_slot_ = -1;
  cover_data_.clear();
  cover_w_ = cover_h_ = 0;

  struct Raw { uint32_t order; BookSlot s; };
  std::vector<Raw> raw;
  const StringPool& pool = BookIndex::instance().pool();
  for (const auto& e : BookIndex::instance().entries()) {
    if (e.last_open_order == 0) continue;
    raw.push_back({e.last_open_order, {
      e.path.to_string(pool),
      std::string(e.title.view(pool)),
      std::string(e.author.view(pool)),
      {},
    }});
  }
  std::stable_sort(raw.begin(), raw.end(), [](const Raw& a, const Raw& b) {
    return a.order > b.order;
  });

  num_books_ = static_cast<int>(std::min(raw.size(), static_cast<size_t>(kMaxBooks)));

  clear_items();
  for (int i = 0; i < num_books_; ++i) {
    slots_[i] = std::move(raw[i].s);
    if (app_ && app_->data_dir_)
      slots_[i].bin_path = cover_bin_path(slots_[i].path.c_str(), app_->data_dir_);
    add_item(slots_[i].title);
  }

  // The book just closed is now first, and on a cold boot the first entry is the
  // one last read either way — always land on it.
  set_selected(0);
}

void LyraExtScreen::on_select(int index) {
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
void LyraExtScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
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
        fwd.press_history[nc++] = fwd.press_history[i];
    fwd.press_history_count = nc;
  }

  if (back_ignore_) {
    if (!back_down)
      back_ignore_ = false;
    ListMenuScreen::update(fwd, buf, runtime);
    return;
  }

  if (back_down) {
    if (back_hold_ms_ <= kHiddenHoldMs)
      back_hold_ms_ += runtime.frame_time_ms();
    back_was_down_ = true;
  } else if (back_was_down_) {
    back_was_down_ = false;
    const uint32_t held = back_hold_ms_;
    back_hold_ms_ = 0;
    if (app_) {
      app_->main_menu()->set_show_hidden(held >= kHiddenHoldMs);
      app_->push_screen(ScreenId::MainMenu);
    }
    return;
  }
  ListMenuScreen::update(fwd, buf, runtime);
}

void LyraExtScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct) const {
  if (!ui_font_.valid()) return;
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);

  static constexpr int kPad        = 16;
  static constexpr int kHeaderY    = 10;
  static constexpr int kHeroGap    = 24;   // header rule → cover box
  static constexpr int kTitleGap   = 26;   // cover box → title baseline block
  static constexpr int kAuthorGap  = 8;
  static constexpr int kDotsGap    = 26;
  static constexpr int kBottomPad  = 28;
  static constexpr int kDotR       = 5;    // selected dot radius
  static constexpr int kDotRSmall  = 3;
  static constexpr int kDotStep    = 24;

  const BitmapFont& tf = ui_font_;                                        // title
  const BitmapFont& af = author_font_.valid() ? author_font_ : ui_font_;  // author
  const BitmapFont& bf = section_font_.valid() ? section_font_ : ui_font_;  // battery

  // ── Header: battery only, top right ──────────────────────────────────────
  int y = kHeaderY;
  if (battery_pct) {
    char pbuf[8];
    std::snprintf(pbuf, sizeof(pbuf), "%u%%", static_cast<unsigned>(*battery_pct));
    const int pw = bf.word_width(pbuf, std::strlen(pbuf), FontStyle::Regular);
    buf.draw_text_proportional(W - kPad - pw, y + bf.baseline(), pbuf, bf, false);
  }
  y += bf.y_advance() + kHeroGap;

  // ── Cover box ────────────────────────────────────────────────────────────
  // Everything below the cover sits at a fixed height so the dots don't hop
  // around as covers of different shapes come and go.
  const int dots_h    = 2 * kDotR + 1;
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
  if (cover_slot_ != sel)
    load_cover_(sel, box_w, box_h);

  if (cover_w_ > 0 && cover_h_ > 0) {
    const int cx = kPad + (box_w - cover_w_) / 2;
    const int cy = box_y + (box_h - cover_h_) / 2;
    const int stride = (cover_w_ + 7) / 8;
    for (int row = 0; row < cover_h_; ++row)
      buf.blit_1bit_row(cx, cy + row, cover_data_.data() + static_cast<size_t>(row) * stride, cover_w_);
    draw_outline(buf, cx - 1, cy - 1, cover_w_ + 2, cover_h_ + 2);
  } else {
    // No cover on the card — a placeholder box keeps the layout intact.
    const int ph_w = std::min(box_w, box_h * 2 / 3);
    draw_outline(buf, kPad + (box_w - ph_w) / 2, box_y, ph_w, box_h);
  }

  y = box_y + box_h + kTitleGap;

  // ── Title (up to two lines) ──────────────────────────────────────────────
  {
    const std::string& t = slots_[sel].title;
    size_t off = 0;
    for (int line = 0; line < 2; ++line) {
      if (off >= t.size()) { y += tf.y_advance(); continue; }
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
  y += af.y_advance() + kDotsGap;

  // ── Dots ─────────────────────────────────────────────────────────────────
  const int dots_cy = y + kDotR;
  const int dots_x0 = W / 2 - (num_books_ - 1) * kDotStep / 2;
  for (int i = 0; i < num_books_; ++i)
    draw_dot(buf, dots_x0 + i * kDotStep, dots_cy, i == sel ? kDotR : kDotRSmall);
}

}  // namespace wintergreen
