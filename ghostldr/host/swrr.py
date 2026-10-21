"""Star Wars Racer Revenge, US and EU: title IDs, save names, and which one runs.

Both regions run the same executable (its code is byte-identical, so the
loader's offsets hold for either).  What differs is the title ID, the Luac0re
save's directory and the PS2 exploit save on its memory card.  Each region
keeps its own saved data: a ghostfs install, Ghost's progress and Luac0re
saves belong to the region they were made in.
"""

from __future__ import annotations

import ftplib

REGIONS = {
    "us": {"title": "CUSA03474", "dir": "SLUS-20268", "temp": "SLUS-RESGN", "ps2": "BASLUS-20268"},
    "eu": {"title": "CUSA03492", "dir": "SLES-50366", "temp": "SLES-RESGN", "ps2": "BESLES-50366"},
}
SAVE_DIRS = {r["dir"] for r in REGIONS.values()}


def by_title(title: str) -> dict:
    for region in REGIONS.values():
        if region["title"] == title:
            return region
    raise SystemExit(f"{title} is not SWRR (US {REGIONS['us']['title']}, EU {REGIONS['eu']['title']})")


def ftp_list(f: ftplib.FTP, path: str) -> list[str]:
    """Names in a directory (LIST: the console's ftpsrv has no NLST)."""
    lines: list[str] = []
    f.retrlines(f"LIST {path}", lines.append)
    names = [line.split(None, 8)[-1] for line in lines if len(line.split(None, 8)) == 9]
    return [n for n in names if n not in (".", "..")]


def running(f: ftplib.FTP) -> dict:
    """The SWRR region that is running: only a running app has a /mnt/sandbox/<title>_000."""
    sandboxes = set(ftp_list(f, "/mnt/sandbox"))
    found = [r for r in REGIONS.values() if f"{r['title']}_000" in sandboxes]
    if len(found) != 1:
        raise SystemExit("start SWRR (US or EU) first" if not found else "both SWRR regions are running?")
    return found[0]
