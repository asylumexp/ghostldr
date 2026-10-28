/*
 * The software OpenGL ES 2.0 behind the Piglet family (hle_gles.c): the
 * pieces that need no console, built for this machine by test_gles.py too.
 *
 * Piglet (libScePigletv2VSH: EGL and GLES2) is in a PS4's /system/common/lib
 * but not in a 13.60 PS5's, so homebrew drawn with it (SDL2's PS4 port, which
 * Apollo Save Tool uses) has no display there.  This draws what such titles
 * draw, on the CPU: textured and coloured triangles, lines and points,
 * blending, scissoring, render-to-texture.  There is no GLSL compiler: a
 * shader is recognised by what it does (gles_shader_parse), and the shaders
 * SDL2's GLES2 renderer uses are all recognised.  An unknown one fails to
 * compile, with the reason in its info log.
 */
#pragma once

#include "ext.h"

/* ---- GL enums ---- */
#define GL_NO_ERROR 0
#define GL_INVALID_ENUM 0x0500
#define GL_INVALID_VALUE 0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_OUT_OF_MEMORY 0x0505
#define GL_INVALID_FRAMEBUFFER_OPERATION 0x0506

#define GL_POINTS 0
#define GL_LINES 1
#define GL_LINE_LOOP 2
#define GL_LINE_STRIP 3
#define GL_TRIANGLES 4
#define GL_TRIANGLE_STRIP 5
#define GL_TRIANGLE_FAN 6

#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE 0x0308
#define GL_FUNC_ADD 0x8006
#define GL_MIN 0x8007
#define GL_MAX 0x8008
#define GL_FUNC_SUBTRACT 0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B

#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_DITHER 0x0BD0
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11

#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400
#define GL_COLOR_BUFFER_BIT 0x4000

#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406

#define GL_ALPHA 0x1906
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_BGRA_EXT 0x80E1
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_UNSIGNED_SHORT_5_6_5 0x8363

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370

#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_DELETE_STATUS 0x8B80
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_VALIDATE_STATUS 0x8B83
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_ATTACHED_SHADERS 0x8B85
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_ACTIVE_ATTRIBUTES 0x8B89
#define GL_SHADER_SOURCE_LENGTH 0x8B88
#define GL_SHADER_TYPE 0x8B4F

#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT 0x8CD6
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT 0x8CD7

#define GL_VIEWPORT 0x0BA2
#define GL_SCISSOR_BOX 0x0C10
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_VIEWPORT_DIMS 0x0D3A
#define GL_RED_BITS 0x0D52
#define GL_GREEN_BITS 0x0D53
#define GL_BLUE_BITS 0x0D54
#define GL_ALPHA_BITS 0x0D55
#define GL_DEPTH_BITS 0x0D56
#define GL_STENCIL_BITS 0x0D57
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_MAX_RENDERBUFFER_SIZE 0x84E8
#define GL_NUM_COMPRESSED_TEXTURE_FORMATS 0x86A2
#define GL_COMPRESSED_TEXTURE_FORMATS 0x86A3
#define GL_MAX_VERTEX_ATTRIBS 0x8869
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#define GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS 0x8B4C
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_IMPLEMENTATION_COLOR_READ_TYPE 0x8B9A
#define GL_IMPLEMENTATION_COLOR_READ_FORMAT 0x8B9B
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_SHADER_BINARY_FORMATS 0x8DF8
#define GL_NUM_SHADER_BINARY_FORMATS 0x8DF9
#define GL_SHADER_COMPILER 0x8DFA
#define GL_MAX_VERTEX_UNIFORM_VECTORS 0x8DFB
#define GL_MAX_VARYING_VECTORS 0x8DFC
#define GL_MAX_FRAGMENT_UNIFORM_VECTORS 0x8DFD

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C

/* ---- shaders ---- */

/* what a fragment shader computes */
enum gles_fs_kind {
    FS_NONE,
    FS_SOLID, /* the interpolated colour */
    FS_TEX,   /* texture2D(u_texture, v_texCoord), swizzled */
    FS_YUV,   /* three planes: Y, U, V */
    FS_NV,    /* two planes: Y and interleaved UV */
};

#define FSF_SWAP_RB 1u   /* the red and blue channels of the texel swapped */
#define FSF_ALPHA_ONE 2u /* the texel's alpha taken as 1 */
#define FSF_MUL_VCOLOR 4u /* times the interpolated colour (v_color) */
#define FSF_MUL_UCOLOR 8u /* times a uniform colour (u_color) */

#define VSF_TEXCOORD 1u /* a_texCoord -> v_texCoord */
#define VSF_COLOR 2u    /* a_color -> v_color */
#define VSF_ROTATE 4u   /* SDL 2.0.10-2.0.16: a_angle (degrees) about a_center */

struct gles_fs {
    u32 kind, flags;
    char uv_swz[2];  /* FS_NV: which texel channels hold U and V ('r','g','b','a') */
    float offset[3]; /* FS_YUV/FS_NV: added to (y, u, v) */
    float mat[9];    /* then rgb = mat * yuv, column-major as GLSL's mat3 */
};

struct gles_vs {
    u32 flags;
};

/* Recognise a shader from its source (the strings glShaderSource got, joined).
 * 0 when it is one this renderer can run, else -1 with the reason in why. */
int gles_vs_parse(const char *src, struct gles_vs *out, char *why, u32 why_size);
int gles_fs_parse(const char *src, struct gles_fs *out, char *why, u32 why_size);

/* ---- textures and render targets ---- */

/* RGBA8: each u32 is R | G << 8 | B << 16 | A << 24 (the bytes R, G, B, A in
 * memory, as glTexImage2D's GL_RGBA data) */
struct gles_tex {
    u32 *pix;
    u32 w, h, cap; /* cap: bytes allocated at pix */
    u32 pitch;     /* texels from one row to the next (w, or more for the GPU's alignment) */
    u32 gpu_seq;   /* the last GPU command buffer that used it (gles_gpu.c), 0: none */
    u32 min_filter, mag_filter, wrap_s, wrap_t;
    u32 target; /* 0 until first bound */
    u32 used;
};

/* Convert rows of format/type pixels to RGBA8.  0, or a GL error. */
int gles_convert(u32 *dst, u32 dst_pitch, const void *src, u32 w, u32 h, u32 format, u32 type, u32 align);

/* A colour buffer the rasteriser writes: the window's (BGRA, rows top to
 * bottom on screen, as VideoOut's A8R8G8B8 scans them) or a texture's (RGBA,
 * GL row 0 first). */
struct gles_target {
    u32 *pix;
    u32 w, h, pitch; /* pitch in pixels */
    bool bgra;       /* pixel = B | G << 8 | R << 16 | A << 24 */
    bool flip_y;     /* GL window row y is memory row h - 1 - y */
    u32 *gpu_seq;    /* a texture's gles_tex.gpu_seq; NULL: the window */
};

struct gles_rect {
    s32 x0, y0, x1, y1; /* half-open */
};

/* the state a draw needs */
struct gles_draw {
    struct gles_target *dst;
    struct gles_rect clip; /* GL window coordinates (bottom-up), already inside dst */
    bool blend;
    u32 src_rgb, dst_rgb, src_a, dst_a; /* blend factors */
    u32 eq_rgb, eq_a;
    const struct gles_fs *fs;
    const struct gles_vs *vs;
    const struct gles_tex *tex[3]; /* u_texture, u_texture_u, u_texture_v */
    float ucolor[4];
};

/* A transformed vertex: window coordinates and what the fragment shader
 * reads (texture coordinates, colour 0..1). */
struct gles_vert {
    float x, y;
    float u, v;
    float c[4];
};

/* Rasterise one primitive into d->dst within d->clip. */
void gles_triangle(const struct gles_draw *d, const struct gles_vert *a, const struct gles_vert *b,
                   const struct gles_vert *c);
void gles_line(const struct gles_draw *d, const struct gles_vert *a, const struct gles_vert *b, bool last);
void gles_point(const struct gles_draw *d, const struct gles_vert *a);

/* glClear's colour part: rgba 0..1, within clip */
void gles_clear(struct gles_target *t, const struct gles_rect *clip, const float rgba[4]);

/* the fragment colour as 0..255 RGBA for one texel lookup (tests) */
u32 gles_sample(const struct gles_tex *t, float u, float v, bool linear);

/* sin and cos of an angle in radians, |x| < 1e4 (no libm here) */
float gles_sinf(float x);
float gles_cosf(float x);

/* ---- the GPU (gles_gpu.c) ---- */

/* What the GPU path calls: the console's Gnm and memory (hle_gles.c), or a test's. */
struct gles_gnm {
    s32 (*submit)(u32 n, void **dcb, u32 *dcb_bytes, void **ccb, u32 *ccb_bytes); /* sceGnmSubmitCommandBuffers */
    s32 (*submit_done)(void);                                                   /* sceGnmSubmitDone */
    s32 (*default_state)(u32 *cmd, u64 ndw); /* sceGnmDrawInitDefaultHardwareState350: dwords written */
    void *(*gpu_alloc)(u64 bytes);           /* memory the CPU and the GPU read and write, 256-byte aligned */
    void (*gpu_free)(void *p);
    u64 (*usec)(void);
    void (*sleep)(u32 us);
    void (*before_window)(void); /* the window's next buffer is about to be written: it must be off screen */
};

/* Start drawing on the GPU: false (and the software path) when it can't. */
bool gles_gpu_start(const struct gles_gnm *g);
bool gles_gpu_on(void);
/* the command buffer being recorded: what a draw records stamps its textures with */
u32 gles_gpu_seq(void);
/* The CPU is about to touch something command buffer seq used: submit it if
 * it is still being recorded, and wait for it. */
void gles_gpu_wait(u32 seq);
/* submit what is recorded and wait for the GPU */
void gles_gpu_finish(void);
/* free gpu_alloc memory once command buffer seq (and those before) are done */
void gles_gpu_free_later(void *p, u32 seq);
/* Record a draw of n transformed vertices; false: not one the GPU path does
 * (lines, points, YUV shaders, ...), to be drawn in software after
 * gles_gpu_finish. */
bool gles_gpu_draw(const struct gles_draw *d, u32 mode, const struct gles_vert *v, u32 n);
/* glClear's colour part within r (GL window coordinates); false: draw it in software */
bool gles_gpu_clear(const struct gles_target *t, const struct gles_rect *r, const float rgba[4]);
void gles_gpu_log(const char *when);

