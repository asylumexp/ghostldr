#!/usr/bin/env python3
"""Make a copy of SWRR's Luac0re save for another PSN account (USB import).

A PS4 save's image is encrypted with a key only a console can use, so the
copy is made on our console by the system itself (ghostldr/lua/save_resign.lua
through Luac0re's loader on 9026): a new save container (SLUS-RESGN, or
SLES-RESGN on EU) gets the Luac0re save's files and a param.sfo with the other
account's ACCOUNT_ID and the save's own SAVEDATA_DIRECTORY.  sce_sys is not writable from the app, so the
param.sfo is patched over FTP as root (ftpsrv: needs the jailbreak) while the
copy stays mounted; the unmount commits (re-signs) it.  The raw image and key
are then fetched over FTP too and laid out
the way the PS4/PS5 "copy from USB" import reads them:

    PS4/SAVEDATA/<account id, 16 hex>/CUSA03474/SLUS-20268      (the image)
    PS4/SAVEDATA/<account id, 16 hex>/CUSA03474/SLUS-20268.bin  (sealed key)

(CUSA03492/SLES-50366 on EU.)  The save is for the SWRR region that makes it:
run this with that region's SWRR at the loader (found over FTP, or --region).
A save also carries its game's keystone, so a US copy relabelled for EU is
reported corrupted; make an EU save on EU SWRR.

    python3 ghostldr/host/resign_save.py lua copy --account 1234567890123456789
    python3 ghostldr/host/resign_save.py lua show
    python3 ghostldr/host/resign_save.py usb --account 1234567890123456789 --name SomeUser
    python3 ghostldr/host/resign_save.py lua delete

With --save ZIP (tools/make_save.py's zip for this region) the copy
gets that save's files instead of the console's Luac0re save's, and its size
(SAVEDATA_BLOCKS): the autoloader's encrypted save (docs/autoload.md).
--anyone makes it no one's: ACCOUNT_ID 0, and the PARAMS user ID and the
console's PSID HMAC zeroed, for Save Wizard to resign:

    python3 ghostldr/host/resign_save.py lua copy --anyone --save build/saves/ghostldr-v1.0.0-save-EU-CUSA03492.zip
    python3 ghostldr/host/resign_save.py usb --anyone --name eu      # build/resigned/eu-CUSA03492-SLES-50366.zip

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
import sys
import time
import zipfile
from pathlib import Path

from console import console_ip
from swrr import REGIONS, ftp_list, running

ROOT = Path(__file__).resolve().parents[2]
LUA = ROOT / "ghostldr/lua/save_resign.lua"
OUT = ROOT / "build/resigned"
LOG_PORT = 18196
# the running SWRR region (main() sets it): its sandbox and save area hold the copy
REGION = REGIONS["us"]


def save_files(path: Path) -> dict[str, bytes]:
    """A make_save zip's files, by path in the save (sce_sys is the system's but for the keystone)."""
    sys.path.insert(0, str(ROOT / "tools"))
    import make_save
    files = make_save.read_save(path)
    sfo = files["sce_sys/param.sfo"]
    if make_save.sfo_str(sfo, "TITLE_ID") != REGION["title"]:
        raise SystemExit(f"{path} is a save for {make_save.sfo_str(sfo, 'TITLE_ID')}; SWRR {REGION['title']} runs")
    return files


def sfo_blocks(sfo: bytes) -> int:
    kt, dt, n = struct.unpack_from("<III", sfo, 8)
    for i in range(n):
        ko, _, _, _, do = struct.unpack_from("<HHIII", sfo, 20 + 16 * i)
        if sfo[kt + ko:sfo.index(0, kt + ko)] == b"SAVEDATA_BLOCKS":
            return struct.unpack_from("<Q", sfo, dt + do)[0]
    raise SystemExit("the save's param.sfo has no SAVEDATA_BLOCKS")


def account_id(text: str) -> int:
    v = int(text, 16) if text.lower().startswith("0x") else int(text)
    if not 0 < v < 1 << 64:
        raise SystemExit("account ID out of range")
    return v


def patch_sfo(data: bytes, acct: int, directory: str, anyone: bool = False) -> bytes:
    """ACCOUNT_ID (u64, little endian) and SAVEDATA_DIRECTORY of a param.sfo;
    anyone: the PARAMS user ID and the console's PSID HMAC (PARAMS+4..+0x28) zeroed too."""
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
        elif key == "PARAMS" and anyone and ln >= 0x28:
            out[dt + do + 4:dt + do + 0x28] = bytes(0x24)
    if found != {"ACCOUNT_ID", "SAVEDATA_DIRECTORY"}:
        raise ValueError(f"param.sfo lacks {({'ACCOUNT_ID', 'SAVEDATA_DIRECTORY'} - found)}")
    return bytes(out)


def ftp() -> ftplib.FTP:
    f = ftplib.FTP()
    f.connect(console_ip(), 2121, timeout=30)
    f.login()
    f.voidcmd("TYPE I")
    return f


def host_patch(mount_point: str, acct: int, directory: str, files: dict[str, bytes] | None = None,
               anyone: bool = False) -> None:
    """Patch the mounted copy's param.sfo as root (ftpsrv, jailbreak), after
    writing files (a make_save save's, but its sce_sys) into it."""
    title = REGION["title"]
    remote = f"/mnt/sandbox/{title}_000{mount_point}/sce_sys/param.sfo"
    f = ftp()
    top = f"/mnt/sandbox/{title}_000{mount_point}"
    for rel in sorted(files or {}):      # before the param: patching it ends the hold
        if rel.startswith("sce_sys/"):
            continue
        parts = rel.split("/")
        for i in range(1, len(parts)):
            try:
                f.mkd(f"{top}/{'/'.join(parts[:i])}")
            except ftplib.error_perm:
                pass                     # there already
        f.storbinary(f"STOR {top}/{rel}", io.BytesIO(files[rel]))
        print(f"host: wrote {rel} ({len(files[rel])} bytes)", flush=True)
    icon = io.BytesIO()                 # the save list's icon, before the param (that ends the hold)
    try:
        f.retrbinary(f"RETR /mnt/sandbox/{title}_000/savedata0/sce_sys/icon0.png", icon.write)
        f.storbinary(f"STOR {remote.replace('param.sfo', 'icon0.png')}", io.BytesIO(icon.getvalue()))
    except ftplib.Error as e:
        print(f"host: icon0.png not copied ({e})", flush=True)
    data = io.BytesIO()
    f.retrbinary(f"RETR {remote}", data.write)
    patched = patch_sfo(data.getvalue(), acct, directory, anyone)
    f.storbinary(f"STOR {remote}", io.BytesIO(patched))
    f.quit()
    print(f"host: patched {remote} ({len(patched)} bytes)", flush=True)


def run_lua(host: str, mode: str, acct: int, wait: float, temp: str, patch_dir: bool = True,
            files: dict[str, bytes] | None = None, anyone: bool = False) -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect((host, 9))
        local_ip = probe.getsockname()[0]
    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.bind(("0.0.0.0", LOG_PORT))
    listener.settimeout(1)
    acct_bytes = ", ".join(f"0x{b:02x}" for b in acct.to_bytes(8, "little"))
    lua = (f'MODE = "{mode}"\nTEMP_DIR = "{temp}"\nORIGINAL_DIR = "{REGION["dir"]}"\n'
           f'ACCOUNT_ID_BYTES = {{{acct_bytes}}}\n'
           f'PATCH_DIR = {"true" if patch_dir else "false"}\n'
           + (f'BLOCKS = {sfo_blocks(files["sce_sys/param.sfo"])}\n' if files else "") +
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
                host_patch(line.rsplit(" ", 1)[1], acct, REGION["dir"] if patch_dir else temp, files, anyone)
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


def find_uid(temp: str) -> str:
    """The local user (8 hex digits) whose saves hold the copy."""
    f = ftp()
    try:
        for uid in ftp_list(f, "/user/home"):
            try:
                if f"sdimg_{temp}" in ftp_list(f, f"/user/home/{uid}/savedata/{REGION['title']}"):
                    return uid
            except ftplib.Error:
                continue
    finally:
        f.quit()
    raise SystemExit(f"no user has {temp}: run `lua copy` first")


def make_usb(host: str, acct: int, uid: str | None, name: str, temp: str) -> Path:
    title, directory = REGION["title"], REGION["dir"]
    uid = uid or find_uid(temp)
    base = f"/user/home/{uid}/savedata/{title}"
    image = fetch(host, f"{base}/sdimg_{temp}")
    key = fetch(host, f"{base}/{temp}.bin")
    if key[:8] != b"pfsSKKey" or len(key) != 96:
        raise SystemExit(f"{temp}.bin is not a sealed key ({len(key)} bytes)")
    if image[0x18] != 0:
        raise SystemExit(f"image header byte 0x18 is {image[0x18]} (not cleanly committed)")
    root = OUT / name
    shutil.rmtree(root, ignore_errors=True)
    dest = root / "PS4/SAVEDATA" / f"{acct:016x}" / title
    dest.mkdir(parents=True)
    (dest / directory).write_bytes(image)
    (dest / f"{directory}.bin").write_bytes(key)
    archive = OUT / f"{name}-{title}-{directory}.zip"
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
    ap.add_argument("--anyone", action="store_true",
                    help="no account: ACCOUNT_ID 0, no PARAMS user ID or console PSID HMAC (for Save Wizard)")
    ap.add_argument("--save", type=Path, help="lua copy: the files of this tools/make_save.py zip, and its size")
    ap.add_argument("--uid", help="local user id of the PS5 user running SWRR (8 hex; default: found over FTP)")
    ap.add_argument("--name", default="resigned", help="output folder name under build/resigned/")
    ap.add_argument("--wait", type=float, default=120)
    ap.add_argument("--region", choices=sorted(REGIONS),
                    help="SWRR region at the loader (default: the running one, found over FTP)")
    ap.add_argument("--temp", help="the copy's container name (10 characters; default SLUS-RESGN / SLES-RESGN)")
    ap.add_argument("--keep-dir", action="store_true",
                    help="control: leave SAVEDATA_DIRECTORY as the container's own name")
    args = ap.parse_args()
    host = console_ip()
    needs_account = args.step == "usb" or args.mode in ("copy", "patch")
    if needs_account and not (args.account or args.anyone):
        raise SystemExit("--account (or --anyone) is required")
    acct = account_id(args.account) if args.account and not args.anyone else 0
    global REGION
    if args.region:
        REGION = REGIONS[args.region]
    else:
        f = ftp()
        try:
            REGION = running(f)
        finally:
            f.quit()
    print(f"SWRR {REGION['title']} (save {REGION['dir']})", flush=True)
    temp = args.temp or REGION["temp"]
    if args.step == "lua":
        files = save_files(args.save) if args.save else None
        return run_lua(host, args.mode, acct, args.wait, temp, not args.keep_dir, files, args.anyone)
    make_usb(host, acct, args.uid, args.name, temp)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
