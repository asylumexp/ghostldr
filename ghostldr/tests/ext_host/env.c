/*
 * Runs the core's real ext.c (fetch, verify, place, relocate, ext_init,
 * self-test, NID lookups), resolve.c (binding order, policies, stubs), elf.c
 * and nid.c against a real ext.bin in an ordinary x86-64 process: on macOS
 * through Rosetta, or natively on x86-64 Linux.  The core services they need
 * are faked here; the "small RX area" and the "JIT block" are mappings that
 * are read+execute except while the fake compiler writes them, so an
 * extension writing to its own text faults as it would on the console.
 * test_ext.py builds and drives it:
 *
 *     ext_host MODE EXT_BIN CRC [NID_FILE]
 *     MODE: small | jit | tiny (small area too small: falls back to a JIT
 *           block) | badcrc | short (the server sends fewer bytes) | nosock
 *           | resolve (imports bound through the self-test family)
 *
 * Prints "ext_host: ..." lines and, with NID_FILE (one NID per line), "nid NID
 * NAME" for each; exits 0 when every step of the mode behaved as expected.
 */
#include "ghost_api.h"

/* sys.c: the host OS, kept out of this file's ghost.h world */
void *sys_map(u64 size);
void sys_protect(void *addr, u64 size, int writable);
int sys_open(const char *path);
s64 sys_read(int fd, void *buf, u64 n);
int sys_close(int fd);
void sys_vlog(const char *fmt, __builtin_va_list ap);
void *sys_calloc(u64 n);
s64 sys_read_file(const char *path, char **out);
u64 sys_strtoul(const char *s);
int sys_unused_fd(void);
int sys_vfmt(char *buf, u64 size, const char *fmt, __builtin_va_list ap);

#ifdef __APPLE__
/* resolve.c's assembly calls the unprefixed C name */
__asm__(".globl c_unresolved\nc_unresolved:\n    jmp _c_unresolved\n");
#endif

struct kfuncs K;
struct boot_ctx *g_ctx;
/* the small RX area after the core (arena.c) */
static u8 *g_small;
static u64 g_small_cur, g_small_end;
struct module *g_modules, *g_main_mod;
u64 g_reported_dmem;

void logf(const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    sys_vlog(f, ap);
    __builtin_va_end(ap);
}

void *arena_alloc(size_t n) { return sys_calloc(n); }

char *arena_strdup(const char *s)
{
    char *d = sys_calloc(strlen(s) + 1);
    memcpy(d, s, strlen(s));
    return d;
}

int fmt(char *buf, size_t size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = sys_vfmt(buf, size, f, ap);
    __builtin_va_end(ap);
    return n;
}

const char *strrchr_c(const char *s, int c)
{
    const char *r = NULL;
    for (; *s; s++)
        if (*s == c)
            r = s;
    return r;
}

const struct ghost_api g_ghost_api = {
    .version = GHOST_API_VERSION,
    .size = sizeof(struct ghost_api),
    .ctx = &g_ctx,
    .k = &K,
    .modules = &g_modules,
    .main_mod = &g_main_mod,
    .reported_dmem = &g_reported_dmem,
    .logf = logf,
    .arena_alloc = arena_alloc,
    .compute_nid = compute_nid,
};

/* the rest of the core that resolve.c reaches */
#define HOST_USLEEP 0x7a5e0000ULL
#define CORE_OPEN 0xc0de0000ULL
u64 hle_lookup(const char *lib, const char *name)
{
    return strcmp(name, "sceKernelOpen") == 0 && (!lib || strcmp(lib, "libkernel") == 0) ? CORE_OPEN : 0;
}
u64 module_export(struct module *m, const char *nid) { return 0; }
/* title profiles (profile.c): no rules, no fingerprint binding here */
u64 profile_resolve(const char *lib, const char *nid) { (void)lib, (void)nid; return 0; }
void profile_bind(const u8 *fingerprint) { (void)fingerprint; }
int host_find_module(const char *name, u32 *handle_out) { return -1; }
u64 call_gadget6(u64 fn, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6) { return (u64)-1; }
void *host_dlsym(u32 handle, const char *name)
{
    return strcmp(name, "sceKernelUsleep") == 0 ? (void *)HOST_USLEEP : NULL;
}
u64 arena_small_alloc(u64 size)
{
    size = ALIGN_UP(size, 16);
    if (g_small_cur + size > g_small_end)
        return 0;
    u64 p = g_small_cur;
    g_small_cur += size;
    return p;
}

/* direct memory */
void *page_alloc(size_t size) { return sys_map(ALIGN_UP(size, 0x4000)); }
void page_free(void *addr, size_t size) { (void)addr, (void)size; } /* leaked: the process is short-lived */


u64 arena_small_alloc_top(u64 size, u64 keep)
{
    size = ALIGN_UP(size, 0x4000);
    if (g_small_end < g_small_cur || g_small_end - g_small_cur < keep + size)
        return 0;
    g_small_end -= size;
    return g_small_end;
}

/* the compiler's writes into RX memory */
int arena_write(u64 dst, const void *src, u64 size)
{
    u64 lo = ALIGN_DOWN(dst, 0x4000), hi = ALIGN_UP(dst + size, 0x4000);
    sys_protect((void *)lo, hi - lo, 1);
    memcpy((void *)dst, src, size);
    sys_protect((void *)lo, hi - lo, 0);
    return 0;
}

int arena_punch_rw(u64 addr, u64 size)
{
    sys_protect((void *)addr, size, 1);
    memset((void *)addr, 0, size);
    return 0;
}

static u32 g_jit_blocks;

int jit_block_create(struct jit_block *b, u64 size, u64 want_rx)
{
    memset(b, 0, sizeof(*b));
    b->size = ALIGN_UP(size, 0x4000);
    b->rx = b->rw = (u64)sys_map(b->size);
    sys_protect((void *)b->rx, b->size, 0);
    g_jit_blocks++;
    return 0;
}

int jit_block_write(struct jit_block *b, u64 offset, const void *src, size_t len)
{
    if (offset + len > b->size)
        return -1;
    return arena_write(b->rw + offset, src, len);
}

static int k_close(int fd) { return sys_close(fd); }
static ssize_t k_read(int fd, void *buf, size_t n) { return sys_read(fd, buf, n); }
static bool g_flex_full;
static u32 g_flex_maps, g_flex_unmaps;
static int k_map_flex(void **addr, size_t len, int prot, int flags)
{
    if (g_flex_full)
        return (int)0x8002000c;
    *addr = sys_map(len);
    g_flex_maps++;
    return 0;
}
static int k_munmap(void *addr, size_t len)
{
    g_flex_unmaps++;
    return 0;
}
static int k_module_info(u32 handle, struct sce_module_info *info)
{
    strlcpy(info->name, handle == 0x77 ? EXT_SELFTEST_ALIAS ".sprx" : "libSceOther.sprx", sizeof(info->name));
    return 0;
}

static int g_failed;
static void expect(bool ok, const char *what)
{
    logf("ext_host: %s %s", ok ? "ok" : "FAILED", what);
    if (!ok)
        g_failed = 1;
}

void stubs_flush(void);

/* imports of a fake module bound by the real resolve_import */
static void resolve_tests(void)
{
    static struct module m;
    m.name = "test.prx";
    const char *libs[] = {EXT_SELFTEST_ALIAS, "libkernel", EXT_SELFTEST_LIB, "libSceFake"};
    for (u32 i = 0; i < 4; i++)
        m.import_libs[i] = (struct libref){(u16)i, libs[i]};
    m.n_import_libs = 4;
    m.needed_mods[0] = (struct libref){0, "libkernel"};
    m.n_needed_mods = 1;
    char sym[32], self[12], missing[12];
    compute_nid(EXT_SELFTEST_NAME, self);
    compute_nid("ghostldrExtMissing", missing);
    u64 fn = ext_hle_lookup(EXT_SELFTEST_LIB, self, NULL);
    /* "NID#lib#module" with base64-digit ids: A=0 B=1 C=2 D=3 */
    fmt(sym, sizeof(sym), "%s#A#A", self);
    expect(fn && resolve_import(&m, sym, false, true) == fn, "by name through a library alias");
    fmt(sym, sizeof(sym), "%s#C#A", EXT_SELFTEST_NID);
    expect(resolve_import(&m, sym, false, true) == fn, "by NID alone (no known name)");
    expect(resolve_import(&m, "1jfXLRVzisc#B#A", false, true) == HOST_USLEEP, "host by name");
    expect(resolve_import(&m, "1G3lF1Gg1k8#B#A", false, true) == CORE_OPEN, "the core's override first");
    fmt(sym, sizeof(sym), "%s#C#A", missing);
    u64 denied = resolve_import(&m, sym, false, true);
    u64 unresolved = resolve_import(&m, "AAAAAAAAAAA#D#A", false, true);
    stubs_flush();
    expect(denied && ((s64(*)(void))denied)() == (s64)EXT_SELFTEST_STUB_RET,
           "no host fallback: the stub returns the library's error");
    expect(unresolved && ((s64(*)(void))unresolved)() == 0, "other unresolved imports return 0");
    void *out = NULL;
    expect(ext_dlsym(0x55, EXT_SELFTEST_NAME, &out) == 0 && (u64)out == fn, "dlsym finds the extension's function");
    expect(ext_dlsym(0x77, "ghostldrExtMissing", &out) == -1, "dlsym refuses a no-host library's module");
    expect(ext_dlsym(0x55, "ghostldrExtMissing", &out) == 1, "dlsym passes other modules to the host");
}

static void lookup_nids(const char *path)
{
    char *text;
    s64 n = sys_read_file(path, &text);
    for (s64 i = 0; i < n;) {
        s64 j = i;
        while (j < n && text[j] != '\n')
            j++;
        text[j] = 0;
        const char *name = nid_to_name(text + i);
        logf("nid %s %s", text + i, name ? name : "-");
        i = j + 1;
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        logf("usage: ext_host MODE EXT_BIN CRC [NID_FILE]");
        return 2;
    }
    const char *mode = argv[1];
    static struct boot_ctx ctx;
    ctx.magic = BOOT_MAGIC;
    ctx.size = sizeof(ctx);
    ctx.ext_crc = sys_strtoul(argv[3]);
    g_ctx = &ctx;
    K.read = k_read;
    K.close = k_close;
    K.map_flex = k_map_flex;
    K.munmap = k_munmap;
    K.module_info = k_module_info;
    ctx.libkernel_handle = 0x2001;
    g_flex_full = strcmp(mode, "jit") == 0; /* and the blob falls back to direct memory */
    char *blob;
    ctx.ext_size = (u64)sys_read_file(argv[2], &blob);
    ctx.ext_fd = (u64)sys_open(argv[2]);
    /* 64 KiB of small area, as after a 192 KiB core image.  An extension
     * that needs more goes to a JIT block on the console ("tiny" tests
     * that); the other modes grow the area so the small-area path stays
     * tested whatever the extension's size. */
    u64 need = ((const struct ext_blob *)blob)->image_memsz + 0x4000 /* ext.c STUB_AREA_KEEP */;
    if (need > 0x10000)
        logf("ext_host: the extension needs %lx bytes of small area, more than a console's 0x10000", need);
    u64 small_size = strcmp(mode, "tiny") == 0 ? 0x8000 : need > 0x10000 ? ALIGN_UP(need, 0x4000) : 0x10000;
    g_small = sys_map(small_size);
    sys_protect(g_small, small_size, 0);
    g_small_cur = (u64)g_small, g_small_end = (u64)g_small + small_size;

    if (strcmp(mode, "badcrc") == 0) {
        ctx.ext_crc ^= 1;
        expect(ext_fetch() != 0, "a wrong CRC is refused");
        return g_failed;
    }
    if (strcmp(mode, "short") == 0) {
        ctx.ext_size += 100; /* the server closes before sending this much */
        expect(ext_fetch() != 0, "a short transfer is refused");
        return g_failed;
    }
    if (strcmp(mode, "nosock") == 0) {
        ctx.ext_fd = (u64)-1;
        expect(ext_fetch() != 0, "no socket stops the boot");
        ctx.ext_fd = (u64)sys_unused_fd();
        ctx.ext_size = 0;
        expect(ext_fetch() != 0, "size 0 stops the boot");
        return g_failed;
    }
    if (strcmp(mode, "jit") == 0)
        ctx.flags |= CTX_FLAG_EXT_JIT;

    expect(ext_fetch() == 0, "fetch");
    expect((s64)ctx.ext_fd == -1, "socket consumed");
    expect(nid_to_name("1jfXLRVzisc") && strcmp(nid_to_name("1jfXLRVzisc"), "sceKernelUsleep") == 0, "NID lookup");
    expect(!nid_to_name("1jfXLRVzis") && !nid_to_name("1jfXLRVziscc") && !nid_to_name("1jfXLRV!isc") && !nid_to_name("1jfXLRVzisd"),
           "malformed NIDs");
    expect(ext_start() == 0, "start (place, relocate, ext_init, self-test)");
    bool want_jit = strcmp(mode, "jit") == 0 || strcmp(mode, "tiny") == 0;
    expect(g_jit_blocks == (want_jit ? 1u : 0u), want_jit ? "placed in a JIT block" : "placed in the small area");
    char self[12];
    compute_nid(EXT_SELFTEST_NAME, self);
    expect(ext_hle_lookup(EXT_SELFTEST_LIB, self, NULL) != 0, "extension HLE lookup by library");
    expect(ext_hle_lookup(EXT_SELFTEST_ALIAS, self, NULL) != 0, "extension HLE lookup through an alias");
    expect(ext_hle_lookup(NULL, self, NULL) != 0, "extension HLE lookup without a library");
    expect(ext_hle_lookup("libkernel", self, NULL) == 0, "other libraries do not match");
    expect(ext_hle_lookup(EXT_SELFTEST_LIB, "1jfXLRVzisc", NULL) == 0, "other NIDs do not match");
    const struct ext_lib *pol = ext_lib_policy(EXT_SELFTEST_ALIAS);
    expect(pol && (pol->flags & EXT_LIB_NO_HOST) && !ext_lib_policy("libkernel"), "library policy by alias");
    if (strcmp(mode, "resolve") == 0)
        resolve_tests();
    if (argc > 4)
        lookup_nids(argv[4]);
    ext_boot_done();
    expect(nid_to_name("1jfXLRVzisc") == NULL, "NID table freed after boot");
    expect(g_flex_maps == g_flex_unmaps, "flexible memory returned");
    expect(ext_hle_lookup(EXT_SELFTEST_LIB, self, NULL) != 0, "extension HLE kept after boot");
    return g_failed;
}
