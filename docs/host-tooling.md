# Host tooling

## `ghostldr.py`

The everyday commands, in the repository's (and the release's) top folder.
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
| `clear-cache` | deletes the bundle the release's `ghostldr.lua` keeps in SWRR's saved data (`GLDRCACHE`), so it asks for one again ([using-the-release.md](using-the-release.md#later-sessions-ghostldrlua-alone)) | `run.py clear-cache` |
| `install DUMP` | installs a game into SWRR's saved data, for a console without a drive | `gfs_pack.py`, `gfs_install.py` |

DRIVE is a drive mounted on this computer (default: the only exFAT or FAT
one), `ps5` (the drive in the PS5, through ghostldr's FTP server while a game
runs) or an `ftp://` address. Options `boot`, `logs`, `doctor` and `install`
don't know are passed on to `run.py` and `gfs_install.py`, so every advanced
and developer flag stays available (`python3 ghostldr/host/run.py --help`).
A release uses its prebuilt core; a git checkout builds its own (`run.py`
runs make), and `boot --build` asks for that anywhere.

The tools in `ghostldr/host` below stay as they were for scripts and the
desktop app; `ghostldr.py` only chooses their options.

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
(no known name) count as resolved. A release tree ships no name
table, so there the check is skipped. `run.py doctor` checks the
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
