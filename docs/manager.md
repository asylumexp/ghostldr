# The web manager: install games, and manage saves, from a browser

`ghostldr/lua/manager.lua` is a small web server that runs inside SWRR, started through Luac0re's loader like the installer. A browser on the same network can then:

- **install a game** into SWRR's saved data from its folder or a `.zip`, on a computer or a phone, with no Python ([Install a game](#install-a-game));
- see the installed titles and their save containers;
- **download a save** to the computer, as a `.tar` file;
- **restore a save** from such a file, into the title's existing save container or a new one;
- copy a save to another container of the same title;
- delete a save, or a title's installed game data;
- stop the manager, which hands Luac0re's loader back.

It needs nothing but Luac0re: no jailbreak, no payloads.

## Install a game

A game goes into SWRR's saved data (no USB drive) from a browser: the page reads the game's files on the computer or phone and the console stores them. Nothing else is needed: no Python, no app, no jailbreak.

### Start the installer

Either of these serves the same page:

- **From ghostldr's game selector:** Settings (□) → Network → **Install over network**. The TV shows the address and a token, and the install's progress. Circle stops it (hold it during an install). This needs ghostldr booted (two files with `nc`, or `ghostldr.lua` alone once its bundle is cached), but nothing installed: with no games the selector shows only Settings.
- **Without the selector, from Luac0re's loader:** send `manager.lua` like any script, from a computer or a payload sender app:

  ```bash
  nc 192.168.1.123 9026 < manager.lua
  ```

  The PS5 shows the address and the token in a notification. Open `http://<address>:8090/install` (the manager's page has an **Install a game** button too).

### Install

1. On a computer or phone on the same network, open the address the TV shows (`http://192.168.1.123:8090/`).
2. **Choose the game:**
   - **Choose folder…** (a computer): the game's `app0` folder, the one with `eboot.bin` and `sce_sys`. A folder above it works too; dragging the folder onto the page does the same.
   - **Choose .zip…** (a phone, or a computer): a `.zip` of that folder. Stored or normally compressed (deflate), up to any size (zip64). The page unpacks it in the browser as it sends; a zip made without compression is lighter on a phone.
   - Left out: files outside the game's folder, and macOS/Windows clutter (`.DS_Store`, `._*`, `__MACOSX`, `Thumbs.db`).
3. **Check** what the page read: the name, title ID and version from the game's `param.sfo`, the size, and the containers it will use. A game with no `param.sfo` (some homebrew) needs its title ID typed in.
4. Type the **token** the TV shows (once per browser tab; `#token=XXXX-XXXX` at the end of the address does it too), then **Install**.

The page shows the progress, the speed and the time left, and says when the game is installed. In the selector, the new game joins the row at once; with `manager.lua`, it is there at the next ghostldr boot.

- **Speed:** about 22–25 MB/s on a PS5, the rate it writes saved data at (a 50 GB game takes about 35 minutes); about 40 MB/s was measured on a PS4 Pro. Busy Wi-Fi on either side sets a slower pace.
- **Keep the page open** and the phone or computer awake until it says the game is installed.
- **A dropped connection** (Wi-Fi, a sleeping phone): the page carries on where the console got to, by itself. Closed the page? Choose the same game again and Install: what is on the console already is kept (each finished container is tagged), and the rest is sent.
- **Pause** stops after the current piece; Install again carries on.
- **Not enough space:** the page checks before sending anything, says how much is missing, and changes nothing. Free space on the console, or remove a game.
- **Already installed:** the same game (the same files) sends nothing. A different version replaces the old one's data; its progress (`<prefix>SAVE`) stays.
- **Remove:** the list of installed games has a **Remove** button per game: its data goes, its saves stay.
- **Several games** live side by side; installing one never deletes another ([ghostfs-catalogue.md](ghostfs-catalogue.md)).
- **On a PS4:** homebrew and small titles. The page warns before a game's dump: a PS4 gives SWRR about 2.5 GiB of memory ([ps4.md](ps4.md)).

### What it does, and the same from Python

The page's JavaScript is a port of `gfs_pack.py` and `gfs_install.py`: the same layout (boot files first, a title's profile rules for Ghost of Tsushima), the same `index.bin` and layout id, the same steps (catalogue entry and index marked incomplete first, containers tagged, leftovers swept, index and catalogue completed last). An install from the page and one from `gfs_install.py` recognise each other: either can finish or check the other's. The Python tools stay for developers ([ghost-on-stock-ps5.md](ghost-on-stock-ps5.md)).

The console side only stores bytes, through the [install requests](#the-install-requests) below. A data container comes in one or more requests; the console keeps it open between them, so a dropped connection loses nothing that arrived. On the console the bytes are received and written at the same time: by the native pump under `manager.lua`, by a second thread in the selector's server (`ghostldr/native/netinst/`).

## Start it

1. Start Luac0re's Remote Lua Loader in SWRR (port 9026), as for the install ([ghost-on-stock-ps5.md](ghost-on-stock-ps5.md), steps 1 and 2).
2. On the computer:

   ```bash
   python3 ghostldr/host/manager.py
   ```

   It prints the address and the session token:

   ```
     ghostldr manager is running
     open   http://192.168.1.123:8090/
     token  K7QF-3MXP   (also shown on the PS5)
   ```

   The PS5 shows both in a notification too.
3. Open the address in a browser on any device on the same network.

- **Stopping:** the page's **Exit** button, Ctrl-C in `manager.py`, or `python3 ghostldr/host/manager.py --exit` from any computer.
- **Leaving it running:** `manager.py --detach` returns once the manager is up.
- **The loader is busy while the manager runs:** stop the manager before `gfs_install.py`, `run.py` or any other script. It never stops by itself; if the browser goes away, open the page again or use `--exit`.
- **Port:** 8090 by default (`--port`). If the port is taken, the manager uses the next free one, up to 8099, and says which.
- **Address:** `manager.py` finds the PS5 like the other tools (`PS5_HOST`, `PS5_HOST_ID`, or the only PS5 that answers).
- **Firewall:** the computer must let Python receive on UDP 18196 for `manager.py` to print the address and token. Without it, read them from the PS5's notification.

## The token

Listing and downloading need nothing. Everything that changes the console needs the session token: restore, copy, both kinds of delete, and the size check before a restore. A device on the network that hasn't seen the PS5's screen or `manager.py`'s output can look, but can't change or delete anything.

- **Unlocking:** enter the token once in the page's token field (case, spaces and the dash don't matter). The page keeps it for the browser tab.
- **A new token each start:** every start of the manager makes a new random token. A page left open from an earlier run asks for the new one.
- **Lost the token?** **Show token on PS5** puts it on the TV again.
- **Wrong tokens:** after 5 wrong tokens, each further try waits longer (up to 5 minutes). After 30, writes stay locked until the manager is restarted.

The token is only sent in a request header (`X-Ghostldr-Token`), never in a URL. The page refuses to be framed by other sites.

## What the page shows

- **Installed titles.** With several titles installed side by side, the list comes from the catalogue (`GFSCATALOG`, see [ghostfs-catalogue.md](ghostfs-catalogue.md)). A v0.1 install (`GHOST000`–`GHOST055` + `GFSINDEX`) has no catalogue; its title comes from `GFSINDEX`.
  - Each title shows its ID, name, version, size, its data containers present out of expected, and its state:
    - `complete`;
    - `install interrupted`;
    - `partial` (data containers missing);
    - `leftover data without an index`.
  - The name comes from the catalogue, else from the title's `param.sfo`, else from `ghostldr/data/titles/*.json` on the computer that started the manager.
- **Saves of each title**, with how much each container uses of its size and when it was last changed:
  - `<prefix>SAVE`: the title's progress (Ghost: `GHOSTSAVE`);
  - `<prefix>D001SAVE`…: save folders;
  - `<prefix>M01SAVE`…: save memory slots.
- **Saves of titles that aren't installed:** a `<prefix>SAVE` stays when its game data is deleted. It can be downloaded, restored and deleted like any other save.
- **SWRR's own save** (`SLUS-20268` on US SWRR, `SLES-50366` on EU): read-only, download only.
- **Other containers**, read-only. A left-over free-space probe of the installer (`GFSFREE00`…) can be deleted.

Free space on the console isn't shown: SWRR's save data API doesn't report it. A restore that doesn't fit stops with "no space left on the console's storage" and changes nothing.

## Actions

### Download

**Download** streams the save container as a `.tar` file named `<CONTAINER>-<date>-<time>.tar`.

- **What's in it:** every file of the container, including the system's `sce_sys` files (`param.sfo`, icons) where SWRR can read them. The archive's first entry, `ghostldr-save.json`, describes the container (below).
- **Reading only:** nothing on the console changes.
- **SWRR's own save:** it is read in place, where SWRR has it mounted; the manager never mounts it.

### Restore

**Restore** puts a save from a `.tar` file into a container.

1. **The page reads the archive first** and shows:
   - the files it will write;
   - what it skips;
   - the size check: whether the files fit in the container, or the size of a new one.
2. **Confirm** by typing the container's name.
3. **Upload:** the manager stores the whole upload in SWRR's temporary folder (`/temp0`) and checks it before touching the save. If the upload breaks off, or the archive is bad, the save is unchanged.
4. **Write:**
   - An existing container gets its files replaced: every file except `sce_sys` is removed, then the archive's files are written.
   - A missing container is created: the original's size from the manifest, at least what the files need.
   - The save is committed, then each file's size is checked.
5. **Skipped entries:**
   - `sce_sys/...`: the system writes these, and apps can't;
   - macOS `._*` and `.DS_Store`.
6. **Refused archives:** paths with `..`, absolute paths, links, device entries, and names other than letters, digits, `.`, `_` and `-`.

- **Archives from elsewhere:** any tar (ustar, pax or GNU, uncompressed) of a save folder works. When everything sits in one folder next to a `ghostldr-save.json`, as in the manager's own downloads, that folder is stripped.
- **Restore into new container:** for a title without its `<prefix>SAVE` (or with one already), the page suggests the next free name: `<prefix>SAVE`, else `<prefix>D001SAVE`…

### Copy

**Copy** copies one save container's files to another container of the same title. An existing target has its files replaced; a missing one is created at the source's size. Confirm by typing the target's name.

### Delete a save

**Delete** removes one save container and everything in it. Confirm by typing its name. Download it first if you may want it back.

### Delete game data

**Delete game data** removes a title's installed data: its index and its data containers. Confirm by typing the title ID.

- **Saves:** the title's saves stay unless **Also delete its save containers** is ticked.
- **Order, as the installer uses it:**
  1. the catalogue entry is marked incomplete;
  2. the index is deleted;
  3. the data containers are deleted;
  4. the entry is dropped from the catalogue.
- **Interrupted:** the title is never left bootable, and `gfs_install.py` reinstalls it from scratch.
- **v0.1 installs:** the same without the catalogue steps. The manager never creates `GFSCATALOG`.

### Exit

**Exit** stops the server, closes its port and returns to Luac0re's loader, which takes the next script.

## What is protected

- **SWRR's own save** (`SLUS-20268`, `SLES-50366`): the PS2 memory cards and Luac0re. The manager reads it from `/savedata0` and never mounts, writes or deletes it. Every write path refuses its name before any save call.
- **Restore, copy and delete** accept only save container names:
  - `<prefix>SAVE`;
  - `<prefix>D<nnn>SAVE`;
  - `<prefix>M<nn>SAVE`;
  - a prefix is 1–12 of `A`–`Z 0-9`.

  They refuse the catalogue, indexes and data containers.
- **Game data** is deleted only through **Delete game data**, for a title the manager found. The list comes from the catalogue and the index headers; the data containers match `<prefix>000`…
- **A title's progress** goes only when you delete that save, or tick the box when deleting game data.

## The archive

A tar file (POSIX ustar, uncompressed):

```
ghostldr-save.json
GHOSTSAVE/
GHOSTSAVE/sdmemory_0.bin
GHOSTSAVE/sdmemory_0.param
GHOSTSAVE/sce_sys/
GHOSTSAVE/sce_sys/param.sfo
...
```

`ghostldr-save.json`:

```json
{"format": "ghostldr-save", "version": 1, "container": "GHOSTSAVE", "kind": "progress", "prefix": "GHOST",
 "blocks": 256, "free_blocks": 190, "mtime": 1792600000, "exported": 1792600600, "swrr": "CUSA03474",
 "files": [{"path": "sdmemory_0.bin", "size": 555008}, ...], "unreadable": []}
```

- **`blocks`:** the container's size in 32 KiB blocks. A restore into a new container uses it.
- **`unreadable`:** files SWRR couldn't open, which aren't in the archive.
- **Extracting:** any tar tool extracts it: `tar -xf GHOSTSAVE-20261022-083546.tar`.

## Good to know

- **One request at a time:** the manager serves requests one after another, so a long download holds up the page until it is done. Most saves are a few MB and take seconds.
- **Memory:** Luac0re runs Lua with its garbage collector off, so the manager's memory grows a little with each request (the page footer shows it). Restart it after a very long session.
- **Restore size:** at most 1 GiB per container, and the upload must fit in SWRR's temporary folder.
- **Saves of other titles:** they belong to the SWRR region and PSN user they were made with, like everything in SWRR's saved data.

## The JSON API

The page uses these; scripts can too. POST requests that change something need the `X-Ghostldr-Token` header, and the ones marked *confirm* need `confirm=<name>` as well.

| Request | What it does |
|---|---|
| `GET /api/state` | Titles, saves, SWRR's save, other containers, catalogue |
| `GET /api/files?c=NAME` | The files in a container |
| `GET /api/download?c=NAME` | The container as a tar archive |
| `POST /api/auth` | Checks the token |
| `POST /api/check-restore?c=NAME&need=BLOCKS` | Whether `BLOCKS` of files fit in `NAME`, or how big a new one would be |
| `POST /api/restore?c=NAME&confirm=NAME[&strip=FOLDER][&blocks=N]` | Body: the tar archive (`Content-Length` required) |
| `POST /api/copy?from=A&to=B&confirm=B` | Copies the files of save `A` to `B` (same title) |
| `POST /api/delete?c=NAME&confirm=NAME` | Deletes a save container (or a left-over `GFSFREEnn`) |
| `POST /api/delete-title?prefix=P&confirm=TITLE_ID[&saves=1]` | Deletes a title's index and data containers (and its saves with `saves=1`) |
| `POST /api/notify` | Shows the token on the PS5 again |
| `POST /api/exit` | Stops the manager (no token needed) |

### The install requests

The install page uses these; both `manager.lua` and the selector's server (`ghostldr/native/netinst/netinst.c`) answer them. POSTs need the token. Container names are checked: `read` and `probe` take anything but SWRR's own save and a title's progress (`*SAVE`); the writes only the kinds named.

| Request | What it does |
|---|---|
| `GET /install` (the selector's server: also `/`) | The page |
| `GET /api/install/info` | Which server, SWRR's title ID, `ps5`/`ps4`, and the data container being written (`session`: `c`, `size`, `written`, `lid`) |
| `GET /api/install/read?c=NAME&f=FILE&off=N&len=N` | Up to 4 MiB of a file; `X-Gfs-Status` is 0, the mount's error, or 4294967295 (no such file) |
| `POST /api/install/auth` | Checks the token |
| `POST /api/install/probe?c=A,B,...` | Up to 16 containers: whether each exists, and its `gfs.tag` (hex) |
| `POST /api/install/free?bytes=N` | Whether N bytes fit (1 GiB probes `GFSFREE00`…, deleted again): `have` |
| `POST /api/install/put?c=NAME&f=FILE&blocks=N` | The body (at most 4 MiB) becomes FILE in a new container of N blocks (deleted first), committed and checked: an index, `GFSCATALOG`, `<prefix>META` |
| `POST /api/install/write?c=NAME&f=FILE` | The body replaces FILE in an existing container: the catalogue, a `gfs.tag` |
| `POST /api/install/delete?c=NAME` | Deletes an index, a data container, `<prefix>META` or a `GFSFREEnn` |
| `POST /api/install/data?c=NAME&blocks=N&size=S&off=O&lid=L` | Bytes O… of a data container's `data.bin`. O = 0 creates the container; it stays open between requests and is committed and checked when S bytes are there (`done`). Any other O must be what the console has (`written`), else 409 with it |
| `POST /api/install/begin?title=&name=&total=`, `/end?title=&ok=1` | What is being installed, for the TV (the selector counts finished installs and refreshes its row) |

Each reply is `{"ok": true, "rc": 0}` or the save call's error in `rc` (as an unsigned number) and `error`.

For example, a backup from a script:

```bash
curl -o GHOSTSAVE.tar "http://192.168.1.123:8090/api/download?c=GHOSTSAVE"
```

## For developers

- **The install page's tests:** `test_netinstall.py` runs the page's JavaScript in node (no browser) and holds it to `gfs_pack.py` and `gfs_install.py` byte for byte, reads zips made by Python, installs through `manager.lua` (`manager_fake.lua`'s live mode) and through the selector's server built for the computer (`tests/ext_native/netinst_host.c`), and drops connections in the middle of a container. `ghostldr/tools/netinstall_page.py` keeps the page's title profiles, its copy in `manager.lua` (with the pump) and `ghostldr/data/netinstall.html.gz` (what the selector serves) current; the tests fail when they aren't.
- **Offline tests:** `python3 -m unittest discover -s ghostldr/tests` runs `manager.lua` end to end against a fake console (`ghostldr/tests/manager_fake.lua`: Luac0re's memory and call API, libkernel's file calls, SWRR's save service and the browser's connections). It needs a desktop Lua 5.3 or 5.4 (`brew install lua`); without one those tests are skipped.
- **Test containers:** `manager.py --scratch GLDRT` treats containers named `GLDRT` followed by letters and digits as saves of a test "title". This lets you exercise restore, copy and delete on a console without touching any real save. The prefix can't be one a title, index or SWRR uses.
- **Not there yet:**
  - **booting a title from the page:** the core (about 240 KiB) is sent by `run.py`, and the loader runs one script at a time. The page could stop the manager and ask `manager.py` to run `run.py boot --title`.
  - **keeping the manager available while a title runs:** this needs the server in the core, on its own thread.
  - **a free-space figure:** SWRR's save API has none. The installer's probe (create 1 GiB containers until the storage is full) is too heavy to run for a page refresh.
