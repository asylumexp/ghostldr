"""The console's FTP server (native/ext/src/ftp.c, docs/ftp.md), offline.

ftp.c is built for this machine against ext_native/ftp_host.c, whose
environment is POSIX over a temporary directory standing in for the
console's "/", and driven with Python's ftplib and raw sockets:

  - logging in with any user, PWD, CWD, CDUP, MKD, RMD, DELE, RNFR/RNTO;
  - uploads and downloads (empty, smaller and larger than a transfer
    buffer, 48 MiB) compared byte for byte, APPE, REST on both sides;
  - SIZE, MDTM (UTC), MLST, MLSD facts, LIST lines, NLST names, a folder
    whose listing is larger than a transfer buffer, names with spaces and
    Unicode;
  - passive (PASV, EPSV) and active (PORT) data connections, a PORT to
    another address refused;
  - paths: "..", "//", "." and absolute ones, never above the root;
  - errors with their codes, Telnet bytes before ABOR, an unknown command,
    a line longer than any command;
  - ten clients at once, an eleventh turned away until one leaves, a client
    that drops its connection in the middle of an upload;
  - ghostldr's own drive tools (usb_drive.py, as gfs_pack.py --usb uses
    them) putting a title on, checking and removing it, asking for more
    connections than the server takes.

    python3 -m unittest discover -s ghostldr/tests -p test_ftp.py
"""

from __future__ import annotations

import ftplib
import io
import os
import re
import shutil
import socket
import subprocess
import tempfile
import time
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
NATIVE = HERE.parent / "native"
sys.path.insert(0, str(HERE.parent / "host"))
sys.path.insert(0, str(HERE))
FTP_BUF = 128 << 10
SESSIONS = 10


def build(out: Path) -> None:
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc:
        raise unittest.SkipTest("no C compiler")
    obj = out.with_suffix(".ftp.o")
    # ftp.c as the extension has it: freestanding, beside ghost.h
    subprocess.run([cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-ffreestanding", "-fno-builtin", f"-I{NATIVE / 'include'}", f"-I{NATIVE / 'ext/include'}",
                    "-c", str(NATIVE / "ext/src/ftp.c"), "-o", str(obj)], check=True)
    subprocess.run([cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    f"-I{NATIVE / 'ext/include'}", str(HERE / "ext_native/ftp_host.c"), str(obj), "-o", str(out)],
                   check=True)


class Server:
    def __init__(self, exe: Path, root: Path, home: str = "/") -> None:
        self.log = tempfile.TemporaryFile()
        self.proc = subprocess.Popen([str(exe), str(root), "0", home], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.log, text=True)
        line = self.proc.stdout.readline().split()
        if len(line) != 2 or line[0] != "listening":
            self.stop()
            raise RuntimeError(f"ftp_host: {line} {self.text()}")
        self.port = int(line[1])

    def text(self) -> str:
        self.log.seek(0)
        return self.log.read().decode(errors="replace")

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.stdin.close()
            self.proc.wait(10)
        self.proc.stdout.close()
        self.log.close()


class FtpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name) / "ftp_host"
        build(cls.exe)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name) / "root"
        self.root.mkdir()
        (self.root / "usb").mkdir()
        self.server = Server(self.exe, self.root)
        self.clients: list[ftplib.FTP] = []

    def tearDown(self) -> None:
        for c in self.clients:
            try:
                c.close()
            except OSError:
                pass
        self.server.stop()
        self.dir.cleanup()

    def client(self, user: str = "anyone", pasv: bool = True) -> ftplib.FTP:
        c = ftplib.FTP()
        c.connect("127.0.0.1", self.server.port, timeout=20)
        c.login(user, "whatever")
        c.set_pasv(pasv)
        self.clients.append(c)
        return c

    def put(self, c: ftplib.FTP, name: str, data: bytes, rest: int | None = None) -> str:
        return c.storbinary(f"STOR {name}", io.BytesIO(data), rest=rest)

    def get(self, c: ftplib.FTP, name: str, rest: int | None = None) -> bytes:
        out = io.BytesIO()
        c.retrbinary(f"RETR {name}", out.write, rest=rest)
        return out.getvalue()

    def test_listening_again_after_rest_mode(self) -> None:
        """ftp_relisten (after rest mode): new listening sockets on the same port, new passive ports."""
        c = self.client()
        self.put(c, "a.bin", b"A" * 1000)
        c.quit()
        self.clients.remove(c)
        self.server.proc.stdin.write("r")
        self.server.proc.stdin.flush()
        self.assertEqual(self.server.proc.stdout.readline().strip(), "relisten 0")
        c = self.client()                                    # the same port answers
        self.assertEqual(self.get(c, "a.bin"), b"A" * 1000)  # through a passive port made again
        self.put(c, "b.bin", b"B" * 10)
        self.assertEqual((self.root / "b.bin").read_bytes(), b"B" * 10)
        self.assertIn("listening on TCP", self.server.text())

    # ---- sessions and paths ----

    def test_login_and_directories(self) -> None:
        c = self.client()
        self.assertEqual(c.pwd(), "/")
        self.assertIn("UNIX", c.sendcmd("SYST"))
        feat = c.sendcmd("FEAT")
        for f in ("MLST type*;size*;modify*;perm*;", "SIZE", "MDTM", "REST STREAM", "EPSV", "UTF8"):
            self.assertIn(f, feat)
        self.assertEqual(c.mkd("usb/ghostldr"), "/usb/ghostldr")
        self.assertTrue((self.root / "usb/ghostldr").is_dir())
        c.cwd("usb/ghostldr")
        self.assertEqual(c.pwd(), "/usb/ghostldr")
        c.sendcmd("CDUP")
        self.assertEqual(c.pwd(), "/usb")
        c.cwd("//usb/./ghostldr/../ghostldr/")
        self.assertEqual(c.pwd(), "/usb/ghostldr")
        c.cwd("/../../..")
        self.assertEqual(c.pwd(), "/")
        with self.assertRaisesRegex(ftplib.error_perm, "^550"):
            c.cwd("missing")
        (self.root / "file").write_bytes(b"x")
        with self.assertRaisesRegex(ftplib.error_perm, "^550.*Not a directory"):
            c.cwd("file")
        self.assertEqual(c.sendcmd('MKD with"quote'), '257 "/with""quote" created')
        c.rmd('with"quote')
        self.assertEqual(c.pwd(), "/")

    def test_home_directory(self) -> None:
        self.server.stop()
        self.server = Server(self.exe, self.root, "/usb")
        self.assertEqual(self.client().pwd(), "/usb")

    def test_never_above_the_root(self) -> None:
        (Path(self.dir.name) / "outside.txt").write_bytes(b"secret")
        c = self.client()
        for name in ("../outside.txt", "/../outside.txt", "usb/../../outside.txt", "../root/../outside.txt"):
            with self.assertRaisesRegex(ftplib.error_perm, "^550"):
                self.get(c, name)
        self.put(c, "../escape.bin", b"data")
        self.assertTrue((self.root / "escape.bin").exists())
        self.assertFalse((Path(self.dir.name) / "escape.bin").exists())

    # ---- transfers ----

    def test_upload_download(self) -> None:
        c = self.client()
        c.cwd("usb")
        blobs = {"empty.bin": b"", "small.txt": b"hello\r\nworld\n", "edge.bin": os.urandom(FTP_BUF),
                 "big.bin": os.urandom(3 * FTP_BUF + 12345), "name with spaces.bin": os.urandom(1000),
                 "ünïcødé ゲーム.bin": os.urandom(777)}
        c.encoding = "utf-8"
        for name, data in blobs.items():
            self.assertTrue(self.put(c, name, data).startswith("226"))
            self.assertEqual((self.root / "usb" / name).read_bytes(), data, name)
            self.assertEqual(self.get(c, name), data, name)
            self.assertEqual(c.size(name), len(data))
        # a second STOR replaces the file
        self.put(c, "big.bin", b"short")
        self.assertEqual((self.root / "usb/big.bin").read_bytes(), b"short")
        # TYPE A changes nothing
        c.sendcmd("TYPE A")
        self.assertEqual(self.get(c, "small.txt"), blobs["small.txt"])

    def test_large_file(self) -> None:
        data = os.urandom(48 << 20)
        c = self.client()
        t = time.monotonic()
        self.put(c, "usb/large.bin", data)
        self.assertEqual(self.get(c, "usb/large.bin"), data)
        self.assertLess(time.monotonic() - t, 60)
        self.assertRegex(self.server.text(), r"STOR /usb/large\.bin: 50331648 bytes in \d+ ms \(\d+ KiB/s; the file took \d+ ms\)")

    def test_resume_and_append(self) -> None:
        data = os.urandom(FTP_BUF * 2 + 99)
        c = self.client()
        cut = FTP_BUF + 7
        self.put(c, "part.bin", data[:cut])
        self.put(c, "part.bin", data[cut:], rest=cut)   # REST then STOR: continues the file
        self.assertEqual((self.root / "part.bin").read_bytes(), data)
        self.assertEqual(self.get(c, "part.bin", rest=5000), data[5000:])
        c.storbinary("APPE part.bin", io.BytesIO(b"tail"))
        self.assertEqual((self.root / "part.bin").read_bytes(), data + b"tail")
        c.storbinary("APPE new.bin", io.BytesIO(b"first"))
        self.assertEqual((self.root / "new.bin").read_bytes(), b"first")
        with self.assertRaisesRegex(ftplib.error_perm, "^501"):
            c.sendcmd("REST abc")
        self.assertTrue(c.sendcmd("REST 0").startswith("350"))

    def test_file_operations(self) -> None:
        c = self.client()
        self.put(c, "a.bin", b"A")
        c.rename("a.bin", "usb/b.bin")
        self.assertFalse((self.root / "a.bin").exists())
        self.assertEqual((self.root / "usb/b.bin").read_bytes(), b"A")
        with self.assertRaisesRegex(ftplib.error_perm, "^503"):
            c.sendcmd("RNTO c.bin")
        with self.assertRaisesRegex(ftplib.error_perm, "^550"):
            c.rename("missing", "c.bin")
        c.mkd("usb/dir")
        self.put(c, "usb/dir/f", b"f")
        with self.assertRaisesRegex(ftplib.error_perm, "^550.*not empty"):
            c.rmd("usb/dir")
        c.delete("usb/dir/f")
        c.rmd("usb/dir")
        self.assertFalse((self.root / "usb/dir").exists())
        with self.assertRaisesRegex(ftplib.error_perm, "^550.*No such file"):
            c.delete("usb/dir/f")
        with self.assertRaisesRegex(ftplib.error_perm, "^550"):
            c.mkd("usb")
        with self.assertRaisesRegex(ftplib.error_perm, "^550.*Is a directory"):
            self.get(c, "usb")
        with self.assertRaisesRegex(ftplib.error_perm, "^550.*Is a directory"):
            c.size("usb")
        with self.assertRaisesRegex(ftplib.error_perm, "^501"):
            c.sendcmd("DELE")

    # ---- listings ----

    def test_listings(self) -> None:
        c = self.client()
        c.encoding = "utf-8"
        (self.root / "usb/sub").mkdir()
        (self.root / "usb/f 1.bin").write_bytes(b"x" * 1234)
        (self.root / "usb/ゲーム").write_bytes(b"")
        old = time.mktime((2021, 3, 4, 5, 6, 7, 0, 0, -1))
        os.utime(self.root / "usb/f 1.bin", (old, old))
        facts = dict(c.mlsd("usb"))
        self.assertEqual(set(facts), {"sub", "f 1.bin", "ゲーム"})
        self.assertEqual(facts["sub"]["type"], "dir")
        self.assertEqual(facts["f 1.bin"]["type"], "file")
        self.assertEqual(facts["f 1.bin"]["size"], "1234")
        self.assertEqual(facts["f 1.bin"]["modify"], time.strftime("%Y%m%d%H%M%S", time.gmtime(old)))
        self.assertEqual(c.sendcmd("MDTM usb/f 1.bin"), "213 " + time.strftime("%Y%m%d%H%M%S", time.gmtime(old)))
        self.assertEqual(sorted(c.nlst("usb")), sorted(["sub", "f 1.bin", "ゲーム"]))
        lines = []
        c.retrlines("LIST -la usb", lines.append)
        self.assertEqual(len(lines), 3)
        by_name = {ln.split(None, 8)[8]: ln for ln in lines}
        self.assertTrue(by_name["sub"].startswith("d"))
        g = time.gmtime(old)   # listings are in UTC
        month = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split()[g.tm_mon - 1]
        self.assertRegex(by_name["f 1.bin"], rf"^-rw-rw-rw- 1 ps ps +1234 {month} {g.tm_mday:2d}  2021 f 1\.bin$")
        lines = []
        c.retrlines("LIST usb/f 1.bin", lines.append)
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].endswith(" f 1.bin"))
        mlst = c.sendcmd("MLST usb/f 1.bin")
        self.assertRegex(mlst, r"^250-.*\n type=file;size=1234;modify=\d{14};perm=adfrw; /usb/f 1\.bin\n250 End")
        with self.assertRaisesRegex(ftplib.error_perm, "^550.*Not a directory"):
            list(c.mlsd("usb/f 1.bin"))
        with self.assertRaisesRegex(ftplib.error_perm, "^550"):
            c.nlst("missing")

    def test_listing_larger_than_a_buffer(self) -> None:
        big = self.root / "usb/many"
        big.mkdir()
        names = {f"file-{i:05d}-{'x' * 40}.bin" for i in range(4000)}
        for n in names:
            (big / n).write_bytes(b"")
        c = self.client()
        listed = {n for n, _ in c.mlsd("usb/many")}
        self.assertEqual(listed, names)
        lines = []
        c.retrlines("LIST usb/many", lines.append)
        self.assertEqual(len(lines), 4000)

    # ---- data connections ----

    def test_active_and_extended_modes(self) -> None:
        c = self.client(pasv=False)            # PORT 127,0,0,1,...
        self.put(c, "act.bin", b"active")
        self.assertEqual(self.get(c, "act.bin"), b"active")
        with self.assertRaisesRegex(ftplib.error_perm, "^504"):
            c.sendcmd("PORT 10,1,2,3,4,5")
        with self.assertRaisesRegex(ftplib.error_perm, "^501"):
            c.sendcmd("PORT 1,2,3")
        c.set_pasv(True)
        port = int(re.search(r"\(\|\|\|(\d+)\|\)", c.sendcmd("EPSV")).group(1))
        with socket.create_connection(("127.0.0.1", port), 10) as d:
            c.putcmd("RETR act.bin")
            self.assertTrue(c.getresp().startswith("150"))
            self.assertEqual(d.recv(100), b"active")
        self.assertTrue(c.getresp().startswith("226"))
        # EPRT to the client's own address
        with socket.create_server(("127.0.0.1", 0)) as srv:
            p = srv.getsockname()[1]
            self.assertTrue(c.sendcmd(f"EPRT |1|127.0.0.1|{p}|").startswith("200"))
            c.putcmd("RETR act.bin")
            d, _ = srv.accept()
            with d:
                self.assertTrue(c.getresp().startswith("150"))
                self.assertEqual(d.recv(100), b"active")
            self.assertTrue(c.getresp().startswith("226"))
        with self.assertRaisesRegex(ftplib.error_temp, "^425"):
            c.sendcmd("RETR act.bin")       # no PASV or PORT since the last transfer

    def test_a_pasv_left_unused(self) -> None:
        c = self.client()
        self.put(c, "x.bin", b"right")
        port = int(re.search(r"\(\|\|\|(\d+)\|\)", c.sendcmd("EPSV")).group(1))
        stale = socket.create_connection(("127.0.0.1", port), 10)   # connects, then never used
        time.sleep(0.2)
        again = int(re.search(r"\(\|\|\|(\d+)\|\)", c.sendcmd("EPSV")).group(1))
        self.assertEqual(again, port)                                # the slot's own port, every time
        self.assertEqual(stale.recv(10), b"")                        # dropped by the second EPSV
        stale.close()
        self.assertEqual(self.get(c, "x.bin"), b"right")

    # ---- protocol edges ----

    def test_protocol_edges(self) -> None:
        c = self.client()
        with self.assertRaisesRegex(ftplib.error_perm, "^502"):
            c.sendcmd("XYZZY")
        c.sock.sendall(b"\xff\xf4\xff\xf2ABOR\r\n")
        self.assertEqual(c.getresp()[:3], "225")
        c.sock.sendall(b"NOOP " + b"a" * 5000 + b"\r\n")   # longer than any command: dropped
        c.sock.sendall(b"noop\r\n")
        self.assertTrue(c.getline().startswith("500"))
        self.assertEqual(c.getline(), "200 OK")
        self.assertTrue(c.sendcmd("TYPE I").startswith("200"))
        with self.assertRaisesRegex(ftplib.error_perm, "^504"):
            c.sendcmd("MODE B")
        self.assertTrue(c.sendcmd("OPTS UTF8 ON").startswith("200"))
        self.assertTrue(c.sendcmd("SITE CHMOD 755 x").startswith("200"))
        self.assertTrue(c.sendcmd("STAT").startswith("211"))
        self.assertTrue(c.sendcmd("HELP").startswith("214"))
        c.quit()

    def test_clients_at_once(self) -> None:
        clients = [self.client() for _ in range(SESSIONS)]
        for i, c in enumerate(clients):
            self.put(c, f"c{i}.bin", bytes([i]) * 1000)
        extra = socket.create_connection(("127.0.0.1", self.server.port), 10)
        self.assertTrue(extra.recv(200).startswith(b"421"))
        extra.close()
        clients[0].quit()
        self.clients.remove(clients[0])
        time.sleep(0.3)
        c = self.client()
        self.assertEqual(self.get(c, "c3.bin"), b"\x03" * 1000)

    def test_dropped_upload(self) -> None:
        c = self.client()
        port = int(re.search(r"\(\|\|\|(\d+)\|\)", c.sendcmd("EPSV")).group(1))
        d = socket.create_connection(("127.0.0.1", port), 10)
        c.putcmd("STOR dropped.bin")
        self.assertTrue(c.getresp().startswith("150"))
        d.sendall(os.urandom(100000))
        d.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
        d.close()   # a reset, not a clean end
        try:
            resp = c.getresp()
        except ftplib.error_temp as e:   # 426
            resp = str(e)
        self.assertRegex(resp, r"^(226|426)")
        c.sock.close()
        self.clients.remove(c)
        time.sleep(0.3)
        c2 = self.client()
        self.assertEqual(c2.size("dropped.bin"), (self.root / "dropped.bin").stat().st_size)

    def test_the_drive_tools(self) -> None:
        import profiles
        import usb_drive
        from test_usb import dump
        many = {f"data/many/f{i:02d}.bin": bytes([i]) * (1000 + i) for i in range(SESSIONS * 2)}
        src = dump(Path(self.dir.name) / "CUSA12345/app0", "CUSA12345", {"data/with space/ü.bin": b"u" * 77, **many})
        url = f"ftp://u:p@127.0.0.1:{self.server.port}/usb"
        (self.root / "usb/user's files.txt").write_bytes(b"keep")
        lines: list[str] = []
        info = usb_drive.put_title(src, url, "CUSA12345", "G12345", profiles.load("CUSA12345"), log=lines.append,
                                   jobs=SESSIONS + 3)
        self.assertTrue(any(f"copying over {SESSIONS} connection(s); the server refused more" in ln for ln in lines),
                        lines)
        self.assertEqual(info["copied"], 6 + len(many))
        folder = self.root / "usb/ghostldr/CUSA12345"
        for rel in usb_drive.tree(src):
            self.assertEqual((folder / "app0" / rel).read_bytes(), (src / rel).read_bytes(), rel)
        self.assertTrue((folder / "app0/data/emptydir").is_dir())
        self.assertEqual((folder / "prefix").read_text(), "G12345\n")
        info = usb_drive.put_title(src, url, "CUSA12345", "G12345", profiles.load("CUSA12345"), log=lines.append)
        self.assertEqual((info["copied"], info["kept"]), (0, 6 + len(many)))
        rows = usb_drive.check(url)
        self.assertEqual(rows[-1][0], "PASS", rows)
        self.assertTrue(usb_drive.remove_title(url, "CUSA12345"))
        self.assertFalse(folder.exists())
        self.assertEqual((self.root / "usb/user's files.txt").read_bytes(), b"keep")


if __name__ == "__main__":
    unittest.main()
