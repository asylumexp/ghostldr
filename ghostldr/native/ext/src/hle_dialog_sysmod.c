/* Dialog sysmodules (dialogs.md §4): every function of these libraries is
 * implemented by the dialog families, or denied (EXT_LIB_NO_HOST), so
 * nothing a title imports from them reaches the real sprx.  Loading one
 * through sceSysmoduleLoadModule then only needs to report success: it
 * returns 0 without mapping the module into SWRR's tight address space.
 *
 * The family that owns libSceSysmodule (the system family's router, planned
 * in system-appcontent.md §3.4) should ask dialog_sysmod_virtual().  Until
 * one does, this family wraps the core's sceSysmoduleLoadModule override;
 * when another family registers sceSysmoduleLoadModule, this one steps
 * aside at init so the two never compete. */
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
    {0xb0, "libSceNpSnsFacebookDialog"},
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

/* the core's own override (hle.c) for everything else */
static void *core_fn(const char *name)
{
    void *fn = (void *)hle_lookup("libSceSysmodule", name);
    return fn ? fn : host_real(name);
}

static s32 hle_sceSysmoduleLoadModule(u32 id)
{
    static s32 (*core)(u32);
    if (virtual_load(id & 0xffff))
        return 0;
    if (!core)
        core = (s32(*)(u32))core_fn("sceSysmoduleLoadModule");
    return core ? core(id) : 0;
}

static s32 hle_sceSysmoduleLoadModuleInternalWithArg(u32 id, size_t argc, const void *argv, u64 flags, s32 *res)
{
    static s32 (*core)(u32, size_t, const void *, u64, s32 *);
    if (virtual_load(id)) { /* a u32 id: internal ids (bit 31) never match */
        if (res)
            *res = 0;
        return 0;
    }
    if (!core)
        core = (s32(*)(u32, size_t, const void *, u64, s32 *))core_fn("sceSysmoduleLoadModuleInternalWithArg");
    return core ? core(id, argc, argv, flags, res) : 0;
}

static s32 hle_sceSysmoduleIsLoaded(u32 id)
{
    static s32 (*real)(u32);
    if (dialog_sysmod_virtual(id & 0xffff))
        return __atomic_load_n(&g_loaded[(id & 0xffff) >> 5], __ATOMIC_ACQUIRE) & (1u << (id & 31))
                   ? 0
                   : SYSMOD_E_UNLOADED;
    if (!real)
        real = (s32(*)(u32))host_real("sceSysmoduleIsLoaded");
    return real ? real(id) : SYSMOD_E_UNLOADED;
}

static const struct ext_hle_ent k_hle[] = {
    {"libSceSysmodule", "sceSysmoduleLoadModule", hle_sceSysmoduleLoadModule, NULL},
    {"libSceSysmodule", "sceSysmoduleLoadModuleInternalWithArg", hle_sceSysmoduleLoadModuleInternalWithArg, NULL},
    {"libSceSysmodule", "sceSysmoduleIsLoaded", hle_sceSysmoduleIsLoaded, NULL},
};

/* Step aside when another family handles sceSysmoduleLoadModule */
extern const struct ext_family *const __ext_families_start[] HIDDEN;
extern const struct ext_family *const __ext_families_end[] HIDDEN;
static const struct ext_family ext_family_;

static int sysmod_init(void)
{
    for (const struct ext_family *const *f = __ext_families_start; f < __ext_families_end; f++)
        for (u32 i = 0; *f != &ext_family_ && i < (*f)->nhle; i++) {
            const struct ext_hle_ent *e = &(*f)->hle[i];
            if (e->lib && e->name && strcmp(e->lib, "libSceSysmodule") == 0 &&
                strcmp(e->name, "sceSysmoduleLoadModule") == 0) {
                logf("dialog_sysmod: family %s owns sceSysmoduleLoadModule; it should ask dialog_sysmod_virtual()",
                     (*f)->name);
                return 1;
            }
        }
    return 0;
}

DLG_MINSIZE_END

EXT_FAMILY("dialog_sysmod", k_hle, sysmod_init);
