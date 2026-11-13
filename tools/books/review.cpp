#include "review.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "labels.h"
#include "miniz.h"
#include "reader_font.h"
#include "wintergreen/content/Book.h"
#include "wintergreen/content/wgb/WgbReader.h"

namespace fs = std::filesystem;

namespace books {

namespace {

mz_zip_archive* Z(void* p) {
  return static_cast<mz_zip_archive*>(p);
}

std::string dir_of(const std::string& path) {
  const size_t s = path.rfind('/');
  return s == std::string::npos ? "" : path.substr(0, s);
}

std::string fragment_of(const std::string& href) {
  const size_t h = href.find('#');
  return h == std::string::npos ? "" : href.substr(h + 1);
}

int hex(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

void descendants(const Node& n, std::vector<const Node*>& out) {
  for (const auto& k : n.kids) {
    if (k->is_text)
      continue;
    out.push_back(k.get());
    descendants(*k, out);
  }
}

const Node* first_named(const Node& n, const char* name) {
  std::vector<const Node*> all;
  descendants(n, all);
  for (const Node* e : all)
    if (e->name == name)
      return e;
  return nullptr;
}

const Node* first_child(const Node& n, const char* name) {
  for (const Node* e : n.elements())
    if (e->name == name)
      return e;
  return nullptr;
}

int ancestors_named(const Node& n, const char* name) {
  int c = 0;
  for (const Node* p = n.parent; p; p = p->parent)
    c += p->name == name;
  return c;
}

bool is_heading(const Node& n, char max_level) {
  return n.name.size() == 2 && n.name[0] == 'h' && n.name[1] >= '1' && n.name[1] <= max_level;
}

std::string qname_of(const std::string& src, const Node& n) {
  size_t a = n.start + 1, b = a;
  while (b < n.tag_end && src[b] != ' ' && src[b] != '\t' && src[b] != '\n' && src[b] != '\r' && src[b] != '/' &&
         src[b] != '>')
    ++b;
  return src.substr(a, b - a);
}

}  // namespace

std::string resolve(const std::string& base, const std::string& href) {
  std::string rel = href.substr(0, href.find('#'));
  std::string dec;
  for (size_t i = 0; i < rel.size(); ++i) {
    if (rel[i] == '%' && i + 2 < rel.size() && hex(rel[i + 1]) >= 0 && hex(rel[i + 2]) >= 0) {
      dec += static_cast<char>(hex(rel[i + 1]) * 16 + hex(rel[i + 2]));
      i += 2;
    } else {
      dec += rel[i];
    }
  }
  std::vector<std::string> parts;
  std::stringstream ss(base + "/" + dec);
  for (std::string p; std::getline(ss, p, '/');) {
    if (p.empty() || p == ".")
      continue;
    if (p == "..") {
      if (!parts.empty())
        parts.pop_back();
      continue;
    }
    parts.push_back(p);
  }
  std::string out;
  for (const auto& p : parts)
    out += (out.empty() ? "" : "/") + p;
  return out;
}

Review::~Review() {
  if (zip_) {
    mz_zip_reader_end(Z(zip_));
    delete Z(zip_);
  }
}

bool Review::read_entry(const std::string& name, std::string& out) const {
  const int idx = mz_zip_reader_locate_file(Z(zip_), name.c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
  if (idx < 0)
    return false;
  size_t n = 0;
  void* p = mz_zip_reader_extract_to_heap(Z(zip_), static_cast<mz_uint>(idx), &n, 0);
  if (!p)
    return false;
  out.assign(static_cast<const char*>(p), n);
  mz_free(p);
  return true;
}

const Doc* Review::doc(const std::string& path) const {
  for (const auto& [p, d] : docs_)
    if (p == path)
      return d.get();
  std::string src;
  if (!read_entry(path, src))
    return nullptr;
  docs_.emplace_back(path, parse(src));
  return docs_.back().second.get();
}

std::string Review::open(const std::string& epub_path) {
  epub_path_ = epub_path;
  zip_ = new mz_zip_archive{};
  if (!mz_zip_reader_init_file(Z(zip_), epub_path.c_str(), 0)) {
    delete Z(zip_);
    zip_ = nullptr;
    return "not a zip";
  }
  const Doc* container = doc("META-INF/container.xml");
  const Node* rootfile = container ? first_named(container->root, "rootfile") : nullptr;
  const std::string* full = rootfile ? rootfile->attr("full-path") : nullptr;
  if (!full || !read_entry(*full, opf_src_))
    return "no OPF";
  opf_path_ = *full;
  opf_dir_ = dir_of(opf_path_);
  if (!opf_dir_.empty())
    opf_dir_ += '/';
  opf_ = parse(opf_src_);

  if (const Node* man = first_named(opf_->root, "manifest")) {
    std::vector<const Node*> all;
    descendants(*man, all);
    for (const Node* e : all) {
      if (e->name != "item")
        continue;
      auto get = [&](const char* a) { return e->attr(a) ? *e->attr(a) : std::string(); };
      manifest_.push_back({get("id"), resolve(dir_of(opf_path_), get("href")), get("media-type"), get("properties")});
    }
  }

  // The spine as the converter sees it, so a spine index here is a spine index
  // in the bake.
  wintergreen::Book book;
  if (book.open(epub_path.c_str()) != wintergreen::EpubError::Ok)
    return "the converter can't open it";
  for (const auto& s : book.epub().spine())
    spine_paths_.emplace_back(book.epub().zip().entry(s.file_idx).name);
  if (spine_paths_.empty())
    return "empty spine";

  if (const Node* meta = first_named(opf_->root, "metadata")) {
    if (const Node* t = first_named(*meta, "title"))
      title = t->text_content();
    if (const Node* c = first_named(*meta, "creator"))
      author = c->text_content();
  }
  return {};
}

std::vector<Review::Raw> Review::raw_entries(bool& reviewed, bool fresh) const {
  std::vector<Raw> out;
  reviewed = false;
  auto add = [&](int depth, const std::string& label, const std::string& href, const std::string& base, bool drop) {
    if (href.empty())
      return;
    out.push_back({depth, label, resolve(base, href), fragment_of(href), drop});
  };

  // The NCX to read: the manifest's, unless `fresh` asks for the book's own
  // TOC behind our wg.ncx. The review drops the original NCX from the manifest
  // but leaves the file in the zip, so it is found by name.
  std::string ncx_path;
  for (const Item& it : manifest_) {
    if (it.type != "application/x-dtbncx+xml")
      continue;
    if (!fresh || it.id != "wgncx") {
      ncx_path = it.path;
      reviewed = it.id == "wgncx";
    }
    break;
  }
  if (fresh && ncx_path.empty()) {
    const mz_uint n = mz_zip_reader_get_num_files(Z(zip_));
    for (mz_uint i = 0; i < n && ncx_path.empty(); ++i) {
      char name[1024];
      mz_zip_reader_get_filename(Z(zip_), i, name, sizeof name);
      const std::string s = name;
      if (s.size() > 4 && s.compare(s.size() - 4, 4, ".ncx") == 0 && s != opf_dir_ + "wg.ncx")
        ncx_path = s;
    }
  }
  if (const Doc* d = ncx_path.empty() ? nullptr : doc(ncx_path)) {
    for (const Node* e : d->all()) {
      if (e->name != "navpoint")
        continue;
      const Node* lab = first_child(*e, "navlabel");
      const Node* content = first_child(*e, "content");
      const std::string* src = content ? content->attr("src") : nullptr;
      const std::string* cls = e->attr("class");
      add(ancestors_named(*e, "navpoint"), lab ? lab->text_content() : "", src ? *src : "", dir_of(ncx_path),
          cls && cls->find("wgdrop") != std::string::npos);
    }
  }
  if (out.empty()) {
    reviewed = false;
    for (const Item& it : manifest_) {
      if (it.props.find("nav") == std::string::npos)
        continue;
      if (const Doc* d = doc(it.path)) {
        for (const Node* nav : d->all()) {
          const std::string* type = nav->attr("type");
          if (nav->name != "nav" || !type || (" " + *type + " ").find(" toc ") == std::string::npos)
            continue;
          std::vector<const Node*> all;
          descendants(*nav, all);
          for (const Node* a : all) {
            if (a->name != "a")
              continue;
            const std::string* href = a->attr("href");
            add(ancestors_named(*a, "li") - 1, a->text_content(), href ? *href : "", dir_of(it.path), false);
          }
        }
      }
      break;
    }
  }
  if (!reviewed && out.size() < 3 && spine_paths_.size() > 3) {
    out.clear();
    for (const std::string& p : spine_paths_) {
      std::string label;
      if (const Doc* d = doc(p)) {
        for (const Node* e : d->all()) {
          if (is_heading(*e, '3')) {
            label = e->text_content();
            break;
          }
        }
      }
      out.push_back({0, label, p, "", false});
    }
  }
  return out;
}


void Review::propose_chapters(bool fresh) {
  chapters.clear();
  notes.clear();
  bool reviewed = false;
  const std::vector<Raw> raw = raw_entries(reviewed, fresh);

  // Onto the spine, in reading order; untitled entries merge into the previous.
  struct T {
    std::string label;
    uint16_t sp;
    std::string frag;
    int depth;
    bool dropped;
    int order;  // document position of the anchor; -1 = file start
  };
  std::vector<T> t;
  size_t last = 0;
  for (const Raw& r : raw) {
    const auto it = std::find(spine_paths_.begin(), spine_paths_.end(), r.path);
    if (it == spine_paths_.end())
      continue;
    const size_t s = static_cast<size_t>(it - spine_paths_.begin());
    if (s < last)
      continue;
    std::string lab = tidy(r.label);
    if (lab.empty())
      continue;
    last = s;
    t.push_back({lab, static_cast<uint16_t>(s), r.frag, r.depth, r.dropped, -1});
  }

  // An anchor the converter will never see would make its chapter start
  // nowhere; it starts at its file's first byte instead. Within a file,
  // entries go in document order, which is the order the converter meets them.
  //
  // An id on <html> or <body> is the file's first byte, and the converter's
  // parser never reports those ids (calibre books put every chapter anchor on
  // <body>), so it is cleared rather than kept.
  for (T& e : t) {
    if (e.frag.empty())
      continue;
    const Doc* d = doc(spine_paths_[e.sp]);
    int pos = 0;
    bool at_top = false;
    if (d) {
      for (const Node* n : d->all()) {
        ++pos;
        const std::string* id = n->attr("id");
        if (id && *id == e.frag) {
          at_top = n->name == "html" || n->name == "body";
          e.order = at_top ? -1 : pos;
          break;
        }
      }
    }
    if (at_top) {
      e.frag.clear();
    } else if (e.order < 0) {
      notes.push_back("anchor #" + e.frag + " not in " + spine_paths_[e.sp] + ": \"" + e.label +
                      "\" starts at the top of that file");
      e.frag.clear();
    }
  }
  std::stable_sort(t.begin(), t.end(), [](const T& a, const T& b) {
    return a.sp != b.sp ? a.sp < b.sp : a.order < b.order;
  });

  if (t.empty() || t[0].sp > 0)
    chapters.push_back({0, "", kFrontLabel, false});  // dropped by default; toggle to keep

  if (reviewed) {
    // Our own wg.ncx: keep its entries and drop marks as they are.
    for (const T& e : t)
      chapters.push_back({e.sp, e.frag, e.label, !e.dropped});
    return;
  }

  // Everything before a Contents entry is title-page material, and so is any
  // entry repeating the book's title. Chapter depth = shallowest non-matter
  // entry; anything deeper is a sub-section and left out.
  int contents = -1;
  for (size_t j = 0; j < t.size(); ++j) {
    if (is_contents(t[j].label)) {
      contents = static_cast<int>(j);
      break;
    }
  }
  const std::string tkey = key(title);
  std::vector<bool> matter(t.size());
  for (size_t j = 0; j < t.size(); ++j)
    matter[j] = static_cast<int>(j) < contents || is_matter(t[j].label) ||
                (!tkey.empty() && key(t[j].label).find(tkey) != std::string::npos);

  // Entries nested under matter count one level up per matter ancestor: the
  // stories under a "Stories of Your Life and Others" title entry, or chapters
  // under "Part One", sit at the depth of top-level "Story Notes".
  std::vector<int> depth(t.size());
  std::vector<std::pair<int, bool>> stack;  // (raw depth, matter) of open ancestors
  for (size_t j = 0; j < t.size(); ++j) {
    while (!stack.empty() && stack.back().first >= t[j].depth)
      stack.pop_back();
    int up = 0;
    for (const auto& a : stack)
      up += a.second;
    depth[j] = t[j].depth - up;
    stack.emplace_back(t[j].depth, matter[j]);
  }
  int cdepth = 99;
  for (size_t j = 0; j < t.size(); ++j)
    if (!matter[j])
      cdepth = std::min(cdepth, depth[j]);

  for (size_t j = 0; j < t.size(); ++j) {
    if (depth[j] > cdepth)
      continue;
    Chapter c{t[j].sp, t[j].frag, t[j].label, true};
    if (matter[j])
      c.keep = !is_junk(c.label);
    chapters.push_back(std::move(c));
  }
}

std::string Review::ncx() const {
  std::string up;
  for (char ch : opf_dir_)
    if (ch == '/')
      up += "../";
  std::string out =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><head/><docTitle><text>" +
      xml_escape(title) + "</text></docTitle><navMap>\n";
  int k = 0;
  for (const Chapter& c : chapters) {
    std::string p = spine_paths_[c.spine];
    if (!opf_dir_.empty() && p.compare(0, opf_dir_.size(), opf_dir_) == 0)
      p = p.substr(opf_dir_.size());
    else
      p = up + p;
    if (!c.fragment.empty())
      p += "#" + c.fragment;
    ++k;
    // Dropped entries stay listed, marked, so a re-review shows them dropped
    // and can bring them back.
    out += "<navPoint id=\"p" + std::to_string(k) + "\" playOrder=\"" + std::to_string(k) + "\"" +
           (c.keep ? "" : " class=\"wgdrop\"") + "><navLabel><text>" + xml_escape(c.label) +
           "</text></navLabel><content src=\"" + xml_escape(p) + "\"/></navPoint>\n";
  }
  return out + "</navMap></ncx>\n";
}

std::string Review::opf_with_edits(bool with_chapters) const {
  struct Edit {
    size_t a, b;
    std::string text;
    int seq;
  };
  std::vector<Edit> ed;
  auto edit = [&](size_t a, size_t b, std::string s) { ed.push_back({a, b, std::move(s), static_cast<int>(ed.size())}); };

  const Node* meta = first_named(opf_->root, "metadata");
  const Node* man = first_named(opf_->root, "manifest");
  const Node* spine = first_named(opf_->root, "spine");
  if (!meta || !man)
    return {};

  auto set_text = [&](const char* local, const char* qname, const std::string& value) {
    const Node* n = first_named(*meta, local);
    if (n && n->end != n->tag_end)
      edit(n->tag_end, n->close_at, xml_escape(value));
    else if (n)
      edit(n->start, n->end, std::string("<") + qname + ">" + xml_escape(value) + "</" + qname + ">");
    else
      edit(meta->close_at, meta->close_at, std::string("<") + qname + ">" + xml_escape(value) + "</" + qname + ">");
  };
  set_text("title", "dc:title", title);
  set_text("creator", "dc:creator", author);

  std::vector<const Node*> items, metas;
  descendants(*man, items);
  descendants(*meta, metas);
  if (with_chapters) {
    for (const Node* it : items) {
      if (it->name != "item")
        continue;
      const std::string* type = it->attr("media-type");
      const std::string* id = it->attr("id");
      if ((type && *type == "application/x-dtbncx+xml") || (id && *id == "wgncx"))
        edit(it->start, it->end, "");
    }
    edit(man->close_at, man->close_at,
         "<item id=\"wgncx\" href=\"wg.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n");
    if (spine) {
      std::string tag = "<" + qname_of(opf_src_, *spine);
      for (const auto& [k, v] : spine->attrs) {
        const size_t colon = k.find(':');
        if ((colon == std::string::npos ? k : k.substr(colon + 1)) != "toc")
          tag += " " + k + "=\"" + xml_escape(v) + "\"";
      }
      tag += " toc=\"wgncx\"";
      tag += spine->end == spine->tag_end ? "/>" : ">";
      edit(spine->start, spine->tag_end, tag);
    }
  }
  if (!new_cover.empty()) {
    for (const Node* m : metas) {
      const std::string* name = m->attr("name");
      if (m->name == "meta" && name && *name == "cover")
        edit(m->start, m->end, "");
    }
    for (const Node* it : items) {
      const std::string* id = it->attr("id");
      if (it->name == "item" && id && *id == "wgcover")
        edit(it->start, it->end, "");
    }
    std::string ext = fs::path(new_cover).extension().string();
    for (char& c : ext)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool jpg = ext == ".jpg" || ext == ".jpeg";
    edit(man->close_at, man->close_at,
         std::string("<item id=\"wgcover\" href=\"wgcover") + (jpg ? ".jpg" : ".png") + "\" media-type=\"" +
             (jpg ? "image/jpeg" : "image/png") + "\" properties=\"cover-image\"/>\n");
    edit(meta->close_at, meta->close_at, "<meta name=\"cover\" content=\"wgcover\"/>\n");
  }

  // Back to front, so earlier offsets stay valid; inserts at one offset keep
  // the order they were added in.
  std::sort(ed.begin(), ed.end(), [](const Edit& x, const Edit& y) { return x.a != y.a ? x.a > y.a : x.seq > y.seq; });
  std::string out = opf_src_;
  for (const Edit& e : ed)
    out.replace(e.a, e.b - e.a, e.text);
  return out;
}

bool Review::write_epub(const std::string& out_path, bool with_chapters) const {
  const std::string opf = opf_with_edits(with_chapters);
  if (opf.empty())
    return false;
  std::string cover_bytes;
  if (!new_cover.empty()) {
    std::ifstream f(new_cover, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    cover_bytes = ss.str();
    if (cover_bytes.empty())
      return false;
  }
  std::string cover_name;
  if (!new_cover.empty()) {
    std::string ext = fs::path(new_cover).extension().string();
    for (char& c : ext)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    cover_name = opf_dir_ + "wgcover" + ((ext == ".jpg" || ext == ".jpeg") ? ".jpg" : ".png");
  }

  mz_zip_archive w{};
  std::error_code ec;
  fs::remove(out_path, ec);
  if (!mz_zip_writer_init_file(&w, out_path.c_str(), 0))
    return false;
  bool ok = true;
  const mz_uint n = mz_zip_reader_get_num_files(Z(zip_));
  // mimetype first and stored, as the EPUB container requires.
  const int mt = mz_zip_reader_locate_file(Z(zip_), "mimetype", nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
  if (mt >= 0)
    ok = ok && mz_zip_writer_add_from_zip_reader(&w, Z(zip_), static_cast<mz_uint>(mt));
  for (mz_uint i = 0; ok && i < n; ++i) {
    if (static_cast<int>(i) == mt)
      continue;
    char name[1024];
    mz_zip_reader_get_filename(Z(zip_), i, name, sizeof name);
    const std::string nm = name;
    if (nm == opf_path_) {
      ok = mz_zip_writer_add_mem(&w, name, opf.data(), opf.size(), MZ_DEFAULT_LEVEL);
      continue;
    }
    if (with_chapters && nm == opf_dir_ + "wg.ncx")
      continue;
    if (!new_cover.empty() && nm.compare(0, opf_dir_.size() + 8, opf_dir_ + "wgcover.") == 0)
      continue;
    ok = mz_zip_writer_add_from_zip_reader(&w, Z(zip_), i);
  }
  if (ok && with_chapters) {
    const std::string x = ncx();
    ok = mz_zip_writer_add_mem(&w, (opf_dir_ + "wg.ncx").c_str(), x.data(), x.size(), MZ_DEFAULT_LEVEL);
  }
  if (ok && !new_cover.empty())
    ok = mz_zip_writer_add_mem(&w, cover_name.c_str(), cover_bytes.data(), cover_bytes.size(), 0);
  ok = mz_zip_writer_finalize_archive(&w) && ok;
  mz_zip_writer_end(&w);
  return ok;
}

std::string Review::bake(const std::string& epub_path, const std::string& out_dir, std::string& summary) const {
  const std::string name = slug(title);
  const fs::path dir = fs::path(out_dir) / name;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  const std::string wgb = (dir / "book.wgb").string();

  wintergreen::Book book;
  if (book.open(epub_path.c_str()) != wintergreen::EpubError::Ok || book.chapter_count() != spine_paths_.size()) {
    summary = "reviewed EPUB does not reopen with the same spine";
    return {};
  }
  std::vector<wintergreen::PlannedChapter> plan;
  for (const Chapter& c : chapters)
    plan.push_back({c.spine, c.fragment, c.keep});
  const wintergreen::BitmapFontSet* fonts = reader_fonts();
  if (!fonts) {
    summary = "embedded reader font does not load";
    return {};
  }
  std::string err;
  if (!wintergreen::convert_epub_to_wgb_streaming(book, wgb.c_str(), plan, *fonts, err)) {
    summary = err.empty() ? "conversion failed" : "conversion failed: " + err;
    return {};
  }
  book.close();
  wintergreen::WgbReader reader;
  if (!reader.open(wgb.c_str())) {
    summary = "output does not reopen";
    return {};
  }
  // Every kept chapter must land in the start table, or the device's numbers
  // stop matching the review. One that does not had a start the parser never
  // reported, and the book would be missing it silently.
  const size_t kept = static_cast<size_t>(
      std::count_if(chapters.begin(), chapters.end(), [](const Chapter& c) { return c.keep; }));
  const size_t found = reader.chapter_count();
  if (found != kept) {
    summary = std::to_string(kept - found) + " of " + std::to_string(kept) + " kept chapters never found their start";
    return {};
  }
  char buf[200];
  std::snprintf(buf, sizeof buf, "%u chapters, %u images, %u/%u pages, %.1f KB", static_cast<unsigned>(found),
                static_cast<unsigned>(reader.image_count()), static_cast<unsigned>(reader.page_count(0)),
                static_cast<unsigned>(reader.page_count(1)), static_cast<double>(fs::file_size(wgb, ec)) / 1024.0);
  summary = buf;
  reader.close();
  return name;
}

}  // namespace books
