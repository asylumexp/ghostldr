"""A writable copy of the release's tools, for installed copies of the app.

The scripts in ghostldr/ find build/ from their own path and write it next to
themselves.  Where the release can't be written (a Linux package in /opt, an
AppImage, a macOS app bundle), they run from a copy in the user's data folder
instead: ghostldr/, the prebuilt files and LICENSE, a few MB.  The copy is
refreshed whenever the installed files change (a new version); the rest of
its build/ (prepared games, the game's log) is kept, as when a release zip is
unpacked over an old one.

    macOS    ~/Library/Application Support/ghostldr/workspace
    Windows  %LOCALAPPDATA%\\ghostldr\\workspace
    Linux    $XDG_DATA_HOME/ghostldr/workspace (~/.local/share/ghostldr/workspace)

GHOSTLDR_APP_WORKSPACE overrides the folder.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path

TREES = ("ghostldr",)
FILES = ("LICENSE", "build/ghostldr/core.bin", "build/ghostldr/core.json", "build/ghostldr/ext.bin",
         "build/ghostldr/selector.bin")
STAMP = ".ghostldr-workspace.json"
SKIP_DIRS = {"__pycache__"}
SKIP_SUFFIXES = (".pyc", ".pyo")


def default_dir() -> Path:
    override = os.environ.get("GHOSTLDR_APP_WORKSPACE")
    if override:
        return Path(override)
    home = Path.home()
    if sys.platform == "win32":
        base = Path(os.environ.get("LOCALAPPDATA") or home / "AppData" / "Local")
    elif sys.platform == "darwin":
        base = home / "Library" / "Application Support"
    else:
        base = Path(os.environ.get("XDG_DATA_HOME") or home / ".local" / "share")
    return base / "ghostldr" / "workspace"


def writable(root: Path) -> bool:
    """Whether the scripts can write build/ in this release folder."""
    build = root / "build"
    return os.access(build if build.is_dir() else root, os.W_OK)


def _files(root: Path) -> list[str]:
    """The release files the copy holds, as sorted '/'-separated relative paths."""
    out = []
    for tree in TREES:
        for d, dirs, names in os.walk(root / tree):
            dirs[:] = sorted(x for x in dirs if x not in SKIP_DIRS)
            rel = Path(d).relative_to(root).as_posix()
            out += [f"{rel}/{n}" for n in names if not n.endswith(SKIP_SUFFIXES)]
    out += [f for f in FILES if (root / f).is_file()]
    return sorted(out)


def fingerprint(root: Path) -> str:
    h = hashlib.sha256()
    for rel in _files(root):
        data = (root / rel).read_bytes()
        h.update(rel.encode("utf-8") + b"\0" + hashlib.sha256(data).digest())
    return h.hexdigest()


def _copy_tree(src: Path, dst: Path) -> None:
    """Files only, with the copy's default permissions (the source may be read-only)."""
    for d, dirs, names in os.walk(src):
        dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
        out = dst / Path(d).relative_to(src)
        out.mkdir(parents=True, exist_ok=True)
        for n in names:
            if not n.endswith(SKIP_SUFFIXES):
                shutil.copyfile(Path(d) / n, out / n)


def _lock(dest: Path):
    """Two launches at once: the second waits for the first's copy (POSIX only)."""
    try:
        import fcntl
    except ImportError:
        return None
    f = open(dest / ".lock", "a+b")
    fcntl.flock(f, fcntl.LOCK_EX)
    return f


def prepare(src: Path, dest: Path) -> bool:
    """Make `dest` a runnable copy of the tools in `src`; True if it copied anything."""
    src, dest = src.resolve(), dest.resolve()
    dest.mkdir(parents=True, exist_ok=True)
    lock = _lock(dest)
    try:
        fp = fingerprint(src)
        try:
            stamp = json.loads((dest / STAMP).read_text(encoding="utf-8"))
        except (OSError, ValueError):
            stamp = {}
        if stamp.get("fingerprint") == fp and all((dest / t).is_dir() for t in TREES):
            return False
        for left in list(dest.glob(".new-*")) + list(dest.glob(".old-*")):     # from a copy that was cut short
            shutil.rmtree(left, ignore_errors=True)
        new = Path(tempfile.mkdtemp(prefix=".new-", dir=dest))
        try:
            for tree in TREES:
                _copy_tree(src / tree, new / tree)
            for tree in TREES:
                if (dest / tree).exists():
                    old = Path(tempfile.mkdtemp(prefix=".old-", dir=dest))
                    os.replace(dest / tree, old / tree)
                    shutil.rmtree(old, ignore_errors=True)
                os.replace(new / tree, dest / tree)
        finally:
            shutil.rmtree(new, ignore_errors=True)
        for rel in FILES:
            if (src / rel).is_file():
                (dest / rel).parent.mkdir(parents=True, exist_ok=True)
                tmp = (dest / rel).with_name(f".{Path(rel).name}.new")
                shutil.copyfile(src / rel, tmp)
                os.replace(tmp, dest / rel)
        (dest / STAMP).write_text(json.dumps({"fingerprint": fp, "source": str(src)}, indent=1), encoding="utf-8")
        return True
    finally:
        if lock:
            lock.close()
