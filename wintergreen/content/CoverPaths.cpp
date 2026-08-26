#include "CoverPaths.h"

#include <string>

namespace wintergreen {

// The four public entry points differ only in a filename, so they share one
// body. Written out separately they inlined into ~1.3 KB apiece.
static std::string sibling_(const char* book_path, const char* name) {
  const std::string p(book_path);
  const size_t slash = p.find_last_of('/');
  return (slash == std::string::npos) ? std::string(name) : p.substr(0, slash + 1) + name;
}

std::string cover_bin_path(const char* book_path) {
  return sibling_(book_path, "cover.bin");
}

std::string cover_sleep_bin_path(const char* book_path) {
  return sibling_(book_path, "cover_sleep.bin");
}

std::string cover_home_bin_path(const char* book_path) {
  return sibling_(book_path, "cover_home.bin");
}

std::string book_pos_path(const char* book_path) {
  return sibling_(book_path, "book.pos");
}

}  // namespace wintergreen
