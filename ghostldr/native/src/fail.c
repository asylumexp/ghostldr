/*
 * Failures the user sees (docs/failures.md).  A boot that stops says on the
 * TV what went wrong and what to do: the extension's screens do it
 * (ext/src/failure.c picks the message from the boot's result and the notes
 * below), its code placed for that when it isn't and given back afterwards
 * (ext.c ext_show).  Without a usable extension, a system notification says
 * the game didn't start and where the log is.  The log keeps the detail.
 *
 * Notes (fail_note): the last title file the loader could not use (missing,
 * unreadable, encrypted, not an executable it can use, too big for memory),
 * and why an installed title can't be served (ghostfs.c).
 */
#include "ghost_api.h"

#define COLD GHOST_COLD

bool ext_show(u32 kind, s64 code); /* ext.c */
const char *bx_why(void) __attribute__((weak)); /* bigexec.c (rel-reclaim): why the code didn't fit, for the user */

/* a system notification: what Luac0re's send_notification writes */
COLD void fail_notify(const char *text)
{
    u8 b[0xC30];
    memset(b, 0, sizeof(b));
    *(s32 *)(b + 0x10) = -1;
    b[0x2C] = 1;
    strlcpy((char *)b + 0x2D, text, 0x3FF);
    strlcpy((char *)b + 0x42D, "cxml://psnotification/tex_icon_system", 0x100);
    int fd = K.open("/dev/notification0", O_WRONLY, 0);
    if (fd >= 0) {
        K.write(fd, b, sizeof(b));
        K.close(fd);
    }
}

COLD void fail_note(u32 kind, const char *path)
{
    struct ghost_diag *D = log_diag();
    if (!D)
        return;
    D->note = kind;
    strlcpy(D->note_path, path, sizeof(D->note_path));
}

const char k_fail_notice[] = "ghostldr: the game stopped. Its log says why: ghostldr/logs on the USB drive, "
                             "or /temp0/ghostldr.log";

/* ghost_boot's result (main.c): a failure is said on the TV */
COLD int fail_result(int rc)
{
    struct ghost_diag *D = log_diag();
    /* -7: ghostldr.lua says it; -20: the selector's "Close ghostldr" (rel-settings' SEL_EXIT_LOADER);
     * CTX_TITLE_BACK: the title ran and ended, back to the game selector */
    if (!rc || !D || D->shown || rc == -7 || rc == -20 || rc == CTX_TITLE_BACK)
        return rc;
    logf("boot: stopped (%d)", rc);
    if (rc == -3 && bx_why && bx_why())
        fail_note(FAIL_CODE_SPACE, bx_why());
    if (rc == -8 || !ext_show(FAIL_BOOT, rc))
        fail_notify(k_fail_notice);
    return rc;
}

/* the title's run ended (boot.c: its entry returned) */
COLD void fail_title(u32 kind, s64 code)
{
    struct ghost_diag *D = log_diag();
    if (D && D->show)
        D->show(kind, code);
}
