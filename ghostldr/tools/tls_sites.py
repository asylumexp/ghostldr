#!/usr/bin/env python3
"""The thread-pointer loads a module's static TLS needs patched, found by
disassembly (research/hle-specs/kernel-modules.md §7.3.1).

    python3 ghostldr/tools/tls_sites.py MODULE [--out FILE]

The core rewrites each "mov r64, fs:[0]" and "add r64, fs:[0]" of a module
with static TLS (the executable, or a prx with initial-exec relocations) to
read fs:[-8].  Without a list it scans the whole executable segment for the
byte pattern, rodata included.  This tool walks the functions .eh_frame
describes (PT_GNU_EH_FRAME), disassembles each from its start (capstone) and
lists:

  patch    "mov/add r64, fs:[0]", with or without the 66 66 66 padding of
           the relaxed form: the text offset of the fs prefix (0x64)
  refuse   any other fs: access (another displacement, a base or index
           register): left for manual work

and the core scan's matches that lie outside every function (data).  --out
writes the patch list, one hex offset per line, for a title profile's
"tls_sites" (ghostldr/data/titles/; profiles.py stores it in the profile, the
core applies exactly those sites after checking each one's bytes).
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import orbis_elf as oe  # noqa: E402


def tls_form(code: bytes, i: int) -> bool:
    """The core's tls_form (loader.c): fs, REX.W, mov/add, modrm [disp32], SIB 25."""
    return (i + 9 <= len(code) and code[i] == 0x64 and code[i + 1] in (0x48, 0x4C) and code[i + 2] in (0x8B, 0x03)
            and (code[i + 3] & 0xC7) == 0x04 and code[i + 4] == 0x25)


def core_scan(code: bytes) -> list[int]:
    """What the core's patch_tls rewrites without a list: every match with displacement 0."""
    out, i = [], 0
    while i + 9 <= len(code):
        if tls_form(code, i) and code[i + 5:i + 9] == b"\0\0\0\0":
            out.append(i)
            i += 9
        else:
            i += 1
    return out


# ---- .eh_frame: the functions' address ranges ----

def _uleb(b: bytes, p: int) -> tuple[int, int]:
    v = shift = 0
    while True:
        c = b[p]
        p += 1
        v |= (c & 0x7F) << shift
        shift += 7
        if not c & 0x80:
            return v, p


def _sleb(b: bytes, p: int) -> tuple[int, int]:
    v = shift = 0
    while True:
        c = b[p]
        p += 1
        v |= (c & 0x7F) << shift
        shift += 7
        if not c & 0x80:
            return (v - (1 << shift) if c & 0x40 else v), p


def _encoded(b: bytes, p: int, enc: int, vaddr: int) -> tuple[int, int]:
    """A DW_EH_PE value at b[p] (whose address is vaddr): (value, next p)."""
    fmt = enc & 0x0F
    if fmt == 0x01:
        v, q = _uleb(b, p)
    elif fmt == 0x09:
        v, q = _sleb(b, p)
    else:
        code, size = {0x00: ("<Q", 8), 0x02: ("<H", 2), 0x03: ("<I", 4), 0x04: ("<Q", 8), 0x0A: ("<h", 2),
                      0x0B: ("<i", 4), 0x0C: ("<q", 8)}[fmt]
        v, q = struct.unpack_from(code, b, p)[0], p + size
    if enc & 0x70 == 0x10:  # pcrel
        v += vaddr
    return v, q


def functions(img: oe.OrbisImage, text: bytes, text_vaddr: int) -> list[tuple[int, int]]:
    """(start, end) of every FDE, as text offsets, from the module's .eh_frame."""
    hdr_ph = img.phdr(oe.PT_GNU_EH_FRAME)
    if not hdr_ph:
        return []
    h = hdr_ph.vaddr - text_vaddr
    if text[h] != 1:
        raise SystemExit(f"{img.path}: .eh_frame_hdr version {text[h]}")
    frame, _ = _encoded(text, h + 4, text[h + 1], h + 4)
    out, p, cies = [], frame, {}
    while p + 4 <= len(text):
        length = struct.unpack_from("<I", text, p)[0]
        if length == 0:
            break
        body, start = p + 4, p
        if length == 0xFFFFFFFF:
            length = struct.unpack_from("<Q", text, p + 4)[0]
            body = p + 12
        end = body + length
        cie_id = struct.unpack_from("<I", text, body)[0]
        if cie_id == 0:  # CIE: its augmentation says how FDEs encode addresses
            q = body + 5
            aug_end = text.index(b"\0", q)
            aug = text[q:aug_end].decode()
            q = aug_end + 1
            if "eh" in aug:
                q += 8
            _, q = _uleb(text, q)  # code alignment
            _, q = _sleb(text, q)  # data alignment
            q = q + 1 if text[body + 4] == 1 else _uleb(text, q)[1]  # return register
            enc = 0x00
            if aug.startswith("z"):
                _, q = _uleb(text, q)
                for c in aug[1:]:
                    if c == "R":
                        enc = text[q]
                        q += 1
                    elif c == "P":
                        penc = text[q]
                        _, q = _encoded(text, q + 1, penc, q + 1)
                    elif c == "L":
                        q += 1
            cies[start] = enc
        else:  # FDE: pc_begin with the CIE's encoding, pc_range in its format alone
            enc = cies.get(body - cie_id, 0x1B)
            pc, q = _encoded(text, body + 4, enc, body + 4)
            rng, _ = _encoded(text, q, enc & 0x0F, q)
            if rng:
                out.append((pc, pc + rng))
        p = end
    return out


def sites(path: Path) -> dict:
    """{patch: [...], refuse: [(offset, text)], outside: [...], functions: n} for a module."""
    import capstone
    img = oe.parse(path)
    ph = next(p for p in img.phdrs if p.type == oe.PT_LOAD and p.flags & 1)
    text = img.elf[ph.offset:ph.offset + ph.filesz]
    funcs = functions(img, text, ph.vaddr)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    patch, refuse = set(), []
    for start, end in funcs:
        for addr, size, mnem, ops in md.disasm_lite(text[start:end], start):
            if "fs:" not in ops:
                continue
            at = next((addr + k for k in range(size) if text[addr + k] == 0x64 and tls_form(text, addr + k)), None)
            if at is not None and text[at + 5:at + 9] == b"\0\0\0\0" and at + 9 == addr + size:
                patch.add(at)
            else:
                refuse.append((addr, f"{mnem} {ops}"))
    inside, refuse = sorted(patch), sorted(set(refuse))  # FDEs may cover a function twice
    scan = core_scan(text)
    covered = [(s, e) for s, e in sorted(funcs)]

    def in_function(o: int) -> bool:
        import bisect
        i = bisect.bisect_right(covered, (o, float("inf"))) - 1
        return any(s <= o < e for s, e in covered[max(0, i - 2):i + 1])

    outside = [o for o in scan if not in_function(o)]
    return {"patch": inside, "refuse": refuse, "outside": outside, "scan": scan, "functions": len(funcs)}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("module", type=Path)
    ap.add_argument("--out", type=Path, help="write the patch list (one hex text offset per line)")
    args = ap.parse_args()
    r = sites(args.module)
    print(f"{args.module.name}: {r['functions']} functions, {len(r['patch'])} sites to patch, "
          f"{len(r['refuse'])} refused, the core's scan matches {len(r['scan'])} "
          f"({len(r['outside'])} outside every function)")
    for at, what in r["refuse"][:50]:
        print(f"  refused {at:#x}: {what}")
    for at in r["outside"][:20]:
        print(f"  outside {at:#x}: a scan match in data, not patched with the list")
    if args.out:
        args.out.write_text(f"# {args.module.name}: static TLS sites (tls_sites.py), text offsets of the fs prefix\n"
                            + "".join(f"{o:#x}\n" for o in r["patch"]))
        print(f"wrote {len(r['patch'])} sites to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
