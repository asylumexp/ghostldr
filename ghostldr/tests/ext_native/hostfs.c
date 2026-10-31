/*
 * The host's files for the native harnesses, behind plain functions: this
 * file uses the system's headers, which the freestanding ghost.h cannot sit
 * next to.  A console path "/x/y" is the host path <root>/x/y.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static char g_root[1024];

void hfs_set_root(const char *root) { snprintf(g_root, sizeof(g_root), "%s", root); }

static void host(const char *path, char *out, size_t size) { snprintf(out, size, "%s%s", g_root, path); }

/* 0 with *dir and *size, or -1 */
int hfs_stat(const char *path, int *dir, long *size, long *mtime)
{
    char p[2048];
    struct stat st;
    host(path, p, sizeof(p));
    if (stat(p, &st) != 0)
        return -1;
    *dir = S_ISDIR(st.st_mode), *size = (long)st.st_size, *mtime = (long)st.st_mtime;
    return 0;
}

/* a handle: a file descriptor (files) or a directory stream, by slot */
static DIR *g_dirs[64];
static int g_fds[64];
static struct {
    int has, dir;
    char name[1024];
} g_pending[64]; /* hfs_getdents: an entry that did not fit */

int hfs_open(const char *path)
{
    char p[2048];
    struct stat st;
    host(path, p, sizeof(p));
    if (stat(p, &st) != 0)
        return -1;
    for (int i = 0; i < 64; i++)
        if (!g_dirs[i] && !g_fds[i]) {
            if (S_ISDIR(st.st_mode)) {
                if (!(g_dirs[i] = opendir(p)))
                    return -1;
            } else if ((g_fds[i] = open(p, O_RDONLY)) < 0) {
                g_fds[i] = 0;
                return -1;
            }
            return i;
        }
    return -1;
}

long hfs_read(int h, void *buf, unsigned long n) { return h >= 0 && h < 64 && g_fds[h] ? read(g_fds[h], buf, n) : -1; }
long hfs_seek(int h, long off, int whence)
{
    return h >= 0 && h < 64 && g_fds[h] ? lseek(g_fds[h], off, whence) : -1;
}

/* the next entry of a directory handle: 1 with name and type, 0 at the end */
int hfs_readdir(int h, char *name, int cap, int *dir)
{
    struct dirent *d;
    if (h < 0 || h >= 64 || !g_dirs[h])
        return -1;
    while ((d = readdir(g_dirs[h])) && (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")))
        ;
    if (!d)
        return 0;
    snprintf(name, (size_t)cap, "%s", d->d_name);
    *dir = d->d_type == DT_DIR;
    return 1;
}

int hfs_close(int h)
{
    if (h < 0 || h >= 64)
        return -1;
    g_pending[h].has = 0;
    if (g_dirs[h])
        closedir(g_dirs[h]), g_dirs[h] = NULL;
    if (g_fds[h])
        close(g_fds[h]), g_fds[h] = 0;
    return 0;
}

int hfs_is_dir_handle(int h) { return h >= 0 && h < 64 && g_dirs[h] != NULL; }

/* FreeBSD's struct stat (0x78 bytes) as the core's file calls fill it: mode, size, mtime */
int hfs_stat_fbsd(const char *path, void *st)
{
    int dir;
    long size, mtime;
    if (hfs_stat(path, &dir, &size, &mtime) != 0)
        return (int)0x80020002;
    unsigned char *s = st;
    memset(s, 0, 0x78);
    unsigned short mode = dir ? 0x41ED : 0x81A4;
    memcpy(s + 8, &mode, 2);
    memcpy(s + 0x48, &size, 8);
    memcpy(s + 0x28, &mtime, 8);
    return 0;
}

/* FreeBSD dirents of a directory handle; an entry that does not fit waits for the next call */
int hfs_getdents(int h, void *buf, int size)
{
    int used = 0;
    if (h < 0 || h >= 64 || !g_dirs[h])
        return (int)0x80020016;
    for (;;) {
        if (!g_pending[h].has) {
            int r = hfs_readdir(h, g_pending[h].name, sizeof(g_pending[h].name), &g_pending[h].dir);
            if (r <= 0)
                return used;
            g_pending[h].has = 1;
        }
        unsigned len = (unsigned)strlen(g_pending[h].name);
        int reclen = (int)((8 + len + 1 + 3) & ~3u);
        if (used + reclen > size)
            return used;
        unsigned char *d = (unsigned char *)buf + used;
        unsigned ino = 1;
        unsigned short rl = (unsigned short)reclen;
        memset(d, 0, (size_t)reclen);
        memcpy(d, &ino, 4);
        memcpy(d + 4, &rl, 2);
        d[6] = g_pending[h].dir ? 4 : 8;
        d[7] = (unsigned char)len;
        memcpy(d + 8, g_pending[h].name, len);
        used += reclen;
        g_pending[h].has = 0;
    }
}

long hfs_image_read(int fd, void *buf, unsigned long n, long off) { return pread(fd, buf, n, off); }
int hfs_image_open(const char *path) { return open(path, O_RDONLY); }
long hfs_image_size(int fd) { return lseek(fd, 0, SEEK_END); }

/* real fds on the host's files (approot_host.c's libkernel for usb_live.c),
 * threads, sleeps and the clock */
int hfs_real_open(const char *path, int flags, int mode)
{
    char p[1200];
    host(path, p, sizeof(p));
    return open(p, flags, mode);
}
long hfs_real_lseek(int fd, long off, int whence) { return lseek(fd, off, whence); }
long hfs_real_read(int fd, void *buf, unsigned long n) { return read(fd, buf, n); }
int hfs_real_dup(int fd) { return dup(fd); }
int hfs_real_close(int fd) { return close(fd); }
int *hfs_errno(void) { return &errno; }
int hfs_usleep(unsigned us) { return usleep(us); }
long hfs_now_s(void)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    return (long)t.tv_sec;
}
int hfs_thread(void *(*fn)(void *), void *arg)
{
    pthread_t t;
    int rc = pthread_create(&t, NULL, fn, arg);
    if (rc == 0)
        pthread_detach(t);
    return rc;
}

/* the test's own change to FSROOT (approot_host --rescan-mv): a folder moved */
int hfs_rename(const char *from, const char *to)
{
    char a[1024], b[1024];
    snprintf(a, sizeof(a), "%s%s", g_root, from);
    snprintf(b, sizeof(b), "%s%s", g_root, to);
    return rename(a, b);
}
