/*
 * The selector's screen, after the PS4 home screen (1920x1080): the focused
 * game's backdrop fills the screen, dimmed towards the edges; a row of square
 * tiles, the focused one larger and lit; its name, details and a Start button
 * under it; the user and the clock at the top.  All of it is drawn here, by
 * the module's own code: no image, sound or font from the console's system
 * software is copied, only the games' own art (or art stored with the
 * install, host/gfs_meta.py) is shown.
 *
 * Frames are drawn only when something changes, and then only the rows that
 * changed (S->changed: the top bar, the row, or everything).
 */
#include "sel.h"

/* layout (pixels at 1920x1080) */
#define MARGIN 96
#define TOP_Y 82            /* baseline of the top bar */
#define TOP_BAND 136        /* rows the top bar can touch */
#define ROW_Y 232           /* top of the tiles (top-aligned, the focused one grows down) */
#define FOCUS_X 96          /* left edge of the focused tile */
#define GAP 26
#define NAME_PX 50
#define INFO_PX 27
#define CLOCK_PX 32
#define USER_PX 27
#define HINT_PX 26
#define NAME_Y (ROW_Y + TILE_BIG + 80)  /* baseline of the focused title's name */
#define INFO_Y (NAME_Y + 52)
#define PILL_Y (INFO_Y + 38)            /* the Start button */
#define PILL_H 62
#define ROW_BAND0 (ROW_Y - 40)          /* rows the row and its text can touch */
#define ROW_BAND1 (PILL_Y + PILL_H + 36)
#define PANEL_W 700

/* timing (microseconds) */
#define SPRING_W 26.0f      /* focus movement: a critically damped spring, rad/s (~0.3 s to settle) */
#define REPEAT_FIRST 380000
#define REPEAT_NEXT 120000
#define BG_SETTLE 180000
#define FADE_US 420000.0f
#define PANEL_US 200000.0f
#define LAUNCH_US 360000.0f

#define WHITE 0xFFFFFFFFu
#define SOFT 0xC8FFFFFFu
#define FAINT 0x8CFFFFFFu
#define SHADOW 0x90000000u
#define WARN 0xFFFFB347u
#define DEFAULT_TINT 0xFF2E64C8u /* the backdrop before (or without) a title's */

/* FNV-1a of the ID (or name): a stable, fairly saturated colour */
u32 title_tint(const char *id, const char *name)
{
    const char *s = id && id[0] ? id : name;
    u32 h = 2166136261u;
    for (; s && *s; s++)
        h = (h ^ (u8)*s) * 16777619u;
    static const u32 base[8] = {0x2E6FD8, 0xD8562E, 0x2EA36B, 0x8E44C8, 0xC8A12E, 0x2EA0B8, 0xC83E6E, 0x5C6E8A};
    return 0xFF000000u | base[h & 7];
}

static const char *display_name(struct sel *S, const struct title *t)
{
    if (t->name[0] && text_fits_font(S, t->name))
        return t->name;
    if (t->id[0])
        return t->id;
    return t->name[0] ? t->name : "Unknown title";
}

static void text_shadowed(struct sel *S, surface *s, int px, int x, int y, const char *str, u32 c, int max_w)
{
    int o = px >= 40 ? 2 : 1;
    draw_text(S, s, px, x + o, y + o, str, ((SHADOW >> 24) * (c >> 24) / 255) << 24, max_w);
    draw_text(S, s, px, x, y, str, c, max_w);
}

/* Noto Sans has no bold: two passes a pixel apart, for the big name */
static void text_bold(struct sel *S, surface *s, int px, int x, int y, const char *str, u32 c, int max_w)
{
    u32 sh = ((SHADOW >> 24) * (c >> 24) / 255) << 24;
    draw_text(S, s, px, x + 2, y + 3, str, sh, max_w);
    draw_text(S, s, px, x, y, str, c, max_w);
    draw_text(S, s, px, x + 1, y, str, c, max_w);
}

/* Greedy word wrap of str into at most max lines of width w (the last one
 * takes the rest: draw_text cuts it with an ellipsis); returns the count. */
static int wrap_lines(struct sel *S, int px, int w, const char *str, char lines[][128], int max)
{
    int n = 0;
    const char *p = str;
    while (*p == ' ')
        p++;
    while (*p && n < max) {
        char *ln = lines[n];
        if (n == max - 1) {
            str_copy(ln, p, 128);
            return n + 1;
        }
        int len = 0;
        for (;;) {
            int end = len;
            while (p[end] == ' ')
                end++;
            int wend = end;
            while (p[wend] && p[wend] != ' ')
                wend++;
            if (wend == end || wend > 127)
                break;
            __builtin_memcpy(ln, p, (u64)wend);
            ln[wend] = 0;
            int tw = text_width(S, px, ln);
            if (len && tw > w)
                break;
            len = wend;
            if (tw > w)
                break; /* one word wider than the line */
        }
        if (!len) /* a word longer than the buffer */
            while (p[len] && len < 127)
                len++;
        __builtin_memcpy(ln, p, (u64)len);
        ln[len] = 0;
        n++;
        p += len;
        while (*p == ' ')
            p++;
    }
    return n;
}

static int lines_fit(struct sel *S, int px, int w, char lines[][128], int n)
{
    for (int i = 0; i < n; i++)
        if (text_width(S, px, lines[i]) > w)
            return 0;
    return 1;
}

/* ---- backdrops ---- */

static void prepare(struct sel *S, int t, surface *dst)
{
    if (t >= 0 && S->titles[t].bg_state != 2) {
        u64 m = sel_mark(S);
        surface img;
        int ok = sel_load_art(S, t, SEL_BACKDROP, &img) == 0 && img.w >= 64 && img.h >= 36;
        if (ok)
            draw_cover(dst, &img);
        sel_release(S, m);
        S->titles[t].bg_state = ok ? 1 : 2;
        if (ok) {
            draw_shade(dst);
            return;
        }
    }
    if (t >= 0)
        S->titles[t].bg_src = ART_GENERATED;
    draw_generated_bg(dst, t >= 0 ? S->titles[t].tint : DEFAULT_TINT);
    draw_shade(dst);
}

void ui_prepare_bg(struct sel *S, int t)
{
    prepare(S, t, &S->bg[!S->bg_cur]);
    S->bg_cur = !S->bg_cur;
    S->bg_from_valid = 1;
    S->bg_fade = 0;
    S->bg_title = t;
    S->changed |= CH_ALL;
}

/* ---- tiles ---- */

/* a tile without a picture: its colour, and its name written on it */
static void fallback_icon(struct sel *S, struct title *t, surface *s)
{
    draw_fallback_tile(s, t->tint);
    const char *name = display_name(S, t);
    int pad = s->w / 10, w = s->w - 2 * pad;
    char lines[3][128];
    int px = s->w * 12 / 100, n = 0;
    for (;; px -= 2) { /* the largest size at which it fits in three lines */
        n = wrap_lines(S, px, w, name, lines, 3);
        if (px <= s->w * 7 / 100 || lines_fit(S, px, w, lines, n))
            break;
    }
    int lh = px * 125 / 100;
    int y = (s->h - n * lh) / 2 + px * 95 / 100 - s->h / 20;
    for (int i = 0; i < n; i++, y += lh) {
        int tw = text_width(S, px, lines[i]);
        int x = (s->w - (tw < w ? tw : w)) / 2;
        draw_text(S, s, px, x + 1, y + 2, lines[i], 0x60000000u, w);
        draw_text(S, s, px, x, y, lines[i], 0xF4FFFFFFu, w);
    }
    if (t->id[0] && name != t->id) {
        int ipx = s->w * 5 / 100, iw = text_width(S, ipx, t->id);
        draw_text(S, s, ipx, (s->w - iw) / 2, s->h - s->h / 11, t->id, 0x99FFFFFFu, 0);
    }
}

void ui_load_icon(struct sel *S, int i)
{
    struct title *t = &S->titles[i];
    if (t->icon_state)
        return;
    u64 m = sel_mark(S);
    surface img;
    if (sel_load_art(S, i, SEL_ICON, &img) == 0 && img.w >= 16 && img.h >= 16) {
        draw_resample(&t->icon, &img);
        draw_resample(&t->icon_small, &img);
        t->icon_state = 1;
    } else {
        fallback_icon(S, t, &t->icon);
        draw_resample(&t->icon_small, &t->icon);
        t->icon_state = 2;
        t->icon_src = ART_GENERATED;
    }
    sel_release(S, m);
    S->changed |= S->panel > 0 ? CH_ALL : CH_ROW;
}

/* where tile i sits when tile k is focused */
static void slot(int k, int i, float *x, float *size)
{
    if (i == k)
        *x = FOCUS_X, *size = TILE_BIG;
    else if (i > k)
        *x = (float)(FOCUS_X + TILE_BIG + GAP + (i - k - 1) * (TILE_SMALL + GAP)), *size = TILE_SMALL;
    else
        *x = (float)(FOCUS_X - (k - i) * (TILE_SMALL + GAP)), *size = TILE_SMALL;
}

static void tile_geom(float pos, int i, int *x, int *size)
{
    int k = (int)pos;
    if (pos < 0)
        k = -1;
    float f = pos - (float)k;
    float x0, s0, x1, s1;
    slot(k, i, &x0, &s0);
    slot(k + 1, i, &x1, &s1);
    float e = f * f * (3 - 2 * f); /* the size changes along an S curve */
    *x = (int)(x0 + (x1 - x0) * f + 0.5f);
    *size = (int)(s0 + (s1 - s0) * e + 0.5f);
}

/* ---- init / input / animation ---- */

int ui_init(struct sel *S, struct title *titles, int n, int initial)
{
    S->titles = titles;
    S->ntitles = n;
    S->sel = clampi(initial, 0, n - 1);
    S->pos = (float)S->sel;
    S->bg_title = -2;
    S->bg_cur = 0;
    surface *all[3] = {&S->back, &S->bg[0], &S->bg[1]};
    for (int i = 0; i < 3; i++) {
        *all[i] = (surface){0, SCREEN_W, SCREEN_H, SCREEN_W, 0, 0};
        all[i]->px = sel_alloc(S, (u64)SCREEN_W * SCREEN_H * 4);
        if (!all[i]->px)
            return -1;
    }
    for (int i = 0; i < n; i++) {
        struct title *t = &titles[i];
        t->tint = title_tint(t->id, t->name);
        t->icon = (surface){sel_alloc(S, (u64)TILE_BIG * TILE_BIG * 4), TILE_BIG, TILE_BIG, TILE_BIG, 0, 0};
        t->icon_small = (surface){sel_alloc(S, (u64)TILE_SMALL * TILE_SMALL * 4), TILE_SMALL, TILE_SMALL, TILE_SMALL,
                                  0, 0};
        if (!t->icon.px || !t->icon_small.px)
            return -1;
    }
    prepare(S, -1, &S->bg[0]);
    S->changed = CH_ALL;
    return 0;
}

void ui_set_clock(struct sel *S, int h, int m, int h24)
{
    if (S->clock_valid && S->clock_h == h && S->clock_m == m && S->clock_24h == h24)
        return;
    S->clock_h = h, S->clock_m = m, S->clock_24h = h24, S->clock_valid = 1;
    S->changed |= CH_TOP;
}

void ui_set_user(struct sel *S, const char *name)
{
    str_copy(S->user, name ? name : "", sizeof(S->user));
    S->changed |= CH_TOP;
}

static void toast(struct sel *S, u64 now, const char *msg)
{
    str_copy(S->toast, msg, sizeof(S->toast));
    S->toast_until_us = now + 2600000;
    S->changed |= CH_ALL;
}

static float approach(float v, float target, float step)
{
    if (v < target)
        return v + step > target ? target : v + step;
    return v - step < target ? target : v - step;
}

int ui_step(struct sel *S, u64 now, u64 dt, u32 buttons, int stick_x, int *boot)
{
    u32 pressed = buttons & ~S->held;
    S->held = buttons;
    if (dt > 200000)
        dt = 200000;

    /* left / right with key repeat (d-pad or left stick) */
    int dir = (buttons & PAD_LEFT) || stick_x < 64 ? -1 : (buttons & PAD_RIGHT) || stick_x > 192 ? 1 : 0;
    int step = 0;
    if (!S->launching && !S->panel_open) {
        if (dir && dir != S->repeat_dir) {
            step = dir;
            S->repeat_at_us = now + REPEAT_FIRST;
        } else if (dir && now >= S->repeat_at_us) {
            step = dir;
            S->repeat_at_us = now + REPEAT_NEXT;
        }
        if (pressed & PAD_L1)
            step = -5;
        if (pressed & PAD_R1)
            step = 5;
    }
    S->repeat_dir = dir;
    if (step) {
        int n = clampi(S->sel + step, 0, S->ntitles - 1);
        if (n != S->sel) {
            S->sel = n;
            S->last_move_us = now;
            S->changed |= CH_ROW;
        }
    }

    if (!S->launching) {
        struct title *t = &S->titles[S->sel];
        if (pressed & PAD_CROSS) {
            if (t->flags & TITLE_COMPLETE) {
                S->launching = 1;
                S->panel_open = 0;
            } else {
                toast(S, now, t->note[0] ? t->note : "This game's install is incomplete: run gfs_install.py again.");
            }
        } else if (pressed & (PAD_OPTIONS | PAD_TRIANGLE)) {
            S->panel_open = !S->panel_open;
        } else if ((pressed & PAD_CIRCLE) && S->panel_open) {
            S->panel_open = 0;
        }
    }

    /* animation: the row on a critically damped spring (no overshoot) */
    float target = (float)S->sel, before = S->pos;
    int steps = (int)(dt / 8000) + 1; /* steps of at most 8 ms keep it stable when a frame is late */
    float sub = (float)dt / 1e6f / (float)steps;
    for (int i = 0; i < steps; i++) {
        float acc = SPRING_W * SPRING_W * (target - S->pos) - 2 * SPRING_W * S->vel;
        S->vel += acc * sub;
        S->pos += S->vel * sub;
    }
    if ((S->pos - target) * (S->pos - target) < 0.00001f && S->vel * S->vel < 0.0025f)
        S->pos = target, S->vel = 0; /* under a pixel away and slow: there */
    if (S->pos != before)
        S->changed |= CH_ROW;
    float panel_before = S->panel;
    S->panel = approach(S->panel, S->panel_open ? 1.0f : 0.0f, (float)dt / PANEL_US);
    float fade_before = S->bg_fade;
    S->bg_fade = approach(S->bg_fade, 1.0f, (float)dt / FADE_US);
    if (S->launching) {
        S->launch = approach(S->launch, 1.0f, (float)dt / LAUNCH_US);
        if (S->launch >= 1.0f && boot)
            *boot = S->sel;
        S->changed |= CH_ALL;
    }
    if (S->panel != panel_before || S->bg_fade != fade_before)
        S->changed |= CH_ALL;

    /* the selected game's backdrop once the row has settled */
    if (S->bg_title != S->sel && S->pos == target && now - S->last_move_us >= BG_SETTLE && !S->launching)
        ui_prepare_bg(S, S->sel);
    /* icons near the selection, one per frame */
    for (int d = 0; d <= 18; d++) {
        int i = d & 1 ? S->sel - (d + 1) / 2 : S->sel + d / 2;
        if (i >= 0 && i < S->ntitles && !S->titles[i].icon_state) {
            ui_load_icon(S, i);
            break;
        }
    }
    if (S->toast_until_us && now >= S->toast_until_us) {
        S->toast_until_us = 0;
        S->changed |= CH_ALL;
    }
    return S->changed != 0;
}

/* ---- drawing ---- */

static void cross_icon(surface *s, float cx, float cy, float r, u32 c)
{
    draw_disc(s, cx, cy, r, 2.4f, c);
    float k = r * 0.42f;
    draw_line(s, cx - k, cy - k, cx + k, cy + k, 2.6f, c);
    draw_line(s, cx - k, cy + k, cx + k, cy - k, 2.6f, c);
}

static void circle_icon(surface *s, float cx, float cy, float r, u32 c)
{
    draw_disc(s, cx, cy, r, 2.4f, c);
    draw_disc(s, cx, cy, r * 0.48f, 2.6f, c);
}

/* "OPTIONS" in a pill, as the button is labelled on the controller */
static int options_icon(struct sel *S, surface *s, int x, int base, u32 c)
{
    int w = text_width(S, 16, "OPTIONS") + 20;
    draw_round_rect(s, (float)x, (float)(base - 25), (float)w, 32, 16, 2.2f, c);
    draw_text(S, s, 16, x + 10, base - 3, "OPTIONS", c, 0);
    return w;
}

/* button hints, right-aligned at the bottom: [icon] label pairs */
static void hints(struct sel *S, surface *s, float panel)
{
    int base = SCREEN_H - 66, x = SCREEN_W - MARGIN;
    if (panel > 0.5f) {
        u32 start_c = S->titles[S->sel].flags & TITLE_COMPLETE ? SOFT : 0x60FFFFFFu;
        int w1 = text_width(S, HINT_PX, "Back"), w2 = text_width(S, HINT_PX, "Start");
        x -= w1;
        text_shadowed(S, s, HINT_PX, x, base, "Back", SOFT, 0);
        circle_icon(s, (float)x - 29, (float)base - 10, 16, SOFT);
        x -= 74 + w2;
        text_shadowed(S, s, HINT_PX, x, base, "Start", start_c, 0);
        cross_icon(s, (float)x - 29, (float)base - 10, 16, start_c);
    } else {
        int w1 = text_width(S, HINT_PX, "Information");
        x -= w1;
        text_shadowed(S, s, HINT_PX, x, base, "Information", SOFT, 0);
        int ow = text_width(S, 16, "OPTIONS") + 20;
        options_icon(S, s, x - 12 - ow, base, SOFT);
    }
}

static void fmt_size(char *out, int cap, u64 bytes)
{
    if (!bytes) {
        str_copy(out, "Unknown", cap);
        return;
    }
    u64 tenths = (bytes * 10 + (1ull << 29)) >> 30;
    if (tenths < 10) {
        u64 mb = (bytes + (1ull << 19)) >> 20;
        sel_fmt(out, cap, "%u MB", (u32)(mb ? mb : 1));
    } else {
        sel_fmt(out, cap, "%u.%u GB", (u32)(tenths / 10), (u32)(tenths % 10));
    }
}

static void clock_text(struct sel *S, char *out, int cap)
{
    if (S->clock_24h) {
        sel_fmt(out, cap, "%02d:%02d", S->clock_h, S->clock_m);
    } else {
        int h = S->clock_h % 12;
        sel_fmt(out, cap, "%d:%02d %s", h ? h : 12, S->clock_m, S->clock_h < 12 ? "AM" : "PM");
    }
}

/* the top bar: the user (a disc with their initial) on the left, the clock on the right */
static void top_bar(struct sel *S, surface *s)
{
    if (S->user[0]) {
        const char *p = S->user;
        u32 cp = utf8_next(&p);
        char ini[8] = {0};
        if (cp >= 'a' && cp <= 'z')
            cp -= 32;
        if (cp < 0x80 && cp > ' ')
            ini[0] = (char)cp;
        float cx = MARGIN + 25, cy = TOP_Y - 10;
        u32 tint = title_tint(S->user, 0);
        draw_disc(s, cx, cy + 2, 27, 0, 0x50000000u);
        draw_disc(s, cx, cy, 25, 0, mix_rgb(tint, 0xFFFFFFFFu, 40));
        draw_disc(s, cx, cy, 25, 2.0f, 0xB0FFFFFFu);
        if (ini[0]) {
            int w = text_width(S, 24, ini);
            draw_text(S, s, 24, (int)cx - w / 2, (int)cy + 9, ini, WHITE, 0);
        }
        if (text_fits_font(S, S->user)) /* else just the disc (no boxes for a script the font lacks) */
            text_shadowed(S, s, USER_PX, MARGIN + 66, TOP_Y, S->user, SOFT, 600);
    } else {
        text_shadowed(S, s, USER_PX, MARGIN, TOP_Y, "ghostldr", FAINT, 0);
    }
    if (S->clock_valid && S->panel < 1.0f) { /* under the information panel, which slides over it */
        char ck[24];
        clock_text(S, ck, sizeof(ck));
        int w = text_width(S, CLOCK_PX, ck);
        text_shadowed(S, s, CLOCK_PX, SCREEN_W - MARGIN - w, TOP_Y + 2, ck, WHITE, 0);
    }
}

static void tiles(struct sel *S, surface *s)
{
    int first = (int)S->pos - 2, last = (int)S->pos + 9;
    int focus = (int)(S->pos + 0.5f);
    for (int pass = 0; pass < 2; pass++)
        for (int i = first; i <= last; i++) {
            if (i < 0 || i >= S->ntitles || (pass == 0) == (i == focus))
                continue;
            int x, size;
            tile_geom(S->pos, i, &x, &size);
            if (x >= SCREEN_W || x + size <= -40)
                continue;
            struct title *t = &S->titles[i];
            float d = S->pos - (float)i;
            float wgt = clampf(1 - (d < 0 ? -d : d), 0, 1); /* 1 when focused */
            draw_shadow(s, x, ROW_Y + 8, size, size, 30, 120 + (int)(50 * wgt));
            if (wgt > 0)
                draw_halo(s, x, ROW_Y, size, size, 22, (int)(64 * wgt), 0xFFFFFF);
            const surface *src = size == TILE_SMALL ? &t->icon_small : &t->icon;
            if (t->icon_state)
                draw_scale(s, x, ROW_Y, size, size, src, 256, 0);
            else
                draw_fill(s, x, ROW_Y, size, size, t->tint);
            if (wgt < 1) /* the others a little darker */
                draw_dim(s, x, x + size, ROW_Y, ROW_Y + size, (int)(46 * (1 - wgt)));
            if (wgt > 0)
                draw_frame(s, x, ROW_Y, size, size, 4, (u32)(wgt * 240) << 24 | 0xFFFFFF);
        }
}

/* the focused game's name, details and Start button */
static void details(struct sel *S, surface *s)
{
    int focus = (int)(S->pos + 0.5f);
    if (focus < 0 || focus >= S->ntitles)
        return;
    const struct title *t = &S->titles[focus];
    float d = S->pos - (float)focus;
    float vis = clampf(1 - 2.2f * (d < 0 ? -d : d), 0, 1);
    u32 a = (u32)(vis * 255);
    if (a < 8)
        return;
    int x = FOCUS_X + 4;
    text_bold(S, s, NAME_PX, x, NAME_Y, display_name(S, t), (a << 24) | 0xFFFFFF, SCREEN_W - x - MARGIN);
    char info[128], size[24];
    fmt_size(size, sizeof(size), t->size);
    const char *dot = "  \xC2\xB7  ";
    if (!(t->flags & TITLE_COMPLETE))
        sel_fmt(info, sizeof(info), "%s%s%s", t->id[0] ? t->id : "", t->id[0] ? dot : "",
                t->where[0] ? "Can't start: press Cross for why" : "Install incomplete");
    else
        sel_fmt(info, sizeof(info), "%s%s%s%s%s%s", t->id[0] ? t->id : "", t->version[0] && t->id[0] ? dot : "",
                t->version[0] ? "Version " : "", t->version, t->size ? dot : "", t->size ? size : "");
    u32 ic = t->flags & TITLE_COMPLETE ? ((a * 0xC0 / 255) << 24 | 0xFFFFFF) : ((a << 24) | (WARN & 0xFFFFFF));
    text_shadowed(S, s, INFO_PX, x, INFO_Y, info, ic, SCREEN_W - x - MARGIN);

    /* the Start button: a light pill, as on the home screen */
    int ok = (t->flags & TITLE_COMPLETE) != 0;
    int tw = text_width(S, 28, "Start"), pw = 40 + 34 + 14 + tw + 34, py = PILL_Y;
    float k = vis * (ok ? 1.0f : 0.45f);
    draw_shadow(s, x + 8, py + 6, pw - 16, PILL_H - 8, 18, (int)(90 * k));
    draw_round_rect(s, (float)x, (float)py, (float)pw, PILL_H, PILL_H / 2.0f, 0, (u32)(k * 238) << 24 | 0xF4F6F8);
    u32 ink = (u32)(k * 255) << 24 | 0x15181D;
    cross_icon(s, (float)x + 36, (float)py + PILL_H / 2.0f, 15, ink);
    draw_text(S, s, 28, x + 36 + 15 + 16, py + PILL_H / 2 + 10, "Start", ink, 0);
}

static const char *art_label(u8 src)
{
    return src == ART_TITLE ? "The game's own" : src == ART_GIVEN ? "Added at install"
         : src == ART_STORE ? "PlayStation Store" : src == ART_GENERATED ? "Generated" : "-";
}

static void panel_cell(struct sel *S, surface *s, int x, int y, int w, const char *label, const char *value, u32 vc)
{
    draw_text(S, s, 21, x, y, label, FAINT, w);
    draw_text(S, s, 28, x, y + 40, value, vc, w);
}

static void info_panel(struct sel *S, surface *s, float k)
{
    float e = 1 - (1 - k) * (1 - k); /* ease out */
    draw_dim(s, 0, SCREEN_W, 0, SCREEN_H, (int)(110 * e));
    int x0 = SCREEN_W - (int)((float)PANEL_W * e);
    draw_vgradient(s, x0, 0, PANEL_W, SCREEN_H, 0xFF171B22u, 0xFF0B0D11u);
    draw_rect_blend(s, x0, 0, 2, SCREEN_H, 0x38FFFFFFu);
    struct title *t = &S->titles[S->sel];
    int x = x0 + 64, cw = PANEL_W - 128, y = 112;
    draw_text(S, s, 22, x, y, "Information", FAINT, 0);
    y += 38;
    int is = 168;
    draw_shadow(s, x, y + 4, is, is, 18, 110);
    if (t->icon_state)
        draw_scale(s, x, y, is, is, &t->icon, 256, 0);
    else
        draw_fill(s, x, y, is, is, t->tint);
    draw_frame(s, x, y, is, is, 1, 0x40FFFFFFu);
    y += is + 66;
    char lines[3][128];
    int n = wrap_lines(S, 40, cw, display_name(S, t), lines, 3);
    for (int i = 0; i < n; i++, y += 52)
        draw_text(S, s, 40, x, y, lines[i], WHITE, cw);
    y += 4;
    draw_rect_blend(s, x, y, cw, 1, 0x40FFFFFFu);
    y += 60;
    char buf[64];
    int half = (cw - 32) / 2, x2 = x + half + 32;
    panel_cell(S, s, x, y, half, "Title ID", t->id[0] ? t->id : "Unknown", WHITE);
    panel_cell(S, s, x2, y, half, "Version", t->version[0] ? t->version : "Unknown", WHITE);
    y += 104;
    if (t->where[0]) { /* a game on a USB drive: where it is, and anything to know about it */
        panel_cell(S, s, x, y, cw, "Folder on the USB drive", t->where, WHITE);
        if (t->note[0]) { /* up to three lines */
            y += 104;
            draw_text(S, s, 21, x, y, t->flags & TITLE_COMPLETE ? "Note" : "Can't start", FAINT, cw);
            char note[3][128];
            int nl = wrap_lines(S, 26, cw, t->note, note, 3);
            for (int i = 0; i < nl; i++)
                draw_text(S, s, 26, x, y + 40 + 36 * i, note[i], t->flags & TITLE_COMPLETE ? WHITE : WARN, cw);
            y += 36 * (nl > 1 ? nl - 1 : 0);
        }
    } else {
        fmt_size(buf, sizeof(buf), t->size);
        panel_cell(S, s, x, y, half, "Size", buf, WHITE);
        panel_cell(S, s, x2, y, half, "Install", t->flags & TITLE_COMPLETE ? "Complete" : "Incomplete",
                   t->flags & TITLE_COMPLETE ? WHITE : WARN);
    }
    y += 104;
    panel_cell(S, s, x, y, half, "Art", art_label(t->icon_src), WHITE);
    if (t->prefix[0]) {
        sel_fmt(buf, sizeof(buf), "%s000 ...", t->prefix);
        panel_cell(S, s, x2, y, half, "Save data", buf, WHITE);
    }
}

void ui_draw(struct sel *S)
{
    surface *s = &S->back;
    /* the rows to redraw: everything, or the top bar and / or the row */
    int y0 = 0, y1 = SCREEN_H;
    if (S->drawn_once && !(S->changed & CH_ALL) && S->panel == 0) {
        y0 = SCREEN_H, y1 = 0;
        if (S->changed & CH_TOP)
            y0 = 0, y1 = TOP_BAND;
        if (S->changed & CH_ROW) {
            y0 = y0 < ROW_BAND0 ? y0 : ROW_BAND0;
            y1 = ROW_BAND1;
        }
        if (y0 >= y1) { /* nothing visible changed */
            S->damage_y0 = S->damage_y1 = 0;
            S->changed = 0;
            return;
        }
    }
    s->cy0 = y0, s->cy1 = y1;

    /* backdrop (crossfading after a change) */
    if (S->bg_from_valid && S->bg_fade < 1.0f) {
        float f = S->bg_fade, e = f * f * (3 - 2 * f);
        draw_mix(s, &S->bg[!S->bg_cur], &S->bg[S->bg_cur], (int)(e * 256));
    } else {
        draw_copy(s, &S->bg[S->bg_cur]);
    }
    top_bar(S, s);
    tiles(S, s);
    details(S, s);
    text_shadowed(S, s, 22, MARGIN, SCREEN_H - 66, "ghostldr", 0x66FFFFFFu, 0);
    if (S->panel > 0)
        info_panel(S, s, S->panel);
    hints(S, s, S->panel);
    if (S->toast_until_us) {
        int w = text_width(S, 26, S->toast) + 64, x = (SCREEN_W - w) / 2, y = SCREEN_H - 200;
        draw_round_rect(s, (float)x, (float)y, (float)w, 64, 32, 0, 0xD8202428u);
        draw_text(S, s, 26, x + 32, y + 42, S->toast, WHITE, 0);
    }
    if (S->launch > 0)
        draw_dim(s, 0, SCREEN_W, 0, SCREEN_H, (int)(S->launch * 256));

    s->cy0 = s->cy1 = 0;
    S->damage_y0 = y0, S->damage_y1 = y1;
    S->drawn_once = 1;
    S->changed = 0;
}
