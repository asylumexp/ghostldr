"""The minimal SQLite writer (native/ext/src/sqlite_min.c), built for this
machine (ext_native/sqlite_min_host.c): its files must read back with
Python's sqlite3 exactly, with one leaf page and with an interior root over
many leaves, and pass PRAGMA integrity_check.

    python3 -m unittest test_sqlite_min        (from ghostldr/tests)
"""

from __future__ import annotations

import sqlite3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
NATIVE = HERE.parent / "native"
sys.path.insert(0, str(HERE))
from test_profiles import host_cc  # noqa: E402


class SqliteMin(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = host_cc()
        if not cc:
            raise unittest.SkipTest("no C compiler whose programs run on this machine")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name) / "sqlite_min_host"
        sources = [HERE / "ext_native/sqlite_min_host.c", NATIVE / "ext/src/sqlite_min.c"]
        subprocess.run(cc + ["-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                             "-Wno-gnu-include-next", "-ffreestanding", "-fno-builtin",
                             f"-I{HERE / 'ext_native'}", f"-I{NATIVE / 'ext/include'}", f"-I{NATIVE / 'include'}",
                             "-DGHOST_ABI=0u", *map(str, sources), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def check(self, n: int) -> None:
        db = Path(self.tmp.name) / f"t{n}.db"
        subprocess.run([str(self.exe), str(db), str(n)], check=True)
        con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
        self.assertEqual(con.execute("PRAGMA integrity_check").fetchone()[0], "ok")
        rows = con.execute("SELECT rowid, title_id, dir_name, main_title, sub_title, blocks, account_id "
                           "FROM savedata ORDER BY rowid").fetchall()
        self.assertEqual(len(rows), n)
        for i, r in enumerate(rows):
            self.assertEqual(r[0], i + 1)
            self.assertEqual(r[1], f"CUSA{i:05d}")
            self.assertEqual(r[2], f"SAVE{i:04d}")
            self.assertEqual(r[4], "sub" if i % 3 else "")
            self.assertEqual(r[5], 96 + i)
            self.assertEqual(r[6], 0x1122334455667788 if i % 2 else -5)
        con.close()

    def test_empty(self) -> None:
        self.check(0)

    def test_one_leaf(self) -> None:
        self.check(5)

    def test_interior_root_over_many_leaves(self) -> None:
        self.check(1500)


if __name__ == "__main__":
    unittest.main()
