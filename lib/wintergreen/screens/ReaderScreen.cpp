#include "ReaderScreen.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "../Application.h"
#include "../content/BookIndex.h"
#include "../HeapLog.h"
#include "../display/ui_font_small.h"

#ifdef ESP_PLATFORM
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
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

// ---------------------------------------------------------------------------
// ReaderScreen — image size resolution
// ---------------------------------------------------------------------------

// resolve_image_size_ removed — image size resolution is now handled by
// make_image_size_query() (MrbReader.h), stored in image_size_fn_.

bool ReaderScreen::decode_image_to_buffer_(uint16_t img_key, uint32_t offset, uint32_t size, DrawBuffer& buf,
                                           int dest_x, int dest_y, uint16_t max_w, uint16_t max_h, uint16_t src_y,
                                           uint16_t clip_h) {
  if (offset == 0 || size == 0)
    return false;
  char cache_path[256];
  snprintf(cache_path, sizeof(cache_path), "%s/img_%u_%ux%u.bin", book_cache_dir_.c_str(),
           static_cast<unsigned>(img_key), static_cast<unsigned>(max_w), static_cast<unsigned>(max_h));

  FILE* cache_f = std::fopen(cache_path, "rb");
  if (cache_f) {
    uint16_t header[2] = {0, 0};
    if (std::fread(header, 2, 2, cache_f) == 2) {
      uint16_t cached_w = header[0];
      uint16_t cached_h = header[1];
      uint16_t row_bytes = (cached_w + 7) / 8;
      std::vector<uint8_t> row_buf(row_bytes);
      for (uint16_t r = 0; r < cached_h; ++r) {
        if (std::fread(row_buf.data(), 1, row_bytes, cache_f) != row_bytes)
          break;
        if (r < src_y)
          continue;
        uint16_t dest_row = static_cast<uint16_t>(r - src_y);
        if (clip_h > 0 && dest_row >= clip_h)
          break;
        buf.blit_1bit_row(dest_x, dest_y + dest_row, row_buf.data(), cached_w);
      }
      std::fclose(cache_f);
      return true;
    }
    std::fclose(cache_f);
  }

  // v12: the image bytes live in the MRB, stored verbatim. Describe them as an
  // uncompressed ("stored") ZIP entry so the existing decoder can read them
  // straight out of the file — no ZIP central directory walk, no inflate pass,
  // and no EPUB. `offset` is the byte offset of the blob within the MRB and
  // `size` its length, both from the image ref table.
  StdioZipFile file;
  if (!file.open(mrb_path_.c_str()))
    return false;
  ZipEntry entry;
  entry.compression = 0;
  entry.data_offset = offset;
  entry.compressed_size = size;
  entry.uncompressed_size = size;
  // Name is left empty: the decoder falls back to sniffing the JPEG/PNG magic
  // bytes, which is what we want for data that no longer has a filename.

  FILE* cache_w = std::fopen(cache_path, "wb");
  if (cache_w) {
    uint16_t dummy[2] = {0, 0};
    std::fwrite(dummy, 2, 2, cache_w);
  }

  // Set up a sink that blits each dithered row directly to the DrawBuffer.
  struct BlitCtx {
    DrawBuffer* buf;
    int x, y;
    uint16_t src_y;
    uint16_t clip_h;  // max rows to render (0 = no clip)
    FILE* cache_w;
    uint16_t out_w;
    uint16_t out_h;
  };
  BlitCtx ctx{&buf, dest_x, dest_y, src_y, clip_h, cache_w, 0, 0};
  ImageRowSink sink;
  sink.ctx = &ctx;
  sink.emit_row = [](void* c, uint16_t row, const uint8_t* data, uint16_t width) {
    auto* bc = static_cast<BlitCtx*>(c);
    bc->out_w = width;
    if (row >= bc->out_h)
      bc->out_h = static_cast<uint16_t>(row + 1);

    if (bc->cache_w) {
      uint16_t row_bytes = static_cast<uint16_t>((width + 7) / 8);
      std::fwrite(data, 1, row_bytes, bc->cache_w);
    }

    if (row < bc->src_y)
      return;
    uint16_t dest_row = static_cast<uint16_t>(row - bc->src_y);
    if (bc->clip_h > 0 && dest_row >= bc->clip_h)
      return;
    bc->buf->blit_1bit_row(bc->x, bc->y + dest_row, data, width);
  };

  // Pixel sink for Adam7 interlaced PNGs: writes pixels directly to the
  // DrawBuffer with no intermediate output buffer.  Caching is skipped for
  // Adam7 (pixels arrive out of order so we can't write a sequential cache).
  struct PixelCtx {
    DrawBuffer* buf;
    int x, y;
    uint16_t src_y;
    uint16_t clip_h;
    uint16_t out_w;
    uint16_t out_h;
  };
  PixelCtx pctx{&buf, dest_x, dest_y, src_y, clip_h, 0, 0};
  ImagePixelSink psink;
  psink.ctx = &pctx;
  psink.set_pixel = [](void* c, uint16_t px, uint16_t py, bool white) {
    auto* pc = static_cast<PixelCtx*>(c);
    if (px >= pc->out_w)
      pc->out_w = static_cast<uint16_t>(px + 1);
    if (py >= pc->out_h)
      pc->out_h = static_cast<uint16_t>(py + 1);
    if (py < pc->src_y)
      return;
    uint16_t dest_row = static_cast<uint16_t>(py - pc->src_y);
    if (pc->clip_h > 0 && dest_row >= pc->clip_h)
      return;
    pc->buf->set_pixel(pc->x + static_cast<int>(px), pc->y + static_cast<int>(dest_row), white);
  };

  // Use the active display buffer as the work buffer to avoid a 44KB heap
  // allocation.  The active buffer is safe to overwrite here: it is not
  // needed for this render pass and will be cleared before the next refresh.
  DecodedImage dims;  // only width/height will be set; data stays empty
  auto err = decode_image_from_entry(file, entry, max_w, max_h, dims, buf.scratch_buf2(), DrawBuffer::kBufSize,
                                     /*scale_to_fill=*/true, &sink, &psink);

  if (cache_w) {
    if (err == ImageError::Ok && ctx.out_w > 0 && ctx.out_h > 0) {
      std::fseek(cache_w, 0, SEEK_SET);
      uint16_t header[2] = {ctx.out_w, ctx.out_h};
      std::fwrite(header, 2, 2, cache_w);
    }
    std::fclose(cache_w);
    // Delete cache file if decode failed, or if Adam7 pixel_sink was used
    // (pixels written directly to DrawBuffer, no sequential cache data written).
    if (err != ImageError::Ok || ctx.out_w == 0) {
#ifdef ESP_PLATFORM
      unlink(cache_path);
#else
      std::remove(cache_path);
#endif
    }
  }

  return err == ImageError::Ok;
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Bookmark / key-file helpers (must precede start())
// ---------------------------------------------------------------------------

// FNV-1a 32-bit hash of an arbitrary byte string.
static uint32_t fnv1a_32(const std::string& s) {
  uint32_t h = 2166136261u;
  for (unsigned char c : s)
    h = (h ^ c) * 16777619u;
  return h;
}

// Build a stable, filesystem-safe book key from raw metadata.
// Always produces an 8-char lowercase hex string derived from a hash of
// title + author + language — works for any language/script.
static std::string make_book_key(const EpubMetadata& meta) {
  std::string raw = meta.title;
  if (meta.author && !meta.author->empty()) {
    raw += '|';
    raw += *meta.author;
  }
  if (meta.language && !meta.language->empty()) {
    raw += '|';
    raw += *meta.language;
  }
  char hex[9];
  snprintf(hex, sizeof(hex), "%08lx", static_cast<unsigned long>(fnv1a_32(raw)));
  return std::string(hex);
}

void ReaderScreen::start(DrawBuffer& buf, IRuntime& runtime) {
  buf_ = &buf;
  if (app_)
    buf.set_rotation(rotation_from_setting(app_->rotate_reader()));
  book_key_.clear();
  pos_path_.clear();
  MR_LOGI("reader", "start: path='%s'", path_.c_str());

  if (app_ && app_->font_manager())
    app_->font_manager()->ensure_ready(buf);
  MR_LOGI("reader", "font ready");

  // An .mrb path *is* the book — converted on a computer and copied over, so it
  // is opened in place and never converted. An .epub is instead a source whose
  // MRB is cached at <data_dir>/cache/<stem>/book.mrb.
  const bool is_mrb = BookIndex::is_mrb_path(path_.c_str());
  if (is_mrb) {
    mrb_path_ = path_;
    const size_t slash = path_.find_last_of('/');
    book_cache_dir_ = (slash == std::string::npos) ? data_dir_ : path_.substr(0, slash);
  } else {
    // cache/ is created here rather than at boot: a library of converted books
    // never needs it, and an empty directory on the card invites the question of
    // what it is for.
    book_cache_dir_ = data_dir_ + "/cache/" + book_stem_();
#ifdef ESP_PLATFORM
    mkdir((data_dir_ + "/cache").c_str(), 0775);
    mkdir(book_cache_dir_.c_str(), 0775);
#else
    std::filesystem::create_directories(book_cache_dir_);
#endif
    mrb_path_ = book_cache_dir_ + "/book.mrb";
  }

  MR_LOGI("reader", "mrb_path='%s'", mrb_path_.c_str());
  bool mrb_ok = mrb_.open(mrb_path_.c_str());
  MR_LOGI("reader", "mrb_ok=%d", (int)mrb_ok);

  buf_was_touched_ = false;

  // There is no on-device conversion. A book is a .mrb produced by
  // tools/epub2mrb; if it will not open there is nothing to fall back to, so
  // say so rather than grinding through an EPUB parse the device no longer has
  // the code for.
  if (!mrb_ok) {
    MR_LOGI("reader", "mrb open failed: '%s'", mrb_path_.c_str());
    open_ok_ = false;
    goto show_error;
  }

  // Derive a stable book key from MRB metadata (title + author).
  // This survives epub file renames while staying unique across different books.
  book_key_ = make_book_key(mrb_.metadata());
  pos_path_ = std::string(data_dir_) + "/" + book_key_ + ".pos";

  open_ok_ = true;
  chapter_idx_ = 0;
  page_pos_ = PagePosition{0, 0};
  image_size_fn_ = make_image_size_query(mrb_, mrb_path_, static_cast<uint16_t>(buf.width()));
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
  layout_engine_.set_hyphenation_lang(detect_language(mrb_.metadata().language));
  render_page_(buf);
#ifdef ESP_PLATFORM
  ESP_LOGI("reader", "BOOK_OK: %s", path_.c_str());
#endif
  return;

show_error:
#ifdef ESP_PLATFORM
  ESP_LOGE("reader", "BOOK_FAIL: %s", path_.c_str());
#endif
  if (buf_was_touched_) {
    buf.fill(true);
    buf.draw_text(kPaddingLeft, kPaddingTop, "Failed to open book", true, kScale);
  }
}

void ReaderScreen::pause() {}

void ReaderScreen::resume(DrawBuffer& buf, IRuntime& runtime) {
  buf_ = &buf;
  if (app_)
    buf.set_rotation(rotation_from_setting(app_->rotate_reader()));
  if (!open_ok_)
    return;

  // Handle pending chapter jump (chosen from the in-book quick menu).
  if (app_ && app_->reader_options()->has_pending()) {
    saved_chapter_idx_ = app_->reader_options()->pending_chapter();
    saved_page_pos_ = PagePosition{app_->reader_options()->pending_para_index(), 0, 0};
    app_->reader_options()->clear_pending();
    load_chapter_(saved_chapter_idx_);
    page_pos_ = saved_page_pos_;
    layout_engine_.set_source(*chapter_src_);
    layout_engine_.set_image_size_fn(image_size_fn_);
    layout_engine_.set_hyphenation_lang(detect_language(mrb_.metadata().language));
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
  if (open_ok_)
    save_position_();
  image_size_fn_ = {};
  chapter_src_.reset();
  mrb_.close();
  page_ = PageContent{};
  mrb_path_.clear();
  mrb_path_.shrink_to_fit();
  pos_path_.clear();
  pos_path_.shrink_to_fit();
  book_key_.clear();
  book_key_.shrink_to_fit();
  open_ok_ = false;
  // Restore the global menu rotation before handing the buffer back.
  if (buf_ && app_)
    buf_->set_rotation(rotation_from_setting(app_->rotate_display()));
  // saved_chapter_idx_ / saved_page_pos_ are intentionally NOT reset here —
  // resume() uses them as the nav-history origin when a link jump is pending.
  buf_ = nullptr;
}

void ReaderScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& /*runtime*/) {
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
  bool had_next_press = false;
  bool had_prev_press = false;

  // Side rocker: the lower button always advances, in every orientation. This
  // started out orientation-dependent, but both holds turned out to want the
  // same thing on hardware, so the branch is gone rather than left as a no-op.
  const bool inv_side = !(app_ && app_->invert_side_buttons());
  const bool inv_bottom = app_ && app_->invert_bottom_paging();

  // Default: Button3=next, Button2=prev (inv_bottom=true flips front buttons).
  // Side: Down=next, Up=prev.
  Button logical_next_front = inv_bottom ? Button::Button2 : Button::Button3;
  Button logical_prev_front = inv_bottom ? Button::Button3 : Button::Button2;
  Button logical_next_side = inv_side ? Button::Down : Button::Up;
  Button logical_prev_side = inv_side ? Button::Up : Button::Down;

  Button btn;
  while (buttons.next_press(btn)) {
    if (btn == logical_next_front || btn == logical_next_side) {
      ++page_delta;
      had_next_press = true;
    } else if (btn == logical_prev_front || btn == logical_prev_side) {
      --page_delta;
      had_prev_press = true;
    } else {
      switch (btn) {
        case Button::Button0:
          app_->pop_screen();
          return;
        case Button::Button1:
          saved_chapter_idx_ = chapter_idx_;
          saved_page_pos_ = page_pos_;
          app_->reader_options()->set_settings(&reader_settings_);
          app_->reader_options()->populate(mrb_.toc(), static_cast<uint16_t>(chapter_idx_), page_pos_.paragraph,
                                           mrb_.metadata().title, progress_pct(), chapter_progress_pct(),
                                           mrb_.chapter_count());
          app_->push_screen(ScreenId::ReaderOptions);
          return;
        default:
          break;
      }
    }
  }

  // Hold-down: advance one page per frame while a nav button is held,
  // but only if no fresh press event arrived this frame (avoids double-counting
  // the initial press).
  if (!had_next_press && (buttons.is_down(logical_next_front) || buttons.is_down(logical_next_side)))
    ++page_delta;
  if (!had_prev_press && (buttons.is_down(logical_prev_front) || buttons.is_down(logical_prev_side)))
    --page_delta;

  bool changed = false;
  if (page_delta > 0) {
    for (int i = 0; i < page_delta; ++i)
      changed = next_page_() || changed;
  } else if (page_delta < 0) {
    for (int i = 0; i > page_delta; --i)
      changed = prev_page_() || changed;
  }

  if (changed) {
    render_page_(buf);
    buf.refresh();
  }
}

void ReaderScreen::load_chapter_(size_t idx) {
  chapter_src_.reset();
  if (idx < mrb_.chapter_count()) {
    chapter_src_ = std::make_unique<MrbChapterSource>(mrb_, static_cast<uint16_t>(idx));
    chapter_idx_ = idx;
    layout_engine_.set_source(*chapter_src_);
  }
}

void ReaderScreen::render_page_(DrawBuffer& buf) {
  const int W = buf.width();
  const int H = buf.height();

#ifdef ESP_PLATFORM
  int64_t t0 = esp_timer_get_time();
#endif

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
  page_pos_ = layout_engine_.resolve_stable_position(page_pos_);
  layout_engine_.set_position(page_pos_);

#ifdef ESP_PLATFORM
  int64_t t_layout = esp_timer_get_time();
#endif
  page_ = layout_engine_.layout();
#ifdef ESP_PLATFORM
  long layout_us = (long)(esp_timer_get_time() - t_layout);
#endif
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
    if (img_item.key >= mrb_.image_count())
      return;
    ImageToDraw itd;
    itd.key = img_item.key;
    itd.x = static_cast<int>(img_item.x_offset);
    itd.y = static_cast<int>(img_item.y_offset);  // y_offset is absolute (vertical centering baked in)
    itd.w = img_w;
    // Use full_height as max_h so the decoder scales to the correct aspect ratio;
    // src_y crops to the visible slice within that full render.
    itd.h = img_item.full_height > 0 ? static_cast<int>(img_item.full_height) : img_h;
    itd.offset = mrb_.image_ref(img_item.key).data_offset;
    itd.size = mrb_.image_ref(img_item.key).data_size;
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
#ifdef ESP_PLATFORM
  int64_t t_draw = esp_timer_get_time();
#endif
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
    if (!decode_image_to_buffer_(itd.key, itd.offset, itd.size, buf, itd.x, itd.y, static_cast<uint16_t>(itd.w),
                                 static_cast<uint16_t>(itd.h), itd.src_y, itd.clip_h)) {
      // Leave the space blank rather than filling it black. A converted book has
      // no EPUB to decode from, so this fires for every inline image until the
      // image data is embedded in the MRB — a black slab per figure is far worse
      // than white space.
      buf.fill_rect(itd.x, itd.y, itd.w, itd.h, true);
    }
  }


  // ── Timing
  // ──────────────────────────────────────────────────────────────
  int n_words = 0;
  for (const auto& ci : page_.items)
    if (const PageTextItem* ti = std::get_if<PageTextItem>(&ci))
      n_words += static_cast<int>(ti->line.words.size());

#ifdef ESP_PLATFORM
  long render_us = (long)(esp_timer_get_time() - t0);
  long draw_us = (long)(esp_timer_get_time() - t_draw);
  ESP_LOGI("perf",
           "render_page: %ldms total (layout=%ldms[miss=%d para=%ldms hyph=%ldms metrics=%ldms] draw=%ldms) words=%d "
           "images=%d",
           render_us / 1000, layout_us / 1000, g_layout_cache_misses, (long)(g_layout_para_us / 1000),
           (long)(g_layout_hyph_us / 1000), (long)(g_layout_metrics_us / 1000), draw_us / 1000, n_words,
           (int)images.size());
#endif
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

void ReaderScreen::bench_render(DrawBuffer& buf, int iterations) {
#ifdef ESP_PLATFORM
  if (!open_ok_) {
    ESP_LOGW("bench", "bench_render: no book open");
    return;
  }
  // Navigate to the start of the book.
  load_chapter_(0);
  page_pos_ = PagePosition{0, 0};

  const int n = (iterations > 200) ? 200 : (iterations < 1 ? 1 : iterations);
  long total_ms = 0;
  long min_ms = INT32_MAX, max_ms = 0;
  int pages_done = 0;

  for (int i = 0; i < n; ++i) {
    int64_t t0 = esp_timer_get_time();
    render_page_(buf);
    long ms = (long)((esp_timer_get_time() - t0) / 1000);
    int word_count = 0;
    for (const auto& ci : page_.items)
      if (const PageTextItem* ti = std::get_if<PageTextItem>(&ci))
        word_count += (int)ti->line.words.size();
    ESP_LOGI("bench", "page[%d/%d]: %ldms words=%d", i + 1, n, ms, word_count);
    total_ms += ms;
    if (ms < min_ms)
      min_ms = ms;
    if (ms > max_ms)
      max_ms = ms;
    ++pages_done;
    if (page_.at_chapter_end && chapter_idx_ + 1 >= mrb_.chapter_count())
      break;
    next_page_();
  }

  long avg_ms = pages_done > 0 ? total_ms / pages_done : 0;
  ESP_LOGI("bench", "RENDER_BENCH:pages=%d,min=%ldms,max=%ldms,avg=%ldms,total=%ldms", pages_done, min_ms, max_ms,
           avg_ms, total_ms);
#endif
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
    if (chapter_idx_ + 1 < mrb_.chapter_count()) {
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
      return true;
    }
    return false;
  }

  // If the current page starts mid-image, snap end to the bottom of that image
  // so layout_backward produces the page ending at the image bottom — which
  // naturally includes the full image (rows 0..end) plus whatever text fits above.
  PagePosition end = layout_engine_.snap_to_image_end(page_pos_);
  layout_engine_.set_position(end);
  auto pc = layout_engine_.layout_backward();
  page_pos_ = pc.start;
  return true;
}

// ---------------------------------------------------------------------------
// Bookmark persistence
// ---------------------------------------------------------------------------

void ReaderScreen::save_position_() {
  if (pos_path_.empty())
    return;
  FILE* f = std::fopen(pos_path_.c_str(), "w");
  if (!f)
    return;
  // Position only: chapter, paragraph, offset, text offset. Reading statistics
  // are not tracked at all, so nothing else needs persisting.
  std::fprintf(f, "%u %u %u %u\n", static_cast<unsigned>(chapter_idx_),
               static_cast<unsigned>(page_pos_.paragraph), static_cast<unsigned>(page_pos_.offset),
               static_cast<unsigned>(page_pos_.text_offset));
  std::fclose(f);
}

void ReaderScreen::load_position_() {
  if (pos_path_.empty())
    return;

  FILE* f = std::fopen(pos_path_.c_str(), "r");
  bool migrating = false;

  // One-time migration from the old <data_dir>/data/ location.
  if (!f) {
    const std::string old_path = std::string(data_dir_) + "/data/" + book_key_ + ".pos";
    f = std::fopen(old_path.c_str(), "r");
    if (f) {
      migrating = true;
      MR_LOGI("reader", "Migrating pos file: '%s' -> '%s'", old_path.c_str(), pos_path_.c_str());
#ifdef ESP_PLATFORM
      unlink(old_path.c_str());
#else
      std::remove(old_path.c_str());
#endif
    }
  }

  if (!f)
    return;
  unsigned ch = 0, para = 0, line = 0, to = 0;
  // Older files appended reading statistics after the position; those fields no
  // longer exist and are simply not consumed, so a short read is expected.
  int scanned = std::fscanf(f, "%u %u %u %u", &ch, &para, &line, &to);
  std::fclose(f);
  if (scanned >= 3) {
    saved_chapter_idx_ = ch;
    saved_page_pos_ = PagePosition{static_cast<uint16_t>(para), static_cast<uint16_t>(line), static_cast<uint32_t>(to)};
    MR_LOGI("reader", "Loaded pos ch=%u para=%u line=%u to=%u (scanned=%d)", ch, para, line, to, scanned);
    if (migrating) {
      FILE* fw = std::fopen(pos_path_.c_str(), "w");
      if (fw) {
        std::fprintf(fw, "%u %u %u %u\n", ch, para, line, to);
        std::fclose(fw);
      }
    }
  }
}

}  // namespace wintergreen
