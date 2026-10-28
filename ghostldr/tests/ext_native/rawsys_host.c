/*
 * hle_rawsys.c, built for this machine as x86-64 (test_rawsys.py): small
 * functions with raw `mov eax/rax, N; mov r10, rcx; syscall` sites are
 * patched as a module's text is, then run.  Each must get what the libkernel
 * function of that number returns, with the carry flag and registers as the
 * kernel leaves them; GoldHEN's kexec (11) and unknown numbers get ENOSYS.
 *
 * Prints "ok ..." / "FAILED ..." lines; exits 0 when every check passed.
 */
#include "ghost_api.h"

long write(int fd, const void *buf, unsigned long n);
int vsnprintf(char *buf, unsigned long size, const char *fmt, __builtin_va_list ap);
int snprintf(char *buf, unsigned long size, const char *fmt, ...);
void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off);
void compute_nid(const char *name, char out[12]);

struct kfuncs K;
const struct ghost_api *G;
static int g_failed;

static void hlog(const char *f, ...)
{
    char line[600];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vsnprintf(line, sizeof(line) - 1, f, ap);
    __builtin_va_end(ap);
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    write(1, line, n);
}

static void expect(bool ok, const char *what)
{
    hlog("%s %s", ok ? "ok" : "FAILED", what);
    if (!ok)
        g_failed = 1;
}

u64 ext_find(const char *lib, const char *nid) { return 0; }
struct module *mod_by_addr(u64 addr) { return NULL; }
/* hle_jbc.c is not part of this build (test_jbc.py runs it): no virtual kernel */
void jbc_scan_text(struct module *m, const u8 *text, u64 size) {}
bool jbc_kexec(u64 fn, u64 args, u64 sp, u64 *ret) { return false; }
s64 jbc_raw_open(const char *path, s64 fd) { return fd; }

static int g_errno;
static int *errno_(void) { return &g_errno; }
static u64 fake_getpid(void) { return 4242; }
static u64 g_open_path;
static s64 fake_open(u64 path, u64 flags, u64 mode)
{
    g_open_path = path;
    g_errno = 13; /* EACCES */
    return -1;
}
static void *host_real_(const char *name)
{
    if (strcmp(name, "getpid") == 0)
        return fake_getpid;
    if (strcmp(name, "open") == 0)
        return fake_open;
    return NULL; /* no "mknod" here: ENOSYS */
}

static u8 *g_exec;
static u32 g_used;
static u64 stub_alloc_(const u8 *code, u32 len)
{
    u8 *p = g_exec + 0x3000 + g_used;
    memcpy(p, code, len);
    g_used += 32;
    return (u64)p;
}

void rawsys_patch_text(struct module *m, u8 *text, u64 size);

static struct ghost_api g_api;

int main(void)
{
    K.errno_fn = errno_;
    g_api = (struct ghost_api){.version = GHOST_API_VERSION, .size = sizeof(struct ghost_api), .k = &K,
                               .logf = hlog, .host_real = host_real_, .compute_nid = compute_nid,
                               .stub_alloc_code = stub_alloc_};
    G = &g_api;
    g_exec = mmap(NULL, 0x4000, 7, 0x1002 /* MAP_ANON | MAP_PRIVATE */ | 0x800 /* MAP_JIT */, -1, 0);
    if (g_exec == (u8 *)-1) {
        hlog("SKIP no executable memory here");
        return 0;
    }
    /* each function returns rax | CF << 32 */
    static const u8 tail[] = {0x0F, 0x92, 0xC2, 0x0F, 0xB6, 0xD2, 0x48, 0xC1, 0xE2, 0x20, 0x48, 0x09, 0xD0, 0xC3};
    static const u8 fn_getpid[] = {0xB8, 20, 0, 0, 0, 0x49, 0x89, 0xCA, 0x0F, 0x05};                 /* mov eax */
    static const u8 fn_open[] = {0x48, 0xC7, 0xC0, 5, 0, 0, 0, 0x49, 0x89, 0xCA, 0x0F, 0x05};         /* mov rax */
    static const u8 fn_kexec[] = {0x48, 0xC7, 0xC0, 11, 0, 0, 0, 0x49, 0x89, 0xCA, 0x0F, 0x05};
    static const u8 fn_mknod[] = {0x48, 0xC7, 0xC0, 14, 0, 0, 0, 0x49, 0x89, 0xCA, 0x0F, 0x05};
    static const u8 fn_indirect[] = {0x48, 0xC7, 0xC0, 0, 0, 0, 0, 0x49, 0x89, 0xCA, 0x0F, 0x05}; /* syscall(rdi, ...) */
    /* r8, r9, rsi kept across the call: rax = r8 + r9 + rsi afterwards */
    static const u8 fn_regs[] = {0x49, 0xC7, 0xC0, 0x11, 0x11, 0, 0, 0x49, 0xC7, 0xC1, 0x22, 0x22, 0, 0,
                                 0x48, 0xC7, 0xC6, 0x33, 0x33, 0, 0, 0xB8, 20, 0, 0, 0, 0x49, 0x89, 0xCA,
                                 0x0F, 0x05, 0x4B, 0x8D, 0x04, 0x08, 0x48, 0x01, 0xF0, 0xC3};
    const u8 *fns[] = {fn_getpid, fn_open, fn_kexec, fn_mknod, fn_indirect};
    const u32 lens[] = {sizeof(fn_getpid), sizeof(fn_open), sizeof(fn_kexec), sizeof(fn_mknod), sizeof(fn_indirect)};
    u64 at[6];
    u32 off = 0;
    for (u32 i = 0; i < 5; i++) {
        at[i] = off;
        memcpy(g_exec + off, fns[i], lens[i]);
        memcpy(g_exec + off + lens[i], tail, sizeof(tail));
        off += 64;
    }
    at[5] = off;
    memcpy(g_exec + off, fn_regs, sizeof(fn_regs));
    /* a decoy: 49 89 ca 0f 05 with no system call number before it stays */
    static const u8 decoy[] = {0x90, 0x90, 0x49, 0x89, 0xCA, 0x0F, 0x05, 0xC3};
    memcpy(g_exec + 0x1000, decoy, sizeof(decoy));
    struct module m;
    memset(&m, 0, sizeof(m));
    m.base = (u64)g_exec;
    m.name = "test.prx";
    rawsys_patch_text(&m, g_exec, 0x2000);
    expect(g_exec[at[0] + 5] == 0xE8 && g_exec[at[1] + 7] == 0xE8 && g_exec[at[5] + 26] == 0xE8,
           "the sites became calls");
    expect(memcmp(g_exec + 0x1000, decoy, sizeof(decoy)) == 0, "a site with no number before it is left alone");

    typedef u64 (*f0)(u64, u64, u64, u64);
    u64 r = ((f0)(g_exec + at[0]))(0, 0, 0, 0);
    expect(r == 4242, "getpid (20) through libkernel's function: 4242, no carry");
    r = ((f0)(g_exec + at[1]))((u64) "/dev/sbl_srv", 2, 0, 0);
    expect(r == ((1ull << 32) | 13) && g_open_path && strcmp((const char *)g_open_path, "/dev/sbl_srv") == 0,
           "open (5) refused: EACCES with the carry flag, the path passed on");
    r = ((f0)(g_exec + at[2]))(0, 0, 0, 0);
    expect(r == ((1ull << 32) | 78), "kexec (11): ENOSYS with the carry flag");
    r = ((f0)(g_exec + at[3]))((u64) "/dev/lvdctl", 0x2000, 0, 0);
    expect(r == ((1ull << 32) | 78), "mknod (14) with no libkernel function here: ENOSYS");
    r = ((f0)(g_exec + at[4]))(20, 0, 0, 0);
    expect(r == 4242, "syscall(20) through number 0");
    r = ((f0)(g_exec + at[5]))(0, 0, 0, 0);
    expect(r == 0x6666, "rsi, r8 and r9 kept across the call");
    return g_failed;
}
