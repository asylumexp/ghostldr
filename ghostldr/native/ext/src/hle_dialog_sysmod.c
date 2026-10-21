/* Dialog sysmodules (dialogs.md §4): every function of these libraries is
 * implemented by the dialog families, or denied (EXT_LIB_NO_HOST), so
 * nothing a title imports from them reaches the real sprx.  Loading one
 * through sceSysmoduleLoadModule then only needs to report success: it
 * returns 0 without mapping the module into SWRR's tight address space.
 *
 * This family answers the dialog ids and passes every other id on: to
 * another family that registers the same calls (a sysmodule router, e.g.
 * hle_sysmodule.c), found at init, else to the core's override (hle.c),
 * else to SWRR's library.  Families merge in link order (ext/src file names)
 * and the first registration wins: should a router sort before this file,
 * it should ask dialog_sysmod_virtual() itself. */
#include "hle_dialog.h"
DLG_MINSIZE_BEGIN

static const struct {
    u16 id;
    const char *name;
} k_virt[] = {
    {0x95, "libSceIme"},
    {0x96, "libSceImeDialog"},
    {0xa0, "libSceSaveDataDialog"},
    {0xa2, "libSceInvitationDialog"},
    {0xa4, "libSceMsgDialog"},
    {0xa8, "libSceNpCommerce"},
    {0xab, "libSceWebBrowserDialog"},
    {0xac, "libSceErrorDialog"},
    {0xb2, "libSceNpProfileDialog"},
    {0xb3, "libSceNpFriendListDialog"},
    {0xb8, "libSceGameCustomDataDialog"},
    {0xb9, "libSceNpEulaDialog"},
    {0xd6, "libSceUsbStorageDialog"},
    {0xda, "libSceNpSnsYouTubeDialog"},
    {0xe2, "libSceLoginDialog"},
    {0xe4, "libSceSigninDialog"},
    {0xf0, "libSceNetCtlApDialog"},
    {0xfd, "libSceVrServiceDialog"},
    {0x100, "libSceSocialScreenDialog"},
    {0x10d, "libScePlayerInvitationDialog"},
};
static u32 g_loaded[(0x10d / 32) + 1]; /* the title "loaded" these virtual ids */

const char *dialog_sysmod_name(u32 id)
{
    for (u32 i = 0; i < ARRAY_SIZE(k_virt); i++)
        if (k_virt[i].id == id)
            return k_virt[i].name;
    return NULL;
}

bool dialog_sysmod_virtual(u32 id) { return dialog_sysmod_name(id) != NULL; }

#define SYSMOD_E_UNLOADED ((s32)0x805A1001) /* SCE_SYSMODULE_ERROR_UNLOADED */

static bool virtual_load(u32 id)
{
    const char *name = dialog_sysmod_name(id);
    if (!name)
        return false;
    u32 bit = 1u << (id & 31), old = __atomic_fetch_or(&g_loaded[id >> 5], bit, __ATOMIC_ACQ_REL);
    if (!(old & bit))
        logf("sysmodule: %x (%s) is implemented by the extension: not loaded", id, name);
    return true;
}

/* where the other ids go: another family's function (found at init), the
 * core's override, or SWRR's */
enum { NEXT_LOAD, NEXT_LOAD_ARG, NEXT_IS_LOADED };
static const char *const k_next[][2] = {
    {"sceSysmoduleLoadModule", "g8cM39EUZ6o"},
    {"sceSysmoduleLoadModuleInternalWithArg", "hHrGoGoNf+s"},
    {"sceSysmoduleIsLoaded", "fMP5NHUOaMk"},
};
static void *g_next[3];

static void *next_fn(u32 i)
{
    if (!g_next[i]) {
        void *fn = (void *)hle_lookup("libSceSysmodule", k_next[i][0]);
        g_next[i] = fn ? fn : host_real(k_next[i][0]);
    }
    return g_next[i];
}

static s32 hle_sceSysmoduleLoadModule(u32 id)
{
    if (virtual_load(id & 0xffff))
        return 0;
    s32 (*next)(u32) = (s32(*)(u32))next_fn(NEXT_LOAD);
    return next ? next(id) : 0;
}

static s32 hle_sceSysmoduleLoadModuleInternalWithArg(u32 id, size_t argc, const void *argv, u64 flags, s32 *res)
{
    if (virtual_load(id)) { /* a u32 id: internal ids (bit 31) never match */
        if (res)
            *res = 0;
        return 0;
    }
    s32 (*next)(u32, size_t, const void *, u64, s32 *) =
        (s32(*)(u32, size_t, const void *, u64, s32 *))next_fn(NEXT_LOAD_ARG);
    return next ? next(id, argc, argv, flags, res) : 0;
}

static s32 hle_sceSysmoduleIsLoaded(u32 id)
{
    if (dialog_sysmod_virtual(id & 0xffff))
        return __atomic_load_n(&g_loaded[(id & 0xffff) >> 5], __ATOMIC_ACQUIRE) & (1u << (id & 31))
                   ? 0
                   : SYSMOD_E_UNLOADED;
    s32 (*next)(u32) = (s32(*)(u32))next_fn(NEXT_IS_LOADED);
    return next ? next(id) : SYSMOD_E_UNLOADED;
}

static const struct ext_hle_ent k_hle[] = {
    {"libSceSysmodule", "sceSysmoduleLoadModule", hle_sceSysmoduleLoadModule, NULL},
    {"libSceSysmodule", "sceSysmoduleLoadModuleInternalWithArg", hle_sceSysmoduleLoadModuleInternalWithArg, NULL},
    {"libSceSysmodule", "sceSysmoduleIsLoaded", hle_sceSysmoduleIsLoaded, NULL},
};

/* another family's sysmodule functions: the next stop for other ids */
extern const struct ext_family *const __ext_families_start[] HIDDEN;
extern const struct ext_family *const __ext_families_end[] HIDDEN;
static const struct ext_family ext_family_;

static int sysmod_init(void)
{
    for (const struct ext_family *const *f = __ext_families_start; f < __ext_families_end; f++)
        for (u32 i = 0; *f != &ext_family_ && i < (*f)->nhle; i++) {
            const struct ext_hle_ent *e = &(*f)->hle[i];
            if (!e->lib || strcmp(e->lib, "libSceSysmodule") != 0)
                continue;
            for (u32 k = 0; k < ARRAY_SIZE(k_next); k++)
                if (!g_next[k] && ((e->name && strcmp(e->name, k_next[k][0]) == 0) ||
                                   (e->nid && strcmp(e->nid, k_next[k][1]) == 0))) {
                    g_next[k] = e->fn;
                    logf("dialog_sysmod: %s: dialog ids here, the others to family %s", k_next[k][0], (*f)->name);
                }
        }
    return 0;
}

DLG_MINSIZE_END

EXT_FAMILY("dialog_sysmod", k_hle, sysmod_init);
