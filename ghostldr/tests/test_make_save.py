"""tools/make_save.py (docs/autoload.md, "Making the saves"), offline, on a
made-up Luac0re save: what goes in, param.sfo made anyone's and big enough,
the other region's relabel, Apollo's USB layout, the refusals, and the saves
beside a release (make_release.py --save).

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import hashlib
import importlib.util
import io
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
_spec = importlib.util.spec_from_file_location("make_save", ROOT / "tools/make_save.py")
S = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(S)
R = S.make_release

ACCOUNT, USER, PSID_HMAC = bytes.fromhex("1122334455667788"), bytes.fromhex("44332211"), bytes(range(1, 33))


def sfo(entries: list[tuple[str, int, bytes, int]]) -> bytes:
    """A param.sfo: (key, fmt, value, max length) each."""
    keys, data, index = b"", b"", b""
    for k, fmt, v, mx in entries:
        index += struct.pack("<HHIII", len(keys), fmt, len(v), mx, len(data))
        keys += k.encode() + b"\0"
        data += v.ljust(mx, b"\0")
    kt = 20 + len(index)
    kt_pad = (kt + len(keys) + 3) & ~3
    return b"\0PSF" + struct.pack("<IIII", 0x101, kt, kt_pad, len(entries)) + index + keys.ljust(kt_pad - kt,
                                                                                               b"\0") + data


def save_sfo(title: str = "CUSA03492", directory: str = "SLES-50366") -> bytes:
    params = bytearray(0x400)
    params[4:8], params[8:0x28] = USER, PSID_HMAC
    params[0x2C:0x2C + len(title)] = params[0x3C:0x3C + len(title)] = title.encode()
    params[0x60:0x68] = b"\x9e\xba\xd8\x6a\x9e\xba\xd8\x6a"
    return sfo([("ACCOUNT_ID", 4, ACCOUNT, 8), ("MAINTITLE", 0x204, b"Saved Data\0", 128),
                ("PARAMS", 4, bytes(params), 0x400), ("SAVEDATA_BLOCKS", 4, struct.pack("<Q", 586), 8),
                ("SAVEDATA_DIRECTORY", 0x204, directory.encode() + b"\0", 32), ("SUBTITLE", 0x204, b"\0", 128),
                ("TITLE_ID", 0x204, title.encode() + b"\0", 12)])


def fake_save(base: Path, **kw) -> Path:
    """A decrypted Luac0re EU save, as Apollo exports one: the save's folder in a folder."""
    root = base / "PS4/APOLLO/1a2b3c4d_CUSA03492_SLES-50366"
    (root / "lua").mkdir(parents=True)
    (root / "sce_sys").mkdir()
    (root / "VMC0.card").write_bytes(b"\0" * 64 + b"BASLUS-20268\0" + b"\0" * 64 + b"BESLES-50366\0" + bytes(1000))
    (root / "VMC1.card").write_bytes(bytes(8650752))
    (root / "disc-swap-cli.conf").write_text("--max-disc-num=0\n--boot-disc-id=0\n--ps2-title-id=SLES-50366\n")
    (root / "trophy.dat").write_bytes(b"OlpsTrpD" + bytes(100))
    (root / "lua/init.lua").write_text('dofile("/savedata0/lua/bundle.lua")\n')
    (root / "lua/bundle.lua").write_text('auto_luafile = "/savedata0/lua/auto.lua"\n')
    (root / "sce_sys/param.sfo").write_bytes(kw.get("sfo", save_sfo()))
    (root / "sce_sys/keystone").write_bytes(b"keystone" + b"E" * 88)
    (root / "sce_sys/icon0.png").write_bytes(b"\x89PNG")
    return base


def entry(data: bytes, key: str) -> bytes:
    _, off, ln, _ = S.sfo_entries(data)[key]
    return data[off:off + ln]


class MakeSave(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)
        self.src = fake_save(self.base / "src")
        self.added = {"lua/auto.lua": b"-- auto\n", "ghostldr/ghostldr.lua": b"-- ghostldr\n",
                      "ghostldr/ghostldr-ext.bin": b"GLDRBNDL" + bytes(3 << 20)}
        self.us_key = b"keystone" + b"U" * 88

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def built(self, region: str = "eu", key: bytes | None = None) -> dict[str, bytes]:
        return S.build(S.read_save(self.src), region, key, self.added)

    def test_ghostldr_goes_in_beside_luac0re(self) -> None:
        files = self.built()
        for n, d in self.added.items():
            self.assertEqual(files[n], d)
        for n in ("VMC0.card", "VMC1.card", "lua/init.lua", "lua/bundle.lua", "trophy.dat", "sce_sys/keystone"):
            self.assertIn(n, files)
        self.assertEqual(files["sce_sys/keystone"], b"keystone" + b"E" * 88)

    def test_param_sfo_is_anyones(self) -> None:
        p = self.built()["sce_sys/param.sfo"]
        self.assertEqual(entry(p, "ACCOUNT_ID"), bytes(8))
        params = entry(p, "PARAMS")
        self.assertEqual(params[4:0x28], bytes(0x24))                    # user ID and the console's PSID HMAC
        self.assertEqual(params[0x60:0x68], bytes(8))
        self.assertEqual(params[0x2C:0x35], b"CUSA03492")
        for secret in (ACCOUNT, USER, PSID_HMAC):
            self.assertNotIn(secret, p)
        self.assertEqual(entry(p, "MAINTITLE"), "Star Wars™: Racer Revenge™\0".encode())
        self.assertEqual(entry(p, "SUBTITLE"), b"Luac0re + ghostldr autoloader\0")
        self.assertEqual(len(p), len(save_sfo()))

    def test_blocks_hold_the_files(self) -> None:
        files = self.built()
        blocks = struct.unpack("<Q", entry(files["sce_sys/param.sfo"], "SAVEDATA_BLOCKS"))[0]
        need = sum(-(-len(d) // S.BLOCK) or 1 for d in files.values())
        self.assertGreaterEqual(blocks, need + S.HEADROOM)
        self.assertEqual(blocks % 32, 0)

    def test_the_other_region(self) -> None:
        files = self.built("us", self.us_key)
        p = files["sce_sys/param.sfo"]
        self.assertEqual(entry(p, "TITLE_ID"), b"CUSA03474\0")
        self.assertEqual(entry(p, "SAVEDATA_DIRECTORY"), b"SLUS-20268\0")
        self.assertEqual(entry(p, "PARAMS")[0x2C:0x3C], b"CUSA03474".ljust(16, b"\0"))
        self.assertEqual(entry(p, "PARAMS")[0x3C:0x4C], b"CUSA03474".ljust(16, b"\0"))
        self.assertEqual(files["sce_sys/keystone"], self.us_key)
        self.assertIn(b"--ps2-title-id=SLUS-20268", files["disc-swap-cli.conf"])

    def test_the_other_region_needs_its_keystone(self) -> None:
        with self.assertRaises(SystemExit) as cm:
            self.built("us")
        self.assertIn("--keystone-us", str(cm.exception))
        with self.assertRaises(SystemExit):
            self.built("us", b"not a keystone")

    def test_vmc0_needs_the_regions_exploit_save(self) -> None:
        card = self.src / "PS4/APOLLO/1a2b3c4d_CUSA03492_SLES-50366/VMC0.card"
        card.write_bytes(card.read_bytes().replace(b"BASLUS-20268", b"BAXXXX-00000"))
        self.built()
        with self.assertRaises(SystemExit) as cm:
            self.built("us", self.us_key)
        self.assertIn("BASLUS-20268", str(cm.exception))

    def test_luac0re_must_run_auto_lua(self) -> None:
        (self.src / "PS4/APOLLO/1a2b3c4d_CUSA03492_SLES-50366/lua/bundle.lua").write_text("remote_lua_loader(9026)\n")
        with self.assertRaises(SystemExit) as cm:
            self.built()
        self.assertIn("auto.lua", str(cm.exception))

    def test_a_save_that_is_not_swrrs(self) -> None:
        (self.src / "PS4/APOLLO/1a2b3c4d_CUSA03492_SLES-50366/sce_sys/param.sfo").write_bytes(
            save_sfo("CUSA07992", "SCUS-97429"))
        with self.assertRaises(SystemExit) as cm:
            self.built()
        self.assertIn("CUSA07992", str(cm.exception))

    def test_an_apollo_zip_reads_as_its_folder(self) -> None:
        z = self.base / "export.zip"
        root = self.src / "PS4/APOLLO/1a2b3c4d_CUSA03492_SLES-50366"
        with zipfile.ZipFile(z, "w") as f:
            for p in sorted(root.rglob("*")):
                if p.is_file():
                    f.write(p, "SLES-50366/" + p.relative_to(root).as_posix())
        self.assertEqual(S.read_save(z), S.read_save(self.src))

    def test_apollo_usb_layout(self) -> None:
        names = list(S.apollo_files(self.built(), "eu"))
        self.assertTrue(all(n.startswith("PS4/APOLLO/00000000_CUSA03492_SLES-50366/") for n in names), names)
        self.assertIn("PS4/APOLLO/00000000_CUSA03492_SLES-50366/lua/auto.lua", names)

    # ---- one zip per region: the decrypted save and the encrypted one

    def encrypted(self, region: str = "eu", account: str = S.NO_ONE, title: str | None = None) -> Path:
        """A zip as resign_save.py writes it: the image and its sealed key."""
        r = S.REGIONS[region]
        top = f"PS4/SAVEDATA/{account}/{title or r['title']}/{r['dir']}"
        z = self.base / f"enc-{region}-{account}.zip"
        with zipfile.ZipFile(z, "w") as f:
            f.writestr(top, bytes(26 * 1048576 // 64))       # 400 blocks of 32 KiB, header byte 0x18 clear
            f.writestr(top + ".bin", b"pfsSKKey" + bytes(88))
        return z

    def zips(self, **kw) -> dict[str, dict[str, bytes]]:
        added = self.base / "added"
        added.mkdir(exist_ok=True)
        paths = {}
        for n, d in self.added.items():
            (added / Path(n).name).write_bytes(d)
            paths[n] = added / Path(n).name
        old, S.ADDED = S.ADDED, paths
        try:
            out = S.save_zips(self.src, "v1.0.0", words=[], keystones={"us": self.us_key_file()}, **kw)
        finally:
            S.ADDED = old
        return {n: {i.filename: zipfile.ZipFile(io.BytesIO(d)).read(i) for i in zipfile.ZipFile(io.BytesIO(d)).infolist()}
                for n, d in out.items()}

    def us_key_file(self) -> Path:
        k = self.base / "us.keystone"
        k.write_bytes(self.us_key)
        return k

    def test_one_zip_per_region_with_both_saves(self) -> None:
        out = self.zips(encrypted={"us": self.encrypted("us"), "eu": self.encrypted("eu")})
        self.assertEqual(sorted(out), ["ghostldr-v1.0.0-save-EU-CUSA03492.zip", "ghostldr-v1.0.0-save-US-CUSA03474.zip"])
        eu = out["ghostldr-v1.0.0-save-EU-CUSA03492.zip"]
        self.assertIn("PS4/APOLLO/00000000_CUSA03492_SLES-50366/ghostldr/ghostldr-ext.bin", eu)
        self.assertEqual(eu["PS4/SAVEDATA/0000000000000000/CUSA03492/SLES-50366.bin"][:8], b"pfsSKKey")
        self.assertIn(b"Save Wizard", eu["README.txt"])
        self.assertIn(b"(CUSA03492)", eu["README.txt"])
        us = out["ghostldr-v1.0.0-save-US-CUSA03474.zip"]
        self.assertIn("PS4/SAVEDATA/0000000000000000/CUSA03474/SLUS-20268", us)
        self.assertFalse([n for n in us if "CUSA03492" in n or "SLES" in n])

    def test_without_an_encrypted_save_the_decrypted_one_alone(self) -> None:
        eu = self.zips(regions=("eu",))["ghostldr-v1.0.0-save-EU-CUSA03492.zip"]
        self.assertFalse([n for n in eu if n.startswith("PS4/SAVEDATA/")])
        self.assertNotIn(b"Save Wizard", eu["README.txt"])

    def test_an_encrypted_save_must_be_no_ones_and_this_regions(self) -> None:
        for bad in (self.encrypted("eu", account="1122334455667788"), self.encrypted("us")):
            with self.assertRaises(SystemExit) as cm:
                self.zips(regions=("eu",), encrypted={"eu": bad})
            self.assertIn("PS4/SAVEDATA/0000000000000000/CUSA03492/SLES-50366", str(cm.exception))

    def test_refuses_personal_data(self) -> None:
        (self.src / "PS4/APOLLO/1a2b3c4d_CUSA03492_SLES-50366/lua/init.lua").write_text("-- 192.168.7.77\n")
        with self.assertRaises(SystemExit) as cm:
            self.zips(regions=("eu",))
        self.assertIn("192.168.7.77", str(cm.exception))

    def test_beside_the_release(self) -> None:
        built, lua = self.base / "build", self.base / "lua"
        built.mkdir()
        lua.mkdir()
        for n in R.BUILT:
            (built / n).write_bytes(n.encode())
        (lua / R.MANAGER).write_text("-- manager\n")
        readme = self.base / "README.in"
        readme.write_text("ghostldr @VERSION@\n")
        saves = {f"ghostldr-v1.0.0-save-{r}.zip": r.encode() for r in ("US-CUSA03474", "EU-CUSA03492")}
        out = self.base / "out"
        R.make(out, "v1.0.0", R.collect("v1.0.0", built, lua, readme), [], saves)
        sums = dict(reversed(line.split("  ")) for line in (out / R.SUMS).read_text().splitlines())
        for n, d in saves.items():
            self.assertEqual(sums[n], hashlib.sha256(d).hexdigest())
            self.assertEqual((out / n).read_bytes(), d)
        with zipfile.ZipFile(out / "ghostldr-v1.0.0.zip") as z:
            self.assertFalse([n for n in z.namelist() if "save" in n])        # the flat zip stays the five


if __name__ == "__main__":
    unittest.main()
