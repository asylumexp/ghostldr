/*
 * ghostfs: the title's /app0 served from SWRR's own save containers.
 *
 * Without a kernel exploit SWRR's sandbox sees nothing but its own app0,
 * save data and a ~1 GiB temp folder, but it can own many save containers
 * of up to 1 GiB each and mount 15 of them at a time.  gfs_pack.py lays the
 * title's files end to end over containers GHOST000.. (data.bin each, `cap`
 * bytes) and writes a sorted index (GHOSTIDX/index.bin); gfs_install.py puts
 * them on the console over the network.
 *
 * Here: the index is loaded at boot; opening /app0/... gives the caller a
 * real placeholder fd (so calls not routed here fail harmlessly) that maps to
 * an index entry; reads become preads on the containers' data.bin, which are
 * mounted read-only on demand and unmounted least-recently-used-first.
 *
 * sceSaveDataMount comes from SWRR's import table (eboot+0x3893F0; resolved
 * through dlsym it refuses every mount), params: userId u32 @0, dirName*
 * @0x10, blocks u64 @0x20, mode u32 @0x28 (RO 1, RW 2, CREATE 4); result:
 * mountPoint char[16] @0.  File calls go to libkernel's own exports.
 */
#include "ghost.h"

void *host_real(const char *name);
void *host_real_in(const char *modname, const char *name);

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define GFS_DIR 1
#define GFS_MAX_CONT 96
#define GFS_SLOTS 12          /* of /savedata1..15: leave room for GHOSTSAVE and a spare */
#define GFS_FILES 512
#define SWRR_SAVEDATA_MOUNT 0x3893F0
#define E_NOENT 0x80020002
#define E_BADF 0x80020009
#define E_ISDIR 0x80020015
#define E_INVAL 0x80020016
#define E_ROFS 0x8002001e
#define E_NOMEM 0x8002000c

/* gfs_pack.py: version 2 has a 128-byte header (title, layout id, flags);
 * version 1 (GHOSTIDX until 2026-10-21) stops at prefix, 64 bytes */
struct gfs_header {
    char magic[4];
    u32 version, containers, entries;
    u64 cap, total;
    u32 names_off, names_size;
    char prefix[16];
    u32 header_size, flags; /* version 2 */
    char title[16];
    u64 layout_id;
};
#define GFS_INCOMPLETE 1
struct gfs_entry {
    u64 off, size;
    u32 name, flags;
};

static const struct gfs_header *g_hdr;
static const struct gfs_entry *g_ent;
static const char *g_names;
static u8 *g_opened;          /* bitmap: first open of each entry is logged */
static bool g_active;
static u32 g_user;

static int (*sd_mount)(void *params, void *result);
static int (*sd_umount)(const char *mount_point);
static int (*k_open)(const char *, int, int);
static int (*k_close)(int);
static s64 (*k_pread)(int, void *, u64, s64);
static int (*k_stat)(const char *, void *);

bool gfs_active(void) { return g_active; }

/* the first few of each kind of unusual request are logged (diagnosis) */
static u32 g_diag[4];
#define DIAG(kind, ...)                                                             \
    do {                                                                            \
        if (__atomic_fetch_add(&g_diag[kind], 1, __ATOMIC_RELAXED) < 48)            \
            logf(__VA_ARGS__);                                                      \
    } while (0)
enum { DIAG_MISS, DIAG_STAT, DIAG_DENTS, DIAG_READ };

/* a lock that sleeps: holders may spend 100+ ms in a mount */
static void lock(volatile int *l)
{
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        K.usleep(200);
}
static void unlock(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

/* ---- save containers ---- */

int gfs_mount(const char *dirname, int mode, u64 blocks, char mount_point[16])
{
    u8 params[128], result[64];
    char dir[32];
    memset(params, 0, sizeof(params));
    memset(result, 0, sizeof(result));
    memset(dir, 0, sizeof(dir));
    strlcpy(dir, dirname, sizeof(dir));
    *(u32 *)(params + 0x00) = g_user;
    *(u64 *)(params + 0x10) = (u64)dir;
    *(u64 *)(params + 0x20) = blocks;
    *(u32 *)(params + 0x28) = (u32)mode;
    int rc = sd_mount(params, result);
    if (rc == 0) {
        memcpy(mount_point, result, 16);
        mount_point[15] = 0;
    }
    return rc;
}

/* the unmount is the commit (and asynchronous): wait until the point is gone */
int gfs_umount(const char mount_point[16], u32 max_ms)
{
    char mp[16];
    u8 st[0x78];
    memcpy(mp, mount_point, 16);
    int rc = sd_umount(mp);
    for (u32 waited = 0; waited < max_ms && k_stat(mp, st) == 0; waited += 10)
        K.usleep(10000);
    return rc;
}

static bool sd_ready(void)
{
    if (sd_mount)
        return true;
    u64 p = *(u64 *)(g_ctx->eboot_base + SWRR_SAVEDATA_MOUNT);
    sd_umount = host_real_in("libSceSaveData", "sceSaveDataUmount");
    int (*get_user)(u32 *) = host_real_in("libSceUserService", "sceUserServiceGetInitialUser");
    k_open = host_real("sceKernelOpen");
    k_close = host_real("sceKernelClose");
    k_pread = host_real("sceKernelPread");
    k_stat = host_real("sceKernelStat");
    if (p < 0x10000 || !sd_umount || !get_user || get_user(&g_user) != 0) {
        logf("ghostfs: save data unavailable (mount %lx, umount %p, user %x)", p, sd_umount, g_user);
        return false;
    }
    sd_mount = (int (*)(void *, void *))p;
    return true;
}

struct cont {
    s32 fd;
    u32 busy;
    u64 last;
    char mp[16];
};
static struct cont *g_cont;          /* page_alloc'd: keeps the core image small */
static volatile int g_cont_lock;
static u32 g_mounted;
static u64 g_tick, g_mounts, g_evictions;

/* fd of container k's data.bin, mounting it (and evicting another) if needed */
static int cont_acquire(u32 k)
{
    if (k >= g_hdr->containers || k >= GFS_MAX_CONT)
        return (int)E_INVAL;
    for (;;) {
        lock(&g_cont_lock);
        struct cont *c = &g_cont[k];
        if (c->fd >= 0) {
            c->busy++;
            c->last = ++g_tick;
            unlock(&g_cont_lock);
            return c->fd;
        }
        if (g_mounted >= GFS_SLOTS) {
            struct cont *victim = NULL;
            for (u32 i = 0; i < g_hdr->containers; i++)
                if (g_cont[i].fd >= 0 && !g_cont[i].busy && (!victim || g_cont[i].last < victim->last))
                    victim = &g_cont[i];
            if (!victim) {                   /* every slot is mid-read: wait */
                unlock(&g_cont_lock);
                K.usleep(1000);
                continue;
            }
            k_close(victim->fd);
            victim->fd = -1;
            gfs_umount(victim->mp, 2000);
            g_mounted--;
            g_evictions++;
        }
        char dir[32], path[48];
        fmt(dir, sizeof(dir), "%s%03u", g_hdr->prefix, k);
        int rc = gfs_mount(dir, 1, 0, c->mp);
        if (rc != 0) {
            unlock(&g_cont_lock);
            logf("ghostfs: mount %s -> %x", dir, rc);
            return rc;
        }
        fmt(path, sizeof(path), "%s/data.bin", c->mp);
        int fd = k_open(path, 0, 0);
        if (fd < 0) {
            gfs_umount(c->mp, 2000);
            unlock(&g_cont_lock);
            logf("ghostfs: open %s -> %x", path, fd);
            return fd;
        }
        c->fd = fd, c->busy = 1, c->last = ++g_tick;
        g_mounted++, g_mounts++;
        if (g_mounts <= 32 || (g_mounts & 63) == 0)
            logf("ghostfs: mounted %s at %s (%u mounted, %lu mounts, %lu evictions)", dir, c->mp, g_mounted, g_mounts,
                 g_evictions);
        unlock(&g_cont_lock);
        return fd;
    }
}

static void cont_release(u32 k)
{
    lock(&g_cont_lock);
    g_cont[k].busy--;
    unlock(&g_cont_lock);
}

/* read n bytes at offset `off` of the linear space */
static s64 read_linear(u64 off, void *buf, u64 n)
{
    u64 done = 0, cap = g_hdr->cap;
    while (done < n) {
        u32 k = (u32)((off + done) / cap);
        u64 local = (off + done) % cap, take = MIN(n - done, cap - local);
        int fd = cont_acquire(k);
        if (fd < 0)
            return done ? (s64)done : fd;
        s64 r = k_pread(fd, (u8 *)buf + done, take, (s64)local);
        cont_release(k);
        if (r <= 0) {
            DIAG(DIAG_READ, "gfs read %s%03u @ %lx (%lu bytes) -> %lx", g_hdr->prefix, k, local, take, (u64)r);
            return done ? (s64)done : r;
        }
        done += (u64)r;
    }
    return (s64)done;
}

struct gfile {
    s32 fd;      /* real placeholder fd handed to the title, -1 = free */
    u32 entry;
    u64 pos;
};
static struct gfile *g_files;        /* page_alloc'd */
static volatile int g_files_lock;

/* ---- index ---- */

static const char *ename(u32 i) { return g_names + g_ent[i].name; }

static s64 find(const char *rel)
{
    s64 lo = 0, hi = (s64)g_hdr->entries - 1;
    while (lo <= hi) {
        s64 mid = (lo + hi) / 2;
        int c = strcmp(ename((u32)mid), rel);
        if (c == 0)
            return mid;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

/* "/app0", "/app0/", "/app0/a//b/" -> "", "", "a/b" */
static bool app0_rel(const char *path, char *out, size_t size)
{
    if (!g_active || !path || strncmp(path, "/app0", 5) != 0 || (path[5] != '/' && path[5] != 0))
        return false;
    const char *p = path + 5;
    size_t n = 0;
    while (*p && n + 1 < size) {
        if (*p == '/' && (n == 0 || out[n - 1] == '/')) {
            p++;
            continue;
        }
        out[n++] = *p++;
    }
    while (n && out[n - 1] == '/')
        n--;
    out[n] = 0;
    return true;
}

int gfs_init(void)
{
    if (!sd_ready())
        return -1;
    char mp[16], path[48];
    int rc = gfs_mount("GFSINDEX", 1, 0, mp);
    if (rc != 0)
        rc = gfs_mount("GHOSTIDX", 1, 0, mp);       /* installs before 2026-10-21 */
    if (rc != 0) {
        logf("ghostfs: no GFSINDEX or GHOSTIDX (%x): nothing installed", rc);
        return rc;
    }
    fmt(path, sizeof(path), "%s/index.bin", mp);
    u8 st[0x78];
    int fd = k_open(path, 0, 0);
    if (fd < 0 || k_stat(path, st) != 0) {
        logf("ghostfs: %s -> %x", path, fd);
        gfs_umount(mp, 2000);
        return -1;
    }
    u64 size = *(u64 *)(st + 0x48);
    u8 *idx = page_alloc(size);
    s64 got = idx ? k_pread(fd, idx, size, 0) : -1;
    k_close(fd);
    gfs_umount(mp, 2000);
    g_hdr = (const struct gfs_header *)idx;
    u32 hsize = got >= 64 && g_hdr->version >= 2 ? g_hdr->header_size : 64;
    if (got != (s64)size || size < 64 || memcmp(g_hdr->magic, "GFS1", 4) != 0 || g_hdr->containers > GFS_MAX_CONT ||
        hsize < 64 || hsize > size) {
        logf("ghostfs: bad index (%ld of %lu bytes)", got, size);
        return -1;
    }
    if (g_hdr->version >= 2 && (g_hdr->flags & GFS_INCOMPLETE)) {
        logf("ghostfs: the install of %.16s is incomplete: rerun gfs_install.py", g_hdr->title);
        return -1;
    }
    g_ent = (const struct gfs_entry *)(idx + hsize);
    g_names = (const char *)idx + g_hdr->names_off;
    g_opened = page_alloc((g_hdr->entries + 7) / 8);
    g_cont = page_alloc(GFS_MAX_CONT * sizeof(*g_cont));
    g_files = page_alloc(GFS_FILES * sizeof(*g_files));
    if (!g_opened || !g_cont || !g_files)
        return -1;
    for (u32 i = 0; i < GFS_MAX_CONT; i++)
        g_cont[i].fd = -1;
    for (u32 i = 0; i < GFS_FILES; i++)
        g_files[i].fd = -1;
    g_active = true;
    logf("ghostfs: %.16s, %u entries, %lu MiB in %u containers of %lu MiB (%.16s000..), user %x",
         g_hdr->version >= 2 ? g_hdr->title : "index v1", g_hdr->entries, g_hdr->total >> 20, g_hdr->containers,
         g_hdr->cap >> 20, g_hdr->prefix, g_user);
    return 0;
}

/* the title's progress container: <prefix>SAVE (GHOSTSAVE for CUSA11456) */
const char *gfs_save_name(void)
{
    static char name[32];
    if (!name[0] && g_hdr)
        fmt(name, sizeof(name), "%.16sSAVE", g_hdr->prefix);
    return name;
}

/* ---- open files ---- */


static struct gfile *gf_of(int fd)
{
    if (!g_active || fd < 0)
        return NULL;
    for (u32 i = 0; i < GFS_FILES; i++)
        if (g_files[i].fd == fd)
            return &g_files[i];
    return NULL;
}

bool gfs_owns(int fd) { return gf_of(fd) != NULL; }

static void fill_stat(void *st, u32 i)
{
    u8 *s = st;
    memset(s, 0, 0x78);
    const struct gfs_entry *e = &g_ent[i];
    *(u32 *)(s + 4) = i + 1;                                       /* st_ino */
    *(u16 *)(s + 8) = (e->flags & GFS_DIR) ? 0x416d : 0x816d;     /* dr-xr-xr-x / -r-xr-xr-x */
    *(u16 *)(s + 10) = 1;
    *(s64 *)(s + 0x48) = (s64)e->size;
    *(s64 *)(s + 0x50) = (s64)((e->size + 511) / 512);
    *(u32 *)(s + 0x58) = 0x10000;
    /* atime, mtime, ctime, birthtime: what the console reports for app0
     * (a zero mtime made the title's file layer drop requests) */
    for (u32 off = 0x18; off <= 0x38; off += 0x10)
        *(s64 *)(s + off) = 1477944229;
    *(s64 *)(s + 0x68) = 1477944229;
}

/* path is the title's ("/app0/..."): returns 1 when it is not ghostfs's */
int gfs_open(const char *path, int flags, int *fd_out)
{
    char rel[512];
    if (!app0_rel(path, rel, sizeof(rel)))
        return 1;
    if (flags & 3) {                                   /* O_WRONLY / O_RDWR */
        *fd_out = (int)E_ROFS;
        return 0;
    }
    s64 i = find(rel);
    if (i < 0) {
        DIAG(DIAG_MISS, "gfs open miss %s (flags %x)", path, flags);
        *fd_out = (int)E_NOENT;
        return 0;
    }
    /* SWRR's own eboot.bin: a harmless real fd (its sandbox refuses
     * /app0/sce_sys).  The path is copied to the stack first, as kernel paths
     * are everywhere else; literals live in the core's JIT (RX) image */
    char placeholder[32];
    strlcpy(placeholder, "/app0/eboot.bin", sizeof(placeholder));
    int fd = k_open(placeholder, 0, 0);
    if (fd < 0) {
        *fd_out = fd;
        return 0;
    }
    lock(&g_files_lock);
    struct gfile *f = NULL;
    for (u32 j = 0; j < GFS_FILES && !f; j++)
        if (g_files[j].fd < 0)
            f = &g_files[j];
    if (f)
        f->fd = fd, f->entry = (u32)i, f->pos = 0;
    unlock(&g_files_lock);
    if (!f) {
        k_close(fd);
        *fd_out = (int)E_NOMEM;
        return 0;
    }
    if (!(g_opened[i >> 3] & (1 << (i & 7)))) {
        g_opened[i >> 3] |= (u8)(1 << (i & 7));
        logf("gfs open /app0/%s (%lu bytes @ %lx) -> %x", rel, g_ent[i].size, g_ent[i].off, fd);
    }
    *fd_out = fd;
    return 0;
}

int gfs_close(int fd)
{
    struct gfile *f = gf_of(fd);
    if (!f)
        return 1;
    lock(&g_files_lock);
    f->fd = -1;
    unlock(&g_files_lock);
    k_close(fd);
    return 0;
}

s64 gfs_pread(int fd, void *buf, u64 n, s64 off)
{
    struct gfile *f = gf_of(fd);
    const struct gfs_entry *e = &g_ent[f->entry];
    if (e->flags & GFS_DIR)
        return (s64)E_ISDIR;
    if (off < 0)
        return (s64)E_INVAL;
    if ((u64)off >= e->size || !n)
        return 0;
    n = MIN(n, e->size - (u64)off);
    return read_linear(e->off + (u64)off, buf, n);
}

s64 gfs_read(int fd, void *buf, u64 n)
{
    struct gfile *f = gf_of(fd);
    s64 r = gfs_pread(fd, buf, n, (s64)f->pos);
    if (r > 0)
        f->pos += (u64)r;
    return r;
}

s64 gfs_lseek(int fd, s64 off, int whence)
{
    struct gfile *f = gf_of(fd);
    s64 base = whence == 0 ? 0 : whence == 1 ? (s64)f->pos : whence == 2 ? (s64)g_ent[f->entry].size : -1;
    if (base < 0 || base + off < 0)
        return (s64)E_INVAL;
    f->pos = (u64)(base + off);
    return (s64)f->pos;
}

int gfs_fstat(int fd, void *st)
{
    DIAG(DIAG_STAT, "gfs fstat /app0/%s", ename(gf_of(fd)->entry));
    fill_stat(st, gf_of(fd)->entry);
    return 0;
}

/* returns 1 when the path is not ghostfs's */
int gfs_stat(const char *path, void *st, int *rc)
{
    char rel[512];
    if (!app0_rel(path, rel, sizeof(rel)))
        return 1;
    s64 i = find(rel);
    if (i < 0) {
        DIAG(DIAG_MISS, "gfs stat miss %s", path);
        *rc = (int)E_NOENT;
        return 0;
    }
    DIAG(DIAG_STAT, "gfs stat %s -> %s %lu", path, (g_ent[i].flags & GFS_DIR) ? "dir" : "file", g_ent[i].size);
    fill_stat(st, (u32)i);
    *rc = 0;
    return 0;
}

/* FreeBSD 11 dirents of the directory's immediate children; f->pos is the
 * next entry index to look at */
int gfs_getdents(int fd, void *buf, int size)
{
    struct gfile *f = gf_of(fd);
    if (!(g_ent[f->entry].flags & GFS_DIR))
        return (int)E_INVAL;
    /* children of "a/b" are the entries starting "a/b/" (sorted, so one
     * contiguous range); of the root, every entry without a '/' */
    char key[520];
    const char *dir = ename(f->entry);
    size_t dlen = strlen(dir);
    if (dlen)
        fmt(key, sizeof(key), "%s/", dir), dlen++;
    else
        key[0] = 0;
    if (!f->pos) {
        s64 lo = 0, hi = (s64)g_hdr->entries;
        while (lo < hi) {               /* first entry >= key */
            s64 mid = (lo + hi) / 2;
            if (strcmp(ename((u32)mid), key) < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        f->pos = (u64)lo;
    }
    int used = 0;
    for (; f->pos < g_hdr->entries; f->pos++) {
        const char *n = ename((u32)f->pos);
        if (dlen && strncmp(n, key, dlen) != 0)
            break;                      /* past dir/'s range */
        const char *child = n + dlen;
        if (!*child || strrchr_c(child, '/'))
            continue;                   /* dir itself, or a grandchild */
        size_t len = strlen(child);
        int reclen = (int)((8 + len + 1 + 3) & ~3UL);
        if (used + reclen > size)
            break;
        u8 *d = (u8 *)buf + used;
        *(u32 *)d = (u32)f->pos + 1;
        *(u16 *)(d + 4) = (u16)reclen;
        d[6] = (g_ent[f->pos].flags & GFS_DIR) ? 4 : 8;   /* DT_DIR / DT_REG */
        d[7] = (u8)len;
        memcpy(d + 8, child, len + 1);
        used += reclen;
    }
    DIAG(DIAG_DENTS, "gfs getdents /app0/%s (%d bytes buffer) -> %d bytes", dir, size, used);
    return used;
}

/* ---- the loader's file access: ghostfs for /app0 paths, the kernel otherwise ---- */

int vfs_open(const char *path, int flags, int mode)
{
    int fd;
    if (gfs_open(path, flags, &fd) == 0)
        return fd;
    return K.open(path, flags, mode);
}
s64 vfs_read(int fd, void *buf, u64 n) { return gfs_owns(fd) ? gfs_read(fd, buf, n) : K.read(fd, buf, n); }
s64 vfs_lseek(int fd, s64 off, int whence) { return gfs_owns(fd) ? gfs_lseek(fd, off, whence) : K.lseek(fd, off, whence); }
int vfs_close(int fd) { return gfs_close(fd) == 0 ? 0 : K.close(fd); }
int vfs_stat(const char *path, void *st)
{
    int rc;
    if (gfs_stat(path, st, &rc) == 0)
        return rc;
    return K.stat(path, st);
}
