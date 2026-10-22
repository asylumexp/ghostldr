/*
 * Text: a glyph cache over two sources.
 *
 *  - The console's system font (sysfont.c, through libSceFont) when it could
 *    be opened: struct glyph_source.
 *  - The built-in outlines (GLF1, tools/mkfont.py; Noto Sans, SIL OFL 1.1),
 *    rasterized here: quadratic contours are flattened to lines and the
 *    exact pixel coverage is accumulated as signed areas, then summed along
 *    each row (the accumulation technique R. Levien described for font-rs).
 */
#include "sel.h"

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static s16 rds16(const u8 *p) { return (s16)rd16(p); }
static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* GLF1 header fields */
#define GLF_UPEM(f) rd16((f) + 4)
#define GLF_ASC(f) rds16((f) + 6)
#define GLF_DESC(f) rds16((f) + 8)
#define GLF_NGLYPHS(f) rd16((f) + 12)
#define GLF_NCMAP(f) rd16((f) + 14)
#define GLF_CMAP(f) rd32((f) + 16)
#define GLF_GLYPHS(f) rd32((f) + 20)
#define GLF_KERN(f) rd32((f) + 24)
#define GLF_NKERN(f) rd32((f) + 28)
#define GLF_DATA(f) rd32((f) + 32)

#define ACC_MAX (300 * 300)

int font_init(struct sel *S, const u8 *glf, u64 cache_bytes)
{
    struct fontcache *fc = &S->fc;
    if (!glf || glf[0] != 'G' || glf[1] != 'L' || glf[2] != 'F' || glf[3] != '1' || !GLF_UPEM(glf))
        return -1;
    fc->glf = glf;
    fc->slots = sel_alloc(S, sizeof(struct glyph) * GLYPH_SLOTS);
    fc->arena = sel_alloc(S, cache_bytes);
    fc->arena_size = cache_bytes;
    fc->acc_cap = ACC_MAX + 8;
    fc->acc = sel_alloc(S, fc->acc_cap * sizeof(float));
    return fc->slots && fc->arena && fc->acc ? 0 : -1;
}

static void cache_reset(struct fontcache *fc)
{
    __builtin_memset(fc->slots, 0, sizeof(struct glyph) * GLYPH_SLOTS);
    fc->arena_used = 0;
}

u8 *glyph_alloc(struct sel *S, u64 size)
{
    struct fontcache *fc = &S->fc;
    size = (size + 15) & ~15ull;
    if (fc->arena_used + size > fc->arena_size)
        return 0;
    u8 *p = fc->arena + fc->arena_used;
    fc->arena_used += size;
    return p;
}

static int glf_gid(const u8 *f, u32 cp)
{
    const u8 *cmap = f + GLF_CMAP(f);
    int lo = 0, hi = GLF_NCMAP(f) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        u32 c = rd32(cmap + 8 * mid);
        if (c == cp)
            return rd16(cmap + 8 * mid + 4);
        if (c < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

static int glf_kern(const u8 *f, int a, int b)
{
    const u8 *k = f + GLF_KERN(f);
    u32 key = (u32)a << 16 | (u32)b;
    int lo = 0, hi = (int)GLF_NKERN(f) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        u32 m = (u32)rd16(k + 8 * mid) << 16 | rd16(k + 8 * mid + 2);
        if (m == key)
            return rds16(k + 8 * mid + 4);
        if (m < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

static u32 varint(const u8 **p)
{
    u32 v = 0;
    int sh = 0;
    for (;;) {
        u8 b = *(*p)++;
        v |= (u32)(b & 0x7F) << sh;
        if (!(b & 0x80) || sh > 28)
            return v;
        sh += 7;
    }
}
static int unzig(u32 v) { return (v & 1) ? -(int)((v + 1) >> 1) : (int)(v >> 1); }

/* ---- rasterizer ---- */
struct raster {
    float *acc;
    int w, h;
};

static inline float fl(float v)
{
    int i = (int)v;
    return (float)(v < (float)i ? i - 1 : i);
}

static void line(struct raster *r, float x0, float y0, float x1, float y1)
{
    if (y0 == y1)
        return;
    float dir = 1;
    if (y0 > y1) {
        float t = x0;
        x0 = x1, x1 = t, t = y0, y0 = y1, y1 = t;
        dir = -1;
    }
    float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    if (y0 < 0) {
        x -= y0 * dxdy;
        y0 = 0;
    }
    if (y1 > (float)r->h)
        y1 = (float)r->h;
    for (int y = (int)y0; (float)y < y1; y++) {
        float top = (float)y > y0 ? (float)y : y0;
        float bot = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        float dy = bot - top;
        if (dy <= 0)
            continue;
        float xn = x + dxdy * dy, d = dy * dir;
        float xa = x < xn ? x : xn, xb = x < xn ? xn : x;
        xa = clampf(xa, 0, (float)r->w - 1.001f);
        xb = clampf(xb, 0, (float)r->w - 1.001f);
        float *row = r->acc + (u64)y * (u64)r->w;
        float xaf = fl(xa);
        int ia = (int)xaf, ib = (int)fl(xb) + 1; /* first cell after the span */
        if (ib <= ia + 1) {
            float xm = 0.5f * (x + xn) - xaf;
            xm = clampf(xm, 0, 1);
            row[ia] += d - d * xm;
            row[ia + 1] += d * xm;
        } else {
            float s = 1.0f / (xb - xa);
            float f0 = xa - xaf;
            float a0 = 0.5f * s * (1 - f0) * (1 - f0);
            float f1 = xb - fl(xb);
            float am = 0.5f * s * f1 * f1;
            row[ia] += d * a0;
            if (ib == ia + 2) {
                row[ia + 1] += d * (1 - a0 - am);
            } else {
                float a1 = s * (1.5f - f0);
                row[ia + 1] += d * (a1 - a0);
                for (int i = ia + 2; i < ib - 1; i++)
                    row[i] += d * s;
                float a2 = a1 + (float)(ib - ia - 3) * s;
                row[ib - 1] += d * (1 - a2 - am);
            }
            row[ib] += d * am;
        }
        x = xn;
    }
}

static void quad(struct raster *r, float x0, float y0, float cx, float cy, float x1, float y1)
{
    float ddx = x0 - 2 * cx + x1, ddy = y0 - 2 * cy + y1;
    float dd = __builtin_sqrtf(ddx * ddx + ddy * ddy);
    int n = 1 + (int)__builtin_sqrtf(dd * 2.0f);
    if (n > 24)
        n = 24;
    float px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, it = 1 - t;
        float qx = it * it * x0 + 2 * it * t * cx + t * t * x1;
        float qy = it * it * y0 + 2 * it * t * cy + t * t * y1;
        line(r, px, py, qx, qy);
        px = qx, py = qy;
    }
}

/* Rasterize built-in glyph gid at px into g (bitmap from the glyph arena). */
static int glf_render(struct sel *S, int px, int gid, struct glyph *g)
{
    const u8 *f = S->fc.glf;
    if (gid >= GLF_NGLYPHS(f))
        gid = 0;
    const u8 *e = f + GLF_GLYPHS(f) + 16 * gid;
    float scale = (float)px / (float)GLF_UPEM(f);
    int adv = rd16(e + 4), xmin = rds16(e + 6), ymin = rds16(e + 8), xmax = rds16(e + 10), ymax = rds16(e + 12);
    int ncont = rd16(e + 14);
    g->gid = (u16)gid;
    g->adv = (s32)((float)adv * scale * 64 + 0.5f);
    if (!ncont) {
        g->w = g->h = 0;
        return 0;
    }
    int left = (int)fl((float)xmin * scale) - 1, right = (int)fl((float)xmax * scale) + 2;
    int top = (int)fl((float)ymax * scale) + 2, bottom = (int)fl((float)ymin * scale) - 1;
    int w = right - left + 1, h = top - bottom;
    if (w <= 0 || h <= 0 || (u64)w * (u64)h + 1 > S->fc.acc_cap)
        return -1;
    struct raster r = {S->fc.acc, w, h};
    __builtin_memset(r.acc, 0, ((u64)w * (u64)h + 1) * sizeof(float));
    const u8 *p = f + GLF_DATA(f) + rd32(e);
    int counts[64];
    if (ncont > 64)
        return -1;
    for (int c = 0; c < ncont; c++)
        counts[c] = (int)varint(&p);
    int ux = 0, uy = 0;
    float ox = (float)-left, oy = (float)top;
    for (int c = 0; c < ncont; c++) {
        int n = counts[c];
        if (n <= 0 || n > 512)
            return -1;
        float X[512], Y[512];
        u8 on[512];
        for (int i = 0; i < n; i++) {
            u32 vx = varint(&p), vy = varint(&p);
            on[i] = (u8)(vx & 1);
            ux += unzig(vx >> 1), uy += unzig(vy);
            X[i] = (float)ux * scale + ox;
            Y[i] = oy - (float)uy * scale;
        }
        /* walk the contour from an on-curve point (or the midpoint of the
         * first two off-curve points) */
        int start = -1;
        for (int i = 0; i < n; i++)
            if (on[i]) {
                start = i;
                break;
            }
        float sx, sy;
        if (start < 0) {
            sx = 0.5f * (X[0] + X[1 % n]), sy = 0.5f * (Y[0] + Y[1 % n]);
            start = 0;
        } else {
            sx = X[start], sy = Y[start];
        }
        float lx = sx, ly = sy, cx = 0, cy = 0;
        int have_c = 0;
        for (int k = 1; k <= n; k++) {
            int i = (start + k) % n;
            float qx = X[i], qy = Y[i];
            int is_on = on[i] || k == n; /* close back at the start point */
            if (k == n)
                qx = sx, qy = sy;
            if (is_on) {
                if (have_c)
                    quad(&r, lx, ly, cx, cy, qx, qy);
                else
                    line(&r, lx, ly, qx, qy);
                lx = qx, ly = qy, have_c = 0;
            } else if (have_c) {
                float mx = 0.5f * (cx + qx), my = 0.5f * (cy + qy);
                quad(&r, lx, ly, cx, cy, mx, my);
                lx = mx, ly = my, cx = qx, cy = qy;
            } else {
                cx = qx, cy = qy, have_c = 1;
            }
        }
        if (have_c)
            quad(&r, lx, ly, cx, cy, sx, sy);
    }
    u8 *a = glyph_alloc(S, (u64)w * (u64)h);
    if (!a)
        return -2;
    float sum = 0;
    for (u64 i = 0; i < (u64)w * (u64)h; i++) {
        sum += r.acc[i];
        float v = sum < 0 ? -sum : sum;
        a[i] = (u8)(v >= 1 ? 255 : (int)(v * 255 + 0.5f));
    }
    g->w = (s16)w, g->h = (s16)h, g->left = (s16)left, g->top = (s16)top, g->alpha = a;
    return 0;
}

const struct glyph *font_glyph(struct sel *S, int px, u32 cp)
{
    struct fontcache *fc = &S->fc;
    if (px <= 0 || px > 255 || cp > 0x1FFFFF)
        return 0;
    u32 key = (u32)px << 21 | cp;
    for (int attempt = 0; attempt < 2; attempt++) {
        u32 h = (key * 2654435761u) >> 21;
        for (u32 i = 0; i < GLYPH_SLOTS; i++) {
            struct glyph *g = &fc->slots[(h + i) & (GLYPH_SLOTS - 1)];
            if (g->key == key)
                return g;
            if (g->key)
                continue;
            if (i > GLYPH_SLOTS / 2)
                break; /* table too full */
            __builtin_memset(g, 0, sizeof(*g));
            int rc = -1;
            if (fc->sys && fc->sys->render)
                rc = fc->sys->render(fc->sys->ctx, S, px, cp, g);
            if (rc != 0) {
                __builtin_memset(g, 0, sizeof(*g));
                rc = glf_render(S, px, glf_gid(fc->glf, cp), g);
            }
            if (rc == -2)
                break; /* arena full: flush and retry once */
            if (rc != 0)
                return 0;
            g->key = key;
            fc->misses++;
            return g;
        }
        cache_reset(fc);
    }
    return 0;
}

u32 utf8_next(const char **ps)
{
    const u8 *p = (const u8 *)*ps;
    u32 c = *p++;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (n)
        c &= 0x3Fu >> n;
    for (; n; n--) {
        if ((*p & 0xC0) != 0x80) {
            c = 0xFFFD;
            break;
        }
        c = c << 6 | (*p++ & 0x3F);
    }
    *ps = (const char *)p;
    return c;
}

static int kern_px(struct sel *S, int px, const struct glyph *prev, const struct glyph *g)
{
    if (!prev || prev->gid == 0xFFFF || g->gid == 0xFFFF)
        return 0;
    int k = glf_kern(S->fc.glf, prev->gid, g->gid);
    return k ? (int)((float)k * (float)px * 64 / (float)GLF_UPEM(S->fc.glf)) : 0;
}

int text_width(struct sel *S, int px, const char *s)
{
    s32 pen = 0;
    const struct glyph *prev = 0;
    while (*s) {
        const struct glyph *g = font_glyph(S, px, utf8_next(&s));
        if (!g)
            continue;
        pen += kern_px(S, px, prev, g) + g->adv;
        prev = g;
    }
    return (pen + 32) >> 6;
}

static void blit_glyph(surface *dst, const struct glyph *g, int x, int y, u32 argb)
{
    u32 ca = argb >> 24;
    for (int j = 0; j < g->h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= dst->h)
            continue;
        const u8 *a = g->alpha + (u64)j * (u64)g->w;
        u32 *d = dst->px + (u64)yy * (u64)dst->stride;
        for (int i = 0; i < g->w; i++) {
            int xx = x + i;
            if (!a[i] || xx < 0 || xx >= dst->w)
                continue;
            u32 t = (u32)a[i] * ca / 255;
            t += t >> 7;
            u32 o = d[xx], it = 256 - t;
            u32 rb = (((o & 0xFF00FFu) * it + (argb & 0xFF00FFu) * t) >> 8) & 0xFF00FFu;
            u32 gg = (((o & 0xFF00u) * it + (argb & 0xFF00u) * t) >> 8) & 0xFF00u;
            d[xx] = 0xFF000000u | rb | gg;
        }
    }
}

int draw_text(struct sel *S, surface *s, int px, int x, int y, const char *str, u32 argb, int max_w)
{
    int ell = 0;
    if (max_w > 0 && text_width(S, px, str) > max_w)
        ell = text_width(S, px, "\xE2\x80\xA6"); /* U+2026 */
    s32 pen = x << 6;
    const struct glyph *prev = 0;
    const char *p = str;
    while (*p) {
        const struct glyph *g = font_glyph(S, px, utf8_next(&p));
        if (!g)
            continue;
        s32 k = kern_px(S, px, prev, g);
        if (ell && ((pen + k + g->adv + 32) >> 6) - x > max_w - ell) {
            p = "\xE2\x80\xA6";
            g = font_glyph(S, px, utf8_next(&p));
            if (g) {
                blit_glyph(s, g, ((pen + 32) >> 6) + g->left, y - g->top, argb);
                pen += g->adv;
            }
            break;
        }
        pen += k;
        if (g->w)
            blit_glyph(s, g, ((pen + 32) >> 6) + g->left, y - g->top, argb);
        pen += g->adv;
        prev = g;
    }
    return ((pen + 32) >> 6) - x;
}

/* Do the built-in outlines (or the system font) cover this text?  CJK names
 * without the system font would come out as boxes; the UI shows the title
 * ID instead. */
int text_fits_font(struct sel *S, const char *s)
{
    if (S->fc.sys)
        return 1;
    int total = 0, have = 0;
    while (*s) {
        u32 c = utf8_next(&s);
        if (c == ' ')
            continue;
        total++;
        have += glf_gid(S->fc.glf, c) != 0;
    }
    return total == 0 || have * 5 >= total * 4;
}

int font_ascent(struct sel *S, int px) { return (int)((float)GLF_ASC(S->fc.glf) * (float)px / (float)GLF_UPEM(S->fc.glf)); }
