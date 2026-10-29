#!/usr/bin/env python3
"""Build build/ghostldr/selector.bin (the game selector module) from selector.elf.

    python3 ghostldr/tools/mksel.py --abi N build/ghostldr/selector.elf build/ghostldr/selector.bin

The core relocates the module's image and copies it into executable memory
(src/sel.c), so it is checked here: one image linked at 0, no writable data,
only R_X86_64_RELATIVE relocations inside the image, and the entry point
inside it.  Layout: struct sel_blob (include/sel_api.h, 48 bytes), the
image, then the relocations as u32 pairs {offset, addend}.  The header records
GHOST_API_VERSION and GHOST_ABI (the Makefile's checksum of ghost.h and
ghost_api.h): the core refuses a selector built against other headers.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

MAGIC = 0x4C45535244484C47  # "GHLDRSEL"
FORMAT = 2
HEADER = struct.Struct("<QIIIIIIIIII")
R_X86_64_RELATIVE = 8
MAX_IMAGE = 4 << 20  # the core reads it into the 4 MiB staging buffer
SHF_WRITE, SHF_ALLOC = 1, 2


def sections(elf: bytes) -> dict[str, tuple[int, int, int, int, int]]:
    """name -> (type, flags, addr, offset, size)"""
    shoff, = struct.unpack_from("<Q", elf, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x3A)
    hdrs = [struct.unpack_from("<IIQQQQIIQQ", elf, shoff + i * shentsize) for i in range(shnum)]
    strtab = hdrs[shstrndx]
    out = {}
    for h in hdrs:
        name = elf[strtab[4] + h[0]:elf.index(b"\0", strtab[4] + h[0])].decode()
        out[name] = (h[1], h[2], h[3], h[4], h[5])
    return out


def symbols(elf: bytes, secs: dict) -> dict[str, int]:
    if ".symtab" not in secs:
        return {}
    _, _, _, off, size = secs[".symtab"]
    stroff = secs[".strtab"][3]
    out = {}
    for i in range(size // 24):
        name, info, other, shndx, value, sz = struct.unpack_from("<IBBHQQ", elf, off + i * 24)
        if name:
            out[elf[stroff + name:elf.index(b"\0", stroff + name)].decode()] = value
    return out


def api_version() -> int:
    text = (Path(__file__).resolve().parents[1] / "native/include/ghost_api.h").read_text()
    return int(re.search(r"#define GHOST_API_VERSION (\d+)", text).group(1))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--abi", type=lambda s: int(s, 0), required=True, help="GHOST_ABI (the Makefile's header checksum)")
    ap.add_argument("elf", type=Path)
    ap.add_argument("out", type=Path)
    args = ap.parse_args()
    elf_path, out = args.elf, args.out
    elf = elf_path.read_bytes()
    if elf[:4] != b"\x7fELF" or elf[4] != 2:
        raise SystemExit(f"{elf_path}: not an ELF64 file")
    secs = sections(elf)
    syms = symbols(elf, secs)
    problems, relocs = [], []
    for name, (typ, flags, addr, off, size) in secs.items():
        if not size or not flags & SHF_ALLOC:
            continue
        if name.startswith(".rela"):
            for i in range(size // 24):
                r_off, info, addend = struct.unpack_from("<QQq", elf, off + i * 24)
                if info & 0xFFFFFFFF != R_X86_64_RELATIVE or info >> 32:
                    problems.append(f"{name}: relocation type {info & 0xFFFFFFFF} at {r_off:#x}")
                relocs.append((r_off, addend))
        elif name.startswith(".rel."):
            problems.append(f"{name}: REL relocations")
        elif flags & SHF_WRITE and name not in (".dynamic", ".data.rel.ro"):   # relocated here, read-only after
            problems.append(f"{name}: {size} bytes of writable data")
    if problems:
        raise SystemExit("selector.elf cannot be loaded as is:\n  " + "\n  ".join(problems))
    phoff, = struct.unpack_from("<Q", elf, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x36)
    loads = [struct.unpack_from("<IIQQQQQQ", elf, phoff + i * phentsize) for i in range(phnum)]
    loads = [p for p in loads if p[0] == 1]
    if len(loads) != 1 or loads[0][3] != 0 or loads[0][5] != loads[0][6]:
        raise SystemExit("selector.elf must have one PT_LOAD at 0 with no bss")
    _, _, offset, _, _, filesz, _, _ = loads[0]
    image = elf[offset:offset + filesz]
    image += b"\0" * (-len(image) % 16)
    entry, = struct.unpack_from("<Q", elf, 0x18)
    font, font_end = syms.get("sel_font_glf", 0), syms.get("sel_font_glf_end", 0)
    if entry >= len(image) or not 0 < font < font_end <= len(image):
        raise SystemExit(f"bad entry {entry:#x} or font {font:#x}-{font_end:#x} (image {len(image):#x})")
    if image[font:font + 4] != b"GLF1":
        raise SystemExit("the built-in font is not in the image")
    for r_off, addend in relocs:
        if not (0 <= r_off <= len(image) - 8 and 0 <= addend < len(image)):
            raise SystemExit(f"relocation {r_off:#x} -> {addend:#x} outside the image ({len(image):#x})")
    table = b"".join(struct.pack("<II", o, a) for o, a in sorted(relocs))
    if len(image) + HEADER.size + len(table) > MAX_IMAGE:
        raise SystemExit(f"image too large: {len(image)} bytes")
    blob = HEADER.pack(MAGIC, FORMAT, HEADER.size, api_version(), args.abi & 0xFFFFFFFF, len(image), entry, font,
                       font_end - font, len(relocs), 0) + image + table
    out.write_bytes(blob)
    print(f"{out.name}: {len(blob)} bytes (image {len(image) // 1024} KiB, of it the font "
          f"{(font_end - font) // 1024} KiB; {len(relocs)} relocations)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
