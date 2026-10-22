/* Host OS side of the ext_host harness (see env.c). */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

void *sys_map(unsigned long size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(3);
    }
    return p;
}

void sys_protect(void *addr, unsigned long size, int writable)
{
    if (mprotect(addr, size, writable ? PROT_READ | PROT_WRITE : PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect");
        exit(3);
    }
}

int sys_open(const char *path) { return open(path, O_RDONLY); }
long sys_read(int fd, void *buf, unsigned long n) { return read(fd, buf, n); }
int sys_close(int fd) { return close(fd); }
void *sys_calloc(unsigned long n) { return calloc(1, n); }
unsigned long sys_strtoul(const char *s) { return strtoul(s, NULL, 0); }
int sys_unused_fd(void) { return dup(0); }
int sys_vfmt(char *buf, unsigned long size, const char *fmt, va_list ap) { return vsnprintf(buf, size, fmt, ap); }

void sys_vlog(const char *fmt, va_list ap)
{
    vprintf(fmt, ap);
    putchar('\n');
    fflush(stdout);
}

long sys_read_file(const char *path, char **out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(3);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = malloc(n + 1);
    if (fread(*out, 1, n, f) != (size_t)n)
        exit(3);
    (*out)[n] = 0;
    fclose(f);
    return n;
}

/* ---- files for the loader tests (kernel.c) ---- */
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

int sys_open_path(const char *path) { return open(path, O_RDONLY); }
long sys_seek(int fd, long off, int whence) { return lseek(fd, off, whence); }

/* 0: regular file, 1: directory, -1: missing */
int sys_kind(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

/* FreeBSD 11 dirents (u32 fileno, u16 reclen, u8 type, u8 namlen, name) of
 * the directory open as fd, continuing where the last call stopped */
int sys_getdents(int fd, void *buf, int size)
{
    static DIR *dirs[64];
    if (fd < 0 || fd >= 64)
        return -1;
    if (!dirs[fd] && !(dirs[fd] = fdopendir(dup(fd))))
        return -1;
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
