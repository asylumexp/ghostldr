/*
 * swap.c (the extension's swap file), built for this machine as x86-64
 * (test_swap.py) and run against swap_os.c: one heap of 8 MiB backed by
 * 2 MiB of memory is written and read back whole, so most of its pages go
 * out to the swap file and come back; pages never touched read as zeros; a
 * kernel read into a page that is out fails, so a syscall's buffer is pinned:
 * a pinned page stays while the others are paged through.  The heap's
 * address is not a multiple of a page here (macOS's mmap ignores the
 * alignment asked for): pages count from its start.
 *
 *     swap_host ROOT
 *
 * Prints "ok ..." per check and "FAILED ..." for each that fails.
 */
#include "ghost_api.h"

int os_init(const char *root);
void os_log(const char *line);
void *os_calloc(u64 n);
void os_free(void *p);
int os_vfmt(char *buf, u64 size, const char *f, __builtin_va_list ap);
int os_reserve(void **addr, size_t len, int flags, size_t align);
int os_alloc_direct(off_t start, off_t end, size_t len, size_t align, int type, off_t *out);
int os_map_direct(void **addr, size_t len, int prot, int flags, off_t phys, size_t align);
int os_mprotect(const void *addr, size_t len, int prot);
int os_munmap(void *addr, size_t len);
int os_open(const char *path, int flags, int mode);
ssize_t os_pread(int fd, void *buf, size_t n, off_t off);
ssize_t os_write(int fd, const void *buf, size_t n);
off_t os_lseek(int fd, off_t off, int whence);
int os_close(int fd);
long os_kernel_read(void *buf, size_t n);
void os_install(int (*hook)(u64 addr));
extern volatile unsigned long os_faults;

/* swap.c */
void swap_pin(const void *buf, u64 len);
void swap_unpin(const void *buf, u64 len);
void swap_log(void);
void swap_end(void);

#define MIB (1UL << 20)
#define PAGE 0x10000UL

const struct ghost_api *G;
static struct kfuncs g_k;
static struct ghost_api g_api;
static struct module *g_main;
static int (*g_hook)(u64 addr);
static int g_failed, g_core_allocs, g_core_maps;

static void hlog(const char *f, ...)
{
    char line[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    os_vfmt(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    os_log(line);
}

static void expect(bool ok, const char *what)
{
    hlog("%s %s", ok ? "ok" : "FAILED", what);
    g_failed += !ok;
}

static void *h_page_alloc(size_t n) { return os_calloc(n); }
static void h_page_free(void *p, size_t n) { os_free(p); }
static void h_lock(volatile int *l)
{
    while (__sync_lock_test_and_set(l, 1))
        ;
}
static void h_unlock(volatile int *l) { __sync_lock_release(l); }
static size_t h_direct_size(void) { return 5UL << 30; }

/* the core's own calls the swap passes the title's other heaps to */
static s32 core_alloc(s64 start, s64 end, u64 len, u64 align, s32 type, s64 *out)
{
    g_core_allocs++;
    *out = 0x1000;
    return 0;
}
static s32 core_map(void **addr, u64 len, s32 prot, s32 flags, s64 phys, u64 align)
{
    g_core_maps++;
    return 0;
}
static s32 core_release(s64 start, u64 len) { return 0; }
static u64 h_hle_lookup(const char *lib, const char *name)
{
    return !strcmp(name, "sceKernelAllocateDirectMemory") ? (u64)core_alloc
           : !strcmp(name, "sceKernelMapDirectMemory")     ? (u64)core_map
           : !strcmp(name, "sceKernelReleaseDirectMemory") ? (u64)core_release
                                                           : 0;
}

/* the plan's one swap entry: 8 MiB from call site 0 (no main module here), 2 resident */
static struct {
    struct prof_swap w;
    char name[8];
} g_rec = {{{REC_SWAP, sizeof(g_rec), 0}, 0, 8, 2, 0}, "TEST"};
static const void *h_profile_record(u32 kind, u32 n) { return kind == REC_SWAP && n == 0 ? &g_rec : NULL; }

static const struct ext_family *g_fam;
void harness_family(const struct ext_family *f) { g_fam = f; }

static void *fn(const char *name)
{
    for (u32 i = 0; g_fam && i < g_fam->nhle; i++)
        if (!strcmp(g_fam->hle[i].name, name))
            return (void *)g_fam->hle[i].fn;
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2 || os_init(argv[1]) != 0)
        return 2;
    g_k = (struct kfuncs){.open = os_open, .pread = os_pread, .write = os_write, .close = os_close,
                          .lseek = os_lseek, .munmap = os_munmap, .mprotect = os_mprotect,
                          .reserve_va = os_reserve, .alloc_direct = os_alloc_direct, .map_direct = os_map_direct,
                          .direct_size = h_direct_size};
    g_api = (struct ghost_api){.version = GHOST_API_VERSION, .size = sizeof(struct ghost_api), .k = &g_k,
                               .main_mod = &g_main, .logf = hlog, .page_alloc = h_page_alloc,
                               .page_free = h_page_free, .spin_lock = h_lock, .spin_unlock = h_unlock,
                               .hle_lookup = h_hle_lookup, .profile_record = h_profile_record,
                               .swap_fault = &g_hook};
    G = &g_api;
    expect(g_fam && g_fam->init && g_fam->init() == 0, "the family is used: the plan has a swap entry");
    s32 (*alloc)(s64, s64, u64, u64, s32, s64 *) = fn("sceKernelAllocateDirectMemory");
    s32 (*map)(void **, u64, s32, s32, s64, u64) = fn("sceKernelMapDirectMemory");
    if (!alloc || !map)
        return 3;

    s64 off = 0;
    void *va = NULL;
    expect(alloc(0, 0, 4 * MIB, 0x10000, 0, &off) == 0 && off == 0x1000 && g_core_allocs == 1 &&
               map(&va, 4 * MIB, 3, 0, off, 0x10000) == 0 && g_core_maps == 1,
           "another heap's calls go to the core");
    expect(alloc(0, 0, 8 * MIB, 0x10000, 0, &off) == 0 && off >= 0x20000000000L && g_core_allocs == 1,
           "the entry's heap gets a synthetic offset");
    expect(map(&va, 8 * MIB, 3, 0, off, 0x10000) == 0 && va && g_core_maps == 1, "its mapping is address space");
    expect(g_hook != NULL, "the core's crash handler asks the swap first");
    os_install(g_hook);

    u64 *h = va;
    u32 pages = 8 * MIB / PAGE, used = 100;
    for (u32 p = 0; p < used; p++)
        h[p * PAGE / 8] = p * 7 + 1, h[(p + 1) * PAGE / 8 - 1] = ~(u64)p;
    bool same = true, zero = true;
    for (u32 p = 0; p < used; p++)
        same &= h[p * PAGE / 8] == p * 7 + 1 && h[(p + 1) * PAGE / 8 - 1] == ~(u64)p;
    for (u32 p = used; p < pages; p++)
        zero &= h[p * PAGE / 8] == 0;
    expect(same, "100 pages written through 32 resident ones read back whole");
    expect(zero, "pages never written read as zeros");
    expect(os_faults >= 2 * used, "the pages went out and came back (faults)");

    /* the reason for pins: a kernel read into page 0, which is out by now */
    expect(os_kernel_read(h, 64) < 0, "unpinned: the kernel can't read into a page that is out");
    /* pinned (as for a syscall's buffer), page 0 stays while all the others
     * come and go; unpinned, it can go out again */
    swap_pin(h, 64);
    for (u32 p = 1; p < used; p++)
        (void)*(volatile u64 *)&h[p * PAGE / 8];
    unsigned long f0 = os_faults;
    bool kept = h[0] == 1;
    expect(kept && os_faults == f0, "a pinned page stays resident while 99 others are paged through");
    swap_unpin(h, 64);
    for (u32 p = 1; p < used; p++)
        (void)*(volatile u64 *)&h[p * PAGE / 8];
    f0 = os_faults;
    expect(h[0] == 1 && os_faults > f0, "unpinned, it goes out again and comes back the same");
    swap_log();
    swap_end();
    expect(g_hook == NULL, "after the title's run, faults are the crash handler's again");
    return g_failed;
}
