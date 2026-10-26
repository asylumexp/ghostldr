# ghostldr on a PS4

Luac0re runs on PS4s too, so ghostldr can be sent to SWRR on a PS4 the same way as on a PS5. Until now it had only run on PS5s (13.60 and 14.10). The first PS4 attempt (ghostldr v0.1.1, firmware 12.02) crashed SWRR while the title's code was being written. This page explains that crash and a second, earlier fault, says what changed, and how to run the diagnostic that answers the open questions. The second attempt, with those changes, stopped at their first check of the compiler: see [The second report](#the-second-report-the-boot-stopped-at-the-first-probe). The third (v0.6.0) got one check further, then the compiler process died: ghostldr had sent every system call through getpid's libkernel stub, which a PS4 refuses. See [The third report](#the-third-report-the-compiler-died-at-its-first-clock_gettime). The fourth (v0.6.1) passed every check, placed Ghost and ran it for about 3 seconds, until the PS4 refused Ghost's texture streaming pool: see [The fourth report](#the-fourth-report-ghost-ran-until-its-streaming-pool-was-refused). The fifth (v0.6.2) measured the PS4's limit: SWRR may allocate 2480 MiB more, about 1.6 GiB less than Ghost needs, and Ghost stopped at a black screen once that ran out: see [The fifth report](#the-fifth-report-a-black-screen-ghost-ran-out-of-the-ps4s-memory). The sixth (v0.6.3) showed that this is SWRR's whole budget on a PS4, 2.5 GiB, so Ghost can't run there: see [The sixth report](#the-sixth-report-swrrs-budget-on-a-ps4-is-25-gib). The seventh traced where that limit is set: a kernel sysctl, most likely written by SceSysCore for PS2 Classics; see [The seventh report](#the-seventh-report-where-the-25-gib-is-set). The eighth measured the same 2.5 GiB on a 9.60 PS4 and on a 12.50 PS4 Pro, and ruled out the compiler process, memory pools and the other direct memory devices as a way past it: see [The eighth report](#the-eighth-report-the-same-25-gib-on-960-1250-and-a-pro). The ninth found what enforces it: the kernel caps PS2 Classics at 2.5 GiB when they start, from their signed executable, and nothing a game can do changes it; see [The ninth report](#the-ninth-report-the-kernel-caps-ps2-classics-at-25-gib).

**Status: experimental.** Everything below was worked out from the logs, Luac0re's source and SWRR's executable. On a 12.02 PS4, Ghost's code has run, but no title has reached its title screen yet. The PS5 path is meant to be unchanged; that needs checking on a PS5 too.

## What differs on a PS4

| | PS5 (13.60) | PS4 (12.02) |
|---|---|---|
| Luac0re's `PLATFORM` | `"PS5"` | `"PS4"` |
| Emulator bridge memory (`BRIDGE_BASE`) | `0x900000000` | `0x914104000` |
| Core (IPU buffer + 16 KiB) | `0x9009d8000` | `0x914adc000` |
| EE program buffer (`ps2_prog_ee_exec`, 32 MiB) | `0x900a1c000`: 16 KiB below a 64 KiB boundary | `0x914b20000`: on a 64 KiB boundary |
| A syscall instruction in the compiler process | Luac0re's `jit_syscall.syscall_address` | none: Luac0re keeps one libkernel wrapper per syscall (`syscall_wrapper[n]`) |
| SWRR's companion-app server (`sceCompanionHttpd`) | not running, as far as we know | running |
| Direct memory free after the release | 3292 MiB | 3292 MiB |
| Also loaded in SWRR | | `libSceSsl2.sprx` |

The buffers are laid out the same way relative to the bridge on both consoles: the IPU buffer at +0x9d4000 (16 KiB header, then 256 KiB), the EE buffer at +0xa1c000, then IOP, VU0 and VU1, each after a 16 KiB gap. Only the base address differs. The compiler process maps the buffers at the same addresses as SWRR's main process does: its own log on the PS4 says `jitmem ... R59 Binary Cache @ 0x00000009:14B20000 -> 0x00000009:16B20000`, which matches the main process's `ps2_prog_ee_exec 914b20000-916b20000`.

## The fatal crash: code written into the EE buffer's first 16 KiB

From the kernel's report:

```
# thread name: Compile_VU0
# proc name: ps2-emu-compiler.self
# reason: page fault (user write data, page not present)
# fault address: 0000000914b20000
# rsi: 00000009141c4000  rdi: 0000000914b20000  rdx: 0000000000060000
```

This is ghostldr's `jit_memcpy` of `libSceFios2.prx`'s text (0x60000 bytes, from the staging buffer in the bridge) to `0x914b20000`, the first byte of the EE buffer. The core's log just before it says `libSceFios2.prx: placed at 914b20000`.

**Why only the PS4.** Modules are placed from the code region's start, rounded up to 64 KiB:

- On the PS5 the EE buffer starts at `0x900a1c000`, so rounding up skips its first 16 KiB, and the first module lands at `0x900a20000`.
- On the PS4 the buffer starts at `0x914b20000`, which is already on a 64 KiB boundary, so the first module landed at the buffer's very start.

**The first 16 KiB of each buffer can't be written through the compiler.** The evidence:

- The core itself goes at the IPU buffer's +16 KiB.
- Luac0re notes "Safe write address = +0x4000" for the EE and IOP buffers, and +0x8000 for VU0 and VU1.
- On a PS5, writing the game selector at the EE buffer's start "killed the compiler call". sel.c has rounded it up since then, but the boot's module placement only got that 16 KiB skip by luck.
- On the PS4 the fault came at exactly that spot.

**Measured on a 13.60 PS5 (the diagnostic, below):** the compiler can neither read nor write the first bytes of any buffer (EFAULT both ways), so nothing is mapped there in its address space. That fits the PS4's "page not present" at the EE buffer's first byte. Past the header it writes every buffer at the same address: the EE and IOP buffers past 16 KiB, VU0 and VU1 past 32 KiB. A probe on a PS5 also found that a fresh read-write alias of an emulator buffer, mapped in the compiler, writes the whole buffer, header included. Whether a PS4's compiler writes the rest of the EE buffer is still unproven, although its own log points that way.

## The earlier, non-fatal fault: SWRR's companion-app thread

```
hostmem: released 201014000-211014000 (256 MiB, prot 3 type 0 phys 10000) munmap 0 release 0
CRASH sig 11 fault addr 20581c5b8 rip 800a2fceb = ?+800a2fceb rsp 7eed27d30
  code+000: c5 fc 11 80 58 02 00 00 c5 fc 11 80 38 02 00 00 ...
  #00 8009d9bdc = ?+8009d9bdc
  #01 4ac126 = ?+4ac126
```

What SWRR's executable shows (US and EU run the same code, loaded at `0x400000`):

- **The 256 MiB mapping is SWRR's libc heap.** At eboot+0x9c76c SWRR allocates 256 MiB of direct memory and maps it read-write. At eboot+0x9c7cb it calls `sceLibcMspaceCreate` on it; the handle goes to eboot+0x3906c0.
- **The faulting thread is SWRR's `SystemServicesThread`.** Return address eboot+0xac126 follows `call sceCompanionHttpdGetEvent` in that thread's entry, eboot+0xabc70, created at eboot+0xa670d with its handle at eboot+0x182f330.
- **What the thread does.** It calls `sceCompanionHttpdInitialize2` with a 10 MiB work buffer that it takes from that heap (`sceLibcMspaceMalloc(heap, 0xa00000)`; the pointer is at eboot+0x182f338). Then it loops: it polls system-service events every ~110 ms and calls `sceCompanionHttpdGetEvent`. The fault is the companion library's AVX memset into its own work buffer, which ghostldr had just released.
- **The server is up on a PS4.** SWRR logs `sceCompanionHttpdInitialize2() failed` (channel `x-srvc`) on failure; the PS4 log has no such line.
- **On a PS5 the same thread runs too,** and its 10 MiB buffer is in the heap (the diagnostic on a 13.60 PS5). Every PS5 boot has nonetheless released the heap without a fault: there, `sceCompanionHttpdGetEvent` doesn't touch the buffer.
- **The thread can be stopped.** The loop leaves when the word at eboot+0x182f328 is non-zero. SWRR never sets it. On leaving, the thread calls `sceCompanionHttpdTerminate`, frees the buffer, clears the pointer and returns.

The crash handler parked the thread, so the boot went on. That's why the fatal crash came later.

## Other things the log and the code show

- **No compiler syscall gadget on a PS4.** `ghostldr.lua` passed `jit_syscall.syscall_address or 0`, and that's 0 on a PS4. `jit_recv_fd` would have called address 0 in the compiler. That happens whenever a new JIT block is made: the current extension (320 KiB) always gets one, so today's build would have crashed even earlier on a PS4, at `ext_start`. Fixed: see below.
- **The GS SubmitDone thread keeps running.** `gsthread [  80.413sec] WARN: SubmitDone: 5923ms since last call!` comes after the crash. It's SWRR's "GS SubmitDone" thread (eboot+0x15e0f0), which only calls `sceGnmSubmitDone`, never touches the released buffers, and isn't cancelled by Luac0re on either console. It isn't a problem.
- **The `i-spu2 ... Abort received` lines** are from the PS2 game, 8–33 s into SWRR, before Luac0re ran. They're not related.
- **Luac0re's thread cancels.** Luac0re cancels the GS, IOP/SPU2 and Runtime Bridge threads, using `scePthreadSelf + 0x10` as `scePthreadCancel`.
  - On a 13.60 PS5 that is the real export, but only the Runtime Bridge thread is gone. GS and IOP/SPU2 are still alive, presumably blocked without reaching a cancellation point. So are VU1, GsCheck, CDVD, AudioOut, VideoRecording, TrophyPump and the dialog thread.
  - Whether the offset holds in a 12.02 libkernel is unverified; the diagnostic checks.
- **`libSceSsl2.sprx` is loaded in SWRR on the PS4.** Host modules are looked up by name, so this changes nothing for the loader.
- **`libSceNpScore.sprx`** fails to load (0x80020002) and is found as `libSceNpScoreRanking`, as the PS5 does.
- **Memory.** Direct memory free after the release was 3292 MiB, as on the PS5. SWRR runs in base mode on a Pro as well, so a Pro shouldn't differ, but no Pro has been tested.

## What changed

Each change is either unconditional but address-neutral on a PS5, or gated on the PS4 flag.

1. **The platform reaches the core.** `ghostldr.lua` sets ctx flag 8192 (`CTX_FLAG_PS4`) when Luac0re's `PLATFORM` is `"PS4"`. It also writes the firmware (`FW_VERSION` "12.02" → `0x1202`) in a new last context field, `fw`. The core logs both.
2. **A compiler syscall gadget on a PS4.** The script takes Luac0re's `getpid` wrapper in the compiler plus 7 bytes, which is the "mov r10, rcx; syscall" that Luac0re's PS5 address also points at. It checks those bytes through the compiler first. `jit_recv_fd` now refuses, with a log line, instead of calling 0. On a PS5 the context gets the same value as before.
3. **The code region starts after the buffer's first 16 KiB, on every console** (`arena_code_region`, `JIT_BUF_HEADER`). On a PS5 this moves nothing: rounding up already skipped those 16 KiB, and the tests check Ghost's four module addresses against the 13.60 log. On a PS4 the first module moves from `0x914b20000` to `0x914b30000`.
4. **PS4 only: no compiler write without proof** (arena.c, ps4.c).
   - `arena_init` probes each executable region past its header, at both ends, plus the small area. A probe uses two system calls that the compiler runs on the address and that return EFAULT instead of crashing it:
     - `write()` sends 16 bytes from the address down Luac0re's socket pair, so this process can read the compiler's own bytes;
     - `clock_gettime()` writes 16 bytes there.

     The region counts as writable only when the written bytes show up here. The compiler's original bytes are then put back.
   - The header is 16 KiB, or 32 KiB when +16 KiB isn't writable (the VU buffers).
   - `sceKernelVirtualQuery` can't answer this: run in the compiler on a PS5, it lists none of the JIT mappings, not even the bridge memory the compiler uses.
   - `arena_write` refuses anything outside the verified parts and reads every write back. The selector's writes now go through the same check (`arena_wr`).
   - If no verified region can hold the title's code, the part it needs (Ghost: 25 MiB) is backed by a new JIT block, mapped over the EE buffer's start at that fixed address only. If that also fails, the boot stops with `the title's code has nowhere to go` and nothing is written.
   - That fallback only helps small titles. A 13.60 PS5 creates at most 768 KiB of new JIT memory; the emulator's own buffers use the rest of the budget. A PS4 is probably similar. A better fallback, not implemented, follows a probe on a PS5: open the emulator's own EE object from the compiler's descriptors, and map a fresh read-write alias of it in the compiler. That alias writes the whole object, the first 16 KiB included, and costs no budget.
5. **PS4 only: SWRR's heap is released only once nothing uses it** (hostmem.c). Before the release, the core sets the thread's quit word and waits, up to 5 s, until the work buffer is freed and the thread has ended. If it doesn't stop, the 256 MiB heap is kept.
   - **Cost:** nothing when the thread stops (the expected case). Otherwise 256 MiB of the 3292 MiB. For Ghost that comes out of the texture streaming pool: about 588 MiB becomes about 332 MiB, below the ~550 MiB at which loading a save was seen to stall on a PS5.
   - The title then gets the system-service events that thread used to consume.
6. **The diagnostic**, below. It also adds `jit_block_destroy`. `jit_block_create` given an address now maps only there; it no longer falls back to `sceKernelJitMapSharedMemory`, which places the block at its home address over whatever is mapped there. The extension's block, which asks for no address, is made as before.

Placement on a PS5 is unchanged: no probes, no test writes, no readback. The only new bytes in the context are `fw` and a larger `size`, and the core reads them only for its log. On a 13.60 PS5, Ghost booted with these changes as before (see What is verified).

## The diagnostic

`run.py probe --diag` sends the core with ctx flag 16384. It logs and then returns to Luac0re's loader. It boots nothing, releases no memory and closes nothing. It runs the PS4 boot's own checks, also on a PS5.

What it logs:

1. **Platform:** Luac0re's platform and firmware, the system software version, Neo (Pro) mode, direct and flexible memory, the addresses Luac0re gave, and the compiler's syscall gadget. Its bytes aren't read here: on a PS5, system code in the compiler is execute-only and reading it kills SWRR. On a PS4, `ghostldr.lua` checks them.
2. **SWRR's threads:** for 13 of them (EE, IOP/SPU2, GS, Runtime Bridge, VU1, SubmitDone, GsCheck, CDVD, AudioOut, SystemServicesThread, VideoRecording, TrophyPump, dialog), whether each is alive (a blocked thread counts) and its name. Also Luac0re's `scePthreadCancel` guess against the real export.
3. **Memory:** SWRR's heap, the companion buffer and the quit word. What a boot would release (only listed). The direct and flexible memory census.
4. **The JIT buffers:**
   - the compiler process's map as its `sceKernelVirtualQuery` lists it (without the JIT mappings, on a PS5);
   - this process's map from the bridge to the VU heaps;
   - the probes checked on known memory first (`jit: PS4 check` lines), then per buffer whether the compiler writes it past its header (the boot's own probes), and where a boot would put the first module.
5. **Writes through the compiler:** each one is put back. They go to the shared scratch, the free small area after the core, and the ends of each buffer.
6. **New JIT blocks** of 64 KiB, 768 KiB, 1, 4, 16 and 32 MiB, each mapped here at a fixed address inside a reserved range. Mapping a JIT object without an address puts it at its "home" address, over whatever is there; on a PS5 that once replaced the core's data and crashed SWRR. Each block is created, has a function written into its last page, is called, then destroyed. This step stops at the first failure: a 13.60 PS5 makes at most 768 KiB of new JIT memory (0x8002000c past it). Only one page is written, so a block that works here shows it can be created and mapped, not that all of it can be filled.
7. **Last, each buffer's first 16 bytes,** read and written through the compiler by the same probes. On a 13.60 PS5 both give EFAULT.
8. **On a PS4 only: SWRR's budget,** the kernel's limits for the application ([the sixth report](#the-sixth-report-swrrs-budget-on-a-ps4-is-25-gib)). Each call goes through libkernel's own stub for its number, with SIGSYS ignored meanwhile: a refused call can end in `nosys()`, which also sends SIGSYS.
   - The calls: `budget_getid` (609), `budget_get_ptype` (610), `get_proc_type_info` (612), the resident counts (613, 615), and `budget_get` (570). The numbers and most prototypes are psdevwiki's.
   - `budget_get`'s and `budget_set`'s prototypes aren't documented. Everything the kernel may write ends at an unmapped page, so a wrong guess fails with EFAULT and never overwrites memory. The returned budget is logged as hex.
   - The libkernel functions around them (`sceKernelGetBudget`, `sceKernelSetBudget`, `sceKernelGetResourceLimit`, `sceKernelSetGameDirectMemoryLimit` and others) are dumped (96 bytes of code each) when SWRR's libkernel exports them. They aren't called. The dumps give their prototypes.
   - **One try to raise the limit:** if the budget holds a value near 2.5 GiB, `budget_set` (571) is called with it raised to 4608 MiB. If the kernel takes it, the diagnostic measures what it then hands out and sets the budget back at once. A game is very unlikely to be allowed to change its own budget, so expect a refusal.
   - **The compiler process's memory** (section 9; [the 2 GiB question](#the-seventh-report-where-the-25-gib-is-set)): the compiler opens `/dev/dmem<container>` itself and, through its own system call stubs (open, ioctl, dmem_container), asks for the device's size, its largest free block and then as much direct memory as the kernel grants it, in pieces. Meanwhile this process measures what it may still allocate, and maps one of the compiler's pieces here. Everything is released. libkernel's own direct memory functions are not called there: the first one never came back on a 9.60 PS4 ([the eighth report](#the-eighth-report-the-same-25-gib-on-960-1250-and-a-pro)).
   - **The game direct memory limit sysctl** ([the seventh report](#the-seventh-report-where-the-25-gib-is-set)): its name, from `sceKernelSetGameDirectMemoryLimit`'s code, and its value. Then one write of 4608 MiB; if the kernel takes it, what it then hands out, and the old value written back. Also `sceKernelGetResourceLimit` for process types 0 to 3, decoded as `{id, attr, limit}` entries.

It changes nothing a boot depends on. The test writes are restored and the JIT blocks are destroyed. Afterwards SWRR is in the same state as after Luac0re's loader, so `run.py boot` can follow in the same session. Restart SWRR for normal play.

### How to run it

On the PS4: start SWRR and go to OPTIONS → HALL OF FAME until Luac0re's "Remote Lua Loader" is listening on TCP 9026, as for a boot. Then, on the computer:

```bash
python3 ghostldr/host/run.py probe --diag --log build/ghostldr/ps4-diag.log
```

- With a release zip, add `--no-build`.
- Add `--host <the PS4's address>` if the console isn't found by itself; `console.py --list` shows PS4s too.
- The run listens for 90 s. The diagnostic itself takes a few seconds; it ends with `diag: done`, and Luac0re shows "core returned 0".

If you can, also run the boot once in the same or a fresh session, and keep its log. It now logs its PS4 checks (lines with `PS4:`) before writing anything:

```bash
python3 ghostldr/host/run.py boot --log build/ghostldr/ps4-boot.log
```

### What to send back

- `build/ghostldr/ps4-diag.log` (and `ps4-boot.log` if you ran the boot).
- If SWRR or the console crashed: the kernel log of that moment (a klog capture as before) and the last `diag:` line in the log.
- If the console stops responding during the JIT block step, power it off and on. Tell us the last `diag: JIT block` line.

Before posting the logs anywhere public, remove user IDs and names from the kernel log. ghostldr's own log has none, apart from the ghostfs user ID line of a boot.

## The second report: the boot stopped at the first probe

A boot with the changes above (12.02, Ghost from ghostfs, `--no-build`) logged up to:

```
arena: RX 914adc000-914b08000 (176 KiB)        <- the IPU buffer from +16 KiB: the core's text
arena: RX 914b20000-916b20000 (32768 KiB)      ... and IOP, VU0, VU1 as before
arena: small area 914b14000-914b1c000           <- past the core's data (914b08000-914b14000)
arena: PS4: 914adc000-914b08000: holds the core
```

and nothing after it, as far as the log went. Whether SWRR crashed (a CE error), hung, or the copy stopped there isn't known.

**Where that is.** `ps4_verify` (arena.c) logs "holds the core" for the IPU buffer without probing it, then probes the EE buffer at `914b24000`; the next line comes only after that buffer's probes. They were the first compiler calls of the boot (the core itself was written by `ghostldr.lua`), and the first use of the syscall gadget and of Luac0re's second socket pair on a PS4. Luac0re never uses that pair on a PS4 below 14.00: its sockets are made directly there. Nothing touches the core's text, its data or the small area before the four buffers are done; `tests/native_host/arena_host.c` runs this exact layout (`ps4-report`) and the logic neither loops nor faults with any compiler.

**What in a probe can stop.**
- `jit_peek` read the compiler's 16 bytes with a blocking read. If the compiler's `write()` returned 16 but the bytes didn't reach this process's end of the pair (another descriptor, or the pair not connected as on a PS5), the read waited forever, with no log. That's the only way the probes could wait for good.
- A compiler call that doesn't return. `jit_call` gave up after 2,000,000 spins with `usleep(50)`. That's about 100 s on a PS5, but a PS4's kernel (FreeBSD 9) sleeps at least a scheduler tick, so the same limit is half an hour or more. It would look like a hang.
- A crash of the compiler process. SWRR is closed with an error, as in the first report. The probes' system calls return EFAULT rather than fault; the error path of the PS4's libkernel wrapper hadn't been used by ghostldr before.

**What changed.** A PS5 boot runs none of it and places everything at the same addresses as before. The diagnostic runs the PS4 checks on any console, so it runs these too.
1. No read on the socket pair can block. `jit_peek` drops stale bytes first, then reads with `MSG_DONTWAIT` for up to ~0.2 s; the compiler's system call has returned by then, so its bytes are queued already. `jit_recv_fd` does the same on a PS4 (up to ~1 s).
2. On a PS4 a compiler call is given up after 30 s by the process clock, with log lines after 2 and 10 s. No compiler call is made after that; they fail at once.
3. Before any buffer is probed, `jit_ps4_check` tests the probes on memory known to be good, each step logged before it runs: `getpid()` through the gadget, `clock_gettime()` into the shared scratch, `write()` of the shared scratch through the pair, and `write()` from the first page (EFAULT expected). If one fails, no buffer is probed, nothing is written through the compiler, and the boot stops with "the compiler checks failed".
4. Each probe logs each step before it runs, so the last line of a log that stops names the call. A probe no longer writes where the compiler's bytes differ from this process's: that's other memory.

`tests/native_host/jitrpc_host.c` runs `jitrpc.c` against a fake compiler thread and a real socket pair. A compiler whose bytes never arrive, or a descriptor that isn't the pair, ends in under a second, and a compiler that never answers ends after 30 s (by its clock). The old read would have blocked forever in the first case.

**What to send back.** The diagnostic's log first (`run.py probe --diag`), then a boot's. In either, the last `jit: PS4 check` or `arena: PS4: probe` line names the call. If SWRR closed with an error, the error code too, and a kernel log if you can capture one.

## The third report: the compiler died at its first clock_gettime

The diagnostic of v0.6.0 on a 12.02 PS4 (`run.py probe --diag`), and a boot after it, both ended at the same line:

```
jit: PS4 check: syscall gadget 83a40c7e7, socket pair: compiler fd 5, here fd 26
jit: PS4 check 1/4: getpid() in the compiler
jit: PS4 check 1/4: compiler pid 82 (this process 81)
jit: PS4 check 2/4: clock_gettime() into the shared scratch 9141a6340
jit_call: fn 83a40c7e7 rax e8: no answer from the compiler after 18 s
jit_call: fn 83a40c7e7 rax e8: no answer from the compiler after 18 s
```

Everything before the checks worked as on a PS5: the platform and firmware, all 13 of SWRR's threads, Luac0re's `scePthreadCancel`, the heap, the memory census, both processes' maps and the arena. The diagnostic's own log ends there; the boot's log stops at check 2/4.

**What the log shows.**
- **The gadget works for getpid.** Check 1/4 ran getpid in the compiler through the gadget, and the compiler's pid came back. So the chain, the gadget address and the way back all work.
- **The same gadget with clock_gettime's number never came back.**
- **This process was stopped too.** `jit_call` logs after 2 s and after 10 s of waiting. Both lines came together, 18 s after the call, and nothing came after them, not even the give-up line at 30 s. This process can only miss its 2 s line if it doesn't run at all. A PS4 stops all of an application's processes while it writes a crash report for one of them, then closes the application. So the compiler process crashed, or was killed, inside that call.

**Why.** The gadget is getpid's libkernel stub, 7 bytes in: `mov rax, 20; mov r10, rcx; syscall`, entered after its `mov rax`. A PS5 runs a system call from any libkernel stub, with any number in rax, and v0.6.0 assumed a PS4 did too. A PS4 doesn't: a system call runs only from its own number's stub.
- Luac0re's own code says so. It takes "any syscall wrapper in libkernel" on a PS5, but on a PS4 it searches libkernel for every number's own `mov rax, N; mov r10, rcx; syscall` and calls each number through its own stub ("PS4 requires valid syscall wrapper from libkernel .text").
- psfree's PS4 chains do the same.
- getpid through its own stub worked; clock_gettime, the first other number, through getpid's stub killed the compiler.

Both v0.6.0 logs end at the first system call that wasn't getpid. Any other compiler system call would have died the same way: moving a descriptor (sendmsg), the probes (write), freeing a JIT block (munmap, close) and the FTP server's socket.

**What changed.**
1. **ghostldr.lua passes the compiler's stub for each number the core and the extension call there:** getpid, write, close, sendmsg, munmap, socket and clock_gettime. They go in a new context field, `jit_sysw`, the slot that held an unused, always-zero `libc_handle`, as (number, address) pairs ended by a zero number.
   - The stubs come from Luac0re's table, which covers the first 128 KiB of libkernel. A number missing there (clock_gettime may be) is searched in the rest of this process's libkernel text, which a PS4 can read, and rebased into the compiler's copy, as Luac0re rebases its own table.
   - Each stub's bytes are checked in the compiler before it goes in the table.
2. **On a PS4, `jit_call` sends a call of the syscall gadget to the stub of the number in rax.** The call sites are unchanged, the extension's included. A number without a stub isn't called: the log says `no libkernel stub for system call N in the compiler: not called`, and that call fails. Nothing goes through another number's stub.
3. **The check's first line names the stubs** it is about to use (getpid, clock_gettime, write, sendmsg). The diagnostic lists the whole table.

A PS5 is unchanged: `jit_sysw` is 0 there, and the gadget takes every number as before.

`tests/native_host/jitrpc_host.c`'s fake compiler now enforces the PS4 rule: a system call through another number's stub "kills" it, and it never answers again. With every number on getpid's stub, as v0.6.0 called them, it dies at check 2/4, like the PS4 did. With each number's own stub, every check passes. Without a stub, nothing is called and nothing dies. `boot_fake.lua` checks the table ghostldr.lua builds: Luac0re's stubs, clock_gettime found past them, a wrapper with unexpected bytes left out, and no table on a PS5.

**What to send back.** The diagnostic's log again (`run.py probe --diag`). Its first lines now list the stubs, and its `jit: PS4 check` lines go on to the buffer probes. If it stops again, also say whether SWRR closed with an error code.

## The fourth report: Ghost ran until its streaming pool was refused

A v0.6.1 boot of Ghost from saved data on a 12.02 PS4 got through everything before the title:
- **The compiler checks:** all four passed. getpid, then clock_gettime, each through its own stub, the socket pair, and EFAULT from the first page.
- **The buffer probes:** they found what a 13.60 PS5 has. The compiler writes the EE and IOP buffers past 16 KiB, and VU0 and VU1 past 32 KiB. The small area after the core is writable too.
- **Placement:** libSceFios2, libc, NpToolkit2 and eboot.bin went to `0x914b30000`, `0x914cc0000`, `0x914de0000` and `0x914ea0000`.
- **The extension:** its 384 KiB went into a new JIT block, mapped at `0x919038000`, and its self-test passed. 415 imports were bound to it.
- **SWRR's memory:** the SystemServicesThread stopped 40 ms after it was asked, and 1236 MiB were released. The largest free block was then 3292 MiB.
- **Ghost's start:** its main thread started, and module starts, video out, the GPU's queues, trophies and NP all went as on a PS5. Ghost began loading its first packs.

Then, 1 s into the title:

```
AllocateDirectMemory(0-120000000, len 24800000, align 200000, type 3) -> 80020023 phys 0 from eboot+eee115
```

That is the texture streaming pool, sized 584 MiB by the plan. A PS5 boot allocates it at phys `a7200000`, right after the 1390 MiB that Ghost had just placed at `50400000`. Here the PS4 refused it with EAGAIN, although by the earlier numbers 1902 MiB were free from `a7200000` in one block. ghostldr's fallback, which takes the size in pieces wherever they fit, failed too. Two seconds later Ghost read through a null pointer (`eboot+1c880`, `mov eax, [rdi+4]` with rdi 0, from the frame chain `eboot+8ba2ad`, `89c145`, `893d12` and the title's entry), most likely where it expected the pool.

**What that means.** A refusal with more than enough room in one free block isn't fragmentation. The PS4 caps how much direct memory the application may allocate, below the free physical range that SWRR's 4608 MiB spans, and the plan had counted only the free holes. The size of the cap, and whether it counts the memory ghostldr released from SWRR, isn't known yet. Ghost had about 2.1 GiB allocated when it was refused. Its whole plan needs about 3.9 GiB, part of it backed only as Ghost touches it. So if a PS4 hands out much less than that, Ghost can't run there, whatever the pool's size.

The PS4's libSceUsbStorage also has no `RequestMap2`, so games can't be loaded from a USB drive there. Saved data (ghostfs) works.

**What changed.**
1. **On a PS4, the plan counts what the kernel hands out.** Before planning, the core takes the largest piece of direct memory the kernel grants, halving down to 2 MiB, again and again until even 2 MiB is refused. Then it releases them all. It does this for type 0 and for type 3 (the GPU's) and logs both: `budget: PS4: the kernel hands out N MiB of direct memory (type 0), M MiB (type 3), of F MiB in free holes`. The streaming pool is sized from the smaller of that and the free holes. A PS5 plans as before.
2. **When the pool's minimum doesn't fit,** the log says by how much the plan is short and that the title will likely fail later.
3. **A refused allocation logs the kernel's largest free block,** so the next refusal shows whether there was room.
4. **The diagnostic measures the same on a PS4,** before anything is released.

`tests/native_host/profile_host.c` runs the planner against a fake PS4 kernel that hands out less than its holes. With 4400 MiB, Ghost's pool comes out at 456 MiB (reporting 4388). With 2600 MiB, the pool is clamped to its minimum and the log says the plan is 1472 MiB short. With more than the holes, the plan is a PS5's. Nothing measured stays taken.

**What to send back.** A boot log again, and a diagnostic (`run.py probe --diag`). The `budget: PS4:` line and the diagnostic's `the kernel hands out` line give the PS4's limit.

## The fifth report: a black screen, Ghost ran out of the PS4's memory

v0.6.2 on the same 12.02 PS4 measured the limit:

```
budget: PS4: the kernel hands out 2480 MiB of direct memory (type 0), 2480 MiB (type 3), of 4529 MiB in free holes
budget: only 2480 MiB free; streaming pool clamped to 128 MiB
budget: PS4: the title's plan needs about 1592 MiB more direct memory than this console hands out; it will likely fail when it allocates the rest
```

Ghost then went further than ever on a PS4:
- its 128 MiB pool was allocated;
- the GPU drew and flipped frames;
- the boot flow went through its splash (the splash movie was skipped: no memory for the video decoder);
- the level warp ran to `Done`;
- Ghost asked for its first game pack.

But its fixed heaps (2234 MiB) and 246 MiB of the heaps it fills as it goes used up the 2480 MiB exactly. From then on every new chunk was refused:

```
vdm: lazy commit of chunk 113 (type 0) failed 80020023 after 114 chunks: out of direct memory (246 MiB of lazy reservations committed, largest free block 1947648 KiB)
```

The sound and game packs couldn't be mapped. Reads into them failed with EFAULT, and Ghost waited for them for good: a black screen, no crash.

**What it means.** On this PS4, SWRR may allocate 2480 MiB once ghostldr has released the emulator's memory. Ghost of Tsushima needs about 3.9 GiB in mode 0. Its fixed heaps alone (CPU 320 MiB, engine 332 MiB, GPU 1390 MiB) and what the first menus fill take about that much. So Ghost can't run in SWRR on a PS4, unless the release turns out not to count. Titles that need less may fit: a title without a memory plan is now told what the PS4 hands out.

**What isn't known yet** is whether releasing SWRR's 1236 MiB raised the limit at all. If the PS4 doesn't count a release, SWRR's own allocations could be handed to the title instead of released: about 1.2 GiB more. That could be enough for Ghost's menus, though still not for all of its plan.

**What changed.**
1. **A PS4 boot measures the limit before and after it releases SWRR's memory,** and logs how much of the release counted: `hostmem: PS4: the kernel hands out B MiB before the release, A MiB after: C of the 1236 MiB released count`.
2. **A plan short by more than 256 MiB on a PS4 stops the boot before the title starts** (`budget: PS4: CUSA11456 needs about N MiB more direct memory than this PS4 lets SWRR allocate: not started`). That replaces the black screen. Less than that, and the title starts with a warning: the heaps it fills as it goes can carry it a while.

`profile_host.c` checks both: 2480 MiB stops Ghost's boot, 1592 MiB short; 3900 MiB starts it, 172 MiB short.

**What to send back.** One boot log with v0.6.3. Its `hostmem: PS4:` line says whether the release counts.

## The sixth report: SWRR's budget on a PS4 is 2.5 GiB

v0.6.3 measured the limit on both sides of the release:

```
hostmem: freed 1236 MiB, direct available now 3292 MiB
hostmem: PS4: the kernel hands out 1268 MiB before the release, 2504 MiB after: 1236 of the 1236 MiB released count
budget: PS4: the kernel hands out 2480 MiB of direct memory (type 0), 2480 MiB (type 3), of 4529 MiB in free holes
budget: PS4: CUSA11456 needs about 1592 MiB more direct memory than this PS4 lets SWRR allocate: not started (docs/ps4.md)
```

**The release counts in full.** Before it, SWRR held 1290 MiB: the 1236 MiB ghostldr releases and 54 MiB it keeps. The kernel would hand out 1268 MiB more. So SWRR's direct memory budget on a 12.02 PS4 is about 2560 MiB, 2.5 GiB, although the size it reports (`sceKernelGetDirectMemorySize`) is 4608 MiB. That number is the size of the range SWRR's memory comes from, not what SWRR may allocate. Sony appears to run this PS2 Classic with a smaller budget than a PS4 game gets. On a PS5, SWRR gets the whole 4.5 GiB.

**What it means.**
- **Ghost of Tsushima can't run in SWRR on a PS4.** It needs about 3.9 GiB, 1.6 GiB more than SWRR may have. Nothing inside SWRR raises a budget the kernel sets. The boot now stops before Ghost starts, as above.
- **Smaller titles may fit.** A title without a memory plan is told what the PS4 hands out: about 2.4 GiB, less ghostldr's reserve and the title's flexible memory.
- **A PS4 on 12.02 is within Lapse's range** (the kernel exploit covers PS4 firmware up to 12.02). A jailbroken PS4 runs PS4 games directly, without SWRR or ghostldr.


## The seventh report: where the 2.5 GiB is set

v0.6.4's diagnostic, and a PS4 SceShellCore read statically:

- **SWRR can't read or set its budget.** `budget_getid` returns ENOSYS: the budget calls need credentials a game doesn't have. A refused call can end in `nosys()`, which also sends SIGSYS; with SIGSYS ignored, SWRR survived.
- **libkernel's budget functions,** decoded from their code in SWRR's libkernel:
  - `sceKernelGetBudget(&id)` is `budget_getid`;
  - `sceKernelSetBudget` passes its arguments to `budget_set`;
  - `sceKernelGetResource` is `budget_get`;
  - `sceKernelGetResourceLimit(type 0-3, entries, &count)` is `budget_get(-2 - type, ...)`.
  - **`sceKernelSetGameDirectMemoryLimit(limit)` writes the limit with `sysctlbyname(name, NULL, NULL, &limit, 8)`.** The game's direct memory limit is a kernel sysctl.
- **ShellCore doesn't set the 2.5 GiB.** It builds every application's budget from the kernel's defaults for its process type: `sceKernelGetResourceLimit(type, entries, &count)`, entries `{u32 id; u32 attr; u64 limit; u64 0}`, nine resources, then `sceKernelCreateBudget`. The type comes from param.sfo's CATEGORY; a game gets type 0, PS2 Classics included. ShellCore never calls `sceKernelSetGameDirectMemoryLimit` or `sceKernelSetBudget`, and writes no memory sysctl. The only PS2-specific thing it does is pass an `isPs2Emu` flag to SceSysCore, and only for CATEGORY `gdo`.
- **SWRR isn't `gdo`.** Its package's param.sfo (EU CUSA03492 1.00) has CATEGORY `gd`, APP_TYPE 1 and ATTRIBUTE 0x2, like an ordinary PS4 game; its 1.02 patch is `gp`. So ShellCore gives SWRR a normal game budget and doesn't mark it as a PS2 emulator. The 2.5 GiB is set elsewhere: most likely in SceSysCore or the kernel, through that sysctl, from something other than the category, such as the executable itself.

**What v0.6.5's diagnostic adds,** on a PS4 (section 8 of the diagnostic):
- the sysctl's name, found in `sceKernelSetGameDirectMemoryLimit`'s code, its value, and the strings stored next to it;
- **one write of 4608 MiB to it.** If the kernel takes it, the diagnostic measures what SWRR can then allocate and writes the old value back. A game writing a system setting will most likely be refused;
- `sceKernelGetResourceLimit` for types 0 to 3, decoded as entries: the kernel's default limits per process type, if a game may read them.

**What it found** (v0.6.5 on 12.02):

```
diag: limit: sceKernelSetGameDirectMemoryLimit writes sysctl 'kern.dmem.game_budget_limit'
diag: limit: kern.dmem.game_budget_limit = 0 (0 MiB), 8 bytes: rc -1 errno 1
diag: limits: sceKernelGetResourceLimit(type 0) -> 8002004e (errno 78), 0 entries
```

- The limit's sysctl is `kern.dmem.game_budget_limit`. libkernel keeps `vm.kern_heap_size`, `vm.ps4dev.trcmem_total` and `vm.ps4dev.trcmem_avail` next to it.
- SWRR can't even read it (EPERM), so it can't write it either, and the write wasn't tried. `sceKernelGetResourceLimit` is refused for every type (ENOSYS), as `budget_getid` is.
- So none of the memory budget can be read or changed from SWRR: its budget, the per-type defaults, the game limit. On a 12.02 PS4, SWRR's 2.5 GiB is final from inside it. Only kernel access (a jailbreak) could change it, and a jailbroken PS4 runs PS4 games directly.

**The 2 GiB question.** SWRR's budget comes to 2560 MiB, which is exactly 4608 − 2048: the 4.5 GiB a PS4 game should get, less exactly 2 GiB. The compiler process joins the same budget. If it holds that 2 GiB, possibly in another direct memory container SWRR can't see, the cut is the compiler's, and having it release that memory would give SWRR the full 4.5 GiB without touching any system setting. v0.6.6's diagnostic checks this from inside the compiler (section 9): its direct memory size, its free block, every direct memory allocation it holds, its flexible memory, and what the kernel hands out there compared with here. It also logs SWRR's direct memory container and the locked-memory budget sysctls `vm.budgets.mlock_avail` and `mlock_total`.

## The eighth report: the same 2.5 GiB on 9.60, 12.50 and a Pro

`run.py probe --diag` on two more consoles, a stock 9.60 PS4 (US SWRR) and a 12.50 PS4 Pro (EU SWRR):

```
diag: SWRR; the loader says PS4, firmware 9.60; system software ' 9.600.011' (9600011, rc 0); Neo mode 0
diag: direct memory 4608 MiB (3324 MiB free), flexible free 38 MiB
diag: the kernel hands out 1276 MiB more direct memory (type 0), 1276 MiB (type 3)
census: 1283 MiB of direct memory allocated

diag: SWRR; the loader says PS4, firmware 12.50; system software '12.508.001' (12508001, rc 0); Neo mode 0
diag: direct memory 4608 MiB (3324 MiB free), flexible free 38 MiB
diag: the kernel hands out 1276 MiB more direct memory (type 0), 1276 MiB (type 3)
census: 1283 MiB of direct memory allocated
```

- **The same limit everywhere.** SWRR holds 1283 MiB and may allocate 1276 MiB more: 2559 MiB in all, as on 12.02. Firmware doesn't change it (9.60, 12.02, 12.50).
- **A Pro doesn't change it either.** A Pro runs SWRR in base mode (Neo mode 0, the 4608 MiB range of a base PS4) and gives it the same 2.5 GiB. A game can't switch itself to Pro mode.

**What doesn't hold the other 2 GiB** (tried from inside SWRR on the stock 9.60 PS4):
- **The compiler process.** It can't open `/dev/dmem1`, its direct memory device (`compiler memory: open -> -1`), so it holds no direct memory and can't allocate any. libkernel's direct memory functions there never return: their first call opens the devices. So the cut isn't memory the compiler holds, and the compiler can't allocate memory for SWRR either.
- **Physical memory.** SWRR's `/dev/dmem1` is 4608 MiB, and its largest free block is 3292 MiB while SWRR may take only 1276 MiB more. Nobody has allocated the missing 2 GiB: the limit is accounting, not memory in use.
- **Memory pools.** Once direct memory is used up, `sceKernelMemoryPoolExpand` is refused with `0x8002000c` for every size down to 2 MiB; after the direct memory is released it succeeds. Pools come out of the same budget.
- **The other direct memory devices.** SWRR's libkernel also opens `/dev/dmem0` and `/dev/dmem2`. dmem0 refuses even its size query; dmem2 is 0 bytes.

**What sets it.** Read on a jailbroken 12.50 Pro during development only; nothing ghostldr does needs or uses a jailbreak. The kernel's default limits per process type, which ShellCore copies into a new application's budget (the seventh report), are, for direct memory:
- type 0, games: 5888 MiB;
- type 1: 1792 MiB;
- type 2, system: 684 MiB.

So a game's budget starts far above what SWRR may allocate. The 2.5 GiB is the kernel's limit for all games together, set when SWRR starts: see [the ninth report](#the-ninth-report-the-kernel-caps-ps2-classics-at-25-gib).

**What it means.** From inside SWRR on a stock PS4, nothing raises it:
- the budget calls are refused;
- the limit's sysctl can't be read;
- the compiler can't allocate direct memory;
- pools and the other devices draw on the same budget or none.

A title gets what is left of the 2.5 GiB once ghostldr has released the emulator's memory, about 2.4 GiB. Ghost of Tsushima needs about 3.9 GiB, so it stops before it starts, with a hint from `run.py` that says why.

## The ninth report: the kernel caps PS2 Classics at 2.5 GiB

Read on a jailbroken 12.50 PS4 Pro during development only, with SWRR (EU) running, from the kernel's live state and its code. Nothing ghostldr does needs or uses this.

**SWRR's own budget isn't the limit.** The budget its process belongs to allows 5888 MiB of direct memory, the default for games; it had 1283 MiB in use, the same 1283 MiB SWRR holds.

**The limit is the kernel's pool for games.** Besides each budget, the kernel keeps one direct memory pool per process type, with a limit and a usage, and every allocation of a game process is charged to the games' pool:

| | Games (type 0) | Type 1 | System (type 2) |
|---|---|---|---|
| Direct memory limit, SWRR running | **2560 MiB** | 1792 MiB | 684 MiB |
| Direct memory limit, GTA V running | **4608 MiB** | 1792 MiB | 684 MiB |
| In use (SWRR running) | 1283 MiB | 0 | 472 MiB |

So SWRR may allocate 2560 − 1283 = 1277 MiB more, which is exactly what it gets.

**Who sets it, and from what.** When an application's main process starts (`kern_exec.c`), the kernel sets the games' pool limit from that process's credentials:
- **2560 MiB** if the credentials carry one capability bit and one attribute bit. SWRR's do; a normal game's (GTA V's) don't.
- **768 MiB** for another attribute bit.
- **Otherwise the whole 4608 MiB** range.

SWRR's credentials compared with GTA V's on the same console:

| | Authority ID | Capabilities | Attributes |
|---|---|---|---|
| SWRR's eboot | `0x3100000000000002` | `2000038000000000 200000000000ff00` | `40004000c0800000` |
| SWRR's compiler | `0x3100000000000002` | `2000038000000000 400000000000ff00` | `40004000c0800000` |
| GTA V's eboot | `0x3100000000000001` | `2000038000000000 000000000000ff00` | `4000400040000000` |

The capabilities and attributes come from the executable's signed authentication info, which Sony signs with the title: SWRR's marks it as a PS2 Classic.

**Nothing afterwards raises it.**
- The kernel keeps the value set at the start. Every later recomputation of the pool, on a Pro-mode or memory-state change, keeps the limit at or below it.
- The only other way to change it is the write-only sysctl `kern.dmem.game_budget_limit` ("Set game budget limit"). Its handler requires a system process's capability, which neither SWRR nor its compiler has, and it never goes above the limit the console allows.
- No system module of 12.50 calls `sceKernelSetGameDirectMemoryLimit`. Two statically linked copies in `mini-syscore.elf` and `SceSysAvControl.elf` have no callers, so the setting appears to be left for development consoles.

**So, on a stock PS4, SWRR's 2.5 GiB is final:**
- the cap is set by the kernel when SWRR starts, from SWRR's signed executable;
- a game can neither change its credentials nor write the setting;
- what it may still use is what the kernel left: about 2.4 GiB for a title once ghostldr has released the emulator's memory.

A PS5 doesn't apply this cap to PS4 software, which is why SWRR gets the whole 4.5 GiB there.

## What is verified

- From the log and SWRR's executable:
  - the address and size of the fatal write;
  - the identity of the faulting SWRR thread, its quit word, and the heap it allocates from;
  - that its server is up on the PS4;
  - the missing syscall gadget.
- Offline (`ghostldr/tests/test_ps4.py`):
  - The real `arena.c` and `ps4.c`, against the PS5 and PS4 layouts from the logs and a fake compiler process:
    - the PS5 placement is unchanged;
    - the v0.1.1 rule faults exactly where the PS4 did;
    - the new rule doesn't;
    - with a compiler that writes nothing, other memory, or only VU1, the boot uses a JIT block or VU1 (for a small title), or stops cleanly;
    - the second report's layout (core text 176 KiB, small area `914b14000-914b1c000`): the same regions, nothing probed in the core's text or data, and when the compiler checks fail, nothing probed or written.
  - The real `jitrpc.c` against a fake compiler thread: the checks and probes end whether the compiler's bytes go astray, the descriptor is wrong, stale bytes wait, or the compiler never answers. A PS5 never reads the clock. Like a PS4, the fake runs a system call only from its own number's stub: every number gets its own, and a number without one isn't called.
  - `ghostldr.lua` under a fake Luac0re: the PS4 flag, the firmware, the gadget and the table of stubs at the offsets `ghost.h` has, and the PS5 values unchanged.
  - `run.py probe --diag`'s flags.
- On a 12.02 PS4, with v0.6.3: SWRR's direct memory budget is about 2.5 GiB, and memory released from SWRR counts against it (the sixth report).
- On a 9.60 PS4 and a 12.50 PS4 Pro (base mode): the same 2.5 GiB; the compiler process can't open its direct memory device; memory pools draw on the same budget (the eighth report).
- On a 12.50 PS4 Pro, from the kernel: the games' direct memory pool limit is 2560 MiB while SWRR runs and 4608 MiB while GTA V runs; it is set when the application's main process starts, from its credentials (the ninth report).
- On a 12.02 PS4, with v0.6.1 (a boot of Ghost from saved data, the fourth report):
  - the compiler checks, each system call through its own stub;
  - the buffer probes: the EE and IOP buffers written past 16 KiB, VU0 and VU1 past 32 KiB, and the small area;
  - Ghost's modules placed past the EE buffer's header and relocated, its TLS sites patched, and its hooks in place;
  - a new JIT block for the extension (384 KiB), and the extension's self-test;
  - the SystemServicesThread stopped when asked (40 ms), and SWRR's buffers released;
  - Ghost's main thread, module starts and first second: video out, the GPU's queues, trophies, NP and its first packs.
- On a 13.60 PS5, with these changes (2026-10-22):
  - `run.py probe --diag` ran to the end without crashing SWRR.
    - Buffers: as above.
    - JIT blocks: 64 KiB and 768 KiB work at a fixed address; 1 MiB is refused.
    - Threads: as above.
    - The PS4 checks pick the address a PS5 boot uses for the first module (`0x900a20000`).
  - A Ghost boot on a fresh SWRR placed and planned exactly as before the change:
    - libSceFios2, libc, NpToolkit2 and eboot.bin at `0x900a20000`, `0x900bb0000`, `0x900cd0000` and `0x900d90000`;
    - the extension at `0x904f34000`;
    - 4532 MiB free, reporting 4520 MiB;
    - 8 logging stubs.

    The only difference is the stub area, now at `0x900a0c000` because the core grew to 208 KiB. Ghost reached the title screen, and CONTINUE loaded into controllable gameplay. No unresolved or denied calls; no crash in the core's log or the kernel log.
  - A boot through the selector (`--select`) placed it at `0x900a20000`, cleared it, then placed Ghost as above, and Ghost reached its title screen.

## What is not verified

- Firmware other than 9.60, 12.02 and 12.50.
- That a base PS4's kernel sets the games' pool the same way: the kernel was read only on a 12.50 Pro. The base PS4s measured (9.60, 12.02) give SWRR the same 2560 MiB.
- How much new JIT memory a PS4 allows. The extension's 384 KiB block worked; a 13.60 PS5 allows 768 KiB in all. The PS4 compiler process's flexible memory was 248.1 of 249.6 MiB. A 25 MiB fallback block is very unlikely to work.
- That no other SWRR thread uses the released buffers on a PS4. None did in the 3 s Ghost ran.
- That Luac0re's thread cancels work on a PS4.
- That a new JIT block maps at a fixed address on a PS4. On a 13.60 PS5, read-only views of the emulator's own JIT objects map with MAP_FIXED anywhere, but this hasn't been checked for new blocks. The fallback needs one over the EE buffer.
- Everything past Ghost's first second on a PS4: graphics on screen, audio, saves and input.
- A 14.10 or 14.20 PS5 with these changes. Only a 13.60 PS5 was booted.
