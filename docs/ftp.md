# FTP server

While the game selector shows and a title runs, ghostldr serves FTP from inside SWRR on TCP 1212. It needs no jailbreak. Clients see everything SWRR can reach: its sandbox, every USB drive and, with a switch in Settings, every logged-in user's saves. The main use is the USB drive: copy titles onto it, or take files off it, over the network without unplugging it, even while a game is running from it.

## Connecting

Use any FTP client in passive mode, the usual default: FileZilla, WinSCP, Cyberduck, `curl`, and ghostldr's own `ghostldr.py copy`/`check` (`ps5` or `ftp://...`), `gfs_pack.py --usb ftp://...` and `usb_drive.py`. Python's `ftplib`, `curl` and ghostldr's tools were used to test it.

| Setting | Value |
|---|---|
| Host | the console's IP address (`run.py boot` prints it) |
| Port | 1212 |
| User and password | anything: every login is accepted |
| Mode | passive (on a PS5 at 8.00 or later, active mode isn't available) |
| Encryption | none (plain FTP) |

`run.py boot` prints the address once the boot is sent:

```
FTP: ftp://192.168.1.23:1212/ while the game selector shows and the title runs (any user and password)
```

The server starts as the title's main thread is created, so it's up shortly after the title begins to load, and it stays up until SWRR closes. The console's log shows it:

```
ftp: listening socket buffers: send 2048 KiB (had 32), receive 2048 KiB (had 64)
ftp: listening on TCP 1212 (10 clients at once, 128 KiB each)
ftp 1: 192.168.1.123 connected; starting in /
ftp 1: STOR /ghostldr_usb/ghostldr-ftp-test/d.bin: 67108864 bytes in 2466 ms (26575 KiB/s; the file took 497 ms)
```

"The file took" is the part of the transfer spent writing or reading the drive; the rest was the network.

## What you see

A client starts in `/`, SWRR's sandbox, and paths are SWRR's own:

| Path | What it is |
|---|---|
| `/ghostldr_usb` | the first USB drive, read-write |
| `/ghostldr_usb2`, `3`, `4` | the second, third and fourth drive |
| `/saves` | every logged-in user's saves, when **Saves in FTP** is on ([below](#saves)) |
| `/app0`, `/savedata0`, `/temp0`, `/dev`, ... | SWRR's own folders and a few system ones |

Every drive plugged in is mounted, up to four; one plugged in later is mounted the next time a client logs in. On a jailbroken console, what its tools put in SWRR's sandbox shows too (`/mnt/usb0`, `/data`).

Leave SWRR's own folders alone. `/app0` is SWRR itself, and `/savedata0` can be SWRR's save data, which holds Luac0re.

The drives are read-write, through the PS5's own exFAT (or FAT32) driver: upload, download, resume (REST), append, rename, delete, make and remove folders.

Before this, a client started on the first drive (`/ghostldr_usb`). Tools that name the drive in their address (`ghostldr.py copy`, `gfs_pack.py --usb ftp://PS5:1212/ghostldr_usb`, `usb_drive.py`) work as before.

## Saves

With **Settings > Advanced > Saves in FTP** on (`run.py boot --ftp-saves` from a computer), `/saves` holds the saves of every user logged in to the console:

```
/saves/<user>/<TITLE>/<save folder>/...
/saves/Alice/CUSA03474/SLUS-20268/VMC0.card     (SWRR's own: Luac0re's memory card)
/saves/Alice/CUSA07992/SCUS-97429/...           (Jak X's)
/saves/Alice/CUSA00900/SAVEDATA00/...           (Bloodborne's, run under ghostldr)
```

`<user>` is the user's name on the console (their user ID in hex when the name has characters other than letters, digits, `-`, `_` and `.`). The switch counts at once: the server reads it at each request, while the selector shows and in the game.

Download a save with any client (`curl -O ftp://PS5:1212/saves/Alice/CUSA07992/SCUS-97429/...`, or a whole folder with FileZilla). What a stock console lets a game do decides what shows:

- **SWRR's own saves** are listed and open: Luac0re's memory card, and the saves of the games ghostldr runs (`<prefix>SAVE`, `<prefix>D001SAVE`, ...; a save tool's `/data` is `<prefix>D000SAVE`). ghostldr's other containers are no saves and are left out: a game's install data and index (`GHOST000`, `GFSINDEX`, ...), `GLDRCONF`, `GLDRCACHE`; the console mounts those as a game is picked, and one held by the FTP server would stop it. While a game runs, its own saves answer `550 ... Busy`: a mount held by the FTP server would make the game's own fail.
- **The saves of games run under ghostldr** show under the game's own title ID, by the names the game gave them (`/saves/Alice/CUSA00900/SAVEDATA00`), read-only. They are SWRR's containers underneath (each game's `<prefix>SAVE/catalog.bin` names its saves and the `<prefix>D<nnn>SAVE` each is in), so they also show raw under SWRR's title. They belong to the user the game ran for (a save tool's, to the user who started SWRR). One named like a save the console has of the same game gets `~ghostldr` after its name. While a game runs, its own aren't listed.
- **Another game's saves** are listed only for title IDs something names, because the system answers only for an exact title ID (about 0.4 s each): the other titles ghostldr runs from (SWRR's other region, Jak X), save folders on a USB drive (`PS4/APOLLO/...`, the system's copies in `PS4/SAVEDATA/<account>/<TITLE>`), and what a save tool run under ghostldr kept (its save scan's answers, `ghostldr/titles.txt` and keystones in its `/data`). A path straight to any title (`cd /saves/Alice/CUSA12345`) asks the system about it, and `mkdir /saves/Alice/CUSA12345` lists it in the user's folder for the rest of the session.
- **Another game's saves are read and written the way a save tool under ghostldr does it** ([compatibility.md](compatibility.md), "Save tools"; [save-resign.md](save-resign.md)):
  - **With that game's keystone** (in `PS4/APOLLO/<folder>/sce_sys/keystone` or `ghostldr/keystones/<TITLE>.keystone` on a drive, or one a save tool kept), the save is mounted read-only with the game's fingerprint, as a game may.
  - **Without one**, it is opened through an **H-RACE copy**: ShellCore binds the save into SWRR before it checks it, and files opened in that moment are copied to `/temp0/ghostldr/ftp/race/` for reads or `/temp0/ghostldr/ftp/rw/` for changes. A fingerprint mount the system refuses (the known other-account case) stays read-only. Each H-RACE copy leaves a mount point behind until SWRR closes, so there are six a session at most; reading and then writing the same no-keystone save uses two.
  - **Writing** (upload, rename, make a folder) makes a writable copy, with a fingerprint mount when the keystone is known and the H-RACE when it is not. Three seconds after the last change, with none of its files open, the copy is written into the save through a read-write mount the system binds and then refuses (`save_race_write`, the resign without a jailbreak). The save keeps its owner and keystone. **Close SWRR afterwards**: the system commits the save when SWRR closes (the log says so, and a banner in a running game). Before writing, the save's files are kept in `<copy>.before`. A save's `sce_sys` is the system's and is never written (`550 ... Permission denied`), and saves can't be made or removed, only changed. Without the keystone, files absent from the working copy cannot safely be removed from the save, so a delete does not persist and the old side of a rename remains. A known other-account save stays read-only.
  - Native PS5 saves are out of reach.
- **SWRR's own saves are never written**, nor the other region's SWRR saves (they hold the exploit): `550 ... Read-only file system`.
- The other titles ghostldr runs from (SWRR's other region, Jak X) show their saves, not ghostldr's install data from when they ran it.

Each save is mounted only while a client uses it (a listing, a download), at most three at once, and unmounted 10 seconds after its last use (SWRR's own after 1.5 seconds, so a game picked next finds its saves free): SWRR, ghostfs and the game share the console's 16 save mounts. Listing a game's folder mounts nothing. The first listing of `/saves` asks the system about every named title in the background, so a user's folder can take a few seconds to show the first time. Changes not yet written when the server stops (back to the game selector, Close ghostldr) are written then.

## Putting a title on the drive

With a title running from anywhere (a USB drive or SWRR's save containers), from the computer:

```bash
python3 ghostldr.py copy /path/to/CUSA12345/app0 ps5
python3 ghostldr.py check ps5
```

`ps5` is `ftp://<the PS5>:1212/ghostldr_usb`; `--folder "My Games/New Game"` puts the game in any folder on the drive. It sends 8 files at once over 8 connections (`--jobs`), then checks the copy. `check` lists the games the PS5 will find on the drive. Then restart SWRR and boot the new title from the drive ([usb.md](usb.md)):

```bash
python3 ghostldr.py boot CUSA12345
```

## How fast

Measured on a stock 13.60 PS5 with Ghost of Tsushima running from the drive (a USB 3 NVMe enclosure, exFAT), over a home network with 5–20 ms round trips to the console:

| Transfer | 1 connection | 4 connections | 8 connections |
|---|---|---|---|
| Upload to the drive | 25 MiB/s | 51 MiB/s | 52 MiB/s |
| Download from the drive | 6 MiB/s | 12 MiB/s | 24 MiB/s |
| Upload of 100 KB files | 12 a second | | 77–83 a second |

- **The network sets the pace.** Writing 256 MiB to the drive took 1.4 s of a 10 s upload, and reading it 0.2 s of a download. Downloads grow with connections because each connection waits for the network round trip; a faster network does better.
- **A whole title:** PS4-Xplorer (79 files, 70 MiB) went on in 4 s with `gfs_pack.py --usb`, against 12 s through a jailbroken console's ftpsrv.
- **Big folders are slow to add files to**, whichever server is used: the console's exFAT adds files to a folder of 20,000 entries at about 10 a second ([usb.md](usb.md#copying-games-onto-the-drive)).

## Options

- `run.py boot --ftp-port PORT` serves on another port (1–65535). The default, 1212, stays clear of the ports console tools usually take: a jailbroken console's ftpsrv has 2121. If the port is taken anyway, the log says so and the title runs without FTP; pick another.
- `run.py boot --ftp-saves` adds `/saves`, as **Saves in FTP** in Settings does ([Saves](#saves)).
- `run.py boot --ftp-port 0` turns the server off. Then ghostldr doesn't open the USB storage session for it either, and a title's own `sceUsbStorageInit` and `Term` reach the library as before.

## Security

There's no password and no encryption: while the selector shows or a title runs, anyone on your network can read and write the USB drive and SWRR's sandbox, and with **Saves in FTP** on, read every logged-in user's saves and change those of games whose keystone is known. Use it on a network you trust, or turn it off with `--ftp-port 0`.

## Limits

- **Ten clients at once.** An eleventh gets `421` until one leaves. That's room for `gfs_pack.py --usb`'s eight connections and a client to browse with.
- **Binary transfers only.** `TYPE A` is accepted and ignored: files are sent as they are.
- **No permissions or times to set.** `SITE CHMOD` is accepted and ignored (exFAT keeps none), and `MFMT` isn't implemented. Uploaded files get the time they were written.
- **Passive mode only on a PS5 at 8.00 or later.** SWRR can't create a socket there once the title runs (below), and active mode (`PORT`, `EPRT`) needs a new one for each transfer. The server accepts `PORT` and then answers `425 ... use passive mode`.
- **IPv4 only.** `EPSV` and `EPRT` work for IPv4.
- **Data connections only with the client.** A `PORT` or `EPRT` to another address is refused, and a passive connection from another address is closed.
- **One title at a time.** The server runs with the title; booting another means restarting SWRR, and the server comes back with the next title.

## How it works

`ghostldr/native/ext/src/ftp.c` is the protocol: RFC 959 plus FEAT and OPTS (RFC 2389), EPSV and EPRT (RFC 2428), and SIZE, MDTM, REST STREAM, MLST and MLSD (RFC 3659). It's plain freestanding C over a small environment (`ext/include/ftp.h`). `ext/src/ftp_sys.c` provides that environment on the console: libkernel's BSD sockets (the calls the Luac0re emulator payloads' FTP servers use in SWRR), `sceKernel*` file calls, and threads.

- **Sockets.** On a PS5 at 8.00 or later (a PS4 at 14.00 or later), SWRR's own `socket()` answers ENOSYS for an internet socket. As Luac0re does for its loader, the JIT compiler process makes the socket and hands it to SWRR (`jit_recv_fd`); `bind`, `listen`, `accept` and the transfers then work in SWRR. That channel belongs to the boot thread, so every socket the server listens on is made at start: the control port and one passive data port for each of the ten client slots. Passive mode answers with the slot's own port, and connections left waiting from an unused `PASV` are dropped at the next one.
- **Buffers.** SWRR's sockets start with 32 KiB to send and 64 KiB to receive, which held one connection to a few MiB/s. Every listening and accepted socket gets 2 MiB of each; the listening ones get it before `listen`, so the connections they accept offer a large enough window.
- **Start.** When the game selector is about to show, the core starts the server from the extension's early entry (`struct ext_early.ftp_start`, `boot.c`), on the boot thread, which still has the compiler's channel for the sockets. When the selector is beside the ELF loader (the release's first call), the port is 1212, the release's: those flags' port bits hold the ELF loader's. Otherwise `run.py --ftp-port` reaches the console in bits 48–63 of the boot context's flags.
- **Through to the title.** Once the extension's code is placed it stays where it is when the title loads, so the server goes on serving: the `title_starting` hook, just before the title's main thread, finds it running and leaves it. A boot without the selector starts it there, as before.
- **Stop.** Before the core gives the extension's code back (Close ghostldr, the selector beside the ELF loader handing over to the title's call, a boot that stops before its title), `ftp_stop` shuts every client's control and data connections, waits for their threads and the listener's (at most 15 s), and frees the slots. A thread that doesn't end in time keeps the code where it is, so nothing runs in freed memory. The title's call starts the server again.
- **The drives.** Before the title's code runs, `approot.c` opens the session to the system's AutoMounter and mounts the plugged-in drives at `/ghostldr_usb`, `/ghostldr_usb2`, ..., as it does for a title on a drive ([usb.md](usb.md#how-the-console-reaches-the-drive)). The title's own `sceUsbStorageInit` and `Term` are then kept off that session (`hle_usbstorage.c`). When a client logs in, a drive that has gone away is forgotten and a newly plugged-in one is mounted (the drive a title runs from is mounted again by `usb_live.c` instead: [usb.md](usb.md#pulling-the-drive-out-rest-mode)).
- **Rest mode.** After rest mode the listening socket is checked (`poll`, `SO_ERROR`, `getsockname`); one that stopped working is made again on the same port, with new passive ports for the idle slots, from the JIT compiler as at the start (`ftp_relisten`). Clients connected before rest mode reconnect.
- **Saves.** `ext/src/ftp_saves.c` answers for `/saves` (`ftp_sys.c` sends it every path under it while `CTX_FLAG_FTP_SAVES` is set). The users come from `sceUserServiceGetLoginUserIdList`, their saves from the real `sceSaveDataDirNameSearch` (one at a time with every other caller in the extension: eight at once froze SWRR on a 13.60 PS5). SWRR's own saves mount through the core's `gfs_mount_user`, another title's through the real `sceSaveDataMount` with its fingerprint (bytes 0x20-0x3f of its keystone, in hex), read-only, each taking one of ghostfs's spare mount slots (`gfs_slot_reserve`); without a fingerprint, or refused, through `save_race_copy`. A write uses `save_fp_copy` with the fingerprint or `save_race_rw_copy` without one, then `save_race_write` (`save_race.c`, as `hle_fsvsh.c` does for save tools). A file or directory a client opened in a save keeps its view until it is closed; the listener's tick unmounts idle mounts and writes changed copies. Before the extension's code goes (`ftp_early_stop`), the background scan is stopped, changed copies are written and every save unmounted.
- **Memory.** Ten client slots, each with a 128 KiB transfer buffer, about 1.4 MiB in all, are allocated once at start, before the title's memory is planned (or as the title starts, without the selector); memory taken later would come out of what it was promised. Each client gets a thread with a 64 KiB stack.
- **Speed** comes from connections: each client has its own thread on the console, so `gfs_pack.py --usb` sends eight files at once (`--jobs`). Most of a dump is small files, and each costs a few round trips.
- **Uploads** are written in 128 KiB pieces, which a USB drive takes much faster than the network's small ones.
- **SIGPIPE** is ignored for the whole process and every socket has `SO_NOSIGPIPE`: a client that drops its connection mustn't take SWRR with it.

## Tests

`ghostldr/tests/test_ftp.py` builds `ftp.c` for the computer against `tests/ext_native/ftp_host.c` (the same environment over POSIX and a temporary folder) and drives it with Python's `ftplib` and raw sockets:

- logins, directories and paths that try to climb above `/`;
- transfers from empty to 48 MiB, compared byte for byte, with REST and APPE;
- SIZE, MDTM, MLST, MLSD, LIST and NLST, including a listing larger than a transfer buffer and Unicode names;
- passive, extended-passive and active data connections, and connections left over from an unused `PASV`;
- error replies, Telnet bytes before ABOR, over-long lines;
- ten clients at once and an eleventh turned away;
- a client that drops its connection mid-upload;
- `usb_drive.py` putting a title on, checking and removing it, asking for more connections than the server takes (it carries on with what it gets).

`test_ftp_saves.py` builds `ftp_saves.c` against a fake console (`tests/ext_native/ftp_saves_os.c`: the save and user services over a temporary folder) and checks the users, titles and saves listed (ghostldr's install data and settings left out), reading SWRR's and another title's saves, race copies for a save without a keystone and another account's (once each, six a session), writing with and without a keystone through a writable copy (written into the save once, after the last change; `sce_sys` refused; an unchanged copy not written; changes left at the stop written), SWRR's own saves never written, a path or folder named after a title, idle saves unmounted while held ones stay, no more than three mounts, the running game's containers refused, the titles and keystones a save tool's `/data` names, and the saves of a game run under ghostldr listed under its title ID from its catalogue (read-only; a name clash; not while it runs).

`test_usb.py` covers the drive side: the mount for the FTP server on a boot from elsewhere, a drive plugged in later, a drive pulled out, and no session at all with `--ftp-port 0`.

```bash
python3 -m unittest discover -s ghostldr/tests -p test_ftp.py
```
