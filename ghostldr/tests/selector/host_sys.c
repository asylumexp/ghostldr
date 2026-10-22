/* Host OS side of selector_host (see host_env.c): kept out of ghost.h's world. */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
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

void sys_unmap(void *p, unsigned long size) { munmap(p, size); }

/* 0: read+execute, 1: read+write */
void sys_protect(void *addr, unsigned long size, int writable)
{
    if (mprotect(addr, size, writable ? PROT_READ | PROT_WRITE : PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect");
        exit(3);
    }
}

int sys_open(const char *path) { return open(path, O_RDONLY); }
long sys_read(int fd, void *buf, unsigned long n) { return read(fd, buf, n); }
long sys_pread(int fd, void *buf, unsigned long n, long off) { return pread(fd, buf, n, off); }
int sys_close(int fd) { return close(fd); }
long sys_file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}
void *sys_calloc(unsigned long n) { return calloc(1, n); }
unsigned long sys_strtoul(const char *s) { return strtoul(s, NULL, 0); }
int sys_vfmt(char *buf, unsigned long size, const char *fmt, va_list ap) { return vsnprintf(buf, size, fmt, ap); }
const char *sys_getenv(const char *name) { return getenv(name); }
void sys_exit(int code)
{
    fflush(stdout);
    exit(code);
}

void sys_vlog(const char *fmt, va_list ap)
{
    vprintf(fmt, ap);
    putchar('\n');
    fflush(stdout);
}
