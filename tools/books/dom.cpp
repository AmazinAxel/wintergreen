#include "dom.h"

#include <cctype>
#include <cstring>

#include "wintergreen/content/HtmlEntities.h"

namespace books {

namespace {

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string local_of(std::string_view qname) {
  const size_t colon = qname.find(':');
  return lower(colon == std::string_view::npos ? qname : qname.substr(colon + 1));
}

// Decode character and entity references; unknown ones stay as written.
std::string decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') {
      out += s[i];
      continue;
    }
    const size_t semi = s.find(';', i + 1);
    if (semi == std::string_view::npos || semi - i > 12) {
      out += '&';
      continue;
    }
    const std::string_view name = s.substr(i + 1, semi - i - 1);
    const auto e = wintergreen::decode_html_entity(name.data(), name.size());
    switch (e.kind) {
      case wintergreen::DecodedEntity::Kind::Unknown:
        out += '&';
        continue;
      case wintergreen::DecodedEntity::Kind::Space:
        out += ' ';
        break;
      case wintergreen::DecodedEntity::Kind::Utf8:
        out += e.utf8;
        break;
      case wintergreen::DecodedEntity::Kind::Codepoint:
        wintergreen::append_utf8(out, e.codepoint);
        break;
    }
    i = semi;
  }
  return out;
}

bool is_void(const std::string& n) {
  static const char* const kVoid[] = {"area", "base", "br", "col", "embed", "hr", "img", "input",
                                      "link", "meta", "param", "source", "track", "wbr"};
  for (const char* v : kVoid)
    if (n == v)
      return true;
  return false;
}

void collect(const Node& n, std::vector<const Node*>& out) {
  for (const auto& k : n.kids) {
    if (k->is_text)
      continue;
    out.push_back(k.get());
    collect(*k, out);
  }
}

void gather_text(const Node& n, std::string& out) {
  for (const auto& k : n.kids) {
    if (k->is_text)
      out += k->text;
    else
      gather_text(*k, out);
  }
}

}  // namespace

const std::string* Node::attr(std::string_view local) const {
  for (const auto& [k, v] : attrs)
    if (local_of(k) == local)
      return &v;
  return nullptr;
}

std::string Node::text_content() const {
  std::string raw;
  gather_text(*this, raw);
  return normalize_space(raw);
}

std::vector<const Node*> Node::elements() const {
  std::vector<const Node*> out;
  for (const auto& k : kids)
    if (!k->is_text)
      out.push_back(k.get());
  return out;
}

std::vector<const Node*> Doc::all() const {
  std::vector<const Node*> out;
  collect(root, out);
  return out;
}

std::string normalize_space(std::string_view s) {
  std::string out;
  bool pending = false;
  for (char c : s) {
    if (is_space(c)) {
      pending = !out.empty();
      continue;
    }
    if (pending)
      out += ' ';
    pending = false;
    out += c;
  }
  return out;
}

std::string xml_escape(std::string_view s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

std::unique_ptr<Doc> parse(std::string_view src) {
  // HTML void elements only make sense in XHTML; an OPF <meta> may have content.
  const bool html = src.find("http://www.w3.org/1999/xhtml") != std::string_view::npos ||
                    src.find("<html") != std::string_view::npos || src.find("<HTML") != std::string_view::npos;
  auto owned = std::make_unique<Doc>();
  Doc& doc = *owned;
  Node* cur = &doc.root;
  doc.root.end = src.size();
  size_t i = 0;
  auto add_text = [&](size_t a, size_t b, bool raw) {
    if (a >= b)
      return;
    auto t = std::make_unique<Node>();
    t->is_text = true;
    t->text = raw ? std::string(src.substr(a, b - a)) : decode(src.substr(a, b - a));
    t->parent = cur;
    t->start = a;
    t->end = t->tag_end = t->close_at = b;
    cur->kids.push_back(std::move(t));
  };
  while (i < src.size()) {
    const size_t lt = src.find('<', i);
    if (lt == std::string_view::npos) {
      add_text(i, src.size(), false);
      break;
    }
    add_text(i, lt, false);
    std::string_view rest = src.substr(lt);
    if (rest.substr(0, 4) == "<!--") {
      const size_t e = src.find("-->", lt + 4);
      i = e == std::string_view::npos ? src.size() : e + 3;
      continue;
    }
    if (rest.substr(0, 9) == "<![CDATA[") {
      const size_t e = src.find("]]>", lt + 9);
      const size_t stop = e == std::string_view::npos ? src.size() : e;
      add_text(lt + 9, stop, true);
      i = e == std::string_view::npos ? src.size() : e + 3;
      continue;
    }
    if (rest.substr(0, 2) == "<!" || rest.substr(0, 2) == "<?") {
      // DOCTYPE (with an optional [internal subset]) or a processing instruction.
      size_t j = lt + 2;
      int bracket = 0;
      while (j < src.size() && !(src[j] == '>' && bracket == 0)) {
        if (src[j] == '[')
          ++bracket;
        else if (src[j] == ']')
          --bracket;
        ++j;
      }
      i = j + 1;
      continue;
    }
    if (rest.substr(0, 2) == "</") {
      const size_t gt = src.find('>', lt);
      const size_t stop = gt == std::string_view::npos ? src.size() : gt;
      size_t a = lt + 2;
      while (a < stop && is_space(src[a]))
        ++a;
      size_t b = a;
      while (b < stop && !is_space(src[b]))
        ++b;
      const std::string name = local_of(src.substr(a, b - a));
      i = gt == std::string_view::npos ? src.size() : gt + 1;
      Node* m = cur;
      while (m != &doc.root && m->name != name)
        m = m->parent;
      if (m == &doc.root)
        continue;  // stray end tag
      for (Node* n = cur; n != m; n = n->parent)
        n->end = n->close_at = lt;  // implicitly closed
      m->close_at = lt;
      m->end = i;
      cur = m->parent;
      continue;
    }
    // Start tag. Find its '>' outside quotes.
    size_t j = lt + 1;
    char q = 0;
    while (j < src.size()) {
      const char c = src[j];
      if (q) {
        if (c == q)
          q = 0;
      } else if (c == '"' || c == '\'') {
        q = c;
      } else if (c == '>') {
        break;
      }
      ++j;
    }
    if (j >= src.size() || j == lt + 1 || is_space(src[lt + 1])) {
      add_text(lt, lt + 1, true);  // a bare '<' in text
      i = lt + 1;
      continue;
    }
    const bool self_close = src[j - 1] == '/';
    const std::string_view body = src.substr(lt + 1, (self_close ? j - 1 : j) - lt - 1);
    auto el = std::make_unique<Node>();
    size_t p = 0;
    while (p < body.size() && !is_space(body[p]))
      ++p;
    el->name = local_of(body.substr(0, p));
    while (p < body.size()) {
      while (p < body.size() && is_space(body[p]))
        ++p;
      const size_t ka = p;
      while (p < body.size() && !is_space(body[p]) && body[p] != '=')
        ++p;
      if (p == ka)
        break;
      std::string key(body.substr(ka, p - ka));
      while (p < body.size() && is_space(body[p]))
        ++p;
      std::string val;
      if (p < body.size() && body[p] == '=') {
        ++p;
        while (p < body.size() && is_space(body[p]))
          ++p;
        if (p < body.size() && (body[p] == '"' || body[p] == '\'')) {
          const char qq = body[p++];
          const size_t va = p;
          while (p < body.size() && body[p] != qq)
            ++p;
          val = decode(body.substr(va, p - va));
          if (p < body.size())
            ++p;
        } else {
          const size_t va = p;
          while (p < body.size() && !is_space(body[p]))
            ++p;
          val = decode(body.substr(va, p - va));
        }
      }
      el->attrs.emplace_back(std::move(key), std::move(val));
    }
    el->parent = cur;
    el->start = lt;
    el->tag_end = j + 1;
    i = j + 1;
    Node* raw = el.get();
    cur->kids.push_back(std::move(el));
    if (self_close || (html && is_void(raw->name))) {
      raw->end = raw->close_at = raw->tag_end;
      continue;
    }
    if (raw->name == "script" || raw->name == "style") {
      // Raw text up to the matching end tag.
      size_t e = i;
      for (;;) {
        e = src.find("</", e);
        if (e == std::string_view::npos || local_of(src.substr(e + 2, raw->name.size())) == raw->name)
          break;
        e += 2;
      }
      const size_t stop = e == std::string_view::npos ? src.size() : e;
      const size_t gt = e == std::string_view::npos ? src.size() : src.find('>', e);
      raw->close_at = stop;
      raw->end = gt == std::string_view::npos ? src.size() : gt + 1;
      i = raw->end;
      continue;
    }
    cur = raw;
  }
  for (Node* n = cur; n != &doc.root; n = n->parent)
    n->end = n->close_at = src.size();
  return owned;
}

}  // namespace books
