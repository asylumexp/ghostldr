/*
 * Give the title the host's direct memory back.
 *
 * SWRR's process has the normal 4.5 GiB direct-memory budget, but the PS2
 * emulator holds ~1.3 GiB of it in large CPU/GPU buffers (EE/GS caches,
 * display buffers).  Luac0re has stopped the emulator's GS and IOP threads and
 * the EE thread never returns from Lua, so those buffers are dead weight.
 * Unmap and release every large direct-memory mapping in the emulator's heap
 * range that the loader did not allocate itself.
 *
 * Jak X (Pro mode, 5 GiB) holds ~2 GiB, 1.5 GiB of it the GS emulation's.
 * UltraC0re leaves the GS and IOP threads running: they are stopped here
 * first, as Luac0re does in SWRR.
 */
#include "ghost.h"

struct vq_info2 {
    u64 start, end, offset;
    s32 prot, type;
    u8 flags;
    char name[32];
} __attribute__((packed));

bool direct_is_ours(u64 addr);

/* SWRR's executable (US and EU are the same code): its 256 MiB libc mspace
 * (created at eboot+0x9c7cb, handle at +0x3906c0) and its
 * "SystemServicesThread" (entry +0xabc70, handle at +0x182f330).  That
 * thread polls system-service events and the companion-app server
 * (sceCompanionHttpd), whose 10 MiB work buffer (pointer at +0x182f338) it
 * took from the mspace; it leaves its loop once the word at +0x182f328 is
 * non-zero (SWRR never sets it), then terminates the server, frees the
 * buffer, clears the pointer and returns. */
#define SWRR_HEAP_MSPACE 0x3906c0
#define SWRR_SYSSVC_QUIT 0x182f328
#define SWRR_SYSSVC_THREAD 0x182f330
#define SWRR_SYSSVC_BUFFER 0x182f338

/* PS4: the companion server runs there (apparently not on a PS5) and the
 * thread writes into its buffer, so the heap cannot go while it runs: on a
 * PS4 v0.1.1 boot it faulted at once (docs/ps4.md).  Stop it the way SWRR's
 * own exit path would; true when nothing of it uses the heap any more. */
GHOST_COLD static bool ps4_stop_system_services(void)
{
    u64 eb = g_ctx->eboot_base, thr = *(volatile u64 *)(eb + SWRR_SYSSVC_THREAD);
    volatile u64 *buf = (volatile u64 *)(eb + SWRR_SYSSVC_BUFFER);
    int (*kill)(u64, int) = host_dlsym((u32)g_ctx->libkernel_handle, "pthread_kill");
    bool alive = thr && kill && kill(thr, 0) == 0;
    u64 b0 = *buf;
    *(volatile s32 *)(eb + SWRR_SYSSVC_QUIT) = 1;
    u32 ms = 0;
    for (; ms < 5000 && (*buf || (alive && kill(thr, 0) == 0)); ms += 20)
        K.usleep(20000);
    bool idle = !*buf && !(alive && kill(thr, 0) == 0);
    logf("hostmem: PS4: SWRR's SystemServicesThread (%s, companion buffer %lx): %s after %u ms",
         alive ? "running" : "not running", b0, idle ? "stopped" : "still running", ms);
    return idle;
}

/* the host's emulator threads its loader did not stop (host.c): cancelled
 * the way the loaders do it; one still running after 200 ms (blocked where
 * a cancel does not reach, waiting for the stopped EE) is suspended, so it
 * never wakes up in the buffers the title gets */
__attribute__((minsize, cold)) void host_stop_emulator_threads(void)
{
    u32 lk = (u32)g_ctx->libkernel_handle;
    int (*cancel)(u64) = host_dlsym(lk, "scePthreadCancel");
    int (*suspend)(u64) = host_dlsym(lk, "scePthreadSuspend");
    int (*kill)(u64, int) = host_dlsym(lk, "pthread_kill");
    int (*getname)(u64, char *) = host_dlsym(lk, "scePthreadGetname");
    for (u32 i = 0; i < ARRAY_SIZE(g_hostgame->stop) && g_hostgame->stop[i].name; i++) {
        u64 thr = *(volatile u64 *)(g_ctx->eboot_base + g_hostgame->stop[i].handle);
        char name[40] = "";
        if (!thr || !cancel || !kill || kill(thr, 0) != 0) {
            logf("hostmem: %s thread %s (%lx): not running", g_hostgame->name, g_hostgame->stop[i].name, thr);
            continue;
        }
        if (getname)
            getname(thr, name);
        name[39] = 0;
        int rc = cancel(thr);
        u32 ms = 0;
        for (; ms < 200 && kill(thr, 0) == 0; ms += 10)
            K.usleep(10000);
        bool alive = kill(thr, 0) == 0;
        int src = alive && suspend ? suspend(thr) : -1;
        logf("hostmem: %s thread %s (%lx, '%s'): cancel %x, %s", g_hostgame->name, g_hostgame->stop[i].name, thr,
             name, rc, !alive ? "gone" : src == 0 ? "still there after 200 ms: suspended" : "STILL RUNNING");
    }
}

u64 host_release_emulator_memory(u64 min_size)
{
    int (*rel)(off_t, size_t) = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelReleaseDirectMemory");
    struct vq_info2 *info = arena_alloc(sizeof(*info) + 16);
    u64 freed = 0, addr = 0x200000000;
    u64 heap = is_ps4() && !is_jakx() ? *(volatile u64 *)(g_ctx->eboot_base + SWRR_HEAP_MSPACE) : 0;
    bool heap_idle = !heap || ps4_stop_system_services();
    /* a PS4 caps what SWRR may allocate below its free memory (budget.c):
     * whether releasing gives any of that back decides what a title can have */
    u64 before = is_ps4() ? budget_allocatable(0) : 0;
    for (int i = 0; i < 400 && addr < 0x280000000; i++) {
        memset(info, 0, sizeof(*info));
        if (K.virtual_query((void *)addr, 1, info, 0x48) != 0)
            break;
        u64 start = info->start, size = info->end - info->start;
        addr = info->end;
        if (start >= 0x280000000)
            break;
        if (!(info->flags & 2) || size < min_size || direct_is_ours(start))
            continue;
        if (ps4_keep_heap(start, info->end, heap, heap_idle)) {
            logf("hostmem: PS4: kept SWRR's heap %lx-%lx (%lu MiB): a thread may still use it", start, info->end,
                 size >> 20);
            continue;
        }
        int urc = K.munmap((void *)start, size);
        int rrc = rel ? rel((off_t)info->offset, size) : -1;
        logf("hostmem: released %lx-%lx (%lu MiB, prot %x type %d phys %lx) munmap %x release %x", start,
             info->end, size >> 20, info->prot, info->type, info->offset, urc, rrc);
        if (rrc == 0)
            freed += size;
    }
    /* the host's display buffers: close its video output first (the title
     * opens its own in sceVideoOutOpen), then release them too */
    s32 host_vo = *(volatile s32 *)(g_ctx->eboot_base + g_hostgame->video_handle);
    int (*vo_close)(s32) = NULL;
    {
        void *host_real_in(const char *modname, const char *name);
        vo_close = (int (*)(s32))host_real_in("libSceVideoOut", "sceVideoOutClose");
    }
    if (host_vo > 0 && vo_close) {
        int c = vo_close(host_vo);
        logf("hostmem: closed %s video output %d -> %x", g_hostgame->name, host_vo, c);
        if (c == 0) {
            *(volatile s32 *)(g_ctx->eboot_base + g_hostgame->video_handle) = 0;
            addr = 0x200000000;
            for (int i = 0; i < 400 && addr < 0x280000000; i++) {
                memset(info, 0, sizeof(*info));
                if (K.virtual_query((void *)addr, 1, info, 0x48) != 0)
                    break;
                u64 start = info->start, size = info->end - info->start;
                addr = info->end;
                if (start >= 0x280000000)
                    break;
                if (!(info->flags & 2) || info->type != 3 || size < 0x700000 || direct_is_ours(start))
                    continue;
                int urc = K.munmap((void *)start, size);
                int rrc = rel ? rel((off_t)info->offset, size) : -1;
                logf("hostmem: released display buffer %lx-%lx phys %lx munmap %x release %x", start, info->end,
                     info->offset, urc, rrc);
                if (rrc == 0)
                    freed += size;
            }
        }
    }
    off_t s = 0;
    size_t avail = 0;
    K.avail_direct(0, (off_t)K.direct_size(), 0x10000, &s, &avail);
    logf("hostmem: freed %lu MiB, direct available now %lu MiB", freed >> 20, (u64)avail >> 20);
    if (is_ps4()) {
        u64 after = budget_allocatable(0);
        logf("hostmem: PS4: the kernel hands out %lu MiB before the release, %lu MiB after: %lu of the %lu MiB "
             "released count",
             before >> 20, after >> 20, (after > before ? after - before : 0) >> 20, freed >> 20);
    }
    return freed;
}

/* Log every allocated physical direct-memory range and every direct mapping
 * in the user VA, so the remaining budget can be attributed. */
void host_dmem_census(void)
{
    struct dmq_info {
        off_t start, end;
        int type;
    } q;
    int (*dmq)(off_t, int, void *, size_t) = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelDirectMemoryQuery");
    u64 total = 0, off = 0, real = K.direct_size();
    for (int i = 0; dmq && i < 16384 && off < real; i++) {
        memset(&q, 0, sizeof(q));
        if (dmq((off_t)off, 1 /* find next */, &q, sizeof(q)) != 0) {
            off = ALIGN_DOWN(off, 1UL << 20) + (1UL << 20); /* see budget.c free_holes */
            continue;
        }
        u64 size = (u64)(q.end - q.start);
        total += size;
        if (size >= 0x100000)
            logf("census: phys %lx-%lx %lu MiB type %d", (u64)q.start, (u64)q.end, size >> 20, q.type);
        off = (u64)q.end;
    }
    logf("census: %lu MiB of direct memory allocated", total >> 20);

    struct vq_info2 *info = arena_alloc(sizeof(*info) + 16);
    u64 addr = 0x10000, mapped = 0;
    for (int i = 0; i < 2000; i++) {
        memset(info, 0, sizeof(*info));
        if (K.virtual_query((void *)addr, 1, info, 0x48) != 0)
            break;
        u64 size = info->end - info->start;
        if ((info->flags & 2) && size >= 0x100000) {
            mapped += size;
            info->name[31] = 0;
            logf("census: va %lx-%lx %lu MiB prot %x type %d phys %lx '%s'", info->start, info->end, size >> 20,
                 info->prot, info->type, info->offset, info->name);
        }
        addr = info->end;
    }
    logf("census: %lu MiB of direct mappings >= 1 MiB", mapped >> 20);

    addr = 0x10000;
    mapped = 0;
    for (int i = 0; i < 4000; i++) {
        memset(info, 0, sizeof(*info));
        if (K.virtual_query((void *)addr, 1, info, 0x48) != 0)
            break;
        u64 size = info->end - info->start;
        if ((info->flags & 1) && size >= 0x100000) {
            mapped += size;
            info->name[31] = 0;
            logf("census: flex va %lx-%lx %lu MiB prot %x '%s'", info->start, info->end, size >> 20, info->prot,
                 info->name);
        }
        addr = info->end;
    }
    size_t fa = 0, fc = 0;
    K.avail_flex(&fa);
    size_t (*conf)(size_t *) = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelConfiguredFlexibleMemorySize");
    if (conf)
        conf(&fc);
    logf("census: flex mappings >= 1 MiB %lu MiB; flex available %lu MiB configured %lu MiB", mapped >> 20,
         (u64)fa >> 20, (u64)fc >> 20);

    /* Do not probe system flexible memory to exhaustion: the map call blocks
     * forever once the budget is gone. */
}
