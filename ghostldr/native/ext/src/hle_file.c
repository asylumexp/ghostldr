/*
 * File calls on the title's paths and fds (kernel-modules.md §9).
 *
 * /app0 paths are ghostfs's (or the app root's) and /savedataN paths the
 * title's own saves (remap_path).  /app0 on a USB drive is a read-write
 * mount of the drive (approot.c) and is left writable.  A ghostfs fd is a real placeholder fd in
 * SWRR's process, so every fd call the title can make is routed here first:
 * one this file did not handle would act on the placeholder instead of the
 * title's file.  SCE functions return SCE errors; POSIX ones -1 with errno.
 *
 * Each import gets a two-instruction wrapper (FILE_FNS) that passes the
 * handler its host function's index, flagged PX for the POSIX convention.
 */
#include "kmod.h"

#define PX 0x100
typedef u64 (*fn_p)(u64, u64, u64, u64, u64, u64);

/* name, handler, PX for the POSIX convention */
#define FILE_FNS(X)                                                                                                   \
    X(sceKernelOpen, h_open, 0) X(open, h_open, PX) X(_open, h_open, PX)                                              \
    X(sceKernelStat, h_stat, 0) X(stat, h_stat, PX) X(lstat, h_stat, PX)                                              \
    X(access, h_access, PX) X(sceKernelCheckReachability, h_access, 0)                                                \
    X(sceKernelMkdir, h_path, 0) X(sceKernelRmdir, h_path, 0) X(sceKernelUnlink, h_path, 0)                           \
    X(sceKernelChmod, h_path, 0) X(sceKernelTruncate, h_path, 0) X(sceKernelUtimes, h_path, 0)                        \
    X(mkdir, h_path, PX) X(rmdir, h_path, PX) X(unlink, h_path, PX) X(chmod, h_path, PX)                              \
    X(truncate, h_path, PX) X(utimes, h_path, PX) X(chdir, h_path, PX)                                                \
    X(sceKernelRename, h_rename, 0) X(rename, h_rename, PX)                                                           \
    X(sceKernelRead, h_read, 0) X(read, h_read, PX) X(_read, h_read, PX)                                              \
    X(sceKernelPread, h_read, 0) X(pread, h_read, PX)                                                                 \
    X(sceKernelReadv, h_readv, 0) X(readv, h_readv, PX) X(_readv, h_readv, PX)                                        \
    X(sceKernelPreadv, h_readv, 0) X(preadv, h_readv, PX)                                                             \
    X(sceKernelLseek, h_lseek, 0) X(lseek, h_lseek, PX)                                                               \
    X(sceKernelFstat, h_fstat, 0) X(fstat, h_fstat, PX) X(_fstat, h_fstat, PX)                                        \
    X(sceKernelClose, h_close, 0) X(close, h_close, PX) X(_close, h_close, PX)                                        \
    X(sceKernelGetdents, h_dents, 0) X(getdents, h_dents, PX)                                                         \
    X(sceKernelGetdirentries, h_dents, 0) X(getdirentries, h_dents, PX) X(_getdirentries, h_dents, PX)                \
    X(sceKernelFcntl, h_fcntl, 0) X(fcntl, h_fcntl, PX) X(_fcntl, h_fcntl, PX)                                        \
    X(dup, h_dup, PX) X(dup2, h_dup, PX)                                                                              \
    X(sceKernelWrite, h_write, 0) X(write, h_write, PX) X(_write, h_write, PX)                                        \
    X(sceKernelAioSubmitReadCommands, h_submit, 0) X(sceKernelAioSubmitReadCommandsMultiple, h_submit, 0)            \
    X(sceKernelAioPollRequest, h_aio1, 0) X(sceKernelAioWaitRequest, h_aio1, 0)                                       \
    X(sceKernelAioDeleteRequest, h_aio1, 0) X(sceKernelAioCancelRequest, h_aio1, 0)                                   \
    X(sceKernelAioPollRequests, h_aion, 0) X(sceKernelAioWaitRequests, h_aion, 0)                                     \
    X(sceKernelAioDeleteRequests, h_aion, 0) X(sceKernelAioCancelRequests, h_aion, 0)

#define ENUM_(n, h, f) R_##n,
enum { FILE_FNS(ENUM_) R_sceKernelMmap, R_mmap, R_COUNT };
#define NAME_(n, h, f) #n "\0"
static const char k_names[] = FILE_FNS(NAME_) "sceKernelMmap\0mmap";

static void *g_fn[R_COUNT];

/* SWRR's function for index i, looked up on first use */
static NOINLINE fn_p real(u32 i)
{
    i &= 0xFF;
    if (!g_fn[i]) {
        const char *n = k_names;
        for (u32 k = 0; k < i; k++)
            n += strlen(n) + 1;
        g_fn[i] = host_real(n);
    }
    return (fn_p)g_fn[i];
}

/* an SCE error in the POSIX convention: -1, errno = its low bits */
static s64 posix(s64 rc)
{
    if (rc < 0 && ((u64)rc >> 16 & 0xFFFF) == 0x8002) {
        if (K.errno_fn)
            *K.errno_fn() = (int)(rc & 0xFFFF);
        return -1;
    }
    return rc;
}

static u64 ret(u32 i, s64 rc) { return (u64)((i & PX) ? posix(rc) : rc); }

/* SWRR's function, or an error when SWRR's libkernel lacks it */
static u64 pass(u32 i, u64 a, u64 b, u64 c, u64 d, u64 e)
{
    fn_p fn = real(i);
    return fn ? fn(a, b, c, d, e, 0) : ret(i, E_INVAL);
}

/* fds of movie files: their reads are logged (splash playback diagnosis) */
static volatile s32 g_watch_fd[8] = {-1, -1, -1, -1, -1, -1, -1, -1};

static bool watched(s32 fd)
{
    for (u32 i = 0; i < ARRAY_SIZE(g_watch_fd); i++)
        if (g_watch_fd[i] == fd)
            return true;
    return false;
}

/* ---- paths ---- */

static NOINLINE u64 h_open(u64 path, u64 flags, u64 mode, u64 d, u64 e, u32 i)
{
    int gfd;
    if (gfs_open((const char *)path, (int)flags, &gfd) == 0)
        return ret(i, gfd);
    char buf[512];
    const char *p = remap_path((const char *)path, buf, sizeof(buf));
    s32 fd = (s32)pass(i, (u64)p, flags, mode, 0, 0);
    static u32 opens;
    if (fd < 0 || (profile_has && profile_has(PF_TRACE_OPENS)) || __atomic_fetch_add(&opens, 1, __ATOMIC_RELAXED) < 400)
        logf("open %s (%s) flags %x -> %x", (const char *)path, p, (u32)flags, fd);
    if (fd >= 0 && strstr((const char *)path, "movies/"))
        for (u32 k = 0; k < ARRAY_SIZE(g_watch_fd); k++)
            if (g_watch_fd[k] < 0) {
                g_watch_fd[k] = fd;
                break;
            }
    return (u64)(s64)fd;
}

static NOINLINE u64 h_stat(u64 path, u64 st, u64 c, u64 d, u64 e, u32 i)
{
    int grc;
    if (gfs_stat((const char *)path, (void *)st, &grc) == 0)
        return ret(i, grc);
    char buf[512];
    return pass(i, (u64)remap_path((const char *)path, buf, sizeof(buf)), st, 0, 0, 0);
}

/* access(path, mode) and sceKernelCheckReachability(path): ghostfs is read-only */
static NOINLINE u64 h_access(u64 path, u64 mode, u64 c, u64 d, u64 e, u32 i)
{
    u8 st[0x78];
    int grc;
    if (gfs_stat((const char *)path, st, &grc) == 0)
        return ret(i, grc ? grc : ((i & PX) && (mode & 2)) ? E_ROFS : 0);
    char buf[512];
    return pass(i, (u64)remap_path((const char *)path, buf, sizeof(buf)), mode, 0, 0, 0);
}

/* calls that only need the /app0 and /savedata remapping */
static NOINLINE u64 h_path(u64 path, u64 b, u64 c, u64 d, u64 e, u32 i)
{
    char buf[512];
    return pass(i, (u64)remap_path((const char *)path, buf, sizeof(buf)), b, c, 0, 0);
}

static NOINLINE u64 h_rename(u64 from, u64 to, u64 c, u64 d, u64 e, u32 i)
{
    char a[512], b[512];
    return pass(i, (u64)remap_path((const char *)from, a, sizeof(a)), (u64)remap_path((const char *)to, b, sizeof(b)),
                0, 0, 0);
}

/* ---- fds ---- */

/* read(fd, buf, n) and pread(fd, buf, n, off) */
static NOINLINE u64 h_read(u64 fd, u64 buf, u64 n, u64 off, u64 e, u32 i)
{
    bool at = (i & 0xFF) == R_sceKernelPread || (i & 0xFF) == R_pread;
    if (gfs_owns((int)fd))
        return ret(i, at ? gfs_pread((int)fd, (void *)buf, n, (s64)off) : gfs_read((int)fd, (void *)buf, n));
    u64 r = pass(i, fd, buf, n, off, 0);
    if (watched((s32)fd))
        logf("%sread(fd %x, %lx @ %lx) -> %lx", at ? "p" : "", (u32)fd, n, at ? off : 0, r);
    return r;
}

/* readv(fd, iov, n) and preadv(fd, iov, n, off); struct iovec {void *base; u64 len} */
static NOINLINE u64 h_readv(u64 fd, u64 iov, u64 n, u64 off, u64 e, u32 i)
{
    bool at = (i & 0xFF) == R_sceKernelPreadv || (i & 0xFF) == R_preadv;
    if (!gfs_owns((int)fd))
        return pass(i, fd, iov, n, off, 0);
    if (at && (s64)off < 0)
        return ret(i, E_INVAL);
    const u64 *v = (const u64 *)iov;
    s64 done = 0;
    for (u64 k = 0; k < (u64)(s32)n; k++) {
        s64 r = at ? gfs_pread((int)fd, (void *)v[2 * k], v[2 * k + 1], (s64)off + done)
                   : gfs_read((int)fd, (void *)v[2 * k], v[2 * k + 1]);
        if (r < 0)
            return ret(i, done ? done : r);
        done += r;
        if ((u64)r < v[2 * k + 1])
            break;
    }
    return (u64)done;
}

static NOINLINE u64 h_lseek(u64 fd, u64 off, u64 whence, u64 d, u64 e, u32 i)
{
    return gfs_owns((int)fd) ? ret(i, gfs_lseek((int)fd, (s64)off, (int)whence)) : pass(i, fd, off, whence, 0, 0);
}

static NOINLINE u64 h_fstat(u64 fd, u64 st, u64 c, u64 d, u64 e, u32 i)
{
    return gfs_owns((int)fd) ? ret(i, gfs_fstat((int)fd, (void *)st)) : pass(i, fd, st, 0, 0, 0);
}

static NOINLINE u64 h_close(u64 fd, u64 b, u64 c, u64 d, u64 e, u32 i)
{
    if (gfs_close((int)fd) == 0)
        return 0;
    if (watched((s32)fd)) {
        logf("close(fd %x)", (u32)fd);
        for (u32 k = 0; k < ARRAY_SIZE(g_watch_fd); k++)
            if (g_watch_fd[k] == (s32)fd)
                g_watch_fd[k] = -1;
    }
    return pass(i, fd, 0, 0, 0, 0);
}

/* getdents(fd, buf, n), getdirentries(fd, buf, n, s64 *basep) */
static NOINLINE u64 h_dents(u64 fd, u64 buf, u64 n, u64 basep, u64 e, u32 i)
{
    if (!gfs_owns((int)fd))
        return pass(i, fd, buf, n, basep, 0);
    bool base = (i & 0xFF) != R_sceKernelGetdents && (i & 0xFF) != R_getdents;
    if (base && basep)
        *(s64 *)basep = gfs_lseek((int)fd, 0, 1);
    return ret(i, gfs_getdents((int)fd, (void *)buf, (int)n));
}

/* F_GETFD 1, F_SETFD 2, F_GETFL 3 (O_RDONLY) answered; others EINVAL */
static NOINLINE u64 h_fcntl(u64 fd, u64 cmd, u64 arg, u64 d, u64 e, u32 i)
{
    if (gfs_owns((int)fd))
        return (s32)cmd >= 1 && (s32)cmd <= 3 ? 0 : ret(i, E_INVAL);
    return pass(i, fd, cmd, arg, 0, 0);
}

/* a copy of a ghostfs fd, or a dup2 over one, would act on the placeholder */
static NOINLINE u64 h_dup(u64 fd, u64 to, u64 c, u64 d, u64 e, u32 i)
{
    if (gfs_owns((int)fd) || ((i & 0xFF) == R_dup2 && gfs_owns((int)to)))
        return ret(i, E_BADF);
    return pass(i, fd, to, 0, 0, 0);
}

/* The title's stdout and stderr go to the UDP log, one line per record;
 * an ELF loader payload's also to its connection (elfldr_sys.c).  ghostfs
 * files are read-only. */
static volatile int g_out_lock;
static char g_out_line[512];
static u32 g_out_len;
unsigned elfldr_output(int fd, const void *buf, u64 len) HIDDEN;

static NOINLINE u64 h_write(u64 fd, u64 buf, u64 len, u64 d, u64 e, u32 i)
{
    if (gfs_owns((int)fd))
        return ret(i, E_BADF);
    if (fd != 1 && fd != 2)
        return pass(i, fd, buf, len, 0, 0);
    const char *c = (const char *)buf;
    unsigned payload = elfldr_output((int)fd, c, len);
    spin_lock(&g_out_lock);
    for (u64 k = 0; k < len; k++) {
        if (c[k] == '\n' || g_out_len == sizeof(g_out_line) - 1) {
            g_out_line[g_out_len] = 0;
            if (payload)
                logf("payload %u%s: %s", payload, fd == 2 ? " (stderr)" : "", g_out_line);
            else
                logf("ghost%d: %s", (int)fd, g_out_line);
            g_out_len = 0;
            if (c[k] == '\n')
                continue;
        }
        g_out_line[g_out_len++] = c[k];
    }
    spin_unlock(&g_out_lock);
    return len;
}

/* ---- AIO: reads of ghostfs files complete at submission ----
 * SceKernelAioRWRequest {s64 offset, nbyte; void *buf;
 * SceKernelAioResult *result; s32 fd}, result {s64 value; u32 state}
 * (state 3 completed, 4 aborted).  A batch with a ghostfs fd is read here
 * (the other fds with the kernel's pread) and gets ids from a range the
 * kernel does not hand out; the poll, wait, delete and cancel calls answer
 * those and pass the others on. */
#define AIO_IDS 0x7AD00000
#define AIO_MASK 0x7FF00000
static volatile u32 g_aio_next;

struct aio_req {
    s64 off, nbyte;
    void *buf;
    s64 *result;
    s32 fd;
};

static bool aio_ours(s32 id) { return (id & AIO_MASK) == AIO_IDS; }
static s32 aio_id(void) { return AIO_IDS | (s32)(__atomic_fetch_add(&g_aio_next, 1, __ATOMIC_RELAXED) & 0xFFFFF); }

/* SubmitReadCommands(req, n, prio, s32 *id), ...Multiple(req, n, prio, s32 *ids) */
static NOINLINE u64 h_submit(u64 reqs, u64 n, u64 prio, u64 ids, u64 e, u32 i)
{
    struct aio_req *req = (struct aio_req *)reqs;
    s32 *id = (s32 *)ids;
    bool any = false, multiple = (i & 0xFF) == R_sceKernelAioSubmitReadCommandsMultiple;
    for (s32 k = 0; req && k < (s32)n; k++)
        any |= gfs_owns(req[k].fd);
    if (!any)
        return pass(i, reqs, n, prio, ids, 0);
    if (!id)
        return (u64)(s64)E_FAULT;
    for (s32 k = 0; k < (s32)n; k++) {
        s64 r = gfs_owns(req[k].fd) ? gfs_pread(req[k].fd, req[k].buf, (u64)req[k].nbyte, req[k].off)
                                   : K.pread(req[k].fd, req[k].buf, (u64)req[k].nbyte, req[k].off);
        if (req[k].result) {
            req[k].result[0] = r;
            *(u32 *)&req[k].result[1] = r < 0 ? 4 : 3;
        }
        if (multiple)
            id[k] = aio_id();
    }
    if (!multiple)
        id[0] = aio_id();
    return 0;
}

/* Poll/Wait/Delete/CancelRequest(id, s32 *out[, u32 *usec]): ours are done
 * (state 3; Delete's result 0) */
static NOINLINE u64 h_aio1(u64 id, u64 out, u64 usec, u64 d, u64 e, u32 i)
{
    if (!aio_ours((s32)id))
        return pass(i, id, out, usec, 0, 0);
    if (out)
        *(s32 *)out = (i & 0xFF) == R_sceKernelAioDeleteRequest ? 0 : 3;
    return 0;
}

/* the Requests forms (ids, n, s32 *out[, mode, u32 *usec]): one by one
 * through the single-id call when any id is ours */
static NOINLINE u64 h_aion(u64 ids, u64 n, u64 out, u64 mode, u64 usec, u32 i)
{
    const s32 *id = (const s32 *)ids;
    bool any = false;
    for (s32 k = 0; id && k < (s32)n; k++)
        any |= aio_ours(id[k]);
    if (!any)
        return pass(i, ids, n, out, mode, usec);
    u32 one = (i & 0xFF) - (R_sceKernelAioPollRequests - R_sceKernelAioPollRequest);
    for (s32 k = 0; k < (s32)n; k++) {
        u64 rc = h_aio1((u64)id[k], out ? (u64)((s32 *)out + k) : 0, usec, 0, 0, one);
        if (rc)
            return rc;
    }
    return 0;
}

/* ---- a ghostfs file mapped: direct memory filled from the file ---- */
static s32 gfs_map(void *addr, u64 len, s32 prot, s32 flags, s32 fd, s64 off, void **res)
{
    if ((flags & MAP_SHARED) && (prot & PROT_WRITE))
        return E_ACCES;
    if (!len || off < 0 || (off & 0x3FFF))
        return E_INVAL;
    u64 size = ALIGN_UP(len, 0x4000);
    u8 *p = direct_alloc(size, PROT_READ | PROT_WRITE, (flags & MAP_FIXED) ? (u64)addr : 0);
    if (!p)
        return E_NOMEM;
    s64 got = gfs_pread(fd, p, len, off);
    if (got < 0) {
        direct_free(p);
        return (s32)got;
    }
    memset(p + got, 0, size - (u64)got);
    if (prot != (PROT_READ | PROT_WRITE) && K.mprotect(p, size, prot) != 0) {
        direct_free(p);
        return E_ACCES;
    }
    logf("mmap of a ghostfs file: %lx bytes at %lx -> %p", len, (u64)off, p);
    *res = p;
    return 0;
}

static s32 hle_sceKernelMmap(void *addr, u64 len, s32 prot, s32 flags, s32 fd, s64 off, void **res)
{
    if (!(flags & MAP_ANON) && gfs_owns(fd))
        return res ? gfs_map(addr, len, prot, flags, fd, off, res) : E_FAULT;
    s32 (*fn)(void *, u64, s32, s32, s32, s64, void **) = (void *)real(R_sceKernelMmap);
    return fn ? fn(addr, len, prot, flags, fd, off, res) : E_INVAL;
}

static void *hle_mmap(void *addr, u64 len, s32 prot, s32 flags, s32 fd, s64 off)
{
    if (!(flags & MAP_ANON) && gfs_owns(fd)) {
        void *p = NULL;
        s32 rc = gfs_map(addr, len, prot, flags, fd, off, &p);
        return rc ? (void *)posix(rc) : p;
    }
    void *(*fn)(void *, u64, s32, s32, s32, s64) = (void *)real(R_mmap);
    return fn ? fn(addr, len, prot, flags, fd, off) : (void *)posix(E_INVAL);
}

/* the wrappers: the import's arguments stay in place, the index goes in r9 */
#define WRAP_(n, h, f)                                                                                                \
    static u64 w_##n(u64 a, u64 b, u64 c, u64 d, u64 e) { return h(a, b, c, d, e, R_##n | (f)); }
FILE_FNS(WRAP_)

#define ENT_(n, h, f) {"libkernel", #n, w_##n, NULL},
static const struct ext_hle_ent k_hle[] = {
    FILE_FNS(ENT_)
    {"libkernel", "sceKernelMmap", hle_sceKernelMmap, NULL},
    {"libkernel", "mmap", hle_mmap, NULL},
};

EXT_FAMILY("file", k_hle, NULL);
