#include "ListMenuScreen.h"

#include <algorithm>
#include <cstdio>
#include <cstring>


#include "../Application.h"
#include "../display/ui_font_header.h"
#include "../display/ui_font_large.h"

namespace wintergreen {

static constexpr int kHeaderBottomGap = 4;  // gap between last header line and first list item
// Nothing is drawn at the bottom of a list any more — only a breathing margin so
// the last row isn't flush against the panel edge.
static constexpr int kBottomPad = 8;

void ListMenuScreen::start(DrawBuffer& buf, IRuntime& runtime) {
  buf_ = &buf;
  ui_font_.init(kFontData_ui_header_wgf, kFontData_ui_header_wgf_size);
  section_font_.init(kFontData_ui_large_wgf, kFontData_ui_large_wgf_size);
  const int prev_selected = selected_;
  clear_items();
  on_start_set_selection_ = false;
  on_start();
  // Restore selection if on_start() didn't explicitly call set_selected()
  // (e.g. returning from a sub-screen after navigating away).
  if (!on_start_set_selection_ && prev_selected > 0 && prev_selected < count())
    selected_ = prev_selected;
  if (on_start_set_selection_ || selected_ > 0)
    center_on_selected_();
  else
    ensure_visible_();
  while (selected_ < count() && !is_item_focusable(selected_))
    ++selected_;
  draw_all_(buf, runtime.battery_percentage());
}

int ListMenuScreen::wintergreen_slot_h_(int index) const {
  static constexpr int kPadT = 5, kGap = 3, kPadB = 6;
  // A row that will never carry a subtitle gets the title line only — reserving
  // the second line leaves it looking like a row with something missing.
  if (index >= 0 && is_single_line_row(index))
    return kPadT + ui_font_.y_advance() + kPadB + 1;
  return kPadT + detail_title_lines_ * ui_font_.y_advance() + kGap + section_font_.y_advance() + kPadB + 1;
}

int ListMenuScreen::wintergreen_visible_from_(int scroll_off, int available_h) const {
  int cnt = 0, h = 0, n = count();
  for (int i = scroll_off; i < n; i++) {
    const int ih = is_separator(i) ? kSeparatorH : wintergreen_slot_h_(i);
    if (h + ih > available_h) break;
    h += ih; cnt++;
  }
  return cnt;
}

int ListMenuScreen::get_visible_count_(int H, int scroll_off) const {
  return wintergreen_visible_from_(scroll_off, H - header_h_() - kBottomPad);
}

void ListMenuScreen::ensure_visible_() {
  if (!ui_font_.valid() || count() == 0 || !buf_) return;
  const int H = buf_->height();
  int visible = get_visible_count_(H, scroll_offset_);
  if (visible <= 0) return;
  if (selected_ < scroll_offset_)
    scroll_offset_ = selected_;
  else if (selected_ >= scroll_offset_ + visible)
    scroll_offset_ = selected_ - visible + 1;
  // Recompute from the updated offset: separators at different positions mean
  // the actual count at the new offset can differ from the pre-adjustment count.
  visible = get_visible_count_(H, scroll_offset_);
  static constexpr int kPad = 2;
  if (selected_ - scroll_offset_ < kPad && scroll_offset_ > 0)
    scroll_offset_ = std::max(0, selected_ - kPad);
  else if (selected_ - scroll_offset_ > visible - 1 - kPad) {
    const int max_scroll = count() > visible ? count() - visible : 0;
    scroll_offset_ = std::min(max_scroll, selected_ - (visible - 1 - kPad));
  }
}

void ListMenuScreen::center_on_selected_() {
  if (!ui_font_.valid() || count() == 0 || !buf_) return;
  const int visible = get_visible_count_(buf_->height(), 0);
  if (visible <= 0) return;
  const int max_scroll = count() > visible ? count() - visible : 0;
  scroll_offset_ = std::max(0, std::min(max_scroll, selected_ - visible / 2));
}

void ListMenuScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct) const {
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);
  // Battery percentage and nothing else: no wordmark, and no rule under it.
  // Same helper the home screen calls, so the two land on the same pixel.
  draw_battery_(buf, W, battery_pct);
  draw_list_(buf, W, H, header_h_());
}

void ListMenuScreen::draw_battery_(DrawBuffer& buf, int W, std::optional<uint8_t> battery_pct) const {
  if (!battery_pct || !section_font_.valid())
    return;
  char pbuf[8];
  std::snprintf(pbuf, sizeof(pbuf), "%u%%", static_cast<unsigned>(*battery_pct));
  const size_t len = std::strlen(pbuf);
  const int pw = section_font_.word_width(pbuf, len, FontStyle::Regular);
  buf.draw_text_proportional(W - kBatteryPadX - pw, kBatteryPadY + section_font_.baseline(), pbuf, len, section_font_,
                             false);
}

// Measured from the font the battery is actually drawn in (battery_row_h_), not
// the header face. Sizing it off the header face reserved a 41 px row for 31 px
// of text, which read as a lopsided gap above the first entry.
int ListMenuScreen::header_h_() const {
  return battery_row_h_() + kHeaderBottomGap;
}

// Two-line rows: title (detail_title_lines_ lines) + subtitle, a full-width
// divider, the right-hand column and the thumbnail column.
void ListMenuScreen::draw_list_(DrawBuffer& buf, int W, int H, int header_h) const {
  const int n = count();
  if (n == 0 || !ui_font_.valid())
    return;

  static constexpr int kPadT = 5, kGap = 3;  // slot height comes from wintergreen_slot_h_
  static constexpr int kLM = 14, kRM = 12;
  const int title_h = ui_font_.y_advance();
  const BitmapFont& sub_font = section_font_;
  const int available_h = H - header_h - kBottomPad;

  const int visible = wintergreen_visible_from_(scroll_offset_, available_h);
  if (visible <= 0) return;
  const int end = std::min(scroll_offset_ + visible, n);
  const bool has_scrollbar = (n > visible);
  const int sb_reserved = has_scrollbar ? 8 : 0;

  char trunc_buf[260];

  // Thumbnail column at the right-hand end of every two-line row, inset from
  // the row's edges by kThumbPad. The divider is the slot's last pixel row.
  static constexpr int kThumbPad = 6, kThumbGap = 12;
  const int thumb_box_h = wintergreen_slot_h_(-1) - 1 - 2 * kThumbPad;
  const int thumb_w = prepare_thumbs(thumb_box_h);

  int y = header_h;
  for (int i = scroll_offset_; i < end; ++i) {
    const bool sel = (i == selected_);

    if (is_separator(i)) {
      draw_separator_(buf, W, y);
      y += kSeparatorH;
      continue;
    }

    const int slot_h = wintergreen_slot_h_(i);
    if (sel)
      buf.fill_rect(0, y, W, slot_h - 1, false);

    const std::string_view label = get_item_label(i);
    const std::string_view sub = get_item_subtitle(i);
    const std::string_view right_txt = get_item_right(i);

    // Right column (reading percentage). It sits on the *subtitle* baseline,
    // so the title gets the full width and only the author has to make room.
    int right_w_actual = 0;
    if (!right_txt.empty())
      right_w_actual = sub_font.word_width(right_txt.data(), right_txt.size(), FontStyle::Regular);
    const int right_reserved = right_txt.empty() ? 0 : (right_w_actual + 8);
    // Text ends where the thumbnail column (if any) begins.
    const bool row_thumb = thumb_w > 0 && !is_single_line_row(i);
    const int text_r = W - kRM - sb_reserved - (row_thumb ? thumb_w + kThumbGap : 0);
    const int max_lw = text_r - kLM;

    // Title: up to detail_title_lines_ lines. Every line but the last breaks
    // after the last whole word that fits (mid-word only for a single word
    // wider than the column); the last takes the rest, ellipsised.
    {
      const char* p = label.data();
      size_t left = label.size();
      for (int line = 0; line < detail_title_lines_ && left > 0; ++line) {
        const int title_y = y + kPadT + line * title_h + ui_font_.baseline();
        const bool last = (line == detail_title_lines_ - 1);
        if (ui_font_.word_width(p, left, FontStyle::Regular) <= max_lw) {
          buf.draw_text_proportional(kLM, title_y, p, left, ui_font_, sel);
          break;
        }
        if (last) {
          const std::string_view t = ellipsize(ui_font_, p, left, max_lw, trunc_buf);
          buf.draw_text_proportional(kLM, title_y, t.data(), t.size(), ui_font_, sel);
          break;
        }
        const size_t fit = fit_prefix(ui_font_, p, left, max_lw);
        size_t brk = fit;
        while (brk > 0 && p[brk] != ' ')
          --brk;
        if (brk == 0)
          brk = fit;  // one word wider than the column: cut it
        if (brk == 0)
          break;      // not even a glyph fits
        buf.draw_text_proportional(kLM, title_y, p, brk, ui_font_, sel);
        p += brk;
        left -= brk;
        while (left > 0 && *p == ' ') {
          ++p;
          --left;
        }
      }
    }

    const int sub_baseline = y + kPadT + detail_title_lines_ * title_h + kGap + sub_font.baseline();

    // Right-aligned percentage, on the author's baseline.
    if (!right_txt.empty()) {
      const int right_x = text_r - right_w_actual;
      buf.draw_text_proportional(right_x, sub_baseline, right_txt.data(), right_txt.size(), sub_font, sel);
    }

    // Subtitle line (author)
    if (!sub.empty()) {
      const std::string_view s = ellipsize(sub_font, sub.data(), sub.size(), text_r - kLM - right_reserved, trunc_buf);
      buf.draw_text_proportional(kLM, sub_baseline, s.data(), s.size(), sub_font, sel);
    }

    // Cover thumbnail, centred in its box. The image keeps its own paper
    // white on a selected (inverted) row, so it reads as a cover rather than
    // a negative. A book without one gets an empty frame in the row's ink.
    if (row_thumb) {
      const int bx = W - kRM - sb_reserved - thumb_w;
      const int by = y + kThumbPad;
      Thumb t;
      if (get_item_thumb(i, t) && t.bits && t.w > 0 && t.h > 0) {
        const int tx = bx + (thumb_w - t.w) / 2;
        const int ty = by + (thumb_box_h - t.h) / 2;
        buf.blit_1bit_cols(tx, ty, t.bits, t.w, t.h);
      } else {
        buf.fill_rect(bx, by, thumb_w, 1, sel);
        buf.fill_rect(bx, by + thumb_box_h - 1, thumb_w, 1, sel);
        buf.fill_rect(bx, by, 1, thumb_box_h, sel);
        buf.fill_rect(bx + thumb_w - 1, by, 1, thumb_box_h, sel);
      }
    }

    // Full-width per-book divider. The last row has nothing below it to be
    // divided from, so it gets none — nor does a row followed by a section
    // hairline, which would otherwise draw two rules a few pixels apart.
    if (i != n - 1 && !is_separator(i + 1))
      buf.fill_rect(0, y + slot_h - 1, W, 1, false);
    y += slot_h;
  }
  end_thumbs();

  // Scrollbar
  if (has_scrollbar) {
    const int sb_x = W - 4;
    const int thumb_h = std::max(available_h * visible / n, 16);
    const int track = available_h - thumb_h;
    const int max_scroll = n - visible;
    const int thumb_y = header_h + (max_scroll > 0 ? track * scroll_offset_ / max_scroll : 0);
    buf.fill_rect(sb_x, thumb_y, 3, thumb_h, false);
  }
}

void ListMenuScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  buf_ = &buf;
  const int n = count();

  // Helper lambdas for selection movement (skip separators).
  auto move_up = [&]() {
    int next = selected_ > 0 ? selected_ - 1 : n - 1;
    while (next != selected_ && !is_item_focusable(next))
      next = next > 0 ? next - 1 : n - 1;
    selected_ = next;
    ensure_visible_();
  };
  auto move_down = [&]() {
    int next = selected_ < n - 1 ? selected_ + 1 : 0;
    while (next != selected_ && !is_item_focusable(next))
      next = next < n - 1 ? next + 1 : 0;
    selected_ = next;
    ensure_visible_();
  };

  bool moved = false;       // selection changed — needs a redraw
  bool needs_draw = false;  // on_select returned true — needs a redraw

  // Front row: Button3=up, Button2=down. Side rocker: Up=up, Down=down.
  constexpr Button logical_up_front = Button::Button3;
  constexpr Button logical_down_front = Button::Button2;
  constexpr Button logical_up_side = Button::Up;
  constexpr Button logical_down_side = Button::Down;

  Button btn;
  while (buttons.next_press(btn)) {
    if (btn == logical_up_front || btn == logical_up_side) {
      if (n > 0) {
        undo_selected_ = selected_;
        move_up();
        moved = true;
      }
    } else if (btn == logical_down_front || btn == logical_down_side) {
      if (n > 0) {
        undo_selected_ = selected_;
        move_down();
        moved = true;
      }
    } else {
      switch (btn) {
        case Button::Undo:
          if (undo_selected_ >= 0 && undo_selected_ < n && undo_selected_ != selected_) {
            selected_ = undo_selected_;
            ensure_visible_();
            moved = true;
          }
          undo_selected_ = -1;
          break;

        case Button::Button0:
          // Back first, draw after: a pending move (an Undo from a clicker
          // hold) must not cost a refresh of a screen that is about to go. If
          // on_back() stays, `moved` still draws it below.
          on_back();
          if (app_ && app_->has_pending_transition()) {
            return;
          }
          break;

        case Button::Button1:  // select
          if (n > 0 && selected_ < n) {
            on_select(selected_);
            if (app_ && app_->has_pending_transition()) {
              return;
            }
            needs_draw = true;
          }
          break;

        default:
          break;
      }
    }
  }

  // Hold-down: repeats start after config::kHoldDelayMs, then come every
  // config::kHoldRepeatMs, with the step growing by config::kHoldAccelStep entries
  // per repeat so a long hold accelerates through the list. Driven by elapsed time
  // rather than frames — the press itself is already handled above, and frame
  // duration swings with render and panel work, so counting frames made a tap
  // register as two presses whenever frames were short.
  const uint32_t dt_ms = runtime.frame_time_ms();
  const int up_repeats = hold_up_.tick(buttons.is_down(logical_up_front) || buttons.is_down(logical_up_side), dt_ms);
  const int down_repeats =
      hold_down_.tick(buttons.is_down(logical_down_front) || buttons.is_down(logical_down_side), dt_ms);

  // The step is read into a local before the inner loop. Writing this as
  // `for (i = 0; i < ++hold_reps_up_; ++i)` re-evaluates the increment in the
  // condition every pass, so the bound outruns i and the loop never ends.
  // Repeat r moves 1 + r*kHoldAccelStep entries, capped at the list length —
  // beyond that it is just spinning around a list it has already crossed.
  auto step_for = [n](int repeat_index) {
    const int step = 1 + repeat_index * config::kHoldAccelStep;
    return step < n ? step : n;
  };

  if (up_repeats > 0 && n > 0) {
    for (int r = 0; r < up_repeats; ++r) {
      const int step = step_for(hold_reps_up_++);
      for (int i = 0; i < step; ++i)
        move_up();
    }
    moved = true;
  } else if (hold_up_.idle()) {
    hold_reps_up_ = 0;
  }

  if (down_repeats > 0 && n > 0) {
    for (int r = 0; r < down_repeats; ++r) {
      const int step = step_for(hold_reps_down_++);
      for (int i = 0; i < step; ++i)
        move_down();
    }
    moved = true;
  } else if (hold_down_.idle()) {
    hold_reps_down_ = 0;
  }

  if (moved || needs_draw || force_redraw_) {
    draw_all_(buf, runtime.battery_percentage());
    buf.refresh();
    force_redraw_ = false;
  }
}

void ListMenuScreen::on_back() {
  if (app_)
    app_->pop_screen();
}

}  // namespace wintergreen
