/*
 * ghostldr game selector: the parts shared by the console module and the
 * offline preview harness (ghostldr/tests/selector).
 *
 * Everything here is plain C over 32-bit BGRA surfaces (0xAARRGGBB in a u32,
 * bytes B G R A in memory: VideoOut's A8R8G8B8).  The module has no writable
 * static data (sel.ld refuses .data and .bss), so all state lives in
 * struct sel, which the platform allocates from the selector's heap.
 *
 * Platform functions (sel_* below) come from main.c on the console and from
 * preview.c on the computer.
 */
#pragma once
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

struct sel;

#define SCREEN_W 1920
#define SCREEN_H 1080
#define SEL_MAX_TITLES 64

typedef struct {
    u32 *px;
    int w, h, stride; /* stride in pixels */
    int cy0, cy1;     /* drawing is clipped to rows cy0..cy1-1 when cy1 > 0 (partial redraws) */
} surface;

static inline int surf_y0(const surface *s) { return s->cy1 > 0 ? s->cy0 : 0; }
static inline int surf_y1(const surface *s) { return s->cy1 > 0 && s->cy1 < s->h ? s->cy1 : s->h; }

/* ---- the catalogue, as the selector sees it ---- */
#define TITLE_COMPLETE 1u /* fully installed: can boot */
#define TITLE_DUMP 2u     /* a game's dump, not homebrew (title_is_dump): marked on a PS4 */
#define TITLE_COPY 4u     /* a second copy of a game on the USB drive (note: which one boots by ID) */
#define TITLE_SAVE_TOOL 8u /* a save tool (its profile's "save_tool"): asks whose saves, then scans them */

/* where a picture came from (host/gfs_meta.py ART_*) */
#define ART_TITLE 1     /* the title's own sce_sys/icon0.png, pic1.png */
#define ART_GIVEN 2     /* gfs_pack.py / gfs_install.py --icon, --backdrop */
#define ART_STORE 3     /* --fetch-art: the PlayStation Store's */
#define ART_GENERATED 4 /* drawn by the selector */
enum { SEL_ICON, SEL_BACKDROP };

/* <prefix>META/meta.bin (host/gfs_meta.py; meta.c) */
#define META_HEADER 256
#define META_SCANNED 1u /* the title's own sce_sys was looked at: what it has is in here */
#define META_FMT_RAW 1
#define META_FMT_QOI 2
enum { META_SFO, META_ICON, META_BACKDROP, META_SECTIONS };
struct meta_sect {
    u32 off, size;
    u16 w, h;
    u8 format, source;
};
struct meta_info {
    u32 flags, size;
    u8 name_source;
    char name[128], version[16];
    struct meta_sect s[META_SECTIONS];
};

struct title {
    char id[16];      /* CUSA11456; may be empty */
    char name[128];   /* UTF-8 display name; empty: use the ID */
    char version[16]; /* 02.18; may be empty */
    char prefix[16];  /* ghostfs container prefix; may be empty */
    u64 size;         /* bytes installed; 0: unknown */
    u32 flags;        /* TITLE_* */
    u32 tint;         /* fallback colour (from the ID) */
    surface icon;     /* TILE_BIG square, pixels allocated at start */
    surface icon_small; /* TILE_SMALL square */
    u8 icon_state;    /* 0 not tried, 1 loaded, 2 none */
    u8 bg_state;      /* 0 not tried, 1 has a backdrop, 2 none */
    u8 icon_src, bg_src; /* ART_*: set by sel_load_art, or ART_GENERATED */
    u8 marked;        /* the PS4 warning is drawn into icon and icon_small */
    char where[128];  /* a game on a USB drive: its folder there; empty: installed */
    char note[96];    /* why it can't start, or about this copy; may be empty */
};

/* ---- the console's users (a save tool asks whose saves to open) ---- */
#define SEL_MAX_USERS 16
struct sel_user {
    s32 id;        /* SceUserServiceUserId */
    u8 signed_in;  /* logged in now: the save service opens only their saves */
    u8 current;    /* the user running SWRR */
    char name[64]; /* UTF-8; empty: unknown */
};

/* ---- Settings (setui.c): what it shows, filled by the platform (main.c on
 * the console, preview.c here) when Settings opens ---- */
#define SEL_MAX_DRIVES 4
#define FS_UNKNOWN 0
#define FS_EXFAT 1
#define FS_FAT32 2
struct sel_drive {
    u32 id;          /* libSceUsbStorage's device ID */
    char name[64];   /* its maker and model */
    u8 fs;           /* FS_* */
    u8 listed;       /* the selector's games come from it */
    s16 games, bad;  /* games found on it and folders that can't start; -1: not looked at */
    u64 total, free; /* bytes; 0: unknown */
};
#define USB_UNKNOWN 0 /* not asked */
#define USB_READ 1    /* drives listed (ndrives may be 0: none plugged in) */
#define USB_PS4 2     /* not read on a PS4 */
#define USB_JAKX 3    /* not read in Jak X */
#define USB_NONE 4    /* libSceUsbStorage unavailable or refused */
#define SEL_MAX_PORTS 6
struct sel_port {
    u16 port;
    char what[112];
};
/* a title installed in saved data, listed when the row shows a USB drive's games */
struct sel_inst {
    char id[16], name[64], version[16];
    u64 size;
    u8 incomplete;
};
/* a switch of Settings > Advanced (conf.c: a boot context flag, kept in GLDRCONF/settings.txt) */
#define SEL_MAX_SWITCHES 8
struct sel_switch {
    char key[16], label[48];
    char on[112], off[112]; /* what On and Off do, one line each */
    u64 flag;               /* CTX_FLAG_* */
    u8 on_sets;             /* On sets the flag (else clears it) */
    u8 def;                 /* the default: 1 On */
    u8 logs;                /* shown under Logs, not Advanced */
};
#define CONF_LOG_USB 0      /* the switch for the log on the USB drive (rel-diag's CTX_FLAG_NO_USB_LOG) */
struct sel_info {
    struct sel_switch sw[SEL_MAX_SWITCHES];
    int nsw;
    u32 sw_on;          /* the switches' states, bit i */
    int sw_safe;        /* this boot left the switches out (the last one with them never reached the selector) */
    int sw_live;        /* this boot started with them: 1, 0 (run.py boots, safe mode), -1 none to apply */
    struct sel_inst inst[SEL_MAX_TITLES];
    int ninst;         /* -1: the row shows the installed titles itself */
    char version[40];  /* ghostldr's (git describe at build time) */
    char build[64];    /* the bundle's build ID, or how this boot came */
    char cache[160];   /* the bundle cache, in words */
    char console[64];  /* "PS5 · firmware 13.60" */
    char host[112];    /* "Star Wars Racer Revenge · US (CUSA03474) · 01.00" */
    char ip[48];       /* "" when unknown */
    struct sel_port ports[SEL_MAX_PORTS];
    int nports;
    int usb;           /* USB_* */
    struct sel_drive drives[SEL_MAX_DRIVES];
    int ndrives;
    int log_usb;       /* the log on a USB drive (switch CONF_LOG_USB): 1 on, 0 off */
    char log_where[160]; /* where it goes, as the core says (drive and file); "" unknown yet */
    int netinst;       /* the network installer is in this build */
};
/* a snapshot of the network installer (ghost_api.h struct netinst_status) */
#define NET_IDLE 0
#define NET_WAITING 1
#define NET_BUSY 2
#define NET_STOPPED 3
#define NET_FAILED 4
struct sel_net {
    u32 state; /* NET_* */
    char url[48], token[16], title[16], name[64];
    u64 done, total;
    u32 rate_kib, installed;
    char message[128];
    u32 changes; /* catalogue writes since the start (installs and removals): the row is read again */
};
enum { NETOP_START, NETOP_STATUS, NETOP_STOP };
/* a row of a Settings section (setui.c builds them from struct sel_info) */
#define SROW_HEAD 1   /* a small-capitals heading */
#define SROW_TEXT 2   /* a label and its value */
#define SROW_ITEM 3   /* a title line, a chip and lines under it (focusable: the list scrolls) */
#define SROW_TOGGLE 4 /* On / Off */
#define SROW_ACTION 5 /* does something on Cross */
struct srow {
    u8 kind, nsub, on; /* on: a switch's state */
    u16 act;        /* SACT_* for toggles and actions */
    u32 chip_c;     /* the chip's colour; 0: no chip */
    char chip[32];
    char text[128]; /* the label, or an item's title line */
    char sub[3][256]; /* the value, or lines under the title */
    int y, h;       /* laid out by setui.c */
};
#define SET_MAX_ROWS 112

/* ---- outline font (font.c) ---- */
struct glf;                  /* the GLF1 blob (tools/mkfont.py) */
struct glyph {
    u32 key;                 /* size << 21 | code point; 0 = empty slot */
    s16 w, h, left, top;     /* bitmap size; offset from the pen (top: above the baseline) */
    s32 adv;                 /* advance in 1/64 px */
    u16 gid;                 /* builtin glyph id (kerning); 0xFFFF for system glyphs */
    u8 *alpha;               /* w * h coverage */
};

/* A glyph source other than the built-in outlines (the console's system
 * font).  render() fills *g and its alpha (from glyph_alloc); nonzero: use
 * the built-in font for this glyph. */
struct glyph_source {
    void *ctx;
    int (*render)(void *ctx, struct sel *S, int px, u32 cp, struct glyph *g);
    int (*metrics)(void *ctx, int px, int *ascent, int *descent);
};

#define GLYPH_SLOTS 2048
struct fontcache {
    const u8 *glf;            /* built-in outlines */
    struct glyph_source *sys; /* optional */
    struct glyph *slots;      /* GLYPH_SLOTS */
    u8 *arena;                /* glyph bitmaps */
    u64 arena_size, arena_used;
    float *acc;               /* rasterizer accumulation buffer */
    u64 acc_cap;              /* floats */
    u32 misses;
};

/* ---- whole-UI state (ui.c) ---- */
#define TILE_BIG 384             /* the focused tile (host/gfs_meta.py ICON) */
#define TILE_SMALL 256
#define SEL_PS5 0                /* the console (ui_set_platform) */
#define SEL_PS4 1                /* SWRR gets 2.5 GiB here (docs/ps4.md): game dumps are marked and need a Yes */
struct sel {
    void *plat;              /* platform state */
    int platform;            /* SEL_PS5 or SEL_PS4 */
    int plat_title;          /* platform scratch: the title being loaded */
    surface back;            /* where frames are drawn (always the whole current frame) */
    int damage_y0, damage_y1; /* rows ui_draw changed in back */
    surface bg[2];           /* prepared (scaled, shaded) backdrops */
    int bg_cur;              /* index into bg of the visible one */
    int bg_title;            /* title whose background is in bg[bg_cur], -1 = default */
    int bg_from_valid;
    float bg_fade;           /* 0..1: crossfade from bg[!bg_cur] */
    struct fontcache fc;
    struct title *titles;
    int ntitles;
    /* navigation */
    int sel;                 /* selected title */
    float pos, vel;          /* animated scroll position and its speed (titles per second) */
    float panel;             /* info panel slide 0..1 */
    int panel_open;
    float launch;            /* 0..1 fade out after Cross */
    int launching;
    u64 last_move_us;        /* time of the last selection change */
    u64 toast_until_us;
    float toast_a;           /* 0..1: the message fading in and out */
    char toast[96];
    int toast_ok;            /* the message is good news (a tick, not a warning sign) */
    /* PS4: the question before a game dump boots */
    int ask_open, ask_yes;   /* the dialog is up; Yes has the focus (No at first) */
    float ask, ask_focus;    /* 0..1: the dialog fading in; the focus moving from No (0) to Yes (1) */
    /* a save tool (TITLE_SAVE_TOOL): whose saves to open, then the save scan */
    struct sel_user users[SEL_MAX_USERS];
    int nusers;
    int users_open, user_focus; /* the question is up; the focused user */
    int users_fixed;            /* the title can't change any more: no going back to the list */
    int user_chosen;            /* Cross on a user: the scan is about to run (ui_scan_*) */
    float users_a, user_y;      /* 0..1 fading in; the focus, moving between rows */
    int scan_req;               /* the platform should run the scan for users[user_focus] */
    int scanning, scan_shown, scan_cancel;
    u32 scan_done, scan_total;
    u64 scan_t0, scan_prev, scan_per; /* microseconds: the start, the last progress, per title (smoothed) */
    u64 scan_left;              /* microseconds left (estimate); 0: not known yet */
    u64 scan_hold;              /* a shown scan that ended: its full bar stays until then */
    float scan_a;               /* 0..1 the progress card fading in */
    /* Settings (setui.c): a sheet over the screen, sections on the left */
    struct sel_info *info;   /* the platform's (sel_info_load when Settings opens) */
    struct srow *rows;       /* SET_MAX_ROWS: the open section's */
    int nrows;
    int set_open, set_sec, set_in, set_row; /* up; the section; focus in its rows; the focused row */
    float set_a, set_scroll, set_scroll_to, set_sec_y, set_row_y;
    int ask_kind;            /* the question up: ASK_PS4 (a game's dump) or ASK_CLOSE ("Close ghostldr") */
    int quit;                /* "Close ghostldr": the screen fades and ui_step reports UI_QUIT */
    /* the network installer's card (Install over network) */
    int net_open;            /* the card is up */
    u64 set_now;             /* the last step's time (the hold's countdown) */
    struct sel_net net;      /* its last snapshot */
    u64 net_poll, net_hold;  /* the next poll; Circle held since (stopping a busy install) */
    float net_a;
    u32 net_seen, net_changes; /* installs finished and catalogue writes, as last seen */
    char qr_text[48];        /* the address its QR code is of */
    u8 qr[29 * 29];          /* QR_N * QR_N modules */
    int reload_req;          /* an install finished: the platform reads the catalogue again */
    /* top bar */
    int clock_h, clock_m, clock_24h, clock_valid;
    char user[64];           /* the signed-in user's name; empty: not shown */
    int drawn_once;          /* the next ui_draw may be partial */
    u32 changed;             /* CH_* since the last ui_draw */
    /* input repeat */
    u32 held;
    int repeat_dir;
    u64 repeat_at_us;
    int dirty;
};

/* buttons (ScePadData.buttons) */
#define PAD_OPTIONS 0x8u
#define PAD_UP 0x10u
#define PAD_RIGHT 0x20u
#define PAD_DOWN 0x40u
#define PAD_LEFT 0x80u
#define PAD_L1 0x400u
#define PAD_R1 0x800u
#define PAD_TRIANGLE 0x1000u
#define PAD_CIRCLE 0x2000u
#define PAD_CROSS 0x4000u
#define PAD_SQUARE 0x8000u

#define PAD_SETTINGS PAD_SQUARE /* Settings from the row */
#define ASK_PS4 0
#define ASK_CLOSE 1
#define UI_QUIT (-20) /* ui_step's *boot after "Close ghostldr" (sel_api.h SEL_EXIT_LOADER) */

#define CH_ALL 1u   /* everything (backdrop, panel, launch, toast) */
#define CH_ROW 2u   /* the tiles and the focused title's name */
#define CH_TOP 4u   /* the top bar (clock) */

/* ---- platform (main.c / preview.c) ---- */
void *sel_alloc(struct sel *S, u64 size); /* 64-byte aligned, zeroed; NULL when the heap is full */
u64 sel_mark(struct sel *S);              /* temporaries: sel_release(S, mark) frees everything since */
void sel_release(struct sel *S, u64 mark);
void sel_log(struct sel *S, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sel_fmt(char *buf, int size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/* The title's tile picture (SEL_ICON) or backdrop (SEL_BACKDROP) as a
 * temporary surface (freed by the caller's sel_release), from its META or
 * its own sce_sys files; sets the title's icon_src / bg_src.  0 on success. */
int sel_load_art(struct sel *S, int title, int which, surface *out);
/* Settings: what to show (again, each time it opens: the drives may have
 * changed), the log on the USB drive (on, off or asked: where it goes), and
 * the network installer (NETOP_*: 0, or < 0; NETOP_STATUS fills *st) */
void sel_info_load(struct sel *S, struct sel_info *info);
int sel_switch(struct sel *S, int i, int want); /* Advanced switch i: want < 0 asks; its state, or -1; i < 0: reset all */
int sel_netinst(struct sel *S, int op, struct sel_net *st);

/* ---- drawing (draw.c) ---- */
void draw_rect_blend(surface *s, int x, int y, int w, int h, u32 argb); /* alpha from argb */
void draw_copy(surface *dst, const surface *src);                        /* same size */
void draw_mix(surface *dst, const surface *a, const surface *b, int t256); /* dst = a + (b - a) * t */
/* src scaled to dw x dh, blended by its alpha * alpha/256, its colour * bright/256 */
void draw_scale(surface *dst, int dx, int dy, int dw, int dh, const surface *src, int alpha, int bright, int clip_x0);
void draw_resample(surface *dst, const surface *src); /* area/bilinear resample into the whole dst */
void draw_cover(surface *dst, const surface *src);    /* scale to cover dst, centred, cropped */
/* soft shadow / glow around a rectangle with corner radius cr, fading over radius */
void draw_shadow(surface *s, int x, int y, int w, int h, int cr, int radius, int alpha);
void draw_halo(surface *s, int x, int y, int w, int h, int cr, int radius, int alpha, u32 rgb);
void draw_rrect(surface *s, int x, int y, int w, int h, int r, u32 argb);        /* filled rounded rect */
void draw_rring(surface *s, int x, int y, int w, int h, int r, int t, u32 argb); /* its outline, t wide */
void draw_round_corners(surface *s, int r); /* cut a picture's corners round (its alpha) */
void draw_wash(surface *s, u32 argb);       /* colours towards argb by its alpha, alpha kept */
void draw_dim(surface *s, int x0, int x1, int y0, int y1, int alpha); /* darken by alpha/256 */
void draw_disc(surface *s, float cx, float cy, float r, float stroke, u32 argb); /* stroke 0: filled */
void draw_line(surface *s, float x0, float y0, float x1, float y1, float w, u32 argb);
void draw_warning(surface *s, float cx, float cy, float size, u32 fill, u32 ink); /* a warning sign */
void draw_ghost(surface *s, int cx, int cy, int size, u32 body, u32 eyes);       /* ghostldr's mark */
void draw_generated_bg(surface *s, u32 tint);  /* a backdrop in the title's colour */
void draw_fallback_tile(surface *s, u32 tint); /* a tile's background in its colour */
void draw_shade(surface *s);                     /* the backdrop's dimming and gradients */
void draw_vgradient(surface *s, int x, int y, int w, int h, u32 top, u32 bottom); /* alpha-blended */
u32 mix_rgb(u32 a, u32 b, int t256);

/* ---- text (font.c) ---- */
int font_init(struct sel *S, const u8 *glf, u64 cache_bytes);
const struct glyph *font_glyph(struct sel *S, int px, u32 cp);
u8 *glyph_alloc(struct sel *S, u64 size);
int text_width(struct sel *S, int px, const char *utf8);
/* draws at the baseline; max_w > 0 truncates with an ellipsis; returns the width */
int draw_text(struct sel *S, surface *s, int px, int x, int y, const char *utf8, u32 argb, int max_w);
int text_fits_font(struct sel *S, const char *utf8); /* 1 when most characters have glyphs */
int font_ascent(struct sel *S, int px);
u32 utf8_next(const char **p);

/* ---- param.sfo (sfo.c) ---- */
int sfo_get(const u8 *sfo, u64 size, const char *key, char *out, int cap, s32 *num);
/* A game's dump rather than homebrew?  id: the catalogue's (or folder's) title
 * ID; sfo_id, content_id: param.sfo's TITLE_ID and CONTENT_ID; any may be
 * empty.  A CONTENT_ID from publisher IVnnnn is homebrew; otherwise a retail
 * title ID (CUSA, PLAS, ... and five digits) in any of them is a dump; and
 * anything else homebrew.  Doubt goes to "dump" (docs/selector.md). */
int title_is_dump(const char *id, const char *sfo_id, const char *content_id);

/* ---- QR codes (qr.c): version 3-M, up to 42 bytes ---- */
#define QR_N 29
int qr_encode(const char *text, u8 m[QR_N * QR_N]); /* 0, or -1: too long */
void qr_draw(surface *s, int x, int y, int size, const u8 *m, u32 a);

/* ---- META and QOI (meta.c) ---- */
int meta_parse(const u8 *d, u64 n, struct meta_info *mi);
int qoi_header(const u8 *d, u64 n, int *w, int *h);
int qoi_decode(const u8 *d, u64 n, surface *out);

/* ---- the UI (ui.c) ---- */
int ui_init(struct sel *S, struct title *titles, int n, int initial);
/* advance by dt microseconds with the given pad buttons and left stick;
 * returns 1 when the frame changed, and sets *boot to the chosen title once
 * the launch fade ends */
int ui_step(struct sel *S, u64 now_us, u64 dt_us, u32 buttons, int stick_x, int stick_y, int *boot);
/* A save tool: the console's users (again when who is signed in changes),
 * the question asked at once for the selected title (it is the only one, or
 * the core has it already: fixed, no going back to the list), and the scan
 * the platform runs once S->scan_req is set: start, its progress (returns
 * nonzero when Circle cancelled it) and its end (back to the question when
 * cancelled, else the launch, after a moment of full bar if it was shown). */
int ui_set_users(struct sel *S, const struct sel_user *users, int n); /* 1: something changed */
int title_copy_of(const struct title *t, char *out, int cap); /* a second copy: where the other one is (ui.c) */
void ui_ask_user(struct sel *S, int fixed);
void ui_scan_start(struct sel *S, u64 now);
int ui_scan_progress(struct sel *S, u64 now, u32 done, u32 total);
void ui_scan_end(struct sel *S, u64 now, int result);
void ui_set_clock(struct sel *S, int h, int m, int h24);
void ui_set_user(struct sel *S, const char *name);
void ui_set_platform(struct sel *S, int platform); /* SEL_PS5 (the default) or SEL_PS4, before ui_init */
void ui_draw(struct sel *S);
void ui_prepare_bg(struct sel *S, int title); /* decode + dim pic1 into the spare buffer */
void ui_load_icon(struct sel *S, int title);
/* the titles again (an install finished): the selection stays on its title;
 * new ones need their tiles' memory (ui_title_alloc) */
int ui_title_alloc(struct sel *S, struct title *t);
void ui_set_titles(struct sel *S, struct title *titles, int n);
/* Settings (setui.c) */
void set_open(struct sel *S, u64 now);
void set_step(struct sel *S, u64 now, u64 dt, u32 pressed, u32 held, int step);
void set_draw(struct sel *S, surface *s);
void set_hints(struct sel *S, surface *s);
int set_busy(const struct sel *S);   /* the sheet or its card is moving or up: draw whole frames */
/* shared drawing (ui.c) */
int ui_hint(struct sel *S, surface *s, int x, int base, int button, const char *label, u32 c);
void ui_card(surface *s, int x, int y, int w, int h, float e);
int ui_chip(struct sel *S, surface *s, int x, int y, u32 fg, int sign, const char *text, u32 a);
void ui_text_shadowed(struct sel *S, surface *s, int px, int x, int y, const char *str, u32 c, int max_w);
void ui_text_bold(struct sel *S, surface *s, int px, int x, int y, const char *str, u32 c, int max_w);
int ui_text_tracked(struct sel *S, surface *s, int px, int x, int y, const char *str, u32 c, int track);
int ui_wrap(struct sel *S, int px, int w, const char *str, char lines[][128], int max);
void ui_toast(struct sel *S, u64 now, const char *msg);
void ui_toast_ok(struct sel *S, u64 now, const char *msg); /* good news */
void ui_gear(surface *s, float cx, float cy, float r, u32 c, u32 hole);
void ui_size(char *out, int cap, u64 bytes);
u32 title_tint(const char *id, const char *name);

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int str_len(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}
static inline int str_same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}
static inline void str_copy(char *d, const char *s, int cap)
{
    int n = 0;
    while (s[n] && n + 1 < cap) {
        d[n] = s[n];
        n++;
    }
    if (cap)
        d[n] = 0;
}
