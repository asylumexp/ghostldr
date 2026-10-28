"""Games on a USB drive in folders of any name (docs/usb.md), offline:

  - the computer finds the same games, in the same order, with the same
    problems as the console (usb_drive.scan against ext/src/approot.c built
    for this machine, ext_native/approot_host.c, on one folder tree);
  - check: what the PS5 will find, broken folders, second copies, games too
    deep for it, sizes and save prefixes;
  - copy: where a copy goes (an existing one, --folder, ghostldr/<TITLE>),
    meta.bin always written, the check after copying catching a bad copy,
    the progress callback, removing only what ghostldr laid out;
  - ghostldr.py: its commands' help, copy and check on a mounted drive and
    through FTP, and boot's arguments to run.py (console and loader faked);
  - run.py: the app root reaching Lua intact, "auto" and the drive's games.

    python3 -m unittest discover -s ghostldr/tests -p test_usb_games.py
"""

from __future__ import annotations

import contextlib
import io
import re
import subprocess
import sys
import tempfile
import unicodedata
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
NATIVE = ROOT / "ghostldr/native"
sys.path.insert(0, str(ROOT / "ghostldr/host"))
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT))
import gfs_meta as gm  # noqa: E402
import profiles  # noqa: E402
import usb_drive as ud  # noqa: E402
from test_profiles import make_sfo  # noqa: E402
from test_usb import Disk, anywhere_files, build_with_hostfs, dump, have_disk_tools, put  # noqa: E402

CLI = ROOT / "ghostldr.py"


def cli(*args: str, timeout: int = 120) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(CLI), *args], capture_output=True, text=True, timeout=timeout)


class ScanTests(unittest.TestCase):
    """usb_drive.scan and the console's approot.c agree."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp_dir = tempfile.TemporaryDirectory()
        cls.tmp = Path(cls.tmp_dir.name)
        cls.fs = cls.tmp / "fs"
        anywhere_files(cls.fs)
        cls.drive = cls.fs / "mnt/usb1"
        cls.exe = cls.tmp / "approot_host"
        build_with_hostfs(cls.exe, [HERE / "ext_native/approot_host.c", NATIVE / "ext/src/approot.c",
                                    NATIVE / "ext/src/usb_live.c"],
                          [HERE / "ext_native", NATIVE / "ext/include", NATIVE / "include"])

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp_dir.cleanup()

    def console_list(self, fs: Path, drive: str, top: str = "") -> list[tuple]:
        out = subprocess.run([str(self.exe), str(fs), f"usb:/{top}", "--map", drive, "--select"],
                             capture_output=True, text=True, timeout=60).stdout
        rows = []
        for ln in out.splitlines():
            if ln.startswith("lib "):
                _, tid, flags, path, folder, note = ln[4:].split("\t")
                rows.append((tid, int(flags), path.removeprefix("/ghostldr_usb/"),
                             folder.removeprefix("/ghostldr_usb/"), note))
        return rows

    @staticmethod
    def computer_list(sc: ud.Scan) -> list[tuple]:
        return [(g.id if not g.problem or g.id else "", (1 if g.problem else 0) | (2 if g.duplicate_of else 0),
                 g.app0, g.folder, g.problem or (f"a second copy: --title boots the one in {g.duplicate_of}"
                                                 if g.duplicate_of else "")) for g in sc.games]

    def test_the_same_games_as_the_console(self) -> None:
        sc = ud.scan(self.drive)
        console = self.console_list(self.fs, "/mnt/usb1")
        self.assertEqual(self.computer_list(sc), console)
        self.assertEqual([(g.id, g.id_from) for g in sc.bootable], [
            ("CUSA00900", "meta.bin"), ("CUSA11456", "param.sfo"), ("CUSA11456", "param.sfo"),
            ("LAPY20001", "folder name")])
        self.assertEqual(sc.first("CUSA11456").folder, "My Games/ghost of tsushima")
        self.assertEqual(sc.games[0].name, "Bloodborne")                 # meta.bin's name

    def test_limits_as_the_console(self) -> None:
        fs = self.tmp / "big"
        for i in range(300):
            put(fs, f"mnt/usb2/f{i:03d}/readme.txt")
        put(fs, "mnt/usb2/f299/game/eboot.bin")
        put(fs, "mnt/usb2/f299/game/sce_sys/param.sfo", make_sfo({"TITLE_ID": "CUSA10101"}))
        sc = ud.scan(fs / "mnt/usb2")
        self.assertTrue(sc.cut)
        self.assertEqual(sc.games, [])                                   # past the folder limit, as on the console
        self.assertEqual(self.console_list(fs, "/mnt/usb2"), [])
        sc = ud.scan(fs / "mnt/usb2", "f299")                            # a folder narrows it
        self.assertEqual([g.id for g in sc.games], ["CUSA10101"])

    def test_title_ids_in_folder_names(self) -> None:
        self.assertEqual(ud.name_title("Ghost of Tsushima (CUSA11456)", False), "CUSA11456")
        self.assertEqual(ud.name_title("cusa11456-app", False), "CUSA11456")
        self.assertEqual(ud.name_title("XCUSA11456", False), "")          # inside a longer word
        self.assertEqual(ud.name_title("CUSA114567", False), "")
        self.assertEqual(ud.name_title("HOMEBREW_1", True), "HOMEBREW_1")  # the old layout: the whole name
        self.assertEqual(ud.name_title("HOMEBREW_1", False), "")

    def test_drive_titles_and_run_py(self) -> None:
        import run
        self.assertEqual(ud.drive_titles(self.drive), {
            "CUSA00900": self.drive / "Bloodborne copy/app0",
            "CUSA11456": self.drive / "My Games/ghost of tsushima",
            "LAPY20001": self.drive / "Homebrew/PS4 Xplorer (LAPY20001)"})
        self.assertEqual(sorted(run.usb_titles("usb:/", self.drive)), ["CUSA00900", "CUSA11456", "LAPY20001"])
        self.assertEqual(sorted(run.usb_titles("usb:/My Games", self.drive)), ["CUSA11456"])
        self.assertEqual(sorted(run.usb_titles("auto", self.drive)), ["CUSA00900", "CUSA11456", "LAPY20001"])
        empty = self.tmp / "empty"
        empty.mkdir(exist_ok=True)
        self.assertIsNone(run.usb_titles("auto", empty))                 # no games here: the installed titles
        self.assertIsNone(run.usb_titles("ghostfs", self.drive))

    def test_mounted_drives(self) -> None:
        mount = ("/dev/disk1s1 on / (apfs, local, journaled)\n"
                 "/dev/disk4s1 on /Volumes/MY DRIVE (exfat, local, nodev, nosuid, noowners)\n"
                 "/dev/disk5s1 on /Volumes/OLD (msdos, local, nodev, nosuid, noowners)\n")
        with mock.patch.object(ud.sys, "platform", "darwin"), \
                mock.patch.object(ud.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, mount, "")):
            table = ud.mount_table()
        self.assertEqual(table[Path("/Volumes/MY DRIVE")], "exfat")
        self.assertEqual(table[Path("/Volumes/OLD")], "msdos")
        self.assertEqual(table[Path("/")], "apfs")


class CheckTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.fs = Path(self.tmp_dir.name)
        anywhere_files(self.fs)
        self.drive = self.fs / "mnt/usb1"
        dump(self.drive / "Big/CUSA20202", "CUSA20202", {"data/huge.bin": b""})
        with open(self.drive / "Big/CUSA20202/data/huge.bin", "wb") as f:
            f.truncate(4 << 30)                                          # sparse

    def tearDown(self) -> None:
        self.tmp_dir.cleanup()

    def test_what_the_ps5_will_find(self) -> None:
        with mock.patch.object(ud, "fs_type", return_value="exfat"):
            r = ud.inspect(self.drive)
        rows = {t: s for s, t in r.rows}
        self.assertEqual(r.rows[0], ("PASS", "file system exFAT"))
        self.assertIn("PASS", [s for t, s in rows.items() if t.startswith("CUSA00900 Bloodborne in Bloodborne copy")])
        self.assertTrue(any(s == "PASS" and "saves in BBSAVE" in t for t, s in rows.items()))       # its prefix file
        self.assertTrue(any(s == "PASS" and t.startswith("CUSA11456") and "saves in GHOSTSAVE" in t
                            for t, s in rows.items()))                    # Ghost's profile's prefix
        self.assertTrue(any(s == "WARN" and "a second copy in ghostldr/CUSA11456" in t for t, s in rows.items()))
        self.assertEqual(rows["Broken/nothing names it: no sce_sys/param.sfo: name the folder after the game's ID "
                              "(CUSA12345)"], "FAIL")
        self.assertTrue(any(s == "WARN" and t.startswith("a/b/c/d/e: CUSA12121 the PS5 won't find")
                            for t, s in rows.items()))
        self.assertFalse(r.ok)
        text = ud.format_report(r)
        self.assertIn("Games the PS5 will find (5):", text)
        self.assertIn("Folders the PS5 can't start (4):", text)
        self.assertIn("a/b/c/d/e: more than 4 folders down from the top: move it up", text)
        self.assertIn("python3 ghostldr.py boot   (the PS5 shows a game selector)", text)
        self.assertNotIn(".Trashes", text)

    def test_fat32_and_a_4_gib_file(self) -> None:
        with mock.patch.object(ud, "fs_type", return_value="msdos"):
            r = ud.inspect(self.drive)
        self.assertEqual(r.rows[0][0], "WARN")
        self.assertTrue(any(s == "FAIL" and "CUSA20202" in t and "FAT32 holds no file of 4 GiB" in t
                            for s, t in r.rows))

    def test_the_cli(self) -> None:
        out = cli("check", str(self.drive))
        self.assertEqual(out.returncode, 1, out.stderr)                  # broken folders
        self.assertIn("Games the PS5 will find (5):", out.stdout)
        self.assertIn("LAPY20001", out.stdout)
        out = cli("check", str(self.fs / "nowhere"))
        self.assertEqual(out.returncode, 1)
        self.assertIn("no such folder", out.stderr)


class CopyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.tmp = Path(self.tmp_dir.name)
        self.drive = self.tmp / "DRIVE"
        self.drive.mkdir()
        self.src = dump(self.tmp / "dumps/CUSA12345-app", "CUSA12345")

    def tearDown(self) -> None:
        self.tmp_dir.cleanup()

    def put(self, **kw) -> dict:
        return ud.put_title(self.src, self.drive, "CUSA12345", "G12345", profiles.load("CUSA12345"),
                            log=lambda *a: None, **kw)

    def test_where_a_copy_goes(self) -> None:
        info = self.put(folder="My Games/A Game")
        self.assertEqual(info["rel"], "My Games/A Game")
        self.assertTrue((self.drive / "My Games/A Game/app0/eboot.bin").is_file())
        info = self.put()                                                 # its copy already there: updated
        self.assertEqual((info["rel"], info["copied"], info["kept"]), ("My Games/A Game", 0, 5))
        self.assertFalse((self.drive / "ghostldr").exists())
        self.assertEqual(ud.drive_titles(self.drive), {"CUSA12345": self.drive / "My Games/A Game/app0"})

    def test_meta_bin_names_a_dump_without_param_sfo(self) -> None:
        (self.src / "sce_sys/param.sfo").unlink()
        self.put(folder="Untitled")                                       # nothing else names it
        self.assertEqual(gm.parse((self.drive / "Untitled/meta.bin").read_bytes())["title"], "CUSA12345")
        g = ud.scan(self.drive).first("CUSA12345")
        self.assertEqual((g.folder, g.id_from), ("Untitled", "meta.bin"))

    def test_a_copy_made_by_hand(self) -> None:
        dump(self.drive / "mine", "CUSA12345")
        with self.assertRaisesRegex(ud.DriveError, 'already on the drive in "mine" \\(copied by hand\\)'):
            self.put()
        self.put(folder="ghostldr/CUSA12345")                             # somewhere else when asked
        self.assertEqual(ud.remove_title(self.drive, "CUSA12345"), ["ghostldr/CUSA12345"])
        self.assertTrue((self.drive / "mine/eboot.bin").is_file())        # never removes what it didn't lay out
        d = ud.LocalDrive(self.drive)
        self.assertIn("copied by hand", ud.removable(d, ud.scan(d).games[0]))

    def test_the_check_after_copying(self) -> None:
        class Flaky(ud.LocalDrive):                                       # loses a byte of eboot.bin `bad` times
            bad = 1

            def put_file(self, src, rel, progress=None):
                super().put_file(src, rel, progress)
                if rel.endswith("eboot.bin") and self.bad:
                    self.bad -= 1
                    p = self.root / rel
                    data = bytearray(p.read_bytes())
                    data[100] ^= 1
                    p.write_bytes(bytes(data))
        logged = []
        info = ud.put_title(self.src, Flaky(self.drive), "CUSA12345", "G12345", profiles.load("CUSA12345"),
                            log=logged.append)                            # caught, and copied again
        self.assertTrue(any("eboot.bin: differs from the dump's after copying; copying it again" in x for x in logged))
        app0 = self.drive / "ghostldr/CUSA12345/app0"
        self.assertEqual((app0 / "eboot.bin").read_bytes(), (self.src / "eboot.bin").read_bytes())
        bad = Flaky(self.drive)
        bad.bad = 2
        (app0 / "eboot.bin").unlink()
        with self.assertRaisesRegex(ud.DriveError, "eboot.bin differs from the dump's after copying it twice"):
            ud.put_title(self.src, bad, "CUSA12345", "G12345", profiles.load("CUSA12345"), log=lambda *a: None)
        (app0 / "data/a/b.bin").write_bytes(b"short")                    # cut short: copied again
        info = self.put(verify="full")                                    # every byte checked
        self.assertEqual(info["verified"], "5 file sizes and 5 file(s) byte for byte")
        self.assertEqual((app0 / "eboot.bin").read_bytes(), (self.src / "eboot.bin").read_bytes())

    def test_progress(self) -> None:
        seen = []
        self.put(on_progress=lambda done, need, count, todo: seen.append((done, need, count, todo)))
        total = sum(ud.tree(self.src).values())
        self.assertEqual(seen[-1], (total, total, 5, 5))

    def test_the_cli_copies_and_checks(self) -> None:
        out = cli("copy", str(self.src.parent / "CUSA12345-app"), str(self.drive), "--folder", "Games/Test")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("Title CUSA12345 (CUSA12345): 5 files", out.stdout)
        self.assertIn("Checked: 5 file sizes and 3 file(s) byte for byte", out.stdout)
        self.assertIn("python3 ghostldr.py boot CUSA12345", out.stdout)
        self.assertTrue((self.drive / "Games/Test/app0/eboot.bin").is_file())
        out = cli("copy", str(self.tmp / "dumps"), str(self.drive))      # not the game's folder
        self.assertEqual(out.returncode, 1)
        self.assertIn("has no eboot.bin: point at the game's folder", out.stderr)
        out = cli("copy", str(self.src), str(self.tmp / "nowhere"))
        self.assertIn("no such folder", out.stderr)

    def test_the_cli_through_ftp(self) -> None:
        from fake_ftp import FakeFtp
        console = self.tmp / "console"
        (console / "ghostldr_usb").mkdir(parents=True)
        (console / "ghostldr_usb/user's file.txt").write_bytes(b"keep")
        with FakeFtp(console) as srv:
            url = srv.url("/ghostldr_usb")
            out = cli("copy", str(self.src), url, "--folder", "My Games/Test Game")
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
            self.assertIn("Checked: 5 file sizes", out.stdout)
            out = cli("check", url)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
            self.assertIn("CUSA12345  Title CUSA12345", out.stdout)
            self.assertIn("My Games/Test Game", out.stdout)
            out = subprocess.run([sys.executable, str(ROOT / "ghostldr/host/usb_drive.py"), "list", url],
                                 capture_output=True, text=True, timeout=60)
            self.assertIn("  CUSA12345  Title CUSA12345  01.00", out.stdout)    # what the app parses
        self.assertTrue((console / "ghostldr_usb/My Games/Test Game/app0/eboot.bin").is_file())
        self.assertEqual((console / "ghostldr_usb/user's file.txt").read_bytes(), b"keep")


class ExfatTests(unittest.TestCase):
    """A real exFAT volume (macOS's tools): copy by name and by hand, the
    file system seen as exFAT, and the console's code finding the same games
    on it as check does."""

    @unittest.skipUnless(have_disk_tools(), "needs macOS's hdiutil and diskutil")
    def test_an_exfat_drive(self) -> None:
        import os
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            exe = tmp / "approot_host"
            build_with_hostfs(exe, [HERE / "ext_native/approot_host.c", NATIVE / "ext/src/approot.c",
                                    NATIVE / "ext/src/usb_live.c"],
                              [HERE / "ext_native", NATIVE / "ext/include", NATIVE / "include"])
            src = dump(tmp / "dumps/CUSA12345-app", "CUSA12345", {"data/Ünïcödé.bin": b"u" * 99})
            label = f"GLGAMES{os.getpid() % 1000:03d}"
            with Disk(tmp / "drive.img", 48 << 20, "ExFAT", "GPT", label) as d:
                out = cli("copy", str(src), str(d.mount), "--folder", "Jeux vidéo/Mon jeu (copie)")
                self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
                dump(d.mount / "by hand/CUSA67890 game", "CUSA67890")
                (d.mount / "by hand/CUSA67890 game/sce_sys/param.sfo").unlink()     # named by its folder
                r = ud.inspect(d.mount)
                self.assertEqual(r.rows[0], ("PASS", "file system exFAT"))
                nfc = lambda x: unicodedata.normalize("NFC", x)  # noqa: E731 - macOS hands back NFD
                self.assertEqual([(g.id, nfc(g.folder), g.id_from) for g in r.scan.games], [
                    ("CUSA12345", "Jeux vidéo/Mon jeu (copie)", "param.sfo"),
                    ("CUSA67890", "by hand/CUSA67890 game", "folder name")])
                self.assertTrue(r.ok, r.rows)
                d.remount()
                console = subprocess.run([str(exe), "/Volumes", "usb:/", "--map", f"/{label}", "--select"],
                                         capture_output=True, text=True, timeout=60).stdout
                found = [ln.split("\t")[1:4:2] for ln in console.splitlines() if ln.startswith("lib ")]
                self.assertEqual([(t, nfc(p)) for t, p in found], [
                    ("CUSA12345", "/ghostldr_usb/Jeux vidéo/Mon jeu (copie)/app0"),
                    ("CUSA67890", "/ghostldr_usb/by hand/CUSA67890 game")])


class CliTests(unittest.TestCase):
    def test_help(self) -> None:
        out = cli("--help")
        self.assertEqual(out.returncode, 0)
        for cmd in ("doctor", "copy", "check", "boot", "logs", "elfldr", "install"):
            self.assertRegex(out.stdout, rf"\n    {cmd} ")
            sub = cli(cmd, "--help")
            self.assertEqual(sub.returncode, 0, sub.stderr)
            self.assertIn(f"usage: ghostldr.py {cmd}", sub.stdout)
        self.assertEqual(cli().returncode, 0)                             # no command: the help
        out = cli("copy", "x", "--bogus")
        self.assertEqual(out.returncode, 2)
        self.assertIn("unknown option(s) --bogus", out.stderr)

    def boot(self, *args: str, listening: int = 9026, prebuilt: bool = True) -> tuple[int, list | None, str]:
        import ghostldr as gl
        seen = {}

        def fake_run(argv):
            seen["argv"] = argv
            return 0
        err = io.StringIO()
        with mock.patch.object(gl, "console", return_value="10.0.0.5"), \
                mock.patch.object(gl, "loader_listening", return_value=listening), \
                mock.patch.object(gl, "use_prebuilt", return_value=prebuilt), \
                mock.patch.object(gl, "run_py", side_effect=fake_run), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
            rc = gl.main(["boot", *args])
        return rc, seen.get("argv"), err.getvalue()

    def test_clear_cache(self) -> None:
        """ghostldr.py clear-cache: run.py clear-cache for the console found, its options passed on."""
        import ghostldr as gl
        seen = {}
        with mock.patch.object(gl, "console", return_value="10.0.0.5"), \
                mock.patch.object(gl, "loader_listening", return_value=9026), \
                mock.patch.object(gl, "run_py", side_effect=lambda argv: seen.setdefault("argv", argv) and 0), \
                contextlib.redirect_stdout(io.StringIO()):
            rc = gl.main(["clear-cache", "--loader-port", "9026"])
        self.assertEqual((rc, seen["argv"]), (0, ["clear-cache", "--host", "10.0.0.5", "--loader-port", "9026"]))
        sub = cli("clear-cache", "--help")
        self.assertEqual(sub.returncode, 0, sub.stderr)
        self.assertIn("GLDRCACHE", sub.stdout)

    def test_boot(self) -> None:
        rc, argv, _ = self.boot()
        self.assertEqual((rc, argv), (0, ["boot", "--app-root", "auto", "--no-build", "--host", "10.0.0.5"]))
        rc, argv, _ = self.boot("cusa11456", "--select", "--usb", "--snap", prebuilt=False)
        self.assertEqual(argv, ["boot", "--app-root", "usb:/", "--title", "CUSA11456", "--select", "--host",
                                "10.0.0.5", "--snap"])                  # other run.py options pass through
        rc, argv, _ = self.boot("tsushima", "--installed")              # part of a name: the profile's
        self.assertEqual(argv[:5], ["boot", "--app-root", "ghostfs", "--title", "CUSA11456"])
        rc, argv, _ = self.boot("--wait", "20", "--log", "x.log")        # options with values stay together
        self.assertEqual(argv, ["boot", "--app-root", "auto", "--no-build", "--host", "10.0.0.5", "--wait", "20",
                                "--log", "x.log"])
        rc, argv, _ = self.boot("--folder", "My Games")
        self.assertEqual(argv[:3], ["boot", "--app-root", "usb:/My Games"])
        rc, argv, err = self.boot(listening=0)
        self.assertEqual((rc, argv), (1, None))
        self.assertIn("isn't listening for ghostldr: start SWRR", err)
        self.assertIn("HALL OF FAME", err)


class RunPyTests(unittest.TestCase):
    def test_the_app_root_reaches_lua_intact(self) -> None:
        import run
        self.assertEqual(run.lua_str('usb:/My "Games"\\x'), '"usb:/My \\034Games\\034\\092x"')
        self.assertEqual(run.lua_str("usb:/Jeux/été"), '"usb:/Jeux/\\195\\169t\\195\\169"')
        if not run.CORE.exists():
            self.skipTest("no core built")
        payload = run.build_payload(0, "10.0.0.1", "usb:/My Games", {}).decode()
        self.assertIn('GHOSTLDR_APP_ROOT = "usb:/My Games"', payload)

    def test_auto_is_the_default(self) -> None:
        import run
        with mock.patch.object(run, "doctor", side_effect=lambda a: a.app_root == "auto" and 7), \
                mock.patch.object(run, "console_ip", return_value="10.0.0.5"):
            self.assertEqual(run.main(["doctor"]), 7)

    def test_doctor_without_a_console(self) -> None:
        import run
        out = io.StringIO()
        with mock.patch.object(run, "console_ip", side_effect=SystemExit("no PS4 or PS5 answered discovery")), \
                mock.patch.object(run, "build_payload", return_value=b""), contextlib.redirect_stdout(out):
            rc = run.main(["doctor"])
        self.assertEqual(rc, 1)
        self.assertRegex(out.getvalue(), r"FAIL\s+console\s+no PS4 or PS5 answered discovery")


if __name__ == "__main__":
    unittest.main()
