/*
 * ftp_saves.c (the FTP server's /saves), built for this machine as x86-64
 * (test_ftp_saves.py) and run against a fake console: ftp_saves_os.c, which
 * keeps the files in a temporary folder and models the save and user
 * services.  This side is the extension's world (ghost.h): the core API and
 * ftp.h's environment that ftp_saves.c uses, faked, and the calls ftp_sys.c
 * makes (ftps_cmd: one command of the driver's script).
 */
#include "ghost_api.h"
#include "ftp.h"
#include "savedata_hooks.h"

/* ftp_saves_os.c */
void *os_real(const char *name);
void os_log(const char *line);
int os_usleep(u32 us);
void *os_calloc(u64 n);
void os_free(void *p);
int os_vfmt(char *buf, u64 size, const char *f, __builtin_va_list ap);
long os_clock_ms(void);
int os_thread(void *(*fn)(void *), void *arg);
int os_gfs_mount(s32 user, const char *dir, int mode, char mp[16]);
void os_out(const char *line);
s32 os_race_copy(s32 user, const char *tid, const char *dir, const char *fp, const char *dst);
s32 os_race_write(s32 user, const char *tid, const char *dir, const char *src, int prune);
u64 os_digest(const char *dir);

/* ftp_saves.c */
bool ftps_owns(const char *path);
int ftps_stat(const char *path, struct fv_stat *st);
int ftps_open(const char *path, int flags);
int ftps_closing(int fd);
void ftps_closed(int k);
int ftps_opendir(const char *path, struct fv_dir *d);
int ftps_readdir(struct fv_dir *d, char *name, unsigned size);
void ftps_closedir(struct fv_dir *d);
int ftps_mkdir(const char *path);
int ftps_rmdir(const char *path);
int ftps_unlink(const char *path);
int ftps_rename(const char *from, const char *to);
const char *ftps_root_name(void);
void ftps_idle(void);
void ftps_halt(void);
int ftps_stop(void);
void ftps_title_starting(void);

struct kfuncs K;
const struct ghost_api *G;
static struct boot_ctx g_ctx_v;
static struct boot_ctx *g_ctx_p = &g_ctx_v;
static char g_save_name[16];

static void hlog(const char *f, ...)
{
    char line[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    os_vfmt(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    os_log(line);
}

static void out(const char *f, ...)
{
    char line[4096];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    os_vfmt(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    os_out(line);
}

static void *h_real(const char *name) { return os_real(name); }
static void *h_real_in(const char *lib, const char *name) { return os_real(name); }
static void *h_page_alloc(size_t n) { return os_calloc(n); }
static void h_page_free(void *p, size_t n) { os_free(p); }
static void h_lock(volatile int *l)
{
    while (__sync_lock_test_and_set(l, 1))
        ;
}
static void h_unlock(volatile int *l) { __sync_lock_release(l); }
static int h_reserve(void) { return 0; }
static void h_release(void) {}
static const char *h_save_name(void) { return g_save_name; }
static int h_mount_user(s32 user, const char *dir, int mode, u64 blocks, char mp[16], u64 *required)
{
    return os_gfs_mount(user, dir, mode, mp);
}

static const struct ghost_api g_api = {
    .version = GHOST_API_VERSION,
    .size = sizeof(struct ghost_api),
    .k = &K,
    .ctx = &g_ctx_p,
    .logf = (void *)hlog,
    .host_real = h_real,
    .host_real_in = h_real_in,
    .page_alloc = h_page_alloc,
    .page_free = h_page_free,
    .spin_lock = h_lock,
    .spin_unlock = h_unlock,
    .gfs_slot_reserve = h_reserve,
    .gfs_slot_release = h_release,
    .gfs_save_name = h_save_name,
    .gfs_mount_user = h_mount_user,
};

/* ftp.h's environment, the part ftp_saves.c uses */
long fv_clock_ms(void) { return os_clock_ms(); }
long fv_now(void) { return 1700000000; }
void fv_sleep_ms(unsigned ms) { os_usleep(ms * 1000); }
int fv_thread(void *(*fn)(void *), void *arg, const char *name) { return os_thread(fn, arg); }

/* save_race.c's: the fake console copies and writes (a race there always wins) */
s32 save_race_copy(s32 user, const char *tid, const char *dir, const char *dst)
{
    return os_race_copy(user, tid, dir, NULL, dst);
}
s32 save_fp_copy(s32 user, const char *tid, const char *dir, const char *fp, const char *dst)
{
    return os_race_copy(user, tid, dir, fp, dst);
}
s32 save_race_write(s32 user, const char *tid, const char *dir, const char *src, const char *fp, const char *backup,
                    struct save_race_result *r)
{
    memset(r, 0, sizeof(*r));
    r->rc = os_race_write(user, tid, dir, src, fp != NULL);
    r->needs_exit = r->rc == 0;
    return r->rc;
}
u64 save_tree_digest(const char *dir) { return os_digest(dir); }
void ov_banner(const char *text, u32 seconds) { hlog("banner: %s", text); }

/* hle_savedata.c's and approot.c's */
static volatile int g_search;
void savedata_search_lock(void)
{
    if (!__sync_lock_test_and_set(&g_search, 1))
        return;
    hlog("VIOLATION two searches at once");
    while (__sync_lock_test_and_set(&g_search, 1))
        ;
}
void savedata_search_unlock(void) { __sync_lock_release(&g_search); }
const char *usb_slot_mp(u32 k)
{
    static const char *const mp[] = {"/ghostldr_usb", "/ghostldr_usb2", "/ghostldr_usb3", "/ghostldr_usb4"};
    return mp[k & 3];
}

static int g_held[8], g_nheld;

static void close_fd(int fd)
{
    int k = ftps_closing(fd);
    K.close(fd);
    ftps_closed(k);
}

/* the driver's commands (ftp_sys.c's calls into ftp_saves.c) */
void ftps_cmd(const char *op, const char *arg)
{
    if (!G) {
        G = &g_api;
        K.usleep = (void *)os_real("usleep");
        K.getpid = (void *)os_real("getpid");
        K.open = (void *)os_real("sceKernelOpen");
        K.read = (void *)os_real("sceKernelRead");
        K.close = (void *)os_real("sceKernelClose");
        K.stat = (void *)os_real("sceKernelStat");
        K.write = (void *)os_real("sceKernelWrite");
        K.pread = (void *)os_real("sceKernelPread");
    }
    /* ftp_sys.c sends a path here only when ftps_owns says it is /saves's */
    if (arg[0] == '/' && strcmp(op, "owns") != 0 && !ftps_owns(arg)) {
        out("%s %s: not /saves", op, arg);
        return;
    }
    if (strcmp(op, "flag") == 0) {
        g_ctx_v.flags = arg[0] == '1' ? CTX_FLAG_FTP_SAVES : 0;
    } else if (strcmp(op, "owns") == 0) {
        out("owns %s: %d root %s", arg, ftps_owns(arg), ftps_root_name() ? ftps_root_name() : "-");
    } else if (strcmp(op, "ls") == 0) {
        struct fv_dir *d = os_calloc(sizeof(*d));
        int rc = ftps_opendir(arg, d);
        if (rc != 0) {
            out("ls %s: error %d", arg, -rc);
            os_free(d);
            return;
        }
        char line[4096], name[256];
        int n = fmt(line, sizeof(line), "ls %s:", arg);
        if (d->virt == 3) {
            while (ftps_readdir(d, name, sizeof(name)) > 0)
                n += fmt(line + n, sizeof(line) - (size_t)n, " %s", name);
            ftps_closedir(d);
        } else {
            int (*getdents)(int, void *, int) = os_real("sceKernelGetdents");
            u8 b[4096];
            for (int got; (got = getdents((int)d->h, b, sizeof(b))) > 0;)
                for (int off = 0; off + 8 <= got;) {
                    u16 reclen = *(u16 *)(b + off + 4);
                    u8 nlen = b[off + 7];
                    memcpy(name, b + off + 8, nlen);
                    name[nlen] = 0;
                    off += reclen;
                    if (strcmp(name, ".") && strcmp(name, ".."))
                        n += fmt(line + n, sizeof(line) - (size_t)n, " %s", name);
                }
            close_fd((int)d->h);
        }
        out("%s", line);
        os_free(d);
    } else if (strcmp(op, "stat") == 0) {
        struct fv_stat st;
        int rc = ftps_stat(arg, &st);
        if (rc != 0)
            out("stat %s: error %d", arg, -rc);
        else
            out("stat %s: %s %ld mode %o mtime %ld", arg, (st.mode & FV_S_IFMT) == FV_S_IFDIR ? "dir" : "file",
                st.size, st.mode & 0777, st.mtime);
    } else if (strcmp(op, "cat") == 0 || strcmp(op, "hold") == 0) {
        int fd = ftps_open(arg, FV_O_RDONLY);
        if (fd < 0) {
            out("%s %s: error %d", op, arg, -fd);
            return;
        }
        if (op[0] == 'h' && g_nheld < 8) {
            g_held[g_nheld++] = fd;
            out("hold %s: ok", arg);
            return;
        }
        char b[1024];
        long r = K.read(fd, b, sizeof(b) - 1);
        b[r > 0 ? r : 0] = 0;
        close_fd(fd);
        out("cat %s: %s", arg, b);
    } else if (strcmp(op, "drop") == 0) {
        while (g_nheld)
            close_fd(g_held[--g_nheld]);
        out("drop: ok");
    } else if (strcmp(op, "write") == 0 || strcmp(op, "put") == 0) {
        /* put PATH TEXT */
        char path[512];
        const char *text = "x";
        strlcpy(path, arg, sizeof(path));
        for (char *c = path; *c; c++)
            if (*c == ' ') {
                *c = 0, text = arg + (c - path) + 1;
                break;
            }
        int fd = ftps_open(path, FV_O_WRONLY | FV_O_CREAT | FV_O_TRUNC);
        if (fd >= 0) {
            K.write(fd, text, strlen(text));
            close_fd(fd);
        }
        out("%s %s: error %d", op, path, fd < 0 ? -fd : 0);
    } else if (strcmp(op, "rm") == 0) {
        out("rm %s: error %d", arg, -ftps_unlink(arg));
    } else if (strcmp(op, "rmdir") == 0) {
        out("rmdir %s: error %d", arg, -ftps_rmdir(arg));
    } else if (strcmp(op, "mv") == 0) {
        char from[512];
        const char *to = "";
        strlcpy(from, arg, sizeof(from));
        for (char *c = from; *c; c++)
            if (*c == ' ') {
                *c = 0, to = arg + (c - from) + 1;
                break;
            }
        out("mv %s: error %d", from, -ftps_rename(from, to));
    } else if (strcmp(op, "mkdir") == 0) {
        int rc = ftps_mkdir(arg);
        out("mkdir %s: error %d", arg, -rc);
    } else if (strcmp(op, "idle") == 0) {
        ftps_idle();
    } else if (strcmp(op, "sleep") == 0) {
        int ms = 0;
        for (const char *p = arg; *p >= '0' && *p <= '9'; p++)
            ms = ms * 10 + (*p - '0');
        os_usleep((u32)ms * 1000);
    } else if (strcmp(op, "title") == 0) {
        fmt(g_save_name, sizeof(g_save_name), "%sSAVE", arg);
        ftps_title_starting();
    } else if (strcmp(op, "stop") == 0) {
        ftps_halt();
        out("stop: %d", ftps_stop());
    } else {
        out("unknown command %s", op);
    }
}
