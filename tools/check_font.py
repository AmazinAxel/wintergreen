#!/usr/bin/env python3
"""Validate a WGF font (bundle, raw bundle or ui_font_*.h) the way BitmapFont.h parses it.

Catches layout mistakes that would silently render blank on device, checks that
every glyph's black plane is inside its gray plane, and draws a sample string.

  python3 tools/check_font.py resources/Libron.bin [--png out.png] [text]
  python3 tools/check_font.py wintergreen/display/ui_font_large.h --png out.png

--png renders the sample in every size as the panel shows it (three levels,
simulated reflectance) at 1:1 and zoomed, for judging weight and thresholds
without a device. Needs Pillow (see make_font.py for the nix shell line).
"""

import re
import struct
import sys
import zlib


HEADER_SIZE = 45
STYLE_NAMES = ["Regular", "Bold", "Italic", "BoldItalic"]
# Simulated panel reflectance per level (white, light, gray, black), 0-255.
# Same table as tools/tests/preview.cpp.
PANEL = (226, 178, 128, 34)


def load_blobs(path):
    """List of (label, bytes, offset) WGF blobs in the file."""
    raw = open(path, "rb").read()
    if path.endswith(".h"):
        text = raw.decode()
        m = re.search(r"\[\] = \{(.*?)\};", text, re.S)
        data = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
        return [("header", data, 0)]
    try:
        d = zlib.decompress(raw[4:])
        size = struct.unpack_from("<I", raw, 0)[0]
        assert len(d) == size, f"length prefix {size} != actual {len(d)}"
    except zlib.error:
        d = raw
    n = d[0]
    sizes = [struct.unpack_from("<I", d, 4 + 4 * i)[0] for i in range(n)]
    print(f"WGFS: {n} size(s), {len(d):,} bytes")
    out = []
    o = 4 + 4 * n
    for i, s in enumerate(sizes):
        out.append((f"[{i}] {s:,}B", d, o))
        o += s
    return out


class Wgf:
    def __init__(self, d, base=0):
        self.d, self.b = d, base
        (self.glyph_h, self.baseline, self.yadv, self.dadv,
         self.style_flags, self.n_ranges, self.n_glyphs, self.nominal) = struct.unpack_from("<BBBBBHHH", d, base)
        (self.kern_len, self.bmp_off, self.bold_off, self.italic_off,
         self.bi_off, self.kern_off, self.col_off, self.col_index_off) = struct.unpack_from("<8I", d, base + 11)

    def style(self, which):
        """(ranges_off, glyphs_off, n_ranges, n_glyphs, kern_len, kern_off)."""
        if which == 0:
            r = self.b + HEADER_SIZE
            g = r + self.n_ranges * 8
            return r, g, self.n_ranges, self.n_glyphs, self.kern_len, self.kern_off
        off = [None, self.bold_off, self.italic_off, self.bi_off][which]
        if not off:
            return None
        nr, ng, kl = struct.unpack_from("<HHI", self.d, self.b + off)
        r = self.b + off + 8
        g = r + nr * 8
        return r, g, nr, ng, kl, (g + ng * 10 - self.b) if kl else 0

    def glyph(self, which, cp):
        s = self.style(which) or self.style(0)
        r, g, nr, ng, _, _ = s
        for i in range(nr):
            fc, cnt, gs = struct.unpack_from("<IHH", self.d, r + 8 * i)
            if fc <= cp < fc + cnt:
                idx = gs + (cp - fc)
                if idx >= ng:
                    return None
                return idx, struct.unpack_from("<IBBBbbB", self.d, g + 10 * idx)
        return None

    def kern_q(self, which, left_idx, right_idx):
        s = self.style(which) or self.style(0)
        _, g, nr, ng, kl, koff = s
        if kl < 2:
            return 0
        base = self.b + koff
        nl = self.d[base] + 1
        nrc = self.d[base + 1] + 1
        lmap = base + 2
        rmap = lmap + ng
        mat = rmap + ng
        lc, rc = self.d[lmap + left_idx], self.d[rmap + right_idx]
        if lc >= nl or rc >= nrc:
            return 0
        v = self.d[mat + lc * nrc + rc]
        return v - 256 if v > 127 else v

    def level(self, off, bw, bh, row, col):
        """0 white, 2 gray, 3 black for one glyph pixel."""
        stride = (bw + 7) // 8
        plane = stride * bh
        p = self.b + self.bmp_off + off + row * stride + (col >> 3)
        mask = 0x80 >> (col & 7)
        if not self.d[p] & mask:
            return 0
        return 3 if self.d[p + plane] & mask else 2


def check(m, label):
    end = len(m.d)
    errs = []
    if not m.col_off or not m.col_index_off:
        errs.append("no column-major copy (run tools/wgf_columns.py)")
    for w, nm in enumerate(STYLE_NAMES):
        s = m.style(w)
        if s is None:
            continue
        r, g, nr, ng, kl, koff = s
        if g + ng * 10 > end:
            errs.append(f"{nm}: glyph table overruns file")
            continue
        if kl:
            base = m.b + koff
            nl, nrc = m.d[base] + 1, m.d[base + 1] + 1
            need = 2 + 2 * ng + nl * nrc
            if kl != need:
                errs.append(f"{nm}: kerning_length {kl} != computed {need}")
            if base + kl > end:
                errs.append(f"{nm}: kerning block overruns file")
        for i in range(ng):
            off, adv, bw, bh, xo, yo, _ = struct.unpack_from("<IBBBbbB", m.d, g + 10 * i)
            if bw == 0 or bh == 0:
                continue
            n = ((bw + 7) // 8) * bh
            p = m.b + m.bmp_off + off
            if p + 2 * n > end:
                errs.append(f"{nm}: glyph {i} planes overrun file")
                break
            hi, lo = m.d[p : p + n], m.d[p + n : p + 2 * n]
            if any(l & ~h for h, l in zip(hi, lo)):
                errs.append(f"{nm}: glyph {i} black plane not inside gray plane")
                break
    blank = sum(1 for cp in (0x41, 0x61, 0x30, 0x2E, 0x2C, 0x2D) if not (m.glyph(0, cp) or (None, (0,) * 7))[1][2])
    if blank and m.n_glyphs > 10:
        errs.append(f"{blank} of 6 essential glyphs (A a 0 . , -) have no bitmap")
    print(f"{label}: nominal={m.nominal} baseline={m.baseline} yadv={m.yadv} "
          f"glyph_h={m.glyph_h} styles={[n for i, n in enumerate(STYLE_NAMES) if m.style(i)]} "
          f"kern={'yes' if m.kern_len else 'no'}")
    for e in errs:
        print(f"   ERROR {e}")
    return not errs


def render(m, text, style=0):
    """{(x, y): level} for the string, laid out as DrawBuffer.cpp draws it."""
    pen_q = 0
    cells = {}
    prev = None
    for ch in text:
        got = m.glyph(style, ord(ch))
        if not got:
            prev = None
            continue
        idx, (off, adv, bw, bh, xo, yo, _) = got
        if prev is not None:
            pen_q += m.kern_q(style, prev, idx)
        x = (pen_q + 2) // 4
        for row in range(bh):
            for col in range(bw):
                lvl = m.level(off, bw, bh, row, col)
                if lvl:
                    key = (x + xo + col, m.baseline + yo + row)
                    cells[key] = max(cells.get(key, 0), lvl)
        pen_q += adv
        pen_q = ((pen_q + 2) // 4) * 4
        prev = idx
    return cells, (pen_q + 2) // 4


def draw_ascii(m, text):
    cells, width = render(m, text)
    if not cells:
        print("   (nothing rendered)")
        return
    xs = [p[0] for p in cells]
    ys = [p[1] for p in cells]
    art = " .:#"
    for y in range(min(ys), max(ys) + 1):
        print("   |" + "".join(art[cells.get((x, y), 0)] for x in range(min(xs), max(xs) + 1)) + "|")
    print(f"   width={width}px")


def draw_png(fonts, text, path):
    from PIL import Image

    lines = []
    for m in fonts:
        for style in range(4):
            if style and not m.style(style):
                continue
            cells, width = render(m, text, style)
            lines.append((m, cells, width))
    pad = 12
    w = max(width for _, _, width in lines) + 2 * pad
    h = sum(m.yadv for m, _, _ in lines) + 2 * pad
    img = Image.new("L", (w, h), PANEL[0])
    px = img.load()
    y0 = pad
    for m, cells, _ in lines:
        for (x, y), lvl in cells.items():
            if 0 <= x + pad < w and 0 <= y + y0 < h:
                px[x + pad, y + y0] = PANEL[lvl]
        y0 += m.yadv
    zoom = 4
    big = img.resize((w * zoom, h * zoom), Image.NEAREST)
    img.save(path)
    zpath = path[:-4] + "-x4.png" if path.endswith(".png") else path + "-x4.png"
    big.save(zpath)
    print(f"   -> {path} ({w}x{h}), {zpath}")


def main():
    args = sys.argv[1:]
    png = None
    if "--png" in args:
        i = args.index("--png")
        png = args[i + 1]
        del args[i : i + 2]
    path = args[0]
    text = args[1] if len(args) > 1 else "Wave AVATAR, To fj. “quick” 0123456789"
    ok = True
    fonts = []
    for label, d, o in load_blobs(path):
        m = Wgf(d, o)
        ok &= check(m, "  " + label)
        fonts.append(m)
    draw_ascii(fonts[0], text)
    if png:
        draw_png(fonts, text, png)
    print("OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
