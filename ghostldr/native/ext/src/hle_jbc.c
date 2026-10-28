/*
 * A virtual kernel for homebrew's jailbreak code (libjbc), and what a
 * jailbroken process would see where ghostldr can stand in for it.
 *
 * Homebrew made for jailbroken PS4s often links libjbc, which escapes the
 * app's sandbox by editing its own process's credentials in kernel memory:
 * jbc_get_cred, jbc_jailbreak_cred and jbc_set_cred read and write struct
 * thread, proc, ucred, filedesc, prison and file through GoldHEN/HEN's kexec
 * system call (11), which runs a function in kernel mode.  Its kexec calls
 * follow one protocol: kexec(trampoline, args), args = {payload, a1..a6} on
 * the caller's stack; the trampoline calls the payload with a1..a6 and stores
 * the result in args[0].  The payloads are assembly, the same in every build:
 *
 *     65 48 8b 04 25 00 00 00 00 c3   mov rax, gs:[0]; ret             curthread
 *     48 89 d1 f3 a4 c3               mov rcx, rdx; rep movsb; ret    a kernel memcpy
 *
 * Without kexec (ENOSYS: SWRR's kernel is not exploited) nothing is read, and
 * libjbc walks lists of garbage forever.  Here those payloads are found where
 * the loader patches a module's raw system calls (hle_rawsys.c), and a kexec
 * of one runs it against a small virtual kernel: a thread, its process, an
 * init process (pid 1) next in the process list, their credentials and file
 * descriptor tables, two prisons and some vnodes, at kernel addresses backed
 * by ghostldr's memory.  libjbc's calls then finish and do what they do on a
 * jailbroken console, to those objects.  Nothing in the real kernel is read
 * or changed: SWRR keeps its sandbox and its rights.
 *
 * Once the virtual credentials have the system's root directory (a
 * jbc_set_cred of jailbroken credentials), the title sees, where ghostldr can
 * stand in for them:
 *   - "/user/<file>": a directory of the title's own (Apollo's success test
 *     writes and removes /user/.jailbreak);
 *   - /dev/sbl_srv, /dev/pfsctldev, /dev/lvdctl (stat, access, mknod, open):
 *     virtual devices.  sbl_srv's two save-key ioctls answer for ghostldr's
 *     sceFsMountSaveData (hle_fsvsh.c), which mounts a save through the save
 *     service and never uses a key: 0x40845303 makes a sealed key (its magic
 *     and version, nothing secret), 0xc0845302 unseals any sealed key to a
 *     fixed marker;
 *   - a save's sealed key file /user/home/<user>/savedata/<TITLE>/<DIR>.bin,
 *     read-only, for each save of another title the save service lists: such
 *     a sealed key;
 *   - setuid() succeeds once the virtual credentials are root's.
 */
#include "kmod.h"
#include "savedata_hooks.h"

/* ---- the virtual kernel ---- */

#define KV_BASE ((u64)0xFFFFA0DE00000000ULL) /* in libjbc's "kernel heap" range */
#define KV_SIZE 0xC000

/* objects, as offsets from KV_BASE */
#define O_TD 0x0000
#define O_PROC 0x0100
#define O_INIT 0x0200
#define O_CRED 0x0300
#define O_CRED0 0x0400
#define O_FD 0x0500
#define O_FD0 0x0600
#define O_PRISON0 0x0700
#define O_PRISON 0x0800
#define O_ROOTVN 0x0900 /* the system's root directory */
#define O_SBOXVN 0x0A00 /* the app's sandbox root */
#define O_FILEVN 0x0B00 /* any other file */
#define O_OFILES 0x1000
#define NFILES 1024
#define O_FILES (O_OFILES + NFILES * 8)
#define FILE_SZ 0x20

/* the FreeBSD 9 / Orbis fields libjbc uses */
#define TD_PROC 0x08
#define P_LE_NEXT 0x00
#define P_LE_PREV 0x08
#define P_UCRED 0x40
#define P_FD 0x48
#define P_PID 0xB0
#define CR_UID 0x04 /* uid, ruid, svuid */
#define CR_NGROUPS 0x10
#define CR_RGID 0x14 /* rgid, svgid */
#define CR_PRISON 0x30
#define CR_AUTHID 0x58
#define CR_CAPS 0x60
#define FD_OFILES 0x00
#define FD_CDIR 0x10
#define FD_RDIR 0x18
#define FD_JDIR 0x20
#define PR_REF 0x14
#define F_DATA 0x00

#define KA(o) (KV_BASE + (u64)(o))

enum { PL_CURTHREAD = 1, PL_KCPY = 2 };

static u8 *g_kv;
static volatile int g_kv_lock;
static struct {
    u64 at;
    u32 kind;
} g_pl[16];
static u32 g_npl;
static u32 g_state; /* uid | prison0 << 32 | root << 33, as last logged */

static u64 rd64(u32 o) { return *(u64 *)(g_kv + o); }
static void wr64(u32 o, u64 v) { *(u64 *)(g_kv + o) = v; }
static u32 rd32(u32 o) { return *(u32 *)(g_kv + o); }
static void wr32(u32 o, u32 v) { *(u32 *)(g_kv + o) = v; }

static u32 real_id(const char *name)
{
    u32 (*f)(void) = host_real(name);
    return f ? f() : 0;
}

/* the objects as a fresh app process has them; under g_kv_lock */
static bool kv_init(void)
{
    if (g_kv)
        return true;
    u8 *kv = page_alloc(KV_SIZE);
    if (!kv)
        return false;
    memset(kv, 0, KV_SIZE);
    g_kv = kv;
    wr64(O_TD + TD_PROC, KA(O_PROC));
    /* the title's process, then init in the process list */
    wr64(O_PROC + P_LE_NEXT, KA(O_INIT));
    wr64(O_PROC + P_UCRED, KA(O_CRED));
    wr64(O_PROC + P_FD, KA(O_FD));
    wr32(O_PROC + P_PID, (u32)(K.getpid ? K.getpid() : 0));
    wr64(O_INIT + P_LE_PREV, KA(O_PROC + P_LE_NEXT));
    wr64(O_INIT + P_UCRED, KA(O_CRED0));
    wr64(O_INIT + P_FD, KA(O_FD0));
    wr32(O_INIT + P_PID, 1);
    /* credentials: the app's real ids in its prison; init's root in prison0 */
    u32 uid = real_id("getuid"), gid = real_id("getgid");
    for (u32 k = 0; k < 3; k++)
        wr32(O_CRED + CR_UID + 4 * k, uid);
    wr32(O_CRED + CR_NGROUPS, 1);
    wr32(O_CRED + CR_RGID, gid);
    wr32(O_CRED + CR_RGID + 4, gid);
    wr64(O_CRED + CR_PRISON, KA(O_PRISON));
    wr32(O_CRED0 + CR_NGROUPS, 1);
    wr64(O_CRED0 + CR_PRISON, KA(O_PRISON0));
    wr32(O_PRISON0 + PR_REF, 1);
    wr32(O_PRISON + PR_REF, 1);
    /* directories: the app's are its sandbox; init's the system's root */
    wr64(O_FD + FD_OFILES, KA(O_OFILES));
    wr64(O_FD + FD_CDIR, KA(O_SBOXVN));
    wr64(O_FD + FD_RDIR, KA(O_SBOXVN));
    wr64(O_FD + FD_JDIR, KA(O_SBOXVN));
    wr64(O_FD0 + FD_CDIR, KA(O_ROOTVN));
    wr64(O_FD0 + FD_RDIR, KA(O_ROOTVN));
    for (u32 i = 0; i < NFILES; i++) {
        wr64(O_OFILES + 8 * i, KA(O_FILES + FILE_SZ * i));
        wr64(O_FILES + FILE_SZ * i + F_DATA, KA(O_FILEVN));
    }
    logf("jbc: a virtual kernel for the title's jailbreak code at %lx (process %u, uid %u): nothing in the real "
         "kernel is read or changed",
         KA(0), rd32(O_PROC + P_PID), uid);
    return true;
}

/* the backing of [a, a+n) when it is all in the virtual kernel */
static u8 *kv_at(u64 a, u64 n)
{
    if (!g_kv || a < KV_BASE || a - KV_BASE > KV_SIZE || n > KV_SIZE - (a - KV_BASE))
        return NULL;
    return g_kv + (a - KV_BASE);
}

static bool user_range(u64 a, u64 n) { return a && a < (u64)0x800000000000ULL && n <= (u64)0x800000000000ULL - a; }

static bool escaped_locked(void) { return g_kv && rd64(O_FD + FD_RDIR) == KA(O_ROOTVN); }

/* log what the title's virtual credentials are, when that changed since the last time they mattered */
static void kv_note_state(void)
{
    u32 uid = rd32(O_CRED + CR_UID);
    bool p0 = rd64(O_CRED + CR_PRISON) == KA(O_PRISON0), root = escaped_locked();
    u32 st = (uid & 0x3FFFFFFF) | (u32)p0 << 30 | (u32)root << 31;
    if (st == g_state)
        return;
    g_state = st;
    logf("jbc: the title's virtual credentials: uid %u, %s, root directory %s, authid %lx%s", uid,
         p0 ? "prison0" : "its own prison", root ? "the system's" : "its sandbox", rd64(O_CRED + CR_AUTHID),
         root ? " (a jailbreak, virtual: SWRR's real rights are unchanged)" : "");
}

/* the kernel memcpy payload: dst and src each in the virtual kernel or the title's memory */
static void kcpy(u64 dst, u64 src, u64 n)
{
    static u32 said;
    if (!n)
        return;
    u8 *d = kv_at(dst, n), *s = kv_at(src, n);
    bool dk = d != NULL;
    if (!d && n <= 0x100000 && user_range(dst, n))
        d = (u8 *)dst;
    if (!s && n <= 0x100000 && user_range(src, n))
        s = (u8 *)src;
    if (d && s) {
        memmove(d, s, n);
        return;
    }
    if (d && !dk) /* a read of kernel memory outside the virtual kernel: zeros */
        memset(d, 0, n);
    if (said++ < 8)
        logf("jbc: kernel copy %lx <- %lx (%lx bytes) outside the virtual kernel: %s", dst, src, n,
             d && !dk ? "read as zeros" : "dropped");
}

/* hle_rawsys.c, as a module's text is patched: libjbc's kernel payloads in it */
void jbc_scan_text(struct module *m, const u8 *text, u64 size) HIDDEN;
void jbc_scan_text(struct module *m, const u8 *text, u64 size)
{
    static const u8 k_td[10] = {0x65, 0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0, 0xC3};
    static const u8 k_cpy[6] = {0x48, 0x89, 0xD1, 0xF3, 0xA4, 0xC3};
    u32 found = 0;
    for (u64 at = 0; at + sizeof(k_td) <= size; at++) {
        u32 kind = 0;
        if (text[at] == 0x65 && memcmp(text + at, k_td, sizeof(k_td)) == 0)
            kind = PL_CURTHREAD;
        else if (text[at] == 0x48 && text[at + 1] == 0x89 && memcmp(text + at, k_cpy, sizeof(k_cpy)) == 0)
            kind = PL_KCPY;
        if (!kind)
            continue;
        spin_lock(&g_kv_lock);
        if (g_npl < ARRAY_SIZE(g_pl))
            g_pl[g_npl].at = m->base + at, g_pl[g_npl++].kind = kind, found++;
        spin_unlock(&g_kv_lock);
    }
    if (found)
        logf("%s: %u kernel payloads of a jailbreak library (curthread / kernel memcpy, libjbc's): a kexec of them "
             "runs against ghostldr's virtual kernel",
             m->name, found);
}

/* kexec(fn, args) from title code (hle_rawsys.c), sp its stack: libjbc's
 * protocol answered (true: *ret is the system call's result), anything else
 * left alone */
bool jbc_kexec(u64 fn, u64 args, u64 sp, u64 *ret) HIDDEN;
bool jbc_kexec(u64 fn, u64 args, u64 sp, u64 *ret)
{
    /* args is on the caller's stack, just above the system call's frame */
    if ((args & 7) || args <= sp || args - sp > 0x10000 || !mod_by_addr(fn))
        return false;
    u64 *a = (u64 *)args;
    u32 kind = 0;
    spin_lock(&g_kv_lock);
    for (u32 i = 0; i < g_npl && !kind; i++)
        if (g_pl[i].at == a[0])
            kind = g_pl[i].kind;
    if (kind && !kv_init())
        kind = 0;
    if (kind == PL_CURTHREAD)
        a[0] = KA(O_TD);
    else if (kind == PL_KCPY)
        kcpy(a[1], a[2], a[3]); /* rax, and so args[0], stays the payload's address */
    spin_unlock(&g_kv_lock);
    static u32 said;
    if (kind && !(said & 1u << kind)) {
        said |= 1u << kind;
        struct module *m = mod_by_addr(fn);
        logf("jbc: kexec of %s from %s+%lx: run against the virtual kernel (the real kernel is never reached)",
             kind == PL_CURTHREAD ? "curthread" : "a kernel memcpy", m ? m->name : "?", m ? fn - m->base : fn);
    }
    *ret = 0;
    return kind != 0;
}

static void jbc_dir(char *out, size_t size, const char *sub);

/* a raw open (system call 5) by title code: fd is what it returned (an error
 * < 0).  In the virtual kernel, the file of fd is the vnode it opened: "/" the
 * root directory of the virtual credentials, "." their working directory.
 * libjbc's jbc_set_cred opens "/" under a temporary root to hold the new
 * directory's vnode; SWRR may refuse "/", so the fd is then one of ghostldr's. */
s64 jbc_raw_open(const char *path, s64 fd) HIDDEN;
s64 jbc_raw_open(const char *path, s64 fd)
{
    if (!g_kv || !path)
        return fd;
    bool root = path[0] == '/' && !path[1], cwd = path[0] == '.' && !path[1];
    if (fd < 0 && (root || cwd)) {
        /* The library takes the raw call's rax as an fd, an errno too, and closes it later: never an errno
         * here.  A directory of ghostldr's, or else an fd nobody has (its later steps then fail, and its close
         * is EBADF). */
        char p[96];
        jbc_dir(p, sizeof(p), "");
        s32 alt = K.open ? K.open(p, 0, 0) : -1;
        static u32 said;
        if (said++ < 4)
            logf("jbc: raw open(\"%s\") refused (%ld): %s stands in (%d)", path, fd, p, alt);
        fd = alt >= 0 ? alt : 0x7FFF;
    }
    if (fd >= 0 && fd < NFILES) {
        spin_lock(&g_kv_lock);
        wr64(O_FILES + FILE_SZ * (u32)fd + F_DATA,
             root ? rd64(O_FD + FD_RDIR) : cwd ? rd64(O_FD + FD_CDIR) : KA(O_FILEVN));
        spin_unlock(&g_kv_lock);
    }
    return fd;
}

/* ---- what the virtual credentials let the title see ---- */

/* whether the virtual credentials have the system's root (what the title sees is logged when it changed) */
static bool escaped(void)
{
    if (!g_kv)
        return false;
    spin_lock(&g_kv_lock);
    bool e = escaped_locked();
    kv_note_state();
    spin_unlock(&g_kv_lock);
    return e;
}

/* "/temp0/ghostldr/<title>/jbc<sub>", the directories made */
static void jbc_dir(char *out, size_t size, const char *sub)
{
    static volatile bool made;
    if (!made) {
        s32 (*mk)(const char *, int) = host_real("sceKernelMkdir");
        char d[96];
        fmt(d, sizeof(d), "/temp0/ghostldr");
        if (mk)
            mk(d, 0777);
        fmt(d, sizeof(d), "/temp0/ghostldr/%s", title_ident());
        if (mk)
            mk(d, 0777);
        fmt(d, sizeof(d), "/temp0/ghostldr/%s/jbc", title_ident());
        if (mk)
            mk(d, 0777);
        fmt(d, sizeof(d), "/temp0/ghostldr/%s/jbc/user", title_ident());
        if (mk)
            mk(d, 0777);
        made = true;
    }
    fmt(out, size, "/temp0/ghostldr/%s/jbc%s", title_ident(), sub);
}

/* "/user/<name>" (no deeper) -> the title's own directory, once escaped */
static const char *jbc_remap(const char *path, char *buf, size_t size)
{
    if (!g_kv || strncmp(path, "/user/", 6) != 0 || !path[6] || strrchr_c(path + 6, '/') || !escaped())
        return NULL;
    char sub[160];
    fmt(sub, sizeof(sub), "/user/%s", path + 6);
    jbc_dir(buf, size, sub);
    static u32 said;
    if (said++ < 4)
        logf("jbc: %s is %s (the system's /user, as a jailbroken process would write it)", path, buf);
    return buf;
}

/* the virtual devices */
static const char *const k_dev[] = {"sbl_srv", "pfsctldev", "lvdctl"};
#define DEV_SBL 0

static s32 dev_of(const char *path)
{
    if (!path || strncmp(path, "/dev/", 5) != 0)
        return -1;
    for (u32 i = 0; i < ARRAY_SIZE(k_dev); i++)
        if (strcmp(path + 5, k_dev[i]) == 0)
            return (s32)i;
    return -1;
}

static struct {
    s32 fd;
    s32 dev;
} g_devfd[16];
static volatile int g_devfd_lock;

static s32 dev_fd(s32 fd)
{
    s32 dev = -1;
    spin_lock(&g_devfd_lock);
    for (u32 i = 0; i < ARRAY_SIZE(g_devfd) && dev < 0; i++)
        if (g_devfd[i].dev > 0 && g_devfd[i].fd == fd)
            dev = g_devfd[i].dev - 1;
    spin_unlock(&g_devfd_lock);
    return dev;
}

/* "/user/home/<8 hex>/savedata/<TITLE>/<DIR>.bin": user, tid, dir */
static bool key_path(const char *p, s32 *user, char tid[16], char dir[32])
{
    static const char pre[] = "/user/home/";
    if (!p || strncmp(p, pre, sizeof(pre) - 1) != 0)
        return false;
    p += sizeof(pre) - 1;
    u32 u = 0;
    for (int k = 0; k < 8; k++, p++) {
        char c = *p;
        u32 d = c >= '0' && c <= '9' ? (u32)(c - '0') : c >= 'a' && c <= 'f' ? (u32)(c - 'a' + 10) : 99;
        if (d > 15)
            return false;
        u = u << 4 | d;
    }
    if (strncmp(p, "/savedata/", 10) != 0)
        return false;
    p += 10;
    for (int k = 0; k < 9; k++)
        if (k < 4 ? (p[k] < 'A' || p[k] > 'Z') : (p[k] < '0' || p[k] > '9'))
            return false;
    if (p[9] != '/')
        return false;
    const char *name = p + 10;
    size_t n = strlen(name);
    if (n < 5 || n - 4 >= 32 || strcmp(name + n - 4, ".bin") != 0 || strrchr_c(name, '/'))
        return false;
    memcpy(tid, p, 9);
    tid[9] = 0;
    memcpy(dir, name, n - 4);
    dir[n - 4] = 0;
    *user = (s32)u;
    return true;
}

/* a sealed key as the key service makes one: magic, key set version, then
 * (here) zeros where the IV, the encrypted key and its MAC are */
#define SEALED_LEN 0x60
#define KEYSET 10
static void sealed_key(u8 k[SEALED_LEN])
{
    memset(k, 0, SEALED_LEN);
    memcpy(k, "pfsSKKey", 8);
    k[8] = KEYSET;
}

/* whether tid's save dir of user is one the save service lists (answers kept 10 s) */
#define MAXD 512
static bool save_listed(s32 user, const char *tid, const char *dir)
{
    static struct {
        s32 user;
        char tid[16];
        s64 at;
        u32 n;
        char (*dirs)[32];
    } c;
    static volatile int lock;
    s64 (*tod)(void *, void *) = host_real("gettimeofday");
    s64 tv[2] = {0, 0};
    if (tod)
        tod(tv, NULL);
    bool hit = false;
    spin_lock(&lock);
    if (!c.dirs)
        c.dirs = page_alloc(MAXD * 32);
    if (c.dirs && (c.user != user || strcmp(c.tid, tid) != 0 || tv[0] - c.at > 10 || !c.at)) {
        u32 hits = 0;
        s32 rc = savedata_foreign_search(user, tid, c.dirs, NULL, NULL, MAXD, &hits);
        c.user = user, c.n = rc == 0 ? hits : 0, c.at = tv[0];
        strlcpy(c.tid, tid, sizeof(c.tid));
    }
    for (u32 i = 0; c.dirs && i < c.n && !hit; i++)
        hit = strncmp(c.dirs[i], dir, 32) == 0;
    spin_unlock(&lock);
    return hit;
}

/* the stand-in file of a save's sealed key (host path), written once a session */
static bool key_file(char *out, size_t size)
{
    static volatile bool made;
    jbc_dir(out, size, "/sealedkey");
    if (made)
        return true;
    u8 k[SEALED_LEN];
    sealed_key(k);
    s32 fd = K.open ? K.open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
    bool ok = fd >= 0 && K.write(fd, k, sizeof(k)) == (s64)sizeof(k);
    if (fd >= 0)
        K.close(fd);
    made = ok;
    return ok;
}

/* a key file of a save the title may see: its stand-in in out */
static bool key_standin(const char *path, char *out, size_t size)
{
    s32 user;
    char tid[16], dir[32];
    if (!key_path(path, &user, tid, dir) || !escaped() || !savedata_is_foreign(tid) || !save_listed(user, tid, dir))
        return false;
    return key_file(out, size);
}

/* a sealed key file written: a tool making a new save image, which can't be
 * made here (a sealed key alone is no sign: Apollo makes one at start to
 * learn the key set); said on screen (hle_fsvsh.c) */
void fsvsh_new_save_refused(void) HIDDEN __attribute__((weak));

/* hle_file.c's open: true when the path is one of these (*fd the fd or an SCE error) */
bool jbc_open(const char *path, int flags, int mode, s64 *fd) HIDDEN;
bool jbc_open(const char *path, int flags, int mode, s64 *fd)
{
    if (!g_kv || !path || path[0] != '/' || (path[1] != 'd' && path[1] != 'u'))
        return false;
    s32 dev = dev_of(path);
    if (dev >= 0) {
        if (!escaped())
            return false;
        char p[128], sub[32];
        fmt(sub, sizeof(sub), "/dev_%s", k_dev[dev]);
        jbc_dir(p, sizeof(p), sub);
        s32 f = K.open ? K.open(p, O_RDWR | O_CREAT, 0666) : E_NOENT;
        if (f >= 0) {
            spin_lock(&g_devfd_lock);
            u32 i = 0;
            while (i < ARRAY_SIZE(g_devfd) && g_devfd[i].dev > 0)
                i++;
            if (i < ARRAY_SIZE(g_devfd))
                g_devfd[i].fd = f, g_devfd[i].dev = dev + 1;
            spin_unlock(&g_devfd_lock);
            if (i == ARRAY_SIZE(g_devfd)) {
                K.close(f);
                f = (s32)0x80020018; /* EMFILE */
            }
        }
        static u32 said;
        if (said++ < 8)
            logf("jbc: open %s (virtual device) -> %x", path, f);
        *fd = f;
        return true;
    }
    char p[128];
    s32 user;
    char tid[16], dir[32];
    if ((flags & 3) != O_RDONLY && key_path(path, &user, tid, dir) && escaped()) {
        logf("jbc: %s opened for writing: a new save image's sealed key (refused as SWRR refuses it)", path);
        if (fsvsh_new_save_refused)
            fsvsh_new_save_refused();
        return false;
    }
    if ((flags & 3) == O_RDONLY && key_standin(path, p, sizeof(p))) {
        *fd = K.open ? K.open(p, O_RDONLY, 0) : E_NOENT;
        static u32 said;
        if (said++ < 8)
            logf("jbc: open %s: the save's sealed key, a stand-in (the virtual mount needs no key) -> %lx", path,
                 *fd);
        return true;
    }
    return false;
}

/* hle_file.c's stat and access: true when the path is one of these (*rc 0 or an SCE error) */
bool jbc_stat(const char *path, void *st, s32 *rc) HIDDEN;
bool jbc_stat(const char *path, void *st, s32 *rc)
{
    if (!g_kv || !path || path[0] != '/' || (path[1] != 'd' && path[1] != 'u'))
        return false;
    s32 dev = dev_of(path);
    if (dev >= 0) {
        if (!escaped())
            return false;
        if (st) {
            u8 *s = st;
            memset(s, 0, 0x78);
            *(u32 *)(s + 0) = 0x6A6263;          /* st_dev */
            *(u32 *)(s + 4) = 100 + (u32)dev;    /* st_ino */
            *(u16 *)(s + 8) = 020666;            /* st_mode: a character device */
            *(u16 *)(s + 10) = 1;                /* st_nlink */
            *(u32 *)(s + 0x14) = 0x100 + (u32)dev; /* st_rdev */
        }
        *rc = 0;
        return true;
    }
    char p[128];
    if (key_standin(path, p, sizeof(p))) {
        u8 tmp[0x78];
        *rc = K.stat ? K.stat(p, st ? st : tmp) : E_NOENT;
        return true;
    }
    return false;
}

/* hle_file.c's close */
void jbc_close(s32 fd) HIDDEN;
void jbc_close(s32 fd)
{
    if (fd < 0 || !g_kv)
        return;
    spin_lock(&g_devfd_lock);
    for (u32 i = 0; i < ARRAY_SIZE(g_devfd); i++)
        if (g_devfd[i].dev > 0 && g_devfd[i].fd == fd)
            g_devfd[i].dev = 0;
    spin_unlock(&g_devfd_lock);
    if (fd < NFILES) {
        spin_lock(&g_kv_lock);
        wr64(O_FILES + FILE_SZ * (u32)fd + F_DATA, KA(O_FILEVN));
        spin_unlock(&g_kv_lock);
    }
}

static s64 posix_err(int e)
{
    if (K.errno_fn)
        *K.errno_fn() = e;
    return -1;
}

/* hle_homebrew.c's ioctl: true when fd is a virtual device (*rc the POSIX result) */
#define SBL_SEAL 0x40845303UL
#define SBL_UNSEAL 0xC0845302UL
bool jbc_ioctl(s32 fd, u64 req, u64 arg, s64 *rc) HIDDEN;
bool jbc_ioctl(s32 fd, u64 req, u64 arg, s64 *rc)
{
    if (!g_kv)
        return false;
    s32 dev = dev_fd(fd);
    if (dev < 0)
        return false;
    u8 *a = (u8 *)arg;
    static u32 said;
    if (dev == DEV_SBL && req == SBL_SEAL && a) {
        sealed_key(a); /* the kernel writes 0x84 bytes; callers read the first 0x60 */
        *rc = 0;
        if (said++ < 8)
            logf("jbc: sbl_srv: a new sealed key (key set %u; a stand-in: the virtual mount needs no key)", KEYSET);
    } else if (dev == DEV_SBL && req == SBL_UNSEAL && a) {
        if (memcmp(a, "pfsSKKey", 8) != 0) {
            *rc = posix_err(22);
        } else {
            static const char mark[32] = "ghostldr: virtual save key";
            memcpy(a + SEALED_LEN, mark, sizeof(mark));
            *rc = 0;
        }
        if (said++ < 8)
            logf("jbc: sbl_srv: unseal a sealed key -> %ld (a marker: ghostldr's save mount uses the save service, "
                 "not this key)", *rc);
    } else {
        *rc = posix_err(22);
        logf("jbc: /dev/%s: ioctl %lx not answered (EINVAL)", k_dev[dev], req);
    }
    return true;
}

/* mknod of a virtual device (raw system call 14, hle_rawsys.c, or libkernel's): 0 once escaped */
s32 jbc_mknod(const char *path) HIDDEN;
s32 jbc_mknod(const char *path)
{
    if (dev_of(path) < 0 || !escaped())
        return -1;
    static u32 said;
    if (said++ < 4)
        logf("jbc: mknod %s: the virtual device is there", path);
    return 0;
}

/* ---- libkernel ---- */

static s64 jb_setuid(u32 uid)
{
    bool root = false;
    spin_lock(&g_kv_lock);
    if (g_kv && rd32(O_CRED + CR_UID) == 0) {
        root = true;
        for (u32 k = 0; k < 3; k++)
            wr32(O_CRED + CR_UID + 4 * k, uid);
    }
    spin_unlock(&g_kv_lock);
    if (root)
        return 0;
    s64 (*real)(u64) = host_real("setuid");
    return real ? real(uid) : posix_err(1);
}

static s64 jb_mknod(const char *path, u32 mode, u32 dev)
{
    if (jbc_mknod(path) == 0)
        return 0;
    s64 (*real)(const char *, u64, u64) = host_real("mknod");
    return real ? real(path, mode, dev) : posix_err(1);
}

static int jbc_init(void)
{
    if (remap_hook_add && remap_hook_add(jbc_remap) != 0)
        logf("jbc: no room for its path hook");
    return 0;
}

static const struct ext_hle_ent k_hle[] = {
    {"libkernel", "setuid", jb_setuid, NULL},
    {"libkernel", "mknod", jb_mknod, NULL},
};

EXT_FAMILY("jbc", k_hle, jbc_init);
