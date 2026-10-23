# Architecture

A map of the repository, for reading or changing the code. To run ghostldr, start with [using-the-release.md](using-the-release.md).

## Layout

| Path | What it is |
|---|---|
| `ghostldr/native/` | **The loader core** (C). It runs inside SWRR from the PS2 emulator's JIT buffer. Two runtime modules sit next to it. The extension (`ext/`, see [its README](../ghostldr/native/ext/README.md)) carries the NID table and more HLE code, and is fetched from the computer at boot. The game selector (`selector/`, see [selector.md](selector.md)) picks the title to boot and is gone before the title loads. |
| `ghostldr/host/` | **Computer-side tools** (Python), below. |
| `ghostldr/lua/` | **Scripts sent to Luac0re's loader:** the bootstrap, the ghostfs receiver, the web manager, the save resigner, and the sandbox and save probes. |
| `ghostldr/tools/` | **Build and analysis helpers:** ELF and NID parsing, the extension packer and its offline checker (`mkext.py`, `check_ext.py`), the import coverage report (`import_report.py`), the code fit check (`exec_fit.py`), the stub report (`stub_report.py`), a disassembler, cross-references, the Luac0re bundler, and the UltraC0re bootstrap patch for Jak X (`ultrac0re_patch.py`). |
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
- **Game selector:** loads, runs and clears the selector module before the title (`sel.c`).
- **Diagnostics:** a crash handler and watchdog, and a PS4 diagnostic (`run.py probe --diag`, [ps4.md](ps4.md)).

## Host tools

In `ghostldr/host/`:

- **`run.py`:** build the core, send it, and log the run. `run.py doctor` checks the setup ([host-tooling.md](host-tooling.md)).
- **`gfs_pack.py`, `gfs_install.py`:** lay out and install titles side by side, checking free space and resuming interrupted installs (`--list`, `--remove TITLE`).
- **`profiles.py`:** check and inspect title profiles. **`capture_order.py`:** turn a boot log into a title's file open order.
- **`manager.py`:** start the web manager on the PS5 ([manager.md](manager.md)).
- **`resign_save.py`:** make a Luac0re save for another PSN account, for the SWRR region it runs on (US or EU). It patches the save over FTP, so it needs a jailbroken console.
- **`recorder.py`:** crash and klog recorder.
- **`console.py`:** finds the console on the network ([ghost-on-stock-ps5.md](ghost-on-stock-ps5.md#2-point-the-scripts-at-your-ps5)).

## Releases

A release zip is this repository plus a prebuilt core and extension: `build/ghostldr/core.bin`, `core.json` and `ext.bin`, all from one build. `selector.bin`, the game selector, is optional and must come from the same build. The core refuses an extension built against other headers.

With these, booting needs only Python: no LLVM and no symbol list, since `ext.bin` holds every known PS4 name. The dev payloads are a separate zip; extract it into the same folder (`build/payloads/`).

## Tests

```bash
python3 -m unittest discover -s ghostldr/tests
```

They run offline:

- the installer and the web manager against fake consoles (the manager's tests need a desktop Lua 5.3 or 5.4);
- profiles and the pack order;
- the core's profile, memory-budget, ghostfs and module-loading code, built for the computer;
- the extension loader, run on the built `ext.bin`;
- the homebrew families, built for the computer;
- the import report against a synthetic title;
- the game selector: screenshots of its UI, and the built `selector.bin` against a fake console.

`ghostldr/tests/homebrew/` is a small test homebrew; building it needs the OpenOrbis toolchain.

The build doesn't need a game dump. With your CUSA11456 v2.18 dump in `build/ghost/app0/` (git ignores it), `ghostldr/tools/check_ext.py` also checks that the extension resolves its imports as before.

## Dev setup (optional)

A jailbroken console with elfldr, FTP and klog, plus `/data` for the game files, adds:

- `recorder.py`: crash and klog recording;
- `upload_tree.py`: upload the game to `/data`;
- `run.py boot --app-root /data/...`;
- the payloads in `host/payloads`: launch or close SWRR (US or EU), unjail it, a save watcher, `ps`, `df`, a module dump. Build them with ps5-payload-sdk: `make PS5_PAYLOAD_SDK=/path/to/sdk` in `host/payloads`.
