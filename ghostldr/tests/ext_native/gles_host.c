/*
 * Runs the Piglet family (hle_gles.c, gles_raster.c), built for this machine
 * with -DGLES_HOST, the way SDL2's GLES2 renderer drives it: EGL set-up, its
 * shaders (SDL is zlib-licensed; the sources below are SDL 2.0.18+'s), then
 * clears, rectangles, textures at 1:1 and scaled, blending, render targets,
 * scissoring, points and lines, glReadPixels, and what eglSwapBuffers shows.
 * test_gles.py builds and drives it:
 *
 *     gles_host shaders|draw|perf
 *
 * Prints "ok ..." / "FAILED ..." lines and the family's log; exits 0 when
 * every check passed.
 *
 * Built with -DGLES_CONSOLE (and the OpenOrbis toolchain) it is a ghostldr
 * ELF loader payload instead: the same scenarios inside SWRR on a console,
 * "draw-gpu" and "perf-gpu" on the console's real GPU (gles_gpu.c through
 * SWRR's libSceGnmDriver), with every frame checked against the software path.
 * The scenario is GLES_SCENARIO, as the payload gets no arguments:
 *
 *   for f in gles_host.c ../../native/src/nid.c ../../native/ext/src/{hle_gles,gles_raster,gles_gpu}.c; do
 *     clang --target=x86_64-pc-freebsd12-elf -fPIC -O2 -std=gnu11 -ffreestanding -fno-builtin -DGLES_HOST \
 *       -DGLES_CONSOLE -DGLES_SCENARIO='"draw-gpu"' -I. -I../../native/ext/include -I../../native/include \
 *       -DGHOST_ABI=0u -c $f; done
 *   ld.lld *.o -o gles_draw_gpu.elf -m elf_x86_64 -pie --script $TC/link.x -L$TC/lib -lc -lkernel \
 *     -lSceGnmDriver $TC/lib/crt1.o            (TC: the OpenOrbis toolchain)
 *   python3 ghostldr/host/elf_send.py gles_draw_gpu.elf        (with run.py elfldr serving)
 *
 * ("perf-gpu" and "perf" with -DGLES_CHECK=0 time a 1080p frame on the GPU and in software.)
 */
#include "ghost_api.h"
#include "../../native/ext/src/gles.h"
#include "../../native/ext/src/gles_gcn.h"
/* gles.h brings ext.h's names for the core's services: this file is the core */
#undef K
#undef logf
#undef compute_nid
#undef page_alloc
#undef page_free
#undef direct_alloc
#undef direct_free
#undef spin_lock
#undef spin_unlock
#undef g_ctx
#undef g_modules
#undef g_main_mod

long write(int fd, const void *buf, unsigned long n);
void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off);
int munmap(void *addr, unsigned long len);
int vsnprintf(char *buf, unsigned long size, const char *fmt, __builtin_va_list ap);
void *calloc(unsigned long n, unsigned long size);
void free(void *p);
int posix_memalign(void **out, unsigned long align, unsigned long size);
long clock(void);
int snprintf(char *buf, unsigned long size, const char *fmt, ...);

void compute_nid(const char *name, char out[12]);

struct kfuncs K;
static struct boot_ctx g_ctx_v;
static struct boot_ctx *g_ctx_p = &g_ctx_v;
static struct module *g_modules_p, *g_main_p;
static u64 g_dmem;
const struct ghost_api *G;
static int g_failed;

static const struct ext_family *g_fam[8];
static u32 g_nfam;
void harness_family(const struct ext_family *f) { g_fam[g_nfam++] = f; }

static void out(const char *f, __builtin_va_list ap)
{
    char line[1200];
    int n = vsnprintf(line, sizeof(line) - 1, f, ap);
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    write(1, line, n);
}

static void hlog(const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    out(f, ap);
    __builtin_va_end(ap);
}

static void expect(bool ok, const char *what)
{
    hlog("%s %s", ok ? "ok" : "FAILED", what);
    if (!ok)
        g_failed = 1;
}

static void *fn(const char *name)
{
    for (u32 k = 0; k < g_nfam; k++)
        for (u32 i = 0; i < g_fam[k]->nhle; i++)
            if (strcmp(g_fam[k]->hle[i].name, name) == 0)
                return g_fam[k]->hle[i].fn;
    hlog("FAILED no %s", name);
    g_failed = 1;
    return NULL;
}

/* "direct memory": below 1 TiB, as the console's, so the GPU's 40-bit
 * addresses reach it (the fake GPU below reads them back from registers) */
#define LOW_BASE 0x8000000000ull
static u64 g_low_next = LOW_BASE;
static struct {
    void *p;
    u64 size;
} g_low[16384];
#ifdef GLES_CONSOLE
int sceKernelAllocateDirectMemory(s64 start, s64 end, u64 len, u64 align, int type, s64 *out);
int sceKernelMapDirectMemory(void **addr, u64 len, int prot, int flags, s64 phys, u64 align);
int sceKernelReleaseDirectMemory(s64 phys, u64 len);
int sceKernelMunmap(void *addr, u64 len);
u64 sceKernelGetDirectMemorySize(void);
u64 sceKernelGetProcessTime(void);
int sceKernelUsleep(u32 us);
s32 sceGnmSubmitCommandBuffers(u32 n, void **dcb, u32 *dcb_bytes, void **ccb, u32 *ccb_bytes);
s32 sceGnmSubmitDone(void);
s32 sceGnmDrawInitDefaultHardwareState350(u32 *cmd, u64 ndw);
static struct {
    void *p;
    u64 size;
    s64 phys;
} g_dm[4096];
/* the console's direct memory, mapped for the CPU and the GPU (onion), as ghostldr's direct_alloc */
static void *direct_alloc_(size_t size, int prot, u64 fixed)
{
    size = (size + 0xFFFF) & ~(size_t)0xFFFF;
    s64 phys = 0;
    void *a = NULL;
    if (sceKernelAllocateDirectMemory(0, (s64)sceKernelGetDirectMemorySize(), size, 0x10000, 0, &phys) != 0)
        return NULL;
    if (sceKernelMapDirectMemory(&a, size, 0x33, 0, phys, 0x10000) != 0) {
        sceKernelReleaseDirectMemory(phys, size);
        return NULL;
    }
    for (u32 i = 0; i < 4096; i++)
        if (!g_dm[i].p) {
            g_dm[i].p = a, g_dm[i].size = size, g_dm[i].phys = phys;
            return a;
        }
    return a;
}
static void direct_free_(void *p)
{
    for (u32 i = 0; p && i < 4096; i++)
        if (g_dm[i].p == p) {
            sceKernelMunmap(p, g_dm[i].size);
            sceKernelReleaseDirectMemory(g_dm[i].phys, g_dm[i].size);
            g_dm[i].p = NULL;
            return;
        }
}
static void direct_free_all(void)
{
    for (u32 i = 0; i < 4096; i++)
        if (g_dm[i].p)
            direct_free_(g_dm[i].p);
}
#define clock() ((long)sceKernelGetProcessTime())
#else
static void *direct_alloc_(size_t size, int prot, u64 fixed)
{
    size = (size + 0xFFFF) & ~(size_t)0xFFFF;
    for (u32 tries = 0; tries < 64; tries++) {
        void *want = (void *)g_low_next;
        g_low_next += size + 0x10000;
        void *p = mmap(want, size, 3, 0x1002 /* MAP_ANON | MAP_PRIVATE */, -1, 0);
        if (p == (void *)-1)
            continue;
        if ((u64)p >= (1ull << 40)) {
            munmap(p, size);
            continue;
        }
        for (u32 i = 0; i < 16384; i++)
            if (!g_low[i].p) {
                g_low[i].p = p, g_low[i].size = size;
                return p;
            }
        munmap(p, size);
        return NULL;
    }
    return NULL;
}
static void direct_free_(void *p)
{
    for (u32 i = 0; p && i < 16384; i++)
        if (g_low[i].p == p) {
            munmap(p, g_low[i].size);
            g_low[i].p = NULL;
            return;
        }
}
#endif
static void *page_alloc_(size_t size) { return calloc(1, size); }
static void page_free_(void *p, size_t size) { free(p); }
static void spin_lock_(volatile int *l)
{
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        ;
}
static void spin_unlock_(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

const struct ext_lib *ext_find_policy(const char *lib) { return NULL; }
u64 ext_find(const char *lib, const char *nid) { return 0; }

/* what eglSwapBuffers showed */
struct surface_view {
    bool used, window;
    u32 w, h;
    u32 *back;
};
static u32 g_shown[64 * 48];
static u32 g_shown_frames;
void gles_host_present(const void *s)
{
    const struct surface_view *v = s;
    if (v->w == 64 && v->h == 48)
        memcpy(g_shown, v->back, sizeof(g_shown));
    g_shown_frames++;
}

/* ---- a fake GPU: runs the PM4 packets gles_gpu.c writes with the software
 * rasteriser, from the registers alone, so every field gles_gpu.c encodes
 * (targets, viewport, scissor, blending, the vertex ring, texture
 * descriptors and samplers) must say what the software path draws ---- */
static bool g_fake_gpu;
static u32 g_cr[0x400], g_sr[0x400], g_prim;
static u64 g_gpu_draws, g_gpu_submits;
#define CR(r) g_cr[((r) - 0x28000) / 4]
#define SR(r) g_sr[((r) - 0xB000) / 4]

static float fl(u32 u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static u32 gl_wrap(u32 c) { return c == 0 ? GL_REPEAT : c == 1 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE; }
static u32 gl_factor(u32 f)
{
    static const u32 k[11] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
                              GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR,
                              GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE};
    return f < 11 ? k[f] : 0xFFFF;
}
static u32 gl_eq(u32 e)
{
    static const u32 k[5] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_MIN, GL_MAX, GL_FUNC_REVERSE_SUBTRACT};
    return e < 5 ? k[e] : 0xFFFF;
}

static void fake_draw(u32 count)
{
    g_gpu_draws++;
    u32 info = CR(0x28C70);
    struct gles_target t = {(u32 *)((u64)CR(0x28C60) << 8), CR(0x28034) & 0xFFFF, CR(0x28034) >> 16,
                            ((CR(0x28C64) & 0x7FF) + 1) * 8, ((info >> 11) & 3) == 1, false, NULL};
    if (((info >> 2) & 31) != 0xA || (CR(0x28C74) & 31) != 8 || !(CR(0x28240) & 0x80000000u)) {
        hlog("FAILED fake GPU: colour buffer info %x attrib %x", info, CR(0x28C74)), g_failed = 1;
        return;
    }
    u32 tl = CR(0x28240), br = CR(0x28244);
    struct gles_draw d;
    memset(&d, 0, sizeof(d));
    d.dst = &t;
    d.clip = (struct gles_rect){(s32)(tl & 0x7FFF), (s32)((tl >> 16) & 0x7FFF), (s32)(br & 0x7FFF),
                                (s32)((br >> 16) & 0x7FFF)};
    u32 b = CR(0x28780);
    d.blend = (b >> 30) & 1;
    d.src_rgb = gl_factor(b & 31), d.eq_rgb = gl_eq((b >> 5) & 7), d.dst_rgb = gl_factor((b >> 8) & 31);
    d.src_a = gl_factor((b >> 16) & 31), d.eq_a = gl_eq((b >> 21) & 7), d.dst_a = gl_factor((b >> 24) & 31);
    if (d.blend && !((b >> 29) & 1))
        d.src_a = d.src_rgb, d.dst_a = d.dst_rgb, d.eq_a = d.eq_rgb;
    const u32 *tab = (const u32 *)((u64)SR(0xB030) | (u64)SR(0xB034) << 32);
    const u32 *code = (const u32 *)((u64)SR(0xB020) << 8 | (u64)(SR(0xB024) & 0xFF) << 40);
    bool yuv = memcmp(code, k_gles_ps_yuv, sizeof(k_gles_ps_yuv)) == 0;
    if (!yuv && memcmp(code, k_gles_ps, sizeof(k_gles_ps)) != 0) {
        hlog("FAILED fake GPU: an unknown pixel shader"), g_failed = 1;
        return;
    }
    static struct gles_tex tex[3];
    static struct gles_fs fs;
    memset(tex, 0, sizeof(tex));
    memset(&fs, 0, sizeof(fs));
    fs.kind = yuv ? FS_YUV : FS_TEX;
    fs.flags = FSF_MUL_VCOLOR | FSF_MUL_UCOLOR;
    u32 sel[3][4];
    for (int i = 0; i < (yuv ? 3 : 1); i++) {
        const u32 *td = tab + 12 * i;
        struct gles_tex *x = &tex[i];
        x->pix = (u32 *)((u64)td[0] << 8 | (u64)(td[1] & 0xFF) << 40);
        x->w = (td[2] & 0x3FFF) + 1, x->h = ((td[2] >> 14) & 0x3FFF) + 1, x->pitch = ((td[4] >> 13) & 0x3FFF) + 1;
        for (int c = 0; c < 4; c++)
            sel[i][c] = (td[3] >> (3 * c)) & 7;
        if (((td[1] >> 20) & 63) != 10 || (td[3] >> 28) != 9 || ((td[3] >> 20) & 31) != 8 || sel[i][1] != 5)
            hlog("FAILED fake GPU: texture descriptor %08x %08x %08x", td[1], td[3], td[4]), g_failed = 1;
        x->wrap_s = gl_wrap(td[8] & 7), x->wrap_t = gl_wrap((td[8] >> 3) & 7);
        x->mag_filter = ((td[10] >> 20) & 3) ? GL_LINEAR : GL_NEAREST;
        x->min_filter = ((td[10] >> 22) & 3) ? GL_LINEAR : GL_NEAREST;
        d.tex[i] = x;
    }
    if (!yuv) {
        if (sel[0][0] == 6 && sel[0][2] == 4)
            fs.flags |= FSF_SWAP_RB;
        else if (sel[0][0] != 4 || sel[0][2] != 6)
            hlog("FAILED fake GPU: channel selects %u %u %u %u", sel[0][0], sel[0][1], sel[0][2], sel[0][3]),
                g_failed = 1;
        if (sel[0][3] == 1)
            fs.flags |= FSF_ALPHA_ONE;
        for (int k = 0; k < 4; k++)
            d.ucolor[k] = fl(tab[12 + k]);
    } else {
        static const char ch[8] = {0, 0, 0, 0, 'r', 'g', 'b', 'a'};
        if (tex[1].pix == tex[2].pix) /* one interleaved plane: NV12/NV21 */
            fs.kind = FS_NV, fs.uv_swz[0] = ch[sel[1][0]], fs.uv_swz[1] = ch[sel[2][0]];
        else if (sel[1][0] != 4 || sel[2][0] != 4)
            hlog("FAILED fake GPU: U/V planes read from %u %u", sel[1][0], sel[2][0]), g_failed = 1;
        if (sel[0][0] != 4)
            hlog("FAILED fake GPU: Y plane read from %u", sel[0][0]), g_failed = 1;
        for (int k = 0; k < 4; k++)
            d.ucolor[k] = fl(tab[36 + k]);
        for (int k = 0; k < 3; k++)
            fs.offset[k] = fl(tab[40 + k]);
        for (int k = 0; k < 9; k++)
            fs.mat[k] = fl(tab[43 + k]);
    }
    d.fs = &fs;
    u64 ring = (u64)SR(0xB130) | (u64)(SR(0xB134) & 0xFFFF) << 32;
    u32 stride = (SR(0xB134) >> 16) & 0x3FFF, first = SR(0xB140);
    float xs = fl(CR(0x2843C)), xo = fl(CR(0x28440)), ys = fl(CR(0x28444)), yo = fl(CR(0x28448));
    static struct gles_vert v[4096];
    if (count > 4096)
        count = 4096;
    for (u32 i = 0; i < count; i++) {
        const float *r = (const float *)(ring + (u64)(first + i) * stride);
        v[i].x = r[0] * xs + xo, v[i].y = r[1] * ys + yo, v[i].u = r[2], v[i].v = r[3];
        memcpy(v[i].c, r + 4, 16);
    }
    if (g_prim == 4)
        for (u32 k = 0; k + 2 < count; k += 3)
            gles_triangle(&d, &v[k], &v[k + 1], &v[k + 2]);
    else if (g_prim == 6)
        for (u32 k = 0; k + 2 < count; k++)
            gles_triangle(&d, &v[k], &v[k + 1], &v[k + 2]);
    else if (g_prim == 5)
        for (u32 k = 1; k + 1 < count; k++)
            gles_triangle(&d, &v[0], &v[k], &v[k + 1]);
    else
        hlog("FAILED fake GPU: primitive type %u", g_prim), g_failed = 1;
}

static s32 fake_submit(u32 n, void **dcb, u32 *bytes, void **ccb, u32 *ccb_bytes)
{
    g_gpu_submits++;
    for (u32 k = 0; k < n; k++) {
        const u32 *p = dcb[k], *end = (const u32 *)((const u8 *)dcb[k] + bytes[k]);
        while (p < end) {
            u32 h = p[0], op = (h >> 8) & 0xFF, cnt = ((h >> 16) & 0x3FFF) + 1;
            if (h >> 30 != 3 || p + 1 + cnt > end) {
                hlog("FAILED fake GPU: bad packet %08x", h), g_failed = 1;
                return -1;
            }
            switch (op) {
            case 0x69:
                for (u32 i = 0; i + 1 < cnt; i++)
                    g_cr[(p[1] + i) & 0x3FF] = p[2 + i];
                break;
            case 0x76:
                for (u32 i = 0; i + 1 < cnt; i++)
                    g_sr[(p[1] + i) & 0x3FF] = p[2 + i];
                break;
            case 0x79:
                if (p[1] == (0x30908 - 0x30000) / 4)
                    g_prim = p[2];
                break;
            case 0x2D: fake_draw(p[1]); break;
            case 0x47: *(volatile u32 *)((u64)p[2] | (u64)(p[3] & 0xFFFF) << 32) = p[4]; break;
            case 0x2F: case 0x43: case 0x46: break;
            default: hlog("FAILED fake GPU: packet %02x", op), g_failed = 1;
            }
            p += cnt + 1;
        }
    }
    return 0;
}
static s32 fake_done(void) { return 0; }
static s32 fake_state(u32 *cmd, u64 ndw) { return 0; }
static void *fake_alloc(u64 bytes) { return direct_alloc_(bytes, 0x33, 0); }
static u64 fake_usec(void) { return (u64)clock(); }
#ifdef GLES_CONSOLE
static void real_sleep(u32 us) { sceKernelUsleep(us); }
static const struct gles_gnm g_fake = {sceGnmSubmitCommandBuffers, sceGnmSubmitDone,
                                       sceGnmDrawInitDefaultHardwareState350, fake_alloc, direct_free_,
                                       sceKernelGetProcessTime, real_sleep, NULL};
#else
static const struct gles_gnm g_fake = {fake_submit, fake_done, fake_state, fake_alloc, direct_free_, fake_usec, NULL,
                                       NULL};
#endif
const struct gles_gnm *gles_host_gnm(void) { return g_fake_gpu ? &g_fake : NULL; }

static struct ghost_api g_api;
static void setup(void)
{
    g_api = (struct ghost_api){
        .version = GHOST_API_VERSION, .size = sizeof(struct ghost_api), .ctx = &g_ctx_p, .k = &K,
        .modules = &g_modules_p, .main_mod = &g_main_p, .reported_dmem = &g_dmem, .logf = hlog,
        .compute_nid = compute_nid, .page_alloc = page_alloc_, .page_free = page_free_,
        .direct_alloc = direct_alloc_, .direct_free = direct_free_, .spin_lock = spin_lock_,
        .spin_unlock = spin_unlock_,
    };
    G = &g_api;
}

/* ---- SDL 2.0.18+'s GLES2 shaders (SDL_shaders_gles2.c, zlib licence) ---- */
static const char k_vs[] =
    "    uniform mat4 u_projection;     attribute vec2 a_position;     attribute vec4 a_color;     attribute vec2 "
    "a_texCoord;     varying vec2 v_texCoord;     varying vec4 v_color;         void main()     {         "
    "v_texCoord = a_texCoord;         gl_Position = u_projection * vec4(a_position, 0.0, 1.0);        "
    "gl_PointSize = 1.0;         v_color = a_color;     } ";
static const char k_fs_solid[] =
    "    precision mediump float;     varying vec4 v_color;         void main()     {         gl_FragColor = "
    "v_color;     } ";
static const char k_fs_abgr[] =
    "    precision mediump float;     uniform sampler2D u_texture;     varying vec4 v_color;     varying vec2 "
    "v_texCoord;         void main()     {         gl_FragColor = texture2D(u_texture, v_texCoord);         "
    "gl_FragColor *= v_color;     } ";
static const char k_fs_argb[] =
    "    precision mediump float;     uniform sampler2D u_texture;     varying vec4 v_color;     varying vec2 "
    "v_texCoord;         void main()     {         vec4 abgr = texture2D(u_texture, v_texCoord);         "
    "gl_FragColor = abgr;         gl_FragColor.r = abgr.b;         gl_FragColor.b = abgr.r;         "
    "gl_FragColor *= v_color;     } ";
static const char k_fs_rgb[] =
    "    precision mediump float;     uniform sampler2D u_texture;     varying vec4 v_color;     varying vec2 "
    "v_texCoord;         void main()     {         vec4 abgr = texture2D(u_texture, v_texCoord);         "
    "gl_FragColor = abgr;         gl_FragColor.r = abgr.b;         gl_FragColor.b = abgr.r;         "
    "gl_FragColor.a = 1.0;         gl_FragColor *= v_color;     } ";
static const char k_fs_bgr[] =
    "    precision mediump float;     uniform sampler2D u_texture;     varying vec4 v_color;     varying vec2 "
    "v_texCoord;         void main()     {         vec4 abgr = texture2D(u_texture, v_texCoord);         "
    "gl_FragColor = abgr;         gl_FragColor.a = 1.0;         gl_FragColor *= v_color;     } ";
static const char k_fs_yuv[] =
    "precision mediump float;\nuniform sampler2D u_texture;\nuniform sampler2D u_texture_u;\nuniform sampler2D "
    "u_texture_v;\nvarying vec4 v_color;\nvarying vec2 v_texCoord;\n\n// YUV offset \nconst vec3 offset = vec3(0, "
    "-0.501960814, -0.501960814);\n\n// RGB coefficients \nconst mat3 matrix = mat3( 1,       1,        1,\n      "
    "                    0,      -0.3441,   1.772,\n                          1.402,  -0.7141,   0);\n\nvoid main()\n"
    "{\n    mediump vec3 yuv;\n    lowp vec3 rgb;\n\n    // Get the YUV values \n    yuv.x = texture2D(u_texture,   "
    "v_texCoord).r;\n    yuv.y = texture2D(u_texture_u, v_texCoord).r;\n    yuv.z = texture2D(u_texture_v, "
    "v_texCoord).r;\n\n    // Do the color transform \n    yuv += offset;\n    rgb = matrix * yuv;\n\n    // That "
    "was easy. :) \n    gl_FragColor = vec4(rgb, 1);\n    gl_FragColor *= v_color;\n}";
static const char k_fs_nv21[] =
    "precision mediump float;\nuniform sampler2D u_texture;\nuniform sampler2D u_texture_u;\nuniform sampler2D "
    "u_texture_v;\nvarying vec4 v_color;\nvarying vec2 v_texCoord;\nconst vec3 offset = vec3(-0.0627451017, "
    "-0.501960814, -0.501960814);\nconst mat3 matrix = mat3( 1.1644,  1.1644,   1.1644,\n 0,      -0.3918,   "
    "2.0172,\n 1.596,  -0.813,    0);\nvoid main()\n{\n    mediump vec3 yuv;\n    lowp vec3 rgb;\n    yuv.x = "
    "texture2D(u_texture,   v_texCoord).r;\n    yuv.yz = texture2D(u_texture_u, v_texCoord).ar;\n    yuv += "
    "offset;\n    rgb = matrix * yuv;\n    gl_FragColor = vec4(rgb, 1);\n    gl_FragColor *= v_color;\n}";
static const char k_fs_ext[] =
    "    #extension GL_OES_EGL_image_external : require\n    precision mediump float;     uniform "
    "samplerExternalOES u_texture;     varying vec4 v_color;     varying vec2 v_texCoord;         void main()     { "
    "        gl_FragColor = texture2D(u_texture, v_texCoord);         gl_FragColor *= v_color;     } ";

static void scenario_shaders(void)
{
    struct gles_vs vs;
    struct gles_fs fs;
    char why[200];
    expect(gles_vs_parse(k_vs, &vs, why, sizeof(why)) == 0 && vs.flags == (VSF_TEXCOORD | VSF_COLOR),
           "SDL's vertex shader: texture coordinates and colour");
    expect(gles_fs_parse(k_fs_solid, &fs, why, sizeof(why)) == 0 && fs.kind == FS_SOLID, "solid");
    expect(gles_fs_parse(k_fs_abgr, &fs, why, sizeof(why)) == 0 && fs.kind == FS_TEX &&
               fs.flags == FSF_MUL_VCOLOR,
           "ABGR texture");
    expect(gles_fs_parse(k_fs_argb, &fs, why, sizeof(why)) == 0 && fs.kind == FS_TEX &&
               fs.flags == (FSF_MUL_VCOLOR | FSF_SWAP_RB),
           "ARGB texture: red and blue swapped");
    expect(gles_fs_parse(k_fs_rgb, &fs, why, sizeof(why)) == 0 &&
               fs.flags == (FSF_MUL_VCOLOR | FSF_SWAP_RB | FSF_ALPHA_ONE),
           "RGB texture: swapped, alpha 1");
    expect(gles_fs_parse(k_fs_bgr, &fs, why, sizeof(why)) == 0 && fs.flags == (FSF_MUL_VCOLOR | FSF_ALPHA_ONE),
           "BGR texture: alpha 1");
    expect(gles_fs_parse(k_fs_yuv, &fs, why, sizeof(why)) == 0 && fs.kind == FS_YUV && fs.offset[1] < -0.5f &&
               fs.offset[1] > -0.502f && fs.mat[5] > 1.77f && fs.mat[5] < 1.773f && fs.mat[6] > 1.401f,
           "YUV (JPEG): offset and matrix read");
    expect(gles_fs_parse(k_fs_nv21, &fs, why, sizeof(why)) == 0 && fs.kind == FS_NV && fs.uv_swz[0] == 'a' &&
               fs.uv_swz[1] == 'r' && fs.offset[0] < -0.06f,
           "NV21 (BT.601): planes swizzled .ar");
    expect(gles_fs_parse(k_fs_ext, &fs, why, sizeof(why)) == 0 && fs.kind == FS_TEX, "external OES texture");
    expect(gles_fs_parse("void main() { gl_FragColor = vec4(sin(gl_FragCoord.x)); }", &fs, why, sizeof(why)) != 0 &&
               strstr(why, "unknown form"),
           "an unknown fragment shader is refused, and says why");
    expect(gles_vs_parse("void main() { gl_Position = vec4(1.0); }", &vs, why, sizeof(why)) != 0,
           "an unknown vertex shader is refused");
    /* sin/cos without libm */
    float s = gles_sinf(0.5235987756f), c = gles_cosf(1.0471975512f), z = gles_sinf(3.14159265f);
    expect(s > 0.4999f && s < 0.5001f && c > 0.4999f && c < 0.5001f && z > -0.0001f && z < 0.0001f,
           "sin(30 deg), cos(60 deg), sin(180 deg)");
}

/* ---- the GL calls, as SDL's renderer makes them ---- */
#define GLP(ret, name, args) static ret(*name) args
GLP(void *, eglGetDisplay, (void *));
GLP(u32, eglInitialize, (void *, s32 *, s32 *));
GLP(u32, eglChooseConfig, (void *, const s32 *, void **, s32, s32 *));
GLP(u32, eglGetConfigAttrib, (void *, void *, s32, s32 *));
GLP(void *, eglCreateWindowSurface, (void *, void *, void *, const s32 *));
GLP(void *, eglCreateContext, (void *, void *, void *, const s32 *));
GLP(u32, eglMakeCurrent, (void *, void *, void *, void *));
GLP(u32, eglSwapBuffers, (void *, void *));
GLP(void *, eglGetProcAddress, (const char *));
GLP(u32, scePigletSetConfigurationVSH, (const void *));
GLP(u32, glCreateShader, (u32));
GLP(void, glShaderSource, (u32, s32, const char *const *, const s32 *));
GLP(void, glCompileShader, (u32));
GLP(void, glShaderBinary, (s32, const u32 *, u32, const void *, s32));
GLP(void, glGetShaderiv, (u32, u32, s32 *));
GLP(void, glGetShaderInfoLog, (u32, s32, s32 *, char *));
GLP(u32, glCreateProgram, (void));
GLP(void, glAttachShader, (u32, u32));
GLP(void, glBindAttribLocation, (u32, u32, const char *));
GLP(void, glLinkProgram, (u32));
GLP(void, glGetProgramiv, (u32, u32, s32 *));
GLP(void, glUseProgram, (u32));
GLP(s32, glGetUniformLocation, (u32, const char *));
GLP(void, glUniform1i, (s32, s32));
GLP(void, glUniformMatrix4fv, (s32, s32, u8, const float *));
GLP(void, glGenBuffers, (s32, u32 *));
GLP(void, glBindBuffer, (u32, u32));
GLP(void, glBufferData, (u32, s64, const void *, u32));
GLP(void, glVertexAttribPointer, (u32, s32, u32, u8, s32, const void *));
GLP(void, glEnableVertexAttribArray, (u32));
GLP(void, glDisableVertexAttribArray, (u32));
GLP(void, glDrawArrays, (u32, s32, s32));
GLP(void, glViewport, (s32, s32, s32, s32));
GLP(void, glScissor, (s32, s32, s32, s32));
GLP(void, glEnable, (u32));
GLP(void, glDisable, (u32));
GLP(void, glBlendFuncSeparate, (u32, u32, u32, u32));
GLP(void, glBlendEquationSeparate, (u32, u32));
GLP(void, glClearColor, (float, float, float, float));
GLP(void, glClear, (u32));
GLP(void, glGenTextures, (s32, u32 *));
GLP(void, glBindTexture, (u32, u32));
GLP(void, glActiveTexture, (u32));
GLP(void, glTexParameteri, (u32, u32, s32));
GLP(void, glTexImage2D, (u32, s32, s32, s32, s32, s32, u32, u32, const void *));
GLP(void, glTexSubImage2D, (u32, s32, s32, s32, s32, s32, u32, u32, const void *));
GLP(void, glGenFramebuffers, (s32, u32 *));
GLP(void, glBindFramebuffer, (u32, u32));
GLP(void, glFramebufferTexture2D, (u32, u32, u32, u32, s32));
GLP(u32, glCheckFramebufferStatus, (u32));
GLP(void, glReadPixels, (s32, s32, s32, s32, u32, u32, void *));
GLP(u32, glGetError, (void));
GLP(void, glGetIntegerv, (u32, s32 *));
GLP(const char *, glGetString, (u32));
GLP(void, glPixelStorei, (u32, s32));

#define BIND(n) n = (__typeof__(n))fn(#n)
static void bind_all(void)
{
    BIND(eglGetDisplay), BIND(eglInitialize), BIND(eglChooseConfig), BIND(eglGetConfigAttrib);
    BIND(eglCreateWindowSurface), BIND(eglCreateContext), BIND(eglMakeCurrent), BIND(eglSwapBuffers);
    BIND(eglGetProcAddress), BIND(scePigletSetConfigurationVSH);
    BIND(glCreateShader), BIND(glShaderSource), BIND(glCompileShader), BIND(glGetShaderiv), BIND(glGetShaderInfoLog);
    BIND(glShaderBinary);
    BIND(glCreateProgram), BIND(glAttachShader), BIND(glBindAttribLocation), BIND(glLinkProgram), BIND(glGetProgramiv);
    BIND(glUseProgram), BIND(glGetUniformLocation), BIND(glUniform1i), BIND(glUniformMatrix4fv);
    BIND(glGenBuffers), BIND(glBindBuffer), BIND(glBufferData), BIND(glVertexAttribPointer);
    BIND(glEnableVertexAttribArray), BIND(glDisableVertexAttribArray), BIND(glDrawArrays), BIND(glViewport);
    BIND(glScissor), BIND(glEnable), BIND(glDisable), BIND(glBlendFuncSeparate), BIND(glBlendEquationSeparate);
    BIND(glClearColor), BIND(glClear), BIND(glGenTextures), BIND(glBindTexture), BIND(glActiveTexture);
    BIND(glTexParameteri), BIND(glTexImage2D), BIND(glTexSubImage2D), BIND(glGenFramebuffers);
    BIND(glBindFramebuffer), BIND(glFramebufferTexture2D), BIND(glCheckFramebufferStatus), BIND(glReadPixels);
    BIND(glGetError), BIND(glGetIntegerv), BIND(glGetString), BIND(glPixelStorei);
}

#define W 64
#define H 48
enum { A_POS = 0, A_COLOR = 1, A_TEX = 2 }; /* SDL's GLES2_ATTRIBUTE_* */

static void *g_dpy, *g_surf;
static u32 g_vbo;

static u32 program(const char *fs_src)
{
    u32 vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
    /* SDL 2.0.20+ passes the shader in two strings */
    const char *two[2] = {"", k_vs};
    glShaderSource(vs, 2, two, NULL);
    glCompileShader(vs);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    s32 okv = 0, okf = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &okv);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &okf);
    u32 p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, A_POS, "a_position");
    glBindAttribLocation(p, A_COLOR, "a_color");
    glBindAttribLocation(p, A_TEX, "a_texCoord");
    glLinkProgram(p);
    s32 linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!okv || !okf || !linked)
        hlog("FAILED program: vs %d fs %d link %d", okv, okf, linked), g_failed = 1;
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "u_texture"), 0);
    return p;
}

/* SDL's projection: the window (y down from the top) or a target (y up) */
static void project(u32 p, u32 w, u32 h, bool target)
{
    float m[16] = {0};
    m[0] = 2.0f / (float)w, m[5] = (target ? 2.0f : -2.0f) / (float)h, m[10] = 0.0f;
    m[12] = -1.0f, m[13] = target ? -1.0f : 1.0f, m[15] = 1.0f;
    glUseProgram(p);
    glUniformMatrix4fv(glGetUniformLocation(p, "u_projection"), 1, 0, m);
}

/* a vertex as SDL 2.0.18+ lays them out: x, y, colour (4 bytes), u, v */
struct sv {
    float x, y;
    u8 c[4];
    float u, v;
};

static void quad(float x0, float y0, float x1, float y1, const u8 c[4], float u0, float v0, float u1, float v1,
                 bool tex)
{
    struct sv v[4] = {{x0, y0, {c[0], c[1], c[2], c[3]}, u0, v0},
                      {x1, y0, {c[0], c[1], c[2], c[3]}, u1, v0},
                      {x0, y1, {c[0], c[1], c[2], c[3]}, u0, v1},
                      {x1, y1, {c[0], c[1], c[2], c[3]}, u1, v1}};
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, 0x88E8 /* DYNAMIC_DRAW */);
    glVertexAttribPointer(A_POS, 2, GL_FLOAT, 0, sizeof(struct sv), (const void *)0);
    glVertexAttribPointer(A_COLOR, 4, GL_UNSIGNED_BYTE, 1, sizeof(struct sv), (const void *)8);
    glEnableVertexAttribArray(A_POS);
    glEnableVertexAttribArray(A_COLOR);
    if (tex) {
        glVertexAttribPointer(A_TEX, 2, GL_FLOAT, 0, sizeof(struct sv), (const void *)12);
        glEnableVertexAttribArray(A_TEX);
    } else
        glDisableVertexAttribArray(A_TEX);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* the window's pixel at (x, y) from the top, as the screen shows it: B, G, R, A bytes */
static u32 shown(u32 x, u32 y) { return g_shown[y * W + x]; }
static u32 bgra(u32 r, u32 g, u32 b, u32 a) { return b | g << 8 | r << 16 | a << 24; }

static void swap(void) { eglSwapBuffers(g_dpy, g_surf); }

/* within one of want in each channel (a GPU's rounding of a float colour) */
static void check_px_near(u32 x, u32 y, u32 want, const char *what)
{
    u32 got = shown(x, y), far = 0;
    for (int k = 0; k < 32; k += 8) {
        int d = (int)((got >> k) & 255) - (int)((want >> k) & 255);
        far += d > 1 || d < -1;
    }
    char line[200];
    snprintf(line, sizeof(line), "%s: (%u, %u) is %08x (want %08x, +-1)", what, x, y, got, want);
    expect(far == 0, line);
}

static void check_px(u32 x, u32 y, u32 want, const char *what)
{
    u32 got = shown(x, y);
    char line[200];
    snprintf(line, sizeof(line), "%s: (%u, %u) is %08x (want %08x)", what, x, y, got, want);
    expect(got == want, line);
}

static void scenario_draw(void)
{
    bind_all();
    expect(scePigletSetConfigurationVSH((const void *)0x1000) == 1, "scePigletSetConfigurationVSH says true");
    g_dpy = eglGetDisplay(NULL);
    s32 maj = 0, min = 0;
    expect(g_dpy && eglInitialize(g_dpy, &maj, &min) && maj == 1 && min == 4, "eglInitialize: EGL 1.4");
    const s32 want[] = {0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3040, 4, 0x3038};
    void *cfgs[8];
    s32 n = 0;
    expect(eglChooseConfig(g_dpy, want, cfgs, 8, &n) && n == 1, "eglChooseConfig: one RGBA8888 GLES2 config");
    s32 red = 0;
    expect(eglGetConfigAttrib(g_dpy, cfgs[0], 0x3024, &red) && red == 8, "eglGetConfigAttrib(RED_SIZE) = 8");
    u32 native[3] = {0, W, H};
    g_surf = eglCreateWindowSurface(g_dpy, cfgs[0], native, NULL);
    const s32 ctx_attr[] = {0x3098, 2, 0x3038};
    void *ctx = eglCreateContext(g_dpy, cfgs[0], NULL, ctx_attr);
    expect(g_surf && ctx && eglMakeCurrent(g_dpy, g_surf, g_surf, ctx), "window surface and context current");
    expect(eglGetProcAddress("glDrawArrays") == (void *)glDrawArrays, "eglGetProcAddress finds GL functions");
    s32 fb = -1, nbin = -1, vp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_NUM_SHADER_BINARY_FORMATS, &nbin);
    glGetIntegerv(GL_VIEWPORT, vp);
    expect(fb == 0 && nbin == 0 && vp[2] == W && vp[3] == H, "the window is framebuffer 0, no binary shaders, "
                                                            "the viewport its size");
    expect(glGetString(GL_VERSION) && strstr(glGetString(GL_VERSION), "OpenGL ES 2.0"), "GL_VERSION");
    glGenBuffers(1, &g_vbo);

    u32 solid = program(k_fs_solid), abgr = program(k_fs_abgr), argb = program(k_fs_argb);
    project(solid, W, H, false), project(abgr, W, H, false), project(argb, W, H, false);
    expect(glGetError() == GL_NO_ERROR, "no GL error setting up");

    /* clear */
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    swap();
    check_px(0, 0, bgra(64, 128, 191, 255), "glClear");
    check_px(W - 1, H - 1, bgra(64, 128, 191, 255), "glClear");

    /* SDL_RenderFillRect(8, 4, 16, 8) in red */
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(solid);
    glDisable(GL_BLEND);
    const u8 redc[4] = {255, 0, 0, 255};
    quad(8, 4, 24, 12, redc, 0, 0, 0, 0, false);
    swap();
    check_px(8, 4, bgra(255, 0, 0, 255), "fill rect: its top-left pixel");
    check_px(23, 11, bgra(255, 0, 0, 255), "fill rect: its bottom-right pixel");
    check_px(24, 4, bgra(0, 0, 0, 255), "fill rect: right of it");
    check_px(7, 4, bgra(0, 0, 0, 255), "fill rect: left of it");
    check_px(8, 12, bgra(0, 0, 0, 255), "fill rect: below it");
    check_px(8, 3, bgra(0, 0, 0, 255), "fill rect: above it");

    /* half-transparent white over black: every pixel 128, no seam where the
     * strip's two triangles meet */
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    const u8 half[4] = {255, 255, 255, 128};
    quad(30, 20, 47, 37, half, 0, 0, 0, 0, false);
    swap();
    u32 bad = 0;
    for (u32 y = 20; y < 37; y++)
        for (u32 x = 30; x < 47; x++)
            bad += shown(x, y) != bgra(128, 128, 128, 255);
    expect(bad == 0, "blended square: all 289 pixels 128 grey, the diagonal drawn once");
    glDisable(GL_BLEND);

    /* a 4x2 ABGR texture: bytes R, G, B, A per texel */
    u8 texels[4 * 2 * 4];
    for (u32 i = 0; i < 8; i++)
        texels[i * 4] = (u8)(i * 30), texels[i * 4 + 1] = (u8)(200 - i * 20), texels[i * 4 + 2] = (u8)(i * 7 + 1),
        texels[i * 4 + 3] = 255;
    u32 t = 0;
    glGenTextures(1, &t);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 2, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(abgr);
    const u8 white[4] = {255, 255, 255, 255};
    quad(10, 10, 14, 12, white, 0, 0, 1, 1, true); /* 1:1 */
    quad(20, 10, 28, 14, white, 0, 0, 1, 1, true); /* 2x */
    glUseProgram(argb);
    quad(40, 10, 44, 12, white, 0, 0, 1, 1, true); /* 1:1, red and blue swapped */
    swap();
    u32 miss = 0;
    for (u32 i = 0; i < 8; i++) {
        u32 x = i % 4, y = i / 4;
        const u8 *tx = &texels[i * 4];
        miss += shown(10 + x, 10 + y) != bgra(tx[0], tx[1], tx[2], 255);
        miss += shown(20 + 2 * x, 10 + 2 * y) != bgra(tx[0], tx[1], tx[2], 255);
        miss += shown(21 + 2 * x, 11 + 2 * y) != bgra(tx[0], tx[1], tx[2], 255);
        miss += shown(40 + x, 10 + y) != bgra(tx[2], tx[1], tx[0], 255);
    }
    expect(miss == 0, "textures: each texel where it belongs at 1:1 and doubled, and ARGB swizzled");
    check_px(14, 10, bgra(0, 0, 0, 255), "texture: nothing right of it");

    /* scaled by 1.5 from x 29.5: pixel 29's centre is on the quad's left
     * edge (drawn), 35's on its right edge (not), and 32's exactly where
     * texel 1 ends: GL's rule takes texel 2 there, as the GPU does (measured
     * on a PS4), not the texel a float rounding error falls into */
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(abgr);
    quad(29.5f, 20, 35.5f, 23, white, 0, 0, 1, 1, true);
    swap();
    const u8 *tx0 = &texels[0], *tx2 = &texels[8];
    check_px(29, 20, bgra(tx0[0], tx0[1], tx0[2], 255), "texel edges: the pixel on the quad's left edge is drawn");
    check_px(32, 20, bgra(tx2[0], tx2[1], tx2[2], 255), "texel edges: a centre on texel 1's end takes texel 2");
    check_px(35, 20, bgra(0, 0, 0, 255), "texel edges: the pixel on the right edge is not");

    /* colour modulation: SDL_SetTextureColorMod(128, 255, 0) */
    glUseProgram(abgr);
    glClear(GL_COLOR_BUFFER_BIT);
    const u8 mod[4] = {128, 255, 0, 255};
    quad(10, 10, 14, 12, mod, 0, 0, 1, 1, true);
    swap();
    const u8 *t1 = &texels[4];
    u32 mr = (t1[0] * 128 + 127) / 255;
    check_px(11, 10, bgra(mr, t1[1], 0, 255), "colour modulation");

    /* render to a texture (SDL_SetRenderTarget), then draw it to the window */
    u32 rt = 0, fbo = 0;
    glGenTextures(1, &rt);
    glBindTexture(GL_TEXTURE_2D, rt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    expect(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "render target complete");
    glViewport(0, 0, 8, 8);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(solid);
    project(solid, 8, 8, true);
    const u8 green[4] = {0, 255, 0, 255};
    quad(0, 0, 8, 2, green, 0, 0, 0, 0, false); /* the target's top two rows */
    u8 rb[8 * 8 * 4];
    glReadPixels(0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, rb);
    expect(rb[1] == 255 && rb[2] == 0 && rb[7 * 32 + 2] == 255,
           "glReadPixels of the target: GL row 0 is SDL's top row (green), row 7 blue");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    project(solid, W, H, false);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(abgr);
    quad(50, 30, 58, 38, white, 0, 0, 1, 1, true);
    swap();
    check_px(50, 30, bgra(0, 255, 0, 255), "the target drawn: its top row green");
    check_px(57, 31, bgra(0, 255, 0, 255), "the target drawn: its second row green");
    check_px(53, 32, bgra(0, 0, 255, 255), "the target drawn: the rest blue");

    /* scissor (GL's box is bottom-up: SDL converts) */
    glEnable(GL_SCISSOR_TEST);
    glScissor(4, H - 8, 4, 4); /* SDL rect (4, 4, 4, 4) */
    glClearColor(1, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    swap();
    check_px(4, 4, bgra(255, 255, 0, 255), "scissored clear: inside");
    check_px(7, 7, bgra(255, 255, 0, 255), "scissored clear: inside");
    check_px(8, 4, bgra(0, 0, 0, 255), "scissored clear: outside");
    check_px(4, 8, bgra(0, 0, 0, 255), "scissored clear: outside");

    /* YUV (three planes) and NV21 (Y + interleaved VU), as SDL draws video textures */
    {
        u32 ty[3];
        glGenTextures(3, ty);
        const u8 yp[4] = {200, 200, 200, 200}, up[1] = {100}, vp1[1] = {180}, vu[2] = {180, 100};
        const u8 *planes[3] = {yp, up, vp1};
        for (int i = 0; i < 3; i++) {
            glActiveTexture(GL_TEXTURE0 + (u32)i);
            glBindTexture(GL_TEXTURE_2D, ty[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, i ? 1 : 2, i ? 1 : 2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE,
                         planes[i]);
        }
        u32 yuv = program(k_fs_yuv);
        project(yuv, W, H, false);
        glUniform1i(glGetUniformLocation(yuv, "u_texture_u"), 1);
        glUniform1i(glGetUniformLocation(yuv, "u_texture_v"), 2);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        quad(2, 2, 6, 6, white, 0, 0, 1, 1, true);
        /* NV21: the second plane holds V then U (luminance-alpha: .r and .a) */
        u32 tvu;
        glGenTextures(1, &tvu);
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, tvu);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, 1, 1, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, vu);
        u32 nv = program(k_fs_nv21);
        project(nv, W, H, false);
        glUniform1i(glGetUniformLocation(nv, "u_texture_u"), 1);
        quad(10, 2, 14, 6, white, 0, 0, 1, 1, true);
        glActiveTexture(GL_TEXTURE0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        swap();
        /* JPEG: rgb = (y, u - 0.502, v - 0.502) * matrix */
        float y = 200 / 255.0f, u = 100 / 255.0f - 0.501960814f, v = 180 / 255.0f - 0.501960814f;
        float r = y + 1.402f * v, g = y - 0.3441f * u - 0.7141f * v, b = y + 1.772f * u;
        u32 want = bgra(r >= 1 ? 255 : (u32)(r * 255 + 0.5f), (u32)(g * 255 + 0.5f), (u32)(b * 255 + 0.5f), 255);
        check_px_near(3, 3, want, "YUV: three planes to RGB");
        /* NV21, BT.601: yuv.yz = (.a, .r) = (U, V) */
        y = 200 / 255.0f - 0.0627451017f, u = 100 / 255.0f - 0.501960814f, v = 180 / 255.0f - 0.501960814f;
        r = 1.1644f * y + 1.596f * v, g = 1.1644f * y - 0.3918f * u - 0.813f * v, b = 1.1644f * y + 2.0172f * u;
        want = bgra(r >= 1 ? 255 : (u32)(r * 255 + 0.5f), (u32)(g * 255 + 0.5f), (u32)(b * 255 + 0.5f), 255);
        check_px_near(11, 3, want, "NV21: Y plane and interleaved VU to RGB");
    }

    /* points and lines, at pixel centres as SDL draws them */
    glUseProgram(solid);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    struct sv pts[2] = {{2.5f, 40.5f, {255, 255, 255, 255}, 0, 0}, {3.5f, 41.5f, {255, 0, 255, 255}, 0, 0}};
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(pts), pts, 0x88E8);
    glVertexAttribPointer(A_POS, 2, GL_FLOAT, 0, sizeof(struct sv), (const void *)0);
    glVertexAttribPointer(A_COLOR, 4, GL_UNSIGNED_BYTE, 1, sizeof(struct sv), (const void *)8);
    glDisableVertexAttribArray(A_TEX);
    glDrawArrays(GL_POINTS, 0, 2);
    struct sv line[2] = {{10.5f, 44.5f, {0, 255, 255, 255}, 0, 0}, {20.5f, 44.5f, {0, 255, 255, 255}, 0, 0}};
    glBufferData(GL_ARRAY_BUFFER, sizeof(line), line, 0x88E8);
    glDrawArrays(GL_LINE_STRIP, 0, 2);
    swap();
    check_px(2, 40, bgra(255, 255, 255, 255), "point");
    check_px(3, 41, bgra(255, 0, 255, 255), "second point");
    check_px(10, 44, bgra(0, 255, 255, 255), "line: first pixel");
    check_px(20, 44, bgra(0, 255, 255, 255), "line: last pixel (a strip's end)");
    check_px(21, 44, bgra(0, 0, 0, 255), "line: nothing past it");

    /* an unknown shader: compile fails, with a log */
    u32 bad_fs = glCreateShader(GL_FRAGMENT_SHADER);
    const char *weird = "void main() { gl_FragColor = vec4(fract(gl_FragCoord.x)); }";
    glShaderSource(bad_fs, 1, &weird, NULL);
    glCompileShader(bad_fs);
    s32 ok = 1, loglen = 0;
    char log[200] = {0};
    glGetShaderiv(bad_fs, GL_COMPILE_STATUS, &ok);
    glGetShaderiv(bad_fs, GL_INFO_LOG_LENGTH, &loglen);
    glGetShaderInfoLog(bad_fs, sizeof(log), NULL, log);
    expect(!ok && loglen > 0 && strstr(log, "ghostldr GLES"), "an unknown shader does not compile, and says why");

    /* SDL-PS4 hands Piglet its shaders as binaries: a 16-byte header (magic,
     * 0x64, 1, source length) and the GLSL source */
    static u8 blob[16 + 4096];
    u32 srclen = (u32)strlen(k_fs_solid);
    u32 hdr[4] = {0xE891BC71u, 0x64, 1, srclen};
    memcpy(blob, hdr, 16);
    memcpy(blob + 16, k_fs_solid, srclen);
    u32 bin_fs = glCreateShader(GL_FRAGMENT_SHADER);
    glGetError();
    glShaderBinary(1, &bin_fs, 0, blob, (s32)(16 + srclen));
    ok = 0;
    glGetShaderiv(bin_fs, GL_COMPILE_STATUS, &ok);
    expect(ok && glGetError() == GL_NO_ERROR, "a Piglet shader binary compiles from the source it carries");
    blob[0] ^= 1;
    u32 bin_bad = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderBinary(1, &bin_bad, 0, blob, (s32)(16 + srclen));
    ok = 1;
    glGetShaderiv(bin_bad, GL_COMPILE_STATUS, &ok);
    expect(!ok && glGetError() == GL_INVALID_ENUM, "any other binary: GL_INVALID_ENUM, not compiled");
    expect(g_shown_frames == 10, "ten frames shown");
}

/* a full-screen 1920x1080 frame like Apollo's: an opaque background, then a
 * blended full-screen layer and 200 small blended icons */
static void scenario_perf(void)
{
    bind_all();
    g_dpy = eglGetDisplay(NULL);
    eglInitialize(g_dpy, NULL, NULL);
    void *cfg;
    s32 n;
    eglChooseConfig(g_dpy, NULL, &cfg, 1, &n);
    u32 native[3] = {0, 1920, 1080};
    g_surf = eglCreateWindowSurface(g_dpy, cfg, native, NULL);
    eglMakeCurrent(g_dpy, g_surf, g_surf, eglCreateContext(g_dpy, cfg, NULL, NULL));
    glGenBuffers(1, &g_vbo);
    u32 p = program(k_fs_abgr);
    project(p, 1920, 1080, false);
    glViewport(0, 0, 1920, 1080);
    u8 *img = calloc(1, 1920 * 1080 * 4);
    for (u32 i = 0; i < 1920 * 1080; i++)
        img[i * 4] = (u8)i, img[i * 4 + 1] = (u8)(i >> 8), img[i * 4 + 2] = 99, img[i * 4 + 3] = (u8)(i * 7);
    u32 t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); /* as SDL sets them */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1920, 1080, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    const u8 white[4] = {255, 255, 255, 255};
    long t0 = clock();
    const int frames = 20;
    for (int f = 0; f < frames; f++) {
        glDisable(GL_BLEND);
        quad(0, 0, 1920, 1080, white, 0, 0, 1, 1, true);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        quad(0, 0, 1920, 1080, white, 0, 0, 1, 1, true);
        for (int k = 0; k < 200; k++)
            quad((float)(k * 9 % 1800), (float)(k * 5 % 1000), (float)(k * 9 % 1800 + 64),
                 (float)(k * 5 % 1000 + 64), white, 0, 0, 0.03f, 0.06f, true);
        eglSwapBuffers(g_dpy, g_surf);
    }
    long t1 = clock();
#ifdef GLES_CONSOLE
    double ms = (double)(t1 - t0) / 1000.0 / frames; /* microseconds */
    hlog("perf: %d.%d ms per 1920x1080 frame (2 full-screen layers, 200 icons) on this console, %s", (int)ms,
         (int)(ms * 10) % 10, g_fake_gpu ? "GPU" : "software");
#else
    double ms = (double)(t1 - t0) * 1000.0 / 1000000.0 / frames;
    hlog("perf: %.1f ms per 1920x1080 frame (2 full-screen layers, 200 icons) on this machine", ms);
#endif
    expect(ms > 0, "perf ran");
    free(img);
}

int main(int argc, char **argv)
{
    setup();
#ifdef GLES_CONSOLE
    const char *sc = GLES_SCENARIO;
#else
    const char *sc = argc > 1 ? argv[1] : "";
#endif
    if (strcmp(sc, "shaders") == 0)
        scenario_shaders();
    else if (strcmp(sc, "draw") == 0)
        scenario_draw();
    else if (strcmp(sc, "draw-gpu") == 0) {
        g_fake_gpu = true;
        scenario_draw();
#ifndef GLES_CONSOLE
        hlog("fake GPU: %llu command buffers, %llu draws", (unsigned long long)g_gpu_submits,
             (unsigned long long)g_gpu_draws);
        expect(g_gpu_draws >= 15 && g_gpu_submits >= 8, "the draws went to the GPU path, a command buffer a frame");
#endif
    }
    else if (strcmp(sc, "perf") == 0)
        scenario_perf();
    else if (strcmp(sc, "perf-gpu") == 0) {
        g_fake_gpu = true;
        scenario_perf();
    } else
        return 2;
#ifdef GLES_CONSOLE
    if (g_fake_gpu)
        gles_gpu_log("at the end");
    direct_free_all();
#endif
    return g_failed;
}
