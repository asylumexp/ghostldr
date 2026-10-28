# Logs

ghostldr writes everything it does to a log. With a USB drive in the console, each boot's log is a file on the drive, so you can read it on a computer, or attach it to a bug report, without any desktop tools.

## Where

| | |
|---|---|
| **A USB drive in the console** | `ghostldr/logs/NNNN_YYYY-MM-DD_HHMM.log` on the drive: one file per boot, numbered (`0012_2026-10-28_2115.log`). |
| **Always** | `/temp0/ghostldr.log` inside SWRR. Every boot of this console session is in it, and it's wiped when the console restarts. While a game runs, ghostldr's FTP server shows it ([ftp.md](ftp.md)). |
| **A computer listening** | `run.py`'s UDP log, or the connection `ghostldr-ext.bin` was sent on (`nc` prints it). |

Every message on the TV names the log's place ([failures.md](failures.md)).

**Which drive.** The drive the game starts from. For an installed game, or when that drive can't be written, the first drive where `ghostldr/logs` can be made. ghostldr writes nothing outside the drive's `ghostldr` folder, and never changes or removes your files. The only files it removes are its own logs in `ghostldr/logs` past the 30 newest boots.

**Turning it off.** In the game selector, Settings, Logs, "Write logs to the USB drive" (on by default). It's kept in ghostldr's settings in SWRR's saved data (`GLDRCONF`), applies to every drive, and takes effect for the game being started. `/temp0` keeps the log either way. Inside ghostldr it's context flag `CTX_FLAG_NO_USB_LOG` (bit 24), which `ghostldr.lua` sets from the settings and the selector sets at once.

**On a PS4** the log stays in `/temp0`: SWRR can't mount a drive there (libSceUsbStorage's mapping call isn't found on 12.50).

**The release's boot in two calls** (the selector beside the ELF loader, then the game): the drive's file starts with the game's call; the selector's part is in `/temp0`.

## What's in it

- Everything the core and the extension log, from the first line of the boot.
- The game's own output: what it prints to stdout and stderr (homebrew), as `ghost1:` and `ghost2:` lines.
- Crash reports: the signal, the address as module+offset, the registers, the call stack.
- The failure that stopped a boot, and the message shown for it (`failure N (code): headline`).

## How it works

- **From the first line.** The core writes each line to `/temp0/ghostldr.log` as it goes (`src/log.c`), with one `write` per line, so a crash loses none. Once the extension's code is placed it takes over the files (`ext/src/logfile.c`): `/temp0`'s file and the drive's.
- **The drive's file** is attached when the extension starts (after the game selector, so a change in Settings applies at once), before a failed boot's message, and again after the drive came back from being pulled out or from rest mode. Each time, it first copies what `/temp0` has of this boot that the drive's file doesn't, so the file is the whole boot whenever it was attached.
- **Kept to disk.** The drive's file is synced (`fsync`) once a second while lines come, before every failure's message, after a crash report and before SWRR closes.
- **Size.** `/temp0`'s file at 8 MiB and a drive's at 32 MiB are renamed to `.1.log` (replacing the one before) and start again, so the newest lines are always there. A boot starts `/temp0` afresh when earlier boots' lines fill half of it.
- **The drive went away.** A write that fails closes the drive's file at once; `/temp0` goes on, and the drive's file catches up when the drive is back.
- **No drive, or logging to USB off:** `/temp0` only.
