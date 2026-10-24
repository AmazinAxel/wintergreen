#!/usr/bin/env bash
#
# tools/merge-bin.sh [out.bin]
#
# Merges a finished `pio run` build into one image flashable at 0x0, for a
# GitHub release. Partition offsets come from the partition table itself, so
# they cannot drift from what the firmware maps (a hand-typed 0xd10000 for the
# sleep image once shipped a release with an empty sleep partition).

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

OUT=${1:-wintergreen.bin}
BUILD=.pio/build/esp32c3
CSV=platforms/esp32/default_16MB.csv
ESPTOOL=~/.platformio/packages/tool-esptoolpy/esptool.py

offset() { awk -F, -v n="$1" '$1 ~ "^"n" *$" {gsub(/ /, "", $4); print $4}' "$CSV"; }
font=$(offset font)
sleep=$(offset sleep)
[[ -n $font && -n $sleep ]] || { echo "font/sleep partitions not found in $CSV" >&2; exit 1; }
for f in bootloader partitions firmware font_partition sleep_partition; do
  [[ -f $BUILD/$f.bin ]] || { echo "missing $BUILD/$f.bin -- run 'pio run' first" >&2; exit 1; }
done

nix-shell -p 'python3.withPackages(ps: [ps.pyserial])' --run "
python3 $ESPTOOL --chip esp32c3 merge_bin -o '$OUT' \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0     $BUILD/bootloader.bin \
  0x8000  $BUILD/partitions.bin \
  0x10000 $BUILD/firmware.bin \
  $font   $BUILD/font_partition.bin \
  $sleep  $BUILD/sleep_partition.bin"
echo "wrote $OUT (font @ $font, sleep @ $sleep)"
