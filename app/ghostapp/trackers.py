"""Per-job progress: each tracker reads its job's lines as they arrive
(parsers.py) and keeps what the UI shows (snapshot()).  finish() runs once
the script has exited."""

from __future__ import annotations

import json
import time
from pathlib import Path

from . import parsers as P


NAME_TABLE_MIN = 1000      # names import_report.py must load for its unresolved count to mean anything


class Tracker:
    def feed(self, line: str) -> None:
        pass

    def finish(self, job) -> None:
        pass

    def snapshot(self) -> dict:
        return dict(self.__dict__)


class DiscoverTracker(Tracker):
    def __init__(self) -> None:
        self.consoles: list[dict] = []
        self.none = False

    def feed(self, line: str) -> None:
        c = P.parse_console_line(line)
        if c:
            self.consoles.append(c)
        elif line.startswith("no PS4 or PS5 answered"):
            self.none = True


class DoctorTracker(Tracker):
    def __init__(self) -> None:
        self.rows: list[dict] = []

    def feed(self, line: str) -> None:
        r = P.parse_doctor_line(line)
        if r:
            self.rows.append(r)

    def snapshot(self) -> dict:
        counts = {s: sum(r["status"] == s for r in self.rows) for s in ("PASS", "WARN", "FAIL")}
        return {"rows": self.rows, "counts": counts}


class PackTracker(Tracker):
    """gfs_pack.py prints nothing until the layout is written, then a summary
    and the fit verdict; the UI estimates the first phase from the folder scan."""

    def __init__(self, estimate_s: float = 0.0, layout_dir: Path | None = None) -> None:
        self.phase = "scanning"
        self.estimate_s = estimate_s
        self.started = time.time()
        self.result: dict = {}
        self.layout: dict = {}
        self.layout_dir = layout_dir
        self.lines: list[str] = []

    def feed(self, line: str) -> None:
        self.lines.append(line)
        if line.startswith("store art for") or line.startswith("  store"):
            self.phase = "art"
        elif line.startswith("selector: "):
            self.phase = "layout"
        elif P.PACK_SUMMARY.match(line):
            self.phase = "fit"
        elif P.parse_fit(line):
            self.phase = "done"

    def finish(self, job) -> None:
        self.result = P.parse_pack("\n".join(self.lines))
        title = self.result.get("title")
        if title and self.layout_dir:
            try:
                info = json.loads((self.layout_dir / title / "layout.json").read_text())
                self.layout = {k: info.get(k) for k in ("title", "name", "version", "total", "prefix", "files",
                                                         "src")}
                self.layout["containers"] = len(info.get("containers", []))
            except (OSError, ValueError):
                pass
        if job.state == "done":
            self.phase = "done"

    def snapshot(self) -> dict:
        return {"phase": self.phase, "estimate_s": self.estimate_s, "started": self.started,
                "result": self.result, "layout": self.layout}


class InstallTracker(Tracker):
    def __init__(self, dry_run: bool = False) -> None:
        self.dry_run = dry_run
        self.phase = "checking"           # checking, connecting, planning, sending, finishing, done
        self.title = ""
        self.resume = False
        self.send_containers = 0
        self.send_gib = 0.0
        self.done = 0
        self.sent_mib = 0
        self.rate_mbs = 0.0
        self.eta_min = None
        self.updated = time.time()
        self.started = time.time()
        self.space: dict | None = None
        self.nothing = False
        self.complete = False
        self.fit: dict | None = None
        self.installed: list[dict] = []
        self.surveyed = False             # the console's list of installed titles was read
        self.steps: list[str] = []        # the plan's other steps ("update GFSCATALOG", ...)
        self.last_container = ""
        self.meta_written = False

    def feed(self, line: str) -> None:
        ev = P.parse_install_line(line)
        if not ev:
            return
        kind = ev["event"]
        if kind == "fit":
            self.fit = ev["fit"]
        elif kind == "receiver":
            self.phase = "connecting"
        elif kind == "connected":
            self.phase = "planning"
        elif kind == "installed":
            self.surveyed = True
            if ev["entry"]:
                self.installed.append(ev["entry"])
        elif kind == "installing":
            self.title, self.resume = ev["title"], ev["resume"]
        elif kind == "step":
            self.steps = (self.steps + [ev["text"]])[-20:]
        elif kind == "plan":
            self.send_containers, self.send_gib = ev["containers"], ev["gib"]
        elif kind == "space":
            self.space = {k: ev[k] for k in ("need_gib", "free_gib", "at_least")}
        elif kind == "nothing":
            self.nothing = True
        elif kind == "progress":
            self.phase = "sending"
            self.done, self.send_containers = ev["index"], ev["count"]
            self.sent_mib += ev["mib"]
            self.rate_mbs, self.eta_min = ev["rate_mbs"], ev["eta_min"]
            self.last_container = ev["container"]
            self.updated = time.time()
            if self.done >= self.send_containers:
                self.phase = "finishing"
        elif kind == "meta_written":
            self.meta_written = True
        elif kind == "complete":
            self.complete, self.phase = True, "done"
        elif kind == "dry_run_done":
            self.phase = "done"

    def finish(self, job) -> None:
        if job.state == "done":
            self.phase = "done"

    def snapshot(self) -> dict:
        d = dict(self.__dict__)
        d["now"] = time.time()
        return d


class LibraryTracker(Tracker):
    def __init__(self) -> None:
        self.lines: list[str] = []
        self.result: dict = {}

    def feed(self, line: str) -> None:
        self.lines.append(line)

    def finish(self, job) -> None:
        self.result = P.parse_library("\n".join(self.lines))

    def snapshot(self) -> dict:
        return {"result": self.result}


class BootTracker(Tracker):
    def __init__(self, title: str = "", select: bool = False) -> None:
        self.stage = ""                   # the furthest of parsers.BOOT_STAGES reached
        self.title = title
        self.select = select
        self.selector_sent = False
        self.listening = False
        self.hints: list[str] = []
        self.crash = ""
        self.preflight = {"state": "", "text": ""}
        self.problems: list[str] = []
        self.log_lines = 0
        self.last_log = time.time()

    def feed(self, line: str) -> None:
        ev = P.parse_boot_line(line)
        if not ev:
            return
        kind = ev["event"]
        if kind == "stage":
            if P.BOOT_STAGES.index(ev["stage"]) > (P.BOOT_STAGES.index(self.stage) if self.stage else -1):
                self.stage = ev["stage"]
            if ev.get("title"):
                self.title = ev["title"]
        if kind in ("stage", "log", "crash"):
            self.log_lines += 1
            self.last_log = time.time()
        if kind == "hint":
            self.hints = (self.hints + [ev["text"]])[-20:]
        elif kind == "crash" and not self.crash:
            self.crash = ev["text"]
        elif kind == "preflight":
            self.preflight = {"state": ev["state"], "text": ev["text"]}
        elif kind == "selector":
            self.selector_sent = True
        elif kind == "listening":
            self.listening = True
        elif kind == "problem":
            self.problems.append(ev["text"])


class ManagerTracker(Tracker):
    def __init__(self) -> None:
        self.lines: list[str] = []
        self.info: dict = {}

    def feed(self, line: str) -> None:
        self.lines.append(line)
        self.info = P.parse_manager("\n".join(self.lines))

    def snapshot(self) -> dict:
        return {"info": self.info}


class JsonResultTracker(Tracker):
    """A tool that writes a JSON report (stub_report.py, import_report.py)."""

    def __init__(self, json_path: Path, md_path: Path | None = None) -> None:
        self.json_path, self.md_path = json_path, md_path
        self.result: dict | None = None
        self.markdown = ""
        self.stderr: list[str] = []

    def feed(self, line: str) -> None:
        if line.startswith(("warning:", "note:", "functions:", "executable memory:")):
            self.stderr.append(line)

    def finish(self, job) -> None:
        try:
            self.result = json.loads(self.json_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            self.result = None
        if self.md_path:
            try:
                self.markdown = self.md_path.read_text(encoding="utf-8")[:400_000]
            except OSError:
                pass

    def snapshot(self) -> dict:
        if self.md_path:
            result = summarize_report(self.result)
        elif isinstance(self.result, dict):          # stub_report.py: its full ranking can be long
            result = {**self.result, "top": list(self.result.get("top") or [])[:50]}
        else:
            result = self.result
        return {"result": result, "markdown": self.markdown, "notes": self.stderr}


def summarize_report(rep: dict | None) -> dict | None:
    """The parts of an import_report.py JSON report the UI shows."""
    if not isinstance(rep, dict):
        return None
    s = rep.get("summary", {})
    fit = rep.get("executable_memory") or {}
    libs = []
    for row in rep.get("libraries", [])[:400]:
        if isinstance(row, dict) and row.get("classes", {}).get("UNRESOLVED"):
            libs.append({"library": row.get("library", ""), "unresolved": row["classes"]["UNRESOLVED"]})
    libs.sort(key=lambda r: -r["unresolved"])
    names = (rep.get("inputs") or {}).get("names") or 0
    return {"names": names,
            # a release ships no build/nid/known_names.txt (tens of thousands of names): without it most
            # imports can't be matched to an implementation and count as unresolved
            "name_table": names >= NAME_TABLE_MIN,"functions": s.get("functions"), "by_class": s.get("functions_by_class", {}),
            "sites": s.get("import_sites"), "fit_verdict": fit.get("verdict", ""), "fit_error": fit.get("error", ""),
            "unresolved_libraries": libs[:12]}
