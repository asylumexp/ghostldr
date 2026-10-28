# ghostldr

ghostldr runs PS4 game dumps and OpenOrbis homebrew inside *Star Wars Racer Revenge* (SWRR) on a stock PS5. It uses [Luac0re](https://github.com/Gezine/Luac0re)'s userland JIT exploit: no kernel exploit, no jailbreak.

You need a PS5 with SWRR (US CUSA03474 or EU CUSA03492) and Luac0re 2.4b, your own game dump, an exFAT USB drive (recommended; installing into SWRR's saved data needs none), and a computer with Python 3.11+. No game files are included.

[![Ghost of Tsushima running inside SWRR on a stock PS5 (click for the full video)](docs/media/ghostldr-demo-preview.gif)](docs/media/ghostldr-demo.mp4?raw=true)

[Full demo, 3½ minutes (MP4)](docs/media/ghostldr-demo.mp4?raw=true): from Luac0re's loader to Ghost's gameplay.

## What works

- **Games run from a USB drive** plugged into the PS5: no install step, and they load faster than installed ones ([docs/usb.md](docs/usb.md)). This is the recommended way. Games can also be installed into SWRR's saved data instead, with no drive.
- **Ghost of Tsushima v2.18** (CUSA11456) plays. It is the most tested title.
- **Bloodborne 1.09** (CUSA00900) reaches gameplay, with its large executable spread over several JIT buffers and no memory plan yet.
- **OpenOrbis homebrew** runs ([details](docs/compatibility.md#homebrew)).
- **An FTP server on the PS5** while a game runs, to put more games onto the drive in it ([docs/ftp.md](docs/ftp.md)).
- **An ELF loader** without a game: send OpenOrbis or freestanding ELF payloads to it with `nc`, and their output comes back on the connection. Userland only, no kernel exploit ([docs/elfldr.md](docs/elfldr.md)).
- **A boot with no computer:** two files sent with `nc` or a payload-sender app start a game or the ELF loader, with nothing on the computer serving files ([below](#boot-with-no-computer)).
- **Other games** boot as far as their imports and memory allow.
- **PS4 consoles** are experimental: Ghost's code runs on a 12.02 PS4, but SWRR's memory budget there is 2.5 GiB, so Ghost doesn't fit and stops before it starts. Games load from saved data there, not from a USB drive ([docs/ps4.md](docs/ps4.md)).
- **Jak X instead of SWRR** (EU CUSA07992 1.04, with [UltraC0re](https://github.com/matem6/UltraC0re)) is experimental: Ghost plays there, in its PS4 Pro mode, from its own install in Jak X's save data ([docs/jakx.md](docs/jakx.md)).

## Getting started

Download the [latest release](https://github.com/asylumexp/ghostldr/releases/latest) and follow [docs/using-the-release.md](docs/using-the-release.md).

Prefer not to use a terminal? The [desktop app](app/README.md) walks through the same steps: find the console, check the setup, copy a game to a USB drive (or install it into SWRR's saved data), and play. The release page has an installer for macOS, Windows and Linux with Python included, and the release zip has it in `app/` (with Python 3.11+).

Quick start, from the release folder. Copy your dump to an exFAT USB drive plugged into the computer (any folder name works; [docs/usb.md](docs/usb.md) says what goes on the drive):

```bash
python3 ghostldr.py copy /path/to/CUSA11456/app0
```

Plug the drive into the PS5, start SWRR, click the right half of the touchpad at PRESS START, then OPTIONS → HALL OF FAME until Luac0re's loader listens on port 9026. Check the setup once, then boot:

```bash
python3 ghostldr.py doctor
```

```bash
python3 ghostldr.py boot
```

With several games on the drive, the game selector shows them on the TV; `boot CUSA11456` (or `boot ghost`) starts one straight away. `python3 ghostldr.py check` shows what the PS5 will find on a drive, and `python3 ghostldr.py --help` lists every command.

No drive? Install the game into SWRR's saved data instead, once per title (about 40 minutes for Ghost), from the SWRR region you will play in, with the loader listening:

```bash
python3 ghostldr.py install /path/to/CUSA11456/app0
```

Then restart SWRR, start Luac0re again, and `python3 ghostldr.py boot`: with no game on a drive, it boots the installed ones.

The computer's firewall must let Python receive on UDP 18194 and TCP 18195–18196.

### Boot with no computer

No Python, no tools, and nothing on the computer serving files during the boot: the release page also has `ghostldr.lua`, `ghostldr-ext.bin` and `ghostldr-elfldr.bin`. With Luac0re's loader listening, send two files with `nc` (or a payload-sender app), where `CONSOLE` is the PS5's address:

```bash
nc CONSOLE 9026 < ghostldr.lua
nc CONSOLE 9021 < ghostldr-ext.bin
```

The first opens TCP 9021 on the PS5 and waits five minutes for the second; a jailbroken console's own ELF loader already has 9021, so there it opens 9022. The TV says which, with the PS5's address, in the dialog where Luac0re shows its own text, and then what happened. The second boots a game as `ghostldr.py boot` does: one on a USB drive in the PS5, else an installed one, with the game selector when there are several. Its log comes back on the same connection. Send `ghostldr-elfldr.bin` instead to start the [ELF loader](docs/elfldr.md). The three files must come from the same release.

**After the first time, send only `ghostldr.lua`.** It keeps the last bundle in SWRR's saved data (a 16 MiB save container, `GLDRCACHE`). On a PS5 the ELF loader then starts on 9021 at once, and the TV counts down five seconds to the games: press OK (or ○) to stay in the ELF loader instead. A new release asks for its bundle again by itself. [docs/using-the-release.md](docs/using-the-release.md#boot-with-no-computer) has the details.

## Documentation

- [Using the release](docs/using-the-release.md): download to gameplay, step by step.
- [USB drives](docs/usb.md): games on a USB drive, the recommended way: what goes on the drive, copying, checking.
- [Ghost on a stock PS5](docs/ghost-on-stock-ps5.md): the same from source, with more troubleshooting.
- [Compatibility](docs/compatibility.md): other games, homebrew, profiles, reading the log.
- [Game selector](docs/selector.md): the picker for several games, on a drive or installed.
- [FTP server](docs/ftp.md): the console's files over FTP while a title runs.
- [ELF loader](docs/elfldr.md): run ELF payloads in SWRR without a game, and build them.
- [Web manager](docs/manager.md): save backup and restore from a browser.
- [Resign a save](docs/save-resign.md): another account's PS4 save into your own save of the game, with no jailbreak (tested on a PS5).
- [Host tooling](docs/host-tooling.md): `ghostldr.py`'s commands, `run.py doctor`, the boot preflight, the stub report.
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
