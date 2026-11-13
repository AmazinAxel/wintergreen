#pragma once

#include <cstdint>
#include <cstring>
#include <deque>
#include <string_view>
#include <vector>

#include "../Input.h"
#include "../display/DrawBuffer.h"
#include "IScreen.h"

namespace wintergreen {

// Longest prefix of `text` whose width fits `budget`, cut on a codepoint
// boundary. Returns a byte count.
//
// Accumulates per-character widths instead of re-measuring the prefix from the
// start each step: the obvious form calls word_width(text, fit + cb) inside the
// loop, which re-scans every preceding character and makes truncating one label
// O(n^2). Book titles are long enough for that to be the bulk of a list repaint.
//
// The per-character sum is a hair wider than word_width() over the same bytes,
// because word_width() applies class kerning between adjacent glyphs. Kerning is
// negative far more often than not, so this cuts at most a character early and
// never overruns the budget — the safe direction for a truncation.
//
// Bounded by `len`: the sources here are string_views into StringPool chunks,
// which are packed back to back and are *not* NUL-terminated, so a loop testing
// `*p` runs off the end of the string into the next one.
inline size_t fit_prefix(const IFont& f, const char* text, size_t len, int budget) {
  if (budget <= 0) return 0;
  size_t fit = 0;
  int w = 0;
  while (fit < len) {
    const uint8_t b = static_cast<uint8_t>(text[fit]);
    const size_t cb = b < 0x80 ? 1u : b < 0xE0 ? 2u : b < 0xF0 ? 3u : 4u;
    if (fit + cb > len) break;
    const char* p = text + fit;
    char32_t cp = static_cast<char32_t>(b);
    if (cb == 2) cp = ((b & 0x1F) << 6) | (p[1] & 0x3F);
    else if (cb == 3) cp = ((b & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    else if (cb == 4) cp = ((b & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    w += f.char_width(cp, FontStyle::Regular);
    if (w > budget) break;
    fit += cb;
  }
  return fit;
}

// `text` as it should be drawn in max_w: unchanged when it fits, otherwise the
// longest prefix that leaves room for "..." (via fit_prefix, so whole UTF-8
// sequences only) with the ellipsis appended, built in `out`. When not even one
// character fits beside it the ellipsis is drawn alone, so the row still reads
// as "there is text here". The one truncation routine every screen uses.
inline std::string_view ellipsize(const IFont& f, const char* text, size_t len, int max_w, char (&out)[260]) {
  if (static_cast<int>(f.word_width(text, len, FontStyle::Regular)) <= max_w)
    return {text, len};
  static constexpr char kEll[] = "...";
  const size_t fit = fit_prefix(f, text, len, max_w - f.word_width(kEll, 3, FontStyle::Regular));
  const size_t cp = fit < sizeof(out) - 4 ? fit : sizeof(out) - 4;
  std::memcpy(out, text, cp);
  std::memcpy(out + cp, kEll, 3);
  return {out, cp + 3};
}

// Border outline around a rectangle, `t` pixels thick, growing inwards. The
// selection mark on the home carousel and the quick menu's chapter grid: a frame
// changes far fewer pixels than an inverted fill, so the move refreshes cleaner.
inline void draw_outline(DrawBuffer& buf, int x, int y, int w, int h, int t = 1) {
  if (w <= 0 || h <= 0) return;
  if (t > w) t = w;
  if (t > h) t = h;
  buf.fill_rect(x,         y,         w, t, false);
  buf.fill_rect(x,         y + h - t, w, t, false);
  buf.fill_rect(x,         y,         t, h, false);
  buf.fill_rect(x + w - t, y,         t, h, false);
}

// Base class for screens that show a list of selectable items: the item
// storage, up/down navigation with wrapping and hold-repeat, scrolling, and the
// battery header. Its own draw pass is the book list's two-line rows; the home
// carousel and the quick menu inherit the machinery and draw everything
// themselves (draw_all_).
//
// Subclasses implement:
//   on_start()      — populate items via add_item()
//   on_select(index) — handle item selection
//   on_back()       — handle back button (default: pop)
class ListMenuScreen : public IScreen {
 public:
  void start(DrawBuffer& buf, IRuntime& runtime) override;
  void stop() override {}
  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

 protected:
  // Title lines a row reserves. With 2 the title word-wraps onto a second line
  // and the row keeps that height even for a one-line title, so every book row
  // is the same size and its cover gets the room. Set by MainMenu.
  int detail_title_lines_ = 1;

  void add_item(const std::string& label, int indent = 0) {
    owned_strings_.push_back(label);
    labels_.push_back(std::string_view(owned_strings_.back()));
    indents_.push_back(indent);
  }
  // Zero-copy overload: stores a view into the caller-owned string.
  // The caller must ensure the referenced string outlives this screen.
  void add_item_view(std::string_view label, int indent = 0) {
    labels_.push_back(label);
    indents_.push_back(indent);
  }
  void clear_items() {
    labels_.clear();
    owned_strings_.clear();
    indents_.clear();
    selected_ = 0;
    scroll_offset_ = 0;
  }
  // Free all item storage without touching selected_/scroll_offset_.
  // Call from stop() to release RAM while preserving the cursor position.
  void free_items_storage() {
    { std::vector<std::string_view> tmp; labels_.swap(tmp); }
    { std::deque<std::string> tmp; owned_strings_.swap(tmp); }
    { std::vector<int> tmp; indents_.swap(tmp); }
  }
  int selected() const { return selected_; }
  int scroll_offset() const { return scroll_offset_; }
  void set_selected(int index) {
    selected_ = index;
    on_start_set_selection_ = true;
  }
  // MainMenu overrides this to mark the hairline dividers it derives from its
  // own entry list.
  virtual bool is_separator(int) const {
    return false;
  }
  // Returns true if the cursor may land on this item. Default: not a separator.
  virtual bool is_item_focusable(int index) const { return !is_separator(index); }
  // A row that never has a subtitle (MainMenu's Sync action) and is therefore
  // one line tall instead of two. Two-line rows are the default.
  virtual bool is_single_line_row(int) const { return false; }

  // Separator geometry, shared by every list in the tree — the book list and
  // QuickmenuScreen's own draw pass — so a hairline sits in the same gap
  // wherever it appears. Do not re-declare a local kSepH.
  static constexpr int kSeparatorH = 14;
  static void draw_separator_(DrawBuffer& buf, int W, int y) {
    buf.fill_rect(0, y + kSeparatorH / 2, W, 1, false);
  }
  virtual int count() const {
    return static_cast<int>(labels_.size());
  }
  // Nesting depth stored with the item (TOC entries use it); 0 when unset.
  int get_item_indent(int index) const {
    return (index >= 0 && index < static_cast<int>(indents_.size())) ? indents_[index] : 0;
  }

  // Label for item at index. Default reads from labels_[]; override to provide
  // labels dynamically without populating the labels_ vector.
  virtual std::string_view get_item_label(int index) const {
    if (index >= 0 && index < static_cast<int>(labels_.size()))
      return labels_[index];
    return {};
  }

  // Second line below the item label (the author). Default: none.
  virtual std::string_view get_item_subtitle(int index) const { return {}; }

  // Right-aligned text on the subtitle line (the reading percentage).
  virtual std::string_view get_item_right(int index) const { return {}; }

  // A 1 bpp image at the right-hand end of a row (MainMenu's covers).
  // MSB first, bit clear = ink, rows (w + 7) / 8 bytes apart.
  struct Thumb {
    const uint8_t* bits = nullptr;
    int w = 0, h = 0;
  };
  // Called once per draw with the height every two-line row offers the image.
  // Returns the width of the column to reserve, 0 for none — which is also what
  // a screen returns when it cannot hold thumbnails right now, so the rows fall
  // back to the full width rather than showing a column of empty frames.
  virtual int prepare_thumbs(int box_h) const { return 0; }
  // The image for row `index`, already fitted to the box. False draws an empty
  // frame in its place.
  virtual bool get_item_thumb(int index, Thumb& out) const { return false; }
  // After the last get_item_thumb of a draw: release anything held for it.
  virtual void end_thumbs() const {}

  // Called during start(). Call add_item() to populate the list.
  virtual void on_start() = 0;

  // Called when user presses select on an item.
  virtual void on_select(int index) = 0;

  // Called when user presses back.
  virtual void on_back();

  BitmapFont ui_font_;       // the 32 px header face: list rows and titles
  BitmapFont section_font_;  // one step below ui_font_: authors, percentages, the battery

  // The battery percentage, top right. Every screen calls this — the position is
  // fixed here rather than per-screen so the header does not shift by a pixel
  // when moving between the home carousel and a list.
  static constexpr int kBatteryPadX = 16;
  static constexpr int kBatteryPadY = 10;
  void draw_battery_(DrawBuffer& buf, int W, std::optional<uint8_t> battery_pct) const;

  // Height the battery row occupies, from the top of the panel.
  int battery_row_h_() const {
    return kBatteryPadY + section_font_.y_advance();
  }

  void request_redraw() {
    force_redraw_ = true;
  }

  virtual void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const;
  void ensure_visible_();
  void set_buf_rotation_(Rotation r) { if (buf_) buf_->set_rotation(r); }

  // Scroll offset that puts the selection mid-list, clamped so neither end of
  // the list leaves the screen part-empty. At the end, `shift` (px) lifts the
  // list so the last row ends on the panel edge; the top row slides under the
  // header. Override in subclasses whose draw_all_() lays rows out differently.
  virtual int centred_offset_(int H, int& shift) const;
  int scroll_shift() const { return scroll_shift_; }

 private:
  std::vector<std::string_view> labels_;
  std::deque<std::string> owned_strings_;  // backing storage for copied labels
  std::vector<int> indents_;

  int selected_ = 0;
  int undo_selected_ = -1;  // cursor before the last press move; Button::Undo returns to it
  int scroll_offset_ = 0;
  int scroll_shift_ = 0;
  HoldRepeat hold_up_;
  HoldRepeat hold_down_;
  int hold_reps_up_ = 0;    // repeats fired so far this hold; drives the step size
  int hold_reps_down_ = 0;

  bool on_start_set_selection_ = false;
  bool force_redraw_ = false;

  DrawBuffer* buf_ = nullptr;

  // Header height: the battery row and its gap. draw_all_ and the scroll maths
  // both use this, so they cannot disagree about where the list starts.
  int header_h_() const;

  // Height of one item slot. Pass a row index for a per-row height (single-line
  // rows are shorter); -1 gives the two-line default.
  int wintergreen_slot_h_(int index = -1) const;

  // how many items are visible starting at scroll_off given available_h pixels.
  // Accounts for separator items being shorter than regular slots.
  int wintergreen_visible_from_(int scroll_off, int available_h) const;

  // Draws the scrollable item list and scrollbar below header_h.
  void draw_list_(DrawBuffer& buf, int W, int H, int header_h) const;
};

}  // namespace wintergreen
