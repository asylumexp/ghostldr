"""The extension's SHA-256 pin (GHOSTLDR_EXT_SHA256), driven through
the ext_host harness from test_ext.py: run.py pins the first
24 bytes of the blob's SHA-256 in the boot context, and ext.c
verifies the served blob against them after the CRC-32 screen.

The case that matters is the last one: a blob whose bytes changed
AND whose CRC was recomputed - the old screen passes it, the pin
refuses it.  (An all-zero pin is not a wrong pin: zero bytes are
how an unpinned boot reads, so the wrong-pin case flips real
digest bytes.)

This class builds only ext_host: at v0.4.0 the kernel_host source
list in test_ext.py is missing loader.c's new dependencies
(bigexec, ghostfs), a pre-existing upstream breakage on every
platform, and the pin lives entirely in ext.c's fetch path.

    python3 -m unittest test_ext_sha
"""

from __future__ import annotations

import hashlib
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

from test_ext import EXT, HERE, ROOT, x86_64_cc


class ExtensionShaPin(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = x86_64_cc()
        if not cc:
            raise unittest.SkipTest("no way to build and run x86-64 code here")
        if not EXT.exists():
            raise unittest.SkipTest("build/ghostldr/ext.bin missing: run make -C ghostldr/native")
        cls.blob = EXT.read_bytes()
        cls.crc = hex(zlib.crc32(cls.blob))
        cls.sha = hashlib.sha256(cls.blob).hexdigest()
        abi, = struct.unpack_from("<I", cls.blob, 20)
        cls.tmp = tempfile.TemporaryDirectory()
        out = Path(cls.tmp.name) / "ext_host"
        flags = ["-O1", "-std=gnu11", "-fno-builtin", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror",
                 f"-I{ROOT / 'ghostldr/native/include'}", f"-DGHOST_ABI={abi}u"]
        core = ROOT / "ghostldr/native/src"
        sources = [(core / f"{name}.c", flags) for name in ("ext", "resolve", "elf", "nid", "sha256")] + \
            [(HERE / "ext_host/sys.c", ["-O1"]), (HERE / "ext_host/env.c", flags)]
        objs = []
        for src, extra in sources:
            obj = Path(cls.tmp.name) / f"ext_host_{src.stem}.o"
            subprocess.run(cc + extra + ["-c", str(src), "-o", str(obj)], check=True)
            objs.append(str(obj))
        subprocess.run(cc + objs + ["-o", str(out)], check=True)
        cls.host = out

    def run_raw(self, sha: str, blob: Path = EXT, crc: str | None = None) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(self.host), "small", str(blob), crc if crc is not None else self.crc, sha],
            capture_output=True, text=True)

    def test_correct_pin_verifies(self) -> None:
        p = self.run_raw(self.sha)
        self.assertIn("SHA-256 verified", p.stdout)

    def test_wrong_pin_refuses(self) -> None:
        wrong = ("0" if self.sha[0] != "0" else "1") + self.sha[1:]
        p = self.run_raw(wrong)
        self.assertNotEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("SHA-256 mismatch", p.stdout)

    def test_dash_leaves_the_boot_unpinned(self) -> None:
        p = self.run_raw("-")
        self.assertIn("ext: received", p.stdout)
        self.assertNotIn("SHA-256 verified", p.stdout)
        self.assertNotIn("SHA-256 mismatch", p.stdout)

    def test_recomputed_crc_does_not_save_a_changed_blob(self) -> None:
        bad = Path(self.tmp.name) / "ext_patched.bin"
        data = bytearray(self.blob)
        data[len(data) // 2] ^= 0xFF
        bad.write_bytes(data)
        p = self.run_raw(self.sha, blob=bad, crc=hex(zlib.crc32(bytes(data))))
        self.assertNotEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("SHA-256 mismatch", p.stdout)


if __name__ == "__main__":
    unittest.main()
