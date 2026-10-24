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
 *                  [--map-fail] [--no-fde] [--inited] [--drive IMAGE] [--pick T] [--ftp [--late|--unplug]]
 *
 * Prints what approot_init (and approot_pick) decided, the profile picked,
 * where saves go, whether the title's eboot.bin is reachable and the
 * mount's state at the end.  --ftp then does what the FTP server does
 * (ftp_sys.c): usb_ftp_prepare as the title starts, usb_ftp_home as a
 * client logs in; --late plugs the drive in between the two, --unplug
 * pulls it out.  It prints whether the title's own Init and Term are kept
 * off the session (usb_session_open) and where a client starts.
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
static struct boot_ctx g_ctx_v, *g_ctx_p = &g_ctx_v;
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

/* libSceUsbStorage, as approot.c finds it on the console */
static const char *g_other;              /* a second drive, listed first, without the library */
static int g_no_fde, g_inited, g_mounted_id;
static int g_hidden;                     /* --late: the drive is not plugged in yet */
static char g_mount_at[64];              /* the sandbox path the drive is mounted at */
static const char *g_mount_src;          /* FSROOT-relative folder that is the mounted drive */
static s32 us_init(void *p)
{
    if (g_inited) {                      /* the real library: 0x80F40002, and the session is gone */
        g_inited = 0;
        g_mounted_id = 0;
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
    if (g_other)
        ids[k++] = 0x1000;
    if (g_map && !g_hidden)
        ids[k++] = 0x1001;
    *n = k;
    return 0;
}
static s32 us_map2(u32 id, const char *dir, const char *mp)
{
    if (!g_inited)
        return (s32)0x80F40009;
    if (g_map_fail)
        return (s32)0x80F40014;
    if (strcmp(dir, "/") != 0 || g_mounted_id)
        return (s32)0x80F40003; /* the harness only mounts the root, once */
    g_mounted_id = (int)id;
    strlcpy(g_mount_at, mp, sizeof(g_mount_at));
    g_mount_src = id == 0x1000 ? g_other : g_map;
    say("log: fake: mounted %x at %s", id, mp);
    return 0;
}
static s32 us_unmap(u32 id, const char *dir)
{
    if (!g_mounted_id || (u32)g_mounted_id != id)
        return (s32)0x80F40002;
    say("log: fake: unmounted %x", id);
    g_mounted_id = 0;
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
static void *f_dlsym(u32 h, const char *name)
{
    if (h != 0x77)
        return NULL;
    if (!strcmp(name, "sceUsbStorageInit"))
        return (void *)us_init;
    if (!strcmp(name, "sceUsbStorageGetDeviceList"))
        return (void *)us_list;
    if (!strcmp(name, "sceUsbStorageRequestUnmap"))
        return (void *)us_unmap;
    if (!strcmp(name, "sceUsbStorageRequestMap"))
        return g_no_fde ? (void *)us_public_map : (void *)((u64)us_map2 + 0xA0);
    return NULL;
}
static void *f_host_real_in(const char *mod, const char *name) { return NULL; }

/* a path under the mount is the drive folder's */
static const char *mnt(const char *path, char *buf, size_t size)
{
    size_t n = strlen(g_mount_at);
    if (g_mounted_id && n && strncmp(path, g_mount_at, n) == 0 && (path[n] == '/' || path[n] == 0)) {
        fmt(buf, size, "%s%s", g_mount_src, path + n);
        return buf;
    }
    return path;
}

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

extern const struct ext_early g_approot_early;
bool usb_session_open(void);
void usb_ftp_prepare(void);
const char *usb_ftp_home(void);
static void f_spin(volatile int *l) {}

int main(int argc, char **argv)
{
    if (argc < 3) {
        say("usage: approot_host FSROOT APP_ROOT [options]");
        return 2;
    }
    hfs_set_root(argv[1]);
    const char *root = argv[2], *pick = NULL;
    int ftp = 0, late = 0, unplug = 0;
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
            unplug = 1;
        else if (!strcmp(o, "--map") && v)
            g_map = v, i++;
        else if (!strcmp(o, "--map-fail"))
            g_map_fail = 1;
        else if (!strcmp(o, "--drive") && v)
            g_drive = v, i++;
        else if (!strcmp(o, "--pick") && v)
            pick = v, i++;
        else {
            say("unknown option %s", o);
            return 2;
        }
    }
    g_ctx_v.size = sizeof(g_ctx_v);
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
    g_api.spin_unlock = f_spin;
    G = &g_api;
    const struct ext_early *e = &g_approot_early;
    if (e->magic != EXT_EARLY_MAGIC || e->size != sizeof(*e))
        return 3;
    const char *r = root;
    int rc = e->approot_init(&r);
    say("init %d root=%s app_root=%s library=%d", rc, r, (const char *)g_ctx_v.app_root,
        (g_ctx_v.flags & CTX_FLAG_LIBRARY) != 0);
    if (rc == 1 && pick) {
        rc = e->approot_pick(pick[0] == '-' ? NULL : pick, &r);
        say("pick %d root=%s app_root=%s library=%d", rc, r, (const char *)g_ctx_v.app_root,
            (g_ctx_v.flags & CTX_FLAG_LIBRARY) != 0);
    }
    say("profile %s", g_selected);
    if (rc == 0) {
        char p[512];
        u8 st[0x78];
        fmt(p, sizeof(p), "%s/eboot.bin", r);
        int s = v_stat(p, st);
        say("eboot %s %ld", s == 0 ? "found" : "missing", s == 0 ? *(s64 *)(st + 0x48) : -1L);
    }
    say("mount %s", g_mounted_id ? g_mount_at : "none");
    if (ftp) {
        usb_ftp_prepare();
        say("prepared %s", g_mounted_id ? g_mount_at : "none");
        if (late)
            g_hidden = 0;
        if (unplug)
            g_mounted_id = 0;
        say("home %s", usb_ftp_home());
        say("mount-after %s", g_mounted_id ? g_mount_at : "none");
    }
    say("session %d", usb_session_open());
    return 0;
}
