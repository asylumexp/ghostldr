/*
 * selector_host: the core's real src/sel.c and the real build/ghostldr/
 * selector.bin, run in an ordinary x86-64 process (Rosetta on Apple
 * silicon) against a fake console.  The "JIT code buffer" the module runs
 * from is read+execute except while the fake compiler writes it, so a module
 * that writes to its own image faults as it would on the console.  SWRR's
 * save data is a directory tree (SAVES/<container>/<file>) mounted through
 * the core's gfs_mount.  The fake console records direct memory, VideoOut,
 * pad, module loads, mounts and open files, and the run fails unless
 * everything the selector took is given back.  test_selector.py builds the
 * trees and drives it:
 *
 *     selector_host MODE SELECTOR_BIN CRC SAVES OUT_DIR
 *     MODE: single  a v0.1 install (GFSINDEX, no catalogue), shown on request,
 *                   built-in font: Options, Circle, Cross -> title 0
 *           skip    the same, not requested: the core does not load it
 *           multi   GFSCATALOG with five titles: Right, Right, Cross -> 2 (CUSA00003)
 *           approot no ghostfs: one title in an app root (dev setups)
 *           nofont  like single, libSceFont.sprx cannot be loaded
 *           badfont like single, libSceFont loads but its library fails
 *           badcrc  sel.c refuses a module with the wrong CRC
 *           ps4     a PS4 (CTX_FLAG_PS4), three titles: Right, Cross, Cross (No),
 *                   Cross, Circle, Cross, Left (Yes), Cross -> 1 (CUSA00001)
 *           apollo  a save tool (APOL00004, its profile's "save_tool") and a game:
 *                   Cross asks whose saves (two users, the second not signed
 *                   in: Down, Cross refused), Up, Cross: the title is mounted
 *                   (title_pick) and scanned (save_prescan, 40 fake titles of
 *                   300 ms: its bar shows); Circle held cancels it, Cross scans
 *                   the rest -> 0 (APOL00004) for user 10000001 (title_user)
 *           apollo-one the save tool alone, not requested (as boot.c shows it
 *                   for a save tool): asked at once, a short scan, no bar -> 0
 *           one     one title, not requested: shown with its Settings tile
 *                   (boot.c now shows it); Right, Left, Cross -> 0
 *           empty   nothing installed: the Settings tile alone; Cross opens
 *                   Settings, About, Close ghostldr, Yes -> SEL_EXIT_LOADER
 *           settings two titles, a test module with the fake network installer
 *                   (tests/selector/netinst_fake); Square, Network, Install over
 *                   network (it waits, installs, then mounts NETFAKEINSTALLED,
 *                   after which GFSCATALOG.after is served for GFSCATALOG),
 *                   Circle stops it; Logs: the switch (a fake log_usb); the row
 *                   now has three titles: Right, Right, Cross -> 2, the new one
 *
 * Environment: SEL_SHOTS="ms:name,..." saves what is on the screen at each
 * time as OUT_DIR/MODE-name.png.  SEL_BUNDLE=1: the module comes in memory,
 * as the release's ghostldr.lua passes it (CTX_FLAG_BUNDLE), and sel.c must
 * unmap it whether it runs it or not.
 */
#include "sel_api.h"

/* host_sys.c / png_host.c (libc world) */
void *sys_map(u64 size);
void sys_unmap(void *p, u64 size);
void sys_protect(void *addr, u64 size, int writable);
int sys_open(const char *path);
int sys_open_w(const char *path);
s64 sys_write(int fd, const void *buf, u64 n);
int sys_mkdir(const char *path);
s64 sys_read(int fd, void *buf, u64 n);
s64 sys_pread(int fd, void *buf, u64 n, s64 off);
int sys_close(int fd);
s64 sys_file_size(const char *path);
void *sys_calloc(u64 n);
u64 sys_strtoul(const char *s);
int sys_vfmt(char *buf, u64 size, const char *fmt, __builtin_va_list ap);
const char *sys_getenv(const char *name);
void sys_vlog(const char *fmt, __builtin_va_list ap);
void sys_exit(int code);
int png_host_decode(const u8 *data, u64 n, u32 *w, u32 *h, u32 *out);
int png_host_write(const char *path, const u32 *px, int w, int h, int stride);

struct kfuncs K;
struct boot_ctx *g_ctx;

static const char *g_mode, *g_art, *g_out;
static int g_failed;

void say(const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    sys_vlog(f, ap);
    __builtin_va_end(ap);
}

static int fmtx(char *buf, u64 size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = sys_vfmt(buf, size, f, ap);
    __builtin_va_end(ap);
    return n;
}

void logf(const char *f, ...)
{
    char line[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    sys_vfmt(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    say("[ghostldr] %s", line);
}

static void check(int ok, const char *what)
{
    say("selector_host: %s %s", ok ? "ok" : "FAILED", what);
    if (!ok)
        g_failed = 1;
}

static int str_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* ---- the JIT code buffer and the compiler ---- */
#define REGION_SIZE (8ul << 20)
#define JIT_MEMCPY 0x5EC0DE01ul
static u8 *g_region;
static u32 g_compiler_writes;

int arena_init(void)
{
    if (!g_region) {
        g_region = sys_map(REGION_SIZE);
        sys_protect(g_region, REGION_SIZE, 0);
        /* a PS4's ps4_verify probes each JIT buffer through the compiler,
         * which copies through the staging buffer's first bytes: over the
         * selector's header (ud2 filler, as read from a JIT buffer) */
        if (g_ctx->flags & CTX_FLAG_PS4)
            memset((void *)g_ctx->staging, 0x0b, 16);
    }
    return 0;
}

int arena_code_region(u64 *lo, u64 *hi)
{
    *lo = (u64)g_region, *hi = (u64)g_region + REGION_SIZE;
    return 0;
}

/* the buffer is written where it is executed here (arena.c's PS4 alias is
 * a console concern) */
u64 arena_wr(u64 dst, u64 size)
{
    (void)size;
    return dst;
}

u64 jit_call(u64 fn, u64 rax, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6)
{
    if (fn != JIT_MEMCPY || a1 < (u64)g_region || a1 + a3 > (u64)g_region + REGION_SIZE) {
        check(0, "jit_call: only memcpy into the code buffer is expected");
        return (u64)-1;
    }
    sys_protect(g_region, REGION_SIZE, 1);
    memcpy((void *)a1, (const void *)a2, a3);
    sys_protect(g_region, REGION_SIZE, 0);
    g_compiler_writes++;
    return a1;
}

u64 call_gadget6(u64 fn, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6)
{
    return ((u64(*)(u64, u64, u64, u64, u64, u64))fn)(a1, a2, a3, a4, a5, a6);
}

/* ---- time ---- */
static u64 g_now = 1000000; /* microseconds; only usleep moves it */
static int k_usleep(u32 us)
{
    g_now += us;
    return 0;
}
static u64 k_process_time(void) { return g_now; }
static int k_gettimeofday(s64 *tv, void *tz)
{
    tv[0] = 1792705620 + (s64)(g_now / 1000000); /* 21:47 UTC */
    tv[1] = 0;
    return 0;
}
static int k_utc2local(s64 utc, s64 *local, void *ts, u64 *dst)
{
    *local = utc;
    return 0;
}

/* ---- direct memory ---- */
#define RUN_START 0x20000000ul
#define RUN_END 0xE0000000ul
struct block {
    u64 phys, len, va;
    int type, prot, released, unmapped;
};
static struct block g_blocks[16];
static int g_nblocks;

static int k_alloc_direct(off_t start, off_t end, size_t len, size_t align, int type, off_t *out)
{
    u64 at = ((u64)end - len) & ~(align - 1);
    if ((u64)start > at || at < RUN_START || at + len > RUN_END)
        at = RUN_START + (u64)g_nblocks * 0x10000000ul; /* "anywhere" */
    for (int i = 0; i < g_nblocks; i++)
        if (!g_blocks[i].released && at < g_blocks[i].phys + g_blocks[i].len && g_blocks[i].phys < at + len)
            return (int)0x8002000c;
    g_blocks[g_nblocks++] = (struct block){at, len, 0, type, 0, 0, 0};
    *out = (off_t)at;
    return 0;
}

static int k_map_direct(void **addr, size_t len, int prot, int flags, off_t phys, size_t align)
{
    for (int i = 0; i < g_nblocks; i++)
        if (g_blocks[i].phys == (u64)phys && !g_blocks[i].va && g_blocks[i].len == len) {
            g_blocks[i].va = (u64)sys_map(len);
            g_blocks[i].prot = prot;
            *addr = (void *)g_blocks[i].va;
            return 0;
        }
    return (int)0x80020016;
}

static u8 *g_bundle_sel; /* SEL_BUNDLE: the module in "flexible memory" */
static u64 g_bundle_len;
static int g_bundle_unmapped;

static int k_munmap(void *addr, size_t len)
{
    if (g_bundle_sel && addr == g_bundle_sel && len == g_bundle_len && !g_bundle_unmapped) {
        g_bundle_unmapped = 1;
        return 0;
    }
    for (int i = 0; i < g_nblocks; i++)
        if (g_blocks[i].va == (u64)addr && g_blocks[i].len == len && !g_blocks[i].unmapped) {
            sys_unmap(addr, len);
            g_blocks[i].unmapped = 1;
            return 0;
        }
    check(0, "munmap of something the selector did not map");
    return (int)0x80020016;
}

static int k_release_direct(off_t phys, size_t len)
{
    for (int i = 0; i < g_nblocks; i++)
        if (g_blocks[i].phys == (u64)phys && g_blocks[i].len == len && !g_blocks[i].released) {
            g_blocks[i].released = 1;
            return 0;
        }
    check(0, "release of direct memory the selector did not allocate");
    return (int)0x80020016;
}

static size_t k_direct_size(void) { return 0x120000000ul; }
static int k_avail_direct(off_t start, off_t end, size_t align, off_t *off, size_t *size)
{
    *off = (off_t)RUN_START, *size = RUN_END - RUN_START;
    return 0;
}
static int k_avail_flex(size_t *out)
{
    *out = 38u << 20;
    return 0;
}

/* ---- system modules ---- */
#define H_KERNEL 1
#define H_VIDEO 0x20
#define H_PAD 0x21
#define H_USER 0x22
#define H_SYSSVC 0x23
#define H_PNG 0x40
#define H_FONT 0x41
#define H_FONTFT 0x42
static const struct {
    u32 handle;
    const char *name;
} g_loaded_by_swrr[] = {
    {H_VIDEO, "libSceVideoOut.sprx"}, {H_PAD, "libScePad.sprx"}, {H_USER, "libSceUserService.sprx"},
    {H_SYSSVC, "libSceSystemService.sprx"},
};
static int g_mod_loaded[3], g_mod_unloaded[3]; /* png, font, fontft */

static int k_module_list(u32 *handles, size_t max, size_t *count)
{
    size_t n = 0;
    for (u32 i = 0; i < sizeof(g_loaded_by_swrr) / sizeof(g_loaded_by_swrr[0]) && n < max; i++)
        handles[n++] = g_loaded_by_swrr[i].handle;
    *count = n;
    return 0;
}

static int k_module_info(u32 handle, struct sce_module_info *info)
{
    for (u32 i = 0; i < sizeof(g_loaded_by_swrr) / sizeof(g_loaded_by_swrr[0]); i++)
        if (g_loaded_by_swrr[i].handle == handle) {
            strcpy(info->name, g_loaded_by_swrr[i].name);
            return 0;
        }
    return (int)0x80020002;
}

static int k_load_start_module(const char *path, size_t argc, const void *argv, u32 flags, void *opt, int *res)
{
    if (str_eq(path, "libScePngDec.sprx")) {
        g_mod_loaded[0]++;
        return H_PNG;
    }
    if (str_eq(path, "libSceFont.sprx") && !str_eq(g_mode, "nofont")) {
        g_mod_loaded[1]++;
        return H_FONT;
    }
    if (str_eq(path, "libSceFontFt.sprx") && !str_eq(g_mode, "nofont")) {
        g_mod_loaded[2]++;
        return H_FONTFT;
    }
    return (int)0x80020002;
}

static int k_stop_unload(u32 handle, size_t argc, const void *argv, u32 flags, void *opt, int *res)
{
    int i = handle == H_PNG ? 0 : handle == H_FONT ? 1 : handle == H_FONTFT ? 2 : -1;
    if (i < 0 || !g_mod_loaded[i]) {
        check(0, "unload of a module the selector did not load");
        return -1;
    }
    g_mod_unloaded[i]++;
    return 0;
}

/* ---- SWRR's save data: SAVES/<container>/<file> ---- */
static char g_mp[16][32]; /* mount point N -> container */
static int g_mounts, g_umounts, g_max_mounted, g_opens, g_closes;

static int mounted_now(void)
{
    int n = 0;
    for (int i = 1; i < 16; i++)
        n += g_mp[i][0] != 0;
    return n;
}

bool gfs_active(void) { return !str_eq(g_mode, "approot"); }
static int g_net_installed, g_conf_creates, g_conf_writes;
int gfs_mount(const char *dirname, int mode, u64 blocks, char mount_point[16])
{
    char dir[1024];
    fmtx(dir, sizeof(dir), "%s/%s", g_art, dirname);
    if (g_net_installed && str_eq(dirname, "GFSCATALOG")) /* the network installer wrote a new catalogue */
        fmtx(dir, sizeof(dir), "%s/GFSCATALOG.after", g_art);
    if (str_eq(dirname, "NETFAKEINSTALLED")) { /* the fake installer says it installed a title */
        g_net_installed = 1;
        say("selector_host: a title was installed over the network");
        return (int)0x809f0008;
    }
    if (mode != 1 && !str_eq(dirname, "GLDRCONF")) {
        check(0, "the selector mounts read-only (but its own settings, GLDRCONF)");
        return (int)0x809f0001;
    }
    if (mode == 6 && sys_file_size(dir) < 0) { /* create: a new container (96 blocks at least) */
        check(blocks >= 96, "a new container of 96 blocks or more");
        sys_mkdir(dir);
        g_conf_creates++;
    }
    if (sys_file_size(dir) < 0)
        return (int)0x809f0008; /* SCE_SAVE_DATA_ERROR_NOT_FOUND */
    if (mode != 1)
        g_conf_writes++;
    for (int i = 1; i < 16; i++)
        if (!g_mp[i][0]) {
            strcpy(g_mp[i], g_net_installed && str_eq(dirname, "GFSCATALOG") ? "GFSCATALOG.after" : dirname);
            fmtx(mount_point, 16, "/savedata%d", i);
            g_mounts++;
            if (mounted_now() > g_max_mounted)
                g_max_mounted = mounted_now();
            return 0;
        }
    return (int)0x809f000c; /* MOUNT_FULL */
}
int gfs_umount(const char mount_point[16], u32 max_ms)
{
    int i = 0;
    for (const char *p = mount_point + 9; *p >= '0' && *p <= '9'; p++)
        i = i * 10 + (*p - '0');
    if (i < 1 || i > 15 || !g_mp[i][0]) {
        check(0, "unmount of something not mounted");
        return -1;
    }
    g_mp[i][0] = 0;
    g_umounts++;
    return 0;
}

/* "/savedataN/file" -> SAVES/<container>/file; app-root paths -> SAVES/... */
static int host_path(char *out, u64 size, const char *path)
{
    if (strncmp(path, "/savedata", 9) == 0) {
        int i = 0;
        const char *p = path + 9;
        while (*p >= '0' && *p <= '9')
            i = i * 10 + (*p++ - '0');
        if (i < 1 || i > 15 || !g_mp[i][0])
            return -1;
        fmtx(out, size, "%s/%s%s", g_art, g_mp[i], p);
        return 0;
    }
    if (strncmp(path, "/data/test/app0/", 16) == 0) {
        fmtx(out, size, "%s/%s", g_art, path + 16);
        return 0;
    }
    return -1;
}

static int k_open(const char *path, int flags, int mode)
{
    char p[1024];
    int fd = host_path(p, sizeof(p), path) == 0 ? (flags & 3 ? sys_open_w(p) : sys_open(p)) : -1;
    if (fd < 0)
        return (int)0x80020002;
    g_opens++;
    return fd;
}
static ssize_t k_read(int fd, void *buf, size_t n) { return sys_read(fd, buf, n); }
static ssize_t k_write(int fd, const void *buf, size_t n) { return sys_write(fd, buf, n); }
static ssize_t k_pread(int fd, void *buf, size_t n, off_t off) { return sys_pread(fd, buf, n, off); }
static int k_close(int fd)
{
    g_closes++;
    return sys_close(fd);
}
static int k_stat(const char *path, void *st)
{
    char p[1024];
    s64 n = host_path(p, sizeof(p), path) == 0 ? sys_file_size(p) : -1;
    if (n < 0)
        return (int)0x80020002;
    memset(st, 0, 0x78);
    *(s64 *)((u8 *)st + 0x48) = n;
    return 0;
}

int vfs_open(const char *path, int flags, int mode) { return k_open(path, flags, mode); }
s64 vfs_read(int fd, void *buf, u64 n) { return sys_read(fd, buf, n); }
int vfs_close(int fd) { return k_close(fd); }
int vfs_stat(const char *path, void *st) { return k_stat(path, st); }

/* ---- the pad: buttons from a script of (ms, buttons) ---- */
struct step {
    u32 ms, buttons;
};
static const struct step SINGLE[] = {{1500, 0x8}, {1600, 0}, {2600, 0x2000}, {2700, 0}, {3400, 0x4000}, {3500, 0}};
static const struct step MULTI[] = {{2500, 0x20}, {2600, 0}, {3000, 0x20}, {3100, 0}, {4400, 0x4000}, {4500, 0}};
static const struct step PS4[] = {{2500, 0x20},   {2600, 0}, {3200, 0x4000}, {3300, 0}, {3800, 0x4000}, {3900, 0},
                                  {4400, 0x4000}, {4500, 0}, {5000, 0x2000}, {5100, 0}, {5600, 0x4000}, {5700, 0},
                                  {6200, 0x80},   {6300, 0}, {6800, 0x4000}, {6900, 0}};
/* Right, Left, Cross (whose saves), Down, Cross (not signed in), Up, Cross (the
 * scan), Circle held (cancel), Cross (the rest) */
static const struct step APOLLO[] = {{2500, 0x20},   {2600, 0}, {3000, 0x80},   {3100, 0}, {3500, 0x4000}, {3600, 0},
                                     {4200, 0x40},   {4300, 0}, {4800, 0x4000}, {4900, 0}, {5500, 0x10},   {5600, 0},
                                     {6200, 0x4000}, {6300, 0}, {8000, 0x2000}, {9000, 0}, {10000, 0x4000}, {10100, 0}};
static const struct step APOLLO_ONE[] = {{1500, 0x4000}, {1600, 0}};
/* Right (the gear), Left, Cross */
static const struct step ONE[] = {{1500, 0x20}, {1600, 0}, {2500, 0x80}, {2600, 0}, {3500, 0x4000}, {3600, 0}};
/* Cross (Settings), Down x5 (About), Right, Cross (Close ghostldr), Left (Yes), Cross */
static const struct step EMPTY[] = {{1500, 0x4000}, {1600, 0}, {1800, 0x40}, {1900, 0}, {2000, 0x40},   {2100, 0},
                                    {2300, 0x40},   {2400, 0}, {2600, 0x40}, {2700, 0}, {2900, 0x40},   {3000, 0},
                                    {3300, 0x20},   {3400, 0}, {3700, 0x4000}, {3800, 0}, {4200, 0x80}, {4300, 0},
                                    {4600, 0x4000}, {4700, 0}};
/* Square, Down x2 (Network), Right, Down (Install over network), Cross: the
 * installer (waits, installs a title by 6 s); Circle stops it, Left; Down
 * (Logs), Right, Cross (the switch: off); Circle, Circle (Settings closes);
 * Right x2 (the new title), Cross */
static const struct step SETTINGS[] = {
    {1500, 0x8000}, {1600, 0}, {2000, 0x40}, {2100, 0}, {2300, 0x40}, {2400, 0}, {2700, 0x20}, {2800, 0},
    {3100, 0x40},   {3200, 0}, {3500, 0x4000}, {3600, 0}, {8000, 0x2000}, {8100, 0}, {8400, 0x80}, {8500, 0},
    {8800, 0x40},   {8900, 0}, {9200, 0x20}, {9300, 0}, {9600, 0x4000}, {9700, 0}, {10000, 0x2000}, {10100, 0},
    {10400, 0x2000}, {10500, 0}, {11000, 0x20}, {11100, 0}, {11500, 0x20}, {11600, 0}, {12500, 0x4000}, {12600, 0}};
#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

static int pad_open(s32 user, s32 type, s32 index, const void *param) { return user == 0x10000001 ? 5 : -1; }
static void shots(void);
static int pad_read(s32 handle, u8 *data)
{
    shots();
    const struct step *s = str_eq(g_mode, "multi") ? MULTI : str_eq(g_mode, "ps4") ? PS4 : str_eq(g_mode, "apollo") ? APOLLO
                         : str_eq(g_mode, "apollo-one") ? APOLLO_ONE : str_eq(g_mode, "one") ? ONE
                         : str_eq(g_mode, "empty") ? EMPTY : str_eq(g_mode, "settings") ? SETTINGS : SINGLE;
    int n = str_eq(g_mode, "ps4") ? N(PS4) : str_eq(g_mode, "apollo") ? N(APOLLO) : str_eq(g_mode, "apollo-one") ? N(APOLLO_ONE)
          : str_eq(g_mode, "one") ? N(ONE) : str_eq(g_mode, "empty") ? N(EMPTY) : str_eq(g_mode, "settings") ? N(SETTINGS) : 6;
    u32 ms = (u32)((g_now - 1000000) / 1000), b = 0;
    for (int i = 0; i < n; i++)
        if (ms >= s[i].ms)
            b = s[i].buttons;
    memset(data, 0, 0x68);
    *(u32 *)data = b | 0x80000000u; /* SWRR's leftover intercepted bit */
    data[4] = data[5] = data[6] = data[7] = 128;
    data[0x4c] = 1;
    return 0;
}
static int user_initial(s32 *u)
{
    *u = 0x10000001;
    return 0;
}
static int user_name(s32 u, char *name, u64 size)
{
    const char *n = u == 0x10000002 ? "Second" : "Tester";
    for (u64 i = 0; i < size; i++)
        if (!(name[i] = i < 6 ? n[i] : 0))
            break;
    return u == 0x10000001 || u == 0x10000002 ? 0 : (int)0x80960009;
}
/* two users on the console; only the first signed in */
static int g_user_lists;
static int user_registered(s32 *list)
{
    for (int i = 0; i < 16; i++)
        list[i] = i < 2 ? 0x10000001 + i : -1;
    g_user_lists++;
    return 0;
}
static int user_login(s32 *list)
{
    for (int i = 0; i < 4; i++)
        list[i] = i ? -1 : 0x10000001;
    return 0;
}

/* ---- a save tool: the core's title_pick, save_prescan and title_user ---- */
static u32 g_user_chosen;
static int g_picks, g_scans, g_scan_cancels;
static char g_picked[16];
static int g_picked_index = -1;
static u32 g_scanned;
static int title_pick(const char *title, int index)
{
    g_picks++;
    for (int i = 0; i < 16; i++)
        g_picked[i] = title[i];
    g_picked_index = index;
    say("selector_host: title_pick(%.16s, %d)", title, index);
    return 0;
}
/* 40 titles of 300 ms (apollo; 3 of 100 ms alone); titles searched before a cancel are not searched again */
static int save_prescan(u32 user, int (*progress)(void *arg, u32 done, u32 total), void *arg)
{
    int one = str_eq(g_mode, "apollo-one");
    u32 all = one ? 3 : 40, total = all - g_scanned, done = 0;
    g_scans++;
    check(g_picks == 1, "the title was mounted before its scan");
    say("selector_host: save_prescan(user %x): %u titles", user, total);
    int stop = progress(arg, 0, total);
    while (!stop && done < total) {
        k_usleep(one ? 100000 : 300000);
        stop = progress(arg, ++done, total);
    }
    g_scanned += done;
    g_scan_cancels += stop != 0;
    return (int)(done / 4);
}
/* ---- Settings: the core's log switch (a fake); the network installer is the
 * module's own (a test build linked with tests/selector/netinst_fake) ---- */
static int g_pages;
static void *f_page_alloc(size_t n)
{
    g_pages++;
    return sys_calloc(n);
}
static void f_page_free(void *p, size_t n) { g_pages--; }
static u32 f_profile_has(u32 mask) { return str_eq(g_mode, "apollo-one") ? mask & PF_SAVE_TOOL : 0; }
static const char *f_title_id(void) { return str_eq(g_mode, "apollo-one") ? "APOL00004" : ""; }
static int param_int(s32 id, s32 *v)
{
    *v = id == 1 ? 1 : id == 3 ? 1 : 0;
    return id == 1 || id == 3 ? 0 : (int)0x80A10003;
}

/* ---- VideoOut ---- */
struct vo_attr {
    u32 pixel_format, tiling, aspect, width, height, pitch, option, reserved0;
    u64 reserved1;
};
static int g_vo_open, g_vo_closed, g_vo_rate = -1, g_registered;
static u64 g_fb[2], g_flips, g_shot_after[8];
static char g_shot_name[8][32];
static int g_nshots, g_shot_done[8];

static int vo_open(s32 user, s32 bus, s32 index, const void *param)
{
    g_vo_open++;
    return user == 0xFF && bus == 0 ? 1 : -1;
}
static void vo_set_attr(struct vo_attr *a, u32 fmt, u32 tiling, u32 aspect, u32 w, u32 h, u32 pitch)
{
    *a = (struct vo_attr){fmt, tiling, aspect, w, h, pitch, 0, 0, 0};
}
static int vo_register(s32 h, s32 start, void *const *addrs, s32 n, const struct vo_attr *a)
{
    check(h == 1 && start == 0 && n == 2, "two buffers registered on the opened handle");
    check(a->pixel_format == 0x80000000u && a->tiling == 1 && a->width == 1920 && a->height == 1080 &&
              a->pitch == 1920,
          "buffer attribute: A8R8G8B8 sRGB, linear, 1920x1080");
    for (int i = 0; i < n; i++) {
        int ok = 0;
        for (int b = 0; b < g_nblocks; b++)
            if (g_blocks[b].type == 3 && g_blocks[b].prot == 0x33 && (u64)addrs[i] >= g_blocks[b].va &&
                (u64)addrs[i] + 1920ul * 1080 * 4 <= g_blocks[b].va + g_blocks[b].len &&
                ((u64)addrs[i] - g_blocks[b].va) % 0x10000 == 0)
                ok = 1;
        check(ok, "scan-out buffer in GPU-visible write-combined memory, 64 KiB aligned");
        g_fb[i] = (u64)addrs[i];
    }
    g_registered = 1;
    return 0;
}
static int vo_rate(s32 h, s32 rate)
{
    g_vo_rate = rate;
    return 0;
}
static int g_shown = -1;
static int vo_submit(s32 h, s32 index, s32 mode, s64 arg)
{
    g_flips++;
    g_shown = index;
    return index == 0 || index == 1 ? 0 : -1;
}

/* what is on the screen at each requested time (checked once per frame) */
static void shots(void)
{
    u32 ms = (u32)((g_now - 1000000) / 1000);
    for (int i = 0; i < g_nshots; i++)
        if (!g_shot_done[i] && ms >= g_shot_after[i] && g_shown >= 0) {
            char path[1024];
            fmtx(path, sizeof(path), "%s/%s-%s.png", g_out, g_mode, g_shot_name[i]);
            png_host_write(path, (const u32 *)g_fb[g_shown], 1920, 1080, 1920);
            say("selector_host: screen at %u ms (%lu flips) -> %s", ms, g_flips, path);
            g_shot_done[i] = 1;
        }
}

static int vo_pending(s32 h) { return 0; }
static int vo_close(s32 h)
{
    g_vo_closed++;
    return 0;
}

/* ---- libScePngDec ---- */
struct png_parse {
    const u8 *data;
    u32 size, reserved;
};
struct png_info {
    u32 w, h;
    u16 cs, depth;
    u32 flags;
};
struct png_dec {
    const u8 *data;
    u32 *image;
    u32 data_size, image_size;
    u16 format, alpha;
    u32 pitch;
};
static int g_png_decodes;
static int png_parse(const struct png_parse *p, struct png_info *i)
{
    u32 w, h;
    if (png_host_decode(p->data, p->size, &w, &h, NULL))
        return (int)0x80690010;
    i->w = w, i->h = h;
    return 0;
}
static int png_query(const u32 *create) { return 4096; }
static int png_create(const u32 *create, void *mem, u32 size, void **h)
{
    *h = mem;
    return mem && size >= 4096 ? 0 : (int)0x80690005;
}
static int png_decode(void *h, const struct png_dec *d, struct png_info *i)
{
    u32 w, hh;
    if (png_host_decode(d->data, d->data_size, &w, &hh, NULL) || d->format != 1 || d->pitch != w * 4 ||
        d->image_size < w * hh * 4)
        return (int)0x80690003;
    g_png_decodes++;
    return png_host_decode(d->data, d->data_size, &w, &hh, d->image) ? (int)0x80690012 : 0;
}
static int png_delete(void *h) { return 0; }

/* ---- libSceFont (badfont: the library refuses) ---- */
static int font_ok(void) { return 0; }
static int font_fail(void) { return (int)0x80460002; }
static const u64 FT_SELECTION[4] = {0x0F0F, 0, 0, 0};
static const void *font_select(int v) { return FT_SELECTION; }

void *host_dlsym(u32 handle, const char *name)
{
    static const struct {
        u32 handle;
        const char *name;
        void *fn;
    } syms[] = {
        {H_KERNEL, "sceKernelGetProcessTime", k_process_time},
        {H_KERNEL, "sceKernelGettimeofday", k_gettimeofday},
        {H_KERNEL, "sceKernelConvertUtcToLocaltime", k_utc2local},
        {H_KERNEL, "sceKernelReleaseDirectMemory", k_release_direct},
        {H_KERNEL, "sceKernelStopUnloadModule", k_stop_unload},
        {H_VIDEO, "sceVideoOutSetBufferAttribute", vo_set_attr},
        {H_VIDEO, "sceVideoOutRegisterBuffers", vo_register},
        {H_VIDEO, "sceVideoOutSetFlipRate", vo_rate},
        {H_VIDEO, "sceVideoOutClose", vo_close},
        {H_VIDEO, "sceVideoOutSubmitFlip", vo_submit},
        {H_VIDEO, "sceVideoOutIsFlipPending", vo_pending},
        {H_PAD, "scePadReadState", pad_read},
        {H_USER, "sceUserServiceGetInitialUser", user_initial},
        {H_USER, "sceUserServiceGetUserName", user_name},
        {H_USER, "sceUserServiceGetRegisteredUserIdList", user_registered},
        {H_USER, "sceUserServiceGetLoginUserIdList", user_login},
        {H_SYSSVC, "sceSystemServiceParamGetInt", param_int},
        {H_PNG, "scePngDecParseHeader", png_parse},
        {H_PNG, "scePngDecQueryMemorySize", png_query},
        {H_PNG, "scePngDecCreate", png_create},
        {H_PNG, "scePngDecDecode", png_decode},
        {H_PNG, "scePngDecDelete", png_delete},
        {H_FONT, "sceFontMemoryInit", font_ok},
        {H_FONT, "sceFontMemoryTerm", font_ok},
        {H_FONT, "sceFontCreateLibrary", font_fail},
        {H_FONT, "sceFontDestroyLibrary", font_ok},
        {H_FONT, "sceFontSupportSystemFonts", font_fail},
        {H_FONT, "sceFontCreateRenderer", font_fail},
        {H_FONT, "sceFontDestroyRenderer", font_ok},
        {H_FONT, "sceFontOpenFontSet", font_fail},
        {H_FONT, "sceFontCloseFont", font_ok},
        {H_FONT, "sceFontBindRenderer", font_fail},
        {H_FONT, "sceFontUnbindRenderer", font_ok},
        {H_FONT, "sceFontSetScalePixel", font_fail},
        {H_FONT, "sceFontSetupRenderScalePixel", font_fail},
        {H_FONT, "sceFontGetRenderCharGlyphMetrics", font_fail},
        {H_FONT, "sceFontRenderSurfaceInit", font_ok},
        {H_FONT, "sceFontRenderSurfaceSetScissor", font_ok},
        {H_FONT, "sceFontRenderCharGlyphImage", font_fail},
        {H_FONTFT, "sceFontSelectLibraryFt", font_select},
        {H_FONTFT, "sceFontSelectRendererFt", font_select},
    };
    for (u32 i = 0; i < sizeof(syms) / sizeof(syms[0]); i++)
        if (syms[i].handle == handle && str_eq(syms[i].name, name))
            return syms[i].fn;
    say("selector_host: dlsym miss %x %s", handle, name);
    return NULL;
}

u64 hle_lookup(const char *lib, const char *name)
{
    if (str_eq(name, "sceVideoOutOpen"))
        return (u64)vo_open;
    if (str_eq(name, "scePadOpen"))
        return (u64)pad_open;
    return 0;
}

/* ---- run ---- */
static void parse_shots(void)
{
    const char *s = sys_getenv("SEL_SHOTS");
    while (s && *s && g_nshots < 8) {
        u32 ms = 0;
        while (*s >= '0' && *s <= '9')
            ms = ms * 10 + (u32)(*s++ - '0');
        if (*s++ != ':')
            break;
        int n = 0;
        while (*s && *s != ',' && n < 31)
            g_shot_name[g_nshots][n++] = *s++;
        g_shot_name[g_nshots][n] = 0;
        g_shot_after[g_nshots++] = ms;
        if (*s == ',')
            s++;
    }
}

/* the core services the selector gets (const in src/ext_api.c; filled here) */
struct ghost_api g_ghost_api;

int main(int argc, char **argv)
{
    if (argc != 6) {
        say("usage: selector_host MODE SELECTOR_BIN CRC SAVES OUT_DIR");
        return 2;
    }
    g_mode = argv[1], g_art = argv[4], g_out = argv[5];
    parse_shots();
    K = (struct kfuncs){0};
    K.usleep = k_usleep, K.open = k_open, K.read = k_read, K.pread = k_pread, K.close = k_close, K.stat = k_stat;
    K.write = k_write;
    K.munmap = k_munmap, K.alloc_direct = k_alloc_direct, K.map_direct = k_map_direct;
    K.direct_size = k_direct_size, K.avail_direct = k_avail_direct, K.avail_flex = k_avail_flex;
    K.module_list = k_module_list, K.module_info = k_module_info, K.load_start_module = k_load_start_module;
    /* ghostldr.lua's context is 0x200 bytes: its last u64 points to the title
     * profiles (profile.c PROFILE_SLOT), where the selector finds their names */
    static union {
        struct boot_ctx c;
        u64 slot[64];
    } ctxmem;
    struct boot_ctx *const cx = &ctxmem.c;
    g_ctx = cx;
    const char *prof = sys_getenv("SEL_PROFILES"); /* profiles.py boot_bundle, from test_selector.py */
    if (prof && *prof) {
        s64 n = sys_file_size(prof);
        int fd = n > 0 ? sys_open(prof) : -1;
        u8 *bundle = fd >= 0 ? sys_calloc((u64)n + 8) : NULL;
        if (bundle && sys_read(fd, bundle, (u64)n) == n)
            ctxmem.slot[63] = (u64)bundle;
        if (fd >= 0)
            sys_close(fd);
    }
    cx->magic = BOOT_MAGIC, cx->size = sizeof(*cx), cx->libkernel_handle = H_KERNEL, cx->jit_memcpy = JIT_MEMCPY;
    cx->staging_size = 4u << 20, cx->staging = (u64)sys_calloc(cx->staging_size);
    cx->app_root = (u64)(str_eq(g_mode, "approot") ? "/data/test/app0" : "ghostfs");
    int apollo = str_eq(g_mode, "apollo") || str_eq(g_mode, "apollo-one");
    int settings = str_eq(g_mode, "one") || str_eq(g_mode, "empty") || str_eq(g_mode, "settings");
    cx->flags = (str_eq(g_mode, "skip") || apollo || settings ? 0 : CTX_FLAG_SELECT) | 2 |
                (str_eq(g_mode, "single") || str_eq(g_mode, "multi") || str_eq(g_mode, "ps4") || apollo || settings
                     ? CTX_FLAG_SEL_FONT_BUILTIN
                     : 0) |
                (str_eq(g_mode, "ps4") ? CTX_FLAG_PS4 : 0);
    struct ghost_api *a = &g_ghost_api;
    a->version = GHOST_API_VERSION, a->size = sizeof(*a), a->ctx = &g_ctx, a->k = &K, a->logf = logf;
    a->call_gadget6 = call_gadget6, a->host_dlsym = host_dlsym, a->hle_lookup = hle_lookup;
    a->vfs_open = vfs_open, a->vfs_read = vfs_read, a->vfs_close = vfs_close, a->vfs_stat = vfs_stat;
    a->gfs_active = gfs_active, a->gfs_mount = gfs_mount, a->gfs_umount = gfs_umount;
    a->profile_has = f_profile_has, a->title_id = f_title_id;
    a->title_user = &g_user_chosen, a->title_pick = title_pick, a->save_prescan = save_prescan;
    a->page_alloc = f_page_alloc, a->page_free = f_page_free;


    u64 size = (u64)sys_file_size(argv[2]);
    const char *in_mem = sys_getenv("SEL_BUNDLE");
    if (in_mem && *in_mem) { /* the release's ghostldr.lua: the module where it received it */
        int fd = sys_open(argv[2]);
        g_bundle_len = (size + 0x3FFF) & ~0x3FFFull;
        g_bundle_sel = sys_map(g_bundle_len);
        check(fd >= 0 && sys_read(fd, g_bundle_sel, size) == (s64)size, "the module read for the bundle");
        sys_close(fd);
        cx->sel_fd = (u64)g_bundle_sel;
        cx->flags |= CTX_FLAG_BUNDLE;
    } else {
        cx->sel_fd = (u64)sys_open(argv[2]); /* the "socket": sel.c closes it */
        g_opens++;
    }
    cx->sel_size = size;
    cx->sel_crc = sys_strtoul(argv[3]) ^ (str_eq(g_mode, "badcrc") ? 1 : 0);
    char title[16];
    /* boot.c: show when ghostfs left the choice to it (several titles: apollo),
     * with run.py --select, or for a save tool the core has (apollo-one) */
    /* and (now) with one title or none, when no title was asked for: one, empty, settings */
    int pick = sel_run((cx->flags & CTX_FLAG_SELECT) || str_eq(g_mode, "apollo") || f_profile_has(PF_SAVE_TOOL) || settings,
                       title);

    int want = str_eq(g_mode, "multi") || str_eq(g_mode, "settings") ? 2 : str_eq(g_mode, "ps4") ? 1
             : str_eq(g_mode, "skip") || str_eq(g_mode, "badcrc") ? -1 : str_eq(g_mode, "empty") ? SEL_EXIT_LOADER : 0;
    say("selector_host: %s chose %d (%.16s)", g_mode, pick, pick >= 0 ? title : "-");
    check(pick == want, "the expected title was chosen");
    if (str_eq(g_mode, "multi"))
        check(str_eq(title, "CUSA00003"), "its ID is returned");
    if (str_eq(g_mode, "ps4"))
        check(str_eq(title, "CUSA00001"), "its ID is returned");
    if (str_eq(g_mode, "settings")) {
        check(str_eq(title, "CUSA00042"), "the title installed over the network was chosen");
        check(g_net_installed && g_pages == 0, "the network installer ran, installed, and gave its memory back");
        check(g_conf_creates == 1 && g_conf_writes == 1, "GLDRCONF made once, written once (the log switch)");
        check((cx->flags & (1ull << 24)) != 0, "the log switch applied to this boot (CTX_FLAG_NO_USB_LOG)");
    }
    if (apollo) {
        check(str_eq(title, "APOL00004"), "its ID is returned");
        check(g_picks == 1 && str_eq(g_picked, "APOL00004") && g_picked_index == 0, "the save tool mounted once, by its place");
        check(g_scans == (str_eq(g_mode, "apollo") ? 2 : 1) && g_scan_cancels == (str_eq(g_mode, "apollo") ? 1 : 0),
              str_eq(g_mode, "apollo") ? "scanned, cancelled with Circle, scanned again" : "scanned once");
        check(g_scanned == (str_eq(g_mode, "apollo") ? 40 : 3), "every title scanned in the end");
        check(g_user_chosen == 0x10000001, "title_user is the chosen user");
        check(g_user_lists >= 1, "the registered users were listed");
    } else {
        check(g_user_chosen == 0 && g_picks == 0 && g_scans == 0, "no user, mount or scan for other titles");
    }
    int shown = want >= 0 || want == SEL_EXIT_LOADER;
    if (shown) {
        check(g_vo_open == 1 && g_vo_closed == 1, "VideoOut opened once and closed");
        check(g_vo_rate == 1, "flip rate 30 Hz");
        check(g_registered && g_flips > 2, "frames were flipped");
        if (!str_eq(g_mode, "empty"))
            check(g_png_decodes >= 1, "PNGs decoded through libScePngDec");
        for (int i = 0; i < g_nshots; i++)
            check(g_shot_done[i], g_shot_name[i]);
    } else {
        check(g_vo_open == 0 && g_nblocks == 0, "nothing opened or allocated");
    }
    int all_back = 1;
    u64 mib = 0;
    for (int i = 0; i < g_nblocks; i++) {
        all_back &= g_blocks[i].released && g_blocks[i].unmapped;
        mib += g_blocks[i].len >> 20;
        say("selector_host: block %d: %lu MiB type %d phys %lx-%lx%s%s", i, g_blocks[i].len >> 20, g_blocks[i].type,
            g_blocks[i].phys, g_blocks[i].phys + g_blocks[i].len, g_blocks[i].unmapped ? " unmapped" : "",
            g_blocks[i].released ? " released" : "");
    }
    check(all_back, "every direct-memory block unmapped and released");
    if (shown)
        check(g_nblocks == 2 && g_blocks[0].phys + g_blocks[0].len == RUN_END &&
                  g_blocks[1].phys + g_blocks[1].len == g_blocks[0].phys,
              "both blocks at the top of the largest free run");
    for (int i = 0; i < 3; i++)
        check(g_mod_loaded[i] == g_mod_unloaded[i], i == 0 ? "libScePngDec unloaded" : i == 1 ? "libSceFont unloaded"
                                                                                           : "libSceFontFt unloaded");
    check(g_mounts == g_umounts && mounted_now() == 0, "every save container unmounted");
    check(g_max_mounted <= 1, "at most one container mounted at a time");
    check(g_opens == g_closes, "every file closed");
    if (g_bundle_sel)
        check(g_bundle_unmapped && (s64)cx->sel_fd == -1, "the bundle's module taken and unmapped");
    int clean = 1;
    if (g_region)
        for (u64 i = 0; i < REGION_SIZE; i++)
            clean &= g_region[i] == 0;
    check(clean, "module image cleared from the code buffer");
    say("selector_host: %s: %lu MiB in %d blocks, %lu flips, %u compiler writes, %d mounts%s", g_mode, mib, g_nblocks,
        g_flips, g_compiler_writes, g_mounts, g_failed ? " FAILED" : "");
    return g_failed;
}
