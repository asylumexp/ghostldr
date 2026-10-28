# Ghost of Tsushima in SWRR on a stock PS5 (no jailbreak)

This guide runs a CUSA11456 v2.18 backup inside *Star Wars Racer Revenge* through
Luac0re on a PS5 with no jailbreak and no payloads.
The game files go on a USB drive plugged into the PS5 (recommended: no install, faster loads),
or into SWRR's own save containers over the network.

## What you need

- **The PS5,** on firmware 13.60 or later. It needs a Luac0re save for its PSN account, imported from USB.
- **The game.** *Star Wars Racer Revenge*, USA (**CUSA03474**) or Europe (**CUSA03492**).
  - ghostldr uses fixed offsets in SWRR's 01.02 executable (the save-data import slot, the video handle, a bootstrap gadget). US and EU run the same executable, so the offsets hold for both.
  - Each region keeps its own saved data. Install the game data from the SWRR you will play in; an install made in one region doesn't appear in the other.
- **A USB drive, exFAT.** It carries the Luac0re save import, and the game itself (about 70 GB free) on the recommended way.
- **A computer on the same network.** It needs:
  - this repo, Python 3.11+ and LLVM 18 (run.py builds the core; see [Building from source](../README.md#building-from-source));
  - the dependencies from `tools/fetch_deps.sh`;
  - your own extracted Ghost files at `build/ghost/app0` (eboot.bin, sce_module/, cache/, movies/ ...).
- **About 60 GB free** on the PS5, only to install the game into SWRR's saved data instead of using the drive (that moves 53 GiB over the LAN).

## 1. Trigger Luac0re

Trigger Luac0re (2.4b) via SWRR or another mast1c0re-compatible game, the usual way, until its Remote Lua Loader listens on port 9026. Any normal Luac0re save works; ghostldr needs nothing in the save.

ghostldr itself uses fixed offsets in SWRR 01.02, US or EU (see "What you need"), so another game needs those ported first.

## 2. Point the scripts at your PS5

The scripts find the console with the PlayStation discovery protocol (PS5s on UDP 9302, PS4s on UDP 987):

```bash
python3 ghostldr/host/console.py --list
```

The list shows each console's type, name, host-id, firmware, whether it is on or in rest mode, the running app (PS4s report it; SWRR is marked), and whether Luac0re's loader listens on 9026.

With one console on, or one running SWRR or Luac0re's loader, nothing needs setting. When several answer, every tool prints this numbered list and marks the one it uses. If none stands out, it asks which to use, or stops with the list when not run from a terminal. To choose one yourself, use one of:

```bash
export PS_HOST_ID=0123456789AB
```

```bash
export PS_HOST=192.168.1.123
```

`PS_HOST_ID` follows the console when DHCP moves it. The older names `PS5_HOST_ID` and `PS5_HOST` still work.

## 3. Put the game on the PS5

### On a USB drive (recommended)

Plug the drive into the computer (here `/Volumes/MYDRIVE`) and copy the dump onto it:

```bash
python3 ghostldr.py copy build/ghost/app0
```

It copies the dump to `ghostldr/CUSA11456/app0` on the drive as it is (`--folder` puts it anywhere else, under any name), with a `prefix` file (`GHOST`, so Ghost's progress is `GHOSTSAVE` as with an install) and the selector's `meta.bin`, then checks the copy. Nothing is packed: copying the folder by hand works too, in a folder of any name ([usb.md](usb.md#what-goes-on-the-drive)). Then plug the drive into the PS5. More games go next to it; `python3 ghostldr.py check` shows what the PS5 will find on a drive. With a game running, the PS5's FTP server takes copies onto the drive in it (`copy DUMP ps5`, [ftp.md](ftp.md)). `gfs_pack.py --usb DRIVE` does the same as `copy`.

### Or install it into SWRR's saved data

**From a browser, with no Python:** ghostldr's game selector → Settings → Network → **Install over network**, or send `manager.lua` to Luac0re's loader; then open the address the TV shows on a computer or phone and choose the game's folder or a `.zip` of it ([manager.md](manager.md#install-a-game)). It lays the game out exactly as the scripts below do.

**From the scripts** (developers), with Luac0re's loader listening (step 1):

```bash
python3 ghostldr/host/gfs_pack.py
```

```bash
python3 ghostldr/host/gfs_install.py
```

- **What the scripts do:** `gfs_pack.py` lays out `build/ghostfs/CUSA11456/`, with the boot files first and then each region's files together (Ghost's profile, `ghostldr/data/titles/CUSA11456.json`). `gfs_install.py` sends a receiver script to the loader, checks the console has room, and streams 56 save containers, GHOST000–GHOST055 (992 MiB each), then GHOSTINDEX (the index) and GFSCATALOG (the list of installed titles).
- **Installs from ghostldr v0.1:** GHOST000–GHOST055 with the index in GFSINDEX keep working as they are; the installer only adds them to GFSCATALOG.
- **Checking first:** `gfs_install.py --dry-run` only reports what the console has installed and what an install would change.
- **Interrupted installs:** run the same command again (after step 1 if SWRR restarted). The console itself records progress: each container gets a tag once sent, and the index and the catalogue entry stay marked incomplete until the end, so an unfinished install never boots.
- **Receiver still running:** if the installer stopped but SWRR didn't, the receiver still holds Luac0re's loader. Reattach with `--port <its port>` (the installer prints it).
- **Choosing the network link:** `--interface en0` sends through a given interface of this computer, when a slower one is the default route.
- **Speed:** about 25 MB/s, the rate the PS5 writes save data at (Ghost takes about 40 minutes). A link slower than that (busy Wi-Fi) sets the pace instead. `--no-pump` falls back to the older, slower path (about 14 MB/s).
- **Storage:** the containers belong to the SWRR region you installed from, for the signed-in user. They appear in the PS5's saved-data list as SWRR saves (about 56 GB). Deleting them there removes the install.

#### More installed titles

Titles are installed side by side; installing one never deletes another. Pack the other dump, then install it the same way:

```bash
python3 ghostldr/host/gfs_pack.py --src /path/to/CUSA12345/app0
```

```bash
python3 ghostldr/host/gfs_install.py --title CUSA12345
```

- **Space:** before sending, the installer checks that the new containers fit. If they don't, it stops and changes nothing: free space on the PS5, or remove a title.
- **What is installed:** `gfs_install.py --list`.
- **Removing a title:** `gfs_install.py --remove CUSA12345` deletes its data containers and index. Its progress (`<prefix>SAVE`, GHOSTSAVE for Ghost) stays unless you add `--remove-saves`. SWRR's own save is never touched.
- **Progress:** each title keeps its progress in its own `<prefix>SAVE` container (a new title CUSA12345 gets `GCUSA12345SAVE`; titles installed before 2026-10-28 keep `G12345SAVE`, docs/ghostfs-catalogue.md). A title that mounts save directories (most games, unlike Ghost) also gets one container per directory, `<prefix>D001SAVE` and up, listed in `<prefix>SAVE`'s catalogue; save data memory too big for `<prefix>SAVE` goes to `<prefix>M<slot>SAVE`. The installer never deletes these unless asked, and the receiver refuses to.
- **Other titles:** ghostldr is title-agnostic. Bloodborne 1.09 reaches gameplay; other games are untested. [compatibility.md](compatibility.md) covers bringing one up: reading the log, profiles and HLE rules.
- Names and formats: [ghostfs-catalogue.md](ghostfs-catalogue.md).

## 4. Boot Ghost

With Luac0re's loader listening (a fresh SWRR start after every Ghost session), from the USB drive:

```bash
python3 ghostldr/host/run.py boot --snap
```

`--app-root` defaults to `auto`: the games on a USB drive in the PS5, else the installed ones. `--app-root usb:/` only looks on the drive, `--app-root ghostfs` only at the installed games. With a prebuilt core in `build/ghostldr` and no LLVM, add `--no-build`; `python3 ghostldr.py boot` does that by itself.

- **Extension:** run.py serves `build/ghostldr/ext.bin` (the NID table and extra HLE code) on TCP 18195, and the core fetches it before touching the title. Without it, the boot stops there with `ext: ...` in the log. See [ghostldr/native/ext/README.md](../ghostldr/native/ext/README.md).
- **Several titles:** add `--title CUSA11456`. Without it the only title boots; with several, the game selector shows them on the TV.
- **Logging:** the core logs to `build/ghostldr/run.log` (UDP 18194 to this computer). Expect `ghostfs: CUSA11456, 40116 entries ...`, `profile: CUSA11456 own profile ...`, then `ghostfs: mounted GHOST000 ...` and the boot progress.
- **Load times:** the first load reads several GiB from the containers, so the title screen and the first world load take longer than from an internal drive.
- **Saves:** Ghost's saves go into a separate container, **GHOSTSAVE**, which is created on the first save. SWRR's own save is never touched.
- **After a session:** when you quit or Ghost stops, close SWRR from the PS menu and start again at step 1.

## Troubleshooting

- **`receiver did not come up` / no reply on 9026:** Luac0re's loader isn't listening. Redo step 1. Any earlier Lua script must have finished; a stuck one holds the loader until SWRR restarts.
- **`no space left on the console's storage` (0x809f000a):** free space on the PS5, then rerun the installer to resume.
- **The boot log stops at `ghostfs: save data unavailable`:** the SWRR executable differs from 01.02 (US and EU 01.02 are the same). Its sceSaveDataMount import slot (`SWRR_SAVEDATA_MOUNT` in `ghostldr/native/src/ghostfs.c`, and in `gfs_receiver.lua` and `save_resign.lua`) must be found again for that version.
- **Memory:** SWRR's memory budget is fixed by the system. Ghost runs with a 590 MiB streaming pool and skips a movie when the video decoder can't get memory. Newer firmware may change the budget. If the boot stops at a memory plan error, the `run.log` shows the plan (`budget:` lines). The plan is in Ghost's profile and applies only to its v2.18 executable: another build runs with the generic budget, which Ghost doesn't fit.
- **`ghostfs: several titles: pick one`:** more than one title is installed: boot with `--title`.
- **`not enough space on the console`** from the installer: nothing was sent or deleted. Free space on the PS5 or remove a title, then rerun.
