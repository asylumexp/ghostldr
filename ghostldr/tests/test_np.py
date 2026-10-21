"""The NP and trophy families (ext/src/hle_np.c, hle_trophy.c) run from the
built ext.bin in an x86-64 process (ext_host/np_host.c, Rosetta on Apple
silicon): the signed-out persona, request lifecycles, callbacks and trophy
records.  Needs `make -C ghostldr/native` first.

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

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
EXT = ROOT / "build/ghostldr/ext.bin"
ELF = ROOT / "build/ghostldr/ext.elf"
sys.path.insert(0, str(ROOT / "ghostldr/tools"))
sys.path.insert(0, str(HERE))


def param_sfo(values: dict[str, str]) -> bytes:
    """A param.sfo with UTF-8 string values."""
    index, keys, data = b"", b"", b""
    for key, value in values.items():
        v = value.encode() + b"\0"
        cap = (len(v) + 3) & ~3
        index += struct.pack("<HHIII", len(keys), 0x0204, len(v), cap, len(data))
        keys += key.encode() + b"\0"
        data += v.ljust(cap, b"\0")
    keys = keys.ljust((len(keys) + 3) & ~3, b"\0")
    key_start = 0x14 + len(index)
    return struct.pack("<4sIIII", b"\0PSF", 0x101, key_start, key_start + len(keys), len(values)) + index + keys + data


def trophy_trp(names: list[str]) -> bytes:
    """A trophy00.trp entry table (big endian; the files themselves are left out)."""
    header = struct.pack(">IIQII", 0xDCA24D00, 3, 0, len(names), 0x40).ljust(0x60, b"\0")
    return header + b"".join(struct.pack(">32sQQI12x", n.encode(), 0, 0, 0) for n in names)


@unittest.skipUnless(EXT.exists() and ELF.exists(), "build/ghostldr/ext.bin missing: run make -C ghostldr/native")
class NpFamiliesOnHost(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        from test_ext import x86_64_cc
        import check_ext
        import elf64
        cc = x86_64_cc()
        if not cc:
            raise unittest.SkipTest("no way to build and run x86-64 code here")
        blob = EXT.read_bytes()
        cls.crc = hex(zlib.crc32(blob))
        abi, = struct.unpack_from("<I", blob, 20)
        trophy = next(f for f in check_ext.Blob(blob).families() if f.name == "trophy")
        unlock = next(e.fn for e in trophy.hle if e.name == "sceNpTrophyUnlockTrophy")
        syms = elf64.Elf(ELF).symbols
        cls.offsets = [hex(unlock), hex(syms["title_store_read"].value), hex(syms["title_store_write"].value)]
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = Path(cls.tmp.name)
        flags = ["-O1", "-std=gnu11", "-fno-builtin", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror",
                 f"-I{ROOT / 'ghostldr/native/include'}", f"-DGHOST_ABI={abi}u"]
        core = ROOT / "ghostldr/native/src"
        # env.c loads the extension as in test_ext.py; np_host.c has the main and the API table
        sources = [(core / f"{name}.c", flags) for name in ("ext", "resolve", "elf", "nid")] + [
            (HERE / "ext_host/env.c", flags + ["-Dmain=ext_host_main", "-Dg_ghost_api=ext_host_api"]),
            (HERE / "ext_host/np_host.c", flags),
            (HERE / "ext_host/sys.c", ["-O1"]),
            (HERE / "ext_host/np_sys.c", ["-O1"]),
        ]
        objs = []
        for src, extra in sources:
            obj = tmp / (src.stem + ".o")
            subprocess.run(cc + extra + ["-c", str(src), "-o", str(obj)], check=True)
            objs.append(str(obj))
        cls.host = tmp / "np_host"
        subprocess.run(cc + objs + ["-o", str(cls.host)], check=True)
        cls.app0 = {}
        for scenario, files in {
            "ghost": {"sce_sys/param.sfo": param_sfo({"CATEGORY": "gd", "TITLE_ID": "CUSA11456"})},
            "trp": {
                "sce_sys/param.sfo": param_sfo({"APP_VER": "01.00", "TITLE_ID": "CUSA00001", "VERSION": "01.00"}),
                "sce_sys/trophy/trophy00.trp": trophy_trp(
                    ["TROPCONF.ESFM", "TROP.ESFM", "TROP_00.ESFM", "ICON0.PNG", "TROP.PNG", "GR000.PNG", "GR001.PNG"]
                    + [f"TROP{i:03d}.PNG" for i in range(12)]),
            },
            "bare": {},
            "npid": {},
        }.items():
            root = tmp / scenario / "app0"
            root.mkdir(parents=True)
            for rel, data in files.items():
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_bytes(data)
            cls.app0[scenario] = root

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_np(self, scenario: str) -> str:
        app0 = self.app0.get(scenario, self.app0["ghost"])
        p = subprocess.run([str(self.host), str(EXT), self.crc, str(app0), scenario, *self.offsets],
                           capture_output=True, text=True, timeout=120)
        self.assertNotIn("FAILED", p.stdout, p.stdout[-6000:] + p.stderr)
        self.assertEqual(p.returncode, 0, p.stdout[-6000:] + p.stderr)
        self.assertIn("np_host: all passed", p.stdout)
        return p.stdout

    def test_signed_out_persona(self) -> None:
        out = self.run_np("persona")
        self.assertIn("np_host: ok sceNpGetState: SIGNED_OUT although the console is signed in", out)
        self.assertIn("the console's own NP state for user 10000001 is 2", out)

    def test_request_lifecycles(self) -> None:
        out = self.run_np("requests")
        self.assertIn("np_host: ok the 33rd: REQUEST_MAX", out)
        self.assertIn("np_host: ok PollAsync: the result is SIGNED_OUT", out)

    def test_callbacks_only_inside_the_check_calls(self) -> None:
        out = self.run_np("callbacks")
        self.assertIn("np_host: ok one SIGNED_OUT event per logged-in user", out)
        self.assertIn("np_host: ok callbacks ran only inside the check calls", out)

    def test_trophies_recorded_per_user(self) -> None:
        out = self.run_np("ghost")
        self.assertIn("trophy: title CUSA11456, 77 trophies", out)
        self.assertIn("trophy 5 unlocked locally", out)
        self.assertIn("kept in memory only", out)  # until the savedata family's title store is merged

    def test_trophy_title_from_the_np_title_id(self) -> None:
        out = self.run_np("npid")  # Ghost's dump has no param.sfo
        self.assertIn("trophy: title CUSA11456 from the NP title ID: 77 trophies", out)

    def test_trophy_count_from_the_trp_icons(self) -> None:
        out = self.run_np("trp")
        self.assertIn("12 trophy icons, 2 group icons", out)

    def test_trophy_count_never_zero(self) -> None:
        out = self.run_np("bare")
        self.assertIn("np_host: ok count 1, never 0", out)


if __name__ == "__main__":
    unittest.main()
