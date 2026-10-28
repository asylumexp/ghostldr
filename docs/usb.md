# Games on a USB drive

ghostldr can boot games from a USB drive plugged into the PS5 instead of installing them into SWRR's save containers ([ghostfs-catalogue.md](ghostfs-catalogue.md)). It needs no jailbreak: the PS5's own USB storage service mounts the drive into SWRR's sandbox, and the game's files are read from there as plain files.

**This is the recommended way to run games.** There's no install step and no packing: a game is its dump's folder copied onto the drive, in a folder of any name. Games load faster than installed ones, and SWRR's saved data only holds their progress. Install into the saved data instead when you have no spare drive, or in Jak X, where drives are untested.

Tested on a stock PS5 at firmware 13.60 with SWRR (US), with no jailbreak: Ghost of Tsushima and Bloodborne boot from a drive into gameplay, started directly and through the game selector, and PS4-Xplorer reaches its menus. Both games load faster from the drive than from an install ([Measured](#booting-titles-from-the-drive)). Missing or empty folders stop the boot cleanly.

## In short

```bash
python3 ghostldr.py copy /path/to/CUSA11456/app0      # onto the drive mounted on this computer
python3 ghostldr.py check                             # what the PS5 will find on it
```

Plug the drive into the PS5, start Luac0re's loader in SWRR (HALL OF FAME), then:

```bash
python3 ghostldr.py boot                              # the only game, or the game selector on the TV
python3 ghostldr.py boot CUSA11456                    # one game (or part of its name: boot ghost)
```

## What goes on the drive

**The drive.** exFAT, the format the PS5 itself offers for USB storage. FAT32 works too, but holds no file of 4 GiB or more, and many PS4 games have one. The PS5 must use it as USB storage (it lists it in Settings, Storage), not as PS5 extended storage. Any size and speed the PS5 takes; a USB 3 NVMe enclosure read at 510–540 MB/s. Your other files on the drive stay as they are.

**A game** is its dump's folder, as dumped: the folder with `eboot.bin` in it (often called `app0`, or `CUSA12345-app`).

| | |
|---|---|
| **Needed** | `eboot.bin`, `sce_module/` (the game's own libraries, when it has them) and every data file and folder the game has: copy the whole folder as it is. |
| **Recommended** | `sce_sys/param.sfo`: it names the game (title ID, name, version). Without it, name the folder after the title ID (below). `sce_sys/icon0.png` and `pic1.png` give the selector its tile and background. |
| **Optional**, beside the game's folder | `prefix`: one line, the save container its progress goes to (below). `meta.bin`: its name and art for the selector, as `ghostldr.py copy` writes it (with `--fetch-art`, the PlayStation Store's). |
| **Not used** | `sce_sys/about`, `keystone`, `sce_discmap.plt` and other system files are harmless and can stay. |

- **Updates:** use a dump with the update in it, one folder with the patch's files over the base game's (a dump of the installed, updated game is that). ghostldr runs what is in the folder: a separate update or patch folder isn't merged in. Ghost of Tsushima is tested at v2.18 (CUSA11456), Bloodborne at 1.09.
- **DLC** isn't loaded.
- **No packing or converting:** files stay whole, as on the computer.

**Where.** Anywhere on the drive, in folders of any name, up to four folders down from the drive's top. Each of these is found:

```
<drive>/
    Ghost of Tsushima (CUSA11456)/      a dump copied by hand: eboot.bin, sce_module/, sce_sys/, ...
    My Games/
        bloodborne/
            app0/                       an app0 folder: eboot.bin, ...
            prefix                      optional: its save container's prefix
            meta.bin                    optional: its name and art for the game selector
    ghostldr/
        CUSA12345/app0/                 where ghostldr.py copy puts games (older releases look only here)
```

## How the PS5 finds games

`ghostldr.py check` shows what it will find on a drive, and why not. The console and the computer follow the same rules (`ext/src/approot.c`, `ghostldr/host/usb_drive.py`):

1. **A game is a folder with `eboot.bin`**, or a folder with `app0/eboot.bin` (that folder then also holds the game's `prefix` and `meta.bin`). Folders' names don't matter, and names compare without regard to case, as on a PC.
2. **Its title ID** is `sce_sys/param.sfo`'s `TITLE_ID`, else the one in its `meta.bin`, else a title ID in its folder's name: four letters and five digits anywhere in it (`CUSA11456`, `Ghost of Tsushima (CUSA11456)`, `cusa11456-app`), or, for `<name>/app0`, the whole name when it is one (the old `ghostldr/<TITLE>/app0` layout, `HOMEBREW_1`).
3. **Where it looks:** from the drive's top (or the folder a boot names, `--folder`), down to 4 folders deep, 256 folders and 64 games in all, and the first 4,096 entries of each folder, so a large drive stays quick (the console's exFAT is slow to list big folders). It doesn't look inside a game's folder, or in folders whose names start with `.` (macOS's `.Trashes`, `.Spotlight-V100`) or Windows' `System Volume Information` and `$RECYCLE.BIN`.
4. **Two copies of one game:** `boot CUSA11456` starts the first by folder (alphabetical). The game selector shows both, each with its folder, so either can be started. Both keep their progress in the same save.
5. **Folders it can't start** are listed in the log, by `check`, and in the selector (Cross says why): a folder with `eboot.bin` that nothing names (no `param.sfo`, no ID in the folder's name), an `app0` without `eboot.bin`, or a game's `sce_sys` without `eboot.bin` (a copy cut short).

## Copying games onto the drive

**With the drive plugged into the computer** (the quickest way):

```bash
python3 ghostldr.py copy /path/to/CUSA12345/app0                       # the only exFAT or FAT drive mounted here
python3 ghostldr.py copy /path/to/CUSA12345/app0 /Volumes/MYDRIVE --folder "Games/My Game"
```

- **Where:** `--folder` (any name), else the game's copy already on the drive (it's updated), else `ghostldr/<TITLE>`.
- **It checks first:** the drive's format (FAT32 holds no file of 4 GiB or more) and its free space, and stops before copying anything if either rules the game out.
- **Progress:** a progress bar with the speed and time left.
- **Then it checks the copy:** every file's size, and `eboot.bin`, `sce_module/` and `param.sfo` byte for byte as the drive holds them (`--verify full`: every byte). A file that differs is copied again; one that differs twice stops it with the file named.
- **Stopped?** Run the same command again: files already there with the same size are kept, changed ones are copied again, and files the dump no longer has are removed.
- It writes `prefix` and `meta.bin` beside the game's `app0`. Each file is written under a `.part` name and renamed when complete, without macOS's `._` companion files.

**With the drive in the PS5,** while a game runs through ghostldr (its FTP server, [ftp.md](ftp.md)):

```bash
python3 ghostldr.py copy /path/to/CUSA12345/app0 ps5
python3 ghostldr.py check ps5
```

`ps5` is `ftp://<the PS5>:1212/ghostldr_usb`; an FTP address works too, also a jailbroken PS5's ftpsrv, `ftp://PS5:2121/mnt/usb0` (`/mnt/usb0` is the console's first USB drive, `/mnt/usb1` the second). Through ghostldr's server, PS4-Xplorer's 79 files went on in 4 s, and large files at 25 MiB/s over one connection or 52 MiB/s over eight.

- Most of a dump is small files, and each costs a few round trips, so it sends several at once, each over its own connection: `--jobs N` (default 8). A server that takes fewer connections (it answers 421) gets as many as it takes.
- ftpsrv shows SELF files (`eboot.bin`, `*.prx`) decrypted unless told not to, with other sizes and contents than what was stored. The tools switch that off (ftpsrv's `SELF` command) when the server offers it.
- Over FTP the drive's format and free space aren't known, so neither is checked. Files are written in place, because a rename costs the console's exFAT about a quarter of a small file's time; a file cut short is shorter, so the next run copies it again.
- **How fast:** through ftpsrv 0.21 on a 13.60 console to a USB 3 NVMe drive (exFAT), Bloodborne (28,752 files, 29.3 GiB) took 13 minutes, about 38 MiB/s, and Ghost of Tsushima (40,105 files, 62.7 GiB) a little over an hour. Small files in small folders go at about 120 a second over 8 connections and large ones at 35–40 MiB/s. Ghost is slow because one of its folders holds 27,674 files: the console's exFAT gets slower to add files to a folder as it fills, down to about 10 a second past 20,000 entries, however many connections there are. Copying from a computer with the drive plugged in avoids that.

**By hand** (Finder, Explorer, `cp`): copy the dump's folder anywhere on the drive. Nothing is packed or converted. If the dump has no `sce_sys/param.sfo` (Ghost's often hasn't), put the title ID in the folder's name, `Ghost of Tsushima (CUSA11456)` for one. Then `ghostldr.py check` says whether the PS5 will find it.

`gfs_pack.py --usb DRIVE` (what the desktop app runs) does what `copy` does, with periodic progress lines instead of a bar, and takes the same `--folder` and `--verify`. Name and art for the selector: `--name`, `--icon`, `--backdrop`, `--fetch-art` ([compatibility.md](compatibility.md#1-pack)).

**Removing a game:** delete its folder on the computer, or `python3 ghostldr/host/usb_drive.py remove DRIVE CUSA12345`, which removes only folders laid out by the copy (`app0`, `prefix`, `meta.bin` and nothing else) and leaves copies made by hand to you. Its progress stays in SWRR's saved data.

## Checking a drive

```
$ python3 ghostldr.py check /Volumes/MYDRIVE
/Volumes/MYDRIVE (exFAT, 412.3 GiB free)

Games the PS5 will find (3):
  CUSA00900  Bloodborne                          01.09  My Games/bloodborne  29.3 GiB  saves: G00900SAVE
  CUSA11456  Ghost of Tsushima                   02.18  Ghost of Tsushima (CUSA11456)  62.7 GiB  saves: GHOSTSAVE
  CUSA11456  Ghost of Tsushima                   02.18  ghostldr/CUSA11456  62.7 GiB  saves: GHOSTSAVE
             ^ a second copy: --title boots the one in Ghost of Tsushima (CUSA11456); the selector shows both

Folders the PS5 can't start (1):
  old/dump: no sce_sys/param.sfo: name the folder after the game's ID (CUSA12345)

Start one with:  python3 ghostldr.py boot   (the PS5 shows a game selector)
```

It reads a drive mounted on this computer (every exFAT or FAT one when none is named), or the one in the PS5 (`ps5`, or an FTP address) while a game runs. On a mounted drive it also adds up each game's files, checks them against FAT32's 4 GiB limit, and looks deeper than the PS5 does, to name games too far down for it. `usb_drive.py list [DRIVE]` prints one line per game.

## Booting

Plug the drive into the PS5 before booting, then start Luac0re's loader:

```bash
python3 ghostldr.py boot                    # the only game, else the game selector
python3 ghostldr.py boot CUSA12345          # that game (part of its name works: boot bloodborne)
python3 ghostldr.py boot --select           # the selector even for one game
python3 ghostldr.py boot --folder "My Games"   # only the games under that folder
python3 ghostldr.py boot --usb              # only the drive's games, not the installed ones
```

- **The drive first, then installed games.** Without `--usb` or `--installed`, a boot looks on the drive first; with no game there (or not the one asked for), it boots the games installed in SWRR's saved data instead. On a PS4 and in Jak X it goes straight to the installed games. On a 13.60 PS5 (EU SWRR) with no drive plugged in, a boot found none, gave the extension's early JIT block back and went on with the installed titles, the game selector included, all in under a second; `--usb` stopped cleanly with the reason, and the next boot followed in the same SWRR session.
- **The selector** lists the drive's games with their names and art, each with its folder in the info panel (Triangle); second copies show which they are, and folders that can't start show why ([selector.md](selector.md)).
- **When this computer has the drive mounted too,** the boot uses its games' dumps for the preflight check and their profiles; the console doesn't need it.

`run.py` takes the same as `--app-root`: `auto` (the default), `usb:/` (the whole drive), `usb:/FOLDER` (the games under a folder, or one game's folder), or `ghostfs`. A dev setup's folder of games on the console works the same way without `usb:`, for example `--app-root /data/games`.

A boot that can't find the drive or the game stops before it touches SWRR's state, gives the drive back, and says why in the log. The next boot can follow at once, in the same SWRR session.

## Progress

A game from a USB drive keeps its progress in SWRR's save containers, in `<prefix>SAVE`, as an installed title does ([ghostfs-catalogue.md](ghostfs-catalogue.md#names)). The prefix is the game's `prefix` file, else its profile's (`GHOST` for Ghost of Tsushima, so a copy made by hand still uses `GHOSTSAVE`), else `G` and its title ID's letters and digits (`GCUSA12345SAVE`); a game played before 2026-10-28, when the default was `G` and only the digits, keeps `G12345SAVE` when its progress is there and none is under the new name. The same game installed and on a drive, with the same prefix, shares its progress. Nothing is written to the drive.

## How the console reaches the drive

SWRR's sandbox has no `/mnt` and sees no USB drive by itself. The PS5's USB storage service, ShellCore's AutoMounter, can mount a drive's folder into an app's sandbox, and libSceUsbStorage (a PS4 library SWRR can load) is its client:

- **The public call is refused.** `sceUsbStorageRequestMap` answers 0x80F40014 for any folder: ShellCore's `sceProcessStarter::CheckUsbAccessDir` checks the folder against a list of USB folders in the app's `param.sfo`, and SWRR's has none.
- **The library's debug call isn't.** libSceUsbStorage also has an unexported `_sceUsbStorageRequestMap2(device, folder, mount point)`. It sends AutoMounter debug data that it logs as `skipParamSfoCheck`, and AutoMounter mounts the folder (a nullfs mount, read-write) at the mount point in the caller's sandbox. A stock 13.60 PS5 honours it.

`ext/src/approot.c` does this before the title's modules load:

1. **Find the call.** It sits 0xA0 bytes before `sceUsbStorageRequestMap` (0x90 in later PS4 builds of the library). The library's code is execute-only, so it can't be read: the module's unwind table (`.eh_frame_hdr`, in a readable segment) must list a function that starts exactly there and ends before `RequestMap`. Otherwise the boot stops and says so; nothing is called on a guess.
2. **Open the session once.** `sceUsbStorageGetDeviceList` comes first: until `sceUsbStorageInit` it answers 0x80F40009, and only then does ghostldr call Init. A second Init answers 0x80F40002 and leaves the library uninitialized, with every mount gone.
3. **Mount the drive.** For each drive in the list (device IDs change with every boot and plug), it mounts the drive's root at `/ghostldr_usb` and keeps the first whose root has the boot's top folder (`My Games` for `usb:/My Games`), or, for the whole drive, the first with a game. The others are unmounted again.
4. **Find the games** as [above](#how-the-ps5-finds-games): one directory listing per folder, breadth first, stopping at a game's folder.
5. **Serve `/app0`.** The game's folder under `/ghostldr_usb` becomes the app root, and the kernel's own file calls read and write it, as on a dev setup. The mount is read-write, and so is the title's `/app0` on it: a title that writes there (homebrew file managers do) writes to the drive.
6. **Keep it.** The session stays open for the whole run: `sceUsbStorageTerm`, or the session ending, would unmount the title's files. If the title calls `sceUsbStorageInit` or `Term` itself, `hle_usbstorage.c` answers them without reaching the library.

If the boot stops before the title loads (no drive, no folder, no title), the drive is unmounted and the extension's code gives back its JIT block, so the next boot finds both free.

### The boot

1. `ghost_boot` receives the extension (`ext_fetch`), as always.
2. For an app root that is `auto`, `usb:...`, or a directory without `eboot.bin`, the core places the extension's code at once: a new JIT block, before SWRR's emulator memory is released. It then calls the extension's early entry, `ext_early` → `approot_init`.
   1. `usb:` (and `auto`, as `usb:/`): the drive is mounted as above, and the path is read under `/ghostldr_usb`.
   2. The games under it are found (`lib_scan`). One game, or the one `--title` asks for: it boots. Several: `app_root` becomes the folder, ctx flag `CTX_FLAG_LIBRARY` is set, the game selector lists `ghost_api.library()` (every game found, the ones that can't start too) and hands its choice back by index: `approot_pick(title, index)`.
   3. The game's folder becomes the app root, which `remap_path` maps `/app0` to. `profile_select(title)` and `gfs_use_saves(title, prefix)` follow.
   4. On failure, `ext_abandon` frees the code's JIT block and the blob, and the boot returns -6. With `auto`, `approot_init` returns 2 instead: the core frees the code's JIT block but keeps the blob (`ext_unplace`), and boots the installed titles (`ghostfs`), where the code is placed again as usual.
3. The title's modules load and the boot carries on as for any other title. `ext_start` runs `ext_init` at its usual place; it only skips placing the code again.

A title's own directory (`--app-root /data/.../app0`) and `ghostfs` never reach the extension's early entry. They boot exactly as before.

### Measured (stock 13.60, a USB 3 NVMe enclosure, exFAT)

- Mounting the drive: about 0.4 ms. Mounting, opening and reading a file, and unmounting, 100 times in a row in one session: all fine, 5.5 ms each.
- Sequential reads through the mount (`sceKernelRead`, 4 MiB at a time): 510–540 MB/s. A 129 MiB file read cold after a restart: 510 MB/s.
- Opening, reading 4 KiB of and closing 192 small files, from Lua's slow calls: about 890 files a second.
- `stat` on the drive: real times (the volume's root reports 2000-01-01), mode 0777, inode and link count 0 as on the PS5's own app file systems. Titles drop requests for files with zero times; these have none.
- Reads don't change the drive's access times.
- The mount survives the PS button, the home screen and resuming the game. It goes when SWRR exits, and ShellCore cleans up the sandbox as usual.

### Booting titles from the drive

Stock 13.60, no jailbreak (no payloads on the console), each boot in a freshly started SWRR. Times are from sending the boot to the screen, read off the HDMI picture about once a second. The installed copies of the same titles (ghostfs, [ghostfs-catalogue.md](ghostfs-catalogue.md)) are on the console's internal storage.

| | From the drive | Installed (ghostfs) |
|---|---|---|
| Ghost of Tsushima: main menu | 35 s | 80 s |
| Ghost: CONTINUE to gameplay (beach checkpoint) | 14 s | 53 s |
| Bloodborne: title screen | 30 s | 37 s |
| Bloodborne: Continue to gameplay (Iosefka's clinic) | 17 s | 40 s |

- **Through the game selector** (`--app-root usb:/ghostldr`, three titles on the drive, before games could be in folders of any name): the selector lists them with their names and art, and Ghost and Bloodborne each start from it and reach gameplay the same way. The title starts within 2 seconds of choosing it.
- **No stalls:** 30–60 seconds of each game's gameplay sampled at about 4.5 frames a second had no frame repeated (no freeze), from the drive and installed alike.
- **Nothing refused:** no unresolved or denied calls, no write to `/app0` refused, no crash, in any of the boots.
- The same title on the drive and installed, with the same prefix, carries on from the same save: both CONTINUEs loaded the same checkpoint.

### What it costs

- **Core:** 1.6 KiB more code: the dispatch to a provider of `/app0`, the early call and its clean-up, the save prefix. The image is 213 KiB of 240.
- **Extension:** 16 KiB more code for USB support and 16 KiB for the FTP server ([ftp.md](ftp.md)). The whole image is 384 KiB.
- **New JIT memory:** placing the extension early takes a JIT block of 384 KiB, out of the roughly 768 KiB a 13.60 PS5 has for a whole SWRR session. It's freed if the boot stops before the title loads.

## Limits

- **Tested on 13.60 only.** Another firmware may move the function (the unwind check then refuses), or AutoMounter may stop honouring the debug data (the mount is then refused, 0x80F40014). Both stop the boot with a log line.
- **Not yet tried:** unplugging the drive while a title runs, rest mode, a FAT32 drive, two drives at once, and Jak X (UltraC0re) as the host.
- **The selector shows no size** for titles on a drive (0 MiB): it reads sizes from an install's index, which a drive has none of.
- **One SWRR session, one mount.** The mount lasts as long as SWRR. Anything else in SWRR that calls `sceUsbStorageInit` again ends it: send no other USB storage scripts while a title runs.
- **One drive:** the first one listed whose root holds the folder, or, for the whole drive, the first with a game.
- **Up to 64 games** on a drive, 4 folders down at most ([How the PS5 finds games](#how-the-ps5-finds-games)).
- **Paths up to about 250 bytes** from the drive's top to a game's folder.

## Route B: the extension's own storage stack (off by default)

`make USB_RAW=1` builds a second way in: the extension drives the drive itself through SWRR's libSceUsbd (Bulk-Only Transport and SCSI) and reads its exFAT or FAT32 volume with its own read-only code, serving `/app0` the way ghostfs does. With it built in, it's tried when the mount fails.

It's off because it can't work as written: on a 13.60 PS5, SWRR's libSceUsbd lists no devices at all. The sandbox does have the USB devices' generic nodes (`/dev/ugenB.A`, world read-write), and FreeBSD's libusb20 calls on them (descriptors, `USB_DO_REQUEST`, the `USB_FS_*` transfer calls on endpoint 0) work on a stock console. A transport over those nodes could bring the stack back. Bulk transfers to a drive that the system's mass-storage driver holds were not tried.

| File | What it is |
|---|---|
| `usb_bot.c` | Bulk-Only Transport and its error handling (reset recovery, stalls, short data, bad or missing CSWs, unit attentions), and the SCSI commands a drive needs, over `struct usbd_ops` (bulk, control, clear-halt). |
| `blk.c` | Block cache: 16 KiB lines in a metadata pool and a file-data pool, read-ahead that doubles up to 512 KiB. |
| `part.c` | MBR, GPT (the primary checked by CRC-32, else the backup) and a volume with no partition table. |
| `fsutil.c`, `exfat.c`, `fat32.c` | exFAT (entry-set checksums, the up-case table, NoFatChain, ValidDataLength, files past 4 GiB) and FAT32/FAT16 with long names. |
| `usbfs.c` | The `/app0` provider (`struct appfs_ops`): placeholder fds, EROFS for writes, FreeBSD `stat` and `getdents`. |
| `usb_sce.c` | SWRR's libSceUsbd as `struct usbd_ops`, finding the drive, and the stack's environment. |

Built in, it adds 48 KiB of code, and 4.5 MiB of direct memory for its cache, 128 KiB for exFAT's up-case table and 200 KiB for its file table at run time. On a computer it reads 5–10 GB/s sequential and 1.3–2 GB/s in random 4 KiB reads; against a modelled USB 3 bus, 512 KiB READs stream at 90% of the bus.

## Tests

`ghostldr/tests/test_usb.py` and `test_usb_games.py` run offline:

- **App roots** (`approot.c` against a fake core and a fake libSceUsbStorage, `ext_native/approot_host.c`): RequestMap2 found through an unwind table and refused without one; Init only when the list says so; the drive that holds the folder (or a game) chosen out of two; the mount kept for the title and released when the boot stops; the FTP server's mount on a boot from anywhere, a drive plugged in later and one pulled out; games in folders of any name and depth, named by `param.sfo`, `meta.bin` or their folder, second copies, folders that can't start, hidden folders, the depth and folder limits; `--title`, the selector's choice by index, a single game's folder; `auto` falling back to the installed titles; save prefixes from the `prefix` file and the profile; a missing or empty folder.
- **The computer agrees with the console:** `usb_drive.scan` and the console's code, on one folder tree, find the same games in the same order with the same problems, and stop at the same limits.
- **The drive tools:** `ghostldr.py copy` and `check`, `gfs_pack.py --usb` and `usb_drive.py` on a folder and through a fake FTP server (`fake_ftp.py`, which answers like ftpsrv: SELF mode on, DELE's 226), including one that won't rename over a file; where a copy goes, resuming, updating and removing (only what the copy laid out); the check after copying catching a file the drive got wrong; several hundred files over eight connections; FAT32 and a 4 GiB file.
- **`ghostldr.py`:** every command's help, and what `boot` hands `run.py` (a title ID or part of a name, `--usb`, `--installed`, `--folder`, `--select`, options passed through, a loader that isn't listening).
- **`run.py`** finding a mounted drive's games and their profiles, the app root reaching Lua intact.
- **The selector:** its catalogue reading the extension's list.
- **Route B's stack:** built for the computer and run against real exFAT (MBR, GPT, none, 4 KiB sectors), FAT32 and FAT16 images made with macOS's tools (`hdiutil`, `diskutil`, `newfs_exfat`; skipped elsewhere), read as a block device and through a fake USB drive that stalls, cuts data short, sends bad CSWs and unit attentions; damaged volumes; a file past 4 GiB (`USB_BIG=1` writes a real 4.5 GiB one); `approot.c` built with `-DGHOST_USB_RAW` falling back to it; a drive prepared by the tools read back by the stack.
