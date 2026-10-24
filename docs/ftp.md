# FTP server

While a title runs, ghostldr serves FTP from inside SWRR on TCP 1212. It needs no jailbreak. The main use is the USB drive: copy titles onto it, or take files off it, over the network without unplugging it, even while a game is running from it.

## Connecting

Use any FTP client in passive mode, the usual default: FileZilla, WinSCP, Cyberduck, `curl`, and ghostldr's own `gfs_pack.py --usb ftp://...` and `usb_drive.py`. Python's `ftplib`, `curl` and ghostldr's tools were used to test it.

| Setting | Value |
|---|---|
| Host | the console's IP address (`run.py boot` prints it) |
| Port | 1212 |
| User and password | anything: every login is accepted |
| Mode | passive (on a PS5 at 8.00 or later, active mode isn't available) |
| Encryption | none (plain FTP) |

`run.py boot` prints the address once the boot is sent:

```
FTP: ftp://192.168.1.50:1212/ once the title starts (any user and password)
```

The server starts as the title's main thread is created, so it's up shortly after the title begins to load, and it stays up until SWRR closes. The console's log shows it:

```
ftp: listening socket buffers: send 2048 KiB (had 32), receive 2048 KiB (had 64)
ftp: listening on TCP 1212 (10 clients at once, 128 KiB each)
ftp 1: 192.168.1.20 connected; starting in /ghostldr_usb
ftp 1: STOR /ghostldr_usb/ghostldr-ftp-test/d.bin: 67108864 bytes in 2466 ms (26575 KiB/s; the file took 497 ms)
```

"The file took" is the part of the transfer spent writing or reading the drive; the rest was the network.

## What you see

Paths are SWRR's own. The USB drive is at `/ghostldr_usb`, and a client starts there when a drive is plugged in. Without one, it starts in `/`, SWRR's sandbox: `app0`, `savedata0`, `temp0`, `dev` and a few system folders. A drive plugged in after the title started is mounted the next time a client logs in.

Leave SWRR's own folders alone. `/app0` is SWRR itself, and `/savedata0` can be SWRR's save data, which holds Luac0re.

The drive is read-write, through the PS5's own exFAT (or FAT32) driver: upload, download, resume (REST), append, rename, delete, make and remove folders.

## Putting a title on the drive

With a title running from anywhere (a USB drive or SWRR's save containers), from the computer:

```bash
python3 ghostldr/host/gfs_pack.py --src /path/to/CUSA12345/app0 --usb ftp://CONSOLE:1212/ghostldr_usb
python3 ghostldr/host/usb_drive.py list ftp://CONSOLE:1212/ghostldr_usb
```

It sends 8 files at once over 8 connections (`--jobs`). Then restart SWRR and boot the new title from the drive ([usb.md](usb.md)):

```bash
python3 ghostldr/host/run.py boot --app-root usb:/ghostldr --title CUSA12345
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
- **Big folders are slow to add files to**, whichever server is used: the console's exFAT adds files to a folder of 20,000 entries at about 10 a second ([usb.md](usb.md#putting-titles-on-a-drive)).

## Options

- `run.py boot --ftp-port PORT` serves on another port (1–65535). The default, 1212, stays clear of the ports console tools usually take: a jailbroken console's ftpsrv has 2121. If the port is taken anyway, the log says so and the title runs without FTP; pick another.
- `run.py boot --ftp-port 0` turns the server off. Then ghostldr doesn't open the USB storage session for it either, and a title's own `sceUsbStorageInit` and `Term` reach the library as before.

## Security

There's no password and no encryption: while a title runs, anyone on your network can read and write the USB drive and SWRR's sandbox. Use it on a network you trust, or turn it off with `--ftp-port 0`.

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
- **Start.** The core calls the extension's `title_starting` hook just before it creates the title's main thread (`boot.c`). By then the boot can't stop short, so a failed boot never leaves a server thread running in freed memory, and the boot thread still has the compiler's channel for the sockets. `run.py --ftp-port` reaches the console in bits 48–63 of the boot context's flags.
- **The drive.** Before the title's code runs, `approot.c` opens the session to the system's AutoMounter and mounts a plugged-in drive at `/ghostldr_usb`, as it does for a title on the drive ([usb.md](usb.md#how-the-console-reaches-the-drive)). The title's own `sceUsbStorageInit` and `Term` are then kept off that session (`hle_usbstorage.c`). When a client logs in, a drive that has gone away is forgotten and a newly plugged-in one is mounted.
- **Memory.** Ten client slots, each with a 128 KiB transfer buffer, about 1.4 MiB in all, are allocated once at start. The title is already running then, and memory taken later would come out of what it was promised. Each client gets a thread with a 64 KiB stack.
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

`test_usb.py` covers the drive side: the mount for the FTP server on a boot from elsewhere, a drive plugged in later, a drive pulled out, and no session at all with `--ftp-port 0`.

```bash
python3 -m unittest discover -s ghostldr/tests -p test_ftp.py
```
