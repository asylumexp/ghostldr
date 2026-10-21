/* Import resolution:  core HLE override (by name) -> extension HLE (by
 * library and NID) -> our loaded modules -> host system modules (loaded on
 * demand, resolved by name through the extension's NID table, unless the
 * extension's policy for the library forbids it) -> logging stub. */
#include "ghost_api.h"

const char *module_import_lib(struct module *m, const char *encoded, char *nid, const char **modname);

/* ---- host modules ---- */
struct host_mod {
    const char *name;
    u32 handle;
    int status;
};
static struct host_mod g_host[96];
static u32 g_nhost;

int host_module_handle(const char *modname, u32 *out)
{
    if (!modname)
        return -1;
    if (strcmp(modname, "libkernel") == 0) {
        *out = (u32)g_ctx->libkernel_handle;
        return 0;
    }
    for (u32 i = 0; i < g_nhost; i++)
        if (strcmp(g_host[i].name, modname) == 0) {
            *out = g_host[i].handle;
            return g_host[i].status;
        }
    char file[96];
    fmt(file, sizeof(file), "%s.sprx", modname);
    u32 handle = 0;
    int status = host_find_module(file, &handle);
    if (status != 0) {
        int res = 0;
        s32 h = (s32)call_gadget6((u64)K.load_start_module, (u64)file, 0, 0, 0, 0, (u64)&res);
        if (h > 0) {
            handle = (u32)h;
            status = 0;
            logf("host: loaded %s handle %x", file, handle);
        } else {
            logf("host: load %s failed %x", file, (u32)h);
            status = -1;
        }
    }
    if (g_nhost < ARRAY_SIZE(g_host))
        g_host[g_nhost++] = (struct host_mod){arena_strdup(modname), handle, status};
    *out = handle;
    return status;
}

/* Module names and file names differ for a few libraries (module
 * libSceAppContentUtil lives in libSceAppContent.sprx): fall back to the
 * importing module's DT_NEEDED entries that share a prefix. */
int host_module_handle_for(struct module *m, const char *modname, u32 *out)
{
    if (host_module_handle(modname, out) == 0)
        return 0;
    size_t ml = strlen(modname);
    for (u32 i = 0; i < m->n_needed_files; i++) {
        char base[96];
        strlcpy(base, m->needed_files[i], sizeof(base));
        size_t bl = strlen(base);
        if (bl > 4 && strcmp(base + bl - 4, ".prx") == 0)
            base[bl -= 4] = 0;
        size_t n = bl < ml ? bl : ml;
        if (n >= 8 && strncmp(base, modname, n) == 0 && strcmp(base, modname) != 0) {
            if (host_module_handle(base, out) == 0) {
                logf("host: module %s found as %s", modname, base);
                return 0;
            }
        }
    }
    return -1;
}

/* ---- unresolved-import stubs ---- */
#define STUB_SIZE 32
#define MAX_STUBS 512 /* 16 KiB of code; the small RX area left after the core is 16-40 KiB */
struct stub_info {
    const char *lib, *nid, *name;
    s64 ret;     /* what the stub returns (the library's policy; 0 by default) */
    bool denied; /* no host fallback for its library */
};
static u64 g_stub_base;
static u8 *g_stub_buf;
static struct stub_info *g_stubs; /* allocated with the stub area (keeps the core image small) */
static u64 *g_count_target;
static u32 g_nstubs, g_stubs_flushed;

u64 c_unresolved(u32 idx)
{
    if (idx >= g_nstubs) {
        logf("UNRESOLVED call #%u", idx);
        return 0;
    }
    const struct stub_info *s = &g_stubs[idx];
    if (s->denied || s->ret)
        logf("%s call: %s %s (%s) -> %x", s->denied ? "DENIED" : "UNRESOLVED", s->lib, s->nid,
             s->name ? s->name : "?", (u32)s->ret);
    else
        logf("UNRESOLVED call: %s %s (%s)", s->lib, s->nid, s->name ? s->name : "?");
    return (u64)s->ret;
}

/* eax = stub index; preserve argument registers, return c_unresolved's value */
__attribute__((naked, used)) static void unresolved_entry(void)
{
    __asm__ volatile(
        ".intel_syntax noprefix\n"
        "push rbp\n"
        "mov rbp, rsp\n"
        "and rsp, -16\n"
        "push rdi\n push rsi\n push rdx\n push rcx\n push r8\n push r9\n"
        "mov edi, eax\n"
        "call c_unresolved\n"
        "pop r9\n pop r8\n pop rcx\n pop rdx\n pop rsi\n pop rdi\n"
        "leave\n"
        "ret\n"
        ".att_syntax prefix\n");
}

static u32 g_stub_cap;

/* The small RX area's size depends on the exploit's JIT layout (16-48 KiB):
 * take the largest power-of-two share of it that fits. */
static bool stubs_ready(void)
{
    if (!g_stub_buf) {
        for (u32 cap = MAX_STUBS; cap >= 64 && !g_stub_base; cap /= 2)
            if ((g_stub_base = arena_small_alloc(cap * STUB_SIZE)))
                g_stub_cap = cap;
        if (!g_stub_base)
            return false;
        g_stub_buf = page_alloc(g_stub_cap * STUB_SIZE);
        g_stubs = page_alloc(g_stub_cap * sizeof(*g_stubs));
        g_count_target = page_alloc(g_stub_cap * sizeof(*g_count_target));
        logf("stubs: room for %u stubs at %lx", g_stub_cap, g_stub_base);
    }
    return g_stub_buf && g_stubs && g_count_target && g_nstubs < g_stub_cap;
}

/* A 32-byte slot of executable code in the stub area (written at stubs_flush) */
u64 stub_alloc_code(const u8 *code, u32 len)
{
    if (len > STUB_SIZE || !stubs_ready())
        return 0;
    u32 idx = g_nstubs++;
    g_stubs[idx] = (struct stub_info){"hook", "", "trampoline", 0, false};
    memcpy(g_stub_buf + idx * STUB_SIZE, code, len);
    return g_stub_base + idx * STUB_SIZE;
}

u64 unresolved_stub(const char *lib, const char *nid, s64 ret, bool denied)
{
    if (!stubs_ready())
        return 0;
    u32 idx = g_nstubs++;
    const char *name = nid_to_name(nid);
    g_stubs[idx] = (struct stub_info){arena_strdup(lib), arena_strdup(nid), name ? arena_strdup(name) : NULL, ret,
                                      denied};
    u8 *s = g_stub_buf + idx * STUB_SIZE;
    s[0] = 0xB8;                        /* mov eax, idx */
    *(u32 *)(s + 1) = idx;
    s[5] = 0xFF, s[6] = 0x25;           /* jmp [rip+0] */
    *(u32 *)(s + 7) = 0;
    *(u64 *)(s + 11) = (u64)unresolved_entry;
    return g_stub_base + idx * STUB_SIZE;
}

void stubs_flush(void)
{
    if (g_stub_buf && g_nstubs > g_stubs_flushed) {
        arena_write(g_stub_base, g_stub_buf, g_nstubs * STUB_SIZE);
        g_stubs_flushed = g_nstubs;
        logf("stubs: %u unresolved imports routed to logging stubs", g_nstubs);
    }
}

/* ---- import call counting (ctx flag 8, diagnostics) ----
 * Each of the executable's resolved imports goes through a stub that bumps a
 * counter and jumps on; the watchdog prints the busiest imports. */
u64 g_import_counts[MAX_STUBS]; /* in the core image: stubs reach it rip-relative */

static u64 counting_stub(u64 target, const char *lib, const char *nid, const char *name)
{
    for (u32 i = 0; i < g_nstubs; i++)
        if (g_count_target[i] == target)
            return g_stub_base + i * STUB_SIZE;
    if (!stubs_ready() || g_nstubs + 24 >= g_stub_cap) /* keep room for hook trampolines */
        return target;
    u32 idx = g_nstubs++;
    g_stubs[idx] = (struct stub_info){arena_strdup(lib), arena_strdup(nid), name ? arena_strdup(name) : NULL, 0,
                                      false};
    g_count_target[idx] = target;
    u64 at = g_stub_base + idx * STUB_SIZE;
    u8 *c = g_stub_buf + idx * STUB_SIZE;
    c[0] = 0xF0, c[1] = 0x48, c[2] = 0xFF, c[3] = 0x05; /* lock inc qword [rip+disp] */
    *(s32 *)(c + 4) = (s32)((s64)(u64)&g_import_counts[idx] - (s64)(at + 8));
    c[8] = 0xFF, c[9] = 0x25; /* jmp [rip+0] */
    *(u32 *)(c + 10) = 0;
    *(u64 *)(c + 14) = target;
    return at;
}

u32 import_counts_top(u32 *idx_out, u64 *delta_out, u32 max, u64 *prev)
{
    u32 n = 0;
    for (u32 i = 0; i < g_nstubs; i++) {
        if (!g_count_target[i])
            continue;
        u64 d = g_import_counts[i] - prev[i];
        prev[i] = g_import_counts[i];
        if (!d)
            continue;
        u32 k = n < max ? n++ : max;
        if (k == max) {
            if (d <= delta_out[max - 1])
                continue;
            k = max - 1;
        }
        while (k > 0 && delta_out[k - 1] < d) {
            idx_out[k] = idx_out[k - 1], delta_out[k] = delta_out[k - 1];
            k--;
        }
        idx_out[k] = i, delta_out[k] = d;
    }
    return n;
}

const char *import_name(u32 idx)
{
    return idx < g_nstubs ? (g_stubs[idx].name ? g_stubs[idx].name : g_stubs[idx].nid) : "?";
}

/* ---- resolution ---- */
static u32 g_logged_misses, g_logged_denied;

static u64 maybe_count(struct module *m, u64 target, const char *lib, const char *nid, const char *name, bool fn)
{
    if (!(g_ctx->flags & 8) || !m->is_main || !target || !fn)
        return target;
    return counting_stub(target, lib ? lib : "?", nid, name);
}

u64 resolve_import(struct module *m, const char *encoded, bool weak, bool plt_func)
{
    char nid[12];
    const char *modname = NULL;
    const char *lib = module_import_lib(m, encoded, nid, &modname);
    const char *name = nid_to_name(nid);

    if (name) {
        u64 h = hle_lookup(lib, name);
        if (h)
            return maybe_count(m, h, lib, nid, name, plt_func);
    }
    u64 h = ext_hle_lookup(lib, nid, name);
    if (h)
        return maybe_count(m, h, lib, nid, name ? name : nid, plt_func);
    for (struct module *o = g_modules; o; o = o->next) {
        for (u32 i = 0; i < o->n_export_libs; i++) {
            if (lib && strcmp(o->export_libs[i].name, lib) == 0) {
                u64 a = module_export(o, nid);
                if (a)
                    return maybe_count(m, a, lib, nid, name ? name : nid, plt_func);
            }
        }
    }
    const struct ext_lib *pol = ext_lib_policy(lib);
    s64 ret = pol ? (s64)pol->stub_ret : 0;
    if (pol && (pol->flags & EXT_LIB_NO_HOST)) {
        if (g_logged_denied++ < 200)
            logf("denied: %s needs %s:%s (%s): %s has no host fallback, the stub returns %x", m->name, lib, nid,
                 name ? name : "?", pol->name, (u32)ret);
        return unresolved_stub(lib, nid, ret, true);
    }
    if (name) {
        u32 handle;
        if (host_module_handle_for(m, modname, &handle) == 0) {
            void *a = host_dlsym(handle, name);
            if (a)
                return maybe_count(m, (u64)a, lib, nid, name, plt_func);
        }
    }
    if (g_logged_misses++ < 200)
        logf("unresolved%s: %s needs %s:%s (%s) from module %s", weak ? " (weak)" : "", m->name,
             lib ? lib : "?", nid, name ? name : "?", modname ? modname : "?");
    return unresolved_stub(lib ? lib : "?", nid, ret, false);
}
