/*
 * libSceNpWebApi and libSceNpWebApi2, virtual (np-online spec §3.1, §3.2).
 *
 * One state machine serves both: they share the id layout (lib context
 * 1..0x7FFF, user context lib << 16 | n, request (s64)user << 32 | n) and
 * their error codes differ only in the base and a few offsets.  Contexts,
 * requests, handles, push filters, push callbacks and push contexts are
 * local and always work; sending a request fails at once with NOT_SIGNED_IN
 * (persona B) or the network error (persona A), and every later read
 * returns that error.  No push event is ever delivered.
 *
 * Ghost's eboot calls sceNpWebApi2Initialize in a loop until it returns
 * more than 0, so Initialize never fails: with every slot in use it hands
 * back the newest library context again.
 */
#include "np_online.h"

NPO_SMALL_BEGIN

#define WA_LIBS 4
#define WA_USERS 16
#define WA_REQS 64
#define WA_OBJS 48
#define WA_PUSHCTX 8

/* error offsets common to both libraries */
#define E_OOM 0x01
#define E_ARG 0x02
#define E_BAD_LIB 0x03
#define E_NO_LIB 0x04
#define E_NO_USER 0x05
#define E_NO_REQ 0x06
#define E_NOT_SIGNED_IN 0x07
#define E_CONTENT 0x08
#define E_ABORTED 0x09
#define E_USER_EXISTS 0x0A
#define E_NO_FILTER 0x0B
#define E_NO_CALLBACK 0x0C
#define E_NO_HANDLE 0x0D

enum { OBJ_HANDLE = 1, OBJ_FILTER, OBJ_SVC_FILTER, OBJ_EXTD_FILTER, OBJ_CALLBACK };

struct wa_lib {
    s32 id;
    u64 pool;
};
struct wa_user {
    s32 id; /* lib << 16 | n */
    s32 user;
};
struct wa_req {
    s64 id; /* (s64)user context << 32 | n */
    s32 err; /* what reads return: OFF, or ABORTED */
    u16 parts;
    bool sent;
};
struct wa_obj {
    s32 id, owner; /* owner: lib context (handles, filters) or user context (callbacks) */
    u8 kind;
};

struct wa_api {
    const char *tag;
    u32 base;
    /* offsets that differ between WebApi and WebApi2 */
    u8 e_lib_max, e_user_max, e_after_send, e_svc_filter, e_extd_filter, e_push_ctx;
    s32 lib_seq, user_seq, req_seq, obj_seq, pctx_seq;
    struct wa_lib lib[WA_LIBS];
    struct wa_user user[WA_USERS];
    struct wa_req req[WA_REQS];
    struct wa_obj obj[WA_OBJS];
    struct {
        s32 user;
        u32 n;
    } pctx[WA_PUSHCTX];
};

/* zero-initialized (bss); np_webapi_init fills in the constants */
static struct wa_api g_wa, g_wa2;
static volatile int g_lock;

#define ERR(a, e) ((s32)((a)->base + (e)))
#define OFF(a) NPO_OFF(ERR(a, E_NOT_SIGNED_IN), NPO_ERR_NET)
#define LOCK() spin_lock(&g_lock)
#define UNLOCK() spin_unlock(&g_lock)

/* ---- tables (callers hold the lock) ---- */

static struct wa_lib *lib_find(struct wa_api *a, s32 id)
{
    for (u32 i = 0; id > 0 && i < WA_LIBS; i++)
        if (a->lib[i].id == id)
            return &a->lib[i];
    return NULL;
}

static struct wa_user *user_find(struct wa_api *a, s32 id)
{
    for (u32 i = 0; id > 0 && i < WA_USERS; i++)
        if (a->user[i].id == id)
            return &a->user[i];
    return NULL;
}

static struct wa_req *req_find(struct wa_api *a, s64 id)
{
    for (u32 i = 0; id > 0 && i < WA_REQS; i++)
        if (a->req[i].id == id)
            return &a->req[i];
    return NULL;
}

static struct wa_obj *obj_find(struct wa_api *a, s32 id, u8 kind)
{
    for (u32 i = 0; id > 0 && i < WA_OBJS; i++)
        if (a->obj[i].id == id && a->obj[i].kind == kind)
            return &a->obj[i];
    return NULL;
}

static bool obj_id_used(struct wa_api *a, s32 id)
{
    for (u32 i = 0; i < WA_OBJS; i++)
        if (a->obj[i].id == id)
            return true;
    return false;
}

/* a new handle, filter or callback id (one counter for all of them) */
static s32 obj_new(struct wa_api *a, s32 owner, u8 kind)
{
    for (u32 i = 0; i < WA_OBJS; i++) {
        if (a->obj[i].id)
            continue;
        s32 id;
        do
            id = npo_next(&a->obj_seq, 0x7FFFFFFF);
        while (obj_id_used(a, id));
        a->obj[i] = (struct wa_obj){id, owner, kind};
        return id;
    }
    return ERR(a, E_OOM);
}

static void drop_user_children(struct wa_api *a, s32 uctx)
{
    for (u32 i = 0; i < WA_REQS; i++)
        if (a->req[i].id && (s32)(a->req[i].id >> 32) == uctx)
            a->req[i].id = 0;
    for (u32 i = 0; i < WA_OBJS; i++)
        if (a->obj[i].owner == uctx)
            a->obj[i].id = 0, a->obj[i].owner = 0;
    for (u32 i = 0; i < WA_PUSHCTX; i++)
        if (a->pctx[i].user == uctx)
            a->pctx[i].user = 0;
}

static void pctx_name(u32 n, char out[37])
{
    fmt(out, 37, "00000000-0000-4000-8000-%012x", n);
}

/* ---- library and user contexts ---- */

static s32 wa_init(struct wa_api *a, s32 http, u64 pool)
{
    LOCK();
    s32 id = 0;
    for (u32 i = 0; i < WA_LIBS && !id; i++) {
        if (a->lib[i].id)
            continue;
        do
            id = npo_next(&a->lib_seq, 0x7FFF);
        while (lib_find(a, id));
        a->lib[i] = (struct wa_lib){id, pool};
    }
    if (!id) /* every slot in use: titles retry until this is > 0, so never fail */
        id = a->lib_seq > 0 ? a->lib_seq : 1;
    UNLOCK();
    NPO_TRACE(id, "npo: %sInitialize(%d, %lx) -> %d", a->tag, http, pool, id);
    return id;
}

static s32 wa_term(struct wa_api *a, s32 lib)
{
    LOCK();
    struct wa_lib *l = lib_find(a, lib);
    s32 r = l ? 0 : ERR(a, E_NO_LIB);
    if (l) {
        l->id = 0;
        for (u32 i = 0; i < WA_USERS; i++)
            if (a->user[i].id && (a->user[i].id >> 16) == lib) {
                drop_user_children(a, a->user[i].id);
                a->user[i].id = 0;
            }
        for (u32 i = 0; i < WA_OBJS; i++)
            if (a->obj[i].owner == lib)
                a->obj[i].id = 0, a->obj[i].owner = 0;
    }
    UNLOCK();
    NPO_TRACE(r, "npo: %sTerminate(%d) -> %x", a->tag, lib, r);
    return r;
}

#define NPO_LEGACY_USER 0x7FFFFFFE /* CreateContext(lib, onlineId): the initial user */

static s32 wa_create_user(struct wa_api *a, s32 lib, s32 user)
{
    s32 r;
    if (lib >= 0x8000)
        r = ERR(a, E_BAD_LIB);
    else if (user == -1)
        r = ERR(a, E_ARG);
    else {
        LOCK();
        r = lib_find(a, lib) ? 0 : ERR(a, E_NO_LIB);
        struct wa_user *slot = NULL;
        for (u32 i = 0; i < WA_USERS && r == 0; i++) {
            if (a->user[i].id && (a->user[i].id >> 16) == lib && a->user[i].user == user)
                r = ERR(a, E_USER_EXISTS);
            else if (!a->user[i].id && !slot)
                slot = &a->user[i];
        }
        if (r == 0 && !slot)
            r = ERR(a, a->e_user_max);
        if (r == 0) {
            do
                r = lib << 16 | npo_next(&a->user_seq, 0xFFFF);
            while (user_find(a, r));
            *slot = (struct wa_user){r, user};
        }
        UNLOCK();
    }
    NPO_TRACE(r, "npo: %sCreateUserContext(%d, %x) -> %x", a->tag, lib, user, r);
    return r;
}

static s32 wa_delete_user(struct wa_api *a, s32 uctx)
{
    LOCK();
    struct wa_user *u = user_find(a, uctx);
    s32 r = u ? 0 : ERR(a, E_NO_USER);
    if (u) {
        drop_user_children(a, uctx);
        u->id = 0;
    }
    UNLOCK();
    NPO_TRACE(r, "npo: %sDeleteUserContext(%x) -> %x", a->tag, uctx, r);
    return r;
}

/* ---- requests ---- */

struct content_param { /* SceNpWebApi[2]ContentParameter */
    u64 length;
    const char *type;
};

static s32 wa_create_req(struct wa_api *a, s32 uctx, const char *grp, const char *path, const char *method,
                         const struct content_param *cp, s64 *out)
{
    s32 r = 0;
    s64 id = 0;
    if (!grp || !path || !method || !out)
        r = ERR(a, E_ARG);
    else if (cp && cp->length && !cp->type)
        r = ERR(a, E_CONTENT);
    else {
        LOCK();
        struct wa_req *slot = NULL;
        for (u32 i = 0; i < WA_REQS && !slot; i++)
            if (!a->req[i].id)
                slot = &a->req[i];
        if (!user_find(a, uctx))
            r = ERR(a, E_NO_USER);
        else if (!slot)
            r = ERR(a, E_OOM);
        else {
            do
                id = (s64)uctx << 32 | npo_next(&a->req_seq, 0x7FFFFFFF);
            while (req_find(a, id));
            *slot = (struct wa_req){id, OFF(a), 0, false};
            *out = id;
        }
        UNLOCK();
    }
    /* what the title wants from PSN */
    NPO_LOG_FIRST(48, "npo: %sCreateRequest(%x, %s, %s %s) -> %x req %lx", a->tag, uctx, grp ? grp : "-",
                  method ? method : "-", path ? path : "-", r, id);
    return r;
}

static const char *method_name(s32 m)
{
    static const char *const k[] = {"GET", "POST", "PUT", "DELETE", "PATCH"};
    return m >= 0 && m < 5 ? k[m] : "?";
}

struct resp_info { /* SceNpWebApi[2]ResponseInformationOption */
    s32 http_status;
    s32 reserved;
    char *error_object;
    u64 error_object_size;
    u64 response_data_size;
};

static s32 wa_send(struct wa_api *a, s64 req, s32 part, bool multipart, const void *data, struct resp_info *ri)
{
    s32 r;
    if (multipart && (part <= 0 || !data))
        r = ERR(a, E_ARG);
    else {
        LOCK();
        struct wa_req *q = req_find(a, req);
        r = !q ? ERR(a, E_NO_REQ) : q->err == ERR(a, E_ABORTED) ? q->err : OFF(a);
        if (q && r == OFF(a))
            q->sent = true, q->err = r;
        UNLOCK();
        if (q && ri) { /* what the real library reports offline: no HTTP status, no body */
            ri->http_status = 0;
            ri->response_data_size = 0;
            if (ri->error_object && ri->error_object_size)
                ri->error_object[0] = 0;
        }
    }
    NPO_TRACE(r, "npo: %sSend%sRequest(%lx) -> %x", a->tag, multipart ? "Multipart" : "", req, r);
    return r;
}

/* the error a request answers reads with (or REQUEST_NOT_FOUND) */
static s32 req_err(struct wa_api *a, s64 req, bool need_unsent)
{
    LOCK();
    struct wa_req *q = req_find(a, req);
    s32 r = !q ? ERR(a, E_NO_REQ) : need_unsent ? (q->sent ? ERR(a, a->e_after_send) : 0) : q->err;
    UNLOCK();
    return r;
}

static s32 wa_read(struct wa_api *a, s64 req, void *buf, u64 size)
{
    s32 r = !buf || !size ? ERR(a, E_ARG) : req_err(a, req, false);
    NPO_TRACE(r, "npo: %sReadData(%lx, %lu) -> %x", a->tag, req, size, r);
    return r;
}

static s32 wa_header_value(struct wa_api *a, s64 req, const char *name, char *val, u64 size)
{
    if (!name || !val || !size)
        return ERR(a, E_ARG);
    val[0] = 0;
    s32 r = req_err(a, req, false);
    NPO_TRACE(r, "npo: %sGetHttpResponseHeaderValue(%lx, %s) -> %x", a->tag, req, name, r);
    return r;
}

static s32 wa_header_length(struct wa_api *a, s64 req, const char *name, u64 *len)
{
    if (!name || !len)
        return ERR(a, E_ARG);
    *len = 0;
    return req_err(a, req, false);
}

static s32 wa_add_header(struct wa_api *a, s64 req, const char *name, const char *value)
{
    s32 r = !name || !value ? ERR(a, E_ARG) : req_err(a, req, true);
    NPO_TRACE(r, "npo: %sAddHttpRequestHeader(%lx, %s) -> %x", a->tag, req, name ? name : "-", r);
    return r;
}

static s32 wa_abort_delete(struct wa_api *a, s64 req, bool del)
{
    LOCK();
    struct wa_req *q = req_find(a, req);
    s32 r = q ? 0 : ERR(a, E_NO_REQ);
    if (q && del)
        q->id = 0;
    else if (q)
        q->err = ERR(a, E_ABORTED);
    UNLOCK();
    NPO_TRACE(r, "npo: %s%sRequest(%lx) -> %x", a->tag, del ? "Delete" : "Abort", req, r);
    return r;
}

/* ---- handles, push filters, push callbacks, push contexts ---- */

static s32 wa_create_obj(struct wa_api *a, s32 owner, u8 kind)
{
    LOCK();
    s32 r = kind == OBJ_CALLBACK ? (user_find(a, owner) ? 0 : ERR(a, E_NO_USER))
                                 : (lib_find(a, owner) ? 0 : ERR(a, E_NO_LIB));
    if (r == 0)
        r = obj_new(a, owner, kind);
    UNLOCK();
    NPO_TRACE(r, "npo: %s create %s(%x) -> %x", a->tag,
              kind == OBJ_HANDLE ? "handle" : kind == OBJ_CALLBACK ? "push callback" : "push filter", owner, r);
    return r;
}

/* delete (or just check) one; missing: not_found */
static s32 wa_obj_op(struct wa_api *a, s32 id, u8 kind, bool del, s32 not_found)
{
    LOCK();
    struct wa_obj *o = obj_find(a, id, kind);
    if (o && del)
        o->id = 0, o->owner = 0;
    UNLOCK();
    return o ? 0 : not_found;
}

static s32 wa_create_filter(struct wa_api *a, s32 lib, const char *svc, u32 label, const void *p, u64 n, u8 kind,
                            bool check_label)
{
    if ((check_label && svc && label == 0xFFFFFFFFu) || (kind != OBJ_SVC_FILTER && (!p || !n)))
        return ERR(a, E_ARG);
    if (svc)
        NPO_LOG_FIRST(16, "npo: %s push filter for service %s label %x", a->tag, svc, label);
    return wa_create_obj(a, lib, kind);
}

static s32 wa_register_cb(struct wa_api *a, s32 uctx, s32 filter, const void *cb)
{
    return cb ? wa_create_obj(a, uctx, OBJ_CALLBACK) : ERR(a, E_ARG);
}

static s32 wa_pool_stats(struct wa_api *a, s32 lib, u64 *stats)
{
    if (!stats)
        return ERR(a, E_ARG);
    LOCK();
    struct wa_lib *l = lib_find(a, lib);
    u64 pool = l ? l->pool : 0;
    UNLOCK();
    if (!l)
        return ERR(a, E_NO_LIB);
    /* {poolSize, maxInuseSize, currentInuseSize, reserved} */
    memset(stats, 0, 0x20);
    stats[0] = pool;
    return 0;
}

static s32 ret0(void) { return 0; }

/* ---- libSceNpWebApi ---- */

static s32 wa1_Initialize(s32 http, u64 pool) { return wa_init(&g_wa, http, pool); }
static s32 wa1_Terminate(s32 lib) { return wa_term(&g_wa, lib); }
static s32 wa1_CreateContextA(s32 lib, s32 user) { return wa_create_user(&g_wa, lib, user); }
static s32 wa1_CreateContext(s32 lib, const void *online_id)
{
    return online_id ? wa_create_user(&g_wa, lib, NPO_LEGACY_USER) : ERR(&g_wa, E_ARG);
}
static s32 wa1_DeleteContext(s32 uctx) { return wa_delete_user(&g_wa, uctx); }
static s32 wa1_CreateRequest(s32 uctx, const char *grp, const char *path, s32 method,
                             const struct content_param *cp, s64 *out)
{
    return wa_create_req(&g_wa, uctx, grp, path, method_name(method), cp, out);
}
static s32 wa1_CreateMultipartRequest(s32 uctx, const char *grp, const char *path, s32 method, s64 *out)
{
    return wa_create_req(&g_wa, uctx, grp, path, method_name(method), NULL, out);
}
static s32 wa1_SendRequest(s64 req, const void *data, u64 size) { return wa_send(&g_wa, req, 0, false, data, NULL); }
static s32 wa1_SendRequest2(s64 req, const void *data, u64 size, struct resp_info *ri)
{
    return wa_send(&g_wa, req, 0, false, data, ri);
}
static s32 wa1_SendMultipartRequest(s64 req, s32 part, const void *data, u64 size)
{
    return wa_send(&g_wa, req, part, true, data, NULL);
}
static s32 wa1_SendMultipartRequest2(s64 req, s32 part, const void *data, u64 size, struct resp_info *ri)
{
    return wa_send(&g_wa, req, part, true, data, ri);
}
static s32 wa1_ReadData(s64 req, void *buf, u64 size) { return wa_read(&g_wa, req, buf, size); }
static s32 wa1_GetHttpStatusCode(s64 req, s32 *status)
{
    if (status)
        *status = 0;
    return req_err(&g_wa, req, false);
}
static s32 wa1_GetHttpResponseHeaderValue(s64 req, const char *name, char *val, u64 size)
{
    return wa_header_value(&g_wa, req, name, val, size);
}
static s32 wa1_GetHttpResponseHeaderValueLength(s64 req, const char *name, u64 *len)
{
    return wa_header_length(&g_wa, req, name, len);
}
static s32 wa1_AddHttpRequestHeader(s64 req, const char *name, const char *value)
{
    return wa_add_header(&g_wa, req, name, value);
}
static s32 wa1_AddMultipartPart(s64 req, const void *param, s32 *index)
{
    if (!index)
        return ERR(&g_wa, E_ARG);
    LOCK();
    struct wa_req *q = req_find(&g_wa, req);
    if (q)
        *index = ++q->parts;
    UNLOCK();
    return q ? 0 : ERR(&g_wa, E_NO_REQ);
}
static s32 wa1_AbortRequest(s64 req) { return wa_abort_delete(&g_wa, req, false); }
static s32 wa1_DeleteRequest(s64 req) { return wa_abort_delete(&g_wa, req, true); }
static s32 wa1_CreateHandle(s32 lib) { return wa_create_obj(&g_wa, lib, OBJ_HANDLE); }
static s32 wa1_DeleteHandle(s32 lib, s32 h) { return wa_obj_op(&g_wa, h, OBJ_HANDLE, true, ERR(&g_wa, E_NO_HANDLE)); }
static s32 wa1_HandleOp(s32 lib, s32 h) { return wa_obj_op(&g_wa, h, OBJ_HANDLE, false, ERR(&g_wa, E_NO_HANDLE)); }
static s32 wa1_GetMemoryPoolStats(s32 lib, u64 *stats) { return wa_pool_stats(&g_wa, lib, stats); }
static s32 wa1_GetConnectionStats(s32 uctx, const char *grp, u32 *stats)
{
    if (!stats)
        return ERR(&g_wa, E_ARG);
    /* {max 1, used 0, unused 0, keepAlive 0, reserved} */
    memset(stats, 0, 0x18);
    stats[0] = 1;
    return 0;
}
static s32 wa1_UtilityParseNpId(const char *json, void *npid)
{
    /* a pure decoder with no identity in it: SWRR's copy is fine */
    static s32 (*real)(const char *, void *);
    if (!real)
        real = (s32(*)(const char *, void *))host_real_in("libSceNpWebApi", "sceNpWebApiUtilityParseNpId");
    s32 r = real ? real(json, npid) : ERR(&g_wa, E_ARG);
    NPO_TRACE(r, "npo: sceNpWebApiUtilityParseNpId -> %x", r);
    return r;
}
static s32 wa1_CreatePushEventFilter(s32 lib, const void *p, u64 n)
{
    return wa_create_filter(&g_wa, lib, NULL, 0, p, n, OBJ_FILTER, false);
}
static s32 wa1_CreateServicePushEventFilter(s32 lib, s32 h, const char *svc, u32 label, const void *p, u64 n)
{
    return wa_create_filter(&g_wa, lib, svc, label, p, n, OBJ_SVC_FILTER, false);
}
static s32 wa1_CreateExtdPushEventFilter(s32 lib, s32 h, const char *svc, u32 label, const void *p, u64 n)
{
    return wa_create_filter(&g_wa, lib, svc, label, p, n, OBJ_EXTD_FILTER, true);
}
static s32 wa1_DeletePushEventFilter(s32 lib, s32 id)
{
    return wa_obj_op(&g_wa, id, OBJ_FILTER, true, ERR(&g_wa, E_NO_FILTER));
}
static s32 wa1_DeleteServicePushEventFilter(s32 lib, s32 id)
{
    return wa_obj_op(&g_wa, id, OBJ_SVC_FILTER, true, ERR(&g_wa, g_wa.e_svc_filter));
}
static s32 wa1_DeleteExtdPushEventFilter(s32 lib, s32 id)
{
    return wa_obj_op(&g_wa, id, OBJ_EXTD_FILTER, true, ERR(&g_wa, g_wa.e_extd_filter));
}
/* every Register*PushEventCallback[A](uctx, filter, cb, arg): never invoked */
static s32 wa1_RegisterCallback(s32 uctx, s32 filter, const void *cb, void *arg)
{
    return wa_register_cb(&g_wa, uctx, filter, cb);
}
static s32 wa1_UnregisterCallback(s32 uctx, s32 id)
{
    wa_obj_op(&g_wa, id, OBJ_CALLBACK, true, 0);
    return 0;
}

/* ---- libSceNpWebApi2 ---- */

static s32 wa2_Initialize(s32 http, u64 pool) { return wa_init(&g_wa2, http, pool); }
static s32 wa2_Terminate(s32 lib) { return wa_term(&g_wa2, lib); }
static s32 wa2_CreateUserContext(s32 lib, s32 user) { return wa_create_user(&g_wa2, lib, user); }
static s32 wa2_DeleteUserContext(s32 uctx) { return wa_delete_user(&g_wa2, uctx); }
static s32 wa2_CreateRequest(s32 uctx, const char *grp, const char *path, const char *method,
                             const struct content_param *cp, s64 *out)
{
    return wa_create_req(&g_wa2, uctx, grp, path, method, cp, out);
}
static s32 wa2_CreateMultipartRequest(s32 uctx, const char *grp, const char *path, const char *method, s64 *out)
{
    return wa_create_req(&g_wa2, uctx, grp, path, method, NULL, out);
}
static s32 wa2_SendRequest(s64 req, const void *data, u64 size, struct resp_info *ri)
{
    return wa_send(&g_wa2, req, 0, false, data, ri);
}
static s32 wa2_SendMultipartRequest(s64 req, s32 part, const void *data, u64 size, struct resp_info *ri)
{
    return wa_send(&g_wa2, req, part, true, data, ri);
}
static s32 wa2_ReadData(s64 req, void *buf, u64 size) { return wa_read(&g_wa2, req, buf, size); }
static s32 wa2_GetHttpResponseHeaderValue(s64 req, const char *name, char *val, u64 size)
{
    return wa_header_value(&g_wa2, req, name, val, size);
}
static s32 wa2_GetHttpResponseHeaderValueLength(s64 req, const char *name, u64 *len)
{
    return wa_header_length(&g_wa2, req, name, len);
}
static s32 wa2_AddHttpRequestHeader(s64 req, const char *name, const char *value)
{
    return wa_add_header(&g_wa2, req, name, value);
}
static s32 wa2_AbortRequest(s64 req) { return wa_abort_delete(&g_wa2, req, false); }
static s32 wa2_DeleteRequest(s64 req) { return wa_abort_delete(&g_wa2, req, true); }
static s32 wa2_GetMemoryPoolStats(s32 lib, u64 *stats) { return wa_pool_stats(&g_wa2, lib, stats); }
static s32 wa2_CreateHandle(s32 lib) { return wa_create_obj(&g_wa2, lib, OBJ_HANDLE); }
static s32 wa2_DeleteHandle(s32 lib, s32 h)
{
    return wa_obj_op(&g_wa2, h, OBJ_HANDLE, true, ERR(&g_wa2, E_NO_HANDLE));
}
static s32 wa2_HandleOp(s32 lib, s32 h) { return wa_obj_op(&g_wa2, h, OBJ_HANDLE, false, ERR(&g_wa2, E_NO_HANDLE)); }
static s32 wa2_CreateFilter(s32 lib, s32 h, const char *svc, u32 label, const void *p, u64 n)
{
    return wa_create_filter(&g_wa2, lib, svc, label, p, n, OBJ_FILTER, true);
}
static s32 wa2_DeleteFilter(s32 lib, s32 id)
{
    return wa_obj_op(&g_wa2, id, OBJ_FILTER, true, ERR(&g_wa2, E_NO_FILTER));
}
static s32 wa2_RegisterCallback(s32 uctx, s32 filter, const void *cb, void *arg)
{
    return wa_register_cb(&g_wa2, uctx, filter, cb);
}
static s32 wa2_UnregisterCallback(s32 uctx, s32 id)
{
    return wa_obj_op(&g_wa2, id, OBJ_CALLBACK, true, ERR(&g_wa2, E_NO_CALLBACK));
}
static s32 wa2_CreatePushContext(s32 uctx, char *out /* SceNpWebApi2PushEventPushContextId: char[37] */)
{
    s32 r = 0;
    if (!out)
        return ERR(&g_wa2, E_ARG);
    LOCK();
    u32 slot = WA_PUSHCTX;
    for (u32 i = 0; i < WA_PUSHCTX && slot == WA_PUSHCTX; i++)
        if (!g_wa2.pctx[i].user)
            slot = i;
    if (!user_find(&g_wa2, uctx))
        r = ERR(&g_wa2, E_NO_USER);
    else if (slot == WA_PUSHCTX)
        r = ERR(&g_wa2, E_OOM);
    else {
        g_wa2.pctx[slot].user = uctx;
        g_wa2.pctx[slot].n = (u32)npo_next(&g_wa2.pctx_seq, 0x7FFFFFFF);
        pctx_name(g_wa2.pctx[slot].n, out);
    }
    UNLOCK();
    NPO_TRACE(r, "npo: sceNpWebApi2PushEventCreatePushContext(%x) -> %x %s", uctx, r, r ? "" : out);
    return r;
}
/* Delete or StartCallback on a push context: it must exist */
static s32 wa2_push_ctx_op(s32 uctx, const char *id, bool del)
{
    if (!id)
        return ERR(&g_wa2, E_ARG);
    s32 r = ERR(&g_wa2, g_wa2.e_push_ctx);
    char name[37];
    LOCK();
    for (u32 i = 0; i < WA_PUSHCTX && r; i++) {
        if (!g_wa2.pctx[i].user)
            continue;
        pctx_name(g_wa2.pctx[i].n, name);
        if (strncmp(name, id, 37) == 0) {
            r = 0;
            if (del)
                g_wa2.pctx[i].user = 0;
        }
    }
    UNLOCK();
    return r;
}
static s32 wa2_DeletePushContext(s32 uctx, const char *id) { return wa2_push_ctx_op(uctx, id, true); }
static s32 wa2_StartPushContextCallback(s32 uctx, const char *id) { return wa2_push_ctx_op(uctx, id, false); }

NPO_SMALL_END

static int np_webapi_init(void)
{
    g_wa.tag = "sceNpWebApi", g_wa.base = 0x80552900u;
    g_wa.e_lib_max = 0x1A, g_wa.e_user_max = 0x1B, g_wa.e_after_send = 0x1E;
    g_wa.e_svc_filter = 0x0E, g_wa.e_extd_filter = 0x1C, g_wa.e_push_ctx = E_ARG;
    g_wa2.tag = "sceNpWebApi2", g_wa2.base = 0x80553400u;
    g_wa2.e_lib_max = 0x18, g_wa2.e_user_max = 0x19, g_wa2.e_after_send = 0x1A;
    g_wa2.e_svc_filter = E_NO_FILTER, g_wa2.e_extd_filter = E_NO_FILTER, g_wa2.e_push_ctx = 0x1C;
    return 0;
}

#define W1(name, fn) {"libSceNpWebApi", "sceNpWebApi" #name, fn, NULL}
#define W2(name, fn) {"libSceNpWebApi2", "sceNpWebApi2" #name, fn, NULL}

static const struct ext_hle_ent k_hle[] = {
    W1(Initialize, wa1_Initialize),
    W1(InitializeForPresence, wa1_Initialize),
    W1(Terminate, wa1_Terminate),
    W1(CreateContextA, wa1_CreateContextA),
    W1(CreateContext, wa1_CreateContext),
    W1(DeleteContext, wa1_DeleteContext),
    W1(CreateRequest, wa1_CreateRequest),
    W1(CreateMultipartRequest, wa1_CreateMultipartRequest),
    W1(SendRequest, wa1_SendRequest),
    W1(SendRequest2, wa1_SendRequest2),
    W1(SendMultipartRequest, wa1_SendMultipartRequest),
    W1(SendMultipartRequest2, wa1_SendMultipartRequest2),
    W1(ReadData, wa1_ReadData),
    W1(GetHttpStatusCode, wa1_GetHttpStatusCode),
    W1(GetHttpResponseHeaderValue, wa1_GetHttpResponseHeaderValue),
    W1(GetHttpResponseHeaderValueLength, wa1_GetHttpResponseHeaderValueLength),
    W1(AddHttpRequestHeader, wa1_AddHttpRequestHeader),
    W1(SetMultipartContentType, ret0),
    W1(AddMultipartPart, wa1_AddMultipartPart),
    W1(AbortRequest, wa1_AbortRequest),
    W1(DeleteRequest, wa1_DeleteRequest),
    W1(SetRequestTimeout, ret0),
    W1(CreateHandle, wa1_CreateHandle),
    W1(DeleteHandle, wa1_DeleteHandle),
    W1(AbortHandle, wa1_HandleOp),
    W1(SetHandleTimeout, wa1_HandleOp),
    W1(CheckTimeout, ret0),
    W1(SetMaxConnection, ret0),
    W1(GetMemoryPoolStats, wa1_GetMemoryPoolStats),
    W1(GetConnectionStats, wa1_GetConnectionStats),
    W1(ClearUnusedConnection, ret0),
    W1(ClearAllUnusedConnection, ret0),
    W1(GetErrorCode, ret0),
    W1(UtilityParseNpId, wa1_UtilityParseNpId),
    W1(CreatePushEventFilter, wa1_CreatePushEventFilter),
    W1(CreateServicePushEventFilter, wa1_CreateServicePushEventFilter),
    W1(CreateExtdPushEventFilter, wa1_CreateExtdPushEventFilter),
    W1(DeletePushEventFilter, wa1_DeletePushEventFilter),
    W1(DeleteServicePushEventFilter, wa1_DeleteServicePushEventFilter),
    W1(DeleteExtdPushEventFilter, wa1_DeleteExtdPushEventFilter),
    W1(RegisterPushEventCallback, wa1_RegisterCallback),
    W1(RegisterServicePushEventCallback, wa1_RegisterCallback),
    W1(RegisterExtdPushEventCallback, wa1_RegisterCallback),
    W1(RegisterExtdPushEventCallbackA, wa1_RegisterCallback),
    W1(UnregisterPushEventCallback, wa1_UnregisterCallback),
    W1(UnregisterServicePushEventCallback, wa1_UnregisterCallback),
    W1(UnregisterExtdPushEventCallback, wa1_UnregisterCallback),
    W1(RegisterNotificationCallback, ret0),
    W1(UnregisterNotificationCallback, ret0),

    W2(Initialize, wa2_Initialize),
    W2(InitializeForPresence, wa2_Initialize),
    W2(Terminate, wa2_Terminate),
    W2(CreateUserContext, wa2_CreateUserContext),
    W2(DeleteUserContext, wa2_DeleteUserContext),
    W2(CreateRequest, wa2_CreateRequest),
    W2(CreateMultipartRequest, wa2_CreateMultipartRequest),
    W2(SendRequest, wa2_SendRequest),
    W2(SendMultipartRequest, wa2_SendMultipartRequest),
    W2(ReadData, wa2_ReadData),
    W2(GetHttpResponseHeaderValue, wa2_GetHttpResponseHeaderValue),
    W2(GetHttpResponseHeaderValueLength, wa2_GetHttpResponseHeaderValueLength),
    W2(AddHttpRequestHeader, wa2_AddHttpRequestHeader),
    W2(AddWebTraceTag, ret0),
    W2(SetMultipartContentType, ret0), /* signatures unconfirmed: no writes; a later send fails anyway */
    W2(AddMultipartPart, ret0),
    W2(AbortRequest, wa2_AbortRequest),
    W2(DeleteRequest, wa2_DeleteRequest),
    W2(SetRequestTimeout, ret0),
    W2(CheckTimeout, ret0),
    W2(GetMemoryPoolStats, wa2_GetMemoryPoolStats),
    W2(PushEventCreateHandle, wa2_CreateHandle),
    W2(PushEventDeleteHandle, wa2_DeleteHandle),
    W2(PushEventAbortHandle, wa2_HandleOp),
    W2(PushEventSetHandleTimeout, wa2_HandleOp),
    W2(PushEventCreateFilter, wa2_CreateFilter),
    W2(PushEventDeleteFilter, wa2_DeleteFilter),
    W2(PushEventRegisterCallback, wa2_RegisterCallback),
    W2(PushEventRegisterPushContextCallback, wa2_RegisterCallback),
    W2(PushEventUnregisterCallback, wa2_UnregisterCallback),
    W2(PushEventUnregisterPushContextCallback, wa2_UnregisterCallback),
    W2(PushEventCreatePushContext, wa2_CreatePushContext),
    W2(PushEventDeletePushContext, wa2_DeletePushContext),
    W2(PushEventStartPushContextCallback, wa2_StartPushContextCallback),
};

/* Everything else these libraries export (the Int* and Vsh* system entry
 * points, anything newer) gets a stub returning the OFF code, never SWRR's. */
static const struct ext_lib k_libs[] = {
    {"libSceNpWebApi", "libSceNpWebApiCompat", EXT_LIB_NO_HOST, NPO_OFF(0x80552907, NPO_ERR_NET)},
    {"libSceNpWebApi2", "libSceNpWebApi2AsyncRestricted", EXT_LIB_NO_HOST, NPO_OFF(0x80553407, NPO_ERR_NET)},
};

EXT_FAMILY_LIBS("np-webapi", k_hle, k_libs, np_webapi_init);
