#pragma once

#include <cstdint>
#include <functional>

#include "FontManager.h"
#include "Input.h"
#include "Runtime.h"
#include "ScreenManager.h"
#include "display/DrawBuffer.h"
#include "screens/ChapterSelectScreen.h"
#include "screens/HiddenBooksMenu.h"
#include "screens/IScreen.h"
#include "screens/LinksScreen.h"
#include "screens/LyraExtScreen.h"
#include "screens/MainMenu.h"
#include "screens/RecentBooksScreen.h"
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
  ChapterSelect,
  Links,
  HiddenBooks,
  LyraExt,
  RecentBooks,
};

// Maps the rotate_display / rotate_reader setting value (0-3) to the DrawBuffer Rotation enum.
// 0=Portrait(Deg90), 1=Landscape(Deg0), 2=Portrait-Flip(Deg270), 3=Landscape-Flip(Deg180)
inline Rotation rotation_from_setting(uint8_t v) {
  switch (v) {
    case 1:  return Rotation::Deg0;
    case 2:  return Rotation::Deg270;
    case 3:  return Rotation::Deg180;
    default: return Rotation::Deg90;
  }
}

inline const char* rotation_label(uint8_t v) {
  switch (v) {
    case 1:  return "Landscape";
    case 2:  return "Portrait Reversed";
    case 3:  return "Landscape Reversed";
    default: return "Portrait";
  }
}

class Application {
 public:
  // Menu font size index passed to ListMenuScreen: 0=small 1=medium 2=large 3=X-large.
  static constexpr int kMenuFontSize = 3;

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
  // Book path to auto-open on next start() (set by load_settings_)
  std::string pending_book_path_;
  // Screen to auto-open on next start() (set by load_settings_)
  std::string pending_screen_;

  // Save all persistent state to the settings file
  void save_settings_();
  // Load all persistent state from the settings file
  void load_settings_();
  // Common sleep sequence (save state, show sleep image, set running_=false)
  // Draws the sleep screen and powers down. wordmark_image=true forces the
  // embedded wintergreen image instead of the book cover (power long-press).
  void do_sleep_(DrawBuffer& buf, bool wordmark_image = false);

  // Font management. set_reader_font() also propagates to the reader screen.
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

  // Optional callback to drop the cached converted font (ESP32 only).
  void set_invalidate_font_fn(std::function<void()> fn) {
    invalidate_font_fn_ = std::move(fn);
  }
  void invalidate_font() {
    if (invalidate_font_fn_)
      invalidate_font_fn_();
  }
  bool has_invalidate_font_fn() const {
    return static_cast<bool>(invalidate_font_fn_);
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
  ChapterSelectScreen* chapter_select() {
    return &chapter_select_;
  }
  LinksScreen* links_screen() {
    return &links_screen_;
  }
  MainMenu* main_menu() {
    return &menu_;
  }
  LyraExtScreen* lyra_ext_screen() {
    return &lyra_ext_;
  }
  RecentBooksScreen* recent_books_screen() {
    return &recent_books_;
  }
  // ── Fixed behaviour ───────────────────────────────────────────────────────
  // There is no settings menu. Everything below is compile-time constant; the
  // only tunables live in WintergreenConfig.h at the project root.

  // Nav-arrow glyphs under the list, converted-book marker, book images.
  static constexpr bool show_nav_arrows() { return true; }
  static constexpr bool show_converted_indicator() { return true; }
  static constexpr bool show_reader_images() { return true; }

  // Sleep screen: book cover, no caption text.
  static constexpr bool show_sleep_text() { return false; }

  static constexpr bool sunlight_fading_fix() { return config::kSunlightFadingFix; }
  static constexpr uint8_t sleep_timeout_min() { return config::kAutoSleepMinutes; }

  // Battery as an icon; lists centred; menu font X-Large.
  static constexpr uint8_t battery_display() { return 0; }
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
    rotate_reader_ = v <= 3 ? v : 0;
    save_settings_();
  }

  void update_book_read_time(const std::string& path, uint64_t ms,
                             uint32_t times_opened = 0, uint32_t page_turns = 0,
                             int progress_pct = 0, uint64_t time_left_ms = 0,
                             uint16_t chapter_count = 0, uint32_t total_chars = 0);

  // Called by MainMenu when the user opens a book: updates the open-order
  // counter in the index and persists both the index and settings.
  void record_book_opened(const std::string& path);

  // Extract cover.bin for the given EPUB if it doesn't exist or is stale.
  // No-op if data_dir is not set or the EPUB has no cover.
  // Blocking — can take ~1s. Pass scratch bufs when available to avoid heap pressure.
  void ensure_cover_bin(const std::string& epub_path,
                        uint8_t* scratch1 = nullptr, uint8_t* scratch2 = nullptr,
                        size_t scratch_size = 0);

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

  uint8_t rotate_reader_ = 0;   // independent reader rotation, same encoding

  uint16_t open_counter_ = 0;  // monotonically increasing; incremented each time a book is opened


  ScreenManager screen_mgr_;


  std::string last_seen_version_;

  LyraExtScreen lyra_ext_;
  RecentBooksScreen recent_books_;
  MainMenu menu_;
  ReaderScreen reader_;
  ReaderOptionsScreen reader_options_;
  ChapterSelectScreen chapter_select_;
  LinksScreen links_screen_;
  HiddenBooksMenu hidden_books_;
  bool font_warning_shown_ = false;

  ScreenId pending_push_ = ScreenId::None;
  ScreenId pending_replace_ = ScreenId::None;

  int pending_pop_count_ = 0;

  const BitmapFontSet* reader_font_ = nullptr;
  FontManager* font_manager_ = nullptr;
  std::function<void()> invalidate_font_fn_;

  IScreen* screen_for_(ScreenId id);
};

}  // namespace wintergreen
