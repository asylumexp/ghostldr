/* Host OS side of the system-family harness (fam_system.c): a fake libkernel
 * file layer over a directory on this machine, with Orbis flags, errors,
 * struct stat and dirent layouts. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define ORBIS_ERR(e) ((int)(0x80020000u | (unsigned)(e)))
#define O_ORBIS_DIRECTORY 0x20000
#define DIR_FD_BASE 0x4000

static DIR *g_dirs[64];

void *fs_map(unsigned long size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(3);
    }
    return p;
}

void fs_unmap(void *p, unsigned long size) { munmap(p, size); }

void fs_protect(void *addr, unsigned long size, int writable)
{
    if (mprotect(addr, size, writable ? PROT_READ | PROT_WRITE : PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect");
        exit(3);
    }
}

void fs_vlog(const char *fmt, va_list ap)
{
    vprintf(fmt, ap);
    putchar('\n');
    fflush(stdout);
}

int fs_vfmt(char *buf, unsigned long size, const char *fmt, va_list ap) { return vsnprintf(buf, size, fmt, ap); }
void *fs_calloc(unsigned long n) { return calloc(1, n); }
unsigned long fs_strtoul(const char *s) { return strtoul(s, NULL, 0); }

int fs_open(const char *path, int flags, int mode)
{
    if (flags & O_ORBIS_DIRECTORY) {
        DIR *d = opendir(path);
        if (!d)
            return ORBIS_ERR(errno);
        for (int i = 0; i < 64; i++)
            if (!g_dirs[i]) {
                g_dirs[i] = d;
                return DIR_FD_BASE + i;
            }
        closedir(d);
        return ORBIS_ERR(EMFILE);
    }
    int f = (flags & 3) == 1 ? O_WRONLY : (flags & 3) == 2 ? O_RDWR : O_RDONLY;
    if (flags & 0x200)
        f |= O_CREAT;
    if (flags & 0x400)
        f |= O_TRUNC;
    int fd = open(path, f, mode ? mode : 0644);
    return fd < 0 ? ORBIS_ERR(errno) : fd;
}

int fs_close(int fd)
{
    if (fd >= DIR_FD_BASE && fd < DIR_FD_BASE + 64 && g_dirs[fd - DIR_FD_BASE]) {
        closedir(g_dirs[fd - DIR_FD_BASE]);
        g_dirs[fd - DIR_FD_BASE] = NULL;
        return 0;
    }
    return close(fd) == 0 ? 0 : ORBIS_ERR(errno);
}

long fs_read(int fd, void *buf, unsigned long n)
{
    long r = read(fd, buf, n);
    return r < 0 ? ORBIS_ERR(errno) : r;
}

long fs_lseek(int fd, long off, int whence)
{
    long r = lseek(fd, off, whence);
    return r < 0 ? ORBIS_ERR(errno) : r;
}

/* Orbis dirents: u32 fileno, u16 reclen, u8 type, u8 namlen, name */
int fs_getdents(int fd, void *buf, int size)
{
    if (fd < DIR_FD_BASE || fd >= DIR_FD_BASE + 64 || !g_dirs[fd - DIR_FD_BASE])
        return ORBIS_ERR(EBADF);
    DIR *d = g_dirs[fd - DIR_FD_BASE];
    int used = 0;
    for (;;) {
        long pos = telldir(d);
        struct dirent *e = readdir(d);
        if (!e)
            break;
        size_t len = strlen(e->d_name);
        int reclen = (int)((8 + len + 1 + 3) & ~3UL);
        if (used + reclen > size) {
            seekdir(d, pos);
            break;
        }
        unsigned char *p = (unsigned char *)buf + used;
        memset(p, 0, (size_t)reclen);
        *(uint32_t *)p = (uint32_t)e->d_ino;
        *(uint16_t *)(p + 4) = (uint16_t)reclen;
        p[6] = e->d_type == DT_DIR ? 4 : e->d_type == DT_REG ? 8 : 0;
        p[7] = (unsigned char)len;
        memcpy(p + 8, e->d_name, len);
        used += reclen;
    }
    return used;
}

/* Orbis struct stat: st_mode u16 @8, st_size s64 @0x48 (0x78 bytes) */
int fs_stat(const char *path, void *out)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return ORBIS_ERR(errno);
    unsigned char *o = out;
    memset(o, 0, 0x78);
    *(uint16_t *)(o + 8) = (uint16_t)st.st_mode;
    *(int64_t *)(o + 0x48) = (int64_t)st.st_size;
    return 0;
}

int fs_unlink(const char *path) { return unlink(path) == 0 ? 0 : ORBIS_ERR(errno); }
int fs_rmdir(const char *path) { return rmdir(path) == 0 ? 0 : ORBIS_ERR(errno); }
int fs_mkdir(const char *path, int mode) { return mkdir(path, (mode_t)mode) == 0 ? 0 : ORBIS_ERR(errno); }
int fs_exists(const char *path) { return access(path, F_OK) == 0; }
int fs_raw_open(const char *path) { return open(path, O_RDONLY); }
long fs_raw_read(int fd, void *buf, unsigned long n) { return read(fd, buf, n); }

long fs_file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

/* the core's hle.c (not linked here): ext_unplace drops host_real's cached names that point into the
 * extension; this harness keeps no such cache */
void host_real_diag_ext_unplace(unsigned long long lo, unsigned long long hi) { (void)lo, (void)hi; }
