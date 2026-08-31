#pragma once

#include <cstdint>
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

// Base class for screens that show a titled list of selectable items.
// Handles drawing (header font for title, UI font for items with selection bar),
// up/down navigation with wrapping, scrolling for long lists, and font
// initialization from embedded data.
//
// Subclasses implement:
//   on_start()      — set title, populate items via add_item()
//   on_select(index) — handle item selection; return true to stay, false to exit
//   on_back()       — handle back button; return true to stay, false to exit (default)
class ListMenuScreen : public IScreen {
 public:
  void start(DrawBuffer& buf, IRuntime& runtime) override;
  void stop() override {}
  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

  int selected_index() const {
    return selected_;
  }
  void set_initial_selection(int index) {
    initial_selection_ = index;
  }

 protected:
  const char* title_ = nullptr;
  const char* title2_ = nullptr;

  std::string subtitle_;
  std::string subtitle2_;

  // Two-line rows (title + subtitle, full-width divider, right-hand column) instead of
  // the default centred single-line rows. Set by book-list style screens.
  bool detail_list_ = false;

  // Plain chrome: centred title header and the battery/nav-glyph footer, instead of the
  // standard "wintergreen" status bar and labelled nav boxes. Used by overlay-style
  // screens (chapter select, links) that appear on top of the reader.
  bool plain_list_ = false;


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
  void set_item_label(int index, const std::string& label) {
    if (index >= 0 && index < static_cast<int>(labels_.size())) {
      owned_strings_.push_back(label);
      labels_[index] = std::string_view(owned_strings_.back());
    }
  }
  void clear_items() {
    labels_.clear();
    owned_strings_.clear();
    indents_.clear();
    selected_ = 0;
    scroll_offset_ = 0;
  }
  // Free all item storage without touching selected_/scroll_offset_.
  // Call from stop() to release RAM while preserving the cursor position
  // for the fallback path in ListMenuScreen::start().
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
  // No screen inserts separator items any more; MainMenu overrides this to mark
  // the hairline divider it derives from its own entry list.
  virtual bool is_separator(int) const {
    return false;
  }
  // Returns true if the cursor may land on this item. Default: not a separator.
  // Override to additionally exclude theme-irrelevant items.
  virtual bool is_item_focusable(int index) const { return !is_separator(index); }
  // A row that never has a subtitle (MainMenu's Sync action) and is therefore
  // one line tall instead of two. Two-line rows are the default.
  virtual bool is_single_line_row(int) const { return false; }

  // Separator geometry, shared by every list in the tree — the book list, the
  // book-details list and QuickmenuScreen's own draw pass — so a hairline
  // sits in the same gap wherever it appears. Do not re-declare a local kSepH.
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

  // secondary line below the item label (e.g. author, read time, setting value).
  // Default returns empty (no subtitle). Override per screen.
  virtual std::string_view get_item_subtitle(int index) const { return {}; }

  // detail_list_ rows: right-aligned text on the title line (e.g. read time, "–").
  // Default returns empty (no right column). Override per screen.
  virtual std::string_view get_item_right(int index) const { return {}; }

  // left side of the top bar header (e.g. "X books", "Settings").
  // Default returns title_ if set. Override per screen.
  virtual std::string wintergreen_header_left() const { return title_ ? title_ : ""; }

  // section title drawn in header_font_ below the status bar.
  // Empty = no section title row. Override in screens that need a page heading.
  virtual std::string wintergreen_section_title() const { return {}; }

  // Called during start(). Set title_ and call add_item() to populate the list.
  virtual void on_start() = 0;

  // Called when user presses select on an item.
  virtual void on_select(int index) = 0;

  // Called when user presses back.
  virtual void on_back();

 protected:
  BitmapFont ui_font_;
  BitmapFont header_font_;
  BitmapFont subtitle_font_;   // always small; used for item subtitles and tight labels
  BitmapFont section_font_;    // one step below ui_font_; use for APPEARANCE/NAVIGATE etc.

  // The battery percentage, top right. Every screen calls this — the position is
  // fixed here rather than per-screen so the header does not shift by a pixel
  // when moving between the home carousel and a list.
  static constexpr int kBatteryPadX = 16;
  static constexpr int kBatteryPadY = 10;
  void draw_battery_(DrawBuffer& buf, int W, std::optional<uint8_t> battery_pct) const;

  // Height the battery row occupies, from the top of the panel.
  int battery_row_h_() const {
    const BitmapFont& bf = section_font_.valid() ? section_font_ : ui_font_;
    return kBatteryPadY + (bf.valid() ? bf.y_advance() : 0);
  }

  void request_redraw() {
    force_redraw_ = true;
  }

  // Re-run start() to rebuild items with updated settings (e.g. after font change).
  void restart() {
    if (buf_ && runtime_)
      start(*buf_, *runtime_);
  }

  virtual void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const;
  virtual void ensure_visible_();
  void set_scroll_offset_(int v) { scroll_offset_ = v; }
  int current_height_() const { return buf_ ? buf_->height() : 0; }
  Rotation current_rotation_() const { return buf_ ? buf_->rotation() : Rotation::Deg90; }
  void set_buf_rotation_(Rotation r) { if (buf_) buf_->set_rotation(r); }
  void center_on_selected_();

  // Returns the number of visual indices visible from scroll_off given screen height H.
  // Override in subclasses whose draw_all_() uses a custom header or item height.
  virtual int get_visible_count_(int H, int scroll_off) const;

 private:
  std::vector<std::string_view> labels_;
  std::deque<std::string> owned_strings_;  // backing storage for copied labels
  std::vector<int> indents_;

  int selected_ = 0;
  int scroll_offset_ = 0;
  int initial_selection_ = -1;
  HoldRepeat hold_up_;
  HoldRepeat hold_down_;
  int hold_reps_up_ = 0;    // repeats fired so far this hold; drives the step size
  int hold_reps_down_ = 0;

  bool on_start_set_selection_ = false;
  bool force_redraw_ = false;

  DrawBuffer* buf_ = nullptr;
  IRuntime* runtime_ = nullptr;

  // Computes the header height (title + subtitles) without drawing anything.
  // Used by ensure_visible_() and center_on_selected_() before a draw pass.
  int compute_header_h_() const;

  // 1. Draws title, title2, and subtitles. Returns the header height (pixels from y=0 to
  //    where list items may start).
  int draw_header_(DrawBuffer& buf, int W, int H, std::optional<uint8_t> battery_pct = {}) const;

  // height of one list item slot (title + subtitle + padding + divider).
  // Height of one item slot. Pass a row index for a per-row height (single-line
  // rows are shorter); -1 gives the two-line default.
  int wintergreen_slot_h_(int index = -1) const;

  // how many items are visible starting at scroll_off given available_h pixels.
  // Accounts for separator items being shorter than regular slots.
  int wintergreen_visible_from_(int scroll_off, int available_h) const;

  // 2. Draws the battery bar and button-hint glyphs at the bottom of the screen.
  //    Returns the height reserved at the bottom (list items must stay above this).
  int draw_bottom_(DrawBuffer& buf, int W, int H, std::optional<uint8_t> battery_pct) const;

  // 3. Draws the scrollable item list, scrollbar, and area-boundary indicator lines,
  //    given the already-known header and bottom heights.
  void draw_list_(DrawBuffer& buf, int W, int H, int header_h, int bottom_h) const;
};

}  // namespace wintergreen
