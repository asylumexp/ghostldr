/*
 * Read-only FAT32, and FAT16 (small cards), with long file names.
 *
 *   - The BPB gives the FAT, the data region and the root directory: a
 *     cluster chain on FAT32, a fixed region on FAT16.  The cluster count
 *     decides the FAT type, as the specification says; FAT12 (volumes under
 *     about 16 MiB) is refused.  With mirroring off, ExtFlags names the
 *     active FAT.
 *   - Directory entries are 32 bytes.  Long names are runs of LFN entries
 *     (13 UTF-16 units each, last one first) before their short entry,
 *     accepted when the sequence is whole and its checksum matches the
 *     short name; otherwise the 8.3 name is used (lower-case flags applied,
 *     bytes above 0x7F read as code page 437).
 *   - Lookups compare names case-insensitively (fsutil.c's folding: FAT
 *     has no up-case table).
 *   - Times are local times with no zone: they are read as UTC.
 */
#include "usbstore.h"

static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static const u16 k_cp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE,
    0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6,
    0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA,
    0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502,
    0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514,
    0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550,
    0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C,
    0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320,
    0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

/* the 32-byte entry at byte position pos of dir, NULL past its end */
static const u8 *entry_at(struct vol *v, const struct fs_node *dir, struct fs_walk *w, u64 pos)
{
    if (dir->fixed_root)
        return pos + 32 <= v->root_size ? blk_peek(v->blk, v->off + v->root_off + pos, 32, BLK_META) : NULL;
    u32 lcn = fs_map(v, dir, w, (u32)(pos >> v->cluster_shift));
    if (lcn < 2)
        return NULL;
    u64 in = pos & ((1ull << v->cluster_shift) - 1);
    return blk_peek(v->blk, v->off + v->heap + ((u64)(lcn - 2) << v->cluster_shift) + in, 32, BLK_META);
}

static u8 sfn_sum(const u8 *e)
{
    u8 s = 0;
    for (int i = 0; i < 11; i++)
        s = (u8)(((s & 1) << 7) + (s >> 1) + e[i]);
    return s;
}

static u32 short_name(const u8 *e, u16 *out)
{
    u32 n = 0, base = 8, ext = 3;
    while (base && e[base - 1] == ' ')
        base--;
    while (ext && e[8 + ext - 1] == ' ')
        ext--;
    for (u32 i = 0; i < base + ext; i++) {
        if (i == base)
            out[n++] = '.';
        u8 c = e[i < base ? i : 8 + i - base];
        if (!i && c == 0x05)
            c = 0xE5;
        bool lower = i < base ? (e[12] & 0x08) : (e[12] & 0x10);
        out[n++] = c >= 0x80 ? k_cp437[c - 0x80] : (lower && c >= 'A' && c <= 'Z') ? (u16)(c + 32) : c;
    }
    return n;
}

struct lfn {
    u16 name[260];
    u8 sum, next, count, ok;
};

/* the next named entry at *pos or after: 1 (its short entry in sfn, name in
 * name), 0 at the end; *pos moves past it */
static int next_entry(struct vol *v, const struct fs_node *dir, struct fs_walk *w, u64 *pos, u8 sfn[32], u64 *at,
                      u16 *name, u32 *nlen)
{
    struct lfn l;
    l.ok = 0;
    for (;;) {
        const u8 *e = entry_at(v, dir, w, *pos);
        if (!e || e[0] == 0)
            return 0;
        *pos += 32;
        if (e[0] == 0xE5) {
            l.ok = 0;
            continue;
        }
        if ((e[11] & 0x3F) == 0x0F) {
            u8 ord = e[0] & 0x1F;
            if (e[0] & 0x40) {
                l.ok = ord >= 1 && ord <= 20;
                l.count = ord, l.sum = e[13], l.next = ord;
                for (u32 i = 0; i < 260; i++)
                    l.name[i] = 0xFFFF;
            } else if (!l.ok || ord != l.next || e[13] != l.sum) {
                l.ok = 0;
            }
            if (l.ok && ord == l.next && ord >= 1) {
                u16 *d = l.name + (ord - 1) * 13;
                for (int i = 0; i < 5; i++)
                    d[i] = le16(e + 1 + 2 * i);
                for (int i = 0; i < 6; i++)
                    d[5 + i] = le16(e + 14 + 2 * i);
                for (int i = 0; i < 2; i++)
                    d[11 + i] = le16(e + 28 + 2 * i);
                l.next = ord - 1;
            }
            continue;
        }
        if ((e[11] & 0x08) || (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' ')))) {
            l.ok = 0; /* a volume label, "." or ".." */
            continue;
        }
        memcpy(sfn, e, 32);
        *at = *pos - 32;
        if (l.ok && l.next == 0 && l.sum == sfn_sum(e)) {
            u32 n = 0;
            while (n < (u32)l.count * 13 && l.name[n] && l.name[n] != 0xFFFF)
                n++;
            memcpy(name, l.name, n * 2);
            *nlen = n;
        } else {
            *nlen = short_name(e, name);
        }
        if (*nlen)
            return 1;
        l.ok = 0;
    }
}

static void entry_node(struct vol *v, const struct fs_node *dir, const u8 *e, u64 at, struct fs_node *n)
{
    memset(n, 0, sizeof(*n));
    n->attr = e[11];
    n->dir = (e[11] & 0x10) != 0;
    n->first = (v->fat_bits == 32 ? (u32)le16(e + 20) << 16 : 0) | le16(e + 26);
    n->size = n->valid = n->dir ? ~0ull : le32(e + 28);
    if (n->first && n->first - 2 >= v->clusters)
        n->first = 0, v->chain_errors++;
    if (!n->first && !n->dir)
        n->size = n->valid = 0;
    n->mtime = fs_dos_time(le16(e + 24), le16(e + 22), 0);
    n->btime = fs_dos_time(le16(e + 16), le16(e + 14), 0);
    if (n->btime)
        n->btime += e[13] / 100, n->btime_ns = (u32)(e[13] % 100) * 10000000u;
    n->atime = fs_dos_time(le16(e + 18), 0, 0);
    n->ino = (u32)(dir->first * 0x9E3779B1u) ^ (u32)(at >> 5) ^ 0x20000u;
}

static int fat_lookup(struct vol *v, const struct fs_node *dir, const char *name, u32 len, struct fs_node *out)
{
    u16 want[256], have[260];
    u32 n = utf8_to_utf16(name, len, want, 255), hn;
    if (!n)
        return len > 255 ? US_ENAMETOOLONG : US_ENOENT;
    for (u32 i = 0; i < n; i++)
        want[i] = fs_upcase(v, want[i]);
    struct fs_walk w = {0};
    u8 e[32];
    u64 pos = 0, at;
    while (next_entry(v, dir, &w, &pos, e, &at, have, &hn) == 1) {
        if (hn != n)
            continue;
        u32 i = 0;
        while (i < n && fs_upcase(v, have[i]) == want[i])
            i++;
        if (i == n) {
            entry_node(v, dir, e, at, out);
            return 0;
        }
    }
    return US_ENOENT;
}

static int fat_readdir(struct vol *v, const struct fs_node *dir, struct fs_walk *w, u64 *cookie,
                       struct fs_dirent *out)
{
    u16 name[260];
    u8 e[32];
    u32 n;
    u64 at;
    while (next_entry(v, dir, w, cookie, e, &at, name, &n) == 1) {
        out->len = utf16_to_utf8(name, n, out->name, sizeof(out->name));
        if (!out->len)
            continue;
        entry_node(v, dir, e, at, &out->node);
        return 1;
    }
    return 0;
}

static const struct fs_ops k_fat32 = {"FAT32", fat_lookup, fat_readdir};
static const struct fs_ops k_fat16 = {"FAT16", fat_lookup, fat_readdir};

int fat_mount(struct vol *v, struct blk *b, u64 off, u64 size)
{
    u8 bs[512];
    memset(v, 0, sizeof(*v));
    v->blk = b, v->off = off, v->size = size;
    if (blk_read(b, off, bs, 512, BLK_META) != 512)
        return US_EIO;
    u32 bps = le16(bs + 11), spc = bs[13], rsvd = le16(bs + 14), nfats = bs[16], root_ents = le16(bs + 17);
    u32 fatsz = le16(bs + 22) ? le16(bs + 22) : le32(bs + 36);
    u32 total = le16(bs + 19) ? le16(bs + 19) : le32(bs + 32);
    if ((bs[0] != 0xEB && bs[0] != 0xE9) || bs[510] != 0x55 || bs[511] != 0xAA || bps < 512 || bps > 4096 ||
        (bps & (bps - 1)) || !spc || (spc & (spc - 1)) || !rsvd || !nfats || nfats > 4 || !fatsz || !total ||
        (u64)total * bps > size)
        return US_ENODEV;
    u32 root_secs = (root_ents * 32 + bps - 1) / bps;
    u64 data = (u64)rsvd + (u64)nfats * fatsz + root_secs;
    if (data >= total)
        return US_ENODEV;
    u32 clusters = (u32)((total - data) / spc);
    if (clusters < 4085) {
        us_log("usb: FAT12 volume (%u clusters): not supported, format the drive exFAT or FAT32", clusters);
        return US_ENODEV;
    }
    bool fat32 = clusters >= 65525;
    if (fat32 && (root_ents || le16(bs + 22)))
        return US_ENODEV;
    /* the FAT must hold an entry for every cluster */
    if ((u64)fatsz * bps < ((u64)clusters + 2) * (fat32 ? 4 : 2))
        return US_ENODEV;
    u32 active = 0;
    if (fat32 && (le16(bs + 40) & 0x80))
        active = le16(bs + 40) & 0x0F;
    if (active >= nfats)
        active = 0;
    v->sector = bps;
    v->cluster_shift = 0;
    while ((1u << v->cluster_shift) < bps * spc)
        v->cluster_shift++;
    v->fat = ((u64)rsvd + (u64)active * fatsz) * bps;
    v->heap = data * bps;
    v->clusters = clusters;
    v->fat_bits = fat32 ? 32 : 16;
    v->ops = fat32 ? &k_fat32 : &k_fat16;
    struct fs_node *r = &v->root;
    r->dir = 1, r->attr = 0x10, r->ino = 2;
    r->size = r->valid = ~0ull;
    if (fat32) {
        r->first = le32(bs + 44);
        if (r->first < 2 || r->first - 2 >= clusters)
            return US_ENODEV;
        v->serial = le32(bs + 67);
    } else {
        r->fixed_root = 1;
        v->root_off = ((u64)rsvd + (u64)nfats * fatsz) * bps;
        v->root_size = (u64)root_ents * 32;
        v->serial = le32(bs + 39);
    }
    /* the label: the root directory's volume entry, else the BPB's */
    const u8 *lab = bs + (fat32 ? 71 : 43);
    struct fs_walk w = {0};
    for (u64 pos = 0; pos < 64 * 32; pos += 32) {
        const u8 *e = entry_at(v, r, &w, pos);
        if (!e || !e[0])
            break;
        if (e[0] != 0xE5 && (e[11] & 0x3F) == 0x08) {
            memcpy(bs, e, 11);
            lab = bs;
            break;
        }
    }
    u16 l[11];
    u32 n = 11;
    while (n && lab[n - 1] == ' ')
        n--;
    for (u32 i = 0; i < n; i++)
        l[i] = lab[i] >= 0x80 ? k_cp437[lab[i] - 0x80] : lab[i];
    utf16_to_utf8(l, n, v->label, sizeof(v->label));
    return 0;
}
