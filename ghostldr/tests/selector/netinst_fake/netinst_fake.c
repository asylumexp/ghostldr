/*
 * The fake network installer (see netinst.h): no globals, no relocations,
 * as the selector module requires.  Its state is in memory from the core's
 * page_alloc.  It waits for 4 polls (one a second at the selector's 250 ms),
 * installs for 6 more, then reports one title installed; the harness learns
 * of the install from a mount of the container NETFAKEINSTALLED (it then
 * serves the catalogue that has the new title: host_env.c).
 */
#include "ghost_api.h"
#include "netinst.h"

struct fake {
    const struct ghost_api *api;
    unsigned polls, installed, stopped;
};

static void put(char *d, const char *s, int cap)
{
    int i = 0;
    for (; s[i] && i + 1 < cap; i++)
        d[i] = s[i];
    d[i] = 0;
}

int netinst_start(const struct ghost_api *api, void **handle)
{
    struct fake *f = api->page_alloc(sizeof(*f));
    if (!f)
        return -1;
    f->api = api;
    *handle = f;
    api->logf("netinst_fake: started");
    return 0;
}

int netinst_status(void *handle, struct netinst_status *st)
{
    struct fake *f = handle;
    if (!f || st->size != sizeof(*st))
        return -1;
    unsigned p = ++f->polls;
    if (p == 11 && !f->installed) {
        char mp[16];
        f->installed = 1;
        f->api->gfs_mount("NETFAKEINSTALLED", 1, 0, mp); /* tells the harness */
    }
    st->state = p > 4 && p <= 10 ? NETINST_BUSY : NETINST_WAITING;
    put(st->url, "http://192.168.0.20:8090/", sizeof(st->url));
    put(st->token, "K7QF-3MXP", sizeof(st->token));
    put(st->title, st->state == NETINST_BUSY ? "CUSA00042" : "", sizeof(st->title));
    put(st->name, st->state == NETINST_BUSY ? "Skyward Harbour" : "", sizeof(st->name));
    st->total = st->state == NETINST_BUSY ? 6000000 : 0;
    st->done = st->state == NETINST_BUSY ? (p - 4) * 1000000ull : 0;
    st->rate_kib = st->state == NETINST_BUSY ? 1000 : 0;
    st->installed = f->installed;
    if (st->size >= sizeof(*st))
        st->changes = f->installed;
    put(st->message, f->installed ? "Skyward Harbour installed" : "Waiting for a browser", sizeof(st->message));
    return 0;
}

void netinst_stop(void **handle)
{
    struct fake *f = *handle;
    if (f) {
        f->api->logf("netinst_fake: stopped after %u polls", f->polls);
        f->api->page_free(f, sizeof(*f));
    }
    *handle = 0;
}
