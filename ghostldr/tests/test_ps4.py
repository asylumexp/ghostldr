"""PS4 support (docs/ps4.md), offline.

- arena.c and ps4.c in a host process (native_host/arena_host.c) against
  the executable regions a PS5 and a PS4 boot logged and a fake compiler
  process: where the title's modules go, which writes the compiler is asked
  to make, and what happens when it cannot write a buffer;
- jitrpc.c in a host process (native_host/jitrpc_host.c) with a fake
  compiler thread and a real socket pair: the PS4 probes and checks end,
  whatever the compiler does (the second PS4 report stopped in them);
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

    def test_buffers_taken_by_bigexec_leave_the_arena(self) -> None:
        """bigexec.c: the whole buffers past the core's; once the EE and VU1
        buffers hold an executable's code, libc and Fios2 go to the IOP one."""
        out = self.run_host("ps5", "drop")
        self.assertEqual(re.findall(r"buffer \+(\w+)-\+(\w+)", out),
                         [("48000", "2048000"), ("204c000", "284c000"), ("2850000", "3050000"), ("3054000", "4054000")])
        self.assertIn("left 2", out)
        self.assertIn("code region 0 +2050000-+284c000", out)
        self.assertEqual(offsets(out), {"libSceFios2.prx": 0x205c000, "libc.prx": 0x21ec000})  # 902a30000 on 13.60

    def test_a_fresh_object_after_a_reclaim(self) -> None:
        """bigexec.c's reclaim: no buffer left; the fresh object mapped where
        EE was is the code region, at the same addresses, and the compiler
        writes it at its own address (the PS4 path checks each write here)."""
        for model in ("ps5", "ps4-header"):
            out = self.run_host(model, "adopt")
            self.assertIn("adopt 0, buffers left 0", out)
            first = 0x4c000 if model == "ps5" else EE + 0x10000             # as without a reclaim
            self.assertIn("code region 0 +4c000-+1450000", out)
            self.assertNotIn("FAULT", out)
            self.assertIn(f"module libSceFios2.prx at +{first:x} rx 1 write 0", out)
            self.assertEqual(out.count("compiler bytes written"), 2, out)

    def test_the_extension_in_a_buffer(self) -> None:
        """ext.c without a new JIT block: the top of VU0 (the smaller buffers'
        highest), which leaves the arena; the same place a second time."""
        out = self.run_host("ps5", "exttop")
        self.assertIn(f"ext at +{0x3050000 - 0xa8000:x}, again +{0x3050000 - 0xa8000:x}", out)     # VU0's top
        self.assertEqual(re.findall(r"buffer \+(\w+)-\+(\w+)", out),
                         [("48000", "2048000"), ("204c000", "284c000"), ("3054000", "4054000")])
        self.assertIn("ext write 0", out)
        self.assertIn("ext at +0, again +0", self.run_host("ps4-header", "exttop"))     # a PS4 keeps its JIT block

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

    def test_ps4_reported_layout(self) -> None:
        """The second PS4 report (12.02): core at 914adc000 (IPU+16 KiB, text
        176 KiB, data to 914b14000), small area 914b14000-914b1c000.  The
        same regions as that log, the compiler checks before any probe, no
        probe in the core's text or data, and Ghost placed as before."""
        out = self.run_host("ps4-report")
        ipu = int(re.search(r"^ipu ([0-9a-f]+)$", out, re.M)[1], 16)
        rel = lambda m: (int(m[0], 16) - ipu, int(m[1], 16) - ipu)  # noqa: E731
        regions = [rel(m) for m in re.findall(r"arena: RX ([0-9a-f]+)-([0-9a-f]+)", out)]
        self.assertEqual(regions[0], (0x4000, 0x30000))           # 914adc000-914b08000 (176 KiB)
        self.assertEqual(regions[1], (EE, EE + 0x2000000))        # 914b20000-916b20000
        self.assertEqual(rel(re.search(r"arena: small area ([0-9a-f]+)-([0-9a-f]+)", out).groups()),
                         (0x3c000, 0x44000))                       # 914b14000-914b1c000
        self.assertLess(out.index("compiler check ok"), out.index("probe "))
        self.assertLess(out.index("holds the core"), out.index("probe "))
        probes = sorted({int(a, 16) - ipu for a in re.findall(r"probe ([0-9a-f]+):", out)})
        self.assertEqual(probes[0], 0x3c000)                       # the lowest: the small area (probed last)
        self.assertIn(0x43ff0, probes)
        self.assertIn(EE + 0x4000, probes)
        self.assertFalse([a for a in probes if a < 0x3c000])       # nothing in the core's text or data
        self.assertEqual(offsets(out), {"libSceFios2.prx": EE + 0x10000, "libc.prx": EE + 0x1a0000,
                                        "libSceNpToolkit2.prx": EE + 0x2c0000, "eboot.bin": EE + 0x380000})
        self.assertIn("stubs at +3c000 write 0", out)

    def test_ps4_failed_check_writes_nothing(self) -> None:
        """When the probes do not work on known memory, no buffer is probed,
        nothing is written through the compiler and arena_init fails."""
        out = self.run_host("ps4-nocheck")
        self.assertIn("compiler check FAILED", out)
        self.assertIn("the boot stops here", out)
        self.assertIn("init -1", out)
        self.assertNotIn("probe ", out)
        self.assertNotIn("module ", out)

    def test_heap_kept_only_while_used(self) -> None:
        self.assertIn("heap 1 0 0 0", self.run_host("heap"))


class JitRpcTests(unittest.TestCase):
    """jitrpc.c against a fake compiler thread: nothing in the PS4 probes can
    block for good (the second PS4 report's log ended at the first probe)."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name) / "jitrpc_host"
        build_host("jitrpc_host", [HERE / "native_host/jitrpc_host.c", NATIVE / "src/jitrpc.c"], cls.exe)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_host(self, mode: str) -> str:
        p = subprocess.run([str(self.exe), mode], capture_output=True, text=True, timeout=30)
        if p.returncode == 77:
            self.skipTest(p.stdout.strip())
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return p.stdout

    def test_working_compiler(self) -> None:
        out = self.run_host("ok")
        self.assertIn("check 0", out)
        self.assertIn("peek 0 same", out)
        self.assertIn("stamp 0", out)
        self.assertIn("refused, as expected", out)
        self.assertNotIn("killed", out)

    def test_one_stub_for_all_numbers_kills_the_compiler(self) -> None:
        """v0.6.0 called every number through getpid's stub: the PS4's
        compiler stopped at check 2/4 (the third report).  The fake does the same."""
        out = self.run_host("one-stub")
        self.assertIn("compiler pid 4242", out)
        self.assertIn("system call 232 through the stub of 20: killed", out)
        self.assertIn("giving up on it", out)
        self.assertIn("check -1", out)

    def test_each_number_gets_its_own_stub(self) -> None:
        out = self.run_host("ok")
        self.assertRegex(out, r"stubs: getpid 5160140, clock_gettime 5160e80, write 5160040, sendmsg 51601c0")

    def test_missing_stubs_call_nothing(self) -> None:
        for mode, nr in (("nostubs", 20), ("noclock", 232)):
            with self.subTest(mode=mode):
                out = self.run_host(mode)
                self.assertIn(f"no libkernel stub for system call {nr} in the compiler: not called", out)
                self.assertIn("check -1", out)
                self.assertNotIn("killed", out)
                self.assertNotIn("giving up", out)

    def test_stale_bytes_are_dropped(self) -> None:
        out = self.run_host("stale")
        self.assertIn("dropped 29 stale bytes", out)
        self.assertIn("check 0", out)
        self.assertIn("peek 0 same", out)

    def test_bytes_that_never_arrive(self) -> None:
        """write() succeeds in the compiler but nothing reaches this end: the
        read used to block forever; now the check and the peek fail."""
        for mode in ("lost", "pipe"):
            with self.subTest(mode=mode):
                out = self.run_host(mode)
                self.assertIn("check -1", out)
                self.assertIn("0 arrived here", out)
                self.assertIn("peek -1", out)

    def test_compiler_that_never_answers(self) -> None:
        out = self.run_host("hang")
        self.assertIn("no answer from the compiler after 2 s", out)
        self.assertIn("giving up on it", out)
        self.assertIn("check -1", out)
        self.assertIn("skipped: the compiler stopped answering earlier", out)
        self.assertIn("stamp -1", out)

    def test_slow_compiler_is_waited_for(self) -> None:
        out = self.run_host("slow")
        self.assertIn("check 0", out)
        self.assertNotIn("giving up", out)

    def test_ps5_never_reads_the_clock(self) -> None:
        out = self.run_host("ps5-slow")
        self.assertIn("check 0", out)
        self.assertIn("clock reads 0", out)
        self.assertNotIn("no answer", out)


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

    def run_fake(self, platform: str, fw: str, *extra: str) -> str:
        p = subprocess.run([find_lua(), str(HERE / "boot_fake.lua"), str(self.payload), platform, fw, *extra],
                           capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return p.stdout

    def ctx(self, platform: str, fw: str, *extra: str) -> dict[int, int]:
        out = self.run_fake(platform, fw, *extra)
        return {int(o, 16): int(v, 16) & (2**64 - 1) for o, v in re.findall(r"^ctx (\w+) (-?\w+)$", out, re.M)}

    def stubs(self, platform: str, fw: str, *extra: str) -> dict[int, int]:
        out = self.run_fake(platform, fw, *extra)
        return {int(n, 16): int(a, 16) for n, a in re.findall(r"^stub (\w+) (\w+)$", out, re.M)}

    def test_ps4(self) -> None:
        c = self.ctx("PS4", "12.02")
        L = self.layout
        self.assertEqual(c[8], L["size"])                    # the size covers fw
        self.assertEqual(c[L["flags"]], 2 | 8192)
        self.assertEqual(c[L["fw"]], 0x1202)
        self.assertEqual(c[L["jit_syscall"]], 0x800060107)   # getpid's wrapper + 7
        self.assertEqual(c[L["reserved"] + 16], 0x914104000 + 0x9D4000)
        self.assertNotEqual(c[L["jit_sysw"]], 0)

    def test_ps4_stubs(self) -> None:
        """Each number the core calls in the compiler, with its own wrapper:
        Luac0re's table, and clock_gettime found past it in libkernel."""
        self.assertEqual(self.layout["jit_sysw"], 0x30)     # where boot_fake.lua reads the table
        self.assertEqual(self.stubs("PS4", "12.02"), {
            0x14: 0x800060100, 0x1c: 0x800060140, 0x4: 0x800060180, 0x6: 0x8000601c0,
            0x49: 0x800060200, 0x61: 0x800060240, 0xe8: 0x800060000 + 0x24000,
            # the diagnostic's direct memory checks in the compiler: open, ioctl, dmem_container
            0x5: 0x800060280, 0x36: 0x8000602c0, 0x24a: 0x800060300,
        })

    def test_memory_hints(self) -> None:
        """A PS4 boot stopped for lack of memory gets a plain-language hint."""
        import run
        stop = ("budget: PS4: CUSA11456 needs about 1592 MiB more direct memory than this PS4 lets SWRR "
                "allocate: not started (docs/ps4.md)")
        self.assertIn("2.5 GiB", run.hint_for(stop))
        self.assertIn("On a PS5", run.hint_for(stop))
        warn = ("budget: PS4: the title's plan needs about 172 MiB more direct memory than this console hands "
                "out; it may fail when it allocates the rest")
        self.assertIn("2.5 GiB", run.hint_for(warn))
        self.assertIsNone(run.hint_for("budget: PS4: the kernel hands out 2480 MiB of direct memory (type 0)"))

    def test_ps4_without_search(self) -> None:
        """No module info: only Luac0re's table, so no clock_gettime."""
        s = self.stubs("PS4", "12.02", "no-search")
        self.assertNotIn(0xe8, s)
        self.assertEqual(s[0x14], 0x800060100)

    def test_ps5_unchanged(self) -> None:
        c = self.ctx("PS5", "13.60")
        L = self.layout
        self.assertEqual(c[L["flags"]], 2)
        self.assertEqual(c[L["jit_syscall"]], 0x800050007)
        self.assertEqual(c[L["fw"]], 0x1360)
        self.assertEqual(c[L["jit_sysw"]], 0)

    def test_ps4_without_gadget(self) -> None:
        """Unexpected bytes after the wrapper's "mov rax, N": no gadget (the
        core then refuses descriptor moves instead of jumping to 0), and no
        getpid stub; the other numbers keep theirs."""
        self.assertEqual(self.ctx("PS4", "11.00", "no-gadget")[self.layout["jit_syscall"]], 0)
        s = self.stubs("PS4", "11.00", "no-gadget")
        self.assertNotIn(0x14, s)
        self.assertEqual(s[0x1c], 0x800060140)


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
                mock.patch.object(run, "send_lua", side_effect=lambda host, payload, port: sent.append(payload)):
            run.main()
        self.assertEqual(len(sent), 1)
        head = {k.decode(): int(v, 0) for k, v in re.findall(rb"^GHOSTLDR_(FLAGS|EXT_SIZE|SEL_SIZE) = (\w+)$", sent[0], re.M)}
        self.assertEqual(head["FLAGS"] & (1 | 2 | 16384), 1 | 16384)   # probe, diag, nothing released
        self.assertEqual((head["EXT_SIZE"], head["SEL_SIZE"]), (0, 0))


if __name__ == "__main__":
    unittest.main()
