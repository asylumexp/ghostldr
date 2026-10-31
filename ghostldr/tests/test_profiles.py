"""Title profiles: parsing, merging and validation (host/profiles.py), the
compiled blobs, and the core's side of them: native/src/profile.c,
budget.c and automem.c built for this machine (native_host/profile_host.c)
and run against fakes of the console.

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import copy
import os
import shutil
import struct
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
sys.path.insert(0, str(ROOT / "ghostldr/tools"))
import orbis_elf as oe  # noqa: E402
import profiles  # noqa: E402

GHOST_FP = "3afb932d5171d24cc9f3d536dedfaf6900000000"
BLOODBORNE_FP = "543f03aa757c0ab8edb7db53668f0c436d42b43d"
BLOODBORNE_SRC = ROOT / "build/bloodborne/app0"     # a CUSA00900 1.09 dump, when there is one


def ghost_lines() -> list[str]:
    """What the 2026-10-21 boots logged for Ghost's memory plan (4532 MiB free)."""
    return ["budget: CPU malloc heap 384 -> 320 MiB",
            "budget: free 298000-1bdc000 (25 MiB)",
            "budget: free 30a4000-10010000 (207 MiB)",
            "budget: free 12800000-13c40000 (20 MiB)",
            "budget: free 13e00000-50080000 (962 MiB)",
            "budget: free 503b8000-11e000000 (3292 MiB)",
            "budget: free 11e74c000-120000000 (24 MiB)",
            "budget: 4532 MiB free in 6 holes; streaming pool 1120 -> 588 MiB; reporting 4520 MiB direct memory"]


class ProfileTests(unittest.TestCase):
    def test_every_profile_validates_and_compiles(self) -> None:
        for name in ["default"] + profiles.known_titles():
            prof = profiles.load(None if name == "default" else name)
            blob = profiles.compile_blob(prof)
            self.assertEqual(len(blob) % 8, 0, name)

    def test_title_merges_over_default(self) -> None:
        g = profiles.load("CUSA11456")
        self.assertTrue(g["_has_profile"])
        self.assertEqual(g["memory"]["mode"], "plan")
        self.assertEqual(g["memory"]["min_mib"], 1024)          # from default.json
        self.assertEqual(g["pack"]["prefix"], "GHOST")
        other = profiles.load("CUSA99999")                       # no profile: the defaults
        self.assertFalse(other["_has_profile"])
        self.assertEqual(other["memory"]["mode"], "plan-free")
        self.assertEqual(other["title"], "CUSA99999")

    def errors(self, **over) -> list[str]:
        p = profiles.merge(profiles.raw_profile("default"), {"title": "CUSA22222", **over})
        return profiles.validate(p)

    def test_validator_rejects_bad_fields(self) -> None:
        self.assertEqual(self.errors(), [])
        self.assertTrue(self.errors(bogus=1))
        self.assertTrue(self.errors(pack={"prefix": "GFSX"}))            # reserved
        self.assertTrue(self.errors(pack={"prefix": "TOOLONGPREFIX1"}))
        self.assertTrue(self.errors(pack={"order": "missing-order.txt"}))
        self.assertTrue(self.errors(pack={"groups": [["(", 0]]}))
        self.assertTrue(self.errors(app_params={"7": 1}))
        self.assertTrue(self.errors(save_blocks=5))
        self.assertTrue(self.errors(features=["ghost_hooks"]))          # Ghost's executable only
        self.assertTrue(self.errors(features=["warp_drive"]))
        self.assertTrue(self.errors(memory={"mode": "magic"}))
        self.assertTrue(self.errors(eboot={"fingerprint": "12"}))
        self.assertTrue(self.errors(hle=[{"lib": "libSceNpManager"}]))   # no function
        self.assertTrue(self.errors(hle=[{"function": "f", "action": "trace"}]))
        self.assertTrue(self.errors(hle=[{"function": "f", "fill": [{"arg": 9}]}]))
        self.assertTrue(self.errors(hle=[{"function": "f", "fill": [{"arg": 1, "size": 3, "value": 1}]}]))
        self.assertEqual(self.errors(hle=[{"lib": "libSceNpManager", "function": "sceNpGetState", "value": 0,
                                           "fill": [{"arg": 2, "size": 4, "value": 3}, {"arg": 3, "zero": 16}]}]),
                         [])

    def test_a_plan_needs_the_fingerprint(self) -> None:
        g = profiles.load("CUSA11456")
        del g["eboot"]
        self.assertTrue([e for e in profiles.validate(g) if "fingerprint" in e])
        g = profiles.load("CUSA11456")
        g["memory"]["plan"]["tables"] = [t for t in g["memory"]["plan"]["tables"] if t["role"] != "absorb"]
        self.assertTrue([e for e in profiles.validate(g) if "absorb" in e])

    def test_ghost_blob(self) -> None:
        blob = profiles.compile_blob(profiles.load("CUSA11456"))
        [b] = profiles.read_blobs(blob)
        self.assertEqual(b["title"], "CUSA11456")
        self.assertEqual(b["fingerprint"], GHOST_FP)
        self.assertEqual(b["flags"], profiles.PF_PLAN | profiles.PF_HOOKS | profiles.PF_DIAG | profiles.PF_OWN
                         | profiles.PF_DIALOG_AUTO)   # a GPU title: its dialogs are answered at once
        self.assertEqual(b["app_params"], [3, 11456, 0, 0, 0])   # cache/t11456.dat
        self.assertEqual((b["save_blocks"], b["runtime_reserve_mib"], b["video_reserve_mib"], b["flex_mib"]),
                         (256, 32, 100, 0))
        kinds = [k for k, _ in b["records"]]
        self.assertEqual(kinds, [profiles.REC_PLAN] + [profiles.REC_TABLE] * 4 + [profiles.REC_NEO]  # Pro mode
                         + [profiles.REC_TLS] + [profiles.REC_NAME, profiles.REC_PREFIX])     # tls_sites, name
        self.assertEqual(b["records"][-1][1].rstrip(b"\0"), b"GHOST")       # a USB drive's copy without a prefix file
        self.assertEqual(struct.unpack_from("<11I", b["records"][5][1]),   # Ghost's mode 2 (Jak X), GPU heap 1680
                         (2, 4636, 74, 0, 0, 0, 0, 1680, 0, 0, 0))
        self.assertEqual(b["records"][-2][1].rstrip(b"\0"), b"Ghost of Tsushima")   # the selector's name
        fixed, over, n, *sites = struct.unpack_from("<6I", b["records"][0][1])
        self.assertEqual((fixed, over, n, sites), (3996, 120, 3, [0x896126, 0x89614A, 0xEEE2CF]))
        table, role, knob, nexp, mib, min_mib = struct.unpack_from("<IBBHII", b["records"][1][1])
        self.assertEqual((table, role, knob, nexp, mib, min_mib), (0x111CD50, 1, 0xFF, 6, 0, 128))
        self.assertTrue(b["records"][1][1][16 + 48:].startswith(b"streaming pool\0"))

    def test_name_record(self) -> None:
        prof = profiles.load("CUSA22222")
        self.assertFalse([k for k, _ in profiles.read_blobs(profiles.compile_blob(prof))[0]["records"]
                          if k == profiles.REC_NAME])      # no name: no record
        prof["name"] = "\u661f" * 60                      # 180 bytes of UTF-8: cut on a character boundary
        [b] = profiles.read_blobs(profiles.compile_blob(prof))
        [rec] = [r for k, r in b["records"] if k == profiles.REC_NAME]
        self.assertEqual(rec.rstrip(b"\0").decode(), "\u661f" * 42)
        self.assertEqual(len(rec) % 8, 0)

    def test_rule_blob_layout(self) -> None:
        prof = profiles.load("CUSA22222")
        prof["hle"] = [{"lib": "libSceNpManager", "function": "sceNpGetState", "value": "0x80550001", "log": 2,
                        "fill": [{"arg": 2, "offset": 4, "size": 4, "value": 7}, {"arg": 2, "offset": 8, "zero": 4}]}]
        [b] = profiles.read_blobs(profiles.compile_blob(prof))
        kind, rec = b["records"][0]
        self.assertEqual(kind, profiles.REC_RULE)
        action, nfill, log, nid, value = struct.unpack_from("<BBH12sQ", rec)
        self.assertEqual((action, nfill, log, value), (1, 2, 2, 0x80550001))
        self.assertEqual(nid.rstrip(b"\0").decode(), oe.compute_nid("sceNpGetState"))
        self.assertEqual(struct.unpack_from("<BBHIQ", rec, 24), (1, 4, 4, 0, 7))
        self.assertEqual(struct.unpack_from("<BBHIQ", rec, 40), (1, 0, 8, 0, 4))
        self.assertTrue(rec[56:].startswith(b"libSceNpManager\0sceNpGetState\0"))

    def test_boot_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(profiles, "TITLES_OUT", Path(tmp)):
            one = profiles.read_blobs(profiles.boot_bundle("cusa11456", ("trace_opens",)))
            self.assertEqual([b["title"] for b in one], ["CUSA11456", ""])
            self.assertTrue(one[0]["flags"] & profiles.PF_REQUESTED)
            self.assertTrue(all(b["flags"] & profiles.PF_TRACE_OPENS for b in one))
            allb = profiles.read_blobs(profiles.boot_bundle())
            self.assertIn("CUSA11456", [b["title"] for b in allb])
            self.assertEqual(allb[-1]["title"], "")
            self.assertFalse(any(b["flags"] & profiles.PF_REQUESTED for b in allb))
        with self.assertRaises(profiles.ProfileError):
            profiles.boot_bundle("not a title!")

    def test_homebrew_without_param_sfo(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp)
            (src / "eboot.bin").write_bytes(b"\x7fELF" + b"\0" * 60)    # no param.sfo, no fingerprint
            self.assertIsNone(profiles.detect_title(src))
            self.assertEqual(profiles.param_sfo(src), {})
            (src / "sce_sys").mkdir()
            (src / "sce_sys/param.sfo").write_bytes(b"garbage")
            self.assertEqual(profiles.param_sfo(src), {})
            self.assertIsNone(profiles.detect_title(src))
        self.assertEqual(profiles.default_prefix("CUSA12345"), "GCUSA12345")
        self.assertNotEqual(profiles.default_prefix("APOL00004"), profiles.default_prefix("CUSA00004"))
        self.assertEqual(profiles.default_prefix("HB-TEST_1"), "GHBTEST1")
        self.assertEqual(profiles.default_prefix("FSXX00001"), "TFSXX00001")        # not one of ghostldr's GFS names
        self.assertEqual(profiles.default_prefix("HOMEBRWD123"), "GHOMEBRWD12X")  # never ends like a save directory
        self.assertEqual(profiles.legacy_prefix("CUSA12345"), "G12345")             # what installs before 2026-10-28 used
        self.assertEqual(profiles.normalize_title(" lapy10010 "), "LAPY10010")

    def test_openorbis_homebrew(self) -> None:
        # every OpenOrbis build has the fingerprint "OPENORBIS-HOMEBREW": it names no build
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp)
            (src / "eboot.bin").write_bytes(orbis_eboot("hello_world"))
            self.assertIsNone(profiles.eboot_fingerprint(src))
            self.assertEqual(profiles.homebrew_toolchain(src), "OpenOrbis")
            self.assertEqual(profiles.eboot_module_name(src), "hello_world")
            self.assertEqual(profiles.detect_title(src), "HELLO_WORLD")        # no param.sfo: the module name
            self.assertNotIn("eboot", profiles.starter(src))
            (src / "eboot.bin").write_bytes(orbis_eboot("a very long name for a project"))
            self.assertEqual(profiles.detect_title(src), "AVERYLONGNAMEFO")
            (src / "sce_sys").mkdir()
            (src / "sce_sys/param.sfo").write_bytes(make_sfo({"TITLE_ID": "BREW00083"}))
            self.assertEqual(profiles.detect_title(src), "BREW00083")         # a usable param.sfo wins
            (src / "sce_sys/param.sfo").write_bytes(make_sfo({"TITLE_ID": "not an id!"}))
            self.assertEqual(profiles.detect_title(src), "AVERYLONGNAMEFO")
            (src / "eboot.bin").write_bytes(orbis_eboot("game", bytes(range(1, 21))))
            self.assertEqual(profiles.eboot_fingerprint(src), bytes(range(1, 21)).hex())
            self.assertIsNone(profiles.homebrew_toolchain(src))
            self.assertIsNone(profiles.detect_title(src))                     # not homebrew: no guess
        bad = profiles.merge(profiles.raw_profile("default"), {"title": "HB1", "eboot": {"fingerprint": OPENORBIS_FP.hex()}})
        self.assertTrue([e for e in profiles.validate(bad) if "names no build" in e])

    def test_param_sfo_title(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp)
            (src / "sce_sys").mkdir()
            (src / "sce_sys/param.sfo").write_bytes(make_sfo({"TITLE_ID": "BREW00042", "TITLE": "Homebrew",
                                                              "APP_VER": "01.00", "USER_DEFINED_PARAM_1": 42}))
            self.assertEqual(profiles.detect_title(src), "BREW00042")
            self.assertEqual(profiles.app_params(profiles.load("BREW00042"), src), [3, 42, 0, 0, 0])

    def test_bloodborne_plan_matches_its_executable(self) -> None:
        """The plan's tables hold what the 1.09 executable has there: config 1
        (Master_ps4_GFXMIX) of the heap table at 0x4736d40."""
        if not (BLOODBORNE_SRC / "eboot.bin").exists():
            self.skipTest("needs the CUSA00900 1.09 dump in build/bloodborne/app0")
        prof = profiles.load("CUSA00900")
        self.assertEqual(profiles.eboot_fingerprint(BLOODBORNE_SRC), prof["eboot"]["fingerprint"])
        img = oe.parse(BLOODBORNE_SRC / "eboot.bin")
        cfg1 = struct.unpack("<16Q", img.read_vaddr(0x4736dc0, 0x80))
        self.assertEqual([v >> 20 for v in cfg1], [524, 276, 152, 2287, 886, 10, 6, 0, 14, 0, 5, 8, 9, 296, 0, 5])
        for t in prof["memory"]["plan"]["tables"]:
            off = int(t["table"], 16)
            self.assertEqual((off - 0x4736dc0) % 8, 0, t["what"])
            self.assertTrue(0x4736dc0 <= off < 0x4736e40, t["what"])
            want = [int(v, 16) for v in t.get("expect", [])]
            self.assertEqual(list(struct.unpack(f"<{len(want)}Q", img.read_vaddr(off, 8 * len(want)))), want)
        for p in prof["memory"]["plan"]["patches"]:  # the 116 MiB block's size, as immediates
            self.assertEqual(img.read_vaddr(int(p["at"], 16), 4).hex(), p["expect"], p["what"])

    def test_plan_patches_compile_to_records(self) -> None:
        blob = profiles.compile_blob(profiles.load("CUSA00900"))
        at = blob.find(struct.pack("<IB3x", 0x2081a8f, 4)) - 8
        self.assertGreater(at, 0)
        self.assertEqual(struct.unpack_from("<H", blob, at)[0], profiles.REC_PATCH)
        self.assertEqual(blob[at + 16:at + 24].hex(), "0000400700000002")
        self.assertEqual(blob[at + 24:at + 60], b"first block 116 -> 32 MiB: allocate\0")
        bad = profiles.load("CUSA00900")
        bad["memory"]["plan"]["patches"][0]["write"] = "00"
        self.assertTrue([e for e in profiles.validate(bad) if "as many of each" in e])

    def test_swap_entries_compile_to_records(self) -> None:
        blob = profiles.compile_blob(profiles.load("CUSA00900"))
        at = blob.find(struct.pack("<4I", 0x2081ec1, 296, 120, 0)) - 8
        self.assertGreater(at, 0)
        self.assertEqual(struct.unpack_from("<H", blob, at)[0], profiles.REC_SWAP)
        self.assertEqual(blob[at + 24:at + 35], b"HAVOK heap\0")
        bad = profiles.load("CUSA00900")
        bad["memory"]["plan"]["swap"][0]["resident_mib"] = 300
        self.assertTrue([e for e in profiles.validate(bad) if "at most len_mib" in e])

    def test_dump_is_detected_by_its_fingerprint(self) -> None:
        src = profiles.DEFAULT_SRC
        if not (src / "eboot.bin").exists():
            self.skipTest("needs the CUSA11456 dump in build/ghost/app0")
        self.assertEqual(profiles.eboot_fingerprint(src), GHOST_FP)
        self.assertEqual(profiles.detect_title(src), "CUSA11456")


OPENORBIS_FP = b"OPENORBIS-HOMEBREW\0\0"


def orbis_eboot(module: str, fingerprint: bytes = OPENORBIS_FP) -> bytes:
    """An eboot.bin with nothing but what the tools read: PT_DYNAMIC and
    PT_SCE_DYNLIBDATA with a string table, the module's name and a
    fingerprint (an OpenOrbis build's, unless told otherwise)."""
    import orbis_elf as oe
    strtab = b"\0" + module.encode() + b"\0"
    data = strtab.ljust(32, b"\0") + fingerprint
    dyn = b"".join(struct.pack("<qQ", t, v) for t, v in (
        (oe.DT_SCE_STRTAB, 0), (oe.DT_SCE_STRSZ, len(strtab)), (oe.DT_SCE_FINGERPRINT, 32),
        (oe.DT_SCE_MODULE_INFO, 1 | 1 << 40 | 1 << 32), (oe.DT_NULL, 0)))
    dyn_off, data_off = 64 + 2 * 56, 64 + 2 * 56 + len(dyn)
    ehdr = b"\x7fELF\x02\x01\x01\x09" + bytes(8) + struct.pack("<HHIQQQIHHHHHH", oe.ET_SCE_DYNEXEC, 0x3E, 1, 0, 64,
                                                                  0, 0, 64, 56, 2, 0, 0, 0)
    phdrs = struct.pack("<IIQQQQQQ", oe.PT_DYNAMIC, 6, dyn_off, 0, 0, len(dyn), len(dyn), 8)
    phdrs += struct.pack("<IIQQQQQQ", oe.PT_SCE_DYNLIBDATA, 4, data_off, 0, 0, len(data), 0, 16)
    return ehdr + phdrs + dyn + data


def make_sfo(entries: dict) -> bytes:
    """A minimal param.sfo: strings (utf-8, fmt 0x0204) and integers (0x0404)."""
    keys, data, index = b"", b"", b""
    for k, v in entries.items():
        raw = struct.pack("<i", v) if isinstance(v, int) else v.encode() + b"\0"
        index += struct.pack("<HHIII", len(keys), 0x0404 if isinstance(v, int) else 0x0204, len(raw), len(raw),
                             len(data))
        keys += k.encode() + b"\0"
        data += raw
    kt = 20 + len(index)
    dt = kt + len(keys)
    return b"\0PSF" + struct.pack("<IIII", 0x101, kt, dt, len(entries)) + index + keys + data


def host_cc() -> list[str] | None:
    """A C compiler for this machine whose output runs here (the core's C is
    portable apart from its x86 entry stubs, which the harnesses avoid)."""
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        return None
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "probe"
        (Path(tmp) / "probe.c").write_text("int main(void) { return 7; }\n")
        try:
            if subprocess.run([cc, str(Path(tmp) / "probe.c"), "-o", str(exe)], capture_output=True).returncode:
                return None
            if subprocess.run([str(exe)], timeout=20).returncode != 7:
                return None
        except (OSError, subprocess.TimeoutExpired):
            return None
    return [cc]


def build_host(name: str, sources: list[Path], out: Path, extra: list[str] | None = None) -> None:
    cc = host_cc()
    if not cc:
        raise unittest.SkipTest("no C compiler whose programs run on this machine")
    subprocess.run(cc + ["-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                         "-ffreestanding", "-fno-builtin", f"-I{NATIVE / 'include'}", *(extra or []),
                         *map(str, sources), "-o", str(out)], check=True)


class CoreProfileTests(unittest.TestCase):
    """profile.c + budget.c in a host process (native_host/profile_host.c)."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name) / "profile_host"
        build_host("profile_host", [HERE / "native_host/profile_host.c"] +
                   [NATIVE / f"src/{n}.c" for n in ("profile", "budget", "automem", "nid")], cls.exe)
        rule_title = profiles.load("CUSA22222")
        rule_title["hle"] = [{"lib": "libSceNpManager", "function": "sceNpGetState", "value": "0x80550001",
                              "fill": [{"arg": 2, "offset": 4, "size": 4, "value": 7},
                                       {"arg": 2, "offset": 8, "zero": 4}]}]
        bound = copy.deepcopy(rule_title)
        bound["title"] = "CUSA33333"
        bound["eboot"] = {"fingerprint": "11" * 20}
        bound["exec"] = {"eboot.bin": [["0x0", "0x2c00000"], ["0x3000000", "0x3100000"]]}
        blobs = [profiles.compile_blob(profiles.load("CUSA11456")), profiles.compile_blob(rule_title),
                 profiles.compile_blob(bound), profiles.compile_blob(profiles.load("CUSA00900")),
                 profiles.compile_blob(profiles.load(None))]
        cls.bundle = Path(cls.tmp.name) / "bundle.bin"
        cls.bundle.write_bytes(b"".join(blobs) + bytes(8))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_host(self, *args: str, env: dict[str, str] | None = None) -> list[str]:
        out = subprocess.run([str(self.exe), str(self.bundle), *args], capture_output=True, text=True, timeout=60,
                             env={**os.environ, **(env or {})})
        self.assertEqual(out.returncode, 0, out.stderr)
        return out.stdout.splitlines()

    def test_ghost_profile_reproduces_the_logged_plan(self) -> None:
        lines = self.run_host("plan", GHOST_FP, "CUSA11456")
        budget = [ln for ln in lines if ln.startswith("budget:")]
        self.assertEqual(budget, ghost_lines())
        self.assertIn("title CUSA11456 thread CUSA11456-main has plan 2 hooks 4 diag 8 save 256 param1 11456",
                      lines)
        self.assertIn("patch rc 0 reported 4520 MiB, stream table 588 MiB, cpu heap 320 MiB", lines)
        self.assertIn("lazy site 0x896126 1, gate 0, flex gate 0", lines)     # no plan-free gates
        # host answers kept, but Neo mode: the plan is for memory mode 0 (Jak X answers 1)
        self.assertIn("neo 0, available direct ffffffff: 0 + 0 MiB", lines)

    def test_bloodborne_plan_shrinks_its_second_gpu_heap(self) -> None:
        """Bloodborne's config 1 asks for 4478 MiB of heaps; with Ghost's logged
        4532 MiB free, the first block shrinks to 32 MiB (patched), HAVOK and
        GFX keep their sizes with 120 and 220 MiB resident (the swap file:
        what crashes' memory census showed they use, with room), and
        GFX_GraphicsPrivateB absorbs the rest (4532 - 3750 fixed - 32 reserve -
        100 video)."""
        lines = self.run_host("bb", BLOODBORNE_FP)
        self.assertIn("title CUSA00900 has plan 2", lines)
        self.assertIn("budget: 4532 MiB free in 6 holes; GFX_GraphicsPrivateB heap 886 -> 650 MiB; reporting 4400 "
                      "MiB direct memory", lines)
        self.assertIn("patch rc 0 reported 4400 MiB; config 1: 524 276 152 2287 650 10 6 0 14 0 5 8 9 296 0 5", lines)

    def test_bloodborne_gpu_heap_knob_moves_memory_between_its_gpu_heaps(self) -> None:
        lines = self.run_host("bb", BLOODBORNE_FP, env={"GHOSTLDR_TEST_GPU_HEAP": "2087"})
        self.assertIn("budget: GFX_GraphicsPrivate heap 2287 -> 2087 MiB", lines)
        self.assertIn("patch rc 0 reported 4400 MiB; config 1: 524 276 152 2087 850 10 6 0 14 0 5 8 9 296 0 5", lines)

    def test_another_build_of_bloodborne_is_left_alone(self) -> None:
        lines = self.run_host("bb", "00" * 20)
        self.assertIn("title CUSA00900 has plan 0", lines)
        self.assertIn("config 1: 524 276 152 2287 886 10 6 0 14 0 5 8 9 296 0 5", lines[-1])

    def test_free_walk_steps_over_a_failing_query(self) -> None:
        # find-next fails inside the 0x13e00000-0x50080000 hole: the walk must
        # still find the allocations after it and count the same 4532 MiB
        lines = self.run_host("plan", GHOST_FP, "CUSA11456", env={"GHOSTLDR_TEST_DMQ_GAP": "0x13e00000-0x50080000"})
        self.assertEqual([ln for ln in lines if ln.startswith("budget:")], ghost_lines())

    def test_ps4_plans_with_what_the_kernel_hands_out(self) -> None:
        """A PS4 refused Ghost's 584 MiB pool with 1.9 GiB free in one hole (the
        fourth PS4 report): there the plan counts what the kernel hands out."""
        lines = self.run_host("plan", GHOST_FP, "CUSA11456", env={"GHOSTLDR_TEST_PS4_CAP": "4400"})
        self.assertIn("budget: PS4: the kernel hands out 4400 MiB of direct memory (type 0), 4400 MiB (type 3), "
                      "of 4532 MiB in free holes", lines)
        self.assertIn("budget: 4532 MiB free in 6 holes; streaming pool 1120 -> 456 MiB; reporting 4388 MiB direct "
                      "memory", lines)
        self.assertIn("ps4 cap: 0 MiB still taken after the plan", lines)

    def test_ps4_far_too_little_stops_the_boot(self) -> None:
        """The fifth PS4 report: 2480 MiB handed out, Ghost's plan ~1.6 GiB short,
        and a black screen once its lazily backed heaps ran out.  Now it stops."""
        lines = self.run_host("plan", GHOST_FP, "CUSA11456", env={"GHOSTLDR_TEST_PS4_CAP": "2480"})
        self.assertIn("budget: only 2480 MiB free; streaming pool clamped to 128 MiB", lines)
        self.assertIn("budget: PS4: CUSA11456 needs about 1592 MiB more direct memory than this PS4 lets SWRR "
                      "allocate: not started (docs/ps4.md)", lines)
        self.assertTrue(any(ln.startswith("patch rc -1 ") for ln in lines), lines)
        self.assertIn("ps4 cap: 0 MiB still taken after the plan", lines)

    def test_ps4_a_little_short_still_starts(self) -> None:
        lines = self.run_host("plan", GHOST_FP, "CUSA11456", env={"GHOSTLDR_TEST_PS4_CAP": "3900"})
        self.assertIn("budget: PS4: the title's plan needs about 172 MiB more direct memory than this console "
                      "hands out; it may fail when it allocates the rest", lines)
        self.assertIn("patch rc 0 reported 4060 MiB, stream table 128 MiB, cpu heap 320 MiB", lines)

    def test_ps4_with_room_plans_as_a_ps5(self) -> None:
        lines = self.run_host("plan", GHOST_FP, "CUSA11456", env={"GHOSTLDR_TEST_PS4_CAP": "9000"})
        self.assertEqual([ln for ln in lines if ln.startswith("budget:") and "PS4" not in ln], ghost_lines())

    def test_ghost_is_found_by_fingerprint_without_ghostfs(self) -> None:
        lines = self.run_host("select", GHOST_FP)
        self.assertTrue(lines[0].startswith("profile: CUSA11456 own profile"))
        self.assertIn("budget: 4532 MiB free in 6 holes; streaming pool 1120 -> 588 MiB; reporting 4520 MiB direct "
                      "memory", lines)

    def test_another_build_of_ghost_gets_the_generic_plan_free_mode(self) -> None:
        lines = self.run_host("plan", "00" * 20, "CUSA11456", "v")
        self.assertIn("is not the profile's", lines[0])
        self.assertIn("title CUSA11456 thread CUSA11456-main has plan 0 hooks 0 diag 0 save 1024 param1 0", lines)
        # 4532 free - 64 ghostldr - 100 video - 116 (Ghost's own flexible memory size)
        self.assertIn("patch rc 0 reported 4252 MiB, stream table 1120 MiB, cpu heap 384 MiB", lines)
        self.assertIn("lazy site 0x896126 0, gate 80020023, flex gate 8002000c", lines)
        self.assertIn("neo 0, available direct 0: 0 + 4252 MiB", lines)

    def test_plan_free_homebrew(self) -> None:
        lines = self.run_host("planfree", "-", "BREW00042", "n")
        self.assertIn("patch rc 0 reported 4020 MiB, stream table 1120 MiB, cpu heap 384 MiB", lines)  # 448 flex
        self.assertIn("neo 0, available direct 0: 0 + 4020 MiB", lines)
        lines = self.run_host("planfree", "-", "BREW00042", "nv")
        self.assertIn("neo 0, available direct 0: 0 + 3920 MiB", lines)

    def test_plan_free_anonymous_mmap_is_flexible_memory(self) -> None:
        # musl's malloc (OpenOrbis homebrew) grows through mmap(MAP_ANON): it gets the
        # title's flexible budget (448 MiB without a mem param), zero-filled, never SWRR's
        lines = self.run_host("mmap", "-", "BREW00042")
        self.assertIn("anon mapped, zeroed 1, flexible left 447 MiB", lines)
        self.assertIn("munmap 0", lines)
        self.assertIn("after munmap flexible left 448 MiB", lines)
        self.assertIn("too big MAP_FAILED errno 12", lines)                       # ENOMEM past the budget
        self.assertIn("sceKernelMmap read-only 0 mapped 1 mprotect 1", lines)     # mapped RW, zeroed, then read-only
        self.assertIn("reserve 0x7000000000", lines)                             # PROT_NONE: address space only
        self.assertIn("file 0x1234000", lines)                                   # a file: the extension's mmap
        self.assertTrue([ln for ln in lines if ln.startswith("mmap anonymous 1c100000 ") and " -> 8002000c " in ln])

    def test_hle_rule_fills_and_returns(self) -> None:
        lines = self.run_host("rules", "-", "CUSA22222", "sceNpGetState", "libSceNpManager")
        self.assertIn("rule stub 5000 returns 80550001, out aaaaaaaa0700000000000000aaaaaaaa", lines)
        lines = self.run_host("rules", "-", "CUSA22222", "sceNpGetState", "libSceOther")
        self.assertIn("rule stub 0 returns 0, out aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", lines)

    def test_rules_of_a_bound_profile_need_its_build(self) -> None:
        lines = self.run_host("rules", "11" * 20, "CUSA33333", "sceNpGetState", "libSceNpManager")
        self.assertIn("rule stub 5000 returns 80550001, out aaaaaaaa0700000000000000aaaaaaaa", lines)
        lines = self.run_host("rules", "22" * 20, "CUSA33333", "sceNpGetState", "libSceNpManager")
        self.assertIn("rule stub 0 returns 0, out aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", lines)

    def test_exec_ranges_only_on_the_profiles_build(self) -> None:
        """bigexec.c reads a profile's code ranges (REC_EXEC) on its own build only."""
        lines = self.run_host("exec", "11" * 20, "CUSA33333")
        self.assertIn("exec eboot.bin 0-2c00000", lines)
        self.assertIn("exec eboot.bin 3000000-3100000", lines)
        self.assertIn("exec records 1", lines)
        self.assertIn("exec records 0", self.run_host("exec", "22" * 20, "CUSA33333"))
        self.assertIn("exec records 0", self.run_host("exec", GHOST_FP, "CUSA11456"))


ANOTHER_GHOST = "00" * 20      # a build of Ghost its profile does not name: plan-free


class AutoMemoryTests(unittest.TestCase):
    """automem.c: Ghost's allocations replayed boot after boot (profile_host
    boot), the learned plan kept in a file between them, against 4252 MiB of
    real memory unless a run says otherwise."""

    @classmethod
    def setUpClass(cls) -> None:
        CoreProfileTests.setUpClass.__func__(cls)
        cls.text = None
        dump = profiles.DEFAULT_SRC / "eboot.bin"
        if dump.exists() and profiles.eboot_fingerprint(profiles.DEFAULT_SRC) == GHOST_FP:
            img = oe.parse(dump)
            text = next(p for p in img.phdrs if p.type == oe.PT_LOAD and p.vaddr == 0)
            cls.text = Path(cls.tmp.name) / "text.bin"
            cls.text.write_bytes(img.read_vaddr(0, text.filesz))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def setUp(self) -> None:
        self.plan = Path(self.tmp.name) / f"{self.id()}.bin"
        self.plan.unlink(missing_ok=True)

    def boot(self, run: str = "", fp: str = ANOTHER_GHOST, **env: str) -> list[str]:
        out = subprocess.run([str(self.exe), str(self.bundle), "boot", fp, "CUSA11456", run], capture_output=True,
                             text=True, timeout=60, env={**os.environ, "GHOSTLDR_TEST_AUTOMEM": str(self.plan), **env})
        self.assertEqual(out.returncode, 0, out.stderr)
        return [ln for ln in out.stdout.splitlines() if ln.startswith(("automem:", "run:"))]

    def test_first_boot_is_the_plan_free_budget_with_lazy_commit(self) -> None:
        lines = self.boot("c1500")
        self.assertIn("automem: told 4352 MiB of direct memory (4352 MiB for real); allocations of 64 MiB or more "
                      "are committed as they are mapped, and admitted past that much", lines)
        # a refusal would have stopped it at the 1096 MiB heap: it reserves all 5050 MiB, then runs out for real
        self.assertIn("automem: 4934 MiB reserved, past the 4352 MiB the title was told: admitted, committed as it "
                      "is mapped", lines)
        self.assertIn("run: told 4352 MiB; cpu 384, engine 332, gpu 1390, stream 1120 MiB; 4252 MiB committed, "
                      "stopped", lines)

    def test_it_learns_to_shrink_the_streaming_pool_first(self) -> None:
        self.boot("c1500")
        second = self.boot("c1500")
        self.assertIn("automem: learned plan, boot 2 of this build: last time told 4352 MiB, reserved at most 5050, "
                      "held 4252, ran out of memory; its heaps left 798 MiB unused", second)
        self.assertIn("automem: the 1120 MiB pool from eboot+eee115 (its size varies 419% across memory modes) "
                      "shrinks to 658 MiB, 3 size cells", second)
        self.assertIn("automem: eboot+eee115 allocated 658 MiB: the size cells were its pool's", second)
        third = self.boot("c1500")
        self.assertIn("automem: the 1120 MiB pool from eboot+eee115 (its size varies 419% across memory modes) "
                      "shrinks to 560 MiB, 3 size cells", third)     # half its size at the most
        self.assertIn("automem: the 332 MiB pool from eboot+390c4c (its size varies 313% across memory modes) "
                      "shrinks to 198 MiB, 2 size cells", third)
        self.assertIn("run: told 4352 MiB; cpu 384, engine 198, gpu 1390, stream 560 MiB; 4032 MiB committed", third)
        fourth = self.boot("c1500")          # it fits: told what it reserves, within what its heaps leave unused
        self.assertIn("automem: told 4420 MiB of direct memory (4352 MiB for real); allocations of 64 MiB or more "
                      "are committed as they are mapped, and admitted past that much", fourth)
        self.assertIn("run: told 4420 MiB; cpu 384, engine 198, gpu 1390, stream 560 MiB; 4032 MiB committed", fourth)
        self.assertFalse([ln for ln in fourth if "shrinks" in ln])

    def test_a_title_that_fits_is_told_what_its_heaps_leave_unused(self) -> None:
        self.boot("c200p9000")
        lines = self.boot("c200p9000")
        self.assertIn("automem: learned plan, boot 2 of this build: last time told 4352 MiB, reserved at most 5050, "
                      "held 3426, did not run out; its heaps left 1624 MiB unused", lines)
        self.assertIn("run: told 5114 MiB; cpu 384, engine 332, gpu 1390, stream 1120 MiB; 3426 MiB committed", lines)

    def test_cells_that_were_not_the_pools_are_never_patched_again(self) -> None:
        self.boot("c1500w")
        second = self.boot("c1500w")      # its streaming pool's size comes from somewhere else
        self.assertIn("automem: eboot+eee115 allocated 1120 MiB: the size cells were not its pool's (never patched "
                      "again)", second)
        third = self.boot("c1500w")
        self.assertFalse([ln for ln in third if "eboot+eee115" in ln], third)
        self.assertIn("automem: the 332 MiB pool from eboot+390c4c (its size varies 313% across memory modes) "
                      "shrinks to 166 MiB, 2 size cells", third)

    def test_two_early_stops_undo_the_last_shrink(self) -> None:
        for run in ("c1500", "c1500", "c1500", "e"):
            self.boot(run)
        lines = self.boot("e")
        self.assertIn("automem: learned plan, boot 5 of this build: last time told 4420 MiB, reserved at most 0, "
                      "held 0, did not run out but stopped within two minutes; its heaps left 0 MiB unused", lines)
        lines = self.boot("c1500")
        self.assertIn("automem: the title twice stopped within two minutes after eboot+eee115's pool shrank: back to "
                      "658 MiB, the size it ran with, and no smaller from now on", lines)
        self.assertIn("automem: the title twice stopped within two minutes after eboot+390c4c's pool shrank: it keeps "
                      "its 332 MiB from now on", lines)
        lines = self.boot("c1500")         # the next pools by elasticity: the CPU heap, then the GPU heap
        self.assertIn("automem: the 384 MiB pool from eboot+a22119 (its size varies 251% across memory modes) "
                      "shrinks to 192 MiB, 2 size cells", lines)
        self.assertFalse([ln for ln in lines if "pool from eboot+eee115" in ln or "pool from eboot+390c4c" in ln])

    def test_nothing_left_to_shrink_refuses_past_what_it_is_told(self) -> None:
        for _ in range(6):      # the pools down to their floors one boot after another, then nothing left
            lines = self.boot("c1700p3200")
        self.assertIn("automem: out of memory with no pool left to shrink: allocations past what the title is told "
                      "are refused from now on", lines)
        lines = self.boot("c1700p3200")
        self.assertIn("automem: told 4352 MiB of direct memory (4352 MiB for real); allocations of 64 MiB or more are "
                      "committed as they are mapped", lines)
        self.assertFalse([ln for ln in lines if "admitted" in ln])

    def seed_bundle(self, plan: bytes, contributors: int = 3) -> Path:
        """Ghost's profiles with a community plan (REC_SEED) for ANOTHER_GHOST on a PS5."""
        out = Path(self.tmp.name) / f"{self.id()}-seed.bundle"
        out.write_bytes(profiles.boot_bundle("CUSA11456", seeds={"CUSA11456": [
            {"fingerprint": ANOTHER_GHOST, "platform": "ps5", "contributors": contributors, "data": plan}]}))
        return out

    def boot_with(self, bundle: Path, run: str, **env: str) -> list[str]:
        out = subprocess.run([str(self.exe), str(bundle), "boot", ANOTHER_GHOST, "CUSA11456", run],
                             capture_output=True, text=True, timeout=60,
                             env={**os.environ, "GHOSTLDR_TEST_AUTOMEM": str(self.plan), **env})
        self.assertEqual(out.returncode, 0, out.stderr)
        return [ln for ln in out.stdout.splitlines() if ln.startswith(("automem:", "run:"))]

    def test_a_build_without_a_plan_starts_from_the_community_plan(self) -> None:
        """REC_SEED (docs/community-plans.md): another console's plan becomes last boot's, and learning goes on."""
        self.boot("c1500")
        shared = self.plan.read_bytes()             # one console's plan after its first boot
        self.plan.unlink()                           # this console has none for the build
        bundle = self.seed_bundle(shared)
        lines = self.boot_with(bundle, "c1500")
        self.assertIn("automem: no plan of its own for this build: starting from the community plan (from 3 consoles, "
                      "1 boots)", lines)
        self.assertIn("automem: learned plan, boot 2 of this build: last time told 4352 MiB, reserved at most 5050, "
                      "held 4252, ran out of memory; its heaps left 798 MiB unused", lines)
        self.assertIn("automem: the 1120 MiB pool from eboot+eee115 (its size varies 419% across memory modes) "
                      "shrinks to 658 MiB, 3 size cells", lines)
        lines = self.boot_with(bundle, "c1500")      # its own from now on: boot 3, the community plan left alone
        self.assertFalse([ln for ln in lines if "community plan" in ln], lines)
        self.assertTrue([ln for ln in lines if "learned plan, boot 3 of this build" in ln], lines)

    def test_community_plans_off_and_other_builds_learn_from_scratch(self) -> None:
        self.boot("c1500")
        bundle = self.seed_bundle(self.plan.read_bytes())
        self.plan.unlink()
        lines = self.boot_with(bundle, "c1500", GHOSTLDR_TEST_FLAGS=str(0x10000000))  # CTX_FLAG_NO_COMMUNITY_MEM
        self.assertFalse([ln for ln in lines if "community plan" in ln], lines)
        self.assertIn("automem: told 4352 MiB of direct memory (4352 MiB for real); allocations of 64 MiB or more "
                      "are committed as they are mapped, and admitted past that much", lines)
        self.plan.unlink()
        lines = self.boot(fp="33" * 20)             # another build: the seed isn't for it
        self.assertFalse([ln for ln in lines if "community plan" in ln], lines)

    def test_a_plan_that_isnt_one_is_not_used(self) -> None:
        lines = self.boot_with(self.seed_bundle(b"\0" * 1080), "c1500")
        self.assertIn("automem: the community plan for this build is not one this ghostldr reads (1080 bytes): not "
                      "used", lines)

    def test_sharing_writes_the_outbox(self) -> None:
        lines = self.boot("c1500", GHOSTLDR_TEST_FLAGS=str(0x8000000))                  # CTX_FLAG_SHARE_MEM
        self.assertIn(f"automem: outbox share-CUSA11456-{ANOTHER_GHOST}-ps5.bin 1080 bytes", lines)
        self.assertFalse([ln for ln in self.boot("c1500") if "outbox" in ln])           # not without the switch

    def test_forget_and_other_builds_start_over(self) -> None:
        self.boot("c1500")
        self.boot("c1500")
        lines = self.boot("c1500F")
        self.assertFalse([ln for ln in lines if "learned plan" in ln or "pool from" in ln])
        self.assertIn("run: told 4352 MiB; cpu 384, engine 332, gpu 1390, stream 1120 MiB; 4252 MiB committed, "
                      "stopped", lines)
        lines = self.boot("c1500", fp="11" * 20)
        self.assertFalse([ln for ln in lines if "learned plan" in ln])

    def test_a_manual_plan_wins_unless_auto_memory_is_asked_for(self) -> None:
        lines = self.boot("c1500", fp=GHOST_FP)
        self.assertFalse([ln for ln in lines if ln.startswith("automem:")])
        self.assertIn("run: told 4520 MiB; cpu 320, engine 332, gpu 1390, stream 588 MiB; 4130 MiB committed", lines)
        lines = self.boot("c1500A", fp=GHOST_FP)
        self.assertIn("automem: told 4284 MiB of direct memory (4284 MiB for real); allocations of 64 MiB or more "
                      "are committed as they are mapped, and admitted past that much", lines)
        self.assertIn("run: told 4284 MiB; cpu 384, engine 332, gpu 1390, stream 1120 MiB; 4252 MiB committed, "
                      "stopped", lines)

    def test_ghosts_own_executable(self) -> None:
        """The same, on the text segment of the CUSA11456 dump: each pool's size
        is found in its own table only, and the streaming pool ranks first."""
        if not self.text:
            self.skipTest("needs the CUSA11456 dump in build/ghost/app0")
        self.boot("c1500", GHOSTLDR_TEST_TEXT=str(self.text))
        lines = self.boot("c1500", GHOSTLDR_TEST_TEXT=str(self.text))
        self.assertIn("automem: the 1120 MiB pool from eboot+eee115 (its size varies 419% across memory modes) "
                      "shrinks to 658 MiB, 3 size cells", lines)
        self.assertIn("automem: eboot+eee115 pool 1120 -> 658 MiB: 3 of 3 cells written", lines)
        lines = self.boot("c1500", GHOSTLDR_TEST_TEXT=str(self.text))
        self.assertIn("automem: the 332 MiB pool from eboot+390c4c (its size varies 313% across memory modes) "
                      "shrinks to 198 MiB, 2 size cells", lines)


if __name__ == "__main__":
    unittest.main()
