"""The drawn dialogs' bitmap font (ext/src/dlg_font.h) is what
tools/mkdlgfont.py makes from the selector's Noto Sans with the selector's
own rasterizer: regenerated here and compared."""

from __future__ import annotations

import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("cc") or shutil.which("clang"), "no C compiler for this computer")
class DialogFont(unittest.TestCase):
    def test_header_matches_the_generator(self) -> None:
        p = subprocess.run([sys.executable, str(ROOT / "ghostldr/tools/mkdlgfont.py"), "--check"],
                           capture_output=True, text=True, timeout=300)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
