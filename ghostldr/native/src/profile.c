/*
 * Title profiles: what ghostldr does differently for one title, as data.
 *
 * host/profiles.py compiles ghostldr/data/titles/<TITLE>.json (merged over
 * default.json) into the little-endian blobs below; run.py sends one per
 * known title plus the generic default (title "") as GHOSTLDR_PROFILE_HEX,
 * and ghostldr.lua leaves a pointer to them, ended by 8 zero bytes, in the
 * last u64 of the boot context.  Without it every title gets k_default.
 *
 * The title is picked by ghostfs (run.py --title, else the only installed
 * one) or, without ghostfs, by the eboot's fingerprint.  A profile that
 * names an eboot fingerprint applies only to that build: on any other build
 * of the title the generic default is used instead.  Code-touching parts
 * (a memory plan with its tables and lazy sites, the diagnostic hooks and
 * watchdog reads) need that match.
 *
 * HLE rules {lib, nid, return value, out-param fills} answer single imports
 * without C: profile_resolve() is consulted first by resolve_import().
 */
#include "ghost.h"

#define PROFILE_SLOT 63 /* the last u64 of the 0x200-byte context ghostldr.lua allocates */
_Static_assert(sizeof(struct boot_ctx) <= PROFILE_SLOT * 8, "boot_ctx grew into the profile slot");

#define MAX_RULES 32
#define COLD GHOST_COLD /* boot-time code: small beats fast */

u64 stub_alloc_code(const u8 *code, u32 len);
u64 budget_resolve(const char *nid);

static const struct prof_hdr k_default = {
    .magic = {'G', 'L', 'P', 'F'}, .version = 1, .header_size = sizeof(struct prof_hdr), .total = sizeof(struct prof_hdr),
    .save_blocks = 1024, .app_param = {3}, .runtime_reserve_mib = 64, .video_reserve_mib = -1, .min_mib = 1024,
};
static const struct prof_hdr *g_prof = &k_default;
static char g_title[16];
static bool g_gated, g_bound;

COLD static const struct prof_hdr *blob(const u8 *p)
{
    const struct prof_hdr *h = (const struct prof_hdr *)p;
    if (!p || memcmp(h->magic, "GLPF", 4) != 0 || h->version != 1 || h->header_size < sizeof(*h) ||
        h->total < h->header_size || h->total > 0x10000 || (h->total & 7))
        return NULL;
    return h;
}

static const struct prof_hdr *first(void) { return blob((const u8 *)((const u64 *)g_ctx)[PROFILE_SLOT]); }
static const struct prof_hdr *next(const struct prof_hdr *h) { return blob((const u8 *)h + h->total); }

COLD static const struct prof_hdr *find(const char *title)
{
    for (const struct prof_hdr *h = first(); h; h = next(h))
        if (strncmp(h->title, title, sizeof(h->title)) == 0)
            return h;
    return NULL;
}

COLD const char *profile_requested(void)
{
    for (const struct prof_hdr *h = first(); h; h = next(h))
        if (h->flags & PF_REQUESTED)
            return h->title;
    return NULL;
}

COLD void profile_select(const char *title)
{
    const struct prof_hdr *h = find(title);
    g_prof = h ? h : find("");
    if (!g_prof)
        g_prof = &k_default;
    strlcpy(g_title, title, sizeof(g_title));
}

const char *profile_title(void) { return g_title; }

COLD static void hex20(char *out, const u8 *fp)
{
    out[0] = '-', out[1] = 0;
    for (u32 i = 0; fp && i < 20; i++)
        fmt(out + 2 * i, 3, "%02x", fp[i]);
}

/* The executable is parsed: pick the profile by fingerprint when nothing
 * picked one yet, and check the profile's fingerprint against the build. */
COLD void profile_bind(const u8 *fp)
{
    static const u8 zero[20];
    if (!g_title[0]) {
        const char *want = profile_requested();
        for (const struct prof_hdr *h = first(); h && !want && fp; h = next(h))
            if (h->title[0] && memcmp(h->fingerprint, zero, 20) && memcmp(h->fingerprint, fp, 20) == 0)
                want = h->title;
        if (want)
            profile_select(want);
    }
    char have[44], need[44];
    hex20(have, fp);
    if (memcmp(g_prof->fingerprint, zero, 20) != 0) {
        g_gated = fp && memcmp(g_prof->fingerprint, fp, 20) == 0;
        if (!g_gated) {
            hex20(need, g_prof->fingerprint);
            logf("profile: %s: eboot fingerprint %s is not the profile's %s (another build): generic defaults",
                 g_title, have, need);
            const struct prof_hdr *d = find("");
            g_prof = d ? d : &k_default;
        }
    }
    g_bound = true;
    u32 f = profile_has(~0u);
    logf("profile: %s %s, eboot %s: memory %s%s%s%s, save %u blocks, app params %d %d", g_title[0] ? g_title : "?",
         (g_prof->flags & PF_OWN) ? "own profile" : "generic", have, (f & PF_PLAN) ? "plan" : "plan-free",
         (f & PF_HOOKS) ? ", hooks" : "", (f & PF_DIAG) ? ", diagnostics" : "",
         (f & PF_TRACE_OPENS) ? ", traced opens" : "", g_prof->save_blocks, g_prof->app_param[0],
         g_prof->app_param[1]);
}

/* the flags in `mask` that apply: code-touching ones only on the profile's build */
u32 profile_has(u32 mask)
{
    u32 f = g_prof->flags & mask;
    if (!g_gated || !g_bound)
        f &= ~(u32)(PF_PLAN | PF_HOOKS | PF_DIAG);
    return f;
}

const struct prof_hdr *profile_hdr(void) { return g_prof; }

s32 profile_app_param(u32 id) { return id < 5 ? g_prof->app_param[id] : 0; }

/* the n-th record of `kind`; plan records only on the profile's build */
COLD const void *profile_record(u32 kind, u32 n)
{
    if (kind == REC_TLS || kind == REC_EXEC ? !(g_gated && g_bound) : kind != REC_RULE && !profile_has(PF_PLAN))
        return NULL; /* TLS site lists and code ranges: on the profile's build only, plan or not */
    const u8 *p = (const u8 *)g_prof + g_prof->header_size, *end = (const u8 *)g_prof + g_prof->total;
    while (p + sizeof(struct prof_rec) <= end) {
        const struct prof_rec *r = (const struct prof_rec *)p;
        if (r->size < sizeof(*r) || p + r->size > end)
            break;
        if (r->kind == kind && n-- == 0)
            return r;
        p += r->size;
    }
    return NULL;
}

/* ---- HLE rules ---- */
static const struct prof_rule *g_rules[MAX_RULES];
static u64 g_rule_stub[MAX_RULES];
static u16 g_rule_calls[MAX_RULES];

static const char *rule_lib(const struct prof_rule *r) { return (const char *)&r->fill[r->nfill]; }

COLD u64 c_rule(u32 idx, const u64 *args)
{
    const struct prof_rule *r = g_rules[idx];
    for (u32 i = 0; i < r->nfill; i++) {
        const struct prof_fill *f = &r->fill[i];
        u8 *p = (u8 *)args[f->arg % 6];
        if ((u64)p < 0x10000)
            continue;
        p += f->offset;
        if (f->size == 0)
            memset(p, 0, f->value);
        else if (f->size == 1)
            *p = (u8)f->value;
        else if (f->size == 2)
            *(u16 *)p = (u16)f->value;
        else if (f->size == 4)
            *(u32 *)p = (u32)f->value;
        else
            *(u64 *)p = f->value;
    }
    if (g_rule_calls[idx] < r->log) {
        g_rule_calls[idx]++;
        const char *lib = rule_lib(r);
        logf("profile rule: %s %s -> %lx", lib, lib + strlen(lib) + 1, r->value);
    }
    return r->value;
}

/* eax = rule index; the argument registers become args[6] */
#if defined(__x86_64__)
__attribute__((naked, used)) static void rule_entry(void)
{
    __asm__ volatile(".intel_syntax noprefix\n"
                     "push rbp\n"
                     "mov rbp, rsp\n"
                     "and rsp, -16\n"
                     "push r9\n push r8\n push rcx\n push rdx\n push rsi\n push rdi\n"
                     "mov edi, eax\n"
                     "mov rsi, rsp\n"
                     "call c_rule\n"
                     "leave\n"
                     "ret\n"
                     ".att_syntax prefix\n");
}
#else
static void rule_entry(void) {} /* host builds of the tests (tests/native_host) call c_rule directly */
#endif

COLD static u64 rule_stub(u32 idx)
{
    if (!g_rule_stub[idx]) {
        u8 c[19] = {0xB8, 0, 0, 0, 0, 0xFF, 0x25}; /* mov eax, idx; jmp [rip+0] */
        *(u32 *)(c + 1) = idx;
        *(u64 *)(c + 11) = (u64)rule_entry;
        g_rule_stub[idx] = stub_alloc_code(c, sizeof(c));
    }
    return g_rule_stub[idx];
}

/* resolve_import's first stop: a profile rule for this import, else a
 * plan-free memory call (budget.c) */
COLD u64 profile_resolve(const char *lib, const char *nid)
{
    const struct prof_rule *r;
    for (u32 i = 0; i < MAX_RULES && (r = profile_record(REC_RULE, i)); i++) {
        const char *rl = rule_lib(r);
        if (strcmp(r->nid, nid) == 0 && (!rl[0] || (lib && strcmp(rl, lib) == 0))) {
            g_rules[i] = r;
            return rule_stub(i);
        }
    }
    return budget_resolve(nid);
}

COLD const char *profile_thread_name(void)
{
    static char name[32];
    fmt(name, sizeof(name), "%s-main", g_title[0] ? g_title : "title");
    return name;
}
