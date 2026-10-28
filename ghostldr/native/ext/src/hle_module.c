/*
 * The title's modules as the title sees them: loading a prx at any time,
 * dlsym, and (module queries) what the kernel would say about modules it
 * never loaded.  SWRR's kernel knows only SWRR's modules, so calls about a
 * title module are answered from the loader's registry (g_modules) and
 * everything else passes through (kernel-modules.md §4.A, §6.3).
 */
#include "kmod.h"

struct module *mod_by_handle(s32 handle)
{
    for (struct module *m = g_modules; m; m = m->next)
        if ((s32)m->fake_handle == handle)
            return m;
    return NULL;
}

struct module *mod_by_name(const char *file)
{
    for (struct module *m = g_modules; m; m = m->next)
        if (strcmp(m->name, file) == 0)
            return m;
    return NULL;
}

struct module *mod_by_addr(u64 addr)
{
    for (struct module *m = g_modules; m; m = m->next)
        if (addr >= m->base && addr < m->base + m->image_size)
            return m;
    return NULL;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr_c(path, '/');
    return s ? s + 1 : path;
}

bool title_vpath(const char *path, char *out, size_t size)
{
    char title[512];
    u8 st[0x78];
    size_t len = strlen(path);
    bool bare = !strrchr_c(path, '/') && len > 4 && strcmp(path + len - 4, ".prx") == 0;
    if (!bare && strncmp(path, "/app0/", 6) != 0)
        return false;
    for (int k = 0; k < (bare ? 2 : 1); k++) {
        if (bare)
            fmt(title, sizeof(title), k ? "/app0/sce_module/%s" : "/app0/%s", path);
        else
            strlcpy(title, path, sizeof(title));
        const char *v = remap_path(title, out, size); /* the app root unless ghostfs serves /app0 */
        if (v != out)
            strlcpy(out, v, size);
        if (vfs_stat(out, st) == 0)
            return true;
    }
    return false;
}

/* ---- virtual modules: libraries the extension is all of (EXT_LIB_VIRTUAL) ----
 * Handles 0x7F00 + i: above the title's own (loader.c counts up from 0x7000),
 * far below the kernel's. */
#define VMOD_BASE 0x7F00
#define VMOD_MAX 16
static char g_vmod[VMOD_MAX][48];
static volatile int g_vmod_lock;

void fsvsh_loaded(void) HIDDEN __attribute__((weak)); /* hle_fsvsh.c; not in the ELF loader's extension */

/* the handle of path's module when the extension is the whole library, else 0 */
static s32 vmod_handle(const char *path)
{
    char lib[48];
    strlcpy(lib, base_name(path), sizeof(lib));
    char *dot = (char *)strrchr_c(lib, '.');
    if (dot && (strcmp(dot, ".sprx") == 0 || strcmp(dot, ".prx") == 0))
        *dot = 0;
    const struct ext_lib *pol = ext_find_policy(lib);
    if (!pol || !(pol->flags & EXT_LIB_VIRTUAL))
        return 0;
    s32 h = 0;
    spin_lock(&g_vmod_lock);
    for (u32 i = 0; i < VMOD_MAX && !h; i++)
        if (strcmp(g_vmod[i], lib) == 0)
            h = VMOD_BASE + (s32)i;
    for (u32 i = 0; i < VMOD_MAX && !h; i++)
        if (!g_vmod[i][0]) {
            strlcpy(g_vmod[i], lib, sizeof(g_vmod[i]));
            h = VMOD_BASE + (s32)i;
        }
    spin_unlock(&g_vmod_lock);
    return h;
}

static bool vmod_is(s32 handle) { return handle >= VMOD_BASE && handle < VMOD_BASE + VMOD_MAX && g_vmod[handle - VMOD_BASE][0]; }

/* s32 sceKernelLoadStartModule(path, argc, argp, flags, opt, s32 *res):
 * the title's own prx load through ghostldr's loader (SWRR's kernel cannot
 * read ghostfs files or load a fake-signed /app0 prx) and start on the
 * calling thread; system modules (anything else) go to the kernel. */
static s32 hle_load_start_module(const char *path, u64 argc, const void *argp, u32 flags, const void *opt,
                                 s32 *res)
{
    if (!path)
        return E_FAULT;
    char vpath[512];
    struct module *m = mod_by_name(base_name(path));
    if (!m && title_vpath(path, vpath, sizeof(vpath))) {
        int err = 0;
        m = module_load_runtime(vpath, &err);
        if (!m) {
            logf("LoadStartModule(%s) -> %x", path, err);
            return err;
        }
    }
    if (m) {
        s32 rc = module_start(m, argc, argp);
        if (res)
            *res = rc;
        logf("LoadStartModule(%s) -> ours %x (start %x)", path, m->fake_handle, rc);
        return (s32)m->fake_handle;
    }
    s32 vh = vmod_handle(path);
    if (vh) {
        if (res)
            *res = 0;
        logf("LoadStartModule(%s) -> extension's virtual module %x", path, vh);
        if (fsvsh_loaded && strstr(path, "libSceFsInternalForVsh"))
            fsvsh_loaded(); /* a save tool: hle_fsvsh.c starts looking for saves */
        return vh;
    }
    char buf[512];
    const char *p = remap_path(path, buf, sizeof(buf));
    s32 h = (s32)call_gadget6((u64)K.load_start_module, (u64)p, argc, (u64)argp, flags, (u64)opt, (u64)res);
    logf("LoadStartModule(%s) -> host %x", path, h);
    return h;
}

/* A title module is never unloaded: its code and data stay where they are. */
static s32 hle_stop_unload_module(s32 handle, u64 argc, const void *argp, u32 flags, const void *opt, s32 *res)
{
    struct module *m = mod_by_handle(handle);
    if (m) {
        logf("StopUnloadModule(%s) kept loaded", m->name);
        if (res)
            *res = 0;
        return 0;
    }
    if (vmod_is(handle)) {
        if (res)
            *res = 0;
        return 0;
    }
    s32 (*real)(s32, u64, const void *, u32, const void *, s32 *) = host_real("sceKernelStopUnloadModule");
    return real ? real(handle, argc, argp, flags, opt, res) : E_SRCH;
}

static bool is_nid(const char *s)
{
    if (strlen(s) != 11)
        return false;
    for (const char *c = s; *c; c++)
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '+' ||
              *c == '-'))
            return false;
    return true;
}

/* s32 sceKernelDlsym(s32 handle, const char *name, void **addr) */
static s32 hle_dlsym(s32 handle, const char *name, void **out)
{
    if (!name || !out)
        return E_FAULT;
    char nid[12];
    compute_nid(name, nid);
    struct module *m = mod_by_handle(handle);
    if (m) {
        u64 a = module_export(m, nid);
        if (!a && is_nid(name)) /* an encoded NID, as shadPS4 accepts */
            a = module_export(m, name);
        *out = (void *)a;
        logf("dlsym(%s, %s) -> %lx", m->name, name, a);
        return a ? 0 : E_SRCH;
    }
    /* the extension's functions first; a no-host-fallback library's module
     * is refused rather than handed SWRR's */
    u64 fn = ext_find(NULL, nid);
    if (vmod_is(handle)) {
        /* libkernel_sys is libkernel on a PS4 (loading it gives libkernel's
         * handle): what the extension lacks is libkernel's own, e.g.
         * sceKernelGetFsSandboxRandomWord (PS4-Xplorer called the NULL it
         * got and SWRR ended) */
        if (!fn && strncmp(g_vmod[handle - VMOD_BASE], "libkernel", 9) == 0) {
            void *real = NULL;
            if (call_gadget6(g_ctx->dlsym, (u32)g_ctx->libkernel_handle, (u64)name, (u64)&real, 0, 0, 0) == 0)
                fn = (u64)real;
        }
        *out = (void *)fn;
        static u32 said;
        if (!fn || said++ < 16)
            logf("dlsym(%s, %s) -> extension %p", g_vmod[handle - VMOD_BASE], name, *out);
        return fn ? 0 : E_SRCH;
    }
    if (fn) {
        *out = (void *)fn;
        logf("dlsym(host %x, %s) -> extension %p", handle, name, *out);
        return 0;
    }
    struct sce_module_info info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (K.module_info && K.module_info((u32)handle, &info) == 0) {
        char *dot = (char *)strrchr_c(info.name, '.');
        if (dot)
            *dot = 0; /* libSceNpTrophy.sprx -> libSceNpTrophy */
        const struct ext_lib *pol = ext_find_policy(info.name);
        if (pol && (pol->flags & EXT_LIB_NO_HOST)) {
            *out = NULL;
            logf("dlsym(host %x, %s) -> refused (%s has no host fallback)", handle, name, pol->name);
            return E_NOENT;
        }
    }
    s32 rc = (s32)call_gadget6(g_ctx->dlsym, (u32)handle, (u64)name, (u64)out, 0, 0, 0);
    logf("dlsym(host %x, %s) -> %x %p", handle, name, rc, *out);
    return rc;
}

/* ---- module queries (kernel-modules.md §5.1-5.3, §8) ----
 * C++ exceptions unwind through the title's frames with these (the title's
 * libc asks sceKernelGetModuleInfoForUnwind for every frame), and
 * thread_local destructors register through FromAddr and the rtld atexit
 * counters.  Addresses and handles outside the title's modules pass through. */

/* eh_frame of a module, from its .eh_frame_hdr (in its text, readable) */
static bool eh_frame(const struct module *m, u64 *frame, u64 *size)
{
    if (!m->eh_frame_hdr)
        return false;
    const u8 *h = (const u8 *)(m->base + m->eh_frame_hdr);
    u64 at;
    if (h[0] != 1)
        return false;
    if (h[1] == 0x1B) /* pcrel sdata4 */
        at = (u64)(h + 4) + (u64)(s64) * (const s32 *)(h + 4);
    else if (h[1] == 0x03) /* udata4 */
        at = m->base + *(const u32 *)(h + 4);
    else
        return false;
    u64 hdr = (u64)h;
    *frame = at;
    *size = hdr > at ? hdr - at : m->base + m->text_size - at;
    return true;
}

static void put_name(u8 *info, const struct module *m) { strlcpy((char *)info + 8, m->name, 256); }

/* {u64 addr; u32 size; s32 prot} x 4, then the count */
static void put_segs(u8 *at, u32 *count, const struct module *m)
{
    for (u32 i = 0; i < m->nsegs; i++) {
        *(u64 *)(at + 16 * i) = m->base + m->segs[i].vaddr;
        *(u32 *)(at + 16 * i + 8) = m->segs[i].size;
        *(s32 *)(at + 16 * i + 12) = m->segs[i].prot;
    }
    *count = m->nsegs;
}

/* SceKernelModuleInfo, 0x160 bytes */
static void fill_info(u8 *info, const struct module *m)
{
    memset(info + 8, 0, 0x160 - 8);
    put_name(info, m);
    put_segs(info + 0x108, (u32 *)(info + 0x148), m);
    memcpy(info + 0x14C, m->fingerprint, 20);
}

/* SceKernelModuleInfoEx, 0x1a8 bytes */
static void fill_info_ex(u8 *info, const struct module *m)
{
    memset(info + 8, 0, 0x1a8 - 8);
    put_name(info, m);
    *(s32 *)(info + 0x108) = (s32)m->fake_handle;
    *(u32 *)(info + 0x10C) = m->tls_id;
    if (m->tls_memsz) {
        *(u64 *)(info + 0x110) = m->base + m->tls_vaddr;
        *(u32 *)(info + 0x118) = (u32)m->tls_filesz;
        *(u32 *)(info + 0x11C) = (u32)m->tls_memsz;
        *(u32 *)(info + 0x120) = (u32)m->tls_offset;
        *(u32 *)(info + 0x124) = (u32)m->tls_align;
    }
    *(u64 *)(info + 0x128) = m->has_init ? m->base + m->init : 0;
    *(u64 *)(info + 0x130) = m->fini ? m->base + m->fini : 0;
    u64 frame, size;
    if (eh_frame(m, &frame, &size)) {
        *(u64 *)(info + 0x148) = m->base + m->eh_frame_hdr;
        *(u64 *)(info + 0x150) = frame;
        *(u32 *)(info + 0x158) = (u32)m->eh_frame_hdr_size;
        *(u32 *)(info + 0x15C) = (u32)size;
    }
    put_segs(info + 0x160, (u32 *)(info + 0x1A0), m);
}

/* SceKernelModuleInfoForUnwind, 0x130 bytes */
static void fill_unwind(u8 *info, const struct module *m, bool name)
{
    memset(info + 8, 0, 0x130 - 8);
    if (name)
        put_name(info, m);
    u64 frame, size;
    if (eh_frame(m, &frame, &size)) {
        *(u64 *)(info + 0x108) = m->base + m->eh_frame_hdr;
        *(u64 *)(info + 0x110) = frame;
        *(u64 *)(info + 0x118) = size;
    }
    *(u64 *)(info + 0x120) = m->base;
    *(u64 *)(info + 0x128) = m->text_size;
}

static s32 pass2(const char *real, u64 a, u64 b)
{
    s32 (*fn)(u64, u64) = host_real(real);
    return fn ? fn(a, b) : E_SRCH;
}

static s32 pass3(const char *real, u64 a, u64 b, u64 c)
{
    s32 (*fn)(u64, u64, u64) = host_real(real);
    return fn ? fn(a, b, c) : E_SRCH;
}

/* by handle: SceKernelModuleInfo (0x160) or, for Internal, the Ex form */
static s32 info_by_handle(const char *real, s32 handle, u8 *info, u64 want)
{
    struct module *m = mod_by_handle(handle);
    if (!m)
        return pass2(real, (u64)handle, (u64)info);
    if (!info)
        return E_FAULT;
    if (*(u64 *)info != want)
        return E_INVAL;
    (want == 0x160 ? fill_info : fill_info_ex)(info, m);
    return 0;
}

static s32 hle_get_module_info(s32 h, u8 *info) { return info_by_handle("sceKernelGetModuleInfo", h, info, 0x160); }
static s32 hle_get_module_info2(s32 h, u8 *info) { return info_by_handle("sceKernelGetModuleInfo2", h, info, 0x160); }
static s32 hle_get_module_info_internal(s32 h, u8 *info)
{
    return info_by_handle("sceKernelGetModuleInfoInternal", h, info, 0x1a8);
}

/* s32 (u64 addr, s32 flags, SceKernelModuleInfoEx *info): the title's libc
 * asks it (flags 2) in __cxa_thread_atexit, its thread-exit hook and
 * __cxa_finalize */
static s32 hle_get_module_info_from_addr(u64 addr, s32 flags, u8 *info)
{
    struct module *m = mod_by_addr(addr);
    if (!m)
        return pass3("sceKernelGetModuleInfoFromAddr", addr, (u64)flags, (u64)info);
    if (!info)
        return E_FAULT;
    if (flags >= 3) {
        memset(info, 0, 0x1a8);
        return E_INVAL;
    }
    fill_info_ex(info, m);
    *(u64 *)info = 0x1a8;
    return 0;
}

/* s32 (u64 addr, s32 flags, SceKernelModuleInfoForUnwind *info): the
 * unwinder's FDE lookup, kernel first, then libSceSysmodule's (which hides
 * the names of IsGame modules) */
static s32 unwind_info(const char *real, u64 addr, s32 flags, u8 *info, bool name)
{
    struct module *m = mod_by_addr(addr);
    if (!m)
        return pass3(real, addr, (u64)flags, (u64)info);
    if (!info)
        return E_FAULT;
    if (flags >= 3) {
        memset(info, 0, 0x130);
        return E_INVAL;
    }
    if (*(u64 *)info < 0x130)
        return E_INVAL;
    fill_unwind(info, m, name);
    return 0;
}

static s32 hle_get_module_info_for_unwind(u64 addr, s32 flags, u8 *info)
{
    return unwind_info("sceKernelGetModuleInfoForUnwind", addr, flags, info, true);
}

static s32 hle_sysmodule_info_for_unwind(u64 addr, s32 flags, u8 *info)
{
    struct module *m = mod_by_addr(addr);
    return unwind_info("sceSysmoduleGetModuleInfoForUnwind", addr, flags, info, m && !sysmod_is_game(m->name));
}

/* handles: the title's modules (executable first), then for List the
 * system modules SWRR loaded (*.sprx; SWRR's own eboot and prx are left out) */
static s32 module_list(s32 *handles, u64 max, u64 *count, bool system)
{
    if (!handles || !count)
        return E_FAULT;
    u64 n = 0;
    struct module *main = g_main_mod;
    if (main && n < max)
        handles[n++] = (s32)main->fake_handle;
    for (struct module *m = g_modules; m; m = m->next)
        if (m != main) {
            if (n == max)
                return E_NOMEM;
            handles[n++] = (s32)m->fake_handle;
        }
    if (system && K.module_list && K.module_info) {
        u32 host[256];
        size_t nhost = 0;
        if (K.module_list(host, ARRAY_SIZE(host), &nhost) == 0)
            for (size_t i = 0; i < nhost; i++) {
                struct sce_module_info mi;
                memset(&mi, 0, sizeof(mi));
                mi.size = sizeof(mi);
                size_t len;
                if (K.module_info(host[i], &mi) != 0 || (len = strlen(mi.name)) < 5 ||
                    strcmp(mi.name + len - 5, ".sprx") != 0)
                    continue;
                if (n == max)
                    return E_NOMEM;
                handles[n++] = (s32)host[i];
            }
    }
    *count = n;
    return 0;
}

static s32 hle_get_module_list(s32 *h, u64 max, u64 *count) { return module_list(h, max, count, true); }
static s32 hle_get_module_list2(s32 *h, u64 max, u64 *count) { return module_list(h, max, count, false); }

/* s32 __elf_phdr_match_addr(const SceKernelModuleInfoEx *info, u64 addr):
 * libc's __cxa_finalize: does an atexit handler belong to the module? */
static s32 hle_elf_phdr_match_addr(const u8 *info, u64 addr)
{
    struct module *m = info ? mod_by_handle(*(const s32 *)(info + 0x108)) : NULL;
    if (!m)
        return pass2("__elf_phdr_match_addr", (u64)info, addr);
    for (u32 i = 0; i < m->nsegs; i++)
        if (addr >= m->base + m->segs[i].vaddr && addr < m->base + m->segs[i].vaddr + m->segs[i].size)
            return 1;
    return 0;
}

/* libc's __cxa_thread_atexit registers a thread_local destructor only when
 * this returns 0 for the object's module (its __dso_handle) */
static s32 hle_rtld_thread_atexit_inc(u64 dso)
{
    static u32 logged;
    struct module *m = mod_by_addr(dso);
    if (!m)
        return pass2("_sceKernelRtldThreadAtexitIncrement", dso, 0);
    if (logged++ < 8)
        logf("thread_local destructor registered for %s", m->name);
    return 0;
}

static s32 hle_rtld_thread_atexit_dec(u64 dso)
{
    return mod_by_addr(dso) ? 0 : pass2("_sceKernelRtldThreadAtexitDecrement", dso, 0);
}

/* libc asks it only with heap tracing on: nothing to report */
static s32 hle_internal_segment_info(u64 *out)
{
    if (out)
        out[0] = out[1] = 0;
    return 0;
}

#define LK "libkernel"
static const struct ext_hle_ent k_hle[] = {
    {LK, "sceKernelLoadStartModule", hle_load_start_module, NULL},
    {LK, "sceKernelStopUnloadModule", hle_stop_unload_module, NULL},
    {LK, "sceKernelDlsym", hle_dlsym, NULL},
    {LK, "sceKernelGetModuleInfo", hle_get_module_info, NULL},
    {LK, "sceKernelGetModuleInfo2", hle_get_module_info2, NULL},
    {LK, "sceKernelGetModuleInfoInternal", hle_get_module_info_internal, NULL},
    {LK, "sceKernelGetModuleInfoFromAddr", hle_get_module_info_from_addr, NULL},
    {LK, "sceKernelGetModuleInfoForUnwind", hle_get_module_info_for_unwind, NULL},
    {LK, "sceKernelGetModuleList", hle_get_module_list, NULL},
    {LK, "sceKernelGetModuleList2", hle_get_module_list2, NULL},
    {LK, "__elf_phdr_match_addr", hle_elf_phdr_match_addr, NULL},
    {LK, "_sceKernelRtldThreadAtexitIncrement", hle_rtld_thread_atexit_inc, NULL},
    {LK, "_sceKernelRtldThreadAtexitDecrement", hle_rtld_thread_atexit_dec, NULL},
    {LK, "sceKernelInternalMemoryGetModuleSegmentInfo", hle_internal_segment_info, NULL},
    {"libSceSysmodule", "sceSysmoduleGetModuleInfoForUnwind", hle_sysmodule_info_for_unwind, NULL},
};

EXT_FAMILY("module", k_hle, NULL);
