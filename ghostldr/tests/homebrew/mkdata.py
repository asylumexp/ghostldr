#!/usr/bin/env python3
"""hbprobe's files next to its eboot.bin: hbprobe/data.txt ("line N" for N in
0..3999) and, given a title ID, a minimal sce_sys/param.sfo.

    python3 mkdata.py APP0 [TITLE_ID] [--sfo-only] [--name TITLE]
"""

import struct
import sys
from pathlib import Path


def param_sfo(entries: dict) -> bytes:
    keys, data, index = b"", b"", b""
    for k, v in entries.items():
        raw = struct.pack("<i", v) if isinstance(v, int) else v.encode() + b"\0"
        size = (len(raw) + 3) & ~3
        index += struct.pack("<HHIII", len(keys), 0x0404 if isinstance(v, int) else 0x0204, len(raw), size, len(data))
        keys += k.encode() + b"\0"
        data += raw.ljust(size, b"\0")
    keys = keys.ljust((len(keys) + 3) & ~3, b"\0")
    key_off = 20 + len(index)
    return struct.pack("<4sIIII", b"\0PSF", 0x101, key_off, key_off + len(keys), len(entries)) + index + keys + data


def main() -> None:
    args = sys.argv[1:]
    sfo_only = "--sfo-only" in args
    name = "hbprobe (system malloc)"
    if "--name" in args:
        i = args.index("--name")
        name = args[i + 1]
        del args[i:i + 2]
    args = [a for a in args if a != "--sfo-only"]
    app0 = Path(args[0])
    if not sfo_only:
        (app0 / "hbprobe").mkdir(parents=True, exist_ok=True)
        (app0 / "hbprobe/data.txt").write_text("".join(f"line {n}\n" for n in range(4000)))
    if len(args) > 1:
        (app0 / "sce_sys").mkdir(parents=True, exist_ok=True)
        (app0 / "sce_sys/param.sfo").write_bytes(param_sfo({
            "APP_TYPE": 1, "APP_VER": "01.00", "CATEGORY": "gd", "TITLE": name,
            "TITLE_ID": args[1], "VERSION": "01.00"}))


if __name__ == "__main__":
    main()
