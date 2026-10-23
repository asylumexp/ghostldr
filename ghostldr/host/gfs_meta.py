#!/usr/bin/env python3
"""Names and art for the game selector: a title's <prefix>META container.

Each installed title can have a small save container <P>META next to its
data (docs/ghostfs-catalogue.md).  Its one file, meta.bin, holds what the
selector (docs/selector.md) shows for the title, already converted to the
sizes it draws, so the selector reads one small file instead of searching
the title's index and decoding its PNGs:

  - a copy of the title's sce_sys/param.sfo (the selector picks the name in
    the system language from it);
  - a display name, and where it came from;
  - the tile icon (ICON x ICON) and the full-screen backdrop (1920 x 1080),
    as QOI images (https://qoiformat.org: lossless, decoded by the selector's
    own code in a few milliseconds).

Sources, best first (the selector applies the same order at run time):

  name      the title's param.sfo TITLE; its profile's "name"
            (ghostldr/data/titles); --name; the PlayStation Store's
            (--fetch-art); else the title ID
  icon      the title's sce_sys/icon0.png; --icon FILE; the store's
            (--fetch-art); else the selector draws a tile with the name
  backdrop  the title's sce_sys/pic1.png (or pic0.png); --backdrop FILE; the
            store's (--fetch-art); else the selector draws one in the
            title's colour

--fetch-art reads the PlayStation Store's public web data (no key, no
sign-in), one request a second at most.  The art is copyrighted by its
owners: it goes into build/ (or --art-dir) and onto your own console only.

    python3 ghostldr/host/gfs_meta.py fetch CUSA11456 --out DIR      # icon.png, backdrop.png, store.json
    python3 ghostldr/host/gfs_meta.py build --title CUSA11456 [--src APP0] [--name N] [--icon F]
                                            [--backdrop F] [--fetch-art] --out meta.bin
    python3 ghostldr/host/gfs_meta.py show meta.bin [--extract DIR]

gfs_pack.py and gfs_install.py take the same options; `gfs_install.py
--update-meta` writes only <P>META, for a title that is already installed.

meta.bin, little endian: a 256-byte header, then the sections it points to.

  0x00 char[4] "GFSM"         0x04 u32 version (1)     0x08 u32 header size (256)
  0x0C u32 flags: bit 0 SCANNED (the title's own sce_sys was looked at: what
       it has is in here, so the selector needn't search its index)
  0x10 char[16] title ID      0x20 u64 written (Unix seconds)
  0x28 u8[8] content id: the first 8 bytes of SHA-256 over the title, name
       source, flags, name and everything after the header
  0x30 u32 file size          0x34 u8 name source (NAME_*)
  0x40 char[128] name, UTF-8, NUL padded
  0xC0 3 sections of 16 bytes: param.sfo, icon, backdrop:
       u32 offset, u32 size, u16 width, u16 height, u8 format (1 raw, 2 QOI),
       u8 source (ART_*), u16 0
  0xF0 char[16] version (APP_VER, or the profile's), may be empty
"""

from __future__ import annotations

import argparse
import array
import hashlib
import io
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zlib
from dataclasses import dataclass, field
from pathlib import Path

import gfs_catalog as gc

ROOT = Path(__file__).resolve().parents[2]
OUT_ROOT = ROOT / "build/ghostfs"

MAGIC = b"GFSM"
VERSION = 1
HEADER_SIZE = 256
META_FILE = "meta.bin"
FLAG_SCANNED = 1
ICON = 384                        # the selector's focused tile (selector/src/sel.h TILE_BIG)
BACKDROP = (1920, 1080)
FMT_RAW, FMT_QOI = 1, 2
SECT_SFO, SECT_ICON, SECT_BACKDROP = range(3)
SECTION = struct.Struct("<IIHHBBH")
# where a name or a picture came from (selector/src/plat.h NAME_*, sel.h ART_*)
NAME_SFO, NAME_PROFILE, NAME_GIVEN, NAME_STORE = 1, 2, 3, 4
ART_TITLE, ART_GIVEN, ART_STORE = 1, 2, 3
NAME_SOURCES = {0: "none", NAME_SFO: "param.sfo", NAME_PROFILE: "profile", NAME_GIVEN: "given",
                NAME_STORE: "PlayStation Store"}
ART_SOURCES = {0: "none", ART_TITLE: "the title's sce_sys", ART_GIVEN: "given", ART_STORE: "PlayStation Store"}
NAME_BYTES = 127
SFO_MAX = 64 << 10


class ArtError(Exception):
    pass


# ---- images ----

@dataclass
class Image:
    w: int
    h: int
    rgba: bytes          # w * h * 4, row by row


def _pil():
    try:
        from PIL import Image as PILImage
        return PILImage
    except ImportError:
        return None


PIL = _pil()


def decode_image(data: bytes, what: str = "image") -> Image:
    """PNG, JPEG, WebP ... as RGBA.  Pillow when installed; else PNG in pure
    Python, and other formats through macOS's sips."""
    if PIL:
        try:
            im = PIL.open(io.BytesIO(data))
            im.load()
        except Exception as e:  # noqa: BLE001 - Pillow raises many kinds
            raise ArtError(f"{what}: not a picture Pillow can read ({e})") from None
        im = im.convert("RGBA")
        return Image(im.width, im.height, im.tobytes())
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return png_decode(data, what)
    if shutil.which("sips"):
        with tempfile.TemporaryDirectory() as tmp:
            src, dst = Path(tmp) / "in", Path(tmp) / "out.png"
            src.write_bytes(data)
            r = subprocess.run(["sips", "-s", "format", "png", str(src), "--out", str(dst)], capture_output=True)
            if r.returncode == 0 and dst.is_file():
                return png_decode(dst.read_bytes(), what)
    raise ArtError(f"{what}: only PNG can be read without Pillow (pip install pillow)")


def png_decode(data: bytes, what: str = "PNG") -> Image:
    """8-bit (or 16-bit) non-interlaced PNG of any colour type, in pure Python."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ArtError(f"{what}: not a PNG file")
    pos, idat, plte, trns, hdr = 8, [], b"", b"", None
    while pos + 8 <= len(data):
        n, kind = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            plte = body
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if not hdr:
        raise ArtError(f"{what}: no IHDR")
    w, h, depth, ctype, _, _, interlace = hdr
    if interlace or depth not in (8, 16) and not (ctype == 3 and depth == 8):
        raise ArtError(f"{what}: interlaced or {depth}-bit PNGs need Pillow (pip install pillow)")
    chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = chans * depth // 8
    stride = w * bpp
    raw = zlib.decompress(b"".join(idat))
    out = bytearray(h * stride)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif f == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b, c = prev[i], prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    if depth == 16:
        out = out[0::2]
    px = bytearray(w * h * 4)
    if ctype == 6:
        px[:] = out
    elif ctype == 2:
        px[0::4], px[1::4], px[2::4] = out[0::3], out[1::3], out[2::3]
        px[3::4] = b"\xff" * (w * h)
    elif ctype == 0 or ctype == 4:
        g = out[0::chans]
        px[0::4] = px[1::4] = px[2::4] = g
        px[3::4] = out[1::2] if ctype == 4 else b"\xff" * (w * h)
    else:
        pal = [plte[i:i + 3] + (bytes([trns[i // 3]]) if i // 3 < len(trns) else b"\xff")
               for i in range(0, len(plte), 3)]
        px = bytearray(b"".join(pal[i] for i in out))
    return Image(w, h, bytes(px))


def flatten(img: Image, bg: tuple[int, int, int] = (0, 0, 0)) -> Image:
    """Alpha composited over bg: the selector's art is opaque."""
    if all(a == 255 for a in img.rgba[3::4]):
        return img
    px = bytearray(img.rgba)
    for i in range(0, len(px), 4):
        a = px[i + 3]
        for c in range(3):
            px[i + c] = (px[i + c] * a + bg[c] * (255 - a) + 127) // 255
        px[i + 3] = 255
    return Image(img.w, img.h, bytes(px))


def cover(img: Image, w: int, h: int) -> Image:
    """Scaled to cover w x h (keeping its aspect), centred and cropped."""
    if (img.w, img.h) == (w, h):
        return img
    if img.w * h >= img.h * w:
        cw, ch = img.h * w // h, img.h
    else:
        cw, ch = img.w, img.w * h // w
    x0, y0 = (img.w - cw) // 2, (img.h - ch) // 2
    if PIL:
        im = PIL.frombytes("RGBA", (img.w, img.h), img.rgba).crop((x0, y0, x0 + cw, y0 + ch))
        return Image(w, h, im.resize((w, h), PIL.LANCZOS).tobytes())
    return _resample(img, x0, y0, cw, ch, w, h)


def _resample(img: Image, x0: int, y0: int, cw: int, ch: int, w: int, h: int) -> Image:
    """Area average when shrinking, nearest when growing (pure Python)."""
    src, out = img.rgba, bytearray(w * h * 4)
    for y in range(h):
        sy0 = y0 + y * ch // h
        sy1 = max(sy0 + 1, y0 + (y + 1) * ch // h)
        for x in range(w):
            sx0 = x0 + x * cw // w
            sx1 = max(sx0 + 1, x0 + (x + 1) * cw // w)
            acc, n = [0, 0, 0, 0], 0
            for yy in range(sy0, sy1):
                row = yy * img.w * 4
                for xx in range(sx0, sx1):
                    o = row + xx * 4
                    acc[0] += src[o]
                    acc[1] += src[o + 1]
                    acc[2] += src[o + 2]
                    acc[3] += src[o + 3]
                    n += 1
            o = (y * w + x) * 4
            out[o:o + 4] = bytes(v // n for v in acc)
    return Image(w, h, bytes(out))


def qoi_encode(img: Image) -> bytes:
    """The QOI format (qoiformat.org), 4 channels."""
    px = array.array("I", img.rgba)
    if sys.byteorder != "little":
        px.byteswap()
    out = bytearray(struct.pack(">4sIIBB", b"qoif", img.w, img.h, 4, 0))
    index = [0] * 64
    prev, run = 0xFF000000, 0                   # r | g << 8 | b << 16 | a << 24, starting at opaque black
    for p in px:
        if p == prev:
            run += 1
            if run == 62:
                out.append(0xC0 | 61)
                run = 0
            continue
        if run:
            out.append(0xC0 | (run - 1))
            run = 0
        r, g, b, a = p & 255, (p >> 8) & 255, (p >> 16) & 255, p >> 24
        h = (r * 3 + g * 5 + b * 7 + a * 11) & 63
        if index[h] == p:
            out.append(h)
        else:
            index[h] = p
            if a == prev >> 24:
                dr = ((r - (prev & 255) + 128) & 255) - 128
                dg = ((g - ((prev >> 8) & 255) + 128) & 255) - 128
                db = ((b - ((prev >> 16) & 255) + 128) & 255) - 128
                if -2 <= dr <= 1 and -2 <= dg <= 1 and -2 <= db <= 1:
                    out.append(0x40 | (dr + 2) << 4 | (dg + 2) << 2 | (db + 2))
                elif -32 <= dg <= 31 and -8 <= dr - dg <= 7 and -8 <= db - dg <= 7:
                    out += bytes((0x80 | (dg + 32), (dr - dg + 8) << 4 | (db - dg + 8)))
                else:
                    out += bytes((0xFE, r, g, b))
            else:
                out += bytes((0xFF, r, g, b, a))
        prev = p
    if run:
        out.append(0xC0 | (run - 1))
    return bytes(out + b"\0" * 7 + b"\1")


def qoi_decode(data: bytes) -> Image:
    """For tests and `show --extract` (the selector has its own, selector/src/qoi.c)."""
    magic, w, h, _, _ = struct.unpack_from(">4sIIBB", data)
    if magic != b"qoif":
        raise ArtError("not a QOI image")
    out, index = bytearray(w * h * 4), [(0, 0, 0, 0)] * 64
    r, g, b, a = 0, 0, 0, 255
    pos, n, run = 14, 0, 0
    while n < w * h:
        if run:
            run -= 1
        else:
            c = data[pos]
            pos += 1
            if c == 0xFE:
                r, g, b = data[pos:pos + 3]
                pos += 3
            elif c == 0xFF:
                r, g, b, a = data[pos:pos + 4]
                pos += 4
            elif c >> 6 == 0:
                r, g, b, a = index[c]
            elif c >> 6 == 1:
                r, g, b = (r + (c >> 4 & 3) - 2) & 255, (g + (c >> 2 & 3) - 2) & 255, (b + (c & 3) - 2) & 255
            elif c >> 6 == 2:
                d = data[pos]
                pos += 1
                dg = (c & 63) - 32
                r, g, b = (r + dg + (d >> 4) - 8) & 255, (g + dg) & 255, (b + dg + (d & 15) - 8) & 255
            else:
                run = c & 63
            index[(r * 3 + g * 5 + b * 7 + a * 11) & 63] = (r, g, b, a)
        out[n * 4:n * 4 + 4] = bytes((r, g, b, a))
        n += 1
    return Image(w, h, bytes(out))


def png_encode(img: Image) -> bytes:
    """A plain RGBA PNG (fetched art and `show --extract`)."""
    if PIL:
        buf = io.BytesIO()
        PIL.frombytes("RGBA", (img.w, img.h), img.rgba).save(buf, "PNG")
        return buf.getvalue()
    rows = b"".join(b"\0" + img.rgba[y * img.w * 4:(y + 1) * img.w * 4] for y in range(img.h))

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", img.w, img.h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


# ---- param.sfo ----

def sfo_string(sfo: bytes | None, key: str = "TITLE") -> str:
    """A string of a param.sfo (TITLE, APP_VER ...), '' when there is none."""
    if not sfo or sfo[:4] != b"\0PSF":
        return ""
    try:
        kt, dt, n = struct.unpack_from("<III", sfo, 8)
        for i in range(min(n, 256)):
            ko, fmt, ln, _, do = struct.unpack_from("<HHIII", sfo, 20 + 16 * i)
            if sfo[kt + ko:sfo.index(b"\0", kt + ko)] == key.encode() and fmt == 0x0204:
                return sfo[dt + do:dt + do + ln].split(b"\0")[0].decode("utf-8", "replace").strip()
    except (struct.error, ValueError):
        pass
    return ""


# ---- meta.bin ----

@dataclass
class Meta:
    title: str
    name: str = ""
    name_source: int = 0
    sfo: bytes | None = None
    version: str = ""
    icon: Image | None = None
    icon_source: int = 0
    backdrop: Image | None = None
    backdrop_source: int = 0
    scanned: bool = True
    notes: list[str] = field(default_factory=list)   # what was chosen and why (printed by the tools)


def utf8_cut(text: str, limit: int) -> str:
    raw = text.encode()
    return raw[:limit].decode("utf-8", "ignore") if len(raw) > limit else text


def build(m: Meta, now: int | None = None) -> bytes:
    """meta.bin for m (images are converted to the selector's sizes here)."""
    sections, body = [], b""

    def add(data: bytes | None, w: int, h: int, fmt: int, source: int) -> None:
        nonlocal body
        if not data:
            sections.append(SECTION.pack(0, 0, 0, 0, 0, 0, 0))
            return
        sections.append(SECTION.pack(HEADER_SIZE + len(body), len(data), w, h, fmt, source, 0))
        body += data
        body += b"\0" * (-len(body) % 16)

    sfo = m.sfo if m.sfo and len(m.sfo) <= SFO_MAX else None
    add(sfo, 0, 0, FMT_RAW, ART_TITLE)
    icon = flatten(cover(m.icon, ICON, ICON)) if m.icon else None
    add(qoi_encode(icon) if icon else None, ICON, ICON, FMT_QOI, m.icon_source)
    bd = flatten(cover(m.backdrop, *BACKDROP)) if m.backdrop else None
    add(qoi_encode(bd) if bd else None, *BACKDROP, FMT_QOI, m.backdrop_source)
    name = utf8_cut(m.name, NAME_BYTES).encode()
    flags = FLAG_SCANNED if m.scanned else 0
    version = m.version.encode()[:15]
    digest = hashlib.sha256(m.title.encode() + bytes([m.name_source, flags]) + name + b"\0" + version + b"\0"
                            + body).digest()[:8]
    header = struct.pack("<4sIII16sQ8sIB11x128s", MAGIC, VERSION, HEADER_SIZE, flags,
                         m.title.encode()[:15], int(time.time()) if now is None else now, digest,
                         HEADER_SIZE + len(body), m.name_source, name) + b"".join(sections) + version.ljust(16, b"\0")
    assert len(header) == HEADER_SIZE
    return header + body


def parse(data: bytes | None) -> dict | None:
    """The header of a meta.bin (None when it isn't one)."""
    if not data or len(data) < HEADER_SIZE or data[:4] != MAGIC:
        return None
    _, version, hsize, flags, title, written, digest, size, nsrc, name = struct.unpack_from(
        "<4sIII16sQ8sIB11x128s", data)
    if hsize < HEADER_SIZE:
        return None
    sects = [SECTION.unpack_from(data, 0xC0 + 16 * i) for i in range(3)]
    return {"version": version, "flags": flags, "title": title.split(b"\0")[0].decode(errors="replace"),
            "written": written, "id": digest.hex(), "size": size, "name_source": nsrc,
            "name": name.split(b"\0")[0].decode(errors="replace"),
            "version": data[0xF0:0x100].split(b"\0")[0].decode(errors="replace"),
            "sections": [dict(zip(("offset", "size", "w", "h", "format", "source"), s[:6])) for s in sects]}


def describe(info: dict) -> str:
    s = info["sections"]
    parts = [f"name {info['name']!r} ({NAME_SOURCES.get(info['name_source'], '?')})"]
    for i, what in ((SECT_ICON, "icon"), (SECT_BACKDROP, "backdrop")):
        parts.append(f"{what} {s[i]['w']}x{s[i]['h']} {s[i]['size'] >> 10} KiB ({ART_SOURCES.get(s[i]['source'], '?')})"
                     if s[i]["size"] else f"no {what}")
    parts.append("param.sfo copy" if s[SECT_SFO]["size"] else "no param.sfo")
    if info.get("version"):
        parts.append(f"version {info['version']}")
    return f"{info['title']}: " + ", ".join(parts) + f"; {info['size'] >> 10} KiB"


def container_blocks(size: int) -> int:
    """32 KiB blocks for a <P>META holding `size` bytes (small containers lose a lot to the filesystem)."""
    return max(512, (2 * size + (4 << 20) + 32767) // 32768)


# ---- the PlayStation Store's public data (no key, no sign-in) ----

STORE_REGIONS = (("US", "en"), ("GB", "en"), ("JP", "ja"), ("HK", "en"))
CHIHIRO = "https://store.playstation.com/store/api/chihiro/00_09_000/titlecontainer/{cc}/{lang}/999/{title}_00"
CATALOG = "https://m.np.playstation.com/api/catalog/v2/titles/{title}_00/concepts/?country={cc}&language={lang}&age=999"
USER_AGENT = "Mozilla/5.0 (compatible; ghostldr)"
_last_request = [0.0]


def http_get(url: str, timeout: float = 30) -> bytes | None:
    """One GET, at most one a second; None on any HTTP or network error."""
    wait = 1.0 - (time.time() - _last_request[0])
    if wait > 0:
        time.sleep(wait)
    try:
        req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": "*/*"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()
    except (urllib.error.URLError, OSError, ValueError):
        return None
    finally:
        _last_request[0] = time.time()


def _json(data: bytes | None):
    try:
        return json.loads(data) if data else None
    except ValueError:
        return None


def store_lookup(title: str, get=http_get, log=print) -> dict:
    """Where the store keeps the title's square key art and its background:
    {"name", "icon_url", "backdrop_url", "source"}; the keys it found."""
    found: dict = {}
    for cc, lang in STORE_REGIONS:
        d = _json(get(CHIHIRO.format(cc=cc, lang=lang, title=title)))
        if isinstance(d, dict) and isinstance(d.get("images"), list):
            imgs = {i.get("type"): i.get("url") for i in d["images"] if isinstance(i, dict) and i.get("url")}
            got = {"name": d.get("title_name") or d.get("name") or "", "icon_url": imgs.get(10) or imgs.get(1),
                   "backdrop_url": imgs.get(12) or imgs.get(13), "source": f"store {cc}/{lang} title container"}
            for k, v in got.items():
                if v and not found.get(k):
                    found[k] = v
        if found.get("icon_url") and found.get("backdrop_url"):
            break
        d = _json(get(CATALOG.format(cc=cc, lang=lang, title=title)))
        if isinstance(d, list) and d and isinstance(d[0], dict):
            imgs = {i.get("type"): i.get("url") for i in (d[0].get("media") or {}).get("images", [])
                    if isinstance(i, dict) and i.get("url")}
            got = {"name": d[0].get("name") or "", "icon_url": imgs.get("MASTER"),
                   "backdrop_url": imgs.get("GAMEHUB_COVER_ART") or imgs.get("BACKGROUND_LAYER_ART"),
                   "source": f"store {cc}/{lang} catalog concept"}
            for k, v in got.items():
                if v and not found.get(k):
                    found[k] = v
        if found.get("icon_url") and found.get("backdrop_url"):
            break
    return found


def fetch_art(title: str, art_dir: Path, refetch: bool = False, get=http_get, log=print) -> dict:
    """The store's name, icon and backdrop for `title`, saved in art_dir
    (store.json, icon.png, backdrop.png) and reused from there next time.
    Returns {"name": str, "icon": bytes | None, "backdrop": bytes | None}."""
    art_dir = Path(art_dir)
    info_file = art_dir / "store.json"
    if not refetch and info_file.is_file():
        info = _json(info_file.read_bytes()) or {}
        if info.get("title") == title:
            out = {"name": info.get("name", ""), "icon": None, "backdrop": None}
            for k in ("icon", "backdrop"):
                if (art_dir / f"{k}.png").is_file():
                    out[k] = (art_dir / f"{k}.png").read_bytes()
            log(f"store art for {title}: reusing {art_dir}")
            return out
    log(f"store art for {title}: asking the PlayStation Store")
    found = store_lookup(title, get, log)
    out = {"name": found.get("name", ""), "icon": None, "backdrop": None}
    art_dir.mkdir(parents=True, exist_ok=True)
    for k in ("icon", "backdrop"):
        url = found.get(f"{k}_url")
        data = get(url) if url else None
        if data:
            try:
                decode_image(data, f"store {k}")
            except ArtError as e:
                log(f"  {e}")
                continue
            out[k] = data if data[:8] == b"\x89PNG\r\n\x1a\n" else png_encode(decode_image(data))
            (art_dir / f"{k}.png").write_bytes(out[k])
    info_file.write_text(json.dumps({"title": title, "name": out["name"], "source": found.get("source", ""),
                                     "icon_url": found.get("icon_url", ""),
                                     "backdrop_url": found.get("backdrop_url", ""),
                                     "fetched": int(time.time())}, indent=1))
    log(f"  {found.get('source') or 'nothing found'}: name {out['name']!r}, "
        f"icon {'yes' if out['icon'] else 'no'}, backdrop {'yes' if out['backdrop'] else 'no'} -> {art_dir}")
    return out


# ---- choosing ----

def own_files_from_dir(src: Path | None) -> tuple[dict[str, bytes], bool]:
    """The title's own sce_sys files in a dump, and whether the dump was there to look at."""
    if not src or not (Path(src) / "eboot.bin").is_file():
        return {}, False
    out = {}
    for f in ("param.sfo", "icon0.png", "pic1.png", "pic0.png"):
        p = Path(src) / "sce_sys" / f
        if p.is_file():
            out[f] = p.read_bytes()
    return out, True


def choose(title: str, own: dict[str, bytes], scanned: bool, profile_name: str = "", name: str | None = None,
           icon: Path | None = None, backdrop: Path | None = None, fetch: bool = False,
           art_dir: Path | None = None, refetch: bool = False, get=http_get, log=print,
           profile_version: str = "") -> Meta:
    """The name and art for `title`, best source first (the module docstring)."""
    m = Meta(title, scanned=scanned)
    sfo = own.get("param.sfo")
    if sfo and len(sfo) <= SFO_MAX:
        m.sfo = sfo
    m.version = sfo_string(sfo, "APP_VER") or profile_version
    picks = [(sfo_string(sfo), NAME_SFO), (profile_name, NAME_PROFILE), (name or "", NAME_GIVEN)]
    m.name, m.name_source = next(((n, s) for n, s in picks if n), ("", 0))
    if name and m.name_source < NAME_GIVEN:
        m.notes.append(f"--name is not used: the title's {NAME_SOURCES[m.name_source]} names it {m.name!r}")

    def own_image(files: tuple[str, ...], what: str) -> Image | None:
        for f in files:
            if f in own:
                try:
                    return decode_image(own[f], f"sce_sys/{f}")
                except ArtError as e:
                    m.notes.append(f"{e}: not used")
        return None

    def given(path: Path | None, what: str) -> Image | None:
        if not path:
            return None
        try:
            return decode_image(Path(path).read_bytes(), str(path))
        except OSError as e:
            raise ArtError(f"--{what} {path}: {e.strerror}") from None

    m.icon = own_image(("icon0.png",), "icon")
    m.icon_source = ART_TITLE if m.icon else 0
    m.backdrop = own_image(("pic1.png", "pic0.png"), "backdrop")
    m.backdrop_source = ART_TITLE if m.backdrop else 0
    for what in ("icon", "backdrop"):
        path = icon if what == "icon" else backdrop
        if getattr(m, what) is None:
            img = given(path, what)
            if img:
                setattr(m, what, img)
                setattr(m, f"{what}_source", ART_GIVEN)
        elif path:
            m.notes.append(f"--{what} is not used: the title has its own")
    if fetch:
        if m.icon and m.backdrop and m.name:
            m.notes.append("--fetch-art: nothing missing, the store was not asked")
        else:
            store = fetch_art(title, art_dir or OUT_ROOT / title / "art", refetch, get, log)
            if not m.name and store["name"]:
                m.name, m.name_source = store["name"], NAME_STORE
            for what in ("icon", "backdrop"):
                if getattr(m, what) is None and store[what]:
                    setattr(m, what, decode_image(store[what], f"store {what}"))
                    setattr(m, f"{what}_source", ART_STORE)
    return m


def profile_fields(title: str) -> tuple[str, str]:
    """The profile's name and version ('' when it has none)."""
    import profiles
    try:
        prof = profiles.raw_profile(title)
    except profiles.ProfileError:
        return "", ""
    return str(prof.get("name") or ""), str(prof.get("version") or "")


def summary(m: Meta) -> str:
    def art(img, src):
        return ART_SOURCES[src] if img else "none (the selector draws one)"
    return (f"{m.title}: name {m.name or m.title!r} ({NAME_SOURCES[m.name_source] if m.name else 'the title ID'}), "
            f"icon: {art(m.icon, m.icon_source)}, backdrop: {art(m.backdrop, m.backdrop_source)}")


# ---- packing (gfs_pack.py, gfs_install.py) ----

def write_pack_meta(info: dict, out: Path, prof: dict, name: str | None = None, icon: Path | None = None,
               backdrop: Path | None = None, fetch: bool = False, art_dir: Path | None = None,
               refetch: bool = False, log=print) -> dict | None:
    """out/meta.bin for the packed title (gfs_meta.py): its name and art for
    the game selector, recorded in layout.json as "meta".  Nothing worth
    storing (no param.sfo, no art, no name but its profile's, which run.py
    sends anyway): no meta.bin."""
    src = Path(info["src"]) if info.get("src") else None
    own, scanned = own_files_from_dir(src)
    m = choose(info["title"], own, scanned, prof.get("name") or "", name, icon, backdrop, fetch, art_dir,
                  refetch, log=log, profile_version=prof.get("version") or "")
    for note in m.notes:
        log(note)
    meta = out / META_FILE
    if m.name:
        info["name"] = m.name               # the catalogue's copy
    if m.sfo or m.icon or m.backdrop or m.name_source in (NAME_GIVEN, NAME_STORE):
        data = build(m)
        meta.write_bytes(data)
        head = parse(data)
        info["meta"] = {"id": head["id"], "size": len(data), "file": str(meta), "summary": summary(m)}
    else:
        meta.unlink(missing_ok=True)
        info.pop("meta", None)
    (out / "layout.json").write_text(json.dumps(info, indent=1))
    return info.get("meta")



def add_args(ap: argparse.ArgumentParser) -> None:
    """--name, --icon, --backdrop, --fetch-art, --art-dir, --refetch (gfs_pack.py and gfs_install.py)."""
    g = ap.add_argument_group("the game selector's name and art (docs/selector.md)")
    g.add_argument("--name", help="display name, used when the title has no param.sfo and no profile name")
    g.add_argument("--icon", type=Path, help="tile picture (square; PNG, or JPEG with Pillow), used when the "
                   "title has no sce_sys/icon0.png")
    g.add_argument("--backdrop", type=Path, help="background picture (16:9), used when the title has no "
                   "sce_sys/pic1.png")
    g.add_argument("--fetch-art", action="store_true", help="download what is still missing (icon, backdrop, "
                   "name) from the PlayStation Store's public data, by title ID")
    g.add_argument("--art-dir", type=Path, help="where --fetch-art keeps what it downloads "
                   "(default build/ghostfs/<TITLE>/art)")
    g.add_argument("--refetch", action="store_true", help="with --fetch-art: ask the store again")


# ---- the console side (gfs_install.py): <P>META through gfs_receiver.lua ----

def meta_target(cat: gc.Catalogue, title: str, prefix: str) -> str:
    """<prefix>META, after checking it can't be anything else: never a data
    container, an index, the catalogue, a title's progress or SWRR's save."""
    name = gc.meta_name(prefix)
    taken = {gc.CATALOG, *gc.LEGACY_INDEXES, *gc.HOST_SAVES}
    for e in cat.entries:
        taken |= {e.index, gc.index_name(e.prefix), gc.save_name(e.prefix)}
    bad = (not gc.PREFIX_RE.fullmatch(prefix) or name in taken or name.startswith("GFS") or name.endswith("SAVE")
           or any(name == gc.data_name(e.prefix, k) for e in cat.entries for k in range(gc.MAX_CONTAINERS))
           or (cat.by_prefix(prefix) and cat.by_prefix(prefix).title != title))
    if bad:
        raise SystemExit(f"refusing to write {name!r} for {title}: it could be another container")
    return name


def put_meta(rx, cat: gc.Catalogue, title: str, prefix: str, data: bytes) -> int:
    """Write <prefix>META whole (PUT: delete and create): the only container
    --update-meta ever writes."""
    name = meta_target(cat, title, prefix)
    blocks = container_blocks(len(data))
    return rx.put(name, META_FILE, blocks, len(data), [data[i:i + READ_CHUNK] for i in range(0, len(data), READ_CHUNK)])[0]


READ_CHUNK = 4 << 20                 # the receiver's READ and WRITE limit


def read_all(rx, dirname: str, fname: str, size: int | None = None, offset: int = 0) -> bytes | None:
    """A file (or `size` bytes of it from `offset`), in READ-sized pieces."""
    out = bytearray()
    while size is None or len(out) < size:
        want = READ_CHUNK if size is None else min(READ_CHUNK, size - len(out))
        part = rx.read(dirname, fname, want, offset + len(out))
        if part is None:
            return None if not out else bytes(out)
        out += part
        if len(part) < want:
            break
    return bytes(out)


OWN_FILES = ("param.sfo", "icon0.png", "pic1.png", "pic0.png")


def installed_files(rx, entry: gc.Entry) -> tuple[dict[str, bytes], bool]:
    """The title's own sce_sys files, read back from the console through its
    index (no dump needed): ({name: bytes}, whether its index was readable)."""
    from gfs_install import parse_header
    idx = read_all(rx, entry.index, gc.INDEX_FILE)
    h = parse_header(entry.index, idx)
    if not h or h.incomplete:
        return {}, False
    hsize = struct.unpack_from("<I", idx, 56)[0] if h.version >= 2 else 64
    cap, = struct.unpack_from("<Q", idx, 16)
    names_off, = struct.unpack_from("<I", idx, 32)
    want = {f"sce_sys/{f}".encode(): f for f in OWN_FILES}
    found: dict[str, bytes] = {}
    for i in range(h.entries):
        off, size, name, flags = struct.unpack_from("<QQII", idx, hsize + 24 * i)
        key = idx[names_off + name:idx.index(b"\0", names_off + name)]
        if key not in want or flags & 1 or not size or size > 32 << 20:
            continue
        data = bytearray()
        while len(data) < size:
            k, local = divmod(off + len(data), cap)
            part = read_all(rx, gc.data_name(h.prefix, k), "data.bin", min(size - len(data), cap - local), local)
            if not part:
                break
            data += part
        if len(data) == size:
            found[want[key]] = bytes(data)
    return found, True


def update_meta(rx, args, log=print) -> int:
    """gfs_install.py --update-meta: work out the title's name and art and
    write <P>META; nothing else on the console changes (the catalogue is not
    even written)."""
    from gfs_install import SAVE_ERRORS, read_catalogue
    cat, _, _ = read_catalogue(rx)
    if args.title:
        entry = cat.find(args.title.upper())
    elif len(cat.entries) == 1:
        entry = cat.entries[0]
    else:
        raise SystemExit(f"pass --title: {', '.join(e.title for e in cat.entries) or 'nothing'} installed")
    if not entry:
        have = ", ".join(e.title for e in cat.entries) or "nothing"
        raise SystemExit(f"{args.title.upper()} is not installed (installed: {have})")
    title, prefix = entry.title, entry.prefix
    target = meta_target(cat, title, prefix)
    src = args.src or (Path(json.loads((OUT_ROOT / title / "layout.json").read_text())["src"])
                       if (OUT_ROOT / title / "layout.json").is_file() else None)
    own, scanned = own_files_from_dir(src)
    if scanned:
        log(f"{title}: its sce_sys from {src}: {', '.join(own) or 'none'}")
    else:
        own, scanned = installed_files(rx, entry)
        log(f"{title}: its sce_sys from the console ({entry.index}): "
            f"{', '.join(own) or 'none'}{'' if scanned else ' (index unreadable)'}")
    pname, pver = profile_fields(title)
    try:
        m = choose(title, own, scanned, pname, args.name, args.icon, args.backdrop, args.fetch_art, args.art_dir,
                      args.refetch, log=log, profile_version=pver)
    except ArtError as e:
        raise SystemExit(str(e))
    for note in m.notes:
        log(note)
    log(summary(m))
    if not (m.sfo or m.icon or m.backdrop or m.name_source in (NAME_GIVEN, NAME_STORE)):
        log("nothing to store: the selector shows the profile's name (or the title ID) and draws the art "
            "(--fetch-art, --icon, --backdrop and --name add some)")
        return 0
    data = build(m)
    (OUT_ROOT / title).mkdir(parents=True, exist_ok=True)
    (OUT_ROOT / title / META_FILE).write_bytes(data)
    have = parse(rx.read(target, META_FILE, HEADER_SIZE))
    if have and have["id"] == parse(data)["id"] and not args.force:
        log(f"{target} is up to date: nothing to write")
        return 0
    log(f"{'replace' if have else 'create'} {target} ({len(data) >> 10} KiB); nothing else changes")
    if args.dry_run:
        log("dry run: nothing changed")
        return 0
    rc = put_meta(rx, cat, title, prefix, data)
    if rc:
        why = SAVE_ERRORS.get(rc, "")
        raise SystemExit(f"{target}: receiver status {rc:#x}{' (' + why + ')' if why else ''}")
    log(f"{target} written: {describe(parse(data))}")
    return 0


# ---- command line ----

def main() -> int:
    ap = argparse.ArgumentParser(description="names and art for the game selector (<prefix>META)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch", help="download a title's icon, backdrop and name from the PlayStation Store")
    f.add_argument("title")
    f.add_argument("--out", type=Path, required=True, help="directory for icon.png, backdrop.png and store.json")
    f.add_argument("--refetch", action="store_true", help="ask the store again even if --out has the art")
    b = sub.add_parser("build", help="write a meta.bin")
    b.add_argument("--title", required=True)
    b.add_argument("--src", type=Path, help="the title's app0, for its own sce_sys files")
    b.add_argument("--name")
    b.add_argument("--icon", type=Path)
    b.add_argument("--backdrop", type=Path)
    b.add_argument("--fetch-art", action="store_true")
    b.add_argument("--art-dir", type=Path)
    b.add_argument("--out", type=Path, required=True)
    s = sub.add_parser("show", help="describe a meta.bin")
    s.add_argument("file", type=Path)
    s.add_argument("--extract", type=Path, help="write its pictures there as PNG")
    args = ap.parse_args()
    try:
        if args.cmd == "fetch":
            got = fetch_art(args.title.upper(), args.out, args.refetch)
            return 0 if got["icon"] or got["backdrop"] else 1
        if args.cmd == "build":
            title = args.title.upper()
            own, scanned = own_files_from_dir(args.src)
            pname, pver = profile_fields(title)
            m = choose(title, own, scanned, pname, args.name, args.icon, args.backdrop, args.fetch_art,
                       args.art_dir, profile_version=pver)
            for n in m.notes:
                print(n)
            args.out.write_bytes(build(m))
            print(describe(parse(args.out.read_bytes())))
            return 0
        data = args.file.read_bytes()
        info = parse(data)
        if not info:
            raise SystemExit(f"{args.file}: not a meta.bin")
        print(describe(info))
        if args.extract:
            args.extract.mkdir(parents=True, exist_ok=True)
            for i, what in ((SECT_ICON, "icon"), (SECT_BACKDROP, "backdrop")):
                sct = info["sections"][i]
                if sct["size"]:
                    img = qoi_decode(data[sct["offset"]:sct["offset"] + sct["size"]])
                    (args.extract / f"{what}.png").write_bytes(png_encode(img))
            sct = info["sections"][SECT_SFO]
            if sct["size"]:
                (args.extract / "param.sfo").write_bytes(data[sct["offset"]:sct["offset"] + sct["size"]])
        return 0
    except ArtError as e:
        raise SystemExit(str(e))


if __name__ == "__main__":
    raise SystemExit(main())
