/*
 * ghostldr's FTP server on the console (ftp.h, docs/ftp.md): ftp.c's
 * environment over libkernel, started as the title's main thread is about
 * to be created (ext_exports.title_starting) on run.py --ftp-port's port
 * (1212 unless given; 0: no server).
 *
 * Before the title's code runs, the session to ShellCore's AutoMounter is
 * opened and a USB drive mounted at /ghostldr_usb when one is plugged in
 * (approot.c usb_ftp_prepare), so clients start on the drive; one plugged in
 * later is mounted as a client logs in (usb_ftp_home).  The title's own
 * sceUsbStorageInit and Term are kept off that session (hle_usbstorage.c).
 *
 * Sockets: on a PS5 at 8.00 or later (and a PS4 at 14.00 or later) SWRR's
 * own socket() answers ENOSYS for an internet socket, so, as Luac0re does,
 * the JIT compiler process makes it and hands it over (jit_recv_fd).  That
 * channel is the boot thread's, so ftp.c makes every socket it listens on
 * in ftp_start, here on the boot thread; bind, listen, accept and the
 * transfers then work in SWRR.  Active mode needs a socket made later, so
 * it works only where SWRR's socket() does.  The other calls are the ones
 * the Luac0re emulator payloads' FTP servers use (recvfrom, sendto,
 * getsockname, setsockopt).  SIGPIPE is ignored in the whole process and
 * every socket here has SO_NOSIGPIPE: a client that drops its connection
 * must not take SWRR with it.
 */
#include "ext.h"
#include "ftp.h"

void usb_ftp_prepare(void) HIDDEN;      /* approot.c */
const char *usb_ftp_home(void) HIDDEN; /* approot.c */

#define AF_INET 2
#define SOCK_STREAM 1
#define SOL_SOCKET 0xffff
#define SO_REUSEADDR 0x4
#define SO_REUSEPORT 0x200
#define SO_NOSIGPIPE 0x800
#define SO_SNDBUF 0x1001
#define SO_RCVBUF 0x1002
#define SO_SNDTIMEO 0x1005
#define SO_RCVTIMEO 0x1006
#define POLLIN 1
#define SIGPIPE 13
#define O_DIRECTORY 0x20000

static struct {
    int (*socket)(int, int, int);
    int (*bind)(int, const void *, u32);
    int (*listen)(int, int);
    int (*accept)(int, void *, u32 *);
    int (*connect)(int, const void *, u32);
    int (*setsockopt)(int, int, int, const void *, u32);
    int (*getsockopt)(int, int, int, void *, u32 *);
    int (*getsockname)(int, void *, u32 *);
    s64 (*recvfrom)(int, void *, u64, int, void *, u32 *);
    int (*shutdown)(int, int);
    int (*poll)(void *, u32, int);
    int (*getdents)(int, void *, int);
    int (*mkdir)(const char *, int);
    int (*rmdir)(const char *);
    int (*unlink)(const char *);
    int (*rename)(const char *, const char *);
    int (*gettimeofday)(void *);
    u64 (*process_time)(void);
    int (*detach)(u64);
    int (*sigaction)(int, const void *, void *);
} S;

struct sin {
    u8 len, family;
    u16 port;
    u32 addr;
    u8 zero[8];
};

static u16 be16(u16 v) { return (u16)(v << 8 | v >> 8); }
static u32 be32(u32 v) { return __builtin_bswap32(v); }

/* an SCE kernel error (0x8002xxxx) as -errno */
static long sce(long rc) { return rc < 0 ? -(long)(rc & 0xFFFF) : rc; }

/* a BSD call's -1 as -errno */
static long bsd(long rc)
{
    if (rc >= 0)
        return rc;
    int e = K.errno_fn ? *K.errno_fn() : 0;
    return -(long)(e ? e : FV_EIO);
}

/* ---- the environment ---- */

void *fv_alloc(unsigned long size) { return page_alloc(size); } /* zeroed */
void fv_free(void *p, unsigned long size) { page_free(p, size); }
void fv_sleep_ms(unsigned ms) { K.usleep(ms * 1000); }

void fv_log(const char *f, ...)
{
    char line[600];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    vfmt(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    logf("%s", line);
}

int fv_thread(void *(*fn)(void *), void *arg, const char *name)
{
    u8 attr[0x40];
    u64 tid = 0;
    K.pthread_attr_init(attr);
    K.pthread_attr_setstacksize(attr, 64 << 10);
    int rc = K.pthread_create(&tid, attr, fn, arg, name);
    K.pthread_attr_destroy(attr);
    if (rc != 0)
        return -FV_EAGAIN;
    if (S.detach)
        S.detach(tid);
    return 0;
}

long fv_now(void)
{
    s64 tv[2] = {0, 0};
    return S.gettimeofday && S.gettimeofday(tv) == 0 ? tv[0] : 0;
}

long fv_clock_ms(void) { return S.process_time ? (long)(S.process_time() / 1000) : 0; }

int fv_open(const char *path, int flags, int mode) { return (int)sce(K.open(path, flags, mode)); }
long fv_read(int fd, void *buf, unsigned long n) { return sce(K.read(fd, buf, n)); }
long fv_write(int fd, const void *buf, unsigned long n) { return sce(K.write(fd, buf, n)); }
long fv_seek(int fd, long off, int whence) { return sce(K.lseek(fd, off, whence)); }
int fv_close(int fd) { return (int)sce(K.close(fd)); }
int fv_mkdir(const char *path) { return (int)sce(S.mkdir(path, 0777)); }
int fv_rmdir(const char *path) { return (int)sce(S.rmdir(path)); }
int fv_unlink(const char *path) { return (int)sce(S.unlink(path)); }
int fv_rename(const char *from, const char *to) { return (int)sce(S.rename(from, to)); }

int fv_stat(const char *path, struct fv_stat *st)
{
    u8 b[0x78]; /* FreeBSD 9's struct stat */
    long rc = sce(K.stat(path, b));
    if (rc < 0)
        return (int)rc;
    st->mode = *(u16 *)(b + 8);
    st->mtime = *(s64 *)(b + 0x28);
    st->size = *(s64 *)(b + 0x48);
    return 0;
}

int fv_opendir(const char *path, struct fv_dir *d)
{
    long fd = sce(K.open(path, O_RDONLY | O_DIRECTORY, 0));
    if (fd < 0)
        return (int)fd;
    d->h = fd;
    d->pos = d->len = 0;
    return 0;
}

int fv_readdir(struct fv_dir *d, char *name, unsigned size)
{
    for (;;) {
        if (d->pos >= d->len) {
            long n = sce(S.getdents((int)d->h, d->buf, sizeof(d->buf)));
            if (n <= 0)
                return (int)n;
            d->len = (unsigned)n;
            d->pos = 0;
        }
        const u8 *e = d->buf + d->pos; /* fileno u32, reclen u16, type u8, namlen u8, name */
        u16 reclen = *(const u16 *)(e + 4);
        if (reclen < 8 || d->pos + reclen > d->len) {
            d->pos = d->len;
            continue;
        }
        d->pos += reclen;
        unsigned n = e[7];
        const char *nm = (const char *)e + 8;
        if (!*(const u32 *)e || !n || (n == 1 && nm[0] == '.') || (n == 2 && nm[0] == '.' && nm[1] == '.'))
            continue;
        if (n > size - 1)
            n = size - 1;
        memcpy(name, nm, n);
        name[n] = 0;
        return 1;
    }
}

void fv_closedir(struct fv_dir *d) { K.close((int)d->h); }

static void nosigpipe(int fd)
{
    int one = 1;
    S.setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, 4);
}

#define SYS_socket 97
#define SYS_close 6

/* A socket the JIT compiler process makes and hands over: fd, or -errno.
 * The compiler's channel is one at a time (rpc_lock): title threads never
 * call this while Luac0re's Lua may use the compiler, only ftp_start on the
 * boot thread and, with no title, the ELF loader's payloads (elfldr_sys.c),
 * whose service holds the boot thread so no Lua runs. */
int jit_socket(int domain, int type, int proto) HIDDEN;
int jit_socket(int domain, int type, int proto)
{
    if (!g_ctx->jit_syscall)
        return -FV_ENOSYS;
    rpc_lock();
    long cfd = (long)jit_call(g_ctx->jit_syscall, SYS_socket, (u64)domain, (u64)type, (u64)proto, 0, 0, 0);
    int fd = cfd >= 0 ? jit_recv_fd((int)cfd) : -1;
    if (cfd >= 0)
        jit_call(g_ctx->jit_syscall, SYS_close, (u64)cfd, 0, 0, 0, 0, 0);
    rpc_unlock();
    if (cfd < 0)
        logf("net: the compiler's socket(%d, %d, %d) failed %ld", domain, type, proto, cfd);
    if (fd >= 0)
        nosigpipe(fd);
    return fd >= 0 ? fd : cfd < 0 ? -FV_ENOSYS : -FV_EIO;
}

/* a TCP socket: SWRR's own, else one the JIT compiler process makes (ftp_start
 * only: it runs on the boot thread, before the title) */
static long new_socket(bool boot)
{
    long fd = bsd(S.socket(AF_INET, SOCK_STREAM, 0));
    if (fd != -FV_ENOSYS || !boot || !g_ctx->jit_syscall)
        return fd;
    fd = jit_socket(AF_INET, SOCK_STREAM, 0);
    return fd < 0 ? -FV_ENOSYS : fd;
}

/* The largest send and receive buffers the kernel takes, up to 2 MiB: a
 * connection moves at most a buffer per round trip, and SWRR's defaults
 * (32 KiB to send, 64 KiB to receive) held one connection to a few MiB/s.
 * Set on listening sockets before listen, where the receive buffer sets
 * the window scale their connections offer, and on every connection they
 * accept.  The first of each is logged with what it had. */
static void big_buffers(int fd, const char *what)
{
    static int said;
    int opt[2] = {SO_SNDBUF, SO_RCVBUF}, was[2] = {0, 0}, now[2] = {0, 0};
    for (int k = 0; k < 2; k++) {
        u32 len = 4;
        if (S.getsockopt)
            S.getsockopt(fd, SOL_SOCKET, opt[k], &was[k], &len);
        for (int size = 2 << 20; size >= 64 << 10; size >>= 1)
            if (S.setsockopt(fd, SOL_SOCKET, opt[k], &size, 4) == 0) {
                now[k] = size;
                break;
            }
    }
    int bit = what[0] == 'l' ? 1 : 2;
    if (!(said & bit))
        logf("ftp: %s socket buffers: send %d KiB (had %d), receive %d KiB (had %d)", what, now[0] >> 10,
             was[0] >> 10, now[1] >> 10, was[1] >> 10);
    said |= bit;
}

int fv_listen(unsigned addr, unsigned short port, int backlog)
{
    long fd = new_socket(true);
    if (fd < 0)
        return (int)fd;
    big_buffers((int)fd, "listening");
    int one = 1;
    S.setsockopt((int)fd, SOL_SOCKET, SO_REUSEADDR, &one, 4);
    S.setsockopt((int)fd, SOL_SOCKET, SO_REUSEPORT, &one, 4);
    struct sin a = {16, AF_INET, be16(port), be32(addr), {0}};
    long rc = bsd(S.bind((int)fd, &a, sizeof(a)));
    if (rc == 0)
        rc = bsd(S.listen((int)fd, backlog));
    if (rc < 0) {
        K.close((int)fd);
        return (int)rc;
    }
    return (int)fd;
}

int fv_accept(int fd, int timeout_ms, unsigned *addr, unsigned short *port)
{
    if (timeout_ms >= 0) {
        struct {
            int fd;
            s16 events, revents;
        } p = {fd, POLLIN, 0};
        long n;
        do
            n = bsd(S.poll(&p, 1, timeout_ms));
        while (n == -FV_EINTR);
        if (n == 0)
            return -FV_ETIMEDOUT;
        if (n < 0)
            return (int)n;
    }
    struct sin a;
    u32 len = sizeof(a);
    long c;
    do
        c = bsd(S.accept(fd, &a, &len));
    while (c == -FV_EINTR);
    if (c < 0)
        return (int)c;
    nosigpipe((int)c);
    big_buffers((int)c, "accepted");
    if (addr)
        *addr = be32(a.addr);
    if (port)
        *port = be16(a.port);
    return (int)c;
}

int fv_connect(unsigned addr, unsigned short port)
{
    long fd = new_socket(false);
    if (fd < 0)
        return (int)fd;
    nosigpipe((int)fd);
    struct sin a = {16, AF_INET, be16(port), be32(addr), {0}};
    long rc = bsd(S.connect((int)fd, &a, sizeof(a)));
    if (rc < 0) {
        K.close((int)fd);
        return (int)rc;
    }
    return (int)fd;
}

int fv_sockname(int fd, unsigned *addr, unsigned short *port)
{
    struct sin a;
    u32 len = sizeof(a);
    long rc = bsd(S.getsockname(fd, &a, &len));
    if (rc < 0)
        return (int)rc;
    *addr = be32(a.addr);
    *port = be16(a.port);
    return 0;
}

int fv_timeouts(int fd, int seconds)
{
    s64 tv[2] = {seconds, 0};
    S.setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, tv, sizeof(tv));
    return (int)bsd(S.setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, tv, sizeof(tv)));
}

long fv_send(int fd, const void *buf, unsigned long n)
{
    unsigned long done = 0;
    while (done < n) {
        long w = bsd(K.sendto(fd, (const u8 *)buf + done, n - done, 0, NULL, 0));
        if (w == -FV_EINTR)
            continue;
        if (w <= 0)
            return w < 0 ? w : -FV_EIO;
        done += (unsigned long)w;
    }
    return (long)n;
}

long fv_recv(int fd, void *buf, unsigned long n)
{
    long r;
    do
        r = bsd(S.recvfrom(fd, buf, n, 0, NULL, NULL));
    while (r == -FV_EINTR);
    return r;
}

void fv_sclose(int fd)
{
    if (S.shutdown)
        S.shutdown(fd, 2); /* SHUT_RDWR: the port can be bound again at once */
    K.close(fd);
}

const char *fv_home(void) { return usb_ftp_home(); }

/* ---- start ---- */

#define SYM(field, name)                                                       \
    do {                                                                       \
        *(void **)&S.field = host_dlsym((u32)g_ctx->libkernel_handle, name);   \
        if (!S.field)                                                          \
            missing = missing ? missing : name;                                \
    } while (0)

/* libkernel's socket and file calls for ftp.c and elfldr.c, and SIGPIPE
 * ignored: 0, or -1 (logged) */
int net_setup(void) HIDDEN;
int net_setup(void)
{
    static int done;
    if (done)
        return done > 0 ? 0 : -1;
    const char *missing = NULL;
    SYM(socket, "socket");
    SYM(bind, "bind");
    SYM(listen, "listen");
    SYM(accept, "accept");
    SYM(connect, "connect");
    SYM(setsockopt, "setsockopt");
    SYM(getsockname, "getsockname");
    SYM(recvfrom, "recvfrom");
    SYM(poll, "poll");
    SYM(getdents, "sceKernelGetdents");
    SYM(mkdir, "sceKernelMkdir");
    SYM(rmdir, "sceKernelRmdir");
    SYM(unlink, "sceKernelUnlink");
    SYM(rename, "sceKernelRename");
    if (missing || !K.sendto) {
        logf("net: libkernel has no %s", missing ? missing : "sendto");
        done = -1;
        return -1;
    }
    *(void **)&S.shutdown = host_dlsym((u32)g_ctx->libkernel_handle, "shutdown");
    *(void **)&S.getsockopt = host_dlsym((u32)g_ctx->libkernel_handle, "getsockopt");
    *(void **)&S.gettimeofday = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelGettimeofday");
    *(void **)&S.process_time = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelGetProcessTime");
    *(void **)&S.detach = host_dlsym((u32)g_ctx->libkernel_handle, "scePthreadDetach");
    *(void **)&S.sigaction = host_dlsym((u32)g_ctx->libkernel_handle, "sigaction");
    if (S.sigaction) {
        struct {
            u64 handler;
            int flags;
            u32 mask[4];
        } sa = {1 /* SIG_IGN */, 0, {0}};
        if (S.sigaction(SIGPIPE, &sa, NULL) != 0)
            logf("net: SIGPIPE stays as it was");
    }
    done = 1;
    return 0;
}

void ftp_title_starting(void) HIDDEN;
void ftp_title_starting(void)
{
    u32 port = CTX_FTP_PORT(g_ctx->flags);
    if (!port) {
        logf("ftp: no FTP server (run.py --ftp-port 0)");
        return;
    }
    if (net_setup() != 0) {
        logf("ftp: no FTP server");
        return;
    }
    usb_ftp_prepare();
    ftp_start((unsigned short)port);
}
