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
} surface;

/* ---- the catalogue, as the selector sees it ---- */
#define TITLE_COMPLETE 1u /* fully installed: can boot */

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
    u8 bg_state;      /* 0 not tried, 1 has pic1, 2 none */
};

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
#define TILE_BIG 360
#define TILE_SMALL 256
struct sel {
    void *plat;              /* platform state */
    int plat_title;          /* platform scratch: the title being loaded */
    surface back;            /* where frames are drawn */
    surface bg[2];           /* prepared (scaled, dimmed) backgrounds */
    surface bg_default;      /* the generated background, dimmed */
    int bg_cur;              /* index into bg of the visible one */
    int bg_title;            /* title whose background is in bg[bg_cur], -1 = default */
    int bg_from_valid;
    float bg_fade;           /* 0..1: crossfade from bg[!bg_cur] */
    struct fontcache fc;
    struct title *titles;
    int ntitles;
    /* navigation */
    int sel;                 /* selected title */
    float pos;               /* animated scroll position */
    float panel;             /* info panel slide 0..1 */
    int panel_open;
    float launch;            /* 0..1 fade out after Cross */
    int launching;
    u64 last_move_us;        /* time of the last selection change */
    u64 toast_until_us;
    char toast[96];
    /* clock */
    int clock_h, clock_m, clock_24h, clock_valid;
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

/* ---- platform (main.c / preview.c) ---- */
void *sel_alloc(struct sel *S, u64 size); /* 64-byte aligned, zeroed; NULL when the heap is full */
u64 sel_mark(struct sel *S);              /* temporaries: sel_release(S, mark) frees everything since */
void sel_release(struct sel *S, u64 mark);
void sel_log(struct sel *S, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sel_fmt(char *buf, int size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/* Decode the title's file (e.g. "sce_sys/icon0.png") into a temporary BGRA
 * surface; 0 on success.  Freed by the caller's sel_release. */
int sel_load_png(struct sel *S, int title, const char *path, surface *out);

/* ---- drawing (draw.c) ---- */
void draw_fill(surface *s, int x, int y, int w, int h, u32 argb);
void draw_rect_blend(surface *s, int x, int y, int w, int h, u32 argb); /* alpha from argb */
void draw_copy(surface *dst, const surface *src);                        /* same size */
void draw_mix(surface *dst, const surface *a, const surface *b, int t256); /* dst = a + (b - a) * t */
void draw_scale(surface *dst, int dx, int dy, int dw, int dh, const surface *src, int alpha, int clip_x0);
void draw_resample(surface *dst, const surface *src); /* area/bilinear resample into the whole dst */
void draw_cover(surface *dst, const surface *src);    /* scale to cover dst, centred, cropped */
void draw_shadow(surface *s, int x, int y, int w, int h, int radius, int alpha);
void draw_frame(surface *s, int x, int y, int w, int h, int t, u32 argb);
void draw_dim(surface *s, int x0, int x1, int y0, int y1, int alpha); /* darken by alpha/256 */
void draw_disc(surface *s, float cx, float cy, float r, float stroke, u32 argb); /* stroke 0: filled */
void draw_line(surface *s, float x0, float y0, float x1, float y1, float w, u32 argb);
void draw_round_rect(surface *s, float x, float y, float w, float h, float r, float stroke, u32 argb);
void draw_triangle_outline(surface *s, float cx, float cy, float r, float w, u32 argb);
void draw_square_outline(surface *s, float cx, float cy, float r, float w, u32 argb);
void draw_default_bg(surface *s);
void draw_fallback_tile(surface *s, u32 tint);

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

/* ---- the UI (ui.c) ---- */
int ui_init(struct sel *S, struct title *titles, int n, int initial);
/* advance by dt microseconds with the given pad buttons; returns 1 when the
 * frame changed, and sets *boot to the chosen title once the launch fade ends */
int ui_step(struct sel *S, u64 now_us, u64 dt_us, u32 buttons, int stick_x, int *boot);
void ui_set_clock(struct sel *S, int h, int m, int h24);
void ui_draw(struct sel *S);
void ui_prepare_bg(struct sel *S, int title); /* decode + dim pic1 into the spare buffer */
void ui_load_icon(struct sel *S, int title);
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
