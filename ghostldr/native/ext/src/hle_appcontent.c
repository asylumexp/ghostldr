/*
 * libSceAppContent (module libSceAppContentUtil, file libSceAppContent.sprx)
 * and its secondary libraries, plus the title's identity (sys_title.h) and
 * its /temp0 and /download0 paths.
 *
 * Everything here is answered for the loaded title, never by SWRR's own
 * AppContent, which answers for CUSA03474: param.sfo values, add-ons (none),
 * entitlements (none), temporary data in /temp0/ghostldr/<TITLE_ID> and
 * download data in .../download0 under it (SWRR's sandbox has no
 * /download0; /temp0 is wiped at reboot, which titles allow for both).
 * Spec: research/hle-specs/system-appcontent.md section 6.
 */
#include "sys_title.h"
#include "savedata_hooks.h"

#define AC_PARAMETER ((s32)0x80D90002)
#define AC_NOT_FOUND ((s32)0x80D90005)
#define AC_NO_ENTITLEMENT ((s32)0x80D90007)
#define AC_NOT_SUPPORTED ((s32)0x80D90009)
#define LABEL_MAX 7 /* service labels 0-7 */

/* ---- the title's identity ---- */

/* Titles whose dumps have no sce_sys/param.sfo.  Interim: the per-title
 * profiles (ghostldr/data/titles/<TITLE>.json, "app_params") replace this
 * once they reach the extension.  Values are param.sfo "KEY=value" pairs. */
static const struct {
    const char *title, *params;
} k_fallback[] = {
    {"CUSA11456", "CONTENT_ID=UP9000-CUSA11456_00-GHOSTSHIP0000000\0"
                  "TITLE=Ghost of Tsushima\0"
                  "APP_VER=01.00\0"
                  "VERSION=02.18\0"
                  "USER_DEFINED_PARAM_1=11456\0"},
};

#define SFO_MAX 0x10000
struct sfo_ent {
    u16 key_off, fmt;
    u32 len, max_len, data_off;
};

static volatile int g_title_lock;
static volatile bool g_title_loaded;
static u8 *g_sfo; /* the title's param.sfo, checked; NULL when it has none */
static u32 g_sfo_n, g_sfo_keys, g_sfo_data;
static const char *g_fallback;
static char g_tid[16];

static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* every key and value inside the file, strings NUL-terminated (we own the copy) */
static bool sfo_check(u8 *s, u32 size)
{
    if (size < 0x14 || memcmp(s, "\0PSF", 4) != 0)
        return false;
    u32 kt = rd32(s + 8), dt = rd32(s + 12), n = rd32(s + 16);
    if (n > 1024 || 0x14 + 16 * n > size || kt > size || dt > size)
        return false;
    for (u32 i = 0; i < n; i++) {
        struct sfo_ent *e = (struct sfo_ent *)(s + 0x14 + 16 * i);
        u32 k = kt + e->key_off, d = dt + e->data_off;
        if (k >= size || d > size || e->max_len > size - d || e->len > e->max_len)
            return false;
        u32 j = k;
        while (j < size && s[j])
            j++;
        if (j == size)
            return false;
        if ((e->fmt == 0x0004 || e->fmt == 0x0204) && e->max_len)
            s[d + (e->len && e->len < e->max_len ? e->len : e->max_len - 1)] = 0;
    }
    g_sfo_keys = kt, g_sfo_data = dt, g_sfo_n = n;
    return true;
}

static void sfo_load(void)
{
    char buf[256];
    int fd = vfs_open(remap_path("/app0/sce_sys/param.sfo", buf, sizeof(buf)), O_RDONLY, 0);
    if (fd < 0)
        return;
    s64 size = vfs_lseek(fd, 0, 2);
    u8 *s = size > 0 && size <= SFO_MAX ? arena_alloc((u64)size) : NULL;
    if (s && vfs_lseek(fd, 0, 0) == 0 && vfs_read(fd, s, (u64)size) == size && sfo_check(s, (u32)size))
        g_sfo = s;
    else
        logf("ext: the title's param.sfo is unreadable (%ld bytes)", size);
    vfs_close(fd);
}

static const struct sfo_ent *sfo_find(const char *key)
{
    for (u32 i = 0; g_sfo && i < g_sfo_n; i++) {
        const struct sfo_ent *e = (const struct sfo_ent *)(g_sfo + 0x14 + 16 * i);
        if (strcmp((const char *)g_sfo + g_sfo_keys + e->key_off, key) == 0)
            return e;
    }
    return NULL;
}

static const char *fallback_find(const char *key)
{
    size_t n = strlen(key);
    for (const char *p = g_fallback; p && *p; p += strlen(p) + 1)
        if (strncmp(p, key, n) == 0 && p[n] == '=')
            return p + n + 1;
    return NULL;
}

static void title_load(void)
{
    if (__atomic_load_n(&g_title_loaded, __ATOMIC_ACQUIRE))
        return;
    spin_lock(&g_title_lock);
    if (!g_title_loaded) {
        sfo_load();
        const struct sfo_ent *e = sfo_find("TITLE_ID");
        const char *t = e && e->fmt != 0x0404 && e->max_len ? (const char *)g_sfo + g_sfo_data + e->data_off : NULL;
        if (!t || !*t)
            t = ext_title_id ? ext_title_id() : "";
        strlcpy(g_tid, *t ? t : "untitled", sizeof(g_tid));
        for (u32 i = 0; i < ARRAY_SIZE(k_fallback); i++)
            if (strcmp(k_fallback[i].title, g_tid) == 0)
                g_fallback = k_fallback[i].params;
        logf("ext: title %s: param.sfo %s, built-in values %s", g_tid, g_sfo ? "read" : "missing",
             g_fallback ? "used where param.sfo lacks a key" : "none");
        __atomic_store_n(&g_title_loaded, true, __ATOMIC_RELEASE);
    }
    spin_unlock(&g_title_lock);
}

const char *title_ident(void)
{
    title_load();
    return g_tid;
}

const char *title_param_str(const char *key)
{
    title_load();
    const struct sfo_ent *e = sfo_find(key);
    if (e && e->fmt != 0x0404)
        return e->max_len ? (const char *)g_sfo + g_sfo_data + e->data_off : "";
    return e ? NULL : fallback_find(key);
}

bool title_param_int(const char *key, s32 *out)
{
    title_load();
    const struct sfo_ent *e = sfo_find(key);
    if (e && e->fmt == 0x0404 && e->len == 4) {
        *out = (s32)rd32(g_sfo + g_sfo_data + e->data_off);
        return true;
    }
    const char *v = e ? NULL : fallback_find(key);
    if (!v || *v < '0' || *v > '9')
        return false;
    s32 n = 0;
    while (*v >= '0' && *v <= '9')
        n = n * 10 + (*v++ - '0');
    *out = n;
    return true;
}

/* ---- /temp0/ghostldr/<TITLE_ID> ---- */

#define O_DIRECTORY 0x20000
#define DT_DIR 4
#define S_IFMT 0xF000
#define S_IFDIR 0x4000
#define WALK_DENTS 0x10000          /* getdents wants at least the file system's block size */
#define WALK_POOL 0xE000            /* directory paths found */
#define WALK_DIRS 0x1000

static struct {
    int (*open)(const char *, int, int);
    int (*close)(int);
    int (*getdents)(int, void *, int);
    int (*stat)(const char *, void *);
    int (*unlink)(const char *);
    int (*rmdir)(const char *);
    int (*mkdir)(const char *, int);
} kf;

/* the real libkernel calls: these paths are already the host's */
static bool kfs(void)
{
    if (!kf.mkdir) {
        kf.open = host_real("sceKernelOpen");
        kf.close = host_real("sceKernelClose");
        kf.getdents = host_real("sceKernelGetdents");
        kf.stat = host_real("sceKernelStat");
        kf.unlink = host_real("sceKernelUnlink");
        kf.rmdir = host_real("sceKernelRmdir");
        kf.mkdir = host_real("sceKernelMkdir");
    }
    return kf.open && kf.close && kf.getdents && kf.stat && kf.unlink && kf.rmdir && kf.mkdir;
}

/* "/temp0/ghostldr/<TITLE_ID>" + sub */
static void title_dir(char *out, size_t size, const char *sub)
{
    fmt(out, size, "/temp0/ghostldr/%s%s", title_ident(), sub);
}

/* mkdir -p for title_dir(sub): the parents are /temp0/ghostldr/... */
static void title_dir_make(const char *sub)
{
    char dir[96];
    title_dir(dir, sizeof(dir), sub);
    if (!kfs())
        return;
    for (char *p = dir + 7; *p; p++)
        if (*p == '/') {
            *p = 0;
            kf.mkdir(dir, 0777);
            *p = '/';
        }
    kf.mkdir(dir, 0777);
}

/* Every file below root: removed (the directories below it too; root
 * stays), or their sizes added up.  The bytes seen, or -1 if root is
 * unreadable.  Breadth first with a list of directories, so the stack
 * stays small; a removal deletes the directories in reverse order. */
static s64 tree_walk(const char *root, bool remove)
{
    if (!kfs())
        return -1;
    u8 *work = page_alloc(WALK_DENTS + WALK_POOL + 2 * WALK_DIRS);
    if (!work)
        return -1;
    u8 *dents = work;
    char *pool = (char *)work + WALK_DENTS;
    u16 *dirs = (u16 *)(work + WALK_DENTS + WALK_POOL);
    u32 ndirs = 1, used = (u32)strlen(root) + 1, lost = 0;
    s64 bytes = 0;
    strlcpy(pool, root, WALK_POOL);
    dirs[0] = 0;
    for (u32 i = 0; i < ndirs; i++) {
        const char *dir = pool + dirs[i];
        int fd = kf.open(dir, O_RDONLY | O_DIRECTORY, 0);
        if (fd < 0) {
            if (i == 0)
                bytes = -1;
            continue;
        }
        int n;
        while ((n = kf.getdents(fd, dents, WALK_DENTS)) > 0) {
            for (int off = 0; off + 8 <= n;) {
                const u8 *d = dents + off;
                u16 reclen = *(const u16 *)(d + 4);
                if (!reclen)
                    break;
                off += reclen;
                const char *name = (const char *)d + 8;
                if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                    continue;
                char path[512];
                u8 st[0x78];
                fmt(path, sizeof(path), "%s/%s", dir, name);
                bool is_dir = d[6] == DT_DIR, have_st = false;
                if (!d[6] || !remove) {
                    have_st = kf.stat(path, st) == 0;
                    is_dir = have_st && (*(u16 *)(st + 8) & S_IFMT) == S_IFDIR;
                }
                size_t len = strlen(path) + 1;
                if (is_dir && ndirs < WALK_DIRS && used + len <= WALK_POOL) {
                    memcpy(pool + used, path, len);
                    dirs[ndirs++] = (u16)used;
                    used += (u32)len;
                } else if (is_dir)
                    lost++;
                else if (remove)
                    kf.unlink(path);
                else if (have_st)
                    bytes += *(s64 *)(st + 0x48);
            }
        }
        kf.close(fd);
    }
    for (u32 i = ndirs; remove && i-- > 1;)
        kf.rmdir(pool + dirs[i]);
    if (lost)
        logf("ext: %s: %u directories too many to %s", root, lost, remove ? "remove" : "count");
    page_free(work, WALK_DENTS + WALK_POOL + 2 * WALK_DIRS);
    return bytes;
}

static void title_dir_wipe(const char *sub)
{
    char dir[96];
    title_dir(dir, sizeof(dir), sub);
    logf("ext: formatting %s", dir);
    tree_walk(dir, true);
}

/* remap_path namespaces: the title's /temp0 and /download0 */
static volatile bool g_temp_made, g_download_made;

static const char *remap_temp0(const char *path, char *buf, size_t size)
{
    if (path[6] != '/' && path[6] != 0)
        return NULL;
    if (!__atomic_exchange_n(&g_temp_made, true, __ATOMIC_ACQ_REL))
        title_dir_make("");
    fmt(buf, size, "/temp0/ghostldr/%s%s", title_ident(), path + 6);
    return buf;
}

static const char *remap_download0(const char *path, char *buf, size_t size)
{
    if (path[10] != '/' && path[10] != 0)
        return NULL;
    if (!__atomic_exchange_n(&g_download_made, true, __ATOMIC_ACQ_REL))
        title_dir_make("/download0");
    fmt(buf, size, "/temp0/ghostldr/%s/download0%s", title_ident(), path + 10);
    return buf;
}

/* /data: a jailbroken console's data partition, where homebrew made for one
 * keeps its files (Apollo: /data/apollo).  SWRR's sandbox has no /data on a
 * stock console, so there the title's /data is a save container of its own,
 * kept across sessions (savedata_data_root: <prefix>D000SAVE), or where there
 * is none, /temp0/ghostldr/<TITLE>/data (wiped when SWRR closes).  Where
 * SWRR does see a /data (a dev console with an unjailed SWRR), paths go to it
 * as before. */
static volatile s32 g_data_real = -1; /* -1 unknown, 0 remapped, 1 real */
static volatile bool g_data_made;

static const char *remap_data(const char *path, char *buf, size_t size)
{
    if (path[5] != '/' && path[5] != 0)
        return NULL;
    if (g_data_real < 0 && kfs()) {
        u8 st[0x78];
        s32 rc = kf.stat("/data", st);
        g_data_real = rc == 0 ? 1 : 0;
        logf("ext: /data %s (stat %x)", rc == 0 ? "is SWRR's own: titles use it" : "is not in SWRR's sandbox: the "
                                                 "title's /data is a save container of its own", (u32)rc);
    }
    if (g_data_real != 0)
        return NULL;
    const char *kept = savedata_data_root();
    if (kept) {
        fmt(buf, size, "%s%s", kept, path + 5);
        return buf;
    }
    if (!__atomic_exchange_n(&g_data_made, true, __ATOMIC_ACQ_REL))
        title_dir_make("/data");
    fmt(buf, size, "/temp0/ghostldr/%s/data%s", title_ident(), path + 5);
    return buf;
}

const char *title_path(const char *path, char *buf, size_t size)
{
    const char *r = strncmp(path, "/data", 5) == 0 ? remap_data(path, buf, size) : NULL;
    return r ? r : remap_path(path, buf, size);
}

const char *usb_title_remap(const char *path, char *buf, size_t size) HIDDEN; /* approot.c */

/* free space on /temp0 in KiB, shared by everything there */
static u64 temp0_free_kb(void)
{
    static s32 (*real)(const char *, u64 *);
    static bool tried;
    if (!tried)
        real = host_real_in("libSceAppContent", "sceAppContentTemporaryDataGetAvailableSpaceKb"), tried = true;
    char mp[16] = "/temp0";
    u64 kb = 0;
    return real && real(mp, &kb) == 0 ? kb : 1048576;
}

/* ---- libSceAppContent ---- */

static s32 hle_sceAppContentInitialize(const void *init, u8 *boot)
{
    /* SWRR initialized AppContent for itself (the real call says BUSY) */
    if (boot)
        memset(boot, 0, 0x28);
    logf("sceAppContentInitialize: answering for %s", title_ident());
    return 0;
}

static s32 hle_sceAppContentAppParamGetInt(u32 id, s32 *val)
{
    static u32 logged;
    s32 v = 0, rc = 0;
    if (!val)
        return AC_PARAMETER;
    if (id == 0)
        v = 3; /* SKU_FLAG: FULL */
    else if (id <= 4) {
        /* USER_DEFINED_PARAM_n: Ghost builds cache/t%05d.dat from n = 1 */
        char key[24];
        fmt(key, sizeof(key), "USER_DEFINED_PARAM_%u", id);
        if (!title_param_int(key, &v))
            v = 0;
    } else
        rc = AC_PARAMETER;
    if (rc == 0)
        *val = v;
    if (logged++ < 16)
        logf("sceAppContentAppParamGetInt(%u) -> %x value %d", id, (u32)rc, v);
    return rc;
}

static s32 hle_sceAppContentAppParamGetString(u32 id, char *buf, size_t size)
{
    if (!buf || !size || id != 201) {
        logf("sceAppContentAppParamGetString(%u, %lu) -> parameter error", id, size);
        return AC_PARAMETER;
    }
    /* 201: the application title, in the system language when it has one */
    char key[12];
    s32 lang = system_lang();
    fmt(key, sizeof(key), "TITLE_%02d", lang);
    const char *t = lang >= 0 && lang < 30 ? title_param_str(key) : NULL;
    if (!t || !*t)
        t = title_param_str("TITLE");
    if (!t || !*t)
        t = title_ident();
    strlcpy(buf, t, size);
    logf("sceAppContentAppParamGetString(%u) -> \"%s\"", id, buf);
    return 0;
}

static s32 hle_sceAppContentGetAddcontInfoList(u32 label, void *list, u32 n, u32 *hit)
{
    if (label > LABEL_MAX || ((!list || !n) && !hit))
        return AC_PARAMETER;
    if (hit)
        *hit = 0; /* the title has no add-ons */
    return 0;
}

static s32 hle_sceAppContentGetAddcontInfo(u32 label, const void *ent, void *info)
{
    if (label > LABEL_MAX || !ent || !info)
        return AC_PARAMETER;
    return AC_NO_ENTITLEMENT;
}

/* sceAppContentAddcontMount, sceAppContentGetEntitlementKey */
static s32 hle_addcont_mount(u32 label, const void *ent, void *out)
{
    if (label > LABEL_MAX || !ent || !out)
        return AC_PARAMETER;
    return AC_NOT_FOUND;
}

static s32 hle_entitlement_key(u32 label, const void *ent, void *key)
{
    if (label > LABEL_MAX || !ent || !key)
        return AC_PARAMETER;
    return AC_NO_ENTITLEMENT;
}

/* mp is SceAppContentMountPoint (char[16]).  The title gets "/temp0", which
 * remap_path sends to its own directory; option 1 is FORMAT. */
static s32 temp_mount(u32 option, char *mp)
{
    if (!mp)
        return AC_PARAMETER;
    title_dir_make("");
    if (option & 1)
        title_dir_wipe("");
    strlcpy(mp, "/temp0", 16);
    logf("sceAppContentTemporaryDataMount(%x) -> /temp0 = /temp0/ghostldr/%s", option, title_ident());
    return 0;
}

static s32 hle_sceAppContentTemporaryDataMount2(u32 option, char *mp) { return temp_mount(option, mp); }
static s32 hle_sceAppContentTemporaryDataMount(char *mp) { return temp_mount(1, mp); }

static s32 hle_sceAppContentTemporaryDataFormat(const char *mp)
{
    if (!mp)
        return AC_PARAMETER;
    title_dir_wipe("");
    return 0;
}

static s32 hle_sceAppContentTemporaryDataGetAvailableSpaceKb(const char *mp, u64 *kb)
{
    if (!mp || !kb)
        return AC_PARAMETER;
    *kb = temp0_free_kb();
    return 0;
}

static s32 hle_sceAppContentDownloadDataGetAvailableSpaceKb(const char *mp, u64 *kb)
{
    if (!kb)
        return AC_PARAMETER;
    s32 mib;
    if (title_param_int("DOWNLOAD_DATA_SIZE", &mib) && mib > 0) {
        char dir[96];
        title_dir(dir, sizeof(dir), "/download0");
        s64 used = tree_walk(dir, false);
        u64 used_kb = used > 0 ? ((u64)used + 1023) / 1024 : 0, cap = (u64)mib * 1024;
        *kb = used_kb < cap ? cap - used_kb : 0;
    } else {
        u64 free = temp0_free_kb();
        *kb = free < 1048576 ? free : 1048576;
    }
    return 0;
}

static s32 hle_sceAppContentDownloadDataFormat(const char *mp)
{
    title_dir_wipe("/download0");
    return 0;
}

/* calls with nothing to do for the title, and calls that would act on
 * SWRR's store entry or need an add-on it does not have */
#define STUB(fn, ret)                                                \
    static s32 fn(void)                                              \
    {                                                                \
        static u32 n;                                                \
        if (!n++)                                                    \
            logf("ext: AppContent stub " #fn " -> %x", (u32)(ret)); \
        return (s32)(ret);                                           \
    }
STUB(ac_ok, 0)
STUB(ac_not_found, AC_NOT_FOUND)
STUB(ac_no_entitlement, AC_NO_ENTITLEMENT)

/* the region enum is unverified: nothing is written (spec: NEEDS-PROBE) */
static s32 hle_sceAppContentGetRegion(s32 *region)
{
    const char *cid = title_param_str("CONTENT_ID");
    logf("sceAppContentGetRegion: content %s, not answered", cid ? cid : "?");
    return 0;
}

#define LIB "libSceAppContent"
#define E(fn) {LIB, #fn, hle_##fn, NULL}
static const struct ext_hle_ent k_hle[] = {
    E(sceAppContentInitialize),
    E(sceAppContentAppParamGetInt),
    E(sceAppContentAppParamGetString),
    E(sceAppContentGetAddcontInfoList),
    E(sceAppContentGetAddcontInfo),
    {LIB, "sceAppContentAddcontMount", hle_addcont_mount, NULL},
    {LIB, "sceAppContentAddcontUnmount", ac_ok, NULL},
    {LIB, "sceAppContentGetEntitlementKey", hle_entitlement_key, NULL},
    E(sceAppContentTemporaryDataMount2),
    E(sceAppContentTemporaryDataMount),
    {LIB, "sceAppContentTemporaryDataUnmount", ac_ok, NULL}, /* never SWRR's /temp0 */
    E(sceAppContentTemporaryDataFormat),
    E(sceAppContentTemporaryDataGetAvailableSpaceKb),
    E(sceAppContentDownloadDataGetAvailableSpaceKb),
    E(sceAppContentDownloadDataFormat),
    {LIB, "sceAppContentDownload0Expand", ac_ok, NULL},
    {LIB, "sceAppContentDownload0Shrink", ac_ok, NULL},
    {LIB, "sceAppContentDownload1Expand", ac_ok, NULL},
    {LIB, "sceAppContentDownload1Shrink", ac_ok, NULL},
    {LIB, "sceAppContentSmallSharedDataMount", ac_not_found, NULL},
    {LIB, "sceAppContentSmallSharedDataUnmount", ac_ok, NULL},
    {LIB, "sceAppContentSmallSharedDataFormat", ac_ok, NULL},
    {LIB, "sceAppContentSmallSharedDataGetAvailableSpaceKb", ac_ok, NULL},
    {LIB, "sceAppContentGetAddcontDownloadProgress", ac_not_found, NULL},
    {LIB, "sceAppContentAddcontEnqueueDownload", ac_not_found, NULL},
    {LIB, "sceAppContentAddcontEnqueueDownloadSp", ac_not_found, NULL},
    {LIB, "sceAppContentAddcontDelete", ac_not_found, NULL},
    {LIB, "sceAppContentAddcontShrink", ac_not_found, NULL},
    {LIB, "sceAppContentRequestPatchInstall", ac_ok, NULL},
    E(sceAppContentGetRegion),
    /* secondary libraries (host lookups by name never reach them) */
    {"libSceAppContentPft", "sceAppContentGetPftFlag", ac_ok, NULL},
    {"libSceAppContentIro", "sceAppContentGetAddcontInfoByEntitlementId", ac_no_entitlement, NULL},
    {"libSceAppContentIro", "sceAppContentGetAddcontInfoListByIroTag", ac_ok, NULL},
    /* Sony's spelling ("Entitlemet") */
    {"libSceAppContentIro", "sceAppContentAddcontMountByEntitlemetId", ac_not_found, "efX3lrPwdKA"},
    {"libSceAppContentIro", "sceAppContentAddcontEnqueueDownloadByEntitlemetId", ac_not_found, "kJmjt81mXKQ"},
    {"libSceAppContentSc", "sceAppContentGetDownloadedStoreCountry", ac_ok, NULL},
    {"libSceAppContentBundle", NULL, ac_ok, "xZo2-418Wdo"}, /* Func_C59A36FF8D7C59DA */
};

/* SWRR's AppContent never answers for the title: functions not listed get
 * NOT_SUPPORTED */
static const struct ext_lib k_libs[] = {
    {LIB, "libSceAppContentIro libSceAppContentPft libSceAppContentSc libSceAppContentBundle", EXT_LIB_NO_HOST,
     AC_NOT_SUPPORTED},
};

static int appcontent_init(void)
{
    if (path_remap_add &&
        (path_remap_add("/temp0", remap_temp0) != 0 || path_remap_add("/download0", remap_download0) != 0 ||
         path_remap_add("/data", remap_data) != 0 || path_remap_add("/mnt/usb", usb_title_remap) != 0))
        logf("ext: no room for the /temp0, /download0, /data and /mnt/usb remaps");
    return 0;
}

EXT_FAMILY_LIBS("appcontent", k_hle, k_libs, appcontent_init);
