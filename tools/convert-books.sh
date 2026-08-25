#!/usr/bin/env bash
#
# convert-books.sh <epub-dir-or-file> [out-dir]
#
# Builds tools/epub2wgb if needed and converts EPUBs into the per-book folders
# the reader expects: <out>/<book name>/book.wgb plus its three covers.
#
# The build is skipped when the binary is already newer than every source it
# depends on. That matters on the machine this usually runs on: converting a
# whole library takes well under a second per book, while a cold compile of the
# converter is minutes on a Raspberry Pi — rebuilding on every invocation made
# the build, not the conversion, the thing you waited for.

set -euo pipefail

SRC=${1:-}
OUT=${2:-./converted}

if [[ -z $SRC ]]; then
  sed -n '2,13p' "$0" | sed 's/^# \?//'
  exit 2
fi

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=${WG_BUILD_DIR:-$REPO/.build/epub2wgb}
BIN=$BUILD/epub2wgb

# True when the binary is missing or older than any source it is built from.
needs_build() {
  [[ ! -x $BIN ]] && return 0
  local newer
  newer=$(find "$REPO/wintergreen" "$REPO/tools/epub2wgb" "$REPO/third_party/miniz" \
            -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.c' -o -name 'CMakeLists.txt' \) \
            -newer "$BIN" -print -quit 2>/dev/null)
  [[ -n $newer ]]
}

build() {
  cmake -S "$REPO/tools/epub2wgb" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$BUILD" -j"$(nproc)" >/dev/null
}

run() {
  "$BIN" -o "$OUT" "$SRC"
}

if needs_build; then
  if command -v cmake >/dev/null 2>&1; then
    build
  elif command -v nix-shell >/dev/null 2>&1; then
    export -f build
    export REPO BUILD
    nix-shell -p cmake gcc --run build
  else
    echo "convert-books.sh: needs cmake (or nix-shell to fetch it) to build $BIN" >&2
    exit 1
  fi
fi

run
