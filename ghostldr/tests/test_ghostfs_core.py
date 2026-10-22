"""The core's ghostfs (native/src/ghostfs.c) against save containers written
by the host tools: gfs_pack.py's index and data, gfs_catalog.py's catalogue.
Built for this machine (native_host/ghostfs_host.c) with a fake
sceSaveDataMount.

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
NATIVE = ROOT / "ghostldr/native"
sys.path.insert(0, str(ROOT / "ghostldr/host"))
sys.path.insert(0, str(HERE))
import gfs_catalog as gc  # noqa: E402
import gfs_install as gi  # noqa: E402
import gfs_pack as gp  # noqa: E402
from test_profiles import build_host  # noqa: E402


class GhostfsCoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.exe = cls.root / "ghostfs_host"
        build_host("ghostfs_host", [HERE / "native_host/ghostfs_host.c", NATIVE / "src/ghostfs.c"], cls.exe)
        cls.titles = {}
        with mock.patch.object(gp, "CAP", 64):
            for title, prefix, text in (("CUSA11111", "G11111", "first title's eboot " * 5),
                                        ("CUSA22222", "G22222", "second title's eboot " * 3)):
                src = cls.root / title / "app0"
                (src / "data").mkdir(parents=True)
                (src / "eboot.bin").write_text(text)
                (src / "data/a.bin").write_bytes(b"x" * 70)
                info = gp.pack(src, title, prefix, cls.root / title / "out")
                cls.titles[title] = (info, (cls.root / title / "out/index.bin").read_bytes(), text)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def setUp(self) -> None:
        self.files: dict[str, bytes] = {}

    def install(self, title: str, index_name: str | None = None, incomplete: bool = False) -> None:
        info, index, _ = self.titles[title]
        if incomplete:
            index = index[:60] + (1).to_bytes(4, "little") + index[64:]
        self.files[f"{index_name or gc.index_name(info['prefix'])}/index.bin"] = index
        for c in info["containers"]:
            src = self.root / title / "app0"
            self.files[f"{c['dir']}/data.bin"] = b"".join(b"".join(gi.container_bytes(src, [p])) for p in c["pieces"])

    def catalogue(self, *entries: tuple[str, int, str | None]) -> None:
        cat = gc.Catalogue()
        for title, flags, index in entries:
            info = self.titles[title][0]
            cat.put(gc.Entry(title, info["prefix"], index or gc.index_name(info["prefix"]), f"name of {title}", "01.00",
                             int(info["layout_id"], 16), info["total"], len(info["containers"]), flags))
        self.files[f"{gc.CATALOG}/{gc.CATALOG_FILE}"] = gc.build(cat)

    def boot(self, want: str = "-", select: bool = False, choice: str = "") -> list[str]:
        """choice: what the game selector picks when the core asks it ("": no selector);
        select: run.py --select."""
        args = []
        for i, (name, data) in enumerate(self.files.items()):
            path = self.root / f"f{i}.bin"
            path.write_bytes(data)
            args.append(f"{name}={path}")
        env = dict(os.environ, SEL_FLAG="1" if select else "0", SEL_CHOICE=choice)
        out = subprocess.run([str(self.exe), want, "/app0/eboot.bin", *args], capture_output=True, text=True,
                             timeout=60, env=env)
        self.assertEqual(out.returncode, 0, out.stderr)
        return out.stdout.splitlines()

    def assert_booted(self, lines: list[str], title: str) -> None:
        text = self.titles[title][2]
        init = [ln for ln in lines if ln.startswith("init ")]
        # the catalogue and the index are unmounted again before the title's data is read
        self.assertEqual(init, [f"init 0, selected {title}, titles {init[0].split('titles ')[1]}"], lines)
        self.assertTrue(init[0].endswith("still mounted -"), init)
        prefix = self.titles[title][0]["prefix"]
        self.assertEqual(lines[-1], f"read {len(text)}: {text}; save container {prefix}SAVE")
        self.assertIn(f"ghostfs: mounted {prefix}000 at /savedata1 (1 mounted, 1 mounts, 0 evictions)", lines)

    def test_legacy_install_without_a_catalogue(self) -> None:
        self.install("CUSA11111", "GFSINDEX")
        lines = self.boot()
        self.assertTrue([ln for ln in lines if ln.startswith("ghostfs: CUSA11111, ")], lines)
        self.assert_booted(lines, "CUSA11111")

    def test_legacy_install_is_not_another_title(self) -> None:
        self.install("CUSA11111", "GFSINDEX")
        lines = self.boot("CUSA22222")
        self.assertIn("ghostfs: GFSINDEX holds CUSA11111 (G11111000..), not CUSA22222", lines)
        self.assertTrue([ln for ln in lines if ln.startswith("init -1")], lines)

    def test_the_only_title_boots(self) -> None:
        self.install("CUSA22222")
        self.catalogue(("CUSA22222", 0, None))
        self.assert_booted(self.boot(), "CUSA22222")

    def test_several_titles_need_a_choice(self) -> None:
        self.install("CUSA11111")
        self.install("CUSA22222")
        self.catalogue(("CUSA11111", 0, None), ("CUSA22222", 0, None))
        lines = self.boot()                                       # the selector isn't there
        self.assertIn("ghostfs: installed: CUSA11111 (name of CUSA11111), CUSA22222 (name of CUSA22222)", lines)
        self.assertIn("deferred to the game selector; it chose nothing", lines)
        self.assertIn("ghostfs: several titles: pick one with run.py boot --title TITLE", lines)
        self.assertTrue([ln for ln in lines if ln.startswith("init -1")], lines)
        lines = self.boot(choice="CUSA11111")                      # the selector picks
        self.assertIn("deferred to the game selector; it chose CUSA11111", lines)
        self.assert_booted(lines, "CUSA11111")
        self.assert_booted(self.boot("CUSA22222"), "CUSA22222")     # --title: no selector
        self.assert_booted(self.boot("CUSA11111"), "CUSA11111")

    def test_select_shows_the_selector_anyway(self) -> None:
        self.install("CUSA11111")
        self.install("CUSA22222")
        self.catalogue(("CUSA11111", 0, None), ("CUSA22222", gc.INCOMPLETE, None))
        lines = self.boot()                                       # one complete title: no selector
        self.assertNotIn("deferred", " ".join(lines))
        self.assert_booted(lines, "CUSA11111")
        lines = self.boot(select=True, choice="CUSA11111")         # run.py --select
        self.assertIn("deferred to the game selector; it chose CUSA11111", lines)
        self.assert_booted(lines, "CUSA11111")
        lines = self.boot("CUSA11111", select=True)                # --select wins over --title; no selector served
        self.assertIn("deferred to the game selector; it chose nothing", lines)
        self.assert_booted(lines, "CUSA11111")                     # falls back to --title

    def test_incomplete_titles_never_boot(self) -> None:
        self.install("CUSA11111")
        self.install("CUSA22222")
        self.catalogue(("CUSA11111", gc.INCOMPLETE, None), ("CUSA22222", 0, None))
        self.assert_booted(self.boot(), "CUSA22222")           # the only complete one
        lines = self.boot("CUSA11111")
        self.assertIn("ghostfs: CUSA11111 is not completely installed: rerun gfs_install.py", lines)
        self.install("CUSA22222", incomplete=True)               # the index says so too
        lines = self.boot("CUSA22222")
        self.assertIn("ghostfs: the install of CUSA22222 is incomplete: rerun gfs_install.py", lines)

    def test_catalogue_entry_for_a_legacy_index(self) -> None:
        self.install("CUSA11111", "GFSINDEX")
        self.install("CUSA22222")
        self.catalogue(("CUSA11111", gc.LEGACY_INDEX, "GFSINDEX"), ("CUSA22222", 0, None))
        self.assert_booted(self.boot("CUSA11111"), "CUSA11111")

    def test_catalogued_title_without_its_index(self) -> None:
        self.install("CUSA22222")
        del self.files["G22222INDEX/index.bin"]
        self.catalogue(("CUSA22222", 0, None))
        lines = self.boot()
        self.assertIn("ghostfs: CUSA22222: no index G22222INDEX (809f0008): reinstall it, or gfs_install.py --remove "
                      "CUSA22222", lines)
        self.assertTrue([ln for ln in lines if ln.startswith("init -")], lines)

    def test_unknown_title_and_damaged_catalogue(self) -> None:
        self.install("CUSA11111", "GFSINDEX")
        self.catalogue(("CUSA11111", gc.LEGACY_INDEX, "GFSINDEX"))
        self.assertIn("ghostfs: CUSA99999 is not installed", self.boot("CUSA99999"))
        self.files[f"{gc.CATALOG}/{gc.CATALOG_FILE}"] = b"GFSC" + b"\xff" * 60
        lines = self.boot()
        self.assertTrue([ln for ln in lines if "damaged catalogue" in ln], lines)
        self.assert_booted(lines, "CUSA11111")                  # back to the legacy index


if __name__ == "__main__":
    unittest.main()
