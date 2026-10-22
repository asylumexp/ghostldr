/*
 * The NP online families (ext/src/hle_np_online*.c) of a real ext.bin, run
 * as x86-64 code (Rosetta on Apple silicon).  env.c's boot comes first: the
 * core's ext.c fetches, places, relocates and self-tests the extension.
 * Then the HLE functions are found by library and NID the way imports bind
 * (ext_hle_lookup, resolve_import) and called: context, request and handle
 * lifecycles, the signed-out (or, with persona A, network) answers, the
 * layouts of Ghost's boot calls, and each library's catch-all stub.
 * test_ext_np_online.py builds and drives it:
 *
 *     np_online EXT_BIN CRC b|a
 *     b: the default build (persona B, Matching2 sync)
 *     a: a build with -DNPO_PERSONA_A=1 -DNPO_M2_ASYNC=1
 *
 * Prints "npo_test: ok|FAILED ..." lines; exits 0 when everything passed.
 */

/* env.c's harness, with its main and its API table renamed: the table
 * below adds the services these families use */
#define main ext_host_env_main
#define g_ghost_api g_ghost_api_env
#include "env.c"
#undef main
#undef g_ghost_api

/* np_online_sys.c */
void sys_thread_create(void *(*fn)(void *), void *arg, u64 *tid);
u64 sys_thread_self(void);
void sys_usleep(u32 us);

void spin_lock(volatile int *l)
{
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        ;
}

void spin_unlock(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

/* SWRR's libraries, for the pass-throughs */
static s32 host_gi_initialize(const void *p) { return (s32)0x80553801; /* ALREADY_INITIALIZED */ }
static s32 host_parse_npid(const char *json, u8 *npid)
{
    npid[0] = 'G';
    return 0;
}
static s32 host_sku_flag(u32 *flag)
{
    *flag = 3;
    return 0;
}
static u32 g_host_calls;

static void *fake_host_real_in(const char *mod, const char *name)
{
    g_host_calls++;
    if (strcmp(name, "sceNpGameIntentInitialize") == 0)
        return (void *)host_gi_initialize;
    if (strcmp(name, "sceNpWebApiUtilityParseNpId") == 0)
        return (void *)host_parse_npid;
    if (strcmp(name, "sceNpEntitlementAccessGetSkuFlag") == 0)
        return (void *)host_sku_flag;
    return NULL;
}

static int fake_thread_create(void *thr, const void *attr, void *(*fn)(void *), void *arg, const char *name)
{
    logf("npo_test: thread %s created", name);
    sys_thread_create(fn, arg, (u64 *)thr);
    return 0;
}

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
    .compute_nid = compute_nid,
    .spin_lock = spin_lock,
    .spin_unlock = spin_unlock,
    .host_real_in = fake_host_real_in,
    .ghost_thread_create = fake_thread_create,
};

static int k_usleep(u32 us)
{
    sys_usleep(us);
    return 0;
}

static int k_module_info_np(u32 handle, struct sce_module_info *info)
{
    strlcpy(info->name, handle == 0x99 ? "libSceNpTus.sprx" : "libSceOther.sprx", sizeof(info->name));
    return 0;
}

/* ---- checks and calls ---- */

static int g_fail;

static void check(bool ok, const char *what)
{
    logf("npo_test: %s %s", ok ? "ok" : "FAILED", what);
    if (!ok)
        g_fail = 1;
}

static void check_eq(s64 got, s64 want, const char *what)
{
    if (got == want)
        logf("npo_test: ok %s", what);
    else {
        logf("npo_test: FAILED %s: got %lx, want %lx", what, (u64)got, (u64)want);
        g_fail = 1;
    }
}

typedef u64 (*fn6)(u64, u64, u64, u64, u64, u64);

static fn6 lookup(const char *lib, const char *name)
{
    char nid[12];
    compute_nid(name, nid);
    return (fn6)ext_hle_lookup(lib, nid, name);
}

static u64 callv(const char *lib, const char *name, const u64 *a)
{
    fn6 f = lookup(lib, name);
    if (!f) {
        logf("npo_test: FAILED %s:%s is not in the extension", lib, name);
        g_fail = 1;
        return 0xBAD0BAD0;
    }
    return f(a[0], a[1], a[2], a[3], a[4], a[5]);
}

/* NP(lib, name, args...): the s32 an HLE function returns */
#define NP(lib, name, ...) ((s32)callv(lib, name, (const u64[6]){__VA_ARGS__}))
#define P(x) ((u64)(x))

#define WA "libSceNpWebApi"
#define WA2 "libSceNpWebApi2"
#define SCORE "libSceNpScore"
#define TUS "libSceNpTus"
#define M2 "libSceNpMatching2"
#define SIG "libSceNpSignaling"
#define SS "libSceNpSessionSignaling"
#define PARTY "libSceNpParty"
#define EA "libSceNpEntitlementAccess"
#define SNS "libSceNpSnsFacebook"
#define SNSD "libSceNpSnsFacebookDialog"
#define UDS "libSceNpUniversalDataSystem"
#define GI "libSceNpGameIntent"

#define NET ((s32)0x8041013D)
#define USER 0x10000001 /* any logged-in user id */
static bool A; /* the persona A + Matching2 async build */

struct resp_info {
    s32 http_status, reserved;
    char *error_object;
    u64 error_object_size, response_data_size;
};

static void dummy_cb(void) {}

/* ---- WebApi2: Ghost's eboot path ---- */
static void test_webapi2(void)
{
    s32 off = A ? NET : (s32)0x80553407;
    s32 lib = NP(WA2, "sceNpWebApi2Initialize", 5, 0x10000);
    check(lib > 0 && lib < 0x8000, "WebApi2 Initialize returns a library context > 0");
    bool all = true;
    for (int i = 0; i < 6; i++) /* more than the table holds: Ghost retries while <= 0 */
        all &= NP(WA2, "sceNpWebApi2Initialize", 5, 0x10000) > 0;
    check(all, "WebApi2 Initialize never fails");
    s32 uctx = NP(WA2, "sceNpWebApi2CreateUserContext", lib, USER);
    check(uctx > 0 && uctx >> 16 == lib, "WebApi2 user context is lib << 16 | n");
    check_eq(NP(WA2, "sceNpWebApi2CreateUserContext", lib, USER), (s32)0x8055340A, "a user's second context");
    check_eq(NP(WA2, "sceNpWebApi2CreateUserContext", 0x8000, USER), (s32)0x80553403, "lib id >= 0x8000");
    check_eq(NP(WA2, "sceNpWebApi2CreateUserContext", lib, P(-1)), (s32)0x80553402, "user id -1");
    check_eq(NP(WA2, "sceNpWebApi2CreateUserContext", 0x7000, USER), (s32)0x80553404, "unknown lib");

    s64 req = 0;
    check_eq(NP(WA2, "sceNpWebApi2CreateRequest", uctx, P("sessionManager"), P("/v1/playerSessions"), P("POST"), 0,
                P(&req)),
             0, "WebApi2 CreateRequest");
    check(req >> 32 == uctx && req >> 48 == lib && (u32)req != 0, "request id is user << 32 | n");
    s64 junk = 0;
    check_eq(NP(WA2, "sceNpWebApi2CreateRequest", uctx, P("g"), P("/p"), 0, 0, P(&junk)), (s32)0x80553402,
             "CreateRequest without a method");
    struct {
        u64 len;
        const char *type;
        u8 reserved[16];
    } cp = {5, NULL, {0}};
    check_eq(NP(WA2, "sceNpWebApi2CreateRequest", uctx, P("g"), P("/p"), P("PUT"), P(&cp), P(&junk)),
             (s32)0x80553408, "content length without a type");
    check_eq(NP(WA2, "sceNpWebApi2AddHttpRequestHeader", req, P("Content-Type"), P("application/json")), 0,
             "header before send");
    struct resp_info ri;
    memset(&ri, 0xFF, sizeof(ri));
    char err[4] = "xxx";
    ri.error_object = err, ri.error_object_size = sizeof(err);
    check_eq(NP(WA2, "sceNpWebApi2SendRequest", req, P("{}"), 2, P(&ri)), off, "SendRequest: not signed in");
    check(ri.http_status == 0 && ri.response_data_size == 0 && err[0] == 0, "SendRequest clears the response info");
    check_eq(NP(WA2, "sceNpWebApi2AddHttpRequestHeader", req, P("a"), P("b")), (s32)0x8055341A, "header after send");
    u8 buf[16];
    check_eq(NP(WA2, "sceNpWebApi2ReadData", req, P(buf), sizeof(buf)), off, "ReadData returns the send error");
    check_eq(NP(WA2, "sceNpWebApi2ReadData", req, 0, 0), (s32)0x80553402, "ReadData without a buffer");
    char val[8] = "zzzzzzz";
    check_eq(NP(WA2, "sceNpWebApi2GetHttpResponseHeaderValue", req, P("X"), P(val), sizeof(val)), off,
             "header value: the send error");
    check(val[0] == 0, "header value cleared");
    check_eq(NP(WA2, "sceNpWebApi2AbortRequest", req), 0, "AbortRequest");
    check_eq(NP(WA2, "sceNpWebApi2ReadData", req, P(buf), sizeof(buf)), (s32)0x80553409, "aborted request: ABORTED");
    check_eq(NP(WA2, "sceNpWebApi2DeleteRequest", req), 0, "DeleteRequest");
    check_eq(NP(WA2, "sceNpWebApi2DeleteRequest", req), (s32)0x80553406, "deleted request is gone");

    s32 h = NP(WA2, "sceNpWebApi2PushEventCreateHandle", lib);
    check(h > 0, "push handle");
    u8 fp[0x58] = {0};
    check_eq(NP(WA2, "sceNpWebApi2PushEventCreateFilter", lib, h, P("sessionManager"), 0xFFFFFFFFu, P(fp), 1),
             (s32)0x80553402, "push filter: service with label -1");
    s32 filter = NP(WA2, "sceNpWebApi2PushEventCreateFilter", lib, h, P("sessionManager"), 0, P(fp), 1);
    check(filter > 0 && filter != h, "push filter");
    check_eq(NP(WA2, "sceNpWebApi2PushEventRegisterCallback", uctx, filter, 0, 0), (s32)0x80553402,
             "push callback NULL");
    s32 cb = NP(WA2, "sceNpWebApi2PushEventRegisterPushContextCallback", uctx, filter, P(dummy_cb), 0);
    check(cb > 0, "push context callback registered (never called)");
    check_eq(NP(WA2, "sceNpWebApi2PushEventUnregisterPushContextCallback", uctx, cb), 0, "unregister");
    check_eq(NP(WA2, "sceNpWebApi2PushEventUnregisterCallback", uctx, cb), (s32)0x8055340C, "unregister twice");
    char pc[0x28];
    memset(pc, 0, sizeof(pc)); /* Ghost clears 0x28 bytes */
    check_eq(NP(WA2, "sceNpWebApi2PushEventCreatePushContext", uctx, P(pc)), 0, "CreatePushContext");
    check(strcmp(pc, "00000000-0000-4000-8000-000000000001") == 0, "push context id is a UUID string");
    check_eq(NP(WA2, "sceNpWebApi2PushEventStartPushContextCallback", uctx, P(pc)), 0, "StartPushContextCallback");
    check_eq(NP(WA2, "sceNpWebApi2PushEventDeletePushContext", uctx, P(pc)), 0, "DeletePushContext");
    check_eq(NP(WA2, "sceNpWebApi2PushEventDeletePushContext", uctx, P(pc)), (s32)0x8055341C, "deleted push context");
    check_eq(NP(WA2, "sceNpWebApi2PushEventDeleteFilter", lib, filter), 0, "DeleteFilter");
    check_eq(NP(WA2, "sceNpWebApi2PushEventDeleteFilter", lib, filter), (s32)0x8055340B, "deleted filter");
    check_eq(NP(WA2, "sceNpWebApi2PushEventDeleteHandle", lib, h), 0, "DeleteHandle");
    check_eq(NP(WA2, "sceNpWebApi2PushEventAbortHandle", lib, h), (s32)0x8055340D, "deleted handle");
    u64 stats[4] = {9, 9, 9, 9};
    check_eq(NP(WA2, "sceNpWebApi2GetMemoryPoolStats", lib, P(stats)), 0, "GetMemoryPoolStats");
    check(stats[0] == 0x10000 && stats[1] == 0 && stats[2] == 0, "pool stats: {pool, 0, 0}");

    check_eq(NP(WA2, "sceNpWebApi2DeleteUserContext", uctx), 0, "DeleteUserContext");
    check_eq(NP(WA2, "sceNpWebApi2CreateRequest", uctx, P("g"), P("/p"), P("GET"), 0, P(&junk)), (s32)0x80553405,
             "request on a deleted user context");
    check_eq(NP(WA2, "sceNpWebApi2Terminate", lib), 0, "Terminate");
    check_eq(NP(WA2, "sceNpWebApi2Terminate", lib), (s32)0x80553404, "Terminate twice");
    check(NP("libSceNpWebApi2AsyncRestricted", "sceNpWebApi2Initialize", 1, 0x1000) > 0,
          "WebApi2 through libSceNpWebApi2AsyncRestricted");
}

/* ---- WebApi: NpToolkit2's path ---- */
static void test_webapi(void)
{
    s32 off = A ? NET : (s32)0x80552907;
    s32 lib = NP(WA, "sceNpWebApiInitialize", 3, 0x20000);
    check(lib > 0, "WebApi Initialize");
    s32 uctx = NP(WA, "sceNpWebApiCreateContextA", lib, USER);
    check(uctx > 0 && uctx >> 16 == lib, "WebApi CreateContextA");
    s64 req = 0;
    check_eq(NP(WA, "sceNpWebApiCreateRequest", uctx, P("commerce"), P("/v1/users/me/container"), 0, 0, P(&req)), 0,
             "WebApi CreateRequest (GET)");
    struct resp_info ri;
    memset(&ri, 0xFF, sizeof(ri));
    ri.error_object = NULL;
    check_eq(NP(WA, "sceNpWebApiSendRequest2", req, 0, 0, P(&ri)), off, "SendRequest2: not signed in");
    check(ri.http_status == 0 && ri.response_data_size == 0, "SendRequest2 response info");
    u64 len = 7;
    check_eq(NP(WA, "sceNpWebApiGetHttpResponseHeaderValueLength", req, P("X"), P(&len)), off, "header length");
    check(len == 0, "header length 0");
    check_eq(NP(WA, "sceNpWebApiDeleteRequest", req), 0, "DeleteRequest");
    s32 h = NP(WA, "sceNpWebApiCreateHandle", lib);
    u8 fp[0x58] = {0};
    s32 f = NP(WA, "sceNpWebApiCreateExtdPushEventFilter", lib, h, 0, 0, P(fp), 1);
    check(h > 0 && f > 0, "handle and extended push filter");
    check_eq(NP(WA, "sceNpWebApiCreateExtdPushEventFilter", lib, h, 0, 0, 0, 1), (s32)0x80552902, "filter NULL");
    s32 cb = NP(WA, "sceNpWebApiRegisterExtdPushEventCallbackA", uctx, f, P(dummy_cb), 0);
    check(cb > 0, "RegisterExtdPushEventCallbackA");
    check_eq(NP(WA, "sceNpWebApiUnregisterExtdPushEventCallback", uctx, cb), 0, "UnregisterExtdPushEventCallback");
    check_eq(NP(WA, "sceNpWebApiDeleteExtdPushEventFilter", lib, f), 0, "DeleteExtdPushEventFilter");
    check_eq(NP(WA, "sceNpWebApiDeleteExtdPushEventFilter", lib, f), (s32)0x8055291C, "deleted extended filter");
    u64 stats[4] = {9, 9, 9, 9};
    NP(WA, "sceNpWebApiGetMemoryPoolStats", lib, P(stats));
    check(stats[0] == 0x20000 && stats[1] == 0, "WebApi pool stats");
    u8 npid[0x24] = {0};
    check(NP(WA, "sceNpWebApiUtilityParseNpId", P("{}"), P(npid)) == 0 && npid[0] == 'G',
          "UtilityParseNpId goes to SWRR's decoder");
    check_eq(NP(WA, "sceNpWebApiTerminate", lib), 0, "WebApi Terminate");
    check_eq(NP(WA, "sceNpWebApiDeleteContext", uctx), (s32)0x80552905, "Terminate dropped the user context");
    check(NP("libSceNpWebApiCompat", "sceNpWebApiInitialize", 1, 0x1000) > 0, "WebApi through libSceNpWebApiCompat");
}

/* ---- Score, Tus and Tss ---- */
static void test_score_tus(void)
{
    s32 off = A ? NET : (s32)0x80550705;
    check_eq(NP(SCORE, "sceNpScoreCreateNpTitleCtxA", 0xFFFFFFFFu, USER), (s32)0x80550704, "Score label -1");
    check_eq(NP(SCORE, "sceNpScoreCreateNpTitleCtx", 0, 0), (s32)0x8055070C, "Score legacy ctx without an NpId");
    s32 res = 0;
    if (!A) {
        check_eq(NP(SCORE, "sceNpScoreCreateNpTitleCtxA", 0, USER), (s32)0x80550006, "Score ctx: SIGNED_OUT");
        check_eq(NP(TUS, "sceNpTusCreateNpTitleCtxA", 0, USER), (s32)0x80550006, "Tus ctx: SIGNED_OUT");
        check_eq(NP(TUS, "sceNpTssCreateNpTitleCtxA", 0, USER), (s32)0x80550006, "Tss ctx: SIGNED_OUT");
        check_eq(NP(SCORE, "sceNpScoreCreateRequest", 1), (s32)0x8055070E, "no Score request without a ctx");
        check_eq(NP(SCORE, "sceNpScoreRecordScore", 1, 7, 100), (s32)0x8055070E, "RecordScore: INVALID_ID");
        check_eq(NP(TUS, "sceNpTssGetData", 1, 0), (s32)0x8055070E, "TssGetData: INVALID_ID");
        check_eq(NP(SCORE, "sceNpScorePollAsync", 1, P(&res)), (s32)0x8055070E, "PollAsync: INVALID_ID");
        return;
    }
    s32 ctx = NP(SCORE, "sceNpScoreCreateNpTitleCtxA", 0, USER);
    s32 req = NP(SCORE, "sceNpScoreCreateRequest", ctx);
    check(ctx > 0 && req > 0, "Score ctx and request");
    check_eq(NP(SCORE, "sceNpScoreRecordScore", req, 7, 100), off, "RecordScore: network error");
    check_eq(NP(SCORE, "sceNpScoreGetRankingByRangeAsync", req, 7, 1), 0, "async op returns 0");
    check_eq(NP(SCORE, "sceNpScorePollAsync", req, P(&res)), 0, "PollAsync: finished");
    check_eq(res, off, "PollAsync result: network error");
    res = 0;
    check_eq(NP(SCORE, "sceNpScoreWaitAsync", req, P(&res)), 0, "WaitAsync never blocks");
    check_eq(NP(SCORE, "sceNpScoreAbortRequest", req), 0, "AbortRequest");
    check_eq(NP(SCORE, "sceNpScoreRecordScore", req, 7, 100), (s32)0x80550707, "aborted request: ABORTED");
    check_eq(NP(SCORE, "sceNpScoreDeleteNpTitleCtx", ctx), 0, "DeleteNpTitleCtx");
    check_eq(NP(SCORE, "sceNpScoreDeleteRequest", req), (s32)0x8055070E, "requests go with their ctx");
    s32 tss = NP(TUS, "sceNpTssCreateNpTitleCtxA", 0, USER);
    s32 treq = NP(TUS, "sceNpTusCreateRequest", tss);
    check(tss > 0 && treq > 0, "Tus request on a Tss context");
    check_eq(NP(TUS, "sceNpTssGetData", treq, 0, 0, 0, 0, 0), off, "TssGetData: network error");
    check_eq(NP("libSceNpTusCompat", "sceNpTusDeleteNpTitleCtx", tss), 0, "Tus through libSceNpTusCompat");
}

/* ---- Matching2 ---- */
static struct {
    volatile u32 calls;
    u16 ctx, event;
    u8 cause;
    s32 err;
    void *arg;
    u64 thread;
} g_m2cb;

static void m2_cb(u16 ctx, u16 event, u8 cause, s32 err, void *arg)
{
    g_m2cb.ctx = ctx, g_m2cb.event = event, g_m2cb.cause = cause, g_m2cb.err = err, g_m2cb.arg = arg;
    g_m2cb.thread = sys_thread_self();
    __atomic_add_fetch(&g_m2cb.calls, 1, __ATOMIC_SEQ_CST);
}

static bool m2_wait(u32 calls)
{
    for (int i = 0; i < 500 && __atomic_load_n(&g_m2cb.calls, __ATOMIC_SEQ_CST) < calls; i++)
        sys_usleep(2000);
    return __atomic_load_n(&g_m2cb.calls, __ATOMIC_SEQ_CST) >= calls;
}

static void test_matching2(void)
{
    u16 ctx = 0;
    struct {
        s32 user;
        u32 label;
        u64 size;
    } cp = {USER, 0, 0x10};
    check_eq(NP(M2, "sceNpMatching2CreateContextA", P(&cp), P(&ctx)), (s32)0x80550C03, "CreateContextA before init");
    u8 ip[0x30] = {0};
    *(u64 *)(ip + 0x20) = 0x20;
    check_eq(NP(M2, "sceNpMatching2Initialize", P(ip)), (s32)0x80550C0A, "Initialize: bad size");
    *(u64 *)(ip + 0x20) = 0x30;
    check_eq(NP(M2, "sceNpMatching2Initialize", P(ip)), 0, "Initialize");
    check_eq(NP(M2, "sceNpMatching2Initialize", P(ip)), (s32)0x80550C02, "Initialize twice");
    cp.size = 0x18;
    check_eq(NP(M2, "sceNpMatching2CreateContextA", P(&cp), P(&ctx)), (s32)0x80550C0A, "CreateContextA: bad size");
    cp.size = 0x10;
    check_eq(NP(M2, "sceNpMatching2CreateContextA", P(&cp), P(&ctx)), 0, "CreateContextA");
    check_eq(ctx, 1, "first Matching2 context is 1");
    check_eq(NP(M2, "sceNpMatching2RegisterContextCallback", P(m2_cb), 0x1234), 0, "RegisterContextCallback");
    check_eq(NP(M2, "sceNpMatching2RegisterRoomEventCallback", ctx, P(m2_cb), 0), 0, "RegisterRoomEventCallback");
    check_eq(NP(M2, "sceNpMatching2RegisterSignalingCallback", 5, P(m2_cb), 0), (s32)0x80550C0B,
             "per-context callback, unknown ctx");
    u32 reqid = 77;
    check_eq(NP(M2, "sceNpMatching2SearchRoom", ctx, 0, 0, P(&reqid)), (s32)0x80550C08, "SearchRoom: not started");
    check_eq(reqid, 77, "no request id");
    check_eq(NP(M2, "sceNpMatching2JoinRoomA", 6, 0, 0, P(&reqid)), (s32)0x80550C0B, "request on an unknown ctx");
    u64 mem[3] = {1, 2, 3};
    check(NP(M2, "sceNpMatching2GetMemoryInfo", P(mem)) == 0 && !mem[0] && !mem[1] && !mem[2], "GetMemoryInfo");
    s32 st = 5;
    check_eq(NP(M2, "sceNpMatching2SignalingGetConnectionStatus", ctx, 1, 1, P(&st), 0, 0), (s32)0x80550E0E,
             "SignalingGetConnectionStatus");
    check_eq(st, 0, "connection INACTIVE");
    if (!A) {
        check_eq(NP(M2, "sceNpMatching2ContextStart", ctx, 10000000), (s32)0x80550C26,
                 "ContextStart fails synchronously (NP_SIGNED_OUT)");
        sys_usleep(20000);
        check_eq(g_m2cb.calls, 0, "no context event");
        check_eq(NP(M2, "sceNpMatching2AbortContextStart", ctx), (s32)0x80550C08, "AbortContextStart: not started");
    } else {
        u64 me = sys_thread_self();
        check_eq(NP(M2, "sceNpMatching2ContextStart", ctx, 10000000), 0, "async ContextStart returns 0");
        check(g_m2cb.calls == 0, "no event inside ContextStart");
        check(m2_wait(1), "START_OVER delivered");
        check(g_m2cb.ctx == ctx && g_m2cb.event == 0x6F01 && g_m2cb.cause == 10 && g_m2cb.err == NET &&
                  g_m2cb.arg == (void *)0x1234,
              "START_OVER, CONTEXT_ERROR, the network error, the registered arg");
        check(g_m2cb.thread != me, "on the pump thread");
        check_eq(NP(M2, "sceNpMatching2ContextStart", ctx, 0), 0, "ContextStart again");
        s32 r = NP(M2, "sceNpMatching2AbortContextStart", ctx);
        bool late = r == (s32)0x80550C08; /* only if the pump already delivered it (a stalled machine) */
        check(r == 0 || late, "AbortContextStart");
        check(m2_wait(2) && g_m2cb.cause == (late ? 10 : 11) && g_m2cb.err == (late ? NET : (s32)0x80550C2A),
              "aborted start: CONTEXT_ACTION, ABORTED");
    }
    check_eq(NP(M2, "sceNpMatching2DestroyContext", ctx), 0, "DestroyContext");
    check_eq(NP(M2, "sceNpMatching2DestroyContext", ctx), (s32)0x80550C06, "DestroyContext twice");
    check_eq(NP(M2, "sceNpMatching2Terminate"), 0, "Terminate");
    check_eq(NP(M2, "sceNpMatching2Terminate"), (s32)0x80550C03, "Terminate twice");
}

/* ---- Signaling ---- */
static void test_signaling(void)
{
    s32 ctx = 0, v = 9;
    check_eq(NP(SIG, "sceNpSignalingCreateContextA", USER, 0, 0, P(&ctx)), (s32)0x80552701, "before Initialize");
    check_eq(NP(SIG, "sceNpSignalingInitialize", 0x40000, 0, 0, 0x4000), 0, "Initialize");
    check_eq(NP(SIG, "sceNpSignalingInitialize", 0x40000, 0, 0, 0x4000), (s32)0x80552702, "Initialize twice");
    check_eq(NP(SIG, "sceNpSignalingCreateContextA", P(-1), 0, 0, P(&ctx)), (s32)0x80552715, "user -1");
    check_eq(NP(SIG, "sceNpSignalingCreateContextA", USER, P(dummy_cb), 0, P(&ctx)), 0, "CreateContextA");
    check(ctx > 0, "context id");
    s32 conn = 3;
    check_eq(NP(SIG, "sceNpSignalingActivateConnectionA", ctx, P(&v), P(&conn)), (s32)0x8055270B,
             "ActivateConnectionA: NETINFO_NOT_AVAILABLE");
    check_eq(NP(SIG, "sceNpSignalingSetContextOption", ctx, 1, 5), 0, "SetContextOption");
    check(NP(SIG, "sceNpSignalingGetContextOption", ctx, 1, P(&v)) == 0 && v == 5, "GetContextOption");
    u64 ni[3] = {0x10, 0, 0};
    check_eq(NP(SIG, "sceNpSignalingGetLocalNetInfo", ctx, P(ni)), (s32)0x80552715, "GetLocalNetInfo: bad size");
    ni[0] = 0x18;
    check_eq(NP(SIG, "sceNpSignalingGetLocalNetInfo", ctx, P(ni)), (s32)0x8055270B, "GetLocalNetInfo");
    check_eq(NP(SIG, "sceNpSignalingDeleteContext", ctx), 0, "DeleteContext");
    check_eq(NP(SIG, "sceNpSignalingDeleteContext", ctx), (s32)0x80552705, "DeleteContext twice");
    check_eq(NP(SIG, "sceNpSignalingTerminate"), 0, "Terminate");
    check_eq(NP(SIG, "sceNpSignalingTerminate"), (s32)0x80552701, "Terminate twice");
}

/* ---- SessionSignaling: Ghost's boot calls (eboot 0x89b90d) ---- */
static void test_session_signaling(void)
{
    s32 off = A ? NET : (s32)0x80550006;
    struct {
        s32 http;
        u32 pad;
        u64 pool, zero;
        s32 prio;
        u32 pad2;
        u64 stack;
    } ip = {7, 0, 0x80000, 0, 700, 0, 0x8000};
    check_eq(sizeof(ip), 0x28, "Ghost's Initialize parameter is 0x28 bytes");
    check_eq(NP(SS, "sceNpSessionSignalingInitialize", P(&ip)), 0, "Initialize");
    check_eq(NP(SS, "sceNpSessionSignalingInitialize", P(&ip)), (s32)0x80553302, "Initialize twice");
    struct {
        s32 user, zero;
        struct {
            void *handler, *arg;
        } h[3];
    } cp = {USER, 0, {{(void *)dummy_cb, &cp}, {(void *)dummy_cb, &cp}, {(void *)dummy_cb, &cp}}};
    s32 ctx = 0; /* Ghost keeps it in a global (eboot 0x289F8EC) */
    check_eq(NP(SS, "sceNpSessionSignalingCreateContext", P(&cp), P(&ctx)), 0, "CreateContext");
    check_eq(ctx, 1, "context id 1");
    u8 out[0x40];
    memset(out, 0xAA, sizeof(out));
    check_eq(NP(SS, "sceNpSessionSignalingRequestPrepare", ctx, P(out)), off, "RequestPrepare: no PSN");
    check(out[0] == 0xAA && out[0x3F] == 0xAA, "RequestPrepare writes nothing");
    s32 id = 0;
    check_eq(NP(SS, "sceNpSessionSignalingActivateUser", ctx, P(out), P(&id)), off, "ActivateUser: no PSN");
    s32 st = 4;
    check_eq(NP(SS, "sceNpSessionSignalingGetConnectionStatus", ctx, 1, P(&st), 0, 0), (s32)0x8055330C,
             "GetConnectionStatus: CONN_NOT_FOUND");
    check_eq(st, 0, "status 0");
    check_eq(NP(SS, "sceNpSessionSignalingDeactivate", ctx, 1), 0, "Deactivate");
    check_eq(NP(SS, "sceNpSessionSignalingDestroyContext", ctx), 0, "DestroyContext");
}

/* ---- Party, EntitlementAccess, SNS, UDS, GameIntent ---- */
static void test_others(void)
{
    u16 pst = 0;
    check(NP(PARTY, "sceNpPartyInitialize") == 0 && NP(PARTY, "sceNpPartyInitialize") == 0, "Party Initialize twice");
    check(NP(PARTY, "sceNpPartyGetState", P(&pst)) == 0 && pst == 2, "Party GetState: NOT_IN_PARTY");
    check_eq(NP(PARTY, "sceNpPartyGetMembers", P(&pst)), (s32)0x80552506, "Party GetMembers: NOT_IN_PARTY");
    check_eq(NP("libSceNpPartyCompat", "sceNpPartyGetStateAsUser", USER, P(&pst)), (s32)0x80552506,
             "Party GetStateAsUser through libSceNpPartyCompat");

    u32 sku = 0;
    check(NP(EA, "sceNpEntitlementAccessGetSkuFlag", P(&sku)) == 0 && sku == 3, "EA GetSkuFlag is SWRR's");
    check_eq(NP(EA, "sceNpEntitlementAccessInitialize", 0), 0, "EA Initialize (SWRR's) usable");
    check_eq(NP(EA, "sceNpEntitlementAccessRequestServiceEntitlementInfo", 1, 0), A ? (s32)0x817D0013 : (s32)0x817D0014,
             "EA request: SIGNED_OUT");

    s32 dres = 0;
    check_eq(NP(SNSD, "sceNpSnsFacebookDialogOpen", P(&dres)), (s32)0x80B80003, "SNS dialog before Initialize");
    check_eq(NP(SNSD, "sceNpSnsFacebookDialogInitialize"), 0, "SNS dialog Initialize");
    check_eq(NP(SNSD, "sceNpSnsFacebookDialogGetResult", P(&dres)), (s32)0x80B80005, "result before it finished");
    check_eq(NP(SNSD, "sceNpSnsFacebookDialogOpen", P(&dres)), 0, "SNS dialog Open");
    check_eq(NP(SNSD, "sceNpSnsFacebookDialogUpdateStatus"), 3, "dialog FINISHED at once");
    check(NP(SNSD, "sceNpSnsFacebookDialogGetResult", P(&dres)) == 0 && dres == 1, "result USER_CANCELED");
    check(NP(SNSD, "sceNpSnsFacebookDialogClose") == 0 && NP(SNSD, "sceNpSnsFacebookDialogTerminate") == 0,
          "SNS dialog Close, Terminate");
    s32 sreq = NP(SNS, "sceNpSnsFacebookCreateRequest");
    check(sreq > 0, "SNS CreateRequest");
    check_eq(NP("libSceNpSns", "sceNpSnsFacebookGetAccessToken", sreq, 0, P(&dres)), A ? NET : (s32)0x80550006,
             "GetAccessToken: no PSN");

    check(NP(UDS, "sceNpUniversalDataSystemInitialize", 0) == 0 && NP(UDS, "sceNpUniversalDataSystemInitialize", 0) == 0,
          "UDS Initialize twice");
    s32 uctx = 0, uh = 0;
    check(NP(UDS, "sceNpUniversalDataSystemCreateContext", P(&uctx), USER, 0, 0) == 0 && uctx > 0,
          "UDS CreateContext");
    check(NP(UDS, "sceNpUniversalDataSystemCreateHandle", P(&uh)) == 0 && uh > 0, "UDS CreateHandle");
    check_eq(NP(UDS, "sceNpUniversalDataSystemRegisterContext", uctx, uh, 0), 0, "UDS RegisterContext");
    u8 *ev = NULL, *props = NULL;
    check(NP(UDS, "sceNpUniversalDataSystemCreateEventPropertyObject", P(&props)) == 0 && props, "property object");
    check_eq(NP(UDS, "sceNpUniversalDataSystemEventPropertyObjectSetString", P(props), P("activityId"), P("act1")), 0,
             "property set (dropped)");
    check(NP(UDS, "sceNpUniversalDataSystemCreateEvent", P("activityStart"), P(props), P(&ev)) == 0 && ev &&
              memcmp(ev, "GUDS", 4) == 0,
          "UDS CreateEvent gives a GUDS block");
    check_eq(NP(UDS, "sceNpUniversalDataSystemPostEvent", uctx, uh, P(ev), 0), 0, "PostEvent accepted and dropped");
    u64 size = 0;
    check(NP(UDS, "sceNpUniversalDataSystemEventEstimateSize", P(ev), P(&size)) == 0 && size == 64, "EstimateSize");
    check(NP(UDS, "sceNpUniversalDataSystemDestroyEvent", P(ev)) == 0 &&
              NP(UDS, "sceNpUniversalDataSystemDestroyEventPropertyObject", P(props)) == 0,
          "UDS Destroy");
    u8 *evs[70];
    bool ok = true;
    for (int i = 0; i < 70; i++)
        ok &= NP(UDS, "sceNpUniversalDataSystemCreateEvent", P("activityEnd"), 0, P(&evs[i])) == 0 && evs[i];
    check(ok && evs[0] != evs[1] && evs[68] == evs[69], "an empty pool still answers (one spare block)");
    for (int i = 0; i < 70; i++)
        NP(UDS, "sceNpUniversalDataSystemDestroyEvent", P(evs[i]));
    check(NP(UDS, "sceNpUniversalDataSystemCreateEvent", P("x"), 0, P(&ev)) == 0 && ev && ev != evs[69],
          "blocks return to the pool");

    check_eq(NP(GI, "sceNpGameIntentInitialize", 0), 0, "GameIntent Initialize: SWRR's ALREADY_INITIALIZED is fine");
    static u8 info[0x4140 + 16];
    memset(info, 0x5A, sizeof(info));
    *(u64 *)info = 0x4140;
    *(s32 *)(info + 8) = -1;
    strcpy((char *)info + 0x0C, "launchActivity");
    check_eq(NP(GI, "sceNpGameIntentReceiveIntent", P(info)), (s32)0x80553806, "ReceiveIntent: INTENT_NOT_FOUND");
    check(*(u64 *)info == 0x4140 && *(s32 *)(info + 8) == -1, "size and user id kept");
    check(info[0x0C] == 0 && info[0x134] == 0 && info[0x413F] == 0, "the intent is zero-filled");
    check(info[0x4140] == 0x5A && info[0x414F] == 0x5A, "nothing past the struct");
    *(u64 *)info = 0x100000; /* a bogus size never writes past 0x4140 */
    memset(info + 0x4140, 0x5A, 16);
    NP(GI, "sceNpGameIntentReceiveIntent", P(info));
    check(info[0x4140] == 0x5A, "a bogus size is clamped");
    char val[0x25];
    memset(val, 'x', sizeof(val));
    check_eq(NP(GI, "sceNpGameIntentGetPropertyValueString", P(info + 0x134), P("playerSessionId"), P(val),
                sizeof(val)),
             (s32)0x80553807, "GetPropertyValueString: VALUE_NOT_FOUND");
    check(val[0] == 0, "value cleared");
    check_eq(NP(GI, "sceNpGameIntentLaunchApp", 0), (s32)0x80553800, "LaunchApp refused");
}

/* ---- catch-alls: an import the families do not implement ---- */
static void test_catch_alls(void)
{
    static struct module m;
    m.name = "npo_test.prx";
    const char *libs[] = {WA,  "libSceNpScoreCompat", M2, UDS, GI, SS, "libSceNpTusCompat", PARTY, WA2,
                          SIG, SNSD,                  EA, SNS};
    for (u32 i = 0; i < ARRAY_SIZE(libs); i++)
        m.import_libs[i] = (struct libref){(u16)i, libs[i]};
    m.n_import_libs = ARRAY_SIZE(libs);
    m.needed_mods[0] = (struct libref){0, "libkernel"};
    m.n_needed_mods = 1;
    /* "NID#lib#module", lib ids as base64 digits: A=0 B=1 ... */
    /* want: persona B's answer, want_a persona A's (the network error for
     * the libraries whose catch-all is their OFF code) */
    static const struct {
        const char *sym, *what;
        s32 want, want_a;
    } k[] = {
        {"N2Jbx4tIaQ4#A#A", "WebApi IntCreateRequest", (s32)0x80552907, NET},
        {"qW9M0bQ-Zx0#B#A", "Score CreateTitleCtx through Compat", (s32)0x80550705, NET},
        {"AAAAAAAAAAA#C#A", "Matching2, a NID with no name", (s32)0x80550C08, (s32)0x80550C08},
        {"AAAAAAAAAAA#D#A", "UDS, a NID with no name", 0, 0},
        {"KNggmcdqc2Q#E#A", "GameIntent GetUdsNpComId", (s32)0x80553800, (s32)0x80553800},
        {"aBuX0PX-T7I#F#A", "SessionSignaling CreateContext2", (s32)0x80550006, NET},
        {"AAAAAAAAAAA#H#A", "Party", (s32)0x80552506, (s32)0x80552506},
        {"81DkaQt6J30#I#A", "WebApi2 IntPushEventCreateDestPs4CtxIndFilter", (s32)0x80553407, NET},
        {"AAAAAAAAAAA#J#A", "Signaling", (s32)0x8055270B, (s32)0x8055270B},
        {"AAAAAAAAAAA#K#A", "SNS dialog", (s32)0x80B80003, (s32)0x80B80003},
        {"AAAAAAAAAAA#L#A", "EntitlementAccess", (s32)0x817D0014, (s32)0x817D0013},
        {"AAAAAAAAAAA#M#A", "SNS Facebook", (s32)0x80550006, NET},
    };
    u64 stub[ARRAY_SIZE(k)];
    for (u32 i = 0; i < ARRAY_SIZE(k); i++)
        stub[i] = resolve_import(&m, k[i].sym, false, true);
    char nid[12], sym[32];
    compute_nid("sceNpTusCreateRequest", nid);
    fmt(sym, sizeof(sym), "%s#G#A", nid);
    check(resolve_import(&m, sym, false, true) == (u64)lookup(TUS, "sceNpTusCreateRequest"),
          "Tus import through libSceNpTusCompat binds to the extension");
    compute_nid("sceNpMatching2CreateContextInternal", nid);
    fmt(sym, sizeof(sym), "%s#C#A", nid);
    u64 internal = resolve_import(&m, sym, false, true);
    stubs_flush();
    for (u32 i = 0; i < ARRAY_SIZE(k); i++) {
        char what[96];
        fmt(what, sizeof(what), "catch-all: %s", k[i].what);
        check_eq(stub[i] ? (s32)((s64(*)(void))stub[i])() : (s32)0xBAD, A ? k[i].want_a : k[i].want, what);
    }
    check_eq((s32)((fn6)internal)(0, 0, 0, 0, 0, 0), (s32)0x80550C0A, "Matching2 CreateContextInternal");
    /* sceKernelDlsym is the extension's (the module family) */
    void *dlsym_fn = (void *)lookup("libkernel", "sceKernelDlsym");
    s32 (*dlsym)(s32, const char *, void **) = dlsym_fn;
    void *out = NULL;
    check(dlsym && dlsym(0x55, "sceNpWebApi2Initialize", &out) == 0 &&
              out == (void *)lookup(WA2, "sceNpWebApi2Initialize"),
          "dlsym finds the extension's WebApi2 Initialize");
    check(dlsym && dlsym(0x99, "sceNpTusNotAFunction", &out) == (s32)0x80020002 && !out,
          "dlsym refuses the rest of libSceNpTus");
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        logf("usage: np_online EXT_BIN CRC b|a");
        return 2;
    }
    A = argv[3][0] == 'a';
    char *env_argv[] = {"np_online", "small", argv[1], argv[2], NULL};
    check(ext_host_env_main(4, env_argv) == 0, "the core loads the extension (env.c's small mode)");
    K.usleep = k_usleep;
    K.module_info = k_module_info_np;
    const struct ext_lib *pol = ext_lib_policy("libSceNpWebApi2AsyncRestricted");
    check(pol && (pol->flags & EXT_LIB_NO_HOST) && strcmp(pol->name, WA2) == 0, "WebApi2 alias policy");
    test_webapi2();
    test_webapi();
    test_score_tus();
    test_matching2();
    test_signaling();
    test_session_signaling();
    test_others();
    test_catch_alls();
    logf("npo_test: %s (persona %s)", g_fail ? "FAILED" : "all passed", A ? "A, Matching2 async" : "B");
    return g_fail;
}
