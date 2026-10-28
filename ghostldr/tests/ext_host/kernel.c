/*
 * kernel_host: the core's module loader and TLS (loader.c, modules.c, tls.c)
 * on a synthetic title test_ext.py builds (orbis_build.kernel_title), and the
 * extension's kernel, module, sysmodule and file families called through the
 * extension's own lookup, as the title's imports would reach them.
 *
 *     kernel_host EXT_BIN CRC APP0
 *
 * env.c's harness is included with its main and API table renamed and
 * without its module_export and call_gadget6 (ENV_REAL_LOADER): loader.c has
 * the real module_export, and the table here adds the services these
 * families use.  The boot loads and places the
 * modules before ext_start (the sysmodule family's init then defers IsGame
 * starts), and the tests run after it.  tls.c is built with
 * TLS_SLOT=g_tls_slot: a variable stands in for fs:[-8].
 */
#define ENV_REAL_LOADER
#define main ext_host_env_main
#define g_ghost_api g_ghost_api_env
#include "env.c"
#undef main
#undef g_ghost_api

/* calls through, as the eboot gadget does on the console */
u64 call_gadget6(u64 fn, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6)
{
    return fn ? ((u64(*)(u64, u64, u64, u64, u64, u64))fn)(a1, a2, a3, a4, a5, a6) : (u64)-1;
}

int sys_open_path(const char *path);
long sys_seek(int fd, long off, int whence);
int sys_kind(const char *path);
int sys_getdents(int fd, void *buf, int size);

#define E_NOENT ((s32)0x80020002)
#define E_SRCH ((s32)0x80020003)
#define E_NOEXEC ((s32)0x80020008)
#define E_BADF ((s32)0x80020009)
#define E_NOMEM ((s32)0x8002000C)
#define E_ACCES ((s32)0x8002000D)
#define E_INVAL ((s32)0x80020016)
#define SYSMOD_INVALID ((s32)0x805A1000)
#define SYSMOD_UNLOADED ((s32)0x805A1001)

/* ---- the core the loader reaches, faked ---- */
static char g_root[512]; /* the synthetic app0: /app0 maps here */

int vfs_open(const char *path, int flags, int mode)
{
    int fd = sys_open_path(path);
    return fd < 0 ? E_NOENT : fd;
}
s64 vfs_read(int fd, void *buf, u64 n) { return sys_read(fd, buf, n); }
s64 vfs_lseek(int fd, s64 off, int whence) { return sys_seek(fd, off, whence); }
int vfs_close(int fd) { return sys_close(fd); }
int vfs_stat(const char *path, void *st) { return sys_kind(path) < 0 ? E_NOENT : 0; }
int vfs_getdents(int fd, void *buf, int size) { return sys_getdents(fd, buf, size); }

const char *remap_path(const char *path, char *buf, size_t size)
{
    if (g_root[0] && strncmp(path, "/app0", 5) == 0 && (path[5] == '/' || !path[5])) {
        fmt(buf, size, "%s%s", g_root, path + 5);
        return buf;
    }
    return path;
}

/* the code region: executable, except while the fake compiler writes it */
#define CODE_SIZE (16UL << 20)
static u64 g_code, g_code_cur;
u64 g_punched;
u64 arena_code_alloc(u64 size)
{
    u64 at = ALIGN_UP(g_code_cur, 0x10000);
    g_code_cur = at + size;
    return at;
}
bool arena_is_rx(u64 addr, u64 size) { return addr >= g_code && addr + size <= g_code + CODE_SIZE; }
int budget_patch_text(struct module *m, u8 *text) { return 0; }
void hooks_patch_text(struct module *m, u8 *text) {}
void spin_lock(volatile int *l)
{
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        ;
}
void spin_unlock(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }
void *direct_alloc(size_t size, int prot, u64 fixed) { return sys_map(ALIGN_UP(size, 0x4000)); }
void direct_free(void *addr) {}
u64 g_tls_slot; /* tls.c's fs:[-8] */
#ifdef __APPLE__
/* tls.c's thunk_entry calls the unprefixed C name */
__asm__(".globl tls_ensure_thread\ntls_ensure_thread:\n    jmp _tls_ensure_thread\n");
#endif

/* the title profile's TLS site list for eboot.bin: the relaxed mov at 0x13
 * (patched), the fs:[0x30] load at 0x30 (refused); the add at 0x20 is left
 * out, so it stays unpatched */
static u64 g_tls_list[(sizeof(struct prof_tls) + 8 + 7) / 8];
const void *profile_record(u32 kind, u32 n)
{
    struct prof_tls *t = (struct prof_tls *)g_tls_list;
    if (kind != REC_TLS || n)
        return NULL;
    t->r.kind = REC_TLS, t->r.size = sizeof(g_tls_list);
    strlcpy(t->module, "eboot.bin", sizeof(t->module));
    t->count = 2, t->off[0] = 0x13, t->off[1] = 0x30;
    return t;
}
static int g_errno;
static int *k_errno(void) { return &g_errno; }
static int k_usleep(u32 us) { return 0; }
static int k_mprotect(const void *addr, size_t len, int prot) { return 0; }

/* ghostfs: paths under /app0/gfs are its files, fds 1000-1009 */
#define GFS_FD 1000
static const char k_gfs_data[] = "0123456789abcdefghijklmnopqrstuvwxyz";
static u64 g_gfs_pos[10];
static bool g_gfs_open[10];
bool gfs_active(void) { return false; }
const char *gfs_title(void) { return "CUSA99999"; }
/* ghostfs.c and bigexec.c are not linked here: ghostfs is inactive (above)
 * and no module in this harness is a big executable */
int gfs_find_file(const char *name, char *dir, size_t size) { (void)name, (void)dir, (void)size; return -1; }
int bx_place(struct module *m) { (void)m; return -1; }
int bx_write_text(struct module *m, const u8 *text) { (void)m, (void)text; return -1; }
u32 bx_code(const struct module *m, const struct bx_range **runs) { (void)m, (void)runs; return 0; }
static int k_getpid(void) { return 77; }
static s32 real_app_info(s32 pid, u8 *info)
{
    strlcpy((char *)info + 0x10, "CUSA03474", 10);
    return 0;
}
bool gfs_owns(int fd) { return fd >= GFS_FD && fd < GFS_FD + 10 && g_gfs_open[fd - GFS_FD]; }
int gfs_open(const char *path, int flags, int *fd_out)
{
    if (strncmp(path, "/app0/gfs/", 10) != 0)
        return 1;
    if (strcmp(path + 10, "a.bin") != 0) {
        *fd_out = E_NOENT;
        return 0;
    }
    for (int i = 0; i < 10; i++)
        if (!g_gfs_open[i]) {
            g_gfs_open[i] = true, g_gfs_pos[i] = 0;
            *fd_out = GFS_FD + i;
            return 0;
        }
    *fd_out = E_NOMEM;
    return 0;
}
int gfs_close(int fd)
{
    if (!gfs_owns(fd))
        return 1;
    g_gfs_open[fd - GFS_FD] = false;
    return 0;
}
int gfs_stat(const char *path, void *st, int *rc)
{
    if (strncmp(path, "/app0/gfs/", 10) != 0)
        return 1;
    *rc = strcmp(path + 10, "a.bin") == 0 ? 0 : E_NOENT;
    return 0;
}
s64 gfs_pread(int fd, void *buf, u64 n, s64 off)
{
    u64 size = sizeof(k_gfs_data) - 1;
    if (off < 0)
        return E_INVAL;
    if ((u64)off >= size)
        return 0;
    n = n < size - (u64)off ? n : size - (u64)off;
    memcpy(buf, k_gfs_data + off, n);
    return (s64)n;
}
s64 gfs_read(int fd, void *buf, u64 n)
{
    s64 r = gfs_pread(fd, buf, n, (s64)g_gfs_pos[fd - GFS_FD]);
    if (r > 0)
        g_gfs_pos[fd - GFS_FD] += (u64)r;
    return r;
}
s64 gfs_lseek(int fd, s64 off, int whence)
{
    g_gfs_pos[fd - GFS_FD] = (u64)(whence == 1 ? (s64)g_gfs_pos[fd - GFS_FD] + off : off);
    return (s64)g_gfs_pos[fd - GFS_FD];
}
int gfs_fstat(int fd, void *st)
{
    memset(st, 0, 0x78);
    *(s64 *)((u8 *)st + 0x48) = sizeof(k_gfs_data) - 1;
    return 0;
}
int gfs_getdents(int fd, void *buf, int size)
{
    if (g_gfs_pos[fd - GFS_FD]++)
        return 0;
    u8 *d = buf;
    *(u32 *)d = 1, *(u16 *)(d + 4) = 16, d[6] = 8, d[7] = 5;
    memcpy(d + 8, "a.bin", 6);
    return 16;
}

/* SWRR's libkernel and libSceSysmodule */
static u32 g_real_unloads, g_real_loads, g_real_reads, g_real_exits, g_real_polls;
static s32 real_load(u32 id)
{
    g_real_loads++;
    return id == 0xe7 ? E_NOENT : 0; /* Json2: SWRR's library reports it missing */
}
static s32 real_unload(u32 id) { return g_real_unloads++, 0; }
static s32 real_is_loaded(u32 id) { return SYSMOD_UNLOADED; }
static s32 real_load_start(const char *path, u64 argc, const void *argp, u32 flags, const void *opt, s32 *res)
{
    return E_NOENT;
}
static s32 real_esrch(void) { return E_SRCH; }
static s32 real_one(void) { return 1; }
static s64 real_read(s32 fd, void *buf, u64 n) { return g_real_reads++, (s64)n; }
static void real_exit(void *ret) { g_real_exits++; }
static s32 real_poll(s32 id, s32 *state)
{
    g_real_polls++;
    *state = 2;
    return 0;
}
/* POSIX pthread_create, faked: the thread runs right here */
static int real_pthread_create(u64 *thr, const void *attr, void *(*fn)(void *), void *arg)
{
    *thr = 0x7777;
    u64 saved = g_tls_slot;
    g_tls_slot = 0; /* a new thread: SWRR's fs:[-8] is not ours */
    fn(arg);
    g_tls_slot = saved;
    return 0;
}

/* SWRR's _fcntl on a PS5: F_SETFL on an internet socket from the JIT compiler is EACCES */
static int g_nbio = -1;
static s64 real_fcntl(s64 fd, s64 cmd, s64 arg)
{
    if (fd == 900 && cmd == 4) {
        g_errno = 13;
        return -1;
    }
    return 0;
}
static int real_setsockopt(int fd, int level, int name, const void *val, u32 len)
{
    if (fd != 900 || level != 0xFFFF || name != 0x1200 || len != 4)
        return -1;
    g_nbio = *(const int *)val;
    return 0;
}

void *host_real(const char *name)
{
    static const struct {
        const char *name;
        void *fn;
    } k[] = {
        {"sceSysmoduleLoadModule", real_load},
        {"sceSysmoduleLoadModuleInternal", real_load},
        {"sceSysmoduleUnloadModule", real_unload},
        {"sceSysmoduleIsLoaded", real_is_loaded},
        {"sceKernelGetModuleInfoFromAddr", real_esrch},
        {"sceKernelGetModuleInfoForUnwind", real_esrch},
        {"_sceKernelRtldThreadAtexitIncrement", real_one},
        {"sceKernelRead", real_read},
        {"scePthreadExit", real_exit},
        {"sceKernelAioPollRequest", real_poll},
        {"pthread_create", real_pthread_create},
        {"sceKernelGetAppInfo", real_app_info},
        {"_fcntl", real_fcntl},
        {"setsockopt", real_setsockopt},
    };
    for (u32 i = 0; i < ARRAY_SIZE(k); i++)
        if (strcmp(k[i].name, name) == 0)
            return k[i].fn;
    return NULL;
}

/* SWRR's modules: its libkernel and its own eboot */
static int k_module_list(u32 *handles, size_t max, size_t *count)
{
    handles[0] = 0x11, handles[1] = 0x12;
    *count = 2;
    return 0;
}
static int k_module_info_kernel(u32 handle, struct sce_module_info *info)
{
    strlcpy(info->name, handle == 0x11 ? "libkernel.sprx" : handle == 0x77 ? EXT_SELFTEST_ALIAS ".sprx" : "eboot.bin",
            sizeof(info->name));
    return 0;
}

static s32 fake_host_dlsym_call(u32 handle, const char *name, void **out)
{
    *out = (void *)0xD15D15ULL;
    return 0;
}

const struct ghost_api g_ghost_api = {
    .version = GHOST_API_VERSION,
    .size = sizeof(struct ghost_api),
    .ctx = &g_ctx,
    .k = &K,
    .modules = &g_modules,
    .main_mod = &g_main_mod,
    .reported_dmem = &g_reported_dmem,
    .logf = logf,
    .arena_alloc = arena_alloc,
    .arena_strdup = arena_strdup,
    .page_alloc = page_alloc,
    .compute_nid = compute_nid,
    .host_real = host_real,
    .host_dlsym = host_dlsym,
    .call_gadget6 = call_gadget6,
    .remap_path = remap_path,
    .vfs_stat = vfs_stat,
    .module_export = module_export,
    .module_load_runtime = module_load_runtime,
    .module_start = module_start,
    .gfs_active = gfs_active,
    .gfs_owns = gfs_owns,
    .gfs_open = gfs_open,
    .gfs_close = gfs_close,
    .gfs_stat = gfs_stat,
    .gfs_read = gfs_read,
    .gfs_pread = gfs_pread,
    .gfs_lseek = gfs_lseek,
    .gfs_fstat = gfs_fstat,
    .gfs_getdents = gfs_getdents,
    .gfs_title = gfs_title,
    .direct_alloc = direct_alloc,
    .direct_free = direct_free,
    .spin_lock = spin_lock,
    .spin_unlock = spin_unlock,
    .tls_get_addr = tls_get_addr,
    .thread_dtors = &g_thread_dtors,
    .ghost_thread_start = ghost_thread_start,
};

/* the extension's function for lib:name, as resolve_import would bind it */
static void *ext_fn(const char *lib, const char *name)
{
    char nid[12];
    compute_nid(name, nid);
    void *fn = (void *)ext_hle_lookup(lib, nid, NULL);
    if (!fn)
        logf("ext_host: no extension function %s:%s", lib, name);
    return fn;
}

static struct module *mod(const char *file)
{
    for (struct module *m = g_modules; m; m = m->next)
        if (strcmp(m->name, file) == 0)
            return m;
    return NULL;
}

/* ---- goal 1: sceLibcHeapGetTraceInfo ---- */
static void heap_trace_tests(void)
{
    void (*get)(u8 *) = ext_fn("libSceLibcInternalExt", "sceLibcHeapGetTraceInfo");
    expect(get != NULL, "sceLibcHeapGetTraceInfo is the extension's");
    if (!get)
        return;
    /* libc.prx's template: {size 0x20, flags 1, get_segment_info 1, NULL, NULL} */
    u8 info[0x20] = {0x20, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1};
    get(info);
    u64 *mask = *(u64 **)(info + 0x10), *table = *(u64 **)(info + 0x18);
    expect(mask && table && (void *)mask != (void *)table, "heap trace info gets private mask and table");
    expect(*(u32 *)(info + 0x0C) == 0 && *(u32 *)(info + 8) == 1, "heap tracing stays off, flags kept");
    if (mask && table) {
        *mask = 0;                /* what _malloc_init_lv2 does (libc.prx+0x52f5f) */
        table[0] = table[63] = 1; /* and its mstate pointers */
    }
    u8 small[0x20] = {0x10};
    get(small);
    expect(*(u64 *)(small + 0x10) == 0, "a struct smaller than 0x20 bytes is left alone");
}

/* ---- goal 2: the boot's module set, starts, sysmodule, run-time loads ---- */
void kernel_boot(const char *app0)
{
    if (!app0)
        return;
    strlcpy(g_root, app0, sizeof(g_root));
    K.usleep = k_usleep;
    K.load_start_module = (void *)real_load_start;
    K.module_list = k_module_list;
    K.module_info = k_module_info_kernel;
    K.errno_fn = k_errno;
    K.mprotect = k_mprotect;
    K.pread = (void *)gfs_pread;
    K.getpid = k_getpid;
    g_code = g_code_cur = (u64)sys_map(CODE_SIZE);
    sys_protect((void *)g_code, CODE_SIZE, 0);
    expect(modules_load_boot(app0, &g_main_mod) == 0 && g_main_mod, "boot: the title's modules load");
    for (struct module *m = g_modules; m; m = m->next)
        if (!m->is_main)
            expect(module_place(m, arena_code_alloc(m->image_size)) == 0, "boot: module placed");
    if (g_main_mod)
        expect(module_place(g_main_mod, arena_code_alloc(g_main_mod->image_size)) == 0, "boot: executable placed");
}

static void boot_tests(void)
{
    const char *order[] = {"libSceFios2.prx", "libc.prx",          "libSceFoo.prx", "libSceNpToolkit2.prx",
                           "libSceTls.prx",   "libSceTlsUser.prx", "libSceZed.prx", "eboot.bin"};
    struct module *m = g_modules;
    u32 i = 0;
    for (; m && i < ARRAY_SIZE(order); m = m->next, i++)
        if (strcmp(m->name, order[i]) != 0)
            break;
    expect(i == ARRAY_SIZE(order) && !m, "boot: Fios2, libc, then what the imports need by name, then the executable");
    expect(!mod("libSceJson.prx") && !mod("libSceNpToolkit.prx"), "boot: modules nothing imports wait");
    expect(mod("libSceNpToolkit2.prx")->start_state == MOD_DEFERRED, "boot: IsGame NpToolkit2 deferred");
    for (m = g_modules; m; m = m->next)
        module_finish(m);
    /* a thread block asked for before the static block's size and alignment
     * are set (an ELF loader thread once was): 16-aligned, in a pool */
    g_tls_slot = 0;
    tls_ensure_thread();
    expect(g_tls_slot > 0x10000 && (g_tls_slot & 15) == 0, "TLS: a thread block before tls_setup: 16-aligned, not 0");
    g_tls_slot = 0;
    tls_setup(g_main_mod);
    bool ok = true;
    for (m = g_modules; m; m = m->next)
        ok &= module_relocate(m) == 0;
    expect(ok, "boot: relocated");
    ext_boot_done();
    for (m = g_modules; m; m = m->next)
        ok &= module_write_text(m) == 0;
    stubs_flush();
    expect(ok, "boot: text written");
    u64 *got = (u64 *)(g_main_mod->base + 0x4000); /* the executable's import of libc's libcFn */
    struct module *libc = mod("libc.prx");
    expect(libc && *got == libc->base + 0x20, "boot: the executable's import binds to the title's libc");
    modules_start_boot();
    expect(mod("libSceFios2.prx")->start_rc == 0xF1 && libc->start_rc == 0xC0, "start: Fios2 and libc");
    expect(mod("libSceFoo.prx")->start_rc == 0xF00, "start: through DT_INIT, not e_entry");
    expect(mod("libSceZed.prx")->start_state == MOD_STARTED, "start: a needed module");
    struct module *zed = mod("libSceZed.prx");
    expect(*(u64 *)(zed->base + 0x4010) == 0 && *(u64 *)(zed->base + 0x4018) != 0,
           "boot: an unresolved variable binds to 0, an unresolved function to a stub");
    expect(mod("libSceNpToolkit2.prx")->start_state == MOD_DEFERRED, "start: NpToolkit2 still waits");
}

static void sysmodule_tests(void)
{
    s32 (*load)(u32) = ext_fn("libSceSysmodule", "sceSysmoduleLoadModule");
    s32 (*loaded)(u32) = ext_fn("libSceSysmodule", "sceSysmoduleIsLoaded");
    s32 (*unload)(u32) = ext_fn("libSceSysmodule", "sceSysmoduleUnloadModule");
    s32 (*with_arg)(u32, s32, const void *, u64, s32 *) =
        ext_fn("libSceSysmodule", "sceSysmoduleLoadModuleInternalWithArg");
    if (!load || !loaded || !unload || !with_arg) {
        expect(false, "sysmodule functions");
        return;
    }
    struct module *tk2 = mod("libSceNpToolkit2.prx");
    expect(loaded(0x102) == SYSMOD_UNLOADED, "sysmodule: NpToolkit2 not loaded before the title asks");
    expect(load(0x102) == 0 && tk2->start_rc == 0x102 && tk2->start_state == MOD_STARTED,
           "sysmodule: LoadModule(0x102) starts the title's NpToolkit2");
    expect(loaded(0x102) == 0 && load(0x102) == 0 && tk2->start_rc == 0x102, "sysmodule: started once, loaded");
    s32 res = -1;
    expect(with_arg(0xd7, 0, NULL, 0, &res) == 0 && res == 0 && loaded(0xd7) == 0,
           "sysmodule: DiscMap answered without loading");
    expect(with_arg(0x102, 0, NULL, 1, &res) == SYSMOD_INVALID, "sysmodule: flags are refused");
    u32 before = g_real_loads;
    expect(load(0xf7) == 0 && mod("libSceNpToolkit.prx") && mod("libSceNpToolkit.prx")->start_rc == 0xF7,
           "sysmodule: an IsGame module the boot left out loads from sce_module");
    expect(load(0xfe) == E_NOENT, "sysmodule: a missing IsGame module is ENOENT");
    expect(g_real_loads == before, "sysmodule: IsGame ids never reach SWRR's library");
    expect(load(0xe7) == E_NOENT && load(0x84) == 0 && g_real_loads == before + 2,
           "sysmodule: other ids pass through with SWRR's real result");
    expect(load(0) == SYSMOD_INVALID, "sysmodule: id 0 is invalid");
    expect(unload(0x102) == 0 && unload(0xa7) == 0 && g_real_unloads == 0 && loaded(0x102) == 0,
           "sysmodule: unload is never forwarded");
    expect(loaded(0x1c) == SYSMOD_UNLOADED, "sysmodule: IsLoaded of a system module asks SWRR's");
}

static void load_start_tests(void)
{
    s32 (*lsm)(const char *, u64, const void *, u32, const void *, s32 *) =
        ext_fn("libkernel", "sceKernelLoadStartModule");
    s32 (*dlsym)(s32, const char *, void **) = ext_fn("libkernel", "sceKernelDlsym");
    s32 (*stop)(s32, u64, const void *, u32, const void *, s32 *) = ext_fn("libkernel", "sceKernelStopUnloadModule");
    if (!lsm || !dlsym || !stop) {
        expect(false, "module functions");
        return;
    }
    s32 res = -1;
    const char *args[3] = {"a", "b", "c"};
    s32 h = lsm("/app0/sce_module/libSceJson.prx", 3, args, 0, NULL, &res);
    struct module *json = mod("libSceJson.prx");
    expect(json && h == (s32)json->fake_handle && res == 3, "LoadStartModule: a title prx loads and starts at run time");
    if (!json)
        return;
    expect(*(u64 *)(json->base + 0x4000) == HOST_USLEEP,
           "LoadStartModule: its host import resolves by a name kept from boot");
    res = -1;
    expect(lsm("libSceJson.prx", 0, NULL, 0, NULL, &res) == h && res == 3, "LoadStartModule: a bare name, started once");
    res = -1;
    expect(lsm("libc.prx", 0, NULL, 0, NULL, &res) == (s32)mod("libc.prx")->fake_handle && res == 0xC0,
           "LoadStartModule: a boot module");
    expect(lsm("/app0/sce_module/nothere.prx", 0, NULL, 0, NULL, &res) == E_NOENT,
           "LoadStartModule: a missing file goes to the kernel");
    expect(lsm("/app0/sce_module/libSceIe.prx", 0, NULL, 0, NULL, &res) == E_NOEXEC && !mod("libSceIe.prx"),
           "LoadStartModule: initial-exec TLS is refused after boot");
    void *a = NULL;
    expect(dlsym(h, "jsonFn", &a) == 0 && (u64)a == json->base + 0x30, "dlsym: a title module's export by name");
    char nid[12];
    compute_nid("jsonFn", nid);
    a = NULL;
    expect(dlsym(h, nid, &a) == 0 && (u64)a == json->base + 0x30, "dlsym: by an encoded NID");
    expect(dlsym(h, "jsonMissing", &a) == E_SRCH && !a, "dlsym: a miss is ESRCH");
    res = -1;
    expect(stop(h, 0, NULL, 0, NULL, &res) == 0 && res == 0 && mod("libSceJson.prx"), "StopUnloadModule: kept loaded");
    /* host modules: the extension's functions first, no-host libraries refused, the rest SWRR's */
    char self[12];
    compute_nid(EXT_SELFTEST_NAME, self);
    void *fn = (void *)ext_hle_lookup(EXT_SELFTEST_LIB, self, NULL);
    a = NULL;
    expect(dlsym(0x55, EXT_SELFTEST_NAME, &a) == 0 && a == fn, "dlsym: the extension's function on a host module");
    expect(dlsym(0x77, "ghostldrExtMissing", &a) == E_NOENT && !a, "dlsym: a no-host library's module is refused");
    g_ctx->dlsym = (u64)fake_host_dlsym_call;
    expect(dlsym(0x55, "ghostldrExtMissing", &a) == 0 && (u64)a == 0xD15D15, "dlsym: other host modules ask SWRR");
    /* libkernel_sys is a virtual module (hle_fsvsh.c); what the extension
     * lacks there is libkernel's own (PS4-Xplorer's
     * sceKernelGetFsSandboxRandomWord), never a NULL */
    s32 ks = lsm("libkernel_sys.sprx", 0, NULL, 0, NULL, &res);
    a = NULL;
    expect(ks >= 0x7F00 && dlsym(ks, "sceKernelGetFsSandboxRandomWord", &a) == 0 && (u64)a == 0xD15D15,
           "dlsym: libkernel_sys's other functions are libkernel's");
    u64 saved = g_punched;
    g_punched = 128UL << 20;
    expect(lsm("/app0/sce_module/libSceLate.prx", 0, NULL, 0, NULL, &res) == E_NOMEM,
           "LoadStartModule: refused with ENOMEM past the punch allowance");
    g_punched = saved;
}

/* ---- goal 3: module queries and thread-exit hooks ---- */
/* the title libc's thread-exit hook, faked: count the runs.  (It used to
 * flip a bit in g_tls_slot as its marker, but the core's thread trampoline
 * runs the hook too -- before its own TLS teardown -- so the marker
 * corrupted the ending thread's slot and its blocks were never recycled:
 * the goal-4 reuse check below could not pass with it.) */
static u32 g_dtors_ran;
static void dtor_hook(void) { g_dtors_ran++; }

static void query_tests(void)
{
    s32 (*from_addr)(u64, s32, u8 *) = ext_fn("libkernel", "sceKernelGetModuleInfoFromAddr");
    s32 (*unwind)(u64, s32, u8 *) = ext_fn("libkernel", "sceKernelGetModuleInfoForUnwind");
    s32 (*sys_unwind)(u64, s32, u8 *) = ext_fn("libSceSysmodule", "sceSysmoduleGetModuleInfoForUnwind");
    s32 (*info)(s32, u8 *) = ext_fn("libkernel", "sceKernelGetModuleInfo");
    s32 (*list)(s32 *, u64, u64 *) = ext_fn("libkernel", "sceKernelGetModuleList");
    s32 (*list2)(s32 *, u64, u64 *) = ext_fn("libkernel", "sceKernelGetModuleList2");
    s32 (*match)(const u8 *, u64) = ext_fn("libkernel", "__elf_phdr_match_addr");
    s32 (*inc)(u64) = ext_fn("libkernel", "_sceKernelRtldThreadAtexitIncrement");
    void (*pexit)(void *) = ext_fn("libkernel", "scePthreadExit");
    if (!from_addr || !unwind || !sys_unwind || !info || !list || !list2 || !match || !inc || !pexit) {
        expect(false, "module query functions");
        return;
    }
    struct module *libc = mod("libc.prx");
    u8 ex[0x1a8];
    memset(ex, 0xEE, sizeof(ex));
    *(u64 *)ex = 0x1a8;
    expect(from_addr(libc->base + 0x10, 2, ex) == 0 && strcmp((char *)ex + 8, "libc.prx") == 0 &&
               *(s32 *)(ex + 0x108) == (s32)libc->fake_handle,
           "FromAddr: a title address names its module and handle");
    expect(*(u64 *)(ex + 0x148) == libc->base + 0x100 && *(u64 *)(ex + 0x150) == libc->base + 0x80 &&
               *(u32 *)(ex + 0x15C) == 0x80 && *(u32 *)(ex + 0x158) == 12,
           "FromAddr: eh_frame_hdr and eh_frame from PT_GNU_EH_FRAME");
    expect(*(u32 *)(ex + 0x10C) == libc->tls_id && *(u64 *)(ex + 0x110) == libc->base + 0x4100 &&
               *(u32 *)(ex + 0x118) == 8 && *(u32 *)(ex + 0x11C) == 16 && *(u64 *)(ex + 0x128) == libc->base &&
               *(u64 *)(ex + 0x130) == libc->base + 0x30,
           "FromAddr: TLS, init and fini");
    expect(*(u32 *)(ex + 0x1A0) == 2 && *(u64 *)(ex + 0x160) == libc->base && *(u32 *)(ex + 0x168) == 0x4000 &&
               *(s32 *)(ex + 0x16C) == 5 && *(u64 *)(ex + 0x170) == libc->base + 0x4000 && *(s32 *)(ex + 0x17C) == 6,
           "FromAddr: segments");
    expect(from_addr(libc->base, 3, ex) == E_INVAL && *(u64 *)ex == 0, "FromAddr: flags 3 is EINVAL");
    u64 stack_var = 0;
    expect(from_addr((u64)&stack_var, 2, ex) == E_SRCH, "FromAddr: other addresses go to the kernel");
    *(u64 *)ex = 0x1a8;
    from_addr(libc->base + 0x10, 2, ex);
    expect(match(ex, libc->base + 0x4008) == 1 && match(ex, (u64)&stack_var) == 0, "__elf_phdr_match_addr");
    u8 uw[0x130];
    *(u64 *)uw = 0x130;
    expect(unwind(libc->base + 0x10, 1, uw) == 0 && *(u64 *)(uw + 0x108) == libc->base + 0x100 &&
               *(u64 *)(uw + 0x110) == libc->base + 0x80 && *(u64 *)(uw + 0x118) == 0x80 &&
               *(u64 *)(uw + 0x120) == libc->base && *(u64 *)(uw + 0x128) == 0x4000 &&
               strcmp((char *)uw + 8, "libc.prx") == 0,
           "ForUnwind: the unwinder finds the title module's FDEs");
    *(u64 *)uw = 0x100;
    expect(unwind(libc->base + 0x10, 1, uw) == E_INVAL, "ForUnwind: a short struct is EINVAL");
    *(u64 *)uw = 0x130;
    expect(sys_unwind(mod("libSceNpToolkit2.prx")->base, 1, uw) == 0 && !uw[8], "sysmodule ForUnwind hides IsGame names");
    u8 mi[0x160];
    *(u64 *)mi = 0x160;
    expect(info((s32)libc->fake_handle, mi) == 0 && strcmp((char *)mi + 8, "libc.prx") == 0 &&
               *(u32 *)(mi + 0x148) == 2 && mi[0x14C] == 1 && mi[0x14C + 19] == 20,
           "GetModuleInfo: name, segments and fingerprint");
    *(u64 *)mi = 0x1a8;
    expect(info((s32)libc->fake_handle, mi) == E_INVAL, "GetModuleInfo: a wrong st_size is EINVAL");
    s32 h[32];
    u64 n = 0;
    expect(list(h, 32, &n) == 0 && h[0] == (s32)g_main_mod->fake_handle && h[n - 1] == 0x11,
           "GetModuleList: the executable first, then SWRR's system modules only");
    u64 n2 = 0;
    expect(list2(h, 32, &n2) == 0 && n2 == n - 1, "GetModuleList2: the title's modules");
    expect(list(h, 2, &n) == E_NOMEM, "GetModuleList: a short array is ENOMEM");
    expect(inc(libc->base + 0x4100) == 0 && inc((u64)&stack_var) == 1,
           "rtld thread atexit: the title's modules register thread_local destructors");
    g_thread_dtors = dtor_hook;
    u32 dtors = g_dtors_ran;
    u32 exits = g_real_exits;
    pexit(NULL);
    expect(g_dtors_ran == dtors + 1 && g_real_exits == exits + 1, "pthread exit runs libc's thread-exit hook");
}

/* ---- goal 4: TLS ---- */
struct tls_index {
    u64 module, offset;
};

static struct module *g_libc;
static void *thread_fn(void *arg)
{
    struct tls_index ti = {1, 0};
    void **seen = arg;
    u8 *v = tls_get_addr(&ti);
    u8 *l = tls_get_addr((struct tls_index *)(g_libc->base + 0x4010));
    seen[0] = memcmp(v, "MAINTLS!", 8) == 0 ? v : NULL;
    seen[1] = memcmp(l - 8, "LIBCTLS!", 8) == 0 ? l : NULL;
    v[0] = l[-8] = 'X'; /* the next thread must not see these */
    return NULL;
}

static u64 thunk_target(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)
{
    return g_tls_slot ? a + b + c + d + e + f : 0;
}

static void tls_tests(void)
{
    struct module *main = g_main_mod, *libc = mod("libc.prx"), *st = mod("libSceTls.prx");
    expect(main->tls_id == 1 && main->tls_offset == 0x30 && st->tls_offset == 0x60 && !libc->tls_offset,
           "TLS: the executable at -0x30, the initial-exec prx below it, libc dynamic");
    expect(*(u64 *)(main->base + 0x4040) == 1 && *(u64 *)(main->base + 0x4048) == 4,
           "TLS: the executable's DTPMOD64 is module 1");
    expect(*(s64 *)(st->base + 0x4020) == 8 - 0x60, "TLS: TPOFF64 is the static offset");
    const u8 *t = (const u8 *)main->base;
    expect(*(const u32 *)(t + 0x18) == 0xFFFFFFF8u && *(const u32 *)(t + 0x25) == 0 &&
               *(const u32 *)(t + 0x35) == 0x30,
           "TLS: the profile's site list: listed sites only, each checked");
    struct module *user = mod("libSceTlsUser.prx");
    expect(*(u64 *)(user->base + 0x4000) == st->tls_id && *(u64 *)(user->base + 0x4008) == 0x14 &&
               *(s64 *)(user->base + 0x4010) == 0x10 - 0x60,
           "TLS: an imported thread_local names the exporting module's block");
    expect(*(const u32 *)(st->base + 0x45) == 0xFFFFFFF8u && *(const u32 *)(mod("libSceZed.prx")->base + 0x45) == 0,
           "TLS: a prx is patched only with initial-exec TLS");
    g_tls_slot = 0;
    struct tls_index ti = {1, 0};
    u8 *mainv = tls_get_addr(&ti);
    u64 gt = g_tls_slot;
    expect(gt && mainv == (u8 *)gt - 0x30 && memcmp(mainv, "MAINTLS!", 8) == 0,
           "TLS: module 1 is the executable's static image");
    ti = (struct tls_index){st->tls_id, 0};
    u8 *stv = tls_get_addr(&ti);
    expect(stv == (u8 *)gt - 0x60 && memcmp(stv, "TLS2", 4) == 0 && ((u64)stv & 31) == 0,
           "TLS: an initial-exec prx through __tls_get_addr");
    struct tls_index *lti = (struct tls_index *)(libc->base + 0x4010);
    u8 *lv = tls_get_addr(lti);
    expect(lti->module == libc->tls_id && lti->offset == 8 && lv && memcmp(lv - 8, "LIBCTLS!", 8) == 0 &&
               tls_get_addr(lti) == lv,
           "TLS: libc's dynamic block, once per thread");
    /* more than 16 modules with TLS */
    static struct module extra[24];
    static u8 image[24][16];
    struct module **tail = &g_modules;
    while (*tail)
        tail = &(*tail)->next;
    bool ok = true;
    for (u32 k = 0; k < 24; k++) {
        image[k][0] = (u8)k;
        extra[k] = (struct module){.name = "extra.prx", .base = (u64)image[k], .tls_filesz = 1, .tls_memsz = 16,
                                   .tls_align = 64, .tls_id = 0x1100 + k};
        *tail = &extra[k];
        tail = &extra[k].next;
    }
    for (u32 k = 0; k < 24; k++) {
        ti = (struct tls_index){0x1100 + k, 0};
        u8 *v = tls_get_addr(&ti);
        ok &= v && *v == k && ((u64)v & 63) == 0;
    }
    expect(ok, "TLS: 24 more modules: the vector grows, blocks aligned");
    for (u32 k = 0; k < 24; k++) {
        ti = (struct tls_index){0x1100 + k, 0};
        ok &= *(u8 *)tls_get_addr(&ti) == k;
    }
    expect(ok, "TLS: and their blocks stay");
    *tail = NULL;
    for (tail = &g_modules; *tail != &extra[0]; tail = &(*tail)->next)
        ;
    *tail = NULL;
    int (*pc)(u64 *, const void *, void *(*)(void *), void *) = ext_fn("libkernel", "pthread_create");
    int (*pc_posix)(u64 *, const void *, void *(*)(void *), void *) = ext_fn("libScePosix", "pthread_create");
    void *seen[2] = {0}, *again[2] = {0};
    u64 thr = 0;
    g_libc = libc;
    expect(pc && pc == pc_posix && pc(&thr, NULL, thread_fn, seen) == 0 && thr == 0x7777 && seen[0] && seen[1] &&
               seen[0] != mainv,
           "pthread_create: the thread gets its own static TLS block");
    expect(pc && pc(&thr, NULL, thread_fn, again) == 0 && again[0] == seen[0] && again[1] == seen[1],
           "TLS: an ended thread's blocks serve the next thread, fresh");
    /* fs:[-8] holding something else: never taken for a block */
    u64 stack_var = 0, before = g_tls_slot;
    g_tls_slot = (u64)&stack_var;
    tls_ensure_thread();
    u64 adopted = g_tls_slot;
    g_tls_slot = before + 8;
    tls_ensure_thread();
    expect(adopted != (u64)&stack_var && g_tls_slot != before + 8 && g_tls_slot != before,
           "TLS: only a block of its pools counts as a thread's");
    g_tls_slot = before;
    /* a host thread calling the title back */
    u64 (*thunk)(u64, u64, u64, u64, u64, u64) = (void *)tls_thunk((u64)thunk_target);
    g_tls_slot = 0;
    u64 sum = thunk((u64)thunk != (u64)thunk_target ? 1 : 100, 2, 3, 4, 5, 6);
    expect(sum == 21 && g_tls_slot && tls_thunk((u64)thunk_target) == (u64)thunk,
           "TLS: a callback thunk gives a host thread its block, arguments intact");
    g_tls_slot = before;
}

/* ---- goal 5: files ---- */
static void file_tests(void)
{
    s32 (*kopen)(const char *, s32, s32) = ext_fn("libkernel", "sceKernelOpen");
    s32 (*popen)(const char *, s32, s32) = ext_fn("libScePosix", "open");
    s64 (*kread)(s32, void *, u64) = ext_fn("libkernel", "sceKernelRead");
    s64 (*pread_)(s32, void *, u64, s64) = ext_fn("libkernel", "pread");
    s64 (*readv)(s32, const u64 *, s32) = ext_fn("libkernel", "readv");
    s64 (*preadv)(s32, const u64 *, s32, s64) = ext_fn("libkernel", "sceKernelPreadv");
    s64 (*lseek)(s32, s64, s32) = ext_fn("libkernel", "lseek");
    s32 (*fstat)(s32, void *) = ext_fn("libkernel", "fstat");
    s32 (*fcntl)(s32, s32, u64) = ext_fn("libkernel", "fcntl");
    s32 (*dup)(s32) = ext_fn("libkernel", "dup");
    s64 (*write)(s32, const void *, u64) = ext_fn("libkernel", "write");
    s32 (*getdirentries)(s32, void *, s32, s64 *) = ext_fn("libkernel", "getdirentries");
    s32 (*access)(const char *, s32) = ext_fn("libkernel", "access");
    s32 (*close)(s32) = ext_fn("libkernel", "close");
    s32 (*mmap)(void *, u64, s32, s32, s32, s64, void **) = ext_fn("libkernel", "sceKernelMmap");
    s32 (*submit)(void *, s32, s32, s32 *) = ext_fn("libkernel", "sceKernelAioSubmitReadCommands");
    s32 (*poll)(s32, s32 *) = ext_fn("libkernel", "sceKernelAioPollRequest");
    s32 (*delete)(s32, s32 *) = ext_fn("libkernel", "sceKernelAioDeleteRequest");
    if (!kopen || !popen || !kread || !pread_ || !readv || !preadv || !lseek || !fstat || !fcntl || !dup || !write ||
        !getdirentries || !access || !close || !mmap || !submit || !poll || !delete) {
        expect(false, "file functions");
        return;
    }
    s32 fd = kopen("/app0/gfs/a.bin", 0, 0);
    char buf[16] = {0};
    expect(fd == GFS_FD && kread(fd, buf, 4) == 4 && memcmp(buf, "0123", 4) == 0, "files: open and read a ghostfs file");
    g_errno = 0;
    expect(popen("/app0/gfs/missing", 0, 0) == -1 && g_errno == 2, "files: POSIX open: -1 and errno ENOENT");
    expect(pread_(fd, buf, 3, 10) == 3 && memcmp(buf, "abc", 3) == 0, "files: POSIX pread");
    char a[3], b[4];
    u64 iov[4] = {(u64)a, 3, (u64)b, 4};
    expect(readv(fd, iov, 2) == 7 && memcmp(a, "456", 3) == 0 && memcmp(b, "789a", 4) == 0, "files: readv");
    expect(preadv(fd, iov, 2, 20) == 7 && memcmp(a, "klm", 3) == 0 && memcmp(b, "nopq", 4) == 0, "files: preadv");
    expect(lseek(fd, 2, 0) == 2 && kread(fd, buf, 1) == 1 && buf[0] == '2', "files: lseek");
    u8 st[0x78];
    expect(fstat(fd, st) == 0 && *(s64 *)(st + 0x48) == 36, "files: fstat");
    g_errno = 0;
    expect(fcntl(fd, 3, 0) == 0 && fcntl(fd, 99, 0) == -1 && g_errno == 22, "files: fcntl F_GETFL, others EINVAL");
    g_errno = 0;
    expect(dup(fd) == -1 && g_errno == 9, "files: dup of a ghostfs fd is EBADF");
    g_errno = 0;
    expect(write(fd, "x", 1) == -1 && g_errno == 9 && write(1, "hello\n", 6) == 6, "files: write");
    g_errno = 0;
    expect(access("/app0/gfs/a.bin", 4) == 0 && access("/app0/gfs/a.bin", 2) == -1 && g_errno == 30,
           "files: access, read-only");
    s32 (*ufcntl)(s32, s32, s64) = ext_fn("libkernel", "_fcntl");
    expect(ufcntl && ufcntl(900, 4, 0x100006) == 0 && g_nbio == 1 && ufcntl(900, 4, 2) == 0 && g_nbio == 0,
           "files: F_SETFL refused on a socket: O_NONBLOCK through SO_NBIO");
    s32 (*kstat)(const char *, void *) = ext_fn("libkernel", "stat");
    u8 dst[0x78];
    g_errno = 0;
    expect(access("/mnt", 0) == 0 && access("/mnt/", 0) == 0 && kstat && kstat("/mnt", dst) == 0 &&
               (*(u16 *)(dst + 8) & 0170000) == 040000,
           "files: /mnt, above /mnt/usb0, is a directory where SWRR shows none");
    expect(access("/mntx", 0) == -1 && access("/mnt/usb0x", 0) == -1, "files: other missing paths stay missing");
    void *p = NULL;
    expect(mmap(NULL, 0x10, 1, 2, fd, 0, &p) == 0 && p && memcmp(p, "0123456789abcdef", 16) == 0,
           "files: mmap of a ghostfs file reads it");
    expect(mmap(NULL, 0x10, 3, 1, fd, 0, &p) == E_ACCES, "files: a writable shared map is EACCES");
    struct {
        s64 off, nbyte;
        void *buf;
        s64 *result;
        s32 fd;
    } req = {5, 4, buf, NULL, fd};
    s64 result[2] = {0, 0};
    req.result = result;
    s32 id = 0, state = 0, rc = -1;
    expect(submit(&req, 1, 0, &id) == 0 && result[0] == 4 && (u32)result[1] == 3 && memcmp(buf, "5678", 4) == 0,
           "files: AIO reads a ghostfs file at submission");
    expect(poll(id, &state) == 0 && state == 3 && delete(id, &rc) == 0 && rc == 0, "files: its id is complete");
    u32 polls = g_real_polls;
    expect(poll(42, &state) == 0 && state == 2 && g_real_polls == polls + 1, "files: other ids go to the kernel");
    /* the fake gfs_getdents above uses the fd's position as its entry index
     * (one entry, at 0): rewind, then *basep is the position before each read */
    s64 base = -1;
    expect(lseek(fd, 0, 0) == 0 && getdirentries(fd, buf, 16, &base) == 16 && base == 0 &&
               strcmp(buf + 8, "a.bin") == 0 && getdirentries(fd, buf, 16, &base) == 0 && base == 1,
           "files: getdirentries");
    expect(close(fd) == 0 && !gfs_owns(fd), "files: close");
    u32 reads = g_real_reads;
    expect(kread(5, buf, 2) == 2 && g_real_reads == reads + 1, "files: other fds go to the kernel");
}

/* ---- goal 6: the program's identity ---- */
static void identity_tests(void)
{
    s32 (*sdk)(s32 *) = ext_fn("libkernel", "sceKernelGetCompiledSdkVersion");
    s32 (*app_info)(s32, u8 *) = ext_fn("libkernel", "sceKernelGetAppInfo");
    s32 (*argc)(void) = ext_fn("libkernel", "getargc");
    char **(*argv)(void) = ext_fn("libkernel", "getargv");
    s32 (*prospero)(void) = ext_fn("libkernel_cpumode_platform", "sceKernelIsProspero");
    if (!sdk || !app_info || !argc || !argv || !prospero) {
        expect(false, "identity functions");
        return;
    }
    s32 v = 0;
    expect(sdk(&v) == 0 && v == 0x08008011 && sdk(NULL) == E_INVAL, "SDK version: the title's proc param");
    u8 info[0x40] = {0};
    expect(app_info(77, info) == 0 && strcmp((char *)info + 0x10, "CUSA99999") == 0, "AppInfo: the title's id");
    expect(argc() == 1 && strcmp(argv()[0], "/app0/eboot.bin") == 0 && !argv()[1], "getargc, getargv");
    expect(prospero() == 0, "sceKernelIsProspero: a PS4 title");
    s32 (*reg_int)(u32, s32) = ext_fn("libSceRegMgr", "sceRegMgrSetInt");
    s32 (*reg_bin)(u32, const void *, u32) = ext_fn("libSceRegMgr", "sceRegMgrSetBin");
    u64 account = 1;
    const s32 eperm = (s32)0x80020001;
    expect(reg_int && reg_bin && reg_int(0x7800, 6) == eperm && reg_bin(0x7801, &account, 8) == eperm,
           "RegMgr: the title's registry writes are refused");
}

void kernel_tests(void)
{
    heap_trace_tests();
    if (!g_main_mod)
        return;
    boot_tests();
    sysmodule_tests();
    load_start_tests();
    query_tests();
    tls_tests();
    file_tests();
    identity_tests();
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        logf("usage: kernel_host EXT_BIN CRC APP0");
        return 2;
    }
    static struct boot_ctx ctx;
    ctx.magic = BOOT_MAGIC;
    ctx.size = sizeof(ctx);
    ctx.ext_crc = sys_strtoul(argv[2]);
    ctx.libkernel_handle = 0x2001;
    g_ctx = &ctx;
    K.read = k_read;
    K.close = k_close;
    K.map_flex = k_map_flex;
    K.munmap = k_munmap;
    char *blob;
    ctx.ext_size = (u64)sys_read_file(argv[1], &blob);
    ctx.ext_fd = (u64)sys_open(argv[1]);
    u64 small_size = ALIGN_UP(((const struct ext_blob *)blob)->image_memsz + 0x4000, 0x4000);
    if (small_size < 0x10000)
        small_size = 0x10000;
    g_small = sys_map(small_size);
    sys_protect(g_small, small_size, 0);
    g_small_cur = (u64)g_small, g_small_end = (u64)g_small + small_size;
    expect(ext_fetch() == 0, "fetch");
    kernel_boot(argv[3]); /* the title's modules are placed before the extension starts */
    expect(ext_start() == 0, "start (place, relocate, ext_init, self-test)");
    kernel_tests();
    expect(nid_to_name("1G3lF1Gg1k8") == NULL, "NID table freed after boot");
    expect(g_flex_maps == g_flex_unmaps, "flexible memory returned");
    return g_failed;
}

/* fail.c: the loader's notes for a failure's message (not looked at here) */
void fail_note(u32 kind, const char *path) { (void)kind, (void)path; }
