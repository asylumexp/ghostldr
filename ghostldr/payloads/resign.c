/*
 * resign: the console side of `ghostldr.py resign` (docs/save-resign.md), an
 * ELF payload for ghostldr's own ELF loader (docs/elfldr.md: SWRR userland,
 * no kernel exploit).
 *
 * It listens on TCP 9031 (or the next free port up to 9040; the first line it
 * prints says which), takes one save from ghostldr/host/resign.py, keeps it
 * in SWRR's /temp0 and hands it to the extension's save writer
 * (ghostldr_save_race_write, ext/src/save_race.c), which makes the user's own
 * save of that game hold exactly those files.  Before anything changes, the
 * save's present files are copied aside and sent back to the computer (an
 * undo).  Or, in check mode, it compares the save with them (after SWRR
 * restarted).  The result goes back on the same connection and to stdout.
 * The staging copies are removed afterwards.
 *
 * Stream (little endian): "GLDRRSGN", u32 version 1, u32 mode (1 write, 2
 * check), s32 user (-1: the user who started SWRR), char title[16],
 * char dir[32], char fp[80] (64 hex characters or empty), u32 entries; then
 * per entry u8 kind (1 file, 2 folder), u16 path length, the path (relative,
 * '/'-separated), and for a file a u64 size and its bytes.  Back: text lines;
 * after a write, "BACKUP <n>\n" and n entries in the same form, then the
 * "RESULT ..." line.
 *
 * MIT license, like the rest of ghostldr.
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <stdarg.h>
#include <sys/time.h>

#define PORT0 9031
#define PORTS 10
#define STAGE "/temp0/ghostldr-resign"
#define SRC STAGE "/src"
#define BAK STAGE "/backup"
#define MAX_ENTRIES 512

/* ext/include/savedata_hooks.h: struct save_race_result (256 bytes) */
struct save_race_result {
    int32_t rc, mount_rc;
    uint32_t slot, written, created, unchanged, deleted, dirs_made, verify_bad, extra;
    uint64_t bytes;
    _Bool committed, needs_exit;
    uint8_t pad[6];
    char why[200];
};
_Static_assert(sizeof(struct save_race_result) == 256, "shared with the extension");

/* bound by the ELF loader to ghostldr's extension (save_race.c) */
extern int ghostldr_save_race_write(int user, const char *tid, const char *dir, const char *src, const char *fp,
                                    const char *backup, struct save_race_result *r);
extern int ghostldr_save_race_check(int user, const char *tid, const char *dir, const char *src, const char *fp,
                                    struct save_race_result *r);
extern int sceUserServiceGetInitialUser(int *);

static int g_client = -1;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char line[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    __builtin_va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    line[n] = 0;
    fputs(line, stdout);
    fflush(stdout);
    if (g_client >= 0)
        send(g_client, line, (size_t)n, 0);
}

static int recv_all(int fd, void *p, size_t n)
{
    uint8_t *b = p;
    while (n) {
        ssize_t r = recv(fd, b, n, 0);
        if (r <= 0)
            return -1;
        b += r, n -= (size_t)r;
    }
    return 0;
}

/* a path the stream may name: relative, no "." or ".." parts, no sce_sys at the top */
static int path_ok(const char *p)
{
    if (!p[0] || p[0] == '/' || strncmp(p, "sce_sys", 7) == 0 && (p[7] == 0 || p[7] == '/'))
        return 0;
    for (const char *c = p; *c;) {
        const char *e = strchr(c, '/');
        size_t len = e ? (size_t)(e - c) : strlen(c);
        if (len == 0 || (len == 1 && c[0] == '.') || (len == 2 && c[0] == '.' && c[1] == '.'))
            return 0;
        c += len + (e ? 1 : 0);
    }
    return 1;
}

static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    struct dirent *e;
    char sub[512];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        if (e->d_type == DT_DIR)
            remove_tree(sub);
        else
            unlink(sub);
    }
    closedir(d);
    rmdir(path);
}

static uint8_t g_buf[1 << 20];

/* the stream's entries into SRC; 0 or -1 (said) */
static int receive(int c, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t kind;
        uint16_t len;
        char rel[400], path[512];
        if (recv_all(c, &kind, 1) || recv_all(c, &len, 2) || len >= sizeof(rel) || recv_all(c, rel, len)) {
            say("resign: the stream broke off at entry %u", i);
            return -1;
        }
        rel[len] = 0;
        if (!path_ok(rel)) {
            say("resign: refused path '%s'", rel);
            return -1;
        }
        snprintf(path, sizeof(path), "%s/%s", SRC, rel);
        if (kind == 2) {
            if (mkdir(path, 0777) != 0 && errno != EEXIST) {
                say("resign: mkdir %s: errno %d", rel, errno);
                return -1;
            }
            continue;
        }
        uint64_t size;
        if (kind != 1 || recv_all(c, &size, 8)) {
            say("resign: bad entry %u", i);
            return -1;
        }
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            say("resign: create %s: errno %d", rel, errno);
            return -1;
        }
        for (uint64_t done = 0; done < size;) {
            size_t want = size - done < sizeof(g_buf) ? (size_t)(size - done) : sizeof(g_buf);
            if (recv_all(c, g_buf, want) || write(fd, g_buf, want) != (ssize_t)want) {
                close(fd);
                say("resign: %s cut short at %llu of %llu bytes", rel, (unsigned long long)done,
                    (unsigned long long)size);
                return -1;
            }
            done += want;
        }
        close(fd);
    }
    return 0;
}

static int send_all(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        ssize_t w = send(fd, b, n, 0);
        if (w <= 0)
            return -1;
        b += w, n -= (size_t)w;
    }
    return 0;
}

/* the backup's entries below root/rel, counted (out NULL) or sent */
static int send_tree(int c, const char *root, const char *rel, uint32_t *count)
{
    char path[512];
    snprintf(path, sizeof(path), rel[0] ? "%s/%s" : "%s", root, rel);
    DIR *d = opendir(path);
    if (!d)
        return 0;
    struct dirent *e;
    int rc = 0;
    while (rc == 0 && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char sub[400];
        if (rel[0])
            snprintf(sub, sizeof(sub), "%s/%s", rel, e->d_name);
        else
            snprintf(sub, sizeof(sub), "%s", e->d_name);
        uint16_t len = (uint16_t)strlen(sub);
        uint8_t kind = e->d_type == DT_DIR ? 2 : 1;
        (*count)++;
        if (c >= 0) {
            if (send_all(c, &kind, 1) || send_all(c, &len, 2) || send_all(c, sub, len))
                rc = -1;
            if (rc == 0 && kind == 1) {
                snprintf(path, sizeof(path), "%s/%s", root, sub);
                int fd = open(path, O_RDONLY);
                /* the size by seeking: the toolchain's struct stat is not the layout SWRR's fstat fills */
                off_t end = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
                uint64_t size = end > 0 && lseek(fd, 0, SEEK_SET) == 0 ? (uint64_t)end : 0;
                rc = send_all(c, &size, 8);
                for (uint64_t done = 0; rc == 0 && done < size;) {
                    ssize_t n = read(fd, g_buf, sizeof(g_buf));
                    if (n <= 0)
                        rc = -1;
                    else
                        rc = send_all(c, g_buf, (size_t)n), done += (uint64_t)n;
                }
                if (fd >= 0)
                    close(fd);
            }
        }
        if (rc == 0 && kind == 2)
            rc = send_tree(c, root, sub, count);
    }
    closedir(d);
    return rc;
}

static int serve(int c)
{
    struct {
        char magic[8];
        uint32_t version, mode;
        int32_t user;
        char title[16], dir[32], fp[80];
        uint32_t entries;
    } __attribute__((packed)) h;
    if (recv_all(c, &h, sizeof(h)) || memcmp(h.magic, "GLDRRSGN", 8) || h.version != 1 || h.entries > MAX_ENTRIES ||
        (h.mode != 1 && h.mode != 2)) {
        say("resign: not a resign stream from ghostldr/host/resign.py (or another version)");
        return 1;
    }
    h.title[15] = h.dir[31] = h.fp[79] = 0;
    int user = h.user;
    if (user < 0)
        sceUserServiceGetInitialUser(&user);
    remove_tree(STAGE);
    mkdir(STAGE, 0777);
    if (mkdir(SRC, 0777) != 0) {
        say("resign: can't make %s (errno %d)", SRC, errno);
        return 1;
    }
    say("resign: receiving %u entries for %s/%s (user %08x)", h.entries, h.title, h.dir, user);
    if (receive(c, h.entries) != 0) {
        remove_tree(STAGE);
        return 1;
    }
    struct save_race_result r;
    memset(&r, 0, sizeof(r));
    int rc = h.mode == 1 ? ghostldr_save_race_write(user, h.title, h.dir, SRC, h.fp[0] ? h.fp : NULL, BAK, &r)
                         : ghostldr_save_race_check(user, h.title, h.dir, SRC, h.fp, &r);
    if (h.mode == 1) { /* the save as it was, back to the computer: the undo */
        uint32_t n = 0;
        send_tree(-1, BAK, "", &n);
        char line[32];
        int len = snprintf(line, sizeof(line), "BACKUP %u\n", n);
        printf("resign: sending the save's earlier files back (%u entries)\n", n);
        if (send_all(c, line, (size_t)len) == 0) {
            n = 0;
            send_tree(c, BAK, "", &n);
        }
    }
    remove_tree(STAGE);
    say("RESULT rc=%08x mount=%08x slot=%u written=%u created=%u unchanged=%u deleted=%u dirs=%u bytes=%llu "
        "bad=%u extra=%u committed=%d needs_exit=%d",
        (unsigned)rc, (unsigned)r.mount_rc, r.slot, r.written, r.created, r.unchanged, r.deleted, r.dirs_made,
        (unsigned long long)r.bytes, r.verify_bad, r.extra, r.committed, r.needs_exit);
    if (r.why[0])
        say("WHY %s", r.why);
    return rc == 0 ? 0 : 1;
}

int main(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        printf("resign: socket() failed: errno %d\n", errno);
        return 1;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    int port = 0;
    for (int p = PORT0; p < PORT0 + PORTS && !port; p++) {
        a.sin_port = htons((uint16_t)p);
        if (bind(s, (struct sockaddr *)&a, sizeof(a)) == 0)
            port = p;
    }
    if (!port || listen(s, 1) != 0) {
        printf("resign: no free port in %d-%d\n", PORT0, PORT0 + PORTS - 1);
        close(s);
        return 1;
    }
    printf("resign: listening on TCP %d\n", port);
    fflush(stdout);
    struct timeval tv = {120, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int c = accept(s, NULL, NULL);
    close(s);
    if (c < 0) {
        printf("resign: nobody connected in 120 s\n");
        return 1;
    }
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    g_client = c;
    int rc = serve(c);
    g_client = -1;
    close(c);
    return rc;
}
