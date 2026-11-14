// books
//
// Review every EPUB in ~/Downloads/books: title, author, cover, chapters. Each
// is baked to WGB in a temp dir; the WGB goes to the homelab library and the
// reviewed EPUB to its backup folder as "<title> by <author>.epub". Once the
// upload succeeds the uploaded EPUBs are deleted here, so nothing is kept on
// this machine. Books that fail stay in ~/Downloads/books.
//
// Chapters are decided here, not on the device: each kept entry becomes one
// WGB chapter, which the device shows only as its number (1, 2, 3... in kept
// order; names never leave this machine), and content of dropped chapters
// never reaches the WGB. The backed-up EPUB carries the same list as wg.ncx,
// dropped entries marked, so reviewing it again starts from it.

#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#include "labels.h"
#include "review.h"
#include "wintergreen/content/Book.h"
#include "wintergreen/content/CoverPaths.h"

namespace fs = std::filesystem;
using books::Review;

namespace {

constexpr const char* kHost = "alec@alechomelab";
constexpr const char* kRemote = "/media/books";
// Backup EPUBs. A dot dir: booksync.js never lists or serves those, and the
// device only ever fetches book.wgb + covers by name anyway.
constexpr const char* kEpubDir = ".epubs";

fs::path g_tmp;

void cleanup() {
  std::error_code ec;
  if (!g_tmp.empty())
    fs::remove_all(g_tmp, ec);
}

[[noreturn]] void quit() {
  std::printf("\n");
  cleanup();
  std::_Exit(1);
}

// ── line editor ──────────────────────────────────────────────────────────────
//
// Raw termios with the default already in the buffer, so a title is fixed by
// editing it rather than retyping it.

size_t cp_start(const std::string& s, size_t i) {  // start of the codepoint before i
  while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80)
    --i;
  return i > 0 ? i - 1 : 0;
}

size_t cp_end(const std::string& s, size_t i) {  // end of the codepoint at i
  if (i >= s.size())
    return i;
  ++i;
  while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80)
    ++i;
  return i;
}

size_t columns(const std::string& s, size_t a, size_t b) {
  size_t n = 0;
  for (size_t i = a; i < b; ++i)
    n += (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80;
  return n;
}

// Returns the edited line; exits on Ctrl-C, or on Ctrl-D / EOF.
std::string edit_line(const std::string& prompt, const std::string& initial) {
  std::fflush(stdout);
  termios old{};
  tcgetattr(STDIN_FILENO, &old);
  termios raw = old;
  raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG);
  raw.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

  std::string buf = initial;
  size_t cur = buf.size();
  auto draw = [&] {
    std::printf("\r\x1b[K%s%s", prompt.c_str(), buf.c_str());
    const size_t back = columns(buf, cur, buf.size());
    if (back)
      std::printf("\x1b[%zuD", back);
    std::fflush(stdout);
  };
  auto done = [&](bool ok) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    std::printf("\n");
    if (!ok)
      quit();
  };
  draw();
  for (;;) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) {
      done(false);
    }
    if (c == '\r' || c == '\n') {
      done(true);
      return buf;
    }
    if (c == 3 || (c == 4 && buf.empty()))
      done(false);
    if (c == 127 || c == 8) {
      if (cur > 0) {
        const size_t a = cp_start(buf, cur);
        buf.erase(a, cur - a);
        cur = a;
      }
    } else if (c == 1) {
      cur = 0;
    } else if (c == 5) {
      cur = buf.size();
    } else if (c == 21) {
      buf.erase(0, cur);
      cur = 0;
    } else if (c == 11) {
      buf.erase(cur);
    } else if (c == 23) {
      size_t a = cur;
      while (a > 0 && buf[a - 1] == ' ')
        --a;
      while (a > 0 && buf[a - 1] != ' ')
        --a;
      buf.erase(a, cur - a);
      cur = a;
    } else if (c == 27) {
      unsigned char s[3] = {};
      if (read(STDIN_FILENO, &s[0], 1) != 1 || read(STDIN_FILENO, &s[1], 1) != 1)
        continue;
      if (s[1] == 'D' && cur > 0)
        cur = cp_start(buf, cur);
      else if (s[1] == 'C')
        cur = cp_end(buf, cur);
      else if (s[1] == 'H')
        cur = 0;
      else if (s[1] == 'F')
        cur = buf.size();
      else if (s[1] >= '0' && s[1] <= '9' && read(STDIN_FILENO, &s[2], 1) == 1 && s[2] == '~') {
        if (s[1] == '3' && cur < buf.size())
          buf.erase(cur, cp_end(buf, cur) - cur);
        else if (s[1] == '1' || s[1] == '7')
          cur = 0;
        else if (s[1] == '4' || s[1] == '8')
          cur = buf.size();
      }
    } else if (c >= 0x20) {
      buf.insert(cur, 1, static_cast<char>(c));
      ++cur;
    }
    draw();
  }
}

// ── cover preview: sixel of the converter's own cover_sleep.bin ─────────────

void sixel(const std::vector<uint8_t>& bits, int w, int h) {
  const size_t stride = (static_cast<size_t>(w) + 7) / 8;
  auto white = [&](int x, int y) { return (bits[static_cast<size_t>(y) * stride + (x >> 3)] >> (7 - (x & 7))) & 1; };
  std::string out = "\x1bP0;1;0q\"1;1;" + std::to_string(w) + ";" + std::to_string(h) +
                    "#0;2;100;100;100#1;2;0;0;0";
  for (int y0 = 0; y0 < h; y0 += 6) {
    for (int color = 0; color < 2; ++color) {
      out += color ? "#1" : "#0";
      char run = 0;
      int n = 0;
      auto flush = [&] {
        if (n > 3)
          out += "!" + std::to_string(n) + run;
        else
          out.append(static_cast<size_t>(n), run);
        n = 0;
      };
      for (int x = 0; x < w; ++x) {
        int v = 0;
        for (int k = 0; k < 6 && y0 + k < h; ++k)
          if (white(x, y0 + k) == (color == 0))
            v |= 1 << k;
        const char ch = static_cast<char>(63 + v);
        if (n && ch != run)
          flush();
        run = ch;
        ++n;
      }
      flush();
      out += color ? "-" : "$";
    }
  }
  out += "\x1b\\\n";
  std::fwrite(out.data(), 1, out.size(), stdout);
}

void show_cover(const Review& r, const std::string& epub) {
  std::string src = epub;
  if (!r.new_cover.empty()) {
    src = (g_tmp / "preview.epub").string();
    if (!r.write_epub(src, false)) {
      std::printf("  (cover preview failed)\n");
      return;
    }
  }
  const std::string bin = (g_tmp / "cover_sleep.bin").string();
  wintergreen::Book book;
  if (book.open(src.c_str()) != wintergreen::EpubError::Ok ||
      !book.write_sleep_cover_bin(bin.c_str(), wintergreen::kSleepCoverW, wintergreen::kSleepCoverH)) {
    std::printf("  (no cover)\n");
    return;
  }
  std::ifstream f(bin, std::ios::binary);
  // The file is the panel's frame; turn it back into portrait rows.
  std::vector<uint8_t> frame(wintergreen::kSleepFrameBytes);
  f.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
  if (!f)
    return;
  const int w = wintergreen::kSleepCoverW, h = wintergreen::kSleepCoverH;
  const size_t stride = (static_cast<size_t>(w) + 7) / 8;
  std::vector<uint8_t> bits(stride * static_cast<size_t>(h), 0xFF);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (!wintergreen::sleep_frame_white(frame.data(), x, y))
        bits[static_cast<size_t>(y) * stride + (x >> 3)] &= static_cast<uint8_t>(~(0x80u >> (x & 7)));
  sixel(bits, w, h);
}

// ── one book ─────────────────────────────────────────────────────────────────

void print_chapters(const Review& r) {
  for (size_t i = 0; i < r.chapters.size(); ++i)
    std::printf("%3zu %s %s\n", i + 1, r.chapters[i].keep ? "[x]" : "[ ]", r.chapters[i].label.c_str());
}

// "Dune by Frank Herbert.epub". Kept within one ustar name field (100 bytes)
// so the tar stream needs no long-name extension.
std::string epub_name(const Review& r, const fs::path& epub) {
  std::string s = r.title.empty() ? epub.stem().string() : r.title;
  if (!r.author.empty())
    s += " by " + r.author;
  for (char& c : s)
    if (c == '/' || static_cast<unsigned char>(c) < 0x20)
      c = '-';
  if (s.size() > 94)
    s.erase(cp_start(s, 95));
  while (!s.empty() && (s.back() == ' ' || s.back() == '.'))
    s.pop_back();
  return s + ".epub";
}

bool process(const fs::path& epub, const fs::path& out, const fs::path& epubs) {
  Review r;
  if (const std::string err = r.open(epub.string()); !err.empty()) {
    std::printf("  %s, skipped\n", err.c_str());
    return false;
  }
  r.title = books::normalize_space(edit_line("title  ", r.title));
  r.author = books::normalize_space(edit_line("author ", r.author));

  for (;;) {
    show_cover(r, epub.string());
    std::string c = edit_line("cover (enter keep, or image path) ", "");
    if (c.empty())
      break;
    while (!c.empty() && (c.back() == ' ' || c.back() == '\'' || c.back() == '"'))
      c.pop_back();
    while (!c.empty() && (c.front() == ' ' || c.front() == '\'' || c.front() == '"'))
      c.erase(0, 1);
    if (c[0] == '~')
      c = std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + c.substr(1);
    std::string ext = fs::path(c).extension().string();
    for (char& ch : ext)
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::error_code ec;
    if (!fs::is_regular_file(c, ec))
      std::printf("  not a file\n");
    else if (ext != ".jpg" && ext != ".jpeg" && ext != ".png")
      std::printf("  jpg or png only\n");
    else
      r.new_cover = fs::absolute(c).string();
  }

  r.propose_chapters();
  for (const auto& n : r.notes)
    std::printf("  note: %s\n", n.c_str());
  static const std::regex range_re(R"(^(\d+)(?:-(\d+))?$)");
  for (;;) {
    print_chapters(r);
    const std::string cmd = edit_line("chapters (N or N-M toggles, reset re-proposes, enter done) ", "");
    if (cmd == "reset") {
      r.propose_chapters(/*fresh=*/true);
      for (const auto& n : r.notes)
        std::printf("  note: %s\n", n.c_str());
      continue;
    }
    if (cmd.empty())
      break;
    std::smatch m;
    size_t p = 0;
    while (p < cmd.size()) {
      const size_t q = std::min(cmd.find(' ', p), cmd.size());
      const std::string tok = cmd.substr(p, q - p);
      p = q + 1;
      if (!std::regex_match(tok, m, range_re))
        continue;
      const size_t a = std::strtoul(m.str(1).c_str(), nullptr, 10);
      const size_t b = m[2].matched ? std::strtoul(m.str(2).c_str(), nullptr, 10) : a;
      for (size_t k = a; k <= b && k <= r.chapters.size(); ++k)
        if (k >= 1)
          r.chapters[k - 1].keep = !r.chapters[k - 1].keep;
    }
  }
  if (std::none_of(r.chapters.begin(), r.chapters.end(), [](const books::Chapter& c) { return c.keep; })) {
    std::printf("  every chapter dropped, skipped\n");
    return false;
  }

  const std::string reviewed = (g_tmp / "book.epub").string();
  if (!r.write_epub(reviewed, true)) {
    std::printf("  writing the reviewed EPUB failed, skipped\n");
    return false;
  }
  std::string summary;
  const std::string name = r.bake(reviewed, out.string(), summary);
  if (name.empty()) {
    std::error_code ec;
    fs::remove_all(out / books::slug(r.title), ec);
    std::printf("  %s, skipped\n", summary.c_str());
    return false;
  }
  const std::string backup = epub_name(r, epub);
  std::error_code ec;
  fs::rename(reviewed, epubs / backup, ec);
  if (ec) {
    fs::remove_all(out / name, ec);
    std::printf("  staging the EPUB failed, skipped\n");
    return false;
  }
  std::printf("  ok  %s: %s\n      %s\n", name.c_str(), summary.c_str(), backup.c_str());
  return true;
}

// ── upload: ustar stream into ssh ────────────────────────────────────────────

bool tar_header(FILE* f, const std::string& path, size_t size, bool dir) {
  char h[512] = {};
  std::string name = path, prefix;
  if (name.size() > 99) {
    const size_t s = name.rfind('/', name.size() - 2);
    prefix = name.substr(0, s);
    name = name.substr(s + 1);
    if (name.size() > 99 || prefix.size() > 154)
      return false;
  }
  std::memcpy(h, name.data(), name.size());
  std::snprintf(h + 100, 8, "%07o", dir ? 0755 : 0644);
  std::snprintf(h + 108, 8, "%07o", 0);
  std::snprintf(h + 116, 8, "%07o", 0);
  std::snprintf(h + 124, 12, "%011lo", static_cast<unsigned long>(size));
  std::snprintf(h + 136, 12, "%011lo", static_cast<unsigned long>(std::time(nullptr)));
  h[156] = dir ? '5' : '0';
  std::memcpy(h + 257, "ustar", 6);
  std::memcpy(h + 263, "00", 2);
  std::memcpy(h + 345, prefix.data(), prefix.size());
  std::memset(h + 148, ' ', 8);
  unsigned sum = 0;
  for (unsigned char c : h)
    sum += c;
  std::snprintf(h + 148, 8, "%06o", sum);
  h[155] = ' ';
  return std::fwrite(h, 1, 512, f) == 512;
}

// Every file of `dir` as "<prefix>/<name>", after a header for the directory.
bool tar_dir(FILE* f, const fs::path& dir, const std::string& prefix) {
  static const char zeros[512] = {};
  if (!tar_header(f, prefix + "/", 0, true))
    return false;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(dir))
    files.push_back(e.path());
  std::sort(files.begin(), files.end());
  for (const auto& p : files) {
    std::ifstream in(p, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t pad = (512 - data.size() % 512) % 512;
    if (!tar_header(f, prefix + "/" + p.filename().string(), data.size(), false) ||
        std::fwrite(data.data(), 1, data.size(), f) != data.size() || std::fwrite(zeros, 1, pad, f) != pad)
      return false;
  }
  return true;
}

// Books under .incoming/<dir>/, EPUBs straight into their backup folder.
bool write_tar(FILE* f, const fs::path& out, const std::vector<std::string>& names, const fs::path& epubs) {
  static const char zeros[1024] = {};
  for (const auto& n : names)
    if (!tar_dir(f, out / n, ".incoming/" + n))
      return false;
  return tar_dir(f, epubs, kEpubDir) && std::fwrite(zeros, 1, 1024, f) == 1024;
}

}  // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  const char* home = std::getenv("HOME");
  const fs::path src = fs::path(home ? home : "") / "Downloads" / "books";

  std::vector<fs::path> sources;
  {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(src, ec)) {
      const std::string ext = e.path().extension().string();
      if (ext == ".epub" || ext == ".EPUB")
        sources.push_back(e.path());
    }
    std::sort(sources.begin(), sources.end());
  }

  const char* tmpdir = std::getenv("TMPDIR");
  std::string tmpl = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/books.XXXXXX";
  if (!mkdtemp(tmpl.data())) {
    std::perror("mkdtemp");
    return 1;
  }
  g_tmp = tmpl;
  std::atexit(cleanup);
  const fs::path out = g_tmp / "out";
  const fs::path epubs = g_tmp / "epubs";
  fs::create_directories(out);
  fs::create_directories(epubs);

  std::vector<fs::path> done;
  for (const auto& e : sources) {
    std::printf("\n== %s\n", e.filename().c_str());
    if (process(e, out, epubs))
      done.push_back(e);
  }

  std::vector<std::string> names;
  for (const auto& d : fs::directory_iterator(out))
    names.push_back(d.path().filename().string());
  std::sort(names.begin(), names.end());
  if (names.empty())
    return 0;

  // Books staged in .incoming (dot dirs are never listed by booksync.js), then
  // moved into place, so the reader never syncs a half-uploaded book. EPUB
  // names never appear in the command, only in the tar stream. The remote
  // login shell may be fish: only syntax valid in both (&&, single quotes).
  const std::string r = kRemote;
  const std::string e = r + "/" + kEpubDir;
  std::string cmd = "mkdir -p " + r + "/.incoming " + e + " && tar -C " + r + " -xf - && chmod -R a+rX " + r +
                    "/.incoming " + e;
  for (const auto& n : names)
    cmd += " && rm -rf '" + r + "/" + n + "' && mv '" + r + "/.incoming/" + n + "' '" + r + "/" + n + "'";
  std::printf("\nuploading %zu book(s) to %s\n", names.size(), kHost);
  std::fflush(stdout);
  FILE* ssh = popen((std::string("ssh ") + kHost + " \"" + cmd + "\"").c_str(), "w");
  bool ok = ssh && write_tar(ssh, out, names, epubs);
  if (ssh)
    ok = pclose(ssh) == 0 && ok;
  if (!ok) {
    std::fprintf(stderr, "upload failed, nothing deleted\n");
    return 1;
  }
  std::error_code ec;
  for (const auto& d : done)
    fs::remove(d, ec);
  fs::remove(src, ec);  // only if now empty
  std::printf("done\n");
  return 0;
}
