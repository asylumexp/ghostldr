/*
 * What exFAT and FAT32 share: the FAT, cluster chains walked into runs,
 * reading a file or directory through them, names (UTF-8 <-> UTF-16, case
 * folding) and DOS timestamps.
 *
 * A file's struct fs_walk keeps the runs of its chain it has seen (up to
 * FS_RUNS, the oldest dropped first), so sequential and nearby reads never
 * walk the FAT again; a read before the first kept run starts over from the
 * chain's start.  Clusters that follow each other on disk become one read.
 */
#include "usbstore.h"

static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

u32 fs_fat_next(struct vol *v, u32 cl)
{
    if (cl < 2 || cl - 2 >= v->clusters)
        return 1;
    u32 w = v->fat_bits / 8;
    const u8 *p = blk_peek(v->blk, v->off + v->fat + (u64)cl * w, w, BLK_META);
    if (!p)
        return 1;
    u32 next;
    if (w == 2) {
        next = le16(p);
        if (next >= 0xFFF8)
            return 0;
    } else if (v->exfat) {
        next = le32(p);
        if (next >= 0xFFFFFFF8)
            return 0;
    } else {
        next = le32(p) & 0x0FFFFFFF;
        if (next >= 0x0FFFFFF8)
            return 0;
    }
    if (next < 2 || next - 2 >= v->clusters) {
        v->chain_errors++;
        return 1; /* free, bad (…F7) or out of range in the middle of a chain */
    }
    return next;
}

u32 fs_chain_len(struct vol *v, u32 first, u32 limit)
{
    u32 n = 0;
    for (u32 cl = first; cl >= 2 && n < limit; cl = fs_fat_next(v, cl))
        n++;
    return n;
}

static void walk_reset(struct vol *v, const struct fs_node *n, struct fs_walk *w)
{
    w->first = n->first;
    w->nrun = 1;
    w->run[0] = (struct fs_run){0, n->first, 1};
    w->next = fs_fat_next(v, n->first);
    w->end = w->next == 0;
    w->bad = w->next == 1;
}

u32 fs_map(struct vol *v, const struct fs_node *n, struct fs_walk *w, u32 vcn)
{
    if (!n->first || n->fixed_root)
        return 0;
    if (n->contig) {
        u64 cs = 1ull << v->cluster_shift;
        if ((u64)vcn >= (n->size + cs - 1) >> v->cluster_shift)
            return 0;
        u64 lcn = (u64)n->first + vcn;
        return lcn - 2 < v->clusters ? (u32)lcn : 1;
    }
    if (w->first != n->first || !w->nrun || vcn < w->run[0].vcn)
        walk_reset(v, n, w);
    /* in a kept run? (binary search: runs ascend) */
    u32 lo = 0, hi = w->nrun;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        const struct fs_run *r = &w->run[mid];
        if (vcn < r->vcn)
            hi = mid;
        else if (vcn >= r->vcn + r->len)
            lo = mid + 1;
        else
            return r->lcn + (vcn - r->vcn);
    }
    /* past the last one: walk on */
    for (;;) {
        struct fs_run *last = &w->run[w->nrun - 1];
        u32 end = last->vcn + last->len;
        if (vcn < end)
            return last->lcn + (vcn - last->vcn);
        if (w->end)
            return 0;
        if (w->bad || end > v->clusters) { /* longer than the volume: a loop */
            w->bad = 1;
            return 1;
        }
        u32 cl = w->next;
        if (cl == last->lcn + last->len) {
            last->len++;
        } else {
            if (w->nrun == FS_RUNS) { /* keep the newer half */
                memmove(&w->run[0], &w->run[FS_RUNS / 2], (FS_RUNS - FS_RUNS / 2) * sizeof(w->run[0]));
                w->nrun = FS_RUNS - FS_RUNS / 2;
            }
            w->run[w->nrun++] = (struct fs_run){end, cl, 1};
        }
        w->next = fs_fat_next(v, cl);
        w->end = w->next == 0;
        w->bad = w->next == 1;
    }
}

s64 fs_read(struct vol *v, const struct fs_node *n, struct fs_walk *w, u64 off, void *buf, u64 len, int hint)
{
    struct fs_walk local;
    if (!w) {
        w = &local;
        w->first = 0, w->nrun = 0;
    }
    u8 *out = buf;
    if (n->fixed_root) {
        if (off >= v->root_size)
            return 0;
        return blk_read(v->blk, v->off + v->root_off + off, out, US_MIN(len, v->root_size - off), hint);
    }
    if (off >= n->size)
        return 0;
    len = US_MIN(len, n->size - off);
    u64 cs = 1ull << v->cluster_shift, done = 0;
    while (done < len) {
        u64 pos = off + done;
        if (pos >= n->valid) { /* exFAT: allocated but never written */
            memset(out + done, 0, len - done);
            return (s64)len;
        }
        u64 lim = US_MIN(len - done, n->valid - pos);
        u32 vcn = (u32)(pos >> v->cluster_shift), in = (u32)(pos & (cs - 1));
        u32 lcn = fs_map(v, n, w, vcn);
        if (lcn == 0 && n->size != ~0ull)
            v->chain_errors++; /* a file whose chain is shorter than its size */
        if (lcn < 2)
            return done ? (s64)done : lcn == 0 ? 0 : US_EIO;
        /* clusters that follow each other on disk: one read */
        u64 span = cs - in;
        for (u32 k = 1; span < lim && k < 0x8000; k++) {
            if (fs_map(v, n, w, vcn + k) != lcn + k)
                break;
            span += cs;
        }
        u64 take = US_MIN(span, lim);
        s64 r = blk_read(v->blk, v->off + v->heap + ((u64)(lcn - 2) << v->cluster_shift) + in, out + done, take, hint);
        if (r <= 0)
            return done ? (s64)done : r < 0 ? r : US_EIO;
        done += (u64)r;
    }
    return (s64)done;
}

/* ---- names ---- */

u32 utf16_to_utf8(const u16 *s, u32 n, char *out, u32 cap)
{
    u32 k = 0;
    for (u32 i = 0; i < n; i++) {
        u32 c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        else if (c >= 0xD800 && c <= 0xDFFF)
            c = 0xFFFD; /* an unpaired surrogate */
        u32 need = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (k + need + 1 > cap)
            return 0;
        if (need == 1) {
            out[k++] = (char)c;
        } else if (need == 2) {
            out[k++] = (char)(0xC0 | c >> 6);
            out[k++] = (char)(0x80 | (c & 0x3F));
        } else if (need == 3) {
            out[k++] = (char)(0xE0 | c >> 12);
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[k++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[k++] = (char)(0xF0 | c >> 18);
            out[k++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[k++] = (char)(0x80 | (c & 0x3F));
        }
    }
    if (cap)
        out[k] = 0;
    return k;
}

int utf8_next(const char **pp, const char *end)
{
    const u8 *p = (const u8 *)*pp;
    if ((const char *)p >= end)
        return -1;
    u32 c = *p++, need = 0, min = 0;
    if (c >= 0xF0 && c <= 0xF4)
        need = 3, c &= 0x07, min = 0x10000;
    else if (c >= 0xE0)
        need = 2, c &= 0x0F, min = 0x800;
    else if (c >= 0xC2 && c < 0xE0)
        need = 1, c &= 0x1F, min = 0x80;
    else if (c >= 0x80)
        return -1;
    for (u32 i = 0; i < need; i++) {
        if ((const char *)p >= end || (*p & 0xC0) != 0x80)
            return -1;
        c = c << 6 | (*p++ & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
        return -1;
    *pp = (const char *)p;
    return (int)c;
}

u32 utf8_to_utf16(const char *s, u32 len, u16 *out, u32 cap)
{
    const char *end = s + len;
    u32 k = 0;
    while (s < end) {
        int c = utf8_next(&s, end);
        if (c < 0)
            return 0;
        if (c >= 0x10000) {
            if (k + 2 > cap)
                return 0;
            c -= 0x10000;
            out[k++] = (u16)(0xD800 + (c >> 10));
            out[k++] = (u16)(0xDC00 + (c & 0x3FF));
        } else {
            if (k + 1 > cap)
                return 0;
            out[k++] = (u16)c;
        }
    }
    return k;
}

/* Without the volume's own table (FAT has none): ASCII, Latin-1, Latin
 * Extended-A, Greek (with tonos) and basic Cyrillic. */
static u16 fold(u16 c)
{
    if (c < 0x80)
        return c >= 'a' && c <= 'z' ? (u16)(c - 32) : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
        return (u16)(c - 32);
    if (c == 0xFF)
        return 0x178;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
        return (u16)(c & ~1);
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
        return (c & 1) ? c : (u16)(c - 1);
    if (c >= 0x3B1 && c <= 0x3CB)
        return c == 0x3C2 ? 0x3A3 : (u16)(c - 32); /* final sigma: sigma */
    if (c == 0x3AC)
        return 0x386;
    if (c >= 0x3AD && c <= 0x3AF)
        return (u16)(c - 37);
    if (c == 0x3CC)
        return 0x38C;
    if (c == 0x3CD || c == 0x3CE)
        return (u16)(c - 63);
    if (c >= 0x430 && c <= 0x44F)
        return (u16)(c - 32);
    if (c >= 0x450 && c <= 0x45F)
        return (u16)(c - 80);
    return c;
}

u16 fs_upcase(const struct vol *v, u16 c) { return v->upcase ? v->upcase[c] : fold(c); }

/* ---- times ---- */

static s64 days_from_civil(s64 y, u32 m, u32 d)
{
    y -= m <= 2;
    s64 era = (y >= 0 ? y : y - 399) / 400;
    u32 yoe = (u32)(y - era * 400);
    u32 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (s64)doe - 719468;
}

s64 fs_dos_time(u16 date, u16 time, s32 utc_min)
{
    u32 day = date & 31, month = (date >> 5) & 15, year = 1980 + (date >> 9);
    u32 sec = (time & 31) * 2, min = (time >> 5) & 63, hour = time >> 11;
    if (!date || !day || !month || month > 12 || hour > 23 || min > 59 || sec > 59)
        return 0;
    return days_from_civil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec - (s64)utc_min * 60;
}

/* ---- mounting ---- */

int fs_mount(struct vol *v, struct blk *b, const struct part *p)
{
    u64 off = p->start * b->dev.sector, size = p->sectors * b->dev.sector;
    int rc = exfat_mount(v, b, off, size);
    if (rc == US_ENODEV)
        rc = fat_mount(v, b, off, size);
    return rc;
}

void fs_unmount(struct vol *v)
{
    if (v->upcase)
        us_free(v->upcase, v->upcase_size);
    memset(v, 0, sizeof(*v));
}
