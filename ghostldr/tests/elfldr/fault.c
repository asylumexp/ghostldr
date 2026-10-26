/*
 * fault: a payload that crashes on purpose (docs/elfldr.md), to check that
 * a payload's fault ends its own thread and nothing else: the loader
 * reports it, SWRR and the other payloads go on.  LLVM alone, like
 * freestanding.c.
 *
 *     nc PS5 9021 < build/elfldr/fault.elf
 *
 * MIT license, like the rest of ghostldr.
 */
typedef unsigned long u64;
typedef long s64;

struct el_args {
    int (*sys_dynlib_dlsym)(int handle, const char *name, void *out);
    int *rwpipe, *rwpair;
    s64 kpipe_addr, kdata_base_addr;
    int *payloadout;
};

int payload_main(struct el_args *a)
{
    s64 (*w)(int, const void *, u64) = 0;
    a->sys_dynlib_dlsym(-1, "write", &w);
    static const char msg[] = "fault: writing to address 0 now\n";
    if (w)
        w(1, msg, sizeof(msg) - 1);
    *(volatile int *)0 = 1;
    return 0;
}
