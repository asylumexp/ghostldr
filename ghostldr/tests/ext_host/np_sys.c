/* Host OS side of the np_host harness (see np_host.c). */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

long sys_lseek(int fd, long off, int whence) { return lseek(fd, off, whence); }

/* ---- the console's libkernel file calls, for host_real ----
 * The save data family's title store (where the trophy records live)
 * asks the host for libkernel's own file calls and is never remapped:
 * on a console those are the real kernel's, here they are this process's
 * POSIX calls with Orbis-shaped results: 0x8002xxxx errors, Orbis open
 * flags, the 0x78-byte stat layout (mode at +8, size at +0x48) and
 * FreeBSD dirents.  Paths arrive already absolute (the store's root is
 * derived from the title's app0), so they are used as they are. */

static int kerr(long rc) { return rc < 0 ? (int)(0x80020000u | (unsigned)-rc) : (int)rc; }

/* Orbis open flags (ghost.h) that differ from this host's */
#define KB_O_APPEND 0x8
#define KB_O_CREAT 0x200
#define KB_O_TRUNC 0x400
#define KB_O_EXCL 0x800
#define KB_O_DIRECTORY 0x20000

int sys_kernel_open(const char *path, int flags, int mode)
{
    int f = flags & 3; /* O_RDONLY / O_WRONLY / O_RDWR agree */
    if (flags & KB_O_APPEND)
        f |= O_APPEND;
    if (flags & KB_O_CREAT)
        f |= O_CREAT;
    if (flags & KB_O_TRUNC)
        f |= O_TRUNC;
    if (flags & KB_O_EXCL)
        f |= O_EXCL;
    if (flags & KB_O_DIRECTORY)
        f |= O_DIRECTORY;
    int fd = open(path, f, mode);
    return fd < 0 ? kerr(-errno) : fd;
}

int sys_kernel_close(int fd) { return close(fd) < 0 ? kerr(-errno) : 0; }

long sys_kernel_pread(int fd, void *b, unsigned long n, long off)
{
    ssize_t r = pread(fd, b, n, (off_t)off);
    return r < 0 ? kerr(-errno) : (long)r;
}

long sys_kernel_pwrite(int fd, const void *b, unsigned long n, long off)
{
    ssize_t r = pwrite(fd, b, n, (off_t)off);
    return r < 0 ? kerr(-errno) : (long)r;
}

int sys_kernel_stat(const char *path, void *st)
{
    struct stat s;
    if (stat(path, &s) != 0)
        return kerr(-errno);
    memset(st, 0, 0x78);
    *(unsigned short *)((char *)st + 8) = S_ISDIR(s.st_mode) ? 0x41ff : 0x81b6;
    *(long *)((char *)st + 0x48) = (long)s.st_size;
    return 0;
}

int sys_kernel_mkdir(const char *path) { return mkdir(path, 0777) < 0 ? kerr(-errno) : 0; }
int sys_kernel_unlink(const char *path) { return unlink(path) < 0 ? kerr(-errno) : 0; }
int sys_kernel_rmdir(const char *path) { return rmdir(path) < 0 ? kerr(-errno) : 0; }

int sys_kernel_rename(const char *from, const char *to)
{
    return rename(from, to) < 0 ? kerr(-errno) : 0;
}

/* FreeBSD dirents (u32 fileno, u16 reclen, u8 type, u8 namlen, name),
 * continuing where the last call on this fd stopped */
int sys_kernel_getdents(int fd, void *buf, int size)
{
    static DIR *dirs[64];
    if (fd < 0 || fd >= 64)
        return (int)0x80020009; /* EBADF */
    if (!dirs[fd] && !(dirs[fd] = fdopendir(dup(fd))))
        return kerr(-errno);
    int used = 0;
    for (;;) {
        long pos = telldir(dirs[fd]);
        struct dirent *e = readdir(dirs[fd]);
        if (!e)
            break;
        int len = (int)strlen(e->d_name), reclen = (8 + len + 1 + 3) & ~3;
        if (used + reclen > size) {
            seekdir(dirs[fd], pos);
            break;
        }
        unsigned char *d = (unsigned char *)buf + used;
        *(unsigned *)d = 1;
        *(unsigned short *)(d + 4) = (unsigned short)reclen;
        d[6] = e->d_type == DT_DIR ? 4 : 8;
        d[7] = (unsigned char)len;
        memcpy(d + 8, e->d_name, len + 1);
        used += reclen;
    }
    return used;
}

int sys_kernel_gettimeofday(void *tv)
{
    return gettimeofday((struct timeval *)tv, NULL) < 0 ? kerr(-errno) : 0;
}
