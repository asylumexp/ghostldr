# The web manager: saves and installed titles from a browser

`ghostldr/lua/manager.lua` is a small web server that runs inside SWRR, started through Luac0re's loader like the installer. A browser on the same network can then:

- see the installed titles and their save containers;
- **download a save** to the computer, as a `.tar` file;
- **restore a save** from such a file, into the title's existing save container or a new one;
- copy a save to another container of the same title;
- delete a save, or a title's installed game data;
- stop the manager, which hands Luac0re's loader back.

It needs nothing but Luac0re: no jailbreak, no payloads.

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

For example, a backup from a script:

```bash
curl -o GHOSTSAVE.tar "http://192.168.1.123:8090/api/download?c=GHOSTSAVE"
```

## For developers

- **Offline tests:** `python3 -m unittest discover -s ghostldr/tests` runs `manager.lua` end to end against a fake console (`ghostldr/tests/manager_fake.lua`: Luac0re's memory and call API, libkernel's file calls, SWRR's save service and the browser's connections). It needs a desktop Lua 5.3 or 5.4 (`brew install lua`); without one those tests are skipped.
- **Test containers:** `manager.py --scratch GLDRT` treats containers named `GLDRT` followed by letters and digits as saves of a test "title". This lets you exercise restore, copy and delete on a console without touching any real save. The prefix can't be one a title, index or SWRR uses.
- **Not there yet:**
  - **booting a title from the page:** the core (about 240 KiB) is sent by `run.py`, and the loader runs one script at a time. The page could stop the manager and ask `manager.py` to run `run.py boot --title`.
  - **keeping the manager available while a title runs:** this needs the server in the core, on its own thread.
  - **a free-space figure:** SWRR's save API has none. The installer's probe (create 1 GiB containers until the storage is full) is too heavy to run for a page refresh.
