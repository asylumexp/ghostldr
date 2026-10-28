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
 * A save tool (TITLE_SAVE_TOOL: Apollo Save Tool) asks whose saves to open,
 * listing the console's users (those not signed in greyed, with why), and
 * the platform then runs the save scan, whose progress shows as a bar with
 * the time left when it is long (ui_scan_*).
 *
 * The row ends with a Settings tile (a gear; Square from anywhere in the
 * row): setui.c draws that screen.  With no title installed the row is that
 * tile alone, and the text under it says what to do.
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
#define USERS_W 980                    /* a save tool's questions: whose saves, then the scan */
#define USERS_HEAD 184                 /* the title's icon and the question above the users */
#define USERS_ROW 84
#define USERS_ROWS 7                   /* users shown at once (more scroll) */
#define SCAN_H 312

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
#define USERS_US 160000.0f
#define SCAN_SHOW_AFTER 1500000u /* a scan shows its bar once it has taken this long, */
#define SCAN_SHOW_LONG 2500000u  /* or as soon as it looks like taking this long in all */
#define SCAN_HOLD 450000u        /* a shown bar stays full this long when the scan ends */

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

/* the Settings tile: the last in the row (the only one with no titles) */
static int on_gear(const struct sel *S) { return S->sel >= S->ntitles; }

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
    S->sel = clampi(initial, 0, n > 0 ? n - 1 : 0);
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
    for (int i = 0; i < n; i++)
        if (ui_title_alloc(S, &titles[i]) != 0)
            return -1;
    S->rows = sel_alloc(S, sizeof(struct srow) * SET_MAX_ROWS);
    if (!S->rows)
        return -1;
    prepare(S, -1, &S->bg[0]);
    S->changed = CH_ALL;
    return 0;
}

/* a title's tiles (both sizes) and its colour */
int ui_title_alloc(struct sel *S, struct title *t)
{
    t->tint = title_tint(t->id, t->name);
    t->icon = (surface){sel_alloc(S, (u64)TILE_BIG * TILE_BIG * 4), TILE_BIG, TILE_BIG, TILE_BIG, 0, 0};
    t->icon_small = (surface){sel_alloc(S, (u64)TILE_SMALL * TILE_SMALL * 4), TILE_SMALL, TILE_SMALL, TILE_SMALL, 0, 0};
    return t->icon.px && t->icon_small.px ? 0 : -1;
}

/* the titles again (an install finished): the selection stays on its title,
 * or on the Settings tile it was on */
void ui_set_titles(struct sel *S, struct title *titles, int n)
{
    char id[16];
    int gear = on_gear(S);
    str_copy(id, gear ? "" : S->titles[S->sel].id, sizeof(id));
    S->titles = titles, S->ntitles = n;
    S->sel = n;
    for (int i = 0; !gear && i < n; i++)
        if (id[0] && str_same(titles[i].id, id))
            S->sel = i;
    S->pos = (float)S->sel, S->vel = 0;
    S->bg_title = -2; /* the backdrop again */
    S->changed |= CH_ALL;
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

void ui_toast(struct sel *S, u64 now, const char *msg);
static void toast(struct sel *S, u64 now, const char *msg) { ui_toast(S, now, msg); }
void ui_toast_ok(struct sel *S, u64 now, const char *msg)
{
    ui_toast(S, now, msg);
    S->toast_ok = 1;
}
void ui_toast(struct sel *S, u64 now, const char *msg)
{
    S->toast_ok = 0;
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

/* ---- a save tool: whose saves to open, then the save scan ---- */

/* a user's name as shown: their own, else "User N" (a user who isn't signed
 * in has no name to read, or one in a script the font lacks) */
static const char *user_label(struct sel *S, int i, char *buf, int cap)
{
    const struct sel_user *u = &S->users[i];
    if (u->name[0] && text_fits_font(S, u->name))
        return u->name;
    sel_fmt(buf, cap, "User %d", i + 1);
    return buf;
}

/* the user to focus: keep if still there, else the user running SWRR, else
 * the first signed in */
static int user_default(const struct sel *S, s32 keep)
{
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < S->nusers; i++) {
            const struct sel_user *u = &S->users[i];
            if (pass == 0 ? u->id == keep && keep != -1 : pass == 1 ? u->signed_in && u->current : u->signed_in)
                return i;
        }
    return 0;
}

int ui_set_users(struct sel *S, const struct sel_user *users, int n)
{
    n = clampi(n, 0, SEL_MAX_USERS);
    int same = n == S->nusers;
    for (int i = 0; same && i < n; i++) {
        const struct sel_user *a = &users[i], *b = &S->users[i];
        same = a->id == b->id && a->signed_in == b->signed_in && a->current == b->current;
        for (int k = 0; same && k < (int)sizeof(a->name) && (a->name[k] || b->name[k]); k++)
            same = a->name[k] == b->name[k];
    }
    if (same)
        return 0;
    s32 keep = S->nusers ? S->users[S->user_focus].id : -1;
    for (int i = 0; i < n; i++)
        S->users[i] = users[i];
    S->nusers = n;
    if (!S->user_chosen)
        S->user_focus = user_default(S, keep);
    S->changed |= CH_ALL;
    return 1;
}

static void open_users(struct sel *S)
{
    struct title *t = &S->titles[S->sel];
    S->panel_open = 0, S->toast_until_us = 0;
    if (!S->nusers) { /* nobody to ask about: the title opens the saves of the user running SWRR */
        sel_log(S, "save tool: no users known: title %d (%s) starts for the user running SWRR", S->sel, t->id);
        S->launching = 1;
        return;
    }
    S->users_open = 1, S->user_chosen = 0;
    S->user_focus = user_default(S, -1);
    if (S->users_a == 0)
        S->user_y = (float)S->user_focus;
    S->changed |= CH_ALL;
    sel_log(S, "save tool: asking whose saves to open for title %d (%s): %d user(s)", S->sel, t->id, S->nusers);
}

void ui_ask_user(struct sel *S, int fixed)
{
    S->users_fixed = fixed;
    open_users(S);
}

void ui_scan_start(struct sel *S, u64 now)
{
    S->scan_req = 0, S->scanning = 1, S->scan_shown = 0, S->scan_cancel = 0;
    S->scan_done = S->scan_total = 0, S->scan_per = 0, S->scan_left = 0, S->scan_hold = 0;
    S->scan_t0 = S->scan_prev = now;
}

int ui_scan_progress(struct sel *S, u64 now, u32 done, u32 total)
{
    if (done > total)
        done = total;
    if (!done) {
        S->scan_prev = now; /* the candidates are known: titles are timed from here */
    } else if (done > S->scan_done) {
        /* the measured rate, refined as it goes: the recent pace (per title
         * since the last report, smoothed), steadied by the average */
        u64 d = (now - S->scan_prev) / (done - S->scan_done), avg = (now - S->scan_t0) / done;
        S->scan_per = S->scan_per ? (S->scan_per * 3 + d) / 4 : d;
        S->scan_left = (done < 4 ? avg : (S->scan_per + avg) / 2) * (total - done);
        S->scan_prev = now;
    }
    S->scan_done = done, S->scan_total = total;
    u64 spent = now - S->scan_t0;
    if (!S->scan_shown && total && done < total &&
        (spent >= SCAN_SHOW_AFTER || (done && spent + S->scan_left >= SCAN_SHOW_LONG))) {
        S->scan_shown = 1;
        sel_log(S, "save tool: a long scan (%u of %u after %u ms, about %u s left): its progress is shown", done, total,
                (u32)(spent / 1000), (u32)(S->scan_left / 1000000));
    }
    S->changed |= CH_ALL;
    return S->scan_cancel;
}

void ui_scan_end(struct sel *S, u64 now, int result)
{
    S->scanning = 0;
    sel_log(S, "save tool: scan %s after %u of %u titles, %u ms: %d", S->scan_cancel ? "cancelled" : "done",
            S->scan_done, S->scan_total, (u32)((now - S->scan_t0) / 1000), result);
    if (S->scan_cancel) { /* back to the question; the title stays (it is mounted now) */
        S->scan_shown = 0, S->user_chosen = 0, S->users_fixed = 1, S->users_open = 1;
    } else if (S->scan_shown) {
        S->scan_done = S->scan_total, S->scan_left = 0;
        S->scan_hold = now + SCAN_HOLD;
    } else {
        S->launching = 1;
    }
    S->changed |= CH_ALL;
}

/* the title starts, or a save tool asks whose saves first */
static void start(struct sel *S)
{
    if (S->titles[S->sel].flags & TITLE_SAVE_TOOL)
        open_users(S);
    else
        S->launching = 1;
}

/* Cross on the selected title: boot it, ask first (a game's dump on a PS4),
 * or say why it can't start */
static void choose(struct sel *S, u64 now)
{
    if (on_gear(S)) {
        S->panel_open = 0;
        set_open(S, now);
        return;
    }
    struct title *t = &S->titles[S->sel];
    if (!(t->flags & TITLE_COMPLETE)) {
        toast(S, now, t->note[0] ? t->note : "This game's install is incomplete: run gfs_install.py again.");
    } else if (ps4_marked(S, t)) {
        S->ask_open = 1, S->ask_yes = 0, S->ask_focus = 0, S->ask_kind = ASK_PS4;
        S->panel_open = 0;
        S->toast_until_us = 0;
        sel_log(S, "PS4: asking before title %d (%s), a game's dump, boots", S->sel, t->id);
    } else {
        S->panel_open = 0;
        start(S);
    }
}

int ui_step(struct sel *S, u64 now, u64 dt, u32 buttons, int stick_x, int stick_y, int *boot)
{
    u32 pressed = buttons & ~S->held;
    S->held = buttons;
    if (dt > 200000)
        dt = 200000;

    /* left / right with key repeat (d-pad or left stick), L1 / R1 jump; up /
     * down in the list of users */
    int users = S->users_open && !S->user_chosen;
    int dir = users || (S->set_open && !S->ask_open) ? ((buttons & PAD_UP) || stick_y < 64 ? -1 : (buttons & PAD_DOWN) || stick_y > 192 ? 1 : 0)
                    : (buttons & PAD_LEFT) || stick_x < 64 ? -1 : (buttons & PAD_RIGHT) || stick_x > 192 ? 1 : 0;
    int step = 0;
    if (dir && dir != S->repeat_dir) {
        step = dir;
        S->repeat_at_us = now + REPEAT_FIRST;
    } else if (dir && now >= S->repeat_at_us) {
        step = dir;
        S->repeat_at_us = now + REPEAT_NEXT;
    }
    S->repeat_dir = dir;

    if (S->launching || S->scan_hold) {
        /* nothing more to choose */
    } else if (S->scanning || S->user_chosen) {
        /* the save scan: Circle cancels it */
        if ((pressed & PAD_CIRCLE) && S->scanning && !S->scan_cancel) {
            S->scan_cancel = 1;
            S->changed |= CH_ALL;
            sel_log(S, "save tool: Circle: cancelling the scan at %u of %u", S->scan_done, S->scan_total);
        }
    } else if (users) {
        /* whose saves: every user can have the focus, only those signed in be chosen */
        int f = clampi(S->user_focus + step, 0, S->nusers - 1);
        if (f != S->user_focus)
            S->user_focus = f, S->changed |= CH_ALL;
        const struct sel_user *u = &S->users[S->user_focus];
        if ((pressed & PAD_CROSS) && u->signed_in) {
            S->user_chosen = 1, S->scan_req = 1, S->toast_until_us = 0;
            S->changed |= CH_ALL;
            sel_log(S, "save tool: user %x chosen for title %d (%s)", (u32)u->id, S->sel, S->titles[S->sel].id);
        } else if (pressed & PAD_CROSS) {
            char msg[96], label[72];
            const char *who = user_label(S, S->user_focus, label, sizeof(label));
            sel_fmt(msg, sizeof(msg), "%s isn\xE2\x80\x99t signed in: sign them in with another controller first",
                    str_len(who) <= 24 ? who : "This user");
            toast(S, now, msg);
        } else if ((pressed & PAD_CIRCLE) && !S->users_fixed) {
            S->users_open = 0, S->toast_until_us = 0;
            S->changed |= CH_ALL;
        }
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
            S->ask_open = 0;
            if (S->ask_kind == ASK_CLOSE) {
                sel_log(S, "settings: Close ghostldr: %s", go ? "Yes: back to Luac0re's loader" : "No");
                if (go)
                    S->quit = 1, S->launching = 1;
            } else {
                sel_log(S, "PS4: %s for title %d (%s)%s", go ? "Yes" : "No", S->sel, S->titles[S->sel].id,
                        go ? ": booting it" : "");
                if (go)
                    start(S);
            }
            S->changed |= CH_ALL;
        }
    } else if (S->set_open) {
        set_step(S, now, dt, pressed, buttons, step);
    } else {
        if (!S->panel_open) {
            if (pressed & PAD_L1)
                step = -5;
            if (pressed & PAD_R1)
                step = 5;
            if (step) {
                int n = clampi(S->sel + step, 0, S->ntitles); /* the last: the Settings tile */
                if (n != S->sel) {
                    int gear = on_gear(S);
                    S->sel = n;
                    S->last_move_us = now;
                    S->changed |= gear != on_gear(S) ? CH_ALL : CH_ROW; /* the hints change too */
                }
            }
        }
        if (pressed & PAD_CROSS)
            choose(S, now);
        else if (pressed & PAD_SETTINGS)
            S->panel_open = 0, set_open(S, now);
        else if ((pressed & (PAD_OPTIONS | PAD_TRIANGLE)) && !on_gear(S))
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
    float toast_before = S->toast_a, users_before = S->users_a, user_y_before = S->user_y, scan_before = S->scan_a;
    /* one card at a time: the question fades out before the scan's card fades in, and back */
    S->users_a = approach(S->users_a, S->users_open && !S->scan_shown && S->scan_a == 0 ? 1.0f : 0.0f,
                          (float)dt / USERS_US);
    S->scan_a = approach(S->scan_a, S->scan_shown && S->users_a == 0 ? 1.0f : 0.0f, (float)dt / USERS_US);
    S->user_y = approach(S->user_y, (float)S->user_focus, (float)dt / ASK_FOCUS_US);
    if (S->scan_hold && now >= S->scan_hold)
        S->scan_hold = 0, S->launching = 1;
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
            *boot = S->quit ? UI_QUIT : S->sel;
        S->changed |= CH_ALL;
    }
    float set_before = S->set_a;
    S->set_a = approach(S->set_a, S->set_open ? 1.0f : 0.0f, (float)dt / PANEL_US);
    if (S->set_a != set_before || set_busy(S))
        S->changed |= CH_ALL;
    if (S->panel != panel_before || S->bg_fade != fade_before || S->ask != ask_before ||
        S->ask_focus != focus_before || S->toast_a != toast_before || S->users_a != users_before ||
        S->user_y != user_y_before || S->scan_a != scan_before)
        S->changed |= CH_ALL;

    /* nothing more is read once a save tool's user is chosen: the title is
     * mounted for its scan */
    if (S->user_chosen || S->scanning)
        return S->changed != 0;
    /* the selected game's backdrop once the row has settled */
    int bg_want = on_gear(S) ? -1 : S->sel;
    if (S->bg_title != bg_want && S->pos == target && now - S->last_move_us >= BG_SETTLE && !S->launching)
        ui_prepare_bg(S, bg_want);
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

static void square_glyph(surface *s, float cx, float cy, float r, u32 disc, u32 ink)
{
    draw_disc(s, cx, cy, r, 0, disc);
    float k = r * 0.38f, w = r * 0.15f;
    draw_line(s, cx - k, cy - k, cx + k, cy - k, w, ink);
    draw_line(s, cx + k, cy - k, cx + k, cy + k, w, ink);
    draw_line(s, cx + k, cy + k, cx - k, cy + k, w, ink);
    draw_line(s, cx - k, cy + k, cx - k, cy - k, w, ink);
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
    else if (button == PAD_SQUARE)
        square_glyph(s, (float)x - 28, cy, 16, fade(0xF0FFFFFFu, c >> 24), INK);
    else
        circle_glyph(s, (float)x - 28, cy, 16, fade(0xF0FFFFFFu, c >> 24), INK);
    return x - 44;
}
int ui_hint(struct sel *S, surface *s, int x, int base, int button, const char *label, u32 c)
{
    return hint(S, s, x, base, button, label, c);
}

/* button hints, right-aligned at the bottom */
static void hints(struct sel *S, surface *s)
{
    int base = SCREEN_H - 64, x = SCREEN_W - MARGIN;
    if (S->scan_a > 0.5f) {
        if (S->scanning && !S->scan_cancel) /* the pad is read between titles: a held Circle is seen */
            hint(S, s, x, base, PAD_CIRCLE, "Hold to cancel", SOFT);
    } else if (S->users_a > 0.5f) {
        if (!S->user_chosen) {
            if (!S->users_fixed)
                x = hint(S, s, x, base, PAD_CIRCLE, "Back", SOFT) - 44;
            hint(S, s, x, base, PAD_CROSS, "Select", S->users[S->user_focus].signed_in ? SOFT : 0x60FFFFFFu);
        }
    } else if (S->ask > 0.5f) {
        x = hint(S, s, x, base, PAD_CIRCLE, "Back", SOFT) - 44;
        hint(S, s, x, base, PAD_CROSS, "Select", SOFT);
    } else if (S->set_a > 0.5f) {
        set_hints(S, s);
    } else if (S->panel > 0.5f) {
        u32 start_c = S->titles[S->sel].flags & TITLE_COMPLETE ? SOFT : 0x60FFFFFFu;
        x = hint(S, s, x, base, PAD_CIRCLE, "Back", SOFT) - 44;
        hint(S, s, x, base, PAD_CROSS, "Start", start_c);
    } else if (on_gear(S)) {
        hint(S, s, x, base, PAD_CROSS, "Settings", SOFT);
    } else {
        x = hint(S, s, x, base, PAD_SQUARE, "Settings", SOFT) - 44;
        hint(S, s, x, base, PAD_OPTIONS, "Information", SOFT);
    }
}

void ui_size(char *out, int cap, u64 bytes);
static void fmt_size(char *out, int cap, u64 bytes) { ui_size(out, cap, bytes); }
void ui_size(char *out, int cap, u64 bytes)
{
    if (!bytes) {
        str_copy(out, "Unknown", cap);
        return;
    }
    u64 tenths = (bytes * 10 + (1ull << 29)) >> 30;
    if (tenths >= 10240) { /* a drive */
        u64 t = (bytes * 10 + (1ull << 39)) >> 40;
        sel_fmt(out, cap, "%u.%u TB", (u32)(t / 10), (u32)(t % 10));
    } else if (tenths < 10) {
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

/* a user's disc: their initial on a colour of their name's, or a figure for
 * a name in a script the font lacks; grey: not signed in */
static void avatar(struct sel *S, surface *s, float cx, float cy, float r, const char *name, u32 a, int grey)
{
    const char *p = name;
    u32 cp = utf8_next(&p);
    char ini[8] = {0};
    if (cp >= 'a' && cp <= 'z')
        cp -= 32;
    if (cp < 0x80 && cp > ' ')
        ini[0] = (char)cp;
    u32 tint = grey ? 0xFF4A4F58u : title_tint(name, 0);
    draw_disc(s, cx, cy + 2 * r / 23, r + 2 * r / 23, 0, fade(0x50000000u, a));
    draw_disc(s, cx, cy, r, 0, fade(mix_rgb(tint, 0xFFFFFFFFu, 40), a));
    draw_disc(s, cx, cy, r, 1.6f, fade(grey ? 0x50FFFFFFu : 0x90FFFFFFu, a));
    u32 ink = fade(grey ? 0xB0FFFFFFu : WHITE, a);
    if (ini[0]) {
        int px = (int)(r * 22 / 23), w = text_width(S, px, ini);
        draw_text(S, s, px, (int)cx - w / 2, (int)cy + (int)(r * 8 / 23), ini, ink, 0);
    } else {
        draw_disc(s, cx, cy - r * 5 / 23, r * 6.5f / 23, 0, fade(0xE6FFFFFFu, a));
        draw_disc(s, cx, cy + r * 13 / 23, r * 10.5f / 23, 0, fade(0xE6FFFFFFu, a));
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
        avatar(S, s, (float)x - 23, TOP_Y - 10, 23, S->user, 255, 0);
    }
}

/* the row's label and where the selection is in it, above the tiles */
static void row_label(struct sel *S, surface *s)
{
    const struct title *t = on_gear(S) ? 0 : &S->titles[S->sel];
    const char *label = !t ? "SETTINGS" : t->where[0] ? "USB DRIVE" : t->prefix[0] ? "INSTALLED" : "GAMES";
    text_tracked(S, s, 19, FOCUS_X + 2, LABEL_Y, label, DIM, 3);
    if (S->ntitles > 1 && t) {
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
            if (i < 0 || i > S->ntitles || (pass == 0) == (i == focus))
                continue;
            int x, size;
            tile_geom(S->pos, i, &x, &size);
            if (x >= SCREEN_W || x + size <= -40)
                continue;
            float d = S->pos - (float)i;
            float wgt = clampf(1 - (d < 0 ? -d : d), 0, 1); /* 1 when focused */
            int r = RADIUS * size / TILE_BIG;
            draw_shadow(s, x + size / 32, ROW_Y + 10 + size / 32, size - size / 16, size - size / 32, r,
                        34 + (int)(10 * wgt), 60 + (int)(80 * wgt));
            if (wgt > 0)
                draw_halo(s, x - RING_GAP, ROW_Y - RING_GAP, size + 2 * RING_GAP, size + 2 * RING_GAP,
                          r + RING_GAP, 22, (int)(70 * wgt), 0xFFFFFF);
            int bright = 256 - (int)(64 * (1 - wgt)); /* the others a little darker */
            if (i == S->ntitles) { /* Settings: a gear on slate */
                draw_rrect(s, x, ROW_Y, size, size, r, mix_rgb(0xFF39414Fu, 0xFF000000u, 256 - bright));
                draw_vgradient(s, x, ROW_Y, size, size / 2, 0x18FFFFFFu, 0x00FFFFFFu);
                ui_gear(s, (float)x + (float)size / 2, (float)ROW_Y + (float)size * 0.46f, (float)size * 0.2f,
                        mix_rgb(0xFFF2F4F7u, 0xFF000000u, 256 - bright), mix_rgb(0xFF39414Fu, 0xFF000000u, 256 - bright));
                if (size > TILE_SMALL + 40)
                    ui_text_tracked(S, s, 19, x + (size - ui_text_tracked(S, 0, 19, 0, 0, "SETTINGS", 0, 3)) / 2,
                                    ROW_Y + size * 82 / 100, "SETTINGS", fade(0xC8FFFFFFu, (u32)(wgt * 255)), 3);
            } else {
            struct title *t = &S->titles[i];
            const surface *src = size == TILE_SMALL ? &t->icon_small : &t->icon;
            if (t->icon_state)
                draw_scale(s, x, ROW_Y, size, size, src, 256, bright, 0);
            else
                draw_rrect(s, x, ROW_Y, size, size, r, mix_rgb(t->tint, 0xFF000000u, 256 - bright));
            }
            if (wgt > 0)
                draw_rring(s, x - RING_GAP - RING_W, ROW_Y - RING_GAP - RING_W, size + 2 * (RING_GAP + RING_W),
                           size + 2 * (RING_GAP + RING_W), r + RING_GAP + RING_W, RING_W,
                           (u32)(wgt * 245) << 24 | 0xFFFFFF);
        }
}

/* A second copy's note from the extension ("a second copy: --title boots
 * the one in X", run.py's words; X with "USB N/" on drive N > 1): where the
 * other copy is, as the selector names places ("USB drive 2: folder"; on
 * drive 1 "USB drive 1: " when the title's own place names a drive).  0
 * when the note isn't one. */
int title_copy_of(const struct title *t, char *out, int cap)
{
    const char *k = "boots the one in ", *x = 0;
    for (const char *c = t->note; *c && !x; c++) {
        int i = 0;
        while (k[i] && c[i] == k[i])
            i++;
        if (!k[i])
            x = c + i;
    }
    if (!x || !(t->flags & TITLE_COPY))
        return 0;
    const char *w = t->where, *d = "USB drive ";
    int named = 1;
    for (int i = 0; d[i] && named; i++)
        named = w[i] == d[i];
    if (x[0] == 'U' && x[1] == 'S' && x[2] == 'B' && x[3] == ' ' && x[4] >= '1' && x[4] <= '9' && x[5] == '/')
        sel_fmt(out, cap, "USB drive %c: %s", x[4], x + 6);
    else
        sel_fmt(out, cap, "%s%s", named ? "USB drive 1: " : "", x);
    return 1;
}

/* a title's note (the extension's, docs/usb.md) as a sentence: capitalised,
 * without "a second copy: " where a chip already says so, and a second
 * copy's said for users (no run.py flag) */
static const char *note_text(const struct title *t, char *out, int cap)
{
    char other[112];
    if (title_copy_of(t, other, sizeof(other))) {
        sel_fmt(out, cap, "The same game is also in %s", other);
        return out;
    }
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
static void start_button(struct sel *S, surface *s, int x, int y, int ok, float k, const char *label)
{
    int tw = text_width(S, 28, label), pw = 32 + 32 + 16 + tw + 40;
    float a = k * (ok ? 1.0f : 0.42f);
    draw_shadow(s, x + 6, y + 8, pw - 12, PILL_H - 8, (PILL_H - 8) / 2, 20, (int)(110 * k));
    draw_rrect(s, x, y, pw, PILL_H, PILL_H / 2, (u32)(a * 242) << 24 | 0xF4F6F8);
    u32 ink = (u32)(a * 255) << 24 | 0x15181D;
    cross_glyph(s, (float)x + 32 + 16, (float)y + PILL_H / 2.0f, 16, ink, (u32)(a * 255) << 24 | 0xF4F6F8);
    draw_text(S, s, 28, x + 32 + 32 + 16, y + PILL_H / 2 + 10, label, ink, 0);
}

/* the focused game's name, details, warnings and Start button */
/* under the Settings tile: what is there, or with no games what to do */
static void gear_details(struct sel *S, surface *s, int x, int y, int max_w, float vis, u32 a)
{
    if (S->ntitles) {
        text_bold(S, s, NAME_PX, x, y, "Settings", (a << 24) | 0xFFFFFF, max_w);
        text_shadowed(S, s, INFO_PX, x, y + 50, "Games and their problems  \xC2\xB7  USB drives  \xC2\xB7  Install over "
                      "network  \xC2\xB7  Logs  \xC2\xB7  About", fade(SOFT, a), max_w);
        start_button(S, s, x, y + 104, 1, vis, "Open");
        return;
    }
    text_bold(S, s, NAME_PX, x, y, "No games yet", (a << 24) | 0xFFFFFF, max_w);
    int ly = y + 50;
    for (int i = 0; i < 2; i++) { /* no table of pointers: the module has no relocations */
        const char *say = i ? "Or install one into saved data from a phone or computer: Settings \xE2\x80\xBA Network "
                              "\xE2\x80\xBA Install over network."
                            : "Copy a game\xE2\x80\x99s folder (the one with eboot.bin) to a USB drive formatted exFAT, "
                              "plug it in and start ghostldr again.";
        char lines[2][128];
        int n = wrap_lines(S, INFO_PX, max_w < 1400 ? max_w : 1400, say, lines, 2);
        for (int k = 0; k < n; k++, ly += 38)
            text_shadowed(S, s, INFO_PX, x, ly, lines[k], fade(SOFT, a), max_w);
        ly += 8;
    }
    start_button(S, s, x, ly + 4, 1, vis, "Settings");
}

static void details(struct sel *S, surface *s)
{
    int focus = (int)(S->pos + 0.5f);
    if (focus < 0 || focus > S->ntitles)
        return;
    float d = S->pos - (float)focus;
    float vis = clampf(1 - 2.2f * (d < 0 ? -d : d), 0, 1);
    u32 a = (u32)(vis * 255);
    if (a < 8)
        return;
    int x = FOCUS_X + 4 + (int)((1 - vis) * 28), max_w = SCREEN_W - x - MARGIN, y = NAME_Y;
    int name_w = max_w < NAME_W ? max_w : NAME_W;
    if (focus == S->ntitles) {
        gear_details(S, s, x, y, name_w, vis, a);
        return;
    }
    const struct title *t = &S->titles[focus];

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
        sel_fmt(info + n, (int)sizeof(info) - n, "%s%s%s", n ? dot : "", /* "USB drive 2: ..." from main.c as is */
                t->where[0] == 'U' && t->where[1] == 'S' && t->where[2] == 'B' ? "" : "USB drive: ", t->where);
    else if (t->prefix[0])
        sel_fmt(info + n, (int)sizeof(info) - n, "%sInstalled", n ? dot : "");
    y += 50;
    text_shadowed(S, s, INFO_PX, x, y, info, fade(SOFT, a), max_w);

    y += 24;
    if (statuses(S, s, t, x, y, max_w, a))
        y += CHIP_H;
    start_button(S, s, x, y + 30, (t->flags & TITLE_COMPLETE) != 0, vis, "Start");
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
    int close = S->ask_kind == ASK_CLOSE;
    if (close)
        ui_gear(s, (float)SCREEN_W / 2, (float)y + 80, 30, fade(0xFFF2F4F7u, a), fade(0xFF202530u, a));
    else
        draw_warning(s, (float)SCREEN_W / 2, (float)y + 78, 68, fade(AMBER, a), fade(0xFF2A1E08u, a));

    const char *msg = close ? "Close ghostldr? SWRR goes back to Luac0re\xE2\x80\x99s loader without starting a game."
                            : "Game dumps largely will not work on PS4 due to RAM limitations";
    char lines[2][128];
    int n = wrap_lines(S, 30, ASK_W - 120, msg, lines, 2), ty = y + 172;
    for (int i = 0; i < n; i++, ty += 42) {
        int w = text_width(S, 30, lines[i]);
        draw_text(S, s, 30, (SCREEN_W - w) / 2, ty, lines[i], fade(WHITE, a), ASK_W - 120);
    }
    const char *q = close ? "Send ghostldr.lua again to come back." : "Continue anyways?";
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

/* a save tool's cards: centred, under the dim ui_draw puts over the screen */
void ui_card(surface *s, int x, int y, int w, int h, float e);
static void card(surface *s, int x, int y, int w, int h, float e) { ui_card(s, x, y, w, h, e); }
void ui_card(surface *s, int x, int y, int w, int h, float e)
{
    u32 a = (u32)(e * 255);
    draw_shadow(s, x, y + 14, w, h, 28, 60, (int)(170 * e));
    draw_rrect(s, x, y, w, h, 28, fade(0xF6202530u, a));
    draw_rring(s, x, y, w, h, 28, 1, fade(0x30FFFFFFu, a));
}

/* the title's icon at the card's top left, the heading beside it in up to
 * two lines (and a line under a one-line heading); returns the heading's
 * line count */
static int card_head(struct sel *S, surface *s, int x, int y, const char *head, const char *sub, u32 a)
{
    const struct title *t = &S->titles[S->sel];
    int is = 104, ix = x + 48, iy = y + 44;
    draw_shadow(s, ix, iy + 5, is, is, 14, 20, (int)(a / 2));
    if (t->icon_state)
        draw_scale(s, ix, iy, is, is, &t->icon, (int)(a + (a >> 7)), 256, 0);
    else
        draw_rrect(s, ix, iy, is, is, 14, fade(t->tint, a));
    char lines[2][128];
    int tx = ix + is + 32, tw = x + USERS_W - 48 - tx;
    int n = wrap_lines(S, 32, tw, head, lines, 2);
    int ty = iy + (n == 1 && sub ? 42 : n == 1 ? 64 : 42);
    for (int i = 0; i < n; i++, ty += 44)
        text_bold(S, s, 32, tx, ty, lines[i], fade(WHITE, a), tw);
    if (n == 1 && sub)
        draw_text(S, s, 24, tx, ty - 2, sub, fade(DIM, a), tw);
    return n;
}

/* a save tool: whose saves to open.  Every user is listed; those not signed
 * in are greyed with why (the save service opens only a signed-in user's
 * saves), and the focus, a light row, slides between them. */
static void users_dialog(struct sel *S, surface *s, float k)
{
    float e = 1 - (1 - k) * (1 - k);
    u32 a = (u32)(e * 255);
    int n = S->nusers, vis = n < USERS_ROWS ? n : USERS_ROWS;
    int h = USERS_HEAD + vis * USERS_ROW + 36;
    int x = (SCREEN_W - USERS_W) / 2, y = (SCREEN_H - h) / 2 - 24 + (int)((1 - e) * 28);
    card(s, x, y, USERS_W, h, e);
    char q[200];
    sel_fmt(q, sizeof(q), "Which user\xE2\x80\x99s saves to open %s for?", display_name(S, &S->titles[S->sel]));
    card_head(S, s, x, y, q, S->user_chosen ? "Looking for saves\xE2\x80\xA6" : 0, a);
    draw_rect_blend(s, x + 48, y + USERS_HEAD - 20, USERS_W - 96, 1, fade(0x30FFFFFFu, a));

    int first = n > vis ? clampi(S->user_focus - vis / 2, 0, n - vis) : 0, ry0 = y + USERS_HEAD - 6;
    float fy = clampf(S->user_y - (float)first, -0.6f, (float)vis - 0.4f);
    int rx = x + 28, rw = USERS_W - 56, can = S->users[S->user_focus].signed_in;
    /* light on a user who can be chosen; only a faint row on one who can't */
    draw_rrect(s, rx, ry0 + (int)(fy * USERS_ROW) + 4, rw, USERS_ROW - 8, 22,
               fade(!can ? 0x2EFFFFFFu : S->user_chosen ? 0xFFDCE3EEu : 0xFFF4F6F8u, a));
    for (int r = 0; r < vis; r++) {
        int i = first + r, ry = ry0 + r * USERS_ROW;
        const struct sel_user *u = &S->users[i];
        float d = S->user_y - (float)i;
        int lit = can ? (int)(clampf(1 - (d < 0 ? -d : d), 0, 1) * 256) : 0; /* how much of the light row is under it */
        char label[72];
        const char *name = user_label(S, i, label, sizeof(label));
        avatar(S, s, (float)rx + 52, (float)ry + USERS_ROW / 2.0f, 27, u->name[0] ? u->name : name, a, !u->signed_in);
        int tx = rx + 100, tw = rw - 100 - 28;
        u32 c = mix_rgb(u->signed_in ? WHITE : 0xFF8C929Bu, INK, lit);
        draw_text(S, s, 29, tx, ry + 37, name, fade(c, a), tw);
        const char *why = !u->signed_in ? "Not signed in \xC2\xB7 sign in with another controller to open their saves"
                        : u->current ? "Signed in \xC2\xB7 playing now" : "Signed in";
        u32 sc = mix_rgb(u->signed_in ? 0xFFB4BAC4u : 0xFF7C828Bu, 0xFF4A515Cu, lit);
        draw_text(S, s, 21, tx, ry + 66, why, fade(sc, a), tw);
    }
    /* more above or below */
    if (first > 0)
        draw_text(S, s, 22, x + USERS_W - 64, ry0 + 4, "\xE2\x86\x91", fade(DIM, a), 0);
    if (first + vis < n)
        draw_text(S, s, 22, x + USERS_W - 64, ry0 + vis * USERS_ROW - 8, "\xE2\x86\x93", fade(DIM, a), 0);
}

/* "About 2 min 10 s left", from the estimate */
static void time_left(struct sel *S, char *out, int cap)
{
    u64 sec = (S->scan_left + 500000) / 1000000;
    if (!S->scanning)
        str_copy(out, S->scan_cancel ? "Cancelled" : "Done", cap);
    else if (S->scan_cancel)
        str_copy(out, "Cancelling\xE2\x80\xA6", cap);
    else if (!S->scan_left)
        str_copy(out, "Working out the time left\xE2\x80\xA6", cap);
    else if (sec < 10)
        str_copy(out, "A few seconds left", cap);
    else if (sec < 60)
        sel_fmt(out, cap, "About %u seconds left", (u32)((sec + 2) / 5 * 5));
    else if (sec < 600 && (sec + 5) / 10 * 10 % 60)
        sel_fmt(out, cap, "About %u min %u s left", (u32)((sec + 5) / 10 * 10 / 60), (u32)((sec + 5) / 10 * 10 % 60));
    else
        sel_fmt(out, cap, "About %u min left", (u32)((sec + 30) / 60));
}

/* a save tool: the save scan, when it is long */
static void scan_card(struct sel *S, surface *s, float k)
{
    float e = 1 - (1 - k) * (1 - k);
    u32 a = (u32)(e * 255);
    int x = (SCREEN_W - USERS_W) / 2, y = (SCREEN_H - SCAN_H) / 2 - 24 + (int)((1 - e) * 28);
    card(s, x, y, USERS_W, SCAN_H, e);
    char head[160], sub[200], label[72];
    sel_fmt(head, sizeof(head), "Looking for %s\xE2\x80\x99s saves", user_label(S, S->user_focus, label, sizeof(label)));
    sel_fmt(sub, sizeof(sub), "%s starts when this is done.", display_name(S, &S->titles[S->sel]));
    card_head(S, s, x, y, head, sub, a);

    int bx = x + 48, bw = USERS_W - 96, by = y + 204, bh = 16;
    u32 total = S->scan_total ? S->scan_total : 1;
    int fill = (int)((u64)bw * (S->scan_done < total ? S->scan_done : total) / total);
    draw_rrect(s, bx, by, bw, bh, bh / 2, fade(0x30FFFFFFu, a));
    if (fill > 0) {
        draw_halo(s, bx, by, fill < bh ? bh : fill, bh, bh / 2, 14, (int)(60 * e), 0xFFFFFF);
        draw_rrect(s, bx, by, fill < bh ? bh : fill, bh, bh / 2, fade(0xFFF4F6F8u, a));
    }
    char count[64], left[64];
    sel_fmt(count, sizeof(count), "%u of %u games checked", S->scan_done, S->scan_total);
    time_left(S, left, sizeof(left));
    draw_text(S, s, 25, bx, by + 66, count, fade(SOFT, a), bw / 2);
    int lw = text_width(S, 25, left);
    draw_text(S, s, 25, bx + bw - lw, by + 66, left, fade(SOFT, a), 0);
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
    if (S->toast_ok) { /* a tick on green */
        draw_disc(s, (float)x + 44, (float)y + h / 2.0f, 16, 0, fade(0xFF4FB86Cu, a));
        draw_line(s, (float)x + 36, (float)y + h / 2.0f + 1, (float)x + 42, (float)y + h / 2.0f + 7, 3.2f, fade(WHITE, a));
        draw_line(s, (float)x + 42, (float)y + h / 2.0f + 7, (float)x + 53, (float)y + h / 2.0f - 6, 3.2f, fade(WHITE, a));
    } else {
        draw_warning(s, (float)x + 44, (float)y + h / 2.0f, 30, fade(RED, a), fade(0xFF2A1E08u, a));
    }
    draw_text(S, s, 25, x + 72, y + 42, S->toast, fade(WHITE, a), w - 72 - 32);
}

void ui_draw(struct sel *S)
{
    surface *s = &S->back;
    /* the rows to redraw: everything, or the top bar and / or the row */
    int y0 = 0, y1 = SCREEN_H;
    if (S->drawn_once && !(S->changed & CH_ALL) && S->panel == 0 && S->ask == 0 && S->toast_a == 0 &&
        S->users_a == 0 && S->scan_a == 0 && S->set_a == 0) {
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
    float cards = S->users_a > S->scan_a ? S->users_a : S->scan_a;
    if (cards > 0) {
        draw_dim(s, 0, SCREEN_W, 0, SCREEN_H, (int)(150 * (1 - (1 - cards) * (1 - cards))));
        if (S->users_a > 0)
            users_dialog(S, s, S->users_a);
        if (S->scan_a > 0)
            scan_card(S, s, S->scan_a);
    }
    if (S->set_a > 0)
        set_draw(S, s);
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

/* ---- shared with setui.c ---- */
int ui_chip(struct sel *S, surface *s, int x, int y, u32 fg, int sign, const char *text, u32 a)
{
    return chip(S, s, x, y, fg, sign, text, a);
}
void ui_text_shadowed(struct sel *S, surface *s, int px, int x, int y, const char *str, u32 c, int max_w)
{
    text_shadowed(S, s, px, x, y, str, c, max_w);
}
void ui_text_bold(struct sel *S, surface *s, int px, int x, int y, const char *str, u32 c, int max_w)
{
    text_bold(S, s, px, x, y, str, c, max_w);
}
int ui_text_tracked(struct sel *S, surface *s, int px, int x, int y, const char *str, u32 c, int track)
{
    return text_tracked(S, s, px, x, y, str, c, track);
}
int ui_wrap(struct sel *S, int px, int w, const char *str, char lines[][128], int max)
{
    return wrap_lines(S, px, w, str, lines, max);
}

/* a gear: eight teeth round a disc, with a hole (Settings) */
void ui_gear(surface *s, float cx, float cy, float r, u32 c, u32 hole)
{
    static const float dir[8][2] = {{1, 0}, {0.7071f, 0.7071f}, {0, 1}, {-0.7071f, 0.7071f},
                                    {-1, 0}, {-0.7071f, -0.7071f}, {0, -1}, {0.7071f, -0.7071f}};
    for (int i = 0; i < 8; i++)
        draw_line(s, cx + dir[i][0] * r * 0.7f, cy + dir[i][1] * r * 0.7f, cx + dir[i][0] * r * 1.18f,
                  cy + dir[i][1] * r * 1.18f, r * 0.34f, c);
    draw_disc(s, cx, cy, r * 0.86f, 0, c);
    draw_disc(s, cx, cy, r * 0.36f, 0, hole);
}
