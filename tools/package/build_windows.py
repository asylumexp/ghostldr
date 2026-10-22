#!/usr/bin/env python3
"""ghostldr-<version>-windows-x64-setup.exe: a per-user installer with its own Python.

    python3 tools/package/build_windows.py [--out DIR]

Needs makensis (NSIS 3: brew install makensis, apt install nsis) and resvg;
runs on macOS or Linux.  The installer (windows/ghostldr.nsi) puts in
%LOCALAPPDATA%\\ghostldr:

    python\\        python.org's Windows embeddable package (amd64), with its
                   python3XX._pth adding ghostldr\\host and ghostldr\\tools to the
                   path (windows/python._pth): the embeddable package leaves out
                   a script's own folder, and the scripts import their neighbours
    app\\, ghostldr\\, build\\ghostldr\\...   the release tree
    ghostldr.ico

and Start menu (and, if ticked, desktop) shortcuts to pythonw.exe app\\launch.py.
The folder can be written, so the scripts run from it (no workspace).
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common as C  # noqa: E402

ICON_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def unzip(archive: Path, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        for name in z.namelist():
            p = PurePosixPath(name)
            if p.is_absolute() or ".." in p.parts or "\\" in name:
                C.die(f"{archive.name}: unsafe path {name}")
        z.extractall(dest)


def embeddable_python(cache: Path, dest: Path) -> str:
    """The embeddable package at dest, its ._pth replaced; returns its version tag (314)."""
    unzip(C.fetch("python-windows-amd64", cache), dest)
    pths = list(dest.glob("python3*._pth"))
    if len(pths) != 1 or not (dest / "pythonw.exe").is_file():
        C.die("unexpected embeddable package: no single python3XX._pth or no pythonw.exe")
    xy = pths[0].name[len("python"):-len("._pth")]
    if not (dest / f"python{xy}.zip").is_file():
        C.die(f"unexpected embeddable package: no python{xy}.zip")
    text = (C.HERE / "windows/python._pth").read_text(encoding="utf-8").replace("{xy}", xy)
    pths[0].write_bytes(text.replace("\n", "\r\n").encode("utf-8"))
    return xy


def nsis_lists(release: Path, out: Path) -> Path:
    """files.nsh: RemoveRelease deletes exactly what the installer puts at the
    top of the folder (but build\\, the scripts' own output)."""
    lines = ["!macro RemoveRelease"]
    for p in sorted(release.iterdir()):
        if p.name == "build":
            continue
        if p.is_dir():
            lines.append(f'  RMDir /r "$INSTDIR\\{p.name}"')
        else:
            lines.append(f'  Delete "$INSTDIR\\{p.name}"')
    lines += ["!macroend", ""]
    out.write_text("\n".join(lines), encoding="utf-8")
    return out


def build(args) -> Path:
    makensis = shutil.which("makensis")
    if not makensis:
        C.die("needs makensis (brew install makensis, or apt install nsis)")
    version = args.version or C.app_version(args.ref)
    parts = (version.split("-")[0].split(".") + ["0", "0", "0"])[:4]
    if not all(x.isdigit() for x in parts):
        C.die(f"version {version}: Windows needs numbers (like 0.4.0)")
    work = C.fresh(args.work / "windows")
    root = C.stage_release(work / "root", args.ref)
    xy = embeddable_python(args.cache, root / "python")
    pngs = C.icons(args.work, sizes=ICON_SIZES)
    C.write_ico(pngs, root / "ghostldr.ico")
    lists = nsis_lists(root, work / "files.nsh")
    C.check_nothing_personal([p for p in root.iterdir() if p.name != "python"] + [lists])
    C.check_nothing_personal([root / "python"], scan_ips=False)

    args.out.mkdir(parents=True, exist_ok=True)
    out = args.out / f"ghostldr-{version}-windows-x64-setup.exe"
    out.unlink(missing_ok=True)
    # makensis 3.x aborts (std::bad_alloc) printing its non-ASCII banner under the C locale
    env = dict(os.environ, LC_ALL="en_US.UTF-8" if sys.platform == "darwin" else "C.UTF-8")
    r = subprocess.run([makensis, "-V2", "-INPUTCHARSET", "UTF8", f"-DVERSION={version}",
                        f"-DVERSION4={'.'.join(parts)}", f"-DSRCDIR={root}", f"-DOUTFILE={out}",
                        f"-DICON={root / 'ghostldr.ico'}", f"-DLISTS={lists}", str(C.HERE / "windows/ghostldr.nsi")],
                       env=env, capture_output=True, text=True)
    if r.returncode != 0 or "warning" in r.stdout.lower():
        print(r.stdout + r.stderr)
    if r.returncode != 0:
        C.die(f"makensis failed ({r.returncode})")
    C.log(f"{out} ({out.stat().st_size / 2**20:.1f} MiB, Python {xy[0]}.{xy[1:]} embeddable)")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    C.common_args(ap)
    build(ap.parse_args())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
