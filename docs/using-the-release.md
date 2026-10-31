# Using the release

This takes a ghostldr release from download to gameplay on a stock PS5: no jailbreak, no Python, and no program left running on a computer. A computer or a phone only sends two small files to the PS5, and, if you install a game without a USB drive, opens a web page. Ghost of Tsushima is the example; other games and OpenOrbis homebrew go the same way with their own dump ([compatibility.md](compatibility.md) says what is known to work).

**Supported:**

- **PS5:** every firmware from 13.60 up, with *Star Wars Racer Revenge* (SWRR) from the USA or Europe.
- **PS4:** homebrew and small titles. Big games can't run there: the kernel gives SWRR, a PS2 Classic, 2.5 GiB of memory at most ([ps4.md](ps4.md), and [below](#on-a-ps4-or-in-jak-x)).
- **Jak X** with UltraC0re instead of SWRR: experimental ([jakx.md](jakx.md)).

There are two ways to get a game to the PS5:

- **A USB drive (recommended):** copy the game's folder onto an exFAT drive and plug it into the PS5. There's no install step, games load faster (Ghost reaches its menu in about 35 seconds instead of 80), and SWRR's saved data only holds the game's progress ([usb.md](usb.md)).
- **SWRR's saved data:** install the game from a browser on a computer or phone, over the network ([below](#no-drive-install-over-the-network)). About 35 minutes for a 50 GB game. Use it when you have no spare drive, on a PS4, or in Jak X.

## What you need

- **A PS5 on firmware 13.60 or later with SWRR, USA (CUSA03474) or Europe (CUSA03492),** where Luac0re 2.4b already works: its Remote Lua Loader starts when you open HALL OF FAME. Both regions work the same way, but they keep separate saved data, so play each game from the same region every time.
- **Your own dump of the game** as a folder of files: the folder with `eboot.bin` in it, with `sce_module/`, `sce_sys/` and the game's data (about 63 GB for Ghost of Tsushima v2.18, CUSA11456). Use a dump with its update merged in. The release has no game files.
- **An exFAT USB drive** with room for the game. FAT32 works too, but holds no file of 4 GiB or more, which many games have (Ghost doesn't). Your other files on the drive are left alone. Without a drive: free space on the PS5 for the install instead.
- **Something to send a file over TCP,** on the same network as the PS5: `nc` on macOS or Linux, or a payload-sender app on a phone or on Windows (or `nc` in WSL).
- **To install without a drive:** a web browser on a computer or phone on the same network.

## 1. Download

From the [release page](https://github.com/asylumexp/ghostldr/releases/latest), download `ghostldr-<version>.zip` and unpack it. It has six files, all at its top:

| File | What it is |
|---|---|
| `ghostldr.lua` | The bootstrap: you send it to Luac0re's loader every session. |
| `ghostldr-ext.bin` | The games: ghostldr's extension and the game selector. Sent once; `ghostldr.lua` keeps it on the PS5 afterwards. |
| `ghostldr-elfldr.bin` | The [ELF loader](elfldr.md) on its own, for payloads without games (PS5). |
| `manager.lua` | The [web manager](manager.md): installs a game over the network, and backs up saves, without booting ghostldr. |
| `README.txt` | The short version of this page. |
| `SHA256SUMS.txt` | Their checksums. |

The same files are attached to the release page one by one too (handy on a phone). Check them in the folder they are in:

```bash
shasum -a 256 --ignore-missing -c SHA256SUMS.txt
```

`ghostldr.lua` only takes a `.bin` from the same release: keep them together. The release page's `ghostldr-dev-payloads-<version>.zip` is for developers with a jailbroken console only. The Python tools and the source are in the [repository](https://github.com/asylumexp/ghostldr) ([For developers](#for-developers)).

## 2. Put the game on a USB drive

Plug the drive into a computer and copy the dump's folder onto it: the folder with `eboot.bin` in it, as it is, with everything in it. Any folder name works, anywhere on the drive up to four folders down, and games sit side by side:

```
<drive>/
    Ghost of Tsushima (CUSA11456)/      eboot.bin, sce_module/, sce_sys/, ...
    Games/
        bloodborne/
            app0/                       eboot.bin, ...
```

- **The title ID:** the PS5 reads it from the game's `sce_sys/param.sfo`. A dump without one (Ghost's often hasn't) needs the ID in its folder's name, like `Ghost of Tsushima (CUSA11456)`.
- **Names and pictures in the game selector** come from the game's `sce_sys` (`param.sfo`, `icon0.png`, `pic1.png`). A game without them gets a tile drawn in its colour, named by ghostldr's profile for it when there is one (Ghost of Tsushima and Bloodborne have one), else by its title ID.
- **The drive** must be used by the PS5 as USB storage, not as extended storage. Up to four drives are read at once, exFAT or FAT32.
- **Updates** must be in the dump's folder (a dump of the updated game); a separate update folder isn't merged in. DLC isn't loaded.

Then eject the drive and plug it into the PS5. [usb.md](usb.md#what-goes-on-the-drive) has the details: how the PS5 finds games, and the limits. While a game runs, ghostldr's FTP server can also put more games onto the drive in the PS5, from an FTP client ([ftp.md](ftp.md)); copying on a computer is quicker.

No drive? Go on with steps 3 and 4: the game selector then has nothing to start, and [Install over network](#no-drive-install-over-the-network) puts a game into SWRR's saved data.

## 3. Start Luac0re

1. Start SWRR, click the right half of the touchpad at PRESS START, then go to **OPTIONS → HALL OF FAME**. Wait until the loader says it is listening on port 9026. If it says the exploit failed, close SWRR and try again.
2. Find the PS5's address: **Settings → Network → Connection Status → View Connection Status → IPv4 Address**. It is `CONSOLE` below.

Never save from SWRR's own menus, and never confirm a "corrupted, will be deleted" prompt.

## 4. Boot

Send the bootstrap to Luac0re's loader:

```bash
nc CONSOLE 9026 < ghostldr.lua
```

Luac0re's dialog on the TV now shows ghostldr's prompt, with the PS5's address:

```
ghostldr 236824efa5f1862e

Send ghostldr-ext.bin (games)
or ghostldr-elfldr.bin (ELF loader)
to 192.168.1.123:9021

Waiting 5 minutes
```

Within five minutes, send the games' bundle to that port:

```bash
nc CONSOLE 9021 < ghostldr-ext.bin
```

On a jailbroken PS5, whose own ELF loader has 9021 already, the prompt says `9022` instead: send it there. The game selector opens on the TV ([step 5](#5-the-game-selector)), and `nc` shows the console's log until you press Ctrl+C (that stops the log, not the game). A payload-sender app that closes the connection right after sending works too; only the log is lost.

- **Which `nc`:** Luac0re's loader runs a script when its connection ends. macOS's `nc` ends it by itself; on Linux add `-N` to the first command (`nc -N CONSOLE 9026 < ghostldr.lua`), or `-q 0` with the traditional netcat. The bundle needs neither.
- **Something else sent to 9021** (a Lua script, an ELF, a bundle from another release, one cut short) is refused with the reason on the TV and on its connection, and the prompt keeps waiting.
- **Nothing in five minutes:** the TV says it timed out and SWRR is back at Luac0re's loader: send `ghostldr.lua` again.

### Every later session: ghostldr.lua alone

The first bundle you send is kept on the PS5, in SWRR's saved data (`GLDRCACHE`, 16 MiB). From then on, send only the first file:

```bash
nc CONSOLE 9026 < ghostldr.lua
```

- **The game selector opens at once,** with the games on the USB drives and the installed ones.
- **On a PS5 the ELF loader listens beside it** on 9021 (9022 on a jailbroken console), until you pick a game: send payloads there as usual ([elfldr.md](elfldr.md)). When you pick a game, it waits for any payload still running (the TV says so) and goes away before the game loads.
- **The log:** nothing was sent, so no connection carries it. `printf 'log\n' | nc CONSOLE 9021` while the selector shows makes that connection the log, the game's included. It is also on the USB drive ([When something goes wrong](#when-something-goes-wrong)).
- **A new release:** its `ghostldr.lua` sees the old bundle, says `Version mismatch` above the usual prompt, and the bundle you send replaces it. A newer bundle sent to the ELF loader while the selector shows is kept for the next boot: "The new ghostldr ext file will be applied on the next ghostldr boot."
- **A damaged cache** is deleted, the TV says `The cached bundle is damaged: deleted. Send the bundle again.`, and the prompt asks for it.
- **Clearing it:** `printf 'forget\n' | nc CONSOLE 9021` while the ELF loader listens, or delete `GLDRCACHE` as under [Removing it](#removing-it).

After a game has run, SWRR must be closed and started again before the next boot: PS button, SWRR's card, Options, Close Game. Then HALL OF FAME, and `ghostldr.lua` again.

## 5. The game selector

The selector shows every game it found, on the USB drives in the PS5 and installed in SWRR's saved data, even when there is only one ([selector.md](selector.md)):

| Button | What it does |
|---|---|
| Left / Right, L1 / R1 | Move along the row (L1 and R1 jump five). |
| Cross | Start the selected game. |
| Options | The game's details: title ID, version, where it is, and what's wrong with it, if anything. |
| Triangle | Refresh: look for games again. A game copied onto the drive (over FTP, say) shows, one taken off goes, without restarting SWRR. |
| Square | Settings (as does the gear at the end of the row). |

**Settings** (Up and Down choose a section, Right or Cross goes in, Left or Circle comes back out):

- **Games:** the drive check. Every game, with `Ready`, or what is wrong and what to do in plain words: an install that isn't finished, a folder on a drive that can't start and why (no `eboot.bin`, no title ID, ...), a second copy of a game. PS5 games on the drive aren't listed (SWRR runs PS4 games): a line says how many were left out. **Cross on a game installed in saved data deletes it**, after a Yes (No is selected first): its install goes, its saves stay.
- **USB drives:** each drive the PS5 has, its file system (exFAT or FAT32, with FAT32's 4 GiB limit), size and free space, the games found on it and the folders that can't start. **Look again** reads the drives again after you plug one in, and the row with them (as Triangle does).
- **Network:** the PS5's address, the ports ghostldr uses, and **Install over network** ([below](#no-drive-install-over-the-network)).
- **Logs:** **Write logs to the USB drive** (on by default), and where the log goes ([logs.md](logs.md)).
- **Advanced:** what were developer options, as switches: **Show the system's dialogs**, **Plan memory automatically**, **Keep Remote Play on**, **Saves in FTP** (the FTP server's `saves` folder: every logged-in user's saves; [ftp.md](ftp.md#saves)), and **Reset to defaults**. A change counts at once, for the game you pick next, and is kept in SWRR's saved data (`GLDRCONF`).
- **Game art:** **Fetch missing art** (off by default) gives a game with no picture or backdrop of its own one from SteamGridDB or IGDB, found by its name and kept with the game. It needs your own key: a SteamGridDB API key (steamgriddb.com › Preferences › API), or an IGDB Client ID and Client Secret (a Twitch application: dev.twitch.tv › Console). Type them on the on-screen keyboard there; they stay in `GLDRCONF`.
- **Safe mode:** if a boot with Advanced's switches stops before the selector comes up, the next boot leaves them out once, and the TV says "Settings > Advanced is left out of this boot", so a switch can't lock you out. Change it there, or **Reset to defaults**.
- **About:** ghostldr's version and build, the bundle cache, the console and its firmware, SWRR's region, and **Close ghostldr**: after a Yes, SWRR is back at Luac0re's loader ("Closed in Settings"), which takes `ghostldr.lua` again.

Pick a game with Cross. The first boot of a game takes a minute or two. On a new save Ghost goes through brightness, audio output, difficulty and experience (keep Standard: only English audio is installed), then the opening. A game's progress goes into SWRR's saved data (`GHOSTSAVE` for Ghost, `<prefix>SAVE` for others), never onto the drive, and SWRR's own save is never touched. While a game runs, the PS5 also serves FTP on port 1212 ([ftp.md](ftp.md)).

## No drive: install over the network

A game can go into SWRR's saved data from a browser on a computer or phone. Nothing else is needed on that device ([manager.md](manager.md#install-a-game)).

1. **Start the installer,** either way:
   - **From the game selector:** Settings → Network → **Install over network**. A card on the TV shows the address to open, a QR code of it for a phone's camera, and a token.
   - **Without booting ghostldr:** with Luac0re's loader listening, `nc CONSOLE 9026 < manager.lua`. A notification shows the address and the token; open `http://CONSOLE:8090/install`.
2. **Open the address** in the browser and choose the game:
   - **Choose folder…** on a computer: the dump's folder, the one with `eboot.bin` (dragging it onto the page works too);
   - **Choose .zip…** on a phone (or a computer): a `.zip` of that folder.
3. **Check** the name, title ID, version and size the page read, type the **token** from the TV, and **Install**.

The page shows the progress, the speed and the time left. On a PS5 it goes at 22–25 MB/s, the rate the PS5 writes saved data at: a 50 GB game takes about 35 minutes. Keep the page open, and the phone or computer awake, until it says the game is installed.

- **Keep the PS5 out of rest mode** while it installs (Settings → System → Power Saving).
- **Interrupted** (Wi-Fi, a sleeping phone): the page carries on by itself. Closed the page? Choose the same game again and Install: what's on the PS5 already is kept.
- **Not enough space:** the page checks before sending anything and says how much is missing.
- **In the selector**, the new game joins the row when it's installed: pick it. With `manager.lua`, press **Exit** on the page (Luac0re's loader is back), then send `ghostldr.lua`.
- **Remove:** the page's list of installed games has a **Remove** button per game. Its data goes; its progress stays.
- **Several games** live side by side; installing one never deletes another.

## When something goes wrong

**ghostldr says every failure on the TV:** what went wrong and what to do, and where the log is ([failures.md](failures.md) lists every message):

- **A boot that stops** before the game runs shows its message in the system's dialog. Press OK: SWRR goes back to Luac0re's loader, which says "The boot stopped. Back to Luac0re's loader." Send `ghostldr.lua` again. **If you press OK on that dialog too and the TV goes black, the next `ghostldr.lua` isn't taken** (seen in the final test): close SWRR from the PS button menu, start it again and open HALL OF FAME.
- **A game that crashes** shows ghostldr's own page in place of its picture: close SWRR from the PS button menu and start it again. **A game that quits** closes SWRR (a PS5 starts it again).
- **A game that stops drawing** gets a notification; it may come back by itself.
- **The system's own error** ("Something went wrong with this game or app", CE-108255-1) is a crash ghostldr couldn't catch: press OK and start SWRR again. If it happens again at the same place, this game may not work with ghostldr yet.

**The logs** ([logs.md](logs.md)): with a USB drive in the PS5, each boot's log is a file on it, `ghostldr/logs/NNNN_YYYY-MM-DD_HHMM.log` (the 30 newest boots are kept), so you can read it on a computer or attach it to a bug report. Without a drive it is `/temp0/ghostldr.log` inside SWRR, which ghostldr's FTP server shows while a game runs, and the connection the bundle was sent on (or `printf 'log\n' | nc CONSOLE 9021` beside the selector) gets it as it happens.

**Common problems:**

- **"No games found." or "No games on the USB drive."** Check the drive in Settings → USB drives (it may need **Look again**) and Settings → Games, which says what is wrong with each folder. Folders deeper than four levels aren't searched.
- **"The game's folder doesn't say which game it is.":** name the folder after the title ID, like `My Game (CUSA12345)`, or copy the dump's `sce_sys/param.sfo` with it.
- **Reads from the drive time out:** the game's files can't be read and each read fails after about 25 seconds. That's the drive's USB link: unplug the drive and plug it in again, preferably in a rear USB port. A bus-powered SSD or hard drive beside another drive may need a port of its own (or a powered hub). If reads still fail, restart the PS5 ([usb.md](usb.md#pulling-the-drive-out-rest-mode)).
- **The drive was pulled out** while a game runs: the game waits, and the TV says to plug it back in. Plug the same drive back in (any port) and the game carries on. **Rest mode** with a game running: wake the PS5 and the game carries on ([usb.md](usb.md#pulling-the-drive-out-rest-mode)).
- **`Version mismatch`, or a bundle refused as damaged:** `ghostldr.lua` and the `.bin` are from different releases. Send both from the same one.
- **"The game needs more memory than SWRR has.":** this game can't run with ghostldr yet (on a PS4, most games: [ps4.md](ps4.md)).
- **The PS5 asks for the disc:** the SWRR disc must be in the console that runs it.

## On a PS4, or in Jak X

- **PS4:** the same two files boot ghostldr there, and the cached bundle opens the selector at once, without the ELF loader (it needs a PS5; `ghostldr-elfldr.bin` is refused). Games come from SWRR's saved data only: install them over the network ([above](#no-drive-install-over-the-network); about 40 MB/s was measured on a PS4 Pro). Homebrew and small titles run; the selector marks a game's dump `May not run on PS4` and asks before it boots one. Logs stay in `/temp0/ghostldr.log`. [ps4.md](ps4.md) has why big games can't run.
- **Jak X with UltraC0re** (Europe, CUSA07992, update 1.04) is experimental, with its own installs of each game. `ghostldr.lua` would go to UltraC0re's port 8888 instead of 9026, but the two-file boot hasn't been tried there: [jakx.md](jakx.md) has the tested way.

## Removing it

- **A game on the USB drive:** delete its folder on a computer. Its progress stays in SWRR's saved data.
- **An installed game:** Settings › Games, Cross on it, Yes (its progress stays). Or **Remove** in the install page's list of installed games. Or by hand in **Settings → Saved Data and Game/App Settings → Saved Data (PS4) → Console Storage → Star Wars Racer Revenge**: its data containers (`GHOST000`…`GHOST055` for Ghost), its index (`GHOSTINDEX`) and, if it's there, `GHOSTMETA`.
- **The bundle cache,** `GLDRCACHE` (16 MiB): `printf 'forget\n' | nc CONSOLE 9021` while the ELF loader listens, or delete it in the same Saved Data list. **ghostldr's settings,** `GLDRCONF`: Settings → Advanced → **Reset to defaults** puts every switch back; the container can be deleted in the same list.
- **Keep** the games' progress (`GHOSTSAVE`, `<prefix>SAVE`) and the Luac0re save (`SLUS-20268` on US SWRR, `SLES-50366` on EU) unless you mean to remove them too.

## Tested for 1.0

On a stock PS5 at 13.60 with US SWRR and two exFAT drives, booted this way (two files with `nc`, then `ghostldr.lua` alone, picks in the selector):

- the selector with 43 games from both drives, and every Settings section;
- **Install over network** of a homebrew from the selector, and its removal (driven by the install page's own code from a computer, not through a browser's window);
- a payload on the ELF loader beside the selector, then a game picked (it waited for the payload);
- Ghost of Tsushima from a drive into gameplay, and a save; Bloodborne 1.09 from a drive, Continue into gameplay;
- each boot's log on the drive, and a boot failure's message on the TV;
- Apollo Save Tool from a drive.

Before the final build: a FAT32 stick, a game running through rest mode, and a drive pulled out and plugged back in (both with a test homebrew; the last fix to the drive's return is tested offline only). On a 12.50 PS4 Pro: the selector, Settings, and Install over network from `manager.lua` and from the selector (a folder and a `.zip`, an install cut short and carried on), then the installed homebrew booted from the row. The install page itself was tried in a desktop browser against an offline build; an install from a phone's browser hasn't been tried on a console yet.

## For developers

The Python tools (`ghostldr.py copy`, `check`, `boot`, `install`, `run.py` and its developer options), building from source and making a release are in the repository: [host-tooling.md](host-tooling.md), [ghost-on-stock-ps5.md](ghost-on-stock-ps5.md) and [architecture.md](architecture.md).
