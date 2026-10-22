#!/usr/bin/env python3
"""Smoke test of an installed ghostldr app, offline: it never looks for a console.

    python3 tools/package/smoke_test.py [--home DIR] [--version V] -- COMMAND [ARGS...]
    python3 tools/package/smoke_test.py --instance FILE      # an app started some other way

Starts COMMAND --no-open (an installed copy's launcher: ghostldr.app's
Contents/MacOS/ghostldr, /usr/bin/ghostldr, an AppImage...), reads the address
it prints (or, from a launcher that puts the app in the background, the
instance.json the app writes) and drives the API as the page does:

- the state: version, the four prebuilt files, the Python, the tools folder;
- the folder browser;
- a generated homebrew dump: its check (profiles.py) and its preparation
  (gfs_pack.py), run by the app with its own Python from its tools folder;
- a compatibility report (import_report.py);
- Quit, and the app stops.

With --home, HOME points at DIR for the run, so the app's settings, logs and
tools folder go there and not in your own.  Needs Python 3.9+ and this repo
(the dump comes from ghostldr/tests/test_profiles.py).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "ghostldr/tests"))


class App:
    def __init__(self, port: int, token: str) -> None:
        self.port, self.token = port, token

    def call(self, method: str, path: str, body: dict | None = None, timeout: float = 60):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}", data=data, method=method,
                                     headers={"X-Ghostldr-Token": self.token, "Host": f"127.0.0.1:{self.port}",
                                              "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status, json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")

    def job(self, path: str, body: dict, limit: float = 180) -> dict:
        status, data = self.call("POST", path, body)
        if status != 200 or "job" not in data:
            raise AssertionError(f"{path}: {status} {data}")
        jid, since, lines = data["job"]["id"], 0, []
        end = time.time() + limit
        while time.time() < end:
            _, got = self.call("GET", f"/api/jobs/{jid}?since={since}&wait=5")
            lines += got.get("output", [])
            since = got.get("next", since)
            if got["state"] not in ("starting", "running"):
                got["all_output"] = lines
                return got
        raise AssertionError(f"{path}: still running after {limit:.0f} s")


def check(cond, what: str, detail="") -> None:
    print(f"  {'ok ' if cond else 'FAIL'} {what}" + (f": {detail}" if detail and not cond else ""), flush=True)
    if not cond:
        raise SystemExit(1)


def make_dump(base: Path) -> Path:
    from test_profiles import orbis_eboot  # ghostldr/tests
    dump = base / "Smoke Test Homebrew" / "app0"
    (dump / "assets").mkdir(parents=True)
    (dump / "eboot.bin").write_bytes(orbis_eboot("smoke_test"))
    (dump / "assets/data.bin").write_bytes(os.urandom(64 << 10))
    return dump


def run_checks(app: App, version: str | None, workspace: bool | None, tmp: Path) -> None:
    status, data = app.call("GET", "/api/ping")
    check(status == 200 and data.get("app") == "ghostldr", "the API answers with the launch's token")
    status, st = app.call("GET", "/api/state")
    check(status == 200, "state")
    print(f"       version {st['version']}, Python {st['python']} on {st['platform']}, tools in {st['root']}")
    if version:
        check(st["version"] == version, f"version {version}", st["version"])
    check(tuple(int(x) for x in st["python"].split(".")[:2]) >= (3, 11), "Python 3.11 or newer", st["python"])
    check(all(st["prebuilt"].values()), "the four prebuilt files", st["prebuilt"])
    root = Path(st["root"])
    check((root / "ghostldr/host/run.py").is_file(), "the tools folder has ghostldr/host", root)
    check(os.access(root, os.W_OK), "the tools folder can be written", root)
    if workspace is not None:
        check((root.name == "workspace") == workspace, "tools run from a workspace" if workspace
              else "tools run from the release folder", root)

    status, ls = app.call("POST", "/api/folder/list", {"path": str(tmp)})
    check(status == 200 and isinstance(ls.get("dirs"), list), "folder browser", ls)

    dump = make_dump(tmp)
    status, info = app.call("POST", "/api/dump/inspect", {"path": str(dump)})
    det = (info or {}).get("detect") or {}
    check(status == 200 and info.get("eboot") and det.get("title"), "dump check (profiles.py)", info)
    title = det["title"]
    print(f"       detected {title}: {det.get('kind') or det.get('name') or ''}")

    job = app.job("/api/pack", {"src": str(dump)})
    check(job["state"] == "done", "prepare the game (gfs_pack.py)", "\n".join(job["all_output"][-15:]))
    check((root / "build/ghostfs" / title / "layout.json").is_file(), f"build/ghostfs/{title}/layout.json written")
    _, st = app.call("GET", "/api/state")
    check(any(p["title"] == title for p in st["packed"]), "the library lists the prepared game")

    job = app.job("/api/report", {"src": str(dump)})
    check(job["state"] == "done", "compatibility report (import_report.py)", "\n".join(job["all_output"][-15:]))

    status, _ = app.call("POST", "/api/quit", {"force": True})
    check(status == 200, "quit")


def instance_file(home: Path) -> Path:
    if sys.platform == "darwin":
        return home / "Library/Application Support/ghostldr/instance.json"
    return home / ".config/ghostldr/instance.json"


def alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def main() -> int:
    ap = argparse.ArgumentParser(description="offline smoke test of an installed ghostldr app")
    ap.add_argument("--home", type=Path, help="HOME for the app (its settings, logs and tools folder)")
    ap.add_argument("--version", help="the version the app must report")
    ap.add_argument("--workspace", choices=("yes", "no"), help="whether the tools must run from a workspace")
    ap.add_argument("--instance", type=Path, help="attach to the app this instance.json names")
    ap.add_argument("command", nargs="*", help="the launcher and its arguments (after --)")
    args = ap.parse_args()
    workspace = None if args.workspace is None else args.workspace == "yes"
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        if args.instance:
            info = json.loads(args.instance.read_text())
            run_checks(App(int(info["port"]), info["token"]), args.version, workspace, tmp)
            print("smoke test passed")
            return 0
        if not args.command:
            ap.error("give the launcher after --, or --instance")
        env = dict(os.environ)
        args.home = args.home or (tmp / "home")
        if args.home:
            args.home.mkdir(parents=True, exist_ok=True)
            env["HOME"] = str(args.home)
            for k in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "GHOSTLDR_APP_CONFIG",
                      "GHOSTLDR_APP_WORKSPACE"):
                env.pop(k, None)
        proc = subprocess.Popen([*args.command, "--no-open"], env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, text=True)
        url = None
        start = time.time()
        printed = []
        while time.time() - start < 120 and proc.poll() is None:
            line = proc.stdout.readline()
            printed.append(line.rstrip())
            print(f"  app: {line.rstrip()}", flush=True)
            m = re.search(r"http://127\.0\.0\.1:(\d+)/#t=([A-Za-z0-9_-]+)", line)
            if m:
                url = m
                break
        if url is None and proc.poll() == 0:           # it went to the background
            inst = instance_file(Path(env["HOME"]))
            end = time.time() + 60
            while time.time() < end and not inst.is_file():
                time.sleep(0.5)
            check(inst.is_file(), "the app starts in the background and writes instance.json", inst)
            time.sleep(0.5)
            info = json.loads(inst.read_text())
            print(f"  app: in the background, pid {info['pid']}")
            run_checks(App(int(info["port"]), info["token"]), args.version, workspace, tmp)
            end = time.time() + 30
            while time.time() < end and alive(int(info["pid"])):
                time.sleep(0.3)
            check(not alive(int(info["pid"])), "the app stops after Quit")
            print("smoke test passed")
            return 0
        check(url is not None, "the app starts and prints its address", "\n".join(printed))
        try:
            run_checks(App(int(url[1]), url[2]), args.version, workspace, tmp)
            rest = proc.communicate(timeout=30)[0]
            for line in rest.splitlines():
                print(f"  app: {line}")
            check(proc.returncode == 0, "the app stops after Quit", proc.returncode)
        finally:
            if proc.poll() is None:
                proc.kill()
    print("smoke test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
