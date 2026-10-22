"""What the app does: one object the HTTP API calls into.

It starts the host scripts as jobs (jobs.py) with the right arguments, keeps
what they reported (consoles found, the library, the save manager's
address), and answers the small local questions itself: what is in a
folder, which titles are packed here, where a title's art is.
"""

from __future__ import annotations

import ipaddress
import json
import os
import re
import socket
import string
import struct
import subprocess
import sys
import threading
import time
import webbrowser
from pathlib import Path

from . import APP_VERSION, parsers, picker
from . import trackers as T
from .jobs import Busy, Job, JobManager, python_argv
from .settings import Settings, config_dir

TITLE_RE = re.compile(r"^[A-Z0-9_][A-Z0-9_-]{0,14}$")
LOADER_PORT = 9026
LOADER_MIN_INTERVAL = 2.0
SCAN_FILE_LIMIT = 250_000
SCAN_TIME_LIMIT = 20.0


class AppError(Exception):
    """A request the app refuses, with a message for the user."""

    def __init__(self, message: str, status: int = 400, **extra) -> None:
        super().__init__(message)
        self.status = status
        self.extra = extra


def norm_title(value) -> str:
    t = str(value or "").strip().upper()
    if not TITLE_RE.match(t):
        raise AppError("A title ID is 1–15 letters, digits, - or _ (like CUSA12345).")
    return t


class Service:
    def __init__(self, root: Path, cfg: Path | None = None, webview: bool = False) -> None:
        self.root = root.resolve()
        self.host = self.root / "ghostldr/host"
        self.tools = self.root / "ghostldr/tools"
        self.build = self.root / "build"
        self.cfg = cfg or config_dir()
        self.settings = Settings(self.cfg / "settings.json")
        self.jobs = JobManager(self.root, self.cfg / "logs")
        self.jobs.on_finish.append(self._job_finished)
        self.webview = webview
        self.lock = threading.Lock()
        self.consoles: list[dict] = []
        self.consoles_at = 0.0
        self.loader = {"ip": "", "listening": None, "checked": 0.0, "note": ""}
        self.library = {"entries": [], "refreshed": 0.0, "ok": False, "catalogue": True}
        self.manager = {"url": "", "token": "", "running": False}
        self.last_request = time.time()
        self._console_mod = None

    # ---- environment for the scripts ----

    def env(self, with_host: bool = True) -> dict:
        env = {k: v for k, v in os.environ.items()
               if k not in ("PS_HOST", "PS5_HOST", "PS_HOST_ID", "PS5_HOST_ID", "PYTHONPATH", "PYTHONHOME")}
        env.update(PYTHONUNBUFFERED="1", PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
        ip = self.settings.get("console")["ip"]
        if with_host and ip:
            env["PS_HOST"] = ip
        return env

    def console_ip(self) -> str:
        ip = self.settings.get("console")["ip"]
        if not ip:
            raise AppError("Choose your PS5 on the Console step first.", 409, action="goto_console")
        return ip

    def start(self, kind: str, label: str, script: Path, args: list[str], locks: set[str], tracker=None,
              need_console: bool = False, meta: dict | None = None) -> Job:
        if need_console:
            self.console_ip()
            if self.manager.get("running") and kind != "manager":
                raise AppError("The save manager is running on the PS5 and holds its loader: stop it first "
                               "(Library → Saves & backups).", 409, action="goto_manager")
        try:
            with self.lock:     # never while loader_status() is connecting to the loader
                return self.jobs.start(kind, label, python_argv(script, *args), locks, tracker,
                                       self.env(with_host=need_console), meta)
        except Busy as b:
            raise AppError(f"Wait for “{b.job.label}” to finish first, or cancel it.", 409,
                           busy=b.job.id, action="show_job")

    # ---- state ----

    def prebuilt(self) -> dict:
        d = self.build / "ghostldr"
        return {n: (d / n).is_file() for n in ("core.bin", "core.json", "ext.bin", "selector.bin")}

    def state(self) -> dict:
        running = self.jobs.running()
        return {
            "version": APP_VERSION, "platform": sys.platform, "python": sys.version.split()[0],
            "root": str(self.root), "config": str(self.cfg), "webview": self.webview,
            "prebuilt": self.prebuilt(), "settings": self.settings.snapshot(),
            "consoles": self.consoles, "consoles_at": self.consoles_at,
            "loader": dict(self.loader), "library": self.library, "packed": self.packed(),
            "manager": dict(self.manager), "jobs": self.jobs.list()[:15],
            "running": [j.id for j in running],
            "console_busy": next((j.summary(progress=False) for j in running if "console" in j.locks), None),
            "latest": {k: (j.id if (j := self.jobs.latest(k)) else None)
                       for k in ("discover", "doctor", "pack", "report", "install", "library", "boot",
                                 "manager", "stubs", "meta", "remove")},
            "runlog": (self.build / "ghostldr/run.log").is_file(),
            "names_table": (self.build / "nid/known_names.txt").is_file(),
        }

    def _job_finished(self, job: Job) -> None:
        tr = job.tracker
        if job.kind == "discover" and isinstance(tr, T.DiscoverTracker) and job.state == "done":
            self._found(tr.consoles)
        elif job.kind == "library" and isinstance(tr, T.LibraryTracker) and tr.result.get("ok"):
            self.library = {"entries": self._named(tr.result["entries"]), "refreshed": time.time(), "ok": True,
                            "catalogue": tr.result["catalogue"]}
        elif job.kind == "install" and isinstance(tr, T.InstallTracker) and job.state == "done":
            self._installed(job.meta.get("title", ""), tr)
        elif job.kind in ("remove", "meta") and job.state == "done":
            self.library["stale"] = True
        elif job.kind == "manager" and isinstance(tr, T.ManagerTracker):
            info = tr.info
            if job.meta.get("action") == "stop":
                if info.get("stopped") or job.state == "done":
                    self.manager = {"url": "", "token": "", "running": False}
            elif info.get("url"):
                self.manager = {"url": info["url"], "token": info.get("token", "") or self.manager.get("token", ""),
                                "running": True}
        if job.kind in ("install", "boot", "manager", "library", "remove", "meta", "doctor"):
            self.loader["checked"] = 0.0          # look again

    def _installed(self, title: str, tr: "T.InstallTracker") -> None:
        """An install (or its dry run) listed what the console has: keep that
        as the library, with the title just installed marked complete."""
        entries = [dict(e) for e in tr.installed if e.get("title") != title or tr.dry_run]
        if tr.complete or tr.nothing:
            have = next((e for e in tr.installed if e.get("title") == title), None)
            try:
                info = json.loads((self.build / "ghostfs" / title / "layout.json").read_text(encoding="utf-8"))
            except (OSError, ValueError):
                info = {}
            entry = dict(have or {})
            entry.update(title=title, complete=True,
                         name=entry.get("name") or info.get("name", ""),
                         version=entry.get("version") or info.get("version", ""),
                         containers=len(info.get("containers", [])) or entry.get("containers", 0),
                         prefix=info.get("prefix") or entry.get("prefix", ""),
                         gib=round(info.get("total", 0) / 2 ** 30, 1) or entry.get("gib", 0.0))
            entries = [e for e in entries if e.get("title") != title] + [entry]
        if tr.surveyed or tr.complete or tr.nothing:
            self.library = {"entries": self._named(entries), "refreshed": time.time(), "ok": True,
                            "catalogue": self.library.get("catalogue", True)}

    def _named(self, entries: list[dict]) -> list[dict]:
        """Catalogue entries, with a name from this computer's pack or the
        title's profile when the console has none (Ghost's dump has no
        param.sfo, so its catalogue entry is nameless)."""
        out = []
        for e in entries:
            e = dict(e)
            if not e.get("name") and e.get("title"):
                try:
                    info = json.loads((self.build / "ghostfs" / e["title"] / "layout.json").read_text(encoding="utf-8"))
                    e["name"] = str(info.get("name") or "")
                except (OSError, ValueError):
                    pass
                e["name"] = e["name"] or self.profile_name(e["title"])
            out.append(e)
        return out

    def _found(self, consoles: list[dict]) -> None:
        """Discovery finished: keep the remembered console (by host-id) up to date."""
        self.consoles, self.consoles_at = consoles, time.time()
        cur = self.settings.get("console")
        match = None
        if cur.get("host_id"):
            match = next((c for c in consoles if c["host_id"] == cur["host_id"]), None)
        if not match and cur.get("ip"):
            match = next((c for c in consoles if c["ip"] == cur["ip"]), None)
        if not match and not cur.get("ip"):
            candidates = [c for c in consoles if c["on"]]
            if len(candidates) > 1:
                candidates = [c for c in candidates if c["loader"]] or candidates
            if len(candidates) == 1:
                match = candidates[0]
        if match:
            self._remember(match)
            self.loader = {"ip": match["ip"], "listening": match["loader"], "checked": time.time(), "note": ""}

    def _remember(self, c: dict) -> None:
        self.settings.update(console={"ip": c["ip"], "host_id": c.get("host_id", ""), "name": c.get("name", ""),
                                      "kind": c.get("kind", "")})

    # ---- console ----

    def discover(self) -> Job:
        return self.start("discover", "Searching for consoles", self.host / "console.py", ["--list"],
                          {"discover"}, T.DiscoverTracker())

    def console_module(self):
        if self._console_mod is None:
            if str(self.host) not in sys.path:
                sys.path.insert(0, str(self.host))
            import console  # ghostldr/host/console.py
            self._console_mod = console
        return self._console_mod

    def select_console(self, ip: str, host_id: str = "") -> dict:
        try:
            ip = str(ipaddress.IPv4Address(str(ip).strip()))
        except ValueError:
            raise AppError("That isn't an IP address (it looks like 192.168.1.23).")
        if ip.startswith(("127.", "0.")) or ip == "255.255.255.255":
            raise AppError("Use the PS5's address on your network (Settings → Network → Connection Status).")
        known = next((c for c in self.consoles if c["ip"] == ip), None)
        if not known:
            known = {"ip": ip, "host_id": "", "name": "", "kind": "", "firmware": "", "on": True, "loader": False,
                     "manual": True}
            try:
                c = self.console_module().probe(ip)
            except Exception:
                c = None
            if c:
                known.update(host_id=c.host_id, name=c.name, kind=c.kind, firmware=c.firmware, on=c.on)
            self.consoles = [x for x in self.consoles if x["ip"] != ip] + [known]
        if host_id and known.get("host_id") and known["host_id"] != host_id:
            raise AppError("That console changed address: search again.")
        self._remember(known)
        self.loader = {"ip": ip, "listening": None, "checked": 0.0, "note": ""}
        return known

    def forget_console(self) -> None:
        self.settings.update(console={"ip": "", "host_id": "", "name": "", "kind": ""})
        self.loader = {"ip": "", "listening": None, "checked": 0.0, "note": ""}

    def loader_status(self) -> dict:
        """Whether Luac0re's loader listens on the chosen console (TCP 9026).
        Not asked while a job uses the console: the loader is busy then."""
        ip = self.settings.get("console")["ip"]
        if not ip:
            return {"ip": "", "listening": None, "checked": 0.0, "note": "no console"}
        busy = self.jobs.holder({"console"})
        if busy:
            return {**self.loader, "ip": ip, "note": f"busy: {busy.label}"}
        if self.manager.get("running"):
            return {**self.loader, "ip": ip, "listening": False, "note": "the save manager holds the loader"}
        with self.lock:
            if self.loader.get("ip") == ip and time.time() - self.loader.get("checked", 0) < LOADER_MIN_INTERVAL:
                return dict(self.loader)
            listening = probe_port(ip, LOADER_PORT)
            self.loader = {"ip": ip, "listening": listening, "checked": time.time(), "note": ""}
            return dict(self.loader)

    def doctor(self, title: str = "") -> Job:
        args = ["doctor"]
        if title:       # check that title's dump (else run.py picks the only one here, if any)
            args += ["--title", norm_title(title)]
        return self.start("doctor", "Checking the setup", self.host / "run.py", args, {"console"},
                          T.DoctorTracker(), need_console=True)

    # ---- the game folder ----

    def pick_folder(self) -> dict:
        available, path = picker.choose_folder()
        return {"available": available, "path": path}

    def roots(self) -> list[dict]:
        home = Path.home()
        out = [{"name": "Home", "path": str(home)}]
        for sub in ("Desktop", "Downloads", "Documents"):
            if (home / sub).is_dir():
                out.append({"name": sub, "path": str(home / sub)})
        if os.name == "nt":
            for letter in string.ascii_uppercase:
                d = f"{letter}:\\"
                if os.path.exists(d):
                    out.append({"name": f"{letter}:", "path": d})
        else:
            bases = [Path("/Volumes")] if sys.platform == "darwin" else \
                [Path("/media") / home.name, Path("/run/media") / home.name, Path("/mnt"), Path("/media")]
            for base in bases:
                try:
                    for d in sorted(base.iterdir()):
                        if d.is_dir() and not d.name.startswith("."):
                            out.append({"name": d.name, "path": str(d)})
                except OSError:
                    pass
            out.append({"name": "Computer", "path": "/"})
        return out

    def list_dir(self, path: str | None) -> dict:
        p = Path(path).expanduser() if path else Path.home()
        if not p.is_absolute():
            raise AppError("Use a full folder path.")
        try:
            names = sorted(os.scandir(p), key=lambda e: e.name.lower())
        except OSError as e:
            raise AppError(f"Can't open that folder: {e.strerror or e}.")
        dirs = []
        for e in names:
            if e.name.startswith(".") or e.name.startswith("$"):
                continue
            try:
                if not e.is_dir():
                    continue
            except OSError:
                continue
            game = os.path.isfile(os.path.join(e.path, "eboot.bin"))
            dirs.append({"name": e.name, "path": e.path, "game": game})
            if len(dirs) >= 800:
                break
        parent = str(p.parent) if p.parent != p else None
        return {"path": str(p), "parent": parent, "dirs": dirs, "game": (p / "eboot.bin").is_file(),
                "roots": self.roots()}

    def inspect_dump(self, path: str) -> dict:
        p = Path(str(path).strip().strip('"')).expanduser()
        if not str(path).strip():
            raise AppError("Choose a folder first.")
        if not p.is_absolute():
            raise AppError("Use a full folder path.")
        if not p.is_dir():
            raise AppError("That folder doesn't exist (was the drive disconnected?).")
        info: dict = {"path": str(p), "eboot": (p / "eboot.bin").is_file(), "suggestions": []}
        if not info["eboot"]:
            for cand in [p / "app0", p / "Image0"] + [d for d in sorted(p.iterdir())[:200] if d.is_dir()]:
                if (cand / "eboot.bin").is_file() and str(cand) not in info["suggestions"]:
                    info["suggestions"].append(str(cand))
            return info
        info.update(sce_module=(p / "sce_module").is_dir(), param_sfo=(p / "sce_sys/param.sfo").is_file(),
                    icon=(p / "sce_sys/icon0.png").is_file())
        info["sfo_name"] = sfo_strings(p / "sce_sys/param.sfo").get("TITLE", "")[:120]
        t0 = time.time()
        files, size, partial = 0, 0, False
        stack = [str(p)]
        while stack and not partial:
            try:
                with os.scandir(stack.pop()) as it:
                    for e in it:
                        try:
                            if e.is_dir(follow_symlinks=False):
                                stack.append(e.path)
                            elif e.is_file():
                                files += 1
                                size += e.stat().st_size
                        except OSError:
                            pass
            except OSError:
                pass
            if files > SCAN_FILE_LIMIT or time.time() - t0 > SCAN_TIME_LIMIT:
                partial = True
        info.update(files=files, bytes=size, partial=partial, scan_seconds=round(time.time() - t0, 2))
        try:
            r = subprocess.run(python_argv(self.host / "profiles.py", "detect", "--src", str(p)),
                               capture_output=True, timeout=90, cwd=str(self.root), env=self.env(False),
                               stdin=subprocess.DEVNULL)
            det = parsers.parse_detect(r.stdout.decode("utf-8", "replace"))
        except (OSError, subprocess.TimeoutExpired):
            det = None
        info["detect"] = det
        title = (det or {}).get("title", "")
        info["profile_name"] = self.profile_name(title) if title else ""
        return info

    def profile_name(self, title: str) -> str:
        try:
            d = json.loads((self.root / "ghostldr/data/titles" / f"{title}.json").read_text(encoding="utf-8"))
            return str(d.get("name") or "")
        except (OSError, ValueError):
            return ""

    # ---- packing and reports (this computer only) ----

    def pack(self, src: str, title: str = "", fetch_art: bool = False) -> Job:
        info = self.inspect_dump(src)
        if not info.get("eboot"):
            raise AppError("That folder has no eboot.bin. Choose the game's app0 folder.",
                           suggestions=info.get("suggestions", []))
        args = ["--src", info["path"]]
        if title:
            args += ["--title", norm_title(title)]
        if fetch_art:
            args.append("--fetch-art")
        estimate = max(1.5, info.get("scan_seconds", 1) * 1.3 + 1.0) + (8.0 if fetch_art else 0.0)
        self.settings.update(dump=info["path"], fetch_art=bool(fetch_art))
        return self.start("pack", "Preparing the game", self.host / "gfs_pack.py", args, {"pack"},
                          T.PackTracker(estimate, self.build / "ghostfs"), meta={"src": info["path"]})

    def report(self, src: str) -> Job:
        p = Path(str(src)).expanduser()
        if not (p / "eboot.bin").is_file():
            raise AppError("Choose a game folder (with eboot.bin) first.")
        out = self.cfg / "reports"
        out.mkdir(parents=True, exist_ok=True)
        js, md = out / "import_report.json", out / "import_report.md"
        for f in (js, md):
            f.unlink(missing_ok=True)
        return self.start("report", "Checking compatibility", self.tools / "import_report.py",
                          [str(p), "--json", str(js), "--md", str(md)], set(), T.JsonResultTracker(js, md),
                          meta={"src": str(p)})

    def packed(self) -> list[dict]:
        out = []
        for f in sorted((self.build / "ghostfs").glob("*/layout.json"), key=lambda f: -f.stat().st_mtime):
            try:
                info = json.loads(f.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            src = info.get("src") or ""
            out.append({"title": info.get("title", f.parent.name), "name": info.get("name", ""),
                        "version": info.get("version", ""), "total": info.get("total", 0),
                        "containers": len(info.get("containers", [])), "src": src,
                        "src_ok": bool(src) and (Path(src) / "eboot.bin").is_file(),
                        "meta": bool(info.get("meta")), "packed": f.stat().st_mtime,
                        "art": self.art_path(info.get("title", ""), probe_only=True)})
        return out

    # ---- the console's titles ----

    def install(self, title: str, dry_run: bool = False, no_fit_check: bool = False) -> Job:
        title = norm_title(title)
        if not (self.build / "ghostfs" / title / "layout.json").is_file():
            raise AppError("Prepare this game first (Game step).", 409, action="goto_game")
        args = ["--title", title]
        if dry_run:
            args.append("--dry-run")
        if no_fit_check:
            args.append("--no-fit-check")
        self.settings.update(title=title)
        label = f"Checking the install of {title}" if dry_run else f"Installing {title}"
        return self.start("install", label, self.host / "gfs_install.py", args, {"console", "pack"},
                          T.InstallTracker(dry_run), need_console=True, meta={"title": title, "dry_run": dry_run})

    def library_refresh(self) -> Job:
        return self.start("library", "Reading the PS5's library", self.host / "gfs_install.py", ["--list"],
                          {"console"}, T.LibraryTracker(), need_console=True)

    def library_remove(self, title: str, confirm: str) -> Job:
        title = norm_title(title)
        if str(confirm or "").strip().upper() != title:
            raise AppError(f"Type {title} to confirm.")
        return self.start("remove", f"Removing {title}", self.host / "gfs_install.py", ["--remove", title],
                          {"console", "pack"}, T.InstallTracker(), need_console=True, meta={"title": title})

    def library_meta(self, title: str, fetch_art: bool = True) -> Job:
        title = norm_title(title)
        args = ["--update-meta", "--title", title]
        if fetch_art:
            args.append("--fetch-art")
        return self.start("meta", f"Updating the name and art of {title}", self.host / "gfs_install.py", args,
                          {"console", "pack"}, T.InstallTracker(), need_console=True, meta={"title": title})

    def art_path(self, title: str, probe_only: bool = False) -> str:
        """A PNG for the title's tile: fetched store art, the dump's icon0.png,
        or the icon in its packed meta.bin (extracted by gfs_meta.py)."""
        try:
            title = norm_title(title)
        except AppError:
            return ""
        d = self.build / "ghostfs" / title
        cands = [d / "art/icon.png"]
        try:
            src = json.loads((d / "layout.json").read_text(encoding="utf-8")).get("src")
            if src:
                cands.append(Path(src) / "sce_sys/icon0.png")
        except (OSError, ValueError):
            pass
        cache = self.cfg / "art" / title
        cands.append(cache / "icon.png")
        for c in cands:
            if c.is_file():
                return str(c)
        meta = d / "meta.bin"
        if meta.is_file() and not probe_only:
            try:
                subprocess.run(python_argv(self.host / "gfs_meta.py", "show", str(meta), "--extract", str(cache)),
                               capture_output=True, timeout=60, cwd=str(self.root), env=self.env(False),
                               stdin=subprocess.DEVNULL)
            except (OSError, subprocess.TimeoutExpired):
                pass
            if (cache / "icon.png").is_file():
                return str(cache / "icon.png")
        return "meta" if meta.is_file() else ""

    def art(self, title: str) -> bytes | None:
        p = self.art_path(title)
        if not p or p == "meta":
            return None
        try:
            data = Path(p).read_bytes()
        except OSError:
            return None
        return data if data[:8] == b"\x89PNG\r\n\x1a\n" and len(data) < 16 << 20 else None

    # ---- playing ----

    def boot(self, title: str = "", select: bool = False, force_preflight: bool = False) -> Job:
        args = ["boot", "--app-root", "ghostfs", "--no-build"]
        if title and not select:
            args += ["--title", norm_title(title)]
        if select:
            args.append("--select")
        if force_preflight:
            args.append("--force-preflight")
        missing = [n for n, ok in self.prebuilt().items() if not ok and n != "selector.bin"]
        if missing:
            raise AppError(f"The prebuilt loader files are missing ({', '.join(missing)}): unpack the release zip "
                           "again and start the app from inside it.")
        if select and not self.prebuilt()["selector.bin"]:
            raise AppError("This copy has no game selector (build/ghostldr/selector.bin): pick a title instead.")
        label = "Playing " + (title or "the game") if not select else "Playing (game selector)"
        return self.start("boot", label, self.host / "run.py", args, {"console", "ports"},
                          T.BootTracker(title, select), need_console=True, meta={"title": title, "select": select})

    def stubs(self) -> Job:
        log = self.build / "ghostldr/run.log"
        if not log.is_file():
            raise AppError("There is no game log yet: play a game first.")
        out = self.cfg / "reports" / "stubs.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.unlink(missing_ok=True)
        return self.start("stubs", "Analysing the game log", self.tools / "stub_report.py",
                          [str(log), "--json", str(out), "--top", "40"], set(), T.JsonResultTracker(out))

    def runlog(self, tail: int = 400) -> dict:
        log = self.build / "ghostldr/run.log"
        if not log.is_file():
            return {"exists": False, "lines": [], "size": 0, "path": str(log)}
        tail = max(10, min(int(tail), 5000))
        size = log.stat().st_size
        with log.open("rb") as f:
            f.seek(max(0, size - tail * 400))
            data = f.read()
        lines = data.decode("utf-8", "replace").splitlines()[-tail:]
        return {"exists": True, "lines": lines, "size": size, "path": str(log), "modified": log.stat().st_mtime}

    # ---- the save manager ----

    def manager_start(self) -> Job:
        return self.start("manager", "Starting the save manager", self.host / "manager.py", ["--detach"],
                          {"console"}, T.ManagerTracker(), need_console=True, meta={"action": "start"})

    def manager_stop(self) -> Job:
        return self.start("manager", "Stopping the save manager", self.host / "manager.py", ["--exit"],
                          {"console"}, T.ManagerTracker(), need_console=True, meta={"action": "stop"})

    # ---- outside the app ----

    def open_url(self, url: str) -> None:
        if not url or url != self.manager.get("url"):
            raise AppError("Only the save manager's address can be opened.")
        webbrowser.open(url)

    def open_folder(self, which: str) -> None:
        target = {"logs": self.cfg / "logs", "build": self.build / "ghostldr", "app": self.root}.get(which)
        if not target:
            raise AppError("Unknown folder.")
        target.mkdir(parents=True, exist_ok=True)
        if sys.platform == "darwin":
            argv = ["open", str(target)]
        elif os.name == "nt":
            argv = ["explorer", str(target)]
        else:
            argv = ["xdg-open", str(target)]
        try:
            subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError as e:
            raise AppError(f"Couldn't open the folder: {e}")


def sfo_strings(path: Path) -> dict[str, str]:
    """The text fields of a param.sfo (TITLE, TITLE_ID, APP_VER...); {} if unreadable."""
    try:
        data = path.read_bytes()[:1 << 20]
        if data[:4] != b"\0PSF" or len(data) < 20:
            return {}
        keys, values, count = struct.unpack_from("<III", data, 8)
        out = {}
        for i in range(min(count, 256)):
            koff, fmt, length, _, voff = struct.unpack_from("<HHIII", data, 20 + 16 * i)
            if fmt != 0x0204:                   # utf-8 text
                continue
            key = data[keys + koff:data.index(b"\0", keys + koff)].decode("ascii", "replace")
            raw = data[values + voff:values + voff + length]
            out[key] = raw.split(b"\0", 1)[0].decode("utf-8", "replace").strip()
        return out
    except (OSError, ValueError, struct.error):
        return {}


def probe_port(ip: str, port: int, timeout: float = 0.8) -> bool:
    """An empty TCP connection (Luac0re's loader runs nothing for it)."""
    try:
        socket.create_connection((ip, port), timeout=timeout).close()
        return True
    except OSError:
        return False
