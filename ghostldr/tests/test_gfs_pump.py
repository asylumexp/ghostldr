"""The ghostfs installer's pump (native/pump/pump.c), offline.

  - its C code, built for the computer (pump_host/harness.c), moves a
    pattern from a socket into a file through its ring of buffers, and
    stops cleanly when the connection drops or a write fails;
  - the checked-in blob the installer sends (data/gfs_pump.bin) is what
    `make pump` builds from the source (needs LLVM 18), and has no
    relocations to apply.

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
NATIVE = ROOT / "ghostldr/native"
BLOB = ROOT / "ghostldr/data/gfs_pump.bin"
LLVM = Path(os.environ.get("LLVM", "/opt/homebrew/opt/llvm@18/bin"))
CC = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
WRITE_FAILED = 0x8002001C - (1 << 32)


def pattern(n: int) -> bytes:
    return bytes((i * 131 + (i >> 9)) & 0xFF for i in range(n))


@unittest.skipUnless(CC, "no C compiler")
class PumpLogic(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name) / "harness"
        subprocess.run([CC, "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-pthread",
                        str(HERE / "pump_host/harness.c"), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_pump(self, size: int, chunk: int, send: int, fail_after: int | None = None) -> tuple[int, int, int, bytes]:
        out = Path(self.tmp.name) / "out.bin"
        args = [str(self.exe), str(out), str(size), str(chunk), str(send)]
        if fail_after is not None:
            args.append(str(fail_after))
        p = subprocess.run(args, capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stderr)
        status, received, written = (int(x) for x in p.stdout.split())
        return status, received, written, out.read_bytes()

    def test_moves_everything_in_order(self) -> None:
        for size, chunk in ((0, 4096), (1, 4096), (4096 * 4, 4096), (4096 * 9 + 17, 4096), (3 << 20, 65536)):
            with self.subTest(size=size, chunk=chunk):
                status, received, written, data = self.run_pump(size, chunk, size)
                self.assertEqual((status, received, written), (0, size, size))
                self.assertEqual(data, pattern(size))

    def test_a_dropped_connection_stops_after_writing_what_arrived(self) -> None:
        size, chunk, sent = 100 * 4096, 4096, 37 * 4096 + 5
        status, received, written, data = self.run_pump(size, chunk, sent)
        self.assertEqual((status, received), (-1, sent))
        self.assertEqual(written, 37 * 4096)          # whole chunks only
        self.assertEqual(data, pattern(written))

    def test_a_failed_write_stops_the_transfer_with_its_error(self) -> None:
        size, chunk = 100 * 4096, 4096
        status, received, written, data = self.run_pump(size, chunk, size, fail_after=10 * 4096)
        self.assertEqual(status, WRITE_FAILED)
        self.assertEqual(written, 10 * 4096)
        self.assertLess(received, size)               # the rest is left for the receiver to drop
        self.assertEqual(data, pattern(written))


class PumpBlob(unittest.TestCase):
    def test_blob_is_flat_code(self) -> None:
        blob = BLOB.read_bytes()
        self.assertTrue(0 < len(blob) < 4096)

    @unittest.skipUnless((LLVM / "clang").exists() and shutil.which("make"), "no LLVM 18")
    def test_blob_matches_the_source(self) -> None:
        with tempfile.TemporaryDirectory() as out:
            subprocess.run(["make", "-s", "-C", str(NATIVE), f"LLVM={LLVM}", f"OUT={out}", f"{out}/pump.bin"],
                           check=True, capture_output=True)
            self.assertEqual(Path(out, "pump.bin").read_bytes(), BLOB.read_bytes(),
                             "data/gfs_pump.bin is stale: run `make pump` in ghostldr/native")
            relocs = subprocess.run([str(LLVM / "llvm-readelf"), "-r", f"{out}/pump/pump.elf"],
                                    capture_output=True, text=True, check=True).stdout
            self.assertIn("no relocations", relocs)


if __name__ == "__main__":
    unittest.main()
