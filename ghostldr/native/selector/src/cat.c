/*
 * The installed titles and their art, read straight from SWRR's save data
 * (docs/ghostfs-catalogue.md):
 *
 *  - GFSCATALOG/catalog.bin lists the titles; without it (installs made by
 *    ghostldr v0.1) the only title is the one in GFSINDEX, else GHOSTIDX;
 *  - a title's index (<prefix>INDEX, or the legacy one) gives the offset and
 *    size of sce_sys/icon0.png, pic1.png and param.sfo in its linear space;
 *  - those bytes are read from its data containers <prefix>000.. (cap bytes
 *    each), mounted read-only for the read and unmounted at once.
 *
 * Everything read goes into the selector's heap and is released after use;
 * no mount outlives a call.  Outside ghostfs (an app root on /data, dev
 * setups) there is one title, read through the core's file calls; or, when
 * the core says the app root is a library (CTX_FLAG_LIBRARY: games found on
 * a USB drive, docs/usb.md), the games the extension found, in its order
 * (ghost_api.library), each with its name and art in its folder's meta.bin
 * when gfs_pack.py --usb wrote one.  Games that can't be booted are listed
 * too, with the reason.
 */
#include "plat.h"

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);
size_t strlen(const char *s);

#define INDEX_MAX (64u << 20)
static const char FILES[NFILES][20] = {"sce_sys/icon0.png", "sce_sys/pic1.png", "sce_sys/param.sfo",
                                        "sce_sys/pic0.png"};

/* ---- one container at a time ---- */
static int mount(struct plat *P, const char *dir, char mp[16])
{
    char d[32];
    strlcpy(d, dir, sizeof(d));
    int rc = P->api->gfs_mount(d, 1 /* read-only */, 0, mp);
    if (rc == 0)
        P->mounts++;
    return rc;
}

static void umount(struct plat *P, const char mp[16])
{
    P->api->gfs_umount(mp, 2000);
    P->umounts++;
}

/* size of mp/file, or -1 */
static s64 file_size(struct plat *P, const char *mp, const char *file, char path[64])
{
    u8 st[0x80];
    fmt(path, 64, "%s/%s", mp, file);
    return P->k->stat(path, st) == 0 ? *(s64 *)(st + 0x48) : -1;
}

/* read up to cap bytes of dir/file at off into buf: bytes read, or < 0 */
static s64 read_in(struct plat *P, const char *dir, const char *file, u64 off, void *buf, u64 cap)
{
    char mp[16], path[64];
    if (mount(P, dir, mp) != 0)
        return -1;
    s64 got = -1;
    fmt(path, sizeof(path), "%s/%s", mp, file);
    int fd = P->k->open(path, O_RDONLY, 0);
    if (fd >= 0) {
        got = 0;
        while ((u64)got < cap) {
            s64 r = P->k->pread(fd, (u8 *)buf + got, cap - (u64)got, (off_t)(off + (u64)got));
            if (r <= 0)
                break;
            got += r;
        }
        P->k->close(fd);
    }
    umount(P, mp);
    return got;
}

/* ---- the list ---- */
struct cat_hdr {
    char magic[4];
    u32 version, header_size, entry_size, count, flags;
    u64 generation;
};
#define CAT_ENTRY 192

static int legacy_header(struct plat *P, u8 h[128])
{
    memset(h, 0, 128);
    for (int i = 0; i < 2; i++) {
        strlcpy(P->legacy, i ? "GHOSTIDX" : "GFSINDEX", sizeof(P->legacy));
        if (read_in(P, P->legacy, "index.bin", 0, h, 128) >= 64 && !memcmp(h, "GFS1", 4))
            return 0;
    }
    P->legacy[0] = 0;
    return -1;
}

/* ---- a library: the games approot.c found (ghost_api.library) ---- */
/* up to cap bytes at off of a file under the app root: bytes read, or < 0 */
static s64 root_read(struct plat *P, const char *path, u64 off, void *buf, u64 cap)
{
    const struct ghost_api *a = P->api;
    int fd = a->vfs_open(path, O_RDONLY, 0);
    if (fd < 0)
        return -1;
    s64 got = a->vfs_lseek(fd, (s64)off, 0) == (s64)off ? 0 : -1;
    while (got >= 0 && (u64)got < cap) {
        s64 r = a->vfs_read(fd, (u8 *)buf + got, cap - (u64)got);
        if (r <= 0)
            break;
        got += r;
    }
    a->vfs_close(fd);
    return got;
}

/* How many titles are installed (before the heap exists). */
int cat_count(struct plat *P)
{
    /* by the app root, not gfs_active(): with several titles the core has
     * not loaded any index yet when it asks the selector */
    if (strcmp((const char *)P->ctx->app_root, "ghostfs") != 0) {
        strlcpy(P->root, (const char *)P->ctx->app_root, sizeof(P->root));
        if (P->ctx->flags & CTX_FLAG_LIBRARY) {
            P->library = 1;
            int n = P->api->library ? P->api->library(&P->lib) : 0;
            return n < SEL_MAX_TITLES ? n : SEL_MAX_TITLES;
        }
        return 1;
    }
    P->ghostfs = 1;
    struct cat_hdr h;
    memset(&h, 0, sizeof(h));
    if (read_in(P, "GFSCATALOG", "catalog.bin", 0, &h, sizeof(h)) == (s64)sizeof(h) && !memcmp(h.magic, "GFSC", 4) &&
        h.header_size >= 64 && h.entry_size >= CAT_ENTRY && h.count <= SEL_MAX_TITLES) {
        P->catalogue = 1;
        if (h.count)
            return (int)h.count;
        P->catalogue = 0; /* an empty catalogue: maybe a legacy install */
    }
    u8 idx[128];
    return legacy_header(P, idx) == 0 ? 1 : 0;
}

static void copy_str(char *d, const u8 *s, int n, int cap)
{
    int i = 0;
    for (; i < n && i + 1 < cap && s[i]; i++)
        d[i] = (char)s[i];
    d[i] = 0;
}

/* ---- names ---- */

/* The title's name in its profile: run.py sends every profile it knows
 * (profiles.py boot_bundle) and ghostldr.lua leaves them in the boot
 * context's last u64 (profile.c PROFILE_SLOT); REC_NAME is the profile's
 * "name".  Read in place: blobs of struct prof_hdr + records, ended by a
 * zero magic. */
#define PROFILE_SLOT 63
static void profile_name(struct plat *P, struct cat_title *c)
{
    const u8 *p = (const u8 *)((const u64 *)P->ctx)[PROFILE_SLOT];
    for (int n = 0; p && c->id[0] && n < 256; n++) {
        const struct prof_hdr *h = (const struct prof_hdr *)p;
        if (memcmp(h->magic, "GLPF", 4) != 0 || h->version != 1 || h->header_size < sizeof(*h) ||
            h->total < h->header_size || h->total > 0x10000 || (h->total & 7))
            return;
        if (strncmp(h->title, c->id, sizeof(h->title)) == 0) {
            for (u32 at = h->header_size; at + sizeof(struct prof_rec) <= h->total;) {
                const struct prof_rec *r = (const struct prof_rec *)(p + at);
                if (r->size < sizeof(*r) || at + r->size > h->total)
                    return;
                if (r->kind == REC_NAME)
                    copy_str(c->prof_name, (const u8 *)(r + 1), (int)(r->size - sizeof(*r)), sizeof(c->prof_name));
                at += r->size;
            }
            return;
        }
        p += h->total;
    }
}

/* The name shown: the title's own param.sfo, its profile, then names stored
 * at install time; none: the UI shows the title ID. */
void cat_pick_name(struct cat_title *c)
{
    const char *pick[] = {c->sfo_name, c->prof_name, c->meta_name, c->cat_name};
    u8 meta_src = c->meta.name_source ? c->meta.name_source : NAME_GIVEN;
    const u8 src[] = {NAME_SFO, NAME_PROFILE, meta_src, NAME_CATALOGUE};
    c->name[0] = 0, c->name_src = 0;
    for (int i = 0; i < 4; i++)
        if (pick[i][0]) {
            strlcpy(c->name, pick[i], sizeof(c->name));
            c->name_src = src[i];
            return;
        }
}

/* param.sfo: the name (in the system language when the title has it), ID and version */
static void sfo_names(struct plat *P, struct cat_title *c, const u8 *sfo, s64 n)
{
    s32 lang = -1;
    char key[16];
    if (P->param_int && P->param_int(1, &lang) == 0 && lang >= 0 && lang < 100) {
        fmt(key, sizeof(key), "TITLE_%02d", lang);
        sfo_get(sfo, (u64)n, key, c->sfo_name, sizeof(c->sfo_name), NULL);
    }
    if (!c->sfo_name[0])
        sfo_get(sfo, (u64)n, "TITLE", c->sfo_name, sizeof(c->sfo_name), NULL);
    if (!c->id[0])
        sfo_get(sfo, (u64)n, "TITLE_ID", c->id, sizeof(c->id), NULL);
    if (!c->version[0])
        sfo_get(sfo, (u64)n, "APP_VER", c->version, sizeof(c->version), NULL);
}

static void read_sfo(struct sel *S, struct plat *P, int t)
{
    struct cat_title *c = &P->cat[t];
    s64 n = cat_file(S, P, t, FILE_SFO, NULL, 0);
    if (n <= 0 || n > 0x10000)
        return;
    u64 m = sel_mark(S);
    u8 *sfo = sel_alloc(S, (u64)n);
    if (sfo && cat_file(S, P, t, FILE_SFO, sfo, (u64)n) == n)
        sfo_names(P, c, sfo, n);
    sel_release(S, m);
    cat_pick_name(c);
}

/* ---- <prefix>META (host/gfs_meta.py): the name and art stored with the install ---- */
s64 cat_meta_read(struct plat *P, int t, u64 off, void *buf, u64 cap)
{
    char dir[320];
    if (P->library) { /* meta.bin in the game's folder, beside its app0 */
        const struct cat_title *c = &P->cat[t];
        strlcpy(dir, c->path, c->dir_len + 1u < sizeof(dir) ? c->dir_len + 1u : sizeof(dir));
        fmt(dir + strlen(dir), sizeof(dir) - strlen(dir), "/meta.bin");
        return root_read(P, dir, off, buf, cap);
    }
    fmt(dir, sizeof(dir), "%sMETA", P->cat[t].prefix);
    return read_in(P, dir, "meta.bin", off, buf, cap);
}

/* What META says about the names, from its start (head, n bytes) */
void cat_meta_names(struct plat *P, int t, const u8 *head, u64 n)
{
    struct cat_title *c = &P->cat[t];
    const struct meta_sect *sf = &c->meta.s[META_SFO];
    if (sf->size && sf->size <= 0x10000 && (u64)sf->off + sf->size <= n)
        sfo_names(P, c, head + sf->off, sf->size);
    str_copy(c->meta_name, c->meta.name, sizeof(c->meta_name));
    if (!c->version[0])
        str_copy(c->version, c->meta.version, sizeof(c->version));
    cat_pick_name(c);
}

int cat_load(struct sel *S, struct plat *P, int n)
{
    P->cat = sel_alloc(S, sizeof(struct cat_title) * (u64)n);
    if (!P->cat)
        return -1;
    P->ncat = n;
    if (P->library) { /* the games found, in the extension's order: the choice goes back by index */
        for (int i = 0; i < n; i++) {
            struct cat_title *c = &P->cat[i];
            const struct lib_title *l = &P->lib[i];
            strlcpy(c->id, l->id, sizeof(c->id));
            strlcpy(c->path, l->path, sizeof(c->path));
            strlcpy(c->note, l->note, sizeof(c->note));
            c->dir_len = l->dir_len < sizeof(c->path) ? l->dir_len : 0;
            c->flags = l->flags & LIB_TITLE_BAD ? CAT_BAD : 0;
            c->indexed = 1;
            read_sfo(S, P, i);
            profile_name(P, c);
            cat_pick_name(c);
        }
        return n ? 0 : -1;
    }
    if (!P->ghostfs) { /* one title at the app root */
        P->cat[0].indexed = 1;
        read_sfo(S, P, 0);
        profile_name(P, &P->cat[0]);
        cat_pick_name(&P->cat[0]);
        return 0;
    }
    if (P->catalogue) {
        u64 m = sel_mark(S);
        char mp[16], path[64];
        s64 size = -1;
        u8 *buf = NULL;
        if (mount(P, "GFSCATALOG", mp) == 0) {
            size = file_size(P, mp, "catalog.bin", path);
            umount(P, mp);
        }
        if (size > 0 && size <= 0x100000 && (buf = sel_alloc(S, (u64)size)) &&
            read_in(P, "GFSCATALOG", "catalog.bin", 0, buf, (u64)size) == size) {
            const struct cat_hdr *h = (const void *)buf;
            n = (u64)h->header_size + (u64)n * h->entry_size <= (u64)size ? n : 0;
            for (int i = 0; i < n; i++) {
                const u8 *e = buf + h->header_size + (u64)i * h->entry_size;
                struct cat_title *c = &P->cat[i];
                copy_str(c->id, e, 16, sizeof(c->id));
                copy_str(c->prefix, e + 0x10, 16, sizeof(c->prefix));
                copy_str(c->index, e + 0x20, 32, sizeof(c->index));
                copy_str(c->cat_name, e + 0x40, 64, sizeof(c->cat_name));
                copy_str(c->version, e + 0x80, 16, sizeof(c->version));
                memcpy(&c->total, e + 0x98, 8);
                memcpy(&c->containers, e + 0xA0, 4);
                memcpy(&c->flags, e + 0xA4, 4);
            }
        } else {
            n = 0;
        }
        sel_release(S, m);
        P->ncat = n;
        for (int i = 0; i < n; i++) {
            profile_name(P, &P->cat[i]);
            cat_pick_name(&P->cat[i]);
        }
        return n ? 0 : -1;
    }
    u8 h[128];
    if (legacy_header(P, h) != 0)
        return -1;
    struct cat_title *c = &P->cat[0];
    u32 version = *(u32 *)(h + 4);
    copy_str(c->prefix, h + 40, 16, sizeof(c->prefix));
    if (version >= 2)
        copy_str(c->id, h + 64, 16, sizeof(c->id));
    strlcpy(c->index, P->legacy, sizeof(c->index));
    memcpy(&c->total, h + 24, 8);
    memcpy(&c->containers, h + 8, 4);
    c->flags = CAT_LEGACY | (version >= 2 && (*(u32 *)(h + 60) & 1) ? CAT_INCOMPLETE : 0);
    profile_name(P, c);
    cat_pick_name(c);
    return 0;
}

/* ---- a title's index: where its sce_sys files are ---- */
struct gfs_ent {
    u64 off, size;
    u32 name, flags;
};

static void index_title(struct sel *S, struct plat *P, int t)
{
    struct cat_title *c = &P->cat[t];
    c->indexed = 2;
    char mp[16], path[64];
    if (!c->index[0] || mount(P, c->index, mp) != 0)
        return;
    s64 size = file_size(P, mp, "index.bin", path);
    umount(P, mp);
    if (size < 64 || (u64)size > INDEX_MAX)
        return;
    u64 m = sel_mark(S);
    u8 *idx = sel_alloc(S, (u64)size);
    if (idx && read_in(P, c->index, "index.bin", 0, idx, (u64)size) == size && !memcmp(idx, "GFS1", 4)) {
        u32 version = *(u32 *)(idx + 4), entries = *(u32 *)(idx + 12), names_off = *(u32 *)(idx + 32);
        u32 hsize = version >= 2 ? *(u32 *)(idx + 56) : 64;
        u64 cap = *(u64 *)(idx + 16);
        if (hsize >= 64 && (u64)hsize + (u64)entries * sizeof(struct gfs_ent) <= (u64)size && names_off < size &&
            cap) {
            const struct gfs_ent *e = (const void *)(idx + hsize);
            const char *names = (const char *)idx + names_off;
            u64 nlen = (u64)size - names_off;
            c->cap = cap;
            for (int f = 0; f < NFILES; f++) {
                s64 lo = 0, hi = (s64)entries - 1;
                while (lo <= hi) {
                    s64 mid = (lo + hi) / 2;
                    if (e[mid].name >= nlen)
                        break;
                    int d = strcmp(names + e[mid].name, FILES[f]);
                    if (d == 0) {
                        if (!(e[mid].flags & 1))
                            c->file[f].off = e[mid].off, c->file[f].size = e[mid].size;
                        break;
                    }
                    if (d < 0)
                        lo = mid + 1;
                    else
                        hi = mid - 1;
                }
            }
            c->indexed = 1;
        }
    }
    sel_release(S, m);
    if (c->indexed == 1 && c->file[FILE_SFO].size) {
        read_sfo(S, P, t);
        if (!c->prof_name[0])
            profile_name(P, c); /* an ID first learnt from param.sfo */
        cat_pick_name(c);
    }
    LOG(P, "title %d (%s): index %s, icon0 %lu, pic1 %lu, pic0 %lu, param.sfo %lu bytes; '%s' %s", t, c->id,
        c->index, c->file[FILE_ICON].size, c->file[FILE_PIC].size, c->file[FILE_PIC0].size, c->file[FILE_SFO].size,
        c->name, c->version);
}

/* A title's file: its size (buf NULL), or the bytes read into buf (at most cap). */
s64 cat_file(struct sel *S, struct plat *P, int t, int which, void *buf, u64 cap)
{
    const struct ghost_api *a = P->api;
    if (!P->ghostfs) {
        char full[512];
        u8 st[0x80];
        if (P->library)
            fmt(full, sizeof(full), "%s/%s", P->cat[t].path, FILES[which]);
        else
            fmt(full, sizeof(full), "%s/%s", P->root, FILES[which]);
        if (a->vfs_stat(full, st) != 0)
            return -1;
        s64 size = *(s64 *)(st + 0x48);
        if (!buf)
            return size;
        int fd = a->vfs_open(full, O_RDONLY, 0);
        if (fd < 0)
            return -1;
        u64 got = 0, want = (u64)size < cap ? (u64)size : cap;
        while (got < want) {
            s64 r = a->vfs_read(fd, (u8 *)buf + got, want - got);
            if (r <= 0)
                break;
            got += (u64)r;
        }
        a->vfs_close(fd);
        return got == want ? (s64)got : -1;
    }
    struct cat_title *c = &P->cat[t];
    if (!c->indexed)
        index_title(S, P, t);
    u64 size = c->file[which].size, off = c->file[which].off;
    if (c->indexed != 1 || !size)
        return -1;
    if (!buf)
        return (s64)size;
    u64 want = size < cap ? size : cap, done = 0;
    while (done < want) { /* a file can span two containers */
        u64 k = (off + done) / c->cap, local = (off + done) % c->cap;
        u64 take = want - done < c->cap - local ? want - done : c->cap - local;
        char dir[32];
        fmt(dir, sizeof(dir), "%s%03lu", c->prefix, k);
        s64 r = read_in(P, dir, "data.bin", local, (u8 *)buf + done, take);
        if (r != (s64)take)
            return -1;
        done += take;
    }
    return (s64)done;
}
