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
binary.with_suffix(".json").write_text(json.dumps(info, indent=2) + "\n")
print(json.dumps({k: hex(v) for k, v in info.items()}))
