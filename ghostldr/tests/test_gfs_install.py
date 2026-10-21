"""Switching the installed ghostfs title: gfs_pack.py + gfs_install.py against a
fake console that follows gfs_receiver.lua's rules (containers as dicts;
PUT/DELETE/WRITE refuse *SAVE and SWRR's own save).

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
import gfs_install as gi  # noqa: E402
import gfs_pack as gp  # noqa: E402

NOT_FOUND = gi.NOT_FOUND


class FakeConsole:
    """Save containers of one user: {name: {file: bytes}}."""

    def __init__(self) -> None:
        self.saves: dict[str, dict[str, bytes]] = {}
        self.fail_put: str | None = None

    @staticmethod
    def protected(name: str) -> bool:
        return name in ("SLUS-20268", "SLES-50366") or name.endswith("SAVE")

    def read(self, d: str, f: str, size: int, offset: int = 0) -> bytes | None:
        data = self.saves.get(d, {}).get(f)
        return None if data is None else data[offset:offset + size]

    def write(self, d: str, f: str, data: bytes) -> int:
        if self.protected(d):
            return 0xFFFFFFFD
        if d not in self.saves:
            return NOT_FOUND
        self.saves[d][f] = data
        return 0

    def delete(self, d: str) -> int:
        if self.protected(d):
            return 0xFFFFFFFD
        return 0 if self.saves.pop(d, None) is not None else NOT_FOUND

    def put(self, d: str, f: str, blocks: int, size: int, chunks) -> tuple[int, int, int]:
        assert not self.protected(d)
        if d == self.fail_put:
            raise SystemExit(f"{d}: connection lost")
        data = b"".join(chunks)
        assert len(data) == size
        self.saves[d] = {f: data}
        return 0, size, 0


def make_title(root: Path, title: str, files: dict[str, bytes]) -> tuple[dict, bytes, Path]:
    src = root / title / "app0"
    for rel, data in files.items():
        (src / rel).parent.mkdir(parents=True, exist_ok=True)
        (src / rel).write_bytes(data)
    info = gp.pack(src, title, gp.PROFILES.get(title, {}).get("prefix") or "G" + title[4:], root / title / "out")
    return info, (root / title / "out/index.bin").read_bytes(), src


def install(console: FakeConsole, info: dict, index: bytes, src: Path, force: bool = False) -> list[tuple]:
    new = gi.parse_header(gi.INDEX, index)
    installed, tagged = gi.survey(console, new, info["containers"])
    steps = gi.plan(installed, new, tagged, force)
    gi.execute(console, steps, new, info, index, src)
    return steps


def payload(console: FakeConsole, info: dict) -> bytes:
    return b"".join(console.saves[c["dir"]]["data.bin"] for c in info["containers"])


@mock.patch.object(gp, "CAP", 64)            # tiny containers: a few bytes of files span several
@mock.patch.object(gi, "log", lambda msg: None)
class SwitchTitleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.console = FakeConsole()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def title(self, title: str, files: dict[str, bytes]):
        return make_title(self.root, title, files)

    def test_fresh_install_writes_tagged_containers_and_index_last(self) -> None:
        info, index, src = self.title("CUSA11111", {"eboot.bin": b"E" * 100, "data/a.bin": b"A" * 90})
        install(self.console, info, index, src)
        self.assertEqual(self.console.saves["GFSINDEX"]["index.bin"], index)
        self.assertEqual(payload(self.console, info), b"E" * 100 + b"A" * 90)
        for k, c in enumerate(info["containers"]):
            tag = self.console.saves[c["dir"]]["gfs.tag"]
            self.assertEqual(tag, gi.tag_bytes(int(info["layout_id"], 16), k, c["size"]))

    def test_another_title_is_removed_first_and_its_progress_kept(self) -> None:
        a, ia, sa = self.title("CUSA11111", {"eboot.bin": b"a" * 300})
        install(self.console, a, ia, sa)
        self.console.saves["G11111SAVE"] = {"sdmemory_0.bin": b"progress"}
        b, ib, sb = self.title("CUSA22222", {"eboot.bin": b"b" * 70})
        steps = install(self.console, b, ib, sb)
        self.assertEqual(steps[0][0], "remove")
        self.assertFalse([d for d in self.console.saves if d.startswith("G11111") and d[6:].isdigit()])
        self.assertEqual(self.console.saves["G11111SAVE"], {"sdmemory_0.bin": b"progress"})
        self.assertEqual(self.console.saves["GFSINDEX"]["index.bin"], ib)
        self.assertEqual(payload(self.console, b), b"b" * 70)

    def test_same_title_complete_sends_nothing(self) -> None:
        info, index, src = self.title("CUSA11111", {"eboot.bin": b"x" * 150})
        install(self.console, info, index, src)
        self.console.saves["G11111000"]["data.bin"] = b"untouched marker"
        steps = install(self.console, info, index, src)
        self.assertEqual(steps, [])
        self.assertEqual(self.console.saves["G11111000"]["data.bin"], b"untouched marker")

    def test_interrupted_install_resumes_and_is_never_bootable_meanwhile(self) -> None:
        info, index, src = self.title("CUSA11111", {"eboot.bin": b"y" * 200})
        self.console.fail_put = "G11111002"
        with self.assertRaises(SystemExit):
            install(self.console, info, index, src)
        hdr = gi.parse_header("GFSINDEX", self.console.saves["GFSINDEX"]["index.bin"])
        self.assertTrue(hdr.incomplete)           # ghostfs.c refuses to boot this
        self.console.fail_put = None
        steps = install(self.console, info, index, src)
        self.assertEqual([s[1] for s in steps if s[0] == "put"], [2, 3])
        self.assertEqual(payload(self.console, info), b"y" * 200)
        self.assertFalse(gi.parse_header("GFSINDEX", self.console.saves["GFSINDEX"]["index.bin"]).incomplete)

    def test_interrupted_removal_of_the_old_title_is_finished_on_rerun(self) -> None:
        a, ia, sa = self.title("CUSA11111", {"eboot.bin": b"a" * 300})
        install(self.console, a, ia, sa)
        b, ib, sb = self.title("CUSA22222", {"eboot.bin": b"b" * 70})
        with mock.patch.object(self.console, "put", side_effect=SystemExit("stub put lost")):
            with self.assertRaises(SystemExit):
                install(self.console, b, ib, sb)
        install(self.console, b, ib, sb)
        self.assertEqual(sorted(self.console.saves), ["G22222000", "G22222001", "GFSINDEX"])

    def test_legacy_ghost_index_with_the_same_data_is_only_upgraded(self) -> None:
        info, index, src = self.title("CUSA11456", {"eboot.bin": b"g" * 130})
        install(self.console, info, index, src)
        # rewrite it as a version 1 install: GHOSTIDX, 64-byte header, no title
        hdr = struct.unpack_from("<4sIIIQQII16s", index)
        v1 = struct.pack("<4sIIIQQII16s", b"GFS1", 1, *hdr[2:6], hdr[6] - 64, hdr[7], hdr[8]).ljust(64, b"\0")
        self.console.saves["GHOSTIDX"] = {"index.bin": v1 + index[gp.HEADER_SIZE:]}
        del self.console.saves["GFSINDEX"]
        for c in info["containers"]:
            del self.console.saves[c["dir"]]["gfs.tag"]
        self.console.saves["GHOSTSAVE"] = {"sdmemory_0.bin": b"jin"}
        steps = install(self.console, info, index, src)
        self.assertEqual(steps, [("index",), ("delete", "GHOSTIDX")])
        self.assertNotIn("GHOSTIDX", self.console.saves)
        self.assertEqual(self.console.saves["GFSINDEX"]["index.bin"], index)
        self.assertEqual(self.console.saves["GHOSTSAVE"], {"sdmemory_0.bin": b"jin"})

    def test_legacy_ghost_is_removed_for_another_title(self) -> None:
        g, ig, sg = self.title("CUSA11456", {"eboot.bin": b"g" * 130})
        install(self.console, g, ig, sg)
        self.console.saves["GHOSTIDX"] = self.console.saves.pop("GFSINDEX")
        b, ib, sb = self.title("CUSA22222", {"eboot.bin": b"b" * 10})
        install(self.console, b, ib, sb)
        self.assertEqual(sorted(self.console.saves), ["G22222000", "GFSINDEX"])

    def test_receiver_refuses_to_touch_progress(self) -> None:
        self.console.saves["GHOSTSAVE"] = {"x": b"1"}
        self.assertNotEqual(self.console.delete("GHOSTSAVE"), 0)
        self.assertIn("GHOSTSAVE", self.console.saves)


class PackTests(unittest.TestCase):
    def test_ghost_layout_is_the_installed_one(self) -> None:
        ref = Path(__file__).resolve().parents[2] / "build/ghostfs/layout.reference-v1.json"
        src = gp.PROFILES["CUSA11456"]["src"]
        if not ref.exists() or not src.exists():
            self.skipTest("needs the CUSA11456 dump and the reference layout")
        import json
        with tempfile.TemporaryDirectory() as tmp:
            info = gp.pack(src, "CUSA11456", "GHOST", Path(tmp))
        old = json.loads(ref.read_text())["containers"]
        self.assertEqual([c["pieces"] for c in info["containers"]], [c["pieces"] for c in old])


if __name__ == "__main__":
    unittest.main()
