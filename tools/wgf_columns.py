#!/usr/bin/env python3
"""Append column-major glyph bitmaps to WGF fonts (see WgfFormat.h).

Portrait text (every list and menu, and the reader in portrait) maps a glyph
column onto a panel row. From row-major bitmaps that is a bit-by-bit transpose
per glyph; from a column-major copy it is the same shifted byte copy landscape
does. The copy is derived from the existing bitmaps, so pixels never change.

Idempotent: an existing column section is dropped and rebuilt.

    tools/wgf_columns.py resources/Libron.bin wintergreen/display/ui_font_*.h

Libron.bin is `[u32 raw size][zlib WGFS bundle]`; headers are make_font.py's
C arrays. No dependencies beyond the standard library, so it runs under
PlatformIO's interpreter. make_font.py calls add_columns() on every font it
builds.
"""

import re
import struct
import sys
import zlib

HEADER_SIZE = 45
GLYPH_SIZE = 10
RANGE_SIZE = 8
STYLE_SECTION_SIZE = 8
COL_FIELDS = 35  # col_bitmap_offset, col_index_offset


def _transpose(bits, w, h):
    row_bytes = (w + 7) // 8
    col_bytes = (h + 7) // 8
    out = bytearray(b"\xff" * (col_bytes * w))
    for r in range(h):
        for c in range(w):
            if not (bits[r * row_bytes + (c >> 3)] >> (7 - (c & 7))) & 1:
                out[c * col_bytes + (r >> 3)] &= ~(0x80 >> (r & 7)) & 0xFF
    return bytes(out)


def add_columns(wgf):
    wgf = bytes(wgf)
    num_ranges, num_glyphs = struct.unpack_from("<HH", wgf, 5)
    bitmap_data_offset, bold, italic, bold_italic = struct.unpack_from("<4I", wgf, 15)
    col_bitmap, col_index = struct.unpack_from("<2I", wgf, COL_FIELDS)
    if col_bitmap:
        wgf = wgf[:col_bitmap]

    tables = [(HEADER_SIZE + num_ranges * RANGE_SIZE, num_glyphs)]
    for off in (bold, italic, bold_italic):
        if off:
            nr, ng = struct.unpack_from("<HH", wgf, off)
            tables.append((off + STYLE_SECTION_SIZE + nr * RANGE_SIZE, ng))

    pool = bytearray()
    index = bytearray()
    seen = {}
    for start, n in tables:
        for i in range(n):
            boff, _, w, h = struct.unpack_from("<IBBB", wgf, start + i * GLYPH_SIZE)
            if w == 0 or h == 0:
                index += struct.pack("<I", 0)
                continue
            if boff not in seen:
                src = wgf[bitmap_data_offset + boff : bitmap_data_offset + boff + ((w + 7) // 8) * h]
                seen[boff] = len(pool)
                pool += _transpose(src, w, h)
            index += struct.pack("<I", seen[boff])

    col_bitmap = len(wgf)
    out = bytearray(wgf)
    out += pool
    col_index = len(out)
    out += index
    struct.pack_into("<2I", out, COL_FIELDS, col_bitmap, col_index)
    return bytes(out)


def _bundle(raw):
    count = raw[0]
    sizes = struct.unpack_from("<%dI" % count, raw, 4)
    off = 4 + 4 * count
    blobs = []
    for s in sizes:
        blobs.append(add_columns(raw[off : off + s]))
        off += s
    out = bytearray(struct.pack("<BBBB", count, 0, 0, 0))
    for b in blobs:
        out += struct.pack("<I", len(b))
    for b in blobs:
        out += b
    return bytes(out)


def _do_bin(path):
    with open(path, "rb") as f:
        blob = f.read()
    raw = zlib.decompress(blob[4:])
    out = _bundle(raw)
    with open(path, "wb") as f:
        f.write(struct.pack("<I", len(out)) + zlib.compress(out, 9))
    print("%s: %d -> %d bytes inflated" % (path, len(raw), len(out)))


def _do_header(path):
    with open(path) as f:
        text = f.read()
    m = re.search(r"(alignas\(4\) inline constexpr uint8_t (\w+)\[\] = \{\n)(.*?)(\n\};\n)", text, re.S)
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(3)))
    out = add_columns(data)
    lines = ",\n".join(
        "    " + ", ".join("0x%02X" % b for b in out[i : i + 16]) for i in range(0, len(out), 16)
    )
    text = text[: m.start(3)] + lines + "," + text[m.end(3) :]
    text = re.sub(r"// Size: \d+ bytes", "// Size: %d bytes" % len(out), text)
    text = re.sub(r"(_wgf_size = )\d+;", r"\g<1>%d;" % len(out), text)
    with open(path, "w") as f:
        f.write(text)
    print("%s: %d -> %d bytes" % (path, len(data), len(out)))


if __name__ == "__main__":
    for p in sys.argv[1:]:
        (_do_header if p.endswith(".h") else _do_bin)(p)
