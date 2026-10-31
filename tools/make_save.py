#!/usr/bin/env python3
"""The autoloader's save: a decrypted Luac0re save with ghostldr in it, for Apollo Save Tool.

    python3 tools/make_save.py --from SAVE [--region us|eu|both] [--keystone-us FILE]
                               [--keystone-eu FILE] [--encrypted-us ZIP] [--encrypted-eu ZIP]
                               [--out DIR] [--version VERSION] [--no-make]

SAVE is a decrypted SWRR save that Luac0re 2.4b runs from (Luac0re's
release's savedata folder, or what Apollo's "Copy save game to USB" or
"Export save game to Zip" makes of one): a folder holding VMC0.card,
VMC1.card, lua/ and sce_sys/, or a zip with such a folder in it.  Into a
copy of it go (docs/autoload.md):

    lua/auto.lua                 ghostldr/lua/auto.lua: Luac0re runs it at
                                 OPTIONS > HALL OF FAME instead of its loader
    ghostldr/ghostldr.lua        from build/ghostldr (the release's)
    ghostldr/ghostldr-ext.bin    from build/ghostldr

and its sce_sys/param.sfo is made anyone's: ACCOUNT_ID, the PARAMS user ID,
console PSID HMAC and times zeroed (Apollo and Save Wizard write the
importer's), SAVEDATA_BLOCKS grown to hold the files, and its name and
subtitle say what it is.  For the other region
than SAVE's, TITLE_ID, SAVEDATA_DIRECTORY, PARAMS' title IDs, the
disc-swap config and the keystone change too (the keystone is that game's:
--keystone-us / --keystone-eu, sce_sys/keystone of a save of that game);
VMC0.card must hold that region's PS2 exploit save already (Luac0re's has
both).

Out (default build/release), one zip per region, both saves of that region
in it, laid out for the root of a USB drive, and a README.txt:

    ghostldr-<version>-save-US-CUSA03474.zip
        PS4/APOLLO/00000000_CUSA03474_SLUS-20268/...          decrypted, for Apollo
        PS4/SAVEDATA/0000000000000000/CUSA03474/SLUS-20268    encrypted, for Save Wizard
    ghostldr-<version>-save-EU-CUSA03492.zip                  the same for SLES-50366

The encrypted save is made on a console from the decrypted one
(ghostldr/host/resign_save.py --anyone --save; docs/autoload.md, "Making the
saves") and given here with --encrypted-us / --encrypted-eu: the zip
resign_save.py writes.  It must be no one's (account 0000000000000000).
Without it a region's zip holds the decrypted save alone, and this says so.
Nothing of this computer goes in (make_release.py's check, on every file but
the encrypted image).
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_release  # noqa: E402

REGIONS = {
    "us": {"title": "CUSA03474", "dir": "SLUS-20268", "ps2": "BASLUS-20268", "label": "US"},
    "eu": {"title": "CUSA03492", "dir": "SLES-50366", "ps2": "BESLES-50366", "label": "EU"},
}
NO_ONE = "0000000000000000"         # the encrypted save's account folder
BLOCK = 32768
HEADROOM = 128          # blocks over the files' own: sce_sys and the file system (586 left ~8 free for Luac0re's)
ADDED = {"lua/auto.lua": REPO / "ghostldr/lua/auto.lua",
         "ghostldr/ghostldr.lua": REPO / "build/ghostldr/ghostldr.lua",
         "ghostldr/ghostldr-ext.bin": REPO / "build/ghostldr/ghostldr-ext.bin"}
NEEDED = ("VMC0.card", "VMC1.card", "sce_sys/param.sfo", "sce_sys/keystone")
AUTO_CALL = b"/savedata0/lua/auto.lua"


def die(msg: str) -> None:
    sys.exit(f"make_save: {msg}")


def read_save(src: Path) -> dict[str, bytes]:
    """The save's files by path inside it (the folder holding VMC0.card)."""
    if src.is_dir():
        found = [p.parent for p in src.rglob("VMC0.card")]
        if len(found) != 1:
            die(f"{src}: expected one save (one VMC0.card), found {len(found)}")
        root = found[0]
        return {p.relative_to(root).as_posix(): p.read_bytes() for p in sorted(root.rglob("*"))
                if p.is_file() and not p.name.startswith(".")}
    with zipfile.ZipFile(src) as z:
        names = [n for n in z.namelist() if n.endswith("VMC0.card")]
        if len(names) != 1:
            die(f"{src}: expected one save (one VMC0.card) in the zip, found {len(names)}")
        prefix = names[0][:-len("VMC0.card")]
        return {n[len(prefix):]: z.read(n) for n in sorted(z.namelist())
                if n.startswith(prefix) and not n.endswith("/") and not Path(n).name.startswith(".")}


def sfo_entries(data: bytes) -> dict[str, tuple[int, int, int, int]]:
    """param.sfo's keys: (index entry offset, data offset, length, max length)."""
    if data[:4] != b"\0PSF":
        die("sce_sys/param.sfo is not a param.sfo")
    kt, dt, n = struct.unpack_from("<III", data, 8)
    out = {}
    for i in range(n):
        ko, _, ln, mx, do = struct.unpack_from("<HHIII", data, 20 + 16 * i)
        out[data[kt + ko:data.index(0, kt + ko)].decode()] = (20 + 16 * i, dt + do, ln, mx)
    return out


def sfo_str(data: bytes, key: str) -> str:
    _, off, ln, _ = sfo_entries(data)[key]
    return data[off:off + ln].rstrip(b"\0").decode()


def make_anyones(sfo: bytes, blocks: int, region: dict | None = None) -> bytes:
    """param.sfo with no account, user or console in it, SAVEDATA_BLOCKS = blocks, and (region) relabelled."""
    out = bytearray(sfo)
    e = sfo_entries(sfo)
    for key in ("ACCOUNT_ID", "SAVEDATA_BLOCKS", "PARAMS", "TITLE_ID", "SAVEDATA_DIRECTORY"):
        if key not in e:
            die(f"sce_sys/param.sfo has no {key}")
    _, off, ln, _ = e["ACCOUNT_ID"]
    out[off:off + ln] = bytes(ln)
    _, off, ln, _ = e["SAVEDATA_BLOCKS"]
    struct.pack_into("<Q", out, off, blocks)
    _, params, ln, _ = e["PARAMS"]
    out[params + 4:params + 0x28] = bytes(0x24)       # the user ID, then HMAC-SHA256 of the console's PSID
    if ln >= 0x68:
        out[params + 0x60:params + 0x68] = bytes(8)   # the times the system wrote at a copy or a save
    names = (("MAINTITLE", "Star Wars\u2122: Racer Revenge\u2122"), ("SUBTITLE", "Luac0re + ghostldr autoloader"))
    for key, value in names:
        if key in e and len(value.encode()) < e[key][3]:
            ent, off, _, mx = e[key]
            out[off:off + mx] = (value.encode() + b"\0").ljust(mx, b"\0")
            struct.pack_into("<I", out, ent + 4, len(value.encode()) + 1)
    if region:
        for off_t in (0x2C, 0x3C):
            out[params + off_t:params + off_t + 16] = region["title"].encode().ljust(16, b"\0")
        for key, value in (("TITLE_ID", region["title"]), ("SAVEDATA_DIRECTORY", region["dir"])):
            ent, off, _, mx = e[key]
            out[off:off + mx] = (value.encode() + b"\0").ljust(mx, b"\0")
            struct.pack_into("<I", out, ent + 4, len(value) + 1)
    return bytes(out)


def blocks_for(files: dict[str, bytes]) -> int:
    """SAVEDATA_BLOCKS for files: each file's 32 KiB blocks, one per folder, the headroom; a multiple of 32."""
    folders = {str(Path(n).parent) for n in files} - {"."}
    n = sum(max(1, -(-len(d) // BLOCK)) for d in files.values()) + len(folders) + HEADROOM
    return -(-n // 32) * 32


def region_of(sfo: bytes) -> str:
    title = sfo_str(sfo, "TITLE_ID")
    for name, r in REGIONS.items():
        if r["title"] == title:
            return name
    die(f"the save is {title}'s, not SWRR's (CUSA03474 or CUSA03492)")


def build(save: dict[str, bytes], region: str, keystone: bytes | None, added: dict[str, bytes]) -> dict[str, bytes]:
    """The save's files for region, with ghostldr added."""
    missing = [n for n in NEEDED if n not in save]
    if missing:
        die(f"the save lacks {', '.join(missing)} (a decrypted Luac0re save has them)")
    if not any(AUTO_CALL in d for n, d in save.items() if n.startswith("lua/") and n.endswith(".lua")):
        die("the save's Luac0re does not run lua/auto.lua (Luac0re 2.4b's main.lua does): use Luac0re 2.4b's save")
    r = REGIONS[region]
    own = region_of(save["sce_sys/param.sfo"])
    files = {n: d for n, d in save.items() if n not in added}
    if region != own:
        if not keystone:
            die(f"a {r['title']} save needs that game's keystone (--keystone-{region}): this save is "
                f"{REGIONS[own]['title']}'s")
        files["sce_sys/keystone"] = keystone
        if "disc-swap-cli.conf" in files:
            files["disc-swap-cli.conf"] = files["disc-swap-cli.conf"].replace(REGIONS[own]["dir"].encode(),
                                                                                r["dir"].encode())
    if keystone and keystone[:8] != b"keystone":
        die("the keystone given is not one (it starts with 'keystone')")
    if r["ps2"].encode() not in files["VMC0.card"]:
        die(f"VMC0.card has no {r['ps2']} (the PS2 exploit save for {r['title']}): use a card Luac0re made for it")
    files.update(added)
    files["sce_sys/param.sfo"] = make_anyones(save["sce_sys/param.sfo"], 0, r if region != own else None)
    files["sce_sys/param.sfo"] = make_anyones(files["sce_sys/param.sfo"], blocks_for(files))
    return files


def apollo_files(files: dict[str, bytes], region: str) -> dict[str, bytes]:
    """The decrypted save's files where Apollo looks on a USB drive."""
    r = REGIONS[region]
    top = f"PS4/APOLLO/00000000_{r['title']}_{r['dir']}/"
    return {top + n: d for n, d in files.items()}


def encrypted_files(path: Path, region: str) -> dict[str, bytes]:
    """resign_save.py's zip of the encrypted save (image and sealed key), checked: this region's, no one's."""
    r = REGIONS[region]
    top = f"PS4/SAVEDATA/{NO_ONE}/{r['title']}/{r['dir']}"
    with zipfile.ZipFile(path) as z:
        names = sorted(n for n in z.namelist() if not n.endswith("/"))
        if names != [top, top + ".bin"]:
            die(f"{path}: expected {top} and {top}.bin (a {r['title']} save made out to no one: resign_save.py "
                f"--anyone), found {', '.join(names[:4])}")
        image, key = z.read(top), z.read(top + ".bin")
    if key[:8] != b"pfsSKKey" or len(key) != 96:
        die(f"{path}: {top}.bin is not a sealed key")
    if not image or len(image) % BLOCK or image[0x18] != 0:
        die(f"{path}: {top} is not a cleanly committed save image")
    return {top: image, top + ".bin": key}


def readme(version: str, region: str, encrypted: bool) -> bytes:
    """README.txt in a region's zip: which folder is which."""
    r = REGIONS[region]
    text = (f"ghostldr {version}: the autoloader's save for SWRR {r['label']} ({r['title']})\n\n"
            "A Luac0re 2.4b save with ghostldr in it: OPTIONS > HALL OF FAME opens the game\n"
            "selector by itself (hold L1+R1 for Luac0re's loader instead).  Copy the PS4\n"
            "folder to the root of a USB drive.  It belongs to no account: resign it to\n"
            "yours on the way in.\n\n"
            f"  PS4/APOLLO/00000000_{r['title']}_{r['dir']}/\n"
            "      decrypted: Apollo Save Tool, Copy save game to HDD (a jailbroken PS4)\n")
    if encrypted:
        text += (f"  PS4/SAVEDATA/{NO_ONE}/{r['title']}/{r['dir']} and {r['dir']}.bin\n"
                 "      encrypted: resign it to your account with Save Wizard, then import it\n"
                 "      from PS4/SAVEDATA/<your account ID, 16 hex>/" + r['title'] + "/\n"
                 "      (PS5: Saved Data (PS4) > Copy or Delete from USB Drive)\n")
    text += f"\nThe guide: https://github.com/asylumexp/ghostldr/blob/{version}/docs/autoload.md\n"
    return text.encode()


def save_zips(src: Path, version: str, regions: tuple[str, ...] = ("us", "eu"),
              keystones: dict[str, Path | None] | None = None, words: list[str] | None = None,
              encrypted: dict[str, Path | None] | None = None) -> dict[str, bytes]:
    """The zips by name, one per region, checked for personal data (build/ghostldr built)."""
    for p in ADDED.values():
        if not p.is_file():
            die(f"{p} is missing: run make -C ghostldr/native (or leave out --no-make)")
    added = {n: p.read_bytes() for n, p in ADDED.items()}
    save = read_save(src)
    out = {}
    for region in regions:
        r = REGIONS[region]
        key = (keystones or {}).get(region)
        files = build(save, region, key.read_bytes() if key else None, added)
        bad = [b for n in sorted(files) for b in make_release.find_personal(n, files[n], words or [])]
        if bad:
            die("not writing it: personal data in the files:\n  " + "\n  ".join(bad[:30]))
        enc = (encrypted or {}).get(region)
        entries = apollo_files(files, region)
        if enc:
            entries.update(encrypted_files(enc, region))
        else:
            print(f"make_save: no encrypted save for {r['title']} (--encrypted-{region}): its zip holds the "
                  "decrypted save alone", file=sys.stderr)
        entries["README.txt"] = readme(version, region, bool(enc))
        out[f"ghostldr-{version}-save-{r['label']}-{r['title']}.zip"] = make_release.zip_bytes(entries)
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--from", dest="src", type=Path, required=True, help="a decrypted Luac0re save (folder or zip)")
    ap.add_argument("--region", choices=("us", "eu", "both"), default="both")
    ap.add_argument("--keystone-us", type=Path, help="CUSA03474's sce_sys/keystone (for a US save from an EU one)")
    ap.add_argument("--keystone-eu", type=Path, help="CUSA03492's sce_sys/keystone (for an EU save from a US one)")
    ap.add_argument("--encrypted-us", type=Path, help="resign_save.py's zip of the US encrypted save (no one's)")
    ap.add_argument("--encrypted-eu", type=Path, help="resign_save.py's zip of the EU encrypted save (no one's)")
    ap.add_argument("--out", type=Path, default=REPO / "build/release", help="folder for the zips (not emptied)")
    ap.add_argument("--version", default=None, help="the release's version (default: git describe)")
    ap.add_argument("--no-make", action="store_true", help="use build/ghostldr as it is")
    ap.add_argument("--deny", action="append", default=[], help="a word that must not be in the save")
    ap.add_argument("--deny-file", type=Path, help="more such words, one per line")
    args = ap.parse_args(argv)
    version = args.version or make_release.git_version()
    if not args.no_make:
        subprocess.run(["make", "-s", "-C", str(REPO / "ghostldr/native")], check=True)
    words = make_release.personal_words(list(args.deny) + (make_release.read_deny_file(args.deny_file)
                                                           if args.deny_file else []))
    regions = ("us", "eu") if args.region == "both" else (args.region,)
    zips = save_zips(args.src, version, regions, {"us": args.keystone_us, "eu": args.keystone_eu}, words,
                     {"us": args.encrypted_us, "eu": args.encrypted_eu})
    args.out.mkdir(parents=True, exist_ok=True)
    for name, data in zips.items():
        (args.out / name).write_bytes(data)
        print(f"{make_release.sha256(data)}  {len(data):>10}  {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
