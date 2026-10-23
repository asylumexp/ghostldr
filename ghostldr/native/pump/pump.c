/* ghostfs installer pump: receive a container's bytes and write them, both at
 * once.  The receiver (ghostldr/lua/gfs_receiver.lua) can only do one thing
 * at a time, and every Lua->native call is a ROP round trip: receiving a
 * chunk, then writing it, adds the two times up (about 14 MB/s on a PS5,
 * where the network alone gives about 30 and the save writes about 25).
 * Here the calling thread receives into a ring of buffers while a second
 * thread writes them out in order, so a transfer runs at the slower of the
 * two.
 *
 * A flat blob: the receiver copies it into the IPU JIT buffer (as
 * ghostldr.lua does with the core) and calls offset 0 with a struct
 * gfs_pump it filled in.  No globals, no relocations; libkernel functions
 * come in the struct.  Built with `make` in ghostldr/native into
 * ghostldr/data/gfs_pump.bin (checked in, so the installer needs no LLVM). */

typedef unsigned long u64;
typedef long s64;
typedef int s32;
typedef unsigned int u32;

#define PUMP_BUFS 4

struct gfs_pump {
    /* in: libkernel functions (the receiver's dlsym) */
    u64 recvfrom;           /* 0x00 */
    u64 write;              /* 0x08 sceKernelWrite */
    u64 thread_create;      /* 0x10 scePthreadCreate */
    u64 thread_join;        /* 0x18 scePthreadJoin */
    u64 usleep;             /* 0x20 sceKernelUsleep */
    /* in: the transfer */
    s32 sock;               /* 0x28 */
    s32 fd;                 /* 0x2c */
    u64 size;               /* 0x30 bytes to move */
    u64 chunk;              /* 0x38 bytes per buffer */
    u64 bufs[PUMP_BUFS];    /* 0x40 */
    /* out */
    s64 status;             /* 0x60 0, -1 connection lost, -2 short write, else the write's error */
    u64 received;           /* 0x68 */
    u64 written;            /* 0x70 */
    /* shared between the two threads */
    u32 full[PUMP_BUFS];    /* 0x78 */
    u64 len[PUMP_BUFS];     /* 0x88 */
    u32 stop;               /* 0xa8 */
    u32 pad;
    u64 tid;                /* 0xb0 */
};                          /* 0xb8 */

_Static_assert(__builtin_offsetof(struct gfs_pump, status) == 0x60, "layout (gfs_receiver.lua)");
_Static_assert(__builtin_offsetof(struct gfs_pump, full) == 0x78, "layout");
_Static_assert(sizeof(struct gfs_pump) == 0xb8, "layout (gfs_receiver.lua clears 0xb8 bytes)");

typedef s64 (*recvfrom_t)(s32, void *, u64, s32, void *, void *);
typedef s64 (*write_t)(s32, const void *, u64);
typedef s32 (*thread_create_t)(u64 *, void *, void *(*)(void *), void *, const char *);
typedef s32 (*thread_join_t)(u64, void **);
typedef s32 (*usleep_t)(u32);

#define MSG_WAITALL 0x40
#define POLL_US 200

static u32 get(u32 *v) { return __atomic_load_n(v, __ATOMIC_ACQUIRE); }
static void set(u32 *v, u32 x) { __atomic_store_n(v, x, __ATOMIC_RELEASE); }

static void *writer(void *arg)
{
    struct gfs_pump *p = arg;
    u64 n = (p->size + p->chunk - 1) / p->chunk;
    for (u64 i = 0; i < n; i++) {
        u32 b = (u32)(i % PUMP_BUFS);
        while (!get(&p->full[b])) {
            if (get(&p->stop)) return 0;
            ((usleep_t)p->usleep)(POLL_US);
        }
        s64 w = ((write_t)p->write)(p->fd, (void *)p->bufs[b], p->len[b]);
        if (w != (s64)p->len[b]) {
            p->status = w < 0 ? w : -2;
            set(&p->stop, 1);
            return 0;
        }
        p->written += (u64)w;
        set(&p->full[b], 0);
    }
    return 0;
}

static u64 recv_exact(struct gfs_pump *p, char *b, u64 n)
{
    u64 got = 0;
    while (got < n) {
        s64 r = ((recvfrom_t)p->recvfrom)(p->sock, b + got, n - got, MSG_WAITALL, 0, 0);
        if (r <= 0 || (u64)r > n - got) break;
        got += (u64)r;
    }
    return got;
}

#ifndef PUMP_HOST     /* ghostldr/tests/test_gfs_pump.py builds it for the computer */
#define PUMP_ENTRY __attribute__((section(".text.entry")))
#else
#define PUMP_ENTRY
#endif

/* returns status: 0 when all `size` bytes were received and written */
PUMP_ENTRY s64 gfs_pump(struct gfs_pump *p)
{
    u64 n = (p->size + p->chunk - 1) / p->chunk;
    p->status = 0;
    p->received = p->written = 0;
    p->stop = 0;
    for (u32 b = 0; b < PUMP_BUFS; b++) p->full[b] = 0;
    s32 rc = ((thread_create_t)p->thread_create)(&p->tid, 0, writer, p, "gfs-pump");
    if (rc != 0) {
        p->status = rc;
        return rc;
    }
    for (u64 i = 0; i < n && !get(&p->stop); i++) {
        u32 b = (u32)(i % PUMP_BUFS);
        while (get(&p->full[b]) && !get(&p->stop)) ((usleep_t)p->usleep)(POLL_US);
        if (get(&p->stop)) break;
        u64 want = p->size - p->received < p->chunk ? p->size - p->received : p->chunk;
        u64 got = recv_exact(p, (char *)p->bufs[b], want);
        p->received += got;
        if (got != want) {
            if (!get(&p->stop)) p->status = -1;
            set(&p->stop, 1);
            break;
        }
        p->len[b] = want;
        set(&p->full[b], 1);
    }
    ((thread_join_t)p->thread_join)(p->tid, 0);
    return p->status;
}
