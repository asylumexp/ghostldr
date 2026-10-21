#!/usr/bin/env python3
"""Read memory of the running title through Luac0re's remote Lua loader.

The loader runs the Lua in SWRR's process (the title's process too) but sends
nothing back, so the values go out through the core's logf to the UDP log
(run.py logs must be listening; lines land in build/ghostldr/run.log).

    python3 ghostldr/host/peek.py eboot+0x1a8cfa0:0x40 0x1000000000:0x20

ADDR is absolute or eboot+OFF (title eboot base 0x900d90000); LEN defaults to 8.
"""

from __future__ import annotations

import argparse
import socket
import subprocess
import time
from pathlib import Path

from console import console_ip

ROOT = Path(__file__).resolve().parents[2]
CORE_BASE = 0x9009D8000
EBOOT_BASE = 0x900D90000
LOG = ROOT / "build/ghostldr/run.log"


def core_sym(name: str) -> int:
    out = subprocess.run(["nm", str(ROOT / "build/ghostldr/core.elf")], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return CORE_BASE + int(parts[0], 16)
    raise SystemExit(f"{name} not in core.elf")


def parse(spec: str) -> tuple[int, int]:
    addr, _, length = spec.partition(":")
    base = 0
    if addr.startswith("eboot+"):
        base, addr = EBOOT_BASE, addr[6:]
    return base + int(addr, 0), int(length, 0) if length else 8


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("ranges", nargs="+")
    ap.add_argument("--host", default=None, help="console address (default: looked up by host-id)")
    args = ap.parse_args()
    args.host = args.host or console_ip()
    logf = core_sym("logf")
    lines = [
        f"local L = func_wrap({logf:#x})",
        "local buf = malloc(256)",
        "local fmt = malloc(4); write_string(fmt, '%s')",
        "local function say(s) write_string(buf, s); L(fmt, buf) end",
    ]
    for spec in args.ranges:
        addr, length = parse(spec)
        lines.append(f"for o = 0, {length - 1}, 32 do local a = {addr:#x} + o; local s = string.format('peek %x:', a); "
                     f"for i = 0, 3 do if o + i * 8 < {length} then s = s .. string.format(' %016x', read64(a + i * 8)) end end; "
                     f"say(s) end")
    mark = time.strftime("%H:%M:%S")
    with socket.create_connection((args.host, 9026), timeout=10) as s:
        s.sendall("\n".join(lines).encode() + b"\n")
        s.shutdown(socket.SHUT_WR)
    time.sleep(2)
    for line in LOG.read_text(errors="replace").splitlines()[-400:]:
        if " peek " in line and line[:8] >= mark:
            print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
