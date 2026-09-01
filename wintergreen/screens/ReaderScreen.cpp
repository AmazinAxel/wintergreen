#include "ReaderScreen.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "../Application.h"
#include "../content/BookIndex.h"
#include "../content/CoverPaths.h"
#include "../display/ui_font_small.h"

#ifdef ESP_PLATFORM
#include <sys/stat.h>
#include <unistd.h>

#include "esp_system.h"
#include "esp_timer.h"
#else
#include <filesystem>
#endif

namespace wintergreen {

// ---------------------------------------------------------------------------
// ReaderScreen — path helpers
// ---------------------------------------------------------------------------

std::string ReaderScreen::book_stem_() const {
  const char* name = path_.c_str();
  const char* sep = std::strrchr(name, '/');
#ifdef _WIN32
  const char* bsep = std::strrchr(name, '\\');
  if (bsep && (!sep || bsep > sep))
    sep = bsep;
#endif
  if (sep)
    name = sep + 1;
  const char* dot = std::strrchr(name, '.');
  size_t len = dot ? static_cast<size_t>(dot - name) : std::strlen(name);
  return std::string(name, len);
}

std::string ReaderScreen::display_title_() const {
  const std::string& t = wgb_.metadata().title;
  if (!t.empty() && t != "none")
    return t;
  // Same fallback BookIndex uses: the containing folder, which the converter
  // names after the original file.
  const size_t last = path_.find_last_of('/');
  if (last == std::string::npos || last == 0)
    return book_stem_();
  const size_t prev = path_.find_last_of('/', last - 1);
  const size_t start = prev == std::string::npos ? 0 : prev + 1;
  return path_.substr(start, last - start);
}

// ---------------------------------------------------------------------------
// ReaderScreen — image size resolution
// ---------------------------------------------------------------------------

// resolve_image_size_ removed — image size resolution is now handled by
// make_image_size_query() (WgbReader.h), stored in image_size_fn_.

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
  if (static_cast<uint32_t>(row_bytes) * height > size)
    return false;
  const uint16_t first = src_y;
  uint16_t last = height;
  if (clip_h > 0 && static_cast<uint32_t>(first) + clip_h < last)
    last = static_cast<uint16_t>(first + clip_h);
  if (first >= last)
    return false;

  if (std::fseek(f, static_cast<long>(offset) + static_cast<long>(first) * row_bytes, SEEK_SET) != 0)
    return false;
  std::vector<uint8_t> row(row_bytes);
  for (uint16_t r = first; r < last; ++r) {
    if (std::fread(row.data(), 1, row_bytes, f) != row_bytes)
      break;
    buf.blit_1bit_row(dest_x, dest_y + static_cast<int>(r - first), row.data(), width);
  }
  return true;
}

// Latching: once the bar is up it stays up until the device is plugged in.
// A cell sags a few hundred millivolts under an e-ink refresh, so an unlatched
// threshold would flicker the bar on and off between page turns right around
// 15%, which is exactly where it matters.
void ReaderScreen::sample_battery_(IRuntime& runtime) {
  if (app_ && app_->usb_powered()) {
    low_battery_ = false;
    return;
  }
  if (low_battery_)
    return;
  const std::optional<uint8_t> pct = runtime.battery_percentage();
  // No reading at all (no cell, uncalibrated ADC) means no bar — the same rule
  // the low-battery cutoff follows.
  low_battery_ = pct.has_value() && *pct <= kLowBatteryBarPct;
}

void ReaderScreen::start(DrawBuffer& buf, IRuntime& runtime) {
  sample_battery_(runtime);
  buf_ = &buf;
  runtime_ = &runtime;
  snapshot_ok_ = false;  // a fresh book must never resume into a previous one's pixels
  predrawn_.valid = false;
  pos_written_valid_ = false;
  if (app_)
    buf.set_rotation(rotation_from_setting(app_->rotate_reader()));
  pos_path_.clear();

  // The path *is* the book: BookIndex only recognises .wgb, so nothing else can
  // reach here. The old else-branch pointed at <data_dir>/cache/<stem>/book.wgb
  // for an EPUB that the firmware would convert — but on-device conversion and
  // EPUB support are both gone, so it created cache directories for a file it
  // could never produce.
  wgb_path_ = path_;

  bool wgb_ok = wgb_.open(wgb_path_.c_str());

  buf_was_touched_ = false;

  if (!wgb_ok) {
    open_ok_ = false;
    goto show_error;
  }

  pos_path_ = book_pos_path(wgb_path_.c_str());

  open_ok_ = true;
  chapter_idx_ = 0;
  page_pos_ = PagePosition{0, 0};
  image_size_fn_ = make_image_size_query(wgb_);
  saved_chapter_idx_ = 0;
  saved_page_pos_ = PagePosition{0, 0};
  load_position_();
  load_chapter_(saved_chapter_idx_);
  if (!chapter_src_) {
    // Fallback to chapter 0 if saved index is invalid.
    saved_chapter_idx_ = 0;
    saved_page_pos_ = PagePosition{0, 0};
    load_chapter_(0);
  }
  if (!chapter_src_) {
    open_ok_ = false;
    goto show_error;
  }
  page_pos_ = saved_page_pos_;
  layout_engine_ = TextLayout{};
  layout_engine_.set_source(*chapter_src_);
  layout_engine_.set_image_size_fn(image_size_fn_);
  layout_engine_.set_hyphenation_lang(detect_language(wgb_.metadata().language));
  // Not done here: the step-back needs a laid-out engine, and set_font/
  // set_options happen inside render_page_. Doing it at this point laid out
  // with font_ still null and took a load fault. render_page_ consumes the
  // flag once, after the engine is fully configured.
  resume_back_pending_ = config::kResumeOnePageBack;
  render_page_(buf);
  return;

show_error:
  if (buf_was_touched_) {
    buf.fill(true);
    buf.draw_text(kPaddingLeft, kPaddingTop, "Failed to open book", true, kScale);
  }
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
  snapshot_ok_ = true;
}

void ReaderScreen::resume(DrawBuffer& buf, IRuntime& runtime) {
  sample_battery_(runtime);
  buf_ = &buf;
  runtime_ = &runtime;
  if (app_)
    buf.set_rotation(rotation_from_setting(app_->rotate_reader()));
  if (!open_ok_)
    return;

  // Fast path: nothing that affects the pixels changed while the quick menu was
  // up, so put the snapshot back instead of laying the page out and drawing
  // every glyph again. The caller (Application's pop handler) does the refresh.
  // page_pos_, page_.end and page_cache_ are all untouched, so the next page
  // turn still hits the prerendered page.
  const bool jumping = app_ && app_->quickmenu()->has_pending();
  if (!jumping && snapshot_ok_ && reader_settings_.font_size_idx == snapshot_font_size_ &&
      (app_ ? app_->rotate_reader() : 0) == snapshot_rotation_ && buf.has_snapshot() &&
      buf.restore_snapshot()) {
    snapshot_ok_ = false;
    return;
  }
  snapshot_ok_ = false;

  // Handle pending chapter jump (chosen from the in-book quick menu).
  if (app_ && app_->quickmenu()->has_pending()) {
    saved_chapter_idx_ = app_->quickmenu()->pending_chapter();
    saved_page_pos_ = PagePosition{app_->quickmenu()->pending_para_index(), 0, 0};
    app_->quickmenu()->clear_pending();
    load_chapter_(saved_chapter_idx_);
    page_pos_ = saved_page_pos_;
    layout_engine_.set_source(*chapter_src_);
    layout_engine_.set_image_size_fn(image_size_fn_);
    layout_engine_.set_hyphenation_lang(detect_language(wgb_.metadata().language));
  }
  // Check if font settings changed (font_size_idx may have been updated in options).
  if (const BitmapFontSet* fset = ext_font_set_ ? ext_font_set_ : (font_set_.valid() ? &font_set_ : nullptr)) {
    const_cast<BitmapFontSet*>(fset)->set_base_size_index(reader_settings_.font_size_idx);
  }

  // Make sure to apply settings when coming from the reader options screen
  if (chapter_src_)
    layout_engine_.set_source(*chapter_src_);

  render_page_(buf);
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
  image_size_fn_ = {};
  chapter_src_.reset();
  wgb_.close();
  page_ = PageContent{};
  page_cache_ = LaidOutPageCache{};  // its words pointed into the source just freed
  predrawn_.valid = false;
  wgb_path_.clear();
  wgb_path_.shrink_to_fit();
  pos_path_.clear();
  pos_path_.shrink_to_fit();
  open_ok_ = false;
  // Restore the global menu rotation before handing the buffer back.
  if (buf_)
    buf_->set_rotation(Rotation::Deg90);
  // saved_chapter_idx_ / saved_page_pos_ are intentionally NOT reset here —
  // resume() uses them as the nav-history origin when a link jump is pending.
  buf_ = nullptr;
}

void ReaderScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  if (!open_ok_) {
    // Auto-pop if the book was never found (no display was touched).
    // Otherwise wait for back button so user can see the error message.
    if (!buf_was_touched_) {
      app_->pop_screen();
      return;
    }
    // Still drain the history so stale events don't bleed into the next frame.
    Button btn;
    while (buttons.next_press(btn)) {
      if (btn == Button::Button0) {
        app_->pop_screen();
        return;
      }
    }
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
  // Landscape is Deg0 or Deg180 — "reversed" is still the same physical hold, so
  // testing only Deg0 would leave the flipped case behaving like portrait.
  const Rotation rot = buf.rotation();
  const bool landscape_hold = (rot == Rotation::Deg0 || rot == Rotation::Deg180);
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
        case Button::Button0:
          app_->pop_screen();
          return;
        case Button::Button1:
          saved_chapter_idx_ = chapter_idx_;
          saved_page_pos_ = page_pos_;
          app_->quickmenu()->set_settings(&reader_settings_);
          app_->quickmenu()->populate(wgb_.toc(), static_cast<uint16_t>(chapter_idx_), page_pos_.paragraph,
                                           display_title_(), progress_pct(), chapter_progress_pct(),
                                           wgb_.chapter_count());
          app_->push_screen(ScreenId::Quickmenu);
          return;
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

  if (changed) {
    // One ADC read per page turn, not per frame. A pre-drawn page carries the
    // sample taken when it was drawn, so the bar can lag one turn.
    sample_battery_(runtime);
    // A forward turn may already have the page drawn and waiting.
    if (page_delta <= 0 || !take_predrawn_(buf))
      render_page_(buf);
    buf.refresh();
    // The panel is now busy for a few hundred ms and the reader is
    // overwhelmingly likely to go forward again. A backward turn already leaves
    // prev_page_()'s layout in page_cache_.
    if (page_delta > 0)
      prerender_next_page_(buf, runtime);
  }
}

// Lay out *and draw* the page after the current one, into the offscreen buffer,
// while the panel is still running this page's waveform.
//
// The next forward turn then costs a memcpy and an SPI write: no layout, no
// glyph blitting. It is the same idea as the quick-menu snapshot, applied
// forwards, and it is free in power terms because the CPU would otherwise be
// idle waiting for the waveform.
//
// Restoring state afterwards is the fiddly part. render_page_() works on page_
// and page_pos_, so the next page is made current for the duration of the draw
// and then put back. `page_` keeps only positional metadata either way — its
// LayoutWord::text pointers into WgbChapterSource's 32-slot paragraph window are
// dead the moment the window slides, which is exactly what laying out the next
// page does. See "Page-turn latency" in CLAUDE.md.
void ReaderScreen::prerender_next_page_(DrawBuffer& buf, IRuntime& runtime) {
  predrawn_.valid = false;
  if (!open_ok_ || !chapter_src_ || page_.at_chapter_end)
    return;  // a chapter crossing needs load_chapter_(); not worth doing speculatively

  PagePosition next = page_.end;
  // Same image snap-back as next_page_(), so the drawn page is the one the next
  // turn will ask for. A mismatch only wastes the work, but that is the point.
  if (next.offset > 0 && next.paragraph < chapter_src_->paragraph_count() &&
      page_pos_.paragraph != next.paragraph) {
    if (chapter_src_->paragraph(next.paragraph).type == ParagraphType::Image ||
        layout_engine_.is_mid_promoted_image(next))
      next.offset = 0;
  }
  if (next == page_pos_)
    return;

  // **Bail out before laying anything out when there is no spare to draw into.**
  // The whole point of this function is to have the next page *drawn* by the
  // time the user asks for it; without the spare the draw cannot happen, and
  // the layout below is speculative work whose only product is a cached page.
  //
  // It used to fall through to begin_offscreen() at the draw step instead, so
  // with a radio up — which is exactly when the spare is released — every
  // forward turn still paid a full speculative layout and cached its result.
  // That is a whole page's worth of line and word vectors allocated on a heap
  // that has none to spare, and it is where split_words' reserve threw
  // std::bad_alloc (an abort, with exceptions off).
  if (!buf.can_offscreen())
    return;

  // **Skip the speculative page when the heap is low.** This whole function is
  // an optimisation — it lays out and draws the *next* page so a forward turn
  // is a memcpy — but it doubles the layout memory live at once, and on a book
  // with long paragraphs (the Odyssey) that is what tips the device over:
  // free heap fell 51 KB -> 14 KB within three page turns and the next
  // allocation aborted, with this function on the stack every time.
  //
  // Skipping costs a slower page turn. Not skipping costs a reboot mid-book,
  // so the trade is not close. The threshold is well above one page's layout
  // so that the *real* page after this one still has room.
  if (runtime.largest_free_block_bytes() < kPrerenderMinBlockBytes)
    return;

  const PagePosition cur_pos = page_pos_;
  const PageContent cur_meta{{}, {}, page_.start, page_.end, page_.at_chapter_end};

  layout_engine_.set_position(next);
  PageContent pc = layout_engine_.layout();

  // Drawing an image reads the book off the SD card, and the card shares SPI2
  // with the panel — draw_image_() therefore drains the waveform first. Doing
  // that *here* would block the UI loop for the couple of hundred milliseconds
  // the page we just showed is still painting, so an image page gets its layout
  // cached and nothing more. Text pages, which is nearly all of them, are drawn.
  bool has_image = false;
  for (const auto& ci : pc.items) {
    if (std::get_if<PageImageItem>(&ci)) {
      has_image = true;
      break;
    }
    if (const PageTextItem* ti = std::get_if<PageTextItem>(&ci)) {
      if (ti->inline_image.has_value()) {
        has_image = true;
        break;
      }
    }
  }

  page_pos_ = next;
  cache_page_(std::move(pc));

  // No spare (released to the heap for the BLE stack) means no pre-draw — the
  // layout above is still cached, so the turn is a normal render rather than a
  // memcpy. Must not fall through: begin_offscreen would otherwise leave
  // draw_target_ pointing at the live buffer.
  // **Re-check before drawing.** The layout above is itself the largest
  // allocation this function makes, so the guard at the top of the function was
  // testing the heap as it stood *before* the speculative page existed. Drawing
  // now runs render_page_, which allocates again — and if its own guard fires it
  // drops the page cache this function just filled, so the draw re-lays the same
  // page out with even less room. Bail instead: the layout stays cached, so the
  // next turn is a normal render rather than a memcpy.
  if (runtime.largest_free_block_bytes() < kLayoutMinBlockBytes) {
    page_ = cur_meta;
    page_pos_ = cur_pos;
    layout_engine_.set_position(cur_pos);
    return;
  }

  if (!has_image && buf.begin_offscreen()) {
    in_prerender_draw_ = true;
    render_page_(buf);  // consumes the cached layout and draws into the spare buffer
    in_prerender_draw_ = false;
    buf.end_offscreen();

    predrawn_.valid = true;
    predrawn_.chapter_idx = chapter_idx_;
    predrawn_.request = next;  // pre-resolve, which is what next_page_() will produce
    predrawn_.start = page_.start;
    predrawn_.end = page_.end;
    predrawn_.at_chapter_end = page_.at_chapter_end;
    predrawn_.opts = last_opts_;
    predrawn_.font_size_idx = reader_settings_.font_size_idx;
  }

  page_ = cur_meta;
  page_pos_ = cur_pos;
  layout_engine_.set_position(cur_pos);
}

// Put the pre-drawn page on screen instead of rendering it. Returns false for any
// mismatch, in which case the caller renders normally — this is only ever an
// optimisation, never the source of truth.
bool ReaderScreen::take_predrawn_(DrawBuffer& buf) {
  if (!predrawn_.valid || !buf.has_offscreen())
    return false;
  if (predrawn_.chapter_idx != chapter_idx_ || !(predrawn_.request == page_pos_))
    return false;
  if (predrawn_.font_size_idx != reader_settings_.font_size_idx)
    return false;
  const bool landscape = (buf.rotation() == Rotation::Deg0 || buf.rotation() == Rotation::Deg180);
  PageOptions opts = make_page_opts(buf.width(), buf.height());
  opts.padding_bottom = bottom_padding_(landscape);
  if (!(predrawn_.opts == opts))
    return false;
  if (!buf.commit_offscreen())
    return false;

  page_ = PageContent{{}, {}, predrawn_.start, predrawn_.end, predrawn_.at_chapter_end};
  page_pos_ = predrawn_.start;
  last_opts_ = opts;
  predrawn_.valid = false;
  return true;
}


void ReaderScreen::cache_page_(PageContent&& pc) {
  page_cache_.valid = true;
  page_cache_.chapter_idx = chapter_idx_;
  page_cache_.pos = pc.start;
  page_cache_.opts = last_opts_;
  page_cache_.font_size_idx = reader_settings_.font_size_idx;
  page_cache_.page = std::move(pc);
}

bool ReaderScreen::take_cached_page_(const PagePosition& pos, const PageOptions& opts) {
  if (!page_cache_.valid || page_cache_.chapter_idx != chapter_idx_ || !(page_cache_.pos == pos) ||
      !(page_cache_.opts == opts) || page_cache_.font_size_idx != reader_settings_.font_size_idx)
    return false;
  // Move, never copy: LayoutWord::text points into WgbChapterSource's paragraph
  // window, so only one PageContent may be live at a time.
  page_ = std::move(page_cache_.page);
  page_cache_ = LaidOutPageCache{};
  return true;
}

void ReaderScreen::load_chapter_(size_t idx) {
  // Rebuilding the source frees every paragraph slot the cached page's words point
  // into. Nothing in the key can detect that, so it has to be dropped explicitly.
  page_cache_ = LaidOutPageCache{};
  chapter_src_.reset();
  if (idx < wgb_.chapter_count()) {
    chapter_src_ = std::make_unique<WgbChapterSource>(wgb_, static_cast<uint16_t>(idx));
    chapter_idx_ = idx;
    layout_engine_.set_source(*chapter_src_);
  }
}

void ReaderScreen::render_page_(DrawBuffer& buf) {
  const int W = buf.width();
  const int H = buf.height();


  // Use proportional font if available, otherwise fallback to fixed.
  FixedFont fixed_font(kGlyphW * kScale, kGlyphH * kScale + 4);
  const BitmapFontSet* fset = ext_font_set_ ? ext_font_set_ : (font_set_.valid() ? &font_set_ : nullptr);
  if (fset) {
    const_cast<BitmapFontSet*>(fset)->set_base_size_index(reader_settings_.font_size_idx);
  }
  IFont& font = fset ? static_cast<IFont&>(const_cast<BitmapFontSet&>(*fset)) : static_cast<IFont&>(fixed_font);

  const bool landscape = (buf.rotation() == Rotation::Deg0 || buf.rotation() == Rotation::Deg180);
  PageOptions opts = make_page_opts(W, H);
  opts.padding_bottom = bottom_padding_(landscape);
  layout_engine_.set_font(font);
  layout_engine_.set_options(opts);
  last_opts_ = opts;
  // Step back one page on a fresh open (config::kResumeOnePageBack). It has to
  // land here rather than in start(): prev_page_() runs a backward layout, and
  // the engine has no font or options until the two calls above.
  if (resume_back_pending_) {
    resume_back_pending_ = false;
    resume_one_page_back_();
  }
  // resolve_stable_position can move page_pos_, so the cache is keyed on the
  // resolved value — checking before this point would match the wrong page.
  page_pos_ = layout_engine_.resolve_stable_position(page_pos_);
  layout_engine_.set_position(page_pos_);

  const bool cached = take_cached_page_(page_pos_, opts);
  if (!cached) {
    // **Last line of defence before laying out.** Exceptions are off, so a
    // std::bad_alloc inside layout is an abort() — the device reboots mid-book
    // with no message, which is what reading with the clicker connected did.
    // There is no way to recover after the fact, so the only option is to check
    // before allocating.
    //
    // Everything dropped here is a cache that rebuilds on demand, so the cost is
    // a slower page turn. The page still renders.
    // Not while drawing a speculative page: prerender_next_page_ has already
    // made this check and put the layout it wants in page_cache_, so dropping
    // the cache here would make it lay the same page out again with less room
    // than the check was made against — the opposite of what the guard is for.
    if (!in_prerender_draw_ && runtime_) {
      const uint32_t block = runtime_->largest_free_block_bytes();
      // **Two stages, because one threshold kept being not quite enough.** The
      // failures have all been small allocations (192, 256, 1212, 1416 bytes) on
      // a fragmented heap, which means the margin was thinner than any single
      // number predicted. Shedding early and cheaply is better than shedding
      // late and completely.
      if (block < kLayoutShedBlockBytes) {
        // Mild pressure: give back the speculative page and the one-page cache.
        // Both are pure latency optimisations and cost nothing to rebuild.
        page_cache_ = LaidOutPageCache{};
        predrawn_.valid = false;
      }
      if (block < kLayoutMinBlockBytes) {
        // Real pressure: also hand back the paragraph cache and stop it
        // refilling. This is the state the reader runs in with a radio up.
        layout_engine_.release_cache_memory();
        layout_engine_.set_cache_limit(1);
        // The spare framebuffer is deliberately *not* released here. It is 48 KB
        // and tempting, but render_page_ can be reached from inside an offscreen
        // draw, and freeing the buffer being drawn into is worse than the abort
        // this is preventing. Application::release_ram_for_radio() is where the
        // spare is given back, on a path that owns the whole frame.
      }
    }
    page_ = layout_engine_.layout();
  }

  // ─────────────────────────────────
  struct ImageToDraw {
    uint16_t key;
    int x, y, w, h;
    uint32_t offset;
    uint32_t size;
    uint16_t src_y = 0;
    uint16_t clip_h = 0;  // rendered slice height (0 = full)
  };
  std::vector<ImageToDraw> images;
  auto collect_img = [&](const PageImageItem& img_item) {
    const int img_w = static_cast<int>(img_item.width);
    const int img_h = static_cast<int>(img_item.height);
    if (img_w <= 0 || img_h <= 0)
      return;
    if (img_item.key >= wgb_.image_count())
      return;
    ImageToDraw itd;
    itd.key = img_item.key;
    itd.x = static_cast<int>(img_item.x_offset);
    itd.y = static_cast<int>(img_item.y_offset);  // y_offset is absolute (vertical centering baked in)
    itd.w = img_w;
    // Use full_height as max_h so the decoder scales to the correct aspect ratio;
    // src_y crops to the visible slice within that full render.
    itd.h = img_item.full_height > 0 ? static_cast<int>(img_item.full_height) : img_h;
    itd.offset = wgb_.image_ref(img_item.key).data_offset;
    itd.size = wgb_.image_ref(img_item.key).data_size;
    itd.src_y = img_item.y_crop;
    // Clip rendered rows to the slice height so the image doesn't overflow past
    // its layout-assigned area (e.g. into the page number zone or page N+1).
    itd.clip_h = static_cast<uint16_t>(img_h);
    images.push_back(itd);
  };
  for (const auto& ci : page_.items) {
    if (const PageImageItem* img = std::get_if<PageImageItem>(&ci)) {
      collect_img(*img);
    } else if (const PageTextItem* ti = std::get_if<PageTextItem>(&ci)) {
      if (ti->inline_image.has_value())
        collect_img(*ti->inline_image);
    }
  }

  // ── BW rendering
  // ────────────────────────────────────────────────────────
  buf.fill(true);

  if (fset) {
    render_text_(buf, *fset, reader_settings_.h_padding());
  } else {
    for (const auto& ci : page_.items) {
      const PageTextItem* item = std::get_if<PageTextItem>(&ci);
      if (!item)
        continue;
      for (const auto& w : item->line.words) {
        if (w.len == 0)
          continue;
        char text[64];
        int tlen = static_cast<int>(w.len);
        if (tlen > 63)
          tlen = 63;
        std::memcpy(text, w.text, tlen);
        text[tlen] = '\0';
        buf.draw_text_no_bg(reader_settings_.h_padding() + w.x, static_cast<int>(item->y_offset), text, false /*black*/,
                            kScale);
      }
    }
  }

  for (const auto& hr : page_.items) {
    const PageHrItem* h = std::get_if<PageHrItem>(&hr);
    if (!h)
      continue;
    const int hr_y = static_cast<int>(h->y_offset) + static_cast<int>(h->height) / 2;
    buf.fill_rect(static_cast<int>(h->x_offset), hr_y - 1, static_cast<int>(h->width), 2, false);
  }

  for (const auto& itd : images) {
    if (!draw_image_(itd.offset, itd.size, buf, itd.x, itd.y, static_cast<uint16_t>(itd.w),
                     static_cast<uint16_t>(itd.h), itd.src_y, itd.clip_h)) {
      // Leave the space blank rather than filling it black — a book whose image
      // failed to rasterise at conversion time has a zero-size entry here, and a
      // black slab per figure is far worse than white space.
      buf.fill_rect(itd.x, itd.y, itd.w, itd.h, true);
    }
  }

  // Charge reminder. Drawn last so an image reaching the bottom margin cannot
  // paint over it, and in logical coordinates so it sits along the bottom edge
  // in both orientations.
  if (low_battery_)
    buf.fill_rect(0, H - kLowBatteryBarH, W, kLowBatteryBarH, false);
}

uint16_t ReaderScreen::bottom_padding_(bool landscape) {
  return static_cast<uint16_t>(ReaderSettings::bottom_margin() + ReaderSettings::v_padding() +
                               (landscape ? 2 : 0));
}

bool ReaderScreen::render_current_page(DrawBuffer& buf) {
  if (!open_ok_)
    return false;
  render_page_(buf);
  return true;
}

bool ReaderScreen::next_page_and_render(DrawBuffer& buf) {
  if (!open_ok_)
    return false;
  if (!next_page_())
    return false;
  render_page_(buf);
  return true;
}

bool ReaderScreen::is_open_ok() const {
  return open_ok_;
}

size_t ReaderScreen::current_chapter_index() const {
  return chapter_idx_;
}

void ReaderScreen::render_text_(DrawBuffer& buf, const BitmapFontSet& fset, int left_padding) {
  uint8_t* render = buf.render_buf();
  for (const auto& ci : page_.items) {
    const PageTextItem* item = std::get_if<PageTextItem>(&ci);
    if (!item)
      continue;
    int baseline_y = static_cast<int>(item->y_offset) + item->baseline;
    buf.draw_layout_line(render, left_padding, baseline_y, item->line, fset, GrayPlane::BW, false);
  }
}

bool ReaderScreen::next_page_() {
  if (page_.at_chapter_end) {
    if (chapter_idx_ + 1 < wgb_.chapter_count()) {
      load_chapter_(chapter_idx_ + 1);
      page_pos_ = PagePosition{0, 0};
      return true;
    }
    return false;
  }
  PagePosition next = page_.end;
  // If the page ended mid-image (offset > 0 into an image paragraph or
  // mid-promoted-inline-image), snap back to the start of that paragraph
  // so the next page shows the full image.
  // We only do this if the image wasn't the first thing on the current page,
  // to avoid infinite loops on images taller than the screen.
  if (next.offset > 0 && chapter_src_ && next.paragraph < chapter_src_->paragraph_count()) {
    if (page_pos_.paragraph != next.paragraph) {
      if (chapter_src_->paragraph(next.paragraph).type == ParagraphType::Image ||
          layout_engine_.is_mid_promoted_image(next))
        next.offset = 0;
    }
  }
  page_pos_ = next;
  return true;
}


bool ReaderScreen::prev_page_() {
  if (page_pos_ == PagePosition{0, 0}) {
    if (chapter_idx_ > 0) {
      load_chapter_(chapter_idx_ - 1);
      // Jump to the last page of the previous chapter using backward layout.
      const uint16_t end_para = static_cast<uint16_t>(chapter_src_->paragraph_count());
      layout_engine_.set_position(PagePosition{end_para, 0});
      auto pc = layout_engine_.layout_backward();
      page_pos_ = pc.start;
      cache_page_(std::move(pc));
      return true;
    }
    return false;
  }

  // If the current page starts mid-image, snap end to the bottom of that image
  // so layout_backward produces the page ending at the image bottom — which
  // naturally includes the full image (rows 0..end) plus whatever text fits above.
  const PagePosition cur_start = page_pos_;

  // **Same pre-check render_page_ makes, for the same reason.** A backward turn
  // lays out at least one page and the resync below can lay out three more, and
  // none of that was guarded — a `std::bad_alloc` here is an abort() and a reboot
  // mid-book, which is exactly what scrolling back through a book with the
  // clicker connected produced. The caches dropped are rebuilt on demand.
  const bool heap_tight = runtime_ && runtime_->largest_free_block_bytes() < kLayoutMinBlockBytes;
  if (heap_tight) {
    page_cache_ = LaidOutPageCache{};
    predrawn_.valid = false;
    layout_engine_.release_cache_memory();
    layout_engine_.set_cache_limit(1);
  }

  PagePosition end = layout_engine_.snap_to_image_end(page_pos_);
  layout_engine_.set_position(end);
  auto pc = layout_engine_.layout_backward();

  // Forward and backward now share one line-fitting rule (see
  // require_full_height in TextLayout), which removed the systematic
  // disagreement. What is left is structural: a page ending at a PageBreak, or
  // at the end of a chapter's content, can legitimately be reached from several
  // starts, and backward picks the fullest rather than the one the reader came
  // from. Left alone that still *skips text* — measured at 1-2 places per book.
  //
  // Resolve it by stepping back one further page for an anchor, then walking
  // forward and keeping the last page that ends at or before cur_start. Forward
  // is deterministic, so from a start that precedes the true predecessor it
  // lands on it exactly.
  //
  // **Bounded on purpose.** Anchoring at the chapter start instead also works
  // and gives perfect agreement, but walking the chapter drags
  // WgbChapterSource's 32-slot paragraph window across it and peaked around
  // 31 KB — with the clicker resident that reintroduced the heap abort, and it
  // put a per-chapter walk on the book-open path. One extra step back costs one
  // layout_backward and at most three forward boundary collections, and removes
  // every skip across all four test books.
  //
  // **Skipped entirely when the heap is tight.** It is a refinement — backward's
  // own answer is a real page, just occasionally not the one the reader came
  // from — and it is the most allocation-hungry part of a backward turn.
  if (!heap_tight) {
    PagePosition anchor = pc.start;
    if (!(anchor == PagePosition{0, 0})) {
      layout_engine_.set_position(anchor);
      PageContent back2 = layout_engine_.layout_backward();
      if (back2.start < anchor)
        anchor = back2.start;
    }

    PagePosition probe = anchor;
    PagePosition best = pc.start;
    for (int guard = 0; guard < kPrevPageProbeLimit; ++guard) {
      const PagePosition probe_end = layout_engine_.layout_end(probe);
      if (!(probe < probe_end))
        break;  // no forward progress: malformed chapter, keep backward's answer
      if (!(probe_end < cur_start)) {
        // Ends at or past where we started. Ending exactly there makes it the
        // predecessor; overshooting means cur_start is not on this chain (a
        // chapter jump or a restored bookmark), so backward's answer stands.
        if (probe_end == cur_start)
          best = probe;
        break;
      }
      probe = probe_end;
    }
    if (!(best == pc.start)) {
      layout_engine_.set_position(best);
      pc = layout_engine_.layout();
    }
  }

  page_pos_ = pc.start;
  // The page we just navigated to is fully laid out right here. Keep it instead of
  // making render_page_ lay out the identical page a second time.
  cache_page_(std::move(pc));
  return true;
}

// Step back one page on a fresh open so the last page read is shown again.
// Driven by resume_back_pending_, which start() sets and render_page_ consumes
// once the layout engine has a font and options. Every wake reaches this —
// deep-sleep wake is a full boot.
//
// prev_page_() is reused rather than reimplemented: it already handles the
// chapter crossing, the mid-image snap, and caching the laid-out page so
// render_page_() does not lay the same page out twice. Its own guard covers the
// very start of the book (page_pos_ {0,0} in chapter 0 → false, no move).
void ReaderScreen::resume_one_page_back_() {
  if (!open_ok_ || !chapter_src_)
    return;
  // Note `page_` is not populated yet — render_page_() runs after this — so the
  // end-of-book test cannot use page_.at_chapter_end. The saved position being
  // at the very start is handled by prev_page_() itself, which returns false
  // without moving. The end of the book needs no guard either: a saved position
  // is the *start* of the last page, so there is always a page behind it.
  prev_page_();
  // Keep the .pos dedup key matching the file, not the page now shown, so this
  // does not walk the bookmark backwards one page per wake when nothing is read.
  // save_position_() writes only if the position moves past it.
}

// ---------------------------------------------------------------------------
// Bookmark persistence
// ---------------------------------------------------------------------------

void ReaderScreen::save_position_() {
  if (pos_path_.empty())
    return;
  // Skip the write when the position has not moved since it was loaded or last
  // written — opening a book and backing straight out, or paging forward and
  // back to where you started, then costs the card nothing. The file is four
  // numbers but a flash erase block is not.
  const PosKey now{chapter_idx_, page_pos_.paragraph, page_pos_.offset, page_pos_.text_offset};
  if (pos_written_valid_ && now == pos_written_)
    return;
  FILE* f = std::fopen(pos_path_.c_str(), "w");
  if (!f)
    return;
  // Position only: chapter, paragraph, offset, text offset. Reading statistics
  // are not tracked at all, so nothing else needs persisting.
  std::fprintf(f, "%u %u %u %u\n", static_cast<unsigned>(chapter_idx_),
               static_cast<unsigned>(page_pos_.paragraph), static_cast<unsigned>(page_pos_.offset),
               static_cast<unsigned>(page_pos_.text_offset));
  if (std::fclose(f) == 0) {
    pos_written_ = now;
    pos_written_valid_ = true;
  }
}

void ReaderScreen::load_position_() {
  if (pos_path_.empty())
    return;
  FILE* f = std::fopen(pos_path_.c_str(), "r");
  if (!f)
    return;
  unsigned ch = 0, para = 0, line = 0, to = 0;
  const int scanned = std::fscanf(f, "%u %u %u %u", &ch, &para, &line, &to);
  std::fclose(f);
  if (scanned >= 3) {
    saved_chapter_idx_ = ch;
    saved_page_pos_ = PagePosition{static_cast<uint16_t>(para), static_cast<uint16_t>(line), static_cast<uint32_t>(to)};
    // Seed the write-dedup key: this is what the file already contains.
    pos_written_ = PosKey{ch, static_cast<uint16_t>(para), static_cast<uint16_t>(line), to};
    pos_written_valid_ = true;
  }
}

}  // namespace wintergreen
