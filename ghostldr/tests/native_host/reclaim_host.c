/*
 * Runs the extension's reclaim (ext/src/reclaim.c) in a host process, built
 * for this machine, against a fake 13.60 PS5: the console-wide JIT cap
 * (65 MiB), the emulator's four buffers (three compiler descriptors each,
 * one here, the compiler's mapping and the view here at each one's home),
 * the core's IPU object, the extension's own block, the compiler's
 * create/alias/map calls with their refusals, its close, munmap and
 * clock_gettime, descriptor passing, reservations and whole-object views.
 * An object's memory comes back when its last reference goes.  Addresses
 * "here" are real memory of this process (a reserved area), so the code
 * reads the stamps it asked the compiler to write.  test_bigexec.py drives
 * it:
 *
 *     reclaim_host CASE CODE_KIB[:LIMIT_KIB]... AMIN_KIB
 *
 * CASE: plain; extra (an 8 MiB JIT object that is no buffer, with
 * descriptors on both sides: it must stay; the cap is 8 MiB larger); payload (a view of VU1 the
 * reclaim cannot know of: its memory does not come back); nostamp (the
 * compiler has no mapping at VU0's home); ps4 (the compiler's buffers
 * refuse an RX alias of the RW alias, and so on, as on a 13.60 PS5, but
 * every descriptor number is high).  The result prints as lines.
 */
#include "ghost_api.h" /* not ext.h: its macros name the API's members */

int printf(const char *f, ...);
int vsnprintf(char *buf, unsigned long size, const char *f, __builtin_va_list ap);
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void exit(int code);
unsigned long strtoul(const char *s, char **end, int base);
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);

#if defined(__linux__)
#define MAP_ANON_ 0x20
#else
#define MAP_ANON_ 0x1000
#endif
#define MiB (1UL << 20)
#define KiB 1024UL
static u64 CAP = 65 * MiB; /* the console's; + 8 MiB in case extra (a console where the extra object fits) */

const struct ghost_api *G;
static struct boot_ctx ctx;
static struct boot_ctx *ctxp = &ctx;
static struct kfuncs kf;
static struct ghost_api api;

static void hlog(const char *f, ...)
{
    char line[1000];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    vsnprintf(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    printf("log: %s\n", line);
}

int fmt(char *buf, size_t size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vsnprintf(buf, size, f, ap);
    __builtin_va_end(ap);
    return n;
}

static void fault(const char *what, u64 a, u64 b)
{
    printf("FAULT %s %lx %lx\n", what, a, b);
    exit(3);
}

/* ---- objects and references ---- */
#define NOBJ 32
#define NFD 1024
struct obj {
    const char *name;
    u64 size;
    u64 home;    /* here, where it was first mapped (a buffer's home); 0: none yet */
    u64 chome;   /* the compiler's first mapping of it */
    u8 *bytes;
    bool jit, live;
};
static struct obj g_o[NOBJ];
static u32 g_no;
/* descriptors: kind 1 base, 2 RX alias, 3 RW alias, 4 R alias; 0: closed */
static struct {
    int obj, kind;
} g_cfd[NFD], g_hfd[NFD];
struct map {
    u64 at, len;
    int obj;
    bool rw;
};
static struct map g_cmap[64], g_hmap[64];
static u32 g_ncmap, g_nhmap;
static u64 g_area, g_area_len, g_scratch; /* this process's fake address space (real memory) */
static u64 g_stamp = 1700000000;
static u32 g_creates, g_closed_c, g_closed_h;

static u64 used(void)
{
    u64 n = 0;
    for (u32 i = 0; i < g_no; i++)
        if (g_o[i].jit && g_o[i].live)
            n += g_o[i].size;
    return n;
}

/* an object with no reference left is gone, and its JIT memory with it */
static void collect(void)
{
    for (u32 i = 0; i < g_no; i++) {
        if (!g_o[i].live)
            continue;
        bool ref = false;
        for (u32 f = 0; f < NFD; f++)
            ref |= (g_cfd[f].kind && g_cfd[f].obj == (int)i) || (g_hfd[f].kind && g_hfd[f].obj == (int)i);
        for (u32 m = 0; m < g_ncmap; m++)
            ref |= g_cmap[m].len && g_cmap[m].obj == (int)i;
        for (u32 m = 0; m < g_nhmap; m++)
            ref |= g_hmap[m].len && g_hmap[m].obj == (int)i;
        if (!ref)
            g_o[i].live = false, printf("gone %s (%lu KiB)\n", g_o[i].name, g_o[i].size / KiB);
    }
}

static int new_obj(const char *name, u64 size, bool jit)
{
    struct obj *o = &g_o[g_no];
    *o = (struct obj){name, size, 0, 0, calloc(1, size), jit, true};
    memset(o->bytes, 0x5A, size);
    return (int)g_no++;
}

static int g_next_cfd = 300, g_next_hfd = 40;
static int add_fd(bool compiler, int obj, int kind)
{
    for (int k = 0; k < NFD; k++) {
        int fd = compiler ? (g_next_cfd + k * 37) % NFD : (g_next_hfd + k) % NFD;
        typeof(g_cfd[0]) *t = compiler ? &g_cfd[fd] : &g_hfd[fd];
        if (fd < 3 || t->kind)
            continue;
        t->obj = obj, t->kind = kind;
        if (compiler)
            g_next_cfd = (fd + 101) % NFD;
        return fd;
    }
    fault("out of descriptors", 0, 0);
    return -1;
}

static void map_add(struct map *t, u32 *n, u64 at, u64 len, int obj, bool rw)
{
    for (u32 m = 0; m < *n; m++) /* MAP_FIXED replaces what was there */
        if (t[m].len && t[m].at < at + len && at < t[m].at + t[m].len) {
            if (t[m].at < at || t[m].at + t[m].len > at + len)
                fault("a mapping replaced in part", t[m].at, at);
            t[m].len = 0;
        }
    t[(*n)++] = (struct map){at, len, obj, rw};
    if (*n == 64)
        fault("too many mappings", 0, 0);
}

static struct map *map_at(struct map *t, u32 n, u64 a)
{
    for (u32 m = 0; m < n; m++)
        if (t[m].len && a >= t[m].at && a < t[m].at + t[m].len)
            return &t[m];
    return NULL;
}

/* the views here of an object show its bytes */
static void sync_views(int obj)
{
    for (u32 m = 0; m < g_nhmap; m++)
        if (g_hmap[m].len && g_hmap[m].obj == obj)
            memcpy((void *)g_hmap[m].at, g_o[obj].bytes, g_hmap[m].len);
}

/* ---- the compiler ---- */
#define FN_CREATE 0x1001
#define FN_ALIAS 0x1002
#define FN_MAP 0x1003
#define FN_SYSCALL 0x1004
static u64 g_cnext = 0x904f34000UL; /* where the compiler maps new objects when nothing freed fits */
static u64 scratch[0x80000 / 8];

static u64 c_free_at(u64 size)
{
    /* first fit among the buffers' freed homes, else after them */
    for (u64 a = g_o[1].home; a + size <= g_o[4].home + g_o[4].size; a += 0x4000) {
        bool used_ = false;
        for (u32 m = 0; m < g_ncmap && !used_; m++)
            used_ = g_cmap[m].len && g_cmap[m].at < a + size && a < g_cmap[m].at + g_cmap[m].len;
        if (!used_)
            return a;
    }
    u64 a = g_cnext;
    g_cnext += ALIGN_UP(size, 0x4000);
    return a;
}

static u64 jit_call_(u64 fn, u64 rax, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6)
{
    if (fn == FN_CREATE) {
        g_creates++;
        if (!a2 || (a2 & 0x3FFF))
            return 0x80020016;
        if (used() + a2 > CAP)
            return 0x8002000c;
        int o = new_obj("fresh", a2, true);
        *(u64 *)a4 = (u64)add_fd(true, o, 1);
        return 0;
    }
    if (fn == FN_ALIAS) {
        if (a1 >= NFD || !g_cfd[a1].kind || !g_o[g_cfd[a1].obj].jit)
            return 0x80020009;
        int k = g_cfd[a1].kind, prot = (int)a2;
        if ((k == 3 && (prot & PROT_EXEC)) || (k == 2 && (prot & PROT_WRITE)) || (k == 4 && prot != PROT_READ))
            return 0x8002000d;
        *(u64 *)a3 = (u64)add_fd(true, g_cfd[a1].obj, prot == 3 ? 3 : prot == 5 ? 2 : 4);
        return 0;
    }
    if (fn == FN_MAP) {
        if (a1 >= NFD || g_cfd[a1].kind != 3 || a2 != 3)
            fault("compiler map of other than an RW alias, RW", a1, a2);
        struct obj *o = &g_o[g_cfd[a1].obj];
        u64 at = o->chome ? o->chome : c_free_at(o->size);
        o->chome = at;
        map_add(g_cmap, &g_ncmap, at, o->size, g_cfd[a1].obj, true);
        *(u64 *)a3 = at;
        return 0;
    }
    if (fn == FN_SYSCALL) {
        if (rax == 6) {
            if (a1 >= NFD || !g_cfd[a1].kind)
                return (u64)-1;
            g_cfd[a1].kind = 0, g_closed_c++;
            collect();
            return 0;
        }
        if (rax == 73) {
            for (u32 m = 0; m < g_ncmap; m++)
                if (g_cmap[m].len && g_cmap[m].at >= a1 && g_cmap[m].at + g_cmap[m].len <= a1 + a2)
                    g_cmap[m].len = 0;
            collect();
            return 0;
        }
        if (rax == 232) {
            struct map *m = map_at(g_cmap, g_ncmap, a2);
            if (!m || !m->rw)
                return (u64)-1; /* EFAULT */
            u64 t[2] = {g_stamp, g_stamp % 1000000000};
            g_stamp++;
            memcpy(g_o[m->obj].bytes + (a2 - m->at), t, 16);
            sync_views(m->obj);
            return 0;
        }
        fault("compiler system call", rax, a1);
    }
    fault("compiler call", fn, 0);
    return 0;
}

static int jit_recv_fd_(int cfd)
{
    if (cfd < 0 || cfd >= NFD || !g_cfd[cfd].kind)
        fault("recv of no descriptor", (u64)cfd, 0);
    return add_fd(false, g_cfd[cfd].obj, g_cfd[cfd].kind);
}

/* ---- this process ---- */
static int k_close(int fd)
{
    if (fd < 0 || fd >= NFD || !g_hfd[fd].kind)
        return -1;
    g_hfd[fd].kind = 0, g_closed_h++;
    collect();
    return 0;
}

static int k_fstat(int fd, void *st)
{
    if (fd < 0 || fd >= NFD || !g_hfd[fd].kind)
        return -1;
    memset(st, 0, 0x78);
    *(u16 *)((u8 *)st + 8) = 0x2000; /* every object here is shared memory */
    *(u64 *)((u8 *)st + 0x48) = g_o[g_hfd[fd].obj].size;
    return 0;
}

static bool in_area(u64 a, u64 len) { return a >= g_area && a + len <= g_area + g_area_len; }

static int k_reserve(void **addr, size_t len, int flags, size_t align)
{
    u64 a = (u64)*addr;
    if (flags & MAP_FIXED) {
        if (!in_area(a, len))
            fault("fixed reservation outside this process's area", a, len);
        for (u32 m = 0; m < g_nhmap; m++)
            if (g_hmap[m].len && g_hmap[m].at < a + len && a < g_hmap[m].at + g_hmap[m].len) {
                if (g_hmap[m].at < a || g_hmap[m].at + g_hmap[m].len > a + len)
                    fault("a view replaced in part", g_hmap[m].at, a);
                g_hmap[m].len = 0;
            }
        memset((void *)a, 0xEE, len);
        collect();
        return 0;
    }
    a = ALIGN_UP(g_scratch, align);
    g_scratch = a + len;
    if (!in_area(a, len))
        fault("out of address space", a, len);
    *addr = (void *)a;
    return 0;
}

static int k_munmap(void *addr, size_t len)
{
    for (u32 m = 0; m < g_nhmap; m++)
        if (g_hmap[m].len && g_hmap[m].at >= (u64)addr && g_hmap[m].at + g_hmap[m].len <= (u64)addr + len)
            g_hmap[m].len = 0;
    collect();
    return 0;
}

static int k_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off, void **res)
{
    if (fd < 0 || fd >= NFD || !g_hfd[fd].kind)
        return (int)0x80020009;
    struct obj *o = &g_o[g_hfd[fd].obj];
    if (!(flags & MAP_FIXED) || !in_area((u64)addr, len))
        fault("view not at a fixed address of this process", (u64)addr, len);
    if (off || len != o->size)
        return (int)0x80020016; /* a view is the whole object */
    int k = g_hfd[fd].kind;
    if ((prot & PROT_WRITE) || ((prot & PROT_EXEC) && k != 2 && k != 1))
        return (int)0x8002000d;
    map_add(g_hmap, &g_nhmap, (u64)addr, len, g_hfd[fd].obj, false);
    memcpy(addr, o->bytes, len);
    collect();
    *res = addr;
    return 0;
}

/* ---- the core's side ---- */
static u64 g_buf[4];
static bool g_dropped[4];
static u64 g_adopt[3];
static u32 arena_buffers_(u64 *st, u64 *en, u32 max)
{
    u32 n = 0;
    for (u32 i = 0; i < 4 && n < max; i++)
        if (!g_dropped[i])
            st[n] = g_buf[i], en[n++] = g_buf[i] + g_o[1 + i].size;
    return n;
}
static void arena_drop_(u64 start)
{
    for (u32 i = 0; i < 4; i++)
        if (g_buf[i] == start)
            g_dropped[i] = true, printf("arena drop %s\n", g_o[1 + i].name);
}
static int arena_adopt_(u64 start, u64 end, s64 delta)
{
    g_adopt[0] = start, g_adopt[1] = end, g_adopt[2] = start + (u64)delta;
    return 0;
}
static void *arena_alloc_(size_t n) { return calloc(1, n); }

int jit_reclaim(struct bx_reclaim *r);

int main(int argc, char **argv)
{
    if (argc < 4)
        return 2;
    const char *c = argv[1];
    bool ps4 = !strcmp(c, "ps4");
    G = &api;
    api.ctx = &ctxp, api.k = &kf, api.logf = hlog, api.arena_alloc = arena_alloc_, api.jit_call = jit_call_;
    api.jit_recv_fd = jit_recv_fd_, api.arena_buffers = arena_buffers_, api.arena_drop = arena_drop_;
    api.arena_adopt = arena_adopt_;
    kf.close = k_close, kf.fstat = k_fstat, kf.mmap = k_mmap, kf.munmap = k_munmap, kf.reserve_va = k_reserve;
    ctx.oob_scratch = (u64)scratch, ctx.jit_syscall = FN_SYSCALL, ctx.jit_shm_create = FN_CREATE;
    ctx.jit_shm_alias = FN_ALIAS, ctx.jit_shm_map = FN_MAP;
    if (ps4)
        g_next_cfd = 900, g_next_hfd = 700;
    g_area_len = 512 * MiB;
    g_area = (u64)mmap(NULL, g_area_len, 3, 0x2 | MAP_ANON_, -1, 0);
    if (g_area == (u64)-1)
        return 2;
    g_area = ALIGN_UP(g_area, 0x10000) + 0xc000; /* homes 16 KiB below a 64 KiB boundary, as on the console */
    g_area_len -= 0x20000;
    g_scratch = g_area + 128 * MiB;
    /* the core's IPU object, the four buffers (homes as on 13.60, rebased), the extension */
    static const char *names[] = {"EE", "IOP", "VU0", "VU1"};
    static const u64 sizes[] = {32 * MiB, 8 * MiB, 8 * MiB, 16 * MiB}, offs[] = {0x48000, 0x204c000, 0x2850000,
                                                                                 0x3054000};
    int ipu = new_obj("IPU", 256 * KiB, true);
    g_o[ipu].home = g_area;
    add_fd(true, ipu, 1), add_fd(true, ipu, 2), add_fd(false, ipu, 2);
    for (u32 i = 0; i < 4; i++) {
        int o = new_obj(names[i], sizes[i], true);
        u64 home = g_area + offs[i], chome = home; /* the compiler maps a buffer where it is here */
        g_o[o].home = home, g_o[o].chome = chome, g_buf[i] = home;
        int base = add_fd(true, o, 1);
        add_fd(true, o, 2), add_fd(true, o, 3), add_fd(false, o, 2);
        (void)base;
        map_add(g_hmap, &g_nhmap, home, sizes[i], o, false);
        if (!(!strcmp(c, "nostamp") && i == 2))
            map_add(g_cmap, &g_ncmap, chome, sizes[i], o, true);
        memcpy((void *)home, g_o[o].bytes, sizes[i]);
    }
    int ext = new_obj("ext", 672 * KiB, true);
    add_fd(true, ext, 1), add_fd(false, ext, 2);
    map_add(g_cmap, &g_ncmap, 0x904f34000UL, 672 * KiB, ext, true);
    g_cnext = 0x904f34000UL + 672 * KiB;
    if (!strcmp(c, "extra")) {
        int extra = new_obj("extra", 8 * MiB, true);
        CAP += 8 * MiB;
        add_fd(true, extra, 1), add_fd(true, extra, 3), add_fd(false, extra, 2);
    }
    if (!strcmp(c, "payload")) /* a payload's view of VU1 above the buffers */
        map_add(g_hmap, &g_nhmap, g_area + 96 * MiB, 16 * MiB, 4, false);
    /* a non-JIT descriptor of a buffer's size: never aliased by the compiler */
    int shm = new_obj("shm", 8 * MiB, false);
    add_fd(false, shm, 2);
    printf("used %lu KiB of %lu\n", used() / KiB, CAP / KiB);

    struct bx_reclaim *r = calloc(1, sizeof(*r));
    r->name = "eboot.bin";
    s64 at = 0;
    for (int i = 2; i < argc - 1 && r->nruns < BX_RECLAIM_MAX; i++) { /* SIZE or SIZE:LIMIT, KiB: runs in a row */
        char *e;
        s64 size = (s64)(strtoul(argv[i], &e, 10) * KiB), limit = *e == ':' ? (s64)(strtoul(e + 1, NULL, 10) * KiB) : size;
        r->runs[r->nruns++] = (struct bx_range){at, at + size};
        at += limit;
    }
    r->text = (u64)at;
    r->keep = strtoul(argv[argc - 1], NULL, 10) * KiB - JIT_BUF_HEADER; /* the least for the others: AMIN_KIB */
    int rc = jit_reclaim(r);
    printf("rc %d\n", rc);
    if (rc)
        printf("why: %s\n", r->why);
    printf("free before %lu KiB, after %lu KiB, arena %lu KiB\n", r->free_before / KiB, r->free_after / KiB,
           r->arena / KiB);
    for (u32 i = 0; i < 4; i++)
        printf("buffer %s %s\n", names[i], g_o[1 + i].live ? "alive" : "gone");
    for (u32 i = 0; i < g_no; i++)
        if (!strcmp(g_o[i].name, "IPU") || !strcmp(g_o[i].name, "ext") || !strcmp(g_o[i].name, "extra") ||
            !strcmp(g_o[i].name, "shm")) {
            u32 cf = 0, hf = 0;
            for (u32 f = 0; f < NFD; f++)
                cf += g_cfd[f].kind && g_cfd[f].obj == (int)i, hf += g_hfd[f].kind && g_hfd[f].obj == (int)i;
            printf("kept %s: %s, %u compiler descriptors, %u here\n", g_o[i].name, g_o[i].live ? "alive" : "GONE", cf,
                   hf);
        }
    for (u32 v = 0; !rc && v < r->n; v++) {
        int fd = r->obj[v].rx;
        bool ok = fd >= 0 && fd < NFD && g_hfd[fd].kind == 2 && g_o[g_hfd[fd].obj].size == r->size[v];
        struct map *m = map_at(g_cmap, g_ncmap, r->obj[v].wr);
        ok = ok && m && m->rw && m->at == r->obj[v].wr && m->obj == g_hfd[fd].obj;
        printf("code object %u: %lu KiB, RX alias here %s, compiler RW map %s\n", v, r->size[v] / KiB,
               ok ? "ok" : "WRONG", ok ? "ok" : "WRONG");
    }
    if (!rc) {
        struct map *h = map_at(g_hmap, g_nhmap, g_adopt[0]), *w = map_at(g_cmap, g_ncmap, g_adopt[2]);
        bool ok = h && w && h->obj == w->obj && h->at == g_adopt[0] && h->len == g_adopt[1] - g_adopt[0] &&
                  w->at == g_adopt[2] && g_o[h->obj].size == r->arena && g_adopt[0] == g_o[1].home;
        printf("arena: %lu KiB where EE was, compiler writes it elsewhere: %s\n", (g_adopt[1] - g_adopt[0]) / KiB,
               ok ? "ok" : "WRONG");
    }
    printf("used now %lu KiB; free %lu KiB; creates %u; closed %u compiler, %u here\n", used() / KiB,
           (CAP - used()) / KiB, g_creates, g_closed_c, g_closed_h);
    return 0;
}
