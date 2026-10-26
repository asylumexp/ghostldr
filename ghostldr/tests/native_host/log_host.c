/*
 * The core's log.c in a host process: with an address in the boot context
 * (run.py) each line is a datagram to it; with none (the release's
 * ghostldr.lua, CTX_FLAG_BUNDLE) log_fd is the bundle's connection.  A send
 * that finds the connection gone ends it: closed once, log_fd -1, nothing
 * sent after; an fd that is not ours any more is let go but not closed;
 * EINTR is retried; ghostldr.lua takes it back the same way.  test_bundle.py builds and
 * runs it; it prints "ok ..." / "FAILED ..." lines and exits 0 when all pass.
 *
 *     log_host
 */
#include "ghost.h"

/* libc, declared here to stay out of its headers */
int socketpair(int domain, int type, int proto, int *fds);
long sendto(int fd, const void *buf, unsigned long n, int flags, const void *addr, unsigned alen);
long recv(int fd, void *buf, unsigned long n, int flags);
int close(int fd);
void *signal(int sig, void *handler);
int vsnprintf(char *buf, unsigned long size, const char *fmt, __builtin_va_list ap);
int printf(const char *fmt, ...);
int pthread_create(u64 *tid, const void *attr, void *(*fn)(void *), void *arg);
int pthread_join(u64 tid, void **ret);

struct kfuncs K;
struct boot_ctx *g_ctx;

int vfmt(char *buf, size_t size, const char *f, __builtin_va_list ap) { return vsnprintf(buf, size, f, ap); }
int fmt(char *buf, size_t size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(buf, size, f, ap);
    __builtin_va_end(ap);
    return n;
}

static int g_failed;
static void expect(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "ok" : "FAILED", what);
    g_failed |= !ok;
}

/* a fake sendto: what it was asked, and what it answers */
static struct {
    int fd, flags, calls;
    const void *addr;
    u32 alen;
    char line[1100];
    s64 answer; /* 0: the length */
} S;
static volatile int g_closes, g_closed_fd;

static ssize_t fake_sendto(int fd, const void *buf, size_t n, int flags, const void *addr, u32 alen)
{
    __atomic_fetch_add(&S.calls, 1, __ATOMIC_SEQ_CST);
    S.fd = fd, S.flags = flags, S.addr = addr, S.alen = alen;
    memcpy(S.line, buf, n < sizeof(S.line) - 1 ? n : sizeof(S.line) - 1);
    S.line[n < sizeof(S.line) - 1 ? n : sizeof(S.line) - 1] = 0;
    return S.answer ? S.answer : (ssize_t)n;
}
static int fake_close(int fd)
{
    __atomic_fetch_add(&g_closes, 1, __ATOMIC_SEQ_CST);
    g_closed_fd = fd;
    return 0;
}
static int fake_getpid(void) { return 96; }
static int g_errno, g_eintr_left; /* the fake's errno; EINTRs before a send goes through */
static int *fake_errno(void) { return &g_errno; }
static ssize_t eintr_sendto(int fd, const void *buf, size_t n, int flags, const void *addr, u32 alen)
{
    if (g_eintr_left-- > 0) {
        g_errno = 4;
        __atomic_fetch_add(&S.calls, 1, __ATOMIC_SEQ_CST);
        return -1;
    }
    return fake_sendto(fd, buf, n, flags, addr, alen);
}

static ssize_t real_sendto(int fd, const void *buf, size_t n, int flags, const void *addr, u32 alen)
{
    return sendto(fd, buf, n, flags, addr, alen);
}
static int real_close(int fd) { return close(fd); }

static void *hammer(void *arg)
{
    for (int i = 0; i < 200; i++)
        logf("thread %ld line %d", (long)arg, i);
    return NULL;
}

int main(void)
{
    static struct boot_ctx ctx;
    static u8 sa[16] = {0, 2, 0x47, 0x12, 192, 0, 2, 1};
    g_ctx = &ctx;
    K.getpid = fake_getpid;
    K.sendto = fake_sendto;
    K.close = fake_close;
    K.errno_fn = fake_errno;

    /* run.py: datagrams to the computer; a failed one changes nothing */
    ctx.log_fd = 5, ctx.log_addr = (u64)sa;
    logf("hello %d", 1);
    expect(S.calls == 1 && S.fd == 5 && S.addr == sa && S.alen == 16, "UDP: each line to the log address");
    expect(strcmp(S.line, "[ghostldr 96] hello 1\n") == 0, "UDP: the line as before");
    S.answer = -1;
    logf("lost");
    logf("next");
    expect(S.calls == 3 && ctx.log_fd == 5 && g_closes == 0, "UDP: a failed datagram keeps the socket");

    /* the bundle's connection: no address */
    S.answer = 0, S.calls = 0;
    ctx.log_fd = 9, ctx.log_addr = 0;
    logf("on the connection");
    expect(S.calls == 1 && S.fd == 9 && !S.addr && S.alen == 0, "TCP: sent with no address");
    S.answer = -1, g_errno = 32; /* EPIPE */
    logf("the sender has gone");
    expect((s64)ctx.log_fd == -1 && g_closes == 1 && g_closed_fd == 9, "TCP: a failed send closes it, log_fd -1");
    logf("after");
    expect(S.calls == 2 && g_closes == 1, "TCP: nothing sent after");

    /* a transient failure costs a line, not the connection */
    S.calls = 0, g_closes = 0, g_errno = 55; /* ENOBUFS */
    ctx.log_fd = 9;
    logf("dropped");
    expect(ctx.log_fd == 9 && g_closes == 0 && S.calls == 1, "TCP: ENOBUFS drops the line, keeps the connection");
    /* EINTR: sent again */
    S.answer = 0, S.calls = 0;
    K.sendto = eintr_sendto, g_eintr_left = 2;
    logf("interrupted");
    expect(ctx.log_fd == 9 && S.calls == 3 && strcmp(S.line, "[ghostldr 96] interrupted\n") == 0,
           "TCP: EINTR is retried");
    K.sendto = fake_sendto;
    /* the title closed the fd (EBADF): let go, never closed (the number may be the title's now) */
    S.answer = -1, g_errno = 9;
    logf("bad fd");
    expect((s64)ctx.log_fd == -1 && g_closes == 0, "TCP: EBADF lets it go without closing");
    /* the sender stopped reading for the send timeout (EAGAIN) */
    ctx.log_fd = 10, g_errno = 35;
    logf("stalled");
    expect((s64)ctx.log_fd == -1 && g_closes == 1 && g_closed_fd == 10, "TCP: the send timeout ends it");
    g_errno = 32;

    /* many threads failing at once: closed once */
    S.calls = 0, g_closes = 0;
    ctx.log_fd = 11;
    u64 tid[8];
    for (long i = 0; i < 8; i++)
        pthread_create(&tid[i], NULL, hammer, (void *)i);
    for (int i = 0; i < 8; i++)
        pthread_join(tid[i], NULL);
    expect(g_closes == 1 && (s64)ctx.log_fd == -1, "TCP: threads failing together close it once");

    /* ghostldr.lua takes it back: log_fd -1 means no more sends */
    S.answer = 0, S.calls = 0;
    ctx.log_fd = (u64)-1;
    logf("nowhere");
    expect(S.calls == 0, "log_fd -1: nothing sent");

    /* a real connection: the reader gets each line; once it is gone, the
     * next line ends the log (SIGPIPE ignored, as SO_NOSIGPIPE does on the console) */
    signal(13 /* SIGPIPE */, (void *)1 /* SIG_IGN */);
    int sv[2];
    if (socketpair(1 /* AF_UNIX */, 1 /* SOCK_STREAM */, 0, sv) != 0) {
        expect(false, "socketpair");
        return 1;
    }
    K.sendto = real_sendto;
    K.close = real_close;
    ctx.log_fd = (u64)sv[0];
    logf("first line");
    char buf[256] = {0};
    long n = recv(sv[1], buf, sizeof(buf) - 1, 0);
    expect(n > 0 && strcmp(buf, "[ghostldr 96] first line\n") == 0, "socket: the reader gets the line");
    close(sv[1]);
    logf("into the void");
    logf("and again");
    expect((s64)ctx.log_fd == -1, "socket: the reader gone, the log stops");
    expect(close(sv[0]) != 0, "socket: the core closed its end");
    return g_failed;
}
