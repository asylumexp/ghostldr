# The game selector

When more than one title is installed, ghostldr shows a game picker before it loads anything, laid out after the consoles' home screens: the selected game's backdrop fills the screen, with a row of rounded game tiles over it, the focused one larger and ringed, its name, details and a Start button under it, and ghostldr's mark, the user and the clock at the top. You pick a game with the controller and it boots. Everything the picker used is freed before the game loads.

On a PS4, where SWRR gets only 2.5 GiB of memory, every game's dump is greyed with a warning sign and asks before it boots; homebrew isn't ([On a PS4](#on-a-ps4)).

Each game shows its real name and art: from the game's own files when it has them, otherwise from what was stored with the install (`<prefix>META`, below), which `gfs_install.py --update-meta --fetch-art` can add to an existing install without reinstalling it.

## Controls

| Button | What it does |
|---|---|
| Left / Right (d-pad or left stick) | Move along the row. Holding repeats. |
| L1 / R1 | Jump 5 titles. |
| Cross | Boot the selected game. On a PS4 a game's dump asks first. An incomplete install, or a folder on a USB drive that can't start, can't be booted: a message says why (rerun `gfs_install.py`; or what is wrong with the folder). |
| Options or Triangle | Show the game's information: title ID, version, size, install status, container prefix; for a game on a USB drive, its folder there and any note. |
| Circle | Close the information panel. |
| In the PS4 question | Left / Right (d-pad or left stick; Up / Down too) choose Yes or No, No first. Cross takes the one chosen, Circle is No. |

There is no way back to SWRR from the picker: to leave without booting, close SWRR from the PS button menu.

## When it appears

`run.py boot` serves the selector (`build/ghostldr/selector.bin`) next to the extension, on TCP 18196. ghostfs decides whether it is needed (`gfs_init`, docs/ghostfs-catalogue.md):

- **More than one complete title installed and no `--title`:** `gfs_init` mounts nothing and returns 1. `boot.c` closes SWRR's message dialog and releases SWRR's memory, then runs the selector. It mounts the chosen title with `gfs_pick` and goes on with the boot: the title's profile is bound and its memory planned only after the selector has freed everything.
- **One complete title, or `--title T`:** that title boots as before. The core closes the selector's socket without loading the module.
- **`run.py boot --select`:** the selector shows anyway, even with one title or `--title` (run.py then sends every title profile it knows, so the title picked gets its own, and ignores `--title`). Use it to check the selector on hardware.
- **No selector** (`--no-selector`, a release without `selector.bin`, or the module fails): several titles stop the boot as before (`pick one with run.py boot --title TITLE`). With `--title`, or one title, that title boots.
- **Games on a USB drive** (`--app-root auto`, the default, or `usb:/...`; [usb.md](usb.md)): when the extension finds more than one game and no `--title` asks for one, it sets `CTX_FLAG_LIBRARY` and the selector lists what it found (`ghost_api.library()`): every game in the extension's order, second copies of a game each with their folder, and folders that can't start, with the reason. The choice goes back by its place in that list (`approot_pick`), so either copy of a game can be started.
- **Outside ghostfs** (`--app-root /data/...`), only with `--select`.

```bash
python3 ghostldr/host/run.py boot --app-root ghostfs --select
```

The selector uses its built-in font by default; `--select-font auto` tries the console's system font first (see Text). The two are ctx flags 2048 and 4096.

## What it shows

- **The list:** `GFSCATALOG/catalog.bin` (docs/ghostfs-catalogue.md). An install made with ghostldr v0.1 has no catalogue; its one title is the one in `GFSINDEX` (or `GHOSTIDX`). On a USB drive, the games the extension found, with names and art from each game's `sce_sys` and the `meta.bin` beside its `app0`. Outside ghostfs (`--app-root /data/...` on a dev console) the one title is the app root.
- **The row:** square tiles with rounded corners and a soft shadow, the focused one 384 px, lit, with a white ring a few pixels outside it, the others 256 px and a little darker. Above the row, where the games are (`INSTALLED`, `USB DRIVE`) and the selection's place in the list (`3 / 8`). Moving along the row slides the tiles on a critically damped spring (about 0.3 s, no overshoot), and the text under the row fades and slides in.
- **Under the focused tile:**
  - its name; a name too long for one line takes two, a little smaller, and then an ellipsis;
  - title ID, version, size and where it is: `Installed`, or `USB drive:` and its folder there;
  - what to know about it, as coloured labels followed by a line of text: `Install incomplete` or `Can't start` (red, with the reason), `Second copy` (blue, with which copy `--title` boots), `May not run on PS4` (amber, a game's dump on a PS4);
  - a **Start** button, greyed out when the title can't start.
- **The backdrop:** the focused game's, full screen, dimmed and shaded darker towards the top, the left (where the text is) and the bottom. It is decoded once the row stops moving and crossfades from the previous one.
- **The top bar:** ghostldr's mark (a little ghost the selector draws) on the left; on the right the signed-in user's name (`sceUserServiceGetUserName`) beside a disc with their initial, and the clock in the system's 12/24-hour setting.
- **Options:** an information panel slides in from the right: the icon and the name, the same labels, then title ID, version, size, install state, where the art came from and the save containers; for a game on a USB drive, its folder on the drive instead of size and install state, and a note (a second copy, or why it can't start). On a PS4 it also says whether the title was taken for a game's dump or homebrew.
- **Messages:** Cross on a title that can't start shows why, in a card at the bottom that fades in and out.
- **Button hints** at the bottom right, drawn as the controller's buttons: Options for the panel; in the panel and the PS4 question, Cross and Circle.

Icons are loaded one per frame, nearest to the selection first.

### Names

The first of these that the title has:

1. **Its own `sce_sys/param.sfo`:** `TITLE_xx` in the system language, else `TITLE`. From the copy in `<prefix>META`, else read through the title's index.
2. **Its profile's `name`** (`ghostldr/data/titles/<TITLE>.json`). run.py sends the profiles at boot (`GHOSTLDR_PROFILE_HEX`; `--select` sends every profile it knows); `profiles.py` compiles the name into a `REC_NAME` record that the selector reads in place, and the core ignores. Ghost of Tsushima's profile says "Ghost of Tsushima", so the v0.1 install, whose dump has no `sce_sys`, shows its name without anything installed.
3. **A name stored at install time:** `--name` given to `gfs_pack.py` or `gfs_install.py`, else the PlayStation Store's (`--fetch-art`), in `<prefix>META`; then the catalogue's.
4. **The title ID.**

Names in scripts the built-in font lacks show the title ID (see Text). Versions come from `param.sfo`'s `APP_VER`, the catalogue, or `<prefix>META` (the profile's `version` for dumps without `param.sfo`).

### Art

The tile and the backdrop each come from the first of:

1. **The title's own** `sce_sys/icon0.png` (tile) and `sce_sys/pic1.png`, else `pic0.png` (backdrop).
2. **Pictures given at pack or install time:** `--icon FILE` and `--backdrop FILE` on `gfs_pack.py` or `gfs_install.py` (PNG; JPEG and others with Pillow, or on macOS).
3. **The PlayStation Store's** (`--fetch-art`): the game's official square key art and background, found by title ID in the store's public web data (the store's title container, then its catalog concept; no key, no sign-in, one request a second). The downloads stay in `build/ghostfs/<TITLE>/art` (or `--art-dir`) and on your console: the art belongs to its owners, so it is never part of ghostldr.
4. **Generated by the selector:** a tile in a colour derived from the title ID, with a soft gradient and the game's name written on it (and its ID under it), and a dark backdrop lit in the same colour from the top right, with a fainter glow in a neighbouring hue and soft light bands.

The installer converts whatever it picks into `<prefix>META` at the sizes the selector draws. Without `<prefix>META` (installs made before it existed) the selector reads the title's own `icon0.png` and `pic1.png` through its index and decodes them with the console's `libScePngDec`, as before.

### Where names and art are stored: `<prefix>META`

A small save container next to the title's data, holding `meta.bin` (`ghostldr/host/gfs_meta.py` has the byte layout):

- a 256-byte header: title ID, the name and where it came from, the version, a content ID, and a flag saying the title's own `sce_sys` was looked at (so the selector needn't search its index);
- a copy of the title's `param.sfo`, when it has one (the selector picks the name in the system language from it);
- the tile, 384x384, and the backdrop, 1920x1080, as [QOI](https://qoiformat.org) images: lossless, decoded by the selector's own 60 lines of C in milliseconds, with no system library. Ghost's store art makes a 3 MB `meta.bin`.

The selector reads the header, the `param.sfo` copy and the tile with one read-only mount of `<prefix>META` (its first MiB), and the backdrop with one more when the row stops on the title.

`gfs_install.py` writes `<prefix>META` last, after the title is complete, and only when it changed; `--remove` deletes it with the title. It is a whole-container write (PUT) of a name ending in `META`, which can't be a data container, an index, the catalogue, a title's progress or SWRR's own save; the installer checks that before writing.

### Adding names and art to an existing install: `--update-meta`

```bash
python3 ghostldr/host/gfs_install.py --update-meta --title CUSA11456 --fetch-art
python3 ghostldr/host/gfs_install.py --update-meta --title CUSA11456 --icon tile.png --backdrop back.jpg
```

With SWRR at Luac0re's loader, this works out the title's name and art and writes **only** `<prefix>META` (`GHOSTMETA` for Ghost). The title's data containers, its index (`GFSINDEX` for a v0.1 install), its progress and the catalogue are only read, never written; the 53 GiB are not sent again.

- The title's own `sce_sys` files come from its dump when this computer has it (`--src`, or the packed one), else they are read back from the console through the title's index.
- `--name NAME`, `--icon FILE`, `--backdrop FILE`, `--fetch-art` as above; `--art-dir DIR` keeps the downloads elsewhere; `--refetch` asks the store again; `--dry-run` shows what would be written.
- Nothing worth storing (no art and no name beyond the profile's): nothing is written.
- An unchanged result: nothing is written.

`gfs_pack.py` takes the same `--name`, `--icon`, `--backdrop` and `--fetch-art` and stores the result with the pack (`build/ghostfs/<TITLE>/meta.bin`), which `gfs_install.py` installs with the title. `python3 ghostldr/host/gfs_meta.py fetch TITLE --out DIR` only downloads the store's art; `gfs_meta.py show FILE --extract DIR` unpacks a `meta.bin`.

## On a PS4

On a PS4 the kernel lets SWRR, a PS2 Classic, use about 2.5 GiB of memory ([ps4.md](ps4.md)). Most games need more; homebrew and small titles can fit. The selector learns the console from the boot context: ctx flag `CTX_FLAG_PS4` (8192), which `ghostldr.lua` sets from Luac0re's `PLATFORM`, reaches it through the core's `ghost_api` like every ctx flag (`main.c` passes it to the UI with `ui_set_platform`). On a PS4:

- **Every game's dump is marked:** its tile is washed grey, with an amber warning sign over it. Under the row it says `May not run on PS4`, and so does its information panel.
- **Cross on one asks first:**

  ```
  Game dumps largely will not work on PS4 due to RAM limitations
  Continue anyways?
  [Yes] [No]
  ```

  **No is selected.** Left / Right (d-pad or left stick, and Up / Down) move between the buttons, Cross takes the one selected, Circle answers No. No goes back to the list; Yes boots the game as usual. Cross in the information panel asks the same.
- **Homebrew** looks and boots as on a PS5.

The log says what each title was taken for, and every answer:

```
selector: module at ...: 3 title(s) installed (GFSCATALOG); a PS4: game dumps are marked and ask before they boot
selector: title 1: CUSA00001 'Starfall Odyssey' (name param.sfo) 01.00, 0 MiB, index G00001INDEX, a game's dump
selector: title 2 (CUSA00009): homebrew, by its param.sfo (CUSA00009, IV0000-CUSA00009_00-HOMEBREW00000000)
selector: PS4: asking before title 1 (CUSA00001), a game's dump, boots
selector: PS4: No for title 1 (CUSA00001)
selector: PS4: Yes for title 1 (CUSA00001): booting it
```

On a PS5 none of this shows: the flag isn't set, nothing is marked and Cross boots at once.

The selector reads the controller only (`scePadReadState`). A keyboard drives it, the question included, wherever something turns its keys into controller buttons, as Remote Play clients do (arrows to the d-pad, Enter to Cross, Escape or Backspace to Circle); SWRR's process gets no keyboard input of its own.

### A game's dump or homebrew?

The selector decides from what it reads anyway, the title's ID and its `param.sfo` (`title_is_dump` in `sfo.c`):

1. **`param.sfo`'s `CONTENT_ID` from publisher `IV` and four digits** (`IV0000-BREW00083_00-HELLOWORLD000000`): **homebrew**. Retail content IDs begin with a region's publisher code (`UP9000-`, `EP0001-`, `JP...`, `HP...`); `IV0000` is what the OpenOrbis samples and homebrew packagers use.
2. Otherwise **a retail title ID** in the catalogue's ID (or the USB folder's), `param.sfo`'s `TITLE_ID`, or the `CONTENT_ID`'s: a PS4 or PS5 game's prefix (`CUSA`, `PLAS`, `PCAS`, `PCJS`, `PLJS`, `PLJM`, `PCKS`, `PLKS`, `PPSA`) and five digits, in either case: **a game's dump**.
3. **Anything else** (`BREW00083`, `HBPR00002`, `LAPY20001`, an OpenOrbis module name such as `HELLO_WORLD`, system apps' `NPXS`, no ID at all): **homebrew**.

Doubt goes to "dump": a retail title is never missed, since its ID is a retail one even without a `param.sfo`, and homebrew packed with a game-like ID and no content ID of its own is marked, which only costs a Yes. The OpenOrbis fingerprint (`OPENORBIS-HOMEBREW`, which `profiles.py` uses) is in the executable, and the selector doesn't read executables: that would cost a mount of each title's containers for every tile.

An install without `<prefix>META` has its `param.sfo` read through its index when its tile first loads, so a title first taken for a dump by its ID can turn out to be homebrew by its `CONTENT_ID`; its tile is then drawn again unmarked, and the log says `homebrew, by its param.sfo`. The offline preview prints what the rule makes of an example: `preview --is-dump ID SFO_TITLE_ID CONTENT_ID`.

## Text

- **Labels** (`INSTALLED`, the information panel's headings) are small capitals spaced a little apart; the big name is the regular face drawn twice a pixel apart, as Noto Sans has no bold here.
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

The core side is about 1 KiB of text: `src/sel.c`, the deferral in `gfs_init` and `gfs_pick` (`ghostfs.c`), a few lines in `boot.c`, and a guard in `arena_init`. The module itself is about 149 KiB: 82 KiB of code and data, and 67 KiB of font.

### Screen and controller

- **VideoOut:** it opens the main output through the core's own `sceVideoOutOpen` override (`hle_sys.c`). That override takes the output over from SWRR if `run.py --keep-host-memory` left SWRR's open. It registers two linear 1920x1080 A8R8G8B8 scan-out buffers and flips at most at 30 Hz. It closes the output before it returns, so the title opens its own as before.
- **Drawing only what changes:** a frame is drawn only when something moves; idle, the selector just polls the pad 30 times a second. Then only the rows that changed are redrawn: the top bar for a clock tick, the band of the row and the text under it while the row moves, everything for a backdrop crossfade, the panel, the PS4 question, a message or the launch fade. Every primitive keeps to that band, and `present` copies only the changed rows into the buffer about to be shown (this frame's and the previous frame's, as the two buffers alternate). The preview checks every partial frame against a whole redraw. At the end the log says what frames cost:

  ```
  selector: 53 frames drawn (11 partial): draw 5 ms average, 6 ms at most; present 1 ms average, 6 ms at most, 1010 rows average
  ```

  (A 13.60 PS5 with two titles, a minute of moving along the row and opening the panel, before the rounded look. With it, the same PS5 with one title, the panel opened and closed: `34 frames drawn (0 partial): draw 5 ms average, 6 ms at most`. The offline preview draws its tour about a third slower than before.) Loading art happens between frames: a title's index and its PNG icon took about a second at start-up, `<prefix>META` a single read.
- **Cheap shapes:** rounded rectangles, rings and soft shadows work out their coverage per pixel only at the corners; along straight edges it is a subtraction. A tile's rounded corners and its PS4 mark are drawn into its picture once, when it loads, so moving the row costs no more than before for them.
- **Pad:** SWRR's shared pad handle, through the core's `scePadOpen` override, read with `scePadReadState` (the leftover "intercepted" bit is ignored).
- **Pictures:** `<prefix>META`'s QOI images are decoded by the module's own `meta.c`. A title's own PNGs (installs without `<prefix>META`) go through the console's `libScePngDec` (`ParseHeader`, `QueryMemorySize`, `Create`, `Decode` to B8G8R8A8, `Delete`), a decoder per image in temporary heap memory, up to 2560x1440; larger ones fall back to the generated art.

## Memory, and how it all goes back

ghostldr's memory is tight: the title's memory plan is computed after the selector has gone, from whatever SWRR's 4.5 GiB budget holds free at that point. So the selector allocates everything it needs as two whole blocks and frees both whole:

| Block | Type | Size | Holds |
|---|---|---|---|
| heap | direct memory, write-back, CPU RW | 52 MiB for one title; plus 0.82 MiB per extra title; plus 8 MiB with the system font | the frame being drawn and two prepared backdrops (7.9 MiB each), each title's tile at two sizes (384 and 256 px), the glyph cache, the system font library's memory, and temporaries (`<prefix>META`'s first MiB, a backdrop's QOI and its pixels; or a title's index, a PNG file, its decoded image, PngDec's work memory) |
| video | direct memory type 3 (write-combined), CPU+GPU RW | 16 MiB | the two scan-out buffers |

- **Placement:** both blocks go at the top of the largest free run of direct memory (`sceKernelAvailableDirectMemorySize`), away from the low hole and the top 32 MiB where the loader's own small blocks go (`mem.c`). Freeing them gives the run back in one piece, so no fragment is left between the run and anything allocated meanwhile.
- **Inside the heap:** a bump allocator with marks: temporaries are dropped as soon as they are used. The generated backdrop is drawn when needed rather than kept. The peak in the offline preview is 37 MiB for Ghost alone with its store art, 43 MiB for eight titles; on a PS5 with Ghost (`GHOSTMETA`) and a title with its own 1920x1080 `pic1.png`, 39 MiB of a 54 MiB heap.
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
  selector: largest free run 20000000+3072 MiB; heap 52 MiB at ... (phys ...), video 16 MiB at ... (phys ...)
  selector: freed video buffers: 16 MiB at ... munmap 0, release 0
  selector: freed heap: 52 MiB at ... munmap 0, release 0
  selector: released 68 MiB of direct memory; largest free run 3072 MiB (was 3072 MiB); 7 mounts, 7 unmounts; booting title 0
  sel: module at 9c2c00000 (134 KiB) cleared; chose 0
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

- **`ghostldr/tests/selector/preview.c`:** the UI code against a fake catalogue with generated placeholder art: games, one with a very long name, and two homebrew titles. It writes PNG screenshots of a tour (moving along the row, fallbacks, an incomplete title, a clock tick), the information panel, the launch fade, games on a USB drive (`--scene usb --usb`), a PS4 (`--scene ps4 --ps4`: the marked tiles, the question with No selected, No, Circle, a homebrew title and its panel, then Yes), and a long name (`--scene long --start 8`), and prints what frames cost. Without `--ps4` the PS4 scene shows that the first Cross boots. `--meta TITLE=meta.bin` reads a `<prefix>META` file as the console does; `--art TITLE=DIR` takes a dump's `sce_sys` (`icon0.png`, `pic1.png`) or what `gfs_meta.py fetch` saved (`icon.png`, `backdrop.png`); `--user NAME` fills the top bar; `--ps4` runs as on a PS4; `--start N` starts at title N; `--verify` compares every partial frame with a whole redraw; `--is-dump ID SFO_TITLE_ID CONTENT_ID` prints whether the selector takes those for a game's dump. The art is read at run time and never copied.

  ```bash
  python3 ghostldr/host/gfs_meta.py fetch CUSA11456 --out ../art/CUSA11456          # outside the repository
  python3 ghostldr/host/gfs_meta.py build --title CUSA11456 --fetch-art --art-dir ../art/CUSA11456 --out /tmp/ghost-meta.bin
  cc -O2 -Ighostldr/native/selector/src ghostldr/tests/selector/{preview,png_host}.c \
     ghostldr/native/selector/src/{ui,draw,font,sfo,meta}.c -lz -lm -o /tmp/preview
  /tmp/preview --font ghostldr/native/selector/font/notosans.glf --out /tmp/shots --scene tour \
     --meta CUSA11456=/tmp/ghost-meta.bin --user Player --verify
  ```
- **`ghostldr/tests/selector/host_env.c`:** runs the core's real `sel.c` and the real `selector.bin` in an x86-64 process (Rosetta on Apple silicon) against a fake console. Its `ps4` mode sets `CTX_FLAG_PS4` over three titles (homebrew by its ID, a game, and homebrew by its `CONTENT_ID`) and answers the question No, Circle, then Yes. Its save data is built by the test in ghostfs's formats. Every run must give back every block, close VideoOut and every file, unload what it loaded, unmount every container and leave the image cleared. `SELECTOR_NO_X86=1` skips these runs.

## Limits

- **Cross is always "start".** It doesn't follow a Circle-to-confirm system setting.
- **Loading art is synchronous:** each icon costs a save-container mount (a few, for an install without `<prefix>META`), so the row can stutter while icons first load. Backdrops wait until the row stops.
- **`--fetch-art` needs the store to know the title ID:** it asks the US, UK, Japanese and Hong Kong stores. Titles delisted everywhere get no art; `--icon` and `--backdrop` still work.
- **The system font** (SST through libSceFont) crashed SWRR on a 13.60 PS5 during the font library's set-up; it is off unless `--select-font auto` is given.
- **No sound,** no Japanese button layout, no touchpad, no keyboard of its own (see [On a PS4](#on-a-ps4)).
- **On a PS4, which titles are marked** is a rule over IDs ([A game's dump or homebrew?](#a-games-dump-or-homebrew)), not a measure of what the title needs: a small game is marked too, and homebrew that needs more than 2.5 GiB isn't.
- **Background art** larger than 2560x1440 isn't decoded; a pic1 at 1920x1080, as PS4 titles ship it, is.
- At most 64 titles, the catalogue's own limit.
