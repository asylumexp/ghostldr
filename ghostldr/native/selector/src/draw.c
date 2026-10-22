/* Software drawing on 32-bit BGRA surfaces (0xAARRGGBB). */
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

static int clip(const surface *s, int *x, int *y, int *w, int *h)
{
    if (*x < 0)
        *w += *x, *x = 0;
    if (*y < 0)
        *h += *y, *y = 0;
    if (*x + *w > s->w)
        *w = s->w - *x;
    if (*y + *h > s->h)
        *h = s->h - *y;
    return *w > 0 && *h > 0;
}

void draw_fill(surface *s, int x, int y, int w, int h, u32 argb)
{
    if (!clip(s, &x, &y, &w, &h))
        return;
    for (int j = 0; j < h; j++) {
        u32 *p = row(s, y + j) + x;
        for (int i = 0; i < w; i++)
            p[i] = argb;
    }
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

void draw_dim(surface *s, int x0, int x1, int y0, int y1, int alpha)
{
    x0 = clampi(x0, 0, s->w), x1 = clampi(x1, 0, s->w);
    y0 = clampi(y0, 0, s->h), y1 = clampi(y1, 0, s->h);
    u32 t = (u32)(256 - clampi(alpha, 0, 256));
    for (int y = y0; y < y1; y++) {
        u32 *p = row(s, y);
        for (int x = x0; x < x1; x++)
            p[x] = scale32(p[x], t);
    }
}

void draw_copy(surface *dst, const surface *src)
{
    int w = dst->w < src->w ? dst->w : src->w, h = dst->h < src->h ? dst->h : src->h;
    if (dst->stride == w && src->stride == w) {
        __builtin_memcpy(dst->px, src->px, (u64)w * (u64)h * 4);
        return;
    }
    for (int y = 0; y < h; y++)
        __builtin_memcpy(row(dst, y), src->px + (u64)y * (u64)src->stride, (u64)w * 4);
}

void draw_mix(surface *dst, const surface *a, const surface *b, int t256)
{
    u32 t = (u32)clampi(t256, 0, 256);
    for (int y = 0; y < dst->h; y++) {
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

/* src scaled to dw x dh at (dx, dy), blended with its alpha times alpha/256;
 * columns left of clip_x0 are not drawn */
void draw_scale(surface *dst, int dx, int dy, int dw, int dh, const surface *src, int alpha, int clip_x0)
{
    if (dw <= 0 || dh <= 0 || !src->px)
        return;
    int x0 = dx < clip_x0 ? clip_x0 : dx, y0 = dy, x1 = dx + dw, y1 = dy + dh;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > dst->w)
        x1 = dst->w;
    if (y1 > dst->h)
        y1 = dst->h;
    if (x0 >= x1 || y0 >= y1)
        return;
    u32 ga = (u32)clampi(alpha, 0, 256);
    int exact = dw == src->w && dh == src->h;
    s32 sx = (s32)(((s64)src->w << 16) / dw), sy = (s32)(((s64)src->h << 16) / dh);
    for (int y = y0; y < y1; y++) {
        u32 *d = row(dst, y);
        s32 fy = (y - dy) * sy + sy / 2 - 0x8000;
        const u32 *srow = src->px + (u64)(y - dy) * (u64)src->stride;
        for (int x = x0; x < x1; x++) {
            u32 c = exact ? srow[x - dx] : sample(src, (x - dx) * sx + sx / 2 - 0x8000, fy);
            u32 t = A(c);
            t += t >> 7;
            t = t * ga >> 8;
            d[x] = t >= 256 ? (c | 0xFF000000u) : over(d[x], c, t);
        }
    }
}

/* Resample src into all of dst: box filter when shrinking, bilinear otherwise. */
void draw_resample(surface *dst, const surface *src)
{
    if (src->w >= dst->w && src->h >= dst->h && (src->w > dst->w || src->h > dst->h)) {
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
        surface c = {src->px + (src->w - cw) / 2, cw, src->h, src->stride};
        draw_resample(dst, &c);
    } else {
        int ch = (int)((s64)src->w * dst->h / dst->w);
        surface c = {src->px + (u64)((src->h - ch) / 2) * (u64)src->stride, src->w, ch, src->stride};
        draw_resample(dst, &c);
    }
}

/* soft shadow around the rectangle (darkens a ring of the given radius) */
void draw_shadow(surface *s, int x, int y, int w, int h, int radius, int alpha)
{
    for (int yy = y - radius; yy < y + h + radius; yy++) {
        if (yy < 0 || yy >= s->h)
            continue;
        int dy = yy < y ? y - yy : yy >= y + h ? yy - (y + h - 1) : 0;
        u32 *p = row(s, yy);
        for (int xx = x - radius; xx < x + w + radius; xx++) {
            if (xx < 0 || xx >= s->w)
                continue;
            int dx = xx < x ? x - xx : xx >= x + w ? xx - (x + w - 1) : 0;
            if (!dx && !dy) {
                xx = x + w - 1; /* skip the inside */
                continue;
            }
            int d2 = dx * dx + dy * dy;
            if (d2 >= radius * radius)
                continue;
            float f = 1.0f - (float)d2 / (float)(radius * radius);
            int a = (int)((float)alpha * f * f);
            p[xx] = scale32(p[xx], (u32)(256 - a));
        }
    }
}

void draw_frame(surface *s, int x, int y, int w, int h, int t, u32 argb)
{
    draw_rect_blend(s, x - t, y - t, w + 2 * t, t, argb);
    draw_rect_blend(s, x - t, y + h, w + 2 * t, t, argb);
    draw_rect_blend(s, x - t, y, t, h, argb);
    draw_rect_blend(s, x + w, y, t, h, argb);
}

/* ---- anti-aliased shapes for the button symbols (small, per-pixel distance) ---- */

static float sqrtf_(float v)
{
    return __builtin_sqrtf(v);
}

static void plot(surface *s, int x, int y, u32 argb, float cov)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h || cov <= 0)
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

void draw_round_rect(surface *s, float x, float y, float w, float h, float r, float stroke, u32 argb)
{
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hx = w * 0.5f - r, hy = h * 0.5f - r;
    for (int py = (int)(y - 1); py <= (int)(y + h + 1); py++)
        for (int px = (int)(x - 1); px <= (int)(x + w + 1); px++) {
            float qx = (float)px + 0.5f - cx, qy = (float)py + 0.5f - cy;
            qx = (qx < 0 ? -qx : qx) - hx, qy = (qy < 0 ? -qy : qy) - hy;
            float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
            float d = sqrtf_(ox * ox + oy * oy) + (in < 0 ? in : 0) - r; /* < 0 inside */
            float e = d + stroke * 0.5f;
            plot(s, px, py, argb, stroke > 0 ? stroke * 0.5f + 0.5f - (e < 0 ? -e : e) : 0.5f - d);
        }
}

void draw_triangle_outline(surface *s, float cx, float cy, float r, float w, u32 argb)
{
    float ax = cx, ay = cy - r, bx = cx + r * 0.866f, by = cy + r * 0.5f, qx = cx - r * 0.866f;
    for (int y = (int)(cy - r - w - 1); y <= (int)(cy + r + w + 1); y++)
        for (int x = (int)(cx - r - w - 1); x <= (int)(cx + r + w + 1); x++) {
            float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
            float d = seg_dist(fx, fy, ax, ay, bx, by), d2 = seg_dist(fx, fy, bx, by, qx, by),
                  d3 = seg_dist(fx, fy, qx, by, ax, ay);
            if (d2 < d)
                d = d2;
            if (d3 < d)
                d = d3;
            plot(s, x, y, argb, w * 0.5f + 0.5f - d);
        }
}

void draw_square_outline(surface *s, float cx, float cy, float r, float w, u32 argb)
{
    draw_round_rect(s, cx - r, cy - r, 2 * r, 2 * r, 0.5f, w, argb);
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

/* A deep blue gradient with soft diagonal light bands, in the spirit of the
 * PS4's default theme (drawn here, nothing taken from the console). */
void draw_default_bg(surface *s)
{
    for (int y = 0; y < s->h; y++) {
        u32 *p = row(s, y);
        float fy = (float)y / (float)s->h;
        for (int x = 0; x < s->w; x++) {
            float fx = (float)x / (float)s->w;
            float band = 0;
            band += 0.5f + 0.5f * sinturn(fx * 0.9f - fy * 0.55f + 0.15f);
            band = band * band * band * 0.22f;
            float glow = 0.5f + 0.5f * sinturn(fx * 0.45f + fy * 0.35f + 0.6f);
            glow = glow * glow * 0.12f;
            float r = 6 + 10 * (1 - fy), g = 30 + 38 * (1 - fy), b = 74 + 70 * (1 - fy);
            float k = 1 + band + glow;
            int ri = (int)(r * k + 40 * band), gi = (int)(g * k + 70 * band), bi = (int)(b * k + 90 * band);
            p[x] = 0xFF000000u | (u32)clampi(ri, 0, 255) << 16 | (u32)clampi(gi, 0, 255) << 8 | (u32)clampi(bi, 0, 255);
        }
    }
}

/* tile used when a title has no icon0.png: a diagonal gradient of its tint */
void draw_fallback_tile(surface *s, u32 tint)
{
    int r = (int)R(tint), g = (int)G(tint), b = (int)B(tint);
    for (int y = 0; y < s->h; y++) {
        u32 *p = row(s, y);
        for (int x = 0; x < s->w; x++) {
            int t = (x + y) * 256 / (s->w + s->h); /* 0 top-left .. 256 bottom-right */
            int k = 300 - t * 3 / 4;               /* lighter to darker */
            p[x] = 0xFF000000u | (u32)clampi(r * k >> 8, 0, 255) << 16 | (u32)clampi(g * k >> 8, 0, 255) << 8 |
                   (u32)clampi(b * k >> 8, 0, 255);
        }
    }
}
