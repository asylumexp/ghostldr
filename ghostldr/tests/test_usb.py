"""USB drives (docs/usb.md), offline.

The extension's USB storage stack (native/ext/src/usb_bot.c, blk.c,
part.c, fsutil.c, exfat.c, fat32.c, usbfs.c; docs/usb.md) built for this
machine (native_host/usb_host.c) and run against disk images that macOS's
own tools make and fill:

  - exFAT on MBR and on GPT, FAT32, FAT16, and an exFAT volume with no
    partition table read as a 4 KiB-sector drive;
  - a title tree with nested folders, long, Unicode and mixed-case names,
    an empty file and folder, a 300-entry folder, and fragmented files;
  - each read as a block device and through a fake USB drive that answers
    the transport's commands from the image, cleanly and with stalls, short
    data, bad and missing CSWs, phase errors, unit attentions and invalid
    CBWs on a schedule;
  - every file's bytes, size, times and type compared with what was copied;
  - the title's view (/app0) with ghostfs's semantics, case-insensitive
    lookups, damaged entry sets and chains, a damaged primary GPT;
  - a file past 4 GiB (sparse: only its first bytes and markers written);
    USB_BIG=1 also writes a real 4.5 GiB one;
  - the read path's speed on this machine.

The extension's app roots (native/ext/src/approot.c), built natively
against a fake core (ext_native/approot_host.c): the drive mounted through
libSceUsbStorage's RequestMap2 (found through the module's unwind table,
Init once, the drive that holds the library, unmounted when the boot stops,
the FTP server's drive), route B's raw stack in a -DGHOST_USB_RAW build, libraries
of titles and single titles, the selector's turn, save prefixes.
The game selector's catalogue reading such a library (selector/cat_host.c).
The computer's side: usb_drive.py, gfs_pack.py --usb, run.py and profiles.py
finding the drive's titles, and a drive prepared by them read back by the
console's stack.

    python3 -m unittest discover -s ghostldr/tests -p test_usb.py

The image tests need macOS (hdiutil, diskutil, newfs_exfat) and are skipped
elsewhere.
"""

from __future__ import annotations

import calendar
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unicodedata
import unittest
import zlib
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
NATIVE = ROOT / "ghostldr/native"
sys.path.insert(0, str(ROOT / "ghostldr/host"))
sys.path.insert(0, str(HERE))
import gfs_meta as gm  # noqa: E402
import profiles  # noqa: E402
import usb_drive  # noqa: E402
from test_profiles import make_sfo  # noqa: E402
SOURCES = [HERE / "native_host/usb_host.c"] + [NATIVE / f"ext/src/{n}.c" for n in
                                                ("usb_bot", "blk", "part", "fsutil", "exfat", "fat32", "usbfs")]
TITLE = "CUSA12345"
APP0 = f"ghostldr/{TITLE}/app0"
SKIP_NAMES = (".fseventsd", ".Spotlight-V100", ".Trashes", ".TemporaryItems")
FAULTS = ("stall_data=5,stall_csw=7,short=3,bad_csw=11,phase=13,ua=2,ua_read=9,rc16=1,nolun=1,"
          "csw_in_data=1,bad_cbw=17")


def build(out: Path) -> None:
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc:
        raise unittest.SkipTest("no C compiler")
    subprocess.run([cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-ffreestanding", "-fno-builtin", f"-I{NATIVE / 'include'}", f"-I{NATIVE / 'ext/include'}",
                    *map(str, SOURCES), "-o", str(out)], check=True)


CFLAGS = ["-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-ffreestanding",
          "-fno-builtin"]


def build_with_hostfs(out: Path, sources: list[Path], includes: list[Path], defines: tuple[str, ...] = ()) -> None:
    """A harness over the host's files (ext_native/hostfs.c, built with the system's headers)."""
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc:
        raise unittest.SkipTest("no C compiler")
    obj = out.with_suffix(".hostfs.o")
    subprocess.run([cc, "-O1", "-Wall", "-Werror", "-c", str(HERE / "ext_native/hostfs.c"), "-o", str(obj)],
                   check=True)
    subprocess.run([cc, *CFLAGS, "-Wno-gnu-include-next", "-DGHOST_ABI=0u", *defines, *[f"-I{i}" for i in includes],
                    *map(str, sources), str(obj), "-o", str(out)], check=True)


def have_disk_tools() -> bool:
    return sys.platform == "darwin" and all(shutil.which(t) for t in ("hdiutil", "diskutil", "newfs_exfat"))


def nfc(s: str) -> str:
    return unicodedata.normalize("NFC", s)


# ---- images ----

class Disk:
    """A raw image file attached as a disk and formatted by macOS.  scheme:
    MBR, GPT or None (newfs_exfat on the whole disk: no partition table)."""

    def __init__(self, path: Path, size: int, fs: str, scheme: str | None, label: str) -> None:
        self.path, self.size, self.fs, self.scheme, self.label = path, size, fs, scheme, label
        self.dev = ""
        self.mount = Path("/Volumes") / label

    def __enter__(self) -> "Disk":
        with open(self.path, "wb") as f:
            f.truncate(self.size)   # sparse
        out = subprocess.run(["hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage", "-nomount",
                              str(self.path)], check=True, capture_output=True, text=True).stdout
        self.dev = out.split()[0]
        if self.scheme:
            subprocess.run(["diskutil", "eraseDisk", self.fs, self.label, self.scheme, self.dev], check=True,
                           capture_output=True)
        else:
            subprocess.run(["newfs_exfat", "-v", self.label, self.dev.replace("/dev/disk", "/dev/rdisk")],
                           check=True, capture_output=True)
            subprocess.run(["diskutil", "mount", self.dev], check=True, capture_output=True)
        if not self.mount.is_dir():
            raise RuntimeError(f"{self.label} did not mount")
        return self

    def remount(self) -> None:
        """Unmount and mount again: what the host reads back is what is on the disk."""
        info = subprocess.run(["diskutil", "info", str(self.mount)], check=True, capture_output=True,
                              text=True).stdout
        target = re.search(r"Device Identifier:\s+(\S+)", info)[1]
        subprocess.run(["diskutil", "unmount", str(self.mount)], check=True, capture_output=True)
        subprocess.run(["diskutil", "mount", target], check=True, capture_output=True)

    def __exit__(self, *exc) -> None:
        for _ in range(5):
            if subprocess.run(["diskutil", "eject", self.dev], capture_output=True).returncode == 0:
                return
            time.sleep(1)
        subprocess.run(["hdiutil", "detach", "-force", self.dev], capture_output=True)


def write_tree(root: Path, rng: random.Random, big: int = 0) -> dict[str, bytes | None]:
    """The test title, and files fragmented on purpose; {relative path: bytes, or None for a folder}."""
    files: dict[str, bytes | None] = {}

    def put(rel: str, data: bytes) -> None:
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        files[rel] = data

    def folder(rel: str) -> None:
        (root / rel).mkdir(parents=True, exist_ok=True)
        files[rel] = None

    put(f"{APP0}/eboot.bin", rng.randbytes(300_001))
    put(f"{APP0}/sce_module/libc.prx", rng.randbytes(100_000))
    put(f"{APP0}/sce_module/libSceFios2.prx", rng.randbytes(50_000))
    put(f"{APP0}/sce_module/libSceNpToolkit2.prx", rng.randbytes(77_777))
    put(f"{APP0}/sce_sys/param.sfo", rng.randbytes(1_024))
    put(f"{APP0}/sce_sys/icon0.png", rng.randbytes(20_000))
    put(f"{APP0}/data/a/b/c/d/e/f/g/deep.bin", rng.randbytes(5_000))
    put(f"{APP0}/data/empty.bin", b"")
    folder(f"{APP0}/data/emptydir")
    for i in range(300):
        put(f"{APP0}/data/many/file{i:04d}.dat", rng.randbytes(rng.randrange(1, 2_000)))
    long_name = "A long file name " + "x" * 180 + " past the 8.3 limit.dat"
    put(f"{APP0}/data/{long_name}", rng.randbytes(3_333))
    for name in ("ゴースト・オブ・ツシマ.txt", "Ünïcödé-ñame.bin", "Ελληνικά και кириллица.txt", "game 🎮 pad.bin",
                 "MixedCase.Bin", "lower.txt", "UPPER.TXT", "dots.in.the.name", "x"):
        put(f"{APP0}/data/{name}", rng.randbytes(rng.randrange(1, 9_000)))
    if big:
        put(f"{APP0}/movies/big.bin", rng.randbytes(big))
    # fragmentation: a folder of 64 KiB files with every other one removed, then a file that fills the holes
    frag = f"ghostldr/{TITLE}/frag"
    holes = [f"{frag}/hole{i:02d}" for i in range(40)]
    for rel in holes:
        put(rel, rng.randbytes(65_536))
    for rel in holes[::2]:
        (root / rel).unlink()
        del files[rel]
    put(f"{frag}/fragmented.bin", rng.randbytes(20 * 65_536 + 12_345))
    # two files written turn about
    a, b = bytearray(), bytearray()
    with open(root / frag / "turn_a.bin", "wb") as fa, open(root / frag / "turn_b.bin", "wb") as fb:
        for _ in range(24):
            for f, buf in ((fa, a), (fb, b)):
                chunk = rng.randbytes(32_768)
                f.write(chunk)
                f.flush()
                os.fsync(f.fileno())
                buf += chunk
    files[f"{frag}/turn_a.bin"], files[f"{frag}/turn_b.bin"] = bytes(a), bytes(b)
    put(f"ghostldr/{TITLE}/prefix", b"G12345\n")
    for rel in list(files):
        parts = rel.split("/")
        for i in range(1, len(parts)):
            files.setdefault("/".join(parts[:i]), None)
    return files


def host_view(mount: Path) -> dict[str, os.stat_result]:
    """What the mounted volume says about every entry (macOS's metadata left out)."""
    out = {}
    for dirpath, dirnames, filenames in os.walk(mount):
        dirnames[:] = [d for d in dirnames if d not in SKIP_NAMES and not d.startswith("._")]
        for n in dirnames + filenames:
            if n.startswith("._"):
                continue
            p = Path(dirpath) / n
            out[nfc(p.relative_to(mount).as_posix())] = p.stat()
    return out


# ---- the harness ----

def parse_walk(text: str) -> dict[str, dict]:
    out = {}
    for line in text.splitlines():
        if not line.startswith(("F\t", "D\t")):
            continue
        f = line.split("\t")
        out[nfc(f[1].lstrip("/"))] = {
            "kind": f[0], "size": int(f[2]), "crc": f[3], "mtime": int(f[4]), "mtime_ns": int(f[5]),
            "btime": int(f[6]), "atime": int(f[7]), "ctime": int(f[8]), "mode": int(f[9], 16), "ino": int(f[10]),
            "frags": int(f[11]), "contig": int(f[12]),
        }
    return out


class Image:
    """A formatted, filled and detached image, and what was put on it."""

    def __init__(self, path: Path, fs: str, scheme: str | None, files: dict, host: dict) -> None:
        self.path, self.fs, self.scheme, self.files, self.host = path, fs, scheme, files, host


def make_image(tmp: Path, name: str, fs: str, scheme: str | None, size: int, seed: int, big: int = 0) -> Image:
    path = tmp / f"{name}.img"
    label = f"GL{name[:6].upper()}{os.getpid() % 1000:03d}"
    with Disk(path, size, fs, scheme, label) as d:
        files = write_tree(d.mount, random.Random(seed), big)
        d.remount()
        host = host_view(d.mount)
    return Image(path, fs, scheme, files, host)


@unittest.skipUnless(have_disk_tools(), "needs macOS's hdiutil, diskutil and newfs_exfat")
class UsbStackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp_dir = tempfile.TemporaryDirectory()
        cls.tmp = Path(cls.tmp_dir.name)
        cls.exe = cls.tmp / "usb_host"
        build(cls.exe)
        cls.images = {
            "exfat": make_image(cls.tmp, "exmbr", "ExFAT", "MBR", 160 << 20, 1, big=40 << 20),
            "exgpt": make_image(cls.tmp, "exgpt", "ExFAT", "GPT", 96 << 20, 2),
            "fat32": make_image(cls.tmp, "fat32", "FAT32", "MBR", 128 << 20, 3),
            "fat16": make_image(cls.tmp, "fat16", "MS-DOS FAT16", "MBR", 64 << 20, 4),
            "super": make_image(cls.tmp, "super", "ExFAT", None, 64 << 20, 5),
        }

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp_dir.cleanup()

    def run_host(self, image: Path, *args: str, opts: tuple[str, ...] = (), ok: bool = True) -> str:
        p = subprocess.run([str(self.exe), *opts, str(image), *args], capture_output=True, timeout=600)
        out = p.stdout.decode("utf-8", "surrogateescape")
        if ok:
            self.assertEqual(p.returncode, 0, out[-3000:] + p.stderr.decode())
            self.assertIn("fds\t0", out)                      # every placeholder fd closed
            self.assertRegex(out, r"memory\tpeak \d+ live 0")  # everything freed
        return out + p.stderr.decode("utf-8", "surrogateescape")

    # ---- the volume's contents ----

    def check_walk(self, img: Image, out: str) -> dict[str, dict]:
        got = parse_walk(out)
        got = {k: v for k, v in got.items() if k.split("/")[0] not in SKIP_NAMES}
        want = {nfc(k): v for k, v in img.files.items()}
        self.assertEqual(sorted(got), sorted(want), "the same paths")
        self.assertFalse([k for k in got if k.rsplit("/", 1)[-1].startswith("._")], "AppleDouble files are hidden")
        inos = [v["ino"] for v in got.values()]
        self.assertEqual(len(inos), len(set(inos)), "inode numbers are unique")
        for rel, data in want.items():
            g, h = got[rel], img.host[rel]
            with self.subTest(path=rel):
                self.assertNotEqual(g["ino"], 0)
                if data is None:
                    self.assertEqual(g["kind"], "D")
                    self.assertEqual(g["mode"], 0x416D)
                    continue
                self.assertEqual(g["kind"], "F")
                self.assertEqual(g["mode"], 0x816D)
                self.assertEqual(g["size"], len(data))
                self.assertEqual(g["crc"], f"{zlib.crc32(data):08x}")
                self.check_times(img, g, h)
        return got

    def check_times(self, img: Image, g: dict, h: os.stat_result) -> None:
        if not g["size"] and g["mtime"] == 1477944229:
            return                                           # nothing recorded: ghostfs's time
        if img.fs == "ExFAT":
            # 10 ms steps with the zone it was written in: the same instant as macOS's
            ours = g["mtime"] * 10**9 + g["mtime_ns"]
            self.assertLess(abs(ours - h.st_mtime_ns), 10_000_000, (g, h.st_mtime_ns))
            self.assertLess(abs(g["btime"] - int(h.st_birthtime)), 2)
        else:
            # local time with no zone, read as UTC; 2-second steps
            local = calendar.timegm(time.localtime(h.st_mtime))
            self.assertLessEqual(abs(g["mtime"] - local), 1, (g, h.st_mtime))
        self.assertEqual(g["ctime"], g["mtime"])

    def test_exfat_mbr_as_a_block_device(self) -> None:
        img = self.images["exfat"]
        out = self.run_host(img.path, "walk")
        self.assertIn("exFAT volume", out)
        self.assertIn("the volume's up-case table", out)
        got = self.check_walk(img, out)
        frag = {k: v for k, v in got.items() if "/frag/" in k}
        self.assertGreater(max(v["frags"] for v in frag.values()), 1, "some file is fragmented")
        self.assertTrue([v for v in got.values() if v["kind"] == "F" and v["contig"]], "some file is NoFatChain")
        self.assertIn(" 0 chain errors", out)

    def test_each_volume(self) -> None:
        for key in ("exgpt", "fat32", "fat16", "super"):
            img = self.images[key]
            with self.subTest(volume=key):
                out = self.run_host(img.path, "walk", "--dents", "300")
                self.check_walk(img, out)
                if img.fs == "FAT32":
                    self.assertIn("FAT32 volume", out)
                    got = parse_walk(out)
                    self.assertGreater(max(v["frags"] for k, v in got.items() if "/frag/" in k), 1)
                if key == "exgpt":
                    parts = self.run_host(img.path, "parts")
                    self.assertRegex(parts, r"part\t\d\t\d+\t\d+\t0\tgpt\t1\t0")   # basic data partition
                if key == "super":
                    self.assertIn("no table, sector 0", out)

    def test_through_a_fake_usb_drive(self) -> None:
        img = self.images["exfat"]
        raw = parse_walk(self.run_host(img.path, "walk", "--hash-max", str(8 << 20)))
        clean = self.run_host(img.path, "walk", "--hash-max", str(8 << 20), opts=("--bot", "none"))
        self.assertEqual(parse_walk(clean), raw)
        self.assertRegex(clean, r"bot\tcommands \d+ stalls 0 resets 0 retries 0 short 0 phase 0 bad_csw 0")
        self.assertIn("GHOSTLDR FAKE BOT DRIVE", clean)
        faulty = self.run_host(img.path, "walk", "--hash-max", str(8 << 20),
                               opts=("--bot", FAULTS, "--xfer", "65536"))
        self.assertEqual(parse_walk(faulty), raw, "every fault recovered from")
        counts = dict(re.findall(r"fault\t(\w+)\t(\d+)", faulty))
        for name in ("stall_data", "stall_csw", "short", "bad_csw", "phase", "ua", "ua_read", "rc16", "nolun",
                     "csw_in_data", "bad_cbw"):
            self.assertGreater(int(counts.get(name, 0)), 0, f"{name} happened")
        bot = re.search(r"bot\tcommands (\d+) stalls (\d+) resets (\d+) retries (\d+) short (\d+) phase (\d+) "
                        r"bad_csw (\d+)", faulty)
        self.assertTrue(all(int(x) > 0 for x in bot.groups()), bot.group(0))

    def test_fat32_through_a_faulty_drive(self) -> None:
        img = self.images["fat32"]
        raw = parse_walk(self.run_host(img.path, "walk"))
        faulty = self.run_host(img.path, "walk", opts=("--bot", FAULTS, "--xfer", str(32 << 10)))
        self.assertEqual(parse_walk(faulty), raw)

    def test_4k_sector_drive(self) -> None:
        """A volume with no partition table on a drive of 4096-byte blocks."""
        img = self.images["super"]
        out = self.run_host(img.path, "walk", opts=("--bot", "stall_data=4,short=3", "--block", "4096"))
        self.assertIn("blocks of 4096 bytes", out)
        self.check_walk(img, out)

    def test_cache_geometry_does_not_change_the_bytes(self) -> None:
        img = self.images["exfat"]
        want = parse_walk(self.run_host(img.path, "walk", "--hash-max", str(8 << 20)))
        for opts in (("--line", "4096", "--meta", "1", "--data", "1", "--ra", "1"),
                     ("--line", "16384", "--meta", "3", "--data", "5", "--ra", "4"),
                     ("--line", str(1 << 20), "--meta", "2", "--data", "4", "--ra", "4")):
            with self.subTest(opts=opts):
                self.assertEqual(parse_walk(self.run_host(img.path, "walk", "--hash-max", str(8 << 20),
                                                          opts=opts)), want)

    # ---- the title's view ----

    def test_app0_semantics(self) -> None:
        for key in ("exfat", "fat32"):
            with self.subTest(volume=key):
                out = self.run_host(self.images[key].path, "app0", f"/{APP0}")
                self.assertRegex(out, r"app0 checks: \d+ passed")
                listed = out.split("entries\n", 1)[1].split("\n")[:3]
                self.assertEqual(sorted(listed), ["libSceFios2.prx", "libSceNpToolkit2.prx", "libc.prx"])

    def test_case_insensitive_names(self) -> None:
        for key in ("exfat", "fat32", "fat16"):
            img = self.images[key]
            for rel in (f"{APP0}/data/Ünïcödé-ñame.bin", f"{APP0}/data/MixedCase.Bin", f"{APP0}/data/lower.txt",
                        f"{APP0}/data/Ελληνικά και кириллица.txt"):
                data = img.files[rel]
                for spelled in (rel, rel.upper(), rel.lower()):
                    with self.subTest(volume=key, path=spelled):
                        out = self.run_host(img.path, "read", "/usb0/" + spelled, "0", str(len(data)))
                        m = re.search(r"read\t(\d+)\t([0-9a-f]{8})", out)
                        self.assertTrue(m, out[-2000:])
                        self.assertEqual((int(m[1]), m[2]), (len(data), f"{zlib.crc32(data):08x}"))

    def test_reads_at_any_offset(self) -> None:
        img = self.images["exfat"]
        rel = f"{APP0}/movies/big.bin"
        data = img.files[rel]
        rng = random.Random(9)
        for _ in range(40):
            off = rng.randrange(0, len(data))
            n = rng.choice((1, 511, 4096, 65_535, 1 << 20, 5 << 20))
            out = self.run_host(img.path, "read", "/usb0/" + rel, str(off), str(n))
            piece = data[off:off + n]
            m = re.search(r"read\t(\d+)\t([0-9a-f]{8})", out)
            self.assertEqual((int(m[1]), m[2]), (len(piece), f"{zlib.crc32(piece):08x}"), (off, n))

    # ---- damage ----

    def test_damaged_volumes(self) -> None:
        img = self.images["exgpt"]
        work = self.tmp / "damaged.img"
        start = int(re.search(r"mounted\t\d+\t\d+\t(\d+)", self.run_host(img.path, "parts"))[1])
        lay = exfat_layout(img.path, start)
        victim = f"{APP0}/sce_module/libc.prx"
        # an entry set whose checksum no longer matches: skipped, the rest still read
        shutil.copyfile(img.path, work)
        ent = find_entry(work, lay, victim)
        with open(work, "r+b") as f:
            f.seek(ent["name_off"] + 2)
            f.write(b"Z\0")
        out = self.run_host(work, "walk")
        self.assertIn("damaged entry set", out)
        got = parse_walk(out)
        self.assertNotIn(victim, got)
        self.assertIn(f"{APP0}/sce_module/libSceFios2.prx", got)
        # a FAT chain that runs into a free cluster: the read stops, nothing crashes
        shutil.copyfile(img.path, work)
        chained = [f"ghostldr/{TITLE}/frag/{n}" for n in ("fragmented.bin", "turn_a.bin", "turn_b.bin")
                   if not find_entry(work, lay, f"ghostldr/{TITLE}/frag/{n}")["contig"]]
        self.assertTrue(chained, "a file that follows the FAT")
        frag = chained[0]
        ent = find_entry(work, lay, frag)
        with open(work, "r+b") as f:
            f.seek(lay["fat"] + 4 * ent["first"])
            f.write(struct.pack("<I", 0))
        out = self.run_host(work, "read", "/usb0/" + frag, "0", str(len(img.files[frag])), ok=False)
        self.assertRegex(out, r"pread -> |read\t\d+\t")
        m = re.search(r"read\t(\d+)\t", out)
        self.assertTrue(m is None or int(m[1]) < len(img.files[frag]))
        # a damaged primary GPT: the backup at the end of the disk is used
        shutil.copyfile(img.path, work)
        with open(work, "r+b") as f:
            f.seek(512 + 16)
            f.write(b"\xde\xad\xbe\xef")
        out = self.run_host(work, "walk", "--hash-max", "0")
        self.assertIn("primary GPT unusable", out)
        self.assertIn(f"/{APP0}/eboot.bin", out)

    # ---- past 4 GiB ----

    def test_file_past_4_gib(self) -> None:
        """A 4.7 GB file whose clusters are allocated but, on the host, never
        written (exFAT's valid data length): only its head is data.  The test
        then sets the valid length to the size itself and writes markers past
        4 GiB straight into the image, so cluster arithmetic past 2^32 bytes
        is read back from real offsets.  The image stays sparse."""
        size = 4_700_000_000
        path = self.tmp / "huge.img"
        with Disk(path, 6 << 30, "ExFAT", "GPT", f"GLHUGE{os.getpid() % 1000:03d}") as d:
            with open(d.mount / "huge.bin", "wb") as f:
                f.write(b"HEAD" * 1024)
                f.truncate(size)
            d.remount()
        try:
            start = int(re.search(r"mounted\t\d+\t\d+\t(\d+)", self.run_host(path, "parts"))[1])
            out = self.run_host(path, "read", "/usb0/huge.bin", str(size - 16), "16")
            self.assertIn("read\t16\t", out)
            m = re.search(r"read\t16\t[0-9a-f]+\t(\w+)", out)
            self.assertEqual(m[1], "00" * 16, "past the valid data: zeros")
            lay = exfat_layout(path, start)
            ent = find_entry(path, lay, "huge.bin")
            self.assertEqual(ent["size"], size)
            if not ent["contig"]:
                self.skipTest("macOS did not allocate huge.bin contiguously")
            marks = {(4 << 30) - 8: b"BEFORE4G", (4 << 30) + 12_345: b"PAST-4-GIB", size - 10: b"LAST-BYTES"}
            set_valid_length(path, lay, ent, size)
            with open(path, "r+b") as f:
                for off, mark in marks.items():
                    f.seek(lay["heap"] + (ent["first"] - 2) * lay["cs"] + off)
                    f.write(mark)
            for off, mark in marks.items():
                out = self.run_host(path, "read", "/usb0/huge.bin", str(off - 3), str(len(mark) + 6))
                want = b"\0\0\0" + mark + b"\0\0\0"
                want = want[:size - (off - 3)]
                m = re.search(r"read\t(\d+)\t([0-9a-f]{8})", out)
                self.assertEqual((int(m[1]), m[2]), (len(want), f"{zlib.crc32(want):08x}"), (off, out[-800:]))
            out = self.run_host(path, "walk", "--hash-max", "0")
            self.assertRegex(out, rf"F\t/huge.bin\t{size}\t")
        finally:
            path.unlink(missing_ok=True)

    @unittest.skipUnless(os.environ.get("USB_BIG"), "USB_BIG=1 writes a real 4.5 GiB file")
    def test_real_file_past_4_gib(self) -> None:
        path = self.tmp / "real.img"
        size = (4 << 30) + (300 << 20)
        with Disk(path, 6 << 30, "ExFAT", "MBR", f"GLREAL{os.getpid() % 1000:03d}") as d:
            rng = random.Random(7)
            crc = 0
            with open(d.mount / "real.bin", "wb") as f:
                left = size
                while left:
                    chunk = rng.randbytes(min(left, 16 << 20))
                    f.write(chunk)
                    crc = zlib.crc32(chunk, crc)
                    left -= len(chunk)
            d.remount()
        try:
            out = self.run_host(path, "walk", opts=("--mmap",))
            self.assertRegex(out, rf"F\t/real.bin\t{size}\t{crc:08x}\t")
        finally:
            path.unlink(missing_ok=True)

    # ---- speed ----

    def bench(self, opts: tuple[str, ...], chunk: int, randoms: int = 2000, runs: int = 3,
              virtual: bool = False) -> tuple[float, float, int, int]:
        """MB/s of sequential and random reads of the 40 MiB file (wall clock, or
        the modelled bus's time), and the device commands each took."""
        img = self.images["exfat"]
        rel = "/usb0/" + f"{APP0}/movies/big.bin"
        size = len(img.files[f"{APP0}/movies/big.bin"])
        best = None
        for _ in range(runs):
            out = self.run_host(img.path, "bench", rel, str(chunk), str(randoms), opts=opts)
            seq = re.search(r"bench\tsequential\t\d+\t(\d+)\t(\d+)\t(\d+)\t(\d+)", out)
            rnd = re.search(r"bench\trandom\t\d+\t(\d+)\t(\d+)\t(\d+)\t(\d+)", out)
            self.assertEqual(int(seq[1]), size)
            t_seq, t_rnd = (int(seq[3]), int(rnd[3])) if virtual else (int(seq[2]), int(rnd[2]))
            r = (int(seq[1]) / max(t_seq, 1) * 1e3, int(rnd[1]) / max(t_rnd, 1) * 1e3, int(seq[4]), int(rnd[4]))
            if not best or r[0] > best[0]:
                best = r
        return best

    def test_read_speed(self) -> None:
        """What the stack itself costs: sequential and random reads of a 40 MiB
        file through the cache from a mapped image, from the file, and through
        the fake USB drive (wall clock on this machine).  Then what a bus
        would see: the same reads timed by a model of a USB 2 stick (0.5 ms per
        command, 35 MB/s) and a USB 3 drive (0.15 ms, 350 MB/s), with 128 KiB
        and 512 KiB READs.  The floors catch a cache gone wrong; the numbers
        are printed."""
        lines = []
        mapped = {}
        for how, opts in (("mapped", ("--mmap", "--quiet")), ("file", ("--quiet",)),
                          ("fake drive", ("--mmap", "--quiet", "--bot", "none"))):
            for chunk in (4096, 65536, 1 << 20, 8 << 20):
                seq, rnd, _, _ = self.bench(opts, chunk)
                if how == "mapped":
                    mapped[chunk] = seq
                lines.append(f"  {how:>10} {chunk >> 10:>5} KiB reads: {seq:8.0f} MB/s sequential, "
                             f"{rnd:8.0f} MB/s random")
        self.assertGreater(mapped[65536], 300, "64 KiB sequential reads from memory")
        self.assertGreater(mapped[1 << 20], 500, "1 MiB sequential reads from memory")
        lines.append("  modelled bus (MB/s of bus time; commands for 40 MiB / for 300 random reads):")
        for bus, model, rate in (("USB 2", "500,35", 35), ("USB 3", "150,350", 350)):
            for xfer in (128 << 10, 512 << 10):
                cells = []
                for chunk in (4096, 65536, 1 << 20):
                    seq, rnd, cseq, crnd = self.bench(("--mmap", "--quiet", "--usb-model", model, "--xfer",
                                                       str(xfer)), chunk, 300, 1, virtual=True)
                    cells.append(f"{chunk >> 10} KiB {seq:5.1f}/{rnd:5.1f} ({cseq}/{crnd})")
                    if chunk == 65536:
                        floor = 0.85 if xfer == 512 << 10 else 0.65
                        self.assertGreater(seq, floor * rate, f"{bus} {xfer >> 10} KiB READs: streaming")
                        self.assertGreater(rnd, 0.3 * rate, f"{bus}: random 64 KiB reads")
                lines.append(f"  {bus} {xfer >> 10:>3} KiB READs, sequential/random: " + ", ".join(cells))
        print("\nusbfs read speed (40 MiB file):\n" + "\n".join(lines), file=sys.stderr)


# ---- app roots: the extension's approot.c against a fake core ----

STACK = [NATIVE / f"ext/src/{n}.c" for n in ("usbfs", "blk", "part", "fsutil", "exfat", "fat32")]


def put(root: Path, rel: str, data: bytes = b"x") -> None:
    p = root / rel.lstrip("/")
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)


def console_files(root: Path) -> None:
    """What the fake console's sandbox sees: a mapped drive at /mnt/usb0, and a dev /data."""
    lib = "mnt/usb0/ghostldr"
    put(root, f"{lib}/CUSA11111/app0/eboot.bin", b"e" * 11)
    put(root, f"{lib}/CUSA22222/app0/eboot.bin", b"e" * 22)
    put(root, f"{lib}/CUSA22222/prefix", b"GHOST\n")
    put(root, f"{lib}/CUSA33333/notes.txt")                         # no app0: not a game
    put(root, f"{lib}/lowercase/app0/eboot.bin")                     # nothing names it
    put(root, f"{lib}/HOMEBREW_1/app0/eboot.bin", b"e" * 7)
    put(root, f"{lib}/HOMEBREW_1/prefix", b"GFSBAD\n")              # a reserved prefix: the default instead
    put(root, "mnt/usb0/games/mygame/eboot.bin", b"e" * 5)
    put(root, "mnt/usb0/games/mygame/sce_sys/param.sfo", make_sfo({"TITLE": "My Game", "TITLE_ID": "CUSA44444"}))
    put(root, "mnt/usb0/games/nosfo/eboot.bin")
    put(root, "mnt/usb0/one/ghostldr/CUSA88888/app0/eboot.bin", b"e" * 8)
    put(root, "mnt/usb0/user's file.txt", b"keep")
    put(root, "data/lib/CUSA55555/app0/eboot.bin")
    put(root, "data/lib/CUSA66666/app0/eboot.bin")
    put(root, "data/empty/readme.txt")


def anywhere_files(root: Path) -> None:
    """A drive (FSROOT/mnt/usb1) with games in folders of any name, as a person would copy them."""
    d = "mnt/usb1"
    meta = gm.Meta("CUSA00900", "Bloodborne")
    meta.name_source = gm.NAME_GIVEN
    files = {
        # param.sfo names it; no app0 level; spaces and parentheses in the names
        "My Games/ghost of tsushima/eboot.bin": b"g" * 26,
        "My Games/ghost of tsushima/sce_sys/param.sfo": make_sfo({"TITLE": "Ghost", "TITLE_ID": "CUSA11456"}),
        "My Games/ghost of tsushima/sce_module/libc.prx": b"m",
        # a game inside a game's folder is not looked for
        "My Games/ghost of tsushima/extras/CUSA99999/eboot.bin": b"x",
        # meta.bin names it (no param.sfo), with an app0 level
        "Bloodborne copy/app0/eboot.bin": b"b" * 9,
        "Bloodborne copy/meta.bin": gm.build(meta),
        "Bloodborne copy/prefix": b"BB\n",
        # the folder's name has the ID (no param.sfo, no meta.bin)
        "Homebrew/PS4 Xplorer (LAPY20001)/eboot.bin": b"h" * 3,
        # the old layout: the whole name is the ID
        "ghostldr/CUSA11456/app0/eboot.bin": b"G" * 26,
        "ghostldr/CUSA11456/app0/sce_sys/param.sfo": make_sfo({"TITLE": "Ghost", "TITLE_ID": "CUSA11456"}),
        # can't be booted
        "Broken/nothing names it/eboot.bin": b"n",
        "Broken/half a copy/app0/sce_sys/param.sfo": make_sfo({"TITLE": "Half", "TITLE_ID": "CUSA77777"}),
        "Broken/no eboot/sce_sys/param.sfo": make_sfo({"TITLE": "Bits", "TITLE_ID": "CUSA66666"}),
        # too deep: 5 folders down
        "a/b/c/d/e/eboot.bin": b"d",
        "a/b/c/d/e/sce_sys/param.sfo": make_sfo({"TITLE": "Deep", "TITLE_ID": "CUSA12121"}),
        # hidden folders are skipped
        ".Trashes/501/old/eboot.bin": b"t",
        ".Trashes/501/old/sce_sys/param.sfo": make_sfo({"TITLE": "Old", "TITLE_ID": "CUSA31313"}),
        "notes/readme.txt": b"r",
    }
    for rel, data in files.items():
        put(root, f"{d}/{rel}", data)


class AppRootTests(unittest.TestCase):
    """approot.c as shipped: the drive mounted by AutoMounter (RequestMap2)."""
    DEFINES: tuple[str, ...] = ()
    SOURCES: list[Path] = []

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp_dir = tempfile.TemporaryDirectory()
        cls.tmp = Path(cls.tmp_dir.name)
        cls.exe = cls.tmp / "approot_host"
        build_with_hostfs(cls.exe, [HERE / "ext_native/approot_host.c", NATIVE / "ext/src/approot.c", *cls.SOURCES],
                          [HERE / "ext_native", NATIVE / "ext/include", NATIVE / "include"], cls.DEFINES)
        cls.fs = cls.tmp / "fs"
        console_files(cls.fs)
        anywhere_files(cls.fs)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp_dir.cleanup()

    def run_root(self, root: str, *opts: str, fs: Path | None = None) -> dict:
        out = subprocess.run([str(self.exe), str(fs or self.fs), root, *opts], capture_output=True, text=True,
                             timeout=60)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        r = {"log": out.stdout, "lib": []}
        for line in out.stdout.splitlines():
            kind, _, rest = line.partition(" ")
            if kind in ("init", "pick"):        # paths may hold spaces
                m = re.fullmatch(r"(-?\d+) root=(.*) app_root=(.*) library=([01])", rest)
                r[kind] = (int(m[1]), m[2], m[3], m[4] == "1")
            elif kind == "lib":
                _, tid, flags, path, folder, note = rest.split("\t")
                r["lib"].append((tid, int(flags), path, folder, note))
            elif kind in ("profile", "saves", "eboot", "registered", "mount", "prepared", "home", "mount-after",
                          "session", "after-pick", "session-flag"):
                r[kind] = rest
        return r

    MAP = ("--map", "/mnt/usb0")
    ANY = ("--map", "/mnt/usb1")
    LIB = "/ghostldr_usb/ghostldr"
    BAD = 1
    DUP = 2

    def test_a_library_on_a_mounted_drive_goes_to_the_selector(self) -> None:
        r = self.run_root("usb:/ghostldr", *self.MAP, "--pick", "CUSA22222")
        self.assertIn("RequestMap2 0x", r["log"])                        # found through the unwind table
        self.assertIn("sceUsbStorageInit -> 0", r["log"])
        self.assertIn("USB drive 1001 mounted at /ghostldr_usb: 0; /ghostldr_usb/ghostldr found", r["log"])
        self.assertIn("approot: 3 games in ghostldr", r["log"])         # CUSA11111, CUSA22222, HOMEBREW_1
        self.assertEqual(r["init"], (1, "usb:/ghostldr", self.LIB, True))
        self.assertEqual([(t, f) for t, f, *_ in r["lib"]],
                         [("CUSA11111", 0), ("CUSA22222", 0), ("HOMEBREW_1", 0), ("", self.BAD)])
        self.assertIn("no sce_sys/param.sfo", r["lib"][3][4])           # lowercase/: nothing names it
        root = f"{self.LIB}/CUSA22222/app0"
        self.assertEqual(r["pick"], (0, root, root, False))
        self.assertEqual(r["after-pick"], "0")                           # the list is gone once chosen
        self.assertEqual(r["profile"], "CUSA22222")
        self.assertEqual(r["saves"], "CUSA22222 GHOSTSAVE")              # its prefix file
        self.assertEqual(r["eboot"], "found 22")
        self.assertEqual(r["mount"], "/ghostldr_usb")                    # kept for the title
        self.assertNotIn("registered", r)

    def test_games_anywhere_on_the_drive(self) -> None:
        r = self.run_root("usb:/", *self.ANY, "--pick", "-")
        self.assertEqual(r["init"][:2], (1, "usb:/"))
        lib = [(t, f, folder.removeprefix("/ghostldr_usb/"), note) for t, f, _, folder, note in r["lib"]]
        ok = [x for x in lib if not x[1] & self.BAD]
        self.assertEqual([(t, f, folder) for t, f, folder, _ in ok], [
            ("CUSA00900", 0, "Bloodborne copy"),                         # meta.bin
            ("CUSA11456", 0, "My Games/ghost of tsushima"),              # param.sfo
            ("CUSA11456", self.DUP, "ghostldr/CUSA11456"),               # a second copy: after the first by folder
            ("LAPY20001", 0, "Homebrew/PS4 Xplorer (LAPY20001)")])       # the folder's name
        self.assertIn("--title boots the one in My Games/ghost of tsushima", ok[2][3])
        bad = {folder: note for _, f, folder, note in lib if f & self.BAD}
        self.assertEqual(set(bad), {"Broken/nothing names it", "Broken/half a copy", "Broken/no eboot"})
        self.assertIn("name the folder after the game's ID", bad["Broken/nothing names it"])
        self.assertIn("app0 folder has no eboot.bin", bad["Broken/half a copy"])
        self.assertIn("no eboot.bin", bad["Broken/no eboot"])
        self.assertNotIn("CUSA99999", r["log"])                          # inside a game's folder
        self.assertNotIn("CUSA12121", r["log"])                          # too deep
        self.assertNotIn("CUSA31313", r["log"])                          # .Trashes
        self.assertIn("approot: 4 games on the USB drive", r["log"])
        self.assertEqual(r["pick"][0], -1)                               # the selector chose nothing
        self.assertIn("no game chosen", r["log"])
        self.assertEqual(r["mount"], "none")

    def test_a_game_by_title_or_by_the_selector_s_copy(self) -> None:
        r = self.run_root("usb:/", *self.ANY, "--title", "CUSA11456")    # the first copy
        root = "/ghostldr_usb/My Games/ghost of tsushima"
        self.assertEqual(r["init"], (0, root, root, False))
        self.assertEqual((r["profile"], r["saves"], r["eboot"]), ("CUSA11456", "CUSA11456 G11456SAVE", "found 26"))
        r = self.run_root("usb:/", *self.ANY, "--pick-at", "2")         # the selector's second copy
        root = "/ghostldr_usb/ghostldr/CUSA11456/app0"
        self.assertEqual(r["pick"], (0, root, root, False))
        r = self.run_root("usb:/", *self.ANY, "--pick", "CUSA00900")
        self.assertEqual(r["pick"][1], "/ghostldr_usb/Bloodborne copy/app0")
        self.assertEqual(r["saves"], "CUSA00900 BBSAVE")
        r = self.run_root("usb:/", *self.ANY, "--title", "lapy20001")    # IDs compare as on a PC
        self.assertEqual(r["init"][1], "/ghostldr_usb/Homebrew/PS4 Xplorer (LAPY20001)")

    def test_a_profile_s_prefix(self) -> None:
        bundle = self.tmp / "profiles.bin"
        bundle.write_bytes(profiles.boot_bundle("CUSA11456"))
        r = self.run_root("usb:/", *self.ANY, "--title", "CUSA11456", "--profiles", str(bundle))
        self.assertEqual(r["saves"], "CUSA11456 GHOSTSAVE")              # no prefix file: Ghost's profile's
        r = self.run_root("usb:/ghostldr", *self.MAP, "--title", "CUSA22222", "--profiles", str(bundle))
        self.assertEqual(r["saves"], "CUSA22222 GHOSTSAVE")              # the prefix file comes first

    def test_auto(self) -> None:
        r = self.run_root("auto", *self.ANY, "--title", "CUSA00900")     # a game on a drive
        self.assertEqual(r["init"][:2], (0, "/ghostldr_usb/Bloodborne copy/app0"))
        r = self.run_root("auto", *self.ANY)                             # several: the selector
        self.assertEqual(r["init"][0], 1)
        r = self.run_root("auto", *self.ANY, "--title", "CUSA55555")     # not on the drive: the installed ones
        self.assertEqual((r["init"][0], r["mount"]), (2, "none"))
        self.assertIn("CUSA55555 is not among the games", r["log"])
        self.assertIn("booting an installed title instead", r["log"])
        r = self.run_root("auto", "--map", "/data/empty")                # a drive without games
        self.assertEqual((r["init"][0], r["mount"]), (2, "none"))
        self.assertIn("holds no games", r["log"])
        self.assertEqual(r["session-flag"], "1")   # the session stays known when the code is placed again
        r = self.run_root("auto")                                        # no USB storage at all
        self.assertEqual(r["init"][0], 2)
        r = self.run_root("auto", *self.ANY, "--map-fail")               # the mount refused
        self.assertEqual(r["init"][0], 2)

    def test_the_drive_with_games_out_of_two(self) -> None:
        r = self.run_root("usb:/", "--other", "/data/empty", *self.ANY, "--title", "LAPY20001")
        self.assertIn("fake: unmounted 1000", r["log"])                  # the first drive has no game
        self.assertEqual(r["init"][:2], (0, "/ghostldr_usb/Homebrew/PS4 Xplorer (LAPY20001)"))

    def test_a_large_drive_is_looked_at_only_so_far(self) -> None:
        fs = self.tmp / "big"
        for i in range(300):
            put(fs, f"mnt/usb2/folder{i:03d}/readme.txt")
        put(fs, "mnt/usb2/zz/game/eboot.bin")
        put(fs, "mnt/usb2/zz/game/sce_sys/param.sfo", make_sfo({"TITLE_ID": "CUSA10101"}))
        r = self.run_root("usb:/", "--map", "/mnt/usb2", fs=fs)
        self.assertIn("stopped looking after", r["log"])
        r = self.run_root("usb:/zz", "--map", "/mnt/usb2", fs=fs)        # a folder narrows it
        self.assertEqual(r["init"][:2], (0, "/ghostldr_usb/zz/game"))

    def test_init_once(self) -> None:
        r = self.run_root("usb:/ghostldr", *self.MAP, "--title", "CUSA11111", "--inited")
        self.assertNotIn("sceUsbStorageInit", r["log"])                  # already open: not again
        self.assertNotIn("second sceUsbStorageInit", r["log"])
        self.assertEqual(r["init"][0], 0)

    def test_the_drive_that_holds_the_library(self) -> None:
        r = self.run_root("usb:/ghostldr", "--other", "/data/empty", *self.MAP, "--title", "CUSA11111")
        self.assertIn("USB drive 1000 mounted at /ghostldr_usb: 0; /ghostldr_usb/ghostldr missing", r["log"])
        self.assertIn("fake: unmounted 1000", r["log"])
        self.assertIn("USB drive 1001 mounted at /ghostldr_usb: 0; /ghostldr_usb/ghostldr found", r["log"])
        self.assertEqual(r["init"][0], 0)

    def test_title_given(self) -> None:
        r = self.run_root("usb:/ghostldr/", *self.MAP, "--title", "CUSA11111")
        root = f"{self.LIB}/CUSA11111/app0"
        self.assertEqual(r["init"], (0, root, root, False))
        self.assertEqual(r["saves"], "CUSA11111 G11111SAVE")             # gfs_pack.py's default prefix
        self.assertEqual(r["eboot"], "found 11")

    def test_the_ftp_server_s_drive(self) -> None:
        # a title from the drive: the FTP server keeps its mount and session
        r = self.run_root("usb:/ghostldr", *self.MAP, "--title", "CUSA11111", "--ftp")
        self.assertEqual(r["log"].count("fake: mounted"), 1)
        self.assertEqual((r["prepared"], r["home"], r["session"]), ("/ghostldr_usb", "/ghostldr_usb", "1"))
        # a title from elsewhere: the drive is mounted for the FTP server before the title runs
        r = self.run_root("/data/lib", *self.MAP, "--title", "CUSA55555", "--ftp")
        self.assertEqual(r["mount"], "none")
        self.assertIn("sceUsbStorageInit -> 0", r["log"])
        self.assertIn("USB drive 1001 mounted at /ghostldr_usb: 0; /ghostldr_usb/ found", r["log"])
        self.assertEqual((r["prepared"], r["home"], r["session"]), ("/ghostldr_usb", "/ghostldr_usb", "1"))
        # no FTP server: the title's own Init and Term reach the library
        r = self.run_root("/data/lib", *self.MAP, "--title", "CUSA55555")
        self.assertEqual((r["session"], r["session-flag"]), ("0", "0"))
        self.assertNotIn("sceUsbStorageInit", r["log"])
        # no drive as the title starts, one plugged in before a client logs in
        r = self.run_root("/data/lib", *self.MAP, "--title", "CUSA55555", "--ftp", "--late")
        self.assertEqual((r["prepared"], r["home"], r["mount-after"], r["session"]),
                         ("none", "/ghostldr_usb", "/ghostldr_usb", "1"))
        self.assertEqual(r["log"].count("sceUsbStorageInit"), 1)        # the session is opened once
        # the drive pulled out: a client starts in /, and the session stays open
        r = self.run_root("/data/lib", *self.MAP, "--title", "CUSA55555", "--ftp", "--unplug")
        self.assertIn("the USB drive at /ghostldr_usb is gone", r["log"])

    def test_a_reserved_prefix_is_not_used(self) -> None:
        r = self.run_root("usb:/ghostldr", *self.MAP, "--title", "HOMEBREW_1")
        self.assertEqual(r["saves"], "HOMEBREW_1 GHOMEBREW1SAVE")

    def test_a_title_not_on_the_drive(self) -> None:
        r = self.run_root("usb:/ghostldr", *self.MAP, "--title", "CUSA99999")
        self.assertEqual(r["init"][0], -1)
        self.assertIn("CUSA99999 is not among the games in ghostldr", r["log"])
        self.assertEqual(r["mount"], "none")                             # the boot stops: the drive goes back
        r = self.run_root("usb:/ghostldr", *self.MAP, "--pick", "-")      # the selector chose nothing
        self.assertEqual(r["pick"][0], -1)
        self.assertIn("no game chosen", r["log"])
        self.assertEqual(r["mount"], "none")

    def test_one_title_needs_no_selector_unless_asked(self) -> None:
        r = self.run_root("usb:/one/ghostldr", *self.MAP)
        root = "/ghostldr_usb/one/ghostldr/CUSA88888/app0"
        self.assertEqual(r["init"], (0, root, root, False))
        r = self.run_root("usb:/one/ghostldr", *self.MAP, "--select", "--pick", "CUSA88888")
        self.assertEqual(r["init"], (1, "usb:/one/ghostldr", "/ghostldr_usb/one/ghostldr", True))
        self.assertEqual(r["pick"][0], 0)

    def test_a_single_title_root(self) -> None:
        r = self.run_root("usb:/ghostldr/CUSA11111/app0", *self.MAP)    # named by its folder
        self.assertEqual(r["init"][:2], (0, f"{self.LIB}/CUSA11111/app0"))
        self.assertEqual(r["saves"], "CUSA11111 G11111SAVE")
        r = self.run_root("usb:/games/mygame", *self.MAP)                # named by param.sfo
        self.assertEqual(r["init"][:2], (0, "/ghostldr_usb/games/mygame"))
        self.assertEqual((r["profile"], r["saves"]), ("CUSA44444", "CUSA44444 G44444SAVE"))
        r = self.run_root("usb:/games/nosfo", *self.MAP)                 # nothing names it
        self.assertEqual(r["init"][0], -1)
        self.assertIn("pass run.py --title", r["log"])
        r = self.run_root("usb:/games/nosfo", *self.MAP, "--title", "NOSFO")
        self.assertEqual((r["init"][0], r["saves"]), (0, "NOSFO GNOSFOSAVE"))
        r = self.run_root("usb:/Bloodborne copy", *self.ANY)            # a game's folder: its app0
        self.assertEqual((r["init"][:2], r["saves"]), ((0, "/ghostldr_usb/Bloodborne copy/app0"), "CUSA00900 BBSAVE"))

    def test_a_missing_or_empty_library(self) -> None:
        r = self.run_root("usb:/nothing", *self.MAP)                     # no such folder on any drive
        self.assertEqual(r["init"][0], -1)
        self.assertIn("/ghostldr_usb/nothing missing", r["log"])
        self.assertIn("no USB drive has /nothing", r["log"])
        self.assertEqual(r["mount"], "none")
        r = self.run_root("usb:/ghostldr/CUSA33333", *self.MAP)          # folders, but no game in them
        self.assertEqual(r["init"][0], -1)
        self.assertIn("holds no games", r["log"])
        self.assertEqual(r["mount"], "none")
        r = self.run_root("usb:/games", *self.MAP)                       # one game, one that nothing names
        self.assertEqual(r["init"][:2], (0, "/ghostldr_usb/games/mygame"))

    def test_no_way_to_the_drive(self) -> None:
        r = self.run_root("usb:/ghostldr", *self.MAP, "--map-fail")
        self.assertEqual(r["init"][0], -1)
        self.assertIn("mounted at /ghostldr_usb: 80f40014", r["log"])
        r = self.run_root("usb:/ghostldr")                               # no libSceUsbStorage at all
        self.assertEqual(r["init"][0], -1)
        self.assertIn("libSceUsbStorage is not loadable", r["log"])
        r = self.run_root("usb:/ghostldr", *self.MAP, "--no-fde")        # RequestMap2 not where it should be
        self.assertEqual(r["init"][0], -1)
        self.assertIn("RequestMap2 0x0", r["log"])
        self.assertIn("lacks what mounting needs", r["log"])

    def test_a_dev_library(self) -> None:
        r = self.run_root("/data/lib", "--pick", "CUSA66666")
        self.assertEqual(r["init"], (1, "/data/lib", "/data/lib", True))
        self.assertEqual(r["pick"][:2], (0, "/data/lib/CUSA66666/app0"))
        self.assertNotIn("saves", r)                                     # saves stay beside the dev dump
        self.assertNotIn("sceUsbStorage", r["log"])
        r = self.run_root("/data/empty")
        self.assertEqual(r["init"][:2], (0, "/data/empty"))               # nothing here: the boot says so later
        self.assertIn("holds no games", r["log"])


class RawRouteTests(AppRootTests):
    """approot.c built with USB_RAW=1 (-DGHOST_USB_RAW): route B, the
    extension's own stack, after the mount fails."""
    DEFINES = ("-DGHOST_USB_RAW",)
    SOURCES = STACK

    @unittest.skipUnless(have_disk_tools(), "needs macOS's hdiutil and diskutil")
    def test_raw_route_from_a_drive_image(self) -> None:
        img = self.tmp / "drive.img"
        with Disk(img, 48 << 20, "ExFAT", "MBR", f"GLROOT{os.getpid() % 1000:03d}") as d:
            put(d.mount, "ghostldr/CUSA77777/app0/eboot.bin", b"E" * 777)
            put(d.mount, "ghostldr/CUSA77777/app0/sce_module/libc.prx", b"L" * 100)
            put(d.mount, "ghostldr/CUSA77777/prefix", b"G77777\n")
            put(d.mount, "ghostldr/CUSA78787/app0/eboot.bin", b"E")
        r = self.run_root("usb:/ghostldr", "--drive", str(img), "--pick", "CUSA77777")   # no mount: route B
        self.assertEqual(r["registered"], "usbfs")
        self.assertIn("route B", r["log"])
        self.assertEqual(r["init"], (1, "usb:/ghostldr", "/usb0/ghostldr", True))
        self.assertEqual(r["pick"], (0, "/app0", "/app0", False))
        self.assertIn("usbfs: /app0 is /ghostldr/CUSA77777/app0 on the drive", r["log"])
        self.assertEqual(r["eboot"], "found 777")
        self.assertEqual(r["saves"], "CUSA77777 G77777SAVE")
        # the mount first, route B when it fails
        r = self.run_root("usb:/ghostldr", *self.MAP, "--map-fail", "--drive", str(img), "--title", "CUSA78787")
        self.assertIn("mounted at /ghostldr_usb: 80f40014", r["log"])
        self.assertEqual(r["init"], (0, "/app0", "/app0", False))
        self.assertEqual(r["eboot"], "found 1")


class SelectorLibraryTests(unittest.TestCase):
    """The game selector's catalogue reading the games the extension found
    (selector/src/cat.c; ghost_api.library)."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp_dir = tempfile.TemporaryDirectory()
        cls.tmp = Path(cls.tmp_dir.name)
        cls.exe = cls.tmp / "cat_host"
        sel = NATIVE / "selector/src"
        build_with_hostfs(cls.exe, [HERE / "selector/cat_host.c", sel / "cat.c", sel / "sfo.c", sel / "meta.c"],
                          [HERE / "selector/console_types", sel, NATIVE / "include"])
        fs = cls.fs = cls.tmp / "fs"
        lib = "usb0/ghostldr"
        put(fs, f"{lib}/CUSA22222/app0/eboot.bin")
        put(fs, f"{lib}/CUSA22222/app0/sce_sys/param.sfo",
            make_sfo({"TITLE": "Second Title", "TITLE_ID": "CUSA22222", "APP_VER": "01.05"}))
        put(fs, f"{lib}/CUSA22222/app0/sce_sys/icon0.png", b"P" * 1234)
        put(fs, f"{lib}/CUSA22222/app0/sce_sys/pic1.png", b"P" * 4321)
        put(fs, f"{lib}/CUSA11111/app0/eboot.bin")
        m = gm.Meta("CUSA11111", "A Named Homebrew")
        m.name_source = gm.NAME_GIVEN
        put(fs, f"{lib}/CUSA11111/meta.bin", gm.build(m))
        put(fs, "usb0/My Games/a game/eboot.bin")                    # no app0 level: its own folder
        put(fs, "usb0/My Games/a game/sce_sys/param.sfo", make_sfo({"TITLE": "Loose", "TITLE_ID": "CUSA33333"}))
        put(fs, "usb0/broken/eboot.bin")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp_dir.cleanup()

    @staticmethod
    def game(tid: str, app0: str, folder: str, flags: int = 0, note: str = "") -> str:
        return f"{tid}|{app0}|{len(folder)}|{flags}|{note}"

    def test_titles_names_and_art(self) -> None:
        games = [self.game("CUSA11111", "/usb0/ghostldr/CUSA11111/app0", "/usb0/ghostldr/CUSA11111"),
                 self.game("CUSA22222", "/usb0/ghostldr/CUSA22222/app0", "/usb0/ghostldr/CUSA22222"),
                 self.game("CUSA33333", "/usb0/My Games/a game", "/usb0/My Games/a game", 2, "a second copy"),
                 self.game("", "/usb0/broken", "/usb0/broken", 1, "no sce_sys/param.sfo")]
        out = subprocess.run([str(self.exe), str(self.fs), "/usb0", "library", *games], capture_output=True,
                             text=True, timeout=60).stdout
        self.assertIn("count 4 library 1 root /usb0", out)
        rows = [ln.split("\t")[1:] for ln in out.splitlines() if ln.startswith("title\t")]
        self.assertEqual([r[0] for r in rows], ["CUSA11111", "CUSA22222", "CUSA33333", ""])   # the extension's order
        self.assertEqual(rows[1][1:7], ["Second Title", "01.05", "1", "1234", "4321",
                                        str(len(make_sfo({"TITLE": "Second Title", "TITLE_ID": "CUSA22222",
                                                          "APP_VER": "01.05"})))])
        self.assertEqual(rows[0][8], "A Named Homebrew")                          # its meta.bin, beside app0
        self.assertEqual(rows[1][8], "-")
        self.assertEqual(rows[2][1], "Loose")                                     # param.sfo in its own folder
        self.assertEqual(rows[2][9:], ["ok", "a second copy"])                    # a second copy can start
        self.assertEqual(rows[3][9:], ["bad", "no sce_sys/param.sfo"])           # this one can't
        self.assertNotIn("FAILED", out)

    def test_one_title_at_the_root_as_before(self) -> None:
        out = subprocess.run([str(self.exe), str(self.fs), "/usb0/ghostldr/CUSA22222/app0"], capture_output=True,
                             text=True, timeout=60).stdout
        self.assertIn("count 1 library 0", out)
        self.assertIn("title\tCUSA22222\tSecond Title\t01.05\t1\t1234\t4321\t", out)   # the ID from param.sfo


# ---- the computer's side ----

def dump(root: Path, title: str, extra: dict[str, bytes] | None = None) -> Path:
    files = {"eboot.bin": b"\x7fELF" + b"\0" * 500, "sce_module/libc.prx": b"L" * 3000,
             "sce_sys/param.sfo": make_sfo({"TITLE": f"Title {title}", "TITLE_ID": title, "APP_VER": "01.00"}),
             "data/a/b.bin": bytes(range(256)) * 40, "data/empty.bin": b""}
    files.update(extra or {})
    for rel, data in files.items():
        put(root, rel, data)
    (root / "data/emptydir").mkdir(parents=True, exist_ok=True)
    return root


class UsbDriveToolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.tmp = Path(self.tmp_dir.name)
        self.drive = self.tmp / "DRIVE"
        self.drive.mkdir()

    def tearDown(self) -> None:
        self.tmp_dir.cleanup()

    def test_put_list_check_remove(self) -> None:
        src = dump(self.tmp / "CUSA12345/app0", "CUSA12345")
        prof = profiles.load("CUSA12345")
        info = usb_drive.put_title(src, self.drive, "CUSA12345", "G12345", prof, log=lambda *a: None)
        self.assertEqual((info["copied"], info["kept"], info["removed"]), (5, 0, 0))
        folder = self.drive / "ghostldr/CUSA12345"
        for rel in ("eboot.bin", "sce_module/libc.prx", "data/a/b.bin", "data/empty.bin"):
            self.assertEqual((folder / "app0" / rel).read_bytes(), (src / rel).read_bytes())
        self.assertTrue((folder / "app0/data/emptydir").is_dir())
        self.assertEqual((folder / "prefix").read_text(), "G12345\n")
        self.assertTrue(info["meta"])
        self.assertEqual(gm.parse((folder / "meta.bin").read_bytes())["name"], "Title CUSA12345")
        self.assertEqual(usb_drive.drive_titles(self.drive), {"CUSA12345": folder / "app0"})
        # again: nothing copied; a changed and a removed file followed
        (src / "data/a/b.bin").write_bytes(b"new")
        (src / "data/empty.bin").unlink()
        info = usb_drive.put_title(src, self.drive, "CUSA12345", "G12345", prof, log=lambda *a: None)
        self.assertEqual((info["copied"], info["removed"]), (1, 1))
        self.assertEqual((folder / "app0/data/a/b.bin").read_bytes(), b"new")
        self.assertFalse((folder / "app0/data/empty.bin").exists())
        with mock.patch.object(usb_drive, "fs_type", return_value="exfat"):
            rows = usb_drive.check(self.drive)
        self.assertEqual([s for s, _ in rows], ["PASS", "PASS"])
        self.assertIn("saves in G12345SAVE", rows[1][1])
        self.assertTrue(usb_drive.remove_title(self.drive, "CUSA12345"))
        self.assertEqual(usb_drive.drive_titles(self.drive), {})

    def test_fat32_refuses_a_4_gib_file(self) -> None:
        src = dump(self.tmp / "app0", "CUSA12345")
        with open(src / "data/huge.bin", "wb") as f:
            f.truncate(4 << 30)                      # sparse
        with mock.patch.object(usb_drive, "fs_type", return_value="msdos"):
            with self.assertRaisesRegex(usb_drive.DriveError, "FAT32 holds no file of 4 GiB"):
                usb_drive.put_title(src, self.drive, "CUSA12345", "G12345", profiles.load(None))
        self.assertFalse((self.drive / "ghostldr").exists())

    def test_gfs_pack_usb(self) -> None:
        src = dump(self.tmp / "dump", "CUSA54321")
        out = subprocess.run([sys.executable, str(ROOT / "ghostldr/host/gfs_pack.py"), "--src", str(src), "--usb",
                              str(self.drive)], capture_output=True, text=True, timeout=120)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("progress goes to G54321SAVE", out.stdout)
        self.assertIn("checked: 5 file sizes and 3 file(s) byte for byte", out.stdout)
        self.assertIn("python3 ghostldr.py boot CUSA54321", out.stdout)
        self.assertTrue((self.drive / "ghostldr/CUSA54321/app0/eboot.bin").is_file())
        self.assertFalse((ROOT / "build/ghostfs/CUSA54321").exists(), "nothing packed into containers")

    def test_run_py_finds_the_drives_titles(self) -> None:
        import run
        dump(self.drive / "ghostldr/CUSA12345/app0", "CUSA12345")
        dump(self.drive / "ghostldr/CUSA67890/app0", "CUSA67890")
        found = run.usb_titles("usb:/ghostldr", self.drive)
        self.assertEqual(sorted(found), ["CUSA12345", "CUSA67890"])
        self.assertIsNone(run.usb_titles("ghostfs", self.drive))
        self.assertEqual(run.title_dump("CUSA67890", False, found), (found["CUSA67890"], "CUSA67890"))
        self.assertIsNone(run.title_dump(None, False, found)[0])          # two: --title picks
        one = run.usb_titles("usb:/ghostldr/CUSA12345/app0", self.drive)
        self.assertEqual(one, {"CUSA12345": self.drive / "ghostldr/CUSA12345/app0"})
        self.assertEqual(run.title_dump(None, False, one)[1], "CUSA12345")
        bundle = profiles.read_blobs(profiles.boot_bundle(None, (), found))
        self.assertTrue({"CUSA12345", "CUSA67890"} <= {b["title"] for b in bundle})

    def test_a_drive_on_the_console_through_ftp(self) -> None:
        from fake_ftp import FakeFtp
        console = self.tmp / "console"
        (console / "mnt/usb0").mkdir(parents=True)
        (console / "mnt/usb0/user's files.txt").write_bytes(b"keep")      # the drive's own files stay
        # a SELF, which ftpsrv would show decrypted unless its SELF mode is off
        src = dump(self.tmp / "CUSA12345/app0", "CUSA12345", {"data/with space/ü.bin": b"u" * 77,
                                                             "sce_module/libSceFios2.prx": bytes.fromhex("4f153d1d") + b"S" * 900})
        prof = profiles.load("CUSA12345")
        quiet = lambda *a: None  # noqa: E731
        for jobs in (4, 1):                                             # several connections at once, and one
            with FakeFtp(console) as srv:
                url = srv.url("/mnt/usb0")
                info = usb_drive.put_title(src, url, "CUSA12345", "G12345", prof, log=quiet, jobs=jobs)
                self.assertEqual(info["fs"], "unknown over FTP")
                folder = console / "mnt/usb0/ghostldr/CUSA12345"
                for rel, size in usb_drive.tree(src).items():
                    self.assertEqual((folder / "app0" / rel).read_bytes(), (src / rel).read_bytes(), rel)
                self.assertTrue((folder / "app0/data/emptydir").is_dir())
                self.assertEqual((folder / "prefix").read_text(), "G12345\n")
                self.assertEqual(gm.parse((folder / "meta.bin").read_bytes())["name"], "Title CUSA12345")
                self.assertFalse(list(folder.rglob("*.part")))
                # again: all kept; then a changed and a removed file followed
                info = usb_drive.put_title(src, url, "CUSA12345", "G12345", prof, log=quiet, jobs=jobs)
                self.assertEqual((info["copied"], info["kept"], info["removed"]), (0, 7, 0))
                # a file cut short by a broken copy (written in place) is copied again
                eboot = folder / "app0/eboot.bin"
                eboot.write_bytes(eboot.read_bytes()[:100])
                info = usb_drive.put_title(src, url, "CUSA12345", "G12345", prof, log=quiet, jobs=jobs)
                self.assertEqual((info["copied"], info["kept"]), (1, 6))
                self.assertEqual(eboot.read_bytes(), (src / "eboot.bin").read_bytes())
                (src / "data/a/b.bin").write_bytes(b"newer and longer")
                (src / "data/empty.bin").unlink()
                info = usb_drive.put_title(src, url, "CUSA12345", "G12345", prof, log=quiet)
                self.assertEqual((info["copied"], info["removed"]), (1, 1))
                self.assertEqual((folder / "app0/data/a/b.bin").read_bytes(), b"newer and longer")
                d = usb_drive.open_drive(url)
                self.assertEqual(d.titles(), {"CUSA12345": "/mnt/usb0/ghostldr/CUSA12345/app0"})
                d.close()
                rows = usb_drive.check(url)
                self.assertEqual([r[0] for r in rows], ["WARN", "PASS"])
                self.assertIn("saves in G12345SAVE", rows[1][1])
                self.assertTrue(usb_drive.remove_title(url, "CUSA12345"))
                self.assertFalse((console / "mnt/usb0/ghostldr/CUSA12345").exists())
                self.assertEqual((console / "mnt/usb0/user's files.txt").read_bytes(), b"keep")
                (src / "data/empty.bin").write_bytes(b"")
                (src / "data/a/b.bin").write_bytes(bytes(range(256)) * 40)
        with self.assertRaisesRegex(usb_drive.DriveError, "no such folder on the console"):
            with FakeFtp(console) as srv:
                usb_drive.put_title(src, srv.url("/mnt/usb5"), "CUSA12345", "G12345", prof, log=quiet)

    def test_ftp_copies_many_files_at_once(self) -> None:
        from fake_ftp import FakeFtp
        console = self.tmp / "console"
        (console / "mnt/usb0").mkdir(parents=True)
        many = {f"data/d{i % 17}/sub{i % 3}/f{i}.bin": bytes([i % 251]) * (i * 37 % 5000) for i in range(300)}
        src = dump(self.tmp / "CUSA12345/app0", "CUSA12345", many)
        prof = profiles.load("CUSA12345")
        with FakeFtp(console) as srv:
            info = usb_drive.put_title(src, srv.url("/mnt/usb0"), "CUSA12345", "G12345", prof,
                                       log=lambda *a: None, jobs=8)
        self.assertEqual(info["copied"], len(usb_drive.tree(src)))
        app0 = console / "mnt/usb0/ghostldr/CUSA12345/app0"
        for rel in usb_drive.tree(src):
            self.assertEqual((app0 / rel).read_bytes(), (src / rel).read_bytes(), rel)
        self.assertFalse(list(app0.rglob("*.part")))

    def test_gfs_pack_usb_through_ftp(self) -> None:
        from fake_ftp import FakeFtp
        console = self.tmp / "console"
        (console / "mnt/usb1").mkdir(parents=True)
        src = dump(self.tmp / "dump", "CUSA54321")
        with FakeFtp(console) as srv:
            out = subprocess.run([sys.executable, str(ROOT / "ghostldr/host/gfs_pack.py"), "--src", str(src), "--usb",
                                  srv.url("/mnt/usb1")], capture_output=True, text=True, timeout=120)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("in /mnt/usb1/ghostldr/CUSA54321 (unknown over FTP)", out.stdout)
        self.assertTrue((console / "mnt/usb1/ghostldr/CUSA54321/app0/eboot.bin").is_file())

    @unittest.skipUnless(have_disk_tools(), "needs macOS's hdiutil and diskutil")
    def test_a_prepared_drive_reads_back_on_the_console_stack(self) -> None:
        exe = self.tmp / "usb_host"
        build(exe)
        src = dump(self.tmp / "dump", "CUSA12345", {"data/Ünïcödé.bin": b"u" * 999, "movies/m.bin": b"m" * 300_000})
        img = self.tmp / "prepared.img"
        with Disk(img, 48 << 20, "ExFAT", "GPT", f"GLPREP{os.getpid() % 1000:03d}") as d:
            info = usb_drive.put_title(src, d.mount, "CUSA12345", "G12345", profiles.load("CUSA12345"),
                                       log=lambda *a: None)
            self.assertEqual(info["fs"], "exfat")
        out = subprocess.run([str(exe), str(img), "walk"], capture_output=True, timeout=120).stdout.decode()
        got = {k: v for k, v in parse_walk(out).items() if k.split("/")[0] not in SKIP_NAMES}
        want = {nfc(f"ghostldr/CUSA12345/app0/{p.relative_to(src).as_posix()}"): p for p in src.rglob("*")}
        for rel, p in want.items():
            self.assertIn(rel, got)
            if p.is_file():
                self.assertEqual(got[rel]["crc"], f"{zlib.crc32(p.read_bytes()):08x}", rel)
        self.assertIn("ghostldr/CUSA12345/prefix", got)
        self.assertIn("ghostldr/CUSA12345/meta.bin", got)
        self.assertFalse([k for k in got if "/._" in k or k.startswith("._")])


# ---- a little exFAT, independently of the C code, to damage and extend images ----

def exfat_layout(img: Path, start: int) -> dict:
    with open(img, "rb") as f:
        f.seek(start)
        bs = f.read(512)
    assert bs[3:11] == b"EXFAT   "
    bps = 1 << bs[108]
    cs = bps << bs[109]
    return {"start": start, "bps": bps, "cs": cs, "fat": start + struct.unpack_from("<I", bs, 80)[0] * bps,
            "heap": start + struct.unpack_from("<I", bs, 88)[0] * bps, "root": struct.unpack_from("<I", bs, 96)[0]}


def _chain(f, lay: dict, first: int) -> list[int]:
    out, cl = [], first
    while 2 <= cl < 0xFFFFFFF7 and len(out) < 1 << 16:
        out.append(cl)
        f.seek(lay["fat"] + 4 * cl)
        cl = struct.unpack("<I", f.read(4))[0]
    return out


def _dir_bytes(f, lay: dict, first: int, contig: bool, size: int | None) -> tuple[bytes, list[int]]:
    """A directory's bytes and the image offset of each of its clusters."""
    if contig:
        clusters = list(range(first, first + (size + lay["cs"] - 1) // lay["cs"]))
    else:
        clusters = _chain(f, lay, first)
    data = b""
    offs = []
    for cl in clusters:
        off = lay["heap"] + (cl - 2) * lay["cs"]
        f.seek(off)
        data += f.read(lay["cs"])
        offs.append(off)
    return data, offs


def find_entry(img: Path, lay: dict, rel: str) -> dict:
    """The entry set of rel: where its entries are in the image, its stream fields."""
    with open(img, "rb") as f:
        first, contig, size = lay["root"], False, None
        for i, part in enumerate(rel.split("/")):
            data, offs = _dir_bytes(f, lay, first, contig, size)
            found = None
            for pos in range(0, len(data), 32):
                e = data[pos:pos + 32]
                if e[0] == 0:
                    break
                if e[0] != 0x85:
                    continue
                sc = e[1]
                st = data[pos + 32:pos + 64]
                n = st[3]
                name = b"".join(data[pos + 64 + 32 * k + 2:pos + 64 + 32 * k + 32] for k in range(sc - 1))
                if nfc(name[:2 * n].decode("utf-16-le")) == nfc(part):
                    def at(p: int) -> int:
                        return offs[p // lay["cs"]] + p % lay["cs"]
                    found = {"file_off": at(pos), "stream_off": at(pos + 32), "name_off": at(pos + 64),
                             "set": [at(pos + 32 * k) for k in range(sc + 1)], "flags": st[1],
                             "contig": bool(st[1] & 2), "valid": struct.unpack_from("<Q", st, 8)[0],
                             "first": struct.unpack_from("<I", st, 20)[0], "size": struct.unpack_from("<Q", st, 24)[0]}
                    break
            if not found:
                raise KeyError(rel)
            first, contig, size = found["first"], found["contig"], found["size"]
        return found


def set_valid_length(img: Path, lay: dict, ent: dict, valid: int) -> None:
    """Rewrite a stream's ValidDataLength and its set's checksum."""
    with open(img, "r+b") as f:
        f.seek(ent["stream_off"] + 8)
        f.write(struct.pack("<Q", valid))
        entries = []
        for off in ent["set"]:
            f.seek(off)
            entries.append(bytearray(f.read(32)))
        c = 0
        for i, e in enumerate(entries):
            for k, byte in enumerate(e):
                if i == 0 and k in (2, 3):
                    continue
                c = (((c & 1) << 15) + (c >> 1) + byte) & 0xFFFF
        f.seek(ent["set"][0] + 2)
        f.write(struct.pack("<H", c))


if __name__ == "__main__":
    unittest.main()
