# Architecture

A map of the repository, for reading or changing the code. To run ghostldr, start with [using-the-release.md](using-the-release.md).

## Layout

| Path | What it is |
|---|---|
| `ghostldr/native/` | **The loader core** (C). It runs inside SWRR from the PS2 emulator's JIT buffer. Two runtime modules sit next to it. The extension (`ext/`, see [its README](../ghostldr/native/ext/README.md)) carries the NID table and more HLE code, and is fetched from the computer at boot. The game selector (`selector/`, see [selector.md](selector.md)) picks the title to boot and is gone before the title loads. The installer's pump (`pump/`) is a separate small blob the ghostfs receiver runs ([ghostfs-catalogue.md](ghostfs-catalogue.md#the-receivers-operations)). |
| `ghostldr/host/` | **Computer-side tools** (Python), below. |
| `ghostldr/lua/` | **Scripts sent to Luac0re's loader:** the bootstrap, the ghostfs receiver, the web manager, the save resigner, and the sandbox and save probes. |
| `ghostldr/tools/` | **Build and analysis helpers:** ELF and NID parsing, the extension packer and its offline checker (`mkext.py`, `check_ext.py`), the two-file boot's files (`mkbundle.py`), the import coverage report (`import_report.py`), the code fit check (`exec_fit.py`), the stub report (`stub_report.py`), a disassembler, cross-references, the Luac0re bundler, and the UltraC0re bootstrap patch for Jak X (`ultrac0re_patch.py`). |
| `ghostldr/data/` | Title profiles (`titles/<TITLE>.json` over `titles/default.json`), Ghost's file open order (file names only), and the system modules a PS4 app can load on a 13.60 PS5. |
| `ghostldr/tests/` | Offline tests, below. |
| `host/payloads/` | **Dev-only console payloads** for a jailbroken console, below. |
| `tools/fetch_deps.sh` | Fetches the public build inputs: the PS4 symbol names (idc/ps4libdoc) to `build/nid/` and Luac0re 2.4b to `build/luac0re-src/`. |

## The core

- **Loading:** a SELF/ELF loader with relocation and NID resolution, and static TLS. Code too big for one JIT buffer is spread over several ([Large executables](compatibility.md#large-executables)).
- **System-call replacements:** file I/O, memory, video decoder, save data, dialogs, user service.
- **Memory:** a stable direct-memory budget every title gets without a plan ([Memory without a plan](compatibility.md#memory-without-a-plan)), per-title memory plans (Ghost's fits its ~5 GiB plan into SWRR's 4.5 GiB), and allocations scattered over the free holes.
- **Title profiles:** the title's memory plan, save size, app parameters and HLE rules, as data, bound to its executable's build ([Write a profile](compatibility.md#5-write-a-profile)).
- **ghostfs:** serves the chosen title's `/app0` from SWRR's save containers ([ghostfs-catalogue.md](ghostfs-catalogue.md)).
- **USB drives and folders of games:** `--app-root auto` (the default: a drive's games, else the installed ones), `usb:/` or `usb:/FOLDER` (or a dev folder of games). The extension decides before the title's modules load (`ext/src/approot.c`): the system's AutoMounter mounts the drive into SWRR's sandbox, the games on it are found by their contents in folders of any name, and the chosen game's folder becomes `/app0` (read-write) ([usb.md](usb.md)).
- **FTP server:** while a title runs, the extension serves FTP on TCP 1212 (`ext/src/ftp.c`, `ftp_sys.c`), starting clients on the USB drive ([ftp.md](ftp.md)).
- **ELF loader:** with no title (`run.py elfldr`, or `ghostldr-elfldr.bin`), the extension runs ELF payloads sent to a TCP port, each from a view of one of the emulator's JIT objects (`ext/src/elfldr.c`, `elfldr_sys.c`, `payload.c`; [elfldr.md](elfldr.md)).
- **Game selector:** loads, runs and clears the selector module before the title (`sel.c`).
- **Diagnostics:** a crash handler and watchdog, and a PS4 diagnostic (`run.py probe --diag`, [ps4.md](ps4.md)).

## Host tools

In `ghostldr/host/`:

- **`run.py`:** build the core, send it, and log the run. `run.py doctor` checks the setup ([host-tooling.md](host-tooling.md)). `run.py elfldr` starts the ELF loader instead of a title, and **`elf_send.py`** sends it a payload and shows its output ([elfldr.md](elfldr.md)).
- **`gfs_pack.py`, `gfs_install.py`:** lay out and install titles side by side, checking free space and resuming interrupted installs (`--list`, `--remove TITLE`). `gfs_pack.py --usb DRIVE` puts a title on a USB drive instead.
- **`usb_drive.py`:** find the games on a USB drive as the console does, copy (with a check afterwards), check and remove them, on a drive mounted on the computer or plugged into the console (through ghostldr's FTP server, or a jailbroken PS5's) ([usb.md](usb.md)).
- **`ghostldr.py`** (the repository's top folder): the everyday commands, `doctor`, `copy`, `check`, `boot`, `logs` and `install`, over the tools above ([host-tooling.md](host-tooling.md#ghostldrpy)).
- **`profiles.py`:** check and inspect title profiles. **`capture_order.py`:** turn a boot log into a title's file open order.
- **`manager.py`:** start the web manager on the PS5 ([manager.md](manager.md)).
- **`resign_save.py`:** make a Luac0re save for another PSN account, for the SWRR region it runs on (US or EU). It patches the save over FTP, so it needs a jailbroken console.
- **`recorder.py`:** crash and klog recorder.
- **`console.py`:** finds the console on the network ([ghost-on-stock-ps5.md](ghost-on-stock-ps5.md#2-point-the-scripts-at-your-ps5)).

## Releases

A release zip is this repository plus a prebuilt core and extension: `build/ghostldr/core.bin`, `core.json` and `ext.bin`, all from one build. `selector.bin`, the game selector, is optional and must come from the same build. The core refuses an extension built against other headers.

With these, booting needs only Python: no LLVM and no symbol list, since `ext.bin` holds every known PS4 name. The dev payloads are a separate zip; extract it into the same folder (`build/payloads/`).

Next to the zip and the installers (`tools/package/build_all.py`), the release has the two-file boot's files, which need no Python at all: `ghostldr.lua`, `ghostldr-ext.bin` and `ghostldr-elfldr.bin` (below).

## Two-file boot

`make -C ghostldr/native` also makes, with `ghostldr/tools/mkbundle.py`, a boot that needs only a network sender ([using-the-release.md](using-the-release.md#boot-with-no-computer)):

- **`ghostldr.lua`:** `ghostldr/lua/ghostldr.lua` with the core, every profile in `ghostldr/data/titles` (nothing of the computer it was built on), `run.py boot`'s default flags and app root (`auto`), and the pins of the two bundles: `GHOSTLDR_MODE = "listen"`. (`run.py` prepends `GHOSTLDR_MODE = "connect"`, and the script works as before.)
- **`ghostldr-ext.bin`:** a header, `ext.bin`, then `selector.bin`.
- **`ghostldr-elfldr.bin`:** a header, then `ext-elfldr.bin`: the extension linked with only the families a payload binds to (`ELFLDR_EXT_SRCS` in the Makefile; `ext/elfldr_only.c` stands in for what the rest would link) and packed with no NID table (`mkext.py --no-nids`).

The header, 128 bytes, little-endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `GLDRBNDL` |
| 8 | 4 | format, 1 |
| 12 | 4 | header size, 128 |
| 16 | 4 | kind: 1 `ghostldr-ext.bin`, 2 `ghostldr-elfldr.bin` |
| 20 | 4 | flags, 0 |
| 24 | 8 | the extension's size |
| 32 | 8 | the selector's size (0: none) |
| 40 | 4 + 4 | their CRC-32s |
| 48 | 32 | the extension's SHA-256 |
| 80 | 32 | the selector's SHA-256 (zeros: none) |
| 112 | 8 | the build ID: the first 8 bytes of a SHA-256 over the core and the three parts, the same in `ghostldr.lua` |
| 120 | 8 | reserved, 0 |

How it boots:

1. **Before anything of SWRR's is touched,** `ghostldr.lua` listens on TCP 9021, else 9022 (a jailbroken console's ELF loader holds 9021), and says which on the TV: in the message dialog Luac0re's loader shows its own text in (Luac0re's `show_dialog`; `ghost_boot` closes it before a game starts), and in a notification. Each outcome after that goes there too, and onto the bundle's connection. It waits up to 5 minutes, then closes the port and returns to Luac0re's loader. On a PS5 its sockets are made as Luac0re's `create_socket` makes them, in the JIT compiler process and handed over, but with the compiler's copy closed afterwards (as the extension's `jit_socket` does): Luac0re leaves that copy open, and it keeps a listening port taken after SWRR has closed its own.
2. **Each connection:** the header must match one of its pins exactly (kind, sizes, CRC-32s, SHA-256s, build ID). Anything else is refused with the reason and what was expected, on the TV, on that connection and in a notification, the rest of what it sends is read and dropped, and the wait goes on. A bundle it takes is read whole, into flexible memory it maps (each part's size rounded to 16 KiB, as `ext.c`'s own fetch takes), before anything is written back: a sender that closes right after sending must lose nothing, and some (Linux, Windows) reset a connection they close with unread data in it.
3. **The ELF loader's bundle:** its port is the first free one after the bundle's (9022 on a stock console), never the bundle's own.
4. **The core is placed and called** as from `run.py`, with `CTX_FLAG_BUNDLE`: `ext_fd` and `sel_fd` are the parts' addresses and their sizes and CRC-32s, and the extension's SHA-256 pin, come from the pins. `ext.c` takes the extension where it lies and checks it as a fetched one (CRC-32, SHA-256, layout); `sel.c` copies the selector into the staging buffer, or not when no choice is needed; each unmaps its part and sets the field to -1.
5. **The log** goes back on the bundle's connection: `log_fd` is the connection and `log_addr` 0 (`log.c`). `ghostldr.lua` sets `SO_NOSIGPIPE` and a 5-second send timeout on it; the first send that fails (the sender closed it, or stopped reading) makes the core close it and set `log_fd` to -1. The game never waits on it.
6. **When the core returns:** `ghostldr.lua` unmaps what the core didn't take. After the ELF loader's `quit`, or a boot that stopped, it takes the connection back (`log_fd` -1), says so on it and closes it, so `nc` ends, and puts Luac0re's loader text back on the TV under what happened. A game that started keeps it for its log.

In the core, all this is a context flag, two short branches (`ext.c`, `sel.c`) and the log's connection case (`log.c`): the listening, the checks against the pins and the refusals are Lua, so the core image stays inside the IPU buffer.

## Tests

```bash
python3 -m unittest discover -s ghostldr/tests
```

They run offline:

- the installer's pump, built for the computer, and its checked-in blob against the source;
- the installer and the web manager against fake consoles (the manager's tests need a desktop Lua 5.3 or 5.4);
- profiles and the pack order;
- the core's profile, memory-budget, ghostfs and module-loading code, built for the computer;
- the extension loader, run on the built `ext.bin`;
- the homebrew families, built for the computer;
- the app roots against a fake core and a fake libSceUsbStorage, and the computer's search for games agreeing with the console's on one folder tree (`test_usb_games.py`); the selector's library listing; the drive tools and `ghostldr.py`, also through a fake FTP server; the FTP server built for the computer and driven by real clients (`test_ftp.py`, [ftp.md](ftp.md#tests)); the optional USB storage stack (`make USB_RAW=1`) against exFAT, FAT32 and FAT16 images made with macOS's tools, read raw and through a fake USB drive that misbehaves on purpose (`test_usb.py`, `test_usb_games.py`, [usb.md](usb.md#tests));
- the import report against a synthetic title;
- the ELF loader: its payload formats, relocations and refusals, and its service over real sockets with a fake loader (`test_elfldr.py`, [elfldr.md](elfldr.md#tests));
- the two-file boot: the bundles against their parts and `ghostldr.lua`'s pins, `ext-elfldr.bin`'s families against the full extension's, the core's log on a connection (`native_host/log_host.c`), and `ghostldr.lua`'s listen mode under a fake Luac0re with a fake network: the context it hands the core, the ports, each refusal, the timeout and what it gives back (`test_bundle.py`); the core taking the parts from memory (`test_ext.py`, `test_selector.py`);
- the game selector: screenshots of its UI, and the built `selector.bin` against a fake console.

`ghostldr/tests/homebrew/` is a small test homebrew; building it needs the OpenOrbis toolchain. `ghostldr/tests/elfldr/` holds the ELF loader's sample payloads.

The build doesn't need a game dump. With your CUSA11456 v2.18 dump in `build/ghost/app0/` (git ignores it), `ghostldr/tools/check_ext.py` also checks that the extension resolves its imports as before.

## Dev setup (optional)

A jailbroken console with elfldr, FTP and klog, plus `/data` for the game files, adds:

- `recorder.py`: crash and klog recording;
- `upload_tree.py`: upload the game to `/data`;
- `run.py boot --app-root /data/...`;
- the payloads in `host/payloads`: launch or close SWRR (US or EU), unjail it, a save watcher, `ps`, `df`, a module dump. Build them with ps5-payload-sdk: `make PS5_PAYLOAD_SDK=/path/to/sdk` in `host/payloads`.
