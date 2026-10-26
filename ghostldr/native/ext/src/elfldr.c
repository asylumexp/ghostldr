/*
 * ghostldr's ELF loader service (elfldr.h, docs/elfldr.md).
 *
 * One TCP port, one payload per connection:
 *
 *   nc PS5 9021 < payload.elf
 *
 * The payload's length comes from its own headers (an ELF's program and
 * section headers, a SELF's file size), so a client that never half-closes
 * its side, as `nc` without -N, still gets its payload run.  The
 * connection then stays open: the payload's stdout and stderr come back on
 * it, between lines of this service's own that start with "[elfldr] ", and
 * it is closed when the payload has finished, after a last line with how.
 * Instead of an ELF a client may send one line of text: "status" (what
 * runs), "quit" (stop serving and give SWRR back to Luac0re's loader, only
 * while nothing runs) or "help".
 *
 * Everything here runs on the thread that called elfldr_serve, which also
 * frees each finished payload (el_poll) between connections.  Payloads run
 * on threads of their own (el_run).
 */
#include "ghost.h"
#include "ftp.h"
#include "elfldr.h"

#ifndef EL_TIMEOUT_S
#define EL_TIMEOUT_S 10  /* a client that sends nothing for this long is dropped */
#endif
#define POLL_MS 250      /* finished payloads are freed at least this often */
#define LINE 512

struct sess {
    struct el_payload *p; /* NULL: free */
    int conn;
    unsigned id;
};

static struct sess g_s[EL_SESSIONS];
static unsigned g_next_id = 1;

static void say(int fd, const char *f, ...) __attribute__((format(printf, 2, 3)));
static void say(int fd, const char *f, ...)
{
    char line[LINE];
    int n = fmt(line, sizeof(line), "[elfldr] ");
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    n += vfmt(line + n, sizeof(line) - n - 1, f, ap);
    __builtin_va_end(ap);
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    fv_log("elfldr: %s", line + 9);
    line[n++] = '\n';
    if (fd >= 0)
        fv_send(fd, line, (unsigned long)n);
}

static u64 rd64(const unsigned char *b) { u64 v; memcpy(&v, b, 8); return v; }
static u16 rd16(const unsigned char *b) { u16 v; memcpy(&v, b, 2); return v; }

long el_file_size(const unsigned char *b, unsigned long have)
{
    static const unsigned char elf[4] = {0x7f, 'E', 'L', 'F'}, self[4] = {0x4f, 0x15, 0x3d, 0x1d};
    bool is_elf = true, is_self = true;
    for (unsigned long i = 0; i < 4 && i < have; i++)
        is_elf &= b[i] == elf[i], is_self &= b[i] == self[i];
    if (!is_elf && !is_self)
        return -1;
    if (is_self)
        return have < 0x20 ? 0 : rd64(b + 0x10) >= 0x20 && rd64(b + 0x10) < (1UL << 40) ? (long)rd64(b + 0x10) : -1;
    if (have < 64)
        return 0;
    if (b[4] != 2) /* ELFCLASS64 only: a 32-bit one is refused once received */
        return 64;
    u64 phoff = rd64(b + 0x20), shoff = rd64(b + 0x28);
    u16 phent = rd16(b + 0x36), phnum = rd16(b + 0x38), shent = rd16(b + 0x3A), shnum = rd16(b + 0x3C);
    if (phoff >= (1UL << 32) || shoff >= (1UL << 40) || (phnum && phent != 56))
        return -1;
    u64 end = 64, ph_end = phoff + (u64)phnum * phent;
    if (shoff && shoff + (u64)shnum * shent > end)
        end = shoff + (u64)shnum * shent;
    if (ph_end > end)
        end = ph_end;
    if (phnum && have < ph_end)
        return 0;
    for (u32 i = 0; i < phnum; i++) {
        const unsigned char *p = b + phoff + i * 56;
        u64 off = rd64(p + 8), filesz = rd64(p + 32);
        if (filesz && off < (1UL << 40) && filesz < (1UL << 40) && off + filesz > end)
            end = off + filesz;
    }
    return (long)end;
}

/* Receive one connection's request: an ELF (*buf, *len: fv_alloc'd), or a
 * line of text in cmd[].  1: an ELF, 2: a command, 0: nothing came, < 0: an
 * error (said). */
static int receive(int c, unsigned char **buf, unsigned long *len, char *cmd, unsigned cmd_size)
{
    unsigned char head[4096];
    unsigned long have = 0;
    long want = 0;
    while (want == 0) {
        long r = fv_recv(c, head + have, sizeof(head) - have);
        if (r <= 0) {
            if (!have)
                return 0;
            say(c, "refused: the connection ended after %lu bytes, inside the headers", have);
            return -1;
        }
        have += (unsigned long)r;
        want = el_file_size(head, have);
        if (want == 0 && have == sizeof(head)) {
            say(c, "refused: no ELF headers in the first %lu bytes", have);
            return -1;
        }
    }
    if (want < 0) { /* a line of text */
        unsigned n = 0;
        for (;;) {
            for (; n < have && n < cmd_size - 1 && head[n] != '\n' && head[n] != '\r'; n++) {
                if (head[n] < 0x20 || head[n] > 0x7e) {
                    say(c, "refused: neither an ELF nor a command (it starts %02x %02x %02x %02x)", head[0],
                        have > 1 ? head[1] : 0, have > 2 ? head[2] : 0, have > 3 ? head[3] : 0);
                    return -1;
                }
                cmd[n] = (char)head[n];
            }
            if (n < have || n == cmd_size - 1)
                break;
            long r = have < sizeof(head) ? fv_recv(c, head + have, sizeof(head) - have) : 0;
            if (r <= 0)
                break;
            have += (unsigned long)r;
        }
        cmd[n] = 0;
        return 2;
    }
    if ((unsigned long)want > EL_MAX_PAYLOAD) {
        say(c, "refused: the ELF is %ld bytes; at most %u are taken", want, EL_MAX_PAYLOAD);
        return -1;
    }
    unsigned char *b = fv_alloc((unsigned long)want);
    if (!b) {
        say(c, "refused: no memory for %ld bytes", want);
        return -1;
    }
    unsigned long got = have < (unsigned long)want ? have : (unsigned long)want;
    memcpy(b, head, got);
    while (got < (unsigned long)want) {
        long r = fv_recv(c, b + got, (unsigned long)want - got);
        if (r <= 0) {
            say(c, "refused: %lu of %ld bytes came (%s)", got, want, r == 0 ? "the client stopped sending" : "timed out");
            fv_free(b, (unsigned long)want);
            return -1;
        }
        got += (unsigned long)r;
    }
    *buf = b, *len = (unsigned long)want;
    return 1;
}

static unsigned running(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < EL_SESSIONS; i++)
        n += g_s[i].p != NULL;
    return n;
}

/* finished payloads: their last line, then the connection closes */
static void reap(void)
{
    char line[LINE];
    for (unsigned i = 0; i < EL_SESSIONS; i++) {
        struct sess *s = &g_s[i];
        if (!s->p || !el_poll(s->p, line, sizeof(line)))
            continue;
        say(s->conn, "%s", line);
        fv_sclose(s->conn);
        s->p = NULL;
    }
}

/* 1: quit */
static int command(int c, const char *cmd)
{
    char line[LINE];
    if (strcmp(cmd, "status") == 0) {
        say(c, "%u payload%s running", running(), running() == 1 ? "" : "s");
        for (unsigned i = 0; i < EL_SESSIONS; i++)
            if (g_s[i].p) {
                el_describe(g_s[i].p, line, sizeof(line));
                say(c, "%s", line);
            }
        return 0;
    }
    if (strcmp(cmd, "quit") == 0) {
        if (running()) {
            say(c, "not quitting: %u payload%s still running", running(), running() == 1 ? "" : "s");
            return 0;
        }
        say(c, "quitting: SWRR goes back to Luac0re's loader");
        return 1;
    }
    say(c, "%ssend an ELF, or one of: status, quit, help", cmd[0] && strcmp(cmd, "help") ? "unknown command; " : "");
    return 0;
}

static int serve_one(int c)
{
    unsigned char *buf = NULL;
    unsigned long len = 0;
    char cmd[64];
    int r = receive(c, &buf, &len, cmd, sizeof(cmd));
    if (r == 2)
        return command(c, cmd) == 1 ? 2 : 0;
    if (r != 1)
        return 0;
    struct sess *s = NULL;
    for (unsigned i = 0; i < EL_SESSIONS && !s; i++)
        if (!g_s[i].p)
            s = &g_s[i];
    char info[LINE];
    if (!s) {
        say(c, "refused: %u payloads are running already", running());
        fv_free(buf, len);
        return 0;
    }
    unsigned id = g_next_id++;
    struct el_payload *p = NULL;
    int rc = el_load(buf, len, id, c, &p, info, sizeof(info));
    fv_free(buf, len);
    if (rc != 0) {
        say(c, "payload %u refused: %s", id, info);
        return 0;
    }
    say(c, "%s", info);
    if (el_run(p, info, sizeof(info)) != 0) {
        say(c, "payload %u did not start: %s", id, info);
        return 0;
    }
    s->p = p, s->conn = c, s->id = id;
    return 1;
}

int elfldr_serve(unsigned short port)
{
    int lfd = fv_listen(0, port, 8);
    if (lfd < 0) {
        fv_log("elfldr: cannot listen on TCP %u (errno %d)", port, -lfd);
        return lfd;
    }
    fv_log("elfldr: listening on TCP %u (send an ELF: nc <ps5> %u < payload.elf)", port, port);
    for (;;) {
        reap();
        unsigned addr = 0;
        unsigned short from = 0;
        int c = fv_accept(lfd, POLL_MS, &addr, &from);
        if (c == -FV_ETIMEDOUT || c == -FV_EINTR)
            continue;
        if (c < 0) {
            fv_log("elfldr: accept failed (errno %d)", -c);
            fv_sleep_ms(POLL_MS);
            continue;
        }
        fv_log("elfldr: client %u.%u.%u.%u:%u", addr >> 24, addr >> 16 & 255, addr >> 8 & 255, addr & 255, from);
        fv_timeouts(c, EL_TIMEOUT_S);
        int r = serve_one(c);
        if (r != 1) /* else the payload's session keeps the connection */
            fv_sclose(c);
        if (r == 2)
            break;
    }
    fv_sclose(lfd);
    return 0;
}
