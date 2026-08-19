#include "CoverPaths.h"

#include <cctype>
#include <cstring>

namespace wintergreen {

static bool is_mrb_(const char* path) {
  const char* dot = std::strrchr(path, '.');
  if (!dot)
    return false;
  return std::tolower(static_cast<unsigned char>(dot[1])) == 'm' &&
         std::tolower(static_cast<unsigned char>(dot[2])) == 'r' &&
         std::tolower(static_cast<unsigned char>(dot[3])) == 'b' && dot[4] == '\0';
}

static std::string sibling_(const char* path, const char* name) {
  const std::string p(path);
  const size_t slash = p.find_last_of('/');
  return (slash == std::string::npos) ? std::string(name) : p.substr(0, slash + 1) + name;
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

std::string cover_bin_path(const char* book_path, const char* data_dir) {
  if (is_mrb_(book_path))
    return sibling_(book_path, "cover.bin");
  return std::string(data_dir) + "/cache/" + stem_(book_path) + "/cover.bin";
}

std::string cover_sleep_bin_path(const char* book_path, const char* data_dir) {
  if (is_mrb_(book_path))
    return sibling_(book_path, "cover_sleep.bin");
  return std::string(data_dir) + "/cache/" + stem_(book_path) + "/cover_sleep.bin";
}

}  // namespace wintergreen
