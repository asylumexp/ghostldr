#!/usr/bin/env python3
"""Make a release: the files a user needs, their checksums, and the zip.

    python3 tools/make_release.py [--out DIR] [--version VERSION] [--no-make]
                                  [--deny WORD ...] [--deny-file FILE]
                                  [--save LUAC0RE_SAVE [--keystone-us FILE] [--keystone-eu FILE]
                                   [--encrypted-us ZIP] [--encrypted-eu ZIP]]

Builds the loader (make -s in ghostldr/native) unless --no-make, then writes
into DIR (default build/release):

    ghostldr.lua, ghostldr-ext.bin, ghostldr-elfldr.bin   from build/ghostldr
    manager.lua                                           from ghostldr/lua
    README.txt                                            tools/release-README.txt, with the version
    SHA256SUMS.txt                                        the five above, the zip and the saves
    ghostldr-<version>.zip                                the five above and their SHA256SUMS.txt, flat
    ghostldr-<version>-save-<US|EU>-<TITLE>.zip           with --save: the autoloader's saves, one zip per
                                                          region: decrypted (Apollo) and, given, encrypted
                                                          (tools/make_save.py, docs/autoload.md)

The release page gets the zip, the five files one by one, the saves and
SHA256SUMS.txt.
No Python and no source go in: developers take the repository.  The zip is
reproducible: entries sorted, fixed times and permissions, the same bytes for
the same inputs.

Before writing anything it checks the five files for anything of this
computer's or its owner's: a private-network address that isn't one of the
docs' examples, this computer's home folder or another user folder path, its
user name, its host name, git's user.email, and every word given with --deny
or in --deny-file (one per line, # comments; console names, account IDs,
PSN names: keep that file outside the repository).
"""

from __future__ import annotations

import argparse
import getpass
import hashlib
import re
import shutil
import socket
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BUILT = ("ghostldr.lua", "ghostldr-ext.bin", "ghostldr-elfldr.bin")
MANAGER = "manager.lua"
README = "README.txt"
SUMS = "SHA256SUMS.txt"
FILES = BUILT + (MANAGER, README)
# The addresses the docs and the tests use as examples
EXAMPLE_IPS = {"192.168.1.10", "192.168.1.23", "192.168.1.123", "192.168.0.20", "192.0.2.10", "127.0.0.1"}
ZIP_TIME = (1980, 1, 1, 0, 0, 0)

_IPV4 = re.compile(rb"(?<![\d.])(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?![\d.])")
_USER_PATHS = re.compile(rb"/Users/[A-Za-z0-9._-]+|(?<!/user)/home/[A-Za-z0-9._-]+|[A-Za-z]:\\{1,2}Users\\{1,2}[A-Za-z0-9._-]+")


def die(msg: str) -> None:
    sys.exit(f"make_release: {msg}")


def private_ip(parts: tuple[int, ...]) -> bool:
    a, b = parts[0], parts[1]
    return (a == 10 or (a == 172 and 16 <= b <= 31) or (a == 192 and b == 168) or (a == 169 and b == 254)
            or (a == 100 and 64 <= b <= 127))


def personal_words(extra: list[str]) -> list[str]:
    """What must not be in a release, from this computer and the caller."""
    words = [str(Path.home())]
    user = getpass.getuser()
    if len(user) >= 5:
        words.append(user)
    host = socket.gethostname().split(".")[0]
    if len(host) >= 5 and host.lower() not in ("localhost",):
        words.append(host)
    try:
        email = subprocess.run(["git", "-C", str(REPO), "config", "user.email"], capture_output=True, text=True,
                               check=False).stdout.strip()
    except OSError:
        email = ""
    if email:
        words.append(email)
    return words + [w for w in extra if w]


def find_personal(name: str, data: bytes, words: list[str]) -> list[str]:
    """What in one file looks like this computer's or its owner's."""
    bad = []
    low = data.lower()
    for w in words:
        if w.lower().encode() in low:
            bad.append(f"{name}: contains {w!r}")
    for m in _IPV4.finditer(data):
        parts = tuple(int(x) for x in m.groups())
        ip = m.group(0).decode()
        if all(p < 256 for p in parts) and private_ip(parts) and ip not in EXAMPLE_IPS:
            bad.append(f"{name}: contains the address {ip}")
    for m in _USER_PATHS.finditer(data):
        bad.append(f"{name}: contains the path {m.group(0).decode(errors='replace')}")
    return bad


def read_deny_file(path: Path) -> list[str]:
    words = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            words.append(line)
    return words


def git_version() -> str:
    r = subprocess.run(["git", "-C", str(REPO), "describe", "--tags", "--always", "--dirty"], capture_output=True,
                       text=True, check=False)
    if r.returncode != 0 or not r.stdout.strip():
        die("no version: pass --version (git describe found nothing)")
    return r.stdout.strip()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sums_text(files: dict[str, bytes]) -> bytes:
    """SHA256SUMS.txt as shasum -a 256 writes it, sorted by name."""
    return "".join(f"{sha256(files[n])}  {n}\n" for n in sorted(files)).encode()


def zip_bytes(files: dict[str, bytes]) -> bytes:
    """A flat zip of files: sorted, fixed times and permissions, deflated."""
    import io
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name in sorted(files):
            info = zipfile.ZipInfo(name, date_time=ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3          # Unix, so the permissions below count
            info.external_attr = 0o644 << 16
            z.writestr(info, files[name], compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    return buf.getvalue()


def collect(version: str, built: Path = REPO / "build/ghostldr", lua: Path = REPO / "ghostldr/lua",
            readme: Path = REPO / "tools/release-README.txt") -> dict[str, bytes]:
    """The five files of a release, by name."""
    files = {}
    for n in BUILT:
        p = built / n
        if not p.is_file():
            die(f"{p} is missing: run make -C ghostldr/native (or leave out --no-make)")
        files[n] = p.read_bytes()
    if not (lua / MANAGER).is_file():
        die(f"{lua / MANAGER} is missing")
    files[MANAGER] = (lua / MANAGER).read_bytes()
    files[README] = readme.read_text(encoding="utf-8").replace("@VERSION@", version).encode()
    return files


def make(out: Path, version: str, files: dict[str, bytes], words: list[str],
         saves: dict[str, bytes] | None = None) -> list[Path]:
    """Check the files, then write them, the sums, the zip and the saves (checked by make_save) into out."""
    bad = [b for n in sorted(files) for b in find_personal(n, files[n], words)]
    if bad:
        die("not releasing: personal data in the files:\n  " + "\n  ".join(bad[:30]))
    inner = dict(files)
    inner[SUMS] = sums_text(files)
    zname = f"ghostldr-{version}.zip"
    zdata = zip_bytes(inner)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    written = []
    for n in sorted(files):
        (out / n).write_bytes(files[n])
        written.append(out / n)
    (out / zname).write_bytes(zdata)
    written.append(out / zname)
    for n in sorted(saves or {}):
        (out / n).write_bytes(saves[n])
        written.append(out / n)
    (out / SUMS).write_bytes(sums_text({**files, zname: zdata, **(saves or {})}))
    written.append(out / SUMS)
    return written


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", type=Path, default=REPO / "build/release", help="output folder (emptied first)")
    ap.add_argument("--version", default=None, help="the release's version, e.g. v1.0.0 (default: git describe)")
    ap.add_argument("--no-make", action="store_true", help="use build/ghostldr as it is")
    ap.add_argument("--deny", action="append", default=[], help="a word that must not be in the release")
    ap.add_argument("--deny-file", type=Path, help="more such words, one per line")
    ap.add_argument("--save", type=Path, help="a decrypted Luac0re save (folder or zip): the autoloader's saves too")
    ap.add_argument("--keystone-us", type=Path, help="with --save: CUSA03474's keystone, when the save is EU's")
    ap.add_argument("--keystone-eu", type=Path, help="with --save: CUSA03492's keystone, when the save is US's")
    ap.add_argument("--encrypted-us", type=Path, help="with --save: resign_save.py's zip of the US encrypted save")
    ap.add_argument("--encrypted-eu", type=Path, help="with --save: resign_save.py's zip of the EU encrypted save")
    args = ap.parse_args(argv)
    version = args.version or git_version()
    if not re.fullmatch(r"[A-Za-z0-9._+-]+", version):
        die(f"version {version!r}: letters, digits and . _ + - only")
    if not args.no_make:
        subprocess.run(["make", "-s", "-C", str(REPO / "ghostldr/native")], check=True)
    extra = list(args.deny) + (read_deny_file(args.deny_file) if args.deny_file else [])
    files = collect(version)
    words = personal_words(extra)
    saves = None
    if args.save:
        import make_save
        saves = make_save.save_zips(args.save, version, keystones={"us": args.keystone_us, "eu": args.keystone_eu},
                                    words=words, encrypted={"us": args.encrypted_us, "eu": args.encrypted_eu})
    written = make(args.out.resolve(), version, files, words, saves)
    for p in written:
        print(f"{sha256(p.read_bytes())}  {p.stat().st_size:>10}  {p.name}")
    print(f"make_release: {version} in {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
