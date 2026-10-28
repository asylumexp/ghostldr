/*
 * The network installer's server (native/netinst/netinst.c) built for this
 * machine: struct ns_env over POSIX, SWRR's save containers as folders.
 * test_netinstall.py starts it and runs the install page's own code against
 * it (netinstall_node.js), as a browser would.
 *
 *     netinst_host ROOT PORT PAGE.gz [CAPACITY_BLOCKS]
 *
 * Containers live in ROOT/saves/NAME (their size in ROOT/saves/NAME.blocks);
 * a mount is the symlink ROOT/savedataN.  Prints "listening PORT TOKEN" once
 * browsers can connect; then each line on stdin "status" prints the status
 * as one line of JSON; at the end of stdin it stops the server (ns_stop) and
 * prints "stopped".  The log goes to stderr.
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "netinst.h"

/* rt.c's formatter, as netinst.c expects it */
int vfmt(char *buf, size_t size, const char *f, va_list ap) { return vsnprintf(buf, size, f, ap); }
int fmt(char *buf, size_t size, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(buf, size, f, ap);
    va_end(ap);
    return n;
}

#define BUSY 0x809F0003u
#define EXISTS 0x809F0007u
#define NOT_FOUND 0x809F0008u
#define NO_SPACE 0x809F000Au
#define PARAMETER 0x809F0000u
#define MOUNT_FULL 0x809F000Cu

static char g_root[1024];
static unsigned long g_capacity = 1ul << 30; /* blocks */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char name[32];
    int rw;
} g_mounts[16];

static int err(void) { return -(errno ? errno : EIO); }
static void host(char *out, size_t size, const char *path) { snprintf(out, size, "%s%s", g_root, path); }

static void *h_alloc(struct ns_env *e, unsigned long size) { return calloc(1, size ? size : 1); }
static void h_free(struct ns_env *e, void *p, unsigned long size) { free(p); }
static void h_log(struct ns_env *e, const char *line)
{
    fprintf(stderr, "%s\n", line);
}
static int h_thread(struct ns_env *e, void *(*fn)(void *), void *arg, unsigned long *handle)
{
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0)
        return -EAGAIN;
    *handle = (unsigned long)t;
    return 0;
}
static void h_join(struct ns_env *e, unsigned long handle) { pthread_join((pthread_t)handle, NULL); }
static void h_sleep(struct ns_env *e, unsigned ms) { usleep(ms ? ms * 1000 : 100); }
static long h_now(struct ns_env *e) { return (long)time(NULL); }
static long h_clock(struct ns_env *e)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

/* which mount a path is in: its slot, or -1 */
static int slot_of(const char *path)
{
    int n;
    if (sscanf(path, "/savedata%d", &n) == 1 && n >= 1 && n < 16)
        return n;
    return -1;
}

static int h_open(struct ns_env *e, const char *path, int flags, int mode)
{
    char p[2048];
    int n = slot_of(path);
    if ((flags & 3) && n > 0 && !g_mounts[n].rw)
        return -EROFS;
    host(p, sizeof(p), path);
    int fd = open(p, flags, mode); /* FreeBSD's O_* are macOS's too */
    return fd < 0 ? err() : fd;
}
static long h_read(struct ns_env *e, int fd, void *b, unsigned long n)
{
    ssize_t r = read(fd, b, n);
    return r < 0 ? err() : r;
}
static long h_write(struct ns_env *e, int fd, const void *b, unsigned long n)
{
    ssize_t r = write(fd, b, n);
    return r < 0 ? err() : r;
}
static long h_seek(struct ns_env *e, int fd, long off)
{
    off_t r = lseek(fd, off, SEEK_SET);
    return r < 0 ? err() : (long)r;
}
static int h_close(struct ns_env *e, int fd) { return close(fd) < 0 ? err() : 0; }
static int h_size(struct ns_env *e, const char *path, unsigned long *size)
{
    char p[2048];
    struct stat st;
    host(p, sizeof(p), path);
    if (stat(p, &st) < 0)
        return err();
    *size = (unsigned long)st.st_size;
    return 0;
}

static int h_listen(struct ns_env *e, unsigned short port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return err();
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 8) < 0) {
        int r = err();
        close(fd);
        return r;
    }
    return fd;
}
static int h_accept(struct ns_env *e, int fd, int timeout_ms)
{
    struct pollfd p = {fd, POLLIN, 0};
    int n = poll(&p, 1, timeout_ms);
    if (n == 0)
        return -ETIMEDOUT;
    if (n < 0)
        return err();
    int c = accept(fd, NULL, NULL);
    if (c < 0)
        return err();
    int one = 1;
    setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    return c;
}
static int h_sockname(struct ns_env *e, int fd, unsigned *addr)
{
    struct sockaddr_in a;
    socklen_t l = sizeof(a);
    if (getsockname(fd, (struct sockaddr *)&a, &l) < 0)
        return err();
    *addr = ntohl(a.sin_addr.s_addr);
    return 0;
}
static void h_timeouts(struct ns_env *e, int fd, int seconds)
{
    struct timeval tv = {seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
static long h_send(struct ns_env *e, int fd, const void *buf, unsigned long n)
{
    unsigned long done = 0;
    while (done < n) {
        ssize_t w = send(fd, (const char *)buf + done, n - done, 0);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return w < 0 ? err() : -EIO;
        done += (unsigned long)w;
    }
    return (long)n;
}
static long h_recv(struct ns_env *e, int fd, void *buf, unsigned long n)
{
    ssize_t r;
    do
        r = recv(fd, buf, n, 0);
    while (r < 0 && errno == EINTR);
    return r < 0 ? err() : r;
}
static void h_shutdown(struct ns_env *e, int fd) { shutdown(fd, SHUT_RDWR); }
static void h_sclose(struct ns_env *e, int fd)
{
    shutdown(fd, SHUT_RDWR);
    close(fd);
}

/* ---- save containers ---- */
static unsigned long blocks_of(const char *name)
{
    char p[2048];
    snprintf(p, sizeof(p), "%s/saves/%s.blocks", g_root, name);
    FILE *f = fopen(p, "r");
    unsigned long b = 0;
    if (f) {
        if (fscanf(f, "%lu", &b) != 1)
            b = 0;
        fclose(f);
    }
    return b;
}
static unsigned long used_blocks(void)
{
    char p[2048];
    snprintf(p, sizeof(p), "%s/saves", g_root);
    DIR *d = opendir(p);
    unsigned long n = 0;
    struct dirent *de;
    while (d && (de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l > 7 && !strcmp(de->d_name + l - 7, ".blocks")) {
            char name[64];
            snprintf(name, sizeof(name), "%.*s", (int)(l - 7), de->d_name);
            n += blocks_of(name);
        }
    }
    if (d)
        closedir(d);
    return n;
}
static int exists(const char *name)
{
    char p[2048];
    struct stat st;
    snprintf(p, sizeof(p), "%s/saves/%s", g_root, name);
    return stat(p, &st) == 0;
}
static int mounted(const char *name)
{
    for (int i = 1; i < 16; i++)
        if (g_mounts[i].name[0] && !strcmp(g_mounts[i].name, name))
            return i;
    return 0;
}

static int h_mount(struct ns_env *e, const char *name, int mode, unsigned long blocks, char mp[16],
                   unsigned long *required)
{
    pthread_mutex_lock(&g_lock);
    unsigned rc = 0;
    if (mounted(name)) {
        rc = BUSY;
    } else if (mode & 4) {
        if (exists(name))
            rc = EXISTS;
        else if (blocks < 96 || blocks > 32768)
            rc = PARAMETER;
        else if (used_blocks() + blocks > g_capacity) {
            rc = NO_SPACE;
            if (required)
                *required = used_blocks() + blocks - g_capacity;
        } else {
            char p[2048];
            snprintf(p, sizeof(p), "%s/saves/%s", g_root, name);
            mkdir(p, 0777);
            snprintf(p, sizeof(p), "%s/saves/%s.blocks", g_root, name);
            FILE *f = fopen(p, "w");
            fprintf(f, "%lu\n", blocks);
            fclose(f);
        }
    } else if (!exists(name)) {
        rc = NOT_FOUND;
    }
    int n = 0;
    if (!rc) {
        for (n = 1; n < 16 && g_mounts[n].name[0]; n++)
            ;
        if (n >= 16)
            rc = MOUNT_FULL;
    }
    if (!rc) {
        char link[2048], target[2048];
        snprintf(link, sizeof(link), "%s/savedata%d", g_root, n);
        snprintf(target, sizeof(target), "saves/%s", name);
        unlink(link);
        if (symlink(target, link) < 0)
            rc = 0x809F000Bu;
        snprintf(g_mounts[n].name, sizeof(g_mounts[n].name), "%s", name);
        g_mounts[n].rw = (mode & 2) != 0;
        snprintf(mp, 16, "/savedata%d", n);
    }
    pthread_mutex_unlock(&g_lock);
    return (int)rc;
}
static int h_umount(struct ns_env *e, const char mp[16])
{
    int n = slot_of(mp);
    if (n < 1 || !g_mounts[n].name[0])
        return (int)0x809F0004u; /* NOT_MOUNTED */
    char link[2048];
    snprintf(link, sizeof(link), "%s/savedata%d", g_root, n);
    pthread_mutex_lock(&g_lock);
    unlink(link);
    g_mounts[n].name[0] = 0;
    pthread_mutex_unlock(&g_lock);
    return 0;
}
static int h_remove(struct ns_env *e, const char *name)
{
    if (mounted(name))
        return (int)BUSY;
    if (!exists(name))
        return 0; /* a 13.60 PS5 answers 0 for a name that doesn't exist */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s/saves/%s' '%s/saves/%s.blocks'", g_root, name, g_root, name);
    return system(cmd) == 0 ? 0 : (int)0x809F000Bu;
}
static unsigned long h_entropy(struct ns_env *e) { return ((unsigned long)arc4random() << 32) | arc4random(); }

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: netinst_host ROOT PORT PAGE.gz [CAPACITY_BLOCKS]\n");
        return 2;
    }
    if (!realpath(argv[1], g_root))
        return 2;
    if (argc > 4)
        g_capacity = strtoul(argv[4], NULL, 10);
    char p[2048];
    snprintf(p, sizeof(p), "%s/saves", g_root);
    mkdir(p, 0777);
    FILE *pf = fopen(argv[3], "rb");
    if (!pf)
        return 2;
    static unsigned char page[1 << 20];
    size_t page_size = fread(page, 1, sizeof(page), pf);
    fclose(pf);
    static struct ns_env env;
    struct ns_env *e = &env;
    e->alloc = h_alloc, e->free = h_free, e->log = h_log, e->thread = h_thread, e->join = h_join;
    e->sleep_ms = h_sleep, e->now = h_now, e->clock_ms = h_clock;
    e->open = h_open, e->read = h_read, e->write = h_write, e->seek = h_seek, e->close = h_close, e->size = h_size;
    e->listen = h_listen, e->accept = h_accept, e->sockname = h_sockname, e->timeouts = h_timeouts;
    e->send = h_send, e->recv = h_recv, e->sclose = h_sclose, e->shutdown = h_shutdown;
    e->mount = h_mount, e->umount = h_umount, e->remove = h_remove, e->entropy = h_entropy;
    e->page = page, e->page_size = page_size;
    snprintf(e->address, sizeof(e->address), "127.0.0.1");
    snprintf(e->swrr, sizeof(e->swrr), "CUSA03474");
    snprintf(e->platform, sizeof(e->platform), "ps5");
    struct netinst *n = NULL;
    int rc = ns_start(e, (unsigned short)atoi(argv[2]), &n);
    struct netinst_status st = {.size = sizeof(st)};
    ns_get_status(n, &st);
    if (rc != 0) {
        printf("failed %d %s\n", rc, st.message);
        return 1;
    }
    unsigned port = 0;
    sscanf(st.url, "http://127.0.0.1:%u/", &port);
    printf("listening %u %s\n", port, st.token);
    fflush(stdout);
    char line[64];
    while (fgets(line, sizeof(line), stdin)) {
        st.size = sizeof(st);
        ns_get_status(n, &st);
        printf("{\"state\":%u,\"url\":\"%s\",\"token\":\"%s\",\"title\":\"%s\",\"name\":\"%s\",\"done\":%llu,"
               "\"total\":%llu,\"rate_kib\":%u,\"installed\":%u,\"changes\":%u,\"message\":\"%s\"}\n",
               st.state, st.url, st.token, st.title, st.name, st.done, st.total, st.rate_kib, st.installed,
               st.changes, st.message);
        fflush(stdout);
    }
    ns_stop(n);
    printf("stopped\n");
    fflush(stdout);
    return 0;
}
