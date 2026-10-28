/*
 * ghostfs: the title's /app0 served from SWRR's own save containers.
 *
 * Without a kernel exploit SWRR's sandbox sees nothing but its own app0,
 * save data and a ~1 GiB temp folder, but it can own many save containers
 * of up to 1 GiB each and mount 15 of them at a time.  gfs_pack.py lays the
 * title's files end to end over containers <prefix>000.. (data.bin each,
 * `cap` bytes) and writes a sorted index (index.bin); gfs_install.py puts
 * them on the console over the network.
 *
 * Several titles can be installed side by side, each with its own prefix
 * and index container <prefix>INDEX, listed in GFSCATALOG
 * (docs/ghostfs-catalogue.md); only the booted title's containers are
 * mounted.
 *
 * Here: the index is loaded at boot; opening /app0/... gives the caller a
 * real placeholder fd (so calls not routed here fail harmlessly) that maps to
 * an index entry; reads become preads on the containers' data.bin, which are
 * mounted read-only on demand and unmounted least-recently-used-first.
 *
 * sceSaveDataMount comes from SWRR's import table (eboot+0x3893F0; resolved
 * through dlsym it refuses every mount), params: userId u32 @0, dirName*
 * @0x10, blocks u64 @0x20, mode u32 @0x28 (RO 1, RW 2, CREATE 4); result:
 * mountPoint char[16] @0.  Jak X imports sceSaveDataMount2 instead (host.c):
 * userId @0, dirName* @0x8, blocks @0x10, mode @0x18, the same result.  File
 * calls go to libkernel's own exports.
 */
#include "ghost_api.h"

void *host_real(const char *name);
void *host_real_in(const char *modname, const char *name);

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define GFS_DIR 1
#define GFS_MAX_CONT 96
#define GFS_SLOTS 12          /* of /savedata1..15: leave room for <prefix>SAVE and a spare */
#define GFS_FILES 512
/* signed, so they stay negative in the s64 results */
#define E_NOENT ((s32)0x80020002)
#define E_BADF ((s32)0x80020009)
#define E_ISDIR ((s32)0x80020015)
#define E_INVAL ((s32)0x80020016)
#define E_ROFS ((s32)0x8002001e)
#define E_NOMEM ((s32)0x8002000c)
#define COLD GHOST_COLD /* boot-time code: small beats fast */

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
/* A title from a USB drive (approot.c): another provider may serve /app0
 * (route B: the extension's usbfs), and its progress goes to <prefix>SAVE
 * as a ghostfs title's does */
static const struct appfs_ops *g_fs;
static bool g_saves;
static char g_save_title[16], g_save_prefix[16];

static int (*sd_mount)(void *params, void *result);
static int (*sd_umount)(const char *mount_point);
static int (*k_open)(const char *, int, int);
static int (*k_close)(int);
static s64 (*k_pread)(int, void *, u64, s64);
static int (*k_stat)(const char *, void *);

/* the title runs from SWRR's save data or a USB drive: no host directory, saves in containers */
bool gfs_active(void) { return g_active || g_fs || g_saves; }
bool gfs_app0_virtual(void) { return g_active || g_fs; }

int appfs_register(const struct appfs_ops *ops)
{
    if (!ops || ops->size < sizeof(*ops) || g_active)
        return -1;
    g_fs = ops;
    logf("ghostfs: %s serves the title's files", ops->name);
    return 0;
}

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

/* user < 0: the initial user.  required_blocks (may be NULL) gets what a
 * NO_SPACE_FS (0x809F000A) result says the create needs. */
int gfs_mount_user(s32 user, const char *dirname, int mode, u64 blocks, char mount_point[16], u64 *required_blocks)
{
    u8 params[128], result[64];
    char dir[32];
    if (!sd_mount)
        return (int)0x809F000B; /* SCE_SAVE_DATA_ERROR_INTERNAL: no save service (not ghostfs) */
    memset(params, 0, sizeof(params));
    memset(result, 0, sizeof(result));
    memset(dir, 0, sizeof(dir));
    strlcpy(dir, dirname, sizeof(dir));
    bool m2 = g_hostgame->save_mount2;
    *(u32 *)(params + 0x00) = user < 0 ? g_user : (u32)user;
    *(u64 *)(params + (m2 ? 0x08 : 0x10)) = (u64)dir;
    *(u64 *)(params + (m2 ? 0x10 : 0x20)) = blocks;
    *(u32 *)(params + (m2 ? 0x18 : 0x28)) = (u32)mode;
    int rc = sd_mount(params, result);
    if (rc == 0) {
        memcpy(mount_point, result, 16);
        mount_point[15] = 0;
    }
    if (required_blocks)
        *required_blocks = *(u64 *)(result + 0x10);
    return rc;
}

int gfs_mount(const char *dirname, int mode, u64 blocks, char mount_point[16])
{
    return gfs_mount_user(-1, dirname, mode, blocks, mount_point, NULL);
}

/* the unmount is the commit (and asynchronous): wait until the point is gone.
 * A refused unmount (BUSY: files still open) returns at once. */
int gfs_umount(const char mount_point[16], u32 max_ms)
{
    char mp[16];
    u8 st[0x78];
    memcpy(mp, mount_point, 16);
    int rc = sd_umount(mp);
    for (u32 waited = 0; rc == 0 && waited < max_ms && k_stat(mp, st) == 0; waited += 10)
        K.usleep(10000);
    return rc;
}

static bool sd_ready(void)
{
    if (sd_mount)
        return true;
    u64 p = *(u64 *)(g_ctx->eboot_base + g_hostgame->save_mount);
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

COLD void gfs_use_saves(const char *title, const char *prefix)
{
    g_saves = sd_ready();
    strlcpy(g_save_title, title, sizeof(g_save_title));
    strlcpy(g_save_prefix, prefix, 13);
    logf("ghostfs: %s keeps its progress in %sSAVE%s", title, prefix, g_saves ? "" : " (no save data service)");
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

/* Real slots other HLE code holds for long (the title's mounted saves).  The
 * first GFS_SPARE fit beside the data containers and <prefix>SAVE; each one
 * after that lowers the data containers' cap, down to GFS_MIN_DATA. */
#define GFS_SPARE 2
#define GFS_MIN_DATA 8
static u32 g_reserved;

static u32 data_cap(u32 reserved)
{
    return GFS_SLOTS - (reserved > GFS_SPARE ? reserved - GFS_SPARE : 0);
}

/* the least recently used data container no read is using, or NULL */
static struct cont *lru_idle(void)
{
    struct cont *victim = NULL;
    for (u32 i = 0; i < g_hdr->containers; i++)
        if (g_cont[i].fd >= 0 && !g_cont[i].busy && (!victim || g_cont[i].last < victim->last))
            victim = &g_cont[i];
    return victim;
}

static void evict(struct cont *victim)
{
    k_close(victim->fd);
    victim->fd = -1;
    gfs_umount(victim->mp, 2000);
    g_mounted--;
    g_evictions++;
}

/* The data containers mounted for reads go: the game selector beside the
 * ELF loader (boot.c elfldr_select) read through them, and the title boots
 * in a second call that mounts them again */
void gfs_unmount_all(void)
{
    if (!g_hdr || !g_cont)
        return;
    lock(&g_cont_lock);
    for (u32 i = 0; i < g_hdr->containers && i < GFS_MAX_CONT; i++)
        if (g_cont[i].fd >= 0 && !g_cont[i].busy)
            evict(&g_cont[i]);
    unlock(&g_cont_lock);
}

/* 0: one more real slot is the caller's until gfs_slot_release (idle data
 * containers are unmounted to make room); -1: no slot can be spared */
int gfs_slot_reserve(void)
{
    if (!g_active)
        return 0;
    for (;;) {
        lock(&g_cont_lock);
        if (data_cap(g_reserved + 1) < GFS_MIN_DATA) {
            unlock(&g_cont_lock);
            return -1;
        }
        struct cont *victim = g_mounted > data_cap(g_reserved + 1) ? lru_idle() : NULL;
        if (g_mounted <= data_cap(g_reserved + 1)) {
            g_reserved++;
            unlock(&g_cont_lock);
            logf("ghostfs: real slot reserved (%u reserved, %u data containers mounted)", g_reserved, g_mounted);
            return 0;
        }
        if (victim)
            evict(victim);
        unlock(&g_cont_lock);
        if (!victim)
            K.usleep(1000); /* every data container is mid-read */
    }
}

void gfs_slot_release(void)
{
    if (!g_active)
        return;
    lock(&g_cont_lock);
    if (g_reserved)
        g_reserved--;
    unlock(&g_cont_lock);
}

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
        if (g_mounted >= data_cap(g_reserved)) {
            struct cont *victim = lru_idle();
            if (!victim) {                   /* every slot is mid-read: wait */
                unlock(&g_cont_lock);
                K.usleep(1000);
                continue;
            }
            evict(victim);
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

/* A file named `name` anywhere in the title but sce_module/ and sce_sys/
 * (Unity keeps its runtime in Media/Modules): its directory as "/app0/dir".
 * The first in index (path) order. */
int gfs_find_file(const char *name, char *dir, size_t size)
{
    size_t len = strlen(name);
    if (!g_active)
        return 1; /* no index to look in */
    for (u32 i = 0; i < g_hdr->entries; i++) {
        const char *e = ename(i);
        size_t el = strlen(e);
        if ((g_ent[i].flags & 1) || el <= len || e[el - len - 1] != '/' || strcmp(e + el - len, name) != 0 ||
            strncmp(e, "sce_module/", 11) == 0 || strncmp(e, "sce_sys/", 8) == 0)
            continue;
        fmt(dir, size, "/app0/%.*s", (int)(el - len - 1), e);
        return 0;
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

/* load an index container's index.bin and serve its title as /app0 */
COLD static int load_index(const char *name, const char *want_title, const char *want_prefix)
{
    char mp[16], path[48];
    int rc = gfs_mount(name, 1, 0, mp);
    if (rc != 0)
        return rc;
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
        hsize < 64 || hsize > size ||
        (g_hdr->version >= 2 && (hsize < sizeof(struct gfs_header) || size < sizeof(struct gfs_header)))) {
        logf("ghostfs: %s: bad index (%ld of %lu bytes)", name, got, size);
        return -1;
    }
    const char *title = g_hdr->version >= 2 ? g_hdr->title : "";
    if ((want_title && g_hdr->version >= 2 && strncmp(title, want_title, 16) != 0) ||
        (want_prefix && strncmp(g_hdr->prefix, want_prefix, 16) != 0)) {
        logf("ghostfs: %s holds %.16s (%.16s000..), not %s", name, title, g_hdr->prefix,
             want_title ? want_title : want_prefix);
        return -1;
    }
    if (g_hdr->version >= 2 && (g_hdr->flags & GFS_INCOMPLETE)) {
        logf("ghostfs: the install of %.16s is incomplete: rerun gfs_install.py", g_hdr->title);
        return -1;
    }
    /* The table and the names come after the header's own checks, so an
     * install's stub index (gfs_install.py's header alone, flagged
     * incomplete) is reported as unfinished above, not as a bad index.
     * The catalogue (gfs_titles, just below) bounds-checks its table; the
     * index did not, and find()/ename() feed entry name offsets straight
     * into strcmp.  Reject a truncated or corrupt index here, before
     * anything reads through it: cap 0 would divide by zero in
     * read_linear, an entry past the table or names blob would read past
     * the index, and an entry past `total` names bytes no container has. */
    if (g_hdr->cap == 0 || (u64)g_hdr->entries * sizeof(struct gfs_entry) > size - hsize ||
        g_hdr->names_off > size || (u64)g_hdr->names_size > size - g_hdr->names_off) {
        logf("ghostfs: %s: bad index: table or names out of range", name);
        return -1;
    }
    {
        const struct gfs_entry *ents = (const struct gfs_entry *)(idx + hsize);
        const char *names = (const char *)idx + g_hdr->names_off;
        for (u32 i = 0; i < g_hdr->entries; i++) {
            u32 noff = ents[i].name;
            bool terminated = false;
            if (noff < g_hdr->names_size) {
                for (u32 k = noff; k < g_hdr->names_size; k++)
                    if (!names[k]) {
                        terminated = true;
                        break;
                    }
            }
            if (!terminated || ents[i].off > g_hdr->total || ents[i].size > g_hdr->total - ents[i].off) {
                logf("ghostfs: %s: bad index: entry %u out of range", name, i);
                return -1;
            }
        }
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
    char t[17];
    strlcpy(t, want_title && !title[0] ? want_title : title, sizeof(t));
    profile_select(t);
    g_active = true;
    logf("ghostfs: %.16s, %u entries, %lu MiB in %u containers of %lu MiB (%.16s000..), user %x",
         g_hdr->version >= 2 ? g_hdr->title : "index v1", g_hdr->entries, g_hdr->total >> 20, g_hdr->containers,
         g_hdr->cap >> 20, g_hdr->prefix, g_user);
    return 0;
}

/* ---- the catalogue of installed titles (GFSCATALOG/catalog.bin) ---- */
struct gfs_cat_header {
    char magic[4];
    u32 version, header_size, entry_size, count, flags;
    u64 generation;
};
static const u8 *g_cat;
static u32 g_cat_n, g_cat_hsize, g_cat_esize;
static bool g_cat_read;

COLD u32 gfs_titles(void)
{
    if (g_cat_read || !sd_ready())
        return g_cat_n;
    g_cat_read = true;
    char mp[16], path[48];
    u8 st[0x78];
    if (gfs_mount("GFSCATALOG", 1, 0, mp) != 0)
        return 0;
    fmt(path, sizeof(path), "%s/catalog.bin", mp);
    int fd = k_open(path, 0, 0);
    u64 size = fd >= 0 && k_stat(path, st) == 0 ? *(u64 *)(st + 0x48) : 0;
    u8 *buf = size >= sizeof(struct gfs_cat_header) && size < (1 << 20) ? page_alloc(size) : NULL;
    s64 got = buf ? k_pread(fd, buf, size, 0) : -1;
    if (fd >= 0)
        k_close(fd);
    gfs_umount(mp, 2000);
    const struct gfs_cat_header *h = (const struct gfs_cat_header *)buf;
    if (got != (s64)size || !buf || memcmp(h->magic, "GFSC", 4) != 0 || h->header_size < sizeof(*h) ||
        h->entry_size < sizeof(struct gfs_cat_entry) || h->count > 256 ||
        (u64)h->header_size + (u64)h->count * h->entry_size > size) {
        logf("ghostfs: GFSCATALOG: damaged catalogue (%ld of %lu bytes): ignored", got, size);
        return 0;
    }
    g_cat = buf, g_cat_n = h->count, g_cat_hsize = h->header_size, g_cat_esize = h->entry_size;
    return g_cat_n;
}

COLD const struct gfs_cat_entry *gfs_title_at(u32 i)
{
    return i < gfs_titles() ? (const struct gfs_cat_entry *)(g_cat + g_cat_hsize + i * g_cat_esize) : NULL;
}

COLD int gfs_mount_title(const char *title)
{
    const struct gfs_cat_entry *e = NULL;
    for (u32 i = 0; i < gfs_titles() && !e; i++)
        if (strncmp(gfs_title_at(i)->title, title, 16) == 0)
            e = gfs_title_at(i);
    if (!e || (e->flags & GFS_CAT_INCOMPLETE)) {
        logf("ghostfs: %s is %s", title, e ? "not completely installed: rerun gfs_install.py" : "not installed");
        return -1;
    }
    char index[33], prefix[17];
    strlcpy(index, e->index, sizeof(index));
    strlcpy(prefix, e->prefix, sizeof(prefix));
    int rc = load_index(index, title, prefix);
    if (rc != 0 && rc != -1)
        logf("ghostfs: %s: no index %s (%x): reinstall it, or gfs_install.py --remove %s", title, index, rc, title);
    return rc;
}

/* The title to serve: the one run.py --title asked for, else the only one
 * installed.  Installs from before the catalogue have GFSINDEX (GHOSTIDX
 * until 2026-10-21) only. */
COLD int gfs_init(void)
{
    if (!sd_ready())
        return -1;
    const char *want = profile_requested();
    u32 n = gfs_titles(), ready = 0;
    if (!n) {
        int rc = load_index("GFSINDEX", want, NULL);
        if (rc > 0 || rc < -1)
            rc = load_index("GHOSTIDX", want, NULL);     /* installs before 2026-10-21 */
        if (rc > 0 || rc < -1)
            logf("ghostfs: no GFSCATALOG, GFSINDEX or GHOSTIDX (%x): nothing installed", rc);
        return rc ? -1 : 0;
    }
    char list[400];
    int len = 0;
    list[0] = 0;
    for (u32 i = 0; i < n; i++) {
        const struct gfs_cat_entry *e = gfs_title_at(i);
        if (len < (int)sizeof(list) - 80)
            len += fmt(list + len, sizeof(list) - len, "%s%.16s (%.40s%s)", i ? ", " : "", e->title, e->name,
                       (e->flags & GFS_CAT_INCOMPLETE) ? ", incomplete" : "");
        if (!(e->flags & GFS_CAT_INCOMPLETE))
            ready++;
    }
    logf("ghostfs: installed: %s", list);
    /* game selector: several titles and none asked for, or run.py --select;
     * boot.c runs it once SWRR's memory is free, then calls gfs_pick */
    if (ready && ((g_ctx->flags & CTX_FLAG_SELECT) || (!want && ready > 1)))
        return 1;
    return gfs_pick(NULL);
}

/* Serve title (the selector's choice); NULL: the one run.py --title asked
 * for, else the only complete one. */
COLD int gfs_pick(const char *title)
{
    if (!title)
        title = profile_requested();
    const struct gfs_cat_entry *only = NULL;
    u32 ready = 0;
    for (u32 i = 0; i < gfs_titles() && !title; i++)
        if (!(gfs_title_at(i)->flags & GFS_CAT_INCOMPLETE))
            ready++, only = gfs_title_at(i);
    if (!title && ready != 1) {
        logf("ghostfs: %s: pick one with run.py boot --title TITLE", ready ? "several titles" : "none is complete");
        return -1;
    }
    char t[17];
    strlcpy(t, title ? title : only->title, sizeof(t));
    return gfs_mount_title(t);
}

/* the title's progress container: <prefix>SAVE (GHOSTSAVE for Ghost of Tsushima) */
const char *gfs_save_name(void)
{
    static char name[32];
    if (!name[0] && g_hdr)
        fmt(name, sizeof(name), "%.16sSAVE", g_hdr->prefix);
    if (!name[0] && g_saves)
        fmt(name, sizeof(name), "%sSAVE", g_save_prefix);
    return name;
}

/* the title ID gfs_pack.py wrote in the index (version 2); a dump outside
 * ghostfs (run.py --app-root): the profile's, bound by the executable's
 * fingerprint; "" when unknown */
const char *title_id(void)
{
    static char id[17];
    if (!id[0] && g_hdr && g_hdr->version >= 2)
        memcpy(id, g_hdr->title, sizeof(g_hdr->title));
    if (!id[0] && g_saves)
        strlcpy(id, g_save_title, sizeof(id));
    if (!id[0] && !g_hdr)
        strlcpy(id, profile_title(), sizeof(id));
    return id;
}

/* the same, or NULL when unknown */
const char *gfs_title(void)
{
    const char *t = title_id();
    return t[0] ? t : NULL;
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

bool gfs_owns(int fd) { return g_fs ? g_fs->owns(fd) : gf_of(fd) != NULL; }

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
    if (g_fs)
        return g_fs->open(path, flags, fd_out);
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
    /* A real fd, so the title's fd numbers and their limit stay the
     * kernel's: SWRR's /app0 directory, so a call that misses the wrappers
     * fails (EISDIR, EBADF) instead of reading SWRR's files; SWRR's
     * eboot.bin if the sandbox refuses the directory (its sandbox refuses
     * /app0/sce_sys).  The path is copied to the stack first, as kernel paths
     * are everywhere else; literals live in the core's JIT (RX) image */
    static int dir_ok = -1;
    char placeholder[32];
    strlcpy(placeholder, dir_ok ? "/app0" : "/app0/eboot.bin", sizeof(placeholder));
    int fd = k_open(placeholder, 0, 0);
    if (dir_ok == -1) {
        dir_ok = fd >= 0;
        logf("ghostfs: placeholder fds are %s (%x)", dir_ok ? "the host's /app0 directory" : "the host's eboot.bin",
             fd);
        if (!dir_ok) {
            strlcpy(placeholder, "/app0/eboot.bin", sizeof(placeholder));
            fd = k_open(placeholder, 0, 0);
        }
    }
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
    if (g_fs)
        return g_fs->close(fd);
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
    if (g_fs)
        return g_fs->pread(fd, buf, n, off);
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
    if (g_fs)
        return g_fs->read(fd, buf, n);
    struct gfile *f = gf_of(fd);
    s64 r = gfs_pread(fd, buf, n, (s64)f->pos);
    if (r > 0)
        f->pos += (u64)r;
    return r;
}

s64 gfs_lseek(int fd, s64 off, int whence)
{
    if (g_fs)
        return g_fs->lseek(fd, off, whence);
    struct gfile *f = gf_of(fd);
    s64 base = whence == 0 ? 0 : whence == 1 ? (s64)f->pos : whence == 2 ? (s64)g_ent[f->entry].size : -1;
    if (base < 0 || base + off < 0)
        return (s64)E_INVAL;
    f->pos = (u64)(base + off);
    return (s64)f->pos;
}

int gfs_fstat(int fd, void *st)
{
    if (g_fs)
        return g_fs->fstat(fd, st);
    DIAG(DIAG_STAT, "gfs fstat /app0/%s", ename(gf_of(fd)->entry));
    fill_stat(st, gf_of(fd)->entry);
    return 0;
}

/* returns 1 when the path is not ghostfs's */
int gfs_stat(const char *path, void *st, int *rc)
{
    char rel[512];
    if (g_fs)
        return g_fs->stat(path, st, rc);
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
    if (g_fs)
        return g_fs->getdents(fd, buf, size);
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
int vfs_getdents(int fd, void *buf, int size)
{
    if (gfs_owns(fd))
        return gfs_getdents(fd, buf, size);
    return ((int (*)(int, void *, int))host_real("sceKernelGetdents"))(fd, buf, size);
}
int vfs_stat(const char *path, void *st)
{
    int rc;
    if (gfs_stat(path, st, &rc) == 0)
        return rc;
    return K.stat(path, st);
}
