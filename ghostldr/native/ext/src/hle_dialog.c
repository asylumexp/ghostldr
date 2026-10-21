/* Common dialogs (dialogs.md): the shared state machine, the answer policy,
 * and libSceCommonDialog, libSceMsgDialog and libSceErrorDialog.  The other
 * libraries are in hle_dialog_online.c, hle_dialog_savedata.c and
 * hle_dialog_ime.c; the design is in ext/include/hle_dialog.h. */
#include "hle_dialog.h"
DLG_MINSIZE_BEGIN

const struct dlg_errs dlg_errs_common = {0x80B80000, 0x03, 0x04, 0x06, 0x0A, 0x0D, 0x05, 0x0B, 0x08};
/* ErrorDialog: Ghost retries Open while it returns 0x80ED0006 SERVICE_BUSY,
 * which this never returns (no slot) */
const struct dlg_errs dlg_errs_error = {0x80ED0000, 0x01, 0x02, 0x05, 0x03, 0x03, 0x05, 0x05, 0x06};
const struct dlg_errs dlg_errs_signin = {0x81350000, 0x01, 0x02, 0x05, 0x03, 0x03, 0x05, 0x05, 0x06};

/* ---- answer overrides ----
 * {library, return address - executable base, answer, IME text}.  A title
 * is told apart by its executable's image size (0: any title).  None are
 * needed for CUSA11456 yet; add one when a log shows a default answer that
 * hangs or harms a title, e.g.
 *     {DLG_MSG, 0xc7713a, 0x1234000, ANS_BTN2, NULL},
 */
struct dlg_override {
    u8 id, answer;
    u32 caller_off;
    u64 image_size;
    const char *text;
};
static const struct dlg_override k_overrides[] = {
    {0, 0, 0, 0, NULL}, /* end */
};
/* set at run time (libGhostldrDialog:ghostldrDialogOverride; the tests) */
static struct {
    u8 id, answer;
    u64 caller_off;
    char text[64];
} g_rt_over[4];
static u32 g_nrt_over;

u32 dlg_policy(u32 id, u32 dflt, u64 caller_off, const char **text)
{
    *text = NULL;
    u64 size = g_main_mod ? g_main_mod->image_size : 0;
    for (const struct dlg_override *o = k_overrides; o->id; o++)
        if (o->id == id && o->caller_off == caller_off && (!o->image_size || o->image_size == size)) {
            *text = o->text;
            return o->answer;
        }
    for (u32 i = 0; i < g_nrt_over && i < ARRAY_SIZE(g_rt_over); i++)
        if (g_rt_over[i].id == id && g_rt_over[i].caller_off == caller_off) {
            *text = g_rt_over[i].text[0] ? g_rt_over[i].text : NULL;
            return g_rt_over[i].answer;
        }
    if ((g_ctx->flags & CTX_FLAG_DIALOG_CANCEL) && dflt != ANS_HOLD)
        return ANS_CANCEL;
    return dflt;
}

static s32 hle_ghostldrDialogOverride(u32 id, u64 caller_off, u32 answer, const char *text)
{
    if (g_nrt_over >= ARRAY_SIZE(g_rt_over) || answer > ANS_HOLD)
        return -1;
    u32 i = g_nrt_over;
    g_rt_over[i].id = (u8)id, g_rt_over[i].answer = (u8)answer, g_rt_over[i].caller_off = caller_off;
    strlcpy(g_rt_over[i].text, text ? text : "", sizeof(g_rt_over[i].text));
    __atomic_store_n(&g_nrt_over, i + 1, __ATOMIC_RELEASE);
    logf("dialog: override %u @eboot+%lx -> %s", id, caller_off, dlg_answer_name(answer));
    return 0;
}

/* ---- helpers ---- */
u64 dlg_now(void)
{
    static u64 (*real)(void);
    static u64 fake;
    if (!real)
        real = (u64(*)(void))host_real("sceKernelGetProcessTime");
    return real ? real() : (fake += 16667); /* no clock: count polls as frames */
}

const char *dlg_answer_name(u32 a)
{
    static const char *const k[] = {"CLOSE", "BUTTON1", "BUTTON2", "CANCEL", "HOLD"};
    return a < ARRAY_SIZE(k) ? k[a] : "?";
}

void dlg_text(char *out, size_t n, const char *s)
{
    size_t i = 0;
    for (; s && s[i] && i + 1 < n; i++)
        out[i] = s[i] == '\n' ? '|' : s[i] == '\r' ? ' ' : s[i];
    out[i] = 0;
}

u64 dlg_where(u64 ra, char *buf, size_t n)
{
    for (struct module *m = g_modules; m; m = m->next)
        if (ra >= m->base && ra - m->base < m->image_size) {
            fmt(buf, n, "%s+%lx", m->is_main ? "eboot" : m->name, ra - m->base);
            return m == g_main_mod ? ra - m->base : ~0UL;
        }
    fmt(buf, n, "%lx", ra);
    return ~0UL;
}

u32 dlg_title_sdk(void)
{
    const u8 *pp = g_main_mod ? (const u8 *)g_main_mod->proc_param : NULL;
    /* SceProcParam: u64 size, u32 magic "ORBI", u32 version, u32 sdk_version */
    return pp && RD64(pp, 0) >= 0x14 && RD32(pp, 8) == 0x4942524f ? RD32(pp, 0x10) : 0;
}

/* ---- the state machine (dialogs.md §5) ---- */
static volatile u32 g_slot; /* DLG_* id of the client holding the CommonDialog slot */
static const char *volatile g_slot_name;

static void finish(struct dlg *d, u32 answer)
{
    d->status = DLG_FINISHED;
    d->result = answer == ANS_CANCEL ? 1 : 0;
    d->button = answer == ANS_BTN1 ? 1 : answer == ANS_BTN2 ? 2 : 0;
}

s32 dlg_initialize(struct dlg *d)
{
    spin_lock(&d->lock);
    s32 rc = 0;
    if (d->status != DLG_NONE)
        rc = DLG_ERR(d, already);
    else if (d->client) {
        u32 none = 0;
        if (!__atomic_compare_exchange_n(&g_slot, &none, d->id, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            rc = DLG_ERR(d, busy);
        else
            g_slot_name = d->name;
    }
    if (rc == 0) {
        d->status = DLG_INITIALIZED;
        d->t_fail = 0, d->stalled = 0;
    }
    spin_unlock(&d->lock);
    if (rc == DLG_ERR(d, busy))
        logf("dialog: %s Initialize -> %x: the CommonDialog slot is held by %s", d->name, (u32)rc,
             g_slot_name ? g_slot_name : "?");
    return rc;
}

s32 dlg_fail(struct dlg *d, s32 rc)
{
    u64 now = dlg_now();
    spin_lock(&d->lock);
    if (!d->t_fail && d->status != DLG_RUNNING)
        d->t_fail = now ? now : 1;
    spin_unlock(&d->lock);
    logf("dialog: %s Open -> %x", d->name, (u32)rc);
    return rc;
}

s32 dlg_check_open(struct dlg *d, const void *p, u32 size_off, u32 size_width, u32 size, u32 flags)
{
    u32 st = __atomic_load_n(&d->status, __ATOMIC_ACQUIRE);
    if (st != DLG_INITIALIZED && st != DLG_FINISHED)
        return dlg_fail(d, DLG_ERR(d, state));
    if (!p)
        return dlg_fail(d, DLG_ERR(d, null));
    bool lenient = flags & DLG_LENIENT;
    if (flags & DLG_BASE) {
        /* SceCommonDialogBaseParam: u64 size 0x30, ..., u32 magic at 0x2C */
        u64 base = RD64(p, 0);
        u32 magic = RD32(p, 0x2c), want = 0xC0D1A109u + (u32)(uintptr_t)p;
        if (base != 0x30) {
            logf("dialog: %s Open: base param size %lx, not 0x30%s", d->name, base, lenient ? " (accepted)" : "");
            if (!lenient)
                return dlg_fail(d, DLG_ERR(d, param));
        }
        if (magic != want) /* our layout is wrong, not the title: accept */
            logf("dialog: %s Open: base param magic %x, expected %x (accepted)", d->name, magic, want);
    }
    if (size) {
        u64 got = size_width == 8 ? RD64(p, size_off) : RD32(p, size_off);
        if (got != size) {
            logf("dialog: %s Open: param size %lx, not %x%s", d->name, got, size, lenient ? " (accepted)" : "");
            if (!lenient)
                return dlg_fail(d, DLG_ERR(d, param));
        }
    }
    return 0;
}

u32 dlg_start(struct dlg *d, u32 dflt, u64 ra, const char *what, const char *text)
{
    char where[64], t[124];
    const char *otext;
    u64 off = dlg_where(ra, where, sizeof(where));
    u32 a = dlg_policy(d->id, dflt, off, &otext);
    u64 now = dlg_now();
    spin_lock(&d->lock);
    d->status = DLG_RUNNING;
    d->answer = a;
    d->polls = 1; /* RUNNING for one poll, then the answer */
    d->t_open = now;
    d->t_fail = 0, d->stalled = 0, d->timed_out = 0;
    d->result = 0, d->button = 0;
    spin_unlock(&d->lock);
    dlg_text(t, sizeof(t), text);
    logf("dialog: %s %s%s%s%s -> %s (%s) @%s", d->name, what, text ? " \"" : "", t, text ? "\"" : "",
         dlg_answer_name(a), a == dflt ? "auto" : "override", where);
    return a;
}

u32 dlg_update(struct dlg *d)
{
    u64 now = dlg_now();
    bool timeout = false, stall = false;
    spin_lock(&d->lock);
    u32 st = d->status;
    if (st == DLG_RUNNING) {
        if (d->answer != ANS_HOLD) {
            if (d->polls)
                d->polls--;
            else
                finish(d, d->answer);
        } else if (now - d->t_open >= DLG_HOLD_US) {
            finish(d, ANS_CLOSE);
            timeout = d->timed_out = 1;
        }
        st = d->status;
    } else if (st != DLG_FINISHED && d->t_fail && now - d->t_fail >= DLG_HOLD_US) {
        /* Open failed but the title keeps waiting for FINISHED (Ghost's save
         * dialog helper ignores Open's result and never times out) */
        stall = !d->stalled;
        d->stalled = 1;
        if (st == DLG_INITIALIZED)
            finish(d, ANS_CANCEL);
        st = DLG_FINISHED;
    }
    spin_unlock(&d->lock);
    if (timeout)
        logf("dialog: %s hold timeout (10 s without Close) -> CLOSE", d->name);
    if (stall)
        logf("dialog: %s stall guard: Open failed but the title polls for 10 s -> FINISHED (CANCEL)", d->name);
    return st;
}

s32 dlg_check_result(struct dlg *d, const void *r)
{
    u32 st = __atomic_load_n(&d->status, __ATOMIC_ACQUIRE);
    if (st == DLG_NONE)
        return DLG_ERR(d, not_init);
    if (st != DLG_FINISHED)
        return DLG_ERR(d, not_finished);
    return r ? 0 : DLG_ERR(d, null);
}

s32 dlg_result_none(struct dlg *d, void *r)
{
    s32 rc = dlg_check_result(d, r);
    return rc ? rc : DLG_E_RESULT_NONE; /* the layout is unknown: write nothing */
}

s32 dlg_close(struct dlg *d)
{
    spin_lock(&d->lock);
    bool running = d->status == DLG_RUNNING, hold = d->answer == ANS_HOLD;
    if (running)
        finish(d, ANS_CLOSE);
    spin_unlock(&d->lock);
    if (!running)
        return DLG_ERR(d, not_running);
    if (hold)
        logf("dialog: %s closed by the title", d->name);
    return 0;
}

s32 dlg_terminate(struct dlg *d)
{
    spin_lock(&d->lock);
    u32 st = d->status;
    d->status = DLG_NONE;
    d->t_fail = 0, d->stalled = 0;
    if (st != DLG_NONE && d->client) {
        u32 me = d->id;
        __atomic_compare_exchange_n(&g_slot, &me, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    spin_unlock(&d->lock);
    return st == DLG_NONE ? DLG_ERR(d, not_init) : 0;
}

s32 dlg_progress(struct dlg *d, u32 mode, u32 target)
{
    if (d->status != DLG_RUNNING)
        return DLG_ERR(d, not_running);
    if (d->mode != mode)
        return DLG_E_NOT_SUPPORTED;
    if (target != 0)
        return DLG_E_PARAM;
    d->t_open = dlg_now(); /* the hold timeout counts from the last update */
    return 0;
}

s32 dlg_open_cancel(struct dlg *d, const void *p, u64 ra, const char *what)
{
    s32 rc = dlg_check_open(d, p, 0x30, 8, 0, DLG_BASE | DLG_LENIENT);
    if (rc)
        return rc;
    dlg_start(d, ANS_CANCEL, ra, what, NULL);
    return 0;
}

/* ---- libSceCommonDialog ---- */
static s32 hle_sceCommonDialogInitialize(void)
{
    static volatile int done;
    /* SWRR initialized the real one: every boot logged 0x80B80002 there */
    return __atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL) ? (s32)0x80B80002 : 0;
}

static bool hle_sceCommonDialogIsUsed(void) { return __atomic_load_n(&g_slot, __ATOMIC_ACQUIRE) != 0; }

/* ---- libSceMsgDialog ---- */
static struct dlg g_msg = DLG_INIT("Msg", DLG_MSG, dlg_errs_common, 1);
DLG_FNS(msg, g_msg)

/* USER_MSG button types: name and the button with default focus */
static const char *const k_msg_buttons[] = {
    "OK", "YESNO", "NONE", "OK_CANCEL", NULL, "WAIT", "WAIT_CANCEL", "YESNO_FOCUS_NO", "OK_CANCEL_FOCUS_CANCEL",
    "TWO_BUTTONS",
};
static const u8 k_msg_answer[] = {ANS_BTN1, ANS_BTN1, ANS_HOLD, ANS_BTN1, 0,         ANS_HOLD,
                                  ANS_HOLD, ANS_BTN2, ANS_CANCEL, ANS_BTN1};

/* MsgParam (0x88): BaseParam, u64 size @30, u32 mode @38, UserMsgParam* @40,
 * ProgressBarParam* @48, SystemMsgParam* @50, s32 userId @58 */
static s32 hle_sceMsgDialogOpen(const void *p)
{
    u64 ra = (u64)__builtin_return_address(0);
    s32 rc = dlg_check_open(&g_msg, p, 0x30, 8, 0x88, DLG_BASE);
    if (rc)
        return rc;
    u32 mode = RD32(p, 0x38);
    const u8 *sub = mode >= 1 && mode <= 3 ? RDPTR(p, 0x40 + 8 * (mode - 1)) : NULL;
    if (!sub)
        return dlg_fail(&g_msg, DLG_E_PARAM);
    char what[160], l1[40], l2[40];
    const char *text;
    u32 a, kind = RD32(sub, 0);
    if (mode == 1) { /* UserMsgParam: u32 buttonType, const char *msg @8, ButtonsParam* @10 */
        if (kind >= ARRAY_SIZE(k_msg_buttons) || !k_msg_buttons[kind])
            return dlg_fail(&g_msg, DLG_E_PARAM);
        text = (const char *)RDPTR(sub, 8);
        a = k_msg_answer[kind];
        const u8 *btn = RDPTR(sub, 0x10); /* ButtonsParam: msg1, msg2 */
        if (kind == 9 && btn) {
            dlg_text(l1, sizeof(l1), (const char *)RDPTR(btn, 0));
            dlg_text(l2, sizeof(l2), (const char *)RDPTR(btn, 8));
            fmt(what, sizeof(what), "USER_MSG TWO_BUTTONS [%s|%s]", l1, l2);
        } else
            fmt(what, sizeof(what), "USER_MSG %s", k_msg_buttons[kind]);
    } else if (mode == 2) { /* ProgressBarParam: u32 barType (0 percentage, 1 with cancel), msg @8 */
        if (kind > 1)
            return dlg_fail(&g_msg, DLG_E_PARAM);
        text = (const char *)RDPTR(sub, 8);
        a = ANS_HOLD;
        fmt(what, sizeof(what), "PROGRESS_BAR %u", kind);
    } else { /* SystemMsgParam: u32 sysMsgType (0 empty store, 1-2 PSN restrictions, 4 camera ...) */
        text = NULL;
        a = ANS_BTN1;
        fmt(what, sizeof(what), "SYSTEM_MSG %u", kind);
    }
    g_msg.mode = mode, g_msg.value = 0;
    dlg_start(&g_msg, a, ra, what, text);
    return 0;
}

/* MsgResult (0x2C): u32 mode, s32 result, u32 buttonId, reserved */
static s32 hle_sceMsgDialogGetResult(void *r)
{
    s32 rc = dlg_check_result(&g_msg, r);
    if (rc == 0) {
        WR32(r, 0, 0); /* what the real one writes here is unprobed (P-6) */
        WR32(r, 4, g_msg.result);
        WR32(r, 8, g_msg.button);
    }
    return rc;
}

static s32 hle_sceMsgDialogProgressBarInc(u32 target, u32 delta)
{
    s32 rc = dlg_progress(&g_msg, 2, target);
    if (rc == 0) {
        u32 old = g_msg.value;
        g_msg.value += delta;
        if (old < 100 && g_msg.value >= 100)
            logf("dialog: Msg progress 100%%");
    }
    return rc;
}

static s32 hle_sceMsgDialogProgressBarSetValue(u32 target, u32 rate)
{
    s32 rc = dlg_progress(&g_msg, 2, target);
    if (rc == 0)
        g_msg.value = rate;
    return rc;
}

static s32 hle_sceMsgDialogProgressBarSetMsg(u32 target, const char *msg)
{
    s32 rc = dlg_progress(&g_msg, 2, target);
    if (rc == 0) {
        char t[124];
        dlg_text(t, sizeof(t), msg);
        logf("dialog: Msg progress \"%s\"", t);
    }
    return rc;
}

/* ---- libSceErrorDialog: no BaseParam, no slot, no result ---- */
static struct dlg g_errdlg = DLG_INIT("Error", DLG_ERROR, dlg_errs_error, 0);
DLG_FNS(errdlg, g_errdlg)

/* ErrParam (0x10): s32 size, s32 errorCode, s32 userId, s32 reserved */
static s32 errdlg_open(const void *p, u64 ra, u32 size, const char *what)
{
    s32 rc = dlg_check_open(&g_errdlg, p, 0, 4, size, 0);
    if (rc)
        return rc;
    char w[64];
    fmt(w, sizeof(w), "%s error %x user %x", what, RD32(p, 4), RD32(p, 8));
    dlg_start(&g_errdlg, ANS_CANCEL, ra, w, NULL);
    return 0;
}

static s32 hle_sceErrorDialogOpen(const void *p)
{
    return errdlg_open(p, (u64)__builtin_return_address(0), 0x10, "Open");
}

/* layouts unknown (P-6): no size check; the code is the second word */
static s32 hle_sceErrorDialogOpenDetail(const void *p)
{
    return errdlg_open(p, (u64)__builtin_return_address(0), 0, "OpenDetail");
}

static s32 hle_sceErrorDialogOpenWithReport(const void *p)
{
    return errdlg_open(p, (u64)__builtin_return_address(0), 0, "OpenWithReport");
}

static const struct ext_hle_ent k_hle[] = {
    {"libSceCommonDialog", "sceCommonDialogInitialize", hle_sceCommonDialogInitialize, NULL},
    {"libSceCommonDialog", "sceCommonDialogIsUsed", hle_sceCommonDialogIsUsed, NULL},

    {"libSceMsgDialog", "sceMsgDialogInitialize", msg_init, NULL},
    {"libSceMsgDialog", "sceMsgDialogOpen", hle_sceMsgDialogOpen, NULL},
    {"libSceMsgDialog", "sceMsgDialogUpdateStatus", msg_update, NULL},
    {"libSceMsgDialog", "sceMsgDialogGetStatus", msg_update, NULL},
    {"libSceMsgDialog", "sceMsgDialogGetResult", hle_sceMsgDialogGetResult, NULL},
    {"libSceMsgDialog", "sceMsgDialogClose", msg_close, NULL},
    {"libSceMsgDialog", "sceMsgDialogTerminate", msg_term, NULL},
    {"libSceMsgDialog", "sceMsgDialogProgressBarInc", hle_sceMsgDialogProgressBarInc, NULL},
    {"libSceMsgDialog", "sceMsgDialogProgressBarSetValue", hle_sceMsgDialogProgressBarSetValue, NULL},
    {"libSceMsgDialog", "sceMsgDialogProgressBarSetMsg", hle_sceMsgDialogProgressBarSetMsg, NULL},

    {"libSceErrorDialog", "sceErrorDialogInitialize", errdlg_init, NULL},
    {"libSceErrorDialog", "sceErrorDialogOpen", hle_sceErrorDialogOpen, NULL},
    {"libSceErrorDialog", "sceErrorDialogOpenDetail", hle_sceErrorDialogOpenDetail, NULL},
    {"libSceErrorDialog", "sceErrorDialogOpenWithReport", hle_sceErrorDialogOpenWithReport, NULL},
    {"libSceErrorDialog", "sceErrorDialogUpdateStatus", errdlg_update, NULL},
    {"libSceErrorDialog", "sceErrorDialogGetStatus", errdlg_update, NULL},
    {"libSceErrorDialog", "sceErrorDialogClose", errdlg_close, NULL},
    {"libSceErrorDialog", "sceErrorDialogTerminate", errdlg_term, NULL},

    /* not a system library: run-time answer overrides (and the tests) */
    {"libGhostldrDialog", "ghostldrDialogOverride", hle_ghostldrDialogOverride, NULL},
};

/* No host fallback: a real dialog in SWRR never gets focus, and imports this
 * family lacks (CommonDialogUtil internals) must not reach SWRR's copy */
static const struct ext_lib k_libs[] = {
    {"libSceCommonDialog", NULL, EXT_LIB_NO_HOST, DLG_E_NOT_SUPPORTED},
    {"libSceMsgDialog", NULL, EXT_LIB_NO_HOST, DLG_E_NOT_SUPPORTED},
    {"libSceErrorDialog", NULL, EXT_LIB_NO_HOST, (s32)0x80ED0005}, /* INVALID_STATE */
};

DLG_MINSIZE_END

EXT_FAMILY_LIBS("dialog", k_hle, k_libs, NULL);
