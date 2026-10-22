# Bringing up a new title

ghostldr loads PS4 titles, games and homebrew alike, inside SWRR. **Ghost of Tsushima (CUSA11456 v2.18) is the only title tested so far.** Nothing in the loader is tied to it any more: everything Ghost needs that other titles don't lives in its profile, `ghostldr/data/titles/CUSA11456.json`, and only applies to that build of its executable. Another title will boot as far as its imports and memory needs allow. This page is the loop for finding out how far that is and for closing the gaps.

1. [Pack](#1-pack) the dump, with or without an open-order file.
2. [Install](#2-install) it next to whatever is installed already.
3. [Boot](#3-boot) it and [read the log](#4-read-the-log).
4. [Write a profile](#5-write-a-profile): memory, save size, app parameters, [HLE rules](#6-hle-rules) for single imports.
5. Write C for what rules can't express, in the [runtime extension](#7-c-level-hle).
6. [Capture an open order](#8-capture-an-open-order) once it plays, so the right containers stay mounted.

## 1. Pack

```bash
python3 ghostldr/host/gfs_pack.py --src /path/to/CUSA12345/app0
```

- **Title ID:** from `--title`, else `sce_sys/param.sfo`'s `TITLE_ID`, else the profile whose eboot fingerprint matches the dump. Homebrew often has no `param.sfo` or an odd one: pass `--title` (1–15 of `A-Z 0-9 _ -`).
- **Prefix:** the profile's `pack.prefix`, else `G` and the ID's digits (`G12345`); `--prefix` picks another. Its rules are in [ghostfs-catalogue.md](ghostfs-catalogue.md). It also names the title's progress, so don't change it once played.
- **Order:** only about 12 containers (12 GiB) stay mounted, so files used together should be packed together. Without an order file the generic order is: `eboot.bin`, `sce_module/`, `sce_sys/`, files up to 64 KiB (fewer if they'd pass 256 MiB together), then the rest directory by directory. With one (`--order FILE` or the profile's `pack.order`), the boot files come first, then the files in the order the title opened them, then the rest.
- **Output:** `build/ghostfs/<TITLE>/layout.json` and `index.bin`. Nothing is sent yet. A missing `sce_module/` is fine.

## 2. Install

```bash
python3 ghostldr/host/gfs_install.py --title CUSA12345
```

- Titles live side by side: installing one never deletes another. Before sending, the installer checks the console's free space and stops with a message if the title doesn't fit.
- `--list` shows what is installed; `--remove CUSA12345` deletes one title's data and index and keeps its progress (`--remove-saves` deletes that too). `--dry-run` shows what would change.
- Interrupted installs resume; an unfinished title never boots. Details: [ghostfs-catalogue.md](ghostfs-catalogue.md).

## 3. Boot

```bash
python3 ghostldr/host/run.py boot --app-root ghostfs --title CUSA12345 --snap
```

- `--title` picks among the installed titles; without it the only installed one boots, or the core logs the list and stops.
- run.py sends every profile it knows with the core; the core uses the title's, or the generic default.
- With a dev setup (`--app-root /data/.../app0`), the title is the one asked for, else the one whose eboot fingerprint matches a profile.
- `--snap` starts the watchdog (every title thread's location every 20 s): that is what shows where a hang sits.

### Memory without a plan

Unless its profile has a plan, a title runs **plan-free** (`budget.c`):

- It is told one stable direct-memory size, computed once its modules are loaded: the free memory, minus 64 MiB for ghostldr, minus 100 MiB if it imports a video decoder (`libSceVideodec*`, `libSceAvPlayer`), minus its flexible-memory budget. `sceKernelAvailableDirectMemorySize` offers it as one contiguous run; ghostldr scatters what no single physical hole holds.
- The flexible budget is the title's own (`mem_param.flexible_memory_size` in its proc param), else 448 MiB. Flexible memory is backed by direct memory; `MapNamedSystemFlexibleMemory` from the title is ordinary flexible memory too, never the console-wide system pool.
- Running out gives the kernel's errors: `EAGAIN` (0x80020023) past the direct size, `ENOMEM` (0x8002000c) past the flexible budget. The first of each is logged.
- `sceKernelIsNeoMode` is 0: titles pick their base-PS4 plan.
- With SWRR's ~4,530 MiB free that comes to about 4,020 MiB of direct memory for a title with the default 448 MiB of flexible memory and no video, 3,920 MiB with video. A base PS4 gives 4,608–4,736 MiB. Titles that need more need a memory plan (below).
- If the size comes out below the profile's `min_mib` (1024 by default), the boot stops before the title starts.
- **Homebrew without its own `libc.prx`** gets SWRR's malloc heap, which has only about 38 MiB to grow into. The log says so (`no bundled libc.prx`). Rerouting that malloc to a ghostldr heap is not done yet.

The log line to look for:

```
budget: plan-free: 4532 MiB free in 6 holes (largest 3292) - 64 ghostldr - 100 video - 448 flex = 3920 MiB direct memory, one run; Neo mode 0
```

## 4. Read the log

Everything lands in `build/ghostldr/run.log`.

| Line | Meaning |
|---|---|
| `ghostfs: installed: ...`, `ghostfs: CUSA12345, N entries ...` | The catalogue, and the title being served as `/app0`. |
| `profile: CUSA12345 own profile, eboot <fingerprint>: memory plan-free ...` | Which profile applies, and what it turns on. `generic` means the default. |
| `unresolved: eboot.bin needs libSceFoo:<NID> (sceFooBar) from module libSceFoo` | At load: an import nothing implements. Its calls go to a logging stub. |
| `UNRESOLVED call: libSceFoo <NID> (sceFooBar)` | At run time: the title called it. The stub returned 0. **These are the work list.** |
| `DENIED call: ...` | With the runtime extension: a library that must not reach SWRR's own (its policy) and has no implementation for this function. |
| `budget: ...` | The memory budget, and the first time the title runs out. |
| `CRASH sig 11 fault addr ... rip ... = eboot.bin+0x...` | A crash, with registers and a backtrace as module+offset. |
| `snap ...` / `watchdog: ...` | Thread locations every 20 s (`--snap`): a title that stops making progress shows where. |
| `ghost1: ...` / `ghost2: ...` | The title's own stdout/stderr (printf, asserts). |

For each `UNRESOLVED call`, decide:

- **It can just succeed** (registration, telemetry, a feature you don't need): an HLE rule returning 0.
- **It must report a state or fill a struct** with constant values: an HLE rule with fills.
- **It needs logic** (callbacks, handles, real work): C in the runtime extension.
- **SWRR's system module would do it** but the name was unknown: the core resolves host modules by name, so the name must be in its NID table. The runtime extension carries all ~42,000 known names. A core without it carries only the names of the dump it was built against: rebuild it with `make -C ghostldr/native APP0=/path/to/app0`.

To see all of it before the first boot, `ghostldr/tools/import_report.py` (the import coverage report) lists every import of the dump and how it would bind.

## 5. Write a profile

```bash
python3 ghostldr/host/profiles.py new --src /path/to/CUSA12345/app0 > ghostldr/data/titles/CUSA12345.json
python3 ghostldr/host/profiles.py check
python3 ghostldr/host/profiles.py show CUSA12345
```

`<TITLE>.json` is merged over `default.json` (objects key by key, the rest replaced) and validated by `profiles.py check`. Changing a profile needs no reinstall unless `pack` changes: run.py sends profiles with every boot.

| Field | Default | What it does |
|---|---|---|
| `title`, `name`, `version`, `status`, `notes` | | The ID (must match the file name), display name and version (the catalogue shows them; the game selector shows the name when the title has no `param.sfo`, and the version is stored with its name and art), and free text. |
| `eboot.fingerprint` | none | 40 hex digits, `DT_SCE_FINGERPRINT` of `eboot.bin` (`profiles.py detect` prints it). **Binds the whole profile to that build:** on any other build the core uses the defaults. Required for a memory plan and the `ghost_*` features. |
| `pack.prefix`, `pack.order`, `pack.late`, `pack.exclude`, `pack.groups` | generic | Container prefix; open-order file (in `ghostldr/data/titles/`); path prefixes whose traced files go after the other traced ones; a regex of files to leave out; `[regex, rank]` rules replacing the generic order (rank 0 is the boot group). Changing these changes the layout: reinstall. |
| `app_params` | `{"0": 3}` | `sceAppContentAppParamGetInt` ids 0–4. Id 0 is the SKU (3 = full game); 1–4 come from `param.sfo`'s `USER_DEFINED_PARAM_1-4`, and the profile's win. |
| `save_blocks` | 1024 | Size of `<prefix>SAVE` when first created, in 32 KiB blocks. |
| `features` | none | `trace_opens` (log every open, as `run.py --trace-opens`). `ghost_hooks` and `ghost_diagnostics` are Ghost's diagnostics at fixed offsets of its 2.18 executable. |
| `memory.mode` | `plan-free` | `plan` for a memory plan (below). |
| `memory.runtime_reserve_mib` | 64 | Kept for ghostldr. `run.py --slack-mib` overrides it. |
| `memory.video_reserve_mib` | auto | Kept for the video decoder: 100 if the title imports one, else 0. |
| `memory.flex_mib` | 0 | Flexible budget: 0 = the title's own, else 448 (plan-free); no cap (plan). |
| `memory.min_mib` | 1024 | Plan-free: stop the boot below this direct size. |
| `memory.plan` | | See [Memory plans](#memory-plans). |
| `hle` | none | [HLE rules](#6-hle-rules). |
| `tls_sites` | none | `{"eboot.bin": "<TITLE>-tls-eboot.txt"}`: the module's thread-pointer loads (`mov`/`add r64, fs:[0]`) as found by `ghostldr/tools/tls_sites.py MODULE --out FILE`, which disassembles the functions `.eh_frame` describes. The core patches exactly these, each checked first, instead of scanning the whole text (rodata included). Bound to the build: needs `eboot.fingerprint`. The tool also lists fs: accesses it cannot patch. |

### Memory plans

A title whose fixed demand is larger than the plan-free size (Ghost's mode-0 plan wants ~5.07 GiB) needs a plan, bound to its build by the fingerprint:

```json
"memory": {
 "mode": "plan", "runtime_reserve_mib": 32, "video_reserve_mib": 100,
 "plan": {
  "title_fixed_mib": 3996, "overcommit_mib": 120,
  "lazy_sites": ["0x896126", "0x89614a", "0xeee2cf"],
  "tables": [
   {"table": "0x111cd50", "role": "absorb", "what": "streaming pool", "min_mib": 128,
    "expect": ["0x46000000", "0x46000000", "0x46000000", "0x84800000", "0x125800000", "0x125800000"]},
   {"table": "0x111cd20", "role": "set", "what": "CPU malloc heap", "knob": "cpu_heap", "mib": 320}
  ]
 }
}
```

- `tables` are u64 arrays in the executable's text, indexed by memory mode. `set` tables get `mib` (or the run.py knob: `gpu_heap`, `pool1`, `cpu_heap`); the one `absorb` table takes the rest: `free - title_fixed - reserve - set changes + overcommit - video`, between `min_mib` and its own size. Its `expect` contents are checked before anything is patched.
- The title is told `title_fixed + set changes + absorb`. `title_fixed_mib` is the sum of its other reservations.
- `lazy_sites` are return addresses (eboot offsets) of `sceKernelAllocateDirectMemory` calls whose memory is committed 2 MiB at a time when first mapped; `overcommit_mib` is what that saves (measured: the watchdog's `direct alloc from eboot+X: N MiB, mapped M MiB`).
- Ghost's plan reproduces its logged boot: `4532 MiB free in 6 holes; streaming pool 1120 -> 588 MiB; reporting 4520 MiB direct memory`.

## 6. HLE rules

A rule answers one import without C: it writes constants through pointer arguments, then returns a value.

```json
"hle": [
 {"lib": "libSceNpManager", "function": "sceNpGetState", "value": 0,
  "fill": [{"arg": 2, "size": 4, "value": 3}]},
 {"function": "sceNpCheckPlus", "value": 0, "fill": [{"arg": 2, "offset": 0, "zero": 16}]},
 {"nid": "AbCdEfGhIjK", "value": "0x80020016", "log": 5}
]
```

- `function` (its NID is computed) or `nid` for an import with no known name; `lib` limits it to one library (empty: any).
- `value`: what the call returns (`0x8...` for an error code).
- `fill`: up to 8 writes. `arg` is the 1-based argument holding the pointer, `offset` bytes into it, then `size` 1/2/4/8 with `value`, or `zero` with a byte count. NULL pointers are skipped.
- `log`: how many calls to log (1 by default) as `profile rule: lib function -> value`.
- At most 32 rules. They take precedence over everything else, including the core's own HLE, and apply only when the profile does (its build, if it names a fingerprint).

## 7. C-level HLE

What needs logic goes in the runtime extension that run.py serves with the core (`ghostldr/native/ext/`, branch `hle/ext-loader`; see its README). One family of functions per file, `ext/src/hle_<family>.c`, registered by library and NID, so it covers imports with no known name too. The core stays within its 240 KiB.

## 8. Capture an open order

Once the title plays, record which files it opens and pack them together:

```bash
python3 ghostldr/host/run.py boot --app-root ghostfs --title CUSA12345 --trace-opens
```

Play the parts that matter (menus, the first areas, a load from a save), over one or several boots. Then:

```bash
python3 ghostldr/host/capture_order.py --title CUSA12345 -o ghostldr/data/titles/CUSA12345-open-order.txt
```

```bash
python3 ghostldr/host/gfs_pack.py --src /path/to/CUSA12345/app0 --order ghostldr/data/titles/CUSA12345-open-order.txt
```

and reinstall it (`gfs_install.py --title CUSA12345` resends only that title). Set `pack.order` in the profile to keep it. ghostfs logs the first open of each file anyway (`gfs open /app0/...`); `--trace-opens` adds every open on a dev setup, where the first 400 are logged otherwise.
