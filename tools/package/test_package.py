#!/usr/bin/env python3
"""Tests of the installer builds' own pieces, offline (no downloads, no makensis).

    python3 -m unittest discover -s tools/package -p 'test_*.py'

The packages themselves are checked by smoke_test.py, on an installed copy.
"""

from __future__ import annotations

import hashlib
import io
import struct
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_all  # noqa: E402
import build_deb  # noqa: E402
import build_windows  # noqa: E402
import common as C  # noqa: E402


def parse_ar(data: bytes) -> list:
    assert data[:8] == b"!<arch>\n"
    out, pos = [], 8
    while pos < len(data):
        head = data[pos:pos + 60]
        assert head[58:60] == b"`\n", head
        name, size = head[:16].decode().rstrip(), int(head[48:58])
        out.append((name, data[pos + 60:pos + 60 + size], head))
        pos += 60 + size + (size % 2)
    return out


class Temp(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()


class DebTest(Temp):
    def test_ar_layout_as_dpkg_writes_it(self) -> None:
        data = build_deb.ar([("debian-binary", b"2.0\n", 1), ("control.tar.xz", b"abc", 1), ("data.tar.xz", b"", 1)])
        members = parse_ar(data)
        self.assertEqual([m[0] for m in members], ["debian-binary", "control.tar.xz", "data.tar.xz"])
        self.assertEqual(members[0][1], b"2.0\n")
        self.assertEqual(members[1][1], b"abc")                        # odd size: padded with \n after
        self.assertEqual(members[0][2][:16], b"debian-binary   ")         # no trailing slash
        self.assertEqual(members[0][2][40:48].decode().strip(), "100644")
        with self.assertRaises(SystemExit):
            build_deb.ar([("a-name-longer-than-15", b"", 1)])

    def test_data_tar(self) -> None:
        root = self.tmp / "root"
        (root / "usr/bin").mkdir(parents=True)
        (root / "usr/bin/tool").write_text("#!/bin/sh\n")
        (root / "usr/bin/tool").chmod(0o700)
        (root / "opt/x").mkdir(parents=True)
        (root / "opt/x/data.txt").write_text("hello")
        entries, md5sums, kib = build_deb.data_entries(root)
        tool_md5 = hashlib.md5("#!/bin/sh\n".encode()).hexdigest()
        self.assertEqual(md5sums, f"{hashlib.md5(b'hello').hexdigest()}  opt/x/data.txt\n{tool_md5}  usr/bin/tool\n")
        self.assertGreaterEqual(kib, 2)
        with tarfile.open(fileobj=io.BytesIO(build_deb.tar_xz(entries, 1234))) as t:
            members = {m.name: m for m in t.getmembers()}
        self.assertEqual(list(members)[:3], [".", "./opt", "./opt/x"])        # (stored as ./opt/ and so on)
        tool = members["./usr/bin/tool"]
        self.assertEqual((tool.uid, tool.gid, tool.uname, tool.mode, tool.mtime), (0, 0, "root", 0o755, 1234))
        self.assertEqual(members["./opt/x/data.txt"].mode, 0o644)

    def test_control(self) -> None:
        text = build_deb.control("1.2.3", 42)
        fields = dict(line.split(": ", 1) for line in text.splitlines() if not line.startswith(" "))
        self.assertEqual((fields["Package"], fields["Version"], fields["Architecture"]), ("ghostldr", "1.2.3", "all"))
        self.assertEqual(fields["Depends"], "python3 (>= 3.11)")
        self.assertEqual(fields["Installed-Size"], "42")
        self.assertTrue(text.endswith("\n"))
        for line in text.splitlines():
            self.assertLessEqual(len(line), 80)


class WindowsTest(Temp):
    def test_embeddable_pth(self) -> None:
        z = self.tmp / "embed.zip"
        with zipfile.ZipFile(z, "w") as f:
            for n in ("python.exe", "pythonw.exe", "python399.zip"):
                f.writestr(n, b"x")
            f.writestr("python399._pth", "python399.zip\r\n.\r\n\r\n#import site\r\n")
        with mock.patch.object(C, "fetch", return_value=z):
            xy = build_windows.embeddable_python(self.tmp, self.tmp / "py")
        self.assertEqual(xy, "399")
        pth = (self.tmp / "py/python399._pth").read_bytes().decode()
        lines = [x.partition("#")[0].strip() for x in pth.split("\r\n")]
        self.assertEqual([x for x in lines if x], ["python399.zip", ".", "..\\ghostldr\\host", "..\\ghostldr\\tools",
                                                   "import site"])

    def test_unsafe_zip(self) -> None:
        z = self.tmp / "bad.zip"
        with zipfile.ZipFile(z, "w") as f:
            f.writestr("../evil.txt", b"x")
        with self.assertRaises(SystemExit):
            build_windows.unzip(z, self.tmp / "out")

    def test_remove_lists_spare_build(self) -> None:
        rel = self.tmp / "release"
        for d in ("app", "ghostldr", "build/ghostldr"):
            (rel / d).mkdir(parents=True)
        (rel / "LICENSE").write_text("")
        text = build_windows.nsis_lists(rel, self.tmp / "files.nsh").read_text()
        self.assertIn('RMDir /r "$INSTDIR\\app"', text)
        self.assertIn('Delete "$INSTDIR\\LICENSE"', text)
        self.assertNotIn("build", text)


class TwoFileBootTest(Temp):
    def test_copied_beside_the_installers_and_summed(self) -> None:
        src = C.REPO / "build/ghostldr"
        if not all((src / n).is_file() for n in build_all.TWO_FILE):
            self.skipTest("make -C ghostldr/native first")
        copied = build_all.two_file_boot(self.tmp)
        self.assertEqual([p.name for p in copied], ["ghostldr.lua", "ghostldr-ext.bin", "ghostldr-elfldr.bin"])
        for p in copied:
            self.assertEqual(p.read_bytes(), (src / p.name).read_bytes())
        sums = C.write_sums(self.tmp, [p.name for p in copied]).read_text()
        for p in copied:
            self.assertIn(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n", sums)

    def test_missing_files_stop_the_release(self) -> None:
        with mock.patch.object(C, "REPO", self.tmp), self.assertRaises(SystemExit) as e:
            build_all.two_file_boot(self.tmp / "out")
        self.assertIn("ghostldr.lua", str(e.exception))


class CommonTest(Temp):
    def tarball(self, members: list) -> Path:
        p = self.tmp / "t.tar.gz"
        with tarfile.open(p, "w:gz") as t:
            for name, kind, target in members:
                info = tarfile.TarInfo(name)
                if kind == "sym":
                    info.type, info.linkname = tarfile.SYMTYPE, target
                    t.addfile(info)
                else:
                    info.size = 1
                    t.addfile(info, io.BytesIO(b"x"))
        return p

    def test_extract_tar_refuses_escapes_and_case_twins(self) -> None:
        for members in ([("../x", "file", "")], [("/abs", "file", "")], [("a/l", "sym", "../../x")],
                        [("a/T", "file", ""), ("a/t", "file", "")]):
            with self.subTest(members=members), self.assertRaises(SystemExit):
                C.extract_tar(self.tarball(members), self.tmp / "out")
        C.extract_tar(self.tarball([("p/ok", "file", ""), ("p/link", "sym", "ok"), ("p/share/T", "file", ""),
                                    ("p/share/t", "file", "")]), self.tmp / "out", skip=("p/share/",))
        self.assertTrue((self.tmp / "out/p/link").is_symlink())
        self.assertFalse((self.tmp / "out/p/share").exists())

    def test_ico(self) -> None:
        pngs = {}
        for s in (16, 32, 256):
            pngs[s] = self.tmp / f"{s}.png"
            pngs[s].write_bytes(b"\x89PNG\r\n\x1a\n" + bytes([s % 256]) * s)
        data = C.write_ico(pngs, self.tmp / "x.ico").read_bytes()
        self.assertEqual(struct.unpack_from("<HHH", data), (0, 1, 3))
        sizes = []
        for i in range(3):
            w, h, _, _, planes, bpp, size, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
            sizes.append(w or 256)
            self.assertEqual(data[offset:offset + 8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual((planes, bpp, size), (1, 32, 8 + (w or 256)))
        self.assertEqual(sizes, [16, 32, 256])

    def test_nothing_personal(self) -> None:
        d = self.tmp / "pkg"
        d.mkdir()
        (d / "ok.txt").write_text("see 192.168.1.23 or 10.0.0.2")
        C.check_nothing_personal([d])
        (d / "ip.txt").write_text("console at 192.168.7.61")
        with self.assertRaises(SystemExit):
            C.check_nothing_personal([d])
        C.check_nothing_personal([d], scan_ips=False)
        (d / "path.txt").write_text(f"built in {Path.home()}/src")
        with self.assertRaises(SystemExit):
            C.check_nothing_personal([d], scan_ips=False)

    def test_sources_are_pinned(self) -> None:
        src = C.sources()
        for key, f in src["files"].items():
            self.assertRegex(f["sha256"], r"^[0-9a-f]{64}$", key)
            self.assertTrue(f["url"].startswith(("https://github.com/astral-sh/python-build-standalone/releases/",
                                                 "https://www.python.org/ftp/python/",
                                                 "https://github.com/AppImage/type2-runtime/releases/")), key)
        self.assertTrue(all(src["python_version"] in urlpart for urlpart in
                            [f["url"].replace("%2B", "+") for k, f in src["files"].items() if k.startswith("python")]))

    def test_version(self) -> None:
        self.assertRegex(C.app_version(), r"^\d+\.\d+\.\d+")


if __name__ == "__main__":
    unittest.main()
