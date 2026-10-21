/* Online dialogs (dialogs.md §8.7-8.16): profile, sign-in, invitations,
 * store, browser, friend list, game custom data, the other *Dialog
 * libraries, and the store icon.  NP runs as SWRR's identity and PSN is
 * unreachable, so every one of them answers "user cancelled": the title then
 * stays offline instead of looping on its online flow.  A purchase or sign-in
 * never reaches SWRR's account.  Result layouts not yet probed (P-6) give
 * RESULT_NONE, so a zeroed result is never read as "OK". */
#include "hle_dialog.h"
DLG_MINSIZE_BEGIN

/* ---- libSceNpProfileDialog (CommonDialog client) ---- */
static struct dlg g_prof = DLG_INIT("NpProfile", DLG_NPPROFILE, dlg_errs_common, 1);
DLG_FNS(prof, g_prof)

/* ParamA (0x78, checked against Ghost): BaseParam, u64 size @30, u32 mode @38
 * (1 profile, 2 friend request, 3 block, 4 grief report), s32 userId @3C,
 * u64 targetAccountId @40, void *userData @50 */
static s32 hle_sceNpProfileDialogOpenA(const void *p)
{
    u64 ra = (u64)__builtin_return_address(0);
    s32 rc = dlg_check_open(&g_prof, p, 0x30, 8, 0x78, DLG_BASE);
    if (rc)
        return rc;
    char what[80];
    fmt(what, sizeof(what), "OpenA mode %u user %x account %lx", RD32(p, 0x38), RD32(p, 0x3c), RD64(p, 0x40));
    g_prof.user_data = RD64(p, 0x50);
    dlg_start(&g_prof, ANS_CANCEL, ra, what, NULL);
    return 0;
}

/* Param (0x80): ... s32 userId @3C, SceNpOnlineId targetOnlineId @40 (char[16]), userData @58 */
static s32 hle_sceNpProfileDialogOpen(const void *p)
{
    u64 ra = (u64)__builtin_return_address(0);
    s32 rc = dlg_check_open(&g_prof, p, 0x30, 8, 0x80, DLG_BASE);
    if (rc)
        return rc;
    char what[80], id[17];
    memcpy(id, (const u8 *)p + 0x40, 16);
    id[16] = 0;
    fmt(what, sizeof(what), "Open mode %u user %x online id %s", RD32(p, 0x38), RD32(p, 0x3c), id);
    g_prof.user_data = RD64(p, 0x58);
    dlg_start(&g_prof, ANS_CANCEL, ra, what, NULL);
    return 0;
}

/* Result (0x30): s32 result (an error code), u32 userAction, void *userData */
static s32 hle_sceNpProfileDialogGetResult(void *r)
{
    s32 rc = dlg_check_result(&g_prof, r);
    if (rc == 0) {
        WR32(r, 0, 0);
        WR32(r, 4, g_prof.result); /* USER_CANCELED */
        WR64(r, 8, g_prof.user_data);
    }
    return rc;
}

/* ---- libSceSigninDialog (0x8135 errors; layout inferred) ---- */
static struct dlg g_signin = DLG_INIT("Signin", DLG_SIGNIN, dlg_errs_signin, 1);
DLG_FNS(signin, g_signin)

static s32 hle_sceSigninDialogOpen(const void *p)
{
    u64 ra = (u64)__builtin_return_address(0);
    s32 rc = dlg_check_open(&g_signin, p, 0x30, 8, 0, DLG_BASE | DLG_LENIENT);
    if (rc)
        return rc;
    char what[48];
    fmt(what, sizeof(what), "Open size %lx user %x", RD64(p, 0x30), RD32(p, 0x38));
    dlg_start(&g_signin, ANS_CANCEL, ra, what, NULL);
    return 0;
}

/* Result (0x10): s32 result (0 OK, 1 USER_CANCELED), reserved */
static s32 hle_sceSigninDialogGetResult(void *r)
{
    s32 rc = dlg_check_result(&g_signin, r);
    if (rc == 0)
        WR32(r, 0, g_signin.result);
    return rc;
}

/* ---- libSceInvitationDialog (NpToolkit2 imports the A variants) ---- */
static struct dlg g_inv = DLG_INIT("Invitation", DLG_INVITATION, dlg_errs_common, 1);
DLG_FNS(inv, g_inv)

/* Param(A) (0x90): BaseParam, u32 size @30, s32 mode @34 (1 send, 2 receive),
 * s32 userId @38, void *callbackArg @40, const DataParam* @48 */
static s32 inv_open(const void *p, u64 ra, const char *name)
{
    s32 rc = dlg_check_open(&g_inv, p, 0x30, 4, 0x90, DLG_BASE);
    if (rc)
        return rc;
    u32 mode = RD32(p, 0x34);
    if ((mode != 1 && mode != 2) || (mode == 1 && !RDPTR(p, 0x48)))
        return dlg_fail(&g_inv, DLG_E_PARAM);
    char what[48];
    fmt(what, sizeof(what), "%s %s user %x", name, mode == 1 ? "SEND" : "RECV", RD32(p, 0x38));
    g_inv.user_data = RD64(p, 0x40);
    dlg_start(&g_inv, ANS_CANCEL, ra, what, NULL);
    return 0;
}

static s32 hle_sceInvitationDialogOpenA(const void *p)
{
    return inv_open(p, (u64)__builtin_return_address(0), "OpenA");
}

static s32 hle_sceInvitationDialogOpen(const void *p)
{
    return inv_open(p, (u64)__builtin_return_address(0), "Open");
}

/* Result(A) (0x38): void *callbackArg, s32 errorCode @8, s32 result @C,
 * UserList *sentUsers @10 (u32 count first) */
static s32 hle_sceInvitationDialogGetResult(void *r)
{
    s32 rc = dlg_check_result(&g_inv, r);
    if (rc == 0) {
        WR64(r, 0, g_inv.user_data);
        WR32(r, 8, 0);
        WR32(r, 0xc, g_inv.result);
        u8 *sent = *(u8 **)((u8 *)r + 0x10);
        if (sent)
            WR32(sent, 0, 0);
    }
    return rc;
}

/* ---- libSceNpCommerce: the store dialog and the PS Store icon ---- */
static struct dlg g_comm = DLG_INIT("NpCommerce", DLG_COMMERCE, dlg_errs_common, 1);
DLG_FNS(comm, g_comm)

/* Param (0x80): BaseParam, s32 size @30, s32 userId @34, s32 mode @38,
 * u32 serviceLabel @3C, const char *const *targets @40, u32 numTargets @48,
 * u64 features @50, void *userData @58 */
static s32 hle_sceNpCommerceDialogOpen(const void *p)
{
    static const char *const k_modes[] = {"CATEGORY", "PRODUCT", "PRODUCT_CODE", "CHECKOUT", "DOWNLOADLIST",
                                          "PLUS"};
    u64 ra = (u64)__builtin_return_address(0);
    s32 rc = dlg_check_open(&g_comm, p, 0x30, 4, 0x80, DLG_BASE);
    if (rc)
        return rc;
    u32 mode = RD32(p, 0x38), n = RD32(p, 0x48);
    if (mode >= ARRAY_SIZE(k_modes))
        return dlg_fail(&g_comm, DLG_E_PARAM);
    const char *const *targets = (const char *const *)RDPTR(p, 0x40);
    char what[120], first[48];
    dlg_text(first, sizeof(first), targets && n ? targets[0] : NULL);
    fmt(what, sizeof(what), "%s label %u, %u targets (%s)", k_modes[mode], RD32(p, 0x3c), n, first);
    g_comm.user_data = RD64(p, 0x58);
    dlg_start(&g_comm, ANS_CANCEL, ra, what, NULL); /* never a purchase under SWRR's account */
    return 0;
}

/* Result (0x30): s32 result (0 OK, 1 canceled, 2 purchased), u8 authorized @4, userData @8 */
static s32 hle_sceNpCommerceDialogGetResult(void *r)
{
    s32 rc = dlg_check_result(&g_comm, r);
    if (rc == 0) {
        WR32(r, 0, g_comm.result);
        *((u8 *)r + 4) = 0;
        WR64(r, 8, g_comm.user_data);
    }
    return rc;
}

/* the real icon would be a system overlay drawn for SWRR */
static s32 hle_store_icon(u32 arg)
{
    static u32 logged;
    if (!logged++)
        logf("dialog: PS Store icon calls are no-ops");
    return 0;
}

/* ---- libSceWebBrowserDialog (layout inferred: url @40) ---- */
static struct dlg g_web = DLG_INIT("WebBrowser", DLG_WEBBROWSER, dlg_errs_common, 1);
DLG_FNS(web, g_web)

static s32 hle_sceWebBrowserDialogOpen(const void *p)
{
    u64 ra = (u64)__builtin_return_address(0);
    s32 rc = dlg_check_open(&g_web, p, 0x30, 8, 0, DLG_BASE | DLG_LENIENT);
    if (rc)
        return rc;
    dlg_start(&g_web, ANS_CANCEL, ra, "Open", (const char *)RDPTR(p, 0x40));
    return 0;
}

static s32 hle_sceWebBrowserDialogOpenForPredeterminedContent(const void *p)
{
    return dlg_open_cancel(&g_web, p, (u64)__builtin_return_address(0), "OpenForPredeterminedContent");
}

static s32 hle_web_no_event(void *ev) { return DLG_E_RESULT_NONE; }
static s32 hle_web_ok(void) { return 0; }

/* ---- result layouts unknown (P-6): CANCEL, GetResult RESULT_NONE ---- */
static struct dlg g_friend = DLG_INIT("NpFriendList", DLG_FRIENDLIST, dlg_errs_common, 1);
DLG_FNS(friend, g_friend)
static struct dlg g_gcd = DLG_INIT("GameCustomData", DLG_GAMECUSTOMDATA, dlg_errs_common, 1);
DLG_FNS(gcd, g_gcd)
static struct dlg g_pinv = DLG_INIT("PlayerInvitation", DLG_PLAYERINVITATION, dlg_errs_common, 1);
DLG_FNS(pinv, g_pinv)

/* ---- other *Dialog libraries (§8.16): the same, by library ----
 * PlayGoDialog (hle_playgo.c) and HmdSetupDialog (hle_peripheral.c) belong
 * to other families. */
static struct dlg g_gen[] = {
    DLG_INIT("NpEula", DLG_GENERIC + 0, dlg_errs_common, 1),
    DLG_INIT("Login", DLG_GENERIC + 1, dlg_errs_common, 1),
    DLG_INIT("NetCtlAp", DLG_GENERIC + 2, dlg_errs_common, 1),
    DLG_INIT("UsbStorage", DLG_GENERIC + 3, dlg_errs_common, 1),
    DLG_INIT("VrService", DLG_GENERIC + 4, dlg_errs_common, 1),
    DLG_INIT("NpSnsFacebook", DLG_GENERIC + 5, dlg_errs_common, 1),
    DLG_INIT("NpSnsYouTube", DLG_GENERIC + 6, dlg_errs_common, 1),
    DLG_INIT("SocialScreen", DLG_GENERIC + 7, dlg_errs_common, 1),
};
DLG_FNS(eula, g_gen[0])
DLG_FNS(login, g_gen[1])
DLG_FNS(netap, g_gen[2])
DLG_FNS(usb, g_gen[3])
DLG_FNS(vrs, g_gen[4])
DLG_FNS(fb, g_gen[5])
DLG_FNS(yt, g_gen[6])
DLG_FNS(social, g_gen[7])

static s32 hle_eula_check_version(void) { return DLG_E_RESULT_NONE; }

/* ---- libSceScreenShot: passed through, except the overlay image, whose
 * path the system would resolve in SWRR's /app0 ---- */
static s32 hle_screenshot_overlay(const char *path, u32 x, u32 y)
{
    char t[124];
    dlg_text(t, sizeof(t), path);
    logf("sceScreenShotSetOverlayImage(%s) ignored", t);
    return 0;
}

#define SM(lib, pfx, fn) {lib, fn "Initialize", pfx##_init, NULL}, {lib, fn "UpdateStatus", pfx##_update, NULL},           \
                         {lib, fn "GetStatus", pfx##_update, NULL}, {lib, fn "Close", pfx##_close, NULL},                     \
                         {lib, fn "Terminate", pfx##_term, NULL}
#define GENERIC(lib, pfx, fn) SM(lib, pfx, fn), {lib, fn "Open", pfx##_open_cancel, NULL},                           \
                                  {lib, fn "GetResult", pfx##_result_none, NULL}

static const struct ext_hle_ent k_hle[] = {
    SM("libSceNpProfileDialog", prof, "sceNpProfileDialog"),
    {"libSceNpProfileDialog", "sceNpProfileDialogOpenA", hle_sceNpProfileDialogOpenA, NULL},
    {"libSceNpProfileDialog", "sceNpProfileDialogOpen", hle_sceNpProfileDialogOpen, NULL},
    {"libSceNpProfileDialog", "sceNpProfileDialogGetResult", hle_sceNpProfileDialogGetResult, NULL},

    SM("libSceSigninDialog", signin, "sceSigninDialog"),
    {"libSceSigninDialog", "sceSigninDialogOpen", hle_sceSigninDialogOpen, NULL},
    {"libSceSigninDialog", "sceSigninDialogGetResult", hle_sceSigninDialogGetResult, NULL},

    SM("libSceInvitationDialog", inv, "sceInvitationDialog"),
    {"libSceInvitationDialog", "sceInvitationDialogOpenA", hle_sceInvitationDialogOpenA, NULL},
    {"libSceInvitationDialog", "sceInvitationDialogOpen", hle_sceInvitationDialogOpen, NULL},
    {"libSceInvitationDialog", "sceInvitationDialogGetResultA", hle_sceInvitationDialogGetResult, NULL},
    {"libSceInvitationDialog", "sceInvitationDialogGetResult", hle_sceInvitationDialogGetResult, NULL},

    SM("libSceNpCommerce", comm, "sceNpCommerceDialog"),
    {"libSceNpCommerce", "sceNpCommerceDialogInitializeInternal", comm_init, NULL},
    {"libSceNpCommerce", "sceNpCommerceDialogOpen", hle_sceNpCommerceDialogOpen, NULL},
    {"libSceNpCommerce", "sceNpCommerceDialogGetResult", hle_sceNpCommerceDialogGetResult, NULL},
    {"libSceNpCommerce", "sceNpCommerceShowPsStoreIcon", hle_store_icon, NULL},
    {"libSceNpCommerce", "sceNpCommerceHidePsStoreIcon", hle_store_icon, NULL},
    {"libSceNpCommerce", "sceNpCommerceSetPsStoreIconLayout", hle_store_icon, NULL},

    SM("libSceWebBrowserDialog", web, "sceWebBrowserDialog"),
    {"libSceWebBrowserDialog", "sceWebBrowserDialogOpen", hle_sceWebBrowserDialogOpen, NULL},
    {"libSceWebBrowserDialog", "sceWebBrowserDialogGetResult", web_result_none, NULL},
    {"libSceWebBrowserDialog", "sceWebBrowserDialogOpenForPredeterminedContent",
     hle_sceWebBrowserDialogOpenForPredeterminedContent, NULL},
    {"libSceWebBrowserDialog", "sceWebBrowserDialogGetEvent", hle_web_no_event, NULL},
    {"libSceWebBrowserDialog", "sceWebBrowserDialogNavigate", hle_web_ok, NULL},
    {"libSceWebBrowserDialog", "sceWebBrowserDialogSetCookie", hle_web_ok, NULL},
    {"libSceWebBrowserDialog", "sceWebBrowserDialogResetCookie", hle_web_ok, NULL},
    {"libSceWebBrowserDialog", "sceWebBrowserDialogSetZoom", hle_web_ok, NULL},

    GENERIC("libSceNpFriendListDialog", friend, "sceNpFriendListDialog"),
    {"libSceNpFriendListDialog", "sceNpFriendListDialogOpenA", friend_open_cancel, NULL},
    {"libSceNpFriendListDialog", "sceNpFriendListDialogGetResultA", friend_result_none, NULL},
    GENERIC("libSceGameCustomDataDialog", gcd, "sceGameCustomDataDialog"),
    {"libSceGameCustomDataDialog", "sceGameCustomDataDialogOpenA", gcd_open_cancel, NULL},
    {"libSceGameCustomDataDialog", "sceGameCustomDataDialogGetResultA", gcd_result_none, NULL},
    GENERIC("libScePlayerInvitationDialog", pinv, "scePlayerInvitationDialog"),

    GENERIC("libSceNpEulaDialog", eula, "sceNpEulaDialog"),
    {"libSceNpEulaDialog", "sceNpEulaDialogCheckVersion", hle_eula_check_version, NULL},
    GENERIC("libSceLoginDialog", login, "sceLoginDialog"),
    GENERIC("libSceNetCtlApDialog", netap, "sceNetCtlApDialog"),
    GENERIC("libSceUsbStorageDialog", usb, "sceUsbStorageDialog"),
    GENERIC("libSceVrServiceDialog", vrs, "sceVrServiceDialog"),
    GENERIC("libSceNpSnsFacebookDialog", fb, "sceNpSnsFacebookDialog"),
    GENERIC("libSceNpSnsYouTubeDialog", yt, "sceNpSnsYouTubeDialog"),
    GENERIC("libSceSocialScreenDialog", social, "sceSocialScreenDialog"),

    {"libSceScreenShot", "sceScreenShotSetOverlayImage", hle_screenshot_overlay, NULL},
    {"libSceScreenShot", "sceScreenShotSetOverlayImageWithOrigin", hle_screenshot_overlay, NULL},
};

#define NO_HOST(lib, aliases) {lib, aliases, EXT_LIB_NO_HOST, DLG_E_NOT_SUPPORTED}
static const struct ext_lib k_libs[] = {
    NO_HOST("libSceNpProfileDialog", "libSceNpProfileDialogCompat"),
    {"libSceSigninDialog", NULL, EXT_LIB_NO_HOST, (s32)0x81350005}, /* INVALID_STATE */
    NO_HOST("libSceInvitationDialog", "libSceInvitationDialogCompat"),
    NO_HOST("libSceNpCommerce", NULL),
    NO_HOST("libSceWebBrowserDialog", "libSceWebBrowserDialogLimited"),
    NO_HOST("libSceNpFriendListDialog", NULL),
    NO_HOST("libSceGameCustomDataDialog", NULL),
    NO_HOST("libScePlayerInvitationDialog", NULL),
    NO_HOST("libSceNpEulaDialog", NULL),
    NO_HOST("libSceLoginDialog", NULL),
    NO_HOST("libSceNetCtlApDialog", NULL),
    NO_HOST("libSceUsbStorageDialog", NULL),
    NO_HOST("libSceVrServiceDialog", NULL),
    NO_HOST("libSceNpSnsFacebookDialog", NULL),
    NO_HOST("libSceNpSnsYouTubeDialog", NULL),
    NO_HOST("libSceSocialScreenDialog", NULL),
    /* libSceScreenShot: no policy, the rest passes through */
};

DLG_MINSIZE_END

EXT_FAMILY_LIBS("dialog_online", k_hle, k_libs, NULL);
