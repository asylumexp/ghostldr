/*
 * hle_jbc.c and hle_rawsys.c, built for this machine as x86-64 (test_jbc.py):
 * a model of a jailbreak library (libjbc's protocol: kexec of a trampoline
 * with {payload, args} on the caller's stack; curthread and kernel-memcpy
 * payloads; the struct offsets it uses; its raw open/close trick that moves
 * a directory's vnode reference) runs its jailbreak against ghostldr's
 * virtual kernel through patched raw system call sites, then the virtual
 * devices, /user files and sealed-key files a jailbroken title sees are
 * checked, and the jailbreak undone.  The model is written for this test
 * from the protocol; it is not libjbc's code.
 *
 *     jbc_host ROOT        (ROOT: an empty directory standing in for SWRR's file system)
 *
 * Prints "ok ..." / "FAILED ..." lines; exits 0 when every check passed.
 */
#include "ghost_api.h"
#include "savedata_hooks.h"

long write(int fd, const void *buf, unsigned long n);
long read(int fd, void *buf, unsigned long n);
int open(const char *path, int flags, ...);
int close(int fd);
int mkdir(const char *path, unsigned short mode);
int access(const char *path, int mode);
int vsnprintf(char *buf, unsigned long size, const char *fmt, __builtin_va_list ap);
int snprintf(char *buf, unsigned long size, const char *fmt, ...);
void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off);
void *calloc(unsigned long n, unsigned long size);
int *__error(void);
void compute_nid(const char *name, char out[12]);

struct kfuncs K;
const struct ghost_api *G;
static int g_failed;
static char g_root[256];

static void hlog(const char *f, ...)
{
    char line[700];
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

/* ---- the harness: families, the fake core and SWRR ---- */
static const struct ext_family *g_fam[4];
static u32 g_nfam;
void harness_family(const struct ext_family *f) { g_fam[g_nfam++] = f; }

static void *fam_fn(const char *name)
{
    for (u32 i = 0; i < g_nfam; i++)
        for (u32 k = 0; k < g_fam[i]->nhle; k++)
            if (strcmp(g_fam[i]->hle[k].name, name) == 0)
                return g_fam[i]->hle[k].fn;
    return NULL;
}

static const char *(*g_hook)(const char *path, char *buf, size_t size);
static int remap_hook_add_(const char *(*fn)(const char *, char *, size_t))
{
    g_hook = fn;
    return 0;
}
static const char *remap(const char *path, char *buf, size_t size)
{
    const char *r = g_hook ? g_hook(path, buf, size) : NULL;
    return r ? r : path;
}

static u8 *g_exec;
static struct module g_mod;
struct module *mod_by_addr(u64 addr) { return addr >= (u64)g_exec && addr < (u64)g_exec + 0x4000 ? &g_mod : NULL; }
const char *title_ident(void) { return "APOL00004"; }
/* hle_fsvsh.c: the on-screen notice for a new save image (weak in hle_jbc.c; Mach-O wants it defined) */
void fsvsh_new_save_refused(void) { logf("fsvsh: a new save image: refused (notice)"); }
bool savedata_is_foreign(const char *tid) { return strcmp(tid, "CUSA03492") != 0 && strcmp(tid, "APOL00004") != 0; }
static u32 g_searches;
s32 savedata_foreign_search(s32 user, const char *tid, char (*dirs)[32], struct sd_param *params, u64 *blocks,
                            u32 max, u32 *hits)
{
    g_searches++;
    *hits = 0;
    if (user == 0x10000001 && strcmp(tid, "CUSA07992") == 0 && max >= 2) {
        memset(dirs, 0, 64);
        strcpy(dirs[0], "SCUS-97429");
        strcpy(dirs[1], "GHOST0001");
        *hits = 2;
    }
    return 0;
}

static int g_errno;
static int *errno_(void) { return &g_errno; }
static int fake_getpid(void) { return 4242; }

/* SWRR's file system: paths under ROOT; SCE errors */
static void under(char *out, const char *path) { snprintf(out, 512, "%s%s", g_root, path); }
static int k_open_(const char *path, int flags, int mode)
{
    char p[512];
    under(p, path);
    int mf = (flags & 3) | (flags & 0x200 ? 0x200 : 0) | (flags & 0x400 ? 0x400 : 0); /* macOS: same bits */
    int fd = open(p, mf, mode);
    return fd >= 0 ? fd : (int)(0x80020000u | (u32)*__error());
}
static ssize_t k_write_(int fd, const void *b, size_t n) { return write(fd, b, n); }
static int k_close_(int fd) { return close(fd); }
static int k_stat_(const char *path, void *st)
{
    char p[512];
    under(p, path);
    if (access(p, 0) != 0)
        return (int)0x80020002;
    memset(st, 0, 0x78);
    *(u16 *)((u8 *)st + 8) = 0100644;
    return 0;
}
static s32 k_mkdir_(const char *path, int mode)
{
    char p[512];
    under(p, path);
    return mkdir(p, (unsigned short)mode) == 0 ? 0 : (s32)(0x80020000u | (u32)*__error());
}
static u32 fake_getuid(void) { return 1000; }
static s64 fake_tod(s64 *tv, void *tz)
{
    static s64 t = 1000;
    tv[0] = t++, tv[1] = 0;
    return 0;
}
static s64 fake_setuid(u64 uid)
{
    g_errno = 1;
    return -1;
}

static void *host_real_(const char *name)
{
    if (strcmp(name, "getuid") == 0 || strcmp(name, "getgid") == 0)
        return fake_getuid;
    if (strcmp(name, "sceKernelMkdir") == 0)
        return k_mkdir_;
    if (strcmp(name, "gettimeofday") == 0)
        return fake_tod;
    if (strcmp(name, "setuid") == 0)
        return fake_setuid;
    return NULL;
}

/* the title's open and close as hle_file.c has them: the virtual devices first; SWRR refuses "/" */
bool jbc_open(const char *path, int flags, int mode, s64 *fd);
void jbc_close(s32 fd);
bool jbc_stat(const char *path, void *st, s32 *rc);
bool jbc_ioctl(s32 fd, u64 req, u64 arg, s64 *rc);
static u32 g_root_opens;
static s64 title_open(const char *path, u64 flags, u64 mode)
{
    s64 fd;
    if (jbc_open(path, (int)flags, (int)mode, &fd))
        return fd < 0 ? (g_errno = (int)(fd & 0xFFFF), -1) : fd;
    if (strcmp(path, "/") == 0) {
        g_root_opens++;
        g_errno = 13; /* EACCES */
        return -1;
    }
    char buf[512];
    int r = k_open_(remap(path, buf, sizeof(buf)), (int)flags, (int)mode);
    return r < 0 ? (g_errno = r & 0xFFFF, -1) : r;
}
static s64 title_close(u64 fd)
{
    jbc_close((s32)fd);
    return close((int)fd);
}
static char g_nid_open[12], g_nid_close[12];
u64 ext_find(const char *lib, const char *nid)
{
    if (strcmp(nid, g_nid_open) == 0)
        return (u64)title_open;
    if (strcmp(nid, g_nid_close) == 0)
        return (u64)title_close;
    return 0;
}

static u32 g_used;
static u64 stub_alloc_(const u8 *code, u32 len)
{
    u8 *p = g_exec + 0x3000 + g_used;
    memcpy(p, code, len);
    g_used += 32;
    return (u64)p;
}
static void *page_alloc_(size_t n) { return calloc(1, n); }
static void lock_(volatile int *l) { *l = 1; }
static void unlock_(volatile int *l) { *l = 0; }

void rawsys_patch_text(struct module *m, u8 *text, u64 size);

/* ---- a model of a jailbreak library ---- */
typedef u64 (*sys4)(u64, u64, u64, u64);
static sys4 sys_kexec, sys_open, sys_close;
static u64 p_td, p_cpy, p_tramp, p_other;

static u64 kexec_call(u64 payload, u64 a1, u64 a2, u64 a3)
{
    volatile u64 args[7] = {payload, a1, a2, a3, 0, 0, 0};
    sys_kexec(p_tramp, (u64)args, 0, 0);
    return args[0];
}

static bool kaddr(u64 a) { return a >= 0xFFFF800000000000ULL && a < 0xFFFFFFFF00000000ULL; }
static int kcopy(u64 dst, u64 src, u64 n)
{
    if (!kaddr(dst) && !kaddr(src))
        return -1;
    kexec_call(p_cpy, dst, src, n);
    return 0;
}
static u64 kr64(u64 a)
{
    u64 v = 0;
    return kaddr(a) && kcopy((u64)&v, a, 8) == 0 ? v : ~0ULL;
}
static u32 kr32(u64 a)
{
    u32 v = 0;
    return kaddr(a) && kcopy((u64)&v, a, 4) == 0 ? v : ~0u;
}
static int kw(u64 a, const void *p, u64 n) { return kaddr(a) ? kcopy(a, (u64)p, n) : -1; }

struct cred {
    u32 uid, ruid, svuid, rgid, svgid, pad;
    u64 prison, cdir, rdir, jdir, authid, caps0, caps1;
};

static u64 td(void) { return kexec_call(p_td, 0, 0, 0); }
static u64 proc_of_td(void) { return kr64(td() + 8); }

static int find_init(u64 *prison0, u64 *rootvn)
{
    for (int tries = 0; tries < 100; tries++) {
        u64 p = proc_of_td();
        for (int hops = 0; hops < 1000; hops++) {
            u32 pid = kr32(p + 0xB0);
            if (pid == ~0u)
                break;
            if (pid == 1) {
                *prison0 = kr64(kr64(p + 0x40) + 0x30);
                *rootvn = kr64(kr64(p + 0x48) + 0x18);
                return 0;
            }
            u64 next = kr64(p);
            if (kr64(next + 8) != p)
                break;
            p = next;
        }
    }
    return -1; /* the real library walks forever */
}

static int get_cred(struct cred *c)
{
    u64 p = proc_of_td(), cr = kr64(p + 0x40), fd = kr64(p + 0x48);
    if (!kaddr(cr) || !kaddr(fd))
        return -1;
    kcopy((u64)&c->uid, cr + 4, 12);
    kcopy((u64)&c->rgid, cr + 0x14, 8);
    kcopy((u64)&c->prison, cr + 0x30, 8);
    kcopy((u64)&c->cdir, fd + 0x10, 24);
    kcopy((u64)&c->authid, cr + 0x58, 24);
    return 0;
}

static int put_cred(const struct cred *c)
{
    u64 p = proc_of_td(), cr = kr64(p + 0x40), fd = kr64(p + 0x48);
    return kw(cr + 4, &c->uid, 12) | kw(cr + 0x14, &c->rgid, 8) | kw(cr + 0x30, &c->prison, 8) |
           kw(fd + 0x10, &c->cdir, 24) | kw(cr + 0x58, &c->authid, 24);
}

static int jailbreak_cred(struct cred *c)
{
    u64 p0, root;
    if (find_init(&p0, &root))
        return -1;
    c->uid = c->ruid = c->svuid = c->rgid = c->svgid = 0;
    c->prison = p0;
    c->cdir = c->rdir = c->jdir = root;
    c->authid = 0x3801000000000013ULL;
    c->caps0 = c->caps1 = ~0ULL;
    return 0;
}

/* an fd holding vnode vn: opened as "/" under a root of vn */
static s64 hold_vnode(const struct cred *cur, u64 vn)
{
    struct cred t = *cur;
    if (jailbreak_cred(&t))
        return -1;
    t.cdir = t.rdir = t.jdir = vn;
    put_cred(&t);
    s64 fd = (s64)sys_open((u64) "/", 0, 0, 0); /* the raw call's rax: an fd, or an errno */
    put_cred(cur);
    return fd;
}

static int set_cred(const struct cred *want)
{
    struct cred cur, t = *want;
    if (get_cred(&cur))
        return -1;
    s64 fds[3] = {hold_vnode(&cur, want->cdir), hold_vnode(&cur, want->rdir), hold_vnode(&cur, want->jdir)};
    t.cdir = cur.cdir, t.rdir = cur.rdir, t.jdir = cur.jdir;
    u32 ref = kr32(t.prison + 0x14) + 1;
    kw(t.prison + 0x14, &ref, 4);
    if (put_cred(&t))
        return -1;
    ref = kr32(cur.prison + 0x14) - 1;
    kw(cur.prison + 0x14, &ref, 4);
    u64 fdp = kr64(proc_of_td() + 0x48), ofiles = kr64(fdp);
    for (int i = 0; i < 3; i++) {
        if (fds[i] < 0)
            continue;
        u64 fp = kr64(ofiles + 8 * (u64)fds[i]), a = kr64(fp), b = kr64(fdp + 0x10 + 8 * (u64)i);
        if (fp == ~0ULL || a == ~0ULL)
            return -1;
        kw(fp, &b, 8);
        kw(fdp + 0x10 + 8 * (u64)i, &a, 8);
        sys_close((u64)fds[i], 0, 0, 0);
    }
    return 0;
}

/* ---- the test ---- */
static struct ghost_api g_api;

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    strlcpy(g_root, argv[1], sizeof(g_root));
    K.errno_fn = errno_;
    K.getpid = fake_getpid;
    K.open = k_open_;
    K.write = k_write_;
    K.close = k_close_;
    K.stat = k_stat_;
    compute_nid("open", g_nid_open);
    compute_nid("close", g_nid_close);
    g_api = (struct ghost_api){.version = GHOST_API_VERSION, .size = sizeof(struct ghost_api), .k = &K,
                               .logf = hlog, .host_real = host_real_, .compute_nid = compute_nid,
                               .stub_alloc_code = stub_alloc_, .page_alloc = page_alloc_, .spin_lock = lock_,
                               .spin_unlock = unlock_, .remap_hook_add = remap_hook_add_};
    G = &g_api;
    for (u32 i = 0; i < g_nfam; i++)
        if (g_fam[i]->init)
            g_fam[i]->init();
    char d[512];
    under(d, "/temp0");
    mkdir(d, 0777);

    g_exec = mmap(NULL, 0x4000, 7, 0x1002 /* MAP_ANON | MAP_PRIVATE */ | 0x800 /* MAP_JIT */, -1, 0);
    if (g_exec == (u8 *)-1) {
        hlog("SKIP no executable memory here");
        return 0;
    }
    static const u8 fn_kexec[] = {0x48, 0xC7, 0xC0, 11, 0, 0, 0, 0x49, 0x89, 0xCA, 0x0F, 0x05, 0xC3};
    static const u8 fn_open[] = {0x48, 0xC7, 0xC0, 5, 0, 0, 0, 0x49, 0x89, 0xCA, 0x0F, 0x05, 0xC3};
    static const u8 fn_close[] = {0x48, 0xC7, 0xC0, 6, 0, 0, 0, 0x49, 0x89, 0xCA, 0x0F, 0x05, 0xC3};
    static const u8 pl_td[] = {0x65, 0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0, 0xC3};
    static const u8 pl_cpy[] = {0x48, 0x89, 0xD1, 0xF3, 0xA4, 0xC3};
    static const u8 pl_other[] = {0x0F, 0x20, 0xC0, 0xC3}; /* mov rax, cr0; ret: a payload nobody knows */
    memcpy(g_exec + 0x000, fn_kexec, sizeof(fn_kexec));
    memcpy(g_exec + 0x040, fn_open, sizeof(fn_open));
    memcpy(g_exec + 0x080, fn_close, sizeof(fn_close));
    memcpy(g_exec + 0x0C0, pl_td, sizeof(pl_td));
    memcpy(g_exec + 0x100, pl_cpy, sizeof(pl_cpy));
    memcpy(g_exec + 0x140, pl_other, sizeof(pl_other));
    g_exec[0x180] = 0xC3; /* the trampoline: never run here */
    memset(&g_mod, 0, sizeof(g_mod));
    g_mod.base = (u64)g_exec;
    g_mod.name = "eboot.bin";
    rawsys_patch_text(&g_mod, g_exec, 0x2000);
    sys_kexec = (sys4)g_exec, sys_open = (sys4)(g_exec + 0x40), sys_close = (sys4)(g_exec + 0x80);
    p_td = (u64)g_exec + 0xC0, p_cpy = (u64)g_exec + 0x100, p_other = (u64)g_exec + 0x140;
    p_tramp = (u64)g_exec + 0x180;
    expect(g_exec[0x7] == 0xE8 && g_exec[0x47] == 0xE8, "the kexec and open sites became calls");

    char buf[512];
    expect(strcmp(remap("/user/.jailbreak", buf, sizeof(buf)), "/user/.jailbreak") == 0,
           "/user/.jailbreak before any jailbreak: the system's (refused)");
    s32 rc = 0;
    expect(!jbc_stat("/dev/sbl_srv", NULL, &rc), "no virtual devices before a jailbreak");

    /* a kexec nobody knows, and libjbc's protocol off the stack: ENOSYS */
    volatile u64 args[7] = {p_other, 0, 0, 0, 0, 0, 0};
    u64 r = sys_kexec(p_tramp, (u64)args, 0, 0);
    expect(r == 78 && args[0] == p_other, "kexec of an unknown payload: ENOSYS, nothing run");
    static u64 g_args[7];
    g_args[0] = p_td;
    r = sys_kexec(p_tramp, (u64)g_args, 0, 0);
    expect(r == 78 && g_args[0] == p_td, "kexec with arguments that are not on the caller's stack: ENOSYS");

    u64 t = td();
    expect(kaddr(t), "curthread: a kernel address (the virtual kernel's thread)");
    struct cred old, c;
    memset(&old, 0, sizeof(old));
    expect(get_cred(&old) == 0 && old.uid == 1000 && old.ruid == 1000 && old.rgid == 1000 && kaddr(old.prison) &&
               kaddr(old.rdir) && old.cdir == old.rdir,
           "jbc_get_cred: the app's ids, its prison and sandbox root");
    u64 p0 = 0, root = 0;
    expect(find_init(&p0, &root) == 0 && kaddr(p0) && kaddr(root) && p0 != old.prison && root != old.rdir,
           "the process list leads to init (pid 1): prison0 and the root vnode, not the app's");
    u32 ref0 = kr32(p0 + 0x14);
    c = old;
    expect(jailbreak_cred(&c) == 0 && set_cred(&c) == 0, "jbc_jailbreak_cred + jbc_set_cred finish");
    expect(g_root_opens == 3, "the three raw opens of \"/\" were SWRR's refusals, stood in for by ghostldr");
    struct cred now;
    expect(get_cred(&now) == 0 && now.uid == 0 && now.svuid == 0 && now.prison == p0 && now.cdir == root &&
               now.rdir == root && now.jdir == root && now.authid == 0x3801000000000013ULL && now.caps0 == ~0ULL,
           "the virtual credentials are jailbroken: uid 0, prison0, the system's root everywhere");
    expect(kr32(p0 + 0x14) == ref0 + 1 && kr32(old.prison + 0x14) == 0, "prison references moved");

    const char *jb = remap("/user/.jailbreak", buf, sizeof(buf));
    expect(strcmp(jb, "/temp0/ghostldr/APOL00004/jbc/user/.jailbreak") == 0,
           "/user/.jailbreak once jailbroken: a file of the title's own");
    int f = k_open_(jb, 0x601, 0644);
    expect(f >= 0, "... which can be written");
    if (f >= 0)
        close(f);
    expect(strcmp(remap("/user/home/10000001/x", buf, sizeof(buf)), "/user/home/10000001/x") == 0,
           "deeper /user paths stay the system's");

    /* init_cred + setuid(0) */
    s64 (*setuid_)(u32) = fam_fn("setuid");
    struct cred ic;
    get_cred(&ic);
    ic.caps0 |= 0x4000000000000000ULL;
    ic.authid = 0x3801000000000013ULL;
    expect(set_cred(&ic) == 0 && setuid_ && setuid_(0) == 0, "init_cred's jbc_set_cred and setuid(0) succeed");

    /* devices */
    s32 (*mknod_)(const char *, u32, u32) = fam_fn("mknod");
    u8 st[0x78];
    expect(jbc_stat("/dev/pfsctldev", st, &rc) && rc == 0 && *(u16 *)(st + 8) == 020666,
           "stat /dev/pfsctldev: a character device");
    expect(mknod_ && mknod_("/dev/pfsctldev", 020777, 0) == 0 && mknod_("/dev/lvdctl", 020777, 0) == 0 &&
               mknod_("/dev/sbl_srv", 020777, 0) == 0,
           "mknod of /dev/pfsctldev, /dev/lvdctl, /dev/sbl_srv");
    s64 sbl = -1;
    expect(jbc_open("/dev/sbl_srv", 2, 0, &sbl) && sbl >= 0, "open /dev/sbl_srv");
    u8 key[0x84 + 0x10], dec[0x90];
    memset(key, 0xEE, sizeof(key));
    s64 irc = -2;
    expect(jbc_ioctl((s32)sbl, 0x40845303, (u64)key, &irc) && irc == 0 && memcmp(key, "pfsSKKey", 8) == 0 &&
               key[8] == 10 && key[9] == 0 && key[0x60] == 0xEE,
           "sbl_srv 0x40845303: a sealed key (magic, key set 10), 0x60 bytes");
    memset(dec, 0, sizeof(dec));
    memcpy(dec, key, 0x60);
    expect(jbc_ioctl((s32)sbl, 0xC0845302, (u64)dec, &irc) && irc == 0 && memcmp(dec + 0x60, "ghostldr", 8) == 0,
           "sbl_srv 0xc0845302: unsealed to the marker after the sealed key");
    dec[0] = 'x';
    expect(jbc_ioctl((s32)sbl, 0xC0845302, (u64)dec, &irc) && irc == -1 && g_errno == 22,
           "... a sealed key without its magic: EINVAL");
    expect(!jbc_ioctl(1, 0x40845303, (u64)key, &irc), "ioctl on another fd: not the device's");
    title_close((u64)sbl);
    expect(!jbc_ioctl((s32)sbl, 0x40845303, (u64)key, &irc), "the device fd closed");

    /* sealed key files */
    s64 kfd = -1;
    expect(jbc_open("/user/home/10000001/savedata/CUSA07992/SCUS-97429.bin", 0, 0, &kfd) && kfd >= 0,
           "a listed save's sealed key file opens read-only");
    u8 k2[0x60];
    memset(k2, 0, sizeof(k2));
    expect(kfd >= 0 && read((int)kfd, k2, sizeof(k2)) == 0x60 && memcmp(k2, "pfsSKKey", 8) == 0,
           "... and reads as a sealed key");
    if (kfd >= 0)
        close((int)kfd);
    expect(jbc_stat("/user/home/10000001/savedata/CUSA07992/SCUS-97429.bin", st, &rc) && rc == 0,
           "... and stat/access find it");
    s64 x;
    u32 searches = g_searches;
    expect(!jbc_open("/user/home/10000001/savedata/CUSA07992/NOPE.bin", 0, 0, &x) &&
               !jbc_stat("/user/home/10000001/savedata/CUSA07992/NOPE.bin", st, &rc),
           "a save the service does not list has no key file");
    expect(g_searches == searches, "... answered from the search kept a few seconds");
    expect(!jbc_open("/user/home/10000001/savedata/CUSA07992/SCUS-97429.bin", 0x601, 0, &x),
           "writing a key file is the system's (refused there)");
    expect(!jbc_open("/user/home/10000001/savedata/CUSA03492/SLES-50000.bin", 0, 0, &x),
           "the host title's saves have none");

    /* undone */
    expect(set_cred(&old) == 0, "jbc_set_cred of the saved credentials (Apollo's exit)");
    expect(get_cred(&now) == 0 && now.uid == 1000 && now.prison == old.prison && now.rdir == old.rdir,
           "the virtual credentials are the app's again");
    expect(strcmp(remap("/user/.jailbreak", buf, sizeof(buf)), "/user/.jailbreak") == 0 &&
               !jbc_stat("/dev/sbl_srv", NULL, &rc) && !jbc_open("/dev/sbl_srv", 2, 0, &x),
           "/user and the devices are the system's again");
    expect(setuid_(0) == -1, "setuid(0) is SWRR's again (refused)");
    return g_failed;
}
