#!/usr/bin/env python3
"""Validate a WGF1 blob / WGFS bundle the way BitmapFont.h parses it.

Catches layout mistakes that would silently render blank on device. Also draws
a sample string so the rasterization can be eyeballed without a flash.

  python3 tools/check_font.py resources/AtkinsonHyperlegible.bin [text]
"""

import struct
import sys
import zlib


HEADER_SIZE = 49
STYLE_NAMES = ["Regular", "Bold", "Italic", "BoldItalic"]

def load(path):
    raw = open(path, "rb").read()
    if raw[:4] in (b"WGFS", b"WGF1"):
        return raw
    size = struct.unpack_from("<I", raw, 0)[0]
    out = zlib.decompress(raw[4:])
    assert len(out) == size, f"length prefix {size} != actual {len(out)}"
    return out


class Wgf:
    def __init__(self, d, base=0):
        self.d, self.b = d, base
        (magic, self.glyph_h, self.baseline, self.yadv, self.dadv,
         self.style_flags, self.n_ranges, self.n_glyphs, self.nominal) = struct.unpack_from("<IBBBBBHHH", d, base)
        assert magic == 0x31464757, f"bad magic {magic:#x} (expected WGF1)"
        (self.kern_len, self.bmp_off, self.bold_off, self.italic_off,
         self.bi_off, self.kern_off, self.lsb_off, self.msb_off) = struct.unpack_from("<8I", d, base + 15)
        self.ul_pos, self.ul_th = struct.unpack_from("<bB", d, base + 47)

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
        s = self.style(which)
        if s is None:
            s = self.style(0)
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


def check(m, label):
    end = len(m.d)
    errs = []
    for w, nm in enumerate(STYLE_NAMES):
        s = m.style(w)
        if s is None:
            continue
        r, g, nr, ng, kl, koff = s
        if g + ng * 10 > end:
            errs.append(f"{nm}: glyph table overruns file")
        if kl:
            base = m.b + koff
            nl, nrc = m.d[base] + 1, m.d[base + 1] + 1
            need = 2 + 2 * ng + nl * nrc
            if kl != need:
                errs.append(f"{nm}: kerning_length {kl} != computed {need}")
            if base + kl > end:
                errs.append(f"{nm}: kerning block overruns file")
        # every glyph's bitmap must lie inside each plane
        for i in range(ng):
            off, adv, bw, bh, xo, yo, _ = struct.unpack_from("<IBBBbbB", m.d, g + 10 * i)
            if bw == 0 or bh == 0:
                continue
            n = ((bw + 7) // 8) * bh
            for plane_off, pn in ((m.bmp_off, "BW"), (m.lsb_off, "LSB"), (m.msb_off, "MSB")):
                if not plane_off:
                    continue
                if m.b + plane_off + off + n > end:
                    errs.append(f"{nm}: glyph {i} {pn} bitmap overruns file")
                    break
    blank = sum(1 for cp in (0x41, 0x61, 0x30, 0x2E, 0x2C, 0x2D) if not (m.glyph(0, cp) or (None, (0,) * 7))[1][2])
    if blank:
        errs.append(f"{blank} of 6 essential glyphs (A a 0 . , -) have no bitmap")
    print(f"{label}: nominal={m.nominal} baseline={m.baseline} yadv={m.yadv} "
          f"glyph_h={m.glyph_h} styles={[n for i, n in enumerate(STYLE_NAMES) if m.style(i)]} "
          f"kern={'yes' if m.kern_len else 'no'} gray={'yes' if m.lsb_off else 'no'}")
    for e in errs:
        print(f"   ERROR {e}")
    return not errs

def draw(m, text):
    """ASCII-art the string, using the same 5 levels the panel gets."""
    pen_q = 0
    cells = {}
    prev = None
    for ch in text:
        got = m.glyph(0, ord(ch))
        if not got:
            prev = None
            continue
        idx, (off, adv, bw, bh, xo, yo, _) = got
        if prev is not None:
            pen_q += m.kern_q(0, prev, idx)
        x = (pen_q + 2) // 4
        stride = (bw + 7) // 8
        for row in range(bh):
            for col in range(bw):
                byte = row * stride + (col >> 3)
                mask = 1 << (7 - (col & 7))
                b = 1 if not m.bmp_off else (m.d[m.b + m.bmp_off + off + byte] & mask) != 0
                l = (m.d[m.b + m.lsb_off + off + byte] & mask) != 0 if m.lsb_off else 0
                s = (m.d[m.b + m.msb_off + off + byte] & mask) != 0 if m.msb_off else 0
                lvl = {(1, 0, 0): 0, (1, 0, 1): 1, (0, 1, 0): 2, (0, 1, 1): 3, (0, 0, 0): 4}.get(
                    (int(b), int(s), int(l)))
                if lvl:
                    cells[(x + xo + col, m.baseline + yo + row)] = lvl
        pen_q += adv
        pen_q = ((pen_q + 2) // 4) * 4
        prev = idx
    if not cells:
        print("   (nothing rendered)")
        return
    xs = [p[0] for p in cells]
    ys = [p[1] for p in cells]
    art = " .:*#"
    for y in range(min(ys), max(ys) + 1):
        print("   |" + "".join(art[cells.get((x, y), 0)] for x in range(min(xs), max(xs) + 1)) + "|")
    print(f"   width={(pen_q + 2) // 4}px")


def main():
    path = sys.argv[1]
    text = sys.argv[2] if len(sys.argv) > 2 else "Wave AVATAR, To fj. “quick”"
    d = load(path)
    ok = True
    if d[:4] == b"WGFS":
        n = d[4]
        sizes = [struct.unpack_from("<I", d, 8 + 4 * i)[0] for i in range(n)]
        print(f"WGFS: {n} size(s), {len(d):,} bytes")
        o = 8 + 4 * n
        for i, s in enumerate(sizes):
            m = Wgf(d, o)
            ok &= check(m, f"  [{i}] {s:,}B")
            o += s
        m = Wgf(d, 8 + 4 * n)
        draw(m, text)
    else:
        m = Wgf(d, 0)
        ok &= check(m, path)
        draw(m, text)
    print("OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
