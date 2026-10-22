#!/usr/bin/env python3
"""run.py's preflight picks the local dump of the title being booted."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
import profiles  # noqa: E402
import run  # noqa: E402


class TitleDumpTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        t = Path(self.tmp.name)
        self.packs, self.default, self.other = t / "ghostfs", t / "ghost/app0", t / "other/app0"
        for d in (self.default, self.other):
            d.mkdir(parents=True)
            (d / "eboot.bin").write_bytes(b"")
        self.patches = [mock.patch.object(profiles, "TITLES_OUT", self.packs),
                        mock.patch.object(profiles, "DEFAULT_SRC", self.default),
                        mock.patch.object(profiles, "detect_title",
                                          lambda src: "CUSA11456" if src == self.default else None)]
        for p in self.patches:
            p.start()

    def tearDown(self) -> None:
        for p in self.patches:
            p.stop()
        self.tmp.cleanup()

    def pack(self, title: str, src: Path) -> None:
        (self.packs / title).mkdir(parents=True)
        (self.packs / title / "layout.json").write_text(json.dumps({"title": title, "src": str(src)}))

    def test_title_from_its_pack(self) -> None:
        self.pack("CUSA00900", self.other)
        self.assertEqual(run.title_dump("CUSA00900", False), (self.other, "CUSA00900"))

    def test_title_in_the_default_dump(self) -> None:
        self.assertEqual(run.title_dump("cusa11456", False), (self.default, "CUSA11456"))

    def test_title_without_a_dump(self) -> None:
        dump, why = run.title_dump("CUSA00001", False)
        self.assertIsNone(dump)
        self.assertIn("no local dump of CUSA00001", why)

    def test_no_title_one_candidate(self) -> None:
        self.pack("CUSA11456", self.default)   # the default dump, packed: still one title
        self.assertEqual(run.title_dump(None, False), (self.default, "CUSA11456"))

    def test_no_title_several_candidates(self) -> None:
        self.pack("CUSA00900", self.other)
        dump, why = run.title_dump(None, False)
        self.assertIsNone(dump)
        self.assertIn("CUSA00900, CUSA11456", why)

    def test_selector(self) -> None:
        self.assertIsNone(run.title_dump("CUSA11456", True)[0])


if __name__ == "__main__":
    unittest.main()
