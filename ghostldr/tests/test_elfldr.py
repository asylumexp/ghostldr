"""The ELF loader (docs/elfldr.md), offline.

- payload.c (with elf.c for Orbis tables) built for this machine
  (native_host/payload_host.c) on ELFs made here: the formats it takes
  (an ELF with its own dynamic section, a freestanding one, Orbis tables in
  an ELF or a fake-signed SELF), every relocation type it applies (text
  relocations and DT_RELR included), the TLS sites it patches, the imports
  it asks the resolver for and how (function, PLT slot, variable, weak), and
  each refusal with its reason;
- the sample payloads in tests/elfldr: freestanding.elf is built when
  LLVM is there; hello.elf, echo.elf and hello.self when the OpenOrbis
  toolchain is (OO_PS4_TOOLCHAIN) or build/elfldr has them;
- the service, elfldr.c, built for this machine over the FTP harness's
  POSIX environment and a fake loader that runs nothing
  (ext_native/elfldr_host.c, elfldr_fake.c): payloads sent the way `nc`
  sends them (no half-close) and with one, their length read from their
  headers (ELF and SELF), output and the last line, commands, refusals,
  two payloads at once and a third turned away, a client that sends
  nothing, one that leaves early, quitting; and host/elf_send.py against it.

    python3 -m unittest test_elfldr        (from ghostldr/tests)
"""

from __future__ import annotations

import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
NATIVE = ROOT / "ghostldr/native"
sys.path.insert(0, str(ROOT / "ghostldr/tools"))
sys.path.insert(0, str(ROOT / "ghostldr/host"))
sys.path.insert(0, str(HERE))
import orbis_build as ob  # noqa: E402
import orbis_elf as oe  # noqa: E402
from test_profiles import host_cc  # noqa: E402

BASE = 0x2000000000
PAGE = 0x4000
R_64, R_PC32, R_GLOB_DAT, R_JUMP_SLOT, R_RELATIVE = 1, 2, 6, 7, 8
R_DTPMOD64, R_DTPOFF64, R_TPOFF64, R_IRELATIVE = 16, 17, 18, 37
STT_NOTYPE, STT_OBJECT, STT_FUNC, STT_TLS = 0, 1, 2, 6
STB_GLOBAL, STB_WEAK = 1, 2
FS0 = bytes.fromhex("64488b042500000000")      # mov rax, fs:[0]
FS0_PATCHED = bytes.fromhex("64488b0425f8ffffff")
SAMPLES = ROOT / "build/elfldr"
LLVM = Path(os.environ.get("LLVM", "/opt/homebrew/opt/llvm@18/bin"))


def gnu_elf(text: bytes = b"\xc3", *, needed=(), imports=(), defined=(), relas=(), jmprels=(), tls=None,
            e_type=3, entry=0x1000, flags=(5, 6), data_vaddr=PAGE, dynamic=True, relr=(), extra_dyn=(),
            data=b"", cls=2, machine=0x3E, extra_phdrs=()) -> bytes:
    """An ELF laid out as ld.lld lays out a PIE, file offset = address: the
    headers and text (R E) in the first 16 KiB, then 16 KiB of data (RW):
    .dynamic at +0, .dynsym +0x400, .dynstr +0x800, .rela.dyn +0x1000,
    .rela.plt +0x1800, packed relocations +0x2000, free from +0x3000.
    imports: (name, type, bind); defined: (name, value, type); relocations:
    (offset, type, symbol name or None, addend)."""
    names = [n for n, _, _ in imports] + [n for n, _, _ in defined]
    strtab = bytearray(b"\0")

    def s(text: str) -> int:
        strtab.extend(text.encode() + b"\0")
        return len(strtab) - len(text) - 1

    needed_off = [s(n) for n in needed]
    syms = [bytes(24)]
    for name, typ, bind in imports:
        syms.append(struct.pack("<IBBHQQ", s(name), (bind << 4) | typ, 0, 0, 0, 0))
    for name, value, typ in defined:
        syms.append(struct.pack("<IBBHQQ", s(name), (STB_GLOBAL << 4) | typ, 0, 1, value, 8))

    def idx(name):
        return 0 if name is None else 1 + names.index(name)

    rela = b"".join(struct.pack("<QQq", o, (idx(n) << 32) | t, a) for o, t, n, a in relas)
    jmp = b"".join(struct.pack("<QQq", o, (idx(n) << 32) | R_JUMP_SLOT, 0) for o, n in jmprels)
    packed = b"".join(struct.pack("<Q", w) for w in relr)
    d = data_vaddr
    dyn = [(oe.DT_NEEDED, o) for o in needed_off]
    dyn += [(5, d + 0x800), (10, len(strtab)), (6, d + 0x400), (11, 24)]
    if rela:
        dyn += [(7, d + 0x1000), (8, len(rela)), (9, 24)]
    if jmp:
        dyn += [(23, d + 0x1800), (2, len(jmp)), (20, 7)]
    if packed:
        dyn += [(36, d + 0x2000), (35, len(packed))]
    dyn += list(extra_dyn) + [(0, 0)]
    dyn_blob = b"".join(struct.pack("<QQ", t, v) for t, v in dyn)
    seg = bytearray(data.ljust(PAGE, b"\0"))
    if dynamic:
        seg[0:len(dyn_blob)] = dyn_blob
        seg[0x400:0x400 + len(syms) * 24] = b"".join(syms)
        seg[0x800:0x800 + len(strtab)] = strtab
        seg[0x1000:0x1000 + len(rela)] = rela
        seg[0x1800:0x1800 + len(jmp)] = jmp
        seg[0x2000:0x2000 + len(packed)] = packed
    phdrs = [(1, flags[0], 0, 0, PAGE, PAGE, PAGE), (1, flags[1], d, d, PAGE, PAGE, PAGE)]
    if dynamic:
        phdrs.append((2, 6, d, d, len(dyn_blob), len(dyn_blob), 8))
    if tls:
        va, filesz, memsz, align = tls
        phdrs.append((7, 4, va, va, filesz, memsz, align))
    phdrs += list(extra_phdrs)
    ehdr = b"\x7fELF" + bytes([cls, 1, 1, 9]) + bytes(8) + struct.pack(
        "<HHIQQQIHHHHHH", e_type, machine, 1, entry, 0x40, 0, 0, 0x40, 0x38, len(phdrs), 0, 0, 0)
    head = ehdr + b"".join(struct.pack("<IIQQQQQQ", t, f, o, va, va, fs, ms, al) for t, f, o, va, fs, ms, al in phdrs)
    image = bytearray(head.ljust(0x1000, b"\0") + text.ljust(PAGE - 0x1000, b"\xcc"))
    if d > PAGE:
        image = image.ljust(d, b"\0")
    elif d < PAGE:
        image = image[:d]
    return bytes(image) + bytes(seg)


def self_wrap(elf: bytes, props: int = 0) -> bytes:
    """A fake-signed SELF around an ELF: one plain entry per PT_LOAD, the
    ELF's headers after the entries, each segment's bytes at its entry's
    offset."""
    phoff, = struct.unpack_from("<Q", elf, 0x20)
    phnum, = struct.unpack_from("<H", elf, 0x38)
    ph = [struct.unpack_from("<IIQQQQQQ", elf, phoff + i * 56) for i in range(phnum)]
    loads = [i for i, p in enumerate(ph) if p[0] in (1, 2, oe.PT_SCE_RELRO, oe.PT_SCE_DYNLIBDATA) and p[5]]
    head_len = 0x20 + 0x20 * len(loads)
    elf_head = elf[:phoff + phnum * 56]
    body = bytearray()
    entries = []
    at = (head_len + len(elf_head) + 0xF) & ~0xF
    for i in loads:
        off, filesz = ph[i][2], ph[i][5]
        entries.append(struct.pack("<QQQQ", 0x800 | props | (i << 20), at + len(body), filesz, filesz))
        body += elf[off:off + filesz]
        body += bytes(-len(body) % 16)
    total = at + len(body)
    hdr = oe.SELF_MAGIC + bytes([0, 1, 1, 0x12]) + struct.pack("<IHHQHH4x", 0x101, head_len, 0, total, len(loads), 0x22)
    out = bytearray(hdr + b"".join(entries) + elf_head)
    out = out.ljust(at, b"\0") + body
    return bytes(out)


def build(out: Path, sources: list[Path], freestanding: list[Path] = (), defines: tuple[str, ...] = ()) -> None:
    cc = host_cc()
    if not cc:
        raise unittest.SkipTest("no C compiler whose programs run on this machine")
    warn = ["-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]
    inc = [f"-I{NATIVE / 'include'}", f"-I{NATIVE / 'ext/include'}"]
    objs = []
    for src in freestanding:
        obj = out.parent / (src.stem + ".o")
        subprocess.run(cc + warn + ["-ffreestanding", "-fno-builtin", *inc, *defines, "-c", str(src), "-o", str(obj)],
                       check=True)
        objs.append(str(obj))
    subprocess.run(cc + warn + inc + [*map(str, sources), *objs, "-o", str(out)], check=True)


class PayloadFormats(unittest.TestCase):
    """payload.c: what it takes, what it refuses, and the image it builds."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.exe = cls.root / "payload_host"
        build(cls.exe, [], [HERE / "native_host/payload_host.c", NATIVE / "ext/src/payload.c",
                            NATIVE / "src/elf.c"])
        cls.n = 0

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_host(self, data: bytes | Path) -> tuple[list[str], bytes]:
        if isinstance(data, Path):
            path = data
        else:
            PayloadFormats.n += 1
            path = self.root / f"p{self.n}.elf"
            path.write_bytes(data)
        img = self.root / "image.bin"
        img.unlink(missing_ok=True)
        r = subprocess.run([str(self.exe), str(path), f"{BASE:x}", str(img)], capture_output=True, text=True,
                           timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout.splitlines(), img.read_bytes() if img.exists() else b""

    def refused(self, data: bytes, why: str) -> None:
        lines, _ = self.run_host(data)
        self.assertTrue(lines and lines[0].startswith("parse fail: ") or
                        [ln for ln in lines if ln.startswith("build fail: ")], lines)
        self.assertIn(why, "\n".join(lines))

    @staticmethod
    def word(img: bytes, off: int) -> int:
        return struct.unpack_from("<Q", img, off)[0]

    def test_freestanding_without_dynamic_section(self) -> None:
        lines, img = self.run_host(gnu_elf(b"\x31\xc0\xc3", dynamic=False))
        self.assertEqual(lines[0], "parse ok abi free sce 0 exec_end 4000 image 8000 relro 0-0 tls 0/0/0 entry 1000")
        self.assertEqual(lines[-1], 'build ok tls_offset 0 tls_id 1000 info ""')
        self.assertEqual(img[0x1000:0x1003], b"\x31\xc0\xc3")
        self.assertEqual(len(img), 0x8000)

    def test_imports_relocations_and_tls(self) -> None:
        d = PAGE
        text = b"\x90" * 0x10 + FS0 + b"\xc3"
        elf = gnu_elf(
            text, needed=("libkernel.so", "libSceVideoOut.so"),
            imports=[("sceKernelUsleep", STT_FUNC, STB_GLOBAL), ("missing_fn", STT_FUNC, STB_WEAK),
                     ("dataObj", STT_OBJECT, STB_GLOBAL), ("plainFn", STT_FUNC, STB_GLOBAL)],
            defined=[("tlsVar", 4, STT_TLS), ("localFn", 0x1000, STT_FUNC)],
            relas=[(d + 0x3000, R_RELATIVE, None, 0x1234),
                   (d + 0x3008, R_64, "dataObj", 8),
                   (d + 0x3010, R_GLOB_DAT, "sceKernelUsleep", 0),
                   (d + 0x3018, R_GLOB_DAT, "missing_fn", 0),
                   (d + 0x3020, R_64, "localFn", 0x10),
                   (d + 0x3028, R_DTPMOD64, "tlsVar", 0),
                   (d + 0x3030, R_DTPOFF64, "tlsVar", 2),
                   (d + 0x3038, R_TPOFF64, "tlsVar", 0),
                   (0x1100, R_RELATIVE, None, 0x40),          # a text relocation
                   (d + 0x3040, R_PC32, "localFn", 0)],
            jmprels=[(d + 0x3048, "plainFn")],
            tls=(d + 0x3800, 4, 0x24, 8))
        lines, img = self.run_host(elf)
        self.assertEqual(lines[0], "parse ok abi orbis sce 0 exec_end 4000 image 8000 relro 0-0 tls 4/24/8 entry 1000")
        self.assertEqual(lines[1:3], ["needed libkernel.so", "needed libSceVideoOut.so"])
        imports = [ln for ln in lines if ln.startswith("import ")]
        # IMPORT_DATA 0 for a function's address, IMPORT_OBJECT 2 for a variable, IMPORT_PLT 1 for a PLT slot
        self.assertEqual(imports, ["import dataObj weak 0 kind 2 -> 7000000000",
                                   "import sceKernelUsleep weak 0 kind 0 -> 7000000010",
                                   "import missing_fn weak 1 kind 0 -> 0",
                                   "import plainFn weak 0 kind 1 -> 7000000020"])
        self.assertEqual(lines[-1], 'build ok tls_offset 30 tls_id 1000 info "1 TLS sites"')
        w = lambda off: self.word(img, off)  # noqa: E731
        self.assertEqual(w(d + 0x3000), BASE + 0x1234)
        self.assertEqual(w(d + 0x3008), 0x7000000008)
        self.assertEqual(w(d + 0x3010), 0x7000000010)
        self.assertEqual(w(d + 0x3018), 0)
        self.assertEqual(w(d + 0x3020), BASE + 0x1010)
        self.assertEqual(w(d + 0x3028), 0x1000)                       # the payload's TLS id
        self.assertEqual(w(d + 0x3030), 6)                            # value + addend in its block
        self.assertEqual(w(d + 0x3038), (4 - 0x30) & (2 ** 64 - 1))   # below the thread pointer (variant II)
        self.assertEqual(w(0x1100), BASE + 0x40)
        self.assertEqual(struct.unpack_from("<i", img, d + 0x3040)[0], 0x1000 - (d + 0x3040))
        self.assertEqual(w(d + 0x3048), 0x7000000020)
        self.assertEqual(img[0x1010:0x1019], FS0_PATCHED)

    def test_packed_relocations(self) -> None:
        d = PAGE
        data = bytearray(PAGE)
        for k in range(8):
            struct.pack_into("<Q", data, 0x3000 + 8 * k, 0x100 + k)
        relr = [d + 0x3000, (0b1011 << 1) | 1]   # +0, then +8, +0x10 and +0x20 from the bitmap
        lines, img = self.run_host(gnu_elf(relr=relr, data=bytes(data)))
        self.assertTrue(lines[-1].startswith("build ok"), lines)
        got = [self.word(img, d + 0x3000 + 8 * k) for k in range(6)]
        self.assertEqual(got, [BASE + 0x100, BASE + 0x101, BASE + 0x102, 0x103, BASE + 0x104, 0x105])

    def test_tls_sites_only_with_tls(self) -> None:
        lines, img = self.run_host(gnu_elf(b"\x90" + FS0 + b"\xc3", dynamic=False))
        self.assertEqual(img[0x1001:0x100a], FS0)

    def test_orbis_tables_in_an_elf_and_a_self(self) -> None:
        m = ob.Module("hb", e_type=oe.ET_SCE_DYNEXEC, text=b"\x90" * 0x20 + FS0 + b"\xc3", entry=0x20,
                      import_libs={2: "libkernel"}, needed_mods={2: "libkernel"},
                      imports=[("sceKernelUsleep", 2, 2, STT_FUNC), ("getpid", 2, 2, STT_FUNC)],
                      relas=[(PAGE + 0x10, R_RELATIVE, 0, 0x20), (PAGE + 0x18, R_GLOB_DAT, 2, 0),
                             (PAGE + 0x20, R_TPOFF64, 0, 4)],
                      jmprels=[(PAGE + 0x28, 1)], tls=(PAGE + 0x100, 8, 0x10, 16), data=bytes(0x100) + b"TLSINIT!")
        elf = m.build()
        for data in (elf, self_wrap(elf)):
            lines, img = self.run_host(data)
            self.assertEqual(lines[0], "parse ok abi orbis sce 1 exec_end 4000 image 8000 relro 0-0 tls 8/10/10 "
                                       "entry 20")
            nid = oe.compute_nid
            self.assertEqual([ln for ln in lines if ln.startswith("import ")],
                             [f"import {nid('getpid')}#C#C weak 0 kind 0 -> 7000000000",
                              f"import {nid('sceKernelUsleep')}#C#C weak 0 kind 1 -> 7000000010"])
            self.assertEqual(lines[-1], 'build ok tls_offset 10 tls_id 1000 info "1 TLS sites"')
            self.assertEqual(self.word(img, PAGE + 0x10), BASE + 0x20)
            self.assertEqual(self.word(img, PAGE + 0x18), 0x7000000000)
            self.assertEqual(self.word(img, PAGE + 0x20), (4 - 0x10) & (2 ** 64 - 1))
            self.assertEqual(self.word(img, PAGE + 0x28), 0x7000000010)
            self.assertEqual(img[PAGE + 0x100:PAGE + 0x108], b"TLSINIT!")
            self.assertEqual(img[0x20:0x29], FS0_PATCHED)

    def test_refusals(self) -> None:
        cases = [
            (gnu_elf(e_type=2), "linked at a fixed address (ET_EXEC)"),
            (gnu_elf(e_type=oe.ET_SCE_EXEC), "linked at a fixed address (ET_EXEC)"),
            (gnu_elf(e_type=1), "an object file (ET_REL)"),
            (gnu_elf(e_type=oe.ET_SCE_DYNAMIC), "a library (an Orbis prx), not a program"),
            (gnu_elf(cls=1), "not a 64-bit little-endian x86-64 ELF"),
            (gnu_elf(machine=0xB7), "not a 64-bit little-endian x86-64 ELF (class 2, data 1, machine 183)"),
            (b"\x00" * 100, "not an ELF"),
            (b"\x7fELF", "only 4 bytes: not an ELF"),
            (gnu_elf(flags=(7, 6)), "segment 0 is both writable and executable"),
            (gnu_elf(data_vaddr=0x2000), "writable data at 0x2000 shares the code's 16 KiB pages"),
            (gnu_elf(needed=("libkernel_web.sprx", "libSceLibcInternal.sprx")), "built with ps5-payload-sdk"),
            (gnu_elf(flags=(7, 6), needed=("libkernel_web.sprx",)), "built with ps5-payload-sdk (it needs libkernel_web"),
            (gnu_elf(extra_dyn=[(17, 0)]), "REL relocations; x86-64 payloads use RELA"),
            (gnu_elf(relas=[(PAGE + 0x3000, R_IRELATIVE, None, 0x1000)]), "an ifunc (IRELATIVE)"),
            (gnu_elf(relas=[(PAGE + 0x3000, 99, None, 0)]), "has type 99, which this loader does not apply"),
            (gnu_elf(relas=[(0x9000, R_RELATIVE, None, 0)]), "outside the image"),
            (gnu_elf(tls=(PAGE, 0, 0x10000, 8)), "its TLS (0x10000 bytes"),
            (gnu_elf(entry=0x5000), "its entry point 0x5000 is not in an executable segment"),
            (gnu_elf(flags=(4, 6)), "it has no executable segment"),
            (gnu_elf(relas=[(PAGE + 0x3000, R_DTPMOD64, None, 0)]), "without TLS"),
            (gnu_elf(extra_phdrs=[(1, 5, 0, 0x2000000, PAGE, PAGE, PAGE)]), "at most 32736 KiB fit one JIT object"),
            (gnu_elf(extra_phdrs=[(1, 4, 0x100000, 0x10000, 0x100, 0x100, PAGE)]), "runs past the end of the file"),
        ]
        for data, why in cases:
            with self.subTest(why=why):
                self.refused(data, why)
        elf = ob.Module("hb", e_type=oe.ET_SCE_DYNEXEC, text=b"\xc3").build()
        self.refused(self_wrap(elf, props=0x2), "a SELF with encrypted or compressed segments")

    def test_samples(self) -> None:
        """tests/elfldr's payloads: freestanding.elf whenever LLVM is here,
        the OpenOrbis ones when the toolchain is (or they are built)."""
        out = self.root / "samples"
        made = []
        if (LLVM / "clang").exists() and (LLVM / "ld.lld").exists():
            r = subprocess.run(["make", "-s", "-C", str(HERE / "elfldr"), f"LLVM={LLVM}", f"OUT={out}", "freestanding"],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            made.append("freestanding")
        tc = os.environ.get("OO_PS4_TOOLCHAIN")
        if tc and (LLVM / "clang").exists():
            r = subprocess.run(["make", "-s", "-C", str(HERE / "elfldr"), f"LLVM={LLVM}", f"OUT={out}",
                                f"OO_PS4_TOOLCHAIN={tc}", "all"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        elif all((SAMPLES / f"{n}.elf").exists() for n in ("hello", "echo")):
            for n in ("hello", "echo"):
                shutil.copy(SAMPLES / f"{n}.elf", out / f"{n}.elf")
        if not made:
            self.skipTest("needs LLVM 18 (clang, ld.lld) for the freestanding sample")
        lines, _ = self.run_host(out / "freestanding.elf")
        self.assertTrue(lines[0].startswith("parse ok abi free sce 0 exec_end 8000 "), lines)
        self.assertFalse([ln for ln in lines if ln.startswith("import ")], lines)
        if not (out / "hello.elf").exists():
            return
        for name, want in (("hello", {"_write", "_writev", "_exit", "pthread_create", "mmap", "munmap"}),
                           ("echo", {"__sys_socketex", "bind", "listen", "accept", "recvfrom", "sendto", "close"})):
            lines, _ = self.run_host(out / f"{name}.elf")
            self.assertTrue(lines[0].startswith("parse ok abi orbis sce 0 "), lines)
            self.assertIn("needed libkernel.so", lines)
            got = {ln.split()[1] for ln in lines if ln.startswith("import ")}
            self.assertLessEqual(want, got)
            self.assertRegex(lines[-1], r'^build ok tls_offset (10|20) tls_id 1000 info "[1-9]\d* TLS sites"$')
        if (SAMPLES / "hello.self").exists():
            lines, _ = self.run_host(SAMPLES / "hello.self")
            self.assertTrue(lines[0].startswith("parse ok abi orbis sce 1 "), lines)
            self.assertTrue(lines[-1].startswith("build ok "), lines)


def elf_with_script(script: str, **kw) -> bytes:
    """A freestanding ELF the fake loader acts out (elfldr_fake.c)."""
    return gnu_elf(b"\xc3" + b"ELFLDR-TEST:" + script.encode() + b"\n", dynamic=bool(kw.get("needed")), **kw)


class Service(unittest.TestCase):
    """elfldr.c over real sockets, with a loader that runs nothing."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.exe = cls.root / "elfldr_host"
        build(cls.exe, [HERE / "ext_native/elfldr_host.c"],
              [NATIVE / "ext/src/elfldr.c", NATIVE / "ext/src/payload.c", NATIVE / "src/elf.c",
               HERE / "ext_native/elfldr_fake.c"], ("-DEL_TIMEOUT_S=2",))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def setUp(self) -> None:
        self.proc = subprocess.Popen([str(self.exe), "serve"], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True)
        line = self.proc.stdout.readline()
        self.assertTrue(line.startswith("listening "), line)
        self.port = int(line.split()[1])

    def tearDown(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait(10)
        self.proc.stdout.close()
        self.proc.stderr.close()

    def connect(self) -> socket.socket:
        for _ in range(50):
            try:
                return socket.create_connection(("127.0.0.1", self.port), timeout=10)
            except ConnectionRefusedError:
                time.sleep(0.05)
        raise AssertionError("the service does not listen")

    def talk(self, data: bytes, half_close: bool = False) -> list[str]:
        """send data, then read until the service closes the connection"""
        with self.connect() as s:
            s.sendall(data)
            if half_close:
                s.shutdown(socket.SHUT_WR)
            buf = b""
            while chunk := s.recv(65536):
                buf += chunk
        return buf.decode("utf-8", "replace").splitlines()

    def test_payload_sent_as_nc_sends_it(self) -> None:
        """no half-close: the ELF's own headers say where it ends"""
        lines = self.talk(elf_with_script("out=hello|world|;sleep=200;code=7;"))
        self.assertEqual(lines, ["[elfldr] payload 1: ELF (freestanding), 16 KiB of code, 16 KiB of data",
                                 "hello", "world", "[elfldr] payload 1 exited with code 7"])

    def test_payload_with_half_close_and_a_self(self) -> None:
        lines = self.talk(elf_with_script("code=3;", needed=("libkernel.so",)), half_close=True)
        self.assertEqual(lines[0], "[elfldr] payload 1: ELF (OpenOrbis/PS4 ABI), 16 KiB of code, 16 KiB of data")
        self.assertEqual(lines[-1], "[elfldr] payload 1 exited with code 3")
        orbis = ob.Module("hb", e_type=oe.ET_SCE_DYNEXEC, text=b"\xc3ELFLDR-TEST:code=4;out=self|;\n").build()
        lines = self.talk(self_wrap(orbis))
        self.assertEqual(lines, ["[elfldr] payload 2: Orbis (OpenOrbis/PS4 ABI), 16 KiB of code, 16 KiB of data",
                                 "self", "[elfldr] payload 2 exited with code 4"])

    def test_file_size_from_headers(self) -> None:
        files = [elf_with_script("x"), self_wrap(ob.Module("hb", e_type=oe.ET_SCE_DYNEXEC, text=b"\xc3").build())]
        files += [p.read_bytes() for p in (SAMPLES / "hello.elf", SAMPLES / "hello.self", SAMPLES / "echo.elf")
                  if p.exists()]
        # an ELF with section headers past its segments: their end is its end
        e = bytearray(gnu_elf())
        struct.pack_into("<Q", e, 0x28, len(e))
        struct.pack_into("<HH", e, 0x3A, 0x40, 3)
        files.append(bytes(e) + bytes(3 * 0x40))
        for i, data in enumerate(files):
            path = self.root / f"size{i}.bin"
            path.write_bytes(data)

            def size(n: int) -> int:
                r = subprocess.run([str(self.exe), "size", str(path), str(n)], capture_output=True, text=True)
                return int(r.stdout)
            self.assertEqual(size(len(data)), len(data))
            self.assertEqual(size(3), 0)
            if data[:4] == b"\x7fELF":
                self.assertEqual(size(63), 0)
        not_elf = self.root / "text.bin"
        not_elf.write_bytes(b"status\n")
        r = subprocess.run([str(self.exe), "size", str(not_elf), "7"], capture_output=True, text=True)
        self.assertEqual(int(r.stdout), -1)

    def test_commands_and_quit(self) -> None:
        self.assertEqual(self.talk(b"help\n"), ["[elfldr] send an ELF, or one of: status, quit, help"])
        self.assertEqual(self.talk(b"bogus\r\n"), ["[elfldr] unknown command; send an ELF, or one of: status, quit, help"])
        self.assertEqual(self.talk(b"\x00\x01binary"),
                         ["[elfldr] refused: neither an ELF nor a command (it starts 00 01 62 69)"])
        s = self.connect()
        s.sendall(elf_with_script("sleep=1500;code=0;"))
        self.assertEqual(s.recv(4096).decode().splitlines()[0][:19], "[elfldr] payload 1:")
        status = self.talk(b"status\n")
        self.assertEqual(status[0], "[elfldr] 1 payload running")
        self.assertRegex(status[1], r"^\[elfldr\] payload 1: running for \d+ ms$")
        self.assertEqual(self.talk(b"quit\n"), ["[elfldr] not quitting: 1 payload still running"])
        rest = b""
        while chunk := s.recv(4096):
            rest += chunk
        s.close()
        self.assertEqual(rest.decode(), "[elfldr] payload 1 exited with code 0\n")
        self.assertEqual(self.talk(b"quit\n"), ["[elfldr] quitting: SWRR goes back to Luac0re's loader"])
        self.assertEqual(self.proc.wait(10), 0)
        self.assertEqual(self.proc.stdout.read().strip(), "served 0")

    def test_refusals(self) -> None:
        lines = self.talk(gnu_elf(flags=(7, 6)))
        self.assertEqual(len(lines), 1)
        self.assertRegex(lines[0], r"^\[elfldr\] payload 1 refused: segment 0 is both writable and executable")
        data = elf_with_script("code=1;")
        with self.connect() as s:   # the client stops sending half way
            s.sendall(data[:1000])
            s.shutdown(socket.SHUT_WR)
            self.assertEqual(s.recv(4096).decode(),
                             f"[elfldr] refused: 1000 of {len(data)} bytes came (the client stopped sending)\n")
        huge = bytearray(gnu_elf())
        struct.pack_into("<Q", huge, 0x28, 65 << 20)
        struct.pack_into("<HH", huge, 0x3A, 0x40, 1)
        self.assertEqual(self.talk(bytes(huge[:0x200])),
                         [f"[elfldr] refused: the ELF is {(65 << 20) + 0x40} bytes; at most {64 << 20} are taken"])
        self.assertEqual(self.talk(b"\x7fEL", half_close=True),
                         ["[elfldr] refused: the connection ended after 3 bytes, inside the headers"])
        # the service is still there; only ELFs it received get a number
        self.assertEqual(self.talk(elf_with_script("code=5;"))[-1], "[elfldr] payload 2 exited with code 5")

    def test_two_at_once_and_a_third_turned_away(self) -> None:
        a, b = self.connect(), self.connect()
        a.sendall(elf_with_script("sleep=1500;code=1;"))
        a.recv(4096)
        b.sendall(elf_with_script("sleep=1500;code=2;"))
        b.recv(4096)
        lines = self.talk(elf_with_script("code=3;"))
        self.assertEqual(lines, ["[elfldr] payload 3 refused: no JIT object free for its 16 KiB of code: 2 payloads "
                                 "hold the emulator's JIT objects (one each)"])
        for s, code, n in ((a, 1, 1), (b, 2, 2)):
            rest = b""
            while chunk := s.recv(4096):
                rest += chunk
            s.close()
            self.assertEqual(rest.decode(), f"[elfldr] payload {n} exited with code {code}\n")
        self.assertEqual(self.talk(elf_with_script("code=4;"))[-1], "[elfldr] payload 4 exited with code 4")

    def test_silent_client_and_one_that_leaves(self) -> None:
        t0 = time.time()
        with self.connect() as s:   # sends nothing: dropped after EL_TIMEOUT_S (2 s here)
            self.assertEqual(s.recv(4096), b"")
        self.assertLess(time.time() - t0, 8)
        with self.connect() as s:   # leaves while its payload runs
            s.sendall(elf_with_script("out=" + "x" * 100 + "|;sleep=500;code=9;"))
            s.recv(10)
        time.sleep(1.2)
        self.assertEqual(self.talk(elf_with_script("code=6;"))[-1], "[elfldr] payload 2 exited with code 6")
        log = self.talk(b"status\n")
        self.assertEqual(log, ["[elfldr] 0 payloads running"])

    def test_elf_send(self) -> None:
        import elf_send
        path = self.root / "p.elf"
        path.write_bytes(elf_with_script("out=from the payload|;code=7;"))
        out = StringIO()
        with redirect_stdout(out):
            rc = run_elf_send(elf_send, ["p.elf", "--host", "127.0.0.1", "--port", str(self.port)], path)
        self.assertEqual(rc, 7)
        self.assertEqual(out.getvalue().splitlines()[1:], ["from the payload", "[elfldr] payload 1 exited with code 7"])
        path.write_bytes(gnu_elf(flags=(7, 6)))
        with redirect_stdout(StringIO()):
            self.assertEqual(run_elf_send(elf_send, ["p.elf", "--host", "127.0.0.1", "--port", str(self.port)], path), 1)
        out = StringIO()
        with redirect_stdout(out):
            self.assertEqual(run_elf_send(elf_send, ["--cmd", "status", "--host", "127.0.0.1", "--port",
                                                     str(self.port)], None), 0)
        self.assertEqual(out.getvalue(), "[elfldr] 0 payloads running\n")


def run_elf_send(mod, argv: list[str], path: Path | None) -> int:
    old = sys.argv
    sys.argv = ["elf_send.py"] + [str(path) if a == "p.elf" and path else a for a in argv]
    try:
        return mod.main()
    finally:
        sys.argv = old


if __name__ == "__main__":
    unittest.main()
