/*
 * Drives the NP and trophy families of a real ext.bin in an x86-64 process
 * (test_np.py).  env.c loads the extension exactly as for test_ext.py
 * (compiled with its main renamed ext_host_main and its API table renamed);
 * this file gives the extension the core services these families use:
 * locks, a fake console (two logged-in users, an NP state of "signed in",
 * the RTC, an InGameMessage function) and /app0 mapped to a host directory.
 *
 *     np_host EXT_BIN CRC APP0_DIR SCENARIO UNLOCK_OFF READ_OFF WRITE_OFF
 *     SCENARIO: persona | requests | callbacks (NpManager, NpAuth, NpUtility,
 *               NpCommerce, NetCtl for the toolkit), or trophies with
 *               ghost (param.sfo says CUSA11456: 77 trophies)
 *             | npid  (no param.sfo; the NP title ID CUSA11456_00 arrives
 *                      after registration, as from NpToolkit2)
 *             | trp   (an unknown title whose trophy00.trp lists 12 trophy
 *                      icons and 2 group icons)
 *             | bare  (no param.sfo, no trp: the count follows the unlocks)
 *     *_OFF: image offsets of sceNpTrophyUnlockTrophy's entry and of
 *            title_store_read / title_store_write (from ext.elf), to reach
 *            the title store the trophy records go to.
 *
 * Prints "np_host: ok|FAILED ..." lines; exits 0 when all passed.
 */
#include "ghost_api.h"

int ext_host_main(int argc, char **argv);
int sys_open(const char *path);
s64 sys_read(int fd, void *buf, u64 n);
int sys_close(int fd);
long sys_lseek(int fd, long off, int whence);
u64 sys_strtoul(const char *s);
void abort(void);

#define USER1 0x10000001
#define USER2 0x10000002
#define USER3 0x10000003
#define USER4 0x10000004
#define TICK 0x00dcbffeeddcba98ULL
#define NPM "libSceNpManager"
#define AUTH "libSceNpAuth"
#define UTIL "libSceNpUtility"
#define COMMERCE "libSceNpCommerce"
#define TROPHY "libSceNpTrophy"
#define NETCTL_TK "libSceNetCtlForNpToolkit"

static int g_failed;

static void expect(bool ok, const char *what)
{
    logf("np_host: %s %s", ok ? "ok" : "FAILED", what);
    if (!ok)
        g_failed = 1;
}

static void expect_eq(s64 got, s64 want, const char *what)
{
    if (got != want)
        logf("np_host: %s: got %lx, want %lx", what, (u64)got, (u64)want);
    expect(got == want, what);
}
/* 32-bit values (return codes) compare as 32 bits: an s32 error is not sign extended */
#define EQ(got, want, what)                                                                                      \
    expect_eq(sizeof(got) <= 4 ? (s64)(u32)(got) : (s64)(got), sizeof(got) <= 4 ? (s64)(u32)(want) : (s64)(want), what)

static bool all_zero(const void *p, u32 n)
{
    for (u32 i = 0; i < n; i++)
        if (((const u8 *)p)[i])
            return false;
    return true;
}

/* ---- the core services ---- */

static void lock(volatile int *l)
{
    for (u64 spins = 0; __atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE); spins++)
        if (spins > 200000000ULL) {
            logf("np_host: FAILED a lock is never released (held across a callback?)");
            abort();
        }
}

static void unlock(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

/* the console */
static u32 g_console_np_calls;
static s32 c_login_list(s32 *list)
{
    list[0] = USER1, list[1] = USER2, list[2] = list[3] = -1;
    return 0;
}
static s32 c_initial_user(s32 *u)
{
    *u = USER1;
    return 0;
}
static s32 c_np_get_state(s32 user, s32 *st)
{
    g_console_np_calls++;
    *st = 2; /* the console's user is signed in */
    return 0;
}
static s32 c_rtc_tick(u64 *t)
{
    *t = TICK;
    return 0;
}
static s64 c_igm_initialize(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) { return (s64)(a + b + f); }

static void *host_real_in_(const char *mod, const char *name)
{
    static const struct {
        const char *mod, *name;
        void *fn;
    } k[] = {
        {"libSceUserService", "sceUserServiceGetLoginUserIdList", c_login_list},
        {"libSceUserService", "sceUserServiceGetInitialUser", c_initial_user},
        {"libSceNpManager", "sceNpGetState", c_np_get_state},
        {"libSceNpManager", "sceNpInGameMessageInitialize", c_igm_initialize},
        {"libSceRtc", "sceRtcGetCurrentTick", c_rtc_tick},
    };
    for (u32 i = 0; i < ARRAY_SIZE(k); i++)
        if (strcmp(k[i].mod, mod) == 0 && strcmp(k[i].name, name) == 0)
            return k[i].fn;
    logf("np_host: FAILED the extension asked the console for %s:%s", mod, name);
    g_failed = 1;
    return NULL;
}

/* /app0 */
static const char *g_root;
static const char *remap(const char *path, char *buf, size_t size)
{
    if (strncmp(path, "/app0/", 6) != 0)
        return path;
    fmt(buf, size, "%s/%s", g_root, path + 6);
    return buf;
}
static int v_open(const char *path, int flags, int mode)
{
    int fd = sys_open(path);
    return fd < 0 ? (int)0x80020002 : fd; /* ENOENT */
}
static s64 v_read(int fd, void *buf, u64 n) { return sys_read(fd, buf, n); }
static s64 v_lseek(int fd, s64 off, int whence) { return sys_lseek(fd, off, whence); }
static int v_close(int fd) { return sys_close(fd); }

const struct ghost_api g_ghost_api = {
    .version = GHOST_API_VERSION,
    .size = sizeof(struct ghost_api),
    .ctx = &g_ctx,
    .k = &K,
    .modules = &g_modules,
    .main_mod = &g_main_mod,
    .reported_dmem = &g_reported_dmem,
    .logf = logf,
    .arena_alloc = arena_alloc,
    .arena_strdup = arena_strdup,
    .compute_nid = compute_nid,
    .spin_lock = lock,
    .spin_unlock = unlock,
    .host_real_in = host_real_in_,
    .remap_path = remap,
    .vfs_open = v_open,
    .vfs_read = v_read,
    .vfs_lseek = v_lseek,
    .vfs_close = v_close,
};

/* ---- the extension's functions ---- */

static s32 missing(void) { return 0x7fffffff; }

static void *fn(const char *lib, const char *name)
{
    char nid[12];
    compute_nid(name, nid);
    void *f = (void *)ext_hle_lookup(lib, nid, name);
    if (!f) {
        logf("np_host: FAILED no extension function %s:%s", lib, name);
        g_failed = 1;
        return (void *)missing;
    }
    return f;
}

#define CALL(type, lib, name) ((type)fn(lib, name))
typedef s32 (*f_v)(void);
typedef s32 (*f_i)(s32);
typedef s32 (*f_p)(void *);
typedef s32 (*f_ii)(s32, s32);
typedef s32 (*f_ip)(s32, void *);
typedef s32 (*f_pp)(void *, void *);
typedef s32 (*f_ipp)(s32, void *, void *);
typedef s32 (*f_iip)(s32, s32, void *);
typedef s32 (*f_iipp)(s32, s32, void *, void *);
typedef s32 (*f_ippp)(s32, void *, void *, void *);
typedef s32 (*f_iiipp)(s32, s32, s32, void *, void *);
typedef s32 (*f_piil)(void *, s32, u32, u64);
typedef s32 (*f_iil)(s32, s32, u64);
typedef s32 (*f_iiip)(s32, s32, s32, void *);

/* ---- persona ---- */

static void persona_tests(void)
{
    s32 st = -1;
    EQ(CALL(f_ip, NPM, "sceNpGetState")(USER1, &st), 0, "sceNpGetState returns 0");
    EQ(st, 1, "sceNpGetState: SIGNED_OUT although the console is signed in");
    EQ(g_console_np_calls, 1, "the console's NP state is read once, for the log");
    CALL(f_ip, NPM, "sceNpGetState")(USER1, &st);
    EQ(g_console_np_calls, 1, "and never again");
    EQ(CALL(f_ip, NPM, "sceNpGetState")(-1, &st), 0x80550003, "sceNpGetState(-1): INVALID_ARGUMENT");
    EQ(CALL(f_ip, NPM, "sceNpGetState")(USER1, NULL), 0x80550003, "sceNpGetState(NULL): INVALID_ARGUMENT");
    expect(fn("libSceNpManagerCompat", "sceNpGetState") == fn(NPM, "sceNpGetState"), "libSceNpManagerCompat alias");

    u8 buf[0x40];
    memset(buf, 0xAA, sizeof(buf));
    EQ(CALL(f_ip, NPM, "sceNpGetOnlineId")(USER1, buf), 0x80550006, "sceNpGetOnlineId: SIGNED_OUT");
    expect(all_zero(buf, 0x14) && buf[0x14] == 0xAA, "sceNpGetOnlineId zeroes exactly SceNpOnlineId");
    memset(buf, 0xAA, sizeof(buf));
    EQ(CALL(f_ip, NPM, "sceNpGetNpId")(USER1, buf), 0x80550006, "sceNpGetNpId: SIGNED_OUT");
    expect(all_zero(buf, 0x24) && buf[0x24] == 0xAA, "sceNpGetNpId zeroes exactly SceNpId");
    u64 acc = 0x1234;
    EQ(CALL(f_ip, NPM, "sceNpGetAccountIdA")(USER1, &acc), 0x80550006, "sceNpGetAccountIdA: SIGNED_OUT");
    EQ(acc, 0, "sceNpGetAccountIdA: account 0");
    memset(buf, 0xAA, sizeof(buf));
    EQ(CALL(f_ip, NPM, "sceNpGetAccountCountryA")(USER1, buf), 0x80550006, "sceNpGetAccountCountryA: SIGNED_OUT");
    expect(all_zero(buf, 4) && buf[4] == 0xAA, "sceNpGetAccountCountryA zeroes the country");
    EQ(CALL(f_ip, NPM, "sceNpGetOnlineId")(-1, buf), 0x80550003, "sceNpGetOnlineId(-1): INVALID_ARGUMENT");
    bool b = false;
    EQ(CALL(f_ip, NPM, "sceNpHasSignedUp")(USER1, &b), 0, "sceNpHasSignedUp returns 0");
    EQ(b, 1, "sceNpHasSignedUp: signed up");
    b = true;
    EQ(CALL(f_ip, NPM, "sceNpIsPlusMember")(USER1, &b), 0, "sceNpIsPlusMember returns 0");
    EQ(b, 0, "sceNpIsPlusMember: no");
    s32 reach = 7;
    EQ(CALL(f_ip, NPM, "sceNpGetNpReachabilityState")(USER1, &reach), 0, "sceNpGetNpReachabilityState returns 0");
    EQ(reach, 0, "reachability UNAVAILABLE");
    acc = 5;
    EQ(CALL(f_pp, NPM, "sceNpGetAccountId")(buf, &acc), 0x80550007, "sceNpGetAccountId(online ID): USER_NOT_FOUND");
    EQ(acc, 0, "sceNpGetAccountId zeroes the account");
    s32 user = 77;
    EQ(CALL(f_ip, NPM, "sceNpGetUserIdByAccountId")(12345, &user), 0x80550006, "sceNpGetUserIdByAccountId");
    EQ(user, 77, "sceNpGetUserIdByAccountId leaves the user untouched");

    u8 restriction[0x18] = {0};
    *(u64 *)restriction = 0x18;
    restriction[8] = 12;
    EQ(CALL(f_p, NPM, "sceNpSetContentRestriction")(restriction), 0, "sceNpSetContentRestriction kept");
    *(u64 *)restriction = 0x10;
    EQ(CALL(f_p, NPM, "sceNpSetContentRestriction")(restriction), 0x80550011, "content restriction: INVALID_SIZE");
    char title[16] = "NPWR00000_00";
    EQ(CALL(f_pp, NPM, "sceNpSetNpTitleId")(title, buf), 0, "sceNpSetNpTitleId kept");
    EQ(CALL(f_pp, NPM, "sceNpSetNpTitleId")(title, NULL), 0x80550003, "sceNpSetNpTitleId(no secret)");
    s64 r = ((s64(*)(u64, u64, u64, u64, u64, u64))fn(NPM, "sceNpInGameMessageInitialize"))(1, 2, 0, 0, 0, 3);
    EQ(r, 6, "sceNpInGameMessageInitialize passes through with its arguments");
    EQ(CALL(f_v, NPM, "sceNpInGameMessageSendDataA")(), 0x80550006, "sceNpInGameMessageSendDataA: SIGNED_OUT");
}

/* ---- requests ---- */

struct check_plus_param {
    u64 size;
    s32 user, pad;
    u64 features;
    u8 reserved[32];
};

static void request_tests(void)
{
    f_v create = CALL(f_v, NPM, "sceNpCreateRequest");
    f_p create_async = CALL(f_p, NPM, "sceNpCreateAsyncRequest");
    f_i del = CALL(f_i, NPM, "sceNpDeleteRequest"), abort_req = CALL(f_i, NPM, "sceNpAbortRequest");
    f_ip poll = CALL(f_ip, NPM, "sceNpPollAsync"), wait = CALL(f_ip, NPM, "sceNpWaitAsync");
    f_ii avail = CALL(f_ii, NPM, "sceNpCheckNpAvailabilityA");
    f_ipp plus = CALL(f_ipp, NPM, "sceNpCheckPlus");

    s32 r1 = create(), r2 = create();
    EQ(r1, 0x20000001, "the first NP request is 0x20000001");
    EQ(r2, 0x20000002, "the next takes the next slot");
    EQ(avail(r1, USER1), 0x80550006, "sync availability check: SIGNED_OUT");
    EQ(avail(r1, USER1), 0x80550015, "one operation per request: INVALID_ID");
    s32 res = 0;
    EQ(poll(r1, &res), 0x80550015, "polling a sync request: INVALID_ID");
    EQ(avail(r2, -1), 0x80550003, "user -1: INVALID_ARGUMENT");
    EQ(avail(0x20000040, USER1), 0x80550014, "unknown request: REQUEST_NOT_FOUND");

    u64 param[3] = {0x10, 0, 0};
    EQ(create_async(NULL), 0x80550003, "async request without a parameter");
    EQ(create_async(param), 0x80550011, "async request parameter size");
    param[0] = 0x18;
    s32 ra = create_async(param);
    EQ(ra, 0x20000003, "async request created");
    EQ(poll(ra, &res), 0x80550015, "polling before the operation: INVALID_ID");
    struct check_plus_param pp = {sizeof(pp), USER1, 0, 1, {0}};
    u8 result[0x21];
    memset(result, 0xAA, sizeof(result));
    EQ(plus(ra, &pp, result), 0, "async CheckPlus returns 0");
    expect(all_zero(result, 0x21), "CheckPlus zeroes the result: not authorized");
    res = 0;
    EQ(poll(ra, &res), 0, "PollAsync: finished");
    EQ(res, 0x80550006, "PollAsync: the result is SIGNED_OUT");
    res = 0;
    EQ(wait(ra, &res), 0, "WaitAsync never blocks");
    EQ(res, 0x80550006, "WaitAsync: SIGNED_OUT");
    EQ(poll(ra, NULL), 0x80550003, "PollAsync(NULL)");
    pp.size = 0x30;
    EQ(plus(r2, &pp, result), 0x80550011, "CheckPlus parameter size");
    EQ(abort_req(r2), 0, "abort");
    pp.size = sizeof(pp);
    EQ(plus(r2, &pp, result), 0x80550012, "an operation on an aborted request: ABORTED");
    EQ(abort_req(ra), 0, "aborting a finished request is ignored");
    res = 0;
    EQ(poll(ra, &res), 0, "and its result stays");
    s8 age = 9;
    u8 info[4];
    memset(info, 0xAA, sizeof(info));
    s32 r3 = create();
    EQ(((s32(*)(s32, s32, s8 *, void *))fn(NPM, "sceNpGetParentalControlInfoA"))(r3, USER1, &age, info), 0x80550006,
       "parental control: SIGNED_OUT");
    expect(age == 0 && all_zero(info, 3) && info[3] == 0xAA, "parental control: age 0, info zeroed");
    for (s32 id = 0x20000001; id <= 0x20000004; id++)
        EQ(del(id), 0, "delete");
    EQ(del(r1), 0x80550014, "delete twice: REQUEST_NOT_FOUND");
    EQ(((s32(*)(s32, s32, u32, u32, u32, u32))fn(NPM, "sceNpSetTimeout"))(0, 0, 1, 1, 1, 1), 0x80550003,
       "SetTimeout(0): INVALID_ARGUMENT");
    s32 ids[33];
    for (u32 i = 0; i < 33; i++)
        ids[i] = create();
    EQ(ids[0], 0x20000001, "slots are reused from the lowest");
    EQ(ids[31], 0x20000020, "32 requests");
    EQ(ids[32], 0x80550013, "the 33rd: REQUEST_MAX");
    for (u32 i = 0; i < 32; i++)
        del(ids[i]);

    /* NpAuth */
    f_v acreate = CALL(f_v, AUTH, "sceNpAuthCreateRequest");
    f_p acreate_async = CALL(f_p, AUTH, "sceNpAuthCreateAsyncRequest");
    f_ippp code = CALL(f_ippp, AUTH, "sceNpAuthGetAuthorizationCodeV3");
    f_ipp token = CALL(f_ipp, AUTH, "sceNpAuthGetIdTokenV3");
    char client[129] = "c0ffee00-0000-0000-0000-000000000000", scope[] = "psn:s2s", secret[257] = "s";
    struct {
        u64 size;
        s32 user, pad;
        const char *client, *scope;
    } cp = {0x20, USER1, 0, client, scope};
    struct {
        u64 size;
        s32 user, pad;
        const char *client, *secret, *scope;
    } tp = {0x28, USER1, 0, client, secret, scope};
    s32 a1 = acreate();
    EQ(a1, 0x10000001, "the first auth request is 0x10000001");
    static u8 out[0x1100];
    memset(out, 0xAA, sizeof(out));
    s32 issuer = 5;
    EQ(code(a1, &cp, out, &issuer), 0x80550006, "authorization code: SIGNED_OUT");
    expect(all_zero(out, 0x88) && out[0x88] == 0xAA && issuer == 0, "the code and issuer are zeroed");
    s32 a2 = acreate();
    cp.size = 0x18;
    EQ(code(a2, &cp, out, &issuer), 0x80550302, "auth parameter size: INVALID_SIZE");
    EQ(code(a2, NULL, out, &issuer), 0x80550301, "auth parameter NULL: INVALID_ARGUMENT");
    param[0] = 0x18;
    s32 a3 = acreate_async(param);
    memset(out, 0xAA, sizeof(out));
    EQ(token(a3, &tp, out), 0, "async ID token returns 0");
    expect(all_zero(out, 0x1008) && out[0x1008] == 0xAA, "the ID token is zeroed");
    res = 0;
    EQ(CALL(f_ip, AUTH, "sceNpAuthPollAsync")(a3, &res), 0, "auth poll: finished");
    EQ(res, 0x80550006, "auth poll: SIGNED_OUT");
    for (s32 id = a1; id <= a3; id++)
        EQ(CALL(f_i, AUTH, "sceNpAuthDeleteRequest")(id), 0, "auth delete");
    expect(fn("libSceNpAuthCompat", "sceNpAuthCreateRequest") == (void *)acreate, "libSceNpAuthCompat alias");

    /* word filter: a title context owns its requests */
    s32 ctx = CALL(f_i, UTIL, "sceNpWordFilterCreateTitleCtxA")(USER1);
    EQ(ctx, 0x01000001, "word filter title context");
    s32 w1 = CALL(f_i, UTIL, "sceNpWordFilterCreateRequest")(ctx);
    EQ(w1, 0x02000001, "word filter request");
    EQ(CALL(f_i, UTIL, "sceNpWordFilterCreateRequest")(0x01000005), 0x8055070E, "request on an unknown context");
    char comment[] = "Jin Sakai", sanitized[64];
    memset(sanitized, 0xAA, sizeof(sanitized));
    EQ(CALL(f_ippp, UTIL, "sceNpWordFilterSanitizeComment")(w1, comment, sanitized, NULL), 0, "sanitize: 0");
    expect(strcmp(sanitized, comment) == 0, "sanitize copies the comment unchanged");
    s32 w2 = CALL(f_i, UTIL, "sceNpWordFilterCreateRequest")(ctx);
    EQ(CALL(f_ipp, UTIL, "sceNpWordFilterCensorComment")(w2, comment, sanitized), 0x80550704, "censor with an option");
    EQ(CALL(f_i, UTIL, "sceNpWordFilterDeleteTitleCtx")(ctx), 0, "delete the context");
    EQ(CALL(f_i, UTIL, "sceNpWordFilterDeleteRequest")(w1), 0x8055070E, "its requests went with it");
    EQ(CALL(f_v, UTIL, "sceNpBandwidthTestInitStart")(), 0x80550006, "bandwidth test: SIGNED_OUT");

    /* the commerce dialog closes at once */
    f_v init = CALL(f_v, COMMERCE, "sceNpCommerceDialogInitialize");
    f_p open = CALL(f_p, COMMERCE, "sceNpCommerceDialogOpen"), get = CALL(f_p, COMMERCE, "sceNpCommerceDialogGetResult");
    f_v update = CALL(f_v, COMMERCE, "sceNpCommerceDialogUpdateStatus");
    EQ(init(), 0, "commerce dialog initialize");
    EQ(init(), 0x80B80004, "initialize twice: ALREADY_INITIALIZED");
    u8 res_buf[0x30];
    EQ(get(res_buf), 0x80B80005, "result before it ran: NOT_FINISHED");
    u8 cdp[0x80] = {0};
    const char *targets[1] = {"EP0000-CUSA00000_00-TEST"};
    *(s32 *)(cdp + 0x38) = 1;
    *(const char *const **)(cdp + 0x40) = targets;
    *(u32 *)(cdp + 0x48) = 1;
    *(u64 *)(cdp + 0x58) = 0x5eed;
    EQ(open(NULL), 0x80B8000D, "open(NULL): ARG_NULL");
    EQ(open(cdp), 0, "open");
    EQ(CALL(f_v, COMMERCE, "sceNpCommerceDialogGetStatus")(), 2, "running");
    EQ(update(), 3, "finished on the next update");
    memset(res_buf, 0xAA, sizeof(res_buf));
    EQ(get(res_buf), 0, "result");
    EQ(*(s32 *)res_buf, 1, "the user canceled");
    expect(res_buf[4] == 0 && *(u64 *)(res_buf + 8) == 0x5eed && all_zero(res_buf + 0x10, 0x20),
           "not authorized, the userdata echoed, the rest zeroed");
    EQ(CALL(f_v, COMMERCE, "sceNpCommerceDialogClose")(), 0x80B8000C, "close after it finished: ALREADY_CLOSE");
    EQ(CALL(f_v, COMMERCE, "sceNpCommerceDialogTerminate")(), 0, "terminate");
    EQ(CALL(f_v, COMMERCE, "sceNpCommerceDialogTerminate")(), 0x80B80003, "terminate twice: NOT_INITIALIZED");
}

/* ---- callbacks ---- */

static bool g_in_check;
static u32 g_a_calls, g_outside, g_reach_calls, g_tk_calls, g_legacy_calls;
static s32 g_a_users[8];

static void cb_a(s32 user, s32 state, void *ud)
{
    g_outside += !g_in_check;
    if (g_a_calls < 8)
        g_a_users[g_a_calls] = user;
    g_a_calls++;
    expect(state == 1 && ud == (void *)0xabc, "the A callback: SIGNED_OUT and its userdata");
    s32 st = -1; /* re-entry: no lock is held while a callback runs */
    expect(CALL(f_ip, NPM, "sceNpGetState")(user, &st) == 0 && st == 1, "a callback can call NpManager again");
}

static void cb_a2(s32 user, s32 state, void *ud) { g_outside += 1000; }

static void cb_legacy(s32 user, s32 state, void *np_id, void *ud) { g_legacy_calls++; }

static void cb_reach(s32 user, s32 reach, void *ud)
{
    g_outside += !g_in_check;
    g_reach_calls++;
    expect(reach == 0 && ud == (void *)0xdef, "the reachability callback: UNAVAILABLE");
}

static void cb_toolkit(s32 user, s32 state, void *ud)
{
    g_outside += !g_in_check;
    g_tk_calls++;
    expect(state == 1 && ud == (void *)0x70, "the toolkit callback: SIGNED_OUT");
}

static void cb_netctl(s32 event, void *arg) { g_outside += 100; }

static void callback_tests(void)
{
    f_pp reg_a = CALL(f_pp, NPM, "sceNpRegisterStateCallbackA");
    f_v check = CALL(f_v, NPM, "sceNpCheckCallback");
    EQ(reg_a((void *)cb_a, (void *)0xabc), 1, "the first A callback gets ID 1");
    EQ(reg_a((void *)cb_a, (void *)0xabc), 0x80550008, "the same callback again: ALREADY_REGISTERED");
    EQ(reg_a(NULL, NULL), 0x80550003, "NULL callback");
    EQ(CALL(f_pp, NPM, "sceNpRegisterNpReachabilityStateCallback")((void *)cb_reach, (void *)0xdef), 0,
       "reachability callback");
    EQ(g_a_calls + g_reach_calls, 0, "nothing is called at registration");
    g_in_check = true;
    EQ(check(), 0, "sceNpCheckCallback");
    g_in_check = false;
    EQ(g_a_calls, 2, "one SIGNED_OUT event per logged-in user");
    expect(g_a_users[0] == USER1 && g_a_users[1] == USER2, "in the login list's order");
    EQ(g_reach_calls, 2, "one UNAVAILABLE per user");
    g_in_check = true;
    check();
    g_in_check = false;
    EQ(g_a_calls + g_reach_calls, 4, "and nothing more");
    EQ(CALL(f_pp, NPM, "sceNpRegisterStateCallback")((void *)cb_legacy, NULL), 0, "legacy callback");
    EQ(CALL(f_pp, NPM, "sceNpRegisterStateCallback")((void *)cb_legacy, NULL), 0x80550008, "one legacy slot");
    check();
    EQ(g_legacy_calls, 0, "the initial events were already delivered");
    EQ(CALL(f_i, NPM, "sceNpUnregisterStateCallbackA")(1), 0, "unregister A");
    EQ(CALL(f_i, NPM, "sceNpUnregisterStateCallbackA")(1), 0x80550009, "unregister twice: NOT_REGISTERED");
    EQ(CALL(f_i, NPM, "sceNpUnregisterStateCallbackA")(9), 0x80550003, "unregister ID 9: INVALID_ARGUMENT");
    EQ(reg_a((void *)cb_a2, NULL), 1, "ID 1 is free again");

    /* NpToolkit2's callbacks: its own queue, pumped by sceNpCheckCallbackForLib */
    void *check_lib = fn("libSceNpManagerForToolkit", "sceNpCheckCallbackForLib");
    expect(check_lib == fn(NPM, "sceNpCheckCallbackForLib"), "libSceNpManagerForToolkit alias");
    EQ(CALL(f_pp, "libSceNpManagerForToolkit", "sceNpRegisterStateCallbackForToolkit")((void *)cb_toolkit,
                                                                                       (void *)0x70),
       1, "the toolkit callback (returns 1)");
    check();
    EQ(g_tk_calls, 0, "sceNpCheckCallback does not deliver the toolkit's events");
    g_in_check = true;
    EQ(((f_v)check_lib)(), 0, "sceNpCheckCallbackForLib");
    g_in_check = false;
    EQ(g_tk_calls, 2, "the toolkit gets one SIGNED_OUT per user");
    ((f_v)check_lib)();
    EQ(g_tk_calls, 2, "once");

    /* NetCtl for the toolkit: IDs written, events never delivered */
    typedef s32 (*f_ppp)(void *, void *, void *);
    f_ppp reg = CALL(f_ppp, NETCTL_TK, "sceNetCtlRegisterCallbackForNpToolkit");
    s32 cid = -5, cid2 = -5;
    EQ(reg((void *)cb_netctl, NULL, &cid), 0, "NetCtl toolkit callback");
    EQ(cid, 0, "its ID is written: 0");
    EQ(reg((void *)cb_netctl, NULL, &cid2), 0, "a second one");
    EQ(cid2, 1, "ID 1");
    EQ(reg((void *)cb_netctl, NULL, NULL), 0x80412107, "no ID pointer: INVALID_ADDR");
    EQ(CALL(f_v, NETCTL_TK, "sceNetCtlCheckCallbackForNpToolkit")(), 0, "NetCtl toolkit check");
    EQ(CALL(f_i, NETCTL_TK, "sceNetCtlUnregisterCallbackForNpToolkit")(cid), 0, "unregister by the ID written");
    EQ(CALL(f_i, NETCTL_TK, "sceNetCtlUnregisterCallbackForNpToolkit")(8), 0x80412105, "ID 8: INVALID_ID");
    EQ(g_outside, 0, "callbacks ran only inside the check calls");
}

/* ---- trophies ---- */

static u64 g_base, g_read_off, g_write_off;

static s64 store_read(const char *name, void *buf, u64 len)
{
    return ((s64(*)(const char *, void *, u64))(g_base + g_read_off))(name, buf, len);
}

static s32 store_write(const char *name, const void *buf, u64 len)
{
    return ((s32(*)(const char *, const void *, u64))(g_base + g_write_off))(name, buf, len);
}

struct game_data {
    u64 size;
    u32 unlocked, platinum, gold, silver, bronze, progress;
};

static s32 g_ctx1, g_handle;

/* a registered context for user */
static s32 open_set(s32 user)
{
    s32 ctx = -1;
    EQ(CALL(f_piil, TROPHY, "sceNpTrophyCreateContext")(&ctx, user, 0, 0), 0, "create a context");
    EQ(CALL(f_iil, TROPHY, "sceNpTrophyRegisterContext")(ctx, g_handle, 0), 0, "register it");
    return ctx;
}

static void trophy_tests(const char *scenario)
{
    bool npid = strcmp(scenario, "npid") == 0, ghost = strcmp(scenario, "ghost") == 0 || npid;
    bool trp = strcmp(scenario, "trp") == 0;
    u32 total = ghost ? 77 : trp ? 12 : 1;
    f_p create_handle = CALL(f_p, TROPHY, "sceNpTrophyCreateHandle");
    f_piil create = CALL(f_piil, TROPHY, "sceNpTrophyCreateContext");
    f_iil reg = CALL(f_iil, TROPHY, "sceNpTrophyRegisterContext");
    f_iiip unlock = CALL(f_iiip, TROPHY, "sceNpTrophyUnlockTrophy");
    f_iipp state = CALL(f_iipp, TROPHY, "sceNpTrophyGetTrophyUnlockState");
    g_base = (u64)fn(TROPHY, "sceNpTrophyUnlockTrophy") - g_base;

    EQ(create_handle(&g_handle), 0, "create a handle");
    EQ(g_handle, 1, "handle 1");
    EQ(create(&g_ctx1, USER1, 0, 0), 0, "create a context");
    EQ(g_ctx1, 1, "context 1");
    s32 other = -1;
    EQ(create(&other, USER1, 0, 0), 0x80551613, "same user and label: CONTEXT_ALREADY_EXISTS");
    EQ(create(&other, USER1, 1, 1), 0x80551604, "options: INVALID_ARGUMENT");
    s32 plat = 0;
    EQ(unlock(g_ctx1, g_handle, 3, &plat), 0x8055160F, "unlock before registering: NOT_REGISTERED");
    EQ(unlock(-1, g_handle, 3, &plat), 0x80551609, "context -1: INVALID_CONTEXT");
    EQ(unlock(g_ctx1, -1, 3, &plat), 0x80551608, "handle -1: INVALID_HANDLE");
    EQ(reg(g_ctx1, g_handle, 0), 0, "register");
    EQ(reg(g_ctx1, g_handle, 0), 0x80551610, "register twice: ALREADY_REGISTERED");
    if (npid) { /* no param.sfo (Ghost's dump has none): NpToolkit2 sets the NP title ID later */
        char np_title[16] = "CUSA11456_00", secret[128] = {0};
        EQ(CALL(f_pp, NPM, "sceNpSetNpTitleId")(np_title, secret), 0, "the toolkit sets the NP title ID");
    }

    u32 flags[4] = {9, 9, 9, 9}, count = 0;
    EQ(state(g_ctx1, g_handle, flags, &count), 0, "unlock state");
    expect(all_zero(flags, sizeof(flags)), "nothing unlocked yet");
    EQ(count, total, ghost ? "77 trophies (the table)" : trp ? "12 trophies (the TRP icons)" : "count 1, never 0");

    EQ(unlock(g_ctx1, g_handle, 5, &plat), 0, "unlock trophy 5");
    EQ(plat, -1, "no platinum");
    EQ(unlock(g_ctx1, g_handle, 5, &plat), 0x8055160C, "a duplicate unlock: ALREADY_UNLOCKED");
    EQ(unlock(g_ctx1, g_handle, 3, NULL), 0x80551604, "no platinum pointer: INVALID_ARGUMENT");
    EQ(unlock(g_ctx1, g_handle, -1, &plat), 0x8055160A, "trophy -1: INVALID_TROPHY_ID");
    EQ(unlock(g_ctx1, g_handle, 128, &plat), 0x8055160A, "trophy 128: INVALID_TROPHY_ID");
    if (!ghost && !trp)
        EQ(unlock(g_ctx1, g_handle, 9, &plat), 0, "unknown count: any trophy below 128");
    else
        EQ(unlock(g_ctx1, g_handle, (s32)total, &plat), 0x8055160A, "a trophy past the count: INVALID_TROPHY_ID");
    EQ(state(g_ctx1, g_handle, flags, &count), 0, "unlock state");
    EQ(flags[0], ghost || trp ? 1u << 5 : (1u << 5) | (1u << 9), "trophy 5 is set");
    EQ(count, ghost || trp ? total : 10, ghost || trp ? "the count stays" : "the count follows the unlocks");

    struct {
        u64 size;
        s32 id, grade, group;
        u8 hidden, pad[3];
        char name[128], description[1024];
    } det;
    struct {
        u64 size;
        s32 id;
        u8 unlocked, pad[3];
        u64 timestamp;
    } data;
    memset(&det, 0xAA, sizeof(det));
    memset(&data, 0xAA, sizeof(data));
    det.size = sizeof(det), data.size = sizeof(data);
    EQ(CALL(f_iiipp, TROPHY, "sceNpTrophyGetTrophyInfo")(g_ctx1, g_handle, 5, &det, &data), 0, "trophy info");
    expect(data.id == 5 && data.unlocked == 1 && data.timestamp == TICK, "unlocked, with the RTC time");
    expect(det.id == 5 && det.grade == 0 && det.group == -1 && !det.hidden && !det.name[0] && !det.description[0],
           "details: grade unknown, base game, no name");
    EQ(CALL(f_iiipp, TROPHY, "sceNpTrophyGetTrophyInfo")(g_ctx1, g_handle, 4, NULL, &data), 0, "one output is enough");
    EQ(data.unlocked, 0, "trophy 4 is locked");
    data.size = 8;
    EQ(CALL(f_iiipp, TROPHY, "sceNpTrophyGetTrophyInfo")(g_ctx1, g_handle, 4, NULL, &data), 0x80551604,
       "a wrong size field");
    EQ(CALL(f_iiipp, TROPHY, "sceNpTrophyGetTrophyInfo")(g_ctx1, g_handle, 4, NULL, NULL), 0x80551604, "no output");

    static u8 details[0x4A0];
    struct game_data gd;
    memset(details, 0xAA, sizeof(details));
    *(u64 *)details = sizeof(details), gd.size = sizeof(gd);
    EQ(CALL(f_iipp, TROPHY, "sceNpTrophyGetGameInfo")(g_ctx1, g_handle, details, &gd), 0, "game info");
    EQ(*(u32 *)(details + 0xC), ghost || trp ? total : 10, "numTrophies");
    EQ(*(u32 *)(details + 8), trp ? 2 : 0, trp ? "numGroups: 2 group icons" : "numGroups unknown");
    expect(!ghost || strcmp((char *)details + 0x20, "CUSA11456") == 0, "the title is the title ID");
    EQ(gd.unlocked, ghost || trp ? 1 : 2, "unlocked trophies");
    EQ(gd.progress, ghost ? 1 : trp ? 8 : 20, "progress percentage");
    u8 group_data[0x28];
    *(u64 *)group_data = sizeof(group_data);
    EQ(CALL(f_iiipp, TROPHY, "sceNpTrophyGetGroupInfo")(g_ctx1, g_handle, -1, details, group_data), 0,
       "group -1 is the whole set");
    EQ(*(u32 *)(details + 0xC), ghost || trp ? total : 10, "the group's trophies");
    EQ(CALL(f_iiipp, TROPHY, "sceNpTrophyGetGroupInfo")(g_ctx1, g_handle, -2, details, group_data), 0x8055160B,
       "group -2: INVALID_GROUP_ID");
    EQ(CALL(f_iiipp, TROPHY, "sceNpTrophyGetGroupInfo")(g_ctx1, g_handle, 2, details, group_data),
       trp ? 0x8055160B : 0, trp ? "group 2 of 2: INVALID_GROUP_ID" : "unknown group count: any group");

    u64 size = 0;
    typedef s32 (*f_iiipp2)(s32, s32, s32, void *, u64 *);
    f_iiipp2 trophy_icon = CALL(f_iiipp2, TROPHY, "sceNpTrophyGetTrophyIcon");
    EQ(trophy_icon(g_ctx1, g_handle, 5, NULL, &size), 0x80551614, "no icons: ICON_FILE_NOT_FOUND");
    EQ(trophy_icon(g_ctx1, g_handle, 6, NULL, &size), 0x8055161A, "a locked trophy's icon: TROPHY_NOT_UNLOCKED");
    EQ(CALL(f_iipp, TROPHY, "sceNpTrophyGetGameIcon")(g_ctx1, g_handle, NULL, NULL), 0x80551604, "no size pointer");
    EQ(CALL(f_ii, TROPHY, "sceNpTrophyShowTrophyList")(g_ctx1, g_handle), 0, "the trophy list shows nothing");
    EQ(CALL(f_i, TROPHY, "sceNpTrophyAbortHandle")(g_handle), 0, "abort the handle");
    EQ(CALL(f_i, TROPHY, "sceNpTrophyAbortHandle")(3), 0x80551608, "abort an unknown handle");
    EQ(CALL(f_v, TROPHY, "sceNpTrophyConfigGetTrophySetInfo")(), 0x80551622, "trophy config: NOT_SUPPORTED");

    /* the record in the title store */
    static u8 rec[2048];
    EQ(store_read("trophy_10000001.bin", rec, sizeof(rec)), 1072, "the record is 1072 bytes");
    EQ(*(u32 *)rec, 0x50525447, "magic GTRP");
    EQ(*(u32 *)(rec + 4), 1, "version 1");
    expect(strcmp((char *)rec + 8, ghost ? "CUSA11456" : trp ? "CUSA00001" : "") == 0, "the title ID");
    EQ(*(u32 *)(rec + 0x18), (ghost && !npid) || trp ? total : 0, "count hint (when known at registration)");
    EQ(*(u32 *)(rec + 0x20), flags[0], "the unlocked set at 0x20");
    EQ(*(u64 *)(rec + 0x30 + 5 * 8), TICK, "trophy 5's tick at 0x30 + 8 * 5");
    EQ(*(u64 *)(rec + 0x30 + 4 * 8), 0, "a locked trophy's tick is 0");

    /* a record already in the store is read back at registration */
    memset(rec, 0, 1072);
    *(u32 *)rec = 0x50525447, *(u32 *)(rec + 4) = 1;
    *(u32 *)(rec + 0x20) = 1u << 7, *(u64 *)(rec + 0x30 + 7 * 8) = 42;
    EQ(store_write("trophy_10000003.bin", rec, 1072), 0, "store a record for user 3");
    s32 c3 = open_set(USER3);
    EQ(state(c3, g_handle, flags, &count), 0, "user 3's state");
    EQ(flags[0], 1u << 7, "read back from the store");
    EQ(unlock(c3, g_handle, 7, &plat), 0x8055160C, "a stored unlock is ALREADY_UNLOCKED");
    EQ(store_write("trophy_10000004.bin", "junk", 4), 0, "store a bad record for user 4");
    s32 c4 = open_set(USER4);
    EQ(state(c4, g_handle, flags, &count), 0, "user 4's state");
    EQ(flags[0], 0, "a bad record starts empty");

    /* the record outlives its context */
    EQ(CALL(f_i, TROPHY, "sceNpTrophyDestroyContext")(g_ctx1), 0, "destroy the context");
    EQ(CALL(f_i, TROPHY, "sceNpTrophyDestroyContext")(g_ctx1), 0x80551609, "destroy twice: INVALID_CONTEXT");
    s32 again = open_set(USER1);
    EQ(again, 1, "the slot is reused");
    EQ(unlock(again, g_handle, 5, &plat), 0x8055160C, "still unlocked");

    /* limits */
    s32 h = 0, n_handles = 1;
    while (n_handles < 8 && create_handle(&h) == 0)
        n_handles++;
    EQ(n_handles, 4, "4 handles");
    EQ(create_handle(&h), 0x80551624, "HANDLE_EXCEEDS_MAX");
    s32 n_ctx = 3, c = 0;
    for (u32 label = 10; n_ctx < 12 && create(&c, USER2, label, 0) == 0; label++)
        n_ctx++;
    EQ(n_ctx, 8, "8 contexts");
    EQ(create(&c, USER2, 99, 0), 0x80551623, "CONTEXT_EXCEEDS_MAX");
}

int main(int argc, char **argv)
{
    if (argc < 8) {
        logf("usage: np_host EXT_BIN CRC APP0_DIR SCENARIO UNLOCK_OFF READ_OFF WRITE_OFF");
        return 2;
    }
    char *load[] = {"ext_host", "small", argv[1], argv[2], NULL};
    if (ext_host_main(4, load) != 0) {
        logf("np_host: FAILED loading the extension");
        return 1;
    }
    g_root = argv[3];
    g_base = sys_strtoul(argv[5]); /* the entry's offset: trophy_tests turns it into the image base */
    g_read_off = sys_strtoul(argv[6]), g_write_off = sys_strtoul(argv[7]);
    const char *scenario = argv[4];
    if (strcmp(scenario, "persona") == 0)
        persona_tests();
    else if (strcmp(scenario, "requests") == 0)
        request_tests();
    else if (strcmp(scenario, "callbacks") == 0)
        callback_tests();
    else
        trophy_tests(scenario);
    logf("np_host: %s", g_failed ? "FAILED" : "all passed");
    return g_failed;
}
