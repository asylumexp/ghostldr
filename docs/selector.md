# The game selector

When more than one title is installed, ghostldr shows a game picker before it loads anything: a row of game tiles over the selected game's background, laid out after the PS4 home screen. You pick a game with the controller and it boots. Everything the picker used is freed before the game loads.

## Controls

| Button | What it does |
|---|---|
| Left / Right (d-pad or left stick) | Move along the row. Holding repeats. |
| L1 / R1 | Jump 5 titles. |
| Cross | Boot the selected game. An incomplete install can't be booted: a message says to rerun `gfs_install.py`. |
| Options or Triangle | Show the game's information: title ID, version, size, install status, container prefix. |
| Circle | Close the information panel. |

There is no way back to SWRR from the picker: to leave without booting, close SWRR from the PS button menu.

## When it appears

`run.py boot` serves the selector (`build/ghostldr/selector.bin`) next to the extension, on TCP 18196. ghostfs decides whether it is needed (`gfs_init`, docs/ghostfs-catalogue.md):

- **More than one complete title installed and no `--title`:** `gfs_init` mounts nothing and returns 1. `boot.c` closes SWRR's message dialog and releases SWRR's memory, then runs the selector. It mounts the chosen title with `gfs_pick` and goes on with the boot: the title's profile is bound and its memory planned only after the selector has freed everything.
- **One complete title, or `--title T`:** that title boots as before. The core closes the selector's socket without loading the module.
- **`run.py boot --select`:** the selector shows anyway, even with one title or `--title` (run.py then sends every title profile it knows, so the title picked gets its own, and ignores `--title`). Use it to check the selector on hardware.
- **No selector** (`--no-selector`, a release without `selector.bin`, or the module fails): several titles stop the boot as before (`pick one with run.py boot --title TITLE`). With `--title`, or one title, that title boots.
- **Outside ghostfs** (`--app-root /data/...`), only with `--select`.

```bash
python3 ghostldr/host/run.py boot --app-root ghostfs --select
```

The selector uses its built-in font by default; `--select-font auto` tries the console's system font first (see Text). The two are ctx flags 2048 and 4096.

## What it shows

- **The list:** `GFSCATALOG/catalog.bin` (docs/ghostfs-catalogue.md). An install made with ghostldr v0.1 has no catalogue; its one title is the one in `GFSINDEX` (or `GHOSTIDX`). Outside ghostfs (`--app-root /data/...` on a dev console) the one title is the app root.
- **Art:** each title's `sce_sys/icon0.png` (the tile) and `sce_sys/pic1.png` (the background), found through the title's own index (`<prefix>INDEX`, or the legacy index) and read from its data containers. The containers are mounted read-only for each read and unmounted at once, one at a time.
- **Names and versions:** from the catalogue. When the catalogue has none, from the title's `sce_sys/param.sfo` (`TITLE_xx` in the system language, else `TITLE`, and `APP_VER`). Dumps without `param.sfo` show their title ID.
- **Missing art:** a tile in a colour derived from the title ID, with the title's initials; a missing background shows a generated blue backdrop.
- **A clock**, in the system's 12/24-hour setting.

Icons are loaded one per frame, nearest to the selection first; a background is decoded once the row stops moving.

## Text

- **The console's system font (SST)**, through `libSceFont` and its FreeType renderer `libSceFontFt`, when they load in SWRR's sandbox and the font set opens. The selector loads both modules itself, renders a test glyph, and uses it only if the glyph looks sane. **Off by default:** on a 13.60 PS5 the font library's set-up crashed SWRR, so it is only tried with `--select-font auto`.
- **Otherwise the built-in font:** Noto Sans Regular, under the SIL Open Font License 1.1 (`ghostldr/native/selector/font/`, with `OFL.txt` and its provenance). It covers Latin, Greek and Cyrillic. Names in other scripts (Japanese, Chinese, Korean) show the title ID instead.

No Sony font, image or sound is in the repository. The look is drawn by the selector's own code; only the games' own art and, if it opens, the system font are loaded at run time.

## How it runs

The selector is a second runtime module, next to the extension (`ghostldr/native/ext`):

1. `ghostldr.lua` connects a socket to run.py's selector server and passes it in the boot context (`sel_fd`, `sel_size`, `sel_crc`).
2. In `ghost_boot`, after SWRR's emulator memory and display buffers are released, `sel_run` (`src/sel.c`) reads the module into the staging buffer that the compiler process shares, so the transfer allocates nothing. It checks the CRC, the magic and `GHOST_ABI`. The selector is built against the same `ghost.h` and `ghost_api.h` as the core, like the extension.
3. The module's image is copied through the compiler into the emulator's JIT code buffer, where the title's modules are placed later. The image has **no writable sections and no relocations**: `sel.ld` refuses `.data` and `.bss`, and `tools/mksel.py` refuses relocations. Placing it is one copy, and nothing has to be unmapped later. All its state lives on the stack and in its own heap.
4. `sel_main(&g_ghost_api, title)` runs on the boot thread, with the same core services the extension gets.
5. When it returns, the core overwrites the image with zeros and continues the boot.

The core side is about 1 KiB of text: `src/sel.c`, the deferral in `gfs_init` and `gfs_pick` (`ghostfs.c`), a few lines in `boot.c`, and a guard in `arena_init`. The module itself is about 122 KiB: 55 KiB of code and data, and 67 KiB of font.

### Screen and controller

- **VideoOut:** it opens the main output through the core's own `sceVideoOutOpen` override (`hle_sys.c`). That override takes the output over from SWRR if `run.py --keep-host-memory` left SWRR's open. It registers two linear 1920x1080 A8R8G8B8 scan-out buffers and flips at most at 30 Hz. A frame is drawn only when something moves; idle, it just polls the pad 30 times a second. It closes the output before it returns, so the title opens its own as before.
- **Pad:** SWRR's shared pad handle, through the core's `scePadOpen` override, read with `scePadReadState` (the leftover "intercepted" bit is ignored).
- **PNG:** the console's `libScePngDec` (`ParseHeader`, `QueryMemorySize`, `Create`, `Decode` to B8G8R8A8, `Delete`). There is a decoder per image, in temporary heap memory. Images up to 2560x1440 are decoded; larger ones fall back to the generated art.

## Memory, and how it all goes back

ghostldr's memory is tight: the title's memory plan is computed after the selector has gone, from whatever SWRR's 4.5 GiB budget holds free at that point. So the selector allocates everything it needs as two whole blocks and frees both whole:

| Block | Type | Size | Holds |
|---|---|---|---|
| heap | direct memory, write-back, CPU RW | 60 MiB for one title; plus 0.75 MiB per extra title; plus 8 MiB with the system font | the frame being drawn, two prepared backgrounds and the generated one (7.9 MiB each), each title's tile at two sizes, the glyph cache, the system font library's memory, and temporaries (a title's index, a PNG file, its decoded image, PngDec's work memory) |
| video | direct memory type 3 (write-combined), CPU+GPU RW | 16 MiB | the two scan-out buffers |

- **Placement:** both blocks go at the top of the largest free run of direct memory (`sceKernelAvailableDirectMemorySize`), away from the low hole and the top 32 MiB where the loader's own small blocks go (`mem.c`). Freeing them gives the run back in one piece, so no fragment is left between the run and anything allocated meanwhile.
- **Inside the heap:** a bump allocator with marks: temporaries are dropped as soon as they are used. The peak in the offline tests is 42 MiB.
- **Teardown, in order:**
  1. wait for pending flips;
  2. close VideoOut;
  3. close the font, renderer and library, and terminate the font memory;
  4. unload `libSceFont`, `libSceFontFt` and `libScePngDec` if the selector loaded them (a module SWRR had already loaded stays);
  5. unmap and release both blocks;
  6. after the selector returns, the core clears its code.

  A title that imports one of those libraries gets it again from the loader's on-demand host loading, at its own address. The selector never goes through the loader's module cache (`host_module_handle`), so nothing stale is left in it.
- **Accounting in the log:**

  ```
  selector: largest free run 20000000+3072 MiB; heap 60 MiB at ... (phys ...), video 16 MiB at ... (phys ...)
  selector: freed video buffers: 16 MiB at ... munmap 0, release 0
  selector: freed heap: 60 MiB at ... munmap 0, release 0
  selector: released 76 MiB of direct memory; largest free run 3072 MiB (was 3072 MiB); 7 mounts, 7 unmounts; booting title 0
  sel: module at 9c2c00000 (122 KiB) cleared; chose 0
  ```

  The two "largest free run" figures should match. The mount and unmount counts must match too.
- **Other memory:** the module image lives in the emulator's JIT buffer and costs nothing. The transfer uses the staging buffer. The system modules it loads use a little of SWRR's flexible memory, and unloading them returns it. Nothing else is allocated, and the boot thread's stack is the only stack used.

## Offline preview and tests

The drawing code is plain C over a framebuffer, so it builds and runs on the computer:

```bash
make -C ghostldr/native
python3 -m unittest ghostldr.tests.test_selector      # or discover -s ghostldr/tests
SELECTOR_SHOTS=/tmp/shots python3 -m unittest ghostldr.tests.test_selector   # keep the screenshots
```

- **`ghostldr/tests/selector/preview.c`:** the UI code against a fake catalogue with generated placeholder art. It writes PNG screenshots of a tour (moving along the row, fallbacks, an incomplete title), the information panel and the launch fade. `--art TITLE=DIR` uses a dump's `sce_sys` directory instead; the art is read at run time and never copied.
- **`ghostldr/tests/selector/host_env.c`:** runs the core's real `sel.c` and the real `selector.bin` in an x86-64 process (Rosetta on Apple silicon) against a fake console. Its save data is built by the test in ghostfs's formats. Every run must give back every block, close VideoOut and every file, unload what it loaded, unmount every container and leave the image cleared. `SELECTOR_NO_X86=1` skips these runs.

## Limits

- **Cross is always "start".** It doesn't follow a Circle-to-confirm system setting.
- **Loading art is synchronous:** each icon costs a few save-container mounts, so the row can stutter while icons first load. Backgrounds wait until the row stops.
- **The system font** (SST through libSceFont) crashed SWRR on a 13.60 PS5 during the font library's set-up; it is off unless `--select-font auto` is given.
- **No sound,** no Japanese button layout, no touchpad.
- **Background art** larger than 2560x1440 isn't decoded; a pic1 at 1920x1080, as PS4 titles ship it, is.
- At most 64 titles, the catalogue's own limit.
