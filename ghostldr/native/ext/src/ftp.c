/*
 * ghostldr's FTP server (ftp.h, docs/ftp.md): RFC 959 with what clients
 * rely on besides: FEAT and OPTS (RFC 2389), EPSV and EPRT (RFC 2428),
 * SIZE, MDTM, REST STREAM, MLST and MLSD (RFC 3659).  Any user name and
 * password are accepted, as by the FTP servers of console homebrew.  Paths
 * are the process's own: on the console, SWRR's sandbox, where the USB
 * drive is /ghostldr_usb.
 *
 * One thread accepts clients and each client gets a thread of its own and
 * one of FTP_SESSIONS slots.  ftp_stop ends them all (the game selector's
 * server, before the extension's code goes; ftp_sys.c).  Everything is made
 * once, at ftp_start: the
 * slots and their transfer buffers (a title is running by then, and memory
 * taken later would come out of what it was promised), and every socket the
 * server listens on, each slot's passive data port included (on a PS5 at
 * 8.00 or later SWRR can't create a socket itself once the title runs:
 * ftp_sys.c).  So passive mode always answers with the slot's own port, and
 * active mode works only where the environment can still connect a new
 * socket.  Transfers are binary (TYPE
 * A is accepted and ignored).  Uploads are written in FTP_BUF pieces, which
 * a USB drive takes far faster than the network's small ones.  Clients that
 * want more speed open more connections: gfs_pack.py --usb sends eight files
 * at once, each from its own thread here.
 */
#include "ghost.h"
#include "ftp.h"

#define FTP_PATH 1024
#define FTP_LINE 1100
#define FV_EADDRINUSE 48

struct sess {
    int ctl;                 /* control socket */
    volatile int data;       /* the transfer's data socket, -1 between transfers (ftp_stop wakes it) */
    int dl;                  /* the slot's passive data listener */
    unsigned short dport;    /* its port */
    bool pasv;               /* PASV/EPSV given: the next transfer's connection comes to dl */
    unsigned local, peer;    /* our address on the control connection, the client's */
    unsigned act_addr;       /* PORT/EPRT: where the client listens */
    unsigned short act_port;
    bool act;
    long rest;               /* REST: where the next transfer starts */
    unsigned id;
    volatile int busy;
    char cwd[FTP_PATH], rnfr[FTP_PATH];
    char in[FTP_LINE * 2]; /* control input not yet taken */
    unsigned in_len;
    char line[FTP_LINE];
    struct fv_dir dir;
    u8 *buf; /* FTP_BUF bytes */
};

static struct sess *g_sess[FTP_SESSIONS];
static int g_lfd = -1;
static unsigned short g_port;
static unsigned g_ids;
static volatile int g_stop, g_listening; /* ftp_stop asked; the listener thread runs */

static const char *err_text(long e)
{
    switch (-e) {
    case FV_EPERM:
    case FV_EACCES:
        return "Permission denied";
    case FV_ENOENT:
        return "No such file or directory";
    case FV_EIO:
        return "I/O error";
    case FV_ENOMEM:
        return "Out of memory";
    case FV_EBUSY:
        return "Busy";
    case FV_EEXIST:
        return "Already exists";
    case FV_EXDEV:
        return "Not on the same drive";
    case FV_ENOTDIR:
        return "Not a directory";
    case FV_EISDIR:
        return "Is a directory";
    case FV_EINVAL:
        return "Invalid argument";
    case FV_ENOSPC:
        return "No space left on the drive";
    case FV_EROFS:
        return "Read-only file system";
    case FV_ETIMEDOUT:
        return "Timed out";
    case FV_EADDRINUSE:
        return "Address in use";
    case FV_ENOSYS:
        return "Not available on this console (use passive mode)";
    case FV_ENAMETOOLONG:
        return "Name too long";
    case FV_ENOTEMPTY:
        return "Directory not empty";
    default:
        return "Failed";
    }
}

/* ---- replies ---- */

static bool say(struct sess *s, const char *f, ...) __attribute__((format(printf, 2, 3)));
static bool say(struct sess *s, const char *f, ...)
{
    char b[FTP_PATH + 160];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(b, sizeof(b) - 2, f, ap);
    __builtin_va_end(ap);
    if (n < 0)
        n = 0;
    if (n > (int)sizeof(b) - 3)
        n = (int)sizeof(b) - 3;
    b[n++] = '\r';
    b[n++] = '\n';
    return fv_send(s->ctl, b, (unsigned long)n) == n;
}

/* code and the error's text */
static void fail(struct sess *s, int code, const char *path, long e)
{
    say(s, "%d %s: %s (errno %ld)", code, path, err_text(e), -e);
}

/* p with its double quotes doubled, for 257 replies */
static void quote(char *o, unsigned cap, const char *p)
{
    unsigned n = 0;
    for (; *p && n + 3 < cap; p++) {
        if (*p == '"')
            o[n++] = '"';
        o[n++] = *p;
    }
    o[n] = 0;
}

/* ---- paths ---- */

/* arg in the session's directory, absolute and without empty, "." or ".."
 * parts (never above "/"), into out (FTP_PATH bytes): 0, or -1 when too long */
static int resolve(const struct sess *s, const char *arg, char *out)
{
    char tmp[FTP_PATH * 2];
    if (!arg || !*arg)
        arg = ".";
    int n = arg[0] == '/' ? fmt(tmp, sizeof(tmp), "%s", arg) : fmt(tmp, sizeof(tmp), "%s/%s", s->cwd, arg);
    if (n >= (int)sizeof(tmp))
        return -1;
    unsigned len = 1;
    out[0] = '/';
    for (const char *p = tmp; *p;) {
        while (*p == '/')
            p++;
        const char *seg = p;
        while (*p && *p != '/')
            p++;
        unsigned k = (unsigned)(p - seg);
        if (!k || (k == 1 && seg[0] == '.'))
            continue;
        if (k == 2 && seg[0] == '.' && seg[1] == '.') {
            while (len > 1 && out[len - 1] != '/')
                len--;
            if (len > 1)
                len--;
            continue;
        }
        if (len + 1 + k >= FTP_PATH)
            return -1;
        if (len > 1)
            out[len++] = '/';
        memcpy(out + len, seg, k);
        len += k;
    }
    out[len] = 0;
    return 0;
}

/* a path command's resolved path, or false with a 501 sent */
static bool path_arg(struct sess *s, const char *arg, char *path)
{
    if (!*arg) {
        say(s, "501 A path is needed");
        return false;
    }
    if (resolve(s, arg, path) != 0) {
        say(s, "501 Path too long");
        return false;
    }
    return true;
}

static const char *base_name(const char *path)
{
    const char *b = strrchr_c(path, '/');
    return b && b[1] ? b + 1 : path;
}

static bool is_dir(const struct fv_stat *st) { return (st->mode & FV_S_IFMT) == FV_S_IFDIR; }

/* ---- times ---- */

/* t (seconds since 1970) as year, month, day, hour, minute, second */
static void civil(long t, unsigned v[6])
{
    long days = t / 86400, rem = t % 86400;
    if (rem < 0)
        rem += 86400, days--;
    v[3] = (unsigned)(rem / 3600);
    v[4] = (unsigned)(rem % 3600 / 60);
    v[5] = (unsigned)(rem % 60);
    days += 719468; /* days from 0000-03-01 */
    long era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned long doe = (unsigned long)(days - era * 146097);
    unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned long mp = (5 * doy + 2) / 153;
    v[2] = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    v[1] = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
    long y = (long)yoe + era * 400 + (v[1] <= 2);
    v[0] = y < 0 ? 0 : y > 9999 ? 9999 : (unsigned)y;
}

/* YYYYMMDDHHMMSS (MDTM, MLSD's modify) */
static void stamp(char out[16], long t)
{
    unsigned v[6];
    civil(t, v);
    fmt(out, 16, "%04u%02u%02u%02u%02u%02u", v[0], v[1], v[2], v[3], v[4], v[5]);
}

/* ---- control input ---- */

/* the next command into s->line (CR LF or LF ended; Telnet commands, which
 * clients send ahead of ABOR, dropped): false when the client is gone or
 * went quiet */
static bool read_line(struct sess *s)
{
    for (;;) {
        for (unsigned i = 0; i < s->in_len; i++) {
            if (s->in[i] != '\n')
                continue;
            unsigned n = 0;
            for (unsigned j = 0; j < i; j++) {
                u8 c = (u8)s->in[j];
                if (c == 0xFF) { /* IAC and its command */
                    j++;
                    continue;
                }
                if (c != '\r' && c && n < FTP_LINE - 1)
                    s->line[n++] = (char)c;
            }
            s->line[n] = 0;
            memmove(s->in, s->in + i + 1, s->in_len - i - 1);
            s->in_len -= i + 1;
            return true;
        }
        if (s->in_len >= sizeof(s->in)) /* a line longer than any command: dropped */
            s->in_len = 0;
        long r = fv_recv(s->ctl, s->in + s->in_len, sizeof(s->in) - s->in_len);
        if (r <= 0)
            return false;
        s->in_len += (unsigned)r;
    }
}

/* ---- data connections ---- */

/* the data connection for a transfer: the client's to the slot's passive
 * listener (from its own address only), or ours to its PORT.  A socket, or
 * -1 with the 425 reply sent. */
static int data_open(struct sess *s)
{
    int fd;
    if (s->pasv) {
        unsigned a = 0;
        s->pasv = false;
        fd = fv_accept(s->dl, 30000, &a, NULL);
        if (fd >= 0 && a != s->peer) {
            fv_sclose(fd);
            say(s, "425 The data connection came from another address");
            return -1;
        }
    } else if (s->act) {
        s->act = false;
        fd = fv_connect(s->act_addr, s->act_port);
    } else {
        say(s, "425 Use PASV, EPSV or PORT first");
        return -1;
    }
    if (fd < 0) {
        say(s, "425 No data connection: %s (errno %d)", err_text(fd), -fd);
        return -1;
    }
    fv_timeouts(fd, 60);
    s->data = fd;
    return fd;
}

static void cmd_pasv(struct sess *s, bool ext)
{
    /* connections left waiting from an earlier PASV that was never used */
    for (int fd; (fd = fv_accept(s->dl, 0, NULL, NULL)) >= 0;)
        fv_sclose(fd);
    s->act = false;
    s->pasv = true;
    unsigned short p = s->dport;
    if (ext)
        say(s, "229 Entering Extended Passive Mode (|||%u|)", p);
    else
        say(s, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)", s->local >> 24, (s->local >> 16) & 255,
            (s->local >> 8) & 255, s->local & 255, (unsigned)p >> 8, (unsigned)p & 255);
}

static bool parse_num(const char **p, unsigned *v, unsigned max)
{
    unsigned n = 0, d = 0;
    while (**p >= '0' && **p <= '9' && d < 10)
        n = n * 10 + (unsigned)(*(*p)++ - '0'), d++;
    if (!d || n > max)
        return false;
    *v = n;
    return true;
}

/* PORT h1,h2,h3,h4,p1,p2 or EPRT |1|a.b.c.d|port| (to the client's own address only) */
static void cmd_port(struct sess *s, const char *arg, bool ext)
{
    unsigned v[6] = {0}, a, port = 0;
    const char *p = arg;
    bool ok;
    if (ext) {
        char d = *p++;
        ok = d && p[0] == '1' && p[1] == d;
        p += 2;
        for (unsigned i = 0; ok && i < 4; i++)
            ok = parse_num(&p, &v[i], 255) && *p++ == (i < 3 ? '.' : d);
        ok = ok && parse_num(&p, &port, 65535) && *p == d;
    } else {
        ok = true;
        for (unsigned i = 0; ok && i < 6; i++)
            ok = parse_num(&p, &v[i], 255) && (i == 5 || *p++ == ',');
        port = v[4] << 8 | v[5];
    }
    if (!ok || !port) {
        say(s, "501 Bad address");
        return;
    }
    a = v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3];
    if (a != s->peer) {
        say(s, "504 Data connections go to the client's own address");
        return;
    }
    s->pasv = false;
    s->act = true;
    s->act_addr = a;
    s->act_port = (unsigned short)port;
    say(s, "200 OK");
}

/* ---- listings ---- */

enum { L_LIST, L_NLST, L_MLSD };

static const char k_mon[12][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/* MLSD and MLST facts, "type=...;...; " */
static int facts(char *o, unsigned cap, const struct fv_stat *st)
{
    char t[16];
    stamp(t, st->mtime);
    if (is_dir(st))
        return fmt(o, cap, "type=dir;modify=%s;perm=cdeflmp; ", t);
    return fmt(o, cap, "type=file;size=%ld;modify=%s;perm=adfrw; ", st->size, t);
}

/* one entry of a listing, at most cap bytes: its length */
static int list_line(char *o, unsigned cap, int kind, const char *name, const struct fv_stat *st, long now)
{
    if (kind == L_NLST)
        return fmt(o, cap, "%s\r\n", name);
    if (kind == L_MLSD) {
        int n = facts(o, cap, st);
        return n + fmt(o + n, cap - (unsigned)n, "%s\r\n", name);
    }
    unsigned v[6];
    civil(st->mtime, v);
    char when[16];
    long age = now - st->mtime;
    if (age > -3600 && age < 180L * 86400)
        fmt(when, sizeof(when), "%s %2u %02u:%02u", k_mon[(v[1] - 1) % 12], v[2], v[3], v[4]);
    else
        fmt(when, sizeof(when), "%s %2u  %4u", k_mon[(v[1] - 1) % 12], v[2], v[0]);
    return fmt(o, cap, "%s 1 ps ps %13ld %s %s\r\n", is_dir(st) ? "drwxrwxrwx" : "-rw-rw-rw-", is_dir(st) ? 0 : st->size,
               when, name);
}

static void cmd_list(struct sess *s, const char *arg, int kind)
{
    /* LIST -la [path]: ls options are ignored */
    while (kind != L_MLSD && arg[0] == '-') {
        while (*arg && *arg != ' ')
            arg++;
        while (*arg == ' ')
            arg++;
    }
    char path[FTP_PATH];
    if (resolve(s, arg, path) != 0) {
        say(s, "501 Path too long");
        return;
    }
    struct fv_stat st;
    long rc = fv_stat(path, &st);
    if (rc == 0 && kind == L_MLSD && !is_dir(&st))
        rc = -FV_ENOTDIR;
    if (rc == 0 && is_dir(&st))
        rc = fv_opendir(path, &s->dir);
    if (rc < 0) {
        fail(s, 550, path, rc);
        return;
    }
    int fd = data_open(s);
    if (fd < 0) {
        if (is_dir(&st))
            fv_closedir(&s->dir);
        return;
    }
    say(s, "150 Listing %s", path);
    long now = fv_now();
    unsigned used = 0;
    bool ok = true;
    if (!is_dir(&st)) {
        used = (unsigned)list_line((char *)s->buf, FTP_BUF, kind, base_name(path), &st, now);
    } else {
        char name[256], full[FTP_PATH + 260];
        int r;
        while (ok && (r = fv_readdir(&s->dir, name, sizeof(name))) > 0) {
            struct fv_stat e = {0, 0, 0};
            if (kind != L_NLST) {
                fmt(full, sizeof(full), "%s%s%s", path, path[1] ? "/" : "", name);
                if (fv_stat(full, &e) < 0)
                    continue;
            }
            if (FTP_BUF - used < 2048) {
                ok = fv_send(fd, s->buf, used) == (long)used;
                used = 0;
            }
            used += (unsigned)list_line((char *)s->buf + used, FTP_BUF - used, kind, name, &e, now);
        }
        fv_closedir(&s->dir);
    }
    if (ok && used)
        ok = fv_send(fd, s->buf, used) == (long)used;
    s->data = -1;
    fv_sclose(fd);
    say(s, ok ? "226 Listing sent" : "426 Connection closed; listing stopped");
}

static void cmd_mlst(struct sess *s, const char *arg)
{
    char path[FTP_PATH], f[160];
    struct fv_stat st;
    if (resolve(s, arg, path) != 0) {
        say(s, "501 Path too long");
        return;
    }
    long rc = fv_stat(path, &st);
    if (rc < 0) {
        fail(s, 550, path, rc);
        return;
    }
    facts(f, sizeof(f), &st);
    say(s, "250-Listing %s\r\n %s%s\r\n250 End", path, f, path);
}

/* ---- transfers ---- */

static long write_all(int f, const u8 *p, unsigned n)
{
    unsigned done = 0;
    while (done < n) {
        long w = fv_write(f, p + done, n - done);
        if (w <= 0)
            return w < 0 ? w : -FV_EIO;
        done += (unsigned)w;
    }
    return done;
}

/* the reply and a log line for a finished transfer (disk_ms: the part of it
 * spent reading or writing the file) */
static void xfer_done(struct sess *s, const char *what, const char *path, long total, long t0, long disk_ms, int code,
                      long err)
{
    long ms = fv_clock_ms() - t0;
    if (ms <= 0)
        ms = 1;
    fv_log("ftp %u: %s %s: %ld bytes in %ld ms (%ld KiB/s; the file took %ld ms)%s", s->id, what, path, total, ms,
           total * 1000 / 1024 / ms, disk_ms, code == 226 ? "" : ", stopped");
    if (code == 226)
        say(s, "226 Transfer complete (%ld bytes)", total);
    else if (code == 426)
        say(s, "426 Connection closed; transfer stopped after %ld bytes", total);
    else
        say(s, "%d %s after %ld bytes (errno %ld)", code, err_text(err), total, -err);
}

static void cmd_retr(struct sess *s, const char *arg)
{
    char path[FTP_PATH];
    long rest = s->rest, rc;
    s->rest = 0;
    struct fv_stat st;
    if (!path_arg(s, arg, path))
        return;
    rc = fv_stat(path, &st);
    if (rc == 0 && is_dir(&st))
        rc = -FV_EISDIR;
    int f = rc < 0 ? (int)rc : fv_open(path, FV_O_RDONLY, 0);
    if (f >= 0 && rest > 0 && (rc = fv_seek(f, rest, 0)) < 0)
        fv_close(f), f = (int)rc;
    if (f < 0) {
        fail(s, 550, path, f);
        return;
    }
    int fd = data_open(s);
    if (fd < 0) {
        fv_close(f);
        return;
    }
    say(s, "150 Sending %s (%ld bytes)", path, st.size > rest ? st.size - rest : 0);
    long t0 = fv_clock_ms(), total = 0, err = 0, disk = 0;
    int code = 226;
    for (;;) {
        long t = fv_clock_ms();
        long n = fv_read(f, s->buf, FTP_BUF);
        disk += fv_clock_ms() - t;
        if (n <= 0) {
            if (n < 0)
                code = 451, err = n;
            break;
        }
        if (fv_send(fd, s->buf, (unsigned long)n) != n) {
            code = 426;
            break;
        }
        total += n;
    }
    fv_close(f);
    s->data = -1;
    fv_sclose(fd);
    xfer_done(s, "RETR", path, total, t0, disk, code, err);
}

static void cmd_stor(struct sess *s, const char *arg, bool append)
{
    char path[FTP_PATH];
    long rest = s->rest, rc = 0;
    s->rest = 0;
    if (!path_arg(s, arg, path))
        return;
    int flags = FV_O_WRONLY | FV_O_CREAT | (append ? FV_O_APPEND : rest > 0 ? 0 : FV_O_TRUNC);
    int f = fv_open(path, flags, 0666);
    if (f >= 0 && rest > 0 && !append && (rc = fv_seek(f, rest, 0)) < 0)
        fv_close(f), f = (int)rc;
    if (f < 0) {
        fail(s, 550, path, f);
        return;
    }
    int fd = data_open(s);
    if (fd < 0) {
        fv_close(f);
        return;
    }
    say(s, "150 Receiving %s", path);
    long t0 = fv_clock_ms(), total = 0, err = 0, disk = 0;
    int code = 226;
    bool eof = false;
    while (!eof && code == 226) {
        unsigned have = 0;
        while (have < FTP_BUF) {
            long n = fv_recv(fd, s->buf + have, FTP_BUF - have);
            if (n <= 0) {
                if (n < 0)
                    code = 426;
                eof = true;
                break;
            }
            have += (unsigned)n;
        }
        long t = fv_clock_ms();
        long w = have ? write_all(f, s->buf, have) : 0;
        disk += fv_clock_ms() - t;
        if (w < 0) {
            code = w == -FV_ENOSPC ? 452 : 451, err = w;
            break;
        }
        total += have;
    }
    long t = fv_clock_ms();
    rc = fv_close(f);
    disk += fv_clock_ms() - t;
    if (rc < 0 && code == 226)
        code = 451, err = rc;
    s->data = -1;
    fv_sclose(fd);
    xfer_done(s, append ? "APPE" : "STOR", path, total, t0, disk, code, err);
}

/* ---- commands ---- */

static void cmd_cwd(struct sess *s, const char *arg)
{
    char path[FTP_PATH];
    struct fv_stat st;
    if (!path_arg(s, arg, path))
        return;
    long rc = fv_stat(path, &st);
    if (rc == 0 && !is_dir(&st))
        rc = -FV_ENOTDIR;
    if (rc < 0) {
        fail(s, 550, path, rc);
        return;
    }
    strlcpy(s->cwd, path, sizeof(s->cwd));
    say(s, "250 Directory is %s", path);
}

/* MKD, SIZE, MDTM, DELE, RMD */
static void cmd_path_op(struct sess *s, const char *verb, const char *arg)
{
    char path[FTP_PATH], q[FTP_PATH + 8];
    if (!path_arg(s, arg, path))
        return;
    struct fv_stat st;
    long rc;
    if (strcmp(verb, "MKD") == 0) {
        rc = fv_mkdir(path);
        if (rc < 0) {
            fail(s, 550, path, rc);
            return;
        }
        quote(q, sizeof(q), path);
        fv_log("ftp %u: MKD %s", s->id, path);
        say(s, "257 \"%s\" created", q);
        return;
    }
    if (strcmp(verb, "SIZE") == 0 || strcmp(verb, "MDTM") == 0) {
        char t[16];
        rc = fv_stat(path, &st);
        if (rc == 0 && verb[0] == 'S' && is_dir(&st))
            rc = -FV_EISDIR;
        if (rc < 0) {
            fail(s, 550, path, rc);
        } else if (verb[0] == 'S') {
            say(s, "213 %ld", st.size);
        } else {
            stamp(t, st.mtime);
            say(s, "213 %s", t);
        }
        return;
    }
    rc = strcmp(verb, "DELE") == 0 ? fv_unlink(path) : fv_rmdir(path);
    if (rc < 0) {
        fail(s, 550, path, rc);
        return;
    }
    fv_log("ftp %u: %s %s", s->id, verb, path);
    say(s, "250 %s removed", path);
}

static void cmd_rename(struct sess *s, const char *arg, bool to)
{
    char path[FTP_PATH];
    struct fv_stat st;
    if (!path_arg(s, arg, path))
        return;
    if (!to) {
        long rc = fv_stat(path, &st);
        if (rc < 0) {
            fail(s, 550, path, rc);
            return;
        }
        strlcpy(s->rnfr, path, sizeof(s->rnfr));
        say(s, "350 Ready for RNTO");
        return;
    }
    if (!s->rnfr[0]) {
        say(s, "503 RNFR first");
        return;
    }
    long rc = fv_rename(s->rnfr, path);
    if (rc < 0) {
        fail(s, 550, path, rc);
    } else {
        fv_log("ftp %u: RNFR %s RNTO %s", s->id, s->rnfr, path);
        say(s, "250 Renamed to %s", path);
    }
    s->rnfr[0] = 0;
}

static const char k_feat[] = "211-Features:\r\n"
                             " EPSV\r\n"
                             " MDTM\r\n"
                             " MLST type*;size*;modify*;perm*;\r\n"
                             " REST STREAM\r\n"
                             " SIZE\r\n"
                             " TVFS\r\n"
                             " UTF8\r\n"
                             "211 End";

static const char k_help[] = "214-Commands:\r\n"
                             " USER PASS QUIT NOOP SYST FEAT OPTS HELP STAT TYPE MODE STRU ALLO\r\n"
                             " PWD CWD CDUP LIST NLST MLSD MLST SIZE MDTM PASV EPSV PORT EPRT\r\n"
                             " RETR STOR APPE REST DELE MKD RMD RNFR RNTO ABOR SITE\r\n"
                             "214 End";

/* one command: false when the session ends */
static bool dispatch(struct sess *s)
{
    char verb[8];
    unsigned k = 0;
    const char *l = s->line;
    while (*l == ' ')
        l++;
    while (*l && *l != ' ' && k < sizeof(verb) - 1) {
        char c = *l++;
        verb[k++] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
    }
    verb[k] = 0;
    if (*l && *l != ' ') {
        say(s, "500 Unknown command");
        return true;
    }
    const char *arg = *l ? l + 1 : l;
#define IS(v) (strcmp(verb, v) == 0)
    if (IS("USER"))
        say(s, "331 Any password will do");
    else if (IS("PASS"))
        say(s, "230 Logged in");
    else if (IS("ACCT") || IS("ALLO"))
        say(s, "202 Not needed");
    else if (IS("QUIT")) {
        say(s, "221 Goodbye");
        return false;
    } else if (IS("NOOP"))
        say(s, "200 OK");
    else if (IS("SYST"))
        say(s, "215 UNIX Type: L8");
    else if (IS("FEAT"))
        say(s, "%s", k_feat);
    else if (IS("HELP"))
        say(s, "%s", k_help);
    else if (IS("OPTS")) {
        if (strncmp(arg, "UTF8", 4) == 0 || strncmp(arg, "utf8", 4) == 0)
            say(s, "200 UTF-8 is always on");
        else if (strncmp(arg, "MLST", 4) == 0 || strncmp(arg, "mlst", 4) == 0)
            say(s, "200 MLST OPTS type;size;modify;perm;");
        else
            say(s, "501 Unknown option");
    } else if (IS("STAT") && !*arg)
        say(s, "211 ghostldr FTP server, session %u, in %s", s->id, s->cwd);
    else if (IS("TYPE")) {
        char t = arg[0] >= 'a' ? (char)(arg[0] - 32) : arg[0];
        if (t == 'A' || t == 'I' || t == 'L')
            say(s, "200 Type set to %c", t);
        else
            say(s, "504 Unknown type");
    } else if (IS("MODE") || IS("STRU")) {
        char t = arg[0] >= 'a' ? (char)(arg[0] - 32) : arg[0];
        if ((IS("MODE") && t == 'S') || (IS("STRU") && t == 'F'))
            say(s, "200 OK");
        else
            say(s, "504 Only stream mode and file structure");
    } else if (IS("PWD") || IS("XPWD")) {
        char q[FTP_PATH + 8];
        quote(q, sizeof(q), s->cwd);
        say(s, "257 \"%s\" is the current directory", q);
    } else if (IS("CWD") || IS("XCWD"))
        cmd_cwd(s, arg);
    else if (IS("CDUP") || IS("XCUP"))
        cmd_cwd(s, "..");
    else if (IS("PASV"))
        cmd_pasv(s, false);
    else if (IS("EPSV")) {
        if (strncmp(arg, "ALL", 3) == 0 || strncmp(arg, "all", 3) == 0)
            say(s, "200 EPSV only from now on");
        else if (*arg && arg[0] != '1')
            say(s, "522 IPv4 only (1)");
        else
            cmd_pasv(s, true);
    } else if (IS("PORT") || IS("EPRT"))
        cmd_port(s, arg, IS("EPRT"));
    else if (IS("LIST"))
        cmd_list(s, arg, L_LIST);
    else if (IS("NLST"))
        cmd_list(s, arg, L_NLST);
    else if (IS("MLSD"))
        cmd_list(s, arg, L_MLSD);
    else if (IS("MLST"))
        cmd_mlst(s, arg);
    else if (IS("RETR"))
        cmd_retr(s, arg);
    else if (IS("STOR") || IS("APPE"))
        cmd_stor(s, arg, IS("APPE"));
    else if (IS("REST")) {
        unsigned long v = 0;
        const char *p = arg;
        while (*p >= '0' && *p <= '9' && v < (1UL << 58))
            v = v * 10 + (unsigned long)(*p++ - '0');
        if (p == arg || *p) {
            say(s, "501 REST needs a byte offset");
        } else {
            s->rest = (long)v;
            say(s, "350 Restarting at %ld", s->rest);
        }
    } else if (IS("SIZE") || IS("MDTM") || IS("DELE") || IS("MKD") || IS("RMD"))
        cmd_path_op(s, verb, arg);
    else if (IS("XMKD"))
        cmd_path_op(s, "MKD", arg);
    else if (IS("XRMD"))
        cmd_path_op(s, "RMD", arg);
    else if (IS("RNFR") || IS("RNTO"))
        cmd_rename(s, arg, IS("RNTO"));
    else if (IS("ABOR"))
        say(s, "225 No transfer to abort");
    else if (IS("SITE")) {
        if (strncmp(arg, "CHMOD", 5) == 0 || strncmp(arg, "chmod", 5) == 0)
            say(s, "200 Permissions are not kept on this drive");
        else
            say(s, "214 SITE CHMOD only");
    } else
        say(s, "502 %s is not implemented", verb);
#undef IS
    return true;
}

static void *session(void *arg)
{
    struct sess *s = arg;
    unsigned short port;
    fv_timeouts(s->ctl, 900); /* a client quiet for 15 minutes is dropped */
    fv_sockname(s->ctl, &s->local, &port);
    strlcpy(s->cwd, fv_home(), sizeof(s->cwd));
    fv_log("ftp %u: %u.%u.%u.%u connected; starting in %s", s->id, s->peer >> 24, (s->peer >> 16) & 255,
           (s->peer >> 8) & 255, s->peer & 255, s->cwd);
    say(s, "220 ghostldr FTP server: any user and password");
    while (read_line(s) && dispatch(s))
        ;
    fv_sclose(s->ctl);
    fv_log("ftp %u: disconnected", s->id);
    __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE);
    return NULL;
}

static void *listener(void *arg)
{
    unsigned fails = 0;
    while (!g_stop) {
        unsigned a = 0;
        unsigned short p = 0;
        fv_idle();
        int c = fv_accept(g_lfd, 500, &a, &p);
        if (c == -FV_ETIMEDOUT || g_stop) {
            if (c >= 0)
                fv_sclose(c);
            continue;
        }
        if (c < 0) {
            if (fails++ % 100 == 0)
                fv_log("ftp: accept: %s (errno %d)", err_text(c), -c);
            fv_sleep_ms(fails > 10 ? 1000 : 50);
            continue;
        }
        fails = 0;
        struct sess *s = NULL;
        for (unsigned i = 0; i < FTP_SESSIONS && !s; i++)
            if (!__atomic_exchange_n(&g_sess[i]->busy, 1, __ATOMIC_ACQUIRE))
                s = g_sess[i];
        if (!s) {
            static const char k_full[] = "421 Too many clients at once; try again later\r\n";
            fv_send(c, k_full, sizeof(k_full) - 1);
            fv_sclose(c);
            continue;
        }
        s->ctl = c;
        s->data = -1;
        s->pasv = false;
        s->peer = a;
        s->act = false;
        s->rest = 0;
        s->rnfr[0] = 0;
        s->in_len = 0;
        s->id = ++g_ids;
        if (fv_thread(session, s, "ghostldr-ftp") != 0) {
            fv_log("ftp: no thread for a client");
            fv_sclose(c);
            __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE);
        }
    }
    __atomic_store_n(&g_listening, 0, __ATOMIC_RELEASE);
    return arg;
}

/* the listening socket (ftp_sys.c, after rest mode): -1 with no server */
int ftp_listen_fd(void) { return g_lfd; }

/* After rest mode, when the listening sockets stopped working (ftp_sys.c):
 * each made again, the server's on its port and every idle slot's passive
 * one.  The listener thread's accept on the old socket fails as it closes,
 * and it carries on with the new one.  0, or -errno. */
int ftp_relisten(void)
{
    if (g_lfd < 0)
        return -FV_EBADF;
    fv_sclose(g_lfd); /* the port free first */
    int fd = g_lfd = fv_listen(0, g_port, 8); /* -1 on failure: its number may be the title's next */
    if (fd < 0) {
        g_lfd = -1;
        fv_log("ftp: TCP %u can't be listened on again: %s (errno %d)", g_port, err_text(fd), -fd);
        return fd;
    }
    unsigned n = 0;
    for (unsigned i = 0; i < FTP_SESSIONS; i++) {
        struct sess *s = g_sess[i];
        if (!s || __atomic_exchange_n(&s->busy, 1, __ATOMIC_ACQUIRE))
            continue; /* a client's: its connection went with rest mode, and its slot is done with soon */
        unsigned a;
        int dl = fv_listen(0, 0, 4);
        unsigned short port = 0;
        if (dl >= 0 && fv_sockname(dl, &a, &port) == 0) {
            fv_sclose(s->dl);
            s->dl = dl, s->dport = port, n++;
        } else if (dl >= 0) {
            fv_sclose(dl);
        }
        __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE);
    }
    fv_log("ftp: listening on TCP %u again, %u passive ports made again", g_port, n);
    return 0;
}

static void free_sessions(void)
{
    for (unsigned i = 0; i < FTP_SESSIONS; i++)
        if (g_sess[i]) {
            if (g_sess[i]->dl >= 0)
                fv_sclose(g_sess[i]->dl);
            fv_free(g_sess[i]->buf, FTP_BUF);
            fv_free(g_sess[i], sizeof(struct sess));
            g_sess[i] = NULL;
        }
}

int ftp_start(unsigned short port)
{
    if (g_lfd >= 0)
        return -FV_EBUSY;
    for (unsigned i = 0; i < FTP_SESSIONS; i++) {
        struct sess *s = g_sess[i] = fv_alloc(sizeof(struct sess));
        if (s)
            s->dl = -1, s->buf = fv_alloc(FTP_BUF);
        if (!s || !s->buf) {
            fv_log("ftp: no memory for %u clients", FTP_SESSIONS);
            free_sessions();
            return -FV_ENOMEM;
        }
        unsigned a;
        int rc = s->dl = fv_listen(0, 0, 4);
        if (rc >= 0)
            rc = fv_sockname(s->dl, &a, &s->dport);
        if (rc < 0) {
            fv_log("ftp: no passive data port: %s (errno %d)", err_text(rc), -rc);
            free_sessions();
            return rc;
        }
    }
    int fd = fv_listen(0, port, 8);
    if (fd < 0) {
        fv_log("ftp: TCP %u: %s (errno %d)%s", port, err_text(fd), -fd,
               fd == -FV_EADDRINUSE ? ": another FTP server has it; run.py --ftp-port picks another port" : "");
        free_sessions();
        return fd;
    }
    g_lfd = fd;
    g_port = port;
    g_stop = 0, g_listening = 1;
    int rc = fv_thread(listener, NULL, "ghostldr-ftpd");
    if (rc != 0) {
        g_listening = 0;
        fv_log("ftp: no thread for the server");
        fv_sclose(fd);
        g_lfd = -1;
        free_sessions();
        return rc;
    }
    fv_log("ftp: listening on TCP %u (%u clients at once, %u KiB each)", port, FTP_SESSIONS, FTP_BUF >> 10);
    return 0;
}

/* The server goes: no new clients, every client's connections shut down
 * (a transfer's too), then its threads and memory.  It waits for them, at
 * most max_ms: 0, or -FV_EBUSY when one is still there (the caller must not
 * take its code away then).  ftp_start may start it again. */
int ftp_stop(unsigned max_ms)
{
    if (g_lfd < 0)
        return 0;
    g_stop = 1;
    for (unsigned i = 0; i < FTP_SESSIONS; i++) {
        struct sess *s = g_sess[i];
        if (s && __atomic_load_n(&s->busy, __ATOMIC_ACQUIRE)) {
            int d = s->data;
            if (d >= 0)
                fv_shutdown(d);
            fv_shutdown(s->ctl);
        }
    }
    unsigned waited = 0;
    for (;; waited += 20) {
        bool busy = __atomic_load_n(&g_listening, __ATOMIC_ACQUIRE);
        for (unsigned i = 0; i < FTP_SESSIONS && !busy; i++)
            busy = g_sess[i] && __atomic_load_n(&g_sess[i]->busy, __ATOMIC_ACQUIRE);
        if (!busy)
            break;
        if (waited >= max_ms) {
            fv_log("ftp: a client's thread is still there after %u ms", waited);
            return -FV_EBUSY;
        }
        fv_sleep_ms(20);
    }
    fv_sclose(g_lfd);
    g_lfd = -1;
    free_sessions();
    fv_log("ftp: stopped (TCP %u)", g_port);
    return 0;
}

int ftp_running(void) { return g_lfd >= 0; }
