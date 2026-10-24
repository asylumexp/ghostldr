"""sha256.c against hashlib: the FIPS 180-4 test vectors plus the
padding edges (55/56/63/64/65 bytes) and a large buffer.

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = """
#include <stdio.h>
#include <stdlib.h>
void sha256(const unsigned char *data, unsigned long len, unsigned char out[32]);
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    static unsigned char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    unsigned char out[32];
    sha256(buf, n, out);
    for (int i = 0; i < 32; i++) printf("%02x", out[i]);
    printf("\\n");
    return 0;
}
"""


@unittest.skipUnless(shutil.which("cc") or shutil.which("gcc"), "no host C compiler")
class Sha256(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        exe = Path(cls.tmp.name) / "sha256_test"
        drv = Path(cls.tmp.name) / "driver.c"
        drv.write_text(DRIVER)
        cc = shutil.which("cc") or shutil.which("gcc")
        subprocess.run([cc, "-O1", "-std=gnu11", f"-I{ROOT / 'ghostldr/native/include'}",
                        str(ROOT / "ghostldr/native/src/sha256.c"), str(drv), "-o", str(exe)],
                       check=True, capture_output=True, text=True)
        cls.exe = exe

    def digest(self, data: bytes) -> str:
        p = Path(self.tmp.name) / "in.bin"
        p.write_bytes(data)
        r = subprocess.run([str(self.exe), str(p)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.strip()

    def test_vectors(self) -> None:
        cases = [
            b"",
            b"abc",
            b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",  # 56 bytes: two-block tail
            b"a" * 55, b"a" * 56, b"a" * 63, b"a" * 64, b"a" * 65,
            bytes(range(256)) * 40,
            b"\x00" * 1000,
        ]
        for data in cases:
            self.assertEqual(self.digest(data), hashlib.sha256(data).hexdigest(),
                             f"len {len(data)}")

    def test_large(self) -> None:
        data = bytes((i * 31 + 7) & 0xFF for i in range(1 << 20))
        self.assertEqual(self.digest(data), hashlib.sha256(data).hexdigest())


if __name__ == "__main__":
    unittest.main()
