#pragma once

#include <cstdint>

#include "FontManager.h"
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

// Two orientations only. The reversed ones (Deg270/Deg180) were dropped: they
// are the same physical hold as their counterparts, so they earned a settings
// toggle that did nothing a user could see except confuse the paging maps —
// portrait is Deg90 *or* Deg270 and landscape is Deg0 *or* Deg180, and code that
// checked for one exact value behaved wrongly in the reversed case.
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
    reader_.set_data_dir(dir);
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
  void set_font_manager(FontManager* fm) {
    font_manager_ = fm;
  }
  FontManager* font_manager() const {
    return font_manager_;
  }

  ReaderScreen* reader() {
    return &reader_;
  }
  // Returns true when the Reader is the top/active screen — used by the main
  // loop to decide whether scratch-needing index ops (Add/Rename) must be
  // deferred to avoid corrupting display buffers the Reader is rendering to.
  bool is_reader_active() const {
    return screen_mgr_.top() == &reader_;
  }
  QuickmenuScreen* quickmenu() {
    return &quickmenu_;
  }
  MainMenu* main_menu() {
    return &menu_;
  }
  HomeScreen* home_screen() {
    return &home_;
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

  // Hand the resident book index (~30 KB) back to the heap so the BLE stack has
  // room. Called synchronously just before the radio is switched on, because
  // bringup allocates immediately and a next-frame release would be too late.
  //
  // The reader does not need the index while a book is open, and every screen
  // reloads it from disk when empty — see BookIndex::release_memory.
  void release_ram_for_radio();

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

  // Reset the inactivity timer so the device won't sleep. Call each tick
  // whenever an external connection (e.g. USB serial) is active.
  // Called each tick while a USB host is attached: suppresses auto-sleep and the
  // low-battery cutoff, since the device is being powered rather than drained.
  void keep_awake() {
    inactivity_ms_ = 0;
    usb_powered_ = true;
    low_battery_ms_ = 0;
  }
  // True while a USB host is attached, i.e. the device is being charged rather
  // than drained. Valid only during update() — main.cpp asserts it each frame
  // via keep_awake() and update() clears it on the way out.
  bool usb_powered() const {
    return usb_powered_;
  }
  // Auto-open a book by path (skips menu, for debugging).
  void auto_open_book(const char* epub_path, DrawBuffer& buf, IRuntime& runtime);
  void update(const ButtonState& buttons, uint32_t dt_ms, DrawBuffer& buf, IRuntime& runtime);
  bool running() const;
  uint64_t tick_count() const;
  uint32_t uptime_ms() const;

 private:
  uint64_t ticks_ = 0;
  uint32_t uptime_ms_ = 0;

  bool started_ = false;
  bool running_ = true;

  uint32_t inactivity_ms_ = 0;
  // Milliseconds since the UI last did anything; drives the DFS performance hold.
  uint32_t perf_idle_ms_ = 0;

  // Low-battery cutoff state — see Application::update.
  // Wall time the battery must stay below the cutoff before acting. Comfortably
  // longer than a full refresh, which sags the rail hardest.
  static constexpr uint32_t kLowBatteryHoldMs = 4000;
  uint32_t low_battery_ms_ = 0;
  uint32_t battery_poll_ms_ = 0;

  bool usb_powered_ = false;

  // Power button: armed on a rising edge only, so the press that woke us from
  // deep sleep (cleared by main.cpp) can never register as an action.
  bool power_armed_ = false;
  uint32_t power_hold_ms_ = 0;

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
  FontManager* font_manager_ = nullptr;

  IScreen* screen_for_(ScreenId id);
};

}  // namespace wintergreen
