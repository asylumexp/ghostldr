#!/usr/bin/env python3
"""Pack the runtime extension: ext.elf (ext/ext.ld) + the full NID table ->
build/ghostldr/ext.bin.  The layout is struct ext_blob in ghost_api.h.

    python3 ghostldr/tools/mkext.py --abi N build/ghostldr/ext.elf build/ghostldr/ext.bin
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import elf64  # noqa: E402
import gen_nids  # noqa: E402

MAGIC = 0x5458455244484C47  # "GHLDREXT"
FORMAT = 1
API_HEADER = Path(__file__).resolve().parents[1] / "native/include/ghost_api.h"
API_VERSION = int(re.search(r"#define GHOST_API_VERSION (\d+)", API_HEADER.read_text()).group(1))
HEADER = struct.Struct("<QIIIIQ14I2I")
BLOB_EARLY = 1  # header flags: ext_early is there (struct ext_blob early_entry)
assert HEADER.size == 96
PAGE = 0x4000


def align(n: int, a: int) -> int:
    return (n + a - 1) & ~(a - 1)


def code_image(elf: elf64.Elf) -> dict:
    """The flat image, its relocations and layout symbols, checked."""
    sym = elf.symbols
    for need in ("ext_init", "__ext_data_start", "__ext_image_end", "__ext_families_start", "__ext_families_end"):
        if need not in sym:
            raise SystemExit(f"{elf.path}: no symbol {need} (link with ext/ext.ld)")
    img, memsz = elf.image()
    text = sym["__ext_data_start"].value
    memsz = align(max(memsz, sym["__ext_image_end"].value), PAGE)
    if text % PAGE or not 0 < text < memsz:
        raise SystemExit(f"{elf.path}: text size {text:#x} is not a 16 KiB boundary inside the image")
    for seg in elf.segments:
        if seg.type == elf64.PT_LOAD and seg.vaddr < text < seg.vaddr + seg.memsz:
            raise SystemExit(f"{elf.path}: a segment crosses the text/data boundary")
    if elf.section(".tdata") or elf.section(".tbss"):
        raise SystemExit(f"{elf.path}: thread-local variables are not supported")
    relocs = []
    for off, rtype, symidx, addend in elf.relas():
        if rtype != elf64.R_X86_64_RELATIVE or symidx:
            raise SystemExit(f"{elf.path}: relocation type {rtype} at {off:#x}: the extension may only "
                             "reference its own code and data (call the core through G)")
        if not (0 <= addend < memsz and off + 8 <= memsz):
            raise SystemExit(f"{elf.path}: relocation {off:#x} -> {addend:#x} outside the image")
        relocs.append((off, addend))
    entry = sym["ext_init"].value
    if entry >= text:
        raise SystemExit(f"{elf.path}: ext_init is not in the text")
    early = sym["ext_early"].value if "ext_early" in sym else None
    if early is not None and early >= text:
        raise SystemExit(f"{elf.path}: ext_early is not in the text")
    fam = sym["__ext_families_start"].value
    nfam = (sym["__ext_families_end"].value - fam) // 8
    return {"image": bytes(img), "memsz": memsz, "text": text, "entry": entry, "early": early,
            "relocs": sorted(relocs), "families": (fam, nfam)}


def pack(code: dict, nids: tuple[int, bytes, bytes, bytes], abi: int) -> bytes:
    count, keys, offs, pool = nids
    parts: list[bytes] = []
    pos = HEADER.size

    def place(data: bytes, alignment: int = 16) -> int:
        nonlocal pos
        start = align(pos, alignment)
        parts.append(b"\0" * (start - pos) + data)
        pos = start + len(data)
        return start

    image_off = place(code["image"])
    reloc_off = place(b"".join(struct.pack("<II", o, a) for o, a in code["relocs"]))
    keys_off = place(keys)
    names_off = place(offs)
    str_off = place(pool)
    size = pos
    header = HEADER.pack(MAGIC, FORMAT, HEADER.size, API_VERSION, abi, size,
                         image_off, len(code["image"]), code["memsz"], code["text"], code["entry"],
                         reloc_off, len(code["relocs"]), code["families"][0], code["families"][1],
                         count, keys_off, names_off, str_off, len(pool), code["early"] or 0,
                         BLOB_EARLY if code["early"] is not None else 0)
    return header + b"".join(parts)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("elf", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--abi", type=lambda s: int(s, 0), required=True, help="GHOST_ABI (the Makefile's header checksum)")
    args = ap.parse_args()
    code = code_image(elf64.Elf(args.elf))
    nids = gen_nids.encode_table(gen_nids.name_db())
    blob = pack(code, nids, args.abi & 0xFFFFFFFF)
    args.out.write_bytes(blob)
    print(f"{args.out}: {len(blob)} bytes, CRC-32 {zlib.crc32(blob):08x}: code {code['memsz'] >> 10} KiB "
          f"(text {code['text'] >> 10} KiB, {len(code['relocs'])} relocations, {code['families'][1]} families), "
          f"{nids[0]} NIDs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
