/* Logging: one send per line, mirrored to the kernel debug output when present.
 *
 *  - ctx->log_addr set (run.py): a UDP datagram to that address, the host's
 *    log server (Luac0re's log_server.py protocol);
 *  - log_addr 0: log_fd is the TCP connection a bundle came on (the release's
 *    ghostldr.lua, CTX_FLAG_BUNDLE), so whoever sent it with nc reads the log.
 *    ghostldr.lua sets SO_NOSIGPIPE and a send timeout on it.  A send that
 *    finds the connection gone (EPIPE, ECONNRESET, ENOTCONN, ECONNABORTED)
 *    or the sender no longer reading (EAGAIN: the send timeout) ends it: the
 *    core closes it and sets log_fd to -1.  An fd that is not a socket of
 *    ours any more (EBADF, ENOTSOCK: the title closed it) is only let go,
 *    never closed; EINTR is retried; anything else costs only that line.
 *
 * log_fd is read for each line, so ghostldr.lua can take the connection back
 * (log_fd -1) once the core has returned. */
#include "ghost.h"

static void emit(const char *line, size_t n)
{
    struct boot_ctx *c = g_ctx;
    u64 fd = c->log_fd;
    const void *to = (const void *)c->log_addr;
    for (int tries = 0; (s64)fd >= 0 && K.sendto && tries < 3; tries++) {
        if (K.sendto((int)fd, line, n, 0, to, to ? 16 : 0) >= 0 || to)
            break;
        int e = K.errno_fn ? *K.errno_fn() : 0;
        if (e == 4 /* EINTR */)
            continue;
        bool gone = e == 32 || e == 54 || e == 57 || e == 53 || e == 35, lost = e == 9 || e == 38;
        if ((gone || lost) &&
            __atomic_compare_exchange_n(&c->log_fd, &fd, (u64)-1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) && gone)
            K.close((int)fd);
        break;
    }
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
