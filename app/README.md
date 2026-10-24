# The ghostldr app

A desktop app for everything in [using-the-release.md](../docs/using-the-release.md), without a terminal. It is a wizard with a status and one main button per step, along one of two paths you choose at the top:

- **USB drive (recommended):** **Console → Game → USB drive → Play**. The game is copied to an exFAT USB drive, which the PS5 reads it from: no install step, and games load faster ([usb.md](../docs/usb.md)). The drive can be plugged into this computer, or into the PS5 while a game started by ghostldr runs, through its FTP server ([ftp.md](../docs/ftp.md)).
- **Saved data:** **Console → Game → Install → Play**. Each game is installed into SWRR's saved data on the PS5, with no drive ([ghostfs-catalogue.md](../docs/ghostfs-catalogue.md)).

The app remembers the path, and you can switch any time. Besides the wizard there is:

- a **Library** of the games installed on the PS5, on the USB drive and prepared on this computer;
- the **save manager**;
- **Logs & Help**: the game's log, everything the app ran, firewall help and troubleshooting.

It follows the system's light or dark mode.

## Installing it

Each release has an installer per system: see [tools/package](../tools/package/README.md). All but the `.deb` bring their own Python.

| System | File | Then |
|---|---|---|
| macOS 10.15 or later (Apple silicon and Intel) | `ghostldr-<version>-macos-universal.dmg` | Drag **ghostldr** to Applications. |
| Windows 10 or 11 (x64) | `ghostldr-<version>-windows-x64-setup.exe` | Run it: it installs for you only, no administrator needed. |
| Debian 12, Ubuntu 24.04 or later | `ghostldr_<version>_all.deb` | `sudo apt install ./ghostldr_<version>_all.deb` (it uses the system's Python 3.11+). |
| Any Linux (x86-64, ARM64) | `ghostldr-<version>-<arch>.AppImage` | `chmod +x` it and run it. |

They start the same app as below, in your browser.

## Starting it from the release folder

It needs Python 3.11 or newer and nothing else.

| System | Start it | First time |
|---|---|---|
| macOS | Double-click `app/ghostldr.command`. | macOS may refuse to open an unsigned file: right-click it, choose **Open**, then **Open**. |
| Windows | Double-click `app\ghostldr.bat`. | Install Python from python.org with **Add python.exe to PATH** ticked. |
| Linux | Double-click `app/ghostldr.sh` and choose **Run**, or run `./app/ghostldr.sh`. | |

- **Window:** the app opens in your browser, or in its own window if [pywebview](https://pywebview.flowrl.com) is installed (`python3 -m pip install pywebview`).
- **Keep the terminal window open:** closing it stops the app.
- **Starting it again** while it runs opens the running app.
- **Without the launchers:** `python3 app/launch.py`, with `--browser` to skip pywebview or `--no-open` to only print the address.
- **Folder:** keep `app/` inside the unpacked release. The app uses the release's `ghostldr/` tools and its prebuilt `build/ghostldr` files.
- **Read-only copies:** the scripts write `build/` next to themselves, so where the release can't be written (the installers' copies, or with `--workspace`), the app runs them from a copy in your data folder: see [What it stores](#what-it-stores).

## How it works

- **The scripts:** the app runs the same scripts as the command line, with the Python that started it, and reads what they print. It doesn't reimplement them.
  - `console.py --list`
  - `run.py doctor`
  - `gfs_pack.py`
  - `gfs_install.py`, including `--list`, `--remove` and `--update-meta`
  - `gfs_pack.py --usb DRIVE` and `usb_drive.py list` and `remove`, where DRIVE is the drive's mount point here or `ftp://PS5:1212/ghostldr_usb`
  - `run.py boot --app-root ghostfs --no-build`, or `--app-root usb:/ghostldr` on the USB path
  - `manager.py --detach` and `--exit`
  - `import_report.py` and `stub_report.py`
- **Raw output:** each script's full output is under **Details**, and saved in the logs folder.
- **One at a time:** only one job uses the console's loader at once, and one the USB drive. A copy to a drive in the PS5 runs alongside a game, which keeps the FTP server up. **Pause** and **Stop** send the script Ctrl+C.
- **The code:**
  - `launch.py` starts the server;
  - `ghostapp/server.py` serves the page and a small JSON API;
  - `ghostapp/service.py` holds what each button does;
  - `jobs.py` runs the scripts in the background;
  - `parsers.py` and `trackers.py` read their output;
  - `problems.py` turns known failures into fixes;
  - `settings.py` keeps the settings;
  - `picker.py` shows the system's folder dialog;
  - `web/` is the page, with no build step and nothing from the internet.

## Safety

- **Local only:** the server listens on 127.0.0.1, on a free port picked at launch.
- **A key per launch:**
  - Every API call must carry that launch's random token.
  - Every request must name the server in its Host header, which stops DNS rebinding.
  - POSTs must be JSON, and there is no CORS.
  - So other web pages can't drive it.
- **No shell:** scripts run from argument lists, never through a shell.

## What it stores

The last game folder, the chosen console, the path and the USB drive, the games it copied to a drive, and the logs of each run. They are kept in your user folder, never in the app folder:

| System | Folder |
|---|---|
| macOS | `~/Library/Application Support/ghostldr` |
| Windows | `%APPDATA%\ghostldr` |
| Linux | `~/.config/ghostldr` |

The scripts still write `build/` in the release folder, as on the command line. Where the release folder can't be written (an installed copy: the macOS app, the `.deb` in `/opt/ghostldr`, an AppImage), they run from a copy of `ghostldr/` and the prebuilt files, a few MB, kept up to date with the installed version. Its `build/` holds what they write:

| System | Folder |
|---|---|
| macOS | `~/Library/Application Support/ghostldr/workspace` |
| Windows | not needed: the installer's folder can be written |
| Linux | `~/.local/share/ghostldr/workspace` |

## Tests

```bash
cd ghostldr/tests && python3 -m unittest test_app
```
