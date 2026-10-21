"""Title profiles: parsing, merging and validation (host/profiles.py), the
compiled blobs, and the core's side of them: native/src/profile.c and
budget.c built for this machine (native_host/profile_host.c) and run
against fakes of the console.

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import copy
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
        self.assertEqual(b["flags"], profiles.PF_PLAN | profiles.PF_HOOKS | profiles.PF_DIAG | profiles.PF_OWN)
        self.assertEqual(b["app_params"], [3, 11456, 0, 0, 0])   # cache/t11456.dat
        self.assertEqual((b["save_blocks"], b["runtime_reserve_mib"], b["video_reserve_mib"], b["flex_mib"]),
                         (256, 32, 100, 0))
        kinds = [k for k, _ in b["records"]]
        self.assertEqual(kinds, [profiles.REC_PLAN] + [profiles.REC_TABLE] * 4)
        fixed, over, n, *sites = struct.unpack_from("<6I", b["records"][0][1])
        self.assertEqual((fixed, over, n, sites), (3996, 120, 3, [0x896126, 0x89614A, 0xEEE2CF]))
        table, role, knob, nexp, mib, min_mib = struct.unpack_from("<IBBHII", b["records"][1][1])
        self.assertEqual((table, role, knob, nexp, mib, min_mib), (0x111CD50, 1, 0xFF, 6, 0, 128))
        self.assertTrue(b["records"][1][1][16 + 48:].startswith(b"streaming pool\0"))

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
        self.assertEqual(profiles.default_prefix("CUSA12345"), "G12345")
        self.assertEqual(profiles.default_prefix("HB-TEST_1"), "GHBTEST1")
        self.assertEqual(profiles.normalize_title(" lapy10010 "), "LAPY10010")

    def test_param_sfo_title(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp)
            (src / "sce_sys").mkdir()
            (src / "sce_sys/param.sfo").write_bytes(make_sfo({"TITLE_ID": "BREW00042", "TITLE": "Homebrew",
                                                              "APP_VER": "01.00", "USER_DEFINED_PARAM_1": 42}))
            self.assertEqual(profiles.detect_title(src), "BREW00042")
            self.assertEqual(profiles.app_params(profiles.load("BREW00042"), src), [3, 42, 0, 0, 0])

    def test_dump_is_detected_by_its_fingerprint(self) -> None:
        src = profiles.DEFAULT_SRC
        if not (src / "eboot.bin").exists():
            self.skipTest("needs the CUSA11456 dump in build/ghost/app0")
        self.assertEqual(profiles.eboot_fingerprint(src), GHOST_FP)
        self.assertEqual(profiles.detect_title(src), "CUSA11456")


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


def build_host(name: str, sources: list[Path], out: Path) -> None:
    cc = host_cc()
    if not cc:
        raise unittest.SkipTest("no C compiler whose programs run on this machine")
    subprocess.run(cc + ["-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                         "-ffreestanding", "-fno-builtin", f"-I{NATIVE / 'include'}", *map(str, sources), "-o",
                         str(out)], check=True)


class CoreProfileTests(unittest.TestCase):
    """profile.c + budget.c in a host process (native_host/profile_host.c)."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name) / "profile_host"
        build_host("profile_host", [HERE / "native_host/profile_host.c"] +
                   [NATIVE / f"src/{n}.c" for n in ("profile", "budget", "nid")], cls.exe)
        rule_title = profiles.load("CUSA22222")
        rule_title["hle"] = [{"lib": "libSceNpManager", "function": "sceNpGetState", "value": "0x80550001",
                              "fill": [{"arg": 2, "offset": 4, "size": 4, "value": 7},
                                       {"arg": 2, "offset": 8, "zero": 4}]}]
        bound = copy.deepcopy(rule_title)
        bound["title"] = "CUSA33333"
        bound["eboot"] = {"fingerprint": "11" * 20}
        blobs = [profiles.compile_blob(profiles.load("CUSA11456")), profiles.compile_blob(rule_title),
                 profiles.compile_blob(bound), profiles.compile_blob(profiles.load(None))]
        cls.bundle = Path(cls.tmp.name) / "bundle.bin"
        cls.bundle.write_bytes(b"".join(blobs) + bytes(8))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_host(self, *args: str) -> list[str]:
        out = subprocess.run([str(self.exe), str(self.bundle), *args], capture_output=True, text=True, timeout=60)
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
        self.assertIn("neo host, available direct ffffffff: 0 + 0 MiB", lines)  # host answers kept

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
        self.assertTrue([ln for ln in lines if "no bundled libc.prx" in ln])
        self.assertIn("neo 0, available direct 0: 0 + 4020 MiB", lines)
        lines = self.run_host("planfree", "-", "BREW00042", "nv")
        self.assertIn("neo 0, available direct 0: 0 + 3920 MiB", lines)

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


if __name__ == "__main__":
    unittest.main()
