#!/usr/bin/env python3
"""Parse PS4 (Orbis) fake-signed SELF / ELF images.

This is the host-side twin of the native loader's parser: it extracts the
plain ELF from an FSELF container, then decodes program headers, the Sony
dynamic table (which lives in PT_SCE_DYNLIBDATA, not in a loaded segment),
modules, libraries, symbols, and relocations.
"""

from __future__ import annotations

import base64
import hashlib
import struct
from dataclasses import dataclass, field
from pathlib import Path

SELF_MAGIC = b"\x4f\x15\x3d\x1d"
ELF_MAGIC = b"\x7fELF"

ET_SCE_EXEC = 0xFE00
ET_SCE_DYNEXEC = 0xFE10
ET_SCE_DYNAMIC = 0xFE18

PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3
PT_TLS = 7
PT_GNU_EH_FRAME = 0x6474E550
PT_SCE_RELA = 0x60000000
PT_SCE_DYNLIBDATA = 0x61000000
PT_SCE_PROCPARAM = 0x61000001
PT_SCE_MODULE_PARAM = 0x61000002
PT_SCE_RELRO = 0x61000010
PT_SCE_COMMENT = 0x6FFFFF00
PT_SCE_LIBVERSION = 0x6FFFFF01

PT_NAMES = {
    PT_LOAD: "LOAD", PT_DYNAMIC: "DYNAMIC", PT_INTERP: "INTERP", PT_TLS: "TLS",
    PT_GNU_EH_FRAME: "GNU_EH_FRAME", PT_SCE_RELA: "SCE_RELA",
    PT_SCE_DYNLIBDATA: "SCE_DYNLIBDATA", PT_SCE_PROCPARAM: "SCE_PROCPARAM",
    PT_SCE_MODULE_PARAM: "SCE_MODULE_PARAM", PT_SCE_RELRO: "SCE_RELRO",
    PT_SCE_COMMENT: "SCE_COMMENT", PT_SCE_LIBVERSION: "SCE_LIBVERSION",
}

DT_NULL = 0
DT_NEEDED = 1
DT_INIT = 12
DT_FINI = 13
DT_SONAME = 14
DT_TEXTREL = 22
DT_INIT_ARRAY = 25
DT_FINI_ARRAY = 26
DT_INIT_ARRAYSZ = 27
DT_FINI_ARRAYSZ = 28
DT_FLAGS = 30
DT_PREINIT_ARRAY = 32
DT_PREINIT_ARRAYSZ = 33
DT_SCE_FINGERPRINT = 0x61000007
DT_SCE_ORIGINAL_FILENAME = 0x61000009
DT_SCE_MODULE_INFO = 0x6100000D
DT_SCE_NEEDED_MODULE = 0x6100000F
DT_SCE_MODULE_ATTR = 0x61000011
DT_SCE_EXPORT_LIB = 0x61000013
DT_SCE_IMPORT_LIB = 0x61000015
DT_SCE_EXPORT_LIB_ATTR = 0x61000017
DT_SCE_IMPORT_LIB_ATTR = 0x61000019
DT_SCE_HASH = 0x61000025
DT_SCE_PLTGOT = 0x61000027
DT_SCE_JMPREL = 0x61000029
DT_SCE_PLTREL = 0x6100002B
DT_SCE_PLTRELSZ = 0x6100002D
DT_SCE_RELA = 0x6100002F
DT_SCE_RELASZ = 0x61000031
DT_SCE_RELAENT = 0x61000033
DT_SCE_STRTAB = 0x61000035
DT_SCE_STRSZ = 0x61000037
DT_SCE_SYMTAB = 0x61000039
DT_SCE_SYMENT = 0x6100003B
DT_SCE_HASHSZ = 0x6100003D
DT_SCE_SYMTABSZ = 0x6100003F

R_X86_64_NONE = 0
R_X86_64_64 = 1
R_X86_64_PC32 = 2
R_X86_64_GLOB_DAT = 6
R_X86_64_JUMP_SLOT = 7
R_X86_64_RELATIVE = 8
R_X86_64_DTPMOD64 = 16
R_X86_64_DTPOFF64 = 17
R_X86_64_TPOFF64 = 18
R_X86_64_DTPOFF32 = 21
R_X86_64_TPOFF32 = 23

RELOC_NAMES = {
    0: "NONE", 1: "64", 2: "PC32", 6: "GLOB_DAT", 7: "JUMP_SLOT", 8: "RELATIVE",
    16: "DTPMOD64", 17: "DTPOFF64", 18: "TPOFF64", 21: "DTPOFF32", 23: "TPOFF32",
}

STB_LOCAL, STB_GLOBAL, STB_WEAK = 0, 1, 2
STT_NOTYPE, STT_OBJECT, STT_FUNC, STT_SECTION, STT_FILE, STT_TLS = 0, 1, 2, 3, 4, 6

NID_SUFFIX = bytes.fromhex("518d64a635ded8c1e6b039b1c3e55230")
ID_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"


def compute_nid(name: str) -> str:
    digest = hashlib.sha1(name.encode() + NID_SUFFIX).digest()[:8]
    return base64.b64encode(digest[::-1]).decode()[:11].replace("/", "-")


def decode_id(text: str) -> int:
    value = 0
    for ch in text:
        value = value * 64 + ID_ALPHABET.index(ch)
    return value


@dataclass
class Phdr:
    type: int
    flags: int
    offset: int
    vaddr: int
    paddr: int
    filesz: int
    memsz: int
    align: int

    @property
    def name(self) -> str:
        return PT_NAMES.get(self.type, hex(self.type))

    @property
    def perms(self) -> str:
        return "".join(c if self.flags & b else "-" for c, b in (("r", 4), ("w", 2), ("x", 1)))


@dataclass
class Symbol:
    index: int
    name: str
    info: int
    other: int
    shndx: int
    value: int
    size: int

    @property
    def bind(self) -> int:
        return self.info >> 4

    @property
    def type(self) -> int:
        return self.info & 0xF

    @property
    def nid(self) -> str:
        return self.name.split("#", 1)[0]

    @property
    def lib_id(self) -> int | None:
        parts = self.name.split("#")
        return decode_id(parts[1]) if len(parts) == 3 else None

    @property
    def mod_id(self) -> int | None:
        parts = self.name.split("#")
        return decode_id(parts[2]) if len(parts) == 3 else None


@dataclass
class Reloc:
    offset: int
    type: int
    sym: int
    addend: int


@dataclass
class ModRef:
    id: int
    name: str
    major: int
    minor: int


@dataclass
class LibRef:
    id: int
    name: str
    version: int
    attr: int = 0


@dataclass
class OrbisImage:
    path: str
    elf: bytes
    e_type: int
    entry: int
    phdrs: list[Phdr]
    dyn: list[tuple[int, int]] = field(default_factory=list)
    strtab: bytes = b""
    symbols: list[Symbol] = field(default_factory=list)
    relas: list[Reloc] = field(default_factory=list)
    jmprels: list[Reloc] = field(default_factory=list)
    needed: list[str] = field(default_factory=list)
    module_info: ModRef | None = None
    needed_modules: dict[int, ModRef] = field(default_factory=dict)
    import_libs: dict[int, LibRef] = field(default_factory=dict)
    export_libs: dict[int, LibRef] = field(default_factory=dict)
    fingerprint: bytes = b""
    original_filename: str = ""
    soname: str = ""

    def phdr(self, kind: int) -> Phdr | None:
        for p in self.phdrs:
            if p.type == kind:
                return p
        return None

    def dyn_values(self, tag: int) -> list[int]:
        return [v for t, v in self.dyn if t == tag]

    def dyn_value(self, tag: int, default: int | None = None) -> int | None:
        values = self.dyn_values(tag)
        return values[0] if values else default

    @property
    def image_size(self) -> int:
        loads = [p for p in self.phdrs if p.type in (PT_LOAD, PT_SCE_RELRO)]
        lo = min(p.vaddr for p in loads)
        hi = max(p.vaddr + p.memsz for p in loads)
        return ((hi + 0x3FFF) & ~0x3FFF) - (lo & ~0x3FFF)

    def string(self, offset: int) -> str:
        end = self.strtab.index(b"\0", offset)
        return self.strtab[offset:end].decode("utf-8", "replace")

    def read_vaddr(self, vaddr: int, size: int) -> bytes:
        for p in self.phdrs:
            if p.type in (PT_LOAD, PT_SCE_RELRO) and p.vaddr <= vaddr < p.vaddr + p.memsz:
                rel = vaddr - p.vaddr
                data = self.elf[p.offset + rel: p.offset + min(p.filesz, rel + size)]
                return data + b"\0" * (size - len(data))
        raise ValueError(f"vaddr {vaddr:#x} not mapped")

    def imports(self) -> list[tuple[Symbol, LibRef | None, ModRef | None]]:
        out = []
        for sym in self.symbols:
            if sym.shndx == 0 and sym.name and "#" in sym.name:
                out.append((sym, self.import_libs.get(sym.lib_id), self.needed_modules.get(sym.mod_id)))
        return out

    def exports(self) -> list[Symbol]:
        return [s for s in self.symbols if s.shndx != 0 and s.name and s.bind in (STB_GLOBAL, STB_WEAK)]


def unwrap_self(data: bytes) -> bytes:
    """Rebuild a flat ELF file from an FSELF container (plain ELF passes through)."""

    if data[:4] == ELF_MAGIC:
        return data
    if data[:4] != SELF_MAGIC:
        raise ValueError("not a SELF or ELF image")
    header_size, meta_size = struct.unpack_from("<HH", data, 0x0C)
    file_size = struct.unpack_from("<Q", data, 0x10)[0]
    num_entries, flags = struct.unpack_from("<HH", data, 0x18)
    entries = [struct.unpack_from("<4Q", data, 0x20 + i * 0x20) for i in range(num_entries)]
    elf_off = 0x20 + num_entries * 0x20
    ehdr = data[elf_off: elf_off + 0x40]
    if ehdr[:4] != ELF_MAGIC:
        raise ValueError("SELF has no embedded ELF header")
    e_phoff, = struct.unpack_from("<Q", ehdr, 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", ehdr, 0x36)
    phdrs = [Phdr(*struct.unpack_from("<IIQQQQQQ", data, elf_off + e_phoff + i * e_phentsize))
             for i in range(e_phnum)]
    end = max([0x40 + e_phnum * e_phentsize] + [p.offset + p.filesz for p in phdrs])
    out = bytearray(end)
    out[:0x40] = ehdr
    out[e_phoff: e_phoff + e_phnum * e_phentsize] = data[elf_off + e_phoff: elf_off + e_phoff + e_phnum * e_phentsize]
    for props, offset, filesz, memsz in entries:
        if not props & 0x800:          # has segment index
            continue
        seg = (props >> 20) & 0xFFF
        if props & 0x2 or props & 0x8:
            raise ValueError(f"SELF segment {seg} is encrypted or compressed; need FSELF")
        ph = phdrs[seg]
        out[ph.offset: ph.offset + filesz] = data[offset: offset + filesz]
    return bytes(out)


def parse(path: str | Path) -> OrbisImage:
    raw = Path(path).read_bytes()
    elf = unwrap_self(raw)
    e_type, = struct.unpack_from("<H", elf, 0x10)
    entry, e_phoff = struct.unpack_from("<QQ", elf, 0x18)
    e_phentsize, e_phnum = struct.unpack_from("<HH", elf, 0x36)
    phdrs = [Phdr(*struct.unpack_from("<IIQQQQQQ", elf, e_phoff + i * e_phentsize)) for i in range(e_phnum)]
    img = OrbisImage(str(path), elf, e_type, entry, phdrs)

    dyn_ph = img.phdr(PT_DYNAMIC)
    data_ph = img.phdr(PT_SCE_DYNLIBDATA)
    if dyn_ph is None or data_ph is None:
        return img
    for i in range(dyn_ph.filesz // 16):
        tag, val = struct.unpack_from("<qQ", elf, dyn_ph.offset + i * 16)
        if tag == DT_NULL:
            break
        img.dyn.append((tag & 0xFFFFFFFFFFFFFFFF, val))

    base = data_ph.offset

    def blob(off_tag: int, size_tag: int) -> bytes:
        off, size = img.dyn_value(off_tag), img.dyn_value(size_tag)
        if off is None or size is None:
            return b""
        return elf[base + off: base + off + size]

    img.strtab = blob(DT_SCE_STRTAB, DT_SCE_STRSZ)
    symtab = blob(DT_SCE_SYMTAB, DT_SCE_SYMTABSZ)
    for i in range(len(symtab) // 24):
        st_name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", symtab, i * 24)
        img.symbols.append(Symbol(i, img.string(st_name) if img.strtab else "", info, other, shndx, value, size))
    for name, (off_tag, size_tag) in (("relas", (DT_SCE_RELA, DT_SCE_RELASZ)),
                                      ("jmprels", (DT_SCE_JMPREL, DT_SCE_PLTRELSZ))):
        table = blob(off_tag, size_tag)
        relocs = []
        for i in range(len(table) // 24):
            off, info, addend = struct.unpack_from("<QQq", table, i * 24)
            relocs.append(Reloc(off, info & 0xFFFFFFFF, info >> 32, addend))
        setattr(img, name, relocs)

    for tag, val in img.dyn:
        if tag == DT_NEEDED:
            img.needed.append(img.string(val & 0xFFFFFFFF))
        elif tag == DT_SONAME:
            img.soname = img.string(val & 0xFFFFFFFF)
        elif tag == DT_SCE_MODULE_INFO:
            img.module_info = ModRef(val >> 48, img.string(val & 0xFFFFFFFF), (val >> 40) & 0xFF, (val >> 32) & 0xFF)
        elif tag == DT_SCE_NEEDED_MODULE:
            ref = ModRef(val >> 48, img.string(val & 0xFFFFFFFF), (val >> 40) & 0xFF, (val >> 32) & 0xFF)
            img.needed_modules[ref.id] = ref
        elif tag == DT_SCE_IMPORT_LIB:
            ref = LibRef(val >> 48, img.string(val & 0xFFFFFFFF), (val >> 32) & 0xFFFF)
            img.import_libs[ref.id] = ref
        elif tag == DT_SCE_EXPORT_LIB:
            ref = LibRef(val >> 48, img.string(val & 0xFFFFFFFF), (val >> 32) & 0xFFFF)
            img.export_libs[ref.id] = ref
        elif tag == DT_SCE_ORIGINAL_FILENAME:
            img.original_filename = img.string(val & 0xFFFFFFFF)
        elif tag == DT_SCE_FINGERPRINT:
            img.fingerprint = elf[base + val: base + val + 20]
    for tag, val in img.dyn:
        if tag == DT_SCE_IMPORT_LIB_ATTR and (val >> 48) in img.import_libs:
            img.import_libs[val >> 48].attr = val & 0xFFFFFFFFFFFF
        elif tag == DT_SCE_EXPORT_LIB_ATTR and (val >> 48) in img.export_libs:
            img.export_libs[val >> 48].attr = val & 0xFFFFFFFFFFFF
    return img


def load_name_db(*paths: str | Path) -> dict[str, str]:
    """Map NID -> symbol name from plain name lists (one name per line)."""

    db: dict[str, str] = {}
    for path in paths:
        p = Path(path)
        if not p.is_file():
            continue
        for line in p.read_text(encoding="utf-8-sig", errors="replace").splitlines():
            name = line.strip()
            if name and not name.startswith("#"):
                db.setdefault(compute_nid(name), name)
    return db
