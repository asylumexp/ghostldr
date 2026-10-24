/*
 * Read-only exFAT (Microsoft's exFAT specification).
 *
 *   - The boot sector names the FAT, the cluster heap and the root
 *     directory's first cluster; its checksum (sector 11) is checked, a
 *     mismatch only logged.  With two FATs, VolumeFlags.ActiveFat picks one.
 *   - Directories are arrays of 32-byte entries.  A file is an entry set: a
 *     File entry (attributes, times), a Stream Extension (size, valid data
 *     length, first cluster, NoFatChain, name length and hash) and File
 *     Name entries of 15 UTF-16 units each.  The set checksum is checked;
 *     a broken set is skipped.  Unused (bit 7 clear) and other entries are
 *     stepped over.
 *   - Lookups compare the up-cased name's hash first, then the up-cased
 *     names, with the volume's up-case table (read at mount, expanded to
 *     65536 entries; built-in folding when it is missing or damaged).
 *   - A NoFatChain file's clusters are consecutive from its first; other
 *     files and the root directory follow the FAT (fsutil.c).  Bytes past
 *     ValidDataLength read as zeros.
 *   - The allocation bitmap is not needed for reading and is never read.
 */
#include "usbstore.h"

static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 le64(const u8 *p) { return le32(p) | (u64)le32(p + 4) << 32; }

#define T_EOD 0x00
#define T_BITMAP 0x81
#define T_UPCASE 0x82
#define T_LABEL 0x83
#define T_FILE 0x85
#define T_STREAM 0xC0
#define T_NAME 0xC1

/* one file's entry set, as read from its directory */
struct set {
    u8 e[19][32];          /* File, Stream, up to 17 File Name entries */
    u32 n;                 /* entries in the set */
    u64 pos;               /* byte position of its File entry in the directory */
};

/* the entry at byte position pos of dir: a pointer into the cache, NULL at
 * the end of the directory's clusters */
static const u8 *entry_at(struct vol *v, const struct fs_node *dir, struct fs_walk *w, u64 pos)
{
    if (pos >= dir->size)
        return NULL;
    u32 lcn = fs_map(v, dir, w, (u32)(pos >> v->cluster_shift));
    if (lcn < 2)
        return NULL;
    u64 in = pos & ((1ull << v->cluster_shift) - 1);
    return blk_peek(v->blk, v->off + v->heap + ((u64)(lcn - 2) << v->cluster_shift) + in, 32, BLK_META);
}

static u16 set_checksum(const struct set *s)
{
    u16 c = 0;
    for (u32 i = 0; i < s->n; i++)
        for (u32 k = 0; k < 32; k++)
            if (i || (k != 2 && k != 3))
                c = (u16)(((c & 1) ? 0x8000 : 0) + (c >> 1) + s->e[i][k]);
    return c;
}

/* the next file entry set at *pos or after: 1 (s filled, *pos past it), 0 at the end */
static int next_set(struct vol *v, const struct fs_node *dir, struct fs_walk *w, u64 *pos, struct set *s)
{
    static u32 broken;
    for (;;) {
        const u8 *e = entry_at(v, dir, w, *pos);
        if (!e || e[0] == T_EOD)
            return 0;
        if (e[0] != T_FILE) {
            *pos += 32;
            continue;
        }
        u32 sc = e[1];
        s->pos = *pos;
        memcpy(s->e[0], e, 32);
        s->n = 1;
        bool ok = sc >= 2 && sc <= 18;
        for (u32 i = 1; ok && i <= sc; i++) {
            const u8 *x = entry_at(v, dir, w, *pos + 32ull * i);
            ok = x && (x[0] & 0x80) && (x[0] & 0x40); /* in use, secondary */
            if (ok)
                memcpy(s->e[i], x, 32), s->n++;
        }
        ok = ok && s->e[1][0] == T_STREAM && set_checksum(s) == le16(s->e[0] + 2);
        u32 names = ok ? (s->e[1][3] + 14u) / 15 : 0;
        ok = ok && s->e[1][3] && names + 2 <= s->n;
        for (u32 i = 0; ok && i < names; i++)
            ok = s->e[2 + i][0] == T_NAME;
        if (!ok) {
            if (broken++ < 16)
                us_log("usb: exFAT: damaged entry set at %lx in the directory at cluster %u: skipped", *pos,
                       dir->first);
            *pos += 32;
            continue;
        }
        *pos += 32ull * s->n;
        return 1;
    }
}

static u32 set_name(const struct set *s, u16 *name)
{
    u32 len = s->e[1][3];
    for (u32 i = 0; i < len; i++)
        name[i] = le16(s->e[2 + i / 15] + 2 + 2 * (i % 15));
    return len;
}

static s64 ex_time(u32 ts, u8 ms10, u8 utc)
{
    s32 off = 0;
    if (utc & 0x80) /* OffsetValid: 15-minute steps, signed 7 bits */
        off = ((s32)(utc & 0x7F) ^ 0x40) - 0x40;
    s64 t = fs_dos_time((u16)(ts >> 16), (u16)ts, off * 15);
    return t ? t + ms10 / 100 : 0;
}

static void set_node(struct vol *v, const struct fs_node *dir, const struct set *s, struct fs_node *n)
{
    const u8 *f = s->e[0], *st = s->e[1];
    memset(n, 0, sizeof(*n));
    u16 attr = le16(f + 4);
    n->attr = (u8)attr;
    n->dir = (attr & 0x10) != 0;
    bool alloc = st[1] & 1;
    n->contig = (st[1] & 2) != 0;
    n->first = alloc ? le32(st + 20) : 0;
    n->size = le64(st + 24);
    n->valid = US_MIN(le64(st + 8), n->size);
    if (n->dir)
        n->valid = n->size;
    if (n->first && (n->first < 2 || n->first - 2 >= v->clusters))
        n->first = 0, n->size = n->valid = 0, v->chain_errors++;
    n->btime = ex_time(le32(f + 8), f[20], f[22]);
    n->btime_ns = (u32)(f[20] % 100) * 10000000u;
    n->mtime = ex_time(le32(f + 12), f[21], f[23]);
    n->mtime_ns = (u32)(f[21] % 100) * 10000000u;
    n->atime = ex_time(le32(f + 16), 0, f[24]);
    /* stable and nonzero: where its entry set is */
    n->ino = (u32)(dir->first * 0x9E3779B1u) ^ (u32)(s->pos >> 5) ^ 0x10000u;
}

static int ex_lookup(struct vol *v, const struct fs_node *dir, const char *name, u32 len, struct fs_node *out)
{
    u16 want[256], have[256];
    u32 n = utf8_to_utf16(name, len, want, 255);
    if (!n)
        return len > 255 ? US_ENAMETOOLONG : US_ENOENT;
    u16 hash = 0;
    for (u32 i = 0; i < n; i++) {
        want[i] = fs_upcase(v, want[i]);
        hash = (u16)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (want[i] & 0xFF));
        hash = (u16)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (want[i] >> 8));
    }
    struct fs_walk w = {0};
    struct set s;
    u64 pos = 0;
    while (next_set(v, dir, &w, &pos, &s) == 1) {
        if (s.e[1][3] != n || le16(s.e[1] + 4) != hash)
            continue;
        set_name(&s, have);
        u32 i = 0;
        while (i < n && fs_upcase(v, have[i]) == want[i])
            i++;
        if (i == n) {
            set_node(v, dir, &s, out);
            return 0;
        }
    }
    return US_ENOENT;
}

static int ex_readdir(struct vol *v, const struct fs_node *dir, struct fs_walk *w, u64 *cookie,
                      struct fs_dirent *out)
{
    struct set s;
    u16 name[256];
    for (;;) {
        if (next_set(v, dir, w, cookie, &s) != 1)
            return 0;
        u32 n = set_name(&s, name);
        out->len = utf16_to_utf8(name, n, out->name, sizeof(out->name));
        if (!out->len)
            continue;
        set_node(v, dir, &s, &out->node);
        return 1;
    }
}

static const struct fs_ops k_exfat = {"exFAT", ex_lookup, ex_readdir};

/* boot region checksum: sectors 0-10, without VolumeFlags and PercentInUse */
static bool boot_checksum_ok(struct vol *v)
{
    u32 ss = v->sector, c = 0;
    u8 buf[512];
    for (u32 sec = 0; sec < 11; sec++)
        for (u32 at = 0; at < ss; at += 512) {
            if (blk_read(v->blk, v->off + (u64)sec * ss + at, buf, 512, BLK_META) != 512)
                return false;
            for (u32 k = 0; k < 512; k++)
                if (sec || (at + k != 106 && at + k != 107 && at + k != 112))
                    c = ((c & 1) ? 0x80000000u : 0) + (c >> 1) + buf[k];
        }
    if (blk_read(v->blk, v->off + 11ull * ss, buf, 4, BLK_META) != 4)
        return false;
    return le32(buf) == c;
}

/* the up-case table: compressed runs (0xFFFF, n: n identity mappings) */
static void load_upcase(struct vol *v, u32 first, u64 size, u32 sum)
{
    if (!first || size < 2 || size > (1u << 18))
        return;
    struct fs_node n = {0};
    n.first = first, n.size = n.valid = size;
    u8 *raw = us_alloc(size);
    u16 *t = us_alloc(65536 * 2);
    bool ok = raw && t && fs_read(v, &n, NULL, 0, raw, size, BLK_META) == (s64)size;
    u32 c = 0;
    for (u64 k = 0; ok && k < size; k++)
        c = ((c & 1) ? 0x80000000u : 0) + (c >> 1) + raw[k];
    if (ok && c != sum)
        us_log("usb: exFAT up-case table checksum %x, expected %x: using it anyway", c, sum);
    u32 at = 0;
    for (u64 k = 0; ok && k + 1 < size && at < 65536; k += 2) {
        u16 x = le16(raw + k);
        if (x == 0xFFFF && k + 3 < size) {
            u32 run = le16(raw + k + 2);
            for (u32 i = 0; i < run && at < 65536; i++, at++)
                t[at] = (u16)at;
            k += 2;
        } else {
            t[at++] = x;
        }
    }
    for (; ok && at < 65536; at++)
        t[at] = (u16)at;
    if (raw)
        us_free(raw, size);
    if (!ok) {
        if (t)
            us_free(t, 65536 * 2);
        us_log("usb: exFAT up-case table unreadable: built-in case folding");
        return;
    }
    v->upcase = t;
    v->upcase_size = 65536 * 2;
}

int exfat_mount(struct vol *v, struct blk *b, u64 off, u64 size)
{
    u8 bs[512];
    memset(v, 0, sizeof(*v));
    v->blk = b, v->off = off, v->size = size;
    if (blk_read(b, off, bs, 512, BLK_META) != 512)
        return US_EIO;
    if (memcmp(bs + 3, "EXFAT   ", 8) != 0)
        return US_ENODEV;
    for (int i = 11; i < 64; i++)
        if (bs[i])
            return US_ENODEV;
    u32 bps = bs[108], spc = bs[109];
    u32 fat_off = le32(bs + 80), fat_len = le32(bs + 84), heap = le32(bs + 88), count = le32(bs + 92);
    u32 root = le32(bs + 96);
    if (bps < 9 || bps > 12 || bps + spc > 25 || !count || count > 0xFFFFFFF5u || bs[110] < 1 || bs[110] > 2 ||
        ((u64)heap << bps) + ((u64)count << (bps + spc)) > size || root < 2 || root - 2 >= count) {
        us_log("usb: exFAT boot sector out of range (sector 2^%u, cluster 2^%u, %u clusters)", bps, bps + spc, count);
        return US_ENODEV;
    }
    v->sector = 1u << bps;
    v->cluster_shift = bps + spc;
    v->fat = (u64)fat_off << bps;
    if ((le16(bs + 106) & 1) && bs[110] == 2) /* ActiveFat: the second */
        v->fat += (u64)fat_len << bps;
    v->heap = (u64)heap << bps;
    v->clusters = count;
    v->fat_bits = 32;
    v->exfat = 1;
    v->serial = le32(bs + 100);
    if (!boot_checksum_ok(v))
        us_log("usb: exFAT boot checksum mismatch: mounting anyway (read-only)");
    v->ops = &k_exfat;
    struct fs_node *r = &v->root;
    r->first = root, r->dir = 1, r->attr = 0x10, r->ino = 2;
    r->size = r->valid = (u64)fs_chain_len(v, root, count) << v->cluster_shift;
    /* the root directory's critical entries: up-case table, label */
    struct fs_walk w = {0};
    for (u64 pos = 0;; pos += 32) {
        const u8 *e = entry_at(v, r, &w, pos);
        if (!e || e[0] == T_EOD)
            break;
        if (e[0] == T_UPCASE && !v->upcase)
            load_upcase(v, le32(e + 20), le64(e + 24), le32(e + 4));
        else if (e[0] == T_LABEL && e[1] <= 11) {
            u16 l[11];
            for (u32 i = 0; i < e[1]; i++)
                l[i] = le16(e + 2 + 2 * i);
            utf16_to_utf8(l, e[1], v->label, sizeof(v->label));
        }
    }
    return 0;
}
