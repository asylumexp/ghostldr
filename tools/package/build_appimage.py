#!/usr/bin/env python3
"""ghostldr-<version>-<arch>.AppImage (x86_64, aarch64): the app with its own Python.

    python3 tools/package/build_appimage.py [--arch x86_64 --arch aarch64] [--out DIR]

Needs mksquashfs (squashfs-tools 4.4+, with zstd) and resvg; runs on macOS
or Linux.  appimagetool isn't needed: an AppImage is the type 2 runtime
(AppImage/type2-runtime) followed by a squashfs image of the AppDir:

    AppRun                                  linux/AppRun: the Python below runs app/launch.py
    ghostldr.desktop, ghostldr.png, .DirIcon
    usr/lib/ghostldr/python                 python-build-standalone, <arch>-unknown-linux-gnu
    usr/lib/ghostldr/ghostldr-app           the release tree
    usr/share/applications, usr/share/icons/hicolor
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common as C  # noqa: E402

ARCHS = ("x86_64", "aarch64")
ELF_MACHINE = {"x86_64": 0x3E, "aarch64": 0xB7}


def desktop_entry(version: str) -> str:
    text = (C.HERE / "linux/ghostldr.desktop").read_text(encoding="utf-8")
    return text + f"X-AppImage-Version={version}\n"


def hicolor(pngs: dict, share: Path) -> None:
    for size, png in pngs.items():
        d = share / f"icons/hicolor/{size}x{size}/apps"
        d.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(png, d / f"{C.NAME}.png")
    d = share / "icons/hicolor/scalable/apps"
    d.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(C.ICON_SVG, d / f"{C.NAME}.svg")


def check_runtime(path: Path, arch: str) -> None:
    head = path.read_bytes()[:20]
    machine = int.from_bytes(head[18:20], "little")
    if head[:4] != b"\x7fELF" or head[8:11] != b"AI\x02" or machine != ELF_MACHINE[arch]:
        C.die(f"{path.name} isn't an AppImage type 2 runtime for {arch}")


def build_one(args, arch: str, version: str, release_src: Path, pngs: dict, hostpy: Path) -> Path:
    work = C.fresh(args.work / f"appimage-{arch}")
    appdir = work / "ghostldr.AppDir"
    lib = appdir / "usr/lib/ghostldr"
    lib.mkdir(parents=True)
    shutil.copytree(release_src, lib / "ghostldr-app", symlinks=True)
    C.python_runtime(f"python-linux-{arch}", args.cache, lib / "python")
    C.precompile(hostpy, lib / "python/lib", strip=appdir)
    C.precompile(hostpy, lib / "ghostldr-app/app", strip=appdir)

    shutil.copyfile(C.HERE / "linux/AppRun", appdir / "AppRun")
    (appdir / "AppRun").chmod(0o755)
    entry = desktop_entry(version)
    (appdir / f"{C.NAME}.desktop").write_text(entry, encoding="utf-8")
    share = appdir / "usr/share"
    (share / "applications").mkdir(parents=True)
    (share / f"applications/{C.NAME}.desktop").write_text(entry, encoding="utf-8")
    hicolor(pngs, share)
    shutil.copyfile(pngs[256], appdir / f"{C.NAME}.png")
    os.symlink(f"{C.NAME}.png", appdir / ".DirIcon")
    C.check_nothing_personal([lib / "ghostldr-app", appdir / "AppRun", appdir / f"{C.NAME}.desktop"])
    C.check_nothing_personal([lib / "python"], scan_ips=False)

    runtime = C.fetch(f"appimage-runtime-{arch}", args.cache)
    check_runtime(runtime, arch)
    squash = work / "image.squashfs"
    env = dict(os.environ, SOURCE_DATE_EPOCH=str(args.epoch)) if args.epoch else None
    C.run(["mksquashfs", appdir, squash, "-noappend", "-all-root", "-no-xattrs", "-comp", "zstd",
           "-quiet", "-no-progress"], env=env, stdout=subprocess.DEVNULL)
    args.out.mkdir(parents=True, exist_ok=True)
    out = args.out / f"ghostldr-{version}-{arch}.AppImage"
    with open(out, "wb") as f:
        f.write(runtime.read_bytes())
        with open(squash, "rb") as s:
            shutil.copyfileobj(s, f, 1 << 20)
    out.chmod(0o755)
    C.log(f"{out} ({out.stat().st_size / 2**20:.1f} MiB)")
    return out


def build(args) -> list:
    if not shutil.which("mksquashfs"):
        C.die("needs mksquashfs (brew install squashfs, or apt install squashfs-tools)")
    version = args.version or C.app_version(args.ref)
    release = C.stage_release(args.work / "appimage-release", args.ref)
    pngs = C.icons(args.work, sizes=(16, 24, 32, 48, 64, 128, 256, 512))
    hostpy = C.host_python(args.cache, args.work)
    return [build_one(args, arch, version, release, pngs, hostpy) for arch in (args.arch or ARCHS)]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    C.common_args(ap)
    ap.add_argument("--arch", action="append", choices=ARCHS, help="default: both")
    ap.add_argument("--epoch", type=int, default=None, help="timestamp for the image (SOURCE_DATE_EPOCH)")
    build(ap.parse_args())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
