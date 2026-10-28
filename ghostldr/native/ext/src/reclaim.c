/*
 * The planner's reclaim (the core's bigexec.c asks for it through struct
 * ext_early; the interface is ghost_api.h struct bx_reclaim).  It is here,
 * not in the core, because the core must fit the IPU buffer.
 *
 * A module whose code no set of whole emulator buffers covers runs from
 * fresh JIT objects sized to its code instead.  There is no room for those
 * beside the buffers: the console has one global cap on JIT memory, shared
 * by the compiler and SWRR, and the emulator's buffers hold nearly all of it.
 * So every reference to them is dropped first, and an object's memory comes
 * back when its last reference goes.  Measured on a 13.60 PS5
 * (docs/compatibility.md, "Large executables"):
 *  - the cap is 65 MiB: the buffers (32 + 16 + 8 + 8 MiB) and the core's
 *    256 KiB object leave 768 KiB; with the four dropped, 64.75 MiB of
 *    fresh objects can be made, in any 16 KiB multiple, one of them that
 *    large;
 *  - each buffer has three compiler descriptors (base, RX alias, RW alias),
 *    one here, the compiler's mapping at its home and the view at its home
 *    here.  Missing one only means its memory does not come back, which the
 *    measurement after the drop shows;
 *  - nothing resizes: ftruncate and the like are refused.
 * The PS2 emulation is dead afterwards; a boot does not need it.
 *
 * What is free is measured (budget: the largest object the compiler can
 * create) before anything is dropped and after; nothing is dropped unless
 * the measured memory plus the buffers holds the plan.  The descriptors are
 * found by content: the compiler's clock_gettime writes a time stamp into
 * each buffer (past its header, where the compiler writes), and a read-only
 * view of each candidate descriptor shows whose stamp it carries.
 */
#include "ext.h"
#include "bxplan.h"

#define MiB (1UL << 20)
#define SYS_close 6
#define SYS_munmap 73
#define SYS_clock_gettime 232
#define TAG_OFF 0x8000           /* past every buffer's header (VU0 and VU1: 32 KiB) */
#define FD_MAX 1024
#define BUDGET_MAX (128UL << 20) /* above the cap: a create this large must be refused */
#define MAX_DROP 8
/* compiler-visible arguments: the core's bigexec.c scratch (jitrpc.c's
 * private area at 0x300-0x30f), used one call at a time */
#define OUT(off) (g_ctx->oob_scratch + 0x70000 + 0x300 + (off))

static u64 c_sys(u64 n, u64 a1, u64 a2) { return jit_call(g_ctx->jit_syscall, n, a1, a2, 0, 0, 0, 0); }
static void c_close(int fd)
{
    if (fd >= 0)
        c_sys(SYS_close, (u64)fd, 0);
}

static int c_alias(int fd, int prot, int *out)
{
    *(volatile u64 *)OUT(8) = (u64)-1;
    int rc = (int)jit_call(g_ctx->jit_shm_alias, 0, (u64)fd, (u64)prot, OUT(8), 0, 0, 0);
    *out = rc == 0 ? (int)*(volatile u32 *)OUT(8) : -1;
    return rc;
}

/* a new object in the compiler (its base descriptor), read-write-execute */
static int c_create(u64 size, int *fd)
{
    memcpy((void *)OUT(0), "ghostbx", 8);
    *(volatile u64 *)OUT(8) = (u64)-1;
    int rc = (int)jit_call(g_ctx->jit_shm_create, 0, OUT(0), size, PROT_READ | PROT_WRITE | PROT_EXEC, OUT(8), 0, 0);
    *fd = rc == 0 ? (int)*(volatile u32 *)OUT(8) : -1;
    return rc;
}

/* The JIT memory the console gives now: the largest object the compiler can
 * create, to 16 KiB (smaller creates are refused).  Each test object is
 * closed at once, which gives its memory back.  *refused: the last refusal. */
static u64 budget(u32 *refused)
{
    u64 lo = 0, hi = BUDGET_MAX;
    int fd, rc;
    if ((rc = c_create(hi, &fd)) == 0) {
        c_close(fd);
        logf("reclaim: a %lu MiB JIT object was created: no cap below that", hi / MiB);
        *refused = 0;
        return hi;
    }
    *refused = (u32)rc;
    while (hi - lo > 0x4000) {
        u64 mid = ALIGN_DOWN(lo + (hi - lo) / 2, 0x4000);
        if ((rc = c_create(mid, &fd)) == 0)
            c_close(fd), lo = mid;
        else
            *refused = (u32)rc, hi = mid;
    }
    return lo;
}

/* a reservation over [va, va + len): evicts a view there, keeps the range */
static int evict(u64 va, u64 len)
{
    void *a = (void *)va;
    if (K.reserve_va(&a, len, MAP_FIXED, 0x4000) == 0 && (u64)a == va)
        return 0;
    K.munmap((void *)va, len);
    a = (void *)va;
    return K.reserve_va(&a, len, 0, 0x4000) == 0 && (u64)a == va ? 0 : -1;
}

/* a JIT object's size behind a descriptor here, and its file type */
static u64 fd_size(int fd, u32 *mode)
{
    u8 st[0x78];
    if (fd < 0 || K.fstat(fd, st) != 0)
        return 0;
    *mode = *(u16 *)(st + 8) & 0xF000;
    return *(u64 *)(st + 0x48);
}

struct drop {
    u64 home, size, tag[2];
    int cfd[6], hfd[6]; /* its descriptors in the compiler and here */
    u32 ncfd, nhfd;
};

/* which buffer of d a JIT object descriptor here is (n: none), by the stamp
 * at TAG_OFF of a read-only view of all of it at va (views are whole) */
static u32 which(const struct drop *d, u32 n, int fd, u64 va)
{
    u32 mode = 0;
    u64 size = fd_size(fd, &mode), tag[2] = {0, 0};
    bool seen = false;
    for (u32 i = 0; i < n && mode == 0x2000; i++) {
        if (d[i].size != size || !d[i].tag[0])
            continue;
        if (!seen) {
            void *res = NULL;
            int rc = K.mmap((void *)va, size, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0, &res);
            if (rc == 0 && (u64)res == va)
                tag[0] = *(volatile u64 *)(va + TAG_OFF), tag[1] = *(volatile u64 *)(va + TAG_OFF + 8);
            evict(va, size);
            if (rc != 0 || (u64)res != va)
                return n;
            seen = true;
        }
        if (tag[0] == d[i].tag[0] && tag[1] == d[i].tag[1])
            return i;
    }
    return n;
}

/* Every descriptor of the buffers in d: the compiler's (each that takes a
 * read-only alias) and this process's.  Read-only throughout. */
static void find_refs(struct drop *d, u32 n, u64 va)
{
    for (int fd = 0; fd < FD_MAX; fd++) {
        int a = -1;
        if (c_alias(fd, PROT_READ, &a) != 0)
            continue;
        int h = jit_recv_fd(a);
        c_close(a);
        u32 i = h >= 0 ? which(d, n, h, va) : n;
        if (h >= 0)
            K.close(h);
        if (i < n && d[i].ncfd < ARRAY_SIZE(d[i].cfd))
            d[i].cfd[d[i].ncfd++] = fd;
    }
    for (int fd = 0; fd < FD_MAX; fd++) {
        u32 i = which(d, n, fd, va);
        if (i < n && d[i].nhfd < ARRAY_SIZE(d[i].hfd))
            d[i].hfd[d[i].nhfd++] = fd;
    }
}

/* every reference to a buffer: its descriptors, the compiler's mapping at
 * its home, the view at its home here (a reservation from now on) */
static void drop(const struct drop *d)
{
    for (u32 i = 0; i < d->ncfd; i++)
        c_close(d->cfd[i]);
    for (u32 i = 0; i < d->nhfd; i++)
        K.close(d->hfd[i]);
    s64 m = (s64)c_sys(SYS_munmap, d->home, d->size);
    int e = evict(d->home, d->size);
    logf("reclaim: the %lu MiB buffer at %lx dropped: %u compiler descriptors, %u here; compiler munmap %ld; its view "
         "here %s", d->size / MiB, d->home, d->ncfd, d->nhfd, m, e ? "NOT replaced" : "replaced by a reservation");
    G->arena_drop(d->home);
}

/* A fresh object: the compiler maps it read-write where it likes (o->wr),
 * and an RX alias of it comes here (o->rx), not mapped yet.  Its compiler
 * descriptors are closed: the mapping and the alias keep it. */
static int fresh(u64 size, struct jit_obj *o)
{
    int base = -1, rw = -1, rxc = -1;
    u64 at = 0;
    *o = (struct jit_obj){0, size, 0, -1, -1};
    int rc = c_create(size, &base);
    if (rc == 0)
        rc = c_alias(base, PROT_READ | PROT_WRITE, &rw);
    if (rc == 0)
        rc = c_alias(base, PROT_READ | PROT_EXEC, &rxc);
    if (rc == 0) {
        *(volatile u64 *)OUT(8) = 0;
        rc = (int)jit_call(g_ctx->jit_shm_map, 0, (u64)rw, PROT_READ | PROT_WRITE, OUT(8), 0, 0, 0);
        at = rc == 0 ? *(volatile u64 *)OUT(8) : 0;
    }
    if (at)
        o->rx = jit_recv_fd(rxc);
    c_close(rxc), c_close(rw), c_close(base);
    if (o->rx < 0) {
        if (at)
            c_sys(SYS_munmap, at, size);
        logf("reclaim: a fresh %lu KiB JIT object: rc %x (compiler map at %lx)", size >> 10, rc, at);
        return -1;
    }
    o->wr = at;
    return 0;
}

int jit_reclaim(struct bx_reclaim *r) HIDDEN;
int jit_reclaim(struct bx_reclaim *r)
{
    u64 st[MAX_DROP], en[MAX_DROP], have = 0, top = 0, code = 0;
    struct drop *d = arena_alloc(MAX_DROP * sizeof(*d));
    u32 n = 0, nb = arena_buffers(st, en, MAX_DROP), refused = 0, big = 0;
    if (!d || !g_ctx->jit_syscall || !g_ctx->jit_shm_create || !G->arena_adopt) {
        fmt(r->why, sizeof(r->why), "%s needs fresh JIT objects, and the compiler cannot make them here",
            r->name);
        return -1;
    }
    /* the plan: an object per code run (bxplan.h), each free to grow up to
     * the next one, the text's end or a data segment */
    struct bx_layout lay;
    if (bx_fresh(r->runs, r->nruns, r->forbid, r->nforbid, r->size, &lay) != 0) {
        fmt(r->why, sizeof(r->why), r->nruns ? "%s: its code overlaps its data" : "%s: no code found in it", r->name);
        return -1;
    }
    r->n = lay.nviews;
    for (u32 v = 0; v < r->n; v++) {
        s64 at = r->off[v] = lay.view[v].off, end = v + 1 < r->n ? lay.view[v + 1].off : (s64)r->text;
        for (u32 f = 0; f < r->nforbid; f++)
            if (r->forbid[f].lo >= at && r->forbid[f].lo < end)
                end = r->forbid[f].lo;
        r->limit[v] = ALIGN_DOWN((u64)(end - at), 0x4000);
        code += r->size[v];
    }
    r->amin = ALIGN_UP(r->keep + JIT_BUF_HEADER, 0x4000);
    if (r->amin < MiB)
        r->amin = MiB; /* larger than the core's region, which arena.c would pick otherwise */
    for (u32 i = 0; i < nb; i++) {
        if ((u64)jit_reclaim - st[i] < en[i] - st[i])
            continue; /* the extension's own block, should it ever be that large */
        d[n] = (struct drop){st[i], en[i] - st[i], {0, 0}, {0}, {0}, 0, 0};
        if (d[n].size > top)
            top = d[n].size, big = n;
        have += d[n++].size;
    }
    r->free_before = budget(&refused);
    logf("reclaim: %s: %lu KiB of code in %u fresh objects + %lu KiB or more for the other modules; free JIT memory "
         "%lu KiB (larger refused: %x), and %u emulator buffers of %lu KiB", r->name, code >> 10, r->n, r->amin >> 10,
         r->free_before >> 10, refused, n, have >> 10);
    if (code + r->amin > r->free_before + have || r->amin > top) {
        fmt(r->why, sizeof(r->why), "%s won't fit: its code needs %lu KiB of JIT memory and the other modules %lu "
            "KiB; with the emulator's buffers reclaimed the console has %lu KiB", r->name, code >> 10, r->amin >> 10,
            (r->free_before + have) >> 10);
        logf("reclaim: %s (nothing was dropped)", r->why);
        return -1;
    }
    /* each buffer's tag: the compiler's clock, written at home + TAG_OFF */
    for (u32 i = 0; i < n; i++) {
        u64 *t = d[i].tag;
        for (u32 tries = 0; tries < 4 && !t[0]; tries++) {
            if ((s64)c_sys(SYS_clock_gettime, 0, d[i].home + TAG_OFF) != 0)
                break;
            t[0] = *(volatile u64 *)(d[i].home + TAG_OFF), t[1] = *(volatile u64 *)(d[i].home + TAG_OFF + 8);
            for (u32 j = 0; j < i; j++)
                if (d[j].tag[0] == t[0] && d[j].tag[1] == t[1])
                    t[0] = 0;
        }
        if (!t[0])
            logf("reclaim: the buffer at %lx took no stamp: its descriptors stay", d[i].home);
    }
    void *va = NULL;
    if (K.reserve_va(&va, top, 0, 0x200000) != 0) {
        fmt(r->why, sizeof(r->why), "%s: no address range to look at the buffers' descriptors", r->name);
        return -1;
    }
    find_refs(d, n, (u64)va);
    K.munmap(va, top);
    for (u32 i = 0; i < n; i++)
        drop(&d[i]);
    r->free_after = budget(&refused);
    logf("reclaim: %lu KiB of JIT memory free now (larger refused: %x)", r->free_after >> 10, refused);
    if (r->free_after < code + r->amin) {
        fmt(r->why, sizeof(r->why), "%s: only %lu KiB of JIT memory came back, and it needs %lu KiB; the PS2 "
            "emulator's buffers are gone: close SWRR and start it again", r->name, r->free_after >> 10,
            (code + r->amin) >> 10);
        return -1;
    }
    /* what is left: room for prx loaded at run time first, then the code
     * objects grow over the text after them (less of it in direct memory),
     * then the rest is the other modules' too */
    u64 left = r->free_after - code, arena = r->amin + BX_RECLAIM_ROOM;
    if (arena > top)
        arena = top;
    if (arena > left)
        arena = left;
    u64 spare = left - arena;
    for (u32 v = 0; v < r->n; v++) {
        u64 g = r->limit[v] > r->size[v] ? r->limit[v] - r->size[v] : 0;
        g = ALIGN_DOWN(g < spare ? g : spare, 0x4000);
        r->size[v] += g, spare -= g, code += g;
    }
    for (u32 v = 0; v < r->n; v++) {
        if (fresh(r->size[v], &r->obj[v]) != 0) {
            fmt(r->why, sizeof(r->why), "%s: a fresh %lu KiB JIT object was refused; close SWRR and start it again",
                r->name, r->size[v] >> 10);
            return -1;
        }
        logf("reclaim: %s: code object %u: %lu KiB, the compiler writes it at %lx", r->name, v, r->size[v] >> 10,
             r->obj[v].wr);
    }
    /* the arena: what is left, up to the largest buffer's size, where it was */
    struct jit_obj a;
    u64 home = d[big].home;
    void *res = NULL;
    left = r->free_after - code;
    r->arena = ALIGN_DOWN(left < top ? left : top, 0x4000);
    if (fresh(r->arena, &a) != 0 ||
        K.mmap((void *)home, r->arena, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED, a.rx, 0, &res) != 0 ||
        (u64)res != home || G->arena_adopt(home, home + r->arena, (s64)(a.wr - home)) != 0) {
        fmt(r->why, sizeof(r->why), "%s: no fresh JIT object for the other modules; close SWRR and start it again",
            r->name);
        return -1;
    }
    K.close(a.rx);
    logf("reclaim: %s: %u fresh objects of %lu KiB for its code and the read-only data after it, %lu KiB at %lx for "
         "the other modules, %lu KiB of JIT memory left", r->name, r->n, code >> 10, r->arena >> 10, home,
         (left - r->arena) >> 10);
    return 0;
}
