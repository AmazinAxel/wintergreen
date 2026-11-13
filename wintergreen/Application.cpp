#include "Application.h"

#include <cstdio>
#include <cstring>

#include "content/CoverPaths.h"
#include "content/BookIndex.h"

namespace wintergreen {

// The sleep cover is composed by the converter at exactly the panel size and
// blitted 1:1. Nothing at runtime can adapt if the two disagree — the size
// check in show_book_cover_sleep_ simply fails and the wordmark shows instead —
// so the mismatch is caught here rather than on the glass. It said 800 once
// (the panel hides 7 columns at the leading edge; app space is 793) and no
// cover ever appeared.
static_assert(kSleepCoverW == DrawBuffer::kWidth, "sleep cover must be panel width");
static_assert(kSleepCoverH == DrawBuffer::kHeight, "sleep cover must be panel height");
static_assert(kSleepFrameBytes == DrawBuffer::kBufSize && kSleepFrameStride == DisplayFrame::kStride &&
                  kSleepFrameRows == DisplayFrame::kPhysicalHeight && kSleepFrameOffX == DisplayFrame::kPanelOffsetX,
              "sleep frame must be the panel's frame");

void Application::start(DrawBuffer& buf, IRuntime& runtime) {
  started_ = true;
  running_ = true;

  // All of the possible menus!! There's only four, pretty much the bare minimum!
  home_.set_app(this);
  menu_.set_app(this);
  reader_.set_app(this);
  quickmenu_.set_app(this);


  load_settings_();

  // Compile-time constant; pushed once here rather than on every frame.
  buf.set_sunlight_fading_fix(config::kSunlightFadingFix);
  buf.set_rotation(Rotation::Deg90);

  // Pick the root screen. With no books at all the carousel has nothing to show
  // and its only useful action is "Back for all books", so start on the list
  // instead — which is also where Sync lives, the one thing that can fix an
  // empty library. MainMenu::on_back() then does nothing, because there is no
  // home screen underneath to return to.
  //
  // Don't auto-open books from the hidden folder
  if (!pending_book_path_.empty() && pending_book_path_.find("/.hidden/") != std::string::npos)
    pending_book_path_.clear();
  if (!(reader_font_ && reader_font_->valid()))
    pending_book_path_.clear();

  if (!pending_book_path_.empty()) {
    // Resuming a book — the common wake. A book was open, so it was opened, so
    // the root is the carousel; it goes on the stack unstarted. Starting it
    // would read and parse the ~30 KB index, read and scale a 25 KB cover off
    // the card and draw it all, only for the reader to paint over it before the
    // panel ever refreshed. It builds itself when the reader pops.
    screen_mgr_.push_inactive(&home_);
    index_deferred_ = true;
    auto_open_book(pending_book_path_.c_str(), buf, runtime);
    pending_book_path_.clear();
  } else {
    // The index has to be loaded here to answer that; both screens load it only
    // when it is empty, so this costs nothing extra.
    if (data_dir_ && BookIndex::instance().entries().empty())
      BookIndex::instance().load(index_path_);

    // The carousel shows the most recently *opened* books, so it needs reading
    // history, not merely a non-empty library. With none it would fall back to
    // showing arbitrary books under a "recents" framing, so start on the list.
    bool any_opened = false;
    for (const auto& e : BookIndex::instance().entries())
      if (e.last_open_order != 0) { any_opened = true; break; }
    screen_mgr_.push(any_opened ? static_cast<IScreen*>(&home_) : static_cast<IScreen*>(&menu_), buf, runtime);
  }


  buf.full_refresh();
}

void Application::auto_open_book(const char* epub_path, DrawBuffer& buf, IRuntime& runtime) {
  reader_.set_path(epub_path);
  screen_mgr_.push(&reader_, buf, runtime);
}

// Show `book_path`'s cover as the sleep screen. Returns false when the book has
// no usable cover_sleep.bin, in which case the caller falls back to the wordmark.
//
// The file is composed by `books` (tools/books) as the panel's own frame
// (CoverPaths.h): artwork scaled, dithered and laid out in physical byte
// order, so one fread straight into the draw buffer is the whole job. No
// scratch buffer, no blit. A short file leaves a partial frame, which the
// caller's wordmark fallback overwrites in full.
static bool show_book_cover_sleep_(DrawBuffer& buf, const char* data_dir, const std::string& book_path) {
  if (!data_dir || book_path.empty()) return false;

  const std::string cpath = cover_sleep_bin_path(book_path.c_str());
  FILE* f = std::fopen(cpath.c_str(), "rb");
  if (!f) return false;
  const bool ok = std::fread(buf.frame(), 1, kSleepFrameBytes, f) == kSleepFrameBytes && std::fgetc(f) == EOF;
  std::fclose(f);
  if (!ok)
    return false;

  // Half ignores prev (CTRL1_BYPASS_RED) and paints from `pixels` alone, which
  // is what keeps it to a single pass and makes the sleep screen appear quietly.
  //
  // RefreshMode::Full was tried here for a reported left/right fade on one
  // cover. It did not fix it and its extra inversion phases were visibly
  // flashier, so it was reverted. The fade is not a panel artefact: measuring
  // cover_sleep.bin directly, ink coverage runs 0.52 at the centre column band
  // down to ~0.30 at both edges, smoothly. That cover's artwork simply has a
  // vignette and the panel is rendering it faithfully. Other covers are
  // near-uniform (the hobbit ~0.95, lord of the flies ~0.10) and have no
  // gradient to show. Don't go looking for a waveform fix for this one.
  buf.full_refresh(RefreshMode::Half, /*turnOffScreen=*/true);
  return true;
}

void Application::do_sleep_(DrawBuffer& buf, bool wordmark_image) {
  // Everything below writes or reads the SD card (position, settings, cover) and
  // the last page turn's refresh may still be running — refreshes no longer block.
  // The card shares SPI2 with the panel, so drain once here for the whole path.
  buf.wait_panel_idle();

  // Reset rotation before drawing the sleep screen — it is always portrait.
  buf.set_rotation(Rotation::Deg90);

  // The cover of the most recently opened book is the default sleep screen.
  // The embedded wintergreen image is kept as the fallback, and is what a
  // power-button long-press should show once that gesture is wired up.
  // The cover shows only while a book is actually open, and never for a hidden
  // one — the sleep screen is on display while the device sits around, so a
  // hidden book must not be advertised by it. Anything else gets the wordmark.
  // Sleeping from the quick menu also shows the wordmark (a deliberate "hide the
  // cover" gesture); the reader is still on the stack, so boot resumes the book.
  std::string cover_book;
  if (!wordmark_image && screen_mgr_.top() != &quickmenu_ && screen_mgr_.contains(&reader_) &&
      reader_.has_path()) {
    const std::string p = reader_.get_path();
    if (p.find("/.hidden/") == std::string::npos)
      cover_book = p;
  }

  // The sleep screen goes up *first* and the state is saved behind it. The
  // writes below (.pos, the ~30 KB index, settings) used to run before it, so
  // every one of them was latency between the power-button hold and the panel
  // reacting. The waveform blocks (turnOffScreen), so the card is free again by
  // the time they run; the panel's own deep-sleep command waits for them, since
  // a reader stop() drains the panel first and must not find it asleep.
  bool shown = false;
  if (!cover_book.empty())
    shown = show_book_cover_sleep_(buf, data_dir_, cover_book);
  if (!shown)
    shown = buf.show_sleep_image_embedded();

  // Stop the active screen so it can save state (e.g. reading position).
  // The reader now persists position only in stop(), so if a child screen (the
  // quick menu) is on top it must be stopped too or the session is lost.
  IScreen* top = screen_mgr_.top();
  if (top)
    top->stop();
  if (top != &reader_ && screen_mgr_.contains(&reader_))
    reader_.stop();
  save_settings_();
  flush_persist();  // anything Back or a reader close left unwritten

  buf.deep_sleep();

  running_ = false;
}

void Application::update(const ButtonState& buttons, uint32_t dt_ms, DrawBuffer& buf, IRuntime& runtime) {
  if (!started_)
    start(buf, runtime);
  if (!running_)
    return;
  buttons_down_ = buttons.current != 0;

  // Dynamic frequency scaling: hold the CPU at 160 MHz while the UI is working,
  // let it idle at 80 MHz otherwise. Page layout and drawing take no PM lock of
  // their own — no driver is involved — so without this a page turn would run at
  // the idle clock. The hold is raised on any button activity or pending screen
  // transition and dropped after kPerfHoldMs of quiet. All the CPU work of a turn
  // — layout, draw, SPI, and the next page's pre-draw — happens within a frame
  // or two of the press; what follows is the waveform, which the CPU only waits
  // out, so holding 160 MHz through it was ~7 mA spent on nothing. 750 ms became
  // 300, still a dozen frames past the last of that work.
  if (buttons.current != 0 || buttons.pressed_latch != 0 || has_pending_transition()) {
    perf_idle_ms_ = 0;
    runtime.set_performance_hold(true);
  } else if (perf_idle_ms_ < kPerfHoldMs) {
    perf_idle_ms_ += dt_ms;
    if (perf_idle_ms_ >= kPerfHoldMs)
      runtime.set_performance_hold(false);
  }


  // Low-battery cutoff. Deep-discharging a Li-ion cell below ~3.0 V costs it
  // permanent capacity, so the device puts itself away before it gets there and
  // main.cpp refuses to boot again until it has been charged.
  //
  // This is dangerous code: a false positive makes the device look bricked, and
  // it shipped once with three faults that together did exactly that. Anything
  // that changes it must preserve all five guards below.
  //
  //   1. A zero reading means "cannot measure" and is ignored.
  //   2. So is anything below kImplausibleMv — a broken ADC reads near zero, and
  //      a cell that far gone is past the protection IC anyway.
  //   3. The low reading must persist for kLowBatteryHoldMs of *wall time*, not
  //      a frame count. This originally required five frames — about 125 ms,
  //      shorter than a single e-ink refresh, which is the largest current spike
  //      the device has and drags the rail down with it.
  //   4. It never samples while the panel is mid-refresh, for the same reason.
  //   5. USB does not bypass it (removed on purpose). Recovery is the charger,
  //      which runs with the SoC asleep; flashing a cut-off device means
  //      starting the upload and holding power.
  //   6. It samples once a second, not once a frame. The cell cannot move in
  //      25 ms, and an ADC conversion plus its calibration curve is real work to
  //      repeat 40 times a second for an answer that never changes.
  if (kLowBatteryCutoffMv > 0 && !buf.display().is_busy()) {
    static constexpr int kImplausibleMv = 2500;
    battery_poll_ms_ += dt_ms;
    if (battery_poll_ms_ >= kBatteryPollMs) {
      const uint32_t elapsed = battery_poll_ms_;
      battery_poll_ms_ = 0;
      const int mv = runtime.battery_millivolts();
      if (mv >= kImplausibleMv && mv < kLowBatteryCutoffMv) {
        // Charge the *measured* interval, so the hold stays wall-clock even
        // though the sampling is coarse.
        low_battery_ms_ += elapsed;
        if (low_battery_ms_ >= kLowBatteryHoldMs) {
          do_sleep_(buf, /*wordmark_image=*/true);
          return;
        }
      } else {
        low_battery_ms_ = 0;
      }
    }
  }

  // Inactivity / auto-sleep tracking
  if (buttons.current != 0 || buttons.pressed_latch != 0) {
    inactivity_ms_ = 0;
  } else {
    inactivity_ms_ += dt_ms;
    if (config::kAutoSleepMinutes > 0) {
      const uint32_t timeout_ms = static_cast<uint32_t>(config::kAutoSleepMinutes) * 60u * 1000u;
      if (inactivity_ms_ >= timeout_ms) {
        do_sleep_(buf);
        return;
      }
    }
  }


  // Power button: hold to sleep, tap to select. The tap resolves on release —
  // the only way to tell it apart from a hold — and is forwarded to the screen
  // as a synthetic Button1 (Confirm) press, so screens need no power handling.
  ButtonState fwd = buttons;
  power_confirm_ = false;
  if (buttons.is_pressed(Button::Power)) {
    power_armed_ = true;
    power_hold_ms_ = 0;
  } else if (power_armed_ && buttons.is_down(Button::Power)) {
    power_hold_ms_ += dt_ms;
    if (power_hold_ms_ >= config::kHoldDelayMs) {
      power_armed_ = false;
      do_sleep_(buf);
      return;
    }
  } else if (power_armed_) {
    power_armed_ = false;
    power_confirm_ = true;  // the reader opens its menu differently for this Confirm
    const uint8_t confirm = static_cast<uint8_t>(Button::Button1);
    fwd.pressed_latch |= static_cast<uint8_t>(1u << confirm);
    if (fwd.press_history_count < ButtonState::kMaxPressHistory)
      fwd.press_history[fwd.press_history_count++] = confirm;
  }
  // Screens never see the raw power key.
  fwd.pressed_latch &= static_cast<uint8_t>(~(1u << static_cast<uint8_t>(Button::Power)));
  fwd.current &= static_cast<uint8_t>(~(1u << static_cast<uint8_t>(Button::Power)));
  {
    uint8_t n = 0;
    for (uint8_t i = 0; i < fwd.press_history_count; ++i)
      if (static_cast<Button>(fwd.press_history[i]) != Button::Power)
        fwd.press_history[n++] = fwd.press_history[i];
    fwd.press_history_count = n;
  }

  IScreen* top = screen_mgr_.top();
  if (top) {
    top->update(fwd, buf, runtime);

    // Process pending navigation (queued by screens via push_screen/replace_screen).
    if (pending_replace_ != ScreenId::None) {
      ScreenId id = pending_replace_;
      pending_replace_ = ScreenId::None;
      screen_mgr_.replace(screen_for_(id), buf, runtime);
      buf.refresh();
    } else if (pending_push_ != ScreenId::None) {
      ScreenId id = pending_push_;
      pending_push_ = ScreenId::None;
      screen_mgr_.push(screen_for_(id), buf, runtime);
      buf.refresh();
    } else if (pending_pop_count_ > 0) {
      int count = pending_pop_count_;
      pending_pop_count_ = 0;
      const bool was_reader_stack = (top == &reader_ || top == &quickmenu_);
      if (was_reader_stack)
        buf.wait_panel_idle();  // the pop writes the card; the panel may still be mid-refresh
      screen_mgr_.pop(count, buf, runtime);
      // Settings are saved *after* the pop, not before — and not until the new
      // screen is on the glass (flush_persist). save_settings_() writes
      // book_path only while the reader is still on the stack, so saving first
      // persisted "resume this book" for a book the user had just closed — and
      // then had to be rewritten without it at the next sleep. One write, and
      // the right contents.
      if (was_reader_stack)
        persist_pending_ = true;  // written by flush_persist(), after the refresh below
      buf.refresh();
    }
  }

  // Card writes deferred off the transition path (persist_pending_). They go
  // out once input has been still for a moment and the panel is idle, so they
  // neither hold up a screen nor share SPI2 with a waveform; do_sleep_() flushes
  // whatever is left, which every ordinary way of putting the device down goes
  // through.
  if (buttons.current != 0 || buttons.pressed_latch != 0)
    persist_quiet_ms_ = 0;
  else if (persist_quiet_ms_ < kPersistQuietMs)
    persist_quiet_ms_ += dt_ms;
  if (persist_pending_ && persist_quiet_ms_ >= kPersistQuietMs && !buf.display().is_busy() &&
      !has_pending_transition())
    flush_persist();
}

uint32_t Application::idle_wait_ms(DrawBuffer& buf) const {
  if (!started_ || !running_ || buttons_down_ || power_armed_ || perf_idle_ms_ < kPerfHoldMs || persist_pending_ ||
      has_pending_transition() || buf.display().is_busy())
    return 0;
  uint32_t wait = UINT32_MAX;
  if (kLowBatteryCutoffMv > 0)
    wait = battery_poll_ms_ < kBatteryPollMs ? kBatteryPollMs - battery_poll_ms_ : 1;
  if (config::kAutoSleepMinutes > 0) {
    const uint32_t timeout_ms = static_cast<uint32_t>(config::kAutoSleepMinutes) * 60u * 1000u;
    const uint32_t left = inactivity_ms_ < timeout_ms ? timeout_ms - inactivity_ms_ : 1;
    if (left < wait)
      wait = left;
  }
  return wait;
}

void Application::flush_persist() {
  if (!persist_pending_)
    return;
  persist_pending_ = false;
  BookIndex::instance().save(index_path_);  // returns at once when clean
  save_settings_();                         // skips the write when unchanged
}

IScreen* Application::screen_for_(ScreenId id) {
  switch (id) {
    case ScreenId::MainMenu:
      return &menu_;
    case ScreenId::Reader:
      return &reader_;
    case ScreenId::Quickmenu:
      return &quickmenu_;
    case ScreenId::HomeScreen:
      return &home_;
    default:
      return nullptr;
  }
}
void Application::save_settings_() {
  if (settings_path_.empty())
    return;

  ReaderScreen* reader = &reader_;
  const bool reader_active = screen_mgr_.contains(reader);

  std::string out;
  char line[600];
  if (reader_active && reader->has_path() && reader->get_path().find("/.hidden/") == std::string::npos) {
    std::snprintf(line, sizeof(line), "book_path=%s\n", reader->get_path().c_str());
    out += line;
  }
  const ReaderSettings& rs = reader->reader_settings();
  std::snprintf(line, sizeof(line), "font_size=%u\n", static_cast<unsigned>(rs.font_size_idx));
  out += line;
  std::snprintf(line, sizeof(line), "rotate_reader=%u\n", static_cast<unsigned>(rotate_reader_));
  out += line;

  if (out == settings_written_) // dont write to file if nothing changed
    return;

  const std::string tmp_path = settings_path_ + ".tmp";
  FILE* f = std::fopen(tmp_path.c_str(), "w");
  if (!f)
    return;
  const bool wrote = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  if (std::fclose(f) != 0 || !wrote) {
    std::remove(tmp_path.c_str());
    return;
  }

  // Write-to-temp + rename is not a backup — no copy of the old file is kept.
  // It is what stops fopen(path, "w") truncating the real file to zero bytes
  // before writing a byte, on a device that saves during auto-sleep.
  //
  // The remove() is required, not tidiness: FatFs f_rename returns FR_EXIST
  // when the destination exists, and ESP-IDF's VFS passes it straight through
  // without unlinking first — so unlike POSIX, rename() here does NOT replace.
  // Without this every save after the first silently failed and left a .tmp.
  std::remove(settings_path_.c_str());
  if (std::rename(tmp_path.c_str(), settings_path_.c_str()) == 0)
    settings_written_ = out;
}


void Application::record_book_opened(const std::string& path) {
  // In-memory only. Neither file is written here.
  //
  // The index goes out once a session, when the reader closes and
  // record_book_progress() runs. Settings used to be written here too, purely so
  // that a reboot resumed this book — but every ordinary way of putting the
  // device down (power button, auto-sleep) routes through do_sleep_(), which
  // saves settings anyway. The write here only mattered when power was lost
  // without sleeping, and it bought that case an SD write on *every* book open.
  // `.pos` is written at close for the same reason, so a hard power loss already
  // resumes at the position you started from.
  BookIndex::instance().mark_opened(path);
}
void Application::record_book_progress(const std::string& path, int pct) {
  if (!data_dir_ || path.empty())
    return;
  BookIndex& idx = BookIndex::instance();
  // A session that resumed straight into a book never read the index (see
  // start()), so read it now — once.
  if (index_deferred_ && idx.entries().empty())
    idx.load(index_path_);
  index_deferred_ = false;
  // The index stays resident for the whole session (see MainMenu::stop), so the
  // reload-then-re-clear dance this used to do — to avoid save() truncating the
  // file when nothing was loaded — is gone. An empty index here means no book
  // was ever indexed, and saving that would be the truncation it guarded against.
  if (idx.entries().empty())
    return;
  idx.set_progress(path, pct);
  // Not saved here: this runs on the reader's way out, ahead of the screen that
  // replaces it, and a ~30 KB index write there was latency on every Back from a
  // book. flush_persist() writes it once the next screen is up.
  persist_pending_ = true;
}

void Application::load_settings_() {
  if (settings_path_.empty())
    return;
  FILE* f = std::fopen(settings_path_.c_str(), "r");
  if (!f) {
    // Power was lost between the remove() and the rename() in save_settings_
    // The .tmp is a complete file, so prefer it to starting from defaults.
    const std::string tmp_path = settings_path_ + ".tmp";
    f = std::fopen(tmp_path.c_str(), "r");
  }
  if (!f)
    return;

  char line[512];
  std::string last_book_path;
  ReaderSettings& rs = reader_.reader_settings();

  while (std::fgets(line, sizeof(line), f)) {
    // Strip trailing newline
    char* nl = std::strchr(line, '\n');
    if (nl)
      *nl = 0;

    char sval[512];
    unsigned uval = 0;
    if (std::sscanf(line, "book_path=%511[^\n]", sval) == 1)
      last_book_path = sval;
    else if (std::sscanf(line, "font_size=%u", &uval) == 1)
      // Anything out of range reads as the default. A settings file written by a
      // build with the old five-size bundle will have an index past the end.
      rs.font_size_idx = uval < ReaderSettings::kNumFontSizePresets ? static_cast<uint8_t>(uval) : 0;
    else if (std::sscanf(line, "rotate_reader=%u", &uval) == 1)
      rotate_reader_ = static_cast<uint8_t>(uval == 1 ? 1 : 0);
  }
  std::fclose(f);


  pending_book_path_ = last_book_path; // to open
}

bool Application::running() const {
  return running_;
}
}  // namespace wintergreen
