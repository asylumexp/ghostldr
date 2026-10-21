/*
 * libSceNpMatching2, libSceNpSignaling and libSceNpSessionSignaling, virtual
 * (np-online spec §3.5 to §3.7).
 *
 * Contexts are local and always created.  Starting one (Matching2
 * ContextStart), activating a connection or preparing a session fails at
 * once, so no title waits for an event: the real libraries deliver those on
 * their own threads, which have no Ghost TLS.  No callback is ever called,
 * except in the NPO_M2_ASYNC build: ContextStart then returns 0 and a pump
 * thread made with ghost_thread_create (title TLS) delivers START_OVER a
 * few milliseconds later, for titles that ignore ContextStart's result.
 *
 * Ghost's eboot runs SessionSignaling Initialize, CreateContext and
 * RequestPrepare unconditionally at boot (0x89b90d).  The parameter layouts
 * below are from those call sites.
 */
#include "np_online.h"

NPO_SMALL_BEGIN

static volatile int g_lock;

static s32 ret0(void) { return 0; }

/* ======================= libSceNpMatching2 ======================= */

#define M2_ALREADY_INITIALIZED ((s32)0x80550C02)
#define M2_NOT_INITIALIZED ((s32)0x80550C03)
#define M2_CONTEXT_MAX ((s32)0x80550C04)
#define M2_CONTEXT_NOT_FOUND ((s32)0x80550C06)
#define M2_CONTEXT_NOT_STARTED ((s32)0x80550C08)
#define M2_INVALID_ARGUMENT ((s32)0x80550C0A)
#define M2_INVALID_CONTEXT_ID ((s32)0x80550C0B)
#define M2_NP_SIGNED_OUT ((s32)0x80550C26)
#define M2_ABORTED ((s32)0x80550C2A)
#define M2SIG_REQ_NOT_FOUND ((s32)0x80550E07)
#define M2SIG_NETINFO_NOT_AVAILABLE ((s32)0x80550E0B)
#define M2SIG_CONN_NOT_FOUND ((s32)0x80550E0E)
#define M2SIG_RESULT_NOT_FOUND ((s32)0x80550E13)
#define M2_OFF NPO_OFF(M2_NP_SIGNED_OUT, NPO_ERR_NET)

#define M2_EVENT_START_OVER 0x6F01
#define M2_CAUSE_CONTEXT_ACTION 11
#define M2_CAUSE (NPO_PERSONA_A ? 10 /* CONTEXT_ERROR */ : 8 /* NP_SIGNED_OUT */)
#define M2_CTX 8

typedef void (*m2_ctx_cb)(u16 ctx, u16 event, u8 cause, s32 err, void *arg);

static struct {
    bool init;
    bool used[M2_CTX]; /* context id = slot + 1 */
    m2_ctx_cb cb;
    void *cb_arg;
#if NPO_M2_ASYNC
    struct m2_event {
        u16 ctx, event;
        u8 cause, ticks; /* passes of the pump left before delivery */
        s32 err;
    } q[16];
    u32 nq;
    bool pump;
#endif
} g_m2;

static bool m2_ctx_ok(u16 ctx) { return ctx >= 1 && ctx <= M2_CTX && g_m2.used[ctx - 1]; }

#if NPO_M2_ASYNC
static void m2_cancel(u16 ctx)
{
    u32 k = 0;
    for (u32 i = 0; i < g_m2.nq; i++)
        if (g_m2.q[i].ctx != ctx)
            g_m2.q[k++] = g_m2.q[i];
    g_m2.nq = k;
}

/* Delivers queued context events on a thread with title TLS, never inside
 * the title's own call and never with the lock held. */
static void *m2_pump(void *unused)
{
    for (;;) {
        K.usleep(2000);
        struct m2_event ev;
        m2_ctx_cb cb = NULL;
        void *arg = NULL;
        spin_lock(&g_lock);
        for (u32 i = 0; i < g_m2.nq; i++)
            if (g_m2.q[i].ticks)
                g_m2.q[i].ticks--;
        if (g_m2.nq && !g_m2.q[0].ticks) {
            ev = g_m2.q[0];
            for (u32 i = 1; i < g_m2.nq; i++)
                g_m2.q[i - 1] = g_m2.q[i];
            g_m2.nq--;
            cb = m2_ctx_ok(ev.ctx) ? g_m2.cb : NULL;
            arg = g_m2.cb_arg;
        }
        spin_unlock(&g_lock);
        if (cb) {
            logf("npo: Matching2 context %u event %x cause %u error %x -> callback %p", ev.ctx, ev.event, ev.cause,
                 (u32)ev.err, (void *)cb);
            cb(ev.ctx, ev.event, ev.cause, ev.err, arg);
        }
    }
    return NULL;
}

/* lock held */
static s32 m2_queue_start_over(u16 ctx)
{
    if (g_m2.nq >= 16)
        return M2_OFF;
    /* ticks 1: at least one 2 ms pass after ContextStart has returned */
    g_m2.q[g_m2.nq++] = (struct m2_event){ctx, M2_EVENT_START_OVER, M2_CAUSE, 1, M2_OFF};
    if (!g_m2.pump) {
        u64 tid;
        g_m2.pump = ghost_thread_create(&tid, NULL, m2_pump, NULL, "npo-m2-pump") == 0;
        if (!g_m2.pump) {
            g_m2.nq--;
            return M2_OFF;
        }
    }
    return 0;
}
#else
static void m2_cancel(u16 ctx) {}
#endif

static s32 m2_Initialize(const u8 *p)
{
    u64 size = p ? *(const u64 *)(p + 0x20) : 0;
    spin_lock(&g_lock);
    s32 r = g_m2.init ? M2_ALREADY_INITIALIZED : p && size != 0x28 && size != 0x30 ? M2_INVALID_ARGUMENT : 0;
    if (r == 0)
        g_m2.init = true;
    spin_unlock(&g_lock);
    NPO_TRACE(r, "npo: sceNpMatching2Initialize(size %lx) -> %x", size, r);
    return r;
}

static s32 m2_Terminate(void)
{
    spin_lock(&g_lock);
    s32 r = g_m2.init ? 0 : M2_NOT_INITIALIZED;
    memset(g_m2.used, 0, sizeof(g_m2.used));
    g_m2.cb = NULL;
#if NPO_M2_ASYNC
    g_m2.nq = 0;
#endif
    g_m2.init = false;
    spin_unlock(&g_lock);
    NPO_TRACE(r, "npo: sceNpMatching2Terminate -> %x", r);
    return r;
}

/* CreateContext[A](param, u16 *ctx): the size field is at +8 (A, 0x10) or +0x20 (0x28) */
static s32 m2_create(const u8 *p, u16 *ctx, u32 size_at, u64 size)
{
    spin_lock(&g_lock);
    s32 r = !g_m2.init ? M2_NOT_INITIALIZED
            : !p || !ctx || *(const u64 *)(p + size_at) != size ? M2_INVALID_ARGUMENT
                                                                : M2_CONTEXT_MAX;
    for (u32 i = 0; i < M2_CTX && r == M2_CONTEXT_MAX; i++)
        if (!g_m2.used[i]) {
            g_m2.used[i] = true;
            *ctx = (u16)(i + 1);
            r = 0;
        }
    spin_unlock(&g_lock);
    NPO_TRACE(r, "npo: sceNpMatching2CreateContext -> %x ctx %u", r, r ? 0 : *ctx);
    return r;
}
static s32 m2_CreateContextA(const u8 *p, u16 *ctx) { return m2_create(p, ctx, 8, 0x10); }
static s32 m2_CreateContext(const u8 *p, u16 *ctx) { return m2_create(p, ctx, 0x20, 0x28); }

static s32 m2_DestroyContext(u16 ctx)
{
    spin_lock(&g_lock);
    s32 r = m2_ctx_ok(ctx) ? 0 : M2_CONTEXT_NOT_FOUND;
    if (r == 0) {
        g_m2.used[ctx - 1] = false;
        m2_cancel(ctx);
    }
    spin_unlock(&g_lock);
    NPO_TRACE(r, "npo: sceNpMatching2DestroyContext(%u) -> %x", ctx, r);
    return r;
}

static s32 m2_RegisterContextCallback(m2_ctx_cb cb, void *arg)
{
    spin_lock(&g_lock);
    s32 r = g_m2.init ? 0 : M2_NOT_INITIALIZED;
    if (r == 0)
        g_m2.cb = cb, g_m2.cb_arg = arg;
    spin_unlock(&g_lock);
    NPO_TRACE(r, "npo: sceNpMatching2RegisterContextCallback(%p) -> %x", (void *)cb, r);
    return r;
}

/* the per-context callbacks (room, lobby, signaling, ...): accepted, never called */
static s32 m2_RegisterCtxCallback(u16 ctx, const void *cb, void *arg)
{
    return m2_ctx_ok(ctx) ? 0 : M2_INVALID_CONTEXT_ID;
}

static s32 m2_SetDefaultRequestOptParam(u16 ctx, const void *opt)
{
    return !opt ? M2_INVALID_ARGUMENT : m2_ctx_ok(ctx) ? 0 : M2_INVALID_CONTEXT_ID;
}

static s32 m2_ContextStart(u16 ctx, u64 timeout)
{
    spin_lock(&g_lock);
    s32 r = !m2_ctx_ok(ctx) ? M2_INVALID_CONTEXT_ID : M2_OFF;
#if NPO_M2_ASYNC
    if (r == M2_OFF)
        r = m2_queue_start_over(ctx);
#endif
    spin_unlock(&g_lock);
    NPO_TRACE(r, "npo: sceNpMatching2ContextStart(%u, %lu us) -> %x%s", ctx, timeout, r,
              NPO_M2_ASYNC && !r ? " (START_OVER follows)" : "");
    return r;
}

static s32 m2_AbortContextStart(u16 ctx)
{
    s32 r = M2_CONTEXT_NOT_STARTED;
#if NPO_M2_ASYNC
    spin_lock(&g_lock);
    for (u32 i = 0; i < g_m2.nq; i++)
        if (g_m2.q[i].ctx == ctx && g_m2.q[i].event == M2_EVENT_START_OVER) {
            g_m2.q[i].cause = M2_CAUSE_CONTEXT_ACTION;
            g_m2.q[i].err = M2_ABORTED;
            r = 0;
        }
    spin_unlock(&g_lock);
#endif
    NPO_TRACE(r, "npo: sceNpMatching2AbortContextStart(%u) -> %x", ctx, r);
    return r;
}

/* room, lobby, user and ping requests (u16 ctx, req, opt, u32 *reqId): no
 * context is ever started, so no request id and no request callback */
static s32 m2_request(u16 ctx)
{
    s32 r = !g_m2.init ? M2_NOT_INITIALIZED : !m2_ctx_ok(ctx) ? M2_INVALID_CONTEXT_ID : M2_CONTEXT_NOT_STARTED;
    NPO_TRACE(r, "npo: sceNpMatching2 request on context %u -> %x", ctx, r);
    return r;
}

static s32 m2_not_started(void) { return M2_CONTEXT_NOT_STARTED; }
static s32 m2_invalid_argument(void) { return M2_INVALID_ARGUMENT; }

/* GetMemoryInfo / GetSslMemoryInfo: {total, current, max}; the full size is unconfirmed */
static s32 m2_GetMemoryInfo(u64 *m)
{
    if (!g_m2.init)
        return M2_NOT_INITIALIZED;
    if (!m)
        return M2_INVALID_ARGUMENT;
    m[0] = m[1] = m[2] = 0;
    return 0;
}

static s32 m2_SignalingGetConnectionStatus(u16 ctx, u64 room, u16 member, s32 *status, u32 *addr, u16 *port)
{
    if (status)
        *status = 0; /* INACTIVE */
    return M2SIG_CONN_NOT_FOUND;
}
static s32 m2_sig_conn_not_found(void) { return M2SIG_CONN_NOT_FOUND; }
static s32 m2_sig_netinfo(void) { return M2SIG_NETINFO_NOT_AVAILABLE; }
static s32 m2_sig_req_not_found(void) { return M2SIG_REQ_NOT_FOUND; }
static s32 m2_sig_result_not_found(void) { return M2SIG_RESULT_NOT_FOUND; }

/* ======================= libSceNpSignaling ======================= */

#define SIG_NOT_INITIALIZED ((s32)0x80552701)
#define SIG_ALREADY_INITIALIZED ((s32)0x80552702)
#define SIG_CTX_NOT_FOUND ((s32)0x80552705)
#define SIG_REQ_NOT_FOUND ((s32)0x80552707)
#define SIG_NETINFO_NOT_AVAILABLE ((s32)0x8055270B)
#define SIG_CONN_NOT_FOUND ((s32)0x8055270E)
#define SIG_CTX_MAX ((s32)0x80552712)
#define SIG_RESULT_NOT_FOUND ((s32)0x80552713)
#define SIG_INVALID_ARGUMENT ((s32)0x80552715)
#define SIG_CTX 8
#define SIG_OPTS 8

static struct {
    bool init;
    s32 seq;
    struct {
        s32 id;
        s32 opt[SIG_OPTS];
    } ctx[SIG_CTX];
} g_sig;

static s32 sig_slot(s32 id)
{
    for (s32 i = 0; id > 0 && i < SIG_CTX; i++)
        if (g_sig.ctx[i].id == id)
            return i;
    return -1;
}

static s32 sig_Initialize(s64 pool, s32 prio, s32 affinity, s64 stack)
{
    spin_lock(&g_lock);
    s32 r = g_sig.init ? SIG_ALREADY_INITIALIZED : 0;
    g_sig.init = true;
    spin_unlock(&g_lock);
    NPO_TRACE(r, "npo: sceNpSignalingInitialize -> %x", r);
    return r;
}

static s32 sig_Terminate(void)
{
    spin_lock(&g_lock);
    s32 r = g_sig.init ? 0 : SIG_NOT_INITIALIZED;
    memset(&g_sig.ctx, 0, sizeof(g_sig.ctx));
    g_sig.init = false;
    spin_unlock(&g_lock);
    return r;
}

/* CreateContext(const SceNpId *, handler, arg, s32 *ctx), CreateContextA(s32 user, ...) */
static s32 sig_create(bool bad, s32 *ctx)
{
    spin_lock(&g_lock);
    s32 r = !g_sig.init ? SIG_NOT_INITIALIZED : bad || !ctx ? SIG_INVALID_ARGUMENT : SIG_CTX_MAX;
    for (u32 i = 0; i < SIG_CTX && r == SIG_CTX_MAX; i++) {
        if (g_sig.ctx[i].id)
            continue;
        do
            r = npo_next(&g_sig.seq, 0x7FFFFFFF);
        while (sig_slot(r) >= 0);
        memset(&g_sig.ctx[i], 0, sizeof(g_sig.ctx[i]));
        g_sig.ctx[i].id = r;
        *ctx = r;
        r = 0;
    }
    spin_unlock(&g_lock);
    NPO_TRACE(r, "npo: sceNpSignalingCreateContext -> %x", r);
    return r;
}
static s32 sig_CreateContext(const void *npid, const void *h, void *arg, s32 *ctx) { return sig_create(!npid, ctx); }
static s32 sig_CreateContextA(s32 user, const void *h, void *arg, s32 *ctx) { return sig_create(user == -1, ctx); }

static s32 sig_DeleteContext(s32 ctx)
{
    spin_lock(&g_lock);
    s32 i = sig_slot(ctx);
    if (i >= 0)
        g_sig.ctx[i].id = 0;
    spin_unlock(&g_lock);
    return i >= 0 ? 0 : SIG_CTX_NOT_FOUND;
}

static s32 sig_ActivateConnection(s32 ctx, const void *peer, s32 *conn)
{
    NPO_TRACE(SIG_NETINFO_NOT_AVAILABLE, "npo: sceNpSignalingActivateConnection(%d) -> %x", ctx,
              (u32)SIG_NETINFO_NOT_AVAILABLE);
    return SIG_NETINFO_NOT_AVAILABLE;
}

static s32 sig_GetConnectionStatus(s32 ctx, s32 conn, s32 *status, u32 *addr, u16 *port)
{
    if (status)
        *status = 0; /* INACTIVE */
    return SIG_CONN_NOT_FOUND;
}

static s32 sig_SetContextOption(s32 ctx, s32 opt, s32 val)
{
    spin_lock(&g_lock);
    s32 i = sig_slot(ctx);
    if (i >= 0 && opt >= 0 && opt < SIG_OPTS)
        g_sig.ctx[i].opt[opt] = val;
    spin_unlock(&g_lock);
    return 0;
}

static s32 sig_GetContextOption(s32 ctx, s32 opt, s32 *val)
{
    if (!val)
        return SIG_INVALID_ARGUMENT;
    spin_lock(&g_lock);
    s32 i = sig_slot(ctx);
    *val = i >= 0 && opt >= 0 && opt < SIG_OPTS ? g_sig.ctx[i].opt[opt] : 0;
    spin_unlock(&g_lock);
    return 0;
}

static s32 sig_GetLocalNetInfo(s32 ctx, const u64 *info /* {u64 size = 0x18; ...} */)
{
    return !info || info[0] != 0x18 ? SIG_INVALID_ARGUMENT : SIG_NETINFO_NOT_AVAILABLE;
}

static s32 sig_netinfo(void) { return SIG_NETINFO_NOT_AVAILABLE; }
static s32 sig_conn_not_found(void) { return SIG_CONN_NOT_FOUND; }
static s32 sig_req_not_found(void) { return SIG_REQ_NOT_FOUND; }
static s32 sig_result_not_found(void) { return SIG_RESULT_NOT_FOUND; }
static s32 sig_GetMemoryInfo(u64 *m)
{
    if (m)
        m[0] = m[1] = m[2] = 0;
    return 0;
}
static s32 sig_GetConnectionStatistics(u64 *s)
{
    if (s)
        s[0] = s[1] = 0;
    return 0;
}

/* =================== libSceNpSessionSignaling =================== */

#define SS_NOT_INITIALIZED ((s32)0x80553301)
#define SS_ALREADY_INITIALIZED ((s32)0x80553302)
#define SS_INVALID_ARGUMENT ((s32)0x80553303)
#define SS_CTXID_NOT_AVAILABLE ((s32)0x80553307)
#define SS_CTX_NOT_FOUND ((s32)0x80553308)
#define SS_CONN_NOT_FOUND ((s32)0x8055330C)
#define SS_OFF NPO_OFF(NPO_NP_SIGNED_OUT, NPO_ERR_NET)
#define SS_CTX 4

static struct {
    bool init;
    s32 seq;
    s32 ctx[SS_CTX];
} g_ss;

/* Ghost (0x89b96d): {s32 http ctx; u64 pool 0x80000 @8; u64 0 @0x10;
 * s32 thread priority 700 @0x18; u64 stack 0x8000 @0x20} */
static s32 ss_Initialize(const u8 *p)
{
    spin_lock(&g_lock);
    s32 r = g_ss.init ? SS_ALREADY_INITIALIZED : !p ? SS_INVALID_ARGUMENT : 0;
    if (r == 0)
        g_ss.init = true;
    spin_unlock(&g_lock);
    NPO_TRACE(r, "npo: sceNpSessionSignalingInitialize(http %d, pool %lx, prio %d, stack %lx) -> %x",
              p ? *(const s32 *)p : 0, p ? *(const u64 *)(p + 8) : 0, p ? *(const s32 *)(p + 0x18) : 0,
              p ? *(const u64 *)(p + 0x20) : 0, r);
    return r;
}

static s32 ss_Terminate(void)
{
    spin_lock(&g_lock);
    g_ss.init = false;
    memset(g_ss.ctx, 0, sizeof(g_ss.ctx));
    spin_unlock(&g_lock);
    return 0;
}

/* Ghost (0x89b9e7): {s32 user; s32 0; then {handler, arg} at +8, +0x18,
 * +0x28}, and the id goes to a global it passes to RequestPrepare.  The
 * handlers are never called. */
static s32 ss_CreateContext(const s32 *p, s32 *ctx)
{
    spin_lock(&g_lock);
    s32 r = !g_ss.init ? SS_NOT_INITIALIZED : !p || !ctx ? SS_INVALID_ARGUMENT : SS_CTXID_NOT_AVAILABLE;
    for (u32 i = 0; i < SS_CTX && r == SS_CTXID_NOT_AVAILABLE; i++) {
        if (g_ss.ctx[i])
            continue;
        s32 id;
        bool used;
        do {
            id = npo_next(&g_ss.seq, 0x7FFFFFFF);
            used = false;
            for (u32 k = 0; k < SS_CTX; k++)
                used |= g_ss.ctx[k] == id;
        } while (used);
        g_ss.ctx[i] = *ctx = id;
        r = 0;
    }
    spin_unlock(&g_lock);
    NPO_TRACE(r, "npo: sceNpSessionSignalingCreateContext(user %x) -> %x ctx %d", p ? p[0] : 0, r, r ? 0 : *ctx);
    return r;
}

static s32 ss_DestroyContext(s32 ctx)
{
    spin_lock(&g_lock);
    s32 r = SS_CTX_NOT_FOUND;
    for (u32 i = 0; i < SS_CTX; i++)
        if (ctx > 0 && g_ss.ctx[i] == ctx)
            g_ss.ctx[i] = 0, r = 0;
    spin_unlock(&g_lock);
    return r;
}

/* RequestPrepare(ctx, out), ActivateUser(ctx, peer, s32 *out), GetLocalNetInfo(ctx, info):
 * PSN is not there.  No writes: Ghost ignores RequestPrepare's result and
 * pre-zeroes what it passes to the others. */
static s32 ss_off(s32 ctx)
{
    NPO_TRACE(SS_OFF, "npo: sceNpSessionSignaling request on context %d -> %x", ctx, (u32)SS_OFF);
    return SS_OFF;
}

static s32 ss_GetConnectionStatus(s32 ctx, s32 conn, s32 *status, u32 *addr, u16 *port)
{
    if (status)
        *status = 0;
    return SS_CONN_NOT_FOUND;
}
static s32 ss_conn_not_found(void) { return SS_CONN_NOT_FOUND; }

NPO_SMALL_END

#define M(name, fn) {"libSceNpMatching2", "sceNpMatching2" #name, fn, NULL}
#define M2_REQ(n) M(n, m2_request),
#define M2_REQUESTS(X)                                                                                             \
    X(CreateJoinRoom) X(CreateJoinRoomA) X(GetLobbyInfoList) X(GetLobbyMemberDataInternal)                         \
    X(GetLobbyMemberDataInternalList) X(GetRoomDataExternalList) X(GetRoomDataInternal)                            \
    X(GetRoomMemberDataExternalList) X(GetRoomMemberDataInternal) X(GetUserInfoList) X(GetUserInfoListA)           \
    X(GetWorldIdArrayForAllServers) X(GetWorldInfoList) X(GrantRoomOwner) X(JoinLobby) X(JoinRoom) X(JoinRoomA)    \
    X(KickoutRoomMember) X(LeaveLobby) X(LeaveRoom) X(SearchRoom) X(SendLobbyChatMessage) X(SendRoomChatMessage)   \
    X(SendRoomMessage) X(SetLobbyMemberDataInternal) X(SetRoomDataExternal) X(SetRoomDataInternal)                 \
    X(SetRoomDataInternalExt) X(SetRoomMemberDataInternal) X(SetSignalingOptParam) X(SetUserInfo)                  \
    X(SignalingGetPingInfo)
#define SG(name, fn) {"libSceNpSignaling", "sceNpSignaling" #name, fn, NULL}
#define SS(name, fn) {"libSceNpSessionSignaling", "sceNpSessionSignaling" #name, fn, NULL}

static const struct ext_hle_ent k_hle[] = {
    M(Initialize, m2_Initialize),
    M(Terminate, m2_Terminate),
    M(SetExtraInitParam, ret0),
    M(CreateContextA, m2_CreateContextA),
    M(CreateContext, m2_CreateContext),
    M(DestroyContext, m2_DestroyContext),
    M(RegisterContextCallback, m2_RegisterContextCallback),
    M(RegisterRoomEventCallback, m2_RegisterCtxCallback),
    M(RegisterRoomMessageCallback, m2_RegisterCtxCallback),
    M(RegisterSignalingCallback, m2_RegisterCtxCallback),
    M(RegisterLobbyEventCallback, m2_RegisterCtxCallback),
    M(RegisterLobbyMessageCallback, m2_RegisterCtxCallback),
    M(RegisterManualUdpSignalingCallback, m2_RegisterCtxCallback),
    M(SetDefaultRequestOptParam, m2_SetDefaultRequestOptParam),
    M(ContextStart, m2_ContextStart),
    M(AbortContextStart, m2_AbortContextStart),
    M(ContextStop, m2_not_started),
    M(GetServerId, m2_not_started),
    M2_REQUESTS(M2_REQ)
    M(GetRoomMemberIdListLocal, m2_not_started),
    M(GetRoomPasswordLocal, m2_not_started),
    M(GetSignalingOptParamLocal, m2_not_started),
    M(GetRoomJoinedSlotMaskLocal, m2_not_started),
    M(GetMemoryInfo, m2_GetMemoryInfo),
    M(GetSslMemoryInfo, m2_GetMemoryInfo),
    M(SignalingGetConnectionStatus, m2_SignalingGetConnectionStatus),
    M(SignalingGetConnectionInfo, m2_sig_conn_not_found),
    M(SignalingGetConnectionInfoA, m2_sig_conn_not_found),
    M(SignalingGetLocalNetInfo, m2_sig_netinfo),
    M(SignalingGetPeerNetInfo, m2_sig_netinfo),
    M(SignalingCancelPeerNetInfo, m2_sig_req_not_found),
    M(SignalingGetPeerNetInfoResult, m2_sig_result_not_found),
    M(SignalingEstablishConnection, m2_not_started),
    M(SignalingAbortConnection, m2_not_started),
    M(SignalingEnableManualUdpMode, m2_not_started),
    M(SignalingGetPort, m2_not_started),
    M(SignalingSetPort, m2_not_started),
    M(CreateContextInternal, m2_invalid_argument),

    SG(Initialize, sig_Initialize),
    SG(Terminate, sig_Terminate),
    SG(CreateContext, sig_CreateContext),
    SG(CreateContextA, sig_CreateContextA),
    SG(DeleteContext, sig_DeleteContext),
    SG(ActivateConnection, sig_ActivateConnection),
    SG(ActivateConnectionA, sig_ActivateConnection),
    SG(DeactivateConnection, sig_conn_not_found),
    SG(TerminateConnection, sig_conn_not_found),
    SG(GetConnectionStatus, sig_GetConnectionStatus),
    SG(GetConnectionInfo, sig_conn_not_found),
    SG(GetConnectionInfoA, sig_conn_not_found),
    SG(GetConnectionFromNpId, sig_conn_not_found),
    SG(GetConnectionFromPeerAddress, sig_conn_not_found),
    SG(GetConnectionFromPeerAddressA, sig_conn_not_found),
    SG(SetContextOption, sig_SetContextOption),
    SG(GetContextOption, sig_GetContextOption),
    SG(GetLocalNetInfo, sig_GetLocalNetInfo),
    SG(GetPeerNetInfo, sig_netinfo),
    SG(GetPeerNetInfoA, sig_netinfo),
    SG(CancelPeerNetInfo, sig_req_not_found),
    SG(GetPeerNetInfoResult, sig_result_not_found),
    SG(GetMemoryInfo, sig_GetMemoryInfo),
    SG(GetConnectionStatistics, sig_GetConnectionStatistics),

    SS(Initialize, ss_Initialize),
    SS(Terminate, ss_Terminate),
    SS(CreateContext, ss_CreateContext),
    SS(DestroyContext, ss_DestroyContext),
    SS(RequestPrepare, ss_off),
    SS(ActivateUser, ss_off),
    SS(Deactivate, ret0),
    SS(GetConnectionFromPeerAddress, ss_conn_not_found),
    SS(GetConnectionStatus, ss_GetConnectionStatus),
    SS(GetConnectionInfo, ss_conn_not_found),
    SS(GetLocalNetInfo, ss_off),
};

/* Unlisted names: Matching2 CONTEXT_NOT_STARTED, Signaling
 * NETINFO_NOT_AVAILABLE, SessionSignaling (CreateContext2, ActivateSession,
 * the ManualUdp and group calls, ...) the OFF code. */
static const struct ext_lib k_libs[] = {
    {"libSceNpMatching2", NULL, EXT_LIB_NO_HOST, M2_CONTEXT_NOT_STARTED},
    {"libSceNpSignaling", NULL, EXT_LIB_NO_HOST, SIG_NETINFO_NOT_AVAILABLE},
    {"libSceNpSessionSignaling", NULL, EXT_LIB_NO_HOST, SS_OFF},
};

EXT_FAMILY_LIBS("np-matching", k_hle, k_libs, NULL);
