#!/usr/bin/env python3
"""Linear-sweep an Orbis image's code segment and classify every fs-segment access."""
import sys, re, json
from collections import Counter
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import orbis_elf as oe
import capstone

img = oe.parse(sys.argv[1])
text = next(p for p in img.phdrs if p.type == oe.PT_LOAD and p.flags & 1)
code = img.elf[text.offset:text.offset + text.filesz]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.skipdata = True
forms = Counter(); samples = {}; sites = []
pos = 0
for insn in md.disasm_lite(code, 0):
    addr, size, mnem, ops = insn
    if "fs:" in ops:
        key = re.sub(r"0x[0-9a-f]+", "N", mnem + " " + ops)
        forms[key] += 1
        raw = code[addr:addr + size]
        samples.setdefault(key, []).append((hex(addr), raw.hex(), ops))
        sites.append((addr, raw.hex(), mnem, ops))
for key, n in forms.most_common():
    print(n, key, samples[key][:2])
print("total", len(sites))
if len(sys.argv) > 2:
    Path(sys.argv[2]).write_text(json.dumps(sites))
