#!/usr/bin/env bash
#
# tools/tests/run.sh [book.wgb | book.epub | dir ...]
#
# Builds and runs every host test under ASan/UBSan. The unit tests always run;
# the book tests (pagination, layout memory) run over whatever books are given:
# .wgb files directly, .epub files after converting them with a sanitized
# epub2wgb (so the converter is exercised too), directories searched for both.
#
# Needs g++ (and cmake for .epub). On this machine:
#   nix-shell -p gcc cmake gnumake --run 'tools/tests/run.sh ~/books'

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
CXX="g++ -std=c++17 -g -O1 $SAN -I . -I wintergreen -I third_party/miniz"
LAYOUT="wintergreen/content/TextLayout.cpp wintergreen/content/hyphenation/Hyphenation.cpp"
READER="wintergreen/content/wgb/WgbReader.cpp"
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
unit book_index_test wintergreen/content/BookIndex.cpp $READER
# shellcheck disable=SC2086
unit stretch_lut_test $DECODE

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
  echo "converting ${#epubs[@]} epub(s) with a sanitized epub2wgb:"
  cmake -S tools/epub2wgb -B "$WORK/epub2wgb" -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="$SAN" -DCMAKE_C_FLAGS="-fsanitize=address" \
    -DCMAKE_EXE_LINKER_FLAGS="$SAN" >/dev/null
  cmake --build "$WORK/epub2wgb" -j "$(nproc)" >/dev/null
  if "$WORK/epub2wgb/epub2wgb" -o "$WORK/books" "${epubs[@]}" >"$WORK/convert.log" 2>&1; then
    echo "  ok   epub2wgb"
  else
    echo "  FAIL epub2wgb"; sed 's/^/       /' "$WORK/convert.log" | tail -20; fail=1
  fi
  while IFS= read -r -d '' f; do books+=("$f"); done < <(find "$WORK/books" -name '*.wgb' -print0 2>/dev/null)
fi

if ((${#books[@]})); then
  echo "book tests:"
  # shellcheck disable=SC2086
  $CXX -o "$WORK/paginate" tools/tests/paginate_book_test.cpp $LAYOUT $READER
  # Instrumented operator new: no sanitizer, as the test documents.
  # shellcheck disable=SC2086
  g++ -std=c++17 -O2 -I . -I wintergreen -o "$WORK/layout_mem" tools/tests/layout_memory_test.cpp $LAYOUT $READER
  for b in "${books[@]}"; do
    name=$(basename "$(dirname "$b")")
    for t in paginate layout_mem; do
      if out=$("$WORK/$t" "$b" 2>&1); then
        echo "  ok   $t $name: $(echo "$out" | tail -1)"
      else
        echo "  FAIL $t $name"; echo "$out" | tail -10 | sed 's/^/       /'; fail=1
      fi
    done
  done
else
  echo "no books given: pagination and layout-memory tests skipped"
fi

((fail == 0)) && echo PASS || { echo FAIL; exit 1; }
