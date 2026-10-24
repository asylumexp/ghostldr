/*
 * usbfs: a title's /app0 served from a folder on a USB drive (docs/usb.md,
 * route B), with the semantics ghostfs gives titles:
 *
 *   - open, stat and the fd calls answer with ghostfs's contracts (struct
 *     appfs_ops in ghost_api.h): 1 for paths that are not usbfs's, SCE
 *     kernel errors, EROFS for writes;
 *   - an open file is a real placeholder fd (us_placeholder_fd: SWRR's own
 *     /app0 directory), so fd numbers and limits stay the kernel's and a
 *     call that misses the wrappers fails instead of reading SWRR's files;
 *   - stat fills FreeBSD's struct stat as ghostfs does, with the volume's
 *     own times; a time before 1980 (none recorded) becomes the one ghostfs
 *     reports, since titles drop requests for files with a zero mtime;
 *   - getdents gives FreeBSD dirents of the directory's children (no "."
 *     or ".."), resuming where the last call stopped; macOS's AppleDouble
 *     files ("._name") are left out.
 *
 * Paths: /usb0/... is the volume (the core lists title folders there) and
 * /app0/... the title's app0 once usbfs_set_root has named it.  "." and
 * ".." components and repeated slashes are resolved here; names compare
 * case-insensitively, as the volume's own file system does.
 *
 * One lock covers the volume (one walk or device transfer at a time); it
 * sleeps, since a USB transfer takes milliseconds.  Directories found on
 * the way to a file are remembered (UFS_DIRS), so opening many files in one
 * folder walks its parents once.
 */
#include "usbstore.h"

#define UFS_FILES 512
#define UFS_DIRS 64
#define VPATH_MAX 1024
#define O_DIRECTORY_ 0x20000
#define NO_TIME 1477944229 /* what ghostfs reports for every file */

struct ufile {
    s32 fd;                /* the placeholder handed to the title; -1: free */
    struct fs_node node;
    u64 pos;               /* files: the byte position; directories: the readdir cookie */
    struct fs_walk walk;
};

struct udir {
    u32 hash, len;
    u64 used;
    struct fs_node node;
    char path[232];
};

static struct {
    struct blk *blk;
    struct vol vol;
    struct part part;
    int part_index;
    char root[512];        /* the title's app0 on the volume ("/ghostldr/CUSA12345/app0"); len 0: none */
    u32 root_len;
    bool mounted;
    struct ufile *files;
    struct udir *dirs;
    u64 dir_clock;
    struct fs_dirent de;   /* readdir's scratch, under lock */
    volatile int lock, flock;
    u64 opens, misses, reads, read_bytes, stats, dents, dir_hits;
} U;

static void lock(volatile int *l)
{
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        us_sleep_us(100);
}
static void unlock(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

/* ---- paths ---- */

/* the volume path of a title path ("/app0/a//b/./c" -> "<root>/a/b/c", "" for
 * the volume's root): 0, 1 when it is not usbfs's, -1 too long */
static int vpath(const char *path, char *out, u32 size)
{
    if (!U.mounted || !path)
        return 1;
    bool app = strncmp(path, "/app0", 5) == 0, usb = strncmp(path, "/usb0", 5) == 0;
    if ((!app && !usb) || (path[5] != '/' && path[5]) || (app && !U.root_len))
        return 1;
    u32 n = 0;
    if (app) {
        memcpy(out, U.root, U.root_len);
        n = U.root_len;
    }
    u32 base = n;
    for (const char *p = path + 5; *p;) {
        while (*p == '/')
            p++;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        u32 len = (u32)(p - s);
        if (!len || (len == 1 && s[0] == '.'))
            continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (n > base && out[n - 1] != '/')
                n--;
            if (n > base)
                n--;
            continue;
        }
        if (n + 1 + len + 1 > size)
            return -1;
        out[n++] = '/';
        memcpy(out + n, s, len);
        n += len;
    }
    out[n] = 0;
    return 0;
}

static u32 hash(const char *s, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++)
        h = (h ^ (u8)s[i]) * 16777619u;
    return h;
}

static struct udir *dir_find(const char *vp, u32 len)
{
    u32 h = hash(vp, len);
    for (u32 i = 0; i < UFS_DIRS; i++) {
        struct udir *d = &U.dirs[i];
        if (d->len == len && d->hash == h && memcmp(d->path, vp, len) == 0) {
            d->used = ++U.dir_clock;
            return d;
        }
    }
    return NULL;
}

static void dir_put(const char *vp, u32 len, const struct fs_node *n)
{
    if (!len || len >= sizeof(U.dirs[0].path))
        return;
    struct udir *v = &U.dirs[0];
    for (u32 i = 1; i < UFS_DIRS; i++)
        if (U.dirs[i].used < v->used)
            v = &U.dirs[i];
    v->hash = hash(vp, len), v->len = len, v->used = ++U.dir_clock, v->node = *n;
    memcpy(v->path, vp, len);
}

/* the node at volume path vp; under U.lock */
static int resolve(const char *vp, struct fs_node *out)
{
    u32 len = (u32)strlen(vp), at = 0;
    struct fs_node cur = U.vol.root;
    for (u32 cut = len; cut;) { /* the nearest remembered directory on the way */
        struct udir *d = dir_find(vp, cut);
        if (d) {
            cur = d->node, at = cut;
            U.dir_hits++;
            break;
        }
        while (cut && vp[cut - 1] != '/')
            cut--;
        if (cut)
            cut--;
    }
    while (at < len) {
        const char *s = vp + at + 1, *e = s;
        while (*e && *e != '/')
            e++;
        if (!cur.dir)
            return US_ENOTDIR;
        struct fs_node next;
        int rc = U.vol.ops->lookup(&U.vol, &cur, s, (u32)(e - s), &next);
        if (rc)
            return rc;
        cur = next;
        at = (u32)(e - vp);
        if (cur.dir)
            dir_put(vp, at, &cur);
    }
    *out = cur;
    return 0;
}

/* ---- stat ---- */

static s64 known(s64 t) { return t > 315532800 ? t : NO_TIME; } /* after 1980-01-01 */

static void put_time(u8 *at, s64 sec, u32 ns)
{
    *(s64 *)at = sec;
    *(s64 *)(at + 8) = ns;
}

static void fill_stat(void *st, const struct fs_node *n)
{
    u8 *s = st;
    u64 cs = 1ull << U.vol.cluster_shift;
    u64 size = n->dir && n->size == ~0ull ? cs : n->size;
    memset(s, 0, 0x78);
    *(u32 *)(s + 4) = n->ino;
    *(u16 *)(s + 8) = n->dir ? 0x416d : 0x816d; /* dr-xr-xr-x / -r-xr-xr-x, as ghostfs */
    *(u16 *)(s + 10) = 1;
    s64 m = known(n->mtime), b = known(n->btime ? n->btime : n->mtime);
    u32 mns = m == n->mtime ? n->mtime_ns : 0;
    put_time(s + 0x18, n->atime ? known(n->atime) : m, 0); /* atime */
    put_time(s + 0x28, m, mns);                            /* mtime */
    put_time(s + 0x38, m, mns);                            /* ctime: FAT keeps none */
    put_time(s + 0x68, b, b == n->btime ? n->btime_ns : 0); /* birthtime */
    *(s64 *)(s + 0x48) = (s64)size;
    *(s64 *)(s + 0x50) = (s64)(ALIGN_UP(size, cs) / 512);
    *(u32 *)(s + 0x58) = 0x10000;
}

/* ---- files ---- */

static struct ufile *file_of(int fd)
{
    if (!U.files || fd < 0)
        return NULL;
    for (u32 i = 0; i < UFS_FILES; i++)
        if (U.files[i].fd == fd)
            return &U.files[i];
    return NULL;
}

static bool u_owns(int fd) { return file_of(fd) != NULL; }

static int u_open(const char *path, int flags, int *fd_out)
{
    char vp[VPATH_MAX];
    int r = vpath(path, vp, sizeof(vp));
    if (r == 1)
        return 1;
    if (r < 0) {
        *fd_out = US_ENAMETOOLONG;
        return 0;
    }
    if (flags & 3) { /* O_WRONLY, O_RDWR */
        *fd_out = US_EROFS;
        return 0;
    }
    struct fs_node n;
    lock(&U.lock);
    int rc = resolve(vp, &n);
    unlock(&U.lock);
    if (!rc && (flags & O_DIRECTORY_) && !n.dir)
        rc = US_ENOTDIR;
    if (rc) {
        if (U.misses++ < 48)
            us_log("usbfs open miss %s (flags %x) -> %x", path, flags, (u32)rc);
        *fd_out = rc;
        return 0;
    }
    s32 fd = us_placeholder_fd();
    if (fd < 0) {
        *fd_out = fd;
        return 0;
    }
    struct ufile *f = NULL;
    lock(&U.flock);
    for (u32 i = 0; i < UFS_FILES && !f; i++)
        if (U.files[i].fd < 0)
            f = &U.files[i];
    if (f) {
        f->fd = fd, f->node = n, f->pos = 0;
        f->walk.first = 0, f->walk.nrun = 0;
    }
    unlock(&U.flock);
    if (!f) {
        us_placeholder_close(fd);
        *fd_out = US_EMFILE;
        return 0;
    }
    if (U.opens++ < 200)
        us_log("usbfs open %s (%s%lu bytes) -> %x", path, n.dir ? "directory, " : "", n.dir ? 0 : n.size, fd);
    *fd_out = fd;
    return 0;
}

static int u_close(int fd)
{
    struct ufile *f = file_of(fd);
    if (!f)
        return 1;
    lock(&U.flock);
    f->fd = -1;
    unlock(&U.flock);
    us_placeholder_close(fd);
    return 0;
}

static s64 u_pread(int fd, void *buf, u64 n, s64 off)
{
    struct ufile *f = file_of(fd);
    if (!f)
        return US_EBADF;
    if (f->node.dir)
        return US_EISDIR;
    if (off < 0)
        return US_EINVAL;
    if ((u64)off >= f->node.size || !n)
        return 0;
    n = US_MIN(n, f->node.size - (u64)off);
    lock(&U.lock);
    s64 r = fs_read(&U.vol, &f->node, &f->walk, (u64)off, buf, n, BLK_DATA);
    U.reads++;
    if (r > 0)
        U.read_bytes += (u64)r;
    unlock(&U.lock);
    return r;
}

static s64 u_read(int fd, void *buf, u64 n)
{
    struct ufile *f = file_of(fd);
    if (!f)
        return US_EBADF;
    s64 r = u_pread(fd, buf, n, (s64)f->pos);
    if (r > 0)
        f->pos += (u64)r;
    return r;
}

static s64 u_lseek(int fd, s64 off, int whence)
{
    struct ufile *f = file_of(fd);
    if (!f)
        return US_EBADF;
    u64 size = f->node.dir ? 0 : f->node.size;
    s64 base = whence == 0 ? 0 : whence == 1 ? (s64)f->pos : whence == 2 ? (s64)size : -1;
    if (base < 0 || base + off < 0)
        return US_EINVAL;
    f->pos = (u64)(base + off);
    return (s64)f->pos;
}

static int u_fstat(int fd, void *st)
{
    struct ufile *f = file_of(fd);
    if (!f)
        return US_EBADF;
    fill_stat(st, &f->node);
    return 0;
}

static int u_stat(const char *path, void *st, int *rc)
{
    char vp[VPATH_MAX];
    int r = vpath(path, vp, sizeof(vp));
    if (r == 1)
        return 1;
    if (r < 0) {
        *rc = US_ENAMETOOLONG;
        return 0;
    }
    struct fs_node n;
    lock(&U.lock);
    *rc = resolve(vp, &n);
    U.stats++;
    unlock(&U.lock);
    if (!*rc)
        fill_stat(st, &n);
    return 0;
}

/* macOS's AppleDouble companions ("._name") never belong to a title */
static bool hidden(const char *name, u32 len) { return len >= 2 && name[0] == '.' && name[1] == '_'; }

static int u_getdents(int fd, void *buf, int size)
{
    struct ufile *f = file_of(fd);
    if (!f)
        return US_EBADF;
    if (!f->node.dir)
        return US_EINVAL;
    int used = 0;
    lock(&U.lock);
    for (;;) {
        u64 before = f->pos;
        int r = U.vol.ops->readdir(&U.vol, &f->node, &f->walk, &f->pos, &U.de);
        if (r <= 0) {
            if (r < 0 && !used)
                used = r;
            break;
        }
        u32 len = U.de.len;
        if (hidden(U.de.name, len) || len > 255)
            continue; /* FreeBSD names are at most 255 bytes */
        int reclen = (int)((8 + len + 1 + 3) & ~3u);
        if (used + reclen > size) {
            f->pos = before;
            if (!used)
                used = US_EINVAL; /* not even one entry fits */
            break;
        }
        u8 *d = (u8 *)buf + used;
        *(u32 *)d = U.de.node.ino;
        *(u16 *)(d + 4) = (u16)reclen;
        d[6] = U.de.node.dir ? 4 : 8; /* DT_DIR / DT_REG */
        d[7] = (u8)len;
        memcpy(d + 8, U.de.name, len);
        memset(d + 8 + len, 0, (u32)reclen - 8 - len);
        used += reclen;
    }
    U.dents++;
    unlock(&U.lock);
    return used;
}

static int u_set_root(const char *path)
{
    char full[VPATH_MAX], vp[VPATH_MAX];
    fmt(full, sizeof(full), "/usb0%s%s", path[0] == '/' ? "" : "/", path);
    U.root_len = 0;
    if (vpath(full, vp, sizeof(vp)) != 0 || strlen(vp) >= sizeof(U.root))
        return US_ENAMETOOLONG;
    struct fs_node n;
    lock(&U.lock);
    int rc = resolve(vp, &n);
    unlock(&U.lock);
    if (!rc && !n.dir)
        rc = US_ENOTDIR;
    if (rc) {
        us_log("usbfs: %s is not a folder on the drive (%x)", path, (u32)rc);
        return rc;
    }
    strlcpy(U.root, vp, sizeof(U.root));
    U.root_len = (u32)strlen(vp);
    us_log("usbfs: /app0 is %s on the drive", U.root_len ? U.root : "/");
    return 0;
}

static const struct appfs_ops k_ops = {
    sizeof(struct appfs_ops), "usbfs", u_open, u_stat, u_owns, u_close, u_read, u_pread, u_lseek, u_fstat,
    u_getdents, u_set_root,
};

const struct appfs_ops *usbfs_ops(void) { return U.mounted ? &k_ops : NULL; }

int usbfs_set_root(const char *path) { return u_set_root(path); }

int usbfs_node(const char *path, struct fs_node *out)
{
    char vp[VPATH_MAX];
    int r = vpath(path, vp, sizeof(vp));
    if (r)
        return r < 0 ? US_ENAMETOOLONG : US_ENOENT;
    lock(&U.lock);
    r = resolve(vp, out);
    unlock(&U.lock);
    return r;
}

struct vol *usbfs_vol(void) { return U.mounted ? &U.vol : NULL; }

int usbfs_part(struct part *out)
{
    *out = U.part;
    return U.mounted ? U.part_index : -1;
}

int usbfs_mount(struct blk *b, int part_index)
{
    struct part parts[PART_MAX];
    usbfs_unmount();
    int n = part_scan(b, parts, PART_MAX);
    if (n <= 0) {
        us_log("usbfs: no partition table and no volume at the drive's start (%d)", n);
        return n < 0 ? n : US_ENODEV;
    }
    /* the first partition with a file system; an EFI system partition only
     * when nothing else has one */
    int rc = US_ENODEV;
    for (int pass = 0; pass < 2 && rc; pass++)
        for (int i = 0; i < n && rc; i++) {
            if ((part_index >= 0 && i != part_index) || (part_index < 0 && parts[i].esp != pass))
                continue;
            rc = fs_mount(&U.vol, b, &parts[i]);
            if (!rc)
                U.part = parts[i], U.part_index = i;
        }
    if (rc) {
        us_log("usbfs: none of the drive's %d partitions holds exFAT or FAT32 (%x)", n, (u32)rc);
        return rc;
    }
    U.files = us_alloc(UFS_FILES * sizeof(*U.files));
    U.dirs = us_alloc(UFS_DIRS * sizeof(*U.dirs));
    if (!U.files || !U.dirs) {
        usbfs_unmount();
        return US_ENOMEM;
    }
    for (u32 i = 0; i < UFS_FILES; i++)
        U.files[i].fd = -1;
    U.blk = b;
    U.mounted = true;
    us_log("usbfs: %s volume '%s' (serial %08x) on partition %d of %d (%s, sector %lx, %lu MiB): %u clusters of "
           "%u bytes, %s case folding",
           U.vol.ops->name, U.vol.label, U.vol.serial, U.part_index + 1, n,
           U.part.whole ? "no table" : U.part.gpt ? "GPT" : "MBR", U.part.start, U.vol.size >> 20, U.vol.clusters,
           1u << U.vol.cluster_shift, U.vol.upcase ? "the volume's up-case table for" : "built-in");
    return 0;
}

void usbfs_stats(char *out, u32 size)
{
    struct blk *b = U.blk;
    fmt(out, size,
        "usbfs: %lu opens (%lu missed), %lu stats, %lu getdents, %lu reads of %lu KiB; directories %lu remembered "
        "hits; cache %lu hits, %lu misses, %lu device reads of %lu KiB (%lu KiB direct, %lu read-ahead lines); "
        "%u chain errors",
        U.opens, U.misses, U.stats, U.dents, U.reads, U.read_bytes >> 10, U.dir_hits, b ? b->hits : 0,
        b ? b->misses : 0, b ? b->dev_reads : 0, b ? b->dev_bytes >> 10 : 0, b ? b->direct_bytes >> 10 : 0,
        b ? b->ra_lines : 0, U.vol.chain_errors);
}

void usbfs_unmount(void)
{
    if (U.files)
        us_free(U.files, UFS_FILES * sizeof(*U.files));
    if (U.dirs)
        us_free(U.dirs, UFS_DIRS * sizeof(*U.dirs));
    if (U.mounted)
        fs_unmount(&U.vol);
    U.files = NULL, U.dirs = NULL, U.mounted = false, U.root_len = 0, U.blk = NULL;
    U.opens = U.misses = U.reads = U.read_bytes = U.stats = U.dents = U.dir_hits = 0;
}
