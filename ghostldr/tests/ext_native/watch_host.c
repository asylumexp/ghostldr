/*
 * The watchdog's reads of Ghost of Tsushima 2.18's globals, now in the
 * extension (ext/src/ghost_watch.c), built for this machine
 * (test_ghost_watch.py): a fake eboot holds known values at the offsets the
 * core's crash.c used to read, and the three phases print the lines the
 * core printed (the same format strings).
 *
 *     watch_host            prints "log: ..." lines and "ok ..." / "FAILED ..."
 */
#include "ghost_api.h"

int vsnprintf(char *buf, unsigned long size, const char *fmt, __builtin_va_list ap);
int printf(const char *fmt, ...);
void *calloc(unsigned long n, unsigned long size);

const struct ghost_api *G;
static struct ghost_api g_api;
static struct boot_ctx g_ctx_v, *g_ctx_p = &g_ctx_v;
static struct module g_eboot, g_libc, *g_mods = &g_eboot, *g_main = &g_eboot;
static int g_failed;

int vfmt(char *buf, size_t size, const char *f, __builtin_va_list ap) { return vsnprintf(buf, size, f, ap); }
int fmt(char *buf, size_t size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(buf, size, f, ap);
    __builtin_va_end(ap);
    return n;
}

static char g_last[1024];
static void h_logf(const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    vfmt(g_last, sizeof(g_last), f, ap);
    __builtin_va_end(ap);
    printf("log: %s\n", g_last);
}
static void h_log_hex(const char *tag, const void *p, size_t n) { printf("log: %s: %zu bytes at %p\n", tag, n, p); }

/* readable: any address the test made (the fake eboot and its heap) */
static u8 *g_eb;
static u64 g_heap_lo, g_heap_hi;
static s32 h_qmp(void *p, void **st, void **en, s32 *prot)
{
    u64 a = (u64)p;
    bool in = (a >= (u64)g_eb && a < (u64)g_eb + 0x4200000) || (a >= g_heap_lo && a < g_heap_hi);
    *prot = in ? 1 : 0;
    return in ? 0 : (s32)0x80020016;
}
static void *h_dlsym(u32 h, const char *name) { return !strcmp(name, "sceKernelQueryMemoryProtection") ? (void *)h_qmp : NULL; }

static void h_nid(const char *name, char out[12]) { strlcpy(out, !strcmp(name, "sceLibcMspaceMallocStats") ? "MSPACESTATS" : "?", 12); }
static u64 h_mstats(u64 msp, u64 *st)
{
    st[1] = 300ull << 20, st[2] = 290ull << 20, st[3] = 285ull << 20, st[4] = 280ull << 20;
    return msp == 0x5150 ? 0 : 1;
}
static u64 h_export(struct module *m, const char *nid) { return m == &g_libc && !strcmp(nid, "MSPACESTATS") ? (u64)h_mstats : 0; }

static void expect(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "ok" : "FAILED", what);
    g_failed |= !ok;
}
static bool said(const char *line) { return strcmp(g_last, line) == 0; }

void ghost_watch(u64 eboot, u32 phase, u64 core_lo, u64 core_hi);

#define AT(T, off) (*(T *)(g_eb + (off)))

int main(void)
{
    g_eb = calloc(1, 0x4200000);
    u8 *heap = calloc(1, 0x10000);
    g_heap_lo = (u64)heap, g_heap_hi = (u64)heap + 0x10000;
    g_eboot = (struct module){.name = "eboot.bin", .is_main = true, .base = (u64)g_eb, .image_size = 0x4200000,
                              .next = &g_libc};
    g_libc = (struct module){.name = "libc.prx", .base = 0x10000, .image_size = 0x1000};
    g_api.ctx = &g_ctx_p, g_api.modules = &g_mods, g_api.main_mod = &g_main;
    g_api.logf = h_logf, g_api.log_hex = h_log_hex, g_api.host_dlsym = h_dlsym;
    g_api.compute_nid = h_nid, g_api.module_export = h_export;
    G = &g_api;
    u64 b = (u64)g_eb;

    /* phase 0: frame time, timescale and its reasons, boot-flow-8 gates */
    union { float f; u32 u; } dt = {0.0166f}, raw = {0.0333f}, one = {1.0f}, half = {0.5f};
    AT(u32, 0x16beac0) = dt.u, AT(u32, 0x16bea98) = raw.u, AT(u32, 0x16beac8) = one.u;
    AT(u64, 0x4141598) = 1000000, AT(u8, 0x3ba3a99) = 1;
    for (u32 r = 0; r < 28; r++)
        AT(u32, 0x16beb1c + r * 4) = r == 3 ? half.u : one.u;
    AT(s32, 0x3bbb310) = 1, AT(s32, 0x16d28fc) = 2, AT(s32, 0x16d289c) = -3, AT(s32, 0x3ba42fc) = 4;
    AT(s32, 0x3d4653c) = 5, AT(u8, 0x16d28a0) = 6;
    ghost_watch(b, 0, 0x9009d8000, 0x900a0c000);
    expect(said("watchdog: bootflow-8 gates: 3bbb310=1 16d28fc=2 16d289c=-3 3ba42fc=4 3d4653c=5 16d28a0=6 3d46630=0"),
           "phase 0 ends with the boot-flow gates");

    /* phase 1: movie pools, the forced title-ready flag (ctx flag 32), the title world target */
    u64 *mp = (u64 *)(g_eb + 0x288ae60 + 0x220 + 2 * 0x70);
    mp[3] = 0x100, mp[4] = 0x80, mp[13] = 0xabc;
    g_ctx_v.flags = 32;
    u8 *want = heap + 0x100, *s1 = heap + 0x200, *s2 = heap + 0x300;
    strlcpy((char *)want + 2, "TitleWorld", 32), strlcpy((char *)s1 + 2, "a", 8), strlcpy((char *)s2 + 2, "b", 8);
    u8 *entries = heap + 0x400;
    *(u64 *)entries = (u64)s1, *(u64 *)(entries + 0x38) = (u64)s2;
    AT(u64, 0x3d46630) = (u64)want, AT(s32, 0x3d46548) = 2, AT(u64, 0x3d46540) = (u64)entries;
    ghost_watch(b, 1, 0, 0);
    expect(AT(u8, 0x1a83739) == 1, "ctx flag 32: the title-ready flag forced");
    expect(said("watchdog: title-world target 'TitleWorld', 2 entries: 'a' 'b'"), "phase 1 ends with the target");
    g_ctx_v.flags = 0;

    /* phase 2: the CPU mspace through the title's libc, a blocking task in state 27 and its world's resources */
    AT(u64, 0x28b1690) = 0x5150;
    u8 *task = heap + 0x1000, *vt = heap + 0x2000, *world = heap + 0x3000, *list = heap + 0x4000;
    *(u64 *)task = (u64)vt, *(u32 *)(task + 0x208) = 27, *(u8 *)(task + 0x239) = 1;
    ((u64 *)vt)[2] = b + 0x123450; /* isBusy: in the eboot */
    *(u64 **)(g_eb + 0x3cc5808) = (u64 *)(heap + 0x5000);
    *(u64 *)(heap + 0x5000) = (u64)task;
    AT(u32, 0x3cc5810) = 1, AT(s32, 0x28e2158) = 7;
    AT(u64, 0x3ce40b8) = (u64)world;
    *(u64 *)(world + 0x38) = (u64)list, *(s32 *)(world + 0x40) = 1, *(u8 *)(world + 0x5c) = 0;
    *(u64 *)list = 0x1122334455667788ull, *(u16 *)(list + 8) = 0x10;
    AT(u64, 0x28e2118) = (u64)(heap + 0x6000), AT(u32, 0x28e2110) = 4; /* an empty load table: it waits */
    ghost_watch(b, 2, 0x9009d8000, 0x900a0c000);
    expect(said("watchdog:   1 of 1 not loaded"), "phase 2: the world's resource still loading");
    printf("%s\n", g_failed ? "FAILED some check" : "all checks passed");
    return g_failed;
}
