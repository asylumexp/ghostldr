"""The Piglet family (hle_gles.c, gles_raster.c): ghostldr's software EGL and
OpenGL ES 2.0 for homebrew drawn with Piglet, built for this machine
(ext_native/gles_host.c) and driven the way SDL2's GLES2 renderer drives it.

    python3 -m unittest test_gles        (from ghostldr/tests)
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


class GlesFamily(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = host_cc()
        if not cc:
            raise unittest.SkipTest("no C compiler whose programs run on this machine")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name) / "gles_host"
        sources = [HERE / "ext_native/gles_host.c", NATIVE / "src/nid.c", NATIVE / "ext/src/hle_gles.c",
                   NATIVE / "ext/src/gles_raster.c", NATIVE / "ext/src/gles_gpu.c"]
        subprocess.run(cc + ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                             "-Wno-gnu-include-next", "-ffreestanding", "-fno-builtin", "-DGLES_HOST",
                             f"-I{HERE / 'ext_native'}", f"-I{NATIVE / 'ext/include'}", f"-I{NATIVE / 'include'}",
                             "-DGHOST_ABI=0u", *map(str, sources), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_host(self, scenario: str) -> list[str]:
        out = subprocess.run([str(self.exe), scenario], capture_output=True, text=True, timeout=300)
        lines = out.stdout.splitlines()
        failed = [ln for ln in lines if ln.startswith("FAILED")]
        self.assertEqual(failed, [], "\n".join(lines))
        self.assertEqual(out.returncode, 0, out.stderr + "\n".join(lines))
        return lines

    def test_sdl_shaders_are_recognised(self) -> None:
        self.run_host("shaders")

    def test_draws_as_sdl_expects(self) -> None:
        lines = self.run_host("draw")
        self.assertIn("ok blended square: all 289 pixels 128 grey, the diagonal drawn once", lines)
        self.assertIn("ok a Piglet shader binary compiles from the source it carries", lines)

    def test_gpu_path_draws_as_software_does(self) -> None:
        """the same draws through gles_gpu.c's command buffers, run by a fake
        GPU (the software rasteriser fed from the registers alone)"""
        lines = self.run_host("draw-gpu")
        self.assertIn("ok blended square: all 289 pixels 128 grey, the diagonal drawn once", lines)
        self.assertIn("ok the draws went to the GPU path, a command buffer a frame", lines)
        checks = [ln for ln in lines if "GPU check frame" in ln]
        self.assertGreaterEqual(len(checks), 8, "\n".join(lines))
        self.assertEqual([ln for ln in checks if "(identical)" not in ln], [])

    def test_gcn_header_matches_shaders(self) -> None:
        """native/ext/src/gles_gcn.h was made from the current gcn/*.s (tools/mkgcn.py)"""
        out = subprocess.run([sys.executable, str(HERE.parent / "tools/mkgcn.py"), "--check"], capture_output=True,
                             text=True)
        self.assertEqual(out.returncode, 0, out.stderr)

    def test_full_hd_frame_time(self) -> None:
        lines = self.run_host("perf")
        print("\n".join(ln for ln in lines if ln.startswith("perf:")))


if __name__ == "__main__":
    unittest.main()
