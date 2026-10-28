"""The watchdog's reads of Ghost of Tsushima 2.18's globals (profile flag
PF_DIAG), moved from the core's crash.c into the extension
(ext/src/ghost_watch.c) to keep the core image small: built for this
machine against a fake eboot (ext_native/watch_host.c), the three phases
print what the core printed, line for line.

    python3 -m unittest test_ghost_watch        (from ghostldr/tests)
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
NATIVE = HERE.parent / "native"
sys.path.insert(0, str(HERE))
from test_profiles import host_cc  # noqa: E402


class GhostWatch(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = host_cc()
        if not cc:
            raise unittest.SkipTest("no C compiler whose programs run on this machine")
        cls.tmp = tempfile.TemporaryDirectory()
        exe = Path(cls.tmp.name) / "watch_host"
        subprocess.run(cc + ["-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                             "-ffreestanding", "-fno-builtin", f"-I{NATIVE / 'ext/include'}",
                             f"-I{NATIVE / 'include'}", "-DGHOST_ABI=0u", str(HERE / "ext_native/watch_host.c"),
                             str(NATIVE / "ext/src/ghost_watch.c"), "-o", str(exe)], check=True)
        out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        cls.out, cls.rc = out.stdout, out.returncode
        cls.log = [ln[5:] for ln in out.stdout.splitlines() if ln.startswith("log: ")]

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_checks_pass(self) -> None:
        self.assertEqual(self.rc, 0, self.out)
        self.assertIn("all checks passed", self.out)

    def test_the_lines_the_core_printed(self) -> None:
        """crash.c's format strings, unchanged: each phase's lines in the core's order"""
        log = [ln for ln in self.log if not ln.startswith("watchdog:   res")]
        self.assertEqual(log[:4], [
            "watchdog: game dt 3c87fcb9 (x1e6 16600) raw 3d086595 (x1e6 33300) timescale 3f800000 (x1e3 1000) "
            "freq 1000000 flag 1",
            "watchdog: timescale reasons: [3]=500/1000",
            "watchdog: bootflow-8 gates: 3bbb310=1 16d28fc=2 16d289c=-3 3ba42fc=4 3d4653c=5 16d28a0=6 3d46630=0",
            "watchdog: movie pools slot 0: cpu limit 0 used 0 | cpu-gpu limit 0 used 0 | gpu limit 0 used 0 | owner 0",
        ])
        self.assertIn("watchdog: movie pools slot 2: cpu limit 100 used 80 | cpu-gpu limit 0 used 0 | gpu limit 0 "
                      "used 0 | owner abc", log)
        i = log.index("watchdog: forced title-ready flag (eboot+0x1a83739)")
        self.assertEqual(log[i + 1], "watchdog: title-world target 'TitleWorld', 2 entries: 'a' 'b'")
        mspace = [ln for ln in log if ln.startswith("watchdog: CPU mspace")]
        self.assertEqual(mspace, ["watchdog: CPU mspace 5150 rc 0: max system 300 MiB, system 290 MiB, max in use "
                                  "285 MiB, in use 280 MiB"])
        task = [ln for ln in log if ln.startswith("watchdog: blocking task")]
        self.assertEqual(len(task), 1)
        self.assertRegex(task[0], r"^watchdog: blocking task 0/1 [0-9a-f]+ vtable \?\+[0-9a-f]+ isBusy eboot\.bin\+123450$")
        self.assertIn("watchdog:   task state 27 enabled 1; load counter 7", log)
        self.assertTrue(any(ln.startswith("watchdog:   mission world list ") and ln.endswith(": 1 resources, flag 0")
                            for ln in log), log)
        self.assertIn("watchdog:   waiting for #0 handle 1122334455667788 gen 10: resource 0", log)
        self.assertEqual(log[-1], "watchdog:   1 of 1 not loaded")

    def test_the_core_only_calls_it(self) -> None:
        """crash.c keeps no reads of the title's globals: PF_DIAG goes through ext_title_watch, phases 0-2"""
        src = (NATIVE / "src/crash.c").read_text()
        self.assertNotIn("0x16beac0", src)
        self.assertNotIn("log_mission_world_wait", src)
        self.assertEqual([src.count(f"ext_title_watch(b, {p}, ") for p in range(3)], [1, 1, 1])


if __name__ == "__main__":
    unittest.main()
