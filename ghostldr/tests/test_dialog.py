"""The common dialog families (ext/src/hle_dialog*.c) of the built ext.bin,
run as x86-64 code (ext_host/dialog.c; Rosetta on Apple silicon) through the
core's real extension loader: the shared state machine, the "press Cross at
once" answers and their overrides, param magic and size checks, the stall
guard for Ghost's save dialog helper, the save list, the IME text policy and
the sysmodule short-circuit.  Needs `make -C ghostldr/native` first.

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
class DialogsOnHost(unittest.TestCase):
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
        for src, extra in [(core / "ext.c", flags), (core / "sha256.c", flags), (core / "nid.c", flags), (HERE / "ext_host/dialog.c", flags),
                           (HERE / "ext_host/sys.c", ["-O1"])]:
            obj = tmp / (src.stem + ".o")
            subprocess.run(cc + extra + ["-c", str(src), "-o", str(obj)], check=True)
            objs.append(str(obj))
        host = tmp / "dialog_host"
        subprocess.run(cc + objs + ["-o", str(host)], check=True)
        p = subprocess.run([str(host), str(EXT), hex(zlib.crc32(blob))], capture_output=True, text=True, timeout=120)
        cls.returncode, cls.out = p.returncode, p.stdout + p.stderr

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def assert_group_ok(self, group: str, at_least: int) -> None:
        """Every check of a group (the [group] tag dialog.c prints) passed."""
        lines = [l for l in self.out.splitlines() if l.startswith("dialog_host: ") and f" [{group}] " in l]
        self.assertGreaterEqual(len(lines), at_least, f"[{group}]: {len(lines)} checks ran\n" + self.out[-3000:])
        bad = [l for l in lines if l.startswith("dialog_host: FAILED")]
        self.assertFalse(bad, "\n".join(bad))

    def test_every_check_passes(self) -> None:
        self.assertEqual(self.returncode, 0, self.out[-6000:])
        self.assertIn("dialog_host: ok [load] the extension loads", self.out)
        m = re.search(r"dialog_host: (\d+) checks, all passed", self.out)
        self.assertTrue(m, self.out[-3000:])
        self.assertGreater(int(m.group(1)), 150)

    def test_state_machine(self) -> None:
        self.assert_group_ok("state", 20)

    def test_answer_policy_and_overrides(self) -> None:
        self.assert_group_ok("answers", 15)

    def test_param_magic_and_size_checks(self) -> None:
        self.assert_group_ok("params", 15)

    def test_online_dialogs_cancel(self) -> None:
        self.assert_group_ok("online", 20)

    def test_ghost_save_dialog_helper_never_hangs(self) -> None:
        self.assert_group_ok("stall", 6)

    def test_save_list_and_answers(self) -> None:
        self.assert_group_ok("savedata", 25)

    def test_ime_policy(self) -> None:
        self.assert_group_ok("ime", 45)

    def test_sysmodule_short_circuit(self) -> None:
        self.assert_group_ok("sysmodule", 7)

if __name__ == "__main__":
    unittest.main()
