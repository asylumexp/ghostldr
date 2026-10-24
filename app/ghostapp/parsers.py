"""Read the host scripts' output.

Every function here takes text the scripts print (console.py --list,
run.py doctor and boot, gfs_pack.py, gfs_install.py, manager.py,
profiles.py detect) and turns it into plain data for the UI.  The formats
are the scripts' own; when one changes, the matching test in
ghostldr/tests/test_app.py says so.
"""

from __future__ import annotations

import ipaddress
import re

STAMP = re.compile(r"^(\d\d:\d\d:\d\d) (.*)$")
TOOL_PREFIX = re.compile(r"^(?:\d\d:\d\d:\d\d )?(?:gfs-install|manager): (.*)$")


def unstamp(line: str) -> str:
    """A gfs_install.py / manager.py log line without its time and tool name."""
    m = TOOL_PREFIX.match(line)
    return m.group(1) if m else line


def _num(text: str, kind=float):
    try:
        return kind(text)
    except (TypeError, ValueError):
        return None


# ---- console.py --list ----

LIST_LINE = re.compile(r"^\s*(\d+)\) (.*)$")


def parse_console_line(line: str) -> dict | None:
    """One `N) ip  kind  name  host-id X  firmware F  on ...` line (Console.describe)."""
    m = LIST_LINE.match(line)
    if not m:
        return None
    parts = [p.strip() for p in m.group(2).split("  ") if p.strip()]
    hi = next((i for i, p in enumerate(parts) if p.startswith("host-id ")), None)
    if hi is None or hi < 2:
        return None
    try:
        ip = str(ipaddress.IPv4Address(parts[0]))
    except ValueError:
        return None
    name = "  ".join(parts[2:hi])
    host_id = parts[hi][len("host-id "):].strip()
    c = {"ip": ip, "kind": "" if parts[1] == "?" else parts[1], "name": "" if name == "?" else name,
         "host_id": "" if host_id == "?" else host_id, "firmware": "", "on": True, "running": "",
         "swrr": "", "loader": False}
    for p in parts[hi + 1:]:
        if p.startswith("firmware "):
            fw = p[len("firmware "):]
            c["firmware"] = "" if fw == "?" else fw
        elif p == "on":
            c["on"] = True
        elif p == "rest mode":
            c["on"] = False
        elif p.startswith("running "):
            c["running"] = p[len("running "):]
        elif p.startswith("SWRR "):
            c["swrr"] = p[len("SWRR "):]
        elif p.startswith("Luac0re loader on "):
            c["loader"] = True
    return c


def parse_console_list(text: str) -> list[dict]:
    return [c for c in map(parse_console_line, text.splitlines()) if c]


# ---- run.py doctor ----

DOCTOR_LINE = re.compile(r"^(PASS|WARN|FAIL)  (.*)$")
DOCTOR_KEYS = [("python", "python"), ("prebuilt core", "core"), ("core.bin", "core"), ("lua payload", "payload"),
               ("UDP 18194", "log_port"), ("TCP 18195", "ext_port"), ("console loader", "loader"),
               ("title dump", "dump")]


def parse_doctor_line(line: str) -> dict | None:
    """`STATUS  label (padded to 22) detail` (run.py doctor)."""
    m = DOCTOR_LINE.match(line)
    if not m:
        return None
    rest = m.group(2)
    if len(rest) >= 23 and rest[22] == " ":
        label, detail = rest[:22].strip(), rest[23:].strip()
    else:
        bits = re.split(r"\s{2,}", rest.strip(), maxsplit=1)
        label, detail = bits[0], (bits[1] if len(bits) > 1 else "")
    key = next((k for needle, k in DOCTOR_KEYS if label.startswith(needle)), "other")
    row = {"status": m.group(1), "label": label, "detail": detail, "key": key}
    if key == "dump":
        # `<title or reason>: N functions, M unresolved` when a dump was checked
        c = re.match(r"^(.*): (\d+) functions, (\d+) unresolved$", detail)
        if c:
            row.update(title=c[1], functions=int(c[2]), unresolved=int(c[3]))
    return row


def parse_doctor(text: str) -> list[dict]:
    return [r for r in map(parse_doctor_line, text.splitlines()) if r]


# ---- executable memory verdict (gfs_pack.py, gfs_install.py, import_report.py) ----

FITS, MULTI, NO_FIT, UNKNOWN = "fits", "multi", "nofit", "unknown"


def parse_fit(line: str) -> dict | None:
    """`executable memory: <verdict>` (tools/exec_fit.py summary)."""
    m = re.search(r"executable memory: (.*)$", line)
    if not m:
        return None
    text = m.group(1).strip()
    if text.startswith("fits the EE buffer"):
        kind = FITS
    elif text.startswith("needs multi-buffer placement"):
        kind = MULTI
    elif text.startswith("won't fit"):
        kind = NO_FIT
    else:
        kind = UNKNOWN
    return {"kind": kind, "text": text, "reclaim": "would fit after reclaim" in text}


# ---- gfs_pack.py ----

PACK_SUMMARY = re.compile(
    r"^(?P<title>\S+) \((?P<how>[^)]*)\): (?P<files>\d+) files \((?P<traced>\d+) from the open order first\), "
    r"(?P<dirs>\d+) dirs, (?P<gib>[\d.]+) GiB in (?P<containers>\d+) containers (?P<prefix>[A-Z0-9]+)000\.\.; "
    r"layout (?P<layout>[0-9a-f]+)$")


def parse_pack(text: str) -> dict:
    out: dict = {"title": "", "files": None, "gib": None, "containers": None, "prefix": "", "own_profile": None,
                 "fit": None, "selector": "", "notes": []}
    for line in text.splitlines():
        m = PACK_SUMMARY.match(line)
        if m:
            out.update(title=m["title"], files=int(m["files"]), dirs=int(m["dirs"]), traced=int(m["traced"]),
                       gib=float(m["gib"]), containers=int(m["containers"]), prefix=m["prefix"],
                       layout=m["layout"], own_profile=m["how"] == "own profile")
            continue
        fit = parse_fit(line)
        if fit:
            out["fit"] = fit
        elif line.startswith("selector: "):
            out["selector"] = line[len("selector: "):]
        elif "homebrew without a usable param.sfo" in line:
            out["notes"].append(line)
    return out


# ---- gfs_pack.py --usb and usb_drive.py ----

USB_START = re.compile(r"^(?P<title>\S+): copying (?P<todo>\d+) of (?P<files>\d+) files, (?P<gib>[\d.]+) GiB$")
USB_PROGRESS = re.compile(r"^(?P<title>\S+): (?P<done>[\d.]+) of (?P<need>[\d.]+) GiB, (?P<count>\d+) of "
                          r"(?P<todo>\d+) files, (?P<rate>\d+) MiB/s$")
USB_SUMMARY = re.compile(r"^(?P<title>\S+): (?P<files>\d+) files, (?P<gib>[\d.]+) GiB in (?P<folder>.+) "
                         r"\((?P<fs>[^)]*)\): (?P<copied>\d+) copied, (?P<kept>\d+) already there, "
                         r"(?P<removed>\d+) removed; progress goes to (?P<prefix>\S+)SAVE on the console$")
USB_CONNECTIONS = re.compile(r"^\S+: copying over (\d+) connection\(s\)")
USB_NAME = re.compile(r"^selector: (\S+): name '(.*?)' \(")
USB_LIST_HEAD = re.compile(r"^(?P<label>\S.*) \((?P<fs>[^()]*)\): (?P<n>\d+) title\(s\)$")


def parse_usb_line(line: str) -> dict | None:
    """One line of gfs_pack.py --usb (usb_drive.put_title) as an event."""
    line = line.rstrip()
    if m := USB_START.match(line):
        return {"event": "start", "title": m["title"], "todo": int(m["todo"]), "files": int(m["files"]),
                "gib": float(m["gib"])}
    if m := USB_PROGRESS.match(line):
        return {"event": "progress", "title": m["title"], "done_gib": float(m["done"]), "need_gib": float(m["need"]),
                "count": int(m["count"]), "todo": int(m["todo"]), "rate": int(m["rate"])}
    if m := USB_SUMMARY.match(line):
        return {"event": "summary", "title": m["title"], "files": int(m["files"]), "gib": float(m["gib"]),
                "folder": m["folder"], "fs": m["fs"], "copied": int(m["copied"]), "kept": int(m["kept"]),
                "removed": int(m["removed"]), "prefix": m["prefix"]}
    if m := USB_CONNECTIONS.match(line):
        return {"event": "connections", "count": int(m[1])}
    if m := USB_NAME.match(line):
        return {"event": "name", "title": m[1], "name": m[2]}
    if line.startswith("selector: "):
        return {"event": "selector", "text": line[len("selector: "):]}
    if "aren't known over FTP" in line:
        return {"event": "ftp"}
    fit = parse_fit(line)
    if fit:
        return {"event": "fit", "fit": fit}
    return None


def parse_usb_list(text: str) -> dict:
    """usb_drive.py list DRIVE: the drive's file system and titles ({title, name, version})."""
    out: dict = {"ok": False, "label": "", "fs": "", "entries": []}
    for line in text.splitlines():
        if m := USB_LIST_HEAD.match(line):
            out.update(ok=True, label=m["label"], fs=m["fs"])
        elif out["ok"] and line.startswith("  ") and line.strip():
            parts = line[2:].split("  ")
            title = parts[0].strip()
            if not re.fullmatch(r"[A-Z0-9_][A-Z0-9_-]{0,14}", title):
                continue
            rest = [p.strip() for p in parts[1:] if p.strip()]
            version = rest.pop() if rest and re.fullmatch(r"\d{2}\.\d{2}", rest[-1]) else ""
            out["entries"].append({"title": title, "name": " ".join(rest), "version": version})
    return out


# ---- gfs_install.py ----

PROGRESS = re.compile(r"^\[(\d+)/(\d+)\] (\S+): (\d+) MiB in (\d+) s \(console (\d+) s\); avg ([\d.]+) MB/s, "
                      r"ETA (\d+) min$")
PLAN_SEND = re.compile(r"^\s+send (\d+) containers, ([\d.]+) GiB$")
INSTALLING = re.compile(r"^installing (\S+)(?: .*)? in (\d+) containers (\S+?)000\.\. \(([\d.]+) GiB(, incomplete)?\)$")
SPACE = re.compile(r"^\s+space: ([\d.]+) GiB needed, (at least )?([\d.]+) GiB free$")
ENTRY = re.compile(r"^(?P<head>\S.*?): (?P<containers>\d+) containers (?P<prefix>[A-Z0-9]+)000\.\. "
                   r"\((?P<gib>[\d.]+) GiB\), index (?P<index>[A-Z0-9_-]+?)(?P<inc>, INCOMPLETE)?$")
VERSION = re.compile(r"^v?\d{1,3}(?:\.\d{1,3}){1,2}$")
TITLES_LINE = re.compile(r"^(\d+) titles installed")


def parse_install_line(line: str) -> dict | None:
    """One gfs_install.py line, as an event: progress, plan, space, done..."""
    msg = unstamp(line)
    m = PROGRESS.match(msg)
    if m:
        return {"event": "progress", "index": int(m[1]), "count": int(m[2]), "container": m[3],
                "mib": int(m[4]), "seconds": int(m[5]), "rate_mbs": float(m[7]), "eta_min": int(m[8])}
    m = PLAN_SEND.match(msg)
    if m:
        return {"event": "plan", "containers": int(m[1]), "gib": float(m[2])}
    m = INSTALLING.match(msg)
    if m:
        return {"event": "installing", "title": m[1], "containers": int(m[2]), "prefix": m[3],
                "gib": float(m[4]), "resume": bool(m[5])}
    m = SPACE.match(msg)
    if m:
        return {"event": "space", "need_gib": float(m[1]), "free_gib": float(m[3]), "at_least": bool(m[2])}
    if msg.strip() == "nothing to do: already installed":
        return {"event": "nothing"}
    if msg == "install complete":
        return {"event": "complete"}
    if msg == "dry run: nothing changed":
        return {"event": "dry_run_done"}
    if msg.startswith("connected to the receiver"):
        return {"event": "connected"}
    if msg.startswith("receiver sent to"):
        return {"event": "receiver"}
    if msg.startswith("installed: "):
        e = parse_entry(msg[len("installed: "):])
        return {"event": "installed", "entry": e} if e else None
    if msg == "nothing installed":
        return {"event": "installed", "entry": None}
    fit = parse_fit(msg)
    if fit:
        return {"event": "fit", "fit": fit}
    m = re.match(r"^  ((?:update|write|delete|drop|add|keep) .+)$", msg)
    if m:                       # a step of the plan (describe_steps), e.g. "  update GFSCATALOG"
        return {"event": "step", "text": m[1]}
    if re.match(r"^\S+META written: ", msg) or re.match(r"^\S+META is up to date", msg):
        return {"event": "meta_written"}
    if msg == "removed":
        return {"event": "removed"}
    return None


def parse_entry(text: str) -> dict | None:
    """A catalogue entry (gfs_catalog.Entry.describe):
    `TITLE[ NAME][ VERSION]: N containers P000.. (X GiB), index PINDEX[, INCOMPLETE]`."""
    m = ENTRY.match(text.strip())
    if not m:
        return None
    head = m["head"].split(" ")
    title, rest = head[0], head[1:]
    version = rest.pop() if rest and VERSION.match(rest[-1]) else ""
    return {"title": "" if title == "(untitled)" else title, "name": " ".join(rest), "version": version,
            "containers": int(m["containers"]), "prefix": m["prefix"], "gib": float(m["gib"]),
            "index": m["index"], "complete": not m["inc"]}


def parse_library(text: str) -> dict:
    """gfs_install.py --list: the titles installed on the console."""
    count, entries, catalogue = None, [], True
    for line in text.splitlines():
        msg = unstamp(line)
        m = TITLES_LINE.match(msg)
        if m:
            count = int(m[1])
            catalogue = "no GFSCATALOG" not in msg
            continue
        if count is not None and msg.startswith("  "):
            e = parse_entry(msg)
            if e:
                entries.append(e)
    return {"ok": count is not None, "count": count or 0, "entries": entries, "catalogue": catalogue}


# ---- run.py boot ----

BOOT_STAGES = ["sending", "core", "extension", "title", "starting"]
CONSOLE_LINE = re.compile(r"^(\d\d:\d\d:\d\d) (?:\[ghostldr \d+\] )?(.*)$")


def parse_boot_line(line: str) -> dict | None:
    """One run.py boot line, as an event for the Play screen."""
    m = re.match(r"^\s+\[hint\] (.*)$", line)
    if m:
        return {"event": "hint", "text": m[1]}
    if line.startswith("preflight: "):
        msg = line[len("preflight: "):]
        if msg.endswith("skipped"):
            return {"event": "preflight", "state": "skipped", "text": msg}
        if "imports would bind to stubs" in msg:
            return {"event": "preflight", "state": "unresolved", "text": msg}
        if msg.startswith("no terminal to ask on") or msg == "boot cancelled":
            return {"event": "preflight", "state": "stopped", "text": msg}
        return {"event": "preflight", "state": "info", "text": msg}
    m = re.match(r"^sending (\d+) bytes of Lua to (\S+):9026$", line)
    if m:
        return {"event": "stage", "stage": "sending"}
    if line.startswith("serving the extension"):
        return {"event": "serving", "what": "extension"}
    if line.startswith("serving the game selector"):
        return {"event": "serving", "what": "selector"}
    if re.match(r"^extension: sent \d+ bytes", line):
        return {"event": "stage", "stage": "extension"}
    if re.match(r"^selector: sent \d+ bytes", line):
        return {"event": "selector"}
    if line.startswith("extension: the console did not fetch it"):
        return {"event": "problem", "text": line}
    if line.startswith("listening for the console's log"):
        return {"event": "listening"}
    m = CONSOLE_LINE.match(line)
    if not m:
        return None
    msg = m[2]
    if "ghostldr core at " in msg:
        return {"event": "stage", "stage": "core"}
    t = re.match(r"^ghostfs: ([A-Z0-9_-]+), (\d+) entries", msg)
    if t:
        return {"event": "stage", "stage": "title", "title": t[1]}
    if msg.startswith("main thread: starting") or msg.startswith("calling title entry"):
        return {"event": "stage", "stage": "starting"}
    if msg.startswith("CRASH") and "report end" not in msg:
        return {"event": "crash", "text": msg}
    return {"event": "log"}


# ---- manager.py ----

def parse_manager(text: str) -> dict:
    out = {"url": "", "token": "", "running": False, "stopped": False}
    for line in text.splitlines():
        m = re.match(r"^\s+open\s+(http://[0-9.]+:\d+/)\s*$", line)
        if m:
            out.update(url=m[1], running=True)
            continue
        m = re.match(r"^\s+token\s+(\S+)", line)
        if m:
            out["token"] = m[1]
            continue
        msg = unstamp(line)
        m = re.match(r"^a manager is already running: (http://[0-9.]+:\d+/)", msg)
        if m:
            out.update(url=m[1], running=True)
        elif msg.startswith("stopping the manager on"):
            out.update(stopped=msg.endswith("done"), running=False)
        elif msg.startswith("no manager answers"):
            out.update(running=False, stopped=True)
    return out


# ---- profiles.py detect ----

DETECT = re.compile(r"^title (\S+) \(param\.sfo TITLE_ID (\S+), APP_VER (\S+)\), eboot fingerprint (.+?)"
                    r"(?:, module (\S+))?$")


def parse_detect(text: str) -> dict | None:
    for line in text.splitlines():
        m = DETECT.match(line.strip())
        if m:
            def opt(v: str | None) -> str:
                return "" if v in (None, "-", "?") else v
            return {"title": opt(m[1]), "sfo_title": opt(m[2]), "version": opt(m[3]),
                    "fingerprint": opt(m[4]), "module": opt(m[5]),
                    "homebrew": "'s generic one" in (m[4] or "")}
    return None
