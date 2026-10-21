/* Logging: one UDP datagram per line to the host log server (Luac0re's
 * log_server.py protocol), mirrored to the kernel debug output when present. */
#include "ghost.h"

static int g_log_fd = -1;
static const void *g_log_addr;

void log_init(struct boot_ctx *ctx)
{
    g_log_fd = (int)ctx->log_fd;
    g_log_addr = (const void *)ctx->log_addr;
}

static void emit(const char *line, size_t n)
{
    if (g_log_fd >= 0 && g_log_addr && K.sendto)
        K.sendto(g_log_fd, line, n, 0, g_log_addr, 16);
    if (K.debug_out)
        K.debug_out(0, line);
}

void logf(const char *f, ...)
{
    char line[1024];
    int pid = K.getpid ? K.getpid() : 0;
    int n = fmt(line, sizeof(line), "[ghostldr %d] ", pid);
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    n += vfmt(line + n, sizeof(line) - n - 1, f, ap);
    __builtin_va_end(ap);
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    line[n] = 0;
    emit(line, n);
}

void log_hex(const char *tag, const void *p, size_t n)
{
    const u8 *b = p;
    char line[200];
    for (size_t i = 0; i < n; i += 32) {
        int k = fmt(line, sizeof(line), "%s+%03lx:", tag, (u64)i);
        for (size_t j = i; j < n && j < i + 32; j++)
            k += fmt(line + k, sizeof(line) - k, " %02x", b[j]);
        logf("%s", line);
    }
}
