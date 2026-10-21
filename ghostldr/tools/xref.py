#!/usr/bin/env python3
"""Cross-reference index for an Orbis image (linear sweep of the text segment).
    xref.py IMAGE ADDR...        list instructions that reference ADDR (rip-relative or call/jmp)
The index is cached next to the image as IMAGE.xref."""
import pickle
import re
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import orbis_elf as oe
import capstone

RIP = re.compile(r"rip ([+-]) 0x([0-9a-f]+)")


def build(path):
    img = oe.parse(path)
    text = next(p for p in img.phdrs if p.type == oe.PT_LOAD and p.flags & 1)
    code = img.elf[text.offset:text.offset + text.filesz]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.skipdata = True
    refs = defaultdict(list)
    for addr, size, mnem, ops in md.disasm_lite(code, text.vaddr):
        m = RIP.search(ops)
        if m:
            disp = int(m.group(2), 16) * (1 if m.group(1) == "+" else -1)
            refs[addr + size + disp].append((addr, f"{mnem} {ops}"))
        elif mnem in ("call", "jmp") and ops.startswith("0x"):
            refs[int(ops, 16)].append((addr, f"{mnem} {ops}"))
    return dict(refs)


def main():
    path = Path(sys.argv[1])
    cache = path.with_suffix(path.suffix + ".xref")
    if cache.exists() and cache.stat().st_mtime > path.stat().st_mtime:
        refs = pickle.loads(cache.read_bytes())
    else:
        refs = build(path)
        cache.write_bytes(pickle.dumps(refs))
    for a in sys.argv[2:]:
        lo, _, hi = a.partition("-")
        lo = int(lo, 16)
        hi = int(hi, 16) if hi else lo + 1
        hits = sorted((t, x) for t in refs if lo <= t < hi for x in refs[t])
        print(f"== {a}: {len(hits)} refs")
        for t, (addr, ins) in hits:
            print(f"  {addr:#9x} -> {t:#x}: {ins}")


if __name__ == "__main__":
    main()
