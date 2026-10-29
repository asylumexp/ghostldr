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
  fake, with save containers in a directory, an instant clock, sockets in
  SWRR and in the JIT compiler, and a scripted core and ELF loader: the
  first boot caches, a later one opens the game selector at once with the
  ELF loader beside it and boots the pick in a second call (or starts the
  ELF loader alone with only ghostldr-elfldr.bin cached), the ELF loader's
  results (boot, a bundle kept for the next boot, forget, quit), another
  release's cache, damaged ones, a full disk, clearing it, and a PS4.

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
SIZE, CTX_IP, CTX_HEADER, CTX_SELECT = 0x8, 0x200, 0x208, 0x210   # listen mode: slots after the profile slot
ELFLDR_SELECT = 1 << 57                                 # the ELF loader beside the game selector
CTX_FLAG_ELFLDR, CTX_FLAG_BUNDLE, CAN_BOOT = 1 << 20, 1 << 21, 1 << 56
BOOT_FLAGS = 2 | 4096 | 1212 << 48                     # run.py boot's (the release's defaults)
RETURN, TITLE_BACK = 1 << 29, 0xBAC                     # CTX_FLAG_RETURN: back to the game selector when the game ends
SELECT = 2048                                           # CTX_FLAG_SELECT: a bundle's boot always shows the selector
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
        self.assertLess(b.image_memsz, 160 << 10)          # 128 KiB built with -Os; the full one needs 608 KiB

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
                "selects": [(int(i), int(u, 16), t, int(b)) for i, u, t, b in
                            re.findall(r"^select (-?\d+) (\w+) (\S*) (\d+)$", out, re.M)],
                "tv": re.findall(r"^dialog: (.*)$", out, re.M),
                "clock": int(re.search(r"^clock (\d+)$", out, re.M)[1])}


class ListenMode(FakeConsole):
    """Bundles sent to the port: the boot context, the ports, every refusal, the timeout, what is given back."""

    def test_full_bundle_boots_with_the_log_on_its_connection(self) -> None:
        """ghostldr-ext.bin on a PS5: cached, then the game selector with the
        ELF loader beside it on the bundle's port (the selector part given
        too), and once a game is picked a second call boots it from the
        cache, the log still on the bundle's connection."""
        r = self.run_fake(f"client={self.full}", "elfldr=pick:0:CUSA00900")
        pin = lua_pins(self.lua.read_text())[1]
        self.assertEqual(r["binds"], [("9021", "0")])
        self.assertEqual(len(r["ctxs"]), 2)
        conn = int(r["accepts"][0])
        first, c = r["ctxs"]
        self.assertEqual(first[FLAGS], CTX_FLAG_ELFLDR | ELFLDR_SELECT | 9021 << 32 | SELECT | 2 | 4096 | CTX_FLAG_BUNDLE)
        self.assertEqual((first[SEL_SIZE], first[LOG_FD], first[SIZE]), (pin["sel_size"], conn, 0x218))
        self.assertEqual(r["elfldr"], ["elfldr port 9021 can-boot 0 selecting", "elfldr listening 9021"])
        self.assertEqual(r["selects"], [(0, 0, "", 0), (0, 0, "CUSA00900", 1)])   # the pick, then booted
        self.assertEqual((c[LOG_FD], c[LOG_ADDR]), (conn, 0))     # log.c: the connection, no address
        self.assertEqual(c[FLAGS], 2 | 4096 | 1212 << 48 | CTX_FLAG_BUNDLE | RETURN)       # no selector again
        maps = {int(a, 16): int(s, 16) for a, s in r["maps"].items()}
        self.assertEqual(maps[c[EXT_FD]], (pin["ext_size"] + 0x3FFF) & ~0x3FFF)
        self.assertEqual(maps[c[SEL_FD]], (pin["sel_size"] + 0x3FFF) & ~0x3FFF)
        self.assertEqual((c[EXT_SIZE], c[EXT_CRC], c[SEL_SIZE], c[SEL_CRC]),
                         (pin["ext_size"], pin["ext_crc"], pin["sel_size"], pin["sel_crc"]))
        self.assertEqual(c[EXT_PIN].to_bytes(8, "little"), bytes.fromhex(pin["ext_sha256"][:16]))
        self.assertTrue(any("9021" in n and "ghostldr-ext.bin" in n for n in r["notes"]))
        self.assertIn("received ghostldr-ext.bin", r["sent"][0])
        self.assertIn("ghostldr: CUSA00900 picked: the ELF loader has stopped; booting it", r["sent"])
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
        self.assertEqual(r["elfldr"], ["elfldr port 9022 can-boot 0", "elfldr listening 9022"])
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
        self.assertIn("Opening the game selector. | ELF loader on 192.0.2.10:9021 until a game is picked.", r["tv"][2])
        r = self.run_fake(f"client={self.slim}", f"client={self.full}", platform="PS4", fw="12.02")
        self.assertIn("Refused: ghostldr-elfldr.bin: the ELF loader runs on a PS5 only. | Send ghostldr-ext.bin "
                      "to boot a game", r["tv"][1])
        r = self.run_fake(f"client={self.full}", "elfldr=pick:0:CUSA00900", "core=-7", "core-takes=none")
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
        self.assertEqual(r["elfldr"], ["elfldr port 9021 can-boot 0", "elfldr listening 9021"])

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
        self.assertIn("not a ghostldr bundle (no GLDRBNDL header: a .bin from something else?) (it starts 717569740a)", refused[2])
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
        self.assertEqual(r["ctx"][FLAGS], 2 | 4096 | 1212 << 48 | CTX_FLAG_BUNDLE | SELECT | 8192)

    def test_no_memory(self) -> None:
        r = self.run_fake(f"client={self.full}", "noflex")
        self.assertIn("refused: no memory for its", r["out"])
        self.assertFalse(r["called"])

    def test_a_boot_that_stops_gives_everything_back(self) -> None:
        """The core returned before taking the bundle: ghostldr.lua unmaps both
        parts and closes the connection, after taking it from the core."""
        r = self.run_fake(f"client={self.full}", "elfldr=pick:0:CUSA00900", "core=-7", "core-takes=none")
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
        r = self.boot(f"client={self.full}", "elfldr=pick:0:CUSA00900")
        self.assertEqual(self.cached(), self.full.read_bytes())     # header and parts, as sent
        self.assertEqual((self.saves / "GLDRCACHE/.blocks").read_text(), "512")   # 16 MiB
        self.assertIn(("mount", "GLDRCACHE 6 512 0"), r["saves"])
        self.assertIn("ghostldr: cached ghostldr-ext.bin in this console's saved data (GLDRCACHE save container "
                      f"16.0 MiB, {self.full.stat().st_size / 1048576:.1f} MiB): from now on send ghostldr.lua alone", r["sent"])
        self.assertEqual(r["ctx"][FLAGS], BOOT_FLAGS | CTX_FLAG_BUNDLE | RETURN)          # the pick, from the cache
        self.assertIn("core-ext ok", r["out"])
        # the cache is written before the core has the bundle's memory
        self.assertLess(r["out"].index("save-umount GLDRCACHE"), r["out"].index("core-call"))

    def test_first_boot_with_the_elf_loader_bundle_caches_it(self) -> None:
        r = self.boot(f"client={self.slim}")
        self.assertEqual(self.cached(), self.slim.read_bytes())
        self.assertEqual(r["elfldr"], ["elfldr port 9021 can-boot 0", "elfldr listening 9021"])

    # ---- Settings > Advanced: GLDRCONF/settings.txt's flag masks (selector/src/conf.c)

    def conf(self, text: str) -> Path:
        """GLDRCONF/settings.txt as the selector's Settings leaves it."""
        d = self.saves / "GLDRCONF"
        d.mkdir(exist_ok=True)
        (d / ".blocks").write_text("96")
        (d / "settings.txt").write_text(text)
        return d / "settings.txt"

    def test_settings_switches_reach_both_calls(self) -> None:
        """flags_on is set and flags_off cleared in the selector's call and the game's; pending=1 is
        written first (the selector clears it when it comes up)."""
        self.cache()
        f = self.conf("# ghostldr settings\ndialogs=off\nflags_on=0x400000\nflags_off=0x1000\n")
        r = self.boot("elfldr=pick:0:CUSA00900")
        self.assertEqual(len(r["ctxs"]), 2)
        for c in r["ctxs"]:
            self.assertEqual(c[FLAGS] & 0x401000, 0x400000)
        self.assertEqual(f.read_text(), "# ghostldr settings\ndialogs=off\nflags_on=0x400000\nflags_off=0x1000\n"
                                        "pending=1\n")
        self.assertLess(r["out"].index("save-umount GLDRCONF"), r["out"].index("core-call"))   # before the core

    def test_settings_safe_mode(self) -> None:
        """pending=1 left by a boot that never reached the selector: this one goes without the switches
        to the selector, once (pending=safe), and the TV says so; the game's call, after the selector,
        has them."""
        self.cache()
        f = self.conf("dialogs=off\nflags_on=0x400000\nflags_off=0x0\npending=1\n")
        r = self.boot("elfldr=pick:0:CUSA00900")
        self.assertEqual(r["ctxs"][0][FLAGS] & 0x400000, 0)
        self.assertEqual(r["ctxs"][1][FLAGS] & 0x400000, 0x400000)
        self.assertEqual(f.read_text(), "dialogs=off\nflags_on=0x400000\nflags_off=0x0\npending=safe\n")
        self.assertTrue([t for t in r["tv"] if "left out of this boot" in t], r["tv"])

    def test_close_ghostldr_in_settings(self) -> None:
        """Settings > About > Close ghostldr beside the ELF loader (SEL_EXIT_LOADER, -20): no second call,
        Luac0re's loader again, and the TV says why."""
        self.cache()
        r = self.boot("elfldr=close")
        self.assertEqual(len(r["ctxs"]), 1)
        self.assertIn("Closed in Settings.", r["tv"][-1])

    def test_settings_at_their_defaults_change_nothing(self) -> None:
        """No masks (or no GLDRCONF): the flags are run.py boot's, and nothing is written."""
        self.cache()
        self.conf("# ghostldr settings\nflags_on=0x0\nflags_off=0x0\n")
        r = self.boot("elfldr=pick:0:CUSA00900")
        self.assertEqual(r["ctxs"][1][FLAGS] & ~(CTX_FLAG_BUNDLE | RETURN), BOOT_FLAGS)
        self.assertNotIn(("mount", "GLDRCONF 2 0 0"), r["saves"])

    # ---- later boots: ghostldr.lua alone

    def test_later_boot_opens_the_selector_beside_the_elf_loader(self) -> None:
        """The cached ghostldr-ext.bin: at once, no countdown, the game
        selector with the ELF loader beside it on 9021 (taken and given back
        whole first), extension and selector from the cache.  The pick
        (title, index, a save tool's user) goes to a second call, which boots
        it without the selector."""
        self.cache()
        r = self.boot("elfldr=pick:1:CUSA11456:10000001")
        self.assertEqual(len(r["ctxs"]), 2)
        c, pin = r["ctxs"][0], self.pins()[1]
        self.assertEqual(c[FLAGS], CTX_FLAG_ELFLDR | ELFLDR_SELECT | 9021 << 32 | SELECT | 2 | 4096 | CTX_FLAG_BUNDLE)
        self.assert_ext_is(c, 1)
        self.assertEqual((c[SEL_SIZE], c[SEL_CRC], c[LOG_FD]), (pin["sel_size"], pin["sel_crc"], NONE))
        self.assertEqual((c[SIZE], c[APP_ROOT] != 0), (0x218, True))
        self.assertTrue(c[CTX_IP] and c[CTX_HEADER] and c[CTX_SELECT])
        self.assertEqual(r["binds"], [("9021", "0")])
        out = r["out"]                                              # 9021 given back in both processes first
        self.assertLess(out.index("jit-close 101"), out.index("bind 8 9021"))
        self.assertLess(out.index("close 8"), out.index("core-call"))
        self.assertEqual(r["elfldr"], ["elfldr port 9021 can-boot 0 selecting", "elfldr listening 9021"])
        self.assertEqual(r["selects"], [(0, 0, "", 0), (1, 0x10000001, "CUSA11456", 1)])
        b = r["ctxs"][1]
        self.assertEqual(b[FLAGS], BOOT_FLAGS | CTX_FLAG_BUNDLE | RETURN)
        self.assert_ext_is(b, 1)
        self.assertEqual(r["unmaps"], {})                             # the core took all, both times
        self.assertIn("ghostldr: the cached ghostldr-ext.bin (build " + lua_build(self.lua) + "): the game selector, "
                      "with the ELF loader on TCP 9021 beside it until a game is picked", r["notes"])
        self.assertIn("Opening the game selector. | ELF loader on 192.0.2.10:9021 until a game is picked.",
                      "\n".join(r["tv"]))
        self.assertIn("Starting CUSA11456.", r["tv"][-1])
        self.assertNotIn("countdown", out.lower())
        self.assertEqual(self.cached(), self.full.read_bytes())
        self.assertEqual(r["clock"], 0)

    def test_nothing_picked_boots_with_the_selector(self) -> None:
        self.cache()
        r = self.boot("elfldr=pick:-1:")
        self.assertEqual(r["selects"][-1][3], 0)
        self.assertEqual(r["ctxs"][1][FLAGS], BOOT_FLAGS | CTX_FLAG_BUNDLE | SELECT | RETURN)

    def test_a_game_that_ends_brings_the_selector_back(self) -> None:
        """The game's boot comes back with CTX_TITLE_BACK (it quit, crashed, or L1+R1+Options+touchpad):
        the cache is read again and the game selector opens beside the ELF loader, then boots the next pick."""
        self.cache()
        r = self.boot("elfldr=pick:0:CUSA11456,pick:1:CUSA00900,quit", f"core={TITLE_BACK},0")
        flags = [c[FLAGS] for c in r["ctxs"]]
        self.assertEqual(len(flags), 4)                               # select, game, select, game
        for i in (0, 2):
            self.assertEqual(flags[i] & ELFLDR_SELECT, ELFLDR_SELECT)
        for i in (1, 3):
            self.assertEqual(flags[i], BOOT_FLAGS | CTX_FLAG_BUNDLE | RETURN)
        self.assertEqual([s[2] for s in r["selects"] if s[3]], ["CUSA11456", "CUSA00900"])
        self.assertIn("ghostldr: the game ended: back to the game selector", r["notes"])
        self.assertIn("The game ended. | Back to the game selector.", "\n".join(r["tv"]))
        self.assertEqual(self.cached(), self.full.read_bytes())

    def test_the_selector_stops_without_a_pick(self) -> None:
        """The core's call ends otherwise (here 0, as for quit): back to Luac0re, the cache kept."""
        self.cache()
        r = self.boot("elfldr=quit")
        self.assertEqual(len(r["ctxs"]), 1)
        self.assertIn("The game selector stopped. | Back to Luac0re's loader.", r["tv"][-1])
        self.assertEqual(self.cached(), self.full.read_bytes())

    def test_a_log_connection_follows_into_the_game(self) -> None:
        """"log" at the ELF loader beside the selector, then a pick: the game's log goes to that connection."""
        self.cache()
        r = self.boot("elfldr=log+pick:0:CUSA00900")
        log = int(re.search(r"^elfldr-log (\d+)$", r["out"], re.M)[1])
        self.assertEqual(r["ctxs"][1][LOG_FD], log)
        self.assertIn((str(log), "ghostldr: the game runs; its log keeps coming here"), r["sent_to"])
        self.assertNotIn(str(log), r["closes"])

    def test_a_bundle_sent_beside_the_selector_is_for_the_next_boot(self) -> None:
        """Another build's ghostldr-ext.bin sent while the selector shows: cached
        (told so on its connection, which then carries the game's log), and
        the picked game boots with the bundle cached before it."""
        self.cache()
        r = self.boot(f"elfldr=bundle:{self.other}+pick:0:CUSA00900")
        conn = re.search(r"^elfldr-bundle (\d+)$", r["out"], re.M)[1]
        self.assertEqual(self.cached(), self.other.read_bytes())
        mine = [line for fd, line in r["sent_to"] if fd == conn]
        self.assertTrue(any("the new ghostldr ext file will be applied on the next ghostldr boot (send ghostldr.lua"
                            in m for m in mine), mine)
        self.assertEqual(mine[-1], "ghostldr: the game runs; its log keeps coming here")
        b = r["ctxs"][1]
        self.assertEqual((b[FLAGS], b[LOG_FD]), (BOOT_FLAGS | CTX_FLAG_BUNDLE | RETURN, int(conn)))
        self.assert_ext_is(b, 1)                                      # this build's, from before
        self.assertNotIn(conn, r["closes"])

    def test_only_the_elf_loader_bundle_cached(self) -> None:
        """ghostldr-elfldr.bin alone: the ELF loader, no "boot" (a
        ghostldr-ext.bin sent there is kept for the next boot)."""
        self.cache(self.slim.read_bytes())
        r = self.boot("elfldr=quit")
        self.assertEqual(r["ctx"][FLAGS], CTX_FLAG_ELFLDR | 9021 << 32 | CTX_FLAG_BUNDLE)
        self.assert_ext_is(r["ctx"], 2)
        self.assertIn("a ghostldr-ext.bin sent there is kept for the next boot", r["notes"][0])
        self.assertIn("The ELF loader quit. | Back to Luac0re's loader.", r["tv"][-1])
        self.assertEqual(self.cached(), self.slim.read_bytes())

    def test_ghostldr_ext_bin_sent_to_the_elf_loader(self) -> None:
        """A bundle sent to the ELF loader's port (told apart by its magic):
        cached in place of the old one but not run: the user is told it is
        applied on the next ghostldr boot, its connection closes, and the ELF
        loader carries on with what it had, now with "boot".  The next boot
        boots it at once."""
        self.cache(self.slim.read_bytes())
        r = self.boot(f"elfldr=bundle:{self.full},quit")
        conn = re.search(r"^elfldr-bundle (\d+)$", r["out"], re.M)[1]
        self.assertEqual(self.cached(), self.full.read_bytes())
        self.assertEqual(len(r["ctxs"]), 2)
        c = r["ctxs"][1]
        self.assertEqual(c[FLAGS], CTX_FLAG_ELFLDR | 9021 << 32 | CTX_FLAG_BUNDLE | CAN_BOOT)
        self.assert_ext_is(c, 2)                                      # the loader it had
        mine = [line for fd, line in r["sent_to"] if fd == conn]
        self.assertIn("cached ghostldr-ext.bin in this console's saved data", mine[0])
        self.assertEqual(mine[-1], f"ghostldr: received ghostldr-ext.bin (build {lua_build(self.lua)}) at the ELF "
                                   "loader: the new ghostldr ext file will be applied on the next ghostldr boot")
        self.assertIn(conn, r["closes"])
        self.assertTrue(any("The new ghostldr ext file will be applied | on the next ghostldr boot." in t
                            for t in r["tv"]))
        self.assertEqual(len(r["unmaps"]), 2)                         # the bundle's parts, cached and let go
        r = self.boot("elfldr=pick:0:CUSA00900")
        self.assertEqual(r["ctxs"][0][FLAGS] & ELFLDR_SELECT, ELFLDR_SELECT)
        self.assertEqual(r["ctx"][FLAGS], BOOT_FLAGS | CTX_FLAG_BUNDLE | RETURN)
        self.assert_ext_is(r["ctx"], 1)

    def test_ghostldr_elfldr_bin_at_the_elf_loader_keeps_a_cached_ghostldr_ext_bin(self) -> None:
        """ghostldr-ext.bin's extension has the ELF loader too, so the cache
        keeps it when ghostldr-elfldr.bin comes beside the selector."""
        self.cache()
        r = self.boot(f"elfldr=bundle:{self.slim}+pick:0:CUSA00900")
        self.assertEqual(self.cached(), self.full.read_bytes())
        self.assertIn("ghostldr: not cached: the cache's ghostldr-ext.bin has the ELF loader too", r["sent"])
        self.assertNotIn("applied on the next ghostldr boot", r["out"])
        self.assertEqual(r["ctxs"][1][FLAGS], BOOT_FLAGS | CTX_FLAG_BUNDLE | RETURN)
        self.assert_ext_is(r["ctxs"][1], 1)

    def test_another_releases_bundle_at_the_elf_loader(self) -> None:
        """Another build's bundle is a ghostldr bundle too: cached for the next
        boot (which needs that build's ghostldr.lua), its memory back, and the
        ELF loader runs on with what it had, without "boot"."""
        self.cache(self.slim.read_bytes())
        other = "%02x" % (int(lua_build(self.lua)[:2], 16) ^ 0x10) + lua_build(self.lua)[2:]   # byte 112
        r = self.boot(f"elfldr=bundle:{self.other},quit")
        conn = re.search(r"^elfldr-bundle (\d+)$", r["out"], re.M)[1]
        mine = [line for fd, line in r["sent_to"] if fd == conn]
        self.assertTrue(mine[-1].endswith(f"applied on the next ghostldr boot (send ghostldr.lua of build {other} "
                                          "with it)"), mine)
        self.assertIn(conn, r["closes"])
        self.assertEqual(self.cached(), self.other.read_bytes())       # its header as it came
        self.assertEqual(len(r["ctxs"]), 2)
        self.assertEqual(r["ctxs"][1][FLAGS], CTX_FLAG_ELFLDR | 9021 << 32 | CTX_FLAG_BUNDLE)
        self.assert_ext_is(r["ctxs"][1], 2)
        self.assertEqual(len(r["unmaps"]), 2)
        r = self.boot()                                                # this build's ghostldr.lua again
        self.assertFalse(r["called"])
        self.assertTrue(any("Version mismatch: the cached ghostldr-ext.bin" in t for t in r["tv"]))

    def test_a_bundle_that_does_not_match_this_builds_pins(self) -> None:
        """This build's ID with other contents: refused as damaged, its memory
        back, and the ELF loader runs again from the cache."""
        self.cache(self.slim.read_bytes())
        data = bytearray(self.full.read_bytes())
        struct.pack_into("<I", data, 40, struct.unpack_from("<I", data, 40)[0] ^ 1)   # the extension's CRC-32
        sent = Path(self.tmp.name) / "wrong-pins.bin"
        sent.write_bytes(bytes(data))
        r = self.boot(f"elfldr=bundle:{sent},quit")
        conn = re.search(r"^elfldr-bundle (\d+)$", r["out"], re.M)[1]
        mine = [line for fd, line in r["sent_to"] if fd == conn]
        self.assertTrue(mine[0].startswith("ghostldr: refused: ghostldr-ext.bin is damaged"), mine)
        self.assertIn(conn, r["closes"])
        self.assertEqual(self.cached(), self.slim.read_bytes())
        self.assertEqual(len(r["ctxs"]), 2)
        self.assert_ext_is(r["ctxs"][1], 2)
        self.assertEqual(len(r["unmaps"]), 2)

    def test_forget_clears_the_cache_and_asks_again(self) -> None:
        self.cache(self.slim.read_bytes())
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
        self.assertEqual(r["elfldr"][0], "elfldr port 9021 can-boot 0")

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
        self.assertEqual(r["out"].count("core-call"), 1)                # only what came next ran

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
        """The selector's CRC-32, checked here when the cache is read, before anything runs."""
        data = bytearray(self.full.read_bytes())
        data[-100] ^= 1
        r = self.damaged(bytes(data))
        self.assertIn("the cached bundle is damaged (its selector's CRC-32 is wrong)", r["out"])
        self.assertEqual([c[FLAGS] & CTX_FLAG_ELFLDR for c in r["ctxs"]], [CTX_FLAG_ELFLDR])

    # ---- storage

    def test_a_full_disk_still_boots(self) -> None:
        r = self.boot(f"client={self.full}", "disk=1000000")
        self.assertIn(("mount", "GLDRCACHE 6 512 809f000a"), r["saves"])
        self.assertIn("ghostldr: ghostldr-ext.bin not cached: the console's storage is full: the cache needs "
                      "16.0 MiB, 15.1 MiB more than is free; ghostldr goes on without it", r["sent"])
        self.assertIsNone(self.cached())
        self.assertEqual(r["ctx"][FLAGS], BOOT_FLAGS | CTX_FLAG_BUNDLE | SELECT)

    def test_no_saved_data_calls(self) -> None:
        r = self.boot(f"client={self.full}", "no-savedata")
        self.assertIn("ghostldr: ghostldr-ext.bin not cached: the saved-data calls are missing; ghostldr goes on without it",
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
        self.assertIn("The game selector stopped. | Back to Luac0re's loader.", r["tv"][-1])

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

    # ---- a PS4 (no ELF loader)

    def test_ps4_boots_the_cache_at_once(self) -> None:
        self.cache()
        r = self.boot(platform="PS4", fw="12.02")
        self.assertEqual(r["clock"], 0)
        self.assertEqual(len(r["ctxs"]), 1)
        self.assertEqual(r["ctx"][FLAGS], BOOT_FLAGS | CTX_FLAG_BUNDLE | SELECT | 8192)
        self.assert_ext_is(r["ctx"], 1)
        self.assertNotIn("elfldr", r["out"])
        self.assertEqual((r["binds"], r["leaks"]), ([], []))

    def test_ps4_damaged_cache_asks_for_the_bundle(self) -> None:
        full = self.full.read_bytes()
        self.cache(full[:-1] + bytes([full[-1] ^ 1]))                  # a damaged selector
        r = self.boot(f"client={self.full}", platform="PS4", fw="12.02")
        self.assertEqual(self.cached(), self.full.read_bytes())
        self.assertIn(f"received ghostldr-ext.bin ({self.full.stat().st_size} bytes", "\n".join(r["sent"]))
        self.assertEqual(r["ctx"][FLAGS], BOOT_FLAGS | CTX_FLAG_BUNDLE | SELECT | 8192)

    def test_ps4_ignores_a_cached_elf_loader_bundle(self) -> None:
        self.cache(self.slim.read_bytes())
        r = self.boot(f"client={self.full}", platform="PS4", fw="12.02")
        self.assertNotIn("booting the cached", "\n".join(r["notes"]))
        self.assertEqual(self.cached(), self.full.read_bytes())
        self.assertEqual(r["ctx"][FLAGS], BOOT_FLAGS | CTX_FLAG_BUNDLE | SELECT | 8192)


if __name__ == "__main__":
    unittest.main()
