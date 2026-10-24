/*
 * Runs the core's ghostfs.c in a host process against a fake of
 * SWRR's save data: containers whose files test_profiles.py wrote with
 * gfs_pack.py and gfs_catalog.py, mounted at /savedataN by a fake
 * sceSaveDataMount.  It boots the catalogue's title (or the one asked for)
 * and reads a file of it.
 *
 *     ghostfs_host TITLE|- FILE APP0_PATH DIR/NAME=HOSTFILE...
 */
#include "ghost.h"

long write(int fd, const void *buf, unsigned long n);
int open(const char *path, int flags, ...);
long read(int fd, void *buf, unsigned long n);
char *getenv(const char *name);
int close(int fd);
int vsnprintf(char *buf, unsigned long size, const char *fmt, __builtin_va_list ap);
void *calloc(unsigned long n, unsigned long size);

struct kfuncs K;
struct boot_ctx *g_ctx;
/* SWRR: sceSaveDataMount at its import slot (g_eboot_slot below) */
static const struct host_game g_swrr = {.name = "SWRR", .save_mount = 0x3893F0};
const struct host_game *g_hostgame = &g_swrr;
const char *profile_title(void) { return ""; } /* no profile: dumps outside ghostfs are not tested here */

int vfmt(char *buf, size_t size, const char *f, __builtin_va_list ap) { return vsnprintf(buf, size, f, ap); }

int fmt(char *buf, size_t size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(buf, size, f, ap);
    __builtin_va_end(ap);
    return n;
}

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
    write(1, line, n);
}

const char *strrchr_c(const char *s, int c)
{
    const char *r = NULL;
    for (; *s; s++)
        if (*s == c)
            r = s;
    return r;
}

void *page_alloc(size_t size) { return calloc(1, size ? size : 1); }

/* ---- profile.c, as far as ghostfs uses it ---- */
static const char *g_want;
static char g_selected[17] = "(none)";
const char *profile_requested(void) { return g_want; }
void profile_select(const char *title) { strlcpy(g_selected, title, sizeof(g_selected)); }

/* ---- the fake save data ---- */
struct file {
    char dir[32], name[32];
    u8 *data;
    u64 size;
};
static struct file g_files_fake[64];
static u32 g_nfiles;
static char g_mounted[16][32]; /* /savedataN -> container */
static u32 g_mounts;

static int fake_mount(void *params, void *result)
{
    const char *dir = *(const char **)((u8 *)params + 0x10);
    bool found = false;
    for (u32 i = 0; i < g_nfiles; i++)
        found |= strcmp(g_files_fake[i].dir, dir) == 0;
    if (!found)
        return (int)0x809F0008;
    for (u32 n = 1; n < 16; n++)
        if (!g_mounted[n][0]) {
            strlcpy(g_mounted[n], dir, sizeof(g_mounted[n]));
            fmt(result, 16, "/savedata%u", n);
            g_mounts++;
            return 0;
        }
    return (int)0x809F000C;
}

/* "/savedataN[/...]" -> N (16: not a mount point) */
static u32 mp_index(const char *path)
{
    u32 n = 0;
    if (strncmp(path, "/savedata", 9) != 0 || path[9] < '0' || path[9] > '9')
        return 16;
    for (const char *p = path + 9; *p >= '0' && *p <= '9'; p++)
        n = n * 10 + (u32)(*p - '0');
    return n < 16 ? n : 16;
}

static int fake_umount(const char *mp)
{
    u32 n = mp_index(mp);
    if (n < 16)
        g_mounted[n][0] = 0;
    return 0;
}

static struct file *lookup(const char *path)
{
    u32 n = mp_index(path);
    const char *slash = strrchr_c(path, '/');
    if (n >= 16 || !g_mounted[n][0] || slash == path)
        return NULL;
    for (u32 i = 0; i < g_nfiles; i++)
        if (strcmp(g_files_fake[i].dir, g_mounted[n]) == 0 && strcmp(g_files_fake[i].name, slash + 1) == 0)
            return &g_files_fake[i];
    return NULL;
}

static int fake_open(const char *path, int flags, int mode)
{
    if (strcmp(path, "/app0/eboot.bin") == 0)
        return 900; /* SWRR's own eboot: the placeholder fd ghostfs hands out */
    struct file *f = lookup(path);
    return f ? 100 + (int)(f - g_files_fake) : (int)0x80020002;
}

static int fake_close(int fd) { return 0; }

static s64 fake_pread(int fd, void *buf, u64 n, s64 off)
{
    if (fd < 100 || fd >= 100 + (int)g_nfiles)
        return (s64)(int)0x80020009;
    struct file *f = &g_files_fake[fd - 100];
    if ((u64)off >= f->size)
        return 0;
    if (n > f->size - (u64)off)
        n = f->size - (u64)off;
    memcpy(buf, f->data + off, n);
    return (s64)n;
}

static int fake_stat(const char *path, void *st)
{
    struct file *f = lookup(path);
    if (strrchr_c(path, '/') == path) { /* a mount point: exists while mounted */
        u32 n = mp_index(path);
        return n < 16 && g_mounted[n][0] ? 0 : (int)0x80020002;
    }
    if (!f)
        return (int)0x80020002;
    memset(st, 0, 0x78);
    *(u64 *)((u8 *)st + 0x48) = f->size;
    return 0;
}

static int fake_user(u32 *user)
{
    *user = 0x10000001;
    return 0;
}

static int fake_usleep(u32 us) { return 0; }

void *host_real(const char *name)
{
    if (strcmp(name, "sceKernelOpen") == 0)
        return fake_open;
    if (strcmp(name, "sceKernelClose") == 0)
        return fake_close;
    if (strcmp(name, "sceKernelPread") == 0)
        return fake_pread;
    if (strcmp(name, "sceKernelStat") == 0)
        return fake_stat;
    return NULL;
}

void *host_real_in(const char *modname, const char *name)
{
    if (strcmp(name, "sceSaveDataUmount") == 0)
        return fake_umount;
    if (strcmp(name, "sceUserServiceGetInitialUser") == 0)
        return fake_user;
    return NULL;
}

static u64 g_eboot_slot;
static u64 g_ctx_mem[64];

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    g_want = strcmp(argv[1], "-") ? argv[1] : NULL;
    for (int i = 3; i < argc; i++) {
        struct file *f = &g_files_fake[g_nfiles++];
        const char *slash = argv[i], *eq;
        while (*slash && *slash != '/')
            slash++;
        for (eq = slash; *eq && *eq != '='; eq++)
            ;
        if (!*slash || !*eq || slash - argv[i] >= 32 || eq - slash > 32)
            return 5;
        memcpy(f->dir, argv[i], (u32)(slash - argv[i]));
        memcpy(f->name, slash + 1, (u32)(eq - slash - 1));
        int fd = open(eq + 1, 0);
        f->data = calloc(1, 1 << 20);
        long n = fd >= 0 ? read(fd, f->data, 1 << 20) : -1;
        if (fd >= 0)
            close(fd);
        f->size = n > 0 ? (u64)n : 0;
    }
    g_ctx = (struct boot_ctx *)g_ctx_mem;
    g_eboot_slot = (u64)fake_mount;
    g_ctx->eboot_base = (u64)&g_eboot_slot - 0x3893F0;
    K.usleep = fake_usleep;
    /* game selector: SEL_FLAG=1 is run.py --select; SEL_CHOICE what it picked ("": no selector) */
    const char *flag = getenv("SEL_FLAG"), *choice = getenv("SEL_CHOICE");
    if (flag && *flag == '1')
        g_ctx->flags |= CTX_FLAG_SELECT;
    int rc = gfs_init();
    if (rc == 1) { /* boot.c runs the selector here, then gfs_pick */
        logf("deferred to the game selector; it chose %s", choice && *choice ? choice : "nothing");
        rc = gfs_pick(choice && *choice ? choice : NULL);
    }
    logf("init %d, selected %s, titles %u, still mounted %s", rc, g_selected, gfs_titles(),
         g_mounted[1][0] ? g_mounted[1] : "-");
    if (rc != 0)
        return 0;
    int fd;
    if (gfs_open(argv[2], 0, &fd) != 0 || fd < 0) {
        logf("open %s failed", argv[2]);
        return 0;
    }
    char buf[256];
    s64 got = gfs_pread(fd, buf, sizeof(buf) - 1, 0);
    buf[got > 0 ? got : 0] = 0;
    logf("read %ld: %s; save container %s", got, buf, gfs_save_name());
    return 0;
}
