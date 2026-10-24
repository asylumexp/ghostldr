/*
 * The title's memory budget inside SWRR's direct memory, fixed at plan time
 * (the executable is relocated, its text not yet written) and never changed.
 *
 * SWRR's process has 4.5 GiB of direct memory, ~4.4-4.5 GiB of it free once
 * hostmem.c releases the emulator's buffers, and almost no flexible memory.
 *
 * Plan-free (every title by default): the title is told one stable direct
 * size, D = free memory - ghostldr's reserve (64 MiB) - the video decoder's
 * (100 MiB if it imports one) - its flexible budget F (its own
 * mem_param.flexible_memory_size, else 448 MiB), and offered it as one
 * contiguous run; vdm.c scatters what no single hole holds.  Allocations past
 * D fail with EAGAIN, flexible mappings past F with ENOMEM (flex is backed by
 * direct memory, never by SWRR's or the system's flex pool).  Neo mode is 0.
 *
 * Planned (a profile's memory plan, only on the profile's own build): for
 * titles whose fixed demand is larger.  Ghost of Tsushima's planner reserves
 * every byte it is told about up front: ~5.07 GiB in memory mode 0.  The
 * plan's "set" tables (u64 per memory mode in the executable's text) get
 * their sizes, its "absorb" table (Ghost: the texture streaming pool,
 * 1120 MiB) takes the shortfall, and lazy sites (vdm.c) back reservations
 * the title fills only partly:
 *     absorb = free - title_fixed - reserve - set deltas + overcommit - video
 *     reported = title_fixed + set deltas + absorb
 * Ghost (CUSA11456.json): 4532 MiB free -> streaming pool 1120 -> 588 MiB,
 * reporting 4520 MiB.
 *
 * A Pro-mode host (Jak X: sceKernelIsNeoMode 1, 5 GiB) runs a planned title
 * in its own Pro memory mode when the plan names it (REC_NEO): the title is
 * told Neo mode 1, the tables are patched at that mode's index and its fixed
 * demand is the profile's, as are its set tables' sizes (knob_mib).
 * Base-mode display buffers are refused there (VideoOut wants 64 KiB
 * alignment in a Neo process).  Ghost: mode 2, 4636 MiB fixed, GPU heap
 * 1908 -> 1680 MiB; 4982 MiB free -> streaming pool 580 MiB.
 */
#include "ghost.h"

#define MiB (1UL << 20)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define E_NOMEM 0x8002000c
#define E_AGAIN 0x80020023
#define E_INVAL 0x80020016
#define DEFAULT_FLEX (448 * MiB)
#define VIDEO_MIB 100
#define COLD GHOST_COLD /* runs once per boot (or rarely): small beats fast */

u64 g_reported_dmem;
static u64 g_flex_cap;                    /* 0: no cap (planned titles without flex_mib) */
static volatile u64 g_flex_used, g_title_dmem;
static bool g_planfree;

/* the plan's Pro mode, when the host runs in Pro mode; else NULL */
COLD static const struct prof_neo *neo_plan(void)
{
    static int host_neo = -1;
    if (host_neo < 0) {
        int (*neo)(void) = host_real("sceKernelIsNeoMode");
        host_neo = neo && neo() == 1;
    }
    return host_neo ? profile_record(REC_NEO, 0) : NULL;
}

COLD bool budget_lazy_site(u64 eboot_off)
{
    const struct prof_plan *p = profile_record(REC_PLAN, 0);
    for (u32 i = 0; p && i < p->nsites; i++)
        if (p->sites[i] == eboot_off)
            return true;
    return false;
}

struct hole {
    u64 s, e;
};
#define MAX_HOLES 96

/* free physical ranges: the complement of the allocated ones */
COLD static u32 free_holes(struct hole *h, u64 *total)
{
    struct {
        off_t start, end;
        int type;
    } q;
    int (*dmq)(off_t, int, void *, size_t) = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelDirectMemoryQuery");
    u64 real = K.direct_size(), cur = 0, off = 0;
    u32 n = 0;
    /* find-next can fail from inside a range released earlier although
     * allocations follow it (seen after freeing GPU-mapped memory): step on
     * 1 MiB instead of ending the walk, which would count everything past
     * that point as free */
    for (int i = 0; dmq && i < 16384 && off < real && n < MAX_HOLES; i++) {
        memset(&q, 0, sizeof(q));
        if (dmq((off_t)off, 1, &q, sizeof(q)) != 0) {
            off = ALIGN_DOWN(off, MiB) + MiB;
            continue;
        }
        if ((u64)q.start > cur)
            h[n++] = (struct hole){cur, (u64)q.start};
        cur = off = (u64)q.end;
    }
    if (cur < real && n < MAX_HOLES)
        h[n++] = (struct hole){cur, real};
    *total = 0;
    for (u32 i = 0; i < n; i++)
        *total += h[i].e - h[i].s;
    return n;
}

COLD static const char *table_name(const struct prof_table *t) { return (const char *)&t->expect[t->nexpect]; }

/* a profile's plan: set tables, absorb the rest (see the top) */
COLD static void plan_tables(struct module *m, u8 *text, const struct prof_plan *plan)
{
    const struct prof_hdr *h = profile_hdr();
    const struct prof_table *t, *absorb = NULL;
    for (u32 i = 0; (t = profile_record(REC_TABLE, i)); i++)
        if (t->role == 1)
            absorb = t;
    const struct prof_neo *neo = neo_plan();
    u32 mode = neo ? neo->mode : 0, fixed = neo ? neo->title_fixed_mib : plan->title_fixed_mib;
    u32 overcommit = neo && neo->overcommit_mib ? neo->overcommit_mib : plan->overcommit_mib;
    u64 *pool = absorb ? (u64 *)(text + absorb->offset) : NULL;
    if (!absorb || mode >= absorb->nexpect || absorb->offset + 8UL * absorb->nexpect > m->text_size ||
        memcmp(pool, absorb->expect, 8UL * absorb->nexpect)) {
        logf("budget: %s table not found; leaving the plan alone", absorb ? table_name(absorb) : "absorbing");
        return;
    }
    if (neo)
        logf("budget: Pro-mode host: the title's memory mode %u, %u MiB fixed, %u overcommitted", mode, fixed,
             overcommit);
    /* run.py --gpu-heap-mib/--pool1-mib/--cpu-heap-mib override a set table's
     * size (boot_ctx reserved[knob]), then the Pro-mode plan's knobs; the
     * absorbing table takes the change */
    s64 delta_mib = 0;
    for (u32 i = 0; (t = profile_record(REC_TABLE, i)); i++) {
        u64 want = t->knob < 8 && g_ctx->reserved[t->knob] ? g_ctx->reserved[t->knob]
                   : neo && t->knob < 8 && neo->knob_mib[t->knob] ? neo->knob_mib[t->knob]
                                                                   : t->mib;
        if (t->role != 0 || !want || t->offset + 8UL > m->text_size)
            continue;
        u64 *v = (u64 *)(text + t->offset);
        delta_mib += (s64)want - (s64)(v[mode] / MiB);
        logf("budget: %s %lu -> %lu MiB", table_name(t), v[mode] / MiB, want);
        v[mode] = want * MiB;
    }
    u64 reserve = g_ctx->reserved[5] ? g_ctx->reserved[5] : h->runtime_reserve_mib; /* run.py --slack-mib */
    struct hole free[MAX_HOLES];
    u64 total;
    u32 nfree = free_holes(free, &total);
    for (u32 i = 0; i < nfree; i++)
        if (free[i].e - free[i].s >= 16 * MiB)
            logf("budget: free %lx-%lx (%lu MiB)", free[i].s, free[i].e, (free[i].e - free[i].s) / MiB);
    s64 video = h->video_reserve_mib >= 0 ? h->video_reserve_mib : VIDEO_MIB;
    s64 orig = (s64)(absorb->expect[mode] / MiB);
    s64 pool_mib = (s64)(total / MiB) - (s64)fixed - (s64)reserve - delta_mib + (s64)overcommit - video;
    if (pool_mib > orig)
        pool_mib = orig;
    if (pool_mib < (s64)absorb->min_mib) {
        logf("budget: only %lu MiB free; %s clamped to %u MiB", total / MiB, table_name(absorb), absorb->min_mib);
        pool_mib = absorb->min_mib;
    }
    pool_mib &= ~1L;
    pool[mode] = pool_mib * MiB;
    g_reported_dmem = (u64)((s64)fixed + delta_mib + pool_mib) * MiB;
    g_flex_cap = h->flex_mib * MiB;
    logf("budget: %lu MiB free in %u holes; %s %ld -> %ld MiB; reporting %lu MiB direct memory", total / MiB, nfree,
         table_name(absorb), orig, pool_mib, g_reported_dmem / MiB);
}

/* what an address of the executable holds at plan time: text is still in
 * `text`, RELRO in its shadow, data in place */
COLD static const u64 *image_u64(struct module *m, const u8 *text, u64 addr)
{
    u64 off = addr - m->base;
    if (addr < m->base || off + 8 > m->image_size)
        return NULL;
    if (off + 8 <= m->text_size)
        return (const u64 *)(text + off);
    if (m->relro_shadow && off >= m->relro_lo && off + 8 <= m->relro_lo + m->relro_size)
        return (const u64 *)(m->relro_shadow + (off - m->relro_lo));
    return (const u64 *)addr;
}

/* proc param +0x40 -> mem param, +0x10 -> u64 *flexible_memory_size */
COLD static u64 title_flex(struct module *m, const u8 *text)
{
    const u64 *mem = m->proc_param ? image_u64(m, text, m->proc_param + 0x40) : NULL;
    const u64 *size = mem && *mem ? image_u64(m, text, *mem) : NULL;
    const u64 *fp = size && *size >= 0x18 ? image_u64(m, text, *mem + 0x10) : NULL;
    const u64 *flex = fp && *fp ? image_u64(m, text, *fp) : NULL;
    return flex && *flex ? ALIGN_UP(*flex, 0x4000) : DEFAULT_FLEX;
}

COLD static bool modules_import(const char *prefix)
{
    for (struct module *m = g_modules; m; m = m->next)
        for (u32 i = 0; i < m->n_import_libs; i++)
            if (strncmp(m->import_libs[i].name, prefix, strlen(prefix)) == 0)
                return true;
    return false;
}

COLD static int plan_free(struct module *m, const u8 *text)
{
    const struct prof_hdr *h = profile_hdr();
    u64 flex = h->flex_mib ? h->flex_mib * MiB : title_flex(m, text);
    u64 reserve = g_ctx->reserved[5] ? g_ctx->reserved[5] : h->runtime_reserve_mib;
    s64 video = h->video_reserve_mib >= 0 ? h->video_reserve_mib
                : modules_import("libSceVideodec") || modules_import("libSceAvPlayer") ? VIDEO_MIB : 0;
    struct hole free[MAX_HOLES];
    u64 total;
    u32 nfree = free_holes(free, &total), largest = 0;
    for (u32 i = 0; i < nfree; i++)
        if (free[i].e - free[i].s > free[largest].e - free[largest].s)
            largest = i;
    s64 d = ((s64)(total / MiB) - (s64)reserve - video - (s64)(flex / MiB)) & ~1L;
    logf("budget: plan-free: %lu MiB free in %u holes (largest %lu) - %lu ghostldr - %ld video - %lu flex = "
         "%ld MiB direct memory, one run; Neo mode 0",
         total / MiB, nfree, nfree ? (free[largest].e - free[largest].s) / MiB : 0, reserve, video, flex / MiB, d);
    if (d < (s64)h->min_mib) {
        logf("budget: %ld MiB of direct memory is below the profile's minimum of %u MiB: the title would fail "
             "later, so it is not started (it needs a memory plan or a smaller flex budget)", d, h->min_mib);
        return -1;
    }
    g_reported_dmem = (u64)d * MiB;
    g_flex_cap = flex;
    g_planfree = true;
    return 0;
}

COLD int budget_patch_text(struct module *m, u8 *text)
{
    const struct prof_plan *plan = profile_record(REC_PLAN, 0);
    if (plan) {
        plan_tables(m, text, plan);
        return 0;
    }
    return plan_free(m, text);
}

/* ---- the budget's gates (hle.c) ---- */
static u32 g_said[2];

int budget_alloc_gate(u64 len)
{
    if (!g_planfree || g_title_dmem + len <= g_reported_dmem)
        return 0;
    if (!g_said[0]++)
        logf("budget: direct memory full: %lu of %lu MiB allocated, %lu more asked: EAGAIN", g_title_dmem / MiB,
             g_reported_dmem / MiB, len >> 20);
    return (int)E_AGAIN;
}

void budget_alloc_note(s64 len)
{
    u64 old = __atomic_fetch_add(&g_title_dmem, (u64)len, __ATOMIC_RELAXED);
    if (len < 0 && old < (u64)-len)
        __atomic_store_n(&g_title_dmem, 0, __ATOMIC_RELAXED);
}

int budget_flex_gate(u64 len)
{
    if (!g_flex_cap || g_flex_used + len <= g_flex_cap)
        return 0;
    if (!g_said[1]++)
        logf("budget: flexible memory full: %lu of %lu MiB mapped, %lu more asked: ENOMEM", g_flex_used / MiB,
             g_flex_cap / MiB, len >> 20);
    return (int)E_NOMEM;
}

void budget_flex_note(s64 len) { __atomic_fetch_add(&g_flex_used, (u64)len, __ATOMIC_RELAXED); }

/* ---- plan-free answers to the memory queries (resolved by NID: the core's
 * name table only knows the imports of the title it was built against) ---- */
void *host_real(const char *name);
void compute_nid(const char *name, char out[12]);
int hle_map_flex(void **addr, size_t len, int prot, int flags);
int hle_alloc_direct(off_t start, off_t end, size_t len, size_t align, int type, off_t *out);
u64 direct_block_size(u64 addr);

COLD static int b_avail_direct(off_t start, off_t end, size_t align, off_t *off, size_t *size)
{
    u64 a = align ? align : 0x4000, lo = ALIGN_UP((u64)start, a), hi = MIN((u64)end, g_reported_dmem);
    struct hole h[MAX_HOLES];
    u64 phys;
    free_holes(h, &phys);
    u64 n = lo < hi ? MIN(hi - lo, g_reported_dmem - MIN(g_title_dmem, g_reported_dmem)) : 0;
    n = ALIGN_DOWN(MIN(n, phys), 0x4000);
    if (off)
        *off = (off_t)lo;
    if (size)
        *size = n;
    return n ? 0 : (int)E_NOMEM;
}

COLD static int b_alloc_main(size_t len, size_t align, int type, off_t *out)
{
    return hle_alloc_direct(0, (off_t)g_reported_dmem, len, align, type, out);
}

COLD static int b_configured_flex(size_t *out)
{
    *out = g_flex_cap;
    return 0;
}

COLD static int b_avail_flex(size_t *out)
{
    *out = g_flex_cap > g_flex_used ? g_flex_cap - g_flex_used : 0;
    return 0;
}

static int b_zero(void) { return 0; }

/* the title's system flex is flex like any other (never the console-wide pool) */
COLD static int b_sys_flex(void **addr, size_t len, int prot, int flags, const char *name)
{
    int rc = hle_map_flex(addr, len, prot, flags);
    logf("MapNamedSystemFlexibleMemory(%s, %lx) -> flex %x at %p", name ? name : "", len, rc, *addr);
    return rc;
}

/* unmapping a whole flex-as-direct block gives its memory back */
COLD NOINLINE int budget_flex_unmap(void *addr, size_t len)
{
    u64 size = direct_block_size((u64)addr);
    if (size && ALIGN_UP(len, 0x4000) == size) {
        direct_free(addr);
        budget_flex_note(-(s64)size);
        return 0;
    }
    return ((int (*)(void *, size_t))host_real("sceKernelMunmap"))(addr, len);
}

COLD static int b_posix_munmap(void *addr, size_t len) { return budget_flex_unmap(addr, len) == 0 ? 0 : -1; }

/* Anonymous mmap is flexible memory too (musl's malloc in OpenOrbis homebrew
 * grows through it): flex-as-direct within F, zero-filled.  PROT_NONE up to
 * 64 MiB is backed too, with no access, because reserve-then-commit
 * allocators (Unity's Mono) open it with mprotect and write; a reservation
 * alone has nothing to open.  Bigger PROT_NONE maps only reserve address
 * space.  Other mappings go to the extension's mmap (a ghostfs file), else
 * SWRR's. */
COLD static void *mmap_next(const char *name)
{
    char nid[12];
    compute_nid(name, nid);
    u64 f = ext_hle_lookup("libkernel", nid, name);
    return f ? (void *)f : host_real(name);
}

COLD static int anon_map(void **addr, size_t len, int prot, int flags)
{
    static u32 logged;
    u64 size = ALIGN_UP(len, 0x4000);
    void *a = (flags & MAP_FIXED) ? *addr : NULL;
    int rc;
    if (!len)
        rc = (int)E_INVAL;
    else if (prot == PROT_NONE && size > (64UL << 20))
        rc = K.reserve_va(&a, size, flags & MAP_FIXED, 0x4000);
    else if ((rc = hle_map_flex(&a, size, PROT_READ | PROT_WRITE, flags & MAP_FIXED)) == 0 &&
             prot != (PROT_READ | PROT_WRITE))
        K.mprotect(a, size, prot); /* zeroed by hle_map_flex */
    if (rc || logged < 8) {
        logged++;
        logf("mmap anonymous %lx prot %x flags %x -> %x at %p (flexible memory used %lu of %lu MiB)", len, prot,
             flags, rc, a, g_flex_used / MiB, g_flex_cap / MiB);
    }
    if (rc == 0)
        *addr = a;
    return rc;
}

COLD static void *b_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    static void *(*next)(void *, size_t, int, int, int, off_t);
    if (!(flags & MAP_ANON)) {
        if (!next)
            next = mmap_next("mmap");
        return next ? next(addr, len, prot, flags, fd, off) : (void *)-1;
    }
    int rc = anon_map(&addr, len, prot, flags);
    if (rc == 0)
        return addr;
    if (K.errno_fn)
        *K.errno_fn() = rc & 0xFFFF;
    return (void *)-1;
}

COLD static int b_kmmap(void *addr, size_t len, int prot, int flags, int fd, off_t off, void **res)
{
    static int (*next)(void *, size_t, int, int, int, off_t, void **);
    if (!(flags & MAP_ANON)) {
        if (!next)
            next = mmap_next("sceKernelMmap");
        return next ? next(addr, len, prot, flags, fd, off, res) : (int)E_INVAL;
    }
    int rc = anon_map(&addr, len, prot, flags);
    if (rc == 0 && res)
        *res = addr;
    return rc;
}

static const struct {
    const char *name;
    void *fn;
} k_mem[] = {
    {"sceKernelAvailableDirectMemorySize", b_avail_direct},
    {"sceKernelAllocateMainDirectMemory", b_alloc_main},
    {"sceKernelConfiguredFlexibleMemorySize", b_configured_flex},
    {"sceKernelAvailableFlexibleMemorySize", b_avail_flex},
    {"sceKernelIsNeoMode", b_zero},
    {"sceKernelMapNamedSystemFlexibleMemory", b_sys_flex},
    {"sceKernelMunmap", budget_flex_unmap},
    {"munmap", b_posix_munmap},
    {"mmap", b_mmap},
    {"sceKernelMmap", b_kmmap},
};

COLD u64 budget_resolve(const char *nid)
{
    static char nids[ARRAY_SIZE(k_mem)][12];
    for (u32 i = 0; i < ARRAY_SIZE(k_mem); i++) {
        if (!nids[i][0])
            compute_nid(k_mem[i].name, nids[i]);
        if (strcmp(nids[i], nid) != 0)
            continue;
        /* planned titles keep the host's answers, but Neo mode: a plan is for
         * memory mode 0 unless it names the title's Pro mode (neo_plan) */
        if (profile_record(REC_PLAN, 0) && (k_mem[i].fn != (void *)b_zero || neo_plan()))
            return 0;
        return (u64)k_mem[i].fn;
    }
    return 0;
}
