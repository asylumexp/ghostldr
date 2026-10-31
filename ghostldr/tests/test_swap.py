"""The extension's swap file (native/ext/src/swap.c) on this machine: a heap
backed in part by memory, the rest paged out to a file and back.

    python3 -m unittest test_swap        (from ghostldr/tests)
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
from test_save_race import x86_64_cc  # noqa: E402


class SwapFile(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = x86_64_cc()
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = Path(cls.tmp.name)
        free = ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                "-Wno-gnu-include-next", "-ffreestanding", "-fno-builtin", f"-I{HERE / 'ext_native'}",
                f"-I{NATIVE / 'ext/include'}", f"-I{NATIVE / 'include'}", "-DGHOST_ABI=0u"]
        objs = []
        for src in (HERE / "ext_native/swap_host.c", NATIVE / "ext/src/swap.c", NATIVE / "src/rt.c"):
            obj = tmp / (src.stem + ".o")
            subprocess.run(cc + free + ["-c", str(src), "-o", str(obj)], check=True)
            objs.append(str(obj))
        obj = tmp / "swap_os.o"
        subprocess.run(cc + ["-std=gnu11", "-O1", "-g", "-Wall", "-c", str(HERE / "ext_native/swap_os.c"), "-o",
                             str(obj)], check=True)
        cls.exe = tmp / "swap_host"
        subprocess.run(cc + objs + [str(obj), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_a_heap_bigger_than_its_memory(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            out = subprocess.run([str(self.exe), root], capture_output=True, text=True, timeout=120)
        lines = out.stdout.splitlines()
        self.assertEqual([ln for ln in lines if ln.startswith("FAILED")], [], out.stdout + out.stderr)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        for want in ("ok 100 pages written through 32 resident ones read back whole",
                     "ok pages never written read as zeros",
                     "ok a pinned page stays resident while 99 others are paged through",
                     "ok unpinned, it goes out again and comes back the same",
                     "ok after the title's run, faults are the crash handler's again"):
            self.assertIn(want, lines)
        self.assertTrue([ln for ln in lines if ln.startswith("swap: TEST: all 2 MiB in use: paging out to ")])


if __name__ == "__main__":
    unittest.main()
