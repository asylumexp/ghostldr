"""Shared steps of the installer builds (see README.md here).

- the release tree: `git archive` of the repo plus the four prebuilt files
  from build/ghostldr, as in the release zip;
- the pinned downloads in sources.json, checked against their SHA-256;
- the icons, rendered from app/web/icon.svg;
- a last check that nothing from this computer ended up in a package.
"""

from __future__ import annotations

import getpass
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
PREBUILT = ("core.bin", "core.json", "ext.bin", "selector.bin")
NAME = "ghostldr"
HOMEPAGE = "https://github.com/asylumexp/ghostldr"
SUMMARY = "Prepare, install and play PS4 game dumps in SWRR on a stock PS5"
ICON_SVG = REPO / "app/web/icon.svg"


def log(msg: str) -> None:
    print(f"[package] {msg}", flush=True)


def die(msg: str) -> None:
    sys.exit(f"[package] error: {msg}")


def run(argv: list, **kw) -> subprocess.CompletedProcess:
    return subprocess.run([str(a) for a in argv], check=True, **kw)


def app_version(ref: str = "HEAD") -> str:
    text = subprocess.run(["git", "-C", str(REPO), "show", f"{ref}:app/ghostapp/__init__.py"], check=True,
                          capture_output=True, text=True).stdout
    m = re.search(r'^APP_VERSION = "([^"]+)"', text, re.M)
    if not m:
        die("no APP_VERSION in app/ghostapp/__init__.py")
    return m.group(1)


def default_dirs() -> tuple[Path, Path, Path]:
    """(downloads cache, work folder, output folder), all under build/ (ignored by git)."""
    base = REPO / "build/package"
    return base / "cache", base / "work", base / "dist"


def common_args(ap) -> None:
    cache, work, dist = default_dirs()
    ap.add_argument("--version", default=None, help="package version (default: APP_VERSION)")
    ap.add_argument("--ref", default="HEAD", help="git revision to package (default: HEAD)")
    ap.add_argument("--out", type=Path, default=dist, help=f"output folder (default: {dist.relative_to(REPO)})")
    ap.add_argument("--cache", type=Path, default=cache, help="downloads cache")
    ap.add_argument("--work", type=Path, default=work, help="scratch folder (emptied per build)")


# ---------------------------------------------------------------- downloads

def sources() -> dict:
    return json.loads((HERE / "sources.json").read_text(encoding="utf-8"))


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(key: str, cache: Path) -> Path:
    """A pinned download from sources.json, from the cache if it's there and intact."""
    src = sources()["files"][key]
    name = urllib.request.unquote(src["url"].rsplit("/", 1)[1])
    dest = cache / key / name
    if dest.is_file() and sha256_file(dest) == src["sha256"]:
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    log(f"downloading {src['url']}")
    tmp = dest.with_name(dest.name + ".part")
    req = urllib.request.Request(src["url"], headers={"User-Agent": "ghostldr-package"})
    with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as out:
        shutil.copyfileobj(r, out, 1 << 20)
    got = sha256_file(tmp)
    if got != src["sha256"]:
        tmp.unlink()
        die(f"{name}: SHA-256 {got}, expected {src['sha256']} (sources.json)")
    os.replace(tmp, dest)
    log(f"verified {name} (SHA-256 {got[:16]}...)")
    return dest


def extract_tar(archive: Path, dest: Path, skip: tuple = ()) -> None:
    """A downloaded tarball, without the members under `skip`, refusing any
    that would land outside `dest`, and any two whose names differ only in
    case (macOS's file system would merge them: Linux's terminfo has some)."""
    dest.mkdir(parents=True, exist_ok=True)
    root = dest.resolve()
    with tarfile.open(archive) as t:
        members = [m for m in t.getmembers() if not m.name.startswith(skip)]
        seen = {}
        for m in members:
            other = seen.setdefault(m.name.rstrip("/").lower(), m.name)
            if other != m.name:
                die(f"{archive.name}: {m.name} and {other} differ only in case")
        for m in members:
            target = (root / m.name).resolve()
            if root != target and root not in target.parents:
                die(f"{archive.name}: unsafe path {m.name}")
            if m.islnk() or m.issym():
                link = (target.parent / m.linkname).resolve() if m.issym() else (root / m.linkname).resolve()
                if root != link and root not in link.parents:
                    die(f"{archive.name}: unsafe link {m.name} -> {m.linkname}")
            if m.isdev():
                die(f"{archive.name}: device file {m.name}")
        if sys.version_info >= (3, 12):
            t.extractall(dest, members, filter="tar")
        else:
            t.extractall(dest, members)


def python_runtime(key: str, cache: Path, dest: Path) -> Path:
    """python-build-standalone's install_only tree at dest (its python/ folder),
    without what the app never uses: headers, the test suite, IDLE, pip, the
    turtle demos, and the shared libpython when nothing links it (the
    interpreter has it built in)."""
    tmp = dest.parent / (dest.name + ".unpack")
    shutil.rmtree(tmp, ignore_errors=True)
    extract_tar(fetch(key, cache), tmp, skip=("python/share/", "python/include/"))
    shutil.rmtree(dest, ignore_errors=True)
    os.replace(tmp / "python", dest)
    shutil.rmtree(tmp)
    lib = next(p for p in (dest / "lib").glob("python3.*") if p.is_dir())
    drop = [dest / "include", dest / "share", dest / "lib/pkgconfig"]
    drop += [lib / d for d in ("test", "idlelib", "turtledemo", "ensurepip", "lib2to3")]
    drop += list((lib / "site-packages").glob("pip*"))
    drop += [p for p in (dest / "bin").iterdir() if p.name.startswith(("idle", "pydoc", "pip")) or
             p.name.endswith("-config")]
    for p in drop:
        if p.is_dir() and not p.is_symlink():
            shutil.rmtree(p)
        elif p.exists() or p.is_symlink():
            p.unlink()
    binaries = [p for p in (dest / "bin").iterdir() if p.is_file() and not p.is_symlink()]
    binaries += [p for p in (lib / "lib-dynload").iterdir() if p.is_file()]
    binaries += [p for p in (dest / "lib").iterdir() if p.is_file() and not p.name.startswith("libpython")]
    if not any(b"libpython3" in p.read_bytes() for p in binaries):
        for p in (dest / "lib").glob("libpython3*"):
            p.unlink()
    return dest


def host_python(cache: Path, work: Path) -> Path:
    """The bundled CPython version, for this computer: it compiles the bytecode."""
    import platform
    machine = platform.machine().lower()
    arch = {"arm64": "arm64", "aarch64": "aarch64", "x86_64": "x86_64", "amd64": "x86_64"}.get(machine, machine)
    if sys.platform == "darwin":
        key = f"python-macos-{'arm64' if arch in ('arm64', 'aarch64') else 'x86_64'}"
    elif sys.platform.startswith("linux"):
        key = f"python-linux-{'aarch64' if arch in ('arm64', 'aarch64') else 'x86_64'}"
    else:
        die("build on macOS or Linux")
    dest = work / "host-python"
    if not (dest / "bin/python3").exists():
        python_runtime(key, cache, dest)
    return dest / "bin/python3"


def precompile(python: Path, tree: Path, strip: Path) -> None:
    """Bytecode next to the sources, checked by hash rather than by time: read
    only, so an installed copy never needs to write it.  `strip` is cut from
    the file names recorded in it (Python puts the real path back on import)."""
    run([python, "-I", "-B", "-m", "compileall", "-q", "-f", "-j", "0", "--invalidation-mode", "checked-hash",
         "-s", strip, tree], stdout=subprocess.DEVNULL)


# ---------------------------------------------------------------- the release tree

def stage_release(dest: Path, ref: str = "HEAD") -> Path:
    """The release tree (as in the release zip) at `dest`."""
    missing = [n for n in PREBUILT if not (REPO / "build/ghostldr" / n).is_file()]
    if missing:
        die(f"build/ghostldr is missing {', '.join(missing)}: run `make -s` in ghostldr/native first "
            "(tools/fetch_deps.sh fetches what it needs)")
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    tar = subprocess.run(["git", "-C", str(REPO), "archive", "--format=tar", ref],
                         check=True, capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        if sys.version_info >= (3, 12):
            t.extractall(dest, filter="tar")
        else:
            t.extractall(dest)
    shutil.rmtree(dest / "tools/package", ignore_errors=True)      # these scripts: not for the user
    (dest / "build/ghostldr").mkdir(parents=True)
    for n in PREBUILT:
        shutil.copyfile(REPO / "build/ghostldr" / n, dest / "build/ghostldr" / n)
    for f in ("app/launch.py", "app/ghostldr.sh", "app/ghostldr.command"):
        if (dest / f).is_file():
            (dest / f).chmod(0o755)
    if not (dest / "ghostldr/native/selector/font/OFL.txt").is_file() or not (dest / "LICENSE").is_file():
        die("the release tree has no LICENSE or font OFL.txt")
    return dest


# ---------------------------------------------------------------- icons

def render_png(svg: Path, size: int, out: Path) -> Path:
    tool = shutil.which("resvg")
    if tool:
        run([tool, "-w", size, "-h", size, svg, out])
    elif shutil.which("rsvg-convert"):
        run(["rsvg-convert", "-w", size, "-h", size, "-o", out, svg])
    else:
        die("rendering the icon needs resvg or rsvg-convert (brew install resvg)")
    return out


def icons(work: Path, sizes=(16, 24, 32, 48, 64, 128, 256, 512)) -> dict:
    """{size: png} of app/web/icon.svg."""
    d = work / "icons"
    d.mkdir(parents=True, exist_ok=True)
    return {s: render_png(ICON_SVG, s, d / f"{NAME}-{s}.png") for s in sizes}


def macos_icon_svg(work: Path) -> Path:
    """The icon on macOS's grid: the 1024 canvas with the shape at 824 (Big Sur and later)."""
    svg = ICON_SVG.read_text(encoding="utf-8")
    inner = svg[svg.index(">") + 1:svg.rindex("</svg>")]
    out = work / "icons/macos.svg"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">'
                   f'<g transform="translate(100 100) scale(25.75)">{inner}</g></svg>', encoding="utf-8")
    return out


def write_ico(pngs: dict, out: Path) -> Path:
    """A Windows .ico of PNG images (Vista and later read PNG entries at every size)."""
    sizes = sorted(s for s in pngs if s <= 256)
    datas = [Path(pngs[s]).read_bytes() for s in sizes]
    head = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for s, data in zip(sizes, datas):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out.write_bytes(head + entries + b"".join(datas))
    return out


# ---------------------------------------------------------------- checks

def check_nothing_personal(paths: list, scan_ips: bool = True) -> None:
    """Fail if this computer's home folder or user name is in a file; with
    scan_ips, also any private-network address but the ones the docs and tests
    use as examples (192.168.1.23, 192.168.1.123, 10.0.0.x)."""
    home = str(Path.home())
    user = getpass.getuser()
    words = [home.encode()] + ([user.encode()] if len(user) >= 5 else [])
    ip = re.compile(rb"\b(?:10\.\d{1,3}|192\.168|172\.(?:1[6-9]|2\d|3[01]))\.\d{1,3}\.\d{1,3}\b")
    allowed_ips = {b"192.168.1.23", b"192.168.1.123"}
    bad = []
    for root in paths:
        root = Path(root)
        files = [root] if root.is_file() else [p for p in root.rglob("*") if p.is_file() and not p.is_symlink()]
        for f in files:
            data = f.read_bytes()
            for w in words:
                if w in data:
                    bad.append(f"{f}: contains {w.decode()!r}")
            if scan_ips:
                for m in ip.findall(data):
                    if m not in allowed_ips and not m.startswith(b"10.0.0."):
                        bad.append(f"{f}: contains {m.decode()}")
    if bad:
        die("personal data in the package:\n  " + "\n  ".join(bad[:20]))


def write_sums(out: Path, names: list) -> Path:
    sums = out / "SHA256SUMS.txt"
    lines = {}
    if sums.is_file():
        for line in sums.read_text().splitlines():
            if "  " in line:
                h, n = line.split("  ", 1)
                lines[n] = h
    for n in names:
        lines[n] = sha256_file(out / n)
    sums.write_text("".join(f"{lines[n]}  {n}\n" for n in sorted(lines) if (out / n).is_file()))
    return sums


def fresh(path: Path) -> Path:
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True)
    return path
