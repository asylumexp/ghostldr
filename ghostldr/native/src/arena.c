/*
 * Executable memory without new JIT allocations.
 *
 * The ps2emu compiler process can only create a little new JIT memory, but the
 * emulator already owns ~64 MiB of JIT program buffers (IPU/EE/IOP/VU0/VU1):
 * RW in the compiler, RX in this process, at the same addresses.  With the
 * emulator stopped we reuse them:  code and RELRO are written through the
 * compiler, and ranges that must be writable here are unmapped and replaced by
 * direct memory ("punched").
 *
 * The first 16 KiB of each buffer (JIT_BUF_HEADER) are never written: the
 * code region starts after them.  On PS5 that changes no address (the EE
 * buffer starts 16 KiB below a 64 KiB boundary); on a PS4 it starts on one.
 *
 * PS4 (ctx CTX_FLAG_PS4, docs/ps4.md): nothing is written through the
 * compiler before a probe shows it can write there and the bytes show up
 * here (probe: system calls that return EFAULT rather than crash).  A
 * region it cannot write is left alone, or its part the title needs is
 * replaced by a new JIT block (delta: where the compiler writes, relative to
 * here).  Every write is read back.
 */
#include "ghost.h"

#define COLD GHOST_COLD

struct rx_region {
    u64 start, end;
    u64 wlo, whi; /* the part written through the compiler: [start, end) on PS5 */
    s64 delta;    /* compiler address - address here (a JIT block backing it) */
    bool fresh;   /* a fresh object after a reclaim (arena_adopt), not an emulator buffer */
};

static struct rx_region g_rx[8];
static u32 g_nrx;
static u64 g_small_cur, g_small_end; /* bump area for stubs/trampolines */
static u64 g_sa_lo, g_sa_hi;         /* PS4: the small area's compiler-writable part */
u64 g_punched;

static int (*qmp)(void *, void **, void **, int *);

/* ---- PS4 ---- */

/* Can the compiler write at addr + delta, and does it show at addr here?
 * 16 bytes: the compiler's own are read first and put back.  Neither probe
 * can crash it (jit_peek, jit_stamp: EFAULT instead).  Bytes that differ
 * from these here are other memory: nothing is written there.  Each step is
 * logged before it runs (a PS4 boot once stopped silently in here). */
COLD static bool probe(u64 addr, s64 delta)
{
    u64 w = addr + (u64)delta, old[2], now[2];
    logf("arena: PS4: probe %lx: write", w);
    if (jit_peek(w, old, 16) != 0) {
        logf("arena: PS4: probe %lx: not readable", w);
        return false;
    }
    if (memcmp((const void *)addr, old, 16) != 0) {
        logf("arena: PS4: probe %lx: other bytes than %lx", w, addr);
        return false;
    }
    logf("arena: PS4: probe %lx: clock_gettime", w);
    if (jit_stamp(w) != 0) {
        logf("arena: PS4: probe %lx: not writable", w);
        return false;
    }
    logf("arena: PS4: probe %lx: write again", w);
    bool ok = jit_peek(w, now, 16) == 0 && memcmp(now, old, 16) != 0 && memcmp((const void *)addr, now, 16) == 0;
    logf("arena: PS4: probe %lx: memcpy back", w);
    jit_copy_to(w, old, 16);
    ok = ok && memcmp((const void *)addr, old, 16) == 0;
    logf("arena: PS4: probe %lx: %s", w, ok ? "written and seen here" : "the write does not show here");
    return ok;
}

/* Each buffer past its header, probed at both ends (JIT objects are mapped
 * whole), and the small area.  The header is 16 KiB (32 KiB for the VU
 * buffers: a 13.60 PS5 writes neither their +16 KiB nor any buffer's first
 * bytes).  The buffer holding the core is not probed, nor the core's data.
 * First the probes themselves are checked on memory known to be good: when
 * they do not work, nothing is probed or written through the compiler and
 * arena_init fails (the boot stops). */
COLD static void ps4_verify(void)
{
    bool usable = jit_ps4_check() == 0;
    if (!usable)
        logf("arena: PS4: the compiler checks failed (above): no JIT buffer is probed and nothing is written "
             "through the compiler; the boot stops here (docs/ps4.md)");
    for (u32 i = 0; i < g_nrx; i++) {
        struct rx_region *r = &g_rx[i];
        u64 lo = r->start + JIT_BUF_HEADER;
        bool ok = usable && r->start > g_small_cur && lo + JIT_BUF_HEADER + 16 <= r->end &&
                  (probe(lo, 0) || probe(lo += JIT_BUF_HEADER, 0)) && probe(r->end - 16, 0);
        r->wlo = ok ? lo : 0, r->whi = ok ? r->end : 0;
        if (r->start <= g_small_cur)
            logf("arena: PS4: %lx-%lx: holds the core", r->start, r->end);
        else if (usable)
            logf("arena: PS4: %lx-%lx: %s %lu KiB", r->start, r->end,
                 ok ? "the compiler writes it past" : "NOT writable through the compiler, even past", (lo - r->start) >> 10);
    }
    bool ok = usable && g_small_end >= g_small_cur + 16 && probe(g_small_cur, 0) && probe(g_small_end - 16, 0);
    g_small_end = ok ? g_small_end : g_small_cur;
    g_sa_lo = g_small_cur, g_sa_hi = g_small_end;
    logf("arena: PS4: small area %lx-%lx", g_small_cur, g_small_end);
}

static u64 g_from; /* where the scan starts: the IPU buffer, which holds the core */

/* The regions holding the core and the small area: never module code. */
static bool holds_core(const struct rx_region *r)
{
    return r->start <= g_small_cur && r->end > g_from;
}

/* PS4: [lo, whi) of a region: its compiler-writable part past the header,
 * from the 64 KiB boundary where boot.c places the first module. */
static u64 usable(const struct rx_region *r, u64 *lo)
{
    *lo = ALIGN_UP(r->wlo > r->start + JIT_BUF_HEADER ? r->wlo : r->start + JIT_BUF_HEADER, 0x10000);
    return r->whi > *lo ? r->whi - *lo : 0;
}

/* PS4: the region with the most compiler-writable bytes past its header.
 * When the title's modules (published by then; none yet for the selector)
 * need more, the largest region's start is backed by a new JIT block. */
static int chosen = -1;
static bool final; /* chosen with the title's modules known (or a fresh object adopted) */
COLD static int ps4_code_region(u64 *start, u64 *end)
{
    if (chosen < 0 || (!final && g_modules)) {
        u64 need = jv_code_need(g_modules), best_len = 0, lo;
        u32 best = 0, largest = 0;
        for (u32 i = 0; i < g_nrx; i++) {
            if (holds_core(&g_rx[i]))
                continue;
            u64 len = usable(&g_rx[i], &lo);
            if (len > best_len)
                best = i, best_len = len;
            if (holds_core(&g_rx[largest]) || g_rx[i].end - g_rx[i].start > g_rx[largest].end - g_rx[largest].start)
                largest = i;
        }
        chosen = -1, final = g_modules != NULL;
        if (best_len && best_len >= need) {
            chosen = (int)best;
        } else if (need && !holds_core(&g_rx[largest])) {
            /* nothing the compiler writes holds the modules: a new JIT block
             * over the largest buffer, where boot.c places them */
            struct rx_region *r = &g_rx[largest];
            struct jit_block b = {0};
            lo = ALIGN_UP(r->start + JIT_BUF_HEADER, 0x10000);
            logf("arena: PS4: no region the compiler writes holds %lx bytes (best %lx): a new JIT block at %lx", need,
                 best_len, lo);
            if (lo + need > r->end || jit_block_create(&b, need, lo) != 0 || b.rx != lo) {
                if (b.size)
                    jit_block_destroy(&b);
                logf("arena: PS4: no JIT block of %lx bytes at %lx: the title's code has nowhere to go", need, lo);
                return -1;
            }
            r->wlo = lo, r->whi = lo + b.size, r->delta = (s64)(b.rw - lo);
            if (!probe(lo, r->delta))
                return -1;
            chosen = (int)largest;
        } else {
            logf("arena: PS4: no region the compiler writes (need %lx, best %lx)", need, best_len);
            return -1;
        }
    }
    struct rx_region *r = &g_rx[chosen];
    *start = r->wlo > r->start + JIT_BUF_HEADER ? r->wlo : r->start + JIT_BUF_HEADER;
    *end = r->whi;
    return 0;
}

int arena_init(void)
{
    if (g_nrx)
        return 0; /* already done (the game selector runs before the title is placed) */
    qmp = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelQueryMemoryProtection");
    u64 from = g_from = g_ctx->reserved[2] ? g_ctx->reserved[2] : 0x9009d4000;
    u64 last_end = 0;
    for (u64 a = from; qmp && a < from + 0x5000000 && g_nrx < ARRAY_SIZE(g_rx); a += 0x4000) {
        if (a < last_end)
            continue;
        void *st = 0, *en = 0;
        int prot = -1;
        if (qmp((void *)a, &st, &en, &prot) != 0)
            continue;
        last_end = (u64)en;
        if (prot == (PROT_READ | PROT_EXEC) && (u64)en - (u64)st >= 0x10000) {
            g_rx[g_nrx++] = (struct rx_region){(u64)st, (u64)en, (u64)st, (u64)en, 0, false};
            logf("arena: RX %lx-%lx (%lu KiB)", (u64)st, (u64)en, ((u64)en - (u64)st) >> 10);
        }
    }
    if (!g_nrx)
        return -1;
    /* the first (smallest, IPU) region holds the core, then stubs and
     * trampolines after it */
    g_small_cur = g_ctx->reserved[3] ? ALIGN_UP(g_ctx->reserved[3], 0x4000) : g_rx[0].start;
    g_small_end = 0;
    for (u32 i = 0; i < g_nrx; i++)
        if (g_small_cur >= g_rx[i].start && g_small_cur < g_rx[i].end)
            g_small_end = g_rx[i].end;
    if (!g_small_end) {
        /* the core's data hole split the IPU buffer: use the RX part after it */
        void *st = 0, *en = 0;
        int prot = 0;
        if (qmp((void *)g_small_cur, &st, &en, &prot) == 0 && prot == (PROT_READ | PROT_EXEC))
            g_small_end = (u64)en;
    }
    logf("arena: small area %lx-%lx", g_small_cur, g_small_end);
    for (u32 i = 0; i < g_nrx; i++)
        g_rx[i].wlo = g_rx[i].start, g_rx[i].whi = g_rx[i].end, g_rx[i].delta = 0;
    if (is_ps4())
        ps4_verify();
    return g_small_end > g_small_cur ? 0 : -1;
}

/* Largest RX region past its header: where modules are placed. */
int arena_code_region(u64 *start, u64 *end)
{
    u32 best = 0;
    for (u32 i = 1; i < g_nrx; i++)
        if (g_rx[i].end - g_rx[i].start > g_rx[best].end - g_rx[best].start)
            best = i;
    if (!g_nrx)
        return -1;
    if (is_ps4())
        return ps4_code_region(start, end);
    *start = g_rx[best].start + JIT_BUF_HEADER;
    *end = g_rx[best].end;
    return 0;
}

/* Module bases, in load order: boot modules, then run-time loads (loader.c). */
u64 arena_code_alloc(u64 size)
{
    static u64 cur;
    u64 lo, hi;
    if (!cur && arena_code_region(&lo, &hi) == 0)
        cur = ALIGN_UP(lo, 0x10000);
    u64 at = cur;
    if (cur)
        cur = ALIGN_UP(cur + size, 0x10000);
    return at;
}

/* The emulator's JIT buffers that hold no part of the core: each region is
 * one whole JIT object (bigexec.c maps some of them elsewhere). */
COLD u32 arena_buffers(u64 *start, u64 *end, u32 max)
{
    u32 n = 0;
    for (u32 i = 0; i < g_nrx && n < max; i++)
        if (!holds_core(&g_rx[i]) && !g_rx[i].fresh && g_rx[i].end - g_rx[i].start >= 0x100000)
            start[n] = g_rx[i].start, end[n++] = g_rx[i].end;
    return n;
}

/* The extension's code when no new JIT block takes it (ext.c; PS5): the
 * top of the smallest emulator buffer that holds it (at the highest address
 * on a tie: VU0 on 13.60), which then leaves the arena.  Its data pages are
 * punched like the small area's.  Placed again (an app root that found no
 * game), it goes to the same place.  0: none. */
COLD u64 arena_buffer_top(u64 size)
{
    static u64 at, have;
    size = ALIGN_UP(size, 0x4000);
    if (at && size <= have)
        return at;
    int pick = -1;
    if (is_ps4() || is_jakx() || arena_init() != 0)
        return 0;
    for (u32 i = 0; i < g_nrx; i++) {
        u64 len = g_rx[i].end - g_rx[i].start;
        if (!holds_core(&g_rx[i]) && len >= 0x100000 && len >= size + 2 * JIT_BUF_HEADER &&
            (pick < 0 || len <= g_rx[pick].end - g_rx[pick].start))
            pick = (int)i;
    }
    if (pick < 0)
        return 0;
    at = g_rx[pick].end - size, have = size;
    logf("arena: the extension at %lx, not a new JIT block", at);
    arena_drop(g_rx[pick].start);
    return at;
}

/* A fresh JIT object (bigexec.c's reclaim) mapped read-execute here at
 * [start, end), where the compiler writes it at start + delta: the code
 * region from now on (the buffers are gone). */
COLD int arena_adopt(u64 start, u64 end, s64 delta)
{
    if (g_nrx == ARRAY_SIZE(g_rx))
        return -1;
    chosen = (int)g_nrx, final = true; /* the code region on a PS4 too */
    g_rx[g_nrx++] = (struct rx_region){start, end, start, end, delta, true};
    logf("arena: RX %lx-%lx: fresh, written at %lx", start, end, start + (u64)delta);
    return 0;
}

/* A buffer whose object now holds a module's code elsewhere: no module,
 * selector or punch goes there any more. */
COLD void arena_drop(u64 start)
{
    for (u32 i = 0; i < g_nrx; i++)
        if (g_rx[i].start == start) {
            logf("arena: %lx-%lx (%lu KiB) leaves the arena", g_rx[i].start, g_rx[i].end,
                 (g_rx[i].end - g_rx[i].start) >> 10);
            for (g_nrx--; i < g_nrx; i++)
                g_rx[i] = g_rx[i + 1];
            return;
        }
}

bool arena_is_rx(u64 addr, u64 size)
{
    for (u32 i = 0; i < g_nrx; i++)
        if (addr >= g_rx[i].start && addr + size <= g_rx[i].end)
            return !is_ps4() || (addr >= g_rx[i].wlo && addr + size <= g_rx[i].whi);
    return false;
}

u64 arena_small_alloc(u64 size)
{
    size = ALIGN_UP(size, 16);
    if (g_small_cur + size > g_small_end)
        return 0;
    u64 p = g_small_cur;
    g_small_cur += size;
    return p;
}

/* 16 KiB-aligned space from the top of the small area (the runtime
 * extension), leaving at least keep bytes below it; 0 when it does not fit. */
u64 arena_small_alloc_top(u64 size, u64 keep)
{
    size = ALIGN_UP(size, 0x4000);
    u64 top = ALIGN_DOWN(g_small_end, 0x4000);
    if (top < g_small_cur || top - g_small_cur < keep + size)
        return 0;
    g_small_end = top - size;
    return g_small_end;
}

/* Where the compiler writes [dst, dst + size): dst itself on PS5, but for
 * a fresh object adopted after a reclaim; on a PS4 only inside what
 * ps4_verify or a JIT block made writable, else 0. */
u64 arena_wr(u64 dst, u64 size)
{
    if (!is_ps4()) {
        for (u32 i = 0; i < g_nrx; i++)
            if (dst >= g_rx[i].start && dst + size <= g_rx[i].end)
                return dst + (u64)g_rx[i].delta;
        return dst;
    }
    for (u32 i = 0; i < g_nrx; i++)
        if (dst >= g_rx[i].wlo && dst + size <= g_rx[i].whi && g_rx[i].whi)
            return dst + (u64)g_rx[i].delta;
    if (dst >= g_sa_lo && dst + size <= g_sa_hi)
        return dst;
    logf("arena: PS4: refusing a compiler write to %lx+%lx: not known to be writable there", dst, size);
    return 0;
}

int arena_write(u64 dst, const void *src, u64 size)
{
    if (!is_ps4())
        return jit_copy_to(arena_wr(dst, size), src, size);
    u64 w = arena_wr(dst, size);
    if (!w)
        return -1;
    jit_copy_to(w, src, size);
    if (memcmp((const void *)dst, src, size) != 0) {
        logf("arena: PS4: %lx bytes written at %lx through the compiler do not show at %lx", size, w, dst);
        return -1;
    }
    return 0;
}

/* Make [addr, addr+size) private RW memory, zero filled.  Fixed direct
 * mappings are refused below 0x10_0000_0000 and SWRR's app flexible budget
 * is nearly exhausted, but the system flexible budget accepts fixed maps
 * here and has room (>= 128 MiB measured). */
int arena_punch_rw(u64 addr, u64 size)
{
    static int (*sysflex)(void **, size_t, int, int, const char *);
    if (!sysflex)
        sysflex = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelMapNamedSystemFlexibleMemory");
    addr = ALIGN_DOWN(addr, 0x4000);
    size = ALIGN_UP(size, 0x4000);
    K.munmap((void *)addr, size);
    void *va = (void *)addr;
    int rc = sysflex ? sysflex(&va, size, PROT_READ | PROT_WRITE, MAP_FIXED, "ghostldr") : -1;
    if (rc != 0 || (u64)va != addr) {
        logf("punch: system flex %lx+%lx rc=%x got %p", addr, size, rc, va);
        return -1;
    }
    g_punched += size;
    memset((void *)addr, 0, size);
    return 0;
}
