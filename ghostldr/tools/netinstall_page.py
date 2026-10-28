#!/usr/bin/env python3
"""Keep the network installer's page and its copies in step.

ghostldr/data/netinstall.html is the page a browser installs games with
(docs/manager.md "Install a game"): its JavaScript packs a game's folder or
.zip the way gfs_pack.py does and installs it the way gfs_install.py does,
through the console's /api/install/ requests.  Two servers send it:
manager.lua (Luac0re, standalone) and the extension's netinst.c (the game
selector's Settings).  This tool

  - writes the PROFILES block of the page from ghostldr/data/titles/*.json:
    each title's name, version and "pack" rules (prefix, exclude, groups,
    late, the open order), with Python's scoped (?i:...) groups spelled out
    for browsers without regexp modifiers;
  - copies the page into manager.lua (between its INSTALL_PAGE markers) and
    the pump blob (ghostldr/data/gfs_pump.bin) into its PUMP_HEX line;
  - writes ghostldr/data/netinstall.html.gz, the page as the game
    selector's server sends it (native/netinst/netinst_sel.c includes it:
    the selector is built without Python's help for that file).

    python3 ghostldr/tools/netinstall_page.py           # update
    python3 ghostldr/tools/netinstall_page.py --check   # exit 1 when anything is out of step (the tests use it)

"""

from __future__ import annotations

import argparse
import gzip
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PAGE = ROOT / "ghostldr/data/netinstall.html"
TITLES = ROOT / "ghostldr/data/titles"
MANAGER = ROOT / "ghostldr/lua/manager.lua"
PUMP = ROOT / "ghostldr/data/gfs_pump.bin"
PAGE_GZ = ROOT / "ghostldr/data/netinstall.html.gz"
PROF_RE = re.compile(r"(/\*PROFILES-BEGIN[^\n]*\*/\n)(.*?)(/\*PROFILES-END\*/)", re.S)
PAGE_RE = re.compile(r"(M\.INSTALL_PAGE = \[(=*)\[\n)(.*?)(\]\2\])", re.S)
PUMP_RE = re.compile(r'(local PUMP_HEX = ")([0-9a-f]*)(")')


def _fold_class(body: str) -> str:
    """A character class's body with the other case of its letters and
    letter ranges added at the end ("a-c_" -> "a-c_A-C")."""
    extra, i = [], 0
    while i < len(body):
        if body[i] == "\\":
            i += 2
            continue
        a = body[i]
        if i + 2 < len(body) and body[i + 1] == "-" and body[i + 2] != "\\":
            b = body[i + 2]
            if a.isascii() and b.isascii() and a.isalpha() and b.isalpha() and a.islower() == b.islower():
                extra.append(a.swapcase() + "-" + b.swapcase())
            i += 3
            continue
        if a.isascii() and a.isalpha():
            extra.append(a.swapcase())
        i += 1
    return body + "".join(extra)


def js_regex(rx: str) -> str:
    """Python's regex as JavaScript reads it: scoped (?i:...) groups become
    plain groups with each letter in both cases ([aA], [a-cA-C]); the rest is
    the same in both for the patterns the profiles use."""
    out, i = [], 0
    while i < len(rx):
        if rx.startswith("(?i:", i):
            depth, j, body = 1, i + 4, []
            while j < len(rx):
                ch = rx[j]
                if ch == "\\":
                    body.append(rx[j:j + 2])
                    j += 2
                    continue
                if ch == "[":
                    k = j + 1
                    if k < len(rx) and rx[k] == "^":
                        k += 1
                    if k < len(rx) and rx[k] == "]":
                        k += 1
                    while k < len(rx) and rx[k] != "]":
                        k += 2 if rx[k] == "\\" else 1
                    head = "[^" if rx[j + 1:j + 2] == "^" else "["
                    inner = rx[j + len(head):k]
                    body.append(head + _fold_class(inner) + "]")
                    j = k + 1
                    continue
                if ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                    if not depth:
                        break
                elif ch.isascii() and ch.isalpha():
                    ch = f"[{ch.lower()}{ch.upper()}]"
                body.append(ch)
                j += 1
            out.append("(?:" + "".join(body) + ")")
            i = j + 1
            continue
        if rx[i] == "\\":
            out.append(rx[i:i + 2])
            i += 2
            continue
        out.append(rx[i])
        i += 1
    js = "".join(out)
    if "(?" in js.replace("(?:", ""):
        raise SystemExit(f"regex {rx!r}: a construct JavaScript may not read")
    return js


def profiles() -> dict:
    out = {}
    for f in sorted(TITLES.glob("*.json")):
        if f.stem == "default":
            continue
        d = json.loads(f.read_text())
        p = {k: d[k] for k in ("name", "version") if d.get(k)}
        pk = d.get("pack") or {}
        if pk:
            q = {}
            if pk.get("prefix"):
                q["prefix"] = pk["prefix"]
            if pk.get("exclude"):
                q["exclude"] = js_regex(pk["exclude"])
            if pk.get("groups"):
                q["groups"] = [[js_regex(rx), int(rank)] for rx, rank in pk["groups"]]
            if pk.get("late"):
                q["late"] = list(pk["late"])
            if pk.get("order"):
                lines = (TITLES / pk["order"]).read_text().splitlines()
                q["order"] = [s.strip() for s in lines if s.strip() and not s.strip().startswith("#")]
            p["pack"] = q
        if p:
            out[f.stem] = p
    return out


def profiles_block() -> str:
    return "const PROFILES = " + json.dumps(profiles(), separators=(",", ":"), ensure_ascii=False) + ";\n"


def page_text() -> str:
    s = PAGE.read_text()
    m = PROF_RE.search(s)
    if not m:
        raise SystemExit(f"{PAGE}: no PROFILES block")
    return s[:m.start(2)] + profiles_block() + s[m.end(2):]


def manager_text(page: str) -> str:
    s = MANAGER.read_text()
    m = PAGE_RE.search(s)
    if not m:
        raise SystemExit(f"{MANAGER}: no M.INSTALL_PAGE block")
    level = "=" * 4
    while f"]{level}]" in page:
        level += "="
    s = s[:m.start()] + f"M.INSTALL_PAGE = [{level}[\n" + page + f"]{level}]" + s[m.end():]
    pm = PUMP_RE.search(s)
    if not pm:
        raise SystemExit(f"{MANAGER}: no PUMP_HEX line")
    return s[:pm.start(2)] + PUMP.read_bytes().hex() + s[pm.end(2):]


def gz_current(page: str) -> bool:
    try:
        return gzip.decompress(PAGE_GZ.read_bytes()) == page.encode()
    except (OSError, EOFError):
        return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="only check that the page and its copies are up to date")
    args = ap.parse_args()
    page = page_text()
    manager = manager_text(page)
    stale = [p for p, text in ((PAGE, page), (MANAGER, manager)) if p.read_text() != text]
    if not gz_current(page):
        stale.append(PAGE_GZ)
    if args.check:
        for p in stale:
            print(f"{p.relative_to(ROOT)} is out of step: run python3 ghostldr/tools/netinstall_page.py")
        return 1 if stale else 0
    for p, text in ((PAGE, page), (MANAGER, manager)):
        if p.read_text() != text:
            p.write_text(text)
            print(f"updated {p.relative_to(ROOT)}")
    if not gz_current(page):
        PAGE_GZ.write_bytes(gzip.compress(page.encode(), 9, mtime=0))
        print(f"updated {PAGE_GZ.relative_to(ROOT)} ({PAGE_GZ.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
