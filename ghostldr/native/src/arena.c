/*
 * Executable memory without new JIT allocations.
 *
 * The ps2emu compiler process can only create a little new JIT memory, but the
 * emulator already owns ~64 MiB of JIT program buffers (IPU/EE/IOP/VU0/VU1):
 * RW in the compiler, RX in this process, at the same addresses.  With the
 * emulator stopped we reuse them:  code and RELRO are written through the
 * compiler, and ranges that must be writable here are unmapped and replaced by
 * direct memory ("punched").
 */
#include "ghost.h"

struct rx_region {
    u64 start, end;
};

static struct rx_region g_rx[8];
static u32 g_nrx;
static u64 g_small_cur, g_small_end; /* bump area for stubs/trampolines */

static int (*qmp)(void *, void **, void **, int *);

int arena_init(void)
{
    qmp = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelQueryMemoryProtection");
    u64 from = g_ctx->reserved[2] ? g_ctx->reserved[2] : 0x9009d4000;
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
            g_rx[g_nrx++] = (struct rx_region){(u64)st, (u64)en};
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
    return g_small_end > g_small_cur ? 0 : -1;
}

/* Largest RX region: where modules are placed. */
int arena_code_region(u64 *start, u64 *end)
{
    u32 best = 0;
    for (u32 i = 1; i < g_nrx; i++)
        if (g_rx[i].end - g_rx[i].start > g_rx[best].end - g_rx[best].start)
            best = i;
    if (!g_nrx)
        return -1;
    *start = g_rx[best].start;
    *end = g_rx[best].end;
    return 0;
}

bool arena_is_rx(u64 addr, u64 size)
{
    for (u32 i = 0; i < g_nrx; i++)
        if (addr >= g_rx[i].start && addr + size <= g_rx[i].end)
            return true;
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

int arena_write(u64 dst, const void *src, u64 size)
{
    return jit_copy_to(dst, src, size);
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
    memset((void *)addr, 0, size);
    return 0;
}
