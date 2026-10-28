"""The save data family (ghostldr/native/ext/src/hle_savedata.c) offline.

Its file is compiled with the host's own C compiler into a test program
(ext_host/savedata_host.c, for the host's own architecture: the family is
plain C, so no Rosetta is needed) and run against a fake
console (ext_host/savedata_fake.c): libkernel's file calls on a temporary
directory and SWRR's save service as measured on hardware, with commits at
unmount, 16 mount slots, ghostfs holding 12 of them, quotas, a storage cap,
BUSY, BROKEN and crashes.  The Ghost replay also links the core's own
savedata.c and checks that Ghost's GHOSTSAVE comes out byte for byte the same.

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
NATIVE = ROOT / "ghostldr/native"


class SaveDataFamily(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = shutil.which("clang") or shutil.which("cc")
        if not cc:
            raise unittest.SkipTest("no C compiler")
        cc = [cc]
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = Path(cls.tmp.name)
        flags = ["-O1", "-g", "-std=gnu11", "-fno-builtin", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror",
                 f"-I{NATIVE / 'include'}", f"-I{NATIVE / 'ext/include'}"]
        sources = [(HERE / "ext_host/savedata_host.c", flags), (HERE / "ext_host/savedata_fake.c", flags),
                   (NATIVE / "src/savedata.c", flags), (HERE / "ext_host/savedata_rt.c", ["-O1", "-Wall", "-Werror"]),
                   (HERE / "ext_host/savedata_os.c", ["-O1", "-Wall", "-Werror"])]
        objs = []
        for src, extra in sources:
            obj = tmp / (src.parent.name + "_" + src.stem + ".o")
            subprocess.run(cc + extra + ["-c", str(src), "-o", str(obj)], check=True)
            objs.append(str(obj))
        cls.host = tmp / "savedata_host"
        subprocess.run(cc + objs + ["-o", str(cls.host)], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_mode(self, mode: str) -> str:
        work = Path(self.tmp.name) / mode
        p = subprocess.run([str(self.host), mode, str(work)], capture_output=True, text=True, timeout=300)
        out = p.stdout + p.stderr
        failed = [line for line in out.splitlines() if "FAILED" in line or "VIOLATION" in line]
        self.assertEqual(failed, [], "\n".join(failed))
        self.assertEqual(p.returncode, 0, out[-4000:])
        return out

    def test_stock_console_containers(self) -> None:
        out = self.run_mode("cont")
        for line in ("first boot: mount RDWR is NOT_FOUND", "CREATE on an existing save: EXISTS",
                     "mount NULL: PARAMETER 0x809F0000", "a 7th: MOUNT_FULL 0x809F000C",
                     "unmount with a file open: BUSY", "the unmount committed the title's file",
                     "Get2 before Setup2: MEMORY_NOT_READY", "it lives in GTESTM05SAVE", "title_store_read",
                     "GTESTM07SAVE made again with 404 blocks", "GTESTM06SAVE and its 4 KiB untouched",
                     "slot 0 untouched in GTESTSAVE", "an existing container keeps its size and the title's files",
                     "sd_list: SAVE0 with its size, icon and userParam", "a save filled to its size still commits",
                     "no real call ever named SWRR's save"):
            self.assertIn(f"savedata_host: ok {line}", out)

    def test_dev_install_directories(self) -> None:
        out = self.run_mode("dir")
        self.assertIn("savedata_host: ok dev: Ghost's old save memory reads unchanged", out)
        self.assertIn("savedata_host: ok dev: a 17th: MOUNT_FULL", out)

    def test_savedata_paths(self) -> None:
        out = self.run_mode("remap")
        self.assertIn("savedata_host: ok no mounts: /savedata0/x is refused", out)
        self.assertIn("savedata_host: ok /savedata16/x is refused", out)
        self.assertIn("savedata_host: ok remap: SWRR's /savedata0 never reached", out)

    def test_ghost_of_tsushima_saves(self) -> None:
        out = self.run_mode("ghost")
        self.assertIn("savedata_host: ok ext: GHOSTSAVE/sdmemory_0.bin is byte for byte the core's", out)
        self.assertIn("savedata_host: ok ext: the core's GHOSTSAVE progress loads unchanged", out)
        self.assertIn("savedata_host: ok core: reads the extension's GHOSTSAVE unchanged", out)
        self.assertIn("savedata_host: ok ext: GHOSTSAVE is the only container it mounted", out)


if __name__ == "__main__":
    unittest.main()
