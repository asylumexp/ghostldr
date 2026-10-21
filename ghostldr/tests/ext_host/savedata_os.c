/* Host OS side of the save data harness (see savedata_fake.c): POSIX calls,
 * kept out of the ghost.h world.  Errors come back as -errno; the BSD errno
 * numbers the fake kernel turns into 0x8002xxxx match FreeBSD's. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PS4_O_DIRECTORY 0x20000

int os_open(const char *p, int flags, int mode)
{
    int f = flags & (O_ACCMODE | O_CREAT | O_TRUNC | O_EXCL | O_APPEND);
    if (flags & PS4_O_DIRECTORY)
        f |= O_DIRECTORY;
    int fd = open(p, f, mode);
    return fd < 0 ? -errno : fd;
}

int os_close(int fd) { return close(fd) < 0 ? -errno : 0; }

long os_pread(int fd, void *b, unsigned long n, long off)
{
    ssize_t r = pread(fd, b, n, off);
    return r < 0 ? -errno : r;
}

long os_pwrite(int fd, const void *b, unsigned long n, long off)
{
    ssize_t r = pwrite(fd, b, n, off);
    return r < 0 ? -errno : r;
}

/* 0, or -errno; *is_dir and *size of path */
int os_stat(const char *p, int *is_dir, long *size)
{
    struct stat st;
    if (stat(p, &st) != 0)
        return -errno;
    *is_dir = S_ISDIR(st.st_mode);
    *size = (long)st.st_size;
    return 0;
}

int os_fstat_size(int fd, long *size)
{
    struct stat st;
    if (fstat(fd, &st) != 0)
        return -errno;
    *size = (long)st.st_size;
    return 0;
}

int os_mkdir(const char *p) { return mkdir(p, 0777) < 0 ? -errno : 0; }
int os_unlink(const char *p) { return unlink(p) < 0 ? -errno : 0; }
int os_rmdir(const char *p) { return rmdir(p) < 0 ? -errno : 0; }
int os_rename(const char *a, const char *b) { return rename(a, b) < 0 ? -errno : 0; }
int os_exists(const char *p) { return access(p, F_OK) == 0; }

void *os_opendir(const char *p) { return opendir(p); }
void os_closedir(void *d) { closedir(d); }
long os_telldir(void *d) { return telldir(d); }
void os_seekdir(void *d, long pos) { seekdir(d, pos); }

/* the next entry's name; 0 at the end */
int os_readdir(void *d, char *name, int cap)
{
    struct dirent *e = readdir(d);
    if (!e)
        return 0;
    snprintf(name, cap, "%s", e->d_name);
    return 1;
}

static int run(const char *fmt, const char *a, const char *b)
{
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), fmt, a, b);
    return system(cmd);
}

int os_rm_rf(const char *p) { return run("rm -rf '%s'%s", p, ""); }
int os_cp_r(const char *from, const char *to) { return run("cp -R '%s' '%s'", from, to); }
int os_mkdir_p(const char *p) { return run("mkdir -p '%s'%s", p, ""); }

/* 32 KiB blocks under p, counted like the real service: each file at least 1 */
unsigned long os_usage_blocks(const char *p)
{
    DIR *d = opendir(p);
    if (!d)
        return 0;
    unsigned long used = 0;
    struct dirent *e;
    char sub[2048];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(sub, sizeof(sub), "%s/%s", p, e->d_name);
        struct stat st;
        if (stat(sub, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            used += 1 + os_usage_blocks(sub);
        else
            used += st.st_size <= 0 ? 1 : (unsigned long)(st.st_size + 32767) / 32768;
    }
    closedir(d);
    return used;
}

int os_write_file(const char *p, const void *b, unsigned long n)
{
    FILE *f = fopen(p, "wb");
    if (!f)
        return -errno;
    size_t w = fwrite(b, 1, n, f);
    fclose(f);
    return w == n ? 0 : -EIO;
}

/* bytes read (at most max), or -errno */
long os_read_file(const char *p, void *b, unsigned long max)
{
    FILE *f = fopen(p, "rb");
    if (!f)
        return -errno;
    size_t r = fread(b, 1, max, f);
    fclose(f);
    return (long)r;
}

void *os_calloc(unsigned long n) { return calloc(1, n ? n : 1); }
void os_free(void *p) { free(p); }

void os_puts(const char *s)
{
    fputs(s, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void os_exit(int code) { exit(code); }
