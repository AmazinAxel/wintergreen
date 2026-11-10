#include "HomeScreen.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../Application.h"
#include "../content/CoverPaths.h"
#include "../content/BookIndex.h"

namespace wintergreen {

// Largest bitmap load_cover_ accepts: it never scales beyond the layout box, so
// this bounds every cover, and it has to fit the spare it is read into.
static constexpr size_t kCoverBytesMax =
    static_cast<size_t>((kHomeCoverW + 7) / 8) * kHomeCoverH;
static_assert(kCoverBytesMax <= DrawBuffer::kBufSize, "home cover must fit the spare framebuffer");

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
  const size_t fit = fit_prefix(f, text, len, max_w);
  size_t last_space = 0;
  for (size_t i = 0; i < fit; ++i)
    if (text[i] == ' ') last_space = i;
  if (fit >= len) { *next = len; return len; }
  if (last_space > 0) { *next = last_space + 1; return last_space; }
  *next = fit;
  return fit;
}

// Centred text, truncated with an ellipsis when it cannot fit in max_w.
static void draw_centred(DrawBuffer& buf, int cx, int baseline, const char* text, size_t len,
                         const BitmapFont& f, int max_w) {
  if (len == 0) return;
  char trunc[260];
  const std::string_view t = ellipsize(f, text, len, max_w, trunc);
  const int tw = f.word_width(t.data(), t.size(), FontStyle::Regular);
  buf.draw_text_proportional(cx - tw / 2, baseline, t.data(), t.size(), f, false);
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

void HomeScreen::load_cover_(DrawBuffer& buf, int i, int box_w, int box_h) const {
  cover_slot_ = i;
  cover_w_ = cover_h_ = 0;
  cover_bits_ = nullptr;
  if (i < 0 || i >= num_books_ || slots_[i].home_path.empty())
    return;
  FILE* f = std::fopen(slots_[i].home_path.c_str(), "rb");
  if (!f)
    return;
  uint16_t hdr[2] = {};
  bool ok = std::fread(hdr, 2, 2, f) == 2 && hdr[0] > 0 && hdr[1] > 0 && hdr[0] <= box_w && hdr[1] <= box_h;
  const size_t need = ok ? static_cast<size_t>((hdr[0] + 7) / 8) * hdr[1] : 0;
  ok = ok && need <= kCoverBytesMax;
  // Claimed after the last allocation (newlib takes the FILE buffer on the
  // first fread), so the OOM handler cannot release it between here and use.
  uint8_t* dst = ok ? buf.claim_spare_cache(this) : nullptr;
  // Exactly the declared size: anything else is not a cover (a truncated
  // download, or an HTML error page saved under the name).
  ok = dst && std::fread(dst, 1, need, f) == need && std::fgetc(f) == EOF;
  std::fclose(f);
  if (!ok)
    return;
  cover_bits_ = dst;
  cover_gen_ = buf.spare_cache_gen();
  cover_w_ = hdr[0];
  cover_h_ = hdr[1];
}

void HomeScreen::on_start() {
  set_buf_rotation_(Rotation::Deg90);

  // Same rule as MainMenu::on_start(): the resident index is authoritative, so
  // only parse book_index.dat when nothing is loaded (a cold boot). Backing out
  // of a book used to re-read and re-parse the whole file off the SD card here.
  if (app_ && app_->data_dir_ && BookIndex::instance().entries().empty()) {
    const std::string& idx_path = app_->index_path();
    BookIndex::instance().load(idx_path);
  }

  back_ignore_ = true;
  back_was_down_ = false;
  back_hold_ms_ = 0;
  back_consumed_ = false;

  num_books_ = 0;
  for (int i = 0; i < kMaxBooks; ++i)
    slots_[i] = BookSlot{};
  // slots_ is about to be rebuilt from the book index, so a cached slot index
  // is meaningless even where the number is unchanged — a sync or a serial
  // upload reorders the recents under us.
  cover_slot_ = -1;
  cover_bits_ = nullptr;
  cover_w_ = cover_h_ = 0;

  // Pick the kMaxBooks most recently opened, by selection rather than by
  // sorting. std::stable_sort over a struct holding six std::strings cost
  // ~20 KB of flash here — mergesort, insertion sort and __rotate all
  // instantiated for a list that is then truncated to five — and it built
  // path/title/author for every book ever opened just to discard all but those
  // five. This walks the index kMaxBooks times instead and copies nothing until
  // a winner is known

  const auto& entries = BookIndex::instance().entries();
  const StringPool& pool = BookIndex::instance().pool();
  int chosen[kMaxBooks];
  uint32_t taken_above = UINT32_MAX;  // strictly-less bound from the previous pick
  for (int slot = 0; slot < kMaxBooks; ++slot) {
    int best = -1;
    uint32_t best_order = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
      const uint32_t o = entries[i].last_open_order;
      // 0 means never opened. Orders are unique per book (mark_opened derives
      // each from max+1), so `< taken_above` cannot skip a tie.
      if (o == 0 || o >= taken_above || o <= best_order)
        continue;
      best_order = o;
      best = static_cast<int>(i);
    }
    if (best < 0)
      break;
    chosen[slot] = best;
    taken_above = best_order;
    ++num_books_;
  }

  // Nothing has been opened yet — a fresh card, or settings that were wiped.
  // Rather than an empty carousel telling the reader there is nothing here, show
  // the start of the library. Ordering matches the book list's never-opened
  // group (alphabetical by title) so the same book is "first" in both places.
  if (num_books_ == 0 && !entries.empty()) {
    for (size_t i = 0; i < entries.size(); ++i) {
      if (num_books_ < kMaxBooks) {
        chosen[num_books_++] = static_cast<int>(i);
        continue;
      }
      // Full: replace the current last if this title sorts before it.
      int worst_slot = 0;
      for (int s = 1; s < num_books_; ++s)
        if (ci_less(entries[chosen[worst_slot]].title.view(pool), entries[chosen[s]].title.view(pool)))
          worst_slot = s;
      if (ci_less(entries[i].title.view(pool), entries[chosen[worst_slot]].title.view(pool)))
        chosen[worst_slot] = static_cast<int>(i);
    }
    // Order the chosen few among themselves.
    for (int a = 1; a < num_books_; ++a) {
      const int v = chosen[a];
      int b = a - 1;
      while (b >= 0 && ci_less(entries[v].title.view(pool), entries[chosen[b]].title.view(pool))) {
        chosen[b + 1] = chosen[b];
        --b;
      }
      chosen[b + 1] = v;
    }
  }

  clear_items();
  for (int i = 0; i < num_books_; ++i) {
    const BookIndexEntry& e = entries[chosen[i]];
    slots_[i].path = e.path.to_string(pool);
    slots_[i].title.assign(e.title.view(pool));
    slots_[i].author.assign(e.author.view(pool));
    slots_[i].home_path = cover_home_bin_path(slots_[i].path.c_str());
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

  // A rising edge with the button not down is an injected press (the BLE
  // clicker sets only pressed_latch, never current), so there is no release to
  // wait for and no hold to distinguish. Open the plain list at once.
  if (!back_down && buttons.is_pressed(Button::Button0) && !back_was_down_) {
    back_hold_ms_ = 0;
    if (app_) {
      app_->main_menu()->set_show_hidden(false);
      app_->push_screen(ScreenId::MainMenu);
    }
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
  // 24, not 26: kDotRing grew by 1 (2 px of dots_h), and kHomeCoverH mirrors
  // this layout. Absorbing it here keeps box_h — and so the cover — unchanged.
  static constexpr int kDotsGap    = 24;
  static constexpr int kBottomPad  = 28;
  static constexpr int kDotR       = 5;   // filled diamond, selected
  // Outline diamond around the selected one: 2 px thick, held 3 px clear of the
  // filled dot. Inner edge is kDotRing - kDotRingW, so this is kDotR + 3 + 2.
  static constexpr int kDotRing    = 10;
  static constexpr int kDotRingW   = 2;
  static constexpr int kDotRSmall  = 3;   // unselected: a step down from the filled one
  static constexpr int kDotStep    = 28;
  static constexpr int kFrameGap   = 8;  // white gap between cover and its frame
  static constexpr int kFrameW     = 2;  // frame thickness

  const BitmapFont& tf = ui_font_;                                         // title
  const BitmapFont& af = section_font_;                                    // author

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

  // Only reachable if the card is genuinely empty, and Application::start
  // normally routes that case to the book list instead. Nothing is drawn.
  if (num_books_ == 0)
    return;

  const int sel = std::min(std::max(selected(), 0), num_books_ - 1);
  // The frame sits outside the cover, so the cover itself gets the box less the
  // gap and the frame on both sides.
  const int inset      = kFrameGap + kFrameW;
  const int cover_boxw = std::max(0, box_w - 2 * inset);
  const int cover_boxh = std::max(0, box_h - 2 * inset);
  // Reload on a new selection, or when the cover was loaded but something has
  // since taken the spare. A book with no cover, or no spare at all, is not
  // retried on every draw.
  if (cover_slot_ != sel || (cover_bits_ && !have_cover_(buf))) {
    // The card shares SPI2 with the panel; never overlap the read with a refresh.
    buf.wait_panel_idle();
    load_cover_(buf, sel, cover_boxw, cover_boxh);
  }

  if (have_cover_(buf)) {
    const int cx = kPad + (box_w - cover_w_) / 2;
    const int cy = box_y + (box_h - cover_h_) / 2;
    buf.blit_1bit(cx, cy, cover_bits_, cover_w_, cover_h_, (cover_w_ + 7) / 8);
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
      draw_diamond_outline(buf, dx, dots_cy, kDotRing, kDotRingW);
    } else {
      draw_diamond(buf, dx, dots_cy, kDotRSmall);
    }
  }
}

}  // namespace wintergreen
