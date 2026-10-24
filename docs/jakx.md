# ghostldr in Jak X (UltraC0re)

ghostldr can use *Jak X: Combat Racing* instead of SWRR as its host: the EU PS2 classic (CUSA07992, update 1.04) with [UltraC0re](https://github.com/matem6/UltraC0re) (matem6's Jak X port of Luac0re) in place of Luac0re. The core, the extension, the selector and the title's files are the same. The bootstrap notices UltraC0re and tells the core, which then uses Jak X's offsets and UltraC0re's way into the compiler.

**Status (a 13.60 PS5):** Ghost of Tsushima v2.18 plays in Jak X, from ghostfs, in its PS4 Pro mode: menus, movies, New Game (the prologue into Komoda beach) and Continue from a save, with the controller. The longest session so far is about 25 minutes.

## What you need

- Jak X EU (CUSA07992) with update 1.04. UltraC0re's gadget offsets match 1.04 only. UltraC0re also supports the US copy (CUSA07842); ghostldr doesn't yet, as its offsets haven't been checked.
- UltraC0re 1.0.2's save (`SCUS-97429`), resigned for your account as its README says. If Jak X's executable loads at `0x400000` on your console (it did on the 13.60 test console), the bootstrap in the save's `VMC0.card` needs a two-instruction patch: `python3 ghostldr/tools/ultrac0re_patch.py` on UltraC0re's `builds/jakx_ultrac0re.bin`, then UltraC0re's `scripts/craft/craft_ultrac0re.py` to rebuild the card. Unpatched, the exploit hangs on a black screen.
- To trigger it: title screen, Load Profile, the UltraC0re profile, Adventure, then Triangle when the cutscene starts. Its dialog then says it listens on port 8888. Jak X's intro can't be skipped, so this takes about 6 minutes from launch.

## Running

The host tools find the loader by its port: Luac0re's on 9026, UltraC0re's on 8888 (`--loader-port` chooses one). Everything else is as in [using-the-release.md](using-the-release.md):

```bash
python3 ghostldr/host/run.py doctor
```

```bash
python3 ghostldr/host/gfs_install.py --title CUSA11456
```

```bash
python3 ghostldr/host/run.py boot
```

- **Installs are per host.** ghostfs keeps the title in the host game's save data, so Jak X needs its own install, and the title's progress (`GHOSTSAVE`) is separate too. Containers copied from SWRR's save data can't be mounted from Jak X (`0x809F000B`). The installer and the web manager never touch Jak X's own save, `SCUS-97429`, which holds UltraC0re.
- **Restart Jak X between big jobs.** Ghost's 53 GiB install took two Jak X sessions: the first crashed after 15 containers (about 20 minutes in), and the session that finished the install crashed when the next payload arrived. The installer resumes where it stopped, and a boot right after a fresh trigger works.
- **Errors show up in the log.** UltraC0re's loader reports a payload's errors only on the TV, so `run.py` first sends `ghostldr/lua/notify_hook.lua`, which copies its notifications and dialogs to the log (`lua: ...` lines).
- **One thing at a time.** As with Luac0re, the compiler serves one caller: don't send other scripts while a title runs.

## What differs from SWRR

| | SWRR + Luac0re | Jak X + UltraC0re |
|---|---|---|
| Compiler calls (`jitrpc.c`) | Luac0re's `jit_rop` chain, 6 arguments | UltraC0re's parked loop (`door1.lua` `jit_run`), with gadgets for rdi, rsi, rdx and rcx only: 4 arguments (no ghostldr call needs more). ghostldr's chain sits in its own scratch with 64 KiB of stack below it, because UltraC0re's `J_CALL` is only 0x100 bytes above the loop itself |
| `sceKernelDlsym` for the core | `SCE_KERNEL_DLSYM` is libkernel's function | UltraC0re's is the bare `dynlib_dlsym` system call. Called through the eboot's `call rbx` gadget, it leaves the core's own frame (JIT memory) as the caller, and the kernel refuses every lookup. The bootstrap passes the exported `sceKernelDlsym` instead |
| `call rbx; ret` gadget | eboot `+0x31aa9` | eboot `+0x19869` |
| Shared bridge scratch | Luac0re's OOB scratch; staging at `SHELLCODE_SCRATCH`, 4 MiB | The emulator uses `+0..+0x2f9000` and `+0x90f000..` of the bridge, UltraC0re `+0x500000..+0x504400`; `+0x540000..+0x90f000` stays zero. Staging is at `+0x540000` (2 MiB), and ghostldr's scratch runs to about `+0x761000` |
| Emulator threads | Luac0re cancels Runtime_IOP-SPU2 and GS | UltraC0re cancels only the bridge listener. The core cancels IOP-SPU2 (`+0x8460148`) and GS (`+0x805f378`). Neither exits, since they wait where a cancel doesn't reach, so they are suspended |
| Direct memory | 4.5 GiB, 4532 MiB free after the release | 5 GiB (Jak X runs as a PS4 Pro app), about 4982 MiB free. The GS emulation alone held 1568 MiB |
| Video output handle | eboot `+0x2d695d0` | eboot `+0x35c7180` |
| Save data import | `sceSaveDataMount` at `+0x3893F0` | `sceSaveDataMount2` at `+0xcd1e40` (dirName `+0x8`, blocks `+0x10`, mode `+0x18`) |
| System libraries | loaded by name when the title imports them | A load by name only finds a library that is already loaded, and only `sceSysmoduleLoadModule` brings new ones in. At boot `libSceHttp2` and `libSceVideodec2` miss; `libSceAudio3d`, `libSceVoiceQoS` and `libSceNpGameIntent` aren't available at all (the extension's virtual versions stand in). A library that missed is looked for again when it's next used: imports bind late (their stub retries up to 64 times, then forwards), and `host_real_in` finds it once the title has loaded it |

### Pro mode

Jak X's `param.sfo` sets the PS4 Pro bit, so its process runs in Pro mode: `sceKernelIsNeoMode` is 1, the GPU driver is `libSceGnmDriverForNeoMode.sprx` (the title's `libSceGnmDriver` imports bind to it), and the display engine reads buffers as Pro surfaces.

- VideoOut accepts only display buffers whose virtual and physical addresses are both 64 KiB aligned. A base-mode title's 1080p buffers are usually only 32 KiB aligned.
- With aligned buffers, a base-mode title shows a scrambled picture: its tiling isn't the one a Pro-mode display engine scans out.

So a title has to run in its own Pro mode there. A memory plan can name it (`memory.plan.neo` in the profile: the mode, an index into the plan's tables, that mode's fixed demand and, optionally, its own overcommit and set-table sizes, `knobs`). When the host answers Neo mode 1, the title is told so, and the plan's table edits and absorb calculation apply to that mode instead of mode 0. Plan-free titles are still told Neo mode 0.

## Ghost of Tsushima

Ghost's planner has six memory modes. Mode 0 is the base PS4: 5184 MiB in its totals table at `0x111ccf0`. Mode 2 is the PS4 Pro: 5824 MiB, most of the difference in the GPU heap (1908 MiB against 1390). Jak X leaves about 4980 MiB, so in Jak X Ghost runs in mode 2 with:

| | Mode 2 | In Jak X |
|---|---|---|
| General GPU heap | 1908 MiB | 1680 MiB (the most Ghost used was 1585: menu, prologue, Continue) |
| CPU malloc heap | 384 MiB | 320 MiB (as in SWRR) |
| Texture streaming pool | 1120 MiB | 580 MiB (what's left) |
| Told | 5824 MiB | 4924 MiB |

It renders at 4K (3840×2160 display buffers).

**The plan has to fit for real.** Three of Ghost's heaps (1946 MiB together) are backed lazily, 2 MiB at a time when first mapped. A chunk stays committed until Ghost releases the whole heap (as on a real console: direct memory keeps its contents across unmap and remap), and Continue loads the title world and then the main world. That commits almost all of them: with 470 MiB overcommitted, and then with 322, Continue ran out of direct memory (the second time with 1748 of the 1946 MiB committed). So the GPU heap gives up what Ghost never used instead. With the plan above the most committed was 1858 MiB, and 4924 MiB fits in what's free.

**The streaming pool also holds streamed packs.** With 400 MiB, the main world's mission packs (`game_missions_tsu.sprig`, `game_missions_all.sprig`) never loaded, and the load waited at `SpawnMissionWorld` for good. With 134 MiB it stopped earlier, at `LoadWorld`. At 580 MiB both New Game and Continue load. The watchdog's `mission world list` lines show what that state waits for. With `--snap` the watchdog also logs `lazy reservations committed` and the GPU heap's use; an out-of-memory line (`vdm: lazy commit ... out of direct memory`) gives both totals.

## Not done yet

- **Long play sessions:** memory can't run out while Ghost's GPU heap stays under its 1680 MiB (it sat at 1585 throughout), but nobody has played for hours yet. Other regions of the island might need more.
- **Your SWRR progress:** Jak X's `GHOSTSAVE` starts empty. Moving a save from SWRR's `GHOSTSAVE` across hasn't been tried.
- **The dev path (`--app-root /data/...`):** it needs no unjail in Jak X, since kstuff mounts `/data` into the sandbox. Unjailing breaks system library loading there.
- **The selector and the desktop app** haven't been tried with Jak X.
