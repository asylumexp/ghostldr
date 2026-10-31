/* Extension entry: collect every file's family into one table and answer
 * the core's lookups (by library and NID, through library aliases). */
#include "ext.h"

const struct ghost_api *G;

extern const struct ext_family *const __ext_families_start[] HIDDEN;
extern const struct ext_family *const __ext_families_end[] HIDDEN;

static struct ext_exports g_exports;
static struct ext_hle_ent *g_hle; /* registered entries, nid filled in */
static const char **g_canon;      /* canonical library of each entry */
static u32 g_nhle;
static const struct ext_lib **g_libs;
static u32 g_nlibs;
static bool *g_use; /* the families whose init succeeded */

#define FAMILIES(f) for (const struct ext_family *const *f = __ext_families_start; f < __ext_families_end; f++)

/* is lib one of the space-separated names in list? */
static bool in_list(const char *list, const char *lib)
{
    size_t n = strlen(lib);
    for (const char *p = list; p && *p;) {
        while (*p == ' ')
            p++;
        const char *e = p;
        while (*e && *e != ' ')
            e++;
        if ((size_t)(e - p) == n && n && strncmp(p, lib, n) == 0)
            return true;
        p = e;
    }
    return false;
}

static const struct ext_lib *policy_of(const char *lib)
{
    for (u32 i = 0; lib && i < g_nlibs; i++)
        if (strcmp(g_libs[i]->name, lib) == 0 || in_list(g_libs[i]->aliases, lib))
            return g_libs[i];
    return NULL;
}

static const char *canon(const char *lib)
{
    const struct ext_lib *p = policy_of(lib);
    return p ? p->name : lib;
}

static u64 ext_lookup(const char *lib, const char *nid)
{
    const char *c = lib ? canon(lib) : NULL;
    for (u32 i = 0; i < g_nhle; i++)
        if (strcmp(g_hle[i].nid, nid) == 0 && (!c || strcmp(g_canon[i], c) == 0))
            return (u64)g_hle[i].fn;
    return 0;
}

static const struct ext_lib *lib_policy(const char *lib) { return policy_of(lib); }

/* A sysmodule load the title asked for failed: from the core (hle.c), or from
 * a family that replaces sceSysmoduleLoadModule. */
void ext_sysmodule_failed(u32 id, s32 rc)
{
    u32 k = 0;
    FAMILIES(f)
    {
        if (g_use && g_use[k] && (*f)->sysmodule_failed)
            (*f)->sysmodule_failed(id, rc);
        k++;
    }
}
/* for the families (dlsym answers from these) */
u64 ext_find(const char *lib, const char *nid) { return ext_lookup(lib, nid); }
const struct ext_lib *ext_find_policy(const char *lib) { return policy_of(lib); }

static bool valid_nid(const char *nid)
{
    if (!nid || strlen(nid) != 11)
        return false;
    for (const char *c = nid; *c; c++)
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '+' ||
              *c == '-'))
            return false;
    return true;
}

static void add_policies(const struct ext_family *fam)
{
    for (u32 i = 0; i < fam->nlibs; i++) {
        const struct ext_lib *l = &fam->libs[i];
        const struct ext_lib *have = policy_of(l->name);
        if (have) {
            logf("ext: library %s declared again by family %s; the first declaration (%s) is used", l->name,
                 fam->name, have->name);
            continue;
        }
        g_libs[g_nlibs++] = l;
    }
}

static void add_functions(const struct ext_family *fam)
{
    for (u32 i = 0; i < fam->nhle; i++) {
        struct ext_hle_ent e = fam->hle[i];
        char *nid = arena_alloc(12);
        if (e.nid && valid_nid(e.nid))
            strlcpy(nid, e.nid, 12);
        else if (!e.nid && e.name)
            compute_nid(e.name, nid);
        else {
            logf("ext: family %s entry %u (%s) has no valid name or NID; ignored", fam->name, i,
                 e.name ? e.name : "?");
            continue;
        }
        e.nid = nid;
        const char *c = canon(e.lib);
        if (ext_lookup(c, nid)) {
            logf("ext: %s:%s registered twice; family %s's copy is ignored", e.lib, e.name ? e.name : nid,
                 fam->name);
            continue;
        }
        g_canon[g_nhle] = c;
        g_hle[g_nhle++] = e;
    }
}

/* Before the title's modules load (ext.c ext_approot_init): app roots on
 * USB drives and folders of titles (approot.c).  ext_init runs later as
 * always. */
extern const struct ext_early g_approot_early HIDDEN;
/* the title's main thread is next (ext_exports.title_starting) */
void ftp_title_starting(void) HIDDEN;
void net_pool_fill(void) HIDDEN; /* hle_net.c */
void usb_live_start(void) HIDDEN __attribute__((weak)); /* usb_live.c: not in the ELF loader's extension */

/* logs and failures (logfile.c, failure.c; not in the ELF loader's extension) */
const struct ext_early *diag_early(const struct ext_early *inner) HIDDEN __attribute__((weak));
void diag_title_starting(void) HIDDEN __attribute__((weak));
/* the title's main thread is next, on the boot thread: the FTP server, the
 * title's own sockets where SWRR makes none (hle_net.c), the failure
 * monitor (failure.c), and the watch on its USB drive and rest mode
 * (usb_live.c) */
static void title_starting(void)
{
    net_pool_fill();
    ftp_title_starting();
    if (diag_title_starting)
        diag_title_starting();
    if (usb_live_start)
        usb_live_start();
}
/* no title: the ELF loader (elfldr_sys.c) */
int elfldr_main(void) HIDDEN;
/* a module's text before it is written (ext_exports.patch_text, hle_rawsys.c) */
void rawsys_patch_text(struct module *m, u8 *text, u64 size) HIDDEN;

/* A memory plan's text patches (REC_PATCH: a size the executable holds as
 * an immediate rather than in a table), on the main executable of the
 * profile's own build: each only where its bytes are as expected. */
static void patch_text(struct module *m, u8 *text, u64 size)
{
    const struct prof_patch *p;
    for (u32 i = 0; m->is_main && G->profile_record && (p = G->profile_record(REC_PATCH, i)); i++) {
        const char *what = (const char *)p->bytes + 2 * p->len;
        if (p->offset + (u64)p->len > size || memcmp(text + p->offset, p->bytes, p->len) != 0) {
            logf("plan: %s: the text at %x is not as expected; not patched", what, p->offset);
            continue;
        }
        memcpy(text + p->offset, p->bytes + p->len, p->len);
        logf("plan: %s: %u bytes patched at %x", what, p->len, p->offset);
    }
    rawsys_patch_text(m, text, size);
}
/* drawn dialogs (dlg_overlay.c); the ELF loader's extension has none */
void ov_video_buffers(s32 handle, s32 start, const u64 *addrs, s32 num, const void *attr) HIDDEN __attribute__((weak));
void ov_pad_state(s32 handle, u8 *data) HIDDEN __attribute__((weak));
/* the watchdog's Ghost 2.18 reads (ghost_watch.c); not in the ELF loader's extension */
void ghost_watch(u64 eboot, u32 phase, u64 core_lo, u64 core_hi) HIDDEN __attribute__((weak));

/* back to the game selector (title_back.c); not in the ELF loader's extension */
int title_back(void) HIDDEN __attribute__((weak));

/* the planner's reclaim (reclaim.c); not in the ELF loader's extension */
int jit_reclaim(struct bx_reclaim *r) HIDDEN __attribute__((weak));

const struct ext_early *ext_early(const struct ghost_api *api)
{
    if (!api || api->version != GHOST_API_VERSION || api->size < sizeof(struct ghost_api))
        return NULL;
    G = api;
    *api->reclaim = jit_reclaim;
    return diag_early ? diag_early(&g_approot_early) : &g_approot_early;
}

const struct ext_exports *ext_init(const struct ghost_api *api)
{
    if (!api || api->version != GHOST_API_VERSION || api->size < sizeof(struct ghost_api))
        return NULL;
    G = api;
    u32 nhle = 0, nlibs = 0, nfam = 0, k = 0;
    FAMILIES(f) nhle += (*f)->nhle, nlibs += (*f)->nlibs, nfam++;
    g_hle = arena_alloc((nhle + 1) * sizeof(*g_hle));
    g_canon = arena_alloc((nhle + 1) * sizeof(*g_canon));
    g_libs = arena_alloc((nlibs + 1) * sizeof(*g_libs));
    bool *use = g_use = arena_alloc(nfam + 1);
    if (!g_hle || !g_canon || !g_libs || !use)
        return NULL;
    /* every family's init, then every policy (aliases decide how entries
     * match), then the functions */
    nfam = 0;
    FAMILIES(f)
    {
        const struct ext_family *fam = *f;
        use[k] = !fam->init || fam->init() == 0;
        if (!use[k])
            logf("ext: family %s failed to initialize; it is not used", fam->name);
        else
            add_policies(fam), nfam++;
        k++;
    }
    k = 0;
    FAMILIES(f)
    {
        if (use[k++])
            add_functions(*f);
    }
    g_exports = (struct ext_exports){EXT_EXPORTS_MAGIC, sizeof(g_exports), ext_lookup, lib_policy, g_hle,
                                     g_nhle, nfam, g_nlibs, ext_sysmodule_failed, title_starting, elfldr_main,
                                     patch_text, ov_video_buffers, ov_pad_state, ghost_watch, title_back};
    return &g_exports;
}
