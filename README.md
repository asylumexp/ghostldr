# ghostldr

Runs a PS4 game backup, Ghost of Tsushima (CUSA11456 v2.18), inside *Star Wars Racer Revenge* (SWRR, PS4 US CUSA03474 or EU CUSA03492) on a PS5. It goes through [Luac0re](https://github.com/Gezine/Luac0re)'s userland JIT exploit, with no kernel exploit and no jailbreak.

https://github.com/user-attachments/assets/6bcab55f-559f-438c-81f5-7104bc5003a3

The game's files live in SWRR's own save containers, installed once over the network ("ghostfs").

**US and EU SWRR both work.** The two regions run the same executable, so the same core and offsets serve both. Each region keeps its own saved data, though: install the game data from the SWRR you will play it in (an install made in US SWRR doesn't appear in EU SWRR, and vice versa).

**No game files are included.** Bring your own CUSA11456 dump; it goes in `build/ghost/app0`, which git ignores.

Start with [docs/ghost-on-stock-ps5.md](docs/ghost-on-stock-ps5.md): triggering Luac0re, the data install, and booting.

## Layout

| Path | What it is |
|---|---|
| `ghostldr/native/` | **The loader core** (C, about 6,400 lines). Runs inside SWRR from Luac0re's JIT buffer. Its runtime extension (`ext/`, see [ext/README.md](ghostldr/native/ext/README.md)) carries the NID table and more HLE code, and is fetched from the computer at boot. |
| `ghostldr/host/` | **Computer-side tools** (Python). |
| `ghostldr/lua/` | **Scripts sent to Luac0re's loader:** the bootstrap, the ghostfs receiver, the save resigner, and the sandbox/save probes. |
| `ghostldr/tools/` | **Build and analysis helpers:** ELF/NID parsing, the extension packer and its offline checker (`mkext.py`, `check_ext.py`), disassembler, cross-references, Luac0re bundler. |
| `ghostldr/data/` | Ghost's file open order, used to pack the containers (file names only). |
| `ghostldr/tests/` | Offline tests: the installer against a fake console, and the extension loader run on the built `ext.bin`. |
| `host/payloads/` | **Dev-only console payloads** (ps5-payload-sdk), for a jailbroken console: launch or close SWRR (US or EU), unjail it, save watcher, ps, df, module dump. |
| `tools/fetch_deps.sh` | Fetches the public build inputs (see Setup). |

What the core contains:

- **Loading:** a SELF/ELF loader with relocation and NID resolution, and static TLS.
- **System-call replacements:** file I/O, memory, video decoder, save data, dialogs, user service.
- **Memory:** a virtual direct-memory manager that fits Ghost's memory plan into SWRR's 4.5 GiB.
- **ghostfs:** serves `/app0` from save containers.
- **Diagnostics:** a crash handler and watchdog.

The host tools:

- **`run.py`:** build the core, send it, and log the run.
- **`gfs_pack.py` and `gfs_install.py`:** lay out and install the ghostfs data, switching titles safely and resuming interrupted installs.
- **`resign_save.py`:** make a Luac0re save for another PSN account, for the SWRR region it runs on (US or EU).
- **`recorder.py`:** crash and klog recorder.
- **`console.py`:** finds the PS5 on the network.

## Releases

A release zip is this repo plus a prebuilt core and extension (`build/ghostldr/core.bin`, `core.json`, `ext.bin`, all from one build); [docs/using-the-release.md](docs/using-the-release.md) walks through it from download to gameplay. With it, booting needs only Python 3, no LLVM or symbol list:

```bash
python3 ghostldr/host/run.py boot --app-root ghostfs --no-build
```

The core's memory plan and patches are built for CUSA11456 v2.18; the symbol table in `ext.bin` holds every known PS4 name. The dev payloads are a separate zip; extract it into the same folder (`build/payloads/`).

## Setup

- **Computer:** macOS or Linux with Python 3.11+ and LLVM 18 (clang, ld.lld, llvm-objcopy, llvm-nm). The build uses Homebrew's `llvm@18` unless given another: `make LLVM=/path/to/llvm/bin` in `ghostldr/native`.
- **Fetch the public inputs:**

  ```bash
  tools/fetch_deps.sh
  ```

  This downloads the PS4 symbol names (idc/ps4libdoc) to `build/nid/` and clones Luac0re 2.4b to `build/luac0re-src/`.
- **Game dump:** your CUSA11456 v2.18 dump goes in `build/ghost/app0/`. The build doesn't need it; `ghostldr/tools/check_ext.py` uses its imports to check that the extension resolves them as before.
- **Tests:**

  ```bash
  python3 -m unittest discover -s ghostldr/tests
  ```

## Choosing a console

The host tools find the PS5 with the PlayStation discovery protocol:

```bash
python3 ghostldr/host/console.py --list
```

Set `PS5_HOST_ID` (follows DHCP moves) or `PS5_HOST` (a fixed address) when more than one console answers.

## Dev setup (optional)

The dev setup is a jailbroken console with elfldr, FTP and klog, plus `/data` for the game files. It adds:

- `recorder.py`: crash and klog recording;
- `upload_tree.py`: upload the game to `/data`;
- `run.py boot --app-root /data/...`;
- the payloads in `host/payloads`, built with ps5-payload-sdk (`make PS5_PAYLOAD_SDK=/path/to/sdk`).

## Credits

- Luac0re, by Gezine and contributors (MIT).
- mast1c0re, by CTurt and McCaulay.
- PS4 symbol names from idc/ps4libdoc.
- ps5-payload-sdk, by John Törnblom.

## License

MIT, see [LICENSE](LICENSE). This covers ghostldr's own code only: no game files are included, and Luac0re, ps4libdoc and ps5-payload-sdk keep their own licenses.
