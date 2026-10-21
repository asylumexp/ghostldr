/*
 * The save data family (ext/src/hle_savedata.c) run as an ordinary program,
 * for the host's own architecture, against savedata_fake.c's console.  The
 * family's file is compiled in here (SAVEDATA_HOST_TEST leaves out its
 * registration), so its static functions are called directly and its state
 * can be cleared to play a new SWRR session.  test_savedata.py builds it:
 *
 *     savedata_host MODE TMPDIR
 *     MODE: cont (stock console: real containers) | dir (dev install)
 *           | remap (/savedataN paths) | ghost (Ghost of Tsushima's calls,
 *           against the core's own savedata.c)
 *
 * Prints "savedata_host: ok|FAILED ..." lines; exits 1 if anything failed.
 */
#define SAVEDATA_HOST_TEST 1
#include "ext.h"
#include "savedata_fake.h"

/* title profiles (the core's profile.c): the core's savedata.c asks for
 * <prefix>SAVE's size; Ghost's profile gives 256 blocks */
const struct prof_hdr *profile_hdr(void)
{
    static struct prof_hdr h = {.magic = {'G', 'L', 'P', 'F'}, .save_blocks = 256};
    return &h;
}

const struct ghost_api *G;

#include "../../native/ext/src/hle_savedata.c"

/* what a PS4 returns (SCE_SAVE_DATA_ERROR_*, kernel errors), spelled out here
 * so the expectations do not follow the family's own constants */
#define W_PARAMETER ((s32)0x809F0000)
#define W_NOT_INITIALIZED ((s32)0x809F0001)
#define W_BUSY ((s32)0x809F0003)
#define W_NOT_MOUNTED ((s32)0x809F0004)
#define W_EXISTS ((s32)0x809F0007)
#define W_NOT_FOUND ((s32)0x809F0008)
#define W_NO_SPACE_FS ((s32)0x809F000A)
#define W_INTERNAL ((s32)0x809F000B)
#define W_MOUNT_FULL ((s32)0x809F000C)
#define W_BAD_MOUNTED ((s32)0x809F000D)
#define W_BROKEN ((s32)0x809F000F)
#define W_INVALID_LOGIN_USER ((s32)0x809F0011)
#define W_MEMORY_NOT_READY ((s32)0x809F0012)
#define W_ENOENT ((s32)0x80020002)
#define W_ENOSPC ((s32)0x8002001C)
#define W_EROFS ((s32)0x8002001E)

/* savedata_os.c */
void *os_opendir(const char *p);
int os_readdir(void *d, char *name, int cap);
void os_closedir(void *d);

static int g_failed;
static char g_tmp[256];

static void expect(bool ok, const char *what)
{
    char line[300];
    fmt(line, sizeof(line), "savedata_host: %s %s", ok ? "ok" : "FAILED", what);
    logf("%s", line);
    if (!ok)
        g_failed = 1;
}

#define EXPECT_RC(call, want, what)                                                                   \
    do {                                                                                              \
        s32 rc_ = (s32)(call);                                                                        \
        char w_[200];                                                                                 \
        fmt(w_, sizeof(w_), "%s (got %x, want %x)", what, (u32)rc_, (u32)(want));                     \
        expect(rc_ == (s32)(want), w_);                                                               \
    } while (0)

/* a new SWRR session: everything the family keeps in memory is gone */
static void fresh_process(void)
{
    g_init = false;
    g_cat_user = -1, g_cat = NULL, g_cat_cap = 0, g_cat_bad = false;
    memset(&g_cath, 0, sizeof(g_cath));
    g_vm = NULL;
    memset(g_ms, 0, sizeof(g_ms));
    g_ev_head = g_ev_count = 0;
    g_destruct = MODE_DESTRUCT_OFF;
    g_sdk = 0, g_prefix[0] = 0, g_save_root[0] = 0, g_title_done = false, g_user0 = -1;
    g_title_id[0] = g_title_name[0] = 0;
    g_walk = NULL, g_kf_ok = false, g_real_delete = NULL;
}

static void crash_and_restart(void)
{
    fake_crash();
    fresh_process();
    sd_initialize(NULL);
}

/* ---- the title's side ---- */

static const char *dn(const char *s) /* a SceSaveDataDirName */
{
    static char bufs[8][32];
    static u32 n;
    char *b = bufs[n++ % 8];
    memset(b, 0, 32);
    strlcpy(b, s, 32);
    return b;
}

static s32 mount2(s32 user, const char *dir, u64 blocks, u32 mode, struct sd_mount_result *r)
{
    struct sd_mount2 m;
    memset(&m, 0, sizeof(m));
    m.user = user, m.dir = dir ? dn(dir) : NULL, m.blocks = blocks, m.mode = mode;
    memset(r, 0xCC, sizeof(*r));
    return sd_mount2(&m, r);
}

static s32 umount(const char *mp)
{
    char b[16];
    memset(b, 0, sizeof(b));
    strlcpy(b, mp, sizeof(b));
    return sd_umount(b);
}

/* the title's own file calls: through remap_path, as the core's hooks do */
static int t_open(const char *path, int flags)
{
    char b[512];
    return ((int (*)(const char *, int, int))host_real("sceKernelOpen"))(remap_path(path, b, sizeof(b)), flags, 0666);
}
static int t_close(int fd) { return ((int (*)(int))host_real("sceKernelClose"))(fd); }
static s64 t_pwrite(int fd, const void *p, u64 n, s64 off)
{
    return ((s64(*)(int, const void *, u64, s64))host_real("sceKernelPwrite"))(fd, p, n, off);
}
static s64 t_pread(int fd, void *p, u64 n, s64 off)
{
    return ((s64(*)(int, void *, u64, s64))host_real("sceKernelPread"))(fd, p, n, off);
}
static int t_stat(const char *path, s64 *size)
{
    char b[512];
    u8 st[0x78];
    int rc = ((int (*)(const char *, void *))host_real("sceKernelStat"))(remap_path(path, b, sizeof(b)), st);
    if (rc == 0 && size)
        *size = *(s64 *)(st + 0x48);
    return rc;
}
static int t_mkdir(const char *path)
{
    char b[512];
    return ((int (*)(const char *, int))host_real("sceKernelMkdir"))(remap_path(path, b, sizeof(b)), 0777);
}

static s32 t_write_file(const char *path, const void *p, u64 n)
{
    int fd = t_open(path, O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0)
        return fd;
    s64 w = n ? t_pwrite(fd, p, n, 0) : 0;
    t_close(fd);
    return w == (s64)n ? 0 : (s32)w;
}

static s64 t_read_file(const char *path, void *p, u64 n)
{
    int fd = t_open(path, O_RDONLY);
    if (fd < 0)
        return fd;
    s64 r = t_pread(fd, p, n, 0);
    t_close(fd);
    return r;
}

/* committed file of a real container (what survives SWRR) */
static long cont_file(const char *cont, const char *rel, void *buf, u64 max)
{
    char p[512];
    fake_cont_dir(FAKE_USER0, cont, p, sizeof(p));
    strlcat(p, "/", sizeof(p));
    strlcat(p, rel, sizeof(p));
    return fake_read_host(p, buf, max);
}

static u32 search(s32 user, const char *pat, u32 key, u32 order, char (*names)[32], u32 max, u32 *set,
                  struct sd_param *params, struct sd_mount_info *infos, s32 *rc)
{
    struct sd_search_cond c;
    struct sd_search_result r;
    memset(&c, 0, sizeof(c));
    memset(&r, 0, sizeof(r));
    c.user = user, c.dir = pat ? dn(pat) : NULL, c.key = key, c.order = order;
    r.dirs = (char *)names, r.dirs_num = max, r.params = params, r.infos = infos;
    r.set_num = 0xEEEE;
    *rc = sd_dir_name_search(&c, &r);
    if (set)
        *set = r.set_num;
    return r.hit_num;
}

static void mount_ok(s32 user, const char *dir, u64 blocks, u32 mode, char mp[16], u32 want_status, const char *what)
{
    struct sd_mount_result r;
    s32 rc = mount2(user, dir, blocks, mode, &r);
    char w[200];
    fmt(w, sizeof(w), "%s: mount %s mode %x -> %x %s status %u", what, dir, mode, (u32)rc, r.mount_point,
        r.mount_status);
    expect(rc == 0 && r.mount_status == want_status && r.required_blocks == 0, w);
    memcpy(mp, r.mount_point, 16);
}

/* ---- stock console: real containers ---- */

static void test_errors_before_and_args(void)
{
    struct sd_mount_result r;
    EXPECT_RC(mount2(FAKE_USER0, "SAVE0", 100, MODE_CREATE | MODE_RDWR, &r), W_NOT_INITIALIZED,
              "mount before Initialize: NOT_INITIALIZED");
    struct sd_mem_setup2 ms;
    memset(&ms, 0, sizeof(ms));
    ms.user = FAKE_USER0, ms.memory_size = 0x1000;
    EXPECT_RC(sd_setup_memory2(&ms, NULL), W_NOT_INITIALIZED, "save memory setup before Initialize");
    EXPECT_RC(umount("/savedata0"), W_NOT_INITIALIZED, "unmount before Initialize");
    EXPECT_RC(sd_terminate(), W_NOT_INITIALIZED, "Terminate before Initialize");
    EXPECT_RC(sd_initialize(NULL), 0, "Initialize3(NULL)");
    EXPECT_RC(sd_initialize(NULL), 0, "Initialize again");
    EXPECT_RC(sd_mount2(NULL, &r), W_PARAMETER, "mount NULL: PARAMETER 0x809F0000, not NOT_INITIALIZED");
    EXPECT_RC(mount2(FAKE_USER0, NULL, 100, MODE_RDWR, &r), W_PARAMETER, "mount without a dirName");
    EXPECT_RC(mount2(-1, "SAVE0", 100, MODE_RDWR, &r), W_INVALID_LOGIN_USER, "mount for user -1");
    EXPECT_RC(mount2(FAKE_USER0, "", 100, MODE_RDWR, &r), W_PARAMETER, "empty dirName");
    EXPECT_RC(mount2(FAKE_USER0, ".hidden", 100, MODE_RDWR, &r), W_PARAMETER, "dirName starting with a dot");
    EXPECT_RC(mount2(FAKE_USER0, "a/b", 100, MODE_RDWR, &r), W_PARAMETER, "dirName with a slash");
    struct sd_mount2 m;
    char full[32];
    memset(full, 'A', sizeof(full));
    memset(&m, 0, sizeof(m));
    m.user = FAKE_USER0, m.dir = full, m.blocks = 100, m.mode = MODE_RDWR;
    EXPECT_RC(sd_mount2(&m, &r), W_PARAMETER, "dirName without a NUL in 32 bytes");
    EXPECT_RC(mount2(FAKE_USER0, "SAVE0", 100, 0, &r), W_PARAMETER, "mode 0");
    EXPECT_RC(mount2(FAKE_USER0, "SAVE0", 100, MODE_CREATE | MODE_CREATE2 | MODE_RDWR, &r), W_PARAMETER,
              "CREATE and CREATE2 together");
    EXPECT_RC(mount2(FAKE_USER0, "SAVE0", 32769, MODE_CREATE | MODE_RDWR, &r), W_PARAMETER,
              "creating more than 32768 blocks");
    EXPECT_RC(umount("/savedata3"), W_NOT_FOUND, "unmounting what is not mounted: NOT_FOUND");
    struct sd_mount_info mi;
    EXPECT_RC(sd_get_mount_info("/savedata0", &mi), W_NOT_MOUNTED, "GetMountInfo of nothing: NOT_MOUNTED");
    EXPECT_RC(sd_get_mount_info("/savedata0", NULL), W_PARAMETER, "GetMountInfo without info");
    expect(fake_violations() == 0 && !fake_cont_exists(FAKE_USER0, "GTESTSAVE"),
           "argument errors touch no container");
}

static void test_first_boot_create_write_commit(void)
{
    struct sd_mount_result r;
    char mp[16];
    s32 rc;
    char names[4][32];
    EXPECT_RC(mount2(FAKE_USER0, "SAVE0", 0, MODE_RDWR, &r), W_NOT_FOUND, "first boot: mount RDWR is NOT_FOUND");
    expect(search(FAKE_USER0, "SAVE%", 0, 0, names, 4, NULL, NULL, NULL, &rc) == 0 && rc == 0,
           "first boot: DirNameSearch finds nothing");
    expect(!fake_cont_exists(FAKE_USER0, "GTESTSAVE"), "reading creates no <prefix>SAVE");

    mount_ok(FAKE_USER0, "SAVE0", 100, MODE_CREATE | MODE_RDWR, mp, 1, "create");
    expect(!strcmp(mp, "/savedata0"), "the first mount is /savedata0");
    expect(fake_cont_exists(FAKE_USER0, "GTESTD001SAVE") && fake_cont_mounted(FAKE_USER0, "GTESTD001SAVE"),
           "the save is container GTESTD001SAVE, held mounted");
    expect(fake_cont_blocks(FAKE_USER0, "GTESTD001SAVE") == 104, "its container has headroom: 100 + 4 blocks");
    expect(fake_last_mode() & MODE_DESTRUCT_OFF, "real read/write mounts ask for DESTRUCT_OFF");
    struct cat_header h;
    expect(cont_file("GTESTSAVE", "catalog.bin", &h, sizeof(h)) == sizeof(h) && h.count == 1 &&
               !memcmp(h.magic, "GSDC", 4),
           "the catalogue in GTESTSAVE is committed with the new entry");

    static u8 data[40000];
    for (u32 i = 0; i < sizeof(data); i++)
        data[i] = (u8)(i * 7);
    EXPECT_RC(t_mkdir("/savedata0/sub"), 0, "the title makes a directory in its save");
    EXPECT_RC(t_write_file("/savedata0/sub/data.bin", data, sizeof(data)), 0, "the title writes a file");
    struct sd_mount_info mi;
    EXPECT_RC(sd_get_mount_info(mp, &mi), 0, "GetMountInfo");
    expect(mi.blocks == 100 && mi.free_blocks == 100 - 3, "GetMountInfo: 100 blocks, 3 used (a dir, 2 for 40000 bytes)");
    u8 back[40000];
    expect(cont_file("GTESTD001SAVE", "sub/data.bin", back, sizeof(back)) < 0, "nothing is durable before the unmount");

    int fd = t_open("/savedata0/sub/data.bin", O_RDONLY);
    expect(fd >= 0, "the title keeps a file open");
    EXPECT_RC(umount(mp), W_BUSY, "unmount with a file open: BUSY");
    EXPECT_RC(sd_get_mount_info(mp, &mi), 0, "still mounted after BUSY");
    t_close(fd);
    EXPECT_RC(umount(mp), 0, "unmount once the file is closed");
    expect(cont_file("GTESTD001SAVE", "sub/data.bin", back, sizeof(back)) == sizeof(data) &&
               !memcmp(back, data, sizeof(data)),
           "the unmount committed the title's file");
    expect(!fake_cont_mounted(FAKE_USER0, "GTESTD001SAVE") && fake_reserved() == 0,
           "the real container and its slot are released");
    EXPECT_RC(umount(mp), W_NOT_FOUND, "a second unmount: NOT_FOUND");
    s64 size;
    expect(t_stat("/savedata0/sub/data.bin", &size) == W_ENOENT, "its paths are ENOENT once unmounted");
}

static void test_param_icon_and_modes(void)
{
    struct sd_mount_result r;
    char mp[16];
    EXPECT_RC(mount2(FAKE_USER0, "SAVE0", 100, MODE_CREATE | MODE_RDWR, &r), W_EXISTS,
              "CREATE on an existing save: EXISTS");
    mount_ok(FAKE_USER0, "SAVE0", 0, MODE_CREATE2 | MODE_RDWR, mp, 0, "CREATE2 on an existing save mounts it");
    u8 back[16];
    expect(t_read_file("/savedata0/sub/data.bin", back, 16) == 16 && back[1] == 7, "the committed file is there");
    EXPECT_RC(mount2(FAKE_USER0, "SAVE0", 0, MODE_RDWR, &r), W_BUSY, "mounting it twice: BUSY");

    EXPECT_RC(sd_param_set(mp, 1, "My Title", 9), 0, "SetParam TITLE");
    EXPECT_RC(sd_param_set(mp, 2, "Chapter 2", 10), 0, "SetParam SUB_TITLE");
    u32 up = 0x77;
    EXPECT_RC(sd_param_set(mp, 4, &up, 4), 0, "SetParam USER_PARAM");
    EXPECT_RC(sd_param_set(mp, 5, &up, 4), W_PARAMETER, "SetParam MTIME: PARAMETER");
    EXPECT_RC(sd_param_set(mp, 4, &up, 2), W_PARAMETER, "SetParam USER_PARAM with 2 bytes");
    EXPECT_RC(sd_param_set("/savedata9", 1, "x", 2), W_NOT_MOUNTED, "SetParam on nothing: NOT_MOUNTED");
    struct sd_param p;
    u64 got = 0;
    EXPECT_RC(sd_param_get(mp, 0, &p, sizeof(p), &got), 0, "GetParam ALL");
    expect(got == 0x530 && !strcmp(p.title, "My Title") && !strcmp(p.sub_title, "Chapter 2") && p.user_param == 0x77 &&
               p.mtime > 0,
           "GetParam ALL returns what was set, with a time");
    char small[4];
    EXPECT_RC(sd_param_get(mp, 1, small, sizeof(small), &got), 0, "GetParam TITLE into 4 bytes");
    expect(!strcmp(small, "My ") && got == 4, "a TITLE cut to 3 characters, gotSize 4");
    EXPECT_RC(sd_param_get(mp, 0, &p, 0x52F, &got), W_PARAMETER, "GetParam ALL into 0x52F bytes");
    EXPECT_RC(sd_param_get(mp, 6, &p, sizeof(p), &got), W_PARAMETER, "GetParam type 6");
    s64 mtime = 0;
    EXPECT_RC(sd_param_get(mp, 5, &mtime, 8, NULL), 0, "GetParam MTIME without gotSize");

    u8 png[3000], icon_back[4000];
    for (u32 i = 0; i < sizeof(png); i++)
        png[i] = (u8)(i ^ 0x5A);
    struct sd_icon icon;
    memset(&icon, 0, sizeof(icon));
    icon.buf = png, icon.buf_size = sizeof(png), icon.data_size = sizeof(png);
    EXPECT_RC(sd_save_icon(mp, &icon), 0, "SaveIcon");
    icon.buf = NULL;
    EXPECT_RC(sd_save_icon(mp, &icon), W_PARAMETER, "SaveIcon without a buffer");
    memset(&icon, 0, sizeof(icon));
    icon.buf = icon_back, icon.buf_size = sizeof(icon_back);
    EXPECT_RC(sd_load_icon(mp, &icon), 0, "LoadIcon");
    expect(icon.data_size == sizeof(png) && !memcmp(icon_back, png, sizeof(png)), "LoadIcon returns the saved icon");
    EXPECT_RC(umount(mp), 0, "unmount");
    char path[512];
    fake_cont_dir(FAKE_USER0, "GTESTSAVE", path, sizeof(path));
    strlcat(path, "/icons/1.png", sizeof(path));
    expect(fake_read_host(path, icon_back, sizeof(icon_back)) == sizeof(png), "the icon is kept in GTESTSAVE/icons");

    mount_ok(FAKE_USER0, "SAVE0", 0, MODE_RDONLY, mp, 0, "read-only mount");
    EXPECT_RC(sd_param_set(mp, 1, "x", 2), W_BAD_MOUNTED, "SetParam read-only: BAD_MOUNTED");
    icon.buf = png, icon.buf_size = sizeof(png), icon.data_size = sizeof(png);
    EXPECT_RC(sd_save_icon(mp, &icon), W_BAD_MOUNTED, "SaveIcon read-only: BAD_MOUNTED");
    EXPECT_RC(sd_param_get(mp, 0, &p, sizeof(p), NULL), 0, "GetParam read-only");
    expect(!strcmp(p.title, "My Title") && p.user_param == 0x77, "param survived the unmount (catalogue)");
    EXPECT_RC(t_write_file("/savedata0/new.bin", "x", 1), W_EROFS, "the title cannot write a read-only mount");
    memset(&icon, 0, sizeof(icon));
    icon.buf = icon_back, icon.buf_size = 100;
    EXPECT_RC(sd_load_icon(mp, &icon), 0, "LoadIcon from the catalogue's icon file");
    expect(icon.data_size == sizeof(png) && !memcmp(icon_back, png, 100), "LoadIcon copies what fits, dataSize is all");
    EXPECT_RC(umount(mp), 0, "unmount read-only");
}

static void test_search(void)
{
    char mp[16];
    char names[8][32];
    struct sd_param params[8];
    struct sd_mount_info infos[8];
    u32 set;
    s32 rc;
    static const struct {
        const char *dir;
        u64 blocks;
        u32 user_param;
    } saves[] = {{"SAVE1", 300, 3}, {"save2", 96, 9}, {"OTHER", 200, 1}, {"sce_internal", 96, 0}};
    for (u32 i = 0; i < ARRAY_SIZE(saves); i++) {
        mount_ok(FAKE_USER0, saves[i].dir, saves[i].blocks, MODE_CREATE | MODE_RDWR, mp, 1, "create for search");
        sd_param_set(mp, 4, &saves[i].user_param, 4);
        umount(mp);
    }
    expect(search(FAKE_USER0, "SAVE%", 0, 0, names, 8, &set, params, infos, &rc) == 3 && set == 3 && rc == 0,
           "'SAVE%' (case blind) finds SAVE0, SAVE1, save2");
    expect(!strcmp(names[0], "SAVE0") && !strcmp(names[1], "SAVE1") && !strcmp(names[2], "save2"),
           "sorted by dirName, byte order");
    expect(params[0].user_param == 0x77 && infos[1].blocks == 300 && infos[1].free_blocks == 300,
           "params and infos follow the names");
    expect(search(FAKE_USER0, "SAVE_", 0, 0, names, 8, NULL, NULL, NULL, &rc) == 3, "'SAVE_' matches 5 characters");
    expect(search(FAKE_USER0, "S%1", 0, 0, names, 8, NULL, NULL, NULL, &rc) == 1 && !strcmp(names[0], "SAVE1"),
           "'S%1' matches the whole name only");
    expect(search(FAKE_USER0, "SAVE", 0, 0, names, 8, NULL, NULL, NULL, &rc) == 0, "'SAVE' is not a prefix match");
    expect(search(FAKE_USER0, "%", 0, 0, names, 8, NULL, NULL, NULL, &rc) == 4, "'%' finds all but sce_ names");
    expect(search(FAKE_USER0, NULL, 0, 1, names, 8, NULL, NULL, NULL, &rc) == 4 && !strcmp(names[0], "save2") &&
               !strcmp(names[3], "OTHER"),
           "no pattern, DESCENT by dirName");
    expect(search(FAKE_USER0, NULL, 2, 0, names, 8, NULL, NULL, NULL, &rc) == 4 && !strcmp(names[0], "save2") &&
               !strcmp(names[3], "SAVE1"),
           "by blocks");
    expect(search(FAKE_USER0, NULL, 1, 1, names, 8, NULL, NULL, NULL, &rc) == 4 && !strcmp(names[0], "SAVE0") &&
               !strcmp(names[1], "save2") && !strcmp(names[3], "OTHER"),
           "by userParam, DESCENT");
    expect(search(FAKE_USER0, NULL, 3, 1, names, 8, NULL, NULL, NULL, &rc) == 4 && !strcmp(names[0], "OTHER"),
           "by mtime, newest first");
    expect(search(FAKE_USER0, NULL, 5, 0, names, 8, NULL, NULL, NULL, &rc) == 4 && !strcmp(names[0], "save2") &&
               !strcmp(names[1], "SAVE0"),
           "by free blocks (SAVE0 uses 3 of its 100)");
    expect(search(FAKE_USER0, NULL, 0, 0, names, 2, &set, NULL, NULL, &rc) == 4 && set == 2, "hitNum 4, setNum 2");
    expect(search(FAKE_USER0, NULL, 0, 0, NULL, 0, &set, NULL, NULL, &rc) == 4 && set == 0, "a count-only search");
    search(FAKE_USER0, NULL, 4, 0, names, 8, NULL, NULL, NULL, &rc);
    EXPECT_RC(rc, W_PARAMETER, "search key 4: PARAMETER");
    search(FAKE_USER0, NULL, 0, 2, names, 8, NULL, NULL, NULL, &rc);
    EXPECT_RC(rc, W_PARAMETER, "search order 2: PARAMETER");
    search(-1, NULL, 0, 0, names, 8, NULL, NULL, NULL, &rc);
    EXPECT_RC(rc, W_INVALID_LOGIN_USER, "search for user -1");

    struct sd_search_cond c;
    struct sd_search_result r;
    char other[16] = "CUSA00001";
    memset(&c, 0, sizeof(c));
    memset(&r, 0, sizeof(r));
    c.user = FAKE_USER0, c.title_id = other;
    r.dirs = (char *)names, r.dirs_num = 8;
    EXPECT_RC(sd_dir_name_search(&c, &r), 0, "search another title");
    expect(r.hit_num == 0 && r.set_num == 0, "another title's saves: none");

    /* titles built before SDK 1.70 / 2.50 pass smaller result structs */
    fake_set_sdk(0x01600000);
    g_sdk = 0;
    memset(params, 0xAB, sizeof(params));
    memset(infos, 0xAB, sizeof(infos));
    expect(search(FAKE_USER0, NULL, 0, 0, names, 2, &set, params, infos, &rc) == 2 && set == 0xEEEE &&
               params[0].user_param == 0xABABABAB && infos[0].blocks == 0xABABABABABABABABull,
           "SDK 1.60: hitNum = names written, setNum, params and infos untouched");
    fake_set_sdk(0x02000000);
    g_sdk = 0;
    expect(search(FAKE_USER0, NULL, 0, 0, names, 2, &set, params, infos, &rc) == 4 && set == 2 &&
               params[1].user_param == 0x77 && infos[0].blocks == 0xABABABABABABABABull,
           "SDK 2.00: params, no infos");
    fake_set_sdk(0x08008011);
    g_sdk = 0;
}

static void test_delete_and_vanished(void)
{
    struct sd_delete d;
    struct sd_mount_result r;
    char mp[16];
    char names[8][32];
    s32 rc;
    memset(&d, 0, sizeof(d));
    d.user = FAKE_USER0, d.dir = dn("SAVE1");
    mount_ok(FAKE_USER0, "SAVE1", 0, MODE_RDWR, mp, 0, "mount SAVE1");
    EXPECT_RC(sd_delete(&d), W_BUSY, "deleting a mounted save: BUSY");
    umount(mp);
    EXPECT_RC(sd_delete(&d), 0, "Delete SAVE1");
    expect(!fake_cont_exists(FAKE_USER0, "GTESTD002SAVE"), "its container is deleted for real");
    expect(search(FAKE_USER0, "SAVE1", 0, 0, names, 8, NULL, NULL, NULL, &rc) == 0, "it is gone from searches");
    EXPECT_RC(sd_delete(&d), W_NOT_FOUND, "deleting it again: NOT_FOUND");
    d.dir = NULL;
    EXPECT_RC(sd_delete(&d), W_PARAMETER, "Delete without a dirName");
    char other[16] = "CUSA00001";
    d.dir = dn("SAVE0"), d.title_id = other;
    EXPECT_RC(sd_delete(&d), W_NOT_FOUND, "Delete of another title's save: NOT_FOUND");

    /* the user deletes a container in the PS5's settings while SWRR is off */
    fake_cont_delete(FAKE_USER0, "GTESTD003SAVE"); /* save2 */
    crash_and_restart();
    EXPECT_RC(mount2(FAKE_USER0, "save2", 0, MODE_RDWR, &r), W_NOT_FOUND, "a container deleted outside: NOT_FOUND");
    expect(search(FAKE_USER0, "save2", 0, 0, names, 8, NULL, NULL, NULL, &rc) == 0,
           "and its catalogue entry is dropped");
    mount_ok(FAKE_USER0, "save2", 96, MODE_CREATE2 | MODE_RDWR, mp, 1, "CREATE2 creates it again");
    umount(mp);

    expect(fake_cont_exists(FAKE_USER0, "GTESTD002SAVE"), "the lowest free number is used again (D002)");

    /* a crash between creating a container and committing the catalogue
     * leaves the next number taken: the next create adopts it */
    fake_cont_create(FAKE_USER0, "GTESTD003SAVE", 200);
    mount_ok(FAKE_USER0, "SAVE9", 100, MODE_CREATE | MODE_RDWR, mp, 1, "create over an orphan container");
    expect(fake_log_has("exists without a catalogue entry") && fake_cont_blocks(FAKE_USER0, "GTESTD003SAVE") == 104,
           "the orphan was deleted and made again at the new size");
    umount(mp);

    fake_set_broken(FAKE_USER0, "GTESTD003SAVE", true);
    EXPECT_RC(mount2(FAKE_USER0, "SAVE9", 0, MODE_RDWR, &r), W_BROKEN, "a broken container: BROKEN");
    fake_set_broken(FAKE_USER0, "GTESTD003SAVE", false);
    expect(fake_reserved() == 0, "failed mounts give their real slot back");
}

static void test_crash_and_quota(void)
{
    char mp[16];
    struct sd_mount_result r;
    mount_ok(FAKE_USER0, "SAVE0", 0, MODE_RDWR, mp, 0, "mount SAVE0 before a crash");
    t_write_file("/savedata0/lost.bin", "lost", 4);
    crash_and_restart();
    mount_ok(FAKE_USER0, "SAVE0", 0, MODE_RDONLY, mp, 0, "after the crash");
    s64 size;
    expect(t_stat("/savedata0/lost.bin", &size) == W_ENOENT && t_stat("/savedata0/sub/data.bin", &size) == 0,
           "uncommitted writes are lost, committed ones kept");
    umount(mp);

    mount_ok(FAKE_USER0, "QUOTA", 96, MODE_CREATE | MODE_RDWR, mp, 1, "a 96-block save");
    char cname[32];
    fmt(cname, sizeof(cname), "GTESTD%03uSAVE", g_vm[0].id);
    expect(fake_cont_blocks(FAKE_USER0, cname) == 100, "its container: 96 + 4");
    static u8 big[32768 * 47];
    memset(big, 0x42, sizeof(big));
    EXPECT_RC(t_write_file("/savedata0/a.bin", big, sizeof(big)), 0, "47 blocks");
    EXPECT_RC(t_write_file("/savedata0/b.bin", big, sizeof(big)), 0, "47 more");
    EXPECT_RC(t_write_file("/savedata0/c.bin", big, 32768 * 2), 0, "2 more: the title's 96 blocks");
    struct sd_mount_info mi;
    sd_get_mount_info(mp, &mi);
    expect(mi.free_blocks == 0, "GetMountInfo: no free blocks");
    EXPECT_RC(t_write_file("/savedata0/d.bin", big, 32768 * 3), W_ENOSPC, "past the container: ENOSPC");
    t_write_file("/savedata0/d.bin", "", 0);
    EXPECT_RC(umount(mp), 0, "a save filled to its size still commits (headroom)");

    fake_set_storage_cap(2000);
    EXPECT_RC(mount2(FAKE_USER0, "HUGE", 30000, MODE_CREATE | MODE_RDWR, &r), W_NO_SPACE_FS,
              "the console is full: NO_SPACE_FS");
    expect(r.required_blocks > 0 && r.required_blocks < 32768, "with the blocks it needs");
    fake_set_storage_cap(0);
    expect(fake_reserved() == 0, "the refused create gave its slot back");
}

static void test_slots(void)
{
    char mps[8][16];
    struct sd_mount_result r;
    char name[8];
    for (u32 i = 0; i < 6; i++) {
        fmt(name, sizeof(name), "S%u", i);
        mount_ok(FAKE_USER0, name, 96, MODE_CREATE2 | MODE_RDWR, mps[i], 1, "concurrent mounts");
    }
    expect(fake_reserved() == 6 && fake_data_mounts() == 8, "6 saves held, ghostfs down to 8 data containers");
    EXPECT_RC(mount2(FAKE_USER0, "S6", 96, MODE_CREATE2 | MODE_RDWR, &r), W_MOUNT_FULL,
              "a 7th: MOUNT_FULL 0x809F000C (not NO_PERMISSION)");
    expect(fake_max_real_mounts() <= 16 && !fake_log_has("no real slot left"),
           "never more than 16 real mounts, and the service never ran out");
    for (u32 i = 0; i < 6; i++)
        EXPECT_RC(umount(mps[i]), 0, "unmount");
    expect(fake_reserved() == 0, "every slot given back");
}

static void test_destruct_off_and_terminate(void)
{
    char mp[16];
    struct sd_mount_result r;
    fake_refuse_destruct_off(true);
    crash_and_restart();
    mount_ok(FAKE_USER0, "SAVE0", 0, MODE_RDWR, mp, 0, "a service refusing DESTRUCT_OFF");
    expect(fake_log_has("refuses DESTRUCT_OFF") && !(fake_last_mode() & 8), "mounts go on without it");
    EXPECT_RC(sd_terminate(), W_BUSY, "Terminate with a save mounted: BUSY");
    umount(mp);
    fake_refuse_destruct_off(false);
    EXPECT_RC(sd_terminate(), 0, "Terminate");
    EXPECT_RC(mount2(FAKE_USER0, "SAVE0", 0, MODE_RDWR, &r), W_NOT_INITIALIZED, "after Terminate: NOT_INITIALIZED");
    sd_initialize(NULL);
}

static void test_events_and_misc(void)
{
    char mp[16];
    struct sd_event e;
    struct sd_mount_result r;
    EXPECT_RC(sd_get_event_result(NULL, &e), W_NOT_FOUND, "no events: NOT_FOUND");
    EXPECT_RC(sd_get_event_result(NULL, NULL), W_PARAMETER, "GetEventResult without an event");
    mount_ok(FAKE_USER0, "SAVE0", 0, MODE_RDWR, mp, 0, "mount for UmountWithBackup");
    char b[16];
    memset(b, 0, sizeof(b));
    strlcpy(b, mp, sizeof(b));
    EXPECT_RC(sd_umount_backup(b), 0, "UmountWithBackup");
    EXPECT_RC(sd_get_event_result(NULL, &e), 0, "its event");
    expect(e.type == 1 && e.error_code == 0 && e.user == FAKE_USER0 && !strcmp(e.dir, "SAVE0") &&
               !strcmp(e.title_id, "CUSA99999"),
           "event UMOUNT_BACKUP for SAVE0 of CUSA99999");
    EXPECT_RC(sd_get_event_result(NULL, &e), W_NOT_FOUND, "one event only");
    struct sd_backup bk;
    memset(&bk, 0, sizeof(bk));
    bk.user = FAKE_USER0, bk.dir = dn("SAVE0");
    EXPECT_RC(sd_backup(&bk), 0, "Backup");
    EXPECT_RC(sd_get_event_result(NULL, &e), 0, "Backup's event");
    expect(e.type == 2, "event BACKUP");
    bk.dir = dn("NOPE");
    EXPECT_RC(sd_backup(&bk), W_NOT_FOUND, "Backup of nothing: NOT_FOUND");
    struct sd_check_backup cb;
    memset(&cb, 0, sizeof(cb));
    cb.user = FAKE_USER0, cb.dir = dn("SAVE0");
    EXPECT_RC(sd_check_backup(&cb), W_NOT_FOUND, "CheckBackupData: no backup kept");
    mount_ok(FAKE_USER0, "SAVE0", 0, MODE_RDONLY, mp, 0, "mounted");
    EXPECT_RC(sd_check_backup(&cb), W_BUSY, "CheckBackupData of a mounted save: BUSY");
    struct sd_restore_backup rb;
    memset(&rb, 0, sizeof(rb));
    rb.user = FAKE_USER0, rb.dir = dn("SAVE0");
    EXPECT_RC(sd_restore_backup(&rb), W_BUSY, "RestoreBackupData of a mounted save: BUSY");
    umount(mp);
    EXPECT_RC(sd_restore_backup(&rb), W_NOT_FOUND, "RestoreBackupData: no backup");

    struct sd_transfer_mount tm;
    char tid[16] = "CUSA00001";
    memset(&tm, 0, sizeof(tm));
    tm.user = FAKE_USER0, tm.title_id = tid, tm.dir = dn("SAVE0");
    EXPECT_RC(sd_transferring_mount(&tm, &r), W_NOT_FOUND, "TransferringMount: NOT_FOUND");
    tm.title_id = NULL;
    EXPECT_RC(sd_transferring_mount(&tm, &r), W_PARAMETER, "TransferringMount without a title");
    u32 progress = 0;
    EXPECT_RC(sd_get_progress(&progress), 0, "GetProgress");
    expect(progress == 0x3F800000, "progress is 1.0f");
    EXPECT_RC(sd_clear_progress(), 0, "ClearProgress");

    struct sd_mount1 m1;
    char own[16] = "CUSA99999";
    memset(&m1, 0, sizeof(m1));
    m1.user = FAKE_USER0, m1.title_id = own, m1.dir = dn("SAVE0"), m1.mode = MODE_RDONLY;
    EXPECT_RC(sd_mount1(&m1, &r), 0, "Mount (v1) with the title's own ID");
    umount(r.mount_point);
    m1.title_id = tid;
    EXPECT_RC(sd_mount1(&m1, &r), W_NOT_FOUND, "Mount (v1) of another title: NOT_FOUND");
    expect(fake_violations() == 0, "nothing reached the real service with a foreign name");
}

static void test_users(void)
{
    char mp[16];
    char names[8][32];
    s32 rc;
    mount_ok(FAKE_USER1, "SAVE0", 100, MODE_CREATE | MODE_RDWR, mp, 1, "a second user creates SAVE0 too");
    umount(mp);
    expect(fake_cont_exists(FAKE_USER1, "GTESTD001SAVE") && fake_cont_exists(FAKE_USER1, "GTESTSAVE"),
           "in that user's own containers");
    expect(search(FAKE_USER1, NULL, 0, 0, names, 8, NULL, NULL, NULL, &rc) == 1, "the second user has one save");
    expect(search(FAKE_USER0, NULL, 0, 0, names, 8, NULL, NULL, NULL, &rc) >= 4, "the first user still has theirs");
}

static void fill(u8 *p, u64 n, u32 seed)
{
    for (u64 i = 0; i < n; i++)
        p[i] = (u8)((i * 31 + seed) >> 3);
}

static void test_memory(void)
{
    static u8 buf[0x87800], back[0x87800];
    struct sd_mem_setup2 s;
    struct sd_mem_setup_result res;
    struct sd_mem_get2 g;
    struct sd_mem_set2 st;
    struct sd_mem_sync sy;
    struct sd_mem_data d[2];
    struct sd_param p, pb;
    memset(&g, 0, sizeof(g));
    memset(&st, 0, sizeof(st));
    memset(&sy, 0, sizeof(sy));
    memset(d, 0, sizeof(d));
    g.user = st.user = sy.user = FAKE_USER0;
    d[0].buf = back, d[0].buf_size = 0x1000;
    g.data = d;
    EXPECT_RC(sd_get_memory2(&g), W_MEMORY_NOT_READY, "Get2 before Setup2: MEMORY_NOT_READY");
    st.data = d, st.data_num = 1;
    EXPECT_RC(sd_set_memory2(&st), W_MEMORY_NOT_READY, "Set2 before Setup2");
    EXPECT_RC(sd_sync_memory(&sy), W_MEMORY_NOT_READY, "Sync before Setup2");
    EXPECT_RC(sd_setup_memory2(NULL, &res), W_PARAMETER, "Setup2 NULL");

    memset(&s, 0, sizeof(s));
    memset(&p, 0, sizeof(p));
    strlcpy(p.title, "Memory title", sizeof(p.title));
    s.user = FAKE_USER0, s.memory_size = 0x87800, s.init_param = &p;
    memset(&res, 0xCC, sizeof(res));
    EXPECT_RC(sd_setup_memory2(&s, &res), 0, "Setup2 slot 0, 0x87800 bytes");
    expect(res.existed == 0, "existedMemorySize 0 for a new one");
    u8 hdr[0x530];
    expect(cont_file("GTESTSAVE", "sdmemory_0.bin", buf, sizeof(buf)) == 0x87800 &&
               cont_file("GTESTSAVE", "sdmemory_0.param", hdr, sizeof(hdr)) == 0x530,
           "GTESTSAVE/sdmemory_0.bin (0x87800) and .param (0x530), committed");
    EXPECT_RC(sd_setup_memory2(&s, &res), W_BUSY, "a second Setup2: BUSY");

    fill(buf, sizeof(buf), 1);
    d[0].buf = buf, d[0].buf_size = 0x10000, d[0].offset = 0;
    d[1].buf = buf + 0x80000, d[1].buf_size = 0x7800, d[1].offset = 0x80000;
    memset(&pb, 0, sizeof(pb));
    strlcpy(pb.title, "Saved", sizeof(pb.title));
    st.data = d, st.data_num = 2, st.param = &pb;
    EXPECT_RC(sd_set_memory2(&st), 0, "Set2 with two blocks and a param");
    expect(cont_file("GTESTSAVE", "sdmemory_0.bin", back, sizeof(back)) == 0x87800 && !memcmp(back, buf, 0x10000) &&
               !memcmp(back + 0x80000, buf + 0x80000, 0x7800),
           "Set2 is committed when it returns");
    d[0].offset = 0x80000, d[0].buf_size = 0x8000;
    st.data_num = 1, st.param = NULL;
    EXPECT_RC(sd_set_memory2(&st), W_PARAMETER, "Set2 past the memory's size: PARAMETER");
    memset(back, 0xEE, sizeof(back));
    d[0].buf = back, d[0].buf_size = 0x9000, d[0].offset = 0x80000;
    g.data = d, g.param = &pb;
    EXPECT_RC(sd_get_memory2(&g), 0, "Get2 across the end");
    expect(!memcmp(back, buf + 0x80000, 0x7800) && back[0x7800] == 0 && back[0x8FFF] == 0 &&
               !strcmp(pb.title, "Saved"),
           "Get2: the data, zeros past the end, the param");
    struct sd_icon ic;
    u8 icbuf[64];
    memset(&ic, 0, sizeof(ic));
    ic.buf = icbuf, ic.buf_size = sizeof(icbuf), ic.data_size = 99;
    g.icon = &ic;
    EXPECT_RC(sd_get_memory2(&g), 0, "Get2 with an icon");
    expect(ic.data_size == 0, "no icon: dataSize 0");
    g.icon = NULL;
    sy.slot = 0;
    EXPECT_RC(sd_sync_memory(&sy), 0, "Sync");
    struct sd_event e;
    expect(sd_get_event_result(NULL, &e) == 0 && e.type == 3 && !strcmp(e.dir, "sce_sdmemory"),
           "Sync's event: SAVE_DATA_MEMORY_SYNC, sce_sdmemory");

    s.slot = 3, s.memory_size = 0x2000, s.init_param = NULL;
    EXPECT_RC(sd_setup_memory2(&s, &res), 0, "Setup2 slot 3");
    expect(cont_file("GTESTSAVE", "sdmemory_3.bin", back, 0x3000) == 0x2000, "slot 3 has its own files");
    s.slot = 16;
    EXPECT_RC(sd_setup_memory2(&s, &res), W_PARAMETER, "slot 16: PARAMETER");

    /* the next SWRR session */
    crash_and_restart();
    s.slot = 0, s.memory_size = 0x87800;
    EXPECT_RC(sd_setup_memory2(&s, &res), 0, "Setup2 again in a new session");
    expect(res.existed == 0x87800, "existedMemorySize is the size kept");
    d[0].buf = back, d[0].buf_size = 0x87800, d[0].offset = 0;
    g.data = d, g.param = NULL;
    EXPECT_RC(sd_get_memory2(&g), 0, "Get2 all");
    expect(!memcmp(back, buf, 0x10000) && !memcmp(back + 0x80000, buf + 0x80000, 0x7800), "the memory survived");

    /* too big for GTESTSAVE (8 MiB): its own container */
    s.slot = 5, s.memory_size = 12u << 20;
    EXPECT_RC(sd_setup_memory2(&s, &res), 0, "Setup2 slot 5, 12 MiB");
    expect(fake_cont_exists(FAKE_USER0, "GTESTM05SAVE") && cont_file("GTESTM05SAVE", "sdmemory_5.bin", back, 16) == 16,
           "it lives in GTESTM05SAVE");
    st.slot = 5, st.data = d, st.data_num = 1, st.param = NULL;
    d[0].buf = buf, d[0].buf_size = 0x1000, d[0].offset = (12 << 20) - 0x1000;
    EXPECT_RC(sd_set_memory2(&st), 0, "Set2 at its end");
    crash_and_restart();
    EXPECT_RC(sd_setup_memory2(&s, &res), 0, "Setup2 slot 5 in a new session");
    expect(res.existed == 12u << 20, "found in its container again");
    g.slot = 5, d[0].buf = back;
    EXPECT_RC(sd_get_memory2(&g), 0, "Get2 slot 5");
    expect(!memcmp(back, buf, 0x1000), "its data");

    /* the SDK 1.x calls: slot 0 */
    crash_and_restart();
    EXPECT_RC(sd_setup_memory(FAKE_USER0, 0x87800, &p), 0, "SetupSaveDataMemory (v1)");
    memset(back, 0, 0x100);
    EXPECT_RC(sd_get_memory(FAKE_USER0, back, 0x100, 0), 0, "GetSaveDataMemory (v1)");
    expect(!memcmp(back, buf, 0x100), "v1 reads slot 0");
    EXPECT_RC(sd_set_memory(FAKE_USER0, buf, 0x100, 0x87800), W_PARAMETER, "v1 write past the end");
}

static void test_title_store(void)
{
    u8 rec[300], back[400];
    fill(rec, sizeof(rec), 9);
    EXPECT_RC(title_store_read("trophy.dat", back, sizeof(back)), TITLE_STORE_E_NOT_FOUND, "no record yet");
    EXPECT_RC(title_store_write("trophy.dat", rec, sizeof(rec)), 0, "title_store_write");
    EXPECT_RC(title_store_read("trophy.dat", back, sizeof(back)), sizeof(rec), "title_store_read");
    expect(!memcmp(back, rec, sizeof(rec)), "the record read back");
    EXPECT_RC(title_store_read("trophy.dat", back, 10), 10, "a short read");
    EXPECT_RC(title_store_size("trophy.dat"), sizeof(rec), "title_store_size");
    expect(cont_file("GTESTSAVE", "store/trophy.dat", back, sizeof(back)) == sizeof(rec),
           "kept in GTESTSAVE/store, committed");
    EXPECT_RC(title_store_write("../escape", rec, 1), TITLE_STORE_E_INVALID, "a bad name");
    EXPECT_RC(title_store_write(".hidden", rec, 1), TITLE_STORE_E_INVALID, "a name starting with a dot");
    EXPECT_RC(title_store_write("x", rec, TITLE_STORE_MAX + 1), TITLE_STORE_E_INVALID, "too large");
    EXPECT_RC(title_store_write("empty", NULL, 0), 0, "an empty record");
    EXPECT_RC(title_store_size("empty"), 0, "its size");
    EXPECT_RC(title_store_remove("trophy.dat"), 0, "title_store_remove");
    EXPECT_RC(title_store_read("trophy.dat", back, sizeof(back)), TITLE_STORE_E_NOT_FOUND, "removed");
    EXPECT_RC(title_store_remove("never"), 0, "removing nothing");
    /* it works whatever the title did with libSceSaveData */
    sd_terminate();
    EXPECT_RC(title_store_write("t2", rec, 4), 0, "title store without Initialize");
    sd_initialize(NULL);
}

static void test_hooks(void)
{
    struct sd_save_info list[16];
    s32 n = sd_list(-1, list, 16);
    expect(n >= 4, "sd_list: the initial user's saves");
    bool found = false;
    for (s32 i = 0; i < n && i < 16; i++)
        found |= !strcmp(list[i].dir, "SAVE0") && list[i].blocks == 100 && list[i].has_icon &&
                 list[i].user_param == 0x77;
    expect(found, "sd_list: SAVE0 with its size, icon and userParam");
    expect(sd_list(FAKE_USER1, NULL, 0) == 1, "sd_list counts another user's");
    expect(sd_exists("SAVE0") && !sd_exists("NOPE") && !sd_exists(NULL), "sd_exists");
    struct sd_param p;
    EXPECT_RC(sd_get_param("SAVE0", &p), 0, "sd_get_param");
    expect(!strcmp(p.title, "My Title") && p.mtime > 0, "sd_get_param: the title and time");
    EXPECT_RC(sd_get_param("NOPE", &p), W_NOT_FOUND, "sd_get_param of nothing");
    expect(sd_mtime("SAVE0") == p.mtime && sd_mtime("NOPE") == -1, "sd_mtime");
    struct sd_icon ic;
    struct sd_mount_info info;
    u8 png[8000];
    memset(&ic, 0, sizeof(ic));
    ic.buf = png, ic.buf_size = sizeof(png);
    EXPECT_RC(savedata_query(FAKE_USER0, "SAVE0", NULL, &ic, &info), 0, "savedata_query");
    expect(ic.data_size == 3000 && png[1] == (u8)(1 ^ 0x5A) && info.blocks == 100, "savedata_query: icon and sizes");
}

/* S1: the only names the real service may ever see; the library's policy */
static void test_gate_and_table(void)
{
    expect(real_name_ok("GTESTSAVE") && real_name_ok("GTESTD001SAVE") && real_name_ok("GTESTD999SAVE") &&
               real_name_ok("GTESTM05SAVE"),
           "real names: <prefix>SAVE, <prefix>D<nnn>SAVE, <prefix>M<ss>SAVE");
    static const char *const bad[] = {"SLUS-20268", "SLES-50366", "GFSINDEX",      "GHOSTIDX",    "GTEST000",
                                      "GTEST012",   "GTESTD01SAVE", "GTESTD0001SAVE", "GTESTDxyzSAVE", "GTESTM5SAVE",
                                      "OTHERSAVE",  "GTESTSAVEX", "GTEST",         "GTESTD001SAV", ""};
    bool none = true;
    for (u32 i = 0; i < ARRAY_SIZE(bad); i++)
        none &= !real_name_ok(bad[i]);
    expect(none, "refused: SWRR's saves, ghostfs's index and data containers, anything else");
    expect(k_libs[0].flags == EXT_LIB_NO_HOST && (u32)k_libs[0].stub_ret == 0x809F000B && !strcmp(k_libs[0].name, SD),
           "libSceSaveData has no host fallback; what is not here returns INTERNAL");
    bool table = true;
    for (u32 i = 0; i < ARRAY_SIZE(k_hle); i++)
        table &= !strcmp(k_hle[i].lib, SD) && k_hle[i].fn && k_hle[i].name && !strncmp(k_hle[i].name, "sceSaveData", 11);
    expect(table, "every entry is a libSceSaveData function");
}

static void run_cont(void)
{
    fake_init(g_tmp, true, "GTEST", 0x08008011);
    sd_init();
    test_gate_and_table();
    test_errors_before_and_args();
    test_first_boot_create_write_commit();
    test_param_icon_and_modes();
    test_search();
    test_delete_and_vanished();
    test_crash_and_quota();
    test_slots();
    test_destruct_off_and_terminate();
    test_events_and_misc();
    test_users();
    test_memory();
    test_title_store();
    test_hooks();
    expect(fake_violations() == 0, "no real call ever named SWRR's save, GFSINDEX or a data container");
    expect(fake_swrr_touched() == 0, "no path ever reached SWRR's /savedata0");
    expect(fake_bad_paths() == 0, "no path outside the test");
    expect(fake_max_real_mounts() <= 16, "at most 16 real mounts at once");
}

/* ---- dev install: directories ---- */

static void run_dir(void)
{
    fake_init(g_tmp, false, "", 0x08008011);
    sd_init();
    sd_initialize(NULL);
    char mp[16], path[512];
    struct sd_mount_result r;
    char names[20][32];
    s32 rc;
    char root[300];
    fmt(root, sizeof(root), "%s/savedata", fake_root());

    /* Ghost's dev saves from before per-user directories */
    static u8 mem[0x87800], back[0x87800];
    fill(mem, sizeof(mem), 3);
    fake_mkdir_host(root);
    fmt(path, sizeof(path), "%s/sdmemory_0.bin", root);
    fake_write_host(path, mem, sizeof(mem));
    struct sd_mem_setup2 s;
    struct sd_mem_setup_result res;
    memset(&s, 0, sizeof(s));
    s.user = FAKE_USER0, s.memory_size = 0x87800;
    EXPECT_RC(sd_setup_memory2(&s, &res), 0, "dev: Setup2 with a save root sdmemory_0.bin");
    expect(res.existed == 0x87800, "dev: the save root's own file is used");
    struct sd_mem_data d;
    struct sd_mem_get2 g;
    memset(&d, 0, sizeof(d));
    memset(&g, 0, sizeof(g));
    d.buf = back, d.buf_size = sizeof(back);
    g.user = FAKE_USER0, g.data = &d;
    EXPECT_RC(sd_get_memory2(&g), 0, "dev: Get2");
    expect(!memcmp(back, mem, sizeof(mem)), "dev: Ghost's old save memory reads unchanged");
    struct sd_mem_set2 st;
    memset(&st, 0, sizeof(st));
    st.user = FAKE_USER0, st.data = &d, st.data_num = 1;
    fill(back, 0x100, 8);
    d.buf_size = 0x100;
    EXPECT_RC(sd_set_memory2(&st), 0, "dev: Set2");
    expect(fake_read_host(path, mem, 0x100) == 0x100 && !memcmp(mem, back, 0x100), "dev: written to the same file");
    s.user = FAKE_USER1;
    EXPECT_RC(sd_setup_memory2(&s, &res), W_BUSY, "dev: slot 0 is set up (per process)");

    mount_ok(FAKE_USER0, "SAVE0", 100, MODE_CREATE | MODE_RDWR, mp, 1, "dev: create");
    fmt(path, sizeof(path), "%s/%08x/d/SAVE0", root, FAKE_USER0);
    expect(fake_host_exists(path), "dev: the save is <save root>/<user>/d/SAVE0");
    EXPECT_RC(t_write_file("/savedata0/f.txt", "hello", 5), 0, "dev: the title writes into it");
    strlcat(path, "/f.txt", sizeof(path));
    expect(fake_host_exists(path), "dev: the file is in the save's directory");
    EXPECT_RC(umount(mp), 0, "dev: unmount");
    fmt(path, sizeof(path), "%s/%08x/catalog.bin", root, FAKE_USER0);
    expect(fake_host_exists(path), "dev: the catalogue is per user");
    EXPECT_RC(mount2(FAKE_USER0, "SAVE0", 100, MODE_CREATE | MODE_RDWR, &r), W_EXISTS, "dev: EXISTS");

    char mps[17][16];
    char name[8];
    for (u32 i = 0; i < 16; i++) {
        fmt(name, sizeof(name), "M%u", i);
        mount_ok(FAKE_USER0, name, 96, MODE_CREATE2 | MODE_RDWR, mps[i], 1, "dev: 16 mounts");
    }
    expect(!strcmp(mps[15], "/savedata15") && !strcmp(mps[10], "/savedata10"), "dev: /savedata10../savedata15");
    EXPECT_RC(t_write_file("/savedata15/x", "x", 1), 0, "dev: a two-digit mount point remaps");
    EXPECT_RC(mount2(FAKE_USER0, "M16", 96, MODE_CREATE2 | MODE_RDWR, &r), W_MOUNT_FULL, "dev: a 17th: MOUNT_FULL");
    for (u32 i = 0; i < 16; i++)
        umount(mps[i]);
    expect(search(FAKE_USER0, "M%", 0, 0, names, 20, NULL, NULL, NULL, &rc) == 16, "dev: search");

    struct sd_delete del;
    memset(&del, 0, sizeof(del));
    del.user = FAKE_USER0, del.dir = dn("SAVE0");
    EXPECT_RC(sd_delete(&del), 0, "dev: Delete");
    fmt(path, sizeof(path), "%s/%08x/d/SAVE0", root, FAKE_USER0);
    expect(!fake_host_exists(path), "dev: its directory is removed");

    u8 rec[16];
    EXPECT_RC(title_store_write("rec", "0123456789abcdef", 16), 0, "dev: title store");
    fmt(path, sizeof(path), "%s/store/rec", root);
    expect(fake_read_host(path, rec, 16) == 16, "dev: the record is in <save root>/store");
    expect(fake_bad_paths() == 0 && fake_swrr_touched() == 0, "dev: no path outside the test or into SWRR's save");
}

/* ---- /savedataN paths ---- */

static void run_remap(void)
{
    fake_init(g_tmp, true, "GTEST", 0x08008011);
    sd_init();
    sd_initialize(NULL);
    char b[512];
    const char *p = remap_path("/savedata0/x", b, sizeof(b));
    expect(!strcmp(p, "/app0/ghostldr-no-savedata/savedata0/x"), "no mounts: /savedata0/x is refused");
    s64 size;
    EXPECT_RC(t_stat("/savedata0/x", &size), W_ENOENT, "and gives ENOENT");
    EXPECT_RC(t_open("/savedata0", O_RDONLY), W_ENOENT, "/savedata0 itself is ENOENT");
    EXPECT_RC(t_mkdir("/savedata0/new"), W_ENOENT, "mkdir under it is ENOENT");
    EXPECT_RC(t_write_file("/savedata0/new.bin", "x", 1), W_ENOENT, "creating a file under it is ENOENT");
    char mp[16], mp2[16];
    mount_ok(FAKE_USER0, "R0", 96, MODE_CREATE | MODE_RDWR, mp, 1, "mount for remap");
    mount_ok(FAKE_USER0, "R1", 96, MODE_CREATE | MODE_RDWR, mp2, 1, "a second mount");
    expect(!strcmp(mp2, "/savedata1"), "the second mount is /savedata1");
    char real[16];
    memcpy(real, g_vm[0].real, 16);
    p = remap_path("/savedata0", b, sizeof(b));
    expect(!strcmp(p, real) && strcmp(real, "/savedata0") != 0, "/savedata0 -> the real mount point (not SWRR's)");
    char want[64];
    fmt(want, sizeof(want), "%s/a/b", real);
    expect(!strcmp(remap_path("/savedata0/a/b", b, sizeof(b)), want), "/savedata0/a/b");
    static const char *const refused[] = {"/savedata00/x", "/savedata1x", "/savedata16/x", "/savedata2/x",
                                          "/savedata0/../savedata0/x", "/savedata0/..", "/savedatax", "/savedata"};
    for (u32 i = 0; i < ARRAY_SIZE(refused); i++) {
        char w[100];
        fmt(w, sizeof(w), "%s is refused", refused[i]);
        expect(!strncmp(remap_path(refused[i], b, sizeof(b)), "/app0/ghostldr-no-savedata/", 27), w);
    }
    fmt(want, sizeof(want), "%s/..x", real);
    expect(!strcmp(remap_path("/savedata0/..x", b, sizeof(b)), want), "/savedata0/..x is a file in the save");
    expect(!strcmp(remap_path("/app0/eboot.bin", b, sizeof(b)), "/app0/eboot.bin"), "/app0 paths are not the hook's");
    expect(!strcmp(remap_path("/temp0/x", b, sizeof(b)), "/temp0/x"), "/temp0 paths pass");
    char small[12];
    expect(!strncmp(remap_path("/savedata0/a-long-name", small, sizeof(small)), "/app0/ghos", 10),
           "a path that does not fit the buffer is refused, not cut");
    EXPECT_RC(t_write_file("/savedata1/in1", "1", 1), 0, "the title writes through /savedata1");
    umount(mp);
    expect(!strncmp(remap_path("/savedata0/x", b, sizeof(b)), "/app0/ghostldr-no-savedata/", 27),
           "unmounted: refused again");
    expect(!strcmp(remap_path("/savedata1", b, sizeof(b)), g_vm[1].real), "the other mount still maps");
    umount(mp2);
    expect(fake_swrr_touched() == 0 && fake_violations() == 0, "remap: SWRR's /savedata0 never reached");
}

/* ---- Ghost of Tsushima: its calls, against the core's own savedata.c ---- */

/* the core's savedata.c (linked beside): same struct layouts */
s64 hle_sdm_setup2(const void *s, u64 *result);
s64 hle_sdm_get2(void *g);
s64 hle_sdm_set2(const void *s);
s64 hle_save_dir_search(const void *c, void *r);

#define GHOST_MEM 0x87800

/* what run.log shows at a boot: Setup2, Get2, DirNameSearch 'infamousss%',
 * then an autosave of six Set2 calls (the first with a param) and Sync */
static void ghost_boot(bool core, u64 *existed, u8 *mem_out, bool autosave, u32 seed)
{
    static u8 chunk[0x16000];
    struct sd_param p;
    memset(&p, 0, sizeof(p));
    strlcpy(p.title, "Ghost of Tsushima", sizeof(p.title));
    strlcpy(p.detail, "Act I", sizeof(p.detail));
    struct sd_mem_setup2 s;
    memset(&s, 0, sizeof(s));
    s.user = FAKE_USER0, s.memory_size = GHOST_MEM, s.icon_memory_size = 0, s.init_param = &p;
    struct sd_mem_setup_result res;
    memset(&res, 0, sizeof(res));
    if (!core)
        sd_initialize(NULL);
    EXPECT_RC(core ? hle_sdm_setup2(&s, &res.existed) : sd_setup_memory2(&s, &res), 0, "Ghost: Setup2");
    *existed = res.existed;
    struct sd_mem_data d;
    struct sd_mem_get2 g;
    struct sd_param pg;
    memset(&d, 0, sizeof(d));
    memset(&g, 0, sizeof(g));
    d.buf = mem_out, d.buf_size = GHOST_MEM;
    g.user = FAKE_USER0, g.data = &d, g.param = &pg;
    EXPECT_RC(core ? hle_sdm_get2(&g) : sd_get_memory2(&g), 0, "Ghost: Get2");
    struct sd_search_cond c;
    struct sd_search_result r;
    char names[1][32];
    memset(&c, 0, sizeof(c));
    memset(&r, 0, sizeof(r));
    c.user = FAKE_USER0, c.dir = dn("infamousss%");
    r.dirs = (char *)names, r.dirs_num = 1;
    EXPECT_RC(core ? hle_save_dir_search(&c, &r) : sd_dir_name_search(&c, &r), 0, "Ghost: DirNameSearch");
    expect(r.hit_num == 0, "Ghost: 'infamousss%' finds nothing");
    if (!autosave)
        return;
    struct sd_mem_set2 st;
    memset(&st, 0, sizeof(st));
    st.user = FAKE_USER0, st.data = &d, st.data_num = 1;
    for (u32 i = 0; i < 6; i++) {
        fill(chunk, sizeof(chunk), seed + i);
        d.buf = chunk, d.buf_size = sizeof(chunk), d.offset = (s64)i * (s64)sizeof(chunk);
        st.param = i == 0 ? &p : NULL;
        EXPECT_RC(core ? hle_sdm_set2(&st) : sd_set_memory2(&st), 0, "Ghost: Set2");
    }
    if (!core) {
        struct sd_mem_sync sy;
        memset(&sy, 0, sizeof(sy));
        sy.user = FAKE_USER0;
        EXPECT_RC(sd_sync_memory(&sy), 0, "Ghost: Sync");
    }
}

static void expected_mem(u8 *mem, u32 seed)
{
    memset(mem, 0, GHOST_MEM);
    for (u32 i = 0; i < 6; i++)
        fill(mem + i * 0x16000, 0x16000, seed + i);
}

static bool ghostsave_files(const char *expect_list)
{
    char dir[512], name[256], list[512];
    list[0] = 0;
    fake_cont_dir(FAKE_USER0, "GHOSTSAVE", dir, sizeof(dir));
    void *dh = os_opendir(dir);
    char sorted[16][64];
    u32 n = 0;
    while (dh && n < 16 && os_readdir(dh, name, sizeof(name)))
        if (name[0] != '.' && strcmp(name, "sce_sys") != 0)
            strlcpy(sorted[n++], name, 64);
    if (dh)
        os_closedir(dh);
    for (u32 i = 0; i < n; i++)
        for (u32 j = i + 1; j < n; j++)
            if (strcmp(sorted[j], sorted[i]) < 0) {
                char t[64];
                memcpy(t, sorted[i], 64), memcpy(sorted[i], sorted[j], 64), memcpy(sorted[j], t, 64);
            }
    for (u32 i = 0; i < n; i++) {
        strlcat(list, sorted[i], sizeof(list));
        strlcat(list, " ", sizeof(list));
    }
    logf("GHOSTSAVE holds: %s", list);
    return !strcmp(list, expect_list);
}

static void run_ghost(void)
{
    static u8 mem[GHOST_MEM], want[GHOST_MEM], core_bin[GHOST_MEM], ext_bin[GHOST_MEM];
    u8 core_param[0x530], ext_param[0x530];
    char root[300];
    u64 existed;

    /* the core's savedata.c: a first boot, an autosave, a second boot */
    fmt(root, sizeof(root), "%s/core", g_tmp);
    fake_init(root, true, "GHOST", 0x08008011);
    ghost_boot(true, &existed, mem, true, 10);
    expect(existed == 0, "core: a first boot");
    expected_mem(want, 10);
    expect(cont_file("GHOSTSAVE", "sdmemory_0.bin", core_bin, GHOST_MEM) == GHOST_MEM &&
               !memcmp(core_bin, want, GHOST_MEM),
           "core: GHOSTSAVE/sdmemory_0.bin holds the autosave");
    cont_file("GHOSTSAVE", "sdmemory_0.param", core_param, sizeof(core_param));

    /* the extension, on a fresh console: the same calls */
    fmt(root, sizeof(root), "%s/ext", g_tmp);
    fake_init(root, true, "GHOST", 0x08008011);
    fresh_process();
    sd_init();
    struct sd_mem_get2 g0;
    memset(&g0, 0, sizeof(g0));
    sd_initialize(NULL);
    EXPECT_RC(sd_get_memory2(&g0), W_MEMORY_NOT_READY, "ext: Get2 before Setup2 is MEMORY_NOT_READY (as on a PS4)");
    ghost_boot(false, &existed, mem, true, 10);
    expect(existed == 0, "ext: a first boot");
    expect(cont_file("GHOSTSAVE", "sdmemory_0.bin", ext_bin, GHOST_MEM) == GHOST_MEM && !memcmp(ext_bin, want, GHOST_MEM),
           "ext: GHOSTSAVE/sdmemory_0.bin is byte for byte the core's");
    cont_file("GHOSTSAVE", "sdmemory_0.param", ext_param, sizeof(ext_param));
    expect(!memcmp(core_param, ext_param, 0x508) && !memcmp(core_param + 0x510, ext_param + 0x510, 0x20),
           "ext: sdmemory_0.param is the core's (but for the time it sets)");
    expect(ghostsave_files("sdmemory_0.bin sdmemory_0.param "), "ext: GHOSTSAVE holds nothing new");
    expect(!strcmp(fake_mounted_names(), "GHOSTSAVE GHOSTSAVE GHOSTSAVE GHOSTSAVE GHOSTSAVE GHOSTSAVE GHOSTSAVE "
                                         "GHOSTSAVE GHOSTSAVE "),
           "ext: GHOSTSAVE is the only container it mounted");
    expect(fake_violations() == 0, "ext: no other name reached the service");

    /* the extension reads the core's save, and the core reads the extension's */
    fmt(root, sizeof(root), "%s/core", g_tmp);
    fake_init(root, true, "GHOST", 0x08008011); /* wipes: write the core's state again */
    ghost_boot(true, &existed, mem, true, 20);
    fresh_process();
    sd_init();
    ghost_boot(false, &existed, mem, false, 0);
    expected_mem(want, 20);
    expect(existed == GHOST_MEM && !memcmp(mem, want, GHOST_MEM), "ext: the core's GHOSTSAVE progress loads unchanged");
    crash_and_restart();
    ghost_boot(false, &existed, mem, true, 30); /* the next session plays on and autosaves */
    expect(existed == GHOST_MEM && !memcmp(mem, want, GHOST_MEM), "ext: and again in the next session");
    crash_and_restart();
    ghost_boot(false, &existed, mem, false, 0);
    expected_mem(want, 30);
    expect(existed == GHOST_MEM && !memcmp(mem, want, GHOST_MEM), "ext: its own autosave loads in the next session");
    ghost_boot(true, &existed, mem, false, 0);
    expect(existed == GHOST_MEM && !memcmp(mem, want, GHOST_MEM), "core: reads the extension's GHOSTSAVE unchanged");
    expect(ghostsave_files("sdmemory_0.bin sdmemory_0.param "), "GHOSTSAVE's layout is unchanged");
    expect(fake_violations() == 0 && fake_swrr_touched() == 0, "Ghost: SWRR's own save untouched");
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        G = &g_fake_api;
        logf("usage: savedata_host cont|dir|remap|ghost TMPDIR");
        return 2;
    }
    G = &g_fake_api;
    strlcpy(g_tmp, argv[2], sizeof(g_tmp));
    const char *mode = argv[1];
    if (!strcmp(mode, "cont"))
        run_cont();
    else if (!strcmp(mode, "dir"))
        run_dir();
    else if (!strcmp(mode, "remap"))
        run_remap();
    else if (!strcmp(mode, "ghost"))
        run_ghost();
    else
        return 2;
    expect(fake_open_fds() == 0, "no file left open");
    return g_failed;
}
