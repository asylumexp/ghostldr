/* libSceImeDialog and libSceIme (dialogs.md §8.5-8.6).  A real keyboard
 * would get no focus inside SWRR.  An ImeDialog over frames ghostldr can
 * draw on is ghostldr's own on-screen keyboard (dlg_overlay.c): the user
 * types the text.  Otherwise (and for libSceIme) the text is decided here,
 * the same way every time:
 *   1. an override for the call site (hle_dialog.c) gives its UTF-8 text;
 *   2. passwords, URLs and mail addresses: UserCanceled, buffer untouched;
 *   3. a buffer the title already filled (its own suggestion): kept, Ok;
 *   4. an empty number field: UserCanceled;
 *   5. otherwise the console user's name (sceUserServiceGetUserName, the
 *      real one), "Player" when that fails or with --ime-fixed-name; Ok.
 * The title's text filter, if any, then sees the text.  ImeDialog statuses
 * are None 0, Running 1, Finished 2 (not the CommonDialog values), and
 * neither library uses the CommonDialog slot. */
#include "hle_dialog.h"
DLG_MINSIZE_BEGIN

typedef u16 char16;

#define IME_E_BUSY ((s32)0x80BC0001)
#define IME_E_NOT_OPENED ((s32)0x80BC0002)
#define IME_E_INVALID_USER_ID ((s32)0x80BC0010)
#define IME_E_INVALID_TYPE ((s32)0x80BC0011)
#define IME_E_INVALID_MAX_TEXT_LENGTH ((s32)0x80BC0016)
#define IME_E_INVALID_INPUT_TEXT_BUFFER ((s32)0x80BC0017)
#define IME_E_INVALID_HORIZONTAL_ALIGNMENT ((s32)0x80BC001A)
#define IME_E_INVALID_VERTICAL_ALIGNMENT ((s32)0x80BC001B)
#define IME_E_INVALID_HANDLER ((s32)0x80BC0022)
#define IME_E_INVALID_ADDRESS ((s32)0x80BC0031)
#define IMED_E_NOT_RUNNING ((s32)0x80BC0105)
#define IMED_E_NOT_FINISHED ((s32)0x80BC0106)
#define IMED_E_NOT_IN_USE ((s32)0x80BC0107)

#define IME_OPT_MULTILINE 1
#define IME_OPT_PASSWORD 4
#define IME_OPT_OVER_2K 0x4000
enum { TYPE_DEFAULT, TYPE_BASIC_LATIN, TYPE_URL, TYPE_MAIL, TYPE_NUMBER };
enum { HOW_KEEP, HOW_TEXT, HOW_NAME, HOW_CANCEL, HOW_TYPED }; /* the text policy's outcome */
enum { END_OK, END_USER_CANCELED, END_ABORTED };

/* What ImeDialogParam and ImeParam share (offsets up to 0x40):
 * s32 user_id, u32 type @4, u64 languages @8, u32 enter_label @10,
 * u32 input_method @14, filter @18, u32 option @20, u32 max_text_length @24,
 * char16 *input_text_buffer @28, f32 posx @30, f32 posy @34, u32 h_align @38,
 * u32 v_align @3C */
struct ime_session {
    char16 *buf;
    u32 max, type, option, how;
    s32 user;
    void *filter;
    u32 posx, posy, halign, valign; /* f32 bits */
    char text[64];                  /* the override's text (HOW_TEXT) */
};

static void ime_save(struct ime_session *s, const void *p)
{
    s->user = (s32)RD32(p, 0), s->type = RD32(p, 4), s->filter = (void *)RDPTR(p, 0x18);
    s->option = RD32(p, 0x20), s->max = RD32(p, 0x24), s->buf = (char16 *)RDPTR(p, 0x28);
    s->posx = RD32(p, 0x30), s->posy = RD32(p, 0x34), s->halign = RD32(p, 0x38), s->valign = RD32(p, 0x3c);
}

static u32 len16(const char16 *s, u32 max)
{
    u32 n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

/* UTF-8 to UTF-16, BMP only (anything else becomes '?'); NUL-terminated */
static u32 utf8_to_16(char16 *out, u32 max, const char *s)
{
    const u8 *p = (const u8 *)s;
    u32 n = 0;
    while (p && *p && n < max) {
        u32 c = *p++, more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (c >= 0x80) {
            c = more ? c & (0x3f >> more) : '?';
            for (; more && (*p & 0xc0) == 0x80; more--)
                c = c << 6 | (*p++ & 0x3f);
            if (more || c > 0xffff || (c >= 0xd800 && c < 0xe000))
                c = '?';
        }
        out[n++] = (char16)c;
    }
    out[n] = 0;
    return n;
}

/* for logs: UTF-16 to ASCII, at most n-1 characters */
static void ascii16(char *out, size_t n, const char16 *s)
{
    size_t i = 0;
    for (; s && s[i] && i + 1 < n; i++)
        out[i] = s[i] >= 0x20 && s[i] < 0x7f ? (char)s[i] : '?';
    out[i] = 0;
}

/* rules 1-4 (rule 5 is HOW_NAME); answer is dlg_policy's, otext its text */
static u32 ime_decide(const struct ime_session *s, u32 answer, const char *otext)
{
    if (answer == ANS_CANCEL)
        return HOW_CANCEL;
    if (otext)
        return HOW_TEXT;
    if ((s->option & IME_OPT_PASSWORD) || s->type == TYPE_URL || s->type == TYPE_MAIL)
        return HOW_CANCEL;
    if (s->buf[0])
        return HOW_KEEP;
    return s->type == TYPE_NUMBER ? HOW_CANCEL : HOW_NAME;
}

/* write the decided text into the title's buffer (max + 1 char16) */
static void ime_commit(struct ime_session *s)
{
    if (s->how == HOW_TEXT)
        utf8_to_16(s->buf, s->max, s->text);
    if (s->how == HOW_NAME) {
        char name[64] = "";
        s32 (*get)(s32, char *, size_t) = NULL;
        if (!(g_ctx->flags & CTX_FLAG_IME_FIXED_NAME))
            get = (s32(*)(s32, char *, size_t))host_real_in("libSceUserService", "sceUserServiceGetUserName");
        if (!get || get(s->user, name, sizeof(name)) != 0 || !name[0])
            strlcpy(name, "Player", sizeof(name));
        u32 n = utf8_to_16(s->buf, s->max, name);
        logf("dialog: IME text is the user name (%u characters)", n);
    }
    if (s->filter && (s->how == HOW_TEXT || s->how == HOW_NAME || s->how == HOW_TYPED)) {
        char16 out[2049];
        u32 n = 0;
        s32 rc = ((s32(*)(char16 *, u32 *, const char16 *, u32))s->filter)(out, &n, s->buf, len16(s->buf, s->max));
        if (rc == 0) {
            n = n < s->max ? n : s->max;
            memcpy(s->buf, out, n * sizeof(char16));
            s->buf[n] = 0;
        }
        logf("dialog: IME text filter -> %x", (u32)rc);
    }
}

static const char *const k_how[] = {"OK (the title's text kept)", "OK (override text)", "OK (user name)",
                                    "USER_CANCELED", "on screen: the user types"};

static void ime_log(const char *lib, const struct ime_session *s, u64 ra, const char16 *title)
{
    char where[64], t[64], cur[64];
    dlg_where(ra, where, sizeof(where));
    ascii16(t, sizeof(t), title);
    ascii16(cur, sizeof(cur), s->buf);
    logf("dialog: %s type %u option %x max %u title \"%s\" text \"%s\" -> %s @%s", lib, s->type, s->option, s->max,
         t, cur, k_how[s->how], where);
}

static void panel_size(u32 type, u32 option, bool dialog, u32 *w, u32 *h)
{
    u32 k = option & IME_OPT_OVER_2K ? 2 : 1;
    *w = (type == TYPE_NUMBER ? 370 : 793) * k;
    *h = (type == TYPE_NUMBER ? (dialog ? 470 : 402) : dialog ? (option & IME_OPT_MULTILINE ? 628 : 528) : 408) * k;
}

/* ---- libSceImeDialog ---- */
static struct {
    volatile int lock;
    u32 status, polls, end;
    bool committing, drawn;
    struct ime_session s;
} g_imed;

/* ImeDialogParam (0x60): the shared part, then placeholder @40, title @48 */
static s32 hle_sceImeDialogInit(const void *p, const void *ext)
{
    u64 ra = (u64)__builtin_return_address(0);
    if (g_imed.status)
        return IME_E_BUSY;
    if (!p)
        return IME_E_INVALID_ADDRESS;
    struct ime_session s;
    memset(&s, 0, sizeof(s));
    ime_save(&s, p);
    if (!s.buf)
        return IME_E_INVALID_INPUT_TEXT_BUFFER;
    if (!s.max || s.max > 2048)
        return IME_E_INVALID_MAX_TEXT_LENGTH;
    if (s.type > TYPE_NUMBER)
        return IME_E_INVALID_TYPE;
    if (s.halign >= 3)
        return IME_E_INVALID_HORIZONTAL_ALIGNMENT;
    if (s.valign >= 3)
        return IME_E_INVALID_VERTICAL_ALIGNMENT;
    char where[8];
    const char *otext;
    u32 a = dlg_policy(DLG_IMEDIALOG, ANS_BTN1, dlg_where(ra, where, sizeof(where)), &otext);
    if (otext)
        strlcpy(s.text, otext, sizeof(s.text));
    s.how = ime_decide(&s, a, otext);
    spin_lock(&g_imed.lock);
    bool busy = g_imed.status != 0;
    spin_unlock(&g_imed.lock);
    if (busy)
        return IME_E_BUSY;
    /* no override and no --dialog-cancel: the user types, where it can be drawn */
    bool drawn = a == ANS_BTN1 && !otext &&
                 ov_ime_open(s.buf, s.max, s.type, s.option, (const char16 *)RDPTR(p, 0x48));
    if (drawn)
        s.how = HOW_TYPED;
    spin_lock(&g_imed.lock);
    g_imed.s = s;
    g_imed.status = 1, g_imed.polls = 1, g_imed.end = END_OK, g_imed.committing = false, g_imed.drawn = drawn;
    spin_unlock(&g_imed.lock);
    if (ext)
        logf("dialog: ImeDialog extended option %x", RD32(ext, 0));
    ime_log("ImeDialog", &s, ra, (const char16 *)RDPTR(p, 0x48));
    return 0;
}

static u32 hle_sceImeDialogGetStatus(void)
{
    if (__atomic_load_n(&g_imed.status, __ATOMIC_ACQUIRE) == 1 && g_imed.drawn) {
        ov_tick();
        int st = ov_ime_state(g_imed.s.buf, g_imed.s.max);
        if (st == 0)
            return 1;
        bool commit = false;
        spin_lock(&g_imed.lock);
        if (g_imed.status == 1 && !g_imed.committing)
            commit = g_imed.committing = true;
        spin_unlock(&g_imed.lock);
        if (commit) {
            if (st == 1)
                ime_commit(&g_imed.s); /* the title's filter */
            logf("dialog: ImeDialog %s on screen (%u characters)", st == 1 ? "entered" : "backed out of",
                 len16(g_imed.s.buf, g_imed.s.max));
            g_imed.end = st == 1 ? END_OK : END_USER_CANCELED;
            ov_ime_end();
            __atomic_store_n(&g_imed.status, 2, __ATOMIC_RELEASE);
        }
        return __atomic_load_n(&g_imed.status, __ATOMIC_ACQUIRE);
    }
    bool commit = false;
    spin_lock(&g_imed.lock);
    u32 st = g_imed.status;
    if (st == 1 && g_imed.polls)
        g_imed.polls--;
    else if (st == 1 && !g_imed.committing)
        commit = g_imed.committing = true;
    spin_unlock(&g_imed.lock);
    if (commit) { /* outside the lock: the title's filter may run */
        ime_commit(&g_imed.s);
        g_imed.end = g_imed.s.how == HOW_CANCEL ? END_USER_CANCELED : END_OK;
        __atomic_store_n(&g_imed.status, st = 2, __ATOMIC_RELEASE);
    }
    return st;
}

/* ImeDialogResult (0x10): u32 endstatus */
static s32 hle_sceImeDialogGetResult(void *r)
{
    if (!g_imed.status)
        return IMED_E_NOT_IN_USE;
    if (!r)
        return IME_E_INVALID_ADDRESS;
    if (g_imed.status != 2)
        return IMED_E_NOT_FINISHED;
    WR32(r, 0, g_imed.end);
    return 0;
}

static s32 hle_sceImeDialogTerm(void)
{
    if (!g_imed.status)
        return IMED_E_NOT_IN_USE;
    if (g_imed.status != 2)
        return IMED_E_NOT_FINISHED;
    g_imed.status = 0;
    return 0;
}

static s32 hle_sceImeDialogAbort(void)
{
    if (!g_imed.status)
        return IMED_E_NOT_IN_USE;
    if (g_imed.status != 1)
        return IMED_E_NOT_RUNNING;
    g_imed.end = END_ABORTED, g_imed.status = 2; /* the buffer stays as it was */
    if (g_imed.drawn)
        ov_ime_end();
    logf("dialog: ImeDialog aborted by the title");
    return 0;
}

static s32 hle_sceImeDialogForceClose(void)
{
    if (!g_imed.status)
        return IMED_E_NOT_IN_USE;
    if (g_imed.drawn && g_imed.status == 1)
        ov_ime_end();
    g_imed.status = 0;
    return 0;
}

static s32 hle_sceImeDialogGetPanelSize(const void *p, u32 *w, u32 *h)
{
    if (!p || !w || !h)
        return IME_E_INVALID_ADDRESS;
    panel_size(RD32(p, 4), RD32(p, 0x20), true, w, h);
    return 0;
}

static s32 hle_sceImeDialogGetPanelSizeExtended(const void *p, const void *ext, u32 *w, u32 *h)
{
    return hle_sceImeDialogGetPanelSize(p, w, h);
}

/* ImePositionAndForm (0x1C): u32 type, f32 x, f32 y, u32 h_align, u32 v_align, u32 w, u32 h */
static void position_and_form(u32 *o, u32 type, const struct ime_session *s, bool dialog)
{
    o[0] = type, o[1] = s->posx, o[2] = s->posy, o[3] = s->halign, o[4] = s->valign;
    panel_size(s->type, s->option, dialog, &o[5], &o[6]);
}

static s32 hle_sceImeDialogGetPanelPositionAndForm(u32 *o)
{
    if (!g_imed.status)
        return IMED_E_NOT_IN_USE;
    if (!o)
        return IME_E_INVALID_ADDRESS;
    position_and_form(o, 2 /* dialog */, &g_imed.s, true);
    return 0;
}

static s32 hle_ime_ok(void) { return 0; }
static s32 hle_ime_not_in_use(void) { return IMED_E_NOT_IN_USE; }

/* ---- libSceIme: events through the title's handler ---- */
enum { EV_OPEN = 0, EV_UPDATE_TEXT = 1, EV_PRESS_CLOSE = 4, EV_PRESS_ENTER = 5 };
static struct {
    volatile int lock;
    bool open, kbd;
    u8 queue[4], nq, next;
    void (*handler)(void *arg, const void *ev);
    void *arg;
    struct ime_session s;
} g_ime;

static void hle_sceImeParamInit(void *p)
{
    if (p) {
        memset(p, 0, 0x60);
        WR32(p, 0, (u32)-1); /* user_id: none */
    }
}

/* ImeParam (0x60): the shared part, then void *work @40, void *arg @48, handler @50 */
static s32 hle_sceImeOpen(const void *p, const void *ext)
{
    u64 ra = (u64)__builtin_return_address(0);
    if (g_ime.open)
        return IME_E_BUSY;
    if (!p)
        return IME_E_INVALID_ADDRESS;
    if (!RDPTR(p, 0x50))
        return IME_E_INVALID_HANDLER;
    struct ime_session s;
    memset(&s, 0, sizeof(s));
    ime_save(&s, p);
    if (!s.buf)
        return IME_E_INVALID_INPUT_TEXT_BUFFER;
    char where[8];
    const char *otext;
    u32 a = dlg_policy(DLG_IME, ANS_BTN1, dlg_where(ra, where, sizeof(where)), &otext);
    if (otext)
        strlcpy(s.text, otext, sizeof(s.text));
    s.how = ime_decide(&s, a, otext);
    spin_lock(&g_ime.lock);
    bool busy = g_ime.open;
    if (!busy) {
        g_ime.s = s;
        g_ime.handler = (void (*)(void *, const void *))RDPTR(p, 0x50);
        g_ime.arg = (void *)RDPTR(p, 0x48);
        g_ime.queue[0] = EV_OPEN;
        if (s.how == HOW_CANCEL)
            g_ime.queue[1] = EV_PRESS_CLOSE, g_ime.nq = 2;
        else
            g_ime.queue[1] = EV_UPDATE_TEXT, g_ime.queue[2] = EV_PRESS_ENTER, g_ime.nq = 3;
        g_ime.next = 0;
        g_ime.open = true;
    }
    spin_unlock(&g_ime.lock);
    if (busy)
        return IME_E_BUSY;
    ime_log("Ime", &s, ra, NULL);
    return 0;
}

/* one queued event per call, on the caller's thread.  SceImeEvent (0x48):
 * u32 id, then @8 a SceImeRect {f32 x, f32 y, u32 w, u32 h} or a
 * SceImeEditText {char16 *str, u32 caret_index, u32 area_num,
 * {u32 mode, u32 index, s32 length}[4]} */
static s32 hle_sceImeUpdate(void (*h)(void *arg, const void *ev))
{
    spin_lock(&g_ime.lock);
    bool open = g_ime.open, have = open && g_ime.next < g_ime.nq;
    u32 id = have ? g_ime.queue[g_ime.next++] : 0;
    void (*fn)(void *, const void *) = h ? h : g_ime.handler;
    void *arg = g_ime.arg;
    spin_unlock(&g_ime.lock);
    if (!open)
        return IME_E_NOT_OPENED;
    if (!have || !fn)
        return 0;
    u8 ev[0x48];
    memset(ev, 0, sizeof(ev));
    WR32(ev, 0, id);
    if (id == EV_OPEN) {
        WR32(ev, 8, g_ime.s.posx), WR32(ev, 0xc, g_ime.s.posy);
        panel_size(g_ime.s.type, g_ime.s.option, false, (u32 *)(ev + 0x10), (u32 *)(ev + 0x14));
    } else if (id != EV_PRESS_CLOSE) {
        if (id == EV_UPDATE_TEXT)
            ime_commit(&g_ime.s);
        u32 len = len16(g_ime.s.buf, g_ime.s.max);
        WR64(ev, 8, g_ime.s.buf);
        WR32(ev, 0x10, len); /* caret after the text */
        WR32(ev, 0x14, 1);   /* one area: */
        WR32(ev, 0x18, 1);   /* EDIT */
        WR32(ev, 0x20, len); /* index 0, the whole text */
    }
    fn(arg, ev); /* on the caller's thread, outside the lock */
    return 0;
}

static s32 hle_sceImeClose(void)
{
    spin_lock(&g_ime.lock);
    bool was = g_ime.open;
    g_ime.open = false, g_ime.nq = g_ime.next = 0;
    spin_unlock(&g_ime.lock);
    return was ? 0 : IME_E_NOT_OPENED;
}

static s32 hle_sceImeSetText(const char16 *t, u32 len)
{
    if (!g_ime.open)
        return IME_E_NOT_OPENED;
    if (t && g_ime.s.buf) {
        len = len < g_ime.s.max ? len : g_ime.s.max;
        memcpy(g_ime.s.buf, t, len * sizeof(char16));
        g_ime.s.buf[len] = 0;
    }
    return 0;
}

static s32 hle_sceImeSetCaret(const void *c) { return g_ime.open ? 0 : IME_E_NOT_OPENED; }

static s32 hle_sceImeGetPanelSize(const void *p, u32 *w, u32 *h)
{
    if (!p || !w || !h)
        return IME_E_INVALID_ADDRESS;
    panel_size(RD32(p, 4), RD32(p, 0x20), false, w, h);
    return 0;
}

static s32 hle_sceImeGetPanelPositionAndForm(u32 *o)
{
    if (!o)
        return IME_E_INVALID_ADDRESS;
    position_and_form(o, 1 /* on-screen keyboard */, &g_ime.s, false);
    return 0;
}

/* ImeKeyboardParam (0x20): u32 option, void *arg @8, handler @10.  There is
 * no USB keyboard here: it opens and never sends an event. */
static s32 hle_sceImeKeyboardOpen(s32 user, const void *p)
{
    if (!p)
        return IME_E_INVALID_ADDRESS;
    if (!RDPTR(p, 0x10))
        return IME_E_INVALID_HANDLER;
    if (user == -1)
        return IME_E_INVALID_USER_ID;
    if (g_ime.kbd)
        return IME_E_BUSY;
    g_ime.kbd = true;
    return 0;
}

static s32 hle_sceImeKeyboardClose(s32 user)
{
    if (!g_ime.kbd)
        return IME_E_NOT_OPENED;
    g_ime.kbd = false;
    return 0;
}

static s32 hle_ime_not_opened(void) { return IME_E_NOT_OPENED; }

static s32 hle_ime_noop(void)
{
    static u32 logged;
    if (!logged++)
        logf("dialog: an IME candidate, controller or dictionary call: nothing to do");
    return 0;
}

#define IMED "libSceImeDialog"
#define IME "libSceIme"
static const struct ext_hle_ent k_hle[] = {
    {IMED, "sceImeDialogInit", hle_sceImeDialogInit, NULL},
    {IMED, "sceImeDialogGetStatus", hle_sceImeDialogGetStatus, NULL},
    {IMED, "sceImeDialogGetResult", hle_sceImeDialogGetResult, NULL},
    {IMED, "sceImeDialogTerm", hle_sceImeDialogTerm, NULL},
    {IMED, "sceImeDialogAbort", hle_sceImeDialogAbort, NULL},
    {IMED, "sceImeDialogForceClose", hle_sceImeDialogForceClose, NULL},
    {IMED, "sceImeDialogGetPanelSize", hle_sceImeDialogGetPanelSize, NULL},
    {IMED, "sceImeDialogGetPanelSizeExtended", hle_sceImeDialogGetPanelSizeExtended, NULL},
    {IMED, "sceImeDialogGetPanelPositionAndForm", hle_sceImeDialogGetPanelPositionAndForm, NULL},
    {IMED, "sceImeDialogSetPanelPosition", hle_ime_ok, NULL},
    {IMED, "sceImeDialogGetCurrentStarState", hle_ime_not_in_use, NULL},

    {IME, "sceImeParamInit", hle_sceImeParamInit, NULL},
    {IME, "sceImeOpen", hle_sceImeOpen, NULL},
    {IME, "sceImeUpdate", hle_sceImeUpdate, NULL},
    {IME, "sceImeClose", hle_sceImeClose, NULL},
    {IME, "sceImeSetText", hle_sceImeSetText, NULL},
    {IME, "sceImeSetCaret", hle_sceImeSetCaret, NULL},
    {IME, "sceImeGetPanelSize", hle_sceImeGetPanelSize, NULL},
    {IME, "sceImeGetPanelPositionAndForm", hle_sceImeGetPanelPositionAndForm, NULL},
    {IME, "sceImeSetTextGeometry", hle_ime_ok, NULL},
    {IME, "sceImeKeyboardOpen", hle_sceImeKeyboardOpen, NULL},
    {IME, "sceImeKeyboardClose", hle_sceImeKeyboardClose, NULL},
    {IME, "sceImeKeyboardGetResourceId", hle_ime_not_opened, NULL},
    {IME, "sceImeKeyboardGetInfo", hle_ime_not_opened, NULL},
    {IME, "sceImeKeyboardSetMode", hle_ime_ok, NULL},
    {IME, "sceImeKeyboardUpdate", hle_ime_ok, NULL},
    {IME, "sceImeConfirmCandidate", hle_ime_noop, NULL},
    {IME, "sceImeSetCandidateIndex", hle_ime_noop, NULL},
    {IME, "sceImeDisableController", hle_ime_noop, NULL},
    {IME, "sceImeDicAddWord", hle_ime_noop, NULL},
    {IME, "sceImeDicDeleteLearnDics", hle_ime_noop, NULL},
    {IME, "sceImeDicDeleteUserDics", hle_ime_noop, NULL},
    {IME, "sceImeDicDeleteWord", hle_ime_noop, NULL},
    {IME, "sceImeDicGetWords", hle_ime_noop, NULL},
    {IME, "sceImeDicReplaceWord", hle_ime_noop, NULL},
};

static const struct ext_lib k_libs[] = {
    {IMED, NULL, EXT_LIB_NO_HOST, IMED_E_NOT_IN_USE},
    {IME, NULL, EXT_LIB_NO_HOST, IME_E_NOT_OPENED},
};

DLG_MINSIZE_END

EXT_FAMILY_LIBS("dialog_ime", k_hle, k_libs, NULL);
