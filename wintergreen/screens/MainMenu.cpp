#include "MainMenu.h"

#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <queue>

#include "../Application.h"
#include "../content/BookIndex.h"
#include "../content/CoverPaths.h"

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
  if (!app_->data_dir_)
    return;

  const std::string& index_path = app_->index_path();

  if (show_hidden_ && hidden_.empty())
    scan_hidden_();


  if (BookIndex::instance().entries().empty())
    BookIndex::instance().load(index_path);

  // Scan here, not from update(). Deferring it meant the first paint happened
  // with an unpopulated list — a lone, vertically centred Sync row — and the
  // real list only replaced it a frame later.
  if (BookIndex::instance().entries().empty())
    scan_directory_();

  populate_list_();
  select_first_book_();
  cached_generation_ = BookIndex::instance().generation();
}

void MainMenu::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  // A sync that downloaded books bumps the generation (wifi_sync.h) while this
  // screen is visible; without this they don't appear until you navigate away
  // and back.
  //
  // One repaint per change, holding everything final. The sync worker bumps the
  // generation a moment before it sets its end state, so while it still reads
  // Working the new books wait; the frame that sees the end state shows the
  // books and the row's final label together. Painting each as it came drew
  // the books under "Syncing", then a second refresh for the label.
  sync_runtime_ = &runtime;
  const SyncState sync_now = runtime.sync_state();
  const bool syncing = has_sync_ && sync_now == SyncState::Working;
  const bool books_changed = !syncing && cached_generation_ != BookIndex::instance().generation();
  // Repaint when the Sync row's text changes too: a sync that finds nothing new
  // never bumps the generation, and the row would sit on "Syncing" forever.
  const bool label_changed = has_sync_ && sync_now != sync_shown_;
  if (books_changed) {
    cached_generation_ = BookIndex::instance().generation();
    forget_thumbs_();  // a sync or upload may have replaced a book in place
    populate_list_();
  }
  if (label_changed)
    sync_shown_ = sync_now;
  if (books_changed || label_changed) {
    // One normal update either way: the old frame is known, so the diff
    // drives only what changed (no flash after a sync).
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
  // The sync reloads the index from disk first; a reader close may still owe it
  // a write (Application::flush_persist), and that progress would be lost.
  if (buf_)
    buf_->wait_panel_idle();  // the card shares SPI2 with the panel
  app_->flush_persist();
  if (sync_runtime_)
    sync_runtime_->start_sync();
}

void MainMenu::select_first_book_() {
  if (entries_.empty() || !is_action_row_(selected()))
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
  const std::string path(path_of_(e));
  // A hidden book is never recorded: mark_opened() would do nothing (it is not
  // indexed) and the index save would be pointless, but more to the point a
  // hidden book must not surface in the recents carousel.
  if (!e.hidden)
    app_->record_book_opened(path);
  app_->reader()->set_path(path);
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
  std::string cur = current_book_path();
  if (!cur.empty())
    initial_selection_ = std::move(cur);

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

void MainMenu::scan_directory_() {
  if (!books_dir_ || !app_->data_dir_)
    return;

  std::string root_dir = books_dir_;
  const std::string& index_path = app_->index_path();

  BookIndex::instance().build_index(root_dir);
  BookIndex::instance().save(index_path);

}

void MainMenu::scan_hidden_() {
  hidden_.clear();
  if (!books_dir_)
    return;
  if (buf_)
    buf_->wait_panel_idle();  // the card shares SPI2 with the panel

  std::vector<std::string> paths;
  std::queue<std::string> q;
  q.push(std::string(books_dir_) + "/.hidden");
  while (!q.empty()) {
    const std::string dir = std::move(q.front());
    q.pop();
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
  }

  // Titles come from `.hidden/.wintergreen-index` (path|title|author lines,
  // written here only when the set of hidden books changed), so a reveal opens
  // no WGB except for a book added since. The folder walk above stays: it is
  // what notices books added or removed by hand.
  const std::string cache_path = std::string(books_dir_) + "/.hidden/.wintergreen-index";
  struct Cached {
    std::string path, title, author;
  };
  std::vector<Cached> cached;
  if (FILE* f = std::fopen(cache_path.c_str(), "rb")) {
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
      line[std::strcspn(line, "\r\n")] = '\0';
      char* s1 = std::strchr(line, '|');
      char* s2 = s1 ? std::strchr(s1 + 1, '|') : nullptr;
      if (!s2)
        continue;
      *s1 = *s2 = '\0';
      cached.push_back({line, s1 + 1, s2 + 1});
    }
    std::fclose(f);
  }
  bool changed = cached.size() != paths.size();
  for (auto& p : paths) {
    BookEntry e;
    auto hit = std::find_if(cached.begin(), cached.end(), [&](const Cached& c) { return c.path == p; });
    if (hit != cached.end()) {
      e.title_own = std::move(hit->title);
      e.author_own = std::move(hit->author);
    } else {
      changed = true;
      if (!read_book_meta(p, e.title_own, e.author_own))
        continue;
    }
    e.path_own = std::move(p);
    e.hidden = true;
    hidden_.push_back(std::move(e));
  }
  if (changed) {
    const std::string tmp = cache_path + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
      for (const BookEntry& e : hidden_)
        std::fprintf(f, "%s|%s|%s\n", e.path_own.c_str(), e.title_own.c_str(), e.author_own.c_str());
      if (std::fclose(f) == 0) {
        std::remove(cache_path.c_str());
        std::rename(tmp.c_str(), cache_path.c_str());
      } else {
        std::remove(tmp.c_str());
      }
    }
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

std::string_view MainMenu::path_of_(const BookEntry& e) const {
  return e.hidden ? std::string_view(e.path_own) : e.path_ref.view(BookIndex::instance().pool());
}

int MainMenu::count() const {
  // The Sync row, if any; its hairline is already in separators_.
  return sync_rows_() + static_cast<int>(entries_.size()) + static_cast<int>(separators_.size());
}

bool MainMenu::is_separator(int index) const {
  for (const int s : separators_)
    if (s == index) return true;
  return false;
}

std::string_view MainMenu::get_item_label(int index) const {
  if (is_action_row_(index)) {
    // Read from the cached state rather than the runtime: this is called per
    // row draw, and update() already tracks the value. Idle and Failed both
    // read "Sync" — a failure and a fresh start call for the same action, so
    // distinguishing them would only be noise.
    switch (sync_shown_) {
      case SyncState::Working: return "Syncing";
      case SyncState::Done:    return "Synced";
      default: return "Sync";
    }
  }
  if (is_separator(index))
    return {};
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
  // Views straight into stable storage — copying into a member string cost an
  // allocation per row per draw for any author past the SSO length.
  return e.hidden ? std::string_view(e.author_own) : e.author_ref.view(BookIndex::instance().pool());
}

void MainMenu::populate_list_() {
  clear_items();
  entries_.clear();
  separators_.clear();

  const StringPool& bpool = BookIndex::instance().pool();
  detail_title_lines_ = 2;
  entries_.reserve(BookIndex::instance().entries().size() + (show_hidden_ ? hidden_.size() : 0));
  int pos = 0;
  for (const auto& idx : BookIndex::instance().entries()) {
    BookEntry e;
    e.index_pos = pos++;
    e.path_ref = idx.path;
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
  // below it by a row — and so does the Sync row above them all. first_book is
  // that combined offset: row 0 Sync, row 1 its hairline, books from row 2.
  const int first_book = has_sync_ ? 2 : 0;
  if (has_sync_ && !entries_.empty())
    separators_.push_back(1);
  if (n_hidden > 0 && n_hidden < static_cast<int>(entries_.size()))
    separators_.push_back(n_hidden + first_book);
  if (split > n_hidden && split < static_cast<int>(entries_.size()))
    separators_.push_back(split + (n_hidden > 0 ? 1 : 0) + first_book);

  if (!initial_selection_.empty()) {
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
      if (path_of_(entries_[i]) == initial_selection_) {
        set_selected(visual_for_entries(i));
        break;
      }
    }
    initial_selection_.clear();
  }
}


// ── Cover thumbnails ────────────────────────────────────────────────────────

int MainMenu::prepare_thumbs(int box_h) const {
  // Covers are 2:3, so the column is two thirds of the box — kListCoverW for the
  // two-title-line row, which is what the converter renders cover_list.bin to.
  return box_h >= kListCoverH ? kListCoverW : 0;
}

bool MainMenu::get_item_thumb(int index, Thumb& out) const {
  if (is_separator(index) || is_action_row_(index))
    return false;
  const int real = entries_index_for(index);
  if (real < 0 || real >= static_cast<int>(entries_.size()))
    return false;

  // Keyed by path, not by entries_ index: entries_ is rebuilt on every visit
  // and its order moves whenever a book is opened.
  const std::string_view path = path_of_(entries_[real]);
  const uint32_t key = book_path_key(path);
  ThumbSlot* slot = nullptr;
  for (ThumbSlot& s : thumb_slots_)
    if (s.valid && s.key == key)
      slot = &s;
  if (!slot) {
    // Least recently drawn slot; empty ones carry stamp 0 and go first. Rows
    // drawn earlier in this pass carry newer stamps, so a page never evicts its
    // own covers.
    slot = &thumb_slots_[0];
    for (ThumbSlot& s : thumb_slots_)
      if (s.used < slot->used)
        slot = &s;
    *slot = ThumbSlot{};
    slot->key = key;
    slot->valid = true;
    load_thumb_(path, entries_[real].index_pos, *slot);
  }
  slot->path = path;  // views move when entries_ is rebuilt
  slot->used = ++thumb_clock_;
  if (slot->w == 0)
    return false;
  out.src = const_cast<MainMenu*>(this);
  out.item = static_cast<uint32_t>(slot - thumb_slots_);
  out.gen = key ^ (thumb_epoch_ * 0x9E3779B9u);
  out.base = slot->packed ? slot->pack_offset : 4;
  out.w = slot->w;
  out.h = slot->h;
  return true;
}

// Where the book's cover is and how big: the pack slot when its key matches
// (one open of the pack for a whole page of rows), else cover_list.bin, whose
// size must be exactly the header's (anything else is not a cover: a truncated
// download, or an HTML error page saved under the name). Leaves w == 0 when
// there is none, cached like any other answer.
void MainMenu::load_thumb_(std::string_view book_path, int index_pos, ThumbSlot& slot) const {
  slot.w = slot.h = 0;
  if (buf_)
    buf_->wait_panel_idle();  // the card shares SPI2 with the panel
  if (index_pos >= 0 && app_) {
    FILE* pack = std::fopen(thumb_pack_path(app_->index_path()).c_str(), "rb");
    uint8_t h[8];
    const uint32_t at = static_cast<uint32_t>(index_pos * thumb_pack_slot_bytes());
    if (pack && std::fseek(pack, static_cast<long>(at), SEEK_SET) == 0 && std::fread(h, 1, 8, pack) == 8) {
      const uint32_t k = h[0] | (h[1] << 8) | (h[2] << 16) | (static_cast<uint32_t>(h[3]) << 24);
      const int w = h[4] | (h[5] << 8), hh = h[6] | (h[7] << 8);
      if (k == slot.key) {
        std::fclose(pack);
        if (w > 0 && hh > 0 && w <= kListCoverW && hh <= kListCoverH) {
          slot.packed = true;
          slot.pack_offset = at + 8;
          slot.w = static_cast<uint8_t>(w);
          slot.h = static_cast<uint8_t>(hh);
        }
        return;  // the pack's answer, cover or none
      }
    }
    if (pack)
      std::fclose(pack);
  }
  const std::string path = cover_list_bin_path(std::string(book_path).c_str());
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f)
    return;
  uint16_t hdr[2] = {};
  bool ok = std::fread(hdr, 2, 2, f) == 2 && hdr[0] > 0 && hdr[1] > 0 && hdr[0] <= kListCoverW &&
            hdr[1] <= kListCoverH;
  ok = ok && std::fseek(f, 0, SEEK_END) == 0 &&
       std::ftell(f) == static_cast<long>(4 + cover_cols_bytes(hdr[0], hdr[1]));
  std::fclose(f);
  if (!ok)
    return;
  slot.packed = false;
  slot.w = static_cast<uint8_t>(hdr[0]);
  slot.h = static_cast<uint8_t>(hdr[1]);
}

bool MainMenu::read(uint32_t item, uint32_t offset, uint8_t* dst, size_t n) {
  if (item >= kThumbSlotsMax || !app_)
    return false;
  const ThumbSlot& slot = thumb_slots_[item];
  FILE* f = nullptr;
  if (slot.packed) {
    if (!pack_ && !pack_tried_) {
      pack_tried_ = true;
      pack_ = std::fopen(thumb_pack_path(app_->index_path()).c_str(), "rb");
    }
    f = pack_;
  } else {
    if (own_slot_ != static_cast<int>(item)) {
      if (own_)
        std::fclose(own_);
      own_ = std::fopen(cover_list_bin_path(std::string(slot.path).c_str()).c_str(), "rb");
      own_slot_ = static_cast<int>(item);
    }
    f = own_;
  }
  return f && std::fseek(f, static_cast<long>(offset), SEEK_SET) == 0 && std::fread(dst, 1, n, f) == n;
}

void MainMenu::release() {
  if (pack_)
    std::fclose(pack_);
  if (own_)
    std::fclose(own_);
  pack_ = own_ = nullptr;
  pack_tried_ = false;
  own_slot_ = -1;
}

void MainMenu::forget_thumbs_() const {
  for (ThumbSlot& s : thumb_slots_)
    s = ThumbSlot{};
  thumb_clock_ = 0;
  ++thumb_epoch_;
}

}  // namespace wintergreen
