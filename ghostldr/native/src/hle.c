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

/* the extension's path translations (the save data family's /savedataN):
 * each returns NULL for paths that are not its own */
typedef const char *(*remap_fn)(const char *path, char *buf, size_t size);
static remap_fn g_remap_hooks[4];

int remap_hook_add(remap_fn fn)
{
    for (u32 i = 0; i < ARRAY_SIZE(g_remap_hooks); i++)
        if (!g_remap_hooks[i]) {
            g_remap_hooks[i] = fn;
            return 0;
        }
    return -1;
}

/* "/app0/x" -> "<app_root>/x", then the extension's namespaces and hooks, then
 * saves.  Other paths pass through. */
const char *remap_path(const char *path, char *buf, size_t size)
{
    if (!path)
        return path;
    for (u32 i = 0; i < ARRAY_SIZE(g_remap_hooks) && g_remap_hooks[i]; i++) {
        const char *h = g_remap_hooks[i](path, buf, size);
        if (h)
            return h;
    }
    if (!g_app_root[0] && g_ctx && g_ctx->app_root)
        strlcpy(g_app_root, (const char *)g_ctx->app_root, sizeof(g_app_root));
    if (!gfs_app0_virtual() && strncmp(path, "/app0", 5) == 0 && (path[5] == '/' || path[5] == 0)) {
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

static void *hle_get_proc_param(void)
{
    struct module *m = ghost_main_module();
    return m ? (void *)m->proc_param : NULL;
}

static int hle_zero(void) { return 0; }

static int hle_discmap_on_hdd(const char *path, u64 off, u64 size, int *ret)
{
    if (ret)
        *ret = 1;
    return 0;
}

int budget_alloc_gate(u64 len);
void budget_alloc_note(s64 len);
int budget_flex_gate(u64 len);
void budget_flex_note(s64 len);

/* A title's new memory reads as zeros, as a fresh process's does on a PS4;
 * the physical pages ghostldr hands out held SWRR's emulator buffers or its
 * own data before (homebrew draws over framebuffers it never cleared).
 * Planned titles too: Bloodborne's engine heaps panic on stale contents
 * ("DLLightMutex.cpp(126) Mutex is not initialized").  Lazily backed pools
 * stay uncommitted: vdm.c zeroes each chunk as it commits it. */
static bool zero_new_memory(void) { return true; }

/* Flexible memory is scarce inside SWRR: back named/unnamed flexible
 * mappings with direct memory instead, within the title's flex budget
 * (budget.c). */
int hle_map_flex(void **addr, size_t len, int prot, int flags)
{
    int rc = budget_flex_gate(len);
    if (rc)
        return rc;
    u64 fixed = (flags & MAP_FIXED) ? (u64)*addr : 0;
    bool zero = zero_new_memory();
    int want = prot ? prot : PROT_READ | PROT_WRITE;
    void *p = direct_alloc(len, zero ? PROT_READ | PROT_WRITE : want, fixed);
    if (!p) {
        logf("flex->direct %lx failed", len);
        return 0x8002000c;
    }
    if (zero) {
        memset(p, 0, ALIGN_UP(len, 0x4000));
        if (want != (PROT_READ | PROT_WRITE))
            K.mprotect(p, ALIGN_UP(len, 0x4000), want);
    }
    budget_flex_note((s64)ALIGN_UP(len, 0x4000));
    *addr = p;
    return 0;
}

/* zero a new direct allocation through a temporary read-write mapping */
static void zero_direct(off_t off, size_t len)
{
    void *va = NULL;
    int rc = vdm_is((u64)off) ? vdm_map(&va, len, PROT_READ | PROT_WRITE, 0, (u64)off, 0x200000)
                              : K.map_direct(&va, len, PROT_READ | PROT_WRITE, 0, off, 0x4000);
    if (rc != 0) {
        static u32 said;
        if (said++ < 4)
            logf("AllocateDirectMemory: could not map %lx+%lx to clear it: %x", (u64)off, len, rc);
        return;
    }
    memset(va, 0, len);
    K.munmap(va, len);
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

int hle_alloc_direct(off_t start, off_t end, size_t len, size_t align, int type, off_t *out)
{
    int gate = budget_alloc_gate(len); /* plan-free: the reported size is the budget */
    if (gate)
        return gate;
    size_t real_size = ((size_t (*)(void))host_real("sceKernelGetDirectMemorySize"))();
    /* the title bounds its searches by the size it was told (budget.c), but
     * the free physical memory lies anywhere below the real size */
    if ((u64)end > real_size || (g_reported_dmem && (u64)end >= g_reported_dmem))
        end = (off_t)real_size;
    u64 ra = (u64)__builtin_return_address(0);
    u64 site = g_main_mod ? ra - g_main_mod->base : ra;
    int rc;
    bool lazy = budget_lazy_site(site) || automem_lazy(len); /* committed as it is mapped (vdm.c) */
    if (lazy && (rc = vdm_alloc_lazy(len, type, zero_new_memory(), out)) != 0)
        lazy = false;
    if (!lazy)
        rc = ((int (*)(off_t, off_t, size_t, size_t, int, off_t *))host_real("sceKernelAllocateDirectMemory"))(
            start, end, len, align, type, out);
    bool planfree = !budget_planned(); /* its offsets are handles: any free memory will do */
    if (rc != 0 && planfree && (start || (u64)end < real_size))
        rc = ((int (*)(off_t, off_t, size_t, size_t, int, off_t *))host_real("sceKernelAllocateDirectMemory"))(
            0, (off_t)real_size, len, align, type, out);
    if (rc != 0 && len >= 0x200000) /* no single hole is big enough: scatter it (vdm.c) */
        rc = vdm_alloc(planfree ? 0 : (u64)start, planfree ? real_size : (u64)end, len, align, type, out);
    if (rc == 0 && out && zero_new_memory() && !lazy)
        zero_direct(*out, len);
    if (rc == 0)
        budget_alloc_note((s64)len);
    if (rc == 0 && out) {
        usage_add_alloc((u64)*out, len, type, site);
        automem_note_alloc(site, len, type, lazy, (u64)*out);
    }
    logf("AllocateDirectMemory(%lx-%lx, len %lx, align %lx, type %d) -> %x phys %lx from eboot+%lx", (u64)start,
         (u64)end, len, align, type, rc, out ? (u64)*out : 0, g_main_mod ? ra - g_main_mod->base : ra);
    if (rc != 0) { /* refused with a block this large free: the kernel's limit, not the holes (docs/ps4.md) */
        off_t s = 0;
        size_t avail = 0;
        if (K.avail_direct(0, (off_t)real_size, align > 0x4000 ? align : 0x4000, &s, &avail) == 0)
            logf("AllocateDirectMemory: the kernel's largest free block is %lx-%lx (%lu MiB)", (u64)s,
                 (u64)s + avail, (u64)avail >> 20);
    }
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
    automem_note_release((u64)start, len);
    int rc = vdm_is((u64)start) ? vdm_release((u64)start, len)
                                : ((int (*)(off_t, size_t))host_real("sceKernelReleaseDirectMemory"))(start, len);
    if (rc == 0)
        budget_alloc_note(-(s64)len);
    logf("ReleaseDirectMemory(%lx, %lx) -> %x", (u64)start, len, rc);
    return rc;
}

/* The other direct-memory calls, for synthetic offsets (vdm.c: lazy or
 * scattered allocations); anything else goes to the kernel.  Resolved by
 * NID for every title (budget.c). */
int hle_map_named_direct(void **addr, size_t len, int prot, int flags, off_t phys, size_t align, const char *name)
{
    if (!vdm_is((u64)phys))
        return ((int (*)(void **, size_t, int, int, off_t, size_t, const char *))host_real(
            "sceKernelMapNamedDirectMemory"))(addr, len, prot, flags, phys, align, name);
    void *want = addr ? *addr : NULL;
    int rc = vdm_map(addr, len, prot, flags, (u64)phys, align);
    if (rc == 0)
        usage_add_map((u64)phys, len);
    logf("MapNamedDirectMemory(%s, want %p, len %lx, prot %x, flags %x, phys %lx) -> %x at %p", name ? name : "",
         want, len, prot, flags, (u64)phys, rc, addr ? *addr : NULL);
    return rc;
}

/* a lazy or scattered allocation keeps the type it was allocated with */
int hle_map_direct2(void **addr, size_t len, int type, int prot, int flags, off_t phys, size_t align)
{
    if (!vdm_is((u64)phys))
        return ((int (*)(void **, size_t, int, int, int, off_t, size_t))host_real("sceKernelMapDirectMemory2"))(
            addr, len, type, prot, flags, phys, align);
    void *want = addr ? *addr : NULL;
    int rc = vdm_map(addr, len, prot, flags, (u64)phys, align);
    if (rc == 0)
        usage_add_map((u64)phys, len);
    logf("MapDirectMemory2(want %p, len %lx, type %d, prot %x, flags %x, phys %lx) -> %x at %p", want, len, type,
         prot, flags, (u64)phys, rc, addr ? *addr : NULL);
    return rc;
}

int hle_checked_release_direct(off_t start, size_t len)
{
    if (!vdm_is((u64)start))
        return ((int (*)(off_t, size_t))host_real("sceKernelCheckedReleaseDirectMemory"))(start, len);
    return hle_release_direct(start, len);
}

struct dmq_info {
    off_t start, end;
    int type;
};

/* the kernel's answer for its own offsets; synthetic ones, and with
 * find-next the synthetic allocations after the kernel's last one */
int hle_dmem_query(off_t off, int flags, struct dmq_info *info, size_t size)
{
    int rc = vdm_is((u64)off) ? (int)0x80020016
                              : ((int (*)(off_t, int, struct dmq_info *, size_t))host_real(
                                    "sceKernelDirectMemoryQuery"))(off, flags, info, size);
    u64 s, e;
    int t;
    if (rc != 0 && info && size >= sizeof(*info) && (vdm_is((u64)off) || (flags & 1)) &&
        vdm_query((u64)off, flags & 1, &s, &e, &t) == 0) {
        info->start = (off_t)s, info->end = (off_t)e, info->type = t;
        rc = 0;
    }
    return rc;
}

int hle_dmem_type(off_t off, int *type, off_t *start, off_t *end)
{
    u64 s, e;
    int t;
    if (!vdm_is((u64)off))
        return ((int (*)(off_t, int *, off_t *, off_t *))host_real("sceKernelGetDirectMemoryType"))(off, type, start,
                                                                                                  end);
    if (vdm_query((u64)off, false, &s, &e, &t) != 0)
        return (int)0x80020016;
    if (type)
        *type = t;
    if (start)
        *start = (off_t)s;
    if (end)
        *end = (off_t)e;
    return 0;
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
    if (m && m->image_size > 0x28e24e8 && profile_has(PF_DIAG)) {
        /* CUSA11456 memory planner inputs (eboot 2.18): config pool sizes */
        u32 *cfg = (u32 *)(m->base + 0x28e24d8);
        u64 *pools = (u64 *)(m->base + 0x16b29c8), *pool2 = (u64 *)(m->base + 0x16b2a90);
        logf("GetDirectMemorySize real %lx reported %lx (cfg %x %x %x %x, pools %lx %lx)", v, g_reported_dmem,
             cfg[0], cfg[1], cfg[2], cfg[3], *pools, *pool2);
    }
    return g_reported_dmem ? g_reported_dmem : v;
}

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
    {LK, "_sceKernelSetThreadDtors", tls_set_thread_dtors},
    {LK, "sceKernelMapFlexibleMemory", hle_map_flex},
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
    {LK, "sceKernelMapNamedFlexibleMemory", hle_map_named_flex},
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
