/*
 * ghostldr's GLES on the console's GPU: the draws hle_gles.c records, as Gnm
 * command buffers (PM4 packets) submitted through the title process's own
 * libSceGnmDriver, as a PS4 game's are.  No console calls here: the Gnm
 * functions come in struct gles_gnm (hle_gles.c passes SWRR's, the tests
 * their fakes), so the file builds for this machine too (tests/test_gles.py).
 *
 * What the GPU does and what stays on the CPU:
 *   - the CPU still runs the vertex stage (hle_gles.c's transform, the same
 *     code as the software path): SDL draws a few thousand vertices a frame.
 *     Each draw's vertices go into a vertex ring in GPU memory, already in
 *     clip space, with the colour each fragment is multiplied by;
 *   - the GPU rasterises, samples, blends and writes the target: one vertex
 *     shader and one pixel shader, GCN code assembled at build time
 *     (ext/gcn, tools/mkgcn.py).  The pixel shader computes
 *     texture(u, v) * colour * constant; a solid draw samples a 1x1 white
 *     texture, and a shader's red/blue swap and "alpha = 1" are the texture
 *     descriptor's channel selects;
 *   - points and lines become one small square per pixel the software
 *     rasteriser's DDA visits (gles_raster.c's rules, so the same pixels);
 *   - what this does not do (a texture without pixels, a line longer than
 *     MAX_LINE_PIXELS) gles_gpu_draw refuses, and hle_gles.c draws it with
 *     the software rasteriser into the same memory once the GPU is idle.
 *
 * Targets and textures are linear RGBA8 (the window: the VideoOut buffers,
 * B G R A) in memory both the CPU and the GPU map, rows 64 pixels apart at
 * least (the GPU's linear-aligned surfaces), so the software path reads and
 * writes them too.
 *
 * Synchronisation is simple: one command buffer is recorded at a time and
 * submitted at eglSwapBuffers (or when the CPU must touch something it
 * uses), and the CPU waits for its end-of-pipe label before going on.  Each
 * texture records the last command buffer that used it (gles_tex.gpu_seq).
 *
 * Register numbers are the GCN (gfx7, "Sea Islands") ones AMD documents and
 * open drivers use; PM4 is AMD's command processor packet format.
 */
#include "gles.h"
#include "gles_gcn.h"

/* ---- PM4 ---- */

#define PM4(op, n) (0xC0000000u | ((u32)((n) - 1) << 16) | ((u32)(op) << 8))
#define IT_NUM_INSTANCES 0x2F
#define IT_DRAW_INDEX_AUTO 0x2D
#define IT_SURFACE_SYNC 0x43
#define IT_EVENT_WRITE 0x46
#define IT_EVENT_WRITE_EOP 0x47
#define IT_SET_CONTEXT_REG 0x69
#define IT_SET_SH_REG 0x76
#define IT_SET_UCONFIG_REG 0x79

/* context registers (byte addresses) */
#define DB_RENDER_CONTROL 0x28000
#define PA_SC_SCREEN_SCISSOR_TL 0x28030
#define PA_SC_SCREEN_SCISSOR_BR 0x28034
#define DB_Z_INFO 0x28040
#define DB_STENCIL_INFO 0x28044
#define PA_SC_WINDOW_OFFSET 0x28200
#define PA_SC_WINDOW_SCISSOR_TL 0x28204
#define PA_SC_WINDOW_SCISSOR_BR 0x28208
#define PA_SC_CLIPRECT_RULE 0x2820C
#define CB_TARGET_MASK 0x28238
#define CB_SHADER_MASK 0x2823C
#define PA_SC_GENERIC_SCISSOR_TL 0x28240
#define PA_SC_GENERIC_SCISSOR_BR 0x28244
#define PA_SC_VPORT_SCISSOR_0_TL 0x28250
#define PA_SC_VPORT_SCISSOR_0_BR 0x28254
#define PA_SC_VPORT_ZMIN_0 0x282D0
#define PA_SC_VPORT_ZMAX_0 0x282D4
#define VGT_MAX_VTX_INDX 0x28400
#define VGT_MIN_VTX_INDX 0x28404
#define VGT_INDX_OFFSET 0x28408
#define PA_CL_VPORT_XSCALE 0x2843C /* then XOFFSET, YSCALE, YOFFSET, ZSCALE, ZOFFSET */
#define SPI_PS_INPUT_CNTL_0 0x28644
#define SPI_VS_OUT_CONFIG 0x286C4
#define SPI_PS_INPUT_ENA 0x286CC
#define SPI_PS_INPUT_ADDR 0x286D0
#define SPI_INTERP_CONTROL_0 0x286D4
#define SPI_PS_IN_CONTROL 0x286D8
#define SPI_BARYC_CNTL 0x286E0
#define SPI_TMPRING_SIZE 0x286E8
#define SPI_SHADER_POS_FORMAT 0x2870C
#define SPI_SHADER_Z_FORMAT 0x28710
#define SPI_SHADER_COL_FORMAT 0x28714
#define CB_BLEND0_CONTROL 0x28780
#define DB_DEPTH_CONTROL 0x28800
#define CB_COLOR_CONTROL 0x28808
#define DB_SHADER_CONTROL 0x2880C
#define PA_CL_CLIP_CNTL 0x28810
#define PA_SU_SC_MODE_CNTL 0x28814
#define PA_CL_VTE_CNTL 0x28818
#define PA_CL_VS_OUT_CNTL 0x2881C
#define PA_SU_POINT_SIZE 0x28A00
#define PA_SU_LINE_CNTL 0x28A08
#define VGT_GS_MODE 0x28A40
#define PA_SC_MODE_CNTL_0 0x28A48
#define VGT_MULTI_PRIM_IB_RESET_EN 0x28A94
#define VGT_SHADER_STAGES_EN 0x28B54
#define PA_SC_AA_CONFIG 0x28BE0
#define PA_SU_VTX_CNTL 0x28BE4
#define PA_SC_AA_MASK_X0Y0_X1Y0 0x28C38
#define PA_SC_AA_MASK_X0Y1_X1Y1 0x28C3C
#define CB_COLOR0_BASE 0x28C60 /* then PITCH, SLICE, VIEW, INFO, ATTRIB, -, CMASK, CMASK_SLICE, FMASK, FMASK_SLICE */
/* persistent (SH) registers */
#define SPI_SHADER_PGM_LO_PS 0xB020 /* then HI, RSRC1, RSRC2 */
#define SPI_SHADER_USER_DATA_PS_0 0xB030
#define SPI_SHADER_PGM_LO_VS 0xB120
#define SPI_SHADER_USER_DATA_VS_0 0xB130
/* user configuration registers */
#define VGT_PRIMITIVE_TYPE 0x30908

/* VGT primitive types */
#define DI_PT_LINELIST 2
#define DI_PT_LINESTRIP 3
#define DI_PT_TRILIST 4
#define DI_PT_TRIFAN 5
#define DI_PT_TRISTRIP 6

#define TILE_LINEAR_ALIGNED 8 /* the GB_TILE_MODE entry for linear-aligned surfaces */
/* SPI_SHADER_PGM_RSRC1's VGPRS and SGPRS fields for a shader's register counts (gles_gcn.h), with room to
 * spare: VGPRs in fours, SGPRs in eights; DX10_CLAMP, the float mode of SDL's shaders (denormals kept for
 * fp16/64) */
#define RSRC1(vgprs, sgprs) (((vgprs) + 3) / 4 | ((sgprs) + 7) / 8 << 6 | 0xC0u << 12 | 1u << 21)
#define FLUSH_ALL_CACHES (1u << 22 | 1u << 23 | 1u << 27 | 1u << 29) /* TCL1, TC (L2), K$, I$: invalidate */

/* ---- memory layout: one block for everything but textures and targets ---- */

#define CMD_BYTES 0x100000u    /* one command buffer */
#define VTX_BYTES 0x400000u    /* the vertex ring: 131072 vertices */
#define TAB_BYTES 0x200000u    /* per-draw tables: 8192 draws */
#define STATIC_BYTES 0x10000u  /* shaders, the white texture, the label */
#define BLOCK_BYTES (CMD_BYTES + VTX_BYTES + TAB_BYTES + STATIC_BYTES)
#define VTX_STRIDE 32u
#define TAB_STRIDE 256u
#define TAB_DWORDS 56u /* the YUV shader's: three T#/S# pairs, the constant, offset and matrix */
#define DRAW_DWORDS 160u       /* the most one draw records */
#define START_DWORDS 600u      /* the default state and this file's fixed state */
#define MAX_LATER 512          /* GPU memory freed once nothing recorded uses it */
#define MAX_LINE_PIXELS 16384  /* the most pixels a draw of points or lines becomes */

static struct {
    bool on, failed;
    struct gles_gnm g;
    u8 *block;
    u32 *cmd0, *cmd, *cmd_end;
    u8 *vtx0;
    u32 nvtx, vtx_cap;
    u8 *tab0;
    u32 ntab, tab_cap;
    u32 *vs, *ps, *ps_yuv; /* the shaders' code, 256-byte aligned */
    u32 *white;           /* 1x1 white texture (pitch 64) */
    volatile u32 *label;  /* end-of-pipe: the last command buffer done */
    u32 seq;              /* the command buffer being recorded (submitted ones are below) */
    u32 done;             /* the last one finished */
    bool begun, window;   /* commands recorded; one of them writes the window */
    /* what the recorded commands have set, to skip repeats */
    u64 rt;
    u32 rt_w, rt_h, rt_pitch, rt_info;
    u32 blend, sc_tl, sc_br, prim;
    const u32 *ps_cur;
    struct later {
        void *p;
        u32 seq;
    } *later; /* [MAX_LATER], in the static block */
    struct gles_vert *px; /* points and lines as squares: 6 vertices a pixel */
    u32 nlater;
    /* for the log */
    u64 submits, draws, refused, waits, t_wait;
    u32 rejected; /* command buffers refused in a row */
} P_;

static inline void emit(u32 v) { *P_.cmd++ = v; }

static void ctx_reg(u32 reg, u32 v)
{
    emit(PM4(IT_SET_CONTEXT_REG, 2));
    emit((reg - 0x28000) >> 2);
    emit(v);
}

static void ctx_regs(u32 reg, const u32 *v, u32 n)
{
    emit(PM4(IT_SET_CONTEXT_REG, n + 1));
    emit((reg - 0x28000) >> 2);
    for (u32 i = 0; i < n; i++)
        emit(v[i]);
}

static void sh_regs(u32 reg, const u32 *v, u32 n)
{
    emit(PM4(IT_SET_SH_REG, n + 1));
    emit((reg - 0xB000) >> 2);
    for (u32 i = 0; i < n; i++)
        emit(v[i]);
}

static void sh_reg(u32 reg, u32 v) { sh_regs(reg, &v, 1); }

static void ucfg_reg(u32 reg, u32 v)
{
    emit(PM4(IT_SET_UCONFIG_REG, 2));
    emit((reg - 0x30000) >> 2);
    emit(v);
}

static inline u32 fbits(float f)
{
    u32 u;
    memcpy(&u, &f, 4);
    return u;
}

/* wait for the colour buffer's writes and make them visible to texture reads */
static void flush_rt(void)
{
    emit(PM4(IT_EVENT_WRITE, 1));
    emit(0x16); /* CACHE_FLUSH_AND_INV_EVENT */
    emit(PM4(IT_SURFACE_SYNC, 4));
    emit(FLUSH_ALL_CACHES | 1u << 25 | 1u << 6); /* + CB_ACTION, CB0_DEST_BASE */
    emit(0xFFFFFFFFu);
    emit(0);
    emit(10);
}

/* the fixed state each command buffer starts with */
static void begin(void)
{
    if (P_.begun)
        return;
    P_.begun = true;
    P_.cmd = P_.cmd0;
    P_.nvtx = P_.ntab = 0;
    P_.rt = 0, P_.blend = 0xFFFFFFFFu, P_.sc_tl = P_.sc_br = 0xFFFFFFFFu, P_.prim = 0, P_.ps_cur = NULL;
    s32 n = P_.g.default_state ? P_.g.default_state(P_.cmd, 256) : 0;
    if (n > 0 && n <= 256)
        P_.cmd += n;
    emit(PM4(IT_SURFACE_SYNC, 4));
    emit(FLUSH_ALL_CACHES);
    emit(0xFFFFFFFFu);
    emit(0);
    emit(10);
    /* no depth or stencil */
    ctx_reg(DB_Z_INFO, 0);
    ctx_reg(DB_STENCIL_INFO, 0);
    ctx_reg(DB_DEPTH_CONTROL, 0);
    ctx_reg(DB_RENDER_CONTROL, 0);
    /* colour output: target 0, all channels, no ROP */
    ctx_reg(CB_TARGET_MASK, 0xF);
    ctx_reg(CB_SHADER_MASK, 0xF);
    ctx_reg(CB_COLOR_CONTROL, 1u << 4 | 0xCCu << 16);
    /* the rasteriser: no clipping (the scissor does it), GL's pixel centres, one sample */
    ctx_reg(PA_SC_WINDOW_OFFSET, 0);
    ctx_reg(PA_SC_CLIPRECT_RULE, 0xFFFF);
    ctx_reg(PA_SC_VPORT_ZMIN_0, 0);
    ctx_reg(PA_SC_VPORT_ZMAX_0, fbits(1.0f));
    ctx_reg(PA_CL_VTE_CNTL, 0x43F); /* viewport scale and offset, w already divided */
    ctx_reg(PA_CL_CLIP_CNTL, 1u << 16 | 1u << 19 | 1u << 24);
    ctx_reg(PA_SU_SC_MODE_CNTL, 0);
    ctx_reg(PA_SU_VTX_CNTL, 0x2D); /* PIX_CENTER 0.5, round to even, 1/256 subpixels */
    ctx_reg(PA_SC_AA_CONFIG, 0);
    ctx_reg(PA_SC_AA_MASK_X0Y0_X1Y0, 0xFFFFFFFFu);
    ctx_reg(PA_SC_AA_MASK_X0Y1_X1Y1, 0xFFFFFFFFu);
    ctx_reg(PA_SC_MODE_CNTL_0, 0);
    ctx_reg(PA_SU_POINT_SIZE, 8u << 16 | 8u);
    ctx_reg(PA_SU_LINE_CNTL, 8);
    /* vertex grouping: a vertex shader only */
    ctx_reg(VGT_SHADER_STAGES_EN, 0);
    ctx_reg(VGT_GS_MODE, 0);
    ctx_reg(VGT_MAX_VTX_INDX, 0xFFFFFFFFu);
    ctx_reg(VGT_MIN_VTX_INDX, 0);
    ctx_reg(VGT_INDX_OFFSET, 0);
    ctx_reg(VGT_MULTI_PRIM_IB_RESET_EN, 0);
    emit(PM4(IT_NUM_INSTANCES, 1));
    emit(1);
    /* the vertex shader: 16 VGPRs, 32 SGPRs, 5 user SGPRs (the ring's V#, the first vertex) */
    u64 vs = (u64)P_.vs, ps = (u64)P_.ps;
    u32 vsr[4] = {(u32)(vs >> 8), (u32)(vs >> 40), RSRC1(GCN_GLES_VS_VGPRS, GCN_GLES_VS_SGPRS), 5u << 1};
    sh_regs(SPI_SHADER_PGM_LO_VS, vsr, 4);
    /* SPI_SHADER_PGM_RSRC3_* (which compute units, how many waves) and LATE_ALLOC_VS stay the system's
     * default state's: it keeps a compute unit for itself */
    u64 ring = (u64)P_.vtx0;
    u32 vsharp[4] = {(u32)ring, (u32)(ring >> 32) | VTX_STRIDE << 16, 0xFFFFFFFFu, 0x27FAC};
    sh_regs(SPI_SHADER_USER_DATA_VS_0, vsharp, 4);
    ctx_reg(SPI_VS_OUT_CONFIG, 1u << 1); /* two parameters */
    ctx_reg(SPI_SHADER_POS_FORMAT, 4);
    ctx_reg(PA_CL_VS_OUT_CNTL, 0);
    /* the pixel shaders (set per draw): 2 user SGPRs (the table), perspective I/J, two interpolants, FP16 colour */
    (void)ps;
    u32 inputs[2] = {0, 1};
    ctx_regs(SPI_PS_INPUT_CNTL_0, inputs, 2);
    ctx_reg(SPI_PS_INPUT_ENA, 2);
    ctx_reg(SPI_PS_INPUT_ADDR, 2);
    ctx_reg(SPI_INTERP_CONTROL_0, 0);
    ctx_reg(SPI_PS_IN_CONTROL, 2);
    ctx_reg(SPI_BARYC_CNTL, 0);
    ctx_reg(SPI_TMPRING_SIZE, 0);
    ctx_reg(SPI_SHADER_Z_FORMAT, 0);
    ctx_reg(SPI_SHADER_COL_FORMAT, 4);
    ctx_reg(DB_SHADER_CONTROL, 0x10);
}

/* ---- set-up ---- */

bool gles_gpu_start(const struct gles_gnm *g)
{
    if (P_.on || P_.failed)
        return P_.on;
    P_.failed = true;
    if (!g || !g->submit || !g->gpu_alloc || !g->usec)
        return false;
    P_.g = *g;
    P_.block = g->gpu_alloc(BLOCK_BYTES);
    if (!P_.block) {
        logf("gles: GPU: no memory for its rings (%u KiB): software drawing", BLOCK_BYTES >> 10);
        return false;
    }
    memset(P_.block, 0, BLOCK_BYTES);
    P_.cmd0 = P_.cmd = (u32 *)P_.block;
    P_.cmd_end = P_.cmd0 + CMD_BYTES / 4;
    P_.vtx0 = P_.block + CMD_BYTES;
    P_.vtx_cap = VTX_BYTES / VTX_STRIDE;
    P_.tab0 = P_.vtx0 + VTX_BYTES;
    P_.tab_cap = TAB_BYTES / TAB_STRIDE;
    u8 *st = P_.tab0 + TAB_BYTES;
    P_.vs = (u32 *)st;
    P_.ps = (u32 *)(st + 0x400);
    P_.ps_yuv = (u32 *)(st + 0x800);
    P_.white = (u32 *)(st + 0x1000);
    P_.label = (volatile u32 *)(st + 0x2000);
    P_.later = (struct later *)(st + 0x4000); /* 8 KiB */
    memcpy(P_.vs, k_gles_vs, sizeof(k_gles_vs));
    memcpy(P_.ps, k_gles_ps, sizeof(k_gles_ps));
    memcpy(P_.ps_yuv, k_gles_ps_yuv, sizeof(k_gles_ps_yuv));
    P_.white[0] = 0xFFFFFFFFu;
    *P_.label = 0;
    P_.seq = 1, P_.done = 0;
    P_.on = true, P_.failed = false;
    logf("gles: GPU drawing: rings at %p (%u KiB), shaders %u + %u + %u bytes", P_.block, BLOCK_BYTES >> 10,
         (u32)sizeof(k_gles_vs), (u32)sizeof(k_gles_ps), (u32)sizeof(k_gles_ps_yuv));
    return true;
}

bool gles_gpu_on(void) { return P_.on; }
u32 gles_gpu_seq(void) { return P_.seq; }

/* ---- submission ---- */

static void free_done(void)
{
    u32 k = 0;
    for (u32 i = 0; i < P_.nlater; i++) {
        if (P_.later[i].seq <= P_.done)
            P_.g.gpu_free(P_.later[i].p);
        else
            P_.later[k++] = P_.later[i];
    }
    P_.nlater = k;
}

/* the GPU stopped answering: the software path takes over from here */
static void give_up(const char *why)
{
    logf("gles: GPU: %s: drawing in software from now on (%lu command buffers done)", why, P_.submits);
    P_.on = false, P_.failed = true;
}

/* submit what is recorded and wait for it */
static void submit_wait(void)
{
    if (!P_.begun || !P_.on)
        return;
    if (P_.window && P_.g.before_window)
        P_.g.before_window();
    u32 seq = P_.seq;
    u64 a = (u64)P_.label;
    emit(PM4(IT_EVENT_WRITE_EOP, 5));
    emit(0x14 | 5u << 8 | 0x38u << 12); /* CACHE_FLUSH_AND_INV_TS_EVENT, write back and invalidate L1 and L2 */
    emit((u32)a);
    emit((u32)(a >> 32) | 1u << 29); /* 32 bits of data, no interrupt */
    emit(seq);
    emit(0);
    void *dcb[1] = {P_.cmd0};
    u32 bytes[1] = {(u32)((u8 *)P_.cmd - (u8 *)P_.cmd0)};
    u64 t0 = P_.g.usec();
    s32 rc = P_.g.submit(1, dcb, bytes, NULL, NULL);
    if (P_.g.submit_done)
        P_.g.submit_done();
    P_.submits++;
    P_.begun = false, P_.window = false;
    P_.seq++;
    if (rc != 0) {
        /* what it drew is lost; the system may refuse submits for a while (its own use of the GPU) */
        if (P_.rejected++ < 3)
            logf("gles: GPU: sceGnmSubmitCommandBuffers(%u bytes) -> %x", bytes[0], (u32)rc);
        if (P_.rejected >= 120)
            give_up("120 command buffers refused in a row");
        P_.done = seq;
        if (P_.nlater)
            free_done();
        return;
    }
    P_.rejected = 0;
    u32 spins = 0;
    while (*P_.label < seq) {
        if (P_.g.usec() - t0 > 2000000) {
            give_up("no end-of-pipe label after 2 s");
            return;
        }
        if (++spins > 64 && P_.g.sleep)
            P_.g.sleep(50);
    }
    P_.done = seq;
    P_.waits++;
    P_.t_wait += P_.g.usec() - t0;
    if (P_.nlater)
        free_done();
}

void gles_gpu_wait(u32 seq)
{
    if (seq >= P_.seq)
        submit_wait(); /* still being recorded */
}

void gles_gpu_finish(void) { submit_wait(); }

void gles_gpu_free_later(void *p, u32 seq)
{
    if (!p)
        return;
    if (!P_.on || seq <= P_.done || seq == 0) {
        P_.g.gpu_free(p);
        return;
    }
    if (P_.nlater == MAX_LATER)
        submit_wait(), free_done();
    if (P_.nlater < MAX_LATER && seq > P_.done)
        P_.later[P_.nlater].p = p, P_.later[P_.nlater++].seq = seq;
    else
        P_.g.gpu_free(p);
}

/* room for one more draw of n vertices: else submit what is recorded first */
static bool room(u32 n)
{
    if (n > P_.vtx_cap)
        return false;
    if (P_.begun && (P_.cmd + DRAW_DWORDS + 16 > P_.cmd_end || P_.nvtx + n > P_.vtx_cap || P_.ntab + 1 > P_.tab_cap))
        submit_wait();
    if (!P_.on)
        return false;
    begin();
    return true;
}

/* ---- per draw state ---- */

static void set_target(const struct gles_target *t)
{
    u64 a = (u64)t->pix;
    u32 info = 0xAu << 2 | (t->bgra ? 1u : 0u) << 11 | 1u << 15; /* 8_8_8_8 UNORM, B G R A or R G B A, blend clamp */
    if (P_.rt == a && P_.rt_w == t->w && P_.rt_h == t->h && P_.rt_pitch == t->pitch && P_.rt_info == info)
        return;
    if (P_.rt)
        flush_rt(); /* what was drawn so far may be sampled from now on */
    P_.rt = a, P_.rt_w = t->w, P_.rt_h = t->h, P_.rt_pitch = t->pitch, P_.rt_info = info;
    u32 slice = t->pitch * t->h / 64 - 1;
    u32 cb[11] = {(u32)(a >> 8), (t->pitch / 8 - 1) | (t->pitch / 8 - 1) << 20, slice, 0, info,
                  TILE_LINEAR_ALIGNED | TILE_LINEAR_ALIGNED << 5, 0, (u32)(a >> 8), 0, (u32)(a >> 8), slice};
    ctx_regs(CB_COLOR0_BASE, cb, 11);
    u32 br = t->w | t->h << 16;
    ctx_reg(PA_SC_SCREEN_SCISSOR_TL, 0);
    ctx_reg(PA_SC_SCREEN_SCISSOR_BR, br);
    ctx_reg(PA_SC_WINDOW_SCISSOR_TL, 0x80000000u);
    ctx_reg(PA_SC_WINDOW_SCISSOR_BR, br);
    ctx_reg(PA_SC_VPORT_SCISSOR_0_TL, 0x80000000u);
    ctx_reg(PA_SC_VPORT_SCISSOR_0_BR, br);
    /* clip space to memory rows: x (-1..1) -> 0..w, y (1..-1) -> 0..h, top row first */
    u32 vp[6] = {fbits((float)t->w * 0.5f), fbits((float)t->w * 0.5f), fbits(-(float)t->h * 0.5f),
                 fbits((float)t->h * 0.5f), fbits(1.0f), fbits(0.0f)};
    ctx_regs(PA_CL_VPORT_XSCALE, vp, 6);
}

static u32 blend_factor(u32 f, bool *ok)
{
    switch (f) {
    case GL_ZERO: return 0;
    case GL_ONE: return 1;
    case GL_SRC_COLOR: return 2;
    case GL_ONE_MINUS_SRC_COLOR: return 3;
    case GL_SRC_ALPHA: return 4;
    case GL_ONE_MINUS_SRC_ALPHA: return 5;
    case GL_DST_ALPHA: return 6;
    case GL_ONE_MINUS_DST_ALPHA: return 7;
    case GL_DST_COLOR: return 8;
    case GL_ONE_MINUS_DST_COLOR: return 9;
    case GL_SRC_ALPHA_SATURATE: return 10;
    default: *ok = false; return 1;
    }
}

static u32 blend_fn(u32 e, bool *ok)
{
    switch (e) {
    case GL_FUNC_ADD: return 0;
    case GL_FUNC_SUBTRACT: return 1;
    case GL_MIN: return 2;
    case GL_MAX: return 3;
    case GL_FUNC_REVERSE_SUBTRACT: return 4;
    default: *ok = false; return 0;
    }
}

/* CB_BLEND0_CONTROL for the draw's blending; false: one GL can do and this cannot */
static bool blend_reg(const struct gles_draw *d, u32 *out)
{
    if (!d->blend) {
        *out = 1u | 1u << 16; /* ONE, ZERO: off */
        return true;
    }
    bool ok = true;
    u32 frgb = blend_fn(d->eq_rgb, &ok), fa = blend_fn(d->eq_a, &ok);
    u32 srgb = blend_factor(d->src_rgb, &ok), drgb = blend_factor(d->dst_rgb, &ok);
    u32 sa = blend_factor(d->src_a, &ok), da = blend_factor(d->dst_a, &ok);
    if (frgb == 2 || frgb == 3)
        srgb = drgb = 1; /* MIN and MAX take no factors */
    if (fa == 2 || fa == 3)
        sa = da = 1;
    *out = srgb | frgb << 5 | drgb << 8 | sa << 16 | fa << 21 | da << 24 | 1u << 29 | 1u << 30;
    return ok;
}

static void set_blend(u32 v)
{
    if (P_.blend != v)
        ctx_reg(CB_BLEND0_CONTROL, v), P_.blend = v;
}

/* the clip rectangle (GL window coordinates, bottom-up) as the scissor, in memory rows */
static void set_scissor(const struct gles_target *t, const struct gles_rect *r)
{
    s32 y0 = t->flip_y ? (s32)t->h - r->y1 : r->y0, y1 = t->flip_y ? (s32)t->h - r->y0 : r->y1;
    u32 tl = (u32)r->x0 | (u32)y0 << 16 | 0x80000000u, br = (u32)r->x1 | (u32)y1 << 16;
    if (P_.sc_tl != tl || P_.sc_br != br) {
        ctx_reg(PA_SC_GENERIC_SCISSOR_TL, tl);
        ctx_reg(PA_SC_GENERIC_SCISSOR_BR, br);
        P_.sc_tl = tl, P_.sc_br = br;
    }
}

/* a texture's descriptor (T#) and sampler (S#) into tab[0..11]; x_sel: the
 * channel read as x (YUV planes), else the fragment shader's swizzle */
static void tex_desc(u32 *tab, const struct gles_tex *t, const struct gles_fs *fs, bool linear, char x_sel)
{
    u64 a = (u64)t->pix;
    u32 x = 4, y = 5, z = 6, w = 7; /* SQ_SEL_X..W */
    if (fs && (fs->flags & FSF_SWAP_RB))
        x = 6, z = 4;
    if (fs && (fs->flags & FSF_ALPHA_ONE))
        w = 1; /* SQ_SEL_1 */
    if (x_sel)
        x = x_sel == 'r' ? 4 : x_sel == 'g' ? 5 : x_sel == 'b' ? 6 : 7;
    tab[0] = (u32)(a >> 8);
    tab[1] = (u32)(a >> 40) | 10u << 20; /* IMG_DATA_FORMAT_8_8_8_8, UNORM */
    tab[2] = (t->w - 1) | (t->h - 1) << 14;
    tab[3] = x | y << 3 | z << 6 | w << 9 | TILE_LINEAR_ALIGNED << 20 | 9u << 28; /* 2D */
    tab[4] = (t->pitch - 1) << 13;
    tab[5] = tab[6] = tab[7] = 0;
    u32 ws = t->wrap_s == GL_REPEAT ? 0 : t->wrap_s == GL_MIRRORED_REPEAT ? 1 : 2;
    u32 wt = t->wrap_t == GL_REPEAT ? 0 : t->wrap_t == GL_MIRRORED_REPEAT ? 1 : 2;
    u32 f = linear ? 1 : 0;
    tab[8] = ws | wt << 3 | 2u << 6;
    tab[9] = 0;
    tab[10] = f << 20 | f << 22;
    tab[11] = 0;
}

static bool tex_linear(const struct gles_tex *t, bool magnify)
{
    u32 f = magnify ? t->mag_filter : t->min_filter;
    return f == GL_LINEAR || f == 0x2701 || f == 0x2703;
}

/* GL_NEAREST or GL_LINEAR for the draw, as the software rasteriser picks it
 * (per triangle there; here from the first triangle) */
static bool draw_linear(const struct gles_tex *t, const struct gles_vert *v, u32 n)
{
    if (n < 3)
        return tex_linear(t, true);
    const struct gles_vert *a = &v[0], *b = &v[1], *c = &v[2];
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (!(area > 1e-12f || area < -1e-12f))
        return tex_linear(t, true);
    float dudx = ((b->u - a->u) * (c->y - a->y) - (c->u - a->u) * (b->y - a->y)) / area;
    float dvdy = ((c->v - a->v) * (b->x - a->x) - (b->v - a->v) * (c->x - a->x)) / area;
    float sx = dudx * (float)t->w, sy = dvdy * (float)t->h;
    return tex_linear(t, sx * sx + sy * sy <= 2.0f);
}

static void mark(const struct gles_tex *t)
{
    if (t)
        ((struct gles_tex *)t)->gpu_seq = P_.seq;
}

static void set_ps(const u32 *ps)
{
    if (P_.ps_cur == ps)
        return;
    P_.ps_cur = ps;
    u64 a = (u64)ps;
    u32 rsrc1 = ps == P_.ps_yuv ? RSRC1(GCN_GLES_PS_YUV_VGPRS, GCN_GLES_PS_YUV_SGPRS)
                                : RSRC1(GCN_GLES_PS_VGPRS, GCN_GLES_PS_SGPRS);
    u32 r[4] = {(u32)(a >> 8), (u32)(a >> 40), rsrc1, 2u << 1};
    sh_regs(SPI_SHADER_PGM_LO_PS, r, 4);
}

/* one draw: n vertices of prim type, with pixel shader ps and its table */
static void record(const struct gles_target *t, const struct gles_rect *clip, u32 blend, u32 prim, u32 first, u32 n,
                   const u32 *ps, const u32 *tab)
{
    set_target(t);
    set_blend(blend);
    set_scissor(t, clip);
    set_ps(ps);
    if (P_.prim != prim)
        ucfg_reg(VGT_PRIMITIVE_TYPE, prim), P_.prim = prim;
    u8 *slot = P_.tab0 + (u64)P_.ntab++ * TAB_STRIDE;
    memcpy(slot, tab, TAB_DWORDS * 4);
    u64 sa = (u64)slot;
    sh_reg(SPI_SHADER_USER_DATA_VS_0 + 16, first);
    u32 ud[2] = {(u32)sa, (u32)(sa >> 32)};
    sh_regs(SPI_SHADER_USER_DATA_PS_0, ud, 2);
    emit(PM4(IT_DRAW_INDEX_AUTO, 2));
    emit(n);
    emit(2); /* DI_SRC_SEL_AUTO_INDEX */
    if (t->gpu_seq)
        *t->gpu_seq = P_.seq;
    else
        P_.window = true;
    P_.draws++;
}

/* vertices into the ring: clip space for target t, the colour each fragment is multiplied by */
static u32 put_verts(const struct gles_target *t, const struct gles_vert *v, u32 n, bool vcolor)
{
    u32 first = P_.nvtx;
    float *o = (float *)(P_.vtx0 + (u64)first * VTX_STRIDE);
    float sx = 2.0f / (float)t->w, sy = 2.0f / (float)t->h;
    for (u32 i = 0; i < n; i++, o += 8) {
        float ym = t->flip_y ? (float)t->h - v[i].y : v[i].y; /* memory rows, top first */
        o[0] = v[i].x * sx - 1.0f;
        o[1] = 1.0f - ym * sy;
        o[2] = v[i].u, o[3] = v[i].v;
        if (vcolor)
            memcpy(o + 4, v[i].c, 16);
        else
            o[4] = o[5] = o[6] = o[7] = 1.0f;
    }
    P_.nvtx += n;
    return first;
}

static bool usable(const struct gles_tex *t)
{
    return t && t->pix && t->w && t->h && !(t->pitch & 63) && !((u64)t->pix & 255);
}

static void white_desc(u32 *tab)
{
    struct gles_tex white = {.pix = P_.white, .w = 1, .h = 1, .pitch = 64, .wrap_s = GL_CLAMP_TO_EDGE,
                             .wrap_t = GL_CLAMP_TO_EDGE};
    tex_desc(tab, &white, NULL, false, 0);
}

/* ---- points and lines: the pixels gles_raster.c's gles_point/gles_line visit, each a square ---- */

static inline s32 px_floor(float f)
{
    s32 i = (s32)f;
    return (f < (float)i) ? i - 1 : i;
}

static inline s32 px_ceil(float f)
{
    s32 i = (s32)f;
    return (f > (float)i) ? i + 1 : i;
}

struct px_out {
    struct gles_vert *v;
    u32 n, cap; /* vertices */
};

/* pixel (x, y) of GL window coordinates, with one colour and texture coordinate */
static bool px_put(struct px_out *o, s32 x, s32 y, float u, float v, const float c[4])
{
    if (o->n + 6 > o->cap)
        return false;
    static const s8 dx[6] = {0, 1, 0, 1, 1, 0}, dy[6] = {0, 0, 1, 0, 1, 1};
    for (int i = 0; i < 6; i++) {
        struct gles_vert *q = &o->v[o->n++];
        q->x = (float)(x + dx[i]), q->y = (float)(y + dy[i]), q->u = u, q->v = v;
        memcpy(q->c, c, sizeof(q->c));
    }
    return true;
}

static bool px_point(struct px_out *o, const struct gles_vert *a)
{
    return px_put(o, px_floor(a->x), px_floor(a->y), a->u, a->v, a->c);
}

static bool px_line(struct px_out *o, const struct gles_vert *a, const struct gles_vert *b, bool last)
{
    float dx = b->x - a->x, dy = b->y - a->y;
    float adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    s32 n = px_ceil(adx > ady ? adx : ady);
    if (n <= 0)
        return last ? px_point(o, a) : true;
    if (n > 1 << 16)
        n = 1 << 16;
    float c[4];
    for (s32 i = 0; i < n + (last ? 1 : 0); i++) {
        float t = (float)i / (float)n;
        for (int k = 0; k < 4; k++)
            c[k] = a->c[k] + (b->c[k] - a->c[k]) * t;
        if (!px_put(o, px_floor(a->x + dx * t), px_floor(a->y + dy * t), a->u + (b->u - a->u) * t,
                    a->v + (b->v - a->v) * t, c))
            return false;
    }
    return true;
}

/* points or lines as a list of triangles; false: too many pixels */
static bool px_expand(u32 mode, const struct gles_vert *v, u32 n, struct px_out *o)
{
    bool ok = true;
    switch (mode) {
    case GL_POINTS:
        for (u32 k = 0; ok && k < n; k++)
            ok = px_point(o, &v[k]);
        break;
    case GL_LINES:
        for (u32 k = 0; ok && k + 1 < n; k += 2)
            ok = px_line(o, &v[k], &v[k + 1], false);
        break;
    case GL_LINE_STRIP:
        for (u32 k = 0; ok && k + 1 < n; k++)
            ok = px_line(o, &v[k], &v[k + 1], k + 2 == n);
        break;
    case GL_LINE_LOOP:
        for (u32 k = 0; ok && k + 1 < n; k++)
            ok = px_line(o, &v[k], &v[k + 1], false);
        if (ok && n > 1)
            ok = px_line(o, &v[n - 1], &v[0], false);
        break;
    default: ok = false;
    }
    return ok;
}

bool gles_gpu_draw(const struct gles_draw *d, u32 mode, const struct gles_vert *v, u32 n)
{
    if (!P_.on)
        return false;
    const struct gles_fs *fs = d->fs;
    u32 prim;
    switch (mode) {
    case GL_TRIANGLES: prim = DI_PT_TRILIST, n -= n % 3; break;
    case GL_TRIANGLE_STRIP: prim = DI_PT_TRISTRIP; break;
    case GL_TRIANGLE_FAN: prim = DI_PT_TRIFAN; break;
    case GL_POINTS:
    case GL_LINES:
    case GL_LINE_STRIP:
    case GL_LINE_LOOP: {
        if (!P_.px)
            P_.px = P_.g.gpu_alloc(sizeof(struct gles_vert) * 6 * MAX_LINE_PIXELS);
        struct px_out o = {P_.px, 0, P_.px ? 6 * MAX_LINE_PIXELS : 0};
        if (!o.v || !px_expand(mode, v, n, &o)) {
            P_.refused++;
            return false;
        }
        if (!o.n)
            return true;
        v = o.v, n = o.n, prim = DI_PT_TRILIST;
        break;
    }
    default: P_.refused++; return false;
    }
    bool yuv = fs->kind == FS_YUV || fs->kind == FS_NV;
    if ((fs->kind == FS_TEX && !usable(d->tex[0])) ||
        (yuv && (!usable(d->tex[0]) || !usable(d->tex[1]) || (fs->kind == FS_YUV && !usable(d->tex[2]))))) {
        P_.refused++; /* no pixels: the software path's black */
        return false;
    }
    u32 blend;
    if ((d->dst->pitch & 63) || !blend_reg(d, &blend) || ((u64)d->dst->pix & 255)) {
        P_.refused++;
        return false;
    }
    if (n < 3 || d->clip.x0 >= d->clip.x1 || d->clip.y0 >= d->clip.y1)
        return true; /* nothing to draw */
    if (!room(n))
        return false;
    /* the colour: vertex colour (when the shader reads it) times the constant (when it reads u_color) */
    bool vcolor = (fs->flags & FSF_MUL_VCOLOR) || (fs->kind == FS_SOLID && !(fs->flags & FSF_MUL_UCOLOR));
    u32 tab[TAB_DWORDS];
    memset(tab, 0, sizeof(tab));
    float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float *k = (fs->flags & FSF_MUL_UCOLOR) ? d->ucolor : one;
    const u32 *ps = P_.ps;
    if (yuv) {
        /* Y, U and V each read into x; filtering as the Y plane's (as the software path's) */
        bool lin = draw_linear(d->tex[0], v, n);
        const struct gles_tex *uv = d->tex[1];
        tex_desc(tab, d->tex[0], NULL, lin, 'r');
        tex_desc(tab + 12, uv, NULL, lin, fs->kind == FS_NV ? fs->uv_swz[0] : 'r');
        tex_desc(tab + 24, fs->kind == FS_NV ? uv : d->tex[2], NULL, lin, fs->kind == FS_NV ? fs->uv_swz[1] : 'r');
        for (int i = 0; i < 4; i++)
            tab[36 + i] = fbits(k[i]);
        for (int i = 0; i < 3; i++)
            tab[40 + i] = fbits(fs->offset[i]);
        for (int i = 0; i < 9; i++)
            tab[43 + i] = fbits(fs->mat[i]);
        ps = P_.ps_yuv;
        for (int i = 0; i < 3; i++)
            mark(d->tex[i]);
    } else {
        if (fs->kind == FS_TEX)
            tex_desc(tab, d->tex[0], fs, draw_linear(d->tex[0], v, n), 0), mark(d->tex[0]);
        else
            white_desc(tab);
        for (int i = 0; i < 4; i++)
            tab[12 + i] = fbits(k[i]);
    }
    u32 first = put_verts(d->dst, v, n, vcolor);
    record(d->dst, &d->clip, blend, prim, first, n, ps, tab);
    return true;
}

bool gles_gpu_clear(const struct gles_target *t, const struct gles_rect *r, const float rgba[4])
{
    if (!P_.on || (t->pitch & 63) || ((u64)t->pix & 255))
        return false;
    if (r->x0 >= r->x1 || r->y0 >= r->y1)
        return true;
    if (!room(4))
        return false;
    /* a strip over the rectangle (GL window coordinates), the colour as vertex colour */
    struct gles_vert q[4];
    float xs[4] = {(float)r->x0, (float)r->x1, (float)r->x0, (float)r->x1};
    float ys[4] = {(float)r->y0, (float)r->y0, (float)r->y1, (float)r->y1};
    for (int i = 0; i < 4; i++) {
        q[i].x = xs[i], q[i].y = ys[i], q[i].u = q[i].v = 0;
        for (int k = 0; k < 4; k++)
            q[i].c[k] = rgba[k] < 0 ? 0 : rgba[k] > 1 ? 1 : rgba[k];
    }
    u32 tab[TAB_DWORDS];
    memset(tab, 0, sizeof(tab));
    white_desc(tab);
    for (int i = 0; i < 4; i++)
        tab[12 + i] = fbits(1.0f);
    u32 first = put_verts(t, q, 4, true);
    record(t, r, 1u | 1u << 16, DI_PT_TRISTRIP, first, 4, P_.ps, tab);
    return true;
}

void gles_gpu_log(const char *when)
{
    logf("gles: GPU %s: %lu command buffers, %lu draws on the GPU, %lu drawn in software, %lu us per buffer "
         "from submit to its end",
         when, P_.submits, P_.draws, P_.refused, P_.waits ? P_.t_wait / P_.waits : 0);
}
