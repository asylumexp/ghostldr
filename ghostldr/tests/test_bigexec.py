"""Large executables (native/src/bigexec.c, bxplan.c) and the fit check
before an install (tools/exec_fit.py), offline.

- bxplan.c with elf.c in a host process (native_host/bx_host.c) and
  exec_fit.py, its Python twin, on the same inputs: Ghost's and Bloodborne's
  modules when build/ghost/app0 and build/bloodborne/app0 are there, made-up
  code maps and covers, and random covers;
- the verdicts for made-up titles of each kind, a profile's "exec" ranges,
  import_report.py's section, gfs_pack.py's line and gfs_install.py's
  refusal.

    python3 -m unittest test_bigexec        (from ghostldr/tests)
"""

from __future__ import annotations

import io
import json
import random
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
NATIVE = ROOT / "ghostldr/native"
sys.path.insert(0, str(ROOT / "ghostldr/host"))
sys.path.insert(0, str(ROOT / "ghostldr/tools"))
sys.path.insert(0, str(HERE))
import exec_fit as ef  # noqa: E402
import gen_nids  # noqa: E402
import orbis_build as ob  # noqa: E402
import orbis_elf as oe  # noqa: E402
import profiles  # noqa: E402
from test_profiles import build_host  # noqa: E402

MiB = 1 << 20
GHOST = ROOT / "build/ghost/app0"
BLOODBORNE = ROOT / "build/bloodborne/app0"
OBJS = [32 * MiB, 16 * MiB, 8 * MiB, 8 * MiB]


def parse_plan(out: str) -> dict:
    r: dict = {"runs": [], "forbid": [], "views": []}
    for line in out.splitlines():
        w = line.split()
        if w[0] == "holds":
            r["holds"] = w[1] == "1"
        elif w[0] == "big":
            r["big"], r["keep"] = w[1], int(w[3], 16)
        elif w[0] == "run":
            r["runs"].append((int(w[1], 16), int(w[2], 16)))
        elif w[0] == "forbid":
            r["forbid"].append((int(w[1], 16), int(w[2], 16)))
        elif w[0] == "view":
            r["views"].append((int(w[1]), int(w[2], 16)))
        elif w[0] == "used":
            r["used"] = int(w[1], 16)
        elif w[0] == "none":
            r["none"] = True
        elif w[0] == "evidence":
            r["evidence"] = {w[i]: int(w[i + 1], 16 if w[i] == "bytes" else 10) for i in range(1, len(w), 2)}
    return r


def fdes(path: Path) -> list[tuple[int, int]]:
    """Every FDE's [start, end), read on its own here (pcrel sdata4 addresses)."""
    img = oe.parse(path)
    text = [p for p in img.phdrs if p.type == oe.PT_LOAD and p.flags & 1][-1]
    seg = img.elf[text.offset:text.offset + text.filesz]
    hdr = img.phdr(oe.PT_GNU_EH_FRAME).vaddr
    assert seg[hdr:hdr + 4] == bytes([1, 0x1B, 0x03, 0x3B])
    count = struct.unpack_from("<I", seg, hdr + 8)[0]
    out = []
    for k in range(count):
        loc, fde = (hdr + v for v in struct.unpack_from("<ii", seg, hdr + 12 + 8 * k))
        # pc_begin as pcrel sdata4 must be the table's: then pc_range follows it
        assert fde + 8 + struct.unpack_from("<i", seg, fde + 8)[0] == loc, hex(fde)
        out.append((loc, loc + struct.unpack_from("<I", seg, fde + 12)[0]))
    return out


def inside(lo: int, hi: int, views: list[tuple[int, int]]) -> bool:
    """[lo, hi) within the union of views (offset, size), merged"""
    merged: list[list[int]] = []
    for off, size in sorted(views):
        if merged and off <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], off + size)
        else:
            merged.append([off, off + size])
    return any(a <= lo and hi <= b for a, b in merged)


def frame(functions: list[tuple[int, int]], at: int, size: int) -> tuple[bytes, int]:
    """a segment of `size` bytes with .eh_frame for functions at `at`
    (and its .eh_frame_hdr after it); returns (bytes, hdr offset)"""
    eh, _ = ob.eh_frame(at, 0, functions)
    hdr_at = (at + len(eh) + 15) & ~15
    eh, hdr = ob.eh_frame(at, hdr_at, functions)
    seg = bytearray(size)
    seg[at:at + len(eh)] = eh
    seg[hdr_at:hdr_at + len(hdr)] = hdr
    return bytes(seg), hdr_at


class PlannerTests(unittest.TestCase):
    """bxplan.c and exec_fit.py: the same answers."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name) / "bx_host"
        build_host("bx_host", [HERE / "native_host/bx_host.c", NATIVE / "src/bxplan.c", NATIVE / "src/elf.c"], cls.exe)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def host(self, *args: str) -> str:
        p = subprocess.run([str(self.exe), *args], capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return p.stdout

    def plan(self, app0: Path, *extra: str) -> dict:
        files = [str(f) for f in gen_nids.boot_modules(app0)] + [str(app0 / "eboot.bin")]
        return parse_plan(self.host("plan", *files, *extra))

    # ---- the two titles ----

    @unittest.skipUnless((GHOST / "eboot.bin").exists(), "no Ghost of Tsushima in build/ghost/app0")
    def test_ghost_stays_in_the_arena(self) -> None:
        """Ghost fits the EE buffer as it is: no views, nothing changes."""
        self.assertEqual(self.plan(GHOST), {"holds": True, "runs": [], "forbid": [], "views": []})
        r = ef.fit(GHOST)
        self.assertEqual(r["verdict"], ef.FITS)
        self.assertEqual([p["base"] for p in r["placed"]], [0x900a20000, 0x900bb0000, 0x900cd0000, 0x900d90000])

    @unittest.skipUnless((BLOODBORNE / "eboot.bin").exists(), "no Bloodborne in build/bloodborne/app0")
    def test_bloodborne_views_cover_all_its_code(self) -> None:
        c = self.plan(BLOODBORNE)
        self.assertFalse(c["holds"])
        self.assertEqual(c["big"], "eboot.bin")
        self.assertEqual(c["runs"], [(0, 0x2c10000)])
        self.assertEqual(c["forbid"], [(0x50dc000, 0x56d4000)])
        self.assertEqual(c["views"], [(0, 0), (1, 0x2000000)])      # 32 MiB at 0, 16 MiB right after
        self.assertEqual(c["used"], 48 * MiB)
        views = [(off, OBJS[j]) for j, off in c["views"]]
        self.assertEqual(len({j for j, _ in c["views"]}), len(c["views"]))
        self.assertLessEqual(c["used"], sum(OBJS))
        eboot = BLOODBORNE / "eboot.bin"
        missed = [f for f in fdes(eboot) if f[1] > f[0] and not inside(*f, views)]
        self.assertEqual(missed, [])
        img = oe.parse(eboot)
        text = img.phdrs[0]
        seg = img.elf[text.offset:text.offset + text.filesz]
        pro = [i for i in range(0, len(seg) - 4, 16) if seg[i:i + 4] == b"\x55\x48\x89\xe5"]
        self.assertGreater(len(pro), 80000)
        self.assertEqual([p for p in pro if not inside(p, p + 4, views)], [])
        for p in (img.entry, img.dyn_value(oe.DT_INIT), img.dyn_value(oe.DT_FINI)):
            self.assertTrue(inside(p, p + 1, views))
        self.assertTrue(inside(0x2bbe4a0, 0x2bc0e00, views))             # the PLT
        for lo, hi in c["forbid"]:
            self.assertTrue(all(off + size <= lo or off >= hi for off, size in views))
        # the twin agrees, and the arena keeps an 8 MiB buffer for libc and Fios2
        r = ef.fit(BLOODBORNE)
        self.assertEqual(r["verdict"], ef.MULTI)
        self.assertEqual((r["runs"], r["keep"]), (c["runs"], c["keep"]))
        self.assertEqual([(v["buffer"], v["offset"]) for v in r["views"]], [("EE", 0), ("VU1", 0x2000000)])
        self.assertEqual(r["arena"], "IOP")
        self.assertEqual(r["evidence"]["fdes"], c["evidence"]["fdes"])
        self.assertEqual(r["evidence"]["prologues"], c["evidence"]["prologues"])
        self.assertEqual(r["evidence"]["plt"], c["evidence"]["plt"])
        self.assertGreater(r["headroom"], r["on_demand_bytes"])

    @unittest.skipUnless((BLOODBORNE / "eboot.bin").exists(), "no Bloodborne in build/bloodborne/app0")
    def test_bloodborne_with_profile_ranges(self) -> None:
        c = self.plan(BLOODBORNE, "--exec", "0:2000000,2400000:2bc1000")
        self.assertEqual(c["runs"], [(0, 0x2000000), (0x2400000, 0x2bc4000)])
        self.assertEqual(c["views"], [(0, 0), (2, 0x23c4000)])       # 32 MiB, then 8 MiB ending with the code
        r = ef.fit(BLOODBORNE, {"eboot.bin": [(0, 0x2000000), (0x2400000, 0x2bc1000)]})
        self.assertEqual(r["runs"], c["runs"])
        self.assertEqual([(v["buffer"], v["offset"]) for v in r["views"]], [("EE", 0), ("IOP", 0x23c4000)])
        self.assertEqual(r["arena"], "VU1")

    # ---- made-up code maps ----

    def runs_both(self, seg: bytes, vaddr: int, text: int, eh: int = 0, got: tuple[int, int] = (0, 0),
                  points: tuple = ()) -> list:
        path = Path(self.tmp.name) / "seg.bin"
        path.write_bytes(seg)
        out = parse_plan(self.host("runs", str(path), *(f"{v:x}" for v in (vaddr, text, eh, *got, *points))))
        st = ef.Stats()
        py = ef.code_runs(ef.CodeIn(seg, vaddr, text, eh, points, got), st)
        self.assertEqual(out["runs"], py)
        self.assertEqual(out["evidence"], {"fdes": st.fdes, "bytes": st.fde_bytes, "prologues": st.prologues,
                                           "plt": st.plt, "points": st.points, "eh": int(st.eh)})
        return py

    def test_code_maps(self) -> None:
        size = 64 * MiB
        seg, hdr = frame([(0x1000, 0x800), (0x3000000, 0x1000)], 0x3800000, size)
        self.assertEqual(self.runs_both(seg, 0, size, hdr), [(0, 0x50000), (0x2fc0000, 0x3050000)])
        # less than 1 MiB apart: one run; a prologue far away: its own
        seg, hdr = frame([(0x100000, 0x100), (0x1e0000, 0x10)], 0x3800000, size)
        seg = bytearray(seg)
        seg[0x2800010:0x2800014] = b"\x55\x48\x89\xe5"
        seg[0x2900004:0x2900008] = b"\x55\x48\x89\xe5"               # not 16-aligned: ignored
        self.assertEqual(self.runs_both(bytes(seg), 0, size, hdr),
                         [(0xc0000, 0x230000), (0x27c0000, 0x2850000)])
        # PLT entries into the GOT, the entry points
        seg = bytearray(size)
        seg[0x1000000:0x1000006] = b"\xff\x25" + struct.pack("<i", 0x5000000 - 0x1000006)
        seg[0x1200000:0x1200006] = b"\xff\x25" + struct.pack("<i", 0x6000000 - 0x1200006)   # not the GOT
        self.assertEqual(self.runs_both(bytes(seg), 0, size, 0, (0x5000000, 0x5000100), (0x20,)),
                         [(0, 0x50000), (0xfc0000, 0x1050000)])

    def test_code_map_details(self) -> None:
        size = 40 * MiB
        # more than six runs: the closest are joined first
        funcs = [(k * 4 * MiB, 0x100) for k in range(9)] + [(36 * MiB + 2 * MiB, 0x100)]
        seg, hdr = frame(sorted(funcs), 39 * MiB, size)
        got = self.runs_both(seg, 0, size, hdr)
        self.assertEqual(len(got), 6)                                # the first five joined, in order
        self.assertEqual(got[0], (0, 16 * MiB + 0x10000 + PAD_))
        self.assertEqual(got[-1], (38 * MiB - PAD_, 38 * MiB + 0x10000 + PAD_))
        # an unusable .eh_frame_hdr (another table encoding): prologues only
        seg = bytearray(seg)
        seg[hdr + 3] = 0x1B
        seg[0x10] = 0x55
        seg[0x11:0x14] = b"\x48\x89\xe5"
        self.assertEqual(self.runs_both(bytes(seg), 0, size, hdr), [(0, 0x50000)])
        # a segment not at 0, ending inside a chunk; text past BX_TEXT_MAX: nothing
        seg = bytearray(0x22000)
        seg[0x20000:0x20004] = b"\x55\x48\x89\xe5"
        self.assertEqual(self.runs_both(bytes(seg), 0x4000, 0x28000, 0), [(0, 0x28000)])
        self.assertEqual(self.runs_both(b"\0" * 64, 0, 513 * MiB, 0, (0, 0), (0,)), [])

    # ---- covers ----

    def cover_both(self, cases: list[tuple]) -> list:
        path = Path(self.tmp.name) / "cases.txt"
        lines = []
        for runs, forbid, objs, keep in cases:
            lines.append(" ".join(["R", *(f"{v:x}" for r in runs for v in r), "F", *(f"{v:x}" for r in forbid for v in r),
                                   "O", *(f"{v:x}" for v in objs), "K", f"{keep:x}"]))
        path.write_text("\n".join(lines) + "\n")
        c = self.host("cover", str(path)).splitlines()
        out = []
        for line, (runs, forbid, objs, keep) in zip(c, cases):
            lay = ef.cover(list(runs), list(forbid), list(objs), keep)
            py = "none" if lay is None else " ".join(f"{j} {o:x}" for j, o in lay["views"]) + f" used {lay['used']:x}"
            self.assertEqual(line.strip(), py, (runs, forbid, objs, keep))
            out.append(None if lay is None else lay["views"])
        self.assertEqual(len(c), len(cases))
        return out

    def test_covers(self) -> None:
        text_end = [(0x5000000, 0x5800000)]                          # a data segment after the text
        got = self.cover_both([
            ([(0, 0x2c10000)], text_end, OBJS, 0x270000),             # Bloodborne: 32 + 16
            ([(0, 0x1000000)], text_end, OBJS, 0),                   # 16 alone
            ([(0, 0x1400000), (0x3000000, 0x3100000)], text_end, OBJS, 0),   # 16 + 8, then 8: 32 left
            ([(0, 0x3200000)], text_end, OBJS, 0),                   # 50 MiB: 32 + 16 + 8 (+ 8 spare)
            ([(0, 0x3200000)], text_end, OBJS, 0x900000),            # and the arena needs > 8 MiB: none
            ([(0, 0x4100000)], [], OBJS, 0),                          # 65 MiB of code: none
            ([(0x4f00000, 0x4fc0000)], text_end, OBJS, 0),           # code right before the data: back off
            ([(0x100000, 0x200000)], [(0x400000, 0x800000)], [16 * MiB], 0),   # before offset 0
            ([(0, 0x100000), (0x900000, 0xa00000)], [(0x400000, 0x500000)], [8 * MiB], 0),  # data between
        ])
        self.assertEqual(got[0], [(0, 0), (1, 0x2000000)])
        self.assertEqual(got[1], [(1, 0)])
        self.assertEqual(got[2], [(1, 0), (3, 0x1000000), (2, 0x2900000)])
        self.assertEqual(got[3], [(0, 0), (1, 0x2000000), (2, 0x3000000)])
        self.assertEqual(got[4:6], [None, None])
        self.assertEqual(got[6], [(2, 0x47c0000)])                 # ends with the code
        self.assertEqual(got[7], [(0, -0xe00000)])                # before the module, ending with the code
        self.assertIsNone(got[8])

    def test_random_covers_agree(self) -> None:
        rnd = random.Random(1022)
        cases = []
        for _ in range(400):
            pos, runs = 0, []
            for _ in range(rnd.randint(1, 6)):
                pos += rnd.choice([0, 0x80000, 0x400000, 0x1000000]) + rnd.randint(1, 64) * 0x10000
                end = pos + rnd.randint(1, 256) * 0x10000
                runs.append((pos, end))
                pos = end
            forbid = []
            if rnd.random() < 0.7:
                lo = pos + rnd.randint(0, 64) * 0x4000
                forbid.append((lo, lo + rnd.randint(1, 512) * 0x4000))
            objs = sorted(rnd.sample([32, 16, 8, 8, 4, 2], rnd.randint(1, 5)), reverse=True)
            cases.append((runs, forbid, [o * MiB for o in objs], rnd.choice([0, 0x200000, 0x900000])))
        got = self.cover_both(cases)
        self.assertGreater(sum(g is not None for g in got), 50)
        self.assertGreater(sum(g is None for g in got), 50)
        for (runs, forbid, objs, _), views in zip(cases, got):
            if views is None:
                continue
            spans = [(o, objs[j]) for j, o in views]
            self.assertTrue(all(inside(lo, hi, spans) for lo, hi in runs))
            self.assertTrue(all(o + s <= lo or o >= hi for o, s in spans for lo, hi in forbid))
            self.assertTrue(all(o % 0x4000 == 0 for o, _ in spans))


PAD_ = ef.PAD


class FakeConsoleTests(unittest.TestCase):
    """bigexec.c itself against a fake 13.60 PS5 (native_host/bigexec_host.c):
    which descriptors it touches, where views and direct memory go, and every
    byte afterwards."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name) / "bigexec_host"
        build_host("bigexec_host", [HERE / "native_host/bigexec_host.c", NATIVE / "src/bigexec.c",
                                    NATIVE / "src/bxplan.c", NATIVE / "src/elf.c"], cls.exe)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def boot(self, app0: Path) -> str:
        files = [str(f) for f in gen_nids.boot_modules(app0)] + [str(app0 / "eboot.bin")]
        p = subprocess.run([str(self.exe), *files], capture_output=True, text=True, timeout=300)
        self.assertEqual(p.returncode, 0, p.stdout[-3000:] + p.stderr)
        self.assertNotIn("FAULT", p.stdout)
        return p.stdout

    @unittest.skipUnless((BLOODBORNE / "eboot.bin").exists(), "no Bloodborne in build/bloodborne/app0")
    def test_bloodborne(self) -> None:
        out = self.boot(BLOODBORNE)
        self.assertIn("prepare 0", out)
        self.assertEqual(re.findall(r"op rw alias of (\w+)", out), ["EE", "VU1"])   # never the core's IPU object
        self.assertIn("arena now 902a24000-903220000", out)                          # libc and Fios2: the IOP buffer
        self.assertIn("op view EE at +0\nop view VU1 at +2000000", out)
        self.assertIn("log: bigexec: eboot.bin: +3000000-+50dc000 not executable (direct memory)", out)
        self.assertIn("write 0", out)
        self.assertIn("executable +0 1, +286e000 1, +50dbfff 0", out)                  # crash.c's test
        self.assertIn("object EE at +0, out of the arena", out)
        self.assertIn("object VU1 at +2000000, out of the arena", out)
        self.assertIn("bytes wrong 0; direct 39760 KiB; compiler aliases left open 0", out)

    @unittest.skipUnless((GHOST / "eboot.bin").exists(), "no Ghost of Tsushima in build/ghost/app0")
    def test_ghost_touches_nothing(self) -> None:
        self.assertIn("prepare 0, 0 alias calls, 0 RW aliases", self.boot(GHOST))

    def test_made_up_titles(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = self.boot(big_title(Path(tmp) / "m", 40 * MiB, [(0x1000, 20 * MiB)]))
            # VU0's base descriptor comes first: 8 MiB, so it is looked at (and is not IOP)
            self.assertEqual(re.findall(r"op rw alias of (\w+)", out), ["VU0", "IOP", "VU1"])
            self.assertIn("op view VU1 at +0\nop view IOP at +1000000", out)
            self.assertIn("arena now 900a20000-902a1c000", out)
            self.assertIn("bytes wrong 0;", out)
            self.assertIn("compiler aliases left open 0", out)
            out = self.boot(big_title(Path(tmp) / "n", 72 * MiB, [(0x1000, 66 * MiB)]))
            self.assertIn("it cannot be loaded", out)
            self.assertIn("prepare -1, 0 alias calls", out)


def title(root: Path, text: bytes, functions: list[tuple[int, int]] | None = None, hdr_at: int = 0) -> Path:
    """a one-module title: eboot.bin with this text (and .eh_frame in it)"""
    app0 = root / "app0"
    m = ob.Module("made_up", e_type=oe.ET_SCE_DYNEXEC, text=text, entry=0x40, data_size=0x8000,
                  eh_frame_hdr=hdr_at if functions else None, eh_keep=True)
    m.write(app0 / "eboot.bin")
    return app0


def big_title(root: Path, size: int, functions: list[tuple[int, int]]) -> Path:
    eh_at = size - 4 * MiB
    seg, hdr = frame(functions, eh_at, size)
    return title(root, seg, functions, hdr)


class FitTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_verdicts(self) -> None:
        small = title(self.root / "s", b"\x55\x48\x89\xe5\xc3".ljust(0x8000, b"\xcc"))
        self.assertEqual(ef.fit(small)["verdict"], ef.FITS)
        multi = big_title(self.root / "m", 40 * MiB, [(0x1000, 20 * MiB)])
        r = ef.fit(multi)
        self.assertEqual(r["verdict"], ef.MULTI)
        self.assertEqual([(v["buffer"], v["offset"]) for v in r["views"]], [("VU1", 0), ("IOP", 0x1000000)])
        self.assertEqual(r["arena"], "EE")
        self.assertIn("needs multi-buffer placement", ef.summary(r))
        none = big_title(self.root / "n", 72 * MiB, [(0x1000, 66 * MiB)])
        r = ef.fit(none)
        self.assertEqual(r["verdict"], ef.NO_FIT)
        self.assertTrue(ef.summary(r).startswith("won't fit (eboot.bin: 66.3 MiB of code; even with"), ef.summary(r))
        # only the packing fails: two 30 MiB runs take every buffer (32, then 16 + 8 + 8), none is left
        # for the arena; fresh objects after a reclaim would hold them
        packing = big_title(self.root / "p", 72 * MiB, [(0x1000, 29 * MiB), (36 * MiB, 29 * MiB)])
        r = ef.fit(packing)
        self.assertEqual(r["verdict"], ef.NO_FIT)
        self.assertTrue(ef.summary(r).startswith("won't fit with buffer views; would fit after reclaim"),
                        ef.summary(r))
        # a profile that says where the code is
        r = ef.fit(none, {"eboot.bin": [(0, 30 * MiB)]})
        self.assertEqual((r["verdict"], r["code_from"]), (ef.MULTI, "profile"))
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(ef.main([str(none)]), 1)
        self.assertIn("verdict: won't fit", out.getvalue())

    def test_profile_exec_ranges(self) -> None:
        prof = profiles.load("CUSA99999")
        prof["exec"] = {"eboot.bin": [["0x0", "0x2c00000"]]}
        self.assertIn("exec ranges are text offsets of one build: set eboot.fingerprint", profiles.validate(prof))
        prof["eboot"] = {"fingerprint": "11" * 20}
        self.assertEqual(profiles.validate(prof), [])
        recs = profiles.read_blobs(profiles.compile_blob(prof))[0]["records"]
        self.assertEqual(recs, [(profiles.REC_EXEC, b"eboot.bin".ljust(32, b"\0") + struct.pack("<III", 1, 0, 0x2c00000)
                                 + bytes(4))])
        for bad in ([["0x100", "0x10"]], [["0", "0x100"], ["0x80", "0x200"]], [], [["0", "1"]] * 7):
            prof["exec"] = {"eboot.bin": bad}
            self.assertTrue(any(e.startswith("exec.eboot.bin") for e in profiles.validate(prof)), bad)

    def test_import_report_section(self) -> None:
        import import_report
        multi = big_title(self.root / "m", 40 * MiB, [(0x1000, 20 * MiB)])
        md = self.root / "r.md"
        with mock.patch.object(sys, "stderr", io.StringIO()) as err:
            self.assertEqual(import_report.main([str(multi), "--md", str(md), "--names", str(self.root / "none")]), 0)
        text = md.read_text()
        self.assertIn("## Executable memory", text)
        self.assertIn("- Executable memory: needs multi-buffer placement", text)
        self.assertIn("VU1 (16 MiB) at +0-+1000000", text)
        self.assertIn("executable memory: needs multi-buffer placement", err.getvalue())

    def test_pack_prints_and_install_refuses(self) -> None:
        import gfs_install as gi
        import gfs_pack as gp
        none = big_title(self.root / "n", 72 * MiB, [(0x1000, 66 * MiB)])
        packs = self.root / "packs"
        out = io.StringIO()
        with mock.patch.object(gp, "OUT_ROOT", packs), redirect_stdout(out), \
                mock.patch.object(sys, "argv", ["gfs_pack.py", "--src", str(none), "--title", "CUSA99991"]):
            self.assertEqual(gp.main(), 0)
        self.assertIn("executable memory: won't fit", out.getvalue())
        self.assertIn("gfs_install.py will refuse it", out.getvalue())
        lines: list[str] = []

        def no_console(args):
            raise AssertionError("connected before the fit check")

        with mock.patch.object(gi, "OUT_ROOT", packs), mock.patch.object(gi, "log", lines.append), \
                mock.patch.object(gi, "connect", no_console), \
                mock.patch.object(sys, "argv", ["gfs_install.py", "--title", "CUSA99991"]):
            with self.assertRaises(SystemExit) as cm:
                gi.main()
        self.assertIn("does not fit the executable memory", str(cm.exception))
        self.assertIn("nothing was sent", str(cm.exception))
        self.assertTrue(lines[0].startswith("executable memory: won't fit"), lines)
        lines.clear()
        with mock.patch.object(gi, "OUT_ROOT", packs), mock.patch.object(gi, "log", lines.append), \
                mock.patch.object(gi, "connect", no_console), \
                mock.patch.object(sys, "argv", ["gfs_install.py", "--title", "CUSA99991", "--no-fit-check"]):
            with self.assertRaises(AssertionError):           # past the check: it connects
                gi.main()
        self.assertIn("  installing anyway (--no-fit-check)", lines)
        layout = json.loads((packs / "CUSA99991/layout.json").read_text())
        self.assertEqual(layout["src"], str(none))


if __name__ == "__main__":
    unittest.main()
