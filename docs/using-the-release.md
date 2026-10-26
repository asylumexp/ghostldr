# Using the prebuilt release

This walks through running Ghost of Tsushima from a ghostldr release on a stock PS5: no jailbreak, no compiler, no build step. It covers download to gameplay. [ghost-on-stock-ps5.md](ghost-on-stock-ps5.md) has the background and more troubleshooting. Other games and OpenOrbis homebrew follow the same steps with their own dump; [compatibility.md](compatibility.md) says what is known to work.

There are two ways to get a game to the PS5:

- **A USB drive (recommended):** copy the game's folder to an exFAT USB drive and plug it into the PS5. There's no install step, games load faster (Ghost reaches its menu in about 35 seconds instead of 80), and SWRR's saved data only holds the game's progress ([usb.md](usb.md)).
- **SWRR's saved data:** install the game over the network into SWRR's saved data, about 40 minutes for Ghost. Use this when you have no spare drive, or in Jak X, where USB drives are untested.

Steps 1 to 4 below take the USB drive way. [The saved-data way](#the-saved-data-way) replaces steps 3 and 4. Every command is `python3 ghostldr.py` and a word: `doctor`, `copy`, `check`, `boot`, `logs` or `install` (`python3 ghostldr.py --help` lists them, `python3 ghostldr.py boot --help` explains one).

## What you need

- **A PS5 with *Star Wars Racer Revenge*, USA (CUSA03474) or Europe (CUSA03492).** Luac0re 2.4b must already work on it: its Remote Lua Loader starts when you open HALL OF FAME. Both regions work the same way; play each game from the same region every time, since US and EU SWRR keep separate saved data (and so separate progress).
  - *Jak X: Combat Racing* (Europe, CUSA07992, update 1.04) with UltraC0re works too, experimentally: [jakx.md](jakx.md) has what it needs and how it differs. It takes its own install.
- **Your own Ghost of Tsushima v2.18 dump (CUSA11456)** as a folder of files (`eboot.bin`, `sce_module/`, `cache/`, `movies/` ...), about 63 GB. The release contains no game files.
- **A USB drive formatted exFAT, with about 70 GB free.** A USB 3 drive or SSD is best. FAT32 works too, but holds no file of 4 GiB or more, which many games have (Ghost doesn't). Your other files on the drive are left alone. (For the saved-data way instead: about 60 GB free on the PS5.)
- **A computer on the same network as the PS5** with Python 3.11 or newer.
  - macOS and Linux work; Windows is untested.
  - A wired or fast Wi-Fi connection helps.
- **The computer's firewall must let Python receive** on UDP port 18194 (the game's log), TCP port 18195 (the loader fetches its extension from the computer) and TCP port 18196 (the game selector, when there is more than one game to choose from). On macOS, click Allow when asked.

## The easy way: the app

The release includes a desktop app that does everything below without a terminal. It needs only Python 3.11 or newer.

1. Download and unpack the release as in step 1.
2. Start the app from the `app` folder:
   - **macOS:** double-click `ghostldr.command`. The first time, right-click it, choose **Open**, then **Open**.
   - **Windows:** double-click `ghostldr.bat`.
   - **Linux:** run `ghostldr.sh`.

The app takes you through the same steps as this guide, on one of two paths you choose at the top:

- **USB drive (recommended):** copy each game to an exFAT USB drive and play it from there.
- **Saved data:** install each game into SWRR's saved data.

Either way the steps are:

- **Console:** find the PS5, start Luac0re and check the setup.
- **Game:** choose your dump (and, for saved data, prepare it).
- **USB drive** or **Install**, then **Play**, with the game's log live.

It also keeps a library of the games on the drive and installed, and offers the save manager.

You still need everything under What you need, including the firewall rules. [app/README.md](../app/README.md) has more about the app. The rest of this guide is the command-line way, which is what the app runs for you.

## 1. Download and unpack

From the release page, download `ghostldr-v0.6.6.zip` and `SHA256SUMS.txt`. You only need the dev payload zip on a jailbroken console. Check the download:

```bash
shasum -a 256 --ignore-missing -c SHA256SUMS.txt
```

Unpack it and work from inside the folder; every command below runs there:

```bash
unzip ghostldr-v0.6.6.zip
```

```bash
cd ghostldr-v0.6.6
```

## 2. Prepare the PS5

1. **Rest mode:** stop the PS5 from sleeping during long transfers. Go to **Settings → System → Power Saving → Set Time Until PS5 Enters Rest Mode** and pick **Don't Put in Rest Mode** (or a long time) while playing media.
2. **Start Luac0re:** start SWRR, click the right half of the touchpad at PRESS START, then go to **OPTIONS → HALL OF FAME**. Wait until the loader says it is listening on port 9026.
   - If it says the exploit failed, close SWRR and try again.
   - Never save from SWRR's own menus, and never confirm a "corrupted, will be deleted" prompt.
3. **Find the PS5's address:** **Settings → Network → Connection Status → View Connection Status → IPv4 Address**. Then on the computer:

   ```bash
   export PS_HOST=192.168.1.123
   ```

   You can usually skip this: the tools find the console themselves, and with several on they pick the one where Luac0re's loader is listening. `python3 ghostldr/host/console.py --list` shows what they see; `--console 192.168.1.123` on any command picks one.
4. **Check the setup** with the loader still listening:

   ```bash
   python3 ghostldr.py doctor
   ```

   It prints one PASS, WARN or FAIL line per check: Python's version, the prebuilt core against `core.json`, the size of the Lua payload, whether UDP 18194 and TCP 18195 are free on this computer, and whether Luac0re's loader answers on the PS5, then the USB drives mounted on this computer and the games on them. Fix any FAIL before going on: each says what to do. `WARN title dump` is normal with a release: it has no name table to check the dump against. Doctor can't see the firewall, so the ports still need allowing as above.

## 3. Copy the game to the USB drive

Plug the drive into the computer and copy the game onto it, naming the dump's folder (the one with `eboot.bin`, `sce_module` and `sce_sys` in it):

```bash
python3 ghostldr.py copy /path/to/CUSA11456/app0
```

- **Which drive:** the only exFAT or FAT drive mounted on the computer. With several, name it after the dump: `/Volumes/MYDRIVE` on macOS, a drive letter such as `E:\` on Windows, usually `/media/<you>/<drive>` on Linux.
- **What it writes:** `ghostldr/CUSA11456/` on the drive: the game's `app0` as it is, a `prefix` file that names its progress save, and its name and art for the game selector (`meta.bin`). `--folder "Games/Ghost of Tsushima"` puts it somewhere else: any folder name works. Nothing else on the drive changes.
- **It checks first:** the drive's format (FAT32 holds no file of 4 GiB or more) and its free space. If either rules the game out, it stops before copying anything.
- **Progress:** a bar with the speed and the time left.
- **Then it checks the copy:** every file's size, and the files a boot reads first byte for byte. A file the drive got wrong is copied again.
- **If it stops,** run it again: files already on the drive are kept, and the copy carries on.
- **More games:** run it with another dump; games sit side by side on the drive.
- **Copying by hand** (Finder, Explorer) works too: put the dump's folder anywhere on the drive. If it has no `sce_sys/param.sfo` (Ghost's often hasn't), put the title ID in the folder's name, like `Ghost of Tsushima (CUSA11456)`. [usb.md](usb.md#what-goes-on-the-drive) has what goes on the drive.
- **Check what the PS5 will find:** `python3 ghostldr.py check` lists the games on the drive, where their progress goes, and what is wrong with any folder it can't start.
- **Without unplugging the drive from the PS5:** while a game runs through ghostldr, the PS5 serves FTP on port 1212 ([ftp.md](ftp.md)), so `python3 ghostldr.py copy /path/to/dump ps5` copies onto the drive in the PS5. That goes at network speed, and a game with as many files as Ghost takes about an hour, so copying on the computer is quicker.

Then eject the drive from the computer and plug it into the PS5.

## 4. Play

1. **Restart SWRR:** close SWRR (PS button → SWRR's card → Options → Close Game), start it again and open HALL OF FAME.
2. Start the game:

   ```bash
   python3 ghostldr.py boot
   ```

   With several games on the drive, the game selector shows them on the TV; `python3 ghostldr.py boot CUSA11456` (or `boot ghost`) starts one straight away.

The PS5 mounts the drive itself as the boot starts: the log shows `approot: USB drive … mounted at /ghostldr_usb`, a line per game it found, and the one it starts. Before sending, it prints a `preflight:` line. With a release it says `skipped`: the release has no name table to check the dump against, so it boots straight on.

The first boot takes a minute or two. On a new save Ghost goes through brightness, audio output, difficulty and experience (keep Standard: only English audio is installed), then the opening. Its progress goes into SWRR's saved data as `GHOSTSAVE`, and SWRR's own save is never touched.

`ghostldr.py boot` keeps printing the console's log until you press Ctrl+C; stopping it doesn't stop the game. The session is also written to `build/ghostldr/run.log` (`python3 ghostldr.py logs --saved` prints its end). That file is what to look at, or share, if something goes wrong.

While the game runs, the PS5 also serves FTP on port 1212 ([ftp.md](ftp.md)): the boot prints the address.

**Every later session:** keep the drive plugged into the PS5, close SWRR, start it again, open HALL OF FAME, and run the same command.

## The saved-data way

Instead of steps 3 and 4, the game can be installed into SWRR's saved data over the network. It needs no drive, but a big game takes a while to install, and loads more slowly.

### Install onto the PS5 (once, about 40–100 minutes)

With SWRR at the Luac0re loader:

```bash
python3 ghostldr.py install /path/to/CUSA11456/app0
```

It first lays the game out for SWRR's save containers on the computer (a few minutes, in `build/ghostfs/CUSA11456/`; for Ghost, 40,100 files, 53.3 GiB, in 56 containers), then sends it. The two steps are `ghostldr/host/gfs_pack.py` and `gfs_install.py`, which take more options.

- **Progress:** it prints a line per container (`[12/56] GHOST011: 992 MiB in 75 s ...`) and finishes with `install complete`.
- **Check first:** `python3 ghostldr/host/gfs_install.py --dry-run` shows what is installed and what would change, without sending anything.
- **If it stops** (network, PS5 asleep, SWRR closed): run the same command again. It keeps what was already sent. If SWRR closed, start Luac0re again first.
- **`no space left on the console's storage`:** free space on the PS5 and run it again.
- **A slow default network link** on the computer: `install DUMP --interface en0` sends through a chosen interface (macOS).
- **Speed:** about 25 MB/s, which is as fast as the PS5 writes save data, so Ghost takes about 40 minutes. A wired PS5 or a good 5 GHz Wi-Fi link keeps up with that.

The data lands in SWRR's saved data under the signed-in user, as 58 entries: `GHOST000`…`GHOST055`, `GHOSTINDEX` and `GFSCATALOG` (an install made with v0.1 has `GFSINDEX` instead of `GHOSTINDEX`, and keeps working). It belongs to the SWRR region you installed from: US and EU SWRR each keep their own saved data, so install from the one you will play in.

- **Not enough space:** the installer checks before sending and stops without changing anything. Free space on the PS5 and run it again.
- **Other titles** can be installed next to Ghost. Bloodborne 1.09 reaches gameplay; other games are untested ([compatibility.md](compatibility.md)).

### Play an installed game

1. **Restart SWRR after the install.** The loader can be left busy afterwards. Close SWRR (PS button → SWRR's card → Options → Close Game), start it again and open HALL OF FAME.
2. Start the game:

   ```bash
   python3 ghostldr.py boot
   ```

   With no game on a USB drive in the PS5 it boots the installed ones; `boot --installed` skips the drive.

Everything else is as in step 4: the preflight line, the first boot, the log and `GHOSTSAVE`. **Every later session:** close SWRR, start it again, open HALL OF FAME, and run the same command. The install is only needed once per PSN user.

## Names and pictures in the game selector (optional)

With more than one game to choose from, or with `ghostldr.py boot --select`, ghostldr shows a game picker in the style of the PS4 home screen before the game loads ([selector.md](selector.md)). A dump without `sce_sys` (Ghost's usually has none) shows its name from ghostldr's profile and a tile drawn in its colour.

To show the game's official icon and background instead, for a game on a USB drive, copy it again with `--fetch-art` (only the name and art are rewritten; the game's files are already there):

```bash
python3 ghostldr.py copy /path/to/CUSA11456/app0 --fetch-art
```

For an installed game, with SWRR at the Luac0re loader:

```bash
python3 ghostldr/host/gfs_install.py --update-meta --title CUSA11456 --fetch-art
```

Both download the art from the PlayStation Store's public pages into `build/ghostfs/CUSA11456/art`. The installed version writes one small entry, `GHOSTMETA` (about 3 MB), to the PS5; the game data, its progress and SWRR's own save are not touched, and nothing is reinstalled. Your own pictures work too: `--icon tile.png --backdrop background.png`. Restart SWRR before the next `ghostldr.py boot`.

## Troubleshooting

Each command says what went wrong and what to do; these are the common ones.

- **`the PS5 at … isn't listening for ghostldr`:** Luac0re's loader isn't listening. Open HALL OF FAME again, or close SWRR, start it again and open HALL OF FAME. After a game has run, SWRR always needs restarting first.
- **`no PS4 or PS5 answered discovery`:** the PS5 is off, asleep or on another network. Set `PS_HOST` to its address (step 2) or pass `--console ADDRESS`.
- **`another run.py, ghostldr.py or the app is running on this computer`:** UDP 18194 or TCP 18195–18196 is taken. Stop the other one (Ctrl+C), then run the command again.
- **`no USB drive is mounted on this computer`** (copy, check): plug it in, or name its folder after the dump.
- **`… has no eboot.bin: point at the game's folder`** (copy, install): name the folder that has `eboot.bin` in it.
- **`receiver did not come up` or `is SWRR at the Luac0re loader?`** (install): the loader isn't listening. Open HALL OF FAME again, or restart SWRR first.
- **`building the core failed`:** run it from the release folder, which has the prebuilt core; `ghostldr.py` uses it.
- **The boot command times out while sending:** restart SWRR, open HALL OF FAME, and send again.
- **Black screen with no log lines on the computer:** the firewall is blocking UDP 18194, or `PS_HOST` points at another device.
- **`extension: the console did not fetch it`, or the log stops at `ext: no extension from the computer`:** the firewall is blocking TCP 18195. Nothing was started: allow it and run the same command again.
- **Any other `ext:` line that stops the boot:** the log follows it with a `[hint]` line giving the cause and the fix, for example a core and `ext.bin` from different builds.
- **`approot: no USB drive: plug one into the PS5`:** the PS5 sees no drive. Plug it into the PS5 (exFAT or FAT32), restart SWRR and boot again. (Without `--usb`, the boot then tries the installed games.)
- **`approot: … holds no games`, or `… is not among the games`:** the PS5 found no game, or not that one, on the drive. `python3 ghostldr.py check` on the computer shows what it finds there and what is wrong with any folder; the log lists every folder it couldn't start, with the reason.
- **`approot: stopped looking after 256 folders`:** the drive has many folders; keep games near its top, or boot with `--folder "My Games"`.
- **`approot: libSceUsbStorage lacks what mounting needs`:** USB drives were tested on a 13.60 PS5, and this console's firmware differs. Use [the saved-data way](#the-saved-data-way) instead.
- **Not sure where it fails:** `python3 ghostldr.py doctor` checks Python, the prebuilt files, the computer's ports, the PS5's loader and the USB drives mounted here in one go.
- **The PS5 asks for the disc:** the SWRR disc must be in the console that runs it.

## Removing it

**A game on the USB drive:** delete its folder (`ghostldr/CUSA11456`, or wherever you put it) from the drive on the computer, or:

```bash
python3 ghostldr/host/usb_drive.py remove /Volumes/MYDRIVE CUSA11456
```

Its progress, `GHOSTSAVE`, stays in SWRR's saved data.

**An installed game,** with Luac0re's loader listening:

```bash
python3 ghostldr/host/gfs_install.py --remove CUSA11456
```

This deletes Ghost's data and index and keeps its progress, `GHOSTSAVE` (add `--remove-saves` to delete that too). Other installed titles and SWRR's own save are not touched.

Or delete the entries by hand in **Settings → Saved Data and Game/App Settings → Saved Data (PS4) → Console Storage → Star Wars Racer Revenge**: `GHOST000`…`GHOST055` and `GHOSTINDEX` (or `GFSINDEX`).

Keep `GHOSTSAVE` (Ghost's progress) and the Luac0re save (`SLUS-20268` on US SWRR, `SLES-50366` on EU) unless you mean to remove those too.
