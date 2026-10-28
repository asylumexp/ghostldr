/*
 * Drawn dialogs (ext/src/dlg_overlay.c) of a real ext.bin, in an ordinary
 * x86-64 process, like dialog.c: two 1920x1080 display buffers in this
 * process's memory stand for the title's, a fake VideoOut records flips, a
 * fake controller is pressed, and MsgDialog and ImeDialog are driven the way
 * Apollo drives them.  test_dialog_overlay.py builds and runs it:
 *
 *     overlay_host EXT_BIN CRC OUT_DIR
 *
 * Writes OUT_DIR/msg.ppm and OUT_DIR/ime.ppm (the frames with a dialog on
 * them) and prints "overlay_host: ok|FAILED [group] ..." per check.
 */
#include "ghost_api.h"

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
int sys_write_file(const char *path, const void *data, u64 n); /* overlay_sys.c */
int sys_thread(void *(*fn)(void *), void *arg);
int sys_usleep(u32 us);

struct kfuncs K;
struct boot_ctx *g_ctx;
struct module *g_modules, *g_main_mod;
u64 g_reported_dmem;

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
static s32 fmtx(char *buf, u64 n, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    s32 r = sys_vfmt(buf, n, f, ap);
    __builtin_va_end(ap);
    return r;
}
static void mark(void) { g_mark = g_loglen; }
static bool logged(const char *s) { return strstr(g_log + g_mark, s) != NULL; }

static int g_failed;
static u32 g_checks;
static const char *g_group = "load";
static void expect(bool ok, const char *what)
{
    g_checks++;
    out("overlay_host: %s [%s] %s", ok ? "ok" : "FAILED", g_group, what);
    if (!ok)
        g_failed = 1;
}
static void expect_eq(u64 got, u64 want, const char *what)
{
    if (got != want)
        out("overlay_host:   got %llx, want %llx", (unsigned long long)got, (unsigned long long)want);
    expect(got == want, what);
}

/* ---- the core services ---- */
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
u64 arena_small_alloc_top(u64 size, u64 keep) { return 0; }
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

/* ---- the fakes: clock, display, controller ---- */
#define W 1920
#define H 1080
#define HANDLE 7
#define PAD 5
static u64 g_now = 1000000;
static u64 fake_time(void) { return g_now; }
static u32 *g_fb[2];
static s32 g_current = -1;
static u32 g_flips;
static u32 g_pad, g_enter = 1;
static s32 fake_flip(s32 h, s32 idx, u32 mode, s64 arg)
{
    g_flips++;
    if (h == HANDLE)
        g_current = idx;
    return 0;
}
static s32 fake_flip_status(s32 h, u8 *st)
{
    *(s32 *)(st + 0x38) = g_current;
    return 0;
}
static s32 fake_query(void *addr, void **start, void **end, s32 *prot)
{
    for (u32 i = 0; i < 2; i++)
        if ((u64)addr >= (u64)g_fb[i] && (u64)addr < (u64)g_fb[i] + (u64)W * H * 4) {
            *start = g_fb[i], *end = (u8 *)g_fb[i] + (u64)W * H * 4, *prot = 0x33;
            return 0;
        }
    *prot = 0x30; /* GPU only */
    *start = addr, *end = (u8 *)addr + 0x10000;
    return 0;
}
static s32 fake_pad_read(s32 h, u8 *data)
{
    memset(data, 0, 0x68);
    *(u32 *)data = g_pad;
    data[4] = data[5] = data[6] = data[7] = 0x80;
    data[0x4c] = 1;
    return h == PAD ? 0 : -1;
}
static s32 fake_pad_handle(s32 user, s32 type, s32 index) { return PAD; }
static s32 fake_initial_user(s32 *u) { return *u = 0x1000, 0; }
static s32 fake_param_int(s32 id, s32 *v) { return id == 1000 ? (*v = (s32)g_enter, 0) : -1; }

static u32 g_threads;
static s32 fake_thread_create(u64 *th, const void *attr, void *(*fn)(void *), void *arg, const char *name)
{
    g_threads++;
    *th = 1;
    return sys_thread(fn, arg);
}
static s32 fake_detach(u64 th) { return 0; }

static void *host_real_(const char *name)
{
    if (strcmp(name, "scePthreadCreate") == 0)
        return (void *)fake_thread_create;
    if (strcmp(name, "scePthreadDetach") == 0)
        return (void *)fake_detach;
    if (strcmp(name, "sceKernelGetProcessTime") == 0)
        return (void *)fake_time;
    if (strcmp(name, "sceKernelQueryMemoryProtection") == 0)
        return (void *)fake_query;
    return NULL;
}
static void *host_real_in_(const char *mod, const char *name)
{
    if (strcmp(name, "sceVideoOutSubmitFlip") == 0)
        return (void *)fake_flip;
    if (strcmp(name, "sceVideoOutGetFlipStatus") == 0)
        return (void *)fake_flip_status;
    if (strcmp(name, "scePadReadState") == 0)
        return (void *)fake_pad_read;
    if (strcmp(name, "scePadGetHandle") == 0)
        return (void *)fake_pad_handle;
    if (strcmp(name, "sceUserServiceGetInitialUser") == 0)
        return (void *)fake_initial_user;
    if (strcmp(name, "sceSystemServiceParamGetInt") == 0)
        return (void *)fake_param_int;
    return NULL;
}
static u64 hle_lookup_(const char *lib, const char *name) { return 0; }
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
    .page_alloc = page_alloc,
    .page_free = page_free,
};

static int k_close(int fd) { return sys_close(fd); }
static ssize_t k_read(int fd, void *buf, size_t n) { return sys_read(fd, buf, n); }
static int k_map_flex(void **addr, size_t len, int prot, int flags)
{
    *addr = sys_map(len);
    return 0;
}
static int k_munmap(void *addr, size_t len) { return 0; }

static void *fn(const char *lib, const char *name)
{
    char nid[12];
    compute_nid(name, nid);
    void *f = (void *)ext_hle_lookup(lib, nid, name);
    if (!f) {
        out("overlay_host: FAILED %s:%s is not in the extension", lib, name);
        g_failed = 1;
    }
    return f;
}

#define W32(p, off, v) (*(u32 *)((u8 *)(p) + (off)) = (u32)(v))
#define W64(p, off, v) (*(u64 *)((u8 *)(p) + (off)) = (u64)(v))
typedef s32 (*f0)(void);
typedef u32 (*u0)(void);
typedef s32 (*f1)(const void *);
typedef s32 (*f2)(const void *, const void *);
typedef s32 (*fu2)(u32, u32);
typedef s32 (*fus)(u32, const char *);
typedef s32 (*fflip)(s32, s32, u32, s64);

static void *msg_init, *msg_open, *msg_update, *msg_result, *msg_close, *msg_term, *submit;
static u64 g_msgp[0x88 / 8], g_user[0x30 / 8], g_prog[0x50 / 8];

static u8 *msg_param(u32 mode)
{
    u8 *p = (u8 *)g_msgp;
    memset(p, 0, 0x88);
    *(u64 *)p = 0x30;
    *(u32 *)(p + 0x2c) = 0xC0D1A109u + (u32)(uintptr_t)p;
    *(u64 *)(p + 0x30) = 0x88;
    W32(p, 0x38, mode);
    W64(p, 0x40, g_user), W64(p, 0x48, g_prog);
    return p;
}

static u32 open_user(u32 type, const char *text)
{
    memset(g_user, 0, sizeof(g_user));
    W32(g_user, 0, type);
    W64(g_user, 8, text);
    return (u32)((f1)msg_open)(msg_param(1));
}

#define BG 0xFF102030u
static void paint(u32 idx)
{
    for (u64 i = 0; i < (u64)W * H; i++)
        g_fb[idx][i] = BG;
}
static u32 px(u32 idx, u32 x, u32 y) { return g_fb[idx][(u64)y * W + x]; }

/* a status poll 20 ms later, with the controller as given */
static u32 poll(u32 buttons)
{
    g_now += 20000, g_pad = buttons;
    return ((u0)msg_update)();
}
static u32 press(u32 b)
{
    poll(b);
    return poll(0);
}

static void write_ppm(const char *dir, const char *name, u32 idx)
{
    static u8 img[32 + W * H * 3];
    char path[512];
    s32 n = fmtx((char *)img, 32, "P6\n%d %d\n255\n", W, H);
    for (u64 i = 0; i < (u64)W * H; i++) {
        u32 c = g_fb[idx][i];
        img[n + i * 3] = (u8)(c >> 16), img[n + i * 3 + 1] = (u8)(c >> 8), img[n + i * 3 + 2] = (u8)c;
    }
    fmtx(path, sizeof(path), "%s/%s", dir, name);
    sys_write_file(path, img, (u64)n + (u64)W * H * 3);
}

#define P_RIGHT 0x20u
#define P_DOWN 0x40u
#define P_LEFT 0x80u
#define P_R2 0x200u
#define P_TRIANGLE 0x1000u
#define P_CIRCLE 0x2000u
#define P_CROSS 0x4000u
#define P_SQUARE 0x8000u
#define CTX_FLAG_DIALOG_AUTO 0x400000

static void register_buffers(u32 tiling)
{
    u64 addrs[2] = {(u64)g_fb[0], (u64)g_fb[1]};
    u32 attr[8] = {0x80000000u, tiling, 0, W, H, W, 0, 0};
    ext_video_buffers(HANDLE, 0, addrs, 2, attr);
}

static void test_msg(const char *dir)
{
    g_group = "auto";
    expect_eq((u32)((f0)msg_init)(), 0, "MsgDialog Initialize");
    mark();
    expect_eq(open_user(0, "Nothing on screen yet"), 0, "OK dialog, no display buffers");
    expect(poll(0) == 2 && poll(0) == 3, "answered at once, as before");
    expect(!logged("drawn over"), "not drawn");

    g_group = "msg";
    register_buffers(1);
    paint(0), paint(1);
    expect_eq((u32)((fflip)submit)(HANDLE, 0, 1, 0), 0, "the title flips buffer 0 (through the extension)");
    expect_eq(g_current, 0, "the real flip happened");
    g_now += 500000; /* then waits for the answer without drawing, as Apollo does */
    mark();
    W64(g_user, 8, 0);
    g_pad = P_CROSS; /* still held from the press that opened it */
    expect_eq(open_user(1, "Delete this save?\nCUSA07992 SCUS-97429 will be gone for good. Nothing on this "
                           "console can bring it back afterwards."),
              0, "YESNO dialog");
    expect(logged("Msg drawn over the title's frames: the user answers (2 buttons)"), "drawn");
    expect_eq(poll(P_CROSS), 2, "RUNNING, with Cross held from before the dialog");
    expect(px(0, W / 2, H / 2) != BG && px(0, 5, 5) == BG, "the panel is drawn into the frame on screen, only there");
    g_now += 30 * 1000000ULL;
    expect_eq(poll(0), 2, "still RUNNING after 30 s: no timeout for a drawn dialog");
    u8 data[0x68];
    memset(data, 0, sizeof(data));
    *(u32 *)data = P_CROSS, data[4] = 0x10;
    ext_pad_state(PAD, data);
    expect(*(u32 *)data == 0 && data[4] == 0x80, "the title's own pad reads show nothing pressed meanwhile");
    expect_eq(poll(0), 2, "released: still RUNNING");
    press(P_RIGHT);
    write_ppm(dir, "msg.ppm", 0);
    expect_eq(press(P_CROSS), 3, "Right, then Cross: FINISHED");
    u32 r[11];
    memset(r, 0, sizeof(r));
    expect_eq((u32)((f1)msg_result)(r), 0, "GetResult");
    expect(r[1] == 0 && r[2] == 2, "No (button 2)");
    expect(px(0, W / 2, H / 2) == BG, "what the panel covered is back");
    expect(logged("Msg answered on screen: BUTTON2"), "logged");
    memset(data, 0, sizeof(data));
    *(u32 *)data = 0;
    ext_pad_state(PAD, data);
    *(u32 *)data = P_CROSS;
    ext_pad_state(PAD, data);
    expect(*(u32 *)data == P_CROSS, "the title has the controller again");

    open_user(0, "Back out");
    poll(0);
    expect_eq(poll(P_CIRCLE), 3, "OK dialog, Circle: FINISHED");
    *(u32 *)data = P_CIRCLE;
    ext_pad_state(PAD, data);
    expect(*(u32 *)data == 0, "the Circle that answered it, still held, is not the title's");
    *(u32 *)data = 0;
    ext_pad_state(PAD, data);
    *(u32 *)data = P_CIRCLE;
    ext_pad_state(PAD, data);
    expect(*(u32 *)data == P_CIRCLE, "a new press is");
    poll(0);
    ((f1)msg_result)(r);
    expect(r[1] == 1, "USER_CANCELED");

    /* the title keeps flipping: drawn into its frames before each flip */
    open_user(3, "Copy it?");
    paint(1);
    ((fflip)submit)(HANDLE, 1, 1, 0);
    expect(px(1, W / 2, H / 2) != BG, "drawn into the next frame before its flip");
    expect_eq(press(P_CROSS), 3, "OK_CANCEL, Cross: OK");
    ((f1)msg_result)(r);
    expect(r[1] == 0 && r[2] == 1, "OK (button 1)");

    /* progress bar and wait: no buttons, the title closes them */
    u8 *pp = msg_param(2);
    memset(g_prog, 0, sizeof(g_prog));
    W64(g_prog, 8, "Downloading apollo-patches.zip");
    g_now += 500000;
    expect_eq((u32)((f1)msg_open)(pp), 0, "progress bar");
    expect_eq((u32)((fu2)fn("libSceMsgDialog", "sceMsgDialogProgressBarSetValue"))(0, 40), 0, "40%");
    expect_eq(poll(P_CROSS), 2, "RUNNING whatever is pressed");
    expect_eq((u32)((f0)msg_close)(), 0, "closed by the title");
    /* a title that opens a wait dialog and works without polling (Apollo while it copies) */
    g_now += 500000;
    paint(0);
    ((fflip)submit)(HANDLE, 0, 1, 0);
    g_now += 500000;
    expect_eq(open_user(5, "Loading save games..."), 0, "WAIT, then no polls");
    expect(g_threads >= 1, "the overlay's thread started");
    for (int k = 0; k < 40 && px(0, W / 2, H / 2) == BG; k++)
        sys_usleep(25000);
    expect(px(0, W / 2, H / 2) != BG, "drawn by the overlay's own thread");
    expect_eq((u32)((f0)msg_close)(), 0, "closed by the title");
    expect(px(0, W / 2, H / 2) == BG, "and gone");
    expect_eq(open_user(5, "Copying files..."), 0, "WAIT");
    g_now += 30 * 1000000ULL;
    expect_eq(poll(P_CIRCLE), 2, "WAIT: RUNNING after 30 s, Circle does nothing");
    expect_eq((u32)((f0)msg_close)(), 0, "closed by the title");
    expect(px(0, W / 2, H / 2) == BG, "nothing left on screen");

    g_group = "banner";
    void (*banner)(const char *, u32) = fn("libGhostldrDialog", "ghostldrBanner");
    banner("The change was refused.", 5);
    paint(1);
    ((fflip)submit)(HANDLE, 1, 1, 0);
    expect(px(1, W / 2, H - 140) != BG && px(1, W / 2, H / 2) == BG, "a banner at the bottom of the next frame");
    g_now += 6000000;
    paint(1);
    ((fflip)submit)(HANDLE, 1, 1, 0);
    expect(px(1, W / 2, H - 140) == BG, "gone after its seconds");
    banner("Saved. To finish saving, close SWRR from the PS button menu.", 0);
    banner("Encrypted saves on USB (PS4/SAVEDATA) can't be opened on a stock console.\n"
           "Use decrypted saves (PS4/APOLLO) instead.", 5);
    paint(1);
    ((fflip)submit)(HANDLE, 1, 1, 0);
    expect(px(1, W / 2, H - 140) != BG && px(1, W / 2, H - 190) != BG, "two lines over the one until SWRR closes");
    write_ppm(dir, "banner.ppm", 1);
    g_now += 6000000;
    paint(1);
    ((fflip)submit)(HANDLE, 1, 1, 0);
    expect(px(1, W / 2, H - 140) != BG && px(1, W / 2, H - 190) == BG, "then that one again, until SWRR closes");
    write_ppm(dir, "banner-sticky.ppm", 1);
    banner("", 0);
    paint(1);
    ((fflip)submit)(HANDLE, 1, 1, 0);
    banner("Trophies are system data.", 5);
    g_now += 1000000; /* the title stops flipping: a modal dialog of its own */
    expect_eq(open_user(5, "No save-games found"), 0, "a dialog while the title does not flip");
    for (int k = 0; k < 40 && px(1, W / 2, H - 140) == BG; k++)
        sys_usleep(25000);
    expect(px(1, W / 2, H - 140) != BG, "the banner is drawn with it into the frame on screen");
    write_ppm(dir, "banner-modal.ppm", 1);
    expect_eq((u32)((f0)msg_close)(), 0, "closed by the title");

    g_group = "auto";
    g_ctx->flags |= CTX_FLAG_DIALOG_AUTO;
    mark();
    open_user(0, "--dialog-auto");
    expect(poll(0) == 2 && poll(0) == 3 && !logged("drawn over"), "--dialog-auto: answered at once");
    g_ctx->flags &= ~(u64)CTX_FLAG_DIALOG_AUTO;
    register_buffers(0);
    ((fflip)submit)(HANDLE, 0, 1, 0);
    mark();
    open_user(0, "tiled");
    expect(poll(0) == 2 && poll(0) == 3 && !logged("drawn over"), "tiled buffers (a GPU title's): answered at once");
    register_buffers(1);
    ((fflip)submit)(HANDLE, 0, 1, 0);
    expect_eq((u32)((f0)msg_term)(), 0, "Terminate");
}

static u16 g_buf[64];
static u64 g_imep[0x60 / 8];

static u8 *ime_param(u32 type, u32 max)
{
    u8 *p = (u8 *)g_imep;
    memset(p, 0, 0x60);
    W32(p, 0, 0x1000), W32(p, 4, type), W32(p, 0x24, max);
    W64(p, 0x28, g_buf);
    W64(p, 0x48, u"Enter the URL");
    return p;
}

static bool eq16(const u16 *a, const char *ascii)
{
    for (; *ascii; a++, ascii++)
        if (*a != (u8)*ascii)
            return false;
    return *a == 0;
}

static void *ime_init, *ime_status, *ime_result, *ime_term;
static u32 ipoll(u32 buttons)
{
    g_now += 20000, g_pad = buttons;
    return ((u0)ime_status)();
}
static u32 ipress(u32 b)
{
    ipoll(b);
    return ipoll(0);
}

static void test_ime(const char *dir)
{
    g_group = "ime";
    ime_init = fn("libSceImeDialog", "sceImeDialogInit");
    ime_status = fn("libSceImeDialog", "sceImeDialogGetStatus");
    ime_result = fn("libSceImeDialog", "sceImeDialogGetResult");
    ime_term = fn("libSceImeDialog", "sceImeDialogTerm");
    memset(g_buf, 0, sizeof(g_buf));
    g_buf[0] = 'h', g_buf[1] = 't';
    paint(0);
    ((fflip)submit)(HANDLE, 0, 1, 0);
    g_now += 500000;
    mark();
    expect_eq((u32)((f2)ime_init)(ime_param(2 /* URL */, 32), NULL), 0, "ImeDialog Init, a URL field");
    expect(logged("keyboard drawn over the title's frames"), "the keyboard is drawn");
    expect_eq(ipoll(0), 1, "Running");
    expect(px(0, W / 2, H / 2) != BG, "on screen");
    ipress(P_DOWN);                         /* from 'q' to 'a' */
    ipress(P_CROSS);                        /* "hta" */
    ipress(P_TRIANGLE);                     /* "hta " */
    ipress(P_SQUARE);                       /* "hta" */
    ipress(P_RIGHT), ipress(P_CROSS);       /* "htas" */
    write_ppm(dir, "ime.ppm", 0);
    expect_eq(ipress(P_R2), 2, "R2: Finished");
    u32 r[4] = {9, 9, 9, 9};
    expect_eq((u32)((f1)ime_result)(r), 0, "GetResult");
    expect_eq(r[0], 0, "OK");
    expect(eq16(g_buf, "htas"), "the text typed is in the title's buffer");
    expect_eq((u32)((f0)ime_term)(), 0, "Term");
    expect(px(0, W / 2, H / 2) == BG, "the keyboard is gone");

    g_buf[0] = 'x', g_buf[1] = 0;
    ((f2)ime_init)(ime_param(0, 32), NULL);
    ipoll(0);
    ipress(P_CROSS);
    expect_eq(ipress(P_CIRCLE), 2, "Circle: Finished");
    ((f1)ime_result)(r);
    expect_eq(r[0], 1, "USER_CANCELED");
    expect(eq16(g_buf, "x"), "the title's buffer untouched");
    ((f0)ime_term)();
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        out("usage: overlay_host EXT_BIN CRC OUT_DIR");
        return 2;
    }
    static struct boot_ctx ctx;
    ctx.magic = BOOT_MAGIC;
    ctx.size = sizeof(ctx);
    ctx.ext_crc = sys_strtoul(argv[2]);
    g_ctx = &ctx;
    K.read = k_read, K.close = k_close, K.map_flex = k_map_flex, K.munmap = k_munmap, K.usleep = sys_usleep;
    char *blob;
    ctx.ext_size = (u64)sys_read_file(argv[1], &blob);
    ctx.ext_fd = (u64)sys_open(argv[1]);
    static u64 proc_param[8] = {0x40, 0x4942524f, 0x04508101};
    static struct module eboot = {.name = "eboot.bin", .is_main = true, .image_size = ~0UL >> 1};
    eboot.proc_param = (u64)proc_param;
    g_main_mod = g_modules = &eboot;
    g_fb[0] = sys_map((u64)W * H * 4), g_fb[1] = sys_map((u64)W * H * 4);

    expect(ext_fetch() == 0 && ext_start() == 0, "the extension loads");
    msg_init = fn("libSceMsgDialog", "sceMsgDialogInitialize");
    msg_open = fn("libSceMsgDialog", "sceMsgDialogOpen");
    msg_update = fn("libSceMsgDialog", "sceMsgDialogUpdateStatus");
    msg_result = fn("libSceMsgDialog", "sceMsgDialogGetResult");
    msg_close = fn("libSceMsgDialog", "sceMsgDialogClose");
    msg_term = fn("libSceMsgDialog", "sceMsgDialogTerminate");
    submit = fn("libSceVideoOut", "sceVideoOutSubmitFlip");
    test_msg(argv[3]);
    test_ime(argv[3]);
    out("overlay_host: %u checks, %s", g_checks, g_failed ? "FAILED" : "all passed");
    return g_failed;
}
