#!/usr/bin/env python3
"""Convert a TrueType font into the game selector's compact outline font (GLF1).

    python3 ghostldr/tools/mkfont.py NotoSans-Regular.ttf ghostldr/native/selector/font/notosans.glf

The selector draws text with its own rasterizer (ghostldr/native/selector/src/
font.c) when the console's system font is not available.  This tool keeps the
glyphs it needs (Latin, Greek, Cyrillic and common punctuation), flattens
composite glyphs, and writes the quadratic outlines as small deltas.  It also
keeps the font's pair kerning between printable ASCII characters (GPOS
'kern', pair adjustment formats 1 and 2).

GLF1 layout (little endian, offsets from the start of the blob):

    0   "GLF1"
    4   u16 units_per_em, s16 ascender, s16 descender, s16 line_gap
    12  u16 glyph count, u16 cmap count, u32 cmap offset, u32 glyph table offset
    24  u32 kerning offset, u32 kerning count, u32 outline data offset, u32 data size
    40  (8 bytes reserved)
    cmap:    {u32 code point, u16 glyph, u16 0} sorted by code point
    glyphs:  {u32 data offset, u16 advance, s16 xmin, s16 ymin, s16 xmax, s16 ymax, u16 contours}
    kerning: {u16 left glyph, u16 right glyph, s16 value, u16 0} sorted by (left, right)
    outline data, per glyph: the point count of each contour (varint), then per
    point varint(zigzag(dx) << 1 | on_curve) and varint(zigzag(dy)), relative to
    the previous point (the first one to 0,0).

Only the standard library is used (no fontTools).
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

RANGES = [
    (0x20, 0x7E),      # ASCII
    (0xA0, 0x17F),     # Latin-1 Supplement, Latin Extended-A
    (0x18F, 0x18F), (0x192, 0x192), (0x1A0, 0x1B0), (0x218, 0x21B), (0x259, 0x259),
    (0x2C6, 0x2C7), (0x2D8, 0x2DD),
    (0x370, 0x3FF),    # Greek
    (0x400, 0x45F),    # Cyrillic
    (0x490, 0x491),
    (0x1E9E, 0x1E9E),
    (0x2010, 0x2027), (0x2030, 0x2030), (0x2039, 0x203A), (0x2044, 0x2044),
    (0x20AC, 0x20AC), (0x2116, 0x2116), (0x2122, 0x2122),
    (0x2190, 0x2193), (0x2212, 0x2212),
]
KERN_ASCII = range(0x20, 0x7F)


class Font:
    def __init__(self, data: bytes) -> None:
        self.d = data
        if data[:4] not in (b"\x00\x01\x00\x00", b"true"):
            raise SystemExit("not a TrueType (glyf) font")
        n, = struct.unpack_from(">H", data, 4)
        self.tables = {}
        for i in range(n):
            tag, _, off, length = struct.unpack_from(">4sIII", data, 12 + 16 * i)
            self.tables[tag.decode("latin-1")] = (off, length)
        for need in ("head", "hhea", "hmtx", "maxp", "cmap", "loca", "glyf"):
            if need not in self.tables:
                raise SystemExit(f"the font has no '{need}' table")
        head = self.t("head")
        self.upem, = struct.unpack_from(">H", data, head + 18)
        self.loca_long, = struct.unpack_from(">h", data, head + 50)
        self.nglyphs, = struct.unpack_from(">H", data, self.t("maxp") + 4)
        hhea = self.t("hhea")
        self.ascender, self.descender, self.line_gap = struct.unpack_from(">hhh", data, hhea + 4)
        self.nhmetrics, = struct.unpack_from(">H", data, hhea + 34)
        self.cmap = self.read_cmap()

    def t(self, tag: str) -> int:
        return self.tables[tag][0]

    def advance(self, g: int) -> int:
        i = min(g, self.nhmetrics - 1)
        return struct.unpack_from(">H", self.d, self.t("hmtx") + 4 * i)[0]

    def read_cmap(self) -> dict[int, int]:
        base = self.t("cmap")
        n, = struct.unpack_from(">H", self.d, base + 2)
        best = None
        for i in range(n):
            pid, eid, off = struct.unpack_from(">HHI", self.d, base + 4 + 8 * i)
            fmt, = struct.unpack_from(">H", self.d, base + off)
            if (pid, eid, fmt) in ((3, 10, 12), (0, 4, 12)):
                best = (12, base + off)
            elif (pid, eid, fmt) in ((3, 1, 4), (0, 3, 4)) and not best:
                best = (4, base + off)
        if not best:
            raise SystemExit("no Unicode cmap (format 4 or 12)")
        fmt, off = best
        out = {}
        if fmt == 12:
            ngroups, = struct.unpack_from(">I", self.d, off + 12)
            for i in range(ngroups):
                start, end, g = struct.unpack_from(">III", self.d, off + 16 + 12 * i)
                for c in range(start, end + 1):
                    out[c] = g + c - start
        else:
            segx2, = struct.unpack_from(">H", self.d, off + 6)
            seg = segx2 // 2
            ends = struct.unpack_from(f">{seg}H", self.d, off + 14)
            starts = struct.unpack_from(f">{seg}H", self.d, off + 16 + segx2)
            deltas = struct.unpack_from(f">{seg}h", self.d, off + 16 + 2 * segx2)
            ro_pos = off + 16 + 3 * segx2
            ranges = struct.unpack_from(f">{seg}H", self.d, ro_pos)
            for s in range(seg):
                for c in range(starts[s], ends[s] + 1):
                    if c == 0xFFFF:
                        continue
                    if ranges[s] == 0:
                        g = (c + deltas[s]) & 0xFFFF
                    else:
                        p = ro_pos + 2 * s + ranges[s] + 2 * (c - starts[s])
                        g, = struct.unpack_from(">H", self.d, p)
                        if g:
                            g = (g + deltas[s]) & 0xFFFF
                    if g:
                        out[c] = g
        return out

    def glyph_range(self, g: int) -> tuple[int, int]:
        loca = self.t("loca")
        if self.loca_long:
            a, b = struct.unpack_from(">II", self.d, loca + 4 * g)
        else:
            a, b = (2 * v for v in struct.unpack_from(">HH", self.d, loca + 2 * g))
        return a, b

    def outline(self, g: int, depth: int = 0) -> list[list[tuple[int, int, bool]]]:
        """The glyph's contours as (x, y, on_curve) points, composites flattened."""
        a, b = self.glyph_range(g)
        if b <= a:
            return []
        p = self.t("glyf") + a
        ncont, = struct.unpack_from(">h", self.d, p)
        if ncont >= 0:
            return self.simple(p, ncont)
        if depth > 8:
            raise SystemExit(f"glyph {g}: composite nesting too deep")
        return self.composite(p, depth)

    def simple(self, p: int, ncont: int) -> list[list[tuple[int, int, bool]]]:
        d = self.d
        ends = struct.unpack_from(f">{ncont}H", d, p + 10)
        npts = (ends[-1] + 1) if ncont else 0
        ilen, = struct.unpack_from(">H", d, p + 10 + 2 * ncont)
        q = p + 12 + 2 * ncont + ilen
        flags = []
        while len(flags) < npts:
            f = d[q]
            q += 1
            flags.append(f)
            if f & 8:
                rep = d[q]
                q += 1
                flags.extend([f] * rep)
        flags = flags[:npts]
        xs, ys = [], []
        for coords, short, same in ((xs, 2, 16), (ys, 4, 32)):
            v = 0
            for f in flags:
                if f & short:
                    dv = d[q]
                    q += 1
                    v += dv if f & same else -dv
                elif not f & same:
                    dv, = struct.unpack_from(">h", d, q)
                    q += 2
                    v += dv
                coords.append(v)
        contours, start = [], 0
        for e in ends:
            contours.append([(xs[i], ys[i], bool(flags[i] & 1)) for i in range(start, e + 1)])
            start = e + 1
        return contours

    def composite(self, p: int, depth: int) -> list[list[tuple[int, int, bool]]]:
        d = self.d
        q = p + 10
        out = []
        while True:
            flags, gi = struct.unpack_from(">HH", d, q)
            q += 4
            if flags & 1:
                a1, a2 = struct.unpack_from(">hh", d, q)
                q += 4
            else:
                a1, a2 = struct.unpack_from(">bb", d, q)
                q += 2
            if not flags & 2:
                raise SystemExit("composite glyph positioned by point matching: unsupported")
            m = (1.0, 0.0, 0.0, 1.0)
            if flags & 8:
                s, = struct.unpack_from(">h", d, q)
                q += 2
                m = (s / 16384, 0.0, 0.0, s / 16384)
            elif flags & 0x40:
                sx, sy = struct.unpack_from(">hh", d, q)
                q += 4
                m = (sx / 16384, 0.0, 0.0, sy / 16384)
            elif flags & 0x80:
                m = tuple(v / 16384 for v in struct.unpack_from(">hhhh", d, q))
                q += 8
            for c in self.outline(gi, depth + 1):
                out.append([(round(x * m[0] + y * m[2] + a1), round(x * m[1] + y * m[3] + a2), on) for x, y, on in c])
            if not flags & 0x20:
                break
        return out

    # ---- GPOS pair kerning ----

    def coverage(self, off: int) -> list[int]:
        fmt, = struct.unpack_from(">H", self.d, off)
        if fmt == 1:
            n, = struct.unpack_from(">H", self.d, off + 2)
            return list(struct.unpack_from(f">{n}H", self.d, off + 4))
        n, = struct.unpack_from(">H", self.d, off + 2)
        out = []
        for i in range(n):
            s, e, _ = struct.unpack_from(">HHH", self.d, off + 4 + 6 * i)
            out.extend(range(s, e + 1))
        return out

    def classdef(self, off: int) -> dict[int, int]:
        fmt, = struct.unpack_from(">H", self.d, off)
        out = {}
        if fmt == 1:
            start, n = struct.unpack_from(">HH", self.d, off + 2)
            for i, c in enumerate(struct.unpack_from(f">{n}H", self.d, off + 6)):
                out[start + i] = c
        else:
            n, = struct.unpack_from(">H", self.d, off + 2)
            for i in range(n):
                s, e, c = struct.unpack_from(">HHH", self.d, off + 4 + 6 * i)
                for g in range(s, e + 1):
                    out[g] = c
        return out

    @staticmethod
    def value_size(fmt: int) -> int:
        return 2 * bin(fmt).count("1")

    def x_advance(self, off: int, fmt: int) -> int:
        """XAdvance of a ValueRecord (0 when the format has none)."""
        if not fmt & 4:
            return 0
        skip = 2 * bin(fmt & 3).count("1")
        return struct.unpack_from(">h", self.d, off + skip)[0]

    def kerning(self, wanted: set[int]) -> dict[tuple[int, int], int]:
        if "GPOS" not in self.tables:
            return {}
        base = self.t("GPOS")
        d = self.d
        _, _, script_off, feat_off, lookup_off = struct.unpack_from(">HHHHH", d, base)
        feats = base + feat_off
        nfeat, = struct.unpack_from(">H", d, feats)
        lookups = set()
        for i in range(nfeat):
            tag, off = struct.unpack_from(">4sH", d, feats + 2 + 6 * i)
            if tag == b"kern":
                n, = struct.unpack_from(">H", d, feats + off + 2)
                lookups.update(struct.unpack_from(f">{n}H", d, feats + off + 4))
        ll = base + lookup_off
        pairs: dict[tuple[int, int], int] = {}
        for li in sorted(lookups):
            lo = ll + struct.unpack_from(">H", d, ll + 2 + 2 * li)[0]
            ltype, _, nsub = struct.unpack_from(">HHH", d, lo)
            for si in range(nsub):
                so = lo + struct.unpack_from(">H", d, lo + 6 + 2 * si)[0]
                t = ltype
                if t == 9:
                    _, t, ext = struct.unpack_from(">HHI", d, so)
                    so += ext
                if t == 2:
                    self.pair_subtable(so, wanted, pairs)
        return pairs

    def pair_subtable(self, so: int, wanted: set[int], pairs: dict) -> None:
        d = self.d
        fmt, cov_off, vf1, vf2 = struct.unpack_from(">HHHH", d, so)
        cov = self.coverage(so + cov_off)
        rec = self.value_size(vf1) + self.value_size(vf2)
        if fmt == 1:
            nsets, = struct.unpack_from(">H", d, so + 8)
            for ci, first in enumerate(cov[:nsets]):
                if first not in wanted:
                    continue
                ps = so + struct.unpack_from(">H", d, so + 10 + 2 * ci)[0]
                n, = struct.unpack_from(">H", d, ps)
                for k in range(n):
                    r = ps + 2 + k * (2 + rec)
                    second, = struct.unpack_from(">H", d, r)
                    v = self.x_advance(r + 2, vf1)
                    if second in wanted and v and (first, second) not in pairs:
                        pairs[(first, second)] = v
        elif fmt == 2:
            cd1, cd2, n1, n2 = struct.unpack_from(">HHHH", d, so + 8)
            c1 = self.classdef(so + cd1)
            c2 = self.classdef(so + cd2)
            for first in cov:
                if first not in wanted:
                    continue
                k1 = c1.get(first, 0)
                for second in wanted:
                    k2 = c2.get(second, 0)
                    if k1 >= n1 or k2 >= n2:
                        continue
                    r = so + 16 + (k1 * n2 + k2) * rec
                    v = self.x_advance(r, vf1)
                    if v and (first, second) not in pairs:
                        pairs[(first, second)] = v


def varint(v: int) -> bytes:
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def zigzag(v: int) -> int:
    return 2 * v if v >= 0 else -2 * v - 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("ttf", type=Path)
    ap.add_argument("out", type=Path)
    args = ap.parse_args()
    font = Font(args.ttf.read_bytes())

    codes = sorted(c for lo, hi in RANGES for c in range(lo, hi + 1) if c in font.cmap)
    old_ids = [0] + sorted({font.cmap[c] for c in codes} - {0})   # .notdef first
    new_id = {g: i for i, g in enumerate(old_ids)}

    data = bytearray()
    table = bytearray()
    for g in old_ids:
        contours = [c for c in font.outline(g) if c]
        pts = [p for c in contours for p in c]
        if pts:
            xmin, xmax = min(p[0] for p in pts), max(p[0] for p in pts)
            ymin, ymax = min(p[1] for p in pts), max(p[1] for p in pts)
        else:
            xmin = ymin = xmax = ymax = 0
        table += struct.pack("<IHhhhhH", len(data), font.advance(g), xmin, ymin, xmax, ymax, len(contours))
        px = py = 0
        for c in contours:
            data += varint(len(c))
        for c in contours:
            for x, y, on in c:
                data += varint(zigzag(x - px) << 1 | int(on)) + varint(zigzag(y - py))
                px, py = x, y

    wanted = {font.cmap[c] for c in KERN_ASCII if c in font.cmap}
    kern = sorted((new_id[a], new_id[b], v) for (a, b), v in font.kerning(wanted).items() if v)

    cmap = b"".join(struct.pack("<IHH", c, new_id[font.cmap[c]], 0) for c in codes)
    kern_b = b"".join(struct.pack("<HHhH", a, b, v, 0) for a, b, v in kern)
    hdr = 48
    cmap_off = hdr
    glyph_off = cmap_off + len(cmap)
    kern_off = glyph_off + len(table)
    data_off = kern_off + len(kern_b)
    head = struct.pack("<4sHhhhHHIIIIII8x", b"GLF1", font.upem, font.ascender, font.descender, font.line_gap,
                       len(old_ids), len(codes), cmap_off, glyph_off, kern_off, len(kern), data_off, len(data))
    assert len(head) == hdr
    blob = head + cmap + table + kern_b + bytes(data)
    args.out.write_bytes(blob)
    print(f"{args.out}: {len(codes)} code points, {len(old_ids)} glyphs, {len(kern)} kerning pairs, "
          f"{len(blob)} bytes (outlines {len(data)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
