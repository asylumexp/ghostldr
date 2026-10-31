/*
 * Logs and failures built for this machine (test_diag.py): the core's log.c
 * and the extension's logfile.c and failure.c, against a fake console whose
 * files are a host directory (ROOT): /temp0 is ROOT/temp0, a USB drive N is
 * ROOT/usbN mounted at /ghostldr_usb (N = 1) or /ghostldr_usbN, saved data
 * GLDRCONF is ROOT/GLDRCONF, notifications go to ROOT/notifications.
 * The system's message dialog, VideoOut and the overlay's page are
 * recorders.
 *
 *     diag_host ROOT log|names|off|nodrive|boot|crash|exit|hang
 *
 * Prints "ok ..." / "FAILED ..." lines and the log; exits 0 when every check
 * passed.
 */
#include "ghost_api.h"

long write(int fd, const void *buf, unsigned long n);
long read(int fd, void *buf, unsigned long n);
long pread(int fd, void *buf, unsigned long n, long off);
long lseek(int fd, long off, int whence);
int open(const char *path, int flags, ...);
int close(int fd);
int mkdir(const char *path, unsigned short mode);
int rename(const char *from, const char *to);
int unlink(const char *path);
int fsync(int fd);
int stat(const char *path, void *st);
int vsnprintf(char *buf, unsigned long size, const char *fmt, __builtin_va_list ap);
int printf(const char *fmt, ...);
void *calloc(unsigned long n, unsigned long size);
void free(void *p);
void exit(int code);
void *opendir(const char *path);
void *readdir(void *d);
int closedir(void *d);
int usleep(unsigned us);
int pthread_create(u64 *thr, const void *attr, void *(*fn)(void *), void *arg);
char *getenv(const char *name);

struct kfuncs K;
struct boot_ctx *g_ctx;
const struct ghost_api *G;
static struct boot_ctx g_ctx_v;
static struct module *g_mods, *g_main;
static u64 g_dmem;
static char g_root[512];
static int g_failed, g_drives = 1, g_fail_drive_writes;
static const char *g_mode;

int vfmt(char *buf, size_t size, const char *f, __builtin_va_list ap) { return vsnprintf(buf, size, f, ap); }
int fmt(char *buf, size_t size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(buf, size, f, ap);
    __builtin_va_end(ap);
    return n;
}

static void expect(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "ok" : "FAILED", what);
    g_failed |= !ok;
}

/* ---- the console's paths, in ROOT ---- */
static const char *host_path(const char *p, char *out)
{
    if (strncmp(p, "/ghostldr_usb", 13) == 0) {
        int n = p[13] >= '2' && p[13] <= '4' ? p[13] - '0' : 1;
        const char *rest = p + 13 + (n > 1);
        fmt(out, 600, "%s/usb%d%s", g_root, n, rest);
    } else if (strncmp(p, "/savedata0", 10) == 0) {
        fmt(out, 600, "%s/GLDRCONF%s", g_root, p + 10);
    } else if (strcmp(p, "/dev/notification0") == 0) {
        fmt(out, 600, "%s/notifications", g_root);
    } else {
        fmt(out, 600, "%s%s", g_root, p);
    }
    return out;
}

static bool drive_present(int n)
{
    char p[600], st[256];
    fmt(p, sizeof(p), "%s/usb%d", g_root, n);
    return n <= g_drives && stat(p, st) == 0;
}

/* fds: real ones, and directories (DIR *) above 1000 */
static void *g_dirs[16];
static int g_drive_fd = -1; /* the last fd opened on a drive (writes to it can be made to fail) */

#define F_O_APPEND 0x8
#define F_O_CREAT 0x200
#define F_O_TRUNC 0x400
#define F_O_DIRECTORY 0x20000

static int k_open(const char *path, int flags, int mode)
{
    char p[600];
    host_path(path, p);
    if (flags & F_O_DIRECTORY) {
        void *d = opendir(p);
        for (int i = 0; d && i < 16; i++)
            if (!g_dirs[i]) {
                g_dirs[i] = d;
                return 1000 + i;
            }
        return (int)0x80020002;
    }
    if (strncmp(path, "/ghostldr_usb", 13) == 0 && !drive_present(path[13] >= '2' ? path[13] - '0' : 1))
        return (int)0x80020002;
    int fd = open(p, (flags & 3) | (flags & F_O_APPEND ? 0x8 : 0) | (flags & F_O_CREAT ? 0x200 : 0) |
                         (flags & F_O_TRUNC ? 0x400 : 0), mode);
    if (fd >= 0 && strncmp(path, "/ghostldr_usb", 13) == 0 && (flags & 3))
        g_drive_fd = fd;
    return fd >= 0 ? fd : (int)0x80020002;
}
static int k_close(int fd)
{
    if (fd >= 1000) {
        closedir(g_dirs[fd - 1000]);
        g_dirs[fd - 1000] = NULL;
        return 0;
    }
    return close(fd);
}
static ssize_t k_write(int fd, const void *b, size_t n)
{
    if (g_fail_drive_writes && fd == g_drive_fd)
        return (ssize_t)0x80020005; /* EIO: the drive went away */
    return write(fd, b, n);
}
static ssize_t k_read(int fd, void *b, size_t n) { return read(fd, b, n); }
static ssize_t k_pread(int fd, void *b, size_t n, off_t o) { return pread(fd, b, n, o); }
static off_t k_lseek(int fd, off_t o, int w) { return lseek(fd, o, w); }
static int k_map_flex(void **addr, size_t len, int prot, int flags)
{
    *addr = calloc(1, len);
    return *addr ? 0 : -1;
}
static int k_usleep(u32 us);
static int k_getpid(void) { return 77; }
static size_t k_direct_size(void) { return 5ULL << 30; }
static int k_alloc_direct(off_t s, off_t e, size_t len, size_t align, int type, off_t *out)
{
    *out = (off_t)(u64)calloc(1, len);
    return *out ? 0 : -1;
}
static int k_map_direct(void **addr, size_t len, int prot, int flags, off_t phys, size_t align)
{
    *addr = (void *)phys;
    return 0;
}

/* two direct allocations of the title in one 4 MiB mapping, for the crash
 * screen's census of written memory (failure.c): the first written, from
 * 1 MiB of the second on */
static struct dm_usage g_usage[32] = {{.phys = 0x10000000, .len = 2 << 20, .mapped = 2 << 20, .from = 0x1234},
                                      {.phys = 0x10200000, .len = 2 << 20, .mapped = 2 << 20, .from = 0x5678, .type = 3}};
static u32 g_nusage = 2;
static u8 *g_heap;
static int k_virtual_query(const void *addr, int flags, void *info, size_t size)
{
    struct {
        u64 start, end, offset;
        s32 prot, type;
        u8 flags;
    } __attribute__((packed)) q = {(u64)g_heap, (u64)g_heap + (4 << 20), 0x10000000, 3, 0, 2};
    if (!g_heap || (u64)addr >= q.end)
        return -1;
    memcpy(info, &q, sizeof(q));
    return 0;
}

/* macOS dirent: ino u64, seekoff u64, reclen u16, namlen u16, type u8, name */
static int k_getdents(int fd, void *buf, int size)
{
    if (fd < 1000 || !g_dirs[fd - 1000])
        return -1;
    int off = 0;
    for (;;) {
        u8 *e = readdir(g_dirs[fd - 1000]);
        if (!e)
            break;
        const char *name = (const char *)e + 21;
        u16 len = (u16)strlen(name), rec = (u16)ALIGN_UP(8 + len + 1, 8);
        if (off + rec > size)
            break; /* (lost: the test's folders are small) */
        u8 *d = (u8 *)buf + off;
        *(u32 *)d = 1, *(u16 *)(d + 4) = rec, d[6] = e[20] == 4 ? 4 : 8, d[7] = (u8)len;
        memcpy(d + 8, name, len + 1);
        off += rec;
    }
    return off;
}
static int k_mkdir(const char *path, int mode)
{
    char p[600];
    return mkdir(host_path(path, p), 0777) == 0 ? 0 : (int)0x80020011;
}
static int k_rename(const char *a, const char *b)
{
    char p[600], q[600];
    return rename(host_path(a, p), host_path(b, q));
}
static int k_unlink(const char *a)
{
    char p[600];
    return unlink(host_path(a, p));
}
static int k_fsync(int fd) { return fsync(fd); }
static int vstat(const char *path, void *st)
{
    char p[600];
    u8 hs[256];
    if (strncmp(path, "/ghostldr_usb", 13) == 0 && !drive_present(path[13] >= '2' ? path[13] - '0' : 1))
        return -1;
    if (stat(host_path(path, p), hs) != 0)
        return -1;
    memset(st, 0, 0x78);
    *(u16 *)((u8 *)st + 8) = *(u16 *)(hs + 4); /* macOS st_mode is at 4 */
    return 0;
}

/* ---- the system's calls ---- */
static int g_polls, g_opened;
static char g_dialog[4096], g_page[4096];
static s32 md_status(void) { return g_opened ? (++g_polls > 3 ? 3 : 2) : 0; }
static s32 md_zero(void) { return 0; }
static s32 md_open(void *p)
{
    const char *text = *(const char **)(*(u8 **)((u8 *)p + 0x40) + 8);
    u8 *m = *(u8 **)((u8 *)p + 0x40);
    for (u32 i = 0x10; i < 0x40; i++)
        if (m[i])
            return (s32)0x80B8000A; /* PARAM_INVALID: reserved bytes must be zero */
    if (((u64)p & 7) || *(u32 *)((u8 *)p + 0x2C) != 0xC0D1A109u + (u32)(u64)p)
        return (s32)0x80B8000A;
    strlcat(g_dialog, text, sizeof(g_dialog));
    strlcat(g_dialog, "\n--\n", sizeof(g_dialog));
    g_opened = 1, g_polls = 0;
    return 0;
}
static s32 rtc(u16 *t)
{
    t[0] = 2026, t[1] = 10, t[2] = 28, t[3] = 21, t[4] = 15, t[5] = 0;
    return 0;
}
static s32 initial_user(s32 *u)
{
    *u = 0x10000001;
    return 0;
}
static s32 usb_list(u32 *ids, s32 *n)
{
    *n = 0;
    for (int i = 1; i <= 4; i++)
        if (drive_present(i))
            ids[(*n)++] = 0x1000 + (u32)i;
    return 0;
}
static s32 vo_reg(s32 h, s32 i, void **a, s32 n, void *attr) { return 0; }
static u32 g_flips;
static s32 vo_flip(s32 h, s32 i, u32 m, s64 arg) { return 0; }
static s32 vo_status(s32 h, u8 *st)
{
    *(u64 *)st = g_flips;
    *(s32 *)(st + 0x38) = 0;
    return 0;
}
static void vo_attr(void *a, u32 f, u32 t, u32 r, u32 w, u32 h, u32 p) {}
static s32 vo_open(s32 a, s32 b, s32 c, const void *d) { return 7; }
static void fake_exit(int code)
{
    printf("_exit(%d)\n", code);
    exit(g_failed);
}
static s32 thread_create(u64 *th, const void *attr, void *(*fn)(void *), void *arg, const char *name)
{
    return pthread_create(th, NULL, fn, arg);
}

static void *host_real_(const char *name)
{
    static const struct {
        const char *name;
        void *fn;
    } k[] = {{"sceKernelMkdir", k_mkdir},       {"sceKernelGetdents", k_getdents}, {"sceKernelRename", k_rename},
             {"sceKernelUnlink", k_unlink},     {"sceKernelFsync", k_fsync},       {"_exit", fake_exit},
             {"scePthreadCreate", thread_create}};
    for (u32 i = 0; i < ARRAY_SIZE(k); i++)
        if (strcmp(k[i].name, name) == 0)
            return k[i].fn;
    return NULL;
}
static void *host_real_in_(const char *mod, const char *name)
{
    static const struct {
        const char *name;
        void *fn;
    } k[] = {{"sceRtcGetCurrentClockLocalTime", rtc},
             {"sceMsgDialogTerminate", md_zero},
             {"sceMsgDialogInitialize", md_zero},
             {"sceMsgDialogOpen", md_open},
             {"sceMsgDialogUpdateStatus", md_status},
             {"sceMsgDialogClose", md_zero},
             {"sceUserServiceGetInitialUser", initial_user},
             {"sceVideoOutRegisterBuffers", vo_reg},
             {"sceVideoOutSetBufferAttribute", vo_attr},
             {"sceVideoOutSubmitFlip", vo_flip},
             {"sceVideoOutGetFlipStatus", vo_status}};
    for (u32 i = 0; i < ARRAY_SIZE(k); i++)
        if (strcmp(k[i].name, name) == 0)
            return k[i].fn;
    return NULL;
}
static int module_handle(const char *name, u32 *out)
{
    *out = 5;
    return strcmp(name, "libSceUsbStorage") == 0 ? 0 : -1;
}
static void *host_dlsym_(u32 h, const char *name)
{
    return strcmp(name, "sceUsbStorageGetDeviceList") == 0 ? (void *)usb_list : NULL;
}
static u64 hle_lookup_(const char *lib, const char *name)
{
    return strcmp(name, "sceVideoOutOpen") == 0 ? (u64)vo_open : 0;
}
static void *page_alloc_(size_t n) { return calloc(1, n); }
static void page_free_(void *p, size_t n) { free(p); }
static void spin_lock_(volatile int *l)
{
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        ;
}
static void spin_unlock_(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }
static int gfs_mount_(const char *dir, int mode, u64 blocks, char mp[16])
{
    char p[600], st[256];
    fmt(p, sizeof(p), "%s/%s", g_root, dir);
    if (strcmp(dir, "GLDRCONF") != 0 || stat(p, st) != 0)
        return (int)0x809F0008; /* NOT_FOUND */
    strlcpy(mp, "/savedata0", 16);
    return 0;
}
static int gfs_umount_(const char mp[16], u32 ms) { return 0; }

/* ---- what the extension's other files give logfile.c and failure.c ---- */
void usb_ftp_prepare(void) { printf("usb_ftp_prepare\n"); }
int usb_title_slot(void) { return getenv("TITLE_SLOT") ? getenv("TITLE_SLOT")[0] - '0' : -1; }
static volatile u32 g_gen;
bool usb_live_waiting(void) { return false; }
u32 usb_live_gen(void) { return g_gen; }
s32 ov_vo_handle(void) { return 3; }
static int g_buttons_at = 3;
u32 ov_buttons(void) { return --g_buttons_at <= 0 ? 0x4000 : 0; }
bool ov_open(void) { return false; }
void ov_page(u32 *px, u32 w, u32 h, u32 pitch, const char *head, const char *body, const char *foot)
{
    fmt(g_page, sizeof(g_page), "%s\n%s\n%s", head, body, foot ? foot : "");
    printf("page: %s\n", g_page);
}
bool ov_page_shown(const char *head, const char *body, const char *foot) { return false; }
void ov_banner(const char *text, u32 seconds) { printf("banner: %s\n", text); }

void harness_family(const struct ext_family *f) { (void)f; } /* the scenarios call what init would */
struct ghost_diag *log_diag(void); /* log.c */

/* swap.c's stats, which a crash report ends with (none here) */
void swap_log(void) { logf("swap: the harness backs no heap in part"); }

static char *slurp(const char *p, long *size);
static int g_usleeps;
static int k_usleep(u32 us)
{
    if (strcmp(g_mode, "crash") == 0 && ++g_usleeps > 3) { /* the crash page stays: done */
        printf("crash page kept: %s\n", g_page);
        expect(strstr(g_page, "The game crashed.") != NULL, "crash: the page says so");
        expect(strstr(g_page, "eboot.bin+0x1234") != NULL, "crash: where, for a report");
        char *log = slurp("/temp0/ghostldr.log", NULL);
        expect(strstr(log, "(2 MiB, prot 3, phys 10000000, allocated from eboot+1234 type 0): 2 MiB written, the last "
                           "at +2 MiB") != NULL &&
                   strstr(log, "(2 MiB, prot 3, phys 10200000, allocated from eboot+5678 type 3): 1 MiB written, the "
                               "last at +2 MiB") != NULL,
               "crash: how full the title's memory was");
        expect(strstr(log, "swap: the harness backs no heap in part") != NULL, "crash: the swap file's stats");
        exit(g_failed);
    }
    if (strcmp(g_mode, "hang") == 0)
        return usleep(us / 2000);
    return 0;
}

static struct ghost_api g_api;

/* ---- checks ---- */
static long file_size(const char *p)
{
    char hp[600];
    int fd = open(host_path(p, hp), 0);
    long n = fd >= 0 ? lseek(fd, 0, 2) : -1;
    if (fd >= 0)
        close(fd);
    return n;
}
static char *slurp(const char *p, long *size)
{
    char hp[600];
    int fd = open(host_path(p, hp), 0);
    long n = fd >= 0 ? lseek(fd, 0, 2) : 0;
    char *b = calloc(1, (unsigned long)n + 1);
    if (fd >= 0)
        pread(fd, b, (unsigned long)n, 0), close(fd);
    if (size)
        *size = n;
    return b;
}
static int count(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)); p++)
        n++;
    return n;
}
static void mk(const char *p)
{
    char hp[600];
    mkdir(host_path(p, hp), 0777);
}
static void put(const char *p, const char *text)
{
    char hp[600];
    int fd = open(host_path(p, hp), 0x601, 0644);
    write(fd, text, strlen(text));
    close(fd);
}

typedef bool (*attach_fn)(void);
bool log_attach(void);
void log_detach(void);
void diag_placed(void);
void diag_title_starting(void);

static void scenario_log(void)
{
    struct ghost_diag *D = log_diag();
    expect(D && D->f[0].fd >= 0 && D->start == 14, "/temp0's log kept what was there; this boot starts after it");
    logf("core line before the extension");
    diag_placed();
    expect(D->sink != NULL, "the extension takes the lines");
    logf("ext line 1");
    expect(log_attach(), "a drive: attached");
    const char *drive = D->f[1].path;
    printf("drive file %s, where %s\n", drive, D->where);
    expect(strstr(drive, "/ghostldr_usb/ghostldr/logs/0001_2026-10-28_2115.log") == drive, "named 0001_date_time");
    logf("both line 2");
    long n;
    char *t = slurp(drive, &n);
    expect(!strstr(t, "old boot") && count(t, "core line before") == 1 && count(t, "ext line 1") == 1 &&
               count(t, "both line 2") == 1,
           "the drive has this boot's lines, once each, and not the earlier boot's");
    free(t);
    log_detach();
    logf("while detached");
    log_attach();
    logf("after again");
    t = slurp(drive, NULL);
    expect(count(t, "while detached") == 1 && count(t, "after again") == 1 && count(t, "both line 2") == 1,
           "detached and attached again: the lines in between are copied, nothing twice");
    free(t);
    g_fail_drive_writes = 1;
    logf("lost on the drive");
    expect(D->f[1].fd < 0 && D->f[1].lost, "a write that fails ends the drive's file");
    logf("only in temp0");
    g_fail_drive_writes = 0;
    g_gen++;
    log_attach();
    logf("drive is back");
    t = slurp(drive, NULL);
    expect(count(t, "lost on the drive") == 1 && count(t, "only in temp0") == 1 && count(t, "drive is back") == 1,
           "the drive back: what it missed is copied first");
    free(t);
    t = slurp("/temp0/ghostldr.log", NULL);
    expect(count(t, "old boot") == 1 && count(t, "drive is back") == 1, "/temp0 has every line too");
    free(t);
    /* rotation: /temp0 at 8 MiB */
    char big[1000];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    for (int i = 0; i < 9000; i++)
        logf("%s", big);
    expect(file_size("/temp0/ghostldr.1.log") > (6 << 20) && file_size("/temp0/ghostldr.log") < (2 << 20),
           "/temp0 past 8 MiB: renamed to ghostldr.1.log and started again");
    expect(file_size(drive) > (8 << 20), "the drive's file goes on (its cap is 32 MiB)");
}

static void scenario_names(void)
{
    mk("/ghostldr_usb/ghostldr");
    mk("/ghostldr_usb/ghostldr/logs");
    char p[128];
    for (int i = 1; i <= 35; i++) {
        fmt(p, sizeof(p), "/ghostldr_usb/ghostldr/logs/%04d_2026-10-01_1000.log", i);
        put(p, "x");
    }
    put("/ghostldr_usb/ghostldr/logs/notes.txt", "mine");
    put("/ghostldr_usb/ghostldr/logs/0002_x.txt", "mine too");
    put("/ghostldr_usb/readme.txt", "the user's");
    diag_placed();
    expect(log_attach(), "attached");
    expect(strstr(log_diag()->f[1].path, "/0036_2026-10-28_2115.log") != NULL, "the next number: 0036");
    expect(file_size("/ghostldr_usb/ghostldr/logs/0006_2026-10-01_1000.log") < 0 &&
               file_size("/ghostldr_usb/ghostldr/logs/0007_2026-10-01_1000.log") == 1,
           "30 kept: 0001-0006 removed, 0007 on stay");
    expect(file_size("/ghostldr_usb/ghostldr/logs/notes.txt") == 4 &&
               file_size("/ghostldr_usb/ghostldr/logs/0002_x.txt") == 8 && file_size("/ghostldr_usb/readme.txt") == 10,
           "files that aren't ghostldr's logs are left alone");
}

static void scenario_off(void)
{
    g_ctx_v.flags |= CTX_FLAG_NO_USB_LOG;
    diag_placed();
    expect(!log_attach(), "CTX_FLAG_NO_USB_LOG: not attached");
    expect(strstr(log_diag()->where, "off") != NULL, "where says it's off");
    expect(file_size("/ghostldr_usb/ghostldr") < 0, "nothing written to the drive");
}

static void scenario_nodrive(void)
{
    diag_placed();
    expect(!log_attach(), "no drive: not attached");
    printf("where: %s\n", log_diag()->where);
    expect(strstr(log_diag()->where, "No USB drive") && strstr(log_diag()->where, "/temp0/ghostldr.log"),
           "where: /temp0");
}

static void boot_case(s64 rc, u32 note, const char *path, const char *want)
{
    struct ghost_diag *D = log_diag();
    D->note = note, D->shown = 0, g_opened = 0, g_polls = 0, g_dialog[0] = 0;
    strlcpy(D->note_path, path, sizeof(D->note_path));
    D->show(FAIL_BOOT, rc);
    char what[200];
    fmt(what, sizeof(what), "boot %ld note %u: \"%s\"", rc, note, want);
    expect(strstr(g_dialog, want) && strstr(g_dialog, "What to do: ") && !strstr(g_dialog, "%s") &&
               strstr(g_dialog, "The log: ") && strstr(g_dialog, "Luac0re's loader takes scripts again"),
           what);
}

static void scenario_boot(void)
{
    diag_placed();
    boot_case(-2, FAIL_FILE_MISSING, "/ghostldr_usb/Games/Foo/eboot.bin", "Games/Foo/eboot.bin isn't there.");
    boot_case(-2, FAIL_ENCRYPTED, "/app0/eboot.bin", "The game's files are encrypted.");
    boot_case(-1, FAIL_FILE_BAD, "/app0/sce_module/libc.prx", "isn't a PS4 program");
    boot_case(-5, FAIL_MEMORY, "eboot.bin", "needs more memory");
    boot_case(-5, 0, "", "couldn't be loaded");
    boot_case(-3, 0, "", "no room for the game's code");
    boot_case(-3, FAIL_CODE_SPACE, "eboot.bin won't fit: its code needs 64000 KiB of JIT memory",
              "eboot.bin won't fit: its code needs 64000 KiB");
    boot_case(-4, 0, "", "couldn't be prepared");
    boot_case(-6, FAIL_NO_DRIVE, "", "No USB drive found.");
    boot_case(-6, FAIL_NO_FOLDER, "My Games", "No USB drive has the folder My Games.");
    boot_case(-6, FAIL_NOT_ON_DRIVE, "CUSA00900", "CUSA00900 isn't among the games");
    boot_case(-6, FAIL_NO_TITLE_ID, "Stuff/dump", "Stuff/dump has no sce_sys/param.sfo");
    boot_case(-6, FAIL_INSTALL_INCOMPLETE, "CUSA11456", "CUSA11456 isn't completely installed.");
    boot_case(-6, FAIL_NOTHING_INSTALLED, "", "The USB drive has no game ghostldr can start, and no game is installed");
    boot_case(0x80020023, 0, "", "Its main thread couldn't be created.");
    boot_case(-9, 0, "", "The game didn't start.");
    printf("dialog: %s\n", g_dialog);
    expect(file_size(log_diag()->f[1].path) < 0 || log_diag()->f[1].fd < 0, "the boot's log file is closed after");
    long n;
    char *t = slurp("/ghostldr_usb/ghostldr/logs/0001_2026-10-28_2115.log", &n);
    expect(n > 0 && strstr(t, "failure "), "the failed boot's log is on the drive");
    free(t);
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    strlcpy(g_root, argv[1], sizeof(g_root));
    g_mode = argv[2];
    char p[600];
    fmt(p, sizeof(p), "%s/temp0", g_root);
    mkdir(p, 0777);
    if (strcmp(g_mode, "nodrive") != 0)
        fmt(p, sizeof(p), "%s/usb1", g_root), mkdir(p, 0777);
    put("/temp0/ghostldr.log", "old boot line\n");
    put("/dev/notification0", ""); /* the device is there */
    K = (struct kfuncs){.usleep = k_usleep, .getpid = k_getpid, .open = k_open, .read = k_read, .pread = k_pread,
                        .write = k_write, .close = k_close, .lseek = k_lseek, .map_flex = k_map_flex,
                        .direct_size = k_direct_size, .alloc_direct = k_alloc_direct, .map_direct = k_map_direct,
                        .virtual_query = k_virtual_query};
    g_ctx = &g_ctx_v;
    g_ctx_v.log_fd = (u64)-1;
    static struct module eboot = {.name = "eboot.bin", .base = 0x400000, .image_size = 0x100000};
    g_mods = g_main = &eboot;
    g_api = (struct ghost_api){.version = GHOST_API_VERSION, .size = sizeof(struct ghost_api), .ctx = &g_ctx,
                               .k = &K, .modules = &g_mods, .main_mod = &g_main, .reported_dmem = &g_dmem,
                               .logf = logf, .host_real = host_real_, .host_real_in = host_real_in_,
                               .host_dlsym = host_dlsym_, .host_module_handle = module_handle,
                               .page_alloc = page_alloc_, .page_free = page_free_, .spin_lock = spin_lock_,
                               .spin_unlock = spin_unlock_, .vfs_stat = vstat, .hle_lookup = hle_lookup_,
                               .gfs_mount = gfs_mount_, .gfs_umount = gfs_umount_, .diag = log_diag,
                               .dm_usage = g_usage, .dm_nusage = &g_nusage};
    G = &g_api;
    logf("first line");
    if (strcmp(g_mode, "log") == 0)
        scenario_log();
    else if (strcmp(g_mode, "names") == 0)
        scenario_names();
    else if (strcmp(g_mode, "off") == 0)
        scenario_off();
    else if (strcmp(g_mode, "nodrive") == 0)
        scenario_nodrive();
    else if (strcmp(g_mode, "boot") == 0)
        scenario_boot();
    else if (strcmp(g_mode, "crash") == 0) {
        diag_placed();
        log_attach();
        struct ghost_diag *D = log_diag();
        D->title = 1; /* (no monitor thread: its sleeps would end the scenario) */
        D->sig = 11, D->rip = 0x401234, D->crashed = 1;
        g_heap = calloc(1, 4 << 20);
        memset(g_heap, 1, 2 << 20);
        memset(g_heap + (3 << 20), 1, 1 << 20);
        D->show(FAIL_CRASH, 11); /* does not return: k_usleep ends it */
        expect(false, "the crash page returned");
    } else if (strcmp(g_mode, "exit") == 0) {
        diag_placed();
        diag_title_starting();
        log_diag()->show(FAIL_EXIT, 3); /* ends in fake_exit */
        expect(false, "exit returned");
    } else if (strcmp(g_mode, "hang") == 0) {
        diag_placed();
        g_flips = 5; /* it drew, then stopped */
        diag_title_starting();
        for (int i = 0; i < 400 && file_size("/dev/notification0") <= 0; i++)
            usleep(10000);
        long n;
        char *t = slurp("/dev/notification0", &n);
        expect(n == 0xC30 && strstr(t + 0x2D, "ghostldr: The game seems stuck."), "a still screen: a notification");
        free(t);
    }
    return g_failed;
}
