#!/usr/bin/env bash
#
# tools/tests/run.sh [book.wgb | book.epub | dir ...]
#
# Builds and runs every host test under ASan/UBSan. The unit tests always run;
# the book test (baked page section) runs over whatever books are given:
# .wgb files directly, .epub files after baking them with a sanitized build of
# the `books` pipeline (tools/books, proposed chapters, no prompts; so the
# review and converter are exercised too), directories searched for both.
#
# Needs g++ (and cmake for .epub). On this machine:
#   nix shell nixpkgs#gcc nixpkgs#cmake nixpkgs#gnumake -c tools/tests/run.sh ~/books

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
CXX="g++ -std=c++17 -g -O1 $SAN -I . -I wintergreen -I third_party/miniz"
LAYOUT="wintergreen/content/TextLayout.cpp wintergreen/content/hyphenation/Hyphenation.cpp"
READER="wintergreen/content/wgb/WgbReader.cpp"
BAKE="wintergreen/content/wgb/WgbPaginator.cpp wintergreen/content/wgb/WgbParagraphs.cpp $READER"
# The device's own reader font, embedded exactly as `books` embeds it.
FONT="-I tools/books -I third_party/miniz -DWG_READER_FONT=\"$PWD/resources/Libron.bin\" tools/books/reader_font.cpp"
MINIZ="third_party/miniz/miniz.c third_party/miniz/miniz_tinfl.c third_party/miniz/miniz_tdef.c"
DECODE="wintergreen/content/ImageDecoder.cpp wintergreen/content/JpegDecoder.cpp
        wintergreen/content/PngDecoder.cpp wintergreen/content/ZipReader.cpp third_party/miniz/miniz*.c"

fail=0
unit() { # unit <name> <sources...>
  local name=$1; shift
  if $CXX -o "$WORK/$name" "tools/tests/$name.cpp" "$@" && "$WORK/$name" >/dev/null; then
    echo "  ok   $name"
  else
    echo "  FAIL $name"; fail=1
  fi
}

echo "unit tests:"
unit json_scan_test
unit blank_para_test $LAYOUT
unit book_index_test wintergreen/content/BookIndex.cpp wintergreen/content/CoverPaths.cpp $READER
# shellcheck disable=SC2086
unit stretch_lut_test $DECODE
unit blit_test wintergreen/display/DrawBuffer.cpp
# shellcheck disable=SC2086
unit baked_line_test wintergreen/display/DrawBuffer.cpp $BAKE $LAYOUT $FONT -x c $MINIZ -x none
CXX="$CXX -I tools/books" unit labels_test tools/books/labels.cpp wintergreen/content/HtmlEntities.cpp

books=()
epubs=()
for arg in "$@"; do
  if [[ -d $arg ]]; then
    while IFS= read -r -d '' f; do books+=("$f"); done < <(find "$arg" -name '*.wgb' -print0)
    while IFS= read -r -d '' f; do epubs+=("$f"); done < <(find "$arg" -name '*.epub' -print0)
  elif [[ $arg == *.epub ]]; then
    epubs+=("$arg")
  else
    books+=("$arg")
  fi
done

if ((${#epubs[@]})); then
  echo "baking ${#epubs[@]} epub(s) with a sanitized books pipeline:"
  cmake -S tools/books -B "$WORK/books-build" -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="$SAN" -DCMAKE_C_FLAGS="-fsanitize=address" \
    -DCMAKE_EXE_LINKER_FLAGS="$SAN" >/dev/null
  cmake --build "$WORK/books-build" --target convert_epubs -j "$(nproc)" >/dev/null
  abs=()
  for e in "${epubs[@]}"; do abs+=("$(realpath "$e")"); done
  mkdir -p "$WORK/books"
  if (cd "$WORK/books" && "$WORK/books-build/convert_epubs" "${abs[@]}") >"$WORK/convert.log" 2>&1; then
    echo "  ok   convert_epubs"
  else
    echo "  FAIL convert_epubs"; sed 's/^/       /' "$WORK/convert.log" | tail -20; fail=1
  fi
  while IFS= read -r -d '' f; do books+=("$f"); done < <(find "$WORK/books" -name '*.wgb' -print0 2>/dev/null)
fi

if ((${#books[@]})); then
  echo "book tests:"
  # shellcheck disable=SC2086
  $CXX -o "$WORK/paginate" tools/tests/paginate_book_test.cpp $READER
  for b in "${books[@]}"; do
    name=$(basename "$(dirname "$b")")
    if out=$("$WORK/paginate" "$b" 2>&1); then
      echo "  ok   paginate $name: $(echo "$out" | tail -1)"
    else
      echo "  FAIL paginate $name"; echo "$out" | tail -10 | sed 's/^/       /'; fail=1
    fi
  done
else
  echo "no books given: page-section test skipped"
fi

((fail == 0)) && echo PASS || { echo FAIL; exit 1; }
