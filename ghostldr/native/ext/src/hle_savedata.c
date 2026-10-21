/*
 * libSceSaveData, all virtual (research/hle-specs/savedata.md).
 *
 * Inside SWRR every real sceSaveData call acts on SWRR's own save area, where
 * its PS2 memory card and the Luac0re exploit live.  So the title never
 * reaches the real library: every libSceSaveData import binds here (the
 * policy at the end denies the rest with INTERNAL), and the real save
 * service only ever sees container names generated here (real_name_ok).
 *
 * Where the title's saves live:
 *   stock console (ghostfs, gfs_active()):
 *     <prefix>SAVE        the title's progress container (GHOSTSAVE for Ghost
 *                         of Tsushima): save data memory sdmemory_<slot>.bin
 *                         and .param exactly where they always were, plus
 *                         catalog.bin (dirName -> container, param, sizes),
 *                         icons/ and store/ (title_store.h);
 *     <prefix>D<nnn>SAVE  one per save directory the title creates;
 *     <prefix>M<ss>SAVE   save data memory too big for <prefix>SAVE.
 *   dev install (/data reachable): <app root>/../savedata/<user>/ with
 *     catalog.bin, icons/ and d/<dirName>/; save data memory in the user's
 *     directory, or still in the save root itself for files written before
 *     per-user directories (Ghost's /data/ghost/savedata/sdmemory_0.*).
 *
 * The title sees virtual mount points /savedata0..15, lowest free first.
 * remap_path sends their paths to the real container or directory through
 * sd_remap, and refuses (ENOENT) any /savedata path without a live mount:
 * the real /savedata0 is SWRR's own.
 *
 * Real mount slots: SWRR holds one, ghostfs up to 12 data containers.  A
 * title's mounted save holds one for its whole mount (gfs_slot_reserve, which
 * shrinks ghostfs's share down to 8), <prefix>SAVE one only for the length of
 * a call.  Everything that mounts runs under one sleeping lock.
 *
 * Also here: the title store (title_store.h) and the save data dialog's view
 * of the saves (savedata_hooks.h).
 */
#include "ext.h"
#include "savedata_hooks.h"
#include "title_store.h"

/* nothing here is hot: every call ends in the kernel or a save mount */
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

/* SCE_SAVE_DATA_ERROR_* (garlicsaves' PS4 table) */
#define E_PARAMETER ((s32)0x809F0000)
#define E_NOT_INITIALIZED ((s32)0x809F0001)
#define E_OUT_OF_MEMORY ((s32)0x809F0002)
#define E_BUSY ((s32)0x809F0003)
#define E_NOT_MOUNTED ((s32)0x809F0004)
#define E_EXISTS ((s32)0x809F0007)
#define E_NOT_FOUND ((s32)0x809F0008)
#define E_NO_SPACE_FS ((s32)0x809F000A)
#define E_INTERNAL ((s32)0x809F000B)
#define E_MOUNT_FULL ((s32)0x809F000C)
#define E_BAD_MOUNTED ((s32)0x809F000D)
#define E_BROKEN ((s32)0x809F000F)
#define E_INVALID_LOGIN_USER ((s32)0x809F0011)
#define E_MEMORY_NOT_READY ((s32)0x809F0012)
/* libkernel */
#define K_ENOENT ((s32)0x80020002)
#define K_ENOSPC ((s32)0x8002001C)
#define O_DIRECTORY 0x20000

/* mount modes */
#define MODE_RDONLY 1u
#define MODE_RDWR 2u
#define MODE_CREATE 4u
#define MODE_DESTRUCT_OFF 8u
#define MODE_COPY_ICON 0x10u
#define MODE_CREATE2 0x20u

#define BLOCK 32768ull
#define BLOCKS_MIN 96ull
#define BLOCKS_MAX 32768ull
#define SAVE_BLOCKS 256ull /* <prefix>SAVE when created here: 8 MiB, as before (Ghost's memory is 0x87800) */
#define VMAX 16u           /* virtual mount points /savedata0..15 */
#define MSLOTS 16u         /* save data memory slots */
#define CAT_MAX 256u
#define ICON_MAX (1u << 20)
#define COMMIT_MS 60000u
#define NEVENTS 16u

/* procparam SDK versions, 0xMMmmmppp */
#define SDK_1_70 0x01700000u
#define SDK_2_50 0x02500000u
#define SDK_4_50 0x04500000u
#define SDK_5_00 0x05000000u

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* ---- the library's structs (savedata.md §3) ---- */

struct sd_icon { /* SceSaveDataIcon */
    void *buf;
    u64 buf_size, data_size;
    u8 reserved[32];
};
struct sd_mount2 { /* SceSaveDataMount2 */
    s32 user;
    u32 pad;
    const char *dir;
    u64 blocks;
    u32 mode;
    u8 reserved[32];
};
struct sd_mount1 { /* SceSaveDataMount */
    s32 user;
    u32 pad;
    const char *title_id;
    const char *dir;
    const void *fingerprint;
    u64 blocks;
    u32 mode;
    u8 reserved[32];
};
struct sd_mount_result { /* SceSaveDataMountResult */
    char mount_point[16];
    u64 required_blocks;
    u32 unused;
    u32 mount_status; /* SDK 4.50+ */
    u8 reserved[28];
};
struct sd_mount_info { /* SceSaveDataMountInfo, SceSaveDataSearchInfo */
    u64 blocks, free_blocks;
    u8 reserved[32];
};
struct sd_delete { /* SceSaveDataDelete */
    s32 user;
    u32 pad;
    const char *title_id;
    const char *dir;
    u32 unused;
    u8 reserved[32];
};
struct sd_transfer_mount { /* SceSaveDataTransferringMount */
    s32 user;
    u32 pad;
    const char *title_id;
    const char *dir;
    const void *fingerprint;
    u8 reserved[32];
};
struct sd_search_cond { /* SceSaveDataDirNameSearchCond */
    s32 user;
    u32 pad;
    const char *title_id;
    const char *dir; /* pattern: % any run, _ one character; NULL or "": all */
    u32 key, order;
    u8 reserved[32];
};
struct sd_search_result { /* SceSaveDataDirNameSearchResult */
    u32 hit_num;
    u32 pad;
    char *dirs; /* SceSaveDataDirName[dirs_num] */
    u32 dirs_num;
    u32 set_num;                    /* SDK 1.70+ */
    struct sd_param *params;        /* SDK 1.70+ */
    struct sd_mount_info *infos;    /* SDK 2.50+ */
    u8 reserved[12];
};
struct sd_mem_setup2 { /* SceSaveDataMemorySetup2 */
    u32 option;
    s32 user;
    u64 memory_size, icon_memory_size;
    const struct sd_param *init_param; /* SDK 4.50+ */
    const struct sd_icon *init_icon;   /* SDK 4.50+ */
    u32 slot;                          /* SDK 5.00+ */
    u8 reserved[20];
};
struct sd_mem_setup_result { /* SceSaveDataMemorySetupResult */
    u64 existed;
    u8 reserved[16];
};
struct sd_mem_data { /* SceSaveDataMemoryData */
    void *buf;
    u64 buf_size;
    s64 offset;
    u8 reserved[40];
};
struct sd_mem_get2 { /* SceSaveDataMemoryGet2 */
    s32 user;
    u32 pad;
    struct sd_mem_data *data;
    struct sd_param *param;
    struct sd_icon *icon;
    u32 slot;
    u8 reserved[28];
};
struct sd_mem_set2 { /* SceSaveDataMemorySet2 */
    s32 user;
    u32 pad;
    const struct sd_mem_data *data;
    const struct sd_param *param;
    const struct sd_icon *icon;
    u32 data_num; /* SDK 5.00+ */
    u32 slot;     /* SDK 5.00+ */
    u8 reserved[32];
};
struct sd_mem_sync { /* SceSaveDataMemorySync */
    s32 user;
    u32 slot;
    u32 option;
    u8 reserved[28];
};
struct sd_backup { /* SceSaveDataBackup */
    s32 user;
    u32 pad;
    const char *title_id;
    const char *dir;
    const void *param;
    u8 reserved[32];
};
struct sd_check_backup { /* SceSaveDataCheckBackupData */
    s32 user;
    u32 pad;
    const char *title_id;
    const char *dir;
    struct sd_param *param;
    struct sd_icon *icon;
    u8 reserved[32];
};
struct sd_restore_backup { /* SceSaveDataRestoreBackupData */
    s32 user;
    u32 pad;
    const char *title_id;
    const char *dir;
    const void *fingerprint;
    u32 unused;
    u8 reserved[32];
};
struct sd_event { /* SceSaveDataEvent */
    u32 type; /* 1 UMOUNT_BACKUP, 2 BACKUP, 3 SAVE_DATA_MEMORY_SYNC */
    s32 error_code;
    s32 user;
    u32 pad;
    char title_id[16];
    char dir[32];
    u8 reserved[40];
};

#define OFF(t, f) __builtin_offsetof(struct t, f)
_Static_assert(sizeof(struct sd_param) == 0x530 && OFF(sd_param, user_param) == 0x500 && OFF(sd_param, mtime) == 0x508,
               "SceSaveDataParam");
_Static_assert(sizeof(struct sd_icon) == 0x38 && OFF(sd_icon, data_size) == 0x10, "SceSaveDataIcon");
_Static_assert(sizeof(struct sd_mount2) == 0x40 && OFF(sd_mount2, blocks) == 0x10 && OFF(sd_mount2, mode) == 0x18,
               "SceSaveDataMount2");
_Static_assert(sizeof(struct sd_mount1) == 0x50 && OFF(sd_mount1, dir) == 0x10 && OFF(sd_mount1, blocks) == 0x20 &&
                   OFF(sd_mount1, mode) == 0x28,
               "SceSaveDataMount");
_Static_assert(sizeof(struct sd_mount_result) == 0x40 && OFF(sd_mount_result, required_blocks) == 0x10 &&
                   OFF(sd_mount_result, mount_status) == 0x1C,
               "SceSaveDataMountResult");
_Static_assert(sizeof(struct sd_mount_info) == 0x30, "SceSaveDataMountInfo");
_Static_assert(sizeof(struct sd_delete) == 0x40 && OFF(sd_delete, dir) == 0x10, "SceSaveDataDelete");
_Static_assert(sizeof(struct sd_transfer_mount) == 0x40, "SceSaveDataTransferringMount");
_Static_assert(sizeof(struct sd_search_cond) == 0x40 && OFF(sd_search_cond, key) == 0x18, "SceSaveDataDirNameSearchCond");
_Static_assert(sizeof(struct sd_search_result) == 0x38 && OFF(sd_search_result, dirs_num) == 0x10 &&
                   OFF(sd_search_result, set_num) == 0x14 && OFF(sd_search_result, params) == 0x18 &&
                   OFF(sd_search_result, infos) == 0x20,
               "SceSaveDataDirNameSearchResult");
_Static_assert(sizeof(struct sd_mem_setup2) == 0x40 && OFF(sd_mem_setup2, init_param) == 0x18 &&
                   OFF(sd_mem_setup2, slot) == 0x28,
               "SceSaveDataMemorySetup2");
_Static_assert(sizeof(struct sd_mem_setup_result) == 0x18, "SceSaveDataMemorySetupResult");
_Static_assert(sizeof(struct sd_mem_data) == 0x40, "SceSaveDataMemoryData");
_Static_assert(sizeof(struct sd_mem_get2) == 0x40 && OFF(sd_mem_get2, slot) == 0x20, "SceSaveDataMemoryGet2");
_Static_assert(sizeof(struct sd_mem_set2) == 0x48 && OFF(sd_mem_set2, data_num) == 0x20 && OFF(sd_mem_set2, slot) == 0x24,
               "SceSaveDataMemorySet2");
_Static_assert(sizeof(struct sd_mem_sync) == 0x28 && OFF(sd_mem_sync, option) == 0x08, "SceSaveDataMemorySync");
_Static_assert(sizeof(struct sd_backup) == 0x40, "SceSaveDataBackup");
_Static_assert(sizeof(struct sd_check_backup) == 0x48 && OFF(sd_check_backup, icon) == 0x20, "SceSaveDataCheckBackupData");
_Static_assert(sizeof(struct sd_restore_backup) == 0x48 && OFF(sd_restore_backup, unused) == 0x20,
               "SceSaveDataRestoreBackupData");
_Static_assert(sizeof(struct sd_event) == 0x68 && OFF(sd_event, dir) == 0x20, "SceSaveDataEvent");

/* ---- state ---- */

static volatile int g_lock; /* everything below except the remap's reads of g_vm */
static bool g_init;         /* Initialize*d and not Terminated */

static void lock(void)
{
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE))
        K.usleep(1000);
}
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

/* libkernel's own file calls: never remapped */
static struct {
    int (*open)(const char *, int, int);
    int (*close)(int);
    s64 (*pread)(int, void *, u64, s64);
    s64 (*pwrite)(int, const void *, u64, s64);
    int (*stat)(const char *, void *);
    int (*mkdir)(const char *, int);
    int (*unlink)(const char *);
    int (*rmdir)(const char *);
    int (*rename)(const char *, const char *);
    int (*getdents)(int, void *, int);
    int (*gettimeofday)(void *);
} kf;
static bool g_kf_ok;

static bool kf_ready(void)
{
    if (g_kf_ok)
        return true;
    kf.open = host_real("sceKernelOpen");
    kf.close = host_real("sceKernelClose");
    kf.pread = host_real("sceKernelPread");
    kf.pwrite = host_real("sceKernelPwrite");
    kf.stat = host_real("sceKernelStat");
    kf.mkdir = host_real("sceKernelMkdir");
    kf.unlink = host_real("sceKernelUnlink");
    kf.rmdir = host_real("sceKernelRmdir");
    kf.rename = host_real("sceKernelRename");
    kf.getdents = host_real("sceKernelGetdents");
    kf.gettimeofday = host_real("sceKernelGettimeofday");
    g_kf_ok = kf.open && kf.close && kf.pread && kf.pwrite && kf.stat && kf.mkdir && kf.unlink && kf.rmdir &&
              kf.rename && kf.getdents && kf.gettimeofday;
    if (!g_kf_ok)
        logf("savedata: libkernel file calls missing: every save call fails");
    return g_kf_ok;
}

static s64 now(void)
{
    s64 tv[2] = {0, 0};
    kf.gettimeofday(tv);
    return tv[0];
}

static bool cont_backend(void) { return gfs_active(); }

/* looked up once (file scope: the tests start over with them cleared) */
static s32 g_user0 = -1;
static u32 g_sdk;
static char g_prefix[20], g_save_root[224];
static bool g_title_done;

static s32 initial_user(void)
{
    if (g_user0 < 0) {
        s32 (*get)(s32 *) = host_real_in("libSceUserService", "sceUserServiceGetInitialUser");
        s32 u = -1;
        if (get && get(&u) == 0 && u >= 0)
            g_user0 = u;
    }
    return g_user0;
}

/* the title's SDK version (procparam), 0xMMmmmppp; unknown: the newest */
static u32 sdk(void)
{
    if (!g_sdk) {
        const u8 *pp = g_main_mod ? (const u8 *)g_main_mod->proc_param : NULL;
        u32 v = 0xFFFFFFFFu;
        if (pp && *(const u64 *)pp >= 0x18 && *(const u32 *)(pp + 8) == 0x4942524F) /* "ORBI" */
            v = *(const u32 *)(pp + 0x10);
        g_sdk = v;
    }
    return g_sdk;
}

/* ---- files ---- */

static s64 file_size(const char *path)
{
    u8 st[0x78];
    return kf.stat(path, st) == 0 ? *(s64 *)(st + 0x48) : -1;
}

static bool is_dir(const char *path)
{
    u8 st[0x78];
    return kf.stat(path, st) == 0 && (*(u16 *)(st + 8) & 0xF000) == 0x4000;
}

static s32 write_all(int fd, const void *buf, u64 n, s64 off)
{
    for (u64 done = 0; done < n;) {
        s64 w = kf.pwrite(fd, (const u8 *)buf + done, n - done, off + (s64)done);
        if (w <= 0)
            return w < 0 ? (s32)w : K_ENOSPC;
        done += (u64)w;
    }
    return 0;
}

/* bytes read (short at the end of the file) or a negative error */
static s64 read_at(const char *path, void *buf, u64 n, s64 off)
{
    int fd = kf.open(path, O_RDONLY, 0);
    if (fd < 0)
        return fd;
    u64 done = 0;
    while (done < n) {
        s64 r = kf.pread(fd, (u8 *)buf + done, n - done, off + (s64)done);
        if (r <= 0)
            break;
        done += (u64)r;
    }
    kf.close(fd);
    return (s64)done;
}

/* the whole file replaced by buf (written beside it, then renamed over it) */
static s32 write_file(const char *path, const void *buf, u64 n)
{
    char tmp[320];
    if (fmt(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return E_INTERNAL;
    int fd = kf.open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return fd;
    s32 rc = write_all(fd, buf, n, 0);
    kf.close(fd);
    if (rc == 0)
        rc = kf.rename(tmp, path);
    if (rc != 0)
        kf.unlink(tmp);
    return rc;
}

/* every missing directory of path (the dev install's tree) */
static void mkdirs(const char *path)
{
    char p[320];
    strlcpy(p, path, sizeof(p));
    for (char *c = p + 1; *c; c++)
        if (*c == '/') {
            *c = 0;
            kf.mkdir(p, 0777);
            *c = '/';
        }
    kf.mkdir(p, 0777);
}

/* Blocks the tree under path uses: each file ceil(size / 32 KiB), at least
 * one, each directory one (as measured on SWRR's own save).  At the root of a
 * container the system's sce_sys is not the title's.  With remove: deletes
 * everything under path instead (path itself stays). */
#define WALK_DEPTH 8
#define WALK_BUF 2048
static u8 *g_walk; /* WALK_DEPTH dirent buffers, page_alloc'd on first use */

static u64 walk(char *path, size_t cap, u32 depth, bool root, bool remove)
{
    u64 used = 0;
    size_t plen = strlen(path);
    u8 *buf = g_walk + depth * WALK_BUF;
    for (bool again = true; again;) {
        again = false;
        int fd = kf.open(path, O_RDONLY | O_DIRECTORY, 0);
        if (fd < 0)
            break;
        int n;
        while ((n = kf.getdents(fd, buf, WALK_BUF)) > 0) {
            for (int off = 0; off + 8 <= n;) {
                u32 ino = *(u32 *)(buf + off);
                u16 reclen = *(u16 *)(buf + off + 4);
                u8 nlen = buf[off + 7];
                const char *name = (const char *)buf + off + 8;
                if (reclen < 8 || off + reclen > n)
                    break;
                off += reclen;
                if (!ino || (nlen == 1 && name[0] == '.') || (nlen == 2 && name[0] == '.' && name[1] == '.'))
                    continue;
                if (root && !remove && nlen == 7 && memcmp(name, "sce_sys", 7) == 0)
                    continue;
                if (plen + 2 + nlen > cap)
                    continue;
                path[plen] = '/';
                memcpy(path + plen + 1, name, nlen);
                path[plen + 1 + nlen] = 0;
                u8 st[0x78];
                if (kf.stat(path, st) == 0) {
                    if ((*(u16 *)(st + 8) & 0xF000) == 0x4000) {
                        used++;
                        if (depth + 1 < WALK_DEPTH)
                            used += walk(path, cap, depth + 1, false, remove);
                        if (remove && kf.rmdir(path) == 0)
                            again = true;
                    } else {
                        s64 size = *(s64 *)(st + 0x48);
                        used += size <= 0 ? 1 : (u64)(size + (s64)BLOCK - 1) / BLOCK;
                        if (remove && kf.unlink(path) == 0)
                            again = true;
                    }
                }
                path[plen] = 0;
            }
        }
        kf.close(fd);
        if (!remove)
            break;
    }
    return used;
}

static u64 used_blocks(const char *base, bool container)
{
    if (!g_walk && !(g_walk = page_alloc(WALK_DEPTH * WALK_BUF)))
        return 0;
    char p[320];
    strlcpy(p, base, sizeof(p));
    return walk(p, sizeof(p), 0, container, false);
}

static void remove_tree(const char *base)
{
    if (!g_walk && !(g_walk = page_alloc(WALK_DEPTH * WALK_BUF)))
        return;
    char p[320];
    strlcpy(p, base, sizeof(p));
    walk(p, sizeof(p), 0, false, true);
    kf.rmdir(p);
}

/* ---- the title (its param.sfo, if packed) ---- */

static char g_title_id[16], g_title_name[128];

static void sfo_get(const u8 *d, u32 n, const char *key, char *out, u32 size)
{
    if (n < 0x14 || memcmp(d, "\0PSF", 4) != 0)
        return;
    u32 kt = *(const u32 *)(d + 8), dt = *(const u32 *)(d + 12), count = *(const u32 *)(d + 16);
    for (u32 i = 0; i < count && 0x14 + 16 * (i + 1) <= n; i++) {
        const u8 *e = d + 0x14 + 16 * i;
        u32 ko = kt + *(const u16 *)e, len = *(const u32 *)(e + 4), off = dt + *(const u32 *)(e + 12);
        size_t klen = strlen(key);
        if (ko + klen + 1 > n || memcmp(d + ko, key, klen + 1) != 0 || off > n || len > n - off)
            continue;
        u32 c = MIN(len, size - 1);
        memcpy(out, d + off, c);
        out[c] = 0;
        return;
    }
}

/* a file of the title's app0 (ghostfs or the dev install), page_alloc'd */
static u8 *app_file(const char *rel, u32 max, u32 *size)
{
    char p[128], buf[320];
    u8 st[0x78];
    fmt(p, sizeof(p), "/app0/%s", rel);
    const char *rp = remap_path(p, buf, sizeof(buf));
    *size = 0;
    if (vfs_stat(rp, st) != 0)
        return NULL;
    s64 len = *(s64 *)(st + 0x48);
    if (len <= 0 || (u64)len > max)
        return NULL;
    u8 *data = page_alloc((u64)len);
    int fd = data ? vfs_open(rp, O_RDONLY, 0) : -1;
    if (fd < 0) {
        if (data)
            page_free(data, (u64)len);
        return NULL;
    }
    s64 got = 0;
    while (got < len) {
        s64 r = vfs_read(fd, data + got, (u64)(len - got));
        if (r <= 0)
            break;
        got += r;
    }
    vfs_close(fd);
    *size = (u32)got;
    return data;
}

static volatile int g_title_lock;

static void title_info(void)
{
    if (__atomic_load_n(&g_title_done, __ATOMIC_ACQUIRE))
        return;
    spin_lock(&g_title_lock);
    if (g_title_done) {
        spin_unlock(&g_title_lock);
        return;
    }
    const char *t = gfs_title();
    if (t)
        strlcpy(g_title_id, t, sizeof(g_title_id));
    u32 n;
    u8 *sfo = app_file("sce_sys/param.sfo", 1 << 16, &n);
    if (sfo) {
        if (!g_title_id[0])
            sfo_get(sfo, n, "TITLE_ID", g_title_id, sizeof(g_title_id));
        sfo_get(sfo, n, "TITLE", g_title_name, sizeof(g_title_name));
        page_free(sfo, n);
    }
    __atomic_store_n(&g_title_done, true, __ATOMIC_RELEASE);
    spin_unlock(&g_title_lock);
    logf("savedata: title %s (%s), SDK %x", g_title_id[0] ? g_title_id : "?", g_title_name[0] ? g_title_name : "-",
         sdk());
}

/* calls that take a SceSaveDataTitleId*: NULL or the title's own ID is the
 * title itself; another title's saves do not exist here (§1.8) */
static bool title_is_self(const char *tid)
{
    title_info();
    return !tid || !tid[0] || !g_title_id[0] || strncmp(tid, g_title_id, 10) == 0;
}

static void param_default(struct sd_param *p)
{
    title_info();
    memset(p, 0, sizeof(*p));
    strlcpy(p->title, g_title_name[0] ? g_title_name : "Saved Data", sizeof(p->title));
    p->mtime = now();
}

/* ---- real containers (stock console) ---- */

static const char *prefix(void)
{
    if (!g_prefix[0]) {
        const char *s = gfs_save_name();
        size_t n = s ? strlen(s) : 0;
        if (n > 4 && n - 4 < sizeof(g_prefix) && strcmp(s + n - 4, "SAVE") == 0) {
            memcpy(g_prefix, s, n - 4);
            g_prefix[n - 4] = 0;
        }
    }
    return g_prefix;
}

/* kind 'S': <prefix>SAVE, 'D': <prefix>D<nnn>SAVE, 'M': <prefix>M<ss>SAVE */
static void cont_name(char out[32], char kind, u32 n)
{
    if (kind == 'D')
        fmt(out, 32, "%sD%03uSAVE", prefix(), n);
    else if (kind == 'M')
        fmt(out, 32, "%sM%02uSAVE", prefix(), n);
    else
        fmt(out, 32, "%sSAVE", prefix());
}

static bool digits(const char *s, u32 n)
{
    for (u32 i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return false;
    return true;
}

/* S1: the only names the real save service is ever given */
static bool real_name_ok(const char *name)
{
    const char *p = prefix();
    size_t pl = strlen(p), n = strlen(name);
    if (!pl || n <= pl || strncmp(name, p, pl) != 0)
        return false;
    const char *r = name + pl;
    return strcmp(r, "SAVE") == 0 || (r[0] == 'D' && n == pl + 8 && digits(r + 1, 3) && strcmp(r + 4, "SAVE") == 0) ||
           (r[0] == 'M' && n == pl + 7 && digits(r + 1, 2) && strcmp(r + 3, "SAVE") == 0);
}

static u32 g_destruct = MODE_DESTRUCT_OFF; /* dropped if the service refuses it */

static s32 real_mount(s32 user, const char *name, u32 mode, u64 blocks, char mp[16], u64 *required)
{
    if (!real_name_ok(name)) {
        logf("savedata: refusing to mount %s for real: not a name of ours", name);
        return E_INTERNAL;
    }
    u32 m = (mode & MODE_RDWR) ? mode | g_destruct : mode;
    s32 rc = gfs_mount_user(user, name, (int)m, blocks, mp, required);
    if (rc == E_PARAMETER && m != mode) {
        logf("savedata: the save service refuses DESTRUCT_OFF (mode %x): mounting without it from now on", m);
        g_destruct = 0;
        rc = gfs_mount_user(user, name, (int)mode, blocks, mp, required);
    }
    return rc;
}

static s32 (*g_real_delete)(const void *);

static s32 real_delete(s32 user, const char *name)
{
    s32 (*del)(const void *) = g_real_delete;
    if (!real_name_ok(name) || strcmp(name + strlen(prefix()), "SAVE") == 0) {
        logf("savedata: refusing to delete %s for real", name);
        return E_INTERNAL;
    }
    if (!del)
        del = g_real_delete = host_real_in("libSceSaveData", "sceSaveDataDelete");
    if (!del)
        return E_INTERNAL;
    char dir[32];
    struct sd_delete d;
    memset(dir, 0, sizeof(dir));
    memset(&d, 0, sizeof(d));
    strlcpy(dir, name, sizeof(dir));
    d.user = user;
    d.dir = dir;
    return del(&d);
}

/* a real error a title may see as it is; the rest become INTERNAL */
static s32 real_error(s32 rc)
{
    if (rc == E_BUSY || rc == E_MOUNT_FULL || rc == E_NO_SPACE_FS || rc == E_BROKEN || rc == E_NOT_FOUND)
        return rc;
    return E_INTERNAL;
}

/* ---- sessions: a container mounted for one call (or a dev directory) ---- */

struct sess {
    s32 user;
    bool rw, mounted;
    char root[256];
};

static const char *save_root(void)
{
    if (!g_save_root[0]) {
        const char *app = g_ctx && g_ctx->app_root ? (const char *)g_ctx->app_root : "/data";
        size_t n = strlen(app);
        while (n > 1 && app[n - 1] != '/')
            n--;
        fmt(g_save_root, sizeof(g_save_root), "%.*ssavedata", (int)n, app);
        kf.mkdir(g_save_root, 0777);
    }
    return g_save_root;
}

static void user_dir(char *out, size_t size, s32 user)
{
    fmt(out, size, "%s/%08x", save_root(), (u32)user);
}

static s32 sess_mount(struct sess *s, s32 user, const char *name, bool rw, u64 create_blocks)
{
    memset(s, 0, sizeof(*s));
    s->user = user, s->rw = rw;
    char mp[16];
    s32 rc = real_mount(user, name, rw ? MODE_RDWR : MODE_RDONLY, 0, mp, NULL);
    if (rc == E_NOT_FOUND && rw && create_blocks)
        rc = real_mount(user, name, MODE_CREATE | MODE_RDWR, create_blocks, mp, NULL);
    if (rc != 0) {
        if (rc != E_NOT_FOUND)
            logf("savedata: mount %s (%s) -> %x", name, rw ? "rw" : "ro", (u32)rc);
        return rc;
    }
    s->mounted = true;
    memcpy(s->root, mp, 16);
    return 0;
}

/* <prefix>SAVE (created if create), or the user's dev directory */
static s32 sess_begin(struct sess *s, s32 user, bool rw, bool create)
{
    if (cont_backend()) {
        char name[32];
        cont_name(name, 'S', 0);
        return sess_mount(s, user, name, rw, create ? SAVE_BLOCKS : 0);
    }
    memset(s, 0, sizeof(*s));
    s->user = user, s->rw = rw;
    user_dir(s->root, sizeof(s->root), user);
    if (rw)
        mkdirs(s->root);
    return 0;
}

/* unmounting is the commit */
static s32 sess_end(struct sess *s)
{
    if (!s->mounted)
        return 0;
    s->mounted = false;
    s32 rc = gfs_umount(s->root, COMMIT_MS);
    if (rc != 0)
        logf("savedata: unmount %s (%s) -> %x", s->root, s->rw ? "commit" : "ro", (u32)rc);
    return rc;
}

/* ---- the catalogue: the title's save directories ---- */

#define CAT_EXISTS 1u
#define CAT_ICON 4u

struct cat_header { /* catalog.bin, 0x40 bytes, then count entries */
    char magic[4];  /* "GSDC" */
    u32 version;    /* 1 */
    u32 count;
    u32 next_id;    /* informational: ids are the lowest free */
    char title_id[16];
    u8 mem_container[16]; /* save data memory slot -> 1: in <prefix>M<ss>SAVE */
    u32 save_blocks;      /* <prefix>SAVE's size if known, else 0 */
    u32 reserved[3];
};
struct cat_entry { /* 0x600 bytes */
    char dir[32];
    u32 flags; /* CAT_* */
    u32 id;    /* <prefix>D<id>SAVE; icons/<id>.png */
    u64 blocks;      /* the title's (what GetMountInfo reports) */
    u64 real_blocks; /* the container's */
    u64 used_blocks; /* at the last read/write unmount */
    s64 mtime;
    u32 icon_size;
    u32 reserved0;
    struct sd_param param;
    u8 reserved[0x80];
};
_Static_assert(sizeof(struct cat_header) == 0x40, "catalogue header");
_Static_assert(sizeof(struct cat_entry) == 0x600 && OFF(cat_entry, param) == 0x50, "catalogue entry");

static struct cat_header g_cath;
static struct cat_entry *g_cat; /* page_alloc'd, g_cat_cap entries */
static u32 g_cat_cap;
static s32 g_cat_user = -1;     /* whose catalogue is loaded; -1: none */
static bool g_cat_bad;          /* catalog.bin was unreadable: keep it as catalog.bad */

static void cat_reset(s32 user)
{
    memset(&g_cath, 0, sizeof(g_cath));
    memcpy(g_cath.magic, "GSDC", 4);
    g_cath.version = 1;
    g_cath.next_id = 1;
    title_info();
    strlcpy(g_cath.title_id, g_title_id, sizeof(g_cath.title_id));
    g_cat_user = user;
    g_cat_bad = false;
}

static bool cat_room(u32 n)
{
    if (n <= g_cat_cap)
        return true;
    if (n > CAT_MAX)
        return false;
    u32 cap = MIN(CAT_MAX, MAX(n, g_cat_cap ? g_cat_cap * 2 : 10u));
    struct cat_entry *e = page_alloc(cap * sizeof(*e));
    if (!e)
        return false;
    if (g_cat) {
        memcpy(e, g_cat, g_cath.count * sizeof(*e));
        page_free(g_cat, g_cat_cap * sizeof(*e));
    }
    g_cat = e, g_cat_cap = cap;
    return true;
}

static s32 cat_read_in(const struct sess *s)
{
    char p[320];
    fmt(p, sizeof(p), "%s/catalog.bin", s->root);
    cat_reset(s->user);
    int fd = kf.open(p, O_RDONLY, 0);
    if (fd < 0)
        return 0; /* no saves yet */
    struct cat_header h;
    s32 rc = 0;
    s64 got = kf.pread(fd, &h, sizeof(h), 0);
    if (got != sizeof(h) || memcmp(h.magic, "GSDC", 4) != 0 || h.version != 1 || h.count > CAT_MAX) {
        logf("savedata: %s is unreadable (%ld bytes): starting a new catalogue, the old one is kept as catalog.bad",
             p, got);
        g_cat_bad = true;
    } else if (!cat_room(h.count)) {
        rc = E_OUT_OF_MEMORY;
    } else {
        u64 want = (u64)h.count * sizeof(struct cat_entry);
        got = want ? kf.pread(fd, g_cat, want, sizeof(h)) : 0;
        if (got != (s64)want) {
            logf("savedata: %s is cut short: %ld of %lu entry bytes", p, got, want);
            h.count = got > 0 ? (u32)((u64)got / sizeof(struct cat_entry)) : 0;
        }
        g_cath = h;
        for (u32 i = 0; i < h.count; i++)
            g_cat[i].dir[31] = 0;
    }
    kf.close(fd);
    if (rc != 0)
        g_cat_user = -1;
    return rc;
}

static s32 cat_write_in(const struct sess *s)
{
    char p[320];
    fmt(p, sizeof(p), "%s/catalog.bin", s->root);
    if (g_cat_bad) {
        char bad[320];
        fmt(bad, sizeof(bad), "%s/catalog.bad", s->root);
        kf.rename(p, bad);
        g_cat_bad = false;
    }
    u64 n = sizeof(g_cath) + (u64)g_cath.count * sizeof(struct cat_entry);
    u8 *buf = page_alloc(n);
    if (!buf)
        return E_OUT_OF_MEMORY;
    memcpy(buf, &g_cath, sizeof(g_cath));
    if (g_cath.count)
        memcpy(buf + sizeof(g_cath), g_cat, n - sizeof(g_cath));
    s32 rc = write_file(p, buf, n);
    page_free(buf, n);
    if (rc != 0)
        logf("savedata: writing %s -> %x", p, (u32)rc);
    return rc;
}

/* the user's catalogue in memory: read once (a read-only mount) */
static s32 cat_load(s32 user)
{
    if (g_cat_user == user)
        return 0;
    struct sess s;
    s32 rc = sess_begin(&s, user, false, false);
    if (rc == E_NOT_FOUND) { /* no <prefix>SAVE: no saves */
        cat_reset(user);
        return 0;
    }
    if (rc != 0)
        return rc;
    rc = cat_read_in(&s);
    sess_end(&s);
    return rc;
}

static s32 cat_find(const char *dir)
{
    for (u32 i = 0; i < g_cath.count; i++)
        if ((g_cat[i].flags & CAT_EXISTS) && strncmp(g_cat[i].dir, dir, 32) == 0)
            return (s32)i;
    return -1;
}

static void cat_remove(u32 i)
{
    memmove(&g_cat[i], &g_cat[i + 1], (g_cath.count - i - 1) * sizeof(*g_cat));
    g_cath.count--;
}

/* the lowest container / icon number no entry uses, 0 if none is left */
static u32 cat_new_id(void)
{
    u8 used[125];
    memset(used, 0, sizeof(used));
    for (u32 i = 0; i < g_cath.count; i++)
        if (g_cat[i].id < 1000)
            used[g_cat[i].id >> 3] |= (u8)(1 << (g_cat[i].id & 7));
    for (u32 id = 1; id < 1000; id++)
        if (!(used[id >> 3] & (1 << (id & 7))))
            return id;
    return 0;
}

/* an icon change made with a catalogue commit */
struct icon_op {
    u32 id; /* icons/<id>.png */
    const u8 *data;
    u32 size;
    bool remove;
};

static void icon_path(char *out, size_t size, const char *root, u32 id)
{
    fmt(out, size, "%s/icons/%u.png", root, id);
}

static void icon_apply(const struct sess *s, const struct icon_op *op)
{
    char p[320];
    icon_path(p, sizeof(p), s->root, op->id);
    if (op->remove) {
        kf.unlink(p);
        return;
    }
    char dir[320];
    fmt(dir, sizeof(dir), "%s/icons", s->root);
    kf.mkdir(dir, 0777);
    s32 rc = write_file(p, op->data, op->size);
    if (rc != 0)
        logf("savedata: icon %s (%u bytes) -> %x", p, op->size, (u32)rc);
}

/* the in-memory catalogue (and an icon change) written and committed */
static s32 cat_commit(s32 user, const struct icon_op *op)
{
    struct sess s;
    s32 rc = sess_begin(&s, user, true, true);
    if (rc != 0)
        return rc;
    if (op)
        icon_apply(&s, op);
    g_cath.next_id = cat_new_id();
    rc = cat_write_in(&s);
    s32 rc2 = sess_end(&s);
    return rc ? rc : rc2;
}

/* ---- virtual mount points ---- */

struct vmount {
    u32 live;  /* published: sd_remap may use base */
    s32 user;
    bool ro;
    bool slot; /* holds a real mount slot */
    bool param_set;
    u32 id;
    u64 blocks;
    char dir[32];
    char real[16];  /* real mount point (stock console) */
    char base[256]; /* what /savedataN stands for */
    struct sd_param param; /* SetParam's, written at the unmount */
    u8 *icon;              /* SaveIcon's, written at the unmount (page_alloc'd) */
    u32 icon_size;
};

static struct vmount *g_vm; /* page_alloc'd at the first mount */
static volatile int g_vlock; /* g_vm[].live and base, for sd_remap */
static u32 g_remap_denied;

/* "/savedata<v>" exactly, in a char[16] */
static s32 vm_index(const char *mp)
{
    if (!g_vm || !mp || strncmp(mp, "/savedata", 9) != 0 || mp[9] < '0' || mp[9] > '9')
        return -1;
    u32 v = (u32)(mp[9] - '0');
    u32 end = 10;
    if (mp[10] >= '0' && mp[10] <= '9' && v) {
        v = v * 10 + (u32)(mp[10] - '0');
        end = 11;
    }
    if (mp[end] != 0 || v >= VMAX || !g_vm[v].live)
        return -1;
    return (s32)v;
}

static bool vm_busy(s32 user, const char *dir)
{
    for (u32 v = 0; g_vm && v < VMAX; v++)
        if (g_vm[v].live && g_vm[v].user == user && strncmp(g_vm[v].dir, dir, 32) == 0)
            return true;
    return false;
}

static void vm_publish(struct vmount *vm, bool live)
{
    spin_lock(&g_vlock);
    vm->live = live;
    spin_unlock(&g_vlock);
}

static bool has_dotdot(const char *p)
{
    for (const char *c = strstr(p, "/.."); c; c = strstr(c + 1, "/.."))
        if (c[3] == '/' || c[3] == 0)
            return true;
    return false;
}

/* remap_path's hook: "/savedata<v>/rest" -> the save behind virtual mount
 * point v.  Every other /savedata path gets a path under a directory that
 * does not exist, so every call on it fails with ENOENT: the real /savedata0
 * is SWRR's own, and never the title's. */
static const char *sd_remap(const char *path, char *buf, size_t size)
{
    if (strncmp(path, "/savedata", 9) != 0)
        return NULL;
    const char *p = path + 9;
    u32 v = VMAX;
    if (*p >= '0' && *p <= '9') {
        v = (u32)(*p++ - '0');
        if (*p >= '0' && *p <= '9' && v)
            v = v * 10 + (u32)(*p++ - '0');
    }
    if (v < VMAX && (*p == '/' || *p == 0) && !has_dotdot(p) && g_vm) {
        char base[256];
        base[0] = 0;
        spin_lock(&g_vlock);
        if (g_vm[v].live)
            memcpy(base, g_vm[v].base, sizeof(base));
        spin_unlock(&g_vlock);
        if (base[0] && fmt(buf, size, "%s%s", base, p) < (int)size)
            return buf;
    }
    if (__atomic_fetch_add(&g_remap_denied, 1, __ATOMIC_RELAXED) < 32)
        logf("savedata: %s: no such mount (ENOENT)", path);
    fmt(buf, size, "/app0/ghostldr-no-savedata/%s", path + 1);
    return buf;
}

/* ---- events (UmountWithBackup, Backup, SyncSaveDataMemory) ---- */

static struct sd_event g_ev[NEVENTS];
static u32 g_ev_head, g_ev_count;

static void event_add(u32 type, s32 user, const char *dir)
{
    struct sd_event *e = &g_ev[(g_ev_head + g_ev_count) % NEVENTS];
    if (g_ev_count == NEVENTS)
        g_ev_head = (g_ev_head + 1) % NEVENTS; /* full: the oldest goes */
    else
        g_ev_count++;
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->user = user;
    title_info();
    strlcpy(e->title_id, g_title_id, sizeof(e->title_id));
    strlcpy(e->dir, dir, sizeof(e->dir));
}

/* ---- argument checks ---- */

/* a SceSaveDataDirName: 1-31 characters, no '/', not starting with '.' */
static bool dir_ok(const char *dir, char out[32])
{
    if (!dir)
        return false;
    u32 n = 0;
    while (n < 32 && dir[n])
        n++;
    if (n == 0 || n == 32 || dir[0] == '.')
        return false;
    for (u32 i = 0; i < n; i++)
        if (dir[i] == '/' || dir[i] == '\\')
            return false;
    memset(out, 0, 32);
    memcpy(out, dir, n);
    return true;
}

/* ---- Initialize / Terminate ---- */

static s32 sd_initialize(const void *params)
{
    if (!g_init)
        logf("savedata: initialized (virtual: SWRR's own save data is never touched)");
    g_init = true;
    return 0;
}

struct memslot {
    bool set_up;
    bool in_m;   /* in <prefix>M<ss>SAVE */
    bool legacy; /* dev: the save root's own sdmemory_* */
    s32 user;
    u64 size;    /* memorySize of the setup */
};
static struct memslot g_ms[MSLOTS];

static s32 sd_terminate(void)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    lock();
    for (u32 v = 0; g_vm && v < VMAX; v++)
        if (g_vm[v].live) {
            unlock();
            return E_BUSY;
        }
    for (u32 i = 0; i < MSLOTS; i++)
        g_ms[i].set_up = false;
    g_init = false;
    unlock();
    logf("savedata: terminated");
    return 0;
}

/* ---- Mount ---- */

static u64 real_blocks_for(u64 blocks)
{
    u64 rb = MAX(BLOCKS_MIN, blocks) + MAX(4ull, blocks / 32);
    if (rb > BLOCKS_MAX) {
        rb = BLOCKS_MAX;
        logf("savedata: a %lu-block save has no headroom in its container: filling it completely may fail to "
             "commit", blocks);
    }
    return rb;
}

static bool vm_ready(void)
{
    if (!g_vm)
        g_vm = page_alloc(VMAX * sizeof(*g_vm));
    return g_vm != NULL;
}

/* a new save directory: its container (or directory), entry and icon */
static s32 create_save(s32 user, const char *dir, u64 blocks, u32 mode, struct vmount *vm,
                       struct sd_mount_result *r)
{
    if (g_cath.count >= CAT_MAX || !cat_room(g_cath.count + 1)) {
        logf("savedata: %u saves: the catalogue is full", g_cath.count);
        return E_INTERNAL;
    }
    u32 id = cat_new_id();
    if (!id)
        return E_INTERNAL;
    u64 rb = real_blocks_for(blocks);
    char name[32];
    s32 rc;
    if (cont_backend()) {
        if (gfs_slot_reserve() != 0)
            return E_MOUNT_FULL;
        cont_name(name, 'D', id);
        u64 req = 0;
        rc = real_mount(user, name, MODE_CREATE | MODE_RDWR, rb, vm->real, &req);
        if (rc == E_EXISTS) { /* ours, left by a crash before the catalogue was written */
            logf("savedata: %s exists without a catalogue entry: deleting it and creating it again", name);
            real_delete(user, name);
            rc = real_mount(user, name, MODE_CREATE | MODE_RDWR, rb, vm->real, &req);
        }
        if (rc != 0) {
            gfs_slot_release();
            if (rc == E_NO_SPACE_FS)
                r->required_blocks = req;
            logf("savedata: creating %s (%lu blocks) for %s -> %x", name, rb, dir, (u32)rc);
            return real_error(rc);
        }
        vm->slot = true;
        strlcpy(vm->base, vm->real, sizeof(vm->base));
    } else {
        char ud[256];
        user_dir(ud, sizeof(ud), user);
        if (fmt(vm->base, sizeof(vm->base), "%s/d/%s", ud, dir) >= (int)sizeof(vm->base))
            return E_INTERNAL;
        if (is_dir(vm->base))
            remove_tree(vm->base); /* left over from a lost catalogue: a new save is empty */
        mkdirs(vm->base);
        if (!is_dir(vm->base))
            return E_INTERNAL;
    }
    struct cat_entry *e = &g_cat[g_cath.count];
    memset(e, 0, sizeof(*e));
    memcpy(e->dir, dir, 32);
    e->flags = CAT_EXISTS;
    e->id = id;
    e->blocks = blocks;
    e->real_blocks = cont_backend() ? rb : 0;
    param_default(&e->param);
    e->mtime = e->param.mtime;
    struct icon_op op = {id, NULL, 0, false};
    u8 *png = NULL;
    u32 png_size = 0;
    if (mode & MODE_COPY_ICON)
        png = app_file("sce_sys/save_data.png", ICON_MAX, &png_size);
    if (png) {
        op.data = png, op.size = png_size;
        e->flags |= CAT_ICON;
        e->icon_size = png_size;
    }
    g_cath.count++;
    rc = cat_commit(user, png ? &op : NULL);
    if (png)
        page_free(png, png_size);
    if (rc != 0) {
        logf("savedata: the catalogue entry for %s could not be committed (%x): undoing the create", dir, (u32)rc);
        g_cath.count--;
        if (cont_backend()) {
            gfs_umount(vm->real, COMMIT_MS);
            gfs_slot_release();
            vm->slot = false;
            real_delete(user, name);
        } else {
            remove_tree(vm->base);
        }
        return E_INTERNAL;
    }
    vm->user = user, vm->ro = false, vm->id = id, vm->blocks = blocks;
    memcpy(vm->dir, dir, 32);
    vm->param = e->param;
    logf("savedata: created %s (%lu blocks) in %s", dir, blocks, cont_backend() ? name : vm->base);
    return 0;
}

static s32 open_save(s32 user, u32 idx, bool rw, struct vmount *vm)
{
    struct cat_entry *e = &g_cat[idx];
    if (cont_backend()) {
        if (gfs_slot_reserve() != 0)
            return E_MOUNT_FULL;
        char name[32];
        cont_name(name, 'D', e->id);
        s32 rc = real_mount(user, name, rw ? MODE_RDWR : MODE_RDONLY, 0, vm->real, NULL);
        if (rc != 0) {
            gfs_slot_release();
            logf("savedata: mounting %s (%s) for %s -> %x", name, rw ? "rw" : "ro", e->dir, (u32)rc);
            return real_error(rc);
        }
        vm->slot = true;
        strlcpy(vm->base, vm->real, sizeof(vm->base));
    } else {
        char ud[256];
        user_dir(ud, sizeof(ud), user);
        fmt(vm->base, sizeof(vm->base), "%s/d/%s", ud, e->dir);
        if (!is_dir(vm->base))
            return E_NOT_FOUND;
    }
    vm->user = user, vm->ro = !rw, vm->id = e->id, vm->blocks = e->blocks;
    memcpy(vm->dir, e->dir, 32);
    vm->param = e->param;
    return 0;
}

static s32 mount_locked(s32 user, const char *dir, u64 blocks, u32 mode, struct sd_mount_result *r)
{
    bool create = (mode & (MODE_CREATE | MODE_CREATE2)) != 0;
    bool rw = create || (mode & MODE_RDWR);
    if (!kf_ready())
        return E_INTERNAL;
    if (!vm_ready())
        return E_OUT_OF_MEMORY;
    if (vm_busy(user, dir))
        return E_BUSY;
    u32 v = 0;
    while (v < VMAX && g_vm[v].live)
        v++;
    if (v == VMAX)
        return E_MOUNT_FULL;
    s32 rc = cat_load(user);
    if (rc != 0)
        return real_error(rc) == E_BROKEN ? E_BROKEN : E_INTERNAL;
    s32 idx = cat_find(dir);
    if (idx >= 0 && (mode & MODE_CREATE))
        return E_EXISTS;
    struct vmount *vm = &g_vm[v]; /* not live: sd_remap does not look at it */
    if (vm->icon)
        page_free(vm->icon, vm->icon_size);
    memset(vm, 0, sizeof(*vm));
    u32 status = 0;
    if (idx >= 0) {
        rc = open_save(user, (u32)idx, rw, vm);
        if (rc == E_NOT_FOUND) { /* deleted outside the title (PS5 settings): forget it */
            logf("savedata: %s is gone: dropping it from the catalogue", dir);
            struct icon_op op = {g_cat[idx].id, NULL, 0, true};
            bool icon = g_cat[idx].flags & CAT_ICON;
            cat_remove((u32)idx);
            cat_commit(user, icon ? &op : NULL);
            idx = -1;
        } else if (rc != 0) {
            return rc;
        }
    }
    if (idx < 0) {
        if (!create)
            return E_NOT_FOUND;
        memset(vm, 0, sizeof(*vm));
        rc = create_save(user, dir, blocks, mode, vm, r);
        if (rc != 0)
            return rc;
        status = 1; /* SCE_SAVE_DATA_MOUNT_STATUS_CREATED */
    }
    vm_publish(vm, true);
    memset(r, 0, sizeof(*r));
    fmt(r->mount_point, sizeof(r->mount_point), "/savedata%u", v);
    r->mount_status = status;
    logf("savedata: mount %s (mode %x) -> %s = %s%s", dir, mode, r->mount_point, vm->base, status ? " (created)" : "");
    return 0;
}

static s32 do_mount(s32 user, const char *title_id, const char *dirp, u64 blocks, u32 mode,
                    struct sd_mount_result *r)
{
    char dir[32];
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!dirp || !r)
        return E_PARAMETER;
    if (user < 0)
        return E_INVALID_LOGIN_USER;
    if (!dir_ok(dirp, dir))
        return E_PARAMETER;
    bool create = (mode & (MODE_CREATE | MODE_CREATE2)) != 0;
    if ((mode & MODE_CREATE) && (mode & MODE_CREATE2))
        return E_PARAMETER;
    if (!create && !(mode & (MODE_RDONLY | MODE_RDWR)))
        return E_PARAMETER;
    if (create && blocks > BLOCKS_MAX)
        return E_PARAMETER;
    if (!title_is_self(title_id)) {
        logf("savedata: mount %s of another title (%.10s): not found", dir, title_id);
        return E_NOT_FOUND;
    }
    lock();
    s32 rc = mount_locked(user, dir, blocks, mode, r);
    unlock();
    if (rc != 0 && rc != E_NOT_FOUND)
        logf("savedata: mount %s (mode %x, %lu blocks) -> %x", dir, mode, blocks, (u32)rc);
    return rc;
}

static s32 sd_mount2(const struct sd_mount2 *m, struct sd_mount_result *r)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!m)
        return E_PARAMETER;
    return do_mount(m->user, NULL, m->dir, m->blocks, m->mode, r);
}

static s32 sd_mount1(const struct sd_mount1 *m, struct sd_mount_result *r)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!m)
        return E_PARAMETER;
    return do_mount(m->user, m->title_id, m->dir, m->blocks, m->mode, r);
}

/* ---- Umount ---- */

static s32 do_umount(const char *mp, bool backup)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!mp)
        return E_PARAMETER;
    lock();
    s32 v = vm_index(mp);
    if (v < 0) {
        unlock();
        return E_NOT_FOUND;
    }
    struct vmount *vm = &g_vm[v];
    u64 used = vm->ro ? 0 : used_blocks(vm->base, vm->real[0] != 0);
    vm_publish(vm, false);
    if (vm->real[0]) {
        s32 rc = gfs_umount(vm->real, COMMIT_MS);
        if (rc != 0) { /* BUSY: the title still has files open in it */
            vm_publish(vm, true);
            unlock();
            logf("savedata: unmount %.16s (%s) -> %x", mp, vm->dir, (u32)rc);
            return rc == E_BUSY ? E_BUSY : E_INTERNAL;
        }
        if (vm->slot)
            gfs_slot_release();
        vm->slot = false;
    }
    if (!vm->ro) {
        s32 idx = cat_load(vm->user) == 0 ? cat_find(vm->dir) : -1;
        struct icon_op op = {vm->id, vm->icon, vm->icon_size, false};
        if (idx >= 0) {
            struct cat_entry *e = &g_cat[idx];
            s64 t = now();
            if (vm->param_set)
                e->param = vm->param;
            e->param.mtime = t;
            e->mtime = t;
            e->used_blocks = used;
            if (vm->icon)
                e->flags |= CAT_ICON, e->icon_size = vm->icon_size;
            s32 rc = cat_commit(vm->user, vm->icon ? &op : NULL);
            if (rc != 0)
                logf("savedata: %s is committed, but its param and time are not (%x)", vm->dir, (u32)rc);
        }
    }
    if (backup)
        event_add(1, vm->user, vm->dir); /* UMOUNT_BACKUP; no backup is taken (phase 1) */
    logf("savedata: unmount %.16s (%s)%s", mp, vm->dir, vm->ro ? "" : ", committed");
    if (vm->icon)
        page_free(vm->icon, vm->icon_size);
    vm->icon = NULL;
    vm->icon_size = 0;
    unlock();
    return 0;
}

static s32 sd_umount(const char *mp) { return do_umount(mp, false); }
static s32 sd_umount_backup(const char *mp) { return do_umount(mp, true); }

/* ---- GetMountInfo, params, icons ---- */

static s32 sd_get_mount_info(const char *mp, struct sd_mount_info *info)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!mp || !info)
        return E_PARAMETER;
    lock();
    s32 v = vm_index(mp);
    if (v < 0) {
        unlock();
        return E_NOT_MOUNTED;
    }
    u64 used = used_blocks(g_vm[v].base, g_vm[v].real[0] != 0), blocks = g_vm[v].blocks;
    unlock();
    memset(info, 0, sizeof(*info));
    info->blocks = blocks;
    info->free_blocks = blocks > used ? blocks - used : 0;
    return 0;
}

static u64 copy_str(char *dst, u64 size, const char *src, u64 max)
{
    u64 n = 0;
    while (n + 1 < size && n < max && src[n])
        dst[n] = src[n], n++;
    dst[n] = 0;
    return n;
}

static s32 sd_param_get(const char *mp, u32 type, void *buf, u64 size, u64 *got)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!mp || !buf || type > 5)
        return E_PARAMETER;
    lock();
    s32 v = vm_index(mp);
    if (v < 0) {
        unlock();
        return E_NOT_MOUNTED;
    }
    struct sd_param p = g_vm[v].param;
    unlock();
    u64 n;
    switch (type) {
    case 0:
        if (size < sizeof(p))
            return E_PARAMETER;
        memcpy(buf, &p, sizeof(p));
        n = sizeof(p);
        break;
    case 1:
    case 2:
    case 3: {
        const char *s = type == 1 ? p.title : type == 2 ? p.sub_title : p.detail;
        u64 cap = type == 3 ? sizeof(p.detail) : sizeof(p.title);
        if (!size)
            return E_PARAMETER;
        n = copy_str(buf, size, s, cap) + 1;
        break;
    }
    case 4:
        if (size < 4)
            return E_PARAMETER;
        *(u32 *)buf = p.user_param;
        n = 4;
        break;
    default:
        if (size < 8)
            return E_PARAMETER;
        *(s64 *)buf = p.mtime;
        n = 8;
        break;
    }
    if (got)
        *got = n;
    return 0;
}

static s32 sd_param_set(const char *mp, u32 type, const void *buf, u64 size)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!mp || !buf || type > 4)
        return E_PARAMETER;
    lock();
    s32 v = vm_index(mp);
    s32 rc = 0;
    if (v < 0) {
        rc = E_NOT_MOUNTED;
    } else if (g_vm[v].ro) {
        rc = E_BAD_MOUNTED;
    } else {
        struct sd_param *p = &g_vm[v].param;
        const struct sd_param *in = buf;
        switch (type) {
        case 0:
            if (size < sizeof(*p)) {
                rc = E_PARAMETER;
                break;
            }
            copy_str(p->title, sizeof(p->title), in->title, sizeof(in->title));
            copy_str(p->sub_title, sizeof(p->sub_title), in->sub_title, sizeof(in->sub_title));
            copy_str(p->detail, sizeof(p->detail), in->detail, sizeof(in->detail));
            p->user_param = in->user_param;
            break;
        case 1:
            copy_str(p->title, sizeof(p->title), buf, size);
            break;
        case 2:
            copy_str(p->sub_title, sizeof(p->sub_title), buf, size);
            break;
        case 3:
            copy_str(p->detail, sizeof(p->detail), buf, size);
            break;
        default:
            if (size < 4)
                rc = E_PARAMETER;
            else
                p->user_param = *(const u32 *)buf;
            break;
        }
        if (rc == 0) {
            p->mtime = now();
            g_vm[v].param_set = true;
        }
    }
    unlock();
    return rc;
}

static s32 sd_save_icon(const char *mp, const struct sd_icon *icon)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!mp || !icon || !icon->buf)
        return E_PARAMETER;
    u64 n = icon->data_size ? MIN(icon->buf_size, icon->data_size) : icon->buf_size;
    if (!n || n > ICON_MAX)
        return E_PARAMETER;
    lock();
    s32 v = vm_index(mp), rc = 0;
    if (v < 0) {
        rc = E_NOT_MOUNTED;
    } else if (g_vm[v].ro) {
        rc = E_BAD_MOUNTED;
    } else {
        struct vmount *vm = &g_vm[v];
        u8 *copy = page_alloc(n);
        if (!copy) {
            rc = E_OUT_OF_MEMORY;
        } else {
            memcpy(copy, icon->buf, n);
            if (vm->icon)
                page_free(vm->icon, vm->icon_size);
            vm->icon = copy, vm->icon_size = (u32)n;
        }
    }
    unlock();
    return rc;
}

/* icons/<id>.png of a save into icon; dataSize 0 when there is none */
static s32 icon_read(s32 user, u32 id, struct sd_icon *icon)
{
    struct sess s;
    char p[320];
    s32 rc = sess_begin(&s, user, false, false);
    if (rc != 0)
        return rc;
    icon_path(p, sizeof(p), s.root, id);
    s64 size = file_size(p);
    icon->data_size = size > 0 ? (u64)size : 0;
    if (size > 0 && icon->buf && icon->buf_size)
        read_at(p, icon->buf, MIN(icon->buf_size, (u64)size), 0);
    sess_end(&s);
    return size > 0 ? 0 : E_NOT_FOUND;
}

static s32 sd_load_icon(const char *mp, struct sd_icon *icon)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!mp || !icon || !icon->buf)
        return E_PARAMETER;
    lock();
    s32 v = vm_index(mp), rc;
    if (v < 0) {
        rc = E_NOT_MOUNTED;
    } else if (g_vm[v].icon) {
        memcpy(icon->buf, g_vm[v].icon, MIN(icon->buf_size, (u64)g_vm[v].icon_size));
        icon->data_size = g_vm[v].icon_size;
        rc = 0;
    } else {
        s32 idx = cat_load(g_vm[v].user) == 0 ? cat_find(g_vm[v].dir) : -1;
        rc = idx >= 0 && (g_cat[idx].flags & CAT_ICON) ? icon_read(g_vm[v].user, g_vm[v].id, icon) : E_NOT_FOUND;
    }
    unlock();
    return rc;
}

/* ---- DirNameSearch ---- */

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

/* SQL LIKE without escapes: % any run (even empty), _ one character; case blind */
static bool like(const char *pat, const char *s)
{
    const char *star = NULL, *mark = NULL;
    while (*s) {
        if (*pat == '%') {
            star = pat++, mark = s;
        } else if (*pat && (*pat == '_' || lower(*pat) == lower(*s))) {
            pat++, s++;
        } else if (star) {
            pat = star + 1, s = ++mark;
        } else {
            return false;
        }
    }
    while (*pat == '%')
        pat++;
    return !*pat;
}

static u64 free_of(const struct cat_entry *e) { return e->blocks > e->used_blocks ? e->blocks - e->used_blocks : 0; }

/* <0, 0, >0 by the search key */
static int key_cmp(const struct cat_entry *a, const struct cat_entry *b, u32 key)
{
    switch (key) {
    case 0:
        return strncmp(a->dir, b->dir, 32);
    case 1:
        return a->param.user_param < b->param.user_param ? -1 : a->param.user_param > b->param.user_param;
    case 2:
        return a->blocks < b->blocks ? -1 : a->blocks > b->blocks;
    case 3:
        return a->mtime < b->mtime ? -1 : a->mtime > b->mtime;
    default:
        return free_of(a) < free_of(b) ? -1 : free_of(a) > free_of(b);
    }
}

static s32 sd_dir_name_search(const struct sd_search_cond *c, struct sd_search_result *r)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!c || !r || (c->key > 3 && c->key != 5) || c->order > 1)
        return E_PARAMETER;
    if (c->user < 0)
        return E_INVALID_LOGIN_USER;
    bool v170 = sdk() >= SDK_1_70, v250 = sdk() >= SDK_2_50;
    char pat[33];
    pat[0] = 0;
    if (c->dir) {
        memcpy(pat, c->dir, 32);
        pat[32] = 0;
    }
    u16 hit[CAT_MAX];
    u32 n = 0;
    if (title_is_self(c->title_id)) {
        lock();
        s32 rc = kf_ready() ? cat_load(c->user) : E_INTERNAL;
        if (rc != 0) {
            logf("savedata: search '%s': the catalogue is unreadable (%x): 0 hits", pat, (u32)rc);
        } else {
            for (u32 i = 0; i < g_cath.count; i++) {
                const struct cat_entry *e = &g_cat[i];
                if (!(e->flags & CAT_EXISTS) || strncmp(e->dir, "sce_", 4) == 0 || (pat[0] && !like(pat, e->dir)))
                    continue;
                /* stable insertion by key; DESCENT flips the comparison */
                u32 j = n++;
                while (j > 0) {
                    int d = key_cmp(&g_cat[hit[j - 1]], e, c->key);
                    if (c->order ? d >= 0 : d <= 0)
                        break;
                    hit[j] = hit[j - 1];
                    j--;
                }
                hit[j] = (u16)i;
            }
        }
        u32 set = MIN(r->dirs_num, n);
        for (u32 i = 0; i < set; i++) {
            const struct cat_entry *e = &g_cat[hit[i]];
            if (r->dirs) {
                memset(r->dirs + 32 * i, 0, 32);
                memcpy(r->dirs + 32 * i, e->dir, strlen(e->dir));
            }
            if (v170 && r->params)
                r->params[i] = e->param;
            if (v250 && r->infos) {
                memset(&r->infos[i], 0, sizeof(r->infos[i]));
                r->infos[i].blocks = e->blocks;
                r->infos[i].free_blocks = free_of(e);
            }
        }
        unlock();
        r->hit_num = v170 ? n : set;
        if (v170)
            r->set_num = set;
    } else {
        r->hit_num = 0;
        if (v170)
            r->set_num = 0;
    }
    logf("savedata: search '%s' -> %u hits", pat[0] ? pat : "*", n);
    return 0;
}

/* ---- Delete ---- */

static s32 sd_delete(const struct sd_delete *d)
{
    char dir[32];
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!d || !dir_ok(d->dir, dir))
        return E_PARAMETER;
    if (d->user < 0)
        return E_INVALID_LOGIN_USER;
    if (!title_is_self(d->title_id))
        return E_NOT_FOUND;
    lock();
    s32 rc = kf_ready() ? 0 : E_INTERNAL;
    if (!rc && vm_busy(d->user, dir))
        rc = E_BUSY;
    if (!rc && (rc = cat_load(d->user)) != 0)
        rc = E_INTERNAL;
    s32 idx = rc ? -1 : cat_find(dir);
    if (!rc && idx < 0)
        rc = E_NOT_FOUND;
    if (!rc) {
        struct cat_entry *e = &g_cat[idx];
        if (cont_backend()) {
            char name[32];
            cont_name(name, 'D', e->id);
            rc = real_delete(d->user, name);
            if (rc == E_NOT_FOUND)
                rc = 0; /* already gone */
            if (rc)
                logf("savedata: deleting %s (%s) -> %x", name, dir, (u32)rc);
        } else {
            char ud[256], p[320];
            user_dir(ud, sizeof(ud), d->user);
            fmt(p, sizeof(p), "%s/d/%s", ud, dir);
            remove_tree(p);
            fmt(p, sizeof(p), "%s/d/%s.bak", ud, dir);
            remove_tree(p);
        }
        if (!rc) {
            struct icon_op op = {e->id, NULL, 0, true};
            bool icon = e->flags & CAT_ICON;
            cat_remove((u32)idx);
            s32 crc = cat_commit(d->user, icon ? &op : NULL);
            if (crc != 0)
                logf("savedata: %s is deleted, but the catalogue still lists it (%x)", dir, (u32)crc);
            logf("savedata: deleted %s", dir);
        } else {
            rc = rc == E_BUSY ? E_BUSY : E_INTERNAL;
        }
    }
    unlock();
    return rc;
}

/* ---- backups and events (phase 1: no backups are kept) ---- */

static s32 backup_target(s32 user, const char *title_id, const char *dirp, char dir[32])
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!dir_ok(dirp, dir))
        return E_PARAMETER;
    if (user < 0)
        return E_INVALID_LOGIN_USER;
    if (!title_is_self(title_id))
        return E_NOT_FOUND;
    if (vm_busy(user, dir))
        return E_BUSY;
    if (!kf_ready() || cat_load(user) != 0)
        return E_INTERNAL;
    return cat_find(dir) < 0 ? E_NOT_FOUND : 0;
}

static s32 sd_backup(const struct sd_backup *b)
{
    char dir[32];
    if (!b)
        return g_init ? E_PARAMETER : E_NOT_INITIALIZED;
    lock();
    s32 rc = backup_target(b->user, b->title_id, b->dir, dir);
    if (rc == 0)
        event_add(2, b->user, dir); /* BACKUP: reported done, none is kept */
    unlock();
    return rc;
}

static s32 sd_check_backup(const struct sd_check_backup *c)
{
    char dir[32];
    if (!c)
        return g_init ? E_PARAMETER : E_NOT_INITIALIZED;
    lock();
    s32 rc = backup_target(c->user, c->title_id, c->dir, dir);
    unlock();
    return rc == 0 ? E_NOT_FOUND : rc; /* no backup: the title deletes and recreates */
}

static s32 sd_restore_backup(const struct sd_restore_backup *rb)
{
    char dir[32];
    if (!rb)
        return g_init ? E_PARAMETER : E_NOT_INITIALIZED;
    lock();
    s32 rc = backup_target(rb->user, rb->title_id, rb->dir, dir);
    unlock();
    return rc == 0 ? E_NOT_FOUND : rc;
}

static s32 sd_get_event_result(const void *param, struct sd_event *e)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!e)
        return E_PARAMETER;
    lock();
    s32 rc = E_NOT_FOUND;
    if (g_ev_count) {
        *e = g_ev[g_ev_head];
        g_ev_head = (g_ev_head + 1) % NEVENTS;
        g_ev_count--;
        rc = 0;
    }
    unlock();
    return rc;
}

static s32 sd_transferring_mount(const struct sd_transfer_mount *m, struct sd_mount_result *r)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!m || !m->title_id || !m->dir || !r)
        return E_PARAMETER;
    logf("savedata: transferring mount of %.10s:%.32s: not found", m->title_id, m->dir);
    return E_NOT_FOUND;
}

static s32 sd_get_progress(u32 *progress)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!progress)
        return E_PARAMETER;
    *progress = 0x3F800000; /* 1.0f: everything here is synchronous */
    return 0;
}

static s32 sd_clear_progress(void) { return g_init ? 0 : E_NOT_INITIALIZED; }

/* ---- save data memory ---- */

static u64 mem_blocks(u64 size)
{
    u64 b = (size + BLOCK - 1) / BLOCK;
    return MIN(BLOCKS_MAX, MAX(BLOCKS_MIN, b + MAX(4ull, b / 32) + 2));
}

/* the slot's files: <prefix>SAVE (or its M container), or a dev directory */
static s32 mem_begin(struct sess *s, const struct memslot *m, u32 slot, bool rw)
{
    if (cont_backend()) {
        if (m->in_m) {
            char name[32];
            cont_name(name, 'M', slot);
            return sess_mount(s, m->user, name, rw, rw ? mem_blocks(m->size) : 0);
        }
        return sess_begin(s, m->user, rw, rw);
    }
    if (m->legacy) {
        memset(s, 0, sizeof(*s));
        s->user = m->user, s->rw = rw;
        strlcpy(s->root, save_root(), sizeof(s->root));
        return 0;
    }
    return sess_begin(s, m->user, rw, rw);
}

static void mem_path(char *out, size_t size, const struct sess *s, u32 slot, const char *what)
{
    if (strcmp(what, "png") == 0)
        fmt(out, size, "%s/icons/m%u.png", s->root, slot);
    else
        fmt(out, size, "%s/sdmemory_%u.%s", s->root, slot, what);
}

static void mem_icon_write(const struct sess *s, u32 slot, const void *data, u64 n)
{
    char p[320];
    fmt(p, sizeof(p), "%s/icons", s->root);
    kf.mkdir(p, 0777);
    mem_path(p, sizeof(p), s, slot, "png");
    s32 rc = n && n <= ICON_MAX ? write_file(p, data, n) : E_PARAMETER;
    if (rc != 0)
        logf("savedata: memory slot %u icon (%lu bytes) -> %x", slot, n, (u32)rc);
}

/* does memory of size fit beside what <prefix>SAVE holds already? */
static bool fits_in_save(const struct sess *s, u64 size)
{
    u64 total = g_cath.save_blocks ? g_cath.save_blocks : SAVE_BLOCKS;
    u64 need = (size + 0x10000 + BLOCK - 1) / BLOCK, margin = MAX(4ull, total / 32);
    return used_blocks(s->root, true) + need + margin <= total;
}

/* the backing file grown (zero-filled) to size, created if missing */
static s32 mem_extend(const char *path, u64 size)
{
    int fd = kf.open(path, O_RDWR | O_CREAT, 0666);
    if (fd < 0)
        return fd;
    s32 rc = size ? write_all(fd, "", 1, (s64)size - 1) : 0;
    kf.close(fd);
    return rc;
}

static s32 mem_setup(s32 user, u32 slot, u64 size, const struct sd_param *init_param, const struct sd_icon *init_icon,
                     u64 *existed)
{
    if (g_ms[slot].set_up)
        return E_BUSY; /* "save data memory setup has already done" */
    struct memslot ms = {false, false, false, user, size};
    struct sess s;
    char p[320];
    s32 rc;
    if (cont_backend()) {
        /* where it lives: the catalogue header says if it went to an M container */
        if ((rc = sess_begin(&s, user, true, true)) != 0)
            return real_error(rc) == E_NO_SPACE_FS ? E_NO_SPACE_FS : E_INTERNAL;
        rc = g_cat_user == user ? 0 : cat_read_in(&s);
        ms.in_m = rc == 0 && g_cath.mem_container[slot];
        mem_path(p, sizeof(p), &s, slot, "bin");
        if (rc == 0 && !ms.in_m && file_size(p) < 0 && !fits_in_save(&s, size)) {
            ms.in_m = true;
            g_cath.mem_container[slot] = 1;
            rc = cat_write_in(&s);
            logf("savedata: memory slot %u (%lu bytes) does not fit in %s: it goes to its own container", slot, size,
                 gfs_save_name());
        }
        if (ms.in_m || rc != 0) {
            s32 rc2 = sess_end(&s);
            if (rc || rc2)
                return E_INTERNAL;
            if ((rc = mem_begin(&s, &ms, slot, true)) != 0)
                return real_error(rc) == E_NO_SPACE_FS ? E_NO_SPACE_FS : E_INTERNAL;
        }
    } else {
        /* per-user files, else the save root's own (written before per-user directories) */
        user_dir(p, sizeof(p), user);
        fmt(p + strlen(p), sizeof(p) - strlen(p), "/sdmemory_%u.bin", slot);
        if (file_size(p) < 0 && user == initial_user()) {
            fmt(p, sizeof(p), "%s/sdmemory_%u.bin", save_root(), slot);
            ms.legacy = file_size(p) >= 0;
        }
        mem_begin(&s, &ms, slot, true);
    }
    mem_path(p, sizeof(p), &s, slot, "bin");
    s64 have = file_size(p);
    rc = have < 0 || (u64)have < size ? mem_extend(p, size) : 0;
    if (rc == 0 && have < 0) {
        struct sd_param prm;
        if (init_param)
            prm = *init_param;
        else
            param_default(&prm);
        prm.mtime = now();
        mem_path(p, sizeof(p), &s, slot, "param");
        rc = write_file(p, &prm, sizeof(prm));
        if (rc == 0 && init_icon && init_icon->buf) {
            mem_icon_write(&s, slot, init_icon->buf,
                           init_icon->data_size ? MIN(init_icon->buf_size, init_icon->data_size) : init_icon->buf_size);
        } else if (rc == 0) {
            u32 n;
            u8 *png = app_file("sce_sys/save_data.png", ICON_MAX, &n);
            if (png) {
                mem_icon_write(&s, slot, png, n);
                page_free(png, n);
            }
        }
    }
    s32 rc2 = sess_end(&s);
    logf("savedata: memory slot %u -> %s/sdmemory_%u.bin (%ld bytes existed, %lx requested)%s", slot,
         ms.in_m ? "M container" : ms.legacy ? save_root() : cont_backend() ? gfs_save_name() : "user dir", slot,
         have, size, rc || rc2 ? ": FAILED" : "");
    if (rc != 0)
        return rc == K_ENOSPC ? E_NO_SPACE_FS : E_INTERNAL;
    if (rc2 != 0)
        return E_INTERNAL;
    ms.set_up = true;
    g_ms[slot] = ms;
    if (existed)
        *existed = have < 0 ? 0 : (u64)have;
    return 0;
}

static s32 mem_get(u32 slot, struct sd_mem_data *data, struct sd_param *param, struct sd_icon *icon)
{
    const struct memslot *m = &g_ms[slot];
    if (!m->set_up)
        return E_MEMORY_NOT_READY;
    if (data && data->buf && data->offset < 0)
        return E_PARAMETER;
    struct sess s;
    char p[320];
    s32 rc = mem_begin(&s, m, slot, false);
    if (rc != 0)
        return E_INTERNAL;
    if (data && data->buf) {
        u64 off = (u64)data->offset, n = data->buf_size;
        u64 in = off < m->size ? MIN(n, m->size - off) : 0;
        mem_path(p, sizeof(p), &s, slot, "bin");
        s64 got = in ? read_at(p, data->buf, in, (s64)off) : 0;
        if (got < 0) {
            logf("savedata: memory slot %u: reading %s -> %lx", slot, p, (u64)got);
            got = 0;
        }
        memset((u8 *)data->buf + got, 0, n - (u64)got); /* past the memory's size: zeros */
    }
    if (param) {
        mem_path(p, sizeof(p), &s, slot, "param");
        if (read_at(p, param, sizeof(*param), 0) != (s64)sizeof(*param))
            memset(param, 0, sizeof(*param));
    }
    if (icon) {
        mem_path(p, sizeof(p), &s, slot, "png");
        s64 size = file_size(p);
        icon->data_size = size > 0 ? (u64)size : 0;
        if (size > 0 && icon->buf && icon->buf_size)
            read_at(p, icon->buf, MIN(icon->buf_size, (u64)size), 0);
    }
    sess_end(&s);
    return 0;
}

static s32 mem_set(u32 slot, const struct sd_mem_data *data, u32 num, const struct sd_param *param,
                   const struct sd_icon *icon)
{
    const struct memslot *m = &g_ms[slot];
    if (!m->set_up)
        return E_MEMORY_NOT_READY;
    for (u32 i = 0; data && i < num; i++)
        if (data[i].buf && (data[i].offset < 0 || data[i].buf_size > m->size ||
                            (u64)data[i].offset > m->size - data[i].buf_size))
            return E_PARAMETER;
    struct sess s;
    char p[320];
    s32 rc = mem_begin(&s, m, slot, true);
    if (rc != 0)
        return real_error(rc) == E_NO_SPACE_FS ? E_NO_SPACE_FS : E_INTERNAL;
    if (data) {
        mem_path(p, sizeof(p), &s, slot, "bin");
        int fd = kf.open(p, O_RDWR | O_CREAT, 0666);
        rc = fd < 0 ? fd : 0;
        for (u32 i = 0; rc == 0 && i < num; i++)
            if (data[i].buf)
                rc = write_all(fd, data[i].buf, data[i].buf_size, data[i].offset);
        if (fd >= 0)
            kf.close(fd);
    }
    if (rc == 0 && param) {
        struct sd_param prm = *param;
        prm.mtime = now();
        mem_path(p, sizeof(p), &s, slot, "param");
        rc = write_file(p, &prm, sizeof(prm));
    }
    if (rc == 0 && icon && icon->buf)
        mem_icon_write(&s, slot, icon->buf, icon->data_size ? MIN(icon->buf_size, icon->data_size) : icon->buf_size);
    s32 rc2 = sess_end(&s); /* the commit: durable from here */
    if (rc != 0 || rc2 != 0)
        logf("savedata: memory slot %u write (%u blocks) -> %x, commit -> %x", slot, num, (u32)rc, (u32)rc2);
    if (rc != 0)
        return rc == K_ENOSPC ? E_NO_SPACE_FS : E_INTERNAL;
    return rc2 ? E_INTERNAL : 0;
}

static u32 mem_slot_of(u32 slot) { return sdk() >= SDK_5_00 ? slot : 0; }

static s32 sd_setup_memory2(const struct sd_mem_setup2 *s, struct sd_mem_setup_result *res)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!s || !s->memory_size)
        return E_PARAMETER;
    if (s->user < 0)
        return E_INVALID_LOGIN_USER;
    u32 slot = mem_slot_of(s->slot);
    if (slot >= MSLOTS)
        return E_PARAMETER;
    bool v450 = sdk() >= SDK_4_50;
    u64 existed = 0;
    lock();
    s32 rc = kf_ready() ? mem_setup(s->user, slot, s->memory_size, v450 ? s->init_param : NULL,
                                    v450 ? s->init_icon : NULL, &existed)
                        : E_INTERNAL;
    unlock();
    if (rc == 0 && res) {
        memset(res, 0, sizeof(*res));
        res->existed = existed;
    }
    return rc;
}

static s32 sd_get_memory2(struct sd_mem_get2 *g)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!g)
        return E_PARAMETER;
    u32 slot = mem_slot_of(g->slot);
    if (slot >= MSLOTS)
        return E_PARAMETER;
    lock();
    s32 rc = mem_get(slot, g->data, g->param, g->icon);
    unlock();
    return rc;
}

static s32 sd_set_memory2(const struct sd_mem_set2 *s)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!s)
        return E_PARAMETER;
    bool v500 = sdk() >= SDK_5_00;
    u32 slot = v500 ? s->slot : 0, num = v500 && s->data_num ? s->data_num : 1;
    if (slot >= MSLOTS)
        return E_PARAMETER;
    lock();
    s32 rc = mem_set(slot, s->data, num, s->param, s->icon);
    unlock();
    return rc;
}

static s32 sd_sync_memory(const struct sd_mem_sync *s)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!s)
        return E_PARAMETER;
    u32 slot = mem_slot_of(s->slot);
    if (slot >= MSLOTS)
        return E_PARAMETER;
    lock();
    s32 rc = E_MEMORY_NOT_READY;
    if (g_ms[slot].set_up) { /* every write was committed already */
        char dir[32];
        if (slot)
            fmt(dir, sizeof(dir), "sce_sdmemory%u", slot);
        else
            strlcpy(dir, "sce_sdmemory", sizeof(dir));
        event_add(3, s->user, dir);
        rc = 0;
    }
    unlock();
    return rc;
}

/* the SDK 1.x-4.x calls: slot 0 */
static s32 sd_setup_memory(s32 user, u64 size, const struct sd_param *param)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!size)
        return E_PARAMETER;
    if (user < 0)
        return E_INVALID_LOGIN_USER;
    lock();
    s32 rc = kf_ready() ? mem_setup(user, 0, size, NULL, NULL, NULL) : E_INTERNAL;
    if (rc == 0 && param)
        rc = mem_set(0, NULL, 0, param, NULL);
    unlock();
    return rc;
}

static s32 sd_get_memory(s32 user, void *buf, u64 size, s64 off)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!buf)
        return E_PARAMETER;
    struct sd_mem_data d;
    memset(&d, 0, sizeof(d));
    d.buf = buf, d.buf_size = size, d.offset = off;
    lock();
    s32 rc = mem_get(0, &d, NULL, NULL);
    unlock();
    return rc;
}

static s32 sd_set_memory(s32 user, const void *buf, u64 size, s64 off)
{
    if (!g_init)
        return E_NOT_INITIALIZED;
    if (!buf)
        return E_PARAMETER;
    struct sd_mem_data d;
    memset(&d, 0, sizeof(d));
    d.buf = (void *)buf, d.buf_size = size, d.offset = off;
    lock();
    s32 rc = mem_set(0, &d, 1, NULL, NULL);
    unlock();
    return rc;
}

/* ---- fixed answers ---- */

static s32 sd_zero(void) { return 0; }
static s32 sd_not_found(void) { return E_NOT_FOUND; }

/* ---- the title store (title_store.h) ---- */

static bool store_name_ok(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (!n || n > TITLE_STORE_NAME_MAX || name[0] == '.')
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-'))
            return false;
    }
    return true;
}

/* <prefix>SAVE of the initial user, or the dev save root (not per user) */
static s32 store_begin(struct sess *s, bool rw, bool create, const char *name, char *path, size_t size)
{
    s32 rc;
    if (!kf_ready())
        return TITLE_STORE_E_INTERNAL;
    if (cont_backend()) {
        rc = sess_begin(s, initial_user(), rw, create);
        if (rc != 0)
            return rc == E_NOT_FOUND ? TITLE_STORE_E_NOT_FOUND : TITLE_STORE_E_INTERNAL;
    } else {
        memset(s, 0, sizeof(*s));
        s->user = initial_user(), s->rw = rw;
        strlcpy(s->root, save_root(), sizeof(s->root));
    }
    char dir[320];
    fmt(dir, sizeof(dir), "%s/store", s->root);
    if (rw)
        kf.mkdir(dir, 0777);
    fmt(path, size, "%s/%s", dir, name);
    return 0;
}

static s32 store_end(struct sess *s) { return sess_end(s) ? TITLE_STORE_E_INTERNAL : 0; }

s64 title_store_read(const char *name, void *buf, u64 len)
{
    if (!store_name_ok(name) || (!buf && len))
        return TITLE_STORE_E_INVALID;
    struct sess s;
    char p[320];
    lock();
    s64 rc = store_begin(&s, false, false, name, p, sizeof(p));
    if (rc == 0) {
        rc = file_size(p) < 0 ? TITLE_STORE_E_NOT_FOUND : len ? read_at(p, buf, len, 0) : 0;
        if (rc < 0 && rc != TITLE_STORE_E_NOT_FOUND)
            rc = TITLE_STORE_E_INTERNAL;
        store_end(&s);
    }
    unlock();
    return rc;
}

s64 title_store_size(const char *name)
{
    if (!store_name_ok(name))
        return TITLE_STORE_E_INVALID;
    struct sess s;
    char p[320];
    lock();
    s64 rc = store_begin(&s, false, false, name, p, sizeof(p));
    if (rc == 0) {
        rc = file_size(p);
        if (rc < 0)
            rc = TITLE_STORE_E_NOT_FOUND;
        store_end(&s);
    }
    unlock();
    return rc;
}

s32 title_store_write(const char *name, const void *buf, u64 len)
{
    if (!store_name_ok(name) || (!buf && len) || len > TITLE_STORE_MAX)
        return TITLE_STORE_E_INVALID;
    struct sess s;
    char p[320];
    lock();
    s32 rc = store_begin(&s, true, true, name, p, sizeof(p));
    if (rc == 0) {
        rc = write_file(p, len ? buf : "", len);
        if (rc != 0)
            logf("savedata: title store %s (%lu bytes) -> %x", name, len, (u32)rc);
        if (rc != 0)
            rc = rc == K_ENOSPC ? TITLE_STORE_E_NO_SPACE : TITLE_STORE_E_INTERNAL;
        s32 rc2 = store_end(&s);
        if (rc == 0)
            rc = rc2;
    }
    unlock();
    return rc;
}

s32 title_store_remove(const char *name)
{
    if (!store_name_ok(name))
        return TITLE_STORE_E_INVALID;
    struct sess s;
    char p[320];
    lock();
    s32 rc = store_begin(&s, true, false, name, p, sizeof(p));
    if (rc == TITLE_STORE_E_NOT_FOUND) {
        rc = 0; /* no container: no record */
    } else if (rc == 0) {
        kf.unlink(p);
        rc = store_end(&s);
    }
    unlock();
    return rc;
}

/* ---- the save data dialog's view (savedata_hooks.h) ---- */

s32 sd_list(s32 user, struct sd_save_info *out, u32 max)
{
    if (user < 0)
        user = initial_user();
    lock();
    s32 rc = kf_ready() ? cat_load(user) : E_INTERNAL;
    u32 n = 0;
    for (u32 i = 0; rc == 0 && i < g_cath.count; i++) {
        const struct cat_entry *e = &g_cat[i];
        if (!(e->flags & CAT_EXISTS) || strncmp(e->dir, "sce_", 4) == 0)
            continue;
        if (out && n < max) {
            struct sd_save_info *o = &out[n];
            memset(o, 0, sizeof(*o));
            memcpy(o->dir, e->dir, 32);
            o->blocks = e->blocks;
            o->free_blocks = free_of(e);
            o->mtime = e->mtime;
            o->user_param = e->param.user_param;
            o->has_icon = (e->flags & CAT_ICON) != 0;
        }
        n++;
    }
    unlock();
    return rc ? rc : (s32)n;
}

s32 savedata_query(s32 user, const char *dirp, struct sd_param *param, void *icon, void *info)
{
    char dir[32];
    if (!dir_ok(dirp, dir))
        return E_PARAMETER;
    if (user < 0)
        user = initial_user();
    lock();
    s32 rc = kf_ready() ? cat_load(user) : E_INTERNAL;
    s32 idx = rc ? -1 : cat_find(dir);
    if (rc == 0 && idx < 0)
        rc = E_NOT_FOUND;
    if (rc == 0) {
        const struct cat_entry *e = &g_cat[idx];
        if (param) {
            *param = e->param;
            param->mtime = e->mtime;
        }
        if (info) {
            struct sd_mount_info *si = info;
            memset(si, 0, sizeof(*si));
            si->blocks = e->blocks;
            si->free_blocks = free_of(e);
        }
        if (icon) {
            struct sd_icon *ic = icon;
            ic->data_size = 0;
            if (e->flags & CAT_ICON)
                icon_read(user, e->id, ic);
        }
    }
    unlock();
    return rc;
}

bool sd_exists(const char *dir) { return savedata_query(-1, dir, NULL, NULL, NULL) == 0; }

s32 sd_get_param(const char *dir, struct sd_param *out)
{
    return out ? savedata_query(-1, dir, out, NULL, NULL) : E_PARAMETER;
}

s64 sd_mtime(const char *dir)
{
    struct sd_param p;
    return savedata_query(-1, dir, &p, NULL, NULL) == 0 ? p.mtime : -1;
}

/* ---- registration ---- */

static int sd_init(void)
{
    if (remap_hook_add(sd_remap) != 0) {
        logf("savedata: no room for the /savedata path hook: the core's save data is used instead");
        return -1;
    }
    return 0;
}

#define SD "libSceSaveData"
static const struct ext_hle_ent k_hle[] = {
    {SD, "sceSaveDataInitialize", sd_initialize, NULL},
    {SD, "sceSaveDataInitialize2", sd_initialize, NULL},
    {SD, "sceSaveDataInitialize3", sd_initialize, NULL},
    {SD, "sceSaveDataTerminate", sd_terminate, NULL},
    {SD, "sceSaveDataMount", sd_mount1, NULL},
    {SD, "sceSaveDataMount2", sd_mount2, NULL},
    {SD, "sceSaveDataUmount", sd_umount, NULL},
    {SD, "sceSaveDataUmountWithBackup", sd_umount_backup, NULL},
    {SD, "sceSaveDataGetMountInfo", sd_get_mount_info, NULL},
    {SD, "sceSaveDataGetParam", sd_param_get, NULL},
    {SD, "sceSaveDataSetParam", sd_param_set, NULL},
    {SD, "sceSaveDataSaveIcon", sd_save_icon, NULL},
    {SD, "sceSaveDataLoadIcon", sd_load_icon, NULL},
    {SD, "sceSaveDataDirNameSearch", sd_dir_name_search, NULL},
    {SD, "sceSaveDataDelete", sd_delete, NULL},
    {SD, "sceSaveDataTransferringMount", sd_transferring_mount, NULL},
    {SD, "sceSaveDataSetupSaveDataMemory2", sd_setup_memory2, NULL},
    {SD, "sceSaveDataGetSaveDataMemory2", sd_get_memory2, NULL},
    {SD, "sceSaveDataSetSaveDataMemory2", sd_set_memory2, NULL},
    {SD, "sceSaveDataSyncSaveDataMemory", sd_sync_memory, NULL},
    {SD, "sceSaveDataSetupSaveDataMemory", sd_setup_memory, NULL},
    {SD, "sceSaveDataGetSaveDataMemory", sd_get_memory, NULL},
    {SD, "sceSaveDataSetSaveDataMemory", sd_set_memory, NULL},
    {SD, "sceSaveDataBackup", sd_backup, NULL},
    {SD, "sceSaveDataCheckBackupData", sd_check_backup, NULL},
    {SD, "sceSaveDataRestoreBackupData", sd_restore_backup, NULL},
    {SD, "sceSaveDataGetEventResult", sd_get_event_result, NULL},
    {SD, "sceSaveDataGetProgress", sd_get_progress, NULL},
    {SD, "sceSaveDataClearProgress", sd_clear_progress, NULL},
    {SD, "sceSaveDataGetEventInfo", sd_not_found, NULL},
    /* setters, diagnostics and debug calls: accepted, nothing happens */
    {SD, "sceSaveDataAbort", sd_zero, NULL},
    {SD, "sceSaveDataShutdownStart", sd_zero, NULL},
    {SD, "sceSaveDataRegisterEventCallback", sd_zero, NULL},
    {SD, "sceSaveDataUnregisterEventCallback", sd_zero, NULL},
    {SD, "sceSaveDataSetEventInfo", sd_zero, NULL},
    {SD, "sceSaveDataSetSaveDataLibraryUser", sd_zero, NULL},
    {SD, "sceSaveDataGetClientThreadPriority", sd_zero, NULL},
    {SD, "sceSaveDataCheckIpmiIfSize", sd_zero, NULL},
    {SD, "sceSaveDataDebug", sd_zero, NULL},
    {SD, "sceSaveDataDebugCleanMount", sd_zero, NULL},
    {SD, "sceSaveDataDebugCompiledSdkVersion", sd_zero, NULL},
    {SD, "sceSaveDataDebugCreateSaveDataRoot", sd_zero, NULL},
    {SD, "sceSaveDataDebugGetThreadId", sd_zero, NULL},
    {SD, "sceSaveDataDebugRemoveSaveDataRoot", sd_zero, NULL},
    {SD, "sceSaveDataDebugTarget", sd_zero, NULL},
    /* everything else (cloud, PSN, internal, DeleteUser, Mount5, the save
     * root's paths, unknown NIDs) gets the policy's INTERNAL stub */
};

static const struct ext_lib k_libs[] = {
    {SD, NULL, EXT_LIB_NO_HOST, E_INTERNAL},
};

#pragma clang attribute pop

#ifndef SAVEDATA_HOST_TEST /* tests/ext_host/savedata_host.c compiles this file into a test program */
EXT_FAMILY_LIBS("savedata", k_hle, k_libs, sd_init);
#endif
