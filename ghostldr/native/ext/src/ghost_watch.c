/*
 * The watchdog's reads of Ghost of Tsushima 2.18's globals (profile flag
 * PF_DIAG, the ghost_diagnostics profile: only that build's fingerprint).
 * The core's watchdog (src/crash.c) logs its own lines every 20 s and calls
 * this between them (ext_exports.title_watch), so a PF_DIAG log reads as it
 * did when this code was in the core; moved here to keep the core image
 * small.  Developers' diagnostics: nothing here changes the title, except
 * the forced title-ready flag of the ctx-flag-32 experiment.
 *
 *   phase 0  frame time, timescale and its reasons, boot-flow-8 gates
 *   phase 1  movie pools, the forced title-ready flag, the title world target
 *   phase 2  the CPU malloc mspace, the blocking tasks (and the mission
 *            world's resources still loading)
 */
#include "ext.h"

static bool readable(u64 p)
{
    static s32 (*qmp)(void *, void **, void **, s32 *);
    if (!qmp)
        qmp = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelQueryMemoryProtection");
    void *st, *en;
    s32 prot = 0;
    return qmp && qmp((void *)(p & ~7UL), &st, &en, &prot) == 0 && (prot & PROT_READ);
}

/* module+offset, as the core's crash.c says it ("ghostldr" for the core) */
static u64 g_core_lo, g_core_hi;
static const char *describe(u64 addr, u64 *off)
{
    for (struct module *m = g_modules; m; m = m->next)
        if (addr >= m->base && addr < m->base + m->image_size) {
            *off = addr - m->base;
            return m->name;
        }
    if (addr >= g_core_lo && addr < g_core_hi) {
        *off = addr - g_core_lo;
        return "ghostldr";
    }
    *off = addr;
    return "?";
}

/* CUSA11456 2.18, the warp task's SpawnMissionWorld (state 27, eboot
 * 0xe5d64e): it waits until each resource the world lists is loaded.  The
 * list: [eboot+0x3ce40b8] +0x38 (16-byte entries: handle, u16 generation),
 * count +0x40; each handle is looked up in the load table (buckets at
 * eboot+0x28e2118, bucket count +0x28e2110; nodes key, resource, u32 hash,
 * next), and a resource is loaded once its +0xe0 is set.  Logs the ones
 * still waiting. */
static void log_mission_world_wait(u64 b)
{
    u64 w = *(u64 *)(b + 0x3ce40b8);
    if (!readable(w)) {
        logf("watchdog:   mission world list: none (%lx)", w);
        return;
    }
    s64 n = *(s32 *)(w + 0x40);
    u64 list = *(u64 *)(w + 0x38), *buckets = *(u64 **)(b + 0x28e2118);
    u32 nb = *(u32 *)(b + 0x28e2110), waiting = 0;
    logf("watchdog:   mission world list %lx: %ld resources, flag %u", w, n, *(u8 *)(w + 0x5c));
    for (s64 i = 0; i < n && i < 4096 && readable(list) && buckets && nb; i++) {
        u64 h = *(u64 *)(list + i * 16);
        u16 gen = *(u16 *)(list + i * 16 + 8);
        u32 hash = (u32)(h >> 32) ^ (u32)h;
        u64 node = buckets[hash % nb], res = 0;
        for (u32 hops = 0; node && hops < 64 && readable(node); node = *(u64 *)(node + 0x18), hops++)
            if (*(u32 *)(node + 0x10) == hash && *(u64 *)node == h) {
                res = *(u64 *)(node + 8);
                break;
            }
        bool ready = res && readable(res) && (u16)(*(u16 *)(res + 0xb0) ^ gen) <= 0xff && *(u64 *)(res + 0xe0);
        if (ready || waiting++ >= 4)
            continue;
        logf("watchdog:   waiting for #%ld handle %lx gen %x: resource %lx", i, h, gen, res);
        if (res && readable(res))
            log_hex("watchdog:   res", (const void *)res, 0x100);
    }
    logf("watchdog:   %u of %ld not loaded", waiting, n);
}

static void phase_timing(u64 b)
{
    union {
        u32 u;
        float f;
    } dt = {*(u32 *)(b + 0x16beac0)}, raw = {*(u32 *)(b + 0x16bea98)}, scale = {*(u32 *)(b + 0x16beac8)};
    logf("watchdog: game dt %x (x1e6 %d) raw %x (x1e6 %d) timescale %x (x1e3 %d) freq %lu flag %u", dt.u,
         (int)(dt.f * 1e6f), raw.u, (int)(raw.f * 1e6f), scale.u, (int)(scale.f * 1e3f), *(u64 *)(b + 0x4141598),
         *(u8 *)(b + 0x3ba3a99));
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

static void phase_world(u64 b)
{
    for (u32 slot = 0; slot < 3; slot++) {
        const u64 *mp = (const u64 *)(b + 0x288ae60 + 0x220 + slot * 0x70);
        logf("watchdog: movie pools slot %u: cpu limit %lx used %lx | cpu-gpu limit %lx used %lx | gpu limit %lx "
             "used %lx | owner %lx",
             slot, mp[3], mp[4], mp[7], mp[8], mp[11], mp[12], mp[13]);
    }
    /* experiment (ctx flag 32): signal the title world's "ready" flag that
     * boot-flow state 8 (WarpForTitleWorld) waits for */
    if ((g_ctx->flags & 32) && *(u8 *)(b + 0x1a83739) == 0) {
        *(volatile u8 *)(b + 0x1a83739) = 1;
        logf("watchdog: forced title-ready flag (eboot+0x1a83739)");
    }
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

static void phase_tasks(u64 b)
{
    /* the CPU malloc mspace (handle at eboot+0x28b1690), via the title's own libc */
    static u64 (*mstats)(u64, u64 *);
    if (!mstats) {
        char nid[12];
        compute_nid("sceLibcMspaceMallocStats", nid);
        for (struct module *m = g_modules; m && !mstats; m = m->next)
            if (!m->is_main)
                mstats = (u64 (*)(u64, u64 *))module_export(m, nid);
    }
    /* SceLibcMallocManagedSize: u16 size, u16 version, u32 reserved, then 4 x size_t */
    u64 msp = *(u64 *)(b + 0x28b1690), st[5] = {0x28 | (1UL << 16), 0, 0, 0, 0};
    if (mstats && msp) {
        u64 rc = mstats(msp, st);
        logf("watchdog: CPU mspace %lx rc %lx: max system %lu MiB, system %lu MiB, max in use %lu MiB, in use "
             "%lu MiB",
             msp, rc, st[1] >> 20, st[2] >> 20, st[3] >> 20, st[4] >> 20);
    }
    u32 nq = *(u32 *)(b + 0x3cc5810);
    u64 *q = *(u64 **)(b + 0x3cc5808);
    for (u32 i = 0; i < nq && i < 4 && q; i++) {
        u64 t = q[i], vt = t ? *(u64 *)t : 0, off;
        const char *vm = describe(vt, &off);
        u64 f2 = vt ? ((u64 *)vt)[2] : 0, foff;
        const char *fm = describe(f2, &foff);
        logf("watchdog: blocking task %u/%u %lx vtable %s+%lx isBusy %s+%lx", i, nq, t, vm, off, fm, foff);
        if (t)
            logf("watchdog:   task state %u enabled %u; load counter %d", *(u32 *)(t + 0x208), *(u8 *)(t + 0x239),
                 *(s32 *)(b + 0x28e2158));
        if (t && *(u32 *)(t + 0x208) == 27)
            log_mission_world_wait(b);
    }
}

void ghost_watch(u64 eboot, u32 phase, u64 core_lo, u64 core_hi) HIDDEN;
void ghost_watch(u64 eboot, u32 phase, u64 core_lo, u64 core_hi)
{
    g_core_lo = core_lo, g_core_hi = core_hi;
    if (phase == 0)
        phase_timing(eboot);
    else if (phase == 1)
        phase_world(eboot);
    else if (phase == 2)
        phase_tasks(eboot);
}
