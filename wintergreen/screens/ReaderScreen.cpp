#include "ReaderScreen.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "../Application.h"
#include "../content/BookIndex.h"
#include "../content/CoverPaths.h"

namespace wintergreen {

std::string ReaderScreen::display_title_() const {
  return book_title_fallback(wgb_.metadata().title, path_);
}

// A stored image's pixels, read through WgbReader's already-open handle while
// the panel refreshes (a strip at a time, so no image buffer exists). `offset`
// is absolute in the book; `item` is only identity.
bool ReaderScreen::read(uint32_t, uint32_t offset, uint8_t* dst, size_t n) {
  FILE* f = wgb_.file();
  return f && std::fseek(f, static_cast<long>(offset), SEEK_SET) == 0 && std::fread(dst, 1, n, f) == n;
}

// Record an image, or the slice of it on this page: [src_y, src_y + clip_h)
// of a w x full_h bitmap, drawn at (x, y). The WGB holds both copies the panel
// needs (WgbFormat.h): rows for landscape, columns for portrait, two planes
// each, so a strip of either is one contiguous read and a byte copy.
bool ReaderScreen::draw_image_(DrawBuffer& buf, const WgbImage& img) {
  WgbImageRef ref;
  // The image table is on the card: wait the waveform out before reading it.
  buf.wait_panel_idle();
  if (!wgb_.image_ref(img.key, ref) || ref.data_offset == 0 || ref.width == 0 || ref.height == 0 ||
      ref.data_size != wgb_image_bytes(ref.width, ref.height))
    return false;
  const int keep_lo = img.src_y;
  const int keep_hi = img.clip_h > 0 ? img.src_y + img.clip_h : ref.height;
  const bool portrait = buf.rotation() == Rotation::Deg90;
  const uint32_t base = ref.data_offset + (portrait ? wgb_image_rows_bytes(ref.width, ref.height) : 0);
  buf.blit(this, img.key, image_gen_, base, portrait ? PixelLayout::Cols : PixelLayout::Rows, img.x,
           img.y - img.src_y, ref.width, ref.height, keep_lo, keep_hi);
  return true;
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

void ReaderScreen::sync_layout_(const DrawBuffer& buf) {
  const uint8_t want = wanted_layout_(buf);
  if (want == layout_)
    return;
  WgbPage p;
  page_ = wgb_.page(layout_, page_, p) ? wgb_.find_page(want, p.para_offset, p.text_offset) : 0;
  layout_ = want;
}

void ReaderScreen::start(DrawBuffer& buf, IRuntime& runtime) {
  sample_battery_(runtime);
  buf_ = &buf;
  snapshot_ok_ = false;  // a fresh book must never resume into a previous one's pixels
  pos_written_valid_ = false;
  if (app_)
    buf.set_rotation(rotation_from_setting(app_->rotate_reader()));
  pos_path_.clear();

  // No reader font, no book: update() pops straight back without drawing.
  open_ok_ = false;
  if (!ext_font_set_ || !ext_font_set_->valid() || !wgb_.open(path_.c_str()))
    return;
  // Image identity for this opening: a book replaced under the same path by a
  // sync must never pass for the pixels already on the glass.
  image_gen_ = book_path_key(path_) ^ (++opens_ * 0x9E3779B9u);
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
  // Step back one page on a fresh open (config::kResumeOnePageBack). The .pos
  // dedup key keeps what the file holds, so only reading moves it.
  if (config::kResumeOnePageBack && page_ > 0)
    --page_;
  render_page_(buf);
  stage_pending_ = true;
}

// Park the page on the glass (DrawBuffer::save_snapshot) so resume() can put it
// back without reading or drawing anything.
// Recording the font size and rotation alongside it is what makes the fast path
// safe: the quick menu is the only screen that pushes over the reader, and those
// two settings are the only things it can change that alter the pixels.
void ReaderScreen::pause() {
  if (!buf_ || !open_ok_)
    return;
  buf_->save_snapshot();
  snapshot_font_size_ = reader_settings_.font_size_idx;
  snapshot_rotation_ = app_ ? app_->rotate_reader() : 0;
  snapshot_ok_ = !pixels_stale_;
  pixels_stale_ = false;
}

void ReaderScreen::resume(DrawBuffer& buf, IRuntime& runtime) {
  sample_battery_(runtime);
  buf_ = &buf;
  if (app_)
    buf.set_rotation(rotation_from_setting(app_->rotate_reader()));
  if (!open_ok_)
    return;

  // Fast path: nothing that affects the pixels changed while the quick menu was
  // up, so put the snapshot back instead of laying the page out and drawing
  // every glyph again. The caller (Application's pop handler) does the refresh.
  const bool jumping = app_ && app_->quickmenu()->has_pending();
  if (!jumping && snapshot_ok_ && reader_settings_.font_size_idx == snapshot_font_size_ &&
      (app_ ? app_->rotate_reader() : 0) == snapshot_rotation_ && buf.restore_snapshot()) {
    snapshot_ok_ = false;
    stage_pending_ = true;
    return;
  }
  snapshot_ok_ = false;
  buf.drop_snapshot();

  // A chapter jump chosen from the quick menu: its page, in the layout the
  // menu was opened in (render_page_ maps it if the layout changed too).
  if (jumping) {
    page_ = app_->quickmenu()->pending_page();
    app_->quickmenu()->clear_pending();
    if (page_ >= wgb_.page_count(layout_))
      page_ = 0;
  }

  render_page_(buf);
  stage_pending_ = true;
}

void ReaderScreen::stop() {
  if (open_ok_) {
    // .pos is an SD write and the panel may still be mid-refresh — see the note in
    // draw_image_. This is the only reader path that writes the card.
    if (buf_)
      buf_->wait_panel_idle();
    save_position_();
    // Cache the percentage for the book list, which has no cheap way to derive
    // it (that would mean opening every WGB and .pos on the card).
    if (app_)
      app_->record_book_progress(path_, progress_pct());
  }
  wgb_.close();
  stage_pending_ = false;
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
          if (last_step_ != 0)
            pixels_stale_ = true;
          last_step_ = 0;
          break;
        case Button::Button0:
          app_->pop_screen();
          return;
        case Button::Button1: {
          std::vector<uint32_t> chapters;
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
    pixels_stale_ = false;
    // One ADC read per page turn, not per frame.
    sample_battery_(runtime);
    // A forward turn finds its display list already staged in the list it
    // records into (no card read on the press path).
    render_page_(buf);
    buf.refresh();
    stage_pending_ = true;
  } else if (stage_pending_ && !buf.display().is_busy()) {
    // First frame after the waveform (the loop runs frames while the panel is
    // busy): the card is free, so read the next page's list now, off the
    // press path. The reader is overwhelmingly likely to go forward next —
    // after a backward turn too, where "forward" is the page just left.
    stage_pending_ = false;
    stage_next_page_(buf);
  }
}

// Display-list tag for page `index` of `layout`: what DrawBuffer's tail holds.
static uint32_t page_tag_(uint8_t layout, uint32_t index) {
  return (static_cast<uint32_t>(layout + 1) << 28) | (index & 0x0FFFFFFFu);
}

// Read page `index` of `layout`'s display list into the tail of the list being
// recorded — unless it is already there (staged). The card shares SPI2 with the
// panel, so the read waits any waveform out first (docs/display.md).
bool ReaderScreen::load_page_(DrawBuffer& buf, uint8_t layout, uint32_t index) {
  const uint32_t tag = page_tag_(layout, index);
  if (buf.tail_tag() == tag)
    return true;
  WgbPage rec;
  if (!wgb_.page(layout, index, rec) || rec.dl_size > kWgbMaxPageBytes)
    return false;
  uint8_t* dst = buf.claim_tail(rec.dl_size, tag);
  if (!dst)
    return false;
  buf.wait_panel_idle();
  if (!wgb_.read_page(rec, dst, rec.dl_size)) {
    buf.claim_tail(0, 0);
    return false;
  }
  return true;
}

// Read page_ + 1's display list into the free list's tail, for the next turn.
void ReaderScreen::stage_next_page_(DrawBuffer& buf) {
  if (open_ok_ && page_ + 1 < wgb_.page_count(layout_))
    load_page_(buf, layout_, page_ + 1);
}

void ReaderScreen::render_page_(DrawBuffer& buf) {
  sync_layout_(buf);
  // A record that does not read draws as a blank page (no error text, by rule).
  const bool ok = load_page_(buf, layout_, page_);
  draw_page_(buf, ok);
}

void ReaderScreen::draw_page_(DrawBuffer& buf, bool have_list) {
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(Level::White);

  // Items are stored in draw order (lines, rules, images); a malformed list
  // just stops drawing where it goes wrong.
  WgbPageView v(buf.tail_data(), have_list ? buf.tail_size() : 0);
  uint8_t tag;
  while (v.peek(tag)) {
    if (tag == kWgbItemLine) {
      WgbLine line;
      if (!v.read_line(line))
        break;
      buf.draw_baked_line(line, v.end(), *ext_font_set_);
    } else if (tag == kWgbItemRule) {
      WgbRule r;
      if (!v.read_rule(r))
        break;
      buf.fill_rect(r.x, r.y + r.h / 2 - 1, r.w, 2, Level::Gray);
    } else if (tag == kWgbItemImage) {
      WgbImage img;
      if (!v.read_image(img))
        break;
      // An image that failed to rasterise at conversion time has no usable
      // entry: its space stays white rather than a black slab per figure.
      draw_image_(buf, img);
    } else {
      break;
    }
  }

  // Charge reminder. Drawn last so an image reaching the bottom margin cannot
  // paint over it, and in logical coordinates so it sits along the bottom edge
  // in both orientations.
  if (low_battery_)
    buf.fill_rect(0, H - kLowBatteryBarH, W, kLowBatteryBarH, Level::Gray);
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
  FILE* f = std::fopen(pos_path_.c_str(), "w");
  if (!f)
    return;
  // Where the page starts, independent of font size and rotation: the
  // paragraph record's offset in the WGB and the text offset within it.
  std::fprintf(f, "%u %u\n", static_cast<unsigned>(now.para_offset), static_cast<unsigned>(now.text_offset));
  if (std::fclose(f) == 0) {
    pos_written_ = now;
    pos_written_valid_ = true;
  }
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
