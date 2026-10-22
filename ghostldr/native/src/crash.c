/* Crash reporting: log signal, fault address, registers and a frame-pointer
 * backtrace (as module+offset) over the UDP log before the process dies. */
#include "ghost.h"

struct ksigaction {
    void (*handler)(int, void *, void *);
    int flags;
    u32 mask[4];
};

#define SA_SIGINFO 0x40
#define SA_ONSTACK 0x1

static const char *describe(u64 addr, u64 *off)
{
    for (struct module *m = g_modules; m; m = m->next)
        if (addr >= m->base && addr < m->base + m->image_size) {
            *off = addr - m->base;
            return m->name;
        }
    extern char __image_start[] HIDDEN;
    extern char __image_end[] HIDDEN;
    if (addr >= (u64)__image_start && addr < (u64)__image_end) {
        *off = addr - (u64)__image_start;
        return "ghostldr";
    }
    *off = addr;
    return "?";
}

static bool readable(u64 p)
{
    static int (*qmp)(void *, void **, void **, int *);
    if (!qmp)
        qmp = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelQueryMemoryProtection");
    void *st, *en;
    int prot = 0;
    return qmp && qmp((void *)(p & ~7UL), &st, &en, &prot) == 0 && (prot & PROT_READ);
}

static volatile int g_crashed;

static void on_signal(int sig, void *info, void *ucv)
{
    u8 *uc = ucv;
    u64 *mc = (u64 *)(uc + 0x40); /* mcontext_t (PS4 ucontext has a 0x40-byte header) */
    static const char *names[] = {"onstack", "rdi", "rsi", "rdx", "rcx", "r8",  "r9",  "rax", "rbx",
                                  "rbp",     "r10", "r11", "r12", "r13", "r14", "r15"};
    u64 rip = mc[20], rsp = mc[23], rbp = mc[9];
    u64 fault = *(u64 *)((u8 *)info + 0x18);
    u64 off;
    const char *mod = describe(rip, &off);
    if (__atomic_exchange_n(&g_crashed, 1, __ATOMIC_SEQ_CST)) {
        logf("CRASH (nested) sig %d rip %lx (%s+%lx)", sig, rip, mod, off);
        for (;;)
            K.usleep(1000000);
    }
    logf("CRASH sig %d fault addr %lx rip %lx = %s+%lx rsp %lx (info %p uc %p)", sig, fault, rip, mod, off,
         rsp, info, ucv);
    for (struct module *m = g_modules; m; m = m->next)
        if (m->bx && rip >= m->base && rip - m->base < m->text_size && !bx_in_view(m, rip))
            logf("CRASH: rip is in %s's text but in none of its JIT views: code its code map missed", m->name);
    if (readable(rsp)) {
        u64 ret = *(u64 *)rsp;
        mod = describe(ret, &off);
        logf("  [rsp] %lx = %s+%lx (caller if rip is a bad call target)", ret, mod, off);
    }
    for (int i = 1; i < 16; i += 3)
        logf("  %-3s %016lx %-3s %016lx %-3s %016lx", names[i], mc[i], names[i + 1], mc[i + 1],
             i + 2 < 16 ? names[i + 2] : "", i + 2 < 16 ? mc[i + 2] : 0);
    if (readable(rip))
        log_hex("  code", (void *)rip, 32);
    /* frame-pointer chain */
    for (int depth = 0; depth < 24 && rbp && readable(rbp) && readable(rbp + 8); depth++) {
        u64 ret = ((u64 *)rbp)[1];
        mod = describe(ret, &off);
        logf("  #%02d %lx = %s+%lx", depth, ret, mod, off);
        u64 next = ((u64 *)rbp)[0];
        if (next <= rbp)
            break;
        rbp = next;
    }
    /* raw stack words that look like code addresses in our modules */
    for (int i = 0; i < 64 && readable(rsp + i * 8); i++) {
        u64 v = ((u64 *)rsp)[i];
        mod = describe(v, &off);
        if (mod[0] != '?')
            logf("  [rsp+%03x] %lx = %s+%lx", i * 8, v, mod, off);
    }
    logf("CRASH report end; parking thread");
    for (;;)
        K.usleep(1000000);
}

/* Non-fatal context snapshot (signal 30, sent by the watchdog): one line
 * with the interrupted rip and the frame-pointer chain. */
#define PROF_DEPTH 6
static volatile int g_prof_on;
static u64 g_prof_stack[PROF_DEPTH];

static void on_snapshot(int sig, void *info, void *ucv)
{
    u64 *mc = (u64 *)((u8 *)ucv + 0x40);
    if (g_prof_on) {
        u64 rbp = mc[9];
        g_prof_stack[0] = mc[20];
        for (int d = 1; d < PROF_DEPTH; d++) {
            u64 ret = 0;
            if (rbp && readable(rbp) && readable(rbp + 8)) {
                ret = ((u64 *)rbp)[1];
                u64 next = ((u64 *)rbp)[0];
                rbp = next > rbp ? next : 0;
            }
            g_prof_stack[d] = ret;
        }
        __atomic_store_n(&g_prof_on, 2, __ATOMIC_SEQ_CST);
        return;
    }
    u64 rip = mc[20], rbp = mc[9], off;
    char line[700];
    const char *mod = describe(rip, &off);
    int n = fmt(line, sizeof(line), "snap %s+%lx", mod, off);
    for (int d = 0; d < 10 && rbp && readable(rbp) && readable(rbp + 8) && n < (int)sizeof(line) - 48; d++) {
        u64 ret = ((u64 *)rbp)[1];
        mod = describe(ret, &off);
        n += fmt(line + n, sizeof(line) - n, " < %s+%lx", mod, off);
        u64 next = ((u64 *)rbp)[0];
        if (next <= rbp)
            break;
        rbp = next;
    }
    extern const char *g_snap_name;
    logf("[%s] %s", g_snap_name ? g_snap_name : "?", line);
    (void)sig, (void)info;
}

const char *g_snap_name;

u32 import_counts_top(u32 *idx_out, u64 *delta_out, u32 max, u64 *prev);
const char *import_name(u32 idx);
static u64 *g_count_prev; /* page_alloc'd: keeps the core image small */

static void log_busiest_imports(void)
{
    u32 idx[64];
    u64 delta[64];
    if (!g_count_prev && !(g_count_prev = page_alloc(512 * sizeof(u64))))
        return;
    u32 n = import_counts_top(idx, delta, 64, g_count_prev);
    /* file I/O and sleeps, whatever their rank */
    char io[400];
    int il = fmt(io, sizeof(io), "imports io:");
    for (u32 i = 0; i < n; i++) {
        const char *nm = import_name(idx[i]);
        if (strstr(nm, "Fios") || strstr(nm, "sceKernelRead") || strstr(nm, "Pread") || strstr(nm, "sceKernelOpen") ||
            strstr(nm, "sceKernelClose") || strstr(nm, "Lseek") || strstr(nm, "sleep") || strstr(nm, "Sleep"))
            il += fmt(io + il, sizeof(io) - il, " %s=%lu", nm, delta[i]);
    }
    logf("%s", io);
    if (n > 24)
        n = 24;
    char line[900];
    int len = fmt(line, sizeof(line), "imports:");
    for (u32 i = 0; i < n && len < (int)sizeof(line) - 64; i++)
        len += fmt(line + len, sizeof(line) - len, " %s=%lu", import_name(idx[i]), delta[i]);
    logf("%s", line);
}

/* 200 samples of the main thread's stack, 50 ms apart, grouped */
static void profile_main(int (*kill)(u64, int), u64 tid)
{
    static u64 (*stacks)[PROF_DEPTH]; /* page_alloc'd: keeps the core image small */
    static u32 *counts;
    if (!stacks && !(stacks = page_alloc(64 * sizeof(*stacks))))
        return;
    if (!counts && !(counts = page_alloc(64 * sizeof(*counts))))
        return;
    u32 nstacks = 0, total = 0;
    memset(counts, 0, 64 * sizeof(*counts));
    for (int i = 0; i < 200; i++) {
        __atomic_store_n(&g_prof_on, 1, __ATOMIC_SEQ_CST);
        kill(tid, 30);
        for (int w = 0; w < 50 && __atomic_load_n(&g_prof_on, __ATOMIC_SEQ_CST) != 2; w++)
            K.usleep(1000);
        if (g_prof_on == 2) {
            total++;
            u32 k = 0;
            while (k < nstacks && memcmp(stacks[k], g_prof_stack, sizeof(g_prof_stack)) != 0)
                k++;
            if (k == nstacks && nstacks < 64)
                memcpy(stacks[nstacks++], g_prof_stack, sizeof(g_prof_stack));
            if (k < 64)
                counts[k]++;
        }
        g_prof_on = 0;
        K.usleep(50 * 1000);
    }
    logf("profile: main thread, %u samples, %u distinct stacks", total, nstacks);
    for (u32 round = 0; round < 20; round++) {
        u32 best = 0;
        for (u32 k = 1; k < nstacks; k++)
            if (counts[k] > counts[best])
                best = k;
        if (!nstacks || !counts[best])
            break;
        char line[600];
        int n = fmt(line, sizeof(line), "profile: %3u", counts[best]);
        for (int d = 0; d < PROF_DEPTH && stacks[best][d]; d++) {
            u64 off;
            const char *mod = describe(stacks[best][d], &off);
            n += fmt(line + n, sizeof(line) - n, " %s+%lx", mod, off);
        }
        logf("%s", line);
        counts[best] = 0;
    }
}

static void *watchdog(void *arg)
{
    int (*kill)(u64, int) = host_dlsym((u32)g_ctx->libkernel_handle, "pthread_kill");
    u64 tids[192];
    const char *names[192];
    for (;;) {
        K.usleep(20 * 1000 * 1000);
        u32 n = ghost_thread_list(tids, names, 192);
        logf("watchdog: %u title threads", n);
        if (n > 26) {
            char line[700];
            int len = fmt(line, sizeof(line), "watchdog: newer threads:");
            for (u32 i = 26; i < n && len < (int)sizeof(line) - 40; i++)
                len += fmt(line + len, sizeof(line) - len, " %s", names[i]);
            logf("%s", line);
        }
        /* reads of Ghost of Tsushima 2.18's globals: only on that build (profile ghost_diagnostics) */
        bool diag = profile_has(PF_DIAG) != 0;
        if (g_main_mod) {
            u64 b = g_main_mod->base;
            if (diag) {
            union {
                u32 u;
                float f;
            } dt = {*(u32 *)(b + 0x16beac0)}, raw = {*(u32 *)(b + 0x16bea98)}, scale = {*(u32 *)(b + 0x16beac8)};
            logf("watchdog: game dt %x (x1e6 %d) raw %x (x1e6 %d) timescale %x (x1e3 %d) freq %lu flag %u", dt.u,
                 (int)(dt.f * 1e6f), raw.u, (int)(raw.f * 1e6f), scale.u, (int)(scale.f * 1e3f),
                 *(u64 *)(b + 0x4141598), *(u8 *)(b + 0x3ba3a99));
            char line[400];
            int len = fmt(line, sizeof(line), "watchdog: timescale reasons:");
            for (u32 r = 0; r < 28; r++) {
                union {
                    u32 u;
                    float f;
                } v = {*(u32 *)(b + 0x16beb1c + r * 4)};
                if (v.f != 1.0f)
                    len += fmt(line + len, sizeof(line) - len, " [%u]=%d/1000", r, (int)(v.f * 1000.0f));
            }
            logf("%s", line);
            logf("watchdog: bootflow-8 gates: 3bbb310=%d 16d28fc=%d 16d289c=%d 3ba42fc=%d 3d4653c=%d 16d28a0=%u "
                 "3d46630=%lx",
                 *(s32 *)(b + 0x3bbb310), *(s32 *)(b + 0x16d28fc), *(s32 *)(b + 0x16d289c), *(s32 *)(b + 0x3ba42fc),
                 *(s32 *)(b + 0x3d4653c), *(u8 *)(b + 0x16d28a0), *(u64 *)(b + 0x3d46630));
            }
            logf("watchdog: lazy reservations committed %u MiB", vdm_lazy_committed_mib());
            vdm_log_lazy();
            for (u32 slot = 0; diag && slot < 3; slot++) {
                const u64 *mp = (const u64 *)(b + 0x288ae60 + 0x220 + slot * 0x70);
                logf("watchdog: movie pools slot %u: cpu limit %lx used %lx | cpu-gpu limit %lx used %lx | gpu limit %lx "
                     "used %lx | owner %lx",
                     slot, mp[3], mp[4], mp[7], mp[8], mp[11], mp[12], mp[13]);
            }
            /* experiment (ctx flag 32): signal the title world's "ready" flag
             * that boot-flow state 8 (WarpForTitleWorld) waits for */
            if (diag && (g_ctx->flags & 32) && *(u8 *)(b + 0x1a83739) == 0) {
                *(volatile u8 *)(b + 0x1a83739) = 1;
                logf("watchdog: forced title-ready flag (eboot+0x1a83739)");
            }
            if (diag) {
                u64 want = *(u64 *)(b + 0x3d46630);
                s32 cnt = *(s32 *)(b + 0x3d46548);
                u64 list = *(u64 *)(b + 0x3d46540);
                char line[600];
                int len = fmt(line, sizeof(line), "watchdog: title-world target '%s', %d entries:",
                              want > 0x10000 ? (const char *)(want + 2) : "?", cnt);
                for (s32 i = 0; i < cnt && i < 12 && list; i++) {
                    u64 str = *(u64 *)(list + (u64)i * 0x38);
                    len += fmt(line + len, sizeof(line) - len, " '%s'", str > 0x10000 ? (const char *)(str + 2) : "?");
                }
                logf("%s", line);
            }
            for (u32 i = 0; i < g_dm_nusage && i < 32; i++)
                if (g_dm_usage[i].len >= 0x1000000)
                    logf("watchdog: direct alloc from eboot+%lx type %d: %lu MiB, mapped %lu MiB", g_dm_usage[i].from,
                         g_dm_usage[i].type, g_dm_usage[i].len >> 20, g_dm_usage[i].mapped >> 20);
            /* the CPU malloc mspace (handle at eboot+0x28b1690), via the title's own libc */
            static u64 (*mstats)(u64, u64 *);
            if (diag && !mstats) {
                char nid[12];
                void compute_nid(const char *name, char out[12]);
                compute_nid("sceLibcMspaceMallocStats", nid);
                for (struct module *m = g_modules; m && !mstats; m = m->next)
                    if (!m->is_main)
                        mstats = (u64 (*)(u64, u64 *))module_export(m, nid);
            }
            /* SceLibcMallocManagedSize: u16 size, u16 version, u32 reserved, then 4 x size_t */
            u64 msp = diag ? *(u64 *)(b + 0x28b1690) : 0, st[5] = {0x28 | (1UL << 16), 0, 0, 0, 0};
            if (mstats && msp) {
                u64 rc = mstats(msp, st);
                logf("watchdog: CPU mspace %lx rc %lx: max system %lu MiB, system %lu MiB, max in use %lu MiB, in use "
                     "%lu MiB",
                     msp, rc, st[1] >> 20, st[2] >> 20, st[3] >> 20, st[4] >> 20);
            }
            u32 nq = diag ? *(u32 *)(b + 0x3cc5810) : 0;
            u64 *q = diag ? *(u64 **)(b + 0x3cc5808) : NULL;
            for (u32 i = 0; i < nq && i < 4 && q; i++) {
                u64 t = q[i], vt = t ? *(u64 *)t : 0, off;
                const char *vm = describe(vt, &off);
                u64 f2 = vt ? ((u64 *)vt)[2] : 0, foff;
                const char *fm = describe(f2, &foff);
                logf("watchdog: blocking task %u/%u %lx vtable %s+%lx isBusy %s+%lx", i, nq, t, vm, off, fm, foff);
                if (t)
                    logf("watchdog:   task state %u enabled %u; load counter %d", *(u32 *)(t + 0x208),
                         *(u8 *)(t + 0x239), *(s32 *)(b + 0x28e2158));
            }
        }
        if (g_ctx->flags & 8)
            log_busiest_imports();
        if (profile_has(PF_HOOKS)) {
            extern u64 g_heap_live[3], g_heap_peak[3];
            logf("watchdog: engine heaps (>=64K allocs) type-0 pool live %lu MiB peak %lu; GPU heap live %lu MiB "
                 "peak %lu; other live %lu MiB",
                 g_heap_live[0] >> 20, g_heap_peak[0] >> 20, g_heap_live[1] >> 20, g_heap_peak[1] >> 20,
                 g_heap_live[2] >> 20);
        }
        extern u64 g_fb_addr[8];
        extern u32 g_fb_count;
        for (u32 b = 0; b < g_fb_count && b < 8; b++) {
            const u8 *px = (const u8 *)g_fb_addr[b];
            u64 rgb = 0, nz = 0, opaque = 0;
            for (u64 o = 0; px && o < 1920 * 1080 * 4; o += 4 * 61) {
                u32 v = px[o] + px[o + 1] + px[o + 2];
                rgb += v;
                nz += v != 0;
                opaque += px[o + 3] != 0;
            }
            logf("watchdog: framebuffer %u at %p: rgb sum %lu, %lu non-black, %lu alpha!=0 of %u sampled; first %08x",
                 b, px, rgb, nz, opaque, 1920 * 1080 / 61, px ? *(const u32 *)px : 0);
        }
        if ((g_ctx->flags & 16) && n && kill) {
            profile_main(kill, tids[0]);
            continue;
        }
        for (u32 i = 0; i < n && kill; i++) {
            g_snap_name = names[i];
            kill(tids[i], 30);
            K.usleep(150 * 1000);
        }
    }
    return arg;
}

void crash_start_watchdog(void)
{
    int (*sigaction)(int, const struct ksigaction *, struct ksigaction *) =
        host_dlsym((u32)g_ctx->libkernel_handle, "sigaction");
    struct ksigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.handler = on_snapshot;
    sa.flags = SA_SIGINFO | 0x2 /* SA_RESTART */;
    if (!sigaction || sigaction(30, &sa, NULL) != 0) {
        logf("watchdog: cannot install the snapshot handler");
        return;
    }
    u64 tid = 0;
    K.pthread_create(&tid, NULL, watchdog, NULL, "ghostldr-watchdog");
}

void crash_install(void)
{
    int (*sigaction)(int, const struct ksigaction *, struct ksigaction *) =
        host_dlsym((u32)g_ctx->libkernel_handle, "sigaction");
    if (!sigaction) {
        logf("crash: no sigaction");
        return;
    }
    struct ksigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.handler = on_signal;
    sa.flags = SA_SIGINFO;
    static const int sigs[] = {4, 8, 10, 11};
    for (u32 i = 0; i < ARRAY_SIZE(sigs); i++) {
        int rc = sigaction(sigs[i], &sa, NULL);
        if (rc != 0)
            logf("crash: sigaction(%d) rc=%d", sigs[i], rc);
    }
}
