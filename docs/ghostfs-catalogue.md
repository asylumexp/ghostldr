# ghostfs: container names and the title catalogue

Several titles can be installed side by side in SWRR's save data. This page fixes the names ghostldr gives its save containers, the catalogue that lists the installed titles, and the rules every tool that reads or writes them follows: `gfs_install.py`, the Lua receiver, the core's ghostfs, and on-console tools such as a game selector or a web manager.

Everything here lives in SWRR's saved data for the signed-in user, in the SWRR region it was installed from (US and EU keep separate saved data).

## Limits that shape the names

- **Directory names** are `char[32]`: at most 31 characters. ghostldr only generates `A`–`Z` and `0`–`9`. (SWRR's own saves, `SLUS-20268` and `SLES-50366`, contain `-`, so no generated name can collide with them.)
- **Container size:** at most 32768 blocks of 32 KiB (1 GiB). A data container holds 992 MiB of the title, so a title has at most 96 data containers (`GFS_MAX_CONT`, about 93 GiB).
- **Mount points:** an app has 16, `/savedata0`–`/savedata15`. SWRR holds `/savedata0`. The core's ghostfs keeps at most 12 data containers mounted (least recently used goes first). `<prefix>SAVE` takes one more for the length of a save call. That leaves 2 spare, for the title's save directories later.

## Names

Each title has a **prefix**: 1–12 characters of `A`–`Z 0-9`, unique among the installed titles.

- The default is `G` and the five digits of a normal title ID (`CUSA12345` → `G12345`); other IDs get `G` and their first 11 letters and digits. A profile can set its own (Ghost of Tsushima: `GHOST`), and so can `gfs_pack.py --prefix`.
- A prefix must not start with `GFS` (ghostldr's shared names) and must not end with `D` and 3 digits or `M` and 2 digits (that would make its `<prefix>SAVE` look like another title's save directory).
- The prefix also names the title's progress, so it must not change once the title has been played.

Containers of one title, with `<P>` its prefix:

| Name | Holds | Created by |
|---|---|---|
| `<P>000` … `<P>095` | `data.bin`: 992 MiB of the title's files laid end to end; `gfs.tag`: the layout id, container number and size, written once the container is complete | `gfs_install.py` |
| `<P>INDEX` | `index.bin`: the title's index (format below) | `gfs_install.py` |
| `<P>SAVE` | The title's progress: save memory (`sdmemory_<slot>.bin`), later the catalogue of its save directories | the core, on the title's first save |
| `<P>D001SAVE` … `<P>D999SAVE` | Reserved: one container per save directory of the title | the core (planned) |
| `<P>M00SAVE` … `<P>M99SAVE` | Reserved: save-memory slots too big for `<P>SAVE` | the core (planned) |

Shared containers:

| Name | Holds |
|---|---|
| `GFSCATALOG` | `catalog.bin`: the list of installed titles (below). |
| `GFSINDEX` | **Legacy.** The single index of ghostldr v0.1.x installs (one title at a time). It stays valid as that title's index and is listed in the catalogue with the `LEGACY_INDEX` flag. New installs never write it; it is deleted only when its own title is removed or reinstalled. |
| `GHOSTIDX` | **Legacy**, the same for installs before 2026-10-21 (version 1 index, no title in its header). |
| `GFSFREE00` … `GFSFREE99` | Transient: the receiver's free-space probe creates and deletes them. Delete any found lying around. |

Never touched by any ghostldr tool: `SLUS-20268` (US SWRR's own save, with Luac0re) and `SLES-50366` (EU).

The longest generated name is `<P>D999SAVE` with a 12-character prefix: 20 characters.

Ghost of Tsushima installed with ghostldr v0.1.x is `GHOST000`…`GHOST055` + `GFSINDEX` + `GHOSTSAVE`. It keeps working as it is: the first run of the new installer adds it to the catalogue with index `GFSINDEX`, and the core boots it without the catalogue too.

## The catalogue: `GFSCATALOG/catalog.bin`

Little endian. A 64-byte header, then `count` entries of `entry_size` bytes, sorted by title ID.

Header:

| Offset | Type | Field |
|---|---|---|
| 0x00 | `char[4]` | magic `GFSC` |
| 0x04 | `u32` | version, 1 |
| 0x08 | `u32` | header_size, 64 |
| 0x0C | `u32` | entry_size, 192 |
| 0x10 | `u32` | count (at most 64) |
| 0x14 | `u32` | flags, 0 |
| 0x18 | `u64` | generation: every writer adds 1 |
| 0x20 | 32 bytes | reserved, zero |

Entry:

| Offset | Type | Field |
|---|---|---|
| 0x00 | `char[16]` | title ID, NUL padded (`CUSA11456`) |
| 0x10 | `char[16]` | prefix (`GHOST`) |
| 0x20 | `char[32]` | index container: `<P>INDEX`, or `GFSINDEX` / `GHOSTIDX` for a legacy install |
| 0x40 | `char[64]` | display name, UTF-8, NUL padded, may be empty |
| 0x80 | `char[16]` | version (`APP_VER`, e.g. `02.18`), may be empty |
| 0x90 | `u64` | layout id (the same as in the index header) |
| 0x98 | `u64` | total bytes of title data |
| 0xA0 | `u32` | data containers (`<P>000` … `<P>nnn`) |
| 0xA4 | `u32` | flags: bit 0 `INCOMPLETE` (being installed, interrupted, or being removed: never boot it), bit 1 `LEGACY_INDEX` (the index is `GFSINDEX`/`GHOSTIDX`) |
| 0xA8 | `u64` | last change, Unix seconds (the writer's clock; 0 if unknown) |
| 0xB0 | 16 bytes | reserved, zero |

The container is created with 512 blocks (16 MiB); `catalog.bin` is its only file. 64 titles take 12,352 bytes.

Readers take `header_size` and `entry_size` from the header (a later version may append fields to both) and ignore flag bits they don't know. Writers keep entries and bytes they don't understand.

### Rules

- **The index decides what boots.** A title is bootable only if its catalogue entry is not `INCOMPLETE` **and** its index header's `INCOMPLETE` flag is clear (the core checks both). The catalogue is the list; the index is the truth about one title.
- **One writer at a time.** Read the whole file, change it, write it back whole (receiver `WRITE`, or `PUT` when the container does not exist yet), and add 1 to `generation`. Luac0re runs one script at a time, so tools on the console don't overlap; a host tool and a console tool must not both be connected.
- **Installing** title `T` with prefix `<P>`, in this order:
  1. refuse if another catalogue entry already uses `<P>`, or if the console lacks the space (`FREE`);
  2. catalogue: add or update `T`'s entry, flags `INCOMPLETE`;
  3. `<P>INDEX`: a header-only index with its `INCOMPLETE` flag;
  4. if `T` was a legacy install (`GFSINDEX`/`GHOSTIDX` with prefix `<P>`) and its data is about to change, delete that legacy index;
  5. the data containers `<P>000` … (each gets `gfs.tag` when complete; a rerun keeps the tagged ones), then delete `<P>` data containers past the new count;
  6. the full `<P>INDEX`;
  7. catalogue: clear `INCOMPLETE`, set the layout id, size and count.
  Other titles' containers are never deleted to make room.
- **Removing** title `T`: mark its entry `INCOMPLETE`, delete its index container, then `<P>000` … `<P>095`, then drop the entry. `<P>SAVE`, `<P>D<nnn>SAVE` and `<P>M<nn>SAVE` are deleted only when the user asks for the progress to go too (`--remove-saves`).
- **No catalogue** (an install made by v0.1.x tools): the only title is the one in `GFSINDEX`, else `GHOSTIDX`. The installer writes the catalogue, with that title as a legacy entry, the first time it runs.
- **A lost or damaged catalogue** (bad magic, size not `header_size + count × entry_size`): treat it as missing. `gfs_install.py` rebuilds it from the index headers it can find (the legacy names and the prefixes of the titles it knows).

## The index: `<P>INDEX/index.bin`

Written by `gfs_pack.py` (the authoritative description is its docstring). Little endian: a 128-byte header `GFS1`, version 2, containers `u32`, entries `u32`, cap `u64` (992 MiB), total `u64`, names_off `u32`, names_size `u32`, prefix `char[16]` at 40, header_size `u32` at 56, flags `u32` at 60 (bit 0 `INCOMPLETE`), title `char[16]` at 64, layout id `u64` at 80; then the entries `{u64 offset, u64 size, u32 name, u32 flags (1 = directory)}` sorted by name bytes, then the NUL-terminated names. File offsets are positions in the title's linear byte space: container `offset / cap`, at `offset % cap` in its `data.bin`.

## The receiver's operations

`ghostldr/lua/gfs_receiver.lua`, started by `gfs_install.py` through Luac0re's loader, listens on TCP. Request, 96 bytes: `"GFSI"`, op `u32`, blocks `u64`, size `u64`, dirName `char[32]`, fileName `char[32]`, 8 reserved. Reply, 32 bytes: `"GFSR"`, status `s32`, value `u64`, millis `u64`, 8 pad.

| Op | Name | What it does |
|---|---|---|
| 1 | PUT | Delete and create `dirName` with `blocks`, write the `size` bytes that follow into `fileName`, commit, verify |
| 2 | QUERY | Size of `fileName` in `dirName` (-1 if missing) |
| 3 | BYE | End the receiver |
| 4 | DELETE | Delete `dirName` |
| 5 | READ | Up to `size` bytes of `fileName` from offset `blocks`; the reply is followed by `value` bytes |
| 6 | WRITE | The `size` bytes that follow (at most 4 MiB) become `fileName` in the existing `dirName`, committed |
| 7 | LIST | The catalogue: the reply is followed by `value` bytes in the format above. Without `GFSCATALOG` it is built from the `GFSINDEX`/`GHOSTIDX` header (one `LEGACY_INDEX` entry), or has no entries |
| 8 | FREE | Whether `size` bytes fit: creates 1 GiB probes `GFSFREE00`… until `size` is covered or the storage is full, then deletes them. `value` = the bytes that fit (at most `size`, rounded up to whole GiB) |
| 9 | DELSAVE | Delete a title's progress container: `dirName` must end in `SAVE` and must not be SWRR's own save |

PUT, DELETE and WRITE refuse names that end in `SAVE` and SWRR's own saves (status -3), so only DELSAVE, sent on explicit request, ever deletes progress.

## The core

- At boot ghostfs reads the catalogue and picks the title: the one `run.py boot --title T` asked for; else the only bootable title; with several it stops and logs the list (`ghostfs: 2 titles installed: ...: pick one with run.py boot --title`). Without a catalogue it boots the legacy `GFSINDEX`/`GHOSTIDX` title.
- Only the picked title's containers are mounted.
- `gfs_titles()` lists the catalogue and `gfs_mount_title(title)` loads one title's index (`ghostldr/native/src/ghostfs.c`, declared in `ghost.h`): an on-console selector chooses with these before the title's modules are loaded.
