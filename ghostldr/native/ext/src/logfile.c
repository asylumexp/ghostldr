/*
 * The log in files (docs/logs.md): /temp0/ghostldr.log always, and this
 * boot's own file on a USB drive when one is plugged in and the "Log to USB"
 * setting is on.
 *
 *  - The core writes every line to /temp0 from the first one on (log.c).
 *    Once this code is placed it takes the lines over (struct ghost_diag's
 *    sink, with the core's log lock held): both files, one write per line, so
 *    a crash loses none.  Past its cap a file is renamed to its .1 name
 *    (replacing the one before) and starts again: /temp0's at 8 MiB, a
 *    drive's at 32 MiB.
 *  - Attaching the drive's file (at ext_init, before a boot failure's
 *    message, again after a drive came back): what /temp0 has of this boot
 *    and the drive's file doesn't is copied first, so the drive's file is the
 *    whole boot however late it was attached, or how often.
 *  - Which drive: the one the game came from (rel-usb's usb_title_slot), else
 *    the first that takes a write in ghostldr/logs.  Nothing is written
 *    outside <drive>/ghostldr/; the only files ever removed are this code's
 *    own logs past the 30 newest.  Name: ghostldr/logs/NNNN_YYYY-MM-DD_HHMM.log,
 *    NNNN one more than the highest there.
 *  - The setting: ctx flag CTX_FLAG_NO_USB_LOG (Settings keep it in GLDRCONF
 *    and ghostldr.lua sets it; the selector sets it live, before ext_init).
 *  - The drive's file is synced (fsync) once a second by the monitor
 *    (failure.c), and before every failure's message.
 */
#include "ext.h"

#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

#define O_APPEND 0x8
#define O_DIRECTORY 0x20000
#define LOG_DRIVE_CAP (32u << 20)
#define LOG_KEEP 30
#define USB_MP "/ghostldr_usb"

/* rel-usb (hle/rel-usb): every drive mounted, at /ghostldr_usb, /ghostldr_usb2..4,
 * in sceUsbStorageGetDeviceList order; the slot the title's drive is in */
int usb_title_slot(void) HIDDEN __attribute__((weak));
/* approot.c: the first drive mounted at /ghostldr_usb when none is */
void usb_ftp_prepare(void) HIDDEN;

static struct {
    int (*rename)(const char *, const char *);
    int (*unlink)(const char *);
    int (*fsync)(int);
    int (*mkdir)(const char *, int);
    int (*getdents)(int, void *, int);
    s32 (*clock)(void *);
    s32 (*list)(u32 *, s32 *);
    char name[32]; /* the drive's, for messages */
} L;

HIDDEN struct ghost_diag *diag_state(void) { return G ? GHOST_DIAG(G) : NULL; }

static void lock(struct ghost_diag *D) { spin_lock(&D->lock); }
static void unlock(struct ghost_diag *D) { spin_unlock(&D->lock); }

/* a file past its cap: renamed to its .1 name and started again (lock held) */
static void rotate(struct ghost_diag *D, struct ghost_log_file *f, u32 i)
{
    char old[136];
    size_t n = strlen(f->path);
    if (!L.rename || n < 4)
        return;
    fmt(old, sizeof(old), "%.*s.1.log", (int)(n - 4), f->path);
    if (L.unlink)
        L.unlink(old);
    L.rename(f->path, old);
    int fd = K.open(f->path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
    K.close(f->fd);
    f->fd = fd, f->bytes = 0;
    if (!i)
        D->start = D->copied = 0;
}

static void sink(const char *line, size_t n)
{
    struct ghost_diag *D = diag_state();
    bool wrote0 = false;
    for (u32 i = 0; D && i < 2; i++) {
        struct ghost_log_file *f = &D->f[i];
        if (f->fd >= 0 && f->bytes + n > (i ? LOG_DRIVE_CAP : LOG_TEMP_CAP))
            rotate(D, f, i);
        if (f->fd < 0)
            continue;
        if (K.write(f->fd, line, n) == (ssize_t)n) {
            f->bytes += n;
            if (!i)
                wrote0 = true;
            else
                D->since_sync += n, D->copied = D->f[0].bytes;
        } else if (i) { /* the drive went away: its file ends; /temp0 goes on */
            K.close(f->fd);
            f->fd = -1, f->lost = 1;
            D->copied = D->f[0].bytes - (wrote0 ? n : 0);
        }
    }
}

/* the drive's file to disk (the monitor, failures) */
HIDDEN void log_sync(void)
{
    struct ghost_diag *D = diag_state();
    if (D && L.fsync && D->f[1].fd >= 0 && D->since_sync) {
        D->since_sync = 0;
        L.fsync(D->f[1].fd);
    }
}

/* ---- the setting: Settings' "Write logs to the USB drive", a ctx flag
 * (ghostldr.lua applies it from GLDRCONF; the selector sets it live) ---- */
static bool setting_on(void) { return !(g_ctx->flags & CTX_FLAG_NO_USB_LOG); }

/* ---- the drive ---- */
static bool is_dir(const char *path)
{
    u8 st[0x78];
    return vfs_stat(path, st) == 0 && (*(u16 *)(st + 8) & 0xF000) == 0x4000;
}

static void slot_mp(int slot, char *mp, size_t size)
{
    if (slot)
        fmt(mp, size, "%s%d", USB_MP, slot + 1);
    else
        strlcpy(mp, USB_MP, size);
}

/* the drives plugged in (0 when the USB storage session isn't open) */
static s32 drives(void)
{
    u32 ids[16];
    s32 n = 0;
    return L.list && L.list(ids, &n) == 0 && n > 0 ? n : 0;
}

/* "USB drive", or "USB drive 2 of 2" (rel-settings' Settings screen names them) */
static void drive_name(int slot)
{
    s32 n = drives();
    if (n > 1)
        fmt(L.name, sizeof(L.name), "USB drive %d of %d", slot + 1, n);
    else
        strlcpy(L.name, "USB drive", sizeof(L.name));
}

/* the highest NNNN of ghostldr/logs' files; old ones past the newest LOG_KEEP removed */
static u32 last_number(const char *dir, u32 drop_below)
{
    u32 best = 0;
    int fd = K.open(dir, O_RDONLY | O_DIRECTORY, 0);
    u8 *buf = fd >= 0 && L.getdents ? page_alloc(0x4000) : NULL;
    int got;
    while (buf && (got = L.getdents(fd, buf, 0x4000)) > 0)
        for (int off = 0; off + 8 < got;) {
            const u8 *d = buf + off;
            u16 reclen = *(const u16 *)(d + 4);
            const char *name = (const char *)d + 8;
            if (!reclen)
                break;
            off += reclen;
            u32 v = 0, k = 0;
            for (; k < 4 && name[k] >= '0' && name[k] <= '9'; k++)
                v = v * 10 + (u32)(name[k] - '0');
            size_t len = strlen(name);
            if (k != 4 || name[4] != '_' || len < 9 || strcmp(name + len - 4, ".log") != 0)
                continue; /* not one of ours */
            if (v < drop_below && L.unlink) {
                char p[192];
                fmt(p, sizeof(p), "%s/%s", dir, name);
                L.unlink(p);
            } else if (v > best) {
                best = v;
            }
        }
    if (buf)
        page_free(buf, 0x4000);
    if (fd >= 0)
        K.close(fd);
    return best;
}

/* this boot's file on the drive at mp: its path in D->f[1].path; false when the drive takes no write */
static bool make_file(struct ghost_diag *D, const char *mp)
{
    char dir[64];
    fmt(dir, sizeof(dir), "%s/ghostldr", mp);
    if (L.mkdir)
        L.mkdir(dir, 0777);
    fmt(dir, sizeof(dir), "%s/ghostldr/logs", mp);
    if (L.mkdir)
        L.mkdir(dir, 0777);
    if (!is_dir(dir))
        return false;
    u32 n = last_number(dir, 0) + 1;
    if (n > LOG_KEEP)
        last_number(dir, n - LOG_KEEP + 1);
    struct {
        u16 year, month, day, hour, minute, second;
        u32 us;
    } t = {0};
    if (n > 9999)
        n = 1;
    if (L.clock && L.clock(&t) == 0 && t.year)
        fmt(D->f[1].path, sizeof(D->f[1].path), "%s/%04u_%04u-%02u-%02u_%02u%02u.log", dir, n, t.year, t.month, t.day,
            t.hour, t.minute);
    else
        fmt(D->f[1].path, sizeof(D->f[1].path), "%s/%04u_boot.log", dir, n);
    return true;
}

/* where the log goes, for the messages and Settings */
static void say_where(struct ghost_diag *D, const char *how)
{
    const char *p = D->f[1].path, *rel = strstr(p, "/ghostldr/logs/");
    if (D->f[1].fd >= 0 && rel)
        fmt(D->where, sizeof(D->where), "%s: %s", L.name, rel + 1);
    else
        fmt(D->where, sizeof(D->where), "%s/temp0/ghostldr.log (wiped when the console restarts)", how);
}

/* the drive's file opened (append) and brought up to date from /temp0 */
static bool open_on(struct ghost_diag *D, const char *mp, int slot)
{
    if (!D->f[1].path[0] || strncmp(D->f[1].path, mp, strlen(mp)) != 0 || D->f[1].path[strlen(mp)] != '/')
        if (!make_file(D, mp))
            return false;
    int fd = K.open(D->f[1].path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) {
        logf("log: %s: not writable (%x)", D->f[1].path, fd);
        D->f[1].path[0] = 0;
        return false;
    }
    off_t end = K.lseek(fd, 0, 2);
    u8 *buf = page_alloc(0x10000);
    int src = K.open(LOG_TEMP, O_RDONLY, 0);
    lock(D);
    u64 from = D->copied <= D->f[0].bytes ? D->copied : D->f[0].bytes, copied = 0;
    while (buf && src >= 0 && from < D->f[0].bytes) {
        u64 want = D->f[0].bytes - from < 0x10000 ? D->f[0].bytes - from : 0x10000;
        ssize_t r = K.pread(src, buf, want, (off_t)from);
        if (r <= 0 || K.write(fd, buf, (size_t)r) != r)
            break;
        from += (u64)r, copied += (u64)r;
    }
    D->f[1].fd = fd, D->f[1].lost = 0;
    D->f[1].bytes = (end > 0 ? (u64)end : 0) + copied;
    D->copied = D->f[0].bytes;
    unlock(D);
    if (src >= 0)
        K.close(src);
    if (buf)
        page_free(buf, 0x10000);
    drive_name(slot);
    say_where(D, "");
    logf("log: on %s at %s (%lu bytes of this boot copied from %s)", L.name, D->f[1].path, copied, LOG_TEMP);
    return true;
}

/* attach the drive's file, if the setting and a drive allow: true when attached */
HIDDEN bool log_attach(void)
{
    struct ghost_diag *D = diag_state();
    if (!D || D->f[1].fd >= 0)
        return D && D->f[1].fd >= 0;
    if (!setting_on()) {
        say_where(D, "Logging to USB is off: ");
        logf("log: logging to a USB drive is off (Settings)");
        return false;
    }
    int first = usb_title_slot ? usb_title_slot() : -1;
    if (!usb_title_slot) { /* one drive at a time here: the first one, mounted now if it isn't (the
                            * mount point stays a folder after an unmount: approot.c knows) */
        u32 note = D->note; /* (its "no drive" note is not why a boot stopped) */
        char path[sizeof(D->note_path)];
        memcpy(path, D->note_path, sizeof(path));
        usb_ftp_prepare();
        D->note = note;
        memcpy(D->note_path, path, sizeof(path));
    }
    char mp[32];
    for (int k = -1; k < (usb_title_slot ? 4 : 1); k++) {
        int slot = k < 0 ? first : k;
        if (slot < 0 || (k >= 0 && slot == first))
            continue;
        slot_mp(slot, mp, sizeof(mp));
        if (is_dir(mp) && open_on(D, mp, slot))
            return true;
    }
    say_where(D, "No USB drive: ");
    logf("log: no USB drive takes the log: %s only", LOG_TEMP);
    return false;
}

/* the drive's file closed (a boot that stopped; the title's drive is about to go) */
HIDDEN void log_detach(void)
{
    struct ghost_diag *D = diag_state();
    if (!D || D->f[1].fd < 0)
        return;
    log_sync();
    lock(D);
    K.close(D->f[1].fd);
    D->f[1].fd = -1;
    D->copied = D->f[0].bytes;
    unlock(D);
}

/* the extension's code is placed (ext_early, ext_init): it takes the lines */
HIDDEN void log_placed(void)
{
    struct ghost_diag *D = diag_state();
    if (!D)
        return;
    L.mkdir = host_real("sceKernelMkdir");
    L.getdents = host_real("sceKernelGetdents");
    L.clock = host_real_in("libSceRtc", "sceRtcGetCurrentClockLocalTime");
    u32 h = 0;
    if (host_module_handle("libSceUsbStorage", &h) == 0)
        L.list = host_dlsym(h, "sceUsbStorageGetDeviceList");
    L.rename = host_real("sceKernelRename");
    L.unlink = host_real("sceKernelUnlink");
    L.fsync = host_real("sceKernelFsync");
    if (!D->f[0].path[0])
        strlcpy(D->f[0].path, LOG_TEMP, sizeof(D->f[0].path));
    D->sink = sink;
    D->drives = (u8)drives();
    if (!D->where[0])
        say_where(D, "");
}

#pragma clang attribute pop
