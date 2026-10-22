# The ghostldr app

A desktop app for everything in [using-the-release.md](../docs/using-the-release.md), without a terminal. It is a wizard (**Console → Game → Install → Play**) with a status and one main button per step, plus:

- a **Library** of the games installed on the PS5 and prepared on this computer;
- the **save manager**;
- **Logs & Help**: the game's log, everything the app ran, firewall help and troubleshooting.

It follows the system's light or dark mode.

## Starting it

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

## How it works

- **The scripts:** the app runs the same scripts as the command line, with the Python that started it, and reads what they print. It doesn't reimplement them.
  - `console.py --list`
  - `run.py doctor`
  - `gfs_pack.py`
  - `gfs_install.py`, including `--list`, `--remove` and `--update-meta`
  - `run.py boot --app-root ghostfs --no-build`
  - `manager.py --detach` and `--exit`
  - `import_report.py` and `stub_report.py`
- **Raw output:** each script's full output is under **Details**, and saved in the logs folder.
- **One at a time:** only one job uses the console at once. **Pause** and **Stop** send the script Ctrl+C.
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

The last game folder, the chosen console and the logs of each run. They are kept in your user folder, never in the app folder:

| System | Folder |
|---|---|
| macOS | `~/Library/Application Support/ghostldr` |
| Windows | `%APPDATA%\ghostldr` |
| Linux | `~/.config/ghostldr` |

The scripts still write `build/` in the release folder, as on the command line.

## Tests

```bash
cd ghostldr/tests && python3 -m unittest test_app
```
