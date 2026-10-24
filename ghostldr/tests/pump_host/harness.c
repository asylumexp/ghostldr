/* the installer pump (native/pump/pump.c) on the computer: a sender thread
 * streams a pattern through a socket pair, the pump writes it to a file.
 *   harness OUT SIZE CHUNK SEND [FAIL_AFTER]
 * SEND bytes are sent (fewer than SIZE: the connection drops early); with
 * FAIL_AFTER, writes past that many bytes fail with 0x8002001c.
 * Prints "status received written". */
#define PUMP_HOST 1
#include "../../native/pump/pump.c"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static long fail_after = -1;
static u64 total_written;

static s64 h_write(s32 fd, const void *b, u64 n)
{
    if (fail_after >= 0 && total_written + n > (u64)fail_after) return (s64)(s32)0x8002001c;
    s64 w = write(fd, b, n);
    if (w > 0) total_written += (u64)w;
    return w;
}

static s32 h_create(u64 *tid, void *attr, void *(*fn)(void *), void *arg, const char *name)
{
    pthread_t t;
    s32 rc = pthread_create(&t, 0, fn, arg);
    *tid = (u64)t;
    return rc;
}

static s32 h_join(u64 tid, void **ret) { return pthread_join((pthread_t)tid, ret); }
static s32 h_usleep(u32 us) { return usleep(us); }

static unsigned char pattern(u64 i) { return (unsigned char)(i * 131 + (i >> 9)); }

struct sender { int fd; u64 n; };

static void *send_all(void *arg)
{
    struct sender *s = arg;
    unsigned char b[7777];
    for (u64 off = 0; off < s->n;) {
        u64 want = s->n - off < sizeof b ? s->n - off : sizeof b;
        for (u64 i = 0; i < want; i++) b[i] = pattern(off + i);
        ssize_t w = write(s->fd, b, want);
        if (w <= 0) break;
        off += (u64)w;
    }
    close(s->fd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 5) return 2;
    signal(SIGPIPE, SIG_IGN);
    u64 size = strtoull(argv[2], 0, 0), chunk = strtoull(argv[3], 0, 0), send_n = strtoull(argv[4], 0, 0);
    if (argc > 5) fail_after = strtol(argv[5], 0, 0);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return 3;
    int out = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) return 4;

    struct gfs_pump p = {0};
    p.recvfrom = (u64)recvfrom;
    p.write = (u64)h_write;
    p.thread_create = (u64)h_create;
    p.thread_join = (u64)h_join;
    p.usleep = (u64)h_usleep;
    p.sock = sv[0];
    p.fd = out;
    p.size = size;
    p.chunk = chunk;
    for (int i = 0; i < PUMP_BUFS; i++) p.bufs[i] = (u64)malloc(chunk);

    struct sender s = {sv[1], send_n};
    pthread_t t;
    pthread_create(&t, 0, send_all, &s);
    s64 status = gfs_pump(&p);
    close(sv[0]);       /* a sender blocked on a stopped pump gets EPIPE */
    pthread_join(t, 0);
    close(out);
    printf("%ld %lu %lu\n", (long)status, (unsigned long)p.received, (unsigned long)p.written);
    return 0;
}
