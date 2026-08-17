#!/usr/bin/env python3
"""Image -> MGR2 sleep image.

MGR2 is a tiny 4-level-grayscale format read by DrawBuffer::Mgr2Source_:

    "MGR2"            4 bytes
    width             uint16 LE
    height            uint16 LE
    pixel data        2 bits per pixel, row-major, stride = ceil(width / 4),
                      packed MSB-first within each byte:
                          value = (row[x // 4] >> (6 - (x % 4) * 2)) & 3
                      0 = white, 1 = light, 2 = dark, 3 = black

The panel shows it via a one-pass grayscale refresh: bit 0 of each value goes to
BW RAM and bit 1 to RED RAM, which is why exactly 4 levels are available and why
no dithering beyond those levels will survive.

Geometry: the image is blitted at native size into physical panel coordinates
with no scaling or centring, so it should be exactly 800x480 (landscape). The
visible area is inset — DisplayFrame hides 10 columns at the left edge — so keep
anything important away from the extreme edges.

Usage (needs Pillow, absent from the PlatformIO interpreter):

    nix-shell -p 'python3.withPackages(ps: [ps.pillow])' --run \\
      'python3 tools/make_sleep_image.py cover.png resources/sleep.mgr'

Add --dither for photographic sources; the default is a plain 4-level quantise,
which is better for line art, logos and text.
"""

import argparse
import struct
import sys

from PIL import Image

PANEL_W = 800
PANEL_H = 480

# Level -> the gray it represents, used for nearest-level quantisation.
LEVEL_GRAYS = (255, 170, 85, 0)  # 0=white, 1=light, 2=dark, 3=black


def quantise(value):
    """8-bit gray -> 2-bit level (0 white .. 3 black)."""
    best = 0
    best_err = 999
    for level, gray in enumerate(LEVEL_GRAYS):
        err = abs(value - gray)
        if err < best_err:
            best_err = err
            best = level
    return best


def to_levels(img, dither):
    """Return a width*height list of 0..3 levels."""
    w, h = img.size
    if dither:
        # Floyd–Steinberg onto the 4 available grays.
        pal = Image.new("P", (1, 1))
        palette = []
        for g in LEVEL_GRAYS:
            palette += [g, g, g]
        palette += [0, 0, 0] * (256 - len(LEVEL_GRAYS))
        pal.putpalette(palette)
        q = img.convert("RGB").quantize(palette=pal, dither=Image.FLOYDSTEINBERG)
        return list(q.getdata())  # palette indices already equal our levels
    px = list(img.getdata())
    return [quantise(v) for v in px]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="any image Pillow can read")
    ap.add_argument("output", help="destination .mgr")
    ap.add_argument("--dither", action="store_true", help="Floyd-Steinberg; use for photos")
    ap.add_argument("--invert", action="store_true", help="swap black and white")
    ap.add_argument("--no-fit", action="store_true",
                    help="use the image as-is instead of fitting it to 800x480")
    args = ap.parse_args()

    img = Image.open(args.input).convert("L")

    if not args.no_fit:
        # Contain within the panel, then paste onto a white 800x480 field so the
        # output is always exactly panel-sized (the renderer does not centre).
        fitted = img.copy()
        fitted.thumbnail((PANEL_W, PANEL_H), Image.LANCZOS)
        canvas = Image.new("L", (PANEL_W, PANEL_H), 255)
        canvas.paste(fitted, ((PANEL_W - fitted.width) // 2, (PANEL_H - fitted.height) // 2))
        img = canvas

    if args.invert:
        img = Image.eval(img, lambda v: 255 - v)

    w, h = img.size
    if w > PANEL_W or h > PANEL_H:
        print(f"warning: {w}x{h} exceeds the {PANEL_W}x{PANEL_H} panel; it will be cropped on device",
              file=sys.stderr)

    levels = to_levels(img, args.dither)

    stride = (w + 3) // 4
    data = bytearray(stride * h)
    for y in range(h):
        row = y * stride
        base = y * w
        for x in range(w):
            lvl = levels[base + x] & 3
            data[row + (x // 4)] |= lvl << (6 - (x % 4) * 2)

    with open(args.output, "wb") as f:
        f.write(b"MGR2")
        f.write(struct.pack("<HH", w, h))
        f.write(data)

    print(f"{args.output}: {w}x{h}, {8 + len(data):,} bytes")


if __name__ == "__main__":
    main()
