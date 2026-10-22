#!/usr/bin/env python3
"""ghostldr-<version>-macos-universal.dmg: ghostldr.app with its own Python.

    python3 tools/package/build_macos.py [--out DIR]

Runs on macOS (clang, codesign, iconutil, hdiutil) with resvg for the icon.

ghostldr.app/Contents:
    MacOS/ghostldr               universal launcher (macos/launcher.c): picks python/<arch>
    Resources/python/arm64       python-build-standalone, aarch64-apple-darwin
    Resources/python/x86_64      python-build-standalone, x86_64-apple-darwin
    Resources/ghostldr-app       the release tree (app/, ghostldr/, build/ghostldr/...)
    Resources/ghostldr.icns

The bundle is signed ad hoc (it runs on Apple silicon), not with a Developer
ID and not notarized: the first open needs the steps in README.md.
"""

from __future__ import annotations

import argparse
import os
import plistlib
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common as C  # noqa: E402

BUNDLE_ID = "io.github.asylumexp.ghostldr"
ARCHS = {"arm64": "python-macos-arm64", "x86_64": "python-macos-x86_64"}
MIN_MACOS = "10.15"            # python-build-standalone: x86_64 10.15, arm64 11.0
MACHO_MAGIC = {b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}


def is_macho(path: Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    with open(path, "rb") as f:
        return f.read(4) in MACHO_MAGIC


def make_icns(work: Path, out: Path) -> Path:
    svg = C.macos_icon_svg(work)
    iconset = C.fresh(work / "ghostldr.iconset")
    for size in (16, 32, 128, 256, 512):
        C.render_png(svg, size, iconset / f"icon_{size}x{size}.png")
        C.render_png(svg, size * 2, iconset / f"icon_{size}x{size}@2x.png")
    C.run(["iconutil", "-c", "icns", "-o", out, iconset])
    return out


def build_launcher(out: Path) -> Path:
    C.run(["clang", "-arch", "arm64", "-arch", "x86_64", f"-mmacosx-version-min={MIN_MACOS}", "-O2", "-Wall",
           "-Wextra", "-Werror", "-o", out, C.HERE / "macos/launcher.c"])
    C.run(["strip", "-x", out])
    return out


def info_plist(version: str) -> dict:
    return {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": "ghostldr",
        "CFBundleExecutable": "ghostldr",
        "CFBundleIconFile": "ghostldr",
        "CFBundleIdentifier": BUNDLE_ID,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": "ghostldr",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": version,
        "LSApplicationCategoryType": "public.app-category.utilities",
        "LSMinimumSystemVersion": MIN_MACOS,
        "LSUIElement": True,
        "NSHighResolutionCapable": True,
        "NSHumanReadableCopyright": "MIT License. Includes Python (PSF License) and Noto Sans (SIL OFL 1.1).",
    }


def sign(app: Path) -> None:
    """Ad hoc: every Mach-O inside first (the Pythons sit in Resources, where
    --deep doesn't look), then the bundle, which seals the rest."""
    inner = [p for p in sorted(app.rglob("*")) if is_macho(p) and p != app / "Contents/MacOS/ghostldr"]
    for i in range(0, len(inner), 200):
        C.run(["codesign", "--force", "--sign", "-", *inner[i:i + 200]], stdout=subprocess.DEVNULL,
              stderr=subprocess.DEVNULL)
    C.run(["codesign", "--force", "--deep", "--sign", "-", "--identifier", BUNDLE_ID, app])
    C.run(["codesign", "--verify", "--deep", "--strict", "--verbose=1", app])
    C.log(f"signed ad hoc: {len(inner)} binaries inside, then the bundle")


def build(args) -> Path:
    if sys.platform != "darwin":
        C.die("the macOS app is built on macOS")
    version = args.version or C.app_version(args.ref)
    work = C.fresh(args.work / "macos")
    stage = C.fresh(work / "dmg")
    app = stage / "ghostldr.app"
    contents = app / "Contents"
    res = contents / "Resources"
    (contents / "MacOS").mkdir(parents=True)
    res.mkdir()

    release = C.stage_release(res / "ghostldr-app", args.ref)
    for arch, key in ARCHS.items():
        C.python_runtime(key, args.cache, res / "python" / arch)
    hostpy = C.host_python(args.cache, args.work)
    for arch in ARCHS:
        C.precompile(hostpy, res / "python" / arch / "lib", strip=res)
    C.precompile(hostpy, release / "app", strip=res)

    make_icns(work, res / "ghostldr.icns")
    build_launcher(contents / "MacOS/ghostldr")
    with open(contents / "Info.plist", "wb") as f:
        plistlib.dump(info_plist(version), f)
    (contents / "PkgInfo").write_text("APPL????")
    C.check_nothing_personal([release, contents / "Info.plist"])
    C.check_nothing_personal([res / "python", contents / "MacOS"], scan_ips=False)
    sign(app)

    os.symlink("/Applications", stage / "Applications")
    shutil.copyfile(C.HERE / "macos/README.txt", stage / "READ ME FIRST.txt")
    args.out.mkdir(parents=True, exist_ok=True)
    dmg = args.out / f"ghostldr-{version}-macos-universal.dmg"
    dmg.unlink(missing_ok=True)
    C.run(["hdiutil", "create", "-quiet", "-volname", f"ghostldr {version}", "-srcfolder", stage,
           "-fs", "HFS+", "-format", "ULMO", "-ov", dmg])
    C.log(f"{dmg} ({dmg.stat().st_size / 2**20:.1f} MiB)")
    return dmg


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    C.common_args(ap)
    build(ap.parse_args())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
