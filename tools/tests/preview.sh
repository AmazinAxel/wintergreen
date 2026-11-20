#!/usr/bin/env bash
#
# tools/tests/preview.sh <out dir> <book.epub ...>
#
# Bakes the EPUBs into <out dir>/card (the `books` pipeline, proposed chapters,
# no prompts), drives the real screens over it on a simulated panel
# (tools/tests/preview.cpp) and writes PNGs of what the glass shows to
# <out dir>, plus the simulated-panel font sample. For judging the look before
# flashing. Needs g++, cmake and Pillow:
#
#   nix shell nixpkgs#gcc nixpkgs#cmake nixpkgs#gnumake -c tools/tests/preview.sh /tmp/x book.epub
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
OUT=$(realpath "$1"); shift
mkdir -p "$OUT/card" "$OUT/build"

cmake -S tools/books -B "$OUT/build/books" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$OUT/build/books" --target convert_epubs -j "$(nproc)" >/dev/null
abs=()
for e in "$@"; do abs+=("$(realpath "$e")"); done
(cd "$OUT/card" && "$OUT/build/books/convert_epubs" "${abs[@]}")

SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
MINIZ="third_party/miniz/miniz.c third_party/miniz/miniz_tinfl.c third_party/miniz/miniz_tdef.c"
# shellcheck disable=SC2086
g++ -std=c++17 -g -O1 $SAN -I . -I wintergreen -I tools/books -I third_party/miniz \
  -DWG_READER_FONT="\"$PWD/resources/Libron.bin\"" -o "$OUT/build/preview" tools/tests/preview.cpp \
  wintergreen/Application.cpp wintergreen/screens/*.cpp wintergreen/content/BookIndex.cpp \
  wintergreen/content/CoverPaths.cpp wintergreen/content/wgb/WgbReader.cpp wintergreen/display/DrawBuffer.cpp \
  tools/books/reader_font.cpp -x c $MINIZ -x none
rm -f "$OUT"/*.pgm
"$OUT/build/preview" "$OUT/card" "$OUT"

nix shell --impure --expr 'with import <nixpkgs> {}; python3.withPackages (ps: [ps.pillow ps.freetype-py ps.fonttools])' -c bash -c "
  python3 -c '
import glob, sys
from PIL import Image
for p in sorted(glob.glob(sys.argv[1] + \"/*.pgm\")):
    Image.open(p).save(p[:-4] + \".png\")
' '$OUT' && rm -f '$OUT'/*.pgm &&
  python3 tools/check_font.py resources/Libron.bin --png '$OUT/font-reader.png' \
    'The quick brown fox jumps over the lazy dog; Wave AVATAR “fj” 1984.' >/dev/null &&
  python3 tools/check_font.py wintergreen/display/ui_font_header.h --png '$OUT/font-ui.png' 'Syncing... Hobbit 87%' >/dev/null"
echo "previews in $OUT"
