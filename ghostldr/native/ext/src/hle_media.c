/*
 * Capture, sharing and voice libraries (research spec "media-peripherals",
 * §4-§15): GameLiveStreaming, ScreenShot, ShareUtility, SharePlay,
 * Remoteplay, VideoRecording, CompanionHttpd/Util, ContentExport, Voice,
 * VoiceQoS, Audio3d, Rudp, RazorCpu.
 *
 * SWRR already runs several of these for itself (GameLiveStreaming,
 * SharePlay, Remoteplay, VideoRecording, CompanionHttpd): the title's init
 * of them is masked and its Terminate never reaches SWRR's instance.  The
 * share menu refuses SWRR's process (no share flag in its param.sfo), so
 * ShareUtility ends up virtual.  Modes and routing: hle_mode.h.
 */
#include "hle_mode.h"

enum { K_GLS, K_SS, K_SU, K_SP, K_RP, K_VR, K_CH, K_CU, K_CE, K_VO, K_VQ, K_A3, K_RU, K_RZ, NLIBS };
#define N_GLS "libSceGameLiveStreaming"
#define N_SS "libSceScreenShot"
#define N_SU "libSceShareUtility"
#define N_SP "libSceSharePlay"
#define N_RP "libSceRemoteplay"
#define N_VR "libSceVideoRecording"
#define N_CH "libSceCompanionHttpd"
#define N_CU "libSceCompanionUtil"
#define N_CE "libSceContentExport"
#define N_VO "libSceVoice"
#define N_VQ "libSceVoiceQoS"
#define N_A3 "libSceAudio3d"
#define N_RU "libSceRudp"
#define N_RZ "libSceRazorCpu"

/* policies: aliases, and what an import the extension lacks returns when
 * the host lacks it too (instead of 0, which looks like success) */
static const struct ext_lib k_pol[NLIBS] = {
    [K_GLS] = {N_GLS, "libSceGameLiveStreamingCompat libSceGameLiveStreaming_debug libSceGameLiveStreaming_direct_streaming",
               0, (s32)0x80A00008}, /* NOT_SUPPORTED */
    [K_SS] = {N_SS, "libSceScreenShotDrc", 0, (s32)0x80BE0141},     /* NOT_SUPPORTED_PLATFORM */
    [K_SU] = {N_SU, NULL, 0, (s32)0x8117000B},                      /* EXECUTE_FUNCTION */
    [K_SP] = {N_SP, "libSceSharePlayCompat", 0, (s32)0x810E0109},   /* INVALID_STATE */
    [K_RP] = {N_RP, NULL, 0, (s32)0x80FC0102},                      /* NOT_CONNECTED */
    [K_VR] = {N_VR, NULL, 0, (s32)0x80A80008},                      /* UNSUPPORTED */
    /* SWRR's own companion server: the title never reaches it */
    [K_CH] = {N_CH, NULL, EXT_LIB_NO_HOST, (s32)0x80E40005},        /* INVALID_OPERATION */
    [K_CU] = {N_CU, NULL, EXT_LIB_NO_HOST, (s32)0x80AD0005},        /* INVALID_OPERATION */
    [K_CE] = {N_CE, NULL, 0, (s32)0x809D3002},                      /* NOTACCEPT */
    [K_VO] = {N_VO, NULL, 0, (s32)0x804E0803},                      /* GENERAL */
    [K_VQ] = {N_VQ, NULL, 0, (s32)0x804E0902},                      /* ARGUMENT_INVALID */
    [K_A3] = {N_A3, NULL, 0, (s32)0x80EA0001},                      /* UNKNOWN */
    [K_RU] = {N_RU, NULL, 0, (s32)0x80770003},                      /* INVALID_CONTEXT_ID */
    [K_RZ] = {N_RZ, NULL, EXT_LIB_NO_HOST, 0},                      /* profiler markers: never loaded */
};

#define SET(...) ((const s32[]){__VA_ARGS__, 0})
static struct hle_lib g_libs[NLIBS] = {
    [K_GLS] = {.pol = &k_pol[K_GLS], .sysmod = {0x0081}, .flags = HL_MASK_ANY,
               .already = SET((s32)0x80A00003),                       /* INVALID_STATUS: SWRR's instance */
               .deny = SET((s32)0x80A00008, (s32)0x80A0000C)},        /* NOT_SUPPORTED, PARAMSFO_NOT_AUTHORIZED */
    [K_SS] = {.pol = &k_pol[K_SS], .sysmod = {0x009C}, .deny = SET((s32)0x80BE0141)},
    [K_SU] = {.pol = &k_pol[K_SU], .sysmod = {0x00C8},
              .already = SET((s32)0x81170101),                        /* ALREADY_INITILIZED */
              .deny = SET((s32)0x81170105, (s32)0x81170103, (s32)0x81170104)}, /* no share flag in param.sfo, ... */
    [K_SP] = {.pol = &k_pol[K_SP], .sysmod = {0x00D3}, .flags = HL_MASK_ANY, .already = SET((s32)0x810E0003)},
    [K_RP] = {.pol = &k_pol[K_RP], .sysmod = {0x00B6}, .flags = HL_MASK_ANY, .already = SET((s32)0x80FC0003)},
    [K_VR] = {.pol = &k_pol[K_VR], .sysmod = {0x0085},
              .deny = SET((s32)0x80A80006, (s32)0x80A80008)},         /* INVALID_STATE (SWRR's session), UNSUPPORTED */
    [K_CH] = {.pol = &k_pol[K_CH], .sysmod = {0x00AA}, .init_mode = HM_VIRT,
              .why = "SWRR's server; no Second Screen device connects to a PS5"},
    [K_CU] = {.pol = &k_pol[K_CU], .sysmod = {0x0082}, .init_mode = HM_VIRT,
              .why = "no Second Screen device connects to a PS5"},
    [K_CE] = {.pol = &k_pol[K_CE], .sysmod = {0x00A6}, .already = SET((s32)0x809D3005)}, /* MULTIPLEINIT */
    [K_VO] = {.pol = &k_pol[K_VO], .sysmod = {0x001A}, .init_mode = HLE_VOICE_PASS ? HM_UNDECIDED : HM_VIRT,
              .why = "voice chat needs PSN (HLE_VOICE_PASS=1 lets the host run it)"},
    [K_VQ] = {.pol = &k_pol[K_VQ], .sysmod = {0x001B}, .already = SET((s32)0x804E0905),
              .deny = SET((s32)0x804E0906)},                          /* LIBVOICEQOS_SYSTEM */
    [K_A3] = {.pol = &k_pol[K_A3], .sysmod = {0x00A7}, .already = SET((s32)0x80EA0007)}, /* NOT_READY: initialised */
    [K_RU] = {.pol = &k_pol[K_RU], .sysmod = {0x0021}, .already = SET((s32)0x80770002)},
    [K_RZ] = {.pol = &k_pol[K_RZ], .init_mode = HM_VIRT, .why = "profiler markers: no-ops"},
};
#define LIB(k) (&g_libs[K_##k])
#define TAG "hle_media"

static s64 v_ok(void) { return 0; }
HLE_RET_FN(r_0, 0)

/* ---- GameLiveStreaming (§4): masked; SWRR initialised it ---- */

struct gls_status {
    u8 on_air, pad[3];
    u32 spectators;
    s32 user_id;
    u8 reserved[60];
};
struct gls_status2 {
    s32 user_id;
    u8 on_air, pad[3];
    u32 spectators, text_messages, command_messages, resolution;
    u8 reserved[48];
};
_Static_assert(sizeof(struct gls_status) == 0x48 && sizeof(struct gls_status2) == 0x48, "GLS status sizes");

static s32 v_gls_status(struct gls_status *s)
{
    if (!s)
        return (s32)0x80A00002; /* INVALID_PARAM */
    memset(s, 0, sizeof(*s));
    s->user_id = -1; /* not on air */
    return 0;
}

static s32 v_gls_status2(struct gls_status2 *s)
{
    if (!s)
        return (s32)0x80A00002;
    memset(s, 0, sizeof(*s));
    s->user_id = -1;
    return 0;
}

HLE_ROOT(sceGameLiveStreamingInitialize, LIB(GLS), v_ok)

/* a path a system process opens (standby screen, screenshot overlay): /app0
 * remapped; under ghostfs no system process can read it, so the call is not
 * made and the setter answers 0 */
static s64 path_setter(struct hle_lib *L, const char *name, void **cache, u64 a[6])
{
    char buf[512];
    const char *p = hle_sys_path((const char *)a[0], buf, sizeof(buf));
    if (a[0] && !p) {
        logf(TAG ": %s(%s): in ghostfs, which no system process can read; answered 0", name, (const char *)a[0]);
        return 0;
    }
    a[0] = (u64)p;
    return hle_setter(L, name, cache, a, 0);
}

#define PATH_SETTER(fn, lib)                                                                      \
    static s64 w_##fn(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)                                  \
    {                                                                                             \
        static void *cache;                                                                       \
        u64 v[6] = {a, b, c, d, e, f};                                                            \
        return path_setter(lib, #fn, &cache, v);                                                  \
    }
PATH_SETTER(sceGameLiveStreamingSetStandbyScreenResource, LIB(GLS))

/* ---- ScreenShot (§5): the host; NOT_SUPPORTED_PLATFORM makes a setter a no-op ---- */

PATH_SETTER(sceScreenShotSetOverlayImage, LIB(SS))
PATH_SETTER(sceScreenShotSetOverlayImageWithOrigin, LIB(SS))
HLE_SETTER(sceScreenShotEnable, LIB(SS), 0)
HLE_SETTER(sceScreenShotDisable, LIB(SS), 0)
HLE_SETTER(sceScreenShotEnableNotification, LIB(SS), 0)
HLE_SETTER(sceScreenShotDisableNotification, LIB(SS), 0)
HLE_SETTER(sceScreenShotSetParam, LIB(SS), 0)
HLE_SETTER(sceScreenShotSetDrcParam, LIB(SS), 0)

/* ---- ShareUtility (§6): refused by SWRR's param.sfo, so virtual ---- */

HLE_ROOT(sceShareUtilityInitializeEx2, LIB(SU), v_ok)
HLE_ROOT(sceShareUtilityAdvanceInitialize, LIB(SU), v_ok)
HLE_ROOT(sceShareUtilityInitialize, LIB(SU), v_ok)

/* ---- SharePlay (§7): masked ---- */

struct sp_connection {
    s32 status, mode; /* 0 DORMANT */
    u8 host_online_id[20], visitor_online_id[20];
    s32 host_user_id, visitor_user_id;
};
_Static_assert(sizeof(struct sp_connection) == 0x38, "SceSharePlayConnectionInfo");

static s32 v_sp_connection(struct sp_connection *c)
{
    if (!c)
        return (s32)0x810E0001; /* INVALID_ARGS */
    memset(c, 0, sizeof(*c));
    return 0;
}

HLE_ROOT(sceSharePlayInitialize, LIB(SP), v_ok)

/* ---- Remoteplay (§8): masked ---- */

static s32 v_rp_status(s32 user_id, s32 *status)
{
    if (!status)
        return (s32)0x80FC0001; /* INVALID_ARGS */
    *status = 0;                /* DISCONNECT */
    return 0;
}

HLE_ROOT(sceRemoteplayInitialize, LIB(RP), v_ok)

/* ---- VideoRecording (§9): the host only for the title's own session ---- */

static volatile u32 g_vr_state; /* virtual session: 0 none, 1 open, 2 running */

static s64 vr_open(const char *name, void **cache, u64 a, u64 b, u64 c, u64 d)
{
    const u64 v[6] = {a, b, c, d};
    s32 rc;
    struct hle_lib *L = LIB(VR);
    if (hle_root(L, name, cache, v, &rc)) {
        if (rc == 0)
            L->owned = 1;
        return rc;
    }
    g_vr_state = 1;
    return 0;
}

static s64 w_vr_open(u64 a, u64 b, u64 c, u64 d)
{
    static void *cache;
    return vr_open("sceVideoRecordingOpen", &cache, a, b, c, d);
}

static s64 w_vr_open2(u64 a, u64 b, u64 c, u64 d)
{
    static void *cache;
    return vr_open("sceVideoRecordingOpen2", &cache, a, b, c, d);
}

static s64 w_vr_close(u64 discard)
{
    static void *cache;
    struct hle_lib *L = LIB(VR);
    void *fn = L->mode != HM_VIRT && L->owned ? hle_host(L, "sceVideoRecordingClose", &cache) : NULL;
    if (fn) {
        s32 r = ((s32(*)(u64))fn)(discard);
        if (r == 0)
            L->owned = 0;
        return r;
    }
    g_vr_state = 0;
    return 0;
}

static s32 v_vr_start(void)
{
    g_vr_state = 2;
    return 0;
}

static s32 v_vr_stop(void)
{
    if (g_vr_state == 2)
        g_vr_state = 1;
    return 0;
}

static s32 v_vr_status(void) { return g_vr_state == 2 ? 1 : 0; /* RUNNING : NONE */ }

static s32 v_vr_set_info(s32 info, const void *val, size_t len)
{
    static const s32 ids[] = {0x2, 0x6, 0x7, 0x8, 0xD, 0xA01, 0xA007, 0xA008, 0xA009, 0};
    return val && hle_in(ids, info) ? 0 : (s32)0x80A80003; /* INVALID_VALUE */
}

/* ---- CompanionHttpd / CompanionUtil (§10): virtual ---- */

HLE_RET_FN(r_httpd_no_event, 0x80E40008)
HLE_RET_FN(r_httpd_no_user, 0x80E4000E)
HLE_RET_FN(r_util_no_event, 0x80AD0008)

/* ---- ContentExport (§15) ---- */

HLE_ROOT(sceContentExportInit, LIB(CE), v_ok)
HLE_ROOT(sceContentExportInit2, LIB(CE), v_ok)

/* ---- Voice (§11): virtual ports ---- */

enum { VP_IN_DEVICE, VP_IN_PCM, VP_IN_VOICE, VP_OUT_PCM, VP_OUT_VOICE, VP_OUT_DEVICE };
#define VOICE_PORTS 64
#define E_VOICE(n) ((s32)(0x804E0800 + (n)))
struct voice_port {
    u8 used, type, muted;
    float volume;
    u32 bitrate;
};
static struct voice_port g_vport[VOICE_PORTS];
static volatile int g_voice_lock;
static volatile u8 g_voice_init;

static struct voice_port *vport(u32 id) { return id < VOICE_PORTS && g_vport[id].used ? &g_vport[id] : NULL; }

static s32 v_voice_init(const void *param, s32 version)
{
    if (!param)
        return E_VOICE(5); /* ARGUMENT_INVALID */
    if (!__sync_bool_compare_and_swap(&g_voice_init, 0, 1))
        return E_VOICE(2); /* LIBVOICE_INITIALIZED */
    return 0;
}

static s32 v_voice_end(void)
{
    if (!g_voice_init)
        return E_VOICE(1); /* LIBVOICE_NOT_INIT */
    spin_lock(&g_voice_lock);
    memset(g_vport, 0, sizeof(g_vport));
    g_voice_init = 0;
    spin_unlock(&g_voice_lock);
    return 0;
}

static s32 v_voice_create_port(s32 *port_id, const u8 *param)
{
    if (!g_voice_init)
        return E_VOICE(1);
    if (!port_id || !param || *(const u32 *)param > VP_OUT_DEVICE)
        return E_VOICE(5);
    s32 rc = E_VOICE(8); /* RESOURCE_INSUFFICIENT */
    spin_lock(&g_voice_lock);
    for (u32 i = 0; i < VOICE_PORTS; i++) {
        if (g_vport[i].used)
            continue;
        u8 type = (u8) * (const u32 *)param;
        g_vport[i] = (struct voice_port){1, type, 0, 1.0f, 0};
        if (type == VP_IN_VOICE || type == VP_OUT_VOICE)
            g_vport[i].bitrate = *(const u32 *)(param + 0x0C);
        *port_id = (s32)i;
        rc = 0;
        break;
    }
    spin_unlock(&g_voice_lock);
    return rc;
}

static s32 v_voice_delete_port(u32 id)
{
    struct voice_port *p = vport(id);
    if (!p)
        return E_VOICE(4); /* PORT_INVALID */
    p->used = 0;
    return 0;
}

static s32 v_voice_port_ok(u32 id) { return vport(id) ? 0 : E_VOICE(4); }

static s32 v_voice_connect(u32 in, u32 out)
{
    struct voice_port *i = vport(in), *o = vport(out);
    if (!i || !o)
        return E_VOICE(4);
    return i->type <= VP_IN_VOICE && o->type >= VP_OUT_PCM ? 0 : E_VOICE(7); /* TOPOLOGY */
}

static s32 v_voice_read(u32 id, void *data, u32 *size)
{
    struct voice_port *p = vport(id);
    if (!p)
        return E_VOICE(4);
    if (!size || p->type == VP_OUT_DEVICE)
        return E_VOICE(5);
    if (p->type <= VP_IN_VOICE)
        return E_VOICE(7);
    *size = 0; /* nobody talks */
    return 0;
}

static s32 v_voice_write(u32 id, const void *data, u32 *size, s16 frame_gaps)
{
    return vport(id) ? 0 : E_VOICE(4); /* all consumed: *size unchanged */
}

struct voice_port_info {
    s32 type, state;
    u32 *edge;
    u32 byte_count, frame_size;
    u16 edge_count, reserved;
};
_Static_assert(sizeof(struct voice_port_info) == 0x20, "SceVoicePortInfo");

static s32 v_voice_port_info(u32 id, struct voice_port_info *info)
{
    struct voice_port *p = vport(id);
    if (!p)
        return E_VOICE(4);
    if (!info)
        return E_VOICE(5);
    memset(info, 0, sizeof(*info));
    info->type = p->type;
    info->frame_size = 1;
    return 0;
}

static s32 v_voice_port_attr(u32 id, s32 attr, void *v, u32 size)
{
    if (!vport(id))
        return E_VOICE(4);
    u32 need = attr == 1000 || attr == 1001 || attr == 2000 || attr == 2002 ? 1
               : attr == 1002 || attr == 2001                               ? 4
               : attr == 1003                                               ? 2
                                                                            : 0;
    if (!need || !v || size < need)
        return E_VOICE(5);
    memset(v, 0, need);
    if (attr == 1002)
        *(u32 *)v = 1; /* silent: no device */
    return 0;
}

static s32 v_voice_set_volume(u32 id, float volume)
{
    struct voice_port *p = vport(id);
    if (!p)
        return E_VOICE(4);
    p->volume = volume;
    return 0;
}

static s32 v_voice_get_volume(u32 id, float *volume)
{
    struct voice_port *p = vport(id);
    if (!p)
        return E_VOICE(4);
    if (!volume)
        return E_VOICE(5);
    *volume = p->volume;
    return 0;
}

static s32 v_voice_set_mute(u32 id, bool mute)
{
    struct voice_port *p = vport(id);
    if (!p)
        return E_VOICE(4);
    p->muted = mute;
    return 0;
}

static s32 v_voice_get_mute(u32 id, bool *mute)
{
    struct voice_port *p = vport(id);
    if (!p)
        return E_VOICE(4);
    if (!mute)
        return E_VOICE(5);
    *mute = p->muted;
    return 0;
}

static s32 v_voice_mute_all(bool mute)
{
    for (u32 i = 0; i < VOICE_PORTS; i++)
        g_vport[i].muted = mute;
    return 0;
}

static s32 v_voice_set_bitrate(u32 id, u32 bitrate)
{
    struct voice_port *p = vport(id);
    if (!p)
        return E_VOICE(4);
    p->bitrate = bitrate;
    return 0;
}

static s32 v_voice_get_bitrate(u32 id, u32 *bitrate)
{
    struct voice_port *p = vport(id);
    if (!p)
        return E_VOICE(4);
    if (!bitrate)
        return E_VOICE(5);
    *bitrate = p->bitrate;
    return 0;
}

static s32 v_voice_resource_info(void *info)
{
    if (!info)
        return E_VOICE(5);
    memset(info, 0, 8);
    return 0;
}

HLE_ROOT(sceVoiceInit, LIB(VO), v_voice_init)
HLE_ROOT(sceVoiceInitHQ, LIB(VO), v_voice_init)

/* ---- VoiceQoS (§12): ids only, no audio ---- */

static volatile s32 g_qos_next;

static s32 v_qos_new_id(s32 *id)
{
    if (!id)
        return (s32)0x804E0902; /* ARGUMENT_INVALID */
    *id = __sync_add_and_fetch(&g_qos_next, 1);
    return 0;
}

static s32 v_qos_read(s32 conn, void *data, u32 *size)
{
    if (!size)
        return (s32)0x804E0902;
    *size = 0;
    return 0;
}

static s32 v_qos_get_attr(s32 id, s32 attr, void *v, s32 size)
{
    if (!v || size <= 0 || size > 0x1000)
        return (s32)0x804E0902;
    memset(v, 0, (size_t)size);
    return 0;
}

HLE_ROOT(sceVoiceQoSInit, LIB(VQ), v_ok)
HLE_ROOT(sceVoiceQoSInitHQ, LIB(VQ), v_ok)

/* ---- Audio3d (§13): the host; a virtual library has no ports ---- */

static s32 v_a3d_init(s64 reserved) { return reserved ? (s32)0x80EA0004 : 0; /* INVALID_PARAMETER */ }

static s32 v_a3d_default_params(u8 *p)
{
    if (!p)
        return (s32)0x80EA0004;
    memset(p, 0, 0x20); /* older callers pass 0x20 bytes: numBeds (+0x20) is left alone */
    *(u64 *)(p + 0x00) = 0x20;
    *(u32 *)(p + 0x08) = 256; /* granularity */
    *(u32 *)(p + 0x0C) = 0;   /* 48 kHz */
    *(u32 *)(p + 0x10) = 512; /* max objects */
    *(u32 *)(p + 0x14) = 2;   /* queue depth */
    *(u32 *)(p + 0x18) = 2;   /* ADVANCE_AND_PUSH */
    return 0;
}

HLE_ROOT(sceAudio3dInitialize, LIB(A3), v_a3d_init)

static s64 w_a3d_port_open(u64 user, u64 params, u32 *port)
{
    static void *cache;
    struct hle_lib *L = LIB(A3);
    void *fn = L->mode == HM_VIRT ? NULL : hle_host(L, "sceAudio3dPortOpen", &cache);
    s32 r;
    if (fn) {
        r = ((s32(*)(u64, u64, u32 *))fn)(user, params, port);
    } else {
        if (port)
            *port = 0xFFFFFFFF;
        r = (s32)0x80EA0006; /* OUT_OF_RESOURCES: titles fall back to AudioOut */
    }
    logf(TAG ": sceAudio3dPortOpen(user %x) -> %x, port %x%s", (u32)user, (u32)r, port ? *port : 0,
         fn ? "" : " (virtual)");
    return r;
}

/* ---- Rudp (§14) ---- */

HLE_ROOT(sceRudpInit, LIB(RU), v_ok)

/* ---- the routed functions: X(lib, name, flags, virtual answer, virtual body) ---- */

#define MEDIA_ROUTES(X)                                                                           \
    X(GLS, sceGameLiveStreamingTerminate, RF_VIRT, 0, 0)                                          \
    X(GLS, sceGameLiveStreamingGetCurrentStatus, 0, 0, v_gls_status)                              \
    X(GLS, sceGameLiveStreamingGetCurrentStatus2, 0, 0, v_gls_status2)                            \
    X(GLS, sceGameLiveStreamingEnableLiveStreaming, 0, 0, 0)                                      \
    X(GLS, sceGameLiveStreamingPermitLiveStreaming, 0, 0, 0)                                      \
    X(GLS, sceGameLiveStreamingPermitServerSideRecording, 0, 0, 0)                                \
    X(GLS, sceGameLiveStreamingSetGuardAreas, 0, 0, 0)                                            \
    X(GLS, sceGameLiveStreamingSetMaxBitrate, 0, 0, 0)                                            \
    X(GLS, sceGameLiveStreamingSetSpoilerTag, 0, 0, 0)                                            \
    X(GLS, sceGameLiveStreamingClearSpoilerTag, 0, 0, 0)                                          \
    X(GLS, sceGameLiveStreamingRegisterCallback, 0, 0, 0)                                         \
    X(GLS, sceGameLiveStreamingUnregisterCallback, 0, 0, 0)                                       \
    X(GLS, sceGameLiveStreamingCheckCallback, 0, 0, 0)                                            \
    X(GLS, sceGameLiveStreamingLaunchLiveViewer, RF_VIRT | RF_LOG, 0, 0)                          \
    X(GLS, sceGameLiveStreamingLaunchLiveViewerA, RF_VIRT | RF_LOG, 0, 0)                         \
    X(GLS, sceGameLiveStreamingGetProgramInfo, 0, 0x80A00008, 0)                                  \
    X(GLS, sceGameLiveStreamingGetCurrentBroadcastScreenLayout, 0, 0x80A00008, 0)                 \
    X(GLS, sceGameLiveStreamingGetSocialFeedbackMessages, 0, 0x80A00008, 0)                       \
    X(GLS, sceGameLiveStreamingGetSocialFeedbackMessagesCount, 0, 0x80A00008, 0)                  \
    X(GLS, sceGameLiveStreamingSetMetadata, 0, 0, 0)                                              \
    X(GLS, sceGameLiveStreamingSetInvitationSessionId, 0, 0, 0)                                   \
    X(GLS, sceGameLiveStreamingSetLinkCommentPreset, 0, 0, 0)                                     \
    X(GLS, sceGameLiveStreamingSetCameraFrameSetting, 0, 0, 0)                                    \
    X(GLS, sceGameLiveStreamingSetDefaultServiceProviderPermission, 0, 0, 0)                      \
    X(GLS, sceGameLiveStreamingSetServiceProviderPermission, 0, 0, 0)                             \
    X(GLS, sceGameLiveStreamingEnableSocialFeedback, 0, 0, 0)                                     \
    X(GLS, sceGameLiveStreamingPostSocialMessage, 0, 0, 0)                                        \
    X(GLS, sceGameLiveStreamingApplySocialFeedbackMessageFilter, 0, 0, 0)                         \
    X(GLS, sceGameLiveStreamingStartSocialFeedbackMessageFiltering, 0, 0, 0)                      \
    X(GLS, sceGameLiveStreamingStopSocialFeedbackMessageFiltering, 0, 0, 0)                       \
    X(GLS, sceGameLiveStreamingClearSocialFeedbackMessages, 0, 0, 0)                              \
    X(GLS, sceGameLiveStreamingSetPresetSocialFeedbackCommands, 0, 0, 0)                          \
    X(GLS, sceGameLiveStreamingClearPresetSocialFeedbackCommands, 0, 0, 0)                        \
    X(GLS, sceGameLiveStreamingSetPresetSocialFeedbackCommandsDescription, 0, 0, 0)               \
    X(GLS, sceGameLiveStreamingScreenInitialize, 0, 0x80A0000C, 0)                                \
    X(GLS, sceGameLiveStreamingScreenTerminate, 0, 0x80A0000C, 0)                                 \
    X(GLS, sceGameLiveStreamingScreenSetMode, 0, 0x80A0000C, 0)                                   \
    X(GLS, sceGameLiveStreamingScreenOpenSeparateMode, 0, 0x80A0000C, 0)                          \
    X(GLS, sceGameLiveStreamingScreenCloseSeparateMode, 0, 0x80A0000C, 0)                         \
    X(GLS, sceGameLiveStreamingScreenConfigureSeparateMode, 0, 0x80A0000C, 0)                     \
    X(GLS, sceGameLiveStreamingScreenInitializeSeparateModeParameter, 0, 0x80A0000C, 0)           \
    X(GLS, sceGameLiveStreamingStartGenerateStandbyScreenResource, 0, 0, 0)                       \
    X(GLS, sceGameLiveStreamingStopGenerateStandbyScreenResource, 0, 0, 0)                        \
    X(GLS, sceGameLiveStreamingStartDebugBroadcast, RF_VIRT, 0, 0)                                \
    X(GLS, sceGameLiveStreamingStopDebugBroadcast, RF_VIRT, 0, 0)                                 \
    X(SS, sceScreenShotIsDisabled, 0, 0, 0)                                                       \
    X(SS, sceScreenShotIsVshScreenCaptureDisabled, 0, 0, 0)                                       \
    X(SS, sceScreenShotGetAppInfo, 0, 0x80BE0141, 0)                                              \
    X(SS, sceScreenShotCapture, 0, 0x80BE0141, 0)                                                 \
    X(SS, sceScreenShotGetDrcParam, 0, 0x80BE0141, 0)                                             \
    X(SU, sceShareUtilityTerminate, 0, 0, 0)                                                      \
    X(SU, sceShareUtilityAdvanceTerminate, 0, 0, 0)                                               \
    X(SU, sceShareUtilityAdvanceSetUploadContentData, 0, 0, 0)                                    \
    X(SU, sceShareUtilityAdvanceSetPlayingContent, 0, 0, 0)                                       \
    X(SU, sceShareUtilityAdvanceResetPlayingContent, 0, 0, 0)                                     \
    X(SU, sceShareUtilityOpenShareMenu, RF_VIRT | RF_LOG, 0, 0)                                   \
    X(SU, sceShareUtilityOpenShareMenuDefault, RF_VIRT | RF_LOG, 0, 0)                            \
    X(SU, sceShareUtilityOpenShareMenuBroadcast, RF_VIRT | RF_LOG, 0, 0)                          \
    X(SU, sceShareUtilityOpenShareMenuForShareContent, RF_VIRT | RF_LOG, 0, 0)                    \
    X(SU, sceShareUtilityAdvanceWakeUpShareMenu, RF_VIRT | RF_LOG, 0, 0)                          \
    X(SU, sceShareUtilityAdvanceWakeUpShareMenuDefault, RF_VIRT | RF_LOG, 0, 0)                   \
    X(SU, sceShareUtilityAdvanceWakeUpShareMenuBroadcast, RF_VIRT | RF_LOG, 0, 0)                 \
    X(SU, sceShareUtilityAdvanceWakeUpShareMenuForShareContent, RF_VIRT | RF_LOG, 0, 0)           \
    X(SU, sceShareUtilityAdvanceGetServiceInfo, 0, 0x81170304, 0)                                 \
    X(SU, sceShareUtilityAdvanceQueryServiceInfo, 0, 0x81170304, 0)                               \
    X(SU, sceShareUtilityAdvanceQueryServiceInfoA, 0, 0x81170304, 0)                              \
    X(SU, sceShareUtilityAdvanceFromFileWithTitleIdList, RF_NOGFS, 0x8117000B, 0)                 \
    X(SU, sceShareUtilityAdvanceFromFileWithContentIdList, RF_NOGFS, 0x8117000B, 0)               \
    X(SU, sceShareUtilityAdvanceValidateContents, RF_NOGFS, 0x8117000B, 0)                        \
    X(SP, sceSharePlayTerminate, RF_VIRT, 0, 0)                                                   \
    X(SP, sceSharePlaySetProhibition, 0, 0, 0)                                                    \
    X(SP, sceSharePlaySetProhibitionModeWithAppId, 0, 0, 0)                                       \
    X(SP, sceSharePlayGetCurrentConnectionInfo, 0, 0, v_sp_connection)                            \
    X(SP, sceSharePlayGetCurrentConnectionInfoA, 0, 0x810E0109, 0)                                \
    X(SP, sceSharePlayGetEvent, 0, 0x810E0107, 0)                                                 \
    X(SP, sceSharePlayGetCurrentInfo, 0, 0x810E0109, 0)                                           \
    X(SP, sceSharePlaySetMode, 0, 0, 0)                                                           \
    X(SP, sceSharePlayNotifyDialogOpen, 0, 0, 0)                                                  \
    X(SP, sceSharePlayNotifyForceCloseForCdlg, 0, 0, 0)                                           \
    X(SP, sceSharePlayNotifyOpenQuickMenu, 0, 0, 0)                                               \
    X(SP, sceSharePlayResumeScreenForCdlg, 0, 0, 0)                                               \
    X(SP, sceSharePlayStartStandby, 0, 0, 0)                                                      \
    X(SP, sceSharePlayStopStandby, 0, 0, 0)                                                       \
    X(SP, sceSharePlayStartStreaming, 0, 0, 0)                                                    \
    X(SP, sceSharePlayStopStreaming, 0, 0, 0)                                                     \
    X(SP, sceSharePlayServerLock, 0, 0, 0)                                                        \
    X(SP, sceSharePlayServerUnLock, 0, 0, 0)                                                      \
    X(SP, sceSharePlayCrashDaemon, RF_VIRT | RF_LOG, 0, 0)                                        \
    X(RP, sceRemoteplayTerminate, RF_VIRT, 0, 0)                                                  \
    X(RP, sceRemoteplayGetConnectionStatus, 0, 0, v_rp_status)                                    \
    X(RP, sceRemoteplayProhibit, RF_KEEPRP, 0, 0)                                                 \
    X(RP, sceRemoteplayApprove, 0, 0, 0)                                                          \
    X(RP, sceRemoteplayProhibitStreaming, RF_KEEPRP, 0, 0)                                        \
    X(RP, sceRemoteplaySetProhibition, RF_KEEPRP, 0, 0)                                           \
    X(RP, sceRemoteplayIsRemotePlaying, 0, 0x80FC0102, 0)                                         \
    X(RP, sceRemoteplayGetRemoteplayStatus, 0, 0x80FC0102, 0)                                     \
    X(RP, sceRemoteplayGetConnectUserId, 0, 0x80FC0102, 0)                                        \
    X(RP, sceRemoteplayGetOperationStatus, 0, 0x80FC0102, 0)                                      \
    X(RP, sceRemoteplayGetRpMode, 0, 0x80FC0102, 0)                                               \
    X(RP, sceRemoteplayGetApMode, 0, 0x80FC0102, 0)                                               \
    X(RP, sceRemoteplayImeOpen, 0, 0x80FC0102, 0)                                                 \
    X(RP, sceRemoteplayImeClose, 0, 0x80FC0102, 0)                                                \
    X(RP, sceRemoteplayImeGetEvent, 0, 0x80FC0104, 0)                                             \
    X(RP, sceRemoteplayImeNotify, 0, 0x80FC0102, 0)                                               \
    X(RP, sceRemoteplayImeNotifyEventResult, 0, 0x80FC0102, 0)                                    \
    X(RP, sceRemoteplayImeFilterResult, 0, 0x80FC0102, 0)                                         \
    X(RP, sceRemoteplayImeSetCaret, 0, 0x80FC0102, 0)                                             \
    X(RP, sceRemoteplayImeSetText, 0, 0x80FC0102, 0)                                              \
    X(RP, sceRemoteplayIsRemoteOskReady, 0, 0x80FC0102, 0)                                        \
    X(RP, sceRemoteplaySetProhibitionForVsh, RF_VIRT, 0, 0)                                       \
    X(RP, sceRemoteplayServerLock, RF_VIRT, 0, 0)                                                 \
    X(RP, sceRemoteplayServerUnLock, RF_VIRT, 0, 0)                                               \
    X(RP, sceRemoteplaySetApMode, RF_VIRT, 0, 0)                                                  \
    X(RP, sceRemoteplaySetRpMode, RF_VIRT, 0, 0)                                                  \
    X(RP, sceRemoteplaySetLogLevel, RF_VIRT, 0, 0)                                                \
    X(RP, sceRemoteplayGeneratePinCode, RF_VIRT, 0, 0)                                            \
    X(RP, sceRemoteplayConfirmDeviceRegist, RF_VIRT, 0, 0)                                        \
    X(RP, sceRemoteplayClearAllRegistData, RF_VIRT, 0, 0)                                         \
    X(RP, sceRemoteplayClearConnectHistory, RF_VIRT, 0, 0)                                        \
    X(RP, sceRemoteplayGetConnectHistory, RF_VIRT, EUNAVAIL, 0)                                   \
    X(RP, sceRemoteplayDisconnect, RF_VIRT, 0, 0)                                                 \
    X(RP, sceRemoteplayChangeEnterKey, RF_VIRT, 0, 0)                                             \
    X(RP, sceRemoteplayGetMbusDeviceInfo, RF_VIRT, EUNAVAIL, 0)                                   \
    X(RP, sceRemoteplayNotifyMbusDeviceRegistComplete, RF_VIRT, 0, 0)                             \
    X(RP, sceRemoteplayNotifyNpPushWakeup, RF_VIRT, 0, 0)                                         \
    X(RP, sceRemoteplayNotifyPinCodeError, RF_VIRT, 0, 0)                                         \
    X(RP, sceRemoteplayNotifyUserDelete, RF_VIRT, 0, 0)                                           \
    X(RP, sceRemoteplayPrintAllRegistData, RF_VIRT, 0, 0)                                         \
    X(VR, sceVideoRecordingQueryMemSize, 0, 0x10000, 0)                                           \
    X(VR, sceVideoRecordingQueryMemSize2, 0, 0x10000, 0)                                          \
    X(VR, sceVideoRecordingQueryHddSize2, 0, 0, 0)                                                \
    X(VR, sceVideoRecordingStart, RF_OWNED, 0, v_vr_start)                                        \
    X(VR, sceVideoRecordingStop, RF_OWNED, 0, v_vr_stop)                                          \
    X(VR, sceVideoRecordingGetStatus, RF_OWNED, 0, v_vr_status)                                   \
    X(VR, sceVideoRecordingSetInfo, RF_OWNED, 0, v_vr_set_info)                                   \
    X(VR, sceVideoRecordingGetInfo, RF_OWNED, 0x80A80006, 0)                                      \
    X(VR, sceVideoRecordingInit, 0, 0, 0)                                                         \
    X(VR, sceVideoRecordingTerm, 0, 0, 0)                                                         \
    X(VR, sceVideoRecordingCopyBGRAtoNV12, RF_PURE, 0x80A80008, 0)                                \
    X(CE, sceContentExportTerm, 0, 0, 0)                                                          \
    X(CE, sceContentExportFromFile, RF_NOGFS, 0x809D3001, 0)                                      \
    X(CE, sceContentExportFromFileWithThumbnail, RF_NOGFS, 0x809D3001, 0)                         \
    X(CE, sceContentExportFromFileWithTitleIdList, RF_NOGFS, 0x809D3001, 0)                       \
    X(CE, sceContentExportFromFileWithContentIdList, RF_NOGFS, 0x809D3001, 0)                     \
    X(CE, sceContentExportFromData, 0, 0x809D3001, 0)                                             \
    X(CE, sceContentExportFromDataWithThumbnail, 0, 0x809D3001, 0)                                \
    X(CE, sceContentExportStart, 0, 0, 0)                                                         \
    X(CE, sceContentExportFinish, 0, 0, 0)                                                        \
    X(CE, sceContentExportCancel, 0, 0, 0)                                                        \
    X(CE, sceContentExportGetProgress, 0, EUNAVAIL, 0)                                            \
    X(CE, sceContentExportValidateContents, 0, 0, 0)                                              \
    X(VO, sceVoiceEnd, 0, 0, v_voice_end)                                                         \
    X(VO, sceVoiceStart, 0, 0, 0)                                                                 \
    X(VO, sceVoiceStop, 0, 0, 0)                                                                  \
    X(VO, sceVoiceCreatePort, 0, 0, v_voice_create_port)                                          \
    X(VO, sceVoiceDeletePort, 0, 0, v_voice_delete_port)                                          \
    X(VO, sceVoiceUpdatePort, 0, 0, v_voice_port_ok)                                              \
    X(VO, sceVoiceConnectIPortToOPort, 0, 0, v_voice_connect)                                     \
    X(VO, sceVoiceDisconnectIPortFromOPort, 0, 0, v_voice_connect)                                \
    X(VO, sceVoiceReadFromOPort, 0, 0, v_voice_read)                                              \
    X(VO, sceVoiceWriteToIPort, 0, 0, v_voice_write)                                              \
    X(VO, sceVoiceGetPortInfo, 0, 0, v_voice_port_info)                                           \
    X(VO, sceVoiceGetPortAttr, 0, 0, v_voice_port_attr)                                           \
    X(VO, sceVoiceSetVolume, 0, 0, v_voice_set_volume)                                            \
    X(VO, sceVoiceGetVolume, 0, 0, v_voice_get_volume)                                            \
    X(VO, sceVoiceSetMuteFlag, 0, 0, v_voice_set_mute)                                            \
    X(VO, sceVoiceGetMuteFlag, 0, 0, v_voice_get_mute)                                            \
    X(VO, sceVoiceSetMuteFlagAll, 0, 0, v_voice_mute_all)                                         \
    X(VO, sceVoiceSetBitRate, 0, 0, v_voice_set_bitrate)                                          \
    X(VO, sceVoiceGetBitRate, 0, 0, v_voice_get_bitrate)                                          \
    X(VO, sceVoicePausePort, 0, 0, v_voice_port_ok)                                               \
    X(VO, sceVoiceResumePort, 0, 0, v_voice_port_ok)                                              \
    X(VO, sceVoicePausePortAll, 0, 0, 0)                                                          \
    X(VO, sceVoiceResumePortAll, 0, 0, 0)                                                         \
    X(VO, sceVoiceResetPort, 0, 0, v_voice_port_ok)                                               \
    X(VO, sceVoiceGetResourceInfo, 0, 0, v_voice_resource_info)                                   \
    X(VO, sceVoiceSetThreadsParams, 0, 0, 0)                                                      \
    X(VO, sceVoiceVADAdjustment, 0, 0, 0)                                                         \
    X(VO, sceVoiceVADSetVersion, 0, 0, 0)                                                         \
    X(VQ, sceVoiceQoSEnd, 0, 0, 0)                                                                \
    X(VQ, sceVoiceQoSCreateLocalEndpoint, 0, 0, v_qos_new_id)                                     \
    X(VQ, sceVoiceQoSCreateRemoteEndpoint, 0, 0, v_qos_new_id)                                    \
    X(VQ, sceVoiceQoSDeleteLocalEndpoint, 0, 0, 0)                                                \
    X(VQ, sceVoiceQoSDeleteRemoteEndpoint, 0, 0, 0)                                               \
    X(VQ, sceVoiceQoSConnect, 0, 0, v_qos_new_id)                                                 \
    X(VQ, sceVoiceQoSDisconnect, 0, 0, 0)                                                         \
    X(VQ, sceVoiceQoSReadPacket, 0, 0, v_qos_read)                                                \
    X(VQ, sceVoiceQoSWritePacket, 0, 0, 0)                                                        \
    X(VQ, sceVoiceQoSSetLocalEndpointAttribute, 0, 0, 0)                                          \
    X(VQ, sceVoiceQoSSetRemoteEndpointAttribute, 0, 0, 0)                                         \
    X(VQ, sceVoiceQoSSetConnectionAttribute, 0, 0, 0)                                             \
    X(VQ, sceVoiceQoSGetLocalEndpointAttribute, 0, 0, v_qos_get_attr)                             \
    X(VQ, sceVoiceQoSGetRemoteEndpointAttribute, 0, 0, v_qos_get_attr)                            \
    X(VQ, sceVoiceQoSGetConnectionAttribute, 0, 0, v_qos_get_attr)                                \
    X(VQ, sceVoiceQoSGetStatus, 0, 0, v_qos_get_attr)                                             \
    X(VQ, sceVoiceQoSGetLocalEndpoint, 0, 0x804E0902, 0)                                          \
    X(VQ, sceVoiceQoSGetRemoteEndpoint, 0, 0x804E0902, 0)                                         \
    X(VQ, sceVoiceQoSDebugGetStatus, 0, 0x804E0902, 0)                                            \
    X(A3, sceAudio3dTerminate, RF_MASKSTUB, 0, 0)                                                 \
    X(A3, sceAudio3dGetDefaultOpenParameters, 0, 0, v_a3d_default_params)                         \
    X(A3, sceAudio3dPortClose, 0, 0x80EA0002, 0)                                                  \
    X(A3, sceAudio3dPortFlush, 0, 0x80EA0002, 0)                                                  \
    X(A3, sceAudio3dPortAdvance, 0, 0x80EA0002, 0)                                                \
    X(A3, sceAudio3dPortPush, 0, 0x80EA0002, 0)                                                   \
    X(A3, sceAudio3dPortGetQueueLevel, 0, 0x80EA0002, 0)                                          \
    X(A3, sceAudio3dPortSetAttribute, 0, 0x80EA0002, 0)                                           \
    X(A3, sceAudio3dPortGetAttributesSupported, 0, 0x80EA0002, 0)                                 \
    X(A3, sceAudio3dObjectReserve, 0, 0x80EA0002, 0)                                              \
    X(A3, sceAudio3dObjectUnreserve, 0, 0x80EA0002, 0)                                            \
    X(A3, sceAudio3dObjectSetAttributes, 0, 0x80EA0002, 0)                                        \
    X(A3, sceAudio3dBedWrite, 0, 0x80EA0002, 0)                                                   \
    X(A3, sceAudio3dBedWrite2, 0, 0x80EA0002, 0)                                                  \
    X(A3, sceAudio3dAudioOutOpen, 0, 0x80EA0002, 0)                                               \
    X(A3, sceAudio3dAudioOutClose, 0, 0x80EA0002, 0)                                              \
    X(A3, sceAudio3dAudioOutOutput, 0, 0x80EA0002, 0)                                             \
    X(A3, sceAudio3dAudioOutOutputs, 0, 0x80EA0002, 0)                                            \
    X(RU, sceRudpEnd, 0, 0, 0)                                                                    \
    X(RU, sceRudpEnableInternalIOThread, 0, 0, 0)                                                 \
    X(RU, sceRudpEnableInternalIOThread2, 0, 0, 0)                                                \
    X(RU, sceRudpSetEventHandler, 0, 0, 0)                                                        \
    X(RU, sceRudpSetOption, 0, 0, 0)                                                              \
    X(RU, sceRudpSetMaxSegmentSize, 0, 0, 0)                                                      \
    X(RU, sceRudpCreateContext, 0, 0x80770008, 0)                                                 \
    X(RU, sceRudpBind, 0, 0x80770003, 0)                                                          \
    X(RU, sceRudpInitiate, 0, 0x80770003, 0)                                                      \
    X(RU, sceRudpActivate, 0, 0x80770003, 0)                                                      \
    X(RU, sceRudpListen, 0, 0x80770003, 0)                                                        \
    X(RU, sceRudpAccept, 0, 0x80770003, 0)                                                        \
    X(RU, sceRudpTerminate, 0, 0, 0)                                                              \
    X(RU, sceRudpRead, 0, 0x80770003, 0)                                                          \
    X(RU, sceRudpWrite, 0, 0x80770003, 0)                                                         \
    X(RU, sceRudpFlush, 0, 0x80770003, 0)                                                         \
    X(RU, sceRudpNetReceived, 0, 0x80770003, 0)                                                   \
    X(RU, sceRudpNetFlush, 0, 0, 0)                                                               \
    X(RU, sceRudpProcessEvents, 0, 0, 0)                                                          \
    X(RU, sceRudpPollCreate, 0, 0, 0)                                                             \
    X(RU, sceRudpPollDestroy, 0, 0, 0)                                                            \
    X(RU, sceRudpPollControl, 0, 0, 0)                                                            \
    X(RU, sceRudpPollCancel, 0, 0, 0)                                                             \
    X(RU, sceRudpPollWait, 0, 0x80770016, 0)                                                      \
    X(RU, sceRudpGetStatus, 0, 0x80770003, 0)                                                     \
    X(RU, sceRudpGetContextStatus, 0, 0x80770003, 0)                                              \
    X(RU, sceRudpGetLocalInfo, 0, 0x80770003, 0)                                                  \
    X(RU, sceRudpGetRemoteInfo, 0, 0x80770003, 0)                                                 \
    X(RU, sceRudpGetOption, 0, 0x80770003, 0)                                                     \
    X(RU, sceRudpGetMaxSegmentSize, 0, 0x80770003, 0)                                             \
    X(RU, sceRudpGetNumberOfPacketsToRead, 0, 0x80770003, 0)                                      \
    X(RU, sceRudpGetSizeReadable, 0, 0x80770003, 0)                                               \
    X(RU, sceRudpGetSizeWritable, 0, 0x80770003, 0)

enum { MEDIA_ROUTES(HLE_ENUM_ROW) R_COUNT };
HLE_TRAMPOLINES(hle_media_tramps, MEDIA_ROUTES);
extern const char hle_media_tramps[] HIDDEN, hle_media_tramps_end[] HIDDEN;

#define ENT(lib, name, flags, ret, virt) {N_##lib, #name, (void *)&hle_media_tramps[16 * R_##name], NULL},
#define WRAP(lib, name) {N_##lib, #name, w_##name, NULL}
#define CONST(lib, name, fn) {N_##lib, #name, fn, NULL}

static const struct ext_hle_ent k_hle[] = {
    /* roots, setters and wrappers that look at the host's answer */
    WRAP(GLS, sceGameLiveStreamingInitialize),
    WRAP(GLS, sceGameLiveStreamingSetStandbyScreenResource),
    WRAP(SS, sceScreenShotSetOverlayImage),
    WRAP(SS, sceScreenShotSetOverlayImageWithOrigin),
    WRAP(SS, sceScreenShotEnable),
    WRAP(SS, sceScreenShotDisable),
    WRAP(SS, sceScreenShotEnableNotification),
    WRAP(SS, sceScreenShotDisableNotification),
    WRAP(SS, sceScreenShotSetParam),
    WRAP(SS, sceScreenShotSetDrcParam),
    WRAP(SU, sceShareUtilityInitializeEx2),
    WRAP(SU, sceShareUtilityAdvanceInitialize),
    WRAP(SU, sceShareUtilityInitialize),
    WRAP(SP, sceSharePlayInitialize),
    WRAP(RP, sceRemoteplayInitialize),
    CONST(VR, sceVideoRecordingOpen, w_vr_open),
    CONST(VR, sceVideoRecordingOpen2, w_vr_open2),
    CONST(VR, sceVideoRecordingClose, w_vr_close),
    WRAP(CE, sceContentExportInit),
    WRAP(CE, sceContentExportInit2),
    WRAP(VO, sceVoiceInit),
    WRAP(VO, sceVoiceInitHQ),
    WRAP(VQ, sceVoiceQoSInit),
    WRAP(VQ, sceVoiceQoSInitHQ),
    WRAP(A3, sceAudio3dInitialize),
    CONST(A3, sceAudio3dPortOpen, w_a3d_port_open),
    WRAP(RU, sceRudpInit),
    /* never the host: SWRR's companion server, profiler markers */
    CONST(CH, sceCompanionHttpdInitialize, r_0),
    CONST(CH, sceCompanionHttpdInitialize2, r_0),
    CONST(CH, sceCompanionHttpdOptParamInitialize, r_0),
    CONST(CH, sceCompanionHttpdStart, r_0),
    CONST(CH, sceCompanionHttpdStop, r_0),
    CONST(CH, sceCompanionHttpdTerminate, r_0),
    CONST(CH, sceCompanionHttpdRegisterRequestCallback, r_0),
    CONST(CH, sceCompanionHttpdRegisterRequestCallback2, r_0),
    CONST(CH, sceCompanionHttpdRegisterRequestBodyReceptionCallback, r_0),
    CONST(CH, sceCompanionHttpdUnregisterRequestCallback, r_0),
    CONST(CH, sceCompanionHttpdUnregisterRequestBodyReceptionCallback, r_0),
    CONST(CH, sceCompanionHttpdGetEvent, r_httpd_no_event),
    CONST(CH, sceCompanionHttpdGetUserId, r_httpd_no_user),
    CONST(CH, sceCompanionHttpdGet2ndScreenStatus, r_0),
    CONST(CH, sceCompanionHttpdAddHeader, r_0),
    CONST(CH, sceCompanionHttpdSetBody, r_0),
    CONST(CH, sceCompanionHttpdSetStatus, r_0),
    CONST(CU, sceCompanionUtilInitialize, r_0),
    CONST(CU, sceCompanionUtilOptParamInitialize, r_0),
    CONST(CU, sceCompanionUtilTerminate, r_0),
    CONST(CU, sceCompanionUtilGetEvent, r_util_no_event),
    CONST(CU, sceCompanionUtilGetRemoteOskEvent, r_util_no_event),
    CONST(RZ, sceRazorCpuPushMarker, r_0),
    CONST(RZ, sceRazorCpuPushMarkerStatic, r_0),
    CONST(RZ, sceRazorCpuPopMarker, r_0),
    CONST(RZ, sceRazorCpuSync, r_0),
    CONST(RZ, sceRazorCpuNamedSync, r_0),
    CONST(RZ, sceRazorCpuPlotValue, r_0),
    CONST(RZ, sceRazorCpuWriteBookmark, r_0),
    CONST(RZ, sceRazorCpuFiberSwitch, r_0),
    CONST(RZ, sceRazorCpuFiberLogNameChange, r_0),
    CONST(RZ, sceRazorCpuJobManagerDispatch, r_0),
    CONST(RZ, sceRazorCpuJobManagerJob, r_0),
    CONST(RZ, sceRazorCpuJobManagerSequence, r_0),
    CONST(RZ, sceRazorCpuWorkloadRunEnd, r_0),
    CONST(RZ, sceRazorCpuWorkloadSubmit, r_0),
    CONST(RZ, sceRazorCpuGpuMarkerSync, r_0),
    CONST(RZ, sceRazorCpuIsCapturing, r_0),
    CONST(RZ, sceRazorCpuFlushOccurred, r_0),
    CONST(RZ, sceRazorCpuInitDataTags, r_0),
    CONST(RZ, sceRazorCpuShutdownDataTags, r_0),
    CONST(RZ, sceRazorCpuTagBuffer, r_0),
    CONST(RZ, sceRazorCpuUnTagBuffer, r_0),
    CONST(RZ, sceRazorCpuTagArray, r_0),
    CONST(RZ, sceRazorCpuResizeTaggedBuffer, r_0),
    CONST(RZ, sceRazorCpuGetDataTagStorageSize, r_0),
    CONST(RZ, sceRazorCpuInitializeGpuMarkerContext, r_0),
    CONST(RZ, sceRazorCpuBeginLogicalFileAccess, r_0),
    CONST(RZ, sceRazorCpuEndLogicalFileAccess, r_0),
    CONST(RZ, sceRazorCpuSetPushMarkerCallback, r_0),
    CONST(RZ, sceRazorCpuSetPushMarkerStaticCallback, r_0),
    CONST(RZ, sceRazorCpuSetPopMarkerCallback, r_0),
    CONST(RZ, sceRazorCpuDisableFiberUserMarkers, r_0),
    CONST(RZ, sceRazorCpuInitializeInternal, r_0),
    CONST(RZ, sceRazorCpuStartCaptureInternal, r_0),
    CONST(RZ, sceRazorCpuStopCaptureInternal, r_0),
    /* routed: the host or the virtual answer, by the library's mode */
    MEDIA_ROUTES(ENT)
};

static const struct hle_route k_routes[R_COUNT] = {MEDIA_ROUTES(HLE_ROUTE_ROW)};

static struct hle_router g_router = {
    .tag = TAG,
    .libs = g_libs,
    .nlibs = NLIBS,
    .hle = k_hle,
    .nhle = ARRAY_SIZE(k_hle),
    .first = ARRAY_SIZE(k_hle) - R_COUNT,
    .routes = k_routes,
    .tramps = hle_media_tramps,
    .tramps_end = hle_media_tramps_end,
};

static int media_init(void) { return hle_router_init(&g_router); }
static void media_sysmodule(u32 id, s32 rc) { hle_router_sysmodule(&g_router, id, rc); }

EXT_FAMILY_HOOKS("media", k_hle, k_pol, media_init, media_sysmodule);
