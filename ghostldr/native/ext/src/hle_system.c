/*
 * libSceSystemService and libSceUserService, with the secondary libraries a
 * host lookup by name never reaches (PlatformPrivacy, Suspend, WebBrowser,
 * RegisteredUserIdList).
 *
 * SWRR initialized both services, owns their per-process event queues and
 * consumed their boot-time events.  Console, settings and user answers PASS
 * to the real service; what is addressed to the app (invitations, add-ons,
 * PlayGo, launch links, game intents) is SWRR's and is dropped; calls that
 * would end SWRR (LoadExec, LaunchApp) are refused; the LOGIN events SWRR
 * consumed are synthesized for every logged-in user.  Registry writes
 * (libSceRegMgr) are refused.
 * Spec: research/hle-specs/system-appcontent.md sections 4 and 5.
 */
#include "sys_title.h"

#define SS_INTERNAL ((s32)0x80A10001)
#define SS_PARAMETER ((s32)0x80A10003)
#define SS_NO_EVENT ((s32)0x80A10004)
#define SS_REJECTED ((s32)0x80A10005)
#define US_ALREADY_INITIALIZED ((s32)0x80960003)
#define US_INVALID_ARGUMENT ((s32)0x80960005)
#define US_NO_EVENT ((s32)0x80960007)
#define EVENT_SIZE 0x2004 /* SceSystemServiceEvent (Ghost memsets 0x2004) */
#define EVENT_TRIES 16

/* a host function, looked up once (a miss is logged once) */
static void *real_once(void *volatile *slot, const char *mod, const char *name)
{
    void *f = *slot;
    if (!f) {
        f = host_real_in(mod, name);
        *slot = f ? f : (void *)1;
    }
    return f == (void *)1 ? NULL : f;
}
#define SS_REAL(type, name) \
    static void *volatile slot_; \
    __typeof__(type) real = (__typeof__(type))real_once(&slot_, "libSceSystemService", name)
#define US_REAL(type, name) \
    static void *volatile slot_; \
    __typeof__(type) real = (__typeof__(type))real_once(&slot_, "libSceUserService", name)

s32 system_lang(void)
{
    SS_REAL(s32(*)(s32, s32 *), "sceSystemServiceParamGetInt");
    s32 v = -1;
    return real && real(1, &v) == 0 ? v : -1;
}

/* ---- libSceSystemService ---- */

static s32 hle_sceSystemServiceGetStatus(u8 *st)
{
    SS_REAL(s32(*)(u8 *), "sceSystemServiceGetStatus");
    static u8 last[9];
    if (!st)
        return SS_PARAMETER;
    s32 rc = real ? real(st) : SS_INTERNAL;
    if (rc != 0)
        return rc;
    /* SWRR's process still counts as overlaid by system UI (left over from
     * the exploit's loader dialog); titles pause while that is set */
    st[4] = 0;
    if (memcmp(last, st, sizeof(last)) != 0) {
        memcpy(last, st, sizeof(last));
        logf("sceSystemServiceGetStatus: events %d uiOverlaid %u background %u cpu7 %u streaming %u vr %u",
             *(s32 *)st, st[4], st[5], st[6], st[7], st[8]);
    }
    return 0;
}

/* Event types addressed to the application or its NP identity: inside SWRR
 * they are SWRR's (its invitations, add-ons, launch links, intents), and
 * PlayGo is virtual.  0x10000017 is the game-intent event Ghost answers
 * with sceNpGameIntentReceiveIntent. */
static bool event_is_swrrs(u32 type)
{
    static const u32 k_drop[] = {0x10000002, 0x10000003, 0x10000004, 0x10000006, 0x10000007,
                                 0x10000008, 0x10000009, 0x1000000b, 0x1000000c, 0x1000000d,
                                 0x1000000e, 0x10000010, 0x10000011, 0x10000014, 0x10000017};
    for (u32 i = 0; i < ARRAY_SIZE(k_drop); i++)
        if (k_drop[i] == type)
            return true;
    return false;
}

/* the system's own events: resume, safe area, VR, browser and settings
 * closed, share menu.  Anything else passes too, noted once. */
static bool event_known(u32 type)
{
    static const u32 k_pass[] = {0x10000000, 0x10000001, 0x10000005, 0x1000000a,
                                 0x1000000f, 0x10000012, 0x10000013, 0x30000000};
    for (u32 i = 0; i < ARRAY_SIZE(k_pass); i++)
        if (k_pass[i] == type)
            return true;
    return false;
}

static s32 hle_sceSystemServiceReceiveEvent(u8 *ev)
{
    SS_REAL(s32(*)(u8 *), "sceSystemServiceReceiveEvent");
    static u32 logged;
    if (!ev)
        return SS_PARAMETER;
    for (u32 i = 0; real && i < EVENT_TRIES; i++) {
        s32 rc = real(ev);
        if (rc != 0)
            return rc; /* NO_EVENT, or an error, as the system gives it */
        u32 type = *(u32 *)ev;
        if (!event_is_swrrs(type)) {
            if (logged++ < 64)
                logf("sceSystemServiceReceiveEvent: %x%s", type, event_known(type) ? "" : " (unknown type, passed)");
            return 0;
        }
        logf("sceSystemServiceReceiveEvent: dropped %x (addressed to SWRR)", type);
        memset(ev, 0, EVENT_SIZE);
    }
    return SS_NO_EVENT;
}

static s32 hle_sceSystemServiceParamGetInt(s32 id, s32 *val)
{
    SS_REAL(s32(*)(s32, s32 *), "sceSystemServiceParamGetInt");
    static u32 logged;
    s32 rc = real ? real(id, val) : SS_INTERNAL;
    if (logged++ < 24)
        logf("sceSystemServiceParamGetInt(%d) -> %x value %d", id, (u32)rc, rc == 0 && val ? *val : -1);
    return rc;
}

/* the report would be filed against SWRR */
static s32 hle_sceSystemServiceReportAbnormalTermination(const void *info)
{
    logf("sceSystemServiceReportAbnormalTermination(%p) ignored", info);
    return 0;
}

/* a launch would suspend or replace SWRR, and ghostldr with it */
static s32 hle_sceSystemServiceLaunchApp(const char *title, const char *const *argv, const void *param)
{
    logf("sceSystemServiceLaunchApp(%.16s) refused: it would end SWRR", title ? title : "?");
    return SS_REJECTED;
}

static s32 hle_sceSystemServiceLoadExec(const char *path, const char *const *argv)
{
    logf("sceSystemServiceLoadExec(%.128s) refused: it would replace SWRR", path ? path : "?");
    return SS_REJECTED;
}

/* PlatformPrivacy: Ghost passes one out pointer (eboot 0x8a02db) and acts on
 * values 1 and 2; 0 is the default setting */
static s32 hle_sceSystemServiceGetPlatformPrivacySetting(s32 *out)
{
    if (!out)
        return SS_PARAMETER;
    *out = 0;
    return 0;
}

/* LaunchWebBrowser lives in libSceSystemServiceWebBrowser: PASS when the host
 * finds it by name, else refused */
static s32 hle_sceSystemServiceLaunchWebBrowser(const char *uri, const void *param)
{
    SS_REAL(s32(*)(const char *, const void *), "sceSystemServiceLaunchWebBrowser");
    s32 rc = real ? real(uri, param) : SS_REJECTED;
    logf("sceSystemServiceLaunchWebBrowser(%.64s) -> %x", uri ? uri : "?", (u32)rc);
    return rc;
}

/* no side effect for the title; the outs of the unverified ones are not written */
static s32 ss_ok(void) { return 0; }

/* ---- libSceUserService ---- */

#define USER_LOGIN 0
#define USER_LOGOUT 1
#define USERS 4
struct user_event {
    s32 type, user;
};

/* LOGIN events for the users logged in at start, which SWRR consumed: the
 * initial user first, then the others in login-list order */
static struct user_event g_uq[2 * USERS];
static u32 g_uq_head, g_uq_count;
static s32 g_synth[USERS]; /* users given a LOGIN with no LOGOUT since */
static u32 g_nsynth;
static volatile int g_user_lock;
static volatile bool g_primed;

static bool user_valid(s32 u) { return u != -1 && u != 0xFF; }

static s32 swrr_user(void)
{
    static s32 u = -1;
    if (u == -1) {
        US_REAL(s32(*)(s32 *), "sceUserServiceGetInitialUser");
        s32 v = -1;
        if (real && real(&v) == 0)
            u = v;
    }
    return u;
}

static u32 picked(void)
{
    u32 *p = G->size >= __builtin_offsetof(struct ghost_api, title_user) + sizeof(void *) ? G->title_user : NULL;
    return p ? *p : 0;
}

s32 title_user(void)
{
    u32 p = picked();
    return p ? (s32)p : swrr_user();
}

bool title_user_picked(void)
{
    u32 p = picked();
    return p && (s32)p != swrr_user();
}

/* sceUserServiceGetInitialUser: the user the title runs for */
static s32 hle_sceUserServiceGetInitialUser(s32 *user)
{
    if (!user)
        return US_INVALID_ARGUMENT;
    s32 u = title_user();
    if (u == -1)
        return (s32)0x80960002; /* NOT_INITIALIZED */
    *user = u;
    static u32 said;
    if (title_user_picked() && !said++)
        logf("sceUserService: the title runs for user %x (picked in the game selector; SWRR runs for %x)", (u32)u,
             (u32)swrr_user());
    return 0;
}

/* scePadOpen / scePadGetHandle: a title running for a picked user reads the
 * controller of the user running SWRR, who picked it in the game selector
 * (on a PS5 every signed-in user has a controller of their own).  As the
 * core's scePadOpen otherwise: SWRR has the pad open already, so a refused
 * open gets SWRR's handle. */
static s32 pad_user(s32 user)
{
    static u32 said;
    if (!title_user_picked() || user != title_user())
        return user;
    if (!said++)
        logf("pad: the title opens the pad of user %x: the controller of user %x (SWRR's) instead", (u32)user,
             (u32)swrr_user());
    return swrr_user();
}

static s32 hle_scePadGetHandle(s32 user, s32 type, s32 index)
{
    static s32 (*get)(s32, s32, s32);
    if (!get)
        get = host_real_in("libScePad", "scePadGetHandle");
    return get ? get(pad_user(user), type, index) : (s32)0x80920001;
}

static s32 hle_scePadOpen(s32 user, s32 type, s32 index, const void *param)
{
    static s32 (*real)(s32, s32, s32, const void *);
    if (!real)
        real = host_real_in("libScePad", "scePadOpen");
    if (!real)
        return (s32)0x80920001;
    user = pad_user(user);
    s32 h = real(user, type, index, param);
    if (h < 0) {
        s32 existing = hle_scePadGetHandle(user, type, index);
        logf("scePadOpen(%x,%d,%d) -> %x, reusing handle %x", (u32)user, type, index, (u32)h, (u32)existing);
        if (existing >= 0)
            return existing;
    }
    return h;
}

static bool synthesized(s32 u)
{
    for (u32 i = 0; i < g_nsynth; i++)
        if (g_synth[i] == u)
            return true;
    return false;
}

static s32 login_list(s32 ids[USERS])
{
    US_REAL(s32(*)(s32 *), "sceUserServiceGetLoginUserIdList");
    for (u32 i = 0; i < USERS; i++)
        ids[i] = -1;
    return real ? real(ids) : US_INVALID_ARGUMENT;
}

static void user_prime(void)
{
    if (__atomic_load_n(&g_primed, __ATOMIC_ACQUIRE))
        return;
    US_REAL(s32(*)(s32 *), "sceUserServiceGetInitialUser");
    s32 initial = -1, ids[USERS];
    if (real)
        real(&initial);
    login_list(ids);
    spin_lock(&g_user_lock);
    if (!g_primed) {
        for (s32 i = -1; i < USERS; i++) {
            s32 u = i < 0 ? initial : ids[i];
            if (!user_valid(u) || synthesized(u) || g_nsynth == USERS)
                continue;
            g_synth[g_nsynth++] = u;
            g_uq[(g_uq_head + g_uq_count++) % ARRAY_SIZE(g_uq)] = (struct user_event){USER_LOGIN, u};
            logf("sceUserService: LOGIN for user %x queued (logged in before the title)", (u32)u);
        }
        __atomic_store_n(&g_primed, true, __ATOMIC_RELEASE);
    }
    spin_unlock(&g_user_lock);
}

static s32 hle_sceUserServiceInitialize(const void *param)
{
    US_REAL(s32(*)(const void *), "sceUserServiceInitialize");
    s32 rc = real ? real(param) : US_ALREADY_INITIALIZED;
    if (rc == US_ALREADY_INITIALIZED)
        rc = 0; /* SWRR's */
    user_prime();
    logf("sceUserServiceInitialize -> %x", (u32)rc);
    return rc;
}

static s32 hle_sceUserServiceInitialize2(s32 prio, u64 affinity)
{
    US_REAL(s32(*)(s32, u64), "sceUserServiceInitialize2");
    s32 rc = real ? real(prio, affinity) : US_ALREADY_INITIALIZED;
    if (rc == US_ALREADY_INITIALIZED)
        rc = 0;
    user_prime();
    return rc;
}

/* never SWRR's service */
static s32 hle_sceUserServiceTerminate(void) { return 0; }

static s32 hle_sceUserServiceGetEvent(struct user_event *ev)
{
    US_REAL(s32(*)(struct user_event *), "sceUserServiceGetEvent");
    static u32 logged;
    if (!ev)
        return US_INVALID_ARGUMENT;
    user_prime();
    spin_lock(&g_user_lock);
    if (g_uq_count) {
        *ev = g_uq[g_uq_head];
        g_uq_head = (g_uq_head + 1) % ARRAY_SIZE(g_uq);
        g_uq_count--;
        spin_unlock(&g_user_lock);
        logf("sceUserServiceGetEvent -> LOGIN %x (synthesized)", (u32)ev->user);
        return 0;
    }
    spin_unlock(&g_user_lock);
    for (u32 i = 0; real && i < EVENT_TRIES; i++) {
        s32 rc = real(ev);
        if (rc != 0)
            return rc;
        spin_lock(&g_user_lock);
        bool dup = ev->type == USER_LOGIN && synthesized(ev->user);
        if (ev->type == USER_LOGOUT)
            for (u32 j = 0; j < g_nsynth; j++)
                if (g_synth[j] == ev->user)
                    g_synth[j] = g_synth[--g_nsynth];
        spin_unlock(&g_user_lock);
        if (!dup) {
            if (logged++ < 32)
                logf("sceUserServiceGetEvent -> type %d user %x", ev->type, (u32)ev->user);
            return 0;
        }
        logf("sceUserServiceGetEvent: dropped a second LOGIN for user %x", (u32)ev->user);
    }
    return US_NO_EVENT;
}

/* lives in libSceUserServiceRegisteredUserIdList: PASS when the host finds
 * it by name, else the logged-in users (all of them registered ones) */
static s32 hle_sceUserServiceGetRegisteredUserIdList(s32 *list)
{
    US_REAL(s32(*)(s32 *), "sceUserServiceGetRegisteredUserIdList");
    if (!list)
        return US_INVALID_ARGUMENT;
    if (real)
        return real(list);
    s32 ids[USERS];
    s32 rc = login_list(ids);
    for (u32 i = 0; i < 16; i++)
        list[i] = i < USERS && rc == 0 ? ids[i] : -1;
    return rc;
}

/* libSceRegMgr: reads go to the console's registry; writes are refused
 * whatever the console would allow a game.  What homebrew writes there is
 * the console's own state (offline account activation writes a user's
 * account ID, NP environment and sign-in flag), and a title in SWRR has no
 * business changing it. */
static s32 regmgr_set(u32 key)
{
    static u32 said;
    static volatile int noticed;
    if (said++ < 8)
        logf("regmgr: the title's write to registry key %x refused", key);
    ov_notice(&noticed, "The console's system settings (registry) can't be changed from SWRR on a stock console:\n"
                        "the change was not made (e.g. account activation).");
    return (s32)0x80020001; /* EPERM */
}

/* reads pass to the console's registry; when SWRR may not read a key (a
 * user's account or name, the parental passcode), the title is told on
 * screen what that costs it, once */
static volatile int g_reg_noticed;
static void regmgr_read_said(const char *fn, u32 key, s32 rc)
{
    static u32 said;
    if (said++ < 8)
        logf("regmgr: %s(%x) -> %x", fn, key, (u32)rc);
    if (rc != 0)
        ov_notice(&g_reg_noticed, "The console's system settings (registry) can't be read from SWRR on a stock console:\n"
                                  "user accounts and passcodes kept there aren't available (e.g. account activation).");
}

static s32 hle_sceRegMgrGetStr(u32 key, char *out, u32 size)
{
    static s32 (*real)(u32, char *, u32);
    if (!real)
        real = host_real_in("libSceRegMgr", "sceRegMgrGetStr");
    s32 rc = real ? real(key, out, size) : (s32)0x80020001;
    regmgr_read_said("sceRegMgrGetStr", key, rc);
    return rc;
}

static s32 hle_sceRegMgrGetBin(u32 key, void *out, u32 size)
{
    static s32 (*real)(u32, void *, u32);
    if (!real)
        real = host_real_in("libSceRegMgr", "sceRegMgrGetBin");
    s32 rc = real ? real(key, out, size) : (s32)0x80020001;
    regmgr_read_said("sceRegMgrGetBin", key, rc);
    return rc;
}

static s32 hle_sceRegMgrGetInt(u32 key, s32 *out)
{
    static s32 (*real)(u32, s32 *);
    if (!real)
        real = host_real_in("libSceRegMgr", "sceRegMgrGetInt");
    s32 rc = real ? real(key, out) : (s32)0x80020001;
    regmgr_read_said("sceRegMgrGetInt", key, rc);
    return rc;
}

#define SS "libSceSystemService"
#define US "libSceUserService"
#define E(lib, fn) {lib, #fn, hle_##fn, NULL}
static const struct ext_hle_ent k_hle[] = {
    E(SS, sceSystemServiceGetStatus),
    E(SS, sceSystemServiceReceiveEvent),
    E(SS, sceSystemServiceParamGetInt),
    E(SS, sceSystemServiceReportAbnormalTermination),
    E(SS, sceSystemServiceLaunchApp),
    E(SS, sceSystemServiceLoadExec),
    {SS, "sceSystemServiceRegisterDaemon", ss_ok, NULL},
    {SS, "sceSystemServiceGetTitleWorkaroundInfo", ss_ok, NULL}, /* SWRR's workarounds otherwise */
    E("libSceSystemServicePlatformPrivacy", sceSystemServiceGetPlatformPrivacySetting),
    {"libSceSystemServicePlatformPrivacy", "sceSystemServiceGetPlatformPrivacyDefinitionVersion", ss_ok, NULL},
    {"libSceSystemServicePlatformPrivacy", "sceSystemServiceGetPlatformPrivacyDefinitionData", ss_ok, NULL},
    {"libSceSystemServiceSuspend", "sceSystemServiceEnableSuspendNotification", ss_ok, NULL},
    {"libSceSystemServiceSuspend", "sceSystemServiceDisableSuspendNotification", ss_ok, NULL},
    {"libSceSystemServiceSuspend", "sceSystemServiceDeclareReadyForSuspend", ss_ok, NULL},
    E("libSceSystemServiceWebBrowser", sceSystemServiceLaunchWebBrowser),
    E(US, sceUserServiceGetInitialUser),
    E(US, sceUserServiceInitialize),
    E(US, sceUserServiceInitialize2),
    E(US, sceUserServiceTerminate),
    E(US, sceUserServiceGetEvent),
    E("libSceUserServiceRegisteredUserIdList", sceUserServiceGetRegisteredUserIdList),
    E("libScePad", scePadOpen),
    E("libScePad", scePadGetHandle),
    E("libSceRegMgr", sceRegMgrGetStr),
    E("libSceRegMgr", sceRegMgrGetBin),
    E("libSceRegMgr", sceRegMgrGetInt),
    {"libSceRegMgr", "sceRegMgrSetInt", regmgr_set, NULL},
    {"libSceRegMgr", "sceRegMgrSetStr", regmgr_set, NULL},
    {"libSceRegMgr", "sceRegMgrSetBin", regmgr_set, NULL},
};

EXT_FAMILY("system", k_hle, NULL);
