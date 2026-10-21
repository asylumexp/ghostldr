"""Packing a title: the generic order without an open-order file, a captured
order, homebrew dumps (no param.sfo, an odd one, no sce_module/), and
capture_order.py reading a boot log.

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
import gfs_pack as gp  # noqa: E402
import profiles  # noqa: E402
from test_profiles import make_sfo  # noqa: E402


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


if __name__ == "__main__":
    unittest.main()
