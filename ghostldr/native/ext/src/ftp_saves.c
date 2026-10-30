/*
 * The FTP server's /saves (Settings > Advanced "Saves in FTP",
 * CTX_FLAG_FTP_SAVES; docs/ftp.md "Saves"): the saves of every user logged
 * in to the console, read-only, through the real save service.
 *
 *     /saves/<user>/<TITLE>/<DIR>/...
 *
 * <user> is the user's name (their ID in hex when the name isn't plain),
 * <TITLE> a title they have saves of, <DIR> one of its save directories.  A
 * save is mounted only while a client uses it (a listing, a download), at
 * most MOUNTS at once, and unmounted IDLE_MS after its last use: the save
 * service's mount slots are shared with SWRR, ghostfs and the title.
 *
 * What a game may do decides what shows (hle_savedata.c, "other titles'
 * saves"):
 *   - SWRR's own saves (Luac0re's, ghostldr's titles' <prefix>...SAVE
 *     containers) are listed and mounted as SWRR's.  ghostldr's other
 *     containers (a title's install data and index, GLDRCONF, GLDRCACHE,
 *     ...) are no saves and are left out: the core mounts them as a game is
 *     picked, and one held here would stop it.  While a title runs, its own
 *     containers are refused too, and SWRR's saves are unmounted
 *     OWN_IDLE_MS after their last use: a mount held here would make the
 *     title's own fail.
 *   - Another title's saves are listed only when asked for by exact title
 *     ID, so only titles something names are asked about: the other exploit
 *     titles, save folders on USB drives (PS4/APOLLO, PS4/SAVEDATA), what a
 *     save tool run under ghostldr kept in its /data (<prefix>D000SAVE:
 *     ghostldr/savesearch.txt, titles.txt, keystones), and any title ID a
 *     client makes a folder for (MKD /saves/<user>/CUSA12345, this session).
 *     Asking takes about 0.4 s a title, so it runs on a thread of its own
 *     from the first listing of /saves.
 *   - Another title's save is read and written as a save tool under ghostldr
 *     reads and writes it ("views" below: a mount with the title's
 *     fingerprint, from a keystone found in those same places, else a race
 *     copy; writes through a writable copy and save_race_write).
 *
 * SWRR's own saves are never written (EROFS), nor any save's sce_sys.
 */
#include "ext.h"
#include "ftp.h"
#include "savedata_hooks.h"

#define ROOT "/saves"
#define ROOT_LEN 6
#define USERS 4
#define TITLES 512 /* titles with saves, all users */
#define HINTS 512  /* titles to ask about */
#define FPS 256    /* fingerprints */
#define DIRS 256   /* a title's saves */
#define MOUNTS 3
#ifndef IDLE_MS /* the tests set shorter ones */
#define IDLE_MS 10000
#endif
#ifndef OWN_IDLE_MS
#define OWN_IDLE_MS 1500 /* SWRR's own: the core may want them for the game picked next */
#endif
#define FDS 64
#define CACHE 4
#define COPIES 16 /* race and writable copies, kept for the session */
#ifndef COMMIT_MS
#define COMMIT_MS 3000 /* a writable copy goes into its save this long after its last change */
#endif
#ifndef WAIT_MS
#define WAIT_MS 25000 /* a user's listing waits this long for its titles */
#endif

#define MODE_RDONLY 1u
#define USER_ANY (-2)
#define PATH_MAX_ 1100
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* ---- the save service's structs (hle_savedata.c has them all) ---- */
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
    u32 unused, mount_status;
    u8 reserved[28];
};
struct sd_search_cond { /* SceSaveDataDirNameSearchCond */
    s32 user;
    u32 pad;
    const char *title_id;
    const char *dir;
    u32 key, order;
    u8 reserved[32];
};
struct sd_search_result { /* SceSaveDataDirNameSearchResult */
    u32 hit_num, pad;
    char *dirs;
    u32 dirs_num, set_num;
    struct sd_param *params;
    void *infos;
    u8 reserved[12];
};
_Static_assert(sizeof(struct sd_mount1) == 0x50, "SceSaveDataMount");
_Static_assert(sizeof(struct sd_mount_result) == 0x40, "SceSaveDataMountResult");
_Static_assert(sizeof(struct sd_search_cond) == 0x40, "SceSaveDataDirNameSearchCond");
_Static_assert(sizeof(struct sd_search_result) == 0x38, "SceSaveDataDirNameSearchResult");

static struct {
    s32 (*search)(const struct sd_search_cond *, struct sd_search_result *);
    s32 (*mount)(const struct sd_mount1 *, struct sd_mount_result *);
    s32 (*umount)(const char *);
    s32 (*login)(s32 *);
    s32 (*initial)(s32 *);
    s32 (*name)(s32, char *, u64);
    s32 (*getdents)(int, void *, int);
    s32 (*app_info)(s32, u8 *);
    s32 (*mkdir)(const char *, int);
    s32 (*rmdir)(const char *);
    s32 (*unlink)(const char *);
    s32 (*rename)(const char *, const char *);
} R;

static bool ready(void)
{
    if (!R.search) {
        R.mount = host_real_in("libSceSaveData", "sceSaveDataMount");
        R.umount = host_real_in("libSceSaveData", "sceSaveDataUmount");
        R.login = host_real_in("libSceUserService", "sceUserServiceGetLoginUserIdList");
        R.initial = host_real_in("libSceUserService", "sceUserServiceGetInitialUser");
        R.name = host_real_in("libSceUserService", "sceUserServiceGetUserName");
        R.getdents = host_real("sceKernelGetdents");
        R.app_info = host_real("sceKernelGetAppInfo");
        R.mkdir = host_real("sceKernelMkdir");
        R.rmdir = host_real("sceKernelRmdir");
        R.unlink = host_real("sceKernelUnlink");
        R.rename = host_real("sceKernelRename");
        R.search = host_real_in("libSceSaveData", "sceSaveDataDirNameSearch");
    }
    return R.search && R.mount && R.umount && R.login && R.getdents && R.mkdir && R.rmdir && R.unlink && R.rename;
}

/* a sleeping lock: holders may spend 100+ ms in a mount */
static volatile int g_lock;
static void lock(void)
{
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE))
        K.usleep(1000);
}
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

/* ---- what is known ---- */

struct user {
    s32 id;
    char name[40]; /* its folder */
};
static struct user g_users[USERS];
static u32 g_nusers;

struct title {
    s32 user;
    char id[10];
    bool own; /* SWRR's */
    u16 saves;
    long mtime; /* its newest save's */
};
static struct title *g_title;
static u32 g_ntitle;

struct hint {
    s32 user; /* USER_ANY: every user's */
    char id[10];
    u8 asked; /* bit per g_users slot */
};
static struct hint *g_hint;
static u32 g_nhint;

struct fpr {
    char id[10];
    char fp[65];
};
static struct fpr *g_fp;
static u32 g_nfp;

/* a title's saves as last listed */
struct listing {
    s32 user;
    char id[10];
    long at; /* fv_clock_ms */
    u32 n;
    struct {
        char dir[33];
        long mtime;
    } e[DIRS];
};
static struct listing *g_cache[CACHE];

static char g_host[16];      /* SWRR's title ID */
static char g_busy_prefix[16]; /* the running title's containers' prefix: "" none */
static bool g_title_runs;
static volatile int g_scan; /* 0 never, 1 running, 2 done */
static volatile int g_stop;
static u8 g_scanned;        /* bit per g_users slot */
static bool g_hints_read;

static bool title_like(const char *s)
{
    for (int k = 0; k < 4; k++)
        if (s[k] < 'A' || s[k] > 'Z')
            return false;
    for (int k = 4; k < 9; k++)
        if (s[k] < '0' || s[k] > '9')
            return false;
    return true;
}

/* the titles ghostldr runs from (SWRR US and EU, Jak X): their G... containers are ghostldr's, whichever ran it */
static bool exploit_title(const char *tid)
{
    return strncmp(tid, "CUSA03474", 9) == 0 || strncmp(tid, "CUSA03492", 9) == 0 || strncmp(tid, "CUSA07992", 9) == 0;
}

/* SWRR's saves, either region's: never written (save_race.c refuses them too) */
static bool swrr_title(const char *tid) { return strncmp(tid, "CUSA03474", 9) == 0 || strncmp(tid, "CUSA03492", 9) == 0; }

static const char *host_title(void)
{
    if (!g_host[0] && R.app_info) {
        u8 info[0x80];
        memset(info, 0, sizeof(info));
        if (R.app_info(K.getpid(), info) == 0 && info[0x10])
            strlcpy(g_host, (const char *)info + 0x10, 10);
    }
    return g_host;
}

/* the logged-in users and their folders' names, asked at most every 2 s (lock held) */
static long g_users_at = -1;
static void users_load(void)
{
    long now = fv_clock_ms();
    if (g_users_at >= 0 && now - g_users_at < 2000)
        return;
    g_users_at = now;
    s32 list[USERS] = {-1, -1, -1, -1};
    if (R.login(list) != 0 && R.initial)
        R.initial(&list[0]);
    struct user was[USERS];
    u32 nwas = g_nusers;
    memcpy(was, g_users, sizeof(was));
    g_nusers = 0;
    u8 scanned = 0;
    for (u32 i = 0; i < USERS; i++) {
        if (list[i] == -1)
            continue;
        struct user *u = &g_users[g_nusers];
        u->id = list[i];
        char nm[32];
        memset(nm, 0, sizeof(nm));
        bool plain = R.name && R.name(list[i], nm, 17) == 0 && nm[0];
        for (u32 k = 0; plain && nm[k]; k++)
            plain = (nm[k] >= 'a' && nm[k] <= 'z') || (nm[k] >= 'A' && nm[k] <= 'Z') ||
                    (nm[k] >= '0' && nm[k] <= '9') || nm[k] == '_' || nm[k] == '-' || nm[k] == '.';
        if (plain)
            strlcpy(u->name, nm, sizeof(u->name));
        else
            fmt(u->name, sizeof(u->name), "%08x", (u32)list[i]);
        for (u32 k = 0; k < g_nusers; k++)
            if (strcmp(g_users[k].name, u->name) == 0)
                fmt(u->name + strlen(u->name), sizeof(u->name) - strlen(u->name), "_%08x", (u32)list[i]);
        for (u32 k = 0; k < nwas; k++) /* still scanned */
            if (was[k].id == u->id && (g_scanned >> k & 1))
                scanned |= (u8)(1u << g_nusers);
        g_nusers++;
    }
    /* a hint's asked bits follow their users */
    for (u32 h = 0; h < g_nhint; h++) {
        u8 a = 0;
        for (u32 k = 0; k < nwas; k++)
            for (u32 j = 0; j < g_nusers; j++)
                if ((g_hint[h].asked >> k & 1) && was[k].id == g_users[j].id)
                    a |= (u8)(1u << j);
        g_hint[h].asked = a;
    }
    g_scanned = scanned;
}

static s32 user_slot(s32 id)
{
    for (u32 k = 0; k < g_nusers; k++)
        if (g_users[k].id == id)
            return (s32)k;
    return -1;
}

static bool alloc_tables(void)
{
    if (!g_title)
        g_title = page_alloc(sizeof(*g_title) * TITLES);
    if (!g_hint)
        g_hint = page_alloc(sizeof(*g_hint) * HINTS);
    if (!g_fp)
        g_fp = page_alloc(sizeof(*g_fp) * FPS);
    return g_title && g_hint && g_fp;
}

/* lock held */
static void hint_add(s32 user, const char *id)
{
    if (!title_like(id) || (host_title()[0] && strncmp(id, g_host, 9) == 0))
        return;
    for (u32 i = 0; i < g_nhint; i++)
        if (memcmp(g_hint[i].id, id, 9) == 0 && (g_hint[i].user == user || g_hint[i].user == USER_ANY))
            return;
    if (g_nhint < HINTS) {
        struct hint *h = &g_hint[g_nhint++];
        h->user = user, h->asked = 0;
        memcpy(h->id, id, 9), h->id[9] = 0;
    }
}

static void fp_add(const char *id, const u8 *ks, s64 n)
{
    if (n != 0x60 || memcmp(ks, "keystone", 8) != 0 || !title_like(id))
        return;
    for (u32 i = 0; i < g_nfp; i++)
        if (memcmp(g_fp[i].id, id, 9) == 0)
            return;
    if (g_nfp >= FPS)
        return;
    struct fpr *f = &g_fp[g_nfp++];
    static const char hex[] = "0123456789abcdef";
    memcpy(f->id, id, 9), f->id[9] = 0;
    for (u32 k = 0; k < 32; k++)
        f->fp[2 * k] = hex[ks[0x20 + k] >> 4], f->fp[2 * k + 1] = hex[ks[0x20 + k] & 15];
    f->fp[64] = 0;
}

static const char *fp_of(const char *id)
{
    for (u32 i = 0; i < g_nfp; i++)
        if (memcmp(g_fp[i].id, id, 9) == 0)
            return g_fp[i].fp;
    return NULL;
}

/* ---- the save service ---- */

static s32 search(s32 user, const char *tid, char (*dirs)[32], struct sd_param *params, u32 max, u32 *hits)
{
    char t[16];
    memset(t, 0, sizeof(t));
    if (tid)
        strlcpy(t, tid, 10);
    struct sd_search_cond c;
    struct sd_search_result r;
    memset(&c, 0, sizeof(c)), memset(&r, 0, sizeof(r));
    c.user = user, c.title_id = tid ? t : NULL;
    r.dirs = (char *)dirs, r.dirs_num = max, r.params = params;
    savedata_search_lock(); /* one question at a time, whoever asks: eight at once froze SWRR */
    s32 rc = R.search(&c, &r);
    savedata_search_unlock();
    *hits = rc == 0 ? MIN(params ? MIN(r.set_num, r.hit_num) : r.hit_num, max) : 0;
    return rc;
}

/* a title's saves for user, into a listing (lock not held): 0, or < 0 */
static s32 list_title(s32 user, const char *tid, bool own, struct listing *l)
{
    char (*dirs)[32] = page_alloc(32 * DIRS);
    struct sd_param *params = page_alloc(sizeof(struct sd_param) * DIRS); /* only for the times: may be NULL */
    u32 n = 0;
    s32 rc = dirs ? search(user, own ? NULL : tid, dirs, params, DIRS, &n) : -1;
    l->user = user, l->n = 0, l->at = fv_clock_ms();
    memcpy(l->id, tid, 9), l->id[9] = 0;
    for (u32 i = 0; rc == 0 && i < n; i++) {
        memcpy(l->e[l->n].dir, dirs[i], 32);
        l->e[l->n].dir[32] = 0;
        const char *d = l->e[l->n].dir;
        size_t len = strlen(d);
        if (!d[0] || strrchr_c(d, '/'))
            continue;
        if ((own || exploit_title(tid)) && d[0] == 'G' && (len < 4 || strcmp(d + len - 4, "SAVE") != 0))
            continue; /* ghostldr's install data, index, settings, cache: no saves */
        l->e[l->n++].mtime = params ? params[i].mtime : 0;
    }
    if (dirs)
        page_free(dirs, 32 * DIRS);
    if (params)
        page_free(params, sizeof(struct sd_param) * DIRS);
    return rc;
}

/* user's entry for tid with what a listing found (lock held) */
static void title_note(s32 user, const char *tid, bool own, const struct listing *l)
{
    struct title *t = NULL;
    for (u32 i = 0; i < g_ntitle && !t; i++)
        if (g_title[i].user == user && memcmp(g_title[i].id, tid, 9) == 0)
            t = &g_title[i];
    if (!l->n) {
        if (t) /* gone: the last of it */
            *t = g_title[--g_ntitle];
        return;
    }
    if (!t && g_ntitle < TITLES)
        t = &g_title[g_ntitle++];
    if (!t)
        return;
    t->user = user, t->own = own, t->saves = (u16)MIN(l->n, 0xFFFFu), t->mtime = 0;
    memcpy(t->id, tid, 9), t->id[9] = 0;
    for (u32 i = 0; i < l->n; i++)
        t->mtime = MAX(t->mtime, l->e[i].mtime);
}

/* ---- where titles are named ---- */

static s64 read_small(const char *path, void *buf, u64 cap)
{
    int fd = K.open(path, 0, 0);
    if (fd < 0)
        return -1;
    s64 n = K.read(fd, buf, cap);
    K.close(fd);
    return n;
}

/* each entry of a directory: fn(arg, name, is_dir) */
static void each(const char *dir, void (*fn)(void *, const char *, bool), void *arg)
{
    int fd = K.open(dir, 0x20000 /* O_DIRECTORY */, 0);
    if (fd < 0)
        return;
    u8 *d = page_alloc(8192);
    for (int got; d && (got = R.getdents(fd, d, 8192)) > 0;)
        for (int off = 0; off + 8 <= got;) {
            u16 reclen = *(u16 *)(d + off + 4);
            u8 type = d[off + 6], nlen = d[off + 7];
            if (!reclen)
                break;
            char name[256];
            memcpy(name, d + off + 8, nlen);
            name[nlen] = 0;
            off += reclen;
            if (name[0] != '.')
                fn(arg, name, type == 4);
        }
    if (d)
        page_free(d, 8192);
    K.close(fd);
}

struct walk {
    const char *dir;
    s32 user;
};

/* a param.sfo's TITLE_ID */
static bool sfo_title(const u8 *d, s64 n, char out[10])
{
    if (n < 0x14 || memcmp(d, "\0PSF", 4) != 0)
        return false;
    u32 kt = *(const u32 *)(d + 8), dt = *(const u32 *)(d + 12), count = *(const u32 *)(d + 16);
    for (u32 i = 0; i < count && 0x14 + 16 * (i + 1) <= (u64)n; i++) {
        const u8 *e = d + 0x14 + 16 * i;
        u32 ko = kt + *(const u16 *)e, off = dt + *(const u32 *)(e + 12);
        if (ko + 9 <= (u64)n && memcmp(d + ko, "TITLE_ID", 9) == 0 && off + 9 <= (u64)n && title_like((const char *)d + off)) {
            memcpy(out, d + off, 9), out[9] = 0;
            return true;
        }
    }
    return false;
}

/* PS4/APOLLO/<name>: a decrypted save a save tool exported: its title, and its keystone */
static void on_apollo(void *arg, const char *name, bool is_dir)
{
    const struct walk *w = arg;
    if (!is_dir)
        return;
    char p[400], id[10];
    u8 *buf = page_alloc(0x4000);
    if (!buf)
        return;
    fmt(p, sizeof(p), "%s/%s/sce_sys/param.sfo", w->dir, name);
    bool got = sfo_title(buf, read_small(p, buf, 0x4000), id);
    for (u32 k = 0; !got && name[k]; k++) /* <user>_<TITLE>_<DIR> */
        if (name[k] == '_' && strlen(name + k + 1) >= 9 && title_like(name + k + 1))
            memcpy(id, name + k + 1, 9), id[9] = 0, got = true;
    if (got) {
        hint_add(USER_ANY, id);
        fmt(p, sizeof(p), "%s/%s/sce_sys/keystone", w->dir, name);
        fp_add(id, buf, read_small(p, buf, 0x61));
    }
    page_free(buf, 0x4000);
}

/* PS4/SAVEDATA/<account>/<TITLE>: the system's copies */
static void on_titles(void *arg, const char *name, bool is_dir)
{
    if (is_dir && strlen(name) >= 9 && title_like(name))
        hint_add(((const struct walk *)arg)->user, name);
}

static void on_account(void *arg, const char *name, bool is_dir)
{
    const struct walk *w = arg;
    char p[400];
    fmt(p, sizeof(p), "%s/%s", w->dir, name);
    struct walk sub = {p, USER_ANY};
    if (is_dir && strlen(name) == 16)
        each(p, on_titles, &sub);
}

/* <TITLE>.keystone files */
static void on_keystone(void *arg, const char *name, bool is_dir)
{
    const struct walk *w = arg;
    u8 ks[0x61];
    char p[400];
    if (is_dir || strlen(name) != 18 || strcmp(name + 9, ".keystone") != 0 || !title_like(name))
        return;
    fmt(p, sizeof(p), "%s/%s", w->dir, name);
    hint_add(USER_ANY, name);
    fp_add(name, ks, read_small(p, ks, sizeof(ks)));
}

/* apollo/<user hex>/<TITLE>/keystone */
static void on_apollo_title(void *arg, const char *name, bool is_dir)
{
    const struct walk *w = arg;
    u8 ks[0x61];
    char p[400];
    if (!is_dir || strlen(name) != 9 || !title_like(name))
        return;
    fmt(p, sizeof(p), "%s/%s/keystone", w->dir, name);
    hint_add(w->user, name);
    fp_add(name, ks, read_small(p, ks, sizeof(ks)));
}

static void on_apollo_user(void *arg, const char *name, bool is_dir)
{
    const struct walk *w = arg;
    char p[400];
    u32 user = 0;
    if (!is_dir || strlen(name) != 8)
        return;
    for (u32 k = 0; k < 8; k++) {
        char c = name[k];
        u32 d = c >= '0' && c <= '9' ? (u32)(c - '0') : c >= 'a' && c <= 'f' ? (u32)(c - 'a' + 10) : 99;
        if (d > 15)
            return;
        user = user << 4 | d;
    }
    fmt(p, sizeof(p), "%s/%s", w->dir, name);
    struct walk sub = {p, (s32)user};
    each(p, on_apollo_title, &sub);
}

/* "<user hex> <TITLE> <hits> <time>" lines (hle_fsvsh.c's savesearch.txt), or any text naming title IDs */
static void hints_in_text(const char *path, bool cache)
{
    u64 cap = 64 << 10;
    char *t = page_alloc(cap);
    s64 n = t ? read_small(path, t, cap - 1) : -1;
    for (s64 i = 0; n > 0 && i < n;) {
        s64 end = i;
        while (end < n && t[end] != '\n')
            end++;
        if (cache && end - i > 20 && t[i + 8] == ' ' && title_like(t + i + 9) && t[i + 18] == ' ') {
            u32 user = 0;
            for (u32 k = 0; k < 8; k++) {
                char c = t[i + k];
                user = user << 4 | (u32)(c >= 'a' ? c - 'a' + 10 : c - '0');
            }
            if (t[i + 19] != '0') /* it had saves */
                hint_add((s32)user, t + i + 9);
        } else if (!cache)
            for (s64 k = i; k + 9 <= end; k++)
                if ((k == i || !(t[k - 1] >= 'A' && t[k - 1] <= 'Z')) && title_like(t + k))
                    hint_add(USER_ANY, t + k), k += 8;
        i = end + 1;
    }
    if (t)
        page_free(t, cap);
}

/* what a save tool kept in its /data: <prefix>D000SAVE, SWRR's, of SWRR's user */
static void hints_in_data(void)
{
    s32 owner = -1;
    if (!R.initial || R.initial(&owner) != 0)
        return;
    struct listing *l = page_alloc(sizeof(*l));
    if (!l)
        return;
    if (host_title()[0])
        list_title(owner, host_title(), true, l);
    for (u32 i = 0; i < l->n && !g_stop; i++) {
        const char *dir = l->e[i].dir;
        size_t len = strlen(dir);
        if (len < 8 || strcmp(dir + len - 8, "D000SAVE") != 0)
            continue;
        if (g_busy_prefix[0] && strncmp(dir, g_busy_prefix, strlen(g_busy_prefix)) == 0)
            continue; /* the running title's /data: mounted read-write for it */
        char mp[16], p[96];
        if (gfs_slot_reserve() != 0)
            break;
        s32 rc = gfs_mount_user(owner, dir, MODE_RDONLY, 0, mp, NULL);
        if (rc != 0) {
            gfs_slot_release();
            logf("ftp: saves: %s (a title's /data) -> %x", dir, (u32)rc);
            continue;
        }
        lock();
        fmt(p, sizeof(p), "%s/ghostldr/savesearch.txt", mp);
        hints_in_text(p, true);
        fmt(p, sizeof(p), "%s/ghostldr/titles.txt", mp);
        hints_in_text(p, false);
        fmt(p, sizeof(p), "%s/ghostldr/keystones", mp);
        struct walk w = {p, USER_ANY};
        each(p, on_keystone, &w);
        fmt(p, sizeof(p), "%s/apollo/data", mp);
        each(p, on_keystone, &w);
        fmt(p, sizeof(p), "%s/apollo", mp);
        each(p, on_apollo_user, &w);
        unlock();
        R.umount(mp);
        gfs_slot_release();
    }
    page_free(l, sizeof(*l));
}

const char *usb_slot_mp(u32 k) HIDDEN;

/* every source but the D000SAVEs, read again at each scan: a drive may have come */
static void hints_read(void)
{
    static const char *const k_exploits[] = {"CUSA03474", "CUSA03492", "CUSA07992"};
    lock();
    for (u32 i = 0; i < ARRAY_SIZE(k_exploits); i++)
        hint_add(USER_ANY, k_exploits[i]);
    for (u32 k = 0; k < 4; k++) {
        char p[64];
        fmt(p, sizeof(p), "%s/PS4/APOLLO", usb_slot_mp(k));
        struct walk w = {p, USER_ANY};
        each(p, on_apollo, &w);
        fmt(p, sizeof(p), "%s/PS4/SAVEDATA", usb_slot_mp(k));
        each(p, on_account, &w);
        fmt(p, sizeof(p), "%s/ghostldr/keystones", usb_slot_mp(k));
        each(p, on_keystone, &w);
    }
    unlock();
    if (!g_hints_read) {
        g_hints_read = true;
        hints_in_data();
    }
}

/* ---- the scan: every logged-in user's titles ---- */

static void scan_user(u32 slot, s32 user)
{
    struct listing *l = page_alloc(sizeof(*l));
    if (!l)
        return;
    u32 asked = 0, with = 0;
    long t0 = fv_clock_ms();
    if (host_title()[0] && list_title(user, host_title(), true, l) == 0) {
        lock();
        title_note(user, host_title(), true, l);
        unlock();
        with += l->n > 0;
    }
    for (u32 i = 0; !g_stop; i++) {
        char id[10];
        lock();
        bool more = i < g_nhint;
        bool ask = more && !(g_hint[i].asked >> slot & 1) && (g_hint[i].user == USER_ANY || g_hint[i].user == user);
        if (more) {
            g_hint[i].asked |= (u8)(1u << slot);
            memcpy(id, g_hint[i].id, 10);
        }
        unlock();
        if (!more)
            break;
        if (!ask)
            continue;
        asked++;
        if (list_title(user, id, false, l) != 0)
            continue;
        lock();
        title_note(user, id, false, l);
        unlock();
        with += l->n > 0;
    }
    page_free(l, sizeof(*l));
    logf("ftp: saves: user %08x: %u titles asked about in %ld ms, %u with saves", (u32)user, asked + 1,
         fv_clock_ms() - t0, with);
}

static void *scan_thread(void *arg)
{
    (void)arg;
    hints_read();
    for (;;) {
        lock();
        users_load();
        s32 user = -1, slot = -1;
        for (u32 k = 0; k < g_nusers && slot < 0; k++)
            if (!(g_scanned >> k & 1))
                slot = (s32)k, user = g_users[k].id;
        unlock();
        if (slot < 0 || g_stop)
            break;
        scan_user((u32)slot, user);
        lock();
        if (user_slot(user) >= 0) /* by ID: the list may have changed meanwhile */
            g_scanned |= (u8)(1u << user_slot(user));
        unlock();
    }
    __atomic_store_n(&g_scan, 2, __ATOMIC_RELEASE);
    return NULL;
}

/* the scan, unless it is running; a finished one starts over for users new since */
static void scan_start(void)
{
    int st = __atomic_load_n(&g_scan, __ATOMIC_ACQUIRE);
    if (st == 1 || g_stop)
        return;
    lock();
    users_load();
    bool all = (g_scanned & ((1u << g_nusers) - 1)) == (1u << g_nusers) - 1;
    unlock();
    if (st == 2 && all)
        return;
    if (!__atomic_compare_exchange_n(&g_scan, &st, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    if (fv_thread(scan_thread, NULL, "ghostldr-ftpsaves") != 0)
        __atomic_store_n(&g_scan, st, __ATOMIC_RELEASE);
}

/* ---- paths ---- */

struct where {
    int level; /* 0 /saves, 1 a user, 2 a title, 3 a save, 4 inside it */
    s32 user;
    char tid[10];
    char dir[33];
    const char *rest; /* level 4: "/..." below the save */
};

static bool part(const char **p, char *out, size_t cap)
{
    const char *s = *p;
    if (*s != '/' || !s[1])
        return false;
    s++;
    size_t n = 0;
    while (s[n] && s[n] != '/')
        n++;
    if (n + 1 > cap)
        return false;
    memcpy(out, s, n);
    out[n] = 0;
    *p = s + n;
    return true;
}

/* path below /saves: 0, or -ENOENT (lock held) */
static int where(const char *path, struct where *w)
{
    memset(w, 0, sizeof(*w));
    const char *p = path + ROOT_LEN;
    char name[64];
    if (!part(&p, name, sizeof(name)))
        return 0;
    w->level = 1, w->user = -1;
    for (u32 k = 0; k < g_nusers; k++)
        if (strcmp(g_users[k].name, name) == 0)
            w->user = g_users[k].id;
    if (w->user == -1)
        return -FV_ENOENT;
    if (!part(&p, name, sizeof(name)))
        return 0;
    if (strlen(name) != 9 || !title_like(name))
        return -FV_ENOENT;
    w->level = 2;
    memcpy(w->tid, name, 10);
    if (!part(&p, w->dir, sizeof(w->dir)))
        return 0;
    w->level = 3;
    if (*p)
        w->level = 4, w->rest = p;
    return 0;
}

static struct title *title_find(s32 user, const char *tid)
{
    for (u32 i = 0; i < g_ntitle; i++)
        if (g_title[i].user == user && memcmp(g_title[i].id, tid, 9) == 0)
            return &g_title[i];
    return NULL;
}

/* user's titles known (its scan done), waiting at most WAIT_MS (lock held, dropped while waiting) */
static void scan_wait(s32 user)
{
    if (user_slot(user) < 0 || (g_scanned >> user_slot(user) & 1))
        return;
    unlock();
    scan_start();
    for (long t0 = fv_clock_ms(); fv_clock_ms() - t0 < WAIT_MS && !g_stop && __atomic_load_n(&g_scan, __ATOMIC_ACQUIRE) == 1;) {
        lock();
        bool done = user_slot(user) >= 0 && (g_scanned >> user_slot(user) & 1);
        unlock();
        if (done)
            break;
        fv_sleep_ms(100);
    }
    lock();
}

/* user's entry for tid: known, or asked about now (a path straight to a title the scan didn't find or hasn't
 * reached; lock held, dropped while asking); NULL when it has no saves */
static struct title *title_get(s32 user, const char *tid)
{
    struct title *t = title_find(user, tid);
    if (t)
        return t;
    scan_wait(user);
    if ((t = title_find(user, tid)))
        return t;
    bool own = host_title()[0] && strncmp(tid, g_host, 9) == 0;
    struct listing *l = page_alloc(sizeof(*l));
    if (!l)
        return NULL;
    unlock();
    s32 rc = list_title(user, tid, own, l);
    lock();
    if (rc == 0)
        title_note(user, tid, own, l);
    page_free(l, sizeof(*l));
    return title_find(user, tid);
}

/* the latest listing of user's tid, made now if there's none newer than max_age ms (lock held, dropped while asking) */
static struct listing *listing(s32 user, const char *tid, long max_age)
{
    struct title *t = title_find(user, tid);
    if (!t)
        return NULL;
    bool own = t->own;
    long now = fv_clock_ms();
    u32 oldest = 0;
    for (u32 i = 0; i < CACHE; i++) {
        struct listing *l = g_cache[i];
        if (l && l->user == user && memcmp(l->id, tid, 9) == 0 && now - l->at <= max_age)
            return l;
        if (!l || (g_cache[oldest] && l->at < g_cache[oldest]->at))
            oldest = i;
    }
    struct listing *fresh = page_alloc(sizeof(*fresh));
    if (!fresh)
        return NULL;
    unlock();
    s32 rc = list_title(user, tid, own, fresh);
    lock();
    if (rc != 0) {
        page_free(fresh, sizeof(*fresh));
        return NULL;
    }
    title_note(user, tid, own, fresh);
    for (u32 i = 0; i < CACHE; i++) /* the slot again: another thread may have filled it */
        if (g_cache[i] && g_cache[i]->user == user && memcmp(g_cache[i]->id, tid, 9) == 0)
            oldest = i;
    if (g_cache[oldest])
        page_free(g_cache[oldest], sizeof(*fresh));
    g_cache[oldest] = fresh;
    return fresh;
}

/* ---- views: what a save is seen through ----
 *
 * As a save tool under ghostldr sees saves (hle_fsvsh.c, save_race.c):
 *   - a mount: SWRR's own saves, and another title's read-only with its
 *     fingerprint; at most MOUNTS, unmounted when idle;
 *   - a race copy: another title's save without a keystone, or one the
 *     system refuses (another account's): a read-only copy in /temp0 made
 *     through a mount ShellCore binds before refusing it (save_race_copy).
 *     Each leaves a mount point behind until SWRR closes, six per session at
 *     most, so it is made only when a client goes into the save, once;
 *   - a writable copy: another title's save with its keystone, once a client
 *     writes into it (save_fp_copy).  From then on the client sees the copy;
 *     COMMIT_MS after the last change with no file of it open, the copy is
 *     written into the save through a read-write mount the service binds and
 *     then refuses (save_race_write): the save keeps its owner and keystone,
 *     and the system commits it when SWRR closes.  Its sce_sys is the
 *     system's: never written.  SWRR's own saves are never written. */

static struct {
    bool used, own;
    s32 user;
    char tid[10], dir[33], mp[16];
    u32 refs;
    long last;
} g_m[MOUNTS];

enum { C_FREE, C_MAKING, C_READY, C_FAILED };
static struct {
    u8 state;
    bool rw;
    s32 user;
    char tid[10], dir[33];
    char host[96];
    u32 refs;
    long last, changed; /* fv_clock_ms; changed: 0 none since the last commit */
    long failed_at;
    s32 err;
    u64 digest; /* rw: the copy's files when last made or committed */
} g_c[COPIES];

static struct {
    int fd;
    s16 m; /* the view, | VIEW_WROTE when opened for writing */
} g_fds[FDS];

#define VIEW_WROTE 0x100

static void unmount(u32 k)
{
    char mp[16];
    memcpy(mp, g_m[k].mp, 16);
    s32 rc = R.umount(mp);
    u8 st[0x78];
    for (u32 waited = 0; rc == 0 && waited < 2000 && K.stat(mp, st) == 0; waited += 10)
        K.usleep(10000);
    gfs_slot_release();
    logf("ftp: saves: %s/%s of user %08x unmounted from %s (%x)", g_m[k].tid, g_m[k].dir, (u32)g_m[k].user, mp,
         (u32)rc);
    g_m[k].used = false;
}

static bool same(s32 user, const char *tid, const char *dir, const struct where *w)
{
    return user == w->user && strcmp(tid, w->tid) == 0 && strcmp(dir, w->dir) == 0;
}

static int mount_find(const struct where *w)
{
    for (u32 k = 0; k < MOUNTS; k++)
        if (g_m[k].used && same(g_m[k].user, g_m[k].tid, g_m[k].dir, w))
            return (int)k;
    return -1;
}

static int copy_find(const struct where *w)
{
    for (u32 c = 0; c < COPIES; c++)
        if (g_c[c].state != C_FREE && same(g_c[c].user, g_c[c].tid, g_c[c].dir, w))
            return (int)c;
    return -1;
}

static bool view_rw(int k) { return k >= MOUNTS && g_c[k - MOUNTS].rw; }

/* the save mounted read-only: the view, or -errno (lock held) */
static int mount_save(const struct where *w, bool own, const char *fp)
{
    s32 k = -1;
    for (u32 i = 0; i < MOUNTS && k < 0; i++)
        if (!g_m[i].used)
            k = (s32)i;
    for (u32 i = 0; i < MOUNTS && k < 0; i++) /* the least recently used idle one goes */
        if (!g_m[i].refs && (k < 0 || g_m[i].last < g_m[k].last))
            k = (s32)i;
    if (k < 0)
        return -FV_EBUSY;
    if (g_m[k].used)
        unmount((u32)k);
    if (gfs_slot_reserve() != 0)
        return -FV_EBUSY;
    char mp[16];
    s32 rc;
    if (own) {
        rc = gfs_mount_user(w->user, w->dir, MODE_RDONLY, 0, mp, NULL);
    } else {
        char tid[16], dir[32];
        struct {
            char data[65];
            char pad[15];
        } f;
        memset(tid, 0, sizeof(tid)), memset(dir, 0, sizeof(dir)), memset(&f, 0, sizeof(f));
        strlcpy(tid, w->tid, 10);
        strlcpy(dir, w->dir, sizeof(dir));
        strlcpy(f.data, fp, sizeof(f.data));
        struct sd_mount1 m;
        struct sd_mount_result r;
        memset(&m, 0, sizeof(m)), memset(&r, 0, sizeof(r));
        m.user = w->user, m.title_id = tid, m.dir = dir, m.fingerprint = &f, m.mode = MODE_RDONLY;
        rc = R.mount(&m, &r);
        memcpy(mp, r.mount_point, 16);
    }
    mp[15] = 0;
    logf("ftp: saves: %s/%s of user %08x read-only -> %x%s%s", w->tid, w->dir, (u32)w->user, (u32)rc,
         rc ? "" : " at ", rc ? "" : mp);
    if (rc != 0) {
        gfs_slot_release();
        /* 0x809F000C no slot, 0x809F0003 busy; 0x809F000F another account's, 0x809F0006 a wrong fingerprint:
         * refused (a race copy may still read it) */
        return rc == (s32)0x809F000C || rc == (s32)0x809F0003 ? -FV_EBUSY : -FV_EACCES;
    }
    g_m[k].used = true, g_m[k].own = own, g_m[k].user = w->user, g_m[k].refs = 1;
    memcpy(g_m[k].tid, w->tid, 10);
    strlcpy(g_m[k].dir, w->dir, sizeof(g_m[k].dir));
    memcpy(g_m[k].mp, mp, 16);
    return k;
}

/* a copy of the save in /temp0, made with the lock let go: rw a writable one (save_fp_copy, with fp), else a
 * read-only one through a refused mount (save_race_copy).  c: its entry, or -1 for a new one.  The view, or
 * -errno (lock held) */
static int make_copy(const struct where *w, int c, bool rw, const char *fp)
{
    if (c < 0)
        for (u32 i = 0; i < COPIES && c < 0; i++)
            if (g_c[i].state == C_FREE)
                c = (int)i;
    if (c < 0)
        return -FV_EBUSY;
    char host[96], fpc[65];
    fmt(host, sizeof(host), "/temp0/ghostldr/ftp/%s/%08x_%s_%s", rw ? "rw" : "race", (u32)w->user, w->tid, w->dir);
    strlcpy(fpc, fp ? fp : "", sizeof(fpc));
    g_c[c].state = C_MAKING, g_c[c].rw = rw, g_c[c].user = w->user, g_c[c].refs = 0, g_c[c].changed = 0;
    memcpy(g_c[c].tid, w->tid, 10);
    strlcpy(g_c[c].dir, w->dir, sizeof(g_c[c].dir));
    strlcpy(g_c[c].host, host, sizeof(g_c[c].host));
    unlock();
    if (!rw)
        logf("ftp: saves: %s/%s of user %08x: %s; a read-only copy through a refused mount (each leaves a mount "
             "point until SWRR closes)", w->tid, w->dir, (u32)w->user, fpc[0] ? "the system refused it" : "no keystone");
    s32 rc = rw ? save_fp_copy(w->user, w->tid, w->dir, fpc, host) : save_race_copy(w->user, w->tid, w->dir, host);
    u64 digest = rc == 0 && rw ? save_tree_digest(host) : 0;
    logf("ftp: saves: %s/%s of user %08x: %s copy in %s -> %x", w->tid, w->dir, (u32)w->user,
         rw ? "a writable" : "a read-only", host, (u32)rc);
    lock();
    if (rc != 0) {
        g_c[c].state = C_FAILED, g_c[c].failed_at = fv_clock_ms();
        g_c[c].err = rc == (s32)0x809F000C ? -FV_EBUSY : rc == (s32)0x809F0008 ? -FV_ENOENT : -FV_EACCES;
        return g_c[c].err;
    }
    g_c[c].state = C_READY, g_c[c].digest = digest, g_c[c].refs = 1;
    return MOUNTS + c;
}

/* the save of w, referenced, for reading or (write) writing: the view, or -errno (lock held, let go while a
 * copy is made) */
static int acquire(const struct where *w, bool write)
{
    for (;;) {
        int c = copy_find(w);
        if (c >= 0 && g_c[c].state == C_MAKING) { /* another client's copy, or a commit */
            unlock();
            fv_sleep_ms(50);
            lock();
            continue;
        }
        if (c >= 0 && g_c[c].state == C_READY && (g_c[c].rw || !write)) {
            g_c[c].refs++;
            return MOUNTS + c;
        }
        int k = mount_find(w);
        if (k >= 0 && !write) {
            g_m[k].refs++;
            return k;
        }
        struct listing *l = listing(w->user, w->tid, 30000);
        if (copy_find(w) != c || mount_find(w) != k) /* changed while the lock was let go */
            continue;
        struct title *t = title_find(w->user, w->tid);
        bool found = false;
        for (u32 i = 0; l && i < l->n && !found; i++)
            found = strcmp(l->e[i].dir, w->dir) == 0;
        if (!t || !found)
            return -FV_ENOENT;
        if (t->own) {
            if (write)
                return -FV_EROFS; /* SWRR's own: never written */
            if (g_title_runs && g_busy_prefix[0] && strncmp(w->dir, g_busy_prefix, strlen(g_busy_prefix)) == 0) {
                logf("ftp: saves: %s/%s is the running game's: not while it runs", w->tid, w->dir);
                return -FV_EBUSY;
            }
            return mount_save(w, true, NULL);
        }
        const char *fp = fp_of(w->tid);
        if (write && swrr_title(w->tid))
            return -FV_EROFS; /* the other region's SWRR: its saves hold the exploit */
        if (write) {
            if (!fp) {
                logf("ftp: saves: no keystone for %s: its saves can be read, not written (a decrypted save of it "
                     "with sce_sys/keystone in PS4/APOLLO on a USB drive, or ghostldr/keystones/%s.keystone on it, "
                     "adds one)", w->tid, w->tid);
                return -FV_EACCES;
            }
            if (c >= 0 && g_c[c].state == C_READY && g_c[c].refs)
                return -FV_EBUSY; /* its read-only copy is being read */
            if (k >= 0) {
                if (g_m[k].refs)
                    return -FV_EBUSY; /* its mount is being read */
                unmount((u32)k);
            }
            if (c >= 0 && g_c[c].state == C_FAILED && g_c[c].rw && fv_clock_ms() - g_c[c].failed_at < 30000)
                return g_c[c].err;
            return make_copy(w, c, true, fp);
        }
        if (c >= 0 && g_c[c].state == C_FAILED && !g_c[c].rw) /* a race that failed: not again this session */
            return g_c[c].err;
        if (fp && (c < 0 || !g_c[c].rw)) {
            int m = mount_save(w, false, fp);
            if (m != -FV_EACCES)
                return m;
        }
        return make_copy(w, c >= 0 && g_c[c].state == C_FAILED ? c : -1, false, NULL);
    }
}

static void release(int k)
{
    bool wrote = k >= 0 && (k & VIEW_WROTE);
    k &= 0xFF;
    if (k < MOUNTS) {
        if (!g_m[k].used)
            return;
        if (g_m[k].refs)
            g_m[k].refs--;
        g_m[k].last = fv_clock_ms();
    } else if (k - MOUNTS < COPIES) {
        int c = k - MOUNTS;
        if (g_c[c].refs)
            g_c[c].refs--;
        g_c[c].last = fv_clock_ms();
        if (wrote)
            g_c[c].changed = g_c[c].last;
    }
}

/* a change made in a writable copy: its commit waits COMMIT_MS from now */
static void changed(int k)
{
    if (view_rw(k))
        g_c[k - MOUNTS].changed = fv_clock_ms();
}

/* a writable copy written into its save (lock held, let go meanwhile) */
static void commit(int c)
{
    struct where w;
    memset(&w, 0, sizeof(w));
    w.user = g_c[c].user;
    memcpy(w.tid, g_c[c].tid, 10);
    strlcpy(w.dir, g_c[c].dir, sizeof(w.dir));
    char host[96], backup[112], fpc[65];
    strlcpy(host, g_c[c].host, sizeof(host));
    const char *fp = fp_of(w.tid);
    strlcpy(fpc, fp ? fp : "", sizeof(fpc));
    u64 was = g_c[c].digest;
    g_c[c].state = C_MAKING, g_c[c].changed = 0;
    unlock();
    u64 now = save_tree_digest(host);
    s32 rc = 0;
    struct save_race_result *r = page_alloc(sizeof(*r));
    if (now != was && r) {
        fmt(backup, sizeof(backup), "%s.before", host);
        rc = save_race_write(w.user, w.tid, w.dir, host, fpc[0] ? fpc : NULL, backup, r);
        logf("ftp: saves: %s/%s of user %08x: the changes -> %x (%u written, %u made, %u deleted)%s%s", w.tid, w.dir,
             (u32)w.user, (u32)rc, r->written, r->created, r->deleted,
             r->needs_exit ? ": written into the save; CLOSE SWRR to keep them (the system commits the save when "
                             "SWRR closes)" : "", rc && r->why[0] ? r->why : "");
    } else if (!r) {
        rc = -1;
    }
    bool tell = rc == 0 && r && r->needs_exit && now != was;
    if (r)
        page_free(r, sizeof(*r));
    if (tell && g_title_runs) /* the game's frames carry it; the selector's log says it */
        ov_banner("Save changed over FTP. To keep it, close SWRR from the PS button menu.", 0);
    lock();
    g_c[c].state = C_READY;
    if (rc == 0)
        g_c[c].digest = now;
}

static void fd_note(int fd, int k)
{
    for (u32 i = 0; i < FDS; i++)
        if (!g_fds[i].fd) {
            g_fds[i].fd = fd + 1, g_fds[i].m = (s16)k; /* fd + 1: 0 is a free entry */
            return;
        }
    logf("ftp: saves: more than %u files open: one save stays held", FDS);
    if ((k & 0xFF) < MOUNTS)
        g_m[k & 0xFF].refs++; /* never let go: the mount can't be released safely without its file */
    else
        g_c[(k & 0xFF) - MOUNTS].refs++;
}

/* the real path of a /saves path in a view */
static void real_path(char *out, size_t cap, int k, const char *rest)
{
    fmt(out, cap, "%s%s", k < MOUNTS ? g_m[k].mp : g_c[k - MOUNTS].host, rest ? rest : "");
}

/* ---- ftp_sys.c's calls ---- */

HIDDEN bool ftps_on(void) { return (g_ctx->flags & CTX_FLAG_FTP_SAVES) && ready() && alloc_tables(); }

HIDDEN bool ftps_owns(const char *path)
{
    return strncmp(path, ROOT, ROOT_LEN) == 0 && (path[ROOT_LEN] == 0 || path[ROOT_LEN] == '/') && ftps_on();
}

static void dir_stat(struct fv_stat *st, long mtime)
{
    st->mode = FV_S_IFDIR | 0555;
    st->size = 0;
    st->mtime = mtime > 0 ? mtime : fv_now();
}

HIDDEN int ftps_stat(const char *path, struct fv_stat *st)
{
    struct where w;
    lock();
    users_load();
    int rc = where(path, &w);
    struct title *t = rc == 0 && w.level >= 2 ? title_get(w.user, w.tid) : NULL;
    if (rc == 0 && w.level >= 2 && !t)
        rc = -FV_ENOENT;
    if (rc == 0 && w.level < 3) {
        dir_stat(st, t ? t->mtime : 0);
    } else if (rc == 0 && w.level == 3) { /* from the listing: a LIST of a title mounts nothing */
        struct listing *l = listing(w.user, w.tid, 30000);
        rc = -FV_ENOENT;
        for (u32 i = 0; l && i < l->n && rc; i++)
            if (strcmp(l->e[i].dir, w.dir) == 0)
                dir_stat(st, l->e[i].mtime), rc = 0;
    } else if (rc == 0) {
        int k = acquire(&w, false);
        rc = k;
        if (k >= 0) {
            char p[PATH_MAX_];
            u8 b[0x78];
            real_path(p, sizeof(p), k, w.rest);
            long r = K.stat(p, b);
            rc = r < 0 ? -(int)(r & 0xFFFF) : 0;
            if (rc == 0) {
                st->mode = *(u16 *)(b + 8) & (view_rw(k) ? ~0u : ~0222u); /* read-only but a writable copy */
                st->mtime = *(s64 *)(b + 0x28);
                st->size = *(s64 *)(b + 0x48);
            }
            release(k);
        }
    }
    unlock();
    return rc;
}

/* the names of a virtual directory, for fv_readdir: d->h points at them */
struct vlist {
    u64 size;
    u32 n, next;
    char name[][48];
};

HIDDEN int ftps_opendir(const char *path, struct fv_dir *d)
{
    struct where w;
    scan_start();
    lock();
    users_load();
    int rc = where(path, &w);
    if (rc == 0 && w.level >= 2 && !title_get(w.user, w.tid))
        rc = -FV_ENOENT;
    if (rc == 0 && w.level >= 3) { /* inside a save: a real directory, its view held until fv_closedir */
        int k = acquire(&w, false);
        rc = k;
        if (k >= 0) {
            char p[PATH_MAX_];
            real_path(p, sizeof(p), k, w.rest);
            long fd = K.open(p, 0x20000 /* O_DIRECTORY */, 0);
            rc = fd < 0 ? -(int)(fd & 0xFFFF) : 0;
            if (rc == 0) {
                fd_note((int)fd, k);
                d->h = fd, d->pos = d->len = 0, d->virt = 0;
            } else
                release(k);
        }
        unlock();
        return rc;
    }
    if (rc != 0) {
        unlock();
        return rc;
    }
    if (w.level == 1) /* its titles: from the scan */
        scan_wait(w.user);
    u32 cap = w.level == 0 ? USERS : w.level == 1 ? TITLES : DIRS;
    u64 size = sizeof(struct vlist) + 48ull * cap;
    struct vlist *v = page_alloc(size);
    if (!v) {
        unlock();
        return -FV_ENOMEM;
    }
    v->size = size;
    if (w.level == 0) {
        for (u32 k = 0; k < g_nusers; k++)
            strlcpy(v->name[v->n++], g_users[k].name, 48);
    } else if (w.level == 1) {
        for (u32 i = 0; i < g_ntitle && v->n < cap; i++)
            if (g_title[i].user == w.user)
                strlcpy(v->name[v->n++], g_title[i].id, 48);
    } else {
        struct listing *l = listing(w.user, w.tid, 2000);
        for (u32 i = 0; l && i < l->n && v->n < cap; i++)
            strlcpy(v->name[v->n++], l->e[i].dir, 48);
    }
    unlock();
    d->h = (long)v, d->pos = d->len = 0, d->virt = 3;
    return 0;
}

HIDDEN int ftps_readdir(struct fv_dir *d, char *name, unsigned size)
{
    struct vlist *v = (struct vlist *)d->h;
    if (v->next >= v->n)
        return 0;
    strlcpy(name, v->name[v->next++], size);
    return 1;
}

HIDDEN void ftps_closedir(struct fv_dir *d)
{
    struct vlist *v = (struct vlist *)d->h;
    page_free(v, v->size);
    d->h = 0;
}

/* a path in a save to change: the save's sce_sys is the system's (lock held) */
static int writable_where(const char *path, struct where *w)
{
    int rc = where(path, w);
    if (rc == 0 && w->level < 4)
        rc = w->level == 3 ? -FV_EROFS : -FV_EPERM; /* a save or a title can't be made, removed or renamed */
    if (rc == 0 && (strcmp(w->rest, "/sce_sys") == 0 || strncmp(w->rest, "/sce_sys/", 9) == 0))
        rc = -FV_EACCES;
    if (rc == 0 && !title_get(w->user, w->tid))
        rc = -FV_ENOENT;
    return rc;
}

HIDDEN int ftps_open(const char *path, int flags)
{
    bool write = (flags & (FV_O_WRONLY | FV_O_APPEND | FV_O_CREAT | FV_O_TRUNC | 2 /* O_RDWR */)) != 0;
    struct where w;
    lock();
    users_load();
    int rc = write ? writable_where(path, &w) : where(path, &w);
    if (rc == 0 && w.level < 4)
        rc = -FV_EISDIR;
    if (rc == 0 && !title_get(w.user, w.tid))
        rc = -FV_ENOENT;
    if (rc == 0) {
        int k = acquire(&w, write);
        rc = k;
        if (k >= 0) {
            char p[PATH_MAX_];
            real_path(p, sizeof(p), k, w.rest);
            long fd = K.open(p, flags, 0666);
            rc = fd < 0 ? -(int)(fd & 0xFFFF) : (int)fd;
            if (fd >= 0)
                fd_note((int)fd, k | (write ? VIEW_WROTE : 0));
            else
                release(k);
        }
    }
    unlock();
    return rc;
}

/* rmdir, unlink, mkdir in a save (its writable copy) */
static int change(const char *path, int op)
{
    struct where w;
    lock();
    users_load();
    int rc = writable_where(path, &w);
    if (rc == 0) {
        int k = acquire(&w, true);
        rc = k;
        if (k >= 0) {
            char p[PATH_MAX_];
            real_path(p, sizeof(p), k, w.rest);
            long r = op == 0 ? R.rmdir(p) : op == 1 ? R.unlink(p) : R.mkdir(p, 0777);
            rc = r < 0 ? -(int)(r & 0xFFFF) : 0;
            if (rc == 0)
                changed(k);
            release(k);
            logf("ftp: saves: %s %s -> %d", op == 0 ? "RMD" : op == 1 ? "DELE" : "MKD", path, rc);
        }
    }
    unlock();
    return rc;
}

HIDDEN int ftps_rmdir(const char *path) { return change(path, 0); }
HIDDEN int ftps_unlink(const char *path) { return change(path, 1); }

/* a rename inside one save */
HIDDEN int ftps_rename(const char *from, const char *to)
{
    struct where a, b;
    lock();
    users_load();
    int rc = writable_where(from, &a);
    if (rc == 0)
        rc = writable_where(to, &b);
    if (rc == 0 && (a.user != b.user || strcmp(a.tid, b.tid) != 0 || strcmp(a.dir, b.dir) != 0))
        rc = -FV_EXDEV;
    if (rc == 0) {
        int k = acquire(&a, true);
        rc = k;
        if (k >= 0) {
            char p[PATH_MAX_], q[PATH_MAX_];
            real_path(p, sizeof(p), k, a.rest);
            real_path(q, sizeof(q), k, b.rest);
            long r = R.rename(p, q);
            rc = r < 0 ? -(int)(r & 0xFFFF) : 0;
            if (rc == 0)
                changed(k);
            release(k);
        }
    }
    unlock();
    return rc;
}

/* fd is about to be closed: the mount it held, or -1 */
HIDDEN int ftps_closing(int fd)
{
    int k = -1;
    lock();
    for (u32 i = 0; i < FDS && k < 0; i++)
        if (g_fds[i].fd == fd + 1)
            k = g_fds[i].m, g_fds[i].fd = 0, g_fds[i].m = 0;
    unlock();
    return k;
}

HIDDEN void ftps_closed(int k)
{
    if (k < 0)
        return;
    lock();
    release(k);
    unlock();
}

/* MKD /saves/<user>/<TITLE>: ask about that title (a game nothing named), so the user's folder lists it from
 * now on (a path straight to it works without) */
HIDDEN int ftps_mkdir(const char *path)
{
    struct where w;
    lock();
    users_load();
    int rc = where(path, &w);
    if (rc == 0 && w.level >= 3) {
        unlock();
        return change(path, 2); /* a folder in a save */
    }
    if (rc == 0 && w.level != 2)
        rc = -FV_EROFS;
    if (rc == 0 && title_find(w.user, w.tid))
        rc = -FV_EEXIST;
    struct title *t = rc == 0 ? title_get(w.user, w.tid) : NULL;
    if (t)
        hint_add(USER_ANY, w.tid); /* the other users' too, at their next scan */
    else if (rc == 0)
        rc = -FV_ENOENT;
    unlock();
    logf("ftp: saves: MKD %s: %s", path, rc ? "no saves of that title" : "its saves are listed");
    return rc;
}

/* the root's own entry: "saves", when it is on */
HIDDEN const char *ftps_root_name(void) { return ftps_on() ? "saves" : NULL; }

/* the listener's tick: idle saves unmounted */
HIDDEN void ftps_idle(void)
{
    if (!g_title)
        return;
    lock();
    long now = fv_clock_ms();
    for (u32 k = 0; k < MOUNTS; k++)
        if (g_m[k].used && !g_m[k].refs && now - g_m[k].last > (g_m[k].own ? OWN_IDLE_MS : IDLE_MS))
            unmount(k);
    for (u32 c = 0; c < COPIES; c++)
        if (g_c[c].state == C_READY && g_c[c].rw && g_c[c].changed && !g_c[c].refs &&
            fv_clock_ms() - g_c[c].changed > COMMIT_MS)
            commit((int)c);
    unlock();
}

/* the title is about to start: its containers are its own from now on */
HIDDEN void ftps_title_starting(void)
{
    const char *s = gfs_save_name(); /* <prefix>SAVE */
    size_t n = s ? strlen(s) : 0;
    lock();
    g_title_runs = true;
    if (n > 4 && n - 4 < sizeof(g_busy_prefix))
        memcpy(g_busy_prefix, s, n - 4), g_busy_prefix[n - 4] = 0;
    for (u32 k = 0; k < MOUNTS; k++)
        if (g_m[k].used && !g_m[k].refs && g_m[k].own)
            unmount(k);
    unlock();
}

/* the server is about to stop: the scan ends, and no client waits for it */
HIDDEN void ftps_halt(void) { g_stop = 1; }

/* the server stopped (ftp_stop: no client is left): every save is unmounted and all is freed; 0, or -1 when
 * the scan's thread is still there (the code must stay) */
HIDDEN int ftps_stop(void)
{
    g_stop = 1;
    for (u32 waited = 0; __atomic_load_n(&g_scan, __ATOMIC_ACQUIRE) == 1 && waited < 15000; waited += 20)
        fv_sleep_ms(20);
    lock();
    for (u32 k = 0; k < MOUNTS; k++)
        if (g_m[k].used)
            unmount(k);
    /* the clients are gone: what they changed goes into the saves now; the copies stay for the session */
    for (u32 c = 0; c < COPIES; c++) {
        g_c[c].refs = 0;
        if (g_c[c].state == C_READY && g_c[c].rw && g_c[c].changed)
            commit((int)c);
        if (g_c[c].state == C_FAILED && g_c[c].rw)
            g_c[c].state = C_FREE;
    }
    for (u32 i = 0; i < CACHE; i++)
        if (g_cache[i])
            page_free(g_cache[i], sizeof(struct listing)), g_cache[i] = NULL;
    if (g_title)
        page_free(g_title, sizeof(*g_title) * TITLES), g_title = NULL;
    if (g_hint)
        page_free(g_hint, sizeof(*g_hint) * HINTS), g_hint = NULL;
    if (g_fp)
        page_free(g_fp, sizeof(*g_fp) * FPS), g_fp = NULL;
    g_ntitle = g_nhint = g_nfp = g_nusers = 0;
    g_scanned = 0, g_hints_read = false, g_users_at = -1, g_title_runs = false, g_busy_prefix[0] = 0;
    memset(g_fds, 0, sizeof(g_fds));
    unlock();
    if (__atomic_load_n(&g_scan, __ATOMIC_ACQUIRE) == 1) {
        logf("ftp: saves: the scan's thread is still there");
        return -1;
    }
    g_scan = 0;
    g_stop = 0;
    return 0;
}
