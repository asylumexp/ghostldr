/*
 * The selector's screen (1920x1080), after the consoles' own home screens:
 * the focused game's backdrop fills the screen, dimmed towards the edges; a
 * row of rounded tiles, the focused one larger, lit and ringed; under it its
 * name, details, any warnings and a Start button; ghostldr's mark, the user
 * and the clock at the top.  All of it is drawn here, by the module's own
 * code: no image, sound or font from the console's system software is
 * copied, only the games' own art (or art stored with the install,
 * host/gfs_meta.py) is shown.
 *
 * On a PS4 (ui_set_platform) SWRR gets 2.5 GiB of memory, which most games
 * need more than (docs/ps4.md): every game's dump (TITLE_DUMP) has its tile
 * greyed with a warning sign, and Cross on one asks first, No selected.
 *
 * Frames are drawn only when something changes, and then only the rows that
 * changed (S->changed: the top bar, the row and the text under it, or
 * everything).
 */
#include "sel.h"

/* layout (pixels at 1920x1080) */
#define MARGIN 96
#define TOP_Y 80            /* baseline of the top bar */
#define TOP_BAND 136        /* rows the top bar can touch */
#define ROW_Y 232           /* top of the tiles (top-aligned, the focused one grows down) */
#define FOCUS_X 96          /* left edge of the focused tile */
#define GAP 26
#define RADIUS 22           /* the focused tile's corners (the others' in proportion) */
#define RING_GAP 6          /* the focus ring: this far outside the tile, RING_W wide */
#define RING_W 4
#define LABEL_Y (ROW_Y - 30) /* baseline of the row's label and position */
#define NAME_PX 54
#define NAME2_PX 46          /* a name too long for one line: two at this size */
#define NAME_W 1400          /* at most this wide */
#define INFO_PX 26
#define CLOCK_PX 30
#define USER_PX 26
#define HINT_PX 24
#define NAME_Y (ROW_Y + TILE_BIG + 86) /* baseline of the focused title's name */
#define CHIP_H 40
#define PILL_H 62                      /* the Start button */
#define ROW_BAND0 (ROW_Y - 64)         /* rows the row and its text can touch */
#define ROW_BAND1 990
#define PANEL_W 720
#define ASK_W 860                      /* the PS4 question */
#define ASK_H 410

/* timing (microseconds) */
#define SPRING_W 26.0f      /* focus movement: a critically damped spring, rad/s (~0.3 s to settle) */
#define REPEAT_FIRST 380000
#define REPEAT_NEXT 120000
#define BG_SETTLE 180000
#define FADE_US 420000.0f
#define PANEL_US 200000.0f
#define LAUNCH_US 360000.0f
#define ASK_US 160000.0f
#define ASK_FOCUS_US 110000.0f
#define TOAST_US 150000.0f

#define WHITE 0xFFFFFFFFu
#define SOFT 0xDCFFFFFFu
#define DIM 0xA8FFFFFFu
#define FAINT 0x80FFFFFFu
#define SHADOW 0x90000000u
#define INK 0xFF14171Cu       /* dark text on light buttons */
#define AMBER 0xFFFFB547u     /* warnings */
#define RED 0xFFFF7A6Bu       /* can't start */
#define BLUE 0xFF8EC2FFu      /* a second copy */
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

/* c with its alpha times a/255 */
static u32 fade(u32 c, u32 a) { return ((c >> 24) * a / 255) << 24 | (c & 0xFFFFFFu); }

static int ps4_marked(const struct sel *S, const struct title *t)
{
    return S->platform == SEL_PS4 && (t->flags & TITLE_DUMP);
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
    draw_text(S, s, px, x + 1, y, str, c, max_w - 1);
}

/* letter-spaced, for small capitals labels; returns the width */
static int text_tracked(struct sel *S, surface *s, int px, int x, int y, const char *str, u32 c, int track)
{
    int x0 = x;
    char one[8];
    for (const char *p = str; *p;) {
        const char *q = p;
        utf8_next(&q);
        int n = (int)(q - p);
        __builtin_memcpy(one, p, (u64)n);
        one[n] = 0;
        int w = text_width(S, px, one);
        if (s)
            draw_text(S, s, px, x, y, one, c, 0);
        x += w + track;
        p = q;
    }
    return x - x0 - (x > x0 ? track : 0);
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

/* PS4: a game's dump greyed, with a warning sign (drawn into the picture once) */
static void mark_icon(surface *s)
{
    draw_wash(s, 0x9E5A5F68u);
    float size = (float)s->w * 0.32f, cx = (float)s->w * 0.5f, cy = (float)s->h * 0.5f;
    draw_warning(s, cx, cy + size * 0.01f, size * 1.09f, 0x70000000u, 0); /* a dark edge */
    draw_warning(s, cx, cy, size, AMBER, 0xFF2A1E08u);
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
    /* the flags are final now: loading the art read the title's param.sfo */
    t->marked = (u8)ps4_marked(S, t);
    if (t->marked) {
        mark_icon(&t->icon);
        mark_icon(&t->icon_small);
    }
    draw_round_corners(&t->icon, RADIUS);
    draw_round_corners(&t->icon_small, RADIUS * TILE_SMALL / TILE_BIG);
    S->changed |= S->panel > 0 || S->ask > 0 ? CH_ALL : CH_ROW;
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

void ui_set_platform(struct sel *S, int platform) { S->platform = platform; }

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
    if (S->toast[0] >= 'a' && S->toast[0] <= 'z')
        S->toast[0] = (char)(S->toast[0] - 32);
    S->toast_until_us = now + 2600000;
    S->changed |= CH_ALL;
}

static float approach(float v, float target, float step)
{
    if (v < target)
        return v + step > target ? target : v + step;
    return v - step < target ? target : v - step;
}

/* Cross on the selected title: boot it, ask first (a game's dump on a PS4),
 * or say why it can't start */
static void choose(struct sel *S, u64 now)
{
    struct title *t = &S->titles[S->sel];
    if (!(t->flags & TITLE_COMPLETE)) {
        toast(S, now, t->note[0] ? t->note : "This game's install is incomplete: run gfs_install.py again.");
    } else if (ps4_marked(S, t)) {
        S->ask_open = 1, S->ask_yes = 0, S->ask_focus = 0;
        S->panel_open = 0;
        S->toast_until_us = 0;
        sel_log(S, "PS4: asking before title %d (%s), a game's dump, boots", S->sel, t->id);
    } else {
        S->launching = 1;
        S->panel_open = 0;
    }
}

int ui_step(struct sel *S, u64 now, u64 dt, u32 buttons, int stick_x, int *boot)
{
    u32 pressed = buttons & ~S->held;
    S->held = buttons;
    if (dt > 200000)
        dt = 200000;

    /* left / right with key repeat (d-pad or left stick), L1 / R1 jump */
    int dir = (buttons & PAD_LEFT) || stick_x < 64 ? -1 : (buttons & PAD_RIGHT) || stick_x > 192 ? 1 : 0;
    int step = 0;
    if (dir && dir != S->repeat_dir) {
        step = dir;
        S->repeat_at_us = now + REPEAT_FIRST;
    } else if (dir && now >= S->repeat_at_us) {
        step = dir;
        S->repeat_at_us = now + REPEAT_NEXT;
    }
    S->repeat_dir = dir;

    if (S->launching) {
        /* nothing more to choose */
    } else if (S->ask_open) {
        /* the question: Yes on the left, No on the right; Circle is No */
        int yes = S->ask_yes;
        if (step < 0 || (pressed & PAD_UP))
            yes = 1;
        if (step > 0 || (pressed & PAD_DOWN))
            yes = 0;
        if (yes != S->ask_yes)
            S->ask_yes = yes, S->changed |= CH_ALL;
        if (pressed & (PAD_CROSS | PAD_CIRCLE)) {
            int go = (pressed & PAD_CROSS) && S->ask_yes;
            sel_log(S, "PS4: %s for title %d (%s)%s", go ? "Yes" : "No", S->sel, S->titles[S->sel].id,
                    go ? ": booting it" : "");
            S->ask_open = 0;
            S->launching = go;
            S->changed |= CH_ALL;
        }
    } else {
        if (!S->panel_open) {
            if (pressed & PAD_L1)
                step = -5;
            if (pressed & PAD_R1)
                step = 5;
            if (step) {
                int n = clampi(S->sel + step, 0, S->ntitles - 1);
                if (n != S->sel) {
                    S->sel = n;
                    S->last_move_us = now;
                    S->changed |= CH_ROW;
                }
            }
        }
        if (pressed & PAD_CROSS)
            choose(S, now);
        else if (pressed & (PAD_OPTIONS | PAD_TRIANGLE))
            S->panel_open = !S->panel_open, S->toast_until_us = 0;
        else if ((pressed & PAD_CIRCLE) && S->panel_open)
            S->panel_open = 0;
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
    float panel_before = S->panel, fade_before = S->bg_fade, ask_before = S->ask, focus_before = S->ask_focus;
    float toast_before = S->toast_a;
    S->panel = approach(S->panel, S->panel_open ? 1.0f : 0.0f, (float)dt / PANEL_US);
    S->bg_fade = approach(S->bg_fade, 1.0f, (float)dt / FADE_US);
    S->ask = approach(S->ask, S->ask_open ? 1.0f : 0.0f, (float)dt / ASK_US);
    S->ask_focus = approach(S->ask_focus, S->ask_yes ? 1.0f : 0.0f, (float)dt / ASK_FOCUS_US);
    if (S->toast_until_us && now >= S->toast_until_us)
        S->toast_until_us = 0;
    S->toast_a = approach(S->toast_a, S->toast_until_us ? 1.0f : 0.0f, (float)dt / TOAST_US);
    if (S->launching) {
        S->launch = approach(S->launch, 1.0f, (float)dt / LAUNCH_US);
        if (S->launch >= 1.0f && boot)
            *boot = S->sel;
        S->changed |= CH_ALL;
    }
    if (S->panel != panel_before || S->bg_fade != fade_before || S->ask != ask_before ||
        S->ask_focus != focus_before || S->toast_a != toast_before)
        S->changed |= CH_ALL;

    /* the selected game's backdrop once the row has settled */
    if (S->bg_title != S->sel && S->pos == target && now - S->last_move_us >= BG_SETTLE && !S->launching)
        ui_prepare_bg(S, S->sel);
    /* a tile marked (or not) before its title's param.sfo said otherwise: again */
    for (int i = 0; i < S->ntitles; i++)
        if (S->titles[i].icon_state && S->titles[i].marked != ps4_marked(S, &S->titles[i]))
            S->titles[i].icon_state = 0;
    /* icons near the selection, one per frame */
    for (int d = 0; d <= 18; d++) {
        int i = d & 1 ? S->sel - (d + 1) / 2 : S->sel + d / 2;
        if (i >= 0 && i < S->ntitles && !S->titles[i].icon_state) {
            ui_load_icon(S, i);
            break;
        }
    }
    return S->changed != 0;
}

/* ---- drawing: controller buttons, as the consoles draw them ---- */

static void cross_glyph(surface *s, float cx, float cy, float r, u32 disc, u32 ink)
{
    draw_disc(s, cx, cy, r, 0, disc);
    float k = r * 0.40f;
    draw_line(s, cx - k, cy - k, cx + k, cy + k, r * 0.17f, ink);
    draw_line(s, cx - k, cy + k, cx + k, cy - k, r * 0.17f, ink);
}

static void circle_glyph(surface *s, float cx, float cy, float r, u32 disc, u32 ink)
{
    draw_disc(s, cx, cy, r, 0, disc);
    draw_disc(s, cx, cy, r * 0.46f, r * 0.17f, ink);
}

/* "OPTIONS" on a pill, as the button is labelled on the controller */
static int options_glyph(struct sel *S, surface *s, int x, int cy, u32 pill, u32 ink)
{
    int w = text_tracked(S, 0, 14, 0, 0, "OPTIONS", 0, 1) + 22;
    draw_rrect(s, x, cy - 15, w, 30, 15, pill);
    text_tracked(S, s, 14, x + 11, cy + 5, "OPTIONS", ink, 1);
    return w;
}

/* one hint, right-aligned at x: [button] label; returns its left edge */
static int hint(struct sel *S, surface *s, int x, int base, int button, const char *label, u32 c)
{
    int w = text_width(S, HINT_PX, label);
    x -= w;
    text_shadowed(S, s, HINT_PX, x, base, label, c, 0);
    float cy = (float)base - 8;
    if (button == PAD_OPTIONS) {
        int ow = text_tracked(S, 0, 14, 0, 0, "OPTIONS", 0, 1) + 22;
        options_glyph(S, s, x - 12 - ow, (int)cy, fade(0xF0FFFFFFu, c >> 24), INK);
        return x - 12 - ow;
    }
    if (button == PAD_CROSS)
        cross_glyph(s, (float)x - 28, cy, 16, fade(0xF0FFFFFFu, c >> 24), INK);
    else
        circle_glyph(s, (float)x - 28, cy, 16, fade(0xF0FFFFFFu, c >> 24), INK);
    return x - 44;
}

/* button hints, right-aligned at the bottom */
static void hints(struct sel *S, surface *s)
{
    int base = SCREEN_H - 64, x = SCREEN_W - MARGIN;
    if (S->ask > 0.5f) {
        x = hint(S, s, x, base, PAD_CIRCLE, "Back", SOFT) - 44;
        hint(S, s, x, base, PAD_CROSS, "Select", SOFT);
    } else if (S->panel > 0.5f) {
        u32 start_c = S->titles[S->sel].flags & TITLE_COMPLETE ? SOFT : 0x60FFFFFFu;
        x = hint(S, s, x, base, PAD_CIRCLE, "Back", SOFT) - 44;
        hint(S, s, x, base, PAD_CROSS, "Start", start_c);
    } else {
        hint(S, s, x, base, PAD_OPTIONS, "Information", SOFT);
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

/* the top bar: ghostldr's mark on the left; the user (a disc with their
 * initial and their name) and the clock on the right */
static void top_bar(struct sel *S, surface *s)
{
    draw_ghost(s, MARGIN + 16, TOP_Y - 10, 32, 0xF0FFFFFFu, 0xFF1B1F27u);
    text_shadowed(S, s, 28, MARGIN + 46, TOP_Y, "ghostldr", SOFT, 0);
    if (S->panel >= 1.0f) /* under the information panel, which slides over the rest */
        return;
    int x = SCREEN_W - MARGIN;
    if (S->clock_valid) {
        char ck[24];
        clock_text(S, ck, sizeof(ck));
        x -= text_width(S, CLOCK_PX, ck);
        text_shadowed(S, s, CLOCK_PX, x, TOP_Y, ck, WHITE, 0);
        x -= 40;
    }
    if (S->user[0]) {
        if (text_fits_font(S, S->user)) { /* else just the disc (no boxes for a script the font lacks) */
            int w = text_width(S, USER_PX, S->user);
            w = w < 420 ? w : 420;
            x -= w;
            text_shadowed(S, s, USER_PX, x, TOP_Y, S->user, SOFT, 420);
            x -= 16;
        }
        const char *p = S->user;
        u32 cp = utf8_next(&p);
        char ini[8] = {0};
        if (cp >= 'a' && cp <= 'z')
            cp -= 32;
        if (cp < 0x80 && cp > ' ')
            ini[0] = (char)cp;
        float cx = (float)x - 23, cy = TOP_Y - 10;
        u32 tint = title_tint(S->user, 0);
        draw_disc(s, cx, cy + 2, 25, 0, 0x50000000u);
        draw_disc(s, cx, cy, 23, 0, mix_rgb(tint, 0xFFFFFFFFu, 40));
        draw_disc(s, cx, cy, 23, 1.6f, 0x90FFFFFFu);
        if (ini[0]) {
            int w = text_width(S, 22, ini);
            draw_text(S, s, 22, (int)cx - w / 2, (int)cy + 8, ini, WHITE, 0);
        } else { /* a name in a script the font lacks: a figure */
            draw_disc(s, cx, cy - 5, 6.5f, 0, 0xE6FFFFFFu);
            draw_disc(s, cx, cy + 13, 10.5f, 0, 0xE6FFFFFFu);
        }
    }
}

/* the row's label and where the selection is in it, above the tiles */
static void row_label(struct sel *S, surface *s)
{
    const struct title *t = &S->titles[S->sel];
    const char *label = t->where[0] ? "USB DRIVE" : t->prefix[0] ? "INSTALLED" : "GAMES";
    text_tracked(S, s, 19, FOCUS_X + 2, LABEL_Y, label, DIM, 3);
    if (S->ntitles > 1) {
        char pos[24];
        sel_fmt(pos, sizeof(pos), "%d / %d", S->sel + 1, S->ntitles);
        int w = text_width(S, 22, pos);
        text_shadowed(S, s, 22, SCREEN_W - MARGIN - w, LABEL_Y, pos, DIM, 0);
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
            int r = RADIUS * size / TILE_BIG;
            draw_shadow(s, x + size / 32, ROW_Y + 10 + size / 32, size - size / 16, size - size / 32, r,
                        34 + (int)(10 * wgt), 60 + (int)(80 * wgt));
            if (wgt > 0)
                draw_halo(s, x - RING_GAP, ROW_Y - RING_GAP, size + 2 * RING_GAP, size + 2 * RING_GAP,
                          r + RING_GAP, 22, (int)(70 * wgt), 0xFFFFFF);
            const surface *src = size == TILE_SMALL ? &t->icon_small : &t->icon;
            int bright = 256 - (int)(64 * (1 - wgt)); /* the others a little darker */
            if (t->icon_state)
                draw_scale(s, x, ROW_Y, size, size, src, 256, bright, 0);
            else
                draw_rrect(s, x, ROW_Y, size, size, r, mix_rgb(t->tint, 0xFF000000u, 256 - bright));
            if (wgt > 0)
                draw_rring(s, x - RING_GAP - RING_W, ROW_Y - RING_GAP - RING_W, size + 2 * (RING_GAP + RING_W),
                           size + 2 * (RING_GAP + RING_W), r + RING_GAP + RING_W, RING_W,
                           (u32)(wgt * 245) << 24 | 0xFFFFFF);
        }
}

/* a title's note (the extension's, docs/usb.md) as a sentence: capitalised,
 * without "a second copy: " where a chip already says so */
static const char *note_text(const struct title *t, char *out, int cap)
{
    const char *n = t->note;
    const char *copy = "a second copy: ";
    int k = 0;
    while (copy[k] && n[k] == copy[k])
        k++;
    if (!copy[k] && (t->flags & TITLE_COPY))
        n += k;
    str_copy(out, n, cap);
    if (out[0] >= 'a' && out[0] <= 'z')
        out[0] = (char)(out[0] - 32);
    return out;
}

/* a small rounded label: [sign] text, at (x, y) top left; returns its width */
static int chip(struct sel *S, surface *s, int x, int y, u32 fg, int sign, const char *text, u32 a)
{
    int tw = text_width(S, 21, text), w = tw + 36 + (sign ? 30 : 0);
    draw_rrect(s, x, y, w, CHIP_H, CHIP_H / 2, fade((mix_rgb(fg, 0xFF000000u, 200) & 0xFFFFFFu) | 0xD0000000u, a));
    draw_rring(s, x, y, w, CHIP_H, CHIP_H / 2, 1, fade(fg & 0x60FFFFFFu, a));
    int tx = x + 18;
    if (sign) {
        draw_warning(s, (float)tx + 11, (float)y + CHIP_H / 2.0f, 23, fade(fg, a), fade(0xFF2A1E08u, a));
        tx += 30;
    }
    draw_text(S, s, 21, tx, y + 27, text, fade(fg, a), 0);
    return w;
}

/* what to say about a title beside its name: chips, then one line of text */
static int statuses(struct sel *S, surface *s, const struct title *t, int x, int y, int max_w, u32 a)
{
    const char *say = 0;
    char note[96];
    int x0 = x;
    if (!(t->flags & TITLE_COMPLETE)) {
        x += chip(S, s, x, y, RED, 1, t->where[0] ? "Can't start" : "Install incomplete", a) + 12;
        say = t->where[0] ? note_text(t, note, sizeof(note)) : "Run gfs_install.py again to finish it";
    }
    if (t->flags & TITLE_COPY) {
        x += chip(S, s, x, y, BLUE, 0, "Second copy", a) + 12;
        say = say ? say : note_text(t, note, sizeof(note));
    }
    if (ps4_marked(S, t)) {
        x += chip(S, s, x, y, AMBER, 1, "May not run on PS4", a) + 12;
        say = say ? say : "Most game dumps need more memory than a PS4 gives SWRR";
    }
    if (x == x0)
        return 0;
    if (say && say[0] && x + 8 < x0 + max_w)
        text_shadowed(S, s, 23, x + 6, y + 28, say, fade(SOFT, a), x0 + max_w - x - 6);
    return 1;
}

/* the start button: a light pill (greyed when the title can't start) */
static void start_button(struct sel *S, surface *s, int x, int y, int ok, float k)
{
    int tw = text_width(S, 28, "Start"), pw = 32 + 32 + 16 + tw + 40;
    float a = k * (ok ? 1.0f : 0.42f);
    draw_shadow(s, x + 6, y + 8, pw - 12, PILL_H - 8, (PILL_H - 8) / 2, 20, (int)(110 * k));
    draw_rrect(s, x, y, pw, PILL_H, PILL_H / 2, (u32)(a * 242) << 24 | 0xF4F6F8);
    u32 ink = (u32)(a * 255) << 24 | 0x15181D;
    cross_glyph(s, (float)x + 32 + 16, (float)y + PILL_H / 2.0f, 16, ink, (u32)(a * 255) << 24 | 0xF4F6F8);
    draw_text(S, s, 28, x + 32 + 32 + 16, y + PILL_H / 2 + 10, "Start", ink, 0);
}

/* the focused game's name, details, warnings and Start button */
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
    int x = FOCUS_X + 4 + (int)((1 - vis) * 28), max_w = SCREEN_W - x - MARGIN, y = NAME_Y;
    int name_w = max_w < NAME_W ? max_w : NAME_W;

    /* the name: one line, or two a little smaller */
    const char *name = display_name(S, t);
    if (text_width(S, NAME_PX, name) <= name_w) {
        text_bold(S, s, NAME_PX, x, y, name, (a << 24) | 0xFFFFFF, name_w);
    } else {
        char lines[2][128];
        int n = wrap_lines(S, NAME2_PX, name_w, name, lines, 2);
        for (int i = 0; i < n; i++, y += i < n ? 56 : 0)
            text_bold(S, s, NAME2_PX, x, y, lines[i], (a << 24) | 0xFFFFFF, name_w);
    }

    /* ID, version, size, and where it is */
    char info[300], size[24];
    const char *dot = "  \xC2\xB7  ";
    int n = 0;
    info[0] = 0;
    if (t->id[0])
        n += sel_fmt(info + n, (int)sizeof(info) - n, "%s", t->id);
    if (t->version[0])
        n += sel_fmt(info + n, (int)sizeof(info) - n, "%sVersion %s", n ? dot : "", t->version);
    if (t->size) {
        fmt_size(size, sizeof(size), t->size);
        n += sel_fmt(info + n, (int)sizeof(info) - n, "%s%s", n ? dot : "", size);
    }
    if (t->where[0])
        sel_fmt(info + n, (int)sizeof(info) - n, "%sUSB drive: %s", n ? dot : "", t->where);
    else if (t->prefix[0])
        sel_fmt(info + n, (int)sizeof(info) - n, "%sInstalled", n ? dot : "");
    y += 50;
    text_shadowed(S, s, INFO_PX, x, y, info, fade(SOFT, a), max_w);

    y += 24;
    if (statuses(S, s, t, x, y, max_w, a))
        y += CHIP_H;
    start_button(S, s, x, y + 30, (t->flags & TITLE_COMPLETE) != 0, vis);
}

static const char *art_label(u8 src)
{
    return src == ART_TITLE ? "The game's own" : src == ART_GIVEN ? "Added at install"
         : src == ART_STORE ? "PlayStation Store" : src == ART_GENERATED ? "Generated" : "-";
}

/* the information panel: a sheet sliding in from the right */
static void info_panel(struct sel *S, surface *s, float k)
{
    float e = 1 - (1 - k) * (1 - k); /* ease out */
    draw_dim(s, 0, SCREEN_W, 0, SCREEN_H, (int)(110 * e));
    int x0 = SCREEN_W - (int)((float)PANEL_W * e);
    draw_vgradient(s, x0, 0, PANEL_W, SCREEN_H, 0xFF181C24u, 0xFF0B0D11u);
    draw_rect_blend(s, x0, 0, 1, SCREEN_H, 0x40FFFFFFu);
    draw_shadow(s, x0, -40, PANEL_W + 40, SCREEN_H + 80, 0, 40, (int)(120 * e));
    struct title *t = &S->titles[S->sel];
    int x = x0 + 64, cw = PANEL_W - 128, y = 104;
    text_tracked(S, s, 19, x, y, "INFORMATION", DIM, 3);
    y += 32;

    /* the icon, the name beside it */
    int is = 152;
    draw_shadow(s, x, y + 6, is, is, 12, 22, 130);
    if (t->icon_state)
        draw_scale(s, x, y, is, is, &t->icon, 256, 256, 0);
    else
        draw_rrect(s, x, y, is, is, 12, t->tint);
    char lines[3][128];
    int nx = x + is + 28, nw = cw - is - 28;
    int n = wrap_lines(S, 34, nw, display_name(S, t), lines, 3);
    for (int i = 0; i < n; i++)
        draw_text(S, s, 34, nx, y + 40 + 44 * i, lines[i], WHITE, nw);
    if (t->id[0] && n < 3)
        draw_text(S, s, 22, nx, y + 40 + 44 * n, t->id, DIM, nw);
    y += is + 30;

    /* warnings */
    int cx = x;
    if (!(t->flags & TITLE_COMPLETE))
        cx += chip(S, s, cx, y, RED, 1, t->where[0] ? "Can't start" : "Install incomplete", 255) + 12;
    if (t->flags & TITLE_COPY)
        cx += chip(S, s, cx, y, BLUE, 0, "Second copy", 255) + 12;
    if (ps4_marked(S, t))
        cx += chip(S, s, cx, y, AMBER, 1, "May not run on PS4", 255) + 12;
    if (cx != x)
        y += CHIP_H + 26;
    draw_rect_blend(s, x, y, cw, 1, 0x30FFFFFFu);
    y += 50;

    /* the facts, two to a row (a folder takes a whole one) */
    struct cell {
        char label[24], value[256];
        u32 c;
        int wide;
    } cells[8];
    int nc = 0;
#define CELL(L, V, C, W) (str_copy(cells[nc].label, L, 24), str_copy(cells[nc].value, V, 256), cells[nc].c = C, \
                          cells[nc++].wide = W)
    CELL("TITLE ID", t->id[0] ? t->id : "Unknown", WHITE, 0);
    CELL("VERSION", t->version[0] ? t->version : "Unknown", WHITE, 0);
    if (t->where[0]) { /* a game on a USB drive: where it is, and anything to know about it */
        CELL("FOLDER ON THE USB DRIVE", t->where, WHITE, 1);
    } else {
        char size[24];
        fmt_size(size, sizeof(size), t->size);
        CELL("SIZE", size, WHITE, 0);
        CELL("INSTALL", t->flags & TITLE_COMPLETE ? "Complete" : "Incomplete", t->flags & TITLE_COMPLETE ? WHITE : RED, 0);
    }
    CELL("ART", art_label(t->icon_src), WHITE, 0);
    if (t->prefix[0]) {
        char buf[40];
        sel_fmt(buf, sizeof(buf), "%s000 ...", t->prefix);
        CELL("SAVE DATA", buf, WHITE, 0);
    }
    if (S->platform == SEL_PS4)
        CELL("KIND", t->flags & TITLE_DUMP ? "A game's dump" : "Homebrew", t->flags & TITLE_DUMP ? AMBER : WHITE, 0);
#undef CELL
    int half = (cw - 32) / 2;
    for (int i = 0, col = 0; i < nc; i++) {
        if (cells[i].wide && col)
            col = 0, y += 96;
        int cx2 = x + col * (half + 32), w = cells[i].wide ? cw : half;
        text_tracked(S, s, 18, cx2, y, cells[i].label, FAINT, 2);
        draw_text(S, s, 28, cx2, y + 40, cells[i].value, cells[i].c, w);
        if (cells[i].wide || col)
            col = 0, y += 96;
        else
            col = 1;
        if (i == nc - 1 && col)
            y += 96;
    }
    if (t->note[0]) { /* up to three lines */
        text_tracked(S, s, 18, x, y, t->flags & TITLE_COMPLETE ? "NOTE" : "WHY IT CAN'T START", FAINT, 2);
        char note[3][128], text[96];
        int nl = wrap_lines(S, 25, cw, note_text(t, text, sizeof(text)), note, 3);
        for (int i = 0; i < nl; i++)
            draw_text(S, s, 25, x, y + 40 + 36 * i, note[i], t->flags & TITLE_COMPLETE ? SOFT : RED, cw);
    }
}

/* PS4: the question before a game's dump boots */
static void ask_dialog(struct sel *S, surface *s, float k)
{
    float e = 1 - (1 - k) * (1 - k);
    u32 a = (u32)(e * 255);
    draw_dim(s, 0, SCREEN_W, 0, SCREEN_H, (int)(150 * e));
    int x = (SCREEN_W - ASK_W) / 2, y = (SCREEN_H - ASK_H) / 2 + (int)((1 - e) * 28);
    draw_shadow(s, x, y + 14, ASK_W, ASK_H, 28, 60, (int)(170 * e));
    draw_rrect(s, x, y, ASK_W, ASK_H, 28, fade(0xF6202530u, a));
    draw_rring(s, x, y, ASK_W, ASK_H, 28, 1, fade(0x30FFFFFFu, a));
    draw_warning(s, (float)SCREEN_W / 2, (float)y + 78, 68, fade(AMBER, a), fade(0xFF2A1E08u, a));

    const char *msg = "Game dumps largely will not work on PS4 due to RAM limitations";
    char lines[2][128];
    int n = wrap_lines(S, 30, ASK_W - 120, msg, lines, 2), ty = y + 172;
    for (int i = 0; i < n; i++, ty += 42) {
        int w = text_width(S, 30, lines[i]);
        draw_text(S, s, 30, (SCREEN_W - w) / 2, ty, lines[i], fade(WHITE, a), ASK_W - 120);
    }
    const char *q = "Continue anyways?";
    int qw = text_width(S, 26, q);
    draw_text(S, s, 26, (SCREEN_W - qw) / 2, ty + 10, q, fade(DIM, a), 0);

    /* [Yes] [No]: the focus (a light button with a ring) slides between them */
    int bw = 228, bh = 64, gap = 28, by = y + ASK_H - bh - 44;
    int bx[2] = {SCREEN_W / 2 - gap / 2 - bw, SCREEN_W / 2 + gap / 2};
    float f = S->ask_focus, fe = f * f * (3 - 2 * f); /* 1: Yes */
    int fx = bx[1] + (int)((float)(bx[0] - bx[1]) * fe);
    for (int i = 0; i < 2; i++)
        draw_rrect(s, bx[i], by, bw, bh, bh / 2, fade(0x26FFFFFFu, a));
    draw_rrect(s, fx, by, bw, bh, bh / 2, fade(0xFFF4F6F8u, a));
    draw_rring(s, fx - 6 - 3, by - 6 - 3, bw + 18, bh + 18, bh / 2 + 9, 3, fade(0xE6FFFFFFu, a));
    for (int i = 0; i < 2; i++) {
        const char *l = i ? "No" : "Yes";
        float lit = i ? 1 - fe : fe; /* how much of the light button is under it */
        u32 c = mix_rgb(WHITE, INK, (int)(lit * 256));
        int w = text_width(S, 28, l);
        draw_text(S, s, 28, bx[i] + (bw - w) / 2, by + bh / 2 + 10, l, fade(c, a), 0);
    }
}

/* a message at the bottom (why a title can't start), fading in and out */
static void toast_card(struct sel *S, surface *s, float k)
{
    float e = k * k * (3 - 2 * k);
    u32 a = (u32)(e * 255);
    int tw = text_width(S, 25, S->toast), w = tw + 64 + 40, h = 66;
    if (w > SCREEN_W - 2 * MARGIN)
        w = SCREEN_W - 2 * MARGIN;
    int x = (SCREEN_W - w) / 2, y = SCREEN_H - 210 + (int)((1 - e) * 16);
    draw_shadow(s, x, y + 6, w, h, h / 2, 26, (int)(140 * e));
    draw_rrect(s, x, y, w, h, h / 2, fade(0xF0262B33u, a));
    draw_warning(s, (float)x + 44, (float)y + h / 2.0f, 30, fade(RED, a), fade(0xFF2A1E08u, a));
    draw_text(S, s, 25, x + 72, y + 42, S->toast, fade(WHITE, a), w - 72 - 32);
}

void ui_draw(struct sel *S)
{
    surface *s = &S->back;
    /* the rows to redraw: everything, or the top bar and / or the row */
    int y0 = 0, y1 = SCREEN_H;
    if (S->drawn_once && !(S->changed & CH_ALL) && S->panel == 0 && S->ask == 0 && S->toast_a == 0) {
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
    row_label(S, s);
    tiles(S, s);
    details(S, s);
    if (S->panel > 0)
        info_panel(S, s, S->panel);
    if (S->toast_a > 0)
        toast_card(S, s, S->toast_a);
    if (S->ask > 0)
        ask_dialog(S, s, S->ask);
    hints(S, s);
    if (S->launch > 0)
        draw_dim(s, 0, SCREEN_W, 0, SCREEN_H, (int)(S->launch * 256));

    s->cy0 = s->cy1 = 0;
    S->damage_y0 = y0, S->damage_y1 = y1;
    S->drawn_once = 1;
    S->changed = 0;
}
