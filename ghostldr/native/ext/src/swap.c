/*
 * A swap file for the title's CPU-only heaps (a memory plan's "swap"
 * entries, REC_SWAP; docs/compatibility.md).
 *
 * A planned title's heaps are made whole at boot, so a heap cut to fit SWRR
 * fails the moment the title needs more of it.  An entry names one direct
 * allocation (the executable's call site and its size): the title gets all
 * of it as address space, but only `resident_mib` of memory backs it, in
 * 64 KiB pages.  A page is mapped the first time it is touched (the core's
 * crash handler asks swap_fault first, crash.c); once every resident page is
 * in use, the oldest one is written to a swap file (/temp0, else the USB
 * drive) and its memory reused.  A page that was never written out comes
 * back as zeros, as new memory does.
 *
 * Only for memory the CPU alone uses: the GPU reads its heaps by physical
 * page and never faults, so a heap mapped for the GPU is refused.  The kernel
 * doesn't fault either: a read or write syscall whose buffer has a page out
 * would fail, so the file calls the extension routes (hle_file.c) page their
 * buffers in and pin them first (swap_pin, swap_unpin).
 */
#include "ext.h"

#define PAGE 0x10000UL
#define CHUNK 0x200000UL               /* the resident pages come in 2 MiB pieces of direct memory */
#define SWAP_BASE 0x20000000000UL      /* synthetic offsets the title gets (vdm.c's end below 0x140_0000_0000) */
#define SWAP_STRIDE 0x1000000000UL
#define MAX_REGIONS 4
#define ON_FILE 1
#define E_INVAL 0x80020016
#define E_NOMEM 0x8002000c

typedef s32 (*alloc_fn)(s64 start, s64 end, u64 len, u64 align, s32 type, s64 *out);
typedef s32 (*map_fn)(void **addr, u64 len, s32 prot, s32 flags, s64 phys, u64 align);
typedef s32 (*release_fn)(s64 start, u64 len);

struct region {
    const struct prof_swap *w;
    u64 off, len, va;                  /* its synthetic offset; where the title mapped it (0: not yet) */
    s32 prot;
    u32 npages, nslots, used, hand;
    u8 *alias;                         /* the resident pages, mapped for the swap itself */
    u64 *chunk;                        /* each 2 MiB piece's physical offset */
    u32 *page;                         /* per page: its slot + 1, 0 when not resident */
    u32 *owner;                        /* per slot: its page + 1 */
    u8 *flag, *pins;                   /* per page */
    int fd;
    char path[64];
    u64 faults, outs, ins;
    volatile int lock;
};

static struct region g_reg[MAX_REGIONS];
static u32 g_nreg;
static alloc_fn g_alloc;
static map_fn g_map;
static release_fn g_release;

static u64 slot_phys(const struct region *r, u32 s) { return r->chunk[s * PAGE / CHUNK] + s * PAGE % CHUNK; }

static struct region *region_at(u64 addr)
{
    for (u32 i = 0; i < g_nreg; i++)
        if (g_reg[i].va && addr - g_reg[i].va < g_reg[i].len)
            return &g_reg[i];
    return NULL;
}

static bool open_file(struct region *r)
{
    static const char *const dirs[] = {"/temp0", "/ghostldr_usb/ghostldr"};
    for (u32 i = 0; i < ARRAY_SIZE(dirs) && r->fd < 0; i++) {
        fmt(r->path, sizeof(r->path), "%s/ghostldr-swap%u.bin", dirs[i], (u32)(r - g_reg));
        r->fd = K.open(r->path, 0x602, 0666); /* O_RDWR | O_CREAT | O_TRUNC */
    }
    return r->fd >= 0;
}

/* a slot for a page: a new one, else the oldest unpinned page's, written out */
static s64 take_slot(struct region *r)
{
    if (r->used < r->nslots)
        return r->used++;
    if (r->fd < 0 && !open_file(r)) {
        logf("swap: %s: no swap file (/temp0 or the USB drive): its %u MiB are all in use",
             (const char *)(r->w + 1), r->nslots / 16);
        return -1;
    }
    for (u32 n = 0; n < 2 * r->nslots; n++) {
        u32 s = r->hand++ % r->nslots, v = r->owner[s] - 1;
        if (!r->owner[s])
            return s; /* a page-in that failed left it free */
        if (r->pins[v])
            continue;
        u64 va = r->va + (u64)v * PAGE;
        K.mprotect((void *)va, PAGE, PROT_NONE); /* other threads wait in swap_fault until it is back */
        s64 at = (s64)v * (s64)PAGE;
        if (K.lseek(r->fd, at, 0) != at || K.write(r->fd, r->alias + (u64)s * PAGE, PAGE) != (s64)PAGE) {
            K.mprotect((void *)va, PAGE, r->prot);
            logf("swap: %s: writing page %u to %s failed", (const char *)(r->w + 1), v, r->path);
            return -1;
        }
        if (!r->outs++)
            logf("swap: %s: all %u MiB in use: paging out to %s", (const char *)(r->w + 1), r->nslots / 16,
                 r->path);
        r->flag[v] |= ON_FILE;
        r->page[v] = 0;
        return s;
    }
    logf("swap: %s: every resident page is pinned", (const char *)(r->w + 1));
    return -1;
}

/* page p resident (and pinned once more with pin); 0, or -1 */
static int page_in(struct region *r, u32 p, bool pin)
{
    spin_lock(&r->lock);
    int rc = 0;
    if (!r->page[p]) {
        s64 s = take_slot(r);
        if (s < 0) {
            rc = -1;
            goto out;
        }
        u8 *a = r->alias + (u64)s * PAGE;
        if (!(r->flag[p] & ON_FILE))
            memset(a, 0, PAGE);
        else if (K.pread(r->fd, a, PAGE, (s64)p * PAGE) != (s64)PAGE) {
            logf("swap: %s: reading page %u from %s failed", (const char *)(r->w + 1), p, r->path);
            r->owner[s] = 0; /* free for the next one */
            rc = -1;
            goto out;
        }
        void *va = (void *)(r->va + (u64)p * PAGE);
        if (K.map_direct(&va, PAGE, r->prot, MAP_FIXED, (s64)slot_phys(r, (u32)s), PAGE) != 0) {
            logf("swap: %s: mapping page %u failed", (const char *)(r->w + 1), p);
            r->owner[s] = 0;
            rc = -1;
            goto out;
        }
        r->page[p] = (u32)s + 1;
        r->owner[s] = p + 1;
        r->ins++;
    }
    if (pin)
        r->pins[p]++;
out:
    spin_unlock(&r->lock);
    return rc;
}

/* the core's crash handler, for any fault: 1 when it was a page of a swapped
 * heap (now resident: the access is tried again) */
static int swap_fault(u64 addr)
{
    static u64 last;
    static u32 again;
    struct region *r = region_at(addr);
    if (!r)
        return 0;
    u32 p = (u32)((addr - r->va) / PAGE);
    /* resident and faulting still, again and again: not the swap's (a write
     * where the title mapped it read-only): the crash report says where */
    again = r->page[p] && addr == last ? again + 1 : 0, last = addr;
    if (again > 64)
        return 0;
    r->faults++;
    return page_in(r, p, false) == 0;
}

/* each page of a swapped heap that [buf, buf + len) touches, counted from
 * the heap's start (its address need not be a multiple of a page) */
static void each_page(const void *buf, u64 len, void (*fn)(struct region *r, u32 p))
{
    for (u64 a = (u64)buf, end = (u64)buf + len; a < end;) {
        struct region *r = region_at(a);
        if (!r) {
            a = (a | (PAGE - 1)) + 1;
            continue;
        }
        u32 p = (u32)((a - r->va) / PAGE);
        fn(r, p);
        a = r->va + (u64)(p + 1) * PAGE;
    }
}

static void pin_page(struct region *r, u32 p)
{
    if (page_in(r, p, true) == 0)
        (void)*(volatile u8 *)(r->va + (u64)p * PAGE); /* touched once: populated before the kernel copies */
}

static void unpin_page(struct region *r, u32 p)
{
    spin_lock(&r->lock);
    if (r->pins[p])
        r->pins[p]--;
    spin_unlock(&r->lock);
}

/* a syscall's buffer: its pages resident and pinned, until swap_unpin */
HIDDEN void swap_pin(const void *buf, u64 len) { each_page(buf, len, pin_page); }
HIDDEN void swap_unpin(const void *buf, u64 len) { each_page(buf, len, unpin_page); }

/* for the crash report and the end of the title's run */
HIDDEN void swap_log(void)
{
    for (u32 i = 0; i < g_nreg; i++) {
        const struct region *r = &g_reg[i];
        logf("swap: %s: %lu MiB, %u MiB resident (%u in use): %lu faults, %lu pages in, %lu out%s%s",
             (const char *)(r->w + 1), r->len >> 20, r->nslots / 16, r->used / 16, r->faults, r->ins, r->outs,
             r->fd >= 0 ? " to " : "", r->fd >= 0 ? r->path : "");
    }
}

/* the title's run is over: nothing faults into the regions any more */
HIDDEN void swap_end(void)
{
    if (!g_nreg)
        return;
    *G->swap_fault = NULL;
    swap_log();
    for (u32 i = 0; i < g_nreg; i++) {
        struct region *r = &g_reg[i];
        if (r->fd >= 0)
            K.close(r->fd);
        if (r->va)
            K.munmap((void *)r->va, r->len);
        if (r->alias)
            K.munmap(r->alias, (u64)r->nslots * PAGE);
        r->fd = -1, r->va = 0;
    }
    g_nreg = 0;
}

static struct region *region_of_off(s64 off)
{
    for (u32 i = 0; i < g_nreg; i++)
        if ((u64)off - g_reg[i].off < g_reg[i].len)
            return &g_reg[i];
    return NULL;
}

/* the resident pages: 2 MiB pieces of direct memory, mapped back to back for the swap */
static bool make_pool(struct region *r)
{
    u64 size = (u64)r->w->resident_mib << 20, n = size / CHUNK;
    r->nslots = (u32)(size / PAGE);
    r->npages = (u32)(r->len / PAGE);
    r->chunk = page_alloc(n * 8);
    r->page = page_alloc(r->npages * 4UL);
    r->owner = page_alloc(r->nslots * 4UL);
    r->flag = page_alloc(r->npages);
    r->pins = page_alloc(r->npages);
    void *a = NULL;
    if (!r->chunk || !r->page || !r->owner || !r->flag || !r->pins || K.reserve_va(&a, size, 0, CHUNK) != 0)
        return false;
    r->alias = a;
    for (u64 i = 0; i < n; i++) {
        off_t phys = 0;
        void *at = r->alias + i * CHUNK;
        if (K.alloc_direct(0, (s64)K.direct_size(), CHUNK, CHUNK, 0, &phys) != 0 ||
            K.map_direct(&at, CHUNK, PROT_READ | PROT_WRITE, MAP_FIXED, phys, CHUNK) != 0)
            return false;
        r->chunk[i] = (u64)phys;
    }
    return true;
}

/* sceKernelAllocateDirectMemory: an allocation a swap entry names gets a
 * synthetic offset and its resident pages; any other goes to the core
 * (a tail call: hle.c sees the title's call site) */
static s32 swap_alloc(s64 start, s64 end, u64 len, u64 align, s32 type, s64 *out)
{
    u64 site = g_main_mod ? (u64)__builtin_return_address(0) - g_main_mod->base : 0;
    const struct prof_swap *w = NULL;
    for (u32 i = 0; !w && (w = G->profile_record(REC_SWAP, i)); i++)
        if (w->site != site || (u64)w->len_mib << 20 != len)
            w = NULL;
    if (!w || g_nreg == MAX_REGIONS || !out)
        __attribute__((musttail)) return g_alloc(start, end, len, align, type, out);
    struct region *r = &g_reg[g_nreg];
    *r = (struct region){.w = w, .off = SWAP_BASE + g_nreg * SWAP_STRIDE, .len = len, .fd = -1};
    if (!make_pool(r)) {
        logf("swap: %s: no memory for its %u resident MiB", (const char *)(w + 1), w->resident_mib);
        return (s32)E_NOMEM;
    }
    g_nreg++;
    *G->swap_fault = swap_fault;
    *out = (s64)r->off;
    logf("AllocateDirectMemory(len %lx, type %d) from eboot+%lx: %s, %u of its %u MiB resident, the rest in a swap file",
         len, type, site, (const char *)(w + 1), w->resident_mib, w->len_mib);
    return 0;
}

/* sceKernelMapDirectMemory: a swapped heap's offset gets address space only;
 * its pages are mapped as they are touched */
static s32 swap_map(void **addr, u64 len, s32 prot, s32 flags, s64 phys, u64 align)
{
    struct region *r = region_of_off(phys);
    if (!r)
        __attribute__((musttail)) return g_map(addr, len, prot, flags, phys, align);
    if ((u64)phys != r->off || len != r->len || r->va || (prot & ~(PROT_READ | PROT_WRITE))) {
        logf("swap: %s: mapped as %lx+%lx prot %x: only whole, once, CPU read-write", (const char *)(r->w + 1),
             (u64)phys - r->off, len, prot);
        return (s32)E_INVAL;
    }
    void *va = addr ? *addr : NULL;
    s32 rc = K.reserve_va(&va, len, flags & MAP_FIXED, align > PAGE ? align : PAGE);
    if (rc == 0) {
        r->prot = prot, r->va = (u64)va;
        if (addr)
            *addr = va;
    }
    logf("MapDirectMemory(%s, len %lx, prot %x) -> %x at %p: its pages are mapped as they are used",
         (const char *)(r->w + 1), len, prot, rc, va);
    return rc;
}

static s32 swap_release(s64 start, u64 len)
{
    if (!region_of_off(start))
        __attribute__((musttail)) return g_release(start, len);
    return 0; /* its pages go back with the title's memory (title_back.c) */
}

static int swap_init(void)
{
    if (!G->profile_record || !G->swap_fault || !G->profile_record(REC_SWAP, 0))
        return -1; /* no swap entries (or not this build): the core's own calls stay */
    g_alloc = (alloc_fn)hle_lookup("libkernel", "sceKernelAllocateDirectMemory");
    g_map = (map_fn)hle_lookup("libkernel", "sceKernelMapDirectMemory");
    g_release = (release_fn)hle_lookup("libkernel", "sceKernelReleaseDirectMemory");
    return g_alloc && g_map && g_release ? 0 : -1;
}

static const struct ext_hle_ent k_hle[] = {
    {"libkernel", "sceKernelAllocateDirectMemory", swap_alloc, NULL},
    {"libkernel", "sceKernelMapDirectMemory", swap_map, NULL},
    {"libkernel", "sceKernelReleaseDirectMemory", swap_release, NULL},
};

EXT_FAMILY("swap", k_hle, swap_init);
