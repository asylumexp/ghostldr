/*
 * The software OpenGL ES 2.0's drawing (gles.h): shader recognition, pixel
 * format conversion and the rasteriser.  No console calls here, so the file
 * builds for this machine too (tests/test_gles.py).
 *
 * Rules followed, as a GPU does them:
 *   - a triangle covers the pixels whose centres are inside it; a centre on
 *     an edge belongs to the triangle on the edge's right (half-open spans
 *     [xl, xr) and rows [ytop, ybottom)), so two triangles sharing an edge
 *     never both draw a pixel and leave no gap;
 *   - attributes are interpolated at pixel centres (affine: every caller of
 *     this renderer draws in 2D, w = 1);
 *   - GL_NEAREST picks the texel under the coordinate, GL_LINEAR blends the
 *     four around it, CLAMP_TO_EDGE and REPEAT wrap;
 *   - blending is glBlendFuncSeparate/glBlendEquationSeparate's, in 8 bits
 *     per channel with correct rounding.
 */
#include "gles.h"

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wfloat-conversion"
#endif

/* ---- small maths (no libm) ---- */

static inline s32 ifloor(float f)
{
    s32 i = (s32)f;
    return (f < (float)i) ? i - 1 : i;
}

static inline s32 iceil(float f)
{
    s32 i = (s32)f;
    return (f > (float)i) ? i + 1 : i;
}

static inline u32 clamp255(s32 v) { return v < 0 ? 0 : v > 255 ? 255 : (u32)v; }

/* x * y / 255, rounded */
static inline u32 mul255(u32 x, u32 y)
{
    u32 t = x * y + 128;
    return (t + (t >> 8)) >> 8;
}

static inline u32 f2u8(float f)
{
    if (!(f > 0.0f))
        return 0;
    if (f >= 1.0f)
        return 255;
    return (u32)(f * 255.0f + 0.5f);
}

float gles_sinf(float x)
{
    /* reduce to [-pi, pi], then a 7th-order odd polynomial on [-pi/2, pi/2] */
    const float pi = 3.14159265358979f, twopi = 6.28318530717959f;
    float k = (float)ifloor(x / twopi + 0.5f);
    x -= k * twopi;
    if (x > pi * 0.5f)
        x = pi - x;
    else if (x < -pi * 0.5f)
        x = -pi - x;
    float x2 = x * x;
    return x * (1.0f - x2 * (1.0f / 6 - x2 * (1.0f / 120 - x2 * (1.0f / 5040 - x2 / 362880))));
}

float gles_cosf(float x) { return gles_sinf(x + 1.57079632679490f); }

/* ---- shader recognition ---- */

/* the source without whitespace, to match statements however they are laid out */
static u32 squeeze(const char *src, char *out, u32 size)
{
    u32 n = 0;
    for (const char *c = src; *c && n + 1 < size; c++)
        if (*c != ' ' && *c != '\t' && *c != '\n' && *c != '\r')
            out[n++] = *c;
    out[n] = 0;
    return n;
}

static void say(char *why, u32 size, const char *what)
{
    if (why && size)
        strlcpy(why, what, size);
}

#define SQ_MAX 8192

int gles_vs_parse(const char *src, struct gles_vs *out, char *why, u32 why_size)
{
    static char sq[SQ_MAX];
    memset(out, 0, sizeof(*out));
    if (!src || squeeze(src, sq, sizeof(sq)) + 1 >= sizeof(sq)) {
        say(why, why_size, "ghostldr GLES: vertex shader missing or too long");
        return -1;
    }
    if (!strstr(sq, "gl_Position") || !strstr(sq, "u_projection") || !strstr(sq, "a_position")) {
        say(why, why_size, "ghostldr GLES: only SDL2-style 2D vertex shaders (u_projection * a_position) are "
                           "supported, and this is not one");
        return -1;
    }
    if (strstr(sq, "v_texCoord=a_texCoord"))
        out->flags |= VSF_TEXCOORD;
    if (strstr(sq, "v_color=a_color"))
        out->flags |= VSF_COLOR;
    if (strstr(sq, "a_angle") && strstr(sq, "a_center"))
        out->flags |= VSF_ROTATE;
    return 0;
}

/* a decimal number at *s (as GLSL writes constants), advancing *s */
static float number(const char **s)
{
    const char *p = *s;
    float sign = 1.0f, v = 0.0f, scale = 1.0f;
    if (*p == '-')
        sign = -1.0f, p++;
    else if (*p == '+')
        p++;
    while (*p >= '0' && *p <= '9')
        v = v * 10.0f + (float)(*p++ - '0');
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            scale *= 0.1f;
            v += (float)(*p++ - '0') * scale;
        }
    }
    if (*p == 'f' || *p == 'F')
        p++;
    *s = p;
    return sign * v;
}

/* n comma-separated numbers after "name(" in the squeezed source */
static bool numbers(const char *sq, const char *name, float *out, u32 n)
{
    const char *p = strstr(sq, name);
    if (!p)
        return false;
    p += strlen(name);
    for (u32 i = 0; i < n; i++) {
        const char *was = p;
        out[i] = number(&p);
        if (p == was)
            return false;
        if (*p != (i + 1 < n ? ',' : ')'))
            return false;
        p++;
    }
    return true;
}

static bool chan(char c) { return c == 'r' || c == 'g' || c == 'b' || c == 'a'; }

int gles_fs_parse(const char *src, struct gles_fs *out, char *why, u32 why_size)
{
    static char sq[SQ_MAX];
    memset(out, 0, sizeof(*out));
    if (!src || squeeze(src, sq, sizeof(sq)) + 1 >= sizeof(sq)) {
        say(why, why_size, "ghostldr GLES: fragment shader missing or too long");
        return -1;
    }
    if (!strstr(sq, "gl_FragColor")) {
        say(why, why_size, "ghostldr GLES: fragment shader writes no gl_FragColor");
        return -1;
    }
    if (strstr(sq, "gl_FragColor*=v_color"))
        out->flags |= FSF_MUL_VCOLOR;
    if (strstr(sq, "gl_FragColor*=u_color") || strstr(sq, "gl_FragColor=u_color"))
        out->flags |= FSF_MUL_UCOLOR;
    if (strstr(sq, "u_texture_u")) {
        const char *nv = strstr(sq, "yuv.yz=texture2D(u_texture_u,v_texCoord).");
        if (nv) {
            nv += strlen("yuv.yz=texture2D(u_texture_u,v_texCoord).");
            if (!chan(nv[0]) || !chan(nv[1]) || nv[2] != ';') {
                say(why, why_size, "ghostldr GLES: NV12/NV21 shader with an unknown swizzle");
                return -1;
            }
            out->kind = FS_NV;
            out->uv_swz[0] = nv[0], out->uv_swz[1] = nv[1];
        } else if (strstr(sq, "yuv.y=texture2D(u_texture_u,v_texCoord).r;") &&
                   strstr(sq, "yuv.z=texture2D(u_texture_v,v_texCoord).r;")) {
            out->kind = FS_YUV;
        } else {
            say(why, why_size, "ghostldr GLES: YUV shader of an unknown layout");
            return -1;
        }
        if (!numbers(sq, "offset=vec3(", out->offset, 3) || !numbers(sq, "matrix=mat3(", out->mat, 9) ||
            !strstr(sq, "rgb=matrix*yuv;")) {
            say(why, why_size, "ghostldr GLES: YUV shader without the offset and matrix constants");
            return -1;
        }
        return 0;
    }
    if (strstr(sq, "texture2D(u_texture,v_texCoord)")) {
        out->kind = FS_TEX;
        if (strstr(sq, "gl_FragColor.r=abgr.b;") && strstr(sq, "gl_FragColor.b=abgr.r;"))
            out->flags |= FSF_SWAP_RB;
        if (strstr(sq, "gl_FragColor.a=1.0;"))
            out->flags |= FSF_ALPHA_ONE;
        return 0;
    }
    if (strstr(sq, "texture2D")) {
        say(why, why_size, "ghostldr GLES: texture lookup of an unknown form");
        return -1;
    }
    if (strstr(sq, "gl_FragColor=v_color;") || (out->flags & FSF_MUL_UCOLOR)) {
        out->kind = FS_SOLID;
        return 0;
    }
    say(why, why_size, "ghostldr GLES: fragment shader of an unknown form (supported: SDL2's GLES2 renderer shaders)");
    return -1;
}

/* ---- pixel formats ---- */

static inline u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | g << 8 | b << 16 | a << 24; }

int gles_convert(u32 *dst, u32 dst_pitch, const void *src, u32 w, u32 h, u32 format, u32 type, u32 align)
{
    u32 bpp;
    if (type == GL_UNSIGNED_BYTE)
        bpp = format == GL_RGBA || format == GL_BGRA_EXT ? 4
              : format == GL_RGB                          ? 3
              : format == GL_LUMINANCE_ALPHA              ? 2
              : format == GL_LUMINANCE || format == GL_ALPHA ? 1
                                                             : 0;
    else if (type == GL_UNSIGNED_SHORT_5_6_5)
        bpp = format == GL_RGB ? 2 : 0;
    else if (type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1)
        bpp = format == GL_RGBA ? 2 : 0;
    else
        return GL_INVALID_ENUM;
    if (!bpp)
        return GL_INVALID_OPERATION;
    if (align != 1 && align != 2 && align != 4 && align != 8)
        align = 4;
    u64 pitch = ((u64)w * bpp + align - 1) & ~(u64)(align - 1);
    for (u32 y = 0; y < h; y++) {
        const u8 *s = (const u8 *)src + pitch * y;
        u32 *d = dst + (u64)dst_pitch * y;
        if (format == GL_RGBA && type == GL_UNSIGNED_BYTE) {
            memcpy(d, s, (u64)w * 4);
            continue;
        }
        for (u32 x = 0; x < w; x++) {
            u32 v;
            switch (bpp == 2 && type != GL_UNSIGNED_BYTE ? type : format) {
            case GL_BGRA_EXT: v = rgba(s[2], s[1], s[0], s[3]), s += 4; break;
            case GL_RGB: v = rgba(s[0], s[1], s[2], 255), s += 3; break;
            case GL_LUMINANCE_ALPHA: v = rgba(s[0], s[0], s[0], s[1]), s += 2; break;
            case GL_LUMINANCE: v = rgba(s[0], s[0], s[0], 255), s += 1; break;
            case GL_ALPHA: v = rgba(0, 0, 0, s[0]), s += 1; break;
            case GL_UNSIGNED_SHORT_5_6_5: {
                u32 p = s[0] | (u32)s[1] << 8;
                s += 2;
                u32 r = p >> 11, g = (p >> 5) & 63, b = p & 31;
                v = rgba(r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255);
                break;
            }
            case GL_UNSIGNED_SHORT_4_4_4_4: {
                u32 p = s[0] | (u32)s[1] << 8;
                s += 2;
                v = rgba((p >> 12) * 17, ((p >> 8) & 15) * 17, ((p >> 4) & 15) * 17, (p & 15) * 17);
                break;
            }
            case GL_UNSIGNED_SHORT_5_5_5_1: {
                u32 p = s[0] | (u32)s[1] << 8;
                s += 2;
                u32 r = p >> 11, g = (p >> 6) & 31, b = (p >> 1) & 31;
                v = rgba(r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, (p & 1) * 255);
                break;
            }
            default: return GL_INVALID_OPERATION;
            }
            d[x] = v;
        }
    }
    return 0;
}

/* ---- textures ---- */

static inline s32 wrap(s32 i, u32 n, u32 mode)
{
    if (mode == GL_REPEAT) {
        i %= (s32)n;
        return i < 0 ? i + (s32)n : i;
    }
    if (mode == GL_MIRRORED_REPEAT) {
        s32 p = (s32)n * 2;
        i %= p;
        if (i < 0)
            i += p;
        return i >= (s32)n ? p - 1 - i : i;
    }
    return i < 0 ? 0 : i >= (s32)n ? (s32)n - 1 : i;
}

static inline u32 texel(const struct gles_tex *t, s32 x, s32 y)
{
    return t->pix[(u64)wrap(y, t->h, t->wrap_t) * t->pitch + (u32)wrap(x, t->w, t->wrap_s)];
}

static inline u32 lerp4(u32 a, u32 b, u32 f) /* f 0..256 */
{
    u32 ag = (a >> 8) & 0x00FF00FF, ar = a & 0x00FF00FF;
    u32 bg = (b >> 8) & 0x00FF00FF, br = b & 0x00FF00FF;
    u32 rg = (ar * (256 - f) + br * f) >> 8 & 0x00FF00FF;
    u32 gg = (ag * (256 - f) + bg * f) & 0xFF00FF00;
    return rg | gg;
}

/* Texel coordinates are taken as the GPU's texture unit takes them: to the
 * nearest 1/256 of a texel.  A pixel centre on a texel edge (text scaled by
 * 1.5) then picks the texel GL's rule names, as the GPU does, rather than
 * the one a float rounding error falls into (measured on a PS4: they
 * differed at every such edge); nearest is floor(t + SUBTEXEL). */
#define SUBTEXEL (1.0f / 512.0f)

static inline s32 q8(float f) /* f to 1/256ths, rounded */
{
    return ifloor(f * 256.0f + 0.5f);
}

u32 gles_sample(const struct gles_tex *t, float u, float v, bool linear)
{
    if (!t || !t->pix || !t->w || !t->h)
        return rgba(0, 0, 0, 255); /* an incomplete texture samples as black */
    if (!linear)
        return texel(t, ifloor(u * (float)t->w + SUBTEXEL), ifloor(v * (float)t->h + SUBTEXEL));
    s32 qx = q8(u * (float)t->w - 0.5f), qy = q8(v * (float)t->h - 0.5f);
    s32 x0 = qx >> 8, y0 = qy >> 8;
    u32 ax = (u32)qx & 255, ay = (u32)qy & 255;
    u32 top = lerp4(texel(t, x0, y0), texel(t, x0 + 1, y0), ax);
    u32 bot = lerp4(texel(t, x0, y0 + 1), texel(t, x0 + 1, y0 + 1), ax);
    return lerp4(top, bot, ay);
}

static inline bool tex_linear(const struct gles_tex *t, bool magnify)
{
    u32 f = magnify ? t->mag_filter : t->min_filter;
    /* mipmapped minification filters: level 0 only (no mipmaps here) */
    return f == GL_LINEAR || f == 0x2701 /* LINEAR_MIPMAP_NEAREST */ || f == 0x2703 /* LINEAR_MIPMAP_LINEAR */;
}

/* ---- fragments ---- */

static inline u32 chan_of(u32 px, char c)
{
    return c == 'r' ? px & 255 : c == 'g' ? (px >> 8) & 255 : c == 'b' ? (px >> 16) & 255 : px >> 24;
}

static u32 yuv_shade(const struct gles_draw *d, float u, float v, bool linear)
{
    const struct gles_fs *fs = d->fs;
    float y = (float)(gles_sample(d->tex[0], u, v, linear) & 255) / 255.0f, cb, cr;
    if (fs->kind == FS_NV) {
        u32 t = gles_sample(d->tex[1], u, v, linear);
        cb = (float)chan_of(t, fs->uv_swz[0]) / 255.0f;
        cr = (float)chan_of(t, fs->uv_swz[1]) / 255.0f;
    } else {
        cb = (float)(gles_sample(d->tex[1], u, v, linear) & 255) / 255.0f;
        cr = (float)(gles_sample(d->tex[2], u, v, linear) & 255) / 255.0f;
    }
    y += fs->offset[0], cb += fs->offset[1], cr += fs->offset[2];
    const float *m = fs->mat; /* column-major */
    float r = m[0] * y + m[3] * cb + m[6] * cr;
    float g = m[1] * y + m[4] * cb + m[7] * cr;
    float b = m[2] * y + m[5] * cb + m[8] * cr;
    return rgba(f2u8(r), f2u8(g), f2u8(b), 255);
}

/* the blend factor f for one channel: s, dc the source and destination of
 * that channel, sa, da their alphas */
static inline u32 factor(u32 f, u32 s, u32 dc, u32 sa, u32 da, bool alpha)
{
    switch (f) {
    case GL_ZERO: return 0;
    case GL_ONE: return 255;
    case GL_SRC_COLOR: return s;
    case GL_ONE_MINUS_SRC_COLOR: return 255 - s;
    case GL_SRC_ALPHA: return sa;
    case GL_ONE_MINUS_SRC_ALPHA: return 255 - sa;
    case GL_DST_ALPHA: return da;
    case GL_ONE_MINUS_DST_ALPHA: return 255 - da;
    case GL_DST_COLOR: return dc;
    case GL_ONE_MINUS_DST_COLOR: return 255 - dc;
    case GL_SRC_ALPHA_SATURATE: return alpha ? 255 : sa < 255 - da ? sa : 255 - da;
    default: return 255;
    }
}

static inline u32 combine(u32 eq, u32 s, u32 fs, u32 dc, u32 fd)
{
    switch (eq) {
    case GL_FUNC_SUBTRACT: return clamp255((s32)mul255(s, fs) - (s32)mul255(dc, fd));
    case GL_FUNC_REVERSE_SUBTRACT: return clamp255((s32)mul255(dc, fd) - (s32)mul255(s, fs));
    case GL_MIN: return s < dc ? s : dc;
    case GL_MAX: return s > dc ? s : dc;
    default: return clamp255((s32)(mul255(s, fs) + mul255(dc, fd)));
    }
}

/* the general blend: src and dst RGBA8 */
static u32 blend_any(const struct gles_draw *d, u32 s, u32 t)
{
    u32 sc[4] = {s & 255, (s >> 8) & 255, (s >> 16) & 255, s >> 24};
    u32 dc[4] = {t & 255, (t >> 8) & 255, (t >> 16) & 255, t >> 24};
    u32 o[4];
    for (u32 k = 0; k < 3; k++)
        o[k] = combine(d->eq_rgb, sc[k], factor(d->src_rgb, sc[k], dc[k], sc[3], dc[3], false), dc[k],
                       factor(d->dst_rgb, sc[k], dc[k], sc[3], dc[3], false));
    o[3] = combine(d->eq_a, sc[3], factor(d->src_a, sc[3], dc[3], sc[3], dc[3], true), dc[3],
                   factor(d->dst_a, sc[3], dc[3], sc[3], dc[3], true));
    return rgba(o[0], o[1], o[2], o[3]);
}

/* the usual "over" blend, SRC_ALPHA / ONE_MINUS_SRC_ALPHA / ONE / ONE_MINUS_SRC_ALPHA, ADD */
static inline u32 blend_over(u32 s, u32 t)
{
    u32 a = s >> 24;
    if (a == 255)
        return s;
    if (a == 0)
        return t;
    u32 ia = 255 - a;
    u32 r = mul255(s & 255, a) + mul255(t & 255, ia);
    u32 g = mul255((s >> 8) & 255, a) + mul255((t >> 8) & 255, ia);
    u32 b = mul255((s >> 16) & 255, a) + mul255((t >> 16) & 255, ia);
    u32 oa = a + mul255(t >> 24, ia);
    return rgba(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b, oa > 255 ? 255 : oa);
}

enum { BL_NONE, BL_OVER, BL_ANY };

static u32 blend_kind(const struct gles_draw *d)
{
    if (!d->blend)
        return BL_NONE;
    if (d->src_rgb == GL_SRC_ALPHA && d->dst_rgb == GL_ONE_MINUS_SRC_ALPHA && d->src_a == GL_ONE &&
        d->dst_a == GL_ONE_MINUS_SRC_ALPHA && d->eq_rgb == GL_FUNC_ADD && d->eq_a == GL_FUNC_ADD)
        return BL_OVER;
    return BL_ANY;
}

static inline u32 to_target(const struct gles_target *t, u32 c)
{
    return t->bgra ? (c & 0xFF00FF00) | (c & 255) << 16 | ((c >> 16) & 255) : c;
}

static inline u32 from_target(const struct gles_target *t, u32 c) { return to_target(t, c); }

static inline u32 *row_of(const struct gles_target *t, s32 y)
{
    return t->pix + (u64)(t->flip_y ? (s32)t->h - 1 - y : y) * t->pitch;
}

/* write one fragment (RGBA8) to p */
static inline void put(const struct gles_draw *d, u32 bk, u32 *p, u32 c)
{
    const struct gles_target *t = d->dst;
    if (bk == BL_NONE)
        *p = to_target(t, c);
    else if (bk == BL_OVER)
        *p = to_target(t, blend_over(c, from_target(t, *p)));
    else
        *p = to_target(t, blend_any(d, c, from_target(t, *p)));
}

static inline u32 modulate(u32 c, u32 m)
{
    if (m == 0xFFFFFFFFu)
        return c;
    return rgba(mul255(c & 255, m & 255), mul255((c >> 8) & 255, (m >> 8) & 255),
                mul255((c >> 16) & 255, (m >> 16) & 255), mul255(c >> 24, m >> 24));
}

static inline u32 swizzle(const struct gles_fs *fs, u32 c)
{
    if (fs->flags & FSF_SWAP_RB)
        c = (c & 0xFF00FF00) | (c & 255) << 16 | ((c >> 16) & 255);
    if (fs->flags & FSF_ALPHA_ONE)
        c |= 0xFF000000u;
    return c;
}

/* the uniform colour as RGBA8 */
static u32 ucolor8(const struct gles_draw *d)
{
    return rgba(f2u8(d->ucolor[0]), f2u8(d->ucolor[1]), f2u8(d->ucolor[2]), f2u8(d->ucolor[3]));
}

/* the colour a fragment gets before blending */
static inline u32 shade(const struct gles_draw *d, float u, float v, u32 vcol, bool linear)
{
    const struct gles_fs *fs = d->fs;
    u32 c;
    if (fs->kind == FS_SOLID)
        c = (fs->flags & FSF_MUL_UCOLOR) && !(fs->flags & FSF_MUL_VCOLOR) ? ucolor8(d) : vcol;
    else if (fs->kind == FS_TEX)
        c = swizzle(fs, gles_sample(d->tex[0], u, v, linear));
    else
        c = yuv_shade(d, u, v, linear);
    if (fs->kind != FS_SOLID) {
        if (fs->flags & FSF_MUL_VCOLOR)
            c = modulate(c, vcol);
        if (fs->flags & FSF_MUL_UCOLOR)
            c = modulate(c, ucolor8(d));
    } else if ((fs->flags & FSF_MUL_UCOLOR) && (fs->flags & FSF_MUL_VCOLOR))
        c = modulate(c, ucolor8(d));
    return c;
}

/* ---- triangles ---- */

struct plane {
    float a0, dx, dy; /* value at (0, 0), and its gradient */
};

static struct plane plane_of(float x0, float y0, float a0, float x1, float y1, float a1, float x2, float y2, float a2,
                             float inv_area)
{
    struct plane p;
    float e1x = x1 - x0, e1y = y1 - y0, e2x = x2 - x0, e2y = y2 - y0;
    p.dx = ((a1 - a0) * e2y - (a2 - a0) * e1y) * inv_area;
    p.dy = ((a2 - a0) * e1x - (a1 - a0) * e2x) * inv_area;
    p.a0 = a0 - p.dx * x0 - p.dy * y0;
    return p;
}

static inline float at(const struct plane *p, float x, float y) { return p->a0 + p->dx * x + p->dy * y; }

/* x where the edge from (xa, ya) to (xb, yb), ya < yb, crosses height y;
 * always computed from the upper end, so both triangles of a shared edge
 * get the same answer */
static inline float edge_x(float xa, float ya, float xb, float yb, float y)
{
    return xa + (y - ya) * ((xb - xa) / (yb - ya));
}

/* a vertex position as the GPU's rasteriser takes it: to 1/256 of a pixel */
static inline float snap8(float f) { return f > -1e6f && f < 1e6f ? (float)q8(f) / 256.0f : f; }

void gles_triangle(const struct gles_draw *d, const struct gles_vert *a0, const struct gles_vert *b0,
                   const struct gles_vert *c0)
{
    /* Positions to 1/256 of a pixel and GL's top-left rule in the target's
     * memory rows (top first, as the GPU covers pixels): a pixel centre on an
     * edge is drawn by the same triangle on both paths */
    struct gles_vert sv[3] = {*a0, *b0, *c0};
    for (int i = 0; i < 3; i++)
        sv[i].x = snap8(sv[i].x), sv[i].y = snap8(sv[i].y);
    const struct gles_vert *a = &sv[0], *b = &sv[1], *c = &sv[2];
    const struct gles_vert *v[3] = {a, b, c};
    /* sort by y */
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2 - i; j++)
            if (v[j]->y > v[j + 1]->y) {
                const struct gles_vert *t = v[j];
                v[j] = v[j + 1], v[j + 1] = t;
            }
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (!(area > 1e-12f || area < -1e-12f))
        return;
    float inv = 1.0f / area;
    const struct gles_fs *fs = d->fs;
    bool tex = fs->kind != FS_SOLID;
    bool need_c = (fs->flags & FSF_MUL_VCOLOR) || (fs->kind == FS_SOLID && !(fs->flags & FSF_MUL_UCOLOR));
    /* a colour that is the same at all three corners needs no interpolation */
    bool flat = a->c[0] == b->c[0] && a->c[0] == c->c[0] && a->c[1] == b->c[1] && a->c[1] == c->c[1] &&
                a->c[2] == b->c[2] && a->c[2] == c->c[2] && a->c[3] == b->c[3] && a->c[3] == c->c[3];
    u32 flat_col = rgba(f2u8(a->c[0]), f2u8(a->c[1]), f2u8(a->c[2]), f2u8(a->c[3]));
    if (!need_c)
        flat = true, flat_col = 0xFFFFFFFFu;
    struct plane pu = {0, 0, 0}, pv = {0, 0, 0}, pc[4];
    if (tex) {
        pu = plane_of(a->x, a->y, a->u, b->x, b->y, b->u, c->x, c->y, c->u, inv);
        pv = plane_of(a->x, a->y, a->v, b->x, b->y, b->v, c->x, c->y, c->v, inv);
    }
    if (!flat)
        for (int k = 0; k < 4; k++)
            pc[k] = plane_of(a->x, a->y, a->c[k], b->x, b->y, b->c[k], c->x, c->y, c->c[k], inv);
    /* minification or magnification: the texel step per pixel against 1 */
    bool linear = false;
    if (tex && d->tex[0]) {
        float sx = pu.dx * (float)d->tex[0]->w, sy = pv.dy * (float)d->tex[0]->h;
        bool magnify = (sx * sx + sy * sy) <= 2.0f;
        linear = tex_linear(d->tex[0], magnify);
    }
    u32 bk = blend_kind(d);
    /* rows whose centre is on an edge: the upper one in memory is drawn (for
     * the window that is GL's higher y) */
    bool up = d->dst->flip_y;
    s32 ytop = up ? ifloor(v[0]->y - 0.5f) + 1 : iceil(v[0]->y - 0.5f);
    s32 ybot = up ? ifloor(v[2]->y - 0.5f) + 1 : iceil(v[2]->y - 0.5f);
    if (ytop < d->clip.y0)
        ytop = d->clip.y0;
    if (ybot > d->clip.y1)
        ybot = d->clip.y1;
    for (s32 y = ytop; y < ybot; y++) {
        float yc = (float)y + 0.5f;
        float xl = edge_x(v[0]->x, v[0]->y, v[2]->x, v[2]->y, yc), xr;
        if (yc < v[1]->y)
            xr = edge_x(v[0]->x, v[0]->y, v[1]->x, v[1]->y, yc);
        else
            xr = edge_x(v[1]->x, v[1]->y, v[2]->x, v[2]->y, yc);
        if (xl > xr) {
            float t = xl;
            xl = xr, xr = t;
        }
        s32 x0 = iceil(xl - 0.5f), x1 = iceil(xr - 0.5f);
        if (x0 < d->clip.x0)
            x0 = d->clip.x0;
        if (x1 > d->clip.x1)
            x1 = d->clip.x1;
        if (x0 >= x1)
            continue;
        u32 *p = row_of(d->dst, y) + x0;
        float xc = (float)x0 + 0.5f;
        float u = tex ? at(&pu, xc, yc) : 0, vv = tex ? at(&pv, xc, yc) : 0;
        float cr = 0, cg = 0, cb = 0, ca = 0;
        if (!flat)
            cr = at(&pc[0], xc, yc), cg = at(&pc[1], xc, yc), cb = at(&pc[2], xc, yc), ca = at(&pc[3], xc, yc);
        if (fs->kind == FS_TEX && !linear && flat && d->tex[0] && d->tex[0]->pix && !(fs->flags & FSF_MUL_UCOLOR)) {
            /* the common case: nearest texels, one colour */
            const struct gles_tex *t = d->tex[0];
            float fw = (float)t->w, fh = (float)t->h;
            float du = pu.dx * fw;
            float fu = u * fw + SUBTEXEL;
            s32 ty = wrap(ifloor(vv * fh + SUBTEXEL), t->h, t->wrap_t);
            float dvx = pv.dx * fh;
            if (dvx == 0.0f && t->wrap_s == GL_CLAMP_TO_EDGE && bk != BL_ANY && fu > -1e6f && fu < 1e6f) {
                /* a row of a 2D blit: 16.16 steps, everything in the target's channel order (blending and
                 * modulation treat red and blue alike, so only the order needs to agree) */
                const u32 *trow = t->pix + (u64)ty * t->pitch;
                bool swap = ((fs->flags & FSF_SWAP_RB) != 0) != d->dst->bgra;
                u32 one = (fs->flags & FSF_ALPHA_ONE) ? 0xFF000000u : 0;
                u32 mod = (fs->flags & FSF_MUL_VCOLOR) ? flat_col : 0xFFFFFFFFu;
                if (d->dst->bgra && mod != 0xFFFFFFFFu)
                    mod = (mod & 0xFF00FF00) | (mod & 255) << 16 | ((mod >> 16) & 255);
                /* 32.32 steps: a long row drifts by far less than SUBTEXEL */
                s64 u32f = (s64)(fu * 4294967296.0f), du32 = (s64)(du * 4294967296.0f);
                s32 maxx = (s32)t->w - 1;
                for (s32 x = x0; x < x1; x++, p++, u32f += du32) {
                    s32 ix = (s32)(u32f >> 32);
                    ix = ix < 0 ? 0 : ix > maxx ? maxx : ix;
                    u32 c = trow[ix] | one;
                    if (swap)
                        c = (c & 0xFF00FF00) | (c & 255) << 16 | ((c >> 16) & 255);
                    if (mod != 0xFFFFFFFFu)
                        c = modulate(c, mod);
                    *p = bk == BL_NONE ? c : blend_over(c, *p);
                }
            } else if (dvx == 0.0f) {
                const u32 *trow = t->pix + (u64)ty * t->pitch;
                for (s32 x = x0; x < x1; x++, p++) {
                    u32 tc = swizzle(fs, trow[(u32)wrap(ifloor(fu + (float)(x - x0) * du), t->w, t->wrap_s)]);
                    put(d, bk, p, (fs->flags & FSF_MUL_VCOLOR) ? modulate(tc, flat_col) : tc);
                }
            } else {
                float fv = vv * fh + SUBTEXEL;
                for (s32 x = x0; x < x1; x++, p++) {
                    float i = (float)(x - x0);
                    u32 tc = swizzle(fs, texel(t, ifloor(fu + i * du), ifloor(fv + i * dvx)));
                    put(d, bk, p, (fs->flags & FSF_MUL_VCOLOR) ? modulate(tc, flat_col) : tc);
                }
            }
            continue;
        }
        if (fs->kind == FS_SOLID && flat) {
            u32 col = shade(d, 0, 0, flat_col, false);
            if (bk == BL_NONE) {
                u32 tc = to_target(d->dst, col);
                for (s32 x = x0; x < x1; x++)
                    *p++ = tc;
            } else
                for (s32 x = x0; x < x1; x++)
                    put(d, bk, p++, col);
            continue;
        }
        for (s32 x = x0; x < x1; x++, p++) {
            u32 vc = flat ? flat_col : rgba(f2u8(cr), f2u8(cg), f2u8(cb), f2u8(ca));
            put(d, bk, p, shade(d, u, vv, vc, linear));
            u += pu.dx, vv += pv.dx;
            if (!flat)
                cr += pc[0].dx, cg += pc[1].dx, cb += pc[2].dx, ca += pc[3].dx;
        }
    }
}

/* ---- lines and points ---- */

static bool inside(const struct gles_draw *d, s32 x, s32 y)
{
    return x >= d->clip.x0 && x < d->clip.x1 && y >= d->clip.y0 && y < d->clip.y1;
}

static void frag(const struct gles_draw *d, s32 x, s32 y, float u, float v, const float c[4])
{
    if (!inside(d, x, y))
        return;
    u32 vc = rgba(f2u8(c[0]), f2u8(c[1]), f2u8(c[2]), f2u8(c[3]));
    bool linear = d->fs->kind != FS_SOLID && d->tex[0] && tex_linear(d->tex[0], true);
    put(d, blend_kind(d), row_of(d->dst, y) + x, shade(d, u, v, vc, linear));
}

void gles_point(const struct gles_draw *d, const struct gles_vert *a)
{
    frag(d, ifloor(a->x), ifloor(a->y), a->u, a->v, a->c);
}

/* A line from a to b: the pixels a DDA visits from a's, b's last unless the
 * caller asks for it (a strip's last segment). */
void gles_line(const struct gles_draw *d, const struct gles_vert *a, const struct gles_vert *b, bool last)
{
    float dx = b->x - a->x, dy = b->y - a->y;
    float adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    s32 n = iceil(adx > ady ? adx : ady);
    if (n <= 0) {
        if (last)
            gles_point(d, a);
        return;
    }
    if (n > 1 << 16)
        n = 1 << 16;
    float c[4];
    for (s32 i = 0; i < n + (last ? 1 : 0); i++) {
        float t = (float)i / (float)n;
        for (int k = 0; k < 4; k++)
            c[k] = a->c[k] + (b->c[k] - a->c[k]) * t;
        frag(d, ifloor(a->x + dx * t), ifloor(a->y + dy * t), a->u + (b->u - a->u) * t, a->v + (b->v - a->v) * t, c);
    }
}

void gles_clear(struct gles_target *t, const struct gles_rect *clip, const float c[4])
{
    struct gles_target *tt = t;
    u32 col = to_target(tt, rgba(f2u8(c[0]), f2u8(c[1]), f2u8(c[2]), f2u8(c[3])));
    for (s32 y = clip->y0; y < clip->y1; y++) {
        u32 *p = row_of(tt, y) + clip->x0;
        for (s32 x = clip->x0; x < clip->x1; x++)
            *p++ = col;
    }
}
