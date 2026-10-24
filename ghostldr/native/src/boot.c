/* Load the title's modules and executable and start its main thread. */
#include "ghost.h"

void module_protect_relro(struct module *m);

struct module *g_main_mod;

static void ghost_exit_handler(void) { logf("title called its atexit handler"); }

static void *ghost_main_thread(void *unused)
{
    logf("main thread: starting %s", g_main_mod->name);
    modules_start_boot();
    /* argv, then an empty environment and auxiliary vector: musl's
     * __libc_start_main (OpenOrbis homebrew) reads on past argv's NULL */
    static struct {
        u64 argc;
        const char *argv[2];
        const char *envp[1];
        u64 auxv[2];
    } args = {1, {"/app0/eboot.bin", NULL}, {NULL}, {0, 0}};
    void (*entry)(void *, void (*)(void)) = (void *)(g_main_mod->base + g_main_mod->entry);
    logf("calling title entry %p", entry);
    entry(&args, ghost_exit_handler);
    logf("title entry returned");
    return NULL;
}

int ghost_boot(void)
{
    const char *root = (const char *)g_ctx->app_root;
    logf("boot: app root %s", root);
    crash_install();
    /* the extension carries the NID table: without it nothing resolves, so
     * stop before anything touches the title or the host's state */
    if (ext_fetch() != 0)
        return -7;
    int choose = 0; /* game selector: 1 when ghostfs or a library left the choice to it */
    bool library = false;
    if (strcmp(root, "ghostfs") == 0) {
        /* the title's files live in the host's save containers (no kernel exploit) */
        if ((choose = gfs_init()) < 0)
            return -6;
        root = "/app0";
    } else {
        /* a title's directory, as before; a USB drive or a folder of titles:
         * the extension decides (ext/src/approot.c) */
        char eboot[288];
        u8 st[0x78];
        fmt(eboot, sizeof(eboot), "%s/eboot.bin", root);
        if (strncmp(root, "usb:", 4) == 0 || vfs_stat(eboot, st) != 0) {
            if ((choose = ext_approot_init(&root)) < 0) {
                ext_abandon();
                return -6;
            }
            library = choose > 0;
        }
    }
    /* Jak X: the emulator threads UltraC0re leaves running (Luac0re stops SWRR's) */
    void host_stop_emulator_threads(void);
    host_stop_emulator_threads();
    {
        /* the loader's message dialog ("Remote Lua Loader", UltraC0re's
         * "Listening") is opened in the host and never terminated: while the
         * dialog library is up the system keeps the app's input intercepted
         * and reports UI as overlaid */
        void *host_real_in(const char *modname, const char *name);
        int (*upd)(void) = (int (*)(void))host_real_in("libSceMsgDialog", "sceMsgDialogUpdateStatus");
        int (*cls)(void) = (int (*)(void))host_real_in("libSceMsgDialog", "sceMsgDialogClose");
        int (*term)(void) = (int (*)(void))host_real_in("libSceMsgDialog", "sceMsgDialogTerminate");
        int st = upd ? upd() : -1;
        int c = (st == 2 /* RUNNING */ && cls) ? cls() : 0;
        int t = term ? term() : -1;
        logf("boot: %s message dialog status %d, close %x, terminate %x", g_hostgame->name, st, c, t);
    }
    if (g_ctx->flags & 2) {
        u64 host_release_emulator_memory(u64 min_size);
        host_release_emulator_memory(0x800000);
        void host_dmem_census(void);
        host_dmem_census();
    }
    /* ---- game selector (sel.c): it picks the title before its index is
     * loaded and its memory planned, and frees all it used first ---- */
    char title[16];
    int picked = sel_run(choose || (g_ctx->flags & CTX_FLAG_SELECT), title);
    if (choose && library && ext_approot_pick(picked >= 0 ? title : NULL, &root) != 0) {
        ext_abandon();
        return -6;
    }
    if (choose && !library && gfs_pick(picked >= 0 ? title : NULL) != 0)
        return -6;

    int rc = modules_load_boot(root, &g_main_mod);
    if (rc != 0)
        return rc;

    u64 code_lo, code_hi;
    /* a module the arena cannot hold gets whole JIT objects of its own
     * (bigexec.c), which leave the arena first; the selector is done by now */
    if (arena_init() != 0 || bx_prepare() != 0 || arena_code_region(&code_lo, &code_hi) != 0)
        return -3;
    /* bundled modules first, then the executable; its data may extend past
     * the code region (over the emulator's other, now unused, buffers) */
    for (struct module *m = g_modules; m; m = m->next)
        if (!m->is_main && module_place(m, m->bx ? 0 : arena_code_alloc(m->image_size)) != 0)
            return -3;
    if (module_place(g_main_mod, g_main_mod->bx ? 0 : arena_code_alloc(g_main_mod->image_size)) != 0)
        return -3;
    logf("boot: code region %lx-%lx, main at %lx, image end %lx", code_lo, code_hi, g_main_mod->base,
         g_main_mod->base + g_main_mod->image_size);
    if (ext_start() != 0)
        return -8;
    for (struct module *m = g_modules; m; m = m->next)
        module_finish(m);
    tls_setup(g_main_mod);
    for (struct module *m = g_modules; m; m = m->next)
        if (module_relocate(m) != 0)
            return -4;
    ext_boot_done();
    for (struct module *m = g_modules; m; m = m->next)
        if (module_write_text(m) != 0)
            return -5;
    stubs_flush();
    for (struct module *m = g_modules; m; m = m->next)
        module_protect_relro(m);

    /* the main thread as the proc param asks (+0x20 name, +0x28 priority,
     * +0x30 stack size; each a pointer): otherwise <TITLE>-main (profile.c),
     * the attribute's own priority and 8 MiB */
    const u64 *pp = (const u64 *)g_main_mod->proc_param;
    const char *name = pp && pp[0] > 0x20 ? (const char *)pp[4] : NULL;
    const u32 *prio = pp && pp[0] > 0x28 ? (const u32 *)pp[5] : NULL;
    const u32 *stack = pp && pp[0] > 0x30 ? (const u32 *)pp[6] : NULL;
    if (!name)
        name = profile_thread_name();
    u8 attr[0x40];
    u64 tid = 0;
    K.pthread_attr_init(attr);
    K.pthread_attr_setstacksize(attr, stack && *stack ? *stack : 8 << 20);
    if (prio && *prio >= 256 && *prio <= 767) {
        void *host_real(const char *name);
        int (*inherit)(void *, int) = host_real("scePthreadAttrSetinheritsched");
        int (*sched)(void *, const int *) = host_real("scePthreadAttrSetschedparam");
        int p = (int)*prio;
        if (inherit && sched)
            inherit(attr, 0 /* PTHREAD_EXPLICIT_SCHED */), sched(attr, &p);
    }
    logf("boot: main thread %s, stack %x, priority %d", name, stack && *stack ? *stack : 8 << 20,
         prio ? (int)*prio : -1);
    rc = ghost_thread_create(&tid, attr, ghost_main_thread, NULL, name);
    K.pthread_attr_destroy(attr);
    logf("boot: main thread created rc=%x", rc);
    if (g_ctx->flags & 4)
        crash_start_watchdog();
    return rc;
}
