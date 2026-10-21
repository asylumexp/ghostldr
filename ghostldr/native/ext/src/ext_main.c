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
    bool *use = arena_alloc(nfam + 1);
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
                                     g_nhle, nfam, g_nlibs};
    return &g_exports;
}
