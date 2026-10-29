/*
 * Back to the game selector (ctx CTX_FLAG_RETURN; docs/selector.md "Back to
 * the game selector").  ghostldr.lua's listen mode on a PS5, with the
 * bundle cached, boots a title and waits in the core (boot.c title_back),
 * which calls title_back on the boot thread.  It waits until the title's run
 * is over, then stops the title in place: SWRR's process, and Luac0re in it,
 * live on.
 *
 *  - The run is over when the title quit or crashed and its page was shown
 *    (failure.c, crash.c: D->back), or when L1+R1+Options+touchpad are held
 *    on the controller.
 *  - Its threads, and ghostldr's own made for it (the core's list, tls.c),
 *    end: each is sent SIGUSR2, whose handler sends it to scePthreadExit
 *    instead of back to what it was doing.  For the first second and a half
 *    only threads stopped in the title's own code are taken (one inside a
 *    system library may hold that library's lock, which the next title
 *    needs), then any that isn't writing ghostldr's log, then any.
 *  - What the title holds in SWRR's process goes: its compute queues (the
 *    GPU reads their rings), AvPlayers (their threads call the title back),
 *    audio ports, its VideoOut handle (the screen reads its buffers), its
 *    save mounts and the FTP server.
 *  - The executable memory: the emulator's JIT buffers its modules and
 *    their data went into are mapped whole again at their homes, and a large
 *    executable's range (bigexec.c's views) is unmapped, so the next boot
 *    finds the arena as the first one did.
 * The core then gives back ghostfs's mounts, this extension and the direct
 * memory allocated since the boot began, and returns to ghostldr.lua, which
 * opens the game selector again.
 */
#include "ext.h"

#define SIGUSR2 31
#define SA_SIGINFO 0x40
#define SA_RESTART 0x2
#define SYS_close 6
#define MAX_THREADS 1024
/* the controller: L1 + R1 + Options + touchpad button, held for 0.3 s */
#define COMBO (0x400u | 0x800u | 0x8u | 0x100000u)
#define COMBO_TICKS 3
#define TICK_US 100000
#define CRASH_TICKS 150 /* a crash whose page never said it was shown: 15 s */
/* compiler-visible out parameters (jitrpc.c's private scratch, as bigexec.c's) */
#define OUT(off) (g_ctx->oob_scratch + 0x70000 + 0x300 + (off))

struct ksigaction {
    void (*handler)(int, void *, void *);
    int flags;
    u32 mask[4];
};

/* failure.c */
void diag_notice(const char *text, u32 seconds) HIDDEN;
s32 ov_vo_handle(void) HIDDEN;
/* hle_savedata.c, ftp_sys.c */
void savedata_title_end(void) HIDDEN;
int ftp_early_stop(void) HIDDEN;

/* ---- what the title holds: noted as it gets it ---- */
#define MAX_HANDLES 64
static struct {
    volatile int lock;
    s32 cq[MAX_HANDLES], ao[MAX_HANDLES];
    u64 av[16];
} H;

static void note(s32 *t, s32 h, bool add)
{
    spin_lock(&H.lock);
    for (u32 i = 0; i < MAX_HANDLES; i++)
        if (add ? !t[i] : t[i] == h) {
            t[i] = add ? h : 0;
            break;
        }
    spin_unlock(&H.lock);
}

/* hle_kernel.c: an AvPlayer the title made */
HIDDEN void back_note_avplayer(u64 h)
{
    spin_lock(&H.lock);
    for (u32 i = 0; h && i < ARRAY_SIZE(H.av); i++)
        if (!H.av[i]) {
            H.av[i] = h;
            break;
        }
    spin_unlock(&H.lock);
}

typedef s32 (*fn6)(u64, u64, u64, u64, u64, u64);
#define GNM "libSceGnmDriver"

/* compute queues: their vqueue ids (as the core's trace logged them) */
static s32 map_cq(const char *name, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)
{
    static u32 calls;
    fn6 real = (fn6)host_real_in(GNM, name);
    s32 r = real ? real(a, b, c, d, e, f) : -1;
    if (calls++ < 8)
        logf("%s(%lx, %lx, %lx, %lx, %lx, %lx) -> %x", name, a, b, c, d, e, f, (u32)r);
    if (r > 0)
        note(H.cq, r, true);
    return r;
}
static s32 hle_map_cq(u64 a, u64 b, u64 c, u64 d, u64 e)
{
    return map_cq("sceGnmMapComputeQueue", a, b, c, d, e, 0);
}
static s32 hle_map_cq_prio(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)
{
    return map_cq("sceGnmMapComputeQueueWithPriority", a, b, c, d, e, f);
}
static s32 hle_unmap_cq(s32 vq)
{
    s32 (*real)(s32) = host_real_in(GNM, "sceGnmUnmapComputeQueue");
    s32 r = real ? real(vq) : -1;
    logf("sceGnmUnmapComputeQueue(%x) -> %x", (u32)vq, (u32)r);
    note(H.cq, vq, false);
    return r;
}

/* audio ports */
static s32 hle_ao_open(u64 user, u64 type, u64 index, u64 len, u64 freq, u64 param)
{
    fn6 real = (fn6)host_real_in("libSceAudioOut", "sceAudioOutOpen");
    s32 r = real ? real(user, type, index, len, freq, param) : -1;
    if (r > 0)
        note(H.ao, r, true);
    return r;
}
static s32 hle_ao_close(s32 h)
{
    s32 (*real)(s32) = host_real_in("libSceAudioOut", "sceAudioOutClose");
    note(H.ao, h, false);
    return real ? real(h) : -1;
}

/* ---- the end of the run ---- */

static u32 pad_buttons(void)
{
    static s32 (*rd)(s32, u8 *), (*get)(s32, s32, s32), (*user)(s32 *), h = -1;
    if (!rd) {
        rd = (s32(*)(s32, u8 *))host_real_in("libScePad", "scePadReadState");
        get = (s32(*)(s32, s32, s32))host_real_in("libScePad", "scePadGetHandle");
        user = (s32(*)(s32 *))host_real_in("libSceUserService", "sceUserServiceGetInitialUser");
    }
    s32 u = -1;
    if (h < 0 && get && user && user(&u) == 0)
        h = get(u, 0, 0);
    u8 data[0x80];
    memset(data, 0, sizeof(data));
    return rd && h >= 0 && rd(h, data) == 0 ? *(u32 *)data : 0;
}

/* until the run is over: what ended it */
static const char *wait_end(struct ghost_diag *D)
{
    for (u32 held = 0, crashed = 0;;) {
        K.usleep(TICK_US);
        if (D && __atomic_load_n(&D->back, __ATOMIC_ACQUIRE))
            return D->crashed ? "the title crashed" : "the title quit";
        if (D && D->crashed && ++crashed >= CRASH_TICKS)
            return "the title crashed (no page)";
        held = (pad_buttons() & COMBO) == COMBO ? held + 1 : 0;
        if (held >= COMBO_TICKS)
            return "L1+R1+Options+touchpad";
    }
}

/* ---- the threads ---- */

static volatile int g_force; /* 0: only in the title's code, 1: unless the log is being written, 2: any */

static bool in_title(u64 rip)
{
    for (struct module *m = g_modules; m; m = m->next)
        if (rip >= m->base && rip - m->base < m->image_size)
            return true;
    return false;
}

__attribute__((force_align_arg_pointer, noreturn)) static void thread_end(void)
{
    void (*ex)(void *) = host_real("scePthreadExit");
    if (ex)
        ex(NULL);
    for (;;)
        K.usleep(1000000);
}

/* SIGUSR2: the thread goes on in thread_end, as if called from where it was
 * (crash.c's payload path); or, not yet taken, back to what it was doing */
static void on_stop(int sig, void *info, void *ucv)
{
    u64 *mc = (u64 *)((u8 *)ucv + 0x40); /* mcontext_t (a 0x40-byte header first) */
    u64 rip = mc[20], rsp = mc[23];
    struct ghost_diag *D = GHOST_DIAG(G);
    int f = __atomic_load_n(&g_force, __ATOMIC_ACQUIRE);
    if ((f == 0 && !in_title(rip)) || (f == 1 && D && __atomic_load_n(&D->lock, __ATOMIC_ACQUIRE)))
        return;
    mc[23] = ALIGN_DOWN(rsp - 0x100, 16) - 8;
    mc[20] = (u64)thread_end;
    (void)sig, (void)info;
}

static int set_handler(void (*fn)(int, void *, void *))
{
    int (*sa)(int, const struct ksigaction *, struct ksigaction *) =
        host_dlsym((u32)g_ctx->libkernel_handle, "sigaction");
    struct ksigaction a;
    memset(&a, 0, sizeof(a));
    a.handler = fn ? fn : (void (*)(int, void *, void *))1; /* SIG_IGN after: none may still come */
    a.flags = fn ? SA_SIGINFO | SA_RESTART : 0;
    return sa ? sa(SIGUSR2, &a, NULL) : -1;
}

/* every listed thread ended: how many are left */
static u32 stop_threads(void)
{
    int (*kill)(u64, int) = host_dlsym((u32)g_ctx->libkernel_handle, "pthread_kill");
    u64 (*self)(void) = host_real("scePthreadSelf");
    u64 *tids = page_alloc(MAX_THREADS * sizeof(u64));
    const char **names = page_alloc(MAX_THREADS * sizeof(char *));
    if (!kill || !tids || !names || set_handler(on_stop) != 0) {
        logf("back: no way to stop the title's threads");
        return 1;
    }
    u32 n = G->thread_list(tids, names, MAX_THREADS), left = n, round = 0;
    u64 me = self ? self() : 0;
    for (; left && round < 200; round++) {
        __atomic_store_n(&g_force, round < 50 ? 0 : round < 120 ? 1 : 2, __ATOMIC_RELEASE);
        left = 0;
        for (u32 i = 0; i < n; i++) {
            if (!tids[i] || tids[i] == me || kill(tids[i], 0) != 0) { /* ended (ESRCH) */
                tids[i] = 0;
                continue;
            }
            kill(tids[i], SIGUSR2);
            left++;
        }
        K.usleep(30000);
    }
    for (u32 i = 0; i < n; i++)
        if (tids[i] && kill(tids[i], 0) == 0)
            logf("back: thread %s didn't end", names[i]);
    logf("back: %u threads listed; %u left after %u ms", n, left, round * 30);
    page_free(tids, MAX_THREADS * sizeof(u64));
    page_free(names, MAX_THREADS * sizeof(char *));
    return left;
}

/* ---- the title's handles ---- */

static void *av_close_thread(void *arg)
{
    s32 (*close)(u64) = host_real_in("libSceAvPlayer", "sceAvPlayerClose");
    for (u32 i = 0; close && i < ARRAY_SIZE(H.av); i++)
        if (H.av[i]) {
            logf("back: AvPlayer %lx closed: %x", H.av[i], (u32)close(H.av[i]));
            H.av[i] = 0;
        }
    *(volatile int *)arg = 1;
    return NULL;
}

static void close_handles(void)
{
    s32 (*unmap_cq)(s32) = host_real_in(GNM, "sceGnmUnmapComputeQueue");
    s32 (*ao_close)(s32) = host_real_in("libSceAudioOut", "sceAudioOutClose");
    s32 (*vo_close)(s32) = host_real_in("libSceVideoOut", "sceVideoOutClose");
    K.usleep(200000); /* the GPU finishes what was submitted */
    for (u32 i = 0; i < MAX_HANDLES; i++) {
        if (H.cq[i] && unmap_cq)
            logf("back: compute queue %x unmapped: %x", (u32)H.cq[i], (u32)unmap_cq(H.cq[i]));
        if (H.ao[i] && ao_close)
            logf("back: audio port %x closed: %x", (u32)H.ao[i], (u32)ao_close(H.ao[i]));
        H.cq[i] = H.ao[i] = 0;
    }
    /* AvPlayer's close frees through the title's allocator, whose lock a
     * stopped thread may hold: on a thread of its own, waited for 2 s */
    static volatile int done;
    u64 tid = 0;
    bool any = false;
    for (u32 i = 0; i < ARRAY_SIZE(H.av); i++)
        any |= H.av[i] != 0;
    if (any && K.pthread_create(&tid, NULL, av_close_thread, (void *)&done, "ghostldr-back") == 0) {
        for (u32 t = 0; t < 100 && !done; t++)
            K.usleep(20000);
        if (!done)
            logf("back: an AvPlayer didn't close in 2 s: left as it is");
    }
    s32 vo = ov_vo_handle();
    if (vo >= 0 && vo_close)
        logf("back: VideoOut handle %d closed: %x", vo, (u32)vo_close(vo));
}

/* ---- executable memory ---- */

static void c_close(int fd)
{
    if (fd >= 0)
        jit_call(g_ctx->jit_syscall, SYS_close, (u64)fd, 0, 0, 0, 0, 0);
}

/* Every JIT object of the compiler's whose home is among the emulator's
 * buffers (past the IPU buffer, which holds the core), mapped whole again
 * there unless it is whole already: the title's modules, their data
 * (punched to RW memory, arena.c) and the extension's (when it went to a
 * buffer's top) leave holes in them otherwise. */
static void restore_buffers(void)
{
    u64 from = g_ctx->reserved[2] ? g_ctx->reserved[2] : 0x9009d4000, self = (u64)restore_buffers;
    int (*qmp)(void *, void **, void **, int *) =
        host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelQueryMemoryProtection");
    for (int fd = 0; fd < 1024 && qmp; fd++) {
        *(volatile u64 *)OUT(0) = (u64)-1;
        if ((int)jit_call(g_ctx->jit_shm_alias, 0, (u64)fd, PROT_READ | PROT_EXEC, OUT(0), 0, 0, 0) != 0)
            continue;
        int rx_c = (int)*(volatile u32 *)OUT(0), rx = jit_recv_fd(rx_c), rw_c = -1;
        c_close(rx_c);
        u8 st[0x78];
        u64 size = rx >= 0 && K.fstat(rx, st) == 0 ? *(u64 *)(st + 0x48) : 0, home = 0;
        if (size >= 0x100000) { /* the core's 256 KiB object and new blocks (the extension's) are not buffers */
            *(volatile u64 *)OUT(0) = (u64)-1;
            if ((int)jit_call(g_ctx->jit_shm_alias, 0, (u64)fd, PROT_READ | PROT_WRITE, OUT(0), 0, 0, 0) == 0) {
                rw_c = (int)*(volatile u32 *)OUT(0);
                *(volatile u64 *)OUT(8) = 0;
                if ((int)jit_call(g_ctx->jit_shm_map, 0, (u64)rw_c, PROT_READ | PROT_WRITE, OUT(8), 0, 0, 0) == 0)
                    home = *(volatile u64 *)OUT(8);
            }
            c_close(rw_c); /* its mapping stays, at its home in the compiler as before */
        }
        void *s = 0, *e = 0;
        int prot = 0;
        bool buffer = home > from && home < from + 0x5000000 && !(self >= home && self < home + size);
        if (buffer && !(qmp((void *)home, &s, &e, &prot) == 0 && (u64)s <= home && (u64)e >= home + size &&
                        prot == (PROT_READ | PROT_EXEC))) {
            void *res = NULL;
            K.munmap((void *)home, size);
            int rc = K.mmap((void *)home, size, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED, rx, 0, &res);
            logf("back: the %lu MiB JIT buffer at %lx mapped whole again: %x", size >> 20, home, rc);
        }
        if (rx >= 0)
            K.close(rx);
    }
    /* a large executable's views and the memory beside them (bigexec.c):
     * its whole reserved range, from its base past its last view */
    for (struct module *m = g_modules; m; m = m->next)
        if (m->bx) {
            u64 len = ALIGN_UP(m->image_size + (32UL << 20), 0x200000);
            logf("back: %s's range %lx+%lx unmapped: %x", m->name, m->base, len, K.munmap((void *)m->base, len));
        }
}

/* ---- the extension's export (ext_main.c) ---- */
HIDDEN int title_back(void)
{
    struct ghost_diag *D = GHOST_DIAG(G);
    const char *why = wait_end(D);
    logf("back: %s: the title stops, then the game selector", why);
    if (D && !D->back)
        diag_notice("Back to the game selector.", 3);
    ftp_early_stop();
    if (stop_threads())
        logf("back: some threads are still there: going on anyway");
    close_handles();
    savedata_title_end();
    restore_buffers();
    set_handler(NULL);
    return 0;
}

static const struct ext_hle_ent k_hle[] = {
    {GNM, "sceGnmMapComputeQueue", hle_map_cq, NULL},
    {GNM, "sceGnmMapComputeQueueWithPriority", hle_map_cq_prio, NULL},
    {GNM, "sceGnmUnmapComputeQueue", hle_unmap_cq, NULL},
    {"libSceAudioOut", "sceAudioOutOpen", hle_ao_open, NULL},
    {"libSceAudioOut", "sceAudioOutClose", hle_ao_close, NULL},
};
EXT_FAMILY("back", k_hle, NULL);
