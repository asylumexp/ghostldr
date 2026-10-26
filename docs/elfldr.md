# ELF loader

In the PS5 scene, *elfldr* is a small server that takes an ELF over TCP (usually port 9021) and runs it. The usual one needs a kernel exploit, so it doesn't exist on current firmware. ghostldr has an ELF loader of its own that runs inside SWRR on a stock PS5 with no kernel exploit: send it a payload and it runs, with its output coming back on the same connection. Then send the next one, without restarting SWRR.

A payload gets SWRR's userland privileges and nothing more. There is no kernel memory access, no jailbreak and no faked one: a payload that asks for something SWRR may not do gets the error SWRR would get.

## Start it

You need the same setup as for a game: SWRR with Luac0re's Remote Lua Loader listening on TCP 9026 ([using-the-release.md](using-the-release.md)), and this repository or a release on the computer. Then:

```bash
python3 ghostldr/host/run.py elfldr
```

It sends the core with no title, serves the extension on TCP 18195 as for a boot, and shows the console's log. Once the console logs `elfldr: listening on TCP 9021`, payloads can be sent. `--elfldr-port` picks another port, `--no-build` uses a release's prebuilt core, and the log listener stops on Ctrl+C while the loader keeps running on the console. On a jailbroken console, whose own elfldr already listens on 9021, `run.py elfldr` says so: add `--elfldr-port 9022` (and send there).

While the loader runs, it holds the thread Luac0re's Lua runs on, so Luac0re's loader on 9026 takes no scripts. To give SWRR back to Luac0re (to boot a game, say), send `quit` once no payload runs; or restart SWRR.

## Send a payload

```bash
nc PS5 9021 < hello.elf
```

or, with the exit code passed on:

```bash
python3 ghostldr/host/elf_send.py hello.elf
```

`nc` needs no options: the loader reads the payload's length from its own headers, so a client that keeps its side open still gets its payload run. Lines from the loader start with `[elfldr] `; everything else is the payload's stdout and stderr, as it writes them. The connection closes when the payload has finished:

```
[elfldr] payload 2: ELF (OpenOrbis/PS4 ABI), 32 KiB of code at 20007f8000 (in the 8 MiB JIT object), 64 KiB of data, 16 imports
hello from payload2, running inside SWRR (pid 96)
hello: thread 1's thread-local counter ended at 101 (100 + 1)
hello: thread 2's thread-local counter ended at 102 (100 + 2)
hello: thread 3's thread-local counter ended at 103 (100 + 3)
hello: main's counter is still 100
hello: malloc(8 MiB) at 0x8821f0020, checksum 337920
hello: monotonic clock 417.743 s
hello: this line went to stderr
hello: exiting with code 7
[elfldr] payload 2 exited with code 7 after 0.254 s
```

The console's UDP log (`run.py elfldr`, or `run.py logs`) has the same output, as `payload 1: ...` lines.

Instead of an ELF, a client may send one line of text:

| Line | What it does |
|---|---|
| `status` | lists the payloads that run: threads, time, where their code is |
| `quit` | stops the loader and returns to Luac0re's loader; refused while a payload runs |
| `help` | lists these |

`elf_send.py --cmd status` sends one. Up to three payloads run at once (below). Each has its own connection for its output.

## Build a payload

### OpenOrbis

The [OpenOrbis PS4 toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) builds PS4 programs, and those are what runs here. Link as its samples do and send the ELF that `ld.lld` writes; `create-fself` isn't needed. Its `.self` or `.oelf` output loads too, but it sets the TLS segment's alignment to 32, and code that `ld.lld` laid out for a smaller alignment then finds its thread-local variables' initial values in the wrong place (they read as 0): send the ELF. `ghostldr/tests/elfldr/` has three samples and a Makefile:

```bash
make -C ghostldr/tests/elfldr OO_PS4_TOOLCHAIN=/path/to/PS4Toolchain
nc PS5 9021 < build/elfldr/hello.elf
```

A hello world is plain C:

```c
#include <stdio.h>

int main(void)
{
    printf("hello from SWRR\n");
    return 0;
}
```

```bash
TC=/path/to/PS4Toolchain
clang --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -isysroot $TC -isystem $TC/include -c hello.c
ld.lld hello.o -o hello.elf -m elf_x86_64 -pie --script $TC/link.x --eh-frame-hdr -L$TC/lib -lc -lkernel $TC/lib/crt1.o
```

`hello.c` in the samples also starts threads with thread-local variables, uses `malloc` and returns 7, which the last line reports. Its threads don't print: OpenOrbis's libc never locks stdout when the threads are libkernel's, so threads printing at once garble each other's lines.

### Sockets

`echo.c` is a TCP echo server on port 9025: `socket`, `bind`, `listen`, `accept`, `recv` and `send` as on any BSD.

```bash
nc PS5 9021 < build/elfldr/echo.elf     # its output comes back here
nc PS5 9025                             # type lines; "quit" stops it
```

SWRR can't make an internet socket itself on a PS5 at 8.00 or later (`socket()` fails with ENOSYS). The loader gives a payload's `socket()`, `sceNetSocket()` and libc's `__sys_socketex()` a socket made by the emulator's JIT compiler process and handed over, as Luac0re and ghostldr's FTP server do; the payload uses it like any other. Sockets a payload leaves open are closed when it finishes.

### Freestanding

A payload with no libc and no imports gets, in `rdi`, the same arguments ps5-payload-sdk's payloads get (`payload_args_t`), with no kernel access in them:

```c
struct el_args {
    int (*sys_dynlib_dlsym)(int handle, const char *name, void *out); /* 0: *out is the address */
    int *rwpipe, *rwpair;            /* NULL: no kernel R/W */
    long kpipe_addr, kdata_base_addr; /* 0 */
    int *payloadout;                 /* the exit code, if the payload sets it */
};
int payload_main(struct el_args *args);  /* its return value is the exit code */
```

`sys_dynlib_dlsym(-1, "write", &fn)` finds a function by name as an import would bind (handle -1: any module; a module handle: that one first). `freestanding.c` in the samples builds with LLVM alone (`make -C ghostldr/tests/elfldr freestanding`).

### Which ELFs are taken

| Taken | |
|---|---|
| an ELF with its own dynamic section | `ld.lld -pie` output; imports by name, from the libraries `DT_NEEDED` names |
| create-fself's OELF, or a fake-signed SELF | Orbis tables, read as a game's are |
| a freestanding ELF | no imports at all; relative relocations (RELA or RELR) only |

It must be position-independent x86-64 (`ET_DYN`), with code and writable data in separate 16 KiB pages (the OpenOrbis link script does this; otherwise `-z max-page-size=0x4000 -z separate-code`). An ELF that breaks a rule is refused with the reason, for instance:

```
[elfldr] payload 2 refused: linked at a fixed address (ET_EXEC): ... link it position-independent (-pie, or -static-pie)
```

## What a payload gets

- **Its own thread**, with a 2 MiB stack, `argc` 1 and `argv[0]` "payload*N*". Its threads (`pthread_create`, `scePthreadCreate`) each get its thread-local variables (up to 32 KiB of them).
- **Memory:** its data, and what it maps (`mmap`, `sceKernelMapFlexibleMemory`, so `malloc` too), is direct memory, of which SWRR has several GiB free; SWRR's own flexible memory is nearly used up. All of it is given back when the payload finishes.
- **Files** as SWRR sees them: its sandbox, `/app0` being SWRR's own files.
- **stdout and stderr** on its connection and the UDP log.
- **The end:** a payload has finished when none of its threads runs any more. Returning from `main`, `exit()` and `_exit()` end the thread that called them (not SWRR); `abort()` and `raise()` end it with 128 + the signal. A thread that faults (a bad pointer, say) is reported and ends; SWRR and the other payloads go on. The last line gives the exit code or the fault:

  ```
  [elfldr] payload 3 crashed after 0.051 s: signal 11 at +4123 (address 0)
  ```

## What doesn't work, and why

- **Nothing that needs the kernel.** No kernel R/W, no jailbreak, no other process's memory, no `ucred` changes: the PS5 kernel isn't exploited, and the loader doesn't pretend it is. Calls that need privileges SWRR lacks fail as they do in SWRR.
- **ps5-payload-sdk payloads are refused.** Their startup code (`crt1.o`) needs kernel R/W before `main`, they link PS5 libraries (`libkernel_web`) and their code segment is writable and executable. Build such a program for the PS4 ABI with OpenOrbis instead, or freestanding.
- **PS4 ABI only.** SWRR is a PS4 program running in the PS5's backward-compatibility mode, where PS5 libraries don't load (`libSceAgc` fails with ENOEXEC).
- **Fixed-address ELFs (`ET_EXEC`)** are refused: SWRR has executable memory only where ghostldr maps it.
- **No new executable memory for the payload.** Code runs only from views of the emulator's JIT objects, written before the payload starts; `mprotect(PROT_EXEC)` fails on a stock console, so a payload can't JIT or load more code itself.
- **Memory limits.** A payload's code and read-only data take one of the emulator's JIT objects: 16, 8 and 8 MiB, the smallest that fits (the 32 MiB one holds ghostldr's extension, the IPU one the core). So at most three payloads run at once, each with up to 16 MiB less 32 KiB of code; the fourth is refused until one finishes. The JIT objects cost nothing of the 768 KiB of new JIT memory a 13.60 PS5 has left.
- **No killing a payload.** `exit()` can't stop other threads, so a payload whose threads never end keeps its JIT object until SWRR restarts.
- **Signal handlers** are ghostldr's: a payload must not install its own for SIGSEGV, SIGBUS, SIGILL or SIGFPE.
- **Graphics and the controller** aren't what the loader is for: Luac0re's loader dialog stays on screen and SWRR keeps its display. A payload may still try.
- **A PS5 and Luac0re in SWRR only.** On a PS4, or Jak X with UltraC0re, `run.py elfldr` refuses.

## How it works

`run.py elfldr` sends ghostldr's core with the context flag `CTX_FLAG_ELFLDR` and the port. The core fetches the extension (it carries the NID table and the HLE families), places it, and calls its ELF loader (`ghostldr/native/ext/src/elfldr.c`, `elfldr_sys.c`, `payload.c`), which serves on the boot thread until told to quit.

- **Executable memory.** Each payload takes one of the emulator's JIT objects (never the IPU object that holds the core). Its code is written through the compiler process at the object's end, and the whole object is mapped read-execute in a range reserved for the payload above 0x10_0000_0000, so that the code ends where the payload's data begins. The data is direct memory mapped right after the view. When the payload finishes, the view, the data and the range go back, and the object is free for the next payload. Nothing is ever writable and executable at once.
- **Imports** bind as a game's do (the extension, then the core's overrides, then SWRR's modules by name), with a few of the loader's own first: `_exit`, `exit`, `abort` and `raise` end the thread; `socket`, `__sys_socketex` and `sceNetSocket` fall back to the compiler; `accept` and `close` keep track of sockets; `mmap`, `munmap` and the flexible-memory calls use direct memory. Imports that are found nowhere bind to a stub that logs the call and returns 0; the first line lists them.
- **TLS:** each payload thread's block holds that payload's thread-local image (the same mechanism a game's threads use, with the `fs:[0]` reads in its code patched to read the block).
- **Threads** are counted per payload; a fault in one ends that thread through the crash handler instead of stopping it.

## Tested on a PS5

On a 13.60 PS5 (EU SWRR, Luac0re 2.4b), the four samples ran back to back and at the same time: `hello` (TLS, threads, `malloc`, exit code 7), `echo` (a client on 9025 got its lines back), `freestanding` (42) and `fault` (reported, its thread ended, `echo` running beside it went on serving), plus `hello.self`, a ps5-payload-sdk build (refused), 15 `hello`s in a row with no memory left behind, and `quit` followed by `run.py elfldr` again in the same SWRR session, three times. That console had a kernel exploit and kstuff loaded for other reasons; the loader logs at start whether `mprotect(PROT_EXEC)` works on ordinary memory (there it did; a stock kernel refuses) and never uses it: payload code runs only from the JIT object views, which is how ghostldr runs games on stock consoles.

## Tests

`ghostldr/tests/test_elfldr.py` runs offline: the payload formats, relocations, TLS sites and refusals (`payload.c` and `elf.c` built for the computer), the samples, and the service over real sockets with a loader that runs nothing (`elfldr.c` built for the computer), and `elf_send.py` against it.

```bash
python3 -m unittest discover -s ghostldr/tests -p test_elfldr.py
```
