"""Pre-build hook: prepare the two raw asset partitions and hand them to esptool.

Neither asset is embedded in the app image any more. `font` and `sleep` are raw
partitions (see platforms/esp32/default_16MB.csv) written at flash time and read
at runtime with esp_partition_mmap, so:

  - the app image carries neither, and every build/upload is ~218 KB smaller;
  - the font is no longer stored twice (once in the image, once in the
    partition) and is no longer decompressed into flash on first boot, which is
    what the old "Installing fonts..." progress screen existed for;
  - the sleep image is read straight out of memory-mapped flash.

The reader font ships as `[uint32 uncompressed size][zlib stream]`; it is
inflated **here**, on the build machine, so the device never runs an inflate.
"""

Import("env")

import os
import struct
import subprocess
import sys
import zlib

PROJECT_DIR = env.subst("$PROJECT_DIR")
BUILD_DIR = env.subst("$BUILD_DIR")

# Must match platforms/esp32/default_16MB.csv.
PARTITIONS = {
    "font": (0xC90000, 0x080000),
    "sleep": (0xD10000, 0x020000),
}


def _fail(msg):
    sys.stderr.write("[assets] ERROR: %s\n" % msg)
    env.Exit(1)


def _write_if_changed(path, data):
    """Avoid rewriting an identical image so esptool's own change detection and
    the build's timestamps stay stable."""
    if os.path.exists(path) and os.path.getsize(path) == len(data):
        with open(path, "rb") as f:
            if f.read() == data:
                return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return True


def _check_fits(name, data):
    _, size = PARTITIONS[name]
    if len(data) > size:
        _fail("%s image is %d bytes, partition is %d" % (name, len(data), size))


# --- font: inflate the WGFS bundle on the host ------------------------------
src = os.path.join(PROJECT_DIR, "resources", "AtkinsonHyperlegible.bin")
with open(src, "rb") as f:
    blob = f.read()
if len(blob) < 8:
    _fail("%s is too small to be a font bundle" % src)
(raw_size,) = struct.unpack("<I", blob[:4])
font = zlib.decompress(blob[4:])
if len(font) != raw_size:
    _fail("font size prefix says %d, inflated %d" % (raw_size, len(font)))
if font[:4] != b"WGFS":
    _fail("inflated font does not start with WGFS")
_check_fits("font", font)

# --- sleep: raw MGR2, copied verbatim ---------------------------------------
src = os.path.join(PROJECT_DIR, "resources", "sleep.mgr")
with open(src, "rb") as f:
    sleep = f.read()
if sleep[:4] != b"MGR2":
    _fail("%s does not start with MGR2" % src)
_check_fits("sleep", sleep)

images = []
for name, data in (("font", font), ("sleep", sleep)):
    path = os.path.join(BUILD_DIR, "%s_partition.bin" % name)
    changed = _write_if_changed(path, data)
    offset, size = PARTITIONS[name]
    print(
        "[assets] %-5s 0x%06X  %7d bytes / %d partition%s"
        % (name, offset, len(data), size, "  (rewritten)" if changed else "")
    )
    images.append((hex(offset), path))

env.Append(FLASH_EXTRA_IMAGES=images)
