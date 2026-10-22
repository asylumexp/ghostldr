/* Dialog sysmodules (dialogs.md §4): every function of these libraries is
 * implemented by the dialog families, or denied (EXT_LIB_NO_HOST), so
 * nothing a title imports from them reaches the real sprx.  Loading one
 * through sceSysmoduleLoadModule then only needs to report success: it
 * returns 0 without mapping the module into SWRR's tight address space.
 *
 * The sysmodule router (hle_sysmodule.c) owns sceSysmoduleLoadModule,
 * IsLoaded and their Internal forms, and asks these helpers about dialog
 * ids first. */
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

/* a dialog id: remember the title loaded it (logged once) and say so */
bool dialog_sysmod_load(u32 id)
{
    const char *name = dialog_sysmod_name(id);
    if (!name)
        return false;
    u32 bit = 1u << (id & 31), old = __atomic_fetch_or(&g_loaded[id >> 5], bit, __ATOMIC_ACQ_REL);
    if (!(old & bit))
        logf("sysmodule: %x (%s) is implemented by the extension: not loaded", id, name);
    return true;
}

bool dialog_sysmod_loaded(u32 id)
{
    return dialog_sysmod_virtual(id) &&
           (__atomic_load_n(&g_loaded[id >> 5], __ATOMIC_ACQUIRE) & (1u << (id & 31))) != 0;
}

DLG_MINSIZE_END
