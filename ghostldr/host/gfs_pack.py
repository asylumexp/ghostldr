#!/usr/bin/env python3
"""Lay a title's app0 out over SWRR save containers ("ghostfs").

SWRR (no kernel exploit) can own many save containers of at most 1 GiB each
(32768 blocks) and mount 15 at once.  The title's files are concatenated in
one linear byte space that is cut into CAP-sized pieces, one per container
(<PREFIX>000, <PREFIX>001, ... each holding data.bin); a sorted index
(GFSINDEX, index.bin) maps every path to its offset and size.  The title's own
saves later go into <PREFIX>SAVE, so every title needs its own prefix.

Order matters because only ~12 containers stay mounted: the executable and
modules first, then (with a profile) the files the title opened in earlier
boots, then the rest grouped by directory.  CUSA11456 (Ghost of Tsushima) has
a profile: its traced open order (ghostldr/data/cusa11456-open-order.txt), the
audio archives after it, area packs by region, and the non-English audio
dropped.  Its layout is the one installed since 2026-10-20 (prefix GHOST).

    python3 ghostldr/host/gfs_pack.py                          # CUSA11456 from build/ghost/app0
    python3 ghostldr/host/gfs_pack.py --src DIR --title CUSA12345 [--prefix NAME]

Writes build/ghostfs/<TITLE>/layout.json and index.bin.

Index (little endian): 128-byte header "GFS1", version 2, containers, entries,
cap u64, total u64, names_off, names_size, prefix[16] @40, header_size @56,
flags @60 (1 = install incomplete), title[16] @64, layout id u64 @80; then
entries {u64 offset, u64 size, u32 name, u32 flags (1 = dir)} sorted by name
bytes; then the names.  Version 1 (GHOSTIDX, until 2026-10-21) had a 64-byte
header and no title.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT_ROOT = ROOT / "build/ghostfs"
CAP = 992 << 20                      # bytes of data per container (1 GiB container, headroom kept)
DATA_BLOCKS = 32768                  # 32 KiB blocks: the 1 GiB maximum
HEADER_SIZE = 128
FLAG_DIR = 1
FLAG_INCOMPLETE = 1                  # header flags: written before the data, cleared by the final index


def ghost_group(rel: str) -> tuple:
    def region(name: str) -> int:
        tokens = re.split(r"[_.#&-]", name.lower())
        for tok, rank in [("izu", 2), ("toy", 3), ("kam", 4), ("iki", 5)]:
            if tok in tokens or name.lower().startswith(tok):
                return rank
        return 1
    parts = rel.split("/")
    top = parts[0]
    if top in ("sce_sys", "sce_module", "ui") or len(parts) == 1:
        return (0, rel)                       # executable, modules, small root files
    if top == "cache" and len(parts) == 2:
        return (1, region(parts[1]), rel)     # area packs, by region
    if top == "cache" and parts[1] == "psarc":
        return (2, rel)                       # audio / music archives
    if top == "cache" and parts[1] == "meshes":
        return (3, rel)
    if top == "cache" and parts[1] == "bitmaps":
        return (4, rel)
    if top == "cache":
        return (5, rel)
    if top == "movies":
        return (6, rel)
    return (7, rel)


def generic_group(rel: str) -> tuple:
    parts = rel.split("/")
    if parts[0] in ("sce_sys", "sce_module") or len(parts) == 1:
        return (0, rel)                       # executable, modules, root files
    return (1, rel)                           # the rest, directory by directory


PROFILES = {
    "CUSA11456": {
        "prefix": "GHOST",
        "exclude": r"^(eboot\.bin\.xref|cache/psarc/lang_(french|latino|brazilian|japanese)_audio\.psarc)$",
        "order": ROOT / "ghostldr/data/cusa11456-open-order.txt",
        "late": "cache/psarc/",               # traced files under here go after the other traced ones
        "group": ghost_group,
        "src": ROOT / "build/ghost/app0",
    },
}


def param_title(src: Path) -> str | None:
    """TITLE_ID from sce_sys/param.sfo, if the dump has one."""
    p = src / "sce_sys/param.sfo"
    if not p.exists():
        return None
    d = p.read_bytes()
    kt, dt, n = struct.unpack_from("<III", d, 8)
    for i in range(n):
        ko, _, ln, _, do = struct.unpack_from("<HHIII", d, 20 + 16 * i)
        if d[kt + ko:d.index(b"\0", kt + ko)] == b"TITLE_ID":
            return d[dt + do:dt + do + ln].rstrip(b"\0").decode()
    return None


def pack(src: Path, title: str, prefix: str, out: Path) -> dict:
    prof = PROFILES.get(title, {})
    exclude = re.compile(prof.get("exclude", r"^$(?!)"))
    group = prof.get("group", generic_group)
    files: dict[str, int] = {}
    dirs: set[str] = {""}
    for p in sorted(src.rglob("*")):
        rel = p.relative_to(src).as_posix()
        if exclude.match(rel):
            continue
        if p.is_dir():
            dirs.add(rel)
        elif p.is_file():
            files[rel] = p.stat().st_size
            d = rel
            while "/" in d:
                d = d.rsplit("/", 1)[0]
                dirs.add(d)

    traced: list[str] = []
    if prof.get("order"):
        lines = prof["order"].read_text().splitlines()
        traced = [p for p in lines if p and not p.startswith("#") and p in files]
    late = prof.get("late")
    boot = sorted((p for p in files if group(p)[0] == 0 and p not in traced))
    traced_first = [p for p in traced if not (late and p.startswith(late))]
    traced_late = [p for p in traced if late and p.startswith(late)]
    placed = set(boot) | set(traced)
    rest = sorted((p for p in files if p not in placed), key=group)
    order = boot + traced_first + traced_late + rest

    # linear layout, then cut into containers
    offset = 0
    layout = []
    for rel in order:
        layout.append((rel, offset, files[rel]))
        offset += files[rel]
    total = offset
    count = (total + CAP - 1) // CAP
    containers = [{"dir": f"{prefix}{i:03d}", "blocks": DATA_BLOCKS, "size": min(CAP, total - i * CAP), "pieces": []}
                  for i in range(count)]
    for rel, off, size in layout:
        pos, end = off, off + size
        while pos < end:
            k = pos // CAP
            take = min(end, (k + 1) * CAP) - pos
            containers[k]["pieces"].append([rel, pos - off, take])
            pos += take

    # the layout id names exactly this byte layout: installs compare it
    h = hashlib.sha256(json.dumps([title, prefix, CAP, [c["pieces"] for c in containers]]).encode())
    layout_id = int.from_bytes(h.digest()[:8], "little")

    # index: header, entries sorted by name bytes, then names
    entries = [(rel.encode(), off, size, 0) for rel, off, size in layout]
    entries += [(d.encode(), 0, 0, FLAG_DIR) for d in dirs]
    entries.sort(key=lambda e: e[0])
    names = bytearray()
    packed = []
    for name, off, size, flags in entries:
        packed.append(struct.pack("<QQII", off, size, len(names), flags))
        names += name + b"\0"
    names_off = HEADER_SIZE + 24 * len(packed)
    header = index_header(count, len(packed), total, names_off, len(names), prefix, title, layout_id, 0)
    index = header + b"".join(packed) + bytes(names)

    out.mkdir(parents=True, exist_ok=True)
    (out / "index.bin").write_bytes(index)
    # small containers lose a lot to filesystem overhead: give the index 4x its size
    idx_blocks = max(512, (4 * len(index) + 32767) // 32768)
    info = {
        "title": title, "prefix": prefix, "layout_id": f"{layout_id:016x}", "src": str(src), "cap": CAP,
        "total": total, "entries": len(packed), "index_blocks": idx_blocks, "containers": containers,
        "traced": len(traced), "files": len(files), "dirs": len(dirs),
    }
    (out / "layout.json").write_text(json.dumps(info, indent=1))
    return info


def index_header(count: int, entries: int, total: int, names_off: int, names_size: int, prefix: str, title: str,
                 layout_id: int, flags: int) -> bytes:
    header = struct.pack("<4sIIIQQII16sII16sQ", b"GFS1", 2, count, entries, CAP, total, names_off, names_size,
                         prefix.encode(), HEADER_SIZE, flags, title.encode(), layout_id)
    return header + b"\0" * (HEADER_SIZE - len(header))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", type=Path, help="the title's extracted app0 (default: CUSA11456's)")
    ap.add_argument("--title", help="title ID (default: sce_sys/param.sfo's, else CUSA11456)")
    ap.add_argument("--prefix", help="container name prefix, unique per title (default: profile's, else G + digits)")
    args = ap.parse_args()
    src = args.src or PROFILES["CUSA11456"]["src"]
    title = args.title or param_title(src) or ("CUSA11456" if not args.src else None)
    if not title or not re.fullmatch(r"[A-Z]{4}\d{5}", title):
        raise SystemExit("pass --title (e.g. CUSA12345): the dump has no sce_sys/param.sfo")
    prefix = args.prefix or PROFILES.get(title, {}).get("prefix") or "G" + title[4:]
    if not re.fullmatch(r"[A-Z0-9]{1,12}", prefix):
        raise SystemExit("the prefix must be 1-12 of A-Z 0-9")
    info = pack(src, title, prefix, OUT_ROOT / title)
    cs = info["containers"]
    print(f"{title}: {info['files']} files ({info['traced']} traced first), {info['dirs']} dirs, "
          f"{info['total'] / 2**30:.1f} GiB in {len(cs)} containers {prefix}000..; layout {info['layout_id']}")
    for c in cs[:6]:
        first = c["pieces"][0][0] if c["pieces"] else "-"
        print(f"  {c['dir']}: {c['size'] >> 20} MiB, {len(c['pieces'])} pieces, starts with {first}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
