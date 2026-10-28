/*
 * libhbmod.prx and libhbdyn.prx: hbrel's own modules (hbrel.c).
 *
 *   libhbmod.prx  (default)      hbrel's executable imports it, so it loads
 *                                and starts at boot like any sce_module file
 *                                a title links against;
 *   libhbdyn.prx  (-DHBDYN)      nothing imports it: hbrel loads it at run
 *                                time with sceKernelLoadStartModule and finds
 *                                its functions with sceKernelDlsym.  It
 *                                imports from libhbmod.prx (a module that is
 *                                already loaded) and from libkernel, and has
 *                                a thread-local variable.  Linked -pie, as
 *                                OpenOrbis links a library, its accesses are
 *                                local-exec: on a PS4 the variable is 4
 *                                bytes below the thread pointer, in the
 *                                executable's TLS, and its first value 7 is
 *                                never copied anywhere.
 *
 * Linked as OpenOrbis links a library (crtlib.o: DT_INIT is its _init, which
 * returns 0).  No libc: only libkernel.  MIT license, like the rest of
 * ghostldr.
 */
#include <stddef.h>
#include <stdint.h>

uint64_t sceKernelGetProcessTime(void);

#ifndef HBDYN

int hbmod_value = 42;

int hbmod_add(int a, int b) { return a + b + hbmod_value - 42; }

int hbmod_calls(void)
{
    static int n;
    return ++n;
}

uint64_t hbmod_time(void) { return sceKernelGetProcessTime(); }

#else

int hbmod_add(int a, int b);

int hbdyn_value = 1234;
static __thread int tls_counter = 7;
static const char k_name[] = "libhbdyn";

int hbdyn_mul(int a, int b) { return a * b; }

/* through the boot module's export */
int hbdyn_chain(int a) { return hbmod_add(a, 1); }

/* the thread-local variable */
int hbdyn_tls(void) { return ++tls_counter; }
void hbdyn_tls_set(int v) { tls_counter = v; }

const char *hbdyn_name(void) { return k_name; }

uint64_t hbdyn_time(void) { return sceKernelGetProcessTime(); }

#endif
