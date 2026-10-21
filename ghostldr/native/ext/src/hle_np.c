/*
 * Core NP (PSN) libraries: libSceNpManager (with its Compat and ForToolkit
 * names), libSceNetCtlForNpToolkit, libSceNpAuth, libSceNpCommon,
 * libSceNpUtility and the NpCommerce dialog.  Trophies are in hle_trophy.c.
 *
 * Persona "signed up, signed out, network up": every local user has a PSN
 * account and is signed out.  sceNpGetState says SIGNED_OUT, identity getters
 * fail with SCE_NP_ERROR_SIGNED_OUT and zero their outputs, and every request
 * that needs PSN completes with SIGNED_OUT.  Titles must boot and play signed
 * out, so this path is always tested, and Ghost and NpToolkit2 skip all their
 * online work on it (no availability check, no PSN error dialog).  The
 * network itself (libSceNetCtl) stays the console's.
 *
 * Nothing here reaches SWRR's own NpManager, NpAuth or NpCommerce state: the
 * libraries have no host fallback (policies at the end), so imports this file
 * does not implement get a stub returning the library's error.  Exceptions:
 * the InGameMessage pool and handle calls, which are local setup, and
 * libSceNpCommon, whose utilities carry no identity.
 *
 * Callbacks run only inside the Check functions the title calls
 * (sceNpCheckCallback, sceNpCheckCallbackForLib), on the caller's thread,
 * with no lock held.  The only events are one SIGNED_OUT per logged-in user,
 * queued when the first state callback is registered, and one UNAVAILABLE
 * reachability per user.
 */
#include "ext.h"
#include "np_async.h"

/* none of this is hot: optimize for size (the extension shares a 48 KiB area) */
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

#define E(x) ((s32)(x))
/* libSceNpManager */
#define NP_INVALID_ARGUMENT E(0x80550003)
#define NP_SIGNED_OUT E(0x80550006)
#define NP_USER_NOT_FOUND E(0x80550007)
#define NP_CALLBACK_ALREADY_REGISTERED E(0x80550008)
#define NP_CALLBACK_NOT_REGISTERED E(0x80550009)
#define NP_INVALID_SIZE E(0x80550011)
#define NP_ABORTED E(0x80550012)
#define NP_REQUEST_MAX E(0x80550013)
#define NP_REQUEST_NOT_FOUND E(0x80550014)
#define NP_INVALID_ID E(0x80550015)
#define NP_CALLBACK_MAX E(0x8055001D)
/* libSceNpAuth */
#define AUTH_INVALID_ARGUMENT E(0x80550301)
#define AUTH_INVALID_SIZE E(0x80550302)
#define AUTH_ABORTED E(0x80550304)
#define AUTH_REQUEST_MAX E(0x80550305)
#define AUTH_REQUEST_NOT_FOUND E(0x80550306)
#define AUTH_INVALID_ID E(0x80550307)
/* community (NpUtility word filter and lookup) */
#define COMM_INVALID_ARGUMENT E(0x80550704)
#define COMM_TOO_MANY_OBJECTS E(0x80550706)
#define COMM_ABORTED E(0x80550707)
#define COMM_INSUFFICIENT_ARGUMENT E(0x8055070C)
#define COMM_INVALID_ID E(0x8055070E)
/* libSceNetCtl */
#define NETCTL_CALLBACK_MAX E(0x80412103)
#define NETCTL_INVALID_ID E(0x80412105)
#define NETCTL_INVALID_ADDR E(0x80412107)
/* common dialog */
#define CD_NOT_INITIALIZED E(0x80B80003)
#define CD_ALREADY_INITIALIZED E(0x80B80004)
#define CD_NOT_FINISHED E(0x80B80005)
#define CD_PARAM_INVALID E(0x80B8000A)
#define CD_NOT_RUNNING E(0x80B8000B)
#define CD_ALREADY_CLOSE E(0x80B8000C)
#define CD_ARG_NULL E(0x80B8000D)
#define CD_NOT_SUPPORTED E(0x80B8000F)

#define NP_STATE_SIGNED_OUT 1
#define NP_REACH_UNAVAILABLE 0
#define USER_INVALID (-1)

/* Debug options (spec §1.3, §4.2).  No title profile sets them yet.
 * real_ids: the online ID, NP ID and account ID getters ask the console's
 * library (user-bound reads, no title identity).  no_initial_event: no
 * SIGNED_OUT event at registration, for a title that takes one as "you were
 * signed out". */
static bool g_np_real_ids;
static bool g_np_no_initial_event;

/* one log line the first time each function is called (some are called 30
 * times a second), and one per error other than the persona's SIGNED_OUT */
#define FIRST(...)                                                                                               \
    do {                                                                                                         \
        static bool once_;                                                                                       \
        if (!once_) {                                                                                            \
            once_ = true;                                                                                        \
            logf(__VA_ARGS__);                                                                                   \
        }                                                                                                        \
    } while (0)

static u32 g_errors_logged;

static s32 ret(const char *fn, s32 rc)
{
    if (rc < 0 && rc != NP_SIGNED_OUT && g_errors_logged++ < 128)
        logf("np: %s -> %x", fn, (u32)rc);
    return rc;
}

static void copy_str(char *dst, u32 size, const char *src)
{
    u32 i = 0;
    for (; src && i + 1 < size && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

/* ---- users, events and callbacks ---- */

struct cb {
    void *fn, *ud;
};
typedef void (*state_cb_fn)(s32 user, s32 state, void *ud);
typedef void (*state_cb_legacy_fn)(s32 user, s32 state, void *np_id, void *ud);
typedef void (*reach_cb_fn)(s32 user, s32 reach, void *ud);

#define NP_USERS 4
#define NP_QLEN 16
#define NP_CB_SLOTS 8

struct evq {
    struct {
        s32 user, state;
    } ev[NP_QLEN];
    u32 head, n;
    bool seeded;
};

static volatile int g_np_lock;
static struct cb g_cb_a[NP_CB_SLOTS], g_cb_legacy, g_cb_reach, g_cb_toolkit, g_cb_plus;
static struct cb g_cb_presence_a[NP_CB_SLOTS], g_cb_presence;
static struct evq g_q_game, g_q_toolkit;
static s32 g_users[NP_USERS], g_last_reach[NP_USERS];
static u32 g_nusers;
static bool g_users_loaded;

static void q_push(struct evq *q, s32 user, s32 state)
{
    if (q->n == NP_QLEN) /* full: drop the oldest */
        q->head = (q->head + 1) % NP_QLEN, q->n--;
    u32 i = (q->head + q->n++) % NP_QLEN;
    q->ev[i].user = user, q->ev[i].state = state;
}

static bool q_pop(struct evq *q, s32 *user, s32 *state)
{
    if (!q->n)
        return false;
    *user = q->ev[q->head].user, *state = q->ev[q->head].state;
    q->head = (q->head + 1) % NP_QLEN, q->n--;
    return true;
}

/* The logged-in users, from the console (sceUserServiceGetLoginUserIdList:
 * four slots, -1 empty), else the initial user. */
static void load_users(void)
{
    if (g_users_loaded)
        return;
    s32 list[NP_USERS] = {USER_INVALID, USER_INVALID, USER_INVALID, USER_INVALID};
    s32 (*get_list)(s32 *) = (s32(*)(s32 *))host_real_in("libSceUserService", "sceUserServiceGetLoginUserIdList");
    s32 rc = get_list ? get_list(list) : -1;
    if (rc != 0) {
        s32 (*initial)(s32 *) = (s32(*)(s32 *))host_real_in("libSceUserService", "sceUserServiceGetInitialUser");
        list[0] = USER_INVALID;
        if (initial)
            initial(&list[0]);
    }
    spin_lock(&g_np_lock);
    if (!g_users_loaded) {
        g_users_loaded = true;
        for (u32 i = 0; i < NP_USERS; i++)
            if (list[i] != USER_INVALID) {
                g_last_reach[g_nusers] = -1; /* never delivered */
                g_users[g_nusers++] = list[i];
            }
    }
    spin_unlock(&g_np_lock);
    logf("np: %u logged-in user(s) (first %x), all signed out (login list rc %x)", g_nusers,
         g_nusers ? (u32)g_users[0] : 0, (u32)rc);
}

/* the initial SIGNED_OUT per user, once per consumer (spec §4.2, source 2) */
static void seed(struct evq *q)
{
    if (g_np_no_initial_event)
        return;
    load_users();
    spin_lock(&g_np_lock);
    if (!q->seeded) {
        q->seeded = true;
        for (u32 i = 0; i < g_nusers; i++)
            q_push(q, g_users[i], NP_STATE_SIGNED_OUT);
    }
    spin_unlock(&g_np_lock);
}

static s32 cb_slot_register(struct cb *slots, void *fn, void *ud)
{
    if (!fn)
        return NP_INVALID_ARGUMENT;
    s32 rc = NP_CALLBACK_MAX;
    spin_lock(&g_np_lock);
    for (u32 i = 0; i < NP_CB_SLOTS; i++)
        if (slots[i].fn == fn) {
            rc = NP_CALLBACK_ALREADY_REGISTERED;
            break;
        }
    for (u32 i = 0; rc == NP_CALLBACK_MAX && i < NP_CB_SLOTS; i++)
        if (!slots[i].fn) {
            slots[i] = (struct cb){fn, ud};
            rc = (s32)i + 1;
        }
    spin_unlock(&g_np_lock);
    return rc;
}

static s32 cb_slot_unregister(struct cb *slots, s32 id, s32 bad_id)
{
    if (id < 1 || id > NP_CB_SLOTS)
        return bad_id;
    s32 rc = 0;
    spin_lock(&g_np_lock);
    if (!slots[id - 1].fn)
        rc = NP_CALLBACK_NOT_REGISTERED;
    slots[id - 1] = (struct cb){0, 0};
    spin_unlock(&g_np_lock);
    return rc;
}

static s32 cb_one_register(struct cb *c, void *fn, void *ud, bool replace)
{
    if (!fn)
        return NP_INVALID_ARGUMENT;
    s32 rc = 0;
    spin_lock(&g_np_lock);
    if (c->fn && !replace)
        rc = NP_CALLBACK_ALREADY_REGISTERED;
    else
        *c = (struct cb){fn, ud};
    spin_unlock(&g_np_lock);
    return rc;
}

static s32 cb_one_unregister(struct cb *c)
{
    spin_lock(&g_np_lock);
    s32 rc = c->fn ? 0 : NP_CALLBACK_NOT_REGISTERED;
    *c = (struct cb){0, 0};
    spin_unlock(&g_np_lock);
    return rc;
}

static s32 hle_sceNpRegisterStateCallbackA(void *fn, void *ud)
{
    s32 rc = cb_slot_register(g_cb_a, fn, ud);
    logf("sceNpRegisterStateCallbackA(%p) -> %x", fn, (u32)rc);
    if (rc > 0)
        seed(&g_q_game);
    return ret("sceNpRegisterStateCallbackA", rc);
}

static s32 hle_sceNpUnregisterStateCallbackA(s32 id)
{
    return ret("sceNpUnregisterStateCallbackA", cb_slot_unregister(g_cb_a, id, NP_INVALID_ARGUMENT));
}

static s32 hle_sceNpRegisterStateCallback(void *fn, void *ud)
{
    s32 rc = cb_one_register(&g_cb_legacy, fn, ud, false);
    logf("sceNpRegisterStateCallback(%p) -> %x", fn, (u32)rc);
    if (rc == 0)
        seed(&g_q_game);
    return ret("sceNpRegisterStateCallback", rc);
}

static s32 hle_sceNpUnregisterStateCallback(void)
{
    return ret("sceNpUnregisterStateCallback", cb_one_unregister(&g_cb_legacy));
}

static s32 hle_sceNpRegisterNpReachabilityStateCallback(void *fn, void *ud)
{
    load_users();
    s32 rc = cb_one_register(&g_cb_reach, fn, ud, false);
    if (rc == 0) {
        spin_lock(&g_np_lock);
        for (u32 i = 0; i < NP_USERS; i++)
            g_last_reach[i] = -1;
        spin_unlock(&g_np_lock);
    }
    logf("sceNpRegisterNpReachabilityStateCallback(%p) -> %x", fn, (u32)rc);
    return ret("sceNpRegisterNpReachabilityStateCallback", rc);
}

static s32 hle_sceNpUnregisterNpReachabilityStateCallback(void)
{
    return ret("sceNpUnregisterNpReachabilityStateCallback", cb_one_unregister(&g_cb_reach));
}

/* the game-side callbacks: per queued event the legacy callback, then the A
 * callbacks in slot order; then reachability for users not told yet */
static s32 hle_sceNpCheckCallback(void)
{
    for (u32 guard = 0; guard < NP_QLEN; guard++) {
        s32 user, state;
        struct cb a[NP_CB_SLOTS], legacy = {0, 0};
        spin_lock(&g_np_lock);
        bool have = q_pop(&g_q_game, &user, &state);
        if (have) {
            memcpy(a, g_cb_a, sizeof(a));
            legacy = g_cb_legacy;
        }
        spin_unlock(&g_np_lock);
        if (!have)
            break;
        logf("np: state event user %x state %d to the title's callbacks", (u32)user, state);
        if (legacy.fn)
            ((state_cb_legacy_fn)legacy.fn)(user, state, NULL, legacy.ud);
        for (u32 i = 0; i < NP_CB_SLOTS; i++)
            if (a[i].fn)
                ((state_cb_fn)a[i].fn)(user, state, a[i].ud);
    }
    s32 users[NP_USERS];
    u32 n = 0;
    spin_lock(&g_np_lock);
    struct cb r = g_cb_reach;
    for (u32 i = 0; r.fn && i < g_nusers; i++)
        if (g_last_reach[i] != NP_REACH_UNAVAILABLE) {
            g_last_reach[i] = NP_REACH_UNAVAILABLE;
            users[n++] = g_users[i];
        }
    spin_unlock(&g_np_lock);
    for (u32 i = 0; i < n; i++) {
        logf("np: reachability event user %x UNAVAILABLE", (u32)users[i]);
        ((reach_cb_fn)r.fn)(users[i], NP_REACH_UNAVAILABLE, r.ud);
    }
    return 0;
}

/* NpToolkit2's state callback (libSceNpManagerForToolkit) */
static s32 hle_sceNpRegisterStateCallbackForToolkit(void *fn, void *ud)
{
    s32 rc = cb_one_register(&g_cb_toolkit, fn, ud, true);
    if (rc == 0) {
        seed(&g_q_toolkit);
        rc = 1; /* the toolkit tests only the sign */
    }
    logf("sceNpRegisterStateCallbackForToolkit(%p) -> %x", fn, (u32)rc);
    return ret("sceNpRegisterStateCallbackForToolkit", rc);
}

static s32 hle_sceNpUnregisterStateCallbackForToolkit(void)
{
    return ret("sceNpUnregisterStateCallbackForToolkit", cb_one_unregister(&g_cb_toolkit));
}

static s32 hle_sceNpCheckCallbackForLib(void)
{
    for (u32 guard = 0; guard < NP_QLEN; guard++) {
        s32 user, state;
        spin_lock(&g_np_lock);
        bool have = q_pop(&g_q_toolkit, &user, &state);
        struct cb c = g_cb_toolkit;
        spin_unlock(&g_np_lock);
        if (!have)
            break;
        logf("np: toolkit state event user %x state %d", (u32)user, state);
        if (c.fn)
            ((state_cb_fn)c.fn)(user, state, c.ud);
    }
    return 0;
}

static s32 hle_sceNpRegisterPlusEventCallback(void *fn, void *ud)
{
    return ret("sceNpRegisterPlusEventCallback", cb_one_register(&g_cb_plus, fn, ud, false));
}

static s32 hle_sceNpUnregisterPlusEventCallback(void)
{
    return ret("sceNpUnregisterPlusEventCallback", cb_one_unregister(&g_cb_plus));
}

static void hle_sceNpRegisterGamePresenceCallback(void *fn, void *ud)
{
    if (fn)
        cb_one_register(&g_cb_presence, fn, ud, true);
}

static s32 hle_sceNpRegisterGamePresenceCallbackA(void *fn, void *ud)
{
    return ret("sceNpRegisterGamePresenceCallbackA", cb_slot_register(g_cb_presence_a, fn, ud));
}

static s32 hle_sceNpUnregisterGamePresenceCallbackA(s32 id)
{
    return ret("sceNpUnregisterGamePresenceCallbackA",
               cb_slot_unregister(g_cb_presence_a, id, NP_CALLBACK_NOT_REGISTERED));
}

/* ---- state and identity getters ---- */

/* what the console itself thinks, logged once (read only) */
static void log_console_state(void)
{
    s32 (*initial)(s32 *) = (s32(*)(s32 *))host_real_in("libSceUserService", "sceUserServiceGetInitialUser");
    s32 (*real)(s32, s32 *) = (s32(*)(s32, s32 *))host_real_in("libSceNpManager", "sceNpGetState");
    s32 user = USER_INVALID, state = -1, rc = -1;
    if (initial && real && initial(&user) == 0)
        rc = real(user, &state);
    logf("np: the console's own NP state for user %x is %d (rc %x); the title sees SIGNED_OUT", (u32)user, state,
         (u32)rc);
}

static s32 hle_sceNpGetState(s32 user, s32 *state)
{
    static bool logged;
    if (!logged) {
        logged = true;
        logf("sceNpGetState(user %x) -> SIGNED_OUT", (u32)user);
        log_console_state();
    }
    if (!state || user == USER_INVALID)
        return ret("sceNpGetState", NP_INVALID_ARGUMENT);
    *state = NP_STATE_SIGNED_OUT;
    return 0;
}

/* out zeroed; rc returned (SIGNED_OUT for identity, 0 for flags) */
static s32 user_out(const char *fn, s32 user, void *out, u32 n, s32 rc)
{
    if (!out || user == USER_INVALID)
        return ret(fn, NP_INVALID_ARGUMENT);
    memset(out, 0, n);
    return ret(fn, rc);
}

/* the online-ID forms: no online ID maps to a user while signed out */
static s32 oid_out(const char *fn, const void *oid, void *out, u32 n, s32 rc)
{
    if (!oid || !out)
        return ret(fn, NP_INVALID_ARGUMENT);
    memset(out, 0, n);
    return ret(fn, rc);
}

static s32 real_id(const char *fn, s32 user, void *out)
{
    s32 (*real)(s32, void *) = (s32(*)(s32, void *))host_real_in("libSceNpManager", fn);
    s32 rc = real ? real(user, out) : NP_SIGNED_OUT;
    logf("np: %s(user %x) passed to the console (real_ids) -> %x", fn, (u32)user, (u32)rc);
    return rc;
}

static s32 hle_sceNpGetOnlineId(s32 user, void *oid)
{
    FIRST("sceNpGetOnlineId(user %x) -> SIGNED_OUT", (u32)user);
    if (g_np_real_ids && oid && user != USER_INVALID)
        return real_id("sceNpGetOnlineId", user, oid);
    return user_out("sceNpGetOnlineId", user, oid, 0x14, NP_SIGNED_OUT);
}

static s32 hle_sceNpGetNpId(s32 user, void *id)
{
    FIRST("sceNpGetNpId(user %x) -> SIGNED_OUT", (u32)user);
    if (g_np_real_ids && id && user != USER_INVALID)
        return real_id("sceNpGetNpId", user, id);
    return user_out("sceNpGetNpId", user, id, 0x24, NP_SIGNED_OUT);
}

static s32 hle_sceNpGetAccountIdA(s32 user, u64 *acc)
{
    FIRST("sceNpGetAccountIdA(user %x) -> SIGNED_OUT", (u32)user);
    if (g_np_real_ids && acc && user != USER_INVALID)
        return real_id("sceNpGetAccountIdA", user, acc);
    return user_out("sceNpGetAccountIdA", user, acc, 8, NP_SIGNED_OUT);
}

static s32 hle_sceNpGetAccountCountryA(s32 user, void *cc)
{
    FIRST("sceNpGetAccountCountryA(user %x) -> SIGNED_OUT", (u32)user);
    return user_out("sceNpGetAccountCountryA", user, cc, 4, NP_SIGNED_OUT);
}

static s32 hle_sceNpGetAccountDateOfBirthA(s32 user, void *date)
{
    FIRST("sceNpGetAccountDateOfBirthA(user %x) -> SIGNED_OUT", (u32)user);
    return user_out("sceNpGetAccountDateOfBirthA", user, date, 4, NP_SIGNED_OUT);
}

static s32 hle_sceNpGetNpReachabilityState(s32 user, s32 *reach)
{
    FIRST("sceNpGetNpReachabilityState(user %x) -> UNAVAILABLE", (u32)user);
    return user_out("sceNpGetNpReachabilityState", user, reach, 4, 0);
}

static s32 hle_sceNpIsPlusMember(s32 user, bool *plus)
{
    FIRST("sceNpIsPlusMember(user %x) -> no", (u32)user);
    return user_out("sceNpIsPlusMember", user, plus, 1, 0);
}

static s32 hle_sceNpGetGamePresenceStatusA(s32 user, s32 *status)
{
    return user_out("sceNpGetGamePresenceStatusA", user, status, 4, 0); /* OFFLINE */
}

static s32 hle_sceNpHasSignedUp(s32 user, bool *signed_up)
{
    FIRST("sceNpHasSignedUp(user %x) -> yes", (u32)user);
    s32 rc = user_out("sceNpHasSignedUp", user, signed_up, 1, 0);
    if (rc == 0)
        *signed_up = true;
    return rc;
}

static s32 hle_sceNpGetAccountId(const void *oid, u64 *acc)
{
    return oid_out("sceNpGetAccountId", oid, acc, 8, NP_USER_NOT_FOUND);
}

static s32 hle_sceNpGetAccountCountry(const void *oid, void *cc)
{
    return oid_out("sceNpGetAccountCountry", oid, cc, 4, NP_USER_NOT_FOUND);
}

static s32 hle_sceNpGetAccountDateOfBirth(const void *oid, void *date)
{
    return oid_out("sceNpGetAccountDateOfBirth", oid, date, 4, NP_USER_NOT_FOUND);
}

static s32 hle_sceNpGetGamePresenceStatus(const void *oid, s32 *status)
{
    return oid_out("sceNpGetGamePresenceStatus", oid, status, 4, 0);
}

static s32 hle_sceNpGetUserIdByOnlineId(const void *oid, s32 *user)
{
    return ret("sceNpGetUserIdByOnlineId", oid && user ? NP_SIGNED_OUT : NP_INVALID_ARGUMENT);
}

static s32 hle_sceNpGetUserIdByAccountId(u64 acc, s32 *user)
{
    FIRST("sceNpGetUserIdByAccountId(%lx) -> SIGNED_OUT", acc);
    return ret("sceNpGetUserIdByAccountId", acc && user ? NP_SIGNED_OUT : NP_INVALID_ARGUMENT);
}

/* ---- settings the title hands over: kept here, never given to SWRR's library ---- */

struct np_age_restriction {
    char country[4];
    s8 age;
    u8 pad[3];
};
struct np_content_restriction {
    u64 size;
    s8 default_age;
    u8 pad[3];
    s32 count;
    const struct np_age_restriction *table;
};
static struct np_content_restriction g_restriction;
static char g_np_title_id[16];

static s32 hle_sceNpSetContentRestriction(const struct np_content_restriction *r)
{
    s32 rc = 0;
    if (!r)
        rc = NP_INVALID_ARGUMENT;
    else if (r->size != sizeof(*r))
        rc = NP_INVALID_SIZE;
    else if (r->default_age < 0 || r->count < 0 || r->count > 256 || (r->count > 0 && !r->table))
        rc = NP_INVALID_ARGUMENT;
    if (rc)
        return ret("sceNpSetContentRestriction", rc);
    g_restriction = *r;
    g_restriction.table = NULL;
    if (r->count > 0)
        logf("sceNpSetContentRestriction: default age %d, %d countries (first %.2s: %d); kept by ghostldr",
             r->default_age, r->count, r->table[0].country, r->table[0].age);
    else
        logf("sceNpSetContentRestriction: default age %d; kept by ghostldr", r->default_age);
    return 0;
}

static s32 hle_sceNpSetNpTitleId(const char *id, const void *secret)
{
    if (!id || !secret)
        return ret("sceNpSetNpTitleId", NP_INVALID_ARGUMENT);
    copy_str(g_np_title_id, 13, id);
    logf("sceNpSetNpTitleId(%s): kept by ghostldr (SWRR's NP title stays bound)", g_np_title_id);
    return 0;
}

/* for the trophy family: the NP title ID ("CUSA11456_00"), "" until set */
const char *np_title_id(void) { return g_np_title_id; }

static s32 hle_sceNpNotifyPlusFeature(const void *param)
{
    FIRST("sceNpNotifyPlusFeature: dropped (nothing is reported under SWRR's title)");
    return ret("sceNpNotifyPlusFeature", param ? 0 : NP_INVALID_ARGUMENT);
}

/* ---- NpManager requests ---- */

static struct npreq_table g_np_req = NPREQ_TABLE("np", 0x20000001, 32, NP_REQUEST_MAX, NP_REQUEST_NOT_FOUND,
                                                 NP_INVALID_ID, NP_ABORTED, NP_INVALID_ARGUMENT, NP_INVALID_SIZE);

static s32 hle_sceNpCreateRequest(void)
{
    s32 id = npreq_create(&g_np_req, false, 0);
    FIRST("sceNpCreateRequest -> %x", (u32)id);
    return ret("sceNpCreateRequest", id);
}

static s32 hle_sceNpCreateAsyncRequest(const void *param)
{
    return ret("sceNpCreateAsyncRequest", npreq_create_async(&g_np_req, param, 0x18, 0));
}

static s32 hle_sceNpDeleteRequest(s32 req) { return ret("sceNpDeleteRequest", npreq_delete(&g_np_req, req)); }
static s32 hle_sceNpAbortRequest(s32 req) { return ret("sceNpAbortRequest", npreq_abort(&g_np_req, req)); }
static s32 hle_sceNpPollAsync(s32 req, s32 *res) { return ret("sceNpPollAsync", npreq_poll(&g_np_req, req, res)); }
static s32 hle_sceNpWaitAsync(s32 req, s32 *res) { return ret("sceNpWaitAsync", npreq_poll(&g_np_req, req, res)); }
static s32 hle_sceNpSetTimeout(s32 req) { return ret("sceNpSetTimeout", npreq_set_timeout(&g_np_req, req)); }

/* An operation on request req: argerr (from the arguments) or the persona's
 * result.  out (n bytes) is zeroed when the arguments are good. */
static s32 np_op(const char *fn, s32 req, s32 argerr, void *out, u32 n, s32 result)
{
    s32 rc = npreq_check(&g_np_req, req);
    if (rc == 0)
        rc = argerr;
    if (rc == 0) {
        if (out)
            memset(out, 0, n);
        rc = npreq_finish(&g_np_req, req, result);
    }
    return ret(fn, rc);
}

static s32 hle_sceNpCheckNpAvailabilityA(s32 req, s32 user)
{
    FIRST("sceNpCheckNpAvailabilityA(%x, user %x) -> SIGNED_OUT", (u32)req, (u32)user);
    return np_op("sceNpCheckNpAvailabilityA", req, user == USER_INVALID ? NP_INVALID_ARGUMENT : 0, NULL, 0,
                 NP_SIGNED_OUT);
}

static s32 hle_sceNpCheckNpAvailability(s32 req, const void *oid)
{
    return ret("sceNpCheckNpAvailability", oid ? NP_USER_NOT_FOUND : NP_INVALID_ARGUMENT);
}

static s32 hle_sceNpCheckNpReachability(s32 req, s32 user)
{
    return np_op("sceNpCheckNpReachability", req, user == USER_INVALID ? NP_INVALID_ARGUMENT : 0, NULL, 0,
                 NP_SIGNED_OUT);
}

struct np_check_plus_param {
    u64 size;
    s32 user;
    u32 pad;
    u64 features;
    u8 reserved[32];
};

static s32 hle_sceNpCheckPlus(s32 req, const struct np_check_plus_param *p, void *result)
{
    FIRST("sceNpCheckPlus(%x) -> not authorized, SIGNED_OUT", (u32)req);
    if (result)
        memset(result, 0, 0x21); /* Ghost reads authorized before the return value */
    s32 argerr = !p || !result ? NP_INVALID_ARGUMENT
                 : p->size != sizeof(*p) ? NP_INVALID_SIZE
                 : p->user == USER_INVALID ? NP_INVALID_ARGUMENT
                 : 0;
    return np_op("sceNpCheckPlus", req, argerr, NULL, 0, NP_SIGNED_OUT);
}

static s32 hle_sceNpGetParentalControlInfoA(s32 req, s32 user, s8 *age, void *info)
{
    FIRST("sceNpGetParentalControlInfoA(%x, user %x) -> SIGNED_OUT", (u32)req, (u32)user);
    s32 argerr = !age || !info || user == USER_INVALID ? NP_INVALID_ARGUMENT : 0;
    if (!argerr)
        *age = 0;
    return np_op("sceNpGetParentalControlInfoA", req, argerr, info, 3, NP_SIGNED_OUT);
}

static s32 hle_sceNpGetParentalControlInfo(s32 req, const void *oid, s8 *age, void *info)
{
    if (!oid || !age || !info)
        return ret("sceNpGetParentalControlInfo", NP_INVALID_ARGUMENT);
    *age = 0;
    memset(info, 0, 3);
    return ret("sceNpGetParentalControlInfo", NP_USER_NOT_FOUND);
}

static s32 hle_sceNpGetAccountLanguageA(s32 req, s32 user, void *lang)
{
    return np_op("sceNpGetAccountLanguageA", req, !lang || user == USER_INVALID ? NP_INVALID_ARGUMENT : 0, lang,
                 0x10, NP_SIGNED_OUT);
}

static s32 hle_sceNpGetAccountLanguage(s32 req, const void *oid, void *lang)
{
    return oid_out("sceNpGetAccountLanguage", oid, lang, 0x10, NP_USER_NOT_FOUND);
}

/* the output's layout is unknown (spec NP-3): nothing is written */
static s32 hle_sceNpGetAccountLanguage2(s32 req, s32 user, void *lang)
{
    FIRST("sceNpGetAccountLanguage2(%x, user %x) -> SIGNED_OUT", (u32)req, (u32)user);
    return np_op("sceNpGetAccountLanguage2", req, !lang || user == USER_INVALID ? NP_INVALID_ARGUMENT : 0, NULL, 0,
                 NP_SIGNED_OUT);
}

static s32 hle_sceNpSetGamePresenceOnlineA(s32 req, s32 user, u32 presence)
{
    return np_op("sceNpSetGamePresenceOnlineA", req, user == USER_INVALID ? NP_INVALID_ARGUMENT : 0, NULL, 0,
                 NP_SIGNED_OUT);
}

static s32 hle_sceNpSetGamePresenceOnline(s32 req, const void *oid, u32 presence)
{
    return np_op("sceNpSetGamePresenceOnline", req, oid ? 0 : NP_INVALID_ARGUMENT, NULL, 0, NP_SIGNED_OUT);
}

/* ---- InGameMessage: pool and handles are local setup (the console's);
 * sending needs a signed-in peer ---- */

typedef s64 (*fn6)(u64, u64, u64, u64, u64, u64);

static s64 pass(const char *lib, const char *fn, fn6 *real, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)
{
    if (!*real)
        *real = (fn6)host_real_in(lib, fn);
    if (!*real) {
        logf("np: %s:%s missing on the console", lib, fn);
        return NP_SIGNED_OUT;
    }
    return (*real)(a, b, c, d, e, f);
}

#define PASS(lib, fn)                                                                                            \
    static s64 hle_##fn(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)                                               \
    {                                                                                                            \
        static fn6 real;                                                                                         \
        FIRST(#fn " passed to the console's " lib);                                                             \
        return pass(lib, #fn, &real, a, b, c, d, e, f);                                                         \
    }

PASS("libSceNpManager", sceNpInGameMessageInitialize)
PASS("libSceNpManager", sceNpInGameMessageTerminate)
PASS("libSceNpManager", sceNpInGameMessageCreateHandle)
PASS("libSceNpManager", sceNpInGameMessageDeleteHandle)
PASS("libSceNpManager", sceNpInGameMessageAbortHandle)
PASS("libSceNpManager", sceNpInGameMessageCheckCallback)
PASS("libSceNpManager", sceNpInGameMessageGetMemoryPoolStatistics)

static s32 hle_np_signed_out(void)
{
    FIRST("np: InGameMessage send or prepare -> SIGNED_OUT");
    return NP_SIGNED_OUT;
}

/* ---- libSceNetCtlForNpToolkit: the network state is real and stable, so
 * no event is ever delivered; NpToolkit2 reads sceNetCtlGetState itself ---- */

typedef void (*netctl_cb_fn)(s32 event, void *arg);
static struct cb g_netctl_tk[8];

static s32 hle_sceNetCtlRegisterCallbackForNpToolkit(void *fn, void *arg, s32 *cid)
{
    s32 rc = NETCTL_CALLBACK_MAX;
    if (!fn || !cid)
        rc = NETCTL_INVALID_ADDR;
    spin_lock(&g_np_lock);
    for (u32 i = 0; rc == NETCTL_CALLBACK_MAX && i < ARRAY_SIZE(g_netctl_tk); i++)
        if (!g_netctl_tk[i].fn) {
            g_netctl_tk[i] = (struct cb){fn, arg};
            *cid = (s32)i;
            rc = 0;
        }
    spin_unlock(&g_np_lock);
    logf("sceNetCtlRegisterCallbackForNpToolkit(%p) -> %x, id %d", fn, (u32)rc, rc == 0 ? *cid : -1);
    return ret("sceNetCtlRegisterCallbackForNpToolkit", rc);
}

static s32 hle_sceNetCtlUnregisterCallbackForNpToolkit(s32 cid)
{
    logf("sceNetCtlUnregisterCallbackForNpToolkit(%d)", cid);
    if (cid < 0 || cid >= (s32)ARRAY_SIZE(g_netctl_tk))
        return ret("sceNetCtlUnregisterCallbackForNpToolkit", NETCTL_INVALID_ID);
    spin_lock(&g_np_lock);
    g_netctl_tk[cid] = (struct cb){0, 0};
    spin_unlock(&g_np_lock);
    return 0;
}

static s32 hle_sceNetCtlCheckCallbackForNpToolkit(void) { return 0; }
static s32 hle_sceNetCtlClearEventForNpToolkit(void) { return 0; }

/* ---- libSceNpAuth: no request reaches the console (it would authenticate
 * under SWRR's NP title) ---- */

static struct npreq_table g_auth_req =
    NPREQ_TABLE("npauth", 0x10000001, 16, AUTH_REQUEST_MAX, AUTH_REQUEST_NOT_FOUND, AUTH_INVALID_ID, AUTH_ABORTED,
                AUTH_INVALID_ARGUMENT, AUTH_INVALID_SIZE);

struct auth_param {
    u64 size;
    union {
        s32 user;          /* the A and V3 forms */
        const void *oid;   /* the online-ID forms */
    };
    const char *client_id; /* SceNpClientId: char id[129] */
    const char *scope_or_secret;
    const char *scope;     /* ID token only */
};

static s32 hle_sceNpAuthCreateRequest(void)
{
    s32 id = npreq_create(&g_auth_req, false, 0);
    FIRST("sceNpAuthCreateRequest -> %x", (u32)id);
    return ret("sceNpAuthCreateRequest", id);
}

static s32 hle_sceNpAuthCreateAsyncRequest(const void *param)
{
    return ret("sceNpAuthCreateAsyncRequest", npreq_create_async(&g_auth_req, param, 0x18, 0));
}

static s32 hle_sceNpAuthDeleteRequest(s32 req) { return ret("sceNpAuthDeleteRequest", npreq_delete(&g_auth_req, req)); }
static s32 hle_sceNpAuthAbortRequest(s32 req) { return ret("sceNpAuthAbortRequest", npreq_abort(&g_auth_req, req)); }
static s32 hle_sceNpAuthPollAsync(s32 req, s32 *res)
{
    return ret("sceNpAuthPollAsync", npreq_poll(&g_auth_req, req, res));
}
static s32 hle_sceNpAuthWaitAsync(s32 req, s32 *res)
{
    return ret("sceNpAuthWaitAsync", npreq_poll(&g_auth_req, req, res));
}
static s32 hle_sceNpAuthSetTimeout(s32 req) { return ret("sceNpAuthSetTimeout", npreq_set_timeout(&g_auth_req, req)); }

/* An authorization code (token: false) or ID token request.  by_user: the
 * A/V3 forms (user ID), else the online-ID form, which fails at once. */
static s32 auth_op(const char *fn, s32 req, const struct auth_param *p, void *out, s32 *issuer, bool token,
                   bool by_user)
{
    u64 size = token ? 0x28 : 0x20;
    u32 out_size = token ? 0x1008 : 0x88;
    s32 rc = by_user ? npreq_check(&g_auth_req, req) : 0;
    if (rc == 0 && (!p || !out))
        rc = AUTH_INVALID_ARGUMENT;
    else if (rc == 0 && p->size != size)
        rc = AUTH_INVALID_SIZE;
    const char *scope = p ? (token ? p->scope : p->scope_or_secret) : NULL;
    if (rc == 0 && ((by_user ? p->user == USER_INVALID : !p->oid) || !p->client_id || !scope ||
                    (token && !p->scope_or_secret)))
        rc = AUTH_INVALID_ARGUMENT;
    if (rc)
        return ret(fn, rc);
    memset(out, 0, out_size);
    if (issuer)
        *issuer = 0;
    logf("%s(%x): client %.64s scope %.96s: signed out", fn, (u32)req, p->client_id, scope);
    if (!by_user)
        return ret(fn, NP_USER_NOT_FOUND);
    return ret(fn, npreq_finish(&g_auth_req, req, NP_SIGNED_OUT));
}

static s32 hle_sceNpAuthGetAuthorizationCodeV3(s32 req, const struct auth_param *p, void *code, s32 *issuer)
{
    return auth_op("sceNpAuthGetAuthorizationCodeV3", req, p, code, issuer, false, true);
}

static s32 hle_sceNpAuthGetAuthorizationCodeA(s32 req, const struct auth_param *p, void *code, s32 *issuer)
{
    return auth_op("sceNpAuthGetAuthorizationCodeA", req, p, code, issuer, false, true);
}

static s32 hle_sceNpAuthGetAuthorizationCode(s32 req, const struct auth_param *p, void *code, s32 *issuer)
{
    return auth_op("sceNpAuthGetAuthorizationCode", req, p, code, issuer, false, false);
}

static s32 hle_sceNpAuthGetIdTokenV3(s32 req, const struct auth_param *p, void *token)
{
    return auth_op("sceNpAuthGetIdTokenV3", req, p, token, NULL, true, true);
}

static s32 hle_sceNpAuthGetIdTokenA(s32 req, const struct auth_param *p, void *token)
{
    return auth_op("sceNpAuthGetIdTokenA", req, p, token, NULL, true, true);
}

static s32 hle_sceNpAuthGetIdToken(s32 req, const struct auth_param *p, void *token)
{
    return auth_op("sceNpAuthGetIdToken", req, p, token, NULL, true, false);
}

/* ---- libSceNpCommon: the console's utilities, except what would run title
 * code on a thread ghostldr did not create ---- */

static s32 hle_sceNpCreateThread(u64 *thr, void *(*entry)(void *), void *arg, s32 prio, u64 stack, u64 affinity,
                                 const char *name)
{
    if (!thr || !entry)
        return ret("sceNpCreateThread", NP_INVALID_ARGUMENT);
    char nm[32];
    copy_str(nm, sizeof(nm), name ? name : "SceNpThread");
    u8 attr[0x40];
    K.pthread_attr_init(attr);
    if (stack)
        K.pthread_attr_setstacksize(attr, stack);
    s32 rc = ghost_thread_create(thr, attr, entry, arg, nm);
    K.pthread_attr_destroy(attr);
    logf("sceNpCreateThread(%s, prio %d, stack %lx, affinity %lx) -> %x (a ghostldr thread)", nm, prio, stack,
         affinity, (u32)rc);
    return rc;
}

/* their handlers run on a thread of the console's library (spec R10):
 * passed through and logged until a title is seen using them */
PASS("libSceNpCommon", sceNpCalloutInitCtx)
PASS("libSceNpCommon", sceNpCalloutStartOnCtx)
PASS("libSceNpCommon", sceNpCalloutStartOnCtx64)
PASS("libSceNpCommon", sceNpCalloutStopOnCtx)
PASS("libSceNpCommon", sceNpCalloutTermCtx)

/* ---- libSceNpUtility: word filter, lookup, bandwidth test ---- */

#define COMMUNITY_ERRORS                                                                                         \
    COMM_TOO_MANY_OBJECTS, COMM_INVALID_ID, COMM_INVALID_ID, COMM_ABORTED, COMM_INSUFFICIENT_ARGUMENT,           \
        COMM_INVALID_ARGUMENT
static struct npreq_table g_wf_ctx = NPREQ_TABLE("wordfilter ctx", 0x01000001, 32, COMMUNITY_ERRORS);
static struct npreq_table g_wf_req = NPREQ_TABLE("wordfilter", 0x02000001, 32, COMMUNITY_ERRORS);
static struct npreq_table g_lookup_ctx = NPREQ_TABLE("lookup ctx", 1, 32, COMMUNITY_ERRORS);
static struct npreq_table g_lookup_req = NPREQ_TABLE("lookup", 1, 32, COMMUNITY_ERRORS);

static s32 hle_sceNpUtilityInit(void) { return 0; }
static s32 hle_sceNpUtilityTerm(void) { return 0; }

static s32 title_ctx_a(const char *fn, struct npreq_table *t, s32 user)
{
    s32 rc = user == USER_INVALID ? COMM_INVALID_ARGUMENT : npreq_create(t, false, 0);
    logf("%s(user %x) -> %x", fn, (u32)user, (u32)rc);
    return ret(fn, rc);
}

static s32 title_ctx(const char *fn, struct npreq_table *t, const void *np_id)
{
    return ret(fn, np_id ? npreq_create(t, false, 0) : COMM_INSUFFICIENT_ARGUMENT);
}

static s32 title_ctx_delete(const char *fn, struct npreq_table *ctxs, struct npreq_table *reqs, s32 ctx)
{
    s32 rc = npreq_delete(ctxs, ctx);
    if (rc == 0)
        npreq_delete_owned(reqs, ctx);
    return ret(fn, rc);
}

static s32 comm_request(const char *fn, struct npreq_table *ctxs, struct npreq_table *reqs, s32 ctx,
                        const void *param, bool async)
{
    if (!npreq_exists(ctxs, ctx))
        return ret(fn, COMM_INVALID_ID);
    return ret(fn, async ? npreq_create_async(reqs, param, 0x18, ctx) : npreq_create(reqs, false, ctx));
}

static s32 hle_sceNpWordFilterCreateTitleCtxA(s32 user)
{
    return title_ctx_a("sceNpWordFilterCreateTitleCtxA", &g_wf_ctx, user);
}
static s32 hle_sceNpWordFilterCreateTitleCtx(const void *np_id)
{
    return title_ctx("sceNpWordFilterCreateTitleCtx", &g_wf_ctx, np_id);
}
static s32 hle_sceNpWordFilterDeleteTitleCtx(s32 ctx)
{
    return title_ctx_delete("sceNpWordFilterDeleteTitleCtx", &g_wf_ctx, &g_wf_req, ctx);
}
static s32 hle_sceNpWordFilterCreateRequest(s32 ctx)
{
    return comm_request("sceNpWordFilterCreateRequest", &g_wf_ctx, &g_wf_req, ctx, NULL, false);
}
static s32 hle_sceNpWordFilterCreateAsyncRequest(s32 ctx, const void *param)
{
    return comm_request("sceNpWordFilterCreateAsyncRequest", &g_wf_ctx, &g_wf_req, ctx, param, true);
}
static s32 hle_sceNpWordFilterDeleteRequest(s32 req)
{
    return ret("sceNpWordFilterDeleteRequest", npreq_delete(&g_wf_req, req));
}
static s32 hle_sceNpWordFilterAbortRequest(s32 req)
{
    return ret("sceNpWordFilterAbortRequest", npreq_abort(&g_wf_req, req));
}
static s32 hle_sceNpWordFilterPollAsync(s32 req, s32 *res)
{
    return ret("sceNpWordFilterPollAsync", npreq_poll(&g_wf_req, req, res));
}
static s32 hle_sceNpWordFilterWaitAsync(s32 req, s32 *res)
{
    return ret("sceNpWordFilterWaitAsync", npreq_poll(&g_wf_req, req, res));
}
static s32 hle_sceNpWordFilterSetTimeout(s32 req)
{
    return ret("sceNpWordFilterSetTimeout", npreq_set_timeout(&g_wf_req, req));
}

/* Comments pass unchanged: titles that sanitize local names (save names,
 * captions) keep working offline. */
static s32 word_filter(const char *fn, s32 req, const char *comment, char *out, bool sanitize, const void *opt)
{
    s32 rc = npreq_check(&g_wf_req, req);
    u32 len = 0;
    while (comment && len <= 0x400 && comment[len])
        len++;
    if (rc == 0 && (!comment || (sanitize && !out)))
        rc = COMM_INSUFFICIENT_ARGUMENT;
    else if (rc == 0 && (opt || len > 0x400))
        rc = COMM_INVALID_ARGUMENT;
    if (rc == 0) {
        if (sanitize)
            memcpy(out, comment, len + 1);
        rc = npreq_finish(&g_wf_req, req, 0);
    }
    FIRST("%s: comments pass unchanged", fn);
    return ret(fn, rc);
}

static s32 hle_sceNpWordFilterCensorComment(s32 req, const char *comment, void *opt)
{
    return word_filter("sceNpWordFilterCensorComment", req, comment, NULL, false, opt);
}

static s32 hle_sceNpWordFilterSanitizeComment(s32 req, const char *comment, char *out, void *opt)
{
    return word_filter("sceNpWordFilterSanitizeComment", req, comment, out, true, opt);
}

static s32 hle_sceNpLookupCreateTitleCtxA(s32 user)
{
    return title_ctx_a("sceNpLookupCreateTitleCtxA", &g_lookup_ctx, user);
}
static s32 hle_sceNpLookupCreateTitleCtx(const void *np_id)
{
    return title_ctx("sceNpLookupCreateTitleCtx", &g_lookup_ctx, np_id);
}
static s32 hle_sceNpLookupDeleteTitleCtx(s32 ctx)
{
    return title_ctx_delete("sceNpLookupDeleteTitleCtx", &g_lookup_ctx, &g_lookup_req, ctx);
}
static s32 hle_sceNpLookupCreateRequest(s32 ctx)
{
    return comm_request("sceNpLookupCreateRequest", &g_lookup_ctx, &g_lookup_req, ctx, NULL, false);
}
static s32 hle_sceNpLookupCreateAsyncRequest(s32 ctx, const void *param)
{
    return comm_request("sceNpLookupCreateAsyncRequest", &g_lookup_ctx, &g_lookup_req, ctx, param, true);
}
static s32 hle_sceNpLookupDeleteRequest(s32 req)
{
    return ret("sceNpLookupDeleteRequest", npreq_delete(&g_lookup_req, req));
}
static s32 hle_sceNpLookupAbortRequest(s32 req)
{
    return ret("sceNpLookupAbortRequest", npreq_abort(&g_lookup_req, req));
}
static s32 hle_sceNpLookupPollAsync(s32 req, s32 *res)
{
    return ret("sceNpLookupPollAsync", npreq_poll(&g_lookup_req, req, res));
}
static s32 hle_sceNpLookupWaitAsync(s32 req, s32 *res)
{
    return ret("sceNpLookupWaitAsync", npreq_poll(&g_lookup_req, req, res));
}
static s32 hle_sceNpLookupSetTimeout(s32 req)
{
    return ret("sceNpLookupSetTimeout", npreq_set_timeout(&g_lookup_req, req));
}

static s32 hle_sceNpLookupNpId(s32 req, const void *oid, void *np_id, void *opt)
{
    s32 rc = npreq_check(&g_lookup_req, req);
    if (rc == 0 && opt)
        rc = COMM_INVALID_ARGUMENT;
    else if (rc == 0 && (!oid || !np_id))
        rc = COMM_INSUFFICIENT_ARGUMENT;
    if (rc == 0)
        rc = npreq_finish(&g_lookup_req, req, NP_SIGNED_OUT); /* np_id untouched */
    return ret("sceNpLookupNpId", rc);
}

static s32 hle_sceNpBandwidthTestInitStart(void)
{
    FIRST("sceNpBandwidthTestInitStart -> SIGNED_OUT");
    return NP_SIGNED_OUT;
}
static s32 hle_sceNpBandwidthTestGetStatus(void) { return NP_SIGNED_OUT; }
static s32 hle_sceNpBandwidthTestAbort(void) { return 0; }
static s32 hle_sceNpBandwidthTestShutdown(void) { return 0; }

/* ---- NpCommerce dialog: the user closes it at once.  A real one would open
 * the store under SWRR's identity, without controller focus. ---- */

enum { CD_NONE, CD_INITIALIZED, CD_RUNNING, CD_FINISHED };
static s32 g_cd_status;
static void *g_cd_userdata;

struct commerce_param {
    u64 size;
    u8 reserved[36];
    u32 magic;
    s32 param_size, user, mode;
    u32 service_label;
    const char *const *targets;
    u32 ntargets, pad;
    u64 features;
    void *userdata;
    u8 reserved2[32];
};

struct commerce_result {
    s32 result;
    bool authorized;
    void *userdata;
    u8 reserved[32];
};

static s32 commerce_init(const char *fn)
{
    spin_lock(&g_np_lock);
    s32 rc = g_cd_status == CD_NONE ? 0 : CD_ALREADY_INITIALIZED;
    if (rc == 0)
        g_cd_status = CD_INITIALIZED;
    spin_unlock(&g_np_lock);
    FIRST("%s -> %x", fn, (u32)rc);
    return ret(fn, rc);
}

static s32 hle_sceNpCommerceDialogInitialize(void) { return commerce_init("sceNpCommerceDialogInitialize"); }
static s32 hle_sceNpCommerceDialogInitializeInternal(void)
{
    return commerce_init("sceNpCommerceDialogInitializeInternal");
}

static s32 hle_sceNpCommerceDialogTerminate(void)
{
    spin_lock(&g_np_lock);
    s32 rc = g_cd_status == CD_NONE ? CD_NOT_INITIALIZED : 0;
    g_cd_status = CD_NONE;
    spin_unlock(&g_np_lock);
    return ret("sceNpCommerceDialogTerminate", rc);
}

static s32 hle_sceNpCommerceDialogOpen(const struct commerce_param *p)
{
    s32 rc = 0;
    spin_lock(&g_np_lock);
    if (g_cd_status != CD_INITIALIZED && g_cd_status != CD_FINISHED)
        rc = CD_NOT_INITIALIZED;
    else if (!p)
        rc = CD_ARG_NULL;
    else if (p->mode < 0 || p->mode > 5)
        rc = CD_PARAM_INVALID;
    else
        g_cd_status = CD_RUNNING, g_cd_userdata = p->userdata;
    spin_unlock(&g_np_lock);
    if (rc == 0)
        logf("sceNpCommerceDialogOpen: mode %d, service label %u, target %.47s: closed by the user at once",
             p->mode, p->service_label, p->targets && p->ntargets && p->targets[0] ? p->targets[0] : "-");
    return ret("sceNpCommerceDialogOpen", rc);
}

static s32 hle_sceNpCommerceDialogUpdateStatus(void)
{
    spin_lock(&g_np_lock);
    if (g_cd_status == CD_RUNNING)
        g_cd_status = CD_FINISHED;
    s32 st = g_cd_status;
    spin_unlock(&g_np_lock);
    return st;
}

static s32 hle_sceNpCommerceDialogGetStatus(void) { return g_cd_status; }

static s32 hle_sceNpCommerceDialogGetResult(struct commerce_result *r)
{
    s32 rc = 0;
    spin_lock(&g_np_lock);
    if (!r)
        rc = CD_ARG_NULL;
    else if (g_cd_status != CD_FINISHED)
        rc = CD_NOT_FINISHED;
    else {
        memset(r, 0, sizeof(*r));
        r->result = 1; /* USER_CANCELED */
        r->userdata = g_cd_userdata;
    }
    spin_unlock(&g_np_lock);
    return ret("sceNpCommerceDialogGetResult", rc);
}

static s32 hle_sceNpCommerceDialogClose(void)
{
    spin_lock(&g_np_lock);
    s32 st = g_cd_status;
    s32 rc = st == CD_NONE ? CD_NOT_INITIALIZED
             : st == CD_INITIALIZED ? CD_NOT_RUNNING
             : st == CD_FINISHED ? CD_ALREADY_CLOSE
             : 0;
    if (rc == 0)
        g_cd_status = CD_FINISHED;
    spin_unlock(&g_np_lock);
    return ret("sceNpCommerceDialogClose", rc);
}

static s32 hle_sceNpCommerceShowPsStoreIcon(s32 pos)
{
    FIRST("sceNpCommerceShowPsStoreIcon(%d): not drawn", pos);
    return 0;
}
static s32 hle_sceNpCommerceHidePsStoreIcon(void) { return 0; }
static s32 hle_sceNpCommerceSetPsStoreIconLayout(s32 layout) { return 0; }

/* ---- registration ---- */

#define M(fn) {"libSceNpManager", #fn, hle_##fn, NULL}
#define TK(fn) {"libSceNetCtlForNpToolkit", #fn, hle_##fn, NULL}
#define AU(fn) {"libSceNpAuth", #fn, hle_##fn, NULL}
#define CO(fn) {"libSceNpCommon", #fn, hle_##fn, NULL}
#define UT(fn) {"libSceNpUtility", #fn, hle_##fn, NULL}
#define CM(fn) {"libSceNpCommerce", #fn, hle_##fn, NULL}

static const struct ext_hle_ent k_hle[] = {
    /* NpManager: state, callbacks, identity */
    M(sceNpGetState),
    M(sceNpRegisterStateCallbackA),
    M(sceNpUnregisterStateCallbackA),
    M(sceNpRegisterStateCallback),
    M(sceNpUnregisterStateCallback),
    M(sceNpRegisterNpReachabilityStateCallback),
    M(sceNpUnregisterNpReachabilityStateCallback),
    M(sceNpCheckCallback),
    M(sceNpCheckCallbackForLib),
    M(sceNpRegisterStateCallbackForToolkit),
    M(sceNpUnregisterStateCallbackForToolkit),
    M(sceNpGetNpReachabilityState),
    M(sceNpHasSignedUp),
    M(sceNpGetOnlineId),
    M(sceNpGetNpId),
    M(sceNpGetAccountIdA),
    M(sceNpGetAccountId),
    M(sceNpGetUserIdByAccountId),
    M(sceNpGetUserIdByOnlineId),
    M(sceNpGetAccountCountryA),
    M(sceNpGetAccountCountry),
    M(sceNpGetAccountDateOfBirthA),
    M(sceNpGetAccountDateOfBirth),
    M(sceNpIsPlusMember),
    M(sceNpRegisterPlusEventCallback),
    M(sceNpUnregisterPlusEventCallback),
    M(sceNpNotifyPlusFeature),
    M(sceNpSetContentRestriction),
    M(sceNpSetNpTitleId),
    M(sceNpGetGamePresenceStatusA),
    M(sceNpGetGamePresenceStatus),
    M(sceNpRegisterGamePresenceCallback),
    M(sceNpRegisterGamePresenceCallbackA),
    M(sceNpUnregisterGamePresenceCallbackA),
    /* NpManager requests */
    M(sceNpCreateRequest),
    M(sceNpCreateAsyncRequest),
    M(sceNpDeleteRequest),
    M(sceNpAbortRequest),
    M(sceNpPollAsync),
    M(sceNpWaitAsync),
    M(sceNpSetTimeout),
    M(sceNpCheckNpAvailabilityA),
    M(sceNpCheckNpAvailability),
    M(sceNpCheckNpReachability),
    M(sceNpCheckPlus),
    M(sceNpGetParentalControlInfoA),
    M(sceNpGetParentalControlInfo),
    M(sceNpGetAccountLanguageA),
    M(sceNpGetAccountLanguage),
    M(sceNpGetAccountLanguage2),
    M(sceNpSetGamePresenceOnlineA),
    M(sceNpSetGamePresenceOnline),
    /* NpManager InGameMessage */
    M(sceNpInGameMessageInitialize),
    M(sceNpInGameMessageTerminate),
    M(sceNpInGameMessageCreateHandle),
    M(sceNpInGameMessageDeleteHandle),
    M(sceNpInGameMessageAbortHandle),
    M(sceNpInGameMessageCheckCallback),
    M(sceNpInGameMessageGetMemoryPoolStatistics),
    {"libSceNpManager", "sceNpInGameMessagePrepareA", hle_np_signed_out, NULL},
    {"libSceNpManager", "sceNpInGameMessageSendDataA", hle_np_signed_out, NULL},
    /* NetCtl for NpToolkit */
    TK(sceNetCtlRegisterCallbackForNpToolkit),
    TK(sceNetCtlUnregisterCallbackForNpToolkit),
    TK(sceNetCtlCheckCallbackForNpToolkit),
    TK(sceNetCtlClearEventForNpToolkit),
    /* NpAuth */
    AU(sceNpAuthCreateRequest),
    AU(sceNpAuthCreateAsyncRequest),
    AU(sceNpAuthDeleteRequest),
    AU(sceNpAuthAbortRequest),
    AU(sceNpAuthPollAsync),
    AU(sceNpAuthWaitAsync),
    AU(sceNpAuthSetTimeout),
    AU(sceNpAuthGetAuthorizationCodeV3),
    AU(sceNpAuthGetAuthorizationCodeA),
    AU(sceNpAuthGetAuthorizationCode),
    AU(sceNpAuthGetIdTokenV3),
    AU(sceNpAuthGetIdTokenA),
    AU(sceNpAuthGetIdToken),
    /* NpCommon */
    CO(sceNpCreateThread),
    CO(sceNpCalloutInitCtx),
    CO(sceNpCalloutStartOnCtx),
    CO(sceNpCalloutStartOnCtx64),
    CO(sceNpCalloutStopOnCtx),
    CO(sceNpCalloutTermCtx),
    /* NpUtility */
    UT(sceNpUtilityInit),
    UT(sceNpUtilityTerm),
    UT(sceNpWordFilterCreateTitleCtxA),
    UT(sceNpWordFilterCreateTitleCtx),
    UT(sceNpWordFilterDeleteTitleCtx),
    UT(sceNpWordFilterCreateRequest),
    UT(sceNpWordFilterCreateAsyncRequest),
    UT(sceNpWordFilterDeleteRequest),
    UT(sceNpWordFilterAbortRequest),
    UT(sceNpWordFilterCensorComment),
    UT(sceNpWordFilterSanitizeComment),
    UT(sceNpWordFilterPollAsync),
    UT(sceNpWordFilterWaitAsync),
    UT(sceNpWordFilterSetTimeout),
    UT(sceNpLookupCreateTitleCtxA),
    UT(sceNpLookupCreateTitleCtx),
    UT(sceNpLookupDeleteTitleCtx),
    UT(sceNpLookupCreateRequest),
    UT(sceNpLookupCreateAsyncRequest),
    UT(sceNpLookupDeleteRequest),
    UT(sceNpLookupAbortRequest),
    UT(sceNpLookupNpId),
    UT(sceNpLookupPollAsync),
    UT(sceNpLookupWaitAsync),
    UT(sceNpLookupSetTimeout),
    UT(sceNpBandwidthTestInitStart),
    UT(sceNpBandwidthTestGetStatus),
    UT(sceNpBandwidthTestAbort),
    UT(sceNpBandwidthTestShutdown),
    /* NpCommerce */
    CM(sceNpCommerceDialogInitialize),
    CM(sceNpCommerceDialogInitializeInternal),
    CM(sceNpCommerceDialogTerminate),
    CM(sceNpCommerceDialogOpen),
    CM(sceNpCommerceDialogUpdateStatus),
    CM(sceNpCommerceDialogGetStatus),
    CM(sceNpCommerceDialogGetResult),
    CM(sceNpCommerceDialogClose),
    CM(sceNpCommerceShowPsStoreIcon),
    CM(sceNpCommerceHidePsStoreIcon),
    CM(sceNpCommerceSetPsStoreIconLayout),
};

/* Imports this file does not implement: NpManager, NpAuth and NpCommerce get
 * a stub with the library's error, never SWRR's library.  NpUtility and
 * NpCommon fall back to the console's (by name). */
static const struct ext_lib k_libs[] = {
    {"libSceNpManager", "libSceNpManagerCompat libSceNpManagerForToolkit", EXT_LIB_NO_HOST, NP_SIGNED_OUT},
    {"libSceNpAuth", "libSceNpAuthCompat", EXT_LIB_NO_HOST, NP_SIGNED_OUT},
    {"libSceNpCommerce", NULL, EXT_LIB_NO_HOST, CD_NOT_SUPPORTED},
    {"libSceNpUtility", "libSceNpUtilityCompat", 0, 0},
    {"libSceNpCommon", "libSceNpCommonCompat", 0, 0},
};

EXT_FAMILY_LIBS("np", k_hle, k_libs, NULL);

#pragma clang attribute pop
