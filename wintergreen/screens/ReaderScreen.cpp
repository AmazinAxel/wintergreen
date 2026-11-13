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

// Draw a stored image, or the slice of it that lands on this page.
//
// The WGB holds images as raw 1-bit bitmaps already scaled and dithered to the
// size they are drawn at, so this is a seek and a row-by-row blit — no decoder,
// no ditherer, no scaling, and no cache file. It replaced a JPEG/PNG decode that
// ran on first view and wrote its result back to the SD card so it would not
// have to run twice.
//
// It reads through WgbReader's already-open handle, so a page with three figures
// does not open the book three times.
//
// `offset`/`size` come from the image table; `src_y`/`clip_h` select the rows
// that belong to this page when an image spans a page break.
bool ReaderScreen::draw_image_(uint32_t offset, uint32_t size, DrawBuffer& buf, int dest_x, int dest_y, uint16_t width,
                               uint16_t height, uint16_t src_y, uint16_t clip_h) {
  if (offset == 0 || size == 0 || width == 0 || height == 0)
    return false;
  // Refreshes are fired without waiting, so the previous page's waveform may still
  // be running. The SD card shares SPI2 with the panel and concurrent traffic
  // corrupts an update in flight, so drain before touching the card. Text-only
  // pages never reach here and keep the full overlap.
  buf.wait_panel_idle();

  FILE* f = wgb_.file();
  if (!f)
    return false;

  const uint16_t row_bytes = static_cast<uint16_t>((width + 7) / 8);
  const uint32_t rows_bytes = wgb_image_rows_bytes(width, height);
  if (rows_bytes + wgb_image_bands_bytes(width, height) > size)
    return false;
  const uint16_t first = src_y;
  uint16_t last = height;
  if (clip_h > 0 && static_cast<uint32_t>(first) + clip_h < last)
    last = static_cast<uint16_t>(first + clip_h);
  if (first >= last)
    return false;

  if (buf.rotation() == Rotation::Deg90) {
    // Portrait: the band copy after the rows (WgbFormat.h). Bands first/8 ..
    // (last-1)/8 are contiguous, read a few at a time; rows of the edge bands
    // outside [first, last) are not drawn.
    if (width > 2 * DisplayFrame::kStride * 8)
      return false;
    const uint32_t b0 = first / 8u, b1 = (last - 1u) / 8u;
    if (std::fseek(f, static_cast<long>(offset + rows_bytes + b0 * width), SEEK_SET) != 0)
      return false;
    static constexpr uint32_t kBandBuf = 16 * DisplayFrame::kStride;
    uint8_t bands[kBandBuf];
    const uint32_t per = std::max<uint32_t>(1, kBandBuf / width);
    for (uint32_t b = b0; b <= b1;) {
      const uint32_t n = std::min(per, b1 - b + 1);
      const size_t got = std::fread(bands, width, n, f);
      for (uint32_t k = 0; k < got; ++k) {
        const int top = static_cast<int>(8 * (b + k));  // image row of the band's first row
        buf.blit_1bit_bands(dest_x, dest_y + top - first, bands + k * width, width, first - top, last - top);
      }
      if (got != n)
        break;
      b += n;
    }
    return true;
  }

  if (row_bytes > DisplayFrame::kStride)
    return false;  // wider than the panel: not something the converter produces
  if (std::fseek(f, static_cast<long>(offset) + static_cast<long>(first) * row_bytes, SEEK_SET) != 0)
    return false;
  // A band of rows on the stack: no heap, one fread per band, and enough rows
  // per blit for the portrait path to pack whole bytes.
  static constexpr uint16_t kBandRows = 16;
  uint8_t band[kBandRows * DisplayFrame::kStride];
  for (uint16_t r = first; r < last;) {
    const uint16_t n = static_cast<uint16_t>(std::min<int>(kBandRows, last - r));
    const size_t got = std::fread(band, row_bytes, n, f);
    if (got > 0)
      buf.blit_1bit(dest_x, dest_y + static_cast<int>(r - first), band, width, static_cast<int>(got), row_bytes);
    if (got != n)
      break;
    r = static_cast<uint16_t>(r + n);
  }
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
  predrawn_.valid = false;
}

void ReaderScreen::start(DrawBuffer& buf, IRuntime& runtime) {
  sample_battery_(runtime);
  buf_ = &buf;
  snapshot_ok_ = false;  // a fresh book must never resume into a previous one's pixels
  predrawn_.valid = false;
  pos_written_valid_ = false;
  if (app_)
    buf.set_rotation(rotation_from_setting(app_->rotate_reader()));
  pos_path_.clear();

  // No reader font, no book: update() pops straight back without drawing.
  open_ok_ = false;
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
  // Step back one page on a fresh open (config::kResumeOnePageBack). The .pos
  // dedup key keeps what the file holds, so only reading moves it.
  if (config::kResumeOnePageBack && page_ > 0)
    --page_;
  render_page_(buf);
  stage_next_page_(buf);
  prerender_pending_ = true;
}

// Snapshot the rendered page so resume() can put it back without re-rendering.
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
      (app_ ? app_->rotate_reader() : 0) == snapshot_rotation_ && buf.has_snapshot() &&
      buf.restore_snapshot()) {
    snapshot_ok_ = false;
    stage_next_page_(buf);
    prerender_pending_ = true;  // the snapshot claimed the spare the pre-drawn page was in
    return;
  }
  snapshot_ok_ = false;

  // A chapter jump chosen from the quick menu: its page, in the layout the
  // menu was opened in (render_page_ maps it if the layout changed too).
  if (jumping) {
    page_ = app_->quickmenu()->pending_page();
    app_->quickmenu()->clear_pending();
    if (page_ >= wgb_.page_count(layout_))
      page_ = 0;
  }

  render_page_(buf);
  stage_next_page_(buf);
  prerender_pending_ = true;
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
  loaded_.valid = false;
  wgb_.close();
  predrawn_.valid = false;
  predraw_after_idle_ = false;
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
          if (last_step_ != 0) {
            predrawn_.valid = false;
            pixels_stale_ = true;
          }
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
    // One ADC read per page turn, not per frame. A pre-drawn page carries the
    // sample taken when it was drawn, so the bar can lag one turn.
    sample_battery_(runtime);
    // A forward turn may already have the page drawn and waiting.
    if (page_delta <= 0 || !take_predrawn_(buf))
      render_page_(buf);
    stage_next_page_(buf);
    buf.refresh();
    // The panel is now busy for a few hundred ms and the reader is
    // overwhelmingly likely to go forward next — after a backward turn too,
    // where "forward" is the page just left. The CPU would only be waiting out
    // the waveform otherwise.
    prerender_next_page_(buf);
    prerender_pending_ = false;
  } else if (prerender_pending_) {
    prerender_pending_ = false;
    prerender_next_page_(buf);
  } else if (predraw_after_idle_ && !buf.display().is_busy()) {
    prerender_next_page_(buf);
  }
}

// The card shares SPI2 with the panel, so every read here waits the waveform
// out first (docs/display.md). A page already in page_buf_ costs nothing.
bool ReaderScreen::load_page_(DrawBuffer& buf, uint8_t layout, uint32_t index) {
  if (loaded_.valid && loaded_.layout == layout && loaded_.index == index)
    return true;
  loaded_.valid = false;
  WgbPage rec;
  if (!page_buf_ || !wgb_.page(layout, index, rec))
    return false;
  buf.wait_panel_idle();
  if (!wgb_.read_page(rec, page_buf_, kWgbMaxPageBytes))
    return false;
  page_len_ = rec.dl_size;
  loaded_ = Loaded{true, layout, index};
  return true;
}

// Read the next page's display list while the panel is idle, right before the
// refresh that puts page_ up, so prerender_next_page_ can draw it during the
// waveform without touching the card. One small read, a few ms.
void ReaderScreen::stage_next_page_(DrawBuffer& buf) {
  if (open_ok_ && page_ + 1 < wgb_.page_count(layout_))
    load_page_(buf, layout_, page_ + 1);
}

bool ReaderScreen::page_has_image_() const {
  WgbPageView v(page_buf_, page_len_);
  uint8_t tag;
  while (v.peek(tag)) {
    if (tag == kWgbItemImage)
      return true;
    v.skip();
  }
  return false;
}

// Draw the page after the current one into the offscreen buffer, while the
// panel is still running this page's waveform.
//
// The next forward turn then costs a memcpy and an SPI write: no glyph
// blitting. It is free in power terms because the CPU would otherwise be idle
// waiting for the waveform. See "Page-turn latency" in docs/display.md.
void ReaderScreen::prerender_next_page_(DrawBuffer& buf) {
  predrawn_.valid = false;
  predraw_after_idle_ = false;
  if (!open_ok_ || page_ + 1 >= wgb_.page_count(layout_))
    return;
  // Only what stage_next_page_ already read: reading now would put card
  // traffic under the running waveform.
  if (!loaded_.valid || loaded_.layout != layout_ || loaded_.index != page_ + 1)
    return;
  // An image page needs the card to draw, and the card shares SPI2 with the
  // panel: draw it on the first frame after the waveform instead (update()).
  // The loop runs frames while the panel is busy, so that frame comes ~25 ms
  // after the refresh ends — long before the reader turns again.
  if (page_has_image_() && buf.display().is_busy()) {
    predraw_after_idle_ = true;
    return;
  }
  if (!buf.begin_offscreen())
    return;
  draw_page_(buf);
  buf.end_offscreen();
  predrawn_ = PreDrawn{true, layout_, page_ + 1};
}

// Put the pre-drawn page on screen instead of rendering it. Returns false for any
// mismatch, in which case the caller renders normally — this is only ever an
// optimisation, never the source of truth.
bool ReaderScreen::take_predrawn_(DrawBuffer& buf) {
  if (!predrawn_.valid || !buf.has_offscreen())
    return false;
  if (predrawn_.layout != wanted_layout_(buf) || predrawn_.layout != layout_ || predrawn_.index != page_)
    return false;
  if (!buf.commit_offscreen())
    return false;
  predrawn_.valid = false;
  return true;
}

void ReaderScreen::render_page_(DrawBuffer& buf) {
  sync_layout_(buf);
  // A record that does not read draws as a blank page (no error text, by rule).
  if (!load_page_(buf, layout_, page_))
    page_len_ = 0;
  draw_page_(buf);
}

void ReaderScreen::draw_page_(DrawBuffer& buf) {
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);

  // Items are stored in draw order (lines, rules, images); a malformed list
  // just stops drawing where it goes wrong.
  WgbPageView v(page_buf_, page_len_);
  uint8_t tag;
  while (v.peek(tag)) {
    if (tag == kWgbItemLine) {
      WgbLine line;
      if (!v.read_line(line))
        break;
      buf.draw_baked_line(line, v.end(), *ext_font_set_, false);
    } else if (tag == kWgbItemRule) {
      WgbRule r;
      if (!v.read_rule(r))
        break;
      buf.fill_rect(r.x, r.y + r.h / 2 - 1, r.w, 2, false);
    } else if (tag == kWgbItemImage) {
      WgbImage img;
      if (!v.read_image(img))
        break;
      // The image table is on the card: wait the waveform out before reading it.
      buf.wait_panel_idle();
      WgbImageRef ref;
      if (!wgb_.image_ref(img.key, ref) ||
          !draw_image_(ref.data_offset, ref.data_size, buf, img.x, img.y, img.w, img.full_h, img.src_y, img.clip_h)) {
        // Leave the space blank rather than filling it black — a book whose image
        // failed to rasterise at conversion time has a zero-size entry here, and a
        // black slab per figure is far worse than white space.
        buf.fill_rect(img.x, img.y, img.w, img.full_h, true);
      }
    } else {
      break;
    }
  }

  // Charge reminder. Drawn last so an image reaching the bottom margin cannot
  // paint over it, and in logical coordinates so it sits along the bottom edge
  // in both orientations.
  if (low_battery_)
    buf.fill_rect(0, H - kLowBatteryBarH, W, kLowBatteryBarH, false);
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
