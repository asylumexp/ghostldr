#!/usr/bin/env python3
"""Assemble the GPU shaders of ghostldr's GLES (native/ext/gcn/*.s) for the
PS4's GPU (GCN gfx7) with LLVM's llvm-mc, into native/ext/src/gles_gcn.h.

The header is checked in, so building ghostldr needs no AMDGPU assembler;
run this after changing a shader:

    python3 ghostldr/tools/mkgcn.py            (LLVM=/path/to/llvm/bin to choose)
    python3 ghostldr/tools/mkgcn.py --check    (exit 1 when the header is stale)

The header records each source's SHA-256, so tests/test_gles.py can tell a
stale header from the sources alone, without LLVM.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "native/ext/gcn"
OUT = ROOT / "native/ext/src/gles_gcn.h"
SHADERS = ("gles_vs", "gles_ps", "gles_ps_yuv")


def llvm_bin() -> Path:
    for cand in (os.environ.get("LLVM"), "/opt/homebrew/opt/llvm@18/bin", "/usr/local/opt/llvm@18/bin",
                 "/usr/lib/llvm-18/bin"):
        if cand and (Path(cand) / "llvm-mc").exists():
            return Path(cand)
    raise SystemExit("mkgcn: no llvm-mc found (set LLVM=/path/to/llvm/bin)")


def source_hash(name: str) -> str:
    return hashlib.sha256((SRC / f"{name}.s").read_bytes()).hexdigest()


def assemble(name: str, llvm: Path) -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        obj, raw = Path(tmp) / "s.o", Path(tmp) / "s.bin"
        subprocess.run([str(llvm / "llvm-mc"), "-arch=amdgcn", "-mcpu=gfx700", "-filetype=obj",
                        str(SRC / f"{name}.s"), "-o", str(obj)], check=True)
        subprocess.run([str(llvm / "llvm-objcopy"), "-O", "binary", "--only-section=.text", str(obj), str(raw)],
                       check=True)
        return raw.read_bytes()


def registers(name: str) -> tuple[int, int]:
    """the VGPRs and SGPRs a shader needs: its highest register + 1 (SGPRs: + 2 for VCC)"""
    text = re.sub(r"//.*", "", (SRC / f"{name}.s").read_text())
    def top(kind: str) -> int:
        hi = -1
        for m in re.finditer(rf"\b{kind}(?:\[(\d+):(\d+)\]|(\d+))", text):
            hi = max(hi, int(m.group(2) or m.group(3)))
        return hi + 1
    return top("v"), top("s") + 2


def render(llvm: Path) -> str:
    out = ["/* GCN gfx7 code of ghostldr's GPU shaders (native/ext/gcn), made by",
           " * tools/mkgcn.py with LLVM's llvm-mc: do not edit. */", "#pragma once", ""]
    for name in SHADERS:
        code = assemble(name, llvm)
        words = struct.unpack(f"<{len(code) // 4}I", code)
        vgprs, sgprs = registers(name)
        out.append(f"/* {name}.s sha256 {source_hash(name)} */")
        out.append(f"#define GCN_{name.upper()}_VGPRS {vgprs}")
        out.append(f"#define GCN_{name.upper()}_SGPRS {sgprs} /* with VCC */")
        out.append(f"static const u32 k_{name}[{len(words)}] = {{")
        for i in range(0, len(words), 6):
            out.append("    " + ", ".join(f"0x{w:08x}" for w in words[i:i + 6]) + ",")
        out.append("};")
        out.append("")
    return "\n".join(out)


def stale_by_hash() -> list[str]:
    """the shaders whose source no longer matches the hash in the header"""
    text = OUT.read_text() if OUT.exists() else ""
    return [n for n in SHADERS if f"/* {n}.s sha256 {source_hash(n)} */" not in text]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="only check that the header matches the sources")
    args = ap.parse_args()
    if args.check:
        stale = stale_by_hash()
        if stale:
            print(f"mkgcn: {OUT.name} is stale for {', '.join(stale)}: run tools/mkgcn.py", file=sys.stderr)
            return 1
        return 0
    OUT.write_text(render(llvm_bin()))
    print(f"mkgcn: wrote {OUT.relative_to(ROOT.parent)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
