"""PS4 support (docs/ps4.md), offline.

- arena.c and ps4.c in a host process (native_host/arena_host.c) against
  the executable regions a PS5 and a PS4 boot logged and a fake compiler
  process: where the title's modules go, which writes the compiler is asked
  to make, and what happens when it cannot write a buffer;
- ghostldr.lua under a fake Luac0re (boot_fake.lua, needs a desktop Lua
  5.3/5.4): the platform flag, the firmware and the compiler's syscall
  gadget in the boot context, at the offsets ghost.h has;
- run.py probe --diag.

    python3 -m unittest test_ps4        (from ghostldr/tests)
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
NATIVE = ROOT / "ghostldr/native"
sys.path.insert(0, str(ROOT / "ghostldr/host"))
sys.path.insert(0, str(HERE))
from test_profiles import build_host  # noqa: E402
from test_manager import find_lua  # noqa: E402

GHOST_MODULES = ("libSceFios2.prx", "libc.prx", "libSceNpToolkit2.prx", "eboot.bin")
# the IPU buffer is 0x48000 below the EE buffer on both consoles
EE = 0x48000


def offsets(out: str) -> dict[str, int]:
    return {m[1]: int(m[2], 16) for m in re.finditer(r"module (\S+) at \+([0-9a-f]+) rx 1 write 0", out)}


class ArenaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name) / "arena_host"
        build_host("arena_host", [HERE / "native_host/arena_host.c", NATIVE / "src/arena.c", NATIVE / "src/ps4.c"],
                   cls.exe)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_host(self, *args: str, code: int = 0) -> str:
        p = subprocess.run([str(self.exe), *args], capture_output=True, text=True, timeout=60)
        if p.returncode == 77:
            self.skipTest(p.stdout.strip())
        self.assertEqual(p.returncode, code, p.stdout + p.stderr)
        if code == 0:
            self.assertNotIn("FAULT", p.stdout)
        return p.stdout

    def test_ps5_places_as_before(self) -> None:
        """No PS4 flag: Ghost's modules where a 13.60 PS5 boot logged them
        (IPU 9009d4000: 900a20000, 900bb0000, 900cd0000, 900d90000), and no
        compiler-view queries."""
        out = self.run_host("ps5")
        self.assertEqual(offsets(out), {"libSceFios2.prx": 0x4c000, "libc.prx": 0x1dc000,
                                        "libSceNpToolkit2.prx": 0x2fc000, "eboot.bin": 0x3bc000})
        self.assertNotIn("PS4", out)
        self.assertIn("selector region 0 +4c000", out)

    def test_old_rule_faults_like_the_ps4(self) -> None:
        """v0.1.1's first module on the PS4 layout: the EE buffer's very start,
        which the compiler cannot write (the 12.02 crash at 914b20000)."""
        out = self.run_host("old-rule", code=3)
        self.assertIn(f"old first module +{EE:x}", out)
        self.assertIn(f"FAULT compiler write at +{EE:x}", out)

    def test_header_rule_alone_fixes_the_ps4_layout(self) -> None:
        out = self.run_host("ps4-noflag")
        self.assertEqual(offsets(out)["libSceFios2.prx"], EE + 0x10000)

    def test_ps4_verified_buffers(self) -> None:
        """The compiler writes each buffer past its first 16 KiB: modules
        16 KiB higher than v0.1.1 put them, test writes put back, the
        header refused."""
        out = self.run_host("ps4-header")
        self.assertEqual(offsets(out), {"libSceFios2.prx": EE + 0x10000, "libc.prx": EE + 0x1a0000,
                                        "libSceNpToolkit2.prx": EE + 0x2c0000, "eboot.bin": EE + 0x380000})
        self.assertIn("header write refused", out)
        self.assertIn("stubs at +38000 write 0", out)
        self.assertNotIn("NOT writable", out)
        self.assertNotIn("block rx", out)

    def test_ps4_nothing_writable_uses_a_jit_block(self) -> None:
        for model in ("ps4-none", "ps4-other", "ps4-vu1"):
            with self.subTest(model=model):
                out = self.run_host(model)
                self.assertIn(f"block rx +{EE + 0x10000:x} size 18f0000", out)
                self.assertEqual(set(offsets(out)), set(GHOST_MODULES))
                self.assertEqual(offsets(out)["libSceFios2.prx"], EE + 0x10000)
        # the EE buffer is writable there but other memory: the probe sees it
        self.assertEqual(self.run_host("ps4-other").count("NOT writable through the compiler"), 1)

    def test_ps4_small_title_fits_vu1(self) -> None:
        out = self.run_host("ps4-vu1", "small")
        self.assertNotIn("block rx", out)
        vu1 = EE + 0x300c000   # past its header it is on a 64 KiB boundary here
        self.assertEqual(offsets(out)["libSceFios2.prx"], vu1 + 0x4000)
        self.assertEqual(set(offsets(out)), set(GHOST_MODULES))

    def test_ps4_no_block_stops_cleanly(self) -> None:
        out = self.run_host("ps4-noblock")
        self.assertIn("code region -1", out)
        self.assertIn("the title's code has nowhere to go", out)

    def test_code_need_matches_the_ps4_log(self) -> None:
        """v0.1.1 on the PS4 placed eboot.bin 0x370000 above libSceFios2,
        text 133c000 and RELRO 1400000+180000."""
        self.assertIn("need 18f0000", self.run_host("ps4-header"))
        self.assertEqual(0x370000 + 0x1580000, 0x18f0000)

    def test_heap_kept_only_while_used(self) -> None:
        self.assertIn("heap 1 0 0 0", self.run_host("heap"))


@unittest.skipUnless(find_lua(), "needs a desktop Lua 5.3 or 5.4")
class BootContextTests(unittest.TestCase):
    """ghostldr.lua's boot context under a fake Luac0re."""

    @classmethod
    def setUpClass(cls) -> None:
        import run
        if not run.CORE.exists() or not run.LAYOUT.exists():
            raise unittest.SkipTest("build the core first (make -C ghostldr/native)")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.payload = Path(cls.tmp.name) / "boot.lua"
        cls.payload.write_bytes(run.build_payload(2, "127.0.0.1", "ghostfs", {}))
        exe = Path(cls.tmp.name) / "arena_host"
        build_host("arena_host", [HERE / "native_host/arena_host.c", NATIVE / "src/arena.c", NATIVE / "src/ps4.c"], exe)
        out = subprocess.run([str(exe), "layout"], capture_output=True, text=True, check=True).stdout
        cls.layout = {k: int(v, 16) for k, v in re.findall(r"(\w+) ([0-9a-f]+)", out)}

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def ctx(self, platform: str, fw: str, *extra: str) -> dict[int, int]:
        p = subprocess.run([find_lua(), str(HERE / "boot_fake.lua"), str(self.payload), platform, fw, *extra],
                           capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return {int(o, 16): int(v, 16) & (2**64 - 1) for o, v in re.findall(r"^ctx (\w+) (-?\w+)$", p.stdout, re.M)}

    def test_ps4(self) -> None:
        c = self.ctx("PS4", "12.02")
        L = self.layout
        self.assertEqual(c[8], L["size"])                    # the size covers fw
        self.assertEqual(c[L["flags"]], 2 | 8192)
        self.assertEqual(c[L["fw"]], 0x1202)
        self.assertEqual(c[L["jit_syscall"]], 0x800060107)   # getpid's wrapper + 7
        self.assertEqual(c[L["reserved"] + 16], 0x914104000 + 0x9D4000)

    def test_ps5_unchanged(self) -> None:
        c = self.ctx("PS5", "13.60")
        L = self.layout
        self.assertEqual(c[L["flags"]], 2)
        self.assertEqual(c[L["jit_syscall"]], 0x800050007)
        self.assertEqual(c[L["fw"]], 0x1360)

    def test_ps4_without_gadget(self) -> None:
        """Unexpected bytes after the wrapper's "mov rax, N": no gadget (the
        core then refuses descriptor moves instead of jumping to 0)."""
        self.assertEqual(self.ctx("PS4", "11.00", "no-gadget")[self.layout["jit_syscall"]], 0)


class RunDiagTests(unittest.TestCase):
    def test_diag_only_with_probe(self) -> None:
        p = subprocess.run([sys.executable, str(ROOT / "ghostldr/host/run.py"), "boot", "--diag"],
                           capture_output=True, text=True, timeout=60)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("run.py probe --diag", p.stderr)

    def test_diag_flags(self) -> None:
        import run
        if not run.CORE.exists() or not run.LAYOUT.exists():
            self.skipTest("build the core first (make -C ghostldr/native)")
        sent = []
        argv = ["run.py", "probe", "--diag", "--no-build", "--host", "192.0.2.1", "--wait", "0"]
        with mock.patch.object(sys, "argv", argv), mock.patch.object(run, "recorder_running", return_value=1), \
                mock.patch.object(run, "local_ip_for", return_value="192.0.2.2"), \
                mock.patch.object(run, "send_lua", side_effect=lambda host, payload: sent.append(payload)):
            run.main()
        self.assertEqual(len(sent), 1)
        head = {k.decode(): int(v, 0) for k, v in re.findall(rb"^GHOSTLDR_(FLAGS|EXT_SIZE|SEL_SIZE) = (\w+)$", sent[0], re.M)}
        self.assertEqual(head["FLAGS"] & (1 | 2 | 16384), 1 | 16384)   # probe, diag, nothing released
        self.assertEqual((head["EXT_SIZE"], head["SEL_SIZE"]), (0, 0))


if __name__ == "__main__":
    unittest.main()
