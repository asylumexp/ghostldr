"""The network installer's page (ghostldr/data/netinstall.html), offline.

Its JavaScript packs a game the way gfs_pack.py does and installs it the way
gfs_install.py does; node runs that part (netinstall_node.js) and these tests
hold it to the Python tools byte for byte:

  - the layout, index.bin and layout id of the same files: the generic order,
    homebrew, files across containers, empty folders, Ghost of Tsushima's
    profile (its open order, groups, late files and exclusions);
  - installs and removals against the same fake console as
    test_gfs_install.py: a fresh install, a second title beside it, an
    interrupted install carried on, nothing to send, a new layout, Ghost's
    v0.1 install adopted, the old default prefix kept, too little space;
  - a .zip read in the browser: stored and deflated entries, a folder above
    app0, macOS junk, each container's bytes whole and from odd offsets;
  - the page's generated parts (profiles, manager.lua's copy) up to date.

Needs node (https://nodejs.org); without it only the up-to-date check runs.

    python3 -m unittest discover -s ghostldr/tests -p test_netinstall.py
"""

from __future__ import annotations

import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PAGE = ROOT / "ghostldr/data/netinstall.html"
DRIVER = HERE / "netinstall_node.js"
sys.path.insert(0, str(ROOT / "ghostldr/host"))
sys.path.insert(0, str(ROOT / "ghostldr/tools"))
sys.path.insert(0, str(HERE))
import gfs_catalog as gc  # noqa: E402
import gfs_install as gi  # noqa: E402
import gfs_pack as gp  # noqa: E402
import netinstall_page  # noqa: E402
import profiles  # noqa: E402
from test_gfs_install import FakeConsole  # noqa: E402
from test_profiles import make_sfo  # noqa: E402

NODE = shutil.which("node")
CAP = 64


def node(req: dict) -> dict:
    out = subprocess.run([NODE, str(DRIVER), str(PAGE)], input=json.dumps(req).encode(), capture_output=True,
                         timeout=120)
    if out.stderr:
        sys.stderr.write(out.stderr.decode())
    ans = json.loads(out.stdout or b"{}")
    if "crash" in ans:
        raise AssertionError(ans["crash"])
    return ans


def tree(root: Path, files: dict[str, bytes], dirs: tuple[str, ...] = ()) -> Path:
    for rel, data in files.items():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_bytes(data)
    for d in dirs:
        (root / d).mkdir(parents=True, exist_ok=True)
    return root


def flat(saves: dict) -> dict:
    """{container: {file: bytes}} with the catalogue's clock fields zeroed."""
    out = {}
    for d, files in saves.items():
        out[d] = {}
        for f, data in files.items():
            data = bytes.fromhex(data) if isinstance(data, str) else data
            if d == gc.CATALOG and f == gc.CATALOG_FILE:
                b = bytearray(data)
                _, _, hsize, esize, count = struct.unpack_from("<4sIIII", b)
                for i in range(count):
                    b[hsize + i * esize + 0xA8:hsize + i * esize + 0xB0] = bytes(8)
                data = bytes(b)
            out[d][f] = data
    return out


@unittest.skipUnless(NODE, "needs node")
class PackParityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def same(self, files: dict[str, bytes], title: str, prefix: str, dirs: tuple[str, ...] = (), cap: int = CAP,
             pk=None) -> dict:
        src = tree(self.root / title, files, dirs)
        with mock.patch.object(gp, "CAP", cap):
            info = gp.pack(src, title, prefix, self.root / "out" / title)
        index = (self.root / "out" / title / "index.bin").read_bytes()
        req = {"op": "pack", "cap": cap, "title": title, "prefix": prefix, "dirs": list(dirs),
               "files": [[rel, len(data)] for rel, data in files.items()]}
        if pk is not None:
            req["pk"] = pk
        js = node(req)
        self.assertEqual(js["layout_id"], info["layout_id"])
        self.assertEqual(js["containers"], [{k: c[k] for k in ("dir", "blocks", "size", "pieces")}
                                            for c in info["containers"]])
        self.assertEqual(bytes.fromhex(js["index"]), index)
        self.assertEqual(js["index_blocks"], info["index_blocks"])
        return js

    def test_generic_order_across_containers(self) -> None:
        files = {"eboot.bin": b"E" * 150, "sce_module/libc.prx": b"L" * 40, "sce_sys/param.sfo": make_sfo({"TITLE_ID": "CUSA12345"}),
                 "sce_sys/icon0.png": b"I" * 70, "data/levels/b.pak": b"B" * 90, "data/levels/a.pak": b"A" * 33,
                 "data/small.cfg": b"c" * 3, "movies/intro.mp4": b"M" * 200, "readme.txt": b"r"}
        js = self.same(files, "CUSA12345", "GCUSA12345")
        self.assertGreater(len(js["containers"]), 5)

    def test_generic_order_at_the_real_size(self) -> None:
        files = {"eboot.bin": b"E" * 5000, "sce_sys/param.sfo": make_sfo({"TITLE_ID": "CUSA00001"}), "a/b/c.bin": b"x" * 70000,
                 "z.dat": b"z" * 10}
        js = self.same(files, "CUSA00001", "GCUSA00001", cap=gp.CAP)
        self.assertEqual(len(js["containers"]), 1)

    def test_homebrew_with_empty_folders_and_odd_names(self) -> None:
        files = {"eboot.bin": b"\x7fELF" + b"h" * 60, "assets/fonts/Font Regular.ttf": b"f" * 20,
                 "assets/émoji ✓.txt": b"u" * 5, "assets/Z.bin": b"Z" * 7, "assets/a.bin": b"a" * 7}
        self.same(files, "HELLO_WORLD", "GHELLOWORLD", dirs=("empty", "assets/also-empty"))

    def test_ghost_profile_order_groups_late_and_exclusions(self) -> None:
        prof = profiles.load("CUSA11456")["pack"]
        order = [s.strip() for s in (profiles.TITLES / prof["order"]).read_text().splitlines()
                 if s.strip() and not s.startswith("#")]
        files = {rel: bytes([i % 251]) * (1 + i % 5) for i, rel in enumerate(order[:60])}
        files.update({"eboot.bin": b"E" * 9, "eboot.bin.xref": b"x", "cache/psarc/lang_french_audio.psarc": b"F" * 3,
                      "cache/psarc/lang_english_audio.psarc": b"G" * 3, "cache/IZU_beach.bin": b"i" * 4,
                      "cache/toy_x.bin": b"t", "cache/foo-Kam-bar": b"k", "cache/meshes/m.bin": b"m" * 6,
                      "cache/bitmaps/b.bin": b"b" * 3, "movies/end.bk2": b"v" * 8, "other/thing": b"o",
                      "ui/menu.swf": b"u" * 2})
        js = self.same(files, "CUSA11456", "GHOST")
        names = [p[0] for c in js["containers"] for p in c["pieces"]]
        self.assertNotIn("eboot.bin.xref", names)
        self.assertNotIn("cache/psarc/lang_french_audio.psarc", names)
        self.assertGreater(js["traced"], 0)

    def test_js_regex_spells_out_scoped_case_folding(self) -> None:
        js = netinstall_page.js_regex(r"^cache/(?i:izu[^/]*|[^/]*[_.#&-]izu(?:[_.#&-][^/]*)?)$")
        self.assertEqual(js, r"^cache/(?:[iI][zZ][uU][^/]*|[^/]*[_.#&-][iI][zZ][uU](?:[_.#&-][^/]*)?)$")
        self.assertEqual(netinstall_page.js_regex(r"(?i:\d[a-c])"), r"(?:\d[a-cA-C])")


@unittest.skipUnless(NODE, "needs node")
@mock.patch.object(gp, "CAP", CAP)
@mock.patch.object(gi, "log", lambda msg: None)
class InstallParityTests(unittest.TestCase):
    """The same scenario through gfs_install.py and through the page: the
    same containers with the same bytes."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def python(self, con: FakeConsole, title: str, files: dict, prefix: str | None = None, force: bool = False):
        src = tree(self.root / "py" / title, files)
        prefix = prefix or profiles.load(title).get("pack", {}).get("prefix") or profiles.default_prefix(title)
        info = gp.pack(src, title, prefix, self.root / "py-out" / title)
        index = (self.root / "py-out" / title / "index.bin").read_bytes()
        new = gi.parse_header(gc.index_name(prefix), index)
        index = gi.keep_installed_prefix(con, new, info, index)
        st = gi.survey(con, new, info["containers"], force)
        steps = gi.plan(st, new, info, force)
        gi.check_space(con, st, steps, new)
        gi.execute(con, steps, st, new, info, index, src)
        return steps

    def js(self, con: FakeConsole, title: str, files: dict, prefix: str | None = None, force: bool = False,
           fail_put: str | None = None) -> dict:
        req = {"op": "install", "cap": CAP, "title": title, "force": force, "info": {"name": "", "version": ""},
               "files": {rel: data.hex() for rel, data in files.items()},
               "console": {"saves": {d: {f: b.hex() for f, b in fs.items()} for d, fs in con.saves.items()},
                           "blocks": con.blocks, "capacity": con.capacity, "fail_put": fail_put}}
        if prefix:
            req["prefix"] = prefix
        return node(req)

    def both(self, setup, title: str, files: dict, **kw) -> tuple[dict, dict]:
        a, b = FakeConsole(), FakeConsole()
        setup(a)
        setup(b)
        try:
            self.python(a, title, files, **kw)
        except SystemExit as e:
            a.error = str(e)
        js = self.js(b, title, files, **kw)
        return a, js

    def assertSameResult(self, py: FakeConsole, js: dict) -> None:
        self.assertEqual(flat(js["saves"]), flat(py.saves))
        self.assertEqual({k: v for k, v in js["blocks"].items() if k in js["saves"]},
                         {k: v for k, v in py.blocks.items() if k in py.saves})

    def game(self, title: str, n: int = 1, extra: bytes = b"") -> dict:
        return {"eboot.bin": bytes([n]) * 100 + extra, "sce_sys/param.sfo": make_sfo({"TITLE_ID": title}),
                "data/a.bin": b"A" * (80 + n), "data/b.bin": b"B" * 70}

    def test_fresh_install(self) -> None:
        py, js = self.both(lambda c: None, "CUSA12345", self.game("CUSA12345"))
        self.assertSameResult(py, js)
        self.assertIn("GCUSA12345INDEX", js["saves"])
        cat = gc.parse(bytes.fromhex(js["saves"][gc.CATALOG][gc.CATALOG_FILE]))
        self.assertTrue(cat.find("CUSA12345").complete)

    def test_second_title_beside_the_first(self) -> None:
        def setup(c):
            self.python(c, "CUSA00001", self.game("CUSA00001"))
        py, js = self.both(setup, "CUSA00002", self.game("CUSA00002", 2))
        self.assertSameResult(py, js)
        self.assertEqual(len(gc.parse(bytes.fromhex(js["saves"][gc.CATALOG][gc.CATALOG_FILE])).entries), 2)

    def test_installed_sends_nothing(self) -> None:
        a = FakeConsole()
        self.python(a, "CUSA12345", self.game("CUSA12345"))
        js = self.js(a, "CUSA12345", self.game("CUSA12345"))
        self.assertEqual(js["steps"], [])
        self.assertEqual(js["ops"], [])

    def test_interrupted_install_carries_on(self) -> None:
        files = self.game("CUSA12345")
        con = FakeConsole()
        first = self.js(con, "CUSA12345", files, fail_put="GCUSA12345003")
        self.assertIn("connection lost", first["error"])
        cat = gc.parse(bytes.fromhex(first["saves"][gc.CATALOG][gc.CATALOG_FILE]))
        self.assertFalse(cat.find("CUSA12345").complete)
        con.saves = {d: {f: bytes.fromhex(h) for f, h in fs.items()} for d, fs in first["saves"].items()}
        con.blocks = first["blocks"]
        second = self.js(con, "CUSA12345", files)
        puts = [s[1] for s in second["steps"] if s[0] == "put"]
        self.assertEqual(puts[0], 3)               # 0..2 were tagged: not sent again
        ref = FakeConsole()                      # the same through gfs_install.py
        ref.fail_put = "GCUSA12345003"
        with self.assertRaises(SystemExit):
            self.python(ref, "CUSA12345", files)
        ref.fail_put = None
        self.python(ref, "CUSA12345", files)
        self.assertEqual(flat(second["saves"]), flat(ref.saves))

    def test_new_layout_replaces_only_its_containers(self) -> None:
        def setup(c):
            self.python(c, "CUSA12345", self.game("CUSA12345", extra=b"x" * 200))
            self.python(c, "CUSA00009", self.game("CUSA00009", 9))
        py, js = self.both(setup, "CUSA12345", self.game("CUSA12345", 3))
        self.assertSameResult(py, js)
        self.assertIn("GCUSA00009000", js["saves"])

    def test_ghost_v01_install_is_adopted(self) -> None:
        ghost = {"eboot.bin": b"G" * 90, "sce_sys/param.sfo": make_sfo({"TITLE_ID": "CUSA11456"}), "ui/x.swf": b"u" * 10}

        def setup(c):
            src = tree(self.root / "legacy" / "CUSA11456", ghost)
            info = gp.pack(src, "CUSA11456", "GHOST", self.root / "legacy-out")
            index = (self.root / "legacy-out/index.bin").read_bytes()
            from test_gfs_install import make_legacy
            make_legacy(c, info, index, "GFSINDEX", src)
        py, js = self.both(setup, "CUSA00003", self.game("CUSA00003", 3))
        self.assertSameResult(py, js)
        cat = gc.parse(bytes.fromhex(js["saves"][gc.CATALOG][gc.CATALOG_FILE]))
        self.assertTrue(cat.find("CUSA11456").legacy)

    def test_old_default_prefix_is_kept(self) -> None:
        def setup(c):
            self.python(c, "CUSA12345", self.game("CUSA12345"), prefix="G12345")
        py, js = self.both(setup, "CUSA12345", self.game("CUSA12345", 4))
        self.assertSameResult(py, js)
        self.assertIn("G12345INDEX", js["saves"])
        self.assertNotIn("GCUSA12345INDEX", js["saves"])

    def test_not_enough_space_changes_nothing(self) -> None:
        con = FakeConsole(capacity_blocks=1200)
        js = self.js(con, "CUSA12345", self.game("CUSA12345"))
        self.assertEqual(js["error"], "not enough space")
        self.assertEqual(js["saves"], {})

    def test_a_prefix_taken_by_another_title_is_refused(self) -> None:
        con = FakeConsole()
        self.python(con, "CUSA00001", self.game("CUSA00001"), prefix="GSAME")
        js = self.js(con, "CUSA00002", self.game("CUSA00002"), prefix="GSAME")
        self.assertIn("belongs to CUSA00001", js["error"])

    def test_remove_keeps_progress(self) -> None:
        con = FakeConsole()
        self.python(con, "CUSA12345", self.game("CUSA12345"))
        self.python(con, "CUSA00001", self.game("CUSA00001"))
        con.saves["GCUSA12345SAVE"] = {"sdmemory_0.bin": b"progress"}
        req = {"op": "remove", "cap": CAP, "title": "CUSA12345",
               "console": {"saves": {d: {f: b.hex() for f, b in fs.items()} for d, fs in con.saves.items()},
                           "blocks": con.blocks}}
        js = node(req)
        left = set(js["saves"])
        self.assertIn("GCUSA12345SAVE", left)
        self.assertFalse([d for d in left if d.startswith("GCUSA12345") and d != "GCUSA12345SAVE"])
        self.assertIn("GCUSA00001000", left)
        cat = gc.parse(bytes.fromhex(js["saves"][gc.CATALOG][gc.CATALOG_FILE]))
        self.assertEqual([e.title for e in cat.entries], ["CUSA00001"])


@unittest.skipUnless(NODE, "needs node")
class ZipTests(unittest.TestCase):
    def test_stored_and_deflated_entries_below_a_folder(self) -> None:
        files = {"eboot.bin": os.urandom(3000), "sce_sys/param.sfo": make_sfo({"TITLE_ID": "CUSA12345"}),
                 "data/big.bin": os.urandom(20000) + b"\0" * 50000, "data/text.txt": b"hello " * 900,
                 "data/empty.bin": b""}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            z = root / "game.zip"
            with zipfile.ZipFile(z, "w") as zf:
                for i, (rel, data) in enumerate(files.items()):
                    zf.writestr("CUSA12345-app/app0/" + rel, data,
                                zipfile.ZIP_DEFLATED if i % 2 else zipfile.ZIP_STORED)
                zf.writestr("CUSA12345-app/app0/emptydir/", b"")
                zf.writestr("__MACOSX/CUSA12345-app/app0/._eboot.bin", b"junk")
                zf.writestr("CUSA12345-app/app0/.DS_Store", b"junk")
                zf.writestr("CUSA12345-app/readme.txt", b"outside")
            src = tree(root / "app0", files)
            with mock.patch.object(gp, "CAP", 30000):
                info = gp.pack(src, "CUSA12345", "GCUSA12345", root / "out")
            js = node({"op": "zip", "zip": str(z), "cap": 30000, "title": "CUSA12345", "prefix": "GCUSA12345",
                       "step": 7777})
            self.assertEqual(js["files"], {rel: len(d) for rel, d in files.items()})
            self.assertEqual(js["dirs"], ["emptydir"])
            self.assertEqual((js["junk"], js["skipped"]), (2, 1))
            self.assertEqual(len(js["containers"]), len(info["containers"]))
            for c, ref in zip(js["containers"], info["containers"]):
                want = b"".join(gi.container_bytes(src, ref["pieces"]))
                self.assertEqual(bytes.fromhex(c["whole"]), want)
                self.assertEqual(bytes.fromhex(c["parts"]), want)
                self.assertEqual(bytes.fromhex(c["back"]), want[len(want) // 2:])


LUA = None
try:
    from test_manager import LUA, lua_value  # noqa: E402
except ImportError:
    pass
FAKE = HERE / "manager_fake.lua"
MANAGER = ROOT / "ghostldr/lua/manager.lua"


@unittest.skipUnless(NODE and LUA, "needs node and Lua 5.3/5.4")
@mock.patch.object(gp, "CAP", CAP)
@mock.patch.object(gi, "log", lambda msg: None)
class ManagerLiveTests(unittest.TestCase):
    """The page's own HTTP client and installer against manager.lua (in
    manager_fake.lua's live mode): what it leaves is what gfs_install.py
    leaves; a dropped upload carries on where the console got to."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def live(self, files: dict, title: str = "CUSA12345", containers: list | None = None, **kw) -> dict:
        out = self.root / f"out{len(list(self.root.iterdir()))}"
        out.mkdir()
        scen = out / "scenario.lua"
        scen.write_text("return " + lua_value({"live": True, "title": "CUSA03474", "containers": containers or []}))
        req = {"op": "live", "cap": CAP, "lua": LUA, "fake": str(FAKE), "manager": str(MANAGER), "scenario": str(scen),
               "outdir": str(out), "title": title, "files": {rel: data.hex() for rel, data in files.items()}}
        req.update(kw)
        return node(req)

    @staticmethod
    def saves(report: dict) -> dict:
        """The fake's containers as {name: {file: bytes}}, without the
        system's sce_sys files every new container gets."""
        return flat({n: {f: d for f, d in c["files"].items() if not f.startswith("sce_sys/")}
                     for n, c in report["containers"].items()})

    def reference(self, files: dict, title: str = "CUSA12345") -> dict:
        con = FakeConsole()
        InstallParityTests.python(self, con, title, files)
        return flat(con.saves)

    def game(self) -> dict:
        return {"eboot.bin": b"E" * 100, "sce_sys/param.sfo": make_sfo({"TITLE_ID": "CUSA12345"}),
                "data/a.bin": bytes(range(256)) * 2, "data/b.bin": b"B" * 70}

    def test_install_through_the_manager(self) -> None:
        r = self.live(self.game())
        self.assertNotIn("error", r, r.get("logs"))
        rep = r["report"]
        self.assertEqual(rep["violations"], [])
        self.assertEqual(rep["still_mounted"], [])
        self.assertEqual(self.saves(rep), self.reference(self.game()))
        self.assertEqual(r["info"]["server"], "manager")
        self.assertTrue(any("installed" in n for n in rep["notes"]))

    def test_a_dropped_upload_carries_on(self) -> None:
        r = self.live(self.game(), drops=[["GCUSA12345002", 20], ["GCUSA12345005", 0]])
        self.assertNotIn("error", r, r.get("logs"))
        self.assertEqual(self.saves(r["report"]), self.reference(self.game()))
        self.assertTrue(any("(dropped)" in q for q in r["requests"]))
        self.assertTrue(any("GCUSA12345002: connection lost, carrying on from 20" in m for m in r["logs"]), r["logs"])

    def test_already_installed_sends_nothing_and_remove_keeps_saves(self) -> None:
        first = self.live(self.game())
        conts = [{"name": n, "blocks": c["blocks"], "files": {f: bytes.fromhex(d) for f, d in c["files"].items()
                                                              if not f.startswith("sce_sys/")}}
                 for n, c in first["report"]["containers"].items()]
        conts.append({"name": "GCUSA12345SAVE", "files": {"sdmemory_0.bin": b"progress"}})
        conts.append({"name": "SLUS-20268", "files": {"VMC0.card": b"luac0re"}})
        r = self.live(self.game(), containers=conts, remove="CUSA12345")
        self.assertEqual(r["steps"], [])
        self.assertFalse([q for q in r["requests"] if "/data?" in q])
        left = set(r["report"]["containers"])
        self.assertIn("GCUSA12345SAVE", left)
        self.assertIn("SLUS-20268", left)
        self.assertFalse([n for n in left if n.startswith("GCUSA12345") and n != "GCUSA12345SAVE"])
        self.assertEqual(r["report"]["violations"], [])

    def test_the_install_api_refuses_saves_and_other_containers(self) -> None:
        from test_manager import request
        out = self.root / "refuse"
        out.mkdir()
        reqs = [request("POST", f"/api/install/{op}?c={c}&f=x.bin&blocks=512", b"abc", token="{{TOKEN}}")
                for op, c in (("put", "GCUSA12345SAVE"), ("delete", "SLUS-20268"), ("delete", "GLDRCACHE"),
                              ("write", "GFSCATALOG"), ("put", "GCUSA12345000"), ("delete", "GHOSTSAVE"))]
        scen = out / "scenario.lua"
        scen.write_text("return " + lua_value({"title": "CUSA03474", "requests": [{"data": r} for r in reqs],
                                                "containers": [{"name": "GLDRCACHE"}, {"name": "GHOSTSAVE"},
                                                               {"name": "SLUS-20268"}]}))
        subprocess.run([LUA, str(FAKE), str(MANAGER), str(scen), str(out)], check=True, timeout=60)
        codes = []
        for i in range(1, len(reqs) + 1):
            raw = (out / f"resp_{i:03d}.bin").read_bytes()
            codes.append(int(raw.split(b" ", 2)[1]))
        self.assertEqual(codes, [403, 403, 403, 200, 403, 403])
        rep = json.loads((out / "report.json").read_text())
        self.assertEqual(set(rep["containers"]), {"GLDRCACHE", "GHOSTSAVE", "SLUS-20268"})
        self.assertEqual(rep["violations"], [])


NATIVE = ROOT / "ghostldr/native"


def build_native(out: Path) -> None:
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc:
        raise unittest.SkipTest("no C compiler")
    obj = out.with_suffix(".o")
    # netinst.c as the selector module has it: freestanding, no libc headers
    subprocess.run([cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-ffreestanding", "-fno-builtin", f"-I{NATIVE / 'netinst'}", "-c",
                    str(NATIVE / "netinst/netinst.c"), "-o", str(obj)], check=True)
    subprocess.run([cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    f"-I{NATIVE / 'netinst'}", str(HERE / "ext_native/netinst_host.c"), str(obj), "-o", str(out)],
                   check=True)


class NativeServer:
    def __init__(self, exe: Path, root: Path, page: Path, capacity: int = 1 << 30) -> None:
        self.log = tempfile.TemporaryFile()
        self.root = root
        port = free_port()
        self.proc = subprocess.Popen([str(exe), str(root), str(port), str(page), str(capacity)], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.log, text=True)
        line = self.proc.stdout.readline().split()
        if len(line) != 3 or line[0] != "listening":
            raise RuntimeError(f"netinst_host: {line} {self.text()}")
        self.port, self.token = int(line[1]), line[2]
        self.base = f"http://127.0.0.1:{self.port}"

    def status(self) -> dict:
        self.proc.stdin.write("status\n")
        self.proc.stdin.flush()
        return json.loads(self.proc.stdout.readline())

    def stop(self, timeout: float = 30) -> float:
        import time
        t0 = time.time()
        if self.proc.poll() is None:
            self.proc.stdin.close()
            self.proc.wait(timeout)
        self.proc.stdout.close()
        self.log.close()
        return time.time() - t0

    def text(self) -> str:
        if self.log.closed:
            return ""
        self.log.seek(0)
        return self.log.read().decode(errors="replace")

    def saves(self) -> dict:
        out = {}
        for d in sorted((self.root / "saves").iterdir()):
            if d.is_dir():
                out[d.name] = {f.name: f.read_bytes() for f in d.iterdir() if f.is_file()}
        return flat(out)


def free_port() -> int:
    import socket
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def http(base: str, method: str, path: str, body: bytes = b"", token: str | None = None) -> tuple[int, dict, bytes]:
    import http.client
    from urllib.parse import urlsplit
    u = urlsplit(base)
    c = http.client.HTTPConnection(u.hostname, u.port, timeout=30)
    headers = {"X-Ghostldr-Token": token} if token else {}
    c.request(method, path, body=body if method == "POST" else None, headers=headers)
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, {k.lower(): v for k, v in r.getheaders()}, data


@unittest.skipUnless(shutil.which("clang") or shutil.which("cc"), "needs a C compiler")
@mock.patch.object(gp, "CAP", CAP)
@mock.patch.object(gi, "log", lambda msg: None)
class NativeServerTests(unittest.TestCase):
    """The selector's server (native/netinst/netinst.c) built for this
    machine: the page's own code installs through it, as through
    manager.lua, and leaves what gfs_install.py leaves."""

    @classmethod
    def setUpClass(cls) -> None:
        import gzip
        cls.tmp_cls = tempfile.TemporaryDirectory()
        d = Path(cls.tmp_cls.name)
        cls.exe = d / "netinst_host"
        build_native(cls.exe)
        cls.page = d / "page.gz"
        cls.page.write_bytes(gzip.compress(PAGE.read_bytes(), 9))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp_cls.cleanup()

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "root"
        self.root.mkdir()
        self.server: NativeServer | None = None

    def tearDown(self) -> None:
        if self.server and self.server.proc.poll() is None:
            self.server.stop()
        self.tmp.cleanup()

    def start(self, capacity: int = 1 << 30) -> NativeServer:
        self.server = NativeServer(self.exe, self.root, self.page, capacity)
        return self.server

    def game_dir(self, files: dict) -> Path:
        return tree(Path(self.tmp.name) / "game" / "app0", files)

    def game(self) -> dict:
        return {"eboot.bin": b"E" * 100, "sce_sys/param.sfo": make_sfo({"TITLE_ID": "CUSA12345", "TITLE": "Test"}),
                "data/a.bin": bytes(range(256)) * 2, "data/b.bin": b"B" * 70}

    def node_install(self, srv: NativeServer, files: dict, **kw) -> dict:
        req = {"op": "http", "cap": CAP, "base": srv.base, "token": srv.token, "dir": str(self.game_dir(files)),
               "quiet": True}
        req.update(kw)
        return node(req)

    @unittest.skipUnless(NODE, "needs node")
    def test_install_through_the_native_server(self) -> None:
        srv = self.start()
        r = self.node_install(srv, self.game())
        self.assertNotIn("error", r, srv.text())
        self.assertEqual(r["info"]["server"], "ghostldr")
        st = srv.status()
        self.assertEqual((st["state"], st["installed"], st["title"], st["name"]), (1, 1, "CUSA12345", "Test"))
        self.assertEqual(st["done"], st["total"])
        ref = FakeConsole()
        InstallParityTests.python(self, ref, "CUSA12345", self.game())
        self.assertEqual(srv.saves(), flat(ref.saves))
        changes = st["changes"]
        self.assertEqual(changes, 2)            # INCOMPLETE, then complete
        # again: nothing to send
        r2 = self.node_install(srv, self.game())
        self.assertEqual(r2["steps"], [])
        # a removal changes the list too (the selector rereads it)
        self.node_install(srv, self.game(), remove="CUSA12345")
        self.assertGreater(srv.status()["changes"], changes)
        self.assertLess(srv.stop(), 5)

    def test_page_and_info(self) -> None:
        import gzip
        srv = self.start()
        code, h, body = http(srv.base, "GET", "/")
        self.assertEqual((code, h["content-encoding"]), (200, "gzip"))
        self.assertEqual(gzip.decompress(body), PAGE.read_bytes())
        self.assertIn("frame-ancestors 'none'", h["content-security-policy"])
        code, _, body = http(srv.base, "GET", "/api/install/info")
        info = json.loads(body)
        self.assertEqual((info["server"], info["swrr"], info["platform"], info["token"]), ("ghostldr", "CUSA03474",
                                                                                        "ps5", True))
        self.assertTrue(srv.status()["url"].startswith("http://127.0.0.1:"))

    def test_the_token_and_what_may_be_touched(self) -> None:
        srv = self.start()
        (self.root / "saves/GHOSTSAVE").mkdir()
        (self.root / "saves/GLDRCACHE").mkdir()
        self.assertEqual(http(srv.base, "POST", "/api/install/auth")[0], 403)
        self.assertEqual(http(srv.base, "POST", "/api/install/auth", token="WRONG-TOKN")[0], 403)
        self.assertEqual(http(srv.base, "POST", "/api/install/auth", token=srv.token.lower())[0], 200)
        for path in ("/api/install/put?c=GHOSTSAVE&f=x.bin&blocks=512", "/api/install/delete?c=SLUS-20268",
                     "/api/install/delete?c=GLDRCACHE", "/api/install/put?c=GCUSA12345000&f=x&blocks=512",
                     "/api/install/data?c=GHOSTINDEX&size=3&blocks=512&off=0",
                     "/api/install/delete?c=" + "A" * 40 + "000"):
            self.assertEqual(http(srv.base, "POST", path, b"abc", token=srv.token)[0], 403, path)
        self.assertEqual(sorted(p.name for p in (self.root / "saves").iterdir()), ["GHOSTSAVE", "GLDRCACHE"])
        for _ in range(6):
            http(srv.base, "POST", "/api/install/auth", token="WRONG-TOKN")
        self.assertEqual(http(srv.base, "POST", "/api/install/auth", token=srv.token)[0], 429)

    def test_a_dropped_upload_carries_on(self) -> None:
        import socket
        srv = self.start()
        t = srv.token
        data = os.urandom(5000)
        q = f"/api/install/data?c=GTEST000&blocks=512&size={len(data)}&lid=00aa"
        s = socket.create_connection(("127.0.0.1", srv.port))
        s.sendall(f"POST {q}&off=0 HTTP/1.1\r\nX-Ghostldr-Token: {t}\r\nContent-Length: {len(data)}\r\n\r\n".encode()
                  + data[:1234])
        s.close()
        import time
        for _ in range(50):
            info = json.loads(http(srv.base, "GET", "/api/install/info")[2])
            if info.get("session", {}).get("written") == 1234:
                break
            time.sleep(0.1)
        self.assertEqual(info["session"], {"c": "GTEST000", "size": 5000, "written": 1234, "lid": "00aa"})
        code, _, body = http(srv.base, "POST", f"{q}&off=1000", data[1000:], token=t)
        self.assertEqual((code, json.loads(body)["written"]), (409, 1234))
        code, _, body = http(srv.base, "POST", f"{q}&off=1234", data[1234:], token=t)
        self.assertEqual(json.loads(body)["done"], True, body)
        self.assertEqual((self.root / "saves/GTEST000/data.bin").read_bytes(), data)

    def test_stop_in_the_middle_of_an_upload_is_prompt_and_commits(self) -> None:
        import socket
        import time
        srv = self.start()
        q = f"/api/install/data?c=GTEST000&blocks=512&size=9000000&off=0&lid=01"
        s = socket.create_connection(("127.0.0.1", srv.port))
        s.sendall(f"POST {q} HTTP/1.1\r\nX-Ghostldr-Token: {srv.token}\r\nContent-Length: 9000000\r\n\r\n".encode()
                  + b"x" * 5_000_000)
        for _ in range(50):
            if srv.status()["state"] == 2:
                break
            time.sleep(0.1)
        self.assertEqual(srv.status()["state"], 2)
        self.assertLess(srv.stop(), 5)          # the server's thread woke from its receive and ended
        s.close()
        self.assertFalse(list(self.root.glob("savedata*")))     # unmounted: committed
        self.assertTrue((self.root / "saves/GTEST000/data.bin").is_file())

    @unittest.skipUnless(NODE, "needs node")
    def test_not_enough_space_changes_nothing(self) -> None:
        srv = self.start(capacity=1500)
        r = self.node_install(srv, self.game())
        self.assertEqual(r["error"], "not enough space")
        self.assertEqual(srv.saves(), {})


class PageTests(unittest.TestCase):
    def test_generated_parts_are_up_to_date(self) -> None:
        out = subprocess.run([sys.executable, str(ROOT / "ghostldr/tools/netinstall_page.py"), "--check"],
                             capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)


if __name__ == "__main__":
    unittest.main()
