/*
 * Drawn dialogs: a MsgDialog (messages, yes/no, progress, wait) and an
 * ImeDialog (an on-screen keyboard) drawn over the title's own frames, and
 * answered with the controller (hle_dialog.h).
 *
 * The real system draws them on a layer of its own, which a title in SWRR
 * never gets.  Here they go into the title's display buffers:
 *
 *   - which buffers: the core reports each sceVideoOutRegisterBuffers
 *     (ext_exports.video_buffers).  Only linear buffers of an 8-bit or
 *     10-bit RGB format that the CPU may write are drawn on: what a title
 *     that draws with the CPU, and ghostldr's software Piglet (hle_gles.c),
 *     show.  A GPU title's tiled buffers, flipped through Gnm, never are.
 *   - when: before each flip of such a buffer (sceVideoOutSubmitFlip, and
 *     hle_gles.c's present), over the title's finished frame.  A title that
 *     waits for the answer without drawing (Apollo spins on UpdateStatus)
 *     flips nothing: then the dialog is drawn into the buffer on screen at
 *     the dialog's status polls, and what it covered is put back when it
 *     closes.
 *   - input: while a dialog is drawn, the overlay reads the controller
 *     itself at the status polls and the title's own reads see nothing
 *     pressed (ext_exports.pad_state).
 *
 * A dialog opened when nothing drawable is on screen is answered at once,
 * as before (hle_dialog.c), as are all dialogs with --dialog-auto or a
 * title profile's "dialogs_auto" feature.
 * Drawing only writes (the display buffers are write-combined): text is
 * blended against the known panel colours, not the frame.  The font is the
 * selector's Noto Sans, rasterized at build time (dlg_font.h).
 */
#include "hle_dialog.h"
#include "dlg_font.h"
DLG_MINSIZE_BEGIN

typedef u16 char16;

#define FMT_ARGB 0x80000000u    /* A8R8G8B8_SRGB: 0xAARRGGBB */
#define FMT_ABGR 0x80002200u    /* A8B8G8R8_SRGB: 0xAABBGGRR */
#define FMT_A2RGB 0x88060000u   /* A2R10G10B10 */
#define FMT_A2RGB_S 0x88000000u /* A2R10G10B10_SRGB */
#define TILING_LINEAR 1
#define OV_BUFS 16
#define T_FLIPPING 120000ULL /* us: the title flipped this recently, so it draws the dialog's frames */
#define T_TICK 16000ULL      /* input and redraws at status polls: at most this often */
#define T_REPEAT0 420000ULL  /* held direction: first repeat */
#define T_REPEAT 90000ULL    /* then */

/* ScePadData.buttons */
#define P_OPTIONS 0x8u
#define P_UP 0x10u
#define P_RIGHT 0x20u
#define P_DOWN 0x40u
#define P_LEFT 0x80u
#define P_L2 0x100u
#define P_R2 0x200u
#define P_L1 0x400u
#define P_R1 0x800u
#define P_TRIANGLE 0x1000u
#define P_CIRCLE 0x2000u
#define P_CROSS 0x4000u
#define P_SQUARE 0x8000u
#define P_DIRS (P_UP | P_RIGHT | P_DOWN | P_LEFT)

/* colours (0xAARRGGBB) */
#define C_PANEL 0xFF20262Eu
#define C_BORDER 0xFF5A6A80u
#define C_TEXT 0xFFEDEFF2u
#define C_DIM 0xFF9AA4B2u
#define C_KEY 0xFF323C4Au
#define C_FOCUS 0xFF2F6FD0u
#define C_FIELD 0xFF141920u
#define C_BAR 0xFF4A90E2u

enum { OV_NONE, OV_MSG, OV_IME };
enum { TYPE_URL = 2, TYPE_MAIL = 3, TYPE_NUMBER = 4 };
#define IME_OPT_PASSWORD 4

struct ov_big {
    char text[1536];
    char label[2][48];
    char title[192];
    char16 ime[2049];
    u16 line_at[24], line_end[24]; /* the wrapped text */
};

static struct {
    volatile int lock;
    /* the display */
    s32 handle;
    u32 nbuf, fmt, w, h, pitch;
    bool linear;
    u64 addr[OV_BUFS];
    u8 writable[OV_BUFS];
    s32 shown; /* the buffer flipped last (-1: none) */
    u64 t_flip;
    /* the controller */
    s32 pad;
    u32 prev, rep_dir, confirm, back;
    volatile u32 held_after; /* buttons held when a dialog ended: the title sees them only once released */
    u64 t_rep, t_tick, t_drawn;
    bool enter_known;
    /* the dialog */
    volatile u32 kind;
    struct dlg *d;
    u8 nbtn, focus, cancel, progress, wait, nlines;
    u8 answer[2];
    s32 value;
    u64 t_open;
    bool dirty;
    /* what the dialog covers on the buffer it was drawn into directly */
    u32 *save;
    u64 save_size;
    s32 save_buf, rx, ry, rw, rh;
    u64 t_save;
    /* the keyboard */
    u32 ime_max, ime_len, ime_caret, ime_type, ime_opt;
    u8 kx, ky, layer, ime_result;
    struct ov_big *big;
} g_ov = {.shown = -1, .pad = -1, .save_buf = -1};

static bool trylock(void) { return !__atomic_exchange_n(&g_ov.lock, 1, __ATOMIC_ACQUIRE); }
static void lock(void) { spin_lock(&g_ov.lock); }
static void unlock(void) { spin_unlock(&g_ov.lock); }

/* ---- the display ---- */

static bool fmt_ok(u32 f) { return f == FMT_ARGB || f == FMT_ABGR || f == FMT_A2RGB || f == FMT_A2RGB_S; }

/* [a, a + n) is mapped for CPU writes */
static bool cpu_writable(u64 a, u64 n)
{
    s32 (*query)(void *, void **, void **, s32 *) = host_real("sceKernelQueryMemoryProtection");
    for (u32 k = 0; query && k < 8; k++) {
        void *st = NULL, *en = NULL;
        s32 prot = 0;
        if (query((void *)a, &st, &en, &prot) != 0 || !(prot & 2) || (u64)en <= a)
            return false;
        if ((u64)en >= a + n)
            return true;
        n -= (u64)en - a, a = (u64)en;
    }
    return false;
}

void ov_video_buffers(s32 handle, s32 start, const u64 *addrs, s32 num, const void *attr) HIDDEN;
void ov_video_buffers(s32 handle, s32 start, const u64 *addrs, s32 num, const void *attr)
{
    if (!addrs || !attr || num <= 0 || start < 0 || start >= OV_BUFS)
        return;
    u32 fmt = RD32(attr, 0), tiling = RD32(attr, 4), w = RD32(attr, 0xc), h = RD32(attr, 0x10),
        pitch = RD32(attr, 0x14);
    u8 wr[OV_BUFS];
    u32 n = (u32)num, ok = 0;
    if (start + n > OV_BUFS)
        n = OV_BUFS - (u32)start;
    for (u32 i = 0; i < n; i++)
        ok += wr[i] = tiling == TILING_LINEAR && fmt_ok(fmt) && cpu_writable(addrs[i], (u64)pitch * h * 4);
    lock();
    if (g_ov.handle != handle || start == 0) {
        memset(g_ov.addr, 0, sizeof(g_ov.addr));
        memset(g_ov.writable, 0, sizeof(g_ov.writable));
        g_ov.nbuf = 0, g_ov.shown = -1, g_ov.save_buf = -1;
    }
    g_ov.handle = handle, g_ov.fmt = fmt, g_ov.linear = tiling == TILING_LINEAR, g_ov.w = w, g_ov.h = h;
    g_ov.pitch = pitch;
    for (u32 i = 0; i < n; i++) {
        g_ov.addr[start + i] = addrs[i], g_ov.writable[start + i] = wr[i];
        if (start + i + 1 > g_ov.nbuf)
            g_ov.nbuf = start + i + 1;
    }
    unlock();
    logf("dialog: display buffers %d-%u on VideoOut handle %d: %ux%u, format %x, %s, %u the CPU may write: "
         "dialogs %s",
         start, start + n - 1, handle, w, h, fmt, tiling == TILING_LINEAR ? "linear" : "tiled", ok,
         ok ? "can be drawn over them" : "are answered at once");
}

/* ---- drawing (write only) ---- */

static u32 conv(u32 c)
{
    u32 f = g_ov.fmt;
    if (f == FMT_ABGR)
        return (c & 0xFF00FF00u) | (c >> 16 & 0xFF) | (c & 0xFF) << 16;
    if (f == FMT_A2RGB || f == FMT_A2RGB_S) {
        u32 r = c >> 16 & 0xFF, g = c >> 8 & 0xFF, b = c & 0xFF;
        return 3u << 30 | (r << 2 | r >> 6) << 20 | (g << 2 | g >> 6) << 10 | (b << 2 | b >> 6);
    }
    return c;
}

static u32 *g_px; /* the buffer being drawn into (lock held) */

static void fill(s32 x, s32 y, s32 w, s32 h, u32 argb)
{
    if (x < 0)
        w += x, x = 0;
    if (y < 0)
        h += y, y = 0;
    if (x + w > (s32)g_ov.w)
        w = (s32)g_ov.w - x;
    if (y + h > (s32)g_ov.h)
        h = (s32)g_ov.h - y;
    u32 c = conv(argb);
    for (s32 j = 0; j < h; j++) {
        u32 *row = g_px + (u64)(y + j) * g_ov.pitch + x;
        for (s32 i = 0; i < w; i++)
            row[i] = c;
    }
}

static void frame_rect(s32 x, s32 y, s32 w, s32 h, s32 t, u32 border, u32 inside)
{
    fill(x, y, w, h, border);
    fill(x + t, y + t, w - 2 * t, h - 2 * t, inside);
}

static const struct dlgf_glyph *glyph(u32 cp)
{
    if (cp >= DLGF_FIRST && cp < DLGF_FIRST + DLGF_ASCII)
        return &k_dlgf[cp - DLGF_FIRST];
    for (u32 i = 0; i < ARRAY_SIZE(k_dlgf_extra); i++)
        if (k_dlgf_extra[i] == cp)
            return &k_dlgf[DLGF_ASCII + i];
    return &k_dlgf['?' - DLGF_FIRST];
}

static u32 mix(u32 bg, u32 fg, u32 a15)
{
    u32 rb = ((bg & 0xFF00FFu) * (15 - a15) + (fg & 0xFF00FFu) * a15) / 15 & 0xFF00FFu;
    u32 g = ((bg & 0xFF00u) * (15 - a15) + (fg & 0xFF00u) * a15) / 15 & 0xFF00u;
    return 0xFF000000u | rb | g;
}

/* one glyph at pen x, baseline y; returns the advance */
static s32 draw_cp(s32 x, s32 y, u32 cp, u32 fg, u32 bg)
{
    const struct dlgf_glyph *g = glyph(cp);
    s32 x0 = x + g->left, y0 = y - g->top;
    for (u32 j = 0; j < g->h; j++) {
        s32 yy = y0 + (s32)j;
        if (yy < 0 || yy >= (s32)g_ov.h)
            continue;
        u32 *row = g_px + (u64)yy * g_ov.pitch;
        for (u32 i = 0; i < g->w; i++) {
            u32 k = j * g->w + i, a = k_dlgf_bits[g->off + k / 2] >> (k & 1) * 4 & 15;
            s32 xx = x0 + (s32)i;
            if (a && xx >= 0 && xx < (s32)g_ov.w)
                row[xx] = conv(a == 15 ? fg : mix(bg, fg, a));
        }
    }
    return g->adv;
}

static u32 utf8_cp(const char **ps)
{
    const u8 *p = (const u8 *)*ps;
    u32 c = *p++, n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (n)
        c &= 0x3Fu >> n;
    for (; n && (*p & 0xC0) == 0x80; n--)
        c = c << 6 | (*p++ & 0x3F);
    *ps = (const char *)p;
    return n ? '?' : c;
}

/* width of s[0..n) (n < 0: to its end) */
static s32 text_w(const char *s, s32 n)
{
    s32 w = 0;
    const char *e = n < 0 ? NULL : s + n;
    while (*s && (!e || s < e))
        w += glyph(utf8_cp(&s))->adv;
    return w;
}

static s32 draw_str(s32 x, s32 y, const char *s, s32 n, u32 fg, u32 bg)
{
    const char *e = n < 0 ? NULL : s + n;
    s32 x0 = x;
    while (*s && (!e || s < e))
        x += draw_cp(x, y, utf8_cp(&s), fg, bg);
    return x - x0;
}

/* word-wrap big->text into lines of at most maxw pixels */
static void wrap(s32 maxw)
{
    struct ov_big *b = g_ov.big;
    const char *t = b->text;
    u32 n = 0, at = 0, len = (u32)strlen(t);
    while (at < len && n < ARRAY_SIZE(b->line_at)) {
        u32 end = at, brk = 0;
        s32 w = 0;
        while (end < len && t[end] != '\n') {
            const char *p = t + end;
            s32 cw = glyph(utf8_cp(&p))->adv;
            if (w + cw > maxw && end > at)
                break;
            w += cw;
            end = (u32)(p - t);
            if (t[end] == ' ')
                brk = end;
        }
        if (end < len && t[end] != '\n' && brk > at)
            end = brk; /* break at the last space */
        b->line_at[n] = (u16)at, b->line_end[n] = (u16)end;
        n++;
        at = end;
        if (at < len && (t[at] == '\n' || t[at] == ' '))
            at++;
    }
    g_ov.nlines = (u8)n;
}

/* ---- the message panel ---- */

#define PAD 44
#define BTN_H 60
#define BTN_GAP 28

static void msg_rect(s32 *x, s32 *y, s32 *w, s32 *h)
{
    s32 pw = (s32)g_ov.w - 64 < 1200 ? (s32)g_ov.w - 64 : 1200;
    if (g_ov.dirty)
        wrap(pw - 2 * PAD);
    s32 ph = 2 * PAD + (s32)g_ov.nlines * DLGF_LINE;
    if (g_ov.progress)
        ph += 56;
    if (g_ov.wait && !g_ov.nbtn)
        ph += 44;
    if (g_ov.nbtn)
        ph += 36 + BTN_H;
    if (ph > (s32)g_ov.h - 32)
        ph = (s32)g_ov.h - 32;
    *w = pw, *h = ph, *x = ((s32)g_ov.w - pw) / 2, *y = ((s32)g_ov.h - ph) / 2;
}

static void draw_msg(u64 now)
{
    struct ov_big *b = g_ov.big;
    s32 x, y, w, h;
    msg_rect(&x, &y, &w, &h);
    frame_rect(x, y, w, h, 3, C_BORDER, C_PANEL);
    s32 ty = y + PAD + DLGF_ASCENT;
    for (u32 i = 0; i < g_ov.nlines && ty < y + h - PAD; i++, ty += DLGF_LINE) {
        s32 n = b->line_end[i] - b->line_at[i];
        s32 lw = text_w(b->text + b->line_at[i], n);
        draw_str(x + (w - lw) / 2, ty, b->text + b->line_at[i], n, C_TEXT, C_PANEL);
    }
    s32 by = y + PAD + (s32)g_ov.nlines * DLGF_LINE + 16;
    if (g_ov.progress) {
        s32 v = g_ov.value < 0 ? 0 : g_ov.value > 100 ? 100 : g_ov.value;
        s32 bw = w - 2 * PAD - 110;
        fill(x + PAD, by + 10, bw, 14, C_KEY);
        fill(x + PAD, by + 10, bw * v / 100, 14, C_BAR);
        char pct[8];
        fmt(pct, sizeof(pct), "%d%%", v);
        draw_str(x + w - PAD - text_w(pct, -1), by + 26, pct, -1, C_DIM, C_PANEL);
        by += 56;
    }
    if (g_ov.wait && !g_ov.nbtn) { /* busy dots */
        u32 lit = (u32)((now - g_ov.t_open) / 250000) % 4;
        for (u32 k = 0; k < 3; k++)
            fill(x + w / 2 - 40 + (s32)k * 32, by + 10, 16, 16, k < lit ? C_BAR : C_KEY);
    }
    if (g_ov.nbtn) {
        by += 20;
        s32 bw[2], total = 0;
        for (u32 i = 0; i < g_ov.nbtn; i++) {
            bw[i] = text_w(b->label[i], -1) + 80;
            if (bw[i] < 220)
                bw[i] = 220;
            total += bw[i] + (i ? BTN_GAP : 0);
        }
        s32 bx = x + (w - total) / 2;
        for (u32 i = 0; i < g_ov.nbtn; i++) {
            u32 bg = i == g_ov.focus ? C_FOCUS : C_KEY;
            frame_rect(bx, by, bw[i], BTN_H, 2, i == g_ov.focus ? C_TEXT : C_BORDER, bg);
            draw_str(bx + (bw[i] - text_w(b->label[i], -1)) / 2, by + BTN_H / 2 + DLGF_ASCENT / 2 - 3, b->label[i],
                     -1, C_TEXT, bg);
            bx += bw[i] + BTN_GAP;
        }
    }
    g_ov.rx = x, g_ov.ry = y, g_ov.rw = w, g_ov.rh = h;
}

/* ---- the keyboard ---- */

static const char *const k_rows[3][4] = {
    {"1234567890", "qwertyuiop", "asdfghjkl@", "zxcvbnm-_."},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL@", "ZXCVBNM-_."},
    {"!?\"'#$%&*+", "()[]{}<>|\\", ":;/=~^`,.@", "-_.!?/:+=%"},
};
enum { K_SHIFT, K_SYMBOLS, K_SPACE, K_DELETE, K_DONE }; /* the bottom row, two columns each */
#define KEY_H 64
#define KEY_GAP 10

static void kbd_rect(s32 *x, s32 *y, s32 *w, s32 *h)
{
    s32 pw = (s32)g_ov.w - 64 < 1400 ? (s32)g_ov.w - 64 : 1400;
    s32 ph = 2 * PAD + DLGF_LINE + 16 + 72 + 24 + 5 * KEY_H + 4 * KEY_GAP + 20 + DLGF_LINE;
    if (ph > (s32)g_ov.h - 16)
        ph = (s32)g_ov.h - 16;
    *w = pw, *h = ph, *x = ((s32)g_ov.w - pw) / 2, *y = ((s32)g_ov.h - ph) / 2;
}

static u32 ime_char(u32 i)
{
    char16 c = g_ov.big->ime[i];
    return (g_ov.ime_opt & IME_OPT_PASSWORD) ? 0x2022 : c;
}

static void draw_kbd(u64 now)
{
    struct ov_big *b = g_ov.big;
    s32 x, y, w, h;
    kbd_rect(&x, &y, &w, &h);
    frame_rect(x, y, w, h, 3, C_BORDER, C_PANEL);
    draw_str(x + PAD, y + PAD + DLGF_ASCENT, b->title, -1, C_TEXT, C_PANEL);
    /* the text, scrolled so the caret shows */
    s32 fx = x + PAD, fy = y + PAD + DLGF_LINE + 16, fw = w - 2 * PAD;
    frame_rect(fx, fy, fw, 72, 2, C_BORDER, C_FIELD);
    u32 from = 0;
    for (;;) {
        s32 cw = 0;
        for (u32 i = from; i < g_ov.ime_caret; i++)
            cw += glyph(ime_char(i))->adv;
        if (cw <= fw - 40 || from >= g_ov.ime_caret)
            break;
        from++;
    }
    s32 px = fx + 16, base = fy + 36 + DLGF_ASCENT / 2 - 2, caret_x = px;
    for (u32 i = from; i < g_ov.ime_len; i++) {
        if (i == g_ov.ime_caret)
            caret_x = px;
        s32 adv = glyph(ime_char(i))->adv;
        if (px + adv > fx + fw - 12)
            break;
        px += draw_cp(px, base, ime_char(i), C_TEXT, C_FIELD);
    }
    if (g_ov.ime_caret >= g_ov.ime_len)
        caret_x = px;
    if ((now - g_ov.t_open) / 500000 % 2 == 0)
        fill(caret_x, fy + 14, 3, 44, C_BAR);
    /* the keys */
    s32 ky0 = fy + 72 + 24, kw = (w - 2 * PAD - 9 * KEY_GAP) / 10;
    for (u32 r = 0; r < 5; r++) {
        s32 ky = ky0 + (s32)r * (KEY_H + KEY_GAP);
        for (u32 c = 0; c < 10; c += r == 4 ? 2 : 1) {
            s32 kx = x + PAD + (s32)c * (kw + KEY_GAP), kww = r == 4 ? 2 * kw + KEY_GAP : kw;
            bool focus = g_ov.ky == r && (r == 4 ? g_ov.kx / 2 == c / 2 : g_ov.kx == c);
            u32 bg = focus ? C_FOCUS : C_KEY;
            frame_rect(kx, ky, kww, KEY_H, 2, focus ? C_TEXT : C_BORDER, bg);
            char label[16];
            if (r < 4) {
                label[0] = k_rows[g_ov.layer][r][c], label[1] = 0;
            } else {
                static const char *const names[] = {"Shift", "#+=", "Space", "Delete", "Done"};
                strlcpy(label, names[c / 2], sizeof(label));
                if (c / 2 == K_SHIFT && g_ov.layer == 1)
                    strlcpy(label, "SHIFT", sizeof(label));
                if (c / 2 == K_SYMBOLS && g_ov.layer == 2)
                    strlcpy(label, "abc", sizeof(label));
            }
            draw_str(kx + (kww - text_w(label, -1)) / 2, ky + KEY_H / 2 + DLGF_ASCENT / 2 - 3, label, -1, C_TEXT, bg);
        }
    }
    static const char hint[] = "Square: delete   Triangle: space   L1/R1: move   L2: shift   R2: done   Circle: back";
    draw_str(x + (w - text_w(hint, -1)) / 2, y + h - PAD + 8, hint, -1, C_DIM, C_PANEL);
    g_ov.rx = x, g_ov.ry = y, g_ov.rw = w, g_ov.rh = h;
}

/* ---- the banner: a box at the bottom of every frame, not a dialog ----
 * What the user must know that no dialog of the title says: that a save
 * written through the save writer is kept only once SWRR is closed (until
 * SWRR closes: "sticky"), or why a feature cannot work on a stock console
 * (for some seconds, over a sticky one, which comes back after).  Up to
 * three lines, split at '\n'. */
static char g_banner[256], g_sticky[256];
static u64 g_banner_until; /* process time */

#define C_BANNER 0xFF3A2A10u
#define C_BANNER_EDGE 0xFFE0A030u
#define BANNER_LINES 3

static void draw_banner(u64 now)
{
    const char *text = g_banner[0] && now < g_banner_until ? g_banner : g_sticky;
    if (!text[0])
        return;
    const char *line[BANNER_LINES];
    s32 len[BANNER_LINES], n = 0, tw = 0;
    for (const char *q = text; *q && n < BANNER_LINES; n++) {
        const char *e = q;
        while (*e && *e != '\n')
            e++;
        line[n] = q, len[n] = (s32)(e - q);
        s32 lw = text_w(q, len[n]);
        tw = lw > tw ? lw : tw;
        q = *e ? e + 1 : e;
    }
    s32 w = tw + 80, h = 22 + n * DLGF_LINE;
    if (w > (s32)g_ov.w - 32)
        w = (s32)g_ov.w - 32;
    s32 x = ((s32)g_ov.w - w) / 2, y = (s32)g_ov.h - 110 - h;
    frame_rect(x, y, w, h, 3, C_BANNER_EDGE, C_BANNER);
    for (s32 i = 0; i < n; i++) {
        s32 lw = text_w(line[i], len[i]);
        draw_str(x + (w - (lw < w - 40 ? lw : w - 40)) / 2, y + 11 + i * DLGF_LINE + DLGF_ASCENT - 2, line[i], len[i],
                 C_TEXT, C_BANNER);
    }
}

/* show text at the bottom of the title's frames for seconds (0: until SWRR closes) */
void ov_banner(const char *text, u32 seconds)
{
    lock();
    if (seconds) {
        strlcpy(g_banner, text ? text : "", sizeof(g_banner));
        g_banner_until = dlg_now() + (u64)seconds * 1000000;
    } else {
        strlcpy(g_sticky, text ? text : "", sizeof(g_sticky));
        g_banner[0] = 0;
    }
    g_ov.dirty = true; /* a dialog drawn while nothing flips is drawn again, with it */
    unlock();
    logf("dialog: banner \"%s\"%s", text ? text : "", seconds ? "" : " until SWRR closes");
}

/* why something the title tried cannot work here, once per session */
#define NOTICE_SECONDS 20
void ov_notice(volatile int *once, const char *text)
{
    if (!__atomic_exchange_n(once, 1, __ATOMIC_ACQ_REL))
        ov_banner(text, NOTICE_SECONDS);
}

/* draw the dialog into buffer idx (lock held) */
static void draw(s32 idx, u64 now)
{
    g_px = (u32 *)g_ov.addr[idx];
    if (g_ov.kind == OV_MSG)
        draw_msg(now);
    else
        draw_kbd(now);
    g_ov.dirty = false, g_ov.t_drawn = now;
}

static bool drawable(s32 idx)
{
    return idx >= 0 && idx < OV_BUFS && g_ov.addr[idx] && g_ov.writable[idx] && g_ov.w >= 640 && g_ov.h >= 360 &&
           g_ov.pitch >= g_ov.w;
}

/* the panel's rectangle, before drawing into the shown buffer directly */
static void cover(s32 idx)
{
    s32 x, y, w, h;
    if (g_ov.kind == OV_MSG)
        msg_rect(&x, &y, &w, &h);
    else
        kbd_rect(&x, &y, &w, &h);
    if (g_ov.save_buf == idx && g_ov.t_save >= g_ov.t_flip && x >= g_ov.rx && y >= g_ov.ry &&
        x + w <= g_ov.rx + g_ov.rw && y + h <= g_ov.ry + g_ov.rh)
        return; /* already kept, and nothing flipped since */
    if (g_ov.save_buf != idx || g_ov.t_save < g_ov.t_flip) {
        /* first direct draw on this frame: keep what the panel covers */
        u64 size = ALIGN_UP((u64)w * (u64)h * 4, 0x4000);
        if (g_ov.save && g_ov.save_size < size) {
            page_free(g_ov.save, g_ov.save_size);
            g_ov.save = NULL;
        }
        if (!g_ov.save && (g_ov.save = page_alloc(size)))
            g_ov.save_size = size;
        g_ov.save_buf = -1;
        if (!g_ov.save)
            return;
        const u32 *src = (const u32 *)g_ov.addr[idx];
        for (s32 j = 0; j < h; j++)
            memcpy(g_ov.save + (u64)j * (u64)w, src + (u64)(y + j) * g_ov.pitch + x, (u64)w * 4);
        g_ov.save_buf = idx, g_ov.t_save = dlg_now();
        g_ov.rx = x, g_ov.ry = y, g_ov.rw = w, g_ov.rh = h;
    }
}

/* put back what the panel covered, if that frame is still on screen (lock held) */
static void uncover(void)
{
    s32 idx = g_ov.save_buf;
    if (idx >= 0 && g_ov.save && idx == g_ov.shown && g_ov.t_save >= g_ov.t_flip && drawable(idx)) {
        u32 *dst = (u32 *)g_ov.addr[idx];
        for (s32 j = 0; j < g_ov.rh; j++)
            memcpy(dst + (u64)(g_ov.ry + j) * g_ov.pitch + g_ov.rx, g_ov.save + (u64)j * (u64)g_ov.rw,
                   (u64)g_ov.rw * 4);
    }
    g_ov.save_buf = -1;
}

/* the title (or hle_gles.c) is about to flip buffer idx */
void ov_present(s32 handle, s32 idx) HIDDEN;
void ov_present(s32 handle, s32 idx)
{
    if (handle != g_ov.handle || idx < 0 || idx >= OV_BUFS)
        return;
    u64 now = dlg_now();
    lock();
    g_ov.shown = idx, g_ov.t_flip = now;
    if (g_ov.kind != OV_NONE && drawable(idx))
        draw(idx, now);
    if ((g_banner[0] || g_sticky[0]) && drawable(idx)) {
        g_px = (u32 *)g_ov.addr[idx];
        draw_banner(now);
    }
    unlock();
}

static s32 hle_sceVideoOutSubmitFlip(s32 handle, s32 idx, u32 mode, s64 arg)
{
    static s32 (*real)(s32, s32, u32, s64);
    if (!real)
        real = (s32(*)(s32, s32, u32, s64))host_real_in("libSceVideoOut", "sceVideoOutSubmitFlip");
    ov_present(handle, idx);
    return real ? real(handle, idx, mode, arg) : (s32)0x80290001; /* INVALID_VALUE */
}

/* the buffer on screen: the one flipped last through here, else what
 * VideoOut says (lock not held) */
static void sync_shown(void)
{
    s32 (*status)(s32, void *) = host_real_in("libSceVideoOut", "sceVideoOutGetFlipStatus");
    u8 st[0x40];
    memset(st, 0, sizeof(st));
    if (status && g_ov.nbuf && status(g_ov.handle, st) == 0) {
        s32 cur = (s32)RD32(st, 0x38); /* currentBuffer */
        lock();
        static u32 said;
        if (cur != g_ov.shown && said++ < 4)
            logf("dialog: VideoOut shows buffer %d, the last flipped here was %d", cur, g_ov.shown);
        if (g_ov.shown < 0)
            g_ov.shown = cur;
        unlock();
    }
}

/* can a dialog be drawn now? */
static bool ready(void)
{
    if ((g_ctx->flags & CTX_FLAG_DIALOG_AUTO) || (profile_has && profile_has(PF_DIALOG_AUTO)))
        return false;
    sync_shown();
    lock();
    bool ok = g_ov.kind == OV_NONE && g_ov.linear && fmt_ok(g_ov.fmt) && drawable(g_ov.shown);
    unlock();
    return ok;
}

static bool big(void)
{
    if (!g_ov.big)
        g_ov.big = page_alloc(ALIGN_UP(sizeof(struct ov_big), 0x4000));
    return g_ov.big != NULL;
}

/* ---- the controller ---- */

static u32 read_pad(void)
{
    static s32 (*rd)(s32, u8 *);
    static s32 (*get)(s32, s32, s32);
    static s32 (*user)(s32 *);
    if (!rd)
        rd = (s32(*)(s32, u8 *))host_real_in("libScePad", "scePadReadState");
    s32 h = __atomic_load_n(&g_ov.pad, __ATOMIC_RELAXED);
    if (h < 0) { /* the title has not read it yet: the initial user's */
        if (!get)
            get = (s32(*)(s32, s32, s32))host_real_in("libScePad", "scePadGetHandle");
        if (!user)
            user = (s32(*)(s32 *))host_real_in("libSceUserService", "sceUserServiceGetInitialUser");
        s32 u = -1;
        if (get && user && user(&u) == 0 && (h = get(u, 0, 0)) >= 0)
            g_ov.pad = h;
    }
    u8 data[0x80];
    memset(data, 0, sizeof(data));
    if (!rd || h < 0 || rd(h, data) != 0)
        return 0;
    u32 b = *(u32 *)data & 0xFFFFu;
    /* the left stick as the d-pad */
    if (data[4] < 0x30)
        b |= P_LEFT;
    else if (data[4] > 0xD0)
        b |= P_RIGHT;
    if (data[5] < 0x30)
        b |= P_UP;
    else if (data[5] > 0xD0)
        b |= P_DOWN;
    return b;
}

/* the title's pad state: nothing pressed while a dialog has the controller */
void ov_pad_state(s32 handle, u8 *data) HIDDEN;
void ov_pad_state(s32 handle, u8 *data)
{
    if (g_ov.pad != handle)
        __atomic_store_n(&g_ov.pad, handle, __ATOMIC_RELAXED);
    if (!data)
        return;
    if (__atomic_load_n(&g_ov.kind, __ATOMIC_ACQUIRE) != OV_NONE) {
        *(u32 *)data &= 0x80000000u; /* keep INTERCEPTED: what the system shows a title under its dialog */
        data[4] = data[5] = data[6] = data[7] = 0x80;
        data[8] = data[9] = 0;
        return;
    }
    u32 held = __atomic_load_n(&g_ov.held_after, __ATOMIC_RELAXED);
    if (held) { /* the press that answered the dialog is not the title's */
        u32 b = *(u32 *)data;
        __atomic_store_n(&g_ov.held_after, held & b, __ATOMIC_RELAXED);
        *(u32 *)data = b & ~held;
    }
}

static void enter_button(void)
{
    if (g_ov.enter_known)
        return;
    g_ov.enter_known = true;
    s32 (*get)(s32, s32 *) = host_real_in("libSceSystemService", "sceSystemServiceParamGetInt");
    s32 v = 1;
    if (get && get(1000 /* ENTER_BUTTON_ASSIGN */, &v) == 0 && v == 0)
        g_ov.confirm = P_CIRCLE, g_ov.back = P_CROSS;
    else
        g_ov.confirm = P_CROSS, g_ov.back = P_CIRCLE;
}

/* presses since the last tick, with held directions repeating */
static u32 presses(u32 now_b, u64 now)
{
    u32 p = now_b & ~g_ov.prev, dir = now_b & P_DIRS;
    if (dir && dir == g_ov.rep_dir) {
        if (now >= g_ov.t_rep) {
            p |= dir;
            g_ov.t_rep = now + T_REPEAT;
        }
    } else {
        g_ov.rep_dir = dir;
        g_ov.t_rep = now + T_REPEAT0;
    }
    g_ov.prev = now_b;
    return p;
}

static void ime_insert(u32 c)
{
    if (g_ov.ime_len >= g_ov.ime_max)
        return;
    if (g_ov.ime_type == TYPE_NUMBER && !((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+'))
        return;
    char16 *t = g_ov.big->ime;
    for (u32 i = g_ov.ime_len; i > g_ov.ime_caret; i--)
        t[i] = t[i - 1];
    t[g_ov.ime_caret++] = (char16)c;
    t[++g_ov.ime_len] = 0;
}

static void ime_delete(void)
{
    if (!g_ov.ime_caret)
        return;
    char16 *t = g_ov.big->ime;
    for (u32 i = g_ov.ime_caret - 1; i < g_ov.ime_len; i++)
        t[i] = t[i + 1];
    g_ov.ime_caret--, g_ov.ime_len--;
}

/* the keyboard's input (lock held) */
static void ime_input(u32 p)
{
    if (p & P_LEFT) {
        if (g_ov.ky == 4)
            g_ov.kx = g_ov.kx >= 2 ? (u8)(g_ov.kx / 2 * 2 - 2) : 8;
        else
            g_ov.kx = g_ov.kx ? (u8)(g_ov.kx - 1) : 9;
    }
    if (p & P_RIGHT) {
        if (g_ov.ky == 4)
            g_ov.kx = g_ov.kx < 8 ? (u8)(g_ov.kx / 2 * 2 + 2) : 0;
        else
            g_ov.kx = g_ov.kx < 9 ? (u8)(g_ov.kx + 1) : 0;
    }
    if (p & P_UP)
        g_ov.ky = g_ov.ky ? (u8)(g_ov.ky - 1) : 4;
    if (p & P_DOWN)
        g_ov.ky = g_ov.ky < 4 ? (u8)(g_ov.ky + 1) : 0;
    if (p & P_L1 && g_ov.ime_caret)
        g_ov.ime_caret--;
    if (p & P_R1 && g_ov.ime_caret < g_ov.ime_len)
        g_ov.ime_caret++;
    if (p & P_L2)
        g_ov.layer = g_ov.layer == 1 ? 0 : 1;
    if (p & P_SQUARE)
        ime_delete();
    if (p & P_TRIANGLE)
        ime_insert(' ');
    if (p & g_ov.confirm) {
        if (g_ov.ky < 4)
            ime_insert((u8)k_rows[g_ov.layer][g_ov.ky][g_ov.kx]);
        else
            switch (g_ov.kx / 2) {
            case K_SHIFT:
                g_ov.layer = g_ov.layer == 1 ? 0 : 1;
                break;
            case K_SYMBOLS:
                g_ov.layer = g_ov.layer == 2 ? 0 : 2;
                break;
            case K_SPACE:
                ime_insert(' ');
                break;
            case K_DELETE:
                ime_delete();
                break;
            default:
                g_ov.ime_result = 1;
            }
    }
    if (p & (P_R2 | P_OPTIONS))
        g_ov.ime_result = 1;
    if (p & g_ov.back)
        g_ov.ime_result = 2;
}

void ov_tick(void)
{
    if (__atomic_load_n(&g_ov.kind, __ATOMIC_ACQUIRE) == OV_NONE)
        return;
    u64 now = dlg_now();
    if (now - g_ov.t_tick < T_TICK || !trylock())
        return;
    struct dlg *answered = NULL;
    u32 answer = 0;
    if (g_ov.kind != OV_NONE) {
        g_ov.t_tick = now;
        enter_button();
        u32 p = presses(read_pad(), now);
        if (p) {
            g_ov.dirty = true;
            if (g_ov.kind == OV_IME)
                ime_input(p);
            else if (g_ov.nbtn) {
                if (p & P_LEFT && g_ov.focus)
                    g_ov.focus--;
                if (p & P_RIGHT && g_ov.focus + 1 < g_ov.nbtn)
                    g_ov.focus++;
                if (p & g_ov.confirm)
                    answered = g_ov.d, answer = g_ov.answer[g_ov.focus];
                else if (p & g_ov.back && g_ov.cancel != 0xFF)
                    answered = g_ov.d, answer = g_ov.cancel;
            }
        }
        /* nothing flips: draw into the frame on screen */
        bool anim = g_ov.kind == OV_IME || g_ov.wait;
        if (!answered && now - g_ov.t_flip >= T_FLIPPING && drawable(g_ov.shown) &&
            (g_ov.dirty || (anim && now - g_ov.t_drawn >= 250000))) {
            static u32 said;
            if (said++ < 3)
                logf("dialog: drawn into buffer %d on screen (the title last flipped %lu ms ago)", g_ov.shown,
                     (now - g_ov.t_flip) / 1000);
            cover(g_ov.shown);
            draw(g_ov.shown, now);
            draw_banner(now); /* a modal title's dialog: the box under it too */
        }
    }
    unlock();
    if (answered)
        dlg_user_answer(answered, answer);
}

/* A thread of the overlay's own ticks while a dialog is open: titles that
 * open a wait dialog and then work without polling (Apollo while it copies)
 * still get it drawn, with its busy dots. */
static volatile int g_thread;

static void *ov_thread(void *arg)
{
    for (;;) {
        for (u32 idle = 0; idle < 64;) {
            K.usleep(30000);
            if (__atomic_load_n(&g_ov.kind, __ATOMIC_ACQUIRE) == OV_NONE)
                idle++;
            else
                idle = 0, ov_tick();
        }
        __atomic_store_n(&g_thread, 0, __ATOMIC_RELEASE);
        int no = 0;
        if (__atomic_load_n(&g_ov.kind, __ATOMIC_ACQUIRE) == OV_NONE ||
            !__atomic_compare_exchange_n(&g_thread, &no, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return NULL;
    }
}

static void start_thread(void)
{
    int no = 0;
    if (!__atomic_compare_exchange_n(&g_thread, &no, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    s32 (*create)(u64 *, const void *, void *(*)(void *), void *, const char *) = host_real("scePthreadCreate");
    s32 (*detach)(u64) = host_real("scePthreadDetach");
    u64 th = 0;
    s32 rc = create ? create(&th, NULL, ov_thread, NULL, "ghostldr_dialog") : -1;
    if (rc != 0) {
        logf("dialog: no thread of its own (%x): drawn at the title's status polls only", (u32)rc);
        __atomic_store_n(&g_thread, 0, __ATOMIC_RELEASE);
        return;
    }
    ext_thread_note(th, "ghostldr_dialog");
    if (detach)
        detach(th);
}

/* ---- the dialogs ---- */

bool ov_msg_open(struct dlg *d, const struct ov_msg *m)
{
    if (!ready() || !big())
        return false;
    lock();
    struct ov_big *b = g_ov.big;
    strlcpy(b->text, m->text ? m->text : "", sizeof(b->text));
    g_ov.nbtn = 0;
    for (u32 i = 0; i < 2 && m->label[i]; i++) {
        strlcpy(b->label[i], m->label[i], sizeof(b->label[i]));
        g_ov.answer[i] = m->answer[i];
        g_ov.nbtn++;
    }
    g_ov.focus = m->focus < g_ov.nbtn ? m->focus : 0;
    g_ov.cancel = m->cancel, g_ov.progress = m->progress, g_ov.wait = m->wait, g_ov.value = 0;
    g_ov.d = d, g_ov.t_open = dlg_now(), g_ov.dirty = true, g_ov.t_tick = 0;
    g_ov.prev = read_pad(), g_ov.rep_dir = 0; /* a button still held from before does nothing */
    __atomic_store_n(&g_ov.kind, OV_MSG, __ATOMIC_RELEASE);
    unlock();
    start_thread();
    logf("dialog: %s drawn over the title's frames: the user answers (%u buttons)", d->name, g_ov.nbtn);
    return true;
}

void ov_msg_progress(struct dlg *d, s32 value, const char *msg)
{
    lock();
    if (g_ov.kind == OV_MSG && g_ov.d == d) {
        if (value >= 0)
            g_ov.value = value;
        if (msg)
            strlcpy(g_ov.big->text, msg, sizeof(g_ov.big->text));
        g_ov.dirty = true;
    }
    unlock();
    ov_tick();
}

static void end_locked(void)
{
    g_ov.held_after = g_ov.prev & ~(u32)P_DIRS;
    uncover();
    g_ov.d = NULL;
    __atomic_store_n(&g_ov.kind, OV_NONE, __ATOMIC_RELEASE);
}

void ov_end(struct dlg *d)
{
    if (__atomic_load_n(&g_ov.kind, __ATOMIC_ACQUIRE) != OV_MSG)
        return;
    lock();
    if (g_ov.kind == OV_MSG && g_ov.d == d)
        end_locked();
    unlock();
}

bool ov_ime_open(const char16 *text, u32 max, u32 type, u32 option, const char16 *title)
{
    if (!text || !ready() || !big())
        return false;
    lock();
    struct ov_big *b = g_ov.big;
    if (max > ARRAY_SIZE(b->ime) - 1)
        max = ARRAY_SIZE(b->ime) - 1;
    u32 n = 0;
    while (n < max && text[n])
        b->ime[n] = text[n], n++;
    b->ime[n] = 0;
    /* the title as UTF-8 (BMP) */
    u32 o = 0;
    for (u32 i = 0; title && title[i] && o + 4 < sizeof(b->title); i++) {
        u32 c = title[i];
        if (c < 0x80)
            b->title[o++] = (char)c;
        else if (c < 0x800)
            b->title[o++] = (char)(0xC0 | c >> 6), b->title[o++] = (char)(0x80 | (c & 0x3F));
        else
            b->title[o++] = (char)(0xE0 | c >> 12), b->title[o++] = (char)(0x80 | (c >> 6 & 0x3F)),
            b->title[o++] = (char)(0x80 | (c & 0x3F));
    }
    b->title[o] = 0;
    g_ov.ime_max = max, g_ov.ime_len = g_ov.ime_caret = n, g_ov.ime_type = type, g_ov.ime_opt = option;
    g_ov.kx = 0, g_ov.ky = 1, g_ov.layer = 0, g_ov.ime_result = 0;
    g_ov.d = NULL, g_ov.t_open = dlg_now(), g_ov.dirty = true, g_ov.t_tick = 0, g_ov.wait = 0;
    g_ov.prev = read_pad(), g_ov.rep_dir = 0;
    __atomic_store_n(&g_ov.kind, OV_IME, __ATOMIC_RELEASE);
    unlock();
    start_thread();
    logf("dialog: ImeDialog keyboard drawn over the title's frames (type %u, at most %u characters)", type, max);
    return true;
}

int ov_ime_state(char16 *out, u32 max)
{
    lock();
    int st = g_ov.kind != OV_IME ? 2 : g_ov.ime_result;
    if (st == 1 && out) {
        u32 n = g_ov.ime_len < max ? g_ov.ime_len : max;
        memcpy(out, g_ov.big->ime, n * sizeof(char16));
        out[n] = 0;
    }
    unlock();
    return st;
}

void ov_ime_end(void)
{
    lock();
    if (g_ov.kind == OV_IME)
        end_locked();
    unlock();
}

/* ---- for failure.c (docs/failures.md): a whole page of ghostldr's own ----
 * drawn into a linear A8R8G8B8 buffer it flips itself (after a crash or an
 * exit, over any title), the title's VideoOut handle, the controller, and
 * whether a drawn dialog is open (the title waits for the user, not stuck) */
HIDDEN s32 ov_vo_handle(void) { return g_ov.nbuf ? g_ov.handle : -1; }
HIDDEN u32 ov_buttons(void) { return read_pad(); }
HIDDEN bool ov_open(void) { return __atomic_load_n(&g_ov.kind, __ATOMIC_ACQUIRE) != OV_NONE; }

/* the lines of text that fit maxw, each drawn at x from y down (fg 0: only
 * measured): the next y */
static s32 page_text(s32 x, s32 y, s32 maxw, const char *t, u32 fg, u32 bg)
{
    while (*t) {
        const char *e = t, *brk = NULL;
        s32 w = 0;
        while (*e && *e != '\n') {
            const char *p = e;
            s32 cw = glyph(utf8_cp(&p))->adv;
            if (w + cw > maxw && e > t)
                break;
            if (*e == ' ')
                brk = e;
            w += cw, e = p;
        }
        if (*e && *e != '\n' && brk)
            e = brk;
        if (fg)
            draw_str(x, y, t, (s32)(e - t), fg, bg);
        y += DLGF_LINE;
        t = *e ? e + 1 : e;
    }
    return y;
}

/* headline, body and foot (dimmed), in a panel sized to them */
static s32 page_lines(s32 x, s32 y, s32 pw, const char *head, const char *body, const char *foot, bool draw)
{
    if (draw)
        fill(x, y + DLGF_ASCENT + 14, 120, 4, C_BAR);
    y = page_text(x, y + DLGF_ASCENT, pw, head, draw ? C_TEXT : 0, C_PANEL) + DLGF_LINE / 2;
    y = page_text(x, y + DLGF_ASCENT, pw, body, draw ? C_TEXT : 0, C_PANEL);
    if (foot)
        y = page_text(x, y + DLGF_ASCENT + DLGF_LINE / 2, pw, foot, draw ? C_DIM : 0, C_PANEL);
    return y;
}

static void page_draw(u32 *px, u32 w, u32 h, u32 pitch, u32 fmt, const char *head, const char *body,
                      const char *foot)
{
    u32 sw = g_ov.w, sh = g_ov.h, sp = g_ov.pitch, sf = g_ov.fmt;
    u32 *spx = g_px;
    g_ov.w = w, g_ov.h = h, g_ov.pitch = pitch, g_ov.fmt = fmt, g_px = px;
    fill(0, 0, (s32)w, (s32)h, 0xFF0E1116u);
    s32 pw = (s32)w - 160 < 1400 ? (s32)w - 160 : 1400, x = ((s32)w - pw) / 2;
    s32 ph = page_lines(x, 0, pw, head, body, foot, false) + PAD, y = ((s32)h - ph) / 2;
    frame_rect(x - PAD, y - PAD, pw + 2 * PAD, ph + 2 * PAD, 3, C_BORDER, C_PANEL);
    page_lines(x, y, pw, head, body, foot, true);
    g_ov.w = sw, g_ov.h = sh, g_ov.pitch = sp, g_ov.fmt = sf, g_px = spx;
}

HIDDEN void ov_page(u32 *px, u32 w, u32 h, u32 pitch, const char *head, const char *body, const char *foot)
{
    lock();
    page_draw(px, w, h, pitch, FMT_ARGB, head, body, foot);
    unlock();
}

/* the page into the title's buffer on screen, when the CPU can draw there
 * (its frames are linear): true when drawn */
HIDDEN bool ov_page_shown(const char *head, const char *body, const char *foot)
{
    sync_shown();
    lock();
    s32 idx = g_ov.shown;
    bool ok = g_ov.linear && fmt_ok(g_ov.fmt) && drawable(idx);
    if (ok)
        page_draw((u32 *)g_ov.addr[idx], g_ov.w, g_ov.h, g_ov.pitch, g_ov.fmt, head, body, foot);
    unlock();
    return ok;
}

static const struct ext_hle_ent k_hle[] = {
    {"libSceVideoOut", "sceVideoOutSubmitFlip", hle_sceVideoOutSubmitFlip, NULL},
    /* not a system library: the banner for the tests */
    {"libGhostldrDialog", "ghostldrBanner", ov_banner, NULL},
};

DLG_MINSIZE_END

EXT_FAMILY("dialog_overlay", k_hle, NULL);
