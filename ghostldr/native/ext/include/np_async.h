/*
 * NP request tables: the create / operation / poll / wait / abort / delete
 * pattern shared by NpManager, NpAuth and the NpUtility community APIs
 * (np-core-trophy spec §5), and usable by any other request-style NP library.
 *
 * A request ID is base + slot (the lowest free slot is reused).  Slots go
 * FREE -> CREATED (sync or async) -> DONE (result) | ABORTED, and back to FREE
 * on delete.  There is no I/O: an operation computes its result at once, so a
 * poll never reports "running".
 *
 *     s32 rc = npreq_check(&t, req);           unknown, aborted, already run
 *     if (rc == 0 && bad_args) rc = t.err.bad_arg;
 *     if (rc == 0) rc = npreq_finish(&t, req, result);   sync: result; async: 0
 *
 * Title contexts (community libraries) use a table of their own: create
 * returns the ID, delete frees it, and npreq_delete_owned drops the requests
 * that were created on the context.
 */
#pragma once
#include "ext.h"

#define NPREQ_MAX 32

struct npreq_errs {
    s32 max;        /* the table is full */
    s32 not_found;  /* no such request */
    s32 invalid_id; /* poll on a sync request or one with no operation; a second operation */
    s32 aborted;    /* operation on an aborted request */
    s32 bad_arg;    /* NULL parameter */
    s32 bad_size;   /* parameter size field */
};

struct npreq_slot {
    u8 state, async;
    s32 result, owner;
};

struct npreq_table {
    const char *name; /* for logs */
    s32 base;         /* ID of slot 0 */
    u32 limit;        /* slots used, at most NPREQ_MAX */
    struct npreq_errs err;
    volatile int lock;
    struct npreq_slot slot[NPREQ_MAX];
};

/* static struct npreq_table t = NPREQ_TABLE("np", 0x20000001, 32, max, not_found, invalid_id, aborted, bad_arg, bad_size); */
#define NPREQ_TABLE(name_, base_, limit_, ...) {.name = name_, .base = base_, .limit = limit_, .err = {__VA_ARGS__}}

s32 npreq_create(struct npreq_table *t, bool async, s32 owner);
/* param points at a structure whose first u64 is its size, which must be size */
s32 npreq_create_async(struct npreq_table *t, const void *param, u64 size, s32 owner);
bool npreq_exists(struct npreq_table *t, s32 id);
s32 npreq_check(struct npreq_table *t, s32 id);
s32 npreq_finish(struct npreq_table *t, s32 id, s32 result);
s32 npreq_poll(struct npreq_table *t, s32 id, s32 *result); /* also WaitAsync: it never blocks */
s32 npreq_abort(struct npreq_table *t, s32 id);
s32 npreq_delete(struct npreq_table *t, s32 id);
s32 npreq_set_timeout(struct npreq_table *t, s32 id);
u32 npreq_delete_owned(struct npreq_table *t, s32 owner);
