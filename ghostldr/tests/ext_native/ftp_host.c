/*
 * The FTP server (ext/src/ftp.c) built for this machine: ftp.h's
 * environment over POSIX, the console's "/" being a host directory.
 * test_ftp.py starts it and drives it with ftplib:
 *
 *     ftp_host ROOT PORT [HOME]
 *
 * prints "listening PORT" once clients can connect (PORT 0: the port the
 * system picked), then serves until its stdin closes.  The log goes to
 * stderr.  HOME is the directory clients start in (default "/").  An "r"
 * on stdin makes the listening sockets again, as after rest mode
 * (ftp_relisten), and prints "relisten RC".
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ftp.h"

/* rt.c's formatter and string helpers, as ftp.c expects them */
int vfmt(char *buf, size_t size, const char *f, va_list ap) { return vsnprintf(buf, size, f, ap); }
int fmt(char *buf, size_t size, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(buf, size, f, ap);
    va_end(ap);
    return n;
}
const char *strrchr_c(const char *s, int c) { return strrchr(s, c); }

static char g_root[1024];
static const char *g_home = "/";

/* the host path of a client path ("/x" -> ROOT/x) */
static const char *host(const char *path, char *buf)
{
    snprintf(buf, 4096, "%s%s", g_root, path);
    return buf;
}

static int err(void) { return -(errno ? errno : EIO); }

void *fv_alloc(unsigned long size) { return calloc(1, size ? size : 1); }
void fv_free(void *p, unsigned long size) { free(p); }

void fv_log(const char *f, ...)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    va_list ap;
    va_start(ap, f);
    pthread_mutex_lock(&lock);
    vfprintf(stderr, f, ap);
    fputc('\n', stderr);
    pthread_mutex_unlock(&lock);
    va_end(ap);
}

int fv_thread(void *(*fn)(void *), void *arg, const char *name)
{
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0)
        return -EAGAIN;
    pthread_detach(t);
    return 0;
}

void fv_sleep_ms(unsigned ms) { usleep(ms * 1000); }
long fv_now(void) { return (long)time(NULL); }
long fv_clock_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

/* ftp.h's O_* are FreeBSD's, which are macOS's too */
int fv_open(const char *path, int flags, int mode)
{
    char b[4096];
    int fd = open(host(path, b), flags, mode);
    return fd < 0 ? err() : fd;
}
long fv_read(int fd, void *buf, unsigned long n)
{
    long r = read(fd, buf, n);
    return r < 0 ? err() : r;
}
long fv_write(int fd, const void *buf, unsigned long n)
{
    long r = write(fd, buf, n);
    return r < 0 ? err() : r;
}
long fv_seek(int fd, long off, int whence)
{
    long r = lseek(fd, off, whence);
    return r < 0 ? err() : r;
}
int fv_close(int fd) { return close(fd) < 0 ? err() : 0; }

int fv_stat(const char *path, struct fv_stat *st)
{
    char b[4096];
    struct stat s;
    if (stat(host(path, b), &s) < 0)
        return err();
    st->mode = s.st_mode;
    st->size = s.st_size;
    st->mtime = s.st_mtime;
    return 0;
}

#define PATH_CALL(name, call)                                                                                         \
    int fv_##name(const char *path)                                                                                    \
    {                                                                                                                  \
        char b[4096];                                                                                                  \
        return call(host(path, b)) < 0 ? err() : 0;                                                                    \
    }
static int mkdir_(const char *p) { return mkdir(p, 0777); }
PATH_CALL(mkdir, mkdir_)
PATH_CALL(rmdir, rmdir)
PATH_CALL(unlink, unlink)

int fv_rename(const char *from, const char *to)
{
    char a[4096], b[4096];
    return rename(host(from, a), host(to, b)) < 0 ? err() : 0;
}

int fv_opendir(const char *path, struct fv_dir *d)
{
    char b[4096];
    DIR *dir = opendir(host(path, b));
    if (!dir)
        return err();
    d->h = (long)dir;
    return 0;
}

int fv_readdir(struct fv_dir *d, char *name, unsigned size)
{
    struct dirent *e;
    while ((e = readdir((DIR *)d->h)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            snprintf(name, size, "%s", e->d_name);
            return 1;
        }
    return 0;
}

void fv_closedir(struct fv_dir *d) { closedir((DIR *)d->h); }

static void nosigpipe(int fd)
{
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
}

int fv_listen(unsigned addr, unsigned short port, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return err();
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_len = sizeof(a), .sin_family = AF_INET, .sin_port = htons(port),
                            .sin_addr.s_addr = htonl(addr)};
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, backlog) < 0) {
        int e = err();
        close(fd);
        return e;
    }
    return fd;
}

int fv_accept(int fd, int timeout_ms, unsigned *addr, unsigned short *port)
{
    if (timeout_ms >= 0) {
        struct pollfd p = {fd, POLLIN, 0};
        int n = poll(&p, 1, timeout_ms);
        if (n == 0)
            return -ETIMEDOUT;
        if (n < 0)
            return err();
    }
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    int c = accept(fd, (struct sockaddr *)&a, &len);
    if (c < 0)
        return err();
    nosigpipe(c);
    if (addr)
        *addr = ntohl(a.sin_addr.s_addr);
    if (port)
        *port = ntohs(a.sin_port);
    return c;
}

int fv_connect(unsigned addr, unsigned short port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return err();
    nosigpipe(fd);
    struct sockaddr_in a = {.sin_len = sizeof(a), .sin_family = AF_INET, .sin_port = htons(port),
                            .sin_addr.s_addr = htonl(addr)};
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        int e = err();
        close(fd);
        return e;
    }
    return fd;
}

int fv_sockname(int fd, unsigned *addr, unsigned short *port)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    if (getsockname(fd, (struct sockaddr *)&a, &len) < 0)
        return err();
    *addr = ntohl(a.sin_addr.s_addr);
    *port = ntohs(a.sin_port);
    return 0;
}

int fv_timeouts(int fd, int seconds)
{
    struct timeval tv = {seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0 ? err() : 0;
}

long fv_send(int fd, const void *buf, unsigned long n)
{
    unsigned long done = 0;
    while (done < n) {
        long w = send(fd, (const char *)buf + done, n - done, 0);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return w < 0 ? err() : -EIO;
        done += (unsigned long)w;
    }
    return (long)n;
}

long fv_recv(int fd, void *buf, unsigned long n)
{
    long r;
    do
        r = recv(fd, buf, n, 0);
    while (r < 0 && errno == EINTR);
    return r < 0 ? err() : r;
}

void fv_shutdown(int fd) { shutdown(fd, SHUT_RDWR); }

void fv_sclose(int fd)
{
    shutdown(fd, SHUT_RDWR);
    close(fd);
}

const char *fv_home(void) { return g_home; }

#ifndef FTP_HOST_NO_MAIN /* elfldr_host.c takes the environment only */
int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: ftp_host ROOT PORT [HOME]\n");
        return 2;
    }
    if (!realpath(argv[1], g_root))
        return 2;
    if (argc > 3)
        g_home = argv[3];
    int port = atoi(argv[2]);
    if (!port) { /* a free port, found the way the system picks one */
        int s = fv_listen(0x7f000001, 0, 1);
        unsigned a;
        unsigned short p;
        fv_sockname(s, &a, &p);
        close(s);
        port = p;
    }
    int rc = ftp_start((unsigned short)port);
    if (rc != 0) {
        printf("failed %d\n", rc);
        return 1;
    }
    printf("listening %d\n", port);
    fflush(stdout);
    int ftp_relisten(void);
    char c;
    while (read(0, &c, 1) > 0)
        if (c == 'r') {
            printf("relisten %d\n", ftp_relisten());
            fflush(stdout);
        } else if (c == 's') { /* the game selector's server going (ftp_sys.c ftp_early_stop) */
            printf("stop %d\n", ftp_stop(15000));
            fflush(stdout);
        } else if (c == 'g') { /* and the title's starting it again */
            printf("start %d\n", ftp_start((unsigned short)port));
            fflush(stdout);
        }
    return 0;
}
#endif
