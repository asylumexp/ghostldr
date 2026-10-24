# Titles on a USB drive

ghostldr can boot titles from a folder on a USB drive instead of installing them into SWRR's save containers ([ghostfs-catalogue.md](ghostfs-catalogue.md)). It needs no jailbreak: the PS5's own USB storage service mounts the drive into SWRR's sandbox, and the title's files are read from there as plain files.

**This is the recommended way to run games.** There's no install step and no packing: a game is its dump's `app0` folder copied onto the drive. Games load faster than installed ones, and SWRR's saved data only holds their progress. Install into the saved data instead when you have no spare drive, or in Jak X, where drives are untested.

Tested on a stock PS5 at firmware 13.60 with SWRR (US), with no jailbreak: Ghost of Tsushima and Bloodborne boot from a drive into gameplay, started directly and through the game selector, and PS4-Xplorer reaches its menus. Both games load faster from the drive than from an install ([Measured](#booting-titles-from-the-drive)). Missing or empty folders stop the boot cleanly.

## The drive

- **File system:** exFAT, the format the PS5 itself offers for USB storage. FAT32 works too, but holds no file of 4 GiB or more, and many PS4 titles have one. The PS5 has to accept the drive as USB storage (it lists it in Settings, Storage), not as PS5 extended storage.
- **Any size and speed the PS5 takes.** A USB 3 NVMe enclosure read at 510–540 MB/s.

Layout:

```
<drive>/ghostldr/
    CUSA12345/
        app0/          the title's app0 as dumped: eboot.bin, sce_module/, sce_sys/, ...
        prefix         one line: its save container prefix (gfs_pack.py writes it)
        meta.bin       optional: its name and art for the game selector (gfs_meta.py's format)
    CUSA67890/
        ...
```

- Each title folder is named after its title ID: 1 to 15 characters of `A`–`Z`, `0`–`9`, `_` and `-`. Other folders are ignored.
- `prefix` names the save container that holds the title's progress (below). Without it, the title gets gfs_pack.py's default: `G` and the ID's digits.
- Names compare case-insensitively, as on a PC. macOS's `._name` companion files are left out of listings.
- Files stay whole: no containers, no 1 GiB pieces, no install step.

## Putting titles on a drive

The desktop app does this on its **USB drive** path ([app/README.md](../app/README.md)): choose the drive, plugged into the computer or into the PS5, and it copies the game and boots it from there.

From a computer with the drive plugged in:

```bash
python3 ghostldr/host/gfs_pack.py --src /path/to/CUSA12345/app0 --usb /Volumes/MYDRIVE
python3 ghostldr/host/usb_drive.py list                       # every mounted drive with a ghostldr folder
python3 ghostldr/host/usb_drive.py check /Volumes/MYDRIVE     # file system, titles, sizes, prefixes
python3 ghostldr/host/usb_drive.py remove /Volumes/MYDRIVE CUSA12345
```

Or with the drive plugged into a jailbroken PS5 that runs an FTP server (ps5-payload-dev's ftpsrv, for one), through it:

```bash
python3 ghostldr/host/gfs_pack.py --src /path/to/CUSA12345/app0 --usb ftp://PS5:2121/mnt/usb0
python3 ghostldr/host/usb_drive.py list ftp://PS5:2121/mnt/usb0
```

`/mnt/usb0` is the console's path of the first USB drive (`/mnt/usb1` the second, and so on).

Or, with no jailbreak, through ghostldr's own FTP server while any title runs ([ftp.md](ftp.md)). It has the drive at `/ghostldr_usb`:

```bash
python3 ghostldr/host/gfs_pack.py --src /path/to/CUSA12345/app0 --usb ftp://CONSOLE:1212/ghostldr_usb
```

Through it, PS4-Xplorer's 79 files went on in 4 s, and large files at 25 MiB/s over one connection or 52 MiB/s over eight.

- Most of a dump is small files, and each costs a few round trips, so it sends several at once, each over its own connection: `--jobs N` (default 8).
- ftpsrv shows SELF files (`eboot.bin`, `*.prx`) decrypted unless told not to, with other sizes and contents than what was stored. The tools switch that off (ftpsrv's `SELF` command) when the server offers it.
- **How fast:** through ftpsrv 0.21 on a 13.60 console to a USB 3 NVMe drive (exFAT), Bloodborne (28,752 files, 29.3 GiB) took 13 minutes, about 38 MiB/s, and Ghost of Tsushima (40,105 files, 62.7 GiB) a little over an hour. Small files in small folders go at about 120 a second over 8 connections and large ones at 35–40 MiB/s. Ghost is slow because one of its folders holds 27,674 files: the console's exFAT gets slower to add files to a folder as it fills, down to about 10 a second past 20,000 entries, however many connections there are. Copying from a computer with the drive plugged in avoids that.
- A copy can be stopped at any time and run again: it carries on from the files already there.
- A server that takes fewer connections than `--jobs` (it answers 421) gets as many as it takes.

- `gfs_pack.py --usb` picks the title ID, prefix, name and art the same way as for an install (`--title`, `--prefix`, `--name`, `--icon`, `--backdrop`, `--fetch-art`; [compatibility.md](compatibility.md#1-pack)).
  - It copies `app0` without macOS metadata, then writes `prefix`, and `meta.bin` when there's a name or art to show.
  - On a mounted drive it refuses files of 4 GiB or more on FAT32 and checks the free space first. Over FTP neither is known, so it can't check them.
- Running it again resumes or updates the copy. Files already there with the same size are kept (on a mounted drive, also only when not older), changed ones are copied again, and files the dump no longer has are removed. On a mounted drive each file is written under a `.part` name and renamed when complete. Over FTP files are written in place, because a rename costs the console's exFAT about a quarter of a small file's time. A file cut short is shorter, so the next run copies it again.
- **Copying by hand works too:** copy the dump's `app0` folder to `<drive>/ghostldr/<TITLE>/app0`, with the folder named after the title ID. Nothing is packed or converted. `prefix` and `meta.bin` are optional. Without `prefix` the title's progress goes to `G` plus its ID's digits (`G12345SAVE`), not to its profile's prefix, so for Ghost write a one-line `prefix` file saying `GHOST` to keep using `GHOSTSAVE` (`gfs_pack.py --usb` writes it). Without `meta.bin` the selector shows the profile's name, or the title ID, on a drawn tile.

## Booting

Plug the drive into the PS5 before booting:

```bash
python3 ghostldr/host/run.py boot --app-root usb:/ghostldr                        # several titles: the selector picks
python3 ghostldr/host/run.py boot --app-root usb:/ghostldr --title CUSA12345
python3 ghostldr/host/run.py boot --app-root usb:/ghostldr/CUSA12345/app0         # one title, no selector
```

- **The title:** `--title`'s, else the only one on the drive, else the game selector's choice ([selector.md](selector.md)). `--select` shows the selector even with one title.
  - A single title's folder (`usb:/path/app0`) takes its ID from its `<TITLE>/app0` folder name, else from `sce_sys/param.sfo`, else from `--title`.
- **`--usb-drive /Volumes/MYDRIVE`:** where this computer has the drive mounted. The preflight coverage check and the titles' profiles then use the drive's dumps. It defaults to the only mounted drive with a `ghostldr` folder. Without one, both are skipped; the console doesn't need it.
- **Dev setups:** a folder of title folders on the console works the same way, without `usb:`. For example, `--app-root /data/ghostldr` holding `CUSA12345/app0`.

A boot that can't find the drive or the title stops before it touches SWRR's state, gives the drive back, and says why in the log. The next boot can follow at once, in the same SWRR session.

## Progress

A title from a USB drive keeps its progress in SWRR's save containers, in `<prefix>SAVE`, as an installed title does ([ghostfs-catalogue.md](ghostfs-catalogue.md#names)). The same title, installed and on a drive with the same prefix, shares its progress. Ghost of Tsushima uses `GHOST` in both places. Nothing is written to the drive.

## How the console reaches the drive

SWRR's sandbox has no `/mnt` and sees no USB drive by itself. The PS5's USB storage service, ShellCore's AutoMounter, can mount a drive's folder into an app's sandbox, and libSceUsbStorage (a PS4 library SWRR can load) is its client:

- **The public call is refused.** `sceUsbStorageRequestMap` answers 0x80F40014 for any folder: ShellCore's `sceProcessStarter::CheckUsbAccessDir` checks the folder against a list of USB folders in the app's `param.sfo`, and SWRR's has none.
- **The library's debug call isn't.** libSceUsbStorage also has an unexported `_sceUsbStorageRequestMap2(device, folder, mount point)`. It sends AutoMounter debug data that it logs as `skipParamSfoCheck`, and AutoMounter mounts the folder (a nullfs mount, read-write) at the mount point in the caller's sandbox. A stock 13.60 PS5 honours it.

`ext/src/approot.c` does this before the title's modules load:

1. **Find the call.** It sits 0xA0 bytes before `sceUsbStorageRequestMap` (0x90 in later PS4 builds of the library). The library's code is execute-only, so it can't be read: the module's unwind table (`.eh_frame_hdr`, in a readable segment) must list a function that starts exactly there and ends before `RequestMap`. Otherwise the boot stops and says so; nothing is called on a guess.
2. **Open the session once.** `sceUsbStorageGetDeviceList` comes first: until `sceUsbStorageInit` it answers 0x80F40009, and only then does ghostldr call Init. A second Init answers 0x80F40002 and leaves the library uninitialized, with every mount gone.
3. **Mount the drive.** For each drive in the list (device IDs change with every boot and plug), it mounts the drive's root at `/ghostldr_usb` and keeps the first whose root has the boot's top folder (`ghostldr` for `usb:/ghostldr`). The others are unmounted again.
4. **Serve `/app0`.** The title's folder under `/ghostldr_usb` becomes the app root, and the kernel's own file calls read and write it, as on a dev setup. The mount is read-write, and so is the title's `/app0` on it: a title that writes there (homebrew file managers do) writes to the drive.
5. **Keep it.** The session stays open for the whole run: `sceUsbStorageTerm`, or the session ending, would unmount the title's files. If the title calls `sceUsbStorageInit` or `Term` itself, `hle_usbstorage.c` answers them without reaching the library.

If the boot stops before the title loads (no drive, no folder, no title), the drive is unmounted and the extension's code gives back its JIT block, so the next boot finds both free.

### The boot

1. `ghost_boot` receives the extension (`ext_fetch`), as always.
2. For an app root that is `usb:...`, or a directory without `eboot.bin`, the core places the extension's code at once: a new JIT block, before SWRR's emulator memory is released. It then calls the extension's early entry, `ext_early` → `approot_init`.
   1. `usb:`: the drive is mounted as above, and the path is read under `/ghostldr_usb`.
   2. A folder of titles: the titles are counted. With several, `app_root` becomes the folder, ctx flag `CTX_FLAG_LIBRARY` is set, and the game selector lists the folders. Then `approot_pick(title)` runs.
   3. The title's folder becomes the app root, which `remap_path` maps `/app0` to. `profile_select(title)` and `gfs_use_saves(title, prefix)` follow.
   4. On failure, `ext_abandon` frees the code's JIT block and the blob, and the boot returns -6.
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

- **Through the game selector** (`--app-root usb:/ghostldr`, three titles on the drive): the selector lists them with their names and art, and Ghost and Bloodborne each start from it and reach gameplay the same way. The title starts within 2 seconds of choosing it.
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
- **One drive:** the first one listed whose root holds the folder.

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

`ghostldr/tests/test_usb.py` runs offline:

- **App roots** (`approot.c` against a fake core and a fake libSceUsbStorage, `ext_native/approot_host.c`): RequestMap2 found through an unwind table and refused without one; Init only when the list says so; the drive that holds the folder chosen out of two; the mount kept for the title and released when the boot stops; the FTP server's mount on a boot from anywhere, a drive plugged in later and one pulled out; libraries, single titles, the selector's turn and save prefixes; a missing or empty folder.
- **The drive tools:** `gfs_pack.py --usb` and `usb_drive.py` on a folder and through a fake FTP server (`fake_ftp.py`, which answers like ftpsrv: SELF mode on, DELE's 226), including one that won't rename over a file; resuming, updating and removing; several hundred files over eight connections.
- **`run.py`** finding a mounted drive's titles and their profiles.
- **The selector:** its catalogue reading a library.
- **Route B's stack:** built for the computer and run against real exFAT (MBR, GPT, none, 4 KiB sectors), FAT32 and FAT16 images made with macOS's tools (`hdiutil`, `diskutil`, `newfs_exfat`; skipped elsewhere), read as a block device and through a fake USB drive that stalls, cuts data short, sends bad CSWs and unit attentions; damaged volumes; a file past 4 GiB (`USB_BIG=1` writes a real 4.5 GiB one); `approot.c` built with `-DGHOST_USB_RAW` falling back to it; a drive prepared by the tools read back by the stack.
