"""Packing a title: the generic order without an open-order file, a captured
order, homebrew dumps (no param.sfo, an odd one, no sce_module/), and
capture_order.py reading a boot log.  The game selector's name and art
(gfs_meta.py): QOI and PNG, which source wins, the store lookup (faked: no
network), meta.bin.

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[0] / "host"))
sys.path.insert(0, str(HERE))
import capture_order as co  # noqa: E402
import gfs_meta as gm  # noqa: E402
import gfs_pack as gp  # noqa: E402
import profiles  # noqa: E402
from test_profiles import make_sfo, orbis_eboot  # noqa: E402


def tree(root: Path, files: dict[str, int | bytes]) -> Path:
    for rel, data in files.items():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_bytes(data if isinstance(data, bytes) else b"\0" * data)
    return root


def order_of(info: dict) -> list[str]:
    seen, out = set(), []
    for c in info["containers"]:
        for rel, _, _ in c["pieces"]:
            if rel not in seen:
                seen.add(rel)
                out.append(rel)
    return out


class HeuristicOrderTests(unittest.TestCase):
    FILES = {"movies/intro.mp4": 900_000, "data/levels/b.pak": 500_000, "data/levels/a.pak": 400_000,
             "data/small.cfg": 300, "sce_sys/icon0.png": 70_000, "sce_module/libc.prx": 200_000,
             "sce_module/libSceFios2.prx": 100_000, "eboot.bin": 2_000_000, "readme.txt": 10,
             "audio/music.at9": 600_000}

    def test_boot_files_first_then_directory_by_directory(self) -> None:
        files = {rel: n for rel, n in self.FILES.items()}
        order, traced = gp.file_order(files, {})
        self.assertEqual(traced, 0)
        self.assertEqual(order, [
            "eboot.bin", "sce_module/libSceFios2.prx", "sce_module/libc.prx", "sce_sys/icon0.png",
            "readme.txt", "data/small.cfg",                                 # small files, by directory
            "audio/music.at9", "data/levels/a.pak", "data/levels/b.pak", "movies/intro.mp4"])

    def test_small_file_limit_halves_until_they_fit(self) -> None:
        many = {f"f/{i:05d}.bin": 60_000 for i in range(6000)}          # 343 MiB of 60 KB files
        self.assertLess(gp.small_limit(many), 60_000)
        self.assertEqual(gp.small_limit({"a": 1, "b": 64 << 10}), 64 << 10)

    def test_open_order_file_comes_after_the_boot_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            order = Path(tmp) / "order.txt"
            order.write_text("# captured\nmovies/intro.mp4\ndata/levels/b.pak\nmissing/file\nmovies/intro.mp4\n")
            files = dict(self.FILES)
            got, traced = gp.file_order(files, {}, order)
        self.assertEqual(traced, 2)
        self.assertEqual(got[:7], ["eboot.bin", "sce_module/libSceFios2.prx", "sce_module/libc.prx",
                                   "sce_sys/icon0.png", "readme.txt", "data/small.cfg", "movies/intro.mp4"])
        self.assertEqual(got[7], "data/levels/b.pak")
        self.assertEqual(sorted(got), sorted(files))

    def test_profile_groups_and_late_files(self) -> None:
        pack = {"groups": [["^(sce_|eboot)", 0], ["^audio/", 5], ["", 9]], "late": ["audio/"]}
        with tempfile.TemporaryDirectory() as tmp:
            order = Path(tmp) / "o.txt"
            order.write_text("audio/music.at9\ndata/levels/a.pak\n")
            got, _ = gp.file_order(dict(self.FILES), pack, order)
        self.assertEqual(got[:4], ["eboot.bin", "sce_module/libSceFios2.prx", "sce_module/libc.prx",
                                   "sce_sys/icon0.png"])
        self.assertEqual(got[4:6], ["data/levels/a.pak", "audio/music.at9"])   # traced, late ones last


@mock.patch.object(gp, "CAP", 1 << 20)
class HomebrewPackTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def pack_main(self, src: Path, *args: str) -> Path:
        out = self.root / "packs"
        with mock.patch.object(gp, "OUT_ROOT", out), mock.patch.object(sys, "argv", ["gfs_pack.py", "--src",
                                                                                     str(src), *args]), \
                mock.patch("builtins.print"):
            gp.main()
        return out

    def test_no_param_sfo_needs_a_title(self) -> None:
        src = tree(self.root / "hb", {"eboot.bin": b"\x7fELF" + b"\0" * 100, "assets/a.bin": 3000})
        with self.assertRaises(SystemExit) as cm:
            self.pack_main(src)
        self.assertIn("pass --title", str(cm.exception))
        out = self.pack_main(src, "--title", "hbtest001")
        info = json.loads((out / "HBTEST001/layout.json").read_text())
        self.assertEqual((info["title"], info["prefix"]), ("HBTEST001", "GHBTEST001"))
        self.assertEqual(order_of(info), ["eboot.bin", "assets/a.bin"])
        self.assertEqual(info["app_params"], [3, 0, 0, 0, 0])

    def test_odd_param_sfo_falls_back_to_the_title_option(self) -> None:
        src = tree(self.root / "odd", {"eboot.bin": 50, "sce_sys/param.sfo": b"\0PSF\x01\x01"})
        with self.assertRaises(SystemExit):
            self.pack_main(src)
        out = self.pack_main(src, "--title", "BREW00007", "--prefix", "brew7")
        self.assertTrue((out / "BREW00007/index.bin").exists())

    def test_param_sfo_names_the_title(self) -> None:
        src = tree(self.root / "ok", {"eboot.bin": 50, "sce_sys/param.sfo": make_sfo(
            {"TITLE_ID": "BREW00042", "TITLE": "Some Homebrew", "APP_VER": "01.02", "USER_DEFINED_PARAM_1": 5})})
        out = self.pack_main(src)
        info = json.loads((out / "BREW00042/layout.json").read_text())
        self.assertEqual((info["prefix"], info["name"], info["version"]), ("G00042", "Some Homebrew", "01.02"))
        self.assertEqual(info["app_params"], [3, 5, 0, 0, 0])

    def test_openorbis_homebrew_without_param_sfo(self) -> None:
        # OpenOrbis homebrew: the title ID and display name come from the module name
        src = tree(self.root / "oo", {"eboot.bin": orbis_eboot("hello_world"), "assets/fonts/a.ttf": 5000,
                                      "sce_module/libc.prx": 3000})
        out = self.pack_main(src)
        info = json.loads((out / "HELLO_WORLD/layout.json").read_text())
        self.assertEqual((info["title"], info["prefix"], info["name"]), ("HELLO_WORLD", "GHELLOWORLD", "hello_world"))
        self.assertEqual(order_of(info), ["eboot.bin", "sce_module/libc.prx", "assets/fonts/a.ttf"])
        self.assertEqual([c["blocks"] for c in info["containers"]], [gp.MIN_BLOCKS])   # 16 MiB, not 1 GiB

    def test_container_sizes(self) -> None:
        self.assertEqual(gp.data_blocks(0), gp.MIN_BLOCKS)
        self.assertEqual(gp.data_blocks(992 << 20), gp.DATA_BLOCKS)                   # full: 1 GiB
        self.assertEqual(gp.data_blocks(100 << 20), 3200 + 48 + 200)                   # data + 1.5 MiB + 1/16
        src = tree(self.root / "big", {"eboot.bin": 1 << 20, "data/a.bin": 300 << 10})  # CAP is 1 MiB here
        info = gp.pack(src, "BREW00009", "G00009", self.root / "big-out", profiles.load(None))
        self.assertEqual([(c["size"], c["blocks"]) for c in info["containers"]],
                         [(1 << 20, gp.MIN_BLOCKS), (300 << 10, gp.MIN_BLOCKS)])

    def test_bad_prefix_is_refused(self) -> None:
        src = tree(self.root / "p", {"eboot.bin": 50})
        with self.assertRaises(SystemExit) as cm:
            self.pack_main(src, "--title", "BREW00001", "--prefix", "GFSBAD")
        self.assertIn("GFS", str(cm.exception))

    def test_no_eboot_is_refused(self) -> None:
        src = tree(self.root / "e", {"data/x": 5})
        with self.assertRaises(SystemExit) as cm:
            self.pack_main(src, "--title", "BREW00001")
        self.assertIn("no eboot.bin", str(cm.exception))


class CaptureOrderTests(unittest.TestCase):
    LOG = """\
10:00:00 [ghostldr 5] ghostldr core at 0x2111c0000, ctx flags 0
10:00:01 [ghostldr 5] ghostfs: CUSA22222, 12 entries, 4 MiB in 1 containers of 992 MiB (G22222000..), user 1
10:00:01 [ghostldr 5] gfs open /app0/sce_module/libc.prx (100 bytes @ 0) -> 29
10:00:01 [ghostldr 5] gfs open /app0/eboot.bin (100 bytes @ 0) -> 29
10:00:02 [ghostldr 5] gfs open /app0/data/first.bin (10 bytes @ 0) -> 29
10:00:03 [ghostldr 5] gfs open /app0/data//second.bin (10 bytes @ 0) -> 2a
10:01:00 [ghostldr 6] ghostldr core at 0x2111c0000, ctx flags 0
10:01:01 [ghostldr 6] profile: CUSA33333 generic, eboot -: memory plan-free
10:01:02 [ghostldr 6] open /app0/other.bin (/data/x/other.bin) flags 0 -> 1f
10:02:00 [ghostldr 7] ghostldr core at 0x2111c0000, ctx flags 0
10:02:01 [ghostldr 7] ghostfs: CUSA22222, 12 entries, 4 MiB in 1 containers of 992 MiB (G22222000..), user 1
10:02:02 [ghostldr 7] gfs open /app0/data/third.bin (10 bytes @ 0) -> 29
10:02:02 [ghostldr 7] gfs open /app0/data/first.bin (10 bytes @ 0) -> 29
10:02:03 [ghostldr 7] open /app0/cache/t00000.dat (/data/x/t.dat) flags 0 -> 80020002
"""

    def test_boots_of_one_title_merge_in_first_open_order(self) -> None:
        boots = co.boots(self.LOG.splitlines())
        self.assertEqual([b["title"] for b in boots], ["CUSA22222", "CUSA33333", "CUSA22222"])
        mine = [b for b in boots if b["title"] == "CUSA22222"]
        self.assertEqual(co.merge(mine), ["data/first.bin", "data/second.bin", "data/third.bin"])
        self.assertEqual(co.merge([boots[1]]), ["other.bin"])      # failed opens are not files


def picture(w: int, h: int, seed: int = 1, alpha: bool = False) -> gm.Image:
    """Gradients, flat runs and noise: every QOI op gets used."""
    px = bytearray()
    for y in range(h):
        for x in range(w):
            if x < w // 4:
                px += bytes((10, 20, 30, 255))
            elif x < w // 2:
                px += bytes(((x + seed) & 255, (y * 2) & 255, (x + y) & 255, 255))
            else:
                v = (x * 7919 + y * 104729 + seed * 31) & 0xFFFFFFFF
                px += bytes((v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >> 24) | 128 if alpha else 255))
    return gm.Image(w, h, bytes(px))


def png(img: gm.Image) -> bytes:
    with mock.patch.object(gm, "PIL", None):
        return gm.png_encode(img)


class MetaTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_qoi_round_trip(self) -> None:
        for alpha in (False, True):
            img = picture(97, 41, alpha=alpha)
            data = gm.qoi_encode(img)
            self.assertEqual(data[:4], b"qoif")
            self.assertEqual(data[-8:], b"\0" * 7 + b"\1")
            self.assertEqual(gm.qoi_decode(data).rgba, img.rgba)
        flat = gm.Image(300, 200, bytes((1, 2, 3, 255)) * 60000)
        self.assertLess(len(gm.qoi_encode(flat)), 1100)        # runs of 62

    def test_png_without_pillow(self) -> None:
        img = picture(33, 17, alpha=True)
        data = png(img)
        with mock.patch.object(gm, "PIL", None):
            self.assertEqual(gm.decode_image(data).rgba, img.rgba)
            small = gm.cover(gm.Image(64, 32, bytes((200, 100, 50, 255)) * 2048), 16, 16)
        self.assertEqual((small.w, small.h, small.rgba[:4]), (16, 16, bytes((200, 100, 50, 255))))
        if gm.PIL:
            self.assertEqual(gm.decode_image(data).rgba, img.rgba)
        with self.assertRaises(gm.ArtError), mock.patch.object(gm, "PIL", None), \
                mock.patch.object(gm.shutil, "which", lambda name: None):
            gm.decode_image(b"\xff\xd8\xff\xe0 a JPEG")

    def test_sources_best_first(self) -> None:
        sfo = make_sfo({"TITLE": "Own Name", "TITLE_ID": "CUSA12345"})
        given = self.root / "given.png"
        given.write_bytes(png(picture(40, 40, 2)))
        own = {"param.sfo": sfo, "icon0.png": png(picture(64, 64, 3)), "pic0.png": png(picture(64, 36, 4))}
        m = gm.choose("CUSA12345", own, True, "Profile Name", "Given", icon=given, log=lambda s: None)
        self.assertEqual((m.name, m.name_source, m.sfo), ("Own Name", gm.NAME_SFO, sfo))
        self.assertEqual(m.version, "")                      # no APP_VER, no profile version
        self.assertEqual((m.icon_source, m.backdrop_source), (gm.ART_TITLE, gm.ART_TITLE))   # pic0 when no pic1
        self.assertTrue(any("--icon is not used" in n for n in m.notes))
        self.assertTrue(any("--name is not used" in n for n in m.notes))
        m = gm.choose("CUSA12345", {}, True, "Profile Name", "Given", backdrop=given, log=lambda s: None)
        self.assertEqual((m.name, m.name_source, m.icon, m.backdrop_source), ("Profile Name", gm.NAME_PROFILE, None,
                                                                              gm.ART_GIVEN))
        m = gm.choose("CUSA12345", {}, False, "", "Given", log=lambda s: None)
        self.assertEqual((m.name, m.name_source, m.scanned), ("Given", gm.NAME_GIVEN, False))
        with self.assertRaises(gm.ArtError):
            gm.choose("CUSA12345", {}, True, icon=self.root / "missing.png", log=lambda s: None)

    def fake_store(self, calls: list):
        icon, back = png(picture(80, 80, 5)), png(picture(160, 90, 6))
        container = {"name": "Product Edition", "title_name": "Store Name",
                     "images": [{"type": 10, "url": "https://img/icon.png"}, {"type": 12, "url": "https://img/bg.png"}]}

        def get(url: str):
            calls.append(url)
            if "titlecontainer/US/en/999/CUSA12345_00" in url:
                return __import__("json").dumps(container).encode()
            return {"https://img/icon.png": icon, "https://img/bg.png": back}.get(url)
        return get

    def test_fetch_art_asks_the_store_once(self) -> None:
        calls: list = []
        art = self.root / "art"
        m = gm.choose("CUSA12345", {}, True, "", None, fetch=True, art_dir=art, get=self.fake_store(calls),
                      log=lambda s: None)
        self.assertEqual((m.name, m.name_source), ("Store Name", gm.NAME_STORE))
        self.assertEqual((m.icon_source, m.backdrop_source, m.icon.w, m.backdrop.w), (gm.ART_STORE, gm.ART_STORE,
                                                                                      80, 160))
        self.assertEqual(len(calls), 3)                    # the title container, then both pictures
        self.assertTrue(all("hmac" not in c.lower() and "tmdb" not in c for c in calls))
        self.assertEqual(sorted(p.name for p in art.iterdir()), ["backdrop.png", "icon.png", "store.json"])
        m2 = gm.choose("CUSA12345", {}, True, "Profile", None, fetch=True, art_dir=art, get=self.fake_store(calls),
                       log=lambda s: None)
        self.assertEqual(len(calls), 3)                    # reused from the art directory
        self.assertEqual((m2.name, m2.icon.rgba), ("Profile", m.icon.rgba))
        own = {"icon0.png": png(picture(8, 8)), "pic1.png": png(picture(16, 9)), "param.sfo": make_sfo({"TITLE": "X"})}
        m3 = gm.choose("CUSA12345", own, True, fetch=True, art_dir=self.root / "unused", get=self.fake_store(calls),
                       log=lambda s: None)
        self.assertEqual(len(calls), 3)                    # nothing missing: the store is not asked
        self.assertFalse((self.root / "unused").exists())
        self.assertEqual(m3.icon_source, gm.ART_TITLE)

    def test_meta_bin(self) -> None:
        sfo = make_sfo({"TITLE": "Own", "TITLE_01": "Own (EN)"})
        m = gm.Meta("CUSA12345", "Own", gm.NAME_SFO, sfo, "01.07", picture(100, 100), gm.ART_TITLE,
                    picture(640, 480), gm.ART_STORE)
        data = gm.build(m, now=1234)
        info = gm.parse(data)
        self.assertEqual(info["version"], "01.07")
        self.assertEqual(data[:4], b"GFSM")
        self.assertEqual((info["title"], info["name"], info["name_source"], info["flags"], info["written"],
                          info["size"]), ("CUSA12345", "Own", gm.NAME_SFO, gm.FLAG_SCANNED, 1234, len(data)))
        sfo_s, icon_s, bd_s = info["sections"]
        self.assertEqual(data[sfo_s["offset"]:sfo_s["offset"] + sfo_s["size"]], sfo)
        self.assertEqual((icon_s["w"], icon_s["h"], icon_s["format"], icon_s["source"]),
                         (gm.ICON, gm.ICON, gm.FMT_QOI, gm.ART_TITLE))
        self.assertEqual((bd_s["w"], bd_s["h"], bd_s["source"]), (*gm.BACKDROP, gm.ART_STORE))
        bd = gm.qoi_decode(data[bd_s["offset"]:bd_s["offset"] + bd_s["size"]])
        self.assertEqual((bd.w, bd.h), gm.BACKDROP)
        self.assertTrue(all(o % 16 == 0 for o in (sfo_s["offset"], icon_s["offset"], bd_s["offset"])))
        m.name = "Other"
        self.assertNotEqual(gm.parse(gm.build(m))["id"], info["id"])     # the id covers the name
        self.assertIsNone(gm.parse(b"GFSX" + data[4:]))
        self.assertGreaterEqual(gm.container_blocks(len(data)) * 32768, 2 * len(data))

    def test_pack_writes_meta_only_when_there_is_something_to_show(self) -> None:
        src = tree(self.root / "app0", {"eboot.bin": b"e" * 50, "sce_sys/icon0.png": png(picture(32, 32)),
                                        "sce_sys/param.sfo": make_sfo({"TITLE": "Packed", "TITLE_ID": "CUSA12345"})})
        out = self.root / "out"
        info = gp.pack(src, "CUSA12345", "G12345", out)
        meta = gm.write_pack_meta(info, out, profiles.load("CUSA12345"), log=lambda s: None)
        head = gm.parse((out / "meta.bin").read_bytes())
        self.assertEqual((meta["id"], head["name"], info["name"]), (head["id"], "Packed", "Packed"))
        self.assertEqual(json.loads((out / "layout.json").read_text())["meta"]["file"], str(out / "meta.bin"))
        bare = tree(self.root / "bare", {"eboot.bin": b"e" * 50})
        info = gp.pack(bare, "CUSA12345", "G12345", out)
        self.assertIsNone(gm.write_pack_meta(info, out, profiles.load("CUSA12345"), log=lambda s: None))
        self.assertFalse((out / "meta.bin").exists())
        self.assertNotIn("meta", json.loads((out / "layout.json").read_text()))


if __name__ == "__main__":
    unittest.main()
