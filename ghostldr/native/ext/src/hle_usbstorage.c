/*
 * libSceUsbStorage as the title sees it (docs/usb.md).  When the title's
 * files are on a USB drive, ghostldr holds libSceUsbStorage's session to
 * ShellCore's AutoMounter for the drive's mount (approot.c).  A title's own
 * sceUsbStorageInit would end that session (a second Init leaves the
 * library uninitialized and every mount gone) and its sceUsbStorageTerm
 * would unmount the title's own files.  While the session is open both
 * answer 0 without reaching the library; the title's other calls share the
 * session.  Otherwise both are the library's.
 */
#include "ext.h"

bool usb_session_open(void) HIDDEN; /* approot.c */

typedef s32 (*fn1)(u64);

static s32 call(const char *name, u64 a)
{
    fn1 f = (fn1)host_real_in("libSceUsbStorage", name);
    return f ? f(a) : (s32)0x80F40009; /* not initialized */
}

static s32 hle_init(void *attr)
{
    if (usb_session_open()) {
        logf("usbstorage: the title's sceUsbStorageInit: the session for its USB drive is already open");
        return 0;
    }
    return call("sceUsbStorageInit", (u64)attr);
}

static s32 hle_term(void)
{
    if (usb_session_open()) {
        logf("usbstorage: the title's sceUsbStorageTerm: kept open, the title's files are on the USB drive");
        return 0;
    }
    return call("sceUsbStorageTerm", 0);
}

static const struct ext_hle_ent k_hle[] = {
    {"libSceUsbStorage", "sceUsbStorageInit", hle_init, NULL},
    {"libSceUsbStorage", "sceUsbStorageTerm", hle_term, NULL},
};
EXT_FAMILY("usbstorage", k_hle, NULL);
