#pragma once

#include <cstdint>

#include "Input.h"
#include "Runtime.h"
#include "ScreenManager.h"
#include "display/DrawBuffer.h"
#include "screens/IScreen.h"
#include "screens/HomeScreen.h"
#include "screens/MainMenu.h"
#include "screens/QuickmenuScreen.h"
#include "screens/ReaderScreen.h"
#include "WintergreenConfig.h"

namespace wintergreen {

// All navigable screens in the application.
enum class ScreenId : uint8_t {
  None = 0,
  MainMenu,
  Reader,
  Quickmenu,
  HomeScreen,
};

// Terminal battery voltage. Below this the device saves state, shows the
// wordmark and deep-sleeps, and main.cpp's verify_battery() refuses to boot
// again until the cell has been charged. 0 disables the whole mechanism.
//
// **Not a config option, on purpose.** It is a property of the cell and the
// board's brownout margin, not a preference, and getting it wrong makes the
// device look bricked — read "Low-battery cutoff" in CLAUDE.md before touching
// it. Three numbers bound the choice:
//
//   ~2500 mV  the cell's own protection IC, far past where damage starts
//   ~3000 mV  where deep-discharge begins costing permanent capacity
//    3300 mV  0% on battery_percentage()'s discharge polynomial
//
// 3200 sits just under the curve's zero and just above the damage point. There
// is almost nothing left to win by going lower: on a Li-ion discharge curve the
// last 200 mV before 3.0 V is one or two percent of capacity, and the reading is
// taken with the panel *idle*, so the loaded voltage during a refresh is already
// lower than what is measured here.
inline constexpr uint16_t kLowBatteryCutoffMv = 3200;

// Two orientations only. The reversed ones were dropped, and with them every
// drawing branch for them: they are the same physical hold as their
// counterparts, so they earned a settings toggle that did nothing a user could
// see except confuse the paging maps.
inline Rotation rotation_from_setting(uint8_t v) {
  return v == 1 ? Rotation::Deg0 : Rotation::Deg90;
}

inline const char* rotation_label(uint8_t v) {
  return v == 1 ? "Landscape" : "Portrait";
}

class Application {
 public:
  Application() = default;
  void set_books_dir(const char* dir) {
    menu_.set_books_dir(dir);
  }

  // Where persistent state lives. Two hidden files at the root of the card, and
  // **no directory**: there used to be a `.wintergreen/` folder holding
  // `settings`, `book_index.dat` and one `<hash>.pos` per book. The positions
  // moved next to their books (see book_pos_path) and the other two are now
  // dotfiles, so nothing has to be created before the first save.
  //
  // They stay two files rather than one because they are written on completely
  // different schedules: settings is a few dozen bytes saved whenever the device
  // sleeps, while the index is ~30 KB written once a session. Merging them would
  // rewrite the index every time a font size changed.
  void set_state_root(const char* dir) {
    data_dir_ = dir;
    if (!dir)
      return;
    settings_path_ = std::string(dir) + "/.wintergreen";
    index_path_ = std::string(dir) + "/.wintergreen-index";
  }

  const std::string& index_path() const {
    return index_path_;
  }

  // Root the EPUB-cache cover paths are derived from. Only the WGB branch of
  // CoverPaths is reachable on device, so this is effectively just "the card".
  const char* data_dir_ = nullptr;

  std::string settings_path_;
  std::string index_path_;
  std::string settings_written_;
  std::string pending_book_path_;

  void save_settings_();
  void load_settings_();
  void do_sleep_(DrawBuffer& buf, bool wordmark_image = false);

  void set_reader_font(const BitmapFontSet* fonts) {
    reader_font_ = fonts;
    reader_.set_fonts(fonts);
  }
  // Null only when the font partition is missing, in which case no book opens.
  const BitmapFontSet* reader_font() const {
    return reader_font_;
  }

  ReaderScreen* reader() {
    return &reader_;
  }
  // Returns true when the Reader is the top/active screen. The clicker swaps
  // its keys while it is.
  bool is_reader_active() const {
    return screen_mgr_.top() == &reader_;
  }
  bool confirm_from_power() const {
    return power_confirm_;
  }
  QuickmenuScreen* quickmenu() {
    return &quickmenu_;
  }
  MainMenu* main_menu() {
    return &menu_;
  }

  // Only the reader has an orientation setting (the in-book quick menu flips
  // it); every list screen is portrait.
  uint8_t rotate_reader() const {
    return rotate_reader_;
  }
  void set_rotate_reader(uint8_t v) {
    rotate_reader_ = v == 1 ? 1 : 0;
    save_settings_();
  }

  // Called when a book is opened: bumps the open-order counter in the in-memory
  // index and persists settings. The index itself is written once per session,
  // by record_book_progress() when the reader closes.
  void record_book_opened(const std::string& path);

  // Called by ReaderScreen when it closes: caches the reading percentage in the
  // index so the book list can show it without reopening every book.
  void record_book_progress(const std::string& path, int pct);

  // Navigate to a screen: push on top of the current screen (current stays on stack).
  // Or replace the current screen (pop it first, then push the new one).
  // safe to call from within a screen's update(); the transition happens after update() returns.
  void push_screen(ScreenId id) {
    pending_push_ = id;
    pending_replace_ = ScreenId::None;
    pending_pop_count_ = 0;
  }
  void replace_screen(ScreenId id) {
    pending_push_ = ScreenId::None;
    pending_replace_ = id;
    pending_pop_count_ = 0;
  }
  void pop_screen(int count = 1) {
    pending_push_ = ScreenId::None;
    pending_replace_ = ScreenId::None;
    pending_pop_count_ = count;
  }

  // True when the top screen is the only one on the stack.
  bool at_root() const {
    return screen_mgr_.depth() <= 1;
  }

  bool has_pending_transition() const {
    return pending_push_ != ScreenId::None || pending_replace_ != ScreenId::None || pending_pop_count_ > 0;
  }

  void start(DrawBuffer& buf, IRuntime& runtime);

  // Reset the inactivity timer so the device won't sleep.
  // Called each tick while a Wi-Fi sync runs: suppresses auto-sleep and the
  // low-battery cutoff, so a transfer is never cut mid-file.
  void keep_awake() {
    inactivity_ms_ = 0;
    low_battery_ms_ = 0;
  }
  // Open a book by path, bypassing the menu: the boot resume path.
  void auto_open_book(const char* epub_path, DrawBuffer& buf, IRuntime& runtime);
  // Write what persist_pending_ says is owed (index, settings) now rather than at
  // the next quiet moment. For anything about to reload the index from disk.
  void flush_persist();
  void update(const ButtonState& buttons, uint32_t dt_ms, DrawBuffer& buf, IRuntime& runtime);
  bool running() const;
  // How long the UI loop may block before update() has work again, or 0 for
  // ordinary frame cadence (anything in motion: a button down, a transition,
  // the panel mid-waveform, the perf hold, a pending card write). Otherwise
  // the nearest timer: the 1 s battery poll or auto-sleep. Input, clicker and
  // sync end the wait early through ui_wake, so this is a ceiling, never a
  // delay on anything that happens.
  uint32_t idle_wait_ms(DrawBuffer& buf) const;

 private:
  bool started_ = false;
  bool running_ = true;

  uint32_t inactivity_ms_ = 0;
  // Milliseconds since the UI last did anything; drives the DFS performance hold.
  uint32_t perf_idle_ms_ = 0;
  static constexpr uint32_t kPerfHoldMs = 300;
  // Any button held at the last update(): power hold and hold-repeat count
  // frames' worth of dt while it lasts, so the loop keeps its cadence.
  bool buttons_down_ = false;

  // Low-battery cutoff state — see Application::update.
  // Wall time the battery must stay below the cutoff before acting. Comfortably
  // longer than a full refresh, which sags the rail hardest.
  static constexpr uint32_t kLowBatteryHoldMs = 4000;
  uint32_t low_battery_ms_ = 0;
  uint32_t battery_poll_ms_ = 0;
  static constexpr uint32_t kBatteryPollMs = 1000;

  // Power button: armed on a rising edge only, so the press that woke us from
  // deep sleep (cleared by main.cpp) can never register as an action.
  bool power_armed_ = false;
  uint32_t power_hold_ms_ = 0;

  // True for the frame whose Confirm is a power-button tap rather than the front
  // select key. The reader opens its quick menu on the top setting for the one
  // and on the current chapter for the other.
  bool power_confirm_ = false;

  // Set by start() when it resumes straight into a book: the home screen sits
  // underneath unstarted and the book index is not read until something needs
  // it. See record_book_progress().
  bool index_deferred_ = false;

  // Index and settings writes owed by a reader close or a pop from the reader
  // stack, held back so the next screen draws first. See the end of update().
  bool persist_pending_ = false;
  static constexpr uint32_t kPersistQuietMs = 400;
  uint32_t persist_quiet_ms_ = 0;

  uint8_t rotate_reader_ = 0;   // independent reader rotation, same encoding

  ScreenManager screen_mgr_;

  HomeScreen home_;
  MainMenu menu_;
  ReaderScreen reader_;
  QuickmenuScreen quickmenu_;

  ScreenId pending_push_ = ScreenId::None;
  ScreenId pending_replace_ = ScreenId::None;

  int pending_pop_count_ = 0;

  const BitmapFontSet* reader_font_ = nullptr;

  IScreen* screen_for_(ScreenId id);
};

}  // namespace wintergreen
