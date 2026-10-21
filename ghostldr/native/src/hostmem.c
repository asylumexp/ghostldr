/*
 * Give the title SWRR's direct memory back.
 *
 * SWRR's process has the normal 4.5 GiB direct-memory budget, but the PS2
 * emulator holds ~1.3 GiB of it in large CPU/GPU buffers (EE/GS caches,
 * display buffers).  Luac0re has stopped the emulator's GS and IOP threads and
 * the EE thread never returns from Lua, so those buffers are dead weight.
 * Unmap and release every large direct-memory mapping in the emulator's heap
 * range that the loader did not allocate itself.
 */
#include "ghost.h"

struct vq_info2 {
    u64 start, end, offset;
    s32 prot, type;
    u8 flags;
    char name[32];
} __attribute__((packed));

bool direct_is_ours(u64 addr);

u64 host_release_emulator_memory(u64 min_size)
{
    int (*rel)(off_t, size_t) = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelReleaseDirectMemory");
    struct vq_info2 *info = arena_alloc(sizeof(*info) + 16);
    u64 freed = 0, addr = 0x200000000;
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
        int urc = K.munmap((void *)start, size);
        int rrc = rel ? rel((off_t)info->offset, size) : -1;
        logf("hostmem: released %lx-%lx (%lu MiB, prot %x type %d phys %lx) munmap %x release %x", start,
             info->end, size >> 20, info->prot, info->type, info->offset, urc, rrc);
        if (rrc == 0)
            freed += size;
    }
    /* SWRR's display buffers: close its video output first (the title opens
     * its own in sceVideoOutOpen), then release them too */
    s32 swrr_vo = *(volatile s32 *)(g_ctx->eboot_base + 0x2d695d0);
    int (*vo_close)(s32) = NULL;
    {
        void *host_real_in(const char *modname, const char *name);
        vo_close = (int (*)(s32))host_real_in("libSceVideoOut", "sceVideoOutClose");
    }
    if (swrr_vo > 0 && vo_close) {
        int c = vo_close(swrr_vo);
        logf("hostmem: closed SWRR video output %d -> %x", swrr_vo, c);
        if (c == 0) {
            *(volatile s32 *)(g_ctx->eboot_base + 0x2d695d0) = 0;
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
    u64 total = 0, off = 0;
    for (int i = 0; dmq && i < 256; i++) {
        memset(&q, 0, sizeof(q));
        if (dmq((off_t)off, 1 /* find next */, &q, sizeof(q)) != 0)
            break;
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
