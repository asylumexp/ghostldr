/*
 * The ELF loader on the console (elfldr.h, docs/elfldr.md): where payloads
 * live, how they run, what they import instead of SWRR's own functions, and
 * freeing them.  No title runs; the core's boot thread serves (elfldr.c)
 * and never goes back to Luac0re's Lua until told to quit, so the JIT
 * compiler process is the core's alone: payloads may have sockets made
 * there from any thread, one at a time (rpc_lock).
 *
 * Executable memory: a payload's code and read-only data go at the end of
 * one of the emulator's own JIT objects (32, 16, 8 or 8 MiB; never the IPU
 * object that holds the core), written through the compiler at the
 * object's home, and run from a view of the whole object mapped elsewhere,
 * above 0x10_0000_0000, in an address range reserved for the payload.  Its
 * data is direct memory right after the view.  A payload holds its object
 * until it ends, so up to four run at once; nothing of the JIT budget
 * (768 KiB left on a 13.60 PS5) is used.  At the end the view, the data and
 * the range are given back.  No memory is ever both writable and
 * executable here, and nothing pretends to be a kernel: a payload has
 * SWRR's privileges, and a call that needs more fails as it would in SWRR.
 *
 * A payload runs on a thread of its own with a TLS block for its image
 * (tls.c).  It has finished when none of its threads runs any more: its
 * main function returned, it called exit/_exit (that thread ends; SWRR
 * does not), or its threads faulted (crash.c ends the thread instead of
 * parking it).  Sockets it made or accepted and did not close are closed
 * then.  Its stdout and stderr go to the connection it came on
 * (hle_file.c) and to the UDP log.
 */
#include "ext.h"
#include "ftp.h"
#include "elfldr.h"

int net_setup(void) HIDDEN;                              /* ftp_sys.c */
int jit_socket(int domain, int type, int proto) HIDDEN; /* ftp_sys.c */

#define MAX_OBJS 4
#define VA_HINT 0x2000000000UL /* where payload ranges are reserved (a hint) */
#define VA_HIGH 0x1000000000UL /* fixed direct memory maps only above this */
#define MAIN_STACK (2u << 20)
#define GRACE_MS 500           /* a payload's last thread has left its code by then */
#define MAX_FDS 64
#define MAX_LIBS_KEPT 16
#define NO_CODE 0x7FFFABCD     /* payloadout untouched */
#define ENOSYS_ 78
#define SCE_NET_ENOSYS ((s32)0x8041014E)

struct el_payload {
    struct module m; /* first: the module tls_owner() names is the payload */
    unsigned id;
    int conn;
    volatile int conn_ok, out_lock;
    struct pl_info info;
    int obj;              /* g_obj index, -1: none */
    u64 va, va_size;      /* the reserved range: the view, then the data */
    u64 data;             /* direct memory at the image's exec_end, 0: none */
    char name[24];
    const char *libs[MAX_LIBS_KEPT]; /* DT_NEEDED as module names (dlsym, unresolved stubs) */
    u32 nlibs;
    u32 imports, unresolved;
    char missing[160];    /* the first unresolved names */
    struct {
        u64 argc;
        const char *argv[2];
        const char *envp[1];
        u64 auxv[2];
    } oargs;
    struct el_args fargs;
    int out_code;
    volatile int returned, exited, crashed;
    s32 code;
    int sig;
    u64 fault_rip, fault_addr;
    u64 t_start, t_done;
    volatile int fd_lock;
    int fds[MAX_FDS];
    u32 nfds;
    volatile int map_lock;
    struct pl_map *maps; /* MAX_MAPS (page_alloc'd on the first) */
    u32 nmaps;
    u64 mapped;          /* bytes in maps */
};

/* anonymous memory a payload maps: direct memory, given back when it ends */
struct pl_map {
    u64 addr, size;
};
#define MAX_MAPS 1024

static struct jit_obj g_obj[MAX_OBJS];
static struct el_payload *g_user[MAX_OBJS];
static u32 g_nobj;

static u64 now_ms(void)
{
    static u64 (*ptime)(void);
    if (!ptime)
        ptime = host_real("sceKernelGetProcessTime");
    return ptime ? ptime() / 1000 : 0;
}

static struct el_payload *current(void)
{
    struct module *m = tls_owner ? tls_owner() : NULL; /* (a test harness's core may have none) */
    return m && m->payload ? (struct el_payload *)m : NULL;
}

/* ---- sockets a payload makes, closed when it ends ---- */

static void fd_track(struct el_payload *p, int fd)
{
    if (!p || fd < 0)
        return;
    spin_lock(&p->fd_lock);
    if (p->nfds < MAX_FDS)
        p->fds[p->nfds++] = fd;
    spin_unlock(&p->fd_lock);
}

static void fd_untrack(struct el_payload *p, int fd)
{
    if (!p)
        return;
    spin_lock(&p->fd_lock);
    for (u32 i = 0; i < p->nfds; i++)
        if (p->fds[i] == fd) {
            p->fds[i] = p->fds[--p->nfds];
            break;
        }
    spin_unlock(&p->fd_lock);
}

/* ---- what payloads import instead of SWRR's own ---- */

typedef s64 (*fn6)(u64, u64, u64, u64, u64, u64);

static fn6 host_fn(void *volatile *slot, const char *mod, const char *name)
{
    void *f = *slot;
    if (!f) {
        f = mod ? host_real_in(mod, name) : host_real(name);
        *slot = f ? f : (void *)1;
    }
    return f == (void *)1 ? NULL : (fn6)f;
}
#define HOST(mod, name)          \
    static void *volatile slot_; \
    fn6 real = host_fn(&slot_, mod, name)

/* this thread of the payload ends; SWRR goes on */
__attribute__((noreturn)) static void end_thread(void)
{
    void (*dtors)(void) = g_thread_dtors;
    if (dtors)
        dtors();
    tls_thread_exit();
    void (*ex)(void *) = host_real("scePthreadExit");
    if (ex)
        ex(NULL);
    for (;;)
        K.usleep(1000000);
}

__attribute__((noreturn)) static void pl_exit_code(int code, const char *how)
{
    struct el_payload *p = current();
    if (p && !p->exited && !p->crashed)
        p->code = code, p->exited = 1;
    logf("elfldr: payload %u: %s(%d): its thread ends, SWRR does not", p ? p->id : 0, how, code);
    end_thread();
}

static void pl__exit(int code) { pl_exit_code(code, "_exit"); }
static void pl_exit(int code) { pl_exit_code(code, "exit"); }

static int pl_raise(int sig)
{
    if (!sig)
        return 0;
    pl_exit_code(128 + sig, "raise");
}

static void pl_abort(void) { pl_exit_code(128 + 6, "abort"); }

/* SWRR's socket() answers ENOSYS for internet sockets on a PS5 at 8.00 or
 * later: those come from the compiler (ftp_sys.c jit_socket) */
static int pl_socket(int domain, int type, int proto)
{
    HOST(NULL, "socket");
    int fd = real ? (int)real((u64)domain, (u64)type, (u64)proto, 0, 0, 0) : -1;
    if (fd < 0 && (!real || *K.errno_fn() == ENOSYS_)) {
        fd = jit_socket(domain, type, proto);
        if (fd < 0)
            *K.errno_fn() = -fd, fd = -1;
    }
    fd_track(current(), fd);
    return fd;
}

static int pl_socketex(const char *name, int domain, int type, int proto)
{
    HOST(NULL, "__sys_socketex");
    int fd = real ? (int)real((u64)name, (u64)domain, (u64)type, (u64)proto, 0, 0) : -1;
    if (fd < 0 && (!real || *K.errno_fn() == ENOSYS_)) {
        fd = jit_socket(domain, type, proto);
        if (fd < 0)
            *K.errno_fn() = -fd, fd = -1;
    }
    fd_track(current(), fd);
    return fd;
}

static int pl_net_socket(const char *name, int family, int type, int proto)
{
    HOST("libSceNet", "sceNetSocket");
    int fd = real ? (int)real((u64)name, (u64)family, (u64)type, (u64)proto, 0, 0) : SCE_NET_ENOSYS;
    if (fd == SCE_NET_ENOSYS) {
        fd = jit_socket(family, type, proto);
        if (fd < 0)
            fd = (int)(0x80410100u | (u32)-fd);
    }
    fd_track(current(), fd);
    return fd;
}

static int pl_accept(int s, void *addr, u32 *len)
{
    HOST(NULL, "accept");
    int fd = real ? (int)real((u64)s, (u64)addr, (u64)len, 0, 0, 0) : -1;
    fd_track(current(), fd);
    return fd;
}

static int pl_net_accept(int s, void *addr, u32 *len)
{
    HOST("libSceNet", "sceNetAccept");
    int fd = real ? (int)real((u64)s, (u64)addr, (u64)len, 0, 0, 0) : (int)0x80410116;
    fd_track(current(), fd);
    return fd;
}

/* ---- anonymous memory: direct memory (SWRR has little flexible memory
 * left), each block remembered so the payload's end gives it back ---- */

#define MAP_FAILED_ ((void *)-1)
#define ENOMEM_ 12

static void *map_anon(struct el_payload *p, u64 len, int prot)
{
    u64 size = ALIGN_UP(len, 0x4000);
    spin_lock(&p->map_lock);
    if (!p->maps)
        p->maps = page_alloc(MAX_MAPS * sizeof(struct pl_map));
    bool room = p->maps && p->nmaps < MAX_MAPS;
    spin_unlock(&p->map_lock);
    void *a = room && len ? direct_alloc(size, PROT_READ | PROT_WRITE, 0) : NULL;
    if (!a)
        return NULL;
    memset(a, 0, size);
    if (prot != (PROT_READ | PROT_WRITE) && K.mprotect(a, size, prot) != 0) {
        direct_free(a);
        return NULL;
    }
    spin_lock(&p->map_lock);
    p->maps[p->nmaps++] = (struct pl_map){(u64)a, size};
    p->mapped += size;
    spin_unlock(&p->map_lock);
    return a;
}

/* 1: [addr, addr+len) was in one of p's blocks (freed, or unmapped in part) */
static int unmap_anon(struct el_payload *p, u64 addr, u64 len)
{
    int found = 0;
    spin_lock(&p->map_lock);
    for (u32 i = 0; i < p->nmaps && !found; i++) {
        struct pl_map *b = &p->maps[i];
        if (addr < b->addr || addr >= b->addr + b->size)
            continue;
        found = 1;
        if (addr == b->addr && ALIGN_UP(len, 0x4000) >= b->size) {
            u64 a = b->addr;
            p->mapped -= b->size;
            *b = p->maps[--p->nmaps];
            spin_unlock(&p->map_lock);
            direct_free((void *)a);
            return 1;
        }
    }
    spin_unlock(&p->map_lock);
    if (found) /* a part: its pages go, the block's memory comes back at the end */
        K.munmap((void *)addr, ALIGN_UP(len, 0x4000));
    return found;
}

static void *pl_mmap(void *addr, u64 len, int prot, int flags, int fd, s64 off)
{
    static void *volatile slot_;
    fn6 real = host_fn(&slot_, NULL, "mmap");
    struct el_payload *p = current();
    if (p && (flags & MAP_ANON) && !(flags & MAP_FIXED) && !(prot & PROT_EXEC)) {
        void *a = map_anon(p, len, prot);
        if (!a)
            *K.errno_fn() = ENOMEM_;
        return a ? a : MAP_FAILED_;
    }
    char nid[12]; /* a file: as the file family maps it */
    compute_nid("mmap", nid);
    u64 f = ext_find("libkernel", nid);
    if (f)
        return ((void *(*)(void *, u64, int, int, int, s64))f)(addr, len, prot, flags, fd, off);
    return real ? (void *)real((u64)addr, len, (u64)prot, (u64)flags, (u64)fd, (u64)off) : MAP_FAILED_;
}

static int pl_munmap(void *addr, u64 len)
{
    HOST(NULL, "munmap");
    struct el_payload *p = current();
    if (p && unmap_anon(p, (u64)addr, len))
        return 0;
    return real ? (int)real((u64)addr, len, 0, 0, 0, 0) : -1;
}

static s32 pl_k_munmap(void *addr, u64 len)
{
    HOST(NULL, "sceKernelMunmap");
    struct el_payload *p = current();
    if (p && unmap_anon(p, (u64)addr, len))
        return 0;
    return real ? (s32)real((u64)addr, len, 0, 0, 0, 0) : (s32)0x80020016;
}

/* sceKernelMapFlexibleMemory and its named form: direct memory as well */
static s32 pl_map_flex(void **addr, u64 len, int prot, int flags)
{
    struct el_payload *p = current();
    if (!p || !addr || (flags & MAP_FIXED) || (prot & PROT_EXEC))
        return flex_map ? flex_map(addr, len, prot, flags) : (s32)0x80020016;
    void *a = map_anon(p, len, prot ? prot : PROT_READ | PROT_WRITE);
    if (!a)
        return (s32)0x8002000C;
    *addr = a;
    return 0;
}

static s32 pl_map_named_flex(void **addr, u64 len, int prot, int flags, const char *name)
{
    return pl_map_flex(addr, len, prot, flags);
}

/* the close the payload would have had (the file family's, else SWRR's) */
static fn6 plain_close(void *volatile *slot, const char *lib, const char *name)
{
    void *f = *slot;
    if (!f) {
        char nid[12];
        compute_nid(name, nid);
        u64 a = ext_find(lib, nid);
        if (!a)
            a = (u64)(lib && strcmp(lib, "libSceNet") == 0 ? host_real_in(lib, name) : host_real(name));
        *slot = f = a ? (void *)a : (void *)1;
    }
    return f == (void *)1 ? NULL : (fn6)f;
}
#define CLOSE(fn, lib, name)                                         \
    static s64 fn(int fd)                                            \
    {                                                                \
        static void *volatile slot_;                                 \
        fn6 real = plain_close(&slot_, lib, name);                   \
        fd_untrack(current(), fd);                                   \
        return real ? real((u64)fd, 0, 0, 0, 0, 0) : -1;             \
    }
CLOSE(pl_close, "libkernel", "close")
CLOSE(pl__close, "libkernel", "_close")
CLOSE(pl_kclose, "libkernel", "sceKernelClose")
CLOSE(pl_net_close, "libSceNet", "sceNetSocketClose")

static const struct {
    const char *name;
    void *fn;
} k_over[] = {
    {"_exit", pl__exit},
    {"_Exit", pl__exit},
    {"exit", pl_exit},
    {"raise", pl_raise},
    {"abort", pl_abort},
    {"socket", pl_socket},
    {"__sys_socketex", pl_socketex},
    {"sceNetSocket", pl_net_socket},
    {"accept", pl_accept},
    {"sceNetAccept", pl_net_accept},
    {"close", pl_close},
    {"_close", pl__close},
    {"sceKernelClose", pl_kclose},
    {"sceNetSocketClose", pl_net_close},
    {"mmap", pl_mmap},
    {"munmap", pl_munmap},
    {"sceKernelMunmap", pl_k_munmap},
    {"sceKernelMapFlexibleMemory", pl_map_flex},
    {"sceKernelMapNamedFlexibleMemory", pl_map_named_flex},
};

static u64 override(const char *name)
{
    for (u32 i = 0; name && i < ARRAY_SIZE(k_over); i++)
        if (strcmp(k_over[i].name, name) == 0)
            return (u64)k_over[i].fn;
    return 0;
}

/* A name as a title's import of it binds (resolve.c), for an ELF that names
 * no library per symbol: the extension (DT_NEEDED's libraries first), the
 * core's overrides, then SWRR's modules (handle first: dlsym's), unless
 * the extension's policy keeps a library from the host. */
static u64 resolve_name(struct el_payload *p, const char *name, int handle)
{
    u64 a = override(name);
    if (a)
        return a;
    char nid[12];
    compute_nid(name, nid);
    for (u32 i = 0; p && i < p->nlibs; i++)
        if ((a = ext_find(p->libs[i], nid)))
            return a;
    if ((a = ext_find(NULL, nid)) || (a = hle_lookup(NULL, name)))
        return a;
    if (handle > 0 && (a = (u64)host_dlsym((u32)handle, name)))
        return a;
    static const char *const k_fallback[] = {"libkernel", "libSceLibcInternal"};
    for (u32 i = 0; i < (p ? p->nlibs : 0) + ARRAY_SIZE(k_fallback); i++) {
        const char *lib = p && i < p->nlibs ? p->libs[i] : k_fallback[i - (p ? p->nlibs : 0)];
        const struct ext_lib *pol = ext_find_policy(lib);
        u32 h;
        if (!(pol && (pol->flags & EXT_LIB_NO_HOST)) && host_module_handle(lib, &h) == 0 &&
            (a = (u64)host_dlsym(h, name)))
            return a;
    }
    return 0;
}

/* sys_dynlib_dlsym for freestanding payloads: SWRR's modules, as imports bind */
static int el_dlsym(int handle, const char *name, void *out)
{
    u64 a = name ? resolve_name(current(), name, handle) : 0;
    if (out)
        *(u64 *)out = a;
    return a ? 0 : -1;
}

static void note_missing(struct el_payload *p, const char *name)
{
    p->unresolved++;
    size_t have = strlen(p->missing);
    if (have + strlen(name) + 3 < sizeof(p->missing))
        fmt(p->missing + have, sizeof(p->missing) - have, "%s%s", have ? ", " : "", name);
    logf("elfldr: payload %u: %s is not there: calls to it return 0", p->id, name);
}

/* pl_build's resolver */
static u64 el_resolve(void *ctx, struct module *m, const char *name, bool weak, int kind)
{
    struct el_payload *p = ctx;
    p->imports++;
    if (p->info.sce) { /* NID#L#M, bound as a title's import is (resolve.c) */
        char nid[12];
        u32 k = 0;
        for (; k < 11 && name[k] && name[k] != '#'; k++)
            nid[k] = name[k];
        nid[k] = 0;
        u64 a = override(nid_to_name(nid));
        return a ? a : resolve_import(m, name, weak, kind);
    }
    u64 a = resolve_name(p, name, 0);
    if (a || weak)
        return a; /* an absent weak import is NULL, as for any ELF */
    note_missing(p, name);
    if (kind == IMPORT_OBJECT)
        return 0;
    char nid[12];
    compute_nid(name, nid);
    const char *lib = p->nlibs ? p->libs[0] : "libkernel";
    const struct ext_lib *pol = ext_find_policy(lib);
    return unresolved_stub(lib, nid, lib, pol ? pol->stub_ret : 0, false);
}

/* ---- the JIT objects ---- */

/* Whether names can be found for NIDs: the extension's NID table.  The ELF
 * loader's own bundle (ghostldr-elfldr.bin) carries none: an ELF names its
 * imports, which is all it needs. */
static bool have_names(void)
{
    char nid[12];
    compute_nid("sceKernelUsleep", nid);
    return nid_to_name(nid) != NULL;
}

int el_init(char *why, unsigned n)
{
    u64 st[8], en[8];
    u32 nb = arena_buffers(st, en, ARRAY_SIZE(st));
    for (u32 i = 0; i < nb && g_nobj < MAX_OBJS; i++)
        g_obj[g_nobj++] = (struct jit_obj){st[i], en[i] - st[i], 0, -1, -1};
    if (!g_nobj) {
        fmt(why, n, "the emulator's JIT buffers were not found");
        return -1;
    }
    rpc_lock();
    jit_objects_find(g_obj, g_nobj, (1u << g_nobj) - 1);
    rpc_unlock();
    u32 usable = 0;
    for (u32 i = 0; i < g_nobj; i++) {
        bool ok = g_obj[i].rx >= 0 && g_obj[i].wr == g_obj[i].home;
        usable += ok;
        logf("elfldr: JIT object %u: %lu MiB at %lx%s", i, g_obj[i].size >> 20, g_obj[i].home,
             ok ? "" : ": no descriptor for it, not used");
        if (!ok)
            g_obj[i].rx = -1;
    }
    if (!usable) {
        fmt(why, n, "no descriptor for any of the emulator's JIT objects");
        return -1;
    }
    tls_static_reserve(PL_TLS_MAX, PL_TLS_ALIGN);
    /* Logged only: whether this console makes ordinary memory executable.
     * A stock kernel refuses; a kernel patch (kstuff) grants it.  Payload
     * code runs from the JIT object views either way. */
    void *probe = direct_alloc(0x4000, PROT_READ | PROT_WRITE, 0);
    if (probe) {
        int rc = K.mprotect(probe, 0x4000, PROT_READ | PROT_EXEC);
        logf("elfldr: mprotect(PROT_EXEC) of direct memory: %s (%x); payload code runs only from JIT object views",
             rc == 0 ? "granted: a kernel patch is loaded" : "refused, as by a stock kernel", rc);
        direct_free(probe);
    }
    off_t start = 0;
    size_t avail = 0, flex = 0;
    K.avail_direct(0, (off_t)K.direct_size(), 0x4000, &start, &avail);
    K.avail_flex(&flex);
    logf("elfldr: %u JIT objects for payloads; free memory: %lu MiB direct (largest block), %lu MiB flexible%s",
         usable, (u64)avail >> 20, (u64)flex >> 20,
         have_names() ? "" : "; no NID table: ELFs only, not SELFs or OELFs");
    return 0;
}

void el_fini(void)
{
    for (u32 i = 0; i < g_nobj; i++)
        if (g_obj[i].rx >= 0)
            K.close(g_obj[i].rx);
    g_nobj = 0;
}

/* ---- a payload's life ---- */

static void on_fault(struct module *m, int sig, u64 rip, u64 addr)
{
    struct el_payload *p = (struct el_payload *)m;
    if (!p->crashed && !p->exited)
        p->crashed = 1, p->sig = sig, p->fault_rip = rip, p->fault_addr = addr;
    if (p->conn_ok) { /* now, from the signal handler: the client may be waiting for output */
        char line[200];
        bool in = rip >= m->base && rip - m->base < m->image_size;
        int k = fmt(line, sizeof(line), "[elfldr] payload %u: a thread faulted: signal %d at %s%lx (address %lx); "
                                        "it ends, SWRR does not\n",
                    p->id, sig, in ? "+" : "", in ? rip - m->base : rip, addr);
        K.sendto(p->conn, line, (u64)k, 0, NULL, 0);
    }
}

/* everything a payload had, back */
static void release(struct el_payload *p)
{
    spin_lock(&p->fd_lock);
    for (u32 i = 0; i < p->nfds; i++) {
        logf("elfldr: payload %u: closing its socket %d", p->id, p->fds[i]);
        K.close(p->fds[i]);
    }
    p->nfds = 0;
    spin_unlock(&p->fd_lock);
    if (p->maps) {
        if (p->nmaps)
            logf("elfldr: payload %u: giving back %u blocks (%lu KiB) it mapped", p->id, p->nmaps, p->mapped >> 10);
        for (u32 i = 0; i < p->nmaps; i++)
            direct_free((void *)p->maps[i].addr);
        page_free(p->maps, MAX_MAPS * sizeof(struct pl_map));
        p->maps = NULL, p->nmaps = 0, p->mapped = 0;
    }
    u64 dt = (u64)g_thread_dtors;
    if (dt && p->m.base && dt - p->m.base < p->info.image_size)
        g_thread_dtors = NULL;
    if (p->m.base) {
        rpc_lock();
        module_unpublish(&p->m);
        rpc_unlock();
    }
    if (p->data)
        direct_free((void *)p->data);
    if (p->va)
        K.munmap((void *)p->va, p->va_size);
    if (p->obj >= 0)
        g_user[p->obj] = NULL;
    p->data = p->va = 0;
    p->obj = -1;
}

/* DT_NEEDED's files as module names ("libkernel.so" -> "libkernel"): where
 * its imports are looked for, also after the file is gone (dlsym) */
static void keep_libs(struct el_payload *p)
{
    struct module *m = &p->m;
    for (u32 i = 0; i < m->n_needed_files && p->nlibs < MAX_LIBS_KEPT; i++) {
        char lib[96];
        strlcpy(lib, m->needed_files[i], sizeof(lib));
        for (char *dot = lib; *dot; dot++)
            if (*dot == '.') {
                *dot = 0;
                break;
            }
        p->libs[p->nlibs++] = arena_strdup(lib);
    }
}

/* the file is gone after loading: nothing may point into it */
static void forget_file(struct el_payload *p)
{
    struct module *m = &p->m;
    m->file = NULL, m->file_size = 0;
    m->phdrs = NULL, m->phnum = 0;
    for (u32 i = 0; i < ARRAY_SIZE(m->seg_data); i++)
        m->seg_data[i] = NULL;
    m->dynlib = NULL, m->strtab = NULL, m->symtab = NULL, m->rela = m->jmprel = NULL;
    m->strsz = m->nsyms = m->nrela = m->njmprel = 0;
    m->n_import_libs = m->n_export_libs = m->n_needed_mods = m->n_needed_files = 0;
    m->modname = m->name;
}

int el_load(const unsigned char *buf, unsigned long len, unsigned id, int conn, struct el_payload **out, char *info,
            unsigned n)
{
    struct el_payload *p = arena_alloc(sizeof(*p));
    u8 *file = p ? page_alloc(len) : NULL;
    if (!file) {
        fmt(info, n, "no memory for it");
        return -1;
    }
    memcpy(file, buf, len);
    fmt(p->name, sizeof(p->name), "payload%u", id);
    struct module *m = &p->m;
    const struct pl_info *pi = &p->info;
    m->name = m->modname = p->name;
    m->payload = true;
    p->id = id, p->conn = conn, p->conn_ok = 1, p->obj = -1;
    u8 *img = NULL;
    int rc = -1;
    if (pl_parse(m, file, len, &p->info, info, n) != 0)
        goto out;
    if (p->info.sce && !have_names()) { /* its imports are NIDs: SWRR's functions are found by name */
        fmt(info, n, "a SELF or an OELF names its imports by NID only, and this ELF loader has no NID table "
                     "(ghostldr-elfldr.bin leaves it out): send the ELF ld.lld wrote, before create-fself, or "
                     "start the loader with run.py elfldr");
        goto out;
    }
    keep_libs(p);
    /* the smallest free JIT object its code fits, past the object's header */
    int best = -1;
    for (u32 i = 0; i < g_nobj; i++)
        if (!g_user[i] && g_obj[i].rx >= 0 && g_obj[i].size >= pi->exec_end + PL_HEADER &&
            (best < 0 || g_obj[i].size < g_obj[best].size))
            best = (int)i;
    if (best < 0) {
        u32 busy = 0;
        for (u32 i = 0; i < g_nobj; i++)
            busy += g_user[i] != NULL;
        fmt(info, n, "%s for its %lu KiB of code: %u payload%s hold%s the emulator's JIT objects (one each)",
            busy ? "no JIT object free" : "no JIT object large enough", pi->exec_end >> 10, busy,
            busy == 1 ? "" : "s", busy == 1 ? "s" : "");
        goto out;
    }
    const struct jit_obj *o = &g_obj[best];
    u64 data = pi->image_size - pi->exec_end;
    void *va = (void *)(VA_HINT + (u64)best * 0x100000000UL);
    p->va_size = ALIGN_UP(o->size + data, 0x200000);
    if (K.reserve_va(&va, p->va_size, 0, 0x200000) != 0 || (u64)va < VA_HIGH) {
        fmt(info, n, "no address range of %lu MiB above 0x%lx (got %p)", p->va_size >> 20, VA_HIGH, va);
        if (va && (u64)va < VA_HIGH)
            K.munmap(va, p->va_size);
        goto out;
    }
    p->va = (u64)va;
    g_user[best] = p, p->obj = best;
    logf("elfldr: payload %u: the %lu MiB JIT object at %lx, viewed at %p; code %lx bytes, data %lx", id,
         o->size >> 20, o->home, va, pi->exec_end, data);
    void *res = NULL;
    if (K.mmap(va, o->size, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED, o->rx, 0, &res) != 0 || res != va) {
        fmt(info, n, "the view of the %lu MiB JIT object could not be mapped at %p", o->size >> 20, va);
        goto out;
    }
    m->base = p->va + o->size - pi->exec_end;
    if (data) {
        void *d = direct_alloc(data, PROT_READ | PROT_WRITE, m->base + pi->exec_end);
        if ((u64)d != m->base + pi->exec_end) {
            fmt(info, n, "no direct memory for its %lu KiB of data", data >> 10);
            if (d)
                direct_free(d);
            goto out;
        }
        p->data = (u64)d;
        memset(d, 0, data);
    }
    if (m->tls_memsz)
        m->tls_offset = ALIGN_UP(m->tls_memsz, m->tls_align > 16 ? m->tls_align : 16);
    rpc_lock();
    module_publish(m); /* its handle and TLS id */
    rpc_unlock();
    img = page_alloc(pi->image_size);
    if (!img) {
        fmt(info, n, "no memory to build its %lu KiB image", pi->image_size >> 10);
        goto out;
    }
    char tls[48];
    if (pl_build(m, pi, img, el_resolve, p, tls, sizeof(tls)) != 0) {
        strlcpy(info, tls, n);
        goto out;
    }
    logf("elfldr: payload %u: relocated for %lx (%u imports); writing its code through the compiler at %lx", id,
         m->base, p->imports, o->wr + o->size - pi->exec_end);
    rpc_lock();
    stubs_flush();
    jit_copy_to(o->wr + o->size - pi->exec_end, img, pi->exec_end);
    rpc_unlock();
    if (memcmp((const void *)m->base, img, pi->exec_end) != 0) {
        fmt(info, n, "its code, written through the compiler at %lx, does not show in the view at %lx",
            o->wr + o->size - pi->exec_end, m->base);
        goto out;
    }
    if (data)
        memcpy((void *)(m->base + pi->exec_end), img + pi->exec_end, data);
    if (pi->relro_hi > pi->relro_lo)
        K.mprotect((void *)(m->base + pi->relro_lo), pi->relro_hi - pi->relro_lo, PROT_READ);
    forget_file(p);
    int k = fmt(info, n, "payload %u: %s %s, %lu KiB of code at %lx (in the %lu MiB JIT object), %lu KiB of data, "
                         "%u imports",
                id, pi->sce ? "Orbis" : "ELF", pi->abi == PL_FREE ? "(freestanding)" : "(OpenOrbis/PS4 ABI)",
                pi->exec_end >> 10, m->base, o->size >> 20, data >> 10, p->imports);
    if (p->unresolved)
        fmt(info + k, n - (u32)k, ", %u not found (calls return 0): %s", p->unresolved, p->missing);
    rc = 0;
out:
    if (img)
        page_free(img, pi->image_size);
    page_free(file, len);
    if (rc != 0) {
        release(p);
        return rc;
    }
    *out = p; /* elfldr.c logs and sends info */
    return 0;
}

static void pl_atexit(void) {}

static void *pl_main(void *arg)
{
    struct el_payload *p = arg;
    u64 entry = p->m.base + p->m.entry;
    if (p->info.abi == PL_ORBIS) { /* as boot.c starts a title: crt1's _start(args, atexit) */
        p->oargs.argc = 1;
        p->oargs.argv[0] = p->name;
        ((void (*)(void *, void (*)(void)))entry)(&p->oargs, pl_atexit);
        p->code = 0;
    } else {
        p->out_code = NO_CODE;
        p->fargs = (struct el_args){el_dlsym, NULL, NULL, 0, 0, &p->out_code};
        int rc = ((int (*)(struct el_args *))entry)(&p->fargs);
        p->code = p->out_code != NO_CODE ? p->out_code : rc;
    }
    p->returned = 1;
    return NULL;
}

int el_run(struct el_payload *p, char *why, unsigned n)
{
    u8 attr[0x40];
    u64 tid = 0;
    K.pthread_attr_init(attr);
    K.pthread_attr_setstacksize(attr, MAIN_STACK);
    int (*detach)(void *, int) = host_real("scePthreadAttrSetdetachstate");
    if (detach)
        detach(attr, 1 /* PTHREAD_CREATE_DETACHED */);
    p->t_start = now_ms();
    logf("elfldr: payload %u: starting at %lx (%s entry)", p->id, p->m.base + p->m.entry,
         p->info.abi == PL_ORBIS ? "crt1 _start" : "freestanding");
    int rc = payload_thread_start(&p->m, &tid, attr, pl_main, p, p->name);
    K.pthread_attr_destroy(attr);
    if (rc != 0) {
        fmt(why, n, "no thread for it (%x)", rc);
        release(p);
        return -1;
    }
    return 0;
}

int el_poll(struct el_payload *p, char *line, unsigned n)
{
    if (__atomic_load_n(&p->m.live_threads, __ATOMIC_ACQUIRE) > 0)
        return 0;
    u64 now = now_ms();
    if (!p->t_done) {
        p->t_done = now ? now : 1;
        return 0;
    }
    if (now - p->t_done < GRACE_MS)
        return 0;
    u64 ms = p->t_done - p->t_start;
    if (p->crashed) {
        bool in = p->fault_rip >= p->m.base && p->fault_rip - p->m.base < p->info.image_size;
        fmt(line, n, "payload %u crashed after %lu.%03lu s: signal %d at %s%lx (address %lx)", p->id, ms / 1000,
            ms % 1000, p->sig, in ? "+" : "", in ? p->fault_rip - p->m.base : p->fault_rip, p->fault_addr);
    } else {
        fmt(line, n, "payload %u %s %d after %lu.%03lu s", p->id,
            p->exited ? "exited with code" : p->returned ? "returned" : "ended, code", p->code, ms / 1000, ms % 1000);
    }
    release(p);
    off_t start = 0;
    size_t avail = 0, flex = 0;
    K.avail_direct(0, (off_t)K.direct_size(), 0x4000, &start, &avail);
    K.avail_flex(&flex);
    logf("elfldr: payload %u freed; free now: %lu MiB direct (largest block), %lu KiB flexible", p->id,
         (u64)avail >> 20, (u64)flex >> 10);
    return 1;
}

void el_describe(struct el_payload *p, char *line, unsigned n)
{
    u64 ms = now_ms() - p->t_start;
    fmt(line, n, "payload %u: %d thread%s, running %lu s, code at %lx (%lu KiB)%s", p->id, p->m.live_threads,
        p->m.live_threads == 1 ? "" : "s", ms / 1000, p->m.base, p->info.exec_end >> 10,
        p->crashed ? ", a thread faulted" : p->exited ? ", exit called" : "");
}

/* stdout and stderr of a payload's thread (hle_file.c): to its connection
 * too.  The payload's id, or 0 when the thread is no payload's. */
unsigned elfldr_output(int fd, const void *buf, u64 len) HIDDEN;
unsigned elfldr_output(int fd, const void *buf, u64 len)
{
    struct el_payload *p = current();
    if (!p)
        return 0;
    if (p->conn_ok && len) {
        while (__atomic_exchange_n(&p->out_lock, 1, __ATOMIC_ACQUIRE))
            K.usleep(200);
        if (fv_send(p->conn, buf, len) < 0)
            p->conn_ok = 0;
        __atomic_store_n(&p->out_lock, 0, __ATOMIC_RELEASE);
    }
    return p->id;
}

/* ---- the extension's entry (ext_exports.elfldr_main) ---- */

int pl_parse_sce(struct module *m, u8 *file, u64 size) { return module_parse(m, file, size); }

/* ---- the TV and ghostldr.lua (elfldr.h) ---- */

static u32 g_port;
static bool g_can_boot;

/* the message dialog Luac0re's loader shows its own text in (its
 * show_dialog: Terminate, Initialize, Open with an OK button), opened again
 * for each change */
static void dialog(const char *text)
{
    static u8 param[0x88 + 0x20] __attribute__((aligned(16)));
    s32 (*term)(void) = host_real_in("libSceMsgDialog", "sceMsgDialogTerminate");
    s32 (*init)(void) = host_real_in("libSceMsgDialog", "sceMsgDialogInitialize");
    s32 (*open)(void *) = host_real_in("libSceMsgDialog", "sceMsgDialogOpen");
    s32 (*user)(s32 *) = host_real_in("libSceUserService", "sceUserServiceGetInitialUser");
    s32 uid = 0;
    if (!term || !init || !open || !user || user(&uid) != 0)
        return;
    term();
    init();
    u8 *p = param, *m = param + 0x88;
    memset(param, 0, sizeof(param));
    *(u64 *)(p + 0x00) = 0x30;                         /* baseParam.size */
    *(u32 *)(p + 0x2C) = 0xC0D1A109u + (u32)(u64)p;    /* magic */
    *(u64 *)(p + 0x30) = 0x88;                         /* size */
    *(u32 *)(p + 0x38) = 1;                            /* USER_MSG */
    *(u64 *)(p + 0x40) = (u64)m;
    *(s32 *)(p + 0x58) = uid;
    *(u64 *)(m + 0x08) = (u64)text;                    /* buttonType 0: OK */
    s32 rc = open(p);
    if (rc != 0)
        logf("elfldr: the TV's dialog did not open (%x)", rc);
}

void el_ui(int what, unsigned n)
{
    static char text[512];
    const char *ip = g_ctx->size >= (CTX_CONSOLE_IP_SLOT + 1) * 8 ? (const char *)((u64 *)g_ctx)[CTX_CONSOLE_IP_SLOT]
                                                                 : NULL;
    if (!ip)
        ip = "this PS5";
    if (what == EL_UI_COUNTDOWN)
        fmt(text, sizeof(text), "ghostldr\n\nLoading the ghostldr extension in %u...\n\nPress OK (or \xe2\x97\x8b) "
            "to cancel and stay in the ELF loader on %s:%u", n, ip, g_port);
    else
        fmt(text, sizeof(text), "ghostldr\n\nELF loader on %s:%u\n(nc %s %u < payload.elf)\n\n%s\nSend \"quit\" there "
            "to go back to Luac0re", ip, g_port, ip, g_port, g_can_boot ? "Send \"boot\" there to boot games."
            : "To boot games, send ghostldr-ext.bin there.");
    dialog(text);
}

int el_ui_cancelled(void)
{
    s32 (*status)(void) = host_real_in("libSceMsgDialog", "sceMsgDialogUpdateStatus");
    return status && status() == 3; /* FINISHED: OK or the circle button */
}

void *el_map(unsigned long size)
{
    void *p = NULL;
    return K.map_flex && K.map_flex(&p, ALIGN_UP(size, 0x4000), PROT_READ | PROT_WRITE, 0) == 0 ? p : NULL;
}

void el_unmap(void *p, unsigned long size) { K.munmap(p, ALIGN_UP(size, 0x4000)); }

/* conn becomes the log (log.c): the previous one, if another, is closed */
void el_log_to(int conn)
{
    struct boot_ctx *c = g_ctx;
    c->log_addr = 0;
    u64 old = __atomic_exchange_n(&c->log_fd, (u64)(s64)conn, __ATOMIC_SEQ_CST);
    if ((s64)old >= 0 && (int)old != conn)
        K.close((int)old);
}

/* A bundle: its parts and header fields where ghostldr.lua reads them
 * (its next boot context is a new one), its whole header for ghostldr.lua
 * to check, and conn becomes the log */
void el_bundle_done(const unsigned char *hdr, void *ext, void *sel, int conn)
{
    struct boot_ctx *c = g_ctx;
    u8 *copy = c->size >= (CTX_BUNDLE_HEADER_SLOT + 1) * 8 ? (u8 *)((u64 *)c)[CTX_BUNDLE_HEADER_SLOT] : NULL;
    if (copy)
        memcpy(copy, hdr, EL_BUNDLE_HEADER);
    c->ext_fd = (u64)ext;
    memcpy(&c->ext_size, hdr + 24, 8);
    c->ext_crc = *(const u32 *)(hdr + 40);
    memcpy(c->ext_sha256, hdr + 48, sizeof(c->ext_sha256));
    c->sel_fd = sel ? (u64)sel : (u64)-1;
    memcpy(&c->sel_size, hdr + 32, 8);
    c->sel_crc = *(const u32 *)(hdr + 44);
    el_log_to(conn);
}

int elfldr_main(void) HIDDEN;
int elfldr_main(void)
{
    char why[200];
    if (net_setup() != 0)
        return -11;
    if (el_init(why, sizeof(why)) != 0) {
        logf("elfldr: %s", why);
        return -12;
    }
    g_payload_fault = on_fault;
    u32 port = CTX_ELFLDR_PORT(g_ctx->flags);
    g_port = port ? port : EL_PORT;
    g_can_boot = (g_ctx->flags & CTX_ELFLDR_CAN_BOOT) != 0;
    struct el_serve s = {(unsigned short)g_port, CTX_ELFLDR_COUNTDOWN(g_ctx->flags), g_can_boot,
                         (g_ctx->flags & CTX_FLAG_BUNDLE) != 0};
    int rc = elfldr_serve(&s);
    g_payload_fault = NULL;
    el_fini();
    return rc;
}
