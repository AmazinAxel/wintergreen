#include "ReaderScreen.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "../Application.h"
#include "../content/BookIndex.h"
#include "../content/CoverPaths.h"

namespace wintergreen {

std::string ReaderScreen::display_title_() const {
  return book_title_fallback(wgb_.metadata().title, path_);
}

// One strip's part of a stored image, for GrayPage (called inside the update,
// panel idle and queued windows drained, so the card is free).
//
// The WGB holds images as raw 1-bit bitmaps already scaled and dithered to the
// size they are drawn at, rows plus a column copy, so this is a seek and one
// read — no decoder, no ditherer, no scaling, and no cache file. It reads
// through WgbReader's already-open handle, so a page with three figures does
// not open the book three times.
bool ReaderScreen::read_image(const WgbImage& img, Rotation rot, int lo, int hi, uint8_t* dst, size_t cap) {
  FILE* f = wgb_.file();
  if (!f || lo < 0 || hi <= lo)
    return false;
  // Every strip an image crosses asks again: read its table entry once.
  if (img.key != ref_key_) {
    if (!wgb_.image_ref(img.key, ref_))
      return false;
    ref_key_ = img.key;
  }
  const WgbImageRef& ref = ref_;
  const uint32_t width = img.w, height = img.full_h;
  if (ref.data_offset == 0 || width == 0 || height == 0 ||
      wgb_image_rows_bytes(width, height) + wgb_image_cols_bytes(width, height) > ref.data_size)
    return false;
  // Landscape: rows [lo, hi) of the row copy; portrait: columns of the column
  // copy (WgbFormat.h).
  const uint32_t unit = rot == Rotation::Deg90 ? (height + 7) / 8 : (width + 7) / 8;
  const uint32_t base = ref.data_offset + (rot == Rotation::Deg90 ? wgb_image_rows_bytes(width, height) : 0);
  const uint32_t n = static_cast<uint32_t>(hi - lo) * unit;
  return n <= cap && std::fseek(f, static_cast<long>(base + static_cast<uint32_t>(lo) * unit), SEEK_SET) == 0 &&
         std::fread(dst, 1, n, f) == n;
}

// Latching: once the bar is up it stays up until the next boot (deep-sleep
// wake is one). A cell sags a few hundred millivolts under an e-ink refresh, so
// an unlatched threshold would flicker the bar on and off between page turns
// right around 15%, which is exactly where it matters.
void ReaderScreen::sample_battery_(IRuntime& runtime) {
  if (low_battery_)
    return;
  const std::optional<uint8_t> pct = runtime.battery_percentage();
  // No reading at all (no cell, uncalibrated ADC) means no bar — the same rule
  // the low-battery cutoff follows.
  low_battery_ = pct.has_value() && *pct <= kLowBatteryBarPct;
}

uint8_t ReaderScreen::wanted_layout_(const DrawBuffer& buf) const {
  const uint8_t size = reader_settings_.font_size_idx < kWgbFontSizes ? reader_settings_.font_size_idx : 0;
  return wgb_layout_index(size, buf.rotation() == Rotation::Deg0);
}

void ReaderScreen::sync_layout_(DrawBuffer& buf) {
  const uint8_t want = wanted_layout_(buf);
  if (want == layout_)
    return;
  buf.wait_panel_idle();  // record + binary search: card reads
  WgbPage p;
  page_ = wgb_.page(layout_, page_, p) ? wgb_.find_page(want, p.para_offset, p.text_offset) : 0;
  layout_ = want;
}

void ReaderScreen::start(DrawBuffer& buf, IRuntime& runtime) {
  sample_battery_(runtime);
  buf_ = &buf;
  pos_written_valid_ = false;
  ref_key_ = UINT32_MAX;  // a new book: its own image table
  ahead_[0].at.valid = ahead_[1].at.valid = false;  // and its own pages
  buf.set_rotation(rotation_from_setting(app_->rotate_reader()));
  pos_path_.clear();

  // No reader font, no book: update() pops straight back without drawing.
  open_ok_ = false;
  buf.wait_panel_idle();  // opening, .pos and the first page are card reads
  app_->write_pending_pos();  // a just-closed book's position, before reading ours
  if (!page_buf_ || !ext_font_set_ || !ext_font_set_->valid() || !wgb_.open(path_.c_str()))
    return;
  layout_ = wanted_layout_(buf);
  if (wgb_.page_count(layout_) == 0) {
    wgb_.close();
    return;
  }

  pos_path_ = book_pos_path(path_.c_str());
  open_ok_ = true;

  page_ = 0;
  PosKey saved;
  if (load_position_(saved))
    page_ = wgb_.find_page(layout_, saved.para_offset, saved.text_offset);
  render_page_(buf);
}

void ReaderScreen::pause() {}

// The quick menu was on the glass (an OTP frame), so the page goes up again as
// a gray update from that frame; the caller (Application's pop handler) does the
// refresh. page_buf_ still holds the page unless the layout changed.
void ReaderScreen::resume(DrawBuffer& buf, IRuntime& runtime) {
  sample_battery_(runtime);
  buf_ = &buf;
  buf.set_rotation(rotation_from_setting(app_->rotate_reader()));
  if (!open_ok_)
    return;

  // A chapter jump chosen from the quick menu: its page, in the layout the
  // menu was opened in (render_page_ maps it if the layout changed too).
  const bool jumping = app_->quickmenu()->has_pending();
  if (jumping) {
    page_ = app_->quickmenu()->pending_page();
    app_->quickmenu()->clear_pending();
    if (page_ >= wgb_.page_count(layout_))
      page_ = 0;
  }

  render_page_(buf);
}

void ReaderScreen::stop() {
  if (open_ok_) {
    // .pos is an SD write and the panel may still be mid-refresh, and the card
    // shares SPI2 with it. This is the only reader path that writes the card.
    if (buf_)
      buf_->wait_panel_idle();
    save_position_();
    // Cache the percentage for the book list, which has no cheap way to derive
    // it (that would mean opening every WGB and .pos on the card).
    app_->record_book_progress(path_, progress_pct());
  }
  loaded_.valid = ahead_[0].at.valid = ahead_[1].at.valid = false;
  wgb_.close();
  pos_path_.clear();
  pos_path_.shrink_to_fit();
  open_ok_ = false;
  // Restore the global menu rotation before handing the buffer back.
  if (buf_)
    buf_->set_rotation(Rotation::Deg90);
  buf_ = nullptr;
}

void ReaderScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  if (!open_ok_) {
    // The book did not open; nothing was drawn, so go straight back.
    app_->pop_screen();
    return;
  }

  // Process press events in the order they arrived.
  int page_delta = 0;

  // Side rocker, which is one physical control whose ends swap places when the
  // device is turned. Rotating to landscape flips it; portrait uses the
  // configured mapping as-is:
  //
  //   portrait   Up = next,   Down = prev
  //   landscape  Down = next, Up   = prev
  //
  const bool landscape_hold = buf.rotation() == Rotation::Deg0;
  // Front row is fixed to the panel and does not rotate with the hold.
  constexpr Button logical_next_front = Button::Button3;
  constexpr Button logical_prev_front = Button::Button2;
  const Button logical_next_side = landscape_hold ? Button::Down : Button::Up;
  const Button logical_prev_side = landscape_hold ? Button::Up : Button::Down;

  Button btn;
  while (buttons.next_press(btn)) {
    if (btn == logical_next_front || btn == logical_next_side) {
      ++page_delta;
    } else if (btn == logical_prev_front || btn == logical_prev_side) {
      --page_delta;
    } else {
      switch (btn) {
        case Button::Undo:
          // A clicker hold: the press already turned the page, the hold wants
          // Back/Confirm from where the reader was. Revert the position only;
          // what follows paints over the glass. A turn still pending in this
          // batch (the frame ran long) is simply cancelled instead.
          if (page_delta != 0) {
            page_delta += page_delta > 0 ? -1 : 1;
            break;
          }
          if (last_step_ > 0)
            prev_page_();
          else if (last_step_ < 0)
            next_page_();
          last_step_ = 0;
          break;
        case Button::Button0:
          app_->pop_screen();
          return;
        case Button::Button1: {
          std::vector<uint32_t> chapters;
          buf.wait_panel_idle();  // the chapter table is on the card
          wgb_.chapter_pages(layout_, chapters);
          const int chapter_pct = chapter_progress_pct_(chapters);
          app_->quickmenu()->set_settings(&reader_settings_);
          // Power tap → the top setting; front select → the chapter being read.
          app_->quickmenu()->set_open_on_chapter(!app_->confirm_from_power());
          app_->quickmenu()->populate(std::move(chapters), page_, display_title_(), progress_pct(), chapter_pct);
          app_->push_screen(ScreenId::Quickmenu);
          return;
        }
        default:
          break;
      }
    }
  }

  // Hold-down: keep paging while a nav button is held. Gated on elapsed hold time,
  // not on frames — the press itself is already counted above, and a tap must never
  // add a repeat no matter how fast the frame loop happens to be running.
  const uint32_t dt_ms = runtime.frame_time_ms();
  page_delta += hold_next_.tick(buttons.is_down(logical_next_front) || buttons.is_down(logical_next_side), dt_ms);
  page_delta -= hold_prev_.tick(buttons.is_down(logical_prev_front) || buttons.is_down(logical_prev_side), dt_ms);

  bool changed = false;
  if (page_delta > 0) {
    for (int i = 0; i < page_delta; ++i)
      changed = next_page_() || changed;
  } else if (page_delta < 0) {
    for (int i = 0; i > page_delta; --i)
      changed = prev_page_() || changed;
  }

  if (page_delta != 0)
    last_step_ = changed ? static_cast<int8_t>(page_delta > 0 ? 1 : -1) : 0;

  if (changed) {
    // One ADC read per page turn, not per frame.
    sample_battery_(runtime);
    render_page_(buf);
    buf.refresh();
  } else if (open_ok_ && !buf.display().is_busy()) {
    // First frame after the waveform (the loop runs frame cadence while the
    // panel is busy): read the neighbouring pages' lists now, so a turn reads
    // nothing from the card on the press path.
    prefetch_();
    app_->load_deferred_index();  // resumed into a book: off the Back path
  }
}

// Read page_'s neighbours (page_ + 1 first) into ahead_. Only with the panel
// idle: the card shares SPI2 with it. A slot holding neither neighbour is the
// one refilled, so a turn either way reads nothing on the press path.
void ReaderScreen::prefetch_() {
  const uint32_t count = wgb_.page_count(layout_);
  const bool has_next = page_ + 1 < count, has_prev = page_ > 0;
  auto wanted = [&](const Loaded& at) {
    return (has_next && at.is(layout_, page_ + 1)) || (has_prev && at.is(layout_, page_ - 1));
  };
  for (const uint32_t want : {page_ + 1, page_ - 1}) {
    if ((want == page_ + 1 && !has_next) || (want == page_ - 1 && !has_prev))
      continue;
    if (ahead_[0].at.is(layout_, want) || ahead_[1].at.is(layout_, want))
      continue;
    Ahead& slot = !wanted(ahead_[0].at) ? ahead_[0] : ahead_[1];
    if (!slot.buf || wanted(slot.at))
      continue;
    slot.at.valid = false;
    WgbPage rec;
    if (!wgb_.page(layout_, want, rec) || !wgb_.read_page(rec, slot.buf, kWgbMaxPageBytes))
      continue;
    slot.len = rec.dl_size;
    slot.at = Loaded{true, layout_, want};
  }
}

// The card shares SPI2 with the panel, so every read here waits the waveform
// out first (docs/display.md). A page already in page_buf_ costs nothing; one
// read ahead into ahead_ is a pointer swap (the slot keeps the page left).
bool ReaderScreen::load_page_(DrawBuffer& buf, uint8_t layout, uint32_t index) {
  if (loaded_.is(layout, index))
    return true;
  for (Ahead& a : ahead_) {
    if (a.at.is(layout, index)) {
      std::swap(page_buf_, a.buf);
      std::swap(page_len_, a.len);
      std::swap(loaded_, a.at);
      return true;
    }
  }
  loaded_.valid = false;
  WgbPage rec;
  if (!page_buf_)
    return false;
  buf.wait_panel_idle();  // the record read below is a card read too
  if (!wgb_.page(layout, index, rec) || !wgb_.read_page(rec, page_buf_, kWgbMaxPageBytes))
    return false;
  page_len_ = rec.dl_size;
  loaded_ = Loaded{true, layout, index};
  return true;
}

// Hand page_ to the gray path (GrayPage): the next refresh puts it up on the
// Turn LUT. The draw buffer is not touched, so it still holds the last 1-bit
// frame on the glass. An image that fails to read stays white (a black slab
// per figure is far worse than white space); the low-battery bar runs along the
// bottom edge in both orientations.
void ReaderScreen::render_page_(DrawBuffer& buf) {
  sync_layout_(buf);
  // A record that does not read draws as a blank page (no error text, by rule).
  if (!load_page_(buf, layout_, page_))
    page_len_ = 0;
  buf.gray().set(page_buf_, page_len_, ext_font_set_, buf.rotation(), low_battery_ ? kLowBatteryBarH : 0, this);
}

bool ReaderScreen::next_page_() {
  if (page_ + 1 >= wgb_.page_count(layout_))
    return false;
  ++page_;
  return true;
}

bool ReaderScreen::prev_page_() {
  if (page_ == 0)
    return false;
  --page_;
  return true;
}

int ReaderScreen::chapter_progress_pct_(const std::vector<uint32_t>& chapter_pages) const {
  uint32_t first = 0;
  uint32_t end = wgb_.page_count(layout_);
  for (uint32_t p : chapter_pages) {
    if (p <= page_)
      first = p;
    else if (p < end) {
      end = p;
      break;
    }
  }
  if (end <= first || page_ + 1 >= end)
    return 100;
  return static_cast<int>(static_cast<uint64_t>(page_ - first) * 100u / (end - first));
}

// ---------------------------------------------------------------------------
// Bookmark persistence
// ---------------------------------------------------------------------------

void ReaderScreen::save_position_() {
  if (pos_path_.empty())
    return;
  WgbPage p;
  if (!wgb_.page(layout_, page_, p))
    return;
  // Skip the write when the position has not moved since it was loaded or last
  // written — opening a book and backing straight out, or paging forward and
  // back to where you started, then costs the card nothing.
  const PosKey now{p.para_offset, p.text_offset};
  if (pos_written_valid_ && now == pos_written_)
    return;
  // Where the page starts, independent of font size and rotation: the
  // paragraph record's offset in the WGB and the text offset within it.
  // Written by Application::flush_persist once the next screen is up.
  char line[24];
  const int n = std::snprintf(line, sizeof(line), "%u %u\n", static_cast<unsigned>(now.para_offset),
                              static_cast<unsigned>(now.text_offset));
  if (n <= 0 || n >= static_cast<int>(sizeof(line)))
    return;
  app_->queue_book_pos(pos_path_, std::string(line, static_cast<size_t>(n)));
  pos_written_ = now;
  pos_written_valid_ = true;
}

bool ReaderScreen::load_position_(PosKey& out) {
  if (pos_path_.empty())
    return false;
  FILE* f = std::fopen(pos_path_.c_str(), "r");
  if (!f)
    return false;
  unsigned para = 0, to = 0;
  const int scanned = std::fscanf(f, "%u %u", &para, &to);
  std::fclose(f);
  if (scanned != 2 || to > 0xFFFF)
    return false;
  out = PosKey{static_cast<uint32_t>(para), static_cast<uint16_t>(to)};
  // Seed the write-dedup key: this is what the file already contains.
  pos_written_ = out;
  pos_written_valid_ = true;
  return true;
}

}  // namespace wintergreen
