/*
 * Runs the built ext.bin's media and peripheral families (hle_media.c,
 * hle_peripheral.c, hle_mode.c) against a fake host in an x86-64 process
 * (Rosetta on macOS), loaded through the core's real ext.c.  The fake host
 * answers like SWRR on 13.60 or lets everything through, depending on the
 * scenario; test_hle_media.py builds and drives it:
 *
 *     fam_host SCENARIO EXT_BIN CRC
 *     SCENARIO: swrr | allow | keeprp | gfs | sysmodule | scan
 *
 * Prints "fam_host: ok|FAILED ..." lines and the extension's log; exits 0
 * when every check of the scenario passed.
 */
#include "ghost_api.h"

void *sys_map(u64 size);
void sys_protect(void *addr, u64 size, int writable);
int sys_open(const char *path);
s64 sys_read(int fd, void *buf, u64 n);
int sys_close(int fd);
void sys_vlog(const char *fmt, __builtin_va_list ap);
void *sys_calloc(u64 n);
s64 sys_read_file(const char *path, char **out);
u64 sys_strtoul(const char *s);
int sys_vfmt(char *buf, u64 size, const char *fmt, __builtin_va_list ap);

struct kfuncs K;
struct boot_ctx *g_ctx;
struct module *g_modules, *g_main_mod;
u64 g_reported_dmem;
static u8 *g_small;
static u64 g_small_cur, g_small_end;
static const char *g_scenario;
static int g_failed;

void logf(const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    sys_vlog(f, ap);
    __builtin_va_end(ap);
}

static void expect(bool ok, const char *what)
{
    logf("fam_host: %s %s", ok ? "ok" : "FAILED", what);
    if (!ok)
        g_failed = 1;
}

static bool is(const char *s) { return strcmp(g_scenario, s) == 0; }

/* ---- what ext.c needs ---- */
void *arena_alloc(size_t n) { return sys_calloc(n); }
char *arena_strdup(const char *s)
{
    char *d = sys_calloc(strlen(s) + 1);
    memcpy(d, s, strlen(s));
    return d;
}
int fmt(char *buf, size_t size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = sys_vfmt(buf, size, f, ap);
    __builtin_va_end(ap);
    return n;
}
const char *strrchr_c(const char *s, int c)
{
    const char *r = NULL;
    for (; *s; s++)
        if (*s == c)
            r = s;
    return r;
}
void *page_alloc(size_t size) { return sys_map(ALIGN_UP(size, 0x4000)); }
void page_free(void *addr, size_t size) {}
u64 arena_small_alloc_top(u64 size, u64 keep)
{
    size = ALIGN_UP(size, 0x4000);
    if (g_small_end < g_small_cur || g_small_end - g_small_cur < keep + size)
        return 0;
    g_small_end -= size;
    return g_small_end;
}
int arena_write(u64 dst, const void *src, u64 size)
{
    u64 lo = ALIGN_DOWN(dst, 0x4000), hi = ALIGN_UP(dst + size, 0x4000);
    sys_protect((void *)lo, hi - lo, 1);
    memcpy((void *)dst, src, size);
    sys_protect((void *)lo, hi - lo, 0);
    return 0;
}
int arena_punch_rw(u64 addr, u64 size)
{
    sys_protect((void *)addr, size, 1);
    memset((void *)addr, 0, size);
    return 0;
}
/* ext.c's ext_abandon (a USB app root that stops the boot): not reached here */
void jit_block_destroy(struct jit_block *b) {}
int jit_block_create(struct jit_block *b, u64 size, u64 want_rx) { return -1; }
int jit_block_write(struct jit_block *b, u64 offset, const void *src, size_t len) { return -1; }
static int k_close(int fd) { return sys_close(fd); }
static ssize_t k_read(int fd, void *buf, size_t n) { return sys_read(fd, buf, n); }
static int k_map_flex(void **addr, size_t len, int prot, int flags)
{
    *addr = sys_map(len);
    return 0;
}
static int k_munmap(void *addr, size_t len) { return 0; }
static u32 g_usleeps;
static int k_usleep(u32 us)
{
    g_usleeps += us;
    return 0;
}

/* ---- the fake host: SWRR's process ---- */
static u32 g_calls[64]; /* host calls, by H_* */
enum {
    H_GLS_INIT, H_GLS_TERM, H_GLS_STATUS, H_GLS_PROBE, H_SU_INIT, H_SU_UPLOAD, H_SU_MENU, H_SP_INIT, H_SP_TERM,
    H_RP_INIT, H_RP_PROHIBIT, H_VR_OPEN, H_VR_START, H_A3_INIT, H_A3_OPEN, H_A3_OUT_OPEN, H_A3_TERM, H_SS_OVERLAY,
    H_SS_ENABLE, H_CE_INIT, H_CE_FILE, H_VQ_INIT, H_VQ_LOCAL, H_HMD_INIT, H_HMD_OPEN, H_HMD_INFO, H_HMD_REPROJ,
    H_CAM_ATTACHED, H_CAM_OPEN, H_USB_INIT, H_USB_LIST, H_MOUSE_INIT, H_KB_INIT, H_VRT_QUERY, H_SG_INIT, H_RZ,
    H_ANY, /* any other function of a loaded module */
};
static char g_overlay_path[256];

static s32 h_gls_init(u64 heap) { return g_calls[H_GLS_INIT]++, is("allow") ? 0 : (s32)0x80A00003; }
static s32 h_gls_term(void) { return g_calls[H_GLS_TERM]++, 0; }
static s32 h_gls_status(u8 *s)
{
    g_calls[H_GLS_STATUS]++;
    memset(s, 0xAB, 0x48);
    return 0;
}
/* every argument a routed function can get: six in registers, two in xmm
 * registers, two on the stack */
static s64 h_probe(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, double x, double y, u64 g, u64 h)
{
    g_calls[H_GLS_PROBE]++;
    return a == 1 && b == 2 && c == 3 && d == 4 && e == 5 && f == 6 && x == 1.5 && y == -2.25 && g == 7 && h == 8
               ? 0x600D
               : 0xBAD;
}
static s32 h_su_init(void) { return g_calls[H_SU_INIT]++, is("allow") ? 0 : (s32)0x81170105; }
static s32 h_su_upload(void) { return g_calls[H_SU_UPLOAD]++, (s32)0x81170102; }
static s32 h_su_menu(void) { return g_calls[H_SU_MENU]++, 0; }
static s32 h_sp_init(void) { return g_calls[H_SP_INIT]++, is("allow") ? 0 : (s32)0x810E0003; }
static s32 h_sp_term(void) { return g_calls[H_SP_TERM]++, 0; }
static s32 h_rp_init(void) { return g_calls[H_RP_INIT]++, (s32)0x80FC0003; }
static s32 h_rp_prohibit(void) { return g_calls[H_RP_PROHIBIT]++, 0; }
static s32 h_vr_open(void) { return g_calls[H_VR_OPEN]++, is("allow") ? 0 : (s32)0x80A80006; }
static s32 h_vr_start(void) { return g_calls[H_VR_START]++, 0; }
static s32 h_a3_init(void) { return g_calls[H_A3_INIT]++, 0; }
static s32 h_a3_open(u32 user, const void *params, u32 *port)
{
    g_calls[H_A3_OPEN]++;
    *port = 7;
    return 0;
}
static s32 h_a3_out_open(u32 port, u32 user, s32 type, s32 index, u32 len, u32 freq, u32 param)
{
    g_calls[H_A3_OUT_OPEN]++;
    return port == 7 && user == 0xFF && index == 0 && len == 256 && freq == 48000 && param == 0x55 ? 0x1234 : -1;
}
static s32 h_a3_term(void) { return g_calls[H_A3_TERM]++, 0; }
static s32 h_ss_overlay(const char *path, s32 x, s32 y)
{
    g_calls[H_SS_OVERLAY]++;
    strlcpy(g_overlay_path, path, sizeof(g_overlay_path));
    return x == 10 && y == 20 ? 0 : -1;
}
static s32 h_ss_enable(void) { return g_calls[H_SS_ENABLE]++, (s32)0x80BE0141; }
static s32 h_ce_init(void) { return g_calls[H_CE_INIT]++, (s32)0x809D3005; }
static s32 h_ce_file(void) { return g_calls[H_CE_FILE]++, 0; }
static s32 h_vq_init(void) { return g_calls[H_VQ_INIT]++, 0; }
static s32 h_vq_local(s32 *id) { return g_calls[H_VQ_LOCAL]++, *id = 0x77, 0; }
static s32 h_hmd_init(void) { return g_calls[H_HMD_INIT]++, 0; }
static s32 h_hmd_open(void) { return g_calls[H_HMD_OPEN]++, is("allow") ? 0x1234 : (s32)0x81110016; }
static s32 h_hmd_info(u32 *info)
{
    g_calls[H_HMD_INFO]++;
    info[0] = is("allow") ? 0 : 2; /* READY : NOT_DETECTED */
    return 0;
}
static s32 h_hmd_reproj(void) { return g_calls[H_HMD_REPROJ]++, 0; }
static s32 h_cam_attached(s32 index) { return g_calls[H_CAM_ATTACHED]++, 0; }
static s32 h_cam_open(void) { return g_calls[H_CAM_OPEN]++, 1; }
static s32 h_usb_init(void) { return g_calls[H_USB_INIT]++, is("allow") ? 0 : (s32)0x80240003; }
static s64 h_usb_list(void ***list) { return g_calls[H_USB_LIST]++, 0; }
static s32 h_mouse_init(void) { return g_calls[H_MOUSE_INIT]++, 0; }
static s32 h_kb_init(void) { return g_calls[H_KB_INIT]++, 0; }
static s32 h_vrt_query(void) { return g_calls[H_VRT_QUERY]++, 0; }
static s32 h_sg_init(void) { return g_calls[H_SG_INIT]++, 0; }
static s32 h_rz(void) { return g_calls[H_RZ]++, 0; }
static s32 h_any(void) { return g_calls[H_ANY]++, 0; }
static u64 h_process_time(void) { return 123456789; }

struct host_fn {
    const char *module, *name;
    void *fn;
};
static const struct host_fn k_host[] = {
    {"libSceGameLiveStreaming", "sceGameLiveStreamingInitialize", h_gls_init},
    {"libSceGameLiveStreaming", "sceGameLiveStreamingTerminate", h_gls_term},
    {"libSceGameLiveStreaming", "sceGameLiveStreamingGetCurrentStatus", h_gls_status},
    {"libSceGameLiveStreaming", "sceGameLiveStreamingSetMaxBitrate", h_probe},
    {"libSceShareUtility", "sceShareUtilityInitializeEx2", h_su_init},
    {"libSceShareUtility", "sceShareUtilityAdvanceInitialize", h_su_init},
    {"libSceShareUtility", "sceShareUtilityAdvanceSetUploadContentData", h_su_upload},
    {"libSceShareUtility", "sceShareUtilityOpenShareMenuDefault", h_su_menu},
    {"libSceSharePlay", "sceSharePlayInitialize", h_sp_init},
    {"libSceSharePlay", "sceSharePlayTerminate", h_sp_term},
    {"libSceRemoteplay", "sceRemoteplayInitialize", h_rp_init},
    {"libSceRemoteplay", "sceRemoteplayProhibit", h_rp_prohibit},
    {"libSceVideoRecording", "sceVideoRecordingOpen2", h_vr_open},
    {"libSceVideoRecording", "sceVideoRecordingStart", h_vr_start},
    {"libSceAudio3d", "sceAudio3dInitialize", h_a3_init},
    {"libSceAudio3d", "sceAudio3dPortOpen", h_a3_open},
    {"libSceAudio3d", "sceAudio3dAudioOutOpen", h_a3_out_open},
    {"libSceAudio3d", "sceAudio3dTerminate", h_a3_term},
    {"libSceScreenShot", "sceScreenShotSetOverlayImage", h_ss_overlay},
    {"libSceScreenShot", "sceScreenShotEnable", h_ss_enable},
    {"libSceContentExport", "sceContentExportInit", h_ce_init},
    {"libSceContentExport", "sceContentExportFromFile", h_ce_file},
    {"libSceVoiceQoS", "sceVoiceQoSInit", h_vq_init},
    {"libSceVoiceQoS", "sceVoiceQoSCreateLocalEndpoint", h_vq_local},
    {"libSceHmd", "sceHmdInitialize", h_hmd_init},
    {"libSceHmd", "sceHmdOpen", h_hmd_open},
    {"libSceHmd", "sceHmdGetDeviceInformation", h_hmd_info},
    {"libSceHmd", "sceHmdReprojectionStart", h_hmd_reproj},
    {"libSceCamera", "sceCameraIsAttached", h_cam_attached},
    {"libSceCamera", "sceCameraOpen", h_cam_open},
    {"libSceUsbd", "sceUsbdInit", h_usb_init},
    {"libSceUsbd", "sceUsbdGetDeviceList", h_usb_list},
    {"libSceMouse", "sceMouseInit", h_mouse_init},
    {"libSceKeyboard", "sceKeyboardInit", h_kb_init},
    {"libSceVrTracker", "sceVrTrackerQueryMemory", h_vrt_query},
    {"libSceSystemGesture", "sceSystemGestureInitializePrimitiveTouchRecognizer", h_sg_init},
    {"libSceRazorCpu", "sceRazorCpuPushMarker", h_rz},
    {"libkernel", "sceKernelGetProcessTime", h_process_time},
};
/* modules that do not load in SWRR (a PS5 build, or absent) */
static bool module_loads(const char *m)
{
    if (strcmp(m, "libSceMove") == 0)
        return false;
    if (is("scan") && strcmp(m, "libSceKeyboard") == 0)
        return false;
    return true;
}

static const char *g_modname[64];
static u32 g_nmod;
int host_module_handle(const char *modname, u32 *out)
{
    if (!modname || !module_loads(modname))
        return -1;
    for (u32 i = 0; i < g_nmod; i++)
        if (strcmp(g_modname[i], modname) == 0)
            return *out = i + 1, 0;
    g_modname[g_nmod] = arena_strdup(modname);
    *out = ++g_nmod;
    return 0;
}
void *host_dlsym(u32 handle, const char *name)
{
    if (!handle || handle > g_nmod)
        return NULL;
    for (u32 i = 0; i < ARRAY_SIZE(k_host); i++)
        if (strcmp(k_host[i].module, g_modname[handle - 1]) == 0 && strcmp(k_host[i].name, name) == 0)
            return k_host[i].fn;
    /* the rest of the library: present, and answers 0 */
    return strcmp(g_modname[handle - 1], "libkernel") ? (void *)h_any : NULL;
}
void *host_real_in(const char *modname, const char *name)
{
    u32 h;
    return host_module_handle(modname, &h) == 0 ? host_dlsym(h, name) : NULL;
}
void *host_real(const char *name) { return host_real_in("libkernel", name); }
static void fake_spin_lock(volatile int *l)
{
    while (__sync_lock_test_and_set(l, 1))
        ;
}
static void fake_spin_unlock(volatile int *l) { __sync_lock_release(l); }
static bool fake_gfs_active(void) { return is("gfs"); }
const char *remap_path(const char *path, char *buf, size_t size)
{
    if (!fake_gfs_active() && strncmp(path, "/app0/", 6) == 0) {
        fmt(buf, size, "/data/ghost/app0%s", path + 5);
        return buf;
    }
    return path;
}

const struct ghost_api g_ghost_api = {
    .version = GHOST_API_VERSION,
    .size = sizeof(struct ghost_api),
    .ctx = &g_ctx,
    .k = &K,
    .modules = &g_modules,
    .main_mod = &g_main_mod,
    .reported_dmem = &g_reported_dmem,
    .logf = logf,
    .host_real = host_real,
    .host_real_in = host_real_in,
    .host_dlsym = host_dlsym,
    .host_module_handle = host_module_handle,
    .nid_to_name = nid_to_name,
    .compute_nid = compute_nid,
    .page_alloc = page_alloc,
    .page_free = page_free,
    .arena_alloc = arena_alloc,
    .arena_strdup = arena_strdup,
    .spin_lock = fake_spin_lock,
    .spin_unlock = fake_spin_unlock,
    .remap_path = remap_path,
    .gfs_active = fake_gfs_active,
};

/* ---- calling the extension ---- */
typedef s64 (*fn_t)();
static fn_t fn(const char *lib, const char *name)
{
    char nid[12];
    compute_nid(name, nid);
    fn_t f = (fn_t)ext_hle_lookup(lib, nid, name);
    if (!f)
        logf("fam_host: FAILED no extension function %s:%s", lib, name), g_failed = 1;
    return f;
}
#define CALL(lib, name, ...) ((s32)fn(lib, name)(__VA_ARGS__))

/* ---- scenarios ---- */
static void media_swrr(void)
{
    /* GameLiveStreaming: SWRR's instance (INVALID_STATUS) is masked */
    expect(CALL("libSceGameLiveStreaming", "sceGameLiveStreamingInitialize", 0x10000) == 0 && g_calls[H_GLS_INIT] == 1,
           "GLS: SWRR's INVALID_STATUS from Initialize answers 0");
    expect(CALL("libSceGameLiveStreaming", "sceGameLiveStreamingTerminate") == 0 && !g_calls[H_GLS_TERM],
           "GLS: Terminate never reaches SWRR's instance");
    u8 st[0x50];
    memset(st, 0, sizeof(st));
    expect(CALL("libSceGameLiveStreamingCompat", "sceGameLiveStreamingGetCurrentStatus", st) == 0 && st[0] == 0xAB &&
               st[0x47] == 0xAB && st[0x48] == 0,
           "GLS: masked, other calls pass to the host (through the Compat alias)");
    s64 (*probe)(u64, u64, u64, u64, u64, u64, double, double, u64, u64) =
        (void *)fn("libSceGameLiveStreaming", "sceGameLiveStreamingSetMaxBitrate");
    expect(probe && probe(1, 2, 3, 4, 5, 6, 1.5, -2.25, 7, 8) == 0x600D && g_calls[H_GLS_PROBE] == 1,
           "routed call: the host gets register, xmm and stack arguments");
    expect(CALL("libSceGameLiveStreaming", "sceGameLiveStreamingLaunchLiveViewer") == 0,
           "GLS: the live viewer (system UI) is not opened");

    /* ShareUtility: refused by SWRR's param.sfo, so virtual */
    expect(CALL("libSceShareUtility", "sceShareUtilityInitializeEx2", 1, 2, 3, 4, 5, 6) == 0 && g_calls[H_SU_INIT] == 1,
           "ShareUtility: NOT_FLAG_IN_PARAM_SFO from InitializeEx2 answers 0");
    expect(CALL("libSceShareUtility", "sceShareUtilityAdvanceInitialize") == 0 && g_calls[H_SU_INIT] == 1,
           "ShareUtility: virtual, the second init stays away from the host");
    expect(CALL("libSceShareUtility", "sceShareUtilityAdvanceSetUploadContentData", 0) == 0 && !g_calls[H_SU_UPLOAD],
           "ShareUtility: virtual setter answers 0");
    expect(CALL("libSceShareUtility", "sceShareUtilityOpenShareMenuDefault") == 0 && !g_calls[H_SU_MENU],
           "ShareUtility: the share menu (system UI) is not opened");
    expect(CALL("libSceShareUtility", "sceShareUtilityAdvanceGetServiceInfo", 0) == (s32)0x81170304,
           "ShareUtility: virtual getter answers GET_SERVICE_INFO");

    /* SharePlay / Remoteplay: masked; Terminate stays with SWRR */
    expect(CALL("libSceSharePlay", "sceSharePlayInitialize", 0, 0) == 0 && g_calls[H_SP_INIT] == 1,
           "SharePlay: ALREADY_INITIALIZED answers 0");
    expect(CALL("libSceSharePlayCompat", "sceSharePlayTerminate") == 0 && !g_calls[H_SP_TERM],
           "SharePlay: Terminate never reaches SWRR's instance");
    expect(CALL("libSceRemoteplay", "sceRemoteplayInitialize", 0, 0) == 0, "Remoteplay: masked init answers 0");
    s32 prohibit = CALL("libSceRemoteplay", "sceRemoteplayProhibit");
    expect(prohibit == 0 && g_calls[H_RP_PROHIBIT] == (is("keeprp") ? 0u : 1u),
           is("keeprp") ? "Remoteplay: Prohibit is a no-op with the keep-Remote-Play flag"
                        : "Remoteplay: Prohibit reaches the host");
    s32 rp_status = 5;
    expect(CALL("libSceRemoteplay", "sceRemoteplayDisconnect") == 0, "Remoteplay: system-wide calls are no-ops");
    (void)rp_status;

    /* VideoRecording: SWRR holds the session (INVALID_STATE): a virtual one */
    expect(CALL("libSceVideoRecording", "sceVideoRecordingQueryMemSize2", 0) == 0, "VideoRecording: memory query passes");
    expect(CALL("libSceVideoRecording", "sceVideoRecordingOpen2", 0, 0x10000, 0) == 0 && g_calls[H_VR_OPEN] == 1,
           "VideoRecording: INVALID_STATE from Open2 gives a virtual session");
    expect(CALL("libSceVideoRecording", "sceVideoRecordingGetStatus") == 0 &&
               CALL("libSceVideoRecording", "sceVideoRecordingStart") == 0 && !g_calls[H_VR_START] &&
               CALL("libSceVideoRecording", "sceVideoRecordingGetStatus") == 1,
           "VideoRecording: the virtual session runs");
    expect(CALL("libSceVideoRecording", "sceVideoRecordingSetInfo", 0x6, "x", 2) == 0 &&
               CALL("libSceVideoRecording", "sceVideoRecordingSetInfo", 0x3, "x", 2) == (s32)0x80A80003 &&
               CALL("libSceVideoRecording", "sceVideoRecordingSetInfo", 0x6, NULL, 0) == (s32)0x80A80003,
           "VideoRecording: SetInfo checks the info id and value");
    expect(CALL("libSceVideoRecording", "sceVideoRecordingQueryMemSize2", 0) == 0x10000,
           "VideoRecording: virtual memory size is never 0");

    /* CompanionHttpd: SWRR's server, never reached */
    expect(CALL("libSceCompanionHttpd", "sceCompanionHttpdGetEvent", st) == (s32)0x80E40008 &&
               CALL("libSceCompanionHttpd", "sceCompanionHttpdStop") == 0,
           "CompanionHttpd: no events; Stop stays away from SWRR's server");
    const struct ext_lib *pol = ext_lib_policy("libSceCompanionHttpd");
    expect(pol && (pol->flags & EXT_LIB_NO_HOST), "CompanionHttpd: no host fallback");

    /* ScreenShot: NOT_SUPPORTED_PLATFORM from a setter answers 0 */
    expect(CALL("libSceScreenShot", "sceScreenShotEnable") == 0 && g_calls[H_SS_ENABLE] == 1,
           "ScreenShot: NOT_SUPPORTED_PLATFORM from a setter answers 0");
    s32 ov = CALL("libSceScreenShot", "sceScreenShotSetOverlayImage", "/app0/shot.png", 10, 20);
    if (is("gfs"))
        expect(ov == 0 && !g_calls[H_SS_OVERLAY], "ScreenShot: an overlay in ghostfs is not passed to the system");
    else
        expect(ov == 0 && strcmp(g_overlay_path, "/data/ghost/app0/shot.png") == 0,
               "ScreenShot: the overlay's /app0 path is remapped");

    /* ContentExport: MULTIPLEINIT masks; files under ghostfs are cancelled */
    expect(CALL("libSceContentExport", "sceContentExportInit", 0) == 0, "ContentExport: MULTIPLEINIT answers 0");
    s32 ce = CALL("libSceContentExport", "sceContentExportFromFile", 0, "/app0/a.mp4");
    expect(is("gfs") ? ce == (s32)0x809D3001 && !g_calls[H_CE_FILE] : ce == 0 && g_calls[H_CE_FILE] == 1,
           is("gfs") ? "ContentExport: a ghostfs file export is cancelled" : "ContentExport: file export passes");
    u32 any = g_calls[H_ANY];
    expect(CALL("libSceContentExport", "sceContentExportTerm") == 0 && g_calls[H_ANY] == any,
           "ContentExport: masked, Term never reaches the instance the title did not start");

    /* Voice: virtual ports (float arguments through the router) */
    u8 init_param[0x28] = {0}, port_param[0x18] = {0};
    expect(CALL("libSceVoice", "sceVoiceInit", init_param, 0) == 0 &&
               CALL("libSceVoice", "sceVoiceInit", init_param, 0) == (s32)0x804E0802,
           "Voice: virtual init, once");
    s32 in_port = -1, out_port = -1;
    *(u32 *)port_param = 0; /* IN_DEVICE */
    expect(CALL("libSceVoice", "sceVoiceCreatePort", &in_port, port_param) == 0 && in_port == 0, "Voice: port 0");
    *(u32 *)port_param = 4; /* OUT_VOICE */
    *(u32 *)(port_param + 0x0C) = 16000;
    expect(CALL("libSceVoice", "sceVoiceCreatePort", &out_port, port_param) == 0 && out_port == 1, "Voice: port 1");
    expect(CALL("libSceVoice", "sceVoiceConnectIPortToOPort", in_port, out_port) == 0 &&
               CALL("libSceVoice", "sceVoiceConnectIPortToOPort", out_port, in_port) == (s32)0x804E0807,
           "Voice: topology check");
    s32 (*set_volume)(u32, float) = (void *)fn("libSceVoice", "sceVoiceSetVolume");
    float vol = 0;
    u32 bitrate = 0, size = 99;
    expect(set_volume && set_volume((u32)out_port, 0.25f) == 0 &&
               CALL("libSceVoice", "sceVoiceGetVolume", out_port, &vol) == 0 && vol == 0.25f,
           "Voice: a float argument reaches the virtual body");
    expect(CALL("libSceVoice", "sceVoiceGetBitRate", out_port, &bitrate) == 0 && bitrate == 16000,
           "Voice: bitrate from the port's parameters");
    u8 out[16];
    expect(CALL("libSceVoice", "sceVoiceReadFromOPort", out_port, out, &size) == 0 && size == 0,
           "Voice: nothing to read");
    struct {
        s32 type, state;
        void *edge;
        u32 bytes, frame;
        u16 edges, reserved;
    } info;
    memset(&info, 0xEE, sizeof(info));
    expect(CALL("libSceVoice", "sceVoiceGetPortInfo", out_port, &info) == 0 && info.type == 4 && info.state == 0 &&
               !info.edge && info.frame == 1 && info.edges == 0,
           "Voice: port info");
    expect(CALL("libSceVoice", "sceVoiceDeletePort", 63) == (s32)0x804E0804 &&
               CALL("libSceVoice", "sceVoiceEnd") == 0 && CALL("libSceVoice", "sceVoiceEnd") == (s32)0x804E0801,
           "Voice: bad port, End once");

    /* VoiceQoS: the host runs it (Init succeeds) */
    s32 local = 0;
    expect(CALL("libSceVoiceQoS", "sceVoiceQoSInit", 0, 0, 0) == 0 &&
               CALL("libSceVoiceQoS", "sceVoiceQoSCreateLocalEndpoint", &local, 0, 0, 0) == 0 && local == 0x77,
           "VoiceQoS: passes to the host");

    /* Audio3d: the host; Terminate passes; PortOpen and a 7-argument call */
    u32 port = 0;
    expect(CALL("libSceAudio3d", "sceAudio3dInitialize", 0) == 0 && g_calls[H_A3_INIT] == 1, "Audio3d: host init");
    expect(CALL("libSceAudio3d", "sceAudio3dPortOpen", 0xFF, out, &port) == 0 && port == 7, "Audio3d: host port");
    s32 (*out_open)(u32, u32, s32, s32, u32, u32, u32) = (void *)fn("libSceAudio3d", "sceAudio3dAudioOutOpen");
    expect(out_open && out_open(7, 0xFF, 0, 0, 256, 48000, 0x55) == 0x1234,
           "Audio3d: the seventh argument (on the stack) reaches the host");
    u8 params[0x28];
    memset(params, 0xCC, sizeof(params));
    expect(CALL("libSceAudio3d", "sceAudio3dTerminate") == 0 && g_calls[H_A3_TERM] == 1, "Audio3d: Terminate passes");

    /* RazorCpu: no-ops, never the host */
    expect(CALL("libSceRazorCpu", "sceRazorCpuPushMarker", "x", 0, 0) == 0 && !g_calls[H_RZ],
           "RazorCpu: markers are no-ops");
}

static void media_allow(void)
{
    /* GLS/ShareUtility/SharePlay: the host runs them, nothing masked */
    expect(CALL("libSceGameLiveStreaming", "sceGameLiveStreamingInitialize", 0x10000) == 0, "GLS: host init");
    expect(CALL("libSceGameLiveStreaming", "sceGameLiveStreamingTerminate") == 0 && !g_calls[H_GLS_TERM],
           "GLS: Terminate is still not forwarded");
    expect(CALL("libSceShareUtility", "sceShareUtilityInitializeEx2") == 0 &&
               CALL("libSceShareUtility", "sceShareUtilityAdvanceSetUploadContentData", 0) == (s32)0x81170102 &&
               g_calls[H_SU_UPLOAD] == 1,
           "ShareUtility: with the share flag the host runs it");
    expect(CALL("libSceVideoRecording", "sceVideoRecordingOpen2", 0, 0, 0) == 0 &&
               CALL("libSceVideoRecording", "sceVideoRecordingStart") == 0 && g_calls[H_VR_START] == 1,
           "VideoRecording: the title's own session passes");
}

static void periph_common(void)
{
    /* Camera: Open is crash class, virtual until probed */
    expect(CALL("libSceCamera", "sceCameraIsAttached", 0) == 0 && g_calls[H_CAM_ATTACHED] == 1,
           "Camera: IsAttached asks the host before any Open");
    s32 h = CALL("libSceCamera", "sceCameraOpen", 0xFF, 0, 0, NULL);
    expect(h == 1 && !g_calls[H_CAM_OPEN], "Camera: Open answers a virtual handle (crash class)");
    expect(CALL("libSceCamera", "sceCameraIsAttached", 0) == 0 && g_calls[H_CAM_ATTACHED] == 1,
           "Camera: virtual after Open");
    u8 frame[0x248];
    memset(frame, 0, sizeof(frame));
    *(u32 *)frame = 0x248;
    expect(CALL("libSceCamera", "sceCameraGetFrameData", h, frame) == (s32)0x802E0006 &&
               *(u32 *)(frame + 0xE8) == 0xFFFFFFFF && *(u32 *)(frame + 0xEC) == 0xFFFFFFFF,
           "Camera: no frames (NOT_START), status marked");
    u8 start[0x18] = {0x18};
    expect(CALL("libSceCamera", "sceCameraStart", h, start) == (s32)0x802E0010, "Camera: Start says NOT_CONNECTED");
    expect(CALL("libSceCamera", "sceCameraClose", h) == 0 && CALL("libSceCamera", "sceCameraClose", h) ==
                                                                 (s32)0x802E0004,
           "Camera: Close, then NOT_OPEN");

    /* Move: the module does not load: virtual, no controller */
    expect(CALL("libSceMove", "sceMoveInit") == 0 && CALL("libSceMove", "sceMoveInit") == (s32)0x80EE0002,
           "Move: virtual init, once");
    s32 mh = CALL("libSceMove", "sceMoveOpen", 1, 0, 1);
    u8 data[0x48];
    memset(data, 0xEE, sizeof(data));
    expect(mh == 0x4D000200, "Move: handle for index 1");
    expect(CALL("libSceMove", "sceMoveReadStateLatest", mh, data) == 1 && data[0] == 0 && data[0x3F] == 0 &&
               data[0x40] == 0xEE,
           "Move: no controller connected (1), 0x40 bytes zeroed");

    /* Hmd: Initialize passes, Open is refused (no VR in SWRR's param.sfo) */
    u8 init_param[0x10] = {0};
    expect(CALL("libSceHmd", "sceHmdInitialize", init_param) == 0 && g_calls[H_HMD_INIT] == 1, "Hmd: host init");
    u32 dinfo[8];
    memset(dinfo, 0xEE, sizeof(dinfo));
    expect(CALL("libSceHmd", "sceHmdGetDeviceInformation", dinfo) == 0 && dinfo[0] == 2 && g_calls[H_HMD_INFO] == 1,
           "Hmd: the host's device state (NOT_DETECTED) reaches the title");
    s32 hh = CALL("libSceHmd", "sceHmdOpen", 1, 0, 0, NULL);
    expect(hh == 0x0F000000 && g_calls[H_HMD_OPEN] == 1, "Hmd: UNSUPPORTED_FEATURE from Open gives a virtual handle");
    memset(dinfo, 0xEE, sizeof(dinfo));
    expect(CALL("libSceHmd", "sceHmdGetDeviceInformation", dinfo) == 0 && dinfo[0] == 2 && dinfo[1] == 1 &&
               dinfo[7] == 0,
           "Hmd: virtual device information (NOT_DETECTED, the opener)");
    expect(CALL("libSceHmd", "sceHmdReprojectionStart") == 0 && !g_calls[H_HMD_REPROJ],
           "Hmd: reprojection never starts (crash class)");
    expect(CALL("libSceHmdDistortion", "sceHmdDistortionGetWorkMemorySize") == 0x10000,
           "Hmd: distortion through its library alias");
    expect(CALL("libSceHmdSetupDialog", "sceHmdSetupDialogOpen", NULL) == 0 &&
               CALL("libSceHmdSetupDialog", "sceHmdSetupDialogUpdateStatus") == 3,
           "HmdSetupDialog: never opened, FINISHED");

    /* VrTracker: virtual, minimal memory */
    u32 qp[16] = {0x40}, qr[16];
    memset(qr, 0xEE, sizeof(qr));
    expect(CALL("libSceVrTracker", "sceVrTrackerQueryMemory", qp, qr) == 0 && qr[0] == 0x40 && qr[1] == 0x10000 &&
               qr[3] == 0x10000 && qr[5] == 0x10000 && qr[2] == 0x10000 && qr[15] == 0 && !g_calls[H_VRT_QUERY],
           "VrTracker: tiny virtual memory sizes");
    qp[1] = 7;
    expect(CALL("libSceVrTracker", "sceVrTrackerQueryMemory", qp, qr) == (s32)0x81260806, "VrTracker: bad profile");
    expect(CALL("libSceVrTracker", "sceVrTrackerInit", qp) == 0 && CALL("libSceVrTracker", "sceVrTrackerInit", qp) ==
                                                                    (s32)0x81260802,
           "VrTracker: virtual init, once");
    expect(CALL("libSceVrTrackerFourDeviceAllowed", "sceVrTrackerRegisterDevice2", 0, 0x0F000000) == 0 &&
               CALL("libSceVrTracker", "sceVrTrackerRegisterDevice", 0, 0x0F000000) == (s32)0x81260804,
           "VrTracker: one device per type (through an alias)");
    u64 t = 0;
    expect(CALL("libSceVrTracker", "sceVrTrackerGetTime", &t) == 0 && t == 123456789, "VrTracker: process time");
    expect(CALL("libSceVrTracker", "sceVrTrackerGpuSubmit", NULL) == (s32)0x81260814, "VrTracker: no GPU work");

    /* Usbd: ACCESS from Init: virtual, no devices */
    void **list = (void **)1;
    expect(CALL("libSceUsbd", "sceUsbdInit") == 0 && g_calls[H_USB_INIT] == 1, "Usbd: ACCESS from Init answers 0");
    expect(fn("libSceUsbd", "sceUsbdGetDeviceList")(&list) == 0 && list && !list[0] && !g_calls[H_USB_LIST],
           "Usbd: an empty device list");
    expect(fn("libSceUsbd", "sceUsbdOpenDeviceWithVidPid")(0x54C, 0x5C4) == 0, "Usbd: no device to open");
    u64 tv[2] = {0, 2500};
    expect(CALL("libSceUsbd", "sceUsbdHandleEventsTimeout", tv) == 0 && g_usleeps == 2500,
           "Usbd: HandleEventsTimeout waits");

    /* Mouse: the host */
    expect(CALL("libSceMouse", "sceMouseInit") == (is("sysmodule") ? 0 : 0), "Mouse: init");
    expect(g_calls[H_MOUSE_INIT] == (is("sysmodule") ? 0u : 1u),
           is("sysmodule") ? "Mouse: its sysmodule failed: virtual, the host is not asked"
                           : "Mouse: Init reaches the host");
    if (is("sysmodule")) {
        s32 m = CALL("libSceMouse", "sceMouseOpen", 1, 0, 0, NULL);
        u8 md[0x28 * 2];
        memset(md, 0xEE, sizeof(md));
        expect(m == 0x6D000001 && CALL("libSceMouse", "sceMouseOpen", 1, 0, 0, NULL) == (s32)0x80DF0004,
               "Mouse: virtual handle, once per index");
        expect(CALL("libSceMouse", "sceMouseRead", m, md, 2) == 1 && *(u64 *)md == 123456789 && md[8] == 0 &&
                   md[0x27] == 0 && md[0x28] == 0xEE,
               "Mouse: one record, not connected");
    }

    /* Keyboard: the scan saw it cannot load */
    expect(CALL("libSceKeyboard", "sceKeyboardInit") == 0 && g_calls[H_KB_INIT] == (is("scan") ? 0u : 1u),
           is("scan") ? "Keyboard: virtual from boot (its module does not load)" : "Keyboard: Init reaches the host");

    /* SystemGesture */
    expect(CALL("libSceSystemGesture", "sceSystemGestureInitializePrimitiveTouchRecognizer", NULL) == 0 &&
               g_calls[H_SG_INIT] == 1,
           "SystemGesture: host init");
}

/* scenario scan: a module importing libSceKeyboard, whose module does not load */
static void make_title(void)
{
    static struct module m;
    static struct elf_sym syms[3];
    static char strtab[64];
    char nid[12];
    compute_nid("sceKeyboardInit", nid);
    fmt(strtab, sizeof(strtab), "x%s#B#B", nid);
    syms[1] = (struct elf_sym){.name = 1, .info = 0x12};
    m.name = "eboot.bin";
    m.symtab = syms;
    m.nsyms = 2;
    m.strtab = strtab;
    m.strsz = sizeof(strtab);
    m.import_libs[0] = (struct libref){1, "libSceKeyboard"};
    m.n_import_libs = 1;
    m.needed_mods[0] = (struct libref){1, "libSceKeyboard"};
    m.n_needed_mods = 1;
    g_modules = &m;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        logf("usage: fam_host SCENARIO EXT_BIN CRC");
        return 2;
    }
    g_scenario = argv[1];
    static struct boot_ctx ctx;
    ctx.magic = BOOT_MAGIC;
    ctx.size = sizeof(ctx);
    ctx.ext_crc = sys_strtoul(argv[3]);
    ctx.flags = is("keeprp") ? CTX_FLAG_KEEP_REMOTE_PLAY : 0;
    g_ctx = &ctx;
    K.read = k_read;
    K.close = k_close;
    K.map_flex = k_map_flex;
    K.munmap = k_munmap;
    K.usleep = k_usleep;
    char *blob;
    ctx.ext_size = (u64)sys_read_file(argv[2], &blob);
    ctx.ext_fd = (u64)sys_open(argv[2]);
    /* room for the whole extension: this harness checks its families, not placement */
    u64 small_size = 0x100000;
    g_small = sys_map(small_size);
    sys_protect(g_small, small_size, 0);
    g_small_cur = (u64)g_small, g_small_end = (u64)g_small + small_size;
    if (is("scan"))
        make_title();
    expect(ext_fetch() == 0 && ext_start() == 0, "extension loaded and self-tested");
    if (is("sysmodule")) {
        ext_note_sysmodule(0x00A9, (s32)0x80020008); /* libSceMouse: ENOEXEC */
        ext_note_sysmodule(0x00A9, 0);               /* (not a failure) */
    }
    ext_boot_done();
    if (is("allow"))
        media_allow();
    else
        media_swrr();
    if (!is("allow"))
        periph_common();
    return g_failed;
}
