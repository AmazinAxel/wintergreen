#pragma once

// One book under review: what `books` reads out of the EPUB, the chapter list it
// proposes, and the two outputs — the reviewed EPUB (written back over the
// original) and the baked book directory that goes to the homelab.

#include <memory>
#include <string>
#include <vector>

#include "dom.h"
#include "wintergreen/content/wgb/WgbConverter.h"

namespace books {

struct Chapter {
  uint16_t spine = 0;    // spine index
  std::string fragment;  // id it starts at; empty = the file's first byte
  std::string label;
  bool keep = true;
};

inline constexpr const char* kFrontLabel = "(front pages)";

class Review {
 public:
  ~Review();

  // Reads container, OPF, spine (through the converter's own parser, so spine
  // indices match the bake) and the current cover. Empty string = ok;
  // otherwise why the book is skipped.
  std::string open(const std::string& epub_path);

  std::string title, author;
  std::string new_cover;  // replacement cover image (jpg/png path), empty = keep

  // Build the proposed chapter list from NCX → EPUB3 nav → h1-h3 per spine
  // file, with the labels.h heuristics. Uses the current `title`.
  // `fresh` ignores a previous review's wg.ncx and proposes from the book's
  // own TOC again (the "reset" command).
  void propose_chapters(bool fresh = false);
  std::vector<Chapter> chapters;
  // Lines worth telling the user about (anchors that were not found).
  std::vector<std::string> notes;

  // Write the reviewed EPUB. With chapters = false only the cover edit is
  // applied (for previews).
  bool write_epub(const std::string& out_path, bool with_chapters) const;

  // Bake `epub_path` (a reviewed EPUB written by write_epub) into
  // `<out_dir>/<slug>/`. Returns the directory name, empty on failure, and
  // a one-line summary in `summary`.
  std::string bake(const std::string& epub_path, const std::string& out_dir, std::string& summary) const;

  size_t spine_size() const { return spine_paths_.size(); }

 private:
  struct Item {
    std::string id, path, type, props;
  };
  struct Raw {
    int depth;
    std::string label, path, frag;
    bool dropped;
  };

  std::string epub_path_;
  void* zip_ = nullptr;  // mz_zip_archive
  std::string opf_path_, opf_dir_;  // opf_dir_ "" or "OEBPS/" (with slash)
  std::string opf_src_;
  std::unique_ptr<Doc> opf_;
  std::vector<Item> manifest_;
  std::vector<std::string> spine_paths_;
  mutable std::vector<std::pair<std::string, std::unique_ptr<Doc>>> docs_;

  bool read_entry(const std::string& name, std::string& out) const;
  const Doc* doc(const std::string& path) const;
  std::vector<Raw> raw_entries(bool& reviewed, bool fresh) const;
  std::string opf_with_edits(bool with_chapters) const;
  std::string ncx() const;
};

// "OEBPS/x/../y.xhtml#frag" style resolution: href (URL-encoded, maybe with a
// fragment) relative to zip dir `base` ("" = root) → zip entry name.
std::string resolve(const std::string& base, const std::string& href);

}  // namespace books
