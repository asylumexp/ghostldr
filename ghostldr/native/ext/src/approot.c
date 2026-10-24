/*
 * App roots on USB drives and folders of titles (docs/usb.md), decided
 * before the title's modules load: the core places the extension early and
 * calls approot_init through ext_early (ext_main.c) for an app root that is
 * "usb:..." or a directory without an eboot.bin.  A title's own directory
 * (run.py --app-root /data/.../app0) never comes here.
 *
 *   usb:/path    a path on a USB drive.  ShellCore's AutoMounter mounts the
 *     drive into SWRR's sandbox (usb_map, below) and the path is a
 *     directory under the mount, read with the kernel's own file calls like
 *     a dev setup's.  /app0 is kept read-only (usb_app0_ro, hle_file.c).
 *     A build with USB_RAW=1 adds route B as a fallback: the extension
 *     drives the drive itself and reads its exFAT or FAT32 volume
 *     (usb_sce.c, usbfs.c), served like ghostfs's.  It is off by default:
 *     SWRR's libSceUsbd lists no devices on a 13.60 PS5.
 *
 * Either may hold one title (eboot.bin in it) or a library: a folder of
 * title folders named by title ID, <TITLE>/app0/eboot.bin, each with an
 * optional <TITLE>/prefix (its save container's prefix) and
 * <TITLE>/meta.bin (its name and art for the selector), as gfs_pack.py
 * --usb lays them out.  The title is run.py --title's, else the only one,
 * else the game selector's (CTX_FLAG_LIBRARY tells it app_root is a
 * library; boot.c then calls approot_pick).
 *
 * A title from a USB drive keeps its progress in SWRR's save containers as
 * a ghostfs title does (<prefix>SAVE, gfs_use_saves): the drive is only
 * read.  A dev library's title keeps its saves beside it, as before.
 */
#include "ext.h"
#include "usbstore.h"

static char g_lib[256];  /* the library a title is chosen from */
static char g_root[256]; /* the chosen title's app0: g_ctx->app_root from then on */
static bool g_usb;       /* on a USB drive */
#ifdef GHOST_USB_RAW
static const struct appfs_ops *g_raw; /* route B's provider (usbfs) */
#endif

/* ---- the drive, mounted into the sandbox by ShellCore's AutoMounter ----
 *
 * libSceUsbStorage is AutoMounter's client.  Its public
 * sceUsbStorageRequestMap is refused for SWRR (0x80F40014: ShellCore's
 * sceProcessStarter::CheckUsbAccessDir checks the directory against a list
 * in the app's param.sfo, and SWRR's has none).  The library's unexported
 * _sceUsbStorageRequestMap2(id, dir, mount point) sends debug data that
 * AutoMounter takes as "skipParamSfoCheck": the drive's dir is then
 * nullfs-mounted at mount point in SWRR's sandbox (read-write).  Checked on
 * a stock 13.60 PS5.
 *
 *   - The function sits 0xA0 bytes before sceUsbStorageRequestMap (0x90 in
 *     later PS4 builds of the library).  The library's code is
 *     execute-only, so the one check possible is the module's unwind table
 *     (eh_frame_hdr, in a readable segment): a function must start exactly
 *     there and end before RequestMap.
 *   - sceUsbStorageInit once per process: a second one answers 0x80F40002
 *     and leaves the library uninitialized, its mounts gone.  So
 *     GetDeviceList comes first (0x80F40009 until Init).  sceUsbStorageTerm,
 *     or SWRR exiting, ends the session and its mounts: the session stays
 *     open (a title's own Init and Term are kept off it: hle_usbstorage.c).
 *   - Device ids change with every boot and plug: always from the list.
 *   - One mount per directory: "/" again, or a folder inside a mounted one,
 *     is a conflict (0x80F40003).  The drive's "/" is mounted once. */
#define USB_MOUNT "/ghostldr_usb" /* in SWRR's sandbox */
#define USBS_NOT_INIT ((s32)0x80F40009)

static struct {
    s32 (*init)(void *);
    s32 (*list)(u32 *ids, s32 *count);
    s32 (*map2)(u32 id, const char *dir, const char *mount_point);
    s32 (*unmap)(u32 id, const char *dir);
    u32 id;      /* the mounted drive's */
    bool mounted;
} U;

/* the length of the function starting at addr, from the unwind table of the
 * module handle; 0 when no function starts there */
static u64 fn_size_at(u32 handle, u64 addr)
{
    struct sce_module_info mi;
    memset(&mi, 0, sizeof(mi));
    mi.size = sizeof(mi);
    if (!K.module_info || K.module_info(handle, &mi) != 0)
        return 0;
    for (u32 s = 0; s < mi.nsegs && s < 4; s++) {
        const u8 *h = (const u8 *)mi.segs[s].base;
        u64 size = mi.segs[s].size;
        /* readable, not executable; version 1, eh_frame_ptr pcrel sdata4,
         * fde_count udata4, table datarel sdata4 */
        if ((mi.segs[s].prot & 4) || !(mi.segs[s].prot & 1) || size < 16 || h[0] != 1 || h[1] != 0x1b ||
            h[2] != 0x03 || h[3] != 0x3b)
            continue;
        u32 n = *(const u32 *)(h + 8);
        const s32 *t = (const s32 *)(h + 12);
        for (u32 i = 0; i < n && 12 + 8 * ((u64)i + 1) <= size; i++) {
            if ((u64)h + (s64)t[2 * i] != addr)
                continue;
            const u8 *fde = h + t[2 * i + 1]; /* length, CIE pointer, pc_begin (pcrel sdata4), pc_range */
            return (u64)(fde + 8) + *(const s32 *)(fde + 8) == addr ? *(const u32 *)(fde + 12) : 0;
        }
    }
    return 0;
}

static int usb_lib(void)
{
    u32 h = 0;
    if (U.map2)
        return 0;
    if (host_module_handle("libSceUsbStorage", &h) != 0) {
        logf("approot: libSceUsbStorage is not loadable");
        return -1;
    }
    U.init = host_dlsym(h, "sceUsbStorageInit");
    U.list = host_dlsym(h, "sceUsbStorageGetDeviceList");
    U.unmap = host_dlsym(h, "sceUsbStorageRequestUnmap");
    u64 rm = (u64)host_dlsym(h, "sceUsbStorageRequestMap");
    static const u32 k_back[] = {0xa0, 0x90};
    for (u32 i = 0; rm && !U.map2 && i < ARRAY_SIZE(k_back); i++) {
        u64 len = fn_size_at(h, rm - k_back[i]);
        if (len && len <= k_back[i])
            U.map2 = (void *)(rm - k_back[i]);
    }
    logf("approot: libSceUsbStorage %u: RequestMap %lx, RequestMap2 %p", h, rm, (void *)U.map2);
    if (!U.init || !U.list || !U.unmap || !U.map2) {
        logf("approot: libSceUsbStorage lacks what mounting needs (docs/usb.md)");
        return -1;
    }
    return 0;
}

static bool is_dir(const char *path)
{
    u8 st[0x78];
    return vfs_stat(path, st) == 0 && (*(u16 *)(st + 8) & 0xF000) == 0x4000;
}

static void usb_unmount(void)
{
    if (!U.mounted)
        return;
    char root[2] = "/";
    s32 rc = U.unmap(U.id, root);
    logf("approot: unmounted the USB drive %x from %s (%x)", U.id, USB_MOUNT, rc);
    U.mounted = false;
}

/* Mount the drive whose root holds rel ("ghostldr", "" for any drive) at
 * USB_MOUNT: 0, or -1 with the reason logged */
static int usb_mount(const char *rel)
{
    if (usb_lib() != 0)
        return -1;
    u32 ids[16];
    s32 n = 0;
    memset(ids, 0, sizeof(ids));
    s32 rc = U.list(ids, &n);
    if (rc == USBS_NOT_INIT) {
        rc = U.init(NULL);
        logf("approot: sceUsbStorageInit -> %x", rc);
        if (rc == 0)
            rc = U.list(ids, &n);
    }
    logf("approot: sceUsbStorageGetDeviceList -> %x, %d drive(s)", rc, n);
    if (rc != 0 || n <= 0) {
        logf("approot: no USB drive%s", rc ? " (libSceUsbStorage refused)" : ": plug one in");
        return -1;
    }
    /* the kernel and the IPC read these: not from the extension's code pages */
    char root[2] = "/", mp[sizeof(USB_MOUNT)] = USB_MOUNT, want[320];
    fmt(want, sizeof(want), "%s/%s", mp, rel);
    for (s32 i = 0; i < n && i < (s32)ARRAY_SIZE(ids); i++) {
        U.unmap(ids[i], root); /* a mount left by an earlier boot of this SWRR: 0x80F40002 when none */
        rc = U.map2(ids[i], root, mp);
        bool has = rc == 0 && is_dir(want);
        logf("approot: USB drive %x mounted at %s: %x; %s %s", ids[i], mp, rc, want, rc ? "-" : has ? "found" : "missing");
        if (has) {
            U.id = ids[i];
            U.mounted = true;
            return 0;
        }
        if (rc == 0)
            U.unmap(ids[i], root);
    }
    logf("approot: no USB drive has /%s", rel);
    return -1;
}

/* hle_file.c: a write to the title's files on a USB drive is refused, as
 * the PS4 refuses one to /app0 */
HIDDEN bool usb_app0_ro(const char *path)
{
    if (!U.mounted || !path)
        return false;
    if (strncmp(path, "/app0", 5) == 0 && (path[5] == '/' || path[5] == 0))
        return true;
    size_t n = sizeof(USB_MOUNT) - 1;
    return strncmp(path, USB_MOUNT, n) == 0 && (path[n] == '/' || path[n] == 0);
}

/* hle_usbstorage.c: the session this file opened, which the title must not end */
HIDDEN bool usb_session_open(void) { return U.mounted; }

/* the drive's directory for path ("/ghostldr" -> "/ghostldr_usb/ghostldr", or route B's "/usb0/ghostldr") */
static int usb_open(const char *path, char *out, size_t size)
{
    char base[64];
    while (*path == '/')
        path++;
    char top[64];
    size_t k = 0;
    while (path[k] && path[k] != '/' && k < sizeof(top) - 1)
        top[k] = path[k], k++;
    top[k] = 0;
    if (usb_mount(top) == 0) {
        strlcpy(base, USB_MOUNT, sizeof(base));
    }
#ifdef GHOST_USB_RAW
    else if ((g_raw = usb_attach((u32)g_ctx->flags)) && appfs_register(g_raw) == 0) {
        strlcpy(base, "/usb0", sizeof(base));
        logf("approot: the extension reads the USB drive itself (route B)");
    }
#endif
    else {
        return -1;
    }
    g_usb = true;
    fmt(out, size, *path ? "%s/%s" : "%s", base, path);
    return 0;
}

/* a boot that stops: the drive goes back */
static int usb_fail(void)
{
    usb_unmount();
    return -1;
}

static bool is_file(const char *dir, const char *name)
{
    char p[320];
    u8 st[0x78];
    fmt(p, sizeof(p), "%s/%s", dir, name);
    return vfs_stat(p, st) == 0 && (*(u16 *)(st + 8) & 0xF000) == 0x8000;
}

static bool title_name(const char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-'))
            return false;
    return n >= 1 && n <= 15;
}

/* up to cap bytes of dir/name: bytes read, or < 0 */
static s64 read_small(const char *dir, const char *name, void *buf, u64 cap)
{
    char p[320];
    fmt(p, sizeof(p), "%s/%s", dir, name);
    int fd = vfs_open(p, O_RDONLY, 0);
    if (fd < 0)
        return fd;
    u64 got = 0;
    s64 r;
    while (got < cap && (r = vfs_read(fd, (u8 *)buf + got, cap - got)) > 0)
        got += (u64)r;
    vfs_close(fd);
    return (s64)got;
}

static void sfo_find(const u8 *sfo, u32 n, char out[16]);

/* param.sfo's TITLE_ID */
static void sfo_title(const char *app0, char out[16])
{
    const u64 cap = 0x10000;
    u8 *sfo = page_alloc(cap);
    out[0] = 0;
    s64 n = sfo ? read_small(app0, "sce_sys/param.sfo", sfo, cap) : -1;
    if (n >= 20 && memcmp(sfo, "\0PSF", 4) == 0)
        sfo_find(sfo, (u32)n, out);
    page_free(sfo, cap);
}

static void sfo_find(const u8 *sfo, u32 n, char out[16])
{
    u32 keys = *(u32 *)(sfo + 8), data = *(u32 *)(sfo + 12), count = *(u32 *)(sfo + 16);
    for (u32 i = 0; i < count && 20 + 16 * (i + 1) <= n; i++) {
        const u8 *e = sfo + 20 + 16 * i;
        u64 key = (u64)keys + *(u16 *)e, len = *(u32 *)(e + 4), off = (u64)data + *(u32 *)(e + 12);
        if (key + 9 <= n && memcmp(sfo + key, "TITLE_ID", 9) == 0 && off + len <= n && len) {
            u32 k = 0;
            while (k < len && k < 15 && sfo[off + k])
                out[k] = (char)sfo[off + k], k++;
            out[k] = 0;
            if (!title_name(out))
                out[0] = 0;
            return;
        }
    }
}

/* the save container's prefix: <TITLE>/prefix, else gfs_pack.py's default
 * (G and a CUSA-style ID's digits, else G and its first 11 letters and digits) */
static void title_prefix(const char *dir, const char *title, char prefix[16])
{
    char b[16];
    s64 n = read_small(dir, "prefix", b, sizeof(b) - 1);
    u32 k = 0;
    while (n > 0 && k < (u32)n && k < 12 && ((b[k] >= 'A' && b[k] <= 'Z') || (b[k] >= '0' && b[k] <= '9')))
        prefix[k] = b[k], k++;
    prefix[k] = 0;
    if (k && strncmp(prefix, "GFS", 3) != 0)
        return;
    bool cusa = strlen(title) == 9;
    for (u32 i = 0; i < 9 && cusa; i++)
        cusa = i < 4 ? title[i] >= 'A' && title[i] <= 'Z' : title[i] >= '0' && title[i] <= '9';
    k = 1;
    prefix[0] = 'G';
    for (const char *c = cusa ? title + 4 : title; *c && k < 12; c++)
        if ((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9'))
            prefix[k++] = *c;
    prefix[k] = 0;
}

/* the title folders in lib: how many, and the first */
static u32 lib_scan(const char *lib, char first[16])
{
    int fd = vfs_open(lib, O_RDONLY, 0);
    if (fd < 0)
        return 0;
    u8 buf[2048];
    int got;
    u32 n = 0;
    while ((got = vfs_getdents(fd, buf, sizeof(buf))) > 0)
        for (int off = 0; off + 8 < got;) {
            const u8 *d = buf + off;
            u16 reclen = *(const u16 *)(d + 4);
            if (!reclen)
                break;
            const char *name = (const char *)d + 8;
            char app0[320];
            fmt(app0, sizeof(app0), "%s/%s/app0", lib, name);
            if ((d[6] == 4 || d[6] == 0) && title_name(name) && is_file(app0, "eboot.bin")) {
                if (!n)
                    strlcpy(first, name, 16);
                n++;
            }
            off += reclen;
        }
    vfs_close(fd);
    return n;
}

/* serve the title in app0; dir: its folder (with its prefix file) */
static int use_title(const char *title, const char *app0, const char *dir)
{
    char sfo_id[16];
    if (!title || !title[0]) {
        sfo_title(app0, sfo_id);
        title = sfo_id[0] ? sfo_id : NULL;
    }
    if (g_usb && !title) {
        logf("approot: %s: no title ID (not in a <TITLE>/app0 folder, no param.sfo TITLE_ID): pass run.py --title",
             app0);
        return -1;
    }
#ifdef GHOST_USB_RAW
    if (g_raw) {
        if (g_raw->set_root(app0 + 5) != 0) /* "/usb0/ghostldr/..." -> "/ghostldr/..." */
            return -1;
        strlcpy(g_root, "/app0", sizeof(g_root));
    } else
#endif
        strlcpy(g_root, app0, sizeof(g_root));
    g_ctx->app_root = (u64)g_root;
    if (title)
        profile_select(title);
    if (g_usb) {
        char prefix[16];
        title_prefix(dir, title, prefix);
        gfs_use_saves(title, prefix);
    }
    logf("approot: %s from %s", title ? title : "the title", app0);
    return 0;
}

static int approot_pick(const char *title, const char **root)
{
    g_ctx->flags &= ~(u64)CTX_FLAG_LIBRARY;
    if (!title || !title[0])
        title = profile_requested();
    if (!title) {
        logf("approot: no title chosen from %s: pick one with run.py boot --title TITLE", g_lib);
        return usb_fail();
    }
    char dir[288], app0[300];
    fmt(dir, sizeof(dir), "%s/%s", g_lib, title);
    fmt(app0, sizeof(app0), "%s/app0", dir);
    if (!is_file(app0, "eboot.bin")) {
        logf("approot: %s has no %s/app0/eboot.bin", g_lib, title);
        return usb_fail();
    }
    if (use_title(title, app0, dir) != 0)
        return usb_fail();
    *root = g_root;
    return 0;
}

static int approot_init(const char **root)
{
    const char *r = *root;
    char dir[256];
    if (strncmp(r, "usb:", 4) == 0) {
        if (usb_open(r + 4, dir, sizeof(dir)) != 0)
            return usb_fail();
    } else {
        strlcpy(dir, r, sizeof(dir));
    }
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/')
        dir[--n] = 0;
    const char *want = profile_requested();
    if (is_file(dir, "eboot.bin")) { /* one title: its ID from --title, its <TITLE>/app0 folder, or param.sfo */
        char up[256], title[16];
        strlcpy(up, dir, sizeof(up));
        char *slash = (char *)strrchr_c(up, '/');
        if (slash)
            *slash = 0;
        const char *parent = strrchr_c(up, '/');
        title[0] = 0;
        if (want)
            strlcpy(title, want, sizeof(title));
        else if (parent && slash && strcmp(slash + 1, "app0") == 0 && title_name(parent + 1))
            strlcpy(title, parent + 1, sizeof(title));
        if (use_title(title, dir, up) != 0)
            return usb_fail();
        *root = g_root;
        return 0;
    }
    char first[16];
    u32 count = lib_scan(dir, first);
    if (!count) {
        logf("approot: %s holds neither eboot.bin nor title folders (<TITLE>/app0/eboot.bin)", dir);
        return g_usb ? usb_fail() : 0; /* a dev directory: the boot says what is missing, as before */
    }
    strlcpy(g_lib, dir, sizeof(g_lib));
    logf("approot: %u titles in %s (the first: %s)", count, dir, first);
    if ((g_ctx->flags & CTX_FLAG_SELECT) || (!want && count > 1)) {
        g_ctx->app_root = (u64)g_lib; /* the selector lists it; boot.c then calls approot_pick */
        g_ctx->flags |= CTX_FLAG_LIBRARY;
        return 1;
    }
    return approot_pick(want ? want : first, root);
}

const struct ext_early g_approot_early HIDDEN = {EXT_EARLY_MAGIC, sizeof(struct ext_early), approot_init,
                                                  approot_pick};
