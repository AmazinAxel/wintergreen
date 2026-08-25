#include "CoverPaths.h"

#include <cctype>
#include <cstring>

namespace wintergreen {

static bool is_wgb_(const char* path) {
  const char* dot = std::strrchr(path, '.');
  if (!dot)
    return false;
  return std::tolower(static_cast<unsigned char>(dot[1])) == 'w' &&
         std::tolower(static_cast<unsigned char>(dot[2])) == 'g' &&
         std::tolower(static_cast<unsigned char>(dot[3])) == 'b' && dot[4] == '\0';
}

static std::string stem_(const char* path) {
  const char* slash = nullptr;
  for (const char* c = path; *c; ++c)
    if (*c == '/' || *c == '\\')
      slash = c;
  const char* name = slash ? slash + 1 : path;
  const char* dot = std::strrchr(name, '.');
  return dot ? std::string(name, static_cast<size_t>(dot - name)) : std::string(name);
}

// The three public entry points differ only in a filename, so they share one
// body. Written out three times they inlined into ~1.3 KB apiece.
static std::string cover_path_(const char* book_path, const char* data_dir, const char* name) {
  if (is_wgb_(book_path)) {
    const std::string p(book_path);
    const size_t slash = p.find_last_of('/');
    return (slash == std::string::npos) ? std::string(name) : p.substr(0, slash + 1) + name;
  }
  return std::string(data_dir) + "/cache/" + stem_(book_path) + "/" + name;
}

std::string cover_bin_path(const char* book_path, const char* data_dir) {
  return cover_path_(book_path, data_dir, "cover.bin");
}

std::string cover_sleep_bin_path(const char* book_path, const char* data_dir) {
  return cover_path_(book_path, data_dir, "cover_sleep.bin");
}

std::string cover_home_bin_path(const char* book_path, const char* data_dir) {
  return cover_path_(book_path, data_dir, "cover_home.bin");
}

std::string book_pos_path(const char* book_path, const char* data_dir) {
  return cover_path_(book_path, data_dir, "book.pos");
}

}  // namespace wintergreen
