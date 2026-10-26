#!/usr/bin/env python3
"""Every installer, the two-file boot's files, and SHA256SUMS.txt beside them.

    python3 tools/package/build_all.py [--out DIR] [--no-make] [--only macos,windows,deb,appimage]

Builds the prebuilt loader files first (make -s in ghostldr/native) unless
--no-make, then:

    ghostldr-<version>-macos-universal.dmg        build_macos.py (on macOS only)
    ghostldr-<version>-windows-x64-setup.exe      build_windows.py
    ghostldr_<version>_all.deb                    build_deb.py
    ghostldr-<version>-x86_64.AppImage            build_appimage.py
    ghostldr-<version>-aarch64.AppImage
    ghostldr.lua, ghostldr-ext.bin, ghostldr-elfldr.bin
                                                  a boot with nothing but nc (ghostldr/tools/mkbundle.py,
                                                  docs/using-the-release.md), from the same build
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_appimage  # noqa: E402
import build_deb  # noqa: E402
import build_macos  # noqa: E402
import build_windows  # noqa: E402
import common as C  # noqa: E402

KINDS = ("macos", "windows", "deb", "appimage")
TWO_FILE = ("ghostldr.lua", "ghostldr-ext.bin", "ghostldr-elfldr.bin")


def two_file_boot(out: Path) -> list[Path]:
    """The two-file boot's files from build/ghostldr (the Makefile makes them
    with the core), copied to the release folder and checked for anything of
    this computer's."""
    src = C.REPO / "build/ghostldr"
    missing = [n for n in TWO_FILE if not (src / n).is_file()]
    if missing:
        C.die(f"build/ghostldr is missing {', '.join(missing)}: run `make -s` in ghostldr/native")
    out.mkdir(parents=True, exist_ok=True)
    copied = []
    for n in TWO_FILE:
        shutil.copyfile(src / n, out / n)
        copied.append(out / n)
    C.check_nothing_personal(copied)
    C.log(f"two-file boot: {', '.join(f'{p.name} ({p.stat().st_size} bytes)' for p in copied)}")
    return copied


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    C.common_args(ap)
    ap.add_argument("--no-make", action="store_true", help="use build/ghostldr as it is")
    ap.add_argument("--only", default=",".join(KINDS), help=f"comma-separated, from {', '.join(KINDS)}")
    ap.add_argument("--epoch", type=int, default=None, help="timestamp for the AppImages (SOURCE_DATE_EPOCH)")
    args = ap.parse_args()
    args.out = args.out.resolve()
    args.arch = None
    kinds = [k.strip() for k in args.only.split(",") if k.strip()]
    unknown = set(kinds) - set(KINDS)
    if unknown:
        ap.error(f"unknown: {', '.join(sorted(unknown))}")
    if "macos" in kinds and sys.platform != "darwin":
        C.log("skipping the macOS app: it is built on macOS")
        kinds.remove("macos")
    if not args.no_make:
        C.log("building the prebuilt loader files (make -s in ghostldr/native)")
        C.run(["make", "-s", "-C", C.REPO / "ghostldr/native"])

    built = two_file_boot(args.out)
    if "macos" in kinds:
        built.append(build_macos.build(args))
    if "windows" in kinds:
        built.append(build_windows.build(args))
    if "deb" in kinds:
        built.append(build_deb.build(args))
    if "appimage" in kinds:
        built += build_appimage.build(args)
    sums = C.write_sums(args.out, [p.name for p in built])
    print()
    for p in built:
        print(f"{C.sha256_file(p)}  {p.stat().st_size:>10}  {p.name}")
    C.log(f"checksums in {sums}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
