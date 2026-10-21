#!/usr/bin/env python3
"""Summarize an Orbis image: layout, TLS, init/fini, relocations, imports."""

from __future__ import annotations

import argparse
import json
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import orbis_elf as oe  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DBS = [ROOT / "build/nid/known_names.txt", ROOT / "ghostldr/data/extra_names.txt"]


def summarize(path: Path, db: dict[str, str], verbose: bool) -> dict:
    img = oe.parse(path)
    out: dict = {
        "path": str(path),
        "e_type": hex(img.e_type),
        "entry": hex(img.entry),
        "image_size": hex(img.image_size) if img.phdrs else None,
        "soname": img.soname,
        "module": img.module_info.__dict__ if img.module_info else None,
        "segments": [
            {"type": p.name, "perm": p.perms, "vaddr": hex(p.vaddr), "memsz": hex(p.memsz),
             "filesz": hex(p.filesz), "align": hex(p.align)}
            for p in img.phdrs
        ],
        "needed": img.needed,
        "needed_modules": sorted(m.name for m in img.needed_modules.values()),
    }
    tls = img.phdr(oe.PT_TLS)
    if tls:
        out["tls"] = {"vaddr": hex(tls.vaddr), "filesz": hex(tls.filesz), "memsz": hex(tls.memsz), "align": hex(tls.align)}
    for tag, name in ((oe.DT_INIT, "init"), (oe.DT_FINI, "fini"), (oe.DT_INIT_ARRAYSZ, "init_array_size"),
                      (oe.DT_FINI_ARRAYSZ, "fini_array_size"), (oe.DT_PREINIT_ARRAYSZ, "preinit_array_size")):
        value = img.dyn_value(tag)
        if value is not None:
            out[name] = hex(value)
    pp = img.phdr(oe.PT_SCE_PROCPARAM)
    if pp:
        raw = img.elf[pp.offset: pp.offset + pp.filesz]
        size, magic, version, sdk = struct.unpack_from("<I4sII", raw, 0)
        fields = struct.unpack_from(f"<{(len(raw) - 0x10) // 8}Q", raw, 0x10)
        out["proc_param"] = {"size": hex(size), "magic": magic.decode(errors="replace"),
                             "version": hex(version), "sdk": hex(sdk),
                             "pointers": [hex(v) for v in fields]}
    relocs = Counter(oe.RELOC_NAMES.get(r.type, hex(r.type)) for r in img.relas + img.jmprels)
    out["relocations"] = dict(relocs)

    imports = img.imports()
    per_lib: dict[str, list[str]] = defaultdict(list)
    unknown = 0
    for sym, lib, mod in imports:
        name = db.get(sym.nid)
        if name is None:
            unknown += 1
        key = f"{lib.name if lib else '?'}@{mod.name if mod else '?'}"
        per_lib[key].append(name or f"<{sym.nid}>")
    out["import_count"] = len(imports)
    out["import_unknown"] = unknown
    out["import_libraries"] = {k: len(v) for k, v in sorted(per_lib.items())}
    if verbose:
        out["imports"] = {k: sorted(v) for k, v in sorted(per_lib.items())}
    exports = img.exports()
    out["export_count"] = len(exports)
    out["export_libraries"] = sorted(lib.name for lib in img.export_libs.values())
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("--db", action="append", type=Path, default=[])
    args = parser.parse_args()
    db = oe.load_name_db(*(DEFAULT_DBS + args.db))
    for path in args.paths:
        print(json.dumps(summarize(path, db, args.verbose), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
