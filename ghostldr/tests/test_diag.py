"""Logs on the USB drive and failures on the TV (docs/logs.md,
docs/failures.md), built for this machine (ext_native/diag_host.c): the
core's log.c with the extension's logfile.c and failure.c, against a fake
console whose /temp0, USB drives and saved data are a directory.

    python3 -m unittest test_diag        (from ghostldr/tests)
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
NATIVE = HERE.parent / "native"
sys.path.insert(0, str(HERE))
from test_profiles import host_cc  # noqa: E402


class LogsAndFailures(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = host_cc()
        if not cc:
            raise unittest.SkipTest("no C compiler whose programs run on this machine")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name) / "diag_host"
        sources = [HERE / "ext_native/diag_host.c", NATIVE / "src/log.c", NATIVE / "ext/src/logfile.c",
                   NATIVE / "ext/src/failure.c"]
        subprocess.run(cc + ["-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                             "-Wno-gnu-include-next", "-Wno-unknown-pragmas", "-ffreestanding", "-fno-builtin",
                             f"-I{HERE / 'ext_native'}", f"-I{NATIVE / 'ext/include'}", f"-I{NATIVE / 'include'}",
                             "-DGHOST_ABI=0u", *map(str, sources), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_host(self, scenario: str, env: dict | None = None) -> list[str]:
        with tempfile.TemporaryDirectory() as root:
            out = subprocess.run([str(self.exe), root, scenario], capture_output=True, text=True, timeout=120,
                                 env={**os.environ, **(env or {})})
        lines = out.stdout.splitlines()
        self.assertEqual([ln for ln in lines if ln.startswith("FAILED")], [], "\n".join(lines))
        self.assertEqual(out.returncode, 0, out.stderr + "\n".join(lines))
        return lines

    def test_lines_reach_temp0_and_the_drive_once_each(self) -> None:
        lines = self.run_host("log")
        self.assertIn("ok the drive back: what it missed is copied first", lines)
        self.assertIn("ok /temp0 past 8 MiB: renamed to ghostldr.1.log and started again", lines)

    def test_names_and_the_30_kept(self) -> None:
        self.assertIn("ok files that aren't ghostldr's logs are left alone", self.run_host("names"))

    def test_the_setting_off(self) -> None:
        self.assertIn("ok nothing written to the drive", self.run_host("off"))

    def test_no_drive(self) -> None:
        self.assertIn("ok where: /temp0", self.run_host("nodrive"))

    def test_every_boot_failure_has_its_message(self) -> None:
        lines = self.run_host("boot")
        self.assertIn("ok the failed boot's log is on the drive", lines)

    def test_a_crash_keeps_its_page(self) -> None:
        lines = self.run_host("crash")
        self.assertIn("ok crash: where, for a report", lines)
        self.assertIn("ok crash: how full the title's memory was", lines)

    def test_an_exit_closes_swrr_after_its_page(self) -> None:
        lines = self.run_host("exit")
        self.assertIn("_exit(3)", lines)
        self.assertTrue(any(ln.startswith("page: The game has quit.") for ln in lines), lines)

    def test_the_page_draws(self) -> None:
        """dlg_overlay.c's ov_page on this machine; kept as build/ghostldr/failure-page.png with Pillow"""
        exe = Path(self.tmp.name) / "page_host"
        subprocess.run(host_cc() + ["-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                                    "-Wno-gnu-include-next", "-Wno-unknown-pragmas", "-ffreestanding", "-fno-builtin",
                                    f"-I{HERE / 'ext_native'}", f"-I{NATIVE / 'ext/include'}",
                                    f"-I{NATIVE / 'include'}", "-DGHOST_ABI=0u", "-include", "ext.h",
                                    str(HERE / "ext_native/page_host.c"), str(NATIVE / "ext/src/dlg_overlay.c"),
                                    "-o", str(exe)], check=True)
        ppm = Path(self.tmp.name) / "page.ppm"
        out = subprocess.run([str(exe), str(ppm), "The game crashed.",
                              "It stopped working and can't go on.\n\nWhat to do: To play again, close SWRR from "
                              "the PS button's menu and start it again.",
                              "The log: USB drive: ghostldr/logs/0012_2026-10-28_2115.log\nCrash: signal 11 at "
                              "eboot.bin+1234"], capture_output=True, text=True, timeout=60)
        self.assertEqual(out.returncode, 0, out.stdout)
        self.assertEqual(ppm.stat().st_size, 1920 * 1080 * 3 + len("P6\n1920 1080\n255\n"))
        try:
            from PIL import Image
            Image.open(ppm).save(HERE.parents[1] / "build/ghostldr/failure-page.png")
        except (ImportError, OSError):
            pass

    def test_a_still_screen_is_said(self) -> None:
        self.assertIn("ok a still screen: a notification", self.run_host("hang"))


if __name__ == "__main__":
    unittest.main()
