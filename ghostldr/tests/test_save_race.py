"""The extension's save writer (ghostldr/native/ext/src/save_race.c, the
resign without a jailbreak), built for this machine as x86-64 and run against
a fake console (ext_native/save_race_os.c) that models SWRR's save service as
measured on a 13.60 PS5: a read-write mount of another title's save is bound
first and refused after (0x809F000F); a descriptor held inside it keeps it
bound; paths into it stop resolving; closing SWRR commits it.  SWRR can't
open anything relative to a descriptor (openat: EPERM).

    python3 -m unittest test_save_race        (from ghostldr/tests)
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
NATIVE = HERE.parent / "native"
sys.path.insert(0, str(HERE))
from test_ext import x86_64_cc  # noqa: E402

TITLE, DIR = "CUSA07992", "SCUS-97429"
KEYSTONE = b"keystone" + bytes(range(1, 89))           # 96 bytes
FP = KEYSTONE[0x20:0x40].hex()
PARAM = b"\0PSF" + b"P" * 2724


def tree(root: Path) -> dict[str, bytes | None]:
    """relative path -> bytes (files) or None (folders)"""
    out: dict[str, bytes | None] = {}
    for p in sorted(root.rglob("*")):
        rel = p.relative_to(root).as_posix()
        out[rel] = None if p.is_dir() else p.read_bytes()
    return out


def write_tree(root: Path, files: dict[str, bytes | None]) -> None:
    for rel, data in files.items():
        p = root / rel
        if data is None:
            p.mkdir(parents=True, exist_ok=True)
        else:
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)


def param_sfo_with_title(title: str) -> bytes:
    """A minimal param.sfo with TITLE_ID only."""
    import struct
    key, data = b"TITLE_ID\0", title.encode() + b"\0\0\0"
    key_off, data_off = 0x14 + 16, 0x14 + 16 + 12
    return (b"\0PSF" + struct.pack("<IIII", 0x101, key_off, data_off, 1) +
            struct.pack("<HHIII", 0, 0x0204, 10, 12, 0) + key.ljust(12, b"\0") + data)


TARGET = {"sce_sys/param.sfo": PARAM, "sce_sys/keystone": KEYSTONE, "sce_sys/icon0.png": b"png",
          "VMC0.card": b"\x01" * 70000, "VMC1.card": b"\x02" * 1000, "trophy.dat": b"T" * 300,
          "lua/init.lua": b"-- ultrac0re", "lua/sub/x.lua": b"x", "old.bin": b"old"}
SOURCE = {"VMC0.card": b"\x0A" * 70000,                       # same size, other bytes
          "VMC1.card": b"\x0B" * 200000,                      # grown
          "trophy.dat": b"T" * 300,                           # the same
          "new.bin": b"new file",                             # made
          "slots/deep/a.sav": b"A" * 40000,                   # made, two folders deep
          "empty.bin": b""}


class SaveRaceBase(unittest.TestCase):
    """Builds the harness once per class; a fresh fake console (a save and a source) per test."""

    @classmethod
    def setUpClass(cls) -> None:
        cc = x86_64_cc()
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = Path(cls.tmp.name)
        free = ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                "-Wno-gnu-include-next", "-ffreestanding", "-fno-builtin", f"-I{HERE / 'ext_native'}",
                f"-I{NATIVE / 'ext/include'}", f"-I{NATIVE / 'include'}", f"-I{NATIVE / 'ext/src'}", "-DGHOST_ABI=0u"]
        objs = []
        for src in (HERE / "ext_native/save_race_host.c", NATIVE / "ext/src/save_race.c", NATIVE / "src/rt.c",
                    NATIVE / "ext/src/hle_fsvsh.c", NATIVE / "ext/src/sqlite_min.c"):
            obj = tmp / (src.stem + ".o")
            subprocess.run(cc + free + ["-c", str(src), "-o", str(obj)], check=True)
            objs.append(str(obj))
        obj = tmp / "save_race_os.o"
        subprocess.run(cc + ["-std=gnu11", "-O1", "-g", "-Wall", "-Wno-unused-result", "-c",
                             str(HERE / "ext_native/save_race_os.c"), "-o", str(obj)], check=True)
        objs.append(str(obj))
        cls.exe = tmp / "save_race_host"
        subprocess.run(cc + objs + ["-o", str(cls.exe), "-lpthread"], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.save = self.root / "saves" / TITLE / DIR
        write_tree(self.save, TARGET)
        self.src = self.root / "temp0/src"
        write_tree(self.src, SOURCE)

    def tearDown(self) -> None:
        self.dir.cleanup()

    def run_host(self, scenario: str, fp: str | None = FP, backup: bool = True, title: str = TITLE,
                 mode: str | None = None) -> tuple[dict, list[str]]:
        argv = [str(self.exe), str(self.root), scenario, title, DIR, "/temp0/src", fp or "-",
                "/temp0/backup" if backup else "-"] + ([mode] if mode else [])
        out = subprocess.run(argv, capture_output=True, text=True, timeout=120)
        self.assertEqual(out.returncode, 0, out.stderr)
        lines = out.stdout.splitlines()
        self.assertEqual([ln for ln in lines if ln.startswith("VIOLATION")], [], "\n".join(lines))
        res = [ln for ln in lines if ln.startswith("RESULT ")]
        self.assertEqual(len(res), 1, "\n".join(lines))
        r = dict(re.findall(r"(\w+)=(\S*)", res[0].split(" why=")[0]))
        r["why"] = res[0].split(" why=", 1)[1] if " why=" in res[0] else ""
        return r, lines

    def assert_sce_sys_untouched(self) -> None:
        got = tree(self.save)
        for k in ("sce_sys/param.sfo", "sce_sys/keystone", "sce_sys/icon0.png"):
            self.assertEqual(got[k], TARGET[k], k)


@unittest.skipUnless(x86_64_cc(), "no way to build and run x86-64 code here")
class SaveRaceWrite(SaveRaceBase):
    def payload(self) -> dict[str, bytes | None]:
        return {k: v for k, v in tree(self.save).items() if not k.startswith("sce_sys")}

    def expected(self) -> dict[str, bytes | None]:
        want = dict(SOURCE)
        want["slots"] = want["slots/deep"] = None
        return dict(sorted(want.items()))

    def test_ps5_write_commits_when_swrr_closes(self) -> None:
        r, lines = self.run_host("ps5")
        self.assertEqual((r["rc"], r["mount"], r["needs_exit"], r["committed"]), ("0", "809f000f", "1", "0"),
                         "\n".join(lines))
        self.assertEqual((r["bad"], r["extra"]), ("0", "0"))
        self.assertEqual(r["reserved"], "1")                         # its mount point stays taken until SWRR closes
        self.assertEqual(int(r["created"]), 3)                       # new.bin, slots/deep/a.sav, empty.bin
        self.assertEqual(int(r["dirs"]), 2)                          # slots, slots/deep
        self.assertEqual(int(r["deleted"]), 5)                       # old.bin, lua/init.lua, lua/sub/x.lua, lua/sub, lua
        self.assertGreaterEqual(int(r["unchanged"]), 1)              # trophy.dat
        self.assertEqual(self.payload(), self.expected())
        self.assert_sce_sys_untouched()
        self.assertTrue(any("CLOSE SWRR NOW" in ln for ln in lines))
        # the undo: the save's earlier files (sce_sys as far as an app may read it: not its keystone)
        backup = tree(self.root / "temp0/backup")
        self.assertEqual({k: v for k, v in backup.items() if v is not None},
                         {k: v for k, v in TARGET.items() if k != "sce_sys/keystone"})

    def test_nothing_lands_before_swrr_closes(self) -> None:
        r, _ = self.run_host("ps5", mode="noexit")
        self.assertEqual((r["rc"], r["needs_exit"]), ("0", "1"))
        self.assertEqual({k: v for k, v in tree(self.save).items() if v is not None}, TARGET)

    def test_without_the_fingerprint_what_only_the_save_has_stays(self) -> None:
        r, lines = self.run_host("ps5", fp=None)
        self.assertEqual((r["rc"], r["bad"], r["extra"], r["deleted"]), ("0", "0", "2", "0"), "\n".join(lines))
        self.assertIn("stay", r["why"])
        want = self.expected()
        want.update({k: v for k, v in TARGET.items() if k.startswith(("lua", "old"))}, lua=None, **{"lua/sub": None})
        self.assertEqual(self.payload(), dict(sorted(want.items())))
        # the undo: the files the source rewrote, as they were
        backup = {k: v for k, v in tree(self.root / "temp0/backup").items() if v is not None}
        self.assertEqual(backup, {k: TARGET[k] for k in ("VMC0.card", "VMC1.card", "trophy.dat")})

    def test_a_folder_in_the_save_that_is_a_file_in_the_source_writes_nothing(self) -> None:
        write_tree(self.src, {"lua": b"a file"})
        r, lines = self.run_host("ps5")
        self.assertNotEqual(r["rc"], "0")
        self.assertIn("nothing was written", r["why"])
        self.assertFalse([ln for ln in lines if " opens; " in ln])    # found from the listing, before the race
        self.assertEqual({k: v for k, v in tree(self.save).items() if v is not None}, TARGET)

    def test_a_path_that_cannot_be_held_writes_nothing(self) -> None:
        # without the fingerprint the clash is found only in the race: no file is written, nothing removed
        write_tree(self.src, {"lua": b"a file"})
        r, lines = self.run_host("ps5", fp=None)
        self.assertNotEqual(r["rc"], "0")
        self.assertIn("nothing was written", r["why"])
        self.assertEqual((r["written"], r["deleted"]), ("0", "0"))
        got = tree(self.save)
        for k, v in TARGET.items():
            self.assertEqual(got[k], v, k)                           # every file as it was
        made = {k for k, v in got.items() if k not in TARGET and v is not None}
        self.assertEqual(int(r["created"]), len(made))               # new ones made in the window stay, empty
        self.assertTrue(all(got[k] == b"" for k in made))
        if made:
            self.assertIn("new empty ones", r["why"])

    def test_a_source_of_the_saves_own_names(self) -> None:
        # every file is the save's already: nothing has to be made
        shutil.rmtree(self.src)
        same_names = {"VMC0.card": b"\x0C" * 70000, "VMC1.card": b"\x0D" * 10, "trophy.dat": b"U" * 300,
                      "lua/init.lua": b"-- new", "lua/sub/x.lua": b"y", "old.bin": b"OLD"}
        write_tree(self.src, same_names)
        r, lines = self.run_host("ps5")
        self.assertEqual((r["rc"], r["bad"], r["extra"], r["created"], r["deleted"]), ("0", "0", "0", "0", "0"),
                         "\n".join(lines))
        self.assertEqual({k: v for k, v in self.payload().items() if v is not None}, same_names)

    def test_a_console_that_unbinds_at_once_gets_nothing_written(self) -> None:
        r, lines = self.run_host("unbinds")
        self.assertNotEqual(r["rc"], "0")
        self.assertEqual(r["needs_exit"], "0")
        self.assertIn("unbound", r["why"] + "\n".join(lines))
        self.assertEqual({k: v for k, v in tree(self.save).items() if v is not None}, TARGET)

    def test_an_allowed_mount_is_committed_by_its_unmount(self) -> None:
        r, lines = self.run_host("allowed", mode="noexit")
        self.assertEqual((r["rc"], r["mount"], r["committed"], r["needs_exit"], r["reserved"]),
                         ("0", "0", "1", "0", "0"), "\n".join(lines))
        self.assertEqual(self.payload(), self.expected())

    def test_no_such_save(self) -> None:
        r, _ = self.run_host("ps5", title="CUSA00001")
        self.assertEqual(r["rc"], "809f0008")
        self.assertEqual(r["reserved"], "0")

    def test_swrrs_own_saves_are_refused(self) -> None:
        for title in ("CUSA03492", "CUSA03474"):                    # both regions: Luac0re saves live there
            r, lines = self.run_host("ps5", title=title)
            self.assertEqual(r["rc"], "8002000d", title)
            self.assertFalse([ln for ln in lines if "mount ->" in ln])

    def test_jak_x_only_when_it_is_not_the_host(self) -> None:
        r, _ = self.run_host("jakx-host")                           # UltraC0re in Jak X: its own save is the exploit
        self.assertEqual(r["rc"], "8002000d")
        self.assertEqual({k: v for k, v in tree(self.save).items() if v is not None}, TARGET)

    def test_sce_sys_in_the_source_is_ignored(self) -> None:
        write_tree(self.src, {"sce_sys/param.sfo": b"evil", "sce_sys/keystone": b"k" * 96})
        r, lines = self.run_host("ps5")
        self.assertEqual(r["rc"], "0", "\n".join(lines))
        self.assert_sce_sys_untouched()

    def test_check_after_the_commit(self) -> None:
        r, _ = self.run_host("ps5")
        self.assertEqual(r["rc"], "0")
        r, lines = self.run_host("ps5", mode="check")
        self.assertEqual((r["rc"], r["bad"], r["extra"]), ("0", "0", "0"), "\n".join(lines))
        (self.save / "VMC1.card").write_bytes(b"changed")
        (self.save / "stray.bin").write_bytes(b"s")
        r, _ = self.run_host("ps5", mode="check")
        self.assertNotEqual(r["rc"], "0")
        self.assertEqual((r["bad"], r["extra"]), ("1", "1"))
        r, _ = self.run_host("ps5", mode="check", fp="00" * 32)
        self.assertEqual(r["rc"], "809f0006")                         # the wrong fingerprint


@unittest.skipUnless(x86_64_cc(), "no way to build and run x86-64 code here")
class SaveRaceCopy(SaveRaceBase):
    """save_race_copy: the read-only copy of a save the service binds and then refuses."""

    def run_copy(self, scenario: str) -> tuple[dict, list[str], dict]:
        argv = [str(self.exe), str(self.root), scenario, TITLE, DIR, "/temp0/copy", "-", "-", "copy"]
        out = subprocess.run(argv, capture_output=True, text=True, timeout=120)
        self.assertEqual(out.returncode, 0, out.stderr)
        lines = out.stdout.splitlines()
        self.assertEqual([ln for ln in lines if ln.startswith("VIOLATION")], [], "\n".join(lines))
        res = [ln for ln in lines if ln.startswith("RESULT ")]
        self.assertEqual(len(res), 1, "\n".join(lines))
        peak = [int(m.group(1)) for ln in lines if (m := re.search(r"fake: peak (\d+) descriptors", ln))]
        r = dict(re.findall(r"(\w+)=(\S*)", res[0].split(" why=")[0]))
        r["peak"] = peak[0] if peak else -1
        return r, lines, tree(self.root / "temp0/copy")

    def expect_full_copy(self, got: dict, lines: list[str]) -> None:
        want = {k: v for k, v in TARGET.items() if k != "sce_sys/keystone"}  # no app may read the keystone
        self.assertEqual({k: v for k, v in got.items() if v is not None}, want, "\n".join(lines))

    def test_ps5_copies_every_readable_file(self) -> None:
        r, lines, got = self.run_copy("ps5")
        self.assertEqual(r["rc"], "0", "\n".join(lines))
        self.expect_full_copy(got, lines)

    def test_a_folder_caught_on_the_bare_mount_point_still_copies(self) -> None:
        # a racer opens /savedataN while only the bare mount point is there (it stays empty), and the save's files
        # show a moment later: the root must be listed through a descriptor opened after the pin
        r, lines, got = self.run_copy("latebind")
        self.assertEqual(r["rc"], "0", "\n".join(lines))
        self.expect_full_copy(got, lines)

    def test_a_race_holds_a_handful_of_descriptors(self) -> None:
        # 3 racers x 2 slots x (one folder + one file), plus the save's files while they are pinned
        r, lines, got = self.run_copy("ps5")
        self.assertGreater(r["peak"], 0, "\n".join(lines))
        self.assertLessEqual(r["peak"], 3 * 2 * 2 + len(TARGET) + 2, "\n".join(lines))


class SaveToolWrites(SaveRaceBase):
    """A save tool's (Apollo's) VSH mount, hle_fsvsh.c: a writable copy, written into the save at the unmount."""

    def run_tool(self, mode: str) -> tuple[dict, list[str]]:
        out = subprocess.run([str(self.exe), str(self.root), "ps5", TITLE, DIR, "-", FP, "-", mode],
                             capture_output=True, text=True, timeout=120)
        lines = out.stdout.splitlines()
        self.assertEqual([ln for ln in lines if ln.startswith("VIOLATION")], [], "\n".join(lines))
        res = [ln for ln in lines if ln.startswith("RESULT ")]
        self.assertEqual(len(res), 1, "\n".join(lines))
        return dict(re.findall(r"(\w+)=(\S*)", res[0])), lines

    def test_tool_changes_are_written_into_the_save(self) -> None:
        r, lines = self.run_tool("fsvsh")
        self.assertEqual((r["rc"], r["mount"]), ("0", "0"), "\n".join(lines))
        got = tree(self.save)
        self.assertEqual(got["VMC0.card"], b"\x5A" * 70000)
        self.assertEqual(got["cheat.txt"], b"infinite boost")
        self.assertEqual(got["lua/init.lua"], TARGET["lua/init.lua"])       # what the tool left alone stays
        self.assert_sce_sys_untouched()                                     # its param.sfo edit is not written
        self.assertTrue(any("CLOSE SWRR to keep them" in ln for ln in lines))

    def usb_save(self, keystone: bool) -> None:
        files = {"sce_sys/param.sfo": param_sfo_with_title(TITLE), "VMC0.card": b"\x0A" * 100}
        if keystone:
            files["sce_sys/keystone"] = KEYSTONE
        write_tree(self.root / "mnt/usb0/PS4/APOLLO/someone_CUSA07992_SCUS-97429", files)

    def test_the_keystone_is_learned_from_a_decrypted_save_on_usb(self) -> None:
        self.usb_save(keystone=True)
        out = subprocess.run([str(self.exe), str(self.root), "ps5", TITLE, DIR, "-", "-", "-", "fsvsh-learn"],
                             capture_output=True, text=True, timeout=120)
        lines = out.stdout.splitlines()
        self.assertTrue(any("keystone, from a decrypted save on USB (PS4/APOLLO/someone_CUSA07992_SCUS-97429)" in ln
                            for ln in lines), "\n".join(lines))
        self.assertEqual((self.root / f"data/ghostldr/keystones/{TITLE}.keystone").read_bytes(), KEYSTONE)
        got = tree(self.save)
        self.assertEqual(got["VMC0.card"], b"\x5A" * 70000)                 # then written as with the keystone
        self.assert_sce_sys_untouched()

    def test_without_a_keystone_anywhere_the_user_is_told(self) -> None:
        self.usb_save(keystone=False)
        out = subprocess.run([str(self.exe), str(self.root), "ps5", TITLE, DIR, "-", "-", "-", "fsvsh-learn-ro"],
                             capture_output=True, text=True, timeout=120)
        lines = out.stdout.splitlines()
        self.assertTrue(any("banner: No keystone for CUSA07992" in ln for ln in lines), "\n".join(lines))
        self.assertTrue(any("its sce_sys/keystone while bound: not readable" in ln for ln in lines), "\n".join(lines))
        self.assertFalse((self.root / f"data/ghostldr/keystones/{TITLE}.keystone").exists())

    def test_an_unchanged_copy_writes_nothing(self) -> None:
        r, lines = self.run_tool("fsvsh-same")
        self.assertEqual((r["rc"], r["mount"]), ("0", "0"), "\n".join(lines))
        self.assertFalse([ln for ln in lines if "refused after binding" in ln])
        self.assertEqual({k: v for k, v in tree(self.save).items() if v is not None}, TARGET)


@unittest.skipUnless(x86_64_cc(), "no way to build and run x86-64 code here")
class SaveScan(SaveRaceBase):
    """The game selector's save scan for a save tool (hle_fsvsh.c save_prescan): candidates from the title's /data
    and its bundled zip, one question per title, progress, the per-user cache in /data and how long it holds."""

    SAVES = "CUSA00001=2,CUSA00003=1"
    NOW = 1_800_000_000

    def setUp(self) -> None:
        super().setUp()
        import zipfile
        write_tree(self.root / "data", {"ghostldr/titles.txt": b"CUSA00001\nCUSA00002 # named by the user\n",
                                        "apollo/data/CUSA00005.keystone": b"k" * 96,
                                        "apollo/data/readme.txt": b"not a title"})
        z = self.root / "app0/assets/misc/appdata.zip"
        z.parent.mkdir(parents=True)
        with zipfile.ZipFile(z, "w") as f:
            f.writestr("data/CUSA00003.keystone", b"k" * 96)
            f.writestr("data/CUSA00004.savepatch", b"; cheats")
            f.writestr("data/CUSA00004.txt", b"no title")
            f.writestr("misc/CUSA0000X.keystone", b"not a title ID")

    def scan(self, user: str = "10000001", stop: int = 0, now: int = NOW) -> tuple[dict, list[str], list[str]]:
        out = subprocess.run([str(self.exe), str(self.root), "ps5", self.SAVES, user, str(stop), "-", "-", "prescan"],
                             capture_output=True, text=True, timeout=120, env={**os.environ, "FAKE_NOW": str(now)})
        self.assertEqual(out.returncode, 0, out.stderr)
        lines = out.stdout.splitlines()
        res = [ln for ln in lines if ln.startswith("RESULT ")]
        self.assertEqual(len(res), 1, "\n".join(lines))
        asked = [ln.split()[-1] for ln in lines if ln.startswith("search ")]
        return dict(re.findall(r"(\w+)=(\S*)", res[0])), asked, [ln for ln in lines if ln.startswith("progress ")]

    def cache(self) -> list[str]:
        return sorted((self.root / "data/ghostldr/savesearch.txt").read_text().splitlines())

    def test_every_candidate_is_asked_once_with_progress(self) -> None:
        r, asked, progress = self.scan()
        self.assertEqual(sorted(asked), [f"CUSA0000{i}" for i in range(1, 6)])
        self.assertEqual(progress, [f"progress {i}/5" for i in range(6)])     # the total first, then each title
        self.assertEqual(r["with"], "2")
        self.assertEqual(self.cache(), sorted(f"10000001 CUSA0000{i} {h} {self.NOW}"
                                              for i, h in ((1, 2), (2, 0), (3, 1), (4, 0), (5, 0))))

    def test_a_cached_scan_asks_nothing_and_shows_no_progress(self) -> None:
        self.scan()
        r, asked, progress = self.scan(now=self.NOW + 3600)
        self.assertEqual((asked, progress, r["with"]), ([], [], "2"))

    def test_each_user_has_its_own_answers(self) -> None:
        self.scan()
        r, asked, _ = self.scan(user="10000002")
        self.assertEqual(len(asked), 5)
        self.assertEqual(len(self.cache()), 10)
        r, asked, _ = self.scan(user="10000001")
        self.assertEqual(asked, [])

    def test_a_stopped_scan_keeps_what_it_found(self) -> None:
        r, asked, progress = self.scan(stop=2)
        self.assertEqual((len(asked), progress[-1]), (2, "progress 2/5"))
        self.assertEqual(len(self.cache()), 2)
        r, asked, progress = self.scan()
        self.assertEqual((len(asked), progress[0], r["with"]), (3, "progress 0/3", "2"))

    def test_titles_without_saves_are_asked_again_after_a_week(self) -> None:
        self.scan()
        _, asked, _ = self.scan(now=self.NOW + 6 * 86400)
        self.assertEqual(asked, [])
        _, asked, _ = self.scan(now=self.NOW + 8 * 86400)
        self.assertEqual(sorted(asked), ["CUSA00002", "CUSA00004", "CUSA00005"])   # not the two with saves


if __name__ == "__main__":
    unittest.main()
