#!/usr/bin/env python3
"""The ghostfs title catalogue and container names (docs/ghostfs-catalogue.md).

Several titles live side by side in SWRR's save data.  Each has a prefix
<P>: data containers <P>000..<P>095, its index <P>INDEX (or the legacy
GFSINDEX / GHOSTIDX of a v0.1 install), and its progress <P>SAVE (later
<P>D001SAVE.., <P>M00SAVE..).  GFSCATALOG/catalog.bin lists the installed
titles.

The core's twin is ghostldr/native/src/ghostfs.c; the receiver's LIST op
(ghostldr/lua/gfs_receiver.lua) builds the same bytes when there is no
catalogue yet.
"""

from __future__ import annotations

import re
import struct
import time
from dataclasses import dataclass, field

CATALOG = "GFSCATALOG"
CATALOG_FILE = "catalog.bin"
CATALOG_BLOCKS = 512                 # 16 MiB: small containers lose a lot to filesystem overhead
INDEX_FILE = "index.bin"
LEGACY_INDEXES = ("GFSINDEX", "GHOSTIDX")    # v0.1.x single-title indexes, newest first
SWRR_SAVES = ("SLUS-20268", "SLES-50366")    # SWRR's own save (with Luac0re): never touched
MAX_CONTAINERS = 96                  # ghostfs.c GFS_MAX_CONT
MAX_TITLES = 64

MAGIC = b"GFSC"
VERSION = 1
HEADER = struct.Struct("<4sIIIIIQ32x")                 # 64 bytes
ENTRY = struct.Struct("<16s16s32s64s16sQQIIQ16x")       # 192 bytes
INCOMPLETE = 1                       # entry flags
LEGACY_INDEX = 2
SYNTHESIZED = 1                      # header flags, LIST replies only: no GFSCATALOG, built from a legacy index

PREFIX_RE = re.compile(r"[A-Z0-9]{1,12}")


def prefix_problem(prefix: str) -> str | None:
    """Why `prefix` cannot be a title's container prefix (None if it can)."""
    if not PREFIX_RE.fullmatch(prefix or ""):
        return "a prefix is 1-12 of A-Z 0-9"
    if prefix.startswith("GFS"):
        return "prefixes starting with GFS are ghostldr's own (GFSINDEX, GFSCATALOG)"
    if re.search(r"D\d{3}$|M\d{2}$", prefix):
        return "a prefix must not end in D + 3 digits or M + 2 digits (its SAVE container would look like " \
               "another title's save directory)"
    return None


def data_name(prefix: str, k: int) -> str:
    return f"{prefix}{k:03d}"


def index_name(prefix: str) -> str:
    return f"{prefix}INDEX"


def save_name(prefix: str) -> str:
    return f"{prefix}SAVE"


def is_progress(name: str) -> bool:
    """A title's progress container (what only DELSAVE may delete)."""
    return name.endswith("SAVE") and name not in SWRR_SAVES


@dataclass
class Entry:
    title: str
    prefix: str
    index: str
    name: str = ""
    version: str = ""
    layout_id: int = 0
    total: int = 0
    containers: int = 0
    flags: int = 0
    updated: int = 0
    extra: bytes = field(default=b"", repr=False)   # bytes past the version-1 entry (kept as they are)

    @property
    def complete(self) -> bool:
        return not self.flags & INCOMPLETE

    @property
    def legacy(self) -> bool:
        return bool(self.flags & LEGACY_INDEX)

    def describe(self) -> str:
        what = f"{self.title or '(untitled)'}" + (f" {self.name}" if self.name else "") + \
               (f" {self.version}" if self.version else "")
        state = "" if self.complete else ", INCOMPLETE"
        return (f"{what}: {self.containers} containers {self.prefix}000.. ({self.total / 2**30:.1f} GiB), "
                f"index {self.index}{state}")


@dataclass
class Catalogue:
    entries: list[Entry] = field(default_factory=list)
    generation: int = 0
    exists: bool = False              # GFSCATALOG is on the console (else it is created on the first write)

    def find(self, title: str) -> Entry | None:
        return next((e for e in self.entries if e.title == title), None)

    def by_prefix(self, prefix: str) -> Entry | None:
        return next((e for e in self.entries if e.prefix == prefix), None)

    def put(self, entry: Entry) -> None:
        self.entries = [e for e in self.entries if e.title != entry.title] + [entry]

    def drop(self, title: str) -> None:
        self.entries = [e for e in self.entries if e.title != title]


def _s(raw: bytes) -> str:
    return raw.split(b"\0")[0].decode("utf-8", "replace")


def _b(text: str, size: int) -> bytes:
    raw = text.encode("utf-8")[:size - 1]
    while raw:                        # never cut a UTF-8 sequence in half
        try:
            raw.decode("utf-8")
            break
        except UnicodeDecodeError:
            raw = raw[:-1]
    return raw


def parse(data: bytes | None) -> Catalogue | None:
    """A catalog.bin (or a LIST reply); None when it is missing or damaged."""
    if not data or len(data) < HEADER.size:
        return None
    magic, version, hsize, esize, count, flags, generation = HEADER.unpack_from(data)
    if magic != MAGIC or hsize < HEADER.size or esize < ENTRY.size or count > 4096 \
            or len(data) < hsize + count * esize:
        return None
    entries = []
    for i in range(count):
        off = hsize + i * esize
        t, p, ix, nm, ver, lid, total, conts, eflags, updated = ENTRY.unpack_from(data, off)
        entries.append(Entry(_s(t), _s(p), _s(ix), _s(nm), _s(ver), lid, total, conts, eflags, updated,
                             data[off + ENTRY.size:off + esize]))
    return Catalogue(entries, generation, not flags & SYNTHESIZED)


def build(cat: Catalogue, flags: int = 0) -> bytes:
    """catalog.bin for `cat`, entries sorted by title.  Unknown trailing
    entry bytes of a newer writer are kept (entry_size grows to fit)."""
    if len(cat.entries) > MAX_TITLES:
        raise ValueError(f"at most {MAX_TITLES} titles")
    entries = sorted(cat.entries, key=lambda e: e.title)
    esize = ENTRY.size + max((len(e.extra) for e in entries), default=0)
    out = bytearray(HEADER.pack(MAGIC, VERSION, HEADER.size, esize, len(entries), flags, cat.generation))
    for e in entries:
        rec = ENTRY.pack(_b(e.title, 16), _b(e.prefix, 16), _b(e.index, 32), _b(e.name, 64), _b(e.version, 16),
                         e.layout_id, e.total, e.containers, e.flags, e.updated) + e.extra
        out += rec.ljust(esize, b"\0")
    return bytes(out)


def entry_from_header(index: str, header) -> Entry:
    """A legacy entry for an index found on the console (an Install from
    gfs_install.parse_header)."""
    return Entry(header.title, header.prefix, index, layout_id=header.layout_id, total=header.total,
                 containers=header.containers, flags=LEGACY_INDEX | (INCOMPLETE if header.incomplete else 0))


def synthesize(headers: list) -> Catalogue:
    """What LIST answers without a GFSCATALOG: the legacy index, if any
    (gfs_receiver.lua does the same)."""
    cat = Catalogue(exists=False)
    for h in headers[:1]:
        cat.put(entry_from_header(h.name, h))
    return cat


def now() -> int:
    return int(time.time())
