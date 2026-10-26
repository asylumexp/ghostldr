#!/usr/bin/env python3
"""ghostldr: play your PS4 game dumps inside Star Wars Racer Revenge on a PS5.

The everyday commands.  Each finds the PS5 by itself and says what to do
next when something is missing.  The tools in ghostldr/host have every
option (run.py --help, gfs_pack.py --help); options this one doesn't know
are passed on to them.
"""

from __future__ import annotations

import argparse
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
HOST = ROOT / "ghostldr/host"
sys.path.insert(0, str(HOST))

EPILOG = """\
The usual way, with a USB drive (exFAT):

  1. python3 ghostldr.py copy /path/to/CUSA11456/app0      copy a game onto the drive
  2. Plug the drive into the PS5.  Start SWRR, click the right half of the
     touchpad at PRESS START, then OPTIONS -> HALL OF FAME.
  3. python3 ghostldr.py boot                              start it (or the selector)

ghostldr.py doctor checks this computer and the PS5; ghostldr.py check says
what the PS5 will find on a drive.  docs/using-the-release.md walks through
it all, docs/usb.md says what goes on the drive.
"""

LOADER_STEPS = ("start SWRR, click the right half of the touchpad at PRESS START, then OPTIONS -> HALL OF FAME, "
                "and run this again. After a game has run, close SWRR and start it again first.")


class Help(argparse.HelpFormatter):
    """Paragraphs wrapped to the terminal; indented lines and lists kept as written."""

    def __init__(self, prog: str) -> None:
        super().__init__(prog, max_help_position=30, width=min(shutil.get_terminal_size().columns, 100))

    def _fill_text(self, text: str, width: int, indent: str) -> str:
        import textwrap
        out = []
        for para in text.split("\n\n"):
            if any(ln[:1].isspace() or ln[:2].rstrip(".").isdigit() for ln in para.splitlines()):
                out.append(para)
            else:
                out.append(textwrap.fill(" ".join(para.split()), width, initial_indent=indent,
                                         subsequent_indent=indent))
        return "\n\n".join(out)


def fail(msg: str) -> int:
    print(f"ghostldr: {msg}", file=sys.stderr)
    return 1


# ---- the console ----

def console(given: str | None) -> str:
    """The PS5's address: --console's, else found on the network (console.py)."""
    if given:
        os.environ["PS_HOST"] = given
        return given
    import console as c
    return c.console_ip()


def loader_listening(ip: str) -> int:
    import console as c
    return c.loader_listening(ip, timeout=2.0)


def use_prebuilt() -> bool:
    """A release (no git checkout, or no compiler): its prebuilt core; a
    developer's checkout builds its own (run.py runs make)."""
    built = all((ROOT / "build/ghostldr" / n).is_file() for n in ("core.bin", "core.json", "ext.bin"))
    dev = (ROOT / ".git").exists() and shutil.which("make") is not None
    return built and not dev


def run_py(argv: list[str]) -> int:
    import run
    return run.main(argv)


# ---- doctor ----

def cmd_doctor(args, extra: list[str]) -> int:
    rc = run_py(["doctor", *(["--host", args.console] if args.console else []), *extra])
    import usb_drive as ud
    drives = ud.mounted_drives()
    if drives:
        print()
        for d in drives:
            try:
                games = ud.list_drive(ud.LocalDrive(d))
            except OSError:
                continue
            fs = {"exfat": "exFAT", "msdos": "FAT32"}.get(ud.fs_type(d) or "", "file system unknown")
            print(f"USB drive here: {d} ({fs}): " + (", ".join(f"{g.id} {g.name}".strip() for g in games)
                                                    or "no games yet (ghostldr.py copy puts one on)"))
    print()
    print("All set: start a game with  python3 ghostldr.py boot" if rc == 0 else
          "Fix the FAIL lines above, then run  python3 ghostldr.py doctor  again.")
    return rc


# ---- copy ----

def find_dump(path: Path) -> Path:
    """The game's files: the folder given (eboot.bin in it) or its app0."""
    path = path.expanduser()
    if (path / "eboot.bin").is_file():
        return path
    if (path / "app0/eboot.bin").is_file():
        return path / "app0"
    if not path.is_dir():
        raise SystemExit(f"ghostldr: {path}: no such folder")
    raise SystemExit(f"ghostldr: {path} has no eboot.bin: point at the game's folder, the one with eboot.bin, "
                     "sce_sys and sce_module in it (often called app0 or CUSA12345-app)")


def pick_drive(given: str | None, console_ip: str | None) -> str:
    import usb_drive as ud
    if given and given.lower() in ("ps5", "console"):
        ip = console(console_ip)
        return f"ftp://{ip}:1212/ghostldr_usb"
    if given:
        if given.startswith("ftp://") or Path(given).expanduser().is_dir():
            return given if given.startswith("ftp://") else str(Path(given).expanduser())
        raise SystemExit(f"ghostldr: {given}: no such folder (where this computer has the USB drive, like "
                         "/Volumes/MYDRIVE, E:\\ or /media/you/MYDRIVE)")
    drives = ud.mounted_drives()
    d = ud.find_drive(None)
    if d:
        return str(d)
    if not drives:
        raise SystemExit("ghostldr: no USB drive is mounted on this computer. Plug it in (formatted exFAT) and run "
                         "this again, or name it: ghostldr.py copy DUMP /Volumes/MYDRIVE. To copy onto the drive in "
                         "the PS5 while a game runs there: ghostldr.py copy DUMP ps5")
    raise SystemExit("ghostldr: several drives are mounted here; name the one to copy to:\n"
                     + "\n".join(f"  {d}" for d in drives))


class Bar:
    """One progress line, redrawn in place on a terminal."""

    def __init__(self, label: str) -> None:
        self.label, self.t0, self.tty = label, time.time(), sys.stdout.isatty()
        self.last = ""

    def __call__(self, done: int, need: int, count: int, todo: int) -> None:
        if not self.tty:
            return
        frac = done / need if need else 1.0
        width = 28
        fill = int(frac * width)
        spent = max(time.time() - self.t0, 1e-3)
        rate = done / spent
        eta = (need - done) / rate if rate > 0 else 0
        eta_s = f"{int(eta // 60)} min" if eta >= 90 else f"{int(eta)} s"
        line = (f"\r{self.label} [{'#' * fill}{'.' * (width - fill)}] {frac * 100:5.1f}%  "
                f"{done / 2**30:.2f}/{need / 2**30:.2f} GiB  {rate / 2**20:.0f} MiB/s  "
                f"{count}/{todo} files  {('ETA ' + eta_s) if done < need else ''}")
        if line != self.last:
            sys.stdout.write(line.ljust(len(self.last)))
            sys.stdout.flush()
            self.last = line

    def end(self) -> None:
        """Leave the bar's line (before other output, and at the end)."""
        if self.tty and self.last:
            sys.stdout.write("\n")
            sys.stdout.flush()
        self.last = ""


def cmd_copy(args, extra: list[str]) -> int:
    import ftplib

    import gfs_meta as gm
    import gfs_pack
    import profiles
    import usb_drive as ud
    src = find_dump(Path(args.dump))
    title, prefix, prof = gfs_pack.resolve(src, args.title, args.prefix)
    drive = pick_drive(args.drive, args.console)
    files = ud.tree(src)
    total = sum(files.values())
    name = prof.get("name") or profiles.param_sfo(src).get("TITLE") or title
    print(f"{name} ({title}): {len(files):,} files, {total / 2**30:.1f} GiB")
    print(f"  to {drive}" + (f"/{args.folder}" if args.folder else "") + f"; its progress goes to {prefix}SAVE")
    bar = Bar(title)

    def log(line: str) -> None:
        if bar.tty and line.startswith(f"{title}: ") and "MiB/s" in line:
            return                       # a progress line: the bar shows it
        bar.end()
        print(line, flush=True)

    try:
        info = ud.put_title(src, drive, title, prefix, prof, args.name, args.icon, args.backdrop, args.fetch_art,
                            None, False, log=log, jobs=args.jobs, folder=args.folder, on_progress=bar,
                            verify=args.verify)
    except (ud.DriveError, gm.ArtError) + ftplib.all_errors as e:
        bar.end()
        return fail(str(e))
    except KeyboardInterrupt:
        bar.end()
        return fail("stopped: run the same command again to carry on where it stopped")
    bar.end()
    print(f"Done: {info['copied']:,} files copied, {info['kept']:,} already there, {info['removed']} removed, "
          f"in {info['folder']}")
    print(f"Checked: {info['verified']}")
    import exec_fit
    fit, line = exec_fit.check(src, title)
    if fit and fit.get("verdict") == exec_fit.NO_FIT:
        print(line)
    print()
    if drive.startswith("ftp://"):
        print(f"Next: restart SWRR and its loader, then  python3 ghostldr.py boot {title}")
    else:
        print("Next: eject the drive, plug it into the PS5, start SWRR's loader (HALL OF FAME), then\n"
              f"  python3 ghostldr.py boot {title}")
    return 0


# ---- check ----

def cmd_check(args, extra: list[str]) -> int:
    import usb_drive as ud
    if args.drive:
        targets = [pick_drive(args.drive, args.console)]
    else:
        targets = [str(d) for d in ud.mounted_drives()]
        if not targets:
            return fail("no USB drive is mounted on this computer: plug it in, or name it (ghostldr.py check "
                        "/Volumes/MYDRIVE), or check the one in the PS5 while a game runs: ghostldr.py check ps5")
    rc = 0
    for i, t in enumerate(targets):
        if i:
            print("\n" + "-" * 60 + "\n")
        try:
            r = ud.inspect(t, sizes=args.sizes or None)
        except ud.DriveError as e:
            rc = fail(str(e))
            continue
        print(ud.format_report(r))
        rc |= 0 if r.ok else 1
    return rc


# ---- boot ----

def known_games() -> dict[str, str]:
    """{title ID: name} this computer knows: profiles, packs, a mounted drive's games."""
    import profiles
    import usb_drive as ud
    out = {}
    for t in profiles.known_titles():
        try:
            out[t] = profiles.load(t).get("name", "") or ""
        except profiles.ProfileError:
            pass
    for f in profiles.TITLES_OUT.glob("*/layout.json"):
        out.setdefault(f.parent.name, "")
    for d in ud.mounted_drives():
        try:
            for g in ud.list_drive(ud.LocalDrive(d)):
                out[g.id] = g.name or out.get(g.id, "")
        except OSError:
            continue
    return out


def game_id(game: str) -> str:
    import profiles
    t = profiles.normalize_title(game)
    if t and (len(t) == 9 or t in known_games()):
        return t
    want = game.lower()
    hits = {t: n for t, n in known_games().items() if want in n.lower()}
    if len(hits) == 1:
        t, n = hits.popitem()
        print(f"{game!r}: {n} ({t})")
        return t
    if hits:
        raise SystemExit(f"ghostldr: {game!r} matches several games; give its title ID:\n"
                         + "\n".join(f"  {t}  {n}" for t, n in sorted(hits.items())))
    if t:
        return t
    raise SystemExit(f"ghostldr: no game called {game!r} is known here: give its title ID (like CUSA11456; "
                     "ghostldr.py check lists a drive's games)")


def cmd_boot(args, extra: list[str]) -> int:
    root = {"auto": "auto", "usb": "usb:/", "installed": "ghostfs"}[args.source]
    if args.folder:
        root = "usb:/" + args.folder.strip("/")
    argv = ["boot", "--app-root", root]
    if args.game:
        argv += ["--title", game_id(args.game)]
    if args.select:
        argv.append("--select")
    if not args.build and use_prebuilt():
        argv.append("--no-build")
    ip = console(args.console)
    if not loader_listening(ip):
        return fail(f"the PS5 at {ip} isn't listening for ghostldr: {LOADER_STEPS}")
    argv += ["--host", ip, *extra]
    where = {"auto": "a USB drive in the PS5, else the installed games", "usb": "the USB drive in the PS5",
             "installed": "the games installed in SWRR's saved data"}[args.source]
    print(f"Starting {args.game or 'a game'} from {where}" + (" (the game selector shows them all)"
                                                                if args.select else "") + f" on {ip}")
    return run_py(argv)


# ---- logs ----

def cmd_logs(args, extra: list[str]) -> int:
    log = ROOT / "build/ghostldr/run.log"
    if args.saved:
        if not log.is_file():
            return fail("no saved log yet: it is written while ghostldr.py boot runs")
        lines = log.read_text(errors="replace").splitlines()
        print("\n".join(lines[-args.saved:]))
        print(f"\n(the whole log: {log})")
        return 0
    print("Listening for the PS5's log; press Ctrl+C to stop (the game keeps running). Also saved to " + str(log))
    argv = ["logs", "--wait", str(args.wait or 10 ** 9), *(["--host", args.console] if args.console else []), *extra]
    return run_py(argv)


# ---- elfldr ----

def cmd_elfldr(args, extra: list[str]) -> int:
    ip = console(args.console)
    if args.payload:     # to the loader already running: its output, and its exit code
        argv = [sys.executable, str(HOST / "elf_send.py"), str(Path(args.payload).expanduser()), "--host", ip,
                *extra]
        return subprocess.run(argv).returncode
    if not loader_listening(ip):
        return fail(f"the PS5 at {ip} isn't listening for ghostldr: {LOADER_STEPS}")
    argv = ["elfldr", "--host", ip, *([] if args.build or not use_prebuilt() else ["--no-build"]), *extra]
    print(f"Starting the ELF loader on {ip} (no game). Then: python3 ghostldr.py elfldr payload.elf")
    return run_py(argv)


# ---- install ----

def cmd_install(args, extra: list[str]) -> int:
    import gfs_pack
    src = find_dump(Path(args.dump))
    title, _, _ = gfs_pack.resolve(src, args.title, None)
    env = dict(os.environ, **({"PS_HOST": args.console} if args.console else {}))
    py = sys.executable
    print(f"Preparing {title} for SWRR's saved data (on this computer)...", flush=True)
    rc = subprocess.run([py, str(HOST / "gfs_pack.py"), "--src", str(src), "--title", title], env=env).returncode
    if rc:
        return rc
    ip = console(args.console)
    if not loader_listening(ip):
        return fail(f"packed, but the PS5 at {ip} isn't listening for ghostldr: {LOADER_STEPS}")
    print(f"Installing {title} on the PS5 at {ip} (Ghost takes about 40 minutes; run this again to carry on if "
          "it stops)...", flush=True)
    return subprocess.run([py, str(HOST / "gfs_install.py"), "--title", title, "--host", ip, *extra],
                          env=env).returncode


# ---- the command line ----

def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="ghostldr.py", formatter_class=Help, epilog=EPILOG,
        description="Play your PS4 game dumps inside Star Wars Racer Revenge on a PS5 (no jailbreak).")
    ap.add_argument("--console", metavar="IP", help="the PS5's address (default: found on the network)")
    sub = ap.add_subparsers(dest="cmd", metavar="COMMAND", title="commands")

    def add(name: str, help_: str, desc: str, epilog: str = "") -> argparse.ArgumentParser:
        p = sub.add_parser(name, help=help_, description=desc, epilog=epilog, formatter_class=Help)
        p.add_argument("--console", metavar="IP", default=argparse.SUPPRESS,
                       help="the PS5's address (default: found on the network)")
        return p

    add("doctor", "check this computer and the PS5 are ready",
        "Check Python, the prebuilt files, this computer's network ports and the PS5's loader, and list the USB "
        "drives mounted here. Start SWRR's loader (HALL OF FAME) first.")

    p = add("copy", "copy a game onto a USB drive",
            "Copy a game dump onto a USB drive, with a progress bar, and check the copy. Run it again after a "
            "stop: it carries on.",
            "DRIVE is where this computer has the drive mounted (/Volumes/MYDRIVE, E:\\, /media/you/MYDRIVE); "
            "default: the only exFAT or FAT drive mounted. \"ps5\" copies onto the drive in the PS5 over its "
            "FTP server while a game runs there (slower).\n\nThe game goes into ghostldr/<TITLE> on the drive "
            "unless --folder says otherwise; any folder name works (docs/usb.md).")
    p.add_argument("dump", metavar="DUMP", help="the game's folder: the one with eboot.bin in it (or its parent)")
    p.add_argument("drive", metavar="DRIVE", nargs="?", help="the USB drive (see below)")
    p.add_argument("--folder", metavar="PATH", help="where on the drive, e.g. \"Games/Ghost of Tsushima\"")
    p.add_argument("--title", metavar="ID", help="the title ID, when the dump doesn't say (no param.sfo)")
    p.add_argument("--name", help="the name the game selector shows, when the dump has none")
    p.add_argument("--fetch-art", action="store_true", help="get the game's icon and background from the "
                   "PlayStation Store for the selector")
    p.add_argument("--verify", choices=["quick", "full", "none"], default="quick",
                   help="after copying: every file's size and the key files' bytes (quick), every byte (full)")
    adv = p.add_argument_group("advanced")
    adv.add_argument("--prefix", help="its save container's prefix (default: its profile's, else G + digits)")
    adv.add_argument("--icon", type=Path, help="a tile picture for the selector")
    adv.add_argument("--backdrop", type=Path, help="a background picture for the selector")
    adv.add_argument("--jobs", type=int, default=8, help="files sent at once over FTP (default 8)")

    p = add("check", "show what the PS5 will find on a USB drive",
            "List the games the PS5 will find on a USB drive, where they are and where their progress goes, and "
            "say what is wrong with any folder it can't start.",
            "DRIVE: as for copy (default: every exFAT or FAT drive mounted here; \"ps5\" for the drive in the "
            "PS5 while a game runs).")
    p.add_argument("drive", metavar="DRIVE", nargs="?", help="the USB drive")
    p.add_argument("--sizes", action="store_true", help="add up each game's files over FTP too (slow)")

    p = add("boot", "start a game on the PS5",
            "Start a game on the PS5. Without GAME: the only game, or the game selector on the TV when there "
            "are several. Games on a USB drive in the PS5 come first, then the ones installed in SWRR's saved "
            "data.",
            "Start SWRR's loader first: click the right half of the touchpad at PRESS START, then OPTIONS -> "
            "HALL OF FAME. The log shows here until Ctrl+C (the game keeps running); it is also saved in "
            "build/ghostldr/run.log.\n\nOther run.py boot options pass through (python3 ghostldr/host/run.py "
            "--help).")
    p.add_argument("game", metavar="GAME", nargs="?", help="title ID (CUSA11456) or part of its name")
    p.add_argument("--select", action="store_true", help="show the game selector even for one game")
    src = p.add_mutually_exclusive_group()
    src.add_argument("--usb", dest="source", action="store_const", const="usb",
                     help="only the USB drive's games")
    src.add_argument("--installed", dest="source", action="store_const", const="installed",
                     help="only the games installed in SWRR's saved data")
    p.add_argument("--folder", metavar="PATH", help="only the games under this folder of the USB drive")
    p.add_argument("--build", action="store_true", help="build the core from source first (developers)")
    p.set_defaults(source="auto")

    p = add("logs", "show the PS5's log",
            "Show the log of the game running on the PS5 (as boot does), or the last saved one.")
    p.add_argument("--saved", type=int, nargs="?", const=60, metavar="LINES",
                   help="print the end of the last saved log instead (default 60 lines)")
    p.add_argument("--wait", type=float, help="stop after this many seconds")

    p = add("elfldr", "run ELF payloads in SWRR, with no game",
            "Without PAYLOAD: start ghostldr's ELF loader in SWRR (no game, no kernel exploit); it takes payloads "
            "on TCP 9021 until it is sent \"quit\". With PAYLOAD: send it to the loader already running and show "
            "its output (docs/elfldr.md).",
            "On a jailbroken console, whose own elfldr has 9021, start it with --elfldr-port 9022 and send with "
            "--port 9022. Other options pass through to run.py elfldr and elf_send.py.")
    p.add_argument("payload", metavar="PAYLOAD", nargs="?", help="an ELF to send to the running loader")
    p.add_argument("--build", action="store_true", help="build the core from source first (developers)")

    p = add("install", "install a game into SWRR's saved data (no USB drive)",
            "Lay the game out for SWRR's saved data, then send it to the PS5 (about 40 minutes for Ghost). "
            "Needs no drive, but loads more slowly than one. Run it again to carry on after a stop.",
            "Other gfs_install.py options pass through (python3 ghostldr/host/gfs_install.py --help).")
    p.add_argument("dump", metavar="DUMP", help="the game's folder: the one with eboot.bin in it (or its parent)")
    p.add_argument("--title", metavar="ID", help="the title ID, when the dump doesn't say (no param.sfo)")
    return ap


def value_options(cmd: str) -> set[str]:
    """The options of the tool `cmd` passes options on to that take a value
    (so "--wait 20" stays together and 20 isn't taken for GAME)."""
    if cmd in ("doctor", "boot", "logs", "elfldr"):
        import run
        opts = {o for a in run.build_parser()._actions if a.nargs != 0 for o in a.option_strings}
    else:
        opts = set()
    if cmd == "elfldr":
        opts |= {"--port", "--cmd", "--klog", "--wait"}            # elf_send.py's
    if cmd == "install":
        opts |= {"--interface", "--port", "--src", "--remove", "--host"}   # gfs_install.py's
    return opts


def split_passthrough(ap: argparse.ArgumentParser, argv: list[str]) -> tuple[list[str], list[str]]:
    """argv without the options for the underlying tool that take a value
    (with their values), and those options."""
    cmd = next((a for a in argv if a in COMMANDS), None)
    if cmd not in PASS_THROUGH:
        return argv, []
    sub = next(a for a in ap._actions if isinstance(a, argparse._SubParsersAction)).choices[cmd]
    mine = {o for a in sub._actions for o in a.option_strings}
    theirs = value_options(cmd) - mine
    kept, held, i = [], [], 0
    while i < len(argv):
        a = argv[i]
        if a in theirs and i + 1 < len(argv):
            held += [a, argv[i + 1]]
            i += 2
            continue
        kept.append(a)
        i += 1
    return kept, held


COMMANDS = {"doctor": cmd_doctor, "copy": cmd_copy, "check": cmd_check, "boot": cmd_boot, "logs": cmd_logs,
            "elfldr": cmd_elfldr, "install": cmd_install}
PASS_THROUGH = {"doctor", "boot", "logs", "elfldr", "install"}


def main(argv: list[str] | None = None) -> int:
    ap = parser()
    argv, held = split_passthrough(ap, list(sys.argv[1:] if argv is None else argv))
    args, extra = ap.parse_known_args(argv)
    extra += held
    if not args.cmd:
        ap.print_help()
        return 0
    if extra and args.cmd not in PASS_THROUGH:
        ap.error(f"{args.cmd}: unknown option(s) {' '.join(extra)} (ghostldr.py {args.cmd} --help)")
    if sys.version_info < (3, 11):
        return fail(f"Python {sys.version_info.major}.{sys.version_info.minor}: ghostldr needs Python 3.11 or newer")
    try:
        return COMMANDS[args.cmd](args, extra) or 0
    except KeyboardInterrupt:
        print()
        return 130
    except socket.timeout as e:
        return fail(f"the PS5 didn't answer in time ({e}): is it on, and on this network?")


if __name__ == "__main__":
    raise SystemExit(main())
