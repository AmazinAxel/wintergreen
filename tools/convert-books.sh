#!/usr/bin/env bash

set -euo pipefail

SRC=${1:-}
OUT=${2:-./converted}

if [[ -z $SRC ]]; then
  sed -n '2,14p' "$0" | sed 's/^# \?//'
  exit 2
fi

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=${WG_BUILD_DIR:-$REPO/.build/epub2mrb}

build_and_run() {
  cmake -S "$REPO/tools/epub2mrb" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$BUILD" -j"$(nproc)" >/dev/null
  "$BUILD/epub2mrb" -o "$OUT" "$SRC"
}

if command -v cmake >/dev/null 2>&1; then
  build_and_run
elif command -v nix-shell >/dev/null 2>&1; then
  export -f build_and_run
  export REPO BUILD OUT SRC
  nix-shell -p cmake gcc --run build_and_run
else
  echo "convert-books.sh: needs cmake (or nix-shell to fetch it)" >&2
  exit 1
fi
