/*
 * Scattered direct-memory allocations.
 *
 * After hostmem.c frees the emulator's buffers, SWRR's direct memory is free
 * in a few separate holes (SWRR's own heap and display buffers sit between
 * them), so a large contiguous request from the title can fail although
 * enough memory is free.  Nothing in the title needs physical contiguity:
 * the CPU and the GPU both reach the memory through virtual mappings.
 *
 * When the kernel cannot satisfy a request in one piece, build it from
 * several aligned pieces and hand the title a synthetic offset above the
 * real direct-memory range.  Map/BatchMap/Release translate synthetic
 * offsets back to the pieces and map them back to back.
 *
 * Lazy allocations (budget.c picks them: reservations the title fills only
 * partly) get no physical memory up front; each 2 MiB chunk is committed the
 * first time any part of it is mapped.
 */
#include "ghost.h"

void *host_real(const char *name);
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define VDM_BASE 0x4000000000UL
#define VDM_STRIDE 0x400000000UL /* 16 GiB of synthetic offsets per allocation */
#define VDM_MAX 16
#define VDM_PARTS 24

struct vdm_part {
    u64 phys, len;
};
#define LAZY_CHUNK 0x200000UL
struct vdm {
    u64 len;
    u32 n;
    struct vdm_part p[VDM_PARTS];
    bool lazy;
    int type;
    u64 *chunk; /* lazy: physical offset of each 2 MiB chunk, 0 = not committed */
    u32 committed;
};
static struct vdm g_vdm[VDM_MAX];

bool vdm_is(u64 off)
{
    return off >= VDM_BASE && off < VDM_BASE + VDM_MAX * VDM_STRIDE;
}

static struct vdm *vdm_of(u64 off, u64 *rel)
{
    u64 i = (off - VDM_BASE) / VDM_STRIDE;
    *rel = (off - VDM_BASE) % VDM_STRIDE;
    return i < VDM_MAX && (g_vdm[i].n || g_vdm[i].lazy) ? &g_vdm[i] : NULL;
}

static int release_real(u64 phys, u64 len)
{
    return ((int (*)(off_t, size_t))host_real("sceKernelReleaseDirectMemory"))((off_t)phys, len);
}

int vdm_alloc(u64 start, u64 end, u64 len, u64 align, int type, off_t *out)
{
    int (*alloc)(off_t, off_t, size_t, size_t, int, off_t *) =
        (int (*)(off_t, off_t, size_t, size_t, int, off_t *))host_real("sceKernelAllocateDirectMemory");
    u32 slot = 0;
    while (slot < VDM_MAX && (g_vdm[slot].n || g_vdm[slot].lazy))
        slot++;
    if (slot == VDM_MAX)
        return 0x8002000c;
    if (align < 0x10000)
        align = 0x10000;
    struct vdm *v = &g_vdm[slot];
    u64 left = len;
    while (left && v->n < VDM_PARTS) {
        off_t s = 0;
        size_t avail = 0;
        if (K.avail_direct((off_t)start, (off_t)end, align, &s, &avail) != 0)
            break;
        u64 take = ALIGN_DOWN(MIN((u64)avail, left), align);
        off_t phys = 0;
        if (!take || alloc((off_t)start, (off_t)end, take, align, type, &phys) != 0)
            break;
        v->p[v->n++] = (struct vdm_part){(u64)phys, take};
        left -= take;
    }
    if (left) {
        for (u32 i = 0; i < v->n; i++)
            release_real(v->p[i].phys, v->p[i].len);
        memset(v, 0, sizeof(*v));
        return 0x80020023; /* ENOMEM-style failure, as the kernel reports */
    }
    v->len = len;
    *out = (off_t)(VDM_BASE + slot * VDM_STRIDE);
    logf("vdm: %lx bytes (type %d) as %u pieces -> synthetic offset %lx", len, type, v->n, (u64)*out);
    for (u32 i = 0; i < v->n; i++)
        logf("vdm:   piece %u phys %lx len %lx", i, v->p[i].phys, v->p[i].len);
    return 0;
}

int vdm_alloc_lazy(u64 len, int type, off_t *out)
{
    u32 slot = 0;
    while (slot < VDM_MAX && (g_vdm[slot].n || g_vdm[slot].lazy))
        slot++;
    if (slot == VDM_MAX)
        return 0x8002000c;
    struct vdm *v = &g_vdm[slot];
    u32 n = (u32)(ALIGN_UP(len, LAZY_CHUNK) / LAZY_CHUNK);
    v->chunk = page_alloc(n * sizeof(u64));
    if (!v->chunk)
        return 0x8002000c;
    v->lazy = true, v->type = type, v->len = len, v->committed = 0;
    *out = (off_t)(VDM_BASE + slot * VDM_STRIDE);
    logf("vdm: lazy %lx bytes (type %d, %u chunks) -> synthetic offset %lx", len, type, n, (u64)*out);
    return 0;
}

u32 vdm_lazy_committed_mib(void);

static int commit_chunk(struct vdm *v, u64 ci)
{
    static u32 failures;
    off_t phys = 0;
    int rc = ((int (*)(off_t, off_t, size_t, size_t, int, off_t *))host_real("sceKernelAllocateDirectMemory"))(
        0, (off_t)K.direct_size(), LAZY_CHUNK, LAZY_CHUNK, v->type, &phys);
    if (rc != 0) {
        if (failures++ < 8) {
            off_t s = 0;
            size_t avail = 0;
            K.avail_direct(0, (off_t)K.direct_size(), 0x10000, &s, &avail);
            logf("vdm: lazy commit of chunk %lu (type %d) failed %x after %u chunks: out of direct memory (%u MiB of "
                 "lazy reservations committed, largest free block %lu KiB)",
                 ci, v->type, rc, v->committed, vdm_lazy_committed_mib(), (u64)avail >> 10);
        }
        return rc;
    }
    v->chunk[ci] = (u64)phys;
    v->committed++;
    return 0;
}

u32 vdm_lazy_committed_mib(void)
{
    u32 n = 0;
    for (u32 i = 0; i < VDM_MAX; i++)
        if (g_vdm[i].lazy)
            n += g_vdm[i].committed * (u32)(LAZY_CHUNK >> 20);
    return n;
}

void vdm_log_lazy(void)
{
    for (u32 i = 0; i < VDM_MAX; i++)
        if (g_vdm[i].lazy)
            logf("watchdog: lazy %lx (type %d): %u of %lu MiB committed", VDM_BASE + i * VDM_STRIDE, g_vdm[i].type,
                 g_vdm[i].committed * (u32)(LAZY_CHUNK >> 20), g_vdm[i].len >> 20);
}

/* Call fn(va, phys, len) for each physical piece behind [off, off+len). */
static int vdm_walk(u64 off, u64 len, u64 va, int (*fn)(u64, u64, u64, void *), void *arg)
{
    u64 rel;
    struct vdm *v = vdm_of(off, &rel);
    if (!v || rel + len > v->len)
        return 0x80020016; /* EINVAL */
    if (v->lazy) {
        while (len) {
            u64 ci = rel / LAZY_CHUNK, inner = rel % LAZY_CHUNK;
            if (!v->chunk[ci]) {
                int rc = commit_chunk(v, ci);
                if (rc)
                    return rc;
            }
            u64 n = MIN(LAZY_CHUNK - inner, len);
            int rc = fn(va, v->chunk[ci] + inner, n, arg);
            if (rc)
                return rc;
            va += n, rel += n, len -= n;
        }
        return 0;
    }
    u64 pos = 0;
    for (u32 i = 0; i < v->n && len; i++) {
        u64 ps = pos, pe = pos + v->p[i].len;
        pos = pe;
        if (rel >= pe)
            continue;
        u64 skip = rel > ps ? rel - ps : 0;
        u64 n = MIN(v->p[i].len - skip, len);
        int rc = fn(va, v->p[i].phys + skip, n, arg);
        if (rc)
            return rc;
        va += n, rel += n, len -= n;
    }
    return 0;
}

static int map_piece(u64 va, u64 phys, u64 len, void *arg)
{
    void *addr = (void *)va;
    int prot = (int)(u64)arg;
    return ((int (*)(void **, size_t, int, int, off_t, size_t))host_real("sceKernelMapDirectMemory"))(
        &addr, len, prot, MAP_FIXED, (off_t)phys, 0);
}

int vdm_map(void **addr, u64 len, int prot, int flags, u64 off, u64 align)
{
    if (!(flags & MAP_FIXED)) {
        int (*reserve)(void **, size_t, int, size_t) =
            host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelReserveVirtualRange");
        int rc = reserve ? reserve(addr, len, 0, align ? align : 0x200000) : 0x8002000c;
        if (rc)
            return rc;
    }
    return vdm_walk(off, len, (u64)*addr, map_piece, (void *)(u64)prot);
}

int vdm_release(u64 off, u64 len)
{
    u64 rel;
    struct vdm *v = vdm_of(off, &rel);
    if (!v || rel != 0 || len != v->len)
        return 0x80020016;
    if (v->lazy) {
        u32 n = (u32)(ALIGN_UP(v->len, LAZY_CHUNK) / LAZY_CHUNK);
        for (u32 i = 0; i < n; i++)
            if (v->chunk[i])
                release_real(v->chunk[i], LAZY_CHUNK);
        page_free(v->chunk, n * sizeof(u64));
        memset(v, 0, sizeof(*v));
        return 0;
    }
    for (u32 i = 0; i < v->n; i++)
        release_real(v->p[i].phys, v->p[i].len);
    memset(v, 0, sizeof(*v));
    return 0;
}

/* Expand BatchMap entries that map synthetic offsets into per-piece ones. */
struct batch_entry {
    void *start;
    off_t offset;
    size_t length;
    u8 prot, type;
    s16 reserved;
    s32 op;
};

struct expand {
    struct batch_entry *out;
    u32 n, cap;
    const struct batch_entry *src;
};

static int add_piece(u64 va, u64 phys, u64 len, void *arg)
{
    struct expand *x = arg;
    if (x->n == x->cap)
        return 0x8002000c;
    x->out[x->n] = *x->src;
    x->out[x->n].start = (void *)va;
    x->out[x->n].offset = (off_t)phys;
    x->out[x->n].length = len;
    x->n++;
    return 0;
}

/* Translate one entry; returns the number of entries written to out (or -1
 * when they do not fit / a lazy commit fails).  Callers flush out between
 * entries, so cap only has to hold one entry's pieces. */
int vdm_expand_entry(const void *entry, void *out, int cap)
{
    const struct batch_entry *e = entry;
    struct expand x = {out, 0, (u32)cap, NULL};
    if (e->op == 0 && vdm_is((u64)e->offset)) {
        x.src = e;
        if (vdm_walk((u64)e->offset, e->length, (u64)e->start, add_piece, &x))
            return -1;
    } else {
        x.out[x.n++] = *e;
    }
    return (int)x.n;
}
