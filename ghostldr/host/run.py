#!/usr/bin/env python3
"""Send the ghostldr bootstrap to Luac0re's remote Lua loader and show logs.

    python3 ghostldr/host/run.py probe            # core probe (flags=1)
    python3 ghostldr/host/run.py boot             # load and start the title
    python3 ghostldr/host/run.py logs             # only listen for UDP logs
    python3 ghostldr/host/run.py doctor           # check this host + console

A boot also serves the runtime extension (build/ghostldr/ext.bin: the NID
table and the extension's HLE code) on TCP 18195; the core fetches it first
and stops before touching the title when it cannot.

A boot first runs a preflight coverage check when a local app0 and the
NID name table are staged: imports that would bind to stubs (which
return 0 and look like success to the title) refuse the boot unless
--force-preflight is given. The full report lands in
build/ghostldr/preflight.md.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "build/ghostldr/core.bin"
LAYOUT = ROOT / "build/ghostldr/core.json"
EXT = ROOT / "build/ghostldr/ext.bin"
BOOT_LUA = ROOT / "ghostldr/lua/ghostldr.lua"
from console import console_ip
LOG_PORT = 18194
EXT_PORT = LOG_PORT + 1   # TCP
LUA_MAX = 500 * 1024      # Luac0re's remote Lua loader reads at most this much
NAMES = ROOT / "build/nid/known_names.txt"

# Plain-language causes for the core's extension failure lines
# (native/src/ext.c). A boot that dies in ext_fetch/ext_start logs one
# of these and nothing else; translate it where the user is looking.
HINTS = [
    ("no extension from the computer",
     "the core could not fetch ext.bin: allow TCP 18195 in this computer's "
     "firewall and keep run.py serving while the console boots"),
    ("CRC-32",
     "the extension arrived corrupted or mismatched: core and ext.bin must "
     "come from the same build — rebuild or re-extract both, then boot again"),
    ("built for API",
     "core and extension are from different builds: rebuild both "
     "(make -C ghostldr/native) and re-send"),
    ("not an extension blob",
     "what answered on TCP 18195 is not an extension blob: check for a "
     "stale server holding the port"),
    ("receive failed",
     "the extension transfer broke mid-way: check the network path to "
     "the console, then boot again"),
    ("implausible size",
     "the announced extension size is wrong: the Lua payload and ext.bin "
     "are from different builds"),
    ("no memory for",
     "the console could not allocate memory for the extension: free memory "
     "on the console (boot without --keep-host-memory)"),
    ("bad NID table",
     "the extension blob is malformed: rebuild it from the same tree as "
     "the core"),
    ("bad image layout",
     "the extension blob is malformed: rebuild it from the same tree as "
     "the core"),
]


def hint_for(line: str) -> str | None:
    for needle, hint in HINTS:
        if needle in line:
            return hint
    return None


def local_ip_for(host: str) -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((host, 9))
        return s.getsockname()[0]


def build_core() -> None:
    try:
        subprocess.run(["make", "-s", "-C", str(ROOT / "ghostldr/native")], check=True)
    except (OSError, subprocess.CalledProcessError) as e:
        hint = " (a release's prebuilt core is there: add --no-build)" if CORE.exists() and EXT.exists() else ""
        raise SystemExit(f"building the core failed: {e}{hint}")


def build_payload(flags: int, log_ip: str, app_root: str, knobs: dict[str, int], ext: bytes = b"") -> bytes:
    core = CORE.read_bytes()
    layout = json.loads(LAYOUT.read_text())
    text_size = (layout["__text_end"] + 0x3FFF) & ~0x3FFF
    if layout["__data_start"] != text_size:
        raise SystemExit(f"core data must start at the text page boundary ({text_size:#x})")
    header = "\n".join([
        f'GHOSTLDR_CORE_HEX = "{core.hex()}"',
        f"GHOSTLDR_TEXT_SIZE = {text_size:#x}",
        f"GHOSTLDR_IMAGE_SIZE = {layout['__image_end']:#x}",
        f'GHOSTLDR_LOG_IP = "{log_ip}"',
        f"GHOSTLDR_LOG_PORT = {LOG_PORT}",
        f"GHOSTLDR_FLAGS = {flags}",
        f'GHOSTLDR_APP_ROOT = "{app_root}"',
        f"GHOSTLDR_EXT_PORT = {EXT_PORT}",
        f"GHOSTLDR_EXT_SIZE = {len(ext)}",
        f"GHOSTLDR_EXT_CRC = {zlib.crc32(ext):#x}",
        *[f"GHOSTLDR_{k.upper()}_MIB = {v}" for k, v in knobs.items()],
        "",
    ])
    payload = (header + BOOT_LUA.read_text()).encode()
    if len(payload) >= LUA_MAX:
        raise SystemExit(f"the Lua payload is {len(payload)} bytes; Luac0re reads at most {LUA_MAX}")
    return payload


class ExtServer(threading.Thread):
    """Serves ext.bin to the core: each connection gets the whole blob."""

    def __init__(self, blob: bytes) -> None:
        super().__init__(daemon=True)
        self.blob = blob
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.sock.bind(("0.0.0.0", EXT_PORT))
        except OSError as e:
            raise SystemExit(f"cannot serve the extension on TCP {EXT_PORT}: {e}")
        self.sock.listen(4)
        self.sock.settimeout(0.5)
        self.stop = threading.Event()
        self.sent = threading.Event()

    def run(self) -> None:
        while not self.stop.is_set():
            try:
                conn, peer = self.sock.accept()
            except socket.timeout:
                continue
            with conn:
                conn.settimeout(30)
                try:
                    conn.sendall(self.blob)
                    self.sent.set()
                    print(f"extension: sent {len(self.blob)} bytes to {peer[0]}", flush=True)
                except OSError as e:
                    print(f"extension: sending to {peer[0]} failed: {e}", flush=True)


class LogListener(threading.Thread):
    def __init__(self, logfile: Path | None) -> None:
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", LOG_PORT))
        self.sock.settimeout(0.5)
        self.lines: list[str] = []
        self.stop = threading.Event()
        self.logfile = logfile.open("a") if logfile else None

    def run(self) -> None:
        while not self.stop.is_set():
            try:
                data, _ = self.sock.recvfrom(65535)
            except socket.timeout:
                continue
            text = data.decode("utf-8", "replace")
            for line in text.splitlines():
                stamped = f"{time.strftime('%H:%M:%S')} {line}"
                print(stamped, flush=True)
                self.lines.append(line)
                if self.logfile:
                    self.logfile.write(stamped + "\n")
                    self.logfile.flush()
                hint = hint_for(line)
                if hint:
                    msg = f"  [hint] {hint}"
                    print(msg, flush=True)
                    if self.logfile:
                        self.logfile.write(msg + "\n")
                        self.logfile.flush()


def recorder_running() -> int:
    """pid of a running recorder.py (it owns the log port), or 0."""
    try:
        pid = int((ROOT / "build/ghostldr/recorder.pid").read_text())
        os.kill(pid, 0)
        return pid
    except (OSError, ValueError):
        return 0


def send_lua(host: str, payload: bytes, port: int = 9026) -> None:
    with socket.create_connection((host, port), timeout=10) as s:
        s.sendall(payload)
        s.shutdown(socket.SHUT_WR)


def preflight(force: bool) -> None:
    """Coverage-gate a boot against a staged local app0.

    Skips cleanly when there is nothing to check against (no local
    app0, or a release tree with no generated name table): the gate
    must never be the reason a boot fails on a tree that cannot run
    the checker.
    """
    app0 = ROOT / "build/ghost/app0"
    if not (app0 / "eboot.bin").exists():
        print("preflight: no local app0 at build/ghost/app0; skipped", flush=True)
        return
    if not NAMES.exists():
        print("preflight: build/nid/known_names.txt missing (no build in "
              "this tree); skipped", flush=True)
        return
    out_json = ROOT / "build/ghostldr/preflight.json"
    out_md = ROOT / "build/ghostldr/preflight.md"
    tool = ROOT / "ghostldr/tools/import_report.py"
    r = subprocess.run([sys.executable, str(tool), str(app0),
                        "--json", str(out_json), "--md", str(out_md)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        tail = (r.stderr.strip().splitlines() or ["?"])[-1]
        print(f"preflight: coverage tool failed ({tail}); continuing "
              "without the gate", flush=True)
        return
    rep = json.loads(out_json.read_text())
    by = rep["summary"]["functions_by_class"]
    un = by.get("UNRESOLVED", 0)
    print(f"preflight: {rep['summary']['functions']} functions - "
          f"{by.get('CORE-HLE', 0)} core-HLE, {by.get('EXT-HLE', 0)} ext-HLE, "
          f"{by.get('TITLE', 0)} title, {by.get('HOST', 0)} host, "
          f"{un} unresolved (report: build/ghostldr/preflight.md)", flush=True)
    if un and not force:
        libs = [row["library"] for row in rep["libraries"]
                if row["classes"].get("UNRESOLVED")]
        raise SystemExit(
            f"preflight: {un} imports would bind to stubs that return 0 "
            f"(libraries: {', '.join(libs[:5])}). See "
            "build/ghostldr/preflight.md, or re-run with "
            "--force-preflight to boot anyway")


def doctor(args) -> int:
    """Host-side checks for the whole boot chain; exit 1 on any FAIL."""
    rows: list[tuple[str, str, str]] = []

    def add(status: str, label: str, detail: str = "") -> None:
        rows.append((status, label, detail))

    v = sys.version_info
    add("PASS" if v >= (3, 11) else "FAIL",
        f"python {v.major}.{v.minor}.{v.micro}",
        "" if v >= (3, 11) else "the release needs Python 3.11+")

    missing = [p.name for p in (CORE, LAYOUT, EXT) if not p.exists()]
    if missing:
        add("FAIL", "prebuilt core",
            f"missing {', '.join(missing)} in build/ghostldr")
    else:
        layout = json.loads(LAYOUT.read_text())
        size = CORE.stat().st_size
        add("PASS" if size == layout["bin_size"] else "FAIL",
            "core.bin integrity",
            f"{size} bytes, core.json bin_size {layout['bin_size']}")
        knobs = {"gpu_heap": 0, "slack": 0, "pool1": 0, "cpu_heap": 0}
        try:
            payload = build_payload(0, "127.0.0.1", args.app_root, knobs,
                                    EXT.read_bytes())
            add("PASS", "lua payload", f"{len(payload)} of {LUA_MAX} bytes")
        except SystemExit as e:
            add("FAIL", "lua payload", str(e))

    rec = recorder_running()
    if rec:
        add("PASS", f"UDP {LOG_PORT} (log)", f"owned by recorder pid {rec}")
    else:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.bind(("0.0.0.0", LOG_PORT))
            add("PASS", f"UDP {LOG_PORT} (log)", "free")
        except OSError as e:
            add("FAIL", f"UDP {LOG_PORT} (log)", f"cannot bind: {e}")
        finally:
            s.close()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("0.0.0.0", EXT_PORT))
        add("PASS", f"TCP {EXT_PORT} (extension)", "free")
    except OSError as e:
        add("FAIL", f"TCP {EXT_PORT} (extension)", f"cannot bind: {e}")
    finally:
        s.close()

    try:
        with socket.create_connection((args.host, 9026), timeout=3):
            pass
        add("PASS", "console loader", f"{args.host}:9026 answers")
    except OSError as e:
        add("FAIL", "console loader",
            f"nothing at {args.host}:9026 ({e}); SWRR up with "
            "Luac0re's loader?")

    app0 = ROOT / "build/ghost/app0"
    if not (app0 / "eboot.bin").exists():
        add("WARN", "title dump", "nothing staged at build/ghost/app0")
    elif not NAMES.exists():
        add("WARN", "title dump",
            "staged, but no name table (build/nid/known_names.txt) "
            "for a coverage check")
    else:
        with tempfile.TemporaryDirectory() as td:
            out = Path(td) / "rep.json"
            r = subprocess.run(
                [sys.executable,
                 str(ROOT / "ghostldr/tools/import_report.py"),
                 str(app0), "--json", str(out)],
                capture_output=True, text=True)
            if r.returncode != 0:
                add("WARN", "title dump",
                    "coverage tool failed on the staged dump")
            else:
                by = json.loads(out.read_text())["summary"]["functions_by_class"]
                un = by.get("UNRESOLVED", 0)
                add("WARN" if un else "PASS", "title dump",
                    f"{sum(by.values())} functions, {un} unresolved")

    for status, label, detail in rows:
        print(f"{status:<4}  {label:<22} {detail}", flush=True)
    return 1 if any(s == "FAIL" for s, _, _ in rows) else 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["probe", "boot", "logs", "lua", "doctor"])
    parser.add_argument("--no-build", action="store_true",
                        help="use the prebuilt core in build/ghostldr (from a release) instead of running make")
    parser.add_argument("--keep-host-memory", action="store_true",
                        help="boot without freeing SWRR's emulator direct-memory buffers first")
    parser.add_argument("--snap", action="store_true",
                        help="log every title thread's location every 20 s (watchdog)")
    parser.add_argument("--count-imports", action="store_true",
                        help="count calls to the title's imports; the watchdog prints the busiest (implies --snap)")
    parser.add_argument("--profile", action="store_true",
                        help="sample the title's main thread every 20 s and print its hottest stacks")
    parser.add_argument("--gpu-heap-mib", type=int, default=0,
                        help="size of the title's general GPU heap (default: its own 1390 MiB); "
                             "the streaming pool gets what this frees")
    parser.add_argument("--slack-mib", type=int, default=0, help="free memory left after the plan (default 128)")
    parser.add_argument("--pool1-mib", type=int, default=0, help="type-0 engine heap at 0x1100000000 (default 332)")
    parser.add_argument("--cpu-heap-mib", type=int, default=0, help="CPU malloc heap at 0x1000000000 (default 384)")
    parser.add_argument("--force-title-ready", action="store_true",
                        help="experiment: set the title world's ready flag from the watchdog")
    parser.add_argument("--ext-jit", action="store_true",
                        help="test: put the extension's code in a new JIT block instead of the core's RX area")
    parser.add_argument("--force-preflight", action="store_true",
                        help="boot even if the preflight coverage check finds unresolved imports")
    parser.add_argument("--host", default=None, help="console address (default: looked up by host-id)")
    parser.add_argument("--wait", type=float, default=20.0)
    parser.add_argument("--app-root", default="/data/ghost/app0")
    parser.add_argument("--file", type=Path, help="raw Lua file for mode=lua")
    parser.add_argument("--log", type=Path, default=ROOT / "build/ghostldr/run.log")
    args = parser.parse_args()
    args.host = args.host or console_ip()
    if args.mode == "doctor":
        return doctor(args)

    recorder = recorder_running()
    listener = None if recorder else LogListener(args.log)
    if listener:
        listener.start()
    else:
        print(f"recorder (pid {recorder}) is logging to build/ghostldr/run.log", flush=True)
    if args.mode in ("probe", "boot"):
        if args.mode == "boot":
            preflight(args.force_preflight)
        if args.no_build:
            needed = (CORE, LAYOUT, EXT) if args.mode == "boot" else (CORE, LAYOUT)
            missing = [p.relative_to(ROOT) for p in needed if not p.exists()]
            if missing:
                raise SystemExit(f"--no-build needs {', '.join(map(str, missing))}")
        else:
            build_core()
        release = args.mode == "boot" and not args.keep_host_memory
        flags = (1 if args.mode == "probe" else 0) | (2 if release else 0) | (4 if args.snap or args.count_imports or args.profile else 0) \
            | (16 if args.profile else 0) \
            | (8 if args.count_imports else 0) | (32 if args.force_title_ready else 0) \
            | (64 if args.ext_jit else 0)
        knobs = {"gpu_heap": args.gpu_heap_mib, "slack": args.slack_mib, "pool1": args.pool1_mib,
                 "cpu_heap": args.cpu_heap_mib}
        ext = EXT.read_bytes() if args.mode == "boot" else b""
        if ext:
            ext_server = ExtServer(ext)
            ext_server.start()
            print(f"serving the extension ({len(ext)} bytes) on TCP {EXT_PORT}", flush=True)
        payload = build_payload(flags, local_ip_for(args.host), args.app_root, knobs, ext)
        print(f"sending {len(payload)} bytes of Lua to {args.host}:9026", flush=True)
        send_lua(args.host, payload)
        if ext and not ext_server.sent.wait(60):
            print(f"extension: the console did not fetch it (is TCP {EXT_PORT} open in this computer's firewall?)",
                  flush=True)
    elif args.mode == "lua":
        send_lua(args.host, args.file.read_bytes())
    if listener:
        try:
            time.sleep(args.wait)
        except KeyboardInterrupt:
            pass
        listener.stop.set()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
