#!/usr/bin/env python3
"""Titles on a USB drive (docs/usb.md): the drive's layout, putting titles
on it from this computer, and finding what one holds.

    <drive>/ghostldr/<TITLE>/app0/...    the title's app0, as dumped
    <drive>/ghostldr/<TITLE>/prefix      its save container prefix (one line)
    <drive>/ghostldr/<TITLE>/meta.bin    its name and art for the game selector (gfs_meta.py),
                                         when there is anything to show beyond its profile's name

<TITLE> is the title ID (1-15 of A-Z 0-9 _ -); the console boots the drive
with run.py boot --app-root usb:/ghostldr (the selector picks, or --title).
The drive is formatted exFAT (FAT32 holds no file of 4 GiB or more), with an
MBR or GPT partition table or none.  The console only reads it: progress goes
to SWRR's save containers, <prefix>SAVE, as for installed titles.

    python3 ghostldr/host/gfs_pack.py --src DUMP --usb /Volumes/DRIVE   # put a title on a drive
    python3 ghostldr/host/usb_drive.py list [DRIVE]                    # what drives hold
    python3 ghostldr/host/usb_drive.py check DRIVE                      # file system, sizes, names
    python3 ghostldr/host/usb_drive.py remove DRIVE CUSA12345

DRIVE is where this computer has the drive mounted, or, for a drive plugged
into a jailbroken PS5 with an FTP server, ftp://HOST[:PORT]/mnt/usbN (the
console's path of the drive): the same layout is written through FTP.  Over
FTP the drive's file system and free space aren't known, so the FAT32 size
limit and the space check are skipped (the PS5 mounts exFAT and FAT32
drives at /mnt/usbN).
"""

from __future__ import annotations

import argparse
import ftplib
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
from pathlib import Path

import gfs_catalog as gc
import gfs_meta as gm
import profiles as titles

LIBRARY = "ghostldr"
PREFIX_FILE = "prefix"
FAT_FILE_MAX = (4 << 30) - 1
APP_ROOT = f"usb:/{LIBRARY}"
# macOS's companions and indexes: never part of a title (the console hides "._" names)
JUNK = re.compile(r"^(\._.*|\.DS_Store|\.Spotlight-V100|\.fseventsd|\.Trashes|\.TemporaryItems)$")


class DriveError(Exception):
    pass


def library(drive: Path) -> Path:
    return Path(drive) / LIBRARY


def drive_titles(drive: Path) -> dict[str, Path]:
    """{title ID: its app0} for every title folder on the drive."""
    lib = library(drive)
    out = {}
    if lib.is_dir():
        for d in sorted(lib.iterdir()):
            if titles.TITLE_RE.fullmatch(d.name) and (d / "app0/eboot.bin").is_file():
                out[d.name] = d / "app0"
    return out


def mounted_drives() -> list[Path]:
    """Mounted volumes that hold a ghostldr folder (macOS /Volumes, Linux /media and /run/media)."""
    roots = [Path("/Volumes")]
    user = os.environ.get("USER", "")
    roots += [Path("/media") / user, Path("/run/media") / user, Path("/media"), Path("/mnt")]
    found = []
    for r in roots:
        try:
            for v in sorted(r.iterdir()):
                if (v / LIBRARY).is_dir() and v not in found:
                    found.append(v)
        except OSError:
            continue
    return found


def find_drive(given: Path | None = None) -> Path | None:
    """The drive run.py should look at: the one given, else the only mounted one with titles."""
    if given:
        return Path(given)
    drives = mounted_drives()
    return drives[0] if len(drives) == 1 else None


def fs_type(path: Path) -> str | None:
    """The file system the path is on ("exfat", "msdos", ...), or None when unknown here."""
    try:
        if sys.platform == "darwin":
            out = subprocess.run(["diskutil", "info", str(path)], capture_output=True, text=True, timeout=20).stdout
            m = re.search(r"Type \(Bundle\):\s+(\S+)", out)
            return m[1].lower() if m else None
        out = subprocess.run(["findmnt", "-no", "FSTYPE", "--target", str(path)], capture_output=True, text=True,
                             timeout=20).stdout.strip()
        return {"vfat": "msdos", "fat": "msdos"}.get(out, out) or None
    except (OSError, subprocess.SubprocessError):
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

    def same(self, src: Path, rel: str) -> bool:
        return _same(src, self.root / rel)

    def put_file(self, src: Path, rel: str) -> None:
        dst = self.root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        tmp = dst.with_name(dst.name + ".part")
        shutil.copyfile(src, tmp)        # no metadata: nothing for macOS to keep in "._" files
        os.replace(tmp, dst)

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
        return {t: str(p) for t, p in drive_titles(self.root).items()}

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
        self.ftp.storbinary(f"STOR {self._p(rel)}", f, blocksize=1 << 20, callback=progress)

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
        out = {}
        for name, is_dir, _ in self._list(LIBRARY) or []:
            if is_dir and titles.TITLE_RE.fullmatch(name) and self.exists(f"{LIBRARY}/{name}/app0/eboot.bin"):
                out[name] = self._p(f"{LIBRARY}/{name}/app0")
        return out

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
              art_dir: Path | None = None, refetch: bool = False, log=print, jobs: int = FTP_JOBS) -> dict:
    """Copy a dump onto the drive as <drive>/ghostldr/<title>: its app0 (files
    already there with the same size, and on a mounted drive a newer time,
    are kept, so a broken copy resumes), the prefix file and the selector's
    meta.bin.  drive: a mount point, or ftp://HOST[:PORT]/PATH (open_drive).
    jobs: files sent at once over FTP, each on its own connection (most of
    a dump is small files, and each costs a few round trips)."""
    d = open_drive(drive)
    try:
        return _put_title(d, src, title, prefix, prof, name, icon, backdrop, fetch, art_dir, refetch, log, jobs)
    finally:
        d.close()


def _copy(d, src: Path, app0: str, todo: list[str], files: dict[str, int], title: str, log, jobs: int) -> None:
    need = sum(files[f] for f in todo)
    done, count, t0, last = 0, 0, time.time(), time.time()
    lock = threading.Lock()

    def one(drive, rel: str) -> None:
        nonlocal done, count, last
        drive.put_file(src / rel, f"{app0}/{rel}")
        with lock:
            done += files[rel]
            count += 1
            if time.time() - last > 30:
                last = time.time()
                log(f"{title}: {done / 2**30:.1f} of {need / 2**30:.1f} GiB, {count} of {len(todo)} files, "
                    f"{done / 2**20 / (last - t0):.0f} MiB/s")

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


def _put_title(d, src: Path, title: str, prefix: str, prof: dict, name, icon, backdrop, fetch, art_dir, refetch,
               log, jobs: int = 1) -> dict:
    d.check_root()
    if not (src / "eboot.bin").is_file():
        raise DriveError(f"{src}: no eboot.bin (point --src at the title's app0)")
    files = tree(src)
    fs = d.fs_type()
    problems = check_files(files, fs)
    if problems:
        raise DriveError(f"{d.label}: " + "; ".join(problems))
    folder = f"{LIBRARY}/{title}"
    app0 = f"{folder}/app0"
    if isinstance(d, FtpDrive):
        d.prepare(app0)
        log(f"{d.label}: file system and free space aren't known over FTP (exFAT holds files of 4 GiB or more)")
    todo = [f for f in files if not d.same(src / f, f"{app0}/{f}")]
    need = sum(files[f] for f in todo)
    free = d.free()
    if free is not None and need > free:
        raise DriveError(f"{d.label}: {need / 2**30:.1f} GiB to copy, {free / 2**30:.1f} GiB free")
    _copy(d, src, app0, todo, files, title, log, jobs)
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
    meta = f"{folder}/{gm.META_FILE}"
    has_meta = bool(m.sfo or m.icon or m.backdrop or m.name_source in (gm.NAME_GIVEN, gm.NAME_STORE))
    if has_meta:
        d.write_bytes(meta, gm.build(m))
    else:
        d.unlink(meta)
    junk = d.clean_junk(folder)
    info = {"title": title, "prefix": prefix, "drive": d.label, "folder": d.where(folder), "src": str(src),
            "files": len(files), "total": sum(files.values()), "copied": copied, "kept": kept, "removed": len(stale),
            "fs": fs or ("unknown over FTP" if isinstance(d, FtpDrive) else "unknown"), "meta": has_meta,
            "summary": gm.summary(m), "junk_removed": junk}
    return info


def remove_title(drive, title: str) -> bool:
    d = open_drive(drive)
    try:
        folder = f"{LIBRARY}/{title}"
        if not d.is_dir(f"{folder}/app0"):
            return False
        d.rmtree(folder)
        return True
    finally:
        d.close()


def check(drive) -> list[tuple[str, str]]:
    """(PASS/WARN/FAIL, text) rows about a prepared drive (a mount point or ftp://...)."""
    d = open_drive(drive)
    try:
        return _check(d)
    finally:
        d.close()


def _check(d) -> list[tuple[str, str]]:
    rows = []
    fs = d.fs_type()
    if fs in ("exfat",):
        rows.append(("PASS", f"file system {fs}"))
    elif fs == "msdos":
        rows.append(("WARN", "FAT32: no file of 4 GiB or more; exFAT is the better choice"))
    elif isinstance(d, FtpDrive):
        rows.append(("WARN", "file system unknown over FTP: the console reads exFAT and FAT32 only"))
    else:
        rows.append(("WARN", f"file system {fs or 'unknown'}: the console reads exFAT and FAT32 only"))
    found = d.titles()
    if not found:
        rows.append(("FAIL", f"no titles in {d.where(LIBRARY)} (<TITLE>/app0/eboot.bin)"))
    if isinstance(d, LocalDrive) and library(d.root).is_dir():
        for p in sorted(library(d.root).iterdir()):
            if p.is_dir() and not JUNK.match(p.name) and p.name not in found:
                rows.append(("WARN", f"{p.name}: not a title folder (name 1-15 of A-Z 0-9 _ -, app0/eboot.bin in it)"))
    for t in found:
        files = d.sizes(f"{LIBRARY}/{t}/app0")
        problems = check_files(files, fs)
        raw = d.read_bytes(f"{LIBRARY}/{t}/{PREFIX_FILE}")
        prefix = raw.decode(errors="replace").strip() if raw else ""
        why = gc.prefix_problem(prefix) if prefix else None
        if problems or why:
            rows.append(("FAIL", f"{t}: " + "; ".join(problems + ([f"prefix {prefix}: {why}"] if why else []))))
        else:
            rows.append(("PASS", f"{t}: {len(files)} files, {sum(files.values()) / 2**30:.2f} GiB, saves in "
                                 f"{prefix or titles.default_prefix(t)}SAVE"))
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description="titles on a USB drive (docs/usb.md)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    ls = sub.add_parser("list", help="the titles on a drive (default: every mounted drive with a ghostldr folder)")
    ls.add_argument("drive", nargs="?", help="a mount point, or ftp://HOST[:PORT]/mnt/usbN on a jailbroken PS5")
    ck = sub.add_parser("check", help="check a drive's file system and titles")
    ck.add_argument("drive")
    rm = sub.add_parser("remove", help="remove a title from a drive")
    rm.add_argument("drive")
    rm.add_argument("title")
    args = ap.parse_args()
    if args.cmd == "list":
        drives = [args.drive] if args.drive else mounted_drives()
        if not drives:
            print("no mounted drive has a ghostldr folder")
            return 1
        for given in drives:
            try:
                d = open_drive(given)
            except DriveError as e:
                print(e)
                return 1
            found = d.titles()
            print(f"{d.label} ({d.fs_type() or 'unknown file system'}): {len(found)} title(s)")
            for t, app0 in found.items():
                sfo = titles.param_sfo(Path(app0)) if isinstance(d, LocalDrive) else {}
                print(f"  {t}  {sfo.get('TITLE', '')}  {sfo.get('APP_VER', '')}".rstrip())
            d.close()
        return 0
    if args.cmd == "check":
        rows = check(args.drive)
        for status, text in rows:
            print(f"{status:<4}  {text}")
        return 1 if any(s == "FAIL" for s, _ in rows) else 0
    title = titles.normalize_title(args.title)
    if not title or not remove_title(args.drive, title):
        print(f"{args.title}: not on {args.drive}")
        return 1
    print(f"removed {title} from {args.drive} (its progress, {title}'s SAVE container on the console, is kept)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
