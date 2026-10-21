"""The runtime extension offline: the built ext.bin goes through the core's
real loader and binder (src/ext.c, resolve.c, elf.c, nid.c) in an x86-64
process (ext_host/, Rosetta on Apple silicon), its NID table answers like the
name lists, check_ext.py passes, and run.py serves it.  Needs
`make -C ghostldr/native` first.

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import os
import platform
import shutil
import socket
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
sys.path.insert(0, str(ROOT / "ghostldr/tools"))
sys.path.insert(0, str(ROOT / "ghostldr/host"))


def x86_64_cc() -> list[str] | None:
    """A compiler command whose output this machine can run as x86-64."""
    machine = platform.machine()
    if sys.platform == "darwin" and shutil.which("clang"):
        if machine == "x86_64" or subprocess.run(["arch", "-x86_64", "/usr/bin/true"]).returncode == 0:
            return ["clang", "-arch", "x86_64"]
    if machine in ("x86_64", "AMD64"):
        cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
        return [cc] if cc else None
    return None


@unittest.skipUnless(EXT.exists(), "build/ghostldr/ext.bin missing: run make -C ghostldr/native")
class ExtensionOnHost(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = x86_64_cc()
        if not cc:
            raise unittest.SkipTest("no way to build and run x86-64 code here")
        cls.blob = EXT.read_bytes()
        cls.crc = hex(zlib.crc32(cls.blob))
        abi, = struct.unpack_from("<I", cls.blob, 20)
        cls.tmp = tempfile.TemporaryDirectory()
        out = Path(cls.tmp.name) / "ext_host"
        flags = ["-O1", "-std=gnu11", "-fno-builtin", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror",
                 f"-I{ROOT / 'ghostldr/native/include'}", f"-DGHOST_ABI={abi}u"]
        objs = []
        core = ROOT / "ghostldr/native/src"
        sources = [(core / f"{name}.c", flags) for name in ("ext", "resolve", "elf", "nid")]
        for src, extra in sources + [(HERE / "ext_host/env.c", flags), (HERE / "ext_host/sys.c", ["-O1"])]:
            obj = Path(cls.tmp.name) / (src.stem + ".o")
            subprocess.run(cc + extra + ["-c", str(src), "-o", str(obj)], check=True)
            objs.append(str(obj))
        subprocess.run(cc + objs + ["-o", str(out)], check=True)
        cls.host = out

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_host(self, mode: str, *extra: str) -> str:
        p = subprocess.run([str(self.host), mode, str(EXT), self.crc, *extra], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertNotIn("FAILED", p.stdout)
        return p.stdout

    def test_loads_in_the_small_area_and_self_tests(self) -> None:
        out = self.run_host("small")
        self.assertIn("into flexible memory", out)
        self.assertIn("in the small RX area", out)
        self.assertIn("ext: self-test ok", out)

    def test_loads_in_a_jit_block(self) -> None:
        out = self.run_host("jit")  # with flexible memory exhausted, too
        self.assertIn("into direct memory", out)
        self.assertIn("in a new JIT block", out)

    def test_falls_back_to_a_jit_block_when_the_small_area_is_full(self) -> None:
        self.assertIn("in a new JIT block", self.run_host("tiny"))

    def test_imports_bind_in_order_with_nids_aliases_and_policies(self) -> None:
        out = self.run_host("resolve")
        self.assertIn("ok by NID alone", out)
        self.assertIn("ok no host fallback: the stub returns the library's error", out)
        self.assertIn("DENIED call: libGhostldrExt", out)
        self.assertIn("UNRESOLVED call: libSceFake AAAAAAAAAAA (?)", out)

    def test_refuses_a_bad_transfer_before_touching_anything(self) -> None:
        self.assertIn("is corrupt", self.run_host("badcrc"))
        self.assertIn("receive failed", self.run_host("short"))
        self.assertIn("boot stops here", self.run_host("nosock"))

    def test_nid_table_matches_the_name_lists(self) -> None:
        import gen_nids
        import orbis_elf as oe
        db = gen_nids.name_db()
        unknown = [oe.compute_nid(f"ghostldr_not_a_symbol_{i}") for i in range(50)]
        nids = sorted(db) + unknown
        listing = Path(self.tmp.name) / "nids.txt"
        listing.write_text("\n".join(nids))
        got = {}
        for line in self.run_host("small", str(listing)).splitlines():
            if line.startswith("nid "):
                _, nid, name = line.split(" ", 2)
                got[nid] = None if name == "-" else name
        self.assertEqual(len(got), len(nids))
        self.assertEqual({nid: got[nid] for nid in db}, db)
        self.assertTrue(all(got[nid] is None for nid in unknown))


@unittest.skipUnless(EXT.exists(), "build/ghostldr/ext.bin missing: run make -C ghostldr/native")
class ExtensionTools(unittest.TestCase):
    def test_check_ext_passes(self) -> None:
        p = subprocess.run([sys.executable, str(ROOT / "ghostldr/tools/check_ext.py")], capture_output=True,
                           text=True)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)

    def test_run_py_serves_the_blob_and_passes_size_and_crc(self) -> None:
        import run
        blob = EXT.read_bytes()
        try:
            server = run.ExtServer(blob)
        except SystemExit as e:
            self.skipTest(str(e))
        server.start()
        try:
            got = bytearray()
            with socket.create_connection(("127.0.0.1", run.EXT_PORT), timeout=10) as s:
                while chunk := s.recv(1 << 20):
                    got += chunk
            self.assertEqual(bytes(got), blob)
            self.assertTrue(server.sent.wait(5))
        finally:
            server.stop.set()
            server.join(2)
            server.sock.close()
        if (ROOT / "build/ghostldr/core.json").exists():
            payload = run.build_payload(0, "192.0.2.1", "ghostfs", {}, blob).decode()
            self.assertIn(f"GHOSTLDR_EXT_SIZE = {len(blob)}", payload)
            self.assertIn(f"GHOSTLDR_EXT_CRC = {zlib.crc32(blob):#x}", payload)
            self.assertIn(f"GHOSTLDR_EXT_PORT = {run.EXT_PORT}", payload)
            self.assertLess(len(payload), run.LUA_MAX)


if __name__ == "__main__":
    unittest.main()
