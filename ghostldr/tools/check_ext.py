#!/usr/bin/env python3
"""Offline checks of the runtime extension (build/ghostldr/ext.bin).

    python3 ghostldr/tools/check_ext.py [--app0 build/ghost/app0] [--log build/ghostldr/run.log]

- the blob: header, bounds, relocations, ext.elf agreement;
- the NID table: every known name present, keys sorted and equal to the
  names' NIDs;
- the HLE tables, decoded statically: families, entries (by name or NID),
  library policies (aliases, no host fallback, stub values), function
  addresses, and ext_init's merge (duplicates, alias clashes);
- with a title's app0: every import of the modules ghost_boot loads gets the
  same name as from the per-title table the core used to compile in, and
  resolves at the same stage (core HLE by name -> extension HLE by library and
  NID -> title modules -> denied stub if the library has no host fallback ->
  host by name -> stub).  The extension may only take, or deny, an import that
  had no resolution before: pass a run log to count "unresolved" host lookups.
  --old-core-elf compares with a core built before the extension (its
  compiled NID table and override tables) instead of regenerating the table.

Exit status 1 on any failure.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
import zlib
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import elf64  # noqa: E402
import gen_nids  # noqa: E402
import mkext  # noqa: E402
import orbis_elf as oe  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/ghostldr"
EXT_LIB_NO_HOST = 1


class Report:
    def __init__(self) -> None:
        self.failures = 0

    def fail(self, msg: str) -> None:
        self.failures += 1
        print(f"FAIL {msg}")

    def warn(self, msg: str) -> None:
        print(f"warn {msg}")

    def ok(self, msg: str) -> None:
        print(f"ok   {msg}")


VERBOSE = False   # --verbose: list every import an extension family took over


def cstr(buf: bytes, off: int) -> str:
    end = buf.index(b"\0", off)
    return buf[off:end].decode()


class Blob:
    """ext.bin, decoded the way ext.c reads it."""

    FIELDS = ("magic format header_size api_version abi blob_size image_off image_filesz image_memsz text_size "
              "entry reloc_off reloc_count families_off families_count nid_count nid_keys_off nid_names_off "
              "str_off str_size reserved0 reserved1").split()

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.h = dict(zip(self.FIELDS, mkext.HEADER.unpack_from(data, 0)))

    def __getattr__(self, name: str) -> int:
        try:
            return self.h[name]
        except KeyError:
            raise AttributeError(name)

    def relocs(self) -> list[tuple[int, int]]:
        return [struct.unpack_from("<II", self.data, self.reloc_off + 8 * i) for i in range(self.reloc_count)]

    def image(self, base: int = 0) -> bytearray:
        """The image relocated at base, bss included."""
        img = bytearray(self.image_memsz)
        img[:self.image_filesz] = self.data[self.image_off:self.image_off + self.image_filesz]
        for off, addend in self.relocs():
            struct.pack_into("<Q", img, off, base + addend)
        return img

    def nid_table(self) -> list[tuple[int, str]]:
        d, n = self.data, self.nid_count
        keys = struct.unpack_from(f"<{n}Q", d, self.nid_keys_off)
        offs = struct.unpack_from(f"<{n}I", d, self.nid_names_off)
        pool = d[self.str_off:self.str_off + self.str_size]
        return [(k, cstr(pool, o)) for k, o in zip(keys, offs)]

    def families(self) -> list[Family]:
        """Every family as registered (struct ext_family, ext_hle_ent, ext_lib)."""
        img = self.image()

        def s(ptr: int) -> str | None:
            return cstr(img, ptr) if ptr else None

        out = []
        for i in range(self.families_count):
            fam, = struct.unpack_from("<Q", img, self.families_off + 8 * i)
            name_p, hle_p, nhle, nlibs, libs_p, init = struct.unpack_from("<QQIIQQ", img, fam)
            ents = [Entry(s(lib), s(name), fn, s(nid))
                    for lib, name, fn, nid in (struct.unpack_from("<QQQQ", img, hle_p + 32 * j) for j in range(nhle))]
            libs = [Lib(s(name), (s(aliases) or "").split(), flags, ret)
                    for name, aliases, flags, ret in (struct.unpack_from("<QQIi", img, libs_p + 24 * j)
                                                      for j in range(nlibs))]
            out.append(Family(s(name_p), init, ents, libs))
        return out


@dataclass
class Entry:
    lib: str
    name: str | None
    fn: int
    nid: str | None

    def key(self) -> str | None:
        """The NID the entry matches, as ext_init computes it."""
        if self.nid is not None:
            return self.nid if gen_nids.nid_key(self.nid) is not None else None
        return oe.compute_nid(self.name) if self.name else None


@dataclass
class Lib:
    name: str
    aliases: list[str]
    flags: int
    stub_ret: int

    @property
    def no_host(self) -> bool:
        return bool(self.flags & EXT_LIB_NO_HOST)


@dataclass
class Family:
    name: str
    init: int
    hle: list[Entry]
    libs: list[Lib]


class ExtModel:
    """ext_init's merged tables and its lookups (ext/src/ext_main.c)."""

    def __init__(self, families: list[Family], r: Report) -> None:
        self.libs: list[Lib] = []
        for fam in families:
            for lib in fam.libs:
                have = self.policy(lib.name)
                clash = [a for a in lib.aliases if self.policy(a)]
                if have:
                    r.warn(f"library {lib.name} declared again by family {fam.name}: {have.name}'s policy wins")
                    continue
                if clash:
                    r.warn(f"family {fam.name}: aliases {clash} of {lib.name} already belong to another library")
                self.libs.append(lib)
        self.entries: list[tuple[str | None, str, Entry, str]] = []
        for fam in families:
            for e in fam.hle:
                nid = e.key()
                if nid is None:
                    r.fail(f"family {fam.name}: {e.lib}:{e.name} has no valid name or NID")
                    continue
                if e.nid and e.name and oe.compute_nid(e.name) != e.nid:
                    r.warn(f"family {fam.name}: {e.lib}:{e.name} gives NID {e.nid}, but the name's NID is "
                           f"{oe.compute_nid(e.name)}")
                if self.lookup(e.lib, nid):
                    r.warn(f"{e.lib}:{e.name or nid} registered twice: family {fam.name}'s copy is ignored")
                    continue
                self.entries.append((self.canon(e.lib), nid, e, fam.name))

    def policy(self, lib: str | None) -> Lib | None:
        return next((l for l in self.libs if lib and (lib == l.name or lib in l.aliases)), None)

    def canon(self, lib: str | None) -> str | None:
        p = self.policy(lib)
        return p.name if p else lib

    def lookup(self, lib: str | None, nid: str) -> Entry | None:
        c = self.canon(lib) if lib else None
        return next((e for canon, n, e, _ in self.entries if n == nid and (c is None or canon == c)), None)


def check_blob(b: Blob, r: Report) -> None:
    size = len(b.data)
    if b.magic != mkext.MAGIC or b.format != mkext.FORMAT or b.header_size != mkext.HEADER.size:
        r.fail(f"header: magic {b.magic:#x} format {b.format} header {b.header_size}")
        raise SystemExit(1)
    if b.api_version != mkext.API_VERSION:
        r.fail(f"API version {b.api_version}, tools expect {mkext.API_VERSION}")
    if b.blob_size != size:
        r.fail(f"blob_size {b.blob_size} but the file has {size} bytes")

    def inside(off: int, n: int) -> bool:
        return 0 <= off and off + n <= size

    layout_ok = (inside(b.image_off, b.image_filesz) and b.image_filesz <= b.image_memsz
                 and b.image_memsz % 0x4000 == 0 and b.text_size % 0x4000 == 0 and 0 < b.text_size < b.image_memsz
                 and b.entry < b.text_size and inside(b.reloc_off, 8 * b.reloc_count)
                 and b.nid_keys_off % 8 == 0 and inside(b.nid_keys_off, 8 * b.nid_count)
                 and b.nid_names_off % 4 == 0 and inside(b.nid_names_off, 4 * b.nid_count)
                 and inside(b.str_off, b.str_size) and b.str_size and b.data[b.str_off + b.str_size - 1] == 0
                 and b.families_off + 8 * b.families_count <= b.image_memsz)
    if not layout_ok:
        r.fail(f"layout: {b.h}")
        raise SystemExit(1)
    r.ok(f"blob: {size} bytes, CRC-32 {zlib.crc32(b.data):08x}, ABI {b.abi:08x}; image {b.image_memsz:#x} "
         f"(text {b.text_size:#x}, {b.image_filesz:#x} stored), entry {b.entry:#x}")

    relocs = b.relocs()
    bad = [(o, a) for o, a in relocs if o + 8 > b.image_memsz or a >= b.image_memsz]
    dups = [o for o, n in Counter(o for o, _ in relocs).items() if n > 1]
    in_text = [o for o, _ in relocs if o < b.text_size]
    if bad or dups:
        r.fail(f"relocations: {len(bad)} outside the image, {len(dups)} duplicate offsets")
    else:
        r.ok(f"relocations: {len(relocs)}, all inside the image"
             + (f" ({len(in_text)} in text, applied before the upload)" if in_text else ""))


def check_elf(b: Blob, elf_path: Path, r: Report) -> None:
    if not elf_path.exists():
        r.warn(f"{elf_path} missing: image not compared with the ELF")
        return
    code = mkext.code_image(elf64.Elf(elf_path))
    same = (code["image"] == b.data[b.image_off:b.image_off + b.image_filesz] and code["memsz"] == b.image_memsz
            and code["text"] == b.text_size and code["entry"] == b.entry and code["relocs"] == sorted(b.relocs())
            and code["families"] == (b.families_off, b.families_count))
    if same:
        r.ok(f"image, relocations and entry match {elf_path.name}")
    else:
        r.fail(f"ext.bin does not match {elf_path.name}: rebuild (make -C ghostldr/native)")


def check_nids(b: Blob, db: dict[str, str], r: Report) -> dict[str, str]:
    table = b.nid_table()
    keys = [k for k, _ in table]
    if any(k2 <= k1 for k1, k2 in zip(keys, keys[1:])):
        r.fail("NID keys are not strictly ascending")
    wrong = [name for key, name in table if gen_nids.nid_key(oe.compute_nid(name)) != key]
    if wrong:
        r.fail(f"{len(wrong)} keys do not match their names' NIDs, e.g. {wrong[:3]}")
    by_nid = {oe.compute_nid(name): name for _, name in table}
    missing = [n for nid, n in db.items() if by_nid.get(nid) != n]
    if missing or len(table) != len(db):
        r.fail(f"NID table has {len(table)} names, the name lists {len(db)}; {len(missing)} missing or different")
    else:
        r.ok(f"NID table: {len(table)} names, sorted, keys match, same as the name lists")
    return by_nid


def check_families(b: Blob, r: Report) -> ExtModel:
    fams = b.families()
    for fam in fams:
        if fam.init and fam.init >= b.text_size:
            r.fail(f"family {fam.name}: init {fam.init:#x} is not in the text")
        for e in fam.hle:
            if not 0 <= e.fn < b.text_size:
                r.fail(f"family {fam.name}: {e.lib}:{e.name or e.nid} -> {e.fn:#x} is not in the text")
    model = ExtModel(fams, r)
    r.ok(f"HLE: {len(fams)} families, {len(model.entries)} functions, {len(model.libs)} library policies: "
         + ", ".join(f"{f.name}({len(f.hle)}/{len(f.libs)})" for f in fams))
    for lib in model.libs:
        print(f"     policy {lib.name}" + (f" (aliases {' '.join(lib.aliases)})" if lib.aliases else "")
              + (", no host fallback" if lib.no_host else "") + f", stubs return {lib.stub_ret & 0xFFFFFFFF:#x}")
    return model


def core_hle(core_elf: Path) -> set[tuple[str, str]]:
    """(lib, name) of the core's override tables g_hle (hle.c) and g_sys (hle_sys.c)."""
    elf = elf64.Elf(core_elf)
    img, _ = elf.image()
    ptr = {off: addend for off, t, _, addend in elf.relas() if t == elf64.R_X86_64_RELATIVE}
    out = set()
    for table in ("g_hle", "g_sys"):
        sym = elf.symbols.get(table)
        if not sym or sym.size % 24:
            raise SystemExit(f"{core_elf}: no override table {table}")
        for i in range(sym.size // 24):
            at = sym.value + 24 * i
            out.add((cstr(img, ptr[at]), cstr(img, ptr[at + 8])))
    return out


def old_core_names(core_elf: Path) -> dict[str, str]:
    """NID -> name from the g_nid_table a pre-extension core compiled in."""
    elf = elf64.Elf(core_elf)
    img, _ = elf.image()
    ptr = {off: addend for off, t, _, addend in elf.relas() if t == elf64.R_X86_64_RELATIVE}
    table, count = elf.symbols.get("g_nid_table"), elf.symbols.get("g_nid_count")
    if not table or not count:
        raise SystemExit(f"{core_elf}: no g_nid_table (not a pre-extension core)")
    n, = struct.unpack_from("<I", img, count.value)
    return {cstr(img, table.value + 24 * i): cstr(img, ptr[table.value + 24 * i + 16]) for i in range(n)}


def unresolved_in_log(log: Path) -> set[tuple[str, str]]:
    """(lib, nid) of imports a run reported as unresolved."""
    pat = re.compile(r"unresolved(?: \(weak\))?: \S+ needs (\S+):(\S+) ")
    return {m.groups() for m in map(pat.search, log.read_text(errors="replace").splitlines()) if m}


def check_title(app0: Path, by_nid: dict[str, str], db: dict[str, str], ext: ExtModel, core_elf: Path,
                old_core_elf: Path | None, log: Path | None, r: Report, base_core_elf: Path | None = None) -> None:
    core = core_hle(core_elf)
    # "before" binds with the base core's override tables (default: this core's)
    base = core_hle(base_core_elf) if base_core_elf else core
    if base_core_elf:
        r.ok(f"before = {base_core_elf} without the extension: {len(base)} core overrides, now {len(core)}")
    if old_core_elf:
        old_names = old_core_names(old_core_elf)
        old_core = core_hle(old_core_elf)
        if old_core != core:
            r.fail(f"the core's override tables differ from {old_core_elf}: "
                   f"{sorted(old_core ^ core)[:6]}")
        else:
            r.ok(f"core override tables: {len(core)} entries, same as {old_core_elf.name}")
        r.ok(f"old compiled NID table: {len(old_names)} names from {old_core_elf}")
    else:
        old_names = gen_nids.title_table(app0, db)
    stubbed = unresolved_in_log(log) if log else set()
    mods = [oe.parse(f) for f in gen_nids.title_files(app0)]
    exports = []
    for img in mods:
        libs = {lib.name for lib in img.export_libs.values()}
        exports.append((libs, {s.nid for s in img.exports()}))

    def core_hit(lib, name, table) -> bool:
        return any(n == name and (lib is None or l == lib) for l, n in table)

    def stage(lib, nid, name, with_ext: bool) -> str:
        """resolve_import's choice, before (no extension) and after."""
        if with_ext and ext.lookup(lib, nid):
            return "extension HLE"
        if name and core_hit(lib, name, core if with_ext else base):
            return "core HLE"
        if lib and any(lib in libs and nid in nids for libs, nids in exports):
            return "title module"
        pol = ext.policy(lib) if with_ext else None
        ret = f" returning {pol.stub_ret & 0xFFFFFFFF:#x}" if pol and pol.stub_ret else ""
        if pol and pol.no_host:
            return f"stub (denied){ret}"
        if name and (lib or "?", nid) not in stubbed:
            return "host by name"
        return ("stub (host lookup failed)" if name else "stub (no name)") + ret

    moved: Counter[str] = Counter()
    stages: Counter[str] = Counter()
    changed = 0
    total = 0
    used: set[int] = set()
    for img in mods:
        for sym, lib, mod in img.imports():
            total += 1
            nid, libname = sym.nid, lib.name if lib else None
            old, new = old_names.get(nid), by_nid.get(nid)
            if old != new:
                r.fail(f"{Path(img.path).name}: {libname}:{nid} was named {old}, now {new}")
                continue
            if (e := ext.lookup(libname, nid)):
                used.add(id(e))
            before, after = stage(libname, nid, old, False), stage(libname, nid, new, True)
            stages[after] += 1
            if before != after:
                where = f"{Path(img.path).name}: {libname}:{new or nid}"
                if before.startswith("stub"):
                    r.ok(f"{where} was a stub: {before} -> {after}")
                elif after in ("extension HLE",) or after.startswith("stub (denied)"):
                    moved[after] += 1      # deliberate: an extension family took it over
                    if VERBOSE:
                        r.ok(f"{where}: {before} -> {after}")
                else:
                    changed += 1
                    hint = "; if a run logged it as unresolved, pass --log" if before == "host by name" else ""
                    r.fail(f"{where} would move from {before} to {after}{hint}")
    for _, _, e, fam in ext.entries:
        if e.name and any(n == e.name and ext.canon(l) == ext.canon(e.lib) for l, n in core):
            r.warn(f"core override {e.lib}:{e.name} is superseded by the extension ({fam}): "
                   f"delete the core's row when convenient")
    for where, n in sorted(moved.items()):
        r.ok(f"{n} imports now bind to {where} instead of the core/title/host (extension families)")
    summary = ", ".join(f"{k} {v}" for k, v in stages.most_common())
    if not changed:
        r.ok(f"{app0}: {total} imports of {len(mods)} modules resolve as before ({summary})")
    print(f"     extension entries no loaded module imports: {len(ext.entries) - len(used)} of {len(ext.entries)}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ext", type=Path, default=BUILD / "ext.bin")
    ap.add_argument("--elf", type=Path, default=BUILD / "ext.elf")
    ap.add_argument("--core-elf", type=Path, default=BUILD / "core.elf")
    ap.add_argument("--app0", type=Path, default=ROOT / "build/ghost/app0",
                    help="a title's app0 to compare resolution against (skipped if missing)")
    ap.add_argument("--log", type=Path, help="run log: imports it reports as unresolved count as stubs")
    ap.add_argument("--old-core-elf", type=Path, help="a core.elf built before the extension, to compare with")
    ap.add_argument("--base-core-elf", type=Path,
                    help="compare with this core's override tables (e.g. the integration branch's core.elf) instead "
                         "of the current core's, so imports moved out of the core are listed")
    ap.add_argument("--verbose", action="store_true", help="list every import an extension family took over")
    args = ap.parse_args()
    global VERBOSE
    VERBOSE = args.verbose
    r = Report()
    b = Blob(args.ext.read_bytes())
    check_blob(b, r)
    check_elf(b, args.elf, r)
    db = gen_nids.name_db()
    by_nid = check_nids(b, db, r)
    model = check_families(b, r)
    if (args.app0 / "eboot.bin").exists():
        check_title(args.app0, by_nid, db, model, args.core_elf, args.old_core_elf, args.log, r, args.base_core_elf)
    else:
        r.warn(f"{args.app0}/eboot.bin missing: title resolution not compared")
    print("FAILED" if r.failures else "all checks passed")
    return 1 if r.failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
