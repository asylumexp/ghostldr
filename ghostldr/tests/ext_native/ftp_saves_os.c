/*
 * The fake console for ftp_saves_host.c: libkernel's file calls on a
 * temporary folder (ROOT) and the save and user services, as a game sees
 * them (hle_savedata.c, "other titles' saves"):
 *
 *   ROOT/saves/<user hex>/<TITLE>/<DIR>/   a save's files
 *   ROOT/fp/<TITLE>                        that title's fingerprint (64 hex)
 *   ROOT/<anything else>                   SWRR's sandbox (ghostldr_usb, ...)
 *
 * The host is SWRR (CUSA03474): its saves mount with no fingerprint (the
 * core's gfs_mount_user), another title's only read-only with its
 * fingerprint (sceSaveDataMount), and a save whose folder has a file
 * .other_account is refused as another account's (0x809F000F).  A mount
 * binds the save at the lowest free /savedataN of 16.  DirNameSearch
 * answers for one exact title ID (or the host's own, with none).
 *
 *     ftp_saves_host ROOT USERS SCRIPT
 *
 * USERS: "10000001:Alice,10000002:Bob" (the first is the initial user).
 * SCRIPT: one command per line (ftp_saves_host.c ftps_cmd).  Prints "OUT"
 * lines for the commands, log lines, "search <user> <title>" per question,
 * "VIOLATION ..." for anything /saves must never do (a write, a read-write
 * mount, an unmount with files open in it, a mount left at the end), and
 * "peak <n>" (the most saves mounted at once).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void ftps_cmd(const char *op, const char *arg);

#define HOST "CUSA03474"
#define SLOTS 16
#define NFD 1024

static char g_root[1024];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int used;
    char host[1100]; /* the save's folder */
} g_slot[SLOTS];
static int g_bound, g_peak;
static struct {
    int used, slot;
    DIR *dir;
} g_fd[NFD];
static int32_t g_users[4] = {-1, -1, -1, -1};
static char g_names[4][32];

void os_log(const char *line)
{
    pthread_mutex_lock(&g_mu);
    printf("%s\n", line);
    fflush(stdout);
    pthread_mutex_unlock(&g_mu);
}
void os_out(const char *line)
{
    char b[4200];
    snprintf(b, sizeof(b), "OUT %s", line);
    os_log(b);
}
int os_usleep(uint32_t us) { return usleep(us); }
void *os_calloc(uint64_t n) { return calloc(1, n); }
void os_free(void *p) { free(p); }
int os_vfmt(char *buf, uint64_t size, const char *f, va_list ap) { return vsnprintf(buf, size, f, ap); }
long os_clock_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
int os_thread(void *(*fn)(void *), void *arg)
{
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0)
        return -1;
    pthread_detach(t);
    return 0;
}

static void violation(const char *what, const char *path)
{
    char b[1300];
    snprintf(b, sizeof(b), "VIOLATION %s: %s", what, path);
    os_log(b);
}

/* a console path on this machine: -1 when it names no bound save */
static int map(const char *p, char *out, size_t size, int *slot)
{
    *slot = -1;
    if (strncmp(p, "/savedata", 9) == 0 && p[9] >= '0' && p[9] <= '9') {
        char *end;
        long n = strtol(p + 9, &end, 10);
        if (n >= SLOTS || (*end && *end != '/') || !g_slot[n].used)
            return -1;
        *slot = (int)n;
        snprintf(out, size, "%s%s", g_slot[n].host, end);
        return 0;
    }
    snprintf(out, size, "%s%s", g_root, p);
    return 0;
}

#define SCE(e) ((int32_t)(0x80020000u | (uint32_t)(e)))

static int k_open(const char *p, int flags, int mode)
{
    char host[1200];
    int slot;
    if (map(p, host, sizeof(host), &slot) != 0)
        return SCE(ENOENT);
    char saves[1100];
    snprintf(saves, sizeof(saves), "%s/saves/", g_root);
    if ((flags & 3) && (slot >= 0 || strncmp(host, saves, strlen(saves)) == 0))
        violation("a write into a save", p);
    int hflags = (flags & 3) == 1 ? O_WRONLY : (flags & 3) == 2 ? O_RDWR : O_RDONLY;
    if (flags & 0x200)
        hflags |= O_CREAT;
    if (flags & 0x400)
        hflags |= O_TRUNC;
    if (flags & 0x8)
        hflags |= O_APPEND;
    pthread_mutex_lock(&g_mu);
    int fd = open(host, hflags, 0666);
    if (fd >= 0 && fd < NFD) {
        g_fd[fd].used = 1, g_fd[fd].slot = slot, g_fd[fd].dir = NULL;
    }
    pthread_mutex_unlock(&g_mu);
    return fd < 0 ? SCE(errno) : fd;
}

static int k_close(int fd)
{
    pthread_mutex_lock(&g_mu);
    if (fd >= 0 && fd < NFD) {
        if (g_fd[fd].dir)
            closedir(g_fd[fd].dir);
        close(fd);
        g_fd[fd].used = 0, g_fd[fd].dir = NULL;
    }
    pthread_mutex_unlock(&g_mu);
    return 0;
}

static int64_t k_read(int fd, void *b, uint64_t n)
{
    int64_t r = read(fd, b, n);
    return r < 0 ? SCE(errno) : r;
}

static int64_t k_pread(int fd, void *b, uint64_t n, int64_t off)
{
    int64_t r = pread(fd, b, n, off);
    return r < 0 ? SCE(errno) : r;
}

static int64_t k_write(int fd, const void *b, uint64_t n)
{
    int64_t r = write(fd, b, n);
    return r < 0 ? SCE(errno) : r;
}

/* the path calls that change something: never in a save itself */
static int host_path(const char *p, char *host, size_t size)
{
    int slot;
    char saves[1100];
    snprintf(saves, sizeof(saves), "%s/saves/", g_root);
    if (map(p, host, size, &slot) != 0)
        return SCE(ENOENT);
    if (slot >= 0 || strncmp(host, saves, strlen(saves)) == 0) {
        violation("a change in a save", p);
        return SCE(EROFS);
    }
    return 0;
}
static int k_mkdir(const char *p, int mode)
{
    char h[1200];
    int e = host_path(p, h, sizeof(h));
    return e ? e : mkdir(h, 0777) == 0 ? 0 : SCE(errno);
}
static int k_rmdir(const char *p)
{
    char h[1200];
    int e = host_path(p, h, sizeof(h));
    return e ? e : rmdir(h) == 0 ? 0 : SCE(errno);
}
static int k_unlink(const char *p)
{
    char h[1200];
    int e = host_path(p, h, sizeof(h));
    return e ? e : unlink(h) == 0 ? 0 : SCE(errno);
}
static int k_rename(const char *a, const char *b)
{
    char h[1200], g[1200];
    int e = host_path(a, h, sizeof(h));
    if (!e)
        e = host_path(b, g, sizeof(g));
    return e ? e : rename(h, g) == 0 ? 0 : SCE(errno);
}

/* ---- save_race.c's, faked: a race always wins here ---- */

static int g_races;

static void sh(const char *fmt, ...)
{
    char cmd[4000];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    if (system(cmd) != 0)
        fprintf(stderr, "failed: %s\n", cmd);
}

/* fp NULL: a race copy (read-only, any of the user's saves, six a session); else with the fingerprint */
int32_t os_race_copy(int32_t user, const char *tid, const char *dir, const char *fp, const char *dst)
{
    char save[1200], p[1300], want[128] = "";
    struct stat st;
    snprintf(save, sizeof(save), "%s/saves/%08x/%.9s/%s", g_root, (uint32_t)user, tid, dir);
    if (stat(save, &st) != 0)
        return (int32_t)0x809F0008;
    snprintf(p, sizeof(p), "%s/.other_account", save);
    if (fp) {
        if (stat(p, &st) == 0)
            return (int32_t)0x809F000F;
        snprintf(p, sizeof(p), "%s/fp/%.9s", g_root, tid);
        FILE *f = fopen(p, "r");
        if (f) {
            if (!fgets(want, sizeof(want), f))
                want[0] = 0;
            fclose(f);
        }
        want[strcspn(want, "\r\n")] = 0;
        if (strcmp(fp, want) != 0)
            return (int32_t)0x809F0006;
    } else if (++g_races > 6) {
        return (int32_t)0x809F000C;
    }
    snprintf(p, sizeof(p), "%s %08x %s/%s -> %s", fp ? "fp_copy" : "race_copy", (uint32_t)user, tid, dir, dst);
    os_log(p);
    sh("rm -rf '%s%s' && mkdir -p '%s%s' && cp -R '%s/' '%s%s/' && rm -f '%s%s/.other_account'%s", g_root, dst, g_root,
       dst, save, g_root, dst, g_root, dst, fp ? "" : "");
    if (!fp)
        sh("chmod -R a-w '%s%s'", g_root, dst);
    return 0;
}

/* the copy's files become the save's (sce_sys left alone); prune: what the copy lacks goes too */
int32_t os_race_write(int32_t user, const char *tid, const char *dir, const char *src, int prune)
{
    char save[1200], line[1400];
    snprintf(save, sizeof(save), "%s/saves/%08x/%.9s/%s", g_root, (uint32_t)user, tid, dir);
    snprintf(line, sizeof(line), "race_write %08x %s/%s <- %s", (uint32_t)user, tid, dir, src);
    os_log(line);
    if (prune)
        sh("cd '%s' && find . -mindepth 1 -maxdepth 1 ! -name sce_sys ! -name .other_account -exec rm -rf {} +",
           save);
    sh("cd '%s%s' && tar cf - --exclude ./sce_sys . | (cd '%s' && tar xf -)", g_root, src, save);
    return 0;
}

static uint64_t digest_walk(const char *dir, uint64_t h)
{
    struct dirent **list;
    int n = scandir(dir, &list, NULL, alphasort);
    for (int i = 0; i < n; i++) {
        const char *nm = list[i]->d_name;
        char p[1400];
        snprintf(p, sizeof(p), "%s/%s", dir, nm);
        if (strcmp(nm, ".") && strcmp(nm, "..")) {
            for (const char *c = nm; *c; c++)
                h = (h ^ (uint8_t)*c) * 0x100000001B3ull;
            struct stat st;
            if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
                h = digest_walk(p, h);
            } else {
                FILE *f = fopen(p, "rb");
                for (int ch; f && (ch = fgetc(f)) != EOF;)
                    h = (h ^ (uint8_t)ch) * 0x100000001B3ull;
                if (f)
                    fclose(f);
            }
        }
        free(list[i]);
    }
    if (n >= 0)
        free(list);
    return h;
}

uint64_t os_digest(const char *dir)
{
    char p[1200];
    snprintf(p, sizeof(p), "%s%s", g_root, dir);
    return digest_walk(p, 0xcbf29ce484222325ull);
}

static int k_stat(const char *p, uint8_t *out)
{
    char host[1200];
    int slot;
    struct stat st;
    if (map(p, host, sizeof(host), &slot) != 0 || stat(host, &st) != 0)
        return SCE(ENOENT);
    memset(out, 0, 0x78);
    uint16_t mode = (S_ISDIR(st.st_mode) ? 0x4000 : 0x8000) | 0777;
    int64_t mtime = st.st_mtime, size = st.st_size;
    memcpy(out + 8, &mode, 2);
    memcpy(out + 0x28, &mtime, 8);
    memcpy(out + 0x48, &size, 8);
    return 0;
}

/* FreeBSD dirents: u32 fileno, u16 reclen, u8 type, u8 namlen, name, NUL, padded to 4 */
static int k_getdents(int fd, uint8_t *buf, int n)
{
    if (fd < 0 || fd >= NFD || !g_fd[fd].used)
        return SCE(EBADF);
    if (!g_fd[fd].dir) {
        int dfd = dup(fd);
        g_fd[fd].dir = dfd >= 0 ? fdopendir(dfd) : NULL;
        if (!g_fd[fd].dir)
            return SCE(ENOTDIR);
    }
    int len = 0;
    struct dirent *e;
    long pos = telldir(g_fd[fd].dir);
    while ((e = readdir(g_fd[fd].dir))) {
        int nl = (int)strlen(e->d_name), rl = (8 + nl + 1 + 3) & ~3;
        if (len + rl > n) {
            seekdir(g_fd[fd].dir, pos);
            break;
        }
        uint32_t ino = 1;
        uint16_t r16 = (uint16_t)rl;
        memset(buf + len, 0, (size_t)rl);
        memcpy(buf + len, &ino, 4);
        memcpy(buf + len + 4, &r16, 2);
        buf[len + 6] = e->d_type == DT_DIR ? 4 : 8, buf[len + 7] = (uint8_t)nl;
        memcpy(buf + len + 8, e->d_name, (size_t)nl);
        len += rl;
        pos = telldir(g_fd[fd].dir);
    }
    return len;
}

static int k_appinfo(int pid, uint8_t *info)
{
    memset(info, 0, 0x80);
    memcpy(info + 0x10, HOST, 9);
    return 0;
}

/* ---- the save service ---- */

struct mount1 {
    int32_t user;
    uint32_t pad;
    const char *title_id, *dir;
    const char *fingerprint;
    uint64_t blocks;
    uint32_t mode;
    uint8_t reserved[32];
};
struct cond {
    int32_t user;
    uint32_t pad;
    const char *title_id, *dir;
    uint32_t key, order;
    uint8_t reserved[32];
};
struct result {
    uint32_t hit_num, pad;
    char *dirs;
    uint32_t dirs_num, set_num;
    uint8_t *params; /* 0x530 each, mtime at 0x508 */
    void *infos;
    uint8_t reserved[12];
};

static int dir_cmp(const void *a, const void *b) { return strcmp(a, b); }

static int32_t k_search(const struct cond *c, struct result *r)
{
    char d[1200], line[128];
    const char *tid = c->title_id ? c->title_id : HOST;
    snprintf(line, sizeof(line), "search %08x %s", (uint32_t)c->user, c->title_id ? tid : "(own)");
    os_log(line);
    snprintf(d, sizeof(d), "%s/saves/%08x/%.9s", g_root, (uint32_t)c->user, tid);
    char names[256][32];
    uint32_t n = 0;
    DIR *dir = opendir(d);
    struct dirent *e;
    while (dir && (e = readdir(dir)) && n < 256)
        if (e->d_name[0] != '.' && e->d_type == DT_DIR)
            snprintf(names[n++], 32, "%s", e->d_name);
    if (dir)
        closedir(dir);
    qsort(names, n, 32, dir_cmp);
    r->hit_num = n, r->set_num = n < r->dirs_num ? n : r->dirs_num;
    for (uint32_t i = 0; i < r->set_num; i++) {
        memcpy(r->dirs + 32 * i, names[i], 32);
        if (r->params) {
            struct stat st;
            char p[1300];
            snprintf(p, sizeof(p), "%s/%s", d, names[i]);
            int64_t mt = stat(p, &st) == 0 ? st.st_mtime : 0;
            memset(r->params + 0x530 * i, 0, 0x530);
            memcpy(r->params + 0x530 * i + 0x508, &mt, 8);
        }
    }
    return 0;
}

static int32_t bind_save(int32_t user, const char *tid, const char *dir, char mp[16])
{
    char host[1200], p[1300];
    struct stat st;
    snprintf(host, sizeof(host), "%s/saves/%08x/%.9s/%s", g_root, (uint32_t)user, tid, dir);
    if (stat(host, &st) != 0)
        return (int32_t)0x809F0008;
    snprintf(p, sizeof(p), "%s/.other_account", host);
    if (stat(p, &st) == 0)
        return (int32_t)0x809F000F;
    pthread_mutex_lock(&g_mu);
    int free_slot = -1;
    for (int i = SLOTS - 1; i >= 0; i--) {
        if (g_slot[i].used && strcmp(g_slot[i].host, host) == 0) {
            pthread_mutex_unlock(&g_mu);
            return (int32_t)0x809F0003; /* busy: mounted already */
        }
        if (!g_slot[i].used)
            free_slot = i;
    }
    if (free_slot < 0) {
        pthread_mutex_unlock(&g_mu);
        return (int32_t)0x809F000C;
    }
    g_slot[free_slot].used = 1;
    snprintf(g_slot[free_slot].host, sizeof(g_slot[free_slot].host), "%s", host);
    if (++g_bound > g_peak)
        g_peak = g_bound;
    pthread_mutex_unlock(&g_mu);
    memset(mp, 0, 16);
    snprintf(mp, 16, "/savedata%d", free_slot);
    return 0;
}

int os_gfs_mount(int32_t user, const char *dir, int mode, char mp[16])
{
    if (mode != 1)
        violation("a read-write mount", dir);
    return bind_save(user < 0 ? g_users[0] : user, HOST, dir, mp);
}

static int32_t k_mount(const struct mount1 *m, char *res)
{
    char want[128] = "", p[1200];
    if (m->mode != 1)
        violation("a read-write mount", m->dir);
    if (strncmp(m->title_id, HOST, 9) == 0)
        violation("the host's own save by title ID", m->dir);
    snprintf(p, sizeof(p), "%s/fp/%.9s", g_root, m->title_id);
    FILE *f = fopen(p, "r");
    if (f) {
        if (!fgets(want, sizeof(want), f))
            want[0] = 0;
        fclose(f);
    }
    want[strcspn(want, "\r\n")] = 0;
    if (!m->fingerprint || !want[0] || strcmp(m->fingerprint, want) != 0)
        return (int32_t)0x809F0006;
    return bind_save(m->user, m->title_id, m->dir, res);
}

static int32_t k_umount(const char *mp)
{
    int n = atoi(mp + 9);
    pthread_mutex_lock(&g_mu);
    if (n < 0 || n >= SLOTS || !g_slot[n].used) {
        pthread_mutex_unlock(&g_mu);
        return (int32_t)0x809F0004;
    }
    for (int fd = 0; fd < NFD; fd++)
        if (g_fd[fd].used && g_fd[fd].slot == n) {
            pthread_mutex_unlock(&g_mu);
            violation("an unmount with files open in it", mp);
            return (int32_t)0x809F0003;
        }
    g_slot[n].used = 0;
    g_bound--;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

/* ---- the user service ---- */

static int32_t k_login(int32_t *list)
{
    memcpy(list, g_users, sizeof(g_users));
    return 0;
}
static int32_t k_initial(int32_t *u)
{
    *u = g_users[0];
    return 0;
}
static int32_t k_name(int32_t user, char *name, uint64_t size)
{
    for (int i = 0; i < 4; i++)
        if (g_users[i] == user && g_users[i] != -1) {
            snprintf(name, size, "%s", g_names[i]);
            return 0;
        }
    return (int32_t)0x80960009;
}

static int k_getpid(void) { return 77; }

static const struct {
    const char *name;
    void *fn;
} k_syms[] = {
    {"sceKernelOpen", (void *)k_open},
    {"sceKernelClose", (void *)k_close},
    {"sceKernelRead", (void *)k_read},
    {"sceKernelStat", (void *)k_stat},
    {"sceKernelWrite", (void *)k_write},
    {"sceKernelPread", (void *)k_pread},
    {"sceKernelMkdir", (void *)k_mkdir},
    {"sceKernelRmdir", (void *)k_rmdir},
    {"sceKernelUnlink", (void *)k_unlink},
    {"sceKernelRename", (void *)k_rename},
    {"sceKernelGetdents", (void *)k_getdents},
    {"sceKernelGetAppInfo", (void *)k_appinfo},
    {"sceSaveDataDirNameSearch", (void *)k_search},
    {"sceSaveDataMount", (void *)k_mount},
    {"sceSaveDataUmount", (void *)k_umount},
    {"sceUserServiceGetLoginUserIdList", (void *)k_login},
    {"sceUserServiceGetInitialUser", (void *)k_initial},
    {"sceUserServiceGetUserName", (void *)k_name},
    {"usleep", (void *)os_usleep},
    {"getpid", (void *)k_getpid},
};

void *os_real(const char *name)
{
    for (size_t i = 0; i < sizeof(k_syms) / sizeof(k_syms[0]); i++)
        if (strcmp(k_syms[i].name, name) == 0)
            return k_syms[i].fn;
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s ROOT USERS SCRIPT\n", argv[0]);
        return 2;
    }
    snprintf(g_root, sizeof(g_root), "%s", argv[1]);
    char users[512];
    snprintf(users, sizeof(users), "%s", argv[2]);
    int nu = 0;
    for (char *tok = strtok(users, ","); tok && nu < 4; tok = strtok(NULL, ",")) {
        char *colon = strchr(tok, ':');
        g_users[nu] = (int32_t)strtoul(tok, NULL, 16);
        snprintf(g_names[nu], sizeof(g_names[nu]), "%s", colon ? colon + 1 : "");
        nu++;
    }
    FILE *f = fopen(argv[3], "r");
    if (!f)
        return 2;
    char line[1200];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#')
            continue;
        char *sp = strchr(line, ' ');
        if (sp)
            *sp = 0;
        ftps_cmd(line, sp ? sp + 1 : "");
    }
    fclose(f);
    for (int i = 0; i < SLOTS; i++)
        if (g_slot[i].used)
            violation("a save left mounted", g_slot[i].host);
    printf("peak %d\n", g_peak);
    return 0;
}
