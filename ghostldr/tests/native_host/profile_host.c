/*
 * Runs the core's profile.c and budget.c (and nid.c) in an x86-64 host
 * process against fakes of what they read on the console: the boot context
 * with a profile bundle, a direct-memory map with the holes a Ghost boot
 * logged (4532 MiB free in 6 holes), and an executable image whose tables
 * hold Ghost 2.18's values.  test_profiles.py builds and drives it:
 *
 *     profile_host BUNDLE plan|planfree|select FINGERPRINT_HEX|- [TITLE [OPTIONS]]
 *     profile_host BUNDLE rules FINGERPRINT_HEX|- TITLE FUNCTION LIB
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

static int fake_dmq(off_t off, int next, void *out, size_t size)
{
    struct {
        off_t start, end;
        int type;
    } *q = out;
    for (u32 i = 0; i < ARRAY_SIZE(k_used); i++)
        if (k_used[i].e > (u64)off) {
            q->start = (off_t)k_used[i].s, q->end = (off_t)k_used[i].e, q->type = 0;
            return 0;
        }
    return (int)0x80020016;
}

static size_t fake_direct_size(void) { return 0x120000000; }

void *host_dlsym(u32 handle, const char *name)
{
    return strcmp(name, "sceKernelDirectMemoryQuery") == 0 ? (void *)fake_dmq : NULL;
}

void *host_real(const char *name) { return NULL; }
u64 stub_alloc_code(const u8 *code, u32 len) { return 0x5000 + code[1]; } /* mov eax, idx: index in byte 1 */
int hle_map_flex(void **addr, size_t len, int prot, int flags) { return 0; }
int hle_alloc_direct(off_t start, off_t end, size_t len, size_t align, int type, off_t *out) { return 0; }
u64 direct_block_size(u64 addr) { return 0; }
void direct_free(void *addr) {}
u64 c_rule(u32 idx, const u64 *args);
#ifdef __APPLE__
/* profile.c's rule_entry calls the ELF name; Mach-O prefixes C symbols with _ */
__asm__(".globl c_rule\nc_rule: jmp _c_rule");
#endif
int budget_alloc_gate(u64 len);
int budget_flex_gate(u64 len);
void budget_flex_note(s64 len);
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
    return 4;
}
