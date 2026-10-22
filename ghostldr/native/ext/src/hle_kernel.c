/* libkernel and libc calls whose answer depends on which program is running:
 * the title's, not SWRR's (research/hle-specs/kernel-modules.md §4.B-4.D). */
#include "kmod.h"

/* SceLibcHeapTraceInfo (0x20 bytes): {u64 size; u32 flags; u32
 * get_segment_info; u64 *mspace_atomic_id_mask; u64 *mstate_table}.  The
 * title's libc.prx (_malloc_init_lv2, libc.prx+0x52f5f in Ghost's) stores
 * through the mask pointer unconditionally and keeps its mstate pointers in
 * the table, so a stub that writes nothing crashes every title that does not
 * replace malloc.  Private storage: never SWRR's own libc globals. */
static u64 g_heap_mask, g_heap_table[64];

static void hle_sceLibcHeapGetTraceInfo(u8 *info)
{
    static u32 calls;
    if (!info || *(u64 *)info < 0x20)
        return;
    *(u32 *)(info + 0x0C) = 0; /* no segment info: libc's heap tracing stays off */
    *(u64 **)(info + 0x10) = &g_heap_mask;
    *(u64 **)(info + 0x18) = g_heap_table;
    if (!calls++)
        logf("sceLibcHeapGetTraceInfo: private mask %p, table %p", (void *)&g_heap_mask, (void *)g_heap_table);
}

/* A title thread that ends through pthread_exit never returns to the
 * core's thread trampoline: run the title libc's thread-exit hook (its
 * thread_local destructors) here, then exit. */
static void thread_exit(const char *real, void *ret)
{
    void (*dtors)(void) = g_thread_dtors;
    if (dtors)
        dtors();
    if (tls_thread_exit)
        tls_thread_exit();
    ((void (*)(void *))host_real(real))(ret);
}

static void hle_scePthreadExit(void *ret) { thread_exit("scePthreadExit", ret); }
static void hle_pthread_exit(void *ret) { thread_exit("pthread_exit", ret); }

/* POSIX thread creation: the same trampoline as scePthreadCreate, so the
 * thread has its static TLS block before the title's code runs (otherwise
 * its first local-exec access reads SWRR's fs:[-8]); the host's own POSIX
 * function keeps POSIX error codes. */
static int hle_pthread_create(void *thr, const void *attr, void *(*fn)(void *), void *arg)
{
    void *real = host_real("pthread_create");
    return real ? ghost_thread_start(real, thr, attr, fn, arg, NULL) : 35 /* EAGAIN */;
}

static int hle_pthread_create_name_np(void *thr, const void *attr, void *(*fn)(void *), void *arg, const char *name)
{
    void *real = host_real("pthread_create_name_np");
    return real ? ghost_thread_start(real, thr, attr, fn, arg, name) : 35;
}

/* ---- the program's identity: the title's, not SWRR's ---- */

/* s32 sceKernelGetCompiledSdkVersion(s32 *ver): the title's proc param
 * (+0x10), not SWRR's 2.50 */
static s32 sdk_version(s32 *ver, const char *real, s32 pid, bool by_pid)
{
    const u64 *pp = g_main_mod ? (const u64 *)g_main_mod->proc_param : NULL;
    if (pp && (!by_pid || pid == K.getpid())) {
        if (!ver)
            return E_INVAL;
        *ver = (s32)pp[2];
        return 0;
    }
    s32 (*fn)(u64, u64) = host_real(real);
    return fn ? (by_pid ? fn((u64)pid, (u64)ver) : fn((u64)ver, 0)) : E_INVAL;
}

static s32 hle_sdk_version(s32 *ver) { return sdk_version(ver, "sceKernelGetCompiledSdkVersion", 0, false); }
static s32 hle_sdk_version_by_pid(s32 pid, s32 *ver)
{
    return sdk_version(ver, "sceKernelGetCompiledSdkVersionByPid", pid, true);
}

/* s32 sceKernelGetAppInfo(s32 pid, SceKernelAppInfo *info): SWRR's, with the
 * title's id in cusa_name (+0x10, 10 bytes) */
static s32 hle_get_app_info(s32 pid, u8 *info)
{
    s32 (*fn)(s32, u8 *) = host_real("sceKernelGetAppInfo");
    s32 rc = fn ? fn(pid, info) : E_INVAL;
    if (rc == 0 && info && gfs_title() && pid == K.getpid())
        strlcpy((char *)info + 0x10, gfs_title(), 10);
    return rc;
}

/* getargc / getargv: what the main thread passed the entry (boot.c) */
static char *g_argv[] = {"/app0/eboot.bin", NULL};
static s32 hle_getargc(void) { return 1; }
static char **hle_getargv(void) { return g_argv; }

/* libc raises these on fatal heap and TLS setup errors (0xA0020010-14) */
static void raise_exception(const char *real, u32 code, u32 arg, u64 from)
{
    struct module *m = mod_by_addr(from);
    logf("%s(%x, %x) from %s+%lx", real, code, arg, m ? m->name : "?", m ? from - m->base : from);
    void (*fn)(u32, u32) = host_real(real);
    if (fn)
        fn(code, arg);
}

static void hle_raise(u32 code, u32 arg)
{
    raise_exception("sceKernelDebugRaiseException", code, arg, (u64)__builtin_return_address(0));
}

static void hle_raise_release(u32 code, u32 arg)
{
    raise_exception("sceKernelDebugRaiseExceptionOnReleaseMode", code, arg, (u64)__builtin_return_address(0));
}

/* ---- callbacks that system libraries call on threads of their own ----
 * AvPlayer calls the title's memory, file and event callbacks on its own
 * threads; the title's code there uses its TLS (kernel-modules.md §7.3.6),
 * so each one goes through a thunk that gives the thread its block first.
 * SceAvPlayerInitData: memory {obj, allocate, deallocate, allocate_texture,
 * deallocate_texture} at 0, file {obj, open, close, read_offset, size} at
 * 0x28, event {obj, callback} at 0x50; the Ex form has a size word first. */
static const u8 k_av_fns[] = {0x08, 0x10, 0x18, 0x20, 0x30, 0x38, 0x40, 0x48, 0x58};

static void *av_wrap(const u8 *data, u64 size, u64 shift)
{
    u8 *copy = arena_alloc(size); /* AvPlayer may keep it: one per player */
    if (!copy)
        return (void *)data;
    memcpy(copy, data, size);
    for (u32 i = 0; i < sizeof(k_av_fns); i++)
        if (*(u64 *)(copy + shift + k_av_fns[i]))
            *(u64 *)(copy + shift + k_av_fns[i]) = tls_thunk(*(u64 *)(copy + shift + k_av_fns[i]));
    return copy;
}

/* SceAvPlayerHandle sceAvPlayerInit(SceAvPlayerInitData *data) (0x78 bytes) */
static u64 hle_av_init(const u8 *data)
{
    u64 (*fn)(const void *) = host_real_in("libSceAvPlayer", "sceAvPlayerInit");
    return fn ? fn(data && tls_thunk ? av_wrap(data, 0x78, 0) : data) : 0;
}

/* s32 sceAvPlayerInitEx(const SceAvPlayerInitDataEx *data, SceAvPlayerHandle *h) */
static s32 hle_av_init_ex(const u8 *data, void *handle)
{
    s32 (*fn)(const void *, void *) = host_real_in("libSceAvPlayer", "sceAvPlayerInitEx");
    u64 size = data ? *(const u64 *)data : 0;
    if (data && tls_thunk && size >= 0x68 && size <= 0x1000)
        data = av_wrap(data, size, 8);
    return fn ? fn(data, handle) : E_INVAL;
}

/* sceKernelIsProspero: the title runs as a PS4 title; libc's thread atexit
 * count and report hooks: nothing to count */
static s32 hle_zero(void) { return 0; }

#define LK "libkernel"
static const struct ext_hle_ent k_hle[] = {
    {LK, "sceKernelGetCompiledSdkVersion", hle_sdk_version, NULL},
    {LK, "sceKernelGetCompiledSdkVersionByPid", hle_sdk_version_by_pid, NULL},
    {LK, "sceKernelGetAppInfo", hle_get_app_info, NULL},
    {LK, "getargc", hle_getargc, NULL},
    {LK, "getargv", hle_getargv, NULL},
    {LK, "sceKernelDebugRaiseException", hle_raise, NULL},
    {LK, "sceKernelDebugRaiseExceptionOnReleaseMode", hle_raise_release, NULL},
    {LK, "sceKernelIsProspero", hle_zero, NULL},
    {LK, "_sceKernelSetThreadAtexitCount", hle_zero, NULL},
    {LK, "_sceKernelSetThreadAtexitReport", hle_zero, NULL},
    {"libSceAvPlayer", "sceAvPlayerInit", hle_av_init, NULL},
    {"libSceAvPlayer", "sceAvPlayerInitEx", hle_av_init_ex, NULL},
    {LK, "pthread_create", hle_pthread_create, NULL},
    {LK, "pthread_create_name_np", hle_pthread_create_name_np, NULL},
    {"libSceLibcInternalExt", "sceLibcHeapGetTraceInfo", hle_sceLibcHeapGetTraceInfo, NULL},
    {LK, "scePthreadExit", hle_scePthreadExit, NULL},
    {LK, "pthread_exit", hle_pthread_exit, NULL},
};

/* libkernel's other export libraries: the same functions (NIDs) under
 * other import names */
static const struct ext_lib k_libs[] = {
    {LK, "libScePosix libkernel_cpumode_platform libkernel_module_info libkernel_psmkit", 0, 0},
};

EXT_FAMILY_LIBS("kernel", k_hle, k_libs, NULL);
