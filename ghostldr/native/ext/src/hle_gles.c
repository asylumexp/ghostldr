/*
 * Piglet (libScePigletv2VSH: EGL 1.4 and OpenGL ES 2.0), in software.
 *
 * A PS4 has Piglet in /system/common/lib; a 13.60 PS5 has none a PS4 app can
 * load (its GL is GLSlim, in /system_ex).  Homebrew that draws with Piglet,
 * such as everything built on SDL2's PS4 port (Apollo Save Tool), would get no
 * display.  This family is the whole library: EGL hands out one display, one
 * config and the window surface, GL calls draw on the CPU (gles_raster.c), and
 * eglSwapBuffers shows the window through VideoOut, as the OpenOrbis samples
 * do.  It is used on a PS4 too, so a title draws the same everywhere and
 * SWRR's process never loads a second GPU driver.
 *
 * Scope: what 2D renderers use.  No depth or stencil buffers, no mipmaps, no
 * GLSL compiler: shaders are recognised (gles.h), and an unknown one fails to
 * compile with the reason in its info log, which the title logs.
 *
 * The module: sceKernelLoadStartModule of libScePigletv2VSH.sprx gets a
 * handle of the module family's virtual modules (hle_module.c, EXT_LIB_VIRTUAL)
 * and sceKernelDlsym on it these functions, so titles that look GL functions
 * up by name (SDL does) find them as titles that import them do.
 */
#include "gles.h"

#define LIB "libScePigletv2VSH"

/* ---- objects ---- */

#define MAX_TEX 4096
#define MAX_BUF 1024
#define MAX_SHADER 256
#define MAX_PROG 128
#define MAX_FBO 128
#define MAX_ATTR 16
#define MAX_UNIT 16
#define LOG_LEN 200

enum { SEM_POS, SEM_TEX, SEM_COLOR, SEM_ANGLE, SEM_CENTER, SEM_N };
static const char *const k_sem_name[SEM_N] = {"a_position", "a_texCoord", "a_color", "a_angle", "a_center"};

/* uniform locations, the same in every program */
enum { U_PROJECTION, U_TEXTURE, U_TEXTURE_U, U_TEXTURE_V, U_COLOR, U_N };
static const char *const k_uni_name[U_N] = {"u_projection", "u_texture", "u_texture_u", "u_texture_v", "u_color"};

struct buf {
    u8 *data;
    u32 size, cap;
    bool used;
};

struct shader {
    bool used, compiled, ok, del;
    u32 type;
    char *src;
    u32 src_cap;
    struct gles_vs vs;
    struct gles_fs fs;
    char log[LOG_LEN];
};

struct program {
    bool used, linked, ok, del;
    u32 vs, fs; /* attached shader names */
    struct gles_vs vsd;
    struct gles_fs fsd;
    char bound[MAX_ATTR][16]; /* glBindAttribLocation names by index */
    s32 loc[SEM_N];           /* each semantic's attribute index after linking, -1 none */
    float proj[16];
    s32 sampler[3];
    float ucolor[4];
    char log[LOG_LEN];
};

struct fbo {
    bool used;
    u32 tex;
};

struct attr {
    bool enabled, normalized;
    s32 size;
    u32 type, stride, buffer;
    u64 ptr; /* a pointer, or an offset into buffer */
};

struct surface {
    bool used, window;
    u32 w, h;
    u32 *back; /* BGRA, top row first */
    u64 bytes;
};

static struct gl {
    bool ready;
    u32 error;
    struct gles_tex *tex; /* [MAX_TEX], name n at n */
    struct buf *buf;
    struct shader *sh;
    struct program *prog;
    struct fbo *fbo;
    struct attr attr[MAX_ATTR];
    u32 unit; /* active texture unit */
    u32 bound2d[MAX_UNIT], bound_ext[MAX_UNIT];
    u32 array_buffer, element_buffer;
    u32 cur_prog, cur_fbo;
    s32 vp[4], sc[4];
    bool scissor, blend;
    u32 bsrc_rgb, bdst_rgb, bsrc_a, bdst_a, beq_rgb, beq_a;
    float clear[4];
    u32 unpack_align, pack_align;
    struct gles_vert *verts;
    u32 nverts_cap;
} G_;

/* EGL */
#define EGL_SUCCESS 0x3000
#define EGL_NOT_INITIALIZED 0x3001
#define EGL_BAD_ALLOC 0x3003
#define EGL_BAD_ATTRIBUTE 0x3004
#define EGL_BAD_CONFIG 0x3005
#define EGL_BAD_CONTEXT 0x3006
#define EGL_BAD_DISPLAY 0x3008
#define EGL_BAD_MATCH 0x3009
#define EGL_BAD_NATIVE_WINDOW 0x300B
#define EGL_BAD_PARAMETER 0x300C
#define EGL_BAD_SURFACE 0x300D
#define EGL_NONE 0x3038
#define EGL_OPENGL_ES_API 0x30A0

#define DISPLAY ((void *)(u64)0x6C67646Cu) /* any non-null value */
#define CONFIG ((void *)(u64)1)
#define CONTEXT ((void *)(u64)0x63747831u)
#define MAX_SURF 8

static struct {
    bool init;
    u32 error, api;
    struct surface surf[MAX_SURF];
    struct surface *draw;
    bool current;
    s32 interval;
} E_ = {.api = EGL_OPENGL_ES_API, .interval = 1};

static volatile int g_lock; /* one GL call at a time: titles draw from more than one thread */
static u64 g_clears, g_uploads, g_compiles; /* for the log */

/* ---- memory: the textures and buffers a title makes ---- */

#define PROT_GPU_RW 0x30 /* SCE_KERNEL_PROT_GPU_READ | GPU_WRITE */

/* textures and buffers: memory the GPU maps too, once drawing on it is on
 * (with room past the end for the texture unit's reads of whole cache lines) */
static void *mem_get(u64 bytes)
{
    bool gpu = gles_gpu_on();
    return direct_alloc(ALIGN_UP((bytes ? bytes : 1) + (gpu ? 0x400 : 0), 0x4000),
                        PROT_READ | PROT_WRITE | (gpu ? PROT_GPU_RW : 0), 0);
}
static void mem_put(void *p)
{
    if (p)
        direct_free(p);
}
/* a texture's memory, once the GPU commands that used it are done */
static void tex_put(struct gles_tex *t)
{
    if (t->pix && t->gpu_seq && gles_gpu_on())
        gles_gpu_free_later(t->pix, t->gpu_seq);
    else
        mem_put(t->pix);
    t->pix = NULL;
}
/* the CPU is about to write a texture's pixels: the GPU must be done with them */
static void tex_touch(const struct gles_tex *t)
{
    if (t->gpu_seq && gles_gpu_on())
        gles_gpu_wait(t->gpu_seq);
}

/* a texture's row length in texels: its width, or the GPU's alignment for linear surfaces */
static u32 tex_pitch(u32 w) { return gles_gpu_on() ? (u32)ALIGN_UP(w, 64) : w; }

static void gpu_start(void);

static bool gl_init(void)
{
    if (G_.ready)
        return true;
    gpu_start(); /* before any texture: their memory must be the GPU's too */
    G_.tex = page_alloc(sizeof(struct gles_tex) * MAX_TEX);
    G_.buf = page_alloc(sizeof(struct buf) * MAX_BUF);
    G_.sh = page_alloc(sizeof(struct shader) * MAX_SHADER);
    G_.prog = page_alloc(sizeof(struct program) * MAX_PROG);
    G_.fbo = page_alloc(sizeof(struct fbo) * MAX_FBO);
    if (!G_.tex || !G_.buf || !G_.sh || !G_.prog || !G_.fbo) {
        logf("gles: no memory for GL's objects");
        return false;
    }
    memset(G_.tex, 0, sizeof(struct gles_tex) * MAX_TEX);
    memset(G_.buf, 0, sizeof(struct buf) * MAX_BUF);
    memset(G_.sh, 0, sizeof(struct shader) * MAX_SHADER);
    memset(G_.prog, 0, sizeof(struct program) * MAX_PROG);
    memset(G_.fbo, 0, sizeof(struct fbo) * MAX_FBO);
    G_.bsrc_rgb = G_.bsrc_a = GL_ONE;
    G_.bdst_rgb = G_.bdst_a = GL_ZERO;
    G_.beq_rgb = G_.beq_a = GL_FUNC_ADD;
    G_.unpack_align = G_.pack_align = 4;
    G_.clear[0] = G_.clear[1] = G_.clear[2] = G_.clear[3] = 0;
    G_.ready = true;
    return true;
}

static void set_error(u32 e)
{
    if (!G_.error)
        G_.error = e;
}

/* the first calls of each GL function, for the log */
static const char *g_seen[128];
static u32 g_seen_n[128];
static void trace(const char *fn)
{
    u32 i = 0;
    for (; i < 128 && g_seen[i] && g_seen[i] != fn; i++)
        ;
    if (i == 128)
        return;
    g_seen[i] = fn;
    if (g_seen_n[i]++ < 2)
        logf("gles: %s", fn);
}

#define LOCK() spin_lock(&g_lock), trace(__func__)
#define UNLOCK() spin_unlock(&g_lock)
/* GL calls with no context made current are ignored, as GL's are */
#define ENTER()                                                                                                       \
    do {                                                                                                              \
        LOCK();                                                                                                       \
        if (!G_.ready && !gl_init()) {                                                                                \
            UNLOCK();                                                                                                 \
            return;                                                                                                   \
        }                                                                                                             \
    } while (0)
#define ENTER_R(v)                                                                                                    \
    do {                                                                                                              \
        LOCK();                                                                                                       \
        if (!G_.ready && !gl_init()) {                                                                                \
            UNLOCK();                                                                                                 \
            return v;                                                                                                 \
        }                                                                                                             \
    } while (0)

static u32 name_alloc(u32 max, bool (*used)(u32))
{
    for (u32 n = 1; n < max; n++)
        if (!used(n))
            return n;
    return 0;
}

static bool tex_used(u32 n) { return G_.tex[n].used; }
static bool buf_used(u32 n) { return G_.buf[n].used; }
static bool sh_used(u32 n) { return G_.sh[n].used; }
static bool prog_used(u32 n) { return G_.prog[n].used; }
static bool fbo_used(u32 n) { return G_.fbo[n].used; }

/* ---- textures ---- */

static void glGenTextures_(s32 n, u32 *out)
{
    ENTER();
    for (s32 i = 0; i < n && out; i++) {
        u32 t = name_alloc(MAX_TEX, tex_used);
        if (!t) {
            set_error(GL_OUT_OF_MEMORY);
            out[i] = 0;
            continue;
        }
        memset(&G_.tex[t], 0, sizeof(G_.tex[t]));
        G_.tex[t].used = true;
        G_.tex[t].min_filter = 0x2702; /* NEAREST_MIPMAP_LINEAR, GL's default */
        G_.tex[t].mag_filter = GL_LINEAR;
        G_.tex[t].wrap_s = G_.tex[t].wrap_t = GL_REPEAT;
        out[i] = t;
    }
    UNLOCK();
}

static void glDeleteTextures_(s32 n, const u32 *names)
{
    ENTER();
    for (s32 i = 0; i < n && names; i++) {
        u32 t = names[i];
        if (!t || t >= MAX_TEX || !G_.tex[t].used)
            continue;
        tex_put(&G_.tex[t]);
        memset(&G_.tex[t], 0, sizeof(G_.tex[t]));
        for (u32 u = 0; u < MAX_UNIT; u++) {
            if (G_.bound2d[u] == t)
                G_.bound2d[u] = 0;
            if (G_.bound_ext[u] == t)
                G_.bound_ext[u] = 0;
        }
        for (u32 f = 1; f < MAX_FBO; f++)
            if (G_.fbo[f].used && G_.fbo[f].tex == t)
                G_.fbo[f].tex = 0;
    }
    UNLOCK();
}

static void glActiveTexture_(u32 unit)
{
    ENTER();
    if (unit < GL_TEXTURE0 || unit >= GL_TEXTURE0 + MAX_UNIT)
        set_error(GL_INVALID_ENUM);
    else
        G_.unit = unit - GL_TEXTURE0;
    UNLOCK();
}

static void glBindTexture_(u32 target, u32 t)
{
    ENTER();
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_EXTERNAL_OES)
        set_error(GL_INVALID_ENUM);
    else if (t >= MAX_TEX)
        set_error(GL_INVALID_VALUE);
    else {
        if (t && !G_.tex[t].used) { /* a name never generated: GL ES makes it */
            memset(&G_.tex[t], 0, sizeof(G_.tex[t]));
            G_.tex[t].used = true;
            G_.tex[t].min_filter = 0x2702, G_.tex[t].mag_filter = GL_LINEAR;
            G_.tex[t].wrap_s = G_.tex[t].wrap_t = GL_REPEAT;
        }
        if (t)
            G_.tex[t].target = target;
        (target == GL_TEXTURE_2D ? G_.bound2d : G_.bound_ext)[G_.unit] = t;
    }
    UNLOCK();
}

static struct gles_tex *bound_tex(u32 target)
{
    u32 t = target == GL_TEXTURE_2D ? G_.bound2d[G_.unit] : target == GL_TEXTURE_EXTERNAL_OES ? G_.bound_ext[G_.unit]
                                                                                              : 0;
    return t ? &G_.tex[t] : NULL;
}

static void tex_param(u32 target, u32 pname, s32 v)
{
    struct gles_tex *t = bound_tex(target);
    if (!t) {
        set_error(target == GL_TEXTURE_2D || target == GL_TEXTURE_EXTERNAL_OES ? GL_INVALID_OPERATION
                                                                                : GL_INVALID_ENUM);
        return;
    }
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: t->min_filter = (u32)v; break;
    case GL_TEXTURE_MAG_FILTER: t->mag_filter = (u32)v; break;
    case GL_TEXTURE_WRAP_S: t->wrap_s = (u32)v; break;
    case GL_TEXTURE_WRAP_T: t->wrap_t = (u32)v; break;
    default: break; /* other parameters have no effect here */
    }
}

static void glTexParameteri_(u32 target, u32 pname, s32 v)
{
    ENTER();
    tex_param(target, pname, v);
    UNLOCK();
}

static void glTexParameterf_(u32 target, u32 pname, float v)
{
    ENTER();
    tex_param(target, pname, (s32)v);
    UNLOCK();
}

static void glTexImage2D_(u32 target, s32 level, s32 ifmt, s32 w, s32 h, s32 border, u32 format, u32 type,
                          const void *data)
{
    ENTER();
    struct gles_tex *t = bound_tex(target);
    if (!t)
        set_error(GL_INVALID_OPERATION);
    else if (w < 0 || h < 0 || w > 16384 || h > 16384 || border)
        set_error(GL_INVALID_VALUE);
    else if (level != 0) {
        /* mipmap levels: not kept (sampling uses level 0) */
    } else {
        u32 pitch = tex_pitch((u32)w);
        u64 bytes = (u64)pitch * (u64)h * 4;
        g_uploads++;
        if (bytes > t->cap) {
            tex_put(t);
            t->gpu_seq = 0;
            t->pix = bytes ? mem_get(bytes) : NULL;
            t->cap = t->pix ? (u32)bytes : 0; /* what it holds with room past it (mem_get) */
            if (bytes && !t->pix) {
                t->w = t->h = 0;
                set_error(GL_OUT_OF_MEMORY);
                logf("gles: no memory for a %dx%d texture", w, h);
                UNLOCK();
                return;
            }
        }
        tex_touch(t);
        t->w = (u32)w, t->h = (u32)h, t->pitch = pitch;
        int e = 0;
        if (data && bytes)
            e = gles_convert(t->pix, t->pitch, data, t->w, t->h, format, type, G_.unpack_align);
        else if (bytes)
            memset(t->pix, 0, bytes);
        if (e)
            set_error((u32)e);
    }
    UNLOCK();
}

static void glTexSubImage2D_(u32 target, s32 level, s32 x, s32 y, s32 w, s32 h, u32 format, u32 type,
                             const void *data)
{
    ENTER();
    struct gles_tex *t = bound_tex(target);
    if (!t)
        set_error(GL_INVALID_OPERATION);
    else if (level == 0 && (x < 0 || y < 0 || w < 0 || h < 0 || (u32)(x + w) > t->w || (u32)(y + h) > t->h))
        set_error(GL_INVALID_VALUE);
    else if (level == 0 && data && w && h) {
        tex_touch(t);
        int e = gles_convert(t->pix + (u64)y * t->pitch + (u32)x, t->pitch, data, (u32)w, (u32)h, format, type,
                             G_.unpack_align);
        if (e)
            set_error((u32)e);
    }
    UNLOCK();
}

static void glPixelStorei_(u32 pname, s32 v)
{
    ENTER();
    if (v != 1 && v != 2 && v != 4 && v != 8)
        set_error(GL_INVALID_VALUE);
    else if (pname == GL_UNPACK_ALIGNMENT)
        G_.unpack_align = (u32)v;
    else if (pname == GL_PACK_ALIGNMENT)
        G_.pack_align = (u32)v;
    else
        set_error(GL_INVALID_ENUM);
    UNLOCK();
}

static void glGenerateMipmap_(u32 target) {}

/* ---- buffers ---- */

static void glGenBuffers_(s32 n, u32 *out)
{
    ENTER();
    for (s32 i = 0; i < n && out; i++) {
        u32 b = name_alloc(MAX_BUF, buf_used);
        if (b) {
            memset(&G_.buf[b], 0, sizeof(G_.buf[b]));
            G_.buf[b].used = true;
        } else
            set_error(GL_OUT_OF_MEMORY);
        out[i] = b;
    }
    UNLOCK();
}

static void glDeleteBuffers_(s32 n, const u32 *names)
{
    ENTER();
    for (s32 i = 0; i < n && names; i++) {
        u32 b = names[i];
        if (!b || b >= MAX_BUF || !G_.buf[b].used)
            continue;
        mem_put(G_.buf[b].data);
        memset(&G_.buf[b], 0, sizeof(G_.buf[b]));
        if (G_.array_buffer == b)
            G_.array_buffer = 0;
        if (G_.element_buffer == b)
            G_.element_buffer = 0;
        for (u32 a = 0; a < MAX_ATTR; a++)
            if (G_.attr[a].buffer == b)
                G_.attr[a].buffer = 0;
    }
    UNLOCK();
}

static void glBindBuffer_(u32 target, u32 b)
{
    ENTER();
    if (b >= MAX_BUF)
        set_error(GL_INVALID_VALUE);
    else if (target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER) {
        if (b && !G_.buf[b].used) {
            memset(&G_.buf[b], 0, sizeof(G_.buf[b]));
            G_.buf[b].used = true;
        }
        *(target == GL_ARRAY_BUFFER ? &G_.array_buffer : &G_.element_buffer) = b;
    } else
        set_error(GL_INVALID_ENUM);
    UNLOCK();
}

static struct buf *bound_buf(u32 target)
{
    u32 b = target == GL_ARRAY_BUFFER ? G_.array_buffer : target == GL_ELEMENT_ARRAY_BUFFER ? G_.element_buffer : 0;
    return b ? &G_.buf[b] : NULL;
}

static void glBufferData_(u32 target, s64 size, const void *data, u32 usage)
{
    ENTER();
    struct buf *b = bound_buf(target);
    if (!b)
        set_error(GL_INVALID_OPERATION);
    else if (size < 0 || size > (1L << 30))
        set_error(GL_INVALID_VALUE);
    else {
        if ((u64)size > b->cap) {
            mem_put(b->data);
            b->data = mem_get((u64)size);
            b->cap = b->data ? (u32)ALIGN_UP((u64)size, 0x4000) : 0;
        }
        if (size && !b->data) {
            b->size = 0;
            set_error(GL_OUT_OF_MEMORY);
        } else {
            b->size = (u32)size;
            if (data && size)
                memcpy(b->data, data, (u64)size);
        }
    }
    UNLOCK();
}

static void glBufferSubData_(u32 target, s64 off, s64 size, const void *data)
{
    ENTER();
    struct buf *b = bound_buf(target);
    if (!b)
        set_error(GL_INVALID_OPERATION);
    else if (off < 0 || size < 0 || (u64)(off + size) > b->size)
        set_error(GL_INVALID_VALUE);
    else if (data && size)
        memcpy(b->data + off, data, (u64)size);
    UNLOCK();
}

/* ---- shaders and programs ---- */

static u32 glCreateShader_(u32 type)
{
    ENTER_R(0);
    u32 s = 0;
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER)
        set_error(GL_INVALID_ENUM);
    else if (!(s = name_alloc(MAX_SHADER, sh_used)))
        set_error(GL_OUT_OF_MEMORY);
    else {
        char *keep = G_.sh[s].src;
        u32 cap = G_.sh[s].src_cap;
        memset(&G_.sh[s], 0, sizeof(G_.sh[s]));
        G_.sh[s].src = keep, G_.sh[s].src_cap = cap;
        G_.sh[s].used = true, G_.sh[s].type = type;
    }
    UNLOCK();
    return s;
}

static struct shader *shader(u32 s)
{
    if (!s || s >= MAX_SHADER || !G_.sh[s].used) {
        set_error(GL_INVALID_VALUE);
        return NULL;
    }
    return &G_.sh[s];
}

static void glShaderSource_(u32 s, s32 count, const char *const *strs, const s32 *lens)
{
    ENTER();
    struct shader *sh = shader(s);
    if (sh && count >= 0 && strs) {
        u64 total = 1;
        for (s32 i = 0; i < count; i++)
            total += strs[i] ? (lens && lens[i] >= 0 ? (u64)lens[i] : strlen(strs[i])) : 0;
        if (total > sh->src_cap) {
            if (sh->src)
                page_free(sh->src, sh->src_cap);
            sh->src_cap = (u32)ALIGN_UP(total, 0x4000);
            sh->src = page_alloc(sh->src_cap);
            if (!sh->src)
                sh->src_cap = 0;
        }
        if (sh->src) {
            u64 n = 0;
            for (s32 i = 0; i < count; i++) {
                if (!strs[i])
                    continue;
                u64 l = lens && lens[i] >= 0 ? (u64)lens[i] : strlen(strs[i]);
                memcpy(sh->src + n, strs[i], l);
                n += l;
            }
            sh->src[n] = 0;
        } else
            set_error(GL_OUT_OF_MEMORY);
        sh->compiled = false;
    }
    UNLOCK();
}

static void glCompileShader_(u32 s)
{
    ENTER();
    struct shader *sh = shader(s);
    if (sh) {
        sh->log[0] = 0;
        int rc = sh->type == GL_VERTEX_SHADER ? gles_vs_parse(sh->src, &sh->vs, sh->log, sizeof(sh->log))
                                              : gles_fs_parse(sh->src, &sh->fs, sh->log, sizeof(sh->log));
        sh->compiled = true, sh->ok = rc == 0;
        g_compiles++;
        if (rc == 0 && sh->type == GL_FRAGMENT_SHADER)
            logf("gles: fragment shader %u: kind %u flags %x", s, sh->fs.kind, sh->fs.flags);
        else if (rc == 0)
            logf("gles: vertex shader %u: flags %x", s, sh->vs.flags);
        else
            logf("gles: shader %u not compiled: %s", s, sh->log);
    }
    UNLOCK();
}

/* Piglet's shader binaries (as SDL2's PS4 port ships its renderer's) start
 * with a header {u32 0xE891BC71, u32, u32, u32 source length} and the GLSL
 * source they were compiled from; the compiled code follows.  The source is
 * what gets recognised, as for glShaderSource. */
#define PIGLET_BIN_MAGIC 0xE891BC71u

static void glShaderBinary_(s32 n, const u32 *shaders, u32 format, const void *bin, s32 len)
{
    ENTER();
    const u8 *b = bin;
    u32 srclen = b && len >= 16 ? *(const u32 *)(b + 12) : 0;
    if (!b || len < 16 || *(const u32 *)b != PIGLET_BIN_MAGIC || !srclen || srclen > (u32)len - 16) {
        set_error(GL_INVALID_ENUM);
        logf("gles: glShaderBinary: format %x, %d bytes: not a Piglet binary with its source", format, len);
        UNLOCK();
        return;
    }
    for (s32 k = 0; k < n && shaders; k++) {
        struct shader *sh = shader(shaders[k]);
        if (!sh)
            continue;
        if (srclen + 1 > sh->src_cap) {
            if (sh->src)
                page_free(sh->src, sh->src_cap);
            sh->src_cap = (u32)ALIGN_UP(srclen + 1, 0x4000);
            sh->src = page_alloc(sh->src_cap);
            if (!sh->src) {
                sh->src_cap = 0;
                set_error(GL_OUT_OF_MEMORY);
                continue;
            }
        }
        memcpy(sh->src, b + 16, srclen);
        sh->src[srclen] = 0;
        sh->log[0] = 0;
        int rc = sh->type == GL_VERTEX_SHADER ? gles_vs_parse(sh->src, &sh->vs, sh->log, sizeof(sh->log))
                                              : gles_fs_parse(sh->src, &sh->fs, sh->log, sizeof(sh->log));
        sh->compiled = true, sh->ok = rc == 0;
        g_compiles++;
        logf("gles: shader %u from a Piglet binary (%u bytes of source): %s", shaders[k], srclen,
             rc == 0 ? "recognised" : sh->log);
    }
    UNLOCK();
}

static void glDeleteShader_(u32 s)
{
    ENTER();
    if (s) {
        struct shader *sh = shader(s);
        if (sh) {
            bool attached = false;
            for (u32 p = 1; p < MAX_PROG; p++)
                if (G_.prog[p].used && (G_.prog[p].vs == s || G_.prog[p].fs == s))
                    attached = true;
            if (attached)
                sh->del = true;
            else
                sh->used = false;
        }
    }
    UNLOCK();
}

static void glGetShaderiv_(u32 s, u32 pname, s32 *out)
{
    ENTER();
    struct shader *sh = shader(s);
    if (sh && out)
        switch (pname) {
        case GL_SHADER_TYPE: *out = (s32)sh->type; break;
        case GL_DELETE_STATUS: *out = sh->del; break;
        case GL_COMPILE_STATUS: *out = sh->compiled && sh->ok; break;
        case GL_INFO_LOG_LENGTH: *out = sh->log[0] ? (s32)strlen(sh->log) + 1 : 0; break;
        case GL_SHADER_SOURCE_LENGTH: *out = sh->src ? (s32)strlen(sh->src) + 1 : 0; break;
        default: set_error(GL_INVALID_ENUM);
        }
    UNLOCK();
}

static void info_log(const char *log, s32 max, s32 *len, char *out)
{
    s32 n = (s32)strlen(log);
    if (max <= 0 || !out) {
        if (len)
            *len = 0;
        return;
    }
    if (n > max - 1)
        n = max - 1;
    memcpy(out, log, (u64)n);
    out[n] = 0;
    if (len)
        *len = n;
}

static void glGetShaderInfoLog_(u32 s, s32 max, s32 *len, char *out)
{
    ENTER();
    struct shader *sh = shader(s);
    if (sh)
        info_log(sh->log, max, len, out);
    UNLOCK();
}

static u32 glCreateProgram_(void)
{
    ENTER_R(0);
    u32 p = name_alloc(MAX_PROG, prog_used);
    if (p) {
        memset(&G_.prog[p], 0, sizeof(G_.prog[p]));
        G_.prog[p].used = true;
        for (int k = 0; k < SEM_N; k++)
            G_.prog[p].loc[k] = -1;
        G_.prog[p].ucolor[0] = G_.prog[p].ucolor[1] = G_.prog[p].ucolor[2] = G_.prog[p].ucolor[3] = 1.0f;
    } else
        set_error(GL_OUT_OF_MEMORY);
    UNLOCK();
    return p;
}

static struct program *program(u32 p)
{
    if (!p || p >= MAX_PROG || !G_.prog[p].used) {
        set_error(GL_INVALID_VALUE);
        return NULL;
    }
    return &G_.prog[p];
}

static void release_shader(u32 s)
{
    if (s && s < MAX_SHADER && G_.sh[s].del) {
        for (u32 p = 1; p < MAX_PROG; p++)
            if (G_.prog[p].used && (G_.prog[p].vs == s || G_.prog[p].fs == s))
                return;
        G_.sh[s].used = false;
    }
}

static void glDeleteProgram_(u32 p)
{
    ENTER();
    if (p) {
        struct program *pr = program(p);
        if (pr) {
            u32 vs = pr->vs, fs = pr->fs;
            pr->used = false;
            release_shader(vs);
            release_shader(fs);
            if (G_.cur_prog == p)
                G_.cur_prog = 0;
        }
    }
    UNLOCK();
}

static void glAttachShader_(u32 p, u32 s)
{
    ENTER();
    struct program *pr = program(p);
    struct shader *sh = pr ? shader(s) : NULL;
    if (pr && sh)
        *(sh->type == GL_VERTEX_SHADER ? &pr->vs : &pr->fs) = s;
    UNLOCK();
}

static void glBindAttribLocation_(u32 p, u32 index, const char *name)
{
    ENTER();
    struct program *pr = program(p);
    if (pr && name) {
        if (index >= MAX_ATTR)
            set_error(GL_INVALID_VALUE);
        else
            strlcpy(pr->bound[index], name, sizeof(pr->bound[index]));
    }
    UNLOCK();
}

static void glLinkProgram_(u32 p)
{
    ENTER();
    struct program *pr = program(p);
    if (pr) {
        pr->linked = true, pr->ok = false, pr->log[0] = 0;
        struct shader *vs = pr->vs ? &G_.sh[pr->vs] : NULL, *fs = pr->fs ? &G_.sh[pr->fs] : NULL;
        if (!vs || !fs || !vs->used || !fs->used)
            strlcpy(pr->log, "ghostldr GLES: a vertex and a fragment shader are needed", sizeof(pr->log));
        else if (!vs->compiled || !vs->ok || !fs->compiled || !fs->ok)
            strlcpy(pr->log, "ghostldr GLES: a shader did not compile", sizeof(pr->log));
        else {
            pr->vsd = vs->vs, pr->fsd = fs->fs, pr->ok = true;
            /* attribute indices: glBindAttribLocation's, else the lowest free */
            bool taken[MAX_ATTR] = {false};
            for (int k = 0; k < SEM_N; k++) {
                pr->loc[k] = -1;
                for (u32 i = 0; i < MAX_ATTR; i++)
                    if (strcmp(pr->bound[i], k_sem_name[k]) == 0)
                        pr->loc[k] = (s32)i, taken[i] = true;
            }
            for (int k = 0; k < SEM_N; k++) {
                bool active = k == SEM_POS || (k == SEM_TEX && (pr->vsd.flags & VSF_TEXCOORD)) ||
                              (k == SEM_COLOR && (pr->vsd.flags & VSF_COLOR)) ||
                              ((k == SEM_ANGLE || k == SEM_CENTER) && (pr->vsd.flags & VSF_ROTATE));
                if (pr->loc[k] >= 0 || !active)
                    continue;
                for (u32 i = 0; i < MAX_ATTR; i++)
                    if (!taken[i] && !pr->bound[i][0]) {
                        pr->loc[k] = (s32)i, taken[i] = true;
                        break;
                    }
            }
            /* GL's defaults: the identity projection, samplers on unit 0 */
            memset(pr->proj, 0, sizeof(pr->proj));
            pr->proj[0] = pr->proj[5] = pr->proj[10] = pr->proj[15] = 1.0f;
        }
        if (!pr->ok)
            logf("gles: program %u not linked: %s", p, pr->log);
    }
    UNLOCK();
}

static void glGetProgramiv_(u32 p, u32 pname, s32 *out)
{
    ENTER();
    struct program *pr = program(p);
    if (pr && out)
        switch (pname) {
        case GL_DELETE_STATUS: *out = pr->del; break;
        case GL_LINK_STATUS: *out = pr->linked && pr->ok; break;
        case GL_VALIDATE_STATUS: *out = pr->ok; break;
        case GL_INFO_LOG_LENGTH: *out = pr->log[0] ? (s32)strlen(pr->log) + 1 : 0; break;
        case GL_ATTACHED_SHADERS: *out = (pr->vs != 0) + (pr->fs != 0); break;
        case GL_ACTIVE_ATTRIBUTES: {
            s32 n = 0;
            for (int k = 0; k < SEM_N; k++)
                n += pr->loc[k] >= 0;
            *out = n;
            break;
        }
        case GL_ACTIVE_UNIFORMS: *out = 2; break;
        default: set_error(GL_INVALID_ENUM);
        }
    UNLOCK();
}

static void glGetProgramInfoLog_(u32 p, s32 max, s32 *len, char *out)
{
    ENTER();
    struct program *pr = program(p);
    if (pr)
        info_log(pr->log, max, len, out);
    UNLOCK();
}

static void glUseProgram_(u32 p)
{
    ENTER();
    if (p && (p >= MAX_PROG || !G_.prog[p].used))
        set_error(GL_INVALID_VALUE);
    else if (p && !G_.prog[p].ok)
        set_error(GL_INVALID_OPERATION);
    else
        G_.cur_prog = p;
    UNLOCK();
}

static s32 glGetAttribLocation_(u32 p, const char *name)
{
    ENTER_R(-1);
    s32 r = -1;
    struct program *pr = program(p);
    if (pr && name) {
        if (!pr->ok)
            set_error(GL_INVALID_OPERATION);
        for (int k = 0; k < SEM_N; k++)
            if (strcmp(name, k_sem_name[k]) == 0)
                r = pr->loc[k];
    }
    UNLOCK();
    return r;
}

static s32 glGetUniformLocation_(u32 p, const char *name)
{
    ENTER_R(-1);
    s32 r = -1;
    struct program *pr = program(p);
    if (pr && name)
        for (int k = 0; k < U_N; k++)
            if (strcmp(name, k_uni_name[k]) == 0)
                r = k;
    UNLOCK();
    return r;
}

static struct program *cur_prog(void)
{
    if (!G_.cur_prog) {
        set_error(GL_INVALID_OPERATION);
        return NULL;
    }
    return &G_.prog[G_.cur_prog];
}

static void glUniform1i_(s32 loc, s32 v)
{
    ENTER();
    struct program *pr = cur_prog();
    if (pr && loc >= U_TEXTURE && loc <= U_TEXTURE_V)
        pr->sampler[loc - U_TEXTURE] = v;
    UNLOCK();
}

static void glUniform4f_(s32 loc, float x, float y, float z, float w)
{
    ENTER();
    struct program *pr = cur_prog();
    if (pr && loc == U_COLOR)
        pr->ucolor[0] = x, pr->ucolor[1] = y, pr->ucolor[2] = z, pr->ucolor[3] = w;
    UNLOCK();
}

static void glUniform4fv_(s32 loc, s32 count, const float *v)
{
    if (v && count > 0)
        glUniform4f_(loc, v[0], v[1], v[2], v[3]);
}

static void glUniform1f_(s32 loc, float v) {}

static void glUniformMatrix4fv_(s32 loc, s32 count, u8 transpose, const float *v)
{
    ENTER();
    struct program *pr = cur_prog();
    if (pr && loc == U_PROJECTION && v && count > 0) {
        for (int i = 0; i < 16; i++)
            pr->proj[i] = transpose ? v[(i % 4) * 4 + i / 4] : v[i];
    }
    UNLOCK();
}

/* ---- vertex attributes ---- */

static void glVertexAttribPointer_(u32 idx, s32 size, u32 type, u8 norm, s32 stride, const void *ptr)
{
    ENTER();
    if (idx >= MAX_ATTR || size < 1 || size > 4 || stride < 0)
        set_error(GL_INVALID_VALUE);
    else {
        struct attr *a = &G_.attr[idx];
        a->size = size, a->type = type, a->normalized = norm != 0, a->stride = (u32)stride;
        a->buffer = G_.array_buffer, a->ptr = (u64)ptr;
    }
    UNLOCK();
}

static void attrib_enable(u32 idx, bool on)
{
    if (idx >= MAX_ATTR)
        set_error(GL_INVALID_VALUE);
    else
        G_.attr[idx].enabled = on;
}

static void glEnableVertexAttribArray_(u32 idx)
{
    ENTER();
    attrib_enable(idx, true);
    UNLOCK();
}

static void glDisableVertexAttribArray_(u32 idx)
{
    ENTER();
    attrib_enable(idx, false);
    UNLOCK();
}

static u32 type_size(u32 type)
{
    switch (type) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT: return 2;
    default: return 4;
    }
}

/* attribute idx of vertex i into out[4] ((0, 0, 0, 1) for what it lacks) */
static void fetch(s32 idx, u32 i, float out[4])
{
    out[0] = out[1] = out[2] = 0.0f, out[3] = 1.0f;
    if (idx < 0 || idx >= MAX_ATTR || !G_.attr[idx].enabled)
        return;
    const struct attr *a = &G_.attr[idx];
    u32 ts = type_size(a->type), stride = a->stride ? a->stride : ts * (u32)a->size;
    const u8 *base;
    if (a->buffer) {
        const struct buf *b = &G_.buf[a->buffer];
        if (!b->data || a->ptr + (u64)stride * i + ts * (u32)a->size > b->size)
            return;
        base = b->data + a->ptr;
    } else {
        if (!a->ptr)
            return;
        base = (const u8 *)a->ptr;
    }
    const u8 *p = base + (u64)stride * i;
    for (s32 k = 0; k < a->size; k++, p += ts) {
        float f;
        switch (a->type) {
        case GL_FLOAT: f = *(const float *)p; break;
        case GL_UNSIGNED_BYTE: f = (float)*p, f = a->normalized ? f / 255.0f : f; break;
        case GL_BYTE: f = (float)*(const s8 *)p, f = a->normalized ? f / 127.0f : f; break;
        case GL_UNSIGNED_SHORT: f = (float)*(const u16 *)p, f = a->normalized ? f / 65535.0f : f; break;
        case GL_SHORT: f = (float)*(const s16 *)p, f = a->normalized ? f / 32767.0f : f; break;
        case 0x140C: f = (float)*(const s32 *)p / 65536.0f; break; /* GL_FIXED */
        default: f = 0.0f;
        }
        out[k] = f;
    }
}

/* ---- state ---- */

static void cap(u32 c, bool on)
{
    switch (c) {
    case GL_BLEND: G_.blend = on; break;
    case GL_SCISSOR_TEST: G_.scissor = on; break;
    case GL_CULL_FACE:
    case GL_DEPTH_TEST:
    case GL_STENCIL_TEST:
    case GL_DITHER:
    case 0x8037: /* POLYGON_OFFSET_FILL */
    case 0x809E: /* SAMPLE_ALPHA_TO_COVERAGE */
    case 0x80A0: /* SAMPLE_COVERAGE */ break;
    default: set_error(GL_INVALID_ENUM);
    }
}

static void glEnable_(u32 c)
{
    ENTER();
    cap(c, true);
    UNLOCK();
}

static void glDisable_(u32 c)
{
    ENTER();
    cap(c, false);
    UNLOCK();
}

static u8 glIsEnabled_(u32 c)
{
    ENTER_R(0);
    u8 r = c == GL_BLEND ? G_.blend : c == GL_SCISSOR_TEST ? G_.scissor : c == GL_DITHER;
    UNLOCK();
    return r;
}

static void glBlendFuncSeparate_(u32 srgb, u32 drgb, u32 sa, u32 da)
{
    ENTER();
    G_.bsrc_rgb = srgb, G_.bdst_rgb = drgb, G_.bsrc_a = sa, G_.bdst_a = da;
    UNLOCK();
}

static void glBlendFunc_(u32 s, u32 d) { glBlendFuncSeparate_(s, d, s, d); }

static void glBlendEquationSeparate_(u32 rgb, u32 a)
{
    ENTER();
    G_.beq_rgb = rgb, G_.beq_a = a;
    UNLOCK();
}

static void glBlendEquation_(u32 e) { glBlendEquationSeparate_(e, e); }

static void glViewport_(s32 x, s32 y, s32 w, s32 h)
{
    ENTER();
    if (w < 0 || h < 0)
        set_error(GL_INVALID_VALUE);
    else
        G_.vp[0] = x, G_.vp[1] = y, G_.vp[2] = w, G_.vp[3] = h;
    UNLOCK();
}

static void glScissor_(s32 x, s32 y, s32 w, s32 h)
{
    ENTER();
    if (w < 0 || h < 0)
        set_error(GL_INVALID_VALUE);
    else
        G_.sc[0] = x, G_.sc[1] = y, G_.sc[2] = w, G_.sc[3] = h;
    UNLOCK();
}

static void glClearColor_(float r, float g, float b, float a)
{
    ENTER();
    G_.clear[0] = r, G_.clear[1] = g, G_.clear[2] = b, G_.clear[3] = a;
    UNLOCK();
}

static u32 glGetError_(void)
{
    ENTER_R(0);
    u32 e = G_.error;
    G_.error = 0;
    UNLOCK();
    return e;
}

static void glFinish_(void)
{
    LOCK();
    if (gles_gpu_on())
        gles_gpu_finish();
    UNLOCK();
}
static void glFlush_(void) {}
static void glHint_(u32 a, u32 b) {}
static void glLineWidth_(float w) {}
static void glDepthMask_(u8 m) {}
static void glColorMask_(u8 r, u8 g, u8 b, u8 a) {}

static void get_ints(u32 pname, s32 *out)
{
    switch (pname) {
    case GL_VIEWPORT: memcpy(out, G_.vp, sizeof(G_.vp)); break;
    case GL_SCISSOR_BOX: memcpy(out, G_.sc, sizeof(G_.sc)); break;
    case GL_MAX_TEXTURE_SIZE:
    case GL_MAX_RENDERBUFFER_SIZE: *out = 8192; break;
    case GL_MAX_VIEWPORT_DIMS: out[0] = out[1] = 8192; break;
    case GL_UNPACK_ALIGNMENT: *out = (s32)G_.unpack_align; break;
    case GL_PACK_ALIGNMENT: *out = (s32)G_.pack_align; break;
    case GL_NUM_SHADER_BINARY_FORMATS:
    case GL_NUM_COMPRESSED_TEXTURE_FORMATS:
    case GL_DEPTH_BITS:
    case GL_STENCIL_BITS: *out = 0; break;
    case GL_SHADER_BINARY_FORMATS:
    case GL_COMPRESSED_TEXTURE_FORMATS: break; /* none: nothing written */
    case GL_RED_BITS:
    case GL_GREEN_BITS:
    case GL_BLUE_BITS:
    case GL_ALPHA_BITS: *out = 8; break;
    case GL_CURRENT_PROGRAM: *out = (s32)G_.cur_prog; break;
    case GL_TEXTURE_BINDING_2D: *out = (s32)G_.bound2d[G_.unit]; break;
    case GL_ARRAY_BUFFER_BINDING: *out = (s32)G_.array_buffer; break;
    case 0x8895: *out = (s32)G_.element_buffer; break; /* ELEMENT_ARRAY_BUFFER_BINDING */
    case GL_ACTIVE_TEXTURE: *out = (s32)(GL_TEXTURE0 + G_.unit); break;
    case GL_FRAMEBUFFER_BINDING: *out = (s32)G_.cur_fbo; break;
    case GL_MAX_VERTEX_ATTRIBS: *out = MAX_ATTR; break;
    case GL_MAX_TEXTURE_IMAGE_UNITS:
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *out = MAX_UNIT; break;
    case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: *out = 0; break;
    case GL_MAX_VERTEX_UNIFORM_VECTORS: *out = 128; break;
    case GL_MAX_FRAGMENT_UNIFORM_VECTORS: *out = 16; break;
    case GL_MAX_VARYING_VECTORS: *out = 8; break;
    case GL_IMPLEMENTATION_COLOR_READ_TYPE: *out = GL_UNSIGNED_BYTE; break;
    case GL_IMPLEMENTATION_COLOR_READ_FORMAT: *out = GL_RGBA; break;
    case GL_BLEND: *out = G_.blend; break;
    case GL_SCISSOR_TEST: *out = G_.scissor; break;
    case 0x80C8: *out = (s32)G_.bsrc_rgb; break; /* BLEND_SRC_RGB */
    case 0x80C9: *out = (s32)G_.bdst_rgb; break;
    case 0x80CB: *out = (s32)G_.bsrc_a; break;
    case 0x80CA: *out = (s32)G_.bdst_a; break;
    case 0x8009: *out = (s32)G_.beq_rgb; break; /* BLEND_EQUATION_RGB */
    case 0x883D: *out = (s32)G_.beq_a; break;
    default: set_error(GL_INVALID_ENUM);
    }
}

static void glGetIntegerv_(u32 pname, s32 *out)
{
    ENTER();
    if (out)
        get_ints(pname, out);
    UNLOCK();
}

static void glGetFloatv_(u32 pname, float *out)
{
    s32 v[4] = {0, 0, 0, 0};
    ENTER();
    if (pname == 0x0C22) { /* COLOR_CLEAR_VALUE */
        if (out)
            memcpy(out, G_.clear, sizeof(G_.clear));
        UNLOCK();
        return;
    }
    get_ints(pname, v);
    UNLOCK();
    u32 n = pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX ? 4 : pname == GL_MAX_VIEWPORT_DIMS ? 2 : 1;
    for (u32 i = 0; out && i < n; i++)
        out[i] = (float)v[i];
}

static void glGetBooleanv_(u32 pname, u8 *out)
{
    s32 v[4] = {0, 0, 0, 0};
    ENTER();
    if (pname == GL_SHADER_COMPILER)
        v[0] = 1;
    else
        get_ints(pname, v);
    UNLOCK();
    if (out)
        *out = v[0] != 0;
}

static const char *glGetString_(u32 name)
{
    switch (name) {
    case GL_VENDOR: return "ghostldr";
    case GL_RENDERER: return "ghostldr software GLES2 (Piglet stand-in)";
    case GL_VERSION: return "OpenGL ES 2.0 ghostldr";
    case GL_SHADING_LANGUAGE_VERSION: return "OpenGL ES GLSL ES 1.00";
    case GL_EXTENSIONS: return "GL_OES_rgb8_rgba8 GL_EXT_texture_format_BGRA8888 GL_EXT_blend_minmax";
    default: return NULL;
    }
}

/* ---- framebuffers ---- */

static void glGenFramebuffers_(s32 n, u32 *out)
{
    ENTER();
    for (s32 i = 0; i < n && out; i++) {
        u32 f = name_alloc(MAX_FBO, fbo_used);
        if (f)
            G_.fbo[f].used = true, G_.fbo[f].tex = 0;
        else
            set_error(GL_OUT_OF_MEMORY);
        out[i] = f;
    }
    UNLOCK();
}

static void glDeleteFramebuffers_(s32 n, const u32 *names)
{
    ENTER();
    for (s32 i = 0; i < n && names; i++) {
        u32 f = names[i];
        if (f && f < MAX_FBO) {
            G_.fbo[f].used = false;
            if (G_.cur_fbo == f)
                G_.cur_fbo = 0;
        }
    }
    UNLOCK();
}

static void glBindFramebuffer_(u32 target, u32 f)
{
    ENTER();
    if (target != GL_FRAMEBUFFER)
        set_error(GL_INVALID_ENUM);
    else if (f >= MAX_FBO)
        set_error(GL_INVALID_OPERATION);
    else {
        if (f && !G_.fbo[f].used)
            G_.fbo[f].used = true, G_.fbo[f].tex = 0;
        G_.cur_fbo = f;
    }
    UNLOCK();
}

static void glFramebufferTexture2D_(u32 target, u32 attach, u32 textarget, u32 tex, s32 level)
{
    ENTER();
    if (target != GL_FRAMEBUFFER || attach != GL_COLOR_ATTACHMENT0)
        set_error(GL_INVALID_ENUM);
    else if (!G_.cur_fbo || tex >= MAX_TEX)
        set_error(GL_INVALID_OPERATION);
    else
        G_.fbo[G_.cur_fbo].tex = tex;
    UNLOCK();
}

static u32 glCheckFramebufferStatus_(u32 target)
{
    ENTER_R(0);
    u32 r = GL_FRAMEBUFFER_COMPLETE;
    if (target != GL_FRAMEBUFFER)
        set_error(GL_INVALID_ENUM), r = 0;
    else if (G_.cur_fbo) {
        u32 t = G_.fbo[G_.cur_fbo].tex;
        if (!t)
            r = GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
        else if (!G_.tex[t].used || !G_.tex[t].pix)
            r = GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
    }
    UNLOCK();
    return r;
}

static bool gpu_window(const struct surface *s, struct gles_target *t);
static void gpu_before_window(void);

/* the colour buffer draws go to: the bound framebuffer's texture, else the
 * current window surface */
static bool target_of(struct gles_target *t)
{
    if (G_.cur_fbo) {
        u32 n = G_.fbo[G_.cur_fbo].tex;
        struct gles_tex *x = n && G_.tex[n].used ? &G_.tex[n] : NULL;
        if (!x || !x->pix)
            return false;
        *t = (struct gles_target){x->pix, x->w, x->h, x->pitch, false, false, &x->gpu_seq};
        return true;
    }
    struct surface *s = E_.draw;
    if (!s || !s->back)
        return false;
    if (s->window && gles_gpu_on() && gpu_window(s, t))
        return true;
    *t = (struct gles_target){s->back, s->w, s->h, s->w, true, true, NULL};
    return true;
}

/* the CPU draws into t from here: what the GPU was given must be done, and a
 * window buffer off screen */
static void cpu_draws(const struct gles_target *t)
{
    if (!gles_gpu_on())
        return;
    gles_gpu_finish();
    if (!t->gpu_seq)
        gpu_before_window();
}

static struct gles_rect clip_of(const struct gles_target *t)
{
    struct gles_rect r = {0, 0, (s32)t->w, (s32)t->h};
    s32 b[4][4] = {{G_.vp[0], G_.vp[1], G_.vp[0] + G_.vp[2], G_.vp[1] + G_.vp[3]},
                   {G_.sc[0], G_.sc[1], G_.sc[0] + G_.sc[2], G_.sc[1] + G_.sc[3]}};
    for (int k = 0; k < (G_.scissor ? 2 : 1); k++) {
        if (b[k][0] > r.x0)
            r.x0 = b[k][0];
        if (b[k][1] > r.y0)
            r.y0 = b[k][1];
        if (b[k][2] < r.x1)
            r.x1 = b[k][2];
        if (b[k][3] < r.y1)
            r.y1 = b[k][3];
    }
    if (r.x1 < r.x0)
        r.x1 = r.x0;
    if (r.y1 < r.y0)
        r.y1 = r.y0;
    return r;
}

/* one draw in software */
static void sw_draw(const struct gles_draw *d, u32 mode, struct gles_vert *v, u32 n)
{
    switch (mode) {
    case GL_TRIANGLES:
        for (u32 k = 0; k + 2 < n; k += 3)
            gles_triangle(d, &v[k], &v[k + 1], &v[k + 2]);
        break;
    case GL_TRIANGLE_STRIP:
        for (u32 k = 0; k + 2 < n; k++)
            gles_triangle(d, &v[k], &v[k + 1], &v[k + 2]);
        break;
    case GL_TRIANGLE_FAN:
        for (u32 k = 1; k + 1 < n; k++)
            gles_triangle(d, &v[0], &v[k], &v[k + 1]);
        break;
    case GL_LINES:
        for (u32 k = 0; k + 1 < n; k += 2)
            gles_line(d, &v[k], &v[k + 1], false);
        break;
    case GL_LINE_STRIP:
        for (u32 k = 0; k + 1 < n; k++)
            gles_line(d, &v[k], &v[k + 1], k + 2 == n);
        break;
    case GL_LINE_LOOP:
        for (u32 k = 0; k + 1 < n; k++)
            gles_line(d, &v[k], &v[k + 1], false);
        if (n > 1)
            gles_line(d, &v[n - 1], &v[0], false);
        break;
    case GL_POINTS:
        for (u32 k = 0; k < n; k++)
            gles_point(d, &v[k]);
        break;
    }
}

/* ---- checking the GPU against the software path ----
 * A few frames (on the console the 120th, 600th and 1800th; every frame in the
 * tests) record what they draw into the window; once the GPU has drawn the
 * frame, the software rasteriser draws the same into memory of its own and
 * the two are compared, pixel by pixel, in the log.  Each check stops the
 * title for a moment, so on the console it is a build option:
 * EXT_CFLAGS=-DGLES_CHECK=1. */
#ifndef GLES_CHECK
#ifdef GLES_HOST
#define GLES_CHECK 1
#else
#define GLES_CHECK 0
#endif
#endif
#define CHK_DRAWS 8192
#define CHK_VERTS 65536
struct chk_draw {
    struct gles_draw d;
    struct gles_tex tex[3]; /* as they were at the draw: a title changes filters between draws */
    u32 mode, first, n;
    bool clear;
    struct gles_rect r;
    float rgba[4];
};
static struct {
    bool on, lost;   /* recording this frame; something it could not record */
    bool rtt;        /* it drew into a texture too */
    struct chk_draw *dr;
    struct gles_vert *v;
    u32 ndr, nv;
    u32 *buf;
    u64 buf_bytes;
} C_;

static bool chk_room(void)
{
    if (!C_.dr) {
        C_.dr = mem_get(sizeof(struct chk_draw) * CHK_DRAWS);
        C_.v = mem_get(sizeof(struct gles_vert) * CHK_VERTS);
    }
    if (!C_.dr || !C_.v || C_.ndr == CHK_DRAWS) {
        C_.lost = true;
        return false;
    }
    return true;
}

static void chk_draw(const struct gles_draw *d, u32 mode, const struct gles_vert *v, u32 n)
{
    if (d->dst->gpu_seq) {
        C_.rtt = true;
        return;
    }
    if (!chk_room() || C_.nv + n > CHK_VERTS) {
        C_.lost = true;
        return;
    }
    struct chk_draw *c = &C_.dr[C_.ndr++];
    memset(c, 0, sizeof(*c));
    c->d = *d, c->mode = mode, c->first = C_.nv, c->n = n;
    for (int i = 0; i < 3; i++)
        if (d->tex[i])
            c->tex[i] = *d->tex[i], c->d.tex[i] = &c->tex[i];
    memcpy(C_.v + C_.nv, v, sizeof(*v) * n);
    C_.nv += n;
}

static void chk_clear(const struct gles_target *t, const struct gles_rect *r, const float rgba[4])
{
    if (t->gpu_seq || !chk_room())
        return;
    struct chk_draw *c = &C_.dr[C_.ndr++];
    memset(c, 0, sizeof(*c));
    c->clear = true, c->r = *r;
    memcpy(c->rgba, rgba, sizeof(c->rgba));
}

/* Both frames of a check that found real differences, as raw B G R A rows
 * (w x h, 4 bytes a pixel), in SWRR's own /temp0/ghostldr (not a save
 * container; gone when SWRR closes), for a closer look on a computer:
 * ghostldr's FTP server serves them */
static void chk_dump(const struct gles_target *t, s64 frame)
{
#ifndef GLES_HOST
    static s32 (*k_open)(const char *, int, int);
    static s64 (*k_write)(int, const void *, u64);
    static s32 (*k_close)(int);
    static s32 (*k_mkdir)(const char *, int);
    if (!k_open) {
        k_write = host_real("sceKernelWrite");
        k_close = host_real("sceKernelClose");
        k_mkdir = host_real("sceKernelMkdir");
        k_open = host_real("sceKernelOpen");
    }
    if (!k_open || !k_write || !k_close || !k_mkdir)
        return;
    char name[80];
    k_mkdir("/temp0/ghostldr", 0777);
    for (int which = 0; which < 2; which++) {
        fmt(name, sizeof(name), "/temp0/ghostldr/gles-check-%ld-%ux%u-%s.bgra", frame, t->w, t->h,
            which ? "software" : "gpu");
        int fd = k_open(name, 0x601 /* O_WRONLY|O_CREAT|O_TRUNC */, 0666);
        if (fd < 0)
            continue;
        for (u32 y = 0; y < t->h; y++)
            k_write(fd, which ? C_.buf + (u64)y * t->w : t->pix + (u64)y * t->pitch, (u64)t->w * 4);
        k_close(fd);
        logf("gles: GPU check frame %ld: %s frame in %s", frame, which ? "the software" : "the GPU's", name);
    }
#endif
}

/* the frame the GPU drew into t, against the software path's */
static void chk_compare(const struct gles_target *t, s64 frame)
{
    u64 bytes = (u64)t->w * t->h * 4;
    if (C_.lost) {
        logf("gles: GPU check frame %ld: too much to record, not compared", frame);
        return;
    }
    if (!C_.buf || C_.buf_bytes < bytes) {
        logf("gles: GPU check frame %ld: no copy of the buffer it drew over, not compared", frame);
        return;
    }
    struct gles_target sw = {C_.buf, t->w, t->h, t->w, true, true, NULL};
    for (u32 i = 0; i < C_.ndr; i++) {
        struct chk_draw *c = &C_.dr[i];
        if (c->clear) {
            gles_clear(&sw, &c->r, c->rgba);
            continue;
        }
        c->d.dst = &sw;
        sw_draw(&c->d, c->mode, C_.v + c->first, c->n);
    }
    u64 differ = 0, big = 0, hist[4] = {0, 0, 0, 0};
    u32 maxd = 0, fx = 0, fy = 0, fg = 0, fs = 0;
    s32 bx0 = (s32)t->w, by0 = (s32)t->h, bx1 = -1, by1 = -1; /* where they differ by more than 2 */
    struct {
        u32 x, y, g, s;
    } big_at[6];
    u32 nbig_at = 0;
    for (u32 y = 0; y < t->h; y++) {
        const volatile u32 *g = t->pix + (u64)y * t->pitch;
        const u32 *s = C_.buf + (u64)y * t->w;
        for (u32 x = 0; x < t->w; x++) {
            u32 a = g[x], b = s[x];
            if (a == b)
                continue;
            u32 dm = 0;
            for (int k = 0; k < 32; k += 8) {
                s32 dd = (s32)((a >> k) & 255) - (s32)((b >> k) & 255);
                dd = dd < 0 ? -dd : dd;
                dm = (u32)dd > dm ? (u32)dd : dm;
            }
            if (!differ)
                fx = x, fy = y, fg = a, fs = b;
            differ++;
            hist[dm == 1 ? 0 : dm == 2 ? 1 : dm <= 16 ? 2 : 3]++;
            if (dm > 2) {
                big++;
                bx0 = (s32)x < bx0 ? (s32)x : bx0, by0 = (s32)y < by0 ? (s32)y : by0;
                bx1 = (s32)x > bx1 ? (s32)x : bx1, by1 = (s32)y > by1 ? (s32)y : by1;
                /* a few, spread out: the first, then one every 1/5 of the frame's rows */
                if (nbig_at < 6 && (!nbig_at || y >= big_at[nbig_at - 1].y + t->h / 5))
                    big_at[nbig_at].x = x, big_at[nbig_at].y = y, big_at[nbig_at].g = a, big_at[nbig_at++].s = b;
            }
            maxd = dm > maxd ? dm : maxd;
        }
    }
    logf("gles: GPU check frame %ld: %u draws%s; %lu of %u pixels differ from the software path's, %lu by more "
         "than 2, at most %u%s",
         frame, C_.ndr, C_.rtt ? " (and some into textures)" : "", differ, t->w * t->h, big, maxd,
         differ ? "" : " (identical)");
    if (!differ)
        return;
    logf("gles: GPU check frame %ld: by 1: %lu, by 2: %lu, by 3-16: %lu, by more: %lu; first at (%u, %u): GPU %08x, "
         "software %08x",
         frame, hist[0], hist[1], hist[2], hist[3], fx, fy, fg, fs);
    if (big)
        logf("gles: GPU check frame %ld: those by more than 2 are within (%d, %d)-(%d, %d)", frame, bx0, by0, bx1, by1);
    for (u32 i = 0; i < nbig_at; i++)
        logf("gles: GPU check frame %ld:   (%u, %u): GPU %08x, software %08x", frame, big_at[i].x, big_at[i].y,
             big_at[i].g, big_at[i].s);
    if (big)
        chk_dump(t, frame);
}

/* Frame next, which will draw over t, is checked: start recording, from a
 * copy of what t holds now (what the frame draws over, as the GPU sees it) */
static void chk_next(s64 next, const struct gles_target *t)
{
    C_.ndr = C_.nv = 0, C_.lost = C_.rtt = false;
#ifdef GLES_HOST
    C_.on = GLES_CHECK && gles_gpu_on() && t;
#else
    C_.on = GLES_CHECK && gles_gpu_on() && t && (next == 120 || next == 600 || next == 1800);
#endif
    if (!C_.on)
        return;
    u64 bytes = (u64)t->w * t->h * 4;
    if (C_.buf_bytes < bytes) {
        mem_put(C_.buf);
        C_.buf = mem_get(bytes);
        C_.buf_bytes = C_.buf ? bytes : 0;
    }
    if (!C_.buf) {
        C_.on = false;
        return;
    }
    for (u32 y = 0; y < t->h; y++)
        memcpy(C_.buf + (u64)y * t->w, t->pix + (u64)y * t->pitch, (u64)t->w * 4);
}

static void glClear_(u32 mask)
{
    ENTER();
    struct gles_target t;
    g_clears++;
    if ((mask & GL_COLOR_BUFFER_BIT) && target_of(&t)) {
        /* glClear ignores the viewport, not the scissor box */
        struct gles_rect r = {0, 0, (s32)t.w, (s32)t.h};
        if (G_.scissor) {
            s32 x1 = G_.sc[0] + G_.sc[2], y1 = G_.sc[1] + G_.sc[3];
            r.x0 = G_.sc[0] > 0 ? G_.sc[0] : 0, r.y0 = G_.sc[1] > 0 ? G_.sc[1] : 0;
            r.x1 = x1 < r.x1 ? x1 : r.x1, r.y1 = y1 < r.y1 ? y1 : r.y1;
        }
        if (C_.on && r.x1 > r.x0 && r.y1 > r.y0)
            chk_clear(&t, &r, G_.clear);
        if (r.x1 > r.x0 && r.y1 > r.y0 && !gles_gpu_clear(&t, &r, G_.clear)) {
            cpu_draws(&t);
            gles_clear(&t, &r, G_.clear);
        }
    }
    UNLOCK();
}

static void glReadPixels_(s32 x, s32 y, s32 w, s32 h, u32 format, u32 type, void *out)
{
    ENTER();
    struct gles_target t;
    if (format != GL_RGBA || type != GL_UNSIGNED_BYTE)
        set_error(GL_INVALID_OPERATION);
    else if (w < 0 || h < 0)
        set_error(GL_INVALID_VALUE);
    else if (out && target_of(&t)) {
        if (gles_gpu_on())
            gles_gpu_finish();
        u64 pitch = ((u64)w * 4 + G_.pack_align - 1) & ~(u64)(G_.pack_align - 1);
        for (s32 j = 0; j < h; j++) {
            u32 *d = (u32 *)((u8 *)out + pitch * (u32)j);
            s32 yy = y + j;
            for (s32 i = 0; i < w; i++) {
                s32 xx = x + i;
                u32 c = 0;
                if (xx >= 0 && yy >= 0 && (u32)xx < t.w && (u32)yy < t.h) {
                    c = t.pix[(u64)(t.flip_y ? (s32)t.h - 1 - yy : yy) * t.pitch + (u32)xx];
                    if (t.bgra)
                        c = (c & 0xFF00FF00) | (c & 255) << 16 | ((c >> 16) & 255);
                }
                d[i] = c;
            }
        }
    }
    UNLOCK();
}

/* ---- drawing ---- */

static bool verts_room(u32 n)
{
    if (n <= G_.nverts_cap)
        return true;
    u32 cap = n < 4096 ? 4096 : n;
    struct gles_vert *v = mem_get((u64)cap * sizeof(*v));
    if (!v)
        return false;
    mem_put(G_.verts);
    G_.verts = v, G_.nverts_cap = cap;
    return true;
}

static void transform(const struct program *pr, u32 i, struct gles_vert *o)
{
    float pos[4], tc[4], col[4];
    fetch(pr->loc[SEM_POS], i, pos);
    float x = pos[0], y = pos[1];
    if (pr->vsd.flags & VSF_ROTATE) {
        float ang[4], ctr[4];
        fetch(pr->loc[SEM_ANGLE], i, ang);
        fetch(pr->loc[SEM_CENTER], i, ctr);
        float r = ang[0] * (3.14159265358979f / 180.0f), c = gles_cosf(r), s = gles_sinf(r);
        float dx = x - ctr[0], dy = y - ctr[1];
        /* mat2(c, -s, s, c) * d: columns (c, -s) and (s, c) */
        x = c * dx + s * dy + ctr[0];
        y = -s * dx + c * dy + ctr[1];
    }
    const float *m = pr->proj;
    float cx = m[0] * x + m[4] * y + m[12], cy = m[1] * x + m[5] * y + m[13], cw = m[3] * x + m[7] * y + m[15];
    if (cw == 0.0f)
        cw = 1.0f;
    float nx = cx / cw, ny = cy / cw;
    o->x = (nx + 1.0f) * 0.5f * (float)G_.vp[2] + (float)G_.vp[0];
    o->y = (ny + 1.0f) * 0.5f * (float)G_.vp[3] + (float)G_.vp[1];
    if (pr->vsd.flags & VSF_TEXCOORD) {
        fetch(pr->loc[SEM_TEX], i, tc);
        o->u = tc[0], o->v = tc[1];
    } else
        o->u = o->v = 0.0f;
    if (pr->vsd.flags & VSF_COLOR) {
        fetch(pr->loc[SEM_COLOR], i, col);
        memcpy(o->c, col, sizeof(o->c));
    } else
        o->c[0] = o->c[1] = o->c[2] = o->c[3] = 1.0f;
}

static const struct gles_tex *sampler_tex(s32 unit)
{
    if (unit < 0 || unit >= MAX_UNIT)
        return NULL;
    u32 t = G_.bound2d[unit] ? G_.bound2d[unit] : G_.bound_ext[unit];
    return t && G_.tex[t].used && G_.tex[t].pix ? &G_.tex[t] : NULL;
}

static u64 g_draws, g_prims, g_draw_calls, g_draw_skipped;


static void draw(u32 mode, u32 n, const u32 *index_of, u32 first)
{
    struct program *pr = cur_prog();
    struct gles_target t;
    g_draw_calls++;
    if (!pr || !target_of(&t)) {
        if (g_draw_skipped++ < 4)
            logf("gles: draw skipped: %s", !pr ? "no program in use" : "no colour buffer bound");
        return;
    }
    if (!verts_room(n)) {
        set_error(GL_OUT_OF_MEMORY);
        return;
    }
    for (u32 k = 0; k < n; k++)
        transform(pr, index_of ? index_of[k] : first + k, &G_.verts[k]);
    struct gles_draw d;
    memset(&d, 0, sizeof(d));
    d.dst = &t;
    d.clip = clip_of(&t);
    if (d.clip.x0 >= d.clip.x1 || d.clip.y0 >= d.clip.y1)
        return;
    d.blend = G_.blend;
    d.src_rgb = G_.bsrc_rgb, d.dst_rgb = G_.bdst_rgb, d.src_a = G_.bsrc_a, d.dst_a = G_.bdst_a;
    d.eq_rgb = G_.beq_rgb, d.eq_a = G_.beq_a;
    d.fs = &pr->fsd, d.vs = &pr->vsd;
    for (int k = 0; k < 3; k++)
        d.tex[k] = sampler_tex(pr->sampler[k]);
    memcpy(d.ucolor, pr->ucolor, sizeof(d.ucolor));
    struct gles_vert *v = G_.verts;
    g_draws++;
    g_prims += mode == GL_TRIANGLES ? n / 3 : mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN ? (n > 2 ? n - 2 : 0) : 0;
    if (mode > GL_TRIANGLE_FAN) {
        set_error(GL_INVALID_ENUM);
        return;
    }
    if (gles_gpu_on()) {
        if (C_.on)
            chk_draw(&d, mode, v, n);
        if (gles_gpu_draw(&d, mode, v, n))
            return;
        cpu_draws(&t);
    }
    sw_draw(&d, mode, v, n);
}

static void glDrawArrays_(u32 mode, s32 first, s32 count)
{
    ENTER();
    if (first < 0 || count < 0)
        set_error(GL_INVALID_VALUE);
    else if (count)
        draw(mode, (u32)count, NULL, (u32)first);
    UNLOCK();
}

static void glDrawElements_(u32 mode, s32 count, u32 type, const void *indices)
{
    ENTER();
    if (count < 0)
        set_error(GL_INVALID_VALUE);
    else if (count) {
        const u8 *src = indices;
        if (G_.element_buffer) {
            const struct buf *b = &G_.buf[G_.element_buffer];
            src = b->data ? b->data + (u64)indices : NULL;
        }
        u32 *idx = mem_get((u64)count * 4);
        if (!idx || !src)
            set_error(GL_OUT_OF_MEMORY);
        else {
            for (s32 k = 0; k < count; k++)
                idx[k] = type == GL_UNSIGNED_BYTE    ? src[k]
                         : type == GL_UNSIGNED_SHORT ? ((const u16 *)src)[k]
                                                     : ((const u32 *)src)[k];
            draw(mode, (u32)count, idx, 0);
        }
        mem_put(idx);
    }
    UNLOCK();
}

/* ---- the display (VideoOut) ---- */

#ifndef GLES_HOST
#define SCAN_BUFFERS 2
static struct {
    bool open, failed;
    s32 handle;
    u32 w, h, pitch;
    u32 *scan[SCAN_BUFFERS];
    void *eq;
    s64 frame;
    s32 (*submit)(s32, s32, u32, s64);
    s32 (*status)(s32, void *);
    s32 (*wait)(void *, void *, s32, s32 *, u32 *);
} V_;

typedef s64 (*fn6)(u64, u64, u64, u64, u64, u64);
void ov_present(s32 handle, s32 idx) HIDDEN; /* dlg_overlay.c */

static bool vo_open(u32 w, u32 h)
{
    if (V_.open || V_.failed)
        return V_.open;
    V_.failed = true;
    fn6 open = (fn6)hle_lookup("libSceVideoOut", "sceVideoOutOpen");
    if (!open)
        open = (fn6)host_real_in("libSceVideoOut", "sceVideoOutOpen");
    void (*attr_fn)(void *, u32, u32, u32, u32, u32, u32) =
        host_real_in("libSceVideoOut", "sceVideoOutSetBufferAttribute");
    fn6 reg = (fn6)hle_lookup("libSceVideoOut", "sceVideoOutRegisterBuffers");
    if (!reg)
        reg = (fn6)host_real_in("libSceVideoOut", "sceVideoOutRegisterBuffers");
    s32 (*rate)(s32, s32) = host_real_in("libSceVideoOut", "sceVideoOutSetFlipRate");
    s32 (*add_flip)(void *, s32, void *) = host_real_in("libSceVideoOut", "sceVideoOutAddFlipEvent");
    s32 (*create_eq)(void **, const char *) = host_real("sceKernelCreateEqueue");
    V_.submit = host_real_in("libSceVideoOut", "sceVideoOutSubmitFlip");
    V_.status = host_real_in("libSceVideoOut", "sceVideoOutGetFlipStatus");
    V_.wait = host_real("sceKernelWaitEqueue");
    if (!open || !attr_fn || !reg || !V_.submit || !V_.status) {
        logf("gles: VideoOut functions missing: no display");
        return false;
    }
    s32 hdl = (s32)open(0xFF, 0, 0, 0, 0, 0);
    if (hdl < 0) {
        logf("gles: sceVideoOutOpen -> %x: no display", (u32)hdl);
        return false;
    }
    u32 pitch = (u32)ALIGN_UP(w, 64); /* the GPU's linear-aligned rows */
    u64 one = ALIGN_UP((u64)pitch * h * 4, 0x10000), size = ALIGN_UP(one * SCAN_BUFFERS, 0x200000);
    off_t off = 0;
    void *mem = NULL;
    if (K.alloc_direct(0, (off_t)K.direct_size(), size, 0x200000, 3 /* WC garlic */, &off) != 0 ||
        K.map_direct(&mem, size, 0x33, 0, off, 0x200000) != 0) {
        mem = direct_alloc(size, PROT_READ | PROT_WRITE | PROT_GPU_RW, 0);
        logf("gles: no write-combined memory for the display; using %p", mem);
    }
    if (!mem)
        return false;
    for (u32 i = 0; i < SCAN_BUFFERS; i++) {
        V_.scan[i] = (u32 *)((u8 *)mem + one * i);
        memset(V_.scan[i], 0, (u64)pitch * h * 4);
    }
    u8 attr[0x40];
    memset(attr, 0, sizeof(attr));
    attr_fn(attr, 0x80000000u /* A8R8G8B8_SRGB */, 1 /* linear */, 0, w, h, pitch);
    s64 r = reg((u64)hdl, 0, (u64)V_.scan, SCAN_BUFFERS, (u64)attr, 0);
    if (r < 0) {
        logf("gles: sceVideoOutRegisterBuffers -> %x: no display", (u32)r);
        return false;
    }
    if (rate)
        rate(hdl, 0);
    if (create_eq && create_eq(&V_.eq, "ghostldr gles") == 0 && add_flip)
        add_flip(V_.eq, hdl, NULL);
    V_.handle = hdl, V_.w = w, V_.h = h, V_.pitch = pitch;
    V_.open = true, V_.failed = false;
    logf("gles: display %ux%u on VideoOut handle %d (%u buffers at %p)", w, h, hdl, SCAN_BUFFERS, mem);
    return true;
}

static s64 flip_arg(void)
{
    u8 st[0x40];
    memset(st, 0, sizeof(st));
    if (V_.status(V_.handle, st) != 0)
        return -1;
    return *(s64 *)(st + 0x18);
}

static u64 usec(void);
static u64 g_t_flipwait; /* time spent waiting for flips, for the log */

/* the flip of frame f has happened (bounded: ~100 ms) */
static void wait_flip(s64 f)
{
    u64 t0 = usec();
    for (int tries = 0; tries < 10 && flip_arg() < f; tries++) {
        if (V_.eq && V_.wait) {
            u8 ev[0x20];
            s32 n = 0;
            u32 us = 10000;
            V_.wait(V_.eq, ev, 1, &n, &us);
        } else
            K.usleep(1000);
    }
    g_t_flipwait += usec() - t0;
}

/* the GPU draws the window straight into the VideoOut buffer the next frame
 * shows (gles_gpu.c): before anything writes it, the flip that took it off
 * screen must have happened */
static bool gpu_window(const struct surface *s, struct gles_target *t)
{
    if (!vo_open(s->w, s->h) || V_.w != s->w || V_.h != s->h)
        return false;
    *t = (struct gles_target){V_.scan[(V_.frame + 1) % SCAN_BUFFERS], s->w, s->h, V_.pitch, true, true, NULL};
    return true;
}

static void gpu_before_window(void)
{
    if (V_.open && V_.frame > 0)
        wait_flip(V_.frame);
}

static u64 usec(void)
{
    static u64 (*ptime)(void);
    if (!ptime)
        ptime = host_real("sceKernelGetProcessTime");
    return ptime ? ptime() : 0;
}

/* the frame to the screen: the software path's back buffer (BGRA, w x h)
 * copied with non-temporal stores into the write-combined scan-out buffer,
 * or the GPU's commands for the buffer it drew into finished */
static void present(const struct surface *s)
{
    static u64 t_last, wait_last;
    if (!vo_open(s->w, s->h))
        return;
    struct gles_target gt;
    bool gpu = gles_gpu_on() && gpu_window(s, &gt);
    s64 f = V_.frame + 1;
    if (gpu) {
        gles_gpu_finish(); /* waits for flip f - 1 before the GPU writes (gpu_before_window) */
        gpu = gles_gpu_on();
        __asm__ volatile("sfence" ::: "memory"); /* the CPU's own draws, if any */
        if (gpu && C_.on)
            chk_compare(&gt, f);
    }
    if (!gpu) {
        if (gles_gpu_on())
            gles_gpu_finish(); /* the GPU drew into the back buffer (no VideoOut buffer of its size) */
        wait_flip(f - 1); /* the buffer drawn now was last shown two frames ago */
        u32 *dst = V_.scan[f % SCAN_BUFFERS];
        u32 w = s->w < V_.w ? s->w : V_.w, h = s->h < V_.h ? s->h : V_.h;
        typedef long long v2di __attribute__((vector_size(16)));
        for (u32 y = 0; y < h; y++) {
            const u32 *src = s->back + (u64)y * s->w;
            u32 *d = dst + (u64)y * V_.pitch;
            u32 x = 0;
            if (((u64)d & 15) == 0)
                for (; x + 4 <= w; x += 4) {
                    v2di v;
                    memcpy(&v, src + x, 16);
                    __builtin_nontemporal_store(v, (v2di *)(d + x));
                }
            for (; x < w; x++)
                d[x] = src[x];
        }
        __asm__ volatile("sfence" ::: "memory");
    }
    V_.frame = f;
    ov_present(V_.handle, (s32)(f % SCAN_BUFFERS)); /* a drawn dialog over the frame (dlg_overlay.c) */
    s32 r = V_.submit(V_.handle, (s32)(f % SCAN_BUFFERS), E_.interval > 0 ? 1 : 2, f);
    if (gpu && gles_gpu_on() && gpu_window(s, &gt)) /* the next frame's buffer: frame f - 1, on screen until flip f */
        chk_next(f + 1, &gt);
    else
        C_.on = false;
    static u32 said;
    if (said++ < 3 || (r < 0 && said < 16) || f % 300 == 0) {
        /* the frame time, and what of it was not spent waiting for the display (flip-locked at 60 Hz) */
        u64 t = usec(), per = f % 300 == 0 && t_last ? (t - t_last) / 300 : 0;
        u64 waited = (g_t_flipwait - wait_last) / 300, busy = per > waited ? per - waited : 0;
        logf("gles: frame %ld shown -> %x (so far: %lu clears, %lu textures, %lu shaders, %lu draw calls, %lu drawn, "
             "%lu triangles; lately %lu.%lu ms per frame, %lu.%lu ms of it not waiting for the display; %s)",
             f, (u32)r, g_clears, g_uploads, g_compiles, g_draw_calls, g_draws, g_prims, per / 1000, per / 100 % 10,
             busy / 1000, busy / 100 % 10, gpu ? "GPU" : "software");
        if (f % 300 == 0) {
            t_last = t, wait_last = g_t_flipwait;
            if (gpu)
                gles_gpu_log("so far");
        }
    }
}

static void *gpu_alloc(u64 bytes) { return direct_alloc(ALIGN_UP(bytes, 0x4000), PROT_READ | PROT_WRITE | PROT_GPU_RW, 0); }
static void gpu_free(void *p) { direct_free(p); }
static void gpu_sleep(u32 us) { K.usleep(us); }

/* Draw on the GPU through SWRR's libSceGnmDriver (gles_gpu.c), unless the
 * build says otherwise (EXT_CFLAGS=-DGLES_GPU=0) */
#ifndef GLES_GPU
#define GLES_GPU 1
#endif
static void gpu_start(void)
{
    if (!GLES_GPU || gles_gpu_on())
        return;
    struct gles_gnm g = {0};
    g.submit = host_real_in("libSceGnmDriver", "sceGnmSubmitCommandBuffers");
    g.submit_done = host_real_in("libSceGnmDriver", "sceGnmSubmitDone");
    g.default_state = host_real_in("libSceGnmDriver", "sceGnmDrawInitDefaultHardwareState350");
    g.gpu_alloc = gpu_alloc, g.gpu_free = gpu_free, g.usec = usec, g.sleep = gpu_sleep;
    g.before_window = gpu_before_window;
    if (!g.submit || !g.submit_done || !g.default_state) {
        logf("gles: libSceGnmDriver's submit functions not found: software drawing");
        return;
    }
    gles_gpu_start(&g);
}
#else
/* this machine (tests/test_gles.py): the test's display and, when it asks, its GPU */
void gles_host_present(const struct surface *s);
const struct gles_gnm *gles_host_gnm(void);
static bool gpu_window(const struct surface *s, struct gles_target *t)
{
    if (s->w % 64)
        return false;
    *t = (struct gles_target){s->back, s->w, s->h, s->w, true, true, NULL};
    return true;
}
static void gpu_before_window(void) {}
static void gpu_start(void) { gles_gpu_start(gles_host_gnm()); }
static void present(const struct surface *s)
{
    static s64 frame;
    frame++;
    struct gles_target t;
    if (gles_gpu_on() && gpu_window(s, &t)) {
        gles_gpu_finish();
        if (C_.on)
            chk_compare(&t, frame);
    }
    gles_host_present(s);
    if (gles_gpu_on() && gpu_window(s, &t))
        chk_next(frame + 1, &t);
}
#endif

/* ---- EGL ---- */

static u32 egl_ok(u32 r)
{
    E_.error = EGL_SUCCESS;
    return r;
}

static u32 egl_fail(u32 e)
{
    E_.error = e;
    return 0;
}

static void *eglGetDisplay_(void *native) { return DISPLAY; }

static u32 eglInitialize_(void *dpy, s32 *major, s32 *minor)
{
    if (dpy != DISPLAY)
        return egl_fail(EGL_BAD_DISPLAY);
    E_.init = true;
    if (major)
        *major = 1;
    if (minor)
        *minor = 4;
    logf("gles: eglInitialize: ghostldr's software GLES2 stands in for Piglet");
    return egl_ok(1);
}

static u32 eglTerminate_(void *dpy)
{
    if (dpy != DISPLAY)
        return egl_fail(EGL_BAD_DISPLAY);
    return egl_ok(1);
}

static u32 eglBindAPI_(u32 api)
{
    if (api != EGL_OPENGL_ES_API)
        return egl_fail(EGL_BAD_PARAMETER);
    E_.api = api;
    return egl_ok(1);
}

static u32 eglQueryAPI_(void) { return E_.api; }

static s32 eglGetError_(void)
{
    s32 e = (s32)(E_.error ? E_.error : EGL_SUCCESS);
    E_.error = EGL_SUCCESS;
    return e;
}

static const char *eglQueryString_(void *dpy, s32 name)
{
    switch (name) {
    case 0x3053: return "ghostldr";                   /* EGL_VENDOR */
    case 0x3054: return "1.4 ghostldr";               /* EGL_VERSION */
    case 0x3055: return "";                           /* EGL_EXTENSIONS */
    case 0x308D: return "OpenGL_ES";                  /* EGL_CLIENT_APIS */
    default: egl_fail(EGL_BAD_PARAMETER); return NULL;
    }
}

static bool config_attr(s32 attr, s32 *v)
{
    switch (attr) {
    case 0x3020: *v = 32; return true;            /* BUFFER_SIZE */
    case 0x3021: case 0x3022: case 0x3023: case 0x3024: *v = 8; return true; /* ALPHA..RED_SIZE */
    case 0x3025: case 0x3026: *v = 0; return true; /* DEPTH, STENCIL */
    case 0x3027: *v = EGL_NONE; return true;      /* CONFIG_CAVEAT */
    case 0x3028: *v = 1; return true;             /* CONFIG_ID */
    case 0x3029: *v = 0; return true;             /* LEVEL */
    case 0x302A: case 0x302C: *v = 8192; return true; /* MAX_PBUFFER_HEIGHT/WIDTH */
    case 0x302B: *v = 8192 * 8192; return true;
    case 0x302D: *v = 0; return true;             /* NATIVE_RENDERABLE */
    case 0x302E: *v = 0; return true;             /* NATIVE_VISUAL_ID */
    case 0x302F: *v = EGL_NONE; return true;
    case 0x3031: case 0x3032: *v = 0; return true; /* SAMPLES, SAMPLE_BUFFERS */
    case 0x3033: *v = 4 | 1; return true;         /* SURFACE_TYPE: WINDOW | PBUFFER */
    case 0x3034: *v = EGL_NONE; return true;      /* TRANSPARENT_TYPE */
    case 0x3039: case 0x303A: *v = 0; return true; /* BIND_TO_TEXTURE */
    case 0x303B: *v = 0; return true;             /* MIN_SWAP_INTERVAL */
    case 0x303C: *v = 1; return true;             /* MAX_SWAP_INTERVAL */
    case 0x303D: case 0x303E: *v = 0; return true;
    case 0x303F: *v = 0x308E; return true;        /* COLOR_BUFFER_TYPE: RGB_BUFFER */
    case 0x3040: case 0x3042: *v = 4; return true; /* RENDERABLE_TYPE, CONFORMANT: ES2 */
    default: return false;
    }
}

static u32 eglChooseConfig_(void *dpy, const s32 *attrs, void **configs, s32 size, s32 *num)
{
    if (dpy != DISPLAY)
        return egl_fail(EGL_BAD_DISPLAY);
    if (!num)
        return egl_fail(EGL_BAD_PARAMETER);
    /* one config: refuse requests it can't meet (depth/stencil asked for are
     * granted as 0 bits, as many drivers do for a 2D surface) */
    bool fits = true;
    for (const s32 *a = attrs; a && a[0] != EGL_NONE; a += 2) {
        s32 v;
        if ((a[0] == 0x3024 || a[0] == 0x3023 || a[0] == 0x3022 || a[0] == 0x3021) && a[1] > 8)
            fits = false;
        if (a[0] == 0x3031 && a[1] > 0) /* SAMPLES */
            fits = false;
        if (a[0] == 0x3040 && (a[1] & ~(s32)(4 | 1)) && config_attr(a[0], &v))
            fits = (a[1] & 4) != 0;
    }
    *num = fits ? 1 : 0;
    if (fits && configs && size > 0)
        configs[0] = CONFIG;
    return egl_ok(1);
}

static u32 eglGetConfigAttrib_(void *dpy, void *cfg, s32 attr, s32 *v)
{
    if (dpy != DISPLAY)
        return egl_fail(EGL_BAD_DISPLAY);
    if (cfg != CONFIG)
        return egl_fail(EGL_BAD_CONFIG);
    if (!v || !config_attr(attr, v))
        return egl_fail(EGL_BAD_ATTRIBUTE);
    return egl_ok(1);
}

static struct surface *new_surface(u32 w, u32 h, bool window)
{
    for (u32 i = 0; i < MAX_SURF; i++)
        if (!E_.surf[i].used) {
            struct surface *s = &E_.surf[i];
            memset(s, 0, sizeof(*s));
            s->bytes = (u64)w * h * 4;
            /* the GPU may draw into it (a pbuffer, or the window if VideoOut fails) */
            s->back = direct_alloc(ALIGN_UP(s->bytes ? s->bytes : 1, 0x4000), PROT_READ | PROT_WRITE | PROT_GPU_RW, 0);
            if (!s->back)
                return NULL;
            memset(s->back, 0, s->bytes);
            s->used = true, s->window = window, s->w = w, s->h = h;
            return s;
        }
    return NULL;
}

/* the native window SDL's PS4 port passes: {u32 id, width, height} */
static void *eglCreateWindowSurface_(void *dpy, void *cfg, void *win, const s32 *attrs)
{
    if (dpy != DISPLAY)
        return (void *)(u64)egl_fail(EGL_BAD_DISPLAY);
    if (cfg != CONFIG)
        return (void *)(u64)egl_fail(EGL_BAD_CONFIG);
    u32 w = 1920, h = 1080;
    if (win) {
        const u32 *nw = win;
        if (nw[1] >= 16 && nw[1] <= 3840 && nw[2] >= 16 && nw[2] <= 2160)
            w = nw[1], h = nw[2];
    }
    LOCK();
    struct surface *s = new_surface(w, h, true);
    UNLOCK();
    if (!s)
        return (void *)(u64)egl_fail(EGL_BAD_ALLOC);
    logf("gles: window surface %ux%u", w, h);
    egl_ok(1);
    return s;
}

static void *eglCreatePbufferSurface_(void *dpy, void *cfg, const s32 *attrs)
{
    if (dpy != DISPLAY)
        return (void *)(u64)egl_fail(EGL_BAD_DISPLAY);
    u32 w = 1, h = 1;
    for (const s32 *a = attrs; a && a[0] != EGL_NONE; a += 2)
        if (a[0] == 0x3057 && a[1] > 0 && a[1] <= 8192)
            w = (u32)a[1];
        else if (a[0] == 0x3056 && a[1] > 0 && a[1] <= 8192)
            h = (u32)a[1];
    LOCK();
    struct surface *s = new_surface(w, h, false);
    UNLOCK();
    if (!s)
        return (void *)(u64)egl_fail(EGL_BAD_ALLOC);
    egl_ok(1);
    return s;
}

static struct surface *surface(void *p)
{
    for (u32 i = 0; i < MAX_SURF; i++)
        if (p == &E_.surf[i] && E_.surf[i].used)
            return &E_.surf[i];
    return NULL;
}

static u32 eglDestroySurface_(void *dpy, void *surf)
{
    struct surface *s = surface(surf);
    if (!s)
        return egl_fail(EGL_BAD_SURFACE);
    LOCK();
    if (E_.draw == s)
        E_.draw = NULL;
    mem_put(s->back);
    memset(s, 0, sizeof(*s));
    UNLOCK();
    return egl_ok(1);
}

static void *eglCreateContext_(void *dpy, void *cfg, void *share, const s32 *attrs)
{
    if (dpy != DISPLAY)
        return (void *)(u64)egl_fail(EGL_BAD_DISPLAY);
    if (cfg != CONFIG)
        return (void *)(u64)egl_fail(EGL_BAD_CONFIG);
    for (const s32 *a = attrs; a && a[0] != EGL_NONE; a += 2)
        if (a[0] == 0x3098 && a[1] != 2) { /* CONTEXT_CLIENT_VERSION */
            logf("gles: eglCreateContext for GLES %d refused (GLES 2 only)", a[1]);
            return (void *)(u64)egl_fail(EGL_BAD_MATCH);
        }
    LOCK();
    bool ok = gl_init();
    UNLOCK();
    if (!ok)
        return (void *)(u64)egl_fail(EGL_BAD_ALLOC);
    egl_ok(1);
    return CONTEXT;
}

static u32 eglDestroyContext_(void *dpy, void *ctx) { return ctx == CONTEXT ? egl_ok(1) : egl_fail(EGL_BAD_CONTEXT); }

static u32 eglMakeCurrent_(void *dpy, void *draw, void *read, void *ctx)
{
    if (!ctx) {
        E_.current = false;
        return egl_ok(1);
    }
    if (ctx != CONTEXT)
        return egl_fail(EGL_BAD_CONTEXT);
    struct surface *s = surface(draw);
    if (!s)
        return egl_fail(EGL_BAD_SURFACE);
    LOCK();
    bool first = !E_.draw;
    E_.draw = s, E_.current = true;
    if (first) /* GL's initial viewport and scissor box: the first surface's size */
        G_.vp[0] = G_.vp[1] = 0, G_.vp[2] = (s32)s->w, G_.vp[3] = (s32)s->h, memcpy(G_.sc, G_.vp, sizeof(G_.sc));
    UNLOCK();
    return egl_ok(1);
}

static u32 eglSwapInterval_(void *dpy, s32 interval)
{
    E_.interval = interval;
    return egl_ok(1);
}

static u32 eglSwapBuffers_(void *dpy, void *surf)
{
    struct surface *s = surface(surf);
    if (!s)
        return egl_fail(EGL_BAD_SURFACE);
    if (!s->window)
        return egl_ok(1);
    LOCK();
    present(s);
    UNLOCK();
    return egl_ok(1);
}

static u32 eglQuerySurface_(void *dpy, void *surf, s32 attr, s32 *v)
{
    struct surface *s = surface(surf);
    if (!s || !v)
        return egl_fail(EGL_BAD_SURFACE);
    if (attr == 0x3057)
        *v = (s32)s->w;
    else if (attr == 0x3056)
        *v = (s32)s->h;
    else if (attr == 0x3028)
        *v = 1;
    else
        return egl_fail(EGL_BAD_ATTRIBUTE);
    return egl_ok(1);
}

static u32 eglWaitGL_(void) { return egl_ok(1); }
static u32 eglWaitNative_(s32 engine) { return egl_ok(1); }
static void *eglGetCurrentContext_(void) { return E_.current ? CONTEXT : NULL; }
static void *eglGetCurrentDisplay_(void) { return E_.current ? DISPLAY : NULL; }
static void *eglGetCurrentSurface_(s32 which) { return E_.current ? E_.draw : NULL; }

static void *eglGetProcAddress_(const char *name);

/* bool scePigletSetConfigurationVSH(const ScePglConfig *): the memory budget
 * and display size the real Piglet would use; nothing to configure here */
static u32 scePigletSetConfigurationVSH_(const void *cfg)
{
    logf("gles: scePigletSetConfigurationVSH: accepted (software GLES2)");
    return 1;
}

static const struct ext_hle_ent k_hle[] = {
    {"libScePigletv2VSH", "eglGetDisplay", eglGetDisplay_, NULL},
    {"libScePigletv2VSH", "eglInitialize", eglInitialize_, NULL},
    {"libScePigletv2VSH", "eglTerminate", eglTerminate_, NULL},
    {"libScePigletv2VSH", "eglBindAPI", eglBindAPI_, NULL},
    {"libScePigletv2VSH", "eglQueryAPI", eglQueryAPI_, NULL},
    {"libScePigletv2VSH", "eglGetError", eglGetError_, NULL},
    {"libScePigletv2VSH", "eglQueryString", eglQueryString_, NULL},
    {"libScePigletv2VSH", "eglChooseConfig", eglChooseConfig_, NULL},
    {"libScePigletv2VSH", "eglGetConfigAttrib", eglGetConfigAttrib_, NULL},
    {"libScePigletv2VSH", "eglCreateWindowSurface", eglCreateWindowSurface_, NULL},
    {"libScePigletv2VSH", "eglCreatePbufferSurface", eglCreatePbufferSurface_, NULL},
    {"libScePigletv2VSH", "eglDestroySurface", eglDestroySurface_, NULL},
    {"libScePigletv2VSH", "eglCreateContext", eglCreateContext_, NULL},
    {"libScePigletv2VSH", "eglDestroyContext", eglDestroyContext_, NULL},
    {"libScePigletv2VSH", "eglMakeCurrent", eglMakeCurrent_, NULL},
    {"libScePigletv2VSH", "eglSwapInterval", eglSwapInterval_, NULL},
    {"libScePigletv2VSH", "eglSwapBuffers", eglSwapBuffers_, NULL},
    {"libScePigletv2VSH", "eglQuerySurface", eglQuerySurface_, NULL},
    {"libScePigletv2VSH", "eglWaitGL", eglWaitGL_, NULL},
    {"libScePigletv2VSH", "eglWaitNative", eglWaitNative_, NULL},
    {"libScePigletv2VSH", "eglGetCurrentContext", eglGetCurrentContext_, NULL},
    {"libScePigletv2VSH", "eglGetCurrentDisplay", eglGetCurrentDisplay_, NULL},
    {"libScePigletv2VSH", "eglGetCurrentSurface", eglGetCurrentSurface_, NULL},
    {"libScePigletv2VSH", "eglGetProcAddress", eglGetProcAddress_, NULL},
    {"libScePigletv2VSH", "scePigletSetConfigurationVSH", scePigletSetConfigurationVSH_, NULL},
    {"libScePigletv2VSH", "glActiveTexture", glActiveTexture_, NULL},
    {"libScePigletv2VSH", "glAttachShader", glAttachShader_, NULL},
    {"libScePigletv2VSH", "glBindAttribLocation", glBindAttribLocation_, NULL},
    {"libScePigletv2VSH", "glBindBuffer", glBindBuffer_, NULL},
    {"libScePigletv2VSH", "glBindFramebuffer", glBindFramebuffer_, NULL},
    {"libScePigletv2VSH", "glBindTexture", glBindTexture_, NULL},
    {"libScePigletv2VSH", "glBlendEquation", glBlendEquation_, NULL},
    {"libScePigletv2VSH", "glBlendEquationSeparate", glBlendEquationSeparate_, NULL},
    {"libScePigletv2VSH", "glBlendFunc", glBlendFunc_, NULL},
    {"libScePigletv2VSH", "glBlendFuncSeparate", glBlendFuncSeparate_, NULL},
    {"libScePigletv2VSH", "glBufferData", glBufferData_, NULL},
    {"libScePigletv2VSH", "glBufferSubData", glBufferSubData_, NULL},
    {"libScePigletv2VSH", "glCheckFramebufferStatus", glCheckFramebufferStatus_, NULL},
    {"libScePigletv2VSH", "glClear", glClear_, NULL},
    {"libScePigletv2VSH", "glClearColor", glClearColor_, NULL},
    {"libScePigletv2VSH", "glColorMask", glColorMask_, NULL},
    {"libScePigletv2VSH", "glCompileShader", glCompileShader_, NULL},
    {"libScePigletv2VSH", "glCreateProgram", glCreateProgram_, NULL},
    {"libScePigletv2VSH", "glCreateShader", glCreateShader_, NULL},
    {"libScePigletv2VSH", "glDeleteBuffers", glDeleteBuffers_, NULL},
    {"libScePigletv2VSH", "glDeleteFramebuffers", glDeleteFramebuffers_, NULL},
    {"libScePigletv2VSH", "glDeleteProgram", glDeleteProgram_, NULL},
    {"libScePigletv2VSH", "glDeleteShader", glDeleteShader_, NULL},
    {"libScePigletv2VSH", "glDeleteTextures", glDeleteTextures_, NULL},
    {"libScePigletv2VSH", "glDepthMask", glDepthMask_, NULL},
    {"libScePigletv2VSH", "glDisable", glDisable_, NULL},
    {"libScePigletv2VSH", "glDisableVertexAttribArray", glDisableVertexAttribArray_, NULL},
    {"libScePigletv2VSH", "glDrawArrays", glDrawArrays_, NULL},
    {"libScePigletv2VSH", "glDrawElements", glDrawElements_, NULL},
    {"libScePigletv2VSH", "glEnable", glEnable_, NULL},
    {"libScePigletv2VSH", "glEnableVertexAttribArray", glEnableVertexAttribArray_, NULL},
    {"libScePigletv2VSH", "glFinish", glFinish_, NULL},
    {"libScePigletv2VSH", "glFlush", glFlush_, NULL},
    {"libScePigletv2VSH", "glFramebufferTexture2D", glFramebufferTexture2D_, NULL},
    {"libScePigletv2VSH", "glGenBuffers", glGenBuffers_, NULL},
    {"libScePigletv2VSH", "glGenFramebuffers", glGenFramebuffers_, NULL},
    {"libScePigletv2VSH", "glGenTextures", glGenTextures_, NULL},
    {"libScePigletv2VSH", "glGenerateMipmap", glGenerateMipmap_, NULL},
    {"libScePigletv2VSH", "glGetAttribLocation", glGetAttribLocation_, NULL},
    {"libScePigletv2VSH", "glGetBooleanv", glGetBooleanv_, NULL},
    {"libScePigletv2VSH", "glGetError", glGetError_, NULL},
    {"libScePigletv2VSH", "glGetFloatv", glGetFloatv_, NULL},
    {"libScePigletv2VSH", "glGetIntegerv", glGetIntegerv_, NULL},
    {"libScePigletv2VSH", "glGetProgramInfoLog", glGetProgramInfoLog_, NULL},
    {"libScePigletv2VSH", "glGetProgramiv", glGetProgramiv_, NULL},
    {"libScePigletv2VSH", "glGetShaderInfoLog", glGetShaderInfoLog_, NULL},
    {"libScePigletv2VSH", "glGetShaderiv", glGetShaderiv_, NULL},
    {"libScePigletv2VSH", "glGetString", glGetString_, NULL},
    {"libScePigletv2VSH", "glGetUniformLocation", glGetUniformLocation_, NULL},
    {"libScePigletv2VSH", "glHint", glHint_, NULL},
    {"libScePigletv2VSH", "glIsEnabled", glIsEnabled_, NULL},
    {"libScePigletv2VSH", "glLineWidth", glLineWidth_, NULL},
    {"libScePigletv2VSH", "glLinkProgram", glLinkProgram_, NULL},
    {"libScePigletv2VSH", "glPixelStorei", glPixelStorei_, NULL},
    {"libScePigletv2VSH", "glReadPixels", glReadPixels_, NULL},
    {"libScePigletv2VSH", "glScissor", glScissor_, NULL},
    {"libScePigletv2VSH", "glShaderBinary", glShaderBinary_, NULL},
    {"libScePigletv2VSH", "glShaderSource", glShaderSource_, NULL},
    {"libScePigletv2VSH", "glTexImage2D", glTexImage2D_, NULL},
    {"libScePigletv2VSH", "glTexParameterf", glTexParameterf_, NULL},
    {"libScePigletv2VSH", "glTexParameteri", glTexParameteri_, NULL},
    {"libScePigletv2VSH", "glTexSubImage2D", glTexSubImage2D_, NULL},
    {"libScePigletv2VSH", "glUniform1f", glUniform1f_, NULL},
    {"libScePigletv2VSH", "glUniform1i", glUniform1i_, NULL},
    {"libScePigletv2VSH", "glUniform4f", glUniform4f_, NULL},
    {"libScePigletv2VSH", "glUniform4fv", glUniform4fv_, NULL},
    {"libScePigletv2VSH", "glUniformMatrix4fv", glUniformMatrix4fv_, NULL},
    {"libScePigletv2VSH", "glUseProgram", glUseProgram_, NULL},
    {"libScePigletv2VSH", "glVertexAttribPointer", glVertexAttribPointer_, NULL},
    {"libScePigletv2VSH", "glViewport", glViewport_, NULL},
};

static void *eglGetProcAddress_(const char *name)
{
    static u32 said;
    if (said++ < 100)
        logf("gles: eglGetProcAddress(%s)", name ? name : "(null)");
    for (u32 i = 0; name && i < ARRAY_SIZE(k_hle); i++)
        if (strcmp(k_hle[i].name, name) == 0)
            return k_hle[i].fn;
    return NULL;
}

/* the whole library is here: nothing of it falls back to a host module, and
 * sceKernelLoadStartModule of libScePigletv2VSH.sprx gets a virtual handle */
static const struct ext_lib k_libs[] = {
    {LIB, NULL, EXT_LIB_NO_HOST | EXT_LIB_VIRTUAL, 0},
};

EXT_FAMILY_LIBS("gles", k_hle, k_libs, NULL);
