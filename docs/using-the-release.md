# Using the prebuilt release

This walks through running Ghost of Tsushima from a ghostldr release on a stock PS5: no jailbreak, no compiler, no build step. It covers download to gameplay. [ghost-on-stock-ps5.md](ghost-on-stock-ps5.md) has the background and more troubleshooting.

## What you need

- **A PS5 with *Star Wars Racer Revenge*, USA (CUSA03474) or Europe (CUSA03492).** Luac0re 2.4b must already work on it: its Remote Lua Loader starts when you open HALL OF FAME. Both regions work the same way; use the same one for the install and for playing.
- **Your own Ghost of Tsushima v2.18 dump (CUSA11456)** as a folder of files (`eboot.bin`, `sce_module/`, `cache/`, `movies/` ...), about 63 GB. The release contains no game files.
- **About 60 GB free on the PS5** for the game data.
- **A computer on the same network as the PS5** with Python 3.11 or newer.
  - macOS and Linux work; Windows is untested.
  - A wired or fast Wi-Fi connection helps.
- **The computer's firewall must let Python receive** on UDP port 18194 (the game's log) and TCP port 18195 (the loader fetches its extension from the computer). On macOS, click Allow when asked.

## 1. Download and unpack

From the release page, download `ghostldr-v0.2.0.zip` and `SHA256SUMS.txt`. You only need the dev payload zip on a jailbroken console. Check the download:

```bash
shasum -a 256 --ignore-missing -c SHA256SUMS.txt
```

Unpack it and work from inside the folder; every command below runs there:

```bash
unzip ghostldr-v0.2.0.zip
```

```bash
cd ghostldr-v0.2.0
```

## 2. Put the game files in place

The tools expect the dump at `build/ghost/app0`. Either move or copy it there, or link it:

```bash
mkdir -p build/ghost
```

```bash
ln -s /path/to/your/CUSA11456/app0 build/ghost/app0
```

Check it: `ls build/ghost/app0` should list `eboot.bin`, `sce_module`, `cache` and `movies`.

## 3. Prepare the PS5

1. **Rest mode:** stop the PS5 from sleeping during the install. Go to **Settings → System → Power Saving → Set Time Until PS5 Enters Rest Mode** and pick **Don't Put in Rest Mode** (or a long time) while playing media.
2. **Start Luac0re:** start SWRR, click the right half of the touchpad at PRESS START, then go to **OPTIONS → HALL OF FAME**. Wait until the loader says it is listening on port 9026.
   - If it says the exploit failed, close SWRR and try again.
   - Never save from SWRR's own menus, and never confirm a "corrupted, will be deleted" prompt.
3. **Find the PS5's address:** **Settings → Network → Connection Status → View Connection Status → IPv4 Address**. Then on the computer:

   ```bash
   export PS5_HOST=192.168.1.123
   ```

   With a single PS5 on the network you can skip this; the tools find it themselves. `python3 ghostldr/host/console.py --list` shows what they see.

## 4. Pack the game data (on the computer, a few minutes)

```bash
python3 ghostldr/host/gfs_pack.py
```

This lays the game out for SWRR's save containers in `build/ghostfs/CUSA11456/`. Nothing is sent yet. It should report 40,100 files, 53.3 GiB, in 56 containers.

## 5. Install onto the PS5 (once, about 65–100 minutes)

With SWRR at the Luac0re loader:

```bash
python3 ghostldr/host/gfs_install.py
```

- **Progress:** it prints a line per container (`[12/56] GHOST011: 992 MiB in 75 s ...`) and finishes with `install complete`.
- **Check first:** `--dry-run` shows what is installed and what would change, without sending anything.
- **If it stops** (network, PS5 asleep, SWRR closed): run the same command again. It keeps what was already sent. If SWRR closed, start Luac0re again first.
- **`no space left on the console's storage`:** free space on the PS5 and run it again.
- **A slow default network link** on the computer: `--interface en0` sends through a chosen interface (macOS).

The data lands in SWRR's saved data under the signed-in user, as about 57 entries named `GHOST000`…`GHOST055` and `GFSINDEX`. It belongs to the SWRR region you installed from: US and EU SWRR each keep their own saved data, so install from the one you will play in.

## 6. Play

1. **Restart SWRR after the install.** The loader can be left busy afterwards. Close SWRR (PS button → SWRR's card → Options → Close Game), start it again and open HALL OF FAME.
2. Send the game:

   ```bash
   python3 ghostldr/host/run.py boot --app-root ghostfs --no-build
   ```

The first boot takes a minute or two. On a new save Ghost goes through brightness, audio output, difficulty and experience (keep Standard: only English audio is installed), then the opening. Its saves go into a separate entry, `GHOSTSAVE`, and SWRR's own save is never touched.

The computer logs the session to `build/ghostldr/run.log`. That file is what to look at, or share, if something goes wrong.

**Every later session:** close SWRR, start it again, open HALL OF FAME, and run the same `run.py` command. The install is only needed once per PSN user.

## Troubleshooting

- **`receiver did not come up` or `is SWRR at the Luac0re loader?`:** the loader isn't listening. Open HALL OF FAME again, or restart SWRR first.
- **`building the core failed`:** add `--no-build`. The release's core is already built.
- **The boot command times out while sending:** restart SWRR, open HALL OF FAME, and send again.
- **Black screen with no log lines on the computer:** the firewall is blocking UDP 18194, or `PS5_HOST` points at another device.
- **`extension: the console did not fetch it`, or the log stops at `ext: no extension from the computer`:** the firewall is blocking TCP 18195. Nothing was started: allow it and run the same command again.
- **The PS5 asks for the disc:** the SWRR disc must be in the console that runs it.

## Removing it

Delete Ghost's data from **Settings → Saved Data and Game/App Settings → Saved Data (PS4) → Console Storage → Star Wars Racer Revenge**. The entries are `GHOST000`…`GHOST055` and `GFSINDEX`.

Keep `GHOSTSAVE` (Ghost's progress) and the Luac0re save (`SLUS-20268` on US SWRR, `SLES-50366` on EU) unless you mean to remove those too.
