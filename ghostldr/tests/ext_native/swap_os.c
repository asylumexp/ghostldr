/*
 * The machine side of swap_host.c (test_swap.py): direct memory is a file
 * whose pages are mapped shared (so a page mapped twice is one page, as on
 * the console), the address space reservations are PROT_NONE mappings, and
 * a SIGSEGV/SIGBUS goes to the swap's fault hook as the core's crash handler
 * sends it there (crash.c).
 */
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_root[512];
static int g_phys = -1;
static uint64_t g_phys_used;
static int (*g_hook)(uint64_t addr);
volatile unsigned long os_faults;

static void path_of(const char *p, char *out, size_t n) { snprintf(out, n, "%s%s", g_root, p); }

int os_init(const char *root)
{
    char p[600];
    snprintf(g_root, sizeof(g_root), "%s", root);
    path_of("/temp0", p, sizeof(p));
    mkdir(p, 0777);
    path_of("/phys", p, sizeof(p));
    g_phys = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
    return g_phys >= 0 && ftruncate(g_phys, 64 << 20) == 0 ? 0 : -1;
}

void os_log(const char *line) { printf("%s\n", line), fflush(stdout); }
void *os_calloc(uint64_t n) { return calloc(1, n); }
void os_free(void *p) { free(p); }
int os_vfmt(char *buf, uint64_t size, const char *f, va_list ap) { return vsnprintf(buf, size, f, ap); }

int os_reserve(void **addr, size_t len, int flags, size_t align)
{
    void *p = mmap(*addr, len, PROT_NONE, MAP_PRIVATE | MAP_ANON | ((flags & 0x10) ? MAP_FIXED : 0), -1, 0);
    if (p == MAP_FAILED)
        return -1;
    *addr = p;
    return 0;
}

int os_alloc_direct(int64_t start, int64_t end, size_t len, size_t align, int type, int64_t *out)
{
    uint64_t at = (g_phys_used + align - 1) & ~(uint64_t)(align - 1);
    if (at + len > (64u << 20))
        return -1;
    g_phys_used = at + len;
    *out = (int64_t)at;
    return 0;
}

int os_map_direct(void **addr, size_t len, int prot, int flags, int64_t phys, size_t align)
{
    void *p = mmap((flags & 0x10) ? *addr : NULL, len, prot & 3, MAP_SHARED | ((flags & 0x10) ? MAP_FIXED : 0),
                   g_phys, phys);
    if (p == MAP_FAILED)
        return -1;
    *addr = p;
    return 0;
}

int os_mprotect(const void *addr, size_t len, int prot) { return mprotect((void *)addr, len, prot & 3); }
int os_munmap(void *addr, size_t len) { return munmap(addr, len); }

int os_open(const char *path, int flags, int mode)
{
    char p[600];
    path_of(path, p, sizeof(p));
    if (strncmp(path, "/temp0/", 7) != 0)
        return -1; /* no USB drive here */
    return open(p, (flags & 3) | ((flags & 0x200) ? O_CREAT : 0) | ((flags & 0x400) ? O_TRUNC : 0), mode);
}
long os_pread(int fd, void *buf, size_t n, int64_t off) { return pread(fd, buf, n, off); }
long os_write(int fd, const void *buf, size_t n) { return write(fd, buf, n); }
int64_t os_lseek(int fd, int64_t off, int whence) { return lseek(fd, off, whence); }
int os_close(int fd) { return close(fd); }

/* a read the kernel does into the buffer: EFAULT if a page of it is out
 * (from another file: macOS won't read a file into a mapping of itself) */
long os_kernel_read(void *buf, size_t n)
{
    char p[600];
    path_of("/temp0/src", p, sizeof(p));
    int fd = open(p, O_RDWR | O_CREAT, 0644);
    char x[256] = {0};
    if (write(fd, x, sizeof(x)) < 0 || lseek(fd, 0, SEEK_SET) != 0)
        return -1;
    long r = read(fd, buf, n);
    int e = errno;
    close(fd);
    return r < 0 ? -e : r;
}

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    os_faults++;
    if (g_hook && g_hook((uint64_t)si->si_addr))
        return;
    fprintf(stderr, "unexpected fault at %p\n", si->si_addr);
    _exit(99);
}

void os_install(int (*hook)(uint64_t addr))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    g_hook = hook;
}
