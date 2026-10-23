#!/usr/bin/env python3
"""Relax UltraC0re's eboot-base sanity check so it accepts a non-randomised eboot.

UltraC0re 1.0.2's PS2 bootstrap (builds/jakx_ultrac0re.bin, loaded at guest
0x71CAF0) accepts a leaked eboot base only if it is above 0x01000000.  On a
13.60 PS5 (CUSA07992 1.04) Jak X's eboot.bin was mapped at 0x400000,
so leak_eboot() rejected every round, returned 0 and main() spun in its
`for (;;)` (black screen after Triangle, no notifications).

The check compiles to (0x71cccc / 0x71ccdc):
    lui   $t2, 0x100        ; t2 = 0x01000000
    addiu $t2, $t2, 1       ; t2 = 0x01000001, reject e < t2
This rewrites it to t2 = 0x00400000 (accept e >= 0x400000; the 4 KiB
alignment and < 0x80000000 checks stay).

    python3 ghostldr/tools/ultrac0re_patch.py <jakx_ultrac0re.bin> <out.bin>

Then rebuild VMC0.card with UltraC0re's scripts/craft/craft_ultrac0re.py run
in a directory holding the release VMC0.card (crafting the unpatched .bin
there reproduces the release card byte for byte).
"""
import sys

BASE = 0x71CAF0
PATCHES = [
    (0x71CCCC, "00010a3c", "40000a3c"),  # lui $t2, 0x100  -> lui $t2, 0x40
    (0x71CCDC, "01004a25", "00004a25"),  # addiu $t2, $t2, 1 -> addiu $t2, $t2, 0
]

b = bytearray(open(sys.argv[1], "rb").read())
for addr, old, new in PATCHES:
    o = addr - BASE
    if b[o:o + 4] != bytes.fromhex(old):
        raise SystemExit(f"{addr:#x}: expected {old}, found {b[o:o + 4].hex()}")
    b[o:o + 4] = bytes.fromhex(new)
open(sys.argv[2], "wb").write(b)
print(f"patched {len(PATCHES)} instructions -> {sys.argv[2]}")
