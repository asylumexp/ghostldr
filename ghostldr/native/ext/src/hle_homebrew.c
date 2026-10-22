/*
 * What OpenOrbis homebrew needs beyond the file and kernel families
 * (hle_file.c, hle_kernel.c) and thread-specific data (hle_tsd.c):
 *
 *  - stdout and stderr: musl's stdio writes them with writev; they go to
 *    the log line by line ("ghost1:"/"ghost2:", the file family's write).  ioctl
 *    TIOCGWINSZ on them answers like a terminal, so musl line-buffers
 *    stdout (on a PS4 it is fully buffered, and lines show up late);
 *  - _exit and raise: logged before SWRR's, which end the process;
 *  - FreeType faces from title files: libSceFreeType reads a font path with
 *    SWRR's own file calls, which do not see ghostfs; a title path is read
 *    here and handed to FT_New_Memory_Face instead.
 *
 * Library libSceFreeType is the module libSceFreeTypeOl.sprx provides
 * (resolve.c).
 */
#include "ext.h"

#define TIOCGWINSZ 0x40087468UL

struct iovec {
    const void *base;
    u64 len;
};

typedef s64 (*fn6)(u64, u64, u64, u64, u64, u64);

static fn6 host_once(void *volatile *slot, const char *mod, const char *name)
{
    void *f = *slot;
    if (!f) {
        f = mod ? host_real_in(mod, name) : host_real(name);
        *slot = f ? f : (void *)1;
    }
    return f == (void *)1 ? NULL : (fn6)f;
}

#define HOST(mod, name)            \
    static void *volatile slot_;   \
    fn6 real = host_once(&slot_, mod, name)

/* the extension's write (hle_file.c): stdout/stderr become log lines */
static s64 log_write(int fd, const void *buf, u64 n)
{
    static s64 (*w)(int, const void *, u64);
    if (!w) { /* the file family's _write, else the core's */
        char nid[12];
        compute_nid("_write", nid);
        u64 f = ext_find("libkernel", nid);
        w = (s64(*)(int, const void *, u64))(f ? f : hle_lookup("libkernel", "_write"));
    }
    return w ? w(fd, buf, n) : (s64)n;
}

static s64 do_writev(int fd, const struct iovec *iov, int n, fn6 real)
{
    if (fd != 1 && fd != 2)
        return real ? real((u64)fd, (u64)iov, (u64)n, 0, 0, 0) : -1;
    s64 total = 0;
    for (int i = 0; i < n; i++)
        if (iov[i].len)
            total += log_write(fd, iov[i].base, iov[i].len);
    return total;
}

static s64 hb_writev(int fd, const struct iovec *iov, int n)
{
    HOST(NULL, "writev");
    return do_writev(fd, iov, n, real);
}

static s64 hb__writev(int fd, const struct iovec *iov, int n)
{
    HOST(NULL, "_writev");
    return do_writev(fd, iov, n, real);
}

static s64 do_ioctl(int fd, u64 req, u64 arg, fn6 real)
{
    if ((fd == 1 || fd == 2) && req == TIOCGWINSZ) {
        u16 *ws = (u16 *)arg;
        if (ws)
            ws[0] = 25, ws[1] = 80, ws[2] = ws[3] = 0; /* rows, columns */
        return 0;
    }
    return real ? real((u64)fd, req, arg, 0, 0, 0) : -1;
}

static s64 hb_ioctl(int fd, u64 req, u64 arg)
{
    HOST(NULL, "ioctl");
    return do_ioctl(fd, req, arg, real);
}

static s64 hb__ioctl(int fd, u64 req, u64 arg)
{
    HOST(NULL, "_ioctl");
    return do_ioctl(fd, req, arg, real);
}

static void hb__exit(int code)
{
    HOST(NULL, "_exit");
    u64 ra = (u64)__builtin_return_address(0);
    struct module *m = g_main_mod;
    logf("title called _exit(%d) from eboot+%lx: SWRR ends with it", code, m && ra > m->base ? ra - m->base : ra);
    if (real)
        real((u64)code, 0, 0, 0, 0, 0);
    for (;;)
        K.usleep(1000000);
}

static s64 hb_raise(int sig)
{
    HOST(NULL, "raise");
    u64 ra = (u64)__builtin_return_address(0);
    struct module *m = g_main_mod;
    logf("title raised signal %d from eboot+%lx", sig, m && ra > m->base ? ra - m->base : ra);
    return real ? real((u64)sig, 0, 0, 0, 0, 0) : 0;
}

/* ---- FreeType faces from title files ---- */
static struct {
    void *face;
    void *buf;
} g_faces[16];
static volatile int g_faces_lock;

#define FT_ERR_CANNOT_OPEN 1

static s64 hb_FT_New_Face(void *lib, const char *path, s64 index, void **aface)
{
    HOST("libSceFreeTypeOl", "FT_New_Face");
    static void *volatile mem_slot;
    fn6 mem = host_once(&mem_slot, "libSceFreeTypeOl", "FT_New_Memory_Face");
    int fd = -1;
    if (!path || gfs_open(path, 0, &fd) != 0) {
        /* not ghostfs's: SWRR's file calls reach it once remapped */
        char buf[512];
        const char *p = path ? remap_path(path, buf, sizeof(buf)) : path;
        return real ? real((u64)lib, (u64)p, (u64)index, (u64)aface, 0, 0) : FT_ERR_CANNOT_OPEN;
    }
    if (fd < 0 || !mem) {
        logf("FT_New_Face(%s): %x", path, fd);
        return FT_ERR_CANNOT_OPEN;
    }
    s64 size = gfs_lseek(fd, 0, 2);
    gfs_lseek(fd, 0, 0);
    u8 *data = size > 0 ? direct_alloc((u64)size, PROT_READ | PROT_WRITE, 0) : NULL;
    s64 got = data ? gfs_read(fd, data, (u64)size) : -1;
    gfs_close(fd);
    if (got != size) {
        logf("FT_New_Face(%s): read %ld of %ld bytes", path, got, size);
        if (data)
            direct_free(data);
        return FT_ERR_CANNOT_OPEN;
    }
    s64 rc = mem((u64)lib, (u64)data, (u64)size, (u64)index, (u64)aface, 0);
    logf("FT_New_Face(%s, %ld): %ld bytes from ghostfs, FT_New_Memory_Face -> %ld", path, index, size, rc);
    bool kept = false;
    spin_lock(&g_faces_lock);
    for (u32 i = 0; rc == 0 && aface && i < ARRAY_SIZE(g_faces) && !kept; i++)
        if (!g_faces[i].face)
            g_faces[i].face = *aface, g_faces[i].buf = data, kept = true;
    spin_unlock(&g_faces_lock);
    if (rc != 0)
        direct_free(data); /* else the face uses it until FT_Done_Face (kept or not) */
    return rc;
}

static s64 hb_FT_Done_Face(void *face)
{
    HOST("libSceFreeTypeOl", "FT_Done_Face");
    s64 rc = real ? real((u64)face, 0, 0, 0, 0, 0) : 0;
    void *buf = NULL;
    spin_lock(&g_faces_lock);
    for (u32 i = 0; face && i < ARRAY_SIZE(g_faces); i++)
        if (g_faces[i].face == face)
            buf = g_faces[i].buf, g_faces[i].face = g_faces[i].buf = NULL;
    spin_unlock(&g_faces_lock);
    if (buf)
        direct_free(buf);
    return rc;
}

#define LK "libkernel"
static const struct ext_hle_ent k_hle[] = {
    {LK, "writev", hb_writev, NULL},
    {LK, "_writev", hb__writev, NULL},
    {LK, "ioctl", hb_ioctl, NULL},
    {LK, "_ioctl", hb__ioctl, NULL},
    {LK, "_exit", hb__exit, NULL},
    {LK, "raise", hb_raise, NULL},
    {"libSceFreeType", "FT_New_Face", hb_FT_New_Face, NULL},
    {"libSceFreeType", "FT_Done_Face", hb_FT_Done_Face, NULL},
};

EXT_FAMILY("homebrew", k_hle, NULL);
