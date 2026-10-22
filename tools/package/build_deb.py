#!/usr/bin/env python3
"""ghostldr_<version>_all.deb: the app for Debian and Ubuntu, with the system's Python.

    python3 tools/package/build_deb.py [--out DIR]

Pure Python (no dpkg-deb needed), so it runs on macOS too; resvg for the icon.
The package depends on python3 (>= 3.11): Debian 12+, Ubuntu 23.04+ (22.04
has 3.10: its users take the AppImage).

    /opt/ghostldr                    the release tree (app/, ghostldr/, build/ghostldr/...)
    /usr/bin/ghostldr                linux/ghostldr: python3 /opt/ghostldr/app/launch.py --workspace
    /usr/share/applications/ghostldr.desktop
    /usr/share/icons/hicolor/*/apps/ghostldr.{png,svg}
    /usr/share/doc/ghostldr/copyright

A .deb is an ar archive of debian-binary ("2.0"), control.tar.xz (control,
md5sums, prerm) and data.tar.xz, in that order.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common as C  # noqa: E402

DEPENDS = "python3 (>= 3.11)"
RECOMMENDS = "xdg-utils"
SUGGESTS = "zenity | kdialog"


def control(version: str, installed_kib: int) -> str:
    return (f"Package: {C.NAME}\n"
            f"Version: {version}\n"
            "Architecture: all\n"
            f"Maintainer: asylumexp <{C.HOMEPAGE}>\n"
            f"Installed-Size: {installed_kib}\n"
            f"Depends: {DEPENDS}\n"
            f"Recommends: {RECOMMENDS}\n"
            f"Suggests: {SUGGESTS}\n"
            "Section: utils\n"
            "Priority: optional\n"
            f"Homepage: {C.HOMEPAGE}\n"
            f"Description: {C.SUMMARY}\n"
            " ghostldr runs PS4 game dumps and OpenOrbis homebrew inside Star Wars Racer\n"
            " Revenge (SWRR) on a stock PS5, through Luac0re's userland exploit. This is\n"
            " its desktop app: find the console, check the setup, prepare and install a\n"
            " game, and play. It opens in the web browser.\n"
            " .\n"
            " You need a PS5 with SWRR and Luac0re, and your own game dump. No game files\n"
            " are included.\n")


def copyright_file(release: Path) -> str:
    def indent(text: str) -> str:
        return "\n".join(" " + (line if line.strip() else ".") for line in text.strip().splitlines())
    mit = (release / "LICENSE").read_text(encoding="utf-8")
    ofl = (release / "ghostldr/native/selector/font/OFL.txt").read_text(encoding="utf-8")
    return ("Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/\n"
            f"Upstream-Name: {C.NAME}\n"
            f"Source: {C.HOMEPAGE}\n\n"
            "Files: *\n"
            "Copyright: 2026 asylumexp\n"
            "License: MIT\n\n"
            "Files: opt/ghostldr/ghostldr/native/selector/font/* opt/ghostldr/build/ghostldr/selector.bin\n"
            "Copyright: 2022 The Noto Project Authors (https://github.com/notofonts/latin-greek-cyrillic)\n"
            "License: OFL-1.1\n\n"
            f"License: MIT\n{indent(mit)}\n\n"
            f"License: OFL-1.1\n{indent(ofl)}\n")


def build_root(root: Path, release: Path, pngs: dict) -> None:
    shutil.copytree(release, root / "opt/ghostldr", symlinks=True)
    (root / "usr/bin").mkdir(parents=True)
    shutil.copyfile(C.HERE / "linux/ghostldr", root / "usr/bin/ghostldr")
    (root / "usr/bin/ghostldr").chmod(0o755)
    (root / "usr/share/applications").mkdir(parents=True)
    shutil.copyfile(C.HERE / "linux/ghostldr.desktop", root / f"usr/share/applications/{C.NAME}.desktop")
    for size, png in pngs.items():
        d = root / f"usr/share/icons/hicolor/{size}x{size}/apps"
        d.mkdir(parents=True)
        shutil.copyfile(png, d / f"{C.NAME}.png")
    d = root / "usr/share/icons/hicolor/scalable/apps"
    d.mkdir(parents=True)
    shutil.copyfile(C.ICON_SVG, d / f"{C.NAME}.svg")
    doc = root / f"usr/share/doc/{C.NAME}"
    doc.mkdir(parents=True)
    (doc / "copyright").write_text(copyright_file(release), encoding="utf-8")


def tar_xz(entries: list, mtime: int) -> bytes:
    """entries: (archive name, local path or bytes, mode); root-owned, fixed time, sorted."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:xz", format=tarfile.GNU_FORMAT, preset=9) as t:
        for name, src, mode in entries:
            info = tarfile.TarInfo(name)
            info.uid = info.gid = 0
            info.uname = info.gname = "root"
            info.mtime = mtime
            info.mode = mode
            if src is None:
                info.type = tarfile.DIRTYPE
                t.addfile(info)
            elif isinstance(src, bytes):
                info.size = len(src)
                t.addfile(info, io.BytesIO(src))
            elif isinstance(src, Path) and src.is_symlink():
                info.type = tarfile.SYMTYPE
                info.linkname = os.readlink(src)
                t.addfile(info)
            else:
                info.size = src.stat().st_size
                with open(src, "rb") as f:
                    t.addfile(info, f)
    return buf.getvalue()


def data_entries(root: Path) -> tuple:
    """(tar entries, md5sums text, installed size in KiB)."""
    entries = [("./", None, 0o755)]
    md5s = []
    kib = 0
    for p in sorted(root.rglob("*"), key=lambda p: p.relative_to(root).as_posix()):
        rel = p.relative_to(root).as_posix()
        if p.is_dir() and not p.is_symlink():
            entries.append((f"./{rel}/", None, 0o755))
            kib += 1
        elif p.is_symlink():
            entries.append((f"./{rel}", p, 0o777))
        else:
            mode = 0o755 if p.stat().st_mode & 0o111 else 0o644
            entries.append((f"./{rel}", p, mode))
            md5s.append(f"{hashlib.md5(p.read_bytes()).hexdigest()}  {rel}\n")
            kib += (p.stat().st_size + 1023) // 1024
    return entries, "".join(md5s), kib


def ar(members: list) -> bytes:
    """A System V ar archive as dpkg writes it: names padded, no trailing slash."""
    out = b"!<arch>\n"
    for name, data, mtime in members:
        if len(name) > 15:
            C.die(f"ar member name too long: {name}")
        out += f"{name:<16}{mtime:<12}{0:<6}{0:<6}{0o100644:<8o}{len(data):<10}`\n".encode()
        out += data + (b"\n" if len(data) % 2 else b"")
    return out


def commit_time(ref: str) -> int:
    return int(subprocess.run(["git", "-C", str(C.REPO), "log", "-1", "--format=%ct", ref], check=True,
                              capture_output=True, text=True).stdout.strip())


def build(args) -> Path:
    version = args.version or C.app_version(args.ref)
    work = C.fresh(args.work / "deb")
    release = C.stage_release(work / "release", args.ref)
    pngs = C.icons(args.work, sizes=(16, 24, 32, 48, 64, 128, 256, 512))
    root = work / "root"
    build_root(root, release, pngs)
    C.check_nothing_personal([root])
    mtime = commit_time(args.ref)

    entries, md5sums, kib = data_entries(root)
    data = tar_xz(entries, mtime)
    ctrl = tar_xz([("./", None, 0o755),
                   ("./control", control(version, kib).encode(), 0o644),
                   ("./md5sums", md5sums.encode(), 0o644),
                   ("./prerm", (C.HERE / "linux/prerm").read_bytes(), 0o755)], mtime)
    args.out.mkdir(parents=True, exist_ok=True)
    out = args.out / f"{C.NAME}_{version}_all.deb"
    out.write_bytes(ar([("debian-binary", b"2.0\n", mtime), ("control.tar.xz", ctrl, mtime),
                        ("data.tar.xz", data, mtime)]))
    C.log(f"{out} ({out.stat().st_size / 2**20:.1f} MiB, {kib} KiB installed)")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    C.common_args(ap)
    build(ap.parse_args())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
