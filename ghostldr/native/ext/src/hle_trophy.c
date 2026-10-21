/*
 * libSceNpTrophy, fully virtual (np-core-trophy spec §6.8).
 *
 * The console's libSceNpTrophy in this process belongs to SWRR: a context
 * there is SWRR's trophy set.  No call reaches it, and the library has no
 * host fallback (policy at the end), so an import missing here gets a stub
 * returning NOT_SUPPORTED.
 *
 * Contexts, handles and unlocks succeed locally.  Unlocks are recorded per
 * user in a "GTRP" record in the title store (title_store.h), written after
 * each new unlock and read back when a context is registered.  There is no
 * UI and no real trophy.
 *
 * The trophy set's metadata is encrypted (TROPCONF.ESFM), so the number of
 * trophies comes from a per-title table, else from the icons listed in
 * sce_sys/trophy/trophy00.trp, else from the highest recorded unlock; it is
 * never 0.  Grades, names and groups are unknown: grade 0, empty strings,
 * base game.
 *
 * Locks: g_lock guards the tables for short moments only.  Title store calls
 * (a save container mount on the console, 100 ms or more) run without it.
 */
#include "ext.h"
#include "title_store.h"

/* none of this is hot: optimize for size (the extension shares a 48 KiB area) */
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

#define E(x) ((s32)(x))
#define T_INVALID_ARGUMENT E(0x80551604)
#define T_INVALID_HANDLE E(0x80551608)
#define T_INVALID_CONTEXT E(0x80551609)
#define T_INVALID_TROPHY_ID E(0x8055160A)
#define T_INVALID_GROUP_ID E(0x8055160B)
#define T_ALREADY_UNLOCKED E(0x8055160C)
#define T_NOT_REGISTERED E(0x8055160F)
#define T_ALREADY_REGISTERED E(0x80551610)
#define T_CONTEXT_ALREADY_EXISTS E(0x80551613)
#define T_ICON_FILE_NOT_FOUND E(0x80551614)
#define T_TROPHY_NOT_UNLOCKED E(0x8055161A)
#define T_NOT_SUPPORTED E(0x80551622)
#define T_CONTEXT_EXCEEDS_MAX E(0x80551623)
#define T_HANDLE_EXCEEDS_MAX E(0x80551624)

#define MAX_CONTEXTS 8
#define MAX_HANDLES 4
#define MAX_USERS 4
#define MAX_TROPHIES 128
#define GROUP_BASE_GAME (-1)

/* the per-user record (title store "trophy_%08x.bin") */
#define GTRP_MAGIC 0x50525447u /* "GTRP" */
#define GTRP_VERSION 1
struct gtrp {
    u32 magic, version;
    char title_id[16];
    u32 count_hint, reserved;
    u32 flags[MAX_TROPHIES / 32]; /* bit id & 31 of word id >> 5, as SceNpTrophyFlagArray */
    u64 tick[MAX_TROPHIES];       /* SceRtcTick of the unlock (µs since 0001-01-01 UTC); 0: locked */
};
_Static_assert(sizeof(struct gtrp) == 1072, "GTRP record layout");

struct user_rec {
    s32 user;
    struct gtrp r;
};

struct context {
    bool used, registered;
    s32 user;
    u32 label;
    struct user_rec *rec;
};

static volatile int g_lock; /* the tables below */
static struct context g_contexts[MAX_CONTEXTS];
static bool g_handles[MAX_HANDLES];
static struct user_rec *g_recs[MAX_USERS]; /* allocated on first use, never freed */

/* ---- the trophy set ---- */

static bool g_meta_loaded;
static char g_title_id[16];
static u32 g_known_trophies, g_known_groups; /* 0: unknown */

/* titles whose trophy count is known (the set itself is encrypted) */
static const struct {
    const char *title;
    u32 trophies, groups;
} k_sets[] = {
    {"CUSA11456", 77, 0}, /* Ghost of Tsushima: the count it clamps to (eboot 2.18 +0xf08555) */
};

const char *np_title_id(void); /* hle_np.c: what the title gave sceNpSetNpTitleId */

static int open_app0(const char *rel)
{
    char path[256], buf[256];
    fmt(path, sizeof(path), "/app0/%s", rel); /* on the stack: kernel paths never point into our text */
    return vfs_open(remap_path(path, buf, sizeof(buf)), O_RDONLY, 0);
}

static bool read_at(int fd, s64 off, void *buf, u64 n)
{
    return vfs_lseek(fd, off, 0) == off && vfs_read(fd, buf, n) == (s64)n;
}

static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* TITLE_ID from sce_sys/param.sfo (little endian: "\0PSF", key table at
 * 8, data table at 12, entry count at 16, entries of 16 bytes from 0x14) */
static void sfo_title_id(void)
{
    int fd = open_app0("sce_sys/param.sfo");
    if (fd < 0)
        return;
    u8 h[0x14], e[16];
    char key[12], id[16] = {0};
    if (read_at(fd, 0, h, sizeof(h)) && le32(h) == 0x46535000u) {
        u32 keys = le32(h + 8), data = le32(h + 12), n = le32(h + 16);
        for (u32 i = 0; i < n && i < 256; i++) {
            /* u16 key offset, u16 format, u32 length, u32 maximum, u32 data offset */
            if (!read_at(fd, 0x14 + 16 * i, e, sizeof(e)) || !read_at(fd, keys + (e[0] | e[1] << 8), key, 9))
                break;
            if (memcmp(key, "TITLE_ID", 9) == 0) {
                u32 len = le32(e + 4);
                if (read_at(fd, (s64)data + le32(e + 12), id, len < 15 ? len : 15))
                    strlcpy(g_title_id, id, sizeof(g_title_id));
                break;
            }
        }
    }
    vfs_close(fd);
}

/* Count the trophy and group icons listed in sce_sys/trophy/trophy00.trp.
 * The entries are plain (only the ESFM files are encrypted).  Layout
 * (big endian): magic 0xDCA24D00, entry count at 0x10, entry size at 0x14,
 * entries from 0x60 {name[32], u64 offset, u64 size, u32 flags, pad}.
 * Unverified on a real title (spec NP-4): everything is bounds checked. */
static void count_trp_icons(void)
{
    int fd = open_app0("sce_sys/trophy/trophy00.trp");
    if (fd < 0)
        return;
    u8 h[0x60];
    u32 trophies = 0, groups = 0, n = 0;
    if (read_at(fd, 0, h, sizeof(h)) && be32(h) == 0xDCA24D00u) {
        u32 esize = be32(h + 0x14);
        n = be32(h + 0x10);
        for (u32 i = 0; esize >= 0x40 && esize <= 0x100 && n <= 4096 && i < n; i++) {
            char name[33] = {0};
            if (!read_at(fd, 0x60 + (s64)i * esize, name, 32))
                break;
            const char *d = strncmp(name, "TROP", 4) == 0 ? name + 4 : strncmp(name, "GR", 2) == 0 ? name + 2 : NULL;
            if (!d || d[0] < '0' || d[0] > '9' || d[1] < '0' || d[1] > '9' || d[2] < '0' || d[2] > '9' ||
                strcmp(d + 3, ".PNG") != 0)
                continue;
            if (d == name + 4)
                trophies++;
            else
                groups++;
        }
    }
    vfs_close(fd);
    logf("trophy: trophy00.trp lists %u entries: %u trophy icons, %u group icons (layout unverified)", n,
         trophies, groups);
    if (trophies && trophies <= MAX_TROPHIES)
        g_known_trophies = trophies, g_known_groups = groups;
}

static void known_set(void)
{
    for (u32 i = 0; i < ARRAY_SIZE(k_sets); i++)
        if (strcmp(g_title_id, k_sets[i].title) == 0)
            g_known_trophies = k_sets[i].trophies, g_known_groups = k_sets[i].groups;
}

/* The title ID: the core's (the ghostfs index; a service the system family
 * adds to struct ghost_api), else param.sfo's.  Then the trophy count. */
static void load_meta(void)
{
    static volatile int lock;
    spin_lock(&lock);
    if (!g_meta_loaded) {
#ifdef title_id
        const char *core_id = title_id();
        if (core_id)
            strlcpy(g_title_id, core_id, sizeof(g_title_id));
#endif
        if (!g_title_id[0])
            sfo_title_id();
        known_set();
        if (!g_known_trophies)
            count_trp_icons();
        logf("trophy: title %s, %u trophies%s, %u groups", g_title_id[0] ? g_title_id : "unknown (yet)",
             g_known_trophies, g_known_trophies ? "" : " (unknown: the highest unlock + 1)", g_known_groups);
        g_meta_loaded = true;
    }
    spin_unlock(&lock);
}

/* No title ID at registration: NpToolkit2 may have set the NP title ID
 * ("CUSA11456_00") since.  Cheap: called with g_lock held. */
static void late_title_id(void)
{
    if (g_title_id[0] || g_known_trophies)
        return;
    const char *np = np_title_id();
    for (u32 i = 0; i < 9; i++)
        if (!np[i] || (i < 4 ? np[i] < 'A' || np[i] > 'Z' : np[i] < '0' || np[i] > '9'))
            return;
    memcpy(g_title_id, np, 9);
    known_set();
    logf("trophy: title %s from the NP title ID: %u trophies", g_title_id, g_known_trophies);
}

static bool unlocked(const struct gtrp *r, u32 id) { return r->flags[id >> 5] >> (id & 31) & 1; }

static u32 count_unlocked(const struct gtrp *r)
{
    u32 n = 0;
    for (u32 i = 0; i < MAX_TROPHIES; i++)
        n += unlocked(r, i);
    return n;
}

/* never 0: titles may divide by it */
static u32 trophy_count(const struct gtrp *r)
{
    late_title_id();
    if (g_known_trophies)
        return g_known_trophies;
    u32 hi = 0;
    for (u32 i = 0; i < MAX_TROPHIES; i++)
        if (unlocked(r, i))
            hi = i + 1;
    return hi ? hi : 1;
}

static bool valid_id(s32 id)
{
    late_title_id();
    return id >= 0 && id < MAX_TROPHIES && (!g_known_trophies || (u32)id < g_known_trophies);
}

/* ---- records ---- */

static void record_name(char *name, u32 size, s32 user) { fmt(name, size, "trophy_%08x.bin", (u32)user); }

/* the user's record (g_lock held) */
static struct user_rec *find_record(s32 user)
{
    for (u32 i = 0; i < MAX_USERS && g_recs[i]; i++)
        if (g_recs[i]->user == user)
            return g_recs[i];
    return NULL;
}

/* read from the title store (no lock held) */
static struct user_rec *load_record(s32 user)
{
    static struct user_rec spare; /* never fail the title's call */
    struct user_rec *u = arena_alloc(sizeof(*u));
    if (!u)
        u = &spare;
    u->user = user;
    char name[32];
    record_name(name, sizeof(name), user);
    s64 n = title_store_read(name, &u->r, sizeof(u->r));
    if (n == (s64)sizeof(u->r) && u->r.magic == GTRP_MAGIC && u->r.version == GTRP_VERSION) {
        logf("trophy: %s: %u trophies unlocked", name, count_unlocked(&u->r));
        return u;
    }
    if (n < 0)
        logf("trophy: %s: no record yet (%lx)", name, (u64)(u32)n);
    else
        logf("trophy: %s: bad record (%ld bytes, magic %x): starting empty", name, n, u->r.magic);
    memset(&u->r, 0, sizeof(u->r));
    u->r.magic = GTRP_MAGIC;
    u->r.version = GTRP_VERSION;
    strlcpy(u->r.title_id, g_title_id, sizeof(u->r.title_id));
    u->r.count_hint = g_known_trophies;
    return u;
}

/* keep u unless another thread kept the user's record first (g_lock held) */
static struct user_rec *keep_record(struct user_rec *u)
{
    struct user_rec *have = find_record(u->user);
    if (have)
        return have;
    u32 i = 0;
    while (i < MAX_USERS && g_recs[i])
        i++;
    if (i == MAX_USERS) {
        i = MAX_USERS - 1;
        logf("trophy: more than %u users: user %x's record replaces user %x's", MAX_USERS, (u32)u->user,
             (u32)g_recs[i]->user);
    }
    return g_recs[i] = u;
}

/* One write at a time, each with the latest state, so the last write wins
 * with everything in it.  Waits by sleeping: a write takes a while. */
static volatile int g_save_lock;

static void record_save(struct user_rec *u)
{
    while (__atomic_exchange_n(&g_save_lock, 1, __ATOMIC_ACQUIRE))
        if (K.usleep)
            K.usleep(1000);
    struct gtrp copy;
    spin_lock(&g_lock);
    copy = u->r;
    spin_unlock(&g_lock);
    char name[32];
    record_name(name, sizeof(name), u->user);
    s32 rc = title_store_write(name, &copy, sizeof(copy));
    __atomic_store_n(&g_save_lock, 0, __ATOMIC_RELEASE);
    if (rc < 0)
        logf("trophy: writing %s failed (%x): the unlock is kept in memory only", name, (u32)rc);
}

/* ---- contexts and handles ---- */

/* the checks every (context, handle) call makes, in the library's order (g_lock held) */
static s32 check(s32 ctx, s32 handle, bool need_registered, struct context **out)
{
    if (ctx == -1)
        return T_INVALID_CONTEXT;
    if (handle == -1)
        return T_INVALID_HANDLE;
    if (ctx < 1 || ctx > MAX_CONTEXTS || !g_contexts[ctx - 1].used)
        return T_INVALID_CONTEXT;
    if (handle < 1 || handle > MAX_HANDLES || !g_handles[handle - 1])
        return T_INVALID_HANDLE;
    if (need_registered && !g_contexts[ctx - 1].registered)
        return T_NOT_REGISTERED;
    *out = &g_contexts[ctx - 1];
    return 0;
}

static u32 g_errors_logged;

static s32 ret(const char *fn, s32 rc)
{
    if (rc < 0 && g_errors_logged++ < 64)
        logf("trophy: %s -> %x", fn, (u32)rc);
    return rc;
}

static s32 hle_sceNpTrophyCreateContext(s32 *ctx, s32 user, u32 label, u64 options)
{
    if (!ctx || options)
        return ret("sceNpTrophyCreateContext", T_INVALID_ARGUMENT);
    s32 rc = T_CONTEXT_EXCEEDS_MAX;
    spin_lock(&g_lock);
    for (u32 i = 0; i < MAX_CONTEXTS; i++)
        if (g_contexts[i].used && g_contexts[i].user == user && g_contexts[i].label == label)
            rc = T_CONTEXT_ALREADY_EXISTS;
    for (u32 i = 0; rc == T_CONTEXT_EXCEEDS_MAX && i < MAX_CONTEXTS; i++)
        if (!g_contexts[i].used) {
            g_contexts[i] = (struct context){true, false, user, label, NULL};
            *ctx = (s32)i + 1;
            rc = 0;
        }
    spin_unlock(&g_lock);
    logf("sceNpTrophyCreateContext(user %x, label %u) -> %x, context %d", (u32)user, label, (u32)rc,
         rc == 0 ? *ctx : -1);
    return ret("sceNpTrophyCreateContext", rc);
}

static s32 hle_sceNpTrophyCreateHandle(s32 *handle)
{
    if (!handle)
        return ret("sceNpTrophyCreateHandle", T_INVALID_ARGUMENT);
    s32 rc = T_HANDLE_EXCEEDS_MAX;
    spin_lock(&g_lock);
    for (u32 i = 0; rc && i < MAX_HANDLES; i++)
        if (!g_handles[i]) {
            g_handles[i] = true;
            *handle = (s32)i + 1;
            rc = 0;
        }
    spin_unlock(&g_lock);
    return ret("sceNpTrophyCreateHandle", rc);
}

/* Returns as soon as the user's record is read: there is no trophy set to
 * install. */
static s32 hle_sceNpTrophyRegisterContext(s32 ctx, s32 handle, u64 options)
{
    if (options)
        return ret("sceNpTrophyRegisterContext", T_INVALID_ARGUMENT);
    load_meta();
    struct context *c;
    spin_lock(&g_lock);
    s32 rc = check(ctx, handle, false, &c);
    if (rc == 0 && c->registered)
        rc = T_ALREADY_REGISTERED;
    s32 user = rc == 0 ? c->user : -1;
    struct user_rec *u = rc == 0 ? find_record(user) : NULL;
    spin_unlock(&g_lock);
    if (rc == 0 && !u) {
        struct user_rec *read = load_record(user); /* the title store: no lock */
        spin_lock(&g_lock);
        u = keep_record(read);
        spin_unlock(&g_lock);
    }
    if (rc == 0) {
        spin_lock(&g_lock); /* the context may have changed meanwhile */
        rc = check(ctx, handle, false, &c);
        if (rc == 0 && c->registered)
            rc = T_ALREADY_REGISTERED;
        else if (rc == 0 && c->user != user)
            rc = T_INVALID_CONTEXT;
        if (rc == 0)
            c->rec = u, c->registered = true;
        spin_unlock(&g_lock);
    }
    logf("sceNpTrophyRegisterContext(%d, %d) -> %x", ctx, handle, (u32)rc);
    return ret("sceNpTrophyRegisterContext", rc);
}

static s32 hle_sceNpTrophyDestroyContext(s32 ctx)
{
    s32 rc = 0;
    spin_lock(&g_lock);
    if (ctx < 1 || ctx > MAX_CONTEXTS || !g_contexts[ctx - 1].used)
        rc = T_INVALID_CONTEXT;
    else
        g_contexts[ctx - 1] = (struct context){0};
    spin_unlock(&g_lock);
    return ret("sceNpTrophyDestroyContext", rc);
}

static s32 hle_sceNpTrophyDestroyHandle(s32 handle)
{
    s32 rc = 0;
    spin_lock(&g_lock);
    if (handle < 1 || handle > MAX_HANDLES || !g_handles[handle - 1])
        rc = T_INVALID_HANDLE;
    else
        g_handles[handle - 1] = false;
    spin_unlock(&g_lock);
    return ret("sceNpTrophyDestroyHandle", rc);
}

/* no call is ever in flight */
static s32 hle_sceNpTrophyAbortHandle(s32 handle)
{
    s32 rc = handle >= 1 && handle <= MAX_HANDLES && g_handles[handle - 1] ? 0 : T_INVALID_HANDLE;
    return ret("sceNpTrophyAbortHandle", rc);
}

/* ---- unlocks ---- */

static u64 rtc_now(void)
{
    static s32 (*now)(u64 *);
    if (!now)
        now = (s32(*)(u64 *))host_real_in("libSceRtc", "sceRtcGetCurrentTick");
    u64 tick = 0;
    if (!now || now(&tick) != 0 || !tick)
        tick = 1; /* unlocked, time unknown */
    return tick;
}

/* Platinum is never awarded: the grades are unknown.  A second unlock of a
 * trophy fails as on the console (titles handle it, and it avoids a second
 * in-game popup).  The record is written before the call returns. */
static s32 hle_sceNpTrophyUnlockTrophy(s32 ctx, s32 handle, s32 id, s32 *platinum)
{
    u64 tick = rtc_now();
    struct context *c;
    struct user_rec *save = NULL;
    u32 n = 0, total = 0;
    s32 user = 0;
    spin_lock(&g_lock);
    s32 rc = check(ctx, handle, true, &c);
    if (rc == 0 && !platinum)
        rc = T_INVALID_ARGUMENT;
    else if (rc == 0 && !valid_id(id))
        rc = T_INVALID_TROPHY_ID;
    if (rc == 0) {
        *platinum = -1;
        struct gtrp *r = &c->rec->r;
        if (unlocked(r, (u32)id))
            rc = T_ALREADY_UNLOCKED;
        else {
            r->flags[id >> 5] |= 1u << (id & 31);
            r->tick[id] = tick;
            if (!r->title_id[0])
                strlcpy(r->title_id, g_title_id, sizeof(r->title_id));
            save = c->rec, user = c->user;
            n = count_unlocked(r), total = trophy_count(r);
        }
    }
    spin_unlock(&g_lock);
    if (save) {
        record_save(save);
        logf("trophy %d unlocked locally (context %d, user %x, %u of %u)", id, ctx, (u32)user, n, total);
    } else if (rc == T_ALREADY_UNLOCKED)
        logf("trophy %d already unlocked", id);
    return rc == T_ALREADY_UNLOCKED ? rc : ret("sceNpTrophyUnlockTrophy", rc);
}

static s32 hle_sceNpTrophyGetTrophyUnlockState(s32 ctx, s32 handle, u32 *flags, u32 *count)
{
    if (!flags || !count)
        return ret("sceNpTrophyGetTrophyUnlockState", T_INVALID_ARGUMENT);
    struct context *c;
    spin_lock(&g_lock);
    s32 rc = check(ctx, handle, true, &c);
    if (rc == 0) {
        memcpy(flags, c->rec->r.flags, sizeof(c->rec->r.flags));
        *count = trophy_count(&c->rec->r);
    }
    spin_unlock(&g_lock);
    return ret("sceNpTrophyGetTrophyUnlockState", rc);
}

/* ---- information ---- */

struct trophy_details {
    u64 size;
    s32 id, grade, group;
    bool hidden;
    u8 pad[3];
    char name[128], description[1024];
};
struct trophy_data {
    u64 size;
    s32 id;
    bool unlocked;
    u64 timestamp;
};
struct set_details { /* SceNpTrophyGameDetails and SceNpTrophyGroupDetails */
    u64 size;
    s32 group;   /* the group's: GroupDetails only (GameDetails: numGroups) */
    u32 trophies, platinum, gold, silver, bronze;
    char title[128], description[1024];
};
struct game_data {
    u64 size;
    u32 unlocked, platinum, gold, silver, bronze, progress;
};
struct group_data {
    u64 size;
    s32 group;
    u32 unlocked, platinum, gold, silver, bronze, progress, pad;
};
_Static_assert(sizeof(struct trophy_details) == 0x498 && __builtin_offsetof(struct trophy_details, name) == 0x18 && sizeof(struct trophy_data) == 0x18 &&
                   sizeof(struct set_details) == 0x4A0 && sizeof(struct game_data) == 0x20 &&
                   sizeof(struct group_data) == 0x28,
               "trophy structure sizes");

/* A pair of optional outputs: at least one, and each with its size field
 * (the first u64) filled in. */
static bool outputs_ok(const void *a, u64 a_size, const void *b, u64 b_size)
{
    return (a || b) && (!a || *(const u64 *)a == a_size) && (!b || *(const u64 *)b == b_size);
}

static s32 hle_sceNpTrophyGetTrophyInfo(s32 ctx, s32 handle, s32 id, struct trophy_details *d, struct trophy_data *t)
{
    struct context *c;
    spin_lock(&g_lock);
    s32 rc = check(ctx, handle, true, &c);
    if (rc == 0 && !valid_id(id))
        rc = T_INVALID_TROPHY_ID;
    else if (rc == 0 && !outputs_ok(d, sizeof(*d), t, sizeof(*t)))
        rc = T_INVALID_ARGUMENT;
    if (rc == 0 && d) {
        memset(d, 0, sizeof(*d));
        d->size = sizeof(*d), d->id = id, d->group = GROUP_BASE_GAME; /* grade 0: unknown */
    }
    if (rc == 0 && t) {
        memset(t, 0, sizeof(*t));
        t->size = sizeof(*t), t->id = id;
        t->unlocked = unlocked(&c->rec->r, (u32)id);
        t->timestamp = c->rec->r.tick[id];
    }
    spin_unlock(&g_lock);
    return ret("sceNpTrophyGetTrophyInfo", rc);
}

static void fill_set(struct set_details *d, s32 group, u32 trophies)
{
    memset(d, 0, sizeof(*d));
    d->size = sizeof(*d), d->group = group, d->trophies = trophies;
    strlcpy(d->title, g_title_id, sizeof(d->title));
}

static u32 progress(u32 unlocked_n, u32 total) { return total ? unlocked_n * 100 / total : 0; }

static s32 hle_sceNpTrophyGetGameInfo(s32 ctx, s32 handle, struct set_details *d, struct game_data *g)
{
    struct context *c;
    spin_lock(&g_lock);
    s32 rc = check(ctx, handle, true, &c);
    if (rc == 0 && !outputs_ok(d, sizeof(*d), g, sizeof(*g)))
        rc = T_INVALID_ARGUMENT;
    if (rc == 0) {
        u32 total = trophy_count(&c->rec->r), n = count_unlocked(&c->rec->r);
        if (d)
            fill_set(d, (s32)g_known_groups, total); /* GameDetails: numGroups in the group field */
        if (g) {
            memset(g, 0, sizeof(*g));
            g->size = sizeof(*g), g->unlocked = n, g->progress = progress(n, total);
        }
    }
    spin_unlock(&g_lock);
    return ret("sceNpTrophyGetGameInfo", rc);
}

/* group -1 is the whole set; other groups report no trophies */
static s32 hle_sceNpTrophyGetGroupInfo(s32 ctx, s32 handle, s32 group, struct set_details *d, struct group_data *g)
{
    struct context *c;
    spin_lock(&g_lock);
    s32 rc = check(ctx, handle, true, &c);
    if (rc == 0 && (group < GROUP_BASE_GAME || (g_known_groups && group >= (s32)g_known_groups)))
        rc = T_INVALID_GROUP_ID;
    else if (rc == 0 && !outputs_ok(d, sizeof(*d), g, sizeof(*g)))
        rc = T_INVALID_ARGUMENT;
    if (rc == 0) {
        bool all = group == GROUP_BASE_GAME;
        u32 total = all ? trophy_count(&c->rec->r) : 0, n = all ? count_unlocked(&c->rec->r) : 0;
        if (d)
            fill_set(d, group, total);
        if (g) {
            memset(g, 0, sizeof(*g));
            g->size = sizeof(*g), g->group = group, g->unlocked = n, g->progress = progress(n, total);
        }
    }
    spin_unlock(&g_lock);
    return ret("sceNpTrophyGetGroupInfo", rc);
}

/* No icons: the trophy set is not installed anywhere. */
static s32 icon(const char *fn, s32 ctx, s32 handle, s32 id, bool trophy, const u64 *size)
{
    if (!size)
        return ret(fn, T_INVALID_ARGUMENT);
    struct context *c;
    spin_lock(&g_lock);
    s32 rc = check(ctx, handle, true, &c);
    if (rc == 0 && trophy && !valid_id(id))
        rc = T_INVALID_TROPHY_ID;
    else if (rc == 0 && trophy && !unlocked(&c->rec->r, (u32)id))
        rc = T_TROPHY_NOT_UNLOCKED;
    spin_unlock(&g_lock);
    return ret(fn, rc ? rc : T_ICON_FILE_NOT_FOUND);
}

static s32 hle_sceNpTrophyGetGameIcon(s32 ctx, s32 handle, void *buf, u64 *size)
{
    return icon("sceNpTrophyGetGameIcon", ctx, handle, 0, false, size);
}

static s32 hle_sceNpTrophyGetGroupIcon(s32 ctx, s32 handle, s32 group, void *buf, u64 *size)
{
    return icon("sceNpTrophyGetGroupIcon", ctx, handle, group, false, size);
}

static s32 hle_sceNpTrophyGetTrophyIcon(s32 ctx, s32 handle, s32 id, void *buf, u64 *size)
{
    return icon("sceNpTrophyGetTrophyIcon", ctx, handle, id, true, size);
}

/* never the console's: it would open SWRR's trophy list */
static s32 hle_sceNpTrophyShowTrophyList(s32 ctx, s32 handle)
{
    struct context *c;
    spin_lock(&g_lock);
    s32 rc = check(ctx, handle, true, &c);
    spin_unlock(&g_lock);
    logf("sceNpTrophyShowTrophyList(%d, %d) -> %x: nothing is shown", ctx, handle, (u32)rc);
    return ret("sceNpTrophyShowTrophyList", rc);
}

static s32 hle_sceNpTrophyCaptureScreenshot(s32 handle, const void *targets, u32 n)
{
    s32 rc = handle >= 1 && handle <= MAX_HANDLES && g_handles[handle - 1] ? 0 : T_INVALID_HANDLE;
    return ret("sceNpTrophyCaptureScreenshot", rc);
}

/* the trophy configuration (TROPCONF) is encrypted: not supported, nothing written */
static s32 hle_trophy_config(void)
{
    static bool logged;
    if (!logged) {
        logged = true;
        logf("trophy: sceNpTrophyConfig* -> NOT_SUPPORTED");
    }
    return T_NOT_SUPPORTED;
}

#define T(fn) {"libSceNpTrophy", #fn, hle_##fn, NULL}
#define CONFIG(fn) {"libSceNpTrophy", #fn, hle_trophy_config, NULL}

static const struct ext_hle_ent k_hle[] = {
    T(sceNpTrophyCreateContext),
    T(sceNpTrophyCreateHandle),
    T(sceNpTrophyRegisterContext),
    T(sceNpTrophyDestroyContext),
    T(sceNpTrophyDestroyHandle),
    T(sceNpTrophyAbortHandle),
    T(sceNpTrophyUnlockTrophy),
    T(sceNpTrophyGetTrophyUnlockState),
    T(sceNpTrophyGetTrophyInfo),
    T(sceNpTrophyGetGameInfo),
    T(sceNpTrophyGetGroupInfo),
    T(sceNpTrophyGetGameIcon),
    T(sceNpTrophyGetGroupIcon),
    T(sceNpTrophyGetTrophyIcon),
    T(sceNpTrophyShowTrophyList),
    T(sceNpTrophyCaptureScreenshot),
    CONFIG(sceNpTrophyConfigGetTrophyFlagArray),
    CONFIG(sceNpTrophyConfigGetTrophyDetails),
    CONFIG(sceNpTrophyConfigGetTrophySetInfo),
    CONFIG(sceNpTrophyConfigGetTrophyGroupArray),
    CONFIG(sceNpTrophyConfigGetTrophyGroupDetails),
    CONFIG(sceNpTrophyConfigGetTrophySetInfoInGroup),
    CONFIG(sceNpTrophyConfigGetTrophySetVersion),
    CONFIG(sceNpTrophyConfigGetTrophyTitleDetails),
    CONFIG(sceNpTrophyConfigHasGroupFeature),
};

static const struct ext_lib k_libs[] = {
    {"libSceNpTrophy", NULL, EXT_LIB_NO_HOST, T_NOT_SUPPORTED},
};

EXT_FAMILY_LIBS("trophy", k_hle, k_libs, NULL);

#pragma clang attribute pop
