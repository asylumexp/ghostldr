/*
 * Community memory plans on the console (plansync.h): struct ps_env over
 * what the game selector module has, ghost_api and libkernel, and the
 * selector's three calls.
 *
 *  - Sockets: as for the network installer (netinst_sel.c), SWRR's own
 *    socket() answers ENOSYS for an internet socket on a PS5 at 8.00 or
 *    later, and the JIT compiler process's sockets can only be asked for on
 *    the boot thread.  plansync_start, on the boot thread, makes all the job
 *    can use (PS_TCP stream, PS_UDP datagram); the job's thread takes them
 *    in turn.  A connect waits at most its timeout (non-blocking, then poll).
 *  - DNS: the network's own servers (sceNetCtlGetInfo PRIMARY_DNS,
 *    SECONDARY_DNS), then 1.1.1.1 and 8.8.8.8 (a network's own may not
 *    answer: one that points the console at a computer that's off).
 *  - Randomness: the kernel's (sysctl kern.arandom), mixed with the TSC.
 *  - The store: ghostldr's GLDRPLANS save container (made when missing),
 *    mounted read-write for the job.  automem.c puts plans to share there
 *    (share-*.bin); this side keeps GitHub's index, its ETag and the plans.
 *  - The collection server's address and key: built in from the build
 *    machine's GHOSTLDR_CONTRIB_URL and GHOSTLDR_CONTRIB_KEY, else its
 *    ~/.config/ghostldr/contrib-url.txt and contrib.key (the Makefile writes
 *    them into contrib.h in the build folder); never in the repository.  A
 *    build without them keeps the outbox for one that has them.
 */
#include "ghost_api.h"
#include "plansync.h"
#include "contrib.h" /* PS_SHARE_URL, PS_SHARE_KEY: the Makefile writes it, from the build machine's own files */

#ifndef GHOSTLDR_VERSION
#define GHOSTLDR_VERSION "unknown"
#endif

#define PS_STORE "GLDRPLANS"
#define PS_STORE_BLOCKS 96 /* 3 MiB: the smallest save container */
#define PS_TCP 16
#define PS_UDP 8
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOL_SOCKET 0xffff
#define SO_NOSIGPIPE 0x800
#define SO_SNDTIMEO 0x1005
#define SO_RCVTIMEO 0x1006
#define SO_ERROR 0x1007
#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 4
#define POLLOUT 4
#define SYS_socket 97
#define SYS_close 6
#define ENOSYS 78
#define EINTR 4
#define EINPROGRESS 36
#define SAVE_NOT_FOUND ((s32)0x809F0008)

struct sel_ps {
    struct ps_env e; /* first: plansync.c's view */
    struct ps_job job;
    const struct ghost_api *api;
    struct boot_ctx *ctx;
    struct kfuncs *k;
    int (*socket)(int, int, int);
    int (*unlink)(const char *);
    int (*shutdown)(int, int);
    int (*connect)(int, const void *, u32);
    int (*setsockopt)(int, int, int, const void *, u32);
    int (*getsockopt)(int, int, int, void *, u32 *);
    int (*fcntl)(int, int, ...);
    int (*poll)(void *, u32, int);
    s64 (*recvfrom)(int, void *, u64, int, void *, u32 *);
    int (*gettimeofday)(void *);
    u64 (*process_time)(void);
    int (*sysctl)(const int *, u32, void *, u64 *, const void *, u64);
    int (*join)(u64, void **);
    s32 (*netctl)(s32, void *);
    int tcp[PS_TCP], udp[PS_UDP];
    unsigned ntcp, nudp, next_tcp, next_udp;
    char mp[16];
    int mounted;
    u64 thread;
};

struct sin {
    u8 len, family;
    u16 port;
    u32 addr;
    u8 zero[8];
};

#define P ((struct sel_ps *)e)
static u16 be16(u16 v) { return (u16)(v << 8 | v >> 8); }
static long errno_(struct ps_env *e) { return P->k->errno_fn ? *P->k->errno_fn() : 5; }

static void *e_alloc(struct ps_env *e, unsigned long size) { return P->api->page_alloc(size); }
static void e_free(struct ps_env *e, void *p, unsigned long size) { P->api->page_free(p, size); }
static void e_log(struct ps_env *e, const char *line) { P->api->logf("%s", line); }

static long e_now(struct ps_env *e)
{
    s64 tv[2] = {0, 0};
    return P->gettimeofday && P->gettimeofday(tv) == 0 ? tv[0] : 0;
}
static long e_clock(struct ps_env *e) { return P->process_time ? (long)(P->process_time() / 1000) : 0; }

static void e_random(struct ps_env *e, void *buf, unsigned long n)
{
    static const int mib[2] = {1 /* CTL_KERN */, 37 /* KERN_ARND */};
    u8 *b = buf;
    for (unsigned long at = 0; at < n;) {
        u8 t[32];
        u64 len = sizeof(t);
        if (!P->sysctl || P->sysctl(mib, 2, t, &len, NULL, 0) != 0 || !len)
            len = 0;
        u32 lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        u64 tsc = ((u64)hi << 32 | lo) * 0x9E3779B97F4A7C15ul;
        for (unsigned long k = 0; k < sizeof(t) && at < n; k++, at++)
            b[at] = (u8)((k < len ? t[k] : 0) ^ (u8)(tsc >> (8 * (k & 7))) ^ (u8)(at * 131));
    }
}

/* one more socket, made on the boot thread: SWRR's own, else the JIT
 * compiler process's (netinst_sel.c's way) */
static int make_socket(struct sel_ps *v, int type)
{
    int fd = v->socket ? v->socket(AF_INET, type, 0) : -1;
    if (fd >= 0 || !v->ctx->jit_syscall)
        return fd;
    if (v->api->rpc_lock)
        v->api->rpc_lock();
    long cfd = (long)v->api->jit_call(v->ctx->jit_syscall, SYS_socket, AF_INET, (u64)type, 0, 0, 0, 0);
    fd = cfd >= 0 ? v->api->jit_recv_fd((int)cfd) : -1;
    if (cfd >= 0)
        v->api->jit_call(v->ctx->jit_syscall, SYS_close, (u64)cfd, 0, 0, 0, 0, 0);
    if (v->api->rpc_unlock)
        v->api->rpc_unlock();
    if (fd >= 0) {
        int one = 1;
        v->setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, 4);
    }
    return fd;
}

static int e_tcp(struct ps_env *e) { return P->next_tcp < P->ntcp ? P->tcp[P->next_tcp++] : -ENOSYS; }
static int e_udp(struct ps_env *e) { return P->next_udp < P->nudp ? P->udp[P->next_udp++] : -ENOSYS; }

static int e_connect(struct ps_env *e, int fd, unsigned addr, unsigned short port, int timeout_ms)
{
    struct sin a = {16, AF_INET, be16(port), __builtin_bswap32(addr), {0}};
    int fl = P->fcntl ? P->fcntl(fd, F_GETFL, 0) : -1;
    if (fl >= 0)
        P->fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = P->connect(fd, &a, sizeof(a));
    if (rc != 0 && errno_(e) == EINPROGRESS) {
        struct {
            int fd;
            s16 events, revents;
        } p = {fd, POLLOUT, 0};
        int n;
        do
            n = P->poll(&p, 1, timeout_ms);
        while (n < 0 && errno_(e) == EINTR);
        int err = n > 0 ? 0 : 60;
        u32 len = 4;
        if (n > 0 && P->getsockopt)
            P->getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        rc = err ? -1 : 0;
    }
    if (fl >= 0)
        P->fcntl(fd, F_SETFL, fl);
    return rc == 0 ? 0 : -1;
}

static void e_timeouts(struct ps_env *e, int fd, int ms)
{
    s64 tv[2] = {ms / 1000, (ms % 1000) * 1000};
    P->setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, tv, sizeof(tv));
    P->setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, tv, sizeof(tv));
}

static long e_send(struct ps_env *e, int fd, const void *buf, unsigned long n)
{
    unsigned long done = 0;
    while (done < n) {
        long w = P->k->sendto(fd, (const u8 *)buf + done, n - done, 0, NULL, 0);
        if (w < 0 && errno_(e) == EINTR)
            continue;
        if (w <= 0)
            return w < 0 ? -errno_(e) : -5;
        done += (unsigned long)w;
    }
    return (long)n;
}

static long e_recv(struct ps_env *e, int fd, void *buf, unsigned long n)
{
    long r;
    do
        r = P->recvfrom(fd, buf, n, 0, NULL, NULL);
    while (r < 0 && errno_(e) == EINTR);
    return r < 0 ? -errno_(e) : r;
}

static void e_sclose(struct ps_env *e, int fd) { P->k->close(fd); }

/* "192.168.1.1" -> host order */
static unsigned ipv4(const char *s)
{
    unsigned a = 0, part = 0, dots = 0;
    for (; *s; s++) {
        if (*s == '.')
            a = a << 8 | part, part = 0, dots++;
        else if (*s >= '0' && *s <= '9')
            part = part * 10 + (unsigned)(*s - '0');
        else
            break;
    }
    return dots == 3 && part < 256 ? a << 8 | part : 0;
}

static int e_dns(struct ps_env *e, unsigned out[4])
{
    int n = 0;
    for (int k = 0; k < 2 && P->netctl; k++) {
        char buf[0x200];
        memset(buf, 0, sizeof(buf));
        unsigned a = P->netctl(17 + k /* PRIMARY_DNS, SECONDARY_DNS */, buf) == 0 ? ipv4(buf) : 0;
        if (a && (n == 0 || out[0] != a))
            out[n++] = a;
    }
    static const unsigned public_dns[2] = {0x01010101, 0x08080808};
    for (int k = 0; k < 2; k++)
        if (n < 4 && (n == 0 || out[0] != public_dns[k]) && (n < 2 || out[1] != public_dns[k]))
            out[n++] = public_dns[k];
    return n;
}

/* ---- the store ---- */

static int store(struct ps_env *e)
{
    if (P->mounted)
        return 0;
    int rc = P->api->gfs_mount(PS_STORE, 2, 0, P->mp);
    if (rc == SAVE_NOT_FOUND)
        rc = P->api->gfs_mount(PS_STORE, 6, PS_STORE_BLOCKS, P->mp);
    if (rc != 0) {
        P->api->logf("plans: %s not mounted (%x)", PS_STORE, (u32)rc);
        return -1;
    }
    P->mounted = 1;
    return 0;
}

static void path(struct ps_env *e, char *out, const char *name) { fmt(out, 96, "%s/%s", P->mp, name); }

static long e_load(struct ps_env *e, const char *name, void *buf, unsigned long cap)
{
    char p[96];
    if (store(e))
        return -5;
    path(e, p, name);
    int fd = P->k->open(p, 0 /* O_RDONLY */, 0);
    if (fd < 0)
        return -2;
    long n = P->k->read(fd, buf, cap);
    P->k->close(fd);
    return n < 0 ? -5 : n;
}

static int e_save(struct ps_env *e, const char *name, const void *buf, unsigned long n)
{
    char p[96];
    if (store(e))
        return -5;
    path(e, p, name);
    int fd = P->k->open(p, 1 | 0x200 | 0x400 /* O_WRONLY | O_CREAT | O_TRUNC */, 0666);
    if (fd < 0)
        return -5;
    long w = P->k->write(fd, buf, n);
    P->k->close(fd);
    return w == (long)n ? 0 : -5;
}

static int e_unlink(struct ps_env *e, const char *name)
{
    char p[96];
    if (store(e))
        return -5;
    path(e, p, name);
    return P->unlink ? P->unlink(p) : -5;
}

static int e_list(struct ps_env *e, const char *prefix, char *names, unsigned long cap)
{
    if (store(e) || !P->api->vfs_open || !P->api->vfs_getdents)
        return 0;
    int fd = P->api->vfs_open(P->mp, 0, 0);
    if (fd < 0)
        return 0;
    int count = 0;
    unsigned long at = 0, pl = strlen(prefix);
    u8 buf[1024];
    int got;
    while ((got = P->api->vfs_getdents(fd, buf, sizeof(buf))) > 0)
        for (int off = 0; off + 8 < got;) { /* FreeBSD dirent: fileno, reclen, type, namlen, name */
            const u8 *d = buf + off;
            u16 reclen = *(const u16 *)(d + 4);
            u8 namlen = d[7];
            if (!reclen)
                break;
            const char *nm = (const char *)d + 8;
            if (namlen >= pl && !memcmp(nm, prefix, pl) && at + namlen + 1 <= cap) {
                memcpy(names + at, nm, namlen), names[at + namlen] = 0;
                at += namlen + 1, count++;
            }
            off += reclen;
        }
    P->k->close(fd);
    return count;
}

/* ---- the selector's calls ---- */

static void *job_thread(void *arg)
{
    struct sel_ps *v = arg;
    ps_run(&v->e, &v->job);
    if (v->mounted && v->api->gfs_umount(v->mp, 60000) == 0)
        v->mounted = 0;
    return NULL;
}

/* Start the job for the catalogue's titles (on the boot thread: it makes the
 * sockets here).  share: send the outbox; fetch: check GitHub.  0, or < 0
 * when nothing was started (then nothing is offered). */
int plansync_start(const struct ghost_api *api, const char (*titles)[16], unsigned n, int share, int fetch,
                   void **handle)
{
    *handle = NULL;
    if (!share && !fetch)
        return -1;
    struct sel_ps *v = api->page_alloc(sizeof(*v));
    if (!v)
        return -12;
    struct ps_env *e = &v->e;
    v->api = api, v->ctx = *api->ctx, v->k = api->k;
    u32 lk = (u32)v->ctx->libkernel_handle;
#define SYM(field, name) *(void **)&v->field = api->host_dlsym(lk, name)
    SYM(socket, "socket");
    SYM(shutdown, "shutdown");
    SYM(unlink, "unlink");
    SYM(connect, "connect");
    SYM(setsockopt, "setsockopt");
    SYM(getsockopt, "getsockopt");
    SYM(fcntl, "fcntl");
    SYM(poll, "poll");
    SYM(recvfrom, "recvfrom");
    SYM(gettimeofday, "sceKernelGettimeofday");
    SYM(process_time, "sceKernelGetProcessTime");
    SYM(sysctl, "sysctl");
    SYM(join, "scePthreadJoin");
#undef SYM
    if (api->host_real_in)
        *(void **)&v->netctl = api->host_real_in("libSceNetCtl", "sceNetCtlGetInfo");
    if (!v->connect || !v->setsockopt || !v->poll || !v->recvfrom || !v->k->sendto || !v->join) {
        api->logf("plans: libkernel lacks a socket or thread call");
        api->page_free(v, sizeof(*v));
        return -ENOSYS;
    }
    for (unsigned i = 0; i < PS_TCP; i++)
        if ((v->tcp[v->ntcp] = make_socket(v, SOCK_STREAM)) >= 0)
            v->ntcp++;
    for (unsigned i = 0; i < PS_UDP; i++)
        if ((v->udp[v->nudp] = make_socket(v, SOCK_DGRAM)) >= 0)
            v->nudp++;
    e->ctx = v;
    e->alloc = e_alloc, e->free = e_free, e->log = e_log, e->now = e_now, e->clock_ms = e_clock;
    e->random = e_random, e->tcp = e_tcp, e->udp = e_udp, e->connect = e_connect, e->timeouts = e_timeouts;
    e->send = e_send, e->recv = e_recv, e->sclose = e_sclose, e->dns = e_dns;
    e->load = e_load, e->save = e_save, e->unlink = e_unlink, e->list = e_list;
    /* "https://host[:port]" or, for a server on the LAN, "http://host[:port]" */
    const char *url = PS_SHARE_URL;
    e->share_tls = !strncmp(url, "https://", 8);
    if (e->share_tls || !strncmp(url, "http://", 7))
        strlcpy(e->share_host, url + (e->share_tls ? 8 : 7), sizeof(e->share_host));
    for (char *s = e->share_host; *s; s++)
        if (*s == '/')
            *s = 0;
    strlcpy(e->share_key, PS_SHARE_KEY, sizeof(e->share_key));
    strlcpy(e->version, GHOSTLDR_VERSION, sizeof(e->version));
    for (unsigned i = 0; i < n && v->job.ntitles < PS_MAX_TITLES; i++)
        if (titles[i][0])
            memcpy(v->job.titles[v->job.ntitles++], titles[i], 16);
    v->job.share = share, v->job.fetch = fetch;
    api->logf("plans: %u stream and %u datagram sockets for %s%s%s (%u games)", v->ntcp, v->nudp,
              share ? "sharing" : "", share && fetch ? " and " : "", fetch ? "GitHub's plans" : "", v->job.ntitles);
    u8 attr[0x40];
    v->k->pthread_attr_init(attr);
    v->k->pthread_attr_setstacksize(attr, 256 << 10);
    int rc = v->k->pthread_create(&v->thread, attr, job_thread, v, "plansync");
    v->k->pthread_attr_destroy(attr);
    if (rc != 0) {
        for (unsigned i = 0; i < v->ntcp; i++)
            v->k->close(v->tcp[i]);
        for (unsigned i = 0; i < v->nudp; i++)
            v->k->close(v->udp[i]);
        api->page_free(v, sizeof(*v));
        return -35;
    }
    *handle = v;
    return 0;
}

/* the finished job, or NULL while it runs */
const struct ps_job *plansync_poll(void *handle)
{
    struct sel_ps *v = handle;
    if (!v)
        return NULL;
    int st = __atomic_load_n(&v->job.state, __ATOMIC_ACQUIRE);
    return st == PS_DONE || st == PS_FAILED ? &v->job : NULL;
}

/* Stop it (whatever step it is at gives up at its next read or write),
 * wait for its thread, close the sockets it didn't use, free it all. */
void plansync_stop(void *handle)
{
    struct sel_ps *v = handle;
    if (!v)
        return;
    v->e.stop = 1;
    for (unsigned i = 0; v->shutdown && i < v->ntcp; i++) /* a step waiting on one wakes now */
        v->shutdown(v->tcp[i], 2);
    for (unsigned i = 0; v->shutdown && i < v->nudp; i++)
        v->shutdown(v->udp[i], 2);
    v->join(v->thread, NULL);
    for (unsigned i = v->next_tcp; i < v->ntcp; i++)
        v->k->close(v->tcp[i]);
    for (unsigned i = v->next_udp; i < v->nudp; i++)
        v->k->close(v->udp[i]);
    if (v->mounted)
        v->api->gfs_umount(v->mp, 60000);
    v->api->page_free(v, sizeof(*v));
}
