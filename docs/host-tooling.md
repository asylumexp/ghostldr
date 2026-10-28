# Host tooling

The computer-side tools, for developers. They are in the repository, not in a
release: a release needs no Python ([using-the-release.md](using-the-release.md)).

## `ghostldr.py`

The everyday commands, in the repository's top folder.
Each finds the PS5 by itself (`--console ADDRESS` picks one) and, when
something is missing, says what to do next. `python3 ghostldr.py --help`
lists them; `python3 ghostldr.py COMMAND --help` explains one.

| Command | What it does | Runs |
|---|---|---|
| `doctor` | checks this computer and the PS5 are ready, and lists the USB drives mounted here with their games | `run.py doctor` |
| `copy DUMP [DRIVE]` | copies a game onto a USB drive with a progress bar, then checks the copy (`--folder`, `--verify full`, `--fetch-art`) | `usb_drive.put_title`, as `gfs_pack.py --usb` |
| `check [DRIVE]` | what the PS5 will find on a drive: games, their folders and saves, folders it can't start and why | `usb_drive.inspect` |
| `boot [GAME]` | starts a game: a title ID or part of its name, else the only game or the game selector; the USB drive's games first, then the installed ones (`--usb`, `--installed`, `--folder`, `--select`) | `run.py boot` |
| `logs` | the PS5's log while a game runs; `--saved` prints the end of the last one | `run.py logs` |
| `clear-cache` | deletes the bundle the release's `ghostldr.lua` keeps in SWRR's saved data (`GLDRCACHE`), so it asks for one again ([using-the-release.md](using-the-release.md#every-later-session-ghostldrlua-alone)) | `run.py clear-cache` |
| `install DUMP` | installs a game into SWRR's saved data, for a console without a drive | `gfs_pack.py`, `gfs_install.py` |

DRIVE is a drive mounted on this computer (default: the only exFAT or FAT
one), `ps5` (the drive in the PS5, through ghostldr's FTP server while a game
runs) or an `ftp://` address. Options `boot`, `logs`, `doctor` and `install`
don't know are passed on to `run.py` and `gfs_install.py`, so every advanced
and developer flag stays available (`python3 ghostldr/host/run.py --help`).
A checkout builds its own core, extension and selector (`run.py` runs
make).

The tools in `ghostldr/host` below stay as they were for scripts;
`ghostldr.py` only chooses their options.

## Other host-side checks

Four host-side additions to the release flow. All are read-only with
respect to the console except where noted.

### `run.py doctor`

    python3 ghostldr/host/run.py doctor [--host <console-ip>]

Checks the whole boot chain from this computer and prints one
PASS / WARN / FAIL line per item: Python version, prebuilt core
integrity (`core.bin` size vs `core.json`), Lua payload size against
Luac0re's 500 KB cap, whether UDP 18194 / TCP 18195 are free (or
already owned by the recorder), whether the console answers discovery
and its Lua loader answers on 9026, the USB drive mounted here and its
games, and — when a dump and a name table are staged — the title's
import coverage summary. A FAIL line says what to do. Exit code 1 if
anything FAILs; without a console the local checks still run.
Doctor sends nothing to the console beyond a TCP connect probe.

### Boot preflight gate

`run.py boot` now runs `tools/import_report.py` over the local dump
of the title it boots first (only when `build/nid/known_names.txt`
also exists; on trees that cannot run the checker it prints
"skipped" and boots as before). The summary line prints how the
title's functions bind — core-HLE, ext-HLE, title, host — and how
many are UNRESOLVED. Unresolved imports bind to stubs that return 0,
which looks like success to the title, so any unresolved count asks
`Boot anyway? [y/N]` before going on (anything but `y` cancels; with
no terminal to ask on, the boot stops). The full report is written to
`build/ghostldr/preflight.md` (and `.json`). To boot without being
asked:

    python3 ghostldr/host/run.py boot --force-preflight

Which dump: `--title`'s, from where `gfs_pack.py` read it (its
`build/ghostfs/<TITLE>/layout.json`), or `build/ghost/app0` when that
is the title. Without `--title`, the only title with a dump on this
computer. With several, or with `--select` (the console picks), it
says so and skips. Functions the extension implements by NID alone
(no known name) count as resolved. Without the name table
(`tools/fetch_deps.sh`), the check is skipped. `run.py doctor` checks the
same dump.

### `tools/stub_report.py`

    python3 ghostldr/tools/stub_report.py build/ghostldr/run.log [--top N] [--json out.json]

Aggregates the stub lines the core logs at runtime (`UNRESOLVED
call: ...`, `DENIED call: ...`) into a ranked per-function hit
table: totals, distinct stubs, denied hits, and the anonymous
`UNRESOLVED call #<idx>` count. Use it after a session to decide
which HLE functions are worth implementing next — by what the
title actually called, not by what it imports.

### Log hints

When the core's extension fetch/start fails, it logs a terse
`ext: ...` line and the boot stops. run.py's log listener now
follows such lines with a `[hint]` line giving the plain-language
cause and the fix (firewall on TCP 18195, core/ext build mismatch,
corrupt blob, and so on).

## Making a release

    python3 tools/make_release.py --version v1.0.0 [--out DIR] [--no-make] [--deny WORD ...] [--deny-file FILE]

For maintainers. It runs `make -s` in `ghostldr/native` (not with
`--no-make`), then writes into `DIR` (default `build/release`, emptied
first):

| File | From |
|---|---|
| `ghostldr.lua`, `ghostldr-ext.bin`, `ghostldr-elfldr.bin` | `build/ghostldr` (the two-file boot, [architecture.md](architecture.md#two-file-boot)) |
| `manager.lua` | `ghostldr/lua` ([manager.md](manager.md)) |
| `README.txt` | `tools/release-README.txt`, with `@VERSION@` replaced |
| `ghostldr-<version>.zip` | the five above and their `SHA256SUMS.txt`, flat: no folders, no Python, no source |
| `SHA256SUMS.txt` | the five above and the zip |

The release page gets the zip, the five files one by one and the outer
`SHA256SUMS.txt` ([using-the-release.md](using-the-release.md#1-download)).
`ghostldr-dev-payloads-<version>.zip` (jailbroken consoles) stays a separate
asset, made apart from this.

- **Version:** `--version`, normally the release's tag; else `git describe
  --tags --always --dirty`. The README's guide link points at that tag.
- **Reproducible:** the zip's entries are sorted, dated 1980-01-01 with mode
  0644, and deflated at level 9, so the same files give the same bytes.
- **Nothing personal:** before writing anything it checks the five files and
  stops, naming what it found, if one holds a private-network address other
  than the docs' examples (192.168.1.123 and the like), a user folder path
  (`/Users/...`, `/home/...`, `C:\Users\...`; the console's own `/user/home/`
  is fine), this computer's home folder, user name or host name, git's
  `user.email`, or a word given with `--deny` or in `--deny-file` (one per
  line, `#` comments). Keep console names, account IDs and PSN names in such a
  file outside the repository.
- Tests: `ghostldr/tests/test_make_release.py`.
