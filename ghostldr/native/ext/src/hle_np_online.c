/*
 * NP online services, virtual (np-online spec §3.8 to §3.12): libSceNpParty,
 * libSceNpEntitlementAccess, SNS Facebook (requests and dialog),
 * libSceNpUniversalDataSystem and libSceNpGameIntent.  The other NP online
 * libraries are in hle_np_online_*.c; shared rules in np_online.h.
 *
 * - Party: never in a party; handlers are accepted and never called.
 * - EntitlementAccess: the local getters (SKU, add-on entitlements) are
 *   SWRR's, a full-SKU title with no add-ons, which is the answer we want;
 *   every PSN request fails with SIGNED_OUT (or NETWORK under persona A).
 * - SNS Facebook: the dialog finishes at once, cancelled; token requests fail.
 * - UDS (activities, telemetry): everything succeeds and events are
 *   dropped, as the real system queues them offline.  Objects are blocks of
 *   a small pool marked "GUDS"; any other pointer is accepted and ignored.
 * - GameIntent: Initialize goes to SWRR's library (already initialized
 *   there is fine); no intent is ever pending; never launch another app.
 */
#include "np_online.h"

NPO_SMALL_BEGIN

static volatile int g_lock;

static s32 ret0(void) { return 0; }

/* ======================= libSceNpParty ======================= */

#define PARTY_INVALID_ARGUMENT ((s32)0x80552504)
#define PARTY_NOT_IN_PARTY ((s32)0x80552506)
#define PARTY_STATE_NOT_IN_PARTY 2

static s32 party_GetState(u16 *state)
{
    if (!state)
        return PARTY_INVALID_ARGUMENT;
    *state = PARTY_STATE_NOT_IN_PARTY;
    NPO_LOG_FIRST(2, "npo: sceNpPartyGetState -> NOT_IN_PARTY");
    return 0;
}

/* GetStateAsUser[A]: the argument order is unconfirmed, so no write yet;
 * members, ids, voice, messages, invitations: there is no party */
static s32 party_not_in_party(void) { return PARTY_NOT_IN_PARTY; }

/* =================== libSceNpEntitlementAccess =================== */

#define EA_NOT_FOUND ((s32)0x817D0005)
#define EA_OFF NPO_OFF(0x817D0014 /* SIGNED_OUT */, 0x817D0013 /* NETWORK */)

/* SWRR's own answer for a local getter (signatures unconfirmed: all six
 * argument registers go through) */
static s32 ea_pass(const char *name, npo_fn6 *real, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)
{
    if (!*real)
        *real = (npo_fn6)host_real_in("libSceNpEntitlementAccess", name);
    s32 r = *real ? (s32)(*real)(a, b, c, d, e, f) : EA_NOT_FOUND;
    NPO_TRACE(r, "npo: %s (SWRR's) -> %x", name, r);
    return r;
}

#define EA_PASS(n)                                                                                                 \
    static s32 ea_##n(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)                                                    \
    {                                                                                                              \
        static npo_fn6 real;                                                                                       \
        return ea_pass("sceNpEntitlementAccess" #n, &real, a, b, c, d, e, f);                                      \
    }
EA_PASS(GetSkuFlag)
EA_PASS(GetGameTrialsFlag)
EA_PASS(GetAddcontEntitlementInfo)
EA_PASS(GetAddcontEntitlementInfoList)
EA_PASS(GetAddcontEntitlementInfoIndividual)
EA_PASS(GetEntitlementKey)

EA_PASS(Initialize)
static s32 ea_InitializeOk(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)
{
    s32 r = ea_Initialize(a, b, c, d, e, f);
    return r < 0 ? 0 : r; /* SWRR may have initialized it already: usable either way */
}

/* Request*, Poll*, GenerateTransactionId, Abort/DeleteRequest: no PSN */
static s32 ea_off(void)
{
    NPO_TRACE(EA_OFF, "npo: sceNpEntitlementAccess request -> %x", (u32)EA_OFF);
    return EA_OFF;
}

/* ========================= SNS Facebook ========================= */

#define CDLG_NOT_INITIALIZED ((s32)0x80B80003)
#define CDLG_NOT_FINISHED ((s32)0x80B80005)
#define CDLG_ARG_NULL ((s32)0x80B8000D)
enum { DLG_NONE, DLG_INITIALIZED, DLG_RUNNING, DLG_FINISHED };
#define SNS_OFF NPO_OFF(NPO_NP_SIGNED_OUT, NPO_ERR_NET)

static struct {
    s32 status;
    s32 req_seq;
} g_sns;

static s32 sns_DialogInitialize(void)
{
    g_sns.status = DLG_INITIALIZED;
    return 0;
}
static s32 sns_DialogTerminate(void)
{
    g_sns.status = DLG_NONE;
    return 0;
}
static s32 sns_DialogOpen(const void *param)
{
    s32 r = !param ? CDLG_ARG_NULL : g_sns.status == DLG_NONE ? CDLG_NOT_INITIALIZED : 0;
    if (r == 0)
        g_sns.status = DLG_FINISHED; /* the user cancelled at once */
    NPO_TRACE(r, "npo: sceNpSnsFacebookDialogOpen -> %x", r);
    return r;
}
static s32 sns_DialogStatus(void) { return g_sns.status; }
static s32 sns_DialogGetResult(s32 *result)
{
    if (!result)
        return CDLG_ARG_NULL;
    if (g_sns.status != DLG_FINISHED)
        return CDLG_NOT_FINISHED;
    *result = 1; /* USER_CANCELED; the rest of the result layout is unconfirmed */
    return 0;
}
static s32 sns_DialogClose(void)
{
    if (g_sns.status != DLG_NONE)
        g_sns.status = DLG_INITIALIZED;
    return 0;
}
static s32 sns_CreateRequest(void)
{
    spin_lock(&g_lock);
    s32 id = npo_next(&g_sns.req_seq, 0x7FFFFFFF);
    spin_unlock(&g_lock);
    return id;
}
static s32 sns_GetAccessToken(s32 req, const void *param, void *result)
{
    NPO_TRACE(SNS_OFF, "npo: sceNpSnsFacebookGetAccessToken(%d) -> %x", req, (u32)SNS_OFF);
    return SNS_OFF;
}

/* ================ libSceNpUniversalDataSystem (UDS) ================ */

#define UDS_POOL 64
#define UDS_MAGIC 0x53445547u /* "GUDS" */
#define UDS_NAME 48

struct uds_obj { /* 64 bytes; titles only pass the pointer back */
    u32 magic;
    u32 kind; /* 1 event, 2 property object, 3 property array */
    struct uds_obj *next_free;
    char name[UDS_NAME];
};

static struct uds_obj *g_uds_pool, *g_uds_free;
static struct uds_obj g_uds_spare = {UDS_MAGIC, 0, NULL, "?"}; /* handed out when the pool is empty */
static s32 g_uds_ctx_seq, g_uds_handle_seq;

static bool uds_is_pool(const struct uds_obj *o)
{
    return g_uds_pool && o >= g_uds_pool && o < g_uds_pool + UDS_POOL &&
           ((u64)o - (u64)g_uds_pool) % sizeof(*o) == 0;
}

/* a pool block (pool made on first use: most titles never touch UDS) */
static struct uds_obj *uds_alloc(u32 kind, const char *name)
{
    spin_lock(&g_lock);
    if (!g_uds_pool && (g_uds_pool = arena_alloc(UDS_POOL * sizeof(struct uds_obj)))) {
        for (u32 i = 0; i < UDS_POOL; i++)
            g_uds_pool[i].next_free = i + 1 < UDS_POOL ? &g_uds_pool[i + 1] : NULL;
        g_uds_free = g_uds_pool;
    }
    struct uds_obj *o = g_uds_free;
    if (o) {
        g_uds_free = o->next_free;
        memset(o, 0, sizeof(*o));
        o->magic = UDS_MAGIC, o->kind = kind;
        strlcpy(o->name, name ? name : "", UDS_NAME);
    } else
        o = &g_uds_spare;
    spin_unlock(&g_lock);
    return o;
}

static s32 uds_free(struct uds_obj *o)
{
    spin_lock(&g_lock);
    if (uds_is_pool(o) && o->magic == UDS_MAGIC) {
        o->magic = 0;
        o->next_free = g_uds_free;
        g_uds_free = o;
    }
    spin_unlock(&g_lock);
    return 0;
}

static s32 uds_Initialize(const u64 *p)
{
    NPO_LOG_FIRST(2, "npo: sceNpUniversalDataSystemInitialize(size %lx, pool %lx) -> 0", p ? p[0] : 0,
                  p ? p[1] : 0);
    return 0;
}

/* CreateContext(s32 *ctx, user, label, opt) and CreateHandle(s32 *handle):
 * the out-param positions are the SDK pattern, not yet seen in a title */
static s32 uds_CreateContext(s32 *ctx, s32 user, u32 label, u64 opt)
{
    spin_lock(&g_lock);
    s32 id = npo_next(&g_uds_ctx_seq, 0x7FFFFFFF);
    spin_unlock(&g_lock);
    if (ctx)
        *ctx = id;
    NPO_LOG_FIRST(4, "npo: sceNpUniversalDataSystemCreateContext(user %x, label %x) -> 0 ctx %d", user, label, id);
    return 0;
}
static s32 uds_CreateHandle(s32 *handle)
{
    spin_lock(&g_lock);
    s32 id = npo_next(&g_uds_handle_seq, 0x7FFFFFFF);
    spin_unlock(&g_lock);
    if (handle)
        *handle = id;
    return 0;
}

/* CreateEvent(const char *name, props, Event **out, ...), CreateRecordData(same) */
static s32 uds_CreateEvent(const char *name, const void *props, struct uds_obj **out)
{
    if (out)
        *out = uds_alloc(1, name);
    return 0;
}
static s32 uds_CreatePropertyObject(struct uds_obj **out)
{
    if (out)
        *out = uds_alloc(2, NULL);
    return 0;
}
static s32 uds_CreatePropertyArray(struct uds_obj **out)
{
    if (out)
        *out = uds_alloc(3, NULL);
    return 0;
}
static s32 uds_Destroy(struct uds_obj *o) { return uds_free(o); }

/* PostEvent(ctx, handle, event, opt): dropped.  The activity events are
 * logged once each, everything else rate-limited. */
static s32 uds_PostEvent(s32 ctx, s32 handle, const struct uds_obj *ev, u64 opt)
{
    static u32 logged, activity;
    const char *name = ev && (uds_is_pool(ev) || ev == &g_uds_spare) && ev->magic == UDS_MAGIC ? ev->name : "?";
    u32 bit = strcmp(name, "activityStart") == 0       ? 1
              : strcmp(name, "activityEnd") == 0       ? 2
              : strcmp(name, "activityTerminate") == 0 ? 4
                                                       : 0;
    bool log = bit ? !(__atomic_fetch_or(&activity, bit, __ATOMIC_RELAXED) & bit)
                   : __atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 32;
    if (log)
        logf("npo: sceNpUniversalDataSystemPostEvent(ctx %d, handle %d, %s) -> 0 (dropped)", ctx, handle, name);
    return 0;
}

static s32 uds_EventEstimateSize(const void *ev, u64 *size)
{
    if (size)
        *size = 64;
    return 0;
}

/* ===================== libSceNpGameIntent ===================== */

#define GI_UNKNOWN ((s32)0x80553800)
#define GI_INVALID_ARGUMENT ((s32)0x80553804)
#define GI_INTENT_NOT_FOUND ((s32)0x80553806)
#define GI_VALUE_NOT_FOUND ((s32)0x80553807)
#define GI_INFO_SIZE 0x4140 /* SceNpGameIntentInfo as Ghost allocates it */

static s32 gi_Initialize(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)
{
    /* SWRR's library: ALREADY_INITIALIZED (or any failure) means usable */
    static npo_fn6 real;
    if (!real)
        real = (npo_fn6)host_real_in("libSceNpGameIntent", "sceNpGameIntentInitialize");
    s32 r = real ? (s32)real(a, b, c, d, e, f) : -1;
    NPO_TRACE(r, "npo: sceNpGameIntentInitialize -> %x%s", r, r < 0 ? ", treating as initialized" : "");
    return r < 0 ? 0 : r;
}

/* ReceiveIntent(SceNpGameIntentInfo *info): {u64 size; s32 userId @8;
 * intentType @0xC; data @0x134}.  No intent: everything after the user id
 * is cleared, so intentType is "" whatever the title left there. */
static s32 gi_ReceiveIntent(u8 *info)
{
    if (!info)
        return GI_INVALID_ARGUMENT;
    u64 size = *(const u64 *)info;
    if (size > GI_INFO_SIZE || size < 0x0C)
        size = GI_INFO_SIZE;
    memset(info + 0x0C, 0, size - 0x0C);
    NPO_LOG_FIRST(4, "npo: sceNpGameIntentReceiveIntent(size %lx, user %x) -> INTENT_NOT_FOUND", *(const u64 *)info,
                  *(const s32 *)(info + 8));
    return GI_INTENT_NOT_FOUND;
}

static s32 gi_GetPropertyValueString(const void *data, const char *key, char *buf, u64 size)
{
    if (buf && size)
        buf[0] = 0;
    NPO_LOG_FIRST(4, "npo: sceNpGameIntentGetPropertyValueString(%s) -> VALUE_NOT_FOUND", key ? key : "-");
    return GI_VALUE_NOT_FOUND;
}

static s32 gi_LaunchApp(void)
{
    NPO_LOG_FIRST(4, "npo: sceNpGameIntentLaunchApp refused: no other app is started from inside SWRR");
    return GI_UNKNOWN;
}

NPO_SMALL_END

#define P(name, fn) {"libSceNpParty", "sceNpParty" #name, fn, NULL}
#define EA(name, fn) {"libSceNpEntitlementAccess", "sceNpEntitlementAccess" #name, fn, NULL}
#define SNS(name, fn) {"libSceNpSnsFacebook", "sceNpSnsFacebook" #name, fn, NULL}
#define SNSD(name, fn) {"libSceNpSnsFacebookDialog", "sceNpSnsFacebookDialog" #name, fn, NULL}
#define U(name, fn) {"libSceNpUniversalDataSystem", "sceNpUniversalDataSystem" #name, fn, NULL}
#define GI(name, fn) {"libSceNpGameIntent", "sceNpGameIntent" #name, fn, NULL}

#define PARTY_NONE(X)                                                                                              \
    X(Create) X(CreateA) X(GetId) X(GetMemberInfo) X(GetMemberInfoA) X(GetMemberSessionInfo)                       \
    X(GetMemberVoiceInfo) X(GetMembers) X(GetMembersA) X(GetVoiceChatPriority) X(Join) X(Leave)                   \
    X(SendBinaryMessage) X(SetVoiceChatPriority) X(ShowInvitationList) X(ShowInvitationListA)                      \
    X(GetStateAsUser) X(GetStateAsUserA)
#define PARTY_NONE_ENT(n) P(n, party_not_in_party),

#define EA_REQUESTS(X)                                                                                             \
    X(AbortRequest) X(DeleteRequest) X(GenerateTransactionId) X(PollConsumableEntitlementInfo)                     \
    X(PollConsumeEntitlement) X(PollServiceEntitlementInfo) X(PollServiceEntitlementInfoList)                      \
    X(PollUnifiedEntitlementInfo) X(PollUnifiedEntitlementInfoList) X(RequestConsumableEntitlementInfo)            \
    X(RequestConsumeEntitlement) X(RequestConsumeServiceEntitlement) X(RequestConsumeUnifiedEntitlement)           \
    X(RequestServiceEntitlementInfo) X(RequestServiceEntitlementInfoList) X(RequestUnifiedEntitlementInfo)         \
    X(RequestUnifiedEntitlementInfoList)
#define EA_REQ_ENT(n) EA(n, ea_off),

#define UDS_SETTERS(X)                                                                                             \
    X(Array) X(Binary) X(Bool) X(Float32) X(Float64) X(Int32) X(Int64) X(Object) X(String) X(UInt32) X(UInt64)
#define UDS_SET_ENT(n) U(EventPropertyObjectSet##n, ret0), U(EventPropertyArraySet##n, ret0),

static const struct ext_hle_ent k_hle[] = {
    P(Initialize, ret0), /* lenient on a second call: SWRR may own the real instance */
    P(Terminate, ret0),
    P(CheckCallback, ret0),
    P(RegisterHandler, ret0),
    P(RegisterHandlerA, ret0),
    P(RegisterPrivateHandler, ret0),
    P(UnregisterPrivateHandler, ret0),
    P(GetState, party_GetState),
    PARTY_NONE(PARTY_NONE_ENT)

    EA(Initialize, ea_InitializeOk),
    EA(GetSkuFlag, ea_GetSkuFlag),
    EA(GetGameTrialsFlag, ea_GetGameTrialsFlag),
    EA(GetAddcontEntitlementInfo, ea_GetAddcontEntitlementInfo),
    EA(GetAddcontEntitlementInfoList, ea_GetAddcontEntitlementInfoList),
    EA(GetAddcontEntitlementInfoIndividual, ea_GetAddcontEntitlementInfoIndividual),
    EA(GetEntitlementKey, ea_GetEntitlementKey),
    EA_REQUESTS(EA_REQ_ENT)

    SNSD(Initialize, sns_DialogInitialize),
    SNSD(Terminate, sns_DialogTerminate),
    SNSD(Open, sns_DialogOpen),
    SNSD(UpdateStatus, sns_DialogStatus),
    SNSD(GetStatus, sns_DialogStatus),
    SNSD(GetResult, sns_DialogGetResult),
    SNSD(Close, sns_DialogClose),
    SNS(CreateRequest, sns_CreateRequest),
    SNS(DeleteRequest, ret0),
    SNS(AbortRequest, ret0),
    SNS(GetAccessToken, sns_GetAccessToken),

    U(Initialize, uds_Initialize),
    U(Terminate, ret0),
    U(CreateContext, uds_CreateContext),
    U(RegisterContext, ret0),
    U(DestroyContext, ret0),
    U(CreateHandle, uds_CreateHandle),
    U(DestroyHandle, ret0),
    U(AbortHandle, ret0),
    U(CreateEvent, uds_CreateEvent),
    U(CreateEventPropertyObject, uds_CreatePropertyObject),
    U(CreateEventPropertyArray, uds_CreatePropertyArray),
    UDS_SETTERS(UDS_SET_ENT)
    U(DestroyEvent, uds_Destroy),
    U(DestroyEventPropertyObject, uds_Destroy),
    U(DestroyEventPropertyArray, uds_Destroy),
    U(PostEvent, uds_PostEvent),
    U(EventEstimateSize, uds_EventEstimateSize),
    U(EventToString, ret0),
    U(GetMemoryStat, ret0),
    U(GetStorageStat, ret0),
    U(CreateRecordObject, uds_CreatePropertyObject),
    U(CreateRecordArray, uds_CreatePropertyArray),
    U(CreateRecordData, uds_CreateEvent),
    U(DestroyRecordObject, uds_Destroy),
    U(DestroyRecordArray, uds_Destroy),
    U(DestroyRecordData, uds_Destroy),
    U(PostRecord, ret0),
    U(PostRecordData, ret0),

    GI(Initialize, gi_Initialize),
    GI(Terminate, ret0), /* never tear down SWRR's instance */
    GI(ReceiveIntent, gi_ReceiveIntent),
    GI(GetPropertyValueString, gi_GetPropertyValueString),
    GI(NotifyAppLaunched, ret0),
    GI(LaunchApp, gi_LaunchApp),
    GI(LaunchApp2, gi_LaunchApp),
};

/* Unlisted names: Party NOT_IN_PARTY, EntitlementAccess and SNS requests
 * the OFF code, the SNS dialog NOT_INITIALIZED, UDS 0 (telemetry must never
 * break a title), GameIntent UNKNOWN (GetUdsNpComId and newer calls). */
static const struct ext_lib k_libs[] = {
    {"libSceNpParty", "libSceNpPartyCompat", EXT_LIB_NO_HOST, PARTY_NOT_IN_PARTY},
    {"libSceNpEntitlementAccess", NULL, EXT_LIB_NO_HOST, EA_OFF},
    {"libSceNpSnsFacebook", "libSceNpSns", EXT_LIB_NO_HOST, SNS_OFF},
    {"libSceNpSnsFacebookDialog", NULL, EXT_LIB_NO_HOST, CDLG_NOT_INITIALIZED},
    {"libSceNpUniversalDataSystem", NULL, EXT_LIB_NO_HOST, 0},
    {"libSceNpGameIntent", NULL, EXT_LIB_NO_HOST, GI_UNKNOWN},
};

EXT_FAMILY_LIBS("np-online", k_hle, k_libs, NULL);
