/*
 * What differs on a PS4 (docs/ps4.md): the decisions, kept free of console
 * calls so tests/native_host/ps4_host.c can run them on the computer.
 * arena.c, hostmem.c and diag.c feed them what the consoles report.
 */
#include "ghost.h"

#define COLD GHOST_COLD

/* Bytes of the code region that boot.c fills through the compiler, from the
 * region's 64 KiB-aligned start: each prx image at 64 KiB alignment
 * (arena_code_alloc), then the executable's text and RELRO.  The
 * executable's data may lie past the region: it is punched, not written
 * through the compiler. */
COLD u64 jv_code_need(const struct module *mods)
{
    u64 need = 0;
    const struct module *main = NULL;
    for (const struct module *m = mods; m; m = m->next)
        if (m->is_main)
            main = m;
        else
            need = ALIGN_UP(need + m->image_size, 0x10000);
    if (!main)
        return need;
    u64 rx = main->text_size;
    for (u32 i = 0; i < main->phnum; i++)
        if (main->phdrs[i].type == PT_SCE_RELRO) {
            u64 end = ALIGN_UP(main->phdrs[i].vaddr + main->phdrs[i].memsz, 0x4000);
            rx = end > rx ? end : rx;
        }
    return need + rx;
}

/* hostmem.c on a PS4: keep the mapping [start, end) when it holds SWRR's
 * heap (the mspace at `heap`) and a thread may still be using that heap. */
COLD bool ps4_keep_heap(u64 start, u64 end, u64 heap, bool heap_idle)
{
    return heap && !heap_idle && heap >= start && heap < end;
}
