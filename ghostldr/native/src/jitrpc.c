/*
 * Calls into the ps2emu JIT-compiler process, ported from Luac0re 2.4b
 * lua/jit/jit_rop.lua (MIT, Gezine).  After jit_init() the compiler spins on
 * a "pop rsp" loop in bridge memory that both processes map at the same
 * address.  We write a chain that loads six arguments + rax, calls the target,
 * stores rax to oob_scratch + 0x2000, restores the loop, and then break the
 * loop by writing the chain start into the loop's rsp slot.
 *
 * Only one caller may use the compiler at a time (Lua must stay idle).
 */
#include "ghost.h"

#define SCRATCH_RET 0x2000
/* Private area of the shared OOB scratch used for our compiler-visible
 * buffers (Luac0re itself uses +0x0..+0x62000 and +0x30000 strings). */
#define PRIV 0x70000

static inline void w64(u64 addr, u64 v) { *(volatile u64 *)addr = v; }
static inline u64 r64(u64 addr) { return *(volatile u64 *)addr; }

static int chain_idx;
static u64 push_chain(u64 v)
{
    u64 slot = g_ctx->fake_rsp + 0x1A0 + (u64)chain_idx * 8;
    chain_idx++;
    if (chain_idx == 66) {
        /* final write releases the compiler: keep it ordered last */
#if defined(__x86_64__)
        __asm__ volatile("mfence" ::: "memory");
#else
        __atomic_thread_fence(__ATOMIC_SEQ_CST); /* tests/native_host/jitrpc_host.c */
#endif
    }
    w64(slot, v);
    return slot;
}

/* PS4: a call the compiler does not answer is given up on after this long,
 * and the compiler is not used again (it may still run the old chain).  The
 * spin limit below is no clock there: a PS4's usleep(50) lasts a scheduler
 * tick (~1 ms), so 2,000,000 spins are half an hour or more, not ~100 s. */
#define PS4_JIT_GIVE_UP_S 30
static bool g_jit_stuck;

static u64 now_us(void)
{
    static u64 (*ptime)(void);
    static bool looked;
    if (!looked)
        ptime = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelGetProcessTime"), looked = true;
    return ptime ? ptime() : 0;
}

/* PS4: true when the compiler has had long enough; logs while it waits */
GHOST_COLD static bool ps4_given_up(u64 fn, u64 rax, u64 *t0, u32 *said)
{
    u64 t = now_us();
    if (!t)
        return false; /* no clock: the spin limit still applies */
    if (!*t0)
        *t0 = t;
    u64 s = (t - *t0) / 1000000;
    if ((s >= 2 && *said == 0) || (s >= 10 && *said == 1)) {
        (*said)++;
        logf("jit_call: fn %lx rax %lx: no answer from the compiler after %lu s", fn, rax, s);
    }
    if (s < PS4_JIT_GIVE_UP_S)
        return false;
    logf("jit_call: fn %lx rax %lx: no answer from the compiler in %lu s: giving up on it, no further compiler calls",
         fn, rax, s);
    g_jit_stuck = true;
    return true;
}

u64 jit_call(u64 fn, u64 rax, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6)
{
    struct boot_ctx *c = g_ctx;
    if (g_jit_stuck) { /* PS4 only: a call before this one never came back */
        logf("jit_call: fn %lx rax %lx skipped: the compiler stopped answering earlier", fn, rax);
        return (u64)-1;
    }
    chain_idx = 0;
    for (int i = 0; i < 6; i++)
        push_chain(c->rop_ret);

    u64 temp_r13 = ((c->fake_rsp - 0x20000 - c->rop_ret) / 4) + 1;
    w64(temp_r13 * 4 + c->rop_ret, 0);

    /* r8 = a5 (the mov-r8 gadget ends in "jmp rax + [rax + r13*4]") */
    push_chain(c->rop_pop_r12_r15_rbp);
    push_chain(c->leak_r12);
    push_chain(temp_r13);
    push_chain(c->leak_r14);
    push_chain(c->leak_r15);
    push_chain(c->leak_rbp);
    push_chain(c->rop_pop_rdi);
    push_chain(c->rop_ret);
    push_chain(c->rop_mov_rax_rdi);
    push_chain(c->rop_pop_rbx);
    push_chain(a5);
    push_chain(c->rop_mov_r8_rbx);
    /* r9 = a6 */
    push_chain(c->rop_pop_r12_r15_rbp);
    push_chain(c->leak_r12);
    push_chain(temp_r13);
    push_chain(c->leak_r14);
    push_chain(c->leak_r15);
    push_chain(c->leak_rbp);
    push_chain(c->rop_pop_rdi);
    push_chain(c->rop_ret);
    push_chain(c->rop_mov_rax_rdi);
    push_chain(c->rop_pop_rbx);
    push_chain(a6);
    push_chain(c->rop_mov_r9_rbx);
    /* rax, rdx, rdi, rsi, rcx */
    push_chain(c->rop_pop_rdi);
    push_chain(rax);
    push_chain(c->rop_mov_rax_rdi);
    push_chain(c->rop_pop_rdi);
    push_chain(a3);
    push_chain(c->rop_mov_rdx_rdi);
    push_chain(c->rop_pop_rdi);
    push_chain(a1);
    push_chain(c->rop_pop_rsi);
    push_chain(a2);
    push_chain(c->rop_pop_rcx);
    push_chain(a4);
    push_chain(fn);
    /* store rax */
    push_chain(c->rop_pop_rbx);
    push_chain(c->oob_scratch + SCRATCH_RET - 8);
    push_chain(c->rop_mov_deref_rbx_rax);
    push_chain(c->leak_rbx);
    push_chain(c->leak_r14);
    push_chain(c->leak_rbp);
    push_chain(c->rop_pop_r12_r15_rbp);
    push_chain(c->leak_r12);
    push_chain(c->leak_r13);
    push_chain(c->leak_r14);
    push_chain(c->leak_r15);
    push_chain(c->leak_rbp);
    /* restore the spin loop: [chain_end + 8] = chain_end */
    push_chain(c->rop_pop_rdi);
    push_chain(c->chain_end);
    push_chain(c->rop_mov_rax_rdi);
    push_chain(c->rop_pop_rbx);
    push_chain(c->chain_end);
    push_chain(c->rop_mov_deref_rbx_rax);
    push_chain(c->leak_rbx);
    push_chain(c->leak_r14);
    push_chain(c->leak_rbp);
    /* slots 64/65 overlay the spin loop: break it */
    push_chain(c->rop_pop_rsp);
    push_chain(c->chain_start);

    u64 t0 = 0;
    u32 said = 0;
    for (u64 spins = 0; r64(c->chain_end + 8) != c->chain_end; spins++) {
        if (spins > 2000000) {
            logf("jit_call: timeout fn=%lx", fn);
            g_jit_stuck = is_ps4();
            return (u64)-1;
        }
        if (spins > 64) {
            K.usleep(50);
            if (is_ps4() && ps4_given_up(fn, rax, &t0, &said))
                return (u64)-1;
        }
    }
    return r64(c->oob_scratch + SCRATCH_RET);
}

static u64 priv(u64 off) { return g_ctx->oob_scratch + PRIV + off; }

u64 jit_read64(u64 addr)
{
    jit_call(g_ctx->jit_memcpy, 0, priv(0xF00), addr, 8, 0, 0, 0);
    return r64(priv(0xF00));
}

int jit_copy_to(u64 dst, const void *src, size_t len)
{
    const u8 *s = src;
    while (len) {
        size_t n = len < g_ctx->staging_size ? len : g_ctx->staging_size;
        memcpy((void *)g_ctx->staging, s, n);
        jit_call(g_ctx->jit_memcpy, 0, dst, g_ctx->staging, n, 0, 0, 0);
        dst += n, s += n, len -= n;
    }
    return 0;
}

#define SOL_SOCKET 0xffff
#define SCM_RIGHTS 1
#define SYS_sendmsg 28
#define MSG_DONTWAIT 0x80
#define EAGAIN 35

/* PS4: receive on this process's end of the socket pair without blocking
 * (Luac0re never uses the pair there itself).  The compiler's system call
 * has returned by now, so what it sent is queued already: up to `tries` ms
 * of waiting covers nothing else.  Bytes received, or -1 (errno set). */
GHOST_COLD static ssize_t sock_recv(void *msg, u32 tries)
{
    for (u32 i = 0;; i++) {
        ssize_t r = K.recvmsg((int)g_ctx->new_main_sock, msg, MSG_DONTWAIT);
        if (r >= 0 || i >= tries || *K.errno_fn() != EAGAIN)
            return r;
        K.usleep(1000);
    }
}

GHOST_COLD static ssize_t sock_read(void *buf, u32 n, u32 tries)
{
    u64 iov[2] = {(u64)buf, n};
    u8 msg[0x30];
    memset(msg, 0, sizeof(msg));
    *(u64 *)(msg + 0x10) = (u64)iov;
    *(u32 *)(msg + 0x18) = 1;
    return sock_recv(msg, tries);
}

/* Move a descriptor from the compiler process into this one. */
int jit_recv_fd(int compiler_fd)
{
    u64 ctl = priv(0x000), data = priv(0x040), iov = priv(0x060), msg = priv(0x080);
    if (!g_ctx->jit_syscall) { /* an old ghostldr.lua on a PS4: Luac0re has no syscall address there */
        logf("jit_recv_fd: no syscall gadget in the compiler (ghostldr.lua predates PS4 support)");
        return -1;
    }
    memset((void *)priv(0), 0, 0x100);
    *(u32 *)(ctl + 0) = 20;
    *(u32 *)(ctl + 4) = SOL_SOCKET;
    *(u32 *)(ctl + 8) = SCM_RIGHTS;
    *(u32 *)(ctl + 16) = (u32)compiler_fd;
    *(u8 *)data = 0;
    w64(iov, data);
    w64(iov + 8, 1);
    w64(msg + 0x10, iov);
    *(u32 *)(msg + 0x18) = 1;
    w64(msg + 0x20, ctl);
    *(u32 *)(msg + 0x28) = 20;
    s64 rc = (s64)jit_call(g_ctx->jit_syscall, SYS_sendmsg, g_ctx->new_jit_sock, msg, 0, 0, 0, 0);
    if (rc < 0) {
        logf("jit_recv_fd: compiler sendmsg failed %ld", rc);
        return -1;
    }
    u8 rctl[32], rdata[4];
    u64 riov[2] = {(u64)rdata, 1};
    u8 rmsg[0x30];
    memset(rctl, 0, sizeof(rctl));
    memset(rmsg, 0, sizeof(rmsg));
    *(u64 *)(rmsg + 0x10) = (u64)riov;
    *(u32 *)(rmsg + 0x18) = 1;
    *(u64 *)(rmsg + 0x20) = (u64)rctl;
    *(u32 *)(rmsg + 0x28) = sizeof(rctl);
    ssize_t got = is_ps4() ? sock_recv(rmsg, 1000) : K.recvmsg((int)g_ctx->new_main_sock, rmsg, 0);
    if (got < 0 || *(u32 *)(rctl + 8) != SCM_RIGHTS) {
        logf("jit_recv_fd: recvmsg failed %ld errno %d", got, *K.errno_fn());
        return -1;
    }
    return *(int *)(rctl + 16);
}

int jit_block_create(struct jit_block *b, u64 size, u64 want_rx)
{
    memset(b, 0, sizeof(*b));
    b->base_fd = b->rw_fd = b->rx_fd_compiler = b->rx_fd = -1;
    size = ALIGN_UP(size, 0x4000);
    b->size = size;
    u64 name = priv(0x200), out = priv(0x240);
    strcpy((char *)name, "ghostldr");
    w64(out, (u64)-1);
    int rc = (int)jit_call(g_ctx->jit_shm_create, 0, name, size, 7, out, 0, 0);
    b->base_fd = rc == 0 ? (int)r64(out) : -1;
    if (rc != 0) {
        logf("jit_block: create(%lx) rc=%x", size, rc);
        return -1;
    }
    rc = (int)jit_call(g_ctx->jit_shm_alias, 0, (u64)b->base_fd, 3, out, 0, 0, 0);
    b->rw_fd = rc == 0 ? (int)r64(out) : -1;
    int rc2 = (int)jit_call(g_ctx->jit_shm_alias, 0, (u64)b->base_fd, 5, out, 0, 0, 0);
    b->rx_fd_compiler = rc2 == 0 ? (int)r64(out) : -1;
    if (rc || rc2) {
        logf("jit_block: alias rc=%x/%x", rc, rc2);
        return -1;
    }
    w64(out, 0);
    rc = (int)jit_call(g_ctx->jit_shm_map, 0, (u64)b->rw_fd, 3, out, 0, 0, 0);
    b->rw = r64(out);
    if (rc != 0 || !b->rw) {
        logf("jit_block: compiler RW map rc=%x", rc);
        return -1;
    }
    b->rx_fd = jit_recv_fd(b->rx_fd_compiler);
    if (b->rx_fd < 0)
        return -1;
    void *addr = (void *)want_rx;
    if (want_rx) {
        /* only there: sceKernelJitMapSharedMemory places it at its home
         * address over whatever is mapped there */
        void *res = NULL;
        rc = K.mmap(addr, size, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED, b->rx_fd, 0, &res);
        logf("jit_block: mmap fixed %lx rc=%x res=%p", want_rx, rc, res);
        if (rc == 0 && (u64)res == want_rx) {
            b->rx = want_rx;
            return 0;
        }
        return -1;
    }
    rc = K.jit_map(b->rx_fd, 5, &addr);
    logf("jit_block: jit_map rc=%x addr=%p (wanted %lx)", rc, addr, want_rx);
    if (rc != 0)
        return -1;
    b->rx = (u64)addr;
    return 0;
}

int jit_block_write(struct jit_block *b, u64 offset, const void *src, size_t len)
{
    if (offset + len > b->size)
        return -1;
    return jit_copy_to(b->rw + offset, src, len);
}

#define SYS_write 4
#define SYS_close 6
#define SYS_munmap 73
#define SYS_clock_gettime 232

/* Reads and writes in the compiler that cannot crash it: a system call
 * that meets an address it may not touch returns EFAULT instead.
 * sceKernelVirtualQuery cannot answer this: in the compiler it lists none
 * of the JIT mappings (a 13.60 PS5 shows 25 entries, no bridge, no buffer).
 *
 * jit_peek: the compiler write()s n bytes at addr into its end of the
 * socket pair Luac0re connected; they are read here, without blocking
 * (a blocking read waited forever when they did not come).  0, or -1. */
GHOST_COLD int jit_peek(u64 addr, void *out, u32 n)
{
    struct boot_ctx *c = g_ctx;
    if (!c->jit_syscall)
        return -1;
    u8 junk[64];
    u64 stale = 0;
    for (ssize_t r; (r = sock_read(junk, sizeof(junk), 0)) > 0;)
        stale += (u64)r;
    if (stale)
        logf("jit_peek: dropped %lu stale bytes from the socket pair", stale);
    s64 rc = (s64)jit_call(c->jit_syscall, SYS_write, c->new_jit_sock, addr, n, 0, 0, 0);
    if (rc != (s64)n) {
        if (rc != -1)
            logf("jit_peek: the compiler's write() of %lx returned %ld", addr, rc);
        return -1;
    }
    for (u32 got = 0; got < n;) {
        ssize_t r = sock_read((u8 *)out + got, n - got, 200);
        if (r <= 0) {
            logf("jit_peek: the compiler wrote %u bytes from %lx to its socket (fd %ld), %u arrived here (fd %ld): "
                 "rc %ld errno %d",
                 n, addr, c->new_jit_sock, got, c->new_main_sock, (s64)r, *K.errno_fn());
            return -1;
        }
        got += (u32)r;
    }
    return 0;
}

/* jit_stamp: the compiler's clock_gettime() writes 16 bytes (the time) at
 * addr.  0 when it could write there, else -1. */
GHOST_COLD int jit_stamp(u64 addr)
{
    if (!g_ctx->jit_syscall)
        return -1;
    return (s64)jit_call(g_ctx->jit_syscall, SYS_clock_gettime, 0 /* CLOCK_REALTIME */, addr, 0, 0, 0, 0) == 0 ? 0
                                                                                                           : -1;
}

#define SYS_getpid 20

/* PS4, before any JIT buffer is probed: the syscall gadget, clock_gettime,
 * the socket pair and the error path, each on memory known to be good (the
 * shared scratch) or known to be absent (the first page).  Each step is
 * logged before it runs, so the last line of a log that stops names it.
 * 0 when all four work. */
GHOST_COLD int jit_ps4_check(void)
{
    struct boot_ctx *c = g_ctx;
    u64 a = priv(0x300), b = priv(0x340);
    u8 got[16];
    logf("jit: PS4 check: syscall gadget %lx, socket pair: compiler fd %ld, here fd %ld", c->jit_syscall,
         c->new_jit_sock, c->new_main_sock);
    if (!c->jit_syscall)
        return -1;
    logf("jit: PS4 check 1/4: getpid() in the compiler");
    s64 pid = (s64)jit_call(c->jit_syscall, SYS_getpid, 0, 0, 0, 0, 0, 0);
    logf("jit: PS4 check 1/4: compiler pid %ld (this process %d)", pid, K.getpid());
    if (pid <= 0)
        return -1;
    logf("jit: PS4 check 2/4: clock_gettime() into the shared scratch %lx", b);
    memset((void *)b, 0, 16);
    int st = jit_stamp(b);
    u64 sec = r64(b), nsec = r64(b + 8);
    logf("jit: PS4 check 2/4: rc %d, seen here %lu s %lu ns", st, sec, nsec);
    if (st != 0 || !sec || nsec >= 1000000000)
        return -1;
    logf("jit: PS4 check 3/4: write() of the shared scratch %lx to the socket pair", a);
    for (u32 i = 0; i < 16; i++)
        ((u8 *)a)[i] = (u8)(0xA5 ^ (i * 17));
    int pk = jit_peek(a, got, 16);
    logf("jit: PS4 check 3/4: rc %d, %s", pk, pk ? "nothing read" : memcmp(got, (void *)a, 16) ? "OTHER bytes" : "same bytes");
    if (pk != 0 || memcmp(got, (void *)a, 16) != 0)
        return -1;
    logf("jit: PS4 check 4/4: write() from an unmapped address (EFAULT expected)");
    pk = jit_peek(0x1000, got, 16);
    logf("jit: PS4 check 4/4: %s", pk ? "refused, as expected" : "it read something there");
    return 0;
}

/* Undo jit_block_create, also a partial one: the RX view and descriptor
 * here, the RW view and the three descriptors in the compiler. */
GHOST_COLD void jit_block_destroy(struct jit_block *b)
{
    if (b->rx)
        K.munmap((void *)b->rx, b->size);
    if (b->rx_fd >= 0)
        K.close(b->rx_fd);
    if (g_ctx->jit_syscall) {
        if (b->rw)
            jit_call(g_ctx->jit_syscall, SYS_munmap, b->rw, b->size, 0, 0, 0, 0);
        int fds[3] = {b->rx_fd_compiler, b->rw_fd, b->base_fd};
        for (int i = 0; i < 3; i++)
            if (fds[i] >= 0)
                jit_call(g_ctx->jit_syscall, SYS_close, (u64)fds[i], 0, 0, 0, 0, 0);
    } else if (b->rw || b->base_fd >= 0) {
        logf("jit_block: no syscall gadget: the compiler keeps %lx bytes at %lx", b->size, b->rw);
    }
    memset(b, 0, sizeof(*b));
    b->base_fd = b->rw_fd = b->rx_fd_compiler = b->rx_fd = -1;
}

/* The compiler process's own map: sceKernelVirtualQuery run there (ctx
 * reserved[0]), find-next from lo, each answer copied out of the shared
 * scratch.  Only reads. */
GHOST_COLD u32 jit_vmap(u64 lo, u64 hi, struct vmq *out, u32 max)
{
    struct vmq *q = (struct vmq *)priv(0x1000);
    u32 n = 0;
    for (u64 addr = lo; g_ctx->reserved[0] && n < max && addr < hi;) {
        memset(q, 0, sizeof(*q));
        if ((int)jit_call(g_ctx->reserved[0], 0, addr, 1 /* find next */, (u64)q, sizeof(*q), 0, 0) != 0 ||
            q->start >= hi || q->end <= addr)
            break;
        out[n++] = *q;
        addr = q->end;
    }
    return n;
}
