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
| `tools/make_release.py` | Makes a release: the files, their checksums and the zip ([host-tooling.md](host-tooling.md#making-a-release)). `tools/release-README.txt` is the zip's `README.txt`. |

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

A release is the two-file boot's files and the web manager, which need no Python and no computer program: `ghostldr.lua`, `ghostldr-ext.bin` and `ghostldr-elfldr.bin` (below), and `ghostldr/lua/manager.lua` ([manager.md](manager.md)). `tools/make_release.py` builds them and writes `ghostldr-<version>.zip` (the four, a `README.txt` and their `SHA256SUMS.txt`, flat, reproducible) with the same files beside it for the release page ([host-tooling.md](host-tooling.md#making-a-release)). The zip has no Python and no source: developers take the repository, where `run.py` builds the core, extension and selector itself. The dev payloads for jailbroken consoles are a separate zip; extract it into the repository (`build/payloads/`).

## Two-file boot

`make -C ghostldr/native` also makes, with `ghostldr/tools/mkbundle.py`, a boot that needs only a network sender ([using-the-release.md](using-the-release.md#4-boot)):

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

1. **Before anything of SWRR's is touched,** `ghostldr.lua` looks in its cache ([below](#the-bundle-cache)); with no usable bundle there, it listens on TCP 9021, else 9022 (a jailbroken console's ELF loader holds 9021), and says which on the TV: in the message dialog Luac0re's loader shows its own text in (Luac0re's `show_dialog`; `ghost_boot` closes it before a game starts), and in a notification. Each outcome after that goes there too, and onto the bundle's connection. It waits up to 5 minutes, then closes the port and returns to Luac0re's loader. On a PS5 its sockets are made as Luac0re's `create_socket` makes them, in the JIT compiler process and handed over, but with the compiler's copy closed afterwards (as the extension's `jit_socket` does): Luac0re leaves that copy open, and it keeps a listening port taken after SWRR has closed its own.
2. **Each connection:** the header must match one of its pins exactly (kind, sizes, CRC-32s, SHA-256s, build ID). Anything else is refused with the reason and what was expected, on the TV, on that connection and in a notification, the rest of what it sends is read and dropped, and the wait goes on. A bundle it takes is read whole, into flexible memory it maps (each part's size rounded to 16 KiB, as `ext.c`'s own fetch takes), before anything is written back: a sender that closes right after sending must lose nothing, and some (Linux, Windows) reset a connection they close with unread data in it.
3. **The ELF loader's bundle:** the ELF loader takes over the bundle's port. `ghostldr.lua` closes its listener first (SWRR's descriptor; the compiler's went when the socket was made), so nothing holds the port but the bundle's accepted connection, which is no bar to a listener with `SO_REUSEADDR` (the extension's `fv_listen` sets it, and `SO_REUSEPORT`).
4. **The core is placed and called** as from `run.py`, with `CTX_FLAG_BUNDLE`: `ext_fd` and `sel_fd` are the parts' addresses and their sizes and CRC-32s, and the extension's SHA-256 pin, come from the pins. `ext.c` takes the extension where it lies and checks it as a fetched one (CRC-32, SHA-256, layout); `sel.c` copies the selector into the staging buffer, or not when no choice is needed; each unmaps its part and sets the field to -1.
5. **The log** goes back on the bundle's connection: `log_fd` is the connection and `log_addr` 0 (`log.c`). `ghostldr.lua` sets `SO_NOSIGPIPE` and a 5-second send timeout on it; the first send that fails (the sender closed it, or stopped reading) makes the core close it and set `log_fd` to -1. The game never waits on it.
6. **When the core returns:** `ghostldr.lua` unmaps what the core didn't take. After the ELF loader's `quit`, or a boot that stopped, it takes the connection back (`log_fd` -1), says so on it and closes it, so `nc` ends, and puts Luac0re's loader text back on the TV under what happened. A game that started keeps it for its log.

In the core, all this is a context flag, two short branches (`ext.c`, `sel.c`) and the log's connection case (`log.c`): the listening, the checks against the pins and the refusals are Lua, so the core image stays inside the IPU buffer. The core and the profiles travel in `ghostldr.lua` as base64 (`GHOSTLDR_CORE_B64`, `GHOSTLDR_PROFILE_B64`; `run.py` sends them so too), which leaves it about 150 KB under Luac0re's 500 KiB.

## The bundle cache

After the first time, only `ghostldr.lua` is sent: it keeps the last bundle it took in a save container of its own, and runs it from there when it is this build's ([using-the-release.md](using-the-release.md#every-later-session-ghostldrlua-alone)).

- **Where:** `GLDRCACHE` in the host's saved data (SWRR's; Jak X's under UltraC0re), one file, `bundle.bin`: the bundle as it came, header first. The container is sized as `gfs_pack.data_blocks` sizes a small one: 512 blocks, 16 MiB, for either bundle. One bundle at a time: a new one deletes the container and makes it again. `ghostldr-elfldr.bin` doesn't replace a cached `ghostldr-ext.bin`, whose extension has the ELF loader too.
- **How:** in Lua, with the ghostfs installer's calls (`gfs_receiver.lua`): `sceSaveDataMount` from the eboot's import table, the file calls through libkernel's own exports (or the commit stays busy), the unmount waited out. A bundle is cached as soon as it is taken, before the core has its memory. No core code: everything the cache needs is in `ghostldr.lua` or the extension.
- **Checks:** before use, the header against the pins (as a sent bundle's) and the file's size against it; with a boot, the selector's CRC-32, here; then the core checks the extension as always (CRC-32 and SHA-256, `-7` when they fail), before anything else runs. Another build's header is a **version mismatch**: said on the TV above the prompt, in a notification and to the first connection; the cache stays until a bundle comes, which replaces it. Anything else wrong (cut short, a wrong checksum, a container that won't mount) is **damaged**: the container is deleted and the prompt says so. A full disk (`0x809F000A`, with the blocks still missing) or no saved-data calls cost only the cache: ghostldr goes on without it, and the log says it wasn't cached.
- **`ghostldr-ext.bin`, cached or just sent, on a PS5: the game selector with the ELF loader beside it.** No countdown. `ghostldr.lua` takes the bundle port as for a prompt (9021, else 9022), closes it, and calls the core with `CTX_FLAG_ELFLDR`, `CTX_ELFLDR_SELECT` (bit 57), `CTX_FLAG_SELECT`, the port, the extension and the selector, and `struct el_select` at `CTX_SELECT_SLOT` (66; `ctx->size` 0x218; `ghost.h`). The core (`boot.c` `elfldr_select`) finds the games as a boot does, places and starts the extension, runs its ELF loader on a thread of its own, and runs the selector, even for one game. Meanwhile the ELF loader takes payloads (their code at the end of a JIT object, never where the selector's image goes: `res_lo`/`res_hi`), `log` and `status`, and holds one ghostldr bundle; `quit`, `boot` and `forget` wait for the pick, and the selector has the TV. Once a game is picked the core sets `stop`; the ELF loader stops taking connections, waits for any payload still running (the TV says how many), closes its JIT object descriptors and returns; ghostfs's data containers are unmounted and the extension is given back with its executable memory (`ext_abandon`). The core returns `CTX_ELFLDR_PICKED` (4) with the pick (index, title, a save tool's user) in the block. `ghostldr.lua` caches a bundle the ELF loader held (for the next boot, below; its connection stays the log), reads the cache as it was before it, and calls the core again with the block's `boot` set: `boot.c` takes the pick instead of running the selector, and the game boots. A sent bundle that couldn't be cached boots as it is, with the selector and no ELF loader.
- **A PS4, or Jak X, with `ghostldr-ext.bin`:** the game selector at once (`CTX_FLAG_SELECT`), with no ELF loader.
- **A PS5 with a cached `ghostldr-elfldr.bin`:** `ghostldr.lua` takes the bundle port as for a prompt (9021, else 9022), closes it, and calls the core at once with `CTX_FLAG_ELFLDR`, the port, the cached extension, and leave to boot when this build's `ghostldr-ext.bin` is cached (bit 56 of the flags, `CTX_ELFLDR_CAN_BOOT` in `ghost.h`; with no title the FTP port's bits are free). The extension's ELF loader (`elfldr.c`) returns what happened, and `ghostldr.lua` carries on: `EL_BOOT` (1: `boot`) reads the cache again, extension and selector, and opens the game selector with the ELF loader beside it, as above; `EL_BUNDLE` (2: a bundle came to the port) is kept for the next boot (below); `EL_FORGET` (3) deletes the cache and opens the prompt on the same port; `-7` deletes a damaged cache and opens the prompt; 0 (`quit`) and the rest go back to Luac0re's loader. The core is placed afresh for each call, as by a second `ghostldr.lua`.
- **A bundle sent to the ELF loader is for the next boot:** any build's ghostldr bundle is taken (its own header checked against its parts by the ELF loader), cached with its header as it came, and not run. The sender, the TV and a notification are told `the new ghostldr ext file will be applied on the next ghostldr boot` (and, from another build, which `ghostldr.lua` it needs); its connection closes, and the ELF loader starts again with what it had (the cache as it was, read before the new bundle replaced it). A bundle with this build's ID but other contents is refused as damaged. The next `ghostldr.lua` of that bundle's build boots it at once; one of another build sees a version mismatch, as for any cache.
- **The ELF loader's side:** in the release's mode (`CTX_FLAG_BUNDLE`) a connection whose first bytes are `GLDRBNDL` is a bundle: read whole into flexible memory and checked against its own header (refused while payloads run, when it isn't format 1 or its sizes are wrong, or when a part's CRC-32 isn't the header's), its parts' addresses, sizes, CRC-32s and SHA-256 pin put in the context as `ghostldr.lua` puts them, its 128-byte header copied for `ghostldr.lua` to cache, and its connection made the log. `log` makes the sending connection the log. Two context slots after the profile slot carry what it needs from `ghostldr.lua`: the console's address as text, for the TV (`CTX_CONSOLE_IP_SLOT`, 64), and where to copy a bundle's header (`CTX_BUNDLE_HEADER_SLOT`, 65); `ctx->size` 0x210 says they are there.
- **A PS4, or Jak X:** no ELF loader, so a cached `ghostldr-elfldr.bin` is ignored there and the prompt asks for `ghostldr-ext.bin`.
- **Clearing:** `forget` at the ELF loader; `GHOSTLDR_CLEAR_CACHE = true` before `ghostldr.lua` (deleted first, then the usual flow); `run.py clear-cache` (`ghostldr.py clear-cache`), which sends `ghostldr.lua` in its `clear-cache` mode, without the core.

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
- the two-file boot: the bundles against their parts and `ghostldr.lua`'s pins, `ext-elfldr.bin`'s families against the full extension's, the core's log on a connection (`native_host/log_host.c`), and `ghostldr.lua`'s listen mode under a fake Luac0re (`boot_fake.lua`): the context it hands the core, the ports, each refusal, the timeout and what it gives back; the bundle cache with save containers in a directory (a full disk, a damaged or half-written file, another release's), an instant clock, sockets that SWRR and the JIT compiler each hold (9021 must be free in both before the ELF loader binds it), and a scripted core and ELF loader (`test_bundle.py`); the ELF loader's commands and bundles (CRC-32s against their header, any build) over real sockets (`test_elfldr.py`); the core taking the parts from memory (`test_ext.py`, `test_selector.py`);
- the game selector: screenshots of its UI, and the built `selector.bin` against a fake console.

`ghostldr/tests/homebrew/` is a small test homebrew; building it needs the OpenOrbis toolchain. `ghostldr/tests/elfldr/` holds the ELF loader's sample payloads.

The build doesn't need a game dump. With your CUSA11456 v2.18 dump in `build/ghost/app0/` (git ignores it), `ghostldr/tools/check_ext.py` also checks that the extension resolves its imports as before.

## Dev setup (optional)

A jailbroken console with elfldr, FTP and klog, plus `/data` for the game files, adds:

- `recorder.py`: crash and klog recording;
- `upload_tree.py`: upload the game to `/data`;
- `run.py boot --app-root /data/...`;
- the payloads in `host/payloads`: launch or close SWRR (US or EU), unjail it, a save watcher, `ps`, `df`, a module dump. Build them with ps5-payload-sdk: `make PS5_PAYLOAD_SDK=/path/to/sdk` in `host/payloads`.
