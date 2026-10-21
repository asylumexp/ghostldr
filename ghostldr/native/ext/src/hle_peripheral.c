/*
 * Peripheral libraries (research spec "media-peripherals", §16-§22):
 * Camera, Move, MoveTracker, Hmd (+Distortion, SetupDialog), VrTracker,
 * Usbd, Keyboard, Mouse, SystemGesture.
 *
 * With no accessory attached these answer like a real PS4 without it: the
 * device's own errors (camera not connected, headset not detected, Move
 * return code 1) reach the title.  Calls that could take SWRR or the console
 * down stay virtual until a hardware probe clears them (hle_mode.h):
 * sceCameraOpen/Start, sceHmdReprojection*, sceVrTrackerInit and its GPU
 * calls.  VrTracker answers its memory query with tiny sizes while virtual
 * (the real one asks for about 70 MiB).  Modes and routing: hle_mode.h.
 */
#include "hle_mode.h"

enum { K_CAM, K_MOVE, K_MT, K_HMD, K_HSD, K_VRT, K_USB, K_KB, K_DKB, K_MOUSE, K_SG, NLIBS };
#define N_CAM "libSceCamera"
#define N_MOVE "libSceMove"
#define N_MT "libSceMoveTracker"
#define N_HMD "libSceHmd"
#define N_HSD "libSceHmdSetupDialog"
#define N_VRT "libSceVrTracker"
#define N_USB "libSceUsbd"
#define N_KB "libSceKeyboard"
#define N_DKB "libSceDbgKeyboard"
#define N_MOUSE "libSceMouse"
#define N_SG "libSceSystemGesture"

static const struct ext_lib k_pol[NLIBS] = {
    [K_CAM] = {N_CAM, NULL, 0, (s32)0x802E0010},                 /* NOT_CONNECTED */
    [K_MOVE] = {N_MOVE, NULL, 0, (s32)0x80EE0001},               /* NOT_INIT */
    [K_MT] = {N_MT, NULL, 0, (s32)0x80EF0004},                   /* INVALID_HANDLE */
    [K_HMD] = {N_HMD, "libSceHmdDistortion libsceHmdReprojectionMultilayer", 0, (s32)0x81110004}, /* DEVICE_DISCONNECTED */
    [K_HSD] = {N_HSD, NULL, EXT_LIB_NO_HOST, 0},                 /* system UI: never opened */
    [K_VRT] = {N_VRT, "libSceVrTrackerFourDeviceAllowed libSceVrTrackerDeviceRejection libSceVrTrackerLiveCapture", 0,
               (s32)0x8126081E},                                 /* NOT_SUPPORTED */
    [K_USB] = {N_USB, NULL, 0, (s32)0x80240004},                 /* NO_DEVICE */
    [K_KB] = {N_KB, NULL, 0, (s32)0x80DA0005},                   /* NOT_INITIALIZED */
    [K_DKB] = {N_DKB, NULL, EXT_LIB_NO_HOST, (s32)0x80DA0005},
    [K_MOUSE] = {N_MOUSE, NULL, 0, (s32)0x80DF0005},             /* NOT_INITIALIZED */
    [K_SG] = {N_SG, NULL, 0, (s32)0x80890003},                   /* NOT_INITIALIZED */
};

#define SET(...) ((const s32[]){__VA_ARGS__, 0})
static struct hle_lib g_libs[NLIBS] = {
    [K_CAM] = {.pol = &k_pol[K_CAM], .sysmod = {0x8000001A}, .probed = HLE_PROBED_CAMERA,
               .deny = SET((s32)0x802E0011, (s32)0x802E00FB, (s32)0x802E00FE), /* NOT_SUPPORTED, MAX_PROCESS, KPROC_CREATE */
               .legit = SET((s32)0x802E0010)},                                 /* NOT_CONNECTED */
    [K_MOVE] = {.pol = &k_pol[K_MOVE], .sysmod = {0x008F}, .already = SET((s32)0x80EE0002)},
    [K_MT] = {.pol = &k_pol[K_MT], .sysmod = {0x00B1}, .init_mode = HLE_PROBED_CAMERA ? HM_UNDECIDED : HM_VIRT,
              .why = "it needs camera frames, and the camera is virtual until probed"},
    [K_HMD] = {.pol = &k_pol[K_HMD], .sysmod = {0x00D4}, .probed = HLE_PROBED_HMD_REPROJECTION,
               .already = SET((s32)0x81110001), .deny = SET((s32)0x81110016), /* UNSUPPORTED_FEATURE */
               .legit = SET((s32)0x81110004)},                                 /* DEVICE_DISCONNECTED */
    [K_HSD] = {.pol = &k_pol[K_HSD], .sysmod = {0x00EB}, .init_mode = HM_VIRT, .why = "system UI"},
    [K_VRT] = {.pol = &k_pol[K_VRT], .sysmod = {0x00ED}, .probed = HLE_PROBED_VR_TRACKER,
               .init_mode = HLE_PROBED_VR_TRACKER ? HM_UNDECIDED : HM_VIRT,
               .why = "Init and its GPU calls are crash class until probed"},
    [K_USB] = {.pol = &k_pol[K_USB], .sysmod = {0x00B7},
               .deny = SET((s32)0x80240003, (s32)0x8024000C)},                 /* ACCESS, NOT_SUPPORTED */
    [K_KB] = {.pol = &k_pol[K_KB], .sysmod = {0x0106}},
    [K_DKB] = {.pol = &k_pol[K_DKB], .sysmod = {0x00A3}, .init_mode = HM_VIRT,
               .why = "a debug keyboard: no such module on a retail console"},
    [K_MOUSE] = {.pol = &k_pol[K_MOUSE], .sysmod = {0x00A9}},
    [K_SG] = {.pol = &k_pol[K_SG], .sysmod = {0x00CE}},
};
#define LIB(k) (&g_libs[K_##k])
#define TAG "hle_periph"

static s64 v_ok(void) { return 0; }
HLE_RET_FN(r_0, 0)
HLE_RET_FN(r_eunavail, EUNAVAIL)

/* ---- Camera (§16): no camera; Open and Start are crash class ---- */

#define E_CAM(n) ((s32)(0x802E0000 + (n)))
static volatile u8 g_cam_open[4]; /* handles 1..4 */

static bool cam_handle(s32 h) { return h >= 1 && h <= 4 && g_cam_open[h - 1]; }

static s32 v_cam_open(s32 user, s32 type, s32 index, const u32 *param)
{
    if (type != 0 || index != 0 || (param && param[0] != 0x10))
        return E_CAM(0x00); /* PARAM */
    for (u32 i = 0; i < 4; i++)
        if (__sync_bool_compare_and_swap(&g_cam_open[i], 0, 1))
            return (s32)i + 1; /* a PS4 opens the camera even when none is attached */
    return E_CAM(0x14);        /* MAX_HANDLE */
}

static s64 w_cam_open(u64 user, u64 type, u64 index, u64 param)
{
    static void *cache;
    struct hle_lib *L = LIB(CAM);
    if (!L->probed)
        hle_set(L, HM_VIRT, "sceCameraOpen", 0, "crash class until probed (system flex memory)");
    const u64 v[6] = {user, type, index, param};
    s32 rc;
    return hle_root(L, "sceCameraOpen", &cache, v, &rc) ? rc
                                                        : v_cam_open((s32)user, (s32)type, (s32)index, (const u32 *)param);
}

static s32 v_cam_close(s32 h)
{
    if (h < 1)
        return E_CAM(0x00);
    if (!cam_handle(h))
        return E_CAM(0x04); /* NOT_OPEN */
    g_cam_open[h - 1] = 0;
    return 0;
}

static s32 v_cam_attached(s32 index) { return index ? E_CAM(0x00) : 0; /* not attached */ }

static s32 v_cam_device_info(s32 reserved, const u32 *info)
{
    if (!info || info[0] != 0x10 || info[1] != 1)
        return E_CAM(0x00);
    for (u32 i = 0; i < 4; i++)
        if (g_cam_open[i])
            return E_CAM(0x10); /* NOT_CONNECTED */
    return E_CAM(0x02);         /* NOT_INIT */
}

static s32 v_cam_config(const u32 *config) { return !config || config[0] != 0x48 ? E_CAM(0x00) : E_CAM(0x10); }
static s32 v_cam_config_h(s32 h, const u32 *config) { return v_cam_config(config); }
static s32 v_cam_start(s32 h, const u32 *param) { return !param || param[0] != 0x18 ? E_CAM(0x00) : E_CAM(0x10); }
static s32 v_cam_stop(s32 h) { return cam_handle(h) ? 0 : E_CAM(0x04); }

static s32 v_cam_frame(s32 h, u8 *frame)
{
    if (!frame)
        return E_CAM(0x00);
    *(u32 *)(frame + 0xE8) = 0xFFFFFFFF; /* status[0..1]: no frame */
    *(u32 *)(frame + 0xEC) = 0xFFFFFFFF;
    return E_CAM(0x06);                  /* NOT_START */
}

static s32 v_cam_mmap_count(u32 *count)
{
    if (!count)
        return E_CAM(0x00);
    *count = 0;
    return 0;
}

/* ---- Move (§17): no controller ---- */

#define E_MOVE(n) ((s32)(0x80EE0000 + (n)))
static volatile u8 g_move_init;

static s32 v_move_init(void) { return __sync_bool_compare_and_swap(&g_move_init, 0, 1) ? 0 : E_MOVE(2); }

static s32 v_move_term(void)
{
    if (!g_move_init)
        return E_MOVE(1); /* NOT_INIT */
    g_move_init = 0;
    return 0;
}

static s32 v_move_open(s32 user, s32 type, s32 index)
{
    if (!g_move_init)
        return E_MOVE(1);
    if (type != 0 || index < 0 || index > 3)
        return E_MOVE(3); /* INVALID_ARG */
    return 0x4D000100 + 0x100 * index; /* a handle even with no controller */
}

static s32 v_move_latest(s32 h, void *data)
{
    if (!data)
        return E_MOVE(3);
    memset(data, 0, 0x40);
    return 1; /* NO_CONTROLLER_CONNECTED */
}

static s32 v_move_recent(s32 h, s64 ts, void *data, s32 *count)
{
    if (count)
        *count = 0;
    return 1;
}

static s32 v_move_device_info(s32 h, void *info)
{
    if (info)
        memset(info, 0, 0x10);
    return 1;
}

static s32 v_move_valid(s32 h) { return (h & ~0xF00) == 0x4D000000 && (h & 0xF00) >= 0x100 && (h & 0xF00) <= 0x400; }

HLE_ROOT(sceMoveInit, LIB(MOVE), v_move_init)
HLE_ROOT(sceMoveTrackerInit, LIB(MT), v_ok)

/* ---- Hmd (§18): no headset; reprojection is crash class ---- */

#define E_HMD(n) ((s32)(0x81110000 + (n)))
#define HMD_HANDLE 0x0F000000
static volatile u8 g_hmd_init, g_hmd_open;
static volatile s32 g_hmd_user = -1;

static s32 v_hmd_init(const void *param)
{
    if (!param)
        return E_HMD(0x08); /* PARAMETER_NULL */
    return __sync_bool_compare_and_swap(&g_hmd_init, 0, 1) ? 0 : E_HMD(0x01);
}

static s64 hmd_init(const char *name, void **cache, u64 param)
{
    const u64 v[6] = {param};
    s32 rc;
    if (hle_root(LIB(HMD), name, cache, v, &rc)) {
        if (rc >= 0)
            g_hmd_init = 1; /* a virtual Open after a refused one finds it initialised */
        return rc;
    }
    return v_hmd_init((const void *)param);
}

static s64 w_hmd_init(u64 param)
{
    static void *cache;
    return hmd_init("sceHmdInitialize", &cache, param);
}

static s64 w_hmd_init315(u64 param)
{
    static void *cache;
    return hmd_init("sceHmdInitialize315", &cache, param);
}

static s32 v_hmd_open(s32 user, s32 type, s32 index, const void *param)
{
    if (!g_hmd_init)
        return E_HMD(0x02); /* NOT_INITIALIZED */
    if (type || index || param || user == -1 || user == 0xFF)
        return E_HMD(0x09); /* PARAMETER_INVALID */
    if (!__sync_bool_compare_and_swap(&g_hmd_open, 0, 1))
        return E_HMD(0x06); /* ALREADY_OPENED */
    g_hmd_user = user;
    return HMD_HANDLE;
}

static s64 w_hmd_open(u64 user, u64 type, u64 index, u64 param)
{
    static void *cache;
    const u64 v[6] = {user, type, index, param};
    s32 rc;
    return hle_root(LIB(HMD), "sceHmdOpen", &cache, v, &rc)
               ? rc
               : v_hmd_open((s32)user, (s32)type, (s32)index, (const void *)param);
}

static s32 v_hmd_close(s32 h)
{
    if (h != HMD_HANDLE || !g_hmd_open)
        return E_HMD(0x03); /* HANDLE_INVALID */
    g_hmd_open = 0;
    g_hmd_user = -1;
    return 0;
}

static s32 v_hmd_term(void)
{
    g_hmd_init = g_hmd_open = 0;
    g_hmd_user = -1;
    return 0;
}

struct hmd_info {
    u32 status; /* 0 READY, 1 NOT_READY, 2 NOT_DETECTED */
    s32 user_id;
    u8 reserve0[4];
    u32 panel_width, panel_height;
    u16 latency90, latency120;
    u8 hmu_mount, reserve1[7];
};
_Static_assert(sizeof(struct hmd_info) == 0x20, "SceHmdDeviceInformation");

static s32 v_hmd_info(struct hmd_info *info)
{
    if (!info)
        return E_HMD(0x08);
    memset(info, 0, sizeof(*info));
    info->status = 2; /* NOT_DETECTED */
    info->user_id = g_hmd_open ? g_hmd_user : -1;
    return 0;
}

/* the device information from the host also tells whether a headset is
 * READY: reprojection, distortion and VrTracker pass only then */
static s64 hmd_info(const char *name, void **cache, bool by_handle, u64 h, struct hmd_info *info)
{
    struct hle_lib *L = LIB(HMD);
    void *fn = L->mode == HM_VIRT ? NULL : hle_host(L, name, cache);
    if (fn) {
        s32 r = by_handle ? ((s32(*)(u64, void *))fn)(h, info) : ((s32(*)(void *))fn)(info);
        if (r == 0 && info && info->status == 0 && !L->ready) {
            L->ready = 1;
            logf(TAG ": %s: the headset is READY", name);
        }
        return r;
    }
    if (by_handle && ((s32)h != HMD_HANDLE || !g_hmd_open))
        return E_HMD(0x03);
    return v_hmd_info(info);
}

static s64 w_hmd_info(struct hmd_info *info)
{
    static void *cache;
    return hmd_info("sceHmdGetDeviceInformation", &cache, false, 0, info);
}

static s64 w_hmd_info_by_handle(u64 h, struct hmd_info *info)
{
    static void *cache;
    return hmd_info("sceHmdGetDeviceInformationByHandle", &cache, true, h, info);
}

/* HmdSetupDialog: system UI, never opened */
struct hsd_result {
    s32 result;
    u8 reserved[32];
};

static s32 v_hsd_open(const void *param)
{
    logf(TAG ": sceHmdSetupDialogOpen: system UI, not opened (answers FINISHED, USER_CANCELED)");
    return 0;
}

static s32 v_hsd_result(struct hsd_result *r)
{
    if (!r)
        return E_HMD(0x08);
    memset(r, 0, sizeof(*r));
    r->result = 1; /* USER_CANCELED */
    return 0;
}
HLE_RET_FN(r_finished, 3)

/* ---- VrTracker (§19): virtual unless a READY headset; tiny memory ---- */

#define E_VRT(n) ((s32)(0x81260800 + (n)))
static volatile u8 g_vrt_init;
static volatile s32 g_vrt_dev[4]; /* registered handle + 1, by device type */

static s32 v_vrt_query(const u32 *param, u32 *result)
{
    if (!param || !result || param[0] != 0x40 || (param[1] != 0 && param[1] != 100))
        return E_VRT(0x06); /* ARGUMENT_INVALID */
    memset(result, 0, 0x40);
    result[0] = 0x40;
    for (u32 i = 1; i <= 6; i++)
        result[i] = 0x10000; /* onion, garlic and work sizes and alignments: the real ones total ~70 MiB */
    return 0;
}

static s64 w_vrt_query(const u32 *param, u32 *result)
{
    static void *cache;
    struct hle_lib *L = LIB(VRT), *H = LIB(HMD);
    if (L->mode == HM_UNDECIDED && !(H->mode == HM_PASS && H->ready))
        hle_set(L, HM_VIRT, "sceVrTrackerQueryMemory", 0, "no READY headset");
    const u64 v[6] = {(u64)param, (u64)result};
    s32 rc;
    if (hle_root(L, "sceVrTrackerQueryMemory", &cache, v, &rc))
        return rc;
    rc = v_vrt_query(param, result);
    logf(TAG ": sceVrTrackerQueryMemory -> %x: virtual, %x bytes each", (u32)rc, rc ? 0 : result[1]);
    return rc;
}

static s32 v_vrt_init(void) { return __sync_bool_compare_and_swap(&g_vrt_init, 0, 1) ? 0 : E_VRT(0x02); }

static s64 w_vrt_init(u64 param)
{
    static void *cache;
    struct hle_lib *L = LIB(VRT);
    if (!L->probed)
        hle_set(L, HM_VIRT, "sceVrTrackerInit", 0, "crash class until probed");
    const u64 v[6] = {param};
    s32 rc;
    return hle_root(L, "sceVrTrackerInit", &cache, v, &rc) ? rc : v_vrt_init();
}

static s32 v_vrt_term(void)
{
    if (!g_vrt_init)
        return E_VRT(0x01); /* NOT_INIT */
    g_vrt_init = 0;
    memset((void *)g_vrt_dev, 0, sizeof(g_vrt_dev));
    return 0;
}

static s32 v_vrt_register(s32 type, s32 handle)
{
    if (type < 0 || type > 3)
        return E_VRT(0x06);
    if (!__sync_bool_compare_and_swap(&g_vrt_dev[type], 0, handle + 1))
        return E_VRT(0x04); /* that type is registered */
    return 0;
}

static s32 v_vrt_unregister(s32 handle)
{
    for (u32 i = 0; i < 4; i++)
        __sync_bool_compare_and_swap(&g_vrt_dev[i], handle + 1, 0);
    return 0;
}

static s32 v_vrt_result(const u32 *param, u8 *data)
{
    if (!param || !data)
        return E_VRT(0x06);
    memset(data, 0, 0x38);
    *(u32 *)(data + 0x00) = param[1]; /* the device handle */
    *(u32 *)(data + 0x34) = 2;        /* NOT_TRACKING; connected (+0x04) stays 0 */
    return 0;
}

static s32 v_vrt_time(u64 *t)
{
    if (!t)
        return E_VRT(0x06);
    *t = hle_time_us();
    return 0;
}

/* ---- Usbd (§20): no devices ---- */

static void *const g_usb_none[1]; /* an empty device list */

static s64 v_usb_list(void *const **list)
{
    if (list)
        *list = g_usb_none;
    return 0;
}

static s32 v_usb_events_timeout(const s64 *tv)
{
    /* a polling loop must not spin: wait as asked, up to a second */
    if (tv && tv[0] >= 0 && tv[1] >= 0) {
        u64 us = (u64)tv[0] * 1000000 + (u64)tv[1];
        K.usleep(us > 1000000 ? 1000000 : (u32)us);
    }
    return 0;
}

HLE_ROOT(sceUsbdInit, LIB(USB), v_ok)

/* ---- Keyboard / Mouse (§21): never connected ---- */

static volatile u8 g_kb_open[2], g_mouse_init, g_mouse_open[2];

static s32 v_kb_init(void) { return 0; }

static s32 v_kb_open(s32 user, s32 type, s32 index, const void *param)
{
    if (index < 0 || index > 1)
        return (s32)0x80DA0001; /* INVALID_ARG */
    g_kb_open[index] = 1;
    return 0x6B000001 + index;
}

static s32 kb_index(s32 h) { return h >= 0x6B000001 && h <= 0x6B000002 && g_kb_open[h - 0x6B000001] ? h - 0x6B000001 : -1; }

static s32 v_kb_read_state(s32 h, void *data)
{
    if (kb_index(h) < 0)
        return (s32)0x80DA0003; /* INVALID_HANDLE */
    if (!data)
        return (s32)0x80DA0001;
    memset(data, 0, 0x60); /* no keys */
    return 0;
}

static s32 v_kb_read(s32 h, void *data, s32 num) { return kb_index(h) < 0 ? (s32)0x80DA0003 : 0; /* no records */ }

static s32 v_kb_close(s32 h)
{
    s32 i = kb_index(h);
    if (i >= 0)
        g_kb_open[i] = 0;
    return 0;
}

HLE_ROOT(sceKeyboardInit, LIB(KB), v_kb_init)

static s32 v_mouse_init(void)
{
    g_mouse_init = 1;
    return 0;
}

static s32 v_mouse_open(s32 user, s32 type, s32 index, const void *param)
{
    if (!g_mouse_init)
        return (s32)0x80DF0005; /* NOT_INITIALIZED */
    if (type != 0 || index < 0 || index > 1)
        return (s32)0x80DF0001; /* INVALID_ARG */
    if (!__sync_bool_compare_and_swap(&g_mouse_open[index], 0, 1))
        return (s32)0x80DF0004; /* ALREADY_OPENED */
    return 0x6D000001 + index;
}

static s32 mouse_index(s32 h)
{
    return h >= 0x6D000001 && h <= 0x6D000002 && g_mouse_open[h - 0x6D000001] ? h - 0x6D000001 : -1;
}

struct mouse_data {
    u64 timestamp;
    u8 connected, pad[3];
    u32 buttons;
    s32 x, y, wheel, tilt;
    u8 reserve[8];
};
_Static_assert(sizeof(struct mouse_data) == 0x28, "SceMouseData");

static s32 v_mouse_read(s32 h, struct mouse_data *data, s32 num)
{
    if (!data || num < 1 || num > 64)
        return (s32)0x80DF0001;
    if (mouse_index(h) < 0)
        return (s32)0x80DF0003; /* INVALID_HANDLE */
    memset(data, 0, sizeof(*data));
    data->timestamp = hle_time_us(); /* one record: not connected */
    return 1;
}

static s32 v_mouse_close(s32 h)
{
    s32 i = mouse_index(h);
    if (i >= 0)
        g_mouse_open[i] = 0;
    return 0;
}

HLE_ROOT(sceMouseInit, LIB(MOUSE), v_mouse_init)

/* ---- SystemGesture (§22): no gestures ---- */

HLE_ROOT(sceSystemGestureInitializePrimitiveTouchRecognizer, LIB(SG), v_ok)

/* ---- the routed functions: X(lib, name, flags, virtual answer, virtual body) ---- */

#define CAM_CRASH RF_CRASH
#define REPROJ (RF_CRASH | RF_READY)
#define PERIPH_ROUTES(X)                                                                          \
    X(CAM, sceCameraIsAttached, 0, 0, v_cam_attached)                                             \
    X(CAM, sceCameraClose, CAM_CRASH, 0, v_cam_close)                                             \
    X(CAM, sceCameraCloseByHandle, CAM_CRASH, 0, v_cam_close)                                     \
    X(CAM, sceCameraGetDeviceInfo, CAM_CRASH, 0, v_cam_device_info)                               \
    X(CAM, sceCameraGetDeviceConfig, CAM_CRASH, 0, v_cam_config_h)                                \
    X(CAM, sceCameraGetDeviceConfigWithoutHandle, CAM_CRASH, 0, v_cam_config)                     \
    X(CAM, sceCameraGetConfig, CAM_CRASH, 0, v_cam_config_h)                                      \
    X(CAM, sceCameraSetConfig, CAM_CRASH, 0x802E0010, 0)                                          \
    X(CAM, sceCameraStart, CAM_CRASH, 0, v_cam_start)                                             \
    X(CAM, sceCameraStartByHandle, CAM_CRASH, 0, v_cam_start)                                     \
    X(CAM, sceCameraStop, CAM_CRASH, 0, v_cam_stop)                                               \
    X(CAM, sceCameraStopByHandle, CAM_CRASH, 0, v_cam_stop)                                       \
    X(CAM, sceCameraGetFrameData, CAM_CRASH, 0, v_cam_frame)                                      \
    X(CAM, sceCameraIsValidFrameData, CAM_CRASH, 0x802E0006, 0)                                   \
    X(CAM, sceCameraGetAttribute, CAM_CRASH, 0x802E0010, 0)                                       \
    X(CAM, sceCameraSetAttribute, CAM_CRASH, 0x802E0010, 0)                                       \
    X(CAM, sceCameraGetExposureGain, CAM_CRASH, 0x802E0010, 0)                                    \
    X(CAM, sceCameraSetExposureGain, CAM_CRASH, 0x802E0010, 0)                                    \
    X(CAM, sceCameraGetWhiteBalance, CAM_CRASH, 0x802E0010, 0)                                    \
    X(CAM, sceCameraSetWhiteBalance, CAM_CRASH, 0x802E0010, 0)                                    \
    X(CAM, sceCameraGetGamma, CAM_CRASH, 0x802E0010, 0)                                           \
    X(CAM, sceCameraSetGamma, CAM_CRASH, 0x802E0010, 0)                                           \
    X(CAM, sceCameraGetSaturation, CAM_CRASH, 0x802E0010, 0)                                      \
    X(CAM, sceCameraSetSaturation, CAM_CRASH, 0x802E0010, 0)                                      \
    X(CAM, sceCameraGetContrast, CAM_CRASH, 0x802E0010, 0)                                        \
    X(CAM, sceCameraSetContrast, CAM_CRASH, 0x802E0010, 0)                                        \
    X(CAM, sceCameraGetSharpness, CAM_CRASH, 0x802E0010, 0)                                       \
    X(CAM, sceCameraSetSharpness, CAM_CRASH, 0x802E0010, 0)                                       \
    X(CAM, sceCameraGetHue, CAM_CRASH, 0x802E0010, 0)                                             \
    X(CAM, sceCameraSetHue, CAM_CRASH, 0x802E0010, 0)                                             \
    X(CAM, sceCameraGetAutoExposureGain, CAM_CRASH, 0x802E0010, 0)                                \
    X(CAM, sceCameraSetAutoExposureGain, CAM_CRASH, 0x802E0010, 0)                                \
    X(CAM, sceCameraGetAutoWhiteBalance, CAM_CRASH, 0x802E0010, 0)                                \
    X(CAM, sceCameraSetAutoWhiteBalance, CAM_CRASH, 0x802E0010, 0)                                \
    X(CAM, sceCameraGetLensCorrection, CAM_CRASH, 0x802E0010, 0)                                  \
    X(CAM, sceCameraSetLensCorrection, CAM_CRASH, 0x802E0010, 0)                                  \
    X(CAM, sceCameraGetDefectivePixelCancellation, CAM_CRASH, 0x802E0010, 0)                      \
    X(CAM, sceCameraSetDefectivePixelCancellation, CAM_CRASH, 0x802E0010, 0)                      \
    X(CAM, sceCameraSetVideoSync, CAM_CRASH, 0x802E0010, 0)                                       \
    X(CAM, sceCameraAudioOpen, CAM_CRASH, 0x802E0010, 0)                                          \
    X(CAM, sceCameraAudioClose, CAM_CRASH, 0x802E0004, 0)                                         \
    X(CAM, sceCameraAudioGetData, CAM_CRASH, 0x802E0004, 0)                                       \
    X(CAM, sceCameraAudioGetData2, CAM_CRASH, 0x802E0004, 0)                                      \
    X(CAM, sceCameraAudioReset, CAM_CRASH, 0x802E0004, 0)                                         \
    X(CAM, sceCameraGetCalibrationData, CAM_CRASH, 0x802E0010, 0)                                 \
    X(CAM, sceCameraGetDeviceIDWithoutOpen, CAM_CRASH, 0x802E0010, 0)                             \
    X(CAM, sceCameraGetProductInfo, CAM_CRASH, 0x802E0010, 0)                                     \
    X(CAM, sceCameraGetMmapConnectedCount, CAM_CRASH, 0, v_cam_mmap_count)                        \
    X(MOVE, sceMoveTerm, RF_MASKSTUB, 0, v_move_term)                                             \
    X(MOVE, sceMoveOpen, 0, 0, v_move_open)                                                       \
    X(MOVE, sceMoveClose, 0, 0, 0)                                                                \
    X(MOVE, sceMoveReadStateLatest, 0, 0, v_move_latest)                                          \
    X(MOVE, sceMoveReadStateRecent, 0, 0, v_move_recent)                                          \
    X(MOVE, sceMoveGetDeviceInfo, 0, 0, v_move_device_info)                                       \
    X(MOVE, sceMoveSetLightSphere, 0, 1, 0)                                                       \
    X(MOVE, sceMoveSetVibration, 0, 1, 0)                                                         \
    X(MOVE, sceMoveResetLightSphere, 0, 0, 0)                                                     \
    X(MOVE, sceMoveGetExtensionPortInfo, 0, 1, 0)                                                 \
    X(MOVE, sceMoveGetDeviceId, 0, 1, 0)                                                          \
    X(MOVE, sceMoveGetCalibrationInfo, 0, 1, 0)                                                   \
    X(MOVE, sceMoveGetBluetoothAddress, 0, 1, 0)                                                  \
    X(MOVE, sceMoveReadStateRecentForTracker, 0, 1, 0)                                            \
    X(MOVE, sceMoveSetLightSphereForTracker, 0, 1, 0)                                             \
    X(MOVE, sceMoveSetExtensionPortOutput, 0, 1, 0)                                               \
    X(MOVE, sceMoveIsValidHandle, 0, 0, v_move_valid)                                             \
    X(MOVE, sceMoveSetCalibrationMode, 0, 0, 0)                                                   \
    X(MOVE, sceMoveIsReproductionModel, 0, 0, 0)                                                  \
    X(MOVE, sceMoveWriteCalibrationInfo, 0, 0, 0)                                                 \
    X(MT, sceMoveTrackerGetWorkingMemorySize, 0, EUNAVAIL, 0)                                     \
    X(MT, sceMoveTrackerTerm, 0, 0, 0)                                                            \
    X(MT, sceMoveTrackerCameraUpdate, 0, 0x80EF0004, 0)                                           \
    X(MT, sceMoveTrackerControllersUpdate, 0, 0x80EF0004, 0)                                      \
    X(MT, sceMoveTrackerGetState, 0, 0x80EF0004, 0)                                               \
    X(MT, sceMoveTrackerCalibrateReset, 0, 0x80EF0004, 0)                                         \
    X(MT, sceMoveTrackerCalibrateYaw, 0, 0x80EF0004, 0)                                           \
    X(MT, sceMoveTrackerGetVersion, 0, 0x80EF0004, 0)                                             \
    X(MT, sceMoveTrackerCaptureInit, 0, 0x80EF0004, 0)                                            \
    X(MT, sceMoveTrackerCaptureTerm, 0, 0x80EF0004, 0)                                            \
    X(MT, sceMoveTrackerPlayStart, 0, 0x80EF0004, 0)                                              \
    X(MT, sceMoveTrackerPlayStep, 0, 0x80EF0004, 0)                                               \
    X(MT, sceMoveTrackerPlayGetImages, 0, 0x80EF0004, 0)                                          \
    X(MT, sceMoveTrackerPlayGetStates, 0, 0x80EF0004, 0)                                          \
    X(MT, sceMoveTrackerRecordStart, 0, 0x80EF0004, 0)                                            \
    X(MT, sceMoveTrackerRecordStop, 0, 0x80EF0004, 0)                                             \
    X(MT, sceMoveTrackerRecordSave, 0, 0x80EF0004, 0)                                             \
    X(HMD, sceHmdTerminate, RF_MASKSTUB, 0, v_hmd_term)                                           \
    X(HMD, sceHmdClose, 0, 0, v_hmd_close)                                                        \
    X(HMD, sceHmdGetFieldOfView, 0, 0x81110004, 0)                                                \
    X(HMD, sceHmdGet2DEyeOffset, 0, 0x81110004, 0)                                                \
    X(HMD, sceHmdGetInertialSensorData, 0, 0x81110004, 0)                                         \
    X(HMD, sceHmdGetEyeStatus, 0, 0x81110004, 0)                                                  \
    X(HMD, sceHmdGetDistortionWorkMemorySize, 0, 0x10000, 0)                                      \
    X(HMD, sceHmdGetDistortionWorkMemoryAlign, 0, 0x100, 0)                                       \
    X(HMD, sceHmdGetDistortionWorkMemorySizeFor2d, 0, 0x10000, 0)                                 \
    X(HMD, sceHmdGetDistortionWorkMemoryAlignFor2d, 0, 0x100, 0)                                  \
    X(HMD, sceHmdGetDistortionParams, RF_READY, 0x81110004, 0)                                    \
    X(HMD, sceHmdGetDistortionCorrectionCommand, RF_READY, 0x81110004, 0)                         \
    X(HMD, sceHmdGet2dVrCommand, RF_READY, 0x81110004, 0)                                         \
    X(HMD, sceHmdGetWideNearDistortionCorrectionCommand, RF_READY, 0x81110004, 0)                 \
    X(HMD, sceHmdGetDistortionMap, RF_READY, 0x81110004, 0)                                       \
    X(HMD, sceHmdDistortionInitialize, RF_READY, 0, 0)                                            \
    X(HMD, sceHmdDistortionGetWorkMemorySize, RF_READY, 0x10000, 0)                               \
    X(HMD, sceHmdDistortionGetWorkMemoryAlign, RF_READY, 0x100, 0)                                \
    X(HMD, sceHmdDistortionGetCorrectionCommand, RF_READY, 0x81110004, 0)                         \
    X(HMD, sceHmdDistortionGetWideNearCorrectionCommand, RF_READY, 0x81110004, 0)                 \
    X(HMD, sceHmdDistortionGetCompoundEyeCorrectionCommand, RF_READY, 0x81110004, 0)              \
    X(HMD, sceHmdDistortionGet2dVrCommand, RF_READY, 0x81110004, 0)                               \
    X(HMD, sceHmdDistortionSetOutputMinColor, RF_READY, 0, 0)                                     \
    X(HMD, sceHmdReprojectionQueryGarlicBuffSize, 0, 0x100000, 0)                                 \
    X(HMD, sceHmdReprojectionQueryGarlicBuffAlign, 0, 0x100, 0)                                   \
    X(HMD, sceHmdReprojectionQueryOnionBuffSize, 0, 0x810, 0)                                     \
    X(HMD, sceHmdReprojectionQueryOnionBuffAlign, 0, 0x100, 0)                                    \
    X(HMD, sceHmdReprojectionInitialize, REPROJ, 0, 0)                                            \
    X(HMD, sceHmdReprojectionFinalize, REPROJ, 0, 0)                                              \
    X(HMD, sceHmdReprojectionStart, REPROJ, 0, 0)                                                 \
    X(HMD, sceHmdReprojectionStartWithOverlay, REPROJ, 0, 0)                                      \
    X(HMD, sceHmdReprojectionStartWideNear, REPROJ, 0, 0)                                         \
    X(HMD, sceHmdReprojectionStartWideNearWithOverlay, REPROJ, 0, 0)                              \
    X(HMD, sceHmdReprojectionStart2dVr, REPROJ, 0, 0)                                             \
    X(HMD, sceHmdReprojectionStartMultilayer, REPROJ, 0, 0)                                       \
    X(HMD, sceHmdReprojectionStartMultilayer2, REPROJ, 0, 0)                                      \
    X(HMD, sceHmdReprojectionStop, REPROJ, 0, 0)                                                  \
    X(HMD, sceHmdReprojectionSetDisplayBuffers, REPROJ, 0, 0)                                     \
    X(HMD, sceHmdReprojectionUnsetDisplayBuffers, REPROJ, 0, 0)                                   \
    X(HMD, sceHmdReprojectionAddDisplayBuffer, REPROJ, 0, 0)                                      \
    X(HMD, sceHmdReprojectionSetCallback, REPROJ, 0, 0)                                           \
    X(HMD, sceHmdReprojectionUnsetCallback, REPROJ, 0, 0)                                         \
    X(HMD, sceHmdReprojectionSetUserEventStart, REPROJ, 0, 0)                                     \
    X(HMD, sceHmdReprojectionSetUserEventEnd, REPROJ, 0, 0)                                       \
    X(HMD, sceHmdReprojectionClearUserEventStart, REPROJ, 0, 0)                                   \
    X(HMD, sceHmdReprojectionClearUserEventEnd, REPROJ, 0, 0)                                     \
    X(HMD, sceHmdReprojectionSetOutputMinColor, REPROJ, 0, 0)                                     \
    X(VRT, sceVrTrackerTerm, 0, 0, v_vrt_term)                                                    \
    X(VRT, sceVrTrackerRegisterDevice, 0, 0, v_vrt_register)                                      \
    X(VRT, sceVrTrackerRegisterDevice2, 0, 0, v_vrt_register)                                     \
    X(VRT, sceVrTrackerRegisterDeviceInternal, 0, 0, v_vrt_register)                              \
    X(VRT, sceVrTrackerUnregisterDevice, 0, 0, v_vrt_unregister)                                  \
    X(VRT, sceVrTrackerGetResult, 0, 0, v_vrt_result)                                             \
    X(VRT, sceVrTrackerGetTime, 0, 0, v_vrt_time)                                                 \
    X(VRT, sceVrTrackerGpuSubmit, RF_CRASH, 0x81260814, 0)                                        \
    X(VRT, sceVrTrackerGpuWait, RF_CRASH, 0x81260814, 0)                                          \
    X(VRT, sceVrTrackerGpuWaitAndCpuProcess, RF_CRASH, 0x81260814, 0)                             \
    X(VRT, sceVrTrackerCpuProcess, 0, 0, 0)                                                       \
    X(VRT, sceVrTrackerNotifyEndOfCpuProcess, 0, 0, 0)                                            \
    X(VRT, sceVrTrackerResetAll, 0, 0, 0)                                                         \
    X(VRT, sceVrTrackerResetOrientationRelative, 0, 0, 0)                                         \
    X(VRT, sceVrTrackerRecalibrate, 0, 0, 0)                                                      \
    X(VRT, sceVrTrackerSetDurationUntilStatusNotTracking, 0, 0, 0)                                \
    X(VRT, sceVrTrackerSetExtendedMode, 0, 0, 0)                                                  \
    X(VRT, sceVrTrackerSetLEDBrightness, 0, 0, 0)                                                 \
    X(VRT, sceVrTrackerSetRestingMode, 0, 0, 0)                                                   \
    X(VRT, sceVrTrackerUpdateMotionSensorData, 0, 0, 0)                                           \
    X(VRT, sceVrTrackerGetPlayAreaWarningInfo, 0, EUNAVAIL, 0)                                    \
    X(VRT, sceVrTrackerSetDeviceRejection, 0, 0, 0)                                               \
    X(VRT, sceVrTrackerStartLiveCapture, 0, 0, 0)                                                 \
    X(VRT, sceVrTrackerStopLiveCapture, 0, 0, 0)                                                  \
    X(VRT, sceVrTrackerCpuPushMarker, 0, 0, 0)                                                    \
    X(VRT, sceVrTrackerCpuPopMarker, 0, 0, 0)                                                     \
    X(USB, sceUsbdExit, 0, 0, 0)                                                                  \
    X(USB, sceUsbdGetDeviceList, 0, 0, v_usb_list)                                                \
    X(USB, sceUsbdFreeDeviceList, 0, 0, 0)                                                        \
    X(USB, sceUsbdOpenDeviceWithVidPid, 0, 0, 0)                                                  \
    X(USB, sceUsbdOpen, 0, 0x80240004, 0)                                                         \
    X(USB, sceUsbdClose, 0, 0, 0)                                                                 \
    X(USB, sceUsbdGetDeviceDescriptor, 0, 0x80240004, 0)                                          \
    X(USB, sceUsbdGetConfigDescriptor, 0, 0x80240004, 0)                                          \
    X(USB, sceUsbdGetConfigDescriptorByValue, 0, 0x80240004, 0)                                   \
    X(USB, sceUsbdGetActiveConfigDescriptor, 0, 0x80240004, 0)                                    \
    X(USB, sceUsbdClaimInterface, 0, 0x80240004, 0)                                               \
    X(USB, sceUsbdReleaseInterface, 0, 0x80240004, 0)                                             \
    X(USB, sceUsbdBulkTransfer, 0, 0x80240004, 0)                                                 \
    X(USB, sceUsbdInterruptTransfer, 0, 0x80240004, 0)                                            \
    X(USB, sceUsbdControlTransfer, 0, 0x80240004, 0)                                              \
    X(USB, sceUsbdSubmitTransfer, 0, 0x80240004, 0)                                               \
    X(USB, sceUsbdCancelTransfer, 0, 0x80240004, 0)                                               \
    X(USB, sceUsbdCheckConnected, 0, 0x80240004, 0)                                               \
    X(USB, sceUsbdHandleEvents, 0, 0, 0)                                                          \
    X(USB, sceUsbdHandleEventsLocked, 0, 0, 0)                                                    \
    X(USB, sceUsbdHandleEventsTimeout, 0, 0, v_usb_events_timeout)                                \
    X(USB, sceUsbdWaitForEvent, 0, 0, 0)                                                          \
    X(USB, sceUsbdAllocTransfer, 0, 0, 0)                                                         \
    X(USB, sceUsbdFreeTransfer, 0, 0, 0)                                                          \
    X(USB, sceUsbdFillBulkTransfer, 0, 0, 0)                                                      \
    X(USB, sceUsbdFillInterruptTransfer, 0, 0, 0)                                                 \
    X(USB, sceUsbdFillControlTransfer, 0, 0, 0)                                                   \
    X(USB, sceUsbdFillControlSetup, 0, 0, 0)                                                      \
    X(USB, sceUsbdFillIsoTransfer, 0, 0, 0)                                                       \
    X(USB, sceUsbdRefDevice, 0, 0, 0)                                                             \
    X(USB, sceUsbdUnrefDevice, 0, 0, 0)                                                           \
    X(USB, sceUsbdLockEvents, 0, 0, 0)                                                            \
    X(USB, sceUsbdUnlockEvents, 0, 0, 0)                                                          \
    X(USB, sceUsbdTryLockEvents, 0, 0, 0)                                                         \
    X(USB, sceUsbdLockEventWaiters, 0, 0, 0)                                                      \
    X(USB, sceUsbdUnlockEventWaiters, 0, 0, 0)                                                    \
    X(KB, sceKeyboardOpen, 0, 0, v_kb_open)                                                       \
    X(KB, sceKeyboardClose, 0, 0, v_kb_close)                                                     \
    X(KB, sceKeyboardReadState, 0, 0, v_kb_read_state)                                            \
    X(KB, sceKeyboardRead, 0, 0, v_kb_read)                                                       \
    X(KB, sceKeyboardSetProcessFocus, 0, 0, 0)                                                    \
    X(KB, sceKeyboardGetConnection, 0, EUNAVAIL, 0)                                               \
    X(KB, sceKeyboardGetDeviceInfo, 0, EUNAVAIL, 0)                                               \
    X(KB, sceKeyboardGetKey2Char, 0, EUNAVAIL, 0)                                                 \
    X(MOUSE, sceMouseOpen, 0, 0, v_mouse_open)                                                    \
    X(MOUSE, sceMouseClose, 0, 0, v_mouse_close)                                                  \
    X(MOUSE, sceMouseRead, 0, 0, v_mouse_read)                                                    \
    X(MOUSE, sceMouseSetHandType, 0, 0, 0)                                                        \
    X(MOUSE, sceMouseSetPointerSpeed, 0, 0, 0)                                                    \
    X(MOUSE, sceMouseSetProcessFocus, 0, 0, 0)                                                    \
    X(MOUSE, sceMouseGetDeviceInfo, 0, EUNAVAIL, 0)                                               \
    X(SG, sceSystemGestureOpen, 0, 0x47000001, 0)                                                 \
    X(SG, sceSystemGestureClose, 0, 0, 0)                                                         \
    X(SG, sceSystemGestureFinalizePrimitiveTouchRecognizer, 0, 0, 0)                              \
    X(SG, sceSystemGestureCreateTouchRecognizer, 0, 0, 0)                                         \
    X(SG, sceSystemGestureAppendTouchRecognizer, 0, 0, 0)                                         \
    X(SG, sceSystemGestureRemoveTouchRecognizer, 0, 0, 0)                                         \
    X(SG, sceSystemGestureResetTouchRecognizer, 0, 0, 0)                                          \
    X(SG, sceSystemGestureResetPrimitiveTouchRecognizer, 0, 0, 0)                                 \
    X(SG, sceSystemGestureUpdateTouchRecognizerRectangle, 0, 0, 0)                                \
    X(SG, sceSystemGestureUpdatePrimitiveTouchRecognizer, 0, 0, 0)                                \
    X(SG, sceSystemGestureUpdateTouchRecognizer, 0, 0, 0)                                         \
    X(SG, sceSystemGestureUpdateAllTouchRecognizer, 0, 0, 0)                                      \
    X(SG, sceSystemGestureGetTouchEventsCount, 0, 0, 0)                                           \
    X(SG, sceSystemGestureGetPrimitiveTouchEventsCount, 0, 0, 0)                                  \
    X(SG, sceSystemGestureGetTouchEvents, 0, 0x80890005, 0)                                       \
    X(SG, sceSystemGestureGetPrimitiveTouchEvents, 0, 0x80890005, 0)                              \
    X(SG, sceSystemGestureGetTouchEventByIndex, 0, 0x80890005, 0)                                 \
    X(SG, sceSystemGestureGetPrimitiveTouchEventByIndex, 0, 0x80890005, 0)                        \
    X(SG, sceSystemGestureGetTouchEventByEventID, 0, 0x80890005, 0)                               \
    X(SG, sceSystemGestureGetPrimitiveTouchEventByPrimitiveID, 0, 0x80890005, 0)                  \
    X(SG, sceSystemGestureGetTouchRecognizerInformation, 0, 0x80890006, 0)

enum { PERIPH_ROUTES(HLE_ENUM_ROW) R_COUNT };
HLE_TRAMPOLINES(hle_periph_tramps, PERIPH_ROUTES);
extern const char hle_periph_tramps[] HIDDEN, hle_periph_tramps_end[] HIDDEN;

#define ENT(lib, name, flags, ret, virt) {N_##lib, #name, (void *)&hle_periph_tramps[16 * R_##name], NULL},
#define WRAP(lib, name) {N_##lib, #name, w_##name, NULL}
#define CONST(lib, name, fn) {N_##lib, #name, fn, NULL}

static const struct ext_hle_ent k_hle[] = {
    /* roots, and wrappers that look at the host's answer */
    CONST(CAM, sceCameraOpen, w_cam_open),
    WRAP(MOVE, sceMoveInit),
    WRAP(MT, sceMoveTrackerInit),
    CONST(HMD, sceHmdInitialize, w_hmd_init),
    CONST(HMD, sceHmdInitialize315, w_hmd_init315),
    CONST(HMD, sceHmdOpen, w_hmd_open),
    CONST(HMD, sceHmdGetDeviceInformation, w_hmd_info),
    CONST(HMD, sceHmdGetDeviceInformationByHandle, w_hmd_info_by_handle),
    CONST(VRT, sceVrTrackerQueryMemory, w_vrt_query),
    CONST(VRT, sceVrTrackerInit, w_vrt_init),
    WRAP(USB, sceUsbdInit),
    WRAP(KB, sceKeyboardInit),
    WRAP(MOUSE, sceMouseInit),
    WRAP(SG, sceSystemGestureInitializePrimitiveTouchRecognizer),
    /* never the host: system UI, a debug-only keyboard */
    CONST(HSD, sceHmdSetupDialogInitialize, r_0),
    CONST(HSD, sceHmdSetupDialogOpen, v_hsd_open),
    CONST(HSD, sceHmdSetupDialogClose, r_0),
    CONST(HSD, sceHmdSetupDialogTerminate, r_0),
    CONST(HSD, sceHmdSetupDialogUpdateStatus, r_finished),
    CONST(HSD, sceHmdSetupDialogGetStatus, r_finished),
    CONST(HSD, sceHmdSetupDialogGetResult, v_hsd_result),
    CONST(DKB, sceDbgKeyboardInit, v_kb_init),
    CONST(DKB, sceDbgKeyboardOpen, v_kb_open),
    CONST(DKB, sceDbgKeyboardClose, v_kb_close),
    CONST(DKB, sceDbgKeyboardRead, v_kb_read),
    CONST(DKB, sceDbgKeyboardReadState, v_kb_read_state),
    CONST(DKB, sceDbgKeyboardGetKey2Char, r_eunavail),
    /* routed: the host or the virtual answer, by the library's mode */
    PERIPH_ROUTES(ENT)
};

static const struct hle_route k_routes[R_COUNT] = {PERIPH_ROUTES(HLE_ROUTE_ROW)};

static struct hle_router g_router = {
    .tag = TAG,
    .libs = g_libs,
    .nlibs = NLIBS,
    .hle = k_hle,
    .nhle = ARRAY_SIZE(k_hle),
    .first = ARRAY_SIZE(k_hle) - R_COUNT,
    .routes = k_routes,
    .tramps = hle_periph_tramps,
    .tramps_end = hle_periph_tramps_end,
};

static int periph_init(void) { return hle_router_init(&g_router); }
static void periph_sysmodule(u32 id, s32 rc) { hle_router_sysmodule(&g_router, id, rc); }

EXT_FAMILY_HOOKS("peripheral", k_hle, k_pol, periph_init, periph_sysmodule);
