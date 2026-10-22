#!/usr/bin/env python3
"""NID -> name tables.

The runtime extension (ext.bin, see tools/mkext.py) carries every known name:
build/nid/known_names.txt plus ghostldr/data/extra_names.txt if present.  The
core resolves host imports with it, so it no longer depends on one title.

    python3 ghostldr/tools/gen_nids.py               # size of the full table
    python3 ghostldr/tools/gen_nids.py --title APP0  # names that title's imports use

Table layout in ext.bin (ghost_api.h, struct ext_blob): u64 keys ascending,
u32 name offsets, then the NUL-terminated names.  A key is the 64-bit value a
NID's 11 base64 digits encode (sha1(name + suffix)[:8] read little-endian).
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import orbis_elf as oe  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
NAME_FILES = (ROOT / "build/nid/known_names.txt", ROOT / "ghostldr/data/extra_names.txt")
# names the loader itself looks up by NID
LOADER_NAMES = ("module_start", "module_stop")
# sce_module files the boot always loads, first (modules.c k_first)
BOOT_FIRST = ("libSceFios2.prx", "libc.prx")


def name_db(files=NAME_FILES) -> dict[str, str]:
    """NID -> name for every known name (first name wins on a duplicate NID)."""
    if not Path(files[0]).is_file():
        raise SystemExit(f"missing {files[0]}: run tools/fetch_deps.sh")
    db = oe.load_name_db(*files)
    for name in LOADER_NAMES:
        db.setdefault(oe.compute_nid(name), name)
    return db


def nid_key(nid: str) -> int | None:
    """The 64-bit value of a canonical 11-digit NID, else None."""
    if len(nid) != 11:
        return None
    v = 0
    for i, ch in enumerate(nid):
        d = oe.ID_ALPHABET.find(ch)
        if d < 0:
            return None
        if i < 10:
            v = v << 6 | d
        elif d & 3:
            return None
        else:
            v = v << 4 | d >> 2
    return v


def encode_table(db: dict[str, str]) -> tuple[int, bytes, bytes, bytes]:
    """(count, keys, name offsets, names) for ext.bin."""
    entries = sorted((nid_key(nid), name) for nid, name in db.items())
    for (k1, n1), (k2, n2) in zip(entries, entries[1:]):
        if k1 == k2:
            raise SystemExit(f"NID collision: {n1} / {n2}")
    keys, offs, pool = bytearray(), bytearray(), bytearray()
    for key, name in entries:
        keys += struct.pack("<Q", key)
        offs += struct.pack("<I", len(pool))
        pool += name.encode() + b"\0"
    return len(entries), bytes(keys), bytes(offs), bytes(pool)


def find_elsewhere(app0: Path, name: str) -> Path | None:
    """A module file the title keeps outside sce_module (Unity: Media/Modules),
    as modules.c finds it: anywhere but sce_module/ and sce_sys/, first in path
    order."""
    for f in sorted(app0.rglob(name)):
        rel = f.relative_to(app0).parts
        if f.is_file() and len(rel) > 1 and rel[0] not in ("sce_module", "sce_sys"):
            return f
    return None


def module_candidates(app0: Path) -> dict[Path, "oe.OrbisImage"]:
    """The title's module files and their images, sorted by file name as the
    core sorts them: sce_module/*.prx, plus every file a loaded module needs
    (DT_NEEDED, not libSce*/libkernel*) that the title keeps elsewhere."""
    mod_dir = app0 / "sce_module"
    imgs: dict[Path, oe.OrbisImage] = {}
    for f in sorted(mod_dir.glob("*.prx")) if mod_dir.is_dir() else []:
        try:
            imgs[f] = oe.parse(f)
        except Exception:   # the core skips files it cannot parse, too
            continue
    main = oe.parse(app0 / "eboot.bin")
    more = True
    while more:
        more = False
        have = {f.name for f in imgs}
        for img in [main] + list(imgs.values()):
            for name in img.needed:
                if name in have or name.startswith(("libSce", "libkernel")) or not name.endswith(".prx"):
                    continue
                f = find_elsewhere(app0, name)
                if f:
                    try:
                        imgs[f] = oe.parse(f)
                        have.add(name)
                        more = True
                    except Exception:
                        pass
    return dict(sorted(imgs.items(), key=lambda kv: kv[0].name))


def boot_modules(app0: Path) -> list[Path]:
    """The title module files ghost_boot loads, in load order (modules.c):
    libSceFios2.prx and libc.prx, then every file exporting a library the
    executable or a chosen module imports, by name."""
    imgs = module_candidates(app0)
    main = oe.parse(app0 / "eboot.bin")

    def imports_from(a, b) -> bool:
        return bool({lib.name for lib in a.import_libs.values()} & {lib.name for lib in b.export_libs.values()})

    sel = {f for f in imgs if f.name in BOOT_FIRST}
    more = True
    while more:
        more = False
        for f, img in imgs.items():
            if f not in sel and (imports_from(main, img) or any(imports_from(imgs[g], img) for g in sel)):
                sel.add(f)
                more = True
    first = [f for n in BOOT_FIRST for f in imgs if f.name == n and f in sel]
    return first + [f for f in imgs if f in sel and f.name not in BOOT_FIRST]


def title_files(app0: Path) -> list[Path]:
    """The modules ghost_boot loads from a title's app0, in load order."""
    return boot_modules(app0) + [app0 / "eboot.bin"]


def title_table(app0: Path, db: dict[str, str] | None = None) -> dict[str, str]:
    """The per-title table the core compiled in before the extension: the
    names of every import of eboot.bin and sce_module/*.prx, plus the loader's."""
    db = db or name_db()
    needed = {}
    for f in [app0 / "eboot.bin"] + sorted((app0 / "sce_module").glob("*.prx")):
        for sym, _, _ in oe.parse(f).imports():
            if sym.nid in db:
                needed[sym.nid] = db[sym.nid]
    for name in LOADER_NAMES:
        needed[oe.compute_nid(name)] = name
    return needed


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--title", type=Path, help="a title's app0: list the names its imports use")
    args = ap.parse_args()
    db = name_db()
    if args.title:
        for nid, name in sorted(title_table(args.title, db).items()):
            print(nid, name)
        return 0
    n, keys, offs, pool = encode_table(db)
    print(f"{n} names: {len(keys) + len(offs) + len(pool)} bytes in ext.bin")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
