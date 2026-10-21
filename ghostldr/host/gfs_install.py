#!/usr/bin/env python3
"""Install a title into SWRR save containers over the network (ghostfs).

Needs only Luac0re's Remote Lua Loader on 9026 (no kernel exploit).  Sends
ghostldr/lua/gfs_receiver.lua, which listens on GFS_PORT, then streams each
container laid out by gfs_pack.py straight from the source files.

One title is installed at a time.  The console says what is installed: the
index container GFSINDEX (GHOSTIDX before 2026-10-21) names the title, its
layout and its containers.
  - the same layout, complete: nothing to send (a GHOSTIDX index is upgraded)
  - another title or layout: its data containers and index are deleted first
    (its <PREFIX>SAVE progress container is kept)
  - the same layout, interrupted: containers whose gfs.tag matches are kept
GFSINDEX is written first as a header marked incomplete and replaced by the
full index last, so an interrupted install is recognised and never booted.

    python3 ghostldr/host/gfs_pack.py [--src DIR --title CUSA12345]
    python3 ghostldr/host/gfs_install.py [--title CUSA12345] [--dry-run] [--force]
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from console import console_ip
from gfs_pack import FLAG_INCOMPLETE, OUT_ROOT, index_header

ROOT = Path(__file__).resolve().parents[2]
RECEIVER = ROOT / "ghostldr/lua/gfs_receiver.lua"
# Luac0re creates sockets in the JIT compiler process and passes them over, so a
# receiver's listening socket outlives it (the compiler keeps its copy): each
# run listens on a fresh port, and connections to an old one would hang.
GFS_PORT = 9032 + int(time.time()) % 200
LOG_PORT = 18195
CHUNK = 4 << 20
OP_PUT, OP_QUERY, OP_BYE, OP_DELETE, OP_READ, OP_WRITE = 1, 2, 3, 4, 5, 6
INDEX, LEGACY_INDEX = "GFSINDEX", "GHOSTIDX"
TAG = "gfs.tag"
MAX_CONTAINERS = 96              # ghostfs.c GFS_MAX_CONT
NOT_FOUND = 0x809F0008
SAVE_ERRORS = {0x809F0003: "busy", 0x809F0008: "not found", 0x809F000A: "no space left on the console's storage",
               0x809F000C: "too many mounted", 0x809F000F: "broken"}


def log(msg: str) -> None:
    print(f"{time.strftime('%H:%M:%S')} gfs-install: {msg}", flush=True)


@dataclass
class Install:
    """What an index header says (installed, or about to be)."""
    name: str            # the container holding the index
    version: int
    title: str
    layout_id: int
    prefix: str
    containers: int
    entries: int
    total: int
    incomplete: bool

    def describe(self) -> str:
        what = self.title or "an unnamed title (version 1 index)"
        state = ", incomplete" if self.incomplete else ""
        return f"{what} in {self.containers} containers {self.prefix}000.. ({self.total / 2**30:.1f} GiB{state})"


def parse_header(name: str, data: bytes) -> Install | None:
    if len(data) < 64 or data[:4] != b"GFS1":
        return None
    version, count, entries, _cap, total = struct.unpack_from("<IIIQQ", data, 4)
    prefix = data[40:56].split(b"\0")[0].decode(errors="replace")
    if version >= 2 and len(data) >= 88:
        flags = struct.unpack_from("<I", data, 60)[0]
        title = data[64:80].split(b"\0")[0].decode(errors="replace")
        layout_id = struct.unpack_from("<Q", data, 80)[0]
        return Install(name, version, title, layout_id, prefix, count, entries, total, bool(flags & FLAG_INCOMPLETE))
    return Install(name, version, "", 0, prefix, count, entries, total, False)


def same_data(have: Install, new: Install) -> bool:
    if have.version >= 2:
        return have.layout_id == new.layout_id
    # version 1 had no layout id: the same prefix and shape is the same layout
    return (have.prefix, have.containers, have.entries, have.total) == (new.prefix, new.containers, new.entries,
                                                                       new.total)


def plan(installed: list[Install], new: Install, tagged: set[int], force: bool) -> list[tuple]:
    """Steps to get `new` installed, given the indexes found (GFSINDEX first)
    and the containers whose tag already matches `new`'s layout."""
    current = installed[0] if installed else None
    steps: list[tuple] = []
    for stale in installed[1:]:              # a GHOSTIDX left beside a GFSINDEX
        if stale.prefix in (new.prefix, current.prefix):
            steps.append(("delete", stale.name))
        else:
            steps.append(("remove", stale))
    if current and same_data(current, new) and not force:
        if not current.incomplete:
            if current.name != INDEX or current.version < 2:
                steps.append(("index",))    # the same data under the old index: upgrade it
                steps.append(("delete", current.name))
            return steps
        resume = True                        # the same layout, interrupted
    else:
        resume = False
        if current:
            steps.append(("remove", current))
    steps.append(("stub",))
    steps += [("put", k) for k in range(new.containers) if force or not (resume and k in tagged)]
    steps.append(("sweep",))
    steps.append(("index",))
    return steps


IP_BOUND_IF = 25                 # macOS: send through this interface whatever the routing table says


class Receiver:
    def __init__(self, host: str, interface: str | None = None) -> None:
        self.host = host
        self.interface = interface       # send through this interface instead of the default route
        self.sock: socket.socket | None = None

    def start(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect((self.host, 9))
            local_ip = probe.getsockname()[0]
        lua = (f'GFS_PORT = {GFS_PORT}\nPROBE_LOG_IP = "{local_ip}"\nPROBE_LOG_PORT = {LOG_PORT}\n'
               + RECEIVER.read_text())
        with socket.create_connection((self.host, 9026), timeout=10) as s:
            s.sendall(lua.encode())
            s.shutdown(socket.SHUT_WR)
        log(f"receiver sent to {self.host}:9026")

    def connect(self) -> None:
        for _ in range(40):
            try:
                self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                if self.interface:
                    self.sock.setsockopt(socket.IPPROTO_IP, IP_BOUND_IF, socket.if_nametoindex(self.interface))
                self.sock.settimeout(10)
                self.sock.connect((self.host, GFS_PORT))
                self.sock.settimeout(300)
                log(f"connected to the receiver on {GFS_PORT}")
                return
            except OSError:
                time.sleep(1.5)
        raise SystemExit("receiver did not come up (is SWRR at the Luac0re loader?)")

    def request(self, op: int, dirname: str, fname: str, blocks: int = 0, size: int = 0) -> None:
        assert self.sock is not None
        self.sock.sendall(struct.pack("<4sIQQ32s32s8s", b"GFSI", op, blocks, size, dirname.encode(),
                                      fname.encode(), b""))

    def recv(self, n: int) -> bytes:
        assert self.sock is not None
        data = b""
        while len(data) < n:
            part = self.sock.recv(n - len(data))
            if not part:
                raise ConnectionError("receiver closed the connection")
            data += part
        return data

    def reply(self) -> tuple[int, int, int]:
        magic, status, value, millis = struct.unpack("<4siQQ8x", self.recv(32))
        if magic != b"GFSR":
            raise ConnectionError(f"bad reply {magic!r}")
        return status & 0xFFFFFFFF, value, millis

    def read(self, dirname: str, fname: str, size: int, offset: int = 0) -> bytes | None:
        self.request(OP_READ, dirname, fname, offset, size)
        status, n, _ = self.reply()
        return self.recv(n) if status == 0 else None

    def write(self, dirname: str, fname: str, data: bytes) -> int:
        self.request(OP_WRITE, dirname, fname, 0, len(data))
        self.sock.sendall(data)
        return self.reply()[0]

    def delete(self, dirname: str) -> int:
        self.request(OP_DELETE, dirname, "")
        return self.reply()[0]

    def put(self, dirname: str, fname: str, blocks: int, size: int, chunks) -> tuple[int, int, int]:
        self.request(OP_PUT, dirname, fname, blocks, size)
        try:
            for block in chunks:
                self.sock.sendall(block)
        except (BrokenPipeError, ConnectionResetError):
            pass                         # a failed create drops the connection: its reply says why
        return self.reply()

    def close(self) -> None:
        if self.sock:
            try:
                self.request(OP_BYE, "", "")
                self.reply()
            except OSError:
                pass
            self.sock.close()
            self.sock = None


def container_bytes(src: Path, pieces: list):
    """Yield the container's bytes in CHUNK-sized blocks, straight from the files."""
    pending = bytearray()
    for rel, off, length in pieces:
        with (src / rel).open("rb") as f:
            f.seek(off)
            left = length
            while left:
                data = f.read(min(CHUNK, left))
                if not data:
                    raise IOError(f"{rel} ended early")
                left -= len(data)
                pending += data
                while len(pending) >= CHUNK:
                    yield bytes(pending[:CHUNK])
                    del pending[:CHUNK]
    if pending:
        yield bytes(pending)


def tag_bytes(layout_id: int, k: int, size: int) -> bytes:
    return struct.pack("<QII", layout_id, k, size)


def installed_indexes(rx: Receiver) -> list[Install]:
    found = []
    for name in (INDEX, LEGACY_INDEX):
        head = rx.read(name, "index.bin", 128)
        info = parse_header(name, head) if head else None
        if info:
            found.append(info)
    return found


def survey(rx, new: Install, jobs: list) -> tuple[list[Install], set[int]]:
    """What the console has: its indexes and, when `new` was interrupted,
    the containers already sent (their tags match)."""
    installed = installed_indexes(rx)
    for inst in installed:
        log(f"installed ({inst.name}): {inst.describe()}")
    if not installed:
        log("nothing installed")
    tagged: set[int] = set()
    if installed and same_data(installed[0], new) and installed[0].incomplete:
        for k, job in enumerate(jobs):
            if rx.read(job["dir"], TAG, 16) == tag_bytes(new.layout_id, k, job["size"]):
                tagged.add(k)
        log(f"resuming: {len(tagged)} of {len(jobs)} containers already sent")
    return installed, tagged


def execute(rx, steps: list[tuple], new: Install, layout: dict, index: bytes, src: Path) -> None:
    jobs = layout["containers"]
    puts = [s[1] for s in steps if s[0] == "put"]
    sent, t_start = 0, time.time()
    total = sum(jobs[k]["size"] for k in puts)
    for s in steps:
        if s[0] == "remove":
            old = s[1]
            for k in range(old.containers):
                rc = rx.delete(f"{old.prefix}{k:03d}")
                if rc not in (0, NOT_FOUND):
                    raise SystemExit(f"delete {old.prefix}{k:03d}: {rc:#x}")
            rc = rx.delete(old.name)
            log(f"removed {old.title or old.prefix} ({old.containers} containers, index {old.name}): {rc:#x}")
        elif s[0] == "delete":
            log(f"deleted {s[1]}: {rx.delete(s[1]):#x}")
        elif s[0] == "stub":
            stub = index_header(new.containers, new.entries, new.total, 0, 0, new.prefix, new.title,
                                new.layout_id, FLAG_INCOMPLETE)
            status, _, _ = rx.put(INDEX, "index.bin", layout["index_blocks"], len(stub), [stub])
            if status:
                raise SystemExit(f"{INDEX} stub: {status:#x}")
        elif s[0] == "put":
            k, job = s[1], jobs[s[1]]
            t0 = time.time()
            status, written, millis = rx.put(job["dir"], "data.bin", job["blocks"], job["size"],
                                             container_bytes(src, job["pieces"]))
            if status:
                why = SAVE_ERRORS.get(status, "")
                raise SystemExit(f"{job['dir']}: receiver status {status:#x}{' (' + why + ')' if why else ''} "
                                 f"after {written >> 20} MiB (rerun to resume)")
            rc = rx.write(job["dir"], TAG, tag_bytes(new.layout_id, k, job["size"]))
            if rc:
                raise SystemExit(f"{job['dir']}: tag write {rc:#x}")
            sent += job["size"]
            rate = sent / max(time.time() - t_start, 1e-3)
            log(f"[{puts.index(k) + 1}/{len(puts)}] {job['dir']}: {job['size'] >> 20} MiB in {time.time() - t0:.0f} s "
                f"(console {millis / 1000:.0f} s); avg {rate / 1e6:.1f} MB/s, "
                f"ETA {(total - sent) / max(rate, 1) / 60:.0f} min")
        elif s[0] == "sweep":
            gone = 0
            for k in range(new.containers, MAX_CONTAINERS):
                if rx.delete(f"{new.prefix}{k:03d}") == 0:
                    gone += 1
            if gone:
                log(f"deleted {gone} leftover {new.prefix} containers past {new.prefix}{new.containers - 1:03d}")
        elif s[0] == "index":
            status, _, _ = rx.put(INDEX, "index.bin", layout["index_blocks"], len(index), [index])
            if status:
                raise SystemExit(f"{INDEX}: {status:#x}")
            log(f"{INDEX} written: {new.describe()}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--title", default="CUSA11456", help="title to install (as packed by gfs_pack.py)")
    ap.add_argument("--dry-run", action="store_true", help="only show what is installed and what would change")
    ap.add_argument("--force", action="store_true", help="delete and resend everything")
    ap.add_argument("--host")
    ap.add_argument("--interface", help="this computer's network interface to send through (e.g. en0)")
    ap.add_argument("--port", type=int, help="reattach to a receiver already listening on this port "
                    "(it holds Luac0re's loader, so a new one cannot start)")
    args = ap.parse_args()
    out = OUT_ROOT / args.title
    layout = json.loads((out / "layout.json").read_text())
    index = (out / "index.bin").read_bytes()
    src = Path(layout["src"])
    new = parse_header(INDEX, index)
    assert new and new.title == args.title, "index.bin does not match layout.json: rerun gfs_pack.py"
    jobs = layout["containers"]

    global GFS_PORT
    rx = Receiver(args.host or console_ip(), args.interface)
    if args.port:
        GFS_PORT = args.port
    else:
        rx.start()
    rx.connect()
    installed, tagged = survey(rx, new, jobs)
    steps = plan(installed, new, tagged, args.force)
    log(f"installing {new.describe()}")
    puts = [s[1] for s in steps if s[0] == "put"]
    for s in steps:
        if s[0] == "remove":
            log(f"  remove {s[1].describe()} (its {s[1].prefix}SAVE progress is kept)")
        elif s[0] == "delete":
            log(f"  delete {s[1]}")
    if puts:
        log(f"  send {len(puts)} containers, {sum(jobs[k]['size'] for k in puts) / 2**30:.1f} GiB")
    if ("index",) in steps:
        log(f"  write {INDEX}" + ("" if puts else " (the data is already there)"))
    if not steps:
        log("  nothing to do: already installed")
    if args.dry_run:
        rx.close()
        log("dry run: nothing changed")
        return 0

    execute(rx, steps, new, layout, index, src)
    rx.close()
    log("install complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
