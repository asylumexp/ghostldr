/*
 * The title's save data, kept out of SWRR's.
 *
 * Inside SWRR every sceSaveData call works on SWRR's own save area, where
 * the PS2 memory card image (and with it the exploit) lives; the title's
 * save-data-memory writes there already made the system discard SWRR's
 * save memory as corrupt.  Redirect everything to files next to the title:
 *   <app root>/../savedata/sdmemory_<slot>.bin   save data memory
 *   <app root>/../savedata/<dirName>/            mounted saves ("/savedataN")
 */
#include "ghost.h"

void *host_real(const char *name);

#define O_RDONLY 0
#define O_RDWR 2
#define O_CREAT 0x200
#define O_DIRECTORY 0x20000
#define ERR_NOT_FOUND 0x809f0008
#define ERR_PARAMETER 0x809f0001
#define MAX_MOUNTS 4
#define MIN_SZ(a, b) ((a) < (b) ? (a) : (b))

struct save_param { /* SceSaveDataParam */
    char title[128], subtitle[128], detail[1024];
    u32 user_param, reserved0;
    s64 mtime;
    u8 reserved[32];
};

static char g_save_root[256];
static char g_mount_dir[MAX_MOUNTS][320];

static int k_open(const char *p, int flags, int mode)
{
    return ((int (*)(const char *, int, int))host_real("sceKernelOpen"))(p, flags, mode);
}
static int k_close(int fd)
{
    return ((int (*)(int))host_real("sceKernelClose"))(fd);
}
static s64 k_pread(int fd, void *b, u64 n, s64 off)
{
    return ((s64(*)(int, void *, u64, s64))host_real("sceKernelPread"))(fd, b, n, off);
}
static s64 k_pwrite(int fd, const void *b, u64 n, s64 off)
{
    return ((s64(*)(int, const void *, u64, s64))host_real("sceKernelPwrite"))(fd, b, n, off);
}
static int k_mkdir(const char *p)
{
    return ((int (*)(const char *, int))host_real("sceKernelMkdir"))(p, 0777);
}
static s64 k_size(const char *p)
{
    u8 st[0x78];
    if (((int (*)(const char *, void *))host_real("sceKernelStat"))(p, st) != 0)
        return -1;
    return *(s64 *)(st + 0x48); /* st_size */
}

/* With ghostfs (no kernel exploit) /data is out of reach: the title's saves
 * live in the SWRR save container <prefix>SAVE (GHOSTSAVE for Ghost),
 * mounted read/write for the length of one call and unmounted (= committed)
 * after it. */
#define ERR_SAVE_NOT_FOUND 0x809f0008
static char g_save_mp[16];
static volatile int g_save_lock;

static bool save_begin(void)
{
    if (!gfs_active())
        return true;
    while (__atomic_exchange_n(&g_save_lock, 1, __ATOMIC_ACQUIRE))
        K.usleep(1000);
    int rc = gfs_mount(gfs_save_name(), 2, 0, g_save_mp);
    if (rc == (int)ERR_SAVE_NOT_FOUND)
        rc = gfs_mount(gfs_save_name(), 6, profile_hdr()->save_blocks, g_save_mp); /* Ghost: 256 (8 MiB) */
    if (rc != 0) {
        logf("savedata: %s mount -> %x", gfs_save_name(), rc);
        __atomic_store_n(&g_save_lock, 0, __ATOMIC_RELEASE);
        return false;
    }
    return true;
}

static void save_end(void)
{
    if (!gfs_active())
        return;
    int rc = gfs_umount(g_save_mp, 60000);
    if (rc != 0)
        logf("savedata: %s commit -> %x", gfs_save_name(), rc);
    __atomic_store_n(&g_save_lock, 0, __ATOMIC_RELEASE);
}

static const char *save_root(void)
{
    if (gfs_active())
        return g_save_mp;
    if (!g_save_root[0]) {
        const char *app = (const char *)g_ctx->app_root;
        size_t n = strlen(app);
        while (n > 1 && app[n - 1] != '/')
            n--;
        fmt(g_save_root, sizeof(g_save_root), "%.*ssavedata", (int)n, app);
        k_mkdir(g_save_root);
    }
    return g_save_root;
}

/* "/savedataN/..." -> the mounted directory; used by remap_path */
const char *savedata_remap(const char *path, char *buf, size_t size)
{
    if (strncmp(path, "/savedata", 9) != 0 || path[9] < '0' || path[9] >= '0' + MAX_MOUNTS ||
        (path[10] != '/' && path[10] != 0))
        return NULL;
    u32 n = (u32)(path[9] - '0');
    if (!g_mount_dir[n][0])
        return NULL;
    fmt(buf, size, "%s%s", g_mount_dir[n], path + 10);
    return buf;
}

/* ---- save data memory ---- */
struct sdm_setup2 {
    u32 option;
    s32 user;
    u64 memory_size, icon_size;
    const struct save_param *init_param;
    const void *init_icon;
    u32 slot;
};
struct sdm_data {
    void *buf;
    u64 size;
    s64 offset;
    u8 reserved[40];
};
struct sdm_get2 {
    s32 user, pad;
    struct sdm_data *data;
    struct save_param *param;
    void *icon;
    u32 slot;
};
struct sdm_set2 {
    s32 user, pad;
    const struct sdm_data *data;
    const struct save_param *param;
    const void *icon;
    u32 num;
    u8 slot;
};

/* A small file of ghostldr's own next to the title's saves (automem.c's
 * learned memory plan): read up to size bytes, or write size bytes.  Bytes
 * read or written, or -1. */
s64 savedata_aux_read(const char *name, void *buf, u64 size)
{
    if (!save_begin())
        return -1;
    char p[320];
    fmt(p, sizeof(p), "%s/%s", save_root(), name);
    int fd = k_open(p, O_RDONLY, 0);
    s64 n = fd >= 0 ? k_pread(fd, buf, size, 0) : -1;
    if (fd >= 0)
        k_close(fd);
    save_end();
    return n;
}

s64 savedata_aux_write(const char *name, const void *buf, u64 size)
{
    if (!save_begin())
        return -1;
    char p[320];
    fmt(p, sizeof(p), "%s/%s", save_root(), name);
    int fd = k_open(p, O_RDWR | O_CREAT, 0666);
    s64 n = fd >= 0 ? k_pwrite(fd, buf, size, 0) : -1;
    if (fd >= 0)
        k_close(fd);
    save_end();
    return n;
}

static void sdm_path(char *buf, size_t size, u32 slot, const char *what)
{
    fmt(buf, size, "%s/sdmemory_%u.%s", save_root(), slot, what);
}

static s64 sdm_setup2_impl(const struct sdm_setup2 *s, u64 *result)
{
    char p[300];
    sdm_path(p, sizeof(p), s ? s->slot : 0, "bin");
    s64 had = k_size(p);
    int fd = k_open(p, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        logf("savedata: memory slot %u: cannot create %s (%x)", s ? s->slot : 0, p, fd);
        return fd;
    }
    if (s && (s64)s->memory_size > had && s->memory_size)
        k_pwrite(fd, "", 1, (s64)s->memory_size - 1); /* extend with zeros */
    k_close(fd);
    if (s && s->init_param && had <= 0) {
        sdm_path(p, sizeof(p), s->slot, "param");
        int pf = k_open(p, O_RDWR | O_CREAT, 0666);
        if (pf >= 0) {
            k_pwrite(pf, s->init_param, sizeof(*s->init_param), 0);
            k_close(pf);
        }
    }
    if (result)
        result[0] = had > 0 ? (u64)had : 0; /* existedMemorySize */
    logf("savedata: memory slot %u -> %s (%ld bytes existed, %lx requested)", s ? s->slot : 0, p, had,
         s ? s->memory_size : 0);
    return 0;
}

s64 hle_sdm_setup2(const struct sdm_setup2 *s, u64 *result)
{
    if (!save_begin())
        return ERR_NOT_FOUND;
    s64 rc = sdm_setup2_impl(s, result);
    save_end();
    return rc;
}

static s64 sdm_get2_impl(struct sdm_get2 *g)
{
    char p[300];
    sdm_path(p, sizeof(p), g ? g->slot : 0, "bin");
    int fd = k_open(p, O_RDONLY, 0);
    if (fd < 0)
        return ERR_NOT_FOUND;
    if (g->data && g->data->buf)
        k_pread(fd, g->data->buf, g->data->size, g->data->offset);
    k_close(fd);
    if (g->param) {
        sdm_path(p, sizeof(p), g->slot, "param");
        int pf = k_open(p, O_RDONLY, 0);
        if (pf >= 0) {
            k_pread(pf, g->param, sizeof(*g->param), 0);
            k_close(pf);
        }
    }
    return 0;
}

s64 hle_sdm_get2(struct sdm_get2 *g)
{
    if (!save_begin())
        return ERR_NOT_FOUND;
    s64 rc = sdm_get2_impl(g);
    save_end();
    return rc;
}

static s64 sdm_set2_impl(const struct sdm_set2 *s)
{
    char p[300];
    sdm_path(p, sizeof(p), s ? s->slot : 0, "bin");
    int fd = k_open(p, O_RDWR | O_CREAT, 0666);
    if (fd < 0)
        return fd;
    u32 n = s->num ? s->num : 1;
    for (u32 i = 0; s->data && i < n; i++)
        if (s->data[i].buf)
            k_pwrite(fd, s->data[i].buf, s->data[i].size, s->data[i].offset);
    k_close(fd);
    if (s->param) {
        sdm_path(p, sizeof(p), s->slot, "param");
        int pf = k_open(p, O_RDWR | O_CREAT, 0666);
        if (pf >= 0) {
            k_pwrite(pf, s->param, sizeof(*s->param), 0);
            k_close(pf);
        }
    }
    return 0;
}

s64 hle_sdm_set2(const struct sdm_set2 *s)
{
    if (!save_begin())
        return ERR_NOT_FOUND;
    s64 rc = sdm_set2_impl(s);
    save_end();
    return rc;
}

s64 hle_sdm_ok(void)
{
    return 0;
}

/* ---- mounted saves ---- */
struct mount2 {
    s32 user, pad;
    const char *dir_name; /* SceSaveDataDirName (char[32]) */
    u64 blocks;
    u32 mode;
};
struct mount_result {
    char mount_point[16];
    u64 required_blocks;
    u32 unused, status;
};
#define MOUNT_RDONLY 1
#define MOUNT_CREATE 4
#define MOUNT_CREATE2 32

s64 hle_save_mount2(const struct mount2 *m, struct mount_result *r)
{
    if (!m || !m->dir_name || !r)
        return ERR_PARAMETER;
    if (gfs_active()) {
        logf("savedata: mount %.32s (mode %x) not supported with ghostfs", m->dir_name, m->mode);
        return ERR_NOT_FOUND;
    }
    char dir[320];
    fmt(dir, sizeof(dir), "%s/%.32s", save_root(), m->dir_name);
    bool exists = k_size(dir) >= 0;
    if (!exists && !(m->mode & (MOUNT_CREATE | MOUNT_CREATE2))) {
        logf("savedata: mount %.32s (mode %x): not found", m->dir_name, m->mode);
        return ERR_NOT_FOUND;
    }
    if (!exists)
        k_mkdir(dir);
    u32 slot = 0;
    while (slot < MAX_MOUNTS && g_mount_dir[slot][0])
        slot++;
    if (slot == MAX_MOUNTS)
        return 0x809f0005; /* mount full */
    strlcpy(g_mount_dir[slot], dir, sizeof(g_mount_dir[slot]));
    memset(r, 0, sizeof(*r));
    fmt(r->mount_point, sizeof(r->mount_point), "/savedata%u", slot);
    r->status = exists ? 0 : 1; /* SCE_SAVE_DATA_MOUNT_STATUS_CREATED */
    logf("savedata: mount %.32s (mode %x) -> %s = %s%s", m->dir_name, m->mode, r->mount_point, dir,
         exists ? "" : " (created)");
    return 0;
}

static int mount_slot(const char *mp)
{
    if (!mp || strncmp(mp, "/savedata", 9) != 0 || mp[9] < '0' || mp[9] >= '0' + MAX_MOUNTS)
        return -1;
    return mp[9] - '0';
}

s64 hle_save_umount(const char *mp)
{
    int s = mount_slot(mp);
    if (s < 0 || !g_mount_dir[s][0])
        return ERR_PARAMETER;
    g_mount_dir[s][0] = 0;
    return 0;
}

s64 hle_save_set_param(const char *mp, u32 type, const void *buf, u64 size)
{
    int s = mount_slot(mp);
    if (s < 0 || !g_mount_dir[s][0] || !buf)
        return ERR_PARAMETER;
    char p[360];
    fmt(p, sizeof(p), "%s/.param", g_mount_dir[s]);
    struct save_param prm;
    memset(&prm, 0, sizeof(prm));
    int fd = k_open(p, O_RDWR | O_CREAT, 0666);
    if (fd < 0)
        return fd;
    k_pread(fd, &prm, sizeof(prm), 0);
    switch (type) {
    case 0: memcpy(&prm, buf, MIN_SZ(size, sizeof(prm))); break;
    case 1: strlcpy(prm.title, buf, sizeof(prm.title)); break;
    case 2: strlcpy(prm.subtitle, buf, sizeof(prm.subtitle)); break;
    case 3: strlcpy(prm.detail, buf, sizeof(prm.detail)); break;
    case 4: prm.user_param = *(const u32 *)buf; break;
    default: break;
    }
    k_pwrite(fd, &prm, sizeof(prm), 0);
    k_close(fd);
    return 0;
}

/* SceSaveDataDirNameSearchCond / SceSaveDataDirNameSearchResult */
struct search_cond {
    s32 user, pad;
    const void *title_id;
    const char *dir_name; /* pattern, '%' wildcard; NULL = all */
    u32 key, order;
};
struct search_result {
    u32 hits, pad;
    char *dir_names; /* char[32] each */
    u32 dir_names_num, set_num;
    struct save_param *params;
    void *infos;
};

static bool name_match(const char *pat, const char *name)
{
    if (!pat || !pat[0])
        return true;
    for (; *pat; pat++, name++) {
        if (*pat == '%')
            return true; /* prefix match up to the wildcard */
        if (*pat != *name)
            return false;
    }
    return *name == 0;
}

s64 hle_save_dir_search(const struct search_cond *c, struct search_result *r)
{
    if (!r)
        return ERR_PARAMETER;
    r->hits = 0, r->set_num = 0;
    if (gfs_active()) {              /* no mounted-save directories in GHOSTSAVE */
        logf("savedata: search '%.32s' -> 0 hits (ghostfs)", c && c->dir_name ? c->dir_name : "*");
        return 0;
    }
    int fd = k_open(save_root(), O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return 0;
    static u8 *buf; /* page_alloc'd: keeps the core image small */
    if (!buf && !(buf = page_alloc(4096))) {
        k_close(fd);
        return 0;
    }
    s64 n;
    while ((n = ((int (*)(int, void *, u64))host_real("sceKernelGetdents"))(fd, buf, 4096)) > 0) {
        for (s64 off = 0; off < n;) {
            u16 reclen = *(u16 *)(buf + off + 4);
            u8 type = buf[off + 6], nlen = buf[off + 7];
            char *name = (char *)buf + off + 8;
            if (!reclen)
                break;
            off += reclen;
            if (type != 4 /* DT_DIR */ || name[0] == '.' || nlen > 31)
                continue;
            char nm[32];
            memcpy(nm, name, nlen);
            nm[nlen] = 0;
            if (!name_match(c ? c->dir_name : NULL, nm))
                continue;
            if (r->set_num < r->dir_names_num && r->dir_names) {
                memset(r->dir_names + r->set_num * 32, 0, 32);
                memcpy(r->dir_names + r->set_num * 32, nm, nlen);
                if (r->params) {
                    char p[360];
                    fmt(p, sizeof(p), "%s/%s/.param", save_root(), nm);
                    memset(&r->params[r->set_num], 0, sizeof(struct save_param));
                    int pf = k_open(p, O_RDONLY, 0);
                    if (pf >= 0) {
                        k_pread(pf, &r->params[r->set_num], sizeof(struct save_param), 0);
                        k_close(pf);
                    }
                }
                r->set_num++;
            }
            r->hits++;
        }
    }
    k_close(fd);
    logf("savedata: search '%.32s' -> %u hits", c && c->dir_name ? c->dir_name : "*", r->hits);
    return 0;
}

s64 hle_save_not_found(void)
{
    return ERR_NOT_FOUND;
}
