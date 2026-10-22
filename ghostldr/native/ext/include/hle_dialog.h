/*
 * The common dialog family (ext/src/hle_dialog*.c; research/hle-specs/dialogs.md).
 *
 * Every PS4 system dialog is answered here and never shown.  Inside SWRR a
 * real dialog gets no controller focus, shares SWRR's one CommonDialog slot,
 * follows SWRR's SDK 2.50 rules, runs under SWRR's NP identity and (the save
 * data dialog) lists SWRR's own saves.  libSceScreenShot, which has no
 * dialog, is not handled here (hle_media.c, or SWRR's library).
 *
 * One state machine (struct dlg) serves every CommonDialog client library
 * and ErrorDialog.  ImeDialog and Ime have their own (hle_dialog_ime.c).
 *
 * Answer policy, "press Cross at once": a dialog answers with the button that
 * has default focus, which is what a user pressing Cross the moment it
 * appears gets.  Button-less dialogs (progress, wait) stay RUNNING until the
 * title closes them, or 10 s after the last progress update.  Online dialogs
 * (profile, sign-in, invitations, store, browser) answer "user cancelled".
 * Every answer is logged with the message text: the log is the user
 * interface.  A per-call-site override table (hle_dialog.c) and two ctx flags
 * adjust it.
 */
#pragma once
#include "ext.h"

/* Dialog code is never hot (a status poll per frame at most): optimize the
 * files for size.  Each hle_dialog*.c starts with DLG_MINSIZE_BEGIN and
 * ends with DLG_MINSIZE_END. */
#define DLG_MINSIZE_BEGIN _Pragma("clang attribute push(__attribute__((minsize)), apply_to = function)")
#define DLG_MINSIZE_END _Pragma("clang attribute pop")

/* ctx flags (run.py --dialog-cancel, --ime-fixed-name) */
#define CTX_FLAG_DIALOG_CANCEL 0x80   /* every dialog with buttons answers CANCEL (to bisect title hangs) */
#define CTX_FLAG_IME_FIXED_NAME 0x100 /* IME text is "Player", never the console user's name */

/* status of every CommonDialog client and of ErrorDialog */
#define DLG_NONE 0
#define DLG_INITIALIZED 1
#define DLG_RUNNING 2
#define DLG_FINISHED 3

#define DLG_HOLD_US 10000000ULL /* button-less dialogs, and the stall guard: 10 s */

/* answers: what pressing Cross at once gives (dialogs.md §7) */
enum {
    ANS_CLOSE,  /* closed by the title: result OK, buttonId INVALID (0) */
    ANS_BTN1,   /* result OK, buttonId 1 (OK / YES / BUTTON1) */
    ANS_BTN2,   /* result OK, buttonId 2 (NO / BUTTON2) */
    ANS_CANCEL, /* result USER_CANCELED (1), buttonId INVALID */
    ANS_HOLD,   /* no buttons: RUNNING until Close or Terminate, ANS_CLOSE after DLG_HOLD_US */
};

/* library ids: the CommonDialog slot owner, and the override table's key */
enum {
    DLG_MSG = 1,
    DLG_SAVEDATA,
    DLG_ERROR,
    DLG_NPPROFILE,
    DLG_SIGNIN,
    DLG_INVITATION,
    DLG_COMMERCE,
    DLG_WEBBROWSER,
    DLG_FRIENDLIST,
    DLG_GAMECUSTOMDATA,
    DLG_PLAYERINVITATION,
    DLG_IMEDIALOG,
    DLG_IME,
    DLG_GENERIC, /* + n: the other *Dialog libraries (hle_dialog_online.c) */
};

/* A library's error codes: base | one of these */
struct dlg_errs {
    u32 base;
    u8 not_init, already, state, param, null, not_finished, not_running, busy;
};
extern const struct dlg_errs dlg_errs_common HIDDEN; /* 0x80B800xx SCE_COMMON_DIALOG_ERROR_* */
extern const struct dlg_errs dlg_errs_error HIDDEN;  /* 0x80ED00xx SCE_ERROR_DIALOG_ERROR_* */
extern const struct dlg_errs dlg_errs_signin HIDDEN; /* 0x813500xx SCE_SIGNIN_DIALOG_ERROR_* */
#define DLG_ERR(d, what) ((s32)((d)->err->base | (d)->err->what))

#define DLG_E_RESULT_NONE ((s32)0x80B80007)   /* SCE_COMMON_DIALOG_ERROR_RESULT_NONE */
#define DLG_E_PARAM ((s32)0x80B8000A)         /* SCE_COMMON_DIALOG_ERROR_PARAM_INVALID */
#define DLG_E_NOT_SUPPORTED ((s32)0x80B8000F) /* SCE_COMMON_DIALOG_ERROR_NOT_SUPPORTED */

/* One dialog library's state.  Never keeps the title's param pointers past
 * Open: only the scalars a result needs. */
struct dlg {
    const char *name; /* in logs: "Msg", "SaveData", ... */
    const struct dlg_errs *err;
    u8 id;     /* DLG_* */
    u8 client; /* holds the CommonDialog slot from Initialize to Terminate */
    u8 stalled, timed_out;
    volatile int lock;
    u32 status, answer, polls;
    s32 result; /* once FINISHED */
    u32 button;
    u64 t_open; /* process time (us) of Open or of the last progress update */
    u64 t_fail; /* first failed Open since the last good Initialize or Open (stall guard) */
    u32 mode;   /* the opened mode, where the library has one */
    s32 user;
    u64 user_data;
    u32 value;    /* progress bar, percent */
    char dir[32]; /* SaveDataDialog: the entry chosen ("" for none or the new item) */
};
#define DLG_INIT(nm, idv, errs, cl) {.name = nm, .err = &(errs), .id = idv, .client = cl}

/* dlg_check_open flags */
#define DLG_BASE 1    /* the param starts with SceCommonDialogBaseParam (size 0x30, magic) */
#define DLG_LENIENT 2 /* layout inferred: log a size or base mismatch but accept it */

u64 dlg_now(void) HIDDEN;
s32 dlg_initialize(struct dlg *d) HIDDEN;
/* Open, before the library reads its param: state, NULL, sizes, magic.  The
 * library size is a size_width-byte field at size_off (size 0: unchecked). */
s32 dlg_check_open(struct dlg *d, const void *p, u32 size_off, u32 size_width, u32 size, u32 flags) HIDDEN;
/* An Open that fails: returns rc and arms the stall guard (a title that
 * ignores Open's result and polls for FINISHED is finished after 10 s). */
s32 dlg_fail(struct dlg *d, s32 rc) HIDDEN;
/* Open succeeded: apply the overrides and flags to the library's default
 * answer, go RUNNING and log "dialog: <lib> <what> "<text>" -> <answer>". */
u32 dlg_start(struct dlg *d, u32 dflt, u64 ra, const char *what, const char *text) HIDDEN;
u32 dlg_update(struct dlg *d) HIDDEN; /* UpdateStatus and GetStatus: the tick */
s32 dlg_check_result(struct dlg *d, const void *r) HIDDEN;
s32 dlg_close(struct dlg *d) HIDDEN;
s32 dlg_terminate(struct dlg *d) HIDDEN;
/* ProgressBar* calls: RUNNING, the progress mode, target 0; counts as activity */
s32 dlg_progress(struct dlg *d, u32 mode, u32 target) HIDDEN;
/* the online dialogs: any param (layout lenient), answer CANCEL */
s32 dlg_open_cancel(struct dlg *d, const void *p, u64 ra, const char *what) HIDDEN;
/* GetResult of a library whose result layout is unknown: RESULT_NONE */
s32 dlg_result_none(struct dlg *d, void *r) HIDDEN;

/* The answer for an Open: an override for (lib, call site) wins, then the
 * ctx flag, then dflt.  *text: the override's UTF-8 text (IME), or NULL. */
u32 dlg_policy(u32 id, u32 dflt, u64 caller_off, const char **text) HIDDEN;
/* "eboot+8a1d24" for a return address; returns the offset in the title's
 * executable (the override table's key), or ~0 outside it. */
u64 dlg_where(u64 ra, char *buf, size_t n) HIDDEN;
/* a title string for a log line: at most n-1 bytes, '\n' -> '|', "" for NULL */
void dlg_text(char *out, size_t n, const char *s) HIDDEN;
const char *dlg_answer_name(u32 answer) HIDDEN;
u32 dlg_title_sdk(void) HIDDEN; /* the title's SDK version (proc param), 0 if unknown */

/* Per-library thin wrappers around the shared state machine (unused ones
 * are dropped by the compiler) */
#define DLG_FNS(pfx, var)                                                                                    \
    __attribute__((unused)) static s32 pfx##_init(void) { return dlg_initialize(&var); }                     \
    __attribute__((unused)) static u32 pfx##_update(void) { return dlg_update(&var); }                       \
    __attribute__((unused)) static s32 pfx##_close(void) { return dlg_close(&var); }                         \
    __attribute__((unused)) static s32 pfx##_term(void) { return dlg_terminate(&var); }                      \
    __attribute__((unused)) static s32 pfx##_result_none(void *r) { return dlg_result_none(&var, r); }       \
    __attribute__((unused)) static s32 pfx##_open_cancel(const void *p)                                     \
    {                                                                                                        \
        return dlg_open_cancel(&var, p, (u64)__builtin_return_address(0), "Open");                           \
    }

/* sysmodule ids whose libraries the dialog families implement completely
 * (every import of them binds here or to a denied stub): loading one may
 * return 0 without mapping the real sprx into SWRR.  The sysmodule router
 * (hle_sysmodule.c) asks these first (hle_dialog_sysmod.c). */
bool dialog_sysmod_virtual(u32 id) HIDDEN;
const char *dialog_sysmod_name(u32 id) HIDDEN;
bool dialog_sysmod_load(u32 id) HIDDEN;   /* a dialog id: mark it loaded (true), else false */
bool dialog_sysmod_loaded(u32 id) HIDDEN; /* a dialog id the title loaded */

#define RD32(p, off) (*(const u32 *)((const u8 *)(p) + (off)))
#define RD64(p, off) (*(const u64 *)((const u8 *)(p) + (off)))
#define RDPTR(p, off) (*(const u8 *const *)((const u8 *)(p) + (off)))
#define WR32(p, off, v) (*(u32 *)((u8 *)(p) + (off)) = (u32)(v))
#define WR64(p, off, v) (*(u64 *)((u8 *)(p) + (off)) = (u64)(v))
