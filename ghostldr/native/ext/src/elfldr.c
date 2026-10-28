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
 * magic, taken whole, checked against its own header (sizes, CRC-32s) and
 * handed to ghostldr.lua (EL_BUNDLE), which caches it: it is applied on the
 * next ghostldr boot, and the loader carries on with what it had.  Beside
 * the game selector (selecting) it serves on a thread of its own: a bundle
 * is held until a title is picked, quit, boot and forget wait for the pick,
 * and once el_stop_requested it stops when no payload runs.
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
static int g_selecting, g_bundle_held; /* beside the game selector; a bundle waits for ghostldr.lua */

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
                    say(c, "refused: not an ELF, a ghostldr bundle or a command (it starts %02x %02x %02x %02x): a "
                           ".bin that is neither an ELF nor ghostldr's (no GLDRBNDL header) is not taken", head[0],
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

/* CRC-32 (zlib's), as mkbundle.py writes it into a bundle's header */
static u32 crc32(const unsigned char *p, unsigned long n)
{
    static const u32 k_nib[16] = {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
                                  0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
                                  0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
    u32 crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ k_nib[crc & 15];
        crc = (crc >> 4) ^ k_nib[crc & 15];
    }
    return ~crc;
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

/* A bundle (its first `have` bytes in head): taken whole, checked against
 * its own header, then handed to ghostldr.lua, which caches it for the
 * next boot.  1: taken (c is the log now), else refused (said). */
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
    if (g_bundle_held) {
        say(c, "refused: a ghostldr bundle came already; it is cached once a game is picked in the selector");
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
        say(c, "refused: a bundle of format %u, kind %u, parts of %lu and %lu bytes: not a ghostldr-ext.bin or "
               "ghostldr-elfldr.bin this ghostldr reads", format, kind, ext_size, sel_size);
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
    u32 ext_crc, sel_crc;
    memcpy(&ext_crc, hdr + 40, 4), memcpy(&sel_crc, hdr + 44, 4);
    if (rc == 0 && (crc32(ext, ext_size) != ext_crc || (sel && crc32(sel, sel_size) != sel_crc))) {
        say(c, "refused: the bundle came damaged (a CRC-32 in it does not match its header): send it again");
        rc = -1;
    }
    if (rc != 0) {
        if (rc == -2)
            say(c, "refused: no memory for the bundle's %lu bytes", total);
        if (ext)
            el_unmap(ext, ext_size);
        if (sel)
            el_unmap(sel, sel_size);
        return -1;
    }
    u8 build[8];
    memcpy(build, hdr + 112, sizeof(build));
    say(c, "received %s (%lu bytes, build %02x%02x%02x%02x%02x%02x%02x%02x): a ghostldr bundle; ghostldr.lua caches "
           "it for the next ghostldr boot%s", kind == 1 ? "ghostldr-ext.bin" : "ghostldr-elfldr.bin", total, build[0],
        build[1], build[2], build[3], build[4], build[5], build[6], build[7],
        g_selecting ? " once a game is picked in the selector" : "");
    g_bundle_held = g_selecting;
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
    if (what && g_selecting) {
        say(c, "not now: the game selector is up; pick a game there (payloads and ghostldr bundles are taken "
               "until then)");
        return 0;
    }
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
            say(c, "not booting: no ghostldr-ext.bin of this build is cached; send one to this port, then \"boot\" "
                   "(or send ghostldr.lua again)");
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
    fv_log("elfldr: listening on TCP %u (send an ELF: nc <ps5> %u < payload.elf)%s", o->port, o->port,
           o->selecting ? ", beside the game selector" : "");
    g_selecting = o->selecting, g_bundle_held = 0;
    if (!o->selecting) /* the selector has the TV */
        el_ui(o->port);
    int rc = 0;
    unsigned waited = 0;
    for (;;) {
        reap();
        if (o->selecting && el_stop_requested()) {
            if (!running())
                break;
            if (waited != running()) {
                waited = running();
                fv_log("elfldr: a title was picked: waiting for %u payload%s to finish", waited, waited == 1 ? "" : "s");
                el_ui_waiting(waited);
            }
            fv_sleep_ms(POLL_MS);
            continue;
        }
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
        int r = serve_one(c, o->can_boot, o->bundles);
        if (r != 1 && r < 5) /* else a payload's session, or the log, keeps the connection */
            fv_sclose(c);
        if (r == 6 && o->selecting) /* held for ghostldr.lua; serving goes on until the pick */
            continue;
        if (r >= 2 && r != 5) {
            rc = r == 2 ? 0 : r == 3 ? EL_BOOT : r == 4 ? EL_FORGET : EL_BUNDLE;
            break;
        }
    }
    fv_sclose(lfd);
    if (o->selecting)
        fv_log("elfldr: stopped beside the game selector%s", g_bundle_held ? "; a bundle waits for ghostldr.lua" : "");
    g_selecting = 0;
    return rc;
}
