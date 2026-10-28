/*
 * The core and the console the save data family sees, faked for
 * savedata_host.c (and for the core's own savedata.c, which the Ghost replay
 * runs beside it):
 *   - libkernel's file calls on a temporary directory, with PS4 errors, stat
 *     and dirent layouts;
 *   - SWRR's save service as measured (savedata.md §1.1): containers whose
 *     writes become durable only when unmounted, 16 mount slots with SWRR's
 *     own save on /savedata0, CREATE on an existing name -> EXISTS, a storage
 *     cap -> NO_SPACE_FS with the blocks needed, BUSY while files are open,
 *     BUSY when a full container is unmounted, BROKEN on demand, ENOSPC past
 *     a container's size;
 *   - ghostfs holding 12 real slots with data containers, refilled before
 *     every mount, and gfs_slot_reserve's arithmetic from ghostfs.c;
 *   - remap_path with the extension's hooks, vfs_* on the title's app0.
 * It counts what must never happen: a real call with a name that is not the
 * title's (SWRR's save, ghostfs's index or data containers), a path reaching
 * SWRR's /savedata0, a path outside the test's directories.
 */
#include "savedata_fake.h"

/* savedata_os.c */
int os_open(const char *p, int flags, int mode);
int os_close(int fd);
long os_pread(int fd, void *b, u64 n, long off);
long os_pwrite(int fd, const void *b, u64 n, long off);
int os_stat(const char *p, int *is_dir, long *size);
int os_fstat_size(int fd, long *size);
int os_mkdir(const char *p);
int os_unlink(const char *p);
int os_rmdir(const char *p);
int os_rename(const char *a, const char *b);
int os_exists(const char *p);
void *os_opendir(const char *p);
void os_closedir(void *d);
long os_telldir(void *d);
void os_seekdir(void *d, long pos);
int os_readdir(void *d, char *name, int cap);
int os_rm_rf(const char *p);
int os_cp_r(const char *from, const char *to);
int os_mkdir_p(const char *p);
u64 os_usage_blocks(const char *p);
int os_write_file(const char *p, const void *b, u64 n);
long os_read_file(const char *p, void *b, u64 max);
void *os_calloc(u64 n);
void os_free(void *p);
void os_puts(const char *s);

#define E_PARAMETER ((s32)0x809F0000)
#define E_BUSY ((s32)0x809F0003)
#define E_NOT_MOUNTED ((s32)0x809F0004)
#define E_EXISTS ((s32)0x809F0007)
#define E_NOT_FOUND ((s32)0x809F0008)
#define E_NO_SPACE_FS ((s32)0x809F000A)
#define E_INTERNAL ((s32)0x809F000B)
#define E_MOUNT_FULL ((s32)0x809F000C)
#define E_BROKEN ((s32)0x809F000F)
#define K_ENOENT ((s32)0x80020002)
#define K_EBADF ((s32)0x80020009)
#define K_ENOSPC ((s32)0x8002001C)
#define K_EROFS ((s32)0x8002001E)
#define PS4_O_DIRECTORY 0x20000

struct kfuncs K;
struct boot_ctx *g_ctx;
struct module *g_modules, *g_main_mod;
u64 g_reported_dmem;

static struct boot_ctx g_boot;
static struct module g_mod;
static u8 g_procparam[0x40];
static char g_root[256], g_app_root[300], g_prefix[16], g_save_name[32];
static bool g_gfs;

/* ---- log ---- */

static char g_lines[256][256];
static u32 g_nlines;

void logf(const char *f, ...)
{
    char line[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    vfmt(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    strlcpy(g_lines[g_nlines++ % 256], line, 256);
    char out[540];
    fmt(out, sizeof(out), "  log: %s", line);
    os_puts(out);
}

bool fake_log_has(const char *needle)
{
    for (u32 i = 0; i < g_nlines && i < 256; i++)
        if (strstr(g_lines[i], needle))
            return true;
    return false;
}

void fake_log_clear(void) { g_nlines = 0; }

/* ---- the save service ---- */

#define NSLOT 16
#define DATA_SLOTS 12 /* ghostfs.c: GFS_SLOTS, GFS_SPARE 2, GFS_MIN_DATA 8 */

struct cont {
    bool exists, broken;
    s32 user;
    u64 blocks;
    char name[32];
};
static struct cont g_conts[256];
static u32 g_nconts;

struct rslot {
    bool used, data, rw;
    s32 cont; /* -1: SWRR's own save or a ghostfs data container */
    char stage[400];
};
static struct rslot g_slot[NSLOT];
static u32 g_violations, g_swrr_touched, g_bad_paths, g_max_used, g_reserved;
static u64 g_storage_cap;
static bool g_refuse_destruct;
static int g_last_mode;
static char g_names[4096];

struct ofd {
    int fd;
    s32 slot;
    void *dir;
    u32 ino;
};
static struct ofd g_fds[256];

static u32 used_slots(void)
{
    u32 n = 0;
    for (u32 i = 0; i < NSLOT; i++)
        n += g_slot[i].used;
    return n;
}

static void note_used(void)
{
    if (used_slots() > g_max_used)
        g_max_used = used_slots();
}

static void cont_path(const struct cont *c, char *out, size_t size)
{
    fmt(out, size, "%s/cont/%08x/%s", g_root, (u32)c->user, c->name);
}

static s32 find_cont(s32 user, const char *name)
{
    for (u32 i = 0; i < g_nconts; i++)
        if (g_conts[i].user == user && strcmp(g_conts[i].name, name) == 0)
            return (s32)i;
    return -1;
}

static s32 slot_of_cont(s32 c)
{
    for (u32 i = 0; i < NSLOT; i++)
        if (g_slot[i].used && !g_slot[i].data && g_slot[i].cont == c)
            return (s32)i;
    return -1;
}

static bool is_digits(const char *s, u32 n)
{
    for (u32 i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return false;
    return true;
}

/* names a title's save calls must never reach the service with */
static bool denied_name(const char *name)
{
    size_t pl = strlen(g_prefix);
    return !strcmp(name, "SLUS-20268") || !strcmp(name, "SLES-50366") || !strcmp(name, "GFSINDEX") ||
           !strcmp(name, "GHOSTIDX") ||
           (pl && !strncmp(name, g_prefix, pl) && strlen(name) == pl + 3 && is_digits(name + pl, 3));
}

static void violation(const char *what, const char *name)
{
    g_violations++;
    logf("FAKE VIOLATION: %s %s", what, name);
}

static u32 data_cap(u32 reserved) { return DATA_SLOTS - (reserved > 2 ? reserved - 2 : 0); }

static u32 data_mounted(void)
{
    u32 n = 0;
    for (u32 i = 0; i < NSLOT; i++)
        n += g_slot[i].used && g_slot[i].data;
    return n;
}

/* ghostfs reads on: it mounts data containers up to its cap */
static void gfs_refill(void)
{
    if (!g_gfs)
        return;
    for (u32 i = 1; i < NSLOT && data_mounted() < data_cap(g_reserved); i++)
        if (!g_slot[i].used) {
            g_slot[i] = (struct rslot){true, true, false, -1, ""};
            note_used();
        }
}

int gfs_slot_reserve(void)
{
    if (!g_gfs)
        return 0;
    if (data_cap(g_reserved + 1) < 8)
        return -1;
    g_reserved++;
    for (u32 i = NSLOT; i-- > 1 && data_mounted() > data_cap(g_reserved);)
        if (g_slot[i].used && g_slot[i].data)
            g_slot[i].used = false; /* evicted */
    return 0;
}

void gfs_slot_release(void)
{
    if (g_gfs && g_reserved)
        g_reserved--;
}

int gfs_mount_user(s32 user, const char *dirname, int mode, u64 blocks, char mount_point[16], u64 *required_blocks)
{
    char name[32], dir[400];
    strlcpy(name, dirname, sizeof(name));
    if (user < 0)
        user = FAKE_USER0;
    g_last_mode = mode;
    if (strlen(g_names) + strlen(name) + 2 < sizeof(g_names)) {
        strlcat(g_names, name, sizeof(g_names));
        strlcat(g_names, " ", sizeof(g_names));
    }
    if (denied_name(name)) {
        violation("mount", name);
        return E_INTERNAL;
    }
    if ((mode & 8) && g_refuse_destruct)
        return E_PARAMETER;
    gfs_refill();
    s32 c = find_cont(user, name);
    bool exists = c >= 0 && g_conts[c].exists;
    if (mode & 4) {
        if (exists)
            return E_EXISTS;
        if (blocks > 32768 || !blocks)
            return E_PARAMETER;
        u64 total = 0;
        for (u32 i = 0; i < g_nconts; i++)
            total += g_conts[i].exists ? g_conts[i].blocks : 0;
        if (g_storage_cap && total + blocks > g_storage_cap) {
            if (required_blocks)
                *required_blocks = total + blocks - g_storage_cap;
            return E_NO_SPACE_FS;
        }
    } else {
        if (!exists)
            return E_NOT_FOUND;
        if (g_conts[c].broken)
            return E_BROKEN;
    }
    if (c >= 0 && slot_of_cont(c) >= 0)
        return E_BUSY;
    s32 s = -1;
    for (u32 i = 1; i < NSLOT && s < 0; i++)
        if (!g_slot[i].used)
            s = (s32)i;
    if (s < 0) {
        logf("FAKE: no real slot left for %s (%u data, %u reserved)", name, data_mounted(), g_reserved);
        return E_MOUNT_FULL;
    }
    if (mode & 4) {
        if (c < 0)
            c = (s32)g_nconts++;
        g_conts[c] = (struct cont){true, false, user, blocks, ""};
        strlcpy(g_conts[c].name, name, sizeof(g_conts[c].name));
        cont_path(&g_conts[c], dir, sizeof(dir));
        os_rm_rf(dir);
        os_mkdir_p(dir);
        char sys[440];
        fmt(sys, sizeof(sys), "%s/sce_sys", dir);
        os_mkdir(sys);
        fmt(sys, sizeof(sys), "%s/sce_sys/param.sfo", dir);
        os_write_file(sys, "SWRR's param.sfo for this container", 36);
    }
    cont_path(&g_conts[c], dir, sizeof(dir));
    struct rslot *r = &g_slot[s];
    *r = (struct rslot){true, false, (mode & 6) != 0, c, ""};
    if (r->rw) { /* writes go to a copy until the unmount commits it */
        fmt(r->stage, sizeof(r->stage), "%s/stage/%d", g_root, s);
        os_rm_rf(r->stage);
        char parent[300];
        fmt(parent, sizeof(parent), "%s/stage", g_root);
        os_mkdir_p(parent);
        os_cp_r(dir, r->stage);
    } else {
        strlcpy(r->stage, dir, sizeof(r->stage));
    }
    note_used();
    fmt(mount_point, 16, "/savedata%d", s);
    return 0;
}

int gfs_mount(const char *dirname, int mode, u64 blocks, char mount_point[16])
{
    return gfs_mount_user(-1, dirname, mode, blocks, mount_point, NULL);
}

static s32 mp_slot(const char *mp)
{
    if (strncmp(mp, "/savedata", 9) != 0 || mp[9] < '0' || mp[9] > '9')
        return -1;
    s32 n = mp[9] - '0';
    const char *e = mp + 10;
    if (*e >= '0' && *e <= '9' && n)
        n = n * 10 + (*e++ - '0');
    return *e == 0 && n < NSLOT ? n : -1;
}

int gfs_umount(const char mount_point[16], u32 max_ms)
{
    char mp[16];
    memcpy(mp, mount_point, 16);
    mp[15] = 0;
    s32 s = mp_slot(mp);
    if (s <= 0 || !g_slot[s].used || g_slot[s].data) {
        if (s == 0)
            violation("unmount", "SWRR's /savedata0");
        return E_NOT_MOUNTED;
    }
    for (u32 i = 0; i < 256; i++)
        if (g_fds[i].fd > 0 && g_fds[i].slot == s)
            return E_BUSY; /* a file is still open in it */
    struct rslot *r = &g_slot[s];
    struct cont *c = &g_conts[r->cont];
    if (r->rw) {
        if (os_usage_blocks(r->stage) >= c->blocks) {
            logf("FAKE: %s is full (%lu of %lu blocks): the commit fails", c->name, os_usage_blocks(r->stage),
                 c->blocks);
            return E_BUSY;
        }
        char dir[400];
        cont_path(c, dir, sizeof(dir));
        os_rm_rf(dir);
        os_rename(r->stage, dir);
    }
    r->used = false;
    return 0;
}

static s32 fake_real_delete(const u8 *req)
{
    s32 user = *(const s32 *)req;
    const char *dir = *(const char *const *)(req + 0x10);
    char name[33];
    memcpy(name, dir, 32);
    name[32] = 0;
    if (denied_name(name)) {
        violation("delete", name);
        return E_INTERNAL;
    }
    s32 c = find_cont(user, name);
    if (c < 0 || !g_conts[c].exists)
        return E_NOT_FOUND;
    if (slot_of_cont(c) >= 0)
        return E_BUSY;
    char p[400];
    cont_path(&g_conts[c], p, sizeof(p));
    os_rm_rf(p);
    g_conts[c].exists = false;
    return 0;
}

/* dlg_overlay.c's banner: logged */
void ov_banner(const char *text, u32 seconds) { logf("banner: %s", text); }

static s32 fake_initial_user(s32 *out)
{
    *out = FAKE_USER0;
    return 0;
}

bool gfs_active(void) { return g_gfs; }
const char *gfs_save_name(void) { return g_save_name; }
const char *gfs_title(void) { return g_gfs ? "CUSA99999" : NULL; }

/* ---- paths ---- */

/* the host path for a kernel path (vfs: the title's app0, else SWRR's) */
static void map_path(const char *p, char *host, size_t size, s32 *slot, bool vfs)
{
    *slot = -1;
    if (!strncmp(p, "/savedata", 9)) {
        const char *e = p + 9;
        s32 n = -1;
        if (*e >= '0' && *e <= '9') {
            n = *e++ - '0';
            if (*e >= '0' && *e <= '9' && n)
                n = n * 10 + (*e++ - '0');
        }
        if (n == 0 && (*e == '/' || !*e)) {
            g_swrr_touched++;
            logf("FAKE VIOLATION: %s reaches SWRR's own save", p);
            fmt(host, size, "%s/swrr_savedata0%s", g_root, e);
            return;
        }
        if (n > 0 && n < NSLOT && (*e == '/' || !*e) && g_slot[n].used) {
            if (g_slot[n].data) {
                violation("path into a ghostfs data container", p);
                fmt(host, size, "%s/nonexistent%s", g_root, p);
                return;
            }
            fmt(host, size, "%s%s", g_slot[n].stage, e);
            *slot = n;
            return;
        }
        fmt(host, size, "%s/nonexistent%s", g_root, p);
        return;
    }
    if (!strncmp(p, "/app0", 5) && (p[5] == '/' || !p[5])) {
        fmt(host, size, "%s/%s%s", g_root, vfs ? "app0" : "swrr_app0", p + 5);
        return;
    }
    size_t n = strlen(p);
    if (!strncmp(p, g_root, strlen(g_root)) || (n < strlen(g_root) && !strncmp(g_root, p, n) && g_root[n] == '/')) {
        strlcpy(host, p, size); /* the test's directories, or one above them (mkdirs: EEXIST) */
        return;
    }
    g_bad_paths++;
    logf("FAKE: a path outside the test: %s", p);
    fmt(host, size, "%s/nonexistent/%s", g_root, p);
}

static s32 kerr(long rc) { return rc < 0 ? (s32)(0x80020000u | (u32)-rc) : (s32)rc; }

static struct ofd *fd_rec(int fd)
{
    for (u32 i = 0; i < 256; i++)
        if (g_fds[i].fd == fd && fd > 0)
            return &g_fds[i];
    return NULL;
}

static int k_open(const char *path, int flags, int mode)
{
    char host[600];
    s32 slot;
    map_path(path, host, sizeof(host), &slot, false);
    if (slot > 0 && !g_slot[slot].rw && (flags & 3))
        return K_EROFS;
    int fd = os_open(host, flags, mode);
    if (fd < 0)
        return kerr(fd);
    for (u32 i = 0; i < 256; i++)
        if (g_fds[i].fd <= 0) {
            int is_dir = 0;
            long size;
            os_stat(host, &is_dir, &size);
            g_fds[i] = (struct ofd){fd, slot, is_dir ? os_opendir(host) : NULL, 1};
            break;
        }
    return fd;
}

static int k_close(int fd)
{
    struct ofd *o = fd_rec(fd);
    if (!o)
        return K_EBADF;
    if (o->dir)
        os_closedir(o->dir);
    *o = (struct ofd){0, -1, NULL, 0};
    return kerr(os_close(fd));
}

static s64 k_pread(int fd, void *b, u64 n, s64 off) { return kerr(os_pread(fd, b, n, off)); }

static s64 k_pwrite(int fd, const void *b, u64 n, s64 off)
{
    struct ofd *o = fd_rec(fd);
    if (o && o->slot > 0) { /* the container's size is a hard limit */
        long size = 0;
        os_fstat_size(fd, &size);
        u64 end = (u64)off + n, old_b = size <= 0 ? 1 : ((u64)size + 32767) / 32768;
        u64 new_b = end > (u64)size ? (end + 32767) / 32768 : old_b;
        struct cont *c = &g_conts[g_slot[o->slot].cont];
        if (os_usage_blocks(g_slot[o->slot].stage) + new_b - old_b > c->blocks)
            return K_ENOSPC;
    }
    return kerr(os_pwrite(fd, b, n, off));
}

static void fill_stat(u8 *st, int is_dir, long size)
{
    memset(st, 0, 0x78);
    *(u16 *)(st + 8) = is_dir ? 0x41ff : 0x81b6;
    *(s64 *)(st + 0x48) = size;
}

static int k_stat(const char *path, void *st)
{
    char host[600];
    s32 slot;
    int is_dir;
    long size;
    map_path(path, host, sizeof(host), &slot, false);
    int rc = os_stat(host, &is_dir, &size);
    if (rc < 0)
        return kerr(rc);
    fill_stat(st, is_dir, size);
    return 0;
}

#define PATH_CALL(fn, os_fn, write)                                                    \
    static int fn(const char *path)                                                    \
    {                                                                                  \
        char host[600];                                                                \
        s32 slot;                                                                      \
        map_path(path, host, sizeof(host), &slot, false);                              \
        if (write && slot > 0 && !g_slot[slot].rw)                                     \
            return K_EROFS;                                                            \
        return kerr(os_fn(host));                                                      \
    }
PATH_CALL(k_unlink, os_unlink, true)
PATH_CALL(k_rmdir, os_rmdir, true)

static int k_mkdir(const char *path, int mode)
{
    char host[600];
    s32 slot;
    map_path(path, host, sizeof(host), &slot, false);
    if (slot > 0 && !g_slot[slot].rw)
        return K_EROFS;
    return kerr(os_mkdir(host));
}

static int k_rename(const char *from, const char *to)
{
    char a[600], b[600];
    s32 sa, sb;
    map_path(from, a, sizeof(a), &sa, false);
    map_path(to, b, sizeof(b), &sb, false);
    return kerr(os_rename(a, b));
}

/* FreeBSD dirents: fileno u32, reclen u16, type u8, namlen u8, name */
static int k_getdents(int fd, void *buf, int n)
{
    struct ofd *o = fd_rec(fd);
    if (!o || !o->dir)
        return K_EBADF;
    int used = 0;
    char name[256];
    for (;;) {
        long pos = os_telldir(o->dir);
        if (!os_readdir(o->dir, name, sizeof(name)))
            break;
        size_t len = strlen(name);
        int reclen = (int)((8 + len + 1 + 3) & ~3ul);
        if (used + reclen > n) {
            os_seekdir(o->dir, pos);
            break;
        }
        u8 *d = (u8 *)buf + used;
        *(u32 *)d = o->ino++;
        *(u16 *)(d + 4) = (u16)reclen;
        d[6] = 0;
        d[7] = (u8)len;
        memcpy(d + 8, name, len + 1);
        used += reclen;
    }
    return used;
}

static s64 g_now = 1700000000;
static int k_gettimeofday(s64 *tv)
{
    tv[0] = g_now++;
    tv[1] = 0;
    return 0;
}

static int k_usleep(u32 us) { return 0; }

/* hle_system.c's title user: the game selector's pick (fake_pick_user) */
static s32 g_picked = -1;
void fake_pick_user(s32 user) { g_picked = user; }
s32 title_user(void) { return g_picked >= 0 ? g_picked : FAKE_USER0; }
bool title_user_picked(void) { return g_picked >= 0 && g_picked != FAKE_USER0; }
static int k_getpid(void) { return 77; }

/* the host's own sceKernelGetAppInfo: SWRR US */
static s32 k_app_info(s32 pid, u8 *info)
{
    if (pid != 77)
        return E_INTERNAL;
    strlcpy((char *)info + 0x10, "CUSA03474", 10);
    return 0;
}

/* the real save service for other titles: none of them has saves here */
static u32 g_real_searches, g_real_mounts;
static s32 fake_real_search(const u8 *cond, u8 *result)
{
    g_real_searches++;
    *(u32 *)result = 0;         /* hitNum */
    *(u32 *)(result + 0x14) = 0; /* setNum */
    return 0;
}
static s32 fake_real_mount(const u8 *m, u8 *result)
{
    g_real_mounts++;
    return (s32)0x809F0008; /* NOT_FOUND */
}
u32 fake_real_calls(void) { return g_real_searches + g_real_mounts; }

void *host_real(const char *name)
{
    static const struct {
        const char *name;
        void *fn;
    } t[] = {
        {"sceKernelOpen", k_open},       {"sceKernelClose", k_close},   {"sceKernelPread", k_pread},
        {"sceKernelPwrite", k_pwrite},   {"sceKernelStat", k_stat},     {"sceKernelMkdir", k_mkdir},
        {"sceKernelUnlink", k_unlink},   {"sceKernelRmdir", k_rmdir},   {"sceKernelRename", k_rename},
        {"sceKernelGetdents", k_getdents}, {"sceKernelGettimeofday", k_gettimeofday},
        {"sceKernelGetAppInfo", k_app_info},
    };
    for (u32 i = 0; i < ARRAY_SIZE(t); i++)
        if (!strcmp(t[i].name, name))
            return t[i].fn;
    logf("FAKE: host_real(%s) is not faked", name);
    return NULL;
}

void *host_real_in(const char *mod, const char *name)
{
    if (!strcmp(mod, "libSceSaveData") && !strcmp(name, "sceSaveDataDelete"))
        return fake_real_delete;
    if (!strcmp(mod, "libSceSaveData") && !strcmp(name, "sceSaveDataDirNameSearch"))
        return fake_real_search;
    if (!strcmp(mod, "libSceSaveData") && !strcmp(name, "sceSaveDataMount"))
        return fake_real_mount;
    if (!strcmp(mod, "libSceUserService") && !strcmp(name, "sceUserServiceGetInitialUser"))
        return fake_initial_user;
    logf("FAKE: host_real_in(%s, %s) is not faked", mod, name);
    return NULL;
}

/* ---- remap_path (hle.c) and the loader's file access (ghostfs.c) ---- */

static const char *(*g_hooks[4])(const char *, char *, size_t);

int remap_hook_add(const char *(*fn)(const char *path, char *buf, size_t size))
{
    for (u32 i = 0; i < 4; i++)
        if (!g_hooks[i]) {
            g_hooks[i] = fn;
            return 0;
        }
    return -1;
}

const char *remap_path(const char *path, char *buf, size_t size)
{
    if (!path)
        return path;
    for (u32 i = 0; i < 4 && g_hooks[i]; i++) {
        const char *h = g_hooks[i](path, buf, size);
        if (h)
            return h;
    }
    if (!g_gfs && !strncmp(path, "/app0", 5) && (path[5] == '/' || !path[5])) {
        fmt(buf, size, "%s%s", g_app_root, path + 5);
        return buf;
    }
    return path;
}

static long g_vfs_pos[1024];

int vfs_open(const char *path, int flags, int mode)
{
    char host[600];
    s32 slot;
    map_path(path, host, sizeof(host), &slot, true);
    int fd = os_open(host, flags, mode);
    if (fd >= 0 && fd < 1024)
        g_vfs_pos[fd] = 0;
    return kerr(fd);
}

s64 vfs_read(int fd, void *buf, u64 n)
{
    long r = os_pread(fd, buf, n, g_vfs_pos[fd]);
    if (r > 0)
        g_vfs_pos[fd] += r;
    return kerr(r);
}

int vfs_close(int fd) { return kerr(os_close(fd)); }

int vfs_stat(const char *path, void *st)
{
    char host[600];
    s32 slot;
    int is_dir;
    long size;
    map_path(path, host, sizeof(host), &slot, true);
    int rc = os_stat(host, &is_dir, &size);
    if (rc < 0)
        return kerr(rc);
    fill_stat(st, is_dir, size);
    return 0;
}

/* ---- memory ---- */

void *page_alloc(size_t size) { return os_calloc(size); }
void page_free(void *addr, size_t size) { os_free(addr); }
void *arena_alloc(size_t size) { return os_calloc(size); }

void spin_lock(volatile int *l)
{
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        ;
}
void spin_unlock(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

const struct ghost_api g_fake_api = {
    .version = GHOST_API_VERSION,
    .size = sizeof(struct ghost_api),
    .ctx = &g_ctx,
    .k = &K,
    .modules = &g_modules,
    .main_mod = &g_main_mod,
    .reported_dmem = &g_reported_dmem,
    .logf = logf,
    .host_real = host_real,
    .host_real_in = host_real_in,
    .page_alloc = page_alloc,
    .page_free = page_free,
    .arena_alloc = arena_alloc,
    .spin_lock = spin_lock,
    .spin_unlock = spin_unlock,
    .remap_path = remap_path,
    .vfs_open = vfs_open,
    .vfs_read = vfs_read,
    .vfs_close = vfs_close,
    .vfs_stat = vfs_stat,
    .gfs_active = gfs_active,
    .gfs_mount = gfs_mount,
    .gfs_umount = gfs_umount,
    .gfs_save_name = gfs_save_name,
    .gfs_mount_user = gfs_mount_user,
    .gfs_slot_reserve = gfs_slot_reserve,
    .gfs_slot_release = gfs_slot_release,
    .gfs_title = gfs_title,
    .remap_hook_add = remap_hook_add,
};

/* ---- the test's controls ---- */

void fake_set_sdk(u32 v)
{
    *(u64 *)g_procparam = sizeof(g_procparam);
    *(u32 *)(g_procparam + 8) = 0x4942524F;
    *(u32 *)(g_procparam + 0x10) = v;
}

void fake_init(const char *root, bool gfs, const char *prefix, u32 sdk_version)
{
    for (u32 i = 0; i < 256; i++)
        if (g_fds[i].fd > 0)
            k_close(g_fds[i].fd);
    strlcpy(g_root, root, sizeof(g_root));
    os_rm_rf(g_root);
    os_mkdir_p(g_root);
    char p[400];
    fmt(p, sizeof(p), "%s/app0/sce_sys", g_root);
    os_mkdir_p(p);
    fmt(p, sizeof(p), "%s/swrr_app0", g_root);
    os_mkdir_p(p);
    fmt(g_app_root, sizeof(g_app_root), "%s/app0", g_root);
    g_gfs = gfs;
    strlcpy(g_prefix, prefix, sizeof(g_prefix));
    fmt(g_save_name, sizeof(g_save_name), "%sSAVE", prefix);
    memset(g_conts, 0, sizeof(g_conts));
    memset(g_slot, 0, sizeof(g_slot));
    g_nconts = 0;
    g_slot[0] = (struct rslot){true, false, true, -1, ""}; /* SWRR's own save */
    g_violations = g_swrr_touched = g_bad_paths = g_max_used = g_reserved = 0;
    g_storage_cap = 0;
    g_refuse_destruct = false;
    g_names[0] = 0;
    memset(g_hooks, 0, sizeof(g_hooks));
    gfs_refill();
    memset(&g_boot, 0, sizeof(g_boot));
    g_boot.app_root = (u64)g_app_root;
    g_ctx = &g_boot;
    K.usleep = k_usleep;
    K.getpid = k_getpid;
    memset(&g_mod, 0, sizeof(g_mod));
    g_mod.proc_param = (u64)g_procparam;
    g_main_mod = &g_mod;
    fake_set_sdk(sdk_version);
}

const char *fake_root(void) { return g_root; }

void fake_crash(void)
{
    for (u32 i = 0; i < 256; i++)
        if (g_fds[i].fd > 0)
            k_close(g_fds[i].fd);
    for (u32 i = 1; i < NSLOT; i++) {
        if (g_slot[i].used && !g_slot[i].data && g_slot[i].rw)
            os_rm_rf(g_slot[i].stage);
        g_slot[i].used = false;
    }
    g_reserved = 0;
    gfs_refill();
}

bool fake_cont_exists(s32 user, const char *name)
{
    s32 c = find_cont(user, name);
    return c >= 0 && g_conts[c].exists;
}

void fake_cont_dir(s32 user, const char *name, char *out, size_t size)
{
    struct cont c = {true, false, user, 0, ""};
    strlcpy(c.name, name, sizeof(c.name));
    cont_path(&c, out, size);
}

u64 fake_cont_blocks(s32 user, const char *name)
{
    s32 c = find_cont(user, name);
    return c >= 0 ? g_conts[c].blocks : 0;
}

void fake_cont_create(s32 user, const char *name, u64 blocks)
{
    char mp[16];
    if (gfs_mount_user(user, name, 6, blocks, mp, NULL) == 0)
        gfs_umount(mp, 0);
}

void fake_cont_delete(s32 user, const char *name)
{
    s32 c = find_cont(user, name);
    if (c >= 0 && g_conts[c].exists) {
        char p[400];
        cont_path(&g_conts[c], p, sizeof(p));
        os_rm_rf(p);
        g_conts[c].exists = false;
    }
}

void fake_set_broken(s32 user, const char *name, bool broken)
{
    s32 c = find_cont(user, name);
    if (c >= 0)
        g_conts[c].broken = broken;
}

bool fake_cont_mounted(s32 user, const char *name)
{
    s32 c = find_cont(user, name);
    return c >= 0 && slot_of_cont(c) >= 0;
}

void fake_set_storage_cap(u64 blocks) { g_storage_cap = blocks; }
void fake_refuse_destruct_off(bool refuse) { g_refuse_destruct = refuse; }
int fake_last_mode(void) { return g_last_mode; }
u32 fake_violations(void) { return g_violations; }
u32 fake_swrr_touched(void) { return g_swrr_touched; }
u32 fake_bad_paths(void) { return g_bad_paths; }
u32 fake_real_mounts(void) { return used_slots(); }
u32 fake_max_real_mounts(void) { return g_max_used; }
u32 fake_data_mounts(void) { return data_mounted(); }
u32 fake_reserved(void) { return g_reserved; }
const char *fake_mounted_names(void) { return g_names; }

u32 fake_open_fds(void)
{
    u32 n = 0;
    for (u32 i = 0; i < 256; i++)
        n += g_fds[i].fd > 0;
    return n;
}

long fake_read_host(const char *path, void *buf, u64 max) { return os_read_file(path, buf, max); }
int fake_write_host(const char *path, const void *buf, u64 n) { return os_write_file(path, buf, n); }
bool fake_host_exists(const char *path) { return os_exists(path); }
void fake_mkdir_host(const char *path) { os_mkdir_p(path); }
