#!/usr/bin/env python3
"""Ask SWRR's sandbox what it can reach (USB, /data, ...) through Luac0re.

Needs only Luac0re's Remote Lua Loader on 9026: run it on a console with no
payloads sent to see the stock sandbox.  Prints the probe's UDP report.
Another probe script (same PROBE_LOG_IP/PORT convention, ending with a
"done" line) can be given instead of ghostldr/lua/sandbox_probe.lua.

    python3 ghostldr/host/sandbox_probe.py [probe.lua] [--wait SECONDS]
"""

from __future__ import annotations

import argparse
import socket
import time
from pathlib import Path

from console import console_ip

ROOT = Path(__file__).resolve().parents[2]
PROBE = ROOT / "ghostldr/lua/sandbox_probe.lua"
PORT = 18195   # the core's log port (18194) may belong to recorder.py


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("probe", nargs="?", type=Path, default=PROBE)
    ap.add_argument("--wait", type=float, default=30, help="seconds to wait for the report")
    args = ap.parse_args()
    host = console_ip()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect((host, 9))
        local_ip = probe.getsockname()[0]
    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.bind(("0.0.0.0", PORT))
    listener.settimeout(1)
    lua = f'PROBE_LOG_IP = "{local_ip}"\nPROBE_LOG_PORT = {PORT}\n' + args.probe.read_text()
    with socket.create_connection((host, 9026), timeout=10) as s:
        s.sendall(lua.encode())
        s.shutdown(socket.SHUT_WR)
    deadline = time.time() + args.wait
    while time.time() < deadline:
        try:
            data, _ = listener.recvfrom(4096)
        except socket.timeout:
            continue
        line = data.decode("utf-8", "replace").rstrip("\0")
        print(line, flush=True)
        if line.endswith("done"):
            return 0
    print("no complete report (check the notification on screen)")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
