#!/usr/bin/env python3
"""Put another account's PS4 save into your own save of the game, with no jailbreak.

A PS4 save belongs to the account in its sce_sys/param.sfo, which only the
system can write.  So the other save's files go into a save your own console
made for you instead: it keeps its owner and the game's keystone, and the game
loads it as yours.  SWRR can write a save of another game on a PS5 (13.60
measured) through a read-write mount the save service binds and then refuses;
the system commits what was written when SWRR closes (docs/save-resign.md).

    python3 ghostldr.py resign SOURCE [--game CUSA12345] [--dir NAME]
    python3 ghostldr.py resign SOURCE --check        after SWRR restarted

SOURCE is the other save, DECRYPTED: a folder of its files (Apollo's "Copy save
game to USB" folder PS4/APOLLO/<user>_<TITLE>_<DIR>/, or any folder with the
save's files, with or without its sce_sys), or a zip of one (Apollo's "Export
save game to Zip").  The game's title ID and the save's folder name come from
its sce_sys/param.sfo or Apollo's folder name, else from --game and --dir.
Its sce_sys/keystone (or --keystone FILE: the game's) lets ghostldr see the
whole present save first; it is never written.

What happens: ghostldr's ELF loader starts in SWRR (run.py elfldr) and runs
ghostldr/payloads/resign.elf, which takes the files, saves what your save
holds now back to this computer (the undo), and writes it.  Then close SWRR:
the PS5 writes the save as SWRR closes.
"""

from __future__ import annotations

import argparse
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

PAYLOAD = ROOT / "build/ghostldr/resign.elf"
TITLE_RE = re.compile(r"^[A-Z]{4}\d{5}$")
DIR_RE = re.compile(r"^[A-Za-z0-9_-][A-Za-z0-9_.-]{0,30}$")
APOLLO_DIR = re.compile(r"^[0-9a-fA-F]{8}_([A-Z]{4}\d{5})_(.+)$")
SWRR_TITLES = {"CUSA03474", "CUSA03492"}
MAX_ENTRIES = 512
WRITE, CHECK = 1, 2


class ResignError(Exception):
    pass


# ---------------------------------------------------------------- the source

def sfo_fields(data: bytes) -> dict[str, bytes]:
    """param.sfo: key -> raw value bytes"""
    if len(data) < 20 or data[:4] != b"\0PSF":
        return {}
    kt, dt, n = struct.unpack_from("<III", data, 8)
    out = {}
    for i in range(min(n, 128)):
        ko, fmt, ln, mx, do = struct.unpack_from("<HHIII", data, 20 + 16 * i)
        end = data.find(b"\0", kt + ko)
        key = data[kt + ko:end].decode("ascii", "replace")
        out[key] = data[dt + do:dt + do + ln]
    return out


def sfo_str(v: bytes | None) -> str:
    return (v or b"").split(b"\0", 1)[0].decode("utf-8", "replace")


class Source:
    """A decrypted save: its files (relative path -> local path, folders as None) and what it says it is."""

    def __init__(self, path: Path, title: str | None = None, dirname: str | None = None,
                 keystone: Path | None = None) -> None:
        self.tmp = None
        path = path.expanduser()
        if path.is_file() and zipfile.is_zipfile(path):
            self.tmp = tempfile.TemporaryDirectory(prefix="ghostldr-resign-")
            path = self._unzip(path, Path(self.tmp.name))
        if not path.is_dir():
            raise ResignError(f"{path}: not a folder or a zip of a save")
        self.root = self._save_root(path)
        sfo = self.root / "sce_sys/param.sfo"
        fields = sfo_fields(sfo.read_bytes()) if sfo.is_file() else {}
        m = APOLLO_DIR.match(self.root.name)
        self.title = title or sfo_str(fields.get("TITLE_ID")) or (m.group(1) if m else "")
        self.dir = dirname or sfo_str(fields.get("SAVEDATA_DIRECTORY")) or (m.group(2) if m else "")
        acct = fields.get("ACCOUNT_ID")
        self.account = f"{struct.unpack('<Q', acct)[0]:016x}" if acct and len(acct) == 8 else ""
        if not TITLE_RE.match(self.title):
            raise ResignError("the game's title ID isn't in the save (no sce_sys/param.sfo): add --game CUSA12345")
        if not DIR_RE.match(self.dir):
            raise ResignError("the save's folder name isn't in the save (no sce_sys/param.sfo): add --dir NAME "
                              "(the name your own save of the game has, e.g. SAVEDATA00)")
        if self.title in SWRR_TITLES:
            raise ResignError("SWRR's own saves can't be written this way: ghostldr runs from your Luac0re save")
        ks = keystone or (self.root / "sce_sys/keystone")
        self.fp, self.keystone = "", b""
        if ks.is_file():
            k = ks.read_bytes()
            if len(k) == 96 and k[:8] == b"keystone":
                self.fp, self.keystone = k[0x20:0x40].hex(), k
            elif keystone:
                raise ResignError(f"{keystone}: not a keystone (96 bytes starting 'keystone')")
        self.entries: list[tuple[str, Path | None]] = []
        for p in sorted(self.root.rglob("*")):
            rel = p.relative_to(self.root).as_posix()
            if rel == "sce_sys" or rel.startswith("sce_sys/") or p.name.startswith("._") or p.name == ".DS_Store":
                continue
            if p.is_symlink():
                raise ResignError(f"{rel}: a link, not a save file")
            if any(part in (".", "..") or not re.match(r"^[^\x00-\x1f/\\]+$", part) for part in rel.split("/")):
                raise ResignError(f"{rel}: unsupported characters in a save file's name")
            self.entries.append((rel, None if p.is_dir() else p))
        if not any(p for _, p in self.entries):
            raise ResignError(f"{self.root}: no save files in it (besides sce_sys)")
        if len(self.entries) > MAX_ENTRIES:
            raise ResignError(f"{self.root}: {len(self.entries)} files and folders; at most {MAX_ENTRIES}")

    @staticmethod
    def _unzip(zpath: Path, dest: Path) -> Path:
        with zipfile.ZipFile(zpath) as z:
            for info in z.infolist():
                name = info.filename
                if name.startswith("/") or ".." in Path(name).parts or "\\" in name:
                    raise ResignError(f"{zpath}: unsafe path in the zip: {name}")
            z.extractall(dest)
        return dest

    @staticmethod
    def _save_root(path: Path) -> Path:
        """the folder holding the save's files: path itself, or its only subfolder (a zip's <DIR>/)"""
        if (path / "sce_sys").is_dir() or any(p.is_file() for p in path.iterdir() if not p.name.startswith(".")):
            return path
        subs = [p for p in path.iterdir() if p.is_dir() and not p.name.startswith((".", "__MACOSX"))]
        return Source._save_root(subs[0]) if len(subs) == 1 else path

    @property
    def size(self) -> int:
        return sum(p.stat().st_size for _, p in self.entries if p)

    def stream(self, mode: int, user: int = -1) -> bytes:
        head = struct.pack("<8sIIi16s32s80sI", b"GLDRRSGN", 1, mode, user, self.title.encode(), self.dir.encode(),
                           self.fp.encode(), len(self.entries))
        parts = [head]
        for rel, p in self.entries:
            name = rel.encode()
            if p is None:
                parts.append(struct.pack("<BH", 2, len(name)) + name)
            else:
                data = p.read_bytes()
                parts.append(struct.pack("<BH", 1, len(name)) + name + struct.pack("<Q", len(data)) + data)
        return b"".join(parts)


# ---------------------------------------------------------------- the console

def port_open(host: str, port: int, timeout: float = 1.0) -> bool:
    try:
        socket.create_connection((host, port), timeout=timeout).close()
        return True
    except OSError:
        return False


def start_elfldr(host: str, port: int | None, log=print) -> int:
    """ghostldr's ELF loader on the console (run.py elfldr); its port.  On a jailbroken console 9021 is the
    jailbreak's own loader, so ghostldr's goes on 9022."""
    if port is None:
        if not port_open(host, 9021):
            port = 9021
        else:
            port = 9022
            if port_open(host, 9022):
                log(f"using the ELF loader already listening on {host}:9022")
                return port
    elif port_open(host, port):
        log(f"using the ELF loader already listening on {host}:{port}")
        return port
    if not port_open(host, 9026):
        raise ResignError(f"Luac0re's loader isn't listening on {host}:9026: start SWRR, click the right half of the "
                          "touchpad at PRESS START, then OPTIONS -> HALL OF FAME")
    argv = [sys.executable, str(HERE / "run.py"), "elfldr", "--host", host, "--elfldr-port", str(port), "--wait", "1"]
    if (ROOT / "build/ghostldr/ghostldr.lua").exists() and not (ROOT / "ghostldr/native/Makefile").exists():
        argv.append("--no-build")
    log(f"starting ghostldr's ELF loader in SWRR (TCP {port})")
    out = subprocess.run(argv, capture_output=True, text=True, timeout=180)
    if not port_open(host, port, 3):
        raise ResignError("ghostldr's ELF loader did not start:\n" + (out.stdout + out.stderr)[-2000:])
    return port


def recv_line(sock: socket.socket, buf: bytearray) -> str | None:
    while b"\n" not in buf:
        chunk = sock.recv(65536)
        if not chunk:
            return None
        buf += chunk
    line, _, rest = bytes(buf).partition(b"\n")
    buf[:] = rest
    return line.decode("utf-8", "replace")


def recv_exact(sock: socket.socket, buf: bytearray, n: int) -> bytes:
    while len(buf) < n:
        chunk = sock.recv(min(1 << 20, max(65536, n - len(buf))))
        if not chunk:
            raise ResignError("the console's connection ended early")
        buf += chunk
    out = bytes(buf[:n])
    del buf[:n]
    return out


def receive_backup(sock: socket.socket, buf: bytearray, n: int, dest: Path) -> int:
    files = 0
    for _ in range(n):
        kind, ln = struct.unpack("<BH", recv_exact(sock, buf, 3))
        rel = recv_exact(sock, buf, ln).decode("utf-8", "replace")
        if rel.startswith("/") or ".." in Path(rel).parts:
            raise ResignError(f"the console sent an unsafe path: {rel}")
        p = dest / rel
        if kind == 2:
            p.mkdir(parents=True, exist_ok=True)
            continue
        size, = struct.unpack("<Q", recv_exact(sock, buf, 8))
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(recv_exact(sock, buf, size))
        files += 1
    return files


def run(src: Source, mode: int, host: str, elfldr_port: int | None, backup: Path | None, log=print) -> dict:
    if not PAYLOAD.exists():
        raise ResignError(f"{PAYLOAD.relative_to(ROOT)} missing: make -C ghostldr/payloads "
                          "OO_PS4_TOOLCHAIN=/path/to/PS4Toolchain (and make -C ghostldr/native)")
    port = start_elfldr(host, elfldr_port, log)
    loader = socket.create_connection((host, port), timeout=30)
    loader.sendall(PAYLOAD.read_bytes())
    lbuf = bytearray()
    data_port = None
    while data_port is None:
        line = recv_line(loader, lbuf)
        if line is None:
            raise ResignError("the ELF loader closed the connection before the payload said where it listens")
        log(line)
        m = re.search(r"resign: listening on TCP (\d+)", line)
        if m:
            data_port = int(m.group(1))
        if "refused" in line or "crashed" in line or "no free port" in line:
            raise ResignError(line)
    stream = src.stream(mode)
    log(f"sending {len(src.entries)} files and folders ({len(stream):,} bytes) for {src.title}/{src.dir}")
    s = socket.create_connection((host, data_port), timeout=600)
    s.sendall(stream)
    buf, result = bytearray(), {}
    while True:
        line = recv_line(s, buf)
        if line is None:
            break
        if line.startswith("BACKUP "):
            n = int(line.split()[1])
            if backup is None:
                raise ResignError("the console sent a backup and no folder was given for it")
            backup.mkdir(parents=True, exist_ok=True)
            got = receive_backup(s, buf, n, backup)
            if src.keystone and (backup / "sce_sys").is_dir():
                # the game's keystone (an app can't read the save's own): the undo can then remove what the
                # resign added
                (backup / "sce_sys/keystone").write_bytes(src.keystone)
            log(f"your save as it was before (the undo): {got} files in {backup}")
            result["backup"] = str(backup)
            continue
        if line.startswith("RESULT "):
            result.update(dict(re.findall(r"(\w+)=(\S+)", line)))
        elif line.startswith("WHY "):
            result["why"] = line[4:]
        log(line)
    s.close()
    loader.settimeout(10)
    try:   # the payload's own lines came on the data connection already: only the loader's
        while (line := recv_line(loader, lbuf)) is not None:
            if line.startswith("[elfldr]"):
                log(line)
    except OSError:
        pass
    loader.close()
    return result


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="ghostldr.py resign", description=__doc__.split("\n\n")[0],
                                 epilog="docs/save-resign.md walks through it.")
    ap.add_argument("source", type=Path, metavar="SOURCE", help="the other save, decrypted: a folder or a zip")
    ap.add_argument("--game", metavar="ID", help="the game's title ID, when the save doesn't say")
    ap.add_argument("--dir", metavar="NAME", help="your save's folder name, when the save doesn't say")
    ap.add_argument("--keystone", type=Path, metavar="FILE", help="the game's keystone, when the save has none")
    ap.add_argument("--check", action="store_true", help="after SWRR restarted: compare your save with SOURCE")
    ap.add_argument("--backup", type=Path, metavar="FOLDER",
                    help="where your save's present files go first (default: beside SOURCE)")
    ap.add_argument("--host", help="the console's address (default: found on the network)")
    ap.add_argument("--elfldr-port", type=int, help="ghostldr's ELF loader port (default 9021, 9022 on a "
                    "jailbroken console)")
    args = ap.parse_args(argv)
    try:
        src = Source(args.source, args.game, args.dir, args.keystone)
        print(f"{src.title}/{src.dir}: {sum(1 for _, p in src.entries if p)} files, {src.size:,} bytes"
              + (f", from account {src.account}" if src.account else "")
              + ("" if src.fp else ", no keystone (files only your save has may stay)"))
        if args.check and not src.fp:
            raise ResignError("--check needs the game's keystone: the save's sce_sys/keystone or --keystone FILE")
        from console import console_ip
        host = args.host or console_ip()
        backup = None
        if not args.check:
            base = args.backup or args.source.expanduser().resolve().parent
            backup = base / f"{src.title}-{src.dir}-before-resign-{time.strftime('%Y%m%d-%H%M%S')}"
        r = run(src, CHECK if args.check else WRITE, host, args.elfldr_port, backup)
        if r.get("rc") != "00000000":
            print(f"\nresign: not done: {r.get('why') or 'see the lines above'}", file=sys.stderr)
            changed = sum(int(r.get(k, "0")) for k in ("written", "created", "deleted", "dirs"))
            if changed and r.get("needs_exit") == "1":
                print("resign: some of it was written into your save; if you close SWRR the PS5 keeps that. "
                      f"Your save as it was is in {backup}: ghostldr.py resign \"{backup}\" --game {src.title} "
                      f"--dir {src.dir} puts it back (in a fresh SWRR session).", file=sys.stderr)
            elif not changed:
                print("resign: nothing was written; your save is as it was.", file=sys.stderr)
            return 1
        if args.check:
            print(f"\nresign: your {src.title} save {src.dir} holds exactly the files of {args.source}.")
            return 0
        if r.get("why"):
            print(f"\nresign: note: {r['why']}")
        if r.get("committed") == "1":
            print("\nresign: written and committed. Start the game.")
        else:
            print(f"""
resign: written into your {src.title} save {src.dir} and checked.  NOW CLOSE SWRR so the PS5 saves it:
  PS button, then on SWRR's tile: Options -> Close Application (or start another game).
Then start {src.title}.  To check afterwards: start SWRR's loader again and run
  python3 ghostldr.py resign "{args.source}" --check""")
        return 0
    except ResignError as e:
        print(f"resign: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
