"""The FTP server's /saves (ghostldr/native/ext/src/ftp_saves.c, Settings >
Advanced "Saves in FTP"), built for this machine as x86-64 and run against a
fake console (ext_native/ftp_saves_os.c): the save service answers for exact
title IDs, mounts the host's (SWRR's) saves freely and another title's only
read-only with its fingerprint, and refuses another account's; save_race.c's
copies and writes (the race a save tool under ghostldr uses) are faked as
always winning, six race copies a session.

    python3 -m unittest test_ftp_saves        (from ghostldr/tests)
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

ALICE, BOB = "10000001", "10000002"
USERS = f"{ALICE}:Alice,{BOB}:Bob"
HOST = "CUSA03474"
KEYSTONE = b"keystone" + bytes(range(1, 89))  # 96 bytes
FP = KEYSTONE[0x20:0x40].hex()


@unittest.skipUnless(x86_64_cc(), "no way to build and run x86-64 code here")
class FtpSaves(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = x86_64_cc()
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = Path(cls.tmp.name)
        free = ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                "-Wno-gnu-include-next", "-ffreestanding", "-fno-builtin", f"-I{HERE / 'ext_native'}",
                f"-I{NATIVE / 'ext/include'}", f"-I{NATIVE / 'include'}", "-DGHOST_ABI=0u",
                "-DIDLE_MS=200", "-DOWN_IDLE_MS=100", "-DWAIT_MS=5000", "-DCOMMIT_MS=100"]
        objs = []
        for src in (HERE / "ext_native/ftp_saves_host.c", NATIVE / "ext/src/ftp_saves.c", NATIVE / "src/rt.c"):
            obj = tmp / (src.stem + ".o")
            subprocess.run(cc + free + ["-c", str(src), "-o", str(obj)], check=True)
            objs.append(str(obj))
        obj = tmp / "ftp_saves_os.o"
        subprocess.run(cc + ["-std=gnu11", "-O1", "-g", "-Wall", "-Wno-unused-result", "-c",
                             str(HERE / "ext_native/ftp_saves_os.c"), "-o", str(obj)], check=True)
        objs.append(str(obj))
        cls.exe = tmp / "ftp_saves_host"
        subprocess.run(cc + objs + ["-o", str(cls.exe), "-lpthread"], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        # SWRR's own saves: Alice's Luac0re save and ghostldr's containers, Bob's Luac0re save
        self.put(f"saves/{ALICE}/{HOST}/SLUS-20268/VMC0.card", b"memory card")
        self.put(f"saves/{ALICE}/{HOST}/GHOSTSAVE/sdmemory_0.bin", b"ghost progress")
        self.put(f"saves/{ALICE}/{HOST}/GLDRCONF/settings.txt", b"ftp_saves=on\n")
        self.put(f"saves/{ALICE}/{HOST}/GHOST000/data.bin", b"install data")
        self.put(f"saves/{ALICE}/{HOST}/GHOSTD001SAVE/save.bin", b"a ghost save")
        self.put(f"saves/{BOB}/{HOST}/SLUS-20268/VMC0.card", b"bob's card")
        # Jak X (an exploit title: always asked about), with its fingerprint known from a USB keystone, and
        # ghostldr's install data from when it ran ghostldr
        self.put(f"saves/{ALICE}/CUSA07992/SCUS-97429/lua/a.lua", b"jak x")
        self.put(f"saves/{ALICE}/CUSA07992/GHOST000/data.bin", b"install data")
        self.put(f"saves/{ALICE}/CUSA07992/GHOSTSAVE/sdmemory_0.bin", b"jak x's ghost progress")
        # the other region's SWRR, keystone known
        self.put(f"saves/{ALICE}/CUSA03492/SLES-50366/VMC0.card", b"eu card")
        self.put("fp/CUSA03492", FP.encode())
        self.put("ghostldr_usb/ghostldr/keystones/CUSA03492.keystone", KEYSTONE)
        self.put("fp/CUSA07992", FP.encode())
        self.put("ghostldr_usb/PS4/APOLLO/1000000a_CUSA07992_SCUS-97429/sce_sys/keystone", KEYSTONE)
        # a game named only by the system's copy on USB, no keystone anywhere
        self.put(f"saves/{ALICE}/CUSA00001/SAVE0/data.bin", b"nokey")
        self.put("fp/CUSA00001", ("ab" * 32).encode())
        self.put("ghostldr_usb/PS4/SAVEDATA/0123456789abcdef/CUSA00001/sdimg_SAVE0", b"")
        # a game nothing names
        self.put(f"saves/{ALICE}/CUSA00009/HIDDEN/data.bin", b"hidden")
        self.put("fp/CUSA00009", FP.encode())

    def tearDown(self) -> None:
        self.dir.cleanup()

    def put(self, rel: str, data: bytes) -> None:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)

    def run_script(self, *cmds: str, flag: bool = True) -> tuple[dict[str, str], list[str]]:
        script = self.root.parent / (self.root.name + ".script")
        script.write_text("\n".join([f"flag {int(flag)}", *cmds, "stop"]) + "\n")
        out = subprocess.run([str(self.exe), str(self.root), USERS, str(script)], capture_output=True, text=True,
                             timeout=120)
        script.unlink()
        self.assertEqual(out.returncode, 0, out.stderr)
        lines = out.stdout.splitlines()
        self.assertEqual([ln for ln in lines if ln.startswith("VIOLATION")], [], "\n".join(lines))
        res: dict[str, str] = {}
        for ln in lines:
            m = re.match(r"OUT (\w+ \S*): (.*)$", ln)
            if m:
                res[m.group(1)] = m.group(2)
        return res, lines

    def test_off_there_is_no_saves_folder(self) -> None:
        r, _ = self.run_script("owns /saves", flag=False)
        self.assertEqual(r["owns /saves"], "0 root -")

    def test_users_titles_and_saves(self) -> None:
        r, lines = self.run_script("owns /saves", "ls /saves", "ls /saves/Alice", "ls /saves/Bob", "ls /saves/Alice/CUSA07992",
                                   f"ls /saves/Alice/{HOST}", "stat /saves/Alice/CUSA07992/SCUS-97429")
        self.assertEqual(r["owns /saves"], "1 root saves")
        self.assertEqual(r["ls /saves"].split(), ["Alice", "Bob"])
        self.assertEqual(sorted(r["ls /saves/Alice"].split()), sorted([HOST, "CUSA03492", "CUSA07992", "CUSA00001"]))
        self.assertEqual(r["ls /saves/Alice/CUSA07992"].split(), ["GHOSTSAVE", "SCUS-97429"])  # no install data
        self.assertEqual(r["ls /saves/Bob"].split(), [HOST])
        # ghostldr's install data and settings are no saves
        self.assertEqual(r[f"ls /saves/Alice/{HOST}"].split(), ["GHOSTD001SAVE", "GHOSTSAVE", "SLUS-20268"])
        self.assertTrue(r["stat /saves/Alice/CUSA07992/SCUS-97429"].startswith("dir 0 mode 555"))
        # listing a title or stat-ing its saves mounts nothing; nothing named CUSA00009
        self.assertIn("peak 0", lines)
        self.assertFalse(any("search" in ln and "CUSA00009" in ln for ln in lines))

    def test_reading_the_hosts_and_another_titles_save(self) -> None:
        r, lines = self.run_script(f"cat /saves/Bob/{HOST}/SLUS-20268/VMC0.card",
                                   "ls /saves/Alice/CUSA07992/SCUS-97429",
                                   "cat /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua",
                                   "stat /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua")
        self.assertEqual(r[f"cat /saves/Bob/{HOST}/SLUS-20268/VMC0.card"], "bob's card")
        self.assertEqual(r["ls /saves/Alice/CUSA07992/SCUS-97429"].split(), ["lua"])
        self.assertEqual(r["cat /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua"], "jak x")
        self.assertTrue(r["stat /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua"].startswith("file 5 mode 555"))

    def test_no_keystone_reads_through_a_race_copy_once(self) -> None:
        r, lines = self.run_script("ls /saves/Alice/CUSA00001/SAVE0", "cat /saves/Alice/CUSA00001/SAVE0/data.bin",
                                   "stat /saves/Alice/CUSA00001/SAVE0/data.bin")
        self.assertEqual(r["ls /saves/Alice/CUSA00001/SAVE0"].split(), ["data.bin"])
        self.assertEqual(r["cat /saves/Alice/CUSA00001/SAVE0/data.bin"], "nokey")
        self.assertTrue(r["stat /saves/Alice/CUSA00001/SAVE0/data.bin"].startswith("file 5 mode 555"))
        self.assertEqual(len([ln for ln in lines if ln.startswith("race_copy")]), 1, "\n".join(lines))
        self.assertIn("peak 0", lines)  # nothing stays mounted for it

    def test_another_accounts_save_reads_through_a_race_copy(self) -> None:
        self.put(f"saves/{ALICE}/CUSA07992/SCUS-97429/.other_account", b"")
        r, lines = self.run_script("cat /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua",
                                   "put /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua changed")
        self.assertEqual(r["cat /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua"], "jak x")
        self.assertEqual(r["put /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua"], "error 13")  # no writable copy
        self.assertTrue(any(ln.startswith("race_copy") for ln in lines))

    def test_six_race_copies_a_session(self) -> None:
        cmds = []
        for i in range(7):
            self.put(f"saves/{ALICE}/CUSA00001/SAVE{i + 1}/data.bin", b"n")
            cmds.append(f"cat /saves/Alice/CUSA00001/SAVE{i + 1}/data.bin")
        r, _ = self.run_script(*cmds)
        self.assertEqual([r[c] for c in cmds], ["n"] * 6 + ["error 16"])

    def test_swrrs_own_saves_are_never_written(self) -> None:
        r, _ = self.run_script(f"write /saves/Alice/{HOST}/SLUS-20268/new.bin", "mkdir /saves/Alice/x",
                               f"mkdir /saves/Alice/{HOST}/SLUS-20268/d", f"rm /saves/Alice/{HOST}/SLUS-20268/VMC0.card",
                               "rmdir /saves/Alice/CUSA07992/SCUS-97429", "mkdir /saves/Alice/CUSA07992/NEWSAVE")
        self.assertEqual(r[f"write /saves/Alice/{HOST}/SLUS-20268/new.bin"], "error 30")
        self.assertEqual(r["mkdir /saves/Alice/x"], "error 2")  # not a title ID: no such folder
        self.assertEqual(r[f"mkdir /saves/Alice/{HOST}/SLUS-20268/d"], "error 30")
        self.assertEqual(r[f"rm /saves/Alice/{HOST}/SLUS-20268/VMC0.card"], "error 30")
        self.assertEqual(r["rmdir /saves/Alice/CUSA07992/SCUS-97429"], "error 30")  # a save itself
        self.assertEqual(r["mkdir /saves/Alice/CUSA07992/NEWSAVE"], "error 30")
        self.assertEqual((self.root / f"saves/{ALICE}/{HOST}/SLUS-20268/VMC0.card").read_bytes(), b"memory card")

    def test_the_other_regions_swrr_is_never_written(self) -> None:
        r, lines = self.run_script("cat /saves/Alice/CUSA03492/SLES-50366/VMC0.card",
                                   "put /saves/Alice/CUSA03492/SLES-50366/VMC0.card x")
        self.assertEqual(r["cat /saves/Alice/CUSA03492/SLES-50366/VMC0.card"], "eu card")
        self.assertEqual(r["put /saves/Alice/CUSA03492/SLES-50366/VMC0.card"], "error 30")
        self.assertFalse(any(ln.startswith("fp_copy") for ln in lines))

    def test_writing_another_titles_save_through_its_writable_copy(self) -> None:
        save = self.root / f"saves/{ALICE}/CUSA07992/SCUS-97429"
        self.put(f"saves/{ALICE}/CUSA07992/SCUS-97429/sce_sys/param.sfo", b"sfo")
        self.put(f"saves/{ALICE}/CUSA07992/SCUS-97429/old.bin", b"old")
        r, lines = self.run_script("put /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua patched",
                                   "cat /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua",  # the copy, before the commit
                                   "mkdir /saves/Alice/CUSA07992/SCUS-97429/cheats",
                                   "put /saves/Alice/CUSA07992/SCUS-97429/cheats/c.txt on",
                                   "mv /saves/Alice/CUSA07992/SCUS-97429/old.bin /saves/Alice/CUSA07992/SCUS-97429/new.bin",
                                   "put /saves/Alice/CUSA07992/SCUS-97429/sce_sys/param.sfo evil",
                                   "stat /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua",
                                   "sleep 50", "idle",  # too soon: nothing written yet
                                   "sleep 200", "idle")
        self.assertEqual(r["put /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua"], "error 0")
        self.assertEqual(r["cat /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua"], "patched")
        self.assertEqual(r["put /saves/Alice/CUSA07992/SCUS-97429/sce_sys/param.sfo"], "error 13")
        self.assertTrue(r["stat /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua"].startswith("file 7 mode 7"))
        writes = [ln for ln in lines if ln.startswith("race_write")]
        self.assertEqual(len(writes), 1, "\n".join(lines))
        self.assertTrue(any(ln.startswith("fp_copy") for ln in lines))
        self.assertEqual((save / "lua/a.lua").read_bytes(), b"patched")
        self.assertEqual((save / "cheats/c.txt").read_bytes(), b"on")
        self.assertEqual((save / "new.bin").read_bytes(), b"old")
        self.assertFalse((save / "old.bin").exists())
        self.assertEqual((save / "sce_sys/param.sfo").read_bytes(), b"sfo")
        self.assertTrue(any("CLOSE SWRR" in ln for ln in lines))

    def test_no_keystone_no_writes(self) -> None:
        r, lines = self.run_script("put /saves/Alice/CUSA00001/SAVE0/data.bin x")
        self.assertEqual(r["put /saves/Alice/CUSA00001/SAVE0/data.bin"], "error 13")
        self.assertEqual((self.root / f"saves/{ALICE}/CUSA00001/SAVE0/data.bin").read_bytes(), b"nokey")

    def test_changes_left_when_the_server_stops_are_written(self) -> None:
        _, lines = self.run_script("put /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua late")
        self.assertEqual(len([ln for ln in lines if ln.startswith("race_write")]), 1)
        self.assertEqual((self.root / f"saves/{ALICE}/CUSA07992/SCUS-97429/lua/a.lua").read_bytes(), b"late")

    def test_unchanged_copies_are_not_written(self) -> None:
        _, lines = self.run_script("put /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua jak x", "sleep 200", "idle")
        self.assertEqual([ln for ln in lines if ln.startswith("race_write")], [])

    def test_a_folder_named_after_a_title_asks_about_it(self) -> None:
        r, lines = self.run_script("ls /saves/Alice", "mkdir /saves/Alice/CUSA00009", "mkdir /saves/Alice/CUSA00042",
                                   "ls /saves/Alice", "cat /saves/Alice/CUSA00009/HIDDEN/data.bin")
        listings = [ln.split(":", 1)[1].split() for ln in lines if ln.startswith("OUT ls /saves/Alice:")]
        self.assertNotIn("CUSA00009", listings[0])
        self.assertEqual(r["mkdir /saves/Alice/CUSA00009"], "error 0")
        self.assertEqual(r["mkdir /saves/Alice/CUSA00042"], "error 2")
        self.assertIn("CUSA00009", r["ls /saves/Alice"].split())
        self.assertEqual(r["cat /saves/Alice/CUSA00009/HIDDEN/data.bin"], "hidden")  # no keystone: a race copy

    def test_idle_saves_are_unmounted_and_held_ones_kept(self) -> None:
        r, lines = self.run_script(f"hold /saves/Alice/{HOST}/SLUS-20268/VMC0.card",
                                   f"cat /saves/Bob/{HOST}/SLUS-20268/VMC0.card",
                                   "cat /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua",
                                   "sleep 150", "idle",  # SWRR's own go first
                                   "sleep 250", "idle",
                                   "cat /saves/Alice/CUSA07992/SCUS-97429/lua/a.lua",
                                   f"cat /saves/Alice/{HOST}/GHOSTSAVE/sdmemory_0.bin",
                                   "drop", "sleep 400", "idle")
        unmounts = [ln for ln in lines if "unmounted" in ln]
        self.assertIn(f"{HOST}/SLUS-20268 of user {BOB}", unmounts[0], "\n".join(lines))
        self.assertIn(f"CUSA07992/SCUS-97429 of user {ALICE}", unmounts[1], "\n".join(lines))
        self.assertNotIn(f"{HOST}/SLUS-20268 of user {ALICE}", " ".join(unmounts[:2]))  # held
        self.assertEqual(r[f"cat /saves/Alice/{HOST}/GHOSTSAVE/sdmemory_0.bin"], "ghost progress")
        peak = int(next(ln for ln in lines if ln.startswith("peak")).split()[1])
        self.assertLessEqual(peak, 3)

    def test_more_saves_than_mounts_while_all_are_held(self) -> None:
        r, _ = self.run_script(f"hold /saves/Alice/{HOST}/SLUS-20268/VMC0.card",
                               f"hold /saves/Alice/{HOST}/GHOSTSAVE/sdmemory_0.bin",
                               f"hold /saves/Bob/{HOST}/SLUS-20268/VMC0.card",
                               f"cat /saves/Alice/{HOST}/GHOSTD001SAVE/save.bin",
                               "drop")
        self.assertEqual(r[f"cat /saves/Alice/{HOST}/GHOSTD001SAVE/save.bin"], "error 16")  # EBUSY

    def test_the_running_games_containers_are_refused(self) -> None:
        r, _ = self.run_script(f"cat /saves/Alice/{HOST}/GLDRCONF/settings.txt", "title GHOST",
                               f"cat /saves/Alice/{HOST}/GHOSTSAVE/sdmemory_0.bin",
                               f"cat /saves/Alice/{HOST}/GHOSTD001SAVE/save.bin",
                               f"cat /saves/Alice/{HOST}/SLUS-20268/VMC0.card")
        self.assertEqual(r[f"cat /saves/Alice/{HOST}/GLDRCONF/settings.txt"], "error 2")  # never listed
        self.assertEqual(r[f"cat /saves/Alice/{HOST}/GHOSTSAVE/sdmemory_0.bin"], "error 16")
        self.assertEqual(r[f"cat /saves/Alice/{HOST}/GHOSTD001SAVE/save.bin"], "error 16")
        self.assertEqual(r[f"cat /saves/Alice/{HOST}/SLUS-20268/VMC0.card"], "memory card")

    def test_a_save_tools_data_names_titles_and_keystones(self) -> None:
        # Apollo's /data under ghostldr: <prefix>D000SAVE of SWRR's user, with its save scan's answers and keystones
        self.put(f"saves/{ALICE}/{HOST}/G00004D000SAVE/ghostldr/savesearch.txt",
                 f"{ALICE} CUSA00009 1 1700000000\n{ALICE} CUSA00010 0 1700000000\n".encode())
        self.put(f"saves/{ALICE}/{HOST}/G00004D000SAVE/ghostldr/keystones/CUSA00009.keystone", KEYSTONE)
        r, lines = self.run_script("ls /saves/Alice", "cat /saves/Alice/CUSA00009/HIDDEN/data.bin")
        self.assertIn("CUSA00009", r["ls /saves/Alice"].split())
        self.assertEqual(r["cat /saves/Alice/CUSA00009/HIDDEN/data.bin"], "hidden")
        self.assertFalse(any("search" in ln and "CUSA00010" in ln for ln in lines))  # no saves when last asked

    def test_paths_that_name_nothing(self) -> None:
        r, _ = self.run_script("ls /saves/Carol", "ls /saves/Alice/CUSA99999", "stat /saves/Alice/notatitle",
                               f"stat /saves/Alice/{HOST}/NOPE", f"cat /saves/Alice/{HOST}/SLUS-20268/nope.bin")
        self.assertEqual(r["ls /saves/Carol"], "error 2")
        self.assertEqual(r["ls /saves/Alice/CUSA99999"], "error 2")
        self.assertEqual(r["stat /saves/Alice/notatitle"], "error 2")
        self.assertEqual(r[f"stat /saves/Alice/{HOST}/NOPE"], "error 2")
        self.assertEqual(r[f"cat /saves/Alice/{HOST}/SLUS-20268/nope.bin"], "error 2")


if __name__ == "__main__":
    unittest.main()
