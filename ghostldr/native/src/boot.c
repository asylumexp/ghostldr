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

/* Save tools (PF_SAVE_TOOL, docs/selector.md): the user whose saves the
 * title opens, as the game selector chose it; 0: the user running SWRR. */
u32 g_title_user;

/* The title the game selector chose, mounted once: by the selector itself
 * before a save tool's scan (ghost_api.title_pick), else after it returns.
 * g_choose: ghostfs or a library left the choice to the selector (0: the
 * title is mounted already). */
static int g_choose, g_pick_rc = 1; /* 1: not yet */
static bool g_library;
static const char *g_pick_root;

/* The title's app root (ctx app_root): "auto" (the games on a USB drive,
 * else the installed ones), "ghostfs", a title's directory, or a USB drive
 * or folder of titles (the extension decides).  0, or -6; *choose: 1 when
 * ghostfs or a library leaves the choice to the game selector. */
static int boot_root(const char **rootp, int *choose_out, bool *library_out)
{
    const char *root = *rootp;
    int choose = 0;
    bool library = false, decided = false; /* decided: "auto" found the title on a USB drive */
    if (strcmp(root, "auto") == 0) {
        /* the games on a USB drive, else the installed ones (run.py's
         * default); drives are untested on a PS4 and in Jak X */
        if (is_ps4() || is_jakx() || (choose = ext_approot_init(&root)) == 2) {
            if (choose == 2)
                ext_unplace();
            logf("boot: %s: the installed titles", is_ps4() || is_jakx() ? "no USB drives here" : "no game on a USB drive");
            root = "ghostfs";
            g_ctx->app_root = (u64)root; /* what the game selector reads */
            choose = 0;
        } else if (choose < 0) {
            ext_abandon();
            return -6;
        } else {
            library = choose > 0;
            decided = true;
        }
    }
    if (strcmp(root, "ghostfs") == 0) {
        /* the title's files live in the host's save containers (no kernel exploit) */
        if ((choose = gfs_init()) < 0)
            return -6;
        root = "/app0";
    } else if (!decided) {
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
    *rootp = root, *choose_out = choose, *library_out = library;
    return 0;
}

/* Before the game selector: what the host leaves running stops, and its
 * message dialog goes (else it keeps the app's input) */
static void boot_quiet_host(void)
{
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
}

static struct el_select *select_block(void)
{
    return g_ctx->size >= (CTX_SELECT_SLOT + 1) * 8 ? (struct el_select *)((u64 *)g_ctx)[CTX_SELECT_SLOT] : NULL;
}

static volatile int g_el_done, g_el_rc;

__attribute__((force_align_arg_pointer)) static void *elfldr_thread(void *unused)
{
    (void)unused;
    g_el_rc = ext_elfldr();
    __atomic_store_n(&g_el_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* The ELF loader beside the game selector (CTX_ELFLDR_SELECT): it serves on
 * a thread of its own while the selector shows.  Once a title is picked it
 * stops (after any payload still running), and the extension goes with its
 * executable memory; the pick is left in s for ghostldr.lua, which boots it
 * in a second call.  CTX_ELFLDR_PICKED, or < 0. */
static int elfldr_select(struct el_select *s)
{
    const char *root = (const char *)g_ctx->app_root;
    int choose = 0;
    bool library = false;
    s->index = -1;
    if (boot_root(&root, &choose, &library) != 0)
        return -6;
    boot_quiet_host();
    u64 lo, hi;
    if (arena_init() != 0 || arena_code_region(&lo, &hi) != 0 || ext_start() != 0) {
        ext_abandon();
        return -8;
    }
    /* sel.c places the selector's image here: no payload's code goes there */
    s->res_lo = ALIGN_UP(lo, 0x10000), s->res_hi = s->res_lo + g_ctx->staging_size;
    u64 tid = 0;
    g_el_done = 0;
    /* a plain thread, as the Lua loader's is when the ELF loader runs alone:
     * ghost_thread_create would give it a TLS block before the ELF loader
     * sizes them (tls_static_reserve), and payload threads' blocks after it
     * would come out of that pool at a bad address */
    if (K.pthread_create(&tid, NULL, elfldr_thread, NULL, "ghostldr-elfldr") != 0) {
        logf("elfldr: no thread for the ELF loader beside the selector");
        ext_abandon();
        return -12;
    }
    g_choose = choose, g_library = library;
    char title[16];
    int picked = sel_run(true, title);
    __atomic_store_n(&s->stop, 1, __ATOMIC_RELEASE);
    logf("elfldr: %s picked: the ELF loader stops once no payload runs", picked >= 0 ? title : "nothing");
    while (!__atomic_load_n(&g_el_done, __ATOMIC_ACQUIRE))
        K.usleep(50000);
    s->index = picked, s->user = g_title_user;
    memcpy(s->title, title, sizeof(s->title));
    gfs_unmount_all();
    ext_abandon();
    logf("elfldr: stopped (%d): the extension and its executable memory are given back", g_el_rc);
    return g_el_rc < 0 ? g_el_rc : CTX_ELFLDR_PICKED;
}

/* No title: the extension's ELF loader serves payloads until it is told to
 * quit (docs/elfldr.md).  It needs the NID table and the extension's code,
 * placed as for a title; then the call returns to Luac0re's loader with the
 * extension's JIT block given back. */
int ghost_elfldr(void)
{
    crash_install();
    if (is_ps4()) {
        logf("elfldr: a PS5 only (its executable memory comes from views of the emulator's JIT objects)");
        return -9;
    }
    if (ext_fetch() != 0)
        return -7;
    struct el_select *s = select_block();
    if ((g_ctx->flags & CTX_ELFLDR_SELECT) && s)
        return elfldr_select(s);
    int rc = arena_init() != 0 || ext_start() != 0 ? -8 : ext_elfldr();
    g_payload_fault = NULL;
    ext_abandon();
    logf("elfldr: back to the Lua loader (%d)", rc);
    return rc;
}

int boot_title_pick(const char *title, int index)
{
    if (g_pick_rc == 1)
        g_pick_rc = !g_choose ? 0
                    : g_library ? ext_approot_pick(title, index, &g_pick_root)
                    : gfs_pick(title) ? -1 : 0;
    return g_pick_rc;
}

int boot_save_prescan(u32 user, int (*progress)(void *arg, u32 done, u32 total), void *arg)
{
    return g_pick_rc ? -1 : ext_save_prescan(user, progress, arg);
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
    int rc = boot_root(&root, &choose, &library);
    if (rc != 0)
        return rc;
    boot_quiet_host();
    /* ---- game selector (sel.c): it picks the title before its index is
     * loaded and its memory planned, and frees all it used first ---- */
    char title[16];
    g_choose = choose, g_library = library;
    struct el_select *s = select_block();
    int picked;
    if (s && s->boot) { /* picked beside the ELF loader, in ghostldr.lua's first call (elfldr_select) */
        memcpy(title, s->title, sizeof(title));
        title[sizeof(title) - 1] = 0;
        picked = s->index, g_title_user = s->user;
        logf("boot: %s, picked in the game selector beside the ELF loader", picked >= 0 ? title : "no title");
    } else /* a save tool (Apollo) asks whose saves even when it is the only title */
        picked = sel_run(choose || (g_ctx->flags & CTX_FLAG_SELECT) || profile_has(PF_SAVE_TOOL), title);
    if (boot_title_pick(picked >= 0 ? title : NULL, picked) != 0) {
        if (library)
            ext_abandon();
        return -6;
    }
    if (choose && library)
        root = g_pick_root;
    if (g_title_user)
        logf("boot: saves of user %x", g_title_user);

    rc = modules_load_boot(root, &g_main_mod);
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
    ext_title_starting();
    rc = ghost_thread_create(&tid, attr, ghost_main_thread, NULL, name);
    K.pthread_attr_destroy(attr);
    logf("boot: main thread created rc=%x", rc);
    if (g_ctx->flags & 4)
        crash_start_watchdog();
    automem_start();
    return rc;
}
