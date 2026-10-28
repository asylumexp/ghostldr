/*
 * hbrel: a test homebrew for three things homebrew needs from ghostldr, run
 * one after the other, with "hbrel: PASS ..." / "hbrel: FAIL ..." lines on
 * stdout (ghostldr's log, "ghost1:" lines):
 *
 *   1. save data memory (sceSaveDataSetupSaveDataMemory2 and family), 8 MiB as
 *      PS4-Xplorer asks for: what one boot writes, the next one reads back
 *      (the boot count and three patterns spread over the 8 MiB), so boot it
 *      twice, closing SWRR in between;
 *   2. the title's own prx: libhbmod.prx is imported, so it loads at boot;
 *      libhbdyn.prx is loaded at run time (sceKernelLoadStartModule +
 *      sceKernelDlsym), imports from libhbmod and libkernel and has a
 *      thread-local variable;
 *   3. Piglet (EGL + GLES2, ghostldr's own on the GPU): a 1920x1080 scene of
 *      points, lines, three-plane YUV, NV12 and NV21 video textures, a render
 *      target drawn back into the window, textures scaled with nearest and
 *      linear filtering, and blending.  The same scene every frame, so a
 *      build of the extension with EXT_CFLAGS=-DGLES_CHECK=1 compares frames
 *      120, 600 and 1800 with the software path; the first frame is also
 *      read back (glReadPixels) and checked here.
 *
 * Built with the OpenOrbis PS4 toolchain (Makefile here).  MIT license, like
 * the rest of ghostldr.  The PS4 functions are declared below.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;

int sceKernelUsleep(unsigned int usec);
uint64_t sceKernelGetProcessTime(void);
int sceKernelLoadStartModule(const char *path, size_t argc, const void *argp, u32 flags, const void *opt, int *res);
int sceKernelStopUnloadModule(int handle, size_t argc, const void *argp, u32 flags, const void *opt, int *res);
int sceKernelDlsym(int handle, const char *name, void **addr);
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int *user);
int sceSystemServiceHideSplashScreen(void);

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    printf("hbrel: %s %s\n", ok ? "PASS" : "FAIL", what);
    if (ok)
        g_pass++;
    else
        g_fail++;
}

/* ================= 1. save data memory ================= */

struct sd_param { /* SceSaveDataParam */
    char title[128], subtitle[128], detail[1024];
    u32 user_param, pad;
    int64_t mtime;
    u8 reserved[32];
};
struct sd_icon { /* SceSaveDataIcon */
    void *buf;
    u64 buf_size, data_size;
    u8 reserved[32];
};
struct sd_mem_setup2 { /* SceSaveDataMemorySetup2 */
    u32 option;
    s32 user;
    u64 memory_size, icon_memory_size;
    const struct sd_param *init_param;
    const struct sd_icon *init_icon;
    u32 slot;
    u8 reserved[20];
};
struct sd_mem_setup_result { /* SceSaveDataMemorySetupResult */
    u64 existed;
    u8 reserved[16];
};
struct sd_mem_data { /* SceSaveDataMemoryData */
    void *buf;
    u64 buf_size;
    int64_t offset;
    u8 reserved[40];
};
struct sd_mem_get2 { /* SceSaveDataMemoryGet2 */
    s32 user;
    u32 pad;
    struct sd_mem_data *data;
    struct sd_param *param;
    struct sd_icon *icon;
    u32 slot;
    u8 reserved[28];
};
struct sd_mem_set2 { /* SceSaveDataMemorySet2 */
    s32 user;
    u32 pad;
    const struct sd_mem_data *data;
    const struct sd_param *param;
    const struct sd_icon *icon;
    u32 data_num, slot;
    u8 reserved[32];
};
struct sd_mem_sync { /* SceSaveDataMemorySync */
    s32 user;
    u32 slot, option;
    u8 reserved[28];
};

int sceSaveDataInitialize3(const void *params);
int sceSaveDataTerminate(void);
int sceSaveDataSetupSaveDataMemory2(const struct sd_mem_setup2 *setup, struct sd_mem_setup_result *result);
int sceSaveDataGetSaveDataMemory2(struct sd_mem_get2 *get);
int sceSaveDataSetSaveDataMemory2(const struct sd_mem_set2 *set);
int sceSaveDataSyncSaveDataMemory(const struct sd_mem_sync *sync);
int sceSaveDataGetSaveDataMemory(s32 user, void *buf, size_t size, int64_t offset);
int sceSaveDataSetSaveDataMemory(s32 user, const void *buf, size_t size, int64_t offset);

struct sd_mount2 { /* SceSaveDataMount2 */
    s32 user;
    u32 pad;
    const char *dir;
    u64 blocks;
    u32 mode;
    u8 reserved[32];
};
struct sd_mount_result { /* SceSaveDataMountResult */
    char mount_point[16];
    u64 required_blocks;
    u32 unused, mount_status;
    u8 reserved[28];
};
struct sd_mount_info { /* SceSaveDataMountInfo */
    u64 blocks, free_blocks;
    u8 reserved[32];
};
struct sd_delete { /* SceSaveDataDelete */
    s32 user;
    u32 pad;
    const char *title_id, *dir;
    u32 unused;
    u8 reserved[32];
};
int sceSaveDataMount2(const struct sd_mount2 *mount, struct sd_mount_result *result);
int sceSaveDataUmount(const char *mount_point);
int sceSaveDataGetMountInfo(const char *mount_point, struct sd_mount_info *info);
int sceSaveDataDelete(const struct sd_delete *del);

/* A save of `blocks` 32 KiB blocks, filled with one file until a write
 * fails: what a title may keep in a save of that size (it should be about
 * the blocks it asked for), then unmounted (the commit) and deleted */
static void test_save_capacity(s32 user, u64 blocks)
{
    char dir[32] = {0}, line[240], path[64];
    snprintf(dir, sizeof(dir), "HBCAP%04lu", (unsigned long)blocks);
    struct sd_mount2 m;
    struct sd_mount_result r;
    memset(&m, 0, sizeof(m));
    memset(&r, 0, sizeof(r));
    m.user = user, m.dir = dir, m.blocks = blocks, m.mode = 6; /* CREATE | RDWR */
    int rc = sceSaveDataMount2(&m, &r);
    if (rc != 0) {
        snprintf(line, sizeof(line), "save: capacity: mount %s (%lu blocks) -> %x", dir, (unsigned long)blocks, rc);
        check(0, line);
        return;
    }
    static u8 chunk[32768];
    memset(chunk, 0x5A, sizeof(chunk));
    snprintf(path, sizeof(path), "%s/fill.bin", r.mount_point);
    FILE *f = fopen(path, "wb");
    u64 wrote = 0;
    while (f && fwrite(chunk, 1, sizeof(chunk), f) == sizeof(chunk) && fflush(f) == 0)
        wrote += sizeof(chunk);
    if (f)
        fclose(f);
    struct sd_mount_info info;
    memset(&info, 0, sizeof(info));
    int ri = sceSaveDataGetMountInfo(r.mount_point, &info);
    int ru = sceSaveDataUmount(r.mount_point);
    struct sd_delete del;
    memset(&del, 0, sizeof(del));
    del.user = user, del.dir = dir;
    int rd = sceSaveDataDelete(&del);
    snprintf(line, sizeof(line),
             "save: capacity: a %lu-block save took %lu blocks of file (%s), mount info %x (%lu blocks, %lu free), "
             "unmount %x, delete %x",
             (unsigned long)blocks, (unsigned long)(wrote / 32768), f ? "until a write failed" : "no file",
             ri, (unsigned long)info.blocks, (unsigned long)info.free_blocks, ru, rd);
    check(f && wrote / 32768 + 2 >= blocks && ru == 0 && rd == 0, line);
}

#define MEM_SIZE (8u << 20)
#define BLK 4096u
static const u64 k_blk_off[3] = {BLK, MEM_SIZE / 2, MEM_SIZE - BLK};

struct mem_hdr {
    char magic[8]; /* "HBRELMEM" */
    u32 boots, seed;
    u64 written_at;
};

static void pattern(u8 *b, u32 seed, u64 off)
{
    u32 x = seed ^ (u32)(off / BLK) * 2654435761u;
    for (u32 i = 0; i < BLK; i += 4) {
        x = x * 1664525u + 1013904223u;
        memcpy(b + i, &x, 4);
    }
}

static int mem_read(s32 user, void *buf, u64 size, u64 off)
{
    struct sd_mem_data d;
    struct sd_mem_get2 g;
    memset(&d, 0, sizeof(d));
    memset(&g, 0, sizeof(g));
    d.buf = buf, d.buf_size = size, d.offset = (int64_t)off;
    g.user = user, g.data = &d;
    return sceSaveDataGetSaveDataMemory2(&g);
}

static int mem_write(s32 user, const void *buf, u64 size, u64 off, const struct sd_param *param)
{
    struct sd_mem_data d;
    struct sd_mem_set2 s;
    memset(&d, 0, sizeof(d));
    memset(&s, 0, sizeof(s));
    d.buf = (void *)buf, d.buf_size = size, d.offset = (int64_t)off;
    s.user = user, s.data = &d, s.param = param, s.data_num = 1;
    return sceSaveDataSetSaveDataMemory2(&s);
}

static void test_save_memory(s32 user)
{
    char line[200];
    static u8 blk[BLK], want[BLK];
    struct sd_param prm;
    memset(&prm, 0, sizeof(prm));
    strcpy(prm.title, "hbrel");
    strcpy(prm.subtitle, "save data memory");
    strcpy(prm.detail, "ghostldr's test homebrew");

    int rc = sceSaveDataInitialize3(NULL);
    snprintf(line, sizeof(line), "save: sceSaveDataInitialize3 -> %x", rc);
    check(rc == 0, line);
    test_save_capacity(user, 96);
    test_save_capacity(user, 1000);

    struct sd_mem_setup2 su;
    struct sd_mem_setup_result res;
    memset(&su, 0, sizeof(su));
    memset(&res, 0, sizeof(res));
    su.user = user, su.memory_size = MEM_SIZE, su.init_param = &prm;
    u64 t0 = sceKernelGetProcessTime();
    rc = sceSaveDataSetupSaveDataMemory2(&su, &res);
    snprintf(line, sizeof(line), "save: SetupSaveDataMemory2(8 MiB) -> %x, existed %lu bytes, %lu ms", rc,
             (unsigned long)res.existed, (unsigned long)((sceKernelGetProcessTime() - t0) / 1000));
    check(rc == 0, line);
    if (rc != 0) {
        sceSaveDataTerminate();
        return;
    }

    /* what the last boot left */
    struct mem_hdr h;
    memset(&h, 0, sizeof(h));
    rc = mem_read(user, &h, sizeof(h), 0);
    u32 boots = 0;
    if (rc == 0 && memcmp(h.magic, "HBRELMEM", 8) == 0) {
        boots = h.boots;
        u32 bad = 0;
        for (int i = 0; i < 3; i++) {
            pattern(want, h.seed, k_blk_off[i]);
            bad += mem_read(user, blk, BLK, k_blk_off[i]) != 0 || memcmp(blk, want, BLK) != 0;
        }
        snprintf(line, sizeof(line),
                 "save: boot %u reads what boot %u wrote (header and 3 patterns up to the last 4 KiB), %u bad",
                 boots + 1, boots, bad);
        check(bad == 0 && res.existed == MEM_SIZE, line);
    } else {
        snprintf(line, sizeof(line), "save: first boot (read -> %x, existed %lu): nothing written before", rc,
                 (unsigned long)res.existed);
        check(rc == 0 && res.existed == 0, line);
    }

    /* this boot's: patterns, then the header */
    memcpy(h.magic, "HBRELMEM", 8);
    h.boots = boots + 1;
    h.seed = (u32)sceKernelGetProcessTime() ^ 0x5eed1234u;
    h.written_at = sceKernelGetProcessTime();
    int bad = 0;
    t0 = sceKernelGetProcessTime();
    for (int i = 0; i < 3; i++) {
        pattern(blk, h.seed, k_blk_off[i]);
        bad += mem_write(user, blk, BLK, k_blk_off[i], NULL) != 0;
    }
    rc = mem_write(user, &h, sizeof(h), 0, &prm);
    snprintf(line, sizeof(line), "save: SetSaveDataMemory2 x4 (boot %u) -> %x, %d failed, %lu ms", h.boots, rc, bad,
             (unsigned long)((sceKernelGetProcessTime() - t0) / 1000));
    check(rc == 0 && bad == 0, line);
    struct sd_mem_sync sy;
    memset(&sy, 0, sizeof(sy));
    sy.user = user;
    rc = sceSaveDataSyncSaveDataMemory(&sy);
    snprintf(line, sizeof(line), "save: SyncSaveDataMemory -> %x", rc);
    check(rc == 0, line);

    /* a whole 1 MiB write and read back */
    u8 *big = malloc(1u << 20);
    if (big) {
        for (u32 i = 0; i < (1u << 20); i++)
            big[i] = (u8)(i * 7 + h.seed);
        t0 = sceKernelGetProcessTime();
        rc = mem_write(user, big, 1u << 20, 2u << 20, NULL);
        u64 tw = sceKernelGetProcessTime() - t0;
        memset(big, 0, 1u << 20);
        int rc2 = mem_read(user, big, 1u << 20, 2u << 20);
        u32 diff = 0;
        for (u32 i = 0; i < (1u << 20); i++)
            diff += big[i] != (u8)(i * 7 + h.seed);
        snprintf(line, sizeof(line), "save: 1 MiB written (%x, %lu ms) and read back (%x): %u bytes differ", rc,
                 (unsigned long)(tw / 1000), rc2, diff);
        check(rc == 0 && rc2 == 0 && diff == 0, line);
        free(big);
    }

    /* out of range: PARAMETER */
    rc = mem_write(user, blk, BLK, MEM_SIZE - 16, NULL);
    snprintf(line, sizeof(line), "save: a write past the end -> %x (PARAMETER 809f0000)", rc);
    check(rc == (int)0x809F0000, line);

    /* set up again without terminating: BUSY */
    rc = sceSaveDataSetupSaveDataMemory2(&su, &res);
    snprintf(line, sizeof(line), "save: a second setup -> %x (BUSY 809f0003)", rc);
    check(rc == (int)0x809F0003, line);

    /* Unity's way: terminate, initialize, set up again, read */
    rc = sceSaveDataTerminate();
    int rc2 = sceSaveDataInitialize3(NULL);
    memset(&res, 0, sizeof(res));
    int rc3 = sceSaveDataSetupSaveDataMemory2(&su, &res);
    struct mem_hdr h2;
    memset(&h2, 0, sizeof(h2));
    int rc4 = mem_read(user, &h2, sizeof(h2), 0);
    snprintf(line, sizeof(line),
             "save: terminate %x, initialize %x, setup %x (existed %lu), read %x: boot %u's header", rc, rc2, rc3,
             (unsigned long)res.existed, rc4, h2.boots);
    check(rc == 0 && rc2 == 0 && rc3 == 0 && res.existed == MEM_SIZE && rc4 == 0 && memcmp(&h, &h2, sizeof(h)) == 0,
          line);

    /* the SDK 1.x calls see the same memory, and the param came back */
    memset(&h2, 0, sizeof(h2));
    rc = sceSaveDataGetSaveDataMemory(user, &h2, sizeof(h2), 0);
    struct sd_param p2;
    struct sd_mem_get2 g;
    memset(&p2, 0, sizeof(p2));
    memset(&g, 0, sizeof(g));
    g.user = user, g.param = &p2;
    rc2 = sceSaveDataGetSaveDataMemory2(&g);
    snprintf(line, sizeof(line), "save: GetSaveDataMemory -> %x (boot %u), its param -> %x: \"%s\" / \"%s\"", rc,
             h2.boots, rc2, p2.title, p2.subtitle);
    check(rc == 0 && h2.boots == h.boots && rc2 == 0 && strcmp(p2.title, "hbrel") == 0, line);
    sceSaveDataTerminate();
    printf("hbrel: save data memory: boot %u done; close SWRR and boot hbrel again to check it was kept\n", h.boots);
}

/* ================= 2. the title's own prx ================= */

extern int hbmod_value;
int hbmod_add(int a, int b);
int hbmod_calls(void);
uint64_t hbmod_time(void);

static int (*g_tls_fn)(void);
static void (*g_tls_set)(int);
static int g_tls_thread;

static void *tls_thread(void *arg)
{
    g_tls_thread = g_tls_fn() * 100 + g_tls_fn();
    g_tls_set(0);
    return arg;
}

static void test_prx(void)
{
    char line[200];
    /* the boot module */
    check(hbmod_add(2, 3) == 5, "prx: libhbmod.prx (imported) is loaded: hbmod_add(2, 3) = 5");
    hbmod_value = 50;
    int r = hbmod_add(2, 3);
    hbmod_value = 42;
    snprintf(line, sizeof(line), "prx: its exported variable is shared: hbmod_add after hbmod_value = 50 is %d", r);
    check(r == 13, line);
    int c1 = hbmod_calls(), c2 = hbmod_calls();
    check(c1 == 1 && c2 == 2, "prx: its own data keeps state (1, then 2)");
    check(hbmod_time() > 0, "prx: its libkernel import works (sceKernelGetProcessTime)");

    /* the run-time module */
    int res = -1;
    u64 t0 = sceKernelGetProcessTime();
    int h = sceKernelLoadStartModule("/app0/sce_module/libhbdyn.prx", 0, NULL, 0, NULL, &res);
    snprintf(line, sizeof(line), "prx: sceKernelLoadStartModule(/app0/sce_module/libhbdyn.prx) -> %x, start %x, %lu ms",
             h, res, (unsigned long)((sceKernelGetProcessTime() - t0) / 1000));
    check(h > 0 && res == 0, line);
    if (h <= 0)
        return;
    int (*mul)(int, int) = NULL, (*chain)(int) = NULL;
    const char *(*name)(void) = NULL;
    uint64_t (*tm)(void) = NULL;
    int *value = NULL;
    void *nope = (void *)1;
    int r1 = sceKernelDlsym(h, "hbdyn_mul", (void **)&mul), r2 = sceKernelDlsym(h, "hbdyn_chain", (void **)&chain);
    int r3 = sceKernelDlsym(h, "hbdyn_tls", (void **)&g_tls_fn), r4 = sceKernelDlsym(h, "hbdyn_value", (void **)&value);
    int r5 = sceKernelDlsym(h, "hbdyn_name", (void **)&name), r6 = sceKernelDlsym(h, "hbdyn_time", (void **)&tm);
    int r7 = sceKernelDlsym(h, "hbdyn_missing", &nope);
    r7 = sceKernelDlsym(h, "hbdyn_tls_set", (void **)&g_tls_set) == 0 ? r7 : 0;
    snprintf(line, sizeof(line), "prx: dlsym %x %x %x %x %x %x; a missing name -> %x", r1, r2, r3, r4, r5, r6, r7);
    check(!r1 && !r2 && !r3 && !r4 && !r5 && !r6 && mul && chain && g_tls_fn && value && name && tm && r7 != 0, line);
    if (r1 || r2 || r3 || r4 || r5 || r6)
        return;
    snprintf(line, sizeof(line), "prx: hbdyn_mul(6, 7) = %d, hbdyn_value = %d, hbdyn_name() = %s", mul(6, 7), *value,
             name());
    check(mul(6, 7) == 42 && *value == 1234 && strcmp(name(), "libhbdyn") == 0, line);
    snprintf(line, sizeof(line), "prx: hbdyn_chain(10) = %d (its import of libhbmod's hbmod_add)", chain(10));
    check(chain(10) == 11, line);
    check(tm() > 0, "prx: the run-time module's libkernel import works");
    /* local-exec from a prx (see hbmod.c): as on a PS4 it shares the
     * executable's TLS (the upper half of musl's __musl_current_locale
     * here), per thread; put back to 0 after, or musl would take it for a
     * locale */
    int a0 = g_tls_fn() - 1;
    g_tls_set(0);
    int a = g_tls_fn(), b = g_tls_fn();
    g_tls_set(0);
    pthread_t th;
    int prc = pthread_create(&th, NULL, tls_thread, NULL);
    if (prc == 0)
        pthread_join(th, NULL);
    snprintf(line, sizeof(line),
             "prx: its thread-local variable: %d at first (0 as on a PS4), then %d, %d; %d, %d in a new thread", a0,
             a, b, g_tls_thread / 100, g_tls_thread % 100);
    check(a0 == 0 && a == 1 && b == 2 && g_tls_thread == 102, line);

    int res2 = -1;
    int h2 = sceKernelLoadStartModule("/app0/sce_module/libhbdyn.prx", 0, NULL, 0, NULL, &res2);
    snprintf(line, sizeof(line), "prx: loading it again -> %x (the same handle %x)", h2, h);
    check(h2 == h, line);
    int hm = sceKernelLoadStartModule("/app0/sce_module/libhbmod.prx", 0, NULL, 0, NULL, NULL);
    void *add = NULL;
    int r8 = hm > 0 ? sceKernelDlsym(hm, "hbmod_add", &add) : -1;
    snprintf(line, sizeof(line), "prx: the boot module by path -> %x, dlsym(hbmod_add) %x: %s", hm, r8,
             add == (void *)hbmod_add ? "the function the import binds" : "another address");
    check(hm > 0 && r8 == 0 && add == (void *)hbmod_add, line);
    int hx = sceKernelLoadStartModule("/app0/sce_module/libnothere.prx", 0, NULL, 0, NULL, NULL);
    snprintf(line, sizeof(line), "prx: a missing prx -> %x (an error)", hx);
    check(hx < 0, line);
    /* libkernel_sys (PS4-Xplorer looks this up and calls it) */
    int hk = sceKernelLoadStartModule("libkernel_sys.sprx", 0, NULL, 0, NULL, NULL);
    const char *(*word)(void) = NULL;
    int rw = hk > 0 ? sceKernelDlsym(hk, "sceKernelGetFsSandboxRandomWord", (void **)&word) : -1;
    const char *wv = rw == 0 && word ? word() : NULL;
    snprintf(line, sizeof(line), "prx: libkernel_sys -> %x, sceKernelGetFsSandboxRandomWord -> %x %p: %s", hk, rw,
             (void *)word, wv && wv[0] ? "a word" : "nothing");
    check(hk > 0 && rw == 0 && word && wv && wv[0], line);
    int ru = sceKernelStopUnloadModule(h, 0, NULL, 0, NULL, NULL);
    snprintf(line, sizeof(line), "prx: sceKernelStopUnloadModule -> %x", ru);
    check(ru == 0, line);
}

/* ================= 3. Piglet: GLES2 ================= */

#define GL_POINTS 0
#define GL_LINES 1
#define GL_LINE_LOOP 2
#define GL_LINE_STRIP 3
#define GL_TRIANGLES 4
#define GL_TRIANGLE_STRIP 5
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE 1
#define GL_FUNC_ADD 0x8006
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_ARRAY_BUFFER 0x8892
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_NO_ERROR 0

void *eglGetDisplay(void *native);
u32 eglInitialize(void *dpy, s32 *major, s32 *minor);
u32 eglBindAPI(u32 api);
u32 eglChooseConfig(void *dpy, const s32 *attr, void **configs, s32 size, s32 *num);
void *eglCreateWindowSurface(void *dpy, void *config, void *win, const s32 *attr);
void *eglCreateContext(void *dpy, void *config, void *share, const s32 *attr);
u32 eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx);
u32 eglSwapBuffers(void *dpy, void *surface);
u32 eglSwapInterval(void *dpy, s32 interval);
u32 scePigletSetConfigurationVSH(const void *cfg);
u32 glCreateShader(u32 type);
void glShaderSource(u32 shader, s32 count, const char *const *src, const s32 *len);
void glCompileShader(u32 shader);
void glGetShaderiv(u32 shader, u32 pname, s32 *out);
void glGetShaderInfoLog(u32 shader, s32 size, s32 *len, char *log);
u32 glCreateProgram(void);
void glAttachShader(u32 program, u32 shader);
void glBindAttribLocation(u32 program, u32 index, const char *name);
void glLinkProgram(u32 program);
void glGetProgramiv(u32 program, u32 pname, s32 *out);
void glUseProgram(u32 program);
s32 glGetUniformLocation(u32 program, const char *name);
void glUniform1i(s32 loc, s32 v);
void glUniformMatrix4fv(s32 loc, s32 count, u8 transpose, const float *m);
void glGenBuffers(s32 n, u32 *out);
void glBindBuffer(u32 target, u32 buf);
void glBufferData(u32 target, intptr_t size, const void *data, u32 usage);
void glVertexAttribPointer(u32 index, s32 size, u32 type, u8 norm, s32 stride, const void *ptr);
void glEnableVertexAttribArray(u32 index);
void glDisableVertexAttribArray(u32 index);
void glDrawArrays(u32 mode, s32 first, s32 count);
void glViewport(s32 x, s32 y, s32 w, s32 h);
void glScissor(s32 x, s32 y, s32 w, s32 h);
void glEnable(u32 cap);
void glDisable(u32 cap);
void glBlendFuncSeparate(u32 sr, u32 dr, u32 sa, u32 da);
void glBlendEquationSeparate(u32 rgb, u32 a);
void glClearColor(float r, float g, float b, float a);
void glClear(u32 mask);
void glGenTextures(s32 n, u32 *out);
void glBindTexture(u32 target, u32 tex);
void glActiveTexture(u32 unit);
void glTexParameteri(u32 target, u32 pname, s32 v);
void glTexImage2D(u32 target, s32 level, s32 ifmt, s32 w, s32 h, s32 border, u32 fmt, u32 type, const void *px);
void glTexSubImage2D(u32 target, s32 level, s32 x, s32 y, s32 w, s32 h, u32 fmt, u32 type, const void *px);
void glPixelStorei(u32 pname, s32 v);
void glGenFramebuffers(s32 n, u32 *out);
void glBindFramebuffer(u32 target, u32 fbo);
void glFramebufferTexture2D(u32 target, u32 attachment, u32 textarget, u32 tex, s32 level);
u32 glCheckFramebufferStatus(u32 target);
void glReadPixels(s32 x, s32 y, s32 w, s32 h, u32 fmt, u32 type, void *out);
u32 glGetError(void);

/* SDL 2.0.18+'s GLES2 shaders (SDL_shaders_gles2.c, zlib licence): the ones
 * ghostldr recognises */
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
static const char k_fs_yuv[] =
    "precision mediump float;\nuniform sampler2D u_texture;\nuniform sampler2D u_texture_u;\nuniform sampler2D "
    "u_texture_v;\nvarying vec4 v_color;\nvarying vec2 v_texCoord;\n\n// YUV offset \nconst vec3 offset = vec3(0, "
    "-0.501960814, -0.501960814);\n\n// RGB coefficients \nconst mat3 matrix = mat3( 1,       1,        1,\n      "
    "                    0,      -0.3441,   1.772,\n                          1.402,  -0.7141,   0);\n\nvoid main()\n"
    "{\n    mediump vec3 yuv;\n    lowp vec3 rgb;\n\n    // Get the YUV values \n    yuv.x = texture2D(u_texture,   "
    "v_texCoord).r;\n    yuv.y = texture2D(u_texture_u, v_texCoord).r;\n    yuv.z = texture2D(u_texture_v, "
    "v_texCoord).r;\n\n    // Do the color transform \n    yuv += offset;\n    rgb = matrix * yuv;\n\n    // That "
    "was easy. :) \n    gl_FragColor = vec4(rgb, 1);\n    gl_FragColor *= v_color;\n}";
static const char k_fs_nv12[] =
    "precision mediump float;\nuniform sampler2D u_texture;\nuniform sampler2D u_texture_u;\nuniform sampler2D "
    "u_texture_v;\nvarying vec4 v_color;\nvarying vec2 v_texCoord;\nconst vec3 offset = vec3(-0.0627451017, "
    "-0.501960814, -0.501960814);\nconst mat3 matrix = mat3( 1.1644,  1.1644,   1.1644,\n 0,      -0.3918,   "
    "2.0172,\n 1.596,  -0.813,    0);\nvoid main()\n{\n    mediump vec3 yuv;\n    lowp vec3 rgb;\n    yuv.x = "
    "texture2D(u_texture,   v_texCoord).r;\n    yuv.yz = texture2D(u_texture_u, v_texCoord).ra;\n    yuv += "
    "offset;\n    rgb = matrix * yuv;\n    gl_FragColor = vec4(rgb, 1);\n    gl_FragColor *= v_color;\n}";
static const char k_fs_nv21[] =
    "precision mediump float;\nuniform sampler2D u_texture;\nuniform sampler2D u_texture_u;\nuniform sampler2D "
    "u_texture_v;\nvarying vec4 v_color;\nvarying vec2 v_texCoord;\nconst vec3 offset = vec3(-0.0627451017, "
    "-0.501960814, -0.501960814);\nconst mat3 matrix = mat3( 1.1644,  1.1644,   1.1644,\n 0,      -0.3918,   "
    "2.0172,\n 1.596,  -0.813,    0);\nvoid main()\n{\n    mediump vec3 yuv;\n    lowp vec3 rgb;\n    yuv.x = "
    "texture2D(u_texture,   v_texCoord).r;\n    yuv.yz = texture2D(u_texture_u, v_texCoord).ar;\n    yuv += "
    "offset;\n    rgb = matrix * yuv;\n    gl_FragColor = vec4(rgb, 1);\n    gl_FragColor *= v_color;\n}";

#define W 1920
#define H 1080
#define RT 256 /* the render target's size */
#define VW 64  /* the video textures' size */
enum { A_POS = 0, A_COLOR = 1, A_TEX = 2 };

struct sv { /* SDL 2.0.18+'s vertex: x, y, colour, u, v */
    float x, y;
    u8 c[4];
    float u, v;
};

static void *g_dpy, *g_surf;
static u32 g_vbo, g_solid, g_abgr, g_yuv, g_nv12, g_nv21;
static u32 g_tex_rgba, g_tex_glyph, g_tex_y, g_tex_u, g_tex_v, g_tex_uv, g_tex_vu, g_rt, g_fbo;
static struct sv *g_pts, *g_lines;
static u32 g_npts, g_nlines;

static u32 program(const char *fs_src, const char *what)
{
    u32 vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
    const char *two[2] = {"", k_vs};
    glShaderSource(vs, 2, two, NULL);
    glCompileShader(vs);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    s32 okv = 0, okf = 0, linked = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &okv);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &okf);
    u32 p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, A_POS, "a_position");
    glBindAttribLocation(p, A_COLOR, "a_color");
    glBindAttribLocation(p, A_TEX, "a_texCoord");
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    char line[160];
    snprintf(line, sizeof(line), "gles: program %s: vertex %d, fragment %d, linked %d", what, okv, okf, linked);
    check(okv && okf && linked, line);
    if (!okf) {
        char log[200] = {0};
        glGetShaderInfoLog(fs, sizeof(log), NULL, log);
        printf("hbrel: its log: %s\n", log);
    }
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "u_texture"), 0);
    glUniform1i(glGetUniformLocation(p, "u_texture_u"), 1);
    glUniform1i(glGetUniformLocation(p, "u_texture_v"), 2);
    return p;
}

/* SDL's projection: the window (y down from the top) or a target (y up) */
static void project(u32 w, u32 h, int target)
{
    float m[16] = {0};
    m[0] = 2.0f / (float)w, m[5] = (target ? 2.0f : -2.0f) / (float)h;
    m[12] = -1.0f, m[13] = target ? -1.0f : 1.0f, m[15] = 1.0f;
    const u32 progs[5] = {g_solid, g_abgr, g_yuv, g_nv12, g_nv21};
    for (int i = 0; i < 5; i++) {
        glUseProgram(progs[i]);
        glUniformMatrix4fv(glGetUniformLocation(progs[i], "u_projection"), 1, 0, m);
    }
}

static void draw(u32 prog, u32 mode, const struct sv *v, u32 n, int tex)
{
    glUseProgram(prog);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, (intptr_t)(sizeof(*v) * n), v, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(A_POS, 2, GL_FLOAT, 0, sizeof(struct sv), (const void *)0);
    glVertexAttribPointer(A_COLOR, 4, GL_UNSIGNED_BYTE, 1, sizeof(struct sv), (const void *)8);
    glEnableVertexAttribArray(A_POS);
    glEnableVertexAttribArray(A_COLOR);
    if (tex) {
        glVertexAttribPointer(A_TEX, 2, GL_FLOAT, 0, sizeof(struct sv), (const void *)12);
        glEnableVertexAttribArray(A_TEX);
    } else
        glDisableVertexAttribArray(A_TEX);
    glDrawArrays(mode, 0, (s32)n);
}

static void quad(u32 prog, float x0, float y0, float x1, float y1, const u8 c[4], int tex)
{
    struct sv v[4] = {{x0, y0, {c[0], c[1], c[2], c[3]}, 0, 0},
                      {x1, y0, {c[0], c[1], c[2], c[3]}, 1, 0},
                      {x0, y1, {c[0], c[1], c[2], c[3]}, 0, 1},
                      {x1, y1, {c[0], c[1], c[2], c[3]}, 1, 1}};
    draw(prog, GL_TRIANGLE_STRIP, v, 4, tex);
}

static void tex_params(int nearest)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static u32 new_tex(u32 fmt, u32 w, u32 h, const void *px)
{
    u32 t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    tex_params(1);
    glTexImage2D(GL_TEXTURE_2D, 0, (s32)fmt, (s32)w, (s32)h, 0, fmt, GL_UNSIGNED_BYTE, px);
    return t;
}

static void gles_setup(void)
{
    char line[200];
    check(scePigletSetConfigurationVSH((const void *)0x1000) != 0, "gles: scePigletSetConfigurationVSH");
    g_dpy = eglGetDisplay(NULL);
    s32 maj = 0, min = 0;
    check(g_dpy && eglInitialize(g_dpy, &maj, &min), "gles: eglInitialize");
    eglBindAPI(0x30A0);
    const s32 want[] = {0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3040, 4, 0x3038};
    void *cfg[4];
    s32 n = 0;
    check(eglChooseConfig(g_dpy, want, cfg, 4, &n) && n >= 1, "gles: eglChooseConfig");
    static u32 native[3] = {0, W, H};
    g_surf = eglCreateWindowSurface(g_dpy, cfg[0], native, NULL);
    const s32 ctx_attr[] = {0x3098, 2, 0x3038};
    void *ctx = eglCreateContext(g_dpy, cfg[0], NULL, ctx_attr);
    check(g_surf && ctx && eglMakeCurrent(g_dpy, g_surf, g_surf, ctx), "gles: 1920x1080 window surface current");
    eglSwapInterval(g_dpy, 1);
    glGenBuffers(1, &g_vbo);
    g_solid = program(k_fs_solid, "solid");
    g_abgr = program(k_fs_abgr, "texture");
    g_yuv = program(k_fs_yuv, "YUV");
    g_nv12 = program(k_fs_nv12, "NV12");
    g_nv21 = program(k_fs_nv21, "NV21");

    /* an RGBA texture: 7x5 texels, each its own colour (scaled, it shows
     * which texel each pixel took) */
    static u8 rgba[7 * 5 * 4];
    for (u32 i = 0; i < 35; i++)
        rgba[i * 4] = (u8)(i * 37), rgba[i * 4 + 1] = (u8)(255 - i * 23), rgba[i * 4 + 2] = (u8)(i * 61),
                 rgba[i * 4 + 3] = 255;
    g_tex_rgba = new_tex(GL_RGBA, 7, 5, rgba);
    /* a "glyph": white with alpha, 11x9, like a font atlas cell */
    static u8 glyph[11 * 9 * 4];
    for (u32 y = 0; y < 9; y++)
        for (u32 x = 0; x < 11; x++) {
            u8 a = (u8)(((x * 3 + y * 5) % 7 == 0 || x == 5 || y == 4) ? 255 : (x + y) % 3 == 0 ? 128 : 0);
            u8 *p = &glyph[(y * 11 + x) * 4];
            p[0] = p[1] = p[2] = 255, p[3] = a;
        }
    g_tex_glyph = new_tex(GL_RGBA, 11, 9, glyph);

    /* video: a 64x64 picture, Y a diagonal ramp, U and V horizontal and vertical ramps (32x32) */
    static u8 yp[VW * VW], up[VW / 2 * VW / 2], vp[VW / 2 * VW / 2], uv[VW / 2 * VW / 2 * 2], vu[VW / 2 * VW / 2 * 2];
    for (u32 y = 0; y < VW; y++)
        for (u32 x = 0; x < VW; x++)
            yp[y * VW + x] = (u8)(16 + (x + y) * 219 / (2 * VW - 2));
    for (u32 y = 0; y < VW / 2; y++)
        for (u32 x = 0; x < VW / 2; x++) {
            u32 i = y * VW / 2 + x;
            up[i] = (u8)(16 + x * 224 / (VW / 2 - 1)), vp[i] = (u8)(240 - y * 224 / (VW / 2 - 1));
            uv[i * 2] = up[i], uv[i * 2 + 1] = vp[i], vu[i * 2] = vp[i], vu[i * 2 + 1] = up[i];
        }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    g_tex_y = new_tex(GL_LUMINANCE, VW, VW, yp);
    g_tex_u = new_tex(GL_LUMINANCE, VW / 2, VW / 2, up);
    g_tex_v = new_tex(GL_LUMINANCE, VW / 2, VW / 2, vp);
    g_tex_uv = new_tex(GL_LUMINANCE_ALPHA, VW / 2, VW / 2, uv);
    g_tex_vu = new_tex(GL_LUMINANCE_ALPHA, VW / 2, VW / 2, vu);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    /* the render target */
    g_rt = new_tex(GL_RGBA, RT, RT, NULL);
    glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_rt, 0);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "gles: render target complete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* points: a 48x30 grid 8 px apart at pixel centres, plus quarter-pixel offsets */
    g_pts = malloc(sizeof(struct sv) * 2000);
    for (u32 y = 0; y < 30; y++)
        for (u32 x = 0; x < 48; x++) {
            struct sv *p = &g_pts[g_npts++];
            memset(p, 0, sizeof(*p));
            p->x = 20.5f + x * 8 + (y % 4) * 0.25f, p->y = 20.5f + y * 8 + (x % 4) * 0.25f;
            p->c[0] = (u8)(x * 5), p->c[1] = (u8)(y * 8), p->c[2] = (u8)(255 - x * 5), p->c[3] = 255;
        }
    /* lines: a fan of 72 from (720, 160) to a circle of radius 140, every slope */
    g_lines = malloc(sizeof(struct sv) * 400);
    for (u32 i = 0; i < 72; i++) {
        float a = (float)i * 6.2831853f / 72.0f;
        /* cos/sin by a short series is enough here; no libm needed */
        float a2 = a > 3.14159265f ? a - 6.2831853f : a;
        float s = a2 - a2 * a2 * a2 / 6 + a2 * a2 * a2 * a2 * a2 / 120 - a2 * a2 * a2 * a2 * a2 * a2 * a2 / 5040;
        float b = a2 + 1.5707963f;
        b = b > 3.14159265f ? b - 6.2831853f : b;
        float c = b - b * b * b / 6 + b * b * b * b * b / 120 - b * b * b * b * b * b * b / 5040;
        struct sv *l = &g_lines[g_nlines];
        memset(l, 0, sizeof(*l) * 2);
        l[0].x = 720.5f, l[0].y = 160.5f;
        l[1].x = 720.5f + 140 * c, l[1].y = 160.5f + 140 * s;
        l[0].c[0] = l[1].c[0] = (u8)(i * 3), l[0].c[1] = l[1].c[1] = (u8)(255 - i * 3);
        l[0].c[2] = l[1].c[2] = 200, l[0].c[3] = l[1].c[3] = 255;
        g_nlines += 2;
    }
    snprintf(line, sizeof(line), "gles: set up (%u points, %u lines); GL error %x", g_npts, g_nlines / 2, glGetError());
    check(1, line);
}

static const u8 k_white[4] = {255, 255, 255, 255};

/* into the render target: a clear, triangles, lines, points and a YUV quad */
static void draw_target(void)
{
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, RT, RT);
    project(RT, RT, 1);
    glClearColor(0.1f, 0.2f, 0.4f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    const u8 red[4] = {230, 40, 30, 255}, green[4] = {30, 200, 60, 255};
    struct sv tri[3] = {{16, 16, {red[0], red[1], red[2], 255}, 0, 0},
                        {240, 40, {green[0], green[1], green[2], 255}, 0, 0},
                        {60, 230, {40, 60, 250, 255}, 0, 0}};
    draw(g_solid, GL_TRIANGLES, tri, 3, 0);
    quad(g_solid, 0, 0, RT, 8, green, 0); /* SDL's top 8 rows (GL rows 0-7 of the target) */
    struct sv box[4] = {{100.5f, 100.5f, {255, 255, 0, 255}, 0, 0},
                        {200.5f, 100.5f, {255, 255, 0, 255}, 0, 0},
                        {200.5f, 200.5f, {255, 255, 0, 255}, 0, 0},
                        {100.5f, 200.5f, {255, 255, 0, 255}, 0, 0}};
    draw(g_solid, GL_LINE_LOOP, box, 4, 0);
    struct sv dots[16];
    for (u32 i = 0; i < 16; i++) {
        memset(&dots[i], 0, sizeof(dots[i]));
        dots[i].x = 120.5f + i * 4, dots[i].y = 150.5f;
        dots[i].c[0] = dots[i].c[1] = dots[i].c[2] = dots[i].c[3] = 255;
    }
    draw(g_solid, GL_POINTS, dots, 16, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_tex_y);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, g_tex_u);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, g_tex_v);
    glActiveTexture(GL_TEXTURE0);
    quad(g_yuv, 160, 16, 224, 80, k_white, 1);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    project(W, H, 0);
}

static void video(u32 prog, u32 t1, u32 t2, u32 t3, float x, float y, float scale, int nearest)
{
    const u32 t[3] = {t1, t2, t3};
    for (int i = 2; i >= 0; i--) {
        if (!t[i])
            continue;
        glActiveTexture(GL_TEXTURE0 + (u32)i);
        glBindTexture(GL_TEXTURE_2D, t[i]);
        tex_params(nearest);
    }
    quad(prog, x, y, x + VW * scale, y + VW * scale, k_white, 1);
}

static void draw_frame(void)
{
    draw_target();
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_BLEND);
    /* points and lines */
    draw(g_solid, GL_POINTS, g_pts, g_npts, 0);
    draw(g_solid, GL_LINES, g_lines, g_nlines, 0);
    struct sv strip[6] = {{900.5f, 30.5f, {255, 128, 0, 255}, 0, 0},  {1000.25f, 90.75f, {255, 128, 0, 255}, 0, 0},
                          {1100.5f, 30.5f, {0, 255, 128, 255}, 0, 0}, {1200.0f, 290.0f, {128, 0, 255, 255}, 0, 0},
                          {1201.5f, 30.5f, {255, 255, 255, 255}, 0, 0}, {1400.5f, 31.5f, {255, 0, 0, 255}, 0, 0}};
    draw(g_solid, GL_LINE_STRIP, strip, 6, 0);
    struct sv loop[4] = {{1450.5f, 40.5f, {0, 200, 255, 255}, 0, 0},
                         {1850.5f, 40.5f, {0, 200, 255, 255}, 0, 0},
                         {1850.5f, 280.5f, {255, 200, 0, 255}, 0, 0},
                         {1450.5f, 280.5f, {255, 200, 0, 255}, 0, 0}};
    draw(g_solid, GL_LINE_LOOP, loop, 4, 0);
    /* video: YUV 1:1 and x3 linear, NV12 x2.5 linear, NV21 1:1 and x2 nearest */
    video(g_yuv, g_tex_y, g_tex_u, g_tex_v, 20, 330, 1.0f, 1);
    video(g_yuv, g_tex_y, g_tex_u, g_tex_v, 100, 330, 3.0f, 0);
    video(g_nv12, g_tex_y, g_tex_uv, 0, 300, 330, 2.5f, 0);
    video(g_nv21, g_tex_y, g_tex_vu, 0, 470, 330, 1.0f, 1);
    video(g_nv21, g_tex_y, g_tex_vu, 0, 550, 330, 2.0f, 1);
    /* the render target: 1:1 (nearest), x1.5 (linear) */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_rt);
    tex_params(1);
    quad(g_abgr, 20, 560, 20 + RT, 560 + RT, k_white, 1);
    tex_params(0);
    quad(g_abgr, 300, 560, 300 + RT * 1.5f, 560 + RT * 1.5f, k_white, 1);
    /* a 7x5 texture scaled by 1.5, 2.5, 10/3 and 0.75, nearest; and 2.5 linear */
    glBindTexture(GL_TEXTURE_2D, g_tex_rgba);
    tex_params(1);
    const float sc[4] = {1.5f, 2.5f, 10.0f / 3.0f, 0.75f};
    float x = 750;
    for (int i = 0; i < 4; i++) {
        quad(g_abgr, x, 560, x + 7 * sc[i] * 4, 560 + 5 * sc[i] * 4, k_white, 1);
        x += 7 * sc[i] * 4 + 20;
    }
    tex_params(0);
    quad(g_abgr, 750, 760, 750 + 7 * 2.5f * 4, 760 + 5 * 2.5f * 4, k_white, 1);
    /* nearest at scales where pixel centres fall exactly on texel edges
     * (as scaled text does): 1.5, 2.5, 1.25 and 3.5, from whole and half pixels */
    tex_params(1);
    const float tie[4] = {1.5f, 2.5f, 1.25f, 3.5f};
    x = 750;
    for (int i = 0; i < 8; i++) {
        float t = tie[i & 3], x0 = x + (i >= 4 ? 0.5f : 0.0f);
        quad(g_abgr, x0, 880, x0 + 7 * t, 880 + 5 * t, k_white, 1);
        x += 7 * t + 8;
    }
    /* glyphs, blended, at the scales text is drawn at, tinted */
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBindTexture(GL_TEXTURE_2D, g_tex_glyph);
    const u8 tint[4] = {250, 220, 120, 255};
    x = 1000;
    for (int i = 0; i < 12; i++) {
        float s = 1.0f + (float)i * 0.37f;
        tex_params(i & 1);
        quad(g_abgr, x, 760, x + 11 * s, 760 + 9 * s, tint, 1);
        x += 11 * s + 6;
    }
    /* half-transparent rectangles over everything */
    const u8 half[4] = {255, 255, 255, 96}, blue[4] = {40, 80, 255, 160};
    quad(g_solid, 1450, 330, 1850, 500, half, 0);
    quad(g_solid, 1600, 420, 1900, 700, blue, 0);
    glDisable(GL_BLEND);
    /* a scissored clear */
    glEnable(GL_SCISSOR_TEST);
    glScissor(1500, H - 1000, 300, 100);
    glClearColor(0.6f, 0.1f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

static u32 px(u32 x, u32 y) /* the window at (x, y) from the top, R G B A bytes */
{
    u32 c = 0;
    glReadPixels((s32)x, (s32)(H - 1 - y), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &c);
    return c;
}

static u32 rgba(u32 r, u32 g, u32 b) { return r | g << 8 | b << 16 | 0xFFu << 24; }

static int near(u32 a, u32 b, int tol)
{
    for (int k = 0; k < 32; k += 8) {
        int d = (int)((a >> k) & 255) - (int)((b >> k) & 255);
        if (d > tol || d < -tol)
            return 0;
    }
    return 1;
}

static void check_px(u32 x, u32 y, u32 want, int tol, const char *what)
{
    char line[200];
    u32 got = px(x, y);
    snprintf(line, sizeof(line), "gles: %s: (%u, %u) is %08x (want %08x%s)", what, x, y, got, want,
             tol ? ", +-1" : "");
    check(near(got, want, tol), line);
}

static u32 yuv_jpeg(u32 Y, u32 U, u32 V)
{
    float y = Y / 255.0f, u = U / 255.0f - 0.501960814f, v = V / 255.0f - 0.501960814f;
    float c[3] = {y + 1.402f * v, y - 0.3441f * u - 0.7141f * v, y + 1.772f * u};
    u32 o[3];
    for (int i = 0; i < 3; i++)
        o[i] = c[i] <= 0 ? 0 : c[i] >= 1 ? 255 : (u32)(c[i] * 255 + 0.5f);
    return rgba(o[0], o[1], o[2]);
}

static u32 yuv_601(u32 Y, u32 U, u32 V)
{
    float y = Y / 255.0f - 0.0627451017f, u = U / 255.0f - 0.501960814f, v = V / 255.0f - 0.501960814f;
    float c[3] = {1.1644f * y + 1.596f * v, 1.1644f * y - 0.3918f * u - 0.813f * v, 1.1644f * y + 2.0172f * u};
    u32 o[3];
    for (int i = 0; i < 3; i++)
        o[i] = c[i] <= 0 ? 0 : c[i] >= 1 ? 255 : (u32)(c[i] * 255 + 0.5f);
    return rgba(o[0], o[1], o[2]);
}

/* the first frame, read back before it is shown */
static void check_frame(void)
{
    /* a point at its pixel centre, and the fan's centre (every line starts there) */
    const struct sv *p = &g_pts[0];
    check_px(20, 20, rgba(p->c[0], p->c[1], p->c[2]), 0, "a point");
    p = &g_pts[5 * 48 + 7]; /* x 76.75, y 61.25: (76, 61) */
    check_px(76, 61, rgba(p->c[0], p->c[1], p->c[2]), 0, "a point a quarter pixel off");
    /* the fan's first line, to (860.5, 160.5): its last pixel is (859, 160) (diamond exit) */
    check_px(859, 160, rgba(0, 255, 200), 0, "the fan's first line");
    check_px(1100, 30, rgba(0, 255, 128), 0, "the strip's third vertex");
    check_px(1650, 40, rgba(0, 200, 255), 0, "the loop's top edge");
    /* the closing edge, (1450.5, 280.5) -> (1450.5, 40.5): halfway, half of each colour */
    check_px(1450, 160, rgba(128, 200, 128), 2, "the loop's closing edge (interpolated colour)");
    /* video, 1:1 nearest: pixel (x, y) is Y[y][x], U[y/2][x/2], V[y/2][x/2] */
    u32 X = 9, Y = 21;
    u32 yv = 16 + (X + Y) * 219 / (2 * VW - 2), uv = 16 + (X / 2) * 224 / (VW / 2 - 1),
        vv = 240 - (Y / 2) * 224 / (VW / 2 - 1);
    check_px(20 + X, 330 + Y, yuv_jpeg(yv, uv, vv) & 0xFFFFFFFF, 1, "YUV (three planes) 1:1");
    check_px(470 + X, 330 + Y, yuv_601(yv, uv, vv), 1, "NV21 1:1");
    check_px(550 + 2 * X, 330 + 2 * Y, yuv_601(yv, uv, vv), 1, "NV21 doubled (nearest)");
    /* the render target at 1:1: its top rows green, the clear colour, its points */
    check_px(20 + 3, 560 + 2, rgba(30, 200, 60), 0, "the render target: its top rows");
    check_px(20 + 250, 560 + 250, rgba(26, 51, 102), 1, "the render target: its clear colour");
    check_px(20 + 120, 560 + 150, rgba(255, 255, 255), 0, "the render target: a point");
    check_px(20 + 150, 560 + 100, rgba(255, 255, 0), 0, "the render target: the loop's top edge");
    /* read straight from the target: GL row 0 is SDL's top row */
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    u32 top = 0, pt = 0;
    glReadPixels(3, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &top);
    glReadPixels(120, 150, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pt);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    char line[160];
    snprintf(line, sizeof(line), "gles: glReadPixels of the target: (3, 2) %08x, (120, 150) %08x", top, pt);
    check(top == rgba(30, 200, 60) && pt == rgba(255, 255, 255), line);
    /* a 7x5 texture at 1.5x4 = x6 nearest: texel (i, j) covers 6x6 pixels from (750 + 6i, 560 + 6j) */
    check_px(750 + 6 * 3 + 2, 560 + 6 * 2 + 3, rgba((17 * 37) & 255, (255 - 17 * 23) & 255, (17 * 61) & 255), 0,
             "a texture scaled x6, nearest");
    /* a texel-edge tie (nearest, x1.5 from x 750): pixel 751's centre is
     * exactly where texel 0 ends; GL's rule (floor) takes texel 1 */
    u32 t0 = rgba(0, 255, 0), t1 = rgba(37, 232, 61), got = px(751, 880);
    printf("hbrel: gles: tie at a texel edge: pixel (751, 880) took texel %s (%08x)\n",
           got == t1 ? "1, as GL's rule says" : got == t0 ? "0" : "neither", got);
    /* the scissored clear: GL's box (1500, 80, 300, 100) is rows 900-999 from the top */
    check_px(1600, 950, rgba(153, 26, 77), 1, "a scissored clear: inside");
    check_px(1600, 899, rgba(13, 13, 20), 1, "a scissored clear: the row above it");
}

static void test_gles(void)
{
    gles_setup();
    u64 t0 = sceKernelGetProcessTime(), tl = t0;
    for (u32 frame = 1;; frame++) {
        draw_frame();
        if (frame == 1)
            check_frame();
        eglSwapBuffers(g_dpy, g_surf);
        if (frame == 1)
            printf("hbrel: %d passed, %d failed; drawing the same frame from now on\n", g_pass, g_fail);
        if (frame % 600 == 0) {
            u64 t = sceKernelGetProcessTime();
            printf("hbrel: frame %u, %.2f ms a frame\n", frame, (double)(t - tl) / 600 / 1000);
            tl = t;
        }
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("hbrel: start\n");
    sceUserServiceInitialize(NULL);
    s32 user = -1;
    sceUserServiceGetInitialUser(&user);
    test_save_memory(user);
    test_prx();
    printf("hbrel: before GLES: %d passed, %d failed\n", g_pass, g_fail);
    test_gles();
    return 0;
}
