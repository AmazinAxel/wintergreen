#pragma once

// Tolerant XML/XHTML tree for the files `books` reads: container.xml, the OPF,
// NCX, EPUB3 nav and chapter XHTML. Not a validating parser: unknown entities
// stay literal, a stray end tag closes back to its matching open element (or is
// ignored), and anything still open at EOF is closed there. Every element keeps
// the byte range of its start tag and of the whole element, which is what the
// OPF rewrite edits by.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace books {

struct Node {
  bool is_text = false;
  std::string name;  // local name, lower-cased for matching; empty for text
  std::vector<std::pair<std::string, std::string>> attrs;  // qualified name as written, decoded value
  std::string text;  // decoded text, for text nodes
  std::vector<std::unique_ptr<Node>> kids;
  Node* parent = nullptr;
  size_t start = 0;      // offset of '<'
  size_t tag_end = 0;    // offset just past the start tag's '>'
  size_t end = 0;        // offset just past the end tag (== tag_end when self-closing)
  size_t close_at = 0;   // offset of the end tag's '<' (== end when there is none)

  // Attribute by local name (prefix ignored: "epub:type" matches "type").
  const std::string* attr(std::string_view local) const;
  // Whitespace-collapsed text of the whole subtree.
  std::string text_content() const;
  std::vector<const Node*> elements() const;  // element children only
};

struct Doc {
  Node root;
  // Every element in document order.
  std::vector<const Node*> all() const;

  Doc() = default;
  // Children point at their parent, root included: never moved.
  Doc(const Doc&) = delete;
  Doc& operator=(const Doc&) = delete;
};

std::unique_ptr<Doc> parse(std::string_view src);

// Collapse runs of whitespace to one space and trim.
std::string normalize_space(std::string_view s);

std::string xml_escape(std::string_view s);

}  // namespace books
