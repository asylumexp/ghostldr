#!/usr/bin/env python3
"""Crash recorder for ghostldr sessions: the console's kernel log, kept.

Start it once and leave it running across boots and relaunches:

    python3 ghostldr/host/recorder.py

While it runs it
  - follows the console's kernel log (klogsrv, TCP 3232) into
    build/ghostldr/klog.log, reconnecting whenever the console comes back,
  - receives the core's UDP log (port 18194) into build/ghostldr/run.log
    (run.py then leaves the port to it).

When something goes wrong it saves a bundle to
build/ghostldr/crashes/<time>-<reason>/ with the kernel log and core log
leading up to the event (and ~20 s after it) plus event.json.  Triggers:
  crash        the core's signal handler logged a CRASH report
  log-silent   the core's watchdog stopped logging mid-session (hang)
  offline      the console stopped answering discovery (power-off, panic)
  klog         the kernel log reported a panic, fatal trap or core dump
A script that relaunches SWRR or restarts the console on purpose can write
build/ghostldr/restart_marker first, so that is not reported.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import console  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/ghostldr"
RUN_LOG = BUILD / "run.log"
KLOG = BUILD / "klog.log"
CRASHES = BUILD / "crashes"
PID_FILE = BUILD / "recorder.pid"
MARKER = BUILD / "restart_marker"   # written before deliberate relaunches
LOG_PORT = 18194

# "is_coredumpFinished={0}" is in every app close (SceLncService BlockingKill): not a crash
KLOG_TRIGGERS = re.compile(r"(?i)\bpanic\b|fatal trap|core ?dump(?!finished)")
SILENT_SECONDS = 150    # the --snap watchdog logs every ~25 s
OFFLINE_CHECKS = 3      # 10 s apart
AFTERMATH_SECONDS = 20  # keep logging this long after an event before saving
MARKER_GRACE = {"swrr": 600, "console": 300}

STOP = threading.Event()
PRINT_LOCK = threading.Lock()


def say(msg: str) -> None:
    with PRINT_LOCK:
        print(f"{time.strftime('%H:%M:%S')} recorder: {msg}", flush=True)


def notify(msg: str) -> None:
    subprocess.run(["osascript", "-e", f'display notification "{msg}" with title "Ghost crash recorder"'],
                   capture_output=True)


def tail(path: Path, lines: int) -> str:
    if not path.exists():
        return ""
    with path.open("rb") as f:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        f.seek(max(0, size - lines * 220))
        return b"\n".join(f.read().splitlines()[-lines:]).decode("utf-8", "replace") + "\n"


def marker() -> tuple[str, float]:
    """(kind, time) of the last deliberate relaunch/restart, or ("", 0)."""
    try:
        kind, stamp = MARKER.read_text().split()
        return kind, float(stamp)
    except (OSError, ValueError):
        return "", 0.0


def in_grace(kind: str) -> bool:
    k, t = marker()
    return k == kind and time.time() - t < MARKER_GRACE[kind]


# ---- event handling ----

class Events:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.busy = False
        self.last_event = 0.0

    def trigger(self, reason: str, detail: str) -> None:
        with self.lock:
            if self.busy or time.time() - self.last_event < 300:
                return
            self.busy = True
        threading.Thread(target=self._bundle, args=(reason, detail), daemon=True).start()

    def _bundle(self, reason: str, detail: str) -> None:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        out = CRASHES / f"{stamp}-{reason}"
        out.mkdir(parents=True, exist_ok=True)
        say(f"EVENT {reason}: {detail.strip()[:200]} -> {out.relative_to(ROOT)}")
        notify(f"{reason}: saving the logs")
        STOP.wait(AFTERMATH_SECONDS)
        (out / "klog.log").write_text(tail(KLOG, 20000))
        (out / "run.log").write_text(tail(RUN_LOG, 8000))
        (out / "event.json").write_text(json.dumps({
            "reason": reason, "detail": detail, "time": stamp,
            "console": console.CACHE.read_text().strip() if console.CACHE.exists() else None,
            "marker": marker(),
        }, indent=2) + "\n")
        say(f"saved {out.relative_to(ROOT)}")
        notify(f"{reason}: saved {out.name}")
        with self.lock:
            self.busy = False
            self.last_event = time.time()


# ---- log sources ----

class Session:
    """What the core's log says about the current Ghost session."""

    def __init__(self) -> None:
        self.started = 0.0
        self.last_line = 0.0
        self.watchdog = False
        self.crash: list[str] = []


def udp_logs(session: Session, events: Events) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", LOG_PORT))
    sock.settimeout(0.5)
    with RUN_LOG.open("a") as log:
        while not STOP.is_set():
            try:
                data, _ = sock.recvfrom(65535)
            except socket.timeout:
                if session.crash and time.time() - session.last_line > 3:
                    events.trigger("crash", "\n".join(session.crash))
                    session.crash = []
                continue
            now = time.time()
            for line in data.decode("utf-8", "replace").splitlines():
                log.write(f"{time.strftime('%H:%M:%S')} {line}\n")
                session.last_line = now
                if "ghostldr core at" in line:
                    session.started, session.watchdog, session.crash = now, False, []
                elif "watchdog:" in line:
                    session.watchdog = True
                if "CRASH" in line:
                    session.crash.append(line)
                    if "CRASH report end" in line:
                        events.trigger("crash", "\n".join(session.crash))
                        session.crash = []
            log.flush()


def klog_follow(events: Events) -> None:
    while not STOP.is_set():
        try:
            ip = console.console_ip()
            with socket.create_connection((ip, 3232), timeout=5) as s, KLOG.open("a") as log:
                s.settimeout(1)
                # a console that dies sends no FIN: let keepalives end the connection
                s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
                for opt, val in (("TCP_KEEPALIVE", 10), ("TCP_KEEPINTVL", 5), ("TCP_KEEPCNT", 3)):
                    if hasattr(socket, opt):
                        s.setsockopt(socket.IPPROTO_TCP, getattr(socket, opt), val)
                connected = time.time()
                log.write(f"--- klog connected to {ip} at {time.strftime('%Y-%m-%d %H:%M:%S')} ---\n")
                buf = b""
                while not STOP.is_set():
                    try:
                        data = s.recv(65536)
                    except socket.timeout:
                        continue
                    if not data:
                        break
                    buf += data
                    *lines, buf = buf.split(b"\n")
                    for raw in lines:
                        line = raw.decode("utf-8", "replace")
                        log.write(f"{time.strftime('%H:%M:%S')} {line}\n")
                        # the first seconds replay klogsrv's backlog: no triggers there
                        if time.time() - connected < 3:
                            continue
                        if "CRASH sig" in line:            # the core's report, if UDP lost it
                            events.trigger("crash", line)
                        elif KLOG_TRIGGERS.search(line) and "[ghostldr" not in line:
                            events.trigger("klog", line)
                    log.flush()
        except (OSError, SystemExit):
            pass
        STOP.wait(10)


def health(session: Session, events: Events) -> None:
    misses = 0
    while not STOP.is_set():
        STOP.wait(10)
        cached = console.CACHE.read_text().strip() if console.CACHE.exists() else ""
        up = bool(cached) and console._ask(cached)
        if not up:
            try:
                up = bool(console.console_ip())
            except SystemExit:
                up = False
        misses = 0 if up else misses + 1
        if misses == OFFLINE_CHECKS and not in_grace("console"):
            events.trigger("offline", f"console stopped answering discovery ({cached or 'unknown address'})")
        silent = time.time() - session.last_line
        if (session.started and session.watchdog and silent > SILENT_SECONDS
                and marker()[1] < session.started):
            events.trigger("log-silent", f"no core log for {int(silent)} s (session started "
                           f"{time.strftime('%H:%M:%S', time.localtime(session.started))})")
            session.watchdog = False   # once per session


def main() -> int:
    argparse.ArgumentParser(description=__doc__.split("\n\n")[0]).parse_args()

    BUILD.mkdir(parents=True, exist_ok=True)
    if PID_FILE.exists():
        try:
            os.kill(int(PID_FILE.read_text()), 0)
            raise SystemExit(f"recorder already running (pid {PID_FILE.read_text().strip()})")
        except (ProcessLookupError, ValueError):
            pass
    PID_FILE.write_text(str(os.getpid()))
    signal.signal(signal.SIGTERM, lambda *_: STOP.set())

    events = Events()
    session = Session()
    for target, targs in ((udp_logs, (session, events)), (klog_follow, (events,)), (health, (session, events))):
        threading.Thread(target=target, args=targs, daemon=True).start()
    say(f"running: {KLOG.relative_to(ROOT)} and {RUN_LOG.relative_to(ROOT)}; bundles -> {CRASHES.relative_to(ROOT)}")
    try:
        while not STOP.is_set():
            STOP.wait(1)
    except KeyboardInterrupt:
        STOP.set()
    PID_FILE.unlink(missing_ok=True)
    say("stopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
