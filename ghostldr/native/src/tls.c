/*
 * Thread-local storage for loaded modules.
 *
 * Main executable: its local-exec accesses were rewritten to read the thread
 * pointer from fs:[-8] (the host main module's 8-byte static TLS slot).  Each
 * Ghost thread gets a block   [ static TLS image | struct gthread ]   and
 * fs:[-8] = &gthread, so "[rax + tpoff]" with negative tpoff lands in the
 * image exactly as it would below a real TCB.
 *
 * Other loaded modules use general-dynamic TLS: their DTPMOD64 relocations
 * carry our ids (>= 0x1000) and __tls_get_addr is routed here.
 */
#include "ghost.h"

#define MIN_U32(a, b) ((a) < (b) ? (a) : (b))

#define MAX_DTLS 16
#define GTHREAD_MAGIC 0x4448544754534847ULL /* "GHSTGTHD" */

struct gthread {
    u64 magic;
    struct gthread *self;
    void *dtv[MAX_DTLS];
    struct gthread *next;
};

static struct module *g_main;
static u64 g_static_size, g_static_align;
static struct gthread *g_threads;
static volatile int g_threads_lock;
static void *(*g_host_tls_get_addr)(void *);

struct module *ghost_main_module(void) { return g_main; }

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

void tls_setup(struct module *main_mod)
{
    g_main = main_mod;
    g_static_align = main_mod->tls_align ? main_mod->tls_align : 16;
    if (g_static_align < 16)
        g_static_align = 16;
    g_static_size = ALIGN_UP(main_mod->tls_memsz, g_static_align);
    g_host_tls_get_addr = host_dlsym((u32)g_ctx->libkernel_handle, "__tls_get_addr");
    logf("tls: main static block %lx (align %lx), host __tls_get_addr %p", g_static_size, g_static_align,
         g_host_tls_get_addr);
}

static bool is_ours(u64 p)
{
    for (struct gthread *t = g_threads; t; t = t->next)
        if ((u64)t == p)
            return true;
    return false;
}

static struct gthread *thread_block_new(void)
{
    u64 total = g_static_size + sizeof(struct gthread) + g_static_align;
    u8 *raw = arena_alloc(total);
    u8 *gt_addr = (u8 *)ALIGN_UP((u64)raw + g_static_size, g_static_align);
    struct gthread *gt = (struct gthread *)gt_addr;
    u8 *image = gt_addr - g_static_size;
    if (g_main && g_main->tls_filesz)
        memcpy(image, (void *)(g_main->base + g_main->tls_vaddr), g_main->tls_filesz);
    gt->magic = GTHREAD_MAGIC;
    gt->self = gt;
    spin_lock(&g_threads_lock);
    gt->next = g_threads;
    g_threads = gt;
    spin_unlock(&g_threads_lock);
    return gt;
}

void tls_ensure_thread(void)
{
    u64 cur = fs_slot_get();
    if (cur && is_ours(cur))
        return;
    struct gthread *gt = thread_block_new();
    fs_slot_set((u64)gt);
}

struct tls_index {
    u64 module;
    u64 offset;
};

void *tls_get_addr(void *arg)
{
    struct tls_index *ti = arg;
    if (ti->module < 0x1000)
        return g_host_tls_get_addr(arg);
    tls_ensure_thread();
    struct gthread *gt = (struct gthread *)fs_slot_get();
    u32 slot = (u32)(ti->module - 0x1000);
    if (slot >= MAX_DTLS)
        return NULL;
    if (!gt->dtv[slot]) {
        struct module *m = g_modules;
        while (m && m->tls_id != ti->module)
            m = m->next;
        if (!m)
            return NULL;
        u8 *block = arena_alloc(ALIGN_UP(m->tls_memsz, 16) + 16);
        if (m->tls_filesz)
            memcpy(block, (void *)(m->base + m->tls_vaddr), m->tls_filesz);
        gt->dtv[slot] = block;
    }
    return (u8 *)gt->dtv[slot] + ti->offset;
}

struct tramp {
    void *(*fn)(void *);
    void *arg;
};

__attribute__((force_align_arg_pointer)) static void *thread_tramp(void *p)
{
    struct tramp t = *(struct tramp *)p;
    tls_ensure_thread();
    return t.fn(t.arg);
}

/* every thread the title created, for the snapshot watchdog (crash.c) */
static struct {
    u64 tid;
    const char *name;
} g_title_threads[192];
static volatile u32 g_nthreads;

int ghost_thread_create(void *thr, const void *attr, void *(*fn)(void *), void *arg, const char *name)
{
    struct tramp *t = arena_alloc(sizeof(*t));
    t->fn = fn;
    t->arg = arg;
    int rc = K.pthread_create(thr, attr, thread_tramp, t, name);
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

u32 ghost_thread_list(u64 *tids, const char **names, u32 max)
{
    u32 n = MIN_U32(g_nthreads, (u32)ARRAY_SIZE(g_title_threads));
    n = MIN_U32(n, max);
    for (u32 i = 0; i < n; i++)
        tids[i] = g_title_threads[i].tid, names[i] = g_title_threads[i].name;
    return n;
}
