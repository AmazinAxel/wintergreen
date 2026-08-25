#include "MainMenu.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <queue>

#include "../Application.h"
#include "../content/BookIndex.h"
#include "../content/wgb/WgbReader.h"

#ifdef ESP_PLATFORM
#include <dirent.h>
#else
#include <filesystem>
namespace fs = std::filesystem;
#endif

namespace wintergreen {


// Stable merge sort over an index permutation.
//
// This exists instead of std::stable_sort because the standard algorithm is a
// template on both the iterator and the comparator, so every call site gets its
// own copy of introsort + insertion sort + __rotate + __merge_adaptive. Three
// call sites over two element types cost ~34 KB of flash — more than every
// screen's drawing code combined — to sort lists that are at most MAX_BOOKS
// long and are built once per screen entry.
//
// The comparator is a plain function pointer, so there is exactly one copy of
// the sort in the image no matter how many callers appear. Indices are sorted
// rather than elements, so BookEntry's four std::strings are never moved during
// the sort — only once, when the permutation is applied.
using IndexLess = bool (*)(uint16_t a, uint16_t b, const void* ctx);

static void merge_indices_(uint16_t* idx, uint16_t* tmp, int lo, int mid, int hi, IndexLess less, const void* ctx) {
  int i = lo, j = mid, k = lo;
  while (i < mid && j < hi)
    tmp[k++] = less(idx[j], idx[i], ctx) ? idx[j++] : idx[i++];  // `!less(a,b)` keeps it stable
  while (i < mid) tmp[k++] = idx[i++];
  while (j < hi) tmp[k++] = idx[j++];
  for (int t = lo; t < hi; ++t) idx[t] = tmp[t];
}

static void stable_sort_indices_(uint16_t* idx, int n, IndexLess less, const void* ctx) {
  if (n < 2) return;
  std::vector<uint16_t> scratch(static_cast<size_t>(n));
  for (int width = 1; width < n; width *= 2)
    for (int lo = 0; lo < n - width; lo += 2 * width)
      merge_indices_(idx, scratch.data(), lo, lo + width, std::min(lo + 2 * width, n), less, ctx);
}

// Reorder `v` in place to match `idx`. One tiny instantiation per element type;
// the sort itself above is shared.
template <class T>
static void apply_permutation_(std::vector<T>& v, const uint16_t* idx) {
  std::vector<T> out;
  out.reserve(v.size());
  for (size_t i = 0; i < v.size(); ++i)
    out.push_back(std::move(v[idx[i]]));
  v.swap(out);
}

// Identity permutation, sized to `n`.
static std::vector<uint16_t> iota_indices_(size_t n) {
  std::vector<uint16_t> idx(n);
  for (size_t i = 0; i < n; ++i)
    idx[i] = static_cast<uint16_t>(i);
  return idx;
}


void MainMenu::on_start() {
  title_ = "wintergreen";

  if (!app_->data_dir_) {
    needs_scan_ = false;
    return;
  }

  const std::string& index_path = app_->index_path();

  if (show_hidden_ && hidden_.empty())
    scan_hidden_();


  if (BookIndex::instance().entries().empty())
    BookIndex::instance().load(index_path);
  if (!BookIndex::instance().entries().empty()) {
    populate_list_();
    select_first_book_();
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
  // todo we can get rid of this
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
    select_first_book_();

    draw_all_(buf, runtime.battery_percentage());
    buf.full_refresh();
    cached_generation_ = BookIndex::instance().generation();
  }

  sync_runtime_ = &runtime;

  // Repaint when the Sync row's text changes. The index generation above covers
  // books arriving, but a sync that finds nothing new never bumps it — without
  // this the row would sit on "Syncing..." forever.
  const SyncState sync_now = runtime.sync_state();
  if (sync_now != sync_shown_) {
    sync_shown_ = sync_now;
    draw_all_(buf, runtime.battery_percentage());
    buf.refresh();
  }

  ListMenuScreen::update(buttons, buf, runtime);
}

void MainMenu::run_sync_() {
  // Everything lives behind IRuntime: the Wi-Fi stack, the HTTP client and the
  // book transfer are all platform code (platforms/esp32/wifi_sync.h), and this
  // screen is portable. Returns immediately — a sync takes seconds and runs on
  // its own task, so the row's label is what reports progress.
  //
  // The runtime is cached by update() because on_select() does not receive one
  // and the base class keeps its own pointer private.
  if (sync_runtime_)
    sync_runtime_->start_sync();
}

void MainMenu::select_first_book_() {
  if (entries_.empty() || selected() != kSyncRow)
    return;
  set_selected(visual_for_entries(0));
}

void MainMenu::on_select(int index) {
  if (is_action_row_(index)) {
    run_sync_();
    return;
  }
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
  // Normally replace, so backing out of a book lands on the home carousel rather
  // than back in this list — the carousel is sorted most-recently-opened first
  // and record_book_opened() has just put this book at its head. It also frees
  // entries_ immediately.
  //
  // The exception is an empty library, where this screen is the root and there
  // is no home screen underneath: replacing it would leave nothing to go back
  // to, so push instead and Back returns here.
  if (app_->at_root())
    app_->push_screen(ScreenId::Reader);
  else
    app_->replace_screen(ScreenId::Reader);
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
  // The shared BookIndex is deliberately NOT cleared. It is ~30 KB against
  // ~200 KB of free heap, and dropping it meant the home screen, the reader's
  // progress write and the next visit to this list each re-read and re-parsed
  // book_index.dat off the SD card, on the bus the panel shares.
  free_items_storage();
}

void MainMenu::on_back() {
  // When the library is empty this screen *is* the root (see Application::start)
  // — there is no home screen underneath, so Back has nowhere to go.
  if (app_->at_root())
    return;
  app_->pop_screen();
}

void MainMenu::scan_directory_(DrawBuffer& buf) {
  if (!books_dir_ || !app_->data_dir_)
    return;

  std::string root_dir = books_dir_;
  const std::string& index_path = app_->index_path();

  buf.sync_bw_ram();

  BookIndex::instance().build_index(root_dir);
  BookIndex::instance().save(index_path);

}

// Name of the directory containing `path` — the title fallback for a book whose
// WGB metadata is empty, matching what BookIndex does for visible books.
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
      else if (BookIndex::is_wgb_path(full.c_str()))
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
        else if (BookIndex::is_wgb_path(full.c_str()))
          paths.push_back(full);
      }
    } catch (...) {}
#endif
  }

  for (auto& p : paths) {
    BookEntry e;
    WgbReader r;
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

  {
    auto idx = iota_indices_(hidden_.size());
    stable_sort_indices_(idx.data(), static_cast<int>(idx.size()),
                         [](uint16_t ai, uint16_t bi, const void* c) {
                           const auto& v = *static_cast<const std::vector<BookEntry>*>(c);
                           return ci_less(v[ai].title_own, v[bi].title_own);
                         },
                         &hidden_);
    apply_permutation_(hidden_, idx.data());
  }
}

int MainMenu::count() const {
  // +1 for the Sync row; its hairline is already in separators_.
  return 1 + static_cast<int>(entries_.size()) + static_cast<int>(separators_.size());
}

bool MainMenu::is_separator(int index) const {
  for (const auto& s : separators_)
    if (s.first == index) return true;
  return false;
}

std::string_view MainMenu::get_item_label(int index) const {
  if (is_action_row_(index)) {
    // Read from the cached state rather than the runtime: this is called per
    // row draw, and update() already tracks the value. Idle and Failed both
    // read "Sync" — a failure and a fresh start call for the same action, so
    // distinguishing them would only be noise.
    switch (sync_shown_) {
      case SyncState::Working: return "Syncing...";
      case SyncState::Done:    return "Synced";
      default:                 return "Sync";
    }
  }
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
  // Title only — every book on the card is a converted WGB, so the old trailing
  // middle dot marked every single row and said nothing.
  return e.hidden ? std::string_view(e.title_own) : e.title_ref.view(pool);
}

// Reading percentage, drawn at the bottom right of the row beside the author.
// Blank for a book that has never been opened, and for hidden books, which are
// never recorded in the index.
std::string_view MainMenu::get_item_right(int index) const {
  if (is_separator(index) || is_action_row_(index)) return {};
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
  if (is_separator(index) || is_action_row_(index)) return {};
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
  // hairline divider between the two groups. See stable_sort_indices_ above for
  // why this is not std::stable_sort.
  {
    struct Ctx { const std::vector<BookEntry>* v; const StringPool* pool; } ctx{&entries_, &bpool};
    auto idx = iota_indices_(entries_.size());
    stable_sort_indices_(idx.data(), static_cast<int>(idx.size()),
                         [](uint16_t ai, uint16_t bi, const void* c) {
                           const Ctx& x = *static_cast<const Ctx*>(c);
                           const BookEntry& a = (*x.v)[ai];
                           const BookEntry& b = (*x.v)[bi];
                           if (a.last_open_order != b.last_open_order)
                             return a.last_open_order > b.last_open_order;
                           return ci_less(a.title_ref.view(*x.pool), b.title_ref.view(*x.pool));
                         },
                         &ctx);
    apply_permutation_(entries_, idx.data());
  }
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
  // below it by a row — and so does the Sync row above them all. kFirstBook is
  // that combined offset: row 0 Sync, row 1 its hairline, books from row 2.
  static constexpr int kFirstBook = 2;
  if (!entries_.empty())
    separators_.push_back({kSyncRow + 1, ""});
  if (n_hidden > 0 && n_hidden < static_cast<int>(entries_.size()))
    separators_.push_back({n_hidden + kFirstBook, ""});
  if (split > n_hidden && split < static_cast<int>(entries_.size()))
    separators_.push_back({split + (n_hidden > 0 ? 1 : 0) + kFirstBook, ""});

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
