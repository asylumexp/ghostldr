/*
 * Save mounting as jailbroken-console homebrew does it (Apollo Save Tool and
 * its kind), on top of what a game may do on a stock console.
 *
 * Such tools load /system/priv/lib/libSceFsInternalForVsh.sprx (a system
 * library no game may load), unseal a save's key with /dev/sbl_srv (system
 * only) and call sceFsMountSaveData on the save image
 * /user/home/<user>/savedata/<TITLE>/sdimg_<DIR>; they list saves from the
 * system's /system_data/savedata/<user>/db/user/savedata.db.  None of that is
 * reachable from SWRR.  What a game may do instead (hle_savedata.c, "other
 * titles' saves"): list any title's saves, delete them, and mount one
 * read-only with that title's fingerprint if it is the user's own.  So:
 *
 *   - libSceFsInternalForVsh is a virtual module here (EXT_LIB_VIRTUAL).
 *     sceFsMountSaveData mounts the save named by the image path through the
 *     real save service, read-only, with the fingerprint from a keystone file
 *     the user has (savedata_fingerprint), and the tool's mount path becomes
 *     that mount.  Writes in it fail (EROFS), as the system has it.  The
 *     unsealed key is never used.  Creating a save image is refused: a save a
 *     game creates gets the creator's keystone, so the real game would refuse
 *     it.
 *   - savedata.db reads as a SQLite file made from the real service's lists
 *     (DirNameSearch for every title a keystone or cheat file, or
 *     /data/ghostldr/titles.txt, names: the service answers only for exact
 *     title IDs), in the
 *     title's /temp0 directory; what the tool writes to it is thrown away at
 *     the next read.
 *     Without a keystone, or when the system refuses the save (another
 *     account's), the tool's mount path is a read-only copy made through a
 *     mount ShellCore binds before refusing it (save_race.c), once per save
 *     and session.
 *   - Writing (a tool's "apply cheats", "import decrypted files", "copy
 *     to HDD" into an existing save, "resign"): with the title's keystone,
 *     the tool's mount path is a writable copy of the save, made through the
 *     read-only fingerprint mount.  At its unmount, if the tool changed it,
 *     the copy is written into the save through a read-write mount the
 *     service binds and then refuses (save_race_write): the save keeps its
 *     owner and keystone, and the system commits it when SWRR closes.
 *     What the tool writes into sce_sys is not written (the system owns it).
 *   - libkernel_sys.sprx, which such tools load for statfs, is a virtual
 *     module with nothing in it: loading a second libkernel into SWRR is not
 *     something to try.
 *
 * The jailbreak such tools do first (libjbc), the key service /dev/sbl_srv
 * they unseal keys with and the sealed key files they read are hle_jbc.c's
 * virtual kernel and devices: Apollo's released eboot.bin runs unmodified.
 */
#include "kmod.h"
#include "savedata_hooks.h"
#include "sqlite_min.h"
#include "sys_title.h"

#define LIB "libSceFsInternalForVsh"
#define E_ACCES_ ((s32)0x8002000D)

/* ---- mount paths the tool chose -> the real read-only mounts ---- */

#define NALIAS 16
static struct {
    bool used;
    char path[160]; /* the tool's mount path, no trailing '/' */
    char mp[16];    /* "/savedataN", or */
    char host[128]; /* a read-only copy (save_race.c): a host path, or */
    bool rw;        /* a writable copy, written into the save at the unmount when it changed: */
    u64 digest;     /* its files when the tool got it (save_tree_digest) */
    s32 user;
    char tid[16], dir[32], fp[65];
} g_alias[NALIAS];

/* writable copies made this session (their save may be held for its commit: they are what it holds) */
#define NRW 8
static struct {
    char key[48];
    char host[128];
} g_rw[NRW];

/* copies made this session, reused: each made one mount that stays */
#define NCOPY 8
static struct {
    char key[48];
    char host[128];
} g_copy[NCOPY];
static volatile int g_alias_lock;

static void norm(char *out, size_t size, const char *in)
{
    strlcpy(out, in, size);
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/')
        out[--n] = 0;
}

/* "/user/home/%08x/savedata/<TITLE>/sdimg_<DIR>" */
static bool parse_volume(const char *v, s32 *user, char tid[16], char dir[32])
{
    static const char pre[] = "/user/home/";
    if (!v || strncmp(v, pre, sizeof(pre) - 1) != 0)
        return false;
    const char *p = v + sizeof(pre) - 1;
    u32 u = 0;
    for (int k = 0; k < 8; k++, p++) {
        char c = *p;
        u32 d = c >= '0' && c <= '9' ? (u32)(c - '0') : c >= 'a' && c <= 'f' ? (u32)(c - 'a' + 10) : 99;
        if (d > 15)
            return false;
        u = u << 4 | d;
    }
    if (strncmp(p, "/savedata/", 10) != 0)
        return false;
    p += 10;
    const char *slash = strrchr_c(p, '/');
    if (!slash || slash - p < 9 || slash - p > 15 || strncmp(slash + 1, "sdimg_", 6) != 0 || !slash[7])
        return false;
    memcpy(tid, p, (size_t)(slash - p));
    tid[slash - p] = 0;
    strlcpy(dir, slash + 7, 32);
    *user = (s32)u;
    return true;
}

/* sceFsInit{Mount,Umount,CreatePfs}SaveDataOpt: the options are never read
 * here, so nothing is written either.  Their sizes differ (a tool's
 * UmountSaveDataOpt can be a single byte on its stack, next to its
 * arguments), and clearing more than the caller's struct clobbers them. */
static s32 vsh_init_opt(void *opt)
{
    (void)opt;
    return 0;
}

/* ---- what cannot work on a stock console, said on screen once per session ---- */
static volatile int g_said_usb, g_said_trophies, g_said_new;

static bool trophy_path(const char *p)
{
    static const char home[] = "/user/home/";
    return p && strncmp(p, home, sizeof(home) - 1) == 0 && strlen(p) > sizeof(home) - 1 + 8 &&
           strncmp(p + sizeof(home) - 1 + 8, "/trophy/", 8) == 0;
}

static void say_trophies(void)
{
    ov_notice(&g_said_trophies, "Trophies are system data: SWRR can't read or change them on a stock console.");
}

/* a save tool tried to make a save image for a title that has none (its
 * sealed key or image): only the save service makes saves, and one it made
 * for that title would carry SWRR's keystone, which the game refuses.  For
 * the families that see the attempt first (the sealed-key device). */
void fsvsh_new_save_refused(void) HIDDEN;
void fsvsh_new_save_refused(void)
{
    ov_notice(&g_said_new, "A new save can't be made for a game on a stock console.\n"
                           "Start the game, save once, then copy over that save.");
}

/* ---- a game's keystone without a computer ----
 * A writable mount of another game's save needs that game's fingerprint
 * (its keystone's bytes 0x20-0x3f, the same in every save of the game), and
 * SWRR may not read a mounted save's sce_sys/keystone.  ghostldr learns a
 * missing one and keeps it in /data/ghostldr/keystones/<TITLE>.keystone:
 * from a decrypted save on a USB drive that carries sce_sys/keystone
 * (Apollo's PS4/APOLLO layout: exports made on a jailbroken console, or by
 * Apollo under ghostldr, which gets the keystone into its copies once it is
 * known).  SWRR may not read it from the save itself, even while the
 * system holds it bound during the H-RACE window (EACCES on a 13.60 PS5).
 * Without one the user is told on screen what is missing. */
static s32 (*k_open)(const char *, int, int);
static s64 (*k_write)(int, const void *, u64);
static s32 (*k_close)(int);
static s32 (*k_mkdir)(const char *, int);
static bool kfuncs(void);

static bool keystone_ok(const u8 *ks, s64 n) { return n == 0x60 && memcmp(ks, "keystone", 8) == 0; }

/* write ks as tid's keystone file in the title's /data (path: a host path, made if missing) */
static bool put_file(const char *path, const void *data, u64 n)
{
    char dir[320];
    strlcpy(dir, path, sizeof(dir));
    for (char *c = dir + 1; *c; c++)
        if (*c == '/')
            *c = 0, k_mkdir(dir, 0777), *c = '/';
    int fd = k_open(path, 0x601 /* O_WRONLY|O_CREAT|O_TRUNC */, 0666);
    bool ok = fd >= 0 && k_write(fd, data, n) == (s64)n;
    if (fd >= 0)
        k_close(fd);
    return ok;
}

static bool keystone_store(const char *tid, const u8 ks[0x60], const char *from)
{
    char rel[96], buf[320];
    if (!keystone_ok(ks, 0x60) || !kfuncs())
        return false;
    fmt(rel, sizeof(rel), "/data/ghostldr/keystones/%.9s.keystone", tid);
    bool ok = put_file(title_path(rel, buf, sizeof(buf)), ks, 0x60);
    logf("fsvsh: %.9s's keystone, from %s -> %s%s", tid, from, rel, ok ? "" : ": could not be written");
    return ok;
}

/* TITLE_ID of a param.sfo */
static bool sfo_title_id(const u8 *sfo, s64 n, char out[16])
{
    if (n < 0x14 || memcmp(sfo, "\0PSF", 4) != 0)
        return false;
    u32 koff = *(const u32 *)(sfo + 8), doff = *(const u32 *)(sfo + 12), cnt = *(const u32 *)(sfo + 16);
    for (u32 i = 0; i < cnt && 0x14 + (i + 1) * 16 <= (u64)n; i++) {
        const u8 *e = sfo + 0x14 + i * 16;
        u32 k = koff + *(const u16 *)e, len = *(const u32 *)(e + 4), d = doff + *(const u32 *)(e + 12);
        if (k + 9 > (u64)n || memcmp(sfo + k, "TITLE_ID", 9) != 0 || d + 9 > (u64)n || len < 9)
            continue;
        memcpy(out, sfo + d, 9);
        out[9] = 0;
        return true;
    }
    return false;
}

/* a small file of the title's view of the file system: its length, or < 0 */
static s64 read_small(const char *path, void *buf, u64 cap)
{
    char b[320];
    int fd = vfs_open(title_path(path, b, sizeof(b)), O_RDONLY, 0);
    if (fd < 0)
        return -1;
    s64 n = vfs_read(fd, buf, cap);
    vfs_close(fd);
    return n;
}

/* tid's keystone from a decrypted save on a USB drive (PS4/APOLLO/<any>/sce_sys/{param.sfo,keystone}) */
static bool keystone_from_usb(const char *tid)
{
    u8 *dents = page_alloc(8192), *sfo = page_alloc(0x4000);
    bool found = false;
    for (u32 usb = 0; usb < 2 && dents && sfo && !found; usb++) {
        char dir[64], b[320];
        fmt(dir, sizeof(dir), "/mnt/usb%u/PS4/APOLLO", usb);
        int fd = vfs_open(title_path(dir, b, sizeof(b)), 0x20000 /* O_DIRECTORY */, 0);
        if (fd < 0)
            continue;
        for (int got; !found && (got = vfs_getdents(fd, dents, 8192)) > 0;)
            for (int off = 0; !found && off + 8 <= got;) {
                u16 reclen = *(u16 *)(dents + off + 4);
                u8 type = dents[off + 6], nlen = dents[off + 7];
                if (!reclen)
                    break;
                char name[256], p[384], id[16];
                memcpy(name, dents + off + 8, nlen);
                name[nlen] = 0;
                off += reclen;
                if (type != 4 || name[0] == '.')
                    continue;
                fmt(p, sizeof(p), "%s/%s/sce_sys/param.sfo", dir, name);
                s64 n = read_small(p, sfo, 0x4000);
                if (!sfo_title_id(sfo, n, id) || strncmp(id, tid, 9) != 0)
                    continue;
                u8 ks[0x61];
                fmt(p, sizeof(p), "%s/%s/sce_sys/keystone", dir, name);
                if (keystone_ok(ks, read_small(p, ks, sizeof(ks)))) {
                    fmt(p, sizeof(p), "a decrypted save on USB (PS4/APOLLO/%s)", name);
                    found = keystone_store(tid, ks, p);
                }
            }
        vfs_close(fd);
    }
    if (dents)
        page_free(dents, 8192);
    if (sfo)
        page_free(sfo, 0x4000);
    return found;
}

/* once per title and session: what is missing */
static void say_no_keystone(const char *tid)
{
    static char said[8][10];
    static volatile int lock;
    spin_lock(&lock);
    u32 i = 0;
    while (i < 8 && said[i][0] && strncmp(said[i], tid, 9) != 0)
        i++;
    bool now = i < 8 && !said[i][0];
    if (now)
        memcpy(said[i], tid, 9), said[i][9] = 0;
    spin_unlock(&lock);
    if (!now)
        return;
    char line[256];
    fmt(line, sizeof(line), "No keystone for %.9s: its saves open read-only, changes can't be saved.\n"
                            "A decrypted save of it with sce_sys/keystone in PS4/APOLLO on USB adds it,\n"
                            "or %.9s.keystone in /data/ghostldr/keystones (ghostldr's FTP).", tid, tid);
    ov_banner(line, 30);
}

/* the game's keystone in a writable copy's sce_sys, so the tool's exports of it carry it (as on a jailbroken
 * console) */
static void keystone_into(const char *host, s32 user, const char *tid)
{
    u8 ks[0x60];
    char p[192];
    if (!savedata_keystone(user, tid, ks) || !kfuncs())
        return;
    fmt(p, sizeof(p), "%s/sce_sys/keystone", host);
    put_file(p, ks, sizeof(ks));
}

/* int sceFsMountSaveData(MountSaveDataOpt *, const char *volume, const char *mountPath, u8 key[32]) */
static s32 vsh_mount(const void *opt, const char *volume, const char *mount_path, const u8 *key)
{
    s32 user;
    char tid[16], dir[32], fp[65];
    if (!mount_path || !parse_volume(volume, &user, tid, dir)) {
        logf("fsvsh: sceFsMountSaveData(%s): not a save image of /user/home: only the save service's own saves "
             "can be mounted here", volume ? volume : "(null)");
        if (trophy_path(volume))
            say_trophies();
        else if (volume)
            ov_notice(&g_said_usb, "Encrypted saves on USB (PS4/SAVEDATA) can't be opened on a stock console.\n"
                                   "Copy decrypted saves (PS4/APOLLO) instead.");
        return E_NOENT;
    }
    if (!savedata_is_foreign(tid)) {
        logf("fsvsh: %s:%s: the running title's or its host's own saves are not mounted this way", tid, dir);
        return E_ACCES_;
    }
    char mp[16], host[128], ckey[48];
    mp[0] = host[0] = 0;
    bool fp_known = savedata_fingerprint(user, tid, fp), rw = false;
    if (!fp_known && keystone_from_usb(tid))
        fp_known = savedata_fingerprint(user, tid, fp);
    fmt(ckey, sizeof(ckey), "%08x/%s/%s", (u32)user, tid, dir);
    s32 rc = E_ACCES_;
    if (fp_known) { /* a writable copy (see the top), made once per session */
        for (u32 c = 0; c < NRW && !host[0]; c++)
            if (strcmp(g_rw[c].key, ckey) == 0)
                strlcpy(host, g_rw[c].host, sizeof(host));
        if (!host[0]) {
            char dst[128];
            fmt(dst, sizeof(dst), "/temp0/ghostldr/%s/rw/%08x_%s_%s", title_ident(), (u32)user, tid, dir);
            if (save_fp_copy(user, tid, dir, fp, dst) == 0) {
                keystone_into(dst, user, tid);
                strlcpy(host, dst, sizeof(host));
                for (u32 c = 0; c < NRW; c++)
                    if (!g_rw[c].key[0]) {
                        strlcpy(g_rw[c].key, ckey, sizeof(g_rw[c].key));
                        strlcpy(g_rw[c].host, dst, sizeof(g_rw[c].host));
                        break;
                    }
            }
        }
        rw = host[0] != 0;
        rc = rw ? 0 : savedata_foreign_mount(user, tid, dir, fp, mp);
    }
    if (rc != 0) {
        /* no keystone, or the system refused the save (another account's): a read-only copy made through a
         * refused mount, once per session (save_race.c) */
        for (u32 c = 0; c < NCOPY && !host[0]; c++)
            if (strcmp(g_copy[c].key, ckey) == 0)
                strlcpy(host, g_copy[c].host, sizeof(host));
        if (!host[0]) {
            char dst[128];
            fmt(dst, sizeof(dst), "/temp0/ghostldr/%s/race/%08x_%s_%s", title_ident(), (u32)user, tid, dir);
            logf("fsvsh: %s:%s: %s; trying a read-only copy through a refused mount", tid, dir,
                 fp_known ? "the fingerprint mount was refused" : "no keystone for its title");
            s32 rc2 = save_race_copy(user, tid, dir, dst);
            if (!fp_known)
                say_no_keystone(tid);
            if (rc2 == 0) {
                strlcpy(host, dst, sizeof(host));
                for (u32 c = 0; c < NCOPY; c++)
                    if (!g_copy[c].key[0]) {
                        strlcpy(g_copy[c].key, ckey, sizeof(g_copy[c].key));
                        strlcpy(g_copy[c].host, dst, sizeof(g_copy[c].host));
                        break;
                    }
            } else if (!fp_known) {
                logf("fsvsh: %s:%s: no copy either (%x): put %s.keystone in /data/ghostldr/keystones", tid, dir,
                     (u32)rc2, tid);
                return rc2;
            } else {
                return rc;
            }
        }
    }
    spin_lock(&g_alias_lock);
    u32 i = 0;
    while (i < NALIAS && g_alias[i].used)
        i++;
    if (i < NALIAS) {
        g_alias[i].used = true;
        norm(g_alias[i].path, sizeof(g_alias[i].path), mount_path);
        memcpy(g_alias[i].mp, mp, 16);
        strlcpy(g_alias[i].host, host, sizeof(g_alias[i].host));
        g_alias[i].rw = rw;
        g_alias[i].user = user;
        strlcpy(g_alias[i].tid, tid, sizeof(g_alias[i].tid));
        strlcpy(g_alias[i].dir, dir, sizeof(g_alias[i].dir));
        strlcpy(g_alias[i].fp, fp_known ? fp : "", sizeof(g_alias[i].fp));
    }
    spin_unlock(&g_alias_lock);
    if (i == NALIAS) {
        if (mp[0])
            savedata_umount(mp);
        return (s32)0x809F000C; /* MOUNT_FULL */
    }
    if (rw)
        g_alias[i].digest = save_tree_digest(host); /* outside the alias lock: it reads the whole copy */
    logf("fsvsh: %s:%s %s at %s for %s", tid, dir, rw ? "as a writable copy (written into the save at the unmount)"
         : "mounted read-only", mp[0] ? mp : host, mount_path);
    return 0;
}

/* int sceFsUmountSaveData(UmountSaveDataOpt *, const char *mountPath, int handle, bool ignoreErrors) */
static s32 vsh_umount(const void *opt, const char *mount_path, s32 handle, u32 ignore)
{
    char p[160], mp[16];
    if (!mount_path)
        return E_INVAL;
    norm(p, sizeof(p), mount_path);
    mp[0] = 0;
    bool copy = false;
    spin_lock(&g_alias_lock);
    u32 i = 0;
    for (; i < NALIAS; i++)
        if (g_alias[i].used && strcmp(g_alias[i].path, p) == 0) {
            memcpy(mp, g_alias[i].mp, 16);
            copy = g_alias[i].host[0] != 0;
            break;
        }
    spin_unlock(&g_alias_lock);
    if (copy && g_alias[i].rw) { /* a writable copy: into the save if the tool changed it */
        s32 rc = 0;
        if (save_tree_digest(g_alias[i].host) != g_alias[i].digest) {
            char backup[160];
            fmt(backup, sizeof(backup), "%s.before", g_alias[i].host);
            struct save_race_result r;
            rc = save_race_write(g_alias[i].user, g_alias[i].tid, g_alias[i].dir, g_alias[i].host, g_alias[i].fp,
                                 backup, &r);
            if (rc == 0)
                g_alias[i].digest = save_tree_digest(g_alias[i].host);
            logf("fsvsh: %s:%s: the tool's changes -> %x%s", g_alias[i].tid, g_alias[i].dir, (u32)rc,
                 r.needs_exit ? ": written into the save; CLOSE SWRR to keep them (the system commits the save "
                                "when SWRR closes)" : "");
            char line[160];
            if (rc == 0 && r.needs_exit)
                ov_banner("Saved. To finish saving, close SWRR from the PS button menu.", 0);
            else if (rc != 0) {
                fmt(line, sizeof(line), "The changes to %s %s could not be written into it (error %x).", g_alias[i].tid,
                    g_alias[i].dir, (u32)rc);
                ov_banner(line, 30);
            }
        }
        spin_lock(&g_alias_lock);
        g_alias[i].used = false;
        spin_unlock(&g_alias_lock);
        return rc;
    }
    if (copy) { /* a read-only copy: nothing is mounted */
        spin_lock(&g_alias_lock);
        g_alias[i].used = false;
        spin_unlock(&g_alias_lock);
        return 0;
    }
    if (!mp[0])
        return E_NOENT;
    s32 rc = savedata_umount(mp);
    if (rc == 0 || (ignore & 0xFF)) {
        spin_lock(&g_alias_lock);
        g_alias[i].used = false;
        spin_unlock(&g_alias_lock);
    }
    logf("fsvsh: unmount %s (%s) -> %x", mount_path, mp, (u32)rc);
    return rc;
}

static s32 vsh_create(void)
{
    logf("fsvsh: creating a save image refused: a save a game creates carries that game's keystone, which the real "
         "title rejects (and its key comes from the system's key service)");
    fsvsh_new_save_refused();
    return E_PERM;
}

/* ---- savedata.db ---- */

static const char k_schema[] =
    "CREATE TABLE savedata(title_id TEXT, dir_name TEXT, main_title TEXT, sub_title TEXT, detail TEXT, "
    "tmp_dir_name TEXT, is_broken INTEGER, user_param INTEGER, blocks INTEGER, free_blocks INTEGER, size_kib "
    "INTEGER, mtime TEXT, fake_broken INTEGER, account_id INTEGER, user_id INTEGER, faked_owner INTEGER, "
    "cloud_icon_url TEXT, cloud_revision INTEGER, game_title_id TEXT)";
#define NCOLS 19

static bool title_like(const char *s, u32 n)
{
    if (n < 9)
        return false;
    for (int k = 0; k < 4; k++)
        if (s[k] < 'A' || s[k] > 'Z')
            return false;
    for (int k = 4; k < 9; k++)
        if (s[k] < '0' || s[k] > '9')
            return false;
    return true;
}

#define MAXT 1024

/* the title IDs to ask the save service about */
struct tlist {
    char (*id)[10];
    u8 *always; /* named by the user (/data/ghostldr): asked every time */
    u32 n;
};

static void add_title(struct tlist *t, const char *s, bool always)
{
    for (u32 i = 0; i < t->n; i++)
        if (memcmp(t->id[i], s, 9) == 0) {
            t->always[i] |= always;
            return;
        }
    if (t->n < MAXT) {
        memcpy(t->id[t->n], s, 9);
        t->id[t->n][9] = 0;
        t->always[t->n++] = always;
    }
}

/* title IDs named by the files (or directories) of a title-view directory */
static void titles_in(struct tlist *t, const char *dir, bool dirs, bool always)
{
    char buf[320];
    int fd = vfs_open(title_path(dir, buf, sizeof(buf)), 0x20000 /* O_DIRECTORY */, 0);
    if (fd < 0)
        return;
    u8 *dents = page_alloc(8192);
    for (int got; dents && (got = vfs_getdents(fd, dents, 8192)) > 0;)
        for (int off = 0; off + 8 <= got;) {
            u16 reclen = *(u16 *)(dents + off + 4);
            u8 type = dents[off + 6], nlen = dents[off + 7];
            if (!reclen)
                break;
            if ((dirs ? type == 4 : type == 8) && title_like((const char *)dents + off + 8, nlen))
                add_title(t, (const char *)dents + off + 8, always);
            off += reclen;
        }
    if (dents)
        page_free(dents, 8192);
    vfs_close(fd);
}

/* title IDs listed in a text file (any separators), e.g. /data/ghostldr/titles.txt: the save service lists a
 * title's saves only when asked for that exact title ID */
static void titles_listed(struct tlist *t, const char *path)
{
    char buf[320];
    int fd = vfs_open(title_path(path, buf, sizeof(buf)), O_RDONLY, 0);
    if (fd < 0)
        return;
    char *text = page_alloc(16384);
    s64 n = text ? vfs_read(fd, text, 16383) : -1;
    vfs_close(fd);
    for (s64 i = 0; n > 0 && i + 9 <= n; i++)
        if ((i == 0 || !(text[i - 1] >= 'A' && text[i - 1] <= 'Z')) && title_like(text + i, 9) &&
            (i + 9 == n || !(text[i + 9] >= '0' && text[i + 9] <= '9')))
            add_title(t, text + i, true), i += 8;
    if (text)
        page_free(text, 16384);
}

static bool collect(struct tlist *t, s32 user)
{
    t->n = 0;
    if (!t->id && !(t->id = page_alloc(sizeof(*t->id) * MAXT)))
        return false;
    if (!t->always && !(t->always = page_alloc(MAXT)))
        return false;
    char d[64];
    titles_listed(t, "/data/ghostldr/titles.txt");
    titles_in(t, "/data/ghostldr/keystones", false, true);
    titles_in(t, "/data/apollo/data", false, false);
    fmt(d, sizeof(d), "/data/apollo/%08x", (u32)user);
    titles_in(t, d, true, false);
    return true;
}

/* title IDs named by the entries of a zip (its central directory only:
 * nothing is inflated): the names of a save tool's bundled keystones and
 * cheat files, e.g. Apollo's assets/misc/appdata.zip, before it unpacks it */
static void titles_in_zip(struct tlist *t, const char *path)
{
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0) {
        char buf[320];
        fd = vfs_open(title_path(path, buf, sizeof(buf)), O_RDONLY, 0);
    }
    if (fd < 0)
        return;
    s64 size = vfs_lseek(fd, 0, 2 /* SEEK_END */);
    u64 tail = size > 0x10000 ? 0x10000 : (u64)(size > 0 ? size : 0);
    u8 *b = page_alloc(0x10000);
    u8 *cd = NULL;
    u64 cd_size = 0;
    if (b && tail >= 22 && vfs_lseek(fd, size - (s64)tail, 0) >= 0 && vfs_read(fd, b, tail) == (s64)tail) {
        for (s64 i = (s64)tail - 22; i >= 0; i--) /* the end of central directory record */
            if (b[i] == 'P' && b[i + 1] == 'K' && b[i + 2] == 5 && b[i + 3] == 6) {
                cd_size = *(u32 *)(b + i + 12);
                u64 cd_off = *(u32 *)(b + i + 16);
                if (cd_size && cd_size <= (4u << 20) && cd_off + cd_size <= (u64)size && (cd = page_alloc(cd_size)) &&
                    (vfs_lseek(fd, (s64)cd_off, 0) < 0 || vfs_read(fd, cd, cd_size) != (s64)cd_size))
                    page_free(cd, cd_size), cd = NULL;
                break;
            }
    }
    vfs_close(fd);
    u32 n0 = t->n;
    for (u64 off = 0; cd && off + 46 <= cd_size && cd[off] == 'P' && cd[off + 1] == 'K' && cd[off + 2] == 1 &&
                      cd[off + 3] == 2;) {
        u16 nlen = *(u16 *)(cd + off + 28), xlen = *(u16 *)(cd + off + 30), clen = *(u16 *)(cd + off + 32);
        const char *name = (const char *)cd + off + 46;
        u32 k = nlen;
        while (k && name[k - 1] != '/')
            k--;
        const char *base = name + k;
        u32 blen = nlen - k;
        if ((blen == 18 && memcmp(base + 9, ".keystone", 9) == 0) ||
            (blen == 19 && memcmp(base + 9, ".savepatch", 10) == 0))
            if (title_like(base, 9))
                add_title(t, base, false);
        off += 46u + nlen + xlen + clen;
    }
    if (cd)
        page_free(cd, cd_size);
    if (b)
        page_free(b, 0x10000);
    if (t->n > n0)
        logf("fsvsh: %s names %u more titles", path, t->n - n0);
}

/* ---- what the save service said about each title, per user, remembered ----
 * One exact DirNameSearch per title, ~0.4 s each on a 13.60 PS5 and a 12.50
 * PS4 (eight at once take as long: the service answers them one at a time),
 * and Apollo's keystone and cheat files name ~350 titles: 140 s.  So the
 * answers are kept in /data/ghostldr/savesearch.txt (the title's kept /data)
 * for each user, and a title without saves is not asked about again for a
 * while: the selector's save scan (save_prescan, with its progress bar) asks
 * about those never asked and those without saves asked more than NONE_TTL
 * ago; the list a save tool reads asks about titles with saves, titles never
 * asked and those the user named; and from the moment a save tool loads the
 * VSH library a background refresh asks about the titles without saves last
 * asked more than a day ago (a game played since shows up once it is done). */
#define SEARCH_MAX 64
#define CACHE_PATH "/data/ghostldr/savesearch.txt"
#define NONE_TTL (7 * 86400)
#define REFRESH_AGE 86400

struct known {
    u32 user;
    char id[10];
    u16 hits;
    s64 at;
};
static struct known *g_known;
static u32 g_nknown;
static volatile int g_known_lock;
static bool g_known_loaded;
static volatile int g_refresh; /* 0 never, 1 running, 2 done */

static s64 now_s(void);

static struct known *known_find(u32 user, const char *id)
{
    for (u32 i = 0; i < g_nknown; i++)
        if (g_known[i].user == user && memcmp(g_known[i].id, id, 9) == 0)
            return &g_known[i];
    return NULL;
}

static void known_note(u32 user, const char *id, u32 hits, s64 at)
{
    spin_lock(&g_known_lock);
    struct known *k = known_find(user, id);
    if (!k && g_known && g_nknown < MAXT * 4)
        k = &g_known[g_nknown++], k->user = user, memcpy(k->id, id, 10);
    if (k)
        k->hits = (u16)(hits > 0xFFFF ? 0xFFFF : hits), k->at = at;
    spin_unlock(&g_known_lock);
}

static s64 parse_num(const char **p, u32 base)
{
    s64 v = 0;
    while (**p == ' ')
        (*p)++;
    for (;; (*p)++) {
        char c = **p;
        u32 d = c >= '0' && c <= '9' ? (u32)(c - '0') : base == 16 && c >= 'a' && c <= 'f' ? (u32)(c - 'a' + 10) : 99;
        if (d >= base)
            return v;
        v = v * base + d;
    }
}

/* "<user hex> <TITLE> <hits> <unix time>" per line */
static void known_load(void)
{
    if (g_known_loaded)
        return;
    if (!g_known && !(g_known = page_alloc(sizeof(*g_known) * MAXT * 4)))
        return;
    g_known_loaded = true;
    char buf[320];
    int fd = vfs_open(title_path(CACHE_PATH, buf, sizeof(buf)), O_RDONLY, 0);
    if (fd < 0)
        return;
    u64 cap = MAXT * 4 * 48;
    char *text = page_alloc(cap);
    s64 n = text ? vfs_read(fd, text, cap - 1) : -1;
    vfs_close(fd);
    if (n > 0) {
        text[n] = 0;
        for (const char *p = text; *p;) {
            const char *q = p;
            u32 user = (u32)parse_num(&q, 16);
            bool hex8 = q - p == 8;
            while (*q == ' ')
                q++;
            if (hex8 && title_like(q, 9) && q[9] == ' ') {
                char id[10];
                memcpy(id, q, 9), id[9] = 0;
                q += 9;
                s64 hits = parse_num(&q, 10), at = parse_num(&q, 10);
                known_note(user, id, (u32)hits, at);
            }
            while (*p && *p != '\n')
                p++;
            if (*p)
                p++;
        }
    }
    if (text)
        page_free(text, cap);
}

static bool kfuncs(void)
{
    if (!k_open) {
        k_write = host_real("sceKernelWrite");
        k_close = host_real("sceKernelClose");
        k_mkdir = host_real("sceKernelMkdir");
        k_open = host_real("sceKernelOpen");
    }
    return k_open && k_write && k_close && k_mkdir;
}

static void known_save(void)
{
    char buf[320], dir[320];
    if (!g_known || !kfuncs())
        return;
    const char *path = title_path(CACHE_PATH, buf, sizeof(buf));
    strlcpy(dir, path, sizeof(dir));
    char *slash = (char *)strrchr_c(dir, '/');
    if (slash)
        *slash = 0, k_mkdir(dir, 0777);
    u64 cap = MAXT * 4 * 48;
    char *text = page_alloc(cap);
    if (!text)
        return;
    u64 len = 0;
    spin_lock(&g_known_lock);
    for (u32 i = 0; i < g_nknown && len + 48 < cap; i++)
        len += (u64)fmt(text + len, cap - len, "%08x %s %u %ld\n", g_known[i].user, g_known[i].id, g_known[i].hits,
                        g_known[i].at);
    spin_unlock(&g_known_lock);
    int fd = k_open(path, 0x601 /* O_WRONLY|O_CREAT|O_TRUNC */, 0666);
    if (fd >= 0) {
        k_write(fd, text, len);
        k_close(fd);
    }
    page_free(text, cap);
}

/* ---- asking ---- */

struct search {
    s32 user;
    struct tlist *t;
    const u8 *ask; /* which of t to ask about */
    u32 total, asked;
    struct sqlm *db; /* rows of the titles with saves (NULL: only remember the answers) */
    u32 rows, titles_with;
    int (*progress)(void *arg, u32 done, u32 total); /* nonzero: stop */
    void *arg;
    bool stopped;
};

static void iso_time(char *out, size_t size, s64 t);

static void search_all(struct search *S)
{
    char (*dirs)[32] = page_alloc(32 * SEARCH_MAX);
    struct sd_param *params = page_alloc(sizeof(struct sd_param) * SEARCH_MAX);
    u64 *blocks = page_alloc(8 * SEARCH_MAX);
    S->total = 0;
    for (u32 i = 0; i < S->t->n; i++)
        S->total += S->ask[i] != 0;
    if (S->progress && S->total && S->progress(S->arg, 0, S->total)) /* the total known: the bar can show */
        S->stopped = true;
    for (u32 i = 0; dirs && params && blocks && i < S->t->n && !S->stopped; i++) {
        if (!S->ask[i])
            continue;
        const char *tid = S->t->id[i];
        u32 hits = 0;
        s32 rc = savedata_foreign_search(S->user, tid, dirs, params, blocks, SEARCH_MAX, &hits);
        S->asked++;
        if (S->progress && S->progress(S->arg, S->asked, S->total))
            S->stopped = true;
        if (rc != 0)
            continue;
        known_note((u32)S->user, tid, hits, now_s());
        if (!S->db || !hits)
            continue;
        S->titles_with++;
        for (u32 k = 0; k < hits; k++) {
            char dir[33], mt[32];
            memcpy(dir, dirs[k], 32);
            dir[32] = 0;
            iso_time(mt, sizeof(mt), params[k].mtime);
            params[k].title[127] = params[k].sub_title[127] = params[k].detail[1023] = 0;
            struct sqlm_val v[NCOLS] = {
                {tid, 0}, {dir, 0}, {params[k].title, 0}, {params[k].sub_title, 0},
                {params[k].detail, 0}, {"", 0}, {NULL, 0}, {NULL, params[k].user_param}, {NULL, (s64)blocks[k]},
                {NULL, 0}, {NULL, (s64)blocks[k] * 32}, {mt, 0}, {NULL, 0}, {NULL, 0}, {NULL, S->user}, {NULL, 0},
                {"", 0}, {NULL, 0}, {tid, 0}};
            if (sqlm_row(S->db, v))
                S->rows++;
        }
    }
    if (dirs)
        page_free(dirs, 32 * SEARCH_MAX);
    if (params)
        page_free(params, sizeof(struct sd_param) * SEARCH_MAX);
    if (blocks)
        page_free(blocks, 8 * SEARCH_MAX);
}

/* which titles of t to ask about: those never asked for user, and (when
 * older_than > 0) those without saves last asked longer ago than that;
 * with_saves: also every title known to have saves and those the user
 * named (a list of saves asks about those every time anyway) */
static u32 mark(struct tlist *t, u8 *ask, u32 user, s64 older_than, bool with_saves)
{
    s64 now = now_s();
    u32 n = 0;
    spin_lock(&g_known_lock);
    for (u32 i = 0; i < t->n; i++) {
        struct known *k = known_find(user, t->id[i]);
        ask[i] = !k || (older_than > 0 && !k->hits && now - k->at > older_than) ||
                 (with_saves && (k->hits || t->always[i]));
        n += ask[i];
    }
    spin_unlock(&g_known_lock);
    return n;
}

/* the candidates: the persistent /data's, and a save tool's bundled lists */
static bool candidates(struct tlist *t, u32 user)
{
    if (!collect(t, user))
        return false;
    titles_in_zip(t, "/app0/assets/misc/appdata.zip"); /* Apollo */
    titles_in_zip(t, "/app0/appdata.zip");
    return true;
}

/* what the scan allocated, freed before it returns (it runs before the
 * title's memory is planned); later users load the cache again */
static void tlist_free(struct tlist *t)
{
    if (t->id)
        page_free(t->id, sizeof(*t->id) * MAXT), t->id = NULL;
    if (t->always)
        page_free(t->always, MAXT), t->always = NULL;
    t->n = 0;
}

static void known_drop(void)
{
    spin_lock(&g_known_lock);
    if (g_known)
        page_free(g_known, sizeof(*g_known) * MAXT * 4), g_known = NULL;
    g_nknown = 0;
    g_known_loaded = false;
    spin_unlock(&g_known_lock);
}

/* The game selector's save scan for a save tool (ghost_api.save_prescan,
 * through struct ext_early: approot.c), after the title is picked and its
 * root mounted, before ext_init: so /data is resolved directly
 * (title_path) and nothing it allocates stays.  For user, ask the save
 * service about every candidate title never asked about, or without saves
 * when last asked more than NONE_TTL ago; progress(arg, 0, total) once the
 * candidates are known and after each title (nonzero: stop, keeping what
 * was found); remember the answers.  Returns how many candidate titles
 * have saves, or < 0. */
int save_prescan(u32 user, int (*progress)(void *arg, u32 done, u32 total), void *arg) HIDDEN;
int save_prescan(u32 user, int (*progress)(void *arg, u32 done, u32 total), void *arg)
{
    struct tlist t;
    memset(&t, 0, sizeof(t));
    s64 t0 = now_s();
    known_load();
    u8 *ask = page_alloc(MAXT);
    int with = -1;
    struct search S;
    memset(&S, 0, sizeof(S));
    if (g_known && ask && candidates(&t, user)) {
        S.user = (s32)user, S.t = &t, S.ask = ask, S.progress = progress, S.arg = arg;
        mark(&t, ask, user, NONE_TTL, false);
        search_all(&S);
        if (S.asked)
            known_save();
        with = 0;
        spin_lock(&g_known_lock);
        for (u32 i = 0; i < t.n; i++) {
            struct known *k = known_find(user, t.id[i]);
            with += k && k->hits;
        }
        spin_unlock(&g_known_lock);
    }
    logf("fsvsh: save scan for user %08x: %u candidate titles, %u asked in %ld s%s, %d with saves", user, t.n,
         S.asked, now_s() - t0, S.stopped ? " (stopped)" : "", with);
    if (ask)
        page_free(ask, MAXT);
    tlist_free(&t);
    known_drop();
    savedata_data_release(); /* nothing stays: the title mounts its /data itself */
    return with;
}

/* the background refresh: titles last asked more than a day ago */
static void *refresh_thread(void *arg)
{
    s32 user = (s32)(u64)arg;
    static struct tlist t;
    s64 t0 = now_s();
    known_load();
    u8 *ask = page_alloc(MAXT);
    if (ask && candidates(&t, (u32)user)) {
        struct search S;
        memset(&S, 0, sizeof(S));
        S.user = user, S.t = &t, S.ask = ask;
        mark(&t, ask, (u32)user, REFRESH_AGE, false);
        search_all(&S);
        if (S.asked)
            known_save();
        logf("fsvsh: background refresh for user %08x: %u titles asked in %ld s", (u32)user, S.asked, now_s() - t0);
    }
    if (ask)
        page_free(ask, MAXT);
    __atomic_store_n(&g_refresh, 2, __ATOMIC_RELEASE);
    return NULL;
}

/* the title loaded libSceFsInternalForVsh (hle_module.c): a save tool, which
 * will list saves: start asking the save service now */
void fsvsh_loaded(void) HIDDEN;
void fsvsh_loaded(void)
{
    int no = 0;
    if (!__atomic_compare_exchange_n(&g_refresh, &no, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    s32 (*create)(u64 *, const void *, void *(*)(void *), void *, const char *) = host_real("scePthreadCreate");
    s32 (*detach)(u64) = host_real("scePthreadDetach");
    s32 user = title_user();
    u64 th = 0;
    if (!create || user < 0 || create(&th, NULL, refresh_thread, (void *)(u64)(u32)user, "ghostldr_refresh") != 0) {
        __atomic_store_n(&g_refresh, 0, __ATOMIC_RELEASE);
        return;
    }
    ext_thread_note(th, "ghostldr_refresh");
    if (detach)
        detach(th);
    logf("fsvsh: a save tool: asking the save service about other titles' saves in the background");
}

/* seconds since 1970 -> "YYYY-MM-DDTHH:MM:SS.00Z" */
static void iso_time(char *out, size_t size, s64 t)
{
    if (t < 0)
        t = 0;
    s64 days = t / 86400, rem = t % 86400;
    /* civil from days (Howard Hinnant's algorithm) */
    s64 z = days + 719468, era = z / 146097, doe = z - era * 146097;
    s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
    s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    s64 d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    fmt(out, size, "%04ld-%02ld-%02ldT%02ld:%02ld:%02ld.00Z", y, m, d, rem / 3600, rem / 60 % 60, rem % 60);
}

#define DB_CAP (4096UL * 512)

/* write the user's savedata.db stand-in to out (a host path); false if it could not be made */
static bool make_db(s32 user, const char *out)
{
    static struct tlist t;
    if (!candidates(&t, (u32)user))
        return false;
    known_load();
    u8 *ask = page_alloc(MAXT);
    u8 *db_buf = direct_alloc(DB_CAP, PROT_READ | PROT_WRITE, 0);
    bool ok = ask && db_buf;
    s64 t0 = now_s();
    if (ok && mark(&t, ask, (u32)user, 0, false) && __atomic_load_n(&g_refresh, __ATOMIC_ACQUIRE) == 1) {
        /* titles never asked about while the background refresh asks: wait for it rather than ask twice */
        logf("fsvsh: waiting for the background search (titles never asked about yet)");
        while (__atomic_load_n(&g_refresh, __ATOMIC_ACQUIRE) == 1)
            K.usleep(100000);
    }
    u32 rows = 0, titles_with = 0, asked = 0;
    if (ok) {
        mark(&t, ask, (u32)user, 0, true); /* never asked, with saves, named by the user */
        struct sqlm db;
        sqlm_begin(&db, db_buf, DB_CAP, "savedata", k_schema, NCOLS);
        struct search S;
        memset(&S, 0, sizeof(S));
        S.user = user, S.t = &t, S.ask = ask, S.db = &db;
        search_all(&S);
        rows = S.rows, titles_with = S.titles_with, asked = S.asked;
        if (asked)
            known_save();
        u64 size = sqlm_end(&db);
        int fd = size && kfuncs() ? k_open(out, 0x601 /* O_WRONLY|O_CREAT|O_TRUNC */, 0666) : -1;
        ok = fd >= 0 && k_write(fd, db_buf, size) == (s64)size;
        if (fd >= 0)
            k_close(fd);
        logf("fsvsh: savedata.db for user %08x: %u saves of %u titles (of %u titles with a keystone or cheat file; "
             "%u asked in %ld s, the rest had none when last asked) -> %s",
             (u32)user, rows, titles_with, t.n, asked, now_s() - t0, ok ? out : "not written");
    }
    if (ask)
        page_free(ask, MAXT);
    if (db_buf)
        direct_free(db_buf);
    return ok;
}

static s64 now_s(void)
{
    static s32 (*tod)(void *, void *);
    if (!tod)
        tod = host_real("gettimeofday");
    s64 tv[2] = {0, 0};
    return tod && tod(tv, NULL) == 0 ? tv[0] : 0;
}

/* "/system_data/savedata/%08x/db/user/savedata.db" -> its stand-in in the title's /temp0 directory */
static const char *db_remap(const char *path, char *buf, size_t size)
{
    static const char pre[] = "/system_data/savedata/";
    if (strncmp(path, pre, sizeof(pre) - 1) != 0) {
        if (trophy_path(path)) /* the trophy database or a trophy set: out of SWRR's reach */
            say_trophies();
        return NULL;
    }
    const char *p = path + sizeof(pre) - 1;
    if (strlen(p) != 8 + strlen("/db/user/savedata.db") || strcmp(p + 8, "/db/user/savedata.db") != 0)
        return NULL;
    u32 user = 0;
    for (int k = 0; k < 8; k++) {
        char c = p[k];
        u32 d = c >= '0' && c <= '9' ? (u32)(c - '0') : c >= 'a' && c <= 'f' ? (u32)(c - 'a' + 10) : 99;
        if (d > 15)
            return NULL;
        user = user << 4 | d;
    }
    static volatile int lock;
    static s64 made_at;
    static u32 made_for;
    char dir[96];
    fmt(dir, sizeof(dir), "/temp0/ghostldr/%s/vsh", title_ident());
    fmt(buf, size, "%s/savedata_%08x.db", dir, user);
    spin_lock(&lock);
    s64 t = now_s();
    if (made_for != user || t - made_at > 3 || !made_at) {
        if (kfuncs()) {
            char parent[96];
            fmt(parent, sizeof(parent), "/temp0/ghostldr/%s", title_ident());
            k_mkdir("/temp0/ghostldr", 0777);
            k_mkdir(parent, 0777);
            k_mkdir(dir, 0777);
        }
        if (make_db((s32)user, buf)) /* stale 3 s after it was made, not after it was started */
            made_at = now_s(), made_for = user;
    }
    spin_unlock(&lock);
    return buf;
}

static const char *vsh_remap(const char *path, char *buf, size_t size)
{
    char tmp[512];
    for (u32 i = 0; i < NALIAS; i++) {
        if (!g_alias[i].used)
            continue;
        size_t n = strlen(g_alias[i].path);
        if (strncmp(path, g_alias[i].path, n) == 0 && (path[n] == '/' || path[n] == 0)) {
            if (g_alias[i].host[0]) { /* a copy: already a host path */
                fmt(buf, size, "%s%s", g_alias[i].host, path + n);
                return buf;
            }
            fmt(tmp, sizeof(tmp), "%.16s%s", g_alias[i].mp, path + n);
            return remap_path(tmp, buf, size);
        }
    }
    return db_remap(path, buf, size);
}

static int fsvsh_init(void)
{
    if (remap_hook_add && remap_hook_add(vsh_remap) != 0)
        logf("fsvsh: no room for its path hook");
    return 0;
}

static const struct ext_hle_ent k_hle[] = {
    {LIB, "sceFsInitMountSaveDataOpt", vsh_init_opt, NULL},
    {LIB, "sceFsMountSaveData", vsh_mount, NULL},
    {LIB, "sceFsInitUmountSaveDataOpt", vsh_init_opt, NULL},
    {LIB, "sceFsUmountSaveData", vsh_umount, NULL},
    {LIB, "sceFsInitCreatePfsSaveDataOpt", vsh_init_opt, NULL},
    {LIB, "sceFsCreatePfsSaveDataImage", vsh_create, NULL},
    {LIB, "sceFsUfsAllocateSaveData", vsh_create, NULL},
};

static const struct ext_lib k_libs[] = {
    {LIB, NULL, EXT_LIB_NO_HOST | EXT_LIB_VIRTUAL, E_PERM},
    {"libkernel_sys", NULL, EXT_LIB_VIRTUAL, E_PERM},
};

EXT_FAMILY_LIBS("fsvsh", k_hle, k_libs, fsvsh_init);
