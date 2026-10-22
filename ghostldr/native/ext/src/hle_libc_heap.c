/*
 * The malloc family for titles without a libc of their own: homebrew that
 * links the system's libSceLibcInternal.  Inside SWRR that library's heap is SWRR's, with
 * about 38 MiB of flexible memory left for it, thread stacks and host
 * modules together.  Its allocation calls come here instead, to a heap
 * backed by the title's flexible budget (budget.c: flex-as-direct, F = the
 * title's mem param or 448 MiB), as a PS4 sizes a title's libc heap.
 * research/hle-specs/memory.md section 6.6.
 *
 * The family turns itself off when a title module exports malloc (its own
 * libc.prx, whose heap already grows through sceKernelMapNamedFlexibleMemory):
 * those titles keep their libc, and their other modules' libSceLibcInternal
 * imports stay SWRR's.
 *
 * The heap: size classes up to 128 KiB carved from 1-4 MiB slabs (never given
 * back), larger blocks mapped on their own and unmapped on free.  Every block
 * has a 16-byte header before it.  Pointers it did not hand out (strdup's
 * and fopen's, from SWRR's libc) go to SWRR's functions.  One lock.
 */
#include "ext.h"

#define MAGIC 0x48424c47u /* "GLBH" */
#define NCLS 48
#define CLS_LARGE 0xFFFF
#define CLS_ALIGNED 0xFFFE
#define SMALL_MAX (128u << 10)
#define PAGE 0x4000UL
#define E_NOMEM 12
#define E_INVAL 22

struct hdr {
    u32 magic;
    u16 cls;
    u16 reserved;
    u32 off;   /* large: user - mapping base; aligned: user - inner pointer */
    u32 pages; /* large: mapping length in 16 KiB pages */
};

struct region {
    u64 start, end;
    bool large;
};

static struct {
    void *free;
    u8 *bump, *end;
} g_cls[NCLS];

static struct region *g_reg; /* sorted by start */
static u32 g_nreg, g_capreg;
static volatile int g_lock;
static u64 g_mapped, g_inuse, g_peak_mapped, g_peak_inuse;
static u32 g_failures;

/* host (SWRR) functions for pointers that are not ours */
static void *g_host[4];
enum { H_FREE, H_REALLOC, H_USABLE, H_REALLOCALIGN };
static const char *const k_host_names[] = {"free", "realloc", "malloc_usable_size", "reallocalign"};

static void *host_fn(u32 i)
{
    if (!g_host[i])
        g_host[i] = host_real_in("libSceLibcInternal", k_host_names[i]);
    return g_host[i];
}

static void set_errno(int e)
{
    if (K.errno_fn)
        *K.errno_fn() = e;
}

/* ---- size classes: 16..128 by 16, then four steps per power of two ---- */
static u32 cls_size(u32 c)
{
    if (c < 8)
        return 16 * (c + 1);
    u32 k = c - 8, p = 128u << (k / 4);
    return p + (p / 4) * (k % 4 + 1);
}

static u32 cls_of(u64 n)
{
    if (n <= 128)
        return n ? (u32)((n - 1) / 16) : 0;
    u32 c = 8;
    while (c < NCLS && cls_size(c) < n)
        c++;
    return c;
}

/* ---- regions ---- */
static s64 reg_find(u64 p)
{
    u32 lo = 0, hi = g_nreg;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_reg[mid].end <= p)
            lo = mid + 1;
        else if (g_reg[mid].start > p)
            hi = mid;
        else
            return mid;
    }
    return -1;
}

static bool reg_add(u64 start, u64 end, bool large)
{
    if (g_nreg == g_capreg) {
        u32 cap = g_capreg ? g_capreg * 2 : 1024;
        struct region *n = page_alloc((u64)cap * sizeof(*n));
        if (!n)
            return false;
        if (g_reg) {
            memcpy(n, g_reg, (u64)g_nreg * sizeof(*n));
            page_free(g_reg, (u64)g_capreg * sizeof(*n));
        }
        g_reg = n, g_capreg = cap;
    }
    u32 at = g_nreg;
    while (at && g_reg[at - 1].start > start)
        g_reg[at] = g_reg[at - 1], at--;
    g_reg[at] = (struct region){start, end, large};
    g_nreg++;
    return true;
}

static void reg_del(u32 i)
{
    memmove(&g_reg[i], &g_reg[i + 1], (u64)(g_nreg - i - 1) * sizeof(*g_reg));
    g_nreg--;
}

/* the title's flexible memory (budget.c), read-write */
static void *map(u64 len, bool large)
{
    void *a = NULL;
    if (flex_map(&a, len, PROT_READ | PROT_WRITE, 0) != 0) {
        if (g_failures++ < 8)
            logf("libc_heap: no flexible memory for %lu KiB more (%lu MiB mapped, %lu MiB in use)", len >> 10,
                 g_mapped >> 20, g_inuse >> 20);
        return NULL;
    }
    if (!reg_add((u64)a, (u64)a + len, large)) {
        flex_unmap(a, len);
        return NULL;
    }
    g_mapped += len;
    if (g_mapped > g_peak_mapped)
        g_peak_mapped = g_mapped;
    return a;
}

/* ---- allocation (g_lock held) ---- */
static void *alloc_small(u32 c)
{
    u32 bs = cls_size(c) + sizeof(struct hdr);
    u8 *b = g_cls[c].free;
    if (b) {
        g_cls[c].free = *(void **)(b + sizeof(struct hdr));
    } else {
        if (!g_cls[c].bump || g_cls[c].bump + bs > g_cls[c].end) {
            u64 len = ALIGN_UP((u64)bs * 16 > (1UL << 20) ? (u64)bs * 16 : (1UL << 20), PAGE);
            u8 *slab = map(len, false);
            if (!slab)
                return NULL;
            g_cls[c].bump = slab, g_cls[c].end = slab + len;
        }
        b = g_cls[c].bump;
        g_cls[c].bump += bs;
    }
    *(struct hdr *)b = (struct hdr){MAGIC, (u16)c, 0, 0, 0};
    g_inuse += cls_size(c);
    return b + sizeof(struct hdr);
}

static void *alloc_large(u64 n)
{
    u64 len = ALIGN_UP(n + sizeof(struct hdr), PAGE);
    if (len < n || len / PAGE > 0xFFFFFFFFUL)
        return NULL;
    u8 *base = map(len, true);
    if (!base)
        return NULL;
    *(struct hdr *)base = (struct hdr){MAGIC, CLS_LARGE, 0, sizeof(struct hdr), (u32)(len / PAGE)};
    g_inuse += len - sizeof(struct hdr);
    return base + sizeof(struct hdr);
}

static void *alloc(u64 n)
{
    void *p = n <= SMALL_MAX ? alloc_small(cls_of(n)) : alloc_large(n);
    if (g_inuse > g_peak_inuse)
        g_peak_inuse = g_inuse;
    return p;
}

/* the header of one of our blocks, or NULL for a pointer that is not ours;
 * *mine says whether it lies in the heap at all (a block freed twice does) */
static struct hdr *ours2(const void *p, bool *mine)
{
    *mine = p && reg_find((u64)p) >= 0;
    if (!*mine || ((u64)p & 15))
        return NULL;
    struct hdr *h = (struct hdr *)p - 1;
    return h->magic == MAGIC ? h : NULL;
}

static struct hdr *ours(const void *p)
{
    bool mine;
    return ours2(p, &mine);
}

static void bad_pointer(const char *what, const void *p)
{
    static u32 said;
    if (said++ < 8)
        logf("libc_heap: %s(%p): not a live block of the heap (freed already?): ignored", what, p);
}

static u64 usable(const struct hdr *h)
{
    if (h->cls == CLS_LARGE)
        return (u64)h->pages * PAGE - h->off;
    if (h->cls == CLS_ALIGNED) {
        const struct hdr *in = ours((const u8 *)(h + 1) - h->off);
        return in ? usable(in) - h->off : 0;
    }
    return h->cls < NCLS ? cls_size(h->cls) : 0;
}

static void release(struct hdr *h)
{
    if (h->cls == CLS_ALIGNED) {
        struct hdr *in = ours((u8 *)(h + 1) - h->off);
        h->magic = 0;
        if (in)
            release(in);
        return;
    }
    if (h->cls == CLS_LARGE) {
        u8 *base = (u8 *)(h + 1) - h->off;
        u64 len = (u64)h->pages * PAGE;
        s64 r = reg_find((u64)base);
        h->magic = 0;
        if (r >= 0)
            reg_del((u32)r);
        g_inuse -= len - h->off;
        g_mapped -= len;
        flex_unmap(base, len);
        return;
    }
    if (h->cls >= NCLS)
        return;
    h->magic = 0;
    *(void **)(h + 1) = g_cls[h->cls].free;
    g_cls[h->cls].free = h;
    g_inuse -= cls_size(h->cls);
}

/* align > 16: a block with room to move the pointer, and a second header */
static void *alloc_aligned(u64 align, u64 n)
{
    if (align <= 16)
        return alloc(n);
    if (n + align + sizeof(struct hdr) < n)
        return NULL;
    u8 *in = alloc(n + align + sizeof(struct hdr));
    if (!in)
        return NULL;
    u8 *user = (u8 *)ALIGN_UP((u64)in + sizeof(struct hdr), align);
    *((struct hdr *)user - 1) = (struct hdr){MAGIC, CLS_ALIGNED, 0, (u32)(user - in), 0};
    return user;
}

/* ---- the family ---- */
static void *h_malloc(u64 n)
{
    spin_lock(&g_lock);
    void *p = alloc(n);
    spin_unlock(&g_lock);
    if (!p)
        set_errno(E_NOMEM);
    return p;
}

static void h_free(void *p)
{
    if (!p)
        return;
    bool mine;
    spin_lock(&g_lock);
    struct hdr *h = ours2(p, &mine);
    if (h)
        release(h);
    spin_unlock(&g_lock);
    if (mine && !h) {
        bad_pointer("free", p);
    } else if (!mine) {
        void (*f)(void *) = host_fn(H_FREE);
        if (f)
            f(p);
    }
}

static void *h_calloc(u64 n, u64 size)
{
    u64 total = n * size;
    if (size && total / size != n) {
        set_errno(E_NOMEM);
        return NULL;
    }
    void *p = h_malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

static void *h_memalign(u64 align, u64 n)
{
    if (align & (align - 1)) {
        set_errno(E_INVAL);
        return NULL;
    }
    spin_lock(&g_lock);
    void *p = alloc_aligned(align, n);
    spin_unlock(&g_lock);
    if (!p)
        set_errno(E_NOMEM);
    return p;
}

static void *h_reallocalign(void *p, u64 n, u64 align)
{
    if (!p)
        return align > 16 ? h_memalign(align, n) : h_malloc(n);
    bool mine;
    spin_lock(&g_lock);
    struct hdr *h = ours2(p, &mine);
    u64 have = h ? usable(h) : 0;
    spin_unlock(&g_lock);
    if (mine && !h) {
        bad_pointer("realloc", p);
        return NULL;
    }
    if (!h) {
        if (align > 16) {
            void *(*f)(void *, u64, u64) = host_fn(H_REALLOCALIGN);
            return f ? f(p, n, align) : NULL;
        }
        void *(*f)(void *, u64) = host_fn(H_REALLOC);
        return f ? f(p, n) : NULL;
    }
    if (n <= have && !((u64)p & (align - 1)))
        return p;
    void *q = align > 16 ? h_memalign(align, n) : h_malloc(n);
    if (q) {
        memcpy(q, p, have < n ? have : n);
        h_free(p);
    }
    return q;
}

static void *h_realloc(void *p, u64 n) { return h_reallocalign(p, n ? n : 1, 16); }

static void *h_reallocf(void *p, u64 n)
{
    void *q = h_realloc(p, n);
    if (!q)
        h_free(p);
    return q;
}

static int h_posix_memalign(void **out, u64 align, u64 n)
{
    if (!out || align < sizeof(void *) || (align & (align - 1)))
        return E_INVAL;
    spin_lock(&g_lock);
    void *p = alloc_aligned(align, n);
    spin_unlock(&g_lock);
    if (!p)
        return E_NOMEM;
    *out = p;
    return 0;
}

static void *h_aligned_alloc(u64 align, u64 n) { return h_memalign(align, n); }

static u64 h_usable(void *p)
{
    if (!p)
        return 0;
    bool mine;
    spin_lock(&g_lock);
    struct hdr *h = ours2(p, &mine);
    u64 n = h ? usable(h) : 0;
    spin_unlock(&g_lock);
    if (mine)
        return n;
    u64 (*f)(void *) = host_fn(H_USABLE);
    return f ? f(p) : 0;
}

/* SceLibcMallocManagedSize: u16 size, u16 version, u32 pad, then the
 * maximum and current system (mapped) and in-use sizes */
static int h_malloc_stats(void *out)
{
    u64 *s = out;
    if (!s)
        return E_INVAL;
    spin_lock(&g_lock);
    s[1] = g_peak_mapped, s[2] = g_mapped, s[3] = g_peak_inuse, s[4] = g_inuse;
    spin_unlock(&g_lock);
    return 0;
}

static void *h_new(u64 n)
{
    void *p = h_malloc(n ? n : 1);
    if (!p)
        logf("libc_heap: operator new(%lu) failed: no flexible memory left (the title would throw bad_alloc)", n);
    return p;
}

static void *h_new_aligned(u64 n, u64 align) { return h_memalign(align, n ? n : 1); }
static void *h_new_nothrow(u64 n, const void *nt) { return h_malloc(n ? n : 1); }
static void *h_new_aligned_nothrow(u64 n, u64 align, const void *nt) { return h_memalign(align, n ? n : 1); }
static void h_delete(void *p) { h_free(p); }

/* on only when no title module defines malloc */
static int heap_init(void)
{
    char nid[16];
    compute_nid("malloc", nid);
    size_t nl = strlen(nid);
    for (struct module *m = g_modules; m; m = m->next)
        for (u64 i = 0; m->symtab && i < m->nsyms; i++) {
            const struct elf_sym *s = &m->symtab[i];
            const char *name = s->name < m->strsz ? m->strtab + s->name : "";
            if (s->shndx && strncmp(name, nid, nl) == 0 && (name[nl] == '#' || !name[nl])) {
                logf("libc_heap: %s exports malloc: the title's own libc keeps its heap", m->name);
                return 1;
            }
        }
    logf("libc_heap: no title module exports malloc: libSceLibcInternal's malloc family goes to ghostldr's heap "
         "(the title's flexible memory)");
    return 0;
}

#define LC "libSceLibcInternal"
static const struct ext_hle_ent k_hle[] = {
    {LC, "malloc", h_malloc, NULL},
    {LC, "free", h_free, NULL},
    {LC, "calloc", h_calloc, NULL},
    {LC, "realloc", h_realloc, NULL},
    {LC, "reallocf", h_reallocf, NULL},
    {LC, "reallocalign", h_reallocalign, NULL},
    {LC, "memalign", h_memalign, NULL},
    {LC, "posix_memalign", h_posix_memalign, NULL},
    {LC, "aligned_alloc", h_aligned_alloc, NULL},
    {LC, "malloc_usable_size", h_usable, NULL},
    {LC, "malloc_stats", h_malloc_stats, NULL},
    {LC, "malloc_stats_fast", h_malloc_stats, NULL},
    {LC, "_Znwm", h_new, NULL},
    {LC, "_Znam", h_new, NULL},
    {LC, "_ZnwmRKSt9nothrow_t", h_new_nothrow, NULL},
    {LC, "_ZnamRKSt9nothrow_t", h_new_nothrow, NULL},
    {LC, "_ZnwmSt11align_val_t", h_new_aligned, NULL},
    {LC, "_ZnamSt11align_val_t", h_new_aligned, NULL},
    {LC, "_ZnwmSt11align_val_tRKSt9nothrow_t", h_new_aligned_nothrow, NULL},
    {LC, "_ZnamSt11align_val_tRKSt9nothrow_t", h_new_aligned_nothrow, NULL},
    {LC, "_ZdlPv", h_delete, NULL},
    {LC, "_ZdaPv", h_delete, NULL},
    {LC, "_ZdlPvm", h_delete, NULL},
    {LC, "_ZdaPvm", h_delete, NULL},
    {LC, "_ZdlPvRKSt9nothrow_t", h_delete, NULL},
    {LC, "_ZdaPvRKSt9nothrow_t", h_delete, NULL},
    {LC, "_ZdlPvmRKSt9nothrow_t", h_delete, NULL},
    {LC, "_ZdaPvmRKSt9nothrow_t", h_delete, NULL},
    {LC, "_ZdlPvSt11align_val_t", h_delete, NULL},
    {LC, "_ZdaPvSt11align_val_t", h_delete, NULL},
    {LC, "_ZdlPvmSt11align_val_t", h_delete, NULL},
    {LC, "_ZdaPvmSt11align_val_t", h_delete, NULL},
};

EXT_FAMILY("libc_heap", k_hle, heap_init);
