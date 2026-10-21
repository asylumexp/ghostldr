/* Load the title's modules and executable and start its main thread. */
#include "ghost.h"

int module_write_text(struct module *m);
void module_protect_relro(struct module *m);
void stubs_flush(void);

static const char *const BUNDLED[] = {
    "libSceFios2.prx", "libc.prx", "libSceNpToolkit2.prx", "libSceNpToolkit.prx",
    "libSceJson2.prx", "libSceJson.prx", "libSceNgs2.prx", "libSceAudioLatencyEstimation.prx",
};

struct module *g_main_mod;

static void ghost_exit_handler(void) { logf("title called its atexit handler"); }

static bool file_exists(const char *path)
{
    u8 st[0x80];
    return vfs_stat(path, st) == 0;
}

static void *ghost_main_thread(void *unused)
{
    logf("main thread: starting %s", g_main_mod->name);
    for (struct module *m = g_modules; m; m = m->next) {
        if (m->is_main)
            continue;
        int (*start)(u64, u64, u64) = (void *)(m->base + m->entry);
        logf("module_start %s at %p", m->name, start);
        int rc = start(0, 0, 0);
        logf("module_start %s -> %x", m->name, rc);
    }
    static struct {
        u64 argc;
        const char *argv[3];
    } args = {1, {"/app0/eboot.bin", NULL, NULL}};
    void (*entry)(void *, void (*)(void)) = (void *)(g_main_mod->base + g_main_mod->entry);
    logf("calling title entry %p", entry);
    entry(&args, ghost_exit_handler);
    logf("title entry returned");
    return NULL;
}

int ghost_boot(void)
{
    const char *root = (const char *)g_ctx->app_root;
    char path[512];
    logf("boot: app root %s", root);
    crash_install();
    if (strcmp(root, "ghostfs") == 0) {
        /* the title's files live in SWRR save containers (no kernel exploit) */
        if (gfs_init() != 0)
            return -6;
        root = "/app0";
    }
    {
        /* Luac0re's "Remote Lua Loader" message dialog is opened in SWRR and
         * never terminated: while the dialog library is up the system keeps
         * the app's input intercepted and reports UI as overlaid */
        void *host_real_in(const char *modname, const char *name);
        int (*upd)(void) = (int (*)(void))host_real_in("libSceMsgDialog", "sceMsgDialogUpdateStatus");
        int (*cls)(void) = (int (*)(void))host_real_in("libSceMsgDialog", "sceMsgDialogClose");
        int (*term)(void) = (int (*)(void))host_real_in("libSceMsgDialog", "sceMsgDialogTerminate");
        int st = upd ? upd() : -1;
        int c = (st == 2 /* RUNNING */ && cls) ? cls() : 0;
        int t = term ? term() : -1;
        logf("boot: SWRR message dialog status %d, close %x, terminate %x", st, c, t);
    }
    if (g_ctx->flags & 2) {
        u64 host_release_emulator_memory(u64 min_size);
        host_release_emulator_memory(0x800000);
        void host_dmem_census(void);
        host_dmem_census();
    }

    for (u32 i = 0; i < ARRAY_SIZE(BUNDLED); i++) {
        fmt(path, sizeof(path), "%s/sce_module/%s", root, BUNDLED[i]);
        if (file_exists(path) && !module_load_file(path, false))
            return -1;
    }
    fmt(path, sizeof(path), "%s/eboot.bin", root);
    g_main_mod = module_load_file(path, true);
    if (!g_main_mod)
        return -2;

    u64 code_lo, code_hi;
    if (arena_init() != 0 || arena_code_region(&code_lo, &code_hi) != 0)
        return -3;
    /* bundled modules first, then the executable; its data may extend past
     * the code region (over the emulator's other, now unused, buffers) */
    u64 cursor = ALIGN_UP(code_lo, 0x10000);
    for (struct module *m = g_modules; m; m = m->next) {
        if (m->is_main)
            continue;
        if (module_place(m, cursor) != 0)
            return -3;
        cursor = ALIGN_UP(cursor + m->image_size, 0x10000);
    }
    if (module_place(g_main_mod, cursor) != 0)
        return -3;
    logf("boot: code region %lx-%lx, main at %lx, image end %lx", code_lo, code_hi, cursor,
         cursor + g_main_mod->image_size);
    for (struct module *m = g_modules; m; m = m->next)
        module_finish(m);
    tls_setup(g_main_mod);
    for (struct module *m = g_modules; m; m = m->next)
        if (module_relocate(m) != 0)
            return -4;
    for (struct module *m = g_modules; m; m = m->next)
        if (module_write_text(m) != 0)
            return -5;
    stubs_flush();
    for (struct module *m = g_modules; m; m = m->next)
        module_protect_relro(m);

    u8 attr[0x40];
    u64 tid = 0;
    K.pthread_attr_init(attr);
    K.pthread_attr_setstacksize(attr, 8 << 20);
    int rc = ghost_thread_create(&tid, attr, ghost_main_thread, NULL, profile_thread_name()); /* <TITLE>-main */
    K.pthread_attr_destroy(attr);
    logf("boot: main thread created rc=%x", rc);
    if (g_ctx->flags & 4)
        crash_start_watchdog();
    return rc;
}
