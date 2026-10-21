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
