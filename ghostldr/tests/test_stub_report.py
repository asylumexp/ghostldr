"""stub_report.py against synthetic log lines: every stub line shape
native/src/resolve.c emits, with and without run.py's HH:MM:SS stamp.

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import stub_report as sr  # noqa: E402

LINES = [
    "12:00:01 UNRESOLVED call: libSceFoo AbCdEfGh (sceFooBar)\n",
    "UNRESOLVED call: libSceFoo AbCdEfGh (sceFooBar)\n",
    "12:00:02 UNRESOLVED call: libSceFoo XyZ12345 (sceFooBaz) -> 0\n",
    "DENIED call: libSceBar NidNidNi (sceBarDenied) -> 80020001\n",
    "UNRESOLVED call #7\n",
    "12:00:03 UNRESOLVED call #7\n",
    "UNRESOLVED call #7\n",
    "12:00:04 boot: unrelated noise\n",
    "ext: received 2266178 bytes (CRC deadbeef)\n",
]


class StubReport(unittest.TestCase):
    def setUp(self) -> None:
        self.st = sr.parse_lines(LINES)

    def test_counts(self) -> None:
        self.assertEqual(self.st.total_lines, len(LINES))
        self.assertEqual(self.st.stub_lines, 7)
        self.assertEqual(self.st.bare, 3)
        d = self.st.as_dict(30)
        self.assertEqual(d["distinct_stubs"], 3)
        self.assertEqual(d["denied_hits"], 1)
        self.assertEqual(d["unnamed_hits"], 3)

    def test_ranking(self) -> None:
        top = self.st.ranked()
        self.assertEqual(top[0]["name"], "sceFooBar")
        self.assertEqual(top[0]["count"], 2)
        self.assertEqual(top[0]["kind"], "UNRESOLVED")
        denied = [r for r in top if r["kind"] == "DENIED"]
        self.assertEqual(len(denied), 1)
        self.assertEqual(denied[0]["rets"], ["80020001"])

    def test_render_mentions_totals(self) -> None:
        text = sr.render(self.st, 30)
        self.assertIn("stub hits: 7", text)
        self.assertIn("sceFooBar", text)


if __name__ == "__main__":
    unittest.main()
