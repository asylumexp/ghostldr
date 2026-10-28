"""Raw system call sites in a title's code (hle_rawsys.c), built for this
machine as x86-64 (ext_native/rawsys_host.c): sites patched as a module's
text is, then run, with libkernel's functions, errno, the carry flag and the
kept registers checked.

    python3 -m unittest test_rawsys        (from ghostldr/tests)
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
from test_ext import x86_64_cc  # noqa: E402


class RawSyscalls(unittest.TestCase):
    def test_sites_go_through_libkernel(self) -> None:
        cc = x86_64_cc()
        if not cc:
            self.skipTest("no way to build and run x86-64 code here")
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "rawsys_host"
            sources = [HERE / "ext_native/rawsys_host.c", NATIVE / "src/nid.c", NATIVE / "ext/src/hle_rawsys.c", NATIVE / "src/rt.c"]
            subprocess.run(cc + ["-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                                 "-Wno-gnu-include-next", "-ffreestanding", "-fno-builtin",
                                 f"-I{HERE / 'ext_native'}", f"-I{NATIVE / 'ext/include'}",
                                 f"-I{NATIVE / 'include'}", f"-I{NATIVE / 'ext/src'}", "-DGHOST_ABI=0u",
                                 *map(str, sources), "-o", str(exe)], check=True)
            out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        lines = out.stdout.splitlines()
        if any(ln.startswith("SKIP") for ln in lines):
            self.skipTest(lines[0])
        self.assertEqual([ln for ln in lines if ln.startswith("FAILED")], [], "\n".join(lines))
        self.assertEqual(out.returncode, 0, out.stderr + "\n".join(lines))
        self.assertIn("ok kexec (11): ENOSYS with the carry flag", lines)


if __name__ == "__main__":
    unittest.main()
