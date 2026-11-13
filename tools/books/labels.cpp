#include "labels.h"

#include <regex>
#include <vector>

#include "wintergreen/content/HtmlEntities.h"

namespace books {

namespace {

// ── UTF-8 and a small case table (Latin, Greek, Cyrillic) ──────────────────

uint32_t next_cp(std::string_view s, size_t& i) {
  const auto c = static_cast<unsigned char>(s[i]);
  int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
  if (i + n > s.size())
    n = 1;
  uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
  for (int k = 1; k < n; ++k)
    cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
  i += n;
  return cp;
}

bool cp_upper(uint32_t c) {
  if (c >= 'A' && c <= 'Z')
    return true;
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
    return true;
  if (c >= 0x100 && c <= 0x17F) {
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
      return c % 2 == 1;
    return c == 0x178 || (c % 2 == 0 && c != 0x138);
  }
  return (c >= 0x391 && c <= 0x3A9) || (c >= 0x400 && c <= 0x42F);
}

bool cp_lower(uint32_t c) {
  if (c >= 'a' && c <= 'z')
    return true;
  if (c >= 0xDF && c <= 0xFF && c != 0xF7)
    return true;
  if (c >= 0x100 && c <= 0x17F)
    return !cp_upper(c);
  return (c >= 0x3B1 && c <= 0x3C9) || (c >= 0x430 && c <= 0x45F);
}

bool cp_letter(uint32_t c) {
  if (cp_upper(c) || cp_lower(c))
    return true;
  return (c >= 0x180 && c <= 0x24F) || (c >= 0x370 && c <= 0x52F) || c >= 0x3040;
}

uint32_t cp_to_lower(uint32_t c) {
  if (!cp_upper(c))
    return c;
  if (c < 0x100)
    return c + 0x20;
  if (c == 0x178)
    return 0xFF;
  if (c < 0x180)
    return c + 1;
  if (c >= 0x400 && c <= 0x40F)
    return c + 0x50;
  return c + 0x20;
}

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// For matching against the ASCII patterns: curly apostrophes as straight ones,
// so "Author’s Note" matches "author.?s note" the way PCRE's UTF mode did.
std::string ascii_quotes(std::string_view s) {
  std::string out(s);
  for (const char* q : {"\xE2\x80\x99", "\xE2\x80\x98"}) {
    for (size_t p; (p = out.find(q)) != std::string::npos;)
      out.replace(p, 3, "'");
  }
  return out;
}

const std::regex& re(const char* pattern) {
  // Patterns here are few and fixed; compile each once.
  static std::vector<std::pair<const char*, std::regex>> cache;
  for (auto& [p, r] : cache)
    if (p == pattern)
      return r;
  cache.emplace_back(pattern, std::regex(pattern, std::regex::ECMAScript | std::regex::icase));
  return cache.back().second;
}

constexpr const char* kMatter =
    "^(cover$|title( page)?$|half[- ]title|copyright|(table of )?contents$|dedication|epigraph|acknowledg|"
    "about the|also by|by the same|other (books|titles)|praise|newsletter|sign up|join |excerpt|preview|"
    "sneak peek|bonus|index$|notes$|endnotes|glossary|bibliography|references$|maps?$|map of|"
    "cast of characters|dramatis|foreword|preface|introduction|prologue|epilogue|afterword|postscript|"
    "coda$|interlude|appendix|author.?s note|a note on|translator|editor.?s note|"
    "(part|book|volume|letter|section|act|scene|canto)\\s+[^\\s:.]+([:.]|$)|.*project gutenberg)";
constexpr const char* kJunk =
    "^(cover$|title( page)?$|half[- ]title|copyright|(table of )?contents$|also by|by the same|"
    "other (books|titles)|praise|newsletter|sign up|join |excerpt|preview|sneak peek|bonus|"
    "about the publisher|index$|.*project gutenberg)";

}  // namespace

std::string tidy(std::string_view in) {
  std::string s;
  bool pending = false;
  for (char c : in) {
    if (is_space(c)) {
      pending = !s.empty();
      continue;
    }
    if (pending)
      s += ' ';
    pending = false;
    s += c;
  }
  return s;
}

bool is_matter(std::string_view label) {
  const std::string s = ascii_quotes(label);
  return std::regex_search(s, re(kMatter));
}

bool is_junk(std::string_view label) {
  const std::string s = ascii_quotes(label);
  return std::regex_search(s, re(kJunk));
}

bool is_contents(std::string_view label) {
  const std::string s(label);
  return std::regex_search(s, re("^(table of )?contents$"));
}

std::string key(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    const size_t a = i;
    const uint32_t cp = next_cp(s, i);
    if ((cp >= '0' && cp <= '9') || cp_letter(cp)) {
      if (cp < 0x80 || cp_upper(cp))
        wintergreen::append_utf8(out, cp_to_lower(cp));
      else
        out.append(s.substr(a, i - a));
    }
  }
  return out;
}

std::string slug(std::string_view in) {
  std::string s;
  bool pending = false;
  for (char c : in) {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c + 32);
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (is_space(c)) {
      pending = true;
      continue;
    }
    if (!ok)
      continue;
    if (pending && !s.empty())
      s += ' ';
    pending = false;
    s += c;
  }
  size_t a = 0, b = s.size();
  auto trimmed = [](char c) { return c == ' ' || c == '.' || c == '-'; };
  while (a < b && trimmed(s[a]))
    ++a;
  while (b > a && trimmed(s[b - 1]))
    --b;
  s = s.substr(a, b - a);
  if (s.empty())
    s = "book";
  return s.substr(0, 100);
}

}  // namespace books
