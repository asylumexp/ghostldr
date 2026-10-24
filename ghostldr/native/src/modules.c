/*
 * The title's module set: which of its sce_module prx files load at boot, and
 * when each one starts (research/hle-specs/kernel-modules.md §6).
 *
 * Boot loads libSceFios2.prx and libc.prx (libkernel's preload list on
 * hardware) and every other sce_module file that exports a library the
 * executable, or a module already chosen, imports.  Imports bind at
 * relocation, so a module some import needs must be there by then; anything
 * else in sce_module loads on demand (loader.c module_load_runtime), and the
 * names of its imports are kept for that.
 *
 * A module some loaded file needs (DT_NEEDED) that isn't in sce_module and
 * isn't a system library (libSce*, libkernel*) is looked for anywhere else in
 * the title: Unity keeps mono-ps4.prx and MonoAssembliesPS4.prx in
 * Media/Modules.  Found ones are candidates like the sce_module files.
 *
 * Start order: libSceFios2, then libc, then the other boot modules in
 * dependency order, on the title's main thread before its entry.  IsGame
 * modules (NpToolkit2, ...) are marked MOD_DEFERRED by the extension's
 * sysmodule family and start at the title's sceSysmoduleLoadModule, as on
 * hardware.  A module starts through its DT_INIT (else e_entry):
 * int init(size_t argc, const void *argp, int (*start)(size_t, const void *)).
 */
#include "ghost.h"

#define COLD GHOST_COLD /* boot-time code: small beats fast */

#define MAX_CAND 48

static const char *const k_first[] = {"libSceFios2.prx", "libc.prx"};
/* only when sce_module cannot be listed */
static const char *const k_fallback[] = {
    "libSceFios2.prx", "libc.prx", "libSceNpToolkit2.prx", "libSceNpToolkit.prx",
    "libSceJson2.prx", "libSceJson.prx", "libSceNgs2.prx", "libSceAudioLatencyEstimation.prx",
};

COLD static bool is_first(const char *name)
{
    return strcmp(name, k_first[0]) == 0 || strcmp(name, k_first[1]) == 0;
}

/* does a import a library b exports? */
COLD static bool imports_from(const struct module *a, const struct module *b)
{
    for (u32 i = 0; i < a->n_import_libs; i++)
        for (u32 j = 0; j < b->n_export_libs; j++)
            if (strcmp(a->import_libs[i].name, b->export_libs[j].name) == 0)
                return true;
    return false;
}

/* sel[i]: candidate i loads at boot */
COLD u32 modules_select(struct module *const *cand, u32 n, const struct module *main, bool *sel)
{
    u32 count = 0;
    for (u32 i = 0; i < n; i++)
        count += sel[i] = is_first(cand[i]->name);
    for (bool more = true; more;) {
        more = false;
        for (u32 i = 0; i < n; i++) {
            bool need = !sel[i] && imports_from(main, cand[i]);
            for (u32 j = 0; j < n && !sel[i] && !need; j++)
                need = sel[j] && imports_from(cand[j], cand[i]);
            if (need)
                sel[i] = more = true, count++;
        }
    }
    return count;
}

COLD static u32 add_candidate(struct module **cand, u32 n, const char *dir, const char *name, int *fatal)
{
    char path[512];
    size_t len = strlen(name);
    if (n >= MAX_CAND || len < 5 || strcmp(name + len - 4, ".prx") != 0)
        return n;
    fmt(path, sizeof(path), "%s/%s", dir, name);
    struct module *m = module_open(path, false);
    if (!m) {
        if (is_first(name)) /* the title's libc or Fios2 is unusable: as before, no boot */
            *fatal = 1;
        return n;
    }
    /* sorted by name: the same order from ghostfs and from a directory */
    u32 at = n;
    while (at && strcmp(cand[at - 1]->name, m->name) > 0)
        cand[at] = cand[at - 1], at--;
    cand[at] = m;
    return n + 1;
}

/* where the title keeps a module it needs outside sce_module: anywhere in a
 * ghostfs title, else Unity's two directories */
COLD static int find_elsewhere(const char *root, const char *name, char *dir, size_t size)
{
    static const char *const k_dirs[] = {"Media/Modules", "Media/Plugins"};
    int rc = gfs_find_file(name, dir, size); /* 1: no ghostfs index to look in */
    if (rc <= 0)
        return rc;
    u8 st[0x78];
    char path[512];
    for (u32 i = 0; i < ARRAY_SIZE(k_dirs); i++) {
        fmt(dir, size, "%s/%s", root, k_dirs[i]);
        fmt(path, sizeof(path), "%s/%s", dir, name);
        if (vfs_stat(path, st) == 0)
            return 0;
    }
    return -1;
}

/* the files m needs that the title ships outside sce_module, as candidates */
COLD static u32 add_needed(struct module **cand, u32 n, const struct module *m, const char *root, int *fatal)
{
    for (u32 k = 0; k < m->n_needed_files; k++) {
        const char *f = m->needed_files[k];
        bool have = strncmp(f, "libSce", 6) == 0 || strncmp(f, "libkernel", 9) == 0;
        for (u32 i = 0; i < n && !have; i++)
            have = strcmp(cand[i]->name, f) == 0;
        char dir[512];
        if (!have && find_elsewhere(root, f, dir, sizeof(dir)) == 0) {
            logf("boot: %s needs %s: found in %s", m->name, f, dir);
            n = add_candidate(cand, n, dir, f, fatal);
        }
    }
    return n;
}

COLD int modules_load_boot(const char *root, struct module **main_out)
{
    char dir[512];
    struct module *cand[MAX_CAND];
    bool sel[MAX_CAND];
    u32 n = 0;
    int fatal = 0;
    fmt(dir, sizeof(dir), "%s/sce_module", root);
    int fd = vfs_open(dir, O_RDONLY, 0);
    if (fd >= 0) {
        u8 buf[1024];
        int got;
        while ((got = vfs_getdents(fd, buf, sizeof(buf))) > 0)
            for (int off = 0; off + 8 < got;) {
                const u8 *d = buf + off;
                u16 reclen = *(const u16 *)(d + 4);
                if (!reclen)
                    break;
                if (d[6] != 4) /* not DT_DIR */
                    n = add_candidate(cand, n, dir, (const char *)d + 8, &fatal);
                off += reclen;
            }
        vfs_close(fd);
    }
    if (!n && !fatal) {
        logf("boot: nothing listed in %s (%x): trying the usual files", dir, fd);
        u8 st[0x78];
        char path[512];
        for (u32 i = 0; i < ARRAY_SIZE(k_fallback); i++) {
            fmt(path, sizeof(path), "%s/%s", dir, k_fallback[i]);
            if (vfs_stat(path, st) == 0)
                n = add_candidate(cand, n, dir, k_fallback[i], &fatal);
        }
    }
    if (fatal)
        return -1;
    char path[512];
    fmt(path, sizeof(path), "%s/eboot.bin", root);
    struct module *main = module_open(path, true);
    if (!main)
        return -2;
    for (u32 prev = ~0u; prev != n && !fatal;) {
        prev = n;
        n = add_needed(cand, n, main, root, &fatal);
        for (u32 i = 0; i < n && !fatal; i++)
            n = add_needed(cand, n, cand[i], root, &fatal);
    }
    if (fatal)
        return -1;
    u32 count = modules_select(cand, n, main, sel);
    logf("boot: %u of %u title modules load now", count, n);
    for (u32 k = 0; k < ARRAY_SIZE(k_first); k++)
        for (u32 i = 0; i < n; i++)
            if (strcmp(cand[i]->name, k_first[k]) == 0)
                module_publish(cand[i]);
    for (u32 i = 0; i < n; i++) {
        struct module *m = cand[i];
        if (sel[i] && !is_first(m->name)) {
            module_publish(m);
        } else if (!sel[i]) {
            /* loads on demand: keep the names its host imports resolve by */
            for (u64 s = 0; s < m->nsyms; s++) {
                char nid[12];
                if (m->symtab[s].shndx || !m->symtab[s].name)
                    continue;
                strlcpy(nid, module_sym_name(m, &m->symtab[s]), sizeof(nid));
                nid_keep(nid);
            }
            logf("boot: %s loads on demand (nothing imports its libraries)", m->name);
            page_free(m->file, m->file_size);
        }
    }
    module_publish(main);
    *main_out = main;
    return 0;
}

COLD int module_start(struct module *m, u64 argc, const void *argp)
{
    if (!m || m->is_main)
        return 0;
    u8 st = __atomic_load_n(&m->start_state, __ATOMIC_ACQUIRE);
    if (st == MOD_STARTED)
        return m->start_rc;
    /* at most once; a start already running (another thread, or the
     * module's own init asking for itself) is not waited for */
    if (st == MOD_STARTING ||
        !__atomic_compare_exchange_n(&m->start_state, &st, MOD_STARTING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 0;
    if (m->has_init && m->init != m->entry)
        logf("%s: DT_INIT %lx differs from e_entry %lx: starting through DT_INIT", m->name, m->init, m->entry);
    int (*start)(u64, const void *, void *) = (void *)(m->base + (m->has_init ? m->init : m->entry));
    logf("module_start %s at %p", m->name, start);
    int rc = start(argc, argp, NULL);
    m->start_rc = rc;
    __atomic_store_n(&m->start_state, MOD_STARTED, __ATOMIC_RELEASE);
    logf("module_start %s -> %x", m->name, rc);
    return rc;
}

/* a module after the title modules it needs (DT_SCE_NEEDED_MODULE) */
COLD static void start_tree(struct module *m, u32 depth)
{
    if (m->is_main || m->start_state != MOD_BOOT || depth > 16)
        return;
    for (u32 i = 0; i < m->n_needed_mods; i++)
        for (struct module *o = g_modules; o; o = o->next)
            if (o != m && o->modname && strcmp(o->modname, m->needed_mods[i].name) == 0)
                start_tree(o, depth + 1);
    module_start(m, 0, NULL);
}

COLD void modules_start_boot(void)
{
    for (u32 k = 0; k < ARRAY_SIZE(k_first); k++)
        for (struct module *m = g_modules; m; m = m->next)
            if (!m->is_main && m->start_state == MOD_BOOT && strcmp(m->name, k_first[k]) == 0)
                module_start(m, 0, NULL);
    for (struct module *m = g_modules; m; m = m->next) {
        if (m->start_state == MOD_DEFERRED)
            logf("module_start %s waits for the title's sceSysmoduleLoadModule", m->name);
        start_tree(m, 0);
    }
}
