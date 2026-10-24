/*
 * The PS4 diagnostic (run.py probe --diag; docs/ps4.md says what to send
 * back).  It logs what the loader needs to know about a console and boots
 * nothing: no memory is released, no module placed, the host's threads are
 * only looked at.  The checks are the PS4 boot's own (arena.c with ctx flag
 * CTX_FLAG_PS4, set here on any console), so a PS5 run shows them passing.
 *
 *  1. platform, firmware, Pro mode, memory, the direct/flexible census;
 *  2. the emulator's JIT buffers: this process's map of them, the compiler
 *     process's map as its sceKernelVirtualQuery lists it (without the JIT
 *     mappings, on a PS5), and per buffer whether the compiler writes it past
 *     its first 16 KiB (probes that return EFAULT rather than crash);
 *  3. the host's threads (host.c): alive (a blocked thread counts) and name;
 *     the loader's scePthreadCancel guess; SWRR's heap and what a boot would
 *     release;
 *  4. writes through the compiler, each put back: the shared scratch, the
 *     free small area after the core, each buffer's ends;
 *  5. new JIT blocks of 64 KiB to 32 MiB, mapped here into a reserved range:
 *     created, a function written and called, destroyed (stops at the first
 *     failure);
 *  6. last: each buffer's first 16 KiB, read and written through the
 *     compiler by the same probes.
 */
#include "ghost.h"

#define COLD GHOST_COLD
#define MAX_MAP 512

void host_dmem_census(void);

static void *lk(const char *name) { return host_dlsym((u32)g_ctx->libkernel_handle, name); }

COLD static void diag_platform(void)
{
    struct boot_ctx *c = g_ctx;
    u64 fw = c->size >= __builtin_offsetof(struct boot_ctx, fw) + 8 ? c->fw : 0;
    struct {
        u64 size;
        char str[0x1c];
        u32 ver;
    } sw = {sizeof(sw), {0}, 0};
    int (*swver)(void *) = lk("sceKernelGetSystemSwVersion");
    int (*neo)(void) = lk("sceKernelIsNeoMode");
    int rc = swver ? swver(&sw) : -1;
    sw.str[sizeof(sw.str) - 1] = 0;
    logf("diag: %s; the loader says %s, firmware %lx.%02lx; system software '%s' (%x, rc %x); Neo mode %d",
         g_hostgame->name, (c->flags & CTX_FLAG_PS4) ? "PS4" : "PS5", fw >> 8, fw & 0xff, sw.str, sw.ver, rc,
         neo ? neo() : -1);
    off_t s = 0;
    size_t avail = 0, flex = 0;
    K.avail_direct(0, (off_t)K.direct_size(), 0x4000, &s, &avail);
    K.avail_flex(&flex);
    logf("diag: direct memory %lu MiB (%lu MiB free), flexible free %lu MiB", (u64)K.direct_size() >> 20,
         (u64)avail >> 20, (u64)flex >> 20);
    logf("diag: eboot %lx, bridge %lx, IPU buffer %lx, core end %lx, staging %lx+%lx, scratch %lx", c->eboot_base,
         c->reserved[1], c->reserved[2], c->reserved[3], c->staging, c->staging_size, c->oob_scratch);
    /* the gadget's bytes are not read here: system code in a PS5's compiler
     * is execute-only and reading it kills the host (ghostldr.lua checks them on
     * a PS4, where they are readable) */
    logf("diag: compiler: memcpy %lx, VirtualQuery %lx, syscall gadget %lx%s, JIT shm %lx/%lx/%lx", c->jit_memcpy,
         c->reserved[0], c->jit_syscall, c->jit_syscall ? "" : " (none: new JIT blocks impossible)", c->jit_shm_create,
         c->jit_shm_alias, c->jit_shm_map);
    /* a PS4 runs each system call only from its own stub (jitrpc.c) */
    const u64 *w = (const u64 *)c->jit_sysw;
    u32 n = 0;
    for (; w && n < 64 && w[2 * n]; n++)
        logf("diag: compiler libkernel stub for system call %lu: %lx", w[2 * n], w[2 * n + 1]);
    if ((c->flags & CTX_FLAG_PS4) && !n)
        logf("diag: no libkernel stubs for the compiler: its system calls are refused on a PS4");
}

COLD static void log_map(const char *who, const struct vmq *q)
{
    char name[33];
    memcpy(name, q->name, 32);
    name[32] = 0;
    logf("diag: %s %lx-%lx %7lu KiB prot %x type %d flags %x off %lx '%s'", who, q->start, q->end,
         (q->end - q->start) >> 10, q->prot, q->type, q->flags, q->offset, name);
}

/* this process's map from the bridge to past the VU heaps, every entry */
COLD static void diag_main_map(void)
{
    struct vmq q;
    u64 lo = g_ctx->reserved[1] ? g_ctx->reserved[1] : 0x900000000, hi = lo + 0x6000000;
    for (u64 a = lo; a < hi;) {
        memset(&q, 0, sizeof(q));
        if (K.virtual_query((void *)a, 1, &q, sizeof(q)) != 0 || q.start >= hi || q.end <= a)
            break;
        log_map("here    ", &q);
        a = q.end;
    }
}

COLD static void diag_jit_views(void)
{
    struct vmq *map = page_alloc(MAX_MAP * sizeof(*map));
    u32 n = 0;
    for (u64 a = 0x10000; map && n < MAX_MAP && a < 0x800000000000UL; a = map[n - 1].end)
        if (!jit_vmap(a, 0x800000000000UL, &map[n], 1) || map[n++].end <= a)
            break;
    logf("diag: the compiler process's sceKernelVirtualQuery lists %u mappings (JIT mappings are not among them on a PS5):",
         n);
    for (u32 i = 0; i < n; i++)
        log_map("compiler", &map[i]);
    diag_main_map();
    /* the PS4 boot's own checks: each buffer past its header and the small
     * area, probed at both ends (16 bytes written by the compiler, put back) */
    u64 lo = 0, hi = 0;
    int rc = arena_init();
    int rc2 = rc == 0 ? arena_code_region(&lo, &hi) : -1;
    logf("diag: as a PS4 boot sees it: arena %d, code region %d %lx-%lx (first module at %lx)", rc, rc2, lo, hi,
         rc2 == 0 ? ALIGN_UP(lo, 0x10000) : 0);
    if (map)
        page_free(map, MAX_MAP * sizeof(*map));
}

COLD static void diag_threads(void)
{
    u64 eb = g_ctx->eboot_base;
    int (*kill)(u64, int) = lk("pthread_kill");
    int (*getname)(u64, char *) = lk("scePthreadGetname");
    for (u32 i = 0; i < ARRAY_SIZE(g_hostgame->threads) && g_hostgame->threads[i].name; i++) {
        u64 h = *(volatile u64 *)(eb + g_hostgame->threads[i].handle);
        int alive = h && kill ? kill(h, 0) : -1;
        char name[40] = "";
        if (alive == 0 && getname)
            getname(h, name);
        name[39] = 0;
        logf("diag: thread %-42s %lx: %s, name '%s'", g_hostgame->threads[i].name, h,
             !h ? "never created" : alive == 0 ? "ALIVE" : alive == 3 ? "gone" : "unknown", name);
    }
    u64 self = *(volatile u64 *)(eb + g_hostgame->pthread_self);
    void *cancel = lk("scePthreadCancel");
    logf("diag: the loader cancels with scePthreadSelf+0x10 = %lx; scePthreadCancel is %p (%s)", self + 0x10, cancel,
         self + 0x10 == (u64)cancel ? "the same" : "NOT the same: its cancels may not have happened");
}

/* SWRR's heap and what a boot's hostmem.c would release (only looked at) */
COLD static void diag_memory(void)
{
    u64 eb = g_ctx->eboot_base, heap = 0;
    if (!is_jakx()) { /* SWRR offsets: in Jak X they are execute-only code */
        heap = *(volatile u64 *)(eb + 0x3906c0);
        logf("diag: SWRR heap (mspace) %lx; companion buffer %lx; services quit word %d", heap,
             *(volatile u64 *)(eb + 0x182f338), *(volatile s32 *)(eb + 0x182f328));
    }
    struct vmq q;
    for (u64 a = 0x200000000; a < 0x280000000;) {
        memset(&q, 0, sizeof(q));
        if (K.virtual_query((void *)a, 1, &q, sizeof(q)) != 0 || q.start >= 0x280000000)
            break;
        if ((q.flags & 2) && q.end - q.start >= 0x800000)
            logf("diag: a boot releases %lx-%lx (%lu MiB, prot %x type %d)%s", q.start, q.end, (q.end - q.start) >> 20,
                 q.prot, q.type, heap >= q.start && heap < q.end ? ": SWRR's heap" : "");
        a = q.end;
    }
    host_dmem_census();
}

COLD static void diag_writes(void)
{
    /* the shared scratch: a compiler write must show here */
    u64 at = g_ctx->oob_scratch + 0x7f000, test = 0x1234567890abcdefULL;
    *(volatile u64 *)at = 0;
    jit_copy_to(at, &test, 8);
    logf("diag: compiler write to the shared scratch %lx: %s", at, *(volatile u64 *)at == test ? "ok" : "NOT SEEN");
    *(volatile u64 *)at = 0;
    /* new JIT blocks, each mapped here at a fixed address inside a reserved
     * range: mapping one without an address puts it at its home address over
     * whatever is there.  A PS5 13.60 makes at most 768 KiB (0x8002000c past
     * it), so this stops at the first failure. */
    static const u32 k_kib[] = {64, 768, 1024, 4096, 16384, 32768};
    void *va = NULL;
    if (K.reserve_va(&va, 0x2400000, 0, 0x100000) != 0 || !va) {
        logf("diag: no address range for JIT block tests");
        return;
    }
    for (u32 i = 0; i < ARRAY_SIZE(k_kib); i++) {
        u64 size = (u64)k_kib[i] << 10, want = (u64)va + 0x100000 + i, got = 0;
        struct jit_block b;
        u8 code[11] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xC3}; /* mov rax, imm64; ret */
        *(u64 *)(code + 2) = want;
        int rc = jit_block_create(&b, size, (u64)va);
        if (rc == 0 && jit_block_write(&b, size - 0x4000, code, sizeof(code)) == 0 &&
            memcmp((void *)(b.rx + size - 0x4000), code, sizeof(code)) == 0)
            got = ((u64(*)(void))(b.rx + size - 0x4000))();
        logf("diag: JIT block of %lu KiB: create %d, compiler RW %lx, here RX %lx (wanted %p), call %s", size >> 10, rc,
             b.rw, b.rx, va, got == want ? "ok" : "FAILED");
        jit_block_destroy(&b);
        if (got != want)
            break;
    }
    K.munmap(va, 0x2400000);
}

/* Last, as it may meet live code: each buffer's first 16 KiB, read through
 * the compiler, then written (16 bytes, put back).  Neither can crash it. */
COLD static void diag_headers(void)
{
    u64 qa = g_ctx->reserved[2], last = 0;
    int (*qmp)(void *, void **, void **, int *) = lk("sceKernelQueryMemoryProtection");
    for (u64 a = qa + 0x40000; qmp && qa && a < qa + 0x5000000; a += 0x4000) {
        void *st = 0, *en = 0;
        int prot = 0;
        if (a < last || qmp((void *)a, &st, &en, &prot) != 0)
            continue;
        last = (u64)en;
        if (prot != (PROT_READ | PROT_EXEC) || (u64)en - (u64)st < 0x10000)
            continue;
        u64 h = (u64)st, old[2] = {0, 0};
        int rd = jit_peek(h, old, 16);
        bool same = rd == 0 && memcmp(old, (const void *)h, 16) == 0;
        int wr = rd == 0 ? jit_stamp(h) : -1;
        if (wr == 0)
            jit_copy_to(h, old, 16);
        logf("diag: buffer %lx-%lx, its first 16 bytes through the compiler: read %s%s, write %s", h, (u64)en,
             rd ? "EFAULT" : "ok", rd ? "" : same ? " (same bytes here)" : " (OTHER bytes here)", wr ? "EFAULT" : "ok");
    }
}

COLD int ghost_diag(void)
{
    logf("diag: start (nothing is booted or released)");
    diag_platform();
    g_ctx->flags |= CTX_FLAG_PS4; /* from here on: the PS4 boot's checks, on any console */
    diag_threads();
    diag_memory();
    diag_jit_views();
    diag_writes();
    diag_headers();
    logf("diag: done");
    return 0;
}
