"""Drawn dialogs (ext/src/dlg_overlay.c) of the built ext.bin, run as x86-64
code (ext_host/overlay.c; Rosetta on Apple silicon): a MsgDialog and an
ImeDialog drawn over a title's display buffers and answered with a fake
controller, the title's pad masked meanwhile, and the answers as before
where nothing can be drawn on.  Needs `make -C ghostldr/native` first.
The frames with the dialogs on them are kept as build/ghostldr/overlay-*.png
when Pillow is installed, to look at.

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
EXT = ROOT / "build/ghostldr/ext.bin"
sys.path.insert(0, str(HERE))
from test_ext import x86_64_cc  # noqa: E402


@unittest.skipUnless(EXT.exists(), "build/ghostldr/ext.bin missing: run make -C ghostldr/native")
class DrawnDialogsOnHost(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = x86_64_cc()
        if not cc:
            raise unittest.SkipTest("no way to build and run x86-64 code here")
        blob = EXT.read_bytes()
        abi, = struct.unpack_from("<I", blob, 20)
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = Path(cls.tmp.name)
        flags = ["-O1", "-std=gnu11", "-fno-builtin", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror",
                 f"-I{ROOT / 'ghostldr/native/include'}", f"-DGHOST_ABI={abi}u"]
        core = ROOT / "ghostldr/native/src"
        objs = []
        for src, extra in [(core / "ext.c", flags), (core / "sha256.c", flags), (core / "nid.c", flags),
                           (HERE / "ext_host/overlay.c", flags), (HERE / "ext_host/sys.c", ["-O1"]),
                           (HERE / "ext_host/overlay_sys.c", ["-O1"])]:
            obj = tmp / (src.stem + ".o")
            subprocess.run(cc + extra + ["-c", str(src), "-o", str(obj)], check=True)
            objs.append(str(obj))
        host = tmp / "overlay_host"
        subprocess.run(cc + objs + ["-o", str(host)], check=True)
        p = subprocess.run([str(host), str(EXT), hex(zlib.crc32(blob)), str(tmp)], capture_output=True, text=True,
                           timeout=300)
        cls.returncode, cls.out = p.returncode, p.stdout + p.stderr
        try:
            from PIL import Image
            for name in ("msg", "ime", "banner", "banner-sticky", "banner-modal"):
                ppm = tmp / f"{name}.ppm"
                if ppm.exists():
                    Image.open(ppm).save(ROOT / f"build/ghostldr/overlay-{name}.png")
        except ImportError:
            pass

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def assert_group_ok(self, group: str, at_least: int) -> None:
        lines = [l for l in self.out.splitlines() if l.startswith("overlay_host: ") and f" [{group}] " in l]
        self.assertGreaterEqual(len(lines), at_least, f"[{group}]: {len(lines)} checks ran\n" + self.out[-3000:])
        bad = [l for l in lines if l.startswith("overlay_host: FAILED")]
        self.assertFalse(bad, "\n".join(bad))

    def test_every_check_passes(self) -> None:
        self.assertEqual(self.returncode, 0, self.out[-6000:])
        m = re.search(r"overlay_host: (\d+) checks, all passed", self.out)
        self.assertTrue(m, self.out[-3000:])

    def test_message_dialogs_drawn_and_answered(self) -> None:
        self.assert_group_ok("msg", 25)

    def test_answered_at_once_where_nothing_is_drawn(self) -> None:
        self.assert_group_ok("auto", 5)

    def test_banner(self) -> None:
        self.assert_group_ok("banner", 6)

    def test_keyboard(self) -> None:
        self.assert_group_ok("ime", 12)
