"""tools/tls_sites.py (the offline TLS site list) and its profile record:
a synthetic module whose .eh_frame describes one function, Ghost's eboot when
build/ghost/app0 is there, and profiles.py's REC_TLS.  Needs capstone.

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "ghostldr/tools"))
sys.path.insert(0, str(ROOT / "ghostldr/host"))
sys.path.insert(0, str(HERE))
import orbis_build as ob  # noqa: E402
import tls_sites  # noqa: E402

try:
    import capstone  # noqa: F401
except ImportError:
    capstone = None

GHOST = ROOT / "build/ghost/app0/eboot.bin"


@unittest.skipUnless(capstone, "capstone is not installed")
class TlsSites(unittest.TestCase):
    def test_functions_only_and_refusals(self) -> None:
        text = bytearray(b"\xcc" * 0x400)
        func = (bytes.fromhex("666666644 88b042500000000".replace(" ", ""))   # mov rax, fs:[0] (relaxed)
                + bytes.fromhex("644c031c2500000000")                           # add r11, fs:[0]
                + bytes.fromhex("64488b3c2530000000")                           # mov rdi, fs:[0x30]: refused
                + b"\xc3")
        text[0x100:0x100 + len(func)] = func
        text[0x200:0x209] = bytes.fromhex("64488b042500000000")   # the same bytes as data
        frame, hdr = ob.eh_frame(0x300, 0x380, [(0x100, 0x40)])
        text[0x300:0x300 + len(frame)] = frame
        text[0x380:0x380 + len(hdr)] = hdr
        with tempfile.TemporaryDirectory() as tmp:
            path = ob.Module("libSceTest", text=bytes(text), eh_frame_hdr=0x380, eh_frame=0x300,
                             eh_keep=True).write(Path(tmp) / "test.prx")
            r = tls_sites.sites(path)
            out = Path(tmp) / "sites.txt"
            sys.argv = ["tls_sites.py", str(path), "--out", str(out)]
            tls_sites.main()
            listed = [int(x, 16) for x in out.read_text().splitlines() if not x.startswith("#")]
        self.assertEqual(r["functions"], 1)
        self.assertEqual(r["patch"], [0x103, 0x10C])
        self.assertEqual([a for a, _ in r["refuse"]], [0x115])
        self.assertEqual(r["scan"], [0x103, 0x10C, 0x200])
        self.assertEqual(r["outside"], [0x200])
        self.assertEqual(listed, [0x103, 0x10C])

    @unittest.skipUnless(GHOST.exists(), "build/ghost/app0/eboot.bin missing")
    def test_ghost_list_is_the_core_scan(self) -> None:
        """Ghost's 1157 sites: the list the profile carries patches exactly what the scan did."""
        import profiles
        r = tls_sites.sites(GHOST)
        self.assertEqual(len(r["patch"]), 1157)
        self.assertEqual(r["patch"], r["scan"])
        self.assertEqual(r["outside"], [])
        self.assertEqual(profiles.tls_site_list("CUSA11456-tls-eboot.txt"), r["patch"])


class TlsProfileRecord(unittest.TestCase):
    def test_ghost_profile_carries_its_list(self) -> None:
        import profiles
        blob = profiles.compile_blob(profiles.load("CUSA11456"))
        recs = [d for k, d in profiles.read_blobs(blob)[0]["records"] if k == profiles.REC_TLS]
        self.assertEqual(len(recs), 1)
        module, count = struct.unpack_from("<32sI", recs[0])
        offs = list(struct.unpack_from(f"<{count}I", recs[0], 36))
        self.assertEqual(module.rstrip(b"\0"), b"eboot.bin")
        self.assertEqual(offs, profiles.tls_site_list("CUSA11456-tls-eboot.txt"))
        self.assertEqual(count, 1157)

    def test_validation(self) -> None:
        import profiles
        good = profiles.load("CUSA11456")
        bad = dict(good, tls_sites={"eboot.bin": "missing.txt"})
        self.assertTrue(any("no file" in e for e in profiles.validate(bad)))
        bad = dict(good, eboot={})
        self.assertTrue(any("tls_sites" in e and "fingerprint" in e for e in profiles.validate(bad)))


if __name__ == "__main__":
    unittest.main()
