#!/usr/bin/env python3
"""Send the ghostldr bootstrap to the console's remote Lua loader and show logs:
Luac0re's in SWRR (TCP 9026) or UltraC0re's in Jak X (TCP 8888), whichever
answers (--loader-port to choose).

    python3 ghostldr/host/run.py probe            # core probe (flags=1)
    python3 ghostldr/host/run.py probe --diag     # PS4 diagnostic: boots nothing (docs/ps4.md)
    python3 ghostldr/host/run.py boot             # load and start the title
    python3 ghostldr/host/run.py logs             # only listen for UDP logs
    python3 ghostldr/host/run.py doctor           # check this host + console

A boot also serves the runtime extension (build/ghostldr/ext.bin: the NID
table and the extension's HLE code) on TCP 18195; the core fetches it first
and stops before touching the title when it cannot.  The game selector
(build/ghostldr/selector.bin, docs/selector.md) is served on TCP 18196: the
core runs it before the title when more than one title is installed, or with
--select.

--app-root usb:/ghostldr boots from a USB drive's folder of titles
(docs/usb.md): gfs_pack.py --usb puts titles there, and the console mounts
the drive into SWRR's sandbox.  When this computer sees the drive too
(--usb-drive, else the only mounted one with a ghostldr folder), its titles'
profiles and dumps are used as installed ones' are.

While the title runs, the console serves FTP on TCP 1212 (--ftp-port; 0:
none; docs/ftp.md): any FTP client, any user and password.  Clients start on
the USB drive when one is plugged in, so titles can go onto it over the
network while a game is running.

A boot first runs a preflight coverage check when a local app0 and the
NID name table are staged: if imports would bind to stubs (which return
0 and look like success to the title), it asks before booting, unless
--force-preflight is given. The full report lands in
build/ghostldr/preflight.md.
"""

from __future__ import annotations

import argparse
import hashlib
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
SELECTOR = ROOT / "build/ghostldr/selector.bin"
BOOT_LUA = ROOT / "ghostldr/lua/ghostldr.lua"
NOTIFY_HOOK = ROOT / "ghostldr/lua/notify_hook.lua"
from console import LOADERS, console_ip, loader_port
import profiles
import usb_drive
LOG_PORT = 18194
EXT_PORT = LOG_PORT + 1   # TCP
SEL_PORT = LOG_PORT + 2   # TCP
FTP_PORT = 1212           # the console's FTP server (TCP; docs/ftp.md); not ftpsrv's 2121
LUA_MAX = 500 * 1024      # Luac0re's and UltraC0re's remote Lua loaders read at most this much
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


def build_payload(flags: int, log_ip: str, app_root: str, knobs: dict[str, int], ext: bytes = b"",
                  sel: bytes = b"") -> bytes:
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
        f'GHOSTLDR_EXT_SHA256 = "{hashlib.sha256(ext).hexdigest()}"',
        f"GHOSTLDR_SEL_PORT = {SEL_PORT}",
        f"GHOSTLDR_SEL_SIZE = {len(sel)}",
        f"GHOSTLDR_SEL_CRC = {zlib.crc32(sel):#x}",
        *[f"GHOSTLDR_{k.upper()}_MIB = {v}" for k, v in knobs.items()],
        "",
    ])
    payload = (header + BOOT_LUA.read_text()).encode()
    if len(payload) >= LUA_MAX:
        raise SystemExit(f"the Lua payload is {len(payload)} bytes; the loader reads at most {LUA_MAX}")
    return payload


class ExtServer(threading.Thread):
    """Serves ext.bin (or selector.bin) to the core: each connection gets the whole blob."""

    def __init__(self, blob: bytes, port: int = EXT_PORT, label: str = "extension") -> None:
        super().__init__(daemon=True)
        self.blob, self.label = blob, label
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.sock.bind(("0.0.0.0", port))
        except OSError as e:
            raise SystemExit(f"cannot serve the {label} on TCP {port}: {e}")
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
                    print(f"{self.label}: sent {len(self.blob)} bytes to {peer[0]}", flush=True)
                except OSError as e:
                    print(f"{self.label}: sending to {peer[0]} failed: {e}"
                          + (" (the core closes it unread when no choice is needed)" if self.label == "selector" else ""),
                          flush=True)


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


def send_lua(host: str, payload: bytes, port: int) -> None:
    if LOADERS.get(port) == "UltraC0re":
        # its loader reports a payload's errors only on the TV: mirror them to the log first
        hook = (f'GHOSTLDR_LOG_IP = "{local_ip_for(host)}"\nGHOSTLDR_LOG_PORT = {LOG_PORT}\n'
                + NOTIFY_HOOK.read_text()).encode()
        with socket.create_connection((host, port), timeout=10) as s:
            s.sendall(hook)
            s.shutdown(socket.SHUT_WR)
    with socket.create_connection((host, port), timeout=10) as s:
        s.sendall(payload)
        s.shutdown(socket.SHUT_WR)


def title_dump(title: str | None, select: bool,
               drive_titles: dict[str, Path] | None = None) -> tuple[Path | None, str]:
    """The local dump of the title this boot starts, and why that one (or why
    none): --title's, else the only title with a dump here (packed ones and
    build/ghost/app0; with a USB app root, the drive's titles instead).  A
    packed title's dump is where gfs_pack.py read it (its layout.json).
    Without --title the console boots its only installed title, which needn't
    be one of these, so several candidates check none."""
    if select:
        return None, "the selector picks the title on the console"
    t = profiles.normalize_title(title) if title else None
    if drive_titles is not None:
        if not t and len(drive_titles) == 1:
            t = next(iter(drive_titles))
        if t in drive_titles:
            return drive_titles[t], t
        return None, (f"{t} is not on the USB drive here" if t else
                      f"{len(drive_titles)} titles on the USB drive here; pass --title to check one"
                      if drive_titles else "the USB drive is not mounted here")
    if not t:
        here = {f.parent.name for f in profiles.TITLES_OUT.glob("*/layout.json")}
        default = profiles.detect_title(profiles.DEFAULT_SRC) if (profiles.DEFAULT_SRC / "eboot.bin").exists() else None
        if default:
            here.add(default)
        if len(here) != 1:
            return None, (f"dumps of {', '.join(sorted(here))} here; pass --title to check one" if here
                          else "no local dump")
        t = here.pop()
    src = profiles.packed(t).get("src")
    if src:
        return Path(src), t
    if profiles.detect_title(profiles.DEFAULT_SRC) == t:
        return profiles.DEFAULT_SRC, t
    return None, f"no local dump of {t} (not packed here, and build/ghost/app0 is another title)"


def usb_titles(app_root: str, drive: Path | None) -> dict[str, Path] | None:
    """With a USB app root: the titles on the drive as this computer sees it
    ({} when it does not), else None."""
    if not app_root.startswith("usb:"):
        return None
    d = usb_drive.find_drive(drive)
    if not d:
        return {}
    rel = app_root[4:].strip("/")
    lib = d / rel if rel else d
    if (lib / "eboot.bin").is_file():           # one title
        t = profiles.detect_title(lib) or (lib.parent.name if profiles.normalize_title(lib.parent.name) else None)
        return {t: lib} if t else {}
    return {p.name: p / "app0" for p in sorted(lib.glob("*")) if profiles.TITLE_RE.fullmatch(p.name)
            and (p / "app0/eboot.bin").is_file()}


def preflight(force: bool, title: str | None, select: bool, drive_titles: dict[str, Path] | None = None) -> None:
    """Coverage-gate a boot against the booted title's local dump.

    Skips cleanly when there is nothing to check against (no local
    dump, or a release tree with no generated name table): the gate
    must never be the reason a boot fails on a tree that cannot run
    the checker.
    """
    app0, which = title_dump(title, select, drive_titles)
    if app0 is None:
        print(f"preflight: {which}; skipped", flush=True)
        return
    if not (app0 / "eboot.bin").exists():
        print(f"preflight: no dump at {app0} ({which}); skipped", flush=True)
        return
    print(f"preflight: checking {which} at {app0}", flush=True)
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
    if not un or force:
        return
    libs = [row["library"] for row in rep["libraries"]
            if row["classes"].get("UNRESOLVED")]
    print(f"preflight: {un} imports would bind to stubs that return 0 "
          f"(libraries: {', '.join(libs[:5])}); see "
          "build/ghostldr/preflight.md", flush=True)
    if not sys.stdin.isatty():
        raise SystemExit("preflight: no terminal to ask on; re-run with "
                         "--force-preflight to boot anyway")
    try:
        answer = input("Boot anyway? [y/N] ").strip().lower()
    except EOFError:
        answer = ""
    if answer not in ("y", "yes"):
        raise SystemExit("preflight: boot cancelled")


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

    ports = [args.loader_port] if args.loader_port else list(LOADERS)
    for i, port in enumerate(ports):
        try:
            with socket.create_connection((args.host, port), timeout=3):
                pass
            add("PASS", "console loader", f"{args.host}:{port} answers ({LOADERS.get(port, 'a loader')})")
            break
        except OSError as e:
            if i == len(ports) - 1:
                add("FAIL", "console loader",
                    f"nothing at {args.host}:{'/'.join(map(str, ports))} ({e}); SWRR up with "
                    "Luac0re's loader, or Jak X with UltraC0re's?")

    drive_titles = usb_titles(args.app_root, args.usb_drive)
    if drive_titles is not None:
        d = usb_drive.find_drive(args.usb_drive)
        add("PASS" if drive_titles else "WARN", "USB drive",
            f"{d}: {', '.join(drive_titles) or 'no titles'}" if d else
            "not mounted here (only needed for the preflight and profiles; the console reads the drive)")
    app0, which = title_dump(args.title, args.select, drive_titles)
    if app0 is None:
        add("WARN", "title dump", which)
    elif not (app0 / "eboot.bin").exists():
        add("WARN", "title dump", f"nothing staged at {app0} ({which})")
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
                    f"{which}: {sum(by.values())} functions, {un} unresolved")

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
    parser.add_argument("--title", help="title to boot: one of the installed ones (ghostfs; default: the only one "
                        "installed) or the dump at --app-root (default: found by its eboot fingerprint)")
    parser.add_argument("--trace-opens", action="store_true",
                        help="log every file the title opens (capture_order.py turns run.log into an open order)")
    parser.add_argument("--dialog-cancel", action="store_true",
                        help="answer every system dialog with buttons by cancelling (to bisect a title hang)")
    parser.add_argument("--ime-fixed-name", action="store_true",
                        help="text input answers \"Player\" instead of the console user's name")
    memory = parser.add_mutually_exclusive_group()
    memory.add_argument("--no-auto-memory", action="store_true",
                        help="a title without a memory plan gets the plain plan-free budget: no lazy commit, no "
                             "learned plan (docs/compatibility.md#memory-learned-at-run-time)")
    memory.add_argument("--auto-memory", action="store_true",
                        help="use the automatic memory plan even when the title's profile has a manual one")
    parser.add_argument("--forget-memory", action="store_true",
                        help="start the title's learned memory plan over (it is kept next to its saves)")
    parser.add_argument("--keep-remote-play", action="store_true",
                        help="dev: ignore the title's sceRemoteplayProhibit, so a Remote Play viewer keeps its picture")
    parser.add_argument("--select", action="store_true",
                        help="show the game selector even with one title installed or --title given "
                             "(docs/selector.md); every profile is sent and --title is not used")
    parser.add_argument("--select-font", choices=["auto", "builtin"], default="builtin",
                        help="selector text: the built-in Noto Sans (default), or try the console's system "
                             "font first (auto; experimental: it crashed SWRR on a 13.60 PS5)")
    parser.add_argument("--no-selector", action="store_true", help="do not serve the game selector")
    parser.add_argument("--diag", action="store_true",
                        help="with probe: log what the loader needs to know about this console (PS4 support, "
                             "docs/ps4.md); boots and releases nothing")
    parser.add_argument("--boot-start-modules", action="store_true",
                        help="start IsGame modules (NpToolkit2, ...) at boot as before, not at the title's "
                             "sceSysmoduleLoadModule")
    parser.add_argument("--host", default=None, help="console address (default: looked up by host-id)")
    parser.add_argument("--wait", type=float, default=None,
                        help="seconds to keep printing the console's log (default: boot keeps listening until "
                             "Ctrl+C; probe and logs stop after 20 s)")
    parser.add_argument("--app-root", default="ghostfs",
                        help="where the title's app0 is: ghostfs (the installed titles, the default), "
                        f"{usb_drive.APP_ROOT} (the titles on a USB drive, docs/usb.md), or a directory on a dev "
                        "console, e.g. /data/ghost/app0 (a folder of <TITLE>/app0 folders works too)")
    parser.add_argument("--usb-drive", type=Path,
                        help="where this computer has the USB drive mounted, for the preflight check and the "
                        "titles' profiles (default: the only mounted drive with a ghostldr folder)")
    parser.add_argument("--ftp-port", type=int, default=FTP_PORT,
                        help=f"the console's FTP server while the title runs (default {FTP_PORT}; 0: none; "
                             "docs/ftp.md)")
    parser.add_argument("--file", type=Path, help="raw Lua file for mode=lua")
    parser.add_argument("--loader-port", type=int, default=0,
                        help="the console's Lua loader: 9026 Luac0re (SWRR), 8888 UltraC0re (Jak X); "
                             "default: whichever answers")
    parser.add_argument("--log", type=Path, default=ROOT / "build/ghostldr/run.log")
    args = parser.parse_args()
    if not 0 <= args.ftp_port <= 65535:
        raise SystemExit(f"--ftp-port {args.ftp_port}: a TCP port, or 0 for no FTP server")
    if args.diag and args.mode != "probe":
        raise SystemExit("--diag goes with probe: run.py probe --diag")
    if args.app_root.startswith("usb:") and not args.app_root.startswith("usb:/"):
        raise SystemExit(f"--app-root {args.app_root}: a USB path starts at the drive's root, e.g. "
                         f"{usb_drive.APP_ROOT}")
    args.host = args.host or console_ip()
    if args.mode == "doctor":
        return doctor(args)

    recorder = recorder_running()
    listener = None if recorder else LogListener(args.log)
    if listener:
        listener.start()
    else:
        print(f"recorder (pid {recorder}) is logging to build/ghostldr/run.log", flush=True)
    drive_titles = usb_titles(args.app_root, args.usb_drive)
    if args.mode in ("probe", "boot"):
        if args.mode == "boot":
            preflight(args.force_preflight, args.title, args.select, drive_titles)
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
            | (64 if args.ext_jit else 0) | (128 if args.dialog_cancel else 0) | (256 if args.ime_fixed_name else 0) \
            | (512 if args.keep_remote_play else 0) | (1024 if args.boot_start_modules else 0) \
            | (2048 if args.select else 0) | (4096 if args.select_font == "builtin" else 0) \
            | (16384 if args.diag else 0) | (65536 if args.no_auto_memory else 0) \
            | (131072 if args.auto_memory else 0) | (524288 if args.forget_memory else 0) \
            | (args.ftp_port << 48 if args.mode == "boot" else 0)
        knobs = {"gpu_heap": args.gpu_heap_mib, "slack": args.slack_mib, "pool1": args.pool1_mib,
                 "cpu_heap": args.cpu_heap_mib}
        ext = EXT.read_bytes() if args.mode == "boot" else b""
        if ext:
            ext_server = ExtServer(ext)
            ext_server.start()
            print(f"serving the extension ({len(ext)} bytes) on TCP {EXT_PORT}", flush=True)
        sel = SELECTOR.read_bytes() if args.mode == "boot" and not args.no_selector and SELECTOR.exists() else b""
        if args.select and not sel:
            raise SystemExit(f"--select needs {SELECTOR.relative_to(ROOT)} (make -C ghostldr/native)")
        if sel:
            ExtServer(sel, SEL_PORT, "selector").start()
            print(f"serving the game selector ({len(sel)} bytes) on TCP {SEL_PORT}", flush=True)
        payload = build_payload(flags, local_ip_for(args.host), args.app_root, knobs, ext, sel)
        if args.select and args.title:   # every profile, so whichever title is picked gets its own
            print(f"--select: the selector picks the title (--title {args.title} is not used)", flush=True)
        try:   # the title profiles (host/profiles.py; read by native/src/profile.c)
            bundle = profiles.boot_bundle(None if args.select else args.title,
                                          ("trace_opens",) if args.trace_opens else (), drive_titles)
        except profiles.ProfileError as e:
            raise SystemExit(str(e))
        payload = f'GHOSTLDR_PROFILE_HEX = "{bundle.hex()}"\n'.encode() + payload
        port = args.loader_port or loader_port(args.host)
        print(f"sending {len(payload)} bytes of Lua to {args.host}:{port}", flush=True)
        send_lua(args.host, payload, port)
        if args.mode == "boot" and args.ftp_port:
            print(f"FTP: ftp://{args.host}:{args.ftp_port}/ once the title starts (any user and password)",
                  flush=True)
        if ext and not ext_server.sent.wait(60):
            print(f"extension: the console did not fetch it (is TCP {EXT_PORT} open in this computer's firewall?)",
                  flush=True)
    elif args.mode == "lua":
        send_lua(args.host, args.file.read_bytes(), args.loader_port or loader_port(args.host))
    if listener:
        wait = args.wait if args.wait is not None else (None if args.mode == "boot" else 90.0 if args.diag else 20.0)
        if wait is None:
            print("listening for the console's log; press Ctrl+C to stop (the game keeps running)", flush=True)
        try:
            if wait is None:
                while True:
                    time.sleep(3600)
            time.sleep(wait)
        except KeyboardInterrupt:
            pass
        listener.stop.set()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
