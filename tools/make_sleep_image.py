#!/usr/bin/env python3
"""Image -> the wordmark sleep frame (resources/sleep.frame).

The frame is the panel's own, the same format the converter writes for a
book's cover_sleep.bin (CoverPaths.h), so one streamer shows both:

    96,000 bytes, no header, no magic. Per physical row r (0..479), 100 bytes
    of the hi plane then 100 bytes of the lo plane, MSB first; level =
    hi << 1 | lo: 0 white, 1 light, 2 gray, 3 black.
    Portrait pixel (x, y) of the 480 x 800 panel is physical row 479 - x,
    bit y. Bits 0..6 (portrait rows 0..6) are behind the bezel.

A full refresh shows all four levels, so the source's antialiased edges come
out as light and gray; flat black and white stay exact. Each pixel takes the
nearest level at the converter's gray and light reflectances. No dither: the
wordmark is line art, and blue noise scattered gray specks through the
near-black strokes and along every edge.

The source is drawn full screen as is: an RGBA image composited over white and
scaled to 480 x 800 (the default source, resources/wintergreen-source.png, is
960 x 1600: exactly 0.5).

  nix shell --impure --expr 'with import <nixpkgs> {}; python3.withPackages (ps: [ps.pillow])' \\
    -c python3 tools/make_sleep_image.py resources/wintergreen-source.png resources/sleep.frame

--png also writes a preview of the frame as the panel would show it.
"""

import argparse

from PIL import Image

PANEL_W = 480   # portrait
PANEL_H = 800   # portrait, including the 7 hidden rows at the top
ROWS = 480      # physical
STRIDE = 100    # bytes per plane row
# Reflectance of each level, 0 = black .. 1 = white. Same constants as the
# converter (ImageDecoder.h kGrayLevel / kLightLevel).
LEVEL_LUM = {0: 1.0, 1: 0.75, 2: 0.5, 3: 0.0}
PREVIEW = {0: 226, 1: 178, 2: 128, 3: 34}


def quantise(g, t):
    """Gray 0..255 -> level, choosing between the two levels around it, in
    8-bit tones exactly as ImageDecoder.cpp quantise_level does."""
    order = [3, 2, 1, 0]  # dark to light
    for lo, hi in zip(order, order[1:]):
        a, b = int(LEVEL_LUM[lo] * 255 + 0.5), int(LEVEL_LUM[hi] * 255 + 0.5)
        if g <= b:
            f = (g - a) / (b - a)
            return hi if f > t else lo
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--png", help="preview of the frame as shown")
    args = ap.parse_args()

    im = Image.open(args.src).convert("RGBA")
    bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
    im = Image.alpha_composite(bg, im).convert("L")
    im = im.resize((PANEL_W, PANEL_H), Image.LANCZOS)
    px = im.load()
    levels = [[0] * PANEL_H for _ in range(PANEL_W)]
    for x in range(PANEL_W):
        for y in range(PANEL_H):
            levels[x][y] = quantise(px[x, y], 0.5)

    out = bytearray(ROWS * 2 * STRIDE)
    for x in range(PANEL_W):
        row = (ROWS - 1 - x) * 2 * STRIDE
        for y in range(PANEL_H):
            lv = levels[x][y]
            m = 0x80 >> (y & 7)
            if lv & 2:
                out[row + (y >> 3)] |= m
            if lv & 1:
                out[row + STRIDE + (y >> 3)] |= m
    with open(args.out, "wb") as f:
        f.write(out)
    counts = [sum(1 for col in levels for v in col if v == k) for k in range(4)]
    print("%s: %d bytes; white %d, light %d, gray %d, black %d" % (args.out, len(out), *counts))

    if args.png:
        prev = Image.new("L", (PANEL_W, PANEL_H))
        pp = prev.load()
        for x in range(PANEL_W):
            for y in range(PANEL_H):
                pp[x, y] = PREVIEW[levels[x][y]]
        prev.save(args.png)


if __name__ == "__main__":
    main()
