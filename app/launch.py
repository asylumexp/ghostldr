#!/usr/bin/env python3
"""Start the ghostldr app.

    python3 app/launch.py              # a window (pywebview, if installed) or the default browser
    python3 app/launch.py --browser    # always the browser
    python3 app/launch.py --no-open    # print the address only
    python3 app/launch.py --workspace  # run the tools from a copy in the user's data folder

Needs only Python 3.11+.  The launchers next to this file (ghostldr.command,
ghostldr.sh, ghostldr.bat) find Python and run this.  A second launch while
the app runs opens the running one instead of starting another.  Installed
copies (the packages in tools/package) pass --workspace, and a release folder
that can't be written gets one too: see ghostapp/workspace.py.
"""

from __future__ import annotations

import sys

if sys.version_info < (3, 11):
    sys.exit(f"ghostldr needs Python 3.11 or newer (this is {sys.version.split()[0]}): "
             "get it from https://www.python.org/downloads/")

import argparse
import json
import os
import signal
import threading
import time
import urllib.request
import webbrowser
from pathlib import Path

APP = Path(__file__).resolve().parent
ROOT = APP.parent
sys.path.insert(0, str(APP))

from ghostapp import APP_VERSION, workspace  # noqa: E402
from ghostapp.server import AppServer  # noqa: E402
from ghostapp.service import Service  # noqa: E402
from ghostapp.settings import config_dir  # noqa: E402

IDLE_EXIT = 30 * 60          # browser mode: stop after this long with no page and nothing running


def say(text: str) -> None:
    """Print, unless the terminal window has gone (the app is closing then)."""
    try:
        print(text, flush=True)
    except (OSError, ValueError):
        pass


def instance_file() -> Path:
    return config_dir() / "instance.json"


def running_instance(root: Path = ROOT) -> str | None:
    """The address (with its token) of an app already running from this folder."""
    try:
        info = json.loads(instance_file().read_text(encoding="utf-8"))
        if info.get("root") != str(root):
            return None
        req = urllib.request.Request(f"http://127.0.0.1:{int(info['port'])}/api/ping",
                                     headers={"X-Ghostldr-Token": info["token"],
                                              "Host": f"127.0.0.1:{int(info['port'])}"})
        with urllib.request.urlopen(req, timeout=2) as r:
            if json.loads(r.read()).get("app") == "ghostldr":
                return f"http://127.0.0.1:{int(info['port'])}/#t={info['token']}"
    except (OSError, ValueError, KeyError, TypeError):
        pass
    return None


def write_instance(server: AppServer, root: Path = ROOT) -> None:
    f = instance_file()
    try:
        f.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(str(f), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as out:
            json.dump({"port": server.port, "token": server.token, "pid": os.getpid(), "root": str(root)}, out)
    except OSError:
        pass


def clear_instance(server: AppServer) -> None:
    try:
        info = json.loads(instance_file().read_text(encoding="utf-8"))
        if info.get("pid") == os.getpid():
            instance_file().unlink()
    except (OSError, ValueError):
        pass


def check_tree() -> None:
    if not (ROOT / "ghostldr/host/run.py").is_file():
        sys.exit(f"ghostldr's tools are not next to the app ({ROOT / 'ghostldr/host'} is missing): "
                 "keep the app/ folder inside the unpacked ghostldr folder.")


def tools_root(option: str | None) -> Path:
    """The folder the scripts run from: the release itself, or a workspace
    (--workspace [DIR], or the release can't be written: an installed copy)."""
    if option is None and workspace.writable(ROOT):
        return ROOT
    return Path(option).expanduser().resolve() if option else workspace.default_dir()


def main() -> int:
    # Started in the background by a shell, SIGINT arrives ignored, and the
    # scripts would inherit that: Cancel (Ctrl+C to the script) needs it back.
    signal.signal(signal.SIGINT, signal.default_int_handler)
    ap = argparse.ArgumentParser(description="the ghostldr desktop app")
    ap.add_argument("--browser", action="store_true", help="open in the default browser even if pywebview is installed")
    ap.add_argument("--no-open", action="store_true", help="don't open anything; print the address")
    ap.add_argument("--port", type=int, default=0, help="local port (default: a free one)")
    ap.add_argument("--workspace", nargs="?", const="", default=None, metavar="DIR",
                    help="run the tools from a copy in DIR (default: the user's data folder), as installed "
                         "copies do; automatic when this folder can't be written")
    args = ap.parse_args()
    check_tree()
    root = tools_root(args.workspace)

    url = running_instance(root)
    if url:
        say(f"ghostldr is already running: {url}")
        if not args.no_open:
            webbrowser.open(url)
        return 0

    webview = None
    if not args.browser and not args.no_open:
        try:
            import webview  # type: ignore  # optional: pip install pywebview
        except Exception:
            webview = None

    if root != ROOT:
        try:
            if workspace.prepare(ROOT, root):
                say(f"Copied ghostldr's tools to {root}")
        except OSError as e:
            sys.exit(f"Couldn't copy ghostldr's tools to {root}: {e}")
    service = Service(root, webview=webview is not None)
    server = AppServer(service, args.port)
    write_instance(server, root)
    url = f"{server.url}#t={server.token}"
    # Closing the terminal window (SIGHUP; on Windows the console's close
    # event arrives as SIGBREAK) or a plain kill: stop the jobs first, as Quit does.
    for name in ("SIGHUP", "SIGTERM", "SIGBREAK"):
        if hasattr(signal, name):
            try:
                signal.signal(getattr(signal, name), lambda *_: server.quit.set())
            except (OSError, ValueError):
                pass
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.3}, daemon=True)
    thread.start()
    say(f"ghostldr app {APP_VERSION} is running at {url}")
    say("Keep this window open while you use it. Press Ctrl+C here to quit.")

    try:
        if webview is not None:
            from ghostapp import picker
            window = webview.create_window("ghostldr", url, width=1180, height=820, min_size=(400, 560),
                                           confirm_close=False)
            picker.set_window(window)

            def close_on_quit() -> None:
                server.quit.wait()
                try:
                    window.destroy()
                except Exception:
                    pass
            threading.Thread(target=close_on_quit, daemon=True).start()
            webview.start()
        else:
            if not args.no_open:
                webbrowser.open(url)
            while not server.quit.wait(5):
                idle = time.time() - service.last_request
                if idle > IDLE_EXIT and not service.jobs.running():
                    say("No window has used the app for 30 minutes: stopping. Start it again any time.")
                    break
    except KeyboardInterrupt:
        pass
    finally:
        say("Stopping ghostldr...")
        service.jobs.cancel_all()
        server.shutdown()
        server.server_close()
        clear_instance(server)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
