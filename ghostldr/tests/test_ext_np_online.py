"""The NP online families of the runtime extension (ext/src/hle_np_online*.c),
run on the built ext.bin through the core's real loader and binder
(ext_host/np_online.c on top of ext_host/env.c; Rosetta on Apple silicon):
context, request and handle lifecycles, the signed-out answers, Ghost's
SessionSignaling and WebApi2 boot calls, and each library's catch-all.  A
second blob built with NPO_PERSONA_A and NPO_M2_ASYNC covers the network
answers and Matching2's START_OVER pump.  Needs `make -C ghostldr/native`.

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
NATIVE = ROOT / "ghostldr/native"
EXT = ROOT / "build/ghostldr/ext.bin"
LLVM = Path(os.environ.get("LLVM", "/opt/homebrew/opt/llvm@18/bin"))
sys.path.insert(0, str(HERE))

from test_ext import x86_64_cc  # noqa: E402


@unittest.skipUnless(EXT.exists(), "build/ghostldr/ext.bin missing: run make -C ghostldr/native")
class NpOnlineFamilies(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = x86_64_cc()
        if not cc:
            raise unittest.SkipTest("no way to build and run x86-64 code here")
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = Path(cls.tmp.name)
        abi, = struct.unpack_from("<I", EXT.read_bytes(), 20)
        flags = ["-O1", "-std=gnu11", "-fno-builtin", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror",
                 f"-I{NATIVE / 'include'}", f"-DGHOST_ABI={abi}u"]
        objs = []
        core = NATIVE / "src"
        sources = [(core / f"{name}.c", flags) for name in ("ext", "resolve", "elf", "nid")]
        sources += [(HERE / "ext_host/np_online.c", flags), (HERE / "ext_host/sys.c", ["-O1"]),
                    (HERE / "ext_host/np_online_sys.c", ["-O1"])]
        for src, extra in sources:
            obj = tmp / (src.stem + ".o")
            subprocess.run(cc + extra + ["-c", str(src), "-o", str(obj)], check=True)
            objs.append(str(obj))
        cls.host = tmp / "np_online"
        subprocess.run(cc + objs + ["-o", str(cls.host)], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_host(self, blob: Path, persona: str) -> str:
        crc = hex(zlib.crc32(blob.read_bytes()))
        p = subprocess.run([str(self.host), str(blob), crc, persona], capture_output=True, text=True, timeout=120)
        out = p.stdout + p.stderr
        failed = [line for line in out.splitlines() if "FAILED" in line]
        self.assertEqual(p.returncode, 0, "\n".join(failed) or out[-4000:])
        self.assertIn("npo_test: all passed", out)
        return out

    def test_signed_out_persona(self) -> None:
        out = self.run_host(EXT, "b")
        self.assertIn("npo_test: ok ContextStart fails synchronously (NP_SIGNED_OUT)", out)
        self.assertIn("npo_test: ok WebApi2 Initialize never fails", out)
        self.assertIn("npo_test: ok catch-all: WebApi IntCreateRequest", out)
        self.assertIn("DENIED call: libSceNpWebApi N2Jbx4tIaQ4", out)
        self.assertNotIn("npo-m2-pump", out)  # no thread in the default build

    @unittest.skipUnless((LLVM / "clang").exists() and shutil.which("make"), "no LLVM 18 to build a variant")
    def test_signed_in_unreachable_persona_with_matching2_async(self) -> None:
        out_dir = Path(self.tmp.name) / "persona_a"
        p = subprocess.run(["make", "-C", str(NATIVE), f"LLVM={LLVM}", f"OUT={out_dir}",
                            "EXT_CFLAGS=-DNPO_PERSONA_A=1 -DNPO_M2_ASYNC=1", str(out_dir / "ext.bin")],
                           capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        out = self.run_host(out_dir / "ext.bin", "a")
        self.assertIn("thread npo-m2-pump created", out)
        self.assertIn("npo_test: ok START_OVER delivered", out)
        self.assertIn("npo_test: ok RecordScore: network error", out)


if __name__ == "__main__":
    unittest.main()
