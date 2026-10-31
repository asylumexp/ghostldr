# The autoloader

With the autoloader, **OPTIONS → HALL OF FAME opens ghostldr's game selector by itself**. No computer, no `nc` and no payload-sender app are needed after the first setup. It is a Luac0re save with three more files in it:

| In the save | What it is |
|---|---|
| `lua/auto.lua` | Luac0re 2.4b runs `/savedata0/lua/auto.lua`, when its save has one, instead of its loader on port 9026 ([`ghostldr/lua/auto.lua`](../ghostldr/lua/auto.lua)) |
| `ghostldr/ghostldr.lua` | the release's `ghostldr.lua` |
| `ghostldr/ghostldr-ext.bin` | the release's `ghostldr-ext.bin` |

Everything else is Luac0re 2.4b's own save (`VMC0.card` with the PS2 exploit save, `VMC1.card`, `lua/`, `sce_sys/`), which the autoloader needs as before.

## The files on the release page

One zip per SWRR region, each with the save twice, laid out for the root of a USB drive, and a `README.txt` saying which is which:

| File | In it |
|---|---|
| `ghostldr-<version>-save-US-CUSA03474.zip` | `PS4/APOLLO/00000000_CUSA03474_SLUS-20268/`: the save **decrypted**, for Apollo Save Tool. `PS4/SAVEDATA/0000000000000000/CUSA03474/SLUS-20268` and `SLUS-20268.bin`: the save **encrypted**, as the console's own USB copy is, for Save Wizard |
| `ghostldr-<version>-save-EU-CUSA03492.zip` | the same for SWRR EU (`SLES-50366`) |

There is no save for Jak X (UltraC0re): the autoloader is for SWRR and Luac0re only.

Neither save belongs to an account. The decrypted one has `ACCOUNT_ID` 0 and no user ID or console in its `param.sfo`. The encrypted one is made out to account 0. So each one has to be **resigned to your account** on the way in, as Luac0re's own save does. The save is 25 MiB (800 blocks). Luac0re's original is 18.3 MiB (586 blocks); `ghostldr-ext.bin` needs the extra room.

## Putting it on the console

The save replaces SWRR's save (Luac0re's) on that account. Copy your current one off first if you want to keep it.

### A jailbroken PS4: Apollo Save Tool

1. Unzip the region's zip onto the root of a USB drive (FAT32 or exFAT), so that `PS4/APOLLO/00000000_CUSA0347x_.../` is on it, and plug it into the PS4.
2. Delete SWRR's Luac0re save in Apollo: **HDD Saves**, the SWRR save (`SLES-50366` or `SLUS-20268`), **Delete**. Use Apollo rather than the PS4's Settings: there every save SWRR holds, ghostldr's containers included, is listed as SWRR's "Saved Data". Apollo then makes a new save of the size the autoloader's needs.
3. In Apollo: **USB Saves**, pick the ghostldr save, **Copy save game to HDD**. Apollo writes your account into it as it copies: no resign is needed.

Tested on a 12.50 PS4 with GoldHEN and Apollo 2.4.0 (EU): the 800-block save was made, and HALL OF FAME opened the game selector. Apollo also lists saves in `/data/fakeusb/PS4/APOLLO/` (the console's own storage) as USB saves, for a PS4 without a drive.

### A PS5 (no jailbreak)

A PS5 imports only an encrypted save that is made out to its own account. There are two ways to get one:

- **Save Wizard:** resign the encrypted save in the region's zip (`PS4/SAVEDATA/0000000000000000/<title>/`) to your PSN profile. Then copy it to a USB drive as `PS4/SAVEDATA/<your account ID, 16 hex>/<title>/` and import it: **Settings → Saved Data and Game/App Settings → Saved Data (PS4) → Copy or Delete from USB Drive**.
- **A jailbroken PS4** on a firmware no newer than the PS5's. Follow [remote_lua_loader's PS5 steps](https://github.com/shahrilnet/remote_lua_loader/blob/main/SETUP.md), with this save in place of the one they make:
  1. Fake-activate a PS4 user with your PS5 account's ID (Apollo → User Tools → Activate PS4 Accounts).
  2. Import the decrypted save as above.
  3. Copy it to USB with the PS4's own **Copy to USB Storage Device**.
  4. Import it on the PS5. This works for US and EU.

## Using it

1. Start SWRR, click the right half of the touchpad at PRESS START, then **OPTIONS → HALL OF FAME**.
2. **The game selector opens** after Luac0re's exploit. On a PS5 the [ELF loader](elfldr.md) listens beside it on 9021 (9022 on a jailbroken console) until a game is picked, as in [Every later session](using-the-release.md#every-later-session-ghostldrlua-alone).
   - The first time, ghostldr copies the save's `ghostldr-ext.bin` into its cache (`GLDRCACHE`, 16 MiB). Later boots read the cache.
   - A bundle cached before, from a computer, is used instead when it is this release's.

- **Luac0re's loader instead:** hold **L1 + R1** from choosing HALL OF FAME until Luac0re's loader shows (the autoloader reads the controller for up to 3 seconds after the exploit, so a boot without them waits that long). You get Luac0re's own loader on 9026, as with no autoloader: send it any script, `ghostldr.lua` included.
- **After ghostldr:** however it ends (**Settings → Close ghostldr**, a failure shown on the TV), Luac0re's loader listens on 9026 after it.
- **A newer ghostldr:** import the new release's save. The old `ghostldr.lua` in the save would replace a newer bundle in the cache with its own at the next Hall of Fame. To try a newer release without a new save, hold L1 + R1 and send its two files to the loader, as in [using-the-release.md](using-the-release.md#4-boot).
- **Something wrong with the save's files:** they come from one release, and ghostldr checks them as it checks a sent bundle. If `ghostldr-ext.bin` is another release's, damaged or missing, the TV says so above the usual prompt, and a `ghostldr-ext.bin` sent to 9021 boots instead. With no `ghostldr.lua` in the save, Luac0re's loader starts at once.
- **Removing it:** import Luac0re's own save again. Or, on a jailbroken console, delete `lua/auto.lua` from the save.

## How it works

Luac0re's `main.lua` checks for `/savedata0/lua/auto.lua` after its JIT exploit. When the file is there, Luac0re runs it in place of `remote_lua_loader(9026)`. `auto.lua` then:

1. Reads the first user's controller (`scePadReadState`) for up to 3 seconds: its states reach SWRR late once the PS2 game stops reading them (measured on a PS5: 2.4 s). With L1 and R1 held, it starts Luac0re's loader and stops.
2. Loads `/savedata0/ghostldr/ghostldr.lua` and runs it with `GHOSTLDR_BUNDLE_FILE = "/savedata0/ghostldr/ghostldr-ext.bin"`.
3. Starts Luac0re's loader on 9026 once ghostldr returns or fails.

`ghostldr.lua` reads the cache first, as always. With nothing of its build in the cache, it reads `GHOSTLDR_BUNDLE_FILE` the way it reads the cache: the header against its pins, the size, and the selector's CRC-32 (the core checks the extension's CRC-32 and SHA-256). Then it caches that bundle and goes on as with a bundle just received. Without the autoloader, `GHOSTLDR_BUNDLE_FILE` is unset, and `ghostldr.lua` asks for the bundle on 9021 as before.

## Making the saves

For maintainers. Build ghostldr first ([Building from source](../README.md#building-from-source)):

```bash
tools/fetch_deps.sh
make -C ghostldr/native
```

### The steps

1. **The decrypted saves.** You need a decrypted Luac0re 2.4b save: a folder holding `VMC0.card`, `VMC1.card`, `lua/` and `sce_sys/`, or a zip with one. Luac0re's release's savedata is one, and so is Apollo's **Copy save game to USB** or **Export save game to Zip** of a Luac0re save. For the other region you also need that game's `sce_sys/keystone`: a save carries its game's keystone, and a game reports a save with another game's keystone as corrupted. The save's `VMC0.card` must already hold that region's PS2 exploit save (`BASLUS-20268` or `BESLES-50366`); Luac0re's has both.

   ```bash
   python3 tools/make_save.py --from LUAC0RE_SAVE --keystone-us CUSA03474.keystone --version v1.0.0 --out build/saves
   ```

   This writes `ghostldr-v1.0.0-save-US-CUSA03474.zip` and `...-EU-CUSA03492.zip` with the decrypted saves alone. `--region us` or `--region eu` makes one region only, and `--keystone-eu FILE` is for an EU save made from a US one.

2. **The encrypted saves**, one per region, on a console (below): `resign_save.py lua copy --anyone --save` with that region's zip from step 1, then `resign_save.py usb --anyone`.

3. **The release**, with both kinds in each region's zip:

   ```bash
   python3 tools/make_release.py --version v1.0.0 --save LUAC0RE_SAVE --keystone-us CUSA03474.keystone \
       --encrypted-us build/resigned/us-CUSA03474-SLUS-20268.zip --encrypted-eu build/resigned/eu-CUSA03492-SLES-50366.zip
   ```

   It writes the two zips beside the release's files and puts them in its `SHA256SUMS.txt`; the flat zip stays the five files ([host-tooling.md](host-tooling.md#making-a-release)). An encrypted save must be the region's and no one's (`PS4/SAVEDATA/0000000000000000/...`), or it is refused; without `--encrypted-us`/`--encrypted-eu` that region's zip holds the decrypted save alone, and the tool says so. Every file but the encrypted image is checked for anything of the computer it runs on (`--deny`, `--deny-file`).

[`tools/make_save.py`](../tools/make_save.py) builds the decrypted save:

- adds `ghostldr/lua/auto.lua` and `build/ghostldr/{ghostldr.lua,ghostldr-ext.bin}`;
- zeroes `ACCOUNT_ID` and, in `PARAMS`, the user ID, the console's PSID HMAC and the copy times;
- sets `SAVEDATA_BLOCKS` to the files' blocks plus 128, rounded up to 32;
- names the save;
- for the other region, relabels `TITLE_ID`, `SAVEDATA_DIRECTORY`, `PARAMS`' title IDs and `disc-swap-cli.conf`, and puts in the keystone.

It refuses a save whose Luac0re doesn't run `auto.lua`, one that isn't SWRR's, and a `VMC0.card` without the region's exploit save. Tests: `ghostldr/tests/test_make_save.py`.

### The encrypted save

Only a console can encrypt a save image. [`ghostldr/host/resign_save.py`](../ghostldr/host/resign_save.py) has the console's own save service make it, so it needs a **jailbroken** PS5 or PS4 with an FTP server as root on 2121 and SWRR of the save's region at Luac0re's loader. For EU:

```bash
python3 ghostldr/host/resign_save.py lua copy --anyone --save build/saves/ghostldr-v1.0.0-save-EU-CUSA03492.zip
python3 ghostldr/host/resign_save.py usb --anyone --name eu
python3 ghostldr/host/resign_save.py lua delete
```

The first command does the following:

- Creates a save container `SLES-RESGN` (`SLUS-RESGN` on US), of the zip's size, in SWRR's saved data.
- Copies Luac0re's save into it, then writes the zip's files over that copy through FTP while the container stays mounted.
- Writes the copy's `param.sfo` last: `ACCOUNT_ID` 0, no `PARAMS` user ID or PSID HMAC, and `SAVEDATA_DIRECTORY` the game's own.
- Commits it by unmounting it.

The second fetches the image and its sealed key and zips them as `build/resigned/eu-CUSA03492-SLES-50366.zip` (`PS4/SAVEDATA/0000000000000000/CUSA03492/`): that zip is `--encrypted-eu`. The third removes the container from the console. US is the same with US SWRR running and the US zip.

With `--account ID` instead of `--anyone`, the copy is made out to one PSN account (decimal, or hex with `0x`): that account's PS5 can import it directly.
