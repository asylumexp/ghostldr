/*
 * The system families of the runtime extension (ext/src/hle_system.c,
 * hle_appcontent.c, hle_playgo.c, hle_gameintent.c) run from the real
 * ext.bin: the core's ext.c fetches, places, relocates and initializes it,
 * and the functions are called as a title would after looking them up by
 * library and NID.  SWRR's system services are faked here (scripted events,
 * users, settings), and libkernel's file calls work on a directory of this
 * machine (fam_system_sys.c).  test_hle_system.py builds and drives it:
 *
 *     fam_system MODE EXT_BIN CRC ROOT TITLE
 *     MODE   params-ghost | params-sfo | params-unknown | temp | download
 *            | events | users | playgo | playgo-dat | discmap | misc
 *     ROOT   the fake file system: ROOT/app0 is the title's app0,
 *            ROOT/temp0 SWRR's temporary data
 *     TITLE  what the core's title_id() says (the ghostfs index), "-" for ""
 *
 * Prints "fam_system: ok|FAILED ..." lines; exits 0 when all passed.
 */
#include "ghost_api.h"

/* fam_system_sys.c */
void *fs_map(u64 size);
void fs_unmap(void *p, u64 size);
void fs_protect(void *addr, u64 size, int writable);
void fs_vlog(const char *fmt, __builtin_va_list ap);
int fs_vfmt(char *buf, u64 size, const char *fmt, __builtin_va_list ap);
void *fs_calloc(u64 n);
u64 fs_strtoul(const char *s);
int fs_open(const char *path, int flags, int mode);
int fs_close(int fd);
s64 fs_read(int fd, void *buf, u64 n);
s64 fs_lseek(int fd, s64 off, int whence);
int fs_getdents(int fd, void *buf, int size);
int fs_stat(const char *path, void *st);
int fs_unlink(const char *path);
int fs_rmdir(const char *path);
int fs_mkdir(const char *path, int mode);
int fs_exists(const char *path);
int fs_raw_open(const char *path);
s64 fs_raw_read(int fd, void *buf, u64 n);
s64 fs_file_size(const char *path);

struct kfuncs K;
struct boot_ctx *g_ctx;
struct module *g_modules, *g_main_mod;
u64 g_reported_dmem;

void logf(const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    fs_vlog(f, ap);
    __builtin_va_end(ap);
}

int fmt(char *buf, size_t size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = fs_vfmt(buf, size, f, ap);
    __builtin_va_end(ap);
    return n;
}

const char *strrchr_c(const char *s, int c)
{
    const char *r = NULL;
    for (; *s; s++)
        if (*s == c)
            r = s;
    return r;
}

static int g_failed;
static void expect(bool ok, const char *what)
{
    logf("fam_system: %s %s", ok ? "ok" : "FAILED", what);
    if (!ok)
        g_failed = 1;
}

/* ---- the core's memory and executable memory (ext.c) ---- */

void *page_alloc(size_t size) { return fs_map(ALIGN_UP(size, 0x4000)); }
void page_free(void *addr, size_t size) { fs_unmap(addr, ALIGN_UP(size, 0x4000)); }
void *arena_alloc(size_t n) { return fs_calloc(n); }
u64 arena_small_alloc_top(u64 size, u64 keep) { return 0; }

int arena_write(u64 dst, const void *src, u64 size)
{
    u64 lo = ALIGN_DOWN(dst, 0x4000), hi = ALIGN_UP(dst + size, 0x4000);
    fs_protect((void *)lo, hi - lo, 1);
    memcpy((void *)dst, src, size);
    fs_protect((void *)lo, hi - lo, 0);
    return 0;
}

int arena_punch_rw(u64 addr, u64 size)
{
    fs_protect((void *)addr, size, 1);
    memset((void *)addr, 0, size);
    return 0;
}

int jit_block_create(struct jit_block *b, u64 size, u64 want_rx)
{
    memset(b, 0, sizeof(*b));
    b->size = ALIGN_UP(size, 0x4000);
    b->rx = b->rw = (u64)fs_map(b->size);
    fs_protect((void *)b->rx, b->size, 0);
    return 0;
}

int jit_block_write(struct jit_block *b, u64 offset, const void *src, size_t len)
{
    return offset + len > b->size ? -1 : arena_write(b->rw + offset, src, len);
}

static void lock(volatile int *l)
{
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        ;
}
static void unlock(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

/* ---- the fake file system: host paths live under ROOT ---- */

static char g_root[512];

static const char *rooted(const char *path, char *buf)
{
    fmt(buf, 1024, "%s%s", g_root, path);
    return buf;
}

static int k_open(const char *p, int flags, int mode)
{
    char b[1024];
    return fs_open(rooted(p, b), flags, mode);
}
static int k_stat(const char *p, void *st)
{
    char b[1024];
    return fs_stat(rooted(p, b), st);
}
static int k_unlink(const char *p)
{
    char b[1024];
    return fs_unlink(rooted(p, b));
}
static int k_rmdir(const char *p)
{
    char b[1024];
    return fs_rmdir(rooted(p, b));
}
static int k_mkdir(const char *p, int mode)
{
    char b[1024];
    return fs_mkdir(rooted(p, b), mode);
}

static bool exists(const char *path)
{
    char b[1024];
    return fs_exists(rooted(path, b));
}

/* the core's remap_path, as hle.c has it: the extension's namespaces */
static struct {
    const char *prefix;
    const char *(*fn)(const char *, char *, size_t);
} g_remaps[4];
static u32 g_nremaps;

int path_remap_add(const char *prefix, const char *(*fn)(const char *, char *, size_t))
{
    if (g_nremaps >= 4)
        return -1;
    g_remaps[g_nremaps].prefix = prefix;
    g_remaps[g_nremaps++].fn = fn;
    return 0;
}

const char *remap_path(const char *path, char *buf, size_t size)
{
    for (u32 i = 0; i < g_nremaps; i++)
        if (strncmp(path, g_remaps[i].prefix, strlen(g_remaps[i].prefix)) == 0) {
            const char *r = g_remaps[i].fn(path, buf, size);
            if (r)
                return r;
        }
    return path;
}

static char g_title[32];
const char *title_id(void) { return g_title; }

/* ---- SWRR's system services, faked ---- */

#define SS_NO_EVENT 0x80A10004
#define US_ALREADY 0x80960003
#define US_NO_EVENT 0x80960007
static s32 g_lang = 1; /* en-US */
static u32 g_ss_events[8], g_ss_nevents, g_ss_next;
static struct { s32 type, user; } g_us_events[8];
static u32 g_us_nevents, g_us_next;
static s32 g_login[4] = {0x1002, 0x1001, -1, 0x1003};
static u32 g_status_calls;

static s32 f_param_get_int(s32 id, s32 *v)
{
    if (id == 1)
        return *v = g_lang, 0;
    if (id == 1000)
        return *v = 1, 0;
    return (s32)0x80A10003;
}

static s32 f_get_status(u8 *st)
{
    g_status_calls++;
    memset(st, 0, 0x88);
    *(s32 *)st = 2;
    st[4] = 1; /* uiOverlaid, as SWRR's process reports it */
    st[6] = 1;
    return 0;
}

static s32 f_receive_event(u8 *ev)
{
    if (g_ss_next >= g_ss_nevents)
        return (s32)SS_NO_EVENT;
    *(u32 *)ev = g_ss_events[g_ss_next++];
    memset(ev + 4, 0xEE, 0x2000); /* payload */
    return 0;
}

static s32 f_initial_user(s32 *u) { return *u = 0x1001, 0; }
static s32 f_login_list(s32 *ids)
{
    memcpy(ids, g_login, sizeof(g_login));
    return 0;
}
static s32 f_us_initialize(const void *p) { return (s32)US_ALREADY; }
static s32 f_us_get_event(s32 *ev)
{
    if (g_us_next >= g_us_nevents)
        return (s32)US_NO_EVENT;
    ev[0] = g_us_events[g_us_next].type;
    ev[1] = g_us_events[g_us_next++].user;
    return 0;
}

static s32 f_temp_space(const char *mp, u64 *kb)
{
    if (strcmp(mp, "/temp0") != 0)
        return (s32)0x80D90004;
    *kb = 500000;
    return 0;
}

static u32 g_gi_inits;
static s32 f_gi_initialize(const void *p) { return g_gi_inits++ ? (s32)0x80553801 : 0; }

void *host_real_in(const char *mod, const char *name)
{
    static const struct {
        const char *mod, *name;
        void *fn;
    } fakes[] = {
        {"libSceSystemService", "sceSystemServiceParamGetInt", f_param_get_int},
        {"libSceSystemService", "sceSystemServiceGetStatus", f_get_status},
        {"libSceSystemService", "sceSystemServiceReceiveEvent", f_receive_event},
        {"libSceUserService", "sceUserServiceGetInitialUser", f_initial_user},
        {"libSceUserService", "sceUserServiceGetLoginUserIdList", f_login_list},
        {"libSceUserService", "sceUserServiceInitialize", f_us_initialize},
        {"libSceUserService", "sceUserServiceGetEvent", f_us_get_event},
        {"libSceAppContent", "sceAppContentTemporaryDataGetAvailableSpaceKb", f_temp_space},
        {"libSceNpGameIntent", "sceNpGameIntentInitialize", f_gi_initialize},
    };
    for (u32 i = 0; i < ARRAY_SIZE(fakes); i++)
        if (strcmp(fakes[i].mod, mod) == 0 && strcmp(fakes[i].name, name) == 0)
            return fakes[i].fn;
    logf("host_real_in: %s:%s missing", mod, name); /* secondary libraries, as on the console */
    return NULL;
}

void *host_real(const char *name)
{
    static const struct {
        const char *name;
        void *fn;
    } fakes[] = {
        {"sceKernelOpen", k_open},     {"sceKernelClose", fs_close}, {"sceKernelGetdents", fs_getdents},
        {"sceKernelStat", k_stat},     {"sceKernelUnlink", k_unlink}, {"sceKernelRmdir", k_rmdir},
        {"sceKernelMkdir", k_mkdir},
    };
    for (u32 i = 0; i < ARRAY_SIZE(fakes); i++)
        if (strcmp(fakes[i].name, name) == 0)
            return fakes[i].fn;
    return NULL;
}

/* vfs: the title's /app0 is ROOT/app0 (ghostfs on the console) */
int vfs_open(const char *path, int flags, int mode) { return k_open(path, flags, mode); }
s64 vfs_read(int fd, void *buf, u64 n) { return fs_read(fd, buf, n); }
s64 vfs_lseek(int fd, s64 off, int whence) { return fs_lseek(fd, off, whence); }
int vfs_close(int fd) { return fs_close(fd); }

const struct ghost_api g_ghost_api = {
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
    .compute_nid = compute_nid,
    .page_alloc = page_alloc,
    .page_free = page_free,
    .arena_alloc = arena_alloc,
    .spin_lock = lock,
    .spin_unlock = unlock,
    .remap_path = remap_path,
    .vfs_open = vfs_open,
    .vfs_read = vfs_read,
    .vfs_lseek = vfs_lseek,
    .vfs_close = vfs_close,
    .title_id = title_id,
    .path_remap_add = path_remap_add,
};

/* ---- the families' functions, found as a title's imports would be ---- */

#define SS "libSceSystemService"
#define US "libSceUserService"
#define AC "libSceAppContent"
#define PG "libScePlayGo"
#define DM "libSceDiscMap"
#define GI "libSceNpGameIntent"

static void *by_nid(const char *lib, const char *nid)
{
    void *f = (void *)ext_hle_lookup(lib, nid, NULL);
    if (!f) {
        logf("fam_system: FAILED %s:%s is not in the extension", lib, nid);
        g_failed = 1;
    }
    return f;
}

static void *by_name(const char *lib, const char *name)
{
    char nid[12];
    compute_nid(name, nid);
    return by_nid(lib, nid);
}

#define FN(type, lib, name) ((type)by_name(lib, name))

/* ---- the checks, one group per mode ---- */

static s32 param_int(u32 id, s32 *v) { return FN(s32(*)(u32, s32 *), AC, "sceAppContentAppParamGetInt")(id, v); }
static s32 param_str(u32 id, char *buf, size_t n)
{
    return FN(s32(*)(u32, char *, size_t), AC, "sceAppContentAppParamGetString")(id, buf, n);
}

static void params_ghost(void)
{
    u8 boot[0x28];
    memset(boot, 0xAA, sizeof(boot));
    s32 rc = FN(s32(*)(const void *, void *), AC, "sceAppContentInitialize")(boot, boot);
    bool zero = true;
    for (u32 i = 0; i < sizeof(boot); i++)
        zero &= boot[i] == 0;
    expect(rc == 0 && zero, "Initialize returns 0 and clears the boot param");
    s32 v = -1;
    expect(param_int(0, &v) == 0 && v == 3, "SKU flag is FULL (3)");
    expect(param_int(1, &v) == 0 && v == 11456, "Ghost without param.sfo: USER_DEFINED_PARAM_1 = 11456");
    expect(param_int(2, &v) == 0 && v == 0, "an absent USER_DEFINED_PARAM reads 0");
    expect(param_int(5, &v) == (s32)0x80D90002 && param_int(1, NULL) == (s32)0x80D90002,
           "an unknown id or a NULL value is a PARAMETER error");
    char t[64];
    expect(param_str(201, t, sizeof(t)) == 0 && strcmp(t, "Ghost of Tsushima") == 0, "title string (201)");
    expect(param_str(201, t, 6) == 0 && strcmp(t, "Ghost") == 0, "title string truncated to the buffer");
    expect(param_str(200, t, sizeof(t)) == (s32)0x80D90002, "other string ids are a PARAMETER error");
    u32 hit = 7;
    expect(FN(s32(*)(u32, void *, u32, u32 *), AC, "sceAppContentGetAddcontInfoList")(0, NULL, 0, &hit) == 0 &&
               hit == 0,
           "no add-ons");
    expect(FN(s32(*)(u32, void *, u32, u32 *), AC, "sceAppContentGetAddcontInfoList")(8, NULL, 0, &hit) ==
               (s32)0x80D90002,
           "service label 8 is a PARAMETER error");
    u8 ent[0x14] = "GHOSTDLC00000001", info[0x18];
    expect(FN(s32(*)(u32, const void *, void *), AC, "sceAppContentGetAddcontInfo")(0, ent, info) ==
               (s32)0x80D90007,
           "an add-on is not owned (DRM_NO_ENTITLEMENT)");
    expect(FN(s32(*)(u32, const void *, void *), AC, "sceAppContentAddcontMount")(0, ent, info) == (s32)0x80D90005,
           "an add-on mount is NOT_FOUND");
}

static void params_sfo(void)
{
    s32 v = -1;
    expect(param_int(1, &v) == 0 && v == 7, "param.sfo: USER_DEFINED_PARAM_1 = 7");
    expect(param_int(2, &v) == 0 && v == 42, "param.sfo: USER_DEFINED_PARAM_2 = 42");
    expect(param_int(3, &v) == 0 && v == 0, "param.sfo wins: no built-in values for its title");
    char t[64];
    expect(param_str(201, t, sizeof(t)) == 0 && strcmp(t, "Test Title (US)") == 0,
           "title string in the system language (TITLE_01)");
    g_lang = 5;
    expect(param_str(201, t, sizeof(t)) == 0 && strcmp(t, "Test Title") == 0,
           "title string without one for the language (TITLE)");
}

static void params_unknown(void)
{
    s32 v = -1;
    expect(param_int(1, &v) == 0 && v == 0, "an unknown title has no USER_DEFINED_PARAM_1");
    char t[64];
    expect(param_str(201, t, sizeof(t)) == 0 && strcmp(t, "untitled") == 0, "an unknown title is named after its ID");
}

static void temp(void)
{
    char buf[512];
    const char *r = remap_path("/temp0/save/a.bin", buf, sizeof(buf));
    expect(strcmp(r, "/temp0/ghostldr/CUSA11456/save/a.bin") == 0, "/temp0/x is the title's own directory");
    expect(strcmp(remap_path("/temp0", buf, sizeof(buf)), "/temp0/ghostldr/CUSA11456") == 0, "/temp0 itself too");
    expect(strcmp(remap_path("/temp00/x", buf, sizeof(buf)), "/temp00/x") == 0, "/temp00 is left alone");
    expect(strcmp(remap_path("/download0/c.bin", buf, sizeof(buf)), "/temp0/ghostldr/CUSA11456/download0/c.bin") == 0,
           "/download0 goes under the title's temporary directory");
    expect(exists("/temp0/ghostldr/CUSA11456/download0"), "the remaps create the directories");
    char mp[16];
    memset(mp, 'x', sizeof(mp));
    s32 (*mount2)(u32, char *) = FN(s32(*)(u32, char *), AC, "sceAppContentTemporaryDataMount2");
    expect(mount2(0, mp) == 0 && strcmp(mp, "/temp0") == 0, "Mount2 gives the title /temp0");
    expect(exists("/temp0/ghostldr/CUSA11456/a.bin") && exists("/temp0/ghostldr/CUSA11456/d1/d2/c.bin"),
           "a mount without FORMAT keeps the title's files");
    expect(mount2(1, mp) == 0, "Mount2 with FORMAT");
    expect(exists("/temp0/ghostldr/CUSA11456") && !exists("/temp0/ghostldr/CUSA11456/a.bin") &&
               !exists("/temp0/ghostldr/CUSA11456/d1"),
           "FORMAT empties the title's directory and keeps it");
    expect(exists("/temp0/swrr.dat") && exists("/temp0/ghostldr/CUSA00001/keep.bin"),
           "FORMAT leaves SWRR's files and other titles' alone");
    expect(mount2(0, NULL) == (s32)0x80D90002, "Mount2 without a mount point is a PARAMETER error");
    u64 kb = 0;
    expect(FN(s32(*)(const char *, u64 *), AC, "sceAppContentTemporaryDataGetAvailableSpaceKb")(mp, &kb) == 0 &&
               kb == 500000,
           "free space is /temp0's");
    expect(FN(s32(*)(const char *, u64 *), AC, "sceAppContentDownloadDataGetAvailableSpaceKb")("/download0", &kb) ==
                   0 &&
               kb == 500000,
           "download data without DOWNLOAD_DATA_SIZE: /temp0's free space, at most 1 GiB");
    expect(FN(s32(*)(const char *), AC, "sceAppContentTemporaryDataUnmount")(mp) == 0, "Unmount is a no-op");
}

static void download(void)
{
    u64 kb = 0;
    expect(FN(s32(*)(const char *, u64 *), AC, "sceAppContentDownloadDataGetAvailableSpaceKb")("/download0", &kb) ==
                   0 &&
               kb == 2048 - 2,
           "download space: DOWNLOAD_DATA_SIZE (2 MiB) less what download0 holds (1.5 KiB)");
    char buf[512];
    expect(strcmp(remap_path("/download0", buf, sizeof(buf)), "/temp0/ghostldr/CUSA99999/download0") == 0,
           "param.sfo's TITLE_ID names the directory");
    expect(FN(s32(*)(const char *), AC, "sceAppContentDownloadDataFormat")("/download0") == 0 &&
               !exists("/temp0/ghostldr/CUSA99999/download0/sub/f.bin") &&
               exists("/temp0/ghostldr/CUSA99999/download0") && exists("/temp0/ghostldr/CUSA99999/t.bin"),
           "DownloadDataFormat empties download0 only");
}

static void events(void)
{
    u8 *ev = fs_map(0x4000);
    s32 (*receive)(u8 *) = FN(s32(*)(u8 *), SS, "sceSystemServiceReceiveEvent");
    u32 script[] = {0x10000017, 0x10000003, 0x10000000, 0x10000002, 0x10000005, 0x10000099};
    memcpy(g_ss_events, script, sizeof(script));
    g_ss_nevents = ARRAY_SIZE(script);
    expect(receive(ev) == 0 && *(u32 *)ev == 0x10000000, "game intent and entitlement events are dropped; OnResume passes");
    expect(receive(ev) == 0 && *(u32 *)ev == 0x10000005, "an invitation is dropped; DisplaySafeAreaUpdate passes");
    expect(receive(ev) == 0 && *(u32 *)ev == 0x10000099, "an unknown event passes");
    expect(receive(ev) == (s32)SS_NO_EVENT, "then NO_EVENT");
    g_ss_events[0] = 0x10000017, g_ss_nevents = 1, g_ss_next = 0;
    bool clear = true;
    expect(receive(ev) == (s32)SS_NO_EVENT, "a dropped event alone leaves NO_EVENT");
    for (u32 i = 0; i < 0x2004; i++)
        clear &= ev[i] == 0;
    expect(clear, "and the dropped event's 0x2004 bytes are cleared");
    expect(receive(NULL) == (s32)0x80A10003, "ReceiveEvent(NULL) is a PARAMETER error");
    u8 st[0x88];
    memset(st, 0x5A, sizeof(st));
    expect(FN(s32(*)(u8 *), SS, "sceSystemServiceGetStatus")(st) == 0 && st[4] == 0 && *(s32 *)st == 2 && st[6] == 1,
           "GetStatus passes with isSystemUiOverlaid cleared");
    expect(FN(s32(*)(u8 *), SS, "sceSystemServiceGetStatus")(NULL) == (s32)0x80A10003 && g_status_calls == 1,
           "GetStatus(NULL) is a PARAMETER error, without the real call");
    s32 v = -1;
    expect(FN(s32(*)(s32, s32 *), SS, "sceSystemServiceParamGetInt")(1000, &v) == 0 && v == 1,
           "ParamGetInt passes");
}

static void users(void)
{
    s32 ev[2];
    s32 (*get)(s32 *) = FN(s32(*)(s32 *), US, "sceUserServiceGetEvent");
    expect(FN(s32(*)(const void *), US, "sceUserServiceInitialize")(NULL) == 0,
           "Initialize: SWRR's ALREADY_INITIALIZED is success");
    g_us_events[0].type = 0, g_us_events[0].user = 0x1002; /* a second LOGIN: dropped */
    g_us_events[1].type = 1, g_us_events[1].user = 0x1002;
    g_us_events[2].type = 0, g_us_events[2].user = 0x1002;
    g_us_nevents = 3;
    expect(get(ev) == 0 && ev[0] == 0 && ev[1] == 0x1001, "LOGIN for the initial user first");
    expect(get(ev) == 0 && ev[0] == 0 && ev[1] == 0x1002, "then LOGIN for each other logged-in user");
    expect(get(ev) == 0 && ev[0] == 0 && ev[1] == 0x1003, "in login-list order");
    expect(get(ev) == 0 && ev[0] == 1 && ev[1] == 0x1002, "the real duplicate LOGIN is dropped, LOGOUT passes");
    expect(get(ev) == 0 && ev[0] == 0 && ev[1] == 0x1002, "a LOGIN after the LOGOUT passes");
    expect(get(ev) == (s32)US_NO_EVENT, "then NO_EVENT");
    expect(get(NULL) == (s32)0x80960005, "GetEvent(NULL) is INVALID_ARGUMENT");
    expect(FN(s32(*)(void), US, "sceUserServiceTerminate")() == 0, "Terminate leaves SWRR's service alone");
    s32 list[16];
    memset(list, 0x33, sizeof(list));
    expect(FN(s32(*)(s32 *), "libSceUserServiceRegisteredUserIdList", "sceUserServiceGetRegisteredUserIdList")(list) ==
                   0 &&
               list[0] == 0x1002 && list[1] == 0x1001 && list[2] == -1 && list[3] == 0x1003 && list[15] == -1,
           "the registered users, from the login list when the secondary library is out of reach");
}

static void playgo(bool dat)
{
    s32 (*init)(const u64 *) = FN(s32(*)(const u64 *), PG, "scePlayGoInitialize");
    u64 param[2] = {0x1000, 0x200000};
    expect(init(NULL) == (s32)0x80B2000A && init(param) == 0 && init(param) == 0, "Initialize, also twice");
    u32 h = 0, n = 0;
    expect(FN(s32(*)(u32 *, const void *), PG, "scePlayGoOpen")(&h, NULL) == 0 && h == 1, "Open gives handle 1");
    s32 (*chunk_id)(u32, u16 *, u32, u32 *) = FN(s32(*)(u32, u16 *, u32, u32 *), PG, "scePlayGoGetChunkId");
    u16 ids[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    u32 want = dat ? 3 : 1;
    expect(chunk_id(h, NULL, 0, &n) == 0 && n == want, dat ? "three chunks (playgo-chunk.dat)" : "one chunk without playgo-chunk.dat");
    expect(chunk_id(h, ids, 8, &n) == 0 && n == want && ids[0] == 0 && ids[want - 1] == want - 1 && ids[want] == 9,
           "the chunk ids");
    expect(chunk_id(h, ids, 0, &n) == (s32)0x80B2000B && chunk_id(h, ids, 1, NULL) == (s32)0x80B2000A,
           "GetChunkId size and pointer errors");
    s8 loci[3] = {0, 0, 0};
    u16 q[2] = {0, (u16)(want - 1)};
    expect(FN(s32(*)(u32, const u16 *, u32, s8 *), PG, "scePlayGoGetLocus")(h, q, 2, loci) == 0 && loci[0] == 3 &&
               loci[1] == 3 && loci[2] == 0,
           "every chunk is LOCAL_FAST");
    s64 eta = -1;
    expect(FN(s32(*)(u32, const u16 *, u32, s64 *), PG, "scePlayGoGetEta")(h, q, 2, &eta) == 0 && eta == 0, "ETA 0");
    u64 prog[2] = {0, 0};
    u64 total = dat ? 3000 + 400000 : 1;
    expect(FN(s32(*)(u32, const u16 *, u32, u64 *), PG, "scePlayGoGetProgress")(h, q, 2, prog) == 0 &&
               prog[0] == total && prog[1] == total,
           dat ? "progress is complete: the chunks' sizes from playgo-chunk.dat" : "progress is complete (1/1)");
    expect(FN(s32(*)(u32, void *, u32, u32 *), PG, "scePlayGoGetToDoList")(h, ids, 8, &n) == 0 && n == 0,
           "nothing to download");
    u64 mask = 0;
    expect(FN(s32(*)(u32, u64 *), PG, "scePlayGoGetLanguageMask")(h, &mask) == 0 && mask == 1ULL << 62,
           "the language mask follows the system language");
    expect(FN(s32(*)(u32, s32), PG, "scePlayGoSetInstallSpeed")(h, 3) == (s32)0x80B20004 &&
               FN(s32(*)(u32, const u16 *, u32, s8), PG, "scePlayGoPrefetch")(h, q, 2, 1) == (s32)0x80B20010,
           "a bad install speed or locus is refused");
    const struct ext_lib *pol = ext_lib_policy("libSceDbgPlayGo");
    expect(pol && (pol->flags & EXT_LIB_NO_HOST) && strcmp(pol->name, PG) == 0, "PlayGo never falls back to SWRR's");
}

static void discmap(void)
{
    s32 flags = 0x55, r1 = 0x55, r2 = 0x55, on_hdd = 0;
    s32 (*f)(const char *, s64, s64, s32 *, s32 *, s32 *) =
        (s32(*)(const char *, s64, s64, s32 *, s32 *, s32 *))by_nid(DM, "fJgP+wqifno");
    expect(f && f("/app0/x.psarc", 0, 0x10000, &flags, &r1, &r2) == 0 && flags == 0 && r1 == 0 && r2 == 0,
           "fJgP+wqifno (no known name) zeroes its three outputs");
    expect(FN(s32(*)(const char *, s64, s64, s32 *), DM, "sceDiscMapIsRequestOnHDD")("/app0/x", 0, 1, &on_hdd) == 0 &&
               on_hdd == 1,
           "every request is on the HDD");
    expect(((s32(*)(void))by_nid(DM, "ioKMruft1ek"))() == (s32)0x81100004, "the other unnamed calls: NO_BITMAP_INFO");
    const struct ext_lib *pol = ext_lib_policy(DM);
    expect(pol && (pol->flags & EXT_LIB_NO_HOST) && pol->stub_ret == (s32)0x81100004,
           "DiscMap never falls back to SWRR's; other calls return NO_BITMAP_INFO");
}

static void misc(void)
{
    s32 v = 7;
    expect(FN(s32(*)(s32 *), "libSceSystemServicePlatformPrivacy", "sceSystemServiceGetPlatformPrivacySetting")(&v) ==
                   0 &&
               v == 0,
           "GetPlatformPrivacySetting writes 0 through its one pointer");
    expect(FN(s32(*)(const char *, const void *), SS, "sceSystemServiceLoadExec")("/app0/eboot.bin", NULL) ==
               (s32)0x80A10005,
           "LoadExec is REJECTED");
    expect(FN(s32(*)(const char *, const void *, const void *), SS, "sceSystemServiceLaunchApp")("CUSA00001", NULL,
                                                                                                 NULL) ==
               (s32)0x80A10005,
           "LaunchApp is REJECTED");
    expect(FN(s32(*)(void), "libSceSystemServiceSuspend", "sceSystemServiceEnableSuspendNotification")() == 0,
           "the Suspend library is answered");
    expect(FN(s32(*)(const char *, const void *), "libSceSystemServiceWebBrowser",
              "sceSystemServiceLaunchWebBrowser")("https://example.com", NULL) == (s32)0x80A10005,
           "LaunchWebBrowser out of the host's reach is REJECTED");
    expect(FN(s32(*)(const void *), GI, "sceNpGameIntentInitialize")(NULL) == 0 &&
               FN(s32(*)(const void *), GI, "sceNpGameIntentInitialize")(NULL) == 0,
           "NpGameIntent Initialize: ALREADY_INITIALIZED is success");
    expect(FN(s32(*)(void), GI, "sceNpGameIntentLaunchApp")() == (s32)0x80553800, "NpGameIntent LaunchApp is refused");
    expect(ext_hle_lookup(GI, "jEIXUAr9XE8", NULL) == 0, "ReceiveIntent passes to the host");
    expect(ext_hle_lookup(SS, "Vo5V8KAwCmk", NULL) == 0, "HideSplashScreen passes to the host");
}

static ssize_t k_read(int fd, void *buf, size_t n) { return fs_raw_read(fd, buf, n); }
static int k_close(int fd) { return fs_close(fd); }
static int k_map_flex(void **addr, size_t len, int prot, int flags)
{
    *addr = fs_map(len);
    return 0;
}
static int k_munmap(void *addr, size_t len)
{
    fs_unmap(addr, len);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        logf("usage: fam_system MODE EXT_BIN CRC ROOT TITLE");
        return 2;
    }
    const char *mode = argv[1];
    strlcpy(g_root, argv[4], sizeof(g_root));
    strlcpy(g_title, strcmp(argv[5], "-") == 0 ? "" : argv[5], sizeof(g_title));
    static struct boot_ctx ctx;
    ctx.magic = BOOT_MAGIC;
    ctx.size = sizeof(ctx);
    ctx.flags = CTX_FLAG_EXT_JIT;
    ctx.ext_crc = fs_strtoul(argv[3]);
    ctx.ext_size = (u64)fs_file_size(argv[2]);
    ctx.ext_fd = (u64)fs_raw_open(argv[2]);
    g_ctx = &ctx;
    K.read = k_read;
    K.close = k_close;
    K.map_flex = k_map_flex;
    K.munmap = k_munmap;
    if (ext_fetch() != 0 || ext_start() != 0) {
        logf("fam_system: FAILED to load the extension");
        return 1;
    }
    if (strcmp(mode, "params-ghost") == 0)
        params_ghost();
    else if (strcmp(mode, "params-sfo") == 0)
        params_sfo();
    else if (strcmp(mode, "params-unknown") == 0)
        params_unknown();
    else if (strcmp(mode, "temp") == 0)
        temp();
    else if (strcmp(mode, "download") == 0)
        download();
    else if (strcmp(mode, "events") == 0)
        events();
    else if (strcmp(mode, "users") == 0)
        users();
    else if (strcmp(mode, "playgo") == 0 || strcmp(mode, "playgo-dat") == 0)
        playgo(strcmp(mode, "playgo-dat") == 0);
    else if (strcmp(mode, "discmap") == 0)
        discmap();
    else if (strcmp(mode, "misc") == 0)
        misc();
    else {
        logf("fam_system: unknown mode %s", mode);
        return 2;
    }
    ext_boot_done();
    return g_failed;
}
