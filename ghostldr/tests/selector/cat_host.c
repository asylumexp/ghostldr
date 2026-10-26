/*
 * The game selector's catalogue (selector/src/cat.c, with sfo.c and meta.c)
 * built for this machine, reading a library of title folders the way the
 * core hands it over (CTX_FLAG_LIBRARY; docs/usb.md): the console's files
 * are a host directory (tests/ext_native/hostfs.c).  test_usb.py drives it:
 *
 *     cat_host FSROOT APP_ROOT [library GAME...]
 *
 * GAME is a game as the extension lists it (struct lib_title):
 * ID|APP0|DIR_LEN|FLAGS|NOTE.  Prints the titles found, their names and
 * versions, the sizes of their art, and what their meta.bin holds.
 */
#include "plat.h"

long write(int fd, const void *buf, unsigned long n);
size_t strlcpy(char *d, const char *s, size_t n);
int vsnprintf(char *buf, unsigned long size, const char *fmt, __builtin_va_list ap);
void *calloc(unsigned long n, unsigned long size);
void hfs_set_root(const char *root);
int hfs_open(const char *path);
long hfs_read(int h, void *buf, unsigned long n);
long hfs_seek(int h, long off, int whence);
int hfs_close(int h);
int hfs_stat_fbsd(const char *path, void *st);
int hfs_getdents(int h, void *buf, int size);

int vfmt(char *buf, size_t size, const char *f, __builtin_va_list ap) { return vsnprintf(buf, size, f, ap); }
int fmt(char *buf, size_t size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(buf, size, f, ap);
    __builtin_va_end(ap);
    return n;
}
static void say(const char *f, ...)
{
    char line[1200];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(line, sizeof(line) - 1, f, ap);
    __builtin_va_end(ap);
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    write(1, line, (unsigned long)n);
}
static void f_logf(const char *f, ...)
{
    char line[1000];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    vfmt(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    say("log: %s", line);
}

/* main.c's heap, over one calloc'd block */
void *sel_alloc(struct sel *S, u64 size)
{
    struct plat *P = PLAT(S);
    u64 at = (P->heap_used + 63) & ~63ull;
    if (at + size > P->heap_size)
        return NULL;
    P->heap_used = at + size;
    memset(P->heap + at, 0, size);
    return P->heap + at;
}
u64 sel_mark(struct sel *S) { return PLAT(S)->heap_used; }
void sel_release(struct sel *S, u64 mark) { PLAT(S)->heap_used = mark; }

#define HOST_FD 5000
static int v_open(const char *path, int flags, int mode)
{
    int h = hfs_open(path);
    return h < 0 ? (int)0x80020002 : HOST_FD + h;
}
static s64 v_read(int fd, void *buf, u64 n) { return hfs_read(fd - HOST_FD, buf, n); }
static s64 v_lseek(int fd, s64 off, int whence) { return hfs_seek(fd - HOST_FD, off, whence); }
static int v_close(int fd) { return hfs_close(fd - HOST_FD); }
static int v_getdents(int fd, void *buf, int size) { return hfs_getdents(fd - HOST_FD, buf, size); }

/* the extension's list (ghost_api.library), from the command line */
static struct lib_title g_lib[16];
static int g_nlib;
static int f_library(const struct lib_title **list)
{
    *list = g_lib;
    return g_nlib;
}
static void parse_game(char *arg)
{
    struct lib_title *t = &g_lib[g_nlib++];
    char *f[5] = {arg, 0, 0, 0, 0};
    for (int k = 1; k < 5; k++) {
        char *bar = f[k - 1];
        while (bar && *bar && *bar != '|')
            bar++;
        if (!bar || !*bar)
            break;
        *bar = 0;
        f[k] = bar + 1;
    }
    strlcpy(t->id, f[0], sizeof(t->id));
    strlcpy(t->path, f[1] ? f[1] : "", sizeof(t->path));
    int len = 0;
    for (const char *c = f[2]; c && *c; c++)
        len = len * 10 + (*c - '0');
    t->dir_len = (u16)len;
    t->flags = f[3] ? (u16)(f[3][0] - '0') : 0;
    strlcpy(t->note, f[4] ? f[4] : "", sizeof(t->note));
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    hfs_set_root(argv[1]);
    static u64 ctx_mem[64]; /* the boot context, with an empty profile slot (cat.c reads slot 63) */
    struct boot_ctx *ctx = (struct boot_ctx *)ctx_mem;
    ctx->size = sizeof(*ctx);
    ctx->app_root = (u64)argv[2];
    ctx->flags = argc > 3 ? CTX_FLAG_LIBRARY : 0;
    for (int i = 4; i < argc && g_nlib < 16; i++)
        parse_game(argv[i]);
    static struct ghost_api api;
    static struct kfuncs k;
    api.version = GHOST_API_VERSION, api.size = sizeof(api);
    api.logf = f_logf;
    api.vfs_open = v_open, api.vfs_read = v_read, api.vfs_lseek = v_lseek, api.vfs_close = v_close;
    api.vfs_stat = hfs_stat_fbsd, api.vfs_getdents = v_getdents;
    api.library = f_library;
    static struct plat P;
    P.api = &api, P.ctx = ctx, P.k = &k;
    P.heap_size = 32u << 20;
    P.heap = calloc(1, P.heap_size);
    struct sel S;
    memset(&S, 0, sizeof(S));
    S.plat = &P;
    int n = cat_count(&P);
    say("count %d library %d root %s", n, P.library, P.root);
    if (n <= 0 || cat_load(&S, &P, n) != 0) {
        say("load failed");
        return 0;
    }
    for (int i = 0; i < P.ncat; i++) {
        const struct cat_title *c = &P.cat[i];
        s64 sz[NFILES];
        for (int f = 0; f < NFILES; f++)
            sz[f] = cat_file(&S, &P, i, f, NULL, 0);
        u8 head[META_HEADER];
        struct meta_info mi;
        s64 m = P.library ? cat_meta_read(&P, i, 0, head, sizeof(head)) : -1; /* as main.c's meta_head asks */
        int meta = m == META_HEADER && meta_parse(head, (u64)m, &mi) == 0;
        say("title\t%s\t%s\t%s\t%u\t%ld\t%ld\t%ld\t%ld\t%s\t%s\t%s", c->id, c->name, c->version, c->name_src,
            sz[FILE_ICON], sz[FILE_PIC], sz[FILE_SFO], sz[FILE_PIC0], meta ? mi.name : "-",
            c->flags & CAT_BAD ? "bad" : "ok", c->note[0] ? c->note : "-");
        u8 sfo[64];
        if (sz[FILE_SFO] > 0 && cat_file(&S, &P, i, FILE_SFO, sfo, sizeof(sfo)) != (sz[FILE_SFO] < 64 ? sz[FILE_SFO] : 64))
            say("FAILED reading %s's param.sfo", c->id);
    }
    return 0;
}
