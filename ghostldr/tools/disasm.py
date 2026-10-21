#!/usr/bin/env python3
"""Disassemble an Orbis image with import names on PLT calls.
    disasm.py IMAGE ADDR [COUNT] [--before N]     |    disasm.py IMAGE --callers NAME"""
import sys, re
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import orbis_elf as oe
import capstone

ROOT = Path(__file__).resolve().parents[2]
img = oe.parse(sys.argv[1])
db = oe.load_name_db(ROOT / "build/nid/known_names.txt")
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
got2name = {r.offset: db.get(img.symbols[r.sym].nid, img.symbols[r.sym].nid) for r in img.jmprels}
text = next(p for p in img.phdrs if p.type == oe.PT_LOAD and p.flags & 1)
plt = {}
code_all = img.elf[text.offset:text.offset + text.filesz]
for m in re.finditer(rb"\xff\x25", code_all):
    a = text.vaddr + m.start()
    disp = int.from_bytes(code_all[m.start() + 2:m.start() + 6], "little", signed=True)
    tgt = a + 6 + disp
    if tgt in got2name:
        plt[a] = got2name[tgt]

def name_of(t):
    return plt.get(t, "")

def dis(addr, count, before=0):
    start = addr - before
    for i, ins in enumerate(md.disasm(img.read_vaddr(start, count * 16), start)):
        if i > count:
            break
        extra = ""
        if ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
            extra = "  ; " + name_of(int(ins.op_str, 16))
        print(f"{ins.address:#9x}: {ins.mnemonic:6s} {ins.op_str}{extra}")

if sys.argv[2] == "--callers":
    want = sys.argv[3]
    stubs = [a for a, n in plt.items() if n == want]
    for ins in md.disasm_lite(code_all[:0xffd000], text.vaddr):  # noqa
        if ins[2] == "call" and ins[3].startswith("0x") and int(ins[3], 16) in stubs:
            print(hex(ins[0]))
else:
    before = int(sys.argv[sys.argv.index("--before") + 1], 0) if "--before" in sys.argv else 0
    dis(int(sys.argv[2], 0), int(sys.argv[3], 0) if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else 40, before)
