/*
 * libSceSysmodule for the title (system-appcontent.md §3).  SWRR's own
 * libSceSysmodule resolves IsGame modules (NpToolkit2, ...: they live in the
 * title's /app0/sce_module) against SWRR's package and fails, and it has
 * nothing to unload that the title's imports do not still point at.
 *
 *   dialog id        the dialog families implement the library: 0, not
 *                    loaded (hle_dialog_sysmod.c)
 *   IsGame id        the title module ghostldr loaded (or loads now from
 *                    /app0/sce_module): started at its first load, 0
 *   bundled module   a module the title ships and ghostldr loaded: 0
 *   0xd7 DiscMap     0 without loading: titles never run from disc here
 *   anything else    SWRR's libSceSysmodule, with its real result.  A
 *                    failure goes to every family's sysmodule_failed hook
 *                    (ext_sysmodule_failed: media and peripheral switch the
 *                    library to their own implementation), and when a family
 *                    has a policy for the library the title gets 0: the
 *                    extension serves it
 *   Unload*          0; nothing is ever unloaded
 *
 * IsGame modules the boot loaded wait for this call to start (family init
 * marks them MOD_DEFERRED), as on hardware, unless ctx flag
 * CTX_FLAG_BOOT_START_ALL asks for the old start-everything-at-boot order.
 */
#include "kmod.h"
#include "hle_dialog.h"

#define SYSMOD_INVALID ((s32)0x805A1000)  /* SCE_SYSMODULE_ERROR_INVALID_VALUE */
#define SYSMOD_UNLOADED ((s32)0x805A1001) /* SCE_SYSMODULE_ERROR_UNLOADED */
#define ID_DISCMAP 0xd7

/* Public ids (12.52 table): u16 id little endian, a flag ('G' IsGame, 'D'
 * debug only, 'P' priv, '-'), the name without "libSce", NUL. */
static const char k_ids[] =
    "\x06\x00" "-Fiber\0" "\x07\x00" "-Ult\0" "\x0b\x00" "-Ngs2\0"
    "\x17\x00" "-Xml\0" "\x19\x00" "-NpUtility\0" "\x1a\x00" "-Voice\0"
    "\x1b\x00" "-VoiceQos\0" "\x1c\x00" "-NpMatching2\0" "\x1e\x00" "-NpScoreRanking\0"
    "\x21\x00" "-Rudp\0" "\x2c\x00" "-NpTus\0" "\x38\x00" "GFace\0"
    "\x39\x00" "GSmart\0" "\x80\x00" "-Json\0" "\x81\x00" "-GameLiveStreaming\0"
    "\x82\x00" "-CompanionUtil\0" "\x83\x00" "-PlayGo\0" "\x84\x00" "-Font\0"
    "\x85\x00" "-VideoRecording\0" "\x86\x00" "GS3DConversion\0" "\x88\x00" "-Audiodec\0"
    "\x8a\x00" "-JpegDec\0" "\x8b\x00" "-JpegEnc\0" "\x8c\x00" "-PngDec\0"
    "\x8d\x00" "-PngEnc\0" "\x8e\x00" "-Videodec\0" "\x8f\x00" "-Move\0"
    "\x91\x00" "-PadTracker\0" "\x92\x00" "-Depth\0" "\x93\x00" "GHand\0"
    "\x95\x00" "-Ime\0" "\x96\x00" "-ImeDialog\0" "\x97\x00" "-NpParty\0"
    "\x98\x00" "-FontFt\0" "\x99\x00" "-FreeTypeOt\0" "\x9a\x00" "-FreeTypeOl\0"
    "\x9b\x00" "-FreeTypeOptOl\0" "\x9c\x00" "-ScreenShot\0" "\x9d\x00" "-NpAuth\0"
    "\x9f\x00" "DSulpha\0" "\xa0\x00" "-SaveDataDialog\0" "\xa2\x00" "-InvitationDialog\0"
    "\xa3\x00" "DKeyboard\0" "\xa4\x00" "-MsgDialog\0" "\xa5\x00" "-AvPlayer\0"
    "\xa6\x00" "-ContentExport\0" "\xa7\x00" "-Audio3d\0" "\xa8\x00" "-NpCommerce\0"
    "\xa9\x00" "-Mouse\0" "\xaa\x00" "-CompanionHttpd\0" "\xab\x00" "-WebBrowserDialog\0"
    "\xac\x00" "-ErrorDialog\0" "\xad\x00" "-NpTrophy\0" "\xae\x00" "-VideoCoreInterface\0"
    "\xaf\x00" "-VideoCoreServerInterface\0" "\xb0\x00" "-NpSnsFacebookDialog\0" "\xb1\x00" "-MoveTracker\0"
    "\xb2\x00" "-NpProfileDialog\0" "\xb3\x00" "-NpFriendListDialog\0" "\xb4\x00" "-AppContent\0"
    "\xb5\x00" "-NpSignaling\0" "\xb6\x00" "-Remoteplay\0" "\xb7\x00" "-Usbd\0"
    "\xb8\x00" "-GameCustomDataDialog\0" "\xb9\x00" "-NpEulaDialog\0" "\xba\x00" "-Random\0"
    "\xbc\x00" "-M4aacEnc\0" "\xbd\x00" "-AudiodecCpu\0" "\xbe\x00" "-AudiodecCpuDdp\0"
    "\xc0\x00" "-AudiodecCpuM4aac\0" "\xc1\x00" "-Bemp2sys\0" "\xc2\x00" "-Beisobmf\0"
    "\xc3\x00" "-PlayReady\0" "\xc4\x00" "-VideoNativeExtEssential\0" "\xc5\x00" "-Zlib\0"
    "\xc6\x00" "-DtcpIp\0" "\xc7\x00" "-ContentSearch\0" "\xc8\x00" "-ShareUtility\0"
    "\xc9\x00" "-AudiodecCpuDtsHdLbr\0" "\xca\x00" "DDeci4h\0" "\xcb\x00" "GHeadTracker\0"
    "\xcc\x00" "-GameUpdate\0" "\xcd\x00" "-AutoMounterClient\0" "\xce\x00" "-SystemGesture\0"
    "\xcf\x00" "-Videodec2\0" "\xd0\x00" "-Vdecwrap\0" "\xd1\x00" "-At9Enc\0"
    "\xd2\x00" "-ConvertKeycode\0" "\xd3\x00" "-SharePlay\0" "\xd4\x00" "-Hmd\0"
    "\xd5\x00" "-UsbStorage\0" "\xd6\x00" "-UsbStorageDialog\0" "\xd7\x00" "-DiscMap\0"
    "\xd8\x00" "GFaceTracker\0" "\xd9\x00" "GHandTracker\0" "\xda\x00" "-NpSnsYouTubeDialog\0"
    "\xdc\x00" "-ProfileCacheExternal\0" "\xdd\x00" "-MusicPlayerService\0" "\xde\x00" "-SpSysCallWrapper\0"
    "\xdf\x00" "-Ps2EmuMenuDialog\0" "\xe0\x00" "-NpSnsDailyMotionDialog\0" "\xe1\x00" "-AudiodecCpuHevag\0"
    "\xe2\x00" "-LoginDialog\0" "\xe3\x00" "-LoginService\0" "\xe4\x00" "-SigninDialog\0"
    "\xe5\x00" "-Vdecsw\0" "\xe6\x00" "-CustomMusicCore\0" "\xe7\x00" "-Json2\0"
    "\xe8\x00" "GAudioLatencyEstimation\0" "\xe9\x00" "-WkFontConfig\0" "\xea\x00" "PVorbisDec\0"
    "\xeb\x00" "-HmdSetupDialog\0" "\xed\x00" "-VrTracker\0" "\xee\x00" "-ContentDelete\0"
    "\xef\x00" "-ImeBackend\0" "\xf0\x00" "-NetCtlApDialog\0" "\xf1\x00" "-PlayGoDialog\0"
    "\xf2\x00" "-SocialScreen\0" "\xf3\x00" "-EditMp4\0" "\xf5\x00" "-PsmKitSystem\0"
    "\xf6\x00" "-TextToSpeech\0" "\xf7\x00" "GNpToolkit\0" "\xf8\x00" "-CustomMusicService\0"
    "\xf9\x00" "-ClSysCallWrapper\0" "\xfa\x00" "-SystemLogger\0" "\xfb\x00" "-BluetoothHid\0"
    "\xfc\x00" "-VideoDecoderArbitration\0" "\xfd\x00" "-VrServiceDialog\0" "\xfe\x00" "GJobManager\0"
    "\xff\x00" "-ShareFactoryUtil\0" "\x00\x01" "-SocialScreenDialog\0" "\x01\x01" "-NpSnsDialog\0"
    "\x02\x01" "GNpToolkit2\0" "\x03\x01" "-SrcUtl\0" "\x04\x01" "-DiscId\0"
    "\x05\x01" "-NpUniversalDataSystem\0" "\x06\x01" "-Keyboard\0" "\x07\x01" "-Gic\0"
    "\x08\x01" "-PlayReady2\0" "\x09\x01" "DHdrScopes\0" "\x0c\x01" "-CesCs\0"
    "\x0d\x01" "-PlayerInvitationDialog\0" "\x12\x01" "-NpSessionSignaling\0" "\x13\x01" "-NpEntitlementAccess\0"
    "\x15\x01" "GNpCppWebApi\0" "\x16\x01" "-HubAppUtil\0" "\x1a\x01" "-NpPartner001\0"
    "\x2f\x01" "-FontGs\0" "\x35\x01" "GFontGsm\0" "\x38\x01" "-NpPartnerSubscription\0"
    "\x39\x01" "-NpAuthAuthorizedAppDialog\0";

/* the entry for id: [0..1] id, [2] flag, name from [3] */
static const char *id_entry(u32 id)
{
    for (const char *p = k_ids; p < k_ids + sizeof(k_ids) - 1; p += 3 + strlen(p + 3) + 1)
        if (((u8)p[0] | (u32)(u8)p[1] << 8) == id)
            return p;
    return NULL;
}

/* the entry for a module file "libSce<name>.prx" */
static const char *file_entry(const char *file)
{
    size_t n = strlen(file);
    if (n < 11 || strncmp(file, "libSce", 6) != 0 || strcmp(file + n - 4, ".prx") != 0)
        return NULL;
    for (const char *p = k_ids; p < k_ids + sizeof(k_ids) - 1; p += 3 + strlen(p + 3) + 1)
        if (strlen(p + 3) == n - 10 && strncmp(p + 3, file + 6, n - 10) == 0)
            return p;
    return NULL;
}

static u32 entry_id(const char *e) { return (u8)e[0] | (u32)(u8)e[1] << 8; }

bool sysmod_is_game(const char *file)
{
    const char *e = file_entry(file);
    return e && e[2] == 'G';
}

/* the title module for an entry, loading an IsGame one from sce_module if
 * the boot did not; NULL with *err set when an IsGame module is missing */
static struct module *title_module(const char *e, s32 *err)
{
    char file[48], path[64], vpath[512];
    fmt(file, sizeof(file), "libSce%s.prx", e + 3);
    struct module *m = mod_by_name(file);
    *err = 0;
    if (m || e[2] != 'G')
        return m;
    fmt(path, sizeof(path), "/app0/sce_module/%s", file);
    if (!title_vpath(path, vpath, sizeof(vpath))) {
        *err = E_NOENT; /* what the real library says when the file is missing */
        return NULL;
    }
    int rc = 0;
    m = module_load_runtime(vpath, &rc);
    *err = rc;
    return m;
}

static volatile u32 g_discmap_loads;

/* LoadModule, LoadModuleInternal, LoadModuleInternalWithArg */
static s32 route_load(const char *fn, u32 id, u64 argc, const void *argp, s32 *res, s32 (*real)(void),
                      u64 rarg2, u64 rarg3)
{
    if (res)
        *res = 0;
    if (!(id & 0x7fffffff))
        return SYSMOD_INVALID;
    if (dialog_sysmod_load(id)) /* internal ids (bit 31) never match */
        return 0;
    const char *e = id_entry(id);
    s32 err = 0;
    struct module *m = e ? title_module(e, &err) : NULL;
    if (m) {
        s32 rc = module_start(m, argc, argp);
        if (res)
            *res = rc;
        logf("%s(%x) -> 0 (%s, start %x)", fn, id, m->name, rc);
        return !res && rc < 0 ? E_INVAL : 0;
    }
    if (err) {
        logf("%s(%x) -> %x (libSce%s.prx)", fn, id, err, e + 3);
        return err;
    }
    if (id == ID_DISCMAP) {
        __atomic_fetch_add(&g_discmap_loads, 1, __ATOMIC_RELAXED);
        logf("%s(%x) -> 0 (DiscMap not loaded: answered by the extension)", fn, id);
        return 0;
    }
    s32 rc = real ? ((s32(*)(u32, u64, u64, u64, s32 *))real)(id, rarg2, rarg3, 0, res) : SYSMOD_INVALID;
    if (rc < 0) {
        ext_sysmodule_failed(id, rc);
        char lib[48];
        fmt(lib, sizeof(lib), "libSce%s", e ? e + 3 : "");
        if (e && ext_find_policy(lib)) {
            logf("%s(%x) -> %x: %s is served by the extension, 0", fn, id, rc, lib);
            if (res)
                *res = 0;
            return 0;
        }
    }
    logf("%s(%x) -> %x", fn, id, rc);
    return rc;
}

static s32 hle_load(u32 id)
{
    return route_load("sceSysmoduleLoadModule", id & 0xffff, 0, NULL, NULL, host_real("sceSysmoduleLoadModule"), 0,
                      0);
}

static s32 hle_load_internal(u32 id)
{
    return route_load("sceSysmoduleLoadModuleInternal", id, 0, NULL, NULL,
                      host_real("sceSysmoduleLoadModuleInternal"), 0, 0);
}

static s32 hle_load_internal_arg(u32 id, s32 argc, const void *argp, u64 flags, s32 *res)
{
    if (flags) {
        if (res)
            *res = 0;
        return SYSMOD_INVALID;
    }
    s32 local = 0;
    return route_load("sceSysmoduleLoadModuleInternalWithArg", id, (u64)argc, argp, res ? res : &local,
                      host_real("sceSysmoduleLoadModuleInternalWithArg"), (u64)argc, (u64)argp);
}

static s32 hle_unload(u32 id)
{
    logf("sceSysmoduleUnloadModule(%x) ignored", id);
    return 0;
}

static s32 hle_unload_arg(u32 id, s32 argc, const void *argp, u64 flags, s32 *res)
{
    if (res)
        *res = 0;
    return hle_unload(id);
}

static s32 is_loaded(u32 id, const char *real_name)
{
    if (!(id & 0x7fffffff))
        return SYSMOD_INVALID;
    const char *e = id_entry(id);
    char file[48];
    struct module *m = NULL;
    if (e) {
        fmt(file, sizeof(file), "libSce%s.prx", e + 3);
        m = mod_by_name(file);
    }
    if (dialog_sysmod_virtual(id))
        return dialog_sysmod_loaded(id) ? 0 : SYSMOD_UNLOADED;
    if (m || (e && e[2] == 'G'))
        return m && m->start_state == MOD_STARTED ? 0 : SYSMOD_UNLOADED;
    if (id == ID_DISCMAP)
        return g_discmap_loads ? 0 : SYSMOD_UNLOADED;
    s32 (*real)(u32) = host_real(real_name);
    return real ? real(id) : SYSMOD_INVALID;
}

static s32 hle_is_loaded(u32 id) { return is_loaded(id & 0xffff, "sceSysmoduleIsLoaded"); }
static s32 hle_is_loaded_internal(u32 id) { return is_loaded(id, "sceSysmoduleIsLoadedInternal"); }

static s32 hle_get_handle_internal(u32 id, s32 *handle)
{
    const char *e = id_entry(id);
    char file[48];
    if (e) {
        fmt(file, sizeof(file), "libSce%s.prx", e + 3);
        struct module *m = mod_by_name(file);
        if (m || e[2] == 'G') {
            if (!m || m->start_state != MOD_STARTED)
                return SYSMOD_UNLOADED;
            if (handle)
                *handle = (s32)m->fake_handle;
            return 0;
        }
    }
    s32 (*real)(u32, s32 *) = host_real("sceSysmoduleGetModuleHandleInternal");
    return real ? real(id, handle) : SYSMOD_INVALID;
}

static s32 hle_by_name_invalid(void)
{
    logf("sceSysmoduleLoadModuleByNameInternal: not supported");
    return SYSMOD_INVALID;
}

static s32 hle_ok(void) { return 0; }

/* IsGame modules the boot loaded start at the title's LoadModule */
static int sysmodule_init(void)
{
    if (g_ctx->flags & CTX_FLAG_BOOT_START_ALL)
        return 0;
    for (struct module *m = g_modules; m; m = m->next) {
        const char *e = m->is_main ? NULL : file_entry(m->name);
        if (e && e[2] == 'G' && m->start_state == MOD_BOOT) {
            m->start_state = MOD_DEFERRED;
            logf("sysmodule: %s starts at sceSysmoduleLoadModule(%x)", m->name, entry_id(e));
        }
    }
    return 0;
}

#define SM "libSceSysmodule"
static const struct ext_hle_ent k_hle[] = {
    {SM, "sceSysmoduleLoadModule", hle_load, NULL},
    {SM, "sceSysmoduleLoadModuleInternal", hle_load_internal, NULL},
    {SM, "sceSysmoduleLoadModuleInternalWithArg", hle_load_internal_arg, NULL},
    {SM, "sceSysmoduleUnloadModule", hle_unload, NULL},
    {SM, "sceSysmoduleUnloadModuleInternal", hle_unload, NULL},
    {SM, "sceSysmoduleUnloadModuleInternalWithArg", hle_unload_arg, NULL},
    {SM, "sceSysmoduleIsLoaded", hle_is_loaded, NULL},
    {SM, "sceSysmoduleIsLoadedInternal", hle_is_loaded_internal, NULL},
    {SM, "sceSysmoduleGetModuleHandleInternal", hle_get_handle_internal, NULL},
    {SM, "sceSysmoduleLoadModuleByNameInternal", hle_by_name_invalid, NULL},
    {SM, "sceSysmoduleUnloadModuleByNameInternal", hle_ok, NULL},
    {SM, "sceSysmodulePreloadModuleForLibkernel", hle_ok, NULL},
    {SM, "sceSysmoduleMapLibcForLibkernel", hle_ok, NULL},
};

EXT_FAMILY("sysmodule", k_hle, sysmodule_init);
