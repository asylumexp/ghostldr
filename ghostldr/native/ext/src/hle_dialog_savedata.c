/* libSceSaveDataDialog (dialogs.md §8.4), virtual only.  The real dialog
 * would list SWRR's saves (the Luac0re save, the install containers) and a
 * DELETE list could destroy them.  This one answers from the title's own
 * virtual save store (savedata_query in savedata_hooks.h, read-only), never
 * deletes anything, and always finishes: Ghost's save dialog helper ignores
 * the results of Initialize and Open and polls UpdateStatus until FINISHED
 * with no timeout. */
#include "hle_dialog.h"
#include "savedata_hooks.h"
DLG_MINSIZE_BEGIN

#define SD_ERR_NOT_FOUND ((s32)0x809F0008) /* SCE_SAVE_DATA_ERROR_NOT_FOUND */

/* Until the save data family (hle_savedata.c) is linked in: an empty store.
 * Its strong definition replaces this one at link time. */
__attribute__((weak)) s32 savedata_query(s32 user, const char *dir, struct sd_param *param, void *icon, void *info)
{
    return SD_ERR_NOT_FOUND;
}

static struct dlg g_sd = DLG_INIT("SaveData", DLG_SAVEDATA, dlg_errs_common, 1);
DLG_FNS(sd, g_sd)

enum { SD_LIST = 1, SD_USER_MSG, SD_SYSTEM_MSG, SD_ERROR_CODE, SD_PROGRESS_BAR };
enum { SD_SAVE = 1, SD_LOAD, SD_DELETE };

/* a fake store for the tests (libGhostldrDialog:ghostldrDialogTestSaves) */
static const char (*g_fake_names)[32];
static const s64 *g_fake_mtimes;
static u32 g_fake_n;

static s32 hle_ghostldrDialogTestSaves(const char (*names)[32], const s64 *mtimes, u32 n)
{
    g_fake_names = names, g_fake_mtimes = mtimes, g_fake_n = names ? n : 0;
    return 0;
}

/* Names the dialog never lists, whatever the store says: SWRR's own saves
 * (memory cards, the ghostfs containers <prefix>NNN, <prefix>SAVE,
 * <prefix>DNNNSAVE and GFSINDEX) and system names. */
static bool digits3(const char *s) { return s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9' && s[2] >= '0' && s[2] <= '9'; }

static bool sd_name_allowed(const char *dir)
{
    static const char *const k_deny[] = {"SLUS-20268", "SLES-50366", "GFSINDEX", "sce_"};
    if (!dir[0])
        return false;
    for (u32 i = 0; i < ARRAY_SIZE(k_deny); i++)
        if (strncmp(dir, k_deny[i], strlen(k_deny[i])) == 0)
            return false;
    const char *save = gfs_save_name ? gfs_save_name() : NULL; /* "<prefix>SAVE" */
    size_t n = save ? strlen(save) : 0;
    if (n > 4 && strncmp(dir, save, n - 4) == 0) {
        const char *rest = dir + n - 4;
        if ((digits3(rest) && !rest[3]) || strcmp(rest, "SAVE") == 0 ||
            (rest[0] == 'D' && digits3(rest + 1) && strcmp(rest + 4, "SAVE") == 0))
            return false;
    }
    return true;
}

static s32 sd_query(s32 user, const char *dir, struct sd_param *prm)
{
    if (!sd_name_allowed(dir))
        return SD_ERR_NOT_FOUND;
    if (!g_fake_names)
        return savedata_query(user, dir, prm, NULL, NULL);
    for (u32 i = 0; i < g_fake_n; i++)
        if (strncmp(g_fake_names[i], dir, 32) == 0) {
            if (prm) {
                memset(prm, 0, sizeof(*prm));
                fmt(prm->title, sizeof(prm->title), "fake %s", dir);
                prm->mtime = g_fake_mtimes ? g_fake_mtimes[i] : 0;
            }
            return 0;
        }
    return SD_ERR_NOT_FOUND;
}

/* LIST, SAVE or LOAD: the entry with focus among the ones the real dialog
 * would show (the new item first, then each of the title's names that
 * exists, in its order).  Returns the number shown; *dir gets the focused
 * name, "" for the new item. */
static u32 sd_pick(const u8 *items, u32 disp, char dir[32], u32 *exist_out, u32 *asked_out)
{
    s32 user = (s32)RD32(items, 0);
    const char (*names)[32] = (const char (*)[32])RDPTR(items, 0x10);
    u32 num = names ? RD32(items, 0x18) : 0, focus = RD32(items, 0x28);
    const char *focus_name = (const char *)RDPTR(items, 0x30);
    bool new_item = disp == SD_SAVE && RDPTR(items, 0x20);
    if (num > 1024) {
        logf("dialog: SaveData LIST with %u names; the first 1024 are used", num);
        num = 1024;
    }
    s32 first = -1, last = -1, latest = -1, oldest = -1, named = -1;
    s64 t_latest = 0, t_oldest = 0;
    u32 exist = 0;
    for (u32 i = 0; i < num; i++) {
        char name[33];
        memcpy(name, names[i], 32);
        name[32] = 0;
        struct sd_param prm;
        if (sd_query(user, name, &prm) != 0)
            continue;
        exist++;
        if (first < 0)
            first = (s32)i;
        last = (s32)i;
        if (latest < 0 || prm.mtime > t_latest)
            latest = (s32)i, t_latest = prm.mtime;
        if (oldest < 0 || prm.mtime < t_oldest)
            oldest = (s32)i, t_oldest = prm.mtime;
        if (focus_name && named < 0 && strncmp(name, focus_name, 32) == 0)
            named = (s32)i;
    }
    /* LISTHEAD 0, LISTTAIL 1, DATAHEAD 2, DATATAIL 3, DATALATEST 4, DATAOLDEST 5, DIRNAME 6 */
    s32 pick = focus == 0 ? (new_item ? -1 : first)
             : focus == 1 ? last
             : focus == 2 ? first
             : focus == 3 ? last
             : focus == 4 ? latest
             : focus == 5 ? oldest
             : focus == 6 ? named
             : -1;
    if (pick < 0) /* nothing matches: the first entry shown */
        pick = new_item ? -1 : first;
    dir[0] = 0;
    if (pick >= 0) {
        memcpy(dir, names[pick], 31);
        dir[31] = 0;
    }
    *exist_out = exist, *asked_out = num;
    return exist + new_item;
}

static const char *const k_sysmsg[] = {
    NULL, "NODATA", "CONFIRM", "OVERWRITE", "NOSPACE", "PROGRESS", "FILE_CORRUPTED", "FINISHED",
    "NOSPACE_CONTINUABLE", NULL, "CORRUPTED_AND_DELETED", "CORRUPTED_AND_CREATED", NULL, "CORRUPTED_AND_RESTORE",
    "TOTAL_SIZE_EXCEEDED",
};
static const char *const k_disp[] = {"?", "SAVE", "LOAD", "DELETE"};

/* Param (0x98): BaseParam, s32 size @30, u32 mode @34, u32 dispType @38,
 * AnimationParam* @40, Items* @48, UserMsgParam* @50, SysMsgParam* @58,
 * ErrorCodeParam* @60, ProgressBarParam* @68, void *userData @70,
 * OptionParam* @78 */
static s32 hle_sceSaveDataDialogOpen(const void *p)
{
    u64 ra = (u64)__builtin_return_address(0);
    s32 rc = dlg_check_open(&g_sd, p, 0x30, 4, 0x98, DLG_BASE);
    if (rc)
        return rc;
    u32 mode = RD32(p, 0x34), disp = RD32(p, 0x38);
    const u8 *items = RDPTR(p, 0x48);
    char what[120], dir[32] = "", about[32] = "";
    const char *text = NULL;
    u32 a;
    if (mode < SD_LIST || mode > SD_PROGRESS_BAR || disp > SD_DELETE)
        return dlg_fail(&g_sd, DLG_E_PARAM);
    const u8 *sub = mode == SD_LIST ? items : RDPTR(p, 0x50 + 8 * (mode - SD_USER_MSG));
    if (!sub)
        return dlg_fail(&g_sd, DLG_E_PARAM);
    if (mode == SD_LIST) {
        if (disp == SD_DELETE || RDPTR(items, 8)) { /* never delete; another title's data: none here */
            a = ANS_CANCEL;
            fmt(what, sizeof(what), "LIST %s%s", k_disp[disp], disp == SD_DELETE ? "" : " of another title");
        } else {
            u32 exist, asked;
            u32 shown = sd_pick(items, disp, dir, &exist, &asked);
            a = shown ? ANS_BTN1 : ANS_CANCEL; /* an empty list: "no data", backed out of */
            fmt(what, sizeof(what), "LIST %s: %u of %u exist%s, focus %u ->", k_disp[disp], exist, asked,
                disp == SD_SAVE && RDPTR(items, 0x20) ? " + new item" : "", RD32(items, 0x28));
            text = shown ? (dir[0] ? dir : "(new item)") : "(no data)";
        }
    } else if (mode == SD_USER_MSG) { /* u32 buttonType (0 OK, 1 YESNO, 2 NONE, 3 OKCANCEL), msgType, msg @8 */
        static const char *const k_btn[] = {"OK", "YESNO", "NONE", "OK_CANCEL"};
        u32 bt = RD32(sub, 0);
        if (bt >= ARRAY_SIZE(k_btn))
            return dlg_fail(&g_sd, DLG_E_PARAM);
        a = bt == 2 ? ANS_HOLD : ANS_BTN1;
        text = (const char *)RDPTR(sub, 8);
        fmt(what, sizeof(what), "USER_MSG %s%s", k_btn[bt], RD32(sub, 4) ? " (error)" : "");
    } else if (mode == SD_SYSTEM_MSG) { /* u32 sysMsgType, u64 value @8 */
        u32 t = RD32(sub, 0);
        u64 value = RD64(sub, 8);
        const char *name = t < ARRAY_SIZE(k_sysmsg) ? k_sysmsg[t] : NULL;
        a = t == 5 ? ANS_HOLD : t == 2 && disp == SD_DELETE ? ANS_BTN2 : ANS_BTN1; /* never confirm a delete */
        if (items && RDPTR(items, 0x10) && RD32(items, 0x18)) { /* the save the message is about */
            memcpy(about, RDPTR(items, 0x10), 31);
            text = about;
        }
        if (name)
            fmt(what, sizeof(what), "SYSTEM_MSG %s %s", name, k_disp[disp]);
        else
            fmt(what, sizeof(what), "SYSTEM_MSG %u %s", t, k_disp[disp]);
        if (t == 4 || t == 8 || t == 14)
            logf("dialog: SaveData WARNING: the title reports no space (%lu blocks): the virtual store should never "
                 "be full",
                 value);
        if (t == 10 || t == 11 || t == 13)
            logf("dialog: SaveData WARNING: the title reports its save %s corrupted; it now repairs its own virtual "
                 "save",
                 about[0] ? about : "(unnamed)");
    } else if (mode == SD_ERROR_CODE) { /* u32 errorCode */
        a = ANS_BTN1;
        fmt(what, sizeof(what), "ERROR_CODE %x", RD32(sub, 0));
    } else { /* PROGRESS_BAR: u32 barType, msg @8, u32 sysMsgType @10 */
        a = ANS_HOLD;
        text = (const char *)RDPTR(sub, 8);
        fmt(what, sizeof(what), "PROGRESS_BAR %u", RD32(sub, 0x10));
    }
    g_sd.mode = mode, g_sd.value = 0;
    g_sd.user = items ? (s32)RD32(items, 0) : -1;
    g_sd.user_data = RD64(p, 0x70);
    memcpy(g_sd.dir, dir, sizeof(dir));
    if (dlg_start(&g_sd, a, ra, what, text) != ANS_BTN1)
        g_sd.dir[0] = 0;
    return 0;
}

static s32 hle_sceSaveDataDialogClose(void)
{
    s32 rc = dlg_close(&g_sd);
    if (rc == 0)
        g_sd.dir[0] = 0; /* closed LIST: nothing chosen */
    return rc;
}

/* Result (0x48): u32 mode, s32 result @4, u32 buttonId @8, DirName *dirName @10,
 * SceSaveDataParam *param @18, void *userData @20 */
static s32 hle_sceSaveDataDialogGetResult(void *r)
{
    s32 rc = dlg_check_result(&g_sd, r);
    if (rc)
        return rc;
    s32 result = g_sd.result;
    u32 button = g_sd.mode == SD_LIST ? 0 : g_sd.button;
    u32 sdk = dlg_title_sdk();
    if (button == 2 && sdk && sdk < 0x04500000) /* before SDK 4.50 "No" is USER_CANCELED */
        result = 1, button = 0;
    WR32(r, 0, g_sd.mode);
    WR32(r, 4, result);
    WR32(r, 8, button);
    char *dn = *(char **)((u8 *)r + 0x10);
    struct sd_param *prm = *(struct sd_param **)((u8 *)r + 0x18);
    if (dn) {
        memset(dn, 0, 32);
        strlcpy(dn, g_sd.dir, 32);
    }
    if (prm && g_sd.dir[0]) {
        memset(prm, 0, sizeof(*prm));
        sd_query(g_sd.user, g_sd.dir, prm);
    }
    WR64(r, 0x20, g_sd.user_data);
    return 0;
}

static s32 hle_sceSaveDataDialogIsReadyToDisplay(void) { return 1; }

static s32 hle_sceSaveDataDialogProgressBarInc(u32 target, u32 delta)
{
    s32 rc = dlg_progress(&g_sd, SD_PROGRESS_BAR, target);
    if (rc == 0)
        g_sd.value += delta;
    return rc;
}

static s32 hle_sceSaveDataDialogProgressBarSetValue(u32 target, u32 rate)
{
    s32 rc = dlg_progress(&g_sd, SD_PROGRESS_BAR, target);
    if (rc == 0)
        g_sd.value = rate;
    return rc;
}

static const struct ext_hle_ent k_hle[] = {
    {"libSceSaveDataDialog", "sceSaveDataDialogInitialize", sd_init, NULL},
    {"libSceSaveDataDialog", "sceSaveDataDialogOpen", hle_sceSaveDataDialogOpen, NULL},
    {"libSceSaveDataDialog", "sceSaveDataDialogUpdateStatus", sd_update, NULL},
    {"libSceSaveDataDialog", "sceSaveDataDialogGetStatus", sd_update, NULL},
    {"libSceSaveDataDialog", "sceSaveDataDialogGetResult", hle_sceSaveDataDialogGetResult, NULL},
    {"libSceSaveDataDialog", "sceSaveDataDialogClose", hle_sceSaveDataDialogClose, NULL},
    {"libSceSaveDataDialog", "sceSaveDataDialogTerminate", sd_term, NULL},
    {"libSceSaveDataDialog", "sceSaveDataDialogIsReadyToDisplay", hle_sceSaveDataDialogIsReadyToDisplay, NULL},
    {"libSceSaveDataDialog", "sceSaveDataDialogProgressBarInc", hle_sceSaveDataDialogProgressBarInc, NULL},
    {"libSceSaveDataDialog", "sceSaveDataDialogProgressBarSetValue", hle_sceSaveDataDialogProgressBarSetValue, NULL},
    {"libGhostldrDialog", "ghostldrDialogTestSaves", hle_ghostldrDialogTestSaves, NULL},
};

static const struct ext_lib k_libs[] = {
    {"libSceSaveDataDialog", NULL, EXT_LIB_NO_HOST, DLG_E_NOT_SUPPORTED},
};

DLG_MINSIZE_END

EXT_FAMILY_LIBS("dialog_savedata", k_hle, k_libs, NULL);
