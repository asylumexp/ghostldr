#!/usr/bin/env python3
"""Community memory plans on this computer (docs/community-plans.md).

plans/index.json in the GitHub repository lists the plans players shared;
sync() keeps a copy of the ones this release doesn't carry in build/plans/
(the override folder).  seeds() is what profiles.py puts in the boot bundle
as REC_SEED records: the release's plans/, and the downloaded ones when this
boot's sync() reached GitHub.  Any GitHub error, a rate limit included, is
silent: the boot just gets no downloaded plans.  (On the console the game
selector does the same itself, and shares the console's own: plansync/.)

    python3 ghostldr/host/plans.py sync      # refresh build/plans/ from GitHub
    python3 ghostldr/host/plans.py list      # the plans a boot would carry
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RELEASE_DIR = ROOT / "plans"                       # the repository's plans/, as the release carries it
OVERRIDE_DIR = ROOT / "build/plans"                # downloaded plans the release doesn't have
REPO = os.environ.get("GHOSTLDR_PLANS_REPO", "asylumexp/ghostldr")
BRANCH = "main"
INDEX_API = f"https://api.github.com/repos/{REPO}/contents/plans/index.json?ref={BRANCH}"
RAW = f"https://raw.githubusercontent.com/{REPO}/{BRANCH}/"
TIMEOUT = 6

# struct am_plan (automem.c, AM_VERSION 1)
PLAN_MAGIC, PLAN_VERSION, PLAN_SIZE = 0x314D454D52444C47, 1, 1080
PLATFORMS = {"ps5": 0, "ps4": 1}
FP_RE = re.compile(r"[0-9a-f]{40}")
TITLE_RE = re.compile(r"[A-Z0-9_-]{1,15}")


def plan_ok(data: bytes, fingerprint: str | None = None) -> bool:
    """A struct am_plan this ghostldr reads, for that build when given."""
    if len(data) != PLAN_SIZE:
        return False
    magic, version, size = struct.unpack_from("<QII", data)
    return (magic, version, size) == (PLAN_MAGIC, PLAN_VERSION, PLAN_SIZE) and \
        (fingerprint is None or data[16:36].hex() == fingerprint)


def plan_info(data: bytes) -> dict:
    """The numbers a person looks at: boots, told, peak, out of memory, minutes."""
    boots, phys, told, peak, demand, oom, _, _, alive = struct.unpack_from("<9I", data, 36)
    return {"boots": boots, "phys_mib": phys, "told_mib": told, "peak_mib": peak, "demand_mib": demand,
            "oom": oom, "alive_min": alive}


# ---- down: GitHub -> build/plans/ ----

def _read_index(folder: Path) -> list[dict]:
    try:
        plans = json.loads((folder / "index.json").read_text()).get("plans", [])
    except (OSError, ValueError, AttributeError):
        return []
    out = []
    for p in plans if isinstance(plans, list) else []:
        if isinstance(p, dict) and TITLE_RE.fullmatch(str(p.get("title", ""))) and \
                FP_RE.fullmatch(str(p.get("fingerprint", ""))) and p.get("platform") in PLATFORMS:
            out.append(p)
    return out


def _key(p: dict) -> tuple[str, str]:
    return p["fingerprint"], p["platform"]


def _get(url: str, headers: dict[str, str]) -> tuple[int, bytes, dict]:
    req = urllib.request.Request(url, headers={"User-Agent": "ghostldr", **headers})
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, b"", dict(e.headers or {})


def sync() -> bool:
    """Refresh build/plans/ from GitHub.  True when GitHub answered (the
    downloaded plans may be offered), False on any error, a rate limit
    included: then nothing is said and none are offered this time."""
    try:
        return _sync()
    except Exception:  # noqa: BLE001 - a plan download must never stop a boot
        return False


def _sync() -> bool:
    OVERRIDE_DIR.mkdir(parents=True, exist_ok=True)
    etag_file = OVERRIDE_DIR / ".etag"
    headers = {"Accept": "application/vnd.github.raw+json"}
    etag = etag_file.read_text().strip() if etag_file.exists() else ""
    if etag and (OVERRIDE_DIR / "index.json").exists():
        headers["If-None-Match"] = etag    # a 304 costs nothing of the rate limit
    status, body, hdrs = _get(INDEX_API, headers)
    if status == 304:
        return True
    if status != 200:          # 403/429: the rate limit; 404: no plans yet; anything else: offline
        return False
    index = json.loads(body)
    plans = [p for p in index.get("plans", []) if isinstance(p, dict)]
    have = {_key(p): p for p in _read_index(RELEASE_DIR)}
    keep = []
    for p in plans:
        try:
            if not (TITLE_RE.fullmatch(p["title"]) and FP_RE.fullmatch(p["fingerprint"]) and
                    p["platform"] in PLATFORMS and re.fullmatch(r"[0-9a-f]{64}", p["sha256"])):
                continue
        except (KeyError, TypeError):
            continue
        rel = have.get(_key(p))
        if rel and rel.get("sha256") == p["sha256"]:
            continue           # the release has this one
        dest = OVERRIDE_DIR / p["title"] / f"{p['fingerprint']}-{p['platform']}.bin"
        if not (dest.exists() and hashlib.sha256(dest.read_bytes()).hexdigest() == p["sha256"]):
            st, data, _ = _get(RAW + f"plans/{p['title']}/{p['fingerprint']}-{p['platform']}.bin", {})
            if st != 200 or hashlib.sha256(data).hexdigest() != p["sha256"] or not plan_ok(data, p["fingerprint"]):
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
        keep.append({k: p[k] for k in ("title", "fingerprint", "platform", "sha256", "contributors", "boots",
                                       "updated") if k in p})
    (OVERRIDE_DIR / "index.json").write_text(json.dumps({"version": 1, "plans": keep}, indent=1) + "\n")
    if hdrs.get("ETag") or hdrs.get("Etag"):
        etag_file.write_text(hdrs.get("ETag") or hdrs.get("Etag"))
    return True


def seeds(downloaded: bool) -> dict[str, list[dict]]:
    """{title: [{fingerprint, platform, contributors, data}]}: the release's
    plans, then (downloaded True) the ones in build/plans/, which replace a
    release plan for the same build only when they're newer."""
    found: dict[tuple[str, str], dict] = {}
    for folder in [RELEASE_DIR] + ([OVERRIDE_DIR] if downloaded else []):
        for p in _read_index(folder):
            path = folder / p["title"] / f"{p['fingerprint']}-{p['platform']}.bin"
            try:
                data = path.read_bytes()
            except OSError:
                continue
            if not plan_ok(data, p["fingerprint"]):
                continue
            old = found.get(_key(p))
            if old and str(old.get("updated", "")) >= str(p.get("updated", "")):
                continue
            found[_key(p)] = {**p, "data": data}
    out: dict[str, list[dict]] = {}
    for p in found.values():
        out.setdefault(p["title"], []).append(p)
    for v in out.values():
        v.sort(key=lambda p: (p["platform"], p["fingerprint"]))
    return out


# ---- command line ----

def main() -> int:
    ap = argparse.ArgumentParser(description="community memory plans (docs/community-plans.md)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("sync", help="refresh build/plans/ from GitHub")
    sub.add_parser("list", help="the plans a boot would carry")
    args = ap.parse_args()
    if args.cmd == "sync":
        ok = sync()
        print("plans: build/plans/ is up to date with GitHub" if ok else "plans: GitHub didn't answer; nothing changed")
        return 0
    got = seeds(downloaded=True)
    for title in sorted(got):
        for p in got[title]:
            info = plan_info(p["data"])
            print(f"{title} {p['platform']} {p['fingerprint']}: from {p.get('contributors', 1)} consoles, "
                  f"boot {info['boots']}, told {info['told_mib']} MiB")
    if not got:
        print("plans: none")
    return 0


if __name__ == "__main__":
    sys.exit(main())
