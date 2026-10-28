/*
 * Runs the core's bxplan.c (and elf.c to parse modules) in a host process,
 * built for this machine, so test_bigexec.py can compare it with
 * tools/exec_fit.py on the same inputs:
 *
 *   bx_host plan FILE... [--exec LO:HI,...]   modules in load order (the
 *       executable is eboot.bin): the arena check in the EE buffer of a 13.60
 *       PS5, then the code map and cover with its 32/16/8/8 MiB objects
 *   bx_host runs SEG VADDR TEXT EH GOTLO GOTHI [POINT...]   code map of raw
 *       segment bytes
 *   bx_host cover CASES   one case per line: "R lo hi ... F lo hi ... O size
 *       ... K keep" (hex), one answer per line
 *   bx_host fresh CASES   the same lines (O and K ignored): bx_fresh's
 *       objects, one answer per line
 */
#include "bxplan.h"

int printf(const char *f, ...);
int snprintf(char *buf, unsigned long size, const char *f, ...);
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void *fopen(const char *path, const char *mode);
int fseek(void *f, long off, int whence);
long ftell(void *f);
size_t fread(void *p, size_t size, size_t n, void *f);
int fclose(void *f);
char *fgets(char *s, int n, void *f);
unsigned long long strtoull(const char *s, char **end, int base);

struct kfuncs K;
struct boot_ctx *g_ctx;
struct module *g_modules, *g_main_mod;

void logf(const char *f, ...)
{
    (void)f;
}
void profile_bind(const u8 *fp) { (void)fp; }
const char *strrchr_c(const char *s, int c)
{
    const char *r = NULL;
    for (; *s; s++)
        if (*s == c)
            r = s;
    return r;
}

#define EE_LO (0x900a1c000UL + JIT_BUF_HEADER)
#define EE_HI 0x902a1c000UL

static u8 *slurp(const char *path, u64 *size)
{
    void *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, 2);
    *size = (u64)ftell(f);
    fseek(f, 0, 0);
    u8 *b = malloc(*size + 16);
    if (b && fread(b, 1, *size, f) != *size)
        b = NULL;
    fclose(f);
    return b;
}

/* signed hex, as Python prints it */
static const char *sx(s64 v)
{
    static char buf[4][24];
    static u32 k;
    char *b = buf[k++ & 3];
    if (v < 0)
        snprintf(b, 24, "-%lx", (u64)-v);
    else
        snprintf(b, 24, "%lx", (u64)v);
    return b;
}

static void print_runs(const struct bx_range *r, u32 n)
{
    for (u32 i = 0; i < n; i++)
        printf("run %lx %lx\n", (u64)r[i].lo, (u64)r[i].hi);
}

static void print_layout(int rc, const struct bx_layout *l)
{
    if (rc) {
        printf("none\n");
        return;
    }
    for (u32 v = 0; v < l->nviews; v++)
        printf("view %u %s\n", l->view[v].obj, sx(l->view[v].off));
    printf("used %lx\n", l->used);
}

static const u64 k_obj[] = {32UL << 20, 16UL << 20, 8UL << 20, 8UL << 20};

static int plan(int argc, char **argv)
{
    static u32 exec[64];
    u32 nexec = 0;
    struct module **tail = &g_modules;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--exec") && i + 1 < argc) {
            for (char *p = argv[++i]; *p && nexec < 62;) {
                exec[nexec++] = (u32)strtoull(p, &p, 16);
                if (*p == ':' || *p == ',')
                    p++;
            }
            continue;
        }
        u64 size;
        u8 *buf = slurp(argv[i], &size);
        struct module *m = calloc(1, sizeof(*m));
        const char *slash = strrchr_c(argv[i], '/');
        m->name = slash ? slash + 1 : argv[i];
        m->is_main = !strcmp(m->name, "eboot.bin");
        if (!buf || module_parse(m, buf, size) != 0) {
            printf("cannot parse %s\n", argv[i]);
            return 2;
        }
        *tail = m, tail = &m->next;
    }
    bool holds = bx_arena_holds(g_modules, EE_LO, EE_HI);
    printf("holds %d\n", holds);
    if (holds)
        return 0;
    u64 keep;
    struct module *big = bx_pick(g_modules, &keep);
    struct bx_code_in in;
    struct bx_code_stats st;
    struct bx_range runs[BX_MAX_RUNS], forbid[BX_MAX_FORBID];
    bx_module_in(big, exec, nexec / 2, &in);
    u32 n = bx_code_runs(&in, runs, &st), nf = bx_data_ranges(big, forbid);
    printf("big %s keep %lx\n", big->name, keep);
    printf("evidence fdes %u bytes %lx prologues %u plt %u points %u eh %d\n", st.fdes, st.fde_bytes, st.prologues,
           st.plt, st.points, st.eh);
    print_runs(runs, n);
    for (u32 i = 0; i < nf; i++)
        printf("forbid %lx %lx\n", (u64)forbid[i].lo, (u64)forbid[i].hi);
    struct bx_layout l;
    print_layout(n ? bx_cover(runs, n, forbid, nf, k_obj, 4, keep, &l) : -1, &l);
    return 0;
}

static int runs(int argc, char **argv)
{
    u64 size;
    struct bx_code_in in = {0};
    struct bx_code_stats st;
    struct bx_range r[BX_MAX_RUNS];
    in.seg = slurp(argv[0], &size);
    in.seg_filesz = size;
    in.seg_vaddr = strtoull(argv[1], NULL, 16);
    in.text_size = strtoull(argv[2], NULL, 16);
    in.eh_hdr = strtoull(argv[3], NULL, 16);
    in.got_lo = strtoull(argv[4], NULL, 16);
    in.got_hi = strtoull(argv[5], NULL, 16);
    for (int i = 6; i < argc && in.npoints < 3; i++)
        in.point[in.npoints++] = strtoull(argv[i], NULL, 16);
    u32 n = bx_code_runs(&in, r, &st);
    printf("evidence fdes %u bytes %lx prologues %u plt %u points %u eh %d\n", st.fdes, st.fde_bytes, st.prologues,
           st.plt, st.points, st.eh);
    print_runs(r, n);
    return 0;
}

/* "R lo hi ... F lo hi ... O size ... K keep" */
static int cover(const char *path, bool fresh)
{
    char line[2048];
    void *in = fopen(path, "r");
    while (in && fgets(line, sizeof(line), in)) {
        struct bx_range r[16], f[16];
        u64 obj[16], keep = 0;
        u32 nr = 0, nf = 0, no = 0;
        char what = 0;
        for (char *p = line; *p;) {
            while (*p == ' ' || *p == '\n')
                p++;
            if (!*p)
                break;
            if (*p >= 'F' && *p <= 'R') {
                what = *p++;
                continue;
            }
            u64 v = strtoull(p, &p, 16);
            if (what == 'R' && nr < 16)
                ((s64 *)r)[nr++] = (s64)v;
            else if (what == 'F' && nf < 16)
                ((s64 *)f)[nf++] = (s64)v;
            else if (what == 'O' && no < 16)
                obj[no++] = v;
            else if (what == 'K')
                keep = v;
        }
        struct bx_layout l;
        u64 size[BX_MAX_VIEWS];
        int rc = fresh ? bx_fresh(r, nr / 2, f, nf / 2, size, &l) : bx_cover(r, nr / 2, f, nf / 2, obj, no, keep, &l);
        if (rc) {
            printf("none\n");
            continue;
        }
        for (u32 v = 0; v < l.nviews; v++)
            if (fresh)
                printf("%u %s %lx ", l.view[v].obj, sx(l.view[v].off), size[v]);
            else
                printf("%u %s ", l.view[v].obj, sx(l.view[v].off));
        printf("used %lx\n", l.used);
    }
    return in ? 0 : 2;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "plan"))
        return plan(argc - 2, argv + 2);
    if (argc > 7 && !strcmp(argv[1], "runs"))
        return runs(argc - 2, argv + 2);
    if (argc > 2 && !strcmp(argv[1], "cover"))
        return cover(argv[2], false);
    if (argc > 2 && !strcmp(argv[1], "fresh"))
        return cover(argv[2], true);
    printf("usage: bx_host plan FILE... | runs SEG VADDR TEXT EH GOTLO GOTHI [POINT...] | cover CASES | fresh CASES\n");
    return 2;
}
