/*
 * Community memory plans on the console (plansync.h, docs/community-plans.md).
 *
 * One job, on a thread of the selector's own:
 *
 *  1. Share: each plan automem.c left in the outbox (share-<title>-<fp>-<platform>.bin
 *     in the store, while "Share learned memory plans" is on) is sent to the
 *     collection server, POST /v1/plans, and removed once the server has it
 *     (or refuses it for good: 400, 401, 413).
 *  2. Fetch: plans/index.json from GitHub's API, with the ETag of the last
 *     copy (a 304 costs nothing of the 60 requests an hour GitHub allows an
 *     address); the plans it lists for the catalogue's games, from
 *     raw.githubusercontent.com, checked against their SHA-256 and kept in
 *     the store (plan-<fp>-<platform>.bin).  The job's plans are those the
 *     index lists now.  An index request that fails (the rate limit's 403
 *     or 429, no network, a bad certificate) ends the job PS_FAILED with no
 *     plans: nothing is offered, and nothing is said.
 *
 * Each request is its own connection (Connection: close): HTTP/1.1 over
 * TLS 1.2 (BearSSL: ECDHE with AES-GCM or ChaCha20-Poly1305, the
 * certificates checked against plansync_ta.c's authorities and the
 * console's clock), DNS over UDP to the console's own servers.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
/* hidden: BearSSL's tables are this module's own (no GOT in the selector) */
#pragma GCC visibility push(hidden)
#include "bearssl.h"
extern const br_x509_trust_anchor PS_TAs[];
extern const unsigned PS_TAs_NUM;
#pragma GCC visibility pop
#include "plansync.h"

#define TIMEOUT_MS 6000
#define INDEX_CAP (256 << 10)
#define PLAN_CAP 4096
#define E_TIMEDOUT 60

/* ---- text ---- */

static unsigned long slen(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    return n;
}

/* append s to b (cap bytes, NUL kept) */
static void cat(char *b, unsigned long cap, const char *s)
{
    unsigned long n = slen(b);
    while (*s && n + 1 < cap)
        b[n++] = *s++;
    b[n] = 0;
}

static void cat_num(char *b, unsigned long cap, unsigned long v)
{
    char t[24];
    int i = 23;
    t[i] = 0;
    do
        t[--i] = (char)('0' + v % 10);
    while ((v /= 10) && i);
    cat(b, cap, t + i);
}

static void cat_hex(char *b, unsigned long cap, const unsigned char *p, unsigned n)
{
    static const char k[] = "0123456789abcdef";
    char t[3] = {0};
    for (unsigned i = 0; i < n; i++)
        t[0] = k[p[i] >> 4], t[1] = k[p[i] & 15], cat(b, cap, t);
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static int unhex(const char *s, unsigned char *out, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);
        if (a < 0 || b < 0)
            return -1;
        out[i] = (unsigned char)(a << 4 | b);
    }
    return 0;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* does s (n bytes) start with p, case aside? */
static int starts(const char *s, unsigned long n, const char *p)
{
    unsigned long k = 0;
    for (; p[k]; k++)
        if (k >= n || lower(s[k]) != lower(p[k]))
            return 0;
    return 1;
}

static void say(struct ps_env *e, const char *a, const char *b, long v)
{
    char line[200] = "plans: ";
    cat(line, sizeof(line), a);
    if (b)
        cat(line, sizeof(line), b);
    if (v >= 0)
        cat_num(line, sizeof(line), (unsigned long)v);
    e->log(e, line);
}

static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void cat_b64(char *b, unsigned long cap, const unsigned char *p, unsigned long n)
{
    unsigned long k = slen(b);
    for (unsigned long j = 0; j < n && k + 5 < cap; j += 3) {
        unsigned v = (unsigned)p[j] << 16 | (j + 1 < n ? (unsigned)p[j + 1] << 8 : 0) | (j + 2 < n ? p[j + 2] : 0);
        b[k++] = k_b64[v >> 18 & 63], b[k++] = k_b64[v >> 12 & 63];
        b[k++] = j + 1 < n ? k_b64[v >> 6 & 63] : '=';
        b[k++] = j + 2 < n ? k_b64[v & 63] : '=';
    }
    b[k] = 0;
}

/* ---- struct am_plan ---- */

static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

int ps_plan_ok(const unsigned char *plan, unsigned long n, const unsigned char fingerprint[20])
{
    /* magic "GLDRMEM1", version 1, size; the build it was learned on */
    static const unsigned char magic[8] = {'G', 'L', 'D', 'R', 'M', 'E', 'M', '1'};
    return n == PS_PLAN_SIZE && !memcmp(plan, magic, 8) && rd32(plan + 8) == 1 && rd32(plan + 12) == PS_PLAN_SIZE &&
           (!fingerprint || !memcmp(plan + 16, fingerprint, 20));
}

/* ---- DNS: an A record from the console's servers ---- */

static int dns_name(unsigned char *q, int at, const char *host)
{
    while (*host) {
        int len = 0;
        while (host[len] && host[len] != '.')
            len++;
        if (len == 0 || len > 63 || at + len + 2 > 300)
            return -1;
        q[at++] = (unsigned char)len;
        memcpy(q + at, host, (unsigned long)len);
        at += len, host += len;
        if (*host == '.')
            host++;
    }
    q[at++] = 0;
    return at;
}

/* skip a (possibly compressed) name at p; the offset after it, or -1 */
static int dns_skip(const unsigned char *r, int n, int p)
{
    while (p < n) {
        if (r[p] == 0)
            return p + 1;
        if ((r[p] & 0xc0) == 0xc0)
            return p + 2 <= n ? p + 2 : -1;
        p += 1 + r[p];
    }
    return -1;
}

static int resolve_with(struct ps_env *e, unsigned server, const char *host, unsigned *addr)
{
    unsigned char q[320], r[512];
    unsigned short id;
    e->random(e, &id, sizeof(id));
    memset(q, 0, 12);
    q[0] = (unsigned char)(id >> 8), q[1] = (unsigned char)id, q[2] = 1; /* recursion desired */
    q[5] = 1;                                                            /* one question */
    int at = dns_name(q, 12, host);
    if (at < 0)
        return -1;
    q[at++] = 0, q[at++] = 1, q[at++] = 0, q[at++] = 1; /* A, IN */
    int fd = e->udp(e);
    if (fd < 0) {
        say(e, "DNS: no datagram socket left (", NULL, -fd);
        return -1;
    }
    int rc = -1;
    long n = 0, sent = 0, conn = 0;
    e->timeouts(e, fd, 2500);
    for (int tries = 0; tries < 2 && rc < 0 && !e->stop; tries++) {
        if ((conn = e->connect(e, fd, server, 53, TIMEOUT_MS)) < 0 || (sent = e->send(e, fd, q, (unsigned long)at)) != at)
            break;
        n = e->recv(e, fd, r, sizeof(r));
        if (n < 12 || r[0] != q[0] || r[1] != q[1] || (r[3] & 15))
            continue;
        int qd = r[4] << 8 | r[5], an = r[6] << 8 | r[7], p = 12;
        for (int i = 0; i < qd && p > 0; i++)
            p = dns_skip(r, (int)n, p) + 4;
        for (int i = 0; i < an && p > 0 && p < n; i++) {
            p = dns_skip(r, (int)n, p);
            if (p < 0 || p + 10 > n)
                break;
            int type = r[p] << 8 | r[p + 1], len = r[p + 8] << 8 | r[p + 9];
            p += 10;
            if (type == 1 && len == 4 && p + 4 <= n) { /* CNAMEs before it are skipped */
                *addr = (unsigned)r[p] << 24 | r[p + 1] << 16 | r[p + 2] << 8 | r[p + 3];
                rc = 0;
                break;
            }
            p += len;
        }
    }
    e->sclose(e, fd);
    if (rc < 0) { /* which server, and how far the question got */
        char line[160] = "DNS: ";
        cat_num(line, sizeof(line), server >> 24), cat(line, sizeof(line), ".");
        cat_num(line, sizeof(line), server >> 16 & 255), cat(line, sizeof(line), ".");
        cat_num(line, sizeof(line), server >> 8 & 255), cat(line, sizeof(line), ".");
        cat_num(line, sizeof(line), server & 255), cat(line, sizeof(line), " for "), cat(line, sizeof(line), host);
        cat(line, sizeof(line), conn < 0 ? ": no connection" : sent != at ? ": not sent, " : ": answer ");
        cat_num(line, sizeof(line), (unsigned long)(conn < 0 ? 0 : sent != at ? -sent : n < 0 ? -n : n));
        if (n < 0)
            cat(line, sizeof(line), " (errno)");
        e->log(e, line);
    }
    return rc;
}

/* host: a name, or an IPv4 address as text */
static int resolve(struct ps_env *e, const char *host, unsigned *addr)
{
    unsigned a = 0, part = 0, dots = 0;
    const char *s = host;
    for (; *s; s++) {
        if (*s == '.' && dots < 3)
            a = a << 8 | part, part = 0, dots++;
        else if (*s >= '0' && *s <= '9' && part < 256)
            part = part * 10 + (unsigned)(*s - '0');
        else
            break;
    }
    if (!*s && dots == 3 && part < 256) {
        *addr = a << 8 | part;
        return 0;
    }
    for (int i = 0; i < 4; i++)
        if (e->dns_cache[i].addr && !memcmp(e->dns_cache[i].host, host, slen(host) + 1)) {
            *addr = e->dns_cache[i].addr;
            return 0;
        }
    unsigned servers[4];
    int n = e->dns(e, servers);
    for (int i = 0; i < n && i < 4; i++)
        if (resolve_with(e, servers[i], host, addr) == 0) {
            for (int k = 0; k < 4; k++)
                if (!e->dns_cache[k].addr && slen(host) < sizeof(e->dns_cache[k].host)) {
                    memcpy(e->dns_cache[k].host, host, slen(host) + 1);
                    e->dns_cache[k].addr = *addr;
                    break;
                }
            return 0;
        }
    return -1;
}

/* ---- a connection: TCP, with TLS when tls ---- */

struct conn {
    struct ps_env *e;
    int fd, tls;
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context io;
    unsigned char buf[BR_SSL_BUFSIZE_BIDI];
};

static int low_read(void *ctx, unsigned char *buf, size_t len)
{
    struct conn *c = ctx;
    long n = c->e->stop ? -1 : c->e->recv(c->e, c->fd, buf, len);
    return n > 0 ? (int)n : -1;
}

static int low_write(void *ctx, const unsigned char *buf, size_t len)
{
    struct conn *c = ctx;
    long n = c->e->stop ? -1 : c->e->send(c->e, c->fd, buf, len);
    return n > 0 ? (int)n : -1;
}

/* BearSSL's client, cut to what GitHub's and Cloudflare's servers offer:
 * TLS 1.2, ECDHE with ECDSA or RSA certificates, AES-GCM or
 * ChaCha20-Poly1305, constant-time code with no CPU feature tests */
static void tls_init(struct conn *c, const char *host, long now)
{
    static const uint16_t suites[] = {
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,       BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,       BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
        BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    };
    br_ssl_client_context *sc = &c->sc;
    br_x509_minimal_context *xc = &c->xc;
    br_ssl_client_zero(sc);
    br_ssl_engine_set_versions(&sc->eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(&sc->eng, suites, sizeof(suites) / sizeof(suites[0]));
    br_x509_minimal_init(xc, &br_sha256_vtable, PS_TAs, PS_TAs_NUM);
    br_ssl_engine_set_hash(&sc->eng, br_sha256_ID, &br_sha256_vtable);
    br_ssl_engine_set_hash(&sc->eng, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(xc, br_sha1_ID, &br_sha1_vtable);
    br_x509_minimal_set_hash(xc, br_sha256_ID, &br_sha256_vtable);
    br_x509_minimal_set_hash(xc, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(xc, br_sha512_ID, &br_sha512_vtable);
    br_ssl_engine_set_ec(&sc->eng, &br_ec_all_m31);
    br_ssl_engine_set_ecdsa(&sc->eng, br_ecdsa_i31_vrfy_asn1);
    br_ssl_engine_set_rsavrfy(&sc->eng, br_rsa_i31_pkcs1_vrfy);
    br_x509_minimal_set_rsa(xc, br_rsa_i31_pkcs1_vrfy);
    br_x509_minimal_set_ecdsa(xc, &br_ec_all_m31, br_ecdsa_i31_vrfy_asn1);
    br_ssl_engine_set_x509(&sc->eng, &xc->vtable);
    br_ssl_engine_set_prf_sha256(&sc->eng, br_tls12_sha256_prf);
    br_ssl_engine_set_prf_sha384(&sc->eng, br_tls12_sha384_prf);
    br_ssl_engine_set_gcm(&sc->eng, &br_sslrec_in_gcm_vtable, &br_sslrec_out_gcm_vtable);
    br_ssl_engine_set_aes_ctr(&sc->eng, &br_aes_ct64_ctr_vtable);
    br_ssl_engine_set_ghash(&sc->eng, br_ghash_ctmul64);
    br_ssl_engine_set_chapol(&sc->eng, &br_sslrec_in_chapol_vtable, &br_sslrec_out_chapol_vtable);
    br_ssl_engine_set_chacha20(&sc->eng, br_chacha20_ct_run);
    br_ssl_engine_set_poly1305(&sc->eng, br_poly1305_ctmul_run);
    /* days since year 0 and seconds into the day, as BearSSL counts them */
    br_x509_minimal_set_time(xc, (uint32_t)(now / 86400 + 719528), (uint32_t)(now % 86400));
    br_ssl_engine_set_buffer(&sc->eng, c->buf, sizeof(c->buf), 1);
    unsigned char seed[32];
    c->e->random(c->e, seed, sizeof(seed));
    br_ssl_engine_inject_entropy(&sc->eng, seed, sizeof(seed));
    br_ssl_client_reset(sc, host, 0);
    br_sslio_init(&c->io, &sc->eng, low_read, c, low_write, c);
}

static struct conn *conn_open(struct ps_env *e, const char *host, unsigned short port, int tls)
{
    unsigned addr;
    if (resolve(e, host, &addr) < 0) {
        say(e, "no address for ", host, -1);
        return NULL;
    }
    int fd = e->tcp(e);
    if (fd < 0)
        return NULL;
    if (e->connect(e, fd, addr, port, TIMEOUT_MS) < 0) {
        say(e, "no connection to ", host, -1);
        e->sclose(e, fd);
        return NULL;
    }
    e->timeouts(e, fd, TIMEOUT_MS);
    struct conn *c = e->alloc(e, sizeof(*c));
    if (!c) {
        e->sclose(e, fd);
        return NULL;
    }
    c->e = e, c->fd = fd, c->tls = tls;
    if (tls)
        tls_init(c, host, e->now(e));
    return c;
}

static void conn_close(struct conn *c)
{
    if (c->tls)
        br_sslio_close(&c->io);
    c->e->sclose(c->e, c->fd);
    c->e->free(c->e, c, sizeof(*c));
}

static int conn_write(struct conn *c, const void *p, unsigned long n)
{
    if (c->tls)
        return br_sslio_write_all(&c->io, p, n);
    return c->e->send(c->e, c->fd, p, n) == (long)n ? 0 : -1;
}

static long conn_read(struct conn *c, void *p, unsigned long n)
{
    if (!c->tls)
        return c->e->recv(c->e, c->fd, p, n);
    int r = br_sslio_read(&c->io, p, n);
    if (r < 0) { /* the peer's close_notify (or a close without one, as HTTP allows): the end */
        int err = br_ssl_engine_last_error(&c->sc.eng);
        return err == BR_ERR_OK || err == BR_ERR_IO ? 0 : -err;
    }
    return r;
}

/* ---- HTTP/1.1, one request per connection ---- */

struct resp {
    int status;
    long remaining;   /* X-RateLimit-Remaining, -1 when absent */
    char etag[96];
    unsigned char *body;
    unsigned long len;
    int truncated;    /* the body was longer than the room for it */
};

/* the request; the body (at most cap bytes, chunked encoding undone) in
 * r->body, r->truncated when there was more.  0, or -1 when there was no
 * complete answer. */
static int http(struct ps_env *e, int tls, const char *host, unsigned short port, const char *method,
                const char *path, const char *headers, const void *body, unsigned long blen, unsigned char *out,
                unsigned long cap, struct resp *r)
{
    memset(r, 0, sizeof(*r));
    r->remaining = -1, r->body = out;
    struct conn *c = conn_open(e, host, port, tls);
    if (!c)
        return -1;
    char req[1024] = "";
    cat(req, sizeof(req), method), cat(req, sizeof(req), " "), cat(req, sizeof(req), path);
    cat(req, sizeof(req), " HTTP/1.1\r\nHost: "), cat(req, sizeof(req), host);
    cat(req, sizeof(req), "\r\nUser-Agent: ghostldr\r\nConnection: close\r\n");
    cat(req, sizeof(req), headers);
    if (body)
        cat(req, sizeof(req), "Content-Length: "), cat_num(req, sizeof(req), blen), cat(req, sizeof(req), "\r\n");
    cat(req, sizeof(req), "\r\n");
    int rc = conn_write(c, req, slen(req)) || (body && conn_write(c, body, blen)) ||
             (tls && br_sslio_flush(&c->io)) ? -1 : 0;
    if (rc) {
        if (tls)
            say(e, "TLS with ", host, br_ssl_engine_last_error(&c->sc.eng)); /* 62: an unknown CA; 54: the clock */
        conn_close(c);
        return -1;
    }
    /* the head, a line at a time, then the body */
    char line[512];
    unsigned long ll = 0, clen = (unsigned long)-1, got = 0;
    int head = 1, chunked = 0, first = 1;
    unsigned char tmp[2048];
    /* the body: as it comes, or chunked: a size line, the chunk, its CRLF */
    enum { B_PLAIN, B_SIZE, B_DATA, B_CRLF } bs = B_PLAIN;
    unsigned long chunk = 0;
    int digits = 0, ext = 0;
    long n;
    while ((n = conn_read(c, tmp, sizeof(tmp))) > 0) {
        for (long i = 0; i < n; i++) {
            unsigned char ch = tmp[i];
            if (head) {
                if (ch != '\n') {
                    if (ll + 1 < sizeof(line) && ch != '\r')
                        line[ll++] = (char)ch;
                    continue;
                }
                line[ll] = 0;
                if (first) { /* HTTP/1.1 200 OK */
                    first = 0;
                    if (ll > 12 && starts(line, ll, "HTTP/1."))
                        r->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
                } else if (ll == 0) {
                    head = 0, bs = chunked ? B_SIZE : B_PLAIN;
                    if (!chunked && clen == 0)
                        goto done;
                } else if (starts(line, ll, "content-length:")) {
                    clen = 0;
                    for (const char *s = line + 15; *s; s++)
                        if (*s >= '0' && *s <= '9')
                            clen = clen * 10 + (unsigned long)(*s - '0');
                } else if (starts(line, ll, "transfer-encoding:") && ll >= 25) {
                    chunked = starts(line + ll - 7, 7, "chunked");
                } else if (starts(line, ll, "etag:")) {
                    const char *s = line + 5;
                    while (*s == ' ')
                        s++;
                    r->etag[0] = 0;
                    cat(r->etag, sizeof(r->etag), s);
                } else if (starts(line, ll, "x-ratelimit-remaining:")) {
                    r->remaining = 0;
                    for (const char *s = line + 22; *s; s++)
                        if (*s >= '0' && *s <= '9')
                            r->remaining = r->remaining * 10 + (*s - '0');
                }
                ll = 0;
                continue;
            }
            if (bs == B_SIZE) { /* "1a2b\r\n", extensions after ';' ignored */
                if (ch == '\n' && digits) {
                    if (chunk == 0) { /* the last chunk */
                        clen = got;
                        goto done;
                    }
                    bs = B_DATA;
                } else if (!ext && hexval((char)ch) >= 0) {
                    chunk = chunk << 4 | (unsigned long)hexval((char)ch), digits++;
                } else if (ch == ';') {
                    ext = 1;
                }
                continue;
            }
            if (bs == B_CRLF) {
                if (ch == '\n')
                    bs = B_SIZE, chunk = 0, digits = 0, ext = 0;
                continue;
            }
            if (got < cap)
                out[got] = ch;
            got++;
            if (bs == B_DATA && --chunk == 0)
                bs = B_CRLF;
            if (bs == B_PLAIN && got == clen)
                goto done;
        }
        if (e->stop)
            break;
    }
done:
    if (!r->status && c->tls)
        say(e, "no answer over TLS from ", host, -1);
    if (!r->status && c->tls)
        say(e, "  BearSSL error ", NULL, br_ssl_engine_last_error(&c->sc.eng)); /* 62: unknown CA, 54: the clock */
    conn_close(c);
    r->len = got < cap ? got : cap;
    r->truncated = got > cap;
    if (!r->status || head || (clen != (unsigned long)-1 && got < clen))
        return -1;
    return 0;
}

/* ---- the share ---- */

/* what the server is called, from share_host: "host[:port]" */
static void split_host(const char *spec, char *host, unsigned long cap, unsigned short *port, int tls)
{
    unsigned long i = 0;
    *port = tls ? 443 : 80;
    while (spec[i] && spec[i] != ':' && i + 1 < cap)
        host[i] = spec[i], i++;
    host[i] = 0;
    if (spec[i] == ':') {
        unsigned p = 0;
        for (const char *s = spec + i + 1; *s >= '0' && *s <= '9'; s++)
            p = p * 10 + (unsigned)(*s - '0');
        if (p && p < 65536)
            *port = (unsigned short)p;
    }
}

/* the store's install ID: random, made once */
static void install_id(struct ps_env *e, char out[33])
{
    char t[40];
    long n = e->load(e, "install-id.txt", t, 32);
    unsigned char b[16];
    if (n == 32 && unhex(t, b, 16) == 0) {
        memcpy(out, t, 32), out[32] = 0;
        return;
    }
    e->random(e, b, sizeof(b));
    out[0] = 0;
    cat_hex(out, 33, b, 16);
    e->save(e, "install-id.txt", out, 32);
}

static void share(struct ps_env *e, struct ps_job *job)
{
    char names[4096];
    int count = e->list(e, "share-", names, sizeof(names));
    if (count <= 0)
        return;
    if (!e->share_host[0] || !e->share_key[0]) { /* a build without the collection's address keeps them */
        job->unshared = (unsigned)count;
        return;
    }
    char host[64], id[33], auth[200] = "Content-Type: application/json\r\nAuthorization: Bearer ";
    unsigned short port;
    split_host(e->share_host, host, sizeof(host), &port, e->share_tls);
    install_id(e, id);
    cat(auth, sizeof(auth), e->share_key), cat(auth, sizeof(auth), "\r\n");
    unsigned char *plan = e->alloc(e, PLAN_CAP);
    char *json = e->alloc(e, 2048);
    unsigned char *reply = e->alloc(e, 512);
    const char *name = names;
    for (int i = 0; i < count && plan && json && reply && !e->stop; i++, name += slen(name) + 1) {
        /* share-<title>-<fingerprint>-<platform>.bin */
        unsigned long nl = slen(name);
        const char *fp = nl > 6 + 40 + 9 ? name + nl - 4 - 4 - 40 : NULL;
        unsigned char fpb[20];
        if (!fp || fp[-1] != '-' || fp[40] != '-' || unhex(fp, fpb, 20)) {
            e->unlink(e, name);
            continue;
        }
        char title[16] = "", platform[4] = {fp[41], fp[42], fp[43], 0};
        for (const char *s = name + 6; s < fp - 1 && slen(title) < 15; s++)
            title[slen(title)] = *s, title[slen(title) + 1] = 0;
        long n = e->load(e, name, plan, PLAN_CAP);
        if (!title[0] || (memcmp(platform, "ps5", 3) && memcmp(platform, "ps4", 3)) || !ps_plan_ok(plan, (unsigned long)n, fpb)) {
            e->unlink(e, name);
            continue;
        }
        json[0] = 0;
        cat(json, 2048, "{\"install\": \""), cat(json, 2048, id);
        cat(json, 2048, "\", \"title\": \""), cat(json, 2048, title);
        cat(json, 2048, "\", \"platform\": \""), cat(json, 2048, platform);
        cat(json, 2048, "\", \"fingerprint\": \""), cat_hex(json, 2048, fpb, 20);
        cat(json, 2048, "\", \"ghostldr\": \""), cat(json, 2048, e->version);
        cat(json, 2048, "\", \"plan\": \""), cat_b64(json, 2048, plan, PS_PLAN_SIZE), cat(json, 2048, "\"}");
        struct resp r;
        int rc = http(e, e->share_tls, host, port, "POST", "/v1/plans", auth, json, slen(json), reply, 512, &r);
        if (rc == 0 && (r.status == 200 || r.status == 201 || r.status == 400 || r.status == 401 || r.status == 413)) {
            e->unlink(e, name); /* kept, or refused for good */
            if (r.status <= 201)
                job->shared++;
            say(e, r.status <= 201 ? "shared " : "the server refused ", title, -1);
        } else {
            job->unshared++; /* kept for the next time */
            say(e, "the collection server didn't take it this time: HTTP ", NULL, rc == 0 ? r.status : 0);
        }
    }
    if (plan)
        e->free(e, plan, PLAN_CAP);
    if (json)
        e->free(e, json, 2048);
    if (reply)
        e->free(e, reply, 512);
}

/* ---- the fetch ---- */

/* the string value of "key" in the object text [p, end); 0 when absent */
static int json_str(const char *p, const char *end, const char *key, char *out, unsigned long cap)
{
    unsigned long kl = slen(key);
    for (; p + kl + 2 < end; p++) {
        if (*p != '"' || memcmp(p + 1, key, kl) || p[kl + 1] != '"')
            continue;
        const char *s = p + kl + 2;
        while (s < end && (*s == ' ' || *s == ':' || *s == '\t' || *s == '\n' || *s == '\r'))
            s++;
        unsigned long n = 0;
        if (s < end && *s == '"') {
            for (s++; s < end && *s != '"' && n + 1 < cap; s++)
                out[n++] = *s;
        } else {
            for (; s < end && ((*s >= '0' && *s <= '9') || *s == '-') && n + 1 < cap; s++)
                out[n++] = *s;
        }
        out[n] = 0;
        return n > 0;
    }
    return 0;
}

static int wanted(const struct ps_job *job, const char *title)
{
    for (unsigned i = 0; i < job->ntitles; i++)
        if (!memcmp(job->titles[i], title, slen(title) + 1))
            return 1;
    return 0;
}

/* split_host's twin for GitHub's hosts: the given one, else the default over TLS */
static int github(struct ps_env *e, const char *given, const char *def, char *host, unsigned short *port)
{
    int tls = !e->github_plain;
    host[0] = 0;
    if (given[0]) {
        split_host(given, host, 64, port, tls);
    } else {
        cat(host, 64, def);
        *port = 443;
    }
    return tls;
}

static void fetch(struct ps_env *e, struct ps_job *job)
{
    char api[64], raw[64];
    unsigned short api_port, raw_port;
    int api_tls = github(e, e->github_api, "api.github.com", api, &api_port);
    int raw_tls = github(e, e->github_raw, "raw.githubusercontent.com", raw, &raw_port);
    unsigned char *index = e->alloc(e, INDEX_CAP + 1);
    unsigned char *plan = e->alloc(e, PLAN_CAP);
    if (!index || !plan)
        goto out;
    char etag[96] = "", headers[200] = "Accept: application/vnd.github.raw+json\r\n";
    long have = e->load(e, "plans-index.json", index, INDEX_CAP);
    long en = have > 0 ? e->load(e, "plans-etag.txt", etag, sizeof(etag) - 1) : -1;
    if (en > 0) {
        etag[en] = 0;
        cat(headers, sizeof(headers), "If-None-Match: "), cat(headers, sizeof(headers), etag);
        cat(headers, sizeof(headers), "\r\n");
    }
    struct resp r;
    int rc = http(e, api_tls, api, api_port, "GET",
                  "/repos/" PS_GITHUB_REPO "/contents/plans/index.json?ref=" PS_GITHUB_BRANCH, headers, NULL, 0,
                  index, INDEX_CAP, &r);
    job->github = rc == 0 ? r.status : 0;
    unsigned long len;
    if (rc == 0 && r.status == 304 && have > 0) {
        len = (unsigned long)have;
        if (e->load(e, "plans-index.json", index, INDEX_CAP) != have)
            goto out;
    } else if (rc == 0 && r.status == 200 && !r.truncated) {
        len = r.len;
        e->save(e, "plans-index.json", index, len);
        if (r.etag[0])
            e->save(e, "plans-etag.txt", r.etag, slen(r.etag));
    } else {
        /* the rate limit (403 or 429 with X-RateLimit-Remaining 0), no plans
         * yet (404), no network: nothing offered, nothing said on screen */
        say(e, "GitHub's index: no answer this time (HTTP ", NULL, job->github);
        goto out;
    }
    index[len] = 0;
    /* the objects in "plans": [ {...}, ... ] */
    const char *p = (const char *)index, *end = p + len;
    while (p < end && job->nplans < PS_MAX_PLANS && !e->stop) {
        while (p < end && *p != '{')
            p++;
        const char *o = p;
        while (p < end && *p != '}')
            p++;
        if (p >= end)
            break;
        char title[16], fp[48], platform[8], sha[72], count[12];
        unsigned char fpb[20], shab[32], got[32];
        if (!json_str(o, p, "title", title, sizeof(title)) || !json_str(o, p, "fingerprint", fp, sizeof(fp)) ||
            !json_str(o, p, "platform", platform, sizeof(platform)) || !json_str(o, p, "sha256", sha, sizeof(sha)) ||
            slen(fp) != 40 || unhex(fp, fpb, 20) || slen(sha) != 64 || unhex(sha, shab, 32) ||
            (memcmp(platform, "ps5", 4) && memcmp(platform, "ps4", 4)) || !wanted(job, title))
            continue;
        char name[64] = "plan-", path[160] = "/" PS_GITHUB_REPO "/" PS_GITHUB_BRANCH "/plans/";
        cat(name, sizeof(name), fp), cat(name, sizeof(name), "-"), cat(name, sizeof(name), platform);
        cat(name, sizeof(name), ".bin");
        long n = e->load(e, name, plan, PLAN_CAP);
        br_sha256_context h;
        br_sha256_init(&h);
        br_sha256_update(&h, plan, n > 0 ? (unsigned long)n : 0);
        br_sha256_out(&h, got);
        if (n != PS_PLAN_SIZE || memcmp(got, shab, 32)) { /* not kept yet, or another version */
            cat(path, sizeof(path), title), cat(path, sizeof(path), "/"), cat(path, sizeof(path), name + 5);
            struct resp pr;
            if (http(e, raw_tls, raw, raw_port, "GET", path, "", NULL, 0, plan, PLAN_CAP, &pr) ||
                pr.status != 200 || pr.truncated)
                continue;
            n = (long)pr.len;
            br_sha256_init(&h);
            br_sha256_update(&h, plan, (unsigned long)n);
            br_sha256_out(&h, got);
            if (memcmp(got, shab, 32) || !ps_plan_ok(plan, (unsigned long)n, fpb))
                continue;
            e->save(e, name, plan, (unsigned long)n);
        }
        if (!ps_plan_ok(plan, (unsigned long)n, fpb))
            continue;
        struct ps_plan *out = &job->plans[job->nplans++];
        memset(out, 0, sizeof(*out));
        memcpy(out->title, title, slen(title));
        memcpy(out->fingerprint, fpb, 20);
        out->platform = platform[2] == '4';
        out->contributors = 1;
        if (json_str(o, p, "contributors", count, sizeof(count))) {
            unsigned v = 0;
            for (const char *s = count; *s >= '0' && *s <= '9'; s++)
                v = v * 10 + (unsigned)(*s - '0');
            out->contributors = v ? v : 1;
        }
        memcpy(out->plan, plan, PS_PLAN_SIZE);
    }
    say(e, "GitHub's index: plans for the games here: ", NULL, (long)job->nplans);
out:
    if (index)
        e->free(e, index, INDEX_CAP + 1);
    if (plan)
        e->free(e, plan, PLAN_CAP);
}

void ps_run(struct ps_env *e, struct ps_job *job)
{
    job->state = PS_RUNNING;
    if (job->share)
        share(e, job);
    if (job->fetch && !e->stop)
        fetch(e, job);
    __atomic_store_n(&job->state, job->github == 200 || job->github == 304 ? PS_DONE : PS_FAILED, __ATOMIC_RELEASE);
}

/* ---- one request for artfetch.c (game art from the services the player gave keys for) ---- */
int ps_http(struct ps_env *e, int tls, const char *host, unsigned short port, const char *method, const char *path,
            const char *headers, const void *body, unsigned long blen, unsigned char *out, unsigned long cap,
            unsigned long *len, int *truncated)
{
    struct resp r;
    int rc = http(e, tls, host, port, method, path, headers, body, blen, out, cap, &r);
    *len = r.len, *truncated = r.truncated;
    return rc == 0 ? r.status : -1;
}
