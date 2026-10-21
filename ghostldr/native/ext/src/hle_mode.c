/* Per-library modes and call routing for the media and peripheral families
 * (see hle_mode.h).  No family of its own. */
#include "hle_mode.h"

/* kernel errno codes (0x80020000 + errno) that refuse a caller rather than
 * describe a device: EPERM, EACCES, EOPNOTSUPP, ENOSYS, ENOENT, ENOEXEC */
static const s32 k_deny_common[] = {(s32)0x80020001, (s32)0x8002000D, (s32)0x8002002D,
                                    (s32)0x8002004E, (s32)0x80020002, (s32)0x80020008, 0};
static const char *const k_mode[] = {"UNDECIDED", "PASS", "MASKED", "VIRT"};
#define HOST_MISSING ((void *)1) /* cache value: the host lacks the function */

static struct hle_router *g_routers[4];
static u32 g_nrouters;

bool hle_in(const s32 *set, s32 v)
{
    for (; set && *set; set++)
        if (*set == v)
            return true;
    return false;
}

void hle_set(struct hle_lib *L, u8 mode, const char *fn, s32 rc, const char *why)
{
    for (;;) {
        u8 cur = L->mode;
        if (cur == mode || cur == HM_VIRT || (cur != HM_UNDECIDED && mode != HM_VIRT))
            return;
        if (__sync_bool_compare_and_swap(&L->mode, cur, mode))
            break;
    }
    if (rc)
        logf("%s: %s -> %s (%s -> %x: %s)", L->tag, L->pol->name, k_mode[mode], fn, (u32)rc, why);
    else
        logf("%s: %s -> %s (%s: %s)", L->tag, L->pol->name, k_mode[mode], fn, why);
}

void *hle_host(struct hle_lib *L, const char *name, void **cache)
{
    void *fn = *cache;
    if (fn)
        return fn == HOST_MISSING ? NULL : fn;
    fn = host_real_in(L->module ? L->module : L->pol->name, name);
    *cache = fn ? fn : HOST_MISSING;
    if (!fn) {
        /* the import would have had the logging stub: an undecided library
         * goes virtual; a decided one answers this function virtually */
        if (L->mode == HM_UNDECIDED)
            hle_set(L, HM_VIRT, name, 0, "not on the host");
        else
            logf("%s: %s:%s is not on the host; it answers virtually", L->tag, L->pol->name, name);
    }
    return fn;
}

bool hle_root(struct hle_lib *L, const char *name, void **cache, const u64 a[6], s32 *rc)
{
    if (L->mode == HM_VIRT)
        return false;
    void *fn = hle_host(L, name, cache);
    if (!fn)
        return false;
    s32 r = (s32)HLE_CALL6(fn, a);
    u8 was = L->mode;
    if (r >= 0 || hle_in(L->legit, r)) {
        hle_set(L, HM_PASS, name, r, r >= 0 ? "the host runs it" : "the device's state goes to the title");
        *rc = r;
        return true;
    }
    if (hle_in(L->deny, r) || hle_in(k_deny_common, r)) {
        hle_set(L, HM_VIRT, name, r, "refused");
        return false;
    }
    if (was == HM_UNDECIDED && (hle_in(L->already, r) || (L->flags & HL_MASK_ANY))) {
        hle_set(L, HM_MASKED, name, r, "SWRR owns it");
        *rc = 0;
        return true;
    }
    if (was == HM_MASKED) {
        *rc = 0; /* SWRR's instance: a repeated init is fine */
        return true;
    }
    hle_set(L, HM_PASS, name, r, "an error for the title");
    *rc = r;
    return true;
}

s32 hle_setter(struct hle_lib *L, const char *name, void **cache, const u64 a[6], s32 virt_ret)
{
    void *fn = L->mode == HM_VIRT ? NULL : hle_host(L, name, cache);
    if (!fn)
        return virt_ret;
    s32 r = (s32)HLE_CALL6(fn, a);
    if (r < 0 && hle_in(L->deny, r)) {
        logf("%s: %s -> %x, answered 0", L->tag, name, (u32)r);
        return 0;
    }
    return r;
}

const char *hle_sys_path(const char *path, char *buf, size_t size)
{
    if (!path)
        return path;
    bool app0 = strncmp(path, "/app0", 5) == 0 && (path[5] == '/' || path[5] == 0);
    if (app0 && gfs_active())
        return NULL;
    return remap_path(path, buf, size);
}

u64 hle_time_us(void)
{
    static u64 (*get)(void);
    if (!get)
        get = (u64(*)(void))host_real("sceKernelGetProcessTime");
    return get ? get() : 0;
}

static bool routed_virtual(const struct hle_lib *L, const struct hle_route *rt)
{
    u16 f = rt->flags;
    if ((f & RF_VIRT) || ((f & RF_CRASH) && !L->probed) || ((f & RF_KEEPRP) && (g_ctx->flags & CTX_FLAG_KEEP_REMOTE_PLAY)))
        return true;
    if ((f & RF_NOGFS) && gfs_active())
        return true;
    if (f & RF_PURE)
        return false;
    return L->mode == HM_VIRT || ((f & RF_MASKSTUB) && L->mode == HM_MASKED) || ((f & RF_OWNED) && !L->owned) ||
           ((f & RF_READY) && !L->ready);
}

/* hle_gen_entry's question: where does the call through trampoline `at` go?
 * The host function or a virtual body (it is jumped to with the title's
 * registers), or 0 with the answer in *answer. */
u64 hle_route(u64 at, u64 *regs, s64 *answer) HIDDEN;
u64 hle_route(u64 at, u64 *regs, s64 *answer)
{
    struct hle_router *r = NULL;
    for (u32 i = 0; i < g_nrouters && !r; i++)
        if (at >= (u64)g_routers[i]->tramps && at < (u64)g_routers[i]->tramps_end)
            r = g_routers[i];
    if (!r) {
        logf("hle: call through an unknown trampoline %lx", at);
        *answer = EUNAVAIL;
        return 0;
    }
    u32 idx = (u32)((at - (u64)r->tramps) >> 4);
    const struct hle_route *rt = &r->routes[idx];
    struct hle_lib *L = &r->libs[rt->lib];
    const char *name = r->hle[r->first + idx].name;
    if (!routed_virtual(L, rt)) {
        void *fn = hle_host(L, name, &r->cache[idx]);
        if (fn)
            return (u64)fn;
    }
    if (rt->flags & RF_LOG)
        logf("%s: %s answered virtually (%x)", r->tag, name, (u32)rt->ret);
    if (rt->virt)
        return (u64)rt->virt;
    *answer = rt->ret;
    return 0;
}

/* Saves the argument registers (rax too: the vector count of a variadic
 * call) and xmm0-7, asks hle_route, restores them and tail-jumps to its
 * choice, or returns its answer.  r11 = the trampoline's address. */
__asm__(".text\n"
        "  .p2align 4\n"
        "  .globl hle_gen_entry\n"
        "  .hidden hle_gen_entry\n"
        "hle_gen_entry:\n"
        "  push %rbp\n"
        "  mov %rsp, %rbp\n"
        "  and $-16, %rsp\n"
        "  sub $0xc0, %rsp\n"
        "  mov %rdi, 0x00(%rsp)\n"
        "  mov %rsi, 0x08(%rsp)\n"
        "  mov %rdx, 0x10(%rsp)\n"
        "  mov %rcx, 0x18(%rsp)\n"
        "  mov %r8, 0x20(%rsp)\n"
        "  mov %r9, 0x28(%rsp)\n"
        "  mov %rax, 0x30(%rsp)\n"
        "  movdqu %xmm0, 0x40(%rsp)\n"
        "  movdqu %xmm1, 0x50(%rsp)\n"
        "  movdqu %xmm2, 0x60(%rsp)\n"
        "  movdqu %xmm3, 0x70(%rsp)\n"
        "  movdqu %xmm4, 0x80(%rsp)\n"
        "  movdqu %xmm5, 0x90(%rsp)\n"
        "  movdqu %xmm6, 0xa0(%rsp)\n"
        "  movdqu %xmm7, 0xb0(%rsp)\n"
        "  mov %r11, %rdi\n"
        "  mov %rsp, %rsi\n"
        "  lea 0x38(%rsp), %rdx\n"
        "  call hle_route\n"
        "  mov %rax, %r11\n"
        "  movdqu 0x40(%rsp), %xmm0\n"
        "  movdqu 0x50(%rsp), %xmm1\n"
        "  movdqu 0x60(%rsp), %xmm2\n"
        "  movdqu 0x70(%rsp), %xmm3\n"
        "  movdqu 0x80(%rsp), %xmm4\n"
        "  movdqu 0x90(%rsp), %xmm5\n"
        "  movdqu 0xa0(%rsp), %xmm6\n"
        "  movdqu 0xb0(%rsp), %xmm7\n"
        "  mov 0x00(%rsp), %rdi\n"
        "  mov 0x08(%rsp), %rsi\n"
        "  mov 0x10(%rsp), %rdx\n"
        "  mov 0x18(%rsp), %rcx\n"
        "  mov 0x20(%rsp), %r8\n"
        "  mov 0x28(%rsp), %r9\n"
        "  test %r11, %r11\n"
        "  jz 1f\n"
        "  mov 0x30(%rsp), %rax\n"
        "  leave\n"
        "  jmp *%r11\n"
        "1:\n"
        "  mov 0x38(%rsp), %rax\n"
        "  leave\n"
        "  ret\n");

/* ---- the title's imports: one the host lacks would have had the logging
 * stub, so its library goes virtual before the title runs ---- */

static const char k_id_digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

static int decode_id(const char *s, const char *end)
{
    int v = 0;
    if (s >= end)
        return -1;
    for (; s < end; s++) {
        const char *d = strrchr_c(k_id_digits, *s);
        if (!*s || !d)
            return -1;
        v = v * 64 + (int)(d - k_id_digits);
    }
    return v;
}

static const char *ref_name(const struct libref *refs, u32 n, int id)
{
    for (u32 i = 0; i < n; i++)
        if (refs[i].id == id)
            return refs[i].name;
    return NULL;
}

static bool in_list(const char *list, const char *lib)
{
    size_t n = strlen(lib);
    for (const char *p = list; p && *p;) {
        while (*p == ' ')
            p++;
        const char *e = p;
        while (*e && *e != ' ')
            e++;
        if ((size_t)(e - p) == n && n && strncmp(p, lib, n) == 0)
            return true;
        p = e;
    }
    return false;
}

static struct hle_lib *lib_named(struct hle_router *r, const char *lib)
{
    for (u32 i = 0; lib && i < r->nlibs; i++)
        if (strcmp(r->libs[i].pol->name, lib) == 0 || in_list(r->libs[i].pol->aliases, lib))
            return &r->libs[i];
    return NULL;
}

/* does the title's call of `name` ever reach the host? */
static bool asks_host(struct hle_router *r, struct hle_lib *L, const char *name)
{
    for (u32 i = 0; i < r->nhle; i++) {
        if (!r->hle[i].name || strcmp(r->hle[i].name, name) != 0)
            continue;
        if (i < r->first)
            return true; /* a root or setter wrapper */
        const struct hle_route *rt = &r->routes[i - r->first];
        return !(rt->flags & RF_VIRT) && !((rt->flags & RF_CRASH) && !L->probed);
    }
    return true; /* not the extension's: it would go to the host by name */
}

static void scan_imports(struct hle_router *r)
{
    for (struct module *m = g_modules; m; m = m->next) {
        for (u64 i = 1; m->symtab && i < m->nsyms; i++) {
            const struct elf_sym *s = &m->symtab[i];
            if (s->shndx || s->name >= m->strsz)
                continue;
            const char *enc = m->strtab + s->name, *h1 = enc;
            while (*h1 && *h1 != '#')
                h1++;
            if (!*h1 || h1 - enc != 11)
                continue;
            const char *h2 = h1 + 1;
            while (*h2 && *h2 != '#')
                h2++;
            struct hle_lib *L = lib_named(r, ref_name(m->import_libs, m->n_import_libs, decode_id(h1 + 1, h2)));
            if (!L || L->mode != HM_UNDECIDED)
                continue;
            char nid[12];
            memcpy(nid, enc, 11);
            nid[11] = 0;
            const char *name = nid_to_name(nid);
            if (name && !asks_host(r, L, name))
                continue;
            const char *mod = *h2 ? ref_name(m->needed_mods, m->n_needed_mods, decode_id(h2 + 1, h2 + strlen(h2)))
                                  : NULL;
            u32 handle;
            const char *why = NULL;
            if (!name)
                why = "an import with no known name";
            else if (host_module_handle(mod ? mod : L->module ? L->module : L->pol->name, &handle) != 0)
                why = "its module does not load";
            else if (!host_dlsym(handle, name))
                why = "not on the host";
            if (why)
                hle_set(L, HM_VIRT, name ? name : nid, 0, why);
        }
    }
}

int hle_router_init(struct hle_router *r)
{
    u32 nroutes = r->nhle - r->first;
    if ((u64)(r->tramps_end - r->tramps) != 16ULL * nroutes || g_nrouters >= ARRAY_SIZE(g_routers)) {
        logf("%s: %u routes but %lu bytes of trampolines", r->tag, nroutes, (u64)(r->tramps_end - r->tramps));
        return -1;
    }
    r->cache = arena_alloc(8 * (nroutes + 1));
    if (!r->cache)
        return -1;
    memset(r->cache, 0, 8 * (nroutes + 1));
    g_routers[g_nrouters++] = r;
    u32 nvirt = 0;
    for (u32 i = 0; i < r->nlibs; i++) {
        struct hle_lib *L = &r->libs[i];
        L->tag = r->tag;
        L->mode = HM_UNDECIDED;
        if (L->init_mode == HM_VIRT) {
            hle_set(L, HM_VIRT, "start", 0, L->why ? L->why : "never passes");
            nvirt++;
        }
    }
    scan_imports(r);
    logf("%s: %u libraries, %u functions (%u routed); %u virtual from the start, the rest decided at first use",
         r->tag, r->nlibs, r->nhle, nroutes, nvirt);
    return 0;
}

void hle_router_sysmodule(struct hle_router *r, u32 id, s32 rc)
{
    for (u32 i = 0; i < r->nlibs; i++) {
        struct hle_lib *L = &r->libs[i];
        if (rc < 0 && id && (L->sysmod[0] == id || L->sysmod[1] == id) && L->mode == HM_UNDECIDED)
            hle_set(L, HM_VIRT, "sceSysmoduleLoadModule", rc, "its sysmodule did not load");
    }
}
