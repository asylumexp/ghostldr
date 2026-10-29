/*
 * A title on a USB drive while it runs (docs/usb.md "Pulling the drive out,
 * rest mode"): the drive may go away under it, pulled out or powered off in
 * rest mode, and come back.
 *
 *   - Its fds on the drive are tracked (hle_file.c: usb_live_open and
 *     usb_live_close): the path each was opened with, and its flags.
 *   - A call on one that fails as a lost device does (EIO, ENXIO, EBADF,
 *     ENODEV, ESTALE), or on a path on the drive that fails while the drive
 *     doesn't answer, waits here (usb_live_fd_failed, usb_live_path_failed)
 *     until the drive is mounted again (approot.c usb_slot_reattach): the
 *     same drive, known by the title's eboot.bin.  Every tracked fd is then
 *     opened again onto its own number, at its offset, and the call
 *     is made once more.  The title sees a slow call, not an error.
 *   - A thread (ghostldr-usb) checks the drive every 2 seconds and notices
 *     rest mode (the wall clock jumps past its sleep), so the drive is
 *     mounted again before the title asks for it, and the user is told.
 *   - What the user must do is said in a notification on the TV, and on
 *     the title's frames where the overlay can draw (dlg_overlay.c): "the
 *     drive was removed: plug it back in", "this drive doesn't have the
 *     game", "the drive is back".  While the drive is gone the title's
 *     calls on it wait; nothing on the drive is written meanwhile, and the
 *     title's saves (in SWRR's save data, not on the drive) are untouched.
 */
#include "kmod.h"

int usb_title_slot(void) HIDDEN;           /* approot.c */
int usb_slot_of(const char *path) HIDDEN;  /* approot.c */
const char *usb_slot_mp(u32 slot) HIDDEN;  /* approot.c: its mount point now */
int usb_slot_check(int slot) HIDDEN;       /* approot.c */
int usb_slot_reattach(int slot) HIDDEN;    /* approot.c */
void ftp_after_resume(void) HIDDEN __attribute__((weak)); /* ftp_sys.c */
void diag_notice(const char *text, u32 seconds) HIDDEN __attribute__((weak)); /* failure.c (rel-diag) */

#define LIVE_FDS 256
#define LIVE_PATH 240
#define O_CREAT_ 0x200
#define O_TRUNC_ 0x400
#define O_EXCL_ 0x800
#define SEEK_SET_ 0
#define SEEK_CUR_ 1

struct live_fd {
    s32 fd; /* -1: free */
    s32 flags;
    s32 slot;             /* the drive's (approot.c) */
    s64 off;              /* its offset as the title's reads, writes and seeks left it */
    char path[LIVE_PATH]; /* where SWRR opened it, after the drive's mount point (which may move) */
};

static struct live_fd *g_fds; /* page_alloc'd at the first fd on a drive */
static u64 g_bits[16];        /* fds below 1024 in g_fds: the calls on others return at once */
static volatile int g_fd_lock;
static volatile u32 g_gen;     /* recoveries done */
static volatile int g_rec_lock; /* one recovery at a time */
static volatile bool g_lost;   /* the title's drive is known to be gone */

static struct {
    int (*dup)(int);
    s64 (*lseek)(int, s64, int);
    int (*open)(const char *, int, int);
    int (*close)(int);
    int (*gettimeofday)(void *);
    int (*notify)(int, void *, u64, int);
    bool done;
} L;

static void syms(void)
{
    if (L.done)
        return;
    u32 lk = (u32)g_ctx->libkernel_handle;
    *(void **)&L.dup = host_dlsym(lk, "dup");
    *(void **)&L.lseek = host_dlsym(lk, "lseek");
    *(void **)&L.open = host_dlsym(lk, "open");
    *(void **)&L.close = host_dlsym(lk, "close");
    *(void **)&L.gettimeofday = host_dlsym(lk, "sceKernelGettimeofday");
    *(void **)&L.notify = host_dlsym(lk, "sceKernelSendNotificationRequest");
    L.done = true;
}

/* ---- what the user sees ---- */

/* A notification on the TV (ShellCore's, over any title), the overlay's
 * banner where it can draw, and the log.  The request is read by the
 * kernel: on this thread's stack. */
static void tell(const char *text, u32 banner_s)
{
    struct {
        s32 type, req_id, priority, msg_id, target_id, user_id, unk1, unk2, app_id, error_num, unk3;
        u8 use_icon;
        char message[1024];
        char icon[1024];
        char unk[1024];
        u8 pad[3];
    } __attribute__((packed)) r; /* 0xC30 bytes */
    _Static_assert(sizeof(r) == 0xC30, "notification request");
    syms();
    logf("usb: %s", text);
    if (diag_notice) { /* the failure display's: a notification and the banner */
        diag_notice(text, banner_s);
        return;
    }
    if (L.notify) {
        memset(&r, 0, sizeof(r));
        r.target_id = -1;
        fmt(r.message, sizeof(r.message), "ghostldr: %s", text);
        L.notify(0, &r, sizeof(r), 0);
    }
    ov_banner(text, banner_s);
}

/* ---- the title's fds on a drive ---- */

static bool tracked(s32 fd) { return fd >= 0 && (fd >= 1024 || (g_bits[fd >> 6] >> (fd & 63) & 1)); }
static void mark(s32 fd, bool on)
{
    if (fd >= 0 && fd < 1024)
        g_bits[fd >> 6] = on ? g_bits[fd >> 6] | 1ull << (fd & 63) : g_bits[fd >> 6] & ~(1ull << (fd & 63));
}

static struct live_fd *find(s32 fd)
{
    for (u32 i = 0; g_fds && i < LIVE_FDS; i++)
        if (g_fds[i].fd == fd)
            return &g_fds[i];
    return NULL;
}

/* hle_file.c: the title opened path (SWRR's, under a drive) as fd */
HIDDEN void usb_live_open(s32 fd, const char *path, int flags)
{
    static u32 full;
    int slot = usb_slot_of(path);
    if (fd < 0 || slot < 0)
        return;
    path += strlen(usb_slot_mp((u32)slot));
    if (strlen(path) >= LIVE_PATH)
        return;
    spin_lock(&g_fd_lock);
    if (!g_fds && (g_fds = page_alloc(sizeof(struct live_fd) * LIVE_FDS)) != NULL)
        for (u32 i = 0; i < LIVE_FDS; i++)
            g_fds[i].fd = -1;
    struct live_fd *e = find(fd);
    if (!e)
        e = find(-1);
    if (e) {
        e->fd = fd;
        e->flags = flags & ~(O_CREAT_ | O_TRUNC_ | O_EXCL_);
        e->off = 0;
        e->slot = slot;
        strlcpy(e->path, path, sizeof(e->path));
        mark(fd, true);
    }
    spin_unlock(&g_fd_lock);
    if (!e && full++ < 4)
        logf("usb: more than %u files open on the drive: fd %d can't be opened again if the drive goes away", LIVE_FDS,
             fd);
}

/* hle_file.c: fd is being closed (or replaced by dup2) */
HIDDEN void usb_live_close(s32 fd)
{
    if (!g_fds || !tracked(fd))
        return;
    spin_lock(&g_fd_lock);
    struct live_fd *e = find(fd);
    if (e)
        e->fd = -1;
    mark(fd, false);
    spin_unlock(&g_fd_lock);
}

/* hle_file.c: a read, write or seek on fd moved its offset (by n, or to n) */
HIDDEN void usb_live_moved(s32 fd, s64 n, bool to)
{
    if (!g_fds || !tracked(fd))
        return;
    spin_lock(&g_fd_lock);
    struct live_fd *e = find(fd);
    if (e)
        e->off = to ? n : e->off + n;
    spin_unlock(&g_fd_lock);
}

/* hle_file.c: to is a copy of from (dup, dup2) */
HIDDEN void usb_live_dup(s32 from, s32 to)
{
    if (!g_fds || from < 0 || to < 0 || from == to)
        return;
    struct live_fd copy;
    bool have = false;
    spin_lock(&g_fd_lock);
    struct live_fd *e = find(from);
    if (e)
        memcpy(&copy, e, sizeof(copy)), have = true;
    spin_unlock(&g_fd_lock);
    char full[LIVE_PATH + 32];
    if (have)
        fmt(full, sizeof(full), "%s%s", usb_slot_mp((u32)copy.slot), copy.path), usb_live_open(to, full, copy.flags),
            usb_live_moved(to, copy.off, true);
    else
        usb_live_close(to);
}

/* fd e opened again onto its own number, at its offset (the dead fd's,
 * else as the title's calls left it): 0, or the errno */
static int reopen(struct live_fd *e)
{
    s64 off = L.lseek(e->fd, 0, SEEK_CUR_);
    if (off != e->off)
        logf("usb: fd %d: offset %ld, %ld by the title's calls", e->fd, off, e->off);
    if (off < 0)
        off = e->off;
    char full[LIVE_PATH + 32];
    fmt(full, sizeof(full), "%s%s", usb_slot_mp((u32)e->slot), e->path);
    int nfd = L.open(full, e->flags, 0);
    if (nfd < 0)
        return K.errno_fn ? *K.errno_fn() : 5;
    if (off > 0)
        L.lseek(nfd, off, SEEK_SET_);
    if (nfd == e->fd)
        return 0;
    /* onto the old number: dup2 is EPERM in SWRR's sandbox (PS5 13.60), so
     * the old fd is closed and dup fills the lowest free numbers until it
     * gives that one; the fillers are closed again */
    s32 fill[256];
    u32 n = 0;
    int got = -1;
    L.close(e->fd);
    while ((got = L.dup(nfd)) >= 0 && got < e->fd && n < ARRAY_SIZE(fill))
        fill[n++] = got;
    while (n)
        L.close(fill[--n]);
    L.close(nfd);
    if (got == e->fd)
        return 0;
    if (got >= 0)
        L.close(got);
    return got < 0 ? (K.errno_fn ? *K.errno_fn() : 5) : 16; /* EBUSY: another file took the number first */
}

/* every tracked fd on slot opened again: how many could not be */
static u32 reopen_all(int slot)
{
    u32 n = 0, bad = 0;
    spin_lock(&g_fd_lock);
    for (u32 i = 0; g_fds && i < LIVE_FDS; i++) {
        struct live_fd *e = &g_fds[i];
        if (e->fd < 0 || e->slot != slot)
            continue;
        int rc = reopen(e);
        n++;
        if (rc) {
            if (bad++ < 8)
                logf("usb: fd %d (%s) can't be opened again: errno %d", e->fd, e->path, rc);
        }
    }
    spin_unlock(&g_fd_lock);
    logf("usb: %u of the title's files on the drive opened again%s", n - bad, bad ? " (the others stay failed)" : "");
    return bad;
}

/* ---- waiting for the drive ---- */

static s64 now_s(void)
{
    s64 tv[2] = {0, 0};
    return L.gettimeofday && L.gettimeofday(tv) == 0 ? tv[0] : 0;
}

/* The title's drive (slot) mounted again, waiting as long as it takes and
 * saying what to do; why: what noticed.  Several threads may ask at once:
 * one waits on the drive, the others for it. */
static void recover(int slot, const char *why)
{
    u32 gen = __atomic_load_n(&g_gen, __ATOMIC_ACQUIRE);
    while (__atomic_exchange_n(&g_rec_lock, 1, __ATOMIC_ACQUIRE)) {
        K.usleep(50000);
        if (__atomic_load_n(&g_gen, __ATOMIC_ACQUIRE) != gen)
            return; /* another thread brought it back */
    }
    if (usb_slot_check(slot) == 0 && !g_lost) {
        __atomic_store_n(&g_rec_lock, 0, __ATOMIC_RELEASE);
        return;
    }
    syms();
    int chk = usb_slot_check(slot);
    logf("usb: the title's USB drive doesn't answer (%s; check: errno %d)", why, chk);
    g_lost = true;
    s64 t0 = now_s(), told = 0;
    int last = -2, rc;
    bool said = false;
    while ((rc = usb_slot_reattach(slot)) != 0) {
        s64 t = now_s();
        /* a drive coming back from rest mode takes a moment: nothing said in the first 3 s */
        if ((rc != last || t - told >= 60) && t - t0 >= 3) {
            if (rc == 2)
                tell("this USB drive doesn't have the game on it. Plug in the drive the game was started from.", 65);
            else if (rc < 0)
                tell("the USB drive can't be reached. Plug it back in; if that doesn't help, close SWRR and start "
                     "the game again (its saves are kept).",
                     65);
            else
                tell("the USB drive was removed. Plug it back in: the game carries on when it's back.", 65);
            said = true;
            told = t;
            last = rc;
        }
        K.usleep(rc == 1 ? 500000 : 1000000);
    }
    reopen_all(slot);
    g_lost = false;
    __atomic_fetch_add(&g_gen, 1, __ATOMIC_RELEASE);
    logf("usb: the title's drive is back after %ld s", now_s() - t0);
    if (said)
        tell("the USB drive is back: the game carries on.", 4);
    __atomic_store_n(&g_rec_lock, 0, __ATOMIC_RELEASE);
}

/* errno values a call on a drive gives when the drive went away */
static bool lost_errno(s32 e, bool path)
{
    return e == 5 /* EIO */ || e == 6 /* ENXIO */ || e == 19 /* ENODEV */ || e == 70 /* ESTALE */ ||
           (!path && e == 9 /* EBADF */) || (path && (e == 2 /* ENOENT */ || e == 20 /* ENOTDIR */));
}

/* hle_file.c: a call on fd failed with errno e.  True when the call should
 * be made again: the fd is the title's, on a drive that went away and is
 * back now (or the fd alone failed and is open again). */
HIDDEN bool usb_live_fd_failed(s32 fd, s32 e)
{
    if (!g_fds || !tracked(fd) || !lost_errno(e, false))
        return false;
    spin_lock(&g_fd_lock);
    struct live_fd *f = find(fd);
    int slot = f ? f->slot : -1;
    spin_unlock(&g_fd_lock);
    if (slot < 0 || slot != usb_title_slot())
        return false;
    static u32 said;
    if (said++ < 16)
        logf("usb: fd %d on the drive failed: errno %d", fd, e);
    if (g_lost || usb_slot_check(slot) != 0) {
        recover(slot, "a call on the title's file failed");
        return true;
    }
    /* the drive answers: this fd alone went bad (rest mode may do that) */
    syms();
    spin_lock(&g_fd_lock);
    f = find(fd);
    int rc = f ? reopen(f) : 9;
    spin_unlock(&g_fd_lock);
    if (said < 16)
        logf("usb: fd %d opened again: errno %d", fd, rc);
    return rc == 0;
}

/* hle_file.c: a call on path (SWRR's, maybe under a drive) failed with
 * errno e.  True when the call should be made again: the path is on the
 * title's drive, which doesn't answer, and is back now. */
HIDDEN bool usb_live_path_failed(const char *path, s32 e)
{
    int slot = usb_slot_of(path);
    if (slot < 0 || slot != usb_title_slot() || !lost_errno(e, true))
        return false;
    if (!g_lost && usb_slot_check(slot) == 0)
        return false; /* the drive answers: the file really isn't there */
    recover(slot, "a path on the title's drive failed");
    return true;
}

/* for the failure monitor (rel-diag): the title's threads may be waiting
 * for its drive (no hang), and recoveries so far (a log file on the drive
 * is opened again when this changes) */
HIDDEN bool usb_live_waiting(void) { return g_lost; }
HIDDEN u32 usb_live_gen(void) { return __atomic_load_n(&g_gen, __ATOMIC_ACQUIRE); }

/* ---- the watch ---- */

static void *watch(void *arg)
{
    u32 tick = 0;
    for (;;) {
        s64 last = now_s(); /* rest mode: the clock jumps across this sleep (not across a wait of ours) */
        K.usleep(1000000);
        s64 t = now_s();
        if (last && t - last > 8) {
            int slot = usb_title_slot();
            logf("usb: back from rest mode (SWRR was stopped for about %ld s); the title's drive: %d", t - last - 1,
                 slot >= 0 ? usb_slot_check(slot) : -1);
            if (ftp_after_resume)
                ftp_after_resume();
        }
        int slot = usb_title_slot();
        if (slot >= 0 && (++tick % 2 == 0 || g_lost) && usb_slot_check(slot) != 0)
            recover(slot, "the drive check");
    }
    return arg;
}

/* the title is about to start (ext_main.c title_starting): the watch */
HIDDEN void usb_live_start(void)
{
    static bool started;
    if (started)
        return;
    started = true;
    syms();
    if (!L.gettimeofday || !L.dup || !L.open || !L.lseek || !L.close) {
        logf("usb: libkernel lacks what the drive watch needs");
        return;
    }
    u8 attr[0x40];
    u64 tid = 0;
    K.pthread_attr_init(attr);
    K.pthread_attr_setstacksize(attr, 64 << 10);
    int rc = K.pthread_create(&tid, attr, watch, NULL, "ghostldr-usb");
    K.pthread_attr_destroy(attr);
    if (rc == 0)
        ext_thread_note(tid, "ghostldr-usb");
    int slot = usb_title_slot();
    logf("usb: watch %s%s", rc == 0 ? "started" : "not started",
         slot >= 0 ? ": the title runs from a USB drive; pulled out or in rest mode, it waits for it" : "");
}
