#!/usr/bin/env python3
"""Aggregate the stub hits in a ghostldr run log.

The core binds every import it cannot resolve to a logging stub
(native/src/resolve.c, c_unresolved). Each call logs one of:

    UNRESOLVED call: <lib> <nid> (<name>)
    UNRESOLVED call: <lib> <nid> (<name>) -> <ret>
    DENIED call: <lib> <nid> (<name>) -> <ret>
    UNRESOLVED call #<idx>           (stub index out of range)

A stub returning 0 looks like success to the title, so the raw log
is the only place these show up. This tool counts them per
(library, function) so HLE work can be prioritised by what titles
actually call, and so a boot's silent-stub surface is one number
instead of a log to grep.

    python3 ghostldr/tools/stub_report.py build/ghostldr/run.log
    python3 ghostldr/tools/stub_report.py run.log --top 10 --json stubs.json

Log lines written by run.py's listener carry an "HH:MM:SS " stamp;
both stamped and raw lines are accepted.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

RICH = re.compile(
    r"^(DENIED|UNRESOLVED) call: (\S+) (\S+) \((.*?)\)(?: -> ([0-9a-fA-Fx]+))?$")
BARE = re.compile(r"^UNRESOLVED call #(\d+)$")
# run.py writes "HH:MM:SS [ghostldr N] text"; older logs have no [ghostldr N]
STAMP = re.compile(r"^\d{2}:\d{2}:\d{2} (?:\[ghostldr \d+\] )?(.*)$")


class Stats:
    def __init__(self) -> None:
        self.rows: dict[tuple[str, str, str, str], dict] = {}
        self.bare = 0          # "UNRESOLVED call #<idx>" lines
        self.stub_lines = 0    # all stub log lines seen
        self.total_lines = 0

    def add(self, kind: str, lib: str, nid: str, name: str, ret: str | None) -> None:
        key = (kind, lib, nid, name)
        row = self.rows.setdefault(key, {"count": 0, "rets": set()})
        row["count"] += 1
        if ret is not None:
            row["rets"].add(ret)
        self.stub_lines += 1

    def ranked(self) -> list[dict]:
        out = []
        for (kind, lib, nid, name), row in self.rows.items():
            out.append({"kind": kind, "lib": lib, "nid": nid, "name": name,
                        "count": row["count"], "rets": sorted(row["rets"])})
        out.sort(key=lambda r: (-r["count"], r["lib"], r["name"]))
        return out

    def as_dict(self, top: int) -> dict:
        ranked = self.ranked()
        return {
            "total_lines": self.total_lines,
            "stub_lines": self.stub_lines,
            "distinct_stubs": len(ranked),
            "denied_hits": sum(r["count"] for r in ranked if r["kind"] == "DENIED"),
            "unnamed_hits": self.bare,
            "top": ranked[:top],
        }


def parse_lines(lines) -> Stats:
    st = Stats()
    for raw in lines:
        st.total_lines += 1
        line = raw.rstrip("\n")
        m = STAMP.match(line)
        if m:
            line = m.group(1)
        m = RICH.match(line)
        if m:
            kind, lib, nid, name, ret = m.groups()
            st.add(kind, lib, nid, name, ret)
            continue
        if BARE.match(line):
            st.bare += 1
            st.stub_lines += 1
    return st


def render(st: Stats, top: int) -> str:
    d = st.as_dict(top)
    out = [
        f"stub hits: {d['stub_lines']} across {d['distinct_stubs']} distinct stubs "
        f"({d['denied_hits']} denied, {d['unnamed_hits']} unnamed-index) "
        f"in {d['total_lines']} log lines",
        "",
        f"{'hits':>7}  {'kind':<10} {'library':<28} {'function':<40} nid / returns",
    ]
    for r in d["top"]:
        rets = ",".join(r["rets"]) if r["rets"] else "-"
        out.append(f"{r['count']:>7}  {r['kind']:<10} {r['lib']:<28} "
                   f"{r['name']:<40} {r['nid']} / {rets}")
    if len(st.rows) > top:
        out.append(f"  ... and {len(st.rows) - top} more (raise --top, or use --json)")
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("log", type=Path, help="a run log (build/ghostldr/run.log)")
    ap.add_argument("--top", type=int, default=30, help="rows to print (default 30)")
    ap.add_argument("--json", type=Path, help="write the full ranking as JSON here")
    args = ap.parse_args(argv)
    if not args.log.exists():
        raise SystemExit(f"no such log: {args.log}")
    st = parse_lines(args.log.read_text(errors="replace").splitlines(keepends=True))
    sys.stdout.write(render(st, args.top))
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(st.as_dict(len(st.rows) or 1), indent=1) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
