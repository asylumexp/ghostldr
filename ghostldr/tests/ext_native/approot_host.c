/*
 * The extension's app roots (ext/src/approot.c) built for this machine,
 * against a fake core: the console's files are a host directory (FSROOT).
 * A fake libSceUsbStorage mounts a drive whose files are FSROOT/PATH
 * (--map PATH) at the sandbox path approot.c asks for.  It has the shape
 * approot.c relies on: GetDeviceList refuses before Init, a second Init
 * ends the session, the unexported RequestMap2 is found 0xA0 before
 * RequestMap through an unwind table (--no-fde: none there), the mount is
 * refused with --map-fail, and --other PATH2 lists a second drive first.
 * With -DGHOST_USB_RAW, route B's drive is a disk image read by the real
 * usbfs stack (--drive).  test_usb.py drives it:
 *
 *     approot_host FSROOT APP_ROOT [--title T] [--select] [--map PATH] [--other PATH2]
 *                  [--map-fail] [--no-fde] [--inited] [--drive IMAGE] [--pick T|-] [--pick-at N]
 *                  [--profiles FILE] [--ftp [--late|--unplug]] [--live SCENARIO]
 *
 * Prints what approot_init (and approot_pick) decided, the games it found
 * (library(), one "lib" line each), the profile picked, where saves go,
 * whether the title's eboot.bin is reachable and the mount's state at the
 * end.  --pick T picks the first copy of T as the selector would, --pick-at
 * N the N-th game; --profiles is a profile bundle (host/profiles.py) for the
 * boot context's profile slot.  --ftp then does what the FTP server does
 * (ftp_sys.c): usb_ftp_prepare as the title starts, usb_ftp_home as a
 * client logs in; --late plugs the drive in between the two, --unplug
 * pulls it out.  It prints whether the title's own Init and Term are kept
 * off the session (usb_session_open) and where a client starts.
 *
 * Every drive listed is mounted at the mount point approot.c asks for (one
 * mount per drive and per mount point).  --live SCENARIO then runs the
 * title's drive going away (usb_live.c) with real fds on FSROOT's files:
 * the title's eboot.bin is opened and 2 bytes read, and
 *   unplug  the drive is pulled out and plugged in again (a new id) 3.8 s
 *           after a read on the fd failed (EIO): past the 3 s grace, so the
 *           user is told;
 *   wrong   a drive without the game comes first, the right one 4.5 s later;
 *   closed  as unplug, but the fd is gone (EBADF): opened again at the
 *           offset the title's calls left;
 *   stale   as unplug, but the old mount point stays taken: the drive is
 *           mounted at another, and the title's root and fds follow;
 *   rest    the mount alone goes (the drive stays listed, with its id):
 *           back at once, nothing said;
 *   path    a file missing from a drive that answers (no wait);
 *   watch   the drive check's thread brings it back (plugged in again 6.5 s
 *           later), with no call failing, and notices the clock jumping 60 s
 *           (rest mode).
 * It prints whether the call is to be made again ("again"), the next 2
 * bytes read on the same fd ("read-after"), the notifications ("notify")
 * and the mounts.
 */
#include "ghost_api.h"
#include "usbstore.h"

long write(int fd, const void *buf, unsigned long n);
int vsnprintf(char *buf, unsigned long size, const char *fmt, __builtin_va_list ap);
void *calloc(unsigned long n, unsigned long size);
void free(void *p);
void exit(int code);
int hfs_stat(const char *path, int *dir, long *size, long *mtime);
int hfs_open(const char *path);
long hfs_read(int h, void *buf, unsigned long n);
long hfs_seek(int h, long off, int whence);
int hfs_close(int h);
void hfs_set_root(const char *root);
long hfs_image_read(int fd, void *buf, unsigned long n, long off);
int hfs_image_open(const char *path);
long hfs_image_size(int fd);
int hfs_real_open(const char *path, int flags, int mode);
long hfs_real_lseek(int fd, long off, int whence);
long hfs_real_read(int fd, void *buf, unsigned long n);
int hfs_real_dup(int fd);
int hfs_real_close(int fd);
int *hfs_errno(void);
int hfs_usleep(unsigned us);
long hfs_now_s(void);
int hfs_thread(void *(*fn)(void *), void *arg);

int vfmt(char *buf, size_t size, const char *f, __builtin_va_list ap) { return vsnprintf(buf, size, f, ap); }
int fmt(char *buf, size_t size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(buf, size, f, ap);
    __builtin_va_end(ap);
    return n;
}
static void say(const char *f, ...)
{
    char line[1200];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(line, sizeof(line) - 1, f, ap);
    __builtin_va_end(ap);
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    write(1, line, (unsigned long)n);
}
const char *strrchr_c(const char *s, int c)
{
    const char *r = NULL;
    for (; *s; s++)
        if (*s == c)
            r = s;
    return r;
}

/* ---- the storage stack's environment ---- */
void *us_alloc(u64 size) { return calloc(1, size ? size : 1); }
void us_free(void *p, u64 size) { free(p); }
void us_sleep_us(u32 us) {}
void us_log(const char *f, ...)
{
    char line[1000];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    vfmt(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    say("log: %s", line);
}
static int g_next_fd = 1000;
s32 us_placeholder_fd(void) { return g_next_fd++; }
void us_placeholder_close(s32 fd) {}

/* ---- the fake core ---- */
const struct ghost_api *G;
static struct ghost_api g_api;
static u64 g_ctx_mem[64]; /* the boot context ghostldr.lua allocates: profiles in its last u64 */
static struct boot_ctx *g_ctx_p = (struct boot_ctx *)g_ctx_mem;
#define g_ctx_v (*g_ctx_p)
static struct kfuncs g_k;
static const char *g_want, *g_map, *g_drive;
static int g_map_fail;
static char g_selected[17] = "-";
static const struct appfs_ops *g_fs;

static void f_logf(const char *f, ...)
{
    char line[1000];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    vfmt(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    say("log: %s", line);
}
static const char *f_requested(void) { return g_want; }
static void f_select(const char *t) { strlcpy(g_selected, t, sizeof(g_selected)); }
static int f_register(const struct appfs_ops *ops)
{
    g_fs = ops;
    say("registered %s", ops->name);
    return 0;
}
static void f_saves(const char *title, const char *prefix) { say("saves %s %sSAVE", title, prefix); }
static void *f_page_alloc(size_t n) { return calloc(1, n ? n : 1); }
static void f_page_free(void *p, size_t n) { free(p); }

/* libSceUsbStorage, as approot.c finds it on the console: drives (listed in
 * order) and their mounts in the sandbox */
static const char *g_other;              /* a second drive, listed first, without the library */
static int g_no_fde, g_inited;
static int g_hidden;                     /* --late: the drive is not plugged in yet */
struct fdrive {
    u32 id;
    const char *src; /* FSROOT-relative folder that is the drive */
    volatile int listed;
};
static struct fdrive g_drv[4];
static volatile int g_ndrv;
struct fmount {
    u32 id;
    char mp[64];
    const char *src;
    int dead; /* its drive went and the mount point stays taken (--live stale) */
};
static struct fmount g_mnt[4];
static volatile int g_nmnt;

static struct fdrive *drive_of(u32 id)
{
    for (int i = 0; i < g_ndrv; i++)
        if (g_drv[i].id == id)
            return &g_drv[i];
    return NULL;
}
static void drop_mount(int k)
{
    g_mnt[k] = g_mnt[--g_nmnt];
}
/* the drive is pulled out: no longer listed, its mounts gone (stale: they
 * stay, dead, their mount points taken) */
static void unplug_as(u32 id, int stale)
{
    struct fdrive *d = drive_of(id);
    if (d)
        d->listed = 0;
    for (int k = g_nmnt - 1; k >= 0; k--)
        if (g_mnt[k].id == id) {
            if (stale)
                g_mnt[k].dead = 1;
            else
                drop_mount(k);
        }
}
static void unplug(u32 id) { unplug_as(id, 0); }
static s32 us_init(void *p)
{
    if (g_inited) {                      /* the real library: 0x80F40002, and the session is gone */
        g_inited = 0;
        g_nmnt = 0;
        say("log: fake: second sceUsbStorageInit");
        return (s32)0x80F40002;
    }
    g_inited = 1;
    return 0;
}
static s32 us_list(u32 *ids, s32 *n)
{
    if (!g_inited)
        return (s32)0x80F40009;
    int k = 0;
    for (int i = 0; i < g_ndrv; i++)
        if (g_drv[i].listed && !(g_hidden && g_drv[i].id == 0x1001))
            ids[k++] = g_drv[i].id;
    *n = k;
    return 0;
}
static s32 us_map2(u32 id, const char *dir, const char *mp)
{
    if (!g_inited)
        return (s32)0x80F40009;
    if (g_map_fail)
        return (s32)0x80F40014;
    struct fdrive *d = drive_of(id);
    if (!d || !d->listed)
        return (s32)0x80F40001;
    if (strcmp(dir, "/") != 0)
        return (s32)0x80F40003; /* the harness only mounts the root */
    for (int k = 0; k < g_nmnt; k++)
        if (g_mnt[k].id == id || strcmp(g_mnt[k].mp, mp) == 0)
            return (s32)0x80F40003;
    struct fmount *m = &g_mnt[g_nmnt];
    m->dead = 0;
    m->id = id;
    strlcpy(m->mp, mp, sizeof(m->mp));
    m->src = d->src;
    g_nmnt++;
    say("log: fake: mounted %x at %s", id, mp);
    return 0;
}
static s32 us_unmap(u32 id, const char *dir)
{
    for (int k = 0; k < g_nmnt; k++)
        if (g_mnt[k].id == id && !g_mnt[k].dead) {
            drop_mount(k);
            say("log: fake: unmounted %x", id);
            return 0;
        }
    return (s32)0x80F40002;
}
static s32 us_info(u32 id, void *info)
{
    struct fdrive *d = drive_of(id);
    if (!d || !d->listed)
        return (s32)0x80F40001;
    u8 *b = info;
    *(u64 *)b = 64ull << 30;
    *(u64 *)(b + 8) = 32ull << 30;
    strlcpy((char *)b + 0x16, "Maker", 32);
    strlcpy((char *)b + 0x120, "Drive", 32);
    *(u32 *)(b + 0x330) = 0x80;
    return 0;
}
static void us_public_map(void) {} /* never called: its address places RequestMap2 */

/* the module's unwind table (eh_frame_hdr + one FDE): a function of 0x92
 * bytes at us_map2, which "RequestMap" (us_map2 + 0xA0) follows */
static u8 g_eh[64] __attribute__((aligned(8)));
static void make_eh(void)
{
    u64 base = (u64)g_eh, fde = base + 32, fn = (u64)us_map2;
    g_eh[0] = 1, g_eh[1] = 0x1b, g_eh[2] = 0x03, g_eh[3] = 0x3b;
    *(s32 *)(g_eh + 4) = 0;
    *(u32 *)(g_eh + 8) = 1;
    *(s32 *)(g_eh + 12) = (s32)((s64)fn - (s64)base);
    *(s32 *)(g_eh + 16) = (s32)(fde - base);
    *(u32 *)(g_eh + 32) = 16;                               /* length */
    *(u32 *)(g_eh + 36) = 0;                                /* CIE pointer */
    *(s32 *)(g_eh + 40) = (s32)((s64)fn - (s64)(fde + 8));  /* pc_begin, pcrel */
    *(u32 *)(g_eh + 44) = 0x92;                             /* pc_range */
}
static int f_module_info(u32 h, struct sce_module_info *mi)
{
    if (h != 0x77)
        return -1;
    mi->nsegs = 2;
    mi->segs[0].base = 0x1000, mi->segs[0].size = 0x10000, mi->segs[0].prot = 4; /* execute-only code */
    mi->segs[1].base = (u64)g_eh, mi->segs[1].size = sizeof(g_eh), mi->segs[1].prot = 1;
    return 0;
}
static int f_module_handle(const char *mod, u32 *out)
{
    if (!g_map || strcmp(mod, "libSceUsbStorage") != 0)
        return -1;
    *out = 0x77;
    return 0;
}
static void *libkernel(const char *name);
static void *f_dlsym(u32 h, const char *name)
{
    if (h == 0x55)
        return libkernel(name);
    if (h != 0x77)
        return NULL;
    if (!strcmp(name, "sceUsbStorageInit"))
        return (void *)us_init;
    if (!strcmp(name, "sceUsbStorageGetDeviceList"))
        return (void *)us_list;
    if (!strcmp(name, "sceUsbStorageGetDeviceInfo"))
        return (void *)us_info;
    if (!strcmp(name, "sceUsbStorageRequestUnmap"))
        return (void *)us_unmap;
    if (!strcmp(name, "sceUsbStorageRequestMap"))
        return g_no_fde ? (void *)us_public_map : (void *)((u64)us_map2 + 0xA0);
    return NULL;
}
/* SWRR's saved data: progress under the old default prefix of CUSA11111
 * (G11111SAVE, from before prefixes took the whole title ID) */
static int f_initial_user(int *u) { return *u = 0x1000, 0; }
static int f_dir_search(const void *cond, void *res)
{
    const char *dir = *(const char *const *)((const char *)cond + 16);
    unsigned hit = dir && strcmp(dir, "G11111SAVE") == 0;
    *(unsigned *)res = hit;
    return 0;
}
static void *f_host_real_in(const char *mod, const char *name)
{
    if (strcmp(name, "sceSaveDataDirNameSearch") == 0)
        return (void *)f_dir_search;
    if (strcmp(name, "sceUserServiceGetInitialUser") == 0)
        return (void *)f_initial_user;
    return NULL;
}

/* a path under a mount is its drive folder's */
static const char *mnt(const char *path, char *buf, size_t size)
{
    for (int k = 0; k < g_nmnt; k++) {
        size_t n = strlen(g_mnt[k].mp);
        if (g_mnt[k].dead)
            continue;
        if (strncmp(path, g_mnt[k].mp, n) == 0 && (path[n] == '/' || path[n] == 0)) {
            fmt(buf, size, "%s%s", g_mnt[k].src, path + n);
            return buf;
        }
    }
    return path;
}

/* the mount points in use, in mount order ("none") */
static const char *mounts(char *buf, size_t size)
{
    buf[0] = 0;
    for (int k = 0; k < g_nmnt; k++)
        fmt(buf + strlen(buf), size - strlen(buf), "%s%s", k ? " " : "", g_mnt[k].mp);
    return g_nmnt ? buf : "none";
}

/* ---- libkernel for usb_live.c: real fds on FSROOT's files ---- */
static int g_notify_n;
static int k_open(const char *path, int flags, int mode)
{
    char b[512];
    return hfs_real_open(mnt(path, b, sizeof(b)), flags, mode);
}
static s64 k_lseek(int fd, s64 off, int whence) { return hfs_real_lseek(fd, off, whence); }
static int k_dup(int fd) { return hfs_real_dup(fd); }
static int k_close(int fd) { return hfs_real_close(fd); }
static volatile long g_skew; /* the wall clock's jump (rest mode) */
static int k_gettimeofday(s64 *tv)
{
    tv[0] = hfs_now_s() + g_skew;
    tv[1] = 0;
    return 0;
}
static int k_notify(int dev, void *req, u64 size, int flags)
{
    g_notify_n++;
    say("notify %s", (const char *)req + 0x2D);
    return size == 0xC30 ? 0 : -1;
}
static void *libkernel(const char *name)
{
    if (!strcmp(name, "dup"))
        return (void *)k_dup;
    if (!strcmp(name, "lseek"))
        return (void *)k_lseek;
    if (!strcmp(name, "open"))
        return (void *)k_open;
    if (!strcmp(name, "close"))
        return (void *)k_close;
    if (!strcmp(name, "sceKernelGettimeofday"))
        return (void *)k_gettimeofday;
    if (!strcmp(name, "sceKernelSendNotificationRequest"))
        return (void *)k_notify;
    return NULL;
}
static int k_usleep(u32 us) { return hfs_usleep(us); }
static int k_attr(void *a) { return 0; }
static int k_attr_stack(void *a, size_t n) { return 0; }
static int k_thread(void *thr, const void *attr, void *(*fn)(void *), void *arg, const char *name)
{
    return hfs_thread(fn, arg);
}
/* the overlay's banner (dlg_overlay.c): nothing drawable here */
void ov_banner(const char *text, u32 seconds) {}
/* the failure display's message (failure.c, rel-diag): as a notification */
void diag_notice(const char *text, u32 seconds)
{
    g_notify_n++;
    say("notify ghostldr: %s", text);
}
/* the FTP server's check after rest mode (ftp_sys.c) */
void ftp_after_resume(void) { say("log: ftp: after rest mode"); }

/* the console's files: a provider's (route B), else the host directory */
#define HOST_FD 5000
int hfs_stat_fbsd(const char *path, void *st);
int hfs_getdents(int h, void *buf, int size);

static int v_open(const char *path, int flags, int mode)
{
    int fd;
    if (g_fs && g_fs->open(path, flags, &fd) == 0)
        return fd;
    char b[512];
    int h = hfs_open(mnt(path, b, sizeof(b)));
    return h < 0 ? (int)0x80020002 : HOST_FD + h;
}
static s64 v_read(int fd, void *buf, u64 n)
{
    if (g_fs && g_fs->owns(fd))
        return g_fs->read(fd, buf, n);
    return hfs_read(fd - HOST_FD, buf, n);
}
static s64 v_lseek(int fd, s64 off, int whence)
{
    if (g_fs && g_fs->owns(fd))
        return g_fs->lseek(fd, off, whence);
    return hfs_seek(fd - HOST_FD, off, whence);
}
static int v_close(int fd)
{
    if (g_fs && g_fs->close(fd) == 0)
        return 0;
    return hfs_close(fd - HOST_FD);
}
static int v_stat(const char *path, void *st)
{
    int rc;
    if (g_fs && g_fs->stat(path, st, &rc) == 0)
        return rc;
    char b[512];
    return hfs_stat_fbsd(mnt(path, b, sizeof(b)), st);
}
static int v_getdents(int fd, void *buf, int size)
{
    if (g_fs && g_fs->owns(fd))
        return g_fs->getdents(fd, buf, size);
    return hfs_getdents(fd - HOST_FD, buf, size);
}

#ifdef GHOST_USB_RAW
/* route B: the drive is a disk image, read raw */
static int g_img = -1;
static int img_read(void *ctx, u64 lba, u32 count, void *buf)
{
    return hfs_image_read(g_img, buf, (u64)count * 512, (long)(lba * 512)) == (long)count * 512 ? 0 : US_EIO;
}
const struct appfs_ops *usb_attach(u32 flags)
{
    static struct blk b;
    if (!g_drive || (g_img = hfs_image_open(g_drive)) < 0) {
        say("log: usb: no drive");
        return NULL;
    }
    struct blkdev dev = {NULL, 512, (u64)hfs_image_size(g_img) / 512, 2048, img_read};
    if (blk_init(&b, &dev, 16 << 10, 64, 192, 32) != 0 || usbfs_mount(&b, -1) != 0)
        return NULL;
    return usbfs_ops();
}
#endif

/* the early entry's save scan (ext/src/save_prescan.c): not exercised here */
int save_prescan(u32 user, int (*progress)(void *arg, u32 done, u32 total), void *arg) { return -1; }

extern const struct ext_early g_approot_early;
bool usb_session_open(void);
void usb_ftp_prepare(void);
const char *usb_ftp_home(void);
static void f_spin(volatile int *l)
{
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        hfs_usleep(100);
}
static void f_unspin(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

/* usb_live.c */
void usb_live_open(s32 fd, const char *path, int flags);
void usb_live_moved(s32 fd, s64 n, bool to);
bool usb_live_fd_failed(s32 fd, s32 e);
bool usb_live_path_failed(const char *path, s32 e);
void usb_live_start(void);
bool usb_live_waiting(void);
u32 usb_live_gen(void);

/* --live: the drive comes back after a while (a thread) */
static volatile u32 g_back_id;
static volatile int g_back_ms, g_wrong_ms;
static void *plug_later(void *arg)
{
    if (g_wrong_ms) { /* a drive without the game first */
        hfs_usleep((unsigned)g_wrong_ms * 1000);
        g_drv[g_ndrv] = (struct fdrive){0x1003, "/data/empty", 1};
        g_ndrv++;
        say("log: fake: a drive without the game plugged in (1003)");
    }
    hfs_usleep((unsigned)g_back_ms * 1000);
    struct fdrive *d = drive_of(0x1001);
    if (g_back_id != 0x1001) /* plugged in again: a new id */
        g_drv[g_ndrv++] = (struct fdrive){g_back_id, d->src, 1};
    else
        d->listed = 1;
    say("log: fake: the drive is back (%x)", g_back_id);
    return arg;
}

static void live(const char *scenario, const char *root)
{
    char p[512], b[8] = {0}, m[256];
    fmt(p, sizeof(p), "%s/eboot.bin", root);
    int hole = k_open(p, 0, 0); /* a free number below the title's fd: reopening fills it and gives it back */
    int fd = k_open(p, 0, 0);
    k_close(hole);
    usb_live_open(fd, p, 0);
    long got = hfs_real_read(fd, b, 2);
    usb_live_moved(fd, got, false); /* as hle_file.c does */
    say("opened fd %d (%ld bytes read)", fd, got);
    int again = -1;
    if (!strcmp(scenario, "unplug") || !strcmp(scenario, "wrong") || !strcmp(scenario, "closed") ||
        !strcmp(scenario, "stale")) {
        unplug_as(0x1001, !strcmp(scenario, "stale"));
        if (!strcmp(scenario, "closed")) /* the system closed the fd: its offset is only known from the calls */
            hfs_real_close(fd);
        g_wrong_ms = !strcmp(scenario, "wrong") ? 100 : 0;
        g_back_id = 0x1002, g_back_ms = g_wrong_ms ? 4500 : 3800; /* past the 3 s before anything is said */
        hfs_thread(plug_later, NULL);
        again = usb_live_fd_failed(fd, !strcmp(scenario, "closed") ? 9 : 5);
    } else if (!strcmp(scenario, "rest")) {
        for (int k = g_nmnt - 1; k >= 0; k--) /* the mount alone */
            if (g_mnt[k].id == 0x1001)
                drop_mount(k);
        again = usb_live_fd_failed(fd, 5);
    } else if (!strcmp(scenario, "path")) {
        fmt(p, sizeof(p), "%s/missing.bin", root);
        again = usb_live_path_failed(p, 2);
        say("path-other %d", usb_live_path_failed("/data/x", 2));
    } else if (!strcmp(scenario, "watch")) {
        usb_live_start();
        hfs_usleep(300000);
        g_skew = 60; /* rest mode */
        unplug(0x1001);
        g_back_id = 0x1002, g_back_ms = 6500;
        hfs_thread(plug_later, NULL);
        for (int t = 0; t < 150 && usb_live_gen() == 0; t++)
            hfs_usleep(100000);
        say("waiting %d", usb_live_waiting());
    }
    say("again %d", again);
    say("gen %u", usb_live_gen());
    memset(b, 0, sizeof(b));
    got = hfs_real_read(fd, b, 2);
    say("read-after %ld %.2s", got, b);
    say("notifications %d", g_notify_n);
    say("live-mount %s", mounts(m, sizeof(m)));
    say("live-root %s", (const char *)g_ctx_v.app_root);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        say("usage: approot_host FSROOT APP_ROOT [options]");
        return 2;
    }
    hfs_set_root(argv[1]);
    const char *root = argv[2], *pick = NULL, *profiles = NULL;
    int ftp = 0, late = 0, pulled = 0, pick_at = -1;
    const char *scenario = NULL;
    u64 flags = 0;
    for (int i = 3; i < argc; i++) {
        const char *o = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(o, "--title") && v)
            g_want = v, i++;
        else if (!strcmp(o, "--select"))
            flags |= CTX_FLAG_SELECT;
        else if (!strcmp(o, "--other") && v)
            g_other = v, i++;
        else if (!strcmp(o, "--no-fde"))
            g_no_fde = 1;
        else if (!strcmp(o, "--inited"))
            g_inited = 1;
        else if (!strcmp(o, "--ftp"))
            ftp = 1;
        else if (!strcmp(o, "--late"))
            late = g_hidden = 1;
        else if (!strcmp(o, "--unplug"))
            pulled = 1;
        else if (!strcmp(o, "--live") && v)
            scenario = v, i++;
        else if (!strcmp(o, "--map") && v)
            g_map = v, i++;
        else if (!strcmp(o, "--map-fail"))
            g_map_fail = 1;
        else if (!strcmp(o, "--drive") && v)
            g_drive = v, i++;
        else if (!strcmp(o, "--pick") && v)
            pick = v, i++;
        else if (!strcmp(o, "--pick-at") && v)
            pick_at = (int)(v[0] - '0'), pick = "", i++;
        else if (!strcmp(o, "--profiles") && v)
            profiles = v, i++;
        else {
            say("unknown option %s", o);
            return 2;
        }
    }
    if (g_other)
        g_drv[g_ndrv++] = (struct fdrive){0x1000, g_other, 1};
    if (g_map)
        g_drv[g_ndrv++] = (struct fdrive){0x1001, g_map, 1};
    g_ctx_v.size = sizeof(struct boot_ctx);
    g_ctx_v.libkernel_handle = 0x55;
    g_ctx_v.app_root = (u64)root;
    g_ctx_v.flags = flags;
    g_api.version = GHOST_API_VERSION;
    g_api.size = sizeof(g_api);
    g_api.ctx = &g_ctx_p;
    g_api.k = &g_k;
    g_api.logf = f_logf;
    g_api.host_real_in = f_host_real_in;
    g_api.host_module_handle = f_module_handle;
    g_api.host_dlsym = f_dlsym;
    g_k.module_info = f_module_info;
    g_k.usleep = k_usleep;
    g_k.errno_fn = hfs_errno;
    g_k.pthread_attr_init = k_attr;
    g_k.pthread_attr_destroy = k_attr;
    g_k.pthread_attr_setstacksize = k_attr_stack;
    g_k.pthread_create = k_thread;
    make_eh();
    g_api.page_alloc = f_page_alloc;
    g_api.page_free = f_page_free;
    g_api.vfs_open = v_open;
    g_api.vfs_read = v_read;
    g_api.vfs_lseek = v_lseek;
    g_api.vfs_close = v_close;
    g_api.vfs_stat = v_stat;
    g_api.vfs_getdents = v_getdents;
    g_api.profile_requested = f_requested;
    g_api.profile_select = f_select;
    g_api.appfs_register = f_register;
    g_api.gfs_use_saves = f_saves;
    g_api.spin_lock = f_spin;
    g_api.spin_unlock = f_unspin;
    G = &g_api;
    if (profiles) { /* a bundle file, read whole (a path on this machine, not under FSROOT) */
        static u8 bundle[1 << 16];
        hfs_set_root("");
        int h = hfs_open(profiles);
        long got = h >= 0 ? hfs_read(h, bundle, sizeof(bundle)) : -1;
        if (h >= 0)
            hfs_close(h);
        hfs_set_root(argv[1]);
        if (got <= 0)
            return 4;
        g_ctx_mem[63] = (u64)bundle;
    }
    const struct ext_early *e = &g_approot_early;
    if (e->magic != EXT_EARLY_MAGIC || e->size != sizeof(*e))
        return 3;
    const char *r = root;
    int rc = e->approot_init(&r);
    say("init %d root=%s app_root=%s library=%d", rc, r, (const char *)g_ctx_v.app_root,
        (g_ctx_v.flags & CTX_FLAG_LIBRARY) != 0);
    const struct lib_title *list = NULL;
    int n = e->library(&list);
    for (int i = 0; i < n; i++)
        say("lib %d\t%s\t%u\t%s\t%.*s\t%s", i, list[i].id, list[i].flags, list[i].path, (int)list[i].dir_len,
            list[i].path, list[i].note);
    if (rc == 1 && pick) {
        int at = pick_at;
        for (int i = 0; i < n && at < 0 && pick[0] != '-'; i++)
            if (!strcmp(list[i].id, pick))
                at = i;
        const char *t = at >= 0 && at < n ? list[at].id : pick[0] == '-' ? NULL : pick;
        rc = e->approot_pick(t, at, &r);
        say("pick %d root=%s app_root=%s library=%d", rc, r, (const char *)g_ctx_v.app_root,
            (g_ctx_v.flags & CTX_FLAG_LIBRARY) != 0);
        say("after-pick %d", e->library(&list));
    }
    say("profile %s", g_selected);
    if (rc == 0) {
        char p[512];
        u8 st[0x78];
        fmt(p, sizeof(p), "%s/eboot.bin", r);
        int s = v_stat(p, st);
        say("eboot %s %ld", s == 0 ? "found" : "missing", s == 0 ? *(s64 *)(st + 0x48) : -1L);
    }
    char mb[256];
    say("mount %s", mounts(mb, sizeof(mb)));
    if (ftp) {
        usb_ftp_prepare();
        say("prepared %s", mounts(mb, sizeof(mb)));
        if (late)
            g_hidden = 0;
        if (pulled)
            g_nmnt = 0;
        say("home %s", usb_ftp_home());
        say("mount-after %s", mounts(mb, sizeof(mb)));
    }
    if (scenario && rc == 0)
        live(scenario, r);
    say("session %d", usb_session_open());
    say("session-flag %d", (g_ctx_v.flags & CTX_FLAG_USB_SESSION) != 0); /* what outlives ext_unplace */
    return 0;
}
