/* Allocation for the loader itself: page allocations from flexible memory and
 * a spinlocked bump arena for small, long-lived metadata. */
#include "ghost.h"

/* Page allocations come from direct memory: SWRR leaves only ~37 MiB of
 * flexible memory but several GiB of direct memory. */
struct dm_ent {
    u64 addr, size;
    off_t phys;
};
/* every block direct_alloc mapped; the table grows when full (anonymous mmap
 * makes one block per large allocation of a title's malloc).  The first one
 * is in the core's image, which must stay small (tools/core_info.py). */
static struct dm_ent g_dm0[64];
static struct dm_ent *volatile g_dm = g_dm0;
static volatile u32 g_dm_cap = ARRAY_SIZE(g_dm0);
static volatile int g_dm_lock;

/* Keep the loader's own blocks out of the large free run the title's pools
 * need (budget.c): use the hole SWRR's released heap leaves below 256 MiB,
 * else the top 32 MiB, else anywhere. */
static int alloc_phys(size_t size, off_t *phys)
{
    off_t top = (off_t)K.direct_size();
    if (K.alloc_direct(0, 0x10000000, size, 0x4000, 0, phys) == 0)
        return 0;
    if (K.alloc_direct(top - 0x2000000, top, size, 0x4000, 0, phys) == 0)
        return 0;
    return K.alloc_direct(0, top, size, 0x4000, 0, phys);
}

/* a free entry (g_dm_lock held), growing the table 4x into memory of its own;
 * the old table stays mapped, so lookups without the lock stay safe */
static struct dm_ent *dm_free_slot(void)
{
    struct dm_ent *t = g_dm;
    u32 cap = g_dm_cap;
    for (u32 i = 0; i < cap; i++)
        if (!t[i].addr)
            return &t[i];
    u64 bytes = ALIGN_UP((u64)cap * 4 * sizeof(*t), 0x4000);
    off_t phys = 0;
    void *a = NULL;
    if (alloc_phys(bytes, &phys) != 0 || K.map_direct(&a, bytes, PROT_READ | PROT_WRITE, 0, phys, 0x4000) != 0)
        return NULL;
    struct dm_ent *n = a;
    memset(n, 0, bytes);
    memcpy(n, t, (u64)cap * sizeof(*t));
    g_dm = n;
    __atomic_store_n(&g_dm_cap, cap * 4, __ATOMIC_RELEASE);
    logf("direct_alloc: block table grown to %u entries", cap * 4);
    return &n[cap];
}

void *direct_alloc(size_t size, int prot, u64 fixed)
{
    size = ALIGN_UP(size, 0x4000);
    off_t phys = 0;
    int rc = alloc_phys(size, &phys);
    if (rc != 0) {
        logf("direct_alloc(%lx) alloc failed rc=%x", size, rc);
        return NULL;
    }
    void *addr = (void *)fixed;
    rc = K.map_direct(&addr, size, prot, fixed ? MAP_FIXED : 0, phys, fixed ? 0 : 0x4000);
    if (rc != 0) {
        logf("direct_alloc(%lx) map failed rc=%x", size, rc);
        int (*rel)(off_t, size_t) = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelReleaseDirectMemory");
        if (rel)
            rel(phys, size);
        return NULL;
    }
    spin_lock(&g_dm_lock);
    struct dm_ent *e = dm_free_slot();
    if (e)
        *e = (struct dm_ent){(u64)addr, size, phys};
    spin_unlock(&g_dm_lock);
    if (!e)
        logf("direct_alloc(%lx): no room to record the block", size);
    return addr;
}

void direct_free(void *addr)
{
    int (*rel)(off_t, size_t) = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelReleaseDirectMemory");
    spin_lock(&g_dm_lock);
    for (u32 i = 0; i < g_dm_cap; i++)
        if (g_dm[i].addr == (u64)addr) {
            struct dm_ent e = g_dm[i];
            g_dm[i].addr = 0;
            spin_unlock(&g_dm_lock);
            K.munmap(addr, e.size);
            if (rel)
                rel(e.phys, e.size);
            return;
        }
    spin_unlock(&g_dm_lock);
}

/* size of the block direct_alloc returned at `addr` (0: none starts there) */
u64 direct_block_size(u64 addr)
{
    u32 cap = __atomic_load_n(&g_dm_cap, __ATOMIC_ACQUIRE); /* before the table: it grows first */
    const struct dm_ent *t = g_dm;
    for (u32 i = 0; i < cap; i++)
        if (t[i].addr == addr && addr)
            return t[i].size;
    return 0;
}

bool direct_is_ours(u64 addr)
{
    u32 cap = __atomic_load_n(&g_dm_cap, __ATOMIC_ACQUIRE); /* before the table: it grows first */
    const struct dm_ent *t = g_dm;
    for (u32 i = 0; i < cap; i++)
        if (t[i].addr && addr >= t[i].addr && addr < t[i].addr + t[i].size)
            return true;
    return false;
}

/* ---- back to the game selector (CTX_FLAG_RETURN, boot.c) ----
 * The direct memory allocated when the title's boot begins (after the
 * host's own went back), and once the title has stopped, everything
 * allocated since given back: the title's, its modules', the extension's.
 * Ranges are compared page by page, so allocations the query merges with
 * older ones still separate. */
#define MARK_MAX 1024
static struct drange {
    u64 s, e;
} *g_mark;
static u32 g_nmark;

/* the next allocated range at or after *off: 0, or -1 when there is none */
static int dmem_next(u64 *off, struct drange *r)
{
    static int (*dmq)(off_t, int, void *, size_t);
    struct {
        off_t start, end;
        int type;
    } q;
    if (!dmq)
        dmq = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelDirectMemoryQuery");
    for (u64 top = K.direct_size(); dmq && *off < top;) {
        memset(&q, 0, sizeof(q));
        if (dmq((off_t)*off, 1 /* find next */, &q, sizeof(q)) == 0) {
            r->s = (u64)q.start, r->e = *off = (u64)q.end;
            return 0;
        }
        *off = ALIGN_DOWN(*off, 1UL << 20) + (1UL << 20); /* as host_dmem_census */
    }
    return -1;
}

void dmem_mark(void)
{
    u64 off = 0;
    if (!g_mark && !(g_mark = page_alloc(MARK_MAX * sizeof(*g_mark))))
        return;
    for (g_nmark = 0; g_nmark < MARK_MAX && dmem_next(&off, &g_mark[g_nmark]) == 0;)
        g_nmark++;
    logf("back: %u direct memory ranges allocated as the title's boot begins", g_nmark);
}

u64 dmem_release_new(void)
{
    int (*rel)(off_t, size_t) = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelReleaseDirectMemory");
    struct drange cur, go[256];
    struct vq_info {
        u64 start, end, offset;
        s32 prot, type;
        u8 flags;
        char name[32];
    } __attribute__((packed)) vq;
    u64 off = 0, freed = 0;
    u32 n = 0;
    while (g_mark && n < ARRAY_SIZE(go) && dmem_next(&off, &cur) == 0)
        for (u64 a = cur.s; a < cur.e && n < ARRAY_SIZE(go);) {
            u64 b = cur.e;
            bool kept = false;
            for (u32 i = 0; i < g_nmark && !kept; i++)
                if (g_mark[i].s <= a && a < g_mark[i].e)
                    a = g_mark[i].e, kept = true;
                else if (g_mark[i].s > a && g_mark[i].s < b)
                    b = g_mark[i].s;
            if (!kept)
                go[n++] = (struct drange){a, b}, a = b;
        }
    /* this process's mappings of it, then the memory */
    for (u64 va = 0x10000; K.virtual_query((void *)va, 1, &vq, sizeof(vq)) == 0 && vq.end > va; va = vq.end)
        for (u32 i = 0; i < n; i++)
            if ((vq.flags & 2) && vq.offset < go[i].e && vq.offset + (vq.end - vq.start) > go[i].s) {
                K.munmap((void *)vq.start, vq.end - vq.start);
                break;
            }
    for (u32 i = 0; i < n && rel; i++)
        if (rel((off_t)go[i].s, go[i].e - go[i].s) == 0)
            freed += go[i].e - go[i].s;
    return freed;
}

void *page_alloc(size_t size)
{
    void *p = direct_alloc(size, PROT_READ | PROT_WRITE, 0);
    if (p)
        memset(p, 0, ALIGN_UP(size, 0x4000));
    return p;
}

void page_free(void *addr, size_t size)
{
    (void)size;
    if (addr)
        direct_free(addr);
}

static u8 *arena_cur, *arena_end;
static volatile int arena_lock;

void spin_lock(volatile int *l)
{
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        while (*l)
            __builtin_ia32_pause();
}

void spin_unlock(volatile int *l)
{
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

void *arena_alloc(size_t size)
{
    size = ALIGN_UP(size, 16);
    spin_lock(&arena_lock);
    if (!arena_cur || arena_cur + size > arena_end) {
        size_t chunk = size > 0x400000 ? ALIGN_UP(size, 0x4000) : 0x400000;
        u8 *fresh = page_alloc(chunk);
        if (!fresh) {
            spin_unlock(&arena_lock);
            return NULL;
        }
        arena_cur = fresh;
        arena_end = fresh + chunk;
    }
    u8 *p = arena_cur;
    arena_cur += size;
    spin_unlock(&arena_lock);
    memset(p, 0, size);
    return p;
}

char *arena_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = arena_alloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}
