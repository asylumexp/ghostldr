/*
 * The PS4 diagnostic (run.py probe --diag; docs/ps4.md says what to send
 * back).  It logs what the loader needs to know about a console and boots
 * nothing: no memory is released, no module placed, the host's threads are
 * only looked at.  The checks are the PS4 boot's own (arena.c with ctx flag
 * CTX_FLAG_PS4, set here on any console), so a PS5 run shows them passing.
 *
 *  1. platform, firmware, Pro mode, memory, the direct/flexible census;
 *  2. the emulator's JIT buffers: this process's map of them, the compiler
 *     process's map as its sceKernelVirtualQuery lists it (without the JIT
 *     mappings, on a PS5), and per buffer whether the compiler writes it past
 *     its first 16 KiB (probes that return EFAULT rather than crash);
 *  3. the host's threads (host.c): alive (a blocked thread counts) and name;
 *     the loader's scePthreadCancel guess; SWRR's heap and what a boot would
 *     release;
 *  4. writes through the compiler, each put back: the shared scratch, the
 *     free small area after the core, each buffer's ends;
 *  5. new JIT blocks of 64 KiB to 32 MiB, mapped here into a reserved range:
 *     created, a function written and called, destroyed (stops at the first
 *     failure);
 *  6. last: each buffer's first 16 KiB, read and written through the
 *     compiler by the same probes;
 *  7. on a PS4 only, before 4-6: SWRR's budget, the kernel's limits for the
 *     application (the 2.5 GiB of the sixth report), and one try to raise
 *     its direct memory limit, put back at once.
 */
#include "ghost.h"

#define COLD GHOST_COLD
#define MAX_MAP 512

void host_dmem_census(void);

static void *lk(const char *name) { return host_dlsym((u32)g_ctx->libkernel_handle, name); }

COLD static void diag_platform(void)
{
    struct boot_ctx *c = g_ctx;
    u64 fw = c->size >= __builtin_offsetof(struct boot_ctx, fw) + 8 ? c->fw : 0;
    struct {
        u64 size;
        char str[0x1c];
        u32 ver;
    } sw = {sizeof(sw), {0}, 0};
    int (*swver)(void *) = lk("sceKernelGetSystemSwVersion");
    int (*neo)(void) = lk("sceKernelIsNeoMode");
    int rc = swver ? swver(&sw) : -1;
    sw.str[sizeof(sw.str) - 1] = 0;
    logf("diag: %s; the loader says %s, firmware %lx.%02lx; system software '%s' (%x, rc %x); Neo mode %d",
         g_hostgame->name, (c->flags & CTX_FLAG_PS4) ? "PS4" : "PS5", fw >> 8, fw & 0xff, sw.str, sw.ver, rc,
         neo ? neo() : -1);
    off_t s = 0;
    size_t avail = 0, flex = 0;
    K.avail_direct(0, (off_t)K.direct_size(), 0x4000, &s, &avail);
    K.avail_flex(&flex);
    logf("diag: direct memory %lu MiB (%lu MiB free), flexible free %lu MiB", (u64)K.direct_size() >> 20,
         (u64)avail >> 20, (u64)flex >> 20);
    /* a PS4 hands out less than its free holes (budget.c); measured there only */
    if (c->flags & CTX_FLAG_PS4)
        logf("diag: the kernel hands out %lu MiB more direct memory (type 0), %lu MiB (type 3)",
             budget_allocatable(0) >> 20, budget_allocatable(3) >> 20);
    logf("diag: eboot %lx, bridge %lx, IPU buffer %lx, core end %lx, staging %lx+%lx, scratch %lx", c->eboot_base,
         c->reserved[1], c->reserved[2], c->reserved[3], c->staging, c->staging_size, c->oob_scratch);
    /* the gadget's bytes are not read here: system code in a PS5's compiler
     * is execute-only and reading it kills the host (ghostldr.lua checks them on
     * a PS4, where they are readable) */
    logf("diag: compiler: memcpy %lx, VirtualQuery %lx, syscall gadget %lx%s, JIT shm %lx/%lx/%lx", c->jit_memcpy,
         c->reserved[0], c->jit_syscall, c->jit_syscall ? "" : " (none: new JIT blocks impossible)", c->jit_shm_create,
         c->jit_shm_alias, c->jit_shm_map);
    /* a PS4 runs each system call only from its own stub (jitrpc.c) */
    const u64 *w = (const u64 *)c->jit_sysw;
    u32 n = 0;
    for (; w && n < 64 && w[2 * n]; n++)
        logf("diag: compiler libkernel stub for system call %lu: %lx", w[2 * n], w[2 * n + 1]);
    if ((c->flags & CTX_FLAG_PS4) && !n)
        logf("diag: no libkernel stubs for the compiler: its system calls are refused on a PS4");
}

COLD static void log_map(const char *who, const struct vmq *q)
{
    char name[33];
    memcpy(name, q->name, 32);
    name[32] = 0;
    logf("diag: %s %lx-%lx %7lu KiB prot %x type %d flags %x off %lx '%s'", who, q->start, q->end,
         (q->end - q->start) >> 10, q->prot, q->type, q->flags, q->offset, name);
}

/* this process's map from the bridge to past the VU heaps, every entry */
COLD static void diag_main_map(void)
{
    struct vmq q;
    u64 lo = g_ctx->reserved[1] ? g_ctx->reserved[1] : 0x900000000, hi = lo + 0x6000000;
    for (u64 a = lo; a < hi;) {
        memset(&q, 0, sizeof(q));
        if (K.virtual_query((void *)a, 1, &q, sizeof(q)) != 0 || q.start >= hi || q.end <= a)
            break;
        log_map("here    ", &q);
        a = q.end;
    }
}

COLD static void diag_jit_views(void)
{
    struct vmq *map = page_alloc(MAX_MAP * sizeof(*map));
    u32 n = 0;
    for (u64 a = 0x10000; map && n < MAX_MAP && a < 0x800000000000UL; a = map[n - 1].end)
        if (!jit_vmap(a, 0x800000000000UL, &map[n], 1) || map[n++].end <= a)
            break;
    logf("diag: the compiler process's sceKernelVirtualQuery lists %u mappings (JIT mappings are not among them on a PS5):",
         n);
    for (u32 i = 0; i < n; i++)
        log_map("compiler", &map[i]);
    diag_main_map();
    /* the PS4 boot's own checks: each buffer past its header and the small
     * area, probed at both ends (16 bytes written by the compiler, put back) */
    u64 lo = 0, hi = 0;
    int rc = arena_init();
    int rc2 = rc == 0 ? arena_code_region(&lo, &hi) : -1;
    logf("diag: as a PS4 boot sees it: arena %d, code region %d %lx-%lx (first module at %lx)", rc, rc2, lo, hi,
         rc2 == 0 ? ALIGN_UP(lo, 0x10000) : 0);
    if (map)
        page_free(map, MAX_MAP * sizeof(*map));
}

COLD static void diag_threads(void)
{
    u64 eb = g_ctx->eboot_base;
    int (*kill)(u64, int) = lk("pthread_kill");
    int (*getname)(u64, char *) = lk("scePthreadGetname");
    for (u32 i = 0; i < ARRAY_SIZE(g_hostgame->threads) && g_hostgame->threads[i].name; i++) {
        u64 h = *(volatile u64 *)(eb + g_hostgame->threads[i].handle);
        int alive = h && kill ? kill(h, 0) : -1;
        char name[40] = "";
        if (alive == 0 && getname)
            getname(h, name);
        name[39] = 0;
        logf("diag: thread %-42s %lx: %s, name '%s'", g_hostgame->threads[i].name, h,
             !h ? "never created" : alive == 0 ? "ALIVE" : alive == 3 ? "gone" : "unknown", name);
    }
    u64 self = *(volatile u64 *)(eb + g_hostgame->pthread_self);
    void *cancel = lk("scePthreadCancel");
    logf("diag: the loader cancels with scePthreadSelf+0x10 = %lx; scePthreadCancel is %p (%s)", self + 0x10, cancel,
         self + 0x10 == (u64)cancel ? "the same" : "NOT the same: its cancels may not have happened");
}

/* SWRR's heap and what a boot's hostmem.c would release (only looked at) */
COLD static void diag_memory(void)
{
    u64 eb = g_ctx->eboot_base, heap = 0;
    if (!is_jakx()) { /* SWRR offsets: in Jak X they are execute-only code */
        heap = *(volatile u64 *)(eb + 0x3906c0);
        logf("diag: SWRR heap (mspace) %lx; companion buffer %lx; services quit word %d", heap,
             *(volatile u64 *)(eb + 0x182f338), *(volatile s32 *)(eb + 0x182f328));
    }
    struct vmq q;
    for (u64 a = 0x200000000; a < 0x280000000;) {
        memset(&q, 0, sizeof(q));
        if (K.virtual_query((void *)a, 1, &q, sizeof(q)) != 0 || q.start >= 0x280000000)
            break;
        if ((q.flags & 2) && q.end - q.start >= 0x800000)
            logf("diag: a boot releases %lx-%lx (%lu MiB, prot %x type %d)%s", q.start, q.end, (q.end - q.start) >> 20,
                 q.prot, q.type, heap >= q.start && heap < q.end ? ": SWRR's heap" : "");
        a = q.end;
    }
    host_dmem_census();
}

/* ---- 7. PS4: SWRR's budget (docs/ps4.md, the sixth report) ----
 * System calls go through this process's own libkernel stub for each number
 * (a PS4 runs a system call only from its own stub).  Numbers, and the
 * prototypes of getid, get_ptype, get_proc_type_info and the resident
 * counts, are psdevwiki's; budget_get's and budget_set's are unknown, so
 * everything the kernel may write ends at an unmapped page: a wrong guess
 * fails with EFAULT and never overwrites memory.  The libkernel functions
 * around them (sceKernelGetBudget, sceKernelSetGameDirectMemoryLimit, ...)
 * are only dumped, for their prototypes, never called. */
#define MiB (1UL << 20)
typedef s64 (*sysfn)(u64, u64, u64, u64, u64, u64);

static const struct {
    u32 nr;
    const char *name;
} k_budget_sys[] = {
    {570, "budget_get"},        {571, "budget_set"},           {609, "budget_getid"},
    {610, "budget_get_ptype"},  {612, "get_proc_type_info"},  {613, "get_resident_count"},
    {615, "get_resident_fmem_count"},
};
enum { B_GET, B_SET, B_GETID, B_PTYPE, B_PTINFO, B_RES, B_FMEM };

/* this number's "mov rax, N; mov r10, rcx; syscall" in libkernel's text */
COLD static sysfn lk_stub(const u8 *t, u64 size, u32 nr)
{
    static const u8 tail[5] = {0x49, 0x89, 0xca, 0x0f, 0x05};
    for (u64 i = 0; i + 12 <= size; i++) {
        u32 n;
        memcpy(&n, t + i + 3, 4);
        if (t[i] == 0x48 && t[i + 1] == 0xc7 && t[i + 2] == 0xc0 && n == nr && memcmp(t + i + 7, tail, 5) == 0)
            return (sysfn)(u64)(t + i);
    }
    return NULL;
}

/* 64 KiB of zeroes with an unmapped page right after them */
COLD static u8 *guarded(void)
{
    void *p = NULL;
    if (K.mmap(NULL, 0x20000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0, &p) != 0 || !p)
        return NULL;
    K.munmap((u8 *)p + 0x10000, 0x10000);
    memset(p, 0, 0x10000);
    return p;
}

/* bytes up to the last non-zero one, rounded up to 8 */
COLD static u64 used_len(const u8 *b, u64 max)
{
    u64 n = max;
    while (n && !b[n - 1])
        n--;
    return ALIGN_UP(n, 8);
}

/* sigaction's argument (crash.c's) */
struct bsigaction {
    u64 handler;
    int flags;
    u32 mask[4];
};
#define SIGSYS 12

COLD static void diag_budget_calls(void);

/* A system call the kernel refuses through nosys() also sends SIGSYS, which
 * would end SWRR: it is ignored while the calls run, then put back. */
COLD static void diag_budget(void)
{
    int (*sigaction)(int, const struct bsigaction *, struct bsigaction *) = lk("sigaction");
    struct bsigaction ign, old;
    memset(&ign, 0, sizeof(ign));
    memset(&old, 0, sizeof(old));
    ign.handler = 1; /* SIG_IGN */
    if (!sigaction || sigaction(SIGSYS, &ign, &old) != 0) {
        logf("diag: budget: SIGSYS cannot be ignored here: nothing called");
        return;
    }
    diag_budget_calls();
    sigaction(SIGSYS, &old, NULL);
}

COLD static void diag_budget_calls(void)
{
    struct sce_module_info lkm;
    memset(&lkm, 0, sizeof(lkm));
    lkm.size = sizeof(lkm);
    u32 seg = 0;
    if (K.module_info((u32)g_ctx->libkernel_handle, &lkm) != 0) {
        logf("diag: budget: no module info for libkernel");
        return;
    }
    while (seg < lkm.nsegs && seg < 4 && !(lkm.segs[seg].prot & PROT_EXEC))
        seg++;
    if (seg >= lkm.nsegs || seg >= 4) {
        logf("diag: budget: libkernel's text not found");
        return;
    }
    const u8 *text = (const u8 *)lkm.segs[seg].base;
    sysfn f[ARRAY_SIZE(k_budget_sys)];
    for (u32 i = 0; i < ARRAY_SIZE(k_budget_sys); i++) {
        f[i] = lk_stub(text, lkm.segs[seg].size, k_budget_sys[i].nr);
        logf("diag: budget: %s (%u): libkernel stub %lx%s", k_budget_sys[i].name, k_budget_sys[i].nr, (u64)f[i],
             f[i] ? "" : " (none: not called)");
    }
    static const char *const fns[] = {"sceKernelGetBudget", "sceKernelSetBudget", "sceKernelGetResourceLimit",
                                      "sceKernelGetResource", "sceKernelGetProcessTypeOfBudget",
                                      "sceKernelGetProcessType", "sceKernelSetGameDirectMemoryLimit"};
    for (u32 i = 0; i < ARRAY_SIZE(fns); i++) { /* a PS4's libkernel is readable; nothing here is called */
        const void *a = lk(fns[i]);
        logf("diag: budget: %s at %lx%s", fns[i], (u64)a, a ? ", its first 96 bytes:" : " (not exported here)");
        if (a)
            log_hex("diag:   code", a, 96);
    }
    u8 *g = guarded();
    if (!g) {
        logf("diag: budget: no guarded buffer (mmap failed)");
        return;
    }
    int *err = K.errno_fn();
    u64 pid = (u64)K.getpid();
    s64 id = f[B_GETID] ? f[B_GETID](0, 0, 0, 0, 0, 0) : -1;
    logf("diag: budget: getid() -> %ld (errno %d)", id, id < 0 ? *err : 0);
    if (f[B_PTYPE] && id >= 0) {
        s64 pt = f[B_PTYPE]((u64)id, 0, 0, 0, 0, 0);
        logf("diag: budget: get_ptype(%ld) -> %ld (errno %d)", id, pt, pt < 0 ? *err : 0);
    }
    if (f[B_PTINFO]) {
        u8 *pti = g + 0x10000 - 0x100;
        s64 r = f[B_PTINFO]((u64)pti, 0, 0, 0, 0, 0);
        logf("diag: budget: get_proc_type_info() -> %ld (errno %d)", r, r < 0 ? *err : 0);
        if (r >= 0)
            log_hex("diag:   info", pti, 64);
        memset(pti, 0, 0x100);
    }
    if (f[B_RES]) {
        s64 r = f[B_RES](pid, 0, 0, 0, 0, 0);
        logf("diag: budget: get_resident_count(%lu) -> %ld (errno %d)", pid, r, r < 0 ? *err : 0);
    }
    if (f[B_FMEM]) {
        s64 r = f[B_FMEM](pid, 0, 0, 0, 0, 0);
        logf("diag: budget: get_resident_fmem_count(%lu) -> %ld (errno %d)", pid, r, r < 0 ? *err : 0);
    }
    if (!f[B_GET] || id < 0) {
        K.munmap(g, 0x10000);
        return;
    }
    /* budget_get(id, out, &count), else budget_get(id, out, count): out runs
     * to the unmapped page, the count is a u32 at the buffer's start */
    u32 *cnt = (u32 *)g;
    u8 *out = g + 0x1000;
    *cnt = 64;
    int shape = 0;
    s64 gr = f[B_GET]((u64)id, (u64)out, (u64)cnt, 0, 0, 0);
    logf("diag: budget: get(%ld, out, &count 64) -> %ld (errno %d), count now %u", id, gr, gr < 0 ? *err : 0, *cnt);
    if (gr < 0) {
        memset(out, 0, 0x10000 - 0x1000);
        shape = 1;
        gr = f[B_GET]((u64)id, (u64)out, 64, 0, 0, 0);
        logf("diag: budget: get(%ld, out, 64) -> %ld (errno %d)", id, gr, gr < 0 ? *err : 0);
    }
    u64 n = used_len(out, 0x7000);
    if (n)
        log_hex("diag:   budget", out, n < 0x400 ? n : 0x400);
    else
        logf("diag: budget: nothing written");
    /* the direct memory limit: the u64 near the 2.5 GiB SWRR gets */
    s64 at = -1;
    for (u64 o = 0; gr >= 0 && o + 8 <= n; o += 8) {
        u64 v;
        memcpy(&v, out + o, 8);
        if (v >= 2300 * MiB && v <= 2800 * MiB) {
            logf("diag: budget: +%lx holds %lu MiB: taken for the direct memory limit", o, v / MiB);
            at = (s64)o;
            break;
        }
    }
    if (at < 0 || !f[B_SET]) {
        logf("diag: budget: set not tried (%s)", !f[B_SET] ? "no stub" : gr < 0 ? "get failed" : "no limit near 2.5 GiB found");
        K.munmap(g, 0x10000);
        return;
    }
    /* one try to raise it to the 4608 MiB a PS4 game's range spans; put
     * back at once with what get returned */
    u8 *raised = g + 0x8000;
    memcpy(raised, out, n);
    u64 want = 4608 * MiB;
    memcpy(raised + at, &want, 8);
    u64 a3 = shape ? 64 : (u64)cnt;
    s64 sr = f[B_SET]((u64)id, (u64)raised, a3, 0, 0, 0);
    logf("diag: budget: set(%ld, limit -> %lu MiB) -> %ld (errno %d)", id, want / MiB, sr, sr < 0 ? *err : 0);
    if (sr >= 0) {
        logf("diag: budget: now the kernel hands out %lu MiB more direct memory (type 0)", budget_allocatable(0) / MiB);
        s64 br = f[B_SET]((u64)id, (u64)out, a3, 0, 0, 0);
        logf("diag: budget: set back -> %ld (errno %d)", br, br < 0 ? *err : 0);
    }
    K.munmap(g, 0x10000);
}

COLD static void diag_writes(void)
{
    /* the shared scratch: a compiler write must show here */
    u64 at = g_ctx->oob_scratch + 0x7f000, test = 0x1234567890abcdefULL;
    *(volatile u64 *)at = 0;
    jit_copy_to(at, &test, 8);
    logf("diag: compiler write to the shared scratch %lx: %s", at, *(volatile u64 *)at == test ? "ok" : "NOT SEEN");
    *(volatile u64 *)at = 0;
    /* new JIT blocks, each mapped here at a fixed address inside a reserved
     * range: mapping one without an address puts it at its home address over
     * whatever is there.  A PS5 13.60 makes at most 768 KiB (0x8002000c past
     * it), so this stops at the first failure. */
    static const u32 k_kib[] = {64, 768, 1024, 4096, 16384, 32768};
    void *va = NULL;
    if (K.reserve_va(&va, 0x2400000, 0, 0x100000) != 0 || !va) {
        logf("diag: no address range for JIT block tests");
        return;
    }
    for (u32 i = 0; i < ARRAY_SIZE(k_kib); i++) {
        u64 size = (u64)k_kib[i] << 10, want = (u64)va + 0x100000 + i, got = 0;
        struct jit_block b;
        u8 code[11] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xC3}; /* mov rax, imm64; ret */
        *(u64 *)(code + 2) = want;
        int rc = jit_block_create(&b, size, (u64)va);
        if (rc == 0 && jit_block_write(&b, size - 0x4000, code, sizeof(code)) == 0 &&
            memcmp((void *)(b.rx + size - 0x4000), code, sizeof(code)) == 0)
            got = ((u64(*)(void))(b.rx + size - 0x4000))();
        logf("diag: JIT block of %lu KiB: create %d, compiler RW %lx, here RX %lx (wanted %p), call %s", size >> 10, rc,
             b.rw, b.rx, va, got == want ? "ok" : "FAILED");
        jit_block_destroy(&b);
        if (got != want)
            break;
    }
    K.munmap(va, 0x2400000);
}

/* Last, as it may meet live code: each buffer's first 16 KiB, read through
 * the compiler, then written (16 bytes, put back).  Neither can crash it. */
COLD static void diag_headers(void)
{
    u64 qa = g_ctx->reserved[2], last = 0;
    int (*qmp)(void *, void **, void **, int *) = lk("sceKernelQueryMemoryProtection");
    for (u64 a = qa + 0x40000; qmp && qa && a < qa + 0x5000000; a += 0x4000) {
        void *st = 0, *en = 0;
        int prot = 0;
        if (a < last || qmp((void *)a, &st, &en, &prot) != 0)
            continue;
        last = (u64)en;
        if (prot != (PROT_READ | PROT_EXEC) || (u64)en - (u64)st < 0x10000)
            continue;
        u64 h = (u64)st, old[2] = {0, 0};
        int rd = jit_peek(h, old, 16);
        bool same = rd == 0 && memcmp(old, (const void *)h, 16) == 0;
        int wr = rd == 0 ? jit_stamp(h) : -1;
        if (wr == 0)
            jit_copy_to(h, old, 16);
        logf("diag: buffer %lx-%lx, its first 16 bytes through the compiler: read %s%s, write %s", h, (u64)en,
             rd ? "EFAULT" : "ok", rd ? "" : same ? " (same bytes here)" : " (OTHER bytes here)", wr ? "EFAULT" : "ok");
    }
}

COLD int ghost_diag(void)
{
    logf("diag: start (nothing is booted or released)");
    bool ps4 = is_ps4();
    diag_platform();
    g_ctx->flags |= CTX_FLAG_PS4; /* from here on: the PS4 boot's checks, on any console */
    diag_threads();
    diag_memory();
    if (ps4)
        diag_budget();
    diag_jit_views();
    diag_writes();
    diag_headers();
    logf("diag: done");
    return 0;
}
