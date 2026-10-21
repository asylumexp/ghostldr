/* Allocation for the loader itself: page allocations from flexible memory and
 * a spinlocked bump arena for small, long-lived metadata. */
#include "ghost.h"

/* Page allocations come from direct memory: SWRR leaves only ~37 MiB of
 * flexible memory but several GiB of direct memory. */
struct dm_ent {
    u64 addr, size;
    off_t phys;
};
static struct dm_ent g_dm[256];
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
    for (u32 i = 0; i < ARRAY_SIZE(g_dm); i++)
        if (!g_dm[i].addr) {
            g_dm[i] = (struct dm_ent){(u64)addr, size, phys};
            break;
        }
    spin_unlock(&g_dm_lock);
    return addr;
}

void direct_free(void *addr)
{
    int (*rel)(off_t, size_t) = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelReleaseDirectMemory");
    spin_lock(&g_dm_lock);
    for (u32 i = 0; i < ARRAY_SIZE(g_dm); i++)
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

bool direct_is_ours(u64 addr)
{
    for (u32 i = 0; i < ARRAY_SIZE(g_dm); i++)
        if (g_dm[i].addr && addr >= g_dm[i].addr && addr < g_dm[i].addr + g_dm[i].size)
            return true;
    return false;
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
