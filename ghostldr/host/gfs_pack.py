#!/usr/bin/env python3
"""Lay a title's app0 out over SWRR save containers ("ghostfs").

SWRR (no kernel exploit) can own many save containers of at most 1 GiB each
(32768 blocks) and mount 15 at once.  The title's files are concatenated in
one linear byte space that is cut into CAP-sized pieces, one per container
(<PREFIX>000, <PREFIX>001, ... each holding data.bin); a sorted index
(GFSINDEX, index.bin) maps every path to its offset and size.  The title's own
saves later go into <PREFIX>SAVE, so every title needs its own prefix.

Order matters because only ~12 containers stay mounted.  The title's profile
(ghostldr/data/titles/<TITLE>.json, "pack") can name an open-order file
traced from earlier boots (capture_order.py writes one), files to place late
or leave out, and its own grouping rules.  Without them the generic layout
is: eboot.bin, sce_module/, sce_sys/, then small files, then everything else
directory by directory (heuristic_key).  A profile with an order file puts
the boot group first, then the traced files, then the rest by its groups.

    python3 ghostldr/host/gfs_pack.py                     # the dump in build/ghost/app0
    python3 ghostldr/host/gfs_pack.py --src DIR [--title CUSA12345] [--prefix NAME] [--order FILE]

The title comes from --title, else sce_sys/param.sfo, else the profile whose
eboot fingerprint matches.  Writes build/ghostfs/<TITLE>/layout.json and
index.bin.

Index (little endian): 128-byte header "GFS1", version 2, containers, entries,
cap u64, total u64, names_off, names_size, prefix[16] @40, header_size @56,
flags @60 (1 = install incomplete), title[16] @64, layout id u64 @80; then
entries {u64 offset, u64 size, u32 name, u32 flags (1 = dir)} sorted by name
bytes; then the names.  Version 1 (GHOSTIDX, until 2026-10-21) had a 64-byte
header and no title.  The layout id hashes the title, prefix, CAP and every
container's pieces, so an unchanged order gives an unchanged id.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

import gfs_catalog as gc
import profiles as titles

ROOT = Path(__file__).resolve().parents[2]
OUT_ROOT = ROOT / "build/ghostfs"
CAP = 992 << 20                      # bytes of data per container (1 GiB container, headroom kept)
DATA_BLOCKS = 32768                  # 32 KiB blocks: the 1 GiB maximum
HEADER_SIZE = 128
FLAG_DIR = 1
FLAG_INCOMPLETE = 1                  # header flags: written before the data, cleared by the final index
SMALL = 64 << 10                     # heuristic: "small" files go right after the boot files...
SMALL_BUDGET = 256 << 20             # ...as long as they stay under this together


def small_limit(files: dict[str, int]) -> int:
    """Size up to which a file counts as small: SMALL, halved until the small
    files fit in SMALL_BUDGET (at least 4 KiB)."""
    limit = SMALL
    while limit > 4096 and sum(s for s in files.values() if s <= limit) > SMALL_BUDGET:
        limit //= 2
    return limit


def heuristic_key(files: dict[str, int]):
    """Generic order for a title without grouping rules: eboot.bin,
    sce_module/, sce_sys/, small files, then the rest directory by directory.
    Rank 0 is the boot group."""
    limit = small_limit(files)

    def key(rel: str) -> tuple:
        top, _, rest = rel.partition("/")
        d, _, name = rel.rpartition("/")
        if rel == "eboot.bin":
            return (0, 0, "", rel)
        if top == "sce_module" and rest:
            return (0, 1, d, name)
        if top == "sce_sys" and rest:
            return (0, 2, d, name)
        if files[rel] <= limit:
            return (0, 3, d, name)
        return (1, 0, d, name)
    return key


def rules_key(groups: list):
    """Order from a profile's [regex, rank] rules: the first rule whose regex
    is found in the path gives the rank (unmatched files go last); rank 0 is
    the boot group.  Within a rank, by path."""
    rules = [(re.compile(rx), int(rank)) for rx, rank in groups]
    last = max((r for _, r in rules), default=0) + 1

    def key(rel: str) -> tuple:
        return (next((r for rx, r in rules if rx.search(rel)), last), rel)
    return key


def read_order(path: Path | None, files: dict[str, int]) -> list[str]:
    if not path:
        return []
    seen: set[str] = set()
    out = []
    for line in path.read_text().splitlines():
        rel = line.strip()
        if rel and not rel.startswith("#") and rel in files and rel not in seen:
            seen.add(rel)
            out.append(rel)
    return out


def file_order(files: dict[str, int], pack_prof: dict, order_file: Path | None = None) -> tuple[list[str], int]:
    """Every file in layout order, and how many came from the order file."""
    groups = pack_prof.get("groups")
    key = rules_key(groups) if groups else heuristic_key(files)
    if order_file is None and pack_prof.get("order"):
        order_file = titles.TITLES / pack_prof["order"]
    traced = read_order(order_file, files)
    late = tuple(pack_prof.get("late", []))
    traced_set = set(traced)
    boot = sorted((p for p in files if key(p)[0] == 0 and p not in traced_set), key=key)
    traced_first = [p for p in traced if not (late and p.startswith(late))]
    traced_late = [p for p in traced if late and p.startswith(late)]
    placed = set(boot) | traced_set
    rest = sorted((p for p in files if p not in placed), key=key)
    return boot + traced_first + traced_late + rest, len(traced)


def pack(src: Path, title: str, prefix: str, out: Path, prof: dict | None = None,
         order_file: Path | None = None) -> dict:
    if prof is None:
        prof = titles.load(title)
    pk = prof.get("pack", {})
    exclude = re.compile(pk.get("exclude") or r"^$(?!)")
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
    if "eboot.bin" not in files:
        raise SystemExit(f"{src}: no eboot.bin (point --src at the title's app0)")
    order, traced = file_order(files, pk, order_file)

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
    sfo = titles.param_sfo(src)
    info = {
        "title": title, "prefix": prefix, "layout_id": f"{layout_id:016x}", "src": str(src), "cap": CAP,
        "total": total, "entries": len(packed), "index_blocks": idx_blocks, "containers": containers,
        "traced": traced, "files": len(files), "dirs": len(dirs),
        # for the catalogue entry and run.py (not part of the layout)
        "name": prof.get("name") or str(sfo.get("TITLE", "")), "version": prof.get("version") or
        str(sfo.get("APP_VER", "")), "app_params": titles.app_params(prof, src),
    }
    (out / "layout.json").write_text(json.dumps(info, indent=1))
    return info


def packed_titles() -> list[str]:
    """Titles packed in build/ghostfs, most recently packed first."""
    found = sorted(OUT_ROOT.glob("*/layout.json"), key=lambda f: f.stat().st_mtime, reverse=True)
    return [f.parent.name for f in found if (f.parent / "index.bin").is_file()]


def default_title(what: str) -> str:
    """The title a tool works on when not told: the only one packed, else the
    most recently packed (said out loud)."""
    packed = packed_titles()
    if not packed:
        raise SystemExit(f"nothing packed in {OUT_ROOT.relative_to(ROOT)}: run gfs_pack.py first, or pass --title")
    if len(packed) > 1:
        print(f"{what} {packed[0]}, the most recently packed of {', '.join(packed)} (--title picks another)")
    return packed[0]


def index_header(count: int, entries: int, total: int, names_off: int, names_size: int, prefix: str, title: str,
                 layout_id: int, flags: int) -> bytes:
    header = struct.pack("<4sIIIQQII16sII16sQ", b"GFS1", 2, count, entries, CAP, total, names_off, names_size,
                         prefix.encode(), HEADER_SIZE, flags, title.encode(), layout_id)
    return header + b"\0" * (HEADER_SIZE - len(header))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", type=Path, default=titles.DEFAULT_SRC,
                    help="the title's extracted app0 (default: build/ghost/app0)")
    ap.add_argument("--title", help="title ID (default: param.sfo's, else the profile matching the eboot)")
    ap.add_argument("--prefix", help="container name prefix, unique per title (default: the profile's, else G + "
                    "the ID's digits)")
    ap.add_argument("--order", type=Path, help="open-order file to use instead of the profile's (capture_order.py)")
    args = ap.parse_args()
    src = args.src
    if not (src / "eboot.bin").is_file():
        raise SystemExit(f"{src}: no eboot.bin (point --src at the title's app0)")
    title = titles.normalize_title(args.title) if args.title else titles.detect_title(src)
    if not title:
        raise SystemExit("pass --title (e.g. CUSA12345): the dump's sce_sys/param.sfo has no usable TITLE_ID "
                         "and no profile matches its eboot")
    try:
        prof = titles.load(title)
    except titles.ProfileError as e:
        raise SystemExit(str(e))
    prefix = (args.prefix or prof.get("pack", {}).get("prefix") or titles.default_prefix(title)).upper()
    why = gc.prefix_problem(prefix)
    if why:
        raise SystemExit(f"prefix {prefix}: {why} (--prefix picks another)")
    info = pack(src, title, prefix, OUT_ROOT / title, prof, args.order)
    cs = info["containers"]
    how = "own profile" if prof["_has_profile"] else "no profile: generic layout"
    print(f"{title} ({how}): {info['files']} files ({info['traced']} from the open order first), {info['dirs']} "
          f"dirs, {info['total'] / 2**30:.1f} GiB in {len(cs)} containers {prefix}000..; layout {info['layout_id']}")
    for c in cs[:6]:
        first = c["pieces"][0][0] if c["pieces"] else "-"
        print(f"  {c['dir']}: {c['size'] >> 20} MiB, {len(c['pieces'])} pieces, starts with {first}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
