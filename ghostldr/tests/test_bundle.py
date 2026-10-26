"""The two-file boot offline (docs/architecture.md, "Two-file boot").  Needs
`make -C ghostldr/native` first.

- the files tools/mkbundle.py makes: each bundle's header against its parts,
  one build ID in all three, the pins in ghostldr.lua, its size under
  Luac0re's cap, its defaults (run.py boot's), and nothing of this computer;
- ext-elfldr.bin: no NID table, and its families are the full extension's
  that a payload binds to, entry for entry;
- the core's log.c on the bundle's connection (native_host/log_host.c);
- ghostldr.lua's listen mode under a fake Luac0re (boot_fake.lua, needs a
  desktop Lua 5.3/5.4): the boot context it hands the core, the ports, every
  refusal, the timeout, and what it gives back.

The core taking the bundle from memory is in test_ext.py (ext.c) and
test_selector.py (sel.c).

    python3 -m unittest test_bundle        (from ghostldr/tests)
"""

from __future__ import annotations

import getpass
import hashlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build/ghostldr"
sys.path.insert(0, str(ROOT / "ghostldr/host"))
sys.path.insert(0, str(ROOT / "ghostldr/tools"))
sys.path.insert(0, str(HERE))
import mkbundle  # noqa: E402
from test_manager import find_lua  # noqa: E402
from test_profiles import build_host  # noqa: E402

FILES = ("ghostldr.lua", "ghostldr-ext.bin", "ghostldr-elfldr.bin")
BUILT = all((OUT / f).exists() for f in FILES + ("ext.bin", "selector.bin", "ext-elfldr.bin"))
NEED_BUILD = "build the two-file boot first (make -C ghostldr/native)"
# boot_ctx (ghost.h) as ghostldr.lua lays it out: 45 fields, then reserved[8]
LOG_FD, LOG_ADDR, FLAGS, APP_ROOT = 0x38, 0x40, 0x160, 0x158
EXT_FD, EXT_SIZE, EXT_CRC, SEL_FD, SEL_SIZE, SEL_CRC, EXT_PIN = (0x1A8, 0x1B0, 0x1B8, 0x1C0, 0x1C8, 0x1D0, 0x1E0)
CTX_FLAG_ELFLDR, CTX_FLAG_BUNDLE = 1 << 20, 1 << 21


def lua_pins(text: str) -> dict[int, dict]:
    """GHOSTLDR_BUNDLES from ghostldr.lua, by kind."""
    pins = {}
    for m in re.finditer(r"\{kind = (\d+), (.*?)\}", text, re.S):
        fields = dict(re.findall(r"(\w+) = (\"[^\"]*\"|0x[0-9a-f]+|\d+)", m[2]))
        pins[int(m[1])] = {k: v.strip('"') if v.startswith('"') else int(v, 0) for k, v in fields.items()}
    return pins


def lua_build(path: Path) -> str:
    return re.search(r'^GHOSTLDR_BUILD = "(\w+)"', path.read_text(), re.M)[1]


@unittest.skipUnless(BUILT, NEED_BUILD)
class BundleFiles(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.lua = (OUT / "ghostldr.lua").read_text()
        cls.full = (OUT / "ghostldr-ext.bin").read_bytes()
        cls.slim = (OUT / "ghostldr-elfldr.bin").read_bytes()
        cls.parts = {n: (OUT / n).read_bytes() for n in ("core.bin", "ext.bin", "selector.bin", "ext-elfldr.bin")}

    def check_bundle(self, data: bytes, kind: int, ext: bytes, sel: bytes) -> dict:
        h = mkbundle.parse_header(data)
        self.assertEqual((h["magic"], h["format"], h["header_size"], h["kind"], h["flags"]),
                         (b"GLDRBNDL", 1, 128, kind, 0))
        self.assertEqual((h["ext_size"], h["sel_size"]), (len(ext), len(sel)))
        self.assertEqual((h["ext_crc"], h["sel_crc"]), (zlib.crc32(ext), zlib.crc32(sel)))
        self.assertEqual(h["ext_sha256"], hashlib.sha256(ext).hexdigest())
        self.assertEqual(h["sel_sha256"], hashlib.sha256(sel).hexdigest() if sel else "00" * 32)
        self.assertEqual(data[128:], ext + sel)
        self.assertEqual(data[120:128], bytes(8))    # reserved
        return h

    def test_full_bundle_is_the_extension_then_the_selector(self) -> None:
        self.check_bundle(self.full, 1, self.parts["ext.bin"], self.parts["selector.bin"])

    def test_elfldr_bundle_is_its_extension_alone(self) -> None:
        self.check_bundle(self.slim, 2, self.parts["ext-elfldr.bin"], b"")
        self.assertLess(len(self.slim), len(self.full) // 20)

    def test_one_build_id_and_the_pins_match(self) -> None:
        build = mkbundle.build_id(self.parts["core.bin"], [self.parts[n] for n in ("ext.bin", "selector.bin",
                                                                                      "ext-elfldr.bin")]).hex()
        self.assertEqual({mkbundle.parse_header(b)["build"] for b in (self.full, self.slim)}, {build})
        self.assertIn(f'GHOSTLDR_BUILD = "{build}"', self.lua)
        self.assertTrue(self.lua.startswith(f"-- ghostldr.lua, build {build}:"))
        pins = lua_pins(self.lua)
        for kind, data in ((1, self.full), (2, self.slim)):
            h, p = mkbundle.parse_header(data), pins[kind]
            self.assertEqual(p["file"], mkbundle.FILES[kind])
            for k in ("ext_size", "sel_size", "ext_crc", "sel_crc", "ext_sha256", "sel_sha256"):
                self.assertEqual(p[k], h[k], k)

    def test_lua_carries_the_core_and_fits_luac0re(self) -> None:
        import json
        import run
        self.assertIn(f'GHOSTLDR_CORE_HEX = "{self.parts["core.bin"].hex()}"', self.lua)
        layout = json.loads((OUT / "core.json").read_text())
        self.assertIn(f"GHOSTLDR_IMAGE_SIZE = {layout['__image_end']:#x}", self.lua)
        self.assertLess(len(self.lua.encode()), run.LUA_MAX)
        self.assertIn('GHOSTLDR_MODE = "listen"', self.lua)
        self.assertTrue(self.lua.endswith(run.BOOT_LUA.read_text()))

    def test_defaults_are_run_py_boots(self) -> None:
        import profiles
        import run
        flags = run.boot_flags(run.build_parser().parse_args(["boot"]))
        self.assertEqual(flags, 2 | 4096 | 1212 << 48)    # release SWRR's buffers, built-in font, FTP 1212
        self.assertIn(f"GHOSTLDR_FLAGS = {flags:#x}", self.lua)
        self.assertIn('GHOSTLDR_APP_ROOT = "auto"', self.lua)
        self.assertIn(f'GHOSTLDR_PROFILE_HEX = "{profiles.release_bundle().hex()}"', self.lua)
        titles = [b["title"] for b in profiles.read_blobs(profiles.release_bundle())]
        self.assertEqual(titles, profiles.known_titles() + [""])    # the repository's profiles and the default
        # an earlier run.py boot in the same Lua state must not leak in
        self.assertIn("GHOSTLDR_LOG_IP, GHOSTLDR_LOG_PORT = nil, nil", self.lua)

    def test_nothing_of_this_computer(self) -> None:
        home, user = str(Path.home()).encode(), getpass.getuser().encode()
        ip = re.compile(rb"\b(?:10\.\d{1,3}|192\.168|172\.(?:1[6-9]|2\d|3[01]))\.\d{1,3}\.\d{1,3}\b")
        for name in FILES:
            data = (OUT / name).read_bytes()
            self.assertNotIn(home, data, name)
            if len(user) >= 5:
                self.assertNotIn(user, data, name)
            self.assertEqual(ip.findall(data), [], name)

    def test_mismatched_parts_are_not_bundled(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            for n in ("core.bin", "core.json", "ext.bin", "selector.bin", "ext-elfldr.bin"):
                shutil.copy(OUT / n, Path(td) / n)
            sel = bytearray((Path(td) / "selector.bin").read_bytes())
            sel[20] ^= 1                                      # another build's ABI
            (Path(td) / "selector.bin").write_bytes(sel)
            with self.assertRaises(SystemExit) as e:
                mkbundle.make(Path(td))
            self.assertIn("different builds", str(e.exception))


@unittest.skipUnless(BUILT, NEED_BUILD)
class ElfldrExtension(unittest.TestCase):
    """ext-elfldr.bin against ext.bin, decoded as the core reads them."""

    def test_no_nid_table(self) -> None:
        import check_ext
        b = check_ext.Blob((OUT / "ext-elfldr.bin").read_bytes())
        self.assertEqual((b.nid_count, b.str_size), (0, 1))
        self.assertLess(b.image_memsz, 160 << 10)          # the full one needs 432 KiB of executable memory

    def test_payload_families_only_and_unchanged(self) -> None:
        """The families a payload binds to, as in the full extension (the same
        entries: an import binds alike); none of a game's."""
        import check_ext
        full = {f.name: f for f in check_ext.Blob((OUT / "ext.bin").read_bytes()).families()}
        slim = {f.name: f for f in check_ext.Blob((OUT / "ext-elfldr.bin").read_bytes()).families()}
        self.assertEqual(set(slim), {"selftest", "kernel", "module", "file", "homebrew", "tsd", "libc_heap",
                                     "libc_stdio"})
        for name, fam in slim.items():
            self.assertEqual([(e.lib, e.name, e.nid) for e in fam.hle],
                             [(e.lib, e.name, e.nid) for e in full[name].hle], name)
            self.assertEqual(fam.libs, full[name].libs, name)
        for game in ("savedata", "trophy", "np", "dialog", "appcontent", "playgo", "media", "system", "sysmodule"):
            self.assertIn(game, full)
            self.assertNotIn(game, slim)


class CoreLog(unittest.TestCase):
    def test_log_on_the_bundles_connection(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            exe = Path(td) / "log_host"
            build_host("log_host", [HERE / "native_host/log_host.c", ROOT / "ghostldr/native/src/log.c"], exe)
            p = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertNotIn("FAILED", p.stdout)
        self.assertEqual(p.stdout.count("\nok ") + p.stdout.startswith("ok "), 15, p.stdout)


@unittest.skipUnless(BUILT, NEED_BUILD)
@unittest.skipUnless(find_lua(), "needs a desktop Lua 5.3 or 5.4")
class ListenMode(unittest.TestCase):
    """The release's ghostldr.lua under boot_fake.lua's fake network."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        cls.full, cls.slim = OUT / "ghostldr-ext.bin", OUT / "ghostldr-elfldr.bin"
        other = bytearray(cls.full.read_bytes())
        other[112] ^= 0x10                                      # another build's ID
        cls.other = d / "other-ext.bin"
        cls.other.write_bytes(bytes(other))
        newer = bytearray(cls.slim.read_bytes())
        struct.pack_into("<I", newer, 8, 2)                     # bundle format 2
        cls.newer = d / "newer.bin"
        cls.newer.write_bytes(bytes(newer))
        cls.elf = d / "hello.elf"
        cls.elf.write_bytes(b"\x7fELF\x02\x01\x01" + bytes(300))
        cls.text = d / "quit.txt"
        cls.text.write_bytes(b"quit\n")
        cls.lua = OUT / "ghostldr.lua"

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_fake(self, *opts: str, platform: str = "PS5", fw: str = "13.60") -> dict:
        p = subprocess.run([find_lua(), str(HERE / "boot_fake.lua"), str(self.lua), platform, fw, *opts],
                           capture_output=True, text=True, timeout=300)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        out = p.stdout
        self.assertNotIn("FAULT", out)
        ctx = {int(o, 16): int(v, 16) & (2**64 - 1) for o, v in re.findall(r"^ctx (\w+) (-?\w+)$", out, re.M)}
        sent = re.findall(r"^sent (\d+) (.*)$", out, re.M)
        return {"out": out, "ctx": ctx, "sent": [line for _, line in sent], "called": "core-call" in out,
                "binds": re.findall(r"^bind \d+ (\d+) (-?\d+)$", out, re.M),
                "maps": dict(re.findall(r"^map (\w+) (\w+)$", out, re.M)),
                "unmaps": dict(re.findall(r"^unmap (\w+) (\w+)$", out, re.M)),
                "leaks": re.findall(r"^leak ", out, re.M) + re.findall(r"^jit-leak ", out, re.M),
                "jit": re.findall(r"^jit-socket (\d+)$", out, re.M),
                "closes": re.findall(r"^close (\d+)$", out, re.M),
                "accepts": re.findall(r"^accept (\d+)$", out, re.M),
                "notes": re.findall(r"^notification: (.*)$", out, re.M),
                "tv": re.findall(r"^dialog: (.*)$", out, re.M),
                "clock": int(re.search(r"^clock (\d+)$", out, re.M)[1])}

    def test_full_bundle_boots_with_the_log_on_its_connection(self) -> None:
        r = self.run_fake(f"client={self.full}")
        c, pin = r["ctx"], lua_pins(self.lua.read_text())[1]
        self.assertEqual(r["binds"], [("9021", "0")])
        self.assertTrue(r["called"])
        conn = int(r["accepts"][0])
        self.assertEqual((c[LOG_FD], c[LOG_ADDR]), (conn, 0))     # log.c: the connection, no address
        self.assertEqual(c[FLAGS], 2 | 4096 | 1212 << 48 | CTX_FLAG_BUNDLE)
        maps = {int(a, 16): int(s, 16) for a, s in r["maps"].items()}
        self.assertEqual(maps[c[EXT_FD]], (pin["ext_size"] + 0x3FFF) & ~0x3FFF)
        self.assertEqual(maps[c[SEL_FD]], (pin["sel_size"] + 0x3FFF) & ~0x3FFF)
        self.assertEqual((c[EXT_SIZE], c[EXT_CRC], c[SEL_SIZE], c[SEL_CRC]),
                         (pin["ext_size"], pin["ext_crc"], pin["sel_size"], pin["sel_crc"]))
        self.assertEqual(c[EXT_PIN].to_bytes(8, "little"), bytes.fromhex(pin["ext_sha256"][:16]))
        self.assertTrue(any("9021" in n and "ghostldr-ext.bin" in n for n in r["notes"]))
        self.assertIn("received ghostldr-ext.bin", r["sent"][0])
        self.assertIn("the game runs", r["sent"][-1])
        self.assertNotIn(str(conn), r["closes"])                  # the game's log keeps it
        self.assertEqual((r["unmaps"], r["leaks"]), ({}, []))      # the core took both

    def test_elfldr_bundle_and_the_ports_when_9021_is_taken(self) -> None:
        r = self.run_fake("taken=9021", f"client={self.slim}")
        self.assertEqual(r["binds"], [("9021", "-1"), ("9022", "0"), ("9023", "0")])
        self.assertTrue(any("9022 (9021 is taken)" in n for n in r["notes"]))
        c = r["ctx"]
        self.assertEqual(c[FLAGS], CTX_FLAG_ELFLDR | 9023 << 32 | CTX_FLAG_BUNDLE)
        self.assertEqual(c[SEL_FD], 2**64 - 1)
        self.assertIn("starting the ELF loader on TCP 9023: send payloads with nc 192.0.2.10 9023", r["out"])
        # quit: back to Luac0re, the connection closed and taken from the core
        self.assertIn("back to Luac0re's loader", r["sent"][-1])
        self.assertIn(r["accepts"][0], r["closes"])
        self.assertEqual(r["leaks"], [])

    def test_the_tv_says_what_to_send_and_what_happened(self) -> None:
        """Luac0re's dialog shows the prompt with the console's address and
        port, then each outcome: a refusal with what was expected (the prompt
        again under it), what was received, the ELF loader's port, and the way
        back to Luac0re."""
        r = self.run_fake("taken=9021", f"client={self.other}", f"client={self.slim}")
        tv = r["tv"]
        self.assertIn("Send ghostldr-ext.bin (games) | or ghostldr-elfldr.bin (ELF loader) | to 192.0.2.10:9022 "
                      "(9021 is taken)", tv[0])
        self.assertTrue(tv[0].startswith("ghostldr " + lua_build(self.lua)))
        self.assertIn("Refused: ghostldr-ext.bin is from build", tv[1])
        self.assertIn("this ghostldr.lua expects build " + lua_build(self.lua), tv[1])
        self.assertIn("to 192.0.2.10:9022", tv[1])                    # the prompt again
        self.assertIn("Received ghostldr-elfldr.bin. |  | ELF loader: send payloads to 192.0.2.10:9023", tv[2])
        self.assertIn("The ELF loader quit. | Back to Luac0re's loader. |  | Luac0re 2.4b by Gezine | "
                      "Remote Lua Loader | Platform: PS5 | FW: 13.60 | Title ID: CUSA03492 | "
                      "Listening: 192.0.2.10:9026", tv[3])
        # each outcome is a debug line on the connection and a notification too
        self.assertTrue(any("refused: ghostldr-ext.bin is from another build" in s for s in r["sent"]))
        self.assertTrue(any("starting the ELF loader on TCP 9023" in n for n in r["notes"]))

    def test_the_tv_on_timeout_wrong_files_and_a_ps4(self) -> None:
        r = self.run_fake()
        self.assertIn("Timed out: nothing came on port 9021 in 5 minutes. | Back to Luac0re's loader: send "
                      "ghostldr.lua again to retry.", r["tv"][-1])
        self.assertIn("Listening: 192.0.2.10:9026", r["tv"][-1])
        r = self.run_fake(f"client={self.elf}", f"client={self.full}")
        self.assertIn("Refused: an ELF from 192.0.2.7. | Expected ghostldr-ext.bin or ghostldr-elfldr.bin. | "
                      "ELF payloads go to the ELF loader", r["tv"][1])
        self.assertIn("Received ghostldr-ext.bin. |  | Booting", r["tv"][2])
        r = self.run_fake(f"client={self.slim}", f"client={self.full}", platform="PS4", fw="12.02")
        self.assertIn("Refused: ghostldr-elfldr.bin: the ELF loader runs on a PS5 only. | Send ghostldr-ext.bin "
                      "to boot a game", r["tv"][1])
        r = self.run_fake(f"client={self.full}", "core=-7", "core-takes=none")
        self.assertIn("The boot stopped. | Back to Luac0re's loader.", r["tv"][-1])

    def test_no_socket_left_in_the_compiler(self) -> None:
        """A PS5 makes sockets in the JIT compiler (Luac0re's create_socket):
        its copy is closed, or a port would stay taken after SWRR closes its
        own (as the first console run showed: 9022 kept listening)."""
        r = self.run_fake("taken=9021", f"client={self.slim}")
        self.assertEqual(len(r["jit"]), 3)                        # 9021, 9022, the ELF loader's 9023
        self.assertEqual(r["leaks"], [])
        r = self.run_fake()                                       # the timeout
        self.assertEqual((len(r["jit"]), r["leaks"]), (1, []))
        r = self.run_fake(f"client={self.slim}", platform="PS4", fw="12.02")   # SWRR's own socket()
        self.assertEqual(r["jit"], [])

    def test_stock_console_ports(self) -> None:
        r = self.run_fake(f"client={self.slim}")
        self.assertEqual(r["binds"], [("9021", "0"), ("9022", "0")])
        self.assertEqual(r["ctx"][FLAGS] >> 32 & 0xFFFF, 9022)

    def test_both_ports_taken(self) -> None:
        p = subprocess.run([find_lua(), str(HERE / "boot_fake.lua"), str(self.lua), "PS5", "13.60", "taken=9021",
                            "taken=9022"], capture_output=True, text=True, timeout=60)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("no port for the bundle: TCP 9021", p.stdout + p.stderr)
        self.assertNotIn("core-call", p.stdout)

    def test_refusals_then_the_right_bundle(self) -> None:
        """Each wrong thing is refused with the reason on its connection and
        closed; the listener stays for the next."""
        r = self.run_fake(f"client={self.other}", f"client={self.elf}", f"client={self.text}",
                          f"client={self.newer}", f"client={self.lua}", f"client={self.full}")
        refused = [s for s in r["sent"] if "refused" in s]
        self.assertEqual(len(refused), 5)
        self.assertIn("ghostldr-ext.bin is from another build", refused[0])
        self.assertIn("send ghostldr.lua and ghostldr-ext.bin from the same release", refused[0])
        self.assertIn("an ELF (it starts 7f454c46", refused[1])
        self.assertIn("not a ghostldr bundle (it starts 717569740a)", refused[2])
        self.assertIn("bundle format 2", refused[3])
        self.assertIn("a Lua script", refused[4])
        self.assertTrue(r["called"])
        self.assertEqual(r["closes"][:5], r["accepts"][:5])
        self.assertEqual(r["leaks"], [])
        self.assertTrue(sum("refused" in n for n in r["notes"]) == 5)

    def test_cut_and_stalled_transfers(self) -> None:
        r = self.run_fake(f"client={self.full}", "cut=1000000")
        self.assertIn(f"1000000 of ghostldr-ext.bin's {self.full.stat().st_size} bytes came (the connection ended)",
                      r["out"])
        self.assertFalse(r["called"])
        self.assertEqual(len(r["unmaps"]), 2)                     # both parts' memory back
        self.assertEqual(r["leaks"], [])
        r = self.run_fake(f"client={self.full}", "cut=60", "stall")
        self.assertIn("the connection ended after 60 bytes, inside the bundle's header", r["out"])
        r = self.run_fake(f"client={self.full}", "cut=0", "stall")
        self.assertIn("refused: nothing came for 30 seconds", r["out"])

    def test_nothing_comes_back_to_luac0re(self) -> None:
        """5 minutes with nothing: the listener closed, nothing of SWRR touched."""
        r = self.run_fake()
        self.assertFalse(r["called"])
        self.assertEqual(r["clock"], 300)
        self.assertEqual(r["closes"], [str(8)])                  # the listener
        self.assertEqual((r["maps"], r["leaks"]), ({}, []))
        self.assertIn("nothing came on TCP 9021 in 5 minutes", r["notes"][-1])
        self.assertNotIn("data map", r["out"])

    def test_elfldr_refused_on_a_ps4(self) -> None:
        r = self.run_fake(f"client={self.slim}", f"client={self.full}", platform="PS4", fw="12.02")
        self.assertIn("the ELF loader runs on a PS5", r["sent"][0])
        self.assertTrue(r["called"])
        self.assertEqual(r["ctx"][FLAGS], 2 | 4096 | 1212 << 48 | CTX_FLAG_BUNDLE | 8192)

    def test_no_memory(self) -> None:
        r = self.run_fake(f"client={self.full}", "noflex")
        self.assertIn("refused: no memory for its", r["out"])
        self.assertFalse(r["called"])

    def test_a_boot_that_stops_gives_everything_back(self) -> None:
        """The core returned before taking the bundle: ghostldr.lua unmaps both
        parts and closes the connection, after taking it from the core."""
        r = self.run_fake(f"client={self.full}", "core=-7", "core-takes=none")
        self.assertEqual(sorted(r["unmaps"]), sorted(r["maps"]))
        self.assertEqual(r["leaks"], [])
        self.assertIn("the boot stopped: back to Luac0re's loader on TCP 9026 (the core returned -7;", r["sent"][-1])
        self.assertIn(r["accepts"][0], r["closes"])

    def test_sender_gone_is_not_closed_twice(self) -> None:
        r = self.run_fake(f"client={self.slim}", "sender-gone")
        conn = r["accepts"][0]
        self.assertNotIn(conn, r["closes"])                       # log.c closed it already
        self.assertFalse(any("back to Luac0re's loader on TCP 9026" in s for s in r["sent"]))

    def test_connect_mode_is_unchanged(self) -> None:
        """run.py's payload: no listener, the UDP log and the extension's socket as before."""
        import run
        payload = Path(self.tmp.name) / "connect.lua"
        payload.write_bytes(run.build_payload(2, "192.0.2.1", "ghostfs", {}, b"x" * 100))
        p = subprocess.run([find_lua(), str(HERE / "boot_fake.lua"), str(payload), "PS5", "13.60"],
                           capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertNotIn("bind ", p.stdout)
        ctx = {int(o, 16): int(v, 16) for o, v in re.findall(r"^ctx (\w+) (-?\w+)$", p.stdout, re.M)}
        self.assertNotEqual(ctx[LOG_ADDR], 0)
        self.assertEqual(ctx[FLAGS] & CTX_FLAG_BUNDLE, 0)
        self.assertEqual(ctx[EXT_SIZE], 100)


if __name__ == "__main__":
    unittest.main()
