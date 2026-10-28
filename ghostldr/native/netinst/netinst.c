/*
 * The network installer's server (netinst.h, docs/manager.md "Install a
 * game"): the install page and the /api/install/ requests manager.lua
 * answers too, one request at a time on a thread of its own.
 *
 * The page lays the game out; this stores bytes:
 *  - read, probe: files of a container (the catalogue, index headers,
 *    gfs.tag), never SWRR's own save or a title's progress;
 *  - put, write, delete: whole small files (index, catalogue, tags) and
 *    containers, each request only for the kinds of container it may touch
 *    (kind());
 *  - data: a data container's data.bin in one or more requests.  The first
 *    (off=0) creates it; it stays mounted and open between requests, so a
 *    dropped connection carries on from what arrived (info's session); the
 *    last commits it and checks its size.  The receiving thread fills four
 *    4 MiB buffers while a second thread writes them, as the installer's
 *    pump does: a container goes in at the slower of the network and the
 *    save writes (about 25 MB/s on a PS5), not their sum.
 * Writes need the token shown on the TV (X-Ghostldr-Token), with a growing
 * wait after wrong ones, as in manager.lua.
 *
 * No globals and no relocations: it links into the selector module, whose
 * image is read-only and wiped when the selector returns.
 */
#include "netinst.h"

typedef __SIZE_TYPE__ size_t;
#define NULL ((void *)0)
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strstr(const char *h, const char *n);
size_t strlcpy(char *d, const char *s, size_t n);
int fmt(char *buf, size_t size, const char *f, ...) __attribute__((format(printf, 3, 4)));
int vfmt(char *buf, size_t size, const char *f, __builtin_va_list ap);

#define E (s->e)
static void LOG(struct netinst *s, const char *f, ...) __attribute__((format(printf, 2, 3)));

#define HEAD_MAX 16384
#define CAP (992ul << 20) /* gfs_pack.py: data bytes per container */
#define MAX_BLOCKS 32768ul
#define MIN_BLOCKS 96ul
#define NOT_FOUND 0x809F0008u
#define NO_SPACE 0x809F000Au
#define TOKEN_CHARS "ACDEFGHJKLMNPQRTUVWXY34679"

struct req {
    char method[8];
    char path[64];
    char query[1024];
    char token[40];
    long length; /* Content-Length, -1 without */
    const unsigned char *pre; /* body bytes that came with the head */
    unsigned long pre_len;
};

struct netinst {
    struct ns_env *e;
    unsigned long thread, wthread; /* the server's and the writer's (joined) */
    int lfd;
    unsigned short port;
    volatile int stop, exited, client;
    volatile int lock;
    struct netinst_status st;
    char token[12];
    unsigned fails;
    long wait_until;
    long last_request; /* clock_ms of the last request: BUSY ends after a quiet minute */
    /* the data container being written */
    struct {
        int open, fd;
        char name[32], mp[16], lid[40];
        unsigned long size, blocks, written;
        long t0;
    } se;
    /* the receive/write ring */
    unsigned char *buf[NETINST_BUFS];
    unsigned long len[NETINST_BUFS];
    volatile unsigned full[NETINST_BUFS];
    volatile unsigned long wcount;
    volatile int wstop, wdone;
    long wstatus;
    unsigned long wwritten;
    int wfd;
    /* speed */
    long rate_t;
    unsigned long rate_b;
    char head[HEAD_MAX + 1];
    char out[2048];
};


/* ---- small helpers ---- */

static unsigned ld(volatile unsigned *v) { return __atomic_load_n(v, __ATOMIC_ACQUIRE); }
static void st_(volatile unsigned *v, unsigned x) { __atomic_store_n(v, x, __ATOMIC_RELEASE); }
static void lock(struct netinst *s)
{
    while (__atomic_exchange_n(&s->lock, 1, __ATOMIC_ACQUIRE))
        E->sleep_ms(E, 0);
}
static void unlock(struct netinst *s) { __atomic_store_n(&s->lock, 0, __ATOMIC_RELEASE); }

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int prefix_ci(const char *s, const char *p)
{
    for (; *p; s++, p++)
        if (lower(*s) != lower(*p))
            return 0;
    return 1;
}
static unsigned long num(const char *s, int *ok)
{
    unsigned long v = 0;
    int any = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (unsigned long)(*s++ - '0'), any = 1;
    if (ok)
        *ok = any && !*s;
    return v;
}
static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 87 : c >= 'A' && c <= 'F' ? c - 55 : -1; }

/* the query parameter `key`, decoded, into out: 1 when present */
static int param(const struct req *r, const char *key, char *out, unsigned size)
{
    unsigned kl = (unsigned)strlen(key);
    const char *q = r->query;
    while (*q) {
        const char *end = q;
        while (*end && *end != '&')
            end++;
        if (!strncmp(q, key, kl) && q[kl] == '=') {
            const char *v = q + kl + 1;
            unsigned n = 0;
            while (v < end && n + 1 < size) {
                int c = *v++;
                if (c == '+')
                    c = ' ';
                else if (c == '%' && end - v >= 2 && hexval(v[0]) >= 0 && hexval(v[1]) >= 0)
                    c = hexval(v[0]) * 16 + hexval(v[1]), v += 2;
                out[n++] = (char)c;
            }
            out[n] = 0;
            if (v < end) { /* too long: not taken, so no name is ever cut into another */
                out[0] = 0;
                return 0;
            }
            return 1;
        }
        q = *end ? end + 1 : end;
    }
    if (size)
        out[0] = 0;
    return 0;
}
static int param_num(const struct req *r, const char *key, unsigned long *v)
{
    char b[24];
    int ok = 0;
    if (!param(r, key, b, sizeof(b)))
        return 0;
    *v = num(b, &ok);
    return ok;
}

static void LOG(struct netinst *s, const char *f, ...)
{
    char line[200];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = fmt(line, sizeof(line), "netinst: ");
    vfmt(line + n, sizeof(line) - (unsigned)n, f, ap);
    __builtin_va_end(ap);
    E->log(E, line);
}

/* logged, and the TV's message line */
static void say(struct netinst *s, const char *f, ...) __attribute__((format(printf, 2, 3)));
static void say(struct netinst *s, const char *f, ...)
{
    char line[128];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    vfmt(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    LOG(s, "%s", line);
    lock(s);
    strlcpy(s->st.message, line, sizeof(s->st.message));
    unlock(s);
}

/* ---- names: what the installer may touch (manager.lua M.install_kind) ---- */
enum { K_NONE, K_CATALOG, K_INDEX, K_FREE, K_META, K_DATA };
static int all_alnum(const char *p, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (!((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9')))
            return 0;
    return 1;
}
static int kind(const char *n)
{
    unsigned l = (unsigned)strlen(n);
    if (l < 1 || l > 31 || !((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9')))
        return K_NONE;
    for (unsigned i = 1; i < l; i++)
        if (!((n[i] >= 'A' && n[i] <= 'Z') || (n[i] >= '0' && n[i] <= '9') || n[i] == '_' || n[i] == '-'))
            return K_NONE;
    if (!strcmp(n, "SLUS-20268") || !strcmp(n, "SLES-50366") || !strcmp(n, "SCUS-97429") ||
        (l >= 4 && !strcmp(n + l - 4, "SAVE")))
        return K_NONE;
    if (!strcmp(n, "GFSCATALOG"))
        return K_CATALOG;
    if (!strcmp(n, "GFSINDEX") || !strcmp(n, "GHOSTIDX"))
        return K_INDEX;
    if (l == 9 && !strncmp(n, "GFSFREE", 7) && all_alnum(n + 7, 2) && n[7] <= '9' && n[8] <= '9')
        return K_FREE;
    if (l > 5 && l - 5 <= 12 && !strcmp(n + l - 5, "INDEX") && all_alnum(n, l - 5))
        return K_INDEX;
    if (l > 4 && l - 4 <= 12 && !strcmp(n + l - 4, "META") && all_alnum(n, l - 4))
        return K_META;
    if (l > 3 && l - 3 <= 12 && all_alnum(n, l) && n[l - 1] <= '9' && n[l - 2] <= '9' && n[l - 3] <= '9' &&
        n[l - 1] >= '0' && n[l - 2] >= '0' && n[l - 3] >= '0')
        return K_DATA;
    return K_NONE;
}
#define KB(k) (1u << (k))
static const unsigned OP_PUT = KB(K_INDEX) | KB(K_META) | KB(K_CATALOG);
static const unsigned OP_WRITE = KB(K_INDEX) | KB(K_META) | KB(K_CATALOG) | KB(K_DATA);
static const unsigned OP_DELETE = KB(K_INDEX) | KB(K_META) | KB(K_DATA) | KB(K_FREE);
static const unsigned OP_DATA = KB(K_DATA);
static const unsigned OP_READ = ~0u & ~KB(K_NONE);

static int file_ok(const char *f)
{
    unsigned l = (unsigned)strlen(f);
    if (l < 1 || l > 31 || f[0] == '.')
        return 0;
    for (unsigned i = 0; i < l; i++)
        if (!((f[i] >= 'A' && f[i] <= 'Z') || (f[i] >= 'a' && f[i] <= 'z') || (f[i] >= '0' && f[i] <= '9') ||
              f[i] == '.' || f[i] == '_' || f[i] == '-'))
            return 0;
    return 1;
}

/* an environment error (-errno) as the SCE kernel error the page shows */
static unsigned sce_of(long e) { return e < 0 ? 0x80020000u | (unsigned)(-e & 0xFFFF) : (unsigned)e; }

/* ---- HTTP out ---- */

static const char *reason(int code)
{
    switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    default: return "Error";
    }
}

static void reply(struct netinst *s, int c, int code, const char *ctype, const void *body, unsigned long n, const char *extra)
{
    char h[768];
    int k = fmt(h, sizeof(h),
                "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n"
                "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n%s\r\n",
                code, reason(code), ctype, n, extra ? extra : "");
    if (E->send(E, c, h, (unsigned long)k) == k && n)
        E->send(E, c, body, n);
}

static void reply_json(struct netinst *s, int c, int code, const char *f, ...) __attribute__((format(printf, 4, 5)));
static void reply_json(struct netinst *s, int c, int code, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(s->out, sizeof(s->out), f, ap);
    __builtin_va_end(ap);
    if (n >= (int)sizeof(s->out))
        n = (int)sizeof(s->out) - 1;
    reply(s, c, code, "application/json", s->out, (unsigned long)n, NULL);
}

/* {"ok":true,"rc":N[,"error":"0x..."]}, as manager.lua's reply_rc */
static void reply_rc(struct netinst *s, int c, unsigned rc, const char *extra)
{
    if (rc)
        reply_json(s, c, 200, "{\"ok\":true,\"rc\":%u,\"error\":\"0x%08x\"%s}", rc, rc, extra ? extra : "");
    else
        reply_json(s, c, 200, "{\"ok\":true,\"rc\":0%s}", extra ? extra : "");
}

static void reply_err(struct netinst *s, int c, int code, const char *msg)
{
    LOG(s, "%d %s", code, msg);
    reply_json(s, c, code, "{\"ok\":false,\"error\":\"%s\"}", msg);
}

/* ---- HTTP in ---- */

/* the head; body bytes that came with it stay in s->head after it */
static int read_head(struct netinst *s, int c, struct req *r)
{
    unsigned long got = 0;
    char *end = NULL;
    while (!end) {
        if (got >= HEAD_MAX)
            return -431;
        long n = E->recv(E, c, s->head + got, HEAD_MAX - got);
        if (n <= 0)
            return -1;
        got += (unsigned long)n;
        s->head[got] = 0;
        end = strstr(s->head, "\r\n\r\n");
    }
    memset(r, 0, sizeof(*r));
    r->length = -1;
    r->pre = (const unsigned char *)end + 4;
    r->pre_len = got - (unsigned long)(end + 4 - s->head);
    *end = 0;
    /* METHOD TARGET HTTP/1.x */
    char *p = s->head, *sp = p;
    while (*sp && *sp != ' ')
        sp++;
    if (*sp != ' ' || sp - p >= (long)sizeof(r->method))
        return -2;
    memcpy(r->method, p, (unsigned long)(sp - p));
    char *t = sp + 1, *te = t;
    while (*te && *te != ' ' && *te != '\r')
        te++;
    char *qm = t;
    while (qm < te && *qm != '?')
        qm++;
    if (qm - t >= (long)sizeof(r->path) || te - qm >= (long)sizeof(r->query))
        return -2;
    memcpy(r->path, t, (unsigned long)(qm - t));
    if (qm < te)
        memcpy(r->query, qm + 1, (unsigned long)(te - qm - 1));
    for (char *l = strstr(te, "\r\n"); l; l = strstr(l, "\r\n")) {
        l += 2;
        if (prefix_ci(l, "content-length:")) {
            const char *v = l + 15;
            while (*v == ' ')
                v++;
            int ok = 0;
            char b[24];
            unsigned k = 0;
            while (v[k] >= '0' && v[k] <= '9' && k < sizeof(b) - 1)
                b[k] = v[k], k++;
            b[k] = 0;
            r->length = (long)num(b, &ok);
            if (!ok)
                r->length = -1;
        } else if (prefix_ci(l, "x-ghostldr-token:")) {
            const char *v = l + 17;
            unsigned k = 0;
            while (*v && *v != '\r' && k < sizeof(r->token) - 1) {
                int ch = *v++;
                if (ch >= 'a' && ch <= 'z')
                    ch -= 32;
                if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
                    r->token[k++] = (char)ch;
            }
        }
    }
    return 0;
}

/* exactly n bytes, or fewer when the connection fails, times out or the server stops */
static unsigned long recv_exact(struct netinst *s, int c, unsigned char *p, unsigned long n)
{
    unsigned long got = 0;
    while (got < n && !s->stop) {
        long r = E->recv(E, c, p + got, n - got);
        if (r <= 0)
            break;
        got += (unsigned long)r;
    }
    return got;
}

/* the body, whole, into buf[0] (at most NETINST_BUF): its length, or -1 */
static long body(struct netinst *s, int c, const struct req *r)
{
    if (r->length < 0 || (unsigned long)r->length > NETINST_BUF)
        return -1;
    unsigned long n = (unsigned long)r->length, pre = r->pre_len < n ? r->pre_len : n;
    memcpy(s->buf[0], r->pre, pre);
    if (pre < n && recv_exact(s, c, s->buf[0] + pre, n - pre) != n - pre)
        return -1;
    return (long)n;
}

/* ---- save containers ---- */

static void join(char *out, unsigned size, const char *mp, const char *f) { fmt(out, size, "%s/%s", mp, f); }

/* up to n bytes of f from off into buf[0]: 0, a mount error, or 0xFFFFFFFF (no such file); *got */
static unsigned read_file(struct netinst *s, const char *name, const char *f, unsigned long off, unsigned long n,
                          unsigned long *got)
{
    char mp[16], path[64];
    *got = 0;
    int rc = E->mount(E, name, 1, 0, mp, NULL);
    if (rc)
        return (unsigned)rc;
    join(path, sizeof(path), mp, f);
    unsigned st = 0xFFFFFFFFu;
    int fd = E->open(E, path, NS_O_RDONLY, 0);
    if (fd >= 0) {
        if (n > NETINST_BUF)
            n = NETINST_BUF;
        long k = E->seek(E, fd, (long)off) < 0 ? -1 : E->read(E, fd, s->buf[0], n);
        if (k >= 0)
            st = 0, *got = (unsigned long)k;
        E->close(E, fd);
    }
    E->umount(E, mp);
    return st;
}

/* the session's data container: closed and committed (what it holds stays, untagged) */
static unsigned se_close(struct netinst *s)
{
    if (!s->se.open)
        return 0;
    s->se.open = 0;
    if (s->se.fd >= 0)
        E->close(E, s->se.fd);
    s->se.fd = -1;
    return (unsigned)E->umount(E, s->se.mp);
}

static unsigned file_size(struct netinst *s, const char *name, const char *f, unsigned long *size)
{
    char mp[16], path[64];
    int rc = E->mount(E, name, 1, 0, mp, NULL);
    if (rc)
        return (unsigned)rc;
    join(path, sizeof(path), mp, f);
    int k = E->size(E, path, size);
    E->umount(E, mp);
    return k < 0 ? 0xFFFFFFFFu : 0;
}

/* ---- the requests ---- */

static int check(struct netinst *s, int c, const char *name, unsigned ops)
{
    int k = kind(name);
    if (k == K_NONE || !(ops & KB(k))) {
        char m[96];
        fmt(m, sizeof(m), "%.40s: not a container the installer may touch", name);
        reply_err(s, c, 403, m);
        return 0;
    }
    return 1;
}

static void h_info(struct netinst *s, int c)
{
    char se[160] = "";
    if (s->se.open)
        fmt(se, sizeof(se), ",\"session\":{\"c\":\"%s\",\"size\":%lu,\"written\":%lu,\"lid\":\"%s\"}", s->se.name,
            s->se.size, s->se.written, s->se.lid);
    reply_json(s, c, 200,
               "{\"ok\":true,\"server\":\"ghostldr\",\"version\":1,\"swrr\":\"%s\",\"platform\":\"%s\",\"token\":true,"
               "\"pump\":true,\"max_write\":%u%s}",
               E->swrr, E->platform, NETINST_BUF, se);
}

static void h_read(struct netinst *s, int c, const struct req *r)
{
    char name[32], f[32];
    unsigned long off = 0, n = 0, got = 0;
    param(r, "c", name, sizeof(name));
    if (!check(s, c, name, OP_READ))
        return;
    param(r, "f", f, sizeof(f));
    if (!file_ok(f))
        return reply_err(s, c, 400, "bad file name");
    param_num(r, "off", &off);
    param_num(r, "len", &n);
    unsigned st = read_file(s, name, f, off, n, &got);
    char extra[48];
    fmt(extra, sizeof(extra), "X-Gfs-Status: %u\r\n", st);
    reply(s, c, 200, "application/octet-stream", s->buf[0], st ? 0 : got, extra);
}

static void h_probe(struct netinst *s, int c, const struct req *r)
{
    char list[1024], *p = list;
    param(r, "c", list, sizeof(list));
    char *o = s->out;
    unsigned room = sizeof(s->out), k = 0;
    int w = fmt(o, room, "{\"ok\":true,\"containers\":[");
    while (*p) {
        char name[32];
        unsigned n = 0;
        while (*p && *p != ',' && n < sizeof(name) - 1)
            name[n++] = *p++;
        name[n] = 0;
        while (*p && *p != ',')
            p++;
        if (*p == ',')
            p++;
        if (k >= 16)
            return reply_err(s, c, 400, "at most 16 containers");
        if (!check(s, c, name, OP_READ))
            return;
        unsigned long got = 0;
        unsigned st = read_file(s, name, "gfs.tag", 0, 16, &got);
        char tag[40] = "null";
        if (!st) {
            tag[0] = '"';
            for (unsigned i = 0; i < got && i < 16; i++)
                fmt(tag + 1 + 2 * i, 3, "%02x", s->buf[0][i]);
            tag[1 + 2 * (got < 16 ? got : 16)] = '"';
            tag[2 + 2 * (got < 16 ? got : 16)] = 0;
        }
        w += fmt(o + w, room - (unsigned)w, "%s{\"name\":\"%s\",\"exists\":%s,\"tag\":%s}", k ? "," : "", name,
                 st == NOT_FOUND ? "false" : "true", tag);
        k++;
    }
    w += fmt(o + w, room - (unsigned)w, "]}");
    reply(s, c, 200, "application/json", o, (unsigned long)w, NULL);
}

static void h_write(struct netinst *s, int c, const struct req *r, int create)
{
    char name[32], f[32], path[64], mp[16];
    unsigned long blocks = 0;
    param(r, "c", name, sizeof(name));
    if (!check(s, c, name, create ? OP_PUT : OP_WRITE))
        return;
    param(r, "f", f, sizeof(f));
    if (!file_ok(f))
        return reply_err(s, c, 400, "bad file name");
    if (create && (!param_num(r, "blocks", &blocks) || blocks < MIN_BLOCKS || blocks > MAX_BLOCKS))
        return reply_err(s, c, 400, "bad blocks");
    long n = body(s, c, r);
    if (n < 0)
        return reply_err(s, c, 400, "the body must be 0 to 4 MiB, with Content-Length");
    if (s->se.open && !strcmp(s->se.name, name))
        se_close(s);
    int rc;
    if (create) {
        E->remove(E, name);
        rc = E->mount(E, name, 6, blocks, mp, NULL);
    } else {
        rc = E->mount(E, name, 2, 0, mp, NULL);
    }
    if (rc)
        return reply_rc(s, c, (unsigned)rc, NULL);
    join(path, sizeof(path), mp, f);
    unsigned wrc = 0;
    int fd = E->open(E, path, NS_O_WRONLY | NS_O_CREAT | NS_O_TRUNC, 0666);
    if (fd < 0) {
        wrc = sce_of(fd);
    } else {
        long w = n ? E->write(E, fd, s->buf[0], (unsigned long)n) : 0;
        int crc = E->close(E, fd);
        wrc = w != n ? sce_of(w < 0 ? w : -NS_EIO) : crc < 0 ? sce_of(crc) : 0;
    }
    unsigned urc = (unsigned)E->umount(E, mp);
    if (!wrc && urc)
        wrc = urc;
    if (!wrc && create) {
        unsigned long size = 0;
        unsigned v = file_size(s, name, f, &size);
        if (v || size != (unsigned long)n)
            wrc = v ? v : 0xFFFFFFFEu;
    }
    LOG(s, "%s %s/%s, %ld bytes -> %x", create ? "put" : "write", name, f, n, wrc);
    if (!wrc && kind(name) == K_CATALOG) { /* the list of installed games changed (an install, a removal) */
        lock(s);
        s->st.changes++;
        unlock(s);
    }
    reply_rc(s, c, wrc, NULL);
}

static void h_delete(struct netinst *s, int c, const struct req *r)
{
    char name[32];
    param(r, "c", name, sizeof(name));
    if (!check(s, c, name, OP_DELETE))
        return;
    if (s->se.open && !strcmp(s->se.name, name))
        se_close(s);
    unsigned rc = (unsigned)E->remove(E, name);
    LOG(s, "delete %s -> %x", name, rc);
    reply_rc(s, c, rc, NULL);
}

/* whether `want` bytes fit: 1 GiB probes GFSFREE00.. until they do or the
 * storage is full (NO_SPACE_FS says what is missing), then deleted */
static void h_free(struct netinst *s, int c, const struct req *r)
{
    unsigned long want = 0, have = 0;
    unsigned made = 0, status = 0;
    param_num(r, "bytes", &want);
    while (have < want && made < 100) {
        char name[16], mp[16];
        unsigned long missing = 0;
        fmt(name, sizeof(name), "GFSFREE%02u", made);
        E->remove(E, name);
        unsigned rc = (unsigned)E->mount(E, name, 6, MAX_BLOCKS, mp, &missing);
        if (rc) {
            if (rc == NO_SPACE) {
                if (missing < MAX_BLOCKS)
                    have += (MAX_BLOCKS - missing) * 32768ul;
            } else {
                status = rc;
            }
            break;
        }
        E->umount(E, mp);
        made++;
        have += 1ul << 30;
    }
    for (unsigned i = 0; i <= made; i++) {
        char name[16];
        fmt(name, sizeof(name), "GFSFREE%02u", i);
        E->remove(E, name);
    }
    LOG(s, "free: %lu MiB wanted, %lu MiB fit (%u probes, %x)", want >> 20, have >> 20, made, status);
    char extra[40];
    fmt(extra, sizeof(extra), ",\"have\":%lu", have);
    reply_rc(s, c, status, extra);
}

/* ---- data: receive into the ring while the writer thread writes ---- */

static void *writer(void *arg)
{
    struct netinst *s = arg;
    for (unsigned long i = 0; i < __atomic_load_n(&s->wcount, __ATOMIC_ACQUIRE); i++) {
        unsigned b = (unsigned)(i % NETINST_BUFS);
        while (!ld(&s->full[b])) {
            if (__atomic_load_n(&s->wstop, __ATOMIC_ACQUIRE) || i >= __atomic_load_n(&s->wcount, __ATOMIC_ACQUIRE))
                goto out;
            E->sleep_ms(E, 1);
        }
        long w = E->write(E, s->wfd, s->buf[b], s->len[b]);
        if (w != (long)s->len[b]) {
            s->wstatus = w < 0 ? w : -NS_EIO;
            __atomic_store_n(&s->wstop, 1, __ATOMIC_RELEASE);
            goto out;
        }
        s->wwritten += (unsigned long)w;
        st_(&s->full[b], 0);
    }
out:
    __atomic_store_n(&s->wdone, 1, __ATOMIC_RELEASE);
    return 0;
}

static void progress(struct netinst *s, unsigned long base, unsigned long now_bytes)
{
    long t = E->clock_ms(E);
    lock(s);
    s->st.done = base + now_bytes;
    if (s->st.total && s->st.done > s->st.total)
        s->st.done = s->st.total;
    if (t - s->rate_t >= 1000) {
        if (s->rate_t && s->st.done >= s->rate_b)
            s->st.rate_kib = (unsigned)((s->st.done - s->rate_b) * 1000 / (unsigned long)(t - s->rate_t) >> 10);
        s->rate_t = t, s->rate_b = (unsigned long)s->st.done;
    }
    unlock(s);
}

/* receive n body bytes (pre first) and write them to the session's file:
 * 0, 1 (the connection was lost), or < 0 (a write error) */
static long take(struct netinst *s, int c, const unsigned char *pre, unsigned long pre_len, unsigned long n,
                 unsigned long base)
{
    unsigned long first = pre_len < n ? pre_len : n;
    if (first) {
        long w = E->write(E, s->se.fd, pre, first);
        if (w != (long)first)
            return w < 0 ? w : -NS_EIO;
        s->se.written += first;
    }
    unsigned long rest = n - first;
    if (!rest)
        return 0;
    for (unsigned b = 0; b < NETINST_BUFS; b++)
        s->full[b] = 0;
    s->wfd = s->se.fd, s->wstatus = 0, s->wwritten = 0, s->wstop = 0, s->wdone = 0;
    s->wcount = (rest + NETINST_BUF - 1) / NETINST_BUF;
    if (E->thread(E, writer, s, &s->wthread) != 0)
        return -NS_EAGAIN;
    unsigned long got = 0;
    int lost = 0;
    for (unsigned long i = 0; i < s->wcount && !lost; i++) {
        unsigned b = (unsigned)(i % NETINST_BUFS);
        while (ld(&s->full[b]) && !__atomic_load_n(&s->wstop, __ATOMIC_ACQUIRE))
            E->sleep_ms(E, 1);
        if (__atomic_load_n(&s->wstop, __ATOMIC_ACQUIRE))
            break;
        unsigned long want = rest - got < NETINST_BUF ? rest - got : NETINST_BUF;
        unsigned long k = recv_exact(s, c, s->buf[b], want);
        got += k;
        if (k != want) {
            lost = 1;
            __atomic_store_n(&s->wcount, i + (k ? 1 : 0), __ATOMIC_RELEASE); /* what arrived is still written */
            if (!k)
                break;
        }
        s->len[b] = k;
        st_(&s->full[b], 1);
        progress(s, base, s->se.written + s->wwritten);
    }
    if (!lost && got < rest)
        __atomic_store_n(&s->wstop, 1, __ATOMIC_RELEASE);
    E->join(E, s->wthread);
    s->se.written += s->wwritten;
    if (s->wstatus)
        return s->wstatus;
    return lost || got < rest ? 1 : 0;
}

static void h_data(struct netinst *s, int c, const struct req *r)
{
    char name[32], lid[40];
    unsigned long size = 0, blocks = 0, off = 0;
    param(r, "c", name, sizeof(name));
    if (!check(s, c, name, OP_DATA))
        return;
    param(r, "lid", lid, sizeof(lid));
    if (!param_num(r, "size", &size) || !param_num(r, "blocks", &blocks) || !param_num(r, "off", &off) ||
        r->length < 0 || size < 1 || blocks < MIN_BLOCKS || blocks > MAX_BLOCKS || size > blocks * 32768ul ||
        off + (unsigned long)r->length > size)
        return reply_err(s, c, 400, "bad data request");
    if (off == 0) {
        se_close(s);
        E->remove(E, name);
        unsigned long need = 0;
        int rc = E->mount(E, name, 6, blocks, s->se.mp, &need);
        if (rc) {
            say(s, "%s: can't create it (%x%s)", name, (unsigned)rc, (unsigned)rc == NO_SPACE ? ": no space" : "");
            return reply_rc(s, c, (unsigned)rc, NULL);
        }
        char path[64];
        join(path, sizeof(path), s->se.mp, "data.bin");
        int fd = E->open(E, path, NS_O_WRONLY | NS_O_CREAT | NS_O_TRUNC, 0666);
        if (fd < 0) {
            E->umount(E, s->se.mp);
            return reply_rc(s, c, sce_of(fd), NULL);
        }
        s->se.open = 1, s->se.fd = fd, s->se.size = size, s->se.blocks = blocks, s->se.written = 0;
        s->se.t0 = E->clock_ms(E);
        strlcpy(s->se.name, name, sizeof(s->se.name));
        strlcpy(s->se.lid, lid, sizeof(s->se.lid));
    } else if (!s->se.open || strcmp(s->se.name, name) || s->se.size != size || strcmp(s->se.lid, lid) ||
               s->se.written != off) {
        unsigned long have = s->se.open && !strcmp(s->se.name, name) && s->se.size == size && !strcmp(s->se.lid, lid)
                                 ? s->se.written
                                 : 0;
        return reply_json(s, c, 409, "{\"ok\":false,\"written\":%lu,\"error\":\"the console has %lu bytes of %s\"}",
                          have, have, name);
    }
    /* container k holds the title's bytes from k * CAP (gfs_pack.py) */
    unsigned long l = (unsigned long)strlen(name), base = 0;
    base = (unsigned long)((name[l - 3] - '0') * 100 + (name[l - 2] - '0') * 10 + (name[l - 1] - '0')) * CAP;
    lock(s);
    s->st.state = NETINST_BUSY;
    fmt(s->st.message, sizeof(s->st.message), "Receiving %s", name);
    unlock(s);
    E->timeouts(E, c, 20);
    long rc = take(s, c, r->pre, r->pre_len, (unsigned long)r->length, base);
    progress(s, base, s->se.written);
    if (rc < 0) {
        say(s, "%s: writing failed (%x) after %lu MiB", name, sce_of(rc), s->se.written >> 20);
        se_close(s);
        return reply_rc(s, c, sce_of(rc), NULL);
    }
    if (rc > 0) {
        say(s, "%s: the connection dropped at %lu MiB: waiting for the browser to carry on", name,
            s->se.written >> 20);
        return;
    }
    if (s->se.written < size) {
        char extra[40];
        fmt(extra, sizeof(extra), ",\"written\":%lu", s->se.written);
        return reply_rc(s, c, 0, extra);
    }
    long ms = E->clock_ms(E) - s->se.t0;
    unsigned crc = (unsigned)sce_of(E->close(E, s->se.fd));
    s->se.fd = -1;
    unsigned urc = se_close(s);
    unsigned long vsize = 0;
    unsigned v = crc ? crc : urc ? urc : file_size(s, name, "data.bin", &vsize);
    if (!v && vsize != size)
        v = 0xFFFFFFFEu;
    LOG(s, "%s: %lu MiB in %ld.%ld s (%lu MB/s), verify %x", name, size >> 20, ms / 1000, ms / 100 % 10,
           size / 1000 / (unsigned long)(ms > 0 ? ms : 1), v);
    if (v)
        return reply_rc(s, c, v, NULL);
    char extra[64];
    fmt(extra, sizeof(extra), ",\"written\":%lu,\"done\":true,\"millis\":%ld", size, ms);
    reply_rc(s, c, 0, extra);
}

static void h_note(struct netinst *s, int c, const struct req *r, int end)
{
    char title[16], name[64], v[24];
    unsigned long total = 0;
    param(r, "title", title, sizeof(title));
    param(r, "name", name, sizeof(name));
    param(r, "ok", v, sizeof(v));
    param_num(r, "total", &total);
    lock(s);
    if (!end) {
        s->st.state = NETINST_BUSY;
        strlcpy(s->st.title, title, sizeof(s->st.title));
        strlcpy(s->st.name, name, sizeof(s->st.name));
        s->st.total = total, s->st.done = 0, s->st.rate_kib = 0;
        s->rate_t = 0;
        fmt(s->st.message, sizeof(s->st.message), "Installing %s", name[0] ? name : title);
    } else {
        s->st.state = NETINST_WAITING;
        s->st.rate_kib = 0;
        if (v[0] == '1') {
            s->st.installed++;
            s->st.done = s->st.total;
            fmt(s->st.message, sizeof(s->st.message), "Installed %s", s->st.name[0] ? s->st.name : title);
        } else {
            fmt(s->st.message, sizeof(s->st.message), "The install of %s stopped: install again to carry on",
                s->st.name[0] ? s->st.name : title);
        }
    }
    unlock(s);
    if (end)
        LOG(s, "%s %s (%s)", v[0] == '1' ? "installed" : "stopped", title, s->st.name);
    else
        LOG(s, "begin %s (%s), %lu bytes", title, name, total);
    reply_json(s, c, 200, "{\"ok\":true}");
}

/* the token, with a growing wait after wrong ones (manager.lua check_token) */
static int auth(struct netinst *s, int c, const struct req *r)
{
    char want[12];
    unsigned k = 0;
    for (const char *t = s->token; *t; t++)
        if (*t != '-')
            want[k++] = *t;
    want[k] = 0;
    if (s->fails >= 30) {
        reply_err(s, c, 403, "writes are locked: too many wrong tokens (stop and start the installer)");
        return 0;
    }
    if (E->now(E) < s->wait_until) {
        reply_err(s, c, 429, "too many wrong tokens: wait a little");
        return 0;
    }
    if (r->token[0] && !strcmp(r->token, want))
        return 1;
    s->fails++;
    if (s->fails >= 5)
        s->wait_until = E->now(E) + (s->fails - 4 >= 9 ? 300 : 1 << (s->fails - 4));
    reply_err(s, c, 403, r->token[0] ? "wrong token" : "the token is missing: type the one the TV shows");
    return 0;
}

static void handle(struct netinst *s, int c)
{
    struct req r;
    int h = read_head(s, c, &r);
    if (h == -431)
        return reply_err(s, c, 400, "request head too large");
    if (h < 0)
        return;
    s->last_request = E->clock_ms(E);
    if (!strcmp(r.method, "GET")) {
        if (!strcmp(r.path, "/") || !strcmp(r.path, "/install") || !strcmp(r.path, "/index.html")) {
            return reply(s, c, 200, "text/html; charset=utf-8", E->page, E->page_size,
                         "Content-Encoding: gzip\r\nX-Frame-Options: DENY\r\nContent-Security-Policy: default-src "
                         "'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self' data: blob:; "
                         "frame-ancestors 'none'\r\n");
        }
        if (!strcmp(r.path, "/favicon.ico"))
            return reply(s, c, 204, "text/plain", "", 0, NULL);
        if (!strcmp(r.path, "/api/install/info"))
            return h_info(s, c);
        if (!strcmp(r.path, "/api/install/read"))
            return h_read(s, c, &r);
        return reply_err(s, c, 404, "not found");
    }
    if (strcmp(r.method, "POST"))
        return reply_err(s, c, 405, "method not allowed");
    if (!strcmp(r.path, "/api/install/exit")) {
        reply_json(s, c, 200, "{\"ok\":true}");
        s->stop = 1;
        return;
    }
    if (!auth(s, c, &r))
        return;
    if (!strcmp(r.path, "/api/install/auth") || !strcmp(r.path, "/api/auth"))
        return reply_json(s, c, 200, "{\"ok\":true}");
    if (!strcmp(r.path, "/api/install/probe"))
        return h_probe(s, c, &r);
    if (!strcmp(r.path, "/api/install/data"))
        return h_data(s, c, &r);
    if (!strcmp(r.path, "/api/install/put"))
        return h_write(s, c, &r, 1);
    if (!strcmp(r.path, "/api/install/write"))
        return h_write(s, c, &r, 0);
    if (!strcmp(r.path, "/api/install/delete"))
        return h_delete(s, c, &r);
    if (!strcmp(r.path, "/api/install/free"))
        return h_free(s, c, &r);
    if (!strcmp(r.path, "/api/install/begin"))
        return h_note(s, c, &r, 0);
    if (!strcmp(r.path, "/api/install/end"))
        return h_note(s, c, &r, 1);
    reply_err(s, c, 404, "not found");
}

static void *serve(void *arg)
{
    struct netinst *s = arg;
    while (!s->stop) {
        int c = E->accept(E, s->lfd, 250);
        if (c == -NS_ETIMEDOUT) {
            /* a browser that went quiet in the middle of an install */
            lock(s);
            if (s->st.state == NETINST_BUSY && E->clock_ms(E) - s->last_request > 60000) {
                s->st.state = NETINST_WAITING, s->st.rate_kib = 0;
                strlcpy(s->st.message, "The browser stopped sending: open the page again to carry on",
                        sizeof(s->st.message));
            }
            unlock(s);
            continue;
        }
        if (c < 0) {
            LOG(s, "accept -> %d", c);
            E->sleep_ms(E, 200);
            continue;
        }
        if (!s->st.url[7] || s->st.url[7] == '(') { /* the address the browsers reach us on */
            unsigned a = 0;
            if (E->sockname(E, c, &a) == 0 && a) {
                lock(s);
                fmt(s->st.url, sizeof(s->st.url), "http://%u.%u.%u.%u:%u/", a >> 24, (a >> 16) & 255, (a >> 8) & 255,
                    a & 255, s->port);
                unlock(s);
            }
        }
        __atomic_store_n(&s->client, c, __ATOMIC_RELEASE);
        E->timeouts(E, c, 10);
        handle(s, c);
        __atomic_store_n(&s->client, -1, __ATOMIC_RELEASE);
        E->sclose(E, c);
    }
    unsigned rc = se_close(s);
    E->sclose(E, s->lfd);
    lock(s);
    s->st.state = NETINST_STOPPED;
    fmt(s->st.message, sizeof(s->st.message), "Stopped%s", rc ? " (committing the last container failed)" : "");
    unlock(s);
    LOG(s, "stopped");
    __atomic_store_n(&s->exited, 1, __ATOMIC_RELEASE);
    return 0;
}

/* ---- start, status, stop ---- */

static void make_token(struct netinst *s, char out[12])
{
    unsigned long x = E->entropy(E) ^ (unsigned long)E->clock_ms(E) * 0x9E3779B97F4A7C15ul ^ (unsigned long)E->now(E);
    const char *a = TOKEN_CHARS;
    unsigned n = (unsigned)strlen(a), k = 0;
    for (unsigned i = 0; i < 8; i++) {
        x ^= x >> 29, x *= 0xBF58476D1CE4E5B9ul, x ^= x >> 32, x += E->entropy(E);
        out[k++] = a[x % n];
        if (i == 3)
            out[k++] = '-';
    }
    out[k] = 0;
}

static void release(struct netinst *s)
{
    struct ns_env *e = s->e;
    for (unsigned b = 0; b < NETINST_BUFS; b++)
        if (s->buf[b])
            e->free(e, s->buf[b], NETINST_BUF);
    e->free(e, s, sizeof(*s));
}

int ns_start(struct ns_env *e, unsigned short port, struct netinst **out)
{
    *out = NULL;
    struct netinst *s = e->alloc(e, sizeof(*s));
    if (!s)
        return -NS_ENOMEM;
    s->e = e;
    s->client = -1, s->se.fd = -1, s->lfd = -1;
    s->st.size = sizeof(s->st);
    for (unsigned b = 0; b < NETINST_BUFS; b++)
        if (!(s->buf[b] = e->alloc(e, NETINST_BUF))) {
            release(s);
            return -NS_ENOMEM;
        }
    *out = s;
    unsigned short p = port ? port : NETINST_PORT;
    int lfd = -1;
    for (unsigned short t = p; t < p + 10; t++)
        if ((lfd = E->listen(E, t)) >= 0) {
            p = t;
            break;
        }
    if (lfd < 0) {
        s->st.state = NETINST_FAILED;
        fmt(s->st.message, sizeof(s->st.message), "No network port free in %u-%u (%d)", port ? port : NETINST_PORT,
            (port ? port : NETINST_PORT) + 9, lfd);
        s->exited = 1;
        return lfd;
    }
    s->lfd = lfd, s->port = p;
    make_token(s, s->token);
    if (E->address[0])
        fmt(s->st.url, sizeof(s->st.url), "http://%s:%u/", E->address, p);
    else
        fmt(s->st.url, sizeof(s->st.url), "http://(this console):%u/", p);
    strlcpy(s->st.token, s->token, sizeof(s->st.token));
    s->st.state = NETINST_WAITING;
    strlcpy(s->st.message, "Open the address in a browser on the same network", sizeof(s->st.message));
    LOG(s, "serving %s, token %s", s->st.url, s->token);
    if (E->thread(E, serve, s, &s->thread) != 0) {
        E->sclose(E, lfd);
        s->lfd = -1;
        s->st.state = NETINST_FAILED;
        strlcpy(s->st.message, "Could not start the server's thread", sizeof(s->st.message));
        s->exited = 1;
        return -NS_EAGAIN;
    }
    return 0;
}

int ns_get_status(struct netinst *s, struct netinst_status *st)
{
    unsigned want = st->size && st->size < sizeof(*st) ? st->size : sizeof(*st);
    if (!s) {
        memset(st, 0, want);
        st->size = want;
        return 0;
    }
    lock(s);
    memcpy(st, &s->st, want);
    unlock(s);
    st->size = want;
    return 0;
}

struct ns_env *ns_env_of(struct netinst *s) { return s->e; }

void ns_stop(struct netinst *s)
{
    if (!s)
        return;
    if (!s->exited || s->thread) {
        s->stop = 1;
        int c = __atomic_load_n(&s->client, __ATOMIC_ACQUIRE);
        if (c >= 0)
            E->shutdown(E, c);
        if (s->thread)
            E->join(E, s->thread);
    }
    release(s);
}
