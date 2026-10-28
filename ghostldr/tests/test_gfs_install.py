"""Installing ghostfs titles side by side: gfs_pack.py + gfs_install.py against
a fake console that follows gfs_receiver.lua's rules (containers as dicts;
PUT/DELETE/WRITE refuse *SAVE and SWRR's own save, DELSAVE only takes *SAVE;
storage is finite).  The game selector's <prefix>META: written last, only
when it changed, removed with its title; --update-meta writes nothing else.

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import argparse  # noqa: E402
import gfs_catalog as gc  # noqa: E402
import gfs_install as gi  # noqa: E402
import gfs_meta as gm  # noqa: E402
import gfs_pack as gp  # noqa: E402
import profiles  # noqa: E402
from test_profiles import make_sfo  # noqa: E402

NOT_FOUND = gi.NOT_FOUND
NO_SPACE = 0x809F000A
BLOCK = 32768


class FakeConsole:
    """Save containers of one user: {name: {file: bytes}}, each taking its
    blocks of a finite storage."""

    def __init__(self, capacity_blocks: int = 1 << 30) -> None:
        self.saves: dict[str, dict[str, bytes]] = {}
        self.blocks: dict[str, int] = {}
        self.capacity = capacity_blocks
        self.fail_put: str | None = None
        self.ops: list[tuple] = []

    @staticmethod
    def protected(name: str) -> bool:
        return name in gc.SWRR_SAVES or name.endswith("SAVE")

    def used(self) -> int:
        return sum(self.blocks.get(d, 0) for d in self.saves)

    def read_status(self, d: str, f: str, size: int, offset: int = 0) -> tuple[int, bytes | None]:
        if d not in self.saves:
            return NOT_FOUND, None
        data = self.saves[d].get(f)
        return (0, data[offset:offset + size]) if data is not None else (0xFFFFFFFF, None)

    def read(self, d: str, f: str, size: int, offset: int = 0) -> bytes | None:
        return self.read_status(d, f, size, offset)[1]

    def probe(self, d: str) -> tuple[bool, bytes | None]:
        status, tag = self.read_status(d, gi.TAG, 16)
        return status != NOT_FOUND, tag

    def write(self, d: str, f: str, data: bytes) -> int:
        self.ops.append(("write", d, f))
        if self.protected(d):
            return 0xFFFFFFFD
        if d not in self.saves:
            return NOT_FOUND
        self.saves[d][f] = data
        return 0

    def delete(self, d: str) -> int:
        self.ops.append(("delete", d))
        if self.protected(d):
            return 0xFFFFFFFD
        self.saves.pop(d, None)
        return 0               # a 13.60 PS5 answers 0 for a name that doesn't exist too

    def delsave(self, d: str) -> int:
        self.ops.append(("delsave", d))
        if not gc.is_progress(d):
            return 0xFFFFFFFD
        return 0 if self.saves.pop(d, None) is not None else NOT_FOUND

    def put(self, d: str, f: str, blocks: int, size: int, chunks) -> tuple[int, int, int]:
        assert not self.protected(d)
        self.ops.append(("put", d))
        if d == self.fail_put:
            raise SystemExit(f"{d}: connection lost")
        self.saves.pop(d, None)
        if self.used() + blocks > self.capacity:
            return NO_SPACE, 0, 0
        data = b"".join(chunks)
        assert len(data) == size
        self.saves[d] = {f: data}
        self.blocks[d] = blocks
        return 0, size, 0

    def free(self, size: int) -> tuple[int, int]:
        self.ops.append(("free", size))
        return 0, (self.capacity - self.used()) * BLOCK

    def list(self) -> bytes:
        data = self.read(gc.CATALOG, gc.CATALOG_FILE, 1 << 20)
        if gc.parse(data):
            return data
        legacy = [h for n in gc.LEGACY_INDEXES if (h := gi.parse_header(n, self.read(n, gc.INDEX_FILE, 128)))]
        return gc.build(gc.synthesize(legacy), gc.SYNTHESIZED)

    def changes(self) -> list[tuple]:
        return [op for op in self.ops if op[0] != "free"]

    def close(self) -> None:
        pass


def make_title(root: Path, title: str, files: dict[str, bytes], prefix: str | None = None) -> tuple[dict, bytes, Path]:
    src = root / title / "app0"
    for rel, data in files.items():
        (src / rel).parent.mkdir(parents=True, exist_ok=True)
        (src / rel).write_bytes(data)
    prefix = prefix or profiles.load(title).get("pack", {}).get("prefix") or profiles.default_prefix(title)
    info = gp.pack(src, title, prefix, root / title / "out")
    return info, (root / title / "out/index.bin").read_bytes(), src


def install(console: FakeConsole, info: dict, index: bytes, src: Path, force: bool = False,
            space_check: bool = True) -> list[tuple]:
    new = gi.parse_header(gc.index_name(info["prefix"]), index)
    st = gi.survey(console, new, info["containers"], force)
    steps = gi.plan(st, new, info, force)
    if space_check:
        gi.check_space(console, st, steps, new)
    gi.execute(console, steps, st, new, info, index, src)
    return steps


def remove(console: FakeConsole, title: str, saves: bool = False) -> list[tuple]:
    cat, legacy, _ = gi.read_catalogue(console)
    steps = gi.plan_remove(cat.find(title), legacy, saves)
    gi.execute(console, steps, gi.State(cat, legacy))
    return steps


def payload(console: FakeConsole, info: dict) -> bytes:
    return b"".join(console.saves[c["dir"]]["data.bin"] for c in info["containers"])


def catalogue(console: FakeConsole) -> gc.Catalogue:
    return gc.parse(console.saves[gc.CATALOG][gc.CATALOG_FILE])


def make_legacy(console: FakeConsole, info: dict, index: bytes, name: str = "GFSINDEX", src: Path | None = None) -> None:
    """What ghostldr v0.1 left: the data containers (zeros, or the files of
    src) and the single GFSINDEX."""
    for k, c in enumerate(info["containers"]):
        data = b"".join(gi.container_bytes(src, c["pieces"])) if src else b"".join(b"\0" * p[2] for p in c["pieces"])
        console.saves[c["dir"]] = {"data.bin": data,
                                   gi.TAG: gi.tag_bytes(int(info["layout_id"], 16), k, c["size"])}
        console.blocks[c["dir"]] = c["blocks"]
    console.saves[name] = {gc.INDEX_FILE: index}
    console.blocks[name] = 512


@mock.patch.object(gp, "CAP", 64)            # tiny containers: a few bytes of files span several
@mock.patch.object(gi, "log", lambda msg: None)
class InstallTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.console = FakeConsole()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def title(self, title: str, files: dict[str, bytes], prefix: str | None = None):
        return make_title(self.root, title, files, prefix)

    def test_fresh_install_writes_tagged_containers_index_and_catalogue(self) -> None:
        info, index, src = self.title("CUSA11111", {"eboot.bin": b"E" * 100, "data/a.bin": b"A" * 90})
        install(self.console, info, index, src)
        self.assertEqual(self.console.saves["GCUSA11111INDEX"]["index.bin"], index)
        self.assertNotIn("GFSINDEX", self.console.saves)
        self.assertEqual(payload(self.console, info), b"E" * 100 + b"A" * 90)
        for k, c in enumerate(info["containers"]):
            tag = self.console.saves[c["dir"]]["gfs.tag"]
            self.assertEqual(tag, gi.tag_bytes(int(info["layout_id"], 16), k, c["size"]))
        e = catalogue(self.console).find("CUSA11111")
        self.assertEqual((e.prefix, e.index, e.containers, e.complete), ("GCUSA11111", "GCUSA11111INDEX", 3, True))
        self.assertEqual(e.layout_id, int(info["layout_id"], 16))

    def test_another_title_is_installed_beside_the_first(self) -> None:
        a, ia, sa = self.title("CUSA11111", {"eboot.bin": b"a" * 300})
        install(self.console, a, ia, sa)
        self.console.saves["GCUSA11111SAVE"] = {"sdmemory_0.bin": b"progress"}
        before = {d: dict(f) for d, f in self.console.saves.items() if d.startswith("GCUSA11111")}
        b, ib, sb = self.title("CUSA22222", {"eboot.bin": b"b" * 70})
        self.console.ops.clear()
        install(self.console, b, ib, sb)
        touched = {op[1] for op in self.console.changes()}
        self.assertFalse([d for d in touched if d.startswith("GCUSA11111")], touched)
        self.assertEqual({d: f for d, f in self.console.saves.items() if d.startswith("GCUSA11111")}, before)
        self.assertEqual(payload(self.console, a), b"a" * 300)
        self.assertEqual(payload(self.console, b), b"b" * 70)
        self.assertEqual(sorted(e.title for e in catalogue(self.console).entries), ["CUSA11111", "CUSA22222"])

    def test_same_title_complete_sends_nothing(self) -> None:
        info, index, src = self.title("CUSA11111", {"eboot.bin": b"x" * 150})
        install(self.console, info, index, src)
        self.console.saves["GCUSA11111000"]["data.bin"] = b"untouched marker"
        self.console.ops.clear()
        steps = install(self.console, info, index, src)
        self.assertEqual(steps, [])
        self.assertEqual(self.console.changes(), [])
        self.assertEqual(self.console.saves["GCUSA11111000"]["data.bin"], b"untouched marker")

    def test_interrupted_install_resumes_and_is_never_bootable_meanwhile(self) -> None:
        info, index, src = self.title("CUSA11111", {"eboot.bin": b"y" * 200})
        self.console.fail_put = "GCUSA11111002"
        with self.assertRaises(SystemExit):
            install(self.console, info, index, src)
        hdr = gi.parse_header("GCUSA11111INDEX", self.console.saves["GCUSA11111INDEX"]["index.bin"])
        self.assertTrue(hdr.incomplete)           # ghostfs.c refuses to boot this
        self.assertFalse(catalogue(self.console).find("CUSA11111").complete)
        self.console.fail_put = None
        steps = install(self.console, info, index, src)
        self.assertEqual([s[1] for s in steps if s[0] == "put"], [2, 3])
        self.assertEqual(payload(self.console, info), b"y" * 200)
        self.assertFalse(gi.parse_header("GCUSA11111INDEX", self.console.saves["GCUSA11111INDEX"]["index.bin"]).incomplete)
        self.assertTrue(catalogue(self.console).find("CUSA11111").complete)

    def test_new_layout_replaces_only_its_own_containers(self) -> None:
        a, ia, sa = self.title("CUSA11111", {"eboot.bin": b"a" * 200})
        install(self.console, a, ia, sa)
        b, ib, sb = self.title("CUSA22222", {"eboot.bin": b"b" * 200})
        install(self.console, b, ib, sb)
        self.assertIn("GCUSA22222003", self.console.saves)
        self.console.saves["GCUSA22222SAVE"] = {"sdmemory_0.bin": b"kept"}
        (sb / "eboot.bin").write_bytes(b"c" * 70)            # a smaller layout of the same title
        b2, ib2, _ = make_title(self.root, "CUSA22222", {"eboot.bin": b"c" * 70})
        install(self.console, b2, ib2, sb)
        self.assertEqual(payload(self.console, b2), b"c" * 70)
        self.assertNotIn("GCUSA22222002", self.console.saves)    # swept: past the new count
        self.assertNotIn("GCUSA22222003", self.console.saves)
        self.assertEqual(self.console.saves["GCUSA22222SAVE"], {"sdmemory_0.bin": b"kept"})
        self.assertEqual(payload(self.console, a), b"a" * 200)

    def test_deletes_touch_only_this_titles_containers(self) -> None:
        a, ia, sa = self.title("CUSA11111", {"eboot.bin": b"a" * 200})
        install(self.console, a, ia, sa)
        other_tag = gi.tag_bytes(0x1234, 5, 64)                   # another title's, under the same names
        self.console.saves["GCUSA11111005"] = {"data.bin": b"other", gi.TAG: other_tag}
        self.console.saves["GCUSA11111006"] = {"data.bin": b"untagged"}
        a2, ia2, _ = make_title(self.root, "CUSA11111", {"eboot.bin": b"c" * 70})
        install(self.console, a2, ia2, sa)
        self.assertNotIn("GCUSA11111003", self.console.saves)    # this title's, past the new count: swept
        self.assertIn("GCUSA11111005", self.console.saves)       # tagged for another layout: left
        self.assertIn("GCUSA11111006", self.console.saves)       # no tag, past the title's range: left
        deletes = [op[1] for op in self.console.changes() if op[0] == "delete"]
        self.assertFalse([d for d in deletes if d.startswith("GCUSA11111") and d[-3:].isdigit()
                          and int(d[-3:]) > 6], deletes)          # no blind deletes of names that don't exist
        remove(self.console, "CUSA11111")
        self.assertEqual(sorted(d for d in self.console.saves if d.startswith("GCUSA11111")),
                         ["GCUSA11111005", "GCUSA11111006"])

    def test_titles_with_the_same_digits_have_their_own_containers(self) -> None:
        a, ia, sa = self.title("APOL00004", {"eboot.bin": b"a" * 100})
        b, ib, sb = self.title("CUSA00004", {"eboot.bin": b"b" * 100})
        install(self.console, a, ia, sa)
        install(self.console, b, ib, sb)
        self.assertEqual((a["prefix"], b["prefix"]), ("GAPOL00004", "GCUSA00004"))
        self.assertEqual((payload(self.console, a), payload(self.console, b)), (b"a" * 100, b"b" * 100))

    def test_a_title_installed_with_the_old_default_prefix_keeps_it(self) -> None:
        old, iold, sold = make_title(self.root, "CUSA11111", {"eboot.bin": b"a" * 200}, prefix="G11111")
        install(self.console, old, iold, sold)
        self.console.saves["G11111SAVE"] = {"sdmemory_0.bin": b"progress"}
        new, inew, snew = make_title(self.root, "CUSA11111", {"eboot.bin": b"d" * 130})
        self.assertEqual(new["prefix"], "GCUSA11111")
        inew = gi.keep_installed_prefix(self.console, gi.parse_header(gc.index_name(new["prefix"]), inew), new, inew)
        install(self.console, new, inew, snew)
        e = catalogue(self.console).find("CUSA11111")
        self.assertEqual((e.prefix, e.index), ("G11111", "G11111INDEX"))
        self.assertEqual(payload(self.console, new), b"d" * 130)
        self.assertEqual(self.console.saves["G11111SAVE"], {"sdmemory_0.bin": b"progress"})
        self.assertFalse([d for d in self.console.saves if d.startswith("GCUSA11111")])
        self.assertEqual(gi.parse_header("G11111INDEX", self.console.saves["G11111INDEX"]["index.bin"]).prefix,
                         "G11111")

    def test_not_enough_space_stops_before_anything_changes(self) -> None:
        a, ia, sa = self.title("CUSA11111", {"eboot.bin": b"a" * 100})
        install(self.console, a, ia, sa)
        self.console.capacity = self.console.used() + 3 * 32768     # room for 3 more containers
        b, ib, sb = self.title("CUSA22222", {"eboot.bin": b"b" * 64 * 5})
        before = {d: dict(f) for d, f in self.console.saves.items()}
        self.console.ops.clear()
        with self.assertRaises(SystemExit) as cm:
            install(self.console, b, ib, sb)
        self.assertIn("not enough space", str(cm.exception))
        self.assertEqual(self.console.changes(), [])
        self.assertEqual(self.console.saves, before)

    def test_a_prefix_taken_by_another_title_is_refused(self) -> None:
        a, ia, sa = self.title("CUSA11111", {"eboot.bin": b"a" * 10}, prefix="SHARED")
        install(self.console, a, ia, sa)
        b, ib, sb = self.title("CUSA22222", {"eboot.bin": b"b" * 10}, prefix="SHARED")
        with self.assertRaises(SystemExit) as cm:
            install(self.console, b, ib, sb)
        self.assertIn("belongs to CUSA11111", str(cm.exception))
        self.assertEqual(payload(self.console, a), b"a" * 10)

    def test_remove_deletes_data_and_index_and_keeps_progress(self) -> None:
        a, ia, sa = self.title("CUSA11111", {"eboot.bin": b"a" * 200})
        b, ib, sb = self.title("CUSA22222", {"eboot.bin": b"b" * 100})
        install(self.console, a, ia, sa)
        install(self.console, b, ib, sb)
        self.console.saves["GCUSA11111SAVE"] = {"sdmemory_0.bin": b"progress"}
        self.console.saves["SLUS-20268"] = {"VMC0.card": b"swrr"}
        remove(self.console, "CUSA11111")
        self.assertFalse([d for d in self.console.saves if d.startswith("GCUSA11111") and d != "GCUSA11111SAVE"])
        self.assertIn("GCUSA11111SAVE", self.console.saves)
        self.assertEqual(payload(self.console, b), b"b" * 100)
        self.assertEqual([e.title for e in catalogue(self.console).entries], ["CUSA22222"])
        self.assertIn("SLUS-20268", self.console.saves)

    def test_remove_with_saves_deletes_the_progress_but_never_swrrs_save(self) -> None:
        a, ia, sa = self.title("CUSA11111", {"eboot.bin": b"a" * 100})
        install(self.console, a, ia, sa)
        for name in ("GCUSA11111SAVE", "GCUSA11111D001SAVE", "GCUSA11111D002SAVE", "GCUSA11111M00SAVE", "SLUS-20268", "SLES-50366"):
            self.console.saves[name] = {"x": b"1"}
        remove(self.console, "CUSA11111", saves=True)
        self.assertEqual(sorted(self.console.saves), [gc.CATALOG, "SLES-50366", "SLUS-20268"])
        self.assertFalse([op for op in self.console.ops if op[0] in ("delete", "delsave") and op[1] in gc.SWRR_SAVES])

    def test_interrupted_remove_is_finished_on_rerun(self) -> None:
        a, ia, sa = self.title("CUSA11111", {"eboot.bin": b"a" * 200})
        install(self.console, a, ia, sa)
        real = self.console.delete

        def flaky(d: str) -> int:
            if d == "GCUSA11111001":
                raise SystemExit("connection lost")
            return real(d)
        with mock.patch.object(self.console, "delete", side_effect=flaky):
            with self.assertRaises(SystemExit):
                remove(self.console, "CUSA11111")
        self.assertFalse(catalogue(self.console).find("CUSA11111").complete)   # never booted meanwhile
        remove(self.console, "CUSA11111")
        self.assertEqual(sorted(self.console.saves), [gc.CATALOG])

    def test_name_and_art_are_written_last_and_only_when_changed(self) -> None:
        icon = gm.png_encode(gm.Image(8, 8, bytes((200, 30, 30, 255)) * 64))
        info, index, src = self.title("CUSA11111", {"eboot.bin": b"a" * 100, "sce_sys/icon0.png": icon})
        out = self.root / "CUSA11111/out"
        gm.write_pack_meta(info, out, profiles.load("CUSA11111"), log=lambda s: None)
        steps = install(self.console, info, index, src)
        self.assertEqual(steps[-2:], [("catalog", steps[-2][1]), ("meta",)])     # after the title is complete
        meta = self.console.saves["GCUSA11111META"][gm.META_FILE]
        self.assertEqual(meta, (out / gm.META_FILE).read_bytes())
        self.assertEqual(gm.parse(meta)["sections"][gm.SECT_ICON]["source"], gm.ART_TITLE)
        self.assertEqual(install(self.console, info, index, src), [])           # the same: nothing to send
        backdrop = self.root / "bd.png"
        backdrop.write_bytes(gm.png_encode(gm.Image(16, 9, bytes((0, 0, 90, 255)) * 144)))
        gm.write_pack_meta(info, out, profiles.load("CUSA11111"), backdrop=backdrop, log=lambda s: None)
        self.console.ops.clear()
        self.assertEqual(install(self.console, info, index, src), [("meta",)])
        self.assertEqual(self.console.changes(), [("put", "GCUSA11111META")])
        remove(self.console, "CUSA11111")
        self.assertNotIn("GCUSA11111META", self.console.saves)

    def test_receiver_refuses_to_touch_progress(self) -> None:
        self.console.saves["GHOSTSAVE"] = {"x": b"1"}
        self.assertNotEqual(self.console.delete("GHOSTSAVE"), 0)
        self.assertNotEqual(self.console.delsave("SLUS-20268"), 0)
        self.assertIn("GHOSTSAVE", self.console.saves)


@mock.patch.object(gp, "CAP", 64)
@mock.patch.object(gi, "log", lambda msg: None)
class LegacyGhostTests(unittest.TestCase):
    """Ghost installed by ghostldr v0.1: GHOST000.. + GFSINDEX (or GHOSTIDX), no catalogue."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.console = FakeConsole()
        self.ghost, self.gindex, self.gsrc = make_title(self.root, "CUSA11456", {"eboot.bin": b"g" * 130})
        make_legacy(self.console, self.ghost, self.gindex)
        self.console.saves["GHOSTSAVE"] = {"sdmemory_0.bin": b"jin"}
        self.legacy_bytes = {d: dict(f) for d, f in self.console.saves.items()}

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_list_without_a_catalogue_shows_the_legacy_install(self) -> None:
        cat = gc.parse(self.console.list())
        self.assertFalse(cat.exists)
        e = cat.entries[0]
        self.assertEqual((e.title, e.prefix, e.index, e.legacy, e.complete), ("CUSA11456", "GHOST", "GFSINDEX",
                                                                             True, True))

    def test_reinstalling_the_same_ghost_only_catalogues_it(self) -> None:
        steps = install(self.console, self.ghost, self.gindex, self.gsrc)
        self.assertEqual([s[0] for s in steps], ["catalog"])
        for d, files in self.legacy_bytes.items():
            self.assertEqual(self.console.saves[d], files)            # GFSINDEX and the data, unchanged
        e = catalogue(self.console).find("CUSA11456")
        self.assertEqual((e.index, e.legacy, e.complete), ("GFSINDEX", True, True))
        self.assertEqual(install(self.console, self.ghost, self.gindex, self.gsrc), [])

    def test_ghost_is_kept_when_another_title_is_installed(self) -> None:
        b, ib, sb = make_title(self.root, "CUSA22222", {"eboot.bin": b"b" * 10})
        install(self.console, b, ib, sb)
        for d, files in self.legacy_bytes.items():
            self.assertEqual(self.console.saves[d], files)
        cat = catalogue(self.console)
        self.assertEqual(sorted(e.title for e in cat.entries), ["CUSA11456", "CUSA22222"])
        self.assertEqual(cat.find("CUSA11456").index, "GFSINDEX")

    def test_a_new_ghost_layout_moves_it_to_its_own_index(self) -> None:
        (self.gsrc / "eboot.bin").write_bytes(b"h" * 100)
        g2, ig2, _ = make_title(self.root, "CUSA11456", {"eboot.bin": b"h" * 100})
        steps = install(self.console, g2, ig2, self.gsrc)
        self.assertIn(("delete", "GFSINDEX"), steps)
        self.assertNotIn("GFSINDEX", self.console.saves)
        self.assertEqual(self.console.saves["GHOSTINDEX"]["index.bin"], ig2)
        self.assertEqual(catalogue(self.console).find("CUSA11456").index, "GHOSTINDEX")
        self.assertEqual(self.console.saves["GHOSTSAVE"], {"sdmemory_0.bin": b"jin"})

    def test_version_1_index_with_the_same_data_is_only_upgraded(self) -> None:
        # rewrite it as a version 1 install: GHOSTIDX, 64-byte header, no title
        hdr = struct.unpack_from("<4sIIIQQII16s", self.gindex)
        v1 = struct.pack("<4sIIIQQII16s", b"GFS1", 1, *hdr[2:6], hdr[6] - 64, hdr[7], hdr[8]).ljust(64, b"\0")
        self.console.saves["GHOSTIDX"] = {"index.bin": v1 + self.gindex[gp.HEADER_SIZE:]}
        del self.console.saves["GFSINDEX"]
        steps = install(self.console, self.ghost, self.gindex, self.gsrc)
        self.assertEqual([s[0] for s in steps], ["index", "catalog", "delete"])
        self.assertNotIn("GHOSTIDX", self.console.saves)
        self.assertEqual(self.console.saves["GHOSTINDEX"]["index.bin"], self.gindex)
        self.assertEqual(self.console.saves["GHOSTSAVE"], {"sdmemory_0.bin": b"jin"})

    def test_remove_legacy_ghost(self) -> None:
        remove(self.console, "CUSA11456")
        self.assertEqual(sorted(self.console.saves), [gc.CATALOG, "GHOSTSAVE"])

    def update_meta(self, **kw) -> int:
        args = argparse.Namespace(title="CUSA11456", src=None, name=None, icon=None, backdrop=None, fetch_art=False,
                                  art_dir=None, refetch=False, force=False, dry_run=False)
        for k, v in kw.items():
            setattr(args, k, v)
        with mock.patch.object(gm, "OUT_ROOT", self.root / "packs"):
            return gm.update_meta(self.console, args, lambda msg: None)

    def test_update_meta_writes_only_ghostmeta(self) -> None:
        icon, back = self.root / "icon.png", self.root / "back.png"
        icon.write_bytes(gm.png_encode(gm.Image(64, 64, bytes((250, 250, 240, 255)) * 4096)))
        back.write_bytes(gm.png_encode(gm.Image(64, 36, bytes((20, 20, 20, 255)) * 2304)))
        self.console.ops.clear()
        self.assertEqual(self.update_meta(icon=icon, backdrop=back, name="My Ghost", dry_run=True), 0)
        self.assertEqual(self.console.changes(), [])
        self.assertEqual(self.update_meta(icon=icon, backdrop=back, name="My Ghost"), 0)
        self.assertEqual(self.console.changes(), [("put", "GHOSTMETA")])
        for d, files in self.legacy_bytes.items():
            self.assertEqual(self.console.saves[d], files)        # GHOST000.., GFSINDEX, GHOSTSAVE: untouched
        self.assertNotIn(gc.CATALOG, self.console.saves)          # not even the catalogue is written
        info = gm.parse(self.console.saves["GHOSTMETA"][gm.META_FILE])
        # its profile names it (over --name); the dump has no sce_sys: the given art
        self.assertEqual((info["title"], info["name"], info["name_source"]), ("CUSA11456", "Ghost of Tsushima",
                                                                            gm.NAME_PROFILE))
        self.assertEqual([x["source"] for x in info["sections"]], [0, gm.ART_GIVEN, gm.ART_GIVEN])
        self.assertEqual(info["version"], "02.18")                 # the profile's: Ghost's dump has no param.sfo
        self.assertTrue(info["flags"] & gm.FLAG_SCANNED)
        self.console.ops.clear()
        self.assertEqual(self.update_meta(icon=icon, backdrop=back), 0)
        self.assertEqual(self.console.changes(), [])               # up to date
        self.assertEqual(self.update_meta(), 0)                    # nothing to store: GHOSTMETA is kept
        self.assertEqual(self.console.changes(), [])
        self.assertIn("GHOSTMETA", self.console.saves)
        with self.assertRaises(SystemExit):
            self.update_meta(title="CUSA99999")

    def test_update_meta_reads_the_titles_own_art_from_the_console(self) -> None:
        icon0 = gm.png_encode(gm.Image(8, 8, bytes((1, 2, 3, 255)) * 64))
        sfo = make_sfo({"TITLE": "Ghost of Tsushima (sfo)", "TITLE_ID": "CUSA11456"})
        files = {"eboot.bin": b"g" * 130, "sce_sys/icon0.png": icon0, "sce_sys/param.sfo": sfo}
        root = self.root / "own"
        info, index, src = make_title(root, "CUSA11456", files)
        console = FakeConsole()
        make_legacy(console, info, index, src=src)
        self.console = console
        self.assertEqual(self.update_meta(), 0)                    # no dump here: read through GFSINDEX
        meta = console.saves["GHOSTMETA"][gm.META_FILE]
        head = gm.parse(meta)
        self.assertEqual((head["name"], head["name_source"]), ("Ghost of Tsushima (sfo)", gm.NAME_SFO))
        sfo_s = head["sections"][gm.SECT_SFO]
        self.assertEqual(meta[sfo_s["offset"]:sfo_s["offset"] + sfo_s["size"]], sfo)
        self.assertEqual(head["sections"][gm.SECT_ICON]["source"], gm.ART_TITLE)

    def test_meta_never_names_another_container(self) -> None:
        cat, _, _ = gi.read_catalogue(self.console)
        self.assertEqual(gm.meta_target(cat, "CUSA11456", "GHOST"), "GHOSTMETA")
        for title, prefix in (("CUSA22222", "GHOST"), ("CUSA11456", "ghost"), ("CUSA11456", "GHOST-1")):
            with self.assertRaises(SystemExit):
                gm.meta_target(cat, title, prefix)


@mock.patch.object(gp, "CAP", 64)
@mock.patch.object(gi, "log", lambda msg: None)
class CommandLineTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.console = FakeConsole()
        self.lines: list[str] = []

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_main(self, *argv: str) -> int:
        with mock.patch.object(gi, "connect", lambda args: self.console), \
                mock.patch.object(gi, "log", self.lines.append), mock.patch.object(gi, "OUT_ROOT", self.root / "packs"), \
                mock.patch.object(sys, "argv", ["gfs_install.py", *argv]):
            return gi.main()

    def test_dry_run_list_and_remove(self) -> None:
        a, ia, sa = make_title(self.root, "CUSA11111", {"eboot.bin": b"a" * 100})
        install(self.console, a, ia, sa)
        info, index, src = make_title(self.root, "CUSA22222", {"eboot.bin": b"b" * 100})
        packs = self.root / "packs/CUSA22222"
        packs.mkdir(parents=True)
        (packs / "layout.json").write_text(__import__("json").dumps(info))
        (packs / "index.bin").write_bytes(index)
        self.console.ops.clear()
        self.assertEqual(self.run_main("--title", "CUSA22222", "--dry-run"), 0)
        self.assertEqual(self.console.changes(), [])
        self.assertTrue(any("send 2 containers" in line for line in self.lines), self.lines)
        self.assertEqual(self.run_main("--title", "CUSA22222"), 0)
        self.lines.clear()
        self.assertEqual(self.run_main("--list"), 0)
        self.assertTrue(any(line.startswith("2 titles installed") for line in self.lines), self.lines)
        with self.assertRaises(SystemExit) as cm:
            self.run_main("--remove", "CUSA33333")
        self.assertIn("not installed (installed: CUSA11111, CUSA22222)", str(cm.exception))
        self.assertEqual(self.run_main("--remove", "cusa11111"), 0)
        self.assertEqual([e.title for e in catalogue(self.console).entries], ["CUSA22222"])


class CatalogueTests(unittest.TestCase):
    def test_round_trip_keeps_unknown_bytes_and_sorts(self) -> None:
        cat = gc.Catalogue([gc.Entry("CUSA22222", "GCUSA22222", "GCUSA22222INDEX", "Zweite", "01.00", 7, 99, 1, 0, 5,
                                     b"future"),
                            gc.Entry("CUSA11456", "GHOST", "GFSINDEX", "Ghost of Tsushima", "02.18", 0xe73b, 1, 56,
                                     gc.LEGACY_INDEX)], 3)
        data = gc.build(cat)
        self.assertEqual(gc.HEADER.size, 64)
        self.assertEqual(gc.ENTRY.size, 192)
        back = gc.parse(data)
        self.assertEqual([e.title for e in back.entries], ["CUSA11456", "CUSA22222"])
        self.assertEqual(back.find("CUSA22222").extra.rstrip(b"\0"), b"future")
        self.assertEqual(back.generation, 3)
        self.assertTrue(back.exists)

    def test_damaged_catalogue_is_missing(self) -> None:
        data = gc.build(gc.Catalogue([gc.Entry("CUSA1", "A", "AINDEX")]))
        self.assertIsNone(gc.parse(data[:-1]))
        self.assertIsNone(gc.parse(b"GFSX" + data[4:]))

    def test_prefix_rules(self) -> None:
        for good in ("GHOST", "G12345", "CUSA12345", "A"):
            self.assertIsNone(gc.prefix_problem(good), good)
        for bad in ("", "ghost", "TOOLONGPREFIX", "GFSX", "GAMED123", "GAMEM12", "G-1"):
            self.assertIsNotNone(gc.prefix_problem(bad), bad)
        names = {gc.data_name("GAME", 95), gc.index_name("GAME"), gc.save_name("GAME"), "GAMED999SAVE"}
        self.assertTrue(all(len(n) <= 31 for n in names))


class PackTests(unittest.TestCase):
    # the layout installed on consoles since 2026-10-20 (GHOST000..055 + GFSINDEX)
    GHOST_LAYOUT = "e73b4cbf2798a86e"

    def test_ghost_layout_is_the_installed_one(self) -> None:
        src = profiles.DEFAULT_SRC
        if not (src / "eboot.bin").exists() or profiles.detect_title(src) != "CUSA11456":
            self.skipTest("needs the CUSA11456 v2.18 dump in build/ghost/app0")
        with tempfile.TemporaryDirectory() as tmp:
            info = gp.pack(src, "CUSA11456", "GHOST", Path(tmp))
        self.assertEqual(info["layout_id"], self.GHOST_LAYOUT)
        self.assertEqual(len(info["containers"]), 56)
        self.assertEqual(info["app_params"][:2], [3, 11456])


if __name__ == "__main__":
    unittest.main()
