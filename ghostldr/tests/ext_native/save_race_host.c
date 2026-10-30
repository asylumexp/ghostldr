/*
 * save_race.c (the extension's save writer), built for this machine as
 * x86-64 (test_save_race.py), run against a fake console: save_race_os.c,
 * which keeps the files in a temporary folder and models SWRR's save service
 * as measured on a 13.60 PS5 (research/apollo, savedata-reach C1-RW).
 *
 *     save_race_host ROOT SCENARIO TITLE DIR SRC FP|- BACKUP|- [check|noexit]
 *     save_race_host ROOT SCENARIO TITLE DIR - FP - fsvsh|fsvsh-same
 *
 * fsvsh: a save tool's mount (hle_fsvsh.c, sceFsMountSaveData) of the save,
 * writes through the tool's own mount path (VMC0.card rewritten, cheat.txt
 * made), and its unmount; fsvsh-same: the same with nothing changed.
 *
 *     save_race_host ROOT SCENARIO SAVES USER STOP - - prescan
 *
 * prescan: the game selector's save scan (hle_fsvsh.c save_prescan) for USER
 * (hex), with the candidates in ROOT's /data and /app0 and the save service
 * answering SAVES ("CUSA00001=2,CUSA00002=0": saves per title, others none);
 * STOP > 0: the progress callback asks to stop after that many titles.
 * Prints "search <user> <title>" per question, "progress <done>/<total>".
 *
 * This side is the extension's world (ghost.h): the core API the writer
 * uses, faked, and the call.  The fake console is in save_race_os.c.
 */
#include "ghost_api.h"
#include "savedata_hooks.h"

/* save_race_os.c */
void *os_real(const char *name);
void os_log(const char *line);
int os_usleep(u32 us);
void *os_calloc(u64 n);
void os_free(void *p);
int os_vfmt(char *buf, u64 size, const char *f, __builtin_va_list ap);

struct kfuncs K;
const struct ghost_api *G;
static u32 g_reserved;

static void hlog(const char *f, ...)
{
    char line[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    os_vfmt(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    os_log(line);
}

static void *h_real(const char *name) { return os_real(name); }
static int h_getpid(void) { return 77; }
static void *h_real_in(const char *lib, const char *name) { return os_real(name); }
static void *h_page_alloc(size_t n) { return os_calloc(n); }
static void h_page_free(void *p, size_t n) { os_free(p); }
static void h_lock(volatile int *l)
{
    while (__sync_lock_test_and_set(l, 1))
        ;
}
static void h_unlock(volatile int *l) { __sync_lock_release(l); }
static const char *h_remap(const char *path, char *buf, size_t size) { return path; }
static int h_reserve(void) { return g_reserved++, 0; }
static void h_release(void) { g_reserved--; }

static int h_hook_add(const char *(*fn)(const char *, char *, size_t));
static int h_vfs_open(const char *p, int f, int m);
static s64 h_vfs_read(int fd, void *b, u64 n);
static int h_vfs_close(int fd);
static int h_vfs_getdents(int fd, void *b, int n);
static s64 h_vfs_lseek(int fd, s64 off, int whence);
static void *h_direct_alloc(size_t n, int prot, u64 fixed);
static void h_direct_free(void *p);

static const struct ghost_api g_api = {
    .version = GHOST_API_VERSION,
    .size = sizeof(struct ghost_api),
    .k = &K,
    .logf = (void *)hlog,
    .host_real = h_real,
    .host_real_in = h_real_in,
    .page_alloc = h_page_alloc,
    .page_free = h_page_free,
    .spin_lock = h_lock,
    .spin_unlock = h_unlock,
    .remap_path = h_remap,
    .gfs_slot_reserve = h_reserve,
    .gfs_slot_release = h_release,
    .remap_hook_add = h_hook_add,
    .vfs_open = h_vfs_open,
    .vfs_read = h_vfs_read,
    .vfs_close = h_vfs_close,
    .vfs_getdents = h_vfs_getdents,
    .vfs_lseek = h_vfs_lseek,
    .direct_alloc = h_direct_alloc,
    .direct_free = h_direct_free,
};

/* the host's own title is SWRR (EU); every other title is another's */
bool savedata_is_foreign(const char *tid) { return strcmp(tid, "CUSA03492") != 0 && strcmp(tid, "CUSA03474") != 0; }

/* hle_fsvsh.c is not built here; nothing else of the extension is called */
u64 ext_find(const char *lib, const char *nid) { return 0; }

static const struct ext_family *g_fsvsh;
void harness_family(const struct ext_family *f)
{
    if (strcmp(f->name, "fsvsh") == 0)
        g_fsvsh = f;
}

/* hle_fsvsh.c's other neighbours: the title (Apollo), the save data family's answers, the core's file layer */
static const char *g_fp;
static const char *g_title = "";
const char *title_ident(void) { return g_title; }
const char *title_path(const char *path, char *buf, size_t size) { return path; }
void ov_banner(const char *text, u32 seconds) { hlog("banner: %s", text); }
void ov_notice(volatile int *once, const char *text)
{
    if (!*once)
        *once = 1, hlog("banner: %s", text);
}
s32 title_user(void) { return 0x10000001; }
bool title_user_picked(void) { return false; }
static bool g_vfs; /* the title's files: the save scan and the keystone learning read them */
/* the keystone file the extension keeps (/data/ghostldr/keystones), when the test gives no fingerprint */
bool savedata_keystone(s32 user, const char *tid, u8 out[0x60])
{
    char p[96];
    int (*o)(const char *, int, int) = os_real("open");
    s64 (*r)(int, void *, u64) = os_real("read");
    int (*c)(int) = os_real("close");
    fmt(p, sizeof(p), "/data/ghostldr/keystones/%.9s.keystone", tid);
    int fd = g_vfs ? o(p, 0, 0) : -1;
    if (fd < 0)
        return false;
    s64 n = r(fd, out, 0x60);
    c(fd);
    return n == 0x60 && memcmp(out, "keystone", 8) == 0;
}
bool savedata_fingerprint(s32 user, const char *tid, char out[65])
{
    u8 ks[0x60];
    if (!g_fp && savedata_keystone(user, tid, ks)) {
        static const char hex[] = "0123456789abcdef";
        for (u32 k = 0; k < 32; k++)
            out[2 * k] = hex[ks[0x20 + k] >> 4], out[2 * k + 1] = hex[ks[0x20 + k] & 15];
        out[64] = 0;
        return true;
    }
    if (!g_fp)
        return false;
    strlcpy(out, g_fp, 65);
    return true;
}
s32 savedata_foreign_mount(s32 user, const char *tid, const char *dir, const char *fp, char mp[16])
{
    return (s32)0x809F000B;
}
s32 savedata_umount(const char *mp) { return 0; }
void savedata_data_release(void) {} /* the prescan lets go of /data (nothing mounted here) */
static const char *g_saves; /* prescan: "CUSA00001=2,..." */
s32 savedata_foreign_search(s32 user, const char *tid, char (*dirs)[32], struct sd_param *params, u64 *blocks,
                            u32 max, u32 *hits)
{
    if (!g_saves)
        return (s32)0x809F0008;
    if (!savedata_is_foreign(tid)) /* the host's own: refused unasked, as hle_savedata.c does */
        return (s32)0x809F0000;
    hlog("search %08x %s", (u32)user, tid);
    u32 n = 0;
    const char *at = strstr(g_saves, tid);
    for (const char *d = at && at[9] == '=' ? at + 10 : ""; *d >= '0' && *d <= '9'; d++)
        n = n * 10 + (u32)(*d - '0');
    for (u32 i = 0; i < n && i < max; i++) {
        memset(dirs[i], 0, 32);
        fmt(dirs[i], 32, "SAVE%02u", i);
        if (params)
            memset(&params[i], 0, sizeof(params[i])), strlcpy(params[i].title, "a save", 128);
        if (blocks)
            blocks[i] = 96;
    }
    *hits = n;
    return 0;
}
static const char *(*g_hook)(const char *, char *, size_t);
static int h_hook_add(const char *(*fn)(const char *, char *, size_t)) { return g_hook = fn, 0; }
static int h_vfs_open(const char *p, int f, int m)
{
    int (*o)(const char *, int, int) = os_real("open");
    return g_vfs ? o(p, f, m) : -1;
}
static s64 h_vfs_read(int fd, void *b, u64 n)
{
    s64 (*r)(int, void *, u64) = os_real("read");
    return g_vfs ? r(fd, b, n) : -1;
}
static int h_vfs_close(int fd)
{
    int (*c)(int) = os_real("close");
    return g_vfs ? c(fd) : 0;
}
static int h_vfs_getdents(int fd, void *b, int n)
{
    int (*g)(int, void *, int) = os_real("getdents");
    return g_vfs ? g(fd, b, n) : -1;
}
static s64 h_vfs_lseek(int fd, s64 off, int whence)
{
    s64 (*l)(int, s64, int) = os_real("lseek");
    return g_vfs ? l(fd, off, whence) : -1;
}
static void *h_direct_alloc(size_t n, int prot, u64 fixed) { return os_calloc(n); }
static void h_direct_free(void *p) { os_free(p); }

/* save_race_os.c: a file of the fake console written whole */
int os_put(const char *console_path, const void *data, u64 n);

static void *fam_fn(const char *name)
{
    for (u32 i = 0; g_fsvsh && i < g_fsvsh->nhle; i++)
        if (g_fsvsh->hle[i].name && strcmp(g_fsvsh->hle[i].name, name) == 0)
            return g_fsvsh->hle[i].fn;
    return NULL;
}

int host_fsvsh(const char *fp, int change)
{
    G = &g_api;
    g_vfs = change >= 2; /* fsvsh-learn(-ro): no fingerprint given, the files of the fake console's /data and USB */
    K.usleep = os_usleep;
    K.getpid = h_getpid;
    g_fp = fp;
    g_title = "APOL00004";
    if (!g_fsvsh || !g_fsvsh->init || g_fsvsh->init() != 0 || !g_hook) {
        hlog("RESULT rc=ffffffff why=the fsvsh family did not start");
        return 0;
    }
    s32 (*mount)(const void *, const char *, const char *, const u8 *) = fam_fn("sceFsMountSaveData");
    s32 (*umount)(const void *, const char *, s32, u32) = fam_fn("sceFsUmountSaveData");
    u8 opt[16] = {0}, key[32] = {0};
    const char *mp = "/data/apollo/mount/SCUS-97429";
    s32 rc = mount(opt, "/user/home/10000000/savedata/CUSA07992/sdimg_SCUS-97429", mp, key);
    hlog("fsvsh: mount -> %x", (u32)rc);
    if (rc == 0 && change && change != 3) {
        char buf[320];
        const char *h = g_hook("/data/apollo/mount/SCUS-97429/VMC0.card", buf, sizeof(buf));
        static u8 card[70000];
        memset(card, 0x5A, sizeof(card));
        os_put(h, card, sizeof(card));
        h = g_hook("/data/apollo/mount/SCUS-97429/cheat.txt", buf, sizeof(buf));
        os_put(h, "infinite boost", 14);
        h = g_hook("/data/apollo/mount/SCUS-97429/sce_sys/param.sfo", buf, sizeof(buf));
        os_put(h, "resigned by the tool", 20);   /* never written into the save */
    }
    s32 urc = rc == 0 ? umount(opt, mp, 0, 0) : rc;
    hlog("RESULT rc=%x mount=%x why=", (u32)urc, (u32)rc);
    return 0;
}

int host_run(const char *tid, const char *dir, const char *src, const char *fp, const char *backup, int check)
{
    G = &g_api;
    K.usleep = os_usleep;
    K.getpid = h_getpid;
    struct save_race_result r;
    s32 rc = check ? save_race_check(-1, tid, dir, src, fp, &r) : save_race_write(-1, tid, dir, src, fp, backup, &r);
    hlog("RESULT rc=%x mount=%x slot=%u written=%u created=%u unchanged=%u deleted=%u dirs=%u bytes=%lu bad=%u "
         "extra=%u committed=%d needs_exit=%d reserved=%u why=%s",
         (u32)rc, (u32)r.mount_rc, r.slot, r.written, r.created, r.unchanged, r.deleted, r.dirs_made, r.bytes,
         r.verify_bad, r.extra, r.committed, r.needs_exit, g_reserved, r.why);
    return 0;
}

/* the read-only copy through a refused mount (save_race_copy) into dst; prints what it returned */
int host_copy(const char *tid, const char *dir, const char *dst)
{
    G = &g_api;
    K.usleep = os_usleep;
    K.getpid = h_getpid;
    s32 rc = save_race_copy(-1, tid, dir, dst);
    hlog("RESULT rc=%x why=", (u32)rc);
    return 0;
}

/* ---- the game selector's save scan (prescan) ---- */
int save_prescan(u32 user, int (*progress)(void *arg, u32 done, u32 total), void *arg);

static int scan_progress(void *arg, u32 done, u32 total)
{
    u32 stop = *(u32 *)arg;
    hlog("progress %u/%u", done, total);
    return stop && done >= stop;
}

int host_prescan(const char *saves, u32 user, u32 stop)
{
    G = &g_api;
    K.usleep = os_usleep;
    K.getpid = h_getpid;
    g_title = "APOL00004";
    g_saves = saves;
    g_vfs = true;
    int rc = save_prescan(user, scan_progress, &stop);
    hlog("RESULT with=%d", rc);
    return 0;
}
