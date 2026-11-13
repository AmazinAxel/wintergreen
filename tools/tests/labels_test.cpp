// Chapter-label heuristics (tools/books/labels.cpp): the cases the original
// fish implementation was tested against (default keep/drop, title matching,
// directory names). Labels themselves are kept raw; tidy only collapses space.

#include <cassert>
#include <cstdio>
#include <string>

#include "labels.h"

using namespace books;

int main() {
  assert(tidy("  THE  RED\n  ROOM ") == "THE RED ROOM");
  assert(tidy("Mixed CASE stays") == "Mixed CASE stays");

  assert(is_matter("Prologue"));
  assert(is_matter("Letter 1"));
  assert(is_matter("Author\xE2\x80\x99s Note"));
  assert(is_matter("The Project Gutenberg eBook of Frankenstein"));
  assert(!is_matter("Down the Rabbit-Hole"));
  assert(is_junk("Contents"));
  assert(!is_junk("Prologue"));
  assert(is_contents("Table of Contents"));

  assert(key("Pride & Prejudice!") == "prideprejudice");
  assert(slug("Pride and Prejudice") == "pride and prejudice");
  assert(slug("  Alice's Adventures: in Wonderland. ") == "alices adventures in wonderland");
  assert(slug("???") == "book");
  assert(slug(std::string(150, 'a')).size() == 100);

  std::puts("labels ok");
  return 0;
}
