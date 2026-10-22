/*
 * stdio for titles without a libc of their own (homebrew that links the
 * system's libSceLibcInternal).  That library is SWRR's, and so are its
 * file calls: they neither see ghostfs nor the title's paths.
 *
 *  - stdout and stderr: SWRR's printf writes SWRR's stdout, which nothing
 *    reads.  printf, puts and the rest come here, are formatted by SWRR's
 *    vsnprintf and go to the log line by line ("ghost1:"/"ghost2:", the
 *    file family's write), as the title's own libc would write them;
 *  - title files: fopen of a ghostfs file gives a FILE of this family's
 *    (read-only; fread, fgets, fgetc, fseek, ftell ...), opendir of a
 *    ghostfs directory a DIR of its own.  Other paths are remapped (/app0 on
 *    a dev setup, the title's /savedataN) and opened by SWRR's libc;
 *  - every other FILE and DIR is SWRR's.
 *
 * Like the heap (hle_libc_heap.c), the family turns itself off when a title
 * module exports malloc, i.e. brings its own libc: that one writes through
 * the title's own _write, which the core already sends to the log.
 */
#include "ext.h"

#define LC "libSceLibcInternal"
#define LINE 2048

typedef __builtin_va_list va_list;
#define va_start __builtin_va_start
#define va_end __builtin_va_end

static void *volatile g_fn[8];
enum { F_VSNPRINTF, F_VFPRINTF, F_FWRITE, F_FPUTS, F_FPUTC, F_STRERROR, F_STDOUT, F_STDERR };
static const char *const k_names[] = {"vsnprintf", "vfprintf", "fwrite", "fputs", "fputc", "strerror"};
/* stdout's and stderr's FILE as SWRR's libc names it: a FILE * variable
 * (stdout, __stdoutp) or the FILE itself (_Stdout) */
static const char *const k_streams[2][3] = {{"stdout", "__stdoutp", "_Stdout"}, {"stderr", "__stderrp", "_Stderr"}};
static void *g_stream[2][3];
static volatile int g_ready;

static void *host_fn(u32 i)
{
    void *f = g_fn[i];
    if (!f) {
        f = host_real_in(LC, k_names[i]);
        g_fn[i] = f ? f : (void *)1;
    }
    return f == (void *)1 ? NULL : f;
}

static void find_streams(void)
{
    if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE))
        return;
    for (u32 s = 0; s < 2; s++)
        for (u32 k = 0; k < 3; k++) {
            void *a = host_real_in(LC, k_streams[s][k]);
            g_stream[s][k] = !a ? NULL : k < 2 ? *(void **)a : a;
        }
    __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
}

/* 1 for stdout, 2 for stderr, 0 for another FILE */
static int stream_fd(const void *f)
{
    find_streams();
    for (u32 s = 0; s < 2; s++)
        for (u32 k = 0; f && k < 3; k++)
            if (g_stream[s][k] == f)
                return (int)s + 1;
    return 0;
}

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

static int log_vprintf(int fd, const char *fmt, va_list ap)
{
    int (*vsn)(char *, u64, const char *, va_list) = host_fn(F_VSNPRINTF);
    char line[LINE];
    int n = vsn ? vsn(line, sizeof(line), fmt, ap) : 0;
    if (n > 0)
        log_write(fd, line, (u64)n < sizeof(line) ? (u64)n : sizeof(line) - 1);
    return n;
}

static int s_vprintf(const char *fmt, va_list ap) { return log_vprintf(1, fmt, ap); }

static int s_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = log_vprintf(1, fmt, ap);
    va_end(ap);
    return n;
}

static int write_ours(void *f);

static int s_vfprintf(void *f, const char *fmt, va_list ap)
{
    if (write_ours(f))
        return -1;
    int fd = stream_fd(f);
    if (fd)
        return log_vprintf(fd, fmt, ap);
    int (*real)(void *, const char *, va_list) = host_fn(F_VFPRINTF);
    return real ? real(f, fmt, ap) : -1;
}

static int s_fprintf(void *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = s_vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

static int s_puts(const char *s)
{
    log_write(1, s, strlen(s));
    log_write(1, "\n", 1);
    return 1;
}

static int s_putchar(int c)
{
    char ch = (char)c;
    log_write(1, &ch, 1);
    return c & 0xFF;
}

static int s_fputs(const char *s, void *f)
{
    if (write_ours(f))
        return -1;
    int fd = stream_fd(f);
    if (fd)
        return log_write(fd, s, strlen(s)) >= 0 ? 1 : -1;
    int (*real)(const char *, void *) = host_fn(F_FPUTS);
    return real ? real(s, f) : -1;
}

static int s_fputc(int c, void *f)
{
    if (write_ours(f))
        return -1;
    int fd = stream_fd(f);
    if (fd) {
        char ch = (char)c;
        log_write(fd, &ch, 1);
        return c & 0xFF;
    }
    int (*real)(int, void *) = host_fn(F_FPUTC);
    return real ? real(c, f) : -1;
}

static u64 s_fwrite(const void *p, u64 size, u64 n, void *f)
{
    if (write_ours(f))
        return 0;
    int fd = stream_fd(f);
    if (fd) {
        if (size && n)
            log_write(fd, p, size * n);
        return n;
    }
    u64 (*real)(const void *, u64, u64, void *) = host_fn(F_FWRITE);
    return real ? real(p, size, n, f) : 0;
}

static void s_perror(const char *s)
{
    const char *(*se)(int) = host_fn(F_STRERROR);
    int e = K.errno_fn ? *K.errno_fn() : 0;
    const char *msg = se ? se(e) : "error";
    if (s && *s) {
        log_write(2, s, strlen(s));
        log_write(2, ": ", 2);
    }
    log_write(2, msg, strlen(msg));
    log_write(2, "\n", 1);
}

/* ---- title files ---- */
#define EBADF 9
#define EINVAL 22
#define EMFILE 24
#define EROFS 30
#define FILE_MAGIC 0x454c4946u /* "FILE" */
#define DIR_MAGIC 0x52494447u  /* "GDIR" */
#define TBUF 0x4000

struct tfile {
    u32 magic;
    int fd, eof, err, unget;
    s64 off, size;    /* the position, the file's size */
    s64 bstart, blen; /* what buf holds of the file */
    u8 buf[TBUF];
};

struct tdir {
    u32 magic;
    int fd;
    int pos, len;
    u8 buf[0x2000];
};

static void *g_open[64]; /* this family's FILEs and DIRs */
static volatile int g_open_lock;

static void set_errno(int e)
{
    if (K.errno_fn)
        *K.errno_fn() = e;
}

static bool track(void *p, bool add)
{
    bool done = false;
    spin_lock(&g_open_lock);
    for (u32 i = 0; i < ARRAY_SIZE(g_open) && !done; i++)
        if (add ? !g_open[i] : g_open[i] == p)
            g_open[i] = add ? p : NULL, done = true;
    spin_unlock(&g_open_lock);
    return done;
}

static void *ours(const void *p, u32 magic)
{
    bool found = false;
    spin_lock(&g_open_lock);
    for (u32 i = 0; p && i < ARRAY_SIZE(g_open) && !found; i++)
        found = g_open[i] == p;
    spin_unlock(&g_open_lock);
    return found && *(const u32 *)p == magic ? (void *)p : NULL;
}

static struct tfile *tf(const void *f) { return ours(f, FILE_MAGIC); }

static void *libc_fn(const char *name)
{
    return host_real_in(LC, name);
}
#define LIBC(type, name)            \
    static __typeof__(type) real_;  \
    if (!real_)                     \
        real_ = (type)libc_fn(name)

static void *s_fopen(const char *path, const char *mode)
{
    LIBC(void *(*)(const char *, const char *), "fopen");
    int fd;
    if (!path || !mode || gfs_open(path, 0, &fd) != 0) {
        char buf[512];
        const char *p = path ? remap_path(path, buf, sizeof(buf)) : path;
        return real_ ? real_(p, mode) : NULL;
    }
    if (fd < 0) {
        set_errno(fd & 0xFFFF);
        return NULL;
    }
    if (strstr(mode, "w") || strstr(mode, "a") || strstr(mode, "+")) {
        gfs_close(fd);
        set_errno(EROFS);
        return NULL;
    }
    struct tfile *t = page_alloc(sizeof(*t));
    if (!t || !track(t, true)) {
        if (t)
            page_free(t, sizeof(*t));
        gfs_close(fd);
        set_errno(EMFILE);
        return NULL;
    }
    t->magic = FILE_MAGIC, t->fd = fd, t->unget = -1, t->size = gfs_lseek(fd, 0, 2);
    static u32 logged;
    if (logged++ < 32)
        logf("libc fopen %s: a title file (%ld bytes)", path, t->size);
    return t;
}

static int s_fclose(void *f)
{
    struct tfile *t = tf(f);
    if (!t) {
        LIBC(int (*)(void *), "fclose");
        return real_ ? real_(f) : -1;
    }
    track(t, false);
    gfs_close(t->fd);
    t->magic = 0;
    page_free(t, sizeof(*t));
    return 0;
}

/* up to n bytes at the position; 0 at the end */
static s64 tf_read(struct tfile *t, u8 *dst, u64 n)
{
    u64 got = 0;
    if (t->unget >= 0 && n) {
        dst[got++] = (u8)t->unget;
        t->unget = -1;
        t->off++;
    }
    while (got < n) {
        if (t->off >= t->bstart && t->off < t->bstart + t->blen) {
            u64 k = (u64)(t->bstart + t->blen - t->off);
            k = k < n - got ? k : n - got;
            memcpy(dst + got, t->buf + (t->off - t->bstart), k);
            got += k, t->off += (s64)k;
            continue;
        }
        if (n - got >= TBUF) { /* large reads straight through */
            s64 r = gfs_pread(t->fd, dst + got, n - got, t->off);
            if (r <= 0) {
                t->err |= r < 0, t->eof |= r == 0;
                break;
            }
            got += (u64)r, t->off += r;
            continue;
        }
        s64 r = gfs_pread(t->fd, t->buf, TBUF, t->off);
        if (r <= 0) {
            t->err |= r < 0, t->eof |= r == 0;
            break;
        }
        t->bstart = t->off, t->blen = r;
    }
    return (s64)got;
}

static u64 s_fread(void *p, u64 size, u64 n, void *f)
{
    struct tfile *t = tf(f);
    if (!t) {
        LIBC(u64 (*)(void *, u64, u64, void *), "fread");
        return real_ ? real_(p, size, n, f) : 0;
    }
    if (!size || !n)
        return 0;
    s64 got = tf_read(t, p, size * n);
    return (u64)got / size;
}

static int s_fgetc(void *f)
{
    struct tfile *t = tf(f);
    if (!t) {
        LIBC(int (*)(void *), "fgetc");
        return real_ ? real_(f) : -1;
    }
    u8 c;
    return tf_read(t, &c, 1) == 1 ? c : -1;
}

static int s_getc(void *f)
{
    if (!tf(f)) {
        LIBC(int (*)(void *), "getc");
        return real_ ? real_(f) : -1;
    }
    return s_fgetc(f);
}

static int s_ungetc(int c, void *f)
{
    struct tfile *t = tf(f);
    if (!t) {
        LIBC(int (*)(int, void *), "ungetc");
        return real_ ? real_(c, f) : -1;
    }
    if (c < 0 || t->unget >= 0 || !t->off)
        return -1;
    t->unget = c & 0xFF, t->off--, t->eof = 0;
    return c & 0xFF;
}

static char *s_fgets(char *s, int n, void *f)
{
    struct tfile *t = tf(f);
    if (!t) {
        LIBC(char *(*)(char *, int, void *), "fgets");
        return real_ ? real_(s, n, f) : NULL;
    }
    int i = 0;
    while (i + 1 < n) {
        u8 c;
        if (tf_read(t, &c, 1) != 1)
            break;
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (!i || n <= 0)
        return NULL;
    s[i] = 0;
    return s;
}

static int s_fseeko(void *f, s64 off, int whence)
{
    struct tfile *t = tf(f);
    if (!t) {
        LIBC(int (*)(void *, s64, int), "fseeko");
        return real_ ? real_(f, off, whence) : -1;
    }
    s64 base = whence == 0 ? 0 : whence == 1 ? t->off : whence == 2 ? t->size : -1;
    if (base < 0 || base + off < 0) {
        set_errno(EINVAL);
        return -1;
    }
    t->off = base + off, t->eof = 0, t->unget = -1;
    return 0;
}

static int s_fseek(void *f, s64 off, int whence)
{
    if (!tf(f)) {
        LIBC(int (*)(void *, s64, int), "fseek");
        return real_ ? real_(f, off, whence) : -1;
    }
    return s_fseeko(f, off, whence);
}

static s64 s_ftello(void *f)
{
    struct tfile *t = tf(f);
    if (!t) {
        LIBC(s64 (*)(void *), "ftello");
        return real_ ? real_(f) : -1;
    }
    return t->off;
}

static s64 s_ftell(void *f)
{
    if (!tf(f)) {
        LIBC(s64 (*)(void *), "ftell");
        return real_ ? real_(f) : -1;
    }
    return s_ftello(f);
}

static void s_rewind(void *f)
{
    struct tfile *t = tf(f);
    if (!t) {
        LIBC(void (*)(void *), "rewind");
        if (real_)
            real_(f);
        return;
    }
    s_fseeko(f, 0, 0);
    t->err = 0;
}

#define FLAG_CALL(fn, name, expr)                     \
    static int fn(void *f)                            \
    {                                                 \
        struct tfile *t = tf(f);                      \
        if (!t) {                                     \
            LIBC(int (*)(void *), name);              \
            return real_ ? real_(f) : -1;             \
        }                                             \
        return expr;                                  \
    }
FLAG_CALL(s_feof, "feof", t->eof)
FLAG_CALL(s_ferror, "ferror", t->err)
FLAG_CALL(s_fileno, "fileno", t->fd)
FLAG_CALL(s_fflush, "fflush", 0)

static void s_clearerr(void *f)
{
    struct tfile *t = tf(f);
    if (!t) {
        LIBC(void (*)(void *), "clearerr");
        if (real_)
            real_(f);
        return;
    }
    t->eof = t->err = 0;
}

static int s_setvbuf(void *f, char *buf, int mode, u64 size)
{
    if (!tf(f)) {
        LIBC(int (*)(void *, char *, int, u64), "setvbuf");
        return real_ ? real_(f, buf, mode, size) : -1;
    }
    return 0;
}

/* writing one of this family's FILEs: they are read-only */
static int write_ours(void *f)
{
    struct tfile *t = tf(f);
    if (!t)
        return 0;
    t->err = 1;
    set_errno(EBADF);
    return 1;
}

static void *s_opendir(const char *path)
{
    LIBC(void *(*)(const char *), "opendir");
    int fd;
    if (!path || gfs_open(path, 0, &fd) != 0) {
        char buf[512];
        const char *p = path ? remap_path(path, buf, sizeof(buf)) : path;
        return real_ ? real_(p) : NULL;
    }
    if (fd < 0) {
        set_errno(fd & 0xFFFF);
        return NULL;
    }
    struct tdir *d = page_alloc(sizeof(*d));
    if (!d || !track(d, true)) {
        if (d)
            page_free(d, sizeof(*d));
        gfs_close(fd);
        set_errno(EMFILE);
        return NULL;
    }
    d->magic = DIR_MAGIC, d->fd = fd;
    return d;
}

/* FreeBSD dirents as gfs_getdents writes them: u32 ino, u16 reclen, u8 type,
 * u8 namlen, name */
static void *s_readdir(void *dir)
{
    struct tdir *d = ours(dir, DIR_MAGIC);
    if (!d) {
        LIBC(void *(*)(void *), "readdir");
        return real_ ? real_(dir) : NULL;
    }
    if (d->pos >= d->len) {
        int n = gfs_getdents(d->fd, d->buf, sizeof(d->buf));
        if (n <= 0)
            return NULL;
        d->pos = 0, d->len = n;
    }
    u8 *e = d->buf + d->pos;
    u16 reclen = *(u16 *)(e + 4);
    d->pos = reclen ? d->pos + reclen : d->len;
    return e;
}

static int s_closedir(void *dir)
{
    struct tdir *d = ours(dir, DIR_MAGIC);
    if (!d) {
        LIBC(int (*)(void *), "closedir");
        return real_ ? real_(dir) : -1;
    }
    track(d, false);
    gfs_close(d->fd);
    d->magic = 0;
    page_free(d, sizeof(*d));
    return 0;
}

static void s_rewinddir(void *dir)
{
    struct tdir *d = ours(dir, DIR_MAGIC);
    if (!d) {
        LIBC(void (*)(void *), "rewinddir");
        if (real_)
            real_(dir);
        return;
    }
    gfs_lseek(d->fd, 0, 0);
    d->pos = d->len = 0;
}

/* on only when no title module defines malloc (no libc of its own) */
static int stdio_init(void)
{
    char nid[16];
    compute_nid("malloc", nid);
    size_t nl = strlen(nid);
    for (struct module *m = g_modules; m; m = m->next)
        for (u64 i = 0; m->symtab && i < m->nsyms; i++) {
            const struct elf_sym *s = &m->symtab[i];
            const char *name = s->name < m->strsz ? m->strtab + s->name : "";
            if (s->shndx && strncmp(name, nid, nl) == 0 && (name[nl] == '#' || !name[nl]))
                return 1;
        }
    return 0;
}

static const struct ext_hle_ent k_hle[] = {
    {LC, "printf", s_printf, NULL},   {LC, "vprintf", s_vprintf, NULL}, {LC, "puts", s_puts, NULL},
    {LC, "putchar", s_putchar, NULL}, {LC, "fprintf", s_fprintf, NULL}, {LC, "vfprintf", s_vfprintf, NULL},
    {LC, "fputs", s_fputs, NULL},     {LC, "fputc", s_fputc, NULL},     {LC, "putc", s_fputc, NULL},
    {LC, "fwrite", s_fwrite, NULL},   {LC, "perror", s_perror, NULL},
    {LC, "fopen", s_fopen, NULL},     {LC, "fclose", s_fclose, NULL},   {LC, "fread", s_fread, NULL},
    {LC, "fgets", s_fgets, NULL},     {LC, "fgetc", s_fgetc, NULL},     {LC, "getc", s_getc, NULL},
    {LC, "ungetc", s_ungetc, NULL},   {LC, "fseek", s_fseek, NULL},     {LC, "fseeko", s_fseeko, NULL},
    {LC, "ftell", s_ftell, NULL},     {LC, "ftello", s_ftello, NULL},   {LC, "rewind", s_rewind, NULL},
    {LC, "feof", s_feof, NULL},       {LC, "ferror", s_ferror, NULL},   {LC, "clearerr", s_clearerr, NULL},
    {LC, "fileno", s_fileno, NULL},   {LC, "fflush", s_fflush, NULL},   {LC, "setvbuf", s_setvbuf, NULL},
    {LC, "opendir", s_opendir, NULL}, {LC, "readdir", s_readdir, NULL}, {LC, "closedir", s_closedir, NULL},
    {LC, "rewinddir", s_rewinddir, NULL},
};

EXT_FAMILY("libc_stdio", k_hle, stdio_init);
