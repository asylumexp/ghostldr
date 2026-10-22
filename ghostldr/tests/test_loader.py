"""The core's module loading (native/src/elf.c, loader.c) built for this
machine (native_host/loader_host.c) on small modules shaped like OpenOrbis
builds: a prx whose e_entry is the interpreter string and whose DT_INIT is
code, and an executable with both relaxed TLS forms lld writes and other
fs:[0] loads; and that the executable's names outlive its file.  With build/ghost/app0 present, Ghost
of Tsushima's modules too; with build/homebrew (tests/homebrew), hbprobe.

    python3 -m unittest test_loader        (from ghostldr/tests)
"""

from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
NATIVE = ROOT / "ghostldr/native"
sys.path.insert(0, str(ROOT / "ghostldr/tools"))
sys.path.insert(0, str(HERE))
import orbis_elf as oe  # noqa: E402
from test_profiles import OPENORBIS_FP, build_host  # noqa: E402

PAGE = 0x4000
INTERP = b"/libexec/ld-elf.so.1".ljust(0x20, b"\0")
LD_FORM = bytes.fromhex("666666" "64488b042500000000") + bytes.fromhex("488d80e8ffffff")   # lld LD->LE (+ lea)
GD_FORM = bytes.fromhex("64488b042500000000" "488d80f8ffffff")                      # lld GD->LE
TID_READ = bytes.fromhex("64488b0c2500000000" "8b5138")                            # musl's thread pointer
GD_POSITIVE = bytes.fromhex("64488b042500000000" "488d8010000000")                  # not a TLS block offset


def module(name: str, text: bytes, entry: int, init: int | None, tls: bool, main: bool) -> bytes:
    """A plain ELF: RX text at 0, RW data at PAGE, PT_DYNAMIC and
    PT_SCE_DYNLIBDATA (empty tables, the module name), optional PT_TLS."""
    strtab = b"\0" + name.encode() + b"\0libSceVideoOut\0"
    data = strtab.ljust(48, b"\0") + OPENORBIS_FP
    dyn = [(oe.DT_SCE_STRTAB, 0), (oe.DT_SCE_STRSZ, len(strtab)), (oe.DT_SCE_SYMTAB, 0), (oe.DT_SCE_SYMTABSZ, 0),
           (oe.DT_SCE_FINGERPRINT, 48), (oe.DT_SCE_MODULE_INFO, 1 | 1 << 40 | 1 << 32),
           (oe.DT_SCE_IMPORT_LIB, (2 + len(name)) | 1 << 32 | 1 << 48)]
    if init is not None:
        dyn.append((oe.DT_INIT, init))
    dyn = b"".join(struct.pack("<qQ", t, v) for t, v in dyn + [(oe.DT_NULL, 0)])
    phdrs = [(oe.PT_LOAD, 5, PAGE, 0, PAGE, PAGE), (oe.PT_LOAD, 6, 2 * PAGE, PAGE, PAGE, PAGE)]
    dyn_off = 3 * PAGE
    data_off = dyn_off + len(dyn)
    phdrs.append((oe.PT_DYNAMIC, 6, dyn_off, 0, len(dyn), len(dyn)))
    phdrs.append((oe.PT_SCE_DYNLIBDATA, 4, data_off, 0, len(data), 0))
    if tls:
        phdrs.append((oe.PT_TLS, 4, PAGE + 0x3000, 0x3000, 0, 0x18))
    ehdr = b"\x7fELF\x02\x01\x01\x09" + bytes(8) + struct.pack(
        "<HHIQQQIHHHHHH", oe.ET_SCE_DYNEXEC if main else oe.ET_SCE_DYNAMIC, 0x3E, 1, entry, 64, 0, 0, 64, 56,
        len(phdrs), 0, 0, 0)
    ph = b"".join(struct.pack("<IIQQQQQQ", t, f, off, va, va, fs, ms, 0x20 if t == oe.PT_TLS else 8)
                  for t, f, off, va, fs, ms in phdrs)
    out = bytearray((ehdr + ph).ljust(PAGE, b"\0"))
    out += text.ljust(PAGE, b"\xcc") + bytes(PAGE)
    return bytes(out) + dyn + data


class LoaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.exe = cls.root / "loader_host"
        build_host("loader_host", [HERE / "native_host/loader_host.c"] +
                   [NATIVE / f"src/{n}.c" for n in ("elf", "loader")], cls.exe)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_host(self, app0: Path, *prx: str) -> list[str]:
        import subprocess
        out = subprocess.run([str(self.exe), str(app0), *prx], capture_output=True, text=True, timeout=120)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        return out.stdout.splitlines()

    def test_openorbis_shaped_modules(self) -> None:
        app0 = self.root / "oo"
        (app0 / "sce_module").mkdir(parents=True)
        # the prx: interpreter string at e_entry (0), "xor eax, eax; ret" at DT_INIT (0x20)
        (app0 / "sce_module/libc.prx").write_bytes(module("libc", INTERP + b"\x31\xc0\xc3", 0, 0x20, False, False))
        text = INTERP.ljust(0x40, b"\x90") + LD_FORM + GD_FORM + TID_READ + GD_POSITIVE
        (app0 / "eboot.bin").write_bytes(module("hb", text, 0x80, 0x90, True, True))
        lines = self.run_host(app0, "libc.prx")
        self.assertTrue([ln for ln in lines if ln.startswith("module libc.prx main 0 entry 0 init 20 start bytes 31c0c3")],
                        lines)
        # every thread-pointer load with displacement 0 reads fs:[-8] (the title's block), lld's forms
        # included; musl's own TCB read goes with them
        self.assertIn("eboot.bin: patched 4 TLS sites (0 other fs: loads left as they are)", lines)
        hexed = next(ln for ln in lines if ln.startswith("main text 40..80 ")).split()[-1]
        patched = bytes.fromhex(hexed)
        fs8 = b"\xf8\xff\xff\xff"
        want = (LD_FORM[:8] + fs8 + LD_FORM[12:] + GD_FORM[:5] + fs8 + GD_FORM[9:] + TID_READ[:5] + fs8 +
                TID_READ[9:] + GD_POSITIVE[:5] + fs8 + GD_POSITIVE[9:])
        self.assertEqual(patched[:len(want)], want)
        # the executable's file is freed once its text is written; its names stay (budget.c reads them)
        self.assertIn("main module hb imports libSceVideoOut ", lines)

    def test_ghost_is_loaded_as_before(self) -> None:
        app0 = ROOT / "build/ghost/app0"
        if not (app0 / "eboot.bin").exists():
            self.skipTest("needs the CUSA11456 dump in build/ghost/app0")
        lines = self.run_host(app0, "libSceFios2.prx", "libc.prx", "libSceNpToolkit2.prx")
        self.assertIn("eboot.bin: patched 1157 TLS sites (1 other fs: loads left as they are)", lines)
        for prx in ("libSceFios2.prx", "libc.prx", "libSceNpToolkit2.prx"):
            self.assertTrue([ln for ln in lines if ln.startswith(f"module {prx} main 0 entry 0 init 0 ")], lines)

    def test_hbprobe(self) -> None:
        app0 = ROOT / "build/homebrew/hbprobe/app0"
        if not (app0 / "eboot.bin").exists():
            self.skipTest("needs build/homebrew (make -C ghostldr/tests/homebrew with the OpenOrbis toolchain)")
        lines = self.run_host(app0)
        sites = [ln for ln in lines if ln.startswith("eboot.bin: patched ")]
        self.assertTrue(sites and int(sites[0].split()[2]) > 0, lines)
        self.assertTrue([ln for ln in lines if "missing 0 unsupported 0" in ln], lines)


if __name__ == "__main__":
    unittest.main()
