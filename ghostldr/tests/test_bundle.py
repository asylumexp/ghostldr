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
  refusal, the timeout, and what it gives back;
- the bundle cache (docs/architecture.md, "The bundle cache") under the same
  fake, with save containers in a directory, the TV's dialog dismissed at a
  given second, an instant clock, sockets in SWRR and in the JIT compiler,
  and a scripted core and ELF loader: the first boot caches, a later one
  starts the ELF loader on 9021 with a countdown, its results (boot, a
  bundle, forget, quit), another release's cache, damaged ones, a full
  disk, clearing it, and a PS4's countdown.

The core taking the bundle from memory is in test_ext.py (ext.c) and
test_selector.py (sel.c).

    python3 -m unittest test_bundle        (from ghostldr/tests)
"""

from __future__ import annotations

import base64
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
SIZE, CTX_IP, CTX_HEADER = 0x8, 0x200, 0x208          # listen mode: two slots after the profile slot
CTX_FLAG_ELFLDR, CTX_FLAG_BUNDLE, CAN_BOOT = 1 << 20, 1 << 21, 1 << 56
BOOT_FLAGS = 2 | 4096 | 1212 << 48                     # run.py boot's (the release's defaults)
NONE = 2**64 - 1


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
        self.assertIn(f'GHOSTLDR_CORE_B64 = "{base64.b64encode(self.parts["core.bin"]).decode()}"', self.lua)
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
        self.assertIn(f'GHOSTLDR_PROFILE_B64 = "{base64.b64encode(profiles.release_bundle()).decode()}"', self.lua)
        titles = [b["title"] for b in profiles.read_blobs(profiles.release_bundle())]
        self.assertEqual(titles, profiles.known_titles() + [""])    # the repository's profiles and the default
        # an earlier run.py boot in the same Lua state must not leak in
        self.assertIn("GHOSTLDR_LOG_IP, GHOSTLDR_LOG_PORT = nil, nil", self.lua)
        self.assertIn("GHOSTLDR_CORE_HEX, GHOSTLDR_PROFILE_HEX = nil, nil", self.lua)

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
class FakeConsole(unittest.TestCase):
    """The release's ghostldr.lua under boot_fake.lua (its fake network, saved data, TV and core)."""

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

    def run_fake(self, *opts: str, platform: str = "PS5", fw: str = "13.60", payload: Path | None = None) -> dict:
        p = subprocess.run([find_lua(), str(HERE / "boot_fake.lua"), str(payload or self.lua), platform, fw, *opts],
                           capture_output=True, text=True, timeout=300)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        out = p.stdout
        self.assertNotIn("FAULT", out)
        calls = out.split("core-call\n")
        ctxs = [{int(o, 16): int(v, 16) & (2**64 - 1) for o, v in re.findall(r"^ctx (\w+) (-?\w+)$", c, re.M)}
                for c in calls[1:]]
        sent = re.findall(r"^sent (\d+) (.*)$", out, re.M)
        return {"out": out, "ctx": ctxs[-1] if ctxs else {}, "ctxs": ctxs, "sent": [line for _, line in sent],
                "sent_to": sent, "called": "core-call" in out,
                "binds": re.findall(r"^bind \d+ (\d+) (-?\d+)$", out, re.M),
                "maps": dict(re.findall(r"^map (\w+) (\w+)$", out, re.M)),
                "unmaps": dict(re.findall(r"^unmap (\w+) (\w+)$", out, re.M)),
                "leaks": re.findall(r"^(?:jit-|listen-|save-|file-)?leak .*$", out, re.M),
                "saves": re.findall(r"^save-(mount|umount|delete) (.*)$", out, re.M),
                "elfldr": re.findall(r"^elfldr (?:listening|port) .*$|^elfldr-bind .*$", out, re.M),
                "jit": re.findall(r"^jit-socket (\d+)$", out, re.M),
                "closes": re.findall(r"^close (\d+)$", out, re.M),
                "accepts": re.findall(r"^accept (\d+)$", out, re.M),
                "notes": re.findall(r"^notification: (.*)$", out, re.M),
                "tv": re.findall(r"^dialog: (.*)$", out, re.M),
                "clock": int(re.search(r"^clock (\d+)$", out, re.M)[1])}


class ListenMode(FakeConsole):
    """Bundles sent to the port: the boot context, the ports, every refusal, the timeout, what is given back."""

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
        """The ELF loader listens on the port the bundle came to (9022 here),
        which ghostldr.lua gave up in both processes first."""
        r = self.run_fake("taken=9021", f"client={self.slim}")
        self.assertEqual(r["binds"], [("9021", "-1"), ("9022", "0")])
        self.assertTrue(any("9022 (9021 is taken)" in n for n in r["notes"]))
        c = r["ctx"]
        self.assertEqual(c[FLAGS], CTX_FLAG_ELFLDR | 9022 << 32 | CTX_FLAG_BUNDLE)
        self.assertEqual(c[SEL_FD], NONE)
        self.assertEqual(r["elfldr"], ["elfldr port 9022 countdown 0 can-boot 0", "elfldr listening 9022"])
        self.assertIn("starting the ELF loader on TCP 9022: send payloads with nc 192.0.2.10 9022", r["out"])
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
        self.assertIn("Refused: Version mismatch: ghostldr-ext.bin is from build", tv[1])
        self.assertIn("this ghostldr.lua expects build " + lua_build(self.lua), tv[1])
        self.assertIn("to 192.0.2.10:9022", tv[1])                    # the prompt again
        self.assertIn("Received ghostldr-elfldr.bin. |  | ELF loader: send payloads to 192.0.2.10:9022", tv[2])
        self.assertIn("The ELF loader quit. | Back to Luac0re's loader. |  | Luac0re 2.4b by Gezine | "
                      "Remote Lua Loader | Platform: PS5 | FW: 13.60 | Title ID: CUSA03492 | "
                      "Listening: 192.0.2.10:9026", tv[3])
        # each outcome is a debug line on the connection and a notification too
        self.assertTrue(any("refused: version mismatch: ghostldr-ext.bin is from build" in s for s in r["sent"]))
        self.assertTrue(any("starting the ELF loader on TCP 9022" in n for n in r["notes"]))

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
        self.assertEqual(len(r["jit"]), 2)                        # 9021 (taken), 9022
        self.assertEqual(r["leaks"], [])
        r = self.run_fake()                                       # the timeout
        self.assertEqual((len(r["jit"]), r["leaks"]), (1, []))
        r = self.run_fake(f"client={self.slim}", platform="PS4", fw="12.02")   # SWRR's own socket()
        self.assertEqual(r["jit"], [])

    def test_stock_console_ports(self) -> None:
        """9021 for the bundle, then the ELF loader on it: ghostldr.lua's
        listener is closed in SWRR and the compiler's copy went at once, so
        the ELF loader binds it (an accepted connection on it is no bar:
        SO_REUSEADDR)."""
        r = self.run_fake(f"client={self.slim}")
        self.assertEqual(r["binds"], [("9021", "0")])
        self.assertEqual(r["ctx"][FLAGS] >> 32 & 0xFFFF, 9021)
        out = r["out"]
        self.assertLess(out.index("jit-close 101"), out.index("bind 8 9021"))   # the compiler's copy, at once
        self.assertLess(out.index("close 8"), out.index("core-call"))            # SWRR's, before the core
        self.assertEqual(r["elfldr"], ["elfldr port 9021 countdown 0 can-boot 0", "elfldr listening 9021"])

    def test_a_port_still_held_is_caught(self) -> None:
        """The fake's check: a compiler copy left open (as Luac0re's own
        create_socket leaves it) keeps 9021, and the ELF loader can't have it."""
        r = self.run_fake(f"client={self.slim}", "jit-close-fails")
        self.assertIn("elfldr-bind 9021 held by compiler fd 101", r["elfldr"])
        self.assertIn("the ELF loader stopped: back to Luac0re's loader on TCP 9026 (the core returned -48", r["out"])

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
        self.assertIn("version mismatch: ghostldr-ext.bin is from build", refused[0])
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



@unittest.skipUnless(BUILT, NEED_BUILD)
@unittest.skipUnless(find_lua(), "needs a desktop Lua 5.3 or 5.4")
class BundleCache(FakeConsole):
    """The bundle cache: GLDRCACHE/bundle.bin in the host's saved data, under
    boot_fake.lua's save containers (a directory per container)."""

    def setUp(self) -> None:
        self.saves = Path(tempfile.mkdtemp(dir=self.tmp.name))

    def cache(self, data: bytes | None = None) -> Path:
        """GLDRCACHE holding data (as a first boot leaves it); the bundle file."""
        d = self.saves / "GLDRCACHE"
        d.mkdir(exist_ok=True)
        (d / ".blocks").write_text("512")
        (d / "bundle.bin").write_bytes(data if data is not None else self.full.read_bytes())
        return d / "bundle.bin"

    def cached(self) -> bytes | None:
        f = self.saves / "GLDRCACHE/bundle.bin"
        return f.read_bytes() if f.exists() else None

    def boot(self, *opts: str, **kw) -> dict:
        r = self.run_fake(f"saves={self.saves}", *opts, **kw)
        self.assertEqual(r["leaks"], [])                              # memory, sockets, mounts, files
        return r

    def pins(self) -> dict[int, dict]:
        return lua_pins(self.lua.read_text())

    def assert_ext_is(self, ctx: dict, kind: int) -> None:
        pin = self.pins()[kind]
        self.assertEqual((ctx[EXT_SIZE], ctx[EXT_CRC]), (pin["ext_size"], pin["ext_crc"]))
        self.assertEqual(ctx[EXT_PIN].to_bytes(8, "little"), bytes.fromhex(pin["ext_sha256"][:16]))

    # ---- the first boot

    def test_first_boot_caches_the_bundle(self) -> None:
        r = self.boot(f"client={self.full}")
        self.assertEqual(self.cached(), self.full.read_bytes())     # header and parts, as sent
        self.assertEqual((self.saves / "GLDRCACHE/.blocks").read_text(), "512")   # 16 MiB
        self.assertIn(("mount", "GLDRCACHE 6 512 0"), r["saves"])
        self.assertIn("ghostldr: cached ghostldr-ext.bin in this console's saved data (GLDRCACHE save container "
                      "16.0 MiB, 2.7 MiB): from now on send ghostldr.lua alone", r["sent"])
        self.assertEqual(r["ctx"][FLAGS], BOOT_FLAGS | CTX_FLAG_BUNDLE)
        self.assertIn("core-ext ok", r["out"])
        # the cache is written before the core has the bundle's memory
        self.assertLess(r["out"].index("save-umount GLDRCACHE"), r["out"].index("core-call"))

    def test_first_boot_with_the_elf_loader_bundle_caches_it(self) -> None:
        r = self.boot(f"client={self.slim}")
        self.assertEqual(self.cached(), self.slim.read_bytes())
        self.assertEqual(r["elfldr"], ["elfldr port 9021 countdown 0 can-boot 0", "elfldr listening 9021"])

    # ---- later boots: ghostldr.lua alone

    def test_later_boot_starts_the_elf_loader_on_9021_with_a_countdown(self) -> None:
        """The cached ghostldr-ext.bin: the ELF loader at once, on 9021 (taken
        and given back whole first), counting down 5 seconds with "boot"
        allowed; quit goes back to Luac0re and keeps the cache."""
        self.cache()
        r = self.boot("elfldr=quit")
        c = r["ctx"]
        self.assertEqual(c[FLAGS], CTX_FLAG_ELFLDR | 9021 << 32 | CTX_FLAG_BUNDLE | 5 << 48 | CAN_BOOT)
        self.assert_ext_is(c, 1)
        self.assertEqual((c[SEL_FD], c[SEL_SIZE], c[LOG_FD]), (NONE, 0, NONE))   # no selector, no connection yet
        self.assertEqual((c[SIZE], c[APP_ROOT] != 0), (0x210, True))
        self.assertNotEqual(c[CTX_IP], 0)
        self.assertNotEqual(c[CTX_HEADER], 0)
        self.assertEqual(r["binds"], [("9021", "0")])
        out = r["out"]                                              # 9021 given back in both processes first
        self.assertLess(out.index("jit-close 101"), out.index("bind 8 9021"))
        self.assertLess(out.index("close 8"), out.index("core-call"))
        self.assertEqual(r["elfldr"], ["elfldr port 9021 countdown 5 can-boot 1", "elfldr listening 9021"])
        self.assertEqual(r["accepts"], [])                          # nothing was waited for
        self.assertIn("ghostldr: the cached ghostldr-ext.bin (build " + lua_build(self.lua) + "): the ELF loader on "
                      "TCP 9021, booting games in 5 seconds unless cancelled on the TV", r["notes"])
        self.assertIn("The ELF loader quit. | Back to Luac0re's loader.", r["tv"][-1])
        self.assertEqual(self.cached(), self.full.read_bytes())
        self.assertEqual(r["clock"], 0)

    def test_countdown_runs_out_and_boots_the_cached_extension(self) -> None:
        """EL_BOOT (the countdown ran out, or "boot"): the cache is read again,
        extension and selector, and games boot as with a sent ghostldr-ext.bin."""
        self.cache()
        r = self.boot("elfldr=boot")
        self.assertEqual(len(r["ctxs"]), 2)
        c, pin = r["ctxs"][1], self.pins()[1]
        self.assertEqual(c[FLAGS], BOOT_FLAGS | CTX_FLAG_BUNDLE)
        self.assert_ext_is(c, 1)
        self.assertEqual((c[SEL_SIZE], c[SEL_CRC]), (pin["sel_size"], pin["sel_crc"]))
        maps = {int(a, 16): int(n, 16) for a, n in r["maps"].items()}
        self.assertEqual(maps[c[SEL_FD]], (pin["sel_size"] + 0x3FFF) & ~0x3FFF)
        self.assertEqual(r["unmaps"], {})                             # the core took all
        self.assertIn("Booting: a game on a USB drive in the console, | else an installed one", r["tv"][-1])

    def test_a_log_connection_follows_into_the_game(self) -> None:
        """"log" at the ELF loader, then boot: the game's log goes to that connection."""
        self.cache()
        r = self.boot("elfldr=log+boot")
        log = int(re.search(r"^elfldr-log (\d+)$", r["out"], re.M)[1])
        self.assertEqual(r["ctxs"][1][LOG_FD], log)
        self.assertIn((str(log), "ghostldr: the game runs; its log keeps coming here"), r["sent_to"])
        self.assertNotIn(str(log), r["closes"])

    def test_only_the_elf_loader_bundle_cached(self) -> None:
        """ghostldr-elfldr.bin alone: the ELF loader, no countdown, no "boot"
        (the TV says to send ghostldr-ext.bin there)."""
        self.cache(self.slim.read_bytes())
        r = self.boot("elfldr=quit")
        self.assertEqual(r["ctx"][FLAGS], CTX_FLAG_ELFLDR | 9021 << 32 | CTX_FLAG_BUNDLE)
        self.assert_ext_is(r["ctx"], 2)
        self.assertIn("send ghostldr-ext.bin there to boot games", r["notes"][0])

    def test_ghostldr_ext_bin_sent_to_the_elf_loader(self) -> None:
        """A bundle sent to the ELF loader's port (told apart by its magic):
        checked against the pins here, cached in place of the old one, and
        run: ghostldr-ext.bin boots games, its connection is the log."""
        self.cache(self.slim.read_bytes())
        r = self.boot(f"elfldr=bundle:{self.full}")
        conn = re.search(r"^elfldr-bundle (\d+)$", r["out"], re.M)[1]
        self.assertEqual(self.cached(), self.full.read_bytes())
        self.assertEqual(len(r["ctxs"]), 2)
        c = r["ctxs"][1]
        self.assertEqual((c[FLAGS], c[LOG_FD]), (BOOT_FLAGS | CTX_FLAG_BUNDLE, int(conn)))
        self.assert_ext_is(c, 1)
        mine = [line for fd, line in r["sent_to"] if fd == conn]
        self.assertEqual(mine[0], "ghostldr: received ghostldr-ext.bin at the ELF loader: booting games")
        self.assertIn("cached ghostldr-ext.bin in this console's saved data", mine[1])
        self.assertEqual(mine[-1], "ghostldr: the game runs; its log keeps coming here")

    def test_ghostldr_elfldr_bin_at_the_elf_loader_keeps_a_cached_ghostldr_ext_bin(self) -> None:
        """ghostldr-ext.bin's extension has the ELF loader too, so the cache
        keeps it; the ELF loader runs on from the one sent, "boot" still
        boots (from the cache)."""
        self.cache()
        r = self.boot(f"elfldr=bundle:{self.slim},boot")
        self.assertEqual(self.cached(), self.full.read_bytes())
        self.assertIn("ghostldr: not cached: the cache's ghostldr-ext.bin has the ELF loader too", r["sent"])
        c = r["ctxs"][1]
        self.assertEqual(c[FLAGS], CTX_FLAG_ELFLDR | 9021 << 32 | CTX_FLAG_BUNDLE | CAN_BOOT)   # no countdown again
        self.assert_ext_is(c, 2)
        self.assertEqual(r["ctxs"][2][FLAGS], BOOT_FLAGS | CTX_FLAG_BUNDLE)
        self.assert_ext_is(r["ctxs"][2], 1)

    def test_another_releases_bundle_at_the_elf_loader(self) -> None:
        """Refused with "version mismatch" on its connection and the TV, its
        memory back, and the ELF loader runs again from the cache."""
        self.cache()
        r = self.boot(f"elfldr=bundle:{self.other},quit")
        conn = re.search(r"^elfldr-bundle (\d+)$", r["out"], re.M)[1]
        mine = [line for fd, line in r["sent_to"] if fd == conn]
        self.assertTrue(mine[0].startswith("ghostldr: refused: version mismatch: ghostldr-ext.bin is from build"))
        self.assertIn(conn, r["closes"])
        self.assertTrue(any(t.startswith(f"ghostldr {lua_build(self.lua)} |  | Refused: Version mismatch")
                            for t in r["tv"]))
        self.assertEqual(len(r["ctxs"]), 2)
        self.assertEqual(r["ctxs"][1][FLAGS], CTX_FLAG_ELFLDR | 9021 << 32 | CTX_FLAG_BUNDLE | CAN_BOOT)
        self.assertEqual(self.cached(), self.full.read_bytes())
        self.assertEqual(len(r["unmaps"]), 2)                         # the refused bundle's two parts

    def test_forget_clears_the_cache_and_asks_again(self) -> None:
        self.cache()
        r = self.boot(f"client={self.slim}", "elfldr=forget,quit")
        self.assertIn(("delete", "GLDRCACHE 0"), r["saves"])
        self.assertIn("The bundle cache is cleared. |  | Send ghostldr-ext.bin (games)", "\n".join(r["tv"]))
        self.assertEqual(self.cached(), self.slim.read_bytes())       # what came next
        self.assertEqual(r["binds"], [("9021", "0"), ("9021", "0")])   # the ELF loader's port, then the prompt's

    # ---- another release's cache, damaged ones

    def test_another_releases_cache_is_refused_and_replaced(self) -> None:
        self.cache(self.other.read_bytes())
        r = self.boot(f"client={self.slim}")
        line = [n for n in r["notes"] if "version mismatch" in n][0]
        self.assertIn("version mismatch: the cached ghostldr-ext.bin is from build", line)
        self.assertIn("this ghostldr.lua is build " + lua_build(self.lua), line)
        self.assertTrue(r["tv"][0].startswith(f"ghostldr {lua_build(self.lua)} |  | Version mismatch: the cached "
                                              "ghostldr-ext.bin is from build"))
        self.assertIn("The bundle sent next replaces it. |  | Send ghostldr-ext.bin (games)", r["tv"][0])
        self.assertIn("version mismatch: the cached", r["sent"][0])     # said on the first connection too
        self.assertEqual(self.cached(), self.slim.read_bytes())
        self.assertEqual(r["elfldr"][0], "elfldr port 9021 countdown 0 can-boot 0")

    def test_another_releases_cache_stays_when_nothing_comes(self) -> None:
        self.cache(self.other.read_bytes())
        r = self.boot()
        self.assertFalse(r["called"])
        self.assertEqual(self.cached(), self.other.read_bytes())

    def damaged(self, data: bytes, *opts: str) -> dict:
        self.cache(data)
        r = self.boot(f"client={self.slim}", *opts)
        self.assertIn(("delete", "GLDRCACHE 0"), r["saves"])
        self.assertIn("The cached bundle is damaged: deleted. | Send the bundle again. |  | Send ghostldr-ext.bin",
                      "\n".join(r["tv"]))
        self.assertEqual(self.cached(), self.slim.read_bytes())       # replaced by what came
        return r

    def test_a_half_written_cache(self) -> None:
        full = self.full.read_bytes()
        r = self.damaged(full[:len(full) // 2])
        self.assertIn(f"the cached bundle is damaged (it holds {len(full) // 2} of the bundle's {len(full)} bytes)",
                      r["notes"][0])
        self.assertNotIn("elfldr port 9021 countdown 5", r["out"])      # never run

    def test_a_cache_with_no_header(self) -> None:
        r = self.damaged(b"GLDR")
        self.assertIn("damaged (its header is missing)", r["notes"][0])
        r = self.damaged(bytes(4096))
        self.assertIn("damaged (its header does not match this ghostldr.lua's pins)", r["notes"][0])

    def test_a_cache_whose_extension_is_damaged(self) -> None:
        """The core checks the extension (CRC-32, then SHA-256) and returns -7:
        the cache goes and the prompt comes."""
        data = bytearray(self.full.read_bytes())
        data[128 + 5000] ^= 1
        r = self.damaged(bytes(data))
        self.assertIn("core-ext crc", r["out"])
        self.assertIn("the cached bundle is damaged (the core's checksum of its extension failed)", r["out"])

    def test_a_cache_whose_selector_is_damaged(self) -> None:
        """The selector's CRC-32, checked here when it is read for a boot."""
        data = bytearray(self.full.read_bytes())
        data[-100] ^= 1
        r = self.damaged(bytes(data), "elfldr=boot,quit")
        self.assertIn("the cached bundle is damaged (its selector's CRC-32 is wrong)", r["out"])
        self.assertEqual([c[FLAGS] & CTX_FLAG_ELFLDR for c in r["ctxs"]], [CTX_FLAG_ELFLDR, CTX_FLAG_ELFLDR])

    # ---- storage

    def test_a_full_disk_still_boots(self) -> None:
        r = self.boot(f"client={self.full}", "disk=1000000")
        self.assertIn(("mount", "GLDRCACHE 6 512 809f000a"), r["saves"])
        self.assertIn("ghostldr: ghostldr-ext.bin not cached: the console's storage is full: the cache needs "
                      "16.0 MiB, 15.1 MiB more than is free; this boot goes on", r["sent"])
        self.assertIsNone(self.cached())
        self.assertEqual(r["ctx"][FLAGS], BOOT_FLAGS | CTX_FLAG_BUNDLE)

    def test_no_saved_data_calls(self) -> None:
        r = self.boot(f"client={self.full}", "no-savedata")
        self.assertIn("ghostldr: ghostldr-ext.bin not cached: the saved-data calls are missing; this boot goes on",
                      r["sent"])
        self.assertTrue(r["called"])

    def test_a_boot_that_stops_on_a_damaged_extension_deletes_what_it_cached(self) -> None:
        data = bytearray(self.full.read_bytes())
        data[128 + 5000] ^= 1
        sent = Path(self.tmp.name) / "damaged-in-transit.bin"
        sent.write_bytes(bytes(data))
        r = self.boot(f"client={sent}")
        self.assertIn("core-ext crc", r["out"])
        self.assertIsNone(self.cached())
        self.assertIn("The boot stopped. | Back to Luac0re's loader.", r["tv"][-1])

    def test_a_damaged_elf_loader_bundle_leaves_a_good_cache(self) -> None:
        """ghostldr-elfldr.bin damaged in transit, sent to the ELF loader while
        ghostldr-ext.bin is cached: it isn't cached, the core refuses it, and
        the cache it never replaced stays."""
        self.cache()
        data = bytearray(self.slim.read_bytes())
        data[128 + 5000] ^= 1
        sent = Path(self.tmp.name) / "damaged-elfldr.bin"
        sent.write_bytes(bytes(data))
        r = self.boot(f"elfldr=bundle:{sent},quit")
        self.assertEqual(r["out"].count("core-ext crc"), 1)
        self.assertNotIn(("delete", "GLDRCACHE 0"), r["saves"])
        self.assertEqual(self.cached(), self.full.read_bytes())
        self.assertIn("The ELF loader stopped. | Back to Luac0re's loader.", r["tv"][-1])

    # ---- clearing it

    def test_clear_cache_before_ghostldr_lua(self) -> None:
        """GHOSTLDR_CLEAR_CACHE = true sent before the release's ghostldr.lua:
        the cache goes first, then it asks for a bundle."""
        self.cache()
        payload = Path(self.tmp.name) / "clear-then-boot.lua"
        payload.write_bytes(b"GHOSTLDR_CLEAR_CACHE = true\n" + self.lua.read_bytes())
        r = self.boot(f"client={self.slim}", payload=payload)
        self.assertEqual(r["saves"][0], ("delete", "GLDRCACHE 0"))
        self.assertIn("ghostldr: the bundle cache is cleared", r["notes"])
        self.assertEqual(self.cached(), self.slim.read_bytes())

    def test_run_py_clear_cache(self) -> None:
        import run
        self.cache()
        payload = Path(self.tmp.name) / "clear.lua"
        payload.write_bytes(run.clear_cache_payload())
        r = self.boot(payload=payload)
        self.assertEqual(r["saves"], [("delete", "GLDRCACHE 0")])
        self.assertIsNone(self.cached())
        self.assertFalse(r["called"])
        self.assertEqual(r["binds"], [])
        self.assertTrue(r["tv"][-1].startswith("ghostldr |  | The bundle cache is cleared: the next ghostldr.lua "
                                               "asks for a bundle."))
        r = self.boot(payload=payload)
        self.assertIn("there is no bundle cache to clear", r["notes"][-1])

    # ---- a PS4 (no ELF loader): the countdown is ghostldr.lua's

    def test_ps4_counts_down_then_boots_the_cache(self) -> None:
        self.cache()
        r = self.boot(platform="PS4", fw="12.02")
        countdown = [t for t in r["tv"] if "Booting the cached ghostldr-ext.bin in" in t]
        self.assertEqual([int(re.search(r" in (\d)\.\.\.", t)[1]) for t in countdown], [5, 4, 3, 2, 1])
        self.assertIn("Press OK (or ○) to cancel and go back to Luac0re's loader.", countdown[0])
        self.assertEqual(r["clock"], 5)
        self.assertEqual(len(r["ctxs"]), 1)
        self.assertEqual(r["ctx"][FLAGS], BOOT_FLAGS | CTX_FLAG_BUNDLE | 8192)
        self.assert_ext_is(r["ctx"], 1)
        self.assertNotIn("elfldr", r["out"])
        self.assertEqual(r["leaks"], [])

    def test_ps4_cancel_goes_back_to_luac0re(self) -> None:
        self.cache()
        r = self.boot("cancel=3", platform="PS4", fw="12.02")
        self.assertFalse(r["called"])
        self.assertEqual(r["clock"], 3)                               # dismissed while it showed 3
        self.assertIn("Cancelled. | Back to Luac0re's loader.", r["tv"][-1])
        self.assertEqual(self.cached(), self.full.read_bytes())
        self.assertEqual((r["maps"], r["leaks"]), ({}, []))

    def test_ps4_bundle_during_the_countdown_replaces_the_cache(self) -> None:
        self.cache(self.full.read_bytes()[:-1] + b"\0")               # a damaged selector, not read yet
        r = self.boot(f"client={self.full}", platform="PS4", fw="12.02")
        self.assertEqual(self.cached(), self.full.read_bytes())
        self.assertEqual(r["clock"], 0)
        self.assertIn("received ghostldr-ext.bin (2853066 bytes", "\n".join(r["sent"]))

    def test_ps4_ignores_a_cached_elf_loader_bundle(self) -> None:
        self.cache(self.slim.read_bytes())
        r = self.boot(f"client={self.full}", platform="PS4", fw="12.02")
        self.assertNotIn("Booting the cached", "\n".join(r["tv"]))
        self.assertEqual(self.cached(), self.full.read_bytes())
        self.assertEqual(r["ctx"][FLAGS], BOOT_FLAGS | CTX_FLAG_BUNDLE | 8192)


if __name__ == "__main__":
    unittest.main()
