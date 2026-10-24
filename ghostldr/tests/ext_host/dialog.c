/*
 * The dialog families (ext/src/hle_dialog*.c) of a real ext.bin, driven in
 * an ordinary x86-64 process (Rosetta on Apple silicon).  The core's real
 * ext.c places, relocates and initializes the extension; this file fakes
 * the few core services it and the dialogs use (a clock, the user name,
 * the core's sysmodule override) and then calls the dialog functions the
 * way a title would, through the extension's own lookup.  test_dialog.py
 * builds and runs it:
 *
 *     dialog_host EXT_BIN CRC
 *
 * Prints every log line and "dialog_host: ok|FAILED [group] ..." per check; exits 0
 * when every check passed.
 */
#include "ghost_api.h"

/* sys.c: the host OS */
void *sys_map(u64 size);
void sys_protect(void *addr, u64 size, int writable);
int sys_open(const char *path);
s64 sys_read(int fd, void *buf, u64 n);
int sys_close(int fd);
void sys_vlog(const char *fmt, __builtin_va_list ap);
void *sys_calloc(u64 n);
s64 sys_read_file(const char *path, char **out);
u64 sys_strtoul(const char *s);
int sys_vfmt(char *buf, u64 size, const char *fmt, __builtin_va_list ap);

struct kfuncs K;
struct boot_ctx *g_ctx;
struct module *g_modules, *g_main_mod;
u64 g_reported_dmem;

/* ---- log: printed, and kept for the checks ---- */
static char g_log[1 << 20];
static u64 g_loglen, g_mark;

static void out(const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    sys_vlog(f, ap);
    __builtin_va_end(ap);
}

void logf(const char *f, ...)
{
    char line[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    sys_vfmt(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    out("%s", line);
    u64 n = strlen(line);
    if (g_loglen + n + 2 < sizeof(g_log)) {
        memcpy(g_log + g_loglen, line, n);
        g_loglen += n;
        g_log[g_loglen++] = '\n';
        g_log[g_loglen] = 0;
    }
}

/* the log since the last mark() contains s */
static void mark(void) { g_mark = g_loglen; }
static bool logged(const char *s) { return strstr(g_log + g_mark, s) != NULL; }

/* the hex number after "@eboot+" in the newest line that has one */
static u64 last_caller(void)
{
    const char *p = NULL;
    for (const char *q = g_log + g_mark; (q = strstr(q, "@eboot+")); q++)
        p = q + 7;
    u64 v = 0;
    for (; p && ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')); p++)
        v = v * 16 + (u64)(*p <= '9' ? *p - '0' : *p - 'a' + 10);
    return v;
}

static int g_failed;
static u32 g_checks;
static const char *g_group = "load"; /* printed with each check: test_dialog.py groups them */
static void expect(bool ok, const char *what)
{
    g_checks++;
    out("dialog_host: %s [%s] %s", ok ? "ok" : "FAILED", g_group, what);
    if (!ok)
        g_failed = 1;
}

static void expect_eq(u64 got, u64 want, const char *what)
{
    if (got != want)
        out("dialog_host:   got %llx, want %llx", (unsigned long long)got, (unsigned long long)want);
    expect(got == want, what);
}

/* ---- the core services ext.c and the extension use ---- */
void *arena_alloc(size_t n) { return sys_calloc(n); }
char *arena_strdup(const char *s)
{
    char *d = sys_calloc(strlen(s) + 1);
    memcpy(d, s, strlen(s));
    return d;
}
const char *strrchr_c(const char *s, int c)
{
    const char *r = NULL;
    for (; *s; s++)
        if (*s == c)
            r = s;
    return r;
}
void *page_alloc(size_t size) { return sys_map(ALIGN_UP(size, 0x4000)); }
void page_free(void *addr, size_t size) {}
u64 arena_small_alloc_top(u64 size, u64 keep) { return 0; } /* always a JIT block */
int arena_write(u64 dst, const void *src, u64 size)
{
    u64 lo = ALIGN_DOWN(dst, 0x4000), hi = ALIGN_UP(dst + size, 0x4000);
    sys_protect((void *)lo, hi - lo, 1);
    memcpy((void *)dst, src, size);
    sys_protect((void *)lo, hi - lo, 0);
    return 0;
}
int arena_punch_rw(u64 addr, u64 size)
{
    sys_protect((void *)addr, size, 1);
    memset((void *)addr, 0, size);
    return 0;
}
/* ext.c's ext_abandon (a USB app root that stops the boot): not reached here */
void jit_block_destroy(struct jit_block *b) {}
int jit_block_create(struct jit_block *b, u64 size, u64 want_rx)
{
    memset(b, 0, sizeof(*b));
    b->size = ALIGN_UP(size, 0x4000);
    b->rx = b->rw = (u64)sys_map(b->size);
    sys_protect((void *)b->rx, b->size, 0);
    return 0;
}
int jit_block_write(struct jit_block *b, u64 offset, const void *src, size_t len)
{
    return offset + len > b->size ? -1 : arena_write(b->rw + offset, src, len);
}
static void spin_lock_(volatile int *l)
{
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        ;
}
static void spin_unlock_(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

/* the clock, the user name, the sysmodule calls */
static u64 g_now = 1000000;
static u64 fake_process_time(void) { return g_now; }
static const char *g_user_name = "Ghost";
static s32 g_user_rc;
static s32 g_user_asked = -2;
static s32 fake_get_user_name(s32 user, char *buf, size_t size)
{
    g_user_asked = user;
    if (g_user_rc)
        return g_user_rc;
    strlcpy(buf, g_user_name, size);
    return 0;
}
static u32 g_core_loads, g_core_load_args, g_host_is_loaded;
static s32 fake_core_load(u32 id) { return g_core_loads++, 0; }
static s32 fake_core_load_arg(u32 id, size_t argc, const void *argv, u32 flags, s32 *res) { return g_core_load_args++, 0; }
static s32 fake_host_is_loaded(u32 id) { return g_host_is_loaded++, (s32)0x805A1001; }

static void *host_real_(const char *name)
{
    if (strcmp(name, "sceKernelGetProcessTime") == 0)
        return (void *)fake_process_time;
    if (strcmp(name, "sceSysmoduleIsLoaded") == 0)
        return (void *)fake_host_is_loaded;
    return NULL;
}
static void *host_real_in_(const char *mod, const char *name)
{
    if (strcmp(mod, "libSceUserService") == 0 && strcmp(name, "sceUserServiceGetUserName") == 0)
        return (void *)fake_get_user_name;
    return NULL;
}
static u64 hle_lookup_(const char *lib, const char *name)
{
    if (strcmp(name, "sceSysmoduleLoadModule") == 0)
        return (u64)fake_core_load;
    if (strcmp(name, "sceSysmoduleLoadModuleInternalWithArg") == 0)
        return (u64)fake_core_load_arg;
    return 0;
}
static const char *gfs_save_name_(void) { return "GHOSTSAVE"; }

const struct ghost_api g_ghost_api = {
    .version = GHOST_API_VERSION,
    .size = sizeof(struct ghost_api),
    .ctx = &g_ctx,
    .k = &K,
    .modules = &g_modules,
    .main_mod = &g_main_mod,
    .reported_dmem = &g_reported_dmem,
    .logf = logf,
    .host_real = host_real_,
    .host_real_in = host_real_in_,
    .compute_nid = compute_nid,
    .arena_alloc = arena_alloc,
    .arena_strdup = arena_strdup,
    .spin_lock = spin_lock_,
    .spin_unlock = spin_unlock_,
    .gfs_save_name = gfs_save_name_,
    .hle_lookup = hle_lookup_,
};

static int k_close(int fd) { return sys_close(fd); }
static ssize_t k_read(int fd, void *buf, size_t n) { return sys_read(fd, buf, n); }
static int k_map_flex(void **addr, size_t len, int prot, int flags)
{
    *addr = sys_map(len);
    return 0;
}
static int k_munmap(void *addr, size_t len) { return 0; }

/* ---- calling the extension like a title ---- */
static void *fn(const char *lib, const char *name)
{
    char nid[12];
    compute_nid(name, nid);
    void *f = (void *)ext_hle_lookup(lib, nid, name);
    if (!f) {
        out("dialog_host: FAILED %s:%s is not in the extension", lib, name);
        g_failed = 1;
    }
    return f;
}

typedef s32 (*f0)(void);
typedef u32 (*u0)(void);
typedef s32 (*f1)(const void *);
typedef s32 (*f2)(const void *, const void *);
typedef s32 (*f3)(const void *, const void *, const void *);
typedef s32 (*fu2)(u32, u32);
typedef s32 (*fus)(u32, const char *);
typedef s32 (*fov)(u32, u64, u32, const char *);
typedef s32 (*ffake)(const void *, const s64 *, u32);

/* one call site per helper: the override test keys on it */
static volatile u32 g_sink;
static NOINLINE s32 call1_site_a(void *f, const void *p)
{
    s32 r = ((f1)f)(p);
    g_sink++;
    return r;
}
static NOINLINE s32 call2_site_b(void *f, const void *p, const void *q)
{
    s32 r = ((f2)f)(p, q);
    g_sink++;
    return r;
}

/* a CommonDialog param: BaseParam (size 0x30, magic) and a size field */
static u8 *param(u64 *mem, u32 bytes, u32 size_off, u32 width, u64 size)
{
    u8 *p = (u8 *)mem;
    memset(p, 0, bytes);
    *(u64 *)p = 0x30;
    *(u32 *)(p + 0x2c) = 0xC0D1A109u + (u32)(uintptr_t)p;
    if (width == 8)
        *(u64 *)(p + size_off) = size;
    else
        *(u32 *)(p + size_off) = (u32)size;
    return p;
}
#define W32(p, off, v) (*(u32 *)((u8 *)(p) + (off)) = (u32)(v))
#define W64(p, off, v) (*(u64 *)((u8 *)(p) + (off)) = (u64)(v))
#define R32(p, off) (*(const u32 *)((const u8 *)(p) + (off)))
#define R64(p, off) (*(const u64 *)((const u8 *)(p) + (off)))

enum { ANS_CLOSE, ANS_BTN1, ANS_BTN2, ANS_CANCEL, ANS_HOLD };
enum { DLG_MSG = 1, DLG_SAVEDATA, DLG_ERROR, DLG_IMEDIALOG = 12, DLG_IME = 13 };
#define CTX_FLAG_DIALOG_CANCEL 0x80
#define CTX_FLAG_IME_FIXED_NAME 0x100

/* ---- MsgDialog: the state machine, the answer policy, param checks ---- */
static void *msg_init, *msg_open, *msg_update, *msg_status, *msg_result, *msg_close, *msg_term;
static u64 g_msgp[0x88 / 8], g_user[0x30 / 8], g_btns[0x30 / 8], g_prog[0x50 / 8], g_sys[0x24 / 8 + 1];

/* open a USER_MSG of buttonType bt, poll to the end, return (result << 8 | buttonId) or ~0 */
static u8 *msg_param(u32 mode)
{
    u8 *p = param(g_msgp, 0x88, 0x30, 8, 0x88);
    W32(p, 0x38, mode);
    W64(p, 0x40, g_user), W64(p, 0x48, g_prog), W64(p, 0x50, g_sys);
    return p;
}

static u32 msg_answer(u32 bt, const char *text)
{
    u8 *p = msg_param(1);
    W32(g_user, 0, bt);
    W64(g_user, 8, text);
    if (((f1)msg_open)(p) != 0)
        return ~0u;
    u32 st = 0;
    for (int i = 0; i < 4 && (st = ((u0)msg_update)()) == 2; i++)
        ;
    if (st != 3)
        return 0xdead;
    u32 r[11];
    memset(r, 0xee, sizeof(r));
    if (((f1)msg_result)(r) != 0)
        return 0xbad;
    return r[1] << 8 | r[2];
}

static void test_msg(void)
{
    msg_init = fn("libSceMsgDialog", "sceMsgDialogInitialize");
    msg_open = fn("libSceMsgDialog", "sceMsgDialogOpen");
    msg_update = fn("libSceMsgDialog", "sceMsgDialogUpdateStatus");
    msg_status = fn("libSceMsgDialog", "sceMsgDialogGetStatus");
    msg_result = fn("libSceMsgDialog", "sceMsgDialogGetResult");
    msg_close = fn("libSceMsgDialog", "sceMsgDialogClose");
    msg_term = fn("libSceMsgDialog", "sceMsgDialogTerminate");
    void *used = fn("libSceCommonDialog", "sceCommonDialogIsUsed");
    void *cd_init = fn("libSceCommonDialog", "sceCommonDialogInitialize");
    void *prof_init = fn("libSceNpProfileDialog", "sceNpProfileDialogInitialize");
    void *prof_term = fn("libSceNpProfileDialog", "sceNpProfileDialogTerminate");
    void *inc = fn("libSceMsgDialog", "sceMsgDialogProgressBarInc");
    void *setv = fn("libSceMsgDialog", "sceMsgDialogProgressBarSetValue");
    void *setm = fn("libSceMsgDialog", "sceMsgDialogProgressBarSetMsg");

    g_group = "state";
    expect_eq((u32)((f0)cd_init)(), 0, "CommonDialog Initialize: first call 0");
    expect_eq((u32)((f0)cd_init)(), 0x80B80002, "CommonDialog Initialize: then ALREADY_SYSTEM_INITIALIZED");

    /* state machine */
    expect_eq((u32)((f0)msg_term)(), 0x80B80003, "Terminate in NONE: NOT_INITIALIZED");
    expect_eq((u32)((f1)msg_open)(msg_param(1)), 0x80B80006, "Open before Initialize: INVALID_STATE");
    expect_eq(((u0)msg_update)(), 0, "UpdateStatus in NONE: NONE");
    expect_eq((u32)((f0)msg_init)(), 0, "Initialize");
    expect_eq((u32)((f0)msg_init)(), 0x80B80004, "Initialize twice: ALREADY_INITIALIZED");
    expect_eq(((bool (*)(void))used)(), 1, "IsUsed while a client holds the slot");
    expect_eq((u32)((f0)prof_init)(), 0x80B80008, "another client's Initialize: BUSY");
    u32 r[11];
    expect_eq((u32)((f1)msg_result)(r), 0x80B80005, "GetResult before Open: NOT_FINISHED");
    expect_eq((u32)((f0)msg_close)(), 0x80B8000B, "Close when not running: NOT_RUNNING");

    mark();
    u8 *p = msg_param(1);
    W32(g_user, 0, 1);
    W64(g_user, 8, "Overwrite?\nYour progress is kept");
    expect_eq((u32)((f1)msg_open)(p), 0, "Open USER_MSG YESNO");
    expect_eq((u32)((f1)msg_open)(p), 0x80B80006, "Open while RUNNING: INVALID_STATE");
    expect_eq(((u0)msg_update)(), 2, "first poll: RUNNING");
    expect_eq(((u0)msg_status)(), 3, "second poll: FINISHED");
    expect_eq(((u0)msg_update)(), 3, "stays FINISHED");
    expect_eq((u32)((f1)msg_result)(NULL), 0x80B8000D, "GetResult(NULL): ARG_NULL");
    memset(r, 0xee, sizeof(r));
    expect_eq((u32)((f1)msg_result)(r), 0, "GetResult");
    expect(r[0] == 0 && r[1] == 0 && r[2] == 1 && r[3] == 0xeeeeeeee, "YESNO -> result OK, button YES, reserved untouched");
    expect(logged("dialog: Msg USER_MSG YESNO \"Overwrite?|Your progress is kept\" -> BUTTON1 (auto) @eboot+"),
           "the answer is logged with the message text");

    /* the answer policy, button type by button type (Open again from FINISHED) */
    g_group = "answers";
    expect_eq(msg_answer(0, "ok"), 0x0001, "OK -> button 1");
    expect_eq(msg_answer(3, "ok/cancel"), 0x0001, "OK_CANCEL -> button 1 (OK has focus)");
    expect_eq(msg_answer(7, "Delete?"), 0x0002, "YESNO_FOCUS_NO -> button 2 (NO)");
    expect_eq(msg_answer(8, "Quit?"), 0x0100, "OK_CANCEL_FOCUS_CANCEL -> USER_CANCELED");
    W64(g_btns, 0, "Retry"), W64(g_btns, 8, "Quit");
    W64(g_user, 0x10, g_btns);
    mark();
    expect_eq(msg_answer(9, "Two"), 0x0001, "TWO_BUTTONS -> button 1");
    expect(logged("USER_MSG TWO_BUTTONS [Retry|Quit] \"Two\" -> BUTTON1"), "both labels logged");
    W64(g_user, 0x10, 0);
    expect_eq(msg_answer(4, "?"), ~0u, "button type 4: PARAM_INVALID");
    expect_eq(msg_answer(10, "?"), ~0u, "button type 10: PARAM_INVALID");
    u8 *ps = msg_param(3);
    W32(g_sys, 0, 4);
    expect_eq((u32)((f1)msg_open)(ps), 0, "SYSTEM_MSG");
    ((u0)msg_update)(), ((u0)msg_update)();
    ((f1)msg_result)(r);
    expect(r[1] == 0 && r[2] == 1, "SYSTEM_MSG -> button 1");
    u8 *pn = msg_param(2);
    W64(pn, 0x48, 0);
    expect_eq((u32)((f1)msg_open)(pn), 0x80B8000A, "PROGRESS_BAR without its param: PARAM_INVALID");
    expect_eq((u32)((f1)msg_open)(msg_param(4)), 0x80B8000A, "mode 4: PARAM_INVALID");

    /* button-less dialogs hold until Close, or 10 s */
    mark();
    W32(g_user, 0, 5);
    W64(g_user, 8, "Saving...");
    expect_eq((u32)((f1)msg_open)(msg_param(1)), 0, "Open WAIT");
    bool held = true;
    for (int i = 0; i < 99; i++, g_now += 100000)
        held &= ((u0)msg_update)() == 2;
    expect(held, "WAIT stays RUNNING for 9.9 s");
    g_now += 200000;
    expect_eq(((u0)msg_update)(), 3, "WAIT finishes after 10 s");
    ((f1)msg_result)(r);
    expect(r[1] == 0 && r[2] == 0, "hold timeout -> the Close result (OK, no button)");
    expect(logged("dialog: Msg hold timeout"), "the hold timeout is logged");
    W32(g_user, 0, 2);
    expect_eq((u32)((f1)msg_open)(msg_param(1)), 0, "Open NONE");
    ((u0)msg_update)(), ((u0)msg_update)();
    mark();
    expect_eq((u32)((f0)msg_close)(), 0, "Close a held dialog");
    expect_eq(((u0)msg_update)(), 3, "FINISHED after Close");
    expect(logged("dialog: Msg closed by the title"), "Close logged");

    /* progress bar */
    expect_eq((u32)((fu2)inc)(0, 10), 0x80B8000B, "ProgressBarInc when not running: NOT_RUNNING");
    u8 *pp = msg_param(2);
    W32(g_prog, 0, 0);
    W64(g_prog, 8, "Installing");
    expect_eq((u32)((f1)msg_open)(pp), 0, "Open PROGRESS_BAR");
    expect_eq((u32)((fu2)inc)(1, 10), 0x80B8000A, "ProgressBarInc target 1: PARAM_INVALID");
    g_now += 8000000;
    expect_eq((u32)((fu2)inc)(0, 60), 0, "ProgressBarInc");
    g_now += 8000000;
    expect_eq(((u0)msg_update)(), 2, "progress updates keep the bar up past 10 s");
    mark();
    expect_eq((u32)((fu2)setv)(0, 100), 0, "ProgressBarSetValue");
    expect_eq((u32)((fus)setm)(0, "Done"), 0, "ProgressBarSetMsg");
    expect(logged("dialog: Msg progress \"Done\""), "progress message logged");
    expect_eq((u32)((f0)msg_close)(), 0, "Close the progress bar");
    W32(g_user, 0, 0);
    expect_eq((u32)((f1)msg_open)(msg_param(1)), 0, "Open OK");
    expect_eq((u32)((fu2)inc)(0, 1), 0x80B8000F, "ProgressBarInc on a USER_MSG: NOT_SUPPORTED");
    ((u0)msg_update)(), ((u0)msg_update)();

    /* param checks: base size and the library's size refuse, the magic is only logged */
    g_group = "params";
    u8 *bad = msg_param(1);
    *(u64 *)bad = 0x28;
    expect_eq((u32)((f1)msg_open)(bad), 0x80B8000A, "base param size 0x28: PARAM_INVALID");
    bad = msg_param(1);
    W64(bad, 0x30, 0x80);
    expect_eq((u32)((f1)msg_open)(bad), 0x80B8000A, "MsgParam size 0x80: PARAM_INVALID");
    expect_eq((u32)((f1)msg_open)(NULL), 0x80B8000D, "Open(NULL): ARG_NULL");
    bad = msg_param(1);
    W32(bad, 0x2c, 0x1234);
    mark();
    expect_eq((u32)((f1)msg_open)(bad), 0, "a wrong magic is accepted");
    expect(logged("base param magic 1234"), "and logged");
    ((u0)msg_update)(), ((u0)msg_update)();

    /* the ctx flag: CANCEL for every dialog with buttons */
    g_group = "answers";
    g_ctx->flags |= CTX_FLAG_DIALOG_CANCEL;
    mark();
    expect_eq(msg_answer(1, "Overwrite?"), 0x0100, "--dialog-cancel: YESNO -> USER_CANCELED");
    expect(logged("-> CANCEL (--dialog-cancel)"), "--dialog-cancel: logged as such");
    W32(g_user, 0, 2);
    ((f1)msg_open)(msg_param(1));
    ((u0)msg_update)();
    expect_eq(((u0)msg_update)(), 2, "--dialog-cancel: a button-less dialog still holds");
    ((f0)msg_close)(), ((u0)msg_update)();
    g_ctx->flags &= ~(u64)CTX_FLAG_DIALOG_CANCEL;

    /* a per-call-site override */
    void *over = fn("libGhostldrDialog", "ghostldrDialogOverride");
    mark();
    W32(g_user, 0, 1);
    expect_eq((u32)call1_site_a(msg_open, msg_param(1)), 0, "Open from call site A");
    u64 site = last_caller();
    ((u0)msg_update)(), ((u0)msg_update)();
    expect(site != 0, "the call site is logged as eboot+offset");
    expect_eq((u32)((fov)over)(DLG_MSG, site, ANS_BTN2, NULL), 0, "override call site A -> button 2");
    mark();
    expect_eq((u32)call1_site_a(msg_open, msg_param(1)), 0, "Open from call site A again");
    ((u0)msg_update)(), ((u0)msg_update)();
    ((f1)msg_result)(r);
    expect(r[1] == 0 && r[2] == 2 && logged("-> BUTTON2 (override)"), "the override answers there");
    expect_eq(msg_answer(1, "elsewhere"), 0x0001, "other call sites keep the default");

    g_group = "state";
    expect_eq((u32)((f0)msg_term)(), 0, "Terminate");
    expect_eq(((bool (*)(void))used)(), 0, "the slot is free");
    expect_eq((u32)((f0)prof_init)(), 0, "another client can Initialize now");
    expect_eq((u32)((f0)prof_term)(), 0, "and Terminate");
}

/* ---- ErrorDialog (0x80ED errors, no BaseParam) and the online dialogs ---- */
static void test_error_and_online(void)
{
    void *init = fn("libSceErrorDialog", "sceErrorDialogInitialize");
    void *open = fn("libSceErrorDialog", "sceErrorDialogOpen");
    void *upd = fn("libSceErrorDialog", "sceErrorDialogUpdateStatus");
    void *close = fn("libSceErrorDialog", "sceErrorDialogClose");
    void *term = fn("libSceErrorDialog", "sceErrorDialogTerminate");
    s32 ep[4] = {0x10, (s32)0x8041013d, 0x1000, 0};
    g_group = "params";
    expect_eq((u32)((f1)open)(ep), 0x80ED0005, "ErrorDialog Open before Initialize: INVALID_STATE");
    expect_eq((u32)((f0)init)(), 0, "ErrorDialog Initialize");
    expect_eq((u32)((f0)init)(), 0x80ED0002, "ErrorDialog Initialize twice: ALREADY_INITIALIZED");
    expect_eq((u32)((f1)open)(NULL), 0x80ED0003, "ErrorDialog Open(NULL): PARAM_INVALID");
    ep[0] = 0x14;
    expect_eq((u32)((f1)open)(ep), 0x80ED0003, "ErrorDialog size 0x14: PARAM_INVALID");
    ep[0] = 0x10;
    mark();
    expect_eq((u32)((f1)open)(ep), 0, "ErrorDialog Open");
    expect(logged("dialog: Error Open error 8041013d user 1000 -> CANCEL (auto) @eboot+"), "error code logged");
    expect_eq(((u0)upd)(), 2, "ErrorDialog RUNNING once");
    expect_eq(((u0)upd)(), 3, "then FINISHED");
    expect_eq((u32)((f0)close)(), 0x80ED0005, "ErrorDialog Close when finished: INVALID_STATE");
    expect_eq((u32)((f0)term)(), 0, "ErrorDialog Terminate");
    expect_eq((u32)((f0)term)(), 0x80ED0001, "ErrorDialog Terminate twice: NOT_INITIALIZED");

    /* Signin: its own 0x8135 errors, lenient layout, CANCEL */
    g_group = "online";
    void *si = fn("libSceSigninDialog", "sceSigninDialogInitialize");
    void *so = fn("libSceSigninDialog", "sceSigninDialogOpen");
    void *su = fn("libSceSigninDialog", "sceSigninDialogUpdateStatus");
    void *sr = fn("libSceSigninDialog", "sceSigninDialogGetResult");
    void *st = fn("libSceSigninDialog", "sceSigninDialogTerminate");
    u64 mem[0x98 / 8];
    s32 res[4] = {-1, 7, 7, 7};
    expect_eq((u32)((f1)sr)(res), 0x81350001, "Signin GetResult in NONE: NOT_INITIALIZED");
    expect_eq((u32)((f0)si)(), 0, "Signin Initialize");
    expect_eq((u32)((f0)si)(), 0x81350002, "Signin Initialize twice");
    expect_eq((u32)((f1)so)(NULL), 0x81350003, "Signin Open(NULL): PARAM_INVALID");
    expect_eq((u32)((f1)so)(param(mem, 0x60, 0x30, 8, 0x48)), 0, "Signin Open (any size)");
    expect_eq((u32)((f1)sr)(res), 0x81350005, "Signin GetResult while running: INVALID_STATE");
    ((u0)su)(), ((u0)su)();
    expect_eq((u32)((f1)sr)(res), 0, "Signin GetResult");
    expect(res[0] == 1 && res[1] == 7, "Signin -> USER_CANCELED");
    ((f0)st)();

    /* NpProfile: ParamA 0x78 */
    void *pi = fn("libSceNpProfileDialog", "sceNpProfileDialogInitialize");
    void *po = fn("libSceNpProfileDialog", "sceNpProfileDialogOpenA");
    void *pu = fn("libSceNpProfileDialog", "sceNpProfileDialogUpdateStatus");
    void *pr = fn("libSceNpProfileDialog", "sceNpProfileDialogGetResult");
    void *pt = fn("libSceNpProfileDialog", "sceNpProfileDialogTerminate");
    ((f0)pi)();
    u8 *p = param(mem, 0x78, 0x30, 8, 0x80);
    expect_eq((u32)((f1)po)(p), 0x80B8000A, "NpProfile OpenA size 0x80: PARAM_INVALID");
    p = param(mem, 0x78, 0x30, 8, 0x78);
    W32(p, 0x38, 1), W64(p, 0x40, 0x1122334455667788ULL), W64(p, 0x50, 0xabcdef);
    expect_eq((u32)((f1)po)(p), 0, "NpProfile OpenA");
    ((u0)pu)(), ((u0)pu)();
    u64 pres[6] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
    expect_eq((u32)((f1)pr)(pres), 0, "NpProfile GetResult");
    expect(R32(pres, 0) == 0 && R32(pres, 4) == 1 && pres[1] == 0xabcdef, "NpProfile -> userAction USER_CANCELED, userData");
    ((f0)pt)();

    /* Invitation (A) */
    void *ii = fn("libSceInvitationDialog", "sceInvitationDialogInitialize");
    void *io = fn("libSceInvitationDialog", "sceInvitationDialogOpenA");
    void *iu = fn("libSceInvitationDialog", "sceInvitationDialogUpdateStatus");
    void *ir = fn("libSceInvitationDialog", "sceInvitationDialogGetResultA");
    void *it = fn("libSceInvitationDialog", "sceInvitationDialogTerminate");
    ((f0)ii)();
    p = param(mem, 0x90, 0x30, 4, 0x90);
    W32(p, 0x34, 3);
    expect_eq((u32)((f1)io)(p), 0x80B8000A, "Invitation mode 3: PARAM_INVALID");
    W32(p, 0x34, 1);
    expect_eq((u32)((f1)io)(p), 0x80B8000A, "Invitation SEND without data: PARAM_INVALID");
    W32(p, 0x34, 2), W64(p, 0x40, 0x5151);
    expect_eq((u32)((f1)io)(p), 0, "Invitation OpenA RECV");
    ((u0)iu)(), ((u0)iu)();
    u32 users[4] = {9, 9, 9, 9};
    u64 ires[7] = {0, ~0ULL, (u64)users, 0, 0, 0, 0};
    expect_eq((u32)((f1)ir)(ires), 0, "Invitation GetResultA");
    expect(ires[0] == 0x5151 && R32(ires, 8) == 0 && R32(ires, 0xc) == 1 && users[0] == 0,
           "Invitation -> callbackArg, no error, USER_CANCELED, no users sent");
    ((f0)it)();

    /* NpCommerce: never a purchase */
    void *ci = fn("libSceNpCommerce", "sceNpCommerceDialogInitialize");
    void *co = fn("libSceNpCommerce", "sceNpCommerceDialogOpen");
    void *cu = fn("libSceNpCommerce", "sceNpCommerceDialogUpdateStatus");
    void *cr = fn("libSceNpCommerce", "sceNpCommerceDialogGetResult");
    void *ct = fn("libSceNpCommerce", "sceNpCommerceDialogTerminate");
    void *icon = fn("libSceNpCommerce", "sceNpCommerceShowPsStoreIcon");
    ((f0)ci)();
    p = param(mem, 0x80, 0x30, 4, 0x80);
    const char *targets[] = {"EP9000-CUSA00000_00-DLC0000000000001"};
    W32(p, 0x38, 1), W64(p, 0x40, targets), W32(p, 0x48, 1), W64(p, 0x58, 0x77);
    mark();
    expect_eq((u32)((f1)co)(p), 0, "NpCommerce Open PRODUCT");
    expect(logged("dialog: NpCommerce PRODUCT label 0, 1 targets (EP9000-CUSA00000_00-DLC0000000000001) -> CANCEL"),
           "the store request is logged");
    ((u0)cu)(), ((u0)cu)();
    u64 cres[6];
    memset(cres, 0xff, sizeof(cres));
    expect_eq((u32)((f1)cr)(cres), 0, "NpCommerce GetResult");
    expect(R32(cres, 0) == 1 && *((u8 *)cres + 4) == 0 && cres[1] == 0x77, "NpCommerce -> canceled, not authorized");
    ((f0)ct)();
    expect_eq((u32)((f1)icon)(0), 0, "ShowPsStoreIcon is a no-op");

    /* unknown result layouts: RESULT_NONE, never a zeroed "OK" */
    void *gi = fn("libSceGameCustomDataDialog", "sceGameCustomDataDialogInitialize");
    void *go = fn("libSceGameCustomDataDialog", "sceGameCustomDataDialogOpenA");
    void *gu = fn("libSceGameCustomDataDialog", "sceGameCustomDataDialogUpdateStatus");
    void *gr = fn("libSceGameCustomDataDialog", "sceGameCustomDataDialogGetResultA");
    void *gt = fn("libSceGameCustomDataDialog", "sceGameCustomDataDialogTerminate");
    ((f0)gi)();
    expect_eq((u32)((f1)go)(param(mem, 0x60, 0x30, 8, 0x58)), 0, "GameCustomData OpenA (any size)");
    ((u0)gu)(), ((u0)gu)();
    u64 gres[8] = {0};
    expect_eq((u32)((f1)gr)(gres), 0x80B80007, "GameCustomData GetResultA: RESULT_NONE");
    expect(gres[0] == 0, "and writes nothing");
    ((f0)gt)();
    void *wi = fn("libSceWebBrowserDialog", "sceWebBrowserDialogInitialize");
    void *wo = fn("libSceWebBrowserDialog", "sceWebBrowserDialogOpen");
    void *wu = fn("libSceWebBrowserDialog", "sceWebBrowserDialogUpdateStatus");
    void *wr = fn("libSceWebBrowserDialog", "sceWebBrowserDialogGetResult");
    void *wt = fn("libSceWebBrowserDialog", "sceWebBrowserDialogTerminate");
    ((f0)wi)();
    p = param(mem, 0x60, 0x30, 8, 0x58);
    W64(p, 0x40, "https://example.com/terms");
    mark();
    expect_eq((u32)((f1)wo)(p), 0, "WebBrowser Open");
    expect(logged("dialog: WebBrowser Open \"https://example.com/terms\" -> CANCEL"), "the URL is logged");
    ((u0)wu)(), ((u0)wu)();
    expect_eq((u32)((f1)wr)(gres), 0x80B80007, "WebBrowser GetResult: RESULT_NONE");
    ((f0)wt)();
    expect_eq((u32)((f0)fn("libSceNpEulaDialog", "sceNpEulaDialogCheckVersion"))(), 0x80B80007,
              "NpEula CheckVersion: RESULT_NONE");
    void *fi = fn("libScePlayerInvitationDialog", "scePlayerInvitationDialogInitialize");
    void *fo = fn("libScePlayerInvitationDialog", "scePlayerInvitationDialogOpen");
    void *ft = fn("libScePlayerInvitationDialog", "scePlayerInvitationDialogTerminate");
    expect_eq((u32)((f0)fi)(), 0, "PlayerInvitation Initialize");
    expect_eq((u32)((f1)fo)(param(mem, 0x60, 0x30, 8, 0x40)), 0, "PlayerInvitation Open");
    expect_eq((u32)((f0)ft)(), 0, "PlayerInvitation Terminate while running");
    expect(fn("libSceNpFriendListDialog", "sceNpFriendListDialogGetResultA") != NULL, "FriendList is covered");
    expect(fn("libSceNpProfileDialogCompat", "sceNpProfileDialogInitialize") == pi, "aliases: NpProfileDialogCompat");
}

/* ---- SaveDataDialog: the stall guard, the answers, the list ---- */
static void *sd_init, *sd_open, *sd_update, *sd_result, *sd_close, *sd_term;
static u64 g_sdp[0x98 / 8], g_items[0x60 / 8], g_sdsub[0x30 / 8], g_newitem[0x38 / 8];
static char g_dirs[8][32];
static char g_res_dir[32];
static u8 g_res_param[0x530];
static u64 g_sdres[0x48 / 8];

static u8 *sd_param(u32 mode, u32 disp)
{
    u8 *p = param(g_sdp, 0x98, 0x30, 4, 0x98);
    W32(p, 0x34, mode), W32(p, 0x38, disp), W64(p, 0x70, 0xd00d);
    W64(p, 0x48, g_items);
    if (mode >= 2)
        W64(p, 0x50 + 8 * (mode - 2), g_sdsub);
    return p;
}

static void sd_items(u32 n, u32 focus, const char *focus_name, bool new_item)
{
    memset(g_items, 0, sizeof(g_items));
    W32(g_items, 0, 0x1000);
    W64(g_items, 0x10, g_dirs), W32(g_items, 0x18, n);
    W64(g_items, 0x20, new_item ? (u64)g_newitem : 0);
    W32(g_items, 0x28, focus), W64(g_items, 0x30, focus_name);
}

/* open, poll to the end like Ghost's helper (250 ms sleeps), GetResult */
static s32 sd_run(u8 *p)
{
    s32 rc = ((f1)sd_open)(p);
    if (rc)
        return rc;
    u32 st = 0;
    for (int i = 0; i < 400 && (st = ((u0)sd_update)()) != 3; i++)
        g_now += 250000;
    if (st != 3)
        return -1;
    memset(g_sdres, 0, sizeof(g_sdres));
    memset(g_res_dir, 'x', sizeof(g_res_dir));
    memset(g_res_param, 0xcc, sizeof(g_res_param));
    W64(g_sdres, 0x10, g_res_dir), W64(g_sdres, 0x18, g_res_param);
    return ((f1)sd_result)(g_sdres);
}
#define SD_RESULT R32(g_sdres, 4)
#define SD_BUTTON R32(g_sdres, 8)

static void test_savedata(void)
{
    sd_init = fn("libSceSaveDataDialog", "sceSaveDataDialogInitialize");
    sd_open = fn("libSceSaveDataDialog", "sceSaveDataDialogOpen");
    sd_update = fn("libSceSaveDataDialog", "sceSaveDataDialogUpdateStatus");
    sd_result = fn("libSceSaveDataDialog", "sceSaveDataDialogGetResult");
    sd_close = fn("libSceSaveDataDialog", "sceSaveDataDialogClose");
    sd_term = fn("libSceSaveDataDialog", "sceSaveDataDialogTerminate");
    void *fake = fn("libGhostldrDialog", "ghostldrDialogTestSaves");

    g_group = "stall";
    /* Ghost's P0 hang: Initialize fails (another dialog holds the slot), Open
     * fails, and the helper polls for FINISHED forever */
    ((f0)msg_init)();
    expect_eq((u32)((f0)sd_init)(), 0x80B80008, "SaveData Initialize while MsgDialog holds the slot: BUSY");
    sd_items(0, 0, NULL, false);
    expect_eq((u32)((f1)sd_open)(sd_param(3, 1)), 0x80B80006, "so Open fails: INVALID_STATE");
    mark();
    u32 polls = 0, st = 0;
    for (; polls < 100 && (st = ((u0)sd_update)()) != 3; polls++)
        g_now += 250000;
    expect(st == 3 && polls >= 39 && polls <= 41, "the stall guard finishes the helper's wait after 10 s");
    expect(logged("dialog: SaveData stall guard"), "and logs it");
    u64 r[9];
    expect_eq((u32)((f1)sd_result)(r), 0x80B80003, "GetResult after the stall guard (never initialized)");
    expect_eq((u32)((f0)sd_term)(), 0x80B80003, "Terminate (never initialized)");
    expect_eq(((u0)sd_update)(), 0, "polls see NONE again");
    ((f0)msg_term)();

    expect_eq((u32)((f0)sd_init)(), 0, "SaveData Initialize");
    u8 *bad = sd_param(3, 1);
    W32(bad, 0x30, 0x90);
    expect_eq((u32)((f1)sd_open)(bad), 0x80B8000A, "SaveData param size 0x90: PARAM_INVALID");
    for (polls = 0; polls < 100 && (st = ((u0)sd_update)()) != 3; polls++)
        g_now += 250000;
    memset(r, 0, sizeof(r));
    expect(st == 3 && ((f1)sd_result)(r) == 0 && R32(r, 4) == 1, "a failed Open: after 10 s FINISHED with CANCEL");

    /* answers */
    g_group = "savedata";
    W32(g_sdsub, 0, 2);
    expect_eq((u32)sd_run(sd_param(3, 3)), 0, "SYSTEM_MSG CONFIRM DELETE");
    expect(SD_RESULT == 0 && SD_BUTTON == 2 && R32(g_sdres, 0) == 3, "CONFIRM DELETE -> NO (never delete)");
    expect(g_sdres[4] == 0xd00d, "userData returned");
    expect_eq((u32)sd_run(sd_param(3, 1)), 0, "SYSTEM_MSG CONFIRM SAVE");
    expect(SD_RESULT == 0 && SD_BUTTON == 1, "CONFIRM SAVE -> YES");
    W32(g_sdsub, 0, 4), W64(g_sdsub, 8, 123);
    mark();
    expect_eq((u32)sd_run(sd_param(3, 1)), 0, "SYSTEM_MSG NOSPACE");
    expect(SD_BUTTON == 1 && logged("WARNING: the title reports no space (123 blocks)"), "NOSPACE -> OK, logged loudly");
    W32(g_sdsub, 0, 1), W64(g_sdsub, 8, "Overwrite your save?");
    expect_eq((u32)sd_run(sd_param(2, 1)), 0, "USER_MSG YESNO");
    expect(SD_BUTTON == 1, "USER_MSG YESNO -> YES");
    W32(g_sdsub, 0, 0x80551622);
    expect_eq((u32)sd_run(sd_param(4, 1)), 0, "ERROR_CODE");
    expect(SD_BUTTON == 1, "ERROR_CODE -> OK");
    W32(g_sdsub, 0, 5);
    expect_eq((u32)((f1)sd_open)(sd_param(3, 1)), 0, "SYSTEM_MSG PROGRESS");
    ((u0)sd_update)();
    expect_eq(((u0)sd_update)(), 2, "PROGRESS holds");
    ((f0)sd_close)(), ((u0)sd_update)();
    expect_eq((u32)((f1)sd_open)(sd_param(6, 1)), 0x80B8000A, "mode 6: PARAM_INVALID");

    /* before SDK 4.50, "No" is USER_CANCELED */
    u8 *pp = (u8 *)g_main_mod->proc_param;
    W32(pp, 0x10, 0x04000000);
    W32(g_sdsub, 0, 2);
    sd_run(sd_param(3, 3));
    expect(SD_RESULT == 1 && SD_BUTTON == 0, "SDK 4.00 title: CONFIRM DELETE -> USER_CANCELED");
    W32(pp, 0x10, 0x08008011);

    /* LIST, from the title's virtual store only */
    static const char names[][32] = {"SAVE0001", "SAVE0002", "SAVE0003", "GHOST001", "GHOSTSAVE",
                                     "GFSINDEX", "SLUS-20268", "sce_sys"};
    static const s64 mtimes[] = {100, 300, 200, 999, 999, 999, 999, 999};
    ((ffake)fake)(names, mtimes, 8);
    static const char *const ask[] = {"SAVE0009", "SAVE0001", "SAVE0002", "SAVE0003", "GHOST001", "GHOSTSAVE",
                                      "GFSINDEX", "SLUS-20268"};
    memset(g_dirs, 0, sizeof(g_dirs));
    for (int i = 0; i < 8; i++)
        strlcpy(g_dirs[i], ask[i], 32);
    sd_items(8, 4, NULL, false);
    mark();
    expect_eq((u32)sd_run(sd_param(1, 2)), 0, "LIST LOAD, focus DATALATEST");
    expect(SD_RESULT == 0 && SD_BUTTON == 0 && R32(g_sdres, 0) == 1 && strcmp(g_res_dir, "SAVE0002") == 0,
           "-> the newest save, SAVE0002");
    expect(strcmp((char *)g_res_param, "fake SAVE0002") == 0 && R64(g_res_param, 0x508) == 300,
           "and its param (title, mtime)");
    expect(logged("LIST LOAD: 3 of 8 exist, focus 4 -> \"SAVE0002\""), "only the title's saves are listed");
    sd_items(8, 5, NULL, false);
    sd_run(sd_param(1, 2));
    expect(strcmp(g_res_dir, "SAVE0001") == 0, "focus DATAOLDEST -> SAVE0001");
    sd_items(8, 1, NULL, false);
    sd_run(sd_param(1, 2));
    expect(strcmp(g_res_dir, "SAVE0003") == 0, "focus LISTTAIL -> SAVE0003 (SWRR's names never listed)");
    sd_items(8, 6, "SAVE0003", false);
    sd_run(sd_param(1, 2));
    expect(strcmp(g_res_dir, "SAVE0003") == 0, "focus DIRNAME SAVE0003");
    sd_items(8, 6, "SAVE0009", false);
    sd_run(sd_param(1, 2));
    expect(strcmp(g_res_dir, "SAVE0001") == 0, "focus DIRNAME not listed -> the first");
    sd_items(8, 0, NULL, true);
    sd_run(sd_param(1, 1));
    expect(SD_RESULT == 0 && g_res_dir[0] == 0 && g_res_param[0] == 0xcc, "LIST SAVE, LISTHEAD -> the new item");
    sd_items(8, 4, NULL, true);
    sd_run(sd_param(1, 1));
    expect(strcmp(g_res_dir, "SAVE0002") == 0, "LIST SAVE, DATALATEST -> SAVE0002");
    sd_items(8, 4, NULL, false);
    sd_run(sd_param(1, 3));
    expect(SD_RESULT == 1 && g_res_dir[0] == 0, "LIST DELETE -> USER_CANCELED, nothing chosen");
    W64(g_items, 8, "CUSA00001");
    sd_run(sd_param(1, 2));
    expect(SD_RESULT == 1, "LIST of another title -> USER_CANCELED");
    sd_items(8, 4, NULL, false);
    for (int i = 1; i < 4; i++)
        strlcpy(g_dirs[i], "SAVE0009", 32);
    sd_run(sd_param(1, 2));
    expect(SD_RESULT == 1 && g_res_dir[0] == 0, "LIST LOAD of SWRR's names only -> no data, USER_CANCELED");
    for (int i = 0; i < 4; i++)
        strlcpy(g_dirs[i], ask[i], 32);
    sd_items(8, 4, NULL, false);
    expect_eq((u32)((f1)sd_open)(sd_param(1, 2)), 0, "LIST LOAD");
    ((f0)sd_close)();
    ((f1)sd_result)(g_sdres);
    expect(SD_RESULT == 0 && g_res_dir[0] == 0, "a closed LIST: OK, nothing chosen");
    ((ffake)fake)(NULL, NULL, 0);
    sd_run(sd_param(1, 2));
    expect(SD_RESULT == 1, "without the save data family: an empty store, USER_CANCELED");
    expect_eq((u32)((f0)sd_term)(), 0, "SaveData Terminate");
}

/* ---- ImeDialog: the text policy ---- */
static void *ime_init, *ime_status, *ime_result, *ime_term;
static u64 g_imep[0x60 / 8];
static u16 g_buf[2050];

static u8 *ime_param(u32 type, u32 option, u32 max)
{
    u8 *p = (u8 *)g_imep;
    memset(p, 0, 0x60);
    W32(p, 0, 0x1000), W32(p, 4, type), W32(p, 0x20, option), W32(p, 0x24, max);
    W64(p, 0x28, g_buf);
    W64(p, 0x48, u"Name your horse");
    return p;
}

static bool eq16(const u16 *a, const char *ascii)
{
    for (; *ascii; a++, ascii++)
        if (*a != (u8)*ascii)
            return false;
    return *a == 0;
}

/* Init, poll to Finished, GetResult, Term: the end status, or ~0 */
static u32 ime_run(u8 *p)
{
    if (((f2)ime_init)(p, NULL) != 0)
        return ~0u;
    u32 a = ((u0)ime_status)(), b = ((u0)ime_status)();
    u32 r[4] = {9, 9, 9, 9};
    s32 rc = ((f1)ime_result)(r);
    ((f0)ime_term)();
    return a == 1 && b == 2 && rc == 0 ? r[0] : 0xbad;
}

static s32 upper_filter(u16 *out_, u32 *n, const u16 *src, u32 len)
{
    for (u32 i = 0; i < len; i++)
        out_[i] = src[i] >= 'a' && src[i] <= 'z' ? src[i] - 32 : src[i];
    *n = len;
    return 0;
}

static void test_ime_dialog(void)
{
    ime_init = fn("libSceImeDialog", "sceImeDialogInit");
    ime_status = fn("libSceImeDialog", "sceImeDialogGetStatus");
    ime_result = fn("libSceImeDialog", "sceImeDialogGetResult");
    ime_term = fn("libSceImeDialog", "sceImeDialogTerm");
    void *abort = fn("libSceImeDialog", "sceImeDialogAbort");
    void *force = fn("libSceImeDialog", "sceImeDialogForceClose");
    void *size = fn("libSceImeDialog", "sceImeDialogGetPanelSize");
    void *over = fn("libGhostldrDialog", "ghostldrDialogOverride");
    u32 r[4];

    g_group = "ime";
    expect_eq(((u0)ime_status)(), 0, "ImeDialog GetStatus with no session: NONE");
    expect_eq((u32)((f1)ime_result)(r), 0x80BC0107, "GetResult with no session: NOT_IN_USE");
    expect_eq((u32)((f0)ime_term)(), 0x80BC0107, "Term with no session: NOT_IN_USE");
    expect_eq((u32)((f2)ime_init)(NULL, NULL), 0x80BC0031, "Init(NULL): INVALID_ADDRESS");
    u8 *p = ime_param(0, 0, 16);
    W64(p, 0x28, 0);
    expect_eq((u32)((f2)ime_init)(p, NULL), 0x80BC0017, "no buffer: INVALID_INPUT_TEXT_BUFFER");
    expect_eq((u32)((f2)ime_init)(ime_param(0, 0, 0), NULL), 0x80BC0016, "max 0: INVALID_MAX_TEXT_LENGTH");
    expect_eq((u32)((f2)ime_init)(ime_param(0, 0, 2049), NULL), 0x80BC0016, "max 2049: INVALID_MAX_TEXT_LENGTH");
    expect_eq((u32)((f2)ime_init)(ime_param(5, 0, 16), NULL), 0x80BC0011, "type 5: INVALID_TYPE");
    p = ime_param(0, 0, 16);
    W32(p, 0x38, 3);
    expect_eq((u32)((f2)ime_init)(p, NULL), 0x80BC001A, "h_align 3: INVALID_HORIZONTAL_ALIGNMENT");
    p = ime_param(0, 0, 16);
    W32(p, 0x3c, 3);
    expect_eq((u32)((f2)ime_init)(p, NULL), 0x80BC001B, "v_align 3: INVALID_VERTICAL_ALIGNMENT");

    /* rule 5: the user's name, UTF-8 to UTF-16, BMP only */
    memset(g_buf, 0x55, sizeof(g_buf));
    g_buf[0] = 0;
    g_user_name = "J\xc3\xafn \xe2\x82\xac\xf0\x9f\x98\x80!";
    mark();
    expect_eq((u32)((f2)ime_init)(ime_param(0, 0, 16), NULL), 0, "Init: empty default field");
    expect_eq((u32)((f2)ime_init)(ime_param(0, 0, 16), NULL), 0x80BC0001, "Init again: BUSY");
    expect_eq((u32)((f1)ime_result)(r), 0x80BC0106, "GetResult while running: NOT_FINISHED");
    expect_eq((u32)((f0)ime_term)(), 0x80BC0106, "Term while running: NOT_FINISHED");
    expect_eq(((u0)ime_status)(), 1, "first poll: Running (1)");
    expect_eq(((u0)ime_status)(), 2, "then Finished (2, not 3)");
    expect_eq((u32)((f1)ime_result)(r), 0, "GetResult");
    expect(r[0] == 0 && g_buf[0] == 'J' && g_buf[1] == 0xef && g_buf[2] == 'n' && g_buf[3] == ' ' &&
               g_buf[4] == 0x20ac && g_buf[5] == '?' && g_buf[6] == '!' && g_buf[7] == 0,
           "-> Ok, the user name in UTF-16 (non-BMP as '?')");
    expect(g_user_asked == 0x1000, "for the param's user");
    expect(logged("dialog: ImeDialog type 0 option 0 max 16 title \"Name your horse\" text \"\" -> OK (user name)"),
           "logged with the title");
    expect_eq((u32)((f0)ime_term)(), 0, "Term");

    g_user_name = "Abcdefg";
    g_buf[0] = 0;
    expect_eq(ime_run(ime_param(0, 0, 4)), 0, "max 4");
    expect(eq16(g_buf, "Abcd"), "the name truncated to max_text_length");
    g_user_rc = (s32)0x80960002;
    g_buf[0] = 0;
    ime_run(ime_param(1, 0, 16));
    expect(eq16(g_buf, "Player"), "GetUserName fails -> \"Player\"");
    g_user_rc = 0;
    g_ctx->flags |= CTX_FLAG_IME_FIXED_NAME;
    g_buf[0] = 0;
    ime_run(ime_param(0, 0, 16));
    expect(eq16(g_buf, "Player"), "--ime-fixed-name -> \"Player\"");
    g_ctx->flags &= ~(u64)CTX_FLAG_IME_FIXED_NAME;

    /* rules 2-4 */
    memcpy(g_buf, u"Jin", 8);
    expect_eq(ime_run(ime_param(0, 0, 16)), 0, "a title's suggestion");
    expect(eq16(g_buf, "Jin"), "is kept, Ok");
    expect_eq(ime_run(ime_param(0, 4, 16)), 1, "PASSWORD -> UserCanceled");
    expect_eq(ime_run(ime_param(2, 0, 16)), 1, "URL -> UserCanceled");
    expect_eq(ime_run(ime_param(3, 0, 16)), 1, "MAIL -> UserCanceled");
    expect(eq16(g_buf, "Jin"), "the buffer untouched when cancelled");
    g_buf[0] = 0;
    expect_eq(ime_run(ime_param(4, 0, 16)), 1, "an empty NUMBER field -> UserCanceled");
    g_ctx->flags |= CTX_FLAG_DIALOG_CANCEL;
    expect_eq(ime_run(ime_param(0, 0, 16)), 1, "--dialog-cancel -> UserCanceled");
    g_ctx->flags &= ~(u64)CTX_FLAG_DIALOG_CANCEL;

    /* rule 6: the title's filter */
    g_user_name = "ghost";
    g_buf[0] = 0;
    p = ime_param(0, 0, 16);
    W64(p, 0x18, upper_filter);
    ime_run(p);
    expect(eq16(g_buf, "GHOST"), "the title's text filter applies");

    /* rule 1: an override's text at a call site */
    g_buf[0] = 0;
    mark();
    expect_eq((u32)call2_site_b(ime_init, ime_param(0, 0, 16), NULL), 0, "Init from call site B");
    u64 site = last_caller();
    ((u0)ime_status)(), ((u0)ime_status)(), ((f0)ime_term)();
    ((fov)over)(DLG_IMEDIALOG, site, ANS_BTN1, "Kage");
    g_buf[0] = 0;
    call2_site_b(ime_init, ime_param(0, 0, 16), NULL);
    ((u0)ime_status)(), ((u0)ime_status)(), ((f0)ime_term)();
    expect(eq16(g_buf, "Kage"), "an override's text at its call site");

    /* Abort, ForceClose, panel sizes */
    memcpy(g_buf, u"keep", 10);
    ((f2)ime_init)(ime_param(0, 0, 16), NULL);
    expect_eq((u32)((f0)abort)(), 0, "Abort");
    ((f1)ime_result)(r);
    expect(r[0] == 2 && eq16(g_buf, "keep"), "Aborted (2), the buffer untouched");
    expect_eq((u32)((f0)abort)(), 0x80BC0105, "Abort when finished: NOT_RUNNING");
    expect_eq((u32)((f0)force)(), 0, "ForceClose");
    expect_eq(((u0)ime_status)(), 0, "no session after ForceClose");
    u32 w = 0, h = 0;
    ((f3)size)(ime_param(4, 0, 8), &w, &h);
    expect(w == 370 && h == 470, "panel: number 370x470");
    ((f3)size)(ime_param(0, 1, 8), &w, &h);
    expect(w == 793 && h == 628, "panel: multiline 793x628");
    ((f3)size)(ime_param(0, 0x4000, 8), &w, &h);
    expect(w == 1586 && h == 1056, "panel: over 2K coordinates doubles it");
    expect_eq((u32)((f3)size)(NULL, &w, &h), 0x80BC0031, "panel size(NULL): INVALID_ADDRESS");
}

/* ---- Ime: the same policy through events ---- */
static u32 g_ev[8], g_nev;
static u16 *g_ev_str;
static u32 g_ev_caret, g_ev_len, g_ev_w;
static void ime_handler(void *arg, const void *ev)
{
    u32 id = R32(ev, 0);
    if (g_nev < 8)
        g_ev[g_nev++] = id;
    if (id == 0)
        g_ev_w = R32(ev, 0x10);
    if (id == 1 || id == 5)
        g_ev_str = (u16 *)R64(ev, 8), g_ev_caret = R32(ev, 0x10), g_ev_len = R32(ev, 0x20);
}

static void test_ime(void)
{
    void *pinit = fn("libSceIme", "sceImeParamInit");
    void *open = fn("libSceIme", "sceImeOpen");
    void *update = fn("libSceIme", "sceImeUpdate");
    void *close = fn("libSceIme", "sceImeClose");
    void *kopen = fn("libSceIme", "sceImeKeyboardOpen");
    void *kclose = fn("libSceIme", "sceImeKeyboardClose");
    u64 p[0x60 / 8];
    memset(p, 0x5a, sizeof(p));
    ((void (*)(void *))pinit)(p);
    expect(R32(p, 0) == 0xffffffff && p[1] == 0 && p[11] == 0, "ParamInit: user -1, the rest zero");
    expect_eq((u32)((f1)update)(NULL), 0x80BC0002, "Update before Open: NOT_OPENED");
    expect_eq((u32)((f2)open)(NULL, NULL), 0x80BC0031, "Open(NULL): INVALID_ADDRESS");
    W32(p, 0, 0x1000), W32(p, 0x24, 16), W64(p, 0x28, g_buf);
    expect_eq((u32)((f2)open)(p, NULL), 0x80BC0022, "no handler: INVALID_HANDLER");
    W64(p, 0x50, ime_handler);
    g_buf[0] = 0;
    g_user_name = "Yuna";
    expect_eq((u32)((f2)open)(p, NULL), 0, "Open");
    expect_eq((u32)((f2)open)(p, NULL), 0x80BC0001, "Open twice: BUSY");
    g_nev = 0;
    for (int i = 0; i < 5; i++)
        ((f1)update)(ime_handler);
    expect(g_nev == 3 && g_ev[0] == 0 && g_ev[1] == 1 && g_ev[2] == 5 && g_ev_w == 793,
           "events: Open (793 wide), UpdateText, PressEnter, then none");
    expect(g_ev_str == g_buf && g_ev_caret == 4 && g_ev_len == 4 && eq16(g_buf, "Yuna"),
           "the text events carry the user name in the title's buffer");
    expect_eq((u32)((f0)close)(), 0, "Close");
    expect_eq((u32)((f0)close)(), 0x80BC0002, "Close twice: NOT_OPENED");
    W32(p, 0x20, 4);
    ((f2)open)(p, NULL);
    g_nev = 0;
    for (int i = 0; i < 4; i++)
        ((f1)update)(NULL); /* the handler from Open */
    expect(g_nev == 2 && g_ev[0] == 0 && g_ev[1] == 4, "a password field: Open, PressClose");
    ((f0)close)();
    u64 kp[4] = {0, 0, (u64)ime_handler, 0};
    expect_eq((u32)((s32(*)(s32, const void *))kopen)(-1, kp), 0x80BC0010, "keyboard for user -1: INVALID_USER_ID");
    expect_eq((u32)((s32(*)(s32, const void *))kopen)(0x1000, kp), 0, "keyboard Open (never sends events)");
    expect_eq((u32)((s32(*)(s32, const void *))kopen)(0x1000, kp), 0x80BC0001, "keyboard Open twice: BUSY");
    expect_eq((u32)((s32(*)(s32))kclose)(0x1000), 0, "keyboard Close");
    expect_eq((u32)((s32(*)(s32))kclose)(0x1000), 0x80BC0002, "keyboard Close twice: NOT_OPENED");
}

/* ---- dialog sysmodules: 0 without loading ---- */
static void test_sysmodule(void)
{
    void *load = fn("libSceSysmodule", "sceSysmoduleLoadModule");
    void *load_arg = fn("libSceSysmodule", "sceSysmoduleLoadModuleInternalWithArg");
    void *is = fn("libSceSysmodule", "sceSysmoduleIsLoaded");
    typedef s32 (*fid)(u32);
    s32 res = 7;
    g_group = "sysmodule";
    expect_eq((u32)((fid)is)(0xb2), 0x805A1001, "IsLoaded(NpProfileDialog) before loading: UNLOADED");
    mark();
    expect(((fid)load)(0xa4) == 0 && ((fid)load)(0xb2) == 0 && g_core_loads == 0, "dialog ids load without the core");
    expect(logged("sysmodule: a4 (libSceMsgDialog) is implemented by the extension: not loaded"), "logged once");
    expect_eq((u32)((fid)is)(0xb2), 0, "IsLoaded(NpProfileDialog) after loading: 0");
    expect(((s32(*)(u32, size_t, const void *, u32, s32 *))load_arg)(0xa0, 0, NULL, 0, &res) == 0 && res == 0 &&
               g_core_load_args == 0,
           "InternalWithArg: a dialog id, res 0");
    expect(((fid)load)(0x102) == 0 && g_core_loads == 1, "other ids go to the core's override");
    expect(((s32(*)(u32, size_t, const void *, u32, s32 *))load_arg)(0x800000a4, 0, NULL, 0, &res) == 0 &&
               g_core_load_args == 1,
           "an internal id is never a dialog id");
    ((fid)is)(0x102);
    expect(g_host_is_loaded == 1, "IsLoaded of other ids asks the host");
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        out("usage: dialog_host EXT_BIN CRC");
        return 2;
    }
    static struct boot_ctx ctx;
    ctx.magic = BOOT_MAGIC;
    ctx.size = sizeof(ctx);
    ctx.ext_crc = sys_strtoul(argv[2]);
    g_ctx = &ctx;
    K.read = k_read, K.close = k_close, K.map_flex = k_map_flex, K.munmap = k_munmap;
    char *blob;
    ctx.ext_size = (u64)sys_read_file(argv[1], &blob);
    ctx.ext_fd = (u64)sys_open(argv[1]);
    /* the title: Ghost's SDK (8.00) in its proc param; every address counts
     * as inside it, so call sites show as eboot+offset */
    static u64 proc_param[8] = {0x40, 0x4942524f, 0x08008011};
    static struct module eboot = {.name = "eboot.bin", .is_main = true, .image_size = ~0UL >> 1};
    eboot.proc_param = (u64)proc_param;
    g_main_mod = g_modules = &eboot;

    expect(ext_fetch() == 0 && ext_start() == 0, "the extension loads");
    test_msg();
    test_error_and_online();
    test_savedata();
    test_ime_dialog();
    test_ime();
    test_sysmodule();
    g_group = "load";
    ext_boot_done();
    out("dialog_host: %u checks, %s", g_checks, g_failed ? "FAILED" : "all passed");
    return g_failed;
}
