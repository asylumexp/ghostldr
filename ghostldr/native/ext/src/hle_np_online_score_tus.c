/*
 * libSceNpScore (ranking) and libSceNpTus (title user storage, plus the
 * sceNpTss title small storage calls it exports), virtual (np-online spec
 * §3.3, §3.4).
 *
 * Persona B: creating an NP title context fails with SIGNED_OUT, as on a
 * signed-out console, so no request can exist and every data operation
 * answers INVALID_ID.  Persona A: contexts and requests work, and every data
 * operation fails at once with the network error.  Either way nothing is
 * written to the title's out-params, async operations finish before they
 * return (PollAsync/WaitAsync report the stored result and never "running"),
 * and the board, slot and account arguments are logged.
 */
#include "np_online.h"

NPO_SMALL_BEGIN

#define C_INVALID_ARGUMENT ((s32)0x80550704)  /* SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT */
#define C_NO_LOGIN ((s32)0x80550705)          /* ..._NO_LOGIN */
#define C_TOO_MANY_OBJECTS ((s32)0x80550706)  /* ..._TOO_MANY_OBJECTS */
#define C_ABORTED ((s32)0x80550707)           /* ..._ABORTED */
#define C_INSUFFICIENT_ARG ((s32)0x8055070C)  /* ..._INSUFFICIENT_ARGUMENT */
#define C_INVALID_ID ((s32)0x8055070E)        /* ..._INVALID_ID */
#define C_OFF NPO_OFF(C_NO_LOGIN, NPO_ERR_NET) /* a data operation's answer */

#define COMM_CTX 32
#define COMM_REQ 32

struct comm {
    const char *tag;
    s32 ctx_seq, req_seq;
    struct {
        s32 id;
        bool tss;
    } ctx[COMM_CTX];
    struct {
        s32 id, ctx;
        s32 result; /* 0 until an operation (or an abort) stored one */
        bool aborted;
    } req[COMM_REQ];
};

static struct comm g_score, g_tus; /* Tss contexts live in the Tus table: sceNpTusCreateRequest takes either */
static volatile int g_lock;

#define F_ASYNC 1u
#define F_TUS 2u

static struct comm *comm_of(u32 flags) { return flags & F_TUS ? &g_tus : &g_score; }

static s32 ctx_slot(struct comm *c, s32 id)
{
    for (s32 i = 0; id > 0 && i < COMM_CTX; i++)
        if (c->ctx[i].id == id)
            return i;
    return -1;
}

static s32 req_slot(struct comm *c, s32 id)
{
    for (s32 i = 0; id > 0 && i < COMM_REQ; i++)
        if (c->req[i].id == id)
            return i;
    return -1;
}

/* CreateNpTitleCtx[A]: legacy takes a SceNpId *, the A form a user id */
static s32 comm_create_ctx(u32 flags, u32 label, u64 self, bool legacy, bool tss)
{
    struct comm *c = comm_of(flags);
    s32 r;
    if (legacy && !self)
        r = C_INSUFFICIENT_ARG;
    else if (label == 0xFFFFFFFFu)
        r = C_INVALID_ARGUMENT;
    else if (!NPO_PERSONA_A)
        r = NPO_NP_SIGNED_OUT; /* a signed-out user gets no NP title context */
    else {
        spin_lock(&g_lock);
        r = C_TOO_MANY_OBJECTS;
        for (u32 i = 0; i < COMM_CTX && r == C_TOO_MANY_OBJECTS; i++) {
            if (c->ctx[i].id)
                continue;
            do
                r = npo_next(&c->ctx_seq, 0x7FFFFFFF);
            while (ctx_slot(c, r) >= 0);
            c->ctx[i].id = r, c->ctx[i].tss = tss;
        }
        spin_unlock(&g_lock);
    }
    NPO_TRACE(r, "npo: %s%sCreateNpTitleCtx%s(label %x, user %lx) -> %x", c->tag, tss ? " Tss" : "",
              legacy ? "" : "A", label, self, r);
    return r;
}

static s32 comm_delete_ctx(u32 flags, s32 ctx)
{
    struct comm *c = comm_of(flags);
    spin_lock(&g_lock);
    s32 i = ctx_slot(c, ctx);
    if (i >= 0) {
        c->ctx[i].id = 0;
        for (u32 k = 0; k < COMM_REQ; k++) /* its requests are aborted and gone */
            if (c->req[k].ctx == ctx)
                c->req[k].id = 0, c->req[k].ctx = 0;
    }
    spin_unlock(&g_lock);
    s32 r = i >= 0 ? 0 : C_INVALID_ID;
    NPO_TRACE(r, "npo: %sDeleteNpTitleCtx(%d) -> %x", c->tag, ctx, r);
    return r;
}

static s32 comm_create_req(u32 flags, s32 ctx)
{
    struct comm *c = comm_of(flags);
    spin_lock(&g_lock);
    s32 r = ctx_slot(c, ctx) < 0 ? C_INVALID_ID : C_TOO_MANY_OBJECTS;
    for (u32 i = 0; i < COMM_REQ && r == C_TOO_MANY_OBJECTS; i++) {
        if (c->req[i].id)
            continue;
        do
            r = npo_next(&c->req_seq, 0x7FFFFFFF);
        while (req_slot(c, r) >= 0);
        c->req[i].id = r, c->req[i].ctx = ctx, c->req[i].result = 0, c->req[i].aborted = false;
    }
    spin_unlock(&g_lock);
    NPO_TRACE(r, "npo: %sCreateRequest(%d) -> %x", c->tag, ctx, r);
    return r;
}

/* Delete, Abort */
static s32 comm_req_op(u32 flags, s32 req, bool del)
{
    struct comm *c = comm_of(flags);
    spin_lock(&g_lock);
    s32 i = req_slot(c, req);
    if (i >= 0 && del)
        c->req[i].id = 0, c->req[i].ctx = 0;
    else if (i >= 0) {
        c->req[i].aborted = true;
        if (!c->req[i].result)
            c->req[i].result = C_ABORTED;
    }
    spin_unlock(&g_lock);
    s32 r = i >= 0 ? 0 : C_INVALID_ID;
    NPO_TRACE(r, "npo: %s%sRequest(%d) -> %x", c->tag, del ? "Delete" : "Abort", req, r);
    return r;
}

/* Every data operation, sync or async: (s32 req, ...).  The second and third
 * arguments are the board id, or the target and slot ids: logged. */
static s32 comm_data_op(s32 req, u64 a2, u64 a3, const char *name, u32 flags)
{
    struct comm *c = comm_of(flags);
    spin_lock(&g_lock);
    s32 i = req_slot(c, req), r;
    if (i < 0)
        r = C_INVALID_ID;
    else if (c->req[i].aborted)
        r = C_ABORTED;
    else {
        c->req[i].result = C_OFF;
        r = flags & F_ASYNC ? 0 : C_OFF; /* async: finished before it returns, PollAsync has the result */
    }
    spin_unlock(&g_lock);
    NPO_LOG_FIRST(48, "npo: %s(req %d, %lx, %lx) -> %x", name, req, a2, a3, r);
    return r;
}

static s32 comm_poll(u32 flags, s32 req, s32 *result)
{
    struct comm *c = comm_of(flags);
    if (!result)
        return C_INVALID_ARGUMENT;
    spin_lock(&g_lock);
    s32 i = req_slot(c, req);
    if (i >= 0)
        *result = c->req[i].result ? c->req[i].result : C_OFF;
    spin_unlock(&g_lock);
    s32 r = i >= 0 ? 0 : C_INVALID_ID; /* 0 = finished; never 1 (running) */
    NPO_TRACE(r, "npo: %sPollAsync(%d) -> %x", c->tag, req, r);
    return r;
}

static s32 ret0(void) { return 0; }

/* ---- libSceNpScore ---- */

static s32 score_CreateNpTitleCtxA(u32 label, s32 user) { return comm_create_ctx(0, label, (u32)user, false, false); }
static s32 score_CreateNpTitleCtx(u32 label, const void *npid)
{
    return comm_create_ctx(0, label, (u64)npid, true, false);
}
static s32 score_DeleteNpTitleCtx(s32 ctx) { return comm_delete_ctx(0, ctx); }
static s32 score_CreateRequest(s32 ctx) { return comm_create_req(0, ctx); }
static s32 score_DeleteRequest(s32 req) { return comm_req_op(0, req, true); }
static s32 score_AbortRequest(s32 req) { return comm_req_op(0, req, false); }
static s32 score_PollAsync(s32 req, s32 *result) { return comm_poll(0, req, result); }

/* ---- libSceNpTus (and Tss) ---- */

static s32 tus_CreateNpTitleCtxA(u32 label, s32 user) { return comm_create_ctx(F_TUS, label, (u32)user, false, false); }
static s32 tus_CreateNpTitleCtx(u32 label, const void *npid)
{
    return comm_create_ctx(F_TUS, label, (u64)npid, true, false);
}
static s32 tss_CreateNpTitleCtxA(u32 label, s32 user) { return comm_create_ctx(F_TUS, label, (u32)user, false, true); }
static s32 tss_CreateNpTitleCtx(u32 label, const void *npid)
{
    return comm_create_ctx(F_TUS, label, (u64)npid, true, true);
}
static s32 tus_DeleteNpTitleCtx(s32 ctx) { return comm_delete_ctx(F_TUS, ctx); }
static s32 tus_CreateRequest(s32 ctx) { return comm_create_req(F_TUS, ctx); }
static s32 tus_DeleteRequest(s32 req) { return comm_req_op(F_TUS, req, true); }
static s32 tus_AbortRequest(s32 req) { return comm_req_op(F_TUS, req, false); }
static s32 tus_PollAsync(s32 req, s32 *result) { return comm_poll(F_TUS, req, result); }

/* ---- the data operations (spec appendix A.3 to A.6) ---- */

#define SCORE_SYNC(X)                                                                                              \
    X(sceNpScoreCensorComment) X(sceNpScoreGetBoardInfo) X(sceNpScoreGetFriendsRanking)                            \
    X(sceNpScoreGetFriendsRankingA) X(sceNpScoreGetFriendsRankingForCrossSave) X(sceNpScoreGetGameData)            \
    X(sceNpScoreGetGameDataByAccountId) X(sceNpScoreGetRankingByAccountId)                                         \
    X(sceNpScoreGetRankingByAccountIdForCrossSave) X(sceNpScoreGetRankingByAccountIdPcId)                          \
    X(sceNpScoreGetRankingByAccountIdPcIdForCrossSave) X(sceNpScoreGetRankingByNpId)                               \
    X(sceNpScoreGetRankingByNpIdPcId) X(sceNpScoreGetRankingByRange) X(sceNpScoreGetRankingByRangeA)               \
    X(sceNpScoreGetRankingByRangeForCrossSave) X(sceNpScoreRecordGameData) X(sceNpScoreRecordScore)                \
    X(sceNpScoreSanitizeComment)

#define SCORE_ASYNC(X)                                                                                             \
    X(sceNpScoreCensorCommentAsync) X(sceNpScoreGetBoardInfoAsync) X(sceNpScoreGetFriendsRankingAAsync)            \
    X(sceNpScoreGetFriendsRankingAsync) X(sceNpScoreGetFriendsRankingForCrossSaveAsync)                            \
    X(sceNpScoreGetGameDataAsync) X(sceNpScoreGetGameDataByAccountIdAsync) X(sceNpScoreGetRankingByAccountIdAsync) \
    X(sceNpScoreGetRankingByAccountIdForCrossSaveAsync) X(sceNpScoreGetRankingByAccountIdPcIdAsync)                \
    X(sceNpScoreGetRankingByAccountIdPcIdForCrossSaveAsync) X(sceNpScoreGetRankingByNpIdAsync)                     \
    X(sceNpScoreGetRankingByNpIdPcIdAsync) X(sceNpScoreGetRankingByRangeAAsync) X(sceNpScoreGetRankingByRangeAsync) \
    X(sceNpScoreGetRankingByRangeForCrossSaveAsync) X(sceNpScoreRecordGameDataAsync) X(sceNpScoreRecordScoreAsync) \
    X(sceNpScoreSanitizeCommentAsync)

#define TUS_SYNC(X)                                                                                                \
    X(sceNpTssGetData) X(sceNpTssGetSmallStorage) X(sceNpTssGetStorage)                                            \
    X(sceNpTusAddAndGetVariable) X(sceNpTusAddAndGetVariableA) X(sceNpTusAddAndGetVariableAVUser)                  \
    X(sceNpTusAddAndGetVariableForCrossSave) X(sceNpTusAddAndGetVariableForCrossSaveVUser)                         \
    X(sceNpTusAddAndGetVariableVUser) X(sceNpTusDeleteMultiSlotData) X(sceNpTusDeleteMultiSlotDataA)               \
    X(sceNpTusDeleteMultiSlotDataVUser) X(sceNpTusDeleteMultiSlotVariable) X(sceNpTusDeleteMultiSlotVariableA)     \
    X(sceNpTusDeleteMultiSlotVariableVUser) X(sceNpTusGetData) X(sceNpTusGetDataA) X(sceNpTusGetDataAVUser)        \
    X(sceNpTusGetDataForCrossSave) X(sceNpTusGetDataForCrossSaveVUser) X(sceNpTusGetDataVUser)                     \
    X(sceNpTusGetFriendsDataStatus) X(sceNpTusGetFriendsDataStatusA) X(sceNpTusGetFriendsDataStatusForCrossSave)   \
    X(sceNpTusGetFriendsVariable) X(sceNpTusGetFriendsVariableA) X(sceNpTusGetFriendsVariableForCrossSave)         \
    X(sceNpTusGetMultiSlotDataStatus) X(sceNpTusGetMultiSlotDataStatusA) X(sceNpTusGetMultiSlotDataStatusAVUser)   \
    X(sceNpTusGetMultiSlotDataStatusForCrossSave) X(sceNpTusGetMultiSlotDataStatusForCrossSaveVUser)               \
    X(sceNpTusGetMultiSlotDataStatusVUser) X(sceNpTusGetMultiSlotVariable) X(sceNpTusGetMultiSlotVariableA)        \
    X(sceNpTusGetMultiSlotVariableAVUser) X(sceNpTusGetMultiSlotVariableForCrossSave)                              \
    X(sceNpTusGetMultiSlotVariableForCrossSaveVUser) X(sceNpTusGetMultiSlotVariableVUser)                          \
    X(sceNpTusGetMultiUserDataStatus) X(sceNpTusGetMultiUserDataStatusA) X(sceNpTusGetMultiUserDataStatusAVUser)   \
    X(sceNpTusGetMultiUserDataStatusForCrossSave) X(sceNpTusGetMultiUserDataStatusForCrossSaveVUser)               \
    X(sceNpTusGetMultiUserDataStatusVUser) X(sceNpTusGetMultiUserVariable) X(sceNpTusGetMultiUserVariableA)        \
    X(sceNpTusGetMultiUserVariableAVUser) X(sceNpTusGetMultiUserVariableForCrossSave)                              \
    X(sceNpTusGetMultiUserVariableForCrossSaveVUser) X(sceNpTusGetMultiUserVariableVUser) X(sceNpTusSetData)       \
    X(sceNpTusSetDataA) X(sceNpTusSetDataAVUser) X(sceNpTusSetDataVUser) X(sceNpTusSetMultiSlotVariable)           \
    X(sceNpTusSetMultiSlotVariableA) X(sceNpTusSetMultiSlotVariableVUser) X(sceNpTusTryAndSetVariable)             \
    X(sceNpTusTryAndSetVariableA) X(sceNpTusTryAndSetVariableAVUser) X(sceNpTusTryAndSetVariableForCrossSave)      \
    X(sceNpTusTryAndSetVariableForCrossSaveVUser) X(sceNpTusTryAndSetVariableVUser)

#define TUS_ASYNC(X)                                                                                               \
    X(sceNpTssGetDataAsync) X(sceNpTssGetSmallStorageAsync) X(sceNpTssGetStorageAsync)                             \
    X(sceNpTusAddAndGetVariableAAsync) X(sceNpTusAddAndGetVariableAVUserAsync) X(sceNpTusAddAndGetVariableAsync)   \
    X(sceNpTusAddAndGetVariableForCrossSaveAsync) X(sceNpTusAddAndGetVariableForCrossSaveVUserAsync)               \
    X(sceNpTusAddAndGetVariableVUserAsync) X(sceNpTusDeleteMultiSlotDataAAsync) X(sceNpTusDeleteMultiSlotDataAsync) \
    X(sceNpTusDeleteMultiSlotDataVUserAsync) X(sceNpTusDeleteMultiSlotVariableAAsync)                              \
    X(sceNpTusDeleteMultiSlotVariableAsync) X(sceNpTusDeleteMultiSlotVariableVUserAsync) X(sceNpTusGetDataAAsync)  \
    X(sceNpTusGetDataAVUserAsync) X(sceNpTusGetDataAsync) X(sceNpTusGetDataForCrossSaveAsync)                      \
    X(sceNpTusGetDataForCrossSaveVUserAsync) X(sceNpTusGetDataVUserAsync) X(sceNpTusGetFriendsDataStatusAAsync)    \
    X(sceNpTusGetFriendsDataStatusAsync) X(sceNpTusGetFriendsDataStatusForCrossSaveAsync)                          \
    X(sceNpTusGetFriendsVariableAAsync) X(sceNpTusGetFriendsVariableAsync)                                         \
    X(sceNpTusGetFriendsVariableForCrossSaveAsync) X(sceNpTusGetMultiSlotDataStatusAAsync)                         \
    X(sceNpTusGetMultiSlotDataStatusAVUserAsync) X(sceNpTusGetMultiSlotDataStatusAsync)                            \
    X(sceNpTusGetMultiSlotDataStatusForCrossSaveAsync) X(sceNpTusGetMultiSlotDataStatusForCrossSaveVUserAsync)     \
    X(sceNpTusGetMultiSlotDataStatusVUserAsync) X(sceNpTusGetMultiSlotVariableAAsync)                              \
    X(sceNpTusGetMultiSlotVariableAVUserAsync) X(sceNpTusGetMultiSlotVariableAsync)                                \
    X(sceNpTusGetMultiSlotVariableForCrossSaveAsync) X(sceNpTusGetMultiSlotVariableForCrossSaveVUserAsync)         \
    X(sceNpTusGetMultiSlotVariableVUserAsync) X(sceNpTusGetMultiUserDataStatusAAsync)                              \
    X(sceNpTusGetMultiUserDataStatusAVUserAsync) X(sceNpTusGetMultiUserDataStatusAsync)                            \
    X(sceNpTusGetMultiUserDataStatusForCrossSaveAsync) X(sceNpTusGetMultiUserDataStatusForCrossSaveVUserAsync)     \
    X(sceNpTusGetMultiUserDataStatusVUserAsync) X(sceNpTusGetMultiUserVariableAAsync)                              \
    X(sceNpTusGetMultiUserVariableAVUserAsync) X(sceNpTusGetMultiUserVariableAsync)                                \
    X(sceNpTusGetMultiUserVariableForCrossSaveAsync) X(sceNpTusGetMultiUserVariableForCrossSaveVUserAsync)         \
    X(sceNpTusGetMultiUserVariableVUserAsync) X(sceNpTusSetDataAAsync) X(sceNpTusSetDataAVUserAsync)               \
    X(sceNpTusSetDataAsync) X(sceNpTusSetDataVUserAsync) X(sceNpTusSetMultiSlotVariableAAsync)                     \
    X(sceNpTusSetMultiSlotVariableAsync) X(sceNpTusSetMultiSlotVariableVUserAsync)                                 \
    X(sceNpTusTryAndSetVariableAAsync) X(sceNpTusTryAndSetVariableAVUserAsync) X(sceNpTusTryAndSetVariableAsync)   \
    X(sceNpTusTryAndSetVariableForCrossSaveAsync) X(sceNpTusTryAndSetVariableForCrossSaveVUserAsync)               \
    X(sceNpTusTryAndSetVariableVUserAsync)

/* one thunk per name, so the log says which operation the title used */
#define THUNK(name, flags)                                                                                         \
    static s32 h_##name(s32 req, u64 a2, u64 a3) { return comm_data_op(req, a2, a3, #name, flags); }
#define SCORE_SYNC_FN(n) THUNK(n, 0)
#define SCORE_ASYNC_FN(n) THUNK(n, F_ASYNC)
#define TUS_SYNC_FN(n) THUNK(n, F_TUS)
#define TUS_ASYNC_FN(n) THUNK(n, F_TUS | F_ASYNC)
SCORE_SYNC(SCORE_SYNC_FN)
SCORE_ASYNC(SCORE_ASYNC_FN)
TUS_SYNC(TUS_SYNC_FN)
TUS_ASYNC(TUS_ASYNC_FN)

NPO_SMALL_END

#define SCORE_ENT(n) {"libSceNpScore", #n, h_##n, NULL},
#define TUS_ENT(n) {"libSceNpTus", #n, h_##n, NULL},
#define S(name, fn) {"libSceNpScore", "sceNpScore" #name, fn, NULL}
#define T(name, fn) {"libSceNpTus", #name, fn, NULL}

static const struct ext_hle_ent k_hle[] = {
    S(CreateNpTitleCtxA, score_CreateNpTitleCtxA),
    S(CreateNpTitleCtx, score_CreateNpTitleCtx),
    S(DeleteNpTitleCtx, score_DeleteNpTitleCtx),
    S(CreateRequest, score_CreateRequest),
    S(DeleteRequest, score_DeleteRequest),
    S(AbortRequest, score_AbortRequest),
    S(SetPlayerCharacterId, ret0),
    S(SetTimeout, ret0),
    S(SetThreadParam, ret0),
    S(ChangeModeForOtherSaveDataOwners, ret0),
    S(PollAsync, score_PollAsync),
    S(WaitAsync, score_PollAsync), /* never blocks: every operation already finished */
    SCORE_SYNC(SCORE_ENT) SCORE_ASYNC(SCORE_ENT)

    T(sceNpTusCreateNpTitleCtxA, tus_CreateNpTitleCtxA),
    T(sceNpTusCreateNpTitleCtx, tus_CreateNpTitleCtx),
    T(sceNpTssCreateNpTitleCtxA, tss_CreateNpTitleCtxA),
    T(sceNpTssCreateNpTitleCtx, tss_CreateNpTitleCtx),
    T(sceNpTusDeleteNpTitleCtx, tus_DeleteNpTitleCtx),
    T(sceNpTusCreateRequest, tus_CreateRequest),
    T(sceNpTusDeleteRequest, tus_DeleteRequest),
    T(sceNpTusAbortRequest, tus_AbortRequest),
    T(sceNpTusSetTimeout, ret0),
    T(sceNpTusSetThreadParam, ret0),
    T(sceNpTusChangeModeForOtherSaveDataOwners, ret0),
    T(sceNpTusPollAsync, tus_PollAsync),
    T(sceNpTusWaitAsync, tus_PollAsync),
    TUS_SYNC(TUS_ENT) TUS_ASYNC(TUS_ENT)
};

/* Anything else (the pre-NpTitle CreateTitleCtx calls, newer names) gets a
 * stub returning the data operations' answer, never SWRR's library. */
static const struct ext_lib k_libs[] = {
    {"libSceNpScore", "libSceNpScoreCompat", EXT_LIB_NO_HOST, C_OFF},
    {"libSceNpTus", "libSceNpTusCompat", EXT_LIB_NO_HOST, C_OFF},
};

static int np_score_tus_init(void)
{
    g_score.tag = "sceNpScore", g_tus.tag = "sceNpTus";
    return 0;
}

EXT_FAMILY_LIBS("np-score-tus", k_hle, k_libs, np_score_tus_init);
