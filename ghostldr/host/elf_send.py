#!/usr/bin/env python3
"""Send an ELF payload to the console's ELF loader and show its output.

    python3 ghostldr/host/elf_send.py payload.elf [--host PS5] [--port 9021]
    python3 ghostldr/host/elf_send.py --cmd status        # or quit, help

ghostldr's ELF loader (run.py elfldr, docs/elfldr.md) sends the payload's
stdout and stderr back on the connection, with lines of its own that start
with "[elfldr] ", and closes it once the payload has finished; this prints
all of it as it comes and exits with the payload's exit code (1 when it was
refused or crashed).  `nc PS5 9021 < payload.elf` does the same.

On a jailbroken console, elfldr (ps5-payload-dev) closes the connection at
once and payloads log to klog: --klog [GREP] prints matching klog lines
(TCP 3232) for --wait seconds after sending instead.
"""

from __future__ import annotations

import argparse
import re
import socket
import sys
import time
from pathlib import Path


def stream(sock: socket.socket, out=None) -> tuple[int | None, bool]:
    """Print what the loader sends until it closes the connection: the
    payload's exit code (None when none was reported) and whether it was
    refused or crashed."""
    out = out or sys.stdout
    code, bad, pending = None, False, b""
    while True:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        pending += chunk
        *lines, pending = pending.split(b"\n")
        for raw in lines:
            line = raw.decode("utf-8", "replace")
            print(line, file=out, flush=True)
            m = re.search(r"\[elfldr\] payload \d+ (?:exited with code|returned|ended, code) (-?\d+)", line)
            if m:
                code = int(m.group(1))
            if re.search(r"\[elfldr\] (?:payload \d+ )?(?:refused|crashed|did not start)", line):
                bad = True
    if pending:
        print(pending.decode("utf-8", "replace"), file=out, flush=True)
    return code, bad


def send(host: str, port: int, data: bytes, half_close: bool = True, timeout: float = 15) -> socket.socket:
    s = socket.create_connection((host, port), timeout=timeout)
    s.sendall(data)
    if half_close:
        s.shutdown(socket.SHUT_WR)
    s.settimeout(1.0)
    return s


def klog(host: str, data: bytes, port: int, grep: str, wait: float) -> int:
    k = socket.create_connection((host, 3232), timeout=3)
    k.settimeout(0.2)
    t = time.time()
    while time.time() - t < 1.5:   # what klog had queued
        try:
            if not k.recv(65536):
                break
        except socket.timeout:
            break
    with socket.create_connection((host, port), timeout=10) as s:
        s.sendall(data)
        s.shutdown(socket.SHUT_WR)
    buf = b""
    t = time.time()
    while time.time() - t < wait:
        try:
            c = k.recv(65536)
            if not c:
                break
            buf += c
        except socket.timeout:
            pass
    for line in buf.decode("utf-8", "replace").splitlines():
        if not grep or any(g in line for g in grep.split("|")):
            print(line[:300])
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("elf", nargs="?", type=Path, help="the payload (ELF, or a fake-signed SELF)")
    ap.add_argument("--host", help="console address (default: looked up, as run.py does)")
    ap.add_argument("--port", type=int, default=9021)
    ap.add_argument("--cmd", choices=["status", "quit", "help"], help="send a command instead of an ELF")
    ap.add_argument("--klog", nargs="?", const="", metavar="GREP",
                    help="a jailbroken console's elfldr: show klog lines (matching GREP, | separated) instead")
    ap.add_argument("--wait", type=float, default=6, help="--klog: seconds of klog to show")
    args = ap.parse_args()
    if not args.elf and not args.cmd:
        ap.error("give a payload or --cmd")
    if args.host is None:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from console import console_ip
        args.host = console_ip()
    data = (args.cmd + "\n").encode() if args.cmd else args.elf.read_bytes()
    if args.klog is not None:
        return klog(args.host, data, args.port, args.klog, args.wait)
    try:
        s = send(args.host, args.port, data)
    except OSError as e:
        print(f"elf_send: {args.host}:{args.port}: {e} (is `run.py elfldr` serving?)", file=sys.stderr)
        return 1
    with s:
        code, bad = stream(s)
    if args.cmd:
        return 0
    return 1 if bad else (code if code is not None and 0 <= code < 256 else 0)


if __name__ == "__main__":
    raise SystemExit(main())
