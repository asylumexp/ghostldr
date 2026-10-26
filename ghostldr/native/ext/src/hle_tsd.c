/*
 * POSIX thread-specific data (pthread_key_create, pthread_get/setspecific).
 *
 * SWRR's libkernel faults (address 0x70) in pthread_setspecific and
 * scePthreadSetspecific the first time a homebrew title's thread stores a
 * value (measured on 13.60: libc++abi keeps its exception globals there, so
 * the OpenOrbis exceptions sample crashed at its first throw; hbprobe too).
 * The POSIX functions are answered here instead: keys and per-thread values
 * of the family's own.  Destructors run when a title thread ends: the
 * family takes the title libc's thread-exit hook (_sceKernelSetThreadDtors,
 * which the core's thread trampoline and pthread_exit call) and runs the
 * title's hook first, then the destructors.  The SCE names stay SWRR's:
 * Ghost imports them.
 */
#include "ext.h"

#define EINVAL 22

#define TSD_KEYS 256
#define TSD_THREADS 1024
#define TSD_DEAD 1UL
#define EAGAIN 35
#define ENOMEM 12

static struct {
    volatile u32 used;
    void (*dtor)(void *);
} g_keys[TSD_KEYS];
static struct {
    volatile u64 tid; /* 0 free, TSD_DEAD a thread that ended */
    void **vals;
} g_tsd[TSD_THREADS];
static volatile int g_tsd_lock;

static u64 self(void)
{
    static u64 (*fn)(void);
    if (!fn)
        fn = (u64(*)(void))host_real("pthread_self");
    return fn ? fn() : 0;
}

/* the calling thread's values (create: make them) */
static void **tsd_vals(bool create)
{
    u64 t = self();
    u32 h = (u32)((t >> 4) % TSD_THREADS);
    for (u32 i = 0; i < TSD_THREADS; i++) {
        u32 k = (h + i) % TSD_THREADS;
        u64 id = __atomic_load_n(&g_tsd[k].tid, __ATOMIC_ACQUIRE);
        if (id == t)
            return g_tsd[k].vals;
        if (!id)
            break;
    }
    if (!create || !t)
        return NULL;
    void **vals = NULL;
    spin_lock(&g_tsd_lock);
    for (u32 i = 0; i < TSD_THREADS && !vals; i++) {
        u32 k = (h + i) % TSD_THREADS;
        u64 id = g_tsd[k].tid;
        if (id && id != TSD_DEAD)
            continue;
        if (!g_tsd[k].vals)
            g_tsd[k].vals = arena_alloc(TSD_KEYS * sizeof(void *));
        vals = g_tsd[k].vals;
        if (vals) {
            memset(vals, 0, TSD_KEYS * sizeof(void *));
            __atomic_store_n(&g_tsd[k].tid, t, __ATOMIC_RELEASE);
        }
    }
    spin_unlock(&g_tsd_lock);
    return vals;
}

/* a thread ends: destructors (up to 4 rounds, as POSIX allows), then its slot goes */
static void tsd_thread_exit(void)
{
    void **vals = tsd_vals(false);
    for (u32 round = 0; vals && round < 4; round++) {
        bool again = false;
        for (u32 k = 0; k < TSD_KEYS; k++) {
            void *v = vals[k];
            void (*d)(void *) = g_keys[k].dtor;
            if (v && g_keys[k].used && d) {
                vals[k] = NULL;
                d(v);
                again = true;
            }
        }
        if (!again)
            break;
    }
    u64 t = self();
    spin_lock(&g_tsd_lock);
    for (u32 k = 0; vals && k < TSD_THREADS; k++)
        if (g_tsd[k].tid == t)
            __atomic_store_n(&g_tsd[k].tid, TSD_DEAD, __ATOMIC_RELEASE);
    spin_unlock(&g_tsd_lock);
}

static s64 p_key_create(u32 *key, void (*dtor)(void *))
{
    for (u32 k = 0; key && k < TSD_KEYS; k++) {
        u32 expect = 0;
        if (__atomic_compare_exchange_n(&g_keys[k].used, &expect, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            g_keys[k].dtor = dtor;
            *key = k;
            return 0;
        }
    }
    return key ? EAGAIN : EINVAL;
}

static s64 p_key_delete(u32 key)
{
    if (key >= TSD_KEYS || !g_keys[key].used)
        return EINVAL;
    g_keys[key].dtor = NULL;
    __atomic_store_n(&g_keys[key].used, 0, __ATOMIC_RELEASE);
    return 0;
}

static void *p_getspecific(u32 key)
{
    void **vals = key < TSD_KEYS ? tsd_vals(false) : NULL;
    return vals ? vals[key] : NULL;
}

static s64 p_setspecific(u32 key, const void *value)
{
    if (key >= TSD_KEYS || !g_keys[key].used)
        return EINVAL;
    void **vals = tsd_vals(true);
    if (!vals)
        return ENOMEM;
    vals[key] = (void *)value;
    return 0;
}

/* the title libc's thread-exit hook, then the destructors */
static void (*g_title_dtors)(void);

static void thread_exit_hook(void)
{
    void (*d)(void) = g_title_dtors;
    if (d)
        d();
    tsd_thread_exit();
}

static s64 hle_set_thread_dtors(void (*fn)(void))
{
    if (tls_owner && tls_owner()) /* an ELF loader payload's: its code goes away before SWRR does */
        return 0;
    g_title_dtors = fn;
    if (G->thread_dtors) /* a host without the core's slot (test harnesses) has nowhere to publish it */
        g_thread_dtors = thread_exit_hook;
    return 0;
}

static int tsd_init(void)
{
    if (G->thread_dtors)
        g_thread_dtors = thread_exit_hook;
    return 0;
}

#define LK "libkernel"
static const struct ext_hle_ent k_hle[] = {
    {LK, "pthread_key_create", p_key_create, NULL},
    {LK, "pthread_key_delete", p_key_delete, NULL},
    {LK, "pthread_getspecific", p_getspecific, NULL},
    {LK, "pthread_setspecific", p_setspecific, NULL},
    {LK, "_sceKernelSetThreadDtors", hle_set_thread_dtors, NULL},
};

EXT_FAMILY("tsd", k_hle, tsd_init);
