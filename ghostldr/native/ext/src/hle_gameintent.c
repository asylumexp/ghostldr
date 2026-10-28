/*
 * libSceNpGameIntent: the parts that must not reach SWRR's process as they
 * are.  SWRR (SDK 2.50) never preloads or initializes the library; the core
 * load-starts it on demand.  The 0x10000017 system event that would lead the
 * title to ReceiveIntent is dropped in hle_system.c.
 *
 * The np-online family (hle_np_online.c, np-online.md section 3.12) answers
 * ReceiveIntent (INTENT_NOT_FOUND), GetPropertyValueString (VALUE_NOT_FOUND)
 * and Terminate (0) itself, and denies the rest of the library to the host;
 * this file keeps the calls that need SWRR's instance or must refuse.  It is
 * the system-appcontent spec's section 10.
 */
#include "ext.h"

#define GI_UNKNOWN ((s32)0x80553800)
#define GI_ALREADY_INITIALIZED ((s32)0x80553801)

static s32 hle_sceNpGameIntentInitialize(const void *param)
{
    static s32 (*real)(const void *);
    static bool tried;
    if (!tried)
        real = host_real_in("libSceNpGameIntent", "sceNpGameIntentInitialize"), tried = true;
    s32 rc = real ? real(param) : 0;
    logf("sceNpGameIntentInitialize -> %x", (u32)rc);
    return rc == GI_ALREADY_INITIALIZED ? 0 : rc;
}

/* another app's launch would end SWRR */
static s32 hle_launch(void)
{
    logf("sceNpGameIntentLaunchApp refused: it would end SWRR");
    return GI_UNKNOWN;
}

static s32 gi_ok(void) { return 0; }

#define GI "libSceNpGameIntent"
static const struct ext_hle_ent k_hle[] = {
    {GI, "sceNpGameIntentInitialize", hle_sceNpGameIntentInitialize, NULL},
    {GI, "sceNpGameIntentNotifyAppLaunched", gi_ok, NULL},
    {GI, "sceNpGameIntentLaunchApp", hle_launch, NULL},
    {GI, "sceNpGameIntentLaunchApp2", hle_launch, NULL},
};

EXT_FAMILY("gameintent", k_hle, NULL);
