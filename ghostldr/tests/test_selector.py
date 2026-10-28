"""The game selector offline (docs/selector.md).  Needs `make -C ghostldr/native`.

  - preview: the UI code (ui.c, draw.c, font.c, meta.c) built natively
    against a fake catalogue renders its scenes to PNG; every partial redraw
    is checked against a whole one; a meta.bin from host/gfs_meta.py is read
    and its QOI pictures decoded by the module's own code; as on a PS4, game
    dumps are marked and ask before they boot, homebrew isn't; a save tool
    (Apollo) asks whose saves to open, greys users who aren't signed in, and
    shows a long save scan's progress, which Circle held cancels.
  - selector_host: the core's real src/sel.c and the real selector.bin run as
    x86-64 (Rosetta on Apple silicon) against a fake console whose save data
    is a directory tree built here in ghostfs's formats: a v0.1 install
    (GFSINDEX, no catalogue) shown on request or not, a five-title
    GFSCATALOG with art spread over small containers, an app root, a missing
    and a broken libSceFont, a bad CRC.  Each run must give back every block
    of direct memory, close VideoOut and every file, unload the modules it
    loaded, unmount every container and leave the module image cleared.
    On a fake PS4 (CTX_FLAG_PS4) a game's dump boots only after a Yes.  A
    save tool (its profile's "save_tool") asks whose saves to open, has the
    core mount it (title_pick) and scan it (save_prescan) before it returns,
    and leaves the user in title_user; alone, it is asked at once.

    python3 -m unittest discover -s ghostldr/tests

SELECTOR_SHOTS=DIR keeps the screenshots there; SELECTOR_NO_X86=1 skips the
x86-64 runs (when Rosetta is unavailable or misbehaving).
"""

from __future__ import annotations

import os
import platform
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
NATIVE = ROOT / "ghostldr/native"
SEL_BIN = ROOT / "build/ghostldr/selector.bin"
FONT = NATIVE / "selector/font/notosans.glf"
SHARED = [NATIVE / "selector/src" / f"{n}.c" for n in ("ui", "draw", "font", "sfo", "meta")]


def x86_64_cc() -> list[str] | None:
    """A compiler command whose output this machine can run as x86-64."""
    if sys.platform == "darwin" and shutil.which("clang"):
        if platform.machine() == "x86_64" or subprocess.run(["arch", "-x86_64", "/usr/bin/true"]).returncode == 0:
            return ["clang", "-arch", "x86_64"]
    if platform.machine() in ("x86_64", "AMD64"):
        cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
        return [cc] if cc else None
    return None


def sign(path: Path) -> None:
    """Unsigned x86-64 binaries can hang at launch under Rosetta: sign ad hoc."""
    if sys.platform == "darwin" and shutil.which("codesign"):
        subprocess.run(["codesign", "-s", "-", "-f", str(path)], capture_output=True)


def param_sfo(entries: list[tuple[str, str]]) -> bytes:
    keys, data, idx = b"", b"", []
    for k, v in entries:
        raw = v.encode() + b"\0"
        size = (len(raw) + 3) & ~3
        idx.append((len(keys), 0x0204, len(raw), size, len(data)))
        keys += k.encode() + b"\0"
        data += raw.ljust(size, b"\0")
    keys = keys.ljust((len(keys) + 3) & ~3, b"\0")
    kt = 20 + 16 * len(entries)
    out = struct.pack("<4sIIII", b"\0PSF", 0x101, kt, kt + len(keys), len(entries))
    return out + b"".join(struct.pack("<HHIII", *e) for e in idx) + keys + data


def pack_title(saves: Path, prefix: str, title: str, files: dict[str, bytes], cap: int, index_dir: str,
               incomplete: bool = False) -> tuple[int, int]:
    """A title in ghostfs's layout (gfs_pack.py, docs/ghostfs-catalogue.md): its files end to end
    over <prefix>000.. (cap bytes each) and a sorted index in index_dir."""
    names = {""}
    for f in files:
        parts = f.split("/")
        names.update("/".join(parts[:i]) for i in range(1, len(parts)))
    names.update(files)
    linear, entries, pool = b"", [], b""
    for n in sorted(names, key=lambda x: x.encode()):
        if n in files:
            entries.append((len(linear), len(files[n]), len(pool), 0))
            linear += files[n]
        else:
            entries.append((0, 0, len(pool), 1))
        pool += n.encode() + b"\0"
    count = max(1, -(-len(linear) // cap))
    for k in range(count):
        (saves / f"{prefix}{k:03d}").mkdir(parents=True)
        (saves / f"{prefix}{k:03d}/data.bin").write_bytes(linear[k * cap:(k + 1) * cap])
    names_off = 128 + 24 * len(entries)
    header = struct.pack("<4sIIIQQII16sII16sQ", b"GFS1", 2, count, len(entries), cap, len(linear), names_off,
                         len(pool), prefix.encode(), 128, 1 if incomplete else 0, title.encode(), 0x1234)
    index = header.ljust(128, b"\0") + b"".join(struct.pack("<QQII", *e) for e in entries) + pool
    (saves / index_dir).mkdir(parents=True, exist_ok=True)
    (saves / index_dir / "index.bin").write_bytes(index)
    return len(linear), count


def catalogue(saves: Path, titles: list[dict]) -> None:
    """GFSCATALOG/catalog.bin (docs/ghostfs-catalogue.md): 64-byte header, 192-byte entries by ID."""
    entries = b""
    for t in sorted(titles, key=lambda t: t["id"]):
        e = struct.pack("<16s16s32s64s16sQQII", t["id"].encode(), t["prefix"].encode(), t["index"].encode(),
                        t["name"].encode(), t["version"].encode(), 0x1234, t["total"], t["containers"],
                        t["flags"])
        entries += e.ljust(192, b"\0")
    header = struct.pack("<4sIIIIIQ", b"GFSC", 1, 64, 192, len(titles), 0, 7).ljust(64, b"\0")
    (saves / "GFSCATALOG").mkdir(parents=True)
    (saves / "GFSCATALOG/catalog.bin").write_bytes(header + entries)


@unittest.skipUnless(SEL_BIN.exists(), "build/ghostldr/selector.bin missing: run make -C ghostldr/native")
class Selector(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        t = Path(cls.tmp.name)
        cls.shots = Path(os.environ.get("SELECTOR_SHOTS") or t / "shots")
        cls.shots.mkdir(parents=True, exist_ok=True)
        cc = shutil.which("cc") or shutil.which("clang")
        cls.preview = t / "preview"
        subprocess.run([cc, "-O2", "-std=gnu11", f"-I{NATIVE / 'selector/src'}", str(HERE / "selector/preview.c"),
                        str(HERE / "selector/png_host.c"), *map(str, SHARED), "-lz", "-lm", "-o", str(cls.preview)],
                       check=True)
        # sample art (drawn by the harness), laid out as ghostfs would serve it
        cls.art = t / "art"
        for i in range(5):
            (cls.art / f"t{i}/sce_sys").mkdir(parents=True)
        subprocess.run([str(cls.preview), "--font", str(FONT), "--write-art", str(cls.art)], check=True,
                       capture_output=True)
        art = lambda i, f: (cls.art / f"t{i}/sce_sys/{f}").read_bytes()
        sfo = param_sfo([("APP_VER", "02.18"), ("TITLE", "Test Title"), ("TITLE_01", "Test Title (English)"),
                         ("TITLE_ID", "CUSA11456")])
        # a v0.1 install: GFSINDEX + GHOST000.., no catalogue
        cls.legacy = t / "saves-legacy"
        pack_title(cls.legacy, "GHOST", "CUSA11456", {"eboot.bin": b"\0" * 4096, "sce_sys/param.sfo": sfo,
                   "sce_sys/icon0.png": art(0, "icon0.png"), "sce_sys/pic1.png": art(0, "pic1.png")},
                   cap=992 << 20, index_dir="GFSINDEX")
        # five titles side by side, art spread over 200 KiB containers
        cls.multi = t / "saves-multi"
        specs = [("CUSA00001", "", "G00001", 1, False, {"sce_sys/param.sfo": param_sfo(
                     [("APP_VER", "01.05"), ("TITLE", "Starfall Odyssey"), ("TITLE_ID", "CUSA00001")])}),
                 ("CUSA00002", "Neon Drift: Night Circuit", "G00002", 2, False, {}),
                 ("CUSA00003", "Harbor Lights", "G00003", 3, False, {}),
                 ("CUSA00004", "Emberfall Chronicles", "G00004", 4, True, {}),
                 ("CUSA11456", "Ghost of Tsushima", "GHOST", 0, False, {})]
        cat = []
        for tid, name, prefix, a, incomplete, extra in specs:
            files = {"eboot.bin": b"\1" * 70000, **extra}
            for f in ("icon0.png", "pic1.png"):
                if (cls.art / f"t{a}/sce_sys/{f}").exists():
                    files[f"sce_sys/{f}"] = art(a, f)
            index = "GFSINDEX" if prefix == "GHOST" else prefix + "INDEX"
            total, count = pack_title(cls.multi, prefix, tid, files, 200 << 10, index, incomplete)
            cat.append({"id": tid, "prefix": prefix, "index": index, "name": name, "version": "01.00",
                        "total": total, "containers": count, "flags": (1 if incomplete else 0) |
                        (2 if index == "GFSINDEX" else 0)})
        catalogue(cls.multi, cat)
        # name and art stored with the install (gfs_install.py --update-meta): CUSA00003 has none of its own
        sys.path.insert(0, str(ROOT / "ghostldr/host"))
        import gfs_meta as gm
        m = gm.choose("CUSA00003", {}, True, "", "Harbor Lights at Dusk", log=lambda s: None)
        m.icon, m.icon_source = gm.decode_image(art(0, "icon0.png")), gm.ART_STORE
        m.backdrop, m.backdrop_source = gm.decode_image(art(0, "pic1.png")), gm.ART_STORE
        (cls.multi / "G00003META").mkdir()
        (cls.multi / "G00003META/meta.bin").write_bytes(gm.build(m))
        # a PS4's titles: homebrew by its ID, a game's dump, and homebrew with a
        # game-like ID that its param.sfo's CONTENT_ID gives away
        cls.ps4saves = t / "saves-ps4"
        cat = []
        for tid, prefix, cid, a in (("BREW00083", "G00083", "", 2),
                                    ("CUSA00001", "G00001", "UP0001-CUSA00001_00-STARFALLODYSSEY0", 1),
                                    ("CUSA00009", "G00009", "IV0000-CUSA00009_00-HOMEBREW00000000", 4)):
            files = {"eboot.bin": b"\1" * 5000, "sce_sys/icon0.png": art(a, "icon0.png")}
            if cid:
                files["sce_sys/param.sfo"] = param_sfo([("CONTENT_ID", cid), ("TITLE", tid), ("TITLE_ID", tid)])
            total, count = pack_title(cls.ps4saves, prefix, tid, files, 200 << 10, prefix + "INDEX")
            cat.append({"id": tid, "prefix": prefix, "index": prefix + "INDEX", "name": "", "version": "01.00",
                        "total": total, "containers": count, "flags": 0})
        catalogue(cls.ps4saves, cat)
        # a save tool (Apollo Save Tool, whose profile has "save_tool") beside a game, and alone
        cls.apollo, cls.apollo_one = t / "saves-apollo", t / "saves-apollo-one"
        for saves, ids in ((cls.apollo, ("APOL00004", "CUSA00001")), (cls.apollo_one, ("APOL00004",))):
            cat = []
            for tid in ids:
                prefix = "G" + tid
                files = {"eboot.bin": b"\1" * 5000, "sce_sys/icon0.png": art(2 if tid[0] == "A" else 1, "icon0.png")}
                total, count = pack_title(saves, prefix, tid, files, 200 << 10, prefix + "INDEX")
                cat.append({"id": tid, "prefix": prefix, "index": prefix + "INDEX", "name": "", "version": "02.40",
                            "total": total, "containers": count, "flags": 0})
            catalogue(saves, cat)
        # an app root (dev setups: no ghostfs)
        cls.approot = t / "approot"
        (cls.approot / "sce_sys").mkdir(parents=True)
        (cls.approot / "sce_sys/param.sfo").write_bytes(sfo)
        (cls.approot / "sce_sys/icon0.png").write_bytes(art(2, "icon0.png"))
        # the title profiles run.py sends: Ghost's own, and a name for CUSA00002 (profiles.py REC_NAME)
        sys.path.insert(0, str(ROOT / "ghostldr/host"))
        import profiles
        named = profiles.load("CUSA00002")
        named["name"] = "Neon Drift (from its profile)"
        cls.bundle = t / "profiles.bin"
        cls.bundle.write_bytes(profiles.compile_blob(profiles.load("CUSA11456")) + profiles.compile_blob(named)
                               + profiles.compile_blob(profiles.load("APOL00004"))
                               + profiles.compile_blob(profiles.load(None)) + b"\0" * 8)
        cls.host = None
        xcc = None if os.environ.get("SELECTOR_NO_X86") else x86_64_cc()
        if xcc:
            abi, = struct.unpack_from("<I", SEL_BIN.read_bytes(), 20)
            flags = ["-O1", "-std=gnu11", "-fno-builtin", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror",
                     f"-I{NATIVE / 'include'}", f"-DGHOST_ABI={abi}u"]
            objs = []
            for src, extra in [(NATIVE / "src/sel.c", flags), (HERE / "selector/host_env.c", flags),
                               (HERE / "selector/host_sys.c", ["-O1"]), (HERE / "selector/png_host.c", ["-O1"])]:
                obj = t / (src.stem + ".o")
                subprocess.run(xcc + extra + ["-c", str(src), "-o", str(obj)], check=True)
                objs.append(str(obj))
            cls.host = t / "selector_host"
            subprocess.run(xcc + objs + ["-lz", "-o", str(cls.host)], check=True)
            sign(cls.host)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    # ---- the blob ----
    def test_blob_layout(self) -> None:
        blob = SEL_BIN.read_bytes()
        magic, fmt, hsize, api, abi, isize, entry, font, font_size = struct.unpack_from("<QIIIIIIII", blob)
        self.assertEqual(magic, 0x4C45535244484C47)
        self.assertEqual((fmt, hsize, hsize + isize, entry), (1, 40, len(blob), 0))
        self.assertEqual(blob[hsize + font:hsize + font + 4], b"GLF1")
        self.assertEqual(blob[hsize + font:hsize + font + font_size], FONT.read_bytes())
        self.assertLess(len(blob), 4 << 20)

    # ---- the UI on this computer ----
    def test_preview_scenes(self) -> None:
        for scene in ("tour", "info", "launch"):
            p = subprocess.run([str(self.preview), "--font", str(FONT), "--out", str(self.shots), "--scene", scene,
                                "--prefix", "preview-", "--user", "Tester", "--verify"], capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertRegex(p.stderr, r"verify: 0 of \d+ partial frames differ")
            if scene == "launch":
                self.assertIn("boot title 0", p.stderr)
            if scene == "tour":   # the row and a clock tick are redrawn alone
                self.assertNotRegex(p.stderr, r"frames: \d+ drawn \(0 partial\)")
        for name in ("home", "moving", "fallbacks", "cjk", "toast", "info", "launching"):
            self.assertGreater((self.shots / f"preview-{name}.png").stat().st_size, 10000, name)

    def test_preview_usb_games(self) -> None:
        """Games on a USB drive: a second copy's folder and note, a folder that can't start and why."""
        p = subprocess.run([str(self.preview), "--font", str(FONT), "--out", str(self.shots), "--scene", "usb",
                            "--usb", "--prefix", "preview-", "--verify"], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertRegex(p.stderr, r"verify: 0 of \d+ partial frames differ")
        self.assertNotIn("boot title", p.stderr)                       # Cross on a folder that can't start
        for name in ("usb-copy", "usb-bad", "usb-why", "usb-bad-info"):
            self.assertGreater((self.shots / f"preview-{name}.png").stat().st_size, 10000, name)

    def preview_run(self, *args: str) -> str:
        p = subprocess.run([str(self.preview), "--font", str(FONT), "--out", str(self.shots), "--prefix", "preview-",
                            "--user", "Tester", "--verify", *args], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertRegex(p.stderr, r"verify: 0 of \d+ partial frames differ")
        return p.stderr

    def test_preview_ps4(self) -> None:
        """On a PS4 a game's dump is marked and Cross asks first, No selected: Cross on No and
        Circle go back, Yes boots it.  Homebrew isn't marked.  On a PS5 the same Cross boots."""
        out = self.preview_run("--scene", "ps4", "--ps4")
        self.assertEqual(out.count("PS4: asking before title 0 (CUSA11456)"), 3)
        self.assertEqual(out.count("PS4: No for title 0 (CUSA11456)"), 2)
        self.assertIn("PS4: Yes for title 0 (CUSA11456): booting it", out)
        self.assertIn("boot title 0 (CUSA11456) at 9", out)               # only after the Yes
        for name in ("ps4-home", "ps4-ask", "ps4-no", "ps4-homebrew", "ps4-homebrew-info", "ps4-info",
                     "ps4-ask-panel", "ps4-ask-yes"):
            self.assertGreater((self.shots / f"preview-{name}.png").stat().st_size, 10000, name)
        out = self.preview_run("--scene", "ps4", "--prefix", "preview-ps5-")
        self.assertNotIn("PS4:", out)
        self.assertIn("boot title 0 (CUSA11456) at 1", out)                # the first Cross

    def test_preview_long_names(self) -> None:
        """A name too long for a line takes two, then an ellipsis; in the panel, three."""
        self.preview_run("--scene", "long", "--start", "8", "--ps4")
        for name in ("long", "long-info"):
            self.assertGreater((self.shots / f"preview-{name}.png").stat().st_size, 10000, name)

    def test_dump_or_homebrew(self) -> None:
        """The rule the selector marks titles by on a PS4 (sfo.c title_is_dump)."""
        cases = [(("CUSA11456", "", ""), "dump"),                          # a retail ID
                 (("PLAS12345", "", ""), "dump"),
                 (("cusa00900", "", ""), "dump"),                          # from a folder's name
                 (("GHOST", "CUSA11456", ""), "dump"),                     # --title over param.sfo's
                 (("", "", "EP9000-CUSA00207_00-BLOODBORNE000000"), "dump"),
                 (("CUSA11456", "CUSA11456", "UP9000-CUSA11456_00-GHOSTOFTSUSHIMA0"), "dump"),
                 (("BREW00083", "BREW00083", "IV0000-BREW00083_00-HELLOWORLD000000"), "homebrew"),
                 (("CUSA99999", "CUSA99999", "IV0000-CUSA99999_00-HOMEBREW00000000"), "homebrew"),  # its publisher
                 (("HELLO_WORLD", "", ""), "homebrew"),                    # an OpenOrbis module name
                 (("LAPY20001", "", ""), "homebrew"),
                 (("NPXS20001", "", ""), "homebrew"),                      # not a game
                 (("CUSA1145", "", ""), "homebrew"),                       # too short
                 (("CUSA114567", "", ""), "homebrew"),                     # too long
                 (("", "", ""), "homebrew")]
        for args, want in cases:
            p = subprocess.run([str(self.preview), "--is-dump", *args], capture_output=True, text=True)
            self.assertEqual(p.stdout.strip(), want, args)

    def test_meta_art(self) -> None:
        """A meta.bin from gfs_meta.py, read and decoded by the module's code."""
        sys.path.insert(0, str(ROOT / "ghostldr/host"))
        import gfs_meta as gm
        own = {"icon0.png": (self.art / "t0/sce_sys/icon0.png").read_bytes(),
               "pic1.png": (self.art / "t0/sce_sys/pic1.png").read_bytes()}
        m = gm.choose("CUSA11456", own, True, "Ghost of Tsushima", log=lambda s: None, profile_version="02.18")
        meta = self.shots / "ghost-meta.bin"
        meta.write_bytes(gm.build(m))
        out = self.shots / "meta-decoded"
        out.mkdir(exist_ok=True)
        p = subprocess.run([str(self.preview), "--decode-meta", str(meta), str(out)], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("name 'Ghost of Tsushima' version '02.18' icon 1 backdrop 1", p.stdout)
        data = meta.read_bytes()
        for what, sect in (("icon", gm.SECT_ICON), ("backdrop", gm.SECT_BACKDROP)):
            s = gm.parse(data)["sections"][sect]
            want = gm.qoi_decode(data[s["offset"]:s["offset"] + s["size"]])
            got = gm.png_decode((out / f"{what}.png").read_bytes())
            self.assertEqual((got.w, got.h), (want.w, want.h))
            self.assertEqual(got.rgba, want.rgba, what)       # B8G8R8A8 from meta.c == the encoder's input
        p = subprocess.run([str(self.preview), "--font", str(FONT), "--out", str(self.shots), "--scene", "single",
                            "--titles", "1", "--meta", f"CUSA11456={meta}", "--prefix", "meta-"],
                           capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertGreater((self.shots / "meta-single.png").stat().st_size, 10000)

    # ---- the real module through the core's loader ----
    def run_host(self, mode: str, shots: str = "", in_memory: bool = False) -> str:
        if not self.host:
            self.skipTest("no way to build and run x86-64 code here")
        saves = {"multi": self.multi, "approot": self.approot, "apollo": self.apollo,
                 "apollo-one": self.apollo_one}.get(mode, self.legacy)
        env = dict(os.environ, SEL_SHOTS=shots, SEL_PROFILES=str(self.bundle), SEL_BUNDLE="1" if in_memory else "")
        crc = hex(zlib.crc32(SEL_BIN.read_bytes()))
        p = subprocess.run([str(self.host), mode, str(SEL_BIN), crc, str(saves), str(self.shots)],
                           capture_output=True, text=True, env=env, timeout=300)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertNotIn("FAILED", p.stdout)
        return p.stdout

    def test_v01_install_on_request(self) -> None:
        out = self.run_host("single", "700:home,1900:info,3300:closed,3600:launch")
        self.assertIn("1 title(s) installed (GFSINDEX), shown on request", out)
        # before its index is read: the profile's name; then param.sfo's, in the system language, wins
        self.assertIn("title 0: CUSA11456 'Ghost of Tsushima' (name profile) , 0 MiB, index GFSINDEX", out)
        self.assertIn("'Test Title (English)' 02.18", out)

    def test_ps4_dumps_ask_first(self) -> None:
        """The real module on a PS4: dumps marked, homebrew not (one only once its param.sfo is
        read), Cross on a dump asks, No and Circle go back, Yes boots it."""
        if not self.host:
            self.skipTest("no way to build and run x86-64 code here")
        env = dict(os.environ, SEL_SHOTS="3600:ask,6600:yes", SEL_PROFILES=str(self.bundle))
        p = subprocess.run([str(self.host), "ps4", str(SEL_BIN), hex(zlib.crc32(SEL_BIN.read_bytes())),
                            str(self.ps4saves), str(self.shots)], capture_output=True, text=True, env=env, timeout=300)
        out = p.stdout
        self.assertEqual(p.returncode, 0, out + p.stderr)
        self.assertNotIn("FAILED", out)
        self.assertIn("a PS4: game dumps are marked and ask before they boot", out)
        self.assertRegex(out, r"title 0: BREW00083 .*, homebrew")
        self.assertRegex(out, r"title 1: CUSA00001 .*, a game's dump")
        self.assertIn("title 2 (CUSA00009): homebrew, by its param.sfo (CUSA00009, IV0000-CUSA00009_00-", out)
        self.assertEqual(out.count("PS4: asking before title 1 (CUSA00001)"), 3)
        self.assertEqual(out.count("PS4: No for title 1 (CUSA00001)"), 2)
        self.assertIn("PS4: Yes for title 1 (CUSA00001): booting it", out)
        self.assertIn("Cross on title 1 (CUSA00001)", out)

    def test_preview_save_tool(self) -> None:
        """A save tool asks whose saves to open: every user listed, those not signed in greyed and
        refused; then the scan, whose progress shows when it is long, and the user goes to the title."""
        out = self.preview_run("--scene", "apollo")
        self.assertIn("save tool: asking whose saves to open for title 11 (APOL00004): 4 user(s)", out)
        self.assertIn("save tool: user 10000001 chosen for title 11 (APOL00004)", out)
        self.assertEqual(out.count("chosen for title"), 1)                 # Cross on a user not signed in: refused
        self.assertIn("save tool: a long scan (1 of 349 after ", out)
        self.assertRegex(out, r"save tool: scan done after 349 of 349 titles, \d+ ms")
        self.assertIn("boot title 11 (APOL00004)", out)
        self.assertIn("title_user 10000001", out)
        for name in ("apollo-home", "apollo-users", "apollo-users-greyed", "apollo-not-signed-in", "apollo-scan-start",
                     "apollo-scan", "apollo-scan-late"):
            self.assertGreater((self.shots / f"preview-{name}.png").stat().st_size, 10000, name)

    def test_preview_save_tool_alone(self) -> None:
        """The save tool alone: asked at once, no going back to a list; a short scan shows no bar."""
        out = self.preview_run("--scene", "apollo-only")
        self.assertIn("save tool: asking whose saves to open for title 0 (APOL00004)", out)
        self.assertNotIn("a long scan", out)
        self.assertIn("boot title 0 (APOL00004)", out)
        self.assertGreater((self.shots / "preview-apollo-only.png").stat().st_size, 10000)

    def test_preview_save_tool_cancel(self) -> None:
        """Circle held during the scan cancels it (a tap between two titles is missed): the question again,
        with no list to go back to; Cross scans what is left."""
        out = self.preview_run("--scene", "apollo-cancel")
        self.assertEqual(out.count("Circle: cancelling the scan"), 1)
        self.assertRegex(out, r"scan cancelled after (\d+) of 349 titles")
        n = int(__import__("re").search(r"scan cancelled after (\d+) of 349", out).group(1))
        self.assertIn(f"scan: user 10000001, {349 - n} titles to search", out)
        self.assertIn("boot title 11 (APOL00004)", out)
        for name in ("apollo-scan-hint", "apollo-cancelled", "apollo-resumed"):
            self.assertGreater((self.shots / f"preview-{name}.png").stat().st_size, 10000, name)

    def test_save_tool_on_the_console_side(self) -> None:
        """The real module: Apollo beside a game; the core mounts it once and scans for the chosen user."""
        if not self.host:
            self.skipTest("no way to build and run x86-64 code here")
        out = self.run_host("apollo", "4000:users,5200:greyed,7500:scan,9600:again,15000:resumed")
        self.assertIn("save tool: asking whose saves to open for title 0 (APOL00004): 2 user(s)", out)
        self.assertIn("user 10000002 'Second', not signed in", out)
        self.assertIn("selector_host: title_pick(APOL00004, 0)", out)
        self.assertIn("title 0 (APOL00004) mounted for its save scan: 0", out)
        self.assertIn("selector_host: save_prescan(user 10000001): 40 titles", out)
        self.assertIn("save tool: a long scan", out)
        self.assertRegex(out, r"save tool: scan cancelled after \d+ of 40 titles")
        self.assertIn("title 0 (APOL00004) opens the saves of user 10000001", out)
        out = self.run_host("apollo-one", "1000:alone")
        self.assertIn("1 title(s) installed (GFSCATALOG), a save tool: whose saves to open", out)
        self.assertIn("selector_host: save_prescan(user 10000001): 3 titles", out)
        self.assertNotIn("a long scan", out)
        self.assertIn("title 0 (APOL00004) opens the saves of user 10000001", out)

    def test_one_title_is_not_shown(self) -> None:
        out = self.run_host("skip")
        self.assertNotIn("selector:", out)  # the core closes the socket without loading the module

    def test_catalogue(self) -> None:
        out = self.run_host("multi", "2000:home,2560:moving,4000:third,4500:launch")
        self.assertIn("5 title(s) installed (GFSCATALOG)", out)
        self.assertIn("Cross on title 2 (CUSA00003)", out)
        self.assertIn("title 2 (CUSA00003): G00003META: 'Harbor Lights at Dusk'", out)
        self.assertIn("title 0 (CUSA00001): index G00001INDEX", out)
        self.assertIn("title 1: CUSA00002 'Neon Drift (from its profile)' (name profile)", out)  # over the catalogue's
        self.assertNotIn("PS4:", out)                                   # a PS5: no question

    def test_app_root(self) -> None:
        out = self.run_host("approot")
        self.assertIn("1 title(s) installed (app root), shown on request", out)

    def test_system_font_missing(self) -> None:
        out = self.run_host("nofont")
        self.assertIn("loaded libSceFont.sprx: handle 80020002", out)
        self.assertIn("using the built-in one", out)

    def test_system_font_broken(self) -> None:
        out = self.run_host("badfont")
        self.assertIn("libSceFont: failed (80460002)", out)
        self.assertIn("unloaded libSceFont ", out)
        self.assertIn("unloaded libSceFontFt ", out)

    def test_bad_crc(self) -> None:
        out = self.run_host("badcrc")
        self.assertIn("sel: no usable selector", out)

    def test_from_a_bundle(self) -> None:
        """The release's ghostldr.lua passes the module in memory (CTX_FLAG_BUNDLE):
        it runs as from the socket, and its memory goes back either way."""
        out = self.run_host("multi", in_memory=True)
        self.assertIn("Cross on title 2 (CUSA00003)", out)
        self.assertIn("ok the bundle's module taken and unmapped", out)
        out = self.run_host("skip", in_memory=True)
        self.assertNotIn("selector:", out)
        self.assertIn("ok the bundle's module taken and unmapped", out)
        self.assertIn("sel: no usable selector", self.run_host("badcrc", in_memory=True))


if __name__ == "__main__":
    unittest.main()
