/*
 * Runs the core's bigexec.c (with bxplan.c and elf.c) in a host process,
 * built for this machine, against a fake console: the emulator's JIT objects
 * of a 13.60 PS5 (homes, sizes, two compiler descriptors each, only the base
 * one taking an RW alias), the compiler RPC, descriptor passing, reserved
 * ranges, views and direct memory.  test_bigexec.py drives it:
 *
 *     bigexec_host FILE...      modules in load order (the executable is eboot.bin)
 *
 * It does what boot.c does with them (bx_prepare, then bx_place and
 * bx_write_text for the module that gets views) and checks every byte: the
 * module's memory, each object's contents, the data segments.  What the core
 * asks of the console prints as "op ..." lines; a request the console would
 * refuse or that would fault there prints FAULT and exits 3.
 */
#include "bxplan.h"

int printf(const char *f, ...);
int vsnprintf(char *buf, unsigned long size, const char *f, __builtin_va_list ap);
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void exit(int code);
void *fopen(const char *path, const char *mode);
int fseek(void *f, long off, int whence);
long ftell(void *f);
size_t fread(void *p, size_t size, size_t n, void *f);
int fclose(void *f);
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);

#define MAP_PRIVATE_ 0x2
#define MAP_ANON_ 0x1000 /* macOS and FreeBSD */
#define MiB (1UL << 20)

struct kfuncs K;
struct boot_ctx *g_ctx;
struct module *g_modules, *g_main_mod;
static struct boot_ctx ctx;

void logf(const char *f, ...)
{
    char line[1000];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    vsnprintf(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    printf("log: %s\n", line);
}

static void fault(const char *what, u64 a, u64 b)
{
    printf("FAULT %s %lx %lx\n", what, a, b);
    exit(3);
}

void profile_bind(const u8 *fp) { (void)fp; }
const void *profile_record(u32 kind, u32 n) { return NULL; }
void *arena_alloc(size_t size) { return calloc(1, size); }
const char *strrchr_c(const char *s, int c)
{
    const char *r = NULL;
    for (; *s; s++)
        if (*s == c)
            r = s;
    return r;
}

/* ---- the emulator's JIT objects ---- */
struct obj {
    const char *name;
    u64 home, size;
    int fd[2]; /* compiler descriptors; fd[0] is the base one */
    u8 *bytes;
    u64 view;   /* where it is mapped here, or 0 */
    bool whole; /* an RW alias is mapped in the compiler: its first 16 KiB are writable too */
    bool dropped;
};
static struct obj g_obj[] = {
    {"IPU", 0x9009d4000, 0x40000, {47, 715}, NULL, 0, false, false},
    {"EE", 0x900a1c000, 32 * MiB, {346, 617}, NULL, 0, false, false},
    {"IOP", 0x902a20000, 8 * MiB, {476, 121}, NULL, 0, false, false},
    {"VU0", 0x903224000, 8 * MiB, {92, 64}, NULL, 0, false, false},
    {"VU1", 0x903a28000, 16 * MiB, {976, 200}, NULL, 0, false, false},
};
#define NOBJ (sizeof(g_obj) / sizeof(g_obj[0]))
static u32 g_alias_calls, g_rw_aliases, g_closed, g_open_aliases;
static struct {
    int fd;
    u32 obj;
    bool rw;
} g_alias[64];
static u32 g_nalias;

static struct obj *obj_of_fd(int fd)
{
    for (u32 i = 0; i < NOBJ; i++)
        if (g_obj[i].fd[0] == fd || g_obj[i].fd[1] == fd)
            return &g_obj[i];
    return NULL;
}

/* ---- arena.c's side ---- */
int arena_code_region(u64 *start, u64 *end)
{
    struct obj *best = NULL;
    for (u32 i = 1; i < NOBJ; i++)
        if (!g_obj[i].dropped && (!best || g_obj[i].size > best->size))
            best = &g_obj[i];
    *start = best->home + JIT_BUF_HEADER, *end = best->home + best->size;
    return 0;
}

u32 arena_buffers(u64 *start, u64 *end, u32 max)
{
    u32 n = 0;
    for (u32 i = 1; i < NOBJ && n < max; i++)
        if (!g_obj[i].dropped)
            start[n] = g_obj[i].home, end[n++] = g_obj[i].home + g_obj[i].size;
    return n;
}

void arena_drop(u64 start)
{
    for (u32 i = 0; i < NOBJ; i++)
        if (g_obj[i].home == start)
            g_obj[i].dropped = true, printf("op drop %s\n", g_obj[i].name);
}

/* ---- the compiler process ---- */
#define FN_ALIAS 0x1001
#define FN_MAP 0x1002
#define FN_SYSCALL 0x1003
#define FN_MEMCPY 0x1004

static u64 scratch[0x80000 / 8];
static u8 staging[0x400000];

static void compiler_write(u64 dst, const u8 *src, u64 n)
{
    for (u32 i = 0; i < NOBJ; i++) {
        struct obj *o = &g_obj[i];
        if (dst < o->home || dst >= o->home + o->size)
            continue;
        if (dst + n > o->home + o->size)
            fault("compiler write past the object", dst, n);
        if (dst < o->home + JIT_BUF_HEADER && !o->whole)
            fault("compiler write in a buffer's first 16 KiB", dst, n);
        memcpy(o->bytes + (dst - o->home), src, n);
        if (o->view)
            memcpy((void *)(o->view + (dst - o->home)), src, n);
        return;
    }
    fault("compiler write outside the JIT objects", dst, n);
}

u64 jit_call(u64 fn, u64 rax, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6)
{
    if (fn == FN_ALIAS) {
        g_alias_calls++;
        struct obj *o = obj_of_fd((int)a1);
        if (!o)
            return 0x80020009;
        if (a2 == 3 && o->fd[0] != (int)a1)
            return 0x8002000d;
        if (a2 != 3 && a2 != 5)
            return 0x80020016;
        int fd = 2000 + (int)g_nalias;
        g_alias[g_nalias++] = (typeof(g_alias[0])){fd, (u32)(o - g_obj), a2 == 3};
        g_open_aliases++;
        if (a2 == 3) {
            g_rw_aliases++;
            printf("op rw alias of %s\n", o->name);
            if (o->size < MiB)
                fault("RW alias of the object holding the core", o->home, o->size);
        }
        *(u64 *)a3 = (u64)fd;
        return 0;
    }
    if (fn == FN_MAP) {
        for (u32 i = 0; i < g_nalias; i++)
            if (g_alias[i].fd == (int)a1 && g_alias[i].rw) {
                struct obj *o = &g_obj[g_alias[i].obj];
                o->whole = true;
                *(u64 *)a3 = o->home; /* it lands at the object's home */
                printf("op compiler maps %s RW at its home\n", o->name);
                return 0;
            }
        fault("compiler map of no RW alias", a1, 0);
    }
    if (fn == FN_SYSCALL) { /* the number in rax */
        if (rax != 6)
            fault("compiler syscall", rax, a1);
        for (u32 i = 0; i < g_nalias; i++)
            if (g_alias[i].fd == (int)a1) {
                g_alias[i].fd = -1;
                g_open_aliases--, g_closed++;
                return 0;
            }
        fault("compiler close of a descriptor ghostldr did not open", a1, 0);
    }
    if (fn == FN_MEMCPY) {
        compiler_write(a1, (const u8 *)a2, a3);
        return a1;
    }
    fault("compiler call", fn, 0);
    return 0;
}

int jit_copy_to(u64 dst, const void *src, size_t len)
{
    compiler_write(dst, src, len);
    return 0;
}

/* a compiler alias passed here: its number + 3000 */
int jit_recv_fd(int cfd)
{
    for (u32 i = 0; i < g_nalias; i++)
        if (g_alias[i].fd == cfd)
            return 3000 + (int)i;
    fault("recv of no alias", (u64)cfd, 0);
    return -1;
}

static struct obj *here(int fd)
{
    u32 i = (u32)(fd - 3000);
    return fd >= 3000 && i < g_nalias ? &g_obj[g_alias[i].obj] : NULL;
}

static int k_fstat(int fd, void *st)
{
    struct obj *o = here(fd);
    if (!o)
        return -1;
    *(u64 *)((u8 *)st + 0x48) = o->size;
    return 0;
}

static u32 g_here_closed;
static int k_close(int fd)
{
    g_here_closed++;
    return 0;
}

static u64 g_res_lo, g_res_hi;
static int k_reserve(void **addr, size_t len, int flags, size_t align)
{
    u8 *p = mmap(*addr, len, 3, MAP_PRIVATE_ | MAP_ANON_, -1, 0);
    if (p != (void *)-1 && (u64)p < 0x1000000000UL) /* the core wants it high: the hint itself */
        p = mmap(*addr, len, 3, MAP_PRIVATE_ | MAP_ANON_ | MAP_FIXED, -1, 0);
    if (p == (void *)-1 || (u64)p & (align - 1))
        fault("host reserve", (u64)*addr, len);
    printf("op reserve %lx bytes at %s\n", (u64)len, (u64)p == (u64)*addr ? "the hint" : "another address");
    g_res_lo = (u64)p, g_res_hi = (u64)p + len;
    *addr = p;
    return 0;
}

static int k_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off, void **res)
{
    struct obj *o = here(fd);
    if (!o || (u64)addr < g_res_lo || (u64)addr + len > g_res_hi || !(flags & MAP_FIXED))
        fault("view outside the reserved range", (u64)addr, len);
    if (off || len != o->size)
        return (int)0x80020016; /* a view is the whole object */
    if (prot != (PROT_READ | PROT_EXEC))
        fault("view prot", (u64)prot, 0);
    memcpy(addr, o->bytes, len);
    o->view = (u64)addr;
    printf("op view %s at +%lx\n", o->name, (u64)addr - g_res_lo);
    *res = addr;
    return 0;
}

static int k_mprotect(const void *addr, size_t len, int prot) { return 0; }

static u64 g_direct;
void *direct_alloc(size_t size, int prot, u64 fixed)
{
    if (!fixed || fixed < g_res_lo || fixed + size > g_res_hi || (fixed & 0x3FFF) || (size & 0x3FFF))
        fault("direct memory outside the reserved range", fixed, size);
    for (u32 i = 0; i < NOBJ; i++)
        if (g_obj[i].view && fixed < g_obj[i].view + g_obj[i].size && fixed + size > g_obj[i].view)
            fault("direct memory over a view", fixed, size);
    memset((void *)fixed, 0xA5, size); /* dirty, as SWRR's memory is */
    g_direct += size;
    return (void *)fixed;
}

static u8 *slurp(const char *path, u64 *size)
{
    void *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, 2);
    *size = (u64)ftell(f);
    fseek(f, 0, 0);
    u8 *b = malloc(*size + 16);
    if (b && fread(b, 1, *size, f) != *size)
        b = NULL;
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    g_ctx = &ctx;
    ctx.jit_shm_alias = FN_ALIAS, ctx.jit_shm_map = FN_MAP, ctx.jit_syscall = FN_SYSCALL, ctx.jit_memcpy = FN_MEMCPY;
    ctx.oob_scratch = (u64)scratch, ctx.staging = (u64)staging, ctx.staging_size = sizeof(staging);
    K.fstat = k_fstat, K.close = k_close, K.reserve_va = k_reserve, K.mmap = k_mmap, K.mprotect = k_mprotect;
    for (u32 i = 0; i < NOBJ; i++) {
        g_obj[i].bytes = malloc(g_obj[i].size);
        memset(g_obj[i].bytes, 0x5A, g_obj[i].size); /* the emulator's code */
    }
    struct module **tail = &g_modules;
    for (int i = 1; i < argc; i++) {
        u64 size;
        u8 *buf = slurp(argv[i], &size);
        struct module *m = calloc(1, sizeof(*m));
        const char *slash = strrchr_c(argv[i], '/');
        m->name = slash ? slash + 1 : argv[i];
        m->is_main = !strcmp(m->name, "eboot.bin");
        if (!buf || module_parse(m, buf, size) != 0) {
            printf("cannot parse %s\n", argv[i]);
            return 2;
        }
        if (m->is_main)
            g_main_mod = m;
        *tail = m, tail = &m->next;
    }
    int rc = bx_prepare();
    printf("prepare %d, %u alias calls, %u RW aliases\n", rc, g_alias_calls, g_rw_aliases);
    struct module *m = g_modules;
    while (m && !m->bx)
        m = m->next;
    if (rc || !m)
        return 0;
    u64 lo, hi;
    arena_code_region(&lo, &hi);
    printf("arena now %lx-%lx\n", lo, hi);
    if (bx_place(m) != 0)
        return 0;
    if (m->base < 0x1000000000UL)
        printf("base %lx: below 0x10_0000_0000 (a host address)\n", m->base);
    u8 *text = calloc(1, m->text_size);
    for (u32 i = 0; i < m->phnum; i++)
        if (m->phdrs[i].type == PT_LOAD && (m->phdrs[i].flags & 1))
            memcpy(text + m->phdrs[i].vaddr, m->seg_data[i], m->phdrs[i].filesz);
    rc = bx_write_text(m, text);
    printf("write %d\n", rc);
    printf("executable +0 %d, +%lx %d, +%lx %d\n", bx_in_view(m, m->base), m->text_size / 2,
           bx_in_view(m, m->base + m->text_size / 2), m->text_size - 1, bx_in_view(m, m->base + m->text_size - 1));
    /* every byte: the segment as written, the data segments as in the file
     * then zero, each object as the module at its view (zero outside) */
    u64 bad = 0;
    for (u64 x = 0; x < m->text_size; x++)
        bad += *(u8 *)(m->base + x) != text[x];
    for (u32 i = 0; i < m->phnum; i++) {
        const struct elf_phdr *p = &m->phdrs[i];
        if (p->type != PT_LOAD || (p->flags & 1))
            continue;
        for (u64 x = 0; x < p->memsz; x++)
            bad += *(u8 *)(m->base + p->vaddr + x) != (x < p->filesz ? m->seg_data[i][x] : 0);
    }
    for (u32 i = 0; i < NOBJ; i++) {
        struct obj *o = &g_obj[i];
        if (!o->view)
            continue;
        s64 off = (s64)(o->view - m->base);
        for (u64 x = 0; x < o->size; x++) {
            s64 at = off + (s64)x;
            bad += o->bytes[x] != (at >= 0 && at < (s64)m->text_size ? text[at] : 0);
        }
        printf("object %s at +%lx%s\n", o->name, (u64)off, o->dropped ? ", out of the arena" : ", STILL IN THE ARENA");
    }
    printf("bytes wrong %lu; direct %lu KiB; compiler aliases left open %u (closed %u); here closed %u\n", bad,
           g_direct >> 10, g_open_aliases, g_closed, g_here_closed);
    return 0;
}
