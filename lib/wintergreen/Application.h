#pragma once

#include <cstdint>
#include <functional>

#include "FontManager.h"
#include "Input.h"
#include "Runtime.h"
#include "ScreenManager.h"
#include "display/DrawBuffer.h"
#include "screens/IScreen.h"
#include "screens/LyraExtScreen.h"
#include "screens/MainMenu.h"
#include "screens/ReaderOptionsScreen.h"
#include "screens/ReaderScreen.h"
#include "WintergreenConfig.h"

namespace wintergreen {

// All navigable screens in the application.
enum class ScreenId : uint8_t {
  None = 0,
  MainMenu,
  Reader,
  ReaderOptions,
  LyraExt,
};

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
  static constexpr int kMenuFontSize = 3; // TODO REMOVE ME

  Application() = default;

  void set_books_dir(const char* dir) {
    menu_.set_books_dir(dir);
  }

  void set_data_dir(const char* dir) {
    data_dir_ = dir;
    reader_.set_data_dir(dir ? dir : "");
  }

  // Path to data directory for settings/state persistence
  const char* data_dir_ = nullptr;

  // Path to the single unified settings file (cached after set_data_dir)
  std::string settings_path_;
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
  // Returns the name() of the top/active screen for serial diagnostics.
  const char* top_screen_name() const {
    IScreen* top = screen_mgr_.top();
    return top ? top->name() : "none";
  }
  ReaderOptionsScreen* reader_options() {
    return &reader_options_;
  }
  MainMenu* main_menu() {
    return &menu_;
  }
  LyraExtScreen* lyra_ext_screen() {
    return &lyra_ext_;
  }

  // todo remove ---------------

  // Converted-book marker.

  // Sleep screen: book cover, no caption text.
  static constexpr bool show_sleep_text() { return false; }

  static constexpr bool sunlight_fading_fix() { return config::kSunlightFadingFix; }
  static constexpr uint8_t sleep_timeout_min() { return config::kAutoSleepMinutes; }
  static constexpr uint32_t power_hold_sleep_ms() { return config::kPowerHoldSleepMs; }

  // Lists centred; menu font X-Large.
  static constexpr uint8_t list_align() { return 0; }
  static constexpr int menu_font_size() { return kMenuFontSize; }

  // Default button mapping.
  static constexpr bool invert_menu_buttons() { return false; }
  static constexpr bool invert_bottom_paging() { return true; }
  static constexpr bool invert_side_buttons() { return false; }

  // Lists are always portrait. The reader can be flipped to landscape from the
  // in-book quick menu, so that one stays a runtime value.
  static constexpr uint8_t rotate_display() { return 0; }

  uint8_t rotate_reader() const {
    return rotate_reader_;
  }
  void set_rotate_reader(uint8_t v) {
    rotate_reader_ = v == 1 ? 1 : 0;
    save_settings_();
  }


  // Called by MainMenu when the user opens a book: updates the open-order
  // counter in the index and persists both the index and settings.
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

  bool has_pending_transition() const {
    return pending_push_ != ScreenId::None || pending_replace_ != ScreenId::None || pending_pop_count_ > 0;
  }

  void start(DrawBuffer& buf, IRuntime& runtime);

  // Reset the inactivity timer so the device won't sleep. Call each tick
  // whenever an external connection (e.g. USB serial) is active.
  void keep_awake() { inactivity_ms_ = 0; }
  // Auto-open a book by path (skips menu, for debugging).
  void auto_open_book(const char* epub_path, DrawBuffer& buf, IRuntime& runtime);
  void update(const ButtonState& buttons, uint32_t dt_ms, DrawBuffer& buf, IRuntime& runtime);
  bool running() const;
  uint64_t tick_count() const;
  uint32_t uptime_ms() const;

 private:
  ButtonState buttons_{};
  uint64_t ticks_ = 0;
  uint32_t uptime_ms_ = 0;

  bool started_ = false;
  bool running_ = true;

  uint32_t inactivity_ms_ = 0;

  // Power button: armed on a rising edge only, so the press that woke us from
  // deep sleep (cleared by main.cpp) can never register as an action.
  bool power_armed_ = false;
  uint32_t power_hold_ms_ = 0;

  uint8_t rotate_reader_ = 0;   // independent reader rotation, same encoding



  ScreenManager screen_mgr_;



  LyraExtScreen lyra_ext_;
  MainMenu menu_;
  ReaderScreen reader_;
  ReaderOptionsScreen reader_options_;
  bool font_warning_shown_ = false;

  ScreenId pending_push_ = ScreenId::None;
  ScreenId pending_replace_ = ScreenId::None;

  int pending_pop_count_ = 0;

  const BitmapFontSet* reader_font_ = nullptr;
  FontManager* font_manager_ = nullptr;

  IScreen* screen_for_(ScreenId id);
};

}  // namespace wintergreen
