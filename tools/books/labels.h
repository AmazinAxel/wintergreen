#pragma once

// Chapter-label heuristics for the review: which entries are kept by default.
// Labels are shown as the EPUB has them; the device numbers kept chapters by
// position and never sees a name. Pure string functions;
// tools/tests/labels_test.cpp covers them.

#include <string>
#include <string_view>

namespace books {

// Collapse whitespace runs to one space and trim; nothing else.
std::string tidy(std::string_view s);

// Unnumbered sections (front/back matter, parts, letters), and the subset
// dropped by default.
bool is_matter(std::string_view label);
bool is_junk(std::string_view label);
bool is_contents(std::string_view label);

// Lower-cased letters and digits only; how a label is matched against the title.
std::string key(std::string_view s);

// Directory name on the homelab: must pass booksync.js safeName().
std::string slug(std::string_view s);

}  // namespace books
