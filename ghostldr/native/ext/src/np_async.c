/* NP request tables (np_async.h): a helper, not a family. */
#include "np_async.h"

/* none of this is hot: optimize for size (the extension shares a 48 KiB area) */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

enum { REQ_FREE, REQ_CREATED, REQ_DONE, REQ_ABORTED };

static struct npreq_slot *slot_of(struct npreq_table *t, s32 id)
{
    s64 i = (s64)id - t->base;
    if (i < 0 || i >= (s64)t->limit || t->slot[i].state == REQ_FREE)
        return NULL;
    return &t->slot[i];
}

s32 npreq_create(struct npreq_table *t, bool async, s32 owner)
{
    s32 id = t->err.max;
    spin_lock(&t->lock);
    for (u32 i = 0; i < t->limit && i < NPREQ_MAX; i++)
        if (t->slot[i].state == REQ_FREE) {
            t->slot[i] = (struct npreq_slot){REQ_CREATED, async, 0, owner};
            id = t->base + (s32)i;
            break;
        }
    spin_unlock(&t->lock);
    if (id == t->err.max)
        logf("%s: no free request (limit %u) -> %x", t->name, t->limit, (u32)id);
    return id;
}

s32 npreq_create_async(struct npreq_table *t, const void *param, u64 size, s32 owner)
{
    if (!param)
        return t->err.bad_arg;
    if (*(const u64 *)param != size)
        return t->err.bad_size;
    return npreq_create(t, true, owner);
}

bool npreq_exists(struct npreq_table *t, s32 id)
{
    spin_lock(&t->lock);
    bool ok = slot_of(t, id) != NULL;
    spin_unlock(&t->lock);
    return ok;
}

static s32 check_locked(struct npreq_table *t, struct npreq_slot *s)
{
    if (!s)
        return t->err.not_found;
    if (s->state == REQ_ABORTED)
        return t->err.aborted;
    if (s->state != REQ_CREATED)
        return t->err.invalid_id; /* one operation per request */
    return 0;
}

s32 npreq_check(struct npreq_table *t, s32 id)
{
    spin_lock(&t->lock);
    s32 rc = check_locked(t, slot_of(t, id));
    spin_unlock(&t->lock);
    return rc;
}

s32 npreq_finish(struct npreq_table *t, s32 id, s32 result)
{
    spin_lock(&t->lock);
    struct npreq_slot *s = slot_of(t, id);
    s32 rc = check_locked(t, s);
    if (rc == 0) {
        s->state = REQ_DONE;
        s->result = result;
        rc = s->async ? 0 : result;
    }
    spin_unlock(&t->lock);
    return rc;
}

s32 npreq_poll(struct npreq_table *t, s32 id, s32 *result)
{
    if (!result)
        return t->err.bad_arg;
    spin_lock(&t->lock);
    struct npreq_slot *s = slot_of(t, id);
    s32 rc = 0;
    if (!s)
        rc = t->err.not_found;
    else if (!s->async || s->state != REQ_DONE)
        rc = t->err.invalid_id;
    else
        *result = s->result; /* 0: finished; "running" (1) is never reported */
    spin_unlock(&t->lock);
    return rc;
}

s32 npreq_abort(struct npreq_table *t, s32 id)
{
    spin_lock(&t->lock);
    struct npreq_slot *s = slot_of(t, id);
    s32 rc = s ? 0 : t->err.not_found;
    if (s && s->state != REQ_DONE)
        s->state = REQ_ABORTED;
    spin_unlock(&t->lock);
    return rc;
}

s32 npreq_delete(struct npreq_table *t, s32 id)
{
    spin_lock(&t->lock);
    struct npreq_slot *s = slot_of(t, id);
    s32 rc = s ? 0 : t->err.not_found;
    if (s)
        s->state = REQ_FREE;
    spin_unlock(&t->lock);
    return rc;
}

s32 npreq_set_timeout(struct npreq_table *t, s32 id)
{
    if (id <= 0)
        return t->err.bad_arg;
    return npreq_exists(t, id) ? 0 : t->err.not_found;
}

u32 npreq_delete_owned(struct npreq_table *t, s32 owner)
{
    u32 n = 0;
    spin_lock(&t->lock);
    for (u32 i = 0; i < t->limit && i < NPREQ_MAX; i++)
        if (t->slot[i].state != REQ_FREE && t->slot[i].owner == owner)
            t->slot[i].state = REQ_FREE, n++;
    spin_unlock(&t->lock);
    return n;
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
