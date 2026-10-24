/*
 * Runs the core's profile.c and budget.c (and nid.c) in a host process
 * (built for this machine) against fakes of what they read on the console: the boot context
 * with a profile bundle, a direct-memory map with the holes a Ghost boot
 * logged (4532 MiB free in 6 holes), and an executable image whose tables
 * hold Ghost 2.18's values.  test_profiles.py builds and drives it:
 *
 *     profile_host BUNDLE plan|planfree|select FINGERPRINT_HEX|- [TITLE [OPTIONS]]
 *     profile_host BUNDLE rules FINGERPRINT_HEX|- TITLE FUNCTION LIB
 *     profile_host BUNDLE mmap - TITLE        (plan-free anonymous mmap)
 *
 * OPTIONS: v = the title imports a video decoder, n = it has no proc param.
 */
#include "ghost.h"

long write(int fd, const void *buf, unsigned long n);
int open(const char *path, int flags, ...);
long read(int fd, void *buf, unsigned long n);
int vsnprintf(char *buf, unsigned long size, const char *fmt, __builtin_va_list ap);
void exit(int code);

struct kfuncs K;
struct boot_ctx *g_ctx;
struct module *g_modules, *g_main_mod;

int vfmt(char *buf, size_t size, const char *f, __builtin_va_list ap) { return vsnprintf(buf, size, f, ap); }

int fmt(char *buf, size_t size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(buf, size, f, ap);
    __builtin_va_end(ap);
    return n;
}

void logf(const char *f, ...)
{
    char line[1200];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(line, sizeof(line) - 1, f, ap);
    __builtin_va_end(ap);
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    write(1, line, n);
}

/* ---- direct memory: the allocated ranges around the logged holes ---- */
static const struct {
    u64 s, e;
} k_used[] = {{0, 0x298000},         {0x1bdc000, 0x30a4000},     {0x10010000, 0x12800000},
              {0x13c40000, 0x13e00000}, {0x50080000, 0x503b8000}, {0x11e000000, 0x11e74c000}};

char *getenv(const char *name);
unsigned long strtoul(const char *s, char **end, int base);

static int fake_dmq(off_t off, int next, void *out, size_t size)
{
    struct {
        off_t start, end;
        int type;
    } *q = out;
    /* GHOSTLDR_TEST_DMQ_GAP=LO-HI: find-next fails from inside [LO, HI)
     * although allocations follow (what a console did after GPU memory was
     * released); the walk must step over it */
    const char *gap = getenv("GHOSTLDR_TEST_DMQ_GAP");
    if (gap) {
        char *dash;
        u64 lo = strtoul(gap, &dash, 0), hi = strtoul(dash + 1, NULL, 0);
        if ((u64)off >= lo && (u64)off < hi)
            return (int)0x80020016;
    }
    for (u32 i = 0; i < ARRAY_SIZE(k_used); i++)
        if (k_used[i].e > (u64)off) {
            q->start = (off_t)k_used[i].s, q->end = (off_t)k_used[i].e, q->type = 0;
            return 0;
        }
    return (int)0x80020016;
}

static size_t fake_direct_size(void) { return 0x120000000; }

/* GHOSTLDR_TEST_PS4_CAP=MIB: the PS4 flag, and a kernel that hands out at
 * most MIB MiB of direct memory however large its holes (the fourth PS4
 * report).  budget.c measures that by taking pieces and releasing them. */
static u64 g_cap, g_taken, g_next_phys = 0x50400000;

static int fake_alloc(off_t start, off_t end, size_t len, size_t align, int type, off_t *out)
{
    if (g_taken + len > g_cap)
        return (int)0x80020023;
    g_taken += len;
    *out = (off_t)g_next_phys;
    g_next_phys += len;
    return 0;
}

static int fake_release(off_t phys, size_t len)
{
    g_taken -= len;
    return 0;
}

static int fake_avail(off_t start, off_t end, size_t align, off_t *s, size_t *size)
{
    *s = 0x50400000, *size = 0xcdc00000; /* the largest hole, 2 MiB aligned (3292 MiB) */
    return 0;
}

void *host_dlsym(u32 handle, const char *name)
{
    if (g_cap && strcmp(name, "sceKernelReleaseDirectMemory") == 0)
        return (void *)fake_release;
    return strcmp(name, "sceKernelDirectMemoryQuery") == 0 ? (void *)fake_dmq : NULL;
}

void *host_real(const char *name)
{
    return g_cap && strcmp(name, "sceKernelAllocateDirectMemory") == 0 ? (void *)fake_alloc : NULL;
}
u64 stub_alloc_code(const u8 *code, u32 len) { return 0x5000 + code[1]; } /* mov eax, idx: index in byte 1 */
int hle_alloc_direct(off_t start, off_t end, size_t len, size_t align, int type, off_t *out) { return 0; }
int budget_flex_gate(u64 len);
void budget_flex_note(s64 len);

/* flexible memory as hle.c maps it (gate, direct memory zeroed for a
 * plan-free title, note), from a pool that starts out dirty */
#define POOL (8UL << 20)
static u8 g_pool[POOL] __attribute__((aligned(0x4000)));
static u64 g_pool_used, g_last_block, g_last_size;
int hle_map_flex(void **addr, size_t len, int prot, int flags)
{
    int rc = budget_flex_gate(len);
    if (rc)
        return rc;
    if (g_pool_used + len > POOL)
        return (int)0x8002000c;
    if (!g_pool_used)
        memset(g_pool, 0xA5, POOL);
    *addr = g_pool + g_pool_used;
    if (!profile_has(PF_PLAN))
        memset(*addr, 0, ALIGN_UP(len, 0x4000));
    g_last_block = (u64)*addr, g_last_size = ALIGN_UP(len, 0x4000);
    g_pool_used += g_last_size;
    budget_flex_note((s64)g_last_size);
    return 0;
}
u64 direct_block_size(u64 addr) { return addr && addr == g_last_block ? g_last_size : 0; }
void direct_free(void *addr) { g_last_block = 0; }
static int g_errno;
static int *fake_errno(void) { return &g_errno; }
static int fake_reserve(void **addr, size_t len, int flags, size_t align)
{
    *addr = (void *)0x7000000000UL;
    return 0;
}
static int g_mprotect;
static int fake_mprotect(const void *addr, size_t len, int prot) { return g_mprotect = prot, 0; }
static void *fake_file_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) { return (void *)0x1234000; }
u64 ext_hle_lookup(const char *lib, const char *nid, const char *name)
{
    return strcmp(name, "mmap") == 0 ? (u64)fake_file_mmap : 0;
}
u64 c_rule(u32 idx, const u64 *args);
#if defined(__APPLE__) && defined(__x86_64__)
/* profile.c's rule_entry calls the ELF name; Mach-O prefixes C symbols with _ */
__asm__(".globl c_rule\nc_rule: jmp _c_rule");
#endif
int budget_alloc_gate(u64 len);
u64 budget_resolve(const char *nid);
void compute_nid(const char *name, char out[12]);

/* ---- the executable: Ghost 2.18's plan tables, a proc param whose mem param
 * says 116 MiB of flexible memory ---- */
#define MiB (1UL << 20)
#define TEXT_SIZE 0x1200000
#define BASE 0x800000000UL
static u8 g_text[TEXT_SIZE];
static struct module g_main;
static const u64 k_stream[6] = {0x46000000, 0x46000000, 0x46000000, 0x84800000, 0x125800000, 0x125800000};

static void make_text(const char *opt)
{
    bool video = false, param = true;
    for (; *opt; opt++)
        video |= *opt == 'v', param &= *opt != 'n';
    memcpy(g_text + 0x111cd50, k_stream, sizeof(k_stream));
    *(u64 *)(g_text + 0x111cd20) = 384 * MiB; /* CPU malloc heap */
    *(u64 *)(g_text + 0x111ce10) = 1390 * MiB; /* general GPU heap */
    *(u64 *)(g_text + 0x111cde0) = 332 * MiB;  /* type-0 engine heap */
    /* proc param at 0x1000: +0x40 -> mem param at 0x1100 {size 0x38, ..., +0x10 -> 0x1200} -> 116 MiB */
    *(u64 *)(g_text + 0x1040) = BASE + 0x1100;
    *(u64 *)(g_text + 0x1100) = 0x38;
    *(u64 *)(g_text + 0x1110) = BASE + 0x1200;
    *(u64 *)(g_text + 0x1200) = 0x7400000;
    g_main = (struct module){.name = "eboot.bin", .is_main = true, .base = BASE, .image_size = TEXT_SIZE,
                             .text_size = TEXT_SIZE, .proc_param = param ? BASE + 0x1000 : 0};
    if (video)
        g_main.import_libs[g_main.n_import_libs++] = (struct libref){1, "libSceVideodec2"};
    g_modules = g_main_mod = &g_main;
}

static u8 g_bundle[1 << 16];
static u64 g_ctx_mem[64];

static int hexval(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    int fd = open(argv[1], 0);
    if (fd < 0 || read(fd, g_bundle, sizeof(g_bundle) - 8) <= 0)
        return 3;
    g_ctx = (struct boot_ctx *)g_ctx_mem;
    g_ctx_mem[63] = (u64)g_bundle;
    K.direct_size = fake_direct_size;
    const char *cap = getenv("GHOSTLDR_TEST_PS4_CAP");
    if (cap) {
        g_cap = strtoul(cap, NULL, 0) * MiB;
        g_ctx->flags |= CTX_FLAG_PS4;
        K.avail_direct = fake_avail;
    }
    u8 fp[20];
    bool have_fp = argc > 3 && strlen(argv[3]) == 40;
    for (u32 i = 0; have_fp && i < 20; i++)
        fp[i] = (u8)(hexval(argv[3][2 * i]) << 4 | hexval(argv[3][2 * i + 1]));
    const char *sc = argv[2];

    if (strcmp(sc, "select") == 0 || strcmp(sc, "plan") == 0 || strcmp(sc, "planfree") == 0) {
        /* "select": nothing picked yet (no ghostfs); else the title in argv[4] */
        if (strcmp(sc, "select") != 0)
            profile_select(argc > 4 ? argv[4] : "");
        make_text(argc > 5 ? argv[5] : "");
        profile_bind(have_fp ? fp : NULL);
        logf("title %s thread %s has plan %u hooks %u diag %u save %u param1 %d", profile_title(),
             profile_thread_name(), profile_has(PF_PLAN), profile_has(PF_HOOKS), profile_has(PF_DIAG),
             profile_hdr()->save_blocks, profile_app_param(1));
        int rc = budget_patch_text(&g_main, g_text);
        logf("patch rc %d reported %lu MiB, stream table %lu MiB, cpu heap %lu MiB", rc, g_reported_dmem / MiB,
             *(u64 *)(g_text + 0x111cd50) / MiB, *(u64 *)(g_text + 0x111cd20) / MiB);
        if (g_cap)
            logf("ps4 cap: %lu MiB still taken after the plan", g_taken / MiB);
        logf("lazy site 0x896126 %d, gate %x, flex gate %x", budget_lazy_site(0x896126),
             budget_alloc_gate(g_reported_dmem + 0x10000), budget_flex_gate(449 * MiB));
        char nid[12];
        compute_nid("sceKernelIsNeoMode", nid);
        u64 neo = budget_resolve(nid);
        compute_nid("sceKernelAvailableDirectMemorySize", nid);
        int (*avail)(off_t, off_t, size_t, off_t *, size_t *) = (void *)budget_resolve(nid);
        off_t off = 0;
        size_t size = 0;
        int arc = avail ? avail(0, 0x120000000, 0x200000, &off, &size) : -1;
        logf("neo %s, available direct %x: %lx + %lu MiB", neo ? ((int (*)(void))neo)() == 0 ? "0" : "?" : "host",
             arc, (u64)off, size / MiB);
        return 0;
    }
    if (strcmp(sc, "exec") == 0) { /* the code ranges bigexec.c would read */
        profile_select(argv[4]);
        make_text("");
        profile_bind(have_fp ? fp : NULL);
        const struct prof_exec *x;
        u32 k = 0;
        for (; (x = profile_record(REC_EXEC, k)); k++)
            for (u32 i = 0; i < x->count; i++)
                logf("exec %s %x-%x", x->module, x->range[2 * i], x->range[2 * i + 1]);
        logf("exec records %u", k);
        return 0;
    }
    if (strcmp(sc, "rules") == 0) {
        profile_select(argv[4]);
        make_text("");
        profile_bind(have_fp ? fp : NULL);
        char nid[12];
        compute_nid(argv[5], nid);
        u64 stub = profile_resolve(argv[6], nid);
        u8 out[16] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
        u64 args[6] = {0, (u64)out, 0, 0, 0, 0};
        u64 ret = stub ? c_rule((u32)(stub - 0x5000), args) : 0;
        char hex[40];
        for (u32 i = 0; i < 16; i++)
            fmt(hex + 2 * i, 3, "%02x", out[i]);
        logf("rule stub %lx returns %lx, out %s", stub, ret, hex);
        return 0;
    }
    if (strcmp(sc, "mmap") == 0) {
        /* a title with no mem param (448 MiB of flexible memory), as OpenOrbis homebrew */
        profile_select(argv[4]);
        make_text("n");
        profile_bind(NULL);
        if (budget_patch_text(&g_main, g_text) != 0)
            return 5;
        K.errno_fn = fake_errno, K.reserve_va = fake_reserve, K.mprotect = fake_mprotect;
        char nid[12];
        compute_nid("mmap", nid);
        void *(*mm)(void *, size_t, int, int, int, off_t) = (void *)budget_resolve(nid);
        compute_nid("sceKernelMmap", nid);
        int (*kmm)(void *, size_t, int, int, int, off_t, void **) = (void *)budget_resolve(nid);
        compute_nid("munmap", nid);
        int (*unmap)(void *, size_t) = (void *)budget_resolve(nid);
        compute_nid("sceKernelAvailableFlexibleMemorySize", nid);
        int (*avail)(size_t *) = (void *)budget_resolve(nid);
        if (!mm || !kmm || !unmap || !avail)
            return 6;
        size_t left = 0;
        u8 *p = mm(NULL, 0x5000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        bool zero = p != (void *)-1;
        for (u32 i = 0; zero && i < 0x8000; i++)
            zero = p[i] == 0;
        avail(&left);
        logf("anon %s, zeroed %d, flexible left %lu MiB", p == (void *)-1 ? "failed" : "mapped", zero, left / MiB);
        logf("munmap %d", unmap(p, 0x5000));
        avail(&left);
        logf("after munmap flexible left %lu MiB", left / MiB);
        void *big = mm(NULL, 449 * MiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        logf("too big %s errno %d", big == (void *)-1 ? "MAP_FAILED" : "mapped", g_errno);
        void *res = NULL;
        int rc = kmm(NULL, 0x4000, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0, &res);
        logf("sceKernelMmap read-only %x mapped %d mprotect %d", rc, res != NULL, g_mprotect);
        logf("reserve %p", mm(NULL, 1UL << 30, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
        logf("file %p", mm(NULL, 0x4000, PROT_READ, MAP_PRIVATE, 5, 0));
        return 0;
    }
    return 4;
}
