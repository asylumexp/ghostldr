/* Overrides for system services SWRR already owns or that would act on
 * SWRR's identity (trophies, PlayGo) instead of the loaded title's. */
#include "ghost.h"

void *host_real_in(const char *modname, const char *name)
{
    u32 handle;
    if (host_module_handle(modname, &handle) != 0) {
        /* module vs. file name mismatches (libSceAppContentUtil) */
        if (strcmp(modname, "libSceAppContentUtil") != 0 || host_module_handle("libSceAppContent", &handle) != 0)
            return NULL;
    }
    void *fn = host_dlsym(handle, name);
    if (!fn)
        logf("host_real_in: %s:%s missing", modname, name);
    return fn;
}

typedef s64 (*fn6)(u64, u64, u64, u64, u64, u64);

/* Initialize-style calls: SWRR already initialized these services, so an
 * "already initialized" error means the service is usable. */
#define INIT_OK(name, mod)                                                       \
    static s64 hle_##name(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)              \
    {                                                                            \
        static fn6 real;                                                         \
        if (!real)                                                               \
            real = (fn6)host_real_in(mod, #name);                                \
        s32 r = real ? (s32)real(a, b, c, d, e, f) : -1;                         \
        if (r < 0) {                                                             \
            logf(#name " -> %x, treating as initialized", (u32)r);               \
            return 0;                                                            \
        }                                                                        \
        return r;                                                                \
    }

INIT_OK(sceUserServiceInitialize, "libSceUserService")
INIT_OK(scePadInit, "libScePad")
INIT_OK(sceAudioOutInit, "libSceAudioOut")
INIT_OK(sceAudioOutMasteringInit, "libSceAudioOut")
INIT_OK(sceSaveDataInitialize3, "libSceSaveData")
INIT_OK(sceCommonDialogInitialize, "libSceCommonDialog")
INIT_OK(sceMsgDialogInitialize, "libSceMsgDialog")
INIT_OK(sceErrorDialogInitialize, "libSceErrorDialog")
INIT_OK(sceNetCtlInit, "libSceNetCtl")
static s32 hle_sceAppContentInitialize(const void *a, void *b)
{
    return 0; /* SWRR initialized AppContent for itself */
}

static s32 hle_sceAppContentAppParamGetInt(u32 id, s32 *value)
{
    /* 0 = SKU flag: 3 = full game.  USER_DEFINED_PARAM_1 is the number the
     * title uses to find its package metadata, cache/t%05d.dat (content id,
     * versions, text/audio languages): CUSA11456 ships t11456.dat. */
    if (value)
        *value = id == 0 ? 3 : id == 1 ? 11456 : 0;
    logf("sceAppContentAppParamGetInt(%u) -> %d", id, value ? *value : -1);
    return 0;
}

static s32 hle_sceAppContentGetAddcontInfo(u32 label, const void *ent, void *info)
{
    return (s32)0x80D90005; /* SCE_APP_CONTENT_ERROR_NOT_FOUND */
}

static s32 hle_sceAppContentGetAddcontInfoList(u32 label, void *list, u32 n, u32 *hit)
{
    if (hit)
        *hit = 0;
    return 0;
}
INIT_OK(sceGameLiveStreamingInitialize, "libSceGameLiveStreaming")
INIT_OK(sceShareUtilityInitializeEx2, "libSceShareUtility")
INIT_OK(sceShareUtilityAdvanceInitialize, "libSceShareUtility")
INIT_OK(sceNpGameIntentInitialize, "libSceNpGameIntent")
INIT_OK(sceAudio3dInitialize, "libSceAudio3d")

static s32 hle_sceSaveDataTerminate(void)
{
    logf("sceSaveDataTerminate ignored (host keeps its save mount)");
    return 0;
}

/* ---- Pad: SWRR has the user's standard pad open ---- */
static s32 hle_scePadOpen(s32 user, s32 type, s32 index, const void *param)
{
    static s32 (*real)(s32, s32, s32, const void *);
    static s32 (*get)(s32, s32, s32);
    if (!real) {
        real = host_real_in("libScePad", "scePadOpen");
        get = host_real_in("libScePad", "scePadGetHandle");
    }
    s32 h = real(user, type, index, param);
    if (h < 0 && get) {
        s32 existing = get(user, type, index);
        logf("scePadOpen(%x,%d,%d) -> %x, reusing handle %x", user, type, index, (u32)h, (u32)existing);
        if (existing >= 0)
            return existing;
    }
    return h;
}

static s32 hle_scePadClose(s32 handle)
{
    logf("scePadClose(%x) ignored (shared with host)", handle);
    return 0;
}

/* ---- VideoOut: take the main output over from the host ---- */
static s32 hle_sceVideoOutOpen(s32 user, s32 bus, s32 index, const void *param)
{
    static s32 (*real)(s32, s32, s32, const void *);
    static s32 (*close)(s32);
    if (!real) {
        real = host_real_in("libSceVideoOut", "sceVideoOutOpen");
        close = host_real_in("libSceVideoOut", "sceVideoOutClose");
    }
    s32 h = real(user, bus, index, param);
    if (h >= 0) {
        logf("sceVideoOutOpen -> %d", h);
        return h;
    }
    /* SWRR still owns the main output.  Its handle lives at eboot+0x2d695d0
     * (SWRR 01.00/01.02, US and EU alike, written right after its sceVideoOutOpen). */
    static s32 (*unreg0)(s32, s32);
    if (!unreg0)
        unreg0 = host_real_in("libSceVideoOut", "sceVideoOutUnregisterBuffers");
    s32 swrr_handle = *(volatile s32 *)(g_ctx->eboot_base + 0x2d695d0);
    if (swrr_handle > 0) {
        s32 c = close(swrr_handle);
        s32 again = real(user, bus, index, param);
        logf("sceVideoOutOpen: SWRR handle %d close -> %x, reopen -> %x", swrr_handle, (u32)c, (u32)again);
        if (again >= 0)
            return again;
        for (s32 set = 0; set < 16 && unreg0; set++)
            unreg0(swrr_handle, set);
        logf("sceVideoOutOpen: sharing SWRR handle %d", swrr_handle);
        return swrr_handle;
    }
    static s32 (*res_status)(s32, void *);
    static s32 (*unreg)(s32, s32);
    if (!res_status) {
        res_status = host_real_in("libSceVideoOut", "sceVideoOutGetResolutionStatus");
        unreg = host_real_in("libSceVideoOut", "sceVideoOutUnregisterBuffers");
    }
    u8 status[0x40];
    for (s32 cand = 0; cand < 256 && res_status; cand++) {
        if (res_status(cand, status) != 0)
            continue;
        s32 c = close(cand);
        s32 again = real(user, bus, index, param);
        logf("sceVideoOutOpen: host handle %d (close %x) reopen -> %x", cand, (u32)c, (u32)again);
        if (again >= 0)
            return again;
        for (s32 set = 0; set < 16 && unreg; set++)
            unreg(cand, set);
        logf("sceVideoOutOpen: sharing host handle %d", cand);
        return cand;
    }
    logf("sceVideoOutOpen -> %x and no host handle found", (u32)h);
    return h;
}

/* ---- NpTrophy: never touch the host title's trophy set ---- */
static s32 hle_trophy_out1(s32 *out)
{
    if (out)
        *out = 1;
    return 0;
}

static s32 hle_sceNpTrophyCreateContext(s32 *ctx, s32 user, u32 label, u64 opt)
{
    return hle_trophy_out1(ctx);
}

static s32 hle_sceNpTrophyUnlockTrophy(s32 ctx, s32 handle, s32 id, s32 *platinum)
{
    logf("trophy unlock %d (suppressed)", id);
    if (platinum)
        *platinum = -1;
    return 0;
}

static s32 hle_sceNpTrophyGetTrophyUnlockState(s32 ctx, s32 handle, u32 *flags, u32 *count)
{
    if (flags)
        memset(flags, 0, 16);
    if (count)
        *count = 0;
    return 0;
}

static s32 hle_sceNpTrophyGetGameInfo(s32 ctx, s32 handle, void *details, void *data)
{
    if (details)
        memset((u8 *)details + 8, 0, 0x10);
    if (data)
        memset((u8 *)data + 8, 0, 0x10);
    return 0;
}

static s32 hle_ok(void) { return 0; }

/* ---- PlayGo: the title's content is fully local ---- */
static s32 hle_scePlayGoOpen(s32 *handle, const void *param)
{
    if (handle)
        *handle = 1;
    return 0;
}

static s32 hle_scePlayGoGetLocus(s32 h, const u16 *ids, u32 n, u8 *loci)
{
    for (u32 i = 0; loci && i < n; i++)
        loci[i] = 3; /* SCE_PLAYGO_LOCUS_LOCAL_FAST */
    return 0;
}

static s32 hle_scePlayGoGetChunkId(s32 h, u16 *ids, u32 n, u32 *out)
{
    if (ids && n)
        ids[0] = 0;
    if (out)
        *out = 1;
    return 0;
}

static s32 hle_scePlayGoGetEta(s32 h, const u16 *ids, u32 n, s64 *eta)
{
    if (eta)
        *eta = 0;
    return 0;
}

static s32 hle_scePlayGoGetProgress(s32 h, const u16 *ids, u32 n, u64 *prog)
{
    if (prog)
        prog[0] = prog[1] = 1;
    return 0;
}

static s32 hle_scePlayGoGetToDoList(s32 h, void *todo, u32 n, u32 *out)
{
    if (out)
        *out = 0;
    return 0;
}

struct hle_ent {
    const char *lib, *name;
    void *fn;
};

/* Error dialogs: the system draws them over the title's video layer but they
 * never get controller focus there, so the title would wait forever.  Log
 * the error and report the dialog as already dismissed. */
struct error_dialog_param {
    s32 size, error_code, user_id, reserved;
};

static s64 hle_sceErrorDialogOpen(const struct error_dialog_param *p)
{
    u64 ra = (u64)__builtin_return_address(0);
    struct module *m = g_main_mod;
    logf("sceErrorDialogOpen(error %x) suppressed (from eboot+%lx)", p ? (u32)p->error_code : 0,
         m && ra > m->base ? ra - m->base : ra);
    return 0;
}

static s64 hle_sceErrorDialogUpdateStatus(void)
{
    return 3; /* SCE_ERROR_DIALOG_STATUS_FINISHED */
}

/* Pass-through tracing: log the first calls and every negative result.
 * Forwards four stack arguments too (sceGnmSubmitAndFlipCommandBuffers has
 * nine parameters); reading unused caller stack slots is harmless. */
typedef s64 (*fn10)(u64, u64, u64, u64, u64, u64, u64, u64, u64, u64);
#define TRACE(name, mod, first)                                                                       \
    static s64 hle_##name(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h, u64 i, u64 j)       \
    {                                                                                                 \
        static fn10 real;                                                                             \
        static u32 calls, errors;                                                                     \
        if (!real)                                                                                    \
            real = (fn10)host_real_in(mod, #name);                                                    \
        s64 r = real ? real(a, b, c, d, e, f, g, h, i, j) : -1;                                       \
        u32 n = __atomic_fetch_add(&calls, 1, __ATOMIC_RELAXED);                                      \
        if (n < (first) || ((s32)r < 0 && __atomic_fetch_add(&errors, 1, __ATOMIC_RELAXED) < 6))      \
            logf(#name "#%u(%lx, %lx, %lx, %lx, %lx, %lx, %lx, %lx, %lx) -> %lx", n, a, b, c, d, e, f, g, \
                 h, i, (u64)r);                                                                       \
        return r;                                                                                     \
    }

u64 g_fb_addr[8];
u32 g_fb_count;

static s64 hle_sceVideoOutRegisterBuffers(u64 handle, u64 start, u64 addrs, u64 num, u64 attr, u64 f)
{
    static fn6 real;
    if (!real)
        real = (fn6)host_real_in("libSceVideoOut", "sceVideoOutRegisterBuffers");
    s64 r = real(handle, start, addrs, num, attr, f);
    const u64 *a = (const u64 *)addrs;
    for (u64 i = 0; i < num && start + i < ARRAY_SIZE(g_fb_addr); i++) {
        g_fb_addr[start + i] = a[i];
        if (start + i + 1 > g_fb_count)
            g_fb_count = (u32)(start + i + 1);
    }
    logf("sceVideoOutRegisterBuffers(%lx, start %lu, %lu buffers: %lx %lx) -> %lx", handle, start, num, a[0],
         num > 1 ? a[1] : 0, (u64)r);
    return r;
}
TRACE(sceVideoOutSetBufferAttribute, "libSceVideoOut", 8)
TRACE(sceVideoOutSetFlipRate, "libSceVideoOut", 8)
TRACE(sceVideoOutGetFlipStatus, "libSceVideoOut", 4)
TRACE(sceVideoOutIsFlipPending, "libSceVideoOut", 6)
TRACE(sceVideoOutConfigureOutputMode, "libSceVideoOut", 8)
TRACE(sceVideoOutModeSetAny, "libSceVideoOut", 8)
TRACE(sceVideoOutGetResolutionStatus, "libSceVideoOut", 4)
TRACE(sceGnmSubmitAndFlipCommandBuffers, "libSceGnmDriver", 6)
TRACE(sceGnmSubmitAndFlipCommandBuffersForWorkload, "libSceGnmDriver", 6)
TRACE(sceGnmSubmitCommandBuffers, "libSceGnmDriver", 6)
TRACE(sceGnmSubmitCommandBuffersForWorkload, "libSceGnmDriver", 6)
TRACE(sceGnmSubmitDone, "libSceGnmDriver", 4)
TRACE(sceGnmSetupMipStatsReport, "libSceGnmDriver", 6)
TRACE(sceGnmRequestMipStatsReportAndReset, "libSceGnmDriver", 6)
TRACE(sceGnmDisableMipStatsReport, "libSceGnmDriver", 4)
TRACE(sceGnmGetGpuInfoStatus, "libSceGnmDriver", 4)
TRACE(sceGnmGetShaderStatus, "libSceGnmDriver", 4)
TRACE(sceGnmDebugHardwareStatus, "libSceGnmDriver", 4)
TRACE(sceGnmIsUserPaEnabled, "libSceGnmDriver", 2)
TRACE(sceGnmGetGpuBlockStatus, "libSceGnmDriver", 4)
TRACE(sceSaveDataDialogInitialize, "libSceSaveDataDialog", 4)
TRACE(sceSaveDataDialogOpen, "libSceSaveDataDialog", 4)
TRACE(sceSaveDataDialogUpdateStatus, "libSceSaveDataDialog", 4)
TRACE(sceSaveDataDialogGetResult, "libSceSaveDataDialog", 4)
TRACE(sceMsgDialogOpen, "libSceMsgDialog", 4)
TRACE(sceMsgDialogUpdateStatus, "libSceMsgDialog", 4)
TRACE(sceMsgDialogGetResult, "libSceMsgDialog", 4)
TRACE(sceGnmMapComputeQueue, "libSceGnmDriver", 8)
TRACE(sceGnmMapComputeQueueWithPriority, "libSceGnmDriver", 8)
TRACE(sceGnmDingDong, "libSceGnmDriver", 4)
TRACE(sceGnmUnmapComputeQueue, "libSceGnmDriver", 4)
TRACE(sceGnmAreSubmitsAllowed, "libSceGnmDriver", 2)
TRACE(sceGnmCreateWorkloadStream, "libSceGnmDriver", 4)
TRACE(sceGnmBeginWorkload, "libSceGnmDriver", 3)
TRACE(sceGnmAddEqEvent, "libSceGnmDriver", 6)
TRACE(sceRtcGetCurrentNetworkTick, "libSceRtc", 3)
TRACE(sceSystemServiceReceiveEvent, "libSceSystemService", 3)
/* Inside SWRR the system reports the controller as intercepted (bit 31 of
 * ScePadData.buttons, the same leftover overlay as isSystemUiOverlaid); the
 * title treats that like an open system menu and pauses. */
#define PAD_INTERCEPTED 0x80000000u
#define PAD_DATA_SIZE 0x68

static s64 hle_scePadReadState(s32 handle, u8 *data)
{
    static int (*real)(s32, u8 *);
    static u32 logged;
    if (!real)
        real = (int (*)(s32, u8 *))host_real_in("libScePad", "scePadReadState");
    int rc = real(handle, data);
    if (rc == 0 && data && (*(u32 *)data & PAD_INTERCEPTED)) {
        if (logged++ == 0)
            logf("scePadReadState: clearing INTERCEPTED (buttons %08x, connected %u)", *(u32 *)data, data[0x4c]);
        *(u32 *)data &= ~PAD_INTERCEPTED;
    }
    static u32 last_buttons, presses;
    if (rc == 0 && data && *(u32 *)data != last_buttons) {
        last_buttons = *(u32 *)data;
        if (presses++ < 40)
            logf("scePadReadState: buttons %08x sticks %02x%02x %02x%02x", last_buttons, data[4], data[5], data[6],
                 data[7]);
    }
    return rc;
}

static s64 hle_scePadRead(s32 handle, u8 *data, s32 num)
{
    static int (*real)(s32, u8 *, s32);
    if (!real)
        real = (int (*)(s32, u8 *, s32))host_real_in("libScePad", "scePadRead");
    int rc = real(handle, data, num);
    for (int i = 0; data && i < rc; i++)
        *(u32 *)(data + i * PAD_DATA_SIZE) &= ~PAD_INTERCEPTED;
    return rc;
}
TRACE(sceNpCheckNpAvailabilityA, "libSceNpManager", 6)
TRACE(sceNpGetState, "libSceNpManager", 6)
TRACE(sceNpCheckCallback, "libSceNpManager", 2)
TRACE(sceVideodec2QueryComputeMemoryInfo, "libSceVideodec2", 8)
TRACE(sceVideodec2AllocateComputeQueue, "libSceVideodec2", 8)
/* Video decoder memory.  PS5's backward-compatible sceVideodec2 asks for
 * ~166 MiB per 1080p decoder (CPU 50, GPU 69, CPU/GPU 48 MiB); the title
 * carves decoder memory from fixed per-player pools sized for the PS4
 * decoder (9-30 MiB free each), so every movie failed before
 * sceVideodec2CreateDecoder.  The title never touches decoder memory itself:
 * report token sizes to it, and give the decoder buffers of the real sizes
 * allocated here (budget.c reserves MOVIE_RESERVE_MIB for them at boot). */
struct vdec_mem {
    u64 this_size, cpu_size, cpu, gpu_size, gpu, cpugpu_size, cpugpu, max_fb;
    u32 fb_align, reserved;
};
#define VDEC_TOKEN 0x10000
static struct {
    const struct vdec_mem *info; /* the title's struct the query filled */
    struct vdec_mem real;        /* sizes the decoder really needs */
    u64 handle;
    u64 buf[3], len[3], phys[3];
} g_vdec[4];

static int dm_map_typed(u64 len, int type, int prot, u64 *addr, u64 *phys)
{
    off_t p = 0;
    if (K.alloc_direct(0, (off_t)K.direct_size(), len, 0x200000, type, &p) != 0)
        return -1;
    void *a = NULL;
    if (K.map_direct(&a, len, prot, 0, p, 0x200000) != 0) {
        ((int (*)(off_t, size_t))host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelReleaseDirectMemory"))(p, len);
        return -1;
    }
    *addr = (u64)a, *phys = (u64)p;
    return 0;
}

static void dm_unmap_typed(u64 addr, u64 len, u64 phys)
{
    K.munmap((void *)addr, len);
    ((int (*)(off_t, size_t))host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelReleaseDirectMemory"))((off_t)phys, len);
}

/* Decoder buffers for g_vdec[i]: cpu (type 0), gpu (type 3), cpu-gpu (type 0).
 * They are released after each movie: the lazily backed pools (vdm.c) need
 * the memory more, and when a decoder cannot be created the title just skips
 * the movie (measured 2026-10-20: the boot splash went straight to "done"). */
static int vdec_mem_get(u32 i, const u64 want[3])
{
    static const int types[3] = {0, 3, 0}, prots[3] = {0x03, 0x33, 0x33};
    for (u32 k = 0; k < 3; k++) {
        g_vdec[i].len[k] = ALIGN_UP(want[k], 0x200000);
        if (!want[k] || dm_map_typed(g_vdec[i].len[k], types[k], prots[k], &g_vdec[i].buf[k], &g_vdec[i].phys[k])) {
            logf("sceVideodec2CreateDecoder: cannot allocate %lx bytes of decoder memory (kind %u): movie skipped",
                 want[k], k);
            for (u32 j = 0; j < k; j++)
                dm_unmap_typed(g_vdec[i].buf[j], g_vdec[i].len[j], g_vdec[i].phys[j]);
            return -1;
        }
    }
    return 0;
}

static void vdec_mem_put(u32 i)
{
    for (u32 k = 0; k < 3; k++)
        dm_unmap_typed(g_vdec[i].buf[k], g_vdec[i].len[k], g_vdec[i].phys[k]);
}

/* SceVideodec2DecoderConfigInfo: u64 thisSize, then u32 resourceType, codecType, profile, maxLevel,
 * maxFrameWidth, maxFrameHeight, maxDpbFrameCount, decodePipelineDepth.  The title leaves the DPB
 * size open (-1) with a pipeline depth of 4: the PS5 decoder then needs 166 MiB at 1080p.  H.264
 * level 4.2 allows 4 reference frames at 1080p, and a 2-deep pipeline still keeps a frame ahead
 * of display: ~95 MiB (measured: depth 1 82, 2 95, 4 122 MiB at 4 frames). */
#define VDEC_DPB_L42_1080P 4
#define VDEC_PIPELINE_DEPTH 2
static const u64 *vdec_config(const u64 *cfg, u64 *tmp, u32 cap)
{
    const u32 *c = (const u32 *)cfg;
    if (!cfg || cfg[0] > cap || cfg[0] < 0x28 || c[5] > 42 || (u64)c[6] * c[7] > 1920UL * 1088)
        return cfg;
    memcpy(tmp, cfg, cfg[0]);
    u32 *t = (u32 *)tmp;
    if (t[8] == 0xffffffffu || t[8] > VDEC_DPB_L42_1080P)
        t[8] = VDEC_DPB_L42_1080P;
    if (t[9] > VDEC_PIPELINE_DEPTH)
        t[9] = VDEC_PIPELINE_DEPTH;
    return tmp;
}

static s64 hle_sceVideodec2QueryDecoderMemoryInfo(const u64 *cfg, struct vdec_mem *info)
{
    static int (*real)(const u64 *, struct vdec_mem *);
    if (!real)
        real = (int (*)(const u64 *, struct vdec_mem *))host_real_in("libSceVideodec2",
                                                                     "sceVideodec2QueryDecoderMemoryInfo");
    u64 tmp[16];
    const u32 *c = (const u32 *)cfg;
    int rc = real(vdec_config(cfg, tmp, sizeof(tmp)), info);
    logf("sceVideodec2QueryDecoderMemoryInfo(level %u %ux%u dpb %d depth %u) -> %x: cpu %lx gpu %lx cpu-gpu %lx "
         "max frame buffer %lx", c[5], c[6], c[7], (s32)c[8], c[9], rc, info->cpu_size, info->gpu_size,
         info->cpugpu_size, info->max_fb);
    if (rc == 0) {
        u32 i = 0;
        while (i < ARRAY_SIZE(g_vdec) && g_vdec[i].info && g_vdec[i].info != info)
            i++;
        if (i < ARRAY_SIZE(g_vdec)) {
            g_vdec[i].info = info;
            g_vdec[i].real = *info;
            info->cpu_size = info->gpu_size = info->cpugpu_size = VDEC_TOKEN;
        }
    }
    return rc;
}

static s64 hle_sceVideodec2CreateDecoder(const u64 *cfg, const struct vdec_mem *info, u64 *handle)
{
    static int (*real)(const u64 *, const struct vdec_mem *, u64 *);
    if (!real)
        real = (int (*)(const u64 *, const struct vdec_mem *, u64 *))host_real_in("libSceVideodec2",
                                                                                 "sceVideodec2CreateDecoder");
    u32 i = 0;
    while (i < ARRAY_SIZE(g_vdec) && g_vdec[i].info != info)
        i++;
    if (i == ARRAY_SIZE(g_vdec))
        return real(cfg, info, handle);
    struct vdec_mem m = *info;
    const u64 want[3] = {g_vdec[i].real.cpu_size, g_vdec[i].real.gpu_size, g_vdec[i].real.cpugpu_size};
    if (vdec_mem_get(i, want) != 0) {
        g_vdec[i].info = NULL;
        return 0x80020023;
    }
    m.cpu_size = want[0], m.cpu = g_vdec[i].buf[0];
    m.gpu_size = want[1], m.gpu = g_vdec[i].buf[1];
    m.cpugpu_size = want[2], m.cpugpu = g_vdec[i].buf[2];
    u64 tmp[16];
    int rc = real(vdec_config(cfg, tmp, sizeof(tmp)), &m, handle);
    logf("sceVideodec2CreateDecoder with own memory (cpu %lx gpu %lx cpu-gpu %lx) -> %x handle %lx", m.cpu, m.gpu,
         m.cpugpu, rc, handle ? *handle : 0);
    if (rc == 0 && handle) {
        g_vdec[i].handle = *handle;
    } else {
        vdec_mem_put(i);
        g_vdec[i].info = NULL;
    }
    return rc;
}

static s64 hle_sceVideodec2DeleteDecoder(u64 handle)
{
    static int (*real)(u64);
    if (!real)
        real = (int (*)(u64))host_real_in("libSceVideodec2", "sceVideodec2DeleteDecoder");
    int rc = real(handle);
    for (u32 i = 0; i < ARRAY_SIZE(g_vdec); i++)
        if (g_vdec[i].info && g_vdec[i].handle == handle) {
            vdec_mem_put(i);
            g_vdec[i].info = NULL, g_vdec[i].handle = 0;
            logf("sceVideodec2DeleteDecoder(%lx) -> %x, decoder memory released", handle, rc);
        }
    return rc;
}

TRACE(sceVideodec2Decode, "libSceVideodec2", 8)
TRACE(sceVideodec2Flush, "libSceVideodec2", 8)
TRACE(sceVideodec2Reset, "libSceVideodec2", 8)

/* SWRR consumed the initial user's LOGIN event at its own start; the title
 * waits for one before its boot flow continues, so deliver it once. */
struct user_event {
    s32 type, user_id;
};

static s64 hle_sceUserServiceGetEvent(struct user_event *ev)
{
    static int (*real)(struct user_event *);
    static int (*initial)(s32 *);
    static bool sent;
    if (!real) {
        real = (int (*)(struct user_event *))host_real_in("libSceUserService", "sceUserServiceGetEvent");
        initial = (int (*)(s32 *))host_real_in("libSceUserService", "sceUserServiceGetInitialUser");
    }
    if (!sent && ev && initial) {
        s32 uid = -1;
        int rc = initial(&uid);
        sent = true;
        ev->type = 0; /* SCE_USER_SERVICE_EVENT_TYPE_LOGIN */
        ev->user_id = uid;
        logf("sceUserServiceGetEvent: synthesized LOGIN for initial user %x (rc %x)", (u32)uid, rc);
        return 0;
    }
    int rc = real ? real(ev) : (s32)0x80960007;
    static u32 logged;
    if (rc != (s32)0x80960007 && logged++ < 8)
        logf("sceUserServiceGetEvent -> %x (type %d user %x)", rc, ev ? ev->type : -1, ev ? (u32)ev->user_id : 0);
    return rc;
}

static s64 hle_sceSystemServiceGetStatus(u8 *st)
{
    static int (*real)(u8 *);
    static u8 last[8];
    if (!real)
        real = (int (*)(u8 *))host_real_in("libSceSystemService", "sceSystemServiceGetStatus");
    int rc = real ? real(st) : -1;
    /* SWRR's process still counts as overlaid by system UI (left over from
     * the exploit's loader dialog); titles pause while that is set */
    if (rc == 0)
        st[4] = 0;
    if (rc == 0 && memcmp(last, st, 8) != 0) {
        memcpy(last, st, 8);
        logf("sceSystemServiceGetStatus: events %d uiOverlaid %u background %u cpu7 %u streaming %u vr %u",
             *(s32 *)st, st[4], st[5], st[6], st[7], st[8]);
    }
    return rc;
}

static s64 hle_sceSystemServiceParamGetInt(s32 id, s32 *val)
{
    static int (*real)(s32, s32 *);
    static u32 logged;
    if (!real)
        real = (int (*)(s32, s32 *))host_real_in("libSceSystemService", "sceSystemServiceParamGetInt");
    int rc = real(id, val);
    if (logged++ < 24)
        logf("sceSystemServiceParamGetInt(%d) -> %x value %d", id, rc, val ? *val : -1);
    return rc;
}

#define E(lib, fn) {lib, #fn, hle_##fn}
s64 hle_sdm_setup2(), hle_sdm_get2(), hle_sdm_set2(), hle_sdm_ok(), hle_save_mount2(), hle_save_umount(),
    hle_save_set_param(), hle_save_dir_search(), hle_save_not_found();
static const struct hle_ent g_sys[] = {
    E("libSceUserService", sceUserServiceInitialize),
    E("libSceUserService", sceUserServiceGetEvent),
    E("libSceSystemService", sceSystemServiceParamGetInt),
    E("libSceSystemService", sceSystemServiceGetStatus),
    E("libScePad", scePadInit),
    E("libScePad", scePadOpen),
    E("libScePad", scePadClose),
    E("libSceAudioOut", sceAudioOutInit),
    E("libSceAudioOut", sceAudioOutMasteringInit),
    E("libSceSaveData", sceSaveDataInitialize3),
    E("libSceSaveData", sceSaveDataTerminate),
    /* the title's saves live in their own files (savedata.c), never in SWRR's */
    {"libSceSaveData", "sceSaveDataSetupSaveDataMemory2", hle_sdm_setup2},
    {"libSceSaveData", "sceSaveDataGetSaveDataMemory2", hle_sdm_get2},
    {"libSceSaveData", "sceSaveDataSetSaveDataMemory2", hle_sdm_set2},
    {"libSceSaveData", "sceSaveDataSyncSaveDataMemory", hle_sdm_ok},
    {"libSceSaveData", "sceSaveDataMount2", hle_save_mount2},
    {"libSceSaveData", "sceSaveDataUmount", hle_save_umount},
    {"libSceSaveData", "sceSaveDataSetParam", hle_save_set_param},
    {"libSceSaveData", "sceSaveDataSaveIcon", hle_sdm_ok},
    {"libSceSaveData", "sceSaveDataDirNameSearch", hle_save_dir_search},
    {"libSceSaveData", "sceSaveDataDelete", hle_sdm_ok},
    {"libSceSaveData", "sceSaveDataTransferringMount", hle_save_not_found},
    E("libSceCommonDialog", sceCommonDialogInitialize),
    E("libSceMsgDialog", sceMsgDialogInitialize),
    E("libSceErrorDialog", sceErrorDialogInitialize),
    E("libSceErrorDialog", sceErrorDialogOpen),
    E("libSceErrorDialog", sceErrorDialogUpdateStatus),
    {"libSceErrorDialog", "sceErrorDialogGetStatus", hle_sceErrorDialogUpdateStatus},
    {"libSceErrorDialog", "sceErrorDialogClose", hle_ok},
    {"libSceErrorDialog", "sceErrorDialogTerminate", hle_ok},
    E("libSceNetCtl", sceNetCtlInit),
    E("libSceAppContent", sceAppContentInitialize),
    E("libSceAppContent", sceAppContentAppParamGetInt),
    E("libSceAppContent", sceAppContentGetAddcontInfo),
    E("libSceAppContent", sceAppContentGetAddcontInfoList),
    E("libSceGameLiveStreaming", sceGameLiveStreamingInitialize),
    E("libSceShareUtility", sceShareUtilityInitializeEx2),
    E("libSceShareUtility", sceShareUtilityAdvanceInitialize),
    E("libSceNpGameIntent", sceNpGameIntentInitialize),
    E("libSceAudio3d", sceAudio3dInitialize),
    E("libSceVideoOut", sceVideoOutOpen),
    E("libSceVideoOut", sceVideoOutRegisterBuffers),
    E("libSceVideoOut", sceVideoOutSetBufferAttribute),
    E("libSceVideoOut", sceVideoOutSetFlipRate),
    E("libSceVideoOut", sceVideoOutGetFlipStatus),
    E("libSceVideoOut", sceVideoOutIsFlipPending),
    E("libSceVideoOut", sceVideoOutConfigureOutputMode),
    E("libSceVideoOut", sceVideoOutModeSetAny),
    E("libSceVideoOut", sceVideoOutGetResolutionStatus),
    E("libSceGnmDriver", sceGnmSubmitAndFlipCommandBuffers),
    E("libSceGnmDriver", sceGnmSubmitAndFlipCommandBuffersForWorkload),
    E("libSceGnmDriver", sceGnmSubmitCommandBuffers),
    E("libSceGnmDriver", sceGnmSubmitCommandBuffersForWorkload),
    E("libSceGnmDriver", sceGnmSubmitDone),
    E("libSceGnmDriver", sceGnmSetupMipStatsReport),
    E("libSceGnmDriver", sceGnmRequestMipStatsReportAndReset),
    E("libSceGnmDriver", sceGnmDisableMipStatsReport),
    E("libSceGnmDriver", sceGnmGetGpuInfoStatus),
    E("libSceGnmDriver", sceGnmGetShaderStatus),
    E("libSceGnmDriver", sceGnmDebugHardwareStatus),
    E("libSceGnmDriver", sceGnmIsUserPaEnabled),
    E("libSceGnmDriver", sceGnmGetGpuBlockStatus),
    E("libSceSaveDataDialog", sceSaveDataDialogInitialize),
    E("libSceSaveDataDialog", sceSaveDataDialogOpen),
    E("libSceSaveDataDialog", sceSaveDataDialogUpdateStatus),
    E("libSceSaveDataDialog", sceSaveDataDialogGetResult),
    E("libSceMsgDialog", sceMsgDialogOpen),
    E("libSceMsgDialog", sceMsgDialogUpdateStatus),
    E("libSceMsgDialog", sceMsgDialogGetResult),
    E("libSceGnmDriver", sceGnmMapComputeQueue),
    E("libSceGnmDriver", sceGnmMapComputeQueueWithPriority),
    E("libSceGnmDriver", sceGnmDingDong),
    E("libSceGnmDriver", sceGnmUnmapComputeQueue),
    E("libSceGnmDriver", sceGnmAreSubmitsAllowed),
    E("libSceGnmDriver", sceGnmCreateWorkloadStream),
    E("libSceGnmDriver", sceGnmBeginWorkload),
    E("libSceGnmDriver", sceGnmAddEqEvent),
    E("libSceRtc", sceRtcGetCurrentNetworkTick),
    E("libSceSystemService", sceSystemServiceReceiveEvent),
    E("libScePad", scePadReadState),
    E("libScePad", scePadRead),
    E("libSceNpManager", sceNpCheckNpAvailabilityA),
    E("libSceNpManager", sceNpGetState),
    E("libSceNpManager", sceNpCheckCallback),
    E("libSceVideodec2", sceVideodec2QueryComputeMemoryInfo),
    E("libSceVideodec2", sceVideodec2AllocateComputeQueue),
    E("libSceVideodec2", sceVideodec2QueryDecoderMemoryInfo),
    E("libSceVideodec2", sceVideodec2CreateDecoder),
    E("libSceVideodec2", sceVideodec2Decode),
    E("libSceVideodec2", sceVideodec2DeleteDecoder),
    E("libSceVideodec2", sceVideodec2Flush),
    E("libSceVideodec2", sceVideodec2Reset),
    E("libSceNpTrophy", sceNpTrophyCreateContext),
    {"libSceNpTrophy", "sceNpTrophyCreateHandle", hle_trophy_out1},
    {"libSceNpTrophy", "sceNpTrophyRegisterContext", hle_ok},
    {"libSceNpTrophy", "sceNpTrophyDestroyContext", hle_ok},
    {"libSceNpTrophy", "sceNpTrophyDestroyHandle", hle_ok},
    {"libSceNpTrophy", "sceNpTrophyGetTrophyInfo", hle_ok},
    E("libSceNpTrophy", sceNpTrophyUnlockTrophy),
    E("libSceNpTrophy", sceNpTrophyGetTrophyUnlockState),
    E("libSceNpTrophy", sceNpTrophyGetGameInfo),
    {"libScePlayGo", "scePlayGoInitialize", hle_ok},
    E("libScePlayGo", scePlayGoOpen),
    E("libScePlayGo", scePlayGoGetLocus),
    E("libScePlayGo", scePlayGoGetChunkId),
    E("libScePlayGo", scePlayGoGetEta),
    E("libScePlayGo", scePlayGoGetProgress),
    E("libScePlayGo", scePlayGoGetToDoList),
    {"libScePlayGo", "scePlayGoSetToDoList", hle_ok},
    {"libScePlayGo", "scePlayGoSetLanguageMask", hle_ok},
};

u64 hle_sys_lookup(const char *lib, const char *name)
{
    for (u32 i = 0; i < ARRAY_SIZE(g_sys); i++)
        if (strcmp(g_sys[i].name, name) == 0 && (!lib || strcmp(g_sys[i].lib, lib) == 0))
            return (u64)g_sys[i].fn;
    return 0;
}
