#!/usr/bin/env python3
"""Make a copy of SWRR's Luac0re save for another PSN account (USB import).

A PS4 save's image is encrypted with a key only a console can use, so the
copy is made on our console by the system itself (ghostldr/lua/save_resign.lua
through Luac0re's loader on 9026): a new save container TEMP_DIR gets the
Luac0re save's files and a param.sfo with the other account's ACCOUNT_ID and
SAVEDATA_DIRECTORY SLUS-20268.  sce_sys is not writable from the app, so the
param.sfo is patched over FTP as root (ftpsrv: needs the jailbreak) while the
copy stays mounted; the unmount commits (re-signs) it.  The raw image and key
are then fetched over FTP too and laid out
the way the PS4/PS5 "copy from USB" import reads them:

    PS4/SAVEDATA/<account id, 16 hex>/CUSA03474/SLUS-20268      (the image)
    PS4/SAVEDATA/<account id, 16 hex>/CUSA03474/SLUS-20268.bin  (sealed key)

    python3 ghostldr/host/resign_save.py lua copy --account 1234567890123456789
    python3 ghostldr/host/resign_save.py lua show
    python3 ghostldr/host/resign_save.py usb --account 1234567890123456789 --name SomeUser
    python3 ghostldr/host/resign_save.py lua delete

The account ID is the 64-bit PSN account ID: decimal (as psn.flipscreen.games
shows it) or hex with a 0x prefix.  Only SWRR's save is read; the copy is a
separate container.
"""

from __future__ import annotations

import argparse
import ftplib
import io
import shutil
import socket
import struct
import time
import zipfile
from pathlib import Path

from console import console_ip

ROOT = Path(__file__).resolve().parents[2]
LUA = ROOT / "ghostldr/lua/save_resign.lua"
OUT = ROOT / "build/resigned"
LOG_PORT = 18196
TITLE = "CUSA03474"
ORIGINAL = "SLUS-20268"
TEMP_DIR = "SLUS-RESGN"   # same length as SLUS-20268


def account_id(text: str) -> int:
    v = int(text, 16) if text.lower().startswith("0x") else int(text)
    if not 0 < v < 1 << 64:
        raise SystemExit("account ID out of range")
    return v


def patch_sfo(data: bytes, acct: int, directory: str = ORIGINAL) -> bytes:
    """ACCOUNT_ID (u64, little endian) and SAVEDATA_DIRECTORY of a param.sfo."""
    out = bytearray(data)
    if out[:4] != b"\0PSF":
        raise ValueError("not a param.sfo")
    kt, dt, n = struct.unpack_from("<III", out, 8)
    found = set()
    for i in range(n):
        ko, fmt, ln, mx, do = struct.unpack_from("<HHIII", out, 20 + 16 * i)
        key = out[kt + ko:out.index(0, kt + ko)].decode()
        if key == "ACCOUNT_ID" and ln == 8:
            out[dt + do:dt + do + 8] = acct.to_bytes(8, "little")
            found.add(key)
        elif key == "SAVEDATA_DIRECTORY":
            value = directory.encode() + b"\0"
            out[dt + do:dt + do + mx] = value.ljust(mx, b"\0")
            struct.pack_into("<I", out, 20 + 16 * i + 4, len(value))
            found.add(key)
    if found != {"ACCOUNT_ID", "SAVEDATA_DIRECTORY"}:
        raise ValueError(f"param.sfo lacks {({'ACCOUNT_ID', 'SAVEDATA_DIRECTORY'} - found)}")
    return bytes(out)


def ftp() -> ftplib.FTP:
    f = ftplib.FTP()
    f.connect(console_ip(), 2121, timeout=30)
    f.login()
    f.voidcmd("TYPE I")
    return f


def host_patch(mount_point: str, acct: int, directory: str = ORIGINAL) -> None:
    """Patch the mounted copy's param.sfo as root (ftpsrv, jailbreak)."""
    remote = f"/mnt/sandbox/{TITLE}_000{mount_point}/sce_sys/param.sfo"
    f = ftp()
    icon = io.BytesIO()                 # the save list's icon, before the param (that ends the hold)
    try:
        f.retrbinary(f"RETR /mnt/sandbox/{TITLE}_000/savedata0/sce_sys/icon0.png", icon.write)
        f.storbinary(f"STOR {remote.replace('param.sfo', 'icon0.png')}", io.BytesIO(icon.getvalue()))
    except ftplib.Error as e:
        print(f"host: icon0.png not copied ({e})", flush=True)
    data = io.BytesIO()
    f.retrbinary(f"RETR {remote}", data.write)
    patched = patch_sfo(data.getvalue(), acct, directory)
    f.storbinary(f"STOR {remote}", io.BytesIO(patched))
    f.quit()
    print(f"host: patched {remote} ({len(patched)} bytes)", flush=True)


def run_lua(host: str, mode: str, acct: int, wait: float, temp: str = TEMP_DIR, patch_dir: bool = True) -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect((host, 9))
        local_ip = probe.getsockname()[0]
    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.bind(("0.0.0.0", LOG_PORT))
    listener.settimeout(1)
    acct_bytes = ", ".join(f"0x{b:02x}" for b in acct.to_bytes(8, "little"))
    lua = (f'MODE = "{mode}"\nTEMP_DIR = "{temp}"\nACCOUNT_ID_BYTES = {{{acct_bytes}}}\n'
           f'PATCH_DIR = {"true" if patch_dir else "false"}\n'
           f'PROBE_LOG_IP = "{local_ip}"\nPROBE_LOG_PORT = {LOG_PORT}\n' + LUA.read_text())
    with socket.create_connection((host, 9026), timeout=10) as s:
        s.sendall(lua.encode())
        s.shutdown(socket.SHUT_WR)
    deadline = time.time() + wait
    while time.time() < deadline:
        try:
            data, _ = listener.recvfrom(4096)
        except socket.timeout:
            continue
        line = data.decode("utf-8", "replace").rstrip("\0")
        print(line, flush=True)
        if ": hold /savedata" in line:
            try:
                host_patch(line.rsplit(" ", 1)[1], acct, ORIGINAL if patch_dir else temp)
            except (OSError, ftplib.Error, ValueError) as e:
                print(f"host: cannot patch over FTP ({e}); is ftpsrv running (jailbreak)?", flush=True)
        if line.endswith("done"):
            return 0
    print("no complete report (is SWRR at the Luac0re loader?)")
    return 1


def fetch(host: str, remote: str) -> bytes:
    f = ftp()
    out = io.BytesIO()
    f.retrbinary(f"RETR {remote}", out.write)
    f.quit()
    return out.getvalue()


def find_uid() -> str:
    """The local user (8 hex digits) whose saves hold the copy."""
    f = ftp()
    try:
        for home in f.nlst("/user/home"):
            uid = home.rstrip("/").rsplit("/", 1)[-1]
            try:
                if any(n.endswith(f"sdimg_{TEMP_DIR}") for n in f.nlst(f"/user/home/{uid}/savedata/{TITLE}")):
                    return uid
            except ftplib.Error:
                continue
    finally:
        f.quit()
    raise SystemExit(f"no user has {TEMP_DIR}: run `lua copy` first")


def make_usb(host: str, acct: int, uid: str | None, name: str) -> Path:
    uid = uid or find_uid()
    base = f"/user/home/{uid}/savedata/{TITLE}"
    image = fetch(host, f"{base}/sdimg_{TEMP_DIR}")
    key = fetch(host, f"{base}/{TEMP_DIR}.bin")
    if key[:8] != b"pfsSKKey" or len(key) != 96:
        raise SystemExit(f"{TEMP_DIR}.bin is not a sealed key ({len(key)} bytes)")
    if image[0x18] != 0:
        raise SystemExit(f"image header byte 0x18 is {image[0x18]} (not cleanly committed)")
    root = OUT / name
    shutil.rmtree(root, ignore_errors=True)
    dest = root / "PS4/SAVEDATA" / f"{acct:016x}" / TITLE
    dest.mkdir(parents=True)
    (dest / ORIGINAL).write_bytes(image)
    (dest / f"{ORIGINAL}.bin").write_bytes(key)
    archive = OUT / f"{name}-{TITLE}-{ORIGINAL}.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(root.rglob("*")):
            if p.is_file():
                z.write(p, p.relative_to(root))
    print(f"{dest}: image {len(image)} bytes, key version {key[8]}")
    print(f"zip: {archive}")
    return archive


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("step", choices=["lua", "usb"])
    ap.add_argument("mode", nargs="?", default="show", choices=["copy", "patch", "show", "delete"])
    ap.add_argument("--account", help="PSN account ID (decimal, or hex with 0x)")
    ap.add_argument("--uid", help="local user id of the PS5 user running SWRR (8 hex; default: found over FTP)")
    ap.add_argument("--name", default="resigned", help="output folder name under build/resigned/")
    ap.add_argument("--wait", type=float, default=120)
    ap.add_argument("--temp", default=TEMP_DIR, help="the copy's container name (10 characters)")
    ap.add_argument("--keep-dir", action="store_true",
                    help="control: leave SAVEDATA_DIRECTORY as the container's own name")
    args = ap.parse_args()
    host = console_ip()
    needs_account = args.step == "usb" or args.mode in ("copy", "patch")
    if needs_account and not args.account:
        raise SystemExit("--account is required")
    acct = account_id(args.account) if args.account else 0
    if args.step == "lua":
        return run_lua(host, args.mode, acct, args.wait, args.temp, not args.keep_dir)
    make_usb(host, acct, args.uid, args.name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
