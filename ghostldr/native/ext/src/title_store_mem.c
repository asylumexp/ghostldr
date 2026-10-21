/*
 * Stand-in title store (title_store.h) until the save data family's is
 * merged: records are kept in memory only and are lost when SWRR exits.
 * Only read and write, and both weak, so the save data family's definitions
 * replace them when it is linked in; delete this file then.
 */
#include "ext.h"
#include "title_store.h"

/* none of this is hot: optimize for size (the extension shares a 48 KiB area) */
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

#define MEM_RECORDS 8

static struct {
    char name[TITLE_STORE_NAME_MAX + 1];
    u8 *data;
    u64 size, cap;
} g_records[MEM_RECORDS];
static volatile int g_lock;

static int find(const char *name)
{
    for (int i = 0; i < MEM_RECORDS; i++)
        if (g_records[i].data && strcmp(g_records[i].name, name) == 0)
            return i;
    return -1;
}

__attribute__((weak)) s64 title_store_read(const char *name, void *buf, u64 len)
{
    if (!name || !buf)
        return TITLE_STORE_E_INVALID;
    spin_lock(&g_lock);
    int i = find(name);
    s64 n = i < 0 ? TITLE_STORE_E_NOT_FOUND : (s64)(g_records[i].size < len ? g_records[i].size : len);
    if (i >= 0)
        memcpy(buf, g_records[i].data, (u64)n);
    spin_unlock(&g_lock);
    return n;
}

__attribute__((weak)) s32 title_store_write(const char *name, const void *buf, u64 len)
{
    if (!name || !*name || strlen(name) > TITLE_STORE_NAME_MAX || (!buf && len) || len > TITLE_STORE_MAX)
        return TITLE_STORE_E_INVALID;
    spin_lock(&g_lock);
    int i = find(name);
    bool first = i < 0;
    for (int k = 0; i < 0 && k < MEM_RECORDS; k++)
        if (!g_records[k].data)
            i = k;
    if (i >= 0 && g_records[i].cap < len + 1) {
        u8 *p = arena_alloc(len + 1); /* the arena is never freed: records rarely grow */
        if (p)
            g_records[i].data = p, g_records[i].cap = len + 1;
    }
    s32 rc = TITLE_STORE_E_NO_SPACE;
    if (i >= 0 && g_records[i].data && g_records[i].cap >= len + 1) {
        strlcpy(g_records[i].name, name, sizeof(g_records[i].name));
        if (len)
            memcpy(g_records[i].data, buf, len);
        g_records[i].size = len;
        rc = 0;
    }
    spin_unlock(&g_lock);
    if (first)
        logf("title store: no persistent store in this build: %s (%lu bytes) is kept in memory only and lost "
             "when SWRR exits (rc %x)", name, len, (u32)rc);
    return rc;
}

#pragma clang attribute pop
