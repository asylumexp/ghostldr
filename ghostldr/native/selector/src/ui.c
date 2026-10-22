/*
 * The selector's screen: a row of square game tiles over the selected game's
 * pic1.png, laid out after the PS4 home screen's content row (1920x1080).
 * All drawing is done here, by the module's own code: no image, sound or
 * font from the console's system software is copied, only the game's own
 * art (icon0.png, pic1.png) is decoded at run time.
 */
#include "sel.h"

/* layout (pixels at 1920x1080) */
#define ROW_Y 300           /* top of the tiles (top-aligned, the focused one grows down) */
#define FOCUS_X 128         /* left edge of the focused tile */
#define GAP 22
#define NAME_PX 44
#define INFO_PX 27
#define CLOCK_PX 34
#define HINT_PX 27
#define PANEL_W 640
#define MARGIN 96

/* timing (microseconds) */
#define SCROLL_TAU 70000.0f
#define REPEAT_FIRST 380000
#define REPEAT_NEXT 120000
#define BG_SETTLE 160000
#define FADE_US 320000.0f
#define PANEL_US 180000.0f
#define LAUNCH_US 360000.0f

#define WHITE 0xFFFFFFFFu
#define SOFT 0xB8FFFFFFu
#define FAINT 0x80FFFFFFu
#define SHADOW 0x90000000u

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

/* ---- backgrounds ---- */

/* the home screen's tone: darker at the top (clock) and towards the bottom */
static void dim_profile(surface *s)
{
    for (int y = 0; y < s->h; y++) {
        int a;
        if (y < 150)
            a = 112 - y * 40 / 150; /* 112 -> 72 */
        else if (y < 560)
            a = 72;
        else
            a = 72 + (y - 560) * 124 / (s->h - 560); /* 72 -> 196 */
        draw_dim(s, 0, s->w, y, y + 1, a);
    }
}

void ui_prepare_bg(struct sel *S, int t)
{
    surface *dst = &S->bg[!S->bg_cur];
    int ok = 0;
    if (t >= 0 && S->titles[t].bg_state != 2) {
        u64 m = sel_mark(S);
        surface img;
        if (sel_load_png(S, t, "sce_sys/pic1.png", &img) == 0 && img.w >= 64 && img.h >= 36) {
            draw_cover(dst, &img);
            dim_profile(dst);
            ok = 1;
        }
        sel_release(S, m);
        S->titles[t].bg_state = ok ? 1 : 2;
    }
    if (!ok)
        draw_copy(dst, &S->bg_default);
    S->bg_cur = !S->bg_cur;
    S->bg_from_valid = 1;
    S->bg_fade = 0;
    S->bg_title = t;
    S->dirty = 1;
}

/* ---- tiles ---- */

/* the first letters of the first two significant words */
static int minor_word(const char *w, int n)
{
    static const char words[][4] = {"of", "the", "a", "an", "and", "to", "in", "on", "for", "at", "by"};
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        int k = 0;
        while (k < n && words[i][k] && (w[k] | 32) == words[i][k])
            k++;
        if (k == n && !words[i][k])
            return 1;
    }
    return 0;
}

static void initials(struct sel *S, const struct title *t, char out[8])
{
    const char *s = display_name(S, t);
    char first = 0, last = 0;
    int nwords = 0;
    while (*s) {
        while (*s && !((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9')))
            s++;
        const char *w = s;
        while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '\'')
            s++;
        int n = (int)(s - w);
        if (!n || (nwords && minor_word(w, n)))
            continue;
        char c = *w >= 'a' && *w <= 'z' ? (char)(*w - 32) : *w;
        if (!nwords++)
            first = c;
        else if (!last)
            last = c;
    }
    int n = 0;
    if (first)
        out[n++] = first;
    if (last)
        out[n++] = last;
    out[n] = 0;
}

static void fallback_icon(struct sel *S, struct title *t, surface *s)
{
    draw_fallback_tile(s, t->tint);
    char ini[8];
    initials(S, t, ini);
    int px = s->w * 2 / 5;
    if (px > 200)
        px = 200;
    int w = text_width(S, px, ini);
    int base = s->h / 2 + px * 36 / 100;
    draw_text(S, s, px, (s->w - w) / 2, base, ini, 0xE6FFFFFFu, 0);
}

void ui_load_icon(struct sel *S, int i)
{
    struct title *t = &S->titles[i];
    if (t->icon_state)
        return;
    u64 m = sel_mark(S);
    surface img;
    if (sel_load_png(S, i, "sce_sys/icon0.png", &img) == 0 && img.w >= 16 && img.h >= 16) {
        draw_resample(&t->icon, &img);
        draw_resample(&t->icon_small, &img);
        t->icon_state = 1;
    } else {
        fallback_icon(S, t, &t->icon);
        fallback_icon(S, t, &t->icon_small);
        t->icon_state = 2;
    }
    sel_release(S, m);
    S->dirty = 1;
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
    float f = pos - (float)k;
    float x0, s0, x1, s1;
    slot(k, i, &x0, &s0);
    slot(k + 1, i, &x1, &s1);
    *x = (int)(x0 + (x1 - x0) * f + 0.5f);
    *size = (int)(s0 + (s1 - s0) * f + 0.5f);
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
    surface *all[4] = {&S->back, &S->bg[0], &S->bg[1], &S->bg_default};
    for (int i = 0; i < 4; i++) {
        all[i]->w = SCREEN_W, all[i]->h = SCREEN_H, all[i]->stride = SCREEN_W;
        all[i]->px = sel_alloc(S, (u64)SCREEN_W * SCREEN_H * 4);
        if (!all[i]->px)
            return -1;
    }
    for (int i = 0; i < n; i++) {
        struct title *t = &titles[i];
        t->tint = title_tint(t->id, t->name);
        t->icon = (surface){sel_alloc(S, (u64)TILE_BIG * TILE_BIG * 4), TILE_BIG, TILE_BIG, TILE_BIG};
        t->icon_small = (surface){sel_alloc(S, (u64)TILE_SMALL * TILE_SMALL * 4), TILE_SMALL, TILE_SMALL, TILE_SMALL};
        if (!t->icon.px || !t->icon_small.px)
            return -1;
    }
    draw_default_bg(&S->bg_default);
    dim_profile(&S->bg_default);
    draw_copy(&S->bg[0], &S->bg_default);
    S->dirty = 1;
    return 0;
}

void ui_set_clock(struct sel *S, int h, int m, int h24)
{
    if (S->clock_valid && S->clock_h == h && S->clock_m == m && S->clock_24h == h24)
        return;
    S->clock_h = h, S->clock_m = m, S->clock_24h = h24, S->clock_valid = 1;
    S->dirty = 1;
}

static void toast(struct sel *S, u64 now, const char *msg)
{
    str_copy(S->toast, msg, sizeof(S->toast));
    S->toast_until_us = now + 2600000;
    S->dirty = 1;
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
    int was_dirty = S->dirty;
    S->dirty = 0;
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
        }
    }

    if (!S->launching) {
        struct title *t = &S->titles[S->sel];
        if (pressed & PAD_CROSS) {
            if (t->flags & TITLE_COMPLETE) {
                S->launching = 1;
                S->panel_open = 0;
            } else {
                toast(S, now, "This game's install is incomplete: run gfs_install.py again.");
            }
        } else if (pressed & (PAD_OPTIONS | PAD_TRIANGLE)) {
            S->panel_open = !S->panel_open;
        } else if ((pressed & PAD_CIRCLE) && S->panel_open) {
            S->panel_open = 0;
        }
    }

    /* animation */
    float target = (float)S->sel, before = S->pos;
    float k = (float)dt / ((float)dt + SCROLL_TAU);
    S->pos += (target - S->pos) * k;
    if ((S->pos - target) * (S->pos - target) < 0.00002f)
        S->pos = target;
    float panel_before = S->panel;
    S->panel = approach(S->panel, S->panel_open ? 1.0f : 0.0f, (float)dt / PANEL_US);
    float fade_before = S->bg_fade;
    S->bg_fade = approach(S->bg_fade, 1.0f, (float)dt / FADE_US);
    if (S->launching) {
        S->launch = approach(S->launch, 1.0f, (float)dt / LAUNCH_US);
        if (S->launch >= 1.0f && boot)
            *boot = S->sel;
    }
    int moving = S->pos != before || S->panel != panel_before || S->bg_fade != fade_before || S->launching;

    /* the selected game's background once the row has settled */
    if (S->bg_title != S->sel && S->pos == target && now - S->last_move_us >= BG_SETTLE && !S->launching) {
        ui_prepare_bg(S, S->sel);
        moving = 1;
    }
    /* icons near the selection, one per frame */
    for (int d = 0; d <= 18; d++) {
        int i = d & 1 ? S->sel - (d + 1) / 2 : S->sel + d / 2;
        if (i >= 0 && i < S->ntitles && !S->titles[i].icon_state) {
            ui_load_icon(S, i);
            break;
        }
    }
    int toast_live = S->toast_until_us && now < S->toast_until_us;
    if (S->toast_until_us && !toast_live) {
        S->toast_until_us = 0;
        moving = 1;
    }
    return was_dirty || S->dirty || moving || pressed != 0;
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
static void hints(struct sel *S, surface *s, int panel)
{
    int base = SCREEN_H - 70, x = SCREEN_W - MARGIN;
    u32 start_c = S->titles[S->sel].flags & TITLE_COMPLETE ? SOFT : 0x60FFFFFFu;
    const char *l1 = panel ? "Back" : "Information", *l2 = "Start";
    int w1 = text_width(S, HINT_PX, l1), w2 = text_width(S, HINT_PX, l2);
    int ow = panel ? 34 : text_width(S, 16, "OPTIONS") + 20;
    x -= w1;
    text_shadowed(S, s, HINT_PX, x, base, l1, SOFT, 0);
    x -= 12 + ow;
    if (panel)
        circle_icon(s, (float)x + 17, (float)base - 10, 16, SOFT);
    else
        options_icon(S, s, x, base, SOFT);
    x -= 44 + w2;
    text_shadowed(S, s, HINT_PX, x, base, l2, start_c, 0);
    cross_icon(s, (float)x - 29, (float)base - 10, 16, start_c);
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

/* greedy word wrap into at most max_lines; returns the y after the text */
static int wrap(struct sel *S, surface *s, int px, int x, int y, int w, const char *str, u32 c, int max_lines,
                int line_h)
{
    char line[160];
    const char *p = str;
    for (int ln = 0; ln < max_lines && *p; ln++) {
        int n = 0, last_space = -1;
        const char *q = p;
        while (*q && n < (int)sizeof(line) - 1) {
            line[n] = *q;
            line[n + 1] = 0;
            if (*q == ' ')
                last_space = n;
            if (text_width(S, px, line) > w && n > 0) {
                if (last_space > 0 && ln < max_lines - 1)
                    n = last_space;
                break;
            }
            n++, q++;
        }
        line[n] = 0;
        int last = ln == max_lines - 1;
        draw_text(S, s, px, x, y, last ? p : line, c, last ? w : 0);
        if (last)
            break;
        p += n;
        while (*p == ' ')
            p++;
        y += line_h;
    }
    return y + line_h;
}

static void panel_row(struct sel *S, surface *s, int x, int *y, const char *label, const char *value, u32 vc)
{
    draw_text(S, s, 22, x, *y, label, FAINT, 0);
    draw_text(S, s, 30, x, *y + 40, value, vc, PANEL_W - 2 * 56);
    *y += 100;
}

static void info_panel(struct sel *S, surface *s, float k)
{
    float e = 1 - (1 - k) * (1 - k); /* ease out */
    draw_dim(s, 0, SCREEN_W, 0, SCREEN_H, (int)(120 * e));
    int x0 = SCREEN_W - (int)((float)PANEL_W * e);
    draw_rect_blend(s, x0, 0, PANEL_W, SCREEN_H, 0xEE15171Bu); /* panel body: dark, nearly opaque */
    draw_rect_blend(s, x0, 0, 2, SCREEN_H, 0x30FFFFFFu);
    const struct title *t = &S->titles[S->sel];
    int x = x0 + 56, y = 150;
    draw_text(S, s, 24, x, y - 50, "Information", FAINT, 0);
    y = wrap(S, s, 38, x, y + 10, PANEL_W - 112, display_name(S, t), WHITE, 3, 50);
    draw_rect_blend(s, x, y - 18, PANEL_W - 112, 1, 0x40FFFFFFu);
    y += 30;
    char buf[64];
    panel_row(S, s, x, &y, "Title ID", t->id[0] ? t->id : "Unknown", WHITE);
    panel_row(S, s, x, &y, "Version", t->version[0] ? t->version : "Unknown", WHITE);
    fmt_size(buf, sizeof(buf), t->size);
    panel_row(S, s, x, &y, "Size", buf, WHITE);
    panel_row(S, s, x, &y, "Install status", t->flags & TITLE_COMPLETE ? "Complete" : "Incomplete",
              t->flags & TITLE_COMPLETE ? WHITE : 0xFFFFB347u);
    if (t->prefix[0]) {
        sel_fmt(buf, sizeof(buf), "%s000 ...", t->prefix);
        panel_row(S, s, x, &y, "Save containers", buf, WHITE);
    }
}

void ui_draw(struct sel *S)
{
    surface *s = &S->back;
    /* background (crossfading after a change) */
    if (S->bg_from_valid && S->bg_fade < 1.0f)
        draw_mix(s, &S->bg[!S->bg_cur], &S->bg[S->bg_cur], (int)(S->bg_fade * 256));
    else
        draw_copy(s, &S->bg[S->bg_cur]);

    /* status: clock at the top right */
    if (S->clock_valid && S->panel < 1.0f) {
        char ck[24];
        clock_text(S, ck, sizeof(ck));
        int w = text_width(S, CLOCK_PX, ck);
        u32 a = (u32)((1.0f - S->panel) * 255);
        text_shadowed(S, s, CLOCK_PX, SCREEN_W - MARGIN - w, 92, ck, a << 24 | 0xFFFFFF, 0);
    }
    text_shadowed(S, s, 26, MARGIN, 90, "ghostldr", FAINT, 0);

    /* the row */
    int first = (int)S->pos - 2, last = (int)S->pos + 9;
    int focus = (int)(S->pos + 0.5f);
    for (int pass = 0; pass < 2; pass++)
        for (int i = first; i <= last; i++) {
            if (i < 0 || i >= S->ntitles || (pass == 0) == (i == focus))
                continue;
            int x, size;
            tile_geom(S->pos, i, &x, &size);
            if (x >= SCREEN_W || x + size <= 0)
                continue;
            struct title *t = &S->titles[i];
            draw_shadow(s, x, ROW_Y + 4, size, size, 22, 110);
            const surface *src = size == TILE_SMALL ? &t->icon_small : &t->icon;
            if (t->icon_state)
                draw_scale(s, x, ROW_Y, size, size, src, 256, 0);
            else
                draw_fill(s, x, ROW_Y, size, size, t->tint);
            float wgt = 1 - (S->pos - (float)i < 0 ? (float)i - S->pos : S->pos - (float)i);
            if (wgt > 0)
                draw_frame(s, x, ROW_Y, size, size, 3, (u32)(wgt * 230) << 24 | 0xFFFFFF);
        }

    /* the focused game's name and details */
    if (focus >= 0 && focus < S->ntitles) {
        const struct title *t = &S->titles[focus];
        float d = S->pos - (float)focus;
        float vis = 1 - 2 * (d < 0 ? -d : d);
        u32 a = (u32)(clampf(vis, 0, 1) * 255);
        if (a > 8) {
            int base = ROW_Y + TILE_BIG + 70;
            text_shadowed(S, s, NAME_PX, FOCUS_X, base, display_name(S, t), (a << 24) | 0xFFFFFF, 1500);
            char info[96], size[24];
            fmt_size(size, sizeof(size), t->size);
            if (!(t->flags & TITLE_COMPLETE))
                str_copy(info, "Install incomplete", sizeof(info));
            else if (t->version[0])
                sel_fmt(info, sizeof(info), "%s  \xC2\xB7  Version %s%s%s", t->id[0] ? t->id : "-", t->version,
                        t->size ? "  \xC2\xB7  " : "", t->size ? size : "");
            else
                sel_fmt(info, sizeof(info), "%s%s%s", t->id, t->size ? "  \xC2\xB7  " : "", t->size ? size : "");
            u32 ic = t->flags & TITLE_COMPLETE ? ((a * 0xB8 / 255) << 24 | 0xFFFFFF) : ((a << 24) | 0xFFB347);
            text_shadowed(S, s, INFO_PX, FOCUS_X, base + 48, info, ic, 1500);
        }
    }

    if (S->panel > 0)
        info_panel(S, s, S->panel);
    hints(S, s, S->panel_open);

    if (S->toast_until_us) {
        int w = text_width(S, 26, S->toast) + 64, x = (SCREEN_W - w) / 2, y = SCREEN_H - 200;
        draw_round_rect(s, (float)x, (float)y, (float)w, 64, 32, 0, 0xD0202428u);
        draw_text(S, s, 26, x + 32, y + 42, S->toast, WHITE, 0);
    }
    if (S->launch > 0)
        draw_dim(s, 0, SCREEN_W, 0, SCREEN_H, (int)(S->launch * 256));
}
