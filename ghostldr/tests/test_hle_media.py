"""The media and peripheral families (ext/src/hle_media.c, hle_peripheral.c,
hle_mode.c) in the built ext.bin: their tables, decoded offline, and their
code, run against a fake host in an x86-64 process (ext_host/fam_host.c,
Rosetta on Apple silicon) through the core's real ext.c.  Needs
`make -C ghostldr/native` first.

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

from test_ext import EXT, ROOT, x86_64_cc

sys.path.insert(0, str(ROOT / "ghostldr/tools"))

FAMILIES = ("media", "peripheral")
# every library of the research spec, with the aliases titles import it under
LIBRARIES = {
    "libSceGameLiveStreaming": {"libSceGameLiveStreamingCompat", "libSceGameLiveStreaming_debug",
                                "libSceGameLiveStreaming_direct_streaming"},
    "libSceScreenShot": {"libSceScreenShotDrc"},
    "libSceShareUtility": set(),
    "libSceSharePlay": {"libSceSharePlayCompat"},
    "libSceRemoteplay": set(),
    "libSceVideoRecording": set(),
    "libSceCompanionHttpd": set(),
    "libSceCompanionUtil": set(),
    "libSceContentExport": set(),
    "libSceVoice": set(),
    "libSceVoiceQoS": set(),
    "libSceAudio3d": set(),
    "libSceRudp": set(),
    "libSceRazorCpu": set(),
    "libSceCamera": set(),
    "libSceMove": set(),
    "libSceMoveTracker": set(),
    "libSceHmd": {"libSceHmdDistortion", "libsceHmdReprojectionMultilayer"},
    "libSceHmdSetupDialog": set(),
    "libSceVrTracker": {"libSceVrTrackerFourDeviceAllowed", "libSceVrTrackerDeviceRejection",
                        "libSceVrTrackerLiveCapture"},
    "libSceUsbd": set(),
    "libSceKeyboard": set(),
    "libSceDbgKeyboard": set(),
    "libSceMouse": set(),
    "libSceSystemGesture": set(),
}
# libraries that must never reach SWRR's own instance (its companion server,
# system UI, profiler markers)
NO_HOST = {"libSceCompanionHttpd", "libSceCompanionUtil", "libSceRazorCpu", "libSceHmdSetupDialog",
           "libSceDbgKeyboard"}
# a sample of the spec's NIDs: the names registered must hash to them
SPEC_NIDS = {
    "kvYEw2lBndk": "sceGameLiveStreamingInitialize", "9yK6Fk8mKOQ": "sceGameLiveStreamingTerminate",
    "j7DlalBzHh8": "sceShareUtilityInitializeEx2", "8hZ2EEl2Tto": "sceShareUtilityOpenShareMenuDefault",
    "isruqthpYcw": "sceSharePlayInitialize", "mrNh78tBpmg": "sceRemoteplayProhibit",
    "s28dalBwp2g": "sceVideoRecordingOpen2", "Vku4big+IYM": "sceCompanionHttpdGetEvent",
    "XeDDK0xJWQA": "sceAudio3dPortOpen", "lvNClhNHzxI": "sceVoiceQoSCreateLocalEndpoint",
    "zw+celG7zSI": "sceRazorCpuPushMarker", "BHn83xrF92E": "sceCameraOpen", "ttU+JOhShl4": "sceMoveReadStateLatest",
    "thDt9upZlp8": "sceHmdGetDeviceInformation", "K7yhYrsIBPc": "sceVrTrackerQueryMemory",
    "8qB9Ar4P5nc": "sceUsbdGetDeviceList", "x8qnXqh-tiM": "sceMouseRead", "6HpE68bzX6M": "sceKeyboardReadState",
    "3pcAvmwKCvM": "sceSystemGestureInitializePrimitiveTouchRecognizer",
    "D5JfdpJKvXk": "sceHmdGetDistortionWorkMemorySize", "36xDKk+Hw7o": "sceHmdDistortionGetWorkMemorySize",
}
RUN_TIMEOUT = 120  # seconds: a wedged Rosetta must fail the test, not hang the suite


@unittest.skipUnless(EXT.exists(), "build/ghostldr/ext.bin missing: run make -C ghostldr/native")
class Tables(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        import check_ext
        import gen_nids
        cls.blob = check_ext.Blob(EXT.read_bytes())
        cls.fams = {f.name: f for f in cls.blob.families()}
        cls.db = gen_nids.name_db()
        cls.report = check_ext.Report()
        cls.model = check_ext.ExtModel(list(cls.fams.values()), cls.report)

    def entries(self):
        for fam in FAMILIES:
            yield from self.fams[fam].hle

    def test_every_library_has_its_policy_and_aliases(self) -> None:
        libs = {lib.name: lib for fam in FAMILIES for lib in self.fams[fam].libs}
        self.assertEqual(set(libs), set(LIBRARIES))
        for name, aliases in LIBRARIES.items():
            self.assertEqual(set(libs[name].aliases), aliases, name)
            self.assertEqual(libs[name].no_host, name in NO_HOST, name)
            for alias in aliases:
                self.assertIs(self.model.policy(alias), libs[name])

    def test_every_name_is_known_and_registered_once(self) -> None:
        import orbis_elf as oe
        names = set(self.db.values())
        seen = set()
        for e in self.entries():
            self.assertIn(e.name, names, f"{e.lib}:{e.name} is in no name list")
            key = (self.model.canon(e.lib), oe.compute_nid(e.name))
            self.assertNotIn(key, seen, f"{e.lib}:{e.name} registered twice")
            seen.add(key)
            self.assertIn(self.model.canon(e.lib), LIBRARIES, e.lib)
            self.assertTrue(0 <= e.fn < self.blob.text_size, e.name)
        self.assertGreater(len(seen), 500)

    def test_names_hash_to_the_spec_nids(self) -> None:
        import orbis_elf as oe
        registered = {e.name for e in self.entries()}
        for nid, name in SPEC_NIDS.items():
            self.assertEqual(oe.compute_nid(name), nid)
            self.assertIn(name, registered)

    def test_routed_functions_have_one_trampoline_each(self) -> None:
        # routed entries point at 16-byte trampolines: "leaq -7(%rip), %r11; jmp hle_gen_entry"
        img = self.blob.image()
        for fam in FAMILIES:
            tramps = [e.fn for e in self.fams[fam].hle if img[e.fn:e.fn + 7] == b"\x4c\x8d\x1d\xf9\xff\xff\xff"]
            self.assertGreater(len(tramps), 100, fam)
            self.assertEqual(len(set(tramps)), len(tramps), fam)
            self.assertEqual(max(tramps) - min(tramps), 16 * (len(tramps) - 1), fam)
            entry = {struct.unpack_from("<i", img, t + 8)[0] + t + 12 for t in tramps}
            self.assertEqual(len(entry), 1, f"{fam}: trampolines jump to one dispatcher")

    @unittest.skipUnless((ROOT / "build/ghost/app0/eboot.bin").exists(), "no title in build/ghost/app0")
    def test_ghost_imports_of_these_libraries_all_bind_to_the_extension(self) -> None:
        import gen_nids
        import orbis_elf as oe
        found = 0
        for f in gen_nids.title_files(ROOT / "build/ghost/app0"):
            for sym, lib, _ in oe.parse(f).imports():
                if lib and self.model.canon(lib.name) in LIBRARIES:
                    found += 1
                    self.assertIsNotNone(self.model.lookup(lib.name, sym.nid), f"{lib.name}:{sym.nid}")
        self.assertGreater(found, 0)


@unittest.skipUnless(EXT.exists(), "build/ghostldr/ext.bin missing: run make -C ghostldr/native")
class OnHost(unittest.TestCase):
    """fam_host: the families' code against a fake host (see fam_host.c)."""

    @classmethod
    def setUpClass(cls) -> None:
        cc = x86_64_cc()
        if not cc:
            raise unittest.SkipTest("no way to build and run x86-64 code here")
        cls.blob = EXT.read_bytes()
        cls.crc = hex(zlib.crc32(cls.blob))
        abi, = struct.unpack_from("<I", cls.blob, 20)
        cls.tmp = tempfile.TemporaryDirectory()
        here = Path(__file__).resolve().parent
        flags = ["-O1", "-std=gnu11", "-fno-builtin", "-Wall", "-Wextra", "-Wno-unused-parameter",
                 "-Wno-deprecated-non-prototype", "-Werror", f"-I{ROOT / 'ghostldr/native/include'}",
                 f"-DGHOST_ABI={abi}u"]
        core = ROOT / "ghostldr/native/src"
        objs = []
        for src, extra in [(core / "ext.c", flags), (core / "nid.c", flags), (here / "ext_host/fam_host.c", flags),
                           (here / "ext_host/sys.c", ["-O1"])]:
            obj = Path(cls.tmp.name) / (src.stem + ".o")
            subprocess.run(cc + extra + ["-c", str(src), "-o", str(obj)], check=True)
            objs.append(str(obj))
        cls.host = Path(cls.tmp.name) / "fam_host"
        subprocess.run(cc + objs + ["-o", str(cls.host)], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_host(self, scenario: str) -> str:
        p = subprocess.run([str(self.host), scenario, str(EXT), self.crc], capture_output=True, text=True,
                           timeout=RUN_TIMEOUT)
        self.assertNotIn("FAILED", p.stdout)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return p.stdout

    def test_swrr_on_13_60(self) -> None:
        out = self.run_host("swrr")
        # each library's mode, chosen once and logged
        for line in ("hle_media: libSceGameLiveStreaming -> MASKED (sceGameLiveStreamingInitialize -> 80a00003",
                     "hle_media: libSceShareUtility -> VIRT (sceShareUtilityInitializeEx2 -> 81170105: refused)",
                     "hle_media: libSceSharePlay -> MASKED", "hle_media: libSceRemoteplay -> MASKED",
                     "hle_media: libSceVideoRecording -> VIRT (sceVideoRecordingOpen2 -> 80a80006: refused)",
                     "hle_media: libSceAudio3d -> PASS", "hle_media: libSceVoiceQoS -> PASS",
                     "hle_media: libSceContentExport -> MASKED",
                     "hle_media: libSceCompanionHttpd -> VIRT (start", "hle_media: libSceVoice -> VIRT (start",
                     "hle_periph: libSceCamera -> VIRT (sceCameraOpen: crash class",
                     "hle_periph: libSceMove -> VIRT (sceMoveInit: not on the host)",
                     "hle_periph: libSceHmd -> PASS", "hle_periph: libSceHmd -> VIRT (sceHmdOpen -> 81110016: refused)",
                     "hle_periph: libSceVrTracker -> VIRT (start",
                     "hle_periph: libSceUsbd -> VIRT (sceUsbdInit -> 80240003: refused)",
                     "hle_periph: libSceMouse -> PASS", "hle_periph: libSceKeyboard -> PASS",
                     "hle_media: sceShareUtilityOpenShareMenuDefault answered virtually"):
            self.assertIn(line, out)
        self.assertNotIn("libSceGameLiveStreaming -> VIRT", out)
        self.assertIn("sceAudio3dPortOpen(user ff) -> 0, port 7", out)

    def test_host_that_allows_everything(self) -> None:
        out = self.run_host("allow")
        self.assertIn("hle_media: libSceShareUtility -> PASS", out)
        self.assertIn("hle_media: libSceGameLiveStreaming -> PASS", out)

    def test_keep_remote_play_flag(self) -> None:
        self.run_host("keeprp")

    def test_paths_under_ghostfs(self) -> None:
        out = self.run_host("gfs")
        self.assertIn("sceScreenShotSetOverlayImage(/app0/shot.png): in ghostfs", out)

    def test_a_failed_sysmodule_load_makes_the_library_virtual(self) -> None:
        out = self.run_host("sysmodule")
        self.assertIn("hle_periph: libSceMouse -> VIRT (sceSysmoduleLoadModule -> 80020008: its sysmodule did not load)",
                      out)

    def test_an_import_the_host_lacks_makes_the_library_virtual_at_boot(self) -> None:
        out = self.run_host("scan")
        boot, _, calls = out.partition("ext: self-test")
        self.assertIn("hle_periph: libSceKeyboard -> VIRT (sceKeyboardInit: its module does not load)", boot)


if __name__ == "__main__":
    unittest.main()
