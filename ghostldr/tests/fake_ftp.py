"""A small FTP server over a host folder, for usb_drive.py's FTP drives
(test_usb.py).  It answers the commands ftplib sends and a PS5's ftpsrv
takes: USER PASS TYPE PWD CWD PASV LIST STOR RETR SIZE MKD RMD DELE RNFR
RNTO NOOP QUIT FEAT SELF, with Unix-style LIST lines and DELE's 226 reply
like ftpsrv's.  As ftpsrv does, it starts in SELF transfer mode: files with
a SELF's magic are shown "decrypted" (another size and contents) by LIST,
SIZE and RETR until the SELF command toggles it off.  Paths are absolute
("/mnt/usb0/...") and map under the folder."""

from __future__ import annotations

import socket
import socketserver
import threading
import time
from pathlib import Path


SELF_MAGIC = bytes.fromhex("4f153d1d")


class _Handler(socketserver.StreamRequestHandler):
    def reply(self, line: str) -> None:
        self.wfile.write((line + "\r\n").encode())

    def view(self, p: Path) -> bytes:
        """A file as LIST, SIZE and RETR show it: a SELF "decrypted" in SELF mode."""
        data = p.read_bytes()
        if self.self_mode and data.startswith(SELF_MAGIC):
            return b"\x7fELF" + data[8:]
        return data

    def path(self, arg: str) -> Path:
        root = self.server.root
        p = (root / arg.lstrip("/")).resolve()
        if root != p and root not in p.parents:
            raise PermissionError(arg)
        return p

    def data(self) -> socket.socket:
        conn, _ = self.pasv.accept()
        self.pasv.close()
        self.pasv = None
        return conn

    def handle(self) -> None:
        self.pasv = None
        self.self_mode = True
        rnfr = None
        self.reply("220 fake ftp")
        for raw in self.rfile:
            line = raw.decode("utf-8", "surrogateescape").rstrip("\r\n")
            cmd, _, arg = line.partition(" ")
            cmd = cmd.upper()
            self.server.log.append(line)
            try:
                if cmd == "USER":
                    self.reply("331 ok")
                elif cmd == "PASS":
                    self.reply("230 ok")
                elif cmd in ("TYPE", "NOOP", "CWD"):
                    self.reply("200 ok")
                elif cmd == "PWD":
                    self.reply('257 "/"')
                elif cmd == "PASV":
                    if self.pasv:
                        self.pasv.close()
                    self.pasv = socket.socket()
                    self.pasv.bind(("127.0.0.1", 0))
                    self.pasv.listen(1)
                    port = self.pasv.getsockname()[1]
                    self.reply(f"227 Entering Passive Mode (127,0,0,1,{port >> 8},{port & 255})")
                elif cmd == "LIST":
                    p = self.path(arg)
                    if not p.is_dir():
                        self.reply("550 not a folder")
                        continue
                    self.reply("150 here")
                    with self.data() as c:
                        for e in [".", ".."] + sorted(x.name for x in p.iterdir()):
                            q = p / e
                            st = q.stat()
                            kind = "d" if q.is_dir() else "-"
                            size = len(self.view(q)) if q.is_file() else st.st_size
                            when = time.strftime("%b %d %H:%M", time.gmtime(st.st_mtime))
                            c.sendall(f"{kind}rwxrwxrwx 1 0 0 {size} {when} {e}\r\n".encode())
                    self.reply("226 done")
                elif cmd == "STOR":
                    p = self.path(arg)
                    if not p.parent.is_dir():
                        self.reply("550 no folder")
                        continue
                    self.reply("150 send")
                    with self.data() as c, open(p, "wb") as f:
                        while chunk := c.recv(1 << 16):
                            f.write(chunk)
                    self.reply("226 stored")
                elif cmd == "RETR":
                    p = self.path(arg)
                    if not p.is_file():
                        self.reply("550 no file")
                        continue
                    self.reply("150 here")
                    with self.data() as c:
                        c.sendall(self.view(p))
                    self.reply("226 done")
                elif cmd == "SIZE":
                    p = self.path(arg)
                    self.reply(f"213 {len(self.view(p))}" if p.is_file() else "550 no file")
                elif cmd == "FEAT":
                    for ln in ("211-Features:", " MLSD", " SELF", "211 End"):
                        self.reply(ln)
                elif cmd == "SELF":
                    self.self_mode = not self.self_mode
                    self.reply(f"226 SELF transfer mode {'enabled' if self.self_mode else 'disabled'}")
                elif cmd == "MKD":
                    p = self.path(arg)
                    if p.exists():
                        self.reply("550 exists")
                    else:
                        p.mkdir()
                        self.reply(f'257 "{arg}"')
                elif cmd == "RMD":
                    self.path(arg).rmdir()
                    self.reply("250 removed")
                elif cmd == "DELE":
                    p = self.path(arg)
                    if not p.is_file():
                        self.reply("550 no file")
                        continue
                    p.unlink()
                    self.reply("226 File deleted")      # what ftpsrv says (ftplib wants 250)
                elif cmd == "RNFR":
                    rnfr = self.path(arg)
                    self.reply("350 next" if rnfr.exists() else "550 no file")
                elif cmd == "RNTO":
                    to = self.path(arg)
                    if rnfr is None:
                        self.reply("550 refused")
                    else:
                        rnfr.replace(to)
                        self.reply("250 renamed")
                    rnfr = None
                elif cmd == "QUIT":
                    self.reply("221 bye")
                    return
                else:
                    self.reply("502 not here")
            except (OSError, PermissionError) as e:
                self.reply(f"550 {e}")


    def finish(self) -> None:
        if self.pasv:
            self.pasv.close()
        super().finish()


class FakeFtp(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, root: Path):
        super().__init__(("127.0.0.1", 0), _Handler)
        self.root = Path(root).resolve()
        self.log: list[str] = []
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)

    def __enter__(self) -> "FakeFtp":
        self.thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self.shutdown()
        self.server_close()

    def url(self, path: str) -> str:
        return f"ftp://127.0.0.1:{self.server_address[1]}{path}"
