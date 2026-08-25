inline size_t find_key_(const std::string& s, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  const size_t k = s.find(needle);
  if (k == std::string::npos)
    return std::string::npos;
  const size_t colon = s.find(':', k + needle.size());
  return colon == std::string::npos ? std::string::npos : colon + 1;
}

// Strings out of the array starting at `pos`. Handles \" and \\ escapes only —
// the server never emits anything else in a book directory name.
inline std::vector<std::string> parse_string_array_(const std::string& s, size_t pos) {
  std::vector<std::string> out;
  if (pos == std::string::npos)
    return out;
  const size_t open = s.find('[', pos);
  if (open == std::string::npos)
    return out;
  size_t i = open + 1;
  while (i < s.size() && s[i] != ']') {
    if (s[i] != '"') { ++i; continue; }
    std::string item;
    for (++i; i < s.size() && s[i] != '"'; ++i) {
      if (s[i] == '\\' && i + 1 < s.size())
        ++i;
      item.push_back(s[i]);
    }
    ++i;  // closing quote
    out.push_back(std::move(item));
  }
  return out;
}

struct PosEntry {
  std::string dir;
  uint32_t v[4] = {};
};

// {"name":[a,b,c,d], ...}
inline std::vector<PosEntry> parse_pos_object_(const std::string& s, size_t pos) {
  std::vector<PosEntry> out;
  if (pos == std::string::npos)
    return out;
  const size_t open = s.find('{', pos);
  if (open == std::string::npos)
    return out;
  size_t i = open + 1;
  while (i < s.size() && s[i] != '}') {
    if (s[i] != '"') { ++i; continue; }
    PosEntry e;
    for (++i; i < s.size() && s[i] != '"'; ++i) {
      if (s[i] == '\\' && i + 1 < s.size())
        ++i;
      e.dir.push_back(s[i]);
    }
    ++i;
    const size_t lb = s.find('[', i);
    if (lb == std::string::npos)
      break;
    int n = 0;
    i = lb + 1;
    for (; i < s.size() && s[i] != ']' && n < 4; ++i) {
      if (s[i] == ' ' || s[i] == ',')
        continue;
      e.v[n++] = static_cast<uint32_t>(std::strtoul(s.c_str() + i, nullptr, 10));
      while (i < s.size() && s[i] != ',' && s[i] != ']')
        ++i;
      --i;
    }
    const size_t rb = s.find(']', i);
    i = (rb == std::string::npos) ? s.size() : rb + 1;
    if (n == 4)
      out.push_back(std::move(e));
  }
  return out;
}
