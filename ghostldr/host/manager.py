#!/usr/bin/env python3
"""Start the ghostldr web manager on the PS5 and show where to open it.

Sends ghostldr/lua/manager.lua to Luac0re's Remote Lua Loader (9026).  The
manager is a small web server inside SWRR (default port 8090): a browser on
the same network lists the installed titles and their saves, downloads and
restores saves, and deletes them.  Writes need the session token the PS5
shows when the manager starts; this script prints it too.

    python3 ghostldr/host/manager.py               # start, print URL + token, wait (Ctrl-C stops it)
    python3 ghostldr/host/manager.py --detach      # start and return; stop it from the page
    python3 ghostldr/host/manager.py --exit        # stop a running manager

While the manager runs, Luac0re's loader takes no other script (gfs_install,
run.py ...): stop it first, with the page's Exit button, Ctrl-C here or --exit.
"""

from __future__ import annotations

import argparse
import json
import re
import socket
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from console import console_ip

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "ghostldr/lua/manager.lua"
TITLES = ROOT / "ghostldr/data/titles"
LOG_PORT = 18196
PORTS = 10                     # the manager takes the first free port from --port up


def log(msg: str) -> None:
    print(f"{time.strftime('%H:%M:%S')} manager: {msg}", flush=True)


def api(host: str, port: int, method: str, path: str, timeout: float = 3.0) -> dict | None:
    req = urllib.request.Request(f"http://{host}:{port}{path}", method=method, data=b"" if method == "POST" else None)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read() or b"{}")
    except (OSError, ValueError, urllib.error.URLError):
        return None


def running_port(host: str, first: int) -> int | None:
    """The port a manager already answers on, if one does."""
    for port in range(first, first + PORTS):
        try:
            with socket.create_connection((host, port), timeout=0.7):
                pass
        except OSError:
            continue
        state = api(host, port, "GET", "/api/state", timeout=20)
        if state and "titles" in state:
            return port
    return None


def stop(host: str, port: int) -> bool:
    return api(host, port, "POST", "/api/exit", timeout=20) is not None


def lua_str(s: str) -> str:
    return '"' + "".join(c if c.isascii() and (c.isalnum() or c in " .,:;-_()'!?&+/") else
                         "".join(f"\\{b:03d}" for b in c.encode()) for c in s) + '"'


def known_titles() -> dict[str, dict[str, str]]:
    """Names and versions from the title profiles (ghostldr/data/titles/*.json),
    for installs whose dump had no param.sfo."""
    out = {}
    for f in sorted(TITLES.glob("*.json")):
        try:
            d = json.loads(f.read_text())
        except (OSError, ValueError):
            continue
        tid, name = d.get("title"), d.get("name")
        if isinstance(tid, str) and re.fullmatch(r"[A-Z]{4}\d{5}", tid) and isinstance(name, str):
            out[tid] = {"name": name[:128], "version": str(d.get("version") or "")[:16]}
    return out


def payload(local_ip: str, port: int, scratch: str | None) -> bytes:
    head = [f"MANAGER_PORT = {port}", f'MANAGER_LOG_IP = "{local_ip}"', f"MANAGER_LOG_PORT = {LOG_PORT}"]
    if scratch:
        head.append(f'MANAGER_SCRATCH = "{scratch}"')
    titles = known_titles()
    if titles:
        head.append("MANAGER_TITLES = {" + ", ".join(
            f"[{lua_str(t)}] = {{name = {lua_str(v['name'])}, version = {lua_str(v['version'])}}}"
            for t, v in titles.items()) + "}")
    return ("\n".join(head) + "\n" + SCRIPT.read_text()).encode()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", help="the PS5's address (default: PS5_HOST, or found on the network)")
    ap.add_argument("--port", type=int, default=8090, help="web server port (default 8090; the next free one is used)")
    ap.add_argument("--detach", action="store_true", help="return once the manager is up, leave it running")
    ap.add_argument("--exit", action="store_true", help="stop a running manager")
    ap.add_argument("--scratch", help="developers: treat containers named <PREFIX>... as test saves "
                    "(3-8 of A-Z 0-9, never a title's prefix)")
    args = ap.parse_args()
    if args.scratch and not re.fullmatch(r"[A-Z][A-Z0-9]{2,7}", args.scratch):
        raise SystemExit("--scratch must be 3-8 of A-Z 0-9, starting with a letter")
    if not 1024 <= args.port <= 65535 - PORTS:
        raise SystemExit("--port must be 1024..65525")
    host = args.host or console_ip()

    already = running_port(host, args.port)
    if args.exit:
        if already is None:
            log(f"no manager answers on {host}:{args.port}-{args.port + PORTS - 1}")
            return 1
        log(f"stopping the manager on {host}:{already}: " + ("done" if stop(host, already) else "no answer"))
        return 0
    if already is not None:
        log(f"a manager is already running: http://{host}:{already}/  (its token was shown on the PS5 at start; "
            "the page's 'Show token on PS5' shows it again)")
        return 0

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect((host, 9))
        local_ip = probe.getsockname()[0]
    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", LOG_PORT))
    listener.settimeout(0.5)
    try:
        with socket.create_connection((host, 9026), timeout=10) as s:
            s.sendall(payload(local_ip, args.port, args.scratch))
            s.shutdown(socket.SHUT_WR)
    except OSError as e:
        raise SystemExit(f"cannot reach Luac0re's loader on {host}:9026 ({e}): start it in SWRR first, and stop "
                         "any script still holding it")
    log(f"manager.lua sent to {host}:9026")

    url = token = None
    port = args.port
    deadline = time.time() + 30
    stopping_since = None
    try:
        while True:
            try:
                data, _ = listener.recvfrom(4096)
            except socket.timeout:
                if url is None and time.time() > deadline:
                    raise SystemExit("the manager did not report back (see the PS5's screen; the computer's firewall "
                                     f"must let UDP {LOG_PORT} in)")
                if stopping_since and time.time() - stopping_since > 30:
                    log("no 'stopped' report: check the PS5")
                    return 1
                continue
            line = data.decode("utf-8", "replace").rstrip("\0").removeprefix("[manager] ")
            if line.startswith("url "):
                url = line[4:]
                m = re.search(r":(\d+)/", url)
                port = int(m.group(1)) if m else port
                url = f"http://{host}:{port}/"
            elif line.startswith("token "):
                token = line[6:]
                print(f"\n  ghostldr manager is running\n  open   {url}\n  token  {token}   (also shown on the PS5)\n",
                      flush=True)
                if args.detach:
                    log("detached: stop it from the page (Exit) or with --exit")
                    return 0
                log("Ctrl-C stops it")
            elif line.startswith("stopped"):
                log(line)
                return 0 if line == "stopped" else 1
            else:
                log(line)
    except KeyboardInterrupt:
        print()
        log("stopping the manager" + (": done" if stop(host, port) else ": no answer (stop it from the page)"))
        stopping_since = time.time()
        try:
            while time.time() - stopping_since < 30:
                try:
                    data, _ = listener.recvfrom(4096)
                except socket.timeout:
                    continue
                line = data.decode("utf-8", "replace").rstrip("\0").removeprefix("[manager] ")
                if line.startswith("stopped"):
                    log(line)
                    break
        except KeyboardInterrupt:
            pass
        return 0


if __name__ == "__main__":
    sys.exit(main())
