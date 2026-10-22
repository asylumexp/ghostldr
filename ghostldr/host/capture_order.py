#!/usr/bin/env python3
"""Turn the file opens in a boot log into an open-order file for gfs_pack.py.

The core logs the first open of every /app0 file: "gfs open /app0/..." when
the title runs from ghostfs, "open /app0/... -> fd" otherwise (every open with
the trace_opens feature, run.py --trace-opens; the first 400 without).  This
lists the files in the order the selected boots first opened them, skipping
what the loader itself reads (eboot.bin, sce_module/).  Play the parts of the
title you care about, then:

    python3 ghostldr/host/run.py boot --app-root ghostfs --title CUSA12345 --trace-opens   # then play
    python3 ghostldr/host/capture_order.py --title CUSA12345 -o ghostldr/data/titles/CUSA12345-open-order.txt
    python3 ghostldr/host/gfs_pack.py --src DIR --title CUSA12345 \
        --order ghostldr/data/titles/CUSA12345-open-order.txt    # or "order" in its profile's "pack"

Then reinstall (the layout changes, so gfs_install.py resends the title's
containers; other titles are not touched).
"""

from __future__ import annotations

import argparse
import re
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LOG = ROOT / "build/ghostldr/run.log"

BOOT = re.compile(r"\] ghostldr core at ")
TITLE = re.compile(r"\] (?:profile|ghostfs): ([A-Z0-9_-]{1,15})[ ,]")
GFS_OPEN = re.compile(r"\] gfs open /app0/(\S+) \(")
OPEN = re.compile(r"\] open /app0/(\S+) \(.*\) flags [0-9a-f]+ -> ([0-9a-f]+)$")


def boots(lines) -> list[dict]:
    """Each boot in the log: its title (if it said) and its /app0 opens in order."""
    out: list[dict] = []
    cur = None
    for line in lines:
        line = line.rstrip("\n")
        if BOOT.search(line):
            cur = {"title": None, "opens": []}
            out.append(cur)
            continue
        if cur is None:
            continue
        m = TITLE.search(line)
        if m and not cur["title"]:
            cur["title"] = m.group(1)
            continue
        m = GFS_OPEN.search(line)
        if m:
            cur["opens"].append(m.group(1))
            continue
        m = OPEN.search(line)
        if m and int(m.group(2), 16) < 0x80000000:       # negative fds are logged as 8xxxxxxx
            cur["opens"].append(m.group(1))
    return out


def loader_file(rel: str) -> bool:
    return rel == "eboot.bin" or rel.startswith("sce_module/")


def merge(selected: list[dict]) -> list[str]:
    seen: set[str] = set()
    order = []
    for b in selected:
        for rel in b["opens"]:
            rel = re.sub(r"/+", "/", rel).strip("/")
            if rel and rel not in seen and not loader_file(rel):
                seen.add(rel)
                order.append(rel)
    return order


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--log", type=Path, default=LOG, help="boot log (default build/ghostldr/run.log)")
    ap.add_argument("--title", help="only boots of this title")
    ap.add_argument("--last", type=int, default=0, help="only the last N matching boots (default: all)")
    ap.add_argument("-o", "--out", type=Path, help="write the order file here (default: print it)")
    args = ap.parse_args()
    with args.log.open(errors="replace") as f:
        found = boots(f)
    title = args.title.upper() if args.title else None
    selected = [b for b in found if not title or b["title"] in (title, None)]
    if args.last:
        selected = selected[-args.last:]
    order = merge(selected)
    if not order:
        raise SystemExit(f"no /app0 opens in {len(selected)} boot(s) of {args.log}")
    head = (f"# {title or 'title'} files in the order {len(selected)} boot(s) first opened them "
            f"(capture_order.py, {time.strftime('%Y-%m-%d')})")
    text = "\n".join([head, *order, ""])
    if args.out:
        args.out.write_text(text)
        print(f"{args.out}: {len(order)} files from {len(selected)} boot(s)")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
