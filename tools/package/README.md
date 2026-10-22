# Installers

The desktop app ([app/](../../app/README.md)) packaged per system, for the release page. Each one installs the release tree (as in the release zip: `app/`, `ghostldr/`, the four prebuilt `build/ghostldr` files, `LICENSE` and the font's `OFL.txt`) and, except the `.deb`, its own Python.

| File | Script | Python | Installs to |
|---|---|---|---|
| `ghostldr-<version>-macos-universal.dmg` | `build_macos.py` | python-build-standalone, arm64 and x86_64 both in the app | `ghostldr.app`, dragged to Applications |
| `ghostldr-<version>-windows-x64-setup.exe` | `build_windows.py` | python.org's embeddable package (amd64) | `%LOCALAPPDATA%\ghostldr`, for the user only |
| `ghostldr_<version>_all.deb` | `build_deb.py` | the system's (`Depends: python3 (>= 3.11)`) | `/opt/ghostldr`, `/usr/bin/ghostldr` |
| `ghostldr-<version>-x86_64.AppImage`, `-aarch64.AppImage` | `build_appimage.py` | python-build-standalone for that architecture | wherever it is saved |

`<version>` is `APP_VERSION` in `app/ghostapp/__init__.py`.

## Building a release

```bash
python3 tools/package/build_all.py --out ~/ghostldr-release
```

- **What it does:** `make -s` in `ghostldr/native` (the prebuilt files; `--no-make` skips it), then every installer from `HEAD` (`--ref` for another revision), then `SHA256SUMS.txt` in the output folder. `--only deb,appimage` builds some of them.
- **Where:** the macOS app is built on macOS, the rest on macOS or Linux. Default output: `build/package/dist`; downloads are cached in `build/package/cache`.
- **Tools:**
  - Python 3.9+ to run the scripts;
  - [resvg](https://github.com/linebender/resvg) (or `rsvg-convert`) for the icon, from `app/web/icon.svg`;
  - Windows: NSIS 3's `makensis`;
  - AppImage: `mksquashfs` (squashfs-tools 4.4+, with zstd);
  - macOS app: Xcode's command line tools (`clang`, `codesign`, `iconutil`, `hdiutil`).

  On macOS: `brew install resvg makensis squashfs`. On Debian or Ubuntu: `apt install resvg nsis squashfs-tools`.
- **Downloads:** only the ones pinned in [sources.json](sources.json), each checked against its SHA-256 before use: python-build-standalone's GitHub releases, python.org, and AppImage/type2-runtime's GitHub releases. To move to a newer Python, change the URLs and take the new SHA-256 from the release's own `SHA256SUMS` (python-build-standalone), python.org's release page (`sha256_sum` in its downloads API), or the GitHub asset's digest (the AppImage runtime). Keep one CPython version across all of them.
- **A last check:** each script fails if this computer's home folder, its user name or a private network address (but the documented examples) is in what it packaged.

## How each one works

- **macOS** (`build_macos.py`): `ghostldr.app/Contents/MacOS/ghostldr` is a small universal launcher ([macos/launcher.c](macos/launcher.c)). It runs `Resources/ghostldr-app/app/launch.py --workspace` with `Resources/python/arm64` or `Resources/python/x86_64`, by the slice that runs. Opened from the Finder it puts the app in the background and quits, so opening it again opens the running app's page. It logs to `~/Library/Logs/ghostldr/app.log`.
  - **Signing:** every binary and then the bundle are signed ad hoc (`codesign --force --deep -s -`), which lets it run on Apple silicon.
  - **Not notarized:** there is no Developer ID signature, so Gatekeeper blocks the first open of a downloaded copy (see the install notes below).
  - **The DMG:** `hdiutil`, LZMA (`ULMO`, macOS 10.15+), with an Applications link and [macos/README.txt](macos/README.txt).
- **Windows** (`build_windows.py`, [windows/ghostldr.nsi](windows/ghostldr.nsi)): an NSIS installer that needs no administrator.
  - **Shortcuts:** Start menu, and optionally the desktop, to `python\pythonw.exe app\launch.py`, so no console window.
  - **Updates and removal:** installing over an old version replaces it and keeps `build\`; the uninstaller removes only what it installed and that `build\`, and leaves the settings in `%APPDATA%\ghostldr`.
  - **The path:** the embeddable package's `python3XX._pth` ([windows/python._pth](windows/python._pth)) adds `ghostldr\host` and `ghostldr\tools`. That Python runs scripts in isolated mode, without their own folder on `sys.path`, and the scripts import their neighbours.
  - **No folder dialog:** it has no `tkinter`. The app asks PowerShell for the folder dialog and falls back to its own folder browser.
- **Debian** (`build_deb.py`): built in Python, no `dpkg-deb` needed: an ar archive of `debian-binary`, `control.tar.xz` (control, md5sums, a `prerm` that clears bytecode) and `data.tar.xz`, root-owned. Ubuntu 22.04's Python is 3.10, too old: its users take the AppImage.
- **AppImage** (`build_appimage.py`): the AppImage type 2 runtime followed by a zstd squashfs of the AppDir, as `appimagetool` makes it. The AppDir holds `AppRun` ([linux/AppRun](linux/AppRun)), `ghostldr.desktop`, the icon, Python, and the release tree in `usr/lib/ghostldr`. Python is linked against glibc 2.17, so it runs on any current distribution. It needs FUSE; `--appimage-extract-and-run` works without.
- **The tools' folder:** the scripts write `build/` next to themselves, and the macOS app, `/opt/ghostldr` and an AppImage can't be written. So these launchers pass `--workspace`, and the app runs the scripts from a copy in `~/Library/Application Support/ghostldr/workspace` or `~/.local/share/ghostldr/workspace` ([app/ghostapp/workspace.py](../../app/ghostapp/workspace.py)). The Windows folder can be written, so the scripts run in place there.
- **Bytecode:** the macOS app and the AppImages carry their Python's and the app's bytecode, checked by hash, so they never write into themselves.

## Testing

```bash
python3 -m unittest discover -s tools/package -p 'test_*.py'      # the scripts' own pieces
python3 tools/package/smoke_test.py -- <launcher>                  # an installed copy
```

`smoke_test.py` starts the launcher with `--no-open` and a scratch `HOME`, then drives the app's API as the page does. It reads the state, prepares a generated homebrew dump (`profiles.py`, `gfs_pack.py`), makes a compatibility report (`import_report.py`) and quits. It never looks for a console. Launchers to test:

- **macOS:** `ghostldr.app/Contents/MacOS/ghostldr`, from the mounted DMG or Applications.
- **Debian 12 or Ubuntu 24.04**, after `apt install ./ghostldr_<version>_all.deb`: run it as a normal user, with `-- ghostldr`.
- **AppImage:** `-- ./ghostldr-<version>-<arch>.AppImage`. In a container without FUSE, `-- env APPIMAGE_EXTRACT_AND_RUN=1 ./ghostldr-…AppImage`. On a system without Python, run `smoke_test.py` with the AppImage's own Python (`--appimage-extract`, then `squashfs-root/usr/lib/ghostldr/python/bin/python3`).

For example, on an arm64 Mac with Docker:

```bash
docker run --rm --platform linux/arm64 -v "$PWD:/repo:ro" debian:12 sh -c '
  apt-get update -qq && apt-get install -y -qq /repo/build/package/dist/ghostldr_*_all.deb >/dev/null &&
  useradd -m t && su t -c "python3 /repo/tools/package/smoke_test.py -- ghostldr"'
```
