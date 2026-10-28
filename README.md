# ghostldr

ghostldr runs PS4 game dumps and OpenOrbis homebrew inside *Star Wars Racer Revenge* (SWRR) on a stock PS5. It uses [Luac0re](https://github.com/Gezine/Luac0re)'s userland JIT exploit: no kernel exploit, no jailbreak.

You need a PS5 on firmware 13.60 or later with SWRR (US CUSA03474 or EU CUSA03492) and Luac0re 2.4b, your own game dump, an exFAT USB drive (recommended; installing into SWRR's saved data from a browser needs none), and a computer or phone that can send a file over TCP (`nc`, or a payload-sender app). No Python is needed, and no game files are included.

[![Ghost of Tsushima running inside SWRR on a stock PS5 (click for the full video)](docs/media/ghostldr-demo-preview.gif)](docs/media/ghostldr-demo.mp4?raw=true)

[Full demo, 3½ minutes (MP4)](docs/media/ghostldr-demo.mp4?raw=true): from Luac0re's loader to Ghost's gameplay.

## What works

- **Every PS5 firmware from 13.60 up**, with US or EU SWRR.
- **Games run from a USB drive** plugged into the PS5: no install step, and they load faster than installed ones ([docs/usb.md](docs/usb.md)). This is the recommended way. Games can also be installed into SWRR's saved data instead, with no drive.
- **Ghost of Tsushima v2.18** (CUSA11456) plays. It is the most tested title.
- **Bloodborne 1.09** (CUSA00900) is playable: its large executable runs from several JIT buffers, and a memory plan gives its heaps what SWRR has; loading, playing and saving work.
- **OpenOrbis homebrew** runs ([details](docs/compatibility.md#homebrew)).
- **An FTP server on the PS5** while a game runs, to put more games onto the drive in it ([docs/ftp.md](docs/ftp.md)).
- **An ELF loader** without a game: send OpenOrbis or freestanding ELF payloads to it with `nc`, and their output comes back on the connection. Userland only, no kernel exploit ([docs/elfldr.md](docs/elfldr.md)).
- **A boot with no computer:** two files sent with `nc` or a payload-sender app start a game or the ELF loader, with nothing on the computer serving files ([below](#getting-started)).
- **The game selector** with **Settings**: what's wrong with a game or a drive, installing over the network, logs, and the options that were `run.py` flags ([docs/selector.md](docs/selector.md)).
- **Install over network:** a game goes into SWRR's saved data from a browser on a computer or phone ([docs/manager.md](docs/manager.md#install-a-game)).
- **Every failure explained on the TV,** and each boot's log on the USB drive ([docs/failures.md](docs/failures.md), [docs/logs.md](docs/logs.md)).
- **Other games** boot as far as their imports and memory allow.
- **PS4 consoles** run homebrew and small titles, from SWRR's saved data (not a USB drive). Big games can't run there: the PS4's kernel caps PS2 Classics such as SWRR at 2.5 GiB of memory, and Ghost needs about 4 GiB ([docs/ps4.md](docs/ps4.md)).
- **Jak X instead of SWRR** (EU CUSA07992 1.04, with [UltraC0re](https://github.com/matem6/UltraC0re)) is experimental: Ghost plays there, in its PS4 Pro mode, from its own install in Jak X's save data ([docs/jakx.md](docs/jakx.md)).

## Getting started

Download `ghostldr-<version>.zip` from the [latest release](https://github.com/asylumexp/ghostldr/releases/latest) and unpack it. [docs/using-the-release.md](docs/using-the-release.md) takes it step by step; in short:

1. **The game:** copy your dump's folder (the one with `eboot.bin`) onto an exFAT USB drive, in a folder of any name, and plug the drive into the PS5 ([docs/usb.md](docs/usb.md)). No drive? Install the game from a browser instead (step 4).
2. **Luac0re:** start SWRR, click the right half of the touchpad at PRESS START, then OPTIONS → HALL OF FAME until Luac0re's loader listens on port 9026.
3. **Boot:** send two files from a computer, or with a payload-sender app from a phone, where `CONSOLE` is the PS5's address:

   ```bash
   nc CONSOLE 9026 < ghostldr.lua
   nc CONSOLE 9021 < ghostldr-ext.bin
   ```

   The first opens TCP 9021 on the PS5 (9022 on a jailbroken console, whose own ELF loader has 9021) and says so on the TV; the second boots. On Linux use `nc -N` for the first. **After the first time, send only `ghostldr.lua`:** it keeps the bundle in SWRR's saved data (`GLDRCACHE`, 16 MiB).
4. **The game selector** opens on the TV with the games on the drive and the installed ones: Cross starts one. On a PS5 the [ELF loader](docs/elfldr.md) listens beside it until a game is picked. Square opens **Settings**: what's wrong with a game or a drive, **Install over network** (an address and a QR code to open in a browser), the logs, Advanced, and Close ghostldr.

The three files must come from the same release. Send `ghostldr-elfldr.bin` instead of `ghostldr-ext.bin` for the ELF loader alone.

The Python tools (`ghostldr.py`, `run.py`) stay in the repository for developers: [docs/host-tooling.md](docs/host-tooling.md).

## Documentation

- [Using the release](docs/using-the-release.md): download to gameplay, step by step, with no Python.
- [USB drives](docs/usb.md): games on a USB drive, the recommended way: what goes on the drive, copying, checking.
- [Ghost on a stock PS5](docs/ghost-on-stock-ps5.md): the same from source with the Python tools, with more troubleshooting.
- [Compatibility](docs/compatibility.md): other games, homebrew, profiles, reading the log.
- [When something goes wrong](docs/failures.md): every message on the TV, and what to do. [Logs](docs/logs.md): each boot's log on the USB drive.
- [Game selector](docs/selector.md): the picker for several games, on a drive or installed.
- [FTP server](docs/ftp.md): the console's files over FTP while a title runs.
- [ELF loader](docs/elfldr.md): run ELF payloads in SWRR without a game, and build them.
- [Web manager](docs/manager.md): install a game into saved data from a browser on a computer or phone (no Python), and back up and restore saves.
- [Resign a save](docs/save-resign.md): another account's PS4 save into your own save of the game, with no jailbreak (tested on a PS5).
- [Host tooling](docs/host-tooling.md): for developers: `ghostldr.py`'s commands, `run.py doctor`, the boot preflight, the stub report, making a release.
- [ghostfs](docs/ghostfs-catalogue.md): container names and the title catalogue.
- [PS4](docs/ps4.md): status and the diagnostic to run.
- [Jak X](docs/jakx.md): ghostldr in Jak X with UltraC0re, and what differs from SWRR.
- [Architecture](docs/architecture.md): repository layout, the core, host tools, dev setup.
- [Runtime extension](ghostldr/native/ext/README.md): the NID table and HLE families.

## Building from source

You need macOS or Linux, Python 3.11+ and LLVM 18 (clang, ld.lld, llvm-objcopy, llvm-nm). Fetch the PS4 symbol names and Luac0re 2.4b into `build/`:

```bash
tools/fetch_deps.sh
```

Build the core, extension and selector. The default is Homebrew's `llvm@18`; add `LLVM=/path/to/llvm/bin` for another.

```bash
make -C ghostldr/native
```

`run.py boot` without `--no-build` runs this itself. The tests are described in [docs/architecture.md](docs/architecture.md#tests).

## License and credits

MIT, see [LICENSE](LICENSE). It covers ghostldr's own code only; Luac0re, ps4libdoc and ps5-payload-sdk keep their own licenses. The selector's built-in font is under the SIL Open Font License 1.1 ([OFL.txt](ghostldr/native/selector/font/OFL.txt)).

- [Luac0re](https://github.com/Gezine/Luac0re), by Gezine and contributors (MIT).
- [mast1c0re](https://github.com/McCaulay/mast1c0re), by CTurt and McCaulay.
- PS4 symbol names from [idc/ps4libdoc](https://github.com/idc/ps4libdoc).
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk), by John Törnblom (dev payloads only).
- [Noto Sans](https://github.com/notofonts/latin-greek-cyrillic), by the Noto Project Authors (the selector's font).
- [shadPS4](https://github.com/shadps4-emu/shadPS4), used only as a reference for how system functions behave. No shadPS4 code is included.
