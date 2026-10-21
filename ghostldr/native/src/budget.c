/*
 * Fit CUSA11456's memory plan into SWRR's direct memory.
 *
 * Ghost's planner (eboot 0x895b40) reserves every byte of
 * sceKernelGetDirectMemorySize() up front.  In memory mode 0 (base PS4) it
 * needs ~5.07 GiB; SWRR's process has 4.5 GiB, ~4.37 GiB of it free once
 * hostmem.c releases the emulator's buffers.
 *
 * Measured: the type 3 pool at 0x1500000000 (table 0x111ce10) is the general
 * GPU heap (engine heap 0x390d80); boot alone needs ~900 MiB of it, so it
 * keeps its size.  The pool at 0x1200000000 (table 0x111cd50) feeds a paged
 * GPU pool manager and grows 1120 -> 2120 -> 4696 MiB with richer memory
 * modes: the texture streaming pool.  It takes the shortfall.  vdm.c removes
 * the need for physically contiguous blocks, so only the total matters.
 */
#include "ghost.h"

#define MiB (1UL << 20)

/* rodata tables indexed by memory mode (0 = base PS4) */
#define TBL_GPU_HEAP 0x111ce10 /* type 3 general GPU heap, mapped at 0x1500000000 */
#define TBL_POOL1 0x111cde0    /* type 0 engine heap, mapped at 0x1100000000 */
#define TBL_CPU_HEAP 0x111cd20 /* type 0 CPU malloc mspace, mapped at 0x1000000000 */
#define TBL_STREAM_POOL 0x111cd50 /* type 3, mapped at 0x1200000000 */
static const u64 k_stream_pool[6] = {0x46000000, 0x46000000, 0x46000000, 0x84800000, 0x125800000, 0x125800000};

/* mode-0 reservations other than the streaming pool, in MiB:
 * 0x111cd20 384 + 0x111cd80 116 + 0x111cdb0 64, 0x111cde0 332 (type 0 pool),
 * 0x111ce10 1390 (GPU heap), subsystem budgets 1660 */
#define FIXED_MIB 3946UL
#define REMAINDER_MIB 48UL /* planner's general-heap remainder */
#define SLACK_MIB 32UL     /* free memory left for late allocations */
/* the CPU malloc mspace (0x111cd20, 384 MiB) gives 64 MiB to the streaming
 * pool: at 352 MiB the pool cannot hold core_common's textures and loading
 * stalls; at ~512 MiB the world loads */
#define CPU_HEAP_MIB 320UL

/* Reservations the title maps only partly; vdm.c backs them lazily, 2 MiB at
 * a time.  At the loading screen (measured 2026-10-20): planner type-0 heap
 * 927 of 1096 MiB mapped, planner type-3 heap 440 of 612, 64 KiB-page GPU
 * pool 56 of 116.  Keys are the call sites' return addresses. */
static const u64 k_lazy_sites[] = {0x896126, 0x89614a, 0xeee2cf};
#define LAZY_TOTAL_MIB (1096UL + 612UL + 116UL) /* their full sizes (mode 0, REMAINDER_MIB 48) */
/* Loading the beach (2026-10-20) commits 1040 + 606 + 62 MiB of them, its
 * fight takes the 64 KiB-page pool to 100 MiB.  Together with the movie
 * reserve below this covers all 1824 MiB, and leaves the streaming pool
 * ~590 MiB (550 stalls the beach load). */
#define LAZY_RESERVE_MIB 1704UL                 /* physical memory kept for their commits */
/* one PS5 BC video decoder as hle_sys.c vdec_config sizes it (DPB of H.264
 * level 4.2, 2-deep pipeline).  Free at boot for the splash movies; later the
 * pools above may take it, and a movie that finds no memory is skipped. */
#define MOVIE_RESERVE_MIB 100UL

bool budget_lazy_site(u64 eboot_off)
{
    for (u32 i = 0; i < ARRAY_SIZE(k_lazy_sites); i++)
        if (k_lazy_sites[i] == eboot_off)
            return true;
    return false;
}
#define MIN_POOL_MIB 128UL

u64 g_reported_dmem;

struct hole {
    u64 s, e;
};
#define MAX_HOLES 96

/* free physical ranges: the complement of the allocated ones */
static u32 free_holes(struct hole *h)
{
    struct {
        off_t start, end;
        int type;
    } q;
    int (*dmq)(off_t, int, void *, size_t) = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelDirectMemoryQuery");
    u64 real = K.direct_size(), cur = 0, off = 0;
    u32 n = 0;
    for (int i = 0; dmq && i < 512 && n < MAX_HOLES; i++) {
        memset(&q, 0, sizeof(q));
        if (dmq((off_t)off, 1, &q, sizeof(q)) != 0)
            break;
        if ((u64)q.start > cur)
            h[n++] = (struct hole){cur, (u64)q.start};
        cur = off = (u64)q.end;
    }
    if (cur < real && n < MAX_HOLES)
        h[n++] = (struct hole){cur, real};
    return n;
}

void budget_patch_text(struct module *m, u8 *text)
{
    u64 *pool = (u64 *)(text + TBL_STREAM_POOL);
    if (TBL_STREAM_POOL + sizeof(k_stream_pool) > m->text_size || memcmp(pool, k_stream_pool, sizeof(k_stream_pool))) {
        logf("budget: streaming pool table not found; leaving the plan alone");
        return;
    }
    /* experiment knobs (run.py --gpu-heap-mib/--pool1-mib/--cpu-heap-mib):
     * resize the other mode-0 reservations; the streaming pool absorbs it */
    static const struct {
        u64 table;
        u32 knob;
        const char *what;
    } k_knobs[] = {{TBL_GPU_HEAP, 4, "general GPU heap"}, {TBL_POOL1, 6, "type-0 engine heap"},
                   {TBL_CPU_HEAP, 7, "CPU malloc heap"}};
    s64 gpu_delta_mib = 0;
    for (u32 i = 0; i < ARRAY_SIZE(k_knobs); i++) {
        u64 want = g_ctx->reserved[k_knobs[i].knob];
        if (!want && k_knobs[i].table == TBL_CPU_HEAP)
            want = CPU_HEAP_MIB;
        if (!want)
            continue;
        u64 *t = (u64 *)(text + k_knobs[i].table);
        gpu_delta_mib += (s64)want - (s64)(t[0] / MiB);
        logf("budget: %s %lu -> %lu MiB", k_knobs[i].what, t[0] / MiB, want);
        t[0] = want * MiB;
    }
    u64 slack_mib = g_ctx->reserved[5] ? g_ctx->reserved[5] : SLACK_MIB;
    struct hole free[MAX_HOLES];
    u32 nfree = free_holes(free);
    u64 total = 0;
    for (u32 i = 0; i < nfree; i++) {
        total += free[i].e - free[i].s;
        if (free[i].e - free[i].s >= 16 * MiB)
            logf("budget: free %lx-%lx (%lu MiB)", free[i].s, free[i].e, (free[i].e - free[i].s) / MiB);
    }
    /* the lazily backed reservations (planner heaps = subsystem budgets +
     * remainder, and the 64 KiB-page pool) count with LAZY_RESERVE_MIB
     * instead of their full LAZY_TOTAL_MIB */
    s64 pool_mib = (s64)(total / MiB) - (s64)(FIXED_MIB + 2 + REMAINDER_MIB + slack_mib) - gpu_delta_mib +
                   (s64)LAZY_TOTAL_MIB - (s64)LAZY_RESERVE_MIB - (s64)MOVIE_RESERVE_MIB;
    if (pool_mib > (s64)(k_stream_pool[0] / MiB))
        pool_mib = (s64)(k_stream_pool[0] / MiB);
    if (pool_mib < (s64)MIN_POOL_MIB) {
        logf("budget: only %lu MiB free; streaming pool clamped to %lu MiB", total / MiB, MIN_POOL_MIB);
        pool_mib = MIN_POOL_MIB;
    }
    pool_mib &= ~1L;
    pool[0] = pool_mib * MiB;
    g_reported_dmem = (u64)((s64)FIXED_MIB + gpu_delta_mib + pool_mib + 2 + (s64)REMAINDER_MIB) * MiB;
    logf("budget: %lu MiB free in %u holes; streaming pool %lu -> %ld MiB; reporting %lu MiB direct memory",
         total / MiB, nfree, k_stream_pool[0] / MiB, pool_mib, g_reported_dmem / MiB);
}
