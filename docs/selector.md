# The game selector

Before it loads anything, ghostldr shows a game picker, laid out after the consoles' home screens: the selected game's backdrop fills the screen, with a row of rounded game tiles over it, the focused one larger and ringed, its name, details and a Start button under it, and ghostldr's mark, the user and the clock at the top. You pick a game with the controller and it boots. Everything the picker used is freed before the game loads.

The row ends with a **Settings** tile (a gear; Square opens it from anywhere in the row): the drive check that `ghostldr.py check` did on a computer, the USB drives, the network installer, the log on the USB drive and what ghostldr, the console and SWRR are ([Settings](#settings)). The picker shows with one title too, and with none: then the row is the Settings tile alone, with what to do under it.

On a PS4, where SWRR gets only 2.5 GiB of memory, every game's dump is greyed with a warning sign and asks before it boots; homebrew isn't ([On a PS4](#on-a-ps4)).

A save tool such as Apollo Save Tool asks first whose saves to open, listing the console's users, and then looks for that user's saves with a progress bar when that takes long, before the tool starts ([Save tools](#save-tools-apollo-save-tool)). It does so even when it is the only title installed.

Each game shows its real name and art: from the game's own files when it has them, otherwise from what was stored with the install (`<prefix>META`, below), which `gfs_install.py --update-meta --fetch-art` can add to an existing install without reinstalling it.

## Controls

| Button | What it does |
|---|---|
| Left / Right (d-pad or left stick) | Move along the row. Holding repeats. |
| L1 / R1 | Jump 5 titles. |
| Cross | Boot the selected game. On a PS4 a game's dump asks first. An incomplete install, or a folder on a USB drive that can't start, can't be booted: a message says why (rerun `gfs_install.py`; or what is wrong with the folder). |
| Options | Show the game's information: title ID, version, size, install status, container prefix; for a game on a USB drive, its folder there and any note. |
| Triangle | Refresh: look for games again (the USB drives', or the installed ones): games copied or removed since show or go, without restarting SWRR ([Refresh](#refresh)). |
| Square | Open Settings (as does Cross on the gear at the end of the row). |
| In Settings | Up / Down move between the sections on the left; Right or Cross goes into the open one, where Up / Down move between its entries and the list scrolls; Cross changes a switch or starts an action; Left or Circle comes back out; Circle on the sections, or Square, closes Settings. |
| In Install over network | Circle stops the installer and closes the card; while a browser is installing, hold it for a second (the install can go on later). |
| In Settings › Games | Cross on a game installed in saved data deletes it, after a question (No first): [Deleting an installed game](#deleting-an-installed-game). |
| On Game art's keyboard | The d-pad moves, Cross types the key, Square deletes the last character, Triangle is Shift, Options (or the Done key) saves, Circle leaves it as it was. |
| Circle | Close the information panel. |
| Square, in the information panel | Turn the game's community memory plan off for that game, or on again. Only when the panel shows one; otherwise Square opens Settings as anywhere in the row. |
| In the PS4 question | Left / Right (d-pad or left stick; Up / Down too) choose Yes or No, No first. Cross takes the one chosen, Circle is No. |
| In a save tool's question (whose saves) | Up / Down (d-pad or left stick) move between the users. Cross opens the saves of the one chosen; on a user who isn't signed in it says why instead. Circle goes back to the list (not when the save tool is the only title, or after a scan was cancelled). |
| While a save tool's saves are looked for | Hold Circle to cancel: the question comes back. |

**While a game runs, L1 + R1 + Options + touchpad** (held together for a moment) stops it and brings the game selector back. A game that quits or crashes comes back to the selector too, after a few seconds of ghostldr's page saying what happened ([Back to the game selector](#back-to-the-game-selector)).

To leave without booting: Settings › About › **Close ghostldr** (after a Yes, SWRR is back at Luac0re's loader, which takes ghostldr.lua again), or close SWRR from the PS button menu.

## When it appears

`run.py boot` serves the selector (`build/ghostldr/selector.bin`) next to the extension, on TCP 18196. ghostfs decides whether it is needed (`gfs_init`, docs/ghostfs-catalogue.md):

- **A selector came with the boot and no `--title`** (the release's two-file boot, `run.py boot`): the selector shows, whatever is installed. `gfs_init` mounts nothing and returns 1: with several titles as before, and now with one, or with none (`no GFSCATALOG, GFSINDEX or GHOSTIDX (...): nothing installed`, then the selector). `boot.c` closes SWRR's message dialog and releases SWRR's memory, then runs the selector. It mounts the chosen title with `gfs_pick` and goes on with the boot: the title's profile is bound and its memory planned only after the selector has freed everything. With none installed, the screen says what to do and has Settings (Install over network); Close ghostldr ends the boot (`SEL_EXIT_LOADER`, -20, which `ghost_boot` returns as it is).
- **`--title T`:** that title boots as before. The core closes the selector's socket without loading the module. **Except a save tool** (its profile's `save_tool` feature, [below](#save-tools-apollo-save-tool)): then the selector shows that title alone, with the question whose saves to open already up.
- **`run.py boot --select`:** the selector shows anyway, even with one title or `--title` (run.py then sends every title profile it knows, so the title picked gets its own, and ignores `--title`). Use it to check the selector on hardware.
- **No selector** (`--no-selector`, a release without `selector.bin`, or the module fails): several titles stop the boot as before (`pick one with run.py boot --title TITLE`). With `--title`, or one title, that title boots; with none, the boot stops (`nothing installed`).
- **Games on a USB drive** (`--app-root auto`, the default, or `usb:/...`; [usb.md](usb.md)): when the extension finds more than one game and no `--title` asks for one, it sets `CTX_FLAG_LIBRARY` and the selector lists what it found (`ghost_api.library()`): every game in the extension's order, second copies of a game each with their folder, and folders that can't start, with the reason. The choice goes back by its place in that list (`approot_pick`), so either copy of a game can be started. One game on a drive is mounted by the extension and shown alone (with its folder), with the Settings tile. With games on more than one drive, each shows its drive: `USB drive 2: folder`.
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
  - what to know about it, as coloured labels followed by a line of text: `Install incomplete` or `Can't start` (red, with the reason), `Second copy` (blue, with where the other copy is, such as `The same game is also in USB drive 1: ghostldr/CUSA11456`: either starts from the row), `May not run on PS4` (amber, a game's dump on a PS4);
  - a **Start** button, greyed out when the title can't start.
- **The backdrop:** the focused game's, full screen, dimmed and shaded darker towards the top, the left (where the text is) and the bottom. It is decoded once the row stops moving and crossfades from the previous one.
- **The top bar:** ghostldr's mark (a little ghost the selector draws) on the left; on the right the signed-in user's name (`sceUserServiceGetUserName`) beside a disc with their initial, and the clock in the system's 12/24-hour setting.
- **Options:** an information panel slides in from the right: the icon and the name, the same labels, then title ID, version, size, install state, where the art came from and the save containers; for a game on a USB drive, its folder on the drive instead of size and install state, and a note (a second copy, or why it can't start). On a PS4 it also says whether the title was taken for a game's dump or homebrew. When the game's profile has a community memory plan for this console (a `REC_SEED` for PS5 or PS4: [community-plans.md](community-plans.md)) and **Use community memory plans** is on, a **Memory plan** row says `Community plan from 12 consoles` (the most of the game's builds), with a switch: Square turns it off for that game (`Off for this game: it learns its own from scratch`) and on again. A game with no such plan, because none was shared or the computer couldn't download them (a GitHub rate limit included), has no row and no message.
- **Messages:** Cross on a title that can't start shows why, in a card at the bottom that fades in and out.
- **Button hints** at the bottom right, drawn as the controller's buttons: Options for the panel; in the panel and the PS4 question, Cross and Circle; in the panel of a game with a community plan, Square too (`Turn off community plan`, `Use community plan`).

Icons are loaded one per frame, nearest to the selection first.

### Names

The first of these that the title has:

1. **Its own `sce_sys/param.sfo`:** `TITLE_xx` in the system language, else `TITLE`. From the copy in `<prefix>META`, else read through the title's index.
2. **Its profile's `name`** (`ghostldr/data/titles/<TITLE>.json`). run.py sends the profiles at boot (`GHOSTLDR_PROFILE_B64`; `--select` sends every profile it knows); `profiles.py` compiles the name into a `REC_NAME` record that the selector reads in place, and the core ignores. Ghost of Tsushima's profile says "Ghost of Tsushima", so the v0.1 install, whose dump has no `sce_sys`, shows its name without anything installed.
3. **A name stored at install time:** `--name` given to `gfs_pack.py` or `gfs_install.py`, else the PlayStation Store's (`--fetch-art`), in `<prefix>META`; then the catalogue's.
4. **The title ID.**

Names in scripts the built-in font lacks show the title ID (see Text). Versions come from `param.sfo`'s `APP_VER`, the catalogue, or `<prefix>META` (the profile's `version` for dumps without `param.sfo`).

### Art

The tile and the backdrop each come from the first of:

1. **The title's own** `sce_sys/icon0.png` (tile) and `sce_sys/pic1.png`, else `pic0.png` (backdrop).
2. **Pictures given at pack or install time:** `--icon FILE` and `--backdrop FILE` on `gfs_pack.py` or `gfs_install.py` (PNG; JPEG and others with Pillow, or on macOS).
3. **The PlayStation Store's** (`--fetch-art`): the game's official square key art and background, found by title ID in the store's public web data (the store's title container, then its catalog concept; no key, no sign-in, one request a second). The downloads stay in `build/ghostfs/<TITLE>/art` (or `--art-dir`) and on your console: the art belongs to its owners, so it is never part of ghostldr.
4. **Fetched by the selector** (Settings › Game art, off by default): SteamGridDB's or IGDB's, with the player's own keys, by the game's name ([Game art](#game-art)), and kept in the same `<prefix>META` (or `meta.bin`).
5. **Generated by the selector:** a tile in a colour derived from the title ID, with a soft gradient and the game's name written on it (and its ID under it), and a dark backdrop lit in the same colour from the top right, with a fainter glow in a neighbouring hue and soft light bands.

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

## Save tools (Apollo Save Tool)

A save tool works on other games' saves, and those belong to one user. Apollo Save Tool for PS4 is one. Before such a tool starts, the selector asks whose saves it should open, looks for that user's saves, and only then starts the tool, for that user. Nothing on the computer is needed for it: the release's `ghostldr.lua` carries every profile in `ghostldr/data/titles`.

**Which titles:** those whose profile has the `save_tool` feature (`PF_SAVE_TOOL` in the compiled profile; [compatibility.md](compatibility.md)). `ghostldr/data/titles/APOL00004.json`, Apollo Save Tool (title ID `APOL00004`, content ID `IV0000-APOL00004_00-APOLLO0000000PS4`), has it, with no eboot fingerprint, so every build of Apollo gets it. The selector reads the flag from the profiles in place, as it reads their names.

**The question:** Cross on the save tool (after a Yes on a PS4, if it were marked there) opens a card over the screen: the tool's icon, "Which user's saves to open Apollo Save Tool for?" and the console's users, each with the initial on a disc and their name (`sceUserServiceGetUserName`):

- **Who is listed:** every user registered on the console when SWRR may list them (`sceUserServiceGetRegisteredUserIdList`; it may on a 13.60 PS5), else the users signed in now (`sceUserServiceGetLoginUserIdList`), and always the user running SWRR. Up to 16, in the console's order; past seven rows the list scrolls with the focus. The name of a user who isn't signed in can't be read on a PS5: they show as "User 1", "User 2", … by their place in the list.
- **Signed in:** they can be chosen. The user running SWRR says "Signed in · playing now" and has the focus at first.
- **Not signed in:** greyed, with one line saying why: "Not signed in · sign in with another controller to open their saves". The save service opens only a signed-in user's saves (any other answers `0x809F0011`, `INVALID_LOGIN_USER`), and on a PS5 a second user is signed in with a second controller. The focus can rest on them, but Cross only says so. Who is signed in is read again every second, so a user who signs in with another controller meanwhile becomes available.

**The scan:** Cross on a user mounts the title at once (`ghost_api.title_pick`: what the core would otherwise do after the selector returns) and runs the save tool's scan (`ghost_api.save_prescan`, the extension's): it asks the save service, title by title, which games the user has saves of, and remembers the answers for the tool. Each question takes about 0.4 s and Apollo knows about 350 titles, so a first scan can take over two minutes; later ones ask only about what the cache doesn't know. While it runs:

- **A short scan shows nothing new:** the card says "Looking for saves…" and the tool starts.
- **A long one shows its progress:** once it has taken 1.5 s, or as soon as it looks like taking 2.5 s in all, the question gives way to a card with "Looking for <user>'s saves", a bar, "120 of 349 games checked" and the time left ("About 1 min 30 s left"). The time left comes from the measured time per title: the average, and the recent pace smoothed, which it follows as it changes. At the end the full bar stays for a moment.
- **Hold Circle to cancel.** The pad is read between two titles (the save service blocks the thread while it answers), so a quick tap can be missed: the hint says to hold it. The scan stops after the title it is on, keeps what it found, and the question comes back; the title stays mounted, so there is no going back to the list. Choosing a user again goes on where it stopped.

Then the title starts with the core's `g_title_user` (`ghost_api.title_user`) set to the chosen user: the tool's user service answers, save lists and mounts are for that user (the extension's side), while ghostldr's own containers and the tool's own settings save stay the SWRR user's. Without the selector (`--no-selector`, a release without `selector.bin`) the tool runs for the user running SWRR, with no scan beforehand.

**The interface** (`ghost_api.h`): `title_user` (0: the user running SWRR), `title_pick(title, index)` (once; boot.c doesn't pick again afterwards) and `save_prescan(user, progress, arg)`, which the core forwards to the extension's early entry (`struct ext_early.save_prescan`). The extension's code is placed for it before the title's modules, in the new JIT block it would have had anyway (as for a USB drive's games), and its `ext_init` still runs at its usual place. The scan calls `progress(arg, 0, total)` once it knows how many titles it will ask about, then `progress(arg, done, total)` after each one; a nonzero answer stops it. It runs on the boot thread while the selector's 68 MiB are allocated, after SWRR's emulator memory was released and before the title's memory is planned, so whatever it allocates it frees.

The log says what happened:

```
selector: users: 2 (registered list 0, signed-in list 0)
selector: user 10000002 '', not signed in
selector: user 10000001 'Player', signed in, running SWRR
selector: save tool: asking whose saves to open for title 0 (APOL00004): 2 user(s)
selector: save tool: user 10000001 chosen for title 0 (APOL00004)
selector: title 0 (APOL00004) mounted for its save scan: 0
ext: code at 904f34000 in a new JIT block: text 74000, data 28000 -> 0
selector: save tool: a long scan (1 of 24 after 668 ms, about 15 s left): its progress is shown
selector: save tool: Circle: cancelling the scan at 9 of 24
selector: save tool: scan cancelled after 9 of 24 titles, 2747 ms: 0
selector: save tool: user 10000001 chosen for title 0 (APOL00004)
selector: save tool: scan done after 24 of 24 titles, 6211 ms: 0
selector: title 0 (APOL00004) opens the saves of user 10000001
selector: released 70 MiB of direct memory; largest free run 3292 MiB (was 3292 MiB); 5 mounts, 5 unmounts; booting title 0
boot: saves of user 10000001
```

(A 13.60 PS5 with the extension's stand-in scan, `ext/src/save_prescan.c`, which reports 24 titles of 250 ms; user IDs and the name replaced.)

## Settings

A sheet over the selector, in its look: the sections on the left, the open one's entries on the right. It is read again each time it opens (a drive plugged in meanwhile shows). It writes only ghostldr's own `GLDRCONF` (the switches and Game art's keys), and deletes only what a Yes to "Delete …?" names.

- **Games:** every title the selector has, installed or on a drive, each with a coloured label and, under it, what is wrong and what to do, in plain words: `Ready`; `Install incomplete` (install it again: it goes on where it stopped); `Can't start` (the extension's reason for that folder, such as `its app0 folder has no eboot.bin (copied in full?)`, and to fix the folder on a computer); `Second copy` (which folder has the other); `May not run on PS4`. A heading counts them: `3 GAMES · 2 READY · 1 TO LOOK AT`. PS5 games found on a drive aren't in the row (SWRR runs in PS4 mode, so they can't start): a line at the top counts them, `2 PS5 games on the USB drive are left out: SWRR runs PS4 games only.` Cross on a game installed in saved data deletes it ([below](#deleting-an-installed-game)); on a USB boot the games in saved data are listed under the row's, and can be deleted the same way.
- **USB drives:** each drive the console has, read only, from `libSceUsbStorage`'s device list and each drive's `sceUsbStorageGetDeviceInfo`: its maker and model, its file system (exFAT, FAT32, or unknown: the features word's 0x80 and 0x40), its size and free space, the games found on it (by the mount point their folders are under: `/ghostldr_usb`, `/ghostldr_usb2`, ... in the device list's order) and the folders that can't start. A FAT32 drive says that a file of 4 GiB or more can't be on it and most PS4 games have one. Then what ghostldr looks for on a drive, where the log goes, and **Look again**: the drives read again, and the row with them, as Triangle does ([Refresh](#refresh)). On a PS4 and in Jak X, where ghostldr doesn't read drives, it says so. The selector opens the USB storage session itself when the extension didn't (an app root given), as the extension would, and leaves it open for the title (`CTX_FLAG_USB_SESSION`).
- **Network:** the console's address (ghostldr.lua's in the two-file boot, else `sceNetCtlGetInfo`), **Install over network**, and the ports ghostldr's features use: Luac0re's loader (9026; UltraC0re's 8888), the bundle port and ELF loader (9021, or 9022 when 9021 was taken; the ELF loader's own when it listens), FTP while a game runs (`CTX_FTP_PORT`, 1212 by default), the network installer (8090).
- **Logs:** **Write logs to the USB drive**, On or Off (a switch, below: the extension's `CTX_FLAG_NO_USB_LOG`), and where the log goes (the extension's, once it has decided: [logs.md](logs.md)). On a PS4 and in Jak X, where drives aren't read, it says so.
- **Advanced:** the switches that were `run.py` flags (below), and **Reset to defaults**.
- **Game art:** **Fetch missing art** (a switch, below), and the keys it uses, typed on a keyboard card: **SteamGridDB API key**, **IGDB Client ID** and **IGDB Client Secret**, each shown by its first and last four characters ([Game art](#game-art)).
- **About:** ghostldr's version (`git describe` when the selector was built), the build (the bundle's build ID when the cached bundle, `GLDRCACHE/bundle.bin`, has this boot's selector; else how this boot came), the bundle cache (empty, this build's, another build's), the console and its firmware (the boot context's `fw`, from Luac0re's `FW_VERSION`), the host game (its own `/app0/sce_sys/param.sfo` where it can be read: name, region by title ID, version; else Luac0re's `TITLE_ID`, which ghostldr.lua leaves 48 bytes into the console address's buffer, `CTX_CONSOLE_IP_SLOT`), and **Close ghostldr** (a question, No first; Yes: `SEL_EXIT_LOADER`, and the TV says "Closed in Settings. Back to Luac0re's loader.").

### Switches and where they are kept

A user of a release has no `run.py`, so what its flags did that a user may need is a switch in Settings, each a boot-context flag with one line saying what On and Off do:

| Switch | Where | Flag | Default | Was |
|---|---|---|---|---|
| Write logs to the USB drive | Logs | `CTX_FLAG_NO_USB_LOG` set when Off | On | the log's own setting |
| Show the system's dialogs | Advanced | 0x400000 (dialogs answered at once) set when Off | On | `--dialog-auto` |
| Plan memory automatically | Advanced | `CTX_FLAG_NO_AUTO_MEM` set when Off | On | `--no-auto-memory` |
| Keep Remote Play on | Advanced | `CTX_FLAG_KEEP_REMOTE_PLAY` set when On | Off | `--keep-remote-play` |
| Share learned memory plans | Advanced | `CTX_FLAG_SHARE_MEM` set when On | Off | `--share-memory` |
| Use community memory plans | Advanced | `CTX_FLAG_NO_COMMUNITY_MEM` set when Off | On | `--no-community-memory` |
| Saves in FTP | Advanced | `CTX_FLAG_FTP_SAVES` set when On | Off | `--ftp-saves` |
| Fetch missing art | Game art | `CTX_FLAG_FETCH_ART` (bit 31) set when On | Off | `--fetch-art` |

**Share learned memory plans** only makes the console log each plan it saves (`automem: plan`); the ghostldr app or `run.py`, when it started the boot, sends those lines to ghostldr's collection. The console never connects anywhere for it. **Use community memory plans** lets a game with no plan of its own on this console start from the one in its profile, and keep learning; a game's Information panel can turn it off for that game alone. **Saves in FTP** adds `/saves` to the FTP server, every logged-in user's saves, read and written as a save tool under ghostldr does ([ftp.md](ftp.md#saves)); the server reads the switch at each request, so it counts at once, and in the game too.

They are kept in a save container of ghostldr's own, `GLDRCONF/settings.txt` (96 blocks, made the first time a switch changes), which only the selector writes (`selector/src/conf.c`):

```
# ghostldr settings: the game selector's Settings writes this file
remote_play=on
flags_on=0x200
flags_off=0x0
```

A key for each switch that isn't at its default, and the two masks they make; Game art's keys when they are set (`steamgriddb_key=`, `igdb_client_id=`, `igdb_client_secret=`: never in the log); then `community_off=CUSA00001,CUSA11456`, the games whose community plan was turned off in their Information panel (at most 511 bytes: the oldest go first; Reset to defaults leaves it). When a game is started the selector sets `CTX_FLAG_NO_COMMUNITY_MEM` in this boot's flags if the switch is off or the game is in that list, and clears it otherwise, unless it was set before the selector (`run.py --no-community-memory`), which stands. A change counts at once: the selector sets or clears the flag in this boot's context. The release's ghostldr.lua sets `flags_on` and clears `flags_off` in the flags it boots with, for the selector's call and again (read once more) for the game's, so a change made in the selector reaches the game picked right after. `run.py` doesn't read the file: a developer's flags stand, and Advanced says that boot started without the switches.

**Safe mode.** A switch must not lock anyone out. ghostldr.lua writes `pending=1` before it uses the masks, and the selector clears it when it comes up. A boot that finds `pending=1` (the last one with the switches never reached the selector) goes to the selector without them, once: it writes `pending=safe`, the TV says "Settings > Advanced is left out of this boot", and so do a message in the selector and a note in Advanced. The game's call after the selector uses them again (the selector was reached). **Reset to defaults** puts every switch back (the log's too) and empties the masks.

A new switch is one `case` in `conf_switch` (key, words, flag, whether On sets it, default): ghostldr.lua needs no change. Keep its flag below bit 32 if the selector's own call needs it (that call gets the low 32 bits only).

The log says what happened:

```
ghostldr: settings: flags 4bc000000003002 -> 4bc000000003202
selector: settings: GLDRCONF/settings.txt: 121 bytes, switches f, flags_on 200, flags_off 0 (this boot has them); pending: cleared
selector: settings: GLDRCONF/settings.txt written: 0 (switches 7: flags_on 0, flags_off 0)
selector: settings: reset to defaults: 0
selector: settings: community memory plan off for CUSA00001: 0 (community_off CUSA00001)
selector: CUSA00001: a community memory plan from 12 console(s): off for this game (CTX_FLAG_NO_COMMUNITY_MEM set)
```

### Refresh

Triangle in the row (and **Look again** in Settings › USB drives) looks for games again without restarting SWRR. "Looking for games again…" shows, then what changed: `1 new game on the USB drives`, `2 games gone`, `No change: 5 games on the USB drives`.

- **Games on USB drives** (the row is a library: `CTX_FLAG_LIBRARY`): the extension looks at the drives as an FTP login does (a drive that went is forgotten, one plugged in since is mounted, up to four), lists the games in the same folder on each again (`ghost_api.library_scan`, `approot.c lib_again`), and the selector reads the row from that list. A game still there keeps its tiles and what was read of it (by its folder: two copies share an ID); a new one takes the tiles of one that went, else new ones while the heap has room (the row's titles and 8 more). Cross then picks from the new list by its place, as before.
- **Games installed in saved data:** `GFSCATALOG` is read again, as after an install over the network.
- **Not possible:** a boot that started with no game on a drive shows the installed ones; a drive's games show when ghostldr starts with it plugged in (switching the boot's app root under a running selector isn't done). A boot given one game (`run.py --app-root`, `--title`) has nothing to look for.

The log: `approot: looked again: 4 games in 1 folder`, `selector: refresh: 3 title(s) (had 3: 1 new, 1 gone)`, `refresh: CUSA00001 'Starfall' is gone from the drives (its tiles reused)`.

### PS5 games on a drive

A folder whose game is a native PS5 title is left out of the row: its title ID starts with `PPSA` (`param.json`'s, or one in the folder's name: `homebrew/PPSA17419-app0`), or it has `sce_sys/param.json` and no `sce_sys/param.sfo`. SWRR runs in PS4 mode and can't start one. The extension logs each (`approot: dumps/PPSA17419-app0: a PS5 game (PPSA17419): not listed, SWRR runs PS4 games`), Settings › Games counts them, and `ghostldr.py check` says so for each (`usb_drive.py ps5_game`, the same rule).

### Deleting an installed game

In Settings › Games, Cross on a game installed in saved data asks "Delete *name*? Its install in saved data (53.3 GB) is removed. Its saves stay." with No selected first. After a Yes, "Deleting…" shows, then the selector does what `gfs_install.py --remove` does, in an order a power cut can't make worse (`cat.c cat_delete`):

1. its `GFSCATALOG` entry is marked incomplete;
2. its index (`<prefix>INDEX`, or the legacy `GFSINDEX` its entry names) is deleted;
3. its data containers `<prefix>000`… whose `gfs.tag` names its layout and their number, or, with no tag, within the count its entry has; the search stops after four missing names in a row, and a container that isn't the install's is left and logged;
4. `<prefix>META`, when its `meta.bin` names the same title;
5. the entry leaves `GFSCATALOG`, and the row is read again.

The deletes are SWRR's own `sceSaveDataDelete`, for the user running SWRR. **The game's progress stays:** `<prefix>SAVE`, `<prefix>D<nnn>SAVE` and `<prefix>M<nn>SAVE` are never deleted from here, nor ghostldr's own containers (`GLDR…`, `GFS…`) or the host's save; a prefix that isn't one `gfs_install.py` gives is refused. Installing the game again later finds its saves. Games on a USB drive aren't deleted from the selector (a computer does that).

### Game art

With **Fetch missing art** on and a key set, a game (not homebrew) with no tile picture or no backdrop of its own gets them while the row shows, looked up by its name (`selector/src/art.c`, `plansync/artfetch.c`):

- **SteamGridDB** (an API key: steamgriddb.com › Preferences › API): the game by name (`/api/v2/search/autocomplete/`), a square static grid (512x512 or 1024x1024) for the tile and a static hero (1920x620 or 1600x650) for the backdrop, PNG only (`mimes=image/png`).
- **IGDB** (a Twitch application's Client ID and a secret: dev.twitch.tv › Console › Applications): an app access token from `id.twitch.tv` (kept for the boot), then the game by name among PS4 games (platform 48), its cover (`t_cover_big_2x`) and an artwork (`t_1080p`), both as PNG from `images.igdb.com`.

SteamGridDB is asked first for each picture, then IGDB for what is still missing. A service that refuses its key isn't asked again that boot (the log says so). Only PNG is used: the selector decodes nothing else (TheGamesDB and RAWG serve JPEG only, so they aren't offered). The requests are HTTPS (BearSSL, the trust anchors in `plansync_ta.c`: Google Trust Services for SteamGridDB, Amazon for Twitch and IGDB) with the console's DNS servers, then 1.1.1.1 and 8.8.8.8, as for community plans; the sockets are made on the boot thread a few at a time, so it costs no JIT memory.

A picture found shows at once and is kept: cropped to fill 384x384 (tile) and 1920x1080 (backdrop), QOI-encoded into the title's `<prefix>META` (made when missing, made again larger when it doesn't fit; what it held stays) or, for a game on a USB drive, `meta.bin` beside its `app0`. Their sources are `ART_SGDB` (5) and `ART_IGDB` (6); the information panel says "SteamGridDB" or "IGDB", and `gfs_meta.py show` reads them. The next boots read it from there and ask nothing. Changing the switch or a key takes effect at once. The art belongs to its owners: it stays on the console.

The log: `art: title 2 (CUSA00002 'Neon Drift') lacks a picture a backdrop: asked (0)`, `art: SteamGridDB for Neon Drift`, `art: title 2 (CUSA00002 'Neon Drift'): icon SteamGridDB, backdrop IGDB`, `art: title 2 (CUSA00002): G00002META, 3120 KiB: 0`.

### Install over network

**Install over network** starts the network installer ([manager.md](manager.md)): a web server that installs a game into saved data from a browser on a phone or computer. It is part of the selector module (`native/netinst`, linked in when it is in the tree), so it runs in the emulator's JIT buffer like the rest of the selector and costs no new JIT memory; it uses its own memory and gives all of it back when it stops. A card shows:

- the address to open (big), and the token the page asks for before it changes anything;
- while a browser installs: the game's name and ID, a bar, how much of how much, the speed and the time left;
- the installer's last message, and how many games it installed.

Circle stops it (held for a second while a browser is installing; the install can go on later). The selector always stops it before it returns. When an install finishes, the selector reads the catalogue again and the new game joins the row (there is room for 8 more titles than were installed at the start), and choosing it boots it: `gfs_mount_title` reads the catalogue again when the title chosen isn't in the one it read at the start. On a boot from a USB drive's games the list is the drive's: a game installed then shows on a boot without games on the drive.

The interface, in `netinst/netinst.h`: `netinst_start(api, &handle)` (on the boot thread; 0, or < 0 with the reason in the status), `netinst_status(handle, &status)` (a snapshot: state, address, token, title, bytes done of total, speed, installs finished, a message), `netinst_stop(&handle)`.

The log says what happened:

```
selector: settings: opened (2 title(s); USB 1, 2 drive(s); logs to USB 1; network installer 1; 192.168.0.20)
selector: settings: drive 0: id 1a2b3c4d, info 0, 'Example SSD', 3446890 of 3907018 MiB free, features 80, 1 game(s), 0 bad
selector: settings: network installer started: 0
selector: settings: network install finished (1 this session): Skyward Harbour
selector: refresh: 3 title(s) (had 2: 1 new, 0 gone)
selector: network installer stopped
```

## Text

- **Labels** (`INSTALLED`, the information panel's headings) are small capitals spaced a little apart; the big name is the regular face drawn twice a pixel apart, as Noto Sans has no bold here.
- **The console's system font (SST)**, through `libSceFont` and its FreeType renderer `libSceFontFt`, when they load in SWRR's sandbox and the font set opens. The selector loads both modules itself, renders a test glyph, and uses it only if the glyph looks sane. **Off by default:** on a 13.60 PS5 the font library's set-up crashed SWRR, so it is only tried with `--select-font auto`.
- **Otherwise the built-in font:** Noto Sans Regular, under the SIL Open Font License 1.1 (`ghostldr/native/selector/font/`, with `OFL.txt` and its provenance). It covers Latin, Greek and Cyrillic. Names in other scripts (Japanese, Chinese, Korean) show the title ID instead.

No Sony font, image or sound is in the repository. The look is drawn by the selector's own code; only the games' own art and, if it opens, the system font are loaded at run time.

## How it runs

The selector is a second runtime module, next to the extension (`ghostldr/native/ext`):

1. `ghostldr.lua` connects a socket to run.py's selector server and passes it in the boot context (`sel_fd`, `sel_size`, `sel_crc`).
2. In `ghost_boot`, after SWRR's emulator memory and display buffers are released, `sel_run` (`src/sel.c`) reads the module into the staging buffer that the compiler process shares, so the transfer allocates nothing. It checks the CRC, the magic and `GHOST_ABI`, and takes what it needs from the header before `arena_init`: on a PS4 the arena's probes of the JIT buffers copy through the staging buffer's first bytes, over the header (reading the image's offset afterwards sent the compiler to a wild address and SWRR stopped there). The selector is built against the same `ghost.h` and `ghost_api.h` as the core, like the extension.
3. The module's image is copied through the compiler into the emulator's JIT code buffer, where the title's modules are placed later. The image has **no writable sections and no relocations**: `sel.ld` refuses `.data` and `.bss`, and `tools/mksel.py` refuses relocations. Placing it is one copy, and nothing has to be unmapped later. All its state lives on the stack and in its own heap.
4. `sel_main(&g_ghost_api, title)` runs on the boot thread, with the same core services the extension gets.
5. When it returns, the core overwrites the image with zeros and continues the boot.

The core side is about 1.5 KiB of text: `src/sel.c`, the deferral in `gfs_init` and `gfs_pick` (`ghostfs.c`), a few lines in `boot.c` (with a save tool's `title_pick` and `save_prescan`, and Close ghostldr's `SEL_EXIT_LOADER`), and a guard in `arena_init`. The module itself is about 197 KiB: 130 KiB of code and data (Settings is `setui.c` and `info.c`), and 67 KiB of font; the network installer adds about 48 KiB (24 KiB of it its page).

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
| heap | direct memory, write-back, CPU RW | 52 MiB for one title; plus 0.82 MiB per extra title (8 more with the network installer); 1 MiB for Settings; plus 8 MiB with the system font | the frame being drawn and two prepared backdrops (7.9 MiB each), each title's tile at two sizes (384 and 256 px), the glyph cache, the system font library's memory, and temporaries (`<prefix>META`'s first MiB, a backdrop's QOI and its pixels; or a title's index, a PNG file, its decoded image, PngDec's work memory) |
| video | direct memory type 3 (write-combined), CPU+GPU RW | 16 MiB | the two scan-out buffers |

- **Placement:** both blocks go at the top of the largest free run of direct memory (`sceKernelAvailableDirectMemorySize`), away from the low hole and the top 32 MiB where the loader's own small blocks go (`mem.c`). Freeing them gives the run back in one piece, so no fragment is left between the run and anything allocated meanwhile.
- **Inside the heap:** a bump allocator with marks: temporaries are dropped as soon as they are used. The generated backdrop is drawn when needed rather than kept. The peak in the offline preview is 37 MiB for Ghost alone with its store art, 43 MiB for eight titles; on a PS5 with Ghost (`GHOSTMETA`) and a title with its own 1920x1080 `pic1.png`, 39 MiB of a 54 MiB heap.
- **Teardown, in order:**
  1. wait for pending flips;
  2. close VideoOut;
  3. close the font, renderer and library, and terminate the font memory;
  4. unload `libSceFont`, `libSceFontFt` and `libScePngDec` if the selector loaded them (a module SWRR had already loaded stays; `libSceUsbStorage`, loaded for Settings when the extension hadn't, stays with its session for the title); stop the network installer if it ran;
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

## Back to the game selector

With the release's two-file boot on a PS5 (`ghostldr.lua` with `ghostldr-ext.bin` cached), the game selector comes back when a game ends, without restarting SWRR or sending `ghostldr.lua` again:

- **L1 + R1 + Options + touchpad**, held together for about 0.3 s while the game runs. A notification says "Back to the game selector." Nothing of the game is saved that it didn't save itself.
- **The game quits** (`exit`, `_exit`, its program returns, or it asks the system to end it with `sceSystemServiceLoadExec`): ghostldr's page says "The game has quit." for 6 seconds (Cross skips it), then the selector.
- **The game crashes:** the crash page, with where it crashed, for 6 seconds (Cross skips it), then the selector. The log on the USB drive has the crash report as before.

A crash the system ends SWRR for (a GPU fault, the system out of memory) still ends SWRR: ghostldr gets no chance to run then ([failures.md](failures.md#when-ghostldr-cant-say-anything)). On a PS4, in Jak X, or when the bundle isn't cached (it was sent and couldn't be stored), the game ends as before: SWRR closes after a quit, and a crash's page stays.

How it works (`ghostldr.lua`, `boot.c title_back`, `ext/src/title_back.c`):

1. `ghostldr.lua` boots the game with ctx flag `CTX_FLAG_RETURN` (536870912). The core then doesn't return once the game's main thread runs. The boot thread (Luac0re's Lua thread) waits in the extension, which reads the controller every 0.1 s and watches for the failure page's end (`ghost_diag.back`, set by `failure.c` after a quit's or a crash's page).
2. **The game's threads stop.** Every thread it made, and ghostldr's own made for it (the monitor, the drive watch, the FTP server, the drawn dialogs, the memory planner, the watchdog), is on the core's list (`tls.c`). Each is sent SIGUSR2, whose handler sends it to `scePthreadExit` instead of back to what it was doing. For the first 1.5 s only threads stopped in the game's own code are taken, since one inside a system library may hold that library's lock, which the next game needs. After that, any thread that isn't writing ghostldr's log is taken, then any thread at all. The FTP server stops first, as it does before a game.
3. **What the game held in SWRR's process goes:** its compute queues (unmapped: the GPU reads their rings), its AvPlayers (closed on a thread of their own, waited for 2 s, since their threads call the game back), its audio ports, its VideoOut handle (the screen reads its buffers) and its save mounts (unmounted as at the game's own unmount).
4. **Executable memory:** each of the emulator's JIT buffers the game's modules, their data or the extension went into is mapped whole again at its home, from the compiler's descriptor of it, and a large executable's range (its views, `bigexec.c`) is unmapped. The next boot's arena finds the buffers as the first boot did.
5. **The core** unmounts ghostfs's containers, gives the extension back (its JIT block and the blob), then gives back every piece of direct memory allocated since the boot began. It marks what was allocated after SWRR's own memory went back (`dmem_mark`), compares page by page (`dmem_release_new`), unmaps this process's mappings of the rest and releases it. It returns `CTX_TITLE_BACK` (0xBAC).
6. `ghostldr.lua` reads the cached bundle again and opens the game selector, with the ELF loader beside it, as at the first boot.

The log says each step (`back: ...`): what ended the run, how many threads were listed and how many were left, each handle and buffer, and how many MiB of direct memory went back.

**Tested** on a PS5 at 13.60 (two-file boot, games on a USB drive), with L1 + R1 + Options + touchpad: PS4-Xplorer (47 threads stopped in 1.6 s, 2319 MiB back), then Bloodborne twice (53 threads, its compute queue unmapped, about 4290 MiB back each time). Each time the selector's largest free run was the same 3292 MiB as at the first boot, the JIT buffers were the same, and the next game booted to its title screen. Not yet on hardware: the quit and crash pages leading back (offline tests only), and a game that needed the reclaim.

**Limits:**

- A game whose code needed fresh JIT objects (the reclaim, [compatibility.md](compatibility.md)) leaves them holding JIT memory. The emulator's buffers were dropped for them, so the next game may not fit: send `ghostldr.lua` after restarting SWRR if a game then stops with "There's no room for the game's code."
- System libraries the game loaded stay loaded, and their state stays with them; the next game gets them as SWRR has them. Sockets the game opened in the JIT compiler stay open.
- A thread with SIGUSR2 blocked doesn't stop; the log names it.

## Offline preview and tests

The drawing code is plain C over a framebuffer, so it builds and runs on the computer:

```bash
make -C ghostldr/native
python3 -m unittest ghostldr.tests.test_selector      # or discover -s ghostldr/tests
SELECTOR_SHOTS=/tmp/shots python3 -m unittest ghostldr.tests.test_selector   # keep the screenshots
```

- **`ghostldr/tests/selector/preview.c`:** the UI code against a fake catalogue with generated placeholder art: games, one with a very long name, and two homebrew titles. It writes PNG screenshots of a tour (moving along the row, fallbacks, an incomplete title, a clock tick), the information panel, the launch fade, games on a USB drive (`--scene usb --usb`), a PS4 (`--scene ps4 --ps4`: the marked tiles, the question with No selected, No, Circle, a homebrew title and its panel, then Yes), a long name (`--scene long --start 8`), a community memory plan (`--scene community`: the panel's row, Square off and on, a game without one, the row gone once the switch is off), and a save tool (`--scene apollo`: the question, a user who isn't signed in, a long scan with its bar; `apollo-only`: the save tool alone, asked at once, a short scan without a bar; `apollo-cancel`: Circle held during the scan, a tap that is missed, the question again, then the rest), and prints what frames cost. Without `--ps4` the PS4 scene shows that the first Cross boots. `--meta TITLE=meta.bin` reads a `<prefix>META` file as the console does; `--art TITLE=DIR` takes a dump's `sce_sys` (`icon0.png`, `pic1.png`) or what `gfs_meta.py fetch` saved (`icon.png`, `backdrop.png`); `--user NAME` fills the top bar; `--users NAME*,NAME+,NAME` lists the console's users (`*` runs SWRR, `+` signed in too, the rest not signed in); `--scan TITLES:MS` simulates a save tool's scan; `--ps4` runs as on a PS4; `--start N` starts at title N; `--verify` compares every partial frame with a whole redraw; `--is-dump ID SFO_TITLE_ID CONTENT_ID` prints whether the selector takes those for a game's dump. The art is read at run time and never copied.

  ```bash
  python3 ghostldr/host/gfs_meta.py fetch CUSA11456 --out ../art/CUSA11456          # outside the repository
  python3 ghostldr/host/gfs_meta.py build --title CUSA11456 --fetch-art --art-dir ../art/CUSA11456 --out /tmp/ghost-meta.bin
  cc -O2 -Ighostldr/native/selector/src ghostldr/tests/selector/{preview,png_host}.c \
     ghostldr/native/selector/src/{ui,draw,font,sfo,meta}.c -lz -lm -o /tmp/preview
  /tmp/preview --font ghostldr/native/selector/font/notosans.glf --out /tmp/shots --scene tour \
     --meta CUSA11456=/tmp/ghost-meta.bin --user Player --verify
  ```
- **Settings in the preview:** `--scene settings` (every section with `--usb`'s two made-up drives, one exFAT and one FAT32; the log switch; Install over network with a made-up installer that installs a title, which joins the row; Close ghostldr: No), `empty` (nothing installed: the gear alone, then Settings), `one` (one title and the gear), `quit` (Close ghostldr: Yes), `refresh` (Triangle: its message, a new game in the row), `delete` (Settings › Games: No, then Yes), `artkeys` (Game art: the switch, a key typed on the keyboard card). `--build-meta OUT` writes a `meta.bin` with `meta.c`'s QOI encoder, which the test reads back with `gfs_meta.py`. What Settings shows comes from `sel_info_load`, which the preview fills with made-up values.
- **`ghostldr/tests/selector/host_env.c`:** runs the core's real `sel.c` and the real `selector.bin` in an x86-64 process (Rosetta on Apple silicon) against a fake console. Its `ps4` mode sets `CTX_FLAG_PS4` over three titles (homebrew by its ID, a game, and homebrew by its `CONTENT_ID`) and answers the question No, Circle, then Yes. Its `apollo` mode has Apollo (with its real profile) beside a game and two users, the second not signed in: the question, the mount and the scan through fakes of the core's `title_pick` and `save_prescan`, a cancel and the rest, and `title_user` at the end; `apollo-one` has the save tool alone, not requested, as `boot.c` shows it. Its save data is built by the test in ghostfs's formats. Its `one` mode shows one title with the Settings tile; `empty` has nothing installed (Settings opens, Close ghostldr returns `SEL_EXIT_LOADER`); `settings` runs a test build of the module with a fake network installer linked in (`tests/selector/netinst_fake`, built with `make SEL_NET_DIR=...`): it installs a title, the catalogue is read again, the new title is chosen, and the installer's memory goes back. `delete` deletes a game from Settings (a fake `sceSaveDataDelete`): its data container, index and META go, its saves and two containers named like its own that aren't (no tag past its count, another layout's tag) stay, and `GFSCATALOG` loses its entry. `usbrefresh` is a library (fake `ghost_api.library` and `library_scan`): Triangle reads it again, one game gone and one new, and the new one is chosen by its place. `test_usb_games.py` checks the extension's side (`approot_host --rescan-mv`) and that PS5 games are left out by the console and by `usb_drive.py` alike; `test_plansync.py` runs `artfetch.c` against a fake SteamGridDB, Twitch and IGDB. Every run must give back every block, close VideoOut and every file, unload what it loaded, unmount every container and leave the image cleared. `SELECTOR_NO_X86=1` skips these runs.

## Limits

- **A drive's file system** comes from `sceUsbStorageGetDeviceInfo`'s features word: 0x80 exFAT, 0x40 FAT32 (both measured, [usb.md](usb.md)); anything else shows as unknown.
- **Games per drive** come from the extension's list: a drive it didn't look at says so.

- **Cross is always "start".** It doesn't follow a Circle-to-confirm system setting.
- **Loading art is synchronous:** each icon costs a save-container mount (a few, for an install without `<prefix>META`), so the row can stutter while icons first load. Backdrops wait until the row stops.
- **Game art goes by name:** a game whose name the services don't know, or know under another, gets nothing (or another game's art: SteamGridDB's first search result is taken). Only PNG pictures are used.
- **`--fetch-art` needs the store to know the title ID:** it asks the US, UK, Japanese and Hong Kong stores. Titles delisted everywhere get no art; `--icon` and `--backdrop` still work.
- **The system font** (SST through libSceFont) crashed SWRR on a 13.60 PS5 during the font library's set-up; it is off unless `--select-font auto` is given.
- **No sound,** no Japanese button layout, no touchpad, no keyboard of its own (see [On a PS4](#on-a-ps4)).
- **On a PS4, which titles are marked** is a rule over IDs ([A game's dump or homebrew?](#a-games-dump-or-homebrew)), not a measure of what the title needs: a small game is marked too, and homebrew that needs more than 2.5 GiB isn't.
- **Background art** larger than 2560x1440 isn't decoded; a pic1 at 1920x1080, as PS4 titles ship it, is.
- At most 64 titles, the catalogue's own limit.
- **A save tool's scan** reads the pad only between two titles: Circle has to be held to cancel it. Its users are the console's 16 at most. The extension's code is placed before the title's modules for the scan, so a title whose image reaches past the emulator's JIT buffers could meet it; save tools are small homebrew. If the boot stops after the scan, that JIT block isn't given back (as after a USB drive's games).
