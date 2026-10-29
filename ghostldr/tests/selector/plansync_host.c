/*
 * plansync.c on this computer (test_plansync.py): struct ps_env over POSIX
 * sockets and a folder standing in for the GLDRCONF container.
 *
 *   plansync_host STORE DNS [--share HOST[:PORT] KEY [--tls]] [--github API RAW] [--no-fetch] [--now SECONDS]
 *                 TITLE...
 *
 * --github points GitHub's two hosts at plain HTTP servers (test_plansync.py's).
 *
 * DNS is the server plansync.c's own resolver asks (an IPv4 address).
 * Prints the job's state, the index's HTTP status and each plan found.
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "plansync.h"

static const char *g_store;
static unsigned g_dns;
static long g_now;

static void *h_alloc(struct ps_env *e, unsigned long n) { return calloc(1, n); }
static void h_free(struct ps_env *e, void *p, unsigned long n) { free(p); }
static void h_log(struct ps_env *e, const char *line) { printf("log: %s\n", line); }
static long h_now(struct ps_env *e) { return g_now ? g_now : (long)time(NULL); }

static long h_clock(struct ps_env *e)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void h_random(struct ps_env *e, void *buf, unsigned long n) { arc4random_buf(buf, n); }
static int h_tcp(struct ps_env *e) { int fd = socket(AF_INET, SOCK_STREAM, 0); return fd < 0 ? -errno : fd; }
static int h_udp(struct ps_env *e) { int fd = socket(AF_INET, SOCK_DGRAM, 0); return fd < 0 ? -errno : fd; }

static int h_connect(struct ps_env *e, int fd, unsigned addr, unsigned short port, int ms)
{
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(addr)};
    return connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0 ? 0 : -errno;
}

static void h_timeouts(struct ps_env *e, int fd, int ms)
{
    struct timeval t = {ms / 1000, (ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t));
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
}

static long h_send(struct ps_env *e, int fd, const void *buf, unsigned long n)
{
    unsigned long done = 0;
    while (done < n) {
        ssize_t r = send(fd, (const char *)buf + done, n - done, 0);
        if (r <= 0)
            return r < 0 ? -errno : (long)done;
        done += (unsigned long)r;
    }
    return (long)done;
}

static long h_recv(struct ps_env *e, int fd, void *buf, unsigned long n)
{
    ssize_t r = recv(fd, buf, n, 0);
    return r < 0 ? -errno : r;
}

static void h_sclose(struct ps_env *e, int fd) { close(fd); }

static int h_dns(struct ps_env *e, unsigned out[4])
{
    out[0] = g_dns;
    return 1;
}

static void path(char *out, const char *name) { snprintf(out, 1024, "%s/%s", g_store, name); }

static long h_load(struct ps_env *e, const char *name, void *buf, unsigned long cap)
{
    char p[1024];
    path(p, name);
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return -errno;
    long n = read(fd, buf, cap);
    close(fd);
    return n;
}

static int h_save(struct ps_env *e, const char *name, const void *buf, unsigned long n)
{
    char p[1024];
    path(p, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -errno;
    long w = write(fd, buf, n);
    close(fd);
    return w == (long)n ? 0 : -EIO;
}

static int h_unlink(struct ps_env *e, const char *name)
{
    char p[1024];
    path(p, name);
    return unlink(p) == 0 ? 0 : -errno;
}

static int h_list(struct ps_env *e, const char *prefix, char *names, unsigned long cap)
{
    DIR *d = opendir(g_store);
    int count = 0;
    unsigned long at = 0;
    struct dirent *de;
    while (d && (de = readdir(d))) {
        unsigned long n = strlen(de->d_name);
        if (strncmp(de->d_name, prefix, strlen(prefix)) || at + n + 1 > cap)
            continue;
        memcpy(names + at, de->d_name, n + 1);
        at += n + 1, count++;
    }
    if (d)
        closedir(d);
    return count;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: plansync_host STORE DNS [--share HOST KEY [--tls]] [--no-fetch] [--now S] TITLE...\n");
        return 2;
    }
    g_store = argv[1];
    struct in_addr a;
    inet_pton(AF_INET, argv[2], &a);
    g_dns = ntohl(a.s_addr);
    static struct ps_env e;
    static struct ps_job job;
    e.alloc = h_alloc, e.free = h_free, e.log = h_log, e.now = h_now, e.clock_ms = h_clock, e.random = h_random;
    e.tcp = h_tcp, e.udp = h_udp, e.connect = h_connect, e.timeouts = h_timeouts, e.send = h_send, e.recv = h_recv;
    e.sclose = h_sclose, e.dns = h_dns, e.load = h_load, e.save = h_save, e.unlink = h_unlink, e.list = h_list;
    strcpy(e.version, "test");
    job.fetch = 1;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--share") && i + 2 < argc) {
            snprintf(e.share_host, sizeof(e.share_host), "%s", argv[++i]);
            snprintf(e.share_key, sizeof(e.share_key), "%s", argv[++i]);
            job.share = 1;
        } else if (!strcmp(argv[i], "--github") && i + 2 < argc) {
            snprintf(e.github_api, sizeof(e.github_api), "%s", argv[++i]);
            snprintf(e.github_raw, sizeof(e.github_raw), "%s", argv[++i]);
            e.github_plain = 1;
        } else if (!strcmp(argv[i], "--tls")) {
            e.share_tls = 1;
        } else if (!strcmp(argv[i], "--no-fetch")) {
            job.fetch = 0;
        } else if (!strcmp(argv[i], "--now") && i + 1 < argc) {
            g_now = atol(argv[++i]);
        } else if (job.ntitles < PS_MAX_TITLES) {
            snprintf(job.titles[job.ntitles++], 16, "%s", argv[i]);
        }
    }
    ps_run(&e, &job);
    printf("state %s github %d shared %u unshared %u plans %u\n",
           job.state == PS_DONE ? "done" : job.state == PS_FAILED ? "failed" : "?", job.github, job.shared,
           job.unshared, job.nplans);
    for (unsigned i = 0; i < job.nplans; i++) {
        printf("plan %s %s ", job.plans[i].title, job.plans[i].platform ? "ps4" : "ps5");
        for (int k = 0; k < 20; k++)
            printf("%02x", job.plans[i].fingerprint[k]);
        printf(" contributors %u\n", job.plans[i].contributors);
    }
    return 0;
}
