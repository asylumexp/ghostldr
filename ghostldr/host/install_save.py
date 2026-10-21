#!/usr/bin/env python3
"""Install Luac0re 2.4b (+ ghostldr auto.lua) into the running SWRR's own save.

Writing into the save that SWRR has mounted avoids resigning: the image and
sealed key stay the console's own.  Steps:
  backup   copy every file of the mounted save to build/swrr-save/<stamp>/
  install  import the exploit PSU into VMC0.card and upload lua/ files
"""

from __future__ import annotations

import argparse
import ftplib
import io
import shutil
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LUAC0RE = ROOT / "build/luac0re-src"
PSU_DIR = ROOT / "build/luac0re-save/ps2"
from console import console_ip
from swrr import by_title, running

HOST = console_ip()


def ftp_connect() -> ftplib.FTP:
    f = ftplib.FTP()
    f.connect(HOST, 2121, timeout=30)
    f.login()
    f.voidcmd("TYPE I")
    return f


def walk(f: ftplib.FTP, path: str) -> list[tuple[str, int]]:
    out: list[tuple[str, int]] = []
    lines: list[str] = []
    f.retrlines(f"LIST {path}", lines.append)
    for line in lines:
        parts = line.split(None, 8)
        if len(parts) < 9 or parts[8] in (".", ".."):
            continue
        name, size = parts[8], int(parts[4])
        full = f"{path}/{name}"
        if line.startswith("d"):
            out += walk(f, full)
        else:
            out.append((full, size))
    return out


def save_root(title: str) -> str:
    return f"/mnt/sandbox/{title}_000/savedata0"


def backup(title: str) -> Path:
    f = ftp_connect()
    root = save_root(title)
    files = walk(f, root)
    dest = ROOT / "build/swrr-save" / time.strftime("%Y%m%d-%H%M%S")
    for path, size in files:
        local = dest / path[len(root) + 1:]
        local.parent.mkdir(parents=True, exist_ok=True)
        with local.open("wb") as fh:
            f.retrbinary(f"RETR {path}", fh.write)
        print(f"backup {path} {size}")
    f.close()
    print(f"saved {len(files)} files to {dest}")
    return dest


def install(title: str, backup_dir: Path, extra_lua: list[Path]) -> None:
    serial = by_title(title)["ps2"]
    card = backup_dir / "VMC0.card"
    if not card.exists():
        raise SystemExit(f"{card} missing: the save layout is not what Luac0re expects")
    work = backup_dir.parent / (backup_dir.name + "-luac0re")
    if work.exists():
        shutil.rmtree(work)
    shutil.copytree(backup_dir, work)
    psu = PSU_DIR / f"{serial}.psu"
    subprocess.run(["mymcplus", str(work / "VMC0.card"), "delete", serial], check=False)
    subprocess.run(["mymcplus", str(work / "VMC0.card"), "import", str(psu)], check=True)
    subprocess.run(["mymcplus", str(work / "VMC0.card"), "dir"], check=False)

    uploads = {"VMC0.card": work / "VMC0.card"}
    for p in sorted((LUAC0RE / "lua").glob("*.lua")):
        uploads[f"lua/{p.name}"] = p
    for p in sorted((LUAC0RE / "lua/jit").glob("*.lua")):
        uploads[f"lua/jit/{p.name}"] = p
    for p in extra_lua:
        uploads[f"lua/{p.name}"] = p

    f = ftp_connect()
    root = save_root(title)
    for d in ("lua", "lua/jit"):
        try:
            f.mkd(f"{root}/{d}")
        except ftplib.error_perm:
            pass
    for rel, local in uploads.items():
        with local.open("rb") as fh:
            f.storbinary(f"STOR {root}/{rel}", fh)
        print(f"uploaded {rel} {local.stat().st_size}")
    f.close()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("step", choices=["list", "backup", "install"])
    ap.add_argument("--title", help="CUSA03474 (US) or CUSA03492 (EU); default: the running SWRR")
    ap.add_argument("--backup-dir", type=Path)
    ap.add_argument("--extra-lua", type=Path, action="append", default=[])
    args = ap.parse_args()
    if not args.title:
        f = ftp_connect()
        args.title = running(f)["title"]
        f.close()
    if args.step == "list":
        f = ftp_connect()
        for path, size in walk(f, save_root(args.title)):
            print(size, path)
        return 0
    if args.step == "backup":
        backup(args.title)
        return 0
    install(args.title, args.backup_dir or backup(args.title), args.extra_lua)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
