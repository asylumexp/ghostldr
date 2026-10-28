/*
 * A minimal SQLite 3 writer (sqlite_min.h), after the file format document
 * (sqlite.org/fileformat.html): page 1 holds the 100-byte header and
 * sqlite_master (one row: the table); page 2 is the table's root, a leaf when
 * every row fits in one page and otherwise an interior page over the leaves
 * that follow it.  Rows get rowids 1, 2, ...; records use the smallest
 * integer serial types; no overflow pages (a row must fit in a leaf).
 */
#include "sqlite_min.h"

static void be16(u8 *p, u32 v) { p[0] = (u8)(v >> 8), p[1] = (u8)v; }
static void be32(u8 *p, u32 v) { p[0] = (u8)(v >> 24), p[1] = (u8)(v >> 16), p[2] = (u8)(v >> 8), p[3] = (u8)v; }

static u32 varint(u8 *out, u64 v)
{
    u8 tmp[10];
    u32 n = 0;
    if (v >> 56) { /* nine bytes: the last carries 8 bits */
        out[8] = (u8)v;
        v >>= 8;
        for (int i = 7; i >= 0; i--, v >>= 7)
            out[i] = (u8)((v & 0x7F) | 0x80);
        return 9;
    }
    do
        tmp[n++] = (u8)(v & 0x7F), v >>= 7;
    while (v);
    for (u32 i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i] | (i + 1 < n ? 0x80 : 0);
    return n;
}

static u32 int_type(s64 v, u32 *len)
{
    if (v == 0)
        return *len = 0, 8;
    if (v == 1)
        return *len = 0, 9;
    if (v >= -128 && v <= 127)
        return *len = 1, 1;
    if (v >= -32768 && v <= 32767)
        return *len = 2, 2;
    if (v >= -8388608 && v <= 8388607)
        return *len = 3, 3;
    if (v >= -2147483648L && v <= 2147483647L)
        return *len = 4, 4;
    if (v >= -140737488355328L && v <= 140737488355327L)
        return *len = 6, 5;
    return *len = 8, 6;
}

/* a record of n values into out (room bytes); its length, 0 if it doesn't fit */
static u32 record(u8 *out, u32 room, const struct sqlm_val *v, u32 n)
{
    u8 hdr[300];
    u32 h = 0, body = 0;
    for (u32 i = 0; i < n; i++) {
        u32 len;
        u64 t = v[i].s ? 13 + 2 * (u64)strlen(v[i].s) : int_type(v[i].i, &len);
        if (v[i].s)
            len = (u32)strlen(v[i].s);
        if (h + 9 > sizeof(hdr) - 2)
            return 0;
        h += varint(hdr + h, t);
        body += len;
    }
    u32 hlen = h + 1;
    if (hlen > 127)
        hlen = h + 2;
    if (hlen + body > room)
        return 0;
    u32 o = varint(out, hlen);
    memcpy(out + o, hdr, h);
    o += h;
    for (u32 i = 0; i < n; i++) {
        if (v[i].s) {
            u32 len = (u32)strlen(v[i].s);
            memcpy(out + o, v[i].s, len);
            o += len;
            continue;
        }
        u32 len;
        int_type(v[i].i, &len);
        for (u32 k = 0; k < len; k++)
            out[o + k] = (u8)((u64)v[i].i >> (8 * (len - 1 - k)));
        o += len;
    }
    return o;
}

static u8 *page(struct sqlm *db, u32 n) { return db->buf + (u64)(n - 1) * SQLM_PAGE; }

/* a fresh table-leaf page: hdr at off (100 on page 1) */
static void leaf_init(struct sqlm *db, u32 n, u32 off)
{
    u8 *p = page(db, n);
    memset(p + off, 0, 8);
    p[off] = 0x0D;
    be16(p + off + 5, SQLM_PAGE == 65536 ? 0 : SQLM_PAGE);
}

/* add a cell (rowid, record of vals) to leaf n; false if it is full */
static bool leaf_add(struct sqlm *db, u32 n, u32 off, s64 rowid, const struct sqlm_val *vals, u32 nv)
{
    u8 *p = page(db, n);
    u32 cells = (u32)p[off + 3] << 8 | p[off + 4];
    u32 content = (u32)p[off + 5] << 8 | p[off + 6];
    if (content == 0)
        content = 65536;
    u32 ptr_end = off + 8 + 2 * (cells + 1);
    if (content < ptr_end + 32)
        return false;
    u8 rec[SQLM_PAGE];
    u32 rl = record(rec, (u32)sizeof(rec) - 20, vals, nv);
    if (!rl || rl > SQLM_PAGE - 35)
        return false;
    u8 cell[24];
    u32 cl = varint(cell, rl);
    cl += varint(cell + cl, (u64)rowid);
    if (content - ptr_end < cl + rl)
        return false;
    content -= cl + rl;
    memcpy(p + content, cell, cl);
    memcpy(p + content + cl, rec, rl);
    be16(p + off + 8 + 2 * cells, content);
    be16(p + off + 3, cells + 1);
    be16(p + off + 5, content);
    return true;
}

void sqlm_begin(struct sqlm *db, u8 *buf, u64 cap, const char *table, const char *sql, u32 ncols)
{
    memset(db, 0, sizeof(*db));
    db->buf = buf, db->cap = cap, db->ncols = ncols;
    if (cap < 3 * SQLM_PAGE) {
        db->overflow = true;
        return;
    }
    memset(buf, 0, 2 * SQLM_PAGE);
    db->pages = 2;
    leaf_init(db, 1, 100);
    struct sqlm_val m[5] = {{"table", 0}, {table, 0}, {table, 0}, {NULL, 2}, {sql, 0}};
    if (!leaf_add(db, 1, 100, 1, m, 5))
        db->overflow = true;
}

bool sqlm_row(struct sqlm *db, const struct sqlm_val *v)
{
    if (db->overflow)
        return false;
    s64 rowid = db->rowid + 1;
    if (db->leaf && leaf_add(db, db->leaf, 0, rowid, v, db->ncols)) {
        db->rowid = rowid, db->last_rowid[db->nleaves - 1] = rowid;
        return true;
    }
    /* a new leaf: page 3 on (page 2 becomes the interior root at the end) */
    if ((u64)(db->pages + 1) * SQLM_PAGE > db->cap || db->nleaves == 512) {
        db->overflow = true;
        return false;
    }
    u32 n = ++db->pages;
    leaf_init(db, n, 0);
    if (!leaf_add(db, n, 0, rowid, v, db->ncols)) {
        db->overflow = true; /* a row bigger than a page */
        return false;
    }
    db->leaf = n;
    db->leaves[db->nleaves] = n;
    db->last_rowid[db->nleaves++] = rowid;
    db->rowid = rowid;
    return true;
}

u64 sqlm_end(struct sqlm *db)
{
    if (db->overflow)
        return 0;
    u8 *root = page(db, 2);
    memset(root, 0, SQLM_PAGE);
    if (db->nleaves <= 1) {
        /* one leaf or none: it becomes page 2 itself */
        if (db->nleaves == 1) {
            memcpy(root, page(db, db->leaves[0]), SQLM_PAGE);
            db->pages = 2;
        } else
            leaf_init(db, 2, 0);
    } else {
        /* interior root: one cell (left child, its largest rowid) per leaf but
         * the last, which is the right-most pointer */
        root[0] = 0x05;
        u32 content = SQLM_PAGE, cells = db->nleaves - 1;
        for (u32 i = 0; i < cells; i++) {
            u8 cell[16];
            be32(cell, db->leaves[i]);
            u32 cl = 4 + varint(cell + 4, (u64)db->last_rowid[i]);
            content -= cl;
            memcpy(root + content, cell, cl);
            be16(root + 12 + 2 * i, content);
        }
        be16(root + 3, cells);
        be16(root + 5, content);
        be32(root + 8, db->leaves[db->nleaves - 1]);
    }
    u8 *h = db->buf;
    memcpy(h, "SQLite format 3", 16);
    be16(h + 16, SQLM_PAGE == 65536 ? 1 : SQLM_PAGE);
    h[18] = 1, h[19] = 1, h[20] = 0, h[21] = 64, h[22] = 32, h[23] = 32;
    be32(h + 24, 1);
    be32(h + 28, db->pages);
    be32(h + 40, 1); /* schema cookie */
    be32(h + 44, 4); /* schema format */
    be32(h + 56, 1); /* UTF-8 */
    be32(h + 92, 1);
    be32(h + 96, 3039004);
    return (u64)db->pages * SQLM_PAGE;
}
