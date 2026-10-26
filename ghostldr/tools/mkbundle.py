#!/usr/bin/env python3
"""The two-file boot's files, from one build (docs/architecture.md, "Two-file boot"):

    ghostldr.lua         the core, every title profile in ghostldr/data/titles,
                         run.py boot's defaults and the two bundles' pins:
                         nc PS5 9026 < ghostldr.lua
    ghostldr-ext.bin     the full bundle: ext.bin (every HLE family, the NID
                         table, the ELF loader, FTP, USB) and selector.bin.
                         Boots games as run.py boot does (--app-root auto)
    ghostldr-elfldr.bin  the ELF loader's bundle: ext-elfldr.bin alone

    nc PS5 9021 < ghostldr-ext.bin       (9022 when 9021 is taken)

    python3 ghostldr/tools/mkbundle.py [--out build/ghostldr]

A bundle is a 128-byte header (HEADER below), the extension, then the
selector (none in the ELF loader's).  ghostldr.lua takes a bundle only when
its header matches the pins it carries, and the core checks the extension
against them again (CRC-32, then SHA-256) and the selector by its CRC-32.
Nothing of the computer it is built on goes in: no address, no path, no
local dump or pack.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "ghostldr/host"))
import profiles  # noqa: E402
import run  # noqa: E402

MAGIC = b"GLDRBNDL"
FORMAT = 1
# magic, format, header size, kind, flags, ext size, selector size, their
# CRC-32s, their SHA-256s, the build ID (8 bytes), reserved
HEADER = struct.Struct("<8sIIIIQQII32s32s8s8x")
assert HEADER.size == 128
KIND_FULL, KIND_ELFLDR = 1, 2
FILES = {KIND_FULL: "ghostldr-ext.bin", KIND_ELFLDR: "ghostldr-elfldr.bin"}
LUA = "ghostldr.lua"


def build_id(core: bytes, parts: list[bytes]) -> bytes:
    """8 bytes naming the build: the core and every bundled part."""
    h = hashlib.sha256(core)
    for p in parts:
        h.update(hashlib.sha256(p).digest())
    return h.digest()[:8]


def header(kind: int, ext: bytes, sel: bytes, build: bytes) -> bytes:
    return HEADER.pack(MAGIC, FORMAT, HEADER.size, kind, 0, len(ext), len(sel), zlib.crc32(ext), zlib.crc32(sel),
                       hashlib.sha256(ext).digest(), hashlib.sha256(sel).digest() if sel else bytes(32), build)


def parse_header(data: bytes) -> dict:
    """A bundle's header, as ghostldr.lua reads it."""
    (magic, fmt, hsize, kind, flags, ext_size, sel_size, ext_crc, sel_crc, ext_sha, sel_sha,
     build) = HEADER.unpack_from(data)
    return {"magic": magic, "format": fmt, "header_size": hsize, "kind": kind, "flags": flags,
            "ext_size": ext_size, "sel_size": sel_size, "ext_crc": ext_crc, "sel_crc": sel_crc,
            "ext_sha256": ext_sha.hex(), "sel_sha256": sel_sha.hex(), "build": build.hex()}


def pin_lua(kind: int, ext: bytes, sel: bytes) -> str:
    sel_sha = hashlib.sha256(sel).hexdigest() if sel else "00" * 32
    return (f'    {{kind = {kind}, file = "{FILES[kind]}", ext_size = {len(ext)}, ext_crc = {zlib.crc32(ext):#x}, '
            f'ext_sha256 = "{hashlib.sha256(ext).hexdigest()}",\n'
            f'     sel_size = {len(sel)}, sel_crc = {zlib.crc32(sel):#x}, sel_sha256 = "{sel_sha}"}},')


def abi(blob: bytes, what: str) -> int:
    if len(blob) < 24:
        raise SystemExit(f"{what}: only {len(blob)} bytes")
    return struct.unpack_from("<I", blob, 20)[0]


def make(out: Path) -> dict[str, Path]:
    core = (out / "core.bin").read_bytes()
    layout = __import__("json").loads((out / "core.json").read_text())
    ext, sel, ext_elfldr = ((out / n).read_bytes() for n in ("ext.bin", "selector.bin", "ext-elfldr.bin"))
    abis = {abi(ext, "ext.bin"), abi(sel, "selector.bin"), abi(ext_elfldr, "ext-elfldr.bin")}
    if len(abis) != 1:
        raise SystemExit("ext.bin, selector.bin and ext-elfldr.bin come from different builds: make -C ghostldr/native")
    text_size = (layout["__text_end"] + 0x3FFF) & ~0x3FFF
    if layout["__data_start"] != text_size:
        raise SystemExit(f"core data must start at the text page boundary ({text_size:#x})")
    build = build_id(core, [ext, sel, ext_elfldr])
    bundles = {KIND_FULL: (ext, sel), KIND_ELFLDR: (ext_elfldr, b"")}
    paths = {}
    for kind, (e, s) in bundles.items():
        paths[FILES[kind]] = out / FILES[kind]
        paths[FILES[kind]].write_bytes(header(kind, e, s, build) + e + s)
    defaults = run.build_parser().parse_args(["boot"])
    lines = [
        f"-- ghostldr.lua, build {build.hex()}: ghostldr's bootstrap with the core in it, for a boot with no",
        "-- computer serving files (docs/using-the-release.md).  Send it to Luac0re's Remote Lua Loader in SWRR,",
        "-- then one bundle of the same build to the port it opens (the PS5's notification names it):",
        "--   nc PS5 9026 < ghostldr.lua",
        "--   nc PS5 9021 < ghostldr-ext.bin       a game: a USB drive's, else an installed one",
        "--   nc PS5 9021 < ghostldr-elfldr.bin    the ELF loader (docs/elfldr.md)",
        "-- 9022 instead when 9021 is taken (a jailbroken console's own ELF loader).",
        'GHOSTLDR_MODE = "listen"',
        f'GHOSTLDR_BUILD = "{build.hex()}"',
        "GHOSTLDR_BUNDLES = {",
        *(pin_lua(kind, e, s) for kind, (e, s) in bundles.items()),
        "}",
        f'GHOSTLDR_CORE_HEX = "{core.hex()}"',
        f"GHOSTLDR_TEXT_SIZE = {text_size:#x}",
        f"GHOSTLDR_IMAGE_SIZE = {layout['__image_end']:#x}",
        f"GHOSTLDR_FLAGS = {run.boot_flags(defaults):#x}",    # run.py boot's: release SWRR's buffers, FTP 1212
        f"GHOSTLDR_APP_ROOT = {run.lua_str(defaults.app_root)}",
        f'GHOSTLDR_PROFILE_HEX = "{profiles.release_bundle().hex()}"',
        # Luac0re keeps one Lua state: what an earlier run.py boot set must not count here
        "GHOSTLDR_LOG_IP, GHOSTLDR_LOG_PORT = nil, nil",
        "GHOSTLDR_EXT_PORT, GHOSTLDR_EXT_SIZE, GHOSTLDR_EXT_CRC, GHOSTLDR_EXT_SHA256 = nil, nil, nil, nil",
        "GHOSTLDR_SEL_PORT, GHOSTLDR_SEL_SIZE, GHOSTLDR_SEL_CRC = nil, nil, nil",
        "GHOSTLDR_GPU_HEAP_MIB, GHOSTLDR_SLACK_MIB, GHOSTLDR_POOL1_MIB, GHOSTLDR_CPU_HEAP_MIB = nil, nil, nil, nil",
        "",
    ]
    lua = ("\n".join(lines) + run.BOOT_LUA.read_text()).encode()
    if len(lua) >= run.LUA_MAX:
        raise SystemExit(f"{LUA} is {len(lua)} bytes; Luac0re's loader reads at most {run.LUA_MAX}")
    paths[LUA] = out / LUA
    paths[LUA].write_bytes(lua)
    return paths


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", type=Path, default=ROOT / "build/ghostldr",
                    help="where core.bin, ext.bin, selector.bin and ext-elfldr.bin are, and the files go")
    args = ap.parse_args()
    paths = make(args.out)
    build = parse_header(paths[FILES[KIND_FULL]].read_bytes())["build"]
    for name, p in paths.items():
        print(f"{name}: {p.stat().st_size} bytes (build {build})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
