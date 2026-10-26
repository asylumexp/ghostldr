#!/usr/bin/env python3
"""Games on a USB drive (docs/usb.md): what the PS5 finds on a drive, putting
games on it from this computer, and checking what one holds.

The PS5 finds games by what is in their folders, not by the folders' names
(ext/src/approot.c; scan() here follows the same rules):

  - a game is a folder with eboot.bin in it (a dump's app0, as dumped), or a
    folder with app0/eboot.bin (then that folder may also hold `prefix` and
    `meta.bin`, as copy_game() and gfs_pack.py --usb write them);
  - its title ID is sce_sys/param.sfo's TITLE_ID, else meta.bin's, else one
    in the folder's name (CUSA12345, "Ghost of Tsushima (CUSA11456)");
  - folders are looked into down to SCAN_DEPTH levels below the drive's top
    (or the folder a boot names), SCAN_DIRS folders in all; a game's own
    folders aren't looked into, nor names starting with "."
  - two copies of one title: the first by folder path is booted by --title,
    and the game selector shows both.

The drive is exFAT (FAT32 holds no file of 4 GiB or more), with an MBR or GPT
partition table or none.  Progress goes to SWRR's save containers,
<prefix>SAVE, as for installed titles.

    python3 ghostldr.py copy DUMP [DRIVE]                 # put a game on a drive (the usual way)
    python3 ghostldr.py check [DRIVE]                     # what the PS5 would find there, and why not
    python3 ghostldr/host/usb_drive.py list [DRIVE]       # the games on drives (one line each)
    python3 ghostldr/host/usb_drive.py check DRIVE        # as ghostldr.py check
    python3 ghostldr/host/usb_drive.py remove DRIVE CUSA12345

DRIVE is where this computer has the drive mounted, or, for a drive plugged
into the PS5, an FTP address: ghostldr's own server while a title runs,
ftp://PS5:1212/ghostldr_usb (docs/ftp.md), or a jailbroken PS5's ftpsrv,
ftp://PS5:2121/mnt/usbN.  The same layout is written through FTP.  Over
FTP the drive's file system and free space aren't known, so the FAT32 size
limit and the space check are skipped (the PS5 mounts exFAT and FAT32
drives at /mnt/usbN).
"""

from __future__ import annotations

import argparse
import ftplib
import hashlib
import io
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import unicodedata
import urllib.parse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

import gfs_catalog as gc
import gfs_meta as gm
import profiles as titles

LIBRARY = "ghostldr"             # where copy_game() puts games unless told otherwise
PREFIX_FILE = "prefix"
FAT_FILE_MAX = (4 << 30) - 1
APP_ROOT = "usb:/"               # run.py --app-root for the games anywhere on a drive
# the console's search (ext/src/approot.c): keep these the same
SCAN_DEPTH = 4                   # folder levels below the top looked into
SCAN_DIRS = 256                  # folders listed at most
SCAN_ENTRIES = 4096              # entries read per folder at most
SCAN_MAX = 64                    # games listed at most (the selector's SEL_MAX_TITLES)
# macOS's companions and indexes: never part of a title (the console hides "._" names)
JUNK = re.compile(r"^(\._.*|\.DS_Store|\.Spotlight-V100|\.fseventsd|\.Trashes|\.TemporaryItems)$")


class DriveError(Exception):
    pass


def library(drive: Path) -> Path:
    return Path(drive) / LIBRARY


# ---- what the PS5 finds on a drive (ext/src/approot.c's rules) ----

@dataclass
class Game:
    """A game folder found on a drive.  Paths are relative to the drive."""
    folder: str              # the game's folder: app0's parent, or app0 itself
    app0: str                # the folder with eboot.bin
    id: str = ""
    id_from: str = ""        # "param.sfo", "meta.bin", "folder name"
    name: str = ""
    version: str = ""
    problem: str = ""        # why the PS5 can't start it; "" when it can
    duplicate_of: str = ""   # a second copy: the first copy's folder
    depth: int = 0           # folder levels below the top
    too_deep: bool = False   # past the PS5's search (check() looks further)

    @property
    def ok(self) -> bool:
        return not self.problem and not self.too_deep

    @property
    def layered(self) -> bool:
        """app0 inside its folder, with room for prefix and meta.bin beside it."""
        return self.app0 != self.folder


@dataclass
class Scan:
    top: str                 # the folder looked in, relative to the drive ("" its top)
    games: list              # Game, as the console orders them: bootable by ID then folder, then the others
    folders: int = 0         # folders listed
    cut: bool = False        # the search stopped short (SCAN_DIRS, SCAN_ENTRIES, SCAN_MAX)

    @property
    def bootable(self) -> list:
        return [g for g in self.games if g.ok]

    def first(self, title: str) -> "Game | None":
        return next((g for g in self.games if g.ok and g.id == title and not g.duplicate_of), None)


def _skip_dir(name: str) -> bool:
    return name.startswith(".") or name.lower() in ("system volume information", "$recycle.bin", "sce_sys",
                                                    "sce_module")


def _title_name(s: str) -> bool:
    return bool(re.fullmatch(r"[A-Z0-9_-]{1,15}", s))


def name_title(name: str, whole: bool) -> str:
    """A title ID in a folder's name: all of it in the old ghostldr/<TITLE>/app0
    layout (whole), else four letters and five digits anywhere in it."""
    if whole and _title_name(name):
        return name
    m = re.search(r"(?<![A-Za-z0-9])([A-Za-z]{4}[0-9]{5})(?![0-9])", name)
    return m[1].upper() if m else ""


def _join(*parts: str) -> str:
    return "/".join(p.strip("/") for p in parts if p.strip("/"))


def identify(d, g: Game) -> None:
    """The game's title ID (and its name and version) as the console finds it."""
    sfo_raw = d.read_bytes(_join(g.app0, "sce_sys/param.sfo"))
    sfo = titles.parse_sfo(sfo_raw)
    sid = str(sfo.get("TITLE_ID", ""))[:15]
    if _title_name(sid):
        g.id, g.id_from = sid, "param.sfo"
    g.name, g.version = str(sfo.get("TITLE", "")), str(sfo.get("APP_VER", ""))
    meta = d.read_bytes(_join(g.folder, gm.META_FILE)) if not g.id or not g.name else None
    if meta and meta[:4] == b"GFSM" and len(meta) >= 0x20:
        mid = meta[0x10:0x1F].split(b"\0")[0].decode(errors="replace")
        if not g.id and _title_name(mid):
            g.id, g.id_from = mid, "meta.bin"
        try:
            info = gm.parse(meta)
            g.name = g.name or info.get("name", "")
            g.version = g.version or info.get("version", "")
        except Exception:   # noqa: BLE001 - a damaged meta.bin names nothing
            pass
    if not g.id:
        base = g.folder.rsplit("/", 1)[-1]
        g.id = name_title(base, g.layered)
        g.id_from = "folder name" if g.id else ""
    if not g.id:
        g.problem = ("its sce_sys/param.sfo has no usable TITLE_ID: name the folder after the game's ID"
                     if sfo_raw is not None else
                     "no sce_sys/param.sfo: name the folder after the game's ID (CUSA12345)")
    if g.id and not g.name:
        try:
            g.name = titles.load(g.id).get("name", "") or ""
        except titles.ProfileError:
            pass


def scan(drive, top: str = "", depth: int = SCAN_DEPTH, dirs: int = SCAN_DIRS) -> Scan:
    """The games under `top` on a drive (a mount point, ftp://..., or an open
    drive), found as the PS5 finds them: breadth first, a folder with
    eboot.bin or app0/eboot.bin is a game and isn't looked into further.
    depth and dirs bound the search as the console's (check() looks further
    to find games the console wouldn't)."""
    d = open_drive(drive)
    out = Scan(top.strip("/"), [])
    queue_: list[tuple[str, int]] = [(out.top, 0)]
    head = 0
    while head < len(queue_):
        rel, level = queue_[head]
        head += 1
        entries = d.listdir(rel)
        if entries is None:
            continue
        out.folders += 1
        if len(entries) > SCAN_ENTRIES:
            entries = entries[:SCAN_ENTRIES]
            out.cut = True
        first = len(queue_)
        names = {}
        for name, is_dir, _ in entries:
            low = name.lower()
            if not is_dir and low == "eboot.bin":
                names["eboot"] = name
            elif is_dir and low in ("app0", "sce_sys"):
                names[low] = name
            elif is_dir and level < depth and not _skip_dir(name) and len("/ghostldr_usb/" + _join(rel, name)) < 250:
                if len(queue_) < dirs:
                    queue_.append((_join(rel, name), level + 1))
                else:
                    out.cut = True
        if not names:
            continue
        del queue_[first:]          # a game's folder, or what is left of one: not looked into
        if len(out.games) >= SCAN_MAX:
            out.cut = True
            continue
        if "eboot" in names:
            g = Game(rel, rel, depth=level)
        elif "app0" in names and d.is_file(_join(rel, names["app0"], "eboot.bin")):
            g = Game(rel, _join(rel, names["app0"]), depth=level)
        else:
            app0 = "app0" in names
            g = Game(rel, _join(rel, names["app0"]) if app0 else rel, depth=level)
            g.id = name_title(rel.rsplit("/", 1)[-1], app0)
            g.problem = ("its app0 folder has no eboot.bin (copied in full?)" if app0 else
                         "a game's sce_sys folder but no eboot.bin (copied in full?)")
            out.games.append(g)
            continue
        identify(d, g)
        out.games.append(g)
    out.cut = out.cut or head < len(queue_)
    # as the console orders them: bootable first, by ID, then by folder; second copies marked
    out.games.sort(key=lambda g: (bool(g.problem), g.id.encode(), ("/ghostldr_usb/" + g.app0).encode()))
    firsts: dict[str, Game] = {}
    for g in out.games:
        if g.problem:
            continue
        if g.id in firsts:
            g.duplicate_of = firsts[g.id].folder
        else:
            firsts[g.id] = g
    return out


def drive_titles(drive) -> dict[str, Path]:
    """{title ID: its app0} for the games the PS5 would boot from a drive
    mounted here (each title's first copy)."""
    try:
        sc = scan(LocalDrive(Path(drive)))
    except OSError:
        return {}
    return {g.id: Path(drive) / g.app0 for g in sc.games if g.ok and not g.duplicate_of}


def mount_table() -> dict[Path, str]:
    """{mount point: file system} of what is mounted here ("exfat", "msdos", ...)."""
    out: dict[Path, str] = {}
    try:
        if sys.platform.startswith("linux"):
            for ln in Path("/proc/mounts").read_text().splitlines():
                parts = ln.split()
                if len(parts) >= 3:
                    mp = parts[1].replace("\\040", " ")
                    out[Path(mp)] = {"vfat": "msdos", "fat": "msdos"}.get(parts[2], parts[2])
        else:
            text = subprocess.run(["mount"], capture_output=True, text=True, timeout=10).stdout
            for ln in text.splitlines():
                m = re.match(r"^.+? on (.+) \((\w+)[,)]", ln)
                if m:
                    out[Path(m[1])] = m[2].lower()
    except (OSError, subprocess.SubprocessError):
        pass
    return out


USB_FS = ("exfat", "msdos", "vfat", "fat32")


def mounted_drives() -> list[Path]:
    """Mounted volumes that could be the PS5's USB drive: exFAT or FAT, or
    with a ghostldr folder (macOS /Volumes, Linux /media and /run/media,
    Windows' drive letters)."""
    if os.name == "nt":                 # drive letters other than Windows' own
        system = (os.environ.get("SystemDrive") or "C:").upper()
        return [Path(f"{c}:\\") for c in "DEFGHIJKLMNOPQRSTUVWXYZ"
                if f"{c}:" != system and os.path.exists(f"{c}:\\")
                and (fs_type(Path(f"{c}:\\")) in USB_FS or Path(f"{c}:\\{LIBRARY}").is_dir())]
    roots = [Path("/Volumes")]
    user = os.environ.get("USER", "")
    roots += [Path("/media") / user, Path("/run/media") / user, Path("/media"), Path("/mnt")]
    table = mount_table()
    found = []
    for r in roots:
        try:
            for v in sorted(r.iterdir()):
                if v in found or not v.is_dir() or os.path.realpath(v) == "/":
                    continue
                if table.get(v) in USB_FS or (v / LIBRARY).is_dir():
                    found.append(v)
        except OSError:
            continue
    return found


def find_drive(given: Path | None = None) -> Path | None:
    """The drive to look at: the one given, else the only mounted one with
    games, else the only exFAT or FAT one."""
    if given:
        return Path(given)
    drives = mounted_drives()
    if len(drives) == 1:
        return drives[0]
    with_games = [v for v in drives if drive_titles(v)]
    return with_games[0] if len(with_games) == 1 else None


def fs_type(path: Path) -> str | None:
    """The file system the path is on ("exfat", "msdos", ...), or None when unknown here."""
    try:
        if os.name == "nt":
            import ctypes
            buf = ctypes.create_unicode_buffer(64)
            ok = ctypes.windll.kernel32.GetVolumeInformationW(ctypes.c_wchar_p(Path(path).anchor or str(path)), None,
                                                              0, None, None, None, buf, len(buf))
            name = buf.value.lower() if ok else ""
            return {"fat32": "msdos", "fat": "msdos"}.get(name, name) or None
        here = Path(os.path.realpath(path))
        mounts = [(mp, fs) for mp, fs in mount_table().items() if here == mp or mp in here.parents]
        if mounts:                       # the mount the path is on: the longest mount point that holds it
            return max(mounts, key=lambda m: len(str(m[0])))[1]
        if sys.platform == "darwin":
            out = subprocess.run(["diskutil", "info", str(path)], capture_output=True, text=True, timeout=20).stdout
            m = re.search(r"Type \(Bundle\):\s+(\S+)", out)
            return m[1].lower() if m else None
        out = subprocess.run(["findmnt", "-no", "FSTYPE", "--target", str(path)], capture_output=True, text=True,
                             timeout=20).stdout.strip()
        return {"vfat": "msdos", "fat": "msdos"}.get(out, out) or None
    except (OSError, subprocess.SubprocessError, AttributeError, ValueError):
        return None


def tree(src: Path) -> dict[str, int]:
    """{relative path: size} of the files in a dump, macOS's metadata left out."""
    out = {}
    for p in sorted(src.rglob("*")):
        rel = p.relative_to(src)
        if any(JUNK.match(part) for part in rel.parts):
            continue
        if p.is_file():
            out[rel.as_posix()] = p.stat().st_size
    return out


def check_files(files: dict[str, int], fs: str | None) -> list[str]:
    """Why these files cannot go on a drive of file system fs (empty: they can)."""
    problems = []
    if fs == "msdos":
        big = [f for f, s in files.items() if s > FAT_FILE_MAX]
        if big:
            problems.append(f"FAT32 holds no file of 4 GiB or more ({big[0]}, {files[big[0]] / 2**30:.1f} GiB"
                            f"{f' and {len(big) - 1} more' if len(big) > 1 else ''}): format the drive exFAT")
    long_names = [f for f in files if any(len(part.encode()) > 255 for part in f.split("/"))]
    if long_names:
        problems.append(f"names longer than 255 bytes: {long_names[0]}")
    return problems


def _same(src: Path, dst: Path) -> bool:
    try:
        a, b = src.stat(), dst.stat()
        return a.st_size == b.st_size and int(b.st_mtime) >= int(a.st_mtime) - 2
    except OSError:
        return False


def _clean_junk(folder: Path) -> int:
    """Remove the AppleDouble files macOS leaves next to copies on exFAT/FAT."""
    n = 0
    for p in list(folder.rglob("._*")):
        try:
            p.unlink()
            n += 1
        except OSError:
            pass          # the system's own, gone with its file or not deletable: the console hides them anyway
    return n


def _hash_file(path: Path, uncached: bool = False) -> str | None:
    """SHA-256 of a file; uncached: read past this computer's file cache
    where it can (macOS F_NOCACHE), so a copy is checked as the drive has it."""
    h = hashlib.sha256()
    try:
        with open(path, "rb", buffering=0) as f:
            if uncached and sys.platform == "darwin":
                try:
                    import fcntl
                    fcntl.fcntl(f.fileno(), 48, 1)          # F_NOCACHE
                except (ImportError, OSError):
                    pass
            while chunk := f.read(8 << 20):
                h.update(chunk)
    except OSError:
        return None
    return h.hexdigest()


class LocalDrive:
    """A drive mounted on this computer."""

    def __init__(self, root: Path):
        self.root = Path(root)
        self.label = str(self.root)

    def where(self, rel: str) -> str:
        return str(self.root / rel)

    def check_root(self) -> None:
        if not self.root.is_dir():
            raise DriveError(f"{self.root}: no such folder (the drive's mount point)")

    def fs_type(self) -> str | None:
        return fs_type(self.root)

    def free(self) -> int | None:
        return shutil.disk_usage(self.root).free

    def is_dir(self, rel: str) -> bool:
        return (self.root / rel).is_dir()

    def is_file(self, rel: str) -> bool:
        return (self.root / rel).is_file()

    def listdir(self, rel: str) -> list[tuple[str, bool, int]] | None:
        """(name, is a folder, size) for each entry of rel; None when it isn't a folder."""
        try:
            with os.scandir(self.root / rel) as it:
                out = []
                for e in it:
                    try:
                        is_dir = e.is_dir()
                        out.append((e.name, is_dir, 0 if is_dir else e.stat().st_size))
                    except OSError:
                        continue
                return out
        except OSError:
            return None

    def same(self, src: Path, rel: str) -> bool:
        return _same(src, self.root / rel)

    def put_file(self, src: Path, rel: str, progress=None) -> None:
        dst = self.root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        tmp = dst.with_name(dst.name + ".part")
        # no metadata: nothing for macOS to keep in "._" files; in pieces, for the progress line
        with open(src, "rb") as fi, open(tmp, "wb") as fo:
            while chunk := fi.read(8 << 20):
                fo.write(chunk)
                if progress:
                    progress(len(chunk))
        os.replace(tmp, dst)

    def file_hash(self, rel: str) -> str | None:
        """SHA-256 of a file as the drive holds it (read past this computer's cache where it can)."""
        return _hash_file(self.root / rel, uncached=True)

    def mkdirs(self, rel: str) -> None:
        (self.root / rel).mkdir(parents=True, exist_ok=True)

    def files(self, rel: str) -> list[str]:
        """Every file under rel, relative to it (macOS's companions left out)."""
        base = self.root / rel
        return [p.relative_to(base).as_posix() for p in base.rglob("*") if p.is_file() and not JUNK.match(p.name)]

    def sizes(self, rel: str) -> dict[str, int]:
        base = self.root / rel
        return {f: (base / f).stat().st_size for f in self.files(rel)}

    def unlink(self, rel: str) -> None:
        (self.root / rel).unlink(missing_ok=True)

    def write_bytes(self, rel: str, data: bytes) -> None:
        (self.root / rel).write_bytes(data)

    def read_bytes(self, rel: str) -> bytes | None:
        p = self.root / rel
        return p.read_bytes() if p.is_file() else None

    def exists(self, rel: str) -> bool:
        return (self.root / rel).exists()

    def rmtree(self, rel: str) -> None:
        shutil.rmtree(self.root / rel)

    def clean_junk(self, rel: str) -> int:
        return _clean_junk(self.root / rel)

    def titles(self) -> dict[str, str]:
        return {g.id: self.where(g.app0) for g in scan(self).games if g.ok and not g.duplicate_of}

    def close(self) -> None:
        pass


FTP_JOBS = 8                    # files sent at once over FTP (put_title)


class FtpDrive:
    """A drive plugged into a console, written through the console's FTP
    server (a jailbroken PS5's ftpsrv): ftp://HOST[:PORT]/mnt/usbN."""

    def __init__(self, url: str, ftp: ftplib.FTP | None = None):
        u = urllib.parse.urlparse(url)
        if u.scheme != "ftp" or not u.hostname:
            raise DriveError(f"{url}: not ftp://HOST[:PORT]/PATH")
        self.label = url
        self.root = "/" + u.path.strip("/") if u.path.strip("/") else ""
        self._url = u
        self.ftp = ftp or self._login()
        self._dirs: set[str] = set()
        self._sizes: dict[str, int] = {}
        self._raw_selfs()

    def _login(self) -> ftplib.FTP:
        u = self._url
        f = ftplib.FTP()
        try:
            f.connect(u.hostname, u.port or 21, timeout=60)
            f.login(urllib.parse.unquote(u.username or "anonymous"), urllib.parse.unquote(u.password or ""))
        except (OSError, ftplib.Error) as e:
            raise DriveError(f"{self.label}: {e}") from e
        return f

    def _raw_selfs(self) -> None:
        """ps5-payload-dev's ftpsrv decrypts SELF files (eboot.bin, *.prx) in
        LIST and RETR unless told not to: sizes and contents then differ from
        what was stored.  Its SELF command toggles that mode."""
        try:
            feat = self.ftp.sendcmd("FEAT")
        except ftplib.all_errors:
            return
        if not any(ln.strip().upper() == "SELF" for ln in feat.splitlines()[1:]):
            return
        for _ in range(2):
            if "disabled" in self.ftp.sendcmd("SELF").lower():
                return
        raise DriveError(f"{self.label}: the FTP server's SELF transfer mode doesn't turn off")

    def _p(self, rel: str) -> str:
        rel = rel.strip("/")
        return f"{self.root}/{rel}" if rel else (self.root or "/")

    def where(self, rel: str) -> str:
        return self._p(rel)

    def _list(self, rel: str) -> list[tuple[str, bool, int]] | None:
        """(name, is a folder, size) for each entry of rel; None when it isn't a folder."""
        lines: list[str] = []
        try:
            self.ftp.retrlines(f"LIST {self._p(rel)}", lines.append)
        except ftplib.error_perm:
            return None
        out = []
        for ln in lines:
            parts = ln.split(None, 8)
            if len(parts) < 9 or parts[8] in (".", ".."):
                continue
            out.append((parts[8], parts[0].startswith("d"), int(parts[4]) if parts[4].isdigit() else 0))
        return out

    def check_root(self) -> None:
        if self._list("") is None:
            raise DriveError(f"{self.label}: no such folder on the console (a USB drive is at /mnt/usb0..7)")

    def fs_type(self) -> str | None:
        return None

    def free(self) -> int | None:
        return None

    def is_dir(self, rel: str) -> bool:
        return self._list(rel) is not None

    def listdir(self, rel: str) -> list[tuple[str, bool, int]] | None:
        return self._list(rel)

    def is_file(self, rel: str) -> bool:
        parent, _, name = rel.strip("/").rpartition("/")
        return any(n == name and not d for n, d, _ in self._list(parent) or [])

    def file_hash(self, rel: str) -> str | None:
        h = hashlib.sha256()
        try:
            self.ftp.retrbinary(f"RETR {self._p(rel)}", h.update, blocksize=1 << 20)
        except ftplib.error_perm:
            return None
        return h.hexdigest()

    def sizes(self, rel: str) -> dict[str, int]:
        """Every file under rel with its size, relative to rel."""
        out: dict[str, int] = {}
        todo = [""]
        while todo:
            sub = todo.pop()
            for name, is_dir, size in self._list(f"{rel}/{sub}" if sub else rel) or []:
                r = f"{sub}/{name}" if sub else name
                if is_dir:
                    self._dirs.add(f"{rel}/{r}".strip("/"))
                    todo.append(r)
                elif not JUNK.match(name):
                    out[r] = size
        return out

    def files(self, rel: str) -> list[str]:
        return list(self.sizes(rel))

    def prepare(self, rel: str) -> None:
        """Know what a folder already holds (sizes), for same()."""
        self._sizes = {f"{rel}/{f}": n for f, n in self.sizes(rel).items()}

    def same(self, src: Path, rel: str) -> bool:
        return self._sizes.get(rel) == src.stat().st_size

    def mkdirs(self, rel: str) -> None:
        path = ""
        for part in rel.strip("/").split("/"):
            path = f"{path}/{part}" if path else part
            if path in self._dirs:
                continue
            try:
                self.ftp.mkd(self._p(path))
            except ftplib.error_perm:
                if self._list(path) is None:
                    raise
            self._dirs.add(path)

    def _store(self, rel: str, f, progress=None) -> None:
        # in place, no .part and rename: a rename costs the console's exFAT
        # about a quarter of a small file's time, and a file cut short by a
        # broken copy is shorter, so the next run's size check copies it again
        parent = rel.rsplit("/", 1)[0] if "/" in rel else ""
        if parent:
            self.mkdirs(parent)
        self.ftp.storbinary(f"STOR {self._p(rel)}", f, blocksize=1 << 20,
                            callback=(lambda block: progress(len(block))) if progress else None)

    def put_file(self, src: Path, rel: str, progress=None) -> None:
        with open(src, "rb") as f:
            self._store(rel, f, progress)

    def write_bytes(self, rel: str, data: bytes) -> None:
        self._store(rel, io.BytesIO(data))

    def read_bytes(self, rel: str) -> bytes | None:
        buf = io.BytesIO()
        try:
            self.ftp.retrbinary(f"RETR {self._p(rel)}", buf.write)
        except ftplib.error_perm:
            return None
        return buf.getvalue()

    def _dele(self, path: str) -> None:
        # ftplib's delete() wants 250; the PS5's ftpsrv answers DELE with 226
        resp = self.ftp.sendcmd(f"DELE {path}")
        if not resp.startswith("2"):
            raise ftplib.error_reply(resp)

    def unlink(self, rel: str) -> None:
        try:
            self._dele(self._p(rel))
        except ftplib.error_perm:
            pass

    def exists(self, rel: str) -> bool:
        parent, _, name = rel.strip("/").rpartition("/")
        return any(n == name for n, _, _ in self._list(parent) or [])

    def rmtree(self, rel: str) -> None:
        for name, is_dir, _ in self._list(rel) or []:
            if is_dir:
                self.rmtree(f"{rel}/{name}")
            else:
                self._dele(self._p(f"{rel}/{name}"))
        self.ftp.rmd(self._p(rel))
        self._dirs = {d for d in self._dirs if d != rel and not d.startswith(rel + "/")}

    def clean_junk(self, rel: str) -> int:
        return 0                        # nothing here makes macOS companions

    def titles(self) -> dict[str, str]:
        return {g.id: self._p(g.app0) for g in scan(self).games if g.ok and not g.duplicate_of}

    def clone(self) -> "FtpDrive":
        """Another connection to the same drive, sharing the folders known
        to exist: for copying several files at once."""
        c = FtpDrive(self.label)
        c._dirs = self._dirs
        return c

    def close(self) -> None:
        try:
            self.ftp.quit()
        except (OSError, ftplib.Error):
            pass


def open_drive(drive) -> LocalDrive | FtpDrive:
    """A drive given as this computer's mount point or as ftp://HOST[:PORT]/PATH."""
    if isinstance(drive, (LocalDrive, FtpDrive)):
        return drive
    if str(drive).startswith("ftp://"):
        return FtpDrive(str(drive))
    return LocalDrive(Path(drive))


def put_title(src: Path, drive, title: str, prefix: str, prof: dict, name: str | None = None,
              icon: Path | None = None, backdrop: Path | None = None, fetch: bool = False,
              art_dir: Path | None = None, refetch: bool = False, log=print, jobs: int = FTP_JOBS,
              progress: float = 30, folder: str | None = None, on_progress=None, verify: str = "quick") -> dict:
    """Copy a dump onto the drive: its app0 into <folder>/app0 (files already
    there with the same size, and on a mounted drive a newer time, are kept,
    so a broken copy resumes), the prefix file and the selector's meta.bin
    beside it, then check the copy.  drive: a mount point, or
    ftp://HOST[:PORT]/PATH (open_drive).  folder: where on the drive (default:
    the title's copy already there, else ghostldr/<title>).  jobs: files sent
    at once over FTP, each on its own connection (most of a dump is small
    files, and each costs a few round trips).  progress: seconds between
    progress lines (the app asks for them often); on_progress(done bytes,
    bytes to copy, files done, files to copy) as the copy goes.  verify:
    "quick" (every file's size, and eboot.bin, sce_module/ and param.sfo
    byte for byte), "full" (every byte) or "none"."""
    d = open_drive(drive)
    try:
        return _put_title(d, src, title, prefix, prof, name, icon, backdrop, fetch, art_dir, refetch, log, jobs,
                          progress, folder, on_progress, verify)
    finally:
        d.close()


def _copy(d, src: Path, app0: str, todo: list[str], files: dict[str, int], title: str, log, jobs: int,
          progress: float = 30, on_progress=None) -> None:
    need = sum(files[f] for f in todo)
    log(f"{title}: copying {len(todo)} of {len(files)} files, {need / 2**30:.2f} GiB")
    done, count, t0, last, shown = 0, 0, time.time(), time.time(), 0.0
    lock = threading.Lock()

    def moved(n: int, whole: bool = False) -> None:
        nonlocal done, count, last, shown
        with lock:
            done += n
            count += whole
            now = time.time()
            if whole and now - last > progress:
                last = now
                log(f"{title}: {done / 2**30:.1f} of {need / 2**30:.1f} GiB, {count} of {len(todo)} files, "
                    f"{done / 2**20 / (now - t0):.0f} MiB/s")
            if on_progress and (now - shown > 0.25 or count == len(todo)):
                shown = now
                on_progress(done, need, count, len(todo))

    def one(drive, rel: str) -> None:
        sent = 0

        def part(n: int) -> None:
            nonlocal sent
            sent += n
            moved(n)
        drive.put_file(src / rel, f"{app0}/{rel}", progress=part if on_progress else None)
        moved(files[rel] - sent, True)

    jobs = min(jobs, len(todo)) if isinstance(d, FtpDrive) else 1
    if jobs <= 1:
        for rel in todo:
            one(d, rel)
        return
    for rel in todo:                    # every folder first, so the connections don't race to make them
        if "/" in rel:
            d.mkdirs(f"{app0}/{rel.rsplit('/', 1)[0]}")
    free: queue.Queue = queue.Queue()
    drives = [d]
    try:
        for _ in range(jobs - 1):
            try:
                drives.append(d.clone())
            except DriveError as e:     # a server that takes fewer clients (421): copy with what it gave
                log(f"{title}: copying over {len(drives)} connection(s); the server refused more ({e})")
                break
        jobs = len(drives)
        for x in drives:
            free.put(x)

        def task(rel: str) -> None:
            drive = free.get()
            try:
                one(drive, rel)
            finally:
                free.put(drive)

        with ThreadPoolExecutor(jobs) as pool:
            futures = [pool.submit(task, rel) for rel in todo]
            try:
                for fu in futures:
                    fu.result()
            except BaseException:
                for fu in futures:
                    fu.cancel()
                raise
    finally:
        for x in drives[1:]:
            x.close()


def key_files(files: dict[str, int]) -> list[str]:
    """The files a boot reads first and a broken copy shows in: eboot.bin,
    sce_module/'s libraries, sce_sys/param.sfo."""
    return [f for f in files if f == "eboot.bin" or f.startswith("sce_module/") or f == "sce_sys/param.sfo"]


def verify_copy(d, src: Path, app0: str, files: dict[str, int], how: str = "quick", log=print) -> str:
    """Check a copy against its dump: every file there with its size, and
    the key files (or, how="full", all of them) byte for byte.  A file that
    differs is copied again once.  What was checked; DriveError when a file
    still differs."""
    if how == "none":
        return "not checked"

    def sizes() -> dict[str, int]:
        return {unicodedata.normalize("NFC", k): v for k, v in d.sizes(app0).items()}

    def again(f: str, why: str) -> None:
        log(f"{d.where(app0)}/{f}: {why} after copying; copying it again")
        d.put_file(src / f, f"{app0}/{f}")

    got = sizes()
    wrong = [f for f, n in files.items() if got.get(unicodedata.normalize("NFC", f)) != n]
    for f in wrong[:16]:
        again(f, "missing or of the wrong size")
    if wrong:
        got = sizes()
        wrong = [f for f, n in files.items() if got.get(unicodedata.normalize("NFC", f)) != n]
    if wrong:
        raise DriveError(f"{d.where(app0)}: {len(wrong)} file(s) missing or of the wrong size after copying, "
                         f"e.g. {wrong[0]}: run the copy again (a drive that keeps doing this may be failing or full)")
    check = list(files) if how == "full" else key_files(files)
    total = sum(files[f] for f in check)
    if total > 64 << 20:
        log(f"checking {len(check)} file(s), {total / 2**20:.0f} MiB, as the drive holds them")
    for f in check:
        want = _hash_file(src / f)
        if d.file_hash(f"{app0}/{f}") == want:
            continue
        again(f, "differs from the dump's")
        if d.file_hash(f"{app0}/{f}") != want:
            raise DriveError(f"{d.where(app0)}/{f} differs from the dump's after copying it twice: the drive may be "
                             "failing (try another drive or USB port)")
    return f"{len(files)} file sizes and {len(check)} file(s) byte for byte"


def place_for(d, title: str, folder: str | None) -> tuple[str, Scan | None]:
    """Where a copy of title goes on the drive: folder, else its copy already
    there, else ghostldr/<title>; and what the drive holds (None when not looked)."""
    if folder is not None:
        folder = folder.strip("/").replace("\\", "/")
        if not folder or any(part in ("", ".", "..") for part in folder.split("/")):
            raise DriveError(f"--folder {folder!r}: a folder on the drive, like \"My Games/Ghost\"")
        return folder, None
    sc = scan(d)
    for g in sc.games:
        if g.id == title and not g.problem and not g.duplicate_of:
            if not g.layered:
                raise DriveError(f"{title} is already on the drive in \"{g.folder}\" (copied by hand): pass "
                                 "--folder to copy it somewhere else, or delete that copy first")
            return g.folder, sc
    return f"{LIBRARY}/{title}", sc


def _put_title(d, src: Path, title: str, prefix: str, prof: dict, name, icon, backdrop, fetch, art_dir, refetch,
               log, jobs: int = 1, progress: float = 30, folder: str | None = None, on_progress=None,
               verify: str = "quick") -> dict:
    d.check_root()
    if not (src / "eboot.bin").is_file():
        raise DriveError(f"{src}: no eboot.bin (point --src at the title's app0)")
    files = tree(src)
    fs = d.fs_type()
    problems = check_files(files, fs)
    if problems:
        raise DriveError(f"{d.label}: " + "; ".join(problems))
    folder, _ = place_for(d, title, folder)
    app0 = f"{folder}/app0"
    if isinstance(d, FtpDrive):
        d.prepare(app0)
        log(f"{d.label}: file system and free space aren't known over FTP (exFAT holds files of 4 GiB or more)")
    todo = [f for f in files if not d.same(src / f, f"{app0}/{f}")]
    need = sum(files[f] for f in todo)
    free = d.free()
    if free is not None and need > free:
        raise DriveError(f"{d.label}: {need / 2**30:.1f} GiB to copy, {free / 2**30:.1f} GiB free")
    _copy(d, src, app0, todo, files, title, log, jobs, progress, on_progress)
    copied, kept = len(todo), len(files) - len(todo)
    d.mkdirs(app0)
    for p in sorted(src.rglob("*")):            # empty folders too
        rel = p.relative_to(src)
        if p.is_dir() and not any(JUNK.match(part) for part in rel.parts):
            d.mkdirs(f"{app0}/{rel.as_posix()}")
    # drives may hand names back in another Unicode normalization (macOS's exFAT: NFD)
    wanted = {unicodedata.normalize("NFC", f) for f in files}
    stale = [f for f in d.files(app0) if unicodedata.normalize("NFC", f) not in wanted]
    for f in stale:
        d.unlink(f"{app0}/{f}")
    d.write_bytes(f"{folder}/{PREFIX_FILE}", (prefix + "\n").encode())
    own, scanned = gm.own_files_from_dir(src)
    m = gm.choose(title, own, scanned, prof.get("name") or "", name, icon, backdrop, fetch,
                  art_dir or gm.OUT_ROOT / title / "art", refetch, log=log, profile_version=prof.get("version") or "")
    for note in m.notes:
        log(note)
    # always written: it names the title on the console when the dump has no param.sfo
    d.write_bytes(f"{folder}/{gm.META_FILE}", gm.build(m))
    junk = d.clean_junk(folder)
    checked = verify_copy(d, src, app0, files, verify, log)
    info = {"title": title, "prefix": prefix, "drive": d.label, "folder": d.where(folder), "rel": folder,
            "src": str(src), "files": len(files), "total": sum(files.values()), "copied": copied, "kept": kept,
            "removed": len(stale), "fs": fs or ("unknown over FTP" if isinstance(d, FtpDrive) else "unknown"),
            "meta": True, "summary": gm.summary(m), "junk_removed": junk, "verified": checked}
    return info


def removable(d, g: Game) -> str:
    """Why remove_title() leaves this copy alone ("" when it removes it): it
    removes only folders laid out as copy_game() writes them."""
    if not g.layered or not g.folder:
        return "copied by hand (no app0 folder of its own): delete it yourself"
    extra = [n for n, _, _ in d.listdir(g.folder) or []
             if n.lower() not in ("app0", PREFIX_FILE, gm.META_FILE) and not JUNK.match(n)]
    if extra:
        return f"its folder holds other things too ({extra[0]}): delete it yourself"
    return ""


def remove_title(drive, title: str) -> list[str]:
    """Remove the title's copies that ghostldr laid out (removable()): the
    folders removed."""
    d = open_drive(drive)
    try:
        gone = []
        for g in scan(d).games:
            if g.id == title and not removable(d, g):
                d.rmtree(g.folder)
                gone.append(g.folder)
        return gone
    finally:
        d.close()


def game_prefix(d, g: Game) -> tuple[str, str]:
    """The save container prefix the console gives the game (approot.c
    title_prefix): its folder's prefix file, else its profile's, else the
    default; and where it came from."""
    raw = d.read_bytes(_join(g.folder, PREFIX_FILE)) if g.layered or g.folder else None
    m = re.match(r"[A-Z0-9]{0,12}", (raw or b"").decode(errors="replace"))
    prefix, source = (m[0] if m else ""), "its prefix file"
    if not prefix:
        try:
            prefix, source = titles.load(g.id).get("pack", {}).get("prefix", "") or "", "its profile"
        except titles.ProfileError:
            prefix = ""
    if not prefix or prefix.startswith("GFS"):
        prefix, source = titles.default_prefix(g.id), "the default"
    return prefix, source


@dataclass
class Report:
    """What check() found on a drive."""
    label: str
    fs: str | None
    free: int | None
    scan: Scan
    deeper: list                 # Game: found by looking further than the PS5 does
    sizes: dict                  # game folder -> (files, bytes, [problems]) when they were walked
    prefixes: dict               # game folder -> (prefix, where from)
    rows: list                   # (PASS/WARN/FAIL, text)

    @property
    def ok(self) -> bool:
        return not any(s == "FAIL" for s, _ in self.rows)


def check(drive, top: str = "", sizes: bool | None = None) -> list[tuple[str, str]]:
    """(PASS/WARN/FAIL, text) rows about a drive (a mount point or ftp://...)."""
    return inspect(drive, top, sizes).rows


def inspect(drive, top: str = "", sizes: bool | None = None) -> Report:
    """The games on a drive as the PS5 would find them under `top` (the drive's
    top by default), what is wrong with any, and games it would miss.
    sizes: walk each game's files (default: on a drive mounted here, not over
    FTP, where it is slow)."""
    d = open_drive(drive)
    try:
        return _inspect(d, top, isinstance(d, LocalDrive) if sizes is None else sizes)
    finally:
        d.close()


def _inspect(d, top: str, walk: bool) -> Report:
    d.check_root()
    fs = d.fs_type()
    rows: list[tuple[str, str]] = []
    if fs == "exfat":
        rows.append(("PASS", "file system exFAT"))
    elif fs == "msdos":
        rows.append(("WARN", "FAT32: no file of 4 GiB or more fits; exFAT is the better choice"))
    elif isinstance(d, FtpDrive):
        rows.append(("WARN", "file system unknown over FTP: the PS5 reads exFAT and FAT32 drives"))
    else:
        rows.append(("WARN", f"file system {fs or 'unknown'}: the PS5 reads exFAT and FAT32 drives only "
                             "(format it exFAT on the PS5 or a computer)"))
    sc = scan(d, top)
    seen = {g.app0 for g in sc.games}
    # further than the PS5 looks, for games it would miss (on a drive mounted here: over FTP it is slow)
    wide = scan(d, top, depth=SCAN_DEPTH + 4, dirs=SCAN_DIRS * 8) if isinstance(d, LocalDrive) else sc
    deeper = [g for g in wide.games if g.app0 not in seen and not g.problem]
    for g in deeper:
        g.too_deep = True
    sizes: dict = {}
    prefixes: dict = {}
    for g in sc.games:
        where = g.folder or "the drive's top folder"
        if g.problem:
            rows.append(("FAIL", f"{where}: {g.problem}"))
            continue
        prefixes[g.folder] = prefix, source = game_prefix(d, g)
        problems = []
        if walk:
            files = d.sizes(g.app0)
            problems = check_files(files, fs)
            sizes[g.folder] = (len(files), sum(files.values()), problems)
        size = f", {sizes[g.folder][0]} files, {sizes[g.folder][1] / 2**30:.2f} GiB" if g.folder in sizes else ""
        what = f"{g.id}{' ' + g.name if g.name else ''}{' ' + g.version if g.version else ''}"
        if g.duplicate_of:
            rows.append(("WARN", f"{what}: a second copy in {where}; --title boots the one in "
                                 f"{g.duplicate_of or 'the top folder'} (the selector shows both)"))
        elif problems:
            rows.append(("FAIL", f"{what} in {where}: " + "; ".join(problems)))
        else:
            rows.append(("PASS", f"{what} in {where}{size}, saves in {prefix}SAVE"))
    for g in deeper:
        rows.append(("WARN", f"{g.folder}: {g.id or 'a game'} the PS5 won't find: more than {SCAN_DEPTH} folders "
                             "down from the top; move it up"))
    if sc.cut:
        rows.append(("WARN", f"the PS5 stops looking after {SCAN_DIRS} folders ({sc.folders} looked at): "
                             "keep games near the drive's top"))
    if not sc.bootable:
        rows.append(("FAIL", f"no games{' in ' + top if top else ''} the PS5 can start: copy one with "
                             "ghostldr.py copy (a game is a folder with eboot.bin, or with app0/eboot.bin)"))
    return Report(d.label, fs, d.free(), sc, deeper, sizes, prefixes, rows)


def format_report(r: Report) -> str:
    """check()'s findings for a person."""
    fs = {"exfat": "exFAT", "msdos": "FAT32"}.get(r.fs or "", r.fs or "file system unknown")
    free = f", {r.free / 2**30:.1f} GiB free" if r.free is not None else ""
    lines = [f"{r.label} ({fs}{free})"]
    for status, text in r.rows[:1]:
        if status != "PASS":
            lines.append(f"  ! {text}")
    games = [g for g in r.scan.games if not g.problem]
    lines.append("")
    lines.append(f"Games the PS5 will find ({len(games)}):" if games else "No games the PS5 can start.")
    for g in games:
        prefix = r.prefixes.get(g.folder, ("?", ""))[0]
        size = f"  {r.sizes[g.folder][1] / 2**30:.1f} GiB" if g.folder in r.sizes else ""
        name = (g.name or "")[:34]
        lines.append(f"  {g.id:<10} {name:<34} {g.version:<6} {g.folder or '(top folder)'}{size}  "
                     f"saves: {prefix}SAVE")
        if g.duplicate_of:
            lines.append(f"  {'':<10} ^ a second copy: --title boots the one in {g.duplicate_of}; the selector shows both")
        if g.folder in r.sizes and r.sizes[g.folder][2]:
            lines.append(f"  {'':<10} ! " + "; ".join(r.sizes[g.folder][2]))
    bad = [g for g in r.scan.games if g.problem] + r.deeper
    if bad:
        lines.append("")
        lines.append(f"Folders the PS5 can't start ({len(bad)}):")
        for g in bad:
            why = g.problem or f"more than {SCAN_DEPTH} folders down from the top: move it up"
            lines.append(f"  {g.folder or '(top folder)'}: {why}")
    rest = [t for s, t in r.rows[1:] if s != "PASS" and "stops looking" in t]
    for t in rest:
        lines.append("")
        lines.append(f"Note: {t}")
    lines.append("")
    if games:
        ids = sorted({g.id for g in games})
        lines.append("Start one with:  python3 ghostldr.py boot" + (f" {ids[0]}" if len(ids) == 1 else
                     "   (the PS5 shows a game selector)"))
    else:
        lines.append("Copy a game with:  python3 ghostldr.py copy /path/to/dump")
    return "\n".join(lines)


def list_drive(d) -> list[Game]:
    """The games the PS5 would boot from the drive, each title once."""
    return [g for g in scan(d).games if g.ok and not g.duplicate_of]


def main() -> int:
    ap = argparse.ArgumentParser(description="games on a USB drive (docs/usb.md); "
                                 "python3 ghostldr.py copy/check do the same for most uses")
    sub = ap.add_subparsers(dest="cmd", required=True)
    ls = sub.add_parser("list", help="the games on a drive (default: every mounted exFAT or FAT drive)")
    ls.add_argument("drive", nargs="?", help="a mount point, or ftp://HOST[:PORT]/PATH for one in the PS5")
    ck = sub.add_parser("check", help="what the PS5 would find on a drive, and why not")
    ck.add_argument("drive")
    rm = sub.add_parser("remove", help="remove a game that ghostldr copied from a drive")
    rm.add_argument("drive")
    rm.add_argument("title")
    args = ap.parse_args()
    if args.cmd == "list":
        drives = [args.drive] if args.drive else mounted_drives()
        if not drives:
            print("no exFAT or FAT drive is mounted here")
            return 1
        for given in drives:
            try:
                d = open_drive(given)
                found = list_drive(d)
            except DriveError as e:
                print(e)
                return 1
            print(f"{d.label} ({d.fs_type() or 'unknown file system'}): {len(found)} title(s)")
            for g in found:
                print(f"  {g.id}  {g.name}  {g.version}".rstrip())
            d.close()
        return 0
    if args.cmd == "check":
        try:
            r = inspect(args.drive)
        except DriveError as e:
            print(e)
            return 1
        print(format_report(r))
        return 0 if r.ok else 1
    title = titles.normalize_title(args.title)
    d = open_drive(args.drive)
    try:
        copies = [g for g in scan(d).games if title and g.id == title]
        kept = [(g, removable(d, g)) for g in copies]
    finally:
        d.close()
    gone = remove_title(args.drive, title) if title else []
    for g, why in kept:
        if why:
            print(f"left {g.folder}: {why}")
    if not gone:
        if not copies:
            print(f"{args.title}: not on {args.drive}")
        return 1
    print(f"removed {title} from {args.drive} ({', '.join(gone)}); its progress, the {title} SAVE container "
          "on the console, is kept")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
