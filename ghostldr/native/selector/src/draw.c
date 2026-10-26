/* Software drawing on 32-bit BGRA surfaces (0xAARRGGBB).  Everything that
 * draws into a surface keeps to its row band (surf_y0..surf_y1): the UI
 * redraws only the rows that changed. */
#include "sel.h"

#define A(c) ((c) >> 24)
#define R(c) (((c) >> 16) & 0xFF)
#define G(c) (((c) >> 8) & 0xFF)
#define B(c) ((c)&0xFF)

static inline u32 *row(surface *s, int y) { return s->px + (u64)y * (u64)s->stride; }

/* a + (b - a) * t / 256 for all four channels */
static inline u32 lerp32(u32 a, u32 b, u32 t)
{
    u32 it = 256 - t;
    u32 rb = (((a & 0xFF00FFu) * it + (b & 0xFF00FFu) * t) >> 8) & 0xFF00FFu;
    u32 ga = (((a >> 8) & 0xFF00FFu) * it + ((b >> 8) & 0xFF00FFu) * t) & 0xFF00FF00u;
    return rb | ga;
}

/* blend colour c over dst with coverage t (0..256), result opaque */
static inline u32 over(u32 dst, u32 c, u32 t) { return lerp32(dst, c | 0xFF000000u, t) | 0xFF000000u; }

static inline u32 scale32(u32 c, u32 t) /* rgb * t / 256 */
{
    u32 rb = ((c & 0xFF00FFu) * t >> 8) & 0xFF00FFu;
    u32 g = ((c & 0xFF00u) * t >> 8) & 0xFF00u;
    return 0xFF000000u | rb | g;
}

u32 mix_rgb(u32 a, u32 b, int t256) { return lerp32(a | 0xFF000000u, b | 0xFF000000u, (u32)clampi(t256, 0, 256)); }

static int clip(const surface *s, int *x, int *y, int *w, int *h)
{
    int y0 = surf_y0(s), y1 = surf_y1(s);
    if (*x < 0)
        *w += *x, *x = 0;
    if (*y < y0)
        *h -= y0 - *y, *y = y0;
    if (*x + *w > s->w)
        *w = s->w - *x;
    if (*y + *h > y1)
        *h = y1 - *y;
    return *w > 0 && *h > 0;
}

void draw_rect_blend(surface *s, int x, int y, int w, int h, u32 argb)
{
    u32 t = A(argb);
    t += t >> 7;
    if (!t || !clip(s, &x, &y, &w, &h))
        return;
    for (int j = 0; j < h; j++) {
        u32 *p = row(s, y + j) + x;
        for (int i = 0; i < w; i++)
            p[i] = over(p[i], argb, t);
    }
}

/* top to bottom from one colour to another, each blended by its own alpha */
void draw_vgradient(surface *s, int x, int y, int w, int h, u32 top, u32 bottom)
{
    int x0 = x, y0 = y, w0 = w, h0 = h;
    if (h0 <= 0 || !clip(s, &x0, &y0, &w0, &h0))
        return;
    for (int j = 0; j < h0; j++) {
        int t = (y0 + j - y) * 256 / h;
        u32 c = lerp32(top, bottom, (u32)t), a = A(c);
        a += a >> 7;
        if (!a)
            continue;
        u32 *p = row(s, y0 + j) + x0;
        for (int i = 0; i < w0; i++)
            p[i] = over(p[i], c, a);
    }
}

void draw_dim(surface *s, int x0, int x1, int y0, int y1, int alpha)
{
    x0 = clampi(x0, 0, s->w), x1 = clampi(x1, 0, s->w);
    y0 = clampi(y0, surf_y0(s), surf_y1(s)), y1 = clampi(y1, surf_y0(s), surf_y1(s));
    u32 t = (u32)(256 - clampi(alpha, 0, 256));
    for (int y = y0; y < y1; y++) {
        u32 *p = row(s, y);
        for (int x = x0; x < x1; x++)
            p[x] = scale32(p[x], t);
    }
}

void draw_copy(surface *dst, const surface *src)
{
    int w = dst->w < src->w ? dst->w : src->w, y0 = surf_y0(dst), y1 = surf_y1(dst);
    if (y1 > src->h)
        y1 = src->h;
    if (dst->stride == w && src->stride == w) {
        __builtin_memcpy(row(dst, y0), src->px + (u64)y0 * (u64)w, (u64)w * (u64)(y1 - y0) * 4);
        return;
    }
    for (int y = y0; y < y1; y++)
        __builtin_memcpy(row(dst, y), src->px + (u64)y * (u64)src->stride, (u64)w * 4);
}

void draw_mix(surface *dst, const surface *a, const surface *b, int t256)
{
    u32 t = (u32)clampi(t256, 0, 256);
    for (int y = surf_y0(dst); y < surf_y1(dst); y++) {
        u32 *d = row(dst, y);
        const u32 *pa = a->px + (u64)y * (u64)a->stride, *pb = b->px + (u64)y * (u64)b->stride;
        for (int x = 0; x < dst->w; x++)
            d[x] = lerp32(pa[x], pb[x], t);
    }
}

/* Bilinear sample at 16.16 fixed point (clamped to the edges). */
static inline u32 sample(const surface *s, s32 fx, s32 fy)
{
    if (fx < 0)
        fx = 0;
    if (fy < 0)
        fy = 0;
    int x = fx >> 16, y = fy >> 16;
    u32 tx = (u32)(fx & 0xFFFF) >> 8, ty = (u32)(fy & 0xFFFF) >> 8;
    if (x >= s->w - 1)
        x = s->w - 1, tx = 0;
    if (y >= s->h - 1)
        y = s->h - 1, ty = 0;
    const u32 *p = s->px + (u64)y * (u64)s->stride + x;
    u32 a = p[0], b = tx ? p[1] : a;
    u32 c = ty ? p[s->stride] : a, d = ty ? (tx ? p[s->stride + 1] : c) : b;
    return lerp32(lerp32(a, b, tx), lerp32(c, d, tx), ty);
}

/* src scaled to dw x dh at (dx, dy), blended with its alpha times alpha/256
 * and its colour times bright/256; columns left of clip_x0 are not drawn */
void draw_scale(surface *dst, int dx, int dy, int dw, int dh, const surface *src, int alpha, int bright, int clip_x0)
{
    if (dw <= 0 || dh <= 0 || !src->px)
        return;
    int x0 = dx < clip_x0 ? clip_x0 : dx, y0 = dy, x1 = dx + dw, y1 = dy + dh;
    if (x0 < 0)
        x0 = 0;
    if (y0 < surf_y0(dst))
        y0 = surf_y0(dst);
    if (x1 > dst->w)
        x1 = dst->w;
    if (y1 > surf_y1(dst))
        y1 = surf_y1(dst);
    if (x0 >= x1 || y0 >= y1)
        return;
    u32 ga = (u32)clampi(alpha, 0, 256), br = (u32)clampi(bright, 0, 256);
    int exact = dw == src->w && dh == src->h;
    s32 sx = (s32)(((s64)src->w << 16) / dw), sy = (s32)(((s64)src->h << 16) / dh);
    for (int y = y0; y < y1; y++) {
        u32 *d = row(dst, y);
        s32 fy = (y - dy) * sy + sy / 2 - 0x8000;
        const u32 *srow = src->px + (u64)(y - dy) * (u64)src->stride;
        if (exact && ga == 256 && br == 256) { /* opaque art at its own size: a copy (the focused tile) */
            int opaque = 1;
            for (int x = x0; x < x1 && opaque; x++)
                opaque = A(srow[x - dx]) == 255;
            if (opaque) {
                __builtin_memcpy(d + x0, srow + (x0 - dx), (u64)(x1 - x0) * 4);
                continue;
            }
        }
        for (int x = x0; x < x1; x++) {
            u32 c = exact ? srow[x - dx] : sample(src, (x - dx) * sx + sx / 2 - 0x8000, fy);
            if (br < 256)
                c = (c & 0xFF000000u) | (scale32(c, br) & 0xFFFFFFu);
            u32 t = A(c);
            t += t >> 7;
            t = t * ga >> 8;
            d[x] = t >= 256 ? (c | 0xFF000000u) : over(d[x], c, t);
        }
    }
}

/* Resample src into all of dst: a copy at the same size, a box filter when
 * shrinking, bilinear otherwise. */
void draw_resample(surface *dst, const surface *src)
{
    if (src->w == dst->w && src->h == dst->h) {
        for (int y = 0; y < dst->h; y++)
            __builtin_memcpy(row(dst, y), src->px + (u64)y * (u64)src->stride, (u64)dst->w * 4);
        return;
    }
    if (src->w >= dst->w && src->h >= dst->h) {
        for (int y = 0; y < dst->h; y++) {
            int sy0 = (int)((s64)y * src->h / dst->h), sy1 = (int)((s64)(y + 1) * src->h / dst->h);
            if (sy1 <= sy0)
                sy1 = sy0 + 1;
            u32 *d = row(dst, y);
            for (int x = 0; x < dst->w; x++) {
                int sx0 = (int)((s64)x * src->w / dst->w), sx1 = (int)((s64)(x + 1) * src->w / dst->w);
                if (sx1 <= sx0)
                    sx1 = sx0 + 1;
                u32 acc[4] = {0, 0, 0, 0}, n = 0;
                for (int j = sy0; j < sy1; j++) {
                    const u32 *p = src->px + (u64)j * (u64)src->stride;
                    for (int i = sx0; i < sx1; i++, n++) {
                        u32 c = p[i];
                        acc[0] += B(c), acc[1] += G(c), acc[2] += R(c), acc[3] += A(c);
                    }
                }
                d[x] = (acc[3] / n) << 24 | (acc[2] / n) << 16 | (acc[1] / n) << 8 | (acc[0] / n);
            }
        }
        return;
    }
    s32 sx = (s32)(((s64)src->w << 16) / dst->w), sy = (s32)(((s64)src->h << 16) / dst->h);
    for (int y = 0; y < dst->h; y++) {
        u32 *d = row(dst, y);
        for (int x = 0; x < dst->w; x++)
            d[x] = sample(src, x * sx + sx / 2 - 0x8000, y * sy + sy / 2 - 0x8000);
    }
}

/* scale src to cover dst (keeping its aspect), centred and cropped */
void draw_cover(surface *dst, const surface *src)
{
    if ((s64)src->w * dst->h >= (s64)src->h * dst->w) {
        int cw = (int)((s64)src->h * dst->w / dst->h);
        surface c = {src->px + (src->w - cw) / 2, cw, src->h, src->stride, 0, 0};
        draw_resample(dst, &c);
    } else {
        int ch = (int)((s64)src->w * dst->h / dst->w);
        surface c = {src->px + (u64)((src->h - ch) / 2) * (u64)src->stride, src->w, ch, src->stride, 0, 0};
        draw_resample(dst, &c);
    }
}

/* Signed distance from (px, py) to a rounded rectangle centred at (cx, cy),
 * half extents hw x hh, corner radius r: negative inside. */
static inline float rr_dist(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    float qx = (px < cx ? cx - px : px - cx) - (hw - r), qy = (py < cy ? cy - py : py - cy) - (hh - r);
    if (qx > 0 && qy > 0)
        return __builtin_sqrtf(qx * qx + qy * qy) - r;
    return (qx > qy ? qx : qy) - r;
}

/* coverage 0..256 of a pixel whose centre is d outside an edge */
static inline u32 cover(float d) { return d <= -0.5f ? 256 : d >= 0.5f ? 0 : (u32)((0.5f - d) * 256.0f); }

/* A soft ring around the rounded rectangle (corner radius cr), towards rgb
 * (glow) or black (shadow): strength alpha/256 at the edge, fading out over
 * radius.  Beside the straight edges the distance is one subtraction; only
 * the corners need a square root. */
static void ring(surface *s, int x, int y, int w, int h, int cr, int radius, int alpha, u32 rgb, int light)
{
    if (radius <= 0 || alpha <= 0 || w <= 0 || h <= 0)
        return;
    int lut[97];
    if (radius > 95)
        radius = 95;
    cr = clampi(cr, 0, (w < h ? w : h) / 2);
    for (int d = 0; d <= radius; d++) {
        int f = 256 - d * 256 / radius;
        lut[d] = alpha * f / 256 * f / 256;
    }
    lut[radius] = 0;
    float cx = (float)x + (float)w * 0.5f, cy = (float)y + (float)h * 0.5f, hw = (float)w * 0.5f, hh = (float)h * 0.5f;
    int y0 = clampi(y - radius, surf_y0(s), surf_y1(s)), y1 = clampi(y + h + radius, surf_y0(s), surf_y1(s));
    int x0 = clampi(x - radius, 0, s->w), x1 = clampi(x + w + radius, 0, s->w);
    int sx0 = clampi(x + cr, x0, x1), sx1 = clampi(x + w - cr, x0, x1); /* the straight part of the top and bottom */
    for (int yy = y0; yy < y1; yy++) {
        u32 *p = row(s, yy);
        int dy = yy < y ? y - 1 - yy : yy >= y + h ? yy - (y + h) : -1; /* whole pixels outside, -1 inside */
        int full = yy >= y + cr && yy < y + h - cr;                       /* the shape spans [x, x + w) */
        for (int xx = x0; xx < x1; xx++) {
            int d;
            if (full) {
                if (xx >= x && xx < x + w) {
                    xx = x + w - 1; /* skip the inside */
                    continue;
                }
                d = xx < x ? x - 1 - xx : xx - (x + w);
            } else if (dy >= 0 && xx >= sx0 && xx < sx1) {
                d = dy;
            } else {
                float f = rr_dist((float)xx + 0.5f, (float)yy + 0.5f, cx, cy, hw, hh, (float)cr);
                if (f <= 0)
                    continue;
                d = (int)f;
            }
            if (d >= radius)
                continue;
            int a = lut[d];
            p[xx] = light ? over(p[xx], rgb, (u32)a) : scale32(p[xx], (u32)(256 - a));
        }
    }
}

void draw_shadow(surface *s, int x, int y, int w, int h, int cr, int radius, int alpha)
{
    ring(s, x, y, w, h, cr, radius, alpha, 0, 0);
}

void draw_halo(surface *s, int x, int y, int w, int h, int cr, int radius, int alpha, u32 rgb)
{
    ring(s, x, y, w, h, cr, radius, alpha, rgb, 1);
}

/* A filled rounded rectangle, anti-aliased, blended by argb's alpha. */
void draw_rrect(surface *s, int x, int y, int w, int h, int r, u32 argb)
{
    u32 a = A(argb);
    a += a >> 7;
    if (w <= 0 || h <= 0 || !a)
        return;
    r = clampi(r, 0, (w < h ? w : h) / 2);
    float cx = (float)x + (float)w * 0.5f, cy = (float)y + (float)h * 0.5f, hw = (float)w * 0.5f, hh = (float)h * 0.5f;
    int y0 = clampi(y, surf_y0(s), surf_y1(s)), y1 = clampi(y + h, surf_y0(s), surf_y1(s));
    int x0 = clampi(x, 0, s->w), x1 = clampi(x + w, 0, s->w);
    for (int yy = y0; yy < y1; yy++) {
        u32 *p = row(s, yy);
        int e = yy < y + r || yy >= y + h - r ? r : 0; /* columns at each end with their own coverage */
        for (int xx = x0; xx < x1; xx++) {
            u32 t = a;
            if (xx < x + e || xx >= x + w - e)
                t = cover(rr_dist((float)xx + 0.5f, (float)yy + 0.5f, cx, cy, hw, hh, (float)r)) * a >> 8;
            if (t)
                p[xx] = over(p[xx], argb, t);
        }
    }
}

/* The outline of a rounded rectangle, t pixels wide inside its edge.  Only
 * the corners need their coverage worked out per pixel. */
void draw_rring(surface *s, int x, int y, int w, int h, int r, int t, u32 argb)
{
    u32 a = A(argb);
    a += a >> 7;
    if (w <= 0 || h <= 0 || t <= 0 || !a)
        return;
    r = clampi(r, 0, (w < h ? w : h) / 2);
    int ri = r > t ? r - t : 0, inner = w > 2 * t && h > 2 * t, e = (r > t ? r : t) + 1;
    float cx = (float)x + (float)w * 0.5f, cy = (float)y + (float)h * 0.5f, hw = (float)w * 0.5f, hh = (float)h * 0.5f;
    int y0 = clampi(y, surf_y0(s), surf_y1(s)), y1 = clampi(y + h, surf_y0(s), surf_y1(s));
    int x0 = clampi(x, 0, s->w), x1 = clampi(x + w, 0, s->w);
    for (int yy = y0; yy < y1; yy++) {
        u32 *p = row(s, yy);
        if (yy >= y + e && yy < y + h - e) { /* beside the straight sides: t pixels at each end */
            for (int xx = x0; xx < x1; xx++) {
                if (xx == x + t && x + w - t > xx)
                    xx = x + w - t;
                p[xx] = over(p[xx], argb, a);
            }
            continue;
        }
        int band = yy < y + t || yy >= y + h - t; /* between the corners, all of the top or bottom, or nothing */
        for (int xx = x0; xx < x1; xx++) {
            u32 c = a;
            if (xx < x + e || xx >= x + w - e) {
                float px = (float)xx + 0.5f, py = (float)yy + 0.5f;
                u32 co = cover(rr_dist(px, py, cx, cy, hw, hh, (float)r));
                u32 ci = inner ? cover(rr_dist(px, py, cx, cy, hw - (float)t, hh - (float)t, (float)ri)) : 0;
                c = (co > ci ? co - ci : 0) * a >> 8;
            } else if (!band) {
                xx = x + w - e - 1;
                continue;
            }
            if (c)
                p[xx] = over(p[xx], argb, c);
        }
    }
}

/* Corners cut round (alpha) in a picture of its own, such as a tile. */
void draw_round_corners(surface *s, int r)
{
    r = clampi(r, 0, (s->w < s->h ? s->w : s->h) / 2);
    float cx = (float)s->w * 0.5f, cy = (float)s->h * 0.5f;
    for (int y = 0; y < s->h; y++) {
        if (y == r && s->h - r > r)
            y = s->h - r; /* only the rows with corners */
        u32 *p = row(s, y);
        for (int x = 0; x < s->w; x++) {
            if (x == r && s->w - r > r)
                x = s->w - r;
            u32 c = cover(rr_dist((float)x + 0.5f, (float)y + 0.5f, cx, cy, cx, cy, (float)r));
            p[x] = (A(p[x]) * c >> 8) << 24 | (p[x] & 0xFFFFFFu);
        }
    }
}

/* Every pixel's colour towards argb's by its alpha; each keeps its own alpha. */
void draw_wash(surface *s, u32 argb)
{
    u32 t = A(argb);
    t += t >> 7;
    for (int y = 0; y < s->h; y++) {
        u32 *p = row(s, y);
        for (int x = 0; x < s->w; x++)
            p[x] = (p[x] & 0xFF000000u) | (lerp32(p[x], argb, t) & 0xFFFFFFu);
    }
}

/* ---- anti-aliased shapes for the button symbols (small, per-pixel distance) ---- */

static float sqrtf_(float v)
{
    return __builtin_sqrtf(v);
}

static void plot(surface *s, int x, int y, u32 argb, float cov)
{
    if (x < 0 || y < surf_y0(s) || x >= s->w || y >= surf_y1(s) || cov <= 0)
        return;
    u32 t = (u32)(clampf(cov, 0, 1) * (float)(A(argb) + (A(argb) >> 7)));
    u32 *p = row(s, y) + x;
    *p = over(*p, argb, t);
}

void draw_disc(surface *s, float cx, float cy, float r, float stroke, u32 argb)
{
    for (int y = (int)(cy - r - 2); y <= (int)(cy + r + 2); y++)
        for (int x = (int)(cx - r - 2); x <= (int)(cx + r + 2); x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float d = sqrtf_(dx * dx + dy * dy);
            float cov = stroke > 0 ? (stroke * 0.5f + 0.5f) - (d > r - stroke * 0.5f ? d - (r - stroke * 0.5f) : (r - stroke * 0.5f) - d)
                                   : r + 0.5f - d;
            plot(s, x, y, argb, cov);
        }
}

static float seg_dist(float px, float py, float x0, float y0, float x1, float y1)
{
    float vx = x1 - x0, vy = y1 - y0, wx = px - x0, wy = py - y0;
    float l2 = vx * vx + vy * vy;
    float t = l2 > 0 ? clampf((wx * vx + wy * vy) / l2, 0, 1) : 0;
    float dx = wx - t * vx, dy = wy - t * vy;
    return sqrtf_(dx * dx + dy * dy);
}

void draw_line(surface *s, float x0, float y0, float x1, float y1, float w, u32 argb)
{
    float lx = x0 < x1 ? x0 : x1, hx = x0 < x1 ? x1 : x0, ly = y0 < y1 ? y0 : y1, hy = y0 < y1 ? y1 : y0;
    for (int y = (int)(ly - w - 1); y <= (int)(hy + w + 1); y++)
        for (int x = (int)(lx - w - 1); x <= (int)(hx + w + 1); x++)
            plot(s, x, y, argb, w * 0.5f + 0.5f - seg_dist((float)x + 0.5f, (float)y + 0.5f, x0, y0, x1, y1));
}

/* A warning sign: a triangle with round corners in fill, an exclamation
 * mark in ink; size wide, centred on (cx, cy). */
void draw_warning(surface *s, float cx, float cy, float size, u32 fill, u32 ink)
{
    float hgt = size * 0.866f, top = cy - hgt * 0.5f, bot = cy + hgt * 0.5f;
    /* the corners are rounded by drawing the triangle shrunk by rr towards its
     * incentre, then everything within rr of it */
    float inr = hgt / 3.0f, rr = size * 0.08f, k = (inr - rr) / inr, iy = bot - inr;
    float ax = cx, ay = iy + (top - iy) * k, bx = cx + size * 0.5f * k, by = iy + (bot - iy) * k, qx = cx - size * 0.5f * k;
    int x0 = (int)(cx - size * 0.5f) - 1, x1 = (int)(cx + size * 0.5f) + 2;
    int y0 = (int)top - 1, y1 = (int)bot + 2;
    for (int y = y0 < surf_y0(s) ? surf_y0(s) : y0; y < y1 && y < surf_y1(s); y++)
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float d = seg_dist(px, py, ax, ay, bx, by), d2 = seg_dist(px, py, bx, by, qx, by),
                  d3 = seg_dist(px, py, qx, by, ax, ay);
            d = d2 < d ? d2 : d, d = d3 < d ? d3 : d;
            /* inside: on the inner side of all three edges */
            int in = py <= by && (bx - ax) * (py - ay) - (by - ay) * (px - ax) >= 0 &&
                     (ax - qx) * (py - by) - (ay - by) * (px - qx) >= 0;
            plot(s, x, y, fill, 0.5f - ((in ? -d : d) - rr));
        }
    float bw = size * 0.095f;
    draw_line(s, cx, top + hgt * 0.36f, cx, top + hgt * 0.63f, bw, ink);
    draw_disc(s, cx, top + hgt * 0.79f, bw * 0.62f, 0, ink);
}

/* ghostldr's mark: a little ghost, size wide, centred on (cx, cy). */
void draw_ghost(surface *s, int cx, int cy, int size, u32 body, u32 eyes)
{
    int r = size / 2, hy = cy - size / 8, by = cy + size * 3 / 8 - r / 3;
    draw_disc(s, (float)cx, (float)hy, (float)r, 0, body);
    draw_rrect(s, cx - r, hy, 2 * r, by - hy, 0, body);
    for (int i = -1; i <= 1; i++) /* the hem */
        draw_disc(s, (float)cx + (float)(i * 2 * r) / 3.0f, (float)by, (float)r / 3.0f, 0, body);
    draw_disc(s, (float)cx - (float)r * 0.36f, (float)hy + (float)r * 0.12f, (float)size * 0.085f, 0, eyes);
    draw_disc(s, (float)cx + (float)r * 0.36f, (float)hy + (float)r * 0.12f, (float)size * 0.085f, 0, eyes);
}

/* ---- generated art (no images from the console needed) ---- */

/* sine for t in turns (period 1), good to ~0.1% */
static float sinturn(float t)
{
    t -= (float)(int)t;
    if (t < 0)
        t += 1;
    float x = t < 0.5f ? t * 4 - 1 : 3 - t * 4; /* triangle -1..1 */
    return x * (1.5f - 0.5f * x * x);           /* smooth it */
}

static inline u32 rgbf(float r, float g, float b)
{
    return 0xFF000000u | (u32)clampi((int)r, 0, 255) << 16 | (u32)clampi((int)g, 0, 255) << 8 |
           (u32)clampi((int)b, 0, 255);
}

/* A deep, dark backdrop lit in the title's colour: a broad glow from the
 * top right, a fainter one in a neighbouring hue from the bottom left, and
 * soft diagonal light bands, in the spirit of the consoles' own themes
 * (drawn here: nothing is taken from the console).  A little noise keeps
 * the dark gradients from banding. */
void draw_generated_bg(surface *s, u32 tint)
{
    float tr = (float)R(tint), tg = (float)G(tint), tb = (float)B(tint);
    float hr = tg * 0.55f + tb * 0.45f, hg = tb * 0.55f + tr * 0.45f, hb = tr * 0.55f + tg * 0.45f; /* the next hue */
    float aspect = (float)s->w / (float)s->h;
    for (int y = 0; y < s->h; y++) {
        u32 *p = row(s, y);
        float fy = (float)y / (float)s->h;
        for (int x = 0; x < s->w; x++) {
            float fx = (float)x / (float)s->w;
            float gx = (fx - 0.80f) * aspect, gy = fy - 0.10f;
            float g1 = 1.0f - (gx * gx + gy * gy) * 0.55f;
            g1 = g1 > 0 ? g1 * g1 * g1 : 0;
            float lx = (fx - 0.08f) * aspect, ly = fy - 1.05f;
            float g2 = 1.0f - (lx * lx + ly * ly) * 0.9f;
            g2 = g2 > 0 ? g2 * g2 : 0;
            float band = 0.5f + 0.5f * sinturn(fx * 0.8f - fy * 0.5f + 0.18f);
            band = band * band * band * band * 0.10f;
            float band2 = 0.5f + 0.5f * sinturn(fx * 1.6f - fy * 0.85f + 0.6f);
            band2 = band2 * band2 * band2 * band2 * band2 * band2 * 0.05f;
            float k = 0.10f + 0.62f * g1 + band + band2, k2 = 0.30f * g2;
            u32 h = (u32)x * 2654435761u ^ (u32)y * 2246822519u;
            float n = (float)((h >> 13) & 255) / 255.0f - 0.5f; /* dither */
            p[x] = rgbf(tr * k + hr * k2 + 8 + 26 * (band + band2) + n, tg * k + hg * k2 + 10 + 30 * (band + band2) + n,
                        tb * k + hb * k2 + 16 + 36 * (band + band2) + n);
        }
    }
}

/* A tile without a picture: a diagonal gradient of the title's colour with a
 * soft sheen; the UI writes the name on it. */
void draw_fallback_tile(surface *s, u32 tint)
{
    float tr = (float)R(tint), tg = (float)G(tint), tb = (float)B(tint);
    for (int y = 0; y < s->h; y++) {
        u32 *p = row(s, y);
        float fy = (float)y / (float)s->h;
        for (int x = 0; x < s->w; x++) {
            float fx = (float)x / (float)s->w;
            float k = 1.10f - 0.62f * (fx * 0.45f + fy * 0.55f); /* lighter top left, darker bottom right */
            float sx = fx - 0.92f, sy = fy + 0.05f;
            float sheen = 1.0f - (sx * sx + sy * sy) * 1.5f;
            sheen = sheen > 0 ? sheen * sheen * 0.22f : 0;
            float stripe = 0.5f + 0.5f * sinturn((fx + fy) * 2.2f);
            stripe = stripe * stripe * stripe * stripe * 0.05f;
            float add = (sheen + stripe) * 255;
            p[x] = rgbf(tr * k + add, tg * k + add, tb * k + add);
        }
    }
}

/* The backdrop's tone, after the PS4 home screen: dimmed overall, darker at
 * the top (status bar), towards the bottom and on the left, where the text
 * is.  Applied once when a backdrop is prepared. */
void draw_shade(surface *s)
{
    u16 col[SCREEN_W];
    int w = s->w < SCREEN_W ? s->w : SCREEN_W;
    for (int x = 0; x < w; x++) {
        float f = x < 1150 ? 1.0f - (float)x / 1150.0f : 0;
        col[x] = (u16)(256 * (1.0f - 0.48f * f * f));
    }
    for (int y = 0; y < s->h; y++) {
        float top = y < 230 ? 1.0f - (float)y / 230.0f : 0;
        float bot = y > 520 ? (float)(y - 520) / (float)(s->h - 520) : 0;
        bot = bot * bot * (3 - 2 * bot);
        float k = 0.60f * (1.0f - 0.45f * top * top) * (1.0f - 0.66f * bot);
        u32 kr = (u32)(k * 256);
        u32 *p = row(s, y);
        for (int x = 0; x < w; x++)
            p[x] = scale32(p[x], kr * col[x] >> 8);
    }
}
