/*
 * The game selector: a runtime module (ghostldr/native/selector, served by
 * run.py as build/ghostldr/selector.bin) that runs before the title loads
 * and leaves nothing behind (interface: sel_api.h).
 *
 *  - The blob comes over the TCP socket ghostldr.lua connected (ctx sel_fd)
 *    into the compiler-visible staging buffer: no allocation.  From a bundle
 *    (CTX_FLAG_BUNDLE) it is copied there from where ghostldr.lua received it.
 *  - Its image (read-only code and data) is relocated where it lies in the
 *    staging buffer, for the address it will run at, then copied through the
 *    compiler into the emulator's JIT code buffer, where the title's modules
 *    are placed afterwards.
 *  - sel_main(&g_ghost_api, title) runs on this thread.  The module allocates
 *    and frees its own memory (whole direct-memory blocks) and closes what it
 *    opened.
 *  - Afterwards the image is overwritten with zeros.
 */
#include "sel_api.h"

extern const struct ghost_api g_ghost_api;
#define COLD GHOST_COLD /* boot-time code: small beats fast */

COLD static u32 crc32(const u8 *p, u64 n)
{
    u32 c = ~0u;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = c >> 1 ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

COLD int sel_run(bool show, char title[16])
{
    struct boot_ctx *c = g_ctx;
    if (c->size < __builtin_offsetof(struct boot_ctx, sel_crc) + 8 || (s64)c->sel_fd < 0)
        return -1;
    int fd = (int)c->sel_fd;
    /* a bundle (CTX_FLAG_BUNDLE): ghostldr.lua's copy, unmapped here */
    const u8 *mem = (c->flags & CTX_FLAG_BUNDLE) ? (const u8 *)c->sel_fd : NULL;
    c->sel_fd = (u64)-1;
    u64 n = c->sel_size, got = 0, lo = 0, hi = 0;
    u8 *b = (u8 *)c->staging;
    const struct sel_blob *h = (const void *)b;
    if (mem) {
        if (show && n <= c->staging_size)
            memcpy(b, mem, got = n);
        K.munmap((void *)mem, ALIGN_UP(n, 0x4000));
    } else {
        while (show && n <= c->staging_size && got < n) {
            ssize_t r = K.read(fd, b + got, n - got);
            if (r <= 0)
                break;
            got += (u64)r;
        }
        K.close(fd);
    }
    if (!show) /* the title is known: no need for the module */
        return -1;
    /* mksel.py checked the image; the CRC proves this is that file */
    bool ok = got == n && crc32(b, n) == (u32)c->sel_crc && h->magic == SEL_BLOB_MAGIC && h->abi == GHOST_ABI &&
              h->format == SEL_FORMAT && h->header_size + (u64)h->image_size + (u64)h->reloc_count * 8 == n;
    /* the header's fields before arena_init: on a PS4 its probes copy
     * through the staging buffer's first bytes, over the header */
    u64 hsize = h->header_size, size = h->image_size, entry = h->entry, nrel = ok ? h->reloc_count : 0;
    if (!ok || arena_init() || arena_code_region(&lo, &hi)) {
        logf("sel: no usable selector (%lu of %lu bytes, ABI %x, core %x)", got, n, got >= sizeof(*h) ? h->abi : 0,
             GHOST_ABI);
        return -1;
    }
    /* where boot.c places the first module (arena_code_region skips the
     * buffer's first 16 KiB: writing there kills the compiler call) */
    lo = ALIGN_UP(lo, 0x10000);
    u64 w = arena_wr(lo, size); /* lo, except on a PS4 (arena.c) */
    if (!w || lo + size > hi)
        return -1;
    const u32 *rel = (const u32 *)(b + hsize + size);
    for (u64 i = 0; i < nrel; i++) {
        if (rel[2 * i] > size - 8 || rel[2 * i + 1] >= size) {
            logf("sel: relocation %lu (%x -> %x) outside the image", i, rel[2 * i], rel[2 * i + 1]);
            return -1;
        }
        *(u64 *)(b + hsize + rel[2 * i]) = lo + rel[2 * i + 1];
    }
    logf("sel: placing %lu KiB at %lx, entry +%lx, %lu relocations", size >> 10, lo, entry, nrel);
    jit_call(c->jit_memcpy, 0, w, (u64)b + hsize, size, 0, 0, 0);
    logf("sel: placed (first qword %lx), calling", *(volatile u64 *)lo);
    memset(title, 0, 16);
    int pick = ((sel_main_fn)(lo + entry))(&g_ghost_api, title);
    memset(b, 0, size); /* clear the module's image */
    jit_call(c->jit_memcpy, 0, w, (u64)b, size, 0, 0, 0);
    logf("sel: module at %lx (%lu KiB) cleared; chose %d", lo, size >> 10, pick);
    return pick;
}
