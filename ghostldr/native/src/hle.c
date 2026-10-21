/* High-level overrides for imports the title must not reach directly inside
 * the SWRR host process. */
#include "ghost.h"

struct module *ghost_main_module(void);
void *host_real(const char *name);

static char g_app_root[256];

/* namespaces the extension adds (/temp0, /download0, ...), set during ext_init */
static struct {
    const char *prefix;
    const char *(*fn)(const char *path, char *buf, size_t size);
} g_remaps[4];
static u32 g_nremaps;

int path_remap_add(const char *prefix, const char *(*fn)(const char *path, char *buf, size_t size))
{
    if (!prefix || !*prefix || !fn || g_nremaps >= ARRAY_SIZE(g_remaps))
        return -1;
    g_remaps[g_nremaps].prefix = prefix;
    g_remaps[g_nremaps++].fn = fn;
    return 0;
}

/* "/app0/x" -> "<app_root>/x", then the extension's namespaces, then saves.
 * Other paths pass through. */
const char *remap_path(const char *path, char *buf, size_t size)
{
    if (!path)
        return path;
    if (!g_app_root[0] && g_ctx && g_ctx->app_root)
        strlcpy(g_app_root, (const char *)g_ctx->app_root, sizeof(g_app_root));
    if (!gfs_active() && strncmp(path, "/app0", 5) == 0 && (path[5] == '/' || path[5] == 0)) {
        fmt(buf, size, "%s%s", g_app_root, path + 5);
        return buf;
    }
    for (u32 i = 0; i < g_nremaps; i++) {
        const char *r = strncmp(path, g_remaps[i].prefix, strlen(g_remaps[i].prefix)) == 0
                            ? g_remaps[i].fn(path, buf, size)
                            : NULL;
        if (r)
            return r;
    }
    const char *savedata_remap(const char *path, char *buf, size_t size);
    const char *s = savedata_remap(path, buf, size);
    return s ? s : path;
}

#define REAL(name) ((u64(*)())host_real(name))

/* fds of movie files: their reads are logged (splash playback diagnosis) */
static volatile int g_watch_fd[8] = {-1, -1, -1, -1, -1, -1, -1, -1};

static bool watched(int fd)
{
    for (u32 i = 0; i < ARRAY_SIZE(g_watch_fd); i++)
        if (g_watch_fd[i] == fd)
            return true;
    return false;
}

static s64 hle_read(int fd, void *buf, size_t len)
{
    if (gfs_owns(fd))
        return gfs_read(fd, buf, len);
    s64 r = ((s64(*)(int, void *, size_t))host_real("sceKernelRead"))(fd, buf, len);
    if (watched(fd))
        logf("read(fd %x, %lx) -> %lx", fd, len, (u64)r);
    return r;
}

static s64 hle_pread(int fd, void *buf, size_t len, s64 off)
{
    if (gfs_owns(fd))
        return gfs_pread(fd, buf, len, off);
    s64 r = ((s64(*)(int, void *, size_t, s64))host_real("sceKernelPread"))(fd, buf, len, off);
    if (watched(fd))
        logf("pread(fd %x, %lx @ %lx) -> %lx", fd, len, (u64)off, (u64)r);
    return r;
}

static int hle_close(int fd)
{
    if (gfs_close(fd) == 0)
        return 0;
    if (watched(fd)) {
        logf("close(fd %x)", fd);
        for (u32 i = 0; i < ARRAY_SIZE(g_watch_fd); i++)
            if (g_watch_fd[i] == fd)
                g_watch_fd[i] = -1;
    }
    return ((int (*)(int))host_real("sceKernelClose"))(fd);
}

static int hle_open(const char *path, int flags, int mode)
{
    int gfd;
    if (gfs_open(path, flags, &gfd) == 0)
        return gfd;
    char buf[512];
    const char *p = remap_path(path, buf, sizeof(buf));
    int fd = ((int (*)(const char *, int, int))host_real("sceKernelOpen"))(p, flags, mode);
    static u32 opens;
    if (fd < 0 || __atomic_fetch_add(&opens, 1, __ATOMIC_RELAXED) < 400)
        logf("open %s (%s) flags %x -> %x", path, p, flags, fd);
    if (fd >= 0 && strstr(path, "movies/"))
        for (u32 i = 0; i < ARRAY_SIZE(g_watch_fd); i++)
            if (g_watch_fd[i] < 0) {
                g_watch_fd[i] = fd;
                break;
            }
    return fd;
}

static int hle_stat(const char *path, void *st)
{
    int grc;
    if (gfs_stat(path, st, &grc) == 0)
        return grc;
    char buf[512];
    return ((int (*)(const char *, void *))host_real("sceKernelStat"))(remap_path(path, buf, sizeof(buf)), st);
}

static s64 hle_lseek(int fd, s64 off, int whence)
{
    if (gfs_owns(fd))
        return gfs_lseek(fd, off, whence);
    return ((s64(*)(int, s64, int))host_real("sceKernelLseek"))(fd, off, whence);
}

static int hle_fstat(int fd, void *st)
{
    if (gfs_owns(fd))
        return gfs_fstat(fd, st);
    return ((int (*)(int, void *))host_real("sceKernelFstat"))(fd, st);
}

static int hle_getdents(int fd, void *buf, int n)
{
    if (gfs_owns(fd))
        return gfs_getdents(fd, buf, n);
    return ((int (*)(int, void *, int))host_real("sceKernelGetdents"))(fd, buf, n);
}

/* libSceFios2's directory listing */
static int hle_getdirentries(int fd, void *buf, int n, s64 *basep)
{
    if (gfs_owns(fd)) {
        if (basep)
            *basep = gfs_lseek(fd, 0, 1);
        return gfs_getdents(fd, buf, n);
    }
    return ((int (*)(int, void *, int, s64 *))host_real("sceKernelGetdirentries"))(fd, buf, n, basep);
}

/* libc's fread() path */
static s64 hle_posix_read(int fd, void *buf, size_t n)
{
    if (gfs_owns(fd)) {
        s64 r = gfs_read(fd, buf, n);
        return r < 0 ? -1 : r;
    }
    return ((s64(*)(int, void *, size_t))host_real("_read"))(fd, buf, n);
}

#define PATH1(fn, real, T)                                                          \
    static int fn(const char *path, T a)                                            \
    {                                                                               \
        char buf[512];                                                              \
        return ((int (*)(const char *, T))host_real(real))(remap_path(path, buf, sizeof(buf)), a); \
    }
PATH1(hle_mkdir, "sceKernelMkdir", int)
PATH1(hle_chmod, "sceKernelChmod", int)
PATH1(hle_truncate, "sceKernelTruncate", off_t)
PATH1(hle_utimes, "sceKernelUtimes", const void *)

static int hle_unlink(const char *path)
{
    char buf[512];
    return ((int (*)(const char *))host_real("sceKernelUnlink"))(remap_path(path, buf, sizeof(buf)));
}

static int hle_rmdir(const char *path)
{
    char buf[512];
    return ((int (*)(const char *))host_real("sceKernelRmdir"))(remap_path(path, buf, sizeof(buf)));
}

static int hle_rename(const char *from, const char *to)
{
    char a[512], b[512];
    return ((int (*)(const char *, const char *))host_real("sceKernelRename"))(
        remap_path(from, a, sizeof(a)), remap_path(to, b, sizeof(b)));
}

static void *hle_get_proc_param(void)
{
    struct module *m = ghost_main_module();
    return m ? (void *)m->proc_param : NULL;
}

static int hle_zero(void) { return 0; }

static int hle_dlsym(u32 handle, const char *name, void **out)
{
    for (struct module *m = g_modules; m; m = m->next) {
        if (m->fake_handle == handle) {
            /* NID of the plain name */
            extern void compute_nid(const char *name, char out[12]);
            char nid[12];
            compute_nid(name, nid);
            u64 a = module_export(m, nid);
            *out = (void *)a;
            logf("dlsym(%s, %s) -> %lx", m->name, name, a);
            return a ? 0 : -1;
        }
    }
    /* the extension's functions first; its no-host-fallback libraries are
     * refused rather than handed SWRR's */
    int erc = ext_dlsym(handle, name, out);
    if (erc <= 0) {
        logf("dlsym(host %x, %s) -> %s %p", handle, name, erc == 0 ? "extension" : "refused", *out);
        return erc == 0 ? 0 : (int)0x80020002; /* ENOENT */
    }
    int rc = (int)call_gadget6(g_ctx->dlsym, handle, (u64)name, (u64)out, 0, 0, 0);
    logf("dlsym(host %x, %s) -> %d %p", handle, name, rc, *out);
    return rc;
}

static int hle_load_start_module(const char *path, size_t argc, const void *argv, u32 flags, void *opt,
                                 int *res)
{
    const char *base = strrchr_c(path, '/');
    base = base ? base + 1 : path;
    for (struct module *m = g_modules; m; m = m->next)
        if (strcmp(m->name, base) == 0) {
            logf("LoadStartModule(%s) -> ours %x", path, m->fake_handle);
            if (res)
                *res = 0;
            return (int)m->fake_handle;
        }
    char buf[512];
    const char *p = remap_path(path, buf, sizeof(buf));
    int h = (int)call_gadget6((u64)K.load_start_module, (u64)p, argc, (u64)argv, flags, (u64)opt, (u64)res);
    logf("LoadStartModule(%s) -> host %x", path, h);
    return h;
}

static int hle_sysmodule_load(u32 id)
{
    int rc = ((int (*)(u32))host_real("sceSysmoduleLoadModule"))(id);
    logf("sceSysmoduleLoadModule(%x) -> %x", id, rc);
    return rc < 0 ? 0 : rc;
}

static int hle_sysmodule_load_internal_arg(u32 id, size_t argc, const void *argv, u32 flags, int *res)
{
    int rc = ((int (*)(u32, size_t, const void *, u32, int *))host_real(
        "sceSysmoduleLoadModuleInternalWithArg"))(id, argc, argv, flags, res);
    logf("sceSysmoduleLoadModuleInternalWithArg(%x) -> %x", id, rc);
    if (rc < 0) {
        if (res)
            *res = 0;
        return 0;
    }
    return rc;
}

static int hle_sysmodule_unload(u32 id)
{
    logf("sceSysmoduleUnloadModule(%x) ignored", id);
    return 0;
}

static int hle_discmap_on_hdd(const char *path, u64 off, u64 size, int *ret)
{
    if (ret)
        *ret = 1;
    return 0;
}

/* Ghost's stdout/stderr (printf, puts, asserts) goes to the UDP log, one
 * line per log record. */
static volatile int g_out_lock;
static char g_out_line[512];
static u32 g_out_len;

static s64 hle_write(int fd, const void *buf, size_t len)
{
    if (fd != 1 && fd != 2)
        return ((s64(*)(int, const void *, size_t))host_real("_write"))(fd, buf, len);
    const char *c = buf;
    spin_lock(&g_out_lock);
    for (size_t i = 0; i < len; i++) {
        if (c[i] == '\n' || g_out_len == sizeof(g_out_line) - 1) {
            g_out_line[g_out_len] = 0;
            logf("ghost%d: %s", fd, g_out_line);
            g_out_len = 0;
            if (c[i] == '\n')
                continue;
        }
        g_out_line[g_out_len++] = c[i];
    }
    spin_unlock(&g_out_lock);
    return (s64)len;
}

/* Flexible memory is scarce inside SWRR: back named/unnamed flexible
 * mappings with direct memory instead. */
static int hle_map_flex(void **addr, size_t len, int prot, int flags)
{
    u64 fixed = (flags & MAP_FIXED) ? (u64)*addr : 0;
    void *p = direct_alloc(len, prot ? prot : PROT_READ | PROT_WRITE, fixed);
    if (!p) {
        logf("flex->direct %lx failed", len);
        return 0x8002000c;
    }
    *addr = p;
    return 0;
}

static int hle_map_named_flex(void **addr, size_t len, int prot, int flags, const char *name)
{
    int rc = hle_map_flex(addr, len, prot, flags);
    logf("MapNamedFlexibleMemory(%s, %lx) -> %x at %p", name ? name : "", len, rc, *addr);
    return rc;
}

/* ---- memory API tracing (pass-through) ---- */
/* The title sees the direct-memory size budget.c planned for it (see there);
 * allocation search ranges are clamped to the real size. */

/* Direct allocations of the title and how much of each it has mapped
 * (map-direct operations only; unmaps are not subtracted), for the watchdog. */
struct dm_usage g_dm_usage[32];
u32 g_dm_nusage;

static void usage_add_alloc(u64 phys, u64 len, int type, u64 from)
{
    u32 i = __atomic_fetch_add(&g_dm_nusage, 1, __ATOMIC_SEQ_CST);
    if (i < ARRAY_SIZE(g_dm_usage))
        g_dm_usage[i] = (struct dm_usage){phys, len, 0, from, type};
}

static void usage_add_map(u64 phys, u64 len)
{
    for (u32 i = 0; i < g_dm_nusage && i < ARRAY_SIZE(g_dm_usage); i++)
        if (phys >= g_dm_usage[i].phys && phys < g_dm_usage[i].phys + g_dm_usage[i].len) {
            __atomic_fetch_add(&g_dm_usage[i].mapped, len, __ATOMIC_RELAXED);
            return;
        }
}

static int hle_alloc_direct(off_t start, off_t end, size_t len, size_t align, int type, off_t *out)
{
    size_t real_size = ((size_t (*)(void))host_real("sceKernelGetDirectMemorySize"))();
    /* the title bounds its searches by the size it was told (budget.c), but
     * the free physical memory lies anywhere below the real size */
    if ((u64)end > real_size || (g_reported_dmem && (u64)end >= g_reported_dmem))
        end = (off_t)real_size;
    u64 ra = (u64)__builtin_return_address(0);
    u64 site = g_main_mod ? ra - g_main_mod->base : ra;
    int rc;
    if (budget_lazy_site(site))
        rc = vdm_alloc_lazy(len, type, out);
    else
        rc = ((int (*)(off_t, off_t, size_t, size_t, int, off_t *))host_real("sceKernelAllocateDirectMemory"))(
            start, end, len, align, type, out);
    if (rc != 0 && len >= 0x200000) /* no single hole is big enough: scatter it (vdm.c) */
        rc = vdm_alloc((u64)start, (u64)end, len, align, type, out);
    if (rc == 0 && out)
        usage_add_alloc((u64)*out, len, type, g_main_mod ? ra - g_main_mod->base : ra);
    logf("AllocateDirectMemory(%lx-%lx, len %lx, align %lx, type %d) -> %x phys %lx from eboot+%lx", (u64)start,
         (u64)end, len, align, type, rc, out ? (u64)*out : 0, g_main_mod ? ra - g_main_mod->base : ra);
    return rc;
}

static int hle_map_direct(void **addr, size_t len, int prot, int flags, off_t phys, size_t align)
{
    void *want = addr ? *addr : NULL;
    int rc = vdm_is((u64)phys) ? vdm_map(addr, len, prot, flags, (u64)phys, align)
                               : ((int (*)(void **, size_t, int, int, off_t, size_t))host_real(
                                     "sceKernelMapDirectMemory"))(addr, len, prot, flags, phys, align);
    if (rc == 0)
        usage_add_map((u64)phys, len);
    logf("MapDirectMemory(want %p, len %lx, prot %x, flags %x, phys %lx, align %lx) -> %x at %p", want, len,
         prot, flags, (u64)phys, align, rc, addr ? *addr : NULL);
    return rc;
}

struct batch_entry {
    void *start;
    off_t offset;
    size_t length;
    u8 prot, type;
    s16 reserved;
    s32 op;
};

/* entries that map scattered or lazy allocations (vdm.c) become one entry
 * per physical piece; submitted in windows so any batch size fits */
static int batch_run(const struct batch_entry *e, int n, int *done, int flags, bool v2)
{
    bool any = false;
    for (int i = 0; i < n; i++)
        any |= e[i].op == 0 && vdm_is((u64)e[i].offset);
    if (!any)
        return v2 ? ((int (*)(const void *, int, int *, int))host_real("sceKernelBatchMap2"))(e, n, done, flags)
                  : ((int (*)(const void *, int, int *))host_real("sceKernelBatchMap"))(e, n, done);
    struct batch_entry buf[80];
    int fill = 0, rc = 0, sub = 0;
    for (int i = 0; i <= n && rc == 0; i++) {
        bool last = i == n;
        int need = 0;
        struct batch_entry tmp[64];
        if (!last) {
            need = vdm_expand_entry(&e[i], tmp, ARRAY_SIZE(tmp));
            if (need < 0) {
                rc = 0x8002000c;
                break;
            }
        }
        if (fill && (last || fill + need > (int)ARRAY_SIZE(buf))) {
            rc = v2 ? ((int (*)(const void *, int, int *, int))host_real("sceKernelBatchMap2"))(buf, fill, &sub, flags)
                    : ((int (*)(const void *, int, int *))host_real("sceKernelBatchMap"))(buf, fill, &sub);
            fill = 0;
        }
        for (int k = 0; k < need; k++)
            buf[fill++] = tmp[k];
    }
    if (done)
        *done = rc == 0 ? n : 0;
    return rc;
}

static void usage_batch(const struct batch_entry *e, int n)
{
    for (int i = 0; i < n; i++)
        if (e[i].op == 0)
            usage_add_map((u64)e[i].offset, e[i].length);
}

static int hle_batch_map(const struct batch_entry *e, int n, int *done)
{
    usage_batch(e, n);
    int rc = batch_run(e, n, done, 0, false);
    logf("BatchMap(%d entries, first op %d start %p len %lx) -> %x done %d", n, n ? e[0].op : -1,
         n ? e[0].start : NULL, n ? e[0].length : 0, rc, done ? *done : -1);
    return rc;
}

static int hle_batch_map2(const struct batch_entry *e, int n, int *done, int flags)
{
    usage_batch(e, n);
    int rc = batch_run(e, n, done, flags, true);
    logf("BatchMap2(%d entries, first op %d start %p len %lx flags %x) -> %x done %d", n, n ? e[0].op : -1,
         n ? e[0].start : NULL, n ? e[0].length : 0, flags, rc, done ? *done : -1);
    return rc;
}

static int hle_release_direct(off_t start, size_t len)
{
    int rc = vdm_is((u64)start) ? vdm_release((u64)start, len)
                                : ((int (*)(off_t, size_t))host_real("sceKernelReleaseDirectMemory"))(start, len);
    logf("ReleaseDirectMemory(%lx, %lx) -> %x", (u64)start, len, rc);
    return rc;
}

static int hle_mprotect(const void *addr, size_t len, int prot)
{
    int rc = ((int (*)(const void *, size_t, int))host_real("sceKernelMprotect"))(addr, len, prot);
    if (rc != 0)
        logf("Mprotect(%p, %lx, %x) -> %x", addr, len, prot, rc);
    return rc;
}

static size_t hle_direct_size(void)
{
    size_t v = ((size_t (*)(void))host_real("sceKernelGetDirectMemorySize"))();
    struct module *m = ghost_main_module();
    if (m && m->image_size > 0x28e24e8) {
        /* CUSA11456 memory planner inputs (eboot 2.18): config pool sizes */
        u32 *cfg = (u32 *)(m->base + 0x28e24d8);
        u64 *pools = (u64 *)(m->base + 0x16b29c8), *pool2 = (u64 *)(m->base + 0x16b2a90);
        logf("GetDirectMemorySize real %lx reported %lx (cfg %x %x %x %x, pools %lx %lx)", v, g_reported_dmem,
             cfg[0], cfg[1], cfg[2], cfg[3], *pools, *pool2);
    }
    return g_reported_dmem ? g_reported_dmem : v;
}

static int hle_is_prospero(void) { return 0; } /* present as a PS4 */
static int hle_one(void) { return 1; }

static int hle_open_psid(u8 *out)
{
    if (out)
        for (int i = 0; i < 16; i++)
            out[i] = (u8)(0x47 + i);
    return 0;
}

static int hle_privacy_setting(int type, int *value)
{
    if (value)
        *value = 0;
    return 0;
}

struct hle_ent {
    const char *lib;
    const char *name;
    void *fn;
};

#define LK "libkernel"
static const struct hle_ent g_hle[] = {
    {LK, "sceKernelGetProcParam", hle_get_proc_param},
    {LK, "__tls_get_addr", tls_get_addr},
    {LK, "scePthreadCreate", ghost_thread_create},
    {LK, "_sceKernelRtldSetApplicationHeapAPI", hle_zero},
    {LK, "_sceKernelSetThreadDtors", hle_zero},
    {LK, "_sceKernelSetThreadAtexitCount", hle_zero},
    {LK, "_sceKernelSetThreadAtexitReport", hle_zero},
    {LK, "sceKernelOpen", hle_open},
    {LK, "sceKernelRead", hle_read},
    {LK, "sceKernelPread", hle_pread},
    {LK, "sceKernelClose", hle_close},
    {LK, "_write", hle_write},
    {LK, "write", hle_write},
    {LK, "sceKernelWrite", hle_write},
    {LK, "sceKernelStat", hle_stat},
    {LK, "sceKernelLseek", hle_lseek},
    {LK, "sceKernelFstat", hle_fstat},
    {LK, "sceKernelGetdents", hle_getdents},
    {LK, "sceKernelGetdirentries", hle_getdirentries},
    {LK, "_read", hle_posix_read},
    {LK, "sceKernelMkdir", hle_mkdir},
    {LK, "sceKernelChmod", hle_chmod},
    {LK, "sceKernelTruncate", hle_truncate},
    {LK, "sceKernelUtimes", hle_utimes},
    {LK, "sceKernelUnlink", hle_unlink},
    {LK, "sceKernelRmdir", hle_rmdir},
    {"libScePosix", "rmdir", hle_rmdir},
    {LK, "rmdir", hle_rmdir},
    {LK, "sceKernelRename", hle_rename},
    {LK, "sceKernelDlsym", hle_dlsym},
    {LK, "sceKernelLoadStartModule", hle_load_start_module},
    {LK, "sceKernelMapFlexibleMemory", hle_map_flex},
    {"libkernel_cpumode_platform", "sceKernelIsProspero", hle_is_prospero},
    {LK, "sceKernelAllocateDirectMemory", hle_alloc_direct},
    {LK, "sceKernelMapDirectMemory", hle_map_direct},
    {LK, "sceKernelBatchMap", hle_batch_map},
    {LK, "sceKernelBatchMap2", hle_batch_map2},
    {LK, "sceKernelReleaseDirectMemory", hle_release_direct},
    {LK, "sceKernelMprotect", hle_mprotect},
    {LK, "sceKernelGetDirectMemorySize", hle_direct_size},
    {"libSceNetCtlForNpToolkit", "sceNetCtlCheckCallbackForNpToolkit", hle_zero},
    {"libSceNetCtlForNpToolkit", "sceNetCtlRegisterCallbackForNpToolkit", hle_zero},
    {"libSceNetCtlForNpToolkit", "sceNetCtlUnregisterCallbackForNpToolkit", hle_zero},
    {"libSceNpManagerForToolkit", "sceNpRegisterStateCallbackForToolkit", hle_one},
    {"libSceNpManagerForToolkit", "sceNpUnregisterStateCallbackForToolkit", hle_zero},
    {"libSceOpenPsId", "sceKernelGetOpenPsId", hle_open_psid},
    {"libSceSystemServicePlatformPrivacy", "sceSystemServiceGetPlatformPrivacySetting", hle_privacy_setting},
    {"libSceCoredump", "sceCoredumpRegisterCoredumpHandler", hle_zero},
    {"libSceCoredump", "sceCoredumpGetStopInfoCpu", hle_zero},
    {"libSceCoredump", "sceCoredumpGetStopInfoGpu", hle_zero},
    {"libSceCoredump", "sceCoredumpAttachMemoryRegionAsUserFile", hle_zero},
    {"libSceCoredump", "sceCoredumpSetUserDataType", hle_zero},
    {"libSceCoredump", "sceCoredumpWriteUserData", hle_zero},
    {"libSceLibcInternalExt", "sceLibcHeapGetTraceInfo", hle_zero},
    {LK, "sceKernelMapNamedFlexibleMemory", hle_map_named_flex},
    {"libSceSysmodule", "sceSysmoduleLoadModule", hle_sysmodule_load},
    {"libSceSysmodule", "sceSysmoduleLoadModuleInternalWithArg", hle_sysmodule_load_internal_arg},
    {"libSceSysmodule", "sceSysmoduleUnloadModule", hle_sysmodule_unload},
    {"libSceDiscMap", "sceDiscMapIsRequestOnHDD", hle_discmap_on_hdd},
};

u64 hle_sys_lookup(const char *lib, const char *name);

u64 hle_lookup(const char *lib, const char *name)
{
    for (u32 i = 0; i < ARRAY_SIZE(g_hle); i++)
        if (strcmp(g_hle[i].name, name) == 0 && (!lib || strcmp(g_hle[i].lib, lib) == 0))
            return (u64)g_hle[i].fn;
    return hle_sys_lookup(lib, name);
}

/* real host libkernel / sysmodule functions, cached */
struct real_ent {
    const char *name;
    void *fn;
};
static struct real_ent g_real[64];
static u32 g_nreal;
static volatile int g_real_lock;

void *host_real(const char *name)
{
    for (u32 i = 0; i < g_nreal; i++)
        if (g_real[i].name == name || strcmp(g_real[i].name, name) == 0)
            return g_real[i].fn;
    u32 handle = (u32)g_ctx->libkernel_handle;
    if (strncmp(name, "sceSysmodule", 12) == 0)
        host_module_handle("libSceSysmodule", &handle);
    void *fn = host_dlsym(handle, name);
    if (!fn)
        logf("host_real: %s missing", name);
    spin_lock(&g_real_lock);
    if (g_nreal < ARRAY_SIZE(g_real))
        g_real[g_nreal++] = (struct real_ent){name, fn};
    spin_unlock(&g_real_lock);
    return fn;
}
