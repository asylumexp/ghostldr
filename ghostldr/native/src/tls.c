/*
 * Thread-local storage for loaded modules (kernel-modules.md §7).
 *
 * Static TLS (variant II): the executable's local-exec accesses were
 * rewritten to read the thread pointer from fs:[-8] (the host main module's
 * 8-byte static TLS slot; loader.c patch_tls).  Each title thread gets a
 * block   [ prx images | main image | struct gthread ]   and fs:[-8] =
 * &gthread, so "[rax + tpoff]" with negative tpoff lands in the image exactly
 * as it would below a real TCB.  A boot prx with initial-exec relocations
 * (TPOFF64/32) gets a static image too, below the executable's.
 *
 * Dynamic TLS: DTPMOD64 relocations carry our ids (the executable 1, prx
 * 0x1000 up) and __tls_get_addr is routed here; each thread's blocks are
 * allocated on first use, in a vector that grows with the modules loaded.
 *
 * Blocks come from pools of their own, so "is fs:[-8] one of ours" is a
 * range test and a self pointer (no list walk).  A title thread that ends
 * gives its blocks back: the thread block to a free list, each dynamic
 * block to its module's.
 */
#include "ghost.h"

#define COLD GHOST_COLD /* boot-time or rare: small beats fast */

#define MIN_U32(a, b) ((a) < (b) ? (a) : (b))

#define GTHREAD_MAGIC 0x4448544754534847ULL /* "GHSTGTHD" */

struct gthread {
    u64 magic;
    struct gthread *self; /* NULL while the block is free */
    void **dtv;           /* blocks by module id - 0x1000 */
    u32 ndtv;
    struct gthread *next; /* free list */
};

static struct module *g_main;
static u64 g_static_size, g_static_align;
static volatile int g_lock;
static void *(*g_host_tls_get_addr)(void *);

/* thread block pools */
#define MAX_POOLS 48
static struct {
    u64 lo, hi;
} g_pools[MAX_POOLS];
static volatile u32 g_npools;
static u64 g_pool_cur, g_pool_end;
static struct gthread *g_free;

/* freed dynamic blocks, by module id - 0x1000 */
static void **g_dyn_free;
static u32 g_ndyn_free;

struct module *ghost_main_module(void) { return g_main; }

#ifndef TLS_SLOT
static inline u64 fs_slot_get(void)
{
    u64 v;
    __asm__ volatile("mov %%fs:-8, %0" : "=r"(v));
    return v;
}

static inline void fs_slot_set(u64 v)
{
    __asm__ volatile("mov %0, %%fs:-8" ::"r"(v) : "memory");
}
#else /* the offline tests: a variable stands in for fs:[-8] */
extern u64 TLS_SLOT;
static inline u64 fs_slot_get(void) { return TLS_SLOT; }
static inline void fs_slot_set(u64 v) { TLS_SLOT = v; }
#endif

/* initial-exec TLS: the module's code adds fixed offsets to fs:[0] */
COLD bool module_has_tpoff(const struct module *m)
{
    for (u64 i = 0; i < m->nrela; i++)
        if ((u32)m->rela[i].info == 18 || (u32)m->rela[i].info == 23) /* R_X86_64_TPOFF64 / TPOFF32 */
            return true;
    return false;
}

/* Static TLS: the executable's image right below the thread pointer, then
 * boot prx with initial-exec relocations.  Their offsets are fixed before
 * relocation, and before any title thread has a block. */
COLD void tls_setup(struct module *main_mod)
{
    g_main = main_mod;
    g_static_align = 16;
    u64 off = 0;
    for (u32 pass = 0; pass < 2; pass++)
        for (struct module *m = g_modules; m; m = m->next) {
            if (!m->tls_memsz || m->is_main != !pass || (pass && !module_has_tpoff(m)))
                continue;
            u64 a = m->tls_align > 16 ? m->tls_align : 16;
            if (a > g_static_align)
                g_static_align = a;
            off = ALIGN_UP(off + m->tls_memsz, a);
            m->tls_offset = off;
            if (pass)
                logf("tls: %s has initial-exec TLS: static image at -%lx", m->name, off);
        }
    g_static_size = ALIGN_UP(off, g_static_align);
    g_host_tls_get_addr = host_dlsym((u32)g_ctx->libkernel_handle, "__tls_get_addr");
    logf("tls: main static block %lx (align %lx), host __tls_get_addr %p", g_static_size, g_static_align,
         g_host_tls_get_addr);
}

/* O(pools): fs:[-8] of a host thread holds whatever SWRR keeps there, so
 * only memory inside a pool is read */
static bool is_ours(u64 p)
{
    u32 n = __atomic_load_n(&g_npools, __ATOMIC_ACQUIRE);
    for (u32 i = 0; i < n; i++)
        if (p >= g_pools[i].lo && p + sizeof(struct gthread) <= g_pools[i].hi)
            return !(p & 7) && ((struct gthread *)p)->self == (struct gthread *)p &&
                   ((struct gthread *)p)->magic == GTHREAD_MAGIC;
    return false;
}

COLD static struct gthread *block_get(void)
{
    spin_lock(&g_lock);
    struct gthread *gt = g_free;
    if (gt) {
        g_free = gt->next;
    } else {
        u64 need = g_static_size + g_static_align + sizeof(struct gthread);
        if (g_pool_cur + need > g_pool_end) {
            /* 64 KiB, doubling up to 1 MiB: MAX_POOLS covers any thread count */
            u64 size = ALIGN_UP((0x10000ULL << (g_npools < 4 ? g_npools : 4)) + need, 0x4000);
            u8 *p = g_npools < MAX_POOLS ? arena_alloc(size) : NULL;
            if (!p) {
                spin_unlock(&g_lock);
                logf("tls: no memory for thread blocks");
                return NULL;
            }
            g_pools[g_npools].lo = (u64)p, g_pools[g_npools].hi = (u64)p + size;
            __atomic_store_n(&g_npools, g_npools + 1, __ATOMIC_RELEASE);
            g_pool_cur = (u64)p, g_pool_end = (u64)p + size;
        }
        gt = (struct gthread *)ALIGN_UP(g_pool_cur + g_static_size, g_static_align);
        g_pool_cur = (u64)gt + sizeof(struct gthread);
    }
    spin_unlock(&g_lock);
    return gt;
}

COLD static struct gthread *thread_block_new(void)
{
    struct gthread *gt = block_get();
    if (!gt)
        return NULL;
    u8 *gt_addr = (u8 *)gt;
    memset(gt_addr - g_static_size, 0, g_static_size); /* a reused block keeps its last owner's values */
    for (struct module *m = g_modules; m; m = m->next)
        if (m->tls_offset && m->tls_filesz)
            memcpy(gt_addr - m->tls_offset, (void *)(m->base + m->tls_vaddr), m->tls_filesz);
    if (gt->dtv)
        memset(gt->dtv, 0, gt->ndtv * sizeof(*gt->dtv));
    gt->magic = GTHREAD_MAGIC;
    __atomic_store_n(&gt->self, gt, __ATOMIC_RELEASE);
    return gt;
}

void tls_ensure_thread(void)
{
    u64 cur = fs_slot_get();
    if (cur && is_ours(cur))
        return;
    struct gthread *gt = thread_block_new();
    if (gt)
        fs_slot_set((u64)gt);
}

/* A title thread ends (its function returned, or pthread_exit): its blocks
 * go back for the next thread, and its fs:[-8] (SWRR's own TLS slot) is
 * cleared: nothing of the title runs on the thread any more. */
COLD void tls_thread_exit(void)
{
    struct gthread *gt = (struct gthread *)fs_slot_get();
    if (!gt || !is_ours((u64)gt))
        return;
    spin_lock(&g_lock);
    for (u32 slot = 0; slot < gt->ndtv; slot++) {
        void **b = gt->dtv[slot];
        if (!b || slot >= g_ndyn_free)
            continue;
        struct module *m = g_modules;
        while (m && m->tls_id != 0x1000 + slot)
            m = m->next;
        if (m && !m->tls_offset) { /* a dynamic block: to its module's free list */
            *b = g_dyn_free[slot];
            g_dyn_free[slot] = b;
        }
    }
    gt->self = NULL;
    gt->next = g_free;
    g_free = gt;
    spin_unlock(&g_lock);
    fs_slot_set(0);
}

struct tls_index {
    u64 module;
    u64 offset;
};

#define TLS_MAIN_ID 1

/* module m's block for this thread: a freed one of the module's, else new */
COLD static u8 *dyn_block(struct module *m, u32 slot)
{
    u8 *block = NULL;
    spin_lock(&g_lock);
    if (slot >= g_ndyn_free) {
        u32 n = (slot + 16) & ~15u;
        void **v = arena_alloc(n * sizeof(*v));
        if (v) {
            memcpy(v, g_dyn_free, g_ndyn_free * sizeof(*v));
            g_dyn_free = v, g_ndyn_free = n;
        }
    }
    if (slot < g_ndyn_free && g_dyn_free[slot]) {
        block = g_dyn_free[slot];
        g_dyn_free[slot] = *(void **)block;
    }
    spin_unlock(&g_lock);
    u64 a = m->tls_align > 16 ? m->tls_align : 16, size = ALIGN_UP(m->tls_memsz, 16);
    if (!block) {
        u8 *raw = arena_alloc(size + a);
        if (!raw)
            return NULL;
        block = (u8 *)ALIGN_UP((u64)raw, a);
    }
    memset(block, 0, size);
    if (m->tls_filesz)
        memcpy(block, (void *)(m->base + m->tls_vaddr), m->tls_filesz);
    return block;
}

void *tls_get_addr(void *arg)
{
    struct tls_index *ti = arg;
    if (ti->module < 0x1000 && ti->module != TLS_MAIN_ID)
        return g_host_tls_get_addr(arg);
    tls_ensure_thread();
    struct gthread *gt = (struct gthread *)fs_slot_get();
    if (ti->module == TLS_MAIN_ID)
        return (u8 *)gt - (g_main ? g_main->tls_offset : 0) + ti->offset;
    u32 slot = (u32)(ti->module - 0x1000);
    if (slot < gt->ndtv && gt->dtv[slot])
        return (u8 *)gt->dtv[slot] + ti->offset;
    struct module *m = g_modules;
    while (m && m->tls_id != ti->module)
        m = m->next;
    if (!m)
        return NULL;
    if (slot >= gt->ndtv) {
        /* only this thread touches its vector: grow it in place of the old */
        u32 n = slot + 1 > 2 * gt->ndtv ? (slot + 16) & ~15u : 2 * gt->ndtv;
        void **dtv = arena_alloc(n * sizeof(*dtv));
        if (!dtv)
            return NULL;
        memcpy(dtv, gt->dtv, gt->ndtv * sizeof(*dtv));
        gt->dtv = dtv, gt->ndtv = n;
    }
    u8 *block = m->tls_offset ? (u8 *)gt - m->tls_offset : dyn_block(m, slot);
    if (!block)
        return NULL;
    gt->dtv[slot] = block;
    return block + ti->offset;
}

struct tramp {
    void *(*fn)(void *);
    void *arg;
};

/* the title libc's thread-exit hook (_sceKernelSetThreadDtors, from its
 * preinit): runs its thread_local destructors when a title thread ends */
void (*g_thread_dtors)(void);
void tls_set_thread_dtors(void (*fn)(void)) { g_thread_dtors = fn; }

__attribute__((force_align_arg_pointer)) static void *thread_tramp(void *p)
{
    struct tramp t = *(struct tramp *)p;
    tls_ensure_thread();
    void *r = t.fn(t.arg);
    if (g_thread_dtors)
        g_thread_dtors();
    tls_thread_exit();
    return r;
}

/* ---- callback thunks (§7.3.6) ----
 * A title function a system library calls on a thread of its own (AvPlayer's
 * memory and file callbacks, ...) runs patched local-exec code with that
 * thread's fs:[-8]: the thunk gives the thread a block first.  A 32-byte
 * stub "mov rax, fn; jmp [thunk_entry]"; thunk_entry keeps the argument
 * registers (integer and vector) across tls_ensure_thread and jumps to fn
 * with the caller's stack as it was. */
__attribute__((naked, used)) static void thunk_entry(void)
{
    __asm__ volatile(".intel_syntax noprefix\n"
                     "push rbp\n"
                     "mov rbp, rsp\n"
                     "and rsp, -16\n"
                     "sub rsp, 0xC0\n"
                     "mov [rsp], rdi\n mov [rsp + 0x08], rsi\n mov [rsp + 0x10], rdx\n"
                     "mov [rsp + 0x18], rcx\n mov [rsp + 0x20], r8\n mov [rsp + 0x28], r9\n"
                     "mov [rsp + 0x30], rax\n"
                     "movaps [rsp + 0x40], xmm0\n movaps [rsp + 0x50], xmm1\n"
                     "movaps [rsp + 0x60], xmm2\n movaps [rsp + 0x70], xmm3\n"
                     "movaps [rsp + 0x80], xmm4\n movaps [rsp + 0x90], xmm5\n"
                     "movaps [rsp + 0xA0], xmm6\n movaps [rsp + 0xB0], xmm7\n"
                     "call tls_ensure_thread\n"
                     "movaps xmm0, [rsp + 0x40]\n movaps xmm1, [rsp + 0x50]\n"
                     "movaps xmm2, [rsp + 0x60]\n movaps xmm3, [rsp + 0x70]\n"
                     "movaps xmm4, [rsp + 0x80]\n movaps xmm5, [rsp + 0x90]\n"
                     "movaps xmm6, [rsp + 0xA0]\n movaps xmm7, [rsp + 0xB0]\n"
                     "mov rdi, [rsp]\n mov rsi, [rsp + 0x08]\n mov rdx, [rsp + 0x10]\n"
                     "mov rcx, [rsp + 0x18]\n mov r8, [rsp + 0x20]\n mov r9, [rsp + 0x28]\n"
                     "mov rax, [rsp + 0x30]\n"
                     "leave\n"
                     "jmp rax\n"
                     ".att_syntax prefix\n");
}

/* fn wrapped (one stub per function, kept), or fn when there is no room */
COLD u64 tls_thunk(u64 fn)
{
    static struct {
        u64 fn, stub;
    } cache[64];
    static u32 n;
    if (!fn)
        return 0;
    rpc_lock();
    u64 stub = 0;
    for (u32 i = 0; i < n && !stub; i++)
        if (cache[i].fn == fn)
            stub = cache[i].stub;
    if (!stub) {
        u8 c[24] = {0x48, 0xB8}; /* mov rax, imm64; jmp [rip+0] */
        *(u64 *)(c + 2) = fn;
        c[10] = 0xFF, c[11] = 0x25;
        *(u64 *)(c + 16) = (u64)thunk_entry;
        stub = stub_alloc_code(c, sizeof(c));
        if (stub) {
            stubs_flush();
            if (n < ARRAY_SIZE(cache))
                cache[n].fn = fn, cache[n++].stub = stub;
        }
    }
    rpc_unlock();
    if (!stub)
        logf("tls: no stub room for a callback thunk to %lx: called as it is", fn);
    return stub ? stub : fn;
}

/* every thread the title created, for the snapshot watchdog (crash.c) */
static struct {
    u64 tid;
    const char *name;
} g_title_threads[192];
static volatile u32 g_nthreads;

/* A title thread through real, a host creation function with
 * scePthreadCreate's arguments (POSIX pthread_create ignores the name):
 * it starts in the trampoline, which gives it its TLS block. */
COLD int ghost_thread_start(void *real, void *thr, const void *attr, void *(*fn)(void *), void *arg, const char *name)
{
    struct tramp *t = arena_alloc(sizeof(*t));
    t->fn = fn;
    t->arg = arg;
    int rc = ((int (*)(void *, const void *, void *(*)(void *), void *, const char *))real)(thr, attr, thread_tramp,
                                                                                          t, name);
    if (rc != 0) {
        logf("thread create %s failed %x", name ? name : "?", rc);
        return rc;
    }
    u32 i = __atomic_fetch_add(&g_nthreads, 1, __ATOMIC_SEQ_CST);
    if (i < ARRAY_SIZE(g_title_threads)) {
        g_title_threads[i].tid = *(u64 *)thr;
        g_title_threads[i].name = arena_strdup(name ? name : "?");
    }
    return rc;
}

int ghost_thread_create(void *thr, const void *attr, void *(*fn)(void *), void *arg, const char *name)
{
    return ghost_thread_start((void *)K.pthread_create, thr, attr, fn, arg, name);
}

u32 ghost_thread_list(u64 *tids, const char **names, u32 max)
{
    u32 n = MIN_U32(g_nthreads, (u32)ARRAY_SIZE(g_title_threads));
    n = MIN_U32(n, max);
    for (u32 i = 0; i < n; i++)
        tids[i] = g_title_threads[i].tid, names[i] = g_title_threads[i].name;
    return n;
}
