"""The system families of the runtime extension, run from the built ext.bin
(ext_host/fam_system.c, x86-64: Rosetta on Apple silicon): app params from
param.sfo or the built-in values, the title's /temp0 and /download0, system
and user events, PlayGo and DiscMap, and the calls that must not reach SWRR.
Needs `make -C ghostldr/native` first.

    python3 -m unittest ghostldr.tests.test_hle_system   (or discover -s ghostldr/tests)
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
sys.path.insert(0, str(HERE))
from test_ext import x86_64_cc  # noqa: E402


def param_sfo(entries: dict[str, str | int]) -> bytes:
    """A PS4 param.sfo with these keys (strings utf8, ints int32)."""
    keys = sorted(entries)
    key_table, data_table, index = b"", b"", b""
    for key in keys:
        value = entries[key]
        if isinstance(value, int):
            data, fmt, max_len = struct.pack("<i", value), 0x0404, 4
        else:
            data, fmt = value.encode() + b"\0", 0x0204
            max_len = (len(data) + 3) & ~3
        index += struct.pack("<HHIII", len(key_table), fmt, len(data), max_len, len(data_table))
        key_table += key.encode() + b"\0"
        data_table += data.ljust(max_len, b"\0")
    key_table = key_table.ljust((len(key_table) + 3) & ~3, b"\0")
    key_off = 0x14 + len(index)
    header = struct.pack("<4sIIII", b"\0PSF", 0x101, key_off, key_off + len(key_table), len(keys))
    return header + index + key_table + data_table


def playgo_chunk_dat(chunks: list[list[int]]) -> bytes:
    """playgo-chunk.dat: one list of mchunk sizes per chunk."""
    ids, attrs, lists, sizes = [], b"", b"", []
    for chunk in chunks:
        attrs += struct.pack("<BBB11sHQII", 0, 0, 3, b"", len(chunk), 0, len(lists), 0)
        for size in chunk:
            lists += struct.pack("<H", len(sizes))
            sizes.append(size)
    mattrs = b"".join(struct.pack("<QQ", 0, s) for s in sizes)
    attrs_off = 0x100
    lists_off = attrs_off + len(attrs)
    mattrs_off = lists_off + len(lists)
    total = mattrs_off + len(mattrs)
    header = bytearray(0x100)
    struct.pack_into("<IHHHHHH", header, 0, 0x6F676C70, 1, 0, 1, len(chunks), len(sizes), 0)
    struct.pack_into("<I", header, 0x10, total)
    struct.pack_into("<IIII", header, 0xC0, attrs_off, len(attrs), lists_off, len(lists))
    struct.pack_into("<II", header, 0xD8, mattrs_off, len(mattrs))
    return bytes(header) + attrs + lists + mattrs


SFO = {"TITLE_ID": "CUSA99999", "CONTENT_ID": "EP0001-CUSA99999_00-TESTTITLE0000000", "TITLE": "Test Title",
       "TITLE_01": "Test Title (US)", "USER_DEFINED_PARAM_1": 7, "USER_DEFINED_PARAM_2": 42,
       "DOWNLOAD_DATA_SIZE": 2}


@unittest.skipUnless(EXT.exists(), "build/ghostldr/ext.bin missing: run make -C ghostldr/native")
class SystemFamiliesOnHost(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = x86_64_cc()
        if not cc:
            raise unittest.SkipTest("no way to build and run x86-64 code here")
        blob = EXT.read_bytes()
        cls.crc = hex(zlib.crc32(blob))
        abi, = struct.unpack_from("<I", blob, 20)
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = Path(cls.tmp.name)
        flags = ["-O1", "-std=gnu11", "-fno-builtin", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror",
                 f"-I{ROOT / 'ghostldr/native/include'}", f"-DGHOST_ABI={abi}u"]
        core = ROOT / "ghostldr/native/src"
        objs = []
        for src, extra in [(core / "ext.c", flags), (core / "sha256.c", flags), (core / "nid.c", flags), (HERE / "ext_host/fam_system.c", flags),
                           (HERE / "ext_host/fam_system_sys.c", ["-O1"])]:
            obj = tmp / (src.stem + ".o")
            subprocess.run(cc + extra + ["-c", str(src), "-o", str(obj)], check=True)
            objs.append(str(obj))
        cls.host = tmp / "fam_system"
        subprocess.run(cc + objs + ["-o", str(cls.host)], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_mode(self, mode: str, title: str = "CUSA11456", files: dict[str, bytes] | None = None) -> str:
        """Run one mode over a fresh fake file system (ROOT/app0, ROOT/temp0)."""
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "app0/sce_sys").mkdir(parents=True)
            (root / "temp0").mkdir()
            for rel, data in (files or {}).items():
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_bytes(data)
            p = subprocess.run([str(self.host), mode, str(EXT), self.crc, str(root), title or "-"],
                               capture_output=True, text=True)
        self.assertNotIn("FAILED", p.stdout, p.stdout + p.stderr)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return p.stdout

    def test_ghost_without_param_sfo_gets_its_built_in_values(self) -> None:
        out = self.run_mode("params-ghost")
        self.assertIn("ok Ghost without param.sfo: USER_DEFINED_PARAM_1 = 11456", out)
        self.assertIn("ext: title CUSA11456: param.sfo missing, built-in values used", out)

    def test_param_sfo_answers_and_wins_over_the_index_title(self) -> None:
        out = self.run_mode("params-sfo", files={"app0/sce_sys/param.sfo": param_sfo(SFO)})
        self.assertIn("ext: title CUSA99999: param.sfo read, built-in values none", out)

    def test_an_unknown_title_reads_zero(self) -> None:
        self.run_mode("params-unknown", title="")

    def test_temp0_is_the_titles_own_directory_and_format_stays_in_it(self) -> None:
        files = {"temp0/swrr.dat": b"SWRR", "temp0/ghostldr/CUSA00001/keep.bin": b"other title",
                 "temp0/ghostldr/CUSA11456/a.bin": b"a", "temp0/ghostldr/CUSA11456/d1/b.bin": b"b",
                 "temp0/ghostldr/CUSA11456/d1/d2/c.bin": b"c"}
        out = self.run_mode("temp", files=files)
        self.assertIn("ext: formatting /temp0/ghostldr/CUSA11456", out)

    def test_download0_has_the_param_sfo_size(self) -> None:
        files = {"app0/sce_sys/param.sfo": param_sfo(SFO),
                 "temp0/ghostldr/CUSA99999/download0/sub/f.bin": b"x" * 1536,
                 "temp0/ghostldr/CUSA99999/t.bin": b"t"}
        self.run_mode("download", files=files)

    def test_system_events_addressed_to_the_app_are_dropped(self) -> None:
        out = self.run_mode("events")
        self.assertIn("sceSystemServiceReceiveEvent: dropped 10000017 (addressed to SWRR)", out)
        self.assertIn("sceSystemServiceReceiveEvent: 10000000", out)   # the rest-mode resume probe looks for this

    def test_login_events_for_every_logged_in_user(self) -> None:
        out = self.run_mode("users")
        self.assertIn("dropped a second LOGIN for user 1002", out)

    def test_playgo_without_chunk_file(self) -> None:
        self.run_mode("playgo")

    def test_playgo_with_chunk_file(self) -> None:
        dat = playgo_chunk_dat([[1000, 2000], [30000], [400000]])
        self.run_mode("playgo-dat", files={"app0/sce_sys/playgo-chunk.dat": dat})

    def test_discmap_says_nothing_is_on_a_disc(self) -> None:
        self.run_mode("discmap")

    def test_calls_that_would_end_or_misreport_swrr(self) -> None:
        self.run_mode("misc")


if __name__ == "__main__":
    unittest.main()
