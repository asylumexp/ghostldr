/*
 * freestanding: a payload with no libc and no imports (docs/elfldr.md),
 * built with LLVM alone.  The ELF loader calls its entry with ps5-payload-
 * sdk's payload_args_t: a userland dlsym, and no kernel access (the kernel
 * R/W fields are empty).  Everything it calls it looks up by name; the
 * value it returns is its exit code.
 *
 *     nc PS5 9021 < build/elfldr/freestanding.elf
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

static s64 (*w)(int fd, const void *buf, u64 n);

static u64 len(const char *s)
{
    u64 n = 0;
    while (s[n])
        n++;
    return n;
}

static void put(const char *s) { w(1, s, len(s)); }

static void put_num(u64 v)
{
    char b[24];
    int i = 23;
    b[i] = 0;
    do
        b[--i] = (char)('0' + v % 10);
    while (v /= 10);
    put(b + i);
}

int payload_main(struct el_args *a)
{
    if (a->sys_dynlib_dlsym(-1, "write", &w) != 0)
        return 1;
    int (*getpid)(void) = 0;
    u64 (*ptime)(void) = 0;
    int (*usleep)(unsigned) = 0;
    void *nothing = 0;
    a->sys_dynlib_dlsym(-1, "getpid", &getpid);
    a->sys_dynlib_dlsym(-1, "sceKernelGetProcessTime", &ptime);
    a->sys_dynlib_dlsym(-1, "sceKernelUsleep", &usleep);
    int none = a->sys_dynlib_dlsym(-1, "noSuchFunctionAnywhere", &nothing);
    put("freestanding: no libc, everything through sys_dynlib_dlsym\n");
    if (getpid) {
        put("freestanding: pid ");
        put_num((u64)getpid());
        put("\n");
    }
    if (ptime && usleep) {
        u64 t0 = ptime();
        usleep(250000);
        put("freestanding: slept ");
        put_num((ptime() - t0) / 1000);
        put(" ms\n");
    }
    put(none != 0 && !nothing ? "freestanding: an unknown name is not found, as it should be\n"
                              : "freestanding: an unknown name was found?\n");
    put(!a->rwpipe && !a->rwpair && !a->kpipe_addr && !a->kdata_base_addr
            ? "freestanding: no kernel R/W was given (userland only)\n"
            : "freestanding: kernel fields are set?\n");
    put("freestanding: returning 42\n");
    return 42;
}
