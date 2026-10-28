#!/usr/bin/env python3
"""Install titles into the host game's save containers over the network (ghostfs).

Needs only the remote Lua loader (no kernel exploit): Luac0re's on 9026 in
SWRR, or UltraC0re's on 8888 in Jak X, which keeps its own containers.  Sends
ghostldr/lua/gfs_receiver.lua, which listens on GFS_PORT, then streams each
container laid out by gfs_pack.py straight from the source files.

Several titles live side by side (docs/ghostfs-catalogue.md): each has its
own prefix P, data containers P000.., index PINDEX and progress PSAVE, and
GFSCATALOG lists them.  Installing a title never deletes another title's
data: when the console lacks the space, the install stops before sending
anything.  For the title being installed:
  - the same layout, complete: nothing to send
  - another layout: its containers are replaced (its PSAVE progress is kept)
  - the same layout, interrupted: containers whose gfs.tag matches are kept
Its catalogue entry and PINDEX are marked incomplete first and completed
last, so an interrupted install is recognised and never booted.  Before
anything is sent, the packed dump's code is checked against the executable
memory ghostldr has inside SWRR (tools/exec_fit.py): a title that won't fit
is refused unless --no-fit-check.  Ghost
installed by ghostldr v0.1 (GHOST000..055 + GFSINDEX) is kept as it is and
added to the catalogue.  The title's name and art for the game selector go
into PMETA (gfs_meta.py), written last; --update-meta writes only that, for
a title that is already installed (nothing else is touched).

    python3 ghostldr/host/gfs_pack.py [--src DIR] [--title CUSA12345]
    python3 ghostldr/host/gfs_install.py [--title CUSA12345] [--dry-run] [--force] [--no-fit-check]
    python3 ghostldr/host/gfs_install.py --list
    python3 ghostldr/host/gfs_install.py --remove CUSA12345 [--remove-saves]
    python3 ghostldr/host/gfs_install.py --update-meta [--title CUSA12345] [--fetch-art]
                                         [--name NAME] [--icon FILE] [--backdrop FILE]

Without --title it installs the title packed in build/ghostfs (the newest one
when there are several).
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time
from dataclasses import dataclass, field, replace
from pathlib import Path

import gfs_catalog as gc
import gfs_meta as gm
from console import console_ip, loader_port
from gfs_pack import FLAG_INCOMPLETE, OUT_ROOT, default_title, exec_fit, index_header

ROOT = Path(__file__).resolve().parents[2]
RECEIVER = ROOT / "ghostldr/lua/gfs_receiver.lua"
# native code the receiver runs a PUT through (ghostldr/native/pump, `make pump`)
PUMP = ROOT / "ghostldr/data/gfs_pump.bin"
# The loaders create sockets in the JIT compiler process and pass them over, so a
# receiver's listening socket outlives it (the compiler keeps its copy): each
# run listens on a fresh port, and connections to an old one would hang.
GFS_PORT = 9032 + int(time.time()) % 200
LOG_PORT = 18195
CHUNK = 4 << 20
GIB = 1 << 30
OP_PUT, OP_QUERY, OP_BYE, OP_DELETE, OP_READ, OP_WRITE, OP_LIST, OP_FREE, OP_DELSAVE = range(1, 10)
INDEX, LEGACY_INDEX = gc.LEGACY_INDEXES
TAG = "gfs.tag"
MAX_CONTAINERS = gc.MAX_CONTAINERS
NOT_FOUND = 0x809F0008
SAVE_ERRORS = {0x809F0003: "busy", 0x809F0008: "not found", 0x809F000A: "no space left on the console's storage",
               0x809F000C: "too many mounted", 0x809F000F: "broken"}
SMALL_CONTAINER = gc.CATALOG_BLOCKS * 32768      # an index or the catalogue (512 blocks)


def log(msg: str) -> None:
    print(f"{time.strftime('%H:%M:%S')} gfs-install: {msg}", flush=True)


@dataclass
class Install:
    """What an index header says (installed, or about to be)."""
    name: str            # the container holding the index
    version: int
    title: str
    layout_id: int
    prefix: str
    containers: int
    entries: int
    total: int
    incomplete: bool

    def describe(self) -> str:
        what = self.title or "an unnamed title (version 1 index)"
        state = ", incomplete" if self.incomplete else ""
        return f"{what} in {self.containers} containers {self.prefix}000.. ({self.total / 2**30:.1f} GiB{state})"


def parse_header(name: str, data: bytes | None) -> Install | None:
    if not data or len(data) < 64 or data[:4] != b"GFS1":
        return None
    version, count, entries, _cap, total = struct.unpack_from("<IIIQQ", data, 4)
    prefix = data[40:56].split(b"\0")[0].decode(errors="replace")
    if version >= 2 and len(data) >= 88:
        flags = struct.unpack_from("<I", data, 60)[0]
        title = data[64:80].split(b"\0")[0].decode(errors="replace")
        layout_id = struct.unpack_from("<Q", data, 80)[0]
        return Install(name, version, title, layout_id, prefix, count, entries, total, bool(flags & FLAG_INCOMPLETE))
    return Install(name, version, "", 0, prefix, count, entries, total, False)


def same_data(have: Install, new: Install) -> bool:
    if have.version >= 2:
        return have.layout_id == new.layout_id
    # version 1 had no layout id: the same prefix and shape is the same layout
    return (have.prefix, have.containers, have.entries, have.total) == (new.prefix, new.containers, new.entries,
                                                                       new.total)


# ---- what the console has ----

@dataclass
class State:
    catalogue: gc.Catalogue
    legacy: list[Install] = field(default_factory=list)    # GFSINDEX / GHOSTIDX headers found
    adopted: list[str] = field(default_factory=list)        # legacy titles not in the catalogue yet
    entry: gc.Entry | None = None       # the title's catalogue entry
    current: Install | None = None      # its installed index header (PINDEX or its legacy index)
    present: set[int] = field(default_factory=set)          # its data containers on the console
    tagged: set[int] = field(default_factory=set)           # ... complete for the new layout
    meta: dict | None = None            # its <P>META header (gfs_meta.parse), if it has one


def known_prefixes() -> dict[str, str]:
    """prefix -> title for the titles this computer knows (profiles, packs):
    names a version 1 index, which has no title in its header."""
    import profiles
    out = {}
    for title in profiles.known_titles():
        try:
            prefix = profiles.raw_profile(title).get("pack", {}).get("prefix")
        except profiles.ProfileError:
            continue
        if prefix:
            out[prefix] = title
    for f in OUT_ROOT.glob("*/layout.json"):
        try:
            info = json.loads(f.read_text())
            out.setdefault(info["prefix"], info["title"])
        except (OSError, ValueError, KeyError):
            pass
    return out


def read_catalogue(rx) -> tuple[gc.Catalogue, list[Install], list[str]]:
    """The catalogue, with legacy (v0.1) installs it doesn't list yet added."""
    cat = gc.parse(rx.read(gc.CATALOG, gc.CATALOG_FILE, 1 << 20)) or gc.Catalogue()
    legacy = [h for n in (INDEX, LEGACY_INDEX) if (h := parse_header(n, rx.read(n, gc.INDEX_FILE, 128)))]
    adopted = []
    for h in legacy:
        if any(e.index == h.name for e in cat.entries):
            continue
        title = h.title or known_prefixes().get(h.prefix, "")
        if not title or cat.find(title) or cat.by_prefix(h.prefix):
            log(f"{h.name}: {h.describe()} is not in the catalogue and cannot be added (title unknown or taken)")
            continue
        e = gc.entry_from_header(h.name, h)
        e.title = title
        cat.put(e)
        adopted.append(title)
    return cat, legacy, adopted


def relabel(new: Install, layout: dict, index: bytes, prefix: str) -> bytes:
    """The same pack under another prefix: its container names, and the
    prefix in its index header (nothing else in a pack depends on it)."""
    new.prefix, new.name = prefix, gc.index_name(prefix)
    layout["prefix"] = prefix
    for k, job in enumerate(layout["containers"]):
        job["dir"] = gc.data_name(prefix, k)
    b = bytearray(index)
    b[40:56] = prefix.encode().ljust(16, b"\0")
    return bytes(b)


def keep_installed_prefix(rx, new: Install, layout: dict, index: bytes) -> bytes:
    """A title installed before the default prefix took the whole title ID
    (G00004, now GAPOL00004) keeps its prefix: its progress <P>SAVE is there.
    Only that change is followed; any other difference is refused by survey."""
    import profiles
    entry = read_catalogue(rx)[0].find(new.title)
    if (entry and entry.prefix != new.prefix and new.prefix == profiles.default_prefix(new.title)
            and entry.prefix == profiles.legacy_prefix(new.title)):
        log(f"{new.title} is installed with the prefix {entry.prefix} (the default before prefixes took the whole "
            f"title ID): it keeps it, and its progress {gc.save_name(entry.prefix)}")
        return relabel(new, layout, index, entry.prefix)
    return index


def survey(rx, new: Install, jobs: list, force: bool = False) -> State:
    """What the console has, for installing `new`."""
    cat, legacy, adopted = read_catalogue(rx)
    st = State(cat, legacy, adopted)
    for e in cat.entries:
        log(f"installed: {e.describe()}")
    if not cat.entries:
        log("nothing installed")
    other = cat.by_prefix(new.prefix)
    if other and other.title != new.title:
        raise SystemExit(f"the prefix {new.prefix} belongs to {other.title} {other.name}: pack {new.title} with "
                         f"another one (gfs_pack.py --prefix NAME)")
    st.entry = cat.find(new.title)
    if st.entry and st.entry.prefix != new.prefix:
        raise SystemExit(f"{new.title} is installed with the prefix {st.entry.prefix}, this pack uses {new.prefix}: "
                         f"remove it first (gfs_install.py --remove {new.title}) or pack it with --prefix "
                         f"{st.entry.prefix} (its progress is in {gc.save_name(st.entry.prefix)})")
    if st.entry:
        st.current = parse_header(st.entry.index, rx.read(st.entry.index, gc.INDEX_FILE, 128))
    st.meta = gm.parse(rx.read(gc.meta_name(new.prefix), gm.META_FILE, gm.HEADER_SIZE))
    cur = st.current
    if cur and same_data(cur, new) and not cur.incomplete and not force:
        return st                          # installed: no container to send
    # which of its data containers exist, and which are complete for this layout
    exists, tag = rx.probe(jobs[0]["dir"]) if jobs else (False, None)
    if st.entry or exists:
        for k, job in enumerate(jobs):
            exists, tag = (exists, tag) if k == 0 else rx.probe(job["dir"])
            if exists:
                st.present.add(k)
            if tag == tag_bytes(new.layout_id, k, job["size"]):
                st.tagged.add(k)
    return st


# ---- what to do ----

def entry_for(new: Install, layout: dict, incomplete: bool) -> gc.Entry:
    return gc.Entry(new.title, new.prefix, gc.index_name(new.prefix), layout.get("name", ""),
                    layout.get("version", ""), new.layout_id, new.total, new.containers,
                    gc.INCOMPLETE if incomplete else 0, gc.now())


def same_entry(a: gc.Entry | None, b: gc.Entry) -> bool:
    return a is not None and replace(a, updated=0, extra=b"") == replace(b, updated=0, extra=b"")


def plan(st: State, new: Install, layout: dict, force: bool) -> list[tuple]:
    """Steps to get `new` installed next to what the console has, then its
    name and art (<P>META) when the pack has them and the console's differ."""
    steps = plan_data(st, new, layout, force)
    meta = layout.get("meta")
    if meta and (force or not st.meta or st.meta["id"] != meta["id"]):
        steps.append(("meta",))
    return steps


def plan_data(st: State, new: Install, layout: dict, force: bool) -> list[tuple]:
    steps: list[tuple] = []
    cur, entry = st.current, st.entry
    if cur and same_data(cur, new) and not cur.incomplete and not force:
        if entry.legacy and entry.index == LEGACY_INDEX:
            # a version 1 index (no title, no layout id): replace it by PINDEX
            steps += [("index",), ("catalog", entry_for(new, layout, False)), ("delete", LEGACY_INDEX)]
        else:
            done = replace(entry_for(new, layout, False), index=entry.index, flags=entry.flags & gc.LEGACY_INDEX)
            if not same_entry(entry, done) or st.adopted:
                steps.append(("catalog", done))
        return steps
    resume = bool(cur and same_data(cur, new) and cur.incomplete and not force)
    steps.append(("catalog", entry_for(new, layout, True)))
    steps.append(("stub",))
    # this title's own legacy index describes data that is about to change
    for h in st.legacy:
        if h.prefix == new.prefix and h.title in (new.title, ""):
            steps.append(("delete", h.name))
    steps += [("put", k) for k in range(new.containers) if force or not (resume and k in st.tagged)]
    steps.append(("sweep",))
    steps.append(("index",))
    steps.append(("catalog", entry_for(new, layout, False)))
    return steps


def space_needed(st: State, steps: list[tuple], new: Install) -> int:
    """Bytes of new containers the steps create (a replaced container frees its
    own space first)."""
    need = sum(GIB for s in steps if s[0] == "put" and s[1] not in st.present)
    writes_index = ("stub",) in steps or ("index",) in steps
    if writes_index and not (st.entry and st.entry.index == gc.index_name(new.prefix)):
        need += SMALL_CONTAINER
    if any(s[0] == "catalog" for s in steps) and not st.catalogue.exists:
        need += SMALL_CONTAINER
    if ("meta",) in steps and not st.meta:
        need += gm.container_blocks(1 << 20) * 32768
    return need


def plan_remove(entry: gc.Entry, legacy: list[Install], saves: bool) -> list[tuple]:
    steps: list[tuple] = [("catalog", replace(entry, flags=entry.flags | gc.INCOMPLETE, updated=gc.now())),
                          ("delete", entry.index)]
    for h in legacy:                      # a stale legacy index of the same title
        if h.prefix == entry.prefix and h.name != entry.index:
            steps.append(("delete", h.name))
    steps += [("delete_data", entry.prefix, entry.title), ("delete_meta", entry.prefix, entry.title),
              ("drop", entry.title)]
    if saves:
        steps.append(("delsaves", entry.prefix))
    return steps


# ---- the receiver ----

IP_BOUND_IF = 25                 # macOS: send through this interface whatever the routing table says


class Receiver:
    def __init__(self, host: str, interface: str | None = None, pump: bool = True) -> None:
        self.host = host
        self.interface = interface       # send through this interface instead of the default route
        self.pump = pump and PUMP.is_file()
        self.sock: socket.socket | None = None

    def start(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect((self.host, 9))
            local_ip = probe.getsockname()[0]
        head = f'GFS_PORT = {GFS_PORT}\nPROBE_LOG_IP = "{local_ip}"\nPROBE_LOG_PORT = {LOG_PORT}\n'
        if self.pump:
            head += f'GFS_PUMP_HEX = "{PUMP.read_bytes().hex()}"\n'
        lua = head + RECEIVER.read_text()
        port = loader_port(self.host)
        with socket.create_connection((self.host, port), timeout=10) as s:
            s.sendall(lua.encode())
            s.shutdown(socket.SHUT_WR)
        log(f"receiver sent to {self.host}:{port}")

    def connect(self) -> None:
        for _ in range(40):
            try:
                self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                if self.interface:
                    self.sock.setsockopt(socket.IPPROTO_IP, IP_BOUND_IF, socket.if_nametoindex(self.interface))
                self.sock.settimeout(10)
                self.sock.connect((self.host, GFS_PORT))
                self.sock.settimeout(300)
                log(f"connected to the receiver on {GFS_PORT}")
                return
            except OSError:
                time.sleep(1.5)
        raise SystemExit("receiver did not come up (is SWRR at the Luac0re loader?)")

    def request(self, op: int, dirname: str, fname: str, blocks: int = 0, size: int = 0) -> None:
        assert self.sock is not None
        self.sock.sendall(struct.pack("<4sIQQ32s32s8s", b"GFSI", op, blocks, size, dirname.encode(),
                                      fname.encode(), b""))

    def recv(self, n: int) -> bytes:
        assert self.sock is not None
        data = b""
        while len(data) < n:
            part = self.sock.recv(n - len(data))
            if not part:
                raise ConnectionError("receiver closed the connection")
            data += part
        return data

    def reply(self) -> tuple[int, int, int]:
        magic, status, value, millis = struct.unpack("<4siQQ8x", self.recv(32))
        if magic != b"GFSR":
            raise ConnectionError(f"bad reply {magic!r}")
        return status & 0xFFFFFFFF, value, millis

    def read_status(self, dirname: str, fname: str, size: int, offset: int = 0) -> tuple[int, bytes | None]:
        self.request(OP_READ, dirname, fname, offset, size)
        status, n, _ = self.reply()
        return status, (self.recv(n) if status == 0 else None)

    def read(self, dirname: str, fname: str, size: int, offset: int = 0) -> bytes | None:
        return self.read_status(dirname, fname, size, offset)[1]

    def probe(self, dirname: str) -> tuple[bool, bytes | None]:
        """Whether the data container exists, and its gfs.tag."""
        status, tag = self.read_status(dirname, TAG, 16)
        return status != NOT_FOUND, tag

    def write(self, dirname: str, fname: str, data: bytes) -> int:
        self.request(OP_WRITE, dirname, fname, 0, len(data))
        self.sock.sendall(data)
        return self.reply()[0]

    def delete(self, dirname: str) -> int:
        self.request(OP_DELETE, dirname, "")
        return self.reply()[0]

    def delsave(self, dirname: str) -> int:
        self.request(OP_DELSAVE, dirname, "")
        return self.reply()[0]

    def list(self) -> bytes | None:
        self.request(OP_LIST, "", "")
        status, n, _ = self.reply()
        return self.recv(n) if status == 0 else None

    def free(self, size: int) -> tuple[int, int]:
        self.request(OP_FREE, "", "", 0, size)
        status, have, _ = self.reply()
        return status, have

    def put(self, dirname: str, fname: str, blocks: int, size: int, chunks) -> tuple[int, int, int]:
        self.request(OP_PUT, dirname, fname, blocks, size)
        try:
            for block in chunks:
                self.sock.sendall(block)
        except (BrokenPipeError, ConnectionResetError):
            pass                         # a failed create drops the connection: its reply says why
        return self.reply()

    def close(self) -> None:
        if self.sock:
            try:
                self.request(OP_BYE, "", "")
                self.reply()
            except OSError:
                pass
            self.sock.close()
            self.sock = None


def container_bytes(src: Path, pieces: list):
    """Yield the container's bytes in CHUNK-sized blocks, straight from the files."""
    pending = bytearray()
    for rel, off, length in pieces:
        with (src / rel).open("rb") as f:
            f.seek(off)
            left = length
            while left:
                data = f.read(min(CHUNK, left))
                if not data:
                    raise IOError(f"{rel} ended early")
                left -= len(data)
                pending += data
                while len(pending) >= CHUNK:
                    yield bytes(pending[:CHUNK])
                    del pending[:CHUNK]
    if pending:
        yield bytes(pending)


def tag_bytes(layout_id: int, k: int, size: int) -> bytes:
    return struct.pack("<QII", layout_id, k, size)


# ---- doing it ----

def write_catalogue(rx, cat: gc.Catalogue) -> None:
    cat.generation += 1
    data = gc.build(cat)
    rc = rx.write(gc.CATALOG, gc.CATALOG_FILE, data)
    if rc == NOT_FOUND:                    # the first catalogue on this console
        rc = rx.put(gc.CATALOG, gc.CATALOG_FILE, gc.CATALOG_BLOCKS, len(data), [data])[0]
    if rc:
        raise SystemExit(f"{gc.CATALOG}: {rc:#x}")
    cat.exists = True


def delete_quiet(rx, name: str) -> bool:
    rc = rx.delete(name)
    if rc not in (0, NOT_FOUND):
        raise SystemExit(f"delete {name}: {rc:#x}")
    return rc == 0


def delete_owned(rx, prefix: str, ks, layout_ids: set[int], untagged_below: int = 0) -> tuple[int, list[str]]:
    """Delete the data containers <prefix>k (k in ks) that are this title's:
    their gfs.tag names one of its layouts and that k, or (k below
    untagged_below, inside the catalogued title's own range) they have no tag
    yet.  Others are left, and named: another title's that happens to use
    the same names (prefixes were not unique before 2026-10-28), or one
    nothing identifies.  Names that don't exist are not deleted at all: the
    console answers 0 for a missing one, so a blind delete can't tell.  Stops
    after 4 missing names in a row (containers are numbered from 0)."""
    gone, left, missing = 0, [], 0
    for k in ks:
        name = gc.data_name(prefix, k)
        exists, tag = rx.probe(name)
        if not exists:
            missing += 1
            if missing >= 4:
                break
            continue
        missing = 0
        mine = False
        if tag and len(tag) >= 12:
            lid, kk = struct.unpack_from("<QI", tag)
            mine = lid in layout_ids and kk == k
        elif k < untagged_below:
            mine = True
        if mine and delete_quiet(rx, name):
            gone += 1
        elif not mine:
            left.append(name)
    return gone, left


def execute(rx, steps: list[tuple], st: State, new: Install | None = None, layout: dict | None = None,
            index: bytes = b"", src: Path | None = None) -> None:
    cat = st.catalogue
    jobs = layout["containers"] if layout else []
    puts = [s[1] for s in steps if s[0] == "put"]
    sent, t_start = 0, time.time()
    total = sum(jobs[k]["size"] for k in puts)
    idx_name = gc.index_name(new.prefix) if new else ""
    for s in steps:
        if s[0] == "catalog":
            cat.put(s[1])
            write_catalogue(rx, cat)
            log(f"{gc.CATALOG}: {s[1].describe()}")
        elif s[0] == "drop":
            cat.drop(s[1])
            write_catalogue(rx, cat)
            log(f"{gc.CATALOG}: {s[1]} removed")
        elif s[0] == "delete":
            log(f"deleted {s[1]}: {rx.delete(s[1]):#x}")
        elif s[0] == "delete_data":
            e = cat.find(s[2]) if len(s) > 2 else None
            ids = {e.layout_id} if e else set()
            gone, left = delete_owned(rx, s[1], range(MAX_CONTAINERS), ids, e.containers if e else 0)
            log(f"deleted {gone} data containers {s[1]}000..")
            if left:
                log(f"left {', '.join(left)}: not this title's (their tags name another layout, or none): delete "
                    f"them in the manager if you know they are")
        elif s[0] == "delete_meta":
            name = gc.meta_name(s[1])
            info = gm.parse(rx.read(name, gm.META_FILE, gm.HEADER_SIZE))
            title = s[2] if len(s) > 2 else ""
            if info and title and info["title"] != title:
                log(f"left {name}: it is {info['title']}'s name and art")
            elif info and delete_quiet(rx, name):
                log(f"deleted {name} (its name and art)")
        elif s[0] == "meta":
            data = Path(layout["meta"]["file"]).read_bytes()
            rc = gm.put_meta(rx, cat, new.title, new.prefix, data)
            if rc:
                log(f"{gc.meta_name(new.prefix)}: {rc:#x}: the title is installed, but not its name and art "
                    f"(rerun with --update-meta)")
            else:
                log(f"{gc.meta_name(new.prefix)} written: {gm.describe(gm.parse(data))}")
        elif s[0] == "delsaves":
            removed: list[str] = []

            def drop(name: str) -> bool:
                rc = rx.delsave(name)
                if rc not in (0, NOT_FOUND):
                    raise SystemExit(f"delete {name}: {rc:#x}")
                if rc == 0:
                    removed.append(name)
                return rc == 0
            drop(gc.save_name(s[1]))
            # save directories D001.. and memory slots M00..: numbered from the
            # start, so a run of misses ends the search
            for kind, first, last, width, misses in (("D", 1, 999, 3, 8), ("M", 0, 99, 2, 4)):
                miss = 0
                for i in range(first, last + 1):
                    miss = 0 if drop(f"{s[1]}{kind}{i:0{width}d}SAVE") else miss + 1
                    if miss >= misses:
                        break
            log(f"deleted the progress: {', '.join(removed) or 'none found'}")
        elif s[0] == "stub":
            stub = index_header(new.containers, new.entries, new.total, 0, 0, new.prefix, new.title,
                                new.layout_id, FLAG_INCOMPLETE)
            status, _, _ = rx.put(idx_name, gc.INDEX_FILE, layout["index_blocks"], len(stub), [stub])
            if status:
                raise SystemExit(f"{idx_name} stub: {status:#x}")
        elif s[0] == "put":
            k, job = s[1], jobs[s[1]]
            t0 = time.time()
            status, written, millis = rx.put(job["dir"], "data.bin", job["blocks"], job["size"],
                                             container_bytes(src, job["pieces"]))
            if status:
                why = SAVE_ERRORS.get(status, "")
                raise SystemExit(f"{job['dir']}: receiver status {status:#x}{' (' + why + ')' if why else ''} "
                                 f"after {written >> 20} MiB (rerun to resume)")
            rc = rx.write(job["dir"], TAG, tag_bytes(new.layout_id, k, job["size"]))
            if rc:
                raise SystemExit(f"{job['dir']}: tag write {rc:#x}")
            sent += job["size"]
            rate = sent / max(time.time() - t_start, 1e-3)
            log(f"[{puts.index(k) + 1}/{len(puts)}] {job['dir']}: {job['size'] >> 20} MiB in {time.time() - t0:.0f} s "
                f"(console {millis / 1000:.0f} s); avg {rate / 1e6:.1f} MB/s, "
                f"ETA {(total - sent) / max(rate, 1) / 60:.0f} min")
        elif s[0] == "sweep":
            ids = {new.layout_id} | ({st.current.layout_id} if st.current else set()) | \
                  ({st.entry.layout_id} if st.entry else set())
            gone, left = delete_owned(rx, new.prefix, range(new.containers, MAX_CONTAINERS), ids)
            if gone:
                log(f"deleted {gone} leftover {new.prefix} containers past {gc.data_name(new.prefix, new.containers - 1)}")
            if left:
                log(f"left {', '.join(left)}: past this title's containers, and not this title's (their tags name "
                    f"another layout, or none)")
        elif s[0] == "index":
            status, _, _ = rx.put(idx_name, gc.INDEX_FILE, layout["index_blocks"], len(index), [index])
            if status:
                raise SystemExit(f"{idx_name}: {status:#x}")
            log(f"{idx_name} written: {new.describe()}")


def describe_steps(steps: list[tuple], jobs: list) -> None:
    puts = [s[1] for s in steps if s[0] == "put"]
    for s in steps:
        if s[0] == "delete":
            log(f"  delete {s[1]}")
        elif s[0] == "delete_data":
            log(f"  delete the data containers {s[1]}000..")
        elif s[0] == "delsaves":
            log(f"  delete the progress {gc.save_name(s[1])} (and {s[1]}D<nnn>SAVE, {s[1]}M<nn>SAVE)")
        elif s[0] == "drop":
            log(f"  drop {s[1]} from {gc.CATALOG}")
        elif s[0] == "delete_meta":
            log(f"  delete its name and art {gc.meta_name(s[1])}")
        elif s[0] == "meta":
            log("  write its name and art for the game selector (<prefix>META)")
    if puts:
        log(f"  send {len(puts)} containers, {sum(jobs[k]['size'] for k in puts) / 2**30:.1f} GiB")
    if ("index",) in steps:
        log("  write the index" + ("" if puts else " (the data is already there)"))
    if any(s[0] == "catalog" for s in steps):
        log(f"  update {gc.CATALOG}")
    if not steps:
        log("  nothing to do: already installed")


def check_space(rx, st: State, steps: list[tuple], new: Install) -> None:
    """Stop before anything is sent when the new containers don't fit: other
    titles are never deleted to make room."""
    need = space_needed(st, steps, new)
    if not need:
        return
    status, have = rx.free(need)
    if status:
        raise SystemExit(f"the free-space check failed: {status:#x} (--no-space-check skips it)")
    log(f"  space: {need / GIB:.1f} GiB needed, {'at least ' if have >= need else ''}{have / GIB:.1f} GiB free")
    if have < need:
        raise SystemExit(f"not enough space on the console: {new.title} needs {need / GIB:.1f} GiB more and "
                         f"{have / GIB:.1f} GiB is free.  Nothing was changed and no other title was touched: "
                         f"free space on the PS5, or remove a title (gfs_install.py --remove TITLE), then rerun.")


def fit_check(src: Path, title: str, skip: bool) -> None:
    """Before anything is sent: can ghostldr run this title's code at all
    (tools/exec_fit.py, the loader's own rules)?  A title that cannot is not
    installed unless --no-fit-check."""
    fit, line = exec_fit.check(src, title)
    log(line)
    if fit and fit["verdict"] == exec_fit.NO_FIT:
        if skip:
            log("  installing anyway (--no-fit-check)")
            return
        raise SystemExit(f"{title}: its code does not fit the executable memory ghostldr has inside SWRR, so it "
                         "could not boot; nothing was sent (--no-fit-check installs it anyway)")


def connect(args) -> Receiver:
    global GFS_PORT
    rx = Receiver(args.host or console_ip(), args.interface, not args.no_pump)
    if args.port:
        GFS_PORT = args.port
    else:
        rx.start()
    rx.connect()
    return rx


def main() -> int:
    ap = argparse.ArgumentParser(description="install ghostfs titles into SWRR's save data")
    ap.add_argument("--title", help="title to install, as packed by gfs_pack.py (default: the only or newest pack)")
    ap.add_argument("--dry-run", action="store_true", help="only show what is installed and what would change")
    ap.add_argument("--force", action="store_true", help="resend every container of the title")
    ap.add_argument("--list", action="store_true", help="list the installed titles")
    ap.add_argument("--remove", metavar="TITLE", help="delete this title's data and index (its progress stays)")
    ap.add_argument("--remove-saves", action="store_true", help="with --remove: delete its progress (<prefix>SAVE) too")
    ap.add_argument("--update-meta", action="store_true", help="only write the installed title's name and art for "
                    "the game selector (<prefix>META); its data, index and progress are not touched")
    ap.add_argument("--src", type=Path, help="with --update-meta: the title's app0, for its own sce_sys files "
                    "(default: the packed one, else they are read from the console)")
    ap.add_argument("--no-space-check", action="store_true", help="skip the free-space check before sending")
    ap.add_argument("--no-fit-check", action="store_true",
                    help="install even when the title's code cannot fit SWRR's executable memory")
    ap.add_argument("--host")
    ap.add_argument("--interface", help="this computer's network interface to send through (e.g. en0)")
    ap.add_argument("--no-pump", action="store_true", help="receive and write each chunk in turn from Lua "
                    "(slower; the native pump does both at once)")
    ap.add_argument("--port", type=int, help="reattach to a receiver already listening on this port "
                    "(it holds Luac0re's loader, so a new one cannot start)")
    gm.add_args(ap)
    args = ap.parse_args()
    if args.remove_saves and not args.remove:
        ap.error("--remove-saves goes with --remove TITLE")
    if args.update_meta and (args.remove or args.list):
        ap.error("--update-meta goes alone (with --title and the name and art options)")
    if args.update_meta:
        rx = connect(args)
        try:
            return gm.update_meta(rx, args, log)
        finally:
            rx.close()

    if args.list or args.remove:
        rx = connect(args)
        try:
            if args.list:
                cat = gc.parse(rx.list()) or gc.Catalogue()
                how = "" if cat.exists else f" (no {gc.CATALOG} yet)"
                log(f"{len(cat.entries)} titles installed{how}")
                for e in cat.entries:
                    log(f"  {e.describe()}")
                return 0
            cat, legacy, _ = read_catalogue(rx)
            title = args.remove.upper()
            entry = cat.find(title)
            if not entry:
                have = ", ".join(e.title for e in cat.entries) or "nothing"
                raise SystemExit(f"{title} is not installed (installed: {have})")
            steps = plan_remove(entry, legacy, args.remove_saves)
            log(f"removing {entry.describe()}")
            describe_steps(steps, [])
            if not args.remove_saves:
                log(f"  keep its progress {gc.save_name(entry.prefix)} (--remove-saves deletes it too)")
            if args.dry_run:
                log("dry run: nothing changed")
                return 0
            execute(rx, steps, State(cat, legacy))
            log("removed")
            return 0
        finally:
            rx.close()

    args.title = args.title.upper() if args.title else default_title("installing")
    out = OUT_ROOT / args.title
    if not (out / "layout.json").is_file():
        raise SystemExit(f"{args.title} is not packed: run gfs_pack.py --src <its app0> first")
    layout = json.loads((out / "layout.json").read_text())
    index = (out / "index.bin").read_bytes()
    if args.name or args.icon or args.backdrop or args.fetch_art:   # art given now: a new meta.bin
        import profiles
        try:
            gm.write_pack_meta(layout, out, profiles.load(args.title), args.name, args.icon, args.backdrop,
                               args.fetch_art, args.art_dir, args.refetch, log=log)
        except (gm.ArtError, profiles.ProfileError) as e:
            raise SystemExit(str(e))
    if layout.get("meta") and not Path(layout["meta"].get("file", "")).is_file():
        layout.pop("meta")
    src = Path(layout["src"])
    new = parse_header(gc.index_name(layout["prefix"]), index)
    assert new and new.title == args.title, "index.bin does not match layout.json: rerun gfs_pack.py"
    jobs = layout["containers"]
    fit_check(src, args.title, args.no_fit_check)

    rx = connect(args)
    index = keep_installed_prefix(rx, new, layout, index)
    st = survey(rx, new, jobs, args.force)
    steps = plan(st, new, layout, args.force)
    log(f"installing {new.describe()}")
    for t in st.adopted:
        log(f"  add the existing {t} install to {gc.CATALOG} (kept as it is)")
    describe_steps(steps, jobs)
    if not args.no_space_check:
        try:
            check_space(rx, st, steps, new)
        except SystemExit:
            rx.close()
            raise
    if args.dry_run:
        rx.close()
        log("dry run: nothing changed")
        return 0

    execute(rx, steps, st, new, layout, index, src)
    rx.close()
    log("install complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
