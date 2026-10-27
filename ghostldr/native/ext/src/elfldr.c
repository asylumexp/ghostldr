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
 * while nothing runs), "boot" (boot games with the extension, when it is
 * the full one), "forget" (the release's ghostldr.lua clears its bundle
 * cache), "log" (this connection gets ghostldr's log) or "help".
 *
 * The release's two-file boot (docs/architecture.md) shares the port: a
 * bundle (ghostldr-ext.bin, ghostldr-elfldr.bin) is told apart by its
 * magic, taken whole and handed to ghostldr.lua (EL_BUNDLE), which checks
 * it, caches it and switches to it.  With a cached full extension the
 * loader starts with a countdown on the TV: unless the user dismisses it
 * or a payload comes first, the games boot when it runs out (EL_BOOT).
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

/* Receive one connection's request: an ELF (*buf, *len: fv_alloc'd), a
 * line of text in cmd[], or the start of a bundle (head[], *have).  1: an
 * ELF, 2: a command, 3: a bundle, 0: nothing came, < 0: an error (said). */
static int receive(int c, unsigned char *head, unsigned long *have_out, unsigned char **buf, unsigned long *len,
                   char *cmd, unsigned cmd_size)
{
    unsigned long have = 0;
    long want = 0;
    static const char magic[] = EL_BUNDLE_MAGIC;
    while (want == 0) {
        long r = fv_recv(c, head + have, 4096 - have);
        if (r <= 0) {
            if (!have)
                return 0;
            say(c, "refused: the connection ended after %lu bytes, inside the headers", have);
            return -1;
        }
        have += (unsigned long)r;
        unsigned long m = have < 8 ? have : 8;
        if (memcmp(head, magic, m) == 0) { /* a bundle, or still maybe one */
            if (have >= 8) {
                *have_out = have;
                return 3;
            }
            continue;
        }
        want = el_file_size(head, have);
        if (want == 0 && have == 4096) {
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
            long r = have < 4096 ? fv_recv(c, head + have, 4096 - have) : 0;
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

/* fill p[0, n): first from the `have` bytes already received at *cur, then
 * from the connection.  0, or -1 (said) */
static int fill(int c, unsigned char *p, unsigned long n, const unsigned char **cur, unsigned long *have,
                unsigned long *done, unsigned long total)
{
    unsigned long from_head = *have < n ? *have : n;
    memcpy(p, *cur, from_head);
    *cur += from_head, *have -= from_head;
    unsigned long got = from_head;
    while (got < n) {
        long r = fv_recv(c, p + got, n - got);
        if (r <= 0) {
            say(c, "refused: %lu of the bundle's %lu bytes came (%s)", *done + got, total,
                r == 0 ? "the client stopped sending" : "timed out");
            return -1;
        }
        got += (unsigned long)r;
    }
    *done += got;
    return 0;
}

/* A bundle (its first `have` bytes in head): taken whole, then handed to
 * ghostldr.lua, which checks it against its pins.  1: taken (c is the log
 * now), else refused (said). */
static int take_bundle(int c, unsigned char *head, unsigned long have, int bundles)
{
    if (!bundles) {
        say(c, "refused: a ghostldr bundle: this ELF loader was started by run.py, which serves the extension "
               "itself; bundles are for the release's ghostldr.lua");
        return -1;
    }
    if (running()) {
        say(c, "refused: a bundle while %u payload%s run%s: send it again once they have finished", running(),
            running() == 1 ? "" : "s", running() == 1 ? "s" : "");
        return -1;
    }
    while (have < EL_BUNDLE_HEADER) {
        long r = fv_recv(c, head + have, 4096 - have);
        if (r <= 0) {
            say(c, "refused: the connection ended after %lu bytes, inside the bundle's header", have);
            return -1;
        }
        have += (unsigned long)r;
    }
    u32 format, hsize, kind;
    u64 ext_size, sel_size;
    memcpy(&format, head + 8, 4), memcpy(&hsize, head + 12, 4), memcpy(&kind, head + 16, 4);
    memcpy(&ext_size, head + 24, 8), memcpy(&sel_size, head + 32, 8);
    if (format != 1 || hsize != EL_BUNDLE_HEADER || (kind != 1 && kind != 2) || !ext_size ||
        ext_size > EL_BUNDLE_MAX || sel_size > EL_BUNDLE_MAX || (kind == 2 && sel_size)) {
        say(c, "refused: a bundle of format %u, kind %u, parts of %lu and %lu bytes: not one this ghostldr.lua "
               "reads; send ghostldr.lua and the bundle from the same release", format, kind, ext_size, sel_size);
        return -1;
    }
    unsigned long total = EL_BUNDLE_HEADER + ext_size + sel_size, done = EL_BUNDLE_HEADER;
    unsigned char hdr[EL_BUNDLE_HEADER];
    memcpy(hdr, head, sizeof(hdr));
    have -= EL_BUNDLE_HEADER;
    const unsigned char *cur = head + EL_BUNDLE_HEADER;
    void *ext = el_map(ext_size), *sel = sel_size ? el_map(sel_size) : NULL;
    int rc = !ext || (sel_size && !sel) ? -2 : 0;
    if (rc == 0)
        rc = fill(c, ext, ext_size, &cur, &have, &done, total);
    if (rc == 0 && sel)
        rc = fill(c, sel, sel_size, &cur, &have, &done, total);
    if (rc != 0) {
        if (rc == -2)
            say(c, "refused: no memory for the bundle's %lu bytes", total);
        if (ext)
            el_unmap(ext, ext_size);
        if (sel)
            el_unmap(sel, sel_size);
        return -1;
    }
    say(c, "received %s (%lu bytes): ghostldr.lua checks it and %s", kind == 1 ? "ghostldr-ext.bin" : "ghostldr-elfldr.bin",
        total, kind == 1 ? "boots it" : "starts the ELF loader with it");
    el_bundle_done(hdr, ext, sel, c);
    return 1;
}

#define CMDS "status, boot, forget, log, quit, help"
/* 1: quit, 2: boot, 3: forget, 4: c is the log now */
static int command(int c, const char *cmd, int can_boot, int bundles)
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
    int what = strcmp(cmd, "quit") == 0 ? 1 : strcmp(cmd, "boot") == 0 ? 2 : strcmp(cmd, "forget") == 0 ? 3 : 0;
    if (what && running()) {
        say(c, "not %s: %u payload%s still running", what == 1 ? "quitting" : what == 2 ? "booting" : "forgetting",
            running(), running() == 1 ? "" : "s");
        return 0;
    }
    if (what == 1) {
        say(c, "quitting: SWRR goes back to Luac0re's loader");
        return 1;
    }
    if (what == 2) {
        if (!can_boot) {
            say(c, "not booting: the extension here is the ELF loader's alone; send ghostldr-ext.bin to this port "
                   "to boot games");
            return 0;
        }
        say(c, "booting games: the ELF loader stops");
        return 2;
    }
    if (what == 3 && !bundles) {
        say(c, "nothing to forget: run.py serves this extension; the bundle cache is the release's ghostldr.lua's");
        return 0;
    }
    if (what == 3) {
        say(c, "forgetting the cached bundles: send ghostldr-ext.bin or ghostldr-elfldr.bin to this port next");
        return 3;
    }
    if (strcmp(cmd, "log") == 0) {
        say(c, "this connection gets ghostldr's log from now on");
        el_log_to(c);
        return 4;
    }
    say(c, "%ssend an ELF, or one of: " CMDS, cmd[0] && strcmp(cmd, "help") ? "unknown command; " : "");
    return 0;
}

/* 0: the connection is done with, 1: a payload's session keeps it, 2: quit,
 * 3: boot, 4: forget, 5: it is the log now, 6: a bundle came (it is the log) */
static int serve_one(int c, int can_boot, int bundles)
{
    unsigned char head[4096];
    unsigned long have = 0;
    unsigned char *buf = NULL;
    unsigned long len = 0;
    char cmd[64];
    int r = receive(c, head, &have, &buf, &len, cmd, sizeof(cmd));
    if (r == 3)
        return take_bundle(c, head, have, bundles) == 1 ? 6 : 0;
    if (r == 2) {
        int k = command(c, cmd, can_boot, bundles);
        return k ? k + 1 : 0;
    }
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

int elfldr_serve(const struct el_serve *o)
{
    int lfd = fv_listen(0, o->port, 8);
    if (lfd < 0) {
        fv_log("elfldr: cannot listen on TCP %u (errno %d)", o->port, -lfd);
        return lfd;
    }
    fv_log("elfldr: listening on TCP %u (send an ELF: nc <ps5> %u < payload.elf)", o->port, o->port);
    /* the countdown: the extension's games boot when it runs out, unless
     * the user dismisses it on the TV or a payload comes first */
    unsigned left = o->can_boot ? o->countdown : 0;
    long tick = fv_clock_ms() + EL_SECOND_MS;
    if (left) {
        fv_log("elfldr: booting games in %u s unless cancelled (on the TV, or by sending a payload)", left);
        el_ui(EL_UI_COUNTDOWN, left);
    } else
        el_ui(EL_UI_READY, o->port);
    int rc = 0;
    for (;;) {
        reap();
        if (left && el_ui_cancelled()) {
            left = 0;
            fv_log("elfldr: countdown cancelled on the TV: the ELF loader stays");
            el_ui(EL_UI_READY, o->port);
        }
        long now = fv_clock_ms();
        if (left && now >= tick) {
            tick += EL_SECOND_MS;
            if (--left == 0) {
                fv_log("elfldr: countdown over: booting games");
                rc = EL_BOOT;
                break;
            }
            el_ui(EL_UI_COUNTDOWN, left);
        }
        long wait = POLL_MS;
        if (left && tick - now < wait)
            wait = tick - now > 0 ? tick - now : 0;
        unsigned addr = 0;
        unsigned short from = 0;
        int c = fv_accept(lfd, (int)wait, &addr, &from);
        if (c == -FV_ETIMEDOUT || c == -FV_EINTR)
            continue;
        if (c < 0) {
            fv_log("elfldr: accept failed (errno %d)", -c);
            fv_sleep_ms(POLL_MS);
            continue;
        }
        fv_log("elfldr: client %u.%u.%u.%u:%u", addr >> 24, addr >> 16 & 255, addr >> 8 & 255, addr & 255, from);
        fv_timeouts(c, EL_TIMEOUT_S);
        int r = serve_one(c, o->can_boot, o->bundles);
        if (r == 1 && left) { /* a payload: the user wants the ELF loader */
            left = 0;
            fv_log("elfldr: a payload came: no countdown, the ELF loader stays");
            el_ui(EL_UI_READY, o->port);
        }
        if (r != 1 && r < 5) /* else a payload's session, or the log, keeps the connection */
            fv_sclose(c);
        if (r >= 2 && r != 5) {
            rc = r == 2 ? 0 : r == 3 ? EL_BOOT : r == 4 ? EL_FORGET : EL_BUNDLE;
            break;
        }
    }
    fv_sclose(lfd);
    return rc;
}
