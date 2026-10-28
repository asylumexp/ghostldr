/*
 * Runs the extension's homebrew families (hle_libc_heap.c, hle_libc_stdio.c,
 * hle_tsd.c, hle_homebrew.c), built for this machine, against a fake core: ghostfs
 * with one title file, flexible memory that counts what is mapped, SWRR's
 * functions as recorders.  test_homebrew.py builds and drives it:
 *
 *     homebrew_host heap|heap-off|tsd|stdio|homebrew
 *
 * Prints "ok ..." / "FAILED ..." lines and the families' log; exits 0 when
 * every check passed.
 */
#include "ghost_api.h"

long write(int fd, const void *buf, unsigned long n);
int vsnprintf(char *buf, unsigned long size, const char *fmt, __builtin_va_list ap);
int snprintf(char *buf, unsigned long size, const char *fmt, ...);
void *calloc(unsigned long n, unsigned long size);
void free(void *p);
char *strcat(char *d, const char *s);
int posix_memalign(void **out, unsigned long align, unsigned long size);
long random(void);
void srandom(unsigned int seed);
int pthread_create(void *thr, const void *attr, void *(*fn)(void *), void *arg);
int pthread_join(u64 thr, void **ret);

void compute_nid(const char *name, char out[12]);

struct kfuncs K;
static struct boot_ctx g_ctx_v;
static struct boot_ctx *g_ctx_p = &g_ctx_v;
static struct module *g_modules_p, *g_main_p;
static u64 g_dmem;
const struct ghost_api *G;
static int g_failed;

/* hle_jbc.c is not part of this build: no virtual devices */
bool jbc_ioctl(s32 fd, u64 req, u64 arg, s64 *rc) { return false; }

/* ---- the harness side ---- */
static const struct ext_family *g_fam[8];
static u32 g_nfam;
void harness_family(const struct ext_family *f) { g_fam[g_nfam++] = f; }

static void out(const char *f, __builtin_va_list ap)
{
    char line[1200];
    int n = vsnprintf(line, sizeof(line) - 1, f, ap);
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    write(1, line, n);
}

static void hlog(const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    out(f, ap);
    __builtin_va_end(ap);
}

static void expect(bool ok, const char *what)
{
    hlog("%s %s", ok ? "ok" : "FAILED", what);
    if (!ok)
        g_failed = 1;
}

static const struct ext_family *family(const char *name)
{
    for (u32 i = 0; i < g_nfam; i++)
        if (strcmp(g_fam[i]->name, name) == 0)
            return g_fam[i];
    return NULL;
}

/* a family's function for name */
static void *fn(const char *fam, const char *lib, const char *name)
{
    const struct ext_family *f = family(fam);
    for (u32 i = 0; f && i < f->nhle; i++)
        if (strcmp(f->hle[i].name, name) == 0 && strcmp(f->hle[i].lib, lib) == 0)
            return f->hle[i].fn;
    hlog("FAILED no %s:%s in family %s", lib, name, fam);
    g_failed = 1;
    return NULL;
}

/* ---- errno, threads ---- */
static int g_errno;
static int *fake_errno(void) { return &g_errno; }
static int fake_usleep(u32 us) { return 0; }
static int g_thread_rc;
static u64 g_tid = 0x1000; /* what SWRR's pthread_self says: the harness switches "threads" */
static int fake_thread(void *thr, const void *attr, void *(*f)(void *), void *arg, const char *name)
{
    hlog("thread create %s", name);
    if (g_thread_rc == 0 && f) { /* run it at once, as another thread */
        u64 was = g_tid;
        g_tid = 0x2000;
        f(arg);
        g_tid = was;
    }
    return g_thread_rc;
}
static u64 host_pthread_self(void) { return g_tid; }
static void *arena_alloc_(size_t n) { return calloc(1, n ? n : 1); }

/* ---- memory: the title's flexible memory counts what is mapped ---- */
static u64 g_flex_used, g_flex_cap = 448UL << 20;
static u32 g_flex_maps, g_flex_unmaps;
static int flex_map(void **addr, size_t len, int prot, int flags)
{
    if (g_flex_used + len > g_flex_cap)
        return (int)0x8002000c;
    void *p = NULL;
    if (posix_memalign(&p, 0x4000, len) != 0)
        return (int)0x8002000c;
    memset(p, 0xA5, len); /* not zeroed, like direct memory */
    g_flex_used += len, g_flex_maps++;
    *addr = p;
    return 0;
}
static int flex_unmap(void *addr, size_t len)
{
    g_flex_used -= len, g_flex_unmaps++;
    free(addr);
    return 0;
}
static u32 g_direct_frees;
static void *direct_alloc_(size_t size, int prot, u64 fixed)
{
    void *p = NULL;
    return posix_memalign(&p, 0x4000, size) == 0 ? p : NULL;
}
static void direct_free_(void *p)
{
    g_direct_frees++;
    free(p);
}
static void *page_alloc_(size_t size) { return calloc(1, size); }
static void page_free_(void *p, size_t size) { free(p); }
static void spin_lock_(volatile int *l)
{
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        ;
}
static void spin_unlock_(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

/* ---- ghostfs: /app0/data.txt and /app0/dir ---- */
static const char k_data[] = "first line\nsecond line\nthird line\n";
#define GFD 100
#define GDIR 101
static s64 g_pos;
static int g_dents_done;
#define GROOT 102
static int g_root_done;
static int gfs_open_(const char *path, int flags, int *fd)
{
    if (strcmp(path, "/app0") == 0) {
        *fd = GROOT, g_root_done = 0;
        return 0;
    }
    if (strncmp(path, "/app0/", 6) != 0)
        return 1;
    if (flags & 3)
        *fd = (int)0x8002001e; /* EROFS */
    else if (strcmp(path, "/app0/data.txt") == 0 || strcmp(path, "/app0/Font.TTF") == 0)
        *fd = GFD, g_pos = 0;
    else if (strcmp(path, "/app0/dir") == 0)
        *fd = GDIR, g_dents_done = 0;
    else
        *fd = (int)0x80020002; /* ENOENT */
    return 0;
}
static bool gfs_owns_(int fd) { return fd == GFD || fd == GDIR || fd == GROOT; }
static int gfs_close_(int fd) { return gfs_owns_(fd) ? 0 : 1; }
static s64 gfs_pread_(int fd, void *buf, u64 n, s64 off)
{
    if (fd == GDIR)
        return (s64)(int)0x80020015; /* EISDIR */
    u64 size = sizeof(k_data) - 1;
    if ((u64)off >= size)
        return 0;
    if (n > size - (u64)off)
        n = size - (u64)off;
    memcpy(buf, k_data + off, n);
    return (s64)n;
}
static s64 gfs_read_(int fd, void *buf, u64 n)
{
    s64 r = gfs_pread_(fd, buf, n, g_pos);
    if (r > 0)
        g_pos += r;
    return r;
}
static s64 gfs_lseek_(int fd, s64 off, int whence)
{
    s64 base = whence == 0 ? 0 : whence == 1 ? g_pos : (s64)sizeof(k_data) - 1;
    return base + off < 0 ? (s64)(int)0x80020016 : (g_pos = base + off);
}
static int gfs_fstat_(int fd, void *st)
{
    memset(st, 0, 0x78);
    *(s64 *)((u8 *)st + 0x48) = sizeof(k_data) - 1;
    return 0;
}
static int gfs_stat_(const char *path, void *st, int *rc)
{
    int fd;
    if (gfs_open_(path, 0, &fd))
        return 1;
    *rc = fd < 0 ? fd : gfs_fstat_(fd, st);
    return 0;
}
static int gfs_getdents_(int fd, void *buf, int size)
{
    if (fd == GROOT) { /* /app0: dir/ and Font.TTF */
        if (g_root_done++)
            return 0;
        u8 *d = buf;
        memset(d, 0, 48);
        *(u32 *)d = 2, *(u16 *)(d + 4) = 16, d[6] = 4, d[7] = 3;
        memcpy(d + 8, "dir", 4);
        *(u32 *)(d + 16) = 3, *(u16 *)(d + 20) = 24, d[22] = 8, d[23] = 8;
        memcpy(d + 24, "Font.TTF", 9);
        return 40;
    }
    if (fd != GDIR)
        return (int)0x80020016;
    if (g_dents_done++)
        return 0;
    u8 *d = buf; /* FreeBSD dirent: ino, reclen, type, namlen, name */
    memset(d, 0, 24);
    *(u32 *)d = 1, *(u16 *)(d + 4) = 24, d[6] = 8, d[7] = 8;
    memcpy(d + 8, "data.txt", 9);
    return 24;
}
static const char *remap_path_(const char *path, char *buf, size_t size) { return path; }
static int vfs_open_(const char *path, int flags, int mode)
{
    int fd;
    return gfs_open_(path, flags & 3, &fd) == 0 ? fd : (int)0x80020002;
}

/* hle_file.c's (not built here): the title's own sandbox path, for TEST00000 */
const char *title_sandbox_path(const char *path, char *buf, size_t size)
{
    static const char pre[] = "/mnt/sandbox/TEST00000_000/";
    if (!path || strncmp(path, pre, sizeof(pre) - 1) != 0)
        return path;
    snprintf(buf, size, "/%s", path + sizeof(pre) - 1);
    return buf;
}
int fmt(char *buf, size_t size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vsnprintf(buf, size, f, ap);
    __builtin_va_end(ap);
    return n;
}

/* ---- SWRR's functions: recorders ---- */
static char g_host_called[64];
static s64 g_host_ret;
static u64 g_host_args[4];
#define HOSTFN(name)                                                         \
    static s64 host_##name(u64 a, u64 b, u64 c, u64 d)                      \
    {                                                                        \
        strcpy(g_host_called, #name);                                        \
        g_host_args[0] = a, g_host_args[1] = b, g_host_args[2] = c, g_host_args[3] = d; \
        return g_host_ret;                                                   \
    }
HOSTFN(open)

HOSTFN(close)
HOSTFN(read)
HOSTFN(_readv)
HOSTFN(lseek)
HOSTFN(fstat)
HOSTFN(stat)
HOSTFN(_fcntl)
HOSTFN(writev)
HOSTFN(_writev)
HOSTFN(_ioctl)
HOSTFN(_exit)
HOSTFN(free)
HOSTFN(realloc)
HOSTFN(malloc_usable_size)
HOSTFN(FT_New_Face)
HOSTFN(FT_Done_Face)
static u64 g_mem_face_size;
static s64 host_FT_New_Memory_Face(u64 lib, u64 data, u64 size, u64 index, void **face)
{
    g_mem_face_size = size;
    *face = (void *)0xFACE;
    return memcmp((const void *)data, k_data, sizeof(k_data) - 1) == 0 ? 0 : 99;
}

HOSTFN(fwrite)
static int g_fake_stdout_file, g_fake_stderr_file, g_other_file;
static void *g_stdout_var = &g_fake_stdout_file; /* SWRR's stdout: a FILE * variable */
static int host_vsnprintf(char *buf, u64 size, const char *fmt, __builtin_va_list ap)
{
    return vsnprintf(buf, size, fmt, ap);
}

static const struct {
    const char *name;
    void *fn;
} k_host[] = {
    {"vsnprintf", host_vsnprintf}, {"fwrite", host_fwrite}, {"stdout", &g_stdout_var},
    {"pthread_self", host_pthread_self},
    {"_Stderr", &g_fake_stderr_file},
    {"open", host_open},     {"_open", host_open},       {"close", host_close},
    {"read", host_read},     {"_readv", host__readv},    {"lseek", host_lseek},
    {"fstat", host_fstat},   {"stat", host_stat},        {"_fcntl", host__fcntl},
    {"writev", host_writev}, {"_writev", host__writev},  {"_ioctl", host__ioctl},
    {"_exit", host__exit},   {"free", host_free},        {"realloc", host_realloc},
    {"malloc_usable_size", host_malloc_usable_size},    {"FT_New_Face", host_FT_New_Face},
    {"FT_Done_Face", host_FT_Done_Face},                 {"FT_New_Memory_Face", host_FT_New_Memory_Face},
};
static void *host_real_(const char *name)
{
    for (u32 i = 0; i < ARRAY_SIZE(k_host); i++)
        if (strcmp(k_host[i].name, name) == 0)
            return k_host[i].fn;
    return NULL;
}
static void *host_real_in_(const char *mod, const char *name) { return host_real_(name); }

/* the core's write: what reaches the log */
static char g_logged[256];
static u32 g_nlogged;
static s64 core_write(int fd, const void *buf, u64 n)
{
    for (u64 i = 0; i < n && g_nlogged + 1 < sizeof(g_logged); i++)
        g_logged[g_nlogged++] = ((const char *)buf)[i];
    return (s64)n;
}
static u64 hle_lookup_(const char *lib, const char *name) { return 0; }

static void (*g_thread_dtors_v)(void);

/* the extension's own lookup (ext_main.c): the file family's _write */
u64 ext_find(const char *lib, const char *nid)
{
    char w[12];
    compute_nid("_write", w);
    return strcmp(nid, w) == 0 ? (u64)core_write : 0;
}

static struct ghost_api g_api;

static void setup(void)
{
    K.errno_fn = fake_errno, K.usleep = fake_usleep, K.pthread_create = fake_thread;
    g_api = (struct ghost_api){
        .version = GHOST_API_VERSION, .size = sizeof(struct ghost_api), .ctx = &g_ctx_p, .k = &K,
        .modules = &g_modules_p, .main_mod = &g_main_p, .reported_dmem = &g_dmem, .logf = hlog,
        .host_real = host_real_, .host_real_in = host_real_in_, .compute_nid = compute_nid,
        .page_alloc = page_alloc_, .page_free = page_free_, .direct_alloc = direct_alloc_,
        .direct_free = direct_free_, .spin_lock = spin_lock_, .spin_unlock = spin_unlock_,
        .remap_path = remap_path_, .gfs_owns = gfs_owns_, .gfs_open = gfs_open_, .gfs_close = gfs_close_,
        .gfs_stat = gfs_stat_, .gfs_read = gfs_read_, .gfs_pread = gfs_pread_, .gfs_lseek = gfs_lseek_,
        .gfs_fstat = gfs_fstat_, .gfs_getdents = gfs_getdents_, .ghost_thread_create = fake_thread,
        .hle_lookup = hle_lookup_, .flex_map = flex_map, .flex_unmap = flex_unmap, .arena_alloc = arena_alloc_,
        .vfs_open = vfs_open_, .vfs_getdents = gfs_getdents_, .vfs_close = gfs_close_,
        .thread_dtors = &g_thread_dtors_v,
    };
    G = &g_api;
}

/* a title module: its symbol table defines malloc (the title's own libc) or not */
static struct module g_mod;
static struct elf_sym g_syms[2];
static char g_strtab[64];
static void title_module(bool exports_malloc)
{
    char nid[12];
    compute_nid(exports_malloc ? "malloc" : "printf", nid);
    strcpy(g_strtab + 1, nid);
    strcat(g_strtab + 1, "#A#A");
    g_syms[1] = (struct elf_sym){.name = 1, .info = 0x12, .shndx = exports_malloc ? 9 : 0};
    g_mod = (struct module){.name = "libc.prx", .symtab = g_syms, .nsyms = 2, .strtab = g_strtab, .strsz = 64};
    g_modules_p = &g_mod;
}

/* ---- scenarios ---- */
typedef void *(*malloc_t)(u64);
typedef void (*free_t)(void *);

static bool pattern_ok(const u8 *p, u64 n, u8 v)
{
    for (u64 i = 0; i < n; i++)
        if (p[i] != v)
            return false;
    return true;
}

struct worker_arg {
    malloc_t m;
    free_t f;
    int seed, ok;
};

static void *heap_worker(void *a)
{
    struct worker_arg *w = a;
    void *keep[64] = {0};
    u64 sizes[64] = {0};
    w->ok = 1;
    for (int i = 0; i < 4000; i++) {
        int k = (int)((u32)(i * 2654435761u + w->seed) % 64);
        if (keep[k]) {
            w->ok &= pattern_ok(keep[k], sizes[k], (u8)k);
            w->f(keep[k]);
            keep[k] = NULL;
        } else {
            sizes[k] = (u64)((i * 7919 + w->seed) % 3000);
            keep[k] = w->m(sizes[k]);
            if (keep[k])
                memset(keep[k], k, sizes[k]);
            w->ok &= keep[k] != NULL;
        }
    }
    for (int k = 0; k < 64; k++)
        w->f(keep[k]);
    return NULL;
}

static void scenario_heap(void)
{
    const struct ext_family *fam = family("libc_heap");
    title_module(false);
    expect(fam && fam->init() == 0, "on when no title module exports malloc");
    const char *L = "libSceLibcInternal";
    malloc_t m = fn("libc_heap", L, "malloc");
    free_t f = fn("libc_heap", L, "free");
    void *(*c)(u64, u64) = fn("libc_heap", L, "calloc");
    void *(*r)(void *, u64) = fn("libc_heap", L, "realloc");
    void *(*ma)(u64, u64) = fn("libc_heap", L, "memalign");
    int (*pm)(void **, u64, u64) = fn("libc_heap", L, "posix_memalign");
    u64 (*us)(void *) = fn("libc_heap", L, "malloc_usable_size");
    int (*st)(void *) = fn("libc_heap", L, "malloc_stats");
    void *(*nw)(u64) = fn("libc_heap", L, "_Znwm");
    void (*del)(void *) = fn("libc_heap", L, "_ZdlPv");
    if (g_failed)
        return;

    /* sizes across every class and past it */
    static const u64 sizes[] = {0, 1, 15, 16, 17, 100, 128, 129, 1000, 4096, 50000, 131072, 131073, 1 << 20, 9 << 20};
    void *p[ARRAY_SIZE(sizes)];
    bool ok = true;
    for (u32 i = 0; i < ARRAY_SIZE(sizes); i++) {
        p[i] = m(sizes[i]);
        ok &= p[i] && !((u64)p[i] & 15) && us(p[i]) >= sizes[i];
        if (p[i])
            memset(p[i], (int)i, sizes[i]);
    }
    for (u32 i = 0; i < ARRAY_SIZE(sizes); i++)
        ok &= pattern_ok(p[i], sizes[i], (u8)i);
    expect(ok, "malloc of 0 B .. 9 MiB: 16-byte aligned, usable, distinct");
    u32 maps = g_flex_maps, unmaps = g_flex_unmaps;
    for (u32 i = 0; i < ARRAY_SIZE(sizes); i++)
        f(p[i]);
    expect(g_flex_unmaps == unmaps + 3, "freeing the three blocks past 128 KiB unmaps them (the budget gets them back)");
    void *again = m(100);
    expect(again == p[5], "a freed small block is reused");
    f(again);
    expect(g_flex_maps == maps, "nothing new was mapped for it");

    u8 *z = c(1000, 1000);
    expect(z && pattern_ok(z, 1000000, 0), "calloc is zero over memory that was not");
    f(z);
    expect(c((u64)1 << 40, (u64)1 << 40) == NULL && g_errno == 12, "calloc overflow fails with ENOMEM");

    char *s = m(10);
    strcpy(s, "keep me");
    s = r(s, 300000);
    expect(s && strcmp(s, "keep me") == 0 && us(s) >= 300000, "realloc to a large block keeps the data");
    char *s2 = r(s, 5);
    expect(s2 == s, "realloc smaller keeps the block");
    f(s2);
    expect(r(NULL, 64) != NULL, "realloc(NULL) allocates");

    ok = true;
    for (u64 a = 32; a <= 65536; a *= 2) {
        u8 *q = ma(a, 777);
        ok &= q && !((u64)q & (a - 1)) && us(q) >= 777;
        if (q)
            memset(q, 1, 777);
        f(q);
    }
    expect(ok, "memalign 32 .. 64 KiB");
    void *q = NULL;
    expect(pm(&q, 24, 10) == 22 && pm(&q, 4096, 10) == 0 && !((u64)q & 4095), "posix_memalign checks its alignment");
    f(q);
    void *n = nw(40);
    expect(n != NULL, "operator new");
    del(n);

    /* pointers the heap did not hand out are SWRR's */
    char foreign[32];
    g_host_called[0] = 0;
    f(foreign + 16);
    expect(strcmp(g_host_called, "free") == 0 && g_host_args[0] == (u64)(foreign + 16), "a foreign pointer goes to SWRR's free");
    g_host_ret = 77;
    expect(us(foreign + 16) == 77, "and to SWRR's malloc_usable_size");
    void *twice = m(48);
    f(twice);
    g_host_called[0] = 0;
    f(twice);
    expect(g_host_called[0] == 0, "a double free is ignored, never SWRR's");

    u64 stats[5] = {0};
    void *big = m(2 << 20);
    expect(st(stats) == 0 && stats[2] >= (2 << 20) && stats[4] >= (2 << 20), "malloc_stats reports the mapped and used sizes");
    f(big);

    /* past the flexible budget: NULL and ENOMEM, no crash */
    g_flex_cap = g_flex_used + (1 << 20);
    g_errno = 0;
    expect(m(4 << 20) == NULL && g_errno == 12, "past the flexible budget: NULL, ENOMEM");
    g_flex_cap = 448UL << 20;

    /* four threads at once */
    struct worker_arg w[4];
    u64 th[4];
    for (int i = 0; i < 4; i++) {
        w[i] = (struct worker_arg){m, f, i * 31, 0};
        pthread_create(&th[i], NULL, heap_worker, &w[i]);
    }
    ok = true;
    for (int i = 0; i < 4; i++)
        pthread_join(th[i], NULL), ok &= w[i].ok;
    expect(ok, "four threads allocating and freeing at once");
    hlog("flexible memory mapped %lu KiB in %u maps, %u unmaps", g_flex_used >> 10, g_flex_maps, g_flex_unmaps);
}

static void scenario_heap_off(void)
{
    title_module(true);
    const struct ext_family *fam = family("libc_heap");
    expect(fam && fam->init() != 0, "off when a title module exports malloc (its own libc)");
}

struct iovec {
    void *base;
    u64 len;
};

static u32 g_dtor_calls;
static void *g_dtor_arg;
static void tsd_dtor(void *v) { g_dtor_calls++, g_dtor_arg = v; }
static u32 g_title_dtor_calls;
static void title_dtors(void) { g_title_dtor_calls++; }

/* thread-specific data (hle_tsd.c) */
static void scenario_tsd(void)
{
    const char *LK = "libkernel";
    s64 (*kc)(u32 *, void (*)(void *)) = fn("tsd", LK, "pthread_key_create");
    s64 (*ss)(u32, const void *) = fn("tsd", LK, "pthread_setspecific");
    void *(*gs)(u32) = fn("tsd", LK, "pthread_getspecific");
    s64 (*kd)(u32) = fn("tsd", LK, "pthread_key_delete");
    s64 (*sd)(void (*)(void)) = fn("tsd", LK, "_sceKernelSetThreadDtors");
    const struct ext_family *fam = family("tsd");
    if (g_failed || !fam)
        return;
    expect(fam->init() == 0 && g_thread_dtors_v != NULL, "the thread-exit hook is the family's from the start");
    u32 k1 = 99, k2 = 99;
    expect(kc(&k1, NULL) == 0 && kc(&k2, tsd_dtor) == 0 && k1 != k2, "pthread_key_create");
    expect(gs(k1) == NULL && ss(k1, &g_tid) == 0 && gs(k1) == &g_tid, "setspecific/getspecific");
    g_tid = 0x3000;
    expect(gs(k1) == NULL, "another thread sees its own (NULL) value");
    expect(ss(k2, &k2) == 0, "that thread stores a value under a key with a destructor");
    expect(sd(title_dtors) == 0, "the title libc sets its thread-exit hook");
    g_thread_dtors_v(); /* the thread ends (the core's trampoline, or pthread_exit) */
    expect(g_title_dtor_calls == 1 && g_dtor_calls == 1 && g_dtor_arg == &k2,
           "a thread that ends runs the title's hook, then its destructors");
    expect(gs(k2) == NULL, "and its values are gone");
    g_tid = 0x1000;
    expect(gs(k1) == &g_tid && ss(200, &g_tid) == 22 && gs(200) == NULL, "unused keys: EINVAL, NULL");
    expect(kd(k2) == 0 && kd(k2) == 22, "pthread_key_delete");
}

static void scenario_stdio(void)
{
    const char *L = "libSceLibcInternal";
    title_module(false);
    const struct ext_family *fam = family("libc_stdio");
    expect(fam && fam->init() == 0, "on when no title module exports malloc");
    int (*pf)(const char *, ...) = fn("libc_stdio", L, "printf");
    int (*fpf)(void *, const char *, ...) = fn("libc_stdio", L, "fprintf");
    int (*ps)(const char *) = fn("libc_stdio", L, "puts");
    u64 (*fw)(const void *, u64, u64, void *) = fn("libc_stdio", L, "fwrite");
    if (g_failed)
        return;
    expect(pf("%s %d %5.2f|", "printf", 42, 3.14159) == 16, "printf returns the length");
    ps("puts");
    expect(strcmp(g_logged, "printf 42  3.14|puts\n") == 0, "printf and puts reach the log");
    g_nlogged = 0;
    memset(g_logged, 0, sizeof(g_logged));
    fpf(&g_fake_stdout_file, "out %c", 'x');
    fpf(&g_fake_stderr_file, " err\n");
    expect(strcmp(g_logged, "out x err\n") == 0, "fprintf to SWRR's stdout (a variable) and stderr (_Stderr)");
    g_host_called[0] = 0;
    expect(fw("abc", 1, 3, &g_other_file) == 0 && strcmp(g_host_called, "fwrite") == 0, "other FILEs are SWRR's");

    /* title files: FILEs and DIRs of the family's own */
    void *(*fo)(const char *, const char *) = fn("libc_stdio", L, "fopen");
    char *(*fg)(char *, int, void *) = fn("libc_stdio", L, "fgets");
    int (*fgc)(void *) = fn("libc_stdio", L, "fgetc");
    int (*ug)(int, void *) = fn("libc_stdio", L, "ungetc");
    u64 (*fr)(void *, u64, u64, void *) = fn("libc_stdio", L, "fread");
    int (*fs)(void *, s64, int) = fn("libc_stdio", L, "fseek");
    s64 (*ft)(void *) = fn("libc_stdio", L, "ftell");
    int (*fe)(void *) = fn("libc_stdio", L, "feof");
    int (*fc)(void *) = fn("libc_stdio", L, "fclose");
    void *(*od)(const char *) = fn("libc_stdio", L, "opendir");
    u8 *(*rd)(void *) = fn("libc_stdio", L, "readdir");
    int (*cd)(void *) = fn("libc_stdio", L, "closedir");
    if (g_failed)
        return;
    void *f = fo("/app0/data.txt", "r");
    char line[64];
    expect(f && fg(line, sizeof(line), f) && strcmp(line, "first line\n") == 0, "fopen/fgets of a title file");
    expect(fgc(f) == 's' && ug('s', f) == 's' && fgc(f) == 's' && ft(f) == 12, "fgetc, ungetc, ftell");
    expect(fs(f, -11, 2) == 0 && fr(line, 1, 20, f) == 11 && !memcmp(line, "third line\n", 11) && fe(f),
           "fseek from the end, fread to the end, feof");
    expect(fw("x", 1, 1, f) == 0, "a title FILE is read-only");
    expect(fc(f) == 0, "fclose");
    g_errno = 0;
    expect(fo("/app0/data.txt", "w") == NULL && g_errno == 30, "fopen for writing: EROFS");
    expect(fo("/app0/none", "r") == NULL && g_errno == 2, "a missing title file: ENOENT");
    void *d = od("/app0/dir");
    u8 *e = d ? rd(d) : NULL;
    expect(e && strcmp((char *)e + 8, "data.txt") == 0 && rd(d) == NULL && cd(d) == 0, "opendir/readdir/closedir");
    title_module(true);
    expect(fam->init() != 0, "off with the title's own libc");
}

static void scenario_homebrew(void)
{
    const char *LK = "libkernel";
    s64 (*wv)(int, const struct iovec *, int) = fn("homebrew", LK, "_writev");
    s64 (*io)(int, u64, u64) = fn("homebrew", LK, "_ioctl");
    s64 (*nf)(void *, const char *, s64, void **) = fn("homebrew", "libSceFreeType", "FT_New_Face");
    s64 (*df)(void *) = fn("homebrew", "libSceFreeType", "FT_Done_Face");
    if (g_failed)
        return;
    char h[] = "hello ", w[] = "world\n";
    struct iovec iov[2] = {{h, 6}, {w, 6}};
    expect(wv(1, iov, 2) == 12 && strcmp(g_logged, "hello world\n") == 0, "writev to stdout reaches the log");
    g_host_ret = 3;
    expect(wv(4, iov, 2) == 3 && strcmp(g_host_called, "_writev") == 0, "writev to other fds is SWRR's");
    u16 ws[4] = {0};
    expect(io(1, 0x40087468, (u64)ws) == 0 && ws[0] == 25 && ws[1] == 80, "TIOCGWINSZ on stdout: a terminal");
    expect(io(7, 0x40087468, (u64)ws) == 3 && strcmp(g_host_called, "_ioctl") == 0, "other ioctls are SWRR's");
    void *face = NULL;
    expect(nf((void *)1, "/app0/data.txt", 0, &face) == 0 && face == (void *)0xFACE &&
           g_mem_face_size == sizeof(k_data) - 1, "FT_New_Face on a title file: FT_New_Memory_Face with its bytes");
    g_direct_frees = 0;
    df(face);
    expect(g_direct_frees == 1 && strcmp(g_host_called, "FT_Done_Face") == 0, "FT_Done_Face frees them");
    g_host_ret = 0;
    expect(nf((void *)1, "/system/font.ttf", 0, &face) == 0 && strcmp(g_host_called, "FT_New_Face") == 0,
           "other fonts: SWRR's FT_New_Face");
    expect(nf((void *)1, "/app0/missing.ttf", 0, &face) == 1, "a missing title font: cannot open");
    g_mem_face_size = 0;
    expect(nf((void *)1, "/mnt/sandbox/TEST00000_000/app0/data.txt", 0, &face) == 0 &&
               g_mem_face_size == sizeof(k_data) - 1,
           "FT_New_Face on the title's own sandbox path: the title file, from ghostfs");
    g_host_ret = 1; /* SWRR's sandbox has no such font */
    g_mem_face_size = 0;
    expect(nf((void *)1, "/preinst/common/font/DFHEI5-SONY.ttf", 0, &face) == 0 &&
               g_mem_face_size == sizeof(k_data) - 1,
           "a system font SWRR lacks: the title's own font (/app0/Font.TTF) instead");
    g_host_ret = 0;
}

int main(int argc, char **argv)
{
    setup();
    const char *sc = argc > 1 ? argv[1] : "";
    if (strcmp(sc, "heap") == 0)
        scenario_heap();
    else if (strcmp(sc, "heap-off") == 0)
        scenario_heap_off();
    else if (strcmp(sc, "tsd") == 0)
        scenario_tsd();
    else if (strcmp(sc, "homebrew") == 0)
        scenario_homebrew();
    else if (strcmp(sc, "stdio") == 0)
        scenario_stdio();
    else
        return 2;
    return g_failed;
}
