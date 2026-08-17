#include "ReaderOptionsScreen.h"

#include <climits>
#include <cstdio>
#include <cstring>

#include "../Application.h"
#include "../content/BookIndex.h"

namespace wintergreen {
constexpr uint8_t ReaderSettings::kNumFontSizePresets;
constexpr const char* ReaderSettings::kFontSizeNames[];

// ---------------------------------------------------------------------------

void ReaderOptionsScreen::populate(const TableOfContents& toc, uint16_t current_chapter, uint16_t current_para,
                                   const std::string& fallback_title, int book_progress_pct, int chapter_progress_pct,
                                   uint16_t chapter_count) {
  toc_ = &toc;
  chapter_count_ = chapter_count;
  book_title_ = fallback_title;
  chapter_title_ = fallback_title;
  int best_match = -1;
  for (size_t i = 0; i < toc_->entries.size(); ++i) {
    if (toc_->entries[i].file_idx < current_chapter ||
        (toc_->entries[i].file_idx == current_chapter && toc_->entries[i].para_index <= current_para)) {
      best_match = static_cast<int>(i);
    }
  }
  if (best_match >= 0) {
    chapter_title_ = toc_->entries[best_match].label.to_string(toc_->pool);
  }

  book_progress_pct_ = book_progress_pct;
  chapter_progress_pct_ = chapter_progress_pct;
  has_pending_ = false;
}

// Build a "Label: Value" string into a fixed buffer.
static const char* fmt_setting(char* buf, size_t bufsz, const char* label, const char* value) {
  snprintf(buf, bufsz, "%s: %s", label, value);
  return buf;
}

std::string_view ReaderOptionsScreen::get_item_subtitle(int index) const {
  std::string_view label = ListMenuScreen::get_item_label(index);
  const auto pos = label.find(": ");
  if (pos == std::string_view::npos) return {};
  subtitle_buf_ = label.substr(pos + 2);
  return subtitle_buf_;
}

std::string ReaderOptionsScreen::wintergreen_header_left() const {
  return "reading";
}

void ReaderOptionsScreen::start(DrawBuffer& buf, IRuntime& runtime) {
  buf_ = &buf;
  // Capture current selection before the base class calls on_start(), which
  // rebuilds the list.
  prev_selected_ = selected_index();
  ListMenuScreen::start(buf, runtime);
}

void ReaderOptionsScreen::on_start() {
  // Use the list/menu rotation rather than inheriting whatever rotation the
  // reader view was using.
  if (app_)
    set_buf_rotation_(rotation_from_setting(app_->rotate_display()));

  title2_ = nullptr;
  book_title1_buf_.clear();

  if (header_font_.valid()) {
    const int max_line_w = buf_->width() - 24;
    const int total_w = header_font_.word_width(book_title_.c_str(), book_title_.length(), FontStyle::Regular);
    if (total_w <= max_line_w) {
      title_ = book_title_.c_str();
    } else {
      std::vector<std::string> words;
      std::string w;
      for (size_t i = 0; i <= book_title_.length(); ++i) {
        if (i == book_title_.length() || book_title_[i] == ' ') {
          if (!w.empty())
            words.push_back(w);
          w.clear();
        } else {
          w += book_title_[i];
        }
      }

      std::vector<int> word_w;
      word_w.reserve(words.size());
      int all_w = 0;
      for (const auto& word : words) {
        int ww = static_cast<int>(header_font_.word_width(word.c_str(), word.length(), FontStyle::Regular));
        word_w.push_back(ww);
        all_w += ww;
      }
      int spaces = static_cast<int>(words.size()) - 1;
      if (spaces > 0)
        all_w += spaces * static_cast<int>(header_font_.word_width(" ", 1, FontStyle::Regular));

      if (all_w <= max_line_w) {
        title_ = book_title_.c_str();
      } else {
        int space_w = static_cast<int>(header_font_.word_width(" ", 1, FontStyle::Regular));
        int ellipsis_w = static_cast<int>(header_font_.word_width("...", 3, FontStyle::Regular));

        int best_split = 1;
        int best_diff = INT_MAX;
        int l1_w = 0;
        for (size_t i = 0; i < words.size(); ++i) {
          if (i > 0)
            l1_w += space_w;
          l1_w += word_w[i];
          if (l1_w > max_line_w)
            break;
          if (i + 1 < words.size()) {
            int l2_w = 0;
            for (size_t j = i + 1; j < words.size(); ++j) {
              if (j > i + 1)
                l2_w += space_w;
              l2_w += word_w[j];
            }
            int diff = std::abs(l1_w - l2_w);
            if (diff < best_diff) {
              best_diff = diff;
              best_split = static_cast<int>(i) + 1;
            }
          }
        }

        std::string line1, line2;
        for (int i = 0; i < best_split; ++i) {
          if (i > 0)
            line1 += ' ';
          line1 += words[i];
        }
        for (int i = best_split; i < static_cast<int>(words.size()); ++i) {
          if (i > best_split)
            line2 += ' ';
          line2 += words[i];
        }

        if (header_font_.word_width(line2.c_str(), line2.length(), FontStyle::Regular) <= max_line_w) {
          book_title2_buf_ = line2;
          title2_ = book_title2_buf_.c_str();
        } else {
          int l2_w = 0;
          int trunc_at = 0;
          for (int i = best_split; i < static_cast<int>(words.size()); ++i) {
            int word_with_space = word_w[i] + ((i > best_split) ? space_w : 0);
            if (l2_w + word_with_space + ellipsis_w > max_line_w)
              break;
            l2_w += word_with_space;
            trunc_at = i + 1;
          }
          line2.clear();
          for (int i = best_split; i < trunc_at; ++i) {
            if (i > best_split)
              line2 += ' ';
            line2 += words[i];
          }
          line2 += "...";
          book_title2_buf_ = line2;
          title2_ = book_title2_buf_.c_str();
        }

        book_title1_buf_ = line1;
        title_ = book_title1_buf_.c_str();
      }
    }
  } else {
    if (book_title_.length() > 30) {
      book_title1_buf_ = book_title_.substr(0, 27) + "...";
      title_ = book_title1_buf_.c_str();
    } else {
      title_ = book_title_.c_str();
    }
  }

  // Author for the details card. Reading statistics are not tracked.
  std::string_view author_sv;
  if (app_ && app_->reader()) {
    const std::string cur_path = app_->reader()->get_path();
    for (const auto& e : BookIndex::instance().entries()) {
      if (e.path.view(BookIndex::instance().pool()) == cur_path) {
        author_sv = e.author.view(BookIndex::instance().pool());
        break;
      }
    }
  }

  subtitle_ = std::string(author_sv);
  char pct_buf[16];
  snprintf(pct_buf, sizeof(pct_buf), "Book %d%%", book_progress_pct_);
  subtitle2_ = pct_buf;

  clear_items();
  idx_font_size_ = idx_rotate_display_ = idx_reader_rotate_display_ = -1;
  first_chapter_ = -1;

  char tmp[40];

  // Flat list, no section headers: the two settings, then Chapters.
  if (settings_) {
    idx_font_size_ = count();
    if (app_ && app_->font_manager() && app_->font_manager()->valid()) {
      auto* fonts = app_->font_manager()->font_set();
      int sz = 0;
      if (fonts && settings_->font_size_idx < fonts->num_fonts()) {
        auto* f = fonts->get_font(settings_->font_size_idx);
        if (f) {
          sz = f->nominal_size();
          if (sz == 0)
            sz = f->y_advance();
        }
      }
      char val[16];
      if (sz > 0)
        snprintf(val, sizeof(val), "%d", sz);
      else
        snprintf(val, sizeof(val), "Unknown");
      add_item(fmt_setting(tmp, sizeof(tmp), "Font Size", val));
    } else {
      add_item(fmt_setting(tmp, sizeof(tmp), "Font Size", ReaderSettings::kFontSizeNames[settings_->font_size_idx]));
    }

    idx_reader_rotate_display_ = count();
    add_item(fmt_setting(tmp, sizeof(tmp), "Reader Display", rotation_label(app_ ? app_->rotate_reader() : 0)));
  }

  // Chapters, inline below the settings. Nested TOC entries keep their depth as
  // an indent; a book with no TOC falls back to numbered spine chapters.
  const bool has_toc = toc_ && !toc_->entries.empty();
  if (has_toc) {
    first_chapter_ = count();
    for (const auto& entry : toc_->entries)
      add_item_view(entry.label.view(toc_->pool), entry.depth);
  } else if (chapter_count_ > 1) {
    first_chapter_ = count();
    char cbuf[20];
    for (uint16_t i = 0; i < chapter_count_; ++i) {
      snprintf(cbuf, sizeof(cbuf), "Chapter %u", static_cast<unsigned>(i + 1));
      add_item(cbuf);
    }
  }

  // Restore selection. The item list is now fixed for a given book, so no
  // shift correction is needed.
  int sel = prev_selected_;
  int max_sel = count() - 1;
  if (max_sel >= 0) {
    if (sel < 0)
      sel = 0;
    if (sel > max_sel)
      sel = max_sel;
  }
  set_selected(sel);
}

void ReaderOptionsScreen::refresh_items_(int restore_selection) {
  prev_selected_ = restore_selection;
  on_start();  // on_start() calls set_selected().
}

int ReaderOptionsScreen::get_visible_count_(int H, int scroll_off) const {
  int list_top = 16;
  if (title_ && header_font_.valid()) {
    list_top += header_font_.y_advance();
    if (title2_) list_top += header_font_.y_advance();
  } else if (title_) {
    list_top += ui_font_.y_advance();
  }
  list_top += 4;
  if (!subtitle_.empty())      list_top += ui_font_.y_advance() + 3;
  if (!chapter_title_.empty()) list_top += ui_font_.y_advance() + 3;
  list_top += ui_font_.y_advance() + 10 + 1;  // stats row + rule
  const int available_h = H - list_top;
  int h = 0, cnt = 0;
  const int n = count();
  for (int i = scroll_off; i < n; ++i) {
    if (h + kRowH > available_h) break;
    h += kRowH;
    cnt++;
  }
  return cnt;
}

void ReaderOptionsScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct) const {
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);

  if (!ui_font_.valid() || !subtitle_font_.valid()) return;

  static constexpr int kLM = 14, kRM = 14;
  int y = 16;

  // ── Book title ──────────────────────────────────────────────────────────
  if (title_ && header_font_.valid()) {
    buf.draw_text_proportional(kLM, y + header_font_.baseline(), title_, header_font_, false);
    y += header_font_.y_advance();
    if (title2_) {
      buf.draw_text_proportional(kLM, y + header_font_.baseline(), title2_, header_font_, false);
      y += header_font_.y_advance();
    }
  } else if (title_) {
    buf.draw_text_proportional(kLM, y + ui_font_.baseline(), title_, ui_font_, false);
    y += ui_font_.y_advance();
  }
  y += 4;

  // ── Author ──────────────────────────────────────────────────────────────
  if (!subtitle_.empty()) {
    buf.draw_text_proportional(kLM, y + ui_font_.baseline(),
                               subtitle_.c_str(), subtitle_.size(), ui_font_, false);
    y += ui_font_.y_advance() + 3;
  }

  // ── Chapter name ────────────────────────────────────────────────────────
  if (!chapter_title_.empty()) {
    static const char kEll[] = "...";
    const int max_cw = W - kLM - kRM;
    const int ell_w = ui_font_.word_width(kEll, 3, FontStyle::Regular);
    const char* cp = chapter_title_.c_str();
    const size_t clen = chapter_title_.size();
    if (ui_font_.word_width(cp, clen, FontStyle::Regular) > max_cw) {
      const int budget = max_cw - ell_w;
      size_t fit = 0;
      const char* p = cp;
      while (*p) {
        const uint8_t b = static_cast<uint8_t>(*p);
        const size_t cb = b < 0x80 ? 1u : b < 0xE0 ? 2u : b < 0xF0 ? 3u : 4u;
        if (ui_font_.word_width(cp, fit + cb, FontStyle::Regular) > budget) break;
        fit += cb; p += cb;
      }
      subtitle_buf_.assign(cp, fit);
      subtitle_buf_ += kEll;
      buf.draw_text_proportional(kLM, y + ui_font_.baseline(),
                                 subtitle_buf_.c_str(), subtitle_buf_.size(), ui_font_, false);
    } else {
      buf.draw_text_proportional(kLM, y + ui_font_.baseline(), cp, clen, ui_font_, false);
    }
    y += ui_font_.y_advance() + 3;
  }

  // ── Stats: "Book X% · Chapter Y%" left, read time right ────────────────
  {
    char stats_l[48];
    snprintf(stats_l, sizeof(stats_l), "Book %d%%  \xc2\xb7  Chapter %d%%",
             book_progress_pct_, chapter_progress_pct_);
    buf.draw_text_proportional(kLM, y + ui_font_.baseline(),
                               stats_l, std::strlen(stats_l), ui_font_, false);
    y += ui_font_.y_advance() + 10;
  }

  buf.fill_rect(0, y, W, 1, false);
  y += 1;

  // ── Item list ────────────────────────────────────────────────────────────
  const int n = count();
  // Compute total content height to decide whether scrolling is needed.
  const int total_h = n * kRowH;
  const int so = (total_h <= H - y) ? 0 : scroll_offset();

  for (int i = so; i < n && y < H; ++i) {
    const bool sel = (i == selected());

    const std::string_view label = get_item_label(i);
    const bool is_chapter = (first_chapter_ >= 0 && i >= first_chapter_);

    // Only a settings row is "Label: Value". Chapter titles legitimately contain
    // ": " ("Chapter 1: The Beginning") and must never be split into columns.
    std::string_view display_label = label;
    std::string_view display_value;
    if (!is_chapter) {
      const auto pos = label.find(": ");
      if (pos != std::string_view::npos) {
        display_label = label.substr(0, pos);
        display_value = label.substr(pos + 2);
      }
    }

    if (sel)
      buf.fill_rect(0, y, W, kRowH, false);

    const int text_y = y + (kRowH - ui_font_.y_advance()) / 2 + ui_font_.baseline();
    const int text_x = kLM + (is_chapter ? get_item_indent(i) * 12 : 0);
    buf.draw_text_proportional(text_x, text_y, display_label.data(), display_label.size(), ui_font_, sel);

    if (!display_value.empty()) {
      const int vw = ui_font_.word_width(display_value.data(), display_value.size(), FontStyle::Regular);
      buf.draw_text_proportional(W - kRM - vw, text_y, display_value.data(), display_value.size(), ui_font_, sel);
    }

    y += kRowH;
  }
}

void ReaderOptionsScreen::on_select(int index) {
  if (first_chapter_ >= 0 && index >= first_chapter_) {
    const int ch = index - first_chapter_;
    if (toc_ && !toc_->entries.empty()) {
      pending_chapter_ = toc_->entries[ch].file_idx;
      pending_para_index_ = toc_->entries[ch].para_index;
    } else {
      pending_chapter_ = static_cast<uint16_t>(ch);
      pending_para_index_ = 0;
    }
    has_pending_ = true;
    app_->pop_screen();
    return;
  }

  if (!settings_)
    return;

  if (index == idx_font_size_) {
    uint8_t max_idx = ReaderSettings::kNumFontSizePresets;
    if (app_ && app_->font_manager() && app_->font_manager()->valid()) {
      auto* fonts = app_->font_manager()->font_set();
      if (fonts && fonts->num_fonts() > 0)
        max_idx = static_cast<uint8_t>(fonts->num_fonts());
    }
    settings_->font_size_idx = static_cast<uint8_t>((settings_->font_size_idx + 1) % max_idx);
    refresh_items_(index);
    return;
  }
  if (index == idx_reader_rotate_display_) {
    if (app_) {
      uint8_t v = static_cast<uint8_t>((app_->rotate_reader() + 1) % 4);
      app_->set_rotate_reader(v);
      refresh_items_(index);
    }
    return;
  }
  return;
}

}  // namespace wintergreen
