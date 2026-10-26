/*
 * The ELF loader's el_* calls for the host tests (test_elfldr.py): payloads
 * are parsed and checked by the real payload.c (and elf.c), but nothing
 * runs.  A thread acts out the script the test put in the file instead:
 *
 *     ELFLDR-TEST:sleep=MS;out=TEXT;code=N;crash=1;
 *
 * it writes TEXT (with "|" for newlines) to the connection, waits MS and
 * finishes with exit code N, or as a crash.  Two payloads at once, as if
 * there were two JIT objects.  Freestanding like elfldr.c: ghost.h, and the
 * environment of ftp.h.
 */
#include "ghost.h"
#include "ftp.h"
#include "elfldr.h"

#define SLOTS 2

struct el_payload {
    struct module m;
    struct pl_info info;
    unsigned id;
    int conn, slot, code, crash;
    unsigned sleep;
    char out[256];
    volatile int done;
    long t0;
};

static struct el_payload *g_slot[SLOTS];

int pl_parse_sce(struct module *m, u8 *file, u64 size) { return module_parse(m, file, size); }

/* elf.c's needs */
long write(int fd, const void *buf, unsigned long n);
void profile_bind(const u8 *fp) {}
void logf(const char *f, ...)
{
    char line[1200];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(line, sizeof(line) - 1, f, ap);
    __builtin_va_end(ap);
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    write(2, line, (unsigned long)n);
}

int el_init(char *why, unsigned size) { return 0; }
void el_fini(void) {}

static const unsigned char *find(const unsigned char *b, unsigned long len, const char *what)
{
    unsigned long n = strlen(what);
    for (unsigned long i = 0; i + n <= len; i++)
        if (memcmp(b + i, what, n) == 0)
            return b + i + n;
    return NULL;
}

static long num(const char *s)
{
    long v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

static void script(struct el_payload *p, const unsigned char *b, unsigned long len)
{
    const unsigned char *s = find(b, len, "ELFLDR-TEST:");
    if (!s)
        return;
    char text[400];
    unsigned long n = 0;
    while (s + n < b + len && s[n] && s[n] != '\n' && n < sizeof(text) - 1)
        text[n] = (char)s[n], n++;
    text[n] = 0;
    for (char *k = text; *k;) {
        char *v = k;
        while (*v && *v != '=' && *v != ';')
            v++;
        char *end = v;
        while (*end && *end != ';')
            end++;
        char save = *end;
        *end = 0;
        if (*v == '=') {
            *v++ = 0;
            if (strcmp(k, "sleep") == 0)
                p->sleep = (unsigned)num(v);
            else if (strcmp(k, "code") == 0)
                p->code = (int)num(v);
            else if (strcmp(k, "crash") == 0)
                p->crash = (int)num(v);
            else if (strcmp(k, "out") == 0) {
                strlcpy(p->out, v, sizeof(p->out));
                for (char *c = p->out; *c; c++)
                    if (*c == '|')
                        *c = '\n';
            }
        }
        k = save ? end + 1 : end;
    }
}

int el_load(const unsigned char *buf, unsigned long len, unsigned id, int conn, struct el_payload **out, char *info,
            unsigned size)
{
    struct el_payload *p = fv_alloc(sizeof(*p));
    u8 *file = fv_alloc(len);
    memcpy(file, buf, len);
    p->m.name = "payload";
    p->m.payload = true;
    p->id = id, p->conn = conn, p->slot = -1;
    if (pl_parse(&p->m, file, len, &p->info, info, size) != 0) {
        fv_free(file, len);
        fv_free(p, sizeof(*p));
        return -1;
    }
    for (int i = 0; i < SLOTS && p->slot < 0; i++)
        if (!g_slot[i])
            p->slot = i;
    if (p->slot < 0) {
        fmt(info, size, "no JIT object free for its %lu KiB of code: %u payloads hold the emulator's JIT objects "
                        "(one each)", p->info.exec_end >> 10, SLOTS);
        fv_free(file, len);
        fv_free(p, sizeof(*p));
        return -1;
    }
    g_slot[p->slot] = p;
    script(p, buf, len);
    fv_free(file, len);
    fmt(info, size, "payload %u: %s %s, %lu KiB of code, %lu KiB of data", id, p->info.sce ? "Orbis" : "ELF",
        p->info.abi == PL_FREE ? "(freestanding)" : "(OpenOrbis/PS4 ABI)", p->info.exec_end >> 10,
        (p->info.image_size - p->info.exec_end) >> 10);
    *out = p;
    return 0;
}

static void *run(void *arg)
{
    struct el_payload *p = arg;
    if (p->out[0])
        fv_send(p->conn, p->out, strlen(p->out));
    fv_sleep_ms(p->sleep);
    p->done = 1;
    return NULL;
}

int el_run(struct el_payload *p, char *why, unsigned size)
{
    p->t0 = fv_clock_ms();
    if (fv_thread(run, p, "payload") != 0) {
        fmt(why, size, "no thread");
        g_slot[p->slot] = NULL;
        return -1;
    }
    return 0;
}

int el_poll(struct el_payload *p, char *line, unsigned size)
{
    if (!p->done)
        return 0;
    if (p->crash)
        fmt(line, size, "payload %u crashed: signal 11", p->id);
    else
        fmt(line, size, "payload %u exited with code %d", p->id, p->code);
    g_slot[p->slot] = NULL;
    fv_free(p, sizeof(*p));
    return 1;
}

void el_describe(struct el_payload *p, char *line, unsigned size)
{
    fmt(line, size, "payload %u: running for %ld ms", p->id, fv_clock_ms() - p->t0);
}
