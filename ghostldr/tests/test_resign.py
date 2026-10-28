"""ghostldr/host/resign.py: reading a decrypted save (Apollo's folder and zip,
a plain folder), the stream the console payload (ghostldr/payloads/resign.c)
takes, and a whole run against a stand-in for the ELF loader and the payload.

    python3 -m unittest test_resign        (from ghostldr/tests)
"""

from __future__ import annotations

import io
import socket
import struct
import sys
import tempfile
import threading
import unittest
import zipfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "ghostldr/host"))
import resign as rs  # noqa: E402

KEYSTONE = b"keystone" + bytes(range(1, 89))


def sfo(values: dict[str, str | bytes]) -> bytes:
    keys, data, entries = b"", b"", []
    for k, v in values.items():
        raw, fmt = (v, 0x0004) if isinstance(v, bytes) else (v.encode() + b"\0", 0x0204)
        mx = (len(raw) + 3) & ~3
        entries.append(struct.pack("<HHIII", len(keys), fmt, len(raw), mx, len(data)))
        keys += k.encode() + b"\0"
        data += raw.ljust(mx, b"\0")
    keys = keys.ljust((len(keys) + 3) & ~3, b"\0")
    kt = 20 + 16 * len(entries)
    return struct.pack("<4sIIII", b"\0PSF", 0x101, kt, kt + len(keys), len(entries)) + b"".join(entries) + keys + data


def apollo_save(folder: Path, account: int = 0x1122334455667788) -> Path:
    root = folder / "PS4/APOLLO/10000001_CUSA07992_SCUS-97429"
    (root / "sce_sys").mkdir(parents=True)
    (root / "sce_sys/param.sfo").write_bytes(sfo({"ACCOUNT_ID": account.to_bytes(8, "little"),
                                                  "SAVEDATA_DIRECTORY": "SCUS-97429", "TITLE_ID": "CUSA07992"}))
    (root / "sce_sys/keystone").write_bytes(KEYSTONE)
    (root / "VMC0.card").write_bytes(b"\x0A" * 5000)
    (root / "slots").mkdir()
    (root / "slots/a.sav").write_bytes(b"A")
    (root / "._VMC0.card").write_bytes(b"mac")
    return root


def parse_stream(data: bytes) -> tuple[dict, list]:
    magic, ver, mode, user, title, dirname, fp, n = struct.unpack_from("<8sIIi16s32s80sI", data)
    off = struct.calcsize("<8sIIi16s32s80sI")
    head = {"magic": magic, "version": ver, "mode": mode, "user": user, "title": title.rstrip(b"\0").decode(),
            "dir": dirname.rstrip(b"\0").decode(), "fp": fp.rstrip(b"\0").decode()}
    entries = []
    for _ in range(n):
        kind, ln = struct.unpack_from("<BH", data, off)
        off += 3
        rel = data[off:off + ln].decode()
        off += ln
        if kind == 1:
            size, = struct.unpack_from("<Q", data, off)
            off += 8
            entries.append((rel, data[off:off + size]))
            off += size
        else:
            entries.append((rel, None))
    assert off == len(data)
    return head, entries


class SourceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_apollo_folder(self) -> None:
        src = rs.Source(apollo_save(self.dir))
        self.assertEqual((src.title, src.dir, src.account, src.fp), ("CUSA07992", "SCUS-97429", "1122334455667788",
                                                                     KEYSTONE[0x20:0x40].hex()))
        self.assertEqual([r for r, _ in src.entries], ["VMC0.card", "slots", "slots/a.sav"])   # no sce_sys, no ._
        head, entries = parse_stream(src.stream(rs.WRITE))
        self.assertEqual((head["magic"], head["version"], head["mode"], head["user"], head["title"], head["dir"]),
                         (b"GLDRRSGN", 1, 1, -1, "CUSA07992", "SCUS-97429"))
        self.assertEqual(entries, [("VMC0.card", b"\x0A" * 5000), ("slots", None), ("slots/a.sav", b"A")])

    def test_apollo_zip(self) -> None:
        root = apollo_save(self.dir / "a")
        zpath = self.dir / "CUSA07992_SCUS-97429.zip"
        with zipfile.ZipFile(zpath, "w") as z:
            for p in sorted(root.rglob("*")):
                z.write(p, "SCUS-97429/" + p.relative_to(root).as_posix())
        src = rs.Source(zpath)
        self.assertEqual((src.title, src.dir), ("CUSA07992", "SCUS-97429"))
        self.assertIn("slots/a.sav", [r for r, _ in src.entries])

    def test_plain_folder_needs_the_names(self) -> None:
        (self.dir / "save").mkdir()
        (self.dir / "save/data.bin").write_bytes(b"d")
        with self.assertRaisesRegex(rs.ResignError, "--game"):
            rs.Source(self.dir / "save")
        with self.assertRaisesRegex(rs.ResignError, "--dir"):
            rs.Source(self.dir / "save", title="CUSA00001")
        src = rs.Source(self.dir / "save", title="CUSA00001", dirname="SAVEDATA00")
        self.assertEqual((src.title, src.dir, src.fp), ("CUSA00001", "SAVEDATA00", ""))
        (self.dir / "ks").write_bytes(KEYSTONE)
        self.assertEqual(rs.Source(self.dir / "save", "CUSA00001", "SAVEDATA00", self.dir / "ks").fp,
                         KEYSTONE[0x20:0x40].hex())

    def test_refusals(self) -> None:
        with self.assertRaisesRegex(rs.ResignError, "SWRR"):
            rs.Source(apollo_save(self.dir), title="CUSA03474")
        evil = self.dir / "evil.zip"
        with zipfile.ZipFile(evil, "w") as z:
            z.writestr("../x", b"x")
        with self.assertRaisesRegex(rs.ResignError, "unsafe"):
            rs.Source(evil)
        empty = self.dir / "empty"
        (empty / "sce_sys").mkdir(parents=True)
        with self.assertRaisesRegex(rs.ResignError, "no save files"):
            rs.Source(empty, title="CUSA00001", dirname="X")


class FakeConsole:
    """An ELF loader on one port that 'runs' resign.elf: it opens a data port, reads the stream, sends a backup
    and a RESULT line, as ghostldr/payloads/resign.c does."""

    def __init__(self, result: str, backup: dict[str, bytes]) -> None:
        self.result, self.backup, self.got = result, backup, None
        self.loader = socket.socket()
        self.loader.bind(("127.0.0.1", 0))
        self.loader.listen(1)
        self.data = socket.socket()
        self.data.bind(("127.0.0.1", 0))
        self.data.listen(1)
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self) -> None:
        c, _ = self.loader.accept()
        elf = c.recv(4)
        assert elf == b"\x7fELF", elf
        c.sendall(f"[elfldr] payload 1: ELF\nresign: listening on TCP {self.data.getsockname()[1]}\n".encode())
        d, _ = self.data.accept()
        buf = b""
        d.settimeout(2)
        try:
            while chunk := d.recv(65536):
                buf += chunk
                try:
                    parse_stream(buf)
                    break
                except (AssertionError, struct.error):
                    continue
        except socket.timeout:
            pass
        self.got = parse_stream(buf)
        d.sendall(b"resign: receiving\n")
        if self.got[0]["mode"] == 1:
            out = [f"BACKUP {len(self.backup)}\n".encode()]
            for rel, data in self.backup.items():
                out.append(struct.pack("<BH", 1, len(rel)) + rel.encode() + struct.pack("<Q", len(data)) + data)
            d.sendall(b"".join(out))
        d.sendall(self.result.encode() + b"\n")
        d.close()
        c.sendall(b"[elfldr] payload 1 exited with code 0\n")
        c.close()


class RunTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.src = rs.Source(apollo_save(self.dir))
        self.payload = self.dir / "resign.elf"
        self.payload.write_bytes(b"\x7fELF" + bytes(100))

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def go(self, result: str, mode: int = rs.WRITE):
        fake = FakeConsole(result, {"VMC0.card": b"old", "lua/init.lua": b"-- old"})
        port = fake.loader.getsockname()[1]
        lines = []
        with mock.patch.object(rs, "PAYLOAD", self.payload), \
                mock.patch.object(rs, "start_elfldr", return_value=port):
            r = rs.run(self.src, mode, "127.0.0.1", None, self.dir / "backup", lines.append)
        return r, lines, fake

    def test_write(self) -> None:
        r, lines, fake = self.go("RESULT rc=00000000 mount=809f000f slot=1 written=1 needs_exit=1")
        self.assertEqual((r["rc"], r["needs_exit"]), ("00000000", "1"))
        self.assertEqual(fake.got[0]["fp"], KEYSTONE[0x20:0x40].hex())
        self.assertEqual((self.dir / "backup/lua/init.lua").read_bytes(), b"-- old")
        self.assertTrue(any("the undo" in ln for ln in lines))

    def test_check(self) -> None:
        r, _, fake = self.go("RESULT rc=00000000 bad=0 extra=0", rs.CHECK)
        self.assertEqual(fake.got[0]["mode"], 2)
        self.assertEqual(r["bad"], "0")

    def test_main_reports_failure(self) -> None:
        err = io.StringIO()
        with mock.patch.object(rs, "run", return_value={"rc": "80020016", "why": "boom", "needs_exit": "0"}), \
                mock.patch("console.console_ip", return_value="127.0.0.1"), redirect_stderr(err), \
                redirect_stdout(io.StringIO()):
            rc = rs.main([str(self.dir / "PS4/APOLLO/10000001_CUSA07992_SCUS-97429"), "--host", "127.0.0.1"])
        self.assertEqual(rc, 1)
        self.assertIn("boom", err.getvalue())
        out = io.StringIO()
        with mock.patch.object(rs, "run", return_value={"rc": "00000000", "needs_exit": "1"}), redirect_stdout(out):
            rc = rs.main([str(self.dir / "PS4/APOLLO/10000001_CUSA07992_SCUS-97429"), "--host", "127.0.0.1"])
        self.assertEqual(rc, 0)
        self.assertIn("NOW CLOSE SWRR", out.getvalue())


class CommandTests(unittest.TestCase):
    def test_ghostldr_py_resign(self) -> None:
        import subprocess
        out = subprocess.run([sys.executable, str(ROOT / "ghostldr.py"), "--help"], capture_output=True, text=True)
        self.assertRegex(out.stdout, r"\n    resign ")
        sys.path.insert(0, str(ROOT))
        import ghostldr as gl
        seen = {}
        with mock.patch.object(rs, "main", side_effect=lambda argv: seen.setdefault("argv", argv) and 0):
            gl.main(["--console", "10.0.0.5", "resign", "save.zip", "--check"])
        self.assertEqual(seen["argv"], ["save.zip", "--check", "--host", "10.0.0.5"])


if __name__ == "__main__":
    unittest.main()
