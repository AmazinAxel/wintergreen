#!/usr/bin/env python3
"""TTF/TTC -> MBF4 font compiler.

Produces the two shapes the firmware consumes:

  bundle  reader font: an FNTS v2 bundle of one MBF4 per size, zlib-compressed
          behind a uint32 length prefix (what asset_blob ships and
          FontPartition::provision_embedded decompresses).
  header  UI font: a bare MBF4 emitted as a C array, matching
          lib/wintergreen/display/ui_font_*.h.

Needs freetype-py, which is not in the PlatformIO interpreter. Run it under:

  nix-shell -p 'python3.withPackages(ps: [ps.freetype-py])' --run '...'

Format notes that are not in BitmapFontFormat.h and were recovered from the
shipped Literata:

  * advance_width is in QUARTER-pixels, not pixels.
  * Antialiasing is 5 levels across the three planes, encoded as (bw, msb, lsb):
        white (1,0,0)  light (1,0,1)  mid (0,1,0)  dark (0,1,1)  black (0,0,0)
    The BW plane draws where its bit is CLEAR; the gray planes draw where their
    bit is SET. So (msb,lsb) is a lightening modifier, not a magnitude.
  * Untouched BW bits must be 1 (white); untouched gray bits 0.

Rendering choices, for quality:

  * FT_LOAD_TARGET_LIGHT — vertical-only hinting. Full hinting would round every
    advance to a whole pixel and distort letterforms; light hinting snaps stems
    to the pixel grid vertically while leaving horizontal metrics linear, so the
    quarter-pixel advances this format supports are actually meaningful.
  * The 5 levels are nearest-neighbour bins of coverage (boundaries at 1/8, 3/8,
    5/8, 7/8), which is the correct rounding for evenly spaced output levels.
  * Class kerning is extracted from GPOS (and the legacy kern table) and packed
    losslessly: glyphs with identical kerning rows share a class.
"""

import argparse
import os
import struct
import sys
import zlib

import freetype

# Same coverage as Literata and Inter: ASCII, Latin-1 + Latin Ext-A, and
# General Punctuation / Currency. 527 glyphs.
RANGES = [(0x0020, 95), (0x00A0, 224), (0x2000, 208)]

# Codepoints a font may not cover, mapped to a visually equivalent one it very
# likely does. Without this the renderer draws nothing at all — glyph coverage
# is sparse and there is no fallback chain (see CLAUDE.md "Fonts").
SUBSTITUTES = {
    0x2010: 0x002D,  # hyphen -> hyphen-minus
    0x2011: 0x002D,  # non-breaking hyphen
    0x2012: 0x2013,  # figure dash -> en dash
    0x2015: 0x2014,  # horizontal bar -> em dash
    0x2032: 0x0027,  # prime -> apostrophe
    0x2033: 0x0022,  # double prime -> quote
    0x2044: 0x002F,  # fraction slash -> solidus
    0x2212: 0x002D,  # minus sign -> hyphen-minus
    0x00A0: 0x0020,  # nbsp -> space
    0x2007: 0x0020,  # figure space
    0x2008: 0x0020,  # punctuation space
    0x2009: 0x0020,  # thin space
    0x202F: 0x0020,  # narrow nbsp
}

STYLE_ORDER = ["regular", "bold", "italic", "bold_italic"]

MBF_MAGIC = 0x3446424D
MBF_VERSION = 4
HEADER_SIZE = 50
RANGE_SIZE = 8
GLYPH_SIZE = 10
STYLE_SECTION_SIZE = 8

# Coverage -> level. Level 4 is solid black, 0 is untouched background.
LEVEL_THRESHOLDS = (32, 96, 160, 224)

# level -> (bw_bit, msb_bit, lsb_bit)
LEVEL_BITS = {
    0: (1, 0, 0),
    1: (1, 0, 1),
    2: (0, 1, 0),
    3: (0, 1, 1),
    4: (0, 0, 0),
}


def codepoints():
    for first, count in RANGES:
        for i in range(count):
            yield first + i


def level_for(coverage):
    for level, t in enumerate(LEVEL_THRESHOLDS):
        if coverage < t:
            return level
    return 4


class Glyph:
    __slots__ = ("advance_q", "width", "height", "x_off", "y_off", "bw", "lsb", "msb")

    def __init__(self):
        self.advance_q = 0
        self.width = 0
        self.height = 0
        self.x_off = 0
        self.y_off = 0
        self.bw = b""
        self.lsb = b""
        self.msb = b""


LOAD_FLAGS = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_LIGHT


def glyph_index(face, cp):
    """Glyph index for a codepoint, falling back to a visual substitute."""
    gi = face.get_char_index(cp)
    if gi == 0 and cp in SUBSTITUTES:
        gi = face.get_char_index(SUBSTITUTES[cp])
    return gi


def render_glyph(face, cp):
    """Rasterize one codepoint into the three planes. Returns None if absent."""
    gi = glyph_index(face, cp)
    if gi == 0:
        return None
    face.load_glyph(gi, LOAD_FLAGS)
    slot = face.glyph
    bmp = slot.bitmap

    g = Glyph()
    g.advance_q = (slot.advance.x + 8) // 16  # 26.6 fixed -> quarter-pixels
    g.width = bmp.width
    g.height = bmp.rows
    g.x_off = slot.bitmap_left
    g.y_off = -slot.bitmap_top

    if g.width == 0 or g.height == 0:
        g.width = g.height = 0
        return g

    stride = (g.width + 7) // 8
    bw = bytearray(b"\xff" * (stride * g.height))
    lsb = bytearray(stride * g.height)
    msb = bytearray(stride * g.height)

    for row in range(g.height):
        src = row * bmp.pitch
        dst = row * stride
        for col in range(g.width):
            b, m, l = LEVEL_BITS[level_for(bmp.buffer[src + col])]
            mask = 1 << (7 - (col & 7))
            idx = dst + (col >> 3)
            if not b:
                bw[idx] &= ~mask & 0xFF
            if m:
                msb[idx] |= mask
            if l:
                lsb[idx] |= mask

    g.bw, g.lsb, g.msb = bytes(bw), bytes(lsb), bytes(msb)
    return g


def build_style(face, px, blank_advance_q):
    """Rasterize every codepoint for one style. Returns (glyphs, missing)."""
    glyphs = []
    missing = []
    for cp in codepoints():
        g = render_glyph(face, cp)
        if g is None:
            missing.append(cp)
            g = Glyph()
            g.advance_q = blank_advance_q
        glyphs.append(g)
    return glyphs, missing


# ── Kerning ──────────────────────────────────────────────────────────────────


def extract_kern_pairs(path, face_index):
    """{(left_gname, right_gname): x_advance_in_units} from GPOS + legacy kern."""
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        print("  fontTools missing — no kerning will be emitted", file=sys.stderr)
        return {}, 1000

    font = TTFont(path, fontNumber=face_index, lazy=True)
    upem = font["head"].unitsPerEm
    pairs = {}

    if "kern" in font:
        for st in font["kern"].kernTables:
            pairs.update(st.kernTable)

    if "GPOS" in font:
        gpos = font["GPOS"].table
        lookups = gpos.LookupList.Lookup if gpos.LookupList else []

        # Only lookups reachable from a 'kern' feature.
        wanted = set()
        for rec in gpos.FeatureList.FeatureRecord if gpos.FeatureList else []:
            if rec.FeatureTag == "kern":
                wanted.update(rec.Feature.LookupListIndex)

        def subtables(lk):
            for st in lk.SubTable:
                if lk.LookupType == 9:  # extension
                    yield st.ExtSubTable
                else:
                    yield st

        for idx in sorted(wanted):
            if idx >= len(lookups):
                continue
            lk = lookups[idx]
            if lk.LookupType not in (2, 9):
                continue
            for st in subtables(lk):
                if getattr(st, "LookupType", 2) != 2 and lk.LookupType == 9:
                    continue
                fmt = getattr(st, "Format", None)
                if fmt == 1:
                    for first, pairset in zip(st.Coverage.glyphs, st.PairSet):
                        for pvr in pairset.PairValueRecord:
                            v = getattr(pvr.Value1, "XAdvance", 0) if pvr.Value1 else 0
                            if v:
                                pairs[(first, pvr.SecondGlyph)] = v
                elif fmt == 2:
                    c1 = st.ClassDef1.classDefs if st.ClassDef1 else {}
                    c2 = st.ClassDef2.classDefs if st.ClassDef2 else {}
                    covered = set(st.Coverage.glyphs)
                    # class -> glyphs, with class 0 = everything not listed
                    left = {}
                    for g in covered:
                        left.setdefault(c1.get(g, 0), []).append(g)
                    right = {}
                    for g in font.getGlyphOrder():
                        right.setdefault(c2.get(g, 0), []).append(g)
                    for i, c1rec in enumerate(st.Class1Record):
                        for j, c2rec in enumerate(c1rec.Class2Record):
                            v = getattr(c2rec.Value1, "XAdvance", 0) if c2rec.Value1 else 0
                            if not v:
                                continue
                            for lg in left.get(i, ()):
                                for rg in right.get(j, ()):
                                    pairs[(lg, rg)] = v

    return pairs, upem


def build_kerning(glyph_names, pairs, upem, px, n_glyphs):
    """Pack pair kerning into the class matrix MbfClassKerning describes.

    Glyphs whose kerning row (or column) is identical share a class, which is
    lossless. Returns the binary block, or b'' if there is nothing to emit.
    """
    if not pairs:
        return b""

    scale = px * 4.0 / upem  # font units -> quarter-pixels

    # rows[i][j] = kern in quarter-pixels, only for glyphs in our table
    name_to_idx = {}
    for i, n in enumerate(glyph_names):
        if n is not None:
            name_to_idx.setdefault(n, i)

    rows = {}
    for (ln, rn), v in pairs.items():
        li, ri = name_to_idx.get(ln), name_to_idx.get(rn)
        if li is None or ri is None:
            continue
        q = int(round(v * scale))
        if q:
            rows.setdefault(li, {})[ri] = max(-128, min(127, q))

    if not rows:
        return b""

    # Left classes: identical rows collapse. Class 0 is the empty row.
    l_class = [0] * n_glyphs
    l_keys = {(): 0}
    l_rows = [{}]
    for li, row in rows.items():
        key = tuple(sorted(row.items()))
        if key not in l_keys:
            l_keys[key] = len(l_rows)
            l_rows.append(row)
        l_class[li] = l_keys[key]

    # Right classes: two right-glyphs are equivalent if every left class kerns
    # them the same. Build each right glyph's column over the left classes.
    cols = {}
    for lc, row in enumerate(l_rows):
        for ri, q in row.items():
            cols.setdefault(ri, {})[lc] = q
    r_class = [0] * n_glyphs
    r_keys = {(): 0}
    r_cols = [{}]
    for ri, col in cols.items():
        key = tuple(sorted(col.items()))
        if key not in r_keys:
            r_keys[key] = len(r_cols)
            r_cols.append(col)
        r_class[ri] = r_keys[key]

    n_l, n_r = len(l_rows), len(r_cols)
    if n_l > 256 or n_r > 256:
        print(
            f"  kerning: {n_l}x{n_r} classes exceeds the uint8 class id limit — dropping kerning",
            file=sys.stderr,
        )
        return b""

    matrix = bytearray(n_l * n_r)
    for rc, col in enumerate(r_cols):
        for lc, q in col.items():
            matrix[lc * n_r + rc] = q & 0xFF

    out = bytearray(struct.pack("<BB", n_l - 1, n_r - 1))
    out += bytes(l_class)
    out += bytes(r_class)
    out += matrix
    return bytes(out)


def face_metrics(face, line_height_pct=None):
    """(baseline, y_advance) in whole pixels.

    The reader multiplies y_advance by the user's line-height setting, so the
    font's own value is the base leading. A font's native height varies wildly
    (Atkinson is 1.25x its em, Literata 1.55x), which would make the same reader
    setting look cramped or airy depending only on which font is loaded.
    line_height_pct pins it to a chosen ratio of the em instead, and the extra
    leading is split evenly above and below the text rather than all falling
    under the baseline.
    """
    m = face.size
    ascender = (m.ascender + 63) >> 6
    descender = (-m.descender + 63) >> 6
    natural = max((m.height + 63) >> 6, ascender + descender)

    if line_height_pct is None:
        return ascender, natural

    em = (m.x_ppem + m.y_ppem) // 2 or 1
    target = max(ascender + descender, round(em * line_height_pct / 100))
    baseline = ascender + (target - (ascender + descender)) // 2
    return baseline, target


def build_mbf(faces, px, name, kern_sources=None, line_height_pct=None):
    """Compile one MBF4 blob for a single pixel size.

    faces: dict of style -> freetype.Face (regular is required).
    kern_sources: dict of style -> (pairs, upem) from extract_kern_pairs.
    """
    for f in faces.values():
        f.set_pixel_sizes(0, px)

    reg = faces["regular"]
    baseline, y_advance = face_metrics(reg, line_height_pct)

    space = reg.get_char_index(0x20)
    if space:
        reg.load_glyph(space, freetype.FT_LOAD_NO_BITMAP)
        default_advance_q = (reg.glyph.advance.x + 8) // 16
    else:
        default_advance_q = px * 2

    styles = {}
    for style, face in faces.items():
        glyphs, missing = build_style(face, px, default_advance_q)
        styles[style] = glyphs
        if missing:
            sample = " ".join(f"U+{c:04X}" for c in missing[:8])
            print(
                f"  [{name} {px}px {style}] {len(missing)} glyph(s) missing, blanked: {sample}"
                + (" ..." if len(missing) > 8 else ""),
                file=sys.stderr,
            )

    present = [s for s in STYLE_ORDER if s in styles]
    extra = [s for s in present if s != "regular"]

    n_ranges = len(RANGES)
    n_glyphs = sum(c for _, c in RANGES)
    table_size = n_ranges * RANGE_SIZE + n_glyphs * GLYPH_SIZE

    # Kerning, per style. BitmapFont expects each style's block immediately
    # after that style's glyph table.
    kern = {}
    for style in present:
        pairs, upem = (kern_sources or {}).get(style, ({}, 1000))
        names = [
            faces[style].get_glyph_name(glyph_index(faces[style], cp)).decode()
            if glyph_index(faces[style], cp)
            else None
            for cp in codepoints()
        ]
        kern[style] = build_kerning(names, pairs, upem, px, n_glyphs)

    # ── Lay out the file ──────────────────────────────────────────────────────
    # Section tables first (Regular inline, extra styles as MbfStyleSection),
    # each followed by its kerning block, then the three shared bitmap pools.
    kern_offset = HEADER_SIZE + table_size if kern["regular"] else 0
    off = HEADER_SIZE + table_size + len(kern["regular"])
    style_offsets = {}
    for style in extra:
        style_offsets[style] = off
        off += STYLE_SECTION_SIZE + table_size + len(kern[style])

    # Bitmap pools. Every style appends into the same pool; a glyph's
    # bitmap_offset is shared by all three planes.
    bw_pool = bytearray()
    lsb_pool = bytearray()
    msb_pool = bytearray()
    offsets = {}
    seen = {}  # identical bitmaps (e.g. the same glyph reused) share an offset
    for style in present:
        offsets[style] = []
        for g in styles[style]:
            if g.width == 0 or g.height == 0:
                offsets[style].append(0)
                continue
            key = (g.bw, g.lsb, g.msb)
            if key not in seen:
                seen[key] = len(bw_pool)
                bw_pool += g.bw
                lsb_pool += g.lsb
                msb_pool += g.msb
            offsets[style].append(seen[key])

    bitmap_data_offset = off
    gray_lsb_offset = bitmap_data_offset + len(bw_pool)
    gray_msb_offset = gray_lsb_offset + len(lsb_pool)

    glyph_height = max((g.height for g in styles["regular"]), default=px)
    underline_pos = max(1, px // 10)
    underline_thickness = max(1, px // 14)

    # style_flags mirrors what the shipped fonts use: all four bits when the
    # extra styles are present, zero for a Regular-only font. Nothing reads it —
    # BitmapFont keys off the section offsets.
    style_flags = 0b1111 if extra else 0b0000

    out = bytearray()
    out += struct.pack(
        "<IBBBBBBHHH",
        MBF_MAGIC,
        MBF_VERSION,
        min(glyph_height, 255),
        min(baseline, 255),
        min(y_advance, 255),
        min(default_advance_q, 255),
        style_flags,
        n_ranges,
        n_glyphs,
        px,
    )
    out += struct.pack(
        "<8I",
        len(kern["regular"]),
        bitmap_data_offset,
        style_offsets.get("bold", 0),
        style_offsets.get("italic", 0),
        style_offsets.get("bold_italic", 0),
        kern_offset,
        gray_lsb_offset,
        gray_msb_offset,
    )
    out += struct.pack("<bB", underline_pos, underline_thickness)
    assert len(out) == HEADER_SIZE

    def emit_tables(style):
        buf = bytearray()
        start = 0
        for first, count in RANGES:
            buf += struct.pack("<IHH", first, count, start)
            start += count
        for g, o in zip(styles[style], offsets[style]):
            buf += struct.pack(
                "<IBBBbbB",
                o,
                min(g.advance_q, 255),
                g.width,
                g.height,
                max(-128, min(127, g.x_off)),
                max(-128, min(127, g.y_off)),
                0,
            )
        assert len(buf) == table_size
        return buf

    out += emit_tables("regular")
    out += kern["regular"]
    for style in extra:
        assert len(out) == style_offsets[style]
        out += struct.pack("<HHI", n_ranges, n_glyphs, len(kern[style]))
        out += emit_tables(style)
        out += kern[style]

    assert len(out) == bitmap_data_offset
    out += bw_pool
    out += lsb_pool
    out += msb_pool
    return bytes(out)


def open_face(spec):
    """'path' or 'path:index' for a TTC face."""
    path, idx = split_spec(spec)
    return freetype.Face(path, idx)


def split_spec(spec):
    if ":" in spec and not os.path.exists(spec):
        path, _, idx = spec.rpartition(":")
        return path, int(idx)
    return spec, 0


def pack_bundle(name, blobs):
    """FNTS v2: [FNTS][num][ver][pad:2][name:32][num x uint32 size][payloads]"""
    out = bytearray(b"FNTS")
    out += struct.pack("<BBH", len(blobs), 2, 0)
    out += name.encode("ascii")[:32].ljust(32, b"\0")
    for b in blobs:
        out += struct.pack("<I", len(b))
    for b in blobs:
        out += b
    return bytes(out)


def emit_header(mbf, symbol, source):
    lines = [
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        f"// Auto-generated by tools/make_font.py from {source}",
        f"// Size: {len(mbf)} bytes",
        "",
        f"alignas(4) inline constexpr uint8_t kFontData_{symbol}_mbf[] = {{",
    ]
    for i in range(0, len(mbf), 16):
        chunk = ", ".join(f"0x{b:02X}" for b in mbf[i : i + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append(f"inline constexpr size_t kFontData_{symbol}_mbf_size = {len(mbf)};")
    lines.append("")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def add_faces(p):
        p.add_argument("--regular", required=True, help="TTF/TTC path, or path:face_index")
        p.add_argument("--bold")
        p.add_argument("--italic")
        p.add_argument("--bold-italic", dest="bold_italic")
        p.add_argument(
            "--line-height",
            type=int,
            default=None,
            help="line height as a percent of the em (e.g. 145). Default: the font's own "
            "metrics, which vary enough between families to change how the reader's "
            "line-height setting feels.",
        )

    b = sub.add_parser("bundle", help="reader font: compressed FNTS v2 bundle")
    add_faces(b)
    b.add_argument("--name", required=True, help="bundle name, <= 32 chars")
    b.add_argument("--sizes", required=True, help="comma-separated pixel sizes")
    b.add_argument("--out", required=True)
    b.add_argument("--raw-out", help="also write the uncompressed .mfb bundle")

    h = sub.add_parser("header", help="UI font: bare MBF4 as a C header")
    add_faces(h)
    h.add_argument("--name", required=True)
    h.add_argument("--size", type=int, required=True)
    h.add_argument("--symbol", required=True, help="e.g. ui_small -> kFontData_ui_small_mbf")
    h.add_argument("--out", required=True)

    args = ap.parse_args()

    faces = {}
    kern_sources = {}
    for style in STYLE_ORDER:
        spec = getattr(args, style, None)
        if not spec:
            continue
        faces[style] = open_face(spec)
        path, idx = split_spec(spec)
        pairs, upem = extract_kern_pairs(path, idx)
        kern_sources[style] = (pairs, upem)
        print(f"  {style}: {len(pairs):,} kern pairs", file=sys.stderr)
    fam = faces["regular"].family_name.decode()
    sty = faces["regular"].style_name.decode()
    print(f"{args.name}: {fam} {sty} ({len(faces)} style(s))", file=sys.stderr)

    if args.cmd == "header":
        mbf = build_mbf(faces, args.size, args.name, kern_sources, args.line_height)
        with open(args.out, "w") as f:
            f.write(emit_header(mbf, args.symbol, f"{fam} {sty} @ {args.size}px"))
        print(f"  -> {args.out} ({len(mbf):,} bytes of font data)", file=sys.stderr)
        return

    sizes = [int(s) for s in args.sizes.split(",")]
    blobs = []
    for px in sizes:
        blob = build_mbf(faces, px, args.name, kern_sources, args.line_height)
        print(f"  {px}px -> {len(blob):,} bytes", file=sys.stderr)
        blobs.append(blob)

    bundle = pack_bundle(args.name, blobs)
    if args.raw_out:
        with open(args.raw_out, "wb") as f:
            f.write(bundle)

    comp = zlib.compress(bundle, 9)
    with open(args.out, "wb") as f:
        f.write(struct.pack("<I", len(bundle)))
        f.write(comp)
    print(
        f"  -> {args.out}: {len(bundle):,} bytes uncompressed, "
        f"{len(comp) + 4:,} compressed",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
