#include "MainMenu.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <queue>

#include "../Application.h"
#include "../content/BookIndex.h"
#include "../content/mrb/MrbReader.h"

#ifdef ESP_PLATFORM
#include <dirent.h>
#else
#include <filesystem>
namespace fs = std::filesystem;
#endif

namespace wintergreen {

static bool ci_less(std::string_view a, std::string_view b) {
  size_t min_len = std::min(a.size(), b.size());
#ifdef _WIN32
  int cmp = _strnicmp(a.data(), b.data(), min_len);
#else
  int cmp = strncasecmp(a.data(), b.data(), min_len);
#endif
  if (cmp != 0) return cmp < 0;
  return a.size() < b.size();
}

void MainMenu::on_start() {
  title_ = "wintergreen";

  if (!app_->data_dir_) {
    needs_scan_ = false;
    return;
  }

  std::string index_path = std::string(app_->data_dir_) + "/book_index.dat";

  if (show_hidden_ && hidden_.empty())
    scan_hidden_();

  const bool loaded = BookIndex::instance().load(index_path);
  if (loaded && !BookIndex::instance().entries().empty()) {
    populate_list_();
    needs_scan_ = false;
  } else {
    needs_scan_ = true;
  }
  cached_generation_ = BookIndex::instance().generation();
}

void MainMenu::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  // Detect external mutations (serial upload/delete/rename) while this screen
  // is visible. The generation counter is bumped by BookIndex on every
  // mutation that changes the logical contents.
  if (cached_generation_ != BookIndex::instance().generation()) {
    cached_generation_ = BookIndex::instance().generation();
    populate_list_();
    draw_all_(buf, runtime.battery_percentage());
    buf.full_refresh();
  }

  if (needs_scan_) {
    needs_scan_ = false;
    scan_directory_(buf);
    populate_list_();

    draw_all_(buf, runtime.battery_percentage());
    buf.full_refresh();
    cached_generation_ = BookIndex::instance().generation();
  }

  ListMenuScreen::update(buttons, buf, runtime);
}

void MainMenu::on_select(int index) {
  if (is_separator(index)) return;
  const int real = entries_index_for(index);
  if (real < 0 || real >= static_cast<int>(entries_.size())) return;
  const BookEntry& e = entries_[real];
  // A hidden book is never recorded: mark_opened() would do nothing (it is not
  // indexed) and the index save would be pointless, but more to the point a
  // hidden book must not surface in the recents carousel.
  if (!e.hidden)
    app_->record_book_opened(e.path);
  app_->reader()->set_path(e.path.c_str());
  app_->push_screen(ScreenId::Reader);
}

void MainMenu::stop() {
  const std::string& cur = current_book_path();
  if (!cur.empty())
    initial_selection_ = cur;

  // Revealing hidden books lasts only as long as the screen does — leaving and
  // coming back means performing the gesture again.
  show_hidden_ = false;
  { std::vector<BookEntry> tmp; hidden_.swap(tmp); }
  { std::vector<BookEntry> tmp; entries_.swap(tmp); }
  free_items_storage();
  BookIndex::instance().clear_entries();
}

void MainMenu::on_back() {
  app_->pop_screen();
}

void MainMenu::scan_directory_(DrawBuffer& buf) {
  if (!books_dir_ || !app_->data_dir_)
    return;

  std::string root_dir = books_dir_;
  const std::string index_path = std::string(app_->data_dir_) + "/book_index.dat";

  buf.sync_bw_ram();

  BookIndex::instance().build_index(root_dir, buf);
  BookIndex::instance().save(index_path);

  buf.reset_after_scratch(true);
}

// Name of the directory containing `path` — the title fallback for a book whose
// MRB metadata is empty, matching what BookIndex does for visible books.
static std::string folder_name_of(const std::string& path) {
  const size_t last = path.find_last_of('/');
  if (last == std::string::npos || last == 0)
    return path;
  const size_t prev = path.find_last_of('/', last - 1);
  const size_t start = prev == std::string::npos ? 0 : prev + 1;
  return path.substr(start, last - start);
}

void MainMenu::scan_hidden_() {
  hidden_.clear();
  if (!books_dir_)
    return;

  std::vector<std::string> paths;
  std::queue<std::string> q;
  q.push(std::string(books_dir_) + "/.hidden");
  while (!q.empty()) {
    const std::string dir = std::move(q.front());
    q.pop();
#ifdef ESP_PLATFORM
    DIR* d = opendir(dir.c_str());
    if (!d) continue;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
      if (ent->d_name[0] == '.') continue;  // only the .hidden root itself is a dot dir
      const std::string full = dir + "/" + ent->d_name;
      if (ent->d_type == DT_DIR)
        q.push(full);
      else if (BookIndex::is_mrb_path(full.c_str()))
        paths.push_back(full);
    }
    closedir(d);
#else
    try {
      for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied)) {
        std::string full = e.path().string();
        for (char& c : full) if (c == '\\') c = '/';
        if (e.path().filename().string()[0] == '.') continue;
        if (e.is_directory())
          q.push(full);
        else if (BookIndex::is_mrb_path(full.c_str()))
          paths.push_back(full);
      }
    } catch (...) {}
#endif
  }

  for (auto& p : paths) {
    BookEntry e;
    MrbReader r;
    if (!r.open(p.c_str()))
      continue;
    e.title_own = r.metadata().title;
    e.author_own = r.metadata().author.value_or("");
    r.close();
    if (e.title_own.empty() || e.title_own == "none")
      e.title_own = folder_name_of(p);
    e.path = std::move(p);
    e.hidden = true;
    hidden_.push_back(std::move(e));
  }

  std::sort(hidden_.begin(), hidden_.end(), [](const BookEntry& a, const BookEntry& b) {
    return ci_less(a.title_own, b.title_own);
  });
}

int MainMenu::count() const {
  return static_cast<int>(entries_.size()) + static_cast<int>(separators_.size());
}

bool MainMenu::is_separator(int index) const {
  for (const auto& s : separators_)
    if (s.first == index) return true;
  return false;
}

std::string_view MainMenu::get_item_label(int index) const {
  if (is_separator(index)) {
    for (const auto& s : separators_)
      if (s.first == index) return s.second;
    return {};
  }
  int real = entries_index_for(index);
  if (real < 0 || real >= static_cast<int>(entries_.size()))
    return {};
  const StringPool& pool = BookIndex::instance().pool();
  const BookEntry& e = entries_[real];
  // Title only — every book on the card is a converted MRB, so the old trailing
  // middle dot marked every single row and said nothing.
  return e.hidden ? std::string_view(e.title_own) : e.title_ref.view(pool);
}

// Reading percentage, drawn at the bottom right of the row beside the author.
// Blank for a book that has never been opened, and for hidden books, which are
// never recorded in the index.
std::string_view MainMenu::get_item_right(int index) const {
  if (is_separator(index)) return {};
  const int real = entries_index_for(index);
  if (real < 0 || real >= static_cast<int>(entries_.size())) return {};
  const BookEntry& e = entries_[real];
  if (e.hidden || e.last_open_order == 0) return {};
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%u%%", static_cast<unsigned>(e.progress_pct));
  right_buf_ = buf;
  return right_buf_;
}

std::string_view MainMenu::get_item_subtitle(int index) const {
  if (is_separator(index)) return {};
  int real = entries_index_for(index);
  if (real < 0 || real >= static_cast<int>(entries_.size())) return {};
  const BookEntry& e = entries_[real];
  if (e.hidden) {
    subtitle_buf_ = e.author_own;
    return subtitle_buf_;
  }
  const StringPool& pool = BookIndex::instance().pool();

  subtitle_buf_ = e.author_ref.view(pool);
  return subtitle_buf_;
}

std::string MainMenu::wintergreen_header_left() const {
  const int n = static_cast<int>(entries_.size());
  char buf[24];
  std::snprintf(buf, sizeof(buf), n == 1 ? "1 BOOK" : "%d BOOKS", n);
  return buf;
}

void MainMenu::populate_list_() {
  clear_items();
  entries_.clear();
  separators_.clear();

  const StringPool& bpool = BookIndex::instance().pool();
  detail_list_ = true;
  for (const auto& idx : BookIndex::instance().entries()) {
    BookEntry e;
    e.path = idx.path.to_string(bpool);
    e.title_ref = idx.title;
    e.author_ref = idx.author;
    e.last_open_order = idx.last_open_order;
    e.progress_pct = idx.progress_pct;
    entries_.push_back(std::move(e));
  }

  // Most recently opened first, then never-opened books alphabetically, with a
  // hairline divider between the two groups.
  std::stable_sort(entries_.begin(), entries_.end(),
                   [&bpool](const BookEntry& a, const BookEntry& b) {
                     if (a.last_open_order != b.last_open_order)
                       return a.last_open_order > b.last_open_order;
                     return ci_less(a.title_ref.view(bpool), b.title_ref.view(bpool));
                   });
  int split = static_cast<int>(entries_.size());
  for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
    if (entries_[i].last_open_order == 0) { split = i; break; }
  }

  // Hidden books go above everything, behind their own divider. They are
  // inserted after the sort so they keep their own alphabetical order and are
  // never mixed into the recently-opened group.
  const int n_hidden = show_hidden_ ? static_cast<int>(hidden_.size()) : 0;
  if (n_hidden > 0) {
    entries_.insert(entries_.begin(), hidden_.begin(), hidden_.end());
    split += n_hidden;
  }

  // Separator positions are *visual* indices, so a separator shifts every one
  // below it by a row.
  if (n_hidden > 0 && n_hidden < static_cast<int>(entries_.size()))
    separators_.push_back({n_hidden, ""});
  if (split > n_hidden && split < static_cast<int>(entries_.size()))
    separators_.push_back({split + (n_hidden > 0 ? 1 : 0), ""});

  if (!initial_selection_.empty()) {
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
      if (entries_[i].path == initial_selection_) {
        set_selected(visual_for_entries(i));
        break;
      }
    }
    initial_selection_.clear();
  }
}


}  // namespace wintergreen
