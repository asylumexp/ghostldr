/*
 * Runs the core's jitrpc.c in a host process (built for this machine) with a
 * fake compiler process: a thread that waits on the chain's spin-loop slot as
 * the compiler's "pop rsp" loop does, reads the call jit_call wrote (target,
 * rax, arguments), performs it and stores rax.  The socket pair is a real
 * one.  test_ps4.py builds and drives it:
 *
 *     jitrpc_host SCENARIO
 *
 * SCENARIO (PS4 flag set unless named ps5-*):
 *   ok       a compiler that answers: jit_ps4_check passes, a peek and a stamp
 *            of a buffer here work, and EFAULT below 64 KiB;
 *   stale    bytes already waiting on the socket pair before a peek;
 *   lost     the compiler's write() returns 16 but the bytes go to another
 *            socket (a blocking read here waited forever);
 *   pipe     this process's descriptor is an empty pipe, not the pair;
 *   hang     the compiler never returns from write(): jit_call gives up (the
 *            process clock runs 1000x fast here) and calls no more;
 *   slow     every call takes ~3 ms (past jit_call's 64 fast spins);
 *   one-stub every number mapped to getpid's stub, as v0.6.0 called them:
 *            the compiler dies at clock_gettime, as on the PS4;
 *   nostubs  no stubs passed (an older ghostldr.lua): nothing is called;
 *   noclock  no clock_gettime stub: the check stops there, nothing dies;
 *   ps5-slow the same as slow without the PS4 flag: the clock is never read.
 * On a PS4 the fake compiler, like the console, runs a system call only from
 * its own number's stub; any other dies ("killed") and never answers again.
 * Prints "check N", "peek N", "clock reads N" and the core's log lines.
 */
#include "ghost.h"
#if defined(__linux__)
#include <sys/socket.h> /* the native struct msghdr for k_recvmsg */
#endif

int printf(const char *f, ...);
int vsnprintf(char *buf, unsigned long size, const char *f, __builtin_va_list ap);
void exit(int code);
void *calloc(size_t n, size_t m);
int usleep(unsigned us);
int getpid(void);
int socketpair(int domain, int type, int proto, int sv[2]);
int pipe(int fd[2]);
ssize_t write(int fd, const void *buf, size_t n);
#if !defined(__linux__) /* <sys/socket.h> declares it there */
ssize_t recvmsg(int fd, void *msg, int flags);
#endif
#if defined(__linux__)
/* glibc's per-thread errno lives behind __errno_location; macOS and the
 * BSDs name the same accessor __error */
int *__errno_location(void);
#define HOST_ERRNO_PTR() __errno_location()
#define HOST_MSG_DONTWAIT 0x40
#else
int *__error(void);
#define HOST_ERRNO_PTR() __error()
#define HOST_MSG_DONTWAIT 0x80
#endif

/* jitrpc.c speaks the console's (FreeBSD) numbers: its MSG_DONTWAIT is
 * 0x80 and its EAGAIN is 35.  BSD hosts share those numbers; Linux calls
 * them 0x40 and 11, and its recvmsg would block instead of returning
 * EAGAIN.  Translate both at this boundary. */
static ssize_t k_recvmsg(int fd, void *msg, int flags)
{
    int host_flags = flags & ~0x80;
    if (flags & 0x80)
        host_flags |= HOST_MSG_DONTWAIT;
#if defined(__linux__)
    /* jitrpc.c also builds its msghdr in the BSD layout (0x30 bytes: iov
     * at +0x10, control at +0x20, u32 lengths at +0x18/+0x28).  Linux's
     * struct msghdr is 0x38 bytes and recvmsg writes msg_flags at +0x30,
     * past the caller's buffer and into its frame (in jit_peek it zeroes
     * the low half of the spilled address argument after a drain).
     * Receive through a native header and copy the results back. */
    struct msghdr h;
    memset(&h, 0, sizeof(h));
    u8 *m = msg;
    h.msg_name = *(void **)(m + 0x00);
    h.msg_namelen = *(u32 *)(m + 0x08);
    h.msg_iov = *(void **)(m + 0x10);
    h.msg_iovlen = *(u32 *)(m + 0x18);
    h.msg_control = *(void **)(m + 0x20);
    h.msg_controllen = *(u32 *)(m + 0x28);
    ssize_t r = recvmsg(fd, &h, host_flags);
    if (r >= 0) {
        *(u32 *)(m + 0x28) = (u32)h.msg_controllen;
        *(u32 *)(m + 0x2c) = (u32)h.msg_flags; /* the BSD flags offset */
    }
    return r;
#else
    return recvmsg(fd, msg, host_flags);
#endif
}

static int *k_errno(void)
{
    static int e;
    e = *HOST_ERRNO_PTR();
#if defined(__linux__)
    if (e == 11) /* Linux EAGAIN */
        e = 35;  /* the console's */
#endif
    return &e;
}
struct timespec_ {
    long sec, nsec;
};
int clock_gettime(int clk, struct timespec_ *ts);
int pthread_create(void **thr, const void *attr, void *(*fn)(void *), void *arg);

struct kfuncs K;
struct boot_ctx *g_ctx;
static struct boot_ctx ctx;

void logf(const char *f, ...)
{
    char line[1000];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    vsnprintf(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    printf("log: %s\n", line);
}

/* sceKernelGetProcessTime, 1000 times fast: jit_call's 30 s are 30 ms here */
static volatile int g_clock_reads;
static u64 fast_process_time(void)
{
    struct timespec_ ts;
    clock_gettime(6 /* CLOCK_MONOTONIC */, &ts);
    g_clock_reads++;
    return ((u64)ts.sec * 1000000 + (u64)ts.nsec / 1000) * 1000;
}

void *host_dlsym(u32 handle, const char *name)
{
    return strcmp(name, "sceKernelGetProcessTime") == 0 ? (void *)fast_process_time : NULL;
}

/* jit_call waits with 50 us sleeps while its process clock runs 1000x
 * fast, so a sleep that returns milliseconds late (a loaded Linux host)
 * moves its give-up warnings by whole fake seconds and the "after 2 s"
 * line the tests assert is never printed; wait short sleeps out against
 * the clock instead of trusting the scheduler's granularity */
static int k_usleep(u32 us)
{
    if (us > 200)
        return usleep(us);
    struct timespec_ ts;
    clock_gettime(0, &ts);
    u64 end = (u64)ts.sec * 1000000 + (u64)ts.nsec / 1000 + us;
    for (;;) {
        clock_gettime(0, &ts);
        if ((u64)ts.sec * 1000000 + (u64)ts.nsec / 1000 >= end)
            return 0;
    }
}

/* ---- the fake compiler ---- */
#define FN_SYSCALL 0x5150000UL /* getpid's stub + 7 on a PS4 */
#define FN_STUB(nr) (0x5160000UL + (u64)(nr) * 0x10) /* PS4: each number's own stub */
#define FN_MEMCPY 0x3e3c000UL
static u64 g_sysw[16];
static const char *g_mode;
static int g_jit_end = -1, g_other = -1; /* the compiler's end of the pair; a socket nobody reads */
static volatile u64 *slot(int i) { return (volatile u64 *)(ctx.fake_rsp + 0x1A0 + (u64)i * 8); }

static s64 compiler_syscall(u64 nr, u64 a1, u64 a2, u64 a3)
{
    switch (nr) {
    case 20: /* getpid */
        return 4242;
    case 4: /* write */
        if (!strcmp(g_mode, "hang"))
            for (;;)
                usleep(1000000);
        if ((int)a1 != g_jit_end)
            return -1;
        if (a2 < 0x10000)
            return -1; /* EFAULT: nothing mapped there */
        return write(!strcmp(g_mode, "lost") ? g_other : g_jit_end, (const void *)a2, a3);
    case 232: { /* clock_gettime */
        if (a2 < 0x10000)
            return -1;
        struct timespec_ ts;
        clock_gettime(0, &ts);
        memcpy((void *)a2, &ts, 16);
        return 0;
    }
    default:
        return -1;
    }
}

static void *compiler(void *unused)
{
    for (;;) {
        if (*slot(65) == ctx.chain_end) { /* the spin loop: [chain_end + 8] = chain_end */
            usleep(20);
            continue;
        }
        if (*slot(64) != ctx.rop_pop_rsp || *slot(65) != ctx.chain_start) {
            printf("BAD chain\n");
            exit(4);
        }
        if (!strcmp(g_mode, "slow") || !strcmp(g_mode, "ps5-slow"))
            usleep(3000);
        u64 fn = *slot(42), rax = *slot(31), a1 = *slot(37), a2 = *slot(39), a3 = *slot(34);
        s64 ret = -1;
        if (fn == FN_SYSCALL && !(ctx.flags & CTX_FLAG_PS4))
            ret = compiler_syscall(rax, a1, a2, a3);
        else if (fn == FN_SYSCALL || (fn >= FN_STUB(0) && fn < FN_STUB(1024))) {
            u64 own = fn == FN_SYSCALL ? 20 : (fn - FN_STUB(0)) / 0x10;
            if (own != rax) {
                printf("compiler: system call %lu through the stub of %lu: killed\n", rax, own);
                for (;;)
                    usleep(1000000);
            }
            ret = compiler_syscall(rax, a1, a2, a3);
        } else if (fn == FN_MEMCPY)
            memcpy((void *)a1, (const void *)a2, a3), ret = (s64)a1;
        *(volatile u64 *)(ctx.oob_scratch + 0x2000) = (u64)ret;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        *slot(65) = ctx.chain_end; /* the chain restores the loop last */
    }
    return NULL;
}

int main(int argc, char **argv)
{
    g_ctx = &ctx;
    g_mode = argv[1];
    ctx.flags = strncmp(g_mode, "ps5", 3) ? CTX_FLAG_PS4 : 0;
    K.usleep = k_usleep;
    K.getpid = getpid;
    K.recvmsg = k_recvmsg;
    K.errno_fn = k_errno;

    /* the shared bridge memory */
    u8 *bridge = calloc(1, 0x200000);
    ctx.rop_ret = 0x1000, ctx.rop_pop_rsp = 0x2000;
    ctx.fake_rsp = ALIGN_UP((u64)bridge + 0x30000, 16);
    ctx.chain_start = ctx.fake_rsp + 0x1A0, ctx.chain_end = ctx.fake_rsp + 0x1A0 + 64 * 8;
    *slot(65) = ctx.chain_end;
    ctx.oob_scratch = (u64)bridge + 0x40000;
    ctx.staging = (u64)bridge + 0x100000, ctx.staging_size = 0x10000;
    ctx.jit_syscall = FN_SYSCALL, ctx.jit_memcpy = FN_MEMCPY;
    static const u64 nrs[] = {20, 4, 6, 28, 73, 97, 232};
    u32 n = 0;
    for (u32 i = 0; (ctx.flags & CTX_FLAG_PS4) && strcmp(g_mode, "nostubs") && i < 7; i++)
        if (strcmp(g_mode, "noclock") || nrs[i] != 232)
            g_sysw[2 * n] = nrs[i], g_sysw[2 * n + 1] = !strcmp(g_mode, "one-stub") ? FN_SYSCALL : FN_STUB(nrs[i]), n++;
    ctx.jit_sysw = n ? (u64)g_sysw : 0;

    int sv[2], ov[2], pv[2];
    if (socketpair(1 /* AF_UNIX */, 1 /* SOCK_STREAM */, 0, sv) || socketpair(1, 1, 0, ov) || pipe(pv)) {
        printf("SKIP no sockets\n");
        return 77;
    }
    g_jit_end = sv[0], g_other = ov[0];
    ctx.new_jit_sock = (u64)sv[0];
    ctx.new_main_sock = (u64)(!strcmp(g_mode, "pipe") ? pv[0] : sv[1]);
    if (!strcmp(g_mode, "stale"))
        write(sv[0], "leftover from an earlier call", 29);
    void *thr;
    pthread_create(&thr, NULL, compiler, NULL);

    printf("check %d\n", jit_ps4_check());
    /* a buffer as arena.c probes it: the same memory in both processes */
    u8 *buf = calloc(1, 0x10000), got[16];
    for (int i = 0; i < 16; i++)
        buf[0x4000 + i] = (u8)(i * 7 + 1);
    int pk = jit_peek((u64)buf + 0x4000, got, 16);
    printf("peek %d %s\n", pk, pk ? "-" : memcmp(got, buf + 0x4000, 16) ? "other" : "same");
    printf("stamp %d\n", jit_stamp((u64)buf + 0x8000));
    printf("clock reads %d\n", g_clock_reads);
    return 0;
}
