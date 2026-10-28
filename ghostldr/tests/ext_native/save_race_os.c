/*
 * The fake console for save_race_host.c: libkernel's file calls on a
 * temporary folder (ROOT) and SWRR's save service, as measured on a 13.60 PS5
 * (research/apollo: savedata-reach A6, C-key, C1-RW; ps4-reference E1):
 *
 *   ROOT/saves/<TITLE>/<DIR>/   a save image's files (sce_sys/param.sfo, keystone, ...)
 *   ROOT/mnt/<N>/               what /savedataN shows while a save is bound there
 *   ROOT/temp0/                 SWRR's /temp0
 *
 * sceSaveDataMountInternal binds the save (a copy of its files) at the lowest
 * free /savedataN, waits (the window in which ShellCore checks it), then
 * refuses: SWRR's keystone is not the save's.  If a descriptor inside the
 * slot is open at that moment, the cleanup unmount fails and the save stays
 * bound ("held"); paths into it no longer resolve (the measured 0x80020018),
 * descriptors keep working.  Closing SWRR (os_exit) commits every read-write
 * bound save: its files replace the image's.  sce_sys is the system's: the
 * app can read param.sfo but not keystone, and can't write either.  SWRR
 * can't name anything relative to a descriptor: openat answers EPERM (and
 * mkdirat, unlinkat ENOSYS: not offered here), as measured on the PS5.
 *
 * Scenarios: ps5 (as above; a 12.50 PS4 behaves the same), unbinds (a console
 * that unbinds the slot at once even when held: descriptors stay valid but
 * nothing is committed), allowed (the keystone matches: a normal read-write
 * mount, committed by its unmount), jakx-host (Jak X is the running host).
 *
 * Prints log lines, "VIOLATION ..." for anything the writer must never do,
 * and the host side's RESULT line; exits 0.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int host_run(const char *tid, const char *dir, const char *src, const char *fp, const char *backup, int check);
int host_fsvsh(const char *fp, int change);
int host_prescan(const char *saves, uint32_t user, uint32_t stop);

#define O_DIRECTORY_ORBIS 0x20000
#define NFD 4096
#define SLOTS 16

static char g_root[1024];
static const char *g_scenario;
static int g_errno;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static struct {
    int bound, held, rw, ro_fp;
    char tid[16], dir[32];
} g_slot[SLOTS];

static struct {
    int used, slot, dir;
    char path[1024]; /* host path */
    char *dents;     /* FreeBSD dirents, made at the first getdents */
    int dlen, doff;
} g_fd[NFD];

static int is(const char *s) { return strcmp(g_scenario, s) == 0; }

int os_put(const char *console_path, const void *data, uint64_t n)
{
    char p[1200];
    snprintf(p, sizeof(p), "%s%s", g_root, console_path);
    FILE *f = fopen(p, "wb");
    if (!f)
        return -1;
    fwrite(data, 1, n, f);
    fclose(f);
    return 0;
}

void os_log(const char *line)
{
    printf("%s\n", line);
    fflush(stdout);
}

int os_usleep(uint32_t us) { return usleep(us); }
void *os_calloc(uint64_t n) { return calloc(1, n); }
void os_free(void *p) { free(p); }
int os_vfmt(char *buf, uint64_t size, const char *f, va_list ap) { return vsnprintf(buf, size, f, ap); }

static void violation(const char *what, const char *path)
{
    printf("VIOLATION %s %s\n", what, path);
    fflush(stdout);
}

/* ---- paths ---- */

/* a console path -> host path; slot of /savedataN (or -1); 0, or the errno */
static int map(const char *p, char *out, size_t size, int *slot)
{
    *slot = -1;
    if (strncmp(p, "/savedata", 9) == 0) {
        char *end;
        long n = strtol(p + 9, &end, 10);
        if (end == p + 9 || n < 0 || n >= SLOTS || (*end && *end != '/'))
            return ENOENT;
        *slot = (int)n;
        if (!g_slot[n].bound)
            return ENOENT;
        if (g_slot[n].held)
            return 24; /* the measured 0x80020018 once ShellCore refused the mount */
        snprintf(out, size, "%s/mnt/%ld%s", g_root, n, end);
        return 0;
    }
    snprintf(out, size, "%s%s", g_root, p);
    return 0;
}

static int sys_part(const char *host, int slot)
{
    if (slot < 0)
        return 0;
    char base[1100];
    snprintf(base, sizeof(base), "%s/mnt/%d/sce_sys", g_root, slot);
    return strncmp(host, base, strlen(base)) == 0 ? (host[strlen(base)] == 0 || host[strlen(base)] == '/' ? 1 : 0) : 0;
}

static int host_flags(int f)
{
    int h = f & 3;
    if (f & 0x200)
        h |= O_CREAT;
    if (f & 0x400)
        h |= O_TRUNC;
    if (f & 0x800)
        h |= O_EXCL;
    if (f & O_DIRECTORY_ORBIS)
        h |= O_DIRECTORY;
    return h;
}

static int track(int fd, int slot, int dir, const char *host)
{
    if (fd < 0 || fd >= NFD) {
        if (fd >= 0)
            close(fd);
        g_errno = EMFILE;
        return -1;
    }
    g_fd[fd].used = 1, g_fd[fd].slot = slot, g_fd[fd].dir = dir, g_fd[fd].dlen = g_fd[fd].doff = 0;
    free(g_fd[fd].dents), g_fd[fd].dents = NULL;
    snprintf(g_fd[fd].path, sizeof(g_fd[fd].path), "%s", host);
    return fd;
}

static int do_open(const char *host, int slot, int flags, int mode)
{
    if (sys_part(host, slot)) {
        if (flags & 3 || flags & 0x200) {
            violation("write into sce_sys:", host);
            g_errno = EACCES;
            return -1;
        }
        if (strstr(host, "keystone")) {
            g_errno = EACCES;
            return -1;
        }
    }
    if (slot >= 0 && g_slot[slot].ro_fp && (flags & 3)) {
        g_errno = EROFS;
        return -1;
    }
    int fd = open(host, host_flags(flags), mode);
    if (fd < 0) {
        g_errno = errno;
        return -1;
    }
    struct stat st;
    fstat(fd, &st);
    pthread_mutex_lock(&g_mu);
    int r = track(fd, slot, S_ISDIR(st.st_mode), host);
    pthread_mutex_unlock(&g_mu);
    return r;
}

static int k_open(const char *p, int flags, int mode)
{
    char host[1100];
    int slot;
    int e = map(p, host, sizeof(host), &slot);
    if (e) {
        g_errno = e;
        return -1;
    }
    return do_open(host, slot, flags, mode);
}

/* SWRR's openat: refused whatever the descriptor (EPERM on the 13.60 PS5) */
static int k_openat(int dirfd, const char *name, int flags, int mode)
{
    g_errno = EPERM;
    return -1;
}

static int k_close(int fd)
{
    pthread_mutex_lock(&g_mu);
    if (fd >= 0 && fd < NFD && g_fd[fd].used) {
        g_fd[fd].used = 0;
        free(g_fd[fd].dents), g_fd[fd].dents = NULL;
    }
    pthread_mutex_unlock(&g_mu);
    return close(fd) == 0 ? 0 : (g_errno = errno, -1);
}

static int64_t k_read(int fd, void *b, uint64_t n) { int64_t r = read(fd, b, n); return r < 0 ? (g_errno = errno, -1) : r; }

static int64_t k_write(int fd, const void *b, uint64_t n)
{
    int64_t r = write(fd, b, n);
    return r < 0 ? (g_errno = errno, -1) : r;
}

static int64_t k_pread(int fd, void *b, uint64_t n, int64_t off)
{
    int64_t r = pread(fd, b, n, off);
    return r < 0 ? (g_errno = errno, -1) : r;
}

static int64_t k_pwrite(int fd, const void *b, uint64_t n, int64_t off)
{
    if (fd >= 0 && fd < NFD && sys_part(g_fd[fd].path, g_fd[fd].slot))
        violation("pwrite into sce_sys:", g_fd[fd].path);
    int64_t r = pwrite(fd, b, n, off);
    return r < 0 ? (g_errno = errno, -1) : r;
}

static int64_t k_lseek(int fd, int64_t off, int whence)
{
    if (fd >= 0 && fd < NFD && g_fd[fd].used && g_fd[fd].dir) {
        if (off != 0 || whence != 0)
            return g_errno = EINVAL, -1;
        g_fd[fd].doff = 0; /* the listing again from its start (made afresh: the folder may have changed) */
        free(g_fd[fd].dents), g_fd[fd].dents = NULL;
        return 0;
    }
    int64_t r = lseek(fd, off, whence);
    return r < 0 ? (g_errno = errno, -1) : r;
}

static int k_ftruncate(int fd, int64_t n) { return ftruncate(fd, n) == 0 ? 0 : (g_errno = errno, -1); }
static int k_fsync(int fd) { return fsync(fd) == 0 ? 0 : (g_errno = errno, -1); }

static void fill_stat(const struct stat *st, uint8_t *out)
{
    memset(out, 0, 0x78);
    uint16_t mode = S_ISDIR(st->st_mode) ? 0x41ff : 0x81b6;
    memcpy(out + 8, &mode, 2);
    uint64_t size = (uint64_t)st->st_size;
    memcpy(out + 0x48, &size, 8);
}

static int k_stat(const char *p, uint8_t *out)
{
    char host[1100];
    int slot, e = map(p, host, sizeof(host), &slot);
    struct stat st;
    if (e) {
        /* a held slot's mount point itself still stats (it is still mounted) */
        if (e == 24 && slot >= 0 && strlen(p) == strlen("/savedata") + (slot > 9 ? 2 : 1)) {
            memset(out, 0, 0x78);
            uint16_t mode = 0x41ff;
            memcpy(out + 8, &mode, 2);
            return 0;
        }
        g_errno = e;
        return -1;
    }
    if (stat(host, &st) != 0)
        return g_errno = errno, -1;
    fill_stat(&st, out);
    return 0;
}

static int k_fstat(int fd, uint8_t *out)
{
    struct stat st;
    if (fstat(fd, &st) != 0)
        return g_errno = errno, -1;
    fill_stat(&st, out);
    return 0;
}

static int k_mkdir(const char *p, int mode)
{
    char host[1100];
    int slot, e = map(p, host, sizeof(host), &slot);
    if (e)
        return g_errno = e, -1;
    if (sys_part(host, slot))
        violation("mkdir in sce_sys:", host);
    return mkdir(host, (mode_t)mode) == 0 ? 0 : (g_errno = errno, -1);
}

static int k_unlink(const char *p)
{
    char host[1100];
    int slot, e = map(p, host, sizeof(host), &slot);
    if (e)
        return g_errno = e, -1;
    if (sys_part(host, slot))
        violation("remove in sce_sys:", host);
    return unlink(host) == 0 ? 0 : (g_errno = errno, -1);
}

static int k_rmdir(const char *p)
{
    char host[1100];
    int slot, e = map(p, host, sizeof(host), &slot);
    if (e)
        return g_errno = e, -1;
    if (sys_part(host, slot))
        violation("remove in sce_sys:", host);
    return rmdir(host) == 0 ? 0 : (g_errno = errno, -1);
}

static int k_chmod(const char *p, int mode)
{
    char host[1100];
    int slot, e = map(p, host, sizeof(host), &slot);
    if (e)
        return g_errno = e, -1;
    return chmod(host, (mode_t)mode) == 0 ? 0 : (g_errno = errno, -1);
}

/* FreeBSD dirents: u32 fileno, u16 reclen, u8 type, u8 namlen, name, NUL, padded to 4 */
static int k_getdents(int fd, uint8_t *buf, int n)
{
    if (fd < 0 || fd >= NFD || !g_fd[fd].used || !g_fd[fd].dir)
        return g_errno = EBADF, -1;
    if (!g_fd[fd].dents) {
        DIR *d = opendir(g_fd[fd].path);
        if (!d)
            return g_errno = errno, -1;
        size_t cap = 65536, len = 0;
        char *out = calloc(1, cap);
        struct dirent *e;
        uint32_t ino = 1;
        while ((e = readdir(d))) {
            size_t nl = strlen(e->d_name), rl = (8 + nl + 1 + 3) & ~(size_t)3;
            if (len + rl > cap)
                break;
            uint8_t type = e->d_type == DT_DIR ? 4 : 8;
            memcpy(out + len, &ino, 4), ino++;
            uint16_t r16 = (uint16_t)rl;
            memcpy(out + len + 4, &r16, 2);
            out[len + 6] = (char)type, out[len + 7] = (char)nl;
            memcpy(out + len + 8, e->d_name, nl);
            len += rl;
        }
        closedir(d);
        g_fd[fd].dents = out, g_fd[fd].dlen = (int)len, g_fd[fd].doff = 0;
    }
    int left = g_fd[fd].dlen - g_fd[fd].doff, take = 0;
    while (take < left) { /* whole records only */
        uint16_t rl;
        memcpy(&rl, g_fd[fd].dents + g_fd[fd].doff + take + 4, 2);
        if (take + rl > n)
            break;
        take += rl;
    }
    memcpy(buf, g_fd[fd].dents + g_fd[fd].doff, (size_t)take);
    g_fd[fd].doff += take;
    return take;
}

static int *k_error(void) { return &g_errno; }

/* sceKernelGetAppInfo: the host is SWRR (EU), or Jak X with the "jakx-host" scenario */
static int k_appinfo(int pid, uint8_t *info)
{
    memset(info, 0, 0x80);
    memcpy(info + 0x10, is("jakx-host") ? "CUSA07992" : "CUSA03492", 9);
    return 0;
}
static int k_yield(void) { return sched_yield(); }

static int k_thread(uint64_t *th, const void *attr, void *(*fn)(void *), void *arg, const char *name)
{
    pthread_t t;
    int r = pthread_create(&t, NULL, fn, arg);
    *th = (uint64_t)(uintptr_t)t;
    return r;
}

static int k_join(uint64_t th, void **ret) { return pthread_join((pthread_t)(uintptr_t)th, ret); }

/* ---- the save service ---- */

static void run(const char *cmd)
{
    if (system(cmd) != 0)
        printf("fake: command failed: %s\n", cmd);
}

static int save_exists(const char *tid, const char *dir)
{
    char p[1200];
    struct stat st;
    snprintf(p, sizeof(p), "%s/saves/%s/%s", g_root, tid, dir);
    return stat(p, &st) == 0;
}

static int bind(const char *tid, const char *dir, int rw)
{
    int n = 1;
    while (n < SLOTS && g_slot[n].bound)
        n++;
    if (n == SLOTS)
        return -1;
    char cmd[3000];
    /* the mount point first (a racer may open it, empty, in that moment), then the save copied aside and
     * moved in: it appears at /savedataN whole, as a mount does */
    snprintf(cmd, sizeof(cmd), "rm -rf '%s/mnt/%d' && mkdir -p '%s/mnt/%d'", g_root, n, g_root, n);
    run(cmd);
    g_slot[n].bound = 1, g_slot[n].held = 0, g_slot[n].rw = rw, g_slot[n].ro_fp = 0;
    usleep(20000);
    g_slot[n].bound = 0;
    /* the bare mount point stays as .emptyN: a descriptor caught on it keeps seeing it empty, as a covered
     * folder does */
    snprintf(cmd, sizeof(cmd), "rm -rf '%s/mnt/.%d' '%s/mnt/.empty%d' && cp -R '%s/saves/%s/%s' '%s/mnt/.%d' && "
             "mv '%s/mnt/%d' '%s/mnt/.empty%d' && mv '%s/mnt/.%d' '%s/mnt/%d'", g_root, n, g_root, n, g_root, tid, dir,
             g_root, n, g_root, n, g_root, n, g_root, n, g_root, n);
    run(cmd);
    g_slot[n].held = 0, g_slot[n].rw = rw, g_slot[n].ro_fp = 0;
    snprintf(g_slot[n].tid, sizeof(g_slot[n].tid), "%s", tid);
    snprintf(g_slot[n].dir, sizeof(g_slot[n].dir), "%s", dir);
    __sync_synchronize();
    g_slot[n].bound = 1;
    return n;
}

static int busy(int n)
{
    int c = 0;
    pthread_mutex_lock(&g_mu);
    for (int fd = 0; fd < NFD; fd++)
        c += g_fd[fd].used && g_fd[fd].slot == n;
    pthread_mutex_unlock(&g_mu);
    return c;
}

static void unbind(int n, int commit)
{
    char cmd[3000];
    if (commit) {
        snprintf(cmd, sizeof(cmd), "rm -rf '%s/saves/%s/%s' && cp -R '%s/mnt/%d' '%s/saves/%s/%s'", g_root,
                 g_slot[n].tid, g_slot[n].dir, g_root, n, g_root, g_slot[n].tid, g_slot[n].dir);
        run(cmd);
    }
    /* moved away, not removed: descriptors still open stay valid (as a vnode outlives its unmount) */
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s/gone' && mv '%s/mnt/%d' '%s/gone/%d-%ld'", g_root, g_root, n, g_root, n,
             (long)random());
    run(cmd);
    g_slot[n].bound = g_slot[n].held = 0;
}

struct mount1 {
    int32_t user;
    uint32_t pad;
    const char *title, *dir;
    const char *fp;
    uint64_t blocks;
    uint32_t mode;
};

static void keystone_fp(const char *tid, const char *dir, char out[65])
{
    char p[1200];
    unsigned char ks[96] = {0};
    snprintf(p, sizeof(p), "%s/saves/%s/%s/sce_sys/keystone", g_root, tid, dir);
    FILE *f = fopen(p, "rb");
    if (f) {
        fread(ks, 1, 96, f);
        fclose(f);
    }
    for (int k = 0; k < 32; k++)
        sprintf(out + 2 * k, "%02x", ks[0x20 + k]);
}

static int32_t k_mount_internal(const struct mount1 *m, char *res, const void *opt)
{
    if (!m->title || !save_exists(m->title, m->dir))
        return (int32_t)0x809F0008;
    int n = bind(m->title, m->dir, m->mode & 2);
    if (n < 0)
        return (int32_t)0x809F000C;
    usleep(80000); /* ShellCore checks the param.sfo, the account, the keystone */
    if (is("allowed")) {
        snprintf(res, 16, "/savedata%d", n);
        return 0;
    }
    int held = busy(n);
    if (held && !is("unbinds")) {
        g_slot[n].held = 1;
        printf("fake: refused %s:%s at /savedata%d; the cleanup unmount failed (EBUSY): %d descriptors\n", m->title,
               m->dir, n, held);
    } else {
        unbind(n, 0);
    }
    return (int32_t)0x809F000F;
}

static int32_t k_mount(const struct mount1 *m, char *res)
{
    if (!m->title || !m->fp)
        return (int32_t)0x809F0000;
    if (!(m->mode & 1) || (m->mode & 2))
        return (int32_t)0x809F0000;
    if (!save_exists(m->title, m->dir))
        return (int32_t)0x809F0008;
    char fp[65];
    keystone_fp(m->title, m->dir, fp);
    if (strcmp(fp, m->fp) != 0)
        return (int32_t)0x809F0006;
    int n = bind(m->title, m->dir, 0);
    g_slot[n].ro_fp = 1;
    snprintf(res, 16, "/savedata%d", n);
    return 0;
}

static int32_t k_umount(const char *mp)
{
    int n = atoi(mp + 9);
    if (n <= 0 || n >= SLOTS || !g_slot[n].bound)
        return (int32_t)0x809F0004;
    if (g_slot[n].held || busy(n))
        return (int32_t)0x809F0003;
    unbind(n, g_slot[n].rw && is("allowed"));
    return 0;
}

/* seconds: the clock, or FAKE_NOW (the save scan's cache ages) */
static int k_tod(int64_t *tv, void *tz)
{
    const char *now = getenv("FAKE_NOW");
    tv[0] = now ? atoll(now) : (int64_t)time(NULL), tv[1] = 0;
    return 0;
}

/* SWRR closes: the forced unmount commits every read-write save still bound */
static void os_exit(void)
{
    for (int n = 1; n < SLOTS; n++)
        if (g_slot[n].bound) {
            printf("fake: SWRR closed: forced unmount of /savedata%d (%s:%s)%s\n", n, g_slot[n].tid, g_slot[n].dir,
                   g_slot[n].rw ? ", committed" : "");
            unbind(n, g_slot[n].rw);
        }
}

static const struct {
    const char *name;
    void *fn;
} k_fns[] = {
    {"open", k_open},       {"openat", k_openat},   {"close", k_close},       {"read", k_read},
    {"write", k_write},     {"mkdir", k_mkdir},     {"getdents", k_getdents}, {"stat", k_stat},
    {"chmod", k_chmod},     {"unlink", k_unlink},   {"rmdir", k_rmdir},       {"sched_yield", k_yield},
    {"ftruncate", k_ftruncate}, {"fsync", k_fsync},
    {"pwrite", k_pwrite},   {"pread", k_pread},     {"fstat", k_fstat},       {"__error", k_error},
    {"lseek", k_lseek},     {"sceKernelGetAppInfo", k_appinfo},
    {"scePthreadCreate", k_thread}, {"scePthreadJoin", k_join},
    {"sceSaveDataMountInternal", k_mount_internal}, {"sceSaveDataUmount", k_umount},
    {"sceSaveDataMount", k_mount},
    /* the save scan's cache (hle_fsvsh.c) */
    {"sceKernelOpen", k_open},  {"sceKernelWrite", k_write}, {"sceKernelClose", k_close}, {"sceKernelMkdir", k_mkdir},
    {"gettimeofday", k_tod},
};

void *os_real(const char *name)
{
    for (size_t i = 0; i < sizeof(k_fns) / sizeof(k_fns[0]); i++)
        if (strcmp(k_fns[i].name, name) == 0)
            return k_fns[i].fn;
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 8) {
        fprintf(stderr, "usage: save_race_host ROOT SCENARIO TITLE DIR SRC FP|- BACKUP|- [check|noexit]\n");
        return 2;
    }
    snprintf(g_root, sizeof(g_root), "%s", argv[1]);
    g_scenario = argv[2];
    srandom(1);
    g_slot[0].bound = 1; /* SWRR's own save */
    snprintf(g_slot[0].tid, sizeof(g_slot[0].tid), "CUSA03492");
    int check = argc > 8 && strcmp(argv[8], "check") == 0, noexit = argc > 8 && strcmp(argv[8], "noexit") == 0;
    if (argc > 8 && strcmp(argv[8], "prescan") == 0)
        host_prescan(argv[3], (uint32_t)strtoul(argv[4], NULL, 16), (uint32_t)atoi(argv[5]));
    else if (argc > 8 && strncmp(argv[8], "fsvsh", 5) == 0)
        host_fsvsh(strcmp(argv[6], "-") ? argv[6] : NULL,
                   strcmp(argv[8], "fsvsh-learn") == 0      ? 2
                   : strcmp(argv[8], "fsvsh-learn-ro") == 0 ? 3
                                                             : strcmp(argv[8], "fsvsh") == 0);
    else
        host_run(argv[3], argv[4], argv[5], strcmp(argv[6], "-") ? argv[6] : NULL,
                 strcmp(argv[7], "-") ? argv[7] : NULL, check);
    for (int fd = 0; fd < NFD; fd++)
        if (g_fd[fd].used && g_fd[fd].slot >= 0)
            printf("VIOLATION descriptor %d left open in /savedata%d\n", fd, g_fd[fd].slot);
    /* the bare mount point (before the save covered it) must stay as it was: empty */
    for (int n = 1; n < SLOTS; n++) {
        char cmd[1300];
        snprintf(cmd, sizeof(cmd), "test -d '%s/mnt/.empty%d' && test -n \"$(ls -A '%s/mnt/.empty%d')\"", g_root, n,
                 g_root, n);
        if (system(cmd) == 0)
            printf("VIOLATION something was made in the bare mount point /savedata%d\n", n);
    }
    if (!noexit)
        os_exit();
    return 0;
}
