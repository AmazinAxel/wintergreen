#include <cstdio>
#include <filesystem>
#include <iostream>
#include <vector>

#include "miniz.h"

#include "desktop_config.h"
#include "display.h"
#include "input.h"
#include "wintergreen/Application.h"
#include "wintergreen/FontManager.h"
#include "wintergreen/Loop.h"
#include "wintergreen/content/BitmapFont.h"
#include "wintergreen/display/DrawBuffer.h"
#include "runtime.h"

// Load a file into a byte vector. Returns empty on failure.
static std::vector<uint8_t> load_file(const char* path) {
  std::vector<uint8_t> data;
  FILE* f = fopen(path, "rb");
  if (!f)
    return data;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz <= 0) {
    fclose(f);
    return data;
  }
  fseek(f, 0, SEEK_SET);
  data.resize(static_cast<size_t>(sz));
  fread(data.data(), 1, data.size(), f);
  fclose(f);
  return data;
}

// Must match kFontAsset in platforms/esp32/font_manager.h.
static constexpr const char* kFontAsset = "AtkinsonHyperlegible.bin";

class DesktopFontManager : public wintergreen::FontManager {
 public:
  explicit DesktopFontManager(wintergreen::Application& app) : app_(app) {}

  void ensure_ready(wintergreen::DrawBuffer&) override {
    if (font_set_.valid())
      return;  // already loaded — there is only one font

    // Same asset the device ships: [uint32 uncompressed size][zlib stream]
    // wrapping the FNTS v2 bundle. See tools/make_font.py.
    static const std::string path =
        (std::filesystem::path(WINTERGREEN_REPO_ROOT) / "resources" / kFontAsset).string();

    const std::vector<uint8_t> packed = load_file(path.c_str());
    if (packed.size() < 4) {
      fprintf(stderr, "[font] cannot read %s\n", path.c_str());
      return;
    }

    const uint32_t raw_size = static_cast<uint32_t>(packed[0]) | (static_cast<uint32_t>(packed[1]) << 8) |
                              (static_cast<uint32_t>(packed[2]) << 16) | (static_cast<uint32_t>(packed[3]) << 24);
    bundle_data_.assign(raw_size, 0);
    mz_ulong out_len = raw_size;
    if (mz_uncompress(bundle_data_.data(), &out_len, packed.data() + 4,
                      static_cast<mz_ulong>(packed.size() - 4)) != MZ_OK ||
        out_len != raw_size) {
      fprintf(stderr, "[font] failed to decompress %s\n", path.c_str());
      bundle_data_.clear();
      return;
    }

    if (load_bundle(bundle_data_.data(), bundle_data_.size())) {
      printf("[font] Loaded %s (%u bytes)\n", kFontAsset, static_cast<unsigned>(bundle_data_.size()));
      app_.set_reader_font(font_set());
    }
  }

 private:
  wintergreen::Application& app_;
  std::vector<uint8_t> bundle_data_;
};

int main() {
  try {
    DesktopRuntime runtime(16);
    DesktopInputSource input(runtime);
    DesktopEmulatorDisplay display(runtime);
    wintergreen::Application app;
    wintergreen::DrawBuffer buf(display);

    // Mount the repo-root sd/ folder as the virtual SD card.
    // WINTERGREEN_SD_DIR is set by CMake via desktop_config.h.
    static std::string books_path = std::filesystem::absolute(WINTERGREEN_SD_DIR).string();
    std::filesystem::create_directories(books_path);
    std::filesystem::create_directories(books_path + "/fonts");
    app.set_books_dir(books_path.c_str());

    // Data directory for converted books, settings, reading state.
    static std::string data_path = (std::filesystem::absolute(WINTERGREEN_SD_DIR) / ".wintergreen").string();
    app.set_data_dir(data_path.c_str());

    DesktopFontManager font_mgr(app);
    app.set_font_manager(&font_mgr);

    // Provide an initial font so that Application::start() passes the auto-open check.
    // The correct custom font will be loaded when ReaderScreen::start() is entered.
    font_mgr.ensure_ready(buf);

    app.start(buf, runtime);
    wintergreen::run_loop(app, buf, input, runtime);

    // sleep for 3 second so we see the sleep screen
    SDL_Delay(3000);
  } catch (const std::exception& e) {
    std::cerr << "Fatal: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
