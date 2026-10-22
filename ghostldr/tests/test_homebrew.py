"""The extension's homebrew families, built for this machine
(ext_native/homebrew_host.c): the heap for titles without a libc of their own
(hle_libc_heap.c) and its stdio (hle_libc_stdio.c), POSIX thread-specific
data (hle_tsd.c), and stdout, ioctl and FreeType faces (hle_homebrew.c).

    python3 -m unittest test_homebrew        (from ghostldr/tests)
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

FAMILIES = ("hle_libc_heap", "hle_libc_stdio", "hle_tsd", "hle_homebrew")


class HomebrewFamilies(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = host_cc()
        if not cc:
            raise unittest.SkipTest("no C compiler whose programs run on this machine")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name) / "homebrew_host"
        sources = [HERE / "ext_native/homebrew_host.c", NATIVE / "src/nid.c"]
        sources += [NATIVE / f"ext/src/{f}.c" for f in FAMILIES]
        subprocess.run(cc + ["-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                             "-Wno-gnu-include-next", "-ffreestanding", "-fno-builtin",
                             f"-I{HERE / 'ext_native'}", f"-I{NATIVE / 'ext/include'}", f"-I{NATIVE / 'include'}",
                             "-DGHOST_ABI=0u", *map(str, sources), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_host(self, scenario: str) -> list[str]:
        out = subprocess.run([str(self.exe), scenario], capture_output=True, text=True, timeout=120)
        lines = out.stdout.splitlines()
        failed = [ln for ln in lines if ln.startswith("FAILED")]
        self.assertEqual(failed, [], "\n".join(lines))
        self.assertEqual(out.returncode, 0, out.stderr + "\n".join(lines))
        return lines

    def test_heap_serves_the_malloc_family(self) -> None:
        lines = self.run_host("heap")
        self.assertIn("ok four threads allocating and freeing at once", lines)
        self.assertIn("libc_heap: no title module exports malloc: libSceLibcInternal's malloc family goes to "
                      "ghostldr's heap (the title's flexible memory)", lines)
        self.assertTrue([ln for ln in lines if ln.startswith("libc_heap: no flexible memory for 4112 KiB more")])

    def test_heap_stays_off_with_the_titles_own_libc(self) -> None:
        lines = self.run_host("heap-off")
        self.assertIn("libc_heap: libc.prx exports malloc: the title's own libc keeps its heap", lines)

    def test_thread_specific_data(self) -> None:
        self.run_host("tsd")

    def test_system_libc_stdout_goes_to_the_log(self) -> None:
        self.run_host("stdio")

    def test_stdout_ioctl_and_fonts(self) -> None:
        lines = self.run_host("homebrew")
        self.assertTrue([ln for ln in lines if ln.startswith("FT_New_Face(/app0/data.txt, 0): 34 bytes from ghostfs")])


if __name__ == "__main__":
    unittest.main()
