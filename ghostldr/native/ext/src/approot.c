/*
 * App roots on USB drives and folders of games (docs/usb.md), decided
 * before the title's modules load: the core places the extension early and
 * calls approot_init through ext_early (ext_main.c) for an app root that is
 * "auto", "usb:..." or a directory without an eboot.bin.  A title's own
 * directory (run.py --app-root /data/.../app0) never comes here.
 *
 *   auto         the games on a USB drive (usb:/, below); with none, the
 *     installed titles: approot_init returns 2 and the core boots ghostfs.
 *   usb:/path    a path on a USB drive.  ShellCore's AutoMounter mounts the
 *     drive into SWRR's sandbox (usb_map, below) and the path is a
 *     directory under the mount, read and written with the kernel's own file
 *     calls like a dev setup's: the mount is read-write, /app0 included.
 *     A build with USB_RAW=1 adds route B as a fallback: the extension
 *     drives the drive itself and reads its exFAT or FAT32 volume
 *     (usb_sce.c, usbfs.c), served like ghostfs's.  It is off by default:
 *     SWRR's libSceUsbd lists no devices on a 13.60 PS5.
 *
 * Either may hold one game (eboot.bin in it) or games anywhere below it,
 * found by their contents, not their folders' names (lib_scan): a game is a
 * folder with eboot.bin, or one with app0/eboot.bin (then that folder holds
 * its optional prefix and meta.bin, as gfs_pack.py --usb lays them out).
 * Its title ID is param.sfo's TITLE_ID, else meta.bin's, else one in its
 * folder's name (CUSA12345, "Ghost (CUSA11456)"; the whole name in the old
 * ghostldr/<TITLE>/app0 layout).  The title is run.py --title's, else the
 * only game, else the game selector's (CTX_FLAG_LIBRARY: it lists
 * library(); boot.c then calls approot_pick with its choice).
 *
 * A title from a USB drive keeps its progress in SWRR's save containers as
 * a ghostfs title does (<prefix>SAVE, gfs_use_saves).  A dev library's
 * title keeps its saves beside it, as before.
 *
 * The FTP server (ftp_sys.c) serves the drive too, and mounts one for any
 * boot: usb_ftp_prepare as the title is about to start, usb_ftp_home when a
 * client logs in (a drive plugged in since).
 */
#include "ext.h"
#include "usbstore.h"

static char g_root[256]; /* the chosen title's app0: g_ctx->app_root from then on */
static char g_lib[256];  /* the folder the games in g_list were found in */
static bool g_usb;       /* on a USB drive */
static bool g_auto;      /* app root "auto": no game on a drive is not an error */
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
    bool inited; /* the session is open: kept for the whole SWRR session */
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

static int stat_type(const char *path)
{
    u8 st[0x78];
    return vfs_stat(path, st) == 0 ? *(u16 *)(st + 8) & 0xF000 : 0;
}
static bool is_dir(const char *path) { return stat_type(path) == 0x4000; }

static void usb_unmount(void)
{
    if (!U.mounted)
        return;
    char root[2] = "/";
    s32 rc = U.unmap(U.id, root);
    logf("approot: unmounted the USB drive %x from %s (%x)", U.id, USB_MOUNT, rc);
    U.mounted = false;
}

/* The drive list, after opening the session if it isn't open: the count,
 * or -1 with the reason logged */
static s32 usb_drives(u32 *ids, s32 max)
{
    if (usb_lib() != 0)
        return -1;
    s32 n = 0;
    memset(ids, 0, sizeof(u32) * (u64)max);
    s32 rc = U.list(ids, &n);
    if (rc == USBS_NOT_INIT) {
        rc = U.init(NULL);
        logf("approot: sceUsbStorageInit -> %x", rc);
        if (rc == 0)
            rc = U.list(ids, &n);
    }
    U.inited |= rc == 0;
    if (U.inited) /* remembered past ext_unplace ("auto" booting an installed title): the code's data starts over */
        g_ctx->flags |= CTX_FLAG_USB_SESSION;
    logf("approot: sceUsbStorageGetDeviceList -> %x, %d drive(s)", rc, n);
    if (rc != 0 || n <= 0) {
        logf("approot: no USB drive%s", rc ? " (libSceUsbStorage refused)" : g_auto ? " in the PS5" : ": plug one into the PS5");
        return -1;
    }
    return n < max ? n : max;
}

/* Mount drive id at USB_MOUNT: whether its root holds rel ("" for any drive);
 * a drive without it is unmounted again */
static bool usb_try(u32 id, const char *rel)
{
    /* the kernel and the IPC read these: not from the extension's code pages */
    char root[2] = "/", mp[sizeof(USB_MOUNT)] = USB_MOUNT, want[320];
    fmt(want, sizeof(want), "%s/%s", mp, rel);
    U.unmap(id, root); /* a mount left by an earlier boot of this SWRR: 0x80F40002 when none */
    s32 rc = U.map2(id, root, mp);
    bool has = rc == 0 && is_dir(want);
    logf("approot: USB drive %x mounted at %s: %x; %s %s", id, mp, rc, want, rc ? "-" : has ? "found" : "missing");
    if (has) {
        U.id = id;
        U.mounted = true;
    } else if (rc == 0) {
        U.unmap(id, root);
    }
    return has;
}

/* Mount the drive whose root holds rel ("ghostldr", "" for any drive) at
 * USB_MOUNT: 0, or -1 with the reason logged */
static int usb_mount(const char *rel)
{
    u32 ids[16];
    s32 n = usb_drives(ids, (s32)ARRAY_SIZE(ids));
    for (s32 i = 0; i < n; i++)
        if (usb_try(ids[i], rel))
            return 0;
    if (n > 0)
        logf("approot: no USB drive has /%s", rel);
    return -1;
}

/* hle_usbstorage.c: the session this file opened, which the title must not end */
HIDDEN bool usb_session_open(void) { return U.inited || (g_ctx->flags & CTX_FLAG_USB_SESSION); }

/* ftp_sys.c, as the title is about to start: a drive mounted for FTP
 * clients when one is plugged in.  The session opens now, before the
 * title's code runs, so the title's own Init and Term stay off it. */
HIDDEN void usb_ftp_prepare(void)
{
    if (!U.mounted)
        usb_mount("");
}

/* ftp_sys.c, as a client logs in: where it starts.  A drive that went away
 * is forgotten, and one plugged in since is mounted. */
HIDDEN const char *usb_ftp_home(void)
{
    static volatile int lock;
    spin_lock(&lock);
    if (U.mounted && !is_dir(USB_MOUNT)) {
        logf("approot: the USB drive at %s is gone", USB_MOUNT);
        U.mounted = false;
    }
    if (!U.mounted && usb_session_open())
        usb_mount("");
    bool m = U.mounted;
    spin_unlock(&lock);
    return m ? USB_MOUNT : "/";
}

static bool is_file(const char *dir, const char *name)
{
    char p[320];
    fmt(p, sizeof(p), "%s/%s", dir, name);
    return stat_type(p) == 0x8000;
}

static bool title_name(const char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-'))
            return false;
    return n >= 1 && n <= 15;
}

static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
static bool same_ci(const char *a, const char *b)
{
    while (*a && upper(*a) == upper(*b))
        a++, b++;
    return upper(*a) == upper(*b);
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

/* param.sfo's TITLE_ID: whether the file is there (the ID may still be unusable) */
static bool sfo_title(const char *app0, char out[16])
{
    const u64 cap = 0x10000;
    u8 *sfo = page_alloc(cap);
    out[0] = 0;
    s64 n = sfo ? read_small(app0, "sce_sys/param.sfo", sfo, cap) : -1;
    if (n >= 20 && memcmp(sfo, "\0PSF", 4) == 0)
        sfo_find(sfo, (u32)n, out);
    page_free(sfo, cap);
    return n >= 0;
}

/* meta.bin's title ID (host/gfs_meta.py: "GFSM", the ID at 0x10) */
static void meta_title(const char *dir, char out[16])
{
    u8 h[0x20];
    out[0] = 0;
    if (read_small(dir, "meta.bin", h, sizeof(h)) == (s64)sizeof(h) && memcmp(h, "GFSM", 4) == 0) {
        memcpy(out, h + 0x10, 15);
        out[15] = 0;
        if (!title_name(out))
            out[0] = 0;
    }
}

/* A title ID in a folder's name: all of it in the old ghostldr/<TITLE>/app0
 * layout (whole), else four letters and five digits anywhere in it
 * ("Ghost of Tsushima (CUSA11456)") */
static void name_title(const char *name, bool whole, char out[16])
{
    out[0] = 0;
    if (whole && title_name(name)) {
        strlcpy(out, name, 16);
        return;
    }
    for (const char *s = name; *s; s++) {
        u32 k = 0;
        while (k < 9 && s[k] && (k < 4 ? upper(s[k]) >= 'A' && upper(s[k]) <= 'Z' : s[k] >= '0' && s[k] <= '9'))
            k++;
        bool edge = s == name || !((upper(s[-1]) >= 'A' && upper(s[-1]) <= 'Z') || (s[-1] >= '0' && s[-1] <= '9'));
        if (k == 9 && edge && !(s[9] >= '0' && s[9] <= '9')) {
            for (k = 0; k < 9; k++)
                out[k] = upper(s[k]);
            out[9] = 0;
            return;
        }
    }
}

/* the profile's save prefix (host/profiles.py REC_PREFIX), from the blobs
 * run.py sent: the last u64 of the boot context (profile.c PROFILE_SLOT) */
static void profile_prefix(const char *title, char prefix[16])
{
    const u8 *p = (const u8 *)((const u64 *)g_ctx)[63];
    prefix[0] = 0;
    for (int n = 0; p && n < 256; n++) {
        const struct prof_hdr *h = (const struct prof_hdr *)p;
        if (memcmp(h->magic, "GLPF", 4) != 0 || h->version != 1 || h->header_size < sizeof(*h) ||
            h->total < h->header_size || h->total > 0x10000 || (h->total & 7))
            return;
        if (strncmp(h->title, title, sizeof(h->title)) == 0) {
            for (u32 at = h->header_size; at + sizeof(struct prof_rec) <= h->total;) {
                const struct prof_rec *r = (const struct prof_rec *)(p + at);
                if (r->size < sizeof(*r) || at + r->size > h->total)
                    return;
                if (r->kind == REC_PREFIX) {
                    const char *s = (const char *)(r + 1);
                    u32 k = 0;
                    while (k < 12 && k + sizeof(*r) < r->size && ((s[k] >= 'A' && s[k] <= 'Z') || (s[k] >= '0' && s[k] <= '9')))
                        prefix[k] = s[k], k++;
                    prefix[k] = 0;
                    return;
                }
                at += r->size;
            }
            return;
        }
        p += h->total;
    }
}

/* the save container's prefix: <folder>/prefix, else the title's profile's,
 * else gfs_pack.py's default (G and a CUSA-style ID's digits, else G and
 * its first 11 letters and digits) */
static void title_prefix(const char *dir, const char *title, char prefix[16])
{
    char b[16];
    s64 n = read_small(dir, "prefix", b, sizeof(b) - 1);
    u32 k = 0;
    while (n > 0 && k < (u32)n && k < 12 && ((b[k] >= 'A' && b[k] <= 'Z') || (b[k] >= '0' && b[k] <= '9')))
        prefix[k] = b[k], k++;
    prefix[k] = 0;
    if (!k)
        profile_prefix(title, prefix);
    if (prefix[0] && strncmp(prefix, "GFS", 3) != 0)
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

/* ---- the games under a folder, found by their contents ----
 *
 * Breadth first from the folder: one listing per folder.  A folder with
 * eboot.bin is a game, and so is one with app0/eboot.bin; neither is
 * looked into further.  Others are looked into down to SCAN_DEPTH levels
 * below the folder, SCAN_DIRS folders in all and SCAN_ENTRIES entries per
 * folder, so a large drive stays quick (the console's exFAT lists a folder
 * of thousands of files slowly).  Names starting with "." are skipped
 * (macOS's .Spotlight-V100, .Trashes, ...), and Windows' system folders. */
#define SCAN_DEPTH 4
#define SCAN_DIRS 256
#define SCAN_ENTRIES 4096
#define LIB_MAX 64 /* the selector's SEL_MAX_TITLES */

static struct lib_title *g_list; /* page_alloc'd while a library is open */
static u32 g_count, g_good;      /* games found; of them bootable */
static bool g_cut;               /* the scan stopped short */

static void lib_free(void)
{
    if (g_list)
        page_free(g_list, sizeof(struct lib_title) * LIB_MAX);
    g_list = NULL;
    g_count = g_good = 0;
    g_lib[0] = 0;
}

/* a path as the user knows it: relative to the drive ("My Games/ghost") */
static const char *shown(const char *path)
{
    size_t n = strlen(USB_MOUNT);
    if (g_usb && strncmp(path, USB_MOUNT, n) == 0)
        return path[n] == '/' ? path + n + 1 : path[n] ? path : "the USB drive";
    return path;
}

static struct lib_title *lib_add(const char *app0, size_t dir_len)
{
    if (g_count >= LIB_MAX) {
        g_cut = true;
        return NULL;
    }
    struct lib_title *t = &g_list[g_count++];
    memset(t, 0, sizeof(*t));
    strlcpy(t->path, app0, sizeof(t->path));
    t->dir_len = (u16)(dir_len < sizeof(t->path) ? dir_len : strlen(t->path));
    return t;
}

static void lib_bad(struct lib_title *t, const char *note)
{
    t->flags |= LIB_TITLE_BAD;
    strlcpy(t->note, note, sizeof(t->note));
}

/* The title ID of the game in app0, whose folder (with prefix and meta.bin)
 * is app0's first dir_len bytes: param.sfo's, meta.bin's, else one in the
 * folder's name.  The reason when there is none. */
static const char *identify(const char *app0, size_t dir_len, char id[16])
{
    char dir[256];
    strlcpy(dir, app0, dir_len + 1 < sizeof(dir) ? dir_len + 1 : sizeof(dir));
    const char *base = strrchr_c(dir, '/');
    base = base ? base + 1 : dir;
    bool sfo = sfo_title(app0, id);
    if (!id[0])
        meta_title(dir, id);
    if (!id[0])
        name_title(base, dir_len < strlen(app0), id);
    if (id[0])
        return NULL;
    return sfo ? "its sce_sys/param.sfo has no usable TITLE_ID: name the folder after the game's ID"
               : "no sce_sys/param.sfo: name the folder after the game's ID (CUSA12345)";
}

/* A game found at app0 (dir_len: its folder) */
static void lib_game(const char *app0, size_t dir_len)
{
    struct lib_title *t = lib_add(app0, dir_len);
    const char *why = t ? identify(app0, dir_len, t->id) : NULL;
    if (why)
        lib_bad(t, why);
}

static bool skip_dir(const char *name)
{
    return name[0] == '.' || same_ci(name, "System Volume Information") || same_ci(name, "$RECYCLE.BIN") ||
           same_ci(name, "sce_sys") || same_ci(name, "sce_module");
}

/* the games under root into g_list, sorted: bootable ones by title ID then
 * folder, second copies marked, the others last */
static void lib_scan(const char *root)
{
    lib_free();
    g_cut = false;
    g_list = page_alloc(sizeof(struct lib_title) * LIB_MAX);
    const u64 qsize = (u64)SCAN_DIRS * 256 + SCAN_DIRS;
    char (*q)[256] = page_alloc(qsize);
    if (!g_list || !q) {
        logf("approot: no memory to look for games");
        if (q)
            page_free(q, qsize);
        return;
    }
    u8 *depth = (u8 *)(q + SCAN_DIRS);
    u32 head = 0, tail = 1, listed = 0;
    strlcpy(q[0], root, 256);
    depth[0] = 0;
    while (head < tail) {
        const char *dir = q[head];
        u32 d = depth[head++], first = tail, entries = 0;
        bool eboot = false, app0 = false, sce_sys = false;
        int fd = vfs_open(dir, O_RDONLY, 0);
        if (fd < 0)
            continue;
        listed++;
        u8 buf[2048];
        int got;
        while ((got = vfs_getdents(fd, buf, sizeof(buf))) > 0 && entries <= SCAN_ENTRIES)
            for (int off = 0; off + 8 < got;) {
                const u8 *e = buf + off;
                u16 reclen = *(const u16 *)(e + 4);
                if (!reclen)
                    break;
                off += reclen;
                const char *name = (const char *)e + 8;
                if (++entries > SCAN_ENTRIES) {
                    logf("approot: %s: more than %u entries, the rest not looked at", shown(dir), SCAN_ENTRIES);
                    g_cut = true;
                    break;
                }
                if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2])))
                    continue;
                char path[256];
                int len = fmt(path, sizeof(path), "%s/%s", dir, name);
                int type = e[6] == 4 ? 0x4000 : e[6] == 8 ? 0x8000 : e[6] ? 0 : stat_type(path);
                if (type == 0x8000 && same_ci(name, "eboot.bin"))
                    eboot = true;
                else if (type == 0x4000 && same_ci(name, "app0"))
                    app0 = true;
                else if (type == 0x4000 && same_ci(name, "sce_sys"))
                    sce_sys = true;
                else if (type == 0x4000 && d < SCAN_DEPTH && !skip_dir(name) && len < 250) {
                    if (tail < SCAN_DIRS) {
                        strlcpy(q[tail], path, 256);
                        depth[tail++] = (u8)(d + 1);
                    } else {
                        g_cut = true;
                    }
                }
            }
        vfs_close(fd);
        if (!eboot && !app0 && !sce_sys)
            continue;
        tail = first; /* a game's folder, or what is left of one: not looked into */
        char path[256];
        if (eboot) {
            lib_game(dir, strlen(dir));
        } else if (app0 && (fmt(path, sizeof(path), "%s/app0", dir), is_file(path, "eboot.bin"))) {
            lib_game(path, strlen(dir));
        } else {
            struct lib_title *t = lib_add(app0 ? path : dir, strlen(dir));
            if (t) {
                name_title(strrchr_c(dir, '/') ? strrchr_c(dir, '/') + 1 : dir, app0, t->id);
                lib_bad(t, app0 ? "its app0 folder has no eboot.bin (copied in full?)"
                                : "a game's sce_sys folder but no eboot.bin (copied in full?)");
            }
        }
    }
    page_free(q, qsize);
    strlcpy(g_lib, root, sizeof(g_lib));
    if (head < tail || g_cut)
        logf("approot: stopped looking after %u folders: keep games within %u folders of %s", listed, SCAN_DEPTH,
             shown(root));
    /* sort: bootable first, by title ID, then by folder */
    for (u32 i = 1; i < g_count; i++)
        for (u32 j = i; j > 0; j--) {
            struct lib_title *a = &g_list[j - 1], *b = &g_list[j];
            int bad = (int)(a->flags & LIB_TITLE_BAD) - (int)(b->flags & LIB_TITLE_BAD);
            int c = bad ? bad : strcmp(a->id, b->id) ? strcmp(a->id, b->id) : strcmp(a->path, b->path);
            if (c <= 0)
                break;
            struct lib_title t;
            memcpy(&t, a, sizeof(t));
            memcpy(a, b, sizeof(t));
            memcpy(b, &t, sizeof(t));
        }
    for (u32 i = 0; i < g_count; i++) {
        struct lib_title *t = &g_list[i];
        if (!(t->flags & LIB_TITLE_BAD)) {
            g_good++;
            for (u32 j = 0; j < i; j++)
                if (!(g_list[j].flags & (LIB_TITLE_BAD | LIB_TITLE_DUP)) && strcmp(g_list[j].id, t->id) == 0) {
                    char first[256];
                    strlcpy(first, g_list[j].path, g_list[j].dir_len + 1u < sizeof(first) ? g_list[j].dir_len + 1u : sizeof(first));
                    t->flags |= LIB_TITLE_DUP;
                    fmt(t->note, sizeof(t->note), "a second copy: --title boots the one in %s", shown(first));
                    break;
                }
        }
        logf("approot:   %s %s%s%s", t->id[0] ? t->id : "?", shown(t->path), t->note[0] ? ": " : "", t->note);
    }
}

/* ghost_api.library: the games the selector lists */
static int lib_get(const struct lib_title **list)
{
    *list = g_list;
    return g_list ? (int)g_count : 0;
}

static int usb_open(const char *path, char *out, size_t size);

/* a boot that stops: the drive goes back; "auto" boots the installed titles instead */
static int give_up(void)
{
    usb_unmount();
    lib_free();
    if (g_auto)
        logf("approot: booting an installed title instead");
    return g_auto ? 2 : -1;
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
        logf("approot: %s: no title ID (no sce_sys/param.sfo TITLE_ID, and the folder isn't named after it): "
             "pass run.py --title", shown(app0));
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
    logf("approot: %s from %s", title ? title : "the title", shown(app0));
    return 0;
}

static int use_entry(const struct lib_title *t)
{
    char dir[256];
    strlcpy(dir, t->path, t->dir_len + 1u < sizeof(dir) ? t->dir_len + 1u : sizeof(dir));
    return use_title(t->id, t->path, dir);
}

/* the first bootable copy of title */
static const struct lib_title *lib_find(const char *title)
{
    for (u32 i = 0; title && i < g_count; i++)
        if (!(g_list[i].flags & LIB_TITLE_BAD) && same_ci(g_list[i].id, title))
            return &g_list[i];
    return NULL;
}

static int approot_pick(const char *title, int index, const char **root)
{
    g_ctx->flags &= ~(u64)CTX_FLAG_LIBRARY;
    const struct lib_title *t = NULL;
    if (title && title[0] && index >= 0 && (u32)index < g_count && !(g_list[index].flags & LIB_TITLE_BAD) &&
        strcmp(g_list[index].id, title) == 0)
        t = &g_list[index]; /* the selector's choice, this copy */
    else if (title && title[0])
        t = lib_find(title);
    else if (!(t = lib_find(profile_requested())))
        logf("approot: no game chosen from %s: pick one with run.py boot --title TITLE", shown(g_lib));
    if (!t && title && title[0])
        logf("approot: %s is not among the games in %s", title, shown(g_lib));
    int rc = t ? use_entry(t) : -1;
    lib_free();
    if (rc != 0) {
        usb_unmount();
        return -1;
    }
    *root = g_root;
    return 0;
}

static int approot_init(const char **root)
{
    const char *r = *root;
    g_auto = strcmp(r, "auto") == 0;
    if (g_auto)
        r = "usb:/";
    char dir[256];
    if (strncmp(r, "usb:", 4) == 0) {
        if (usb_open(r + 4, dir, sizeof(dir)) != 0)
            return give_up();
    } else {
        strlcpy(dir, r, sizeof(dir));
    }
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/')
        dir[--n] = 0;
    const char *want = profile_requested();
    if (is_file(dir, "eboot.bin")) { /* one game: its ID from --title, else as a found game's */
        const char *slash = strrchr_c(dir, '/');
        size_t dir_len = slash && same_ci(slash + 1, "app0") ? (size_t)(slash - dir) : n;
        char folder[256], title[16];
        strlcpy(folder, dir, dir_len + 1 < sizeof(folder) ? dir_len + 1 : sizeof(folder));
        title[0] = 0;
        if (want)
            strlcpy(title, want, sizeof(title));
        else
            identify(dir, dir_len, title);
        lib_free();
        if (use_title(title, dir, folder) != 0)
            return give_up();
        *root = g_root;
        return 0;
    }
    if (!g_list || strcmp(g_lib, dir) != 0)
        lib_scan(dir);
    logf("approot: %u game%s %s%s%s", g_good, g_good == 1 ? "" : "s", g_usb && !strcmp(dir, USB_MOUNT) ? "on " : "in ",
         g_usb && !strcmp(dir, USB_MOUNT) ? "the USB drive" : shown(dir),
         g_count > g_good ? " (and folders that can't be booted, above)" : "");
    if (!g_good) {
        logf("approot: %s holds no games (a folder with eboot.bin, or with app0/eboot.bin)", shown(dir));
        if (!g_usb && !g_auto) { /* a dev directory: the boot says what is missing, as before */
            lib_free();
            return 0;
        }
        return give_up();
    }
    if (want && !lib_find(want)) {
        logf("approot: %s is not among the games in %s", want, shown(dir));
        return give_up();
    }
    if ((g_ctx->flags & CTX_FLAG_SELECT) || (!want && g_good > 1)) {
        g_ctx->app_root = (u64)g_lib; /* the selector lists library(); boot.c then calls approot_pick */
        g_ctx->flags |= CTX_FLAG_LIBRARY;
        return 1;
    }
    const struct lib_title *t = lib_find(want);
    for (u32 i = 0; !t && i < g_count; i++)
        if (!(g_list[i].flags & LIB_TITLE_BAD))
            t = &g_list[i];
    int rc = use_entry(t);
    lib_free();
    if (rc != 0)
        return give_up();
    *root = g_root;
    return 0;
}

/* the drive's directory for path ("/ghostldr" -> "/ghostldr_usb/ghostldr",
 * or route B's "/usb0/ghostldr").  With several drives and the drive's top
 * ("usb:/", "auto"), the first that holds a game. */
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
    int rc = -1;
    if (!top[0]) {
        u32 ids[16];
        s32 n = usb_drives(ids, (s32)ARRAY_SIZE(ids));
        for (s32 i = 0; i < n && rc != 0; i++) {
            if (!usb_try(ids[i], ""))
                continue;
            g_usb = true;
            if (n > 1) /* several drives: the first with a game (its list is kept) */
                lib_scan(USB_MOUNT);
            if (n == 1 || g_good > 0 || i == n - 1)
                rc = 0;
            else
                usb_unmount();
        }
    } else {
        rc = usb_mount(top);
    }
    if (rc == 0) {
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

const struct ext_early g_approot_early HIDDEN = {EXT_EARLY_MAGIC, sizeof(struct ext_early), approot_init,
                                                  approot_pick, lib_get};
