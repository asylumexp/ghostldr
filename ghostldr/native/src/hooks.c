/*
 * Inline hooks on the title's own code (diagnostics).
 *
 * The entry of a hooked function is overwritten in the text image (before it
 * is uploaded) with `jmp rel32` to a C hook; the displaced prologue moves to
 * a trampoline in the stub area that jumps back past the patch.  Prologues
 * are matched byte for byte and must not contain rip-relative instructions.
 */
#include "ghost.h"

u64 stub_alloc_code(const u8 *code, u32 len);

typedef u64 (*fn6)(u64, u64, u64, u64, u64, u64);

struct hook {
    u64 offset;
    u8 prologue[16];
    u32 len;
    void *hook;
    fn6 *orig;
    const char *name;
};

static u64 caller_off(u64 ra)
{
    return g_main_mod && ra >= g_main_mod->base ? ra - g_main_mod->base : ra;
}

/* Live-usage accounting for the engine heaps (allocations >= 64 KiB):
 * [0] type-0 pool (heap object eboot+0x16b2930), [1] general GPU heap
 * (eboot+0x16b29f8), [2] others.  Reported by the watchdog. */
#define TRACK_MIN 0x10000
#define TRACK_SLOTS 8192
struct track_ent {
    u64 ptr;
    u32 size_k;
    u32 ra1, ra2; /* eboot offsets: the allocator's caller and its caller */
    u8 heap;
};
static struct track_ent *g_track; /* page_alloc'd: keeps the core image small */
static volatile int g_track_lock;
u64 g_heap_live[3], g_heap_peak[3];

static u32 heap_index(u64 heap)
{
    u64 off = heap - g_main_mod->base;
    return off == 0x16b2930 ? 0 : off == 0x16b29f8 ? 1 : 2;
}

static void track_add(u64 ptr, u64 size, u32 h, u32 ra1, u32 ra2)
{
    if (!g_track)
        return;
    spin_lock(&g_track_lock);
    for (u32 i = (u32)(ptr >> 8) % TRACK_SLOTS, n = 0; n < TRACK_SLOTS; n++, i = (i + 1) % TRACK_SLOTS)
        if (!g_track[i].ptr) {
            g_track[i].ptr = ptr, g_track[i].size_k = (u32)(size >> 10), g_track[i].heap = (u8)h;
            g_track[i].ra1 = ra1, g_track[i].ra2 = ra2;
            g_heap_live[h] += size;
            if (g_heap_live[h] > g_heap_peak[h])
                g_heap_peak[h] = g_heap_live[h];
            break;
        }
    spin_unlock(&g_track_lock);
}

static void track_del(u64 ptr)
{
    if (!g_track)
        return;
    spin_lock(&g_track_lock);
    for (u32 i = (u32)(ptr >> 8) % TRACK_SLOTS, n = 0; n < TRACK_SLOTS && g_track[i].ptr; n++,
             i = (i + 1) % TRACK_SLOTS)
        if (g_track[i].ptr == ptr) {
            g_heap_live[g_track[i].heap] -= (u64)g_track[i].size_k << 10;
            /* backward-shift deletion keeps the probe chains intact */
            u32 j = i;
            for (;;) {
                j = (j + 1) % TRACK_SLOTS;
                if (!g_track[j].ptr)
                    break;
                u32 home = (u32)(g_track[j].ptr >> 8) % TRACK_SLOTS;
                if ((j > i && (home <= i || home > j)) || (j < i && home <= i && home > j)) {
                    g_track[i] = g_track[j];
                    i = j;
                }
            }
            g_track[i].ptr = 0;
            break;
        }
    spin_unlock(&g_track_lock);
}

/* eboot 0x390d80: engine heap allocate (heap, size, align) */
static fn6 o_heap_alloc;
static u64 h_heap_alloc(u64 heap, u64 size, u64 align, u64 a3, u64 a4, u64 a5)
{
    u64 r = o_heap_alloc(heap, size, align, a3, a4, a5);
    if (!r)
        logf("heap_alloc(heap %lx, size %lx, align %lx) -> %lx from eboot+%lx", heap, size, align, r,
             caller_off((u64)__builtin_return_address(0)));
    else if (size >= TRACK_MIN) {
        /* the caller keeps a frame pointer (title code is built with them): its
         * saved rbp is at our frame's base, its return address just above */
        u64 *fp = __builtin_frame_address(0), *cfp = (u64 *)fp[0];
        u64 ra2 = (cfp > fp && (u64)cfp - (u64)fp < 0x100000) ? cfp[1] : 0;
        track_add(r, size, heap_index(heap), (u32)caller_off((u64)__builtin_return_address(0)), (u32)caller_off(ra2));
    }
    return r;
}

/* eboot 0x391290: engine heap free (ptr) */
static fn6 o_heap_free;
static u64 h_heap_free(u64 ptr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
    if (ptr)
        track_del(ptr);
    return o_heap_free(ptr, a1, a2, a3, a4, a5);
}

/* eboot 0xe593e0: boot flow state machine, (flow, requested state) */
static fn6 o_boot_step;
static u64 h_boot_step(u64 flow, u64 want, u64 a2, u64 a3, u64 a4, u64 a5)
{
    static u32 last_state = ~0u, last_want = ~0u, last_ctr = ~0u, last_g = ~0u;
    u32 st = *(u32 *)(flow + 0x30), ctr = *(u32 *)(flow + 0x48);
    u64 b = g_main_mod->base;
    u32 g3 = *(u32 *)(b + 0x3cb5bcc);
    if (st != last_state || (u32)want != last_want || ctr != last_ctr || g3 != last_g) {
        last_state = st, last_want = (u32)want, last_ctr = ctr, last_g = g3;
        logf("bootflow %lx: state %u want %u ctr %u | splash-hidden %u g3bbb310 %u g285d8bc %d g3cb5bcc %u", flow, st,
             (u32)want, ctr, *(u8 *)(b + 0x288b6d0), *(u32 *)(b + 0x3bbb310), *(s32 *)(b + 0x285d8bc), g3);
    }
    u64 r = o_boot_step(flow, want, a2, a3, a4, a5);
    u32 st2 = *(u32 *)(flow + 0x30);
    if (st2 != st)
        logf("bootflow %lx: -> state %u", flow, st2);
    return r;
}

/* eboot 0x8858e0: movie player "finished?" (player) */
static fn6 o_movie_done;
static u64 h_movie_done(u64 player, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
    static u32 last = ~0u, calls;
    u32 st = *(u32 *)(player + 0x40), ab = *(u32 *)(player + 0x26f8);
    u64 r = o_movie_done(player, a1, a2, a3, a4, a5);
    if (st != last || (calls++ % 300) == 0) {
        last = st;
        const u32 *w = (const u32 *)player;
        logf("movie %lx: state %u abort %u -> done %lu | %08x %08x %08x %08x %08x %08x %08x %08x", player, st, ab,
             r & 0xff, w[0x44 / 4], w[0x48 / 4], w[0x4c / 4], w[0x50 / 4], w[0x54 / 4], w[0x58 / 4], w[0x5c / 4],
             w[0x60 / 4]);
    }
    return r;
}

/* eboot 0x88a8b0: movie player set state (manager, player, state) */
static fn6 o_movie_state;
static u64 h_movie_state(u64 mgr, u64 player, u64 state, u64 a3, u64 a4, u64 a5)
{
    logf("movie_set_state(player %lx: %d -> %d) from eboot+%lx", player, *(s32 *)(player + 0x40), (s32)state,
         caller_off((u64)__builtin_return_address(0)));
    return o_movie_state(mgr, player, state, a3, a4, a5);
}

/* eboot 0x650d80: streamed resource "still loading?" (stream) */
static fn6 o_stream_busy;
static u64 h_stream_busy(u64 st, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
    static u32 calls;
    u64 r = o_stream_busy(st, a1, a2, a3, a4, a5);
    if ((calls++ % 150) == 0) {
        u64 ck = *(u64 *)(st + 0x18);
        logf("stream %lx busy %lu: err %d state %d cb %lx pos %lx win %lx-%lx chunk %lx{idx %d off %d ready %u}", st,
             r & 0xff, *(s32 *)(st + 0xf0), *(s32 *)(st + 0xf4), *(u64 *)(st + 0xe0), *(u64 *)(st + 0xc8),
             *(u64 *)(st + 0x80), *(u64 *)(st + 0x88), ck, ck ? *(s32 *)(ck + 0x28) : -1,
             ck ? *(s32 *)(ck + 0x2c) : -1, ck ? *(u8 *)(ck + 0x30) : 0);
    }
    return r;
}

/* eboot 0xa2e200: create a tracked load request (counted at 0x28e2158) */
static fn6 o_load_req;
static u64 h_load_req(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
    char name[96] = "";
    /* a1 is a pointer to a refcounted string (u16 refcount, then chars) or a C string */
    if (a1 > 0x10000) {
        const char *p = (const char *)a1;
        const char *q = (p[0] >= 0x20 && p[0] < 0x7f) ? p : p + 2;
        u32 i = 0;
        for (; i < sizeof(name) - 1 && q[i] >= 0x20 && q[i] < 0x7f; i++)
            name[i] = q[i];
        name[i] = 0;
    }
    u64 r = o_load_req(a0, a1, a2, a3, a4, a5);
    logf("load_req(%lx, %lx '%s', %lx, %lx, %lx) -> %lx from eboot+%lx; counter %d", a0, a1, name, a2, a3, a4, r,
         caller_off((u64)__builtin_return_address(0)), *(s32 *)(g_main_mod->base + 0x28e2158));
    return r;
}

/* eboot 0xe5c310: warp/load task step (task); state at +0x208 */
static const char *const k_warp_states[] = {
    "Pending", "Start", "HideScreen", "UnloadStart", "UnloadHero", "UnloadRestWorld", "WaitForNetConnection",
    "LoadPackManifest", "WaitForTCSQuery", "LoadStory", "ResolveCheckpoint", "ResolveWorld", "FinishInstall",
    "LangCheck", "LangAbortPulse", "LangUnloadText", "LangLoadText", "LangCheckAudio", "LangUnloadAudio",
    "LangLoadAudio", "PlayMovieDuringWarp", "SyncPackManifest", "WaitForNetworkPacks", "LoadNetworkPacks",
    "LoadWorld", "SpawnHero", "LoadRequiredPacks", "SpawnMissionWorld", "SpawnMission", "WaitForNetPosition",
    "LoadRegion", "SetTransforms", "StartMission", "WaitForTerrainDraw", "WaitForPathManager",
    "WaitForCharacterPacks", "WaitForTextures", "WaitForClientsReady", "WaitForCoopChecks", "WaitForUserInput",
    "Settle", "Release", "Done"};
static fn6 o_warp_step;
static u64 h_warp_step(u64 task, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
    static u32 last = ~0u;
    static u64 last_task;
    u64 r = o_warp_step(task, a1, a2, a3, a4, a5);
    u32 st = *(u32 *)(task + 0x208);
    if (st != last || task != last_task) {
        last = st, last_task = task;
        logf("warp %lx: state %u %s (busy %lu)", task, st, st < ARRAY_SIZE(k_warp_states) ? k_warp_states[st] : "?",
             r & 0xff);
    }
    return r;
}

static struct hook k_hooks[] = {
    {0xe5c310, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, 6, h_warp_step, &o_warp_step, "warp_step"},
    {0x391290, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, 6, h_heap_free, &o_heap_free, "heap_free"},
    {0xa2e200, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, 6, h_load_req, &o_load_req, "load_req"},
    {0x650d80, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, 6, h_stream_busy, &o_stream_busy, "stream_busy"},
    {0x88a8b0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, 6, h_movie_state, &o_movie_state, "movie_set_state"},
    {0x8858e0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, 6, h_movie_done, &o_movie_done, "movie_done"},
    {0xe593e0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, 6, h_boot_step, &o_boot_step, "boot_step"}, /* writable: holds pointers the core relocates */
    {0x390d80, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, 6, h_heap_alloc, &o_heap_alloc, "heap_alloc"},
};

void hooks_patch_text(struct module *m, u8 *text)
{
    if (!g_track)
        g_track = page_alloc(TRACK_SLOTS * sizeof(*g_track));
    for (u32 i = 0; i < ARRAY_SIZE(k_hooks); i++) {
        struct hook *h = &k_hooks[i];
        u8 *at = text + h->offset;
        if (h->offset + h->len > m->text_size || memcmp(at, h->prologue, h->len) != 0) {
            logf("hook %s: prologue mismatch at %lx", h->name, h->offset);
            continue;
        }
        u8 tramp[32];
        memcpy(tramp, h->prologue, h->len);
        u8 *j = tramp + h->len;
        j[0] = 0xFF, j[1] = 0x25; /* jmp [rip+0] */
        *(u32 *)(j + 2) = 0;
        *(u64 *)(j + 6) = m->base + h->offset + h->len;
        u64 t = stub_alloc_code(tramp, h->len + 14);
        s64 rel = (s64)(u64)h->hook - (s64)(m->base + h->offset + 5);
        if (!t || rel != (s32)rel) {
            logf("hook %s: trampoline %lx, hook %p, rel %lx: cannot patch", h->name, t, h->hook, (u64)rel);
            continue;
        }
        *h->orig = (fn6)t;
        at[0] = 0xE9;
        *(s32 *)(at + 1) = (s32)rel;
        for (u32 k = 5; k < h->len; k++)
            at[k] = 0xCC;
        logf("hook %s at eboot+%lx -> %p (trampoline %lx)", h->name, h->offset, h->hook, t);
    }
}
