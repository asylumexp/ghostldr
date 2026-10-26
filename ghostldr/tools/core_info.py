#!/usr/bin/env python3
"""Write core.json (layout symbols) next to the flat ghostldr core.bin."""

import json
import os
import subprocess
import sys
from pathlib import Path

elf, binary = Path(sys.argv[1]), Path(sys.argv[2])
nm = Path(os.environ.get("LLVM", "/opt/homebrew/opt/llvm@18/bin")) / "llvm-nm"   # the Makefile exports LLVM
symbols = {}
for line in subprocess.run([str(nm), str(elf)], capture_output=True, text=True, check=True).stdout.splitlines():
    parts = line.split()
    if len(parts) == 3:
        symbols[parts[2]] = int(parts[0], 16)
info = {k: symbols[k] for k in ("ghost_entry", "__text_end", "__data_start", "__image_end", "__rela_start", "__rela_end")}
info["bin_size"] = binary.stat().st_size
if info["ghost_entry"] != 0:
    raise SystemExit("ghost_entry must be at offset 0")
# The core's image (code, data, bss) starts 16 KiB into the emulator's IPU JIT
# buffer, which has 256 KiB from there.  The loader's small area (stubs,
# trampolines) starts at the image's end, rounded to 16 KiB (ghostldr.lua),
# and needs at least 16 KiB of that buffer; past it, arena.c takes the whole
# next buffer (the PS2 EE's 32 MiB, where titles' code goes) for it.
SMALL_AREA_AT_MOST = 0x3C000
small = (info["__image_end"] + 0x3FFF) & ~0x3FFF
if small > SMALL_AREA_AT_MOST:
    binary.unlink()
    raise SystemExit(f"core image ends at {info['__image_end']:#x}: {small - SMALL_AREA_AT_MOST} bytes over "
                     f"{SMALL_AREA_AT_MOST:#x}, where the small area must start (the IPU buffer's last 16 KiB). "
                     f"Move large static tables out of the core (allocate them at run time).")
binary.with_suffix(".json").write_text(json.dumps(info, indent=2) + "\n")
print(json.dumps({k: hex(v) for k, v in info.items()}))
