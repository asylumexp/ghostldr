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
 *
 * Every drive plugged in is mounted, up to USB_SLOTS: the first at
 * /ghostldr_usb, the others at /ghostldr_usb2, 3 and 4, and the games on all
 * of them are listed together.  A drive that goes away while its title
 * runs (pulled out, or rest mode) is mounted again when it is back
 * (usb_slot_reattach, for usb_live.c).
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
#define USB_MOUNT "/ghostldr_usb" /* the first drive, in SWRR's sandbox; the others at USB_MOUNT "2" .. "4" */
#define USB_SLOTS 4
#define USBS_NOT_INIT ((s32)0x80F40009)

static struct {
    s32 (*init)(void *);
    s32 (*info)(u32 id, void *info);
    s32 (*list)(u32 *ids, s32 *count);
    s32 (*map2)(u32 id, const char *dir, const char *mount_point);
    s32 (*unmap)(u32 id, const char *dir);
    bool inited;            /* the session is open: kept for the whole SWRR session */
    u32 id[USB_SLOTS];      /* the drive mounted at each slot's mount point */
    bool mounted[USB_SLOTS];
} U;
static volatile int g_mount_lock; /* U's slots, once the title runs (usb_live.c's thread, FTP logins) */

/* the games' folders on the drives (usb_open): one per drive that has it */
static char g_roots[USB_SLOTS][256];
static u32 g_nroots;

/* the title's drive: its slot and its eboot.bin as it was at boot, to know
 * the drive again when it comes back (usb_live.c) */
static struct {
    int slot;
    s64 size, mtime;
} g_ident = {-1, 0, 0};

/* each slot's mount point: /ghostldr_usb, /ghostldr_usb2, ..., or, after a
 * drive came back and its old mount point was still taken, one with a
 * suffix (/ghostldr_usb_1) */
static char g_mp[USB_SLOTS][24];

HIDDEN const char *usb_slot_mp(u32 k)
{
    if (!g_mp[k][0])
        fmt(g_mp[k], sizeof(g_mp[k]), k ? "%s%u" : "%s", USB_MOUNT, k + 1);
    return g_mp[k];
}

/* slot k's mount point, in buf (the kernel and the IPC read it: not from the extension's code pages) */
static const char *slot_mp(u32 k, char buf[24])
{
    strlcpy(buf, usb_slot_mp(k), 24);
    return buf;
}

/* the slot whose mount point path is or is under: -1 when none */
HIDDEN int usb_slot_of(const char *path)
{
    for (u32 k = 0; path && k < USB_SLOTS; k++) {
        const char *mp = usb_slot_mp(k);
        size_t n = strlen(mp);
        if (strncmp(path, mp, n) == 0 && (path[n] == 0 || path[n] == '/'))
            return (int)k;
    }
    return -1;
}

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

/* why a boot stops, for its message on the TV (ext/src/failure.c, docs/failures.md) */
__attribute__((noinline, minsize)) static void NOTE(u32 kind, const char *what)
{
    struct ghost_diag *D = GHOST_DIAG(G);
    if (D)
        D->note = kind, strlcpy(D->note_path, what, sizeof(D->note_path));
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
    U.info = host_dlsym(h, "sceUsbStorageGetDeviceInfo");
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
        NOTE(FAIL_DRIVE_REFUSED, "");
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

static void usb_unmount_slot(u32 k)
{
    if (!U.mounted[k])
        return;
    char root[2] = "/", mp[24];
    s32 rc = U.unmap(U.id[k], root);
    logf("approot: unmounted the USB drive %x from %s (%x)", U.id[k], slot_mp(k, mp), rc);
    U.mounted[k] = false;
}

static void usb_unmount(void)
{
    for (u32 k = 0; k < USB_SLOTS; k++)
        usb_unmount_slot(k);
}

static bool any_mounted(void)
{
    for (u32 k = 0; k < USB_SLOTS; k++)
        if (U.mounted[k])
            return true;
    return false;
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
        NOTE(rc ? FAIL_DRIVE_REFUSED : FAIL_NO_DRIVE, "");
        return -1;
    }
    return n < max ? n : max;
}

/* what the system says of drive id, in the log: its size, free space,
 * maker and model, and its file system (SceUsbStorageDeviceInfo: u64 total
 * and free bytes, vid/pid at 0x10, strings at 0x16, 0x120 and 0x228, the
 * features word at 0x330: 0x80 exFAT, 0x40 FAT32) */
static void usb_info(u32 id)
{
    u8 b[0x400];
    if (!U.info)
        return;
    memset(b, 0, sizeof(b));
    s32 rc = U.info(id, b);
    if (rc != 0) {
        logf("approot: USB drive %x: sceUsbStorageGetDeviceInfo -> %x", id, rc);
        return;
    }
    char vendor[40], product[40];
    strlcpy(vendor, (const char *)b + 0x16, sizeof(vendor));
    strlcpy(product, (const char *)b + 0x120, sizeof(product));
    u32 feat = *(u32 *)(b + 0x330);
    logf("approot: USB drive %x: %s %s, %lu MiB, %lu MiB free, usb %04x:%04x, features %x%s", id, vendor, product,
         *(u64 *)b >> 20, *(u64 *)(b + 8) >> 20, *(u16 *)(b + 0x10), *(u16 *)(b + 0x12), feat,
         feat & 0x80 ? " (exFAT)" : feat & 0x40 ? " (FAT32)" : "");
}

/* Every drive plugged in mounted, the first USB_SLOTS of the list in its
 * order: the number mounted */
static u32 usb_mount_all(void)
{
    u32 ids[16];
    s32 n = usb_drives(ids, (s32)ARRAY_SIZE(ids));
    char root[2] = "/", mp[24];
    for (s32 i = 0; i < n; i++)
        U.unmap(ids[i], root); /* a mount left by an earlier boot of this SWRR: 0x80F40002 when none */
    u32 k = 0;
    for (s32 i = 0; i < n && k < USB_SLOTS; i++) {
        s32 rc = U.map2(ids[i], root, slot_mp(k, mp));
        logf("approot: USB drive %x mounted at %s: %x", ids[i], mp, rc);
        usb_info(ids[i]);
        if (rc == 0) {
            U.id[k] = ids[i];
            U.mounted[k++] = true;
        }
    }
    if (n > USB_SLOTS)
        logf("approot: %d USB drives: the first %u are used", n, USB_SLOTS);
    return k;
}

/* Drives plugged in since, mounted in the free slots; the ones mounted stay
 * as they are.  Not while the title's own drive is away: it is usb_live.c's
 * to mount again, and may come back with a new id. */
static void usb_mount_new(void)
{
    u32 ids[16];
    s32 n = 0;
    if ((g_ident.slot >= 0 && !U.mounted[g_ident.slot]) || !U.inited || !U.list)
        return;
    memset(ids, 0, sizeof(ids));
    if (U.list(ids, &n) != 0 || n > (s32)ARRAY_SIZE(ids)) /* not usb_drives: no drive is no failure here */
        return;
    char root[2] = "/", mp[24];
    for (s32 i = 0; i < n; i++) {
        bool have = false;
        for (u32 k = 0; k < USB_SLOTS; k++)
            have |= U.mounted[k] && U.id[k] == ids[i];
        u32 k = 0;
        while (k < USB_SLOTS && U.mounted[k])
            k++;
        if (have || k == USB_SLOTS)
            continue;
        U.unmap(ids[i], root); /* a mount left by an earlier boot */
        s32 rc = U.map2(ids[i], root, slot_mp(k, mp));
        logf("approot: USB drive %x plugged in since: mounted at %s: %x", ids[i], mp, rc);
        if (rc == 0)
            U.id[k] = ids[i], U.mounted[k] = true;
    }
}

/* hle_usbstorage.c: the session this file opened, which the title must not end */
HIDDEN bool usb_session_open(void) { return U.inited || (g_ctx->flags & CTX_FLAG_USB_SESSION); }

/* ftp_sys.c, as the title is about to start: the drives mounted for FTP
 * clients when any is plugged in.  The session opens now, before the
 * title's code runs, so the title's own Init and Term stay off it. */
HIDDEN void usb_ftp_prepare(void)
{
    spin_lock(&g_mount_lock);
    if (!any_mounted())
        usb_mount_all();
    spin_unlock(&g_mount_lock);
}

/* ftp_sys.c, as a client logs in: the first drive's mount point, or "/".  A
 * drive that went away is forgotten, and those plugged in since are mounted;
 * the title's own drive is usb_live.c's to mount again. */
HIDDEN const char *usb_ftp_home(void)
{
    char mp[24];
    spin_lock(&g_mount_lock);
    for (u32 k = 0; k < USB_SLOTS; k++)
        if (U.mounted[k] && (int)k != g_ident.slot && !is_dir(slot_mp(k, mp))) {
            logf("approot: the USB drive at %s is gone", mp);
            U.mounted[k] = false;
        }
    if (!any_mounted() && usb_session_open() && g_ident.slot < 0)
        usb_mount_all();
    else
        usb_mount_new();
    bool m = U.mounted[0];
    spin_unlock(&g_mount_lock);
    return m ? usb_slot_mp(0) : "/";
}

/* ---- the title's drive while it runs (usb_live.c) ---- */

/* the slot of the drive the title runs from; -1: not from a drive */
HIDDEN int usb_title_slot(void) { return g_ident.slot; }

/* Whether slot's drive answers: 0, else the errno its check gave (ENOENT 2
 * when the title's eboot.bin is gone or another drive's is there).  For the
 * title's slot its eboot.bin, as it was at boot; for another, its mount
 * point. */
HIDDEN int usb_slot_check(int slot)
{
    char p[300];
    u8 st[0x78];
    if (slot < 0 || slot >= USB_SLOTS)
        return 22;
    /* the device first: a stat may still be answered from the cache a moment after it went */
    u32 ids[16];
    s32 n = 0;
    if (U.list && U.mounted[slot] && U.list(ids, &n) == 0) {
        bool there = false;
        for (s32 i = 0; i < n && i < 16; i++)
            there |= ids[i] == U.id[slot];
        if (!there)
            return 19; /* ENODEV */
    }
    if (slot != g_ident.slot)
        slot_mp((u32)slot, p);
    else
        fmt(p, sizeof(p), "%s/eboot.bin", g_root);
    s32 rc = (s32)vfs_stat(p, st);
    if (rc != 0)
        return ((u32)rc >> 16) == 0x8002 ? (int)(rc & 0xFFFF) : 5;
    if (slot == g_ident.slot && (*(s64 *)(st + 0x48) != g_ident.size || *(s64 *)(st + 0x28) != g_ident.mtime))
        return 2;
    return 0;
}

/* slot's mount point is mp from now on: the title's app root follows */
static void move_slot(u32 slot, const char *mp)
{
    size_t n = strlen(g_mp[slot]);
    if ((int)slot == g_ident.slot && strncmp(g_root, g_mp[slot], n) == 0) {
        char r[sizeof(g_root)];
        fmt(r, sizeof(r), "%s%s", mp, g_root + n);
        strlcpy(g_root, r, sizeof(g_root)); /* in place: g_ctx->app_root points at it */
    }
    strlcpy(g_mp[slot], mp, sizeof(g_mp[slot]));
    logf("usb: slot %u is at %s now; the title's root %s", slot, mp, g_root);
}

/* Slot's drive mounted again, once it is plugged in (or back from rest
 * mode): 0 when it answers again, 1 while no drive is there to mount, 2 when
 * only drives without the title are (they're left unmounted), < 0 when the
 * system refuses (the reason logged once).  The session is opened again if
 * it ended. */
HIDDEN int usb_slot_reattach(int slot)
{
    static s32 last_rc = 1, last_n = -1;
    static u32 wrong[8], nwrong; /* drives (ids) found without the title: not mounted again */
    if (slot < 0 || slot >= USB_SLOTS || usb_lib() != 0)
        return -1;
    spin_lock(&g_mount_lock);
    u32 ids[16];
    s32 n = 0;
    memset(ids, 0, sizeof(ids));
    s32 rc = U.list(ids, &n);
    if (rc == USBS_NOT_INIT) {
        rc = U.init(NULL);
        logf("usb: the USB storage session had ended: sceUsbStorageInit -> %x", rc);
        if (rc == 0)
            rc = U.list(ids, &n);
    }
    if (rc != last_rc || n != last_n)
        logf("usb: sceUsbStorageGetDeviceList -> %x, %d drive(s)", rc, n);
    last_rc = rc, last_n = n;
    int out = rc != 0 ? -1 : 1;
    char root[2] = "/", mp[24];
    slot_mp((u32)slot, mp);
    if (rc == 0 && U.mounted[slot]) { /* the old mount first: its device may be gone, so the answer only goes to the log */
        s32 u = U.unmap(U.id[slot], root);
        logf("usb: the old mount of drive %x at %s released: %x", U.id[slot], mp, u);
        U.mounted[slot] = false;
    }
    for (s32 i = 0; rc == 0 && i < n && i < 16 && out != 0; i++) {
        bool other = false, bad = false;
        for (u32 k = 0; k < USB_SLOTS; k++)
            other |= (int)k != slot && U.mounted[k] && U.id[k] == ids[i];
        for (u32 k = 0; k < nwrong && k < ARRAY_SIZE(wrong); k++)
            bad |= wrong[k] == ids[i];
        if (bad && out == 1)
            out = 2;
        if (other || bad)
            continue;
        U.unmap(ids[i], root);
        s32 m = U.map2(ids[i], root, mp);
        if (m != 0 && m != USBS_NOT_INIT) { /* the old mount point still taken: another one, and the title's paths follow */
            char alt[24];
            static u32 n_alt;
            fmt(alt, sizeof(alt), "%s_%u", USB_MOUNT, ++n_alt);
            if (slot)
                fmt(alt, sizeof(alt), "%s%d_%u", USB_MOUNT, slot + 1, n_alt);
            s32 m2 = U.map2(ids[i], root, alt);
            logf("usb: drive %x at %s: %x; at %s: %x", ids[i], mp, m, alt, m2);
            if (m2 == 0) {
                m = 0;
                move_slot((u32)slot, alt);
                strlcpy(mp, alt, sizeof(mp));
            }
        }
        int chk = m == 0 ? usb_slot_check(slot) : -1;
        logf("usb: drive %x mounted at %s: %x%s", ids[i], mp, m, m ? "" : chk == 0 ? ", the title's drive" : ", not the title's drive");
        if (m == 0 && chk == 0) {
            U.id[slot] = ids[i];
            U.mounted[slot] = true;
            out = 0;
        } else if (m == 0) {
            U.unmap(ids[i], root);
            wrong[nwrong++ % ARRAY_SIZE(wrong)] = ids[i];
            out = 2;
        }
    }
    spin_unlock(&g_mount_lock);
    return out;
}

/* "/mnt/usb0/x": where a jailbroken PS4's apps see its first USB drive
 * (/mnt/usb1 the second), and homebrew made for one looks for it.  The
 * drives ghostldr mounts in SWRR's sandbox (usb_ftp_home: mounted now if
 * plugged in later) stand in for them; with no such drive the path stays as
 * it is and isn't found. */
HIDDEN const char *usb_title_remap(const char *path, char *buf, size_t size)
{
    if (strncmp(path, "/mnt/usb", 8) != 0 || path[8] < '0' || path[8] >= '0' + USB_SLOTS ||
        (path[9] != '/' && path[9] != 0))
        return NULL;
    u32 k = (u32)(path[8] - '0');
    usb_ftp_home();
    char mp[24];
    if (!U.mounted[k])
        return NULL;
    fmt(buf, size, "%s%s", slot_mp(k, mp), path + 9);
    return buf;
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

/* the host's own save directory name exists (one exact DirNameSearch) */
static bool host_save_exists(const char *name)
{
    s32 (*search)(const void *, void *) = host_real_in("libSceSaveData", "sceSaveDataDirNameSearch");
    s32 (*initial)(s32 *) = host_real_in("libSceUserService", "sceUserServiceGetInitialUser");
    s32 user = -1;
    if (!search || !initial || initial(&user) != 0)
        return false;
    struct {
        s32 user;
        u32 pad;
        const char *title, *dir;
        u32 key, order;
        u8 reserved[32];
    } c;
    struct {
        u32 hits, pad;
        char *dirs;
        u32 ndirs, set;
        void *params, *infos;
        u8 reserved[12];
    } r;
    char dirs[32];
    memset(&c, 0, sizeof(c)), memset(&r, 0, sizeof(r));
    c.user = user, c.dir = name;
    r.dirs = dirs, r.ndirs = 1;
    return search(&c, &r) == 0 && r.hits > 0;
}

/* G + the title ID's letters and digits, at most 12 (host/profiles.py
 * default_prefix); legacy: before 2026-10-28, G + only the digits of a
 * CUSA-style ID (legacy_prefix), which two titles could share */
static void default_prefix(const char *title, char prefix[16], bool legacy)
{
    bool cusa = strlen(title) == 9;
    for (u32 i = 0; i < 9 && cusa; i++)
        cusa = i < 4 ? title[i] >= 'A' && title[i] <= 'Z' : title[i] >= '0' && title[i] <= '9';
    u32 k = 1;
    prefix[0] = 'G';
    for (const char *c = legacy && cusa ? title + 4 : title; *c && k < 12; c++) {
        char u = *c >= 'a' && *c <= 'z' ? (char)(*c - 32) : *c;
        if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9'))
            prefix[k++] = u;
    }
    prefix[k] = 0;
    if (!legacy && strncmp(prefix, "GFS", 3) == 0)
        prefix[0] = 'T';
    char *e = prefix + k;
    if (!legacy && ((k >= 4 && e[-4] == 'D' && e[-3] >= '0' && e[-3] <= '9' && e[-2] >= '0' && e[-2] <= '9' &&
                     e[-1] >= '0' && e[-1] <= '9') ||
                    (k >= 3 && e[-3] == 'M' && e[-2] >= '0' && e[-2] <= '9' && e[-1] >= '0' && e[-1] <= '9')))
        e[-1] = 'X';
}

/* the save container's prefix: <folder>/prefix, else the title's profile's,
 * else the default; a title played before the default took the whole title
 * ID keeps the legacy prefix its progress is under */
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
    char legacy[16], save[32];
    default_prefix(title, prefix, false);
    default_prefix(title, legacy, true);
    if (strcmp(prefix, legacy) == 0)
        return;
    fmt(save, sizeof(save), "%sSAVE", prefix);
    if (host_save_exists(save))
        return;
    fmt(save, sizeof(save), "%sSAVE", legacy);
    if (host_save_exists(save)) {
        logf("approot: %s's progress is in %s (its prefix before prefixes took the whole title ID): kept", title,
             save);
        strlcpy(prefix, legacy, 16);
    }
}

/* ---- the games under a folder, found by their contents ----
 *
 * Breadth first from the folder: one listing per folder.  A folder with
 * eboot.bin is a game, and so is one with app0/eboot.bin; neither is
 * looked into further.  Others are looked into down to SCAN_DEPTH levels
 * below the folder, SCAN_DIRS folders in all and SCAN_ENTRIES entries per
 * folder, so a large drive stays quick (the console's exFAT lists a folder
 * of thousands of files slowly).  Names starting with "." are skipped
 * (macOS's .Spotlight-V100, .Trashes, ...), Windows' system folders, and
 * the consoles' standard save/export trees. */
#define SCAN_DEPTH 4
#define SCAN_DIRS 256
#define SCAN_ENTRIES 4096
#define LIB_MAX 64 /* the selector's SEL_MAX_TITLES */

static struct lib_title *g_list; /* page_alloc'd while a library is open */
static u32 g_count, g_good;      /* games found; of them bootable */
static u32 g_ps5;                /* PS5 games found and left out (ps5_game) */
static bool g_cut;               /* the scan stopped short */

static void lib_free(void)
{
    if (g_list)
        page_free(g_list, sizeof(struct lib_title) * LIB_MAX);
    g_list = NULL;
    g_count = g_good = g_ps5 = 0;
    g_lib[0] = 0;
}

/* a path as the user knows it: relative to the drive ("My Games/ghost"),
 * the second drive's as "USB 2/My Games/ghost" */
static const char *shown(const char *path)
{
    static char buf[2][280];
    static u32 turn;
    int k = g_usb ? usb_slot_of(path) : -1;
    size_t n = k >= 0 ? strlen(usb_slot_mp((u32)k)) : 0;
    if (k < 0)
        return path;
    if (k == 0)
        return path[n] == '/' ? path + n + 1 : "the USB drive";
    char *b = buf[turn++ & 1];
    fmt(b, sizeof(buf[0]), path[n] == '/' ? "USB %d/%s" : "USB drive %d", k + 1, path + n + 1);
    return b;
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

/* A PS5 game's dump (a PPSA title ID, or sce_sys/param.json and no
 * param.sfo): SWRR runs in PS4 mode, so it can't start one */
static bool ps5_game(const char *app0, const char *id)
{
    if (strncmp(id, "PPSA", 4) == 0)
        return true;
    char sys[300];
    fmt(sys, sizeof(sys), "%s/sce_sys", app0);
    return is_file(sys, "param.json") && !is_file(sys, "param.sfo");
}

/* A game found at app0 (dir_len: its folder); a PS5 game's is left out of
 * the list (counted, and logged) */
static void lib_game(const char *app0, size_t dir_len)
{
    struct lib_title *t = lib_add(app0, dir_len);
    const char *why = t ? identify(app0, dir_len, t->id) : NULL;
    if (t && ps5_game(app0, t->id)) {
        logf("approot: %s: a PS5 game (%s): not listed, SWRR runs PS4 games", shown(app0), t->id[0] ? t->id : "param.json");
        g_count--, g_ps5++;
        return;
    }
    if (why)
        lib_bad(t, why);
}

static bool skip_dir(const char *name)
{
    return name[0] == '.' || same_ci(name, "System Volume Information") || same_ci(name, "$RECYCLE.BIN") ||
           same_ci(name, "sce_sys") || same_ci(name, "sce_module");
}

/* Standard console and Apollo save trees at the drive's top.  Apollo saves
 * have sce_sys but no eboot.bin, so looking inside PS4/APOLLO would otherwise
 * show each one as a broken game.  Match the whole drive-relative path: a
 * real Apollo homebrew copied to ghostldr/apollo remains a game. */
static bool save_tree(const char *path)
{
    int slot = usb_slot_of(path);
    if (slot < 0)
        return false;
    const char *rel = path + strlen(usb_slot_mp((u32)slot));
    if (*rel == '/')
        rel++;
    return same_ci(rel, "PS4/APOLLO") || same_ci(rel, "PS4/SAVEDATA") || same_ci(rel, "PS4/EXPORT") ||
           same_ci(rel, "PS5/APOLLO") || same_ci(rel, "PS5/SAVEDATA") || same_ci(rel, "PS5/EXPORT");
}

/* the games under root (one drive's) into g_list */
static void lib_scan_one(const char *root, char (*q)[256])
{
    u8 *depth = (u8 *)(q + SCAN_DIRS);
    u32 head = 0, tail = 1, listed = 0;
    bool cut = g_cut;
    g_cut = false;
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
                else if (type == 0x4000 && d < SCAN_DEPTH && len < 250) {
                    if (save_tree(path)) {
                        logf("approot: %s: a save/export folder: not listed", shown(path));
                    } else if (!skip_dir(name) && tail < SCAN_DIRS) {
                        strlcpy(q[tail], path, 256);
                        depth[tail++] = (u8)(d + 1);
                    } else if (!skip_dir(name)) {
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
    if (head < tail || g_cut)
        logf("approot: stopped looking after %u folders: keep games within %u folders of %s", listed, SCAN_DEPTH,
             shown(root));
    g_cut |= cut;
}

/* the games under roots[0..n) (the same folder on each drive) into g_list,
 * sorted: bootable ones by title ID then folder, second copies marked, the
 * others last */
static void lib_scan(char (*roots)[256], u32 n)
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
    for (u32 i = 0; i < n; i++)
        lib_scan_one(roots[i], q);
    page_free(q, qsize);
    strlcpy(g_lib, roots[0], sizeof(g_lib));
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
static char g_path[256]; /* usb_open's path below the drives' top ("" for the top): looked in again */

/* the same folder on each drive mounted now, in g_roots (said: each drive's answer logged) */
static void usb_roots(bool said)
{
    char top[64];
    size_t k = 0;
    while (g_path[k] && g_path[k] != '/' && k < sizeof(top) - 1)
        top[k] = g_path[k], k++;
    top[k] = 0;
    g_nroots = 0;
    for (u32 s = 0; s < USB_SLOTS; s++) {
        char mp[24], want[320];
        if (!U.mounted[s])
            continue;
        slot_mp(s, mp);
        fmt(want, sizeof(want), "%s/%s", mp, top);
        bool has = !top[0] || is_dir(want);
        if (top[0] && said)
            logf("approot: %s %s", want, has ? "found" : "missing");
        if (has)
            fmt(g_roots[g_nroots++], sizeof(g_roots[0]), g_path[0] ? "%s/%s" : "%s", mp, g_path);
    }
}

/* ghost_api.library_scan (the game selector's refresh): again nonzero looks
 * at the drives again, as an FTP login does (a drive gone is forgotten, one
 * plugged in since mounted), and lists the games in the same folder on each
 * again; *ps5 gets the PS5 games left out.  The count, or -1 when this
 * boot's games don't come from a folder of games (any more). */
static int lib_again(const struct lib_title **list, u32 *ps5, int again)
{
    *list = NULL;
    if (!g_list)
        return -1;
    if (again) {
        if (g_usb) {
            char mp[24];
            spin_lock(&g_mount_lock);
            for (u32 k = 0; k < USB_SLOTS; k++)
                if (U.mounted[k] && !is_dir(slot_mp(k, mp))) {
                    logf("approot: the USB drive at %s is gone", mp);
                    U.mounted[k] = false;
                }
            usb_mount_new();
            usb_roots(false);
            spin_unlock(&g_mount_lock);
        }
        char keep[256];
        strlcpy(keep, g_lib, sizeof(keep));
        lib_scan(g_roots, g_nroots);
        if (!g_nroots) /* no drive has the folder now: the library stays where it was, empty */
            strlcpy(g_lib, keep, sizeof(g_lib));
        logf("approot: looked again: %u game%s in %u folder%s%s", g_good, g_good == 1 ? "" : "s", g_nroots,
             g_nroots == 1 ? "" : "s", g_ps5 ? " (PS5 games left out, above)" : "");
    }
    *list = g_list;
    if (ps5)
        *ps5 = g_ps5;
    return g_list ? (int)g_count : -1;
}

/* a boot that stops: the drive goes back; "auto" boots the installed titles instead */
static int give_up(void)
{
    usb_unmount();
    lib_free();
    if (g_auto)
        logf("approot: booting an installed title instead");
    return g_auto ? 2 : -1;
}

/* the title's drive, to know it again: its slot, and its eboot.bin's size
 * and time (usb_live.c) */
static void ident_take(void)
{
    char p[300];
    u8 st[0x78];
    fmt(p, sizeof(p), "%s/eboot.bin", g_root);
    g_ident.slot = usb_slot_of(g_root);
    if (g_ident.slot >= 0 && vfs_stat(p, st) == 0) {
        g_ident.size = *(s64 *)(st + 0x48);
        g_ident.mtime = *(s64 *)(st + 0x28);
    } else {
        g_ident.slot = -1;
    }
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
        NOTE(FAIL_NO_TITLE_ID, shown(app0));
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
        ident_take();
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
        strlcpy(g_roots[0], r, sizeof(g_roots[0]));
        g_nroots = 1;
    }
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/')
        dir[--n] = 0;
    for (u32 i = 0; i < g_nroots; i++)
        for (size_t k = strlen(g_roots[i]); k > 1 && g_roots[i][k - 1] == '/';)
            g_roots[i][--k] = 0;
    const char *want = profile_requested();
    if (g_nroots == 1 && is_file(dir, "eboot.bin")) { /* one game: its ID from --title, else as a found game's */
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
    lib_scan(g_roots, g_nroots);
    bool top = g_usb && usb_slot_of(dir) >= 0 && strrchr_c(dir, '/') == dir;
    char where[64];
    if (top)
        fmt(where, sizeof(where), g_nroots > 1 ? "on %u USB drives" : "on the USB drive", g_nroots);
    logf("approot: %u game%s %s%s%s%s", g_good, g_good == 1 ? "" : "s", top ? where : "in ", top ? "" : shown(dir),
         g_count > g_good ? " (and folders that can't be booted, above)" : "", g_ps5 ? " (PS5 games left out, above)" : "");
    if (!g_good) {
        logf("approot: %s holds no games (a folder with eboot.bin, or with app0/eboot.bin)", shown(dir));
        NOTE(FAIL_NO_GAMES, shown(dir));
        if (!g_usb && !g_auto) { /* a dev directory: the boot says what is missing, as before */
            lib_free();
            return 0;
        }
        return give_up();
    }
    if (want && !lib_find(want)) {
        logf("approot: %s is not among the games in %s", want, shown(dir));
        NOTE(FAIL_NOT_ON_DRIVE, want);
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

/* the drives' directory for path ("/ghostldr" -> "/ghostldr_usb/ghostldr",
 * or route B's "/usb0/ghostldr"), in out; g_roots: the same folder on each
 * drive that has its top folder (every drive for the drive's top, "usb:/"
 * and "auto"), their games listed together */
static int usb_open(const char *path, char *out, size_t size)
{
    while (*path == '/')
        path++;
    char top[64];
    size_t k = 0;
    while (path[k] && path[k] != '/' && k < sizeof(top) - 1)
        top[k] = path[k], k++;
    top[k] = 0;
    u32 m = usb_mount_all();
    strlcpy(g_path, path, sizeof(g_path));
    usb_roots(true);
    if (g_nroots) {
        g_usb = true;
        strlcpy(out, g_roots[0], size);
        return 0;
    }
    if (m) {
        logf("approot: no USB drive has /%s", top);
        NOTE(FAIL_NO_FOLDER, top);
    }
    usb_unmount();
#ifdef GHOST_USB_RAW
    if ((g_raw = usb_attach((u32)g_ctx->flags)) && appfs_register(g_raw) == 0) {
        logf("approot: the extension reads the USB drive itself (route B)");
        g_usb = true;
        fmt(out, size, *path ? "/usb0/%s" : "/usb0", path);
        strlcpy(g_roots[0], out, sizeof(g_roots[0]));
        g_nroots = 1;
        return 0;
    }
#endif
    return -1;
}

/* a save tool's scan before the title loads (ghost_api.save_prescan) */
int save_prescan(u32 user, int (*progress)(void *arg, u32 done, u32 total), void *arg) HIDDEN;

/* the game selector's FTP server (ftp_sys.c; the ELF loader's extension has a stand-in server, elfldr_only.c) */
int ftp_early_start(u32 port) HIDDEN;
int ftp_early_stop(void) HIDDEN;

const struct ext_early g_approot_early HIDDEN = {EXT_EARLY_MAGIC, sizeof(struct ext_early), approot_init,
                                                  approot_pick, lib_get, save_prescan, ftp_early_start,
                                                  ftp_early_stop, lib_again};
