#!/usr/bin/env python3
"""The daily job's half of community memory plans (docs/community-plans.md):
pull what the collection server (server.py) received since the last run,
pick one plan per eboot fingerprint and platform, and write the repository's
plans/ folder.  It does not commit; the job that runs it does.

    python3 tools/contrib/sync_plans.py                 # pull, write plans/
    python3 tools/contrib/sync_plans.py --dry-run       # say what it would write
    python3 tools/contrib/sync_plans.py --server http://plans-host:8790 --repo ~/ghostldr

The server keeps one plan per install, fingerprint and platform; this keeps
the same in its state file (~/.config/ghostldr/contrib-sync.json), with the
last submission id it saw, so each run pulls only what is new and still
chooses among every install's latest plan.  For each fingerprint and
platform the chosen plan is, in order:

  - one that did not run out of memory (oom, u32 at 56, is 0) and stayed up
    at least 2 minutes (alive_min, u32 at 68);
  - then the one with the most boots (u32 at 36);
  - then the latest.

`contributors` is the number of installs that ever sent a plan for that
fingerprint and platform.  Files are rewritten only when their bytes change.
The admin key is read from ~/.config/ghostldr/contrib-admin.key
(--admin-key-file); the server from --server, else GHOSTLDR_CONTRIB_SERVER or
~/.config/ghostldr/contrib-server.txt (its address on this network), else the
consoles' own GHOSTLDR_CONTRIB_URL or ~/.config/ghostldr/contrib-url.txt.
"""

from __future__ import annotations

import argparse
import base64
import datetime
import hashlib
import json
import os
import struct
import sys
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import server as contract        # noqa: E402  (the same checks the server makes)

CONFIG = Path.home() / ".config" / "ghostldr"
def default_server() -> str:
    cfg = Path.home() / ".config/ghostldr"
    for env, name in (("GHOSTLDR_CONTRIB_SERVER", "contrib-server.txt"), ("GHOSTLDR_CONTRIB_URL", "contrib-url.txt")):
        if os.environ.get(env):
            return os.environ[env].strip()
        try:
            return (cfg / name).read_text().strip()
        except OSError:
            pass
    return ""
REPO = Path(__file__).resolve().parents[2]
MIN_ALIVE = 2                                    # minutes


def plan_stats(plan: bytes) -> tuple[int, int, int]:
    """(boots, oom, alive_min) from a struct am_plan."""
    boots, = struct.unpack_from("<I", plan, 36)
    oom, = struct.unpack_from("<I", plan, 56)
    alive, = struct.unpack_from("<I", plan, 68)
    return boots, oom, alive


def rank(cand: dict) -> tuple:
    """Higher is better: a healthy run, then boots, then the latest."""
    boots, oom, alive = plan_stats(base64.b64decode(cand["plan"]))
    return (oom == 0 and alive >= MIN_ALIVE, boots, cand["id"])


def pull(server: str, key: str, after: int) -> list[dict]:
    """Every submission with an id above `after`, oldest first."""
    out: list[dict] = []
    while True:
        req = urllib.request.Request(f"{server.rstrip('/')}/v1/submissions?after={after}",
                                     headers={"Authorization": f"Bearer {key}"})
        with urllib.request.urlopen(req, timeout=60) as r:
            page = json.load(r)
        rows = page.get("submissions", [])
        if not rows:
            return out
        out += rows
        after = max(after, int(page.get("last", rows[-1]["id"])))


def load_json(path: Path, default: dict) -> dict:
    try:
        return json.loads(path.read_text())
    except FileNotFoundError:
        return default


def write_if_changed(path: Path, data: bytes, dry_run: bool) -> bool:
    if path.is_file() and path.read_bytes() == data:
        return False
    if dry_run:
        print(f"would write {path}")
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_name(path.name + ".tmp")
        tmp.write_bytes(data)
        os.replace(tmp, path)
    return True


def sync(server: str, key: str, repo: Path, state_path: Path, dry_run: bool = False) -> str:
    """One run; returns the summary line."""
    state = load_json(state_path, {"version": 1, "last": 0, "plans": {}})
    rows = pull(server, key, int(state["last"]))
    if not rows:
        return "no new submissions"

    # keep each install's latest plan per fingerprint and platform
    touched: set[str] = set()
    skipped = 0
    for row in rows:
        state["last"] = max(int(state["last"]), int(row["id"]))
        try:
            sub = contract.check_submission(row)
        except contract.Invalid as e:
            skipped += 1
            print(f"skipping submission {row.get('id')}: {e}", file=sys.stderr)
            continue
        group = f"{sub['fingerprint']}-{sub['platform']}"
        state["plans"].setdefault(group, {})[sub["install"]] = {
            "id": int(row["id"]), "received": str(row.get("received", "")), "title": sub["title"],
            "plan": base64.b64encode(sub["plan"]).decode()}
        touched.add(group)

    index_path = repo / "plans" / "index.json"
    index = load_json(index_path, {"version": 1, "plans": []})
    entries = {f"{e['fingerprint']}-{e['platform']}": e for e in index.get("plans", [])}
    added, updated = [], []
    for group in sorted(touched):
        cands = state["plans"][group]
        best = max(cands.values(), key=rank)
        fingerprint, platform = group.rsplit("-", 1)
        plan = base64.b64decode(best["plan"])
        rel = f"plans/{best['title']}/{group}.bin"
        old = entries.get(group)
        if old and old["path"] != rel and not dry_run:            # the build was filed under another title
            (repo / old["path"]).unlink(missing_ok=True)
        wrote = write_if_changed(repo / rel, plan, dry_run)
        entry = {"title": best["title"], "fingerprint": fingerprint, "platform": platform, "path": rel,
                 "sha256": hashlib.sha256(plan).hexdigest(), "contributors": len(cands),
                 "boots": plan_stats(plan)[0], "updated": best["received"][:10] or
                 datetime.date.today().isoformat()}
        if old is None:
            added.append(entry)
        elif wrote or old != entry:
            updated.append(entry)
        entries[group] = entry

    index = {"version": 1, "plans": sorted(entries.values(), key=lambda e: (e["title"], e["fingerprint"],
                                                                          e["platform"]))}
    write_if_changed(index_path, (json.dumps(index, indent=2) + "\n").encode(), dry_run)
    if not dry_run:
        state_path.parent.mkdir(parents=True, exist_ok=True)
        write_if_changed(state_path, (json.dumps(state, indent=1, sort_keys=True) + "\n").encode(), False)

    def names(es: list[dict]) -> str:
        return ", ".join(f"{e['title']} {e['platform']}" for e in es)

    parts = [f"{len(added)} new plan{'s' * (len(added) != 1)}", f"{len(updated)} updated"]
    summary = ", ".join(parts)
    changed = names(added + updated)
    if changed:
        summary += f": {changed}"
    if skipped:
        summary += f" ({skipped} invalid skipped)"
    return ("dry run: " if dry_run else "") + summary


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--server", default=default_server())
    ap.add_argument("--repo", type=Path, default=REPO, help="the ghostldr checkout to write plans/ into")
    ap.add_argument("--admin-key-file", type=Path, default=CONFIG / "contrib-admin.key")
    ap.add_argument("--state", type=Path, default=CONFIG / "contrib-sync.json")
    ap.add_argument("--dry-run", action="store_true", help="print what would be written, change nothing")
    args = ap.parse_args(argv)
    if not args.server:
        raise SystemExit("sync_plans: no server: --server, or ~/.config/ghostldr/contrib-server.txt")
    key = args.admin_key_file.read_text().strip()
    try:
        print(sync(args.server, key, args.repo, args.state, args.dry_run))
    except urllib.error.HTTPError as e:
        print(f"sync_plans: {args.server}: HTTP {e.code}", file=sys.stderr)
        return 1
    except (urllib.error.URLError, OSError) as e:
        print(f"sync_plans: {args.server}: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
