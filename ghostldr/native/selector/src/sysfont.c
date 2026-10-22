/*
 * The console's system font through libSceFont (with its FreeType renderer,
 * libSceFontFt), used as the glyph source when it opens.  Nothing from the
 * font is kept: glyphs are rendered into the selector's own glyph cache, and
 * the library, renderer and font are closed and both modules unloaded before
 * the selector returns.  Any failure on the way leaves the built-in outlines
 * (font.c) in charge.
 *
 * libSceFont allocates through callbacks; they serve it from a first-fit
 * free list in a region of the selector's heap.
 */
#include "plat.h"

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);

/* ---- a small allocator for the library ---- */
struct blk {
    u64 size; /* including this header; bit 0: in use */
    u64 pad;
};

struct mspace {
    u8 *base;
    u64 size;
};

static void *ms_alloc(void *obj, u32 n)
{
    struct mspace *ms = obj;
    u64 need = ((u64)n + sizeof(struct blk) + 15) & ~15ull;
    for (u8 *p = ms->base; p < ms->base + ms->size;) {
        struct blk *b = (struct blk *)p;
        u64 sz = b->size & ~1ull;
        if (!sz)
            break;
        if (!(b->size & 1)) {
            /* merge the free blocks after this one */
            struct blk *nx = (struct blk *)(p + sz);
            while ((u8 *)nx < ms->base + ms->size && nx->size && !(nx->size & 1)) {
                sz += nx->size;
                nx = (struct blk *)(p + sz);
            }
            b->size = sz;
            if (sz >= need) {
                if (sz - need >= 64) {
                    struct blk *rest = (struct blk *)(p + need);
                    rest->size = sz - need;
                    sz = need;
                }
                b->size = sz | 1;
                return b + 1;
            }
        }
        p += sz;
    }
    return 0;
}

static void ms_free(void *obj, void *ptr)
{
    if (ptr)
        ((struct blk *)ptr - 1)->size &= ~1ull;
}

static void *ms_calloc(void *obj, u32 n, u32 size)
{
    u64 total = (u64)n * size;
    if (total > 0xFFFFFFFFull)
        return 0;
    void *p = ms_alloc(obj, (u32)total);
    if (p)
        memset(p, 0, total);
    return p;
}

static void *ms_realloc(void *obj, void *ptr, u32 n)
{
    if (!ptr)
        return ms_alloc(obj, n);
    u64 have = (((struct blk *)ptr - 1)->size & ~1ull) - sizeof(struct blk);
    if (have >= n)
        return ptr;
    void *q = ms_alloc(obj, n);
    if (q) {
        memcpy(q, ptr, have);
        ms_free(obj, ptr);
    }
    return q;
}

/* ---- the library ---- */
#define FONTSET_SST_JP 0x18070444u       /* SST with Japanese (covers Latin too) */
#define FONTSET_SST_EUROPEAN 0x18070044u
#define OPEN_FILE_STREAM 1

struct sysfont {
    struct plat *P;
    struct mspace ms;
    u8 mem[256];       /* SceFontMemory (0x40 bytes; room to spare) */
    void *iface[8];    /* SceFontMemoryInterface: alloc, free, realloc, calloc, mspace create/destroy */
    void *lib, *renderer, *font;
    int scale_px;
    u8 surf[256];      /* SceFontRenderSurface (0x80) */
    u8 *pixels;        /* scratch for one glyph */
    u32 pixels_size;
    struct glyph_source src;
    /* functions */
    s32 (*mem_init)(void *, void *, u32, const void *, void *, void *, void *);
    s32 (*mem_term)(void *);
    s32 (*lib_create)(const void *, const void *, void **);
    s32 (*lib_destroy)(void **);
    s32 (*sys_fonts)(void *);
    s32 (*rnd_create)(const void *, const void *, void **);
    s32 (*rnd_destroy)(void **);
    s32 (*open_set)(void *, u32, u32, const void *, void **);
    s32 (*close)(void *);
    s32 (*bind)(void *, void *);
    s32 (*unbind)(void *);
    s32 (*scale)(void *, float, float);
    s32 (*render_scale)(void *, float, float);
    s32 (*metrics)(void *, u32, void *);
    void (*surf_init)(void *, void *, int, int, int, int);
    void (*surf_scissor)(void *, int, int, int, int);
    s32 (*render)(void *, u32, void *, float, float, void *, void *);
};

/* SceFontGlyphMetrics: width, height, then horizontal bearing x/y and advance */
struct font_metrics {
    float w, h, bx, by, adv, vbx, vby, vadv;
};

static int fl_i(float v) { int i = (int)v; return (float)i > v ? i - 1 : i; }
static int ce_i(float v) { int i = (int)v; return (float)i < v ? i + 1 : i; }

static int sf_render(void *ctx, struct sel *S, int px, u32 cp, struct glyph *g)
{
    struct sysfont *sf = ctx;
    if (sf->scale_px != px) {
        if (sf->scale(sf->font, (float)px, (float)px) < 0 || sf->render_scale(sf->font, (float)px, (float)px) < 0)
            return -1;
        sf->scale_px = px;
    }
    struct font_metrics m[2];
    memset(m, 0, sizeof(m));
    if (sf->metrics(sf->font, cp, m) < 0)
        return -1;
    if (!(m[0].adv >= 0 && m[0].adv < 4.0f * (float)px && m[0].w >= 0 && m[0].w < 4.0f * (float)px && m[0].h >= 0 &&
          m[0].h < 4.0f * (float)px))
        return -1; /* implausible: the layout guess is wrong; use the built-in font */
    g->gid = 0xFFFF;
    g->adv = (s32)(m[0].adv * 64 + 0.5f);
    if (m[0].w <= 0 || m[0].h <= 0) { /* a space */
        g->w = g->h = 0;
        return 0;
    }
    int ox = 2 - fl_i(m[0].bx), oy = 2 + ce_i(m[0].by);
    int w = ce_i(m[0].w) + 5, h = ce_i(m[0].h) + 5;
    if ((u32)(w * h) > sf->pixels_size)
        return -1;
    memset(sf->pixels, 0, (u64)w * h);
    sf->surf_init(sf->surf, sf->pixels, w, 1, w, h);
    if (sf->surf_scissor)
        sf->surf_scissor(sf->surf, 0, 0, w, h);
    u8 result[128];
    memset(result, 0, sizeof(result));
    if (sf->render(sf->font, cp, sf->surf, (float)ox, (float)oy, m, result) < 0)
        return -1;
    u8 *a = glyph_alloc(S, (u64)w * h);
    if (!a)
        return -2;
    memcpy(a, sf->pixels, (u64)w * h);
    g->w = (s16)w, g->h = (s16)h, g->left = (s16)-ox, g->top = (s16)oy, g->alpha = a;
    return 0;
}

#define SYM(field, h, name)                                        \
    do {                                                           \
        *(void **)&sf->field = sys_sym(P, h, name);                \
        if (!sf->field)                                            \
            return -1;                                             \
    } while (0)

static int sf_bind(struct sysfont *sf, struct plat *P, u32 font, u32 ft)
{
    SYM(mem_init, font, "sceFontMemoryInit");
    SYM(mem_term, font, "sceFontMemoryTerm");
    SYM(lib_create, font, "sceFontCreateLibrary");
    SYM(lib_destroy, font, "sceFontDestroyLibrary");
    SYM(sys_fonts, font, "sceFontSupportSystemFonts");
    SYM(rnd_create, font, "sceFontCreateRenderer");
    SYM(rnd_destroy, font, "sceFontDestroyRenderer");
    SYM(open_set, font, "sceFontOpenFontSet");
    SYM(close, font, "sceFontCloseFont");
    SYM(bind, font, "sceFontBindRenderer");
    SYM(unbind, font, "sceFontUnbindRenderer");
    SYM(scale, font, "sceFontSetScalePixel");
    SYM(render_scale, font, "sceFontSetupRenderScalePixel");
    SYM(metrics, font, "sceFontGetRenderCharGlyphMetrics");
    SYM(surf_init, font, "sceFontRenderSurfaceInit");
    SYM(render, font, "sceFontRenderCharGlyphImage");
    sf->surf_scissor = (void (*)(void *, int, int, int, int))sys_sym(P, font, "sceFontRenderSurfaceSetScissor");
    return 0;
}

int sysfont_open(struct sel *S, struct plat *P, u64 mem_bytes)
{
    struct sysfont *sf = sel_alloc(S, sizeof(*sf));
    if (!sf)
        return -1;
    sf->P = P;
    P->sf = sf;
    sf->ms.size = mem_bytes - (1u << 20);
    sf->ms.base = sel_alloc(S, sf->ms.size);
    sf->pixels_size = 1u << 20;
    sf->pixels = sel_alloc(S, sf->pixels_size);
    if (!sf->ms.base || !sf->pixels)
        return -1;
    ((struct blk *)sf->ms.base)->size = sf->ms.size;
    u32 font = sys_module(P, "libSceFont.sprx", 1, &P->mod_font);
    u32 ft = sys_module(P, "libSceFontFt.sprx", 1, &P->mod_fontft);
    if (!font || !ft || sf_bind(sf, P, font, ft) != 0)
        return -1;
    const void *(*sel_lib)(int) = (const void *(*)(int))sys_sym(P, ft, "sceFontSelectLibraryFt");
    const void *(*sel_rnd)(int) = (const void *(*)(int))sys_sym(P, ft, "sceFontSelectRendererFt");
    if (!sel_lib || !sel_rnd)
        return -1;
    sf->iface[0] = (void *)ms_alloc, sf->iface[1] = (void *)ms_free;
    sf->iface[2] = (void *)ms_realloc, sf->iface[3] = (void *)ms_calloc;
    s32 rc;
    sel_log(S, "libSceFont: memory init");
    if ((rc = sf->mem_init(sf->mem, NULL, 0, sf->iface, &sf->ms, NULL, NULL)) < 0)
        goto fail;
    sel_log(S, "libSceFont: library");
    if ((rc = sf->lib_create(sf->mem, sel_lib(0), &sf->lib)) < 0 || !sf->lib)
        goto fail;
    sel_log(S, "libSceFont: system fonts");
    if ((rc = sf->sys_fonts(sf->lib)) < 0)
        goto fail;
    sel_log(S, "libSceFont: renderer");
    if ((rc = sf->rnd_create(sf->mem, sel_rnd(0), &sf->renderer)) < 0 || !sf->renderer)
        goto fail;
    sel_log(S, "libSceFont: open SST");
    if ((rc = sf->open_set(sf->lib, FONTSET_SST_JP, OPEN_FILE_STREAM, NULL, &sf->font)) < 0 || !sf->font)
        rc = sf->open_set(sf->lib, FONTSET_SST_EUROPEAN, OPEN_FILE_STREAM, NULL, &sf->font);
    if (rc < 0 || !sf->font)
        goto fail;
    if ((rc = sf->bind(sf->font, sf->renderer)) < 0)
        goto fail;
    /* try one glyph before trusting it */
    struct glyph probe;
    memset(&probe, 0, sizeof(probe));
    u64 m = S->fc.arena_used;
    if (sf_render(sf, S, 40, 'H', &probe) != 0 || probe.w < 8 || probe.h < 20 || probe.adv < 16 * 64) {
        sel_log(S, "libSceFont: test glyph looks wrong (%dx%d, advance %d/64)", probe.w, probe.h, probe.adv);
        S->fc.arena_used = m;
        rc = -1;
        goto fail;
    }
    S->fc.arena_used = m;
    sf->src.ctx = sf;
    sf->src.render = sf_render;
    S->fc.sys = &sf->src;
    sel_log(S, "libSceFont: system font open (SST)");
    return 0;
fail:
    sel_log(S, "libSceFont: failed (%x)", (u32)rc);
    sysfont_close(P);
    return -1;
}

void sysfont_close(struct plat *P)
{
    struct sysfont *sf = P->sf;
    if (sf) {
        if (sf->font) {
            sf->unbind(sf->font);
            sf->close(sf->font);
        }
        if (sf->renderer)
            sf->rnd_destroy(&sf->renderer);
        if (sf->lib)
            sf->lib_destroy(&sf->lib);
        if (sf->mem_term && sf->mem[0] | sf->mem[1])
            sf->mem_term(sf->mem);
        sf->font = sf->renderer = sf->lib = NULL;
        P->sf = NULL;
    }
    sys_unload(P, &P->mod_fontft, "libSceFontFt");
    sys_unload(P, &P->mod_font, "libSceFont");
}
