# Runtime extension

The core has to fit in 240 KiB of the emulator's IPU JIT buffer, and the Lua payload that carries it is capped at 500 KB. The extension, `build/ghostldr/ext.bin`, holds what doesn't fit there. run.py serves it from the computer at boot. It carries:

- **the NID → name table:** every name in `build/nid/known_names.txt` and `ghostldr/data/extra_names.txt`, about 42,000. The core resolves host imports by name with it, so the core no longer carries a per-title table.
- **extension code:** a freestanding x86-64 PIC image, compiled with the core's flags. It registers HLE functions (matched by library and NID) and per-library policies (aliases, no host fallback, stub return values).

## Boot

1. `run.py boot` serves `ext.bin` on **TCP 18195** (the UDP log is on 18194). It passes the blob's size, CRC-32 and SHA-256 to `ghostldr.lua`.
2. `ghostldr.lua` creates a TCP socket (through the compiler, as `create_socket` does for AF_INET) and connects it to the computer. It passes the socket, size and CRC in `struct boot_ctx` (`ext_fd`, `ext_size`, `ext_crc`), plus the first 24 bytes of the SHA-256 (`ext_sha256`, the last field before the profile slot). The Lua side never holds the blob.
3. **`ext_fetch`** (`src/ext.c`) runs first in `ghost_boot`. It reads the blob and checks the CRC, then the SHA-256 pin when the context carries one (a CRC is a checksum, not a fingerprint: a swapped blob with a recomputed CRC passes the first screen and fails this one), then the header, API version and ABI, the bounds, and the NID table order.
   - The blob goes in app flexible memory, which the title never gets (its flexible requests become direct memory). The direct memory the loader and the memory plan see is therefore laid out as before. Direct memory is used only if the flexible map fails.
   - **The release's `ghostldr.lua`** receives the blob itself, in a bundle sent to the PS5 ([architecture.md](../../../docs/architecture.md#two-file-boot)), into flexible memory it maps, and passes its address instead of a socket (ctx flag `CTX_FLAG_BUNDLE`). `ext_fetch` then takes it where it lies, checks it the same way, and unmaps it when it is done with it.
   - **Without a valid extension the boot stops here** (`ghost_boot` returns -7). Nothing has touched the title or SWRR's state yet, so you can fix it and send `run.py boot` again in the same SWRR session. Without the NID table nothing could be resolved by name, so there is no fallback.
4. **`ext_start`** runs once the title's modules are placed, **before the first relocation**, so every import binds with the extension in place. Module starts run later still.
   - It places the code at the top of the small RX area after the core (`arena_small_alloc_top`). The text is written through the compiler like module text, and the data pages after it are punched RW like module data.
   - A new JIT block from the compiler process is used instead when the image won't fit there with 16 KiB left for stubs, or with `run.py --ext-jit`.
   - It applies the relocations and calls `ext_init(&g_ghost_api)`, then runs the self-test (`selftest.c`): name, NID-only and alias lookups, the policy, and calls. A failure returns -8 before relocation.
5. Each import binds to the first of:
   1. the **extension**, by library and NID, so imports with no known name can be overridden too. Library aliases from the policies count as the same library. The extension supersedes the core;
   2. the core's overrides (`hle.c` `g_hle`, `hle_sys.c` `g_sys`), by library and name;
   3. the title's own modules;
   4. if the extension's policy for the library has `EXT_LIB_NO_HOST`: a stub returning the policy's `stub_ret`, logged as `denied:` and `DENIED call: ...`;
   5. host modules by name, using the full table;
   6. a logging stub (`UNRESOLVED call ...`). It returns 0, or the library policy's `stub_ret` when there is one.
   - **App roots on a USB drive or a folder of titles** ([usb.md](../../../docs/usb.md)) need the extension earlier, before the title's modules load. There `ext_approot_init` places the code in a new JIT block, while SWRR's emulator memory is still allocated, so its address can't collide with module placement later. It then calls the blob's second entry, `ext_early` (`src/approot.c`), and `ext_start` only runs `ext_init`. A boot that stops there gives the JIT block back (`ext_abandon`): a 13.60 PS5 has about 768 KiB of new JIT memory per SWRR session. ghostfs and a title's own directory never take this path.
6. **`ext_boot_done`** runs after relocation. It frees the blob (about 2.2 MiB: the NID table and the image copy) before the memory plan is computed. The extension's code and data stay. `nid_to_name` returns NULL from then on, so stubs keep their own copies of names.
7. **At run time,** `sceKernelDlsym` on a host module (the module family, `ext/src/hle_module.c`) first asks the extension, by the NID of the name. If the module's library has `EXT_LIB_NO_HOST` and the extension lacks the function, the call is refused with ENOENT rather than given SWRR's. A title prx loaded after boot (`sceKernelLoadStartModule`, `sceSysmoduleLoadModule`) binds the same way; the names of its host imports are kept from boot (`nid_keep`) for the sce_module files the boot did not load.
8. **Sysmodule loads.** The sysmodule family (`ext/src/hle_sysmodule.c`) owns `sceSysmoduleLoadModule`, `IsLoaded` and `Unload` and their Internal forms: IsGame ids start the title's own module, dialog ids go to the dialogs family (`dialog_sysmod_virtual`), DiscMap answers without loading, and other ids reach SWRR's library with its real result. A failure is passed to `ext_sysmodule_failed(id, rc)`, which calls every family's `sysmodule_failed` hook (`EXT_FAMILY_HOOKS`); the core's `ext_note_sysmodule` does the same for callers in the core.

With the core at 208 KiB, the small area has 48 KiB. The extension gets up to 32 KiB of it (text and data, each rounded to 16 KiB). The image holds code, rodata and data only: `ext.ld` keeps the relocation and symbol tables out of it (`mkext.py` reads them from the ELF). With the media and peripheral families it needs more than that, so it goes in a new JIT block.

## Files

| File | What it is |
|---|---|
| `include/ghost_api.h` | The interface: `struct ghost_api` (core services), `struct ext_hle_ent` / `ext_lib` / `ext_family` / `ext_exports`, and the blob header `struct ext_blob`. |
| `ext/include/ext.h` | Included by every extension file. Maps the core's names (`logf`, `K`, `g_ctx`, `host_real_in`, ...) to the API, and defines `EXT_FAMILY` and `EXT_FAMILY_LIBS`. |
| `ext/src/ext_main.c` | `ext_init`: walks the families (linker section `ext_families`), runs their `init`, merges policies and entries (computing NIDs from names), drops duplicates, and answers the core's `lookup` and `lib_policy`. |
| `ext/src/selftest.c` | The family the core self-tests with (`libGhostldrExt`, alias `libGhostldrExtAlias`, which no title imports). |
| `ext/include/hle_mode.h`, `ext/src/hle_mode.c` | Per-library modes (PASS, MASKED, VIRT, chosen once and logged) for libraries SWRR may refuse or already own, and the trampolines that route a call to the host or to a virtual body with the title's registers and stack untouched. No family of its own. |
| `ext/src/hle_homebrew.c`, `ext/src/hle_tsd.c` | What OpenOrbis homebrew calls beyond the kernel families (docs/compatibility.md, Homebrew): stdout/stderr `writev` to the log, `TIOCGWINSZ`, `_exit`/`raise` logged, FreeType faces from ghostfs (`homebrew`); POSIX thread-specific data of its own, since SWRR's faults on a homebrew thread, with destructors run from the thread-exit hook (`tsd`). |
| `ext/src/hle_libc_heap.c`, `ext/src/hle_libc_stdio.c` | For titles without a libc of their own (no title module exports `malloc`; both families are off otherwise): `libSceLibcInternal`'s malloc family in a heap in the title's flexible memory (`libc_heap`); its stdout/stderr calls to the log and read-only `FILE`/`DIR`s for ghostfs files (`libc_stdio`). |
| `ext/src/hle_media.c`, `ext/src/hle_peripheral.c` | Capture, sharing and voice libraries (GameLiveStreaming, ScreenShot, ShareUtility, SharePlay, Remoteplay, VideoRecording, Companion, ContentExport, Voice, VoiceQoS, Audio3d, Rudp, RazorCpu) and peripherals (Camera, Move, MoveTracker, Hmd, VrTracker, Usbd, Keyboard, Mouse, SystemGesture). |
| `ext/ext.ld` | Text at 0, data at the next 16 KiB boundary. |
| `src/ext.c`, `src/ext_api.c` | The core side: fetch, verify, place, relocate, lookups, dlsym; the API table. |
| `ext/src/hle_kernel.c`, `hle_module.c`, `hle_sysmodule.c`, `hle_file.c` (+ `kmod.h`) | The kernel families (research spec kernel-modules.md): libc and process identity, title modules (load, start, dlsym, module queries), the sysmodule router, and file calls on ghostfs fds. |
| `ext/src/approot.c`, `hle_usbstorage.c` | App roots on USB drives and folders of titles, called through `ext_early` before the title's modules load: the drive mounted into SWRR's sandbox by AutoMounter (libSceUsbStorage's unexported RequestMap2), the library, the title's ID and save prefix, and `/app0` kept read-only (`hle_file.c`). `hle_usbstorage.c` keeps a title's own `sceUsbStorageInit`/`Term` off that session ([usb.md](../../../docs/usb.md)). |
| `ext/include/usbstore.h`, `ext/src/usb_bot.c`, `blk.c`, `part.c`, `fsutil.c`, `exfat.c`, `fat32.c`, `usbfs.c`, `usb_sce.c` | Route B, built only with `make USB_RAW=1`: the extension's own USB storage stack (Bulk-Only Transport and SCSI, a block cache, MBR/GPT, read-only exFAT and FAT32, and the `/app0` provider, `struct appfs_ops`, with ghostfs's semantics), tried when the mount fails. Off by default: SWRR's libSceUsbd lists no devices on a 13.60 PS5. Everything but `usb_sce.c` (libSceUsbd and the environment) builds for the computer too (`tests/test_usb.py`). |
| `ext/include/elfldr.h`, `ext/src/elfldr.c`, `elfldr_sys.c`, `payload.c` | The ELF loader, run with no title (`run.py elfldr`, [elfldr.md](../../../docs/elfldr.md)): the TCP service, the console side (JIT object views, direct memory, threads, the imports payloads get instead of SWRR's), and the payload formats. With the release's `ghostldr.lua` also the countdown on the TV, `boot`, `forget` and `log`, and bundles sent to its port, handed back to `ghostldr.lua` ([architecture.md](../../../docs/architecture.md#the-bundle-cache)). `elfldr.c` and `payload.c` build for the computer too (`tests/test_elfldr.py`). |
| `tools/mkext.py`, `tools/gen_nids.py`, `tools/elf64.py` | Build `ext.bin` from `ext.elf` and the name lists. |
| `ext/elfldr_only.c` | The ELF loader's own extension, `ext-elfldr.bin` (in the release's `ghostldr-elfldr.bin`): the families a payload binds to (`ELFLDR_EXT_SRCS` in the Makefile), linked with this in place of the FTP server, the app roots and the sysmodule router, and packed with no NID table (`mkext.py --no-nids`). |
| `tools/check_ext.py` | Offline checks (see below). |
| `tests/test_ext.py`, `tests/ext_host/` | Runs the core's real `ext.c`, `resolve.c`, `elf.c` and `nid.c` on the built blob as x86-64 (Rosetta on Apple silicon), stubs included. |
| `tests/test_homebrew.py`, `tests/ext_native/` | Builds the homebrew families from source for the computer (no x86-64 needed) and runs them against a fake core; `ext_native/ext.h` registers each family with the harness. |

## Adding a family

One file per library family. Each file registers itself, so adding one never touches a shared file.

1. Create `ghostldr/native/ext/src/hle_<family>.c`:

   ```c
   #include "ext.h"

   static s32 hle_sceFooBar(s32 a, void *out)
   {
       static s32 (*real)(s32, void *);
       if (!real)
           real = host_real_in("libSceFoo", "sceFooBar");
       logf("sceFooBar(%d)", a);
       return real ? real(a, out) : 0;
   }

   static s32 hle_foo_unnamed(void) { return 0; }

   static int foo_init(void) { return 0; }   /* optional: nonzero drops the family */

   static const struct ext_hle_ent k_hle[] = {
       {"libSceFoo", "sceFooBar", hle_sceFooBar},
       {"libSceFoo", NULL, hle_foo_unnamed, "fJgP+wqifno"},   /* by NID: no known name */
   };

   /* optional: aliases, no host fallback, the value its stubs return */
   static const struct ext_lib k_libs[] = {
       {"libSceFoo", "libSceFooCompat libSceFooForToolkit", EXT_LIB_NO_HOST, (s32)0x80AA0001},
   };

   EXT_FAMILY_LIBS("foo", k_hle, k_libs, foo_init);   /* EXT_FAMILY("foo", k_hle, foo_init) without policies */
   /* or EXT_FAMILY_HOOKS("foo", k_hle, k_libs, foo_init, foo_sysmodule_failed): also hear of failed sysmodule loads */
   ```

2. Write it like core code. Use `logf`, `host_real`, `host_real_in`, `K.*`, `g_ctx`, `g_main_mod`, `page_alloc`, `arena_alloc`, the `gfs_*` and `vfs_*` calls, `spin_lock` and the rest through the macros in `ext.h`. `rt.c` (memcpy, strcmp, fmt, ...) is linked into the extension.
   - A core function that `ext.h` doesn't map is an undefined symbol, and the link fails. To expose one, append a member to `struct ghost_api` and fill it in `src/ext_api.c`. Then add the macro to `ext.h`.
   - Never reorder or remove members. Bump `GHOST_API_VERSION` in `ghost_api.h` only for an incompatible change.
3. Entries and policies:
   - An entry matches imports whose NID is the NID of `name`, or `nid` when given. `name` may be NULL for a NID-only entry, and a name not in the name lists still works.
   - An entry matches imports from `lib` or from any library in the same policy's `aliases`, in either direction. Without a policy, only `lib` matches exactly.
   - `EXT_LIB_NO_HOST` keeps a library away from SWRR's own copy. Use it for libraries that would act on SWRR's state: NpTrophy, NpManager, NpAuth, NpCommerce, SaveData. Imports of the library that the extension doesn't implement get a stub returning `stub_ret`, e.g. `0x80551622` (trophy NOT_SUPPORTED) or `0x809F000B` (save data INTERNAL). It also refuses `sceKernelDlsym` on the library's module.
   - Declare each library in one family only. A second declaration, or an alias already claimed, is ignored with a log line (`check_ext.py` warns).
4. Rules:
   - No thread-local variables.
   - Only one `EXT_FAMILY` / `EXT_FAMILY_LIBS` per file.
   - Static data, string tables and function pointers are fine; they're relocated.
   - The extension takes precedence over the core's own overrides, so a family can reimplement a core override without touching the core. Delete the superseded core row when convenient (it frees core space); `check_ext.py` lists them.
   - Imports that move to the extension (or to a denied stub) are counted as deliberate by `check_ext.py` (`--verbose` lists them); any other change in how one of Ghost's imports resolves fails the check.
5. Build and check:

   ```bash
   make -C ghostldr/native
   python3 ghostldr/tools/check_ext.py --log build/ghostldr/run.log   # --log: a run whose "unresolved" imports may be taken
   python3 -m unittest discover -s ghostldr/tests
   ```

   `check_ext.py` decodes `ext.bin` without running it: header, relocations, the NID table against the name lists, and every family, entry and policy, merged the way `ext_init` merges them. With `build/ghost/app0` present, it replays `resolve_import` for every import of the modules `ghost_boot` loads. Each must keep its name and its resolution stage, except imports that were stubs before. `--old-core-elf` compares with a core built before the extension existed.
6. On the console, the boot log shows `ext: N HLE functions and P library policies from M families`, one `ext:   lib:name (nid)` line per function (up to 32), and `ext: self-test ok`. The first 64 imports bound to the extension are logged as `ext: lib:name -> extension ...`.

## Blob format

See `struct ext_blob` in `include/ghost_api.h`. The header also records `ext_early`'s offset (`early_entry`, flag `EXT_BLOB_EARLY`). The layout is: a 96-byte header, the flat image, then the relocations as `{u32 offset, u32 addend}` (R_X86_64_RELATIVE only; `mkext.py` refuses anything else). Then come the NID keys (u64, ascending: the 64-bit value of the NID's 11 base64 digits), the u32 name offsets, and the NUL-terminated names.

The header records `GHOST_API_VERSION` and `GHOST_ABI`, a checksum of `ghost.h` and `ghost_api.h` that the Makefile computes. The core refuses a blob built against other headers, so ship `core.bin`, `core.json` and `ext.bin` from the same build.
