/*
 * A title's <prefix>META/meta.bin (host/gfs_meta.py): its name and art,
 * already at the sizes the selector draws, and the QOI images in it
 * (https://qoiformat.org).  Shared by the module and the offline preview.
 */
#include "sel.h"

static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u32 be32(const u8 *p) { return (u32)p[3] | (u32)p[2] << 8 | (u32)p[1] << 16 | (u32)p[0] << 24; }

/* The header; 0 when d holds one.  n may be the whole file or its start. */
int meta_parse(const u8 *d, u64 n, struct meta_info *mi)
{
    __builtin_memset(mi, 0, sizeof(*mi));
    if (n < META_HEADER || d[0] != 'G' || d[1] != 'F' || d[2] != 'S' || d[3] != 'M' || le32(d + 8) < META_HEADER)
        return -1;
    mi->flags = le32(d + 0x0C);
    mi->size = le32(d + 0x30);
    mi->name_source = d[0x34];
    for (int i = 0; i < 127 && d[0x40 + i]; i++)
        mi->name[i] = (char)d[0x40 + i];
    for (int i = 0; i < 15 && d[0xF0 + i]; i++)
        mi->version[i] = (char)d[0xF0 + i];
    for (int s = 0; s < META_SECTIONS; s++) {
        const u8 *e = d + 0xC0 + 16 * s;
        struct meta_sect *m = &mi->s[s];
        m->off = le32(e), m->size = le32(e + 4);
        m->w = (u16)(e[8] | e[9] << 8), m->h = (u16)(e[10] | e[11] << 8);
        m->format = e[12], m->source = e[13];
        if (m->size && ((u64)m->off + m->size > mi->size || m->off < META_HEADER))
            m->size = 0; /* points outside the file: ignore it */
    }
    return 0;
}

int qoi_header(const u8 *d, u64 n, int *w, int *h)
{
    if (n < 22 || d[0] != 'q' || d[1] != 'o' || d[2] != 'i' || d[3] != 'f')
        return -1;
    u32 W = be32(d + 4), H = be32(d + 8);
    if (!W || !H || W > 4096 || H > 4096)
        return -1;
    *w = (int)W, *h = (int)H;
    return 0;
}

/* Decode into out (its w and h must be the image's; px allocated) as
 * 0xAARRGGBB.  A truncated stream repeats the last pixel; -1 for a bad header. */
int qoi_decode(const u8 *d, u64 n, surface *out)
{
    int w, h;
    if (qoi_header(d, n, &w, &h) != 0 || w != out->w || h != out->h)
        return -1;
    u32 index[64];
    __builtin_memset(index, 0, sizeof(index));
    u32 r = 0, g = 0, b = 0, a = 255;
    u64 p = 14, end = n - 8; /* every op reads at most 5 bytes: the end marker keeps them in bounds */
    int run = 0;
    for (int y = 0; y < h; y++) {
        u32 *row = out->px + (u64)y * (u64)out->stride;
        for (int x = 0; x < w; x++) {
            if (run > 0) {
                run--;
            } else if (p < end) {
                u32 c = d[p++];
                if (c == 0xFE) {
                    r = d[p], g = d[p + 1], b = d[p + 2];
                    p += 3;
                } else if (c == 0xFF) {
                    r = d[p], g = d[p + 1], b = d[p + 2], a = d[p + 3];
                    p += 4;
                } else if ((c >> 6) == 0) {
                    u32 v = index[c];
                    a = v >> 24, r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
                } else if ((c >> 6) == 1) {
                    r = (r + ((c >> 4) & 3) - 2) & 255;
                    g = (g + ((c >> 2) & 3) - 2) & 255;
                    b = (b + (c & 3) - 2) & 255;
                } else if ((c >> 6) == 2) {
                    u32 e = d[p++], dg = (c & 63) - 32;
                    r = (r + dg + (e >> 4) - 8) & 255;
                    g = (g + dg) & 255;
                    b = (b + dg + (e & 15) - 8) & 255;
                } else {
                    run = (int)(c & 63);
                }
                index[(r * 3 + g * 5 + b * 7 + a * 11) & 63] = a << 24 | r << 16 | g << 8 | b;
            }
            row[x] = a << 24 | r << 16 | g << 8 | b;
        }
    }
    return 0;
}

/* ---- writing (art.c: Game art's pictures, kept with the title) ---- */

static void put32le(u8 *p, u32 v) { p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24); }
static void put32be(u8 *p, u32 v) { p[3] = (u8)v, p[2] = (u8)(v >> 8), p[1] = (u8)(v >> 16), p[0] = (u8)(v >> 24); }

/* worst case: a byte of tag and four of colour per pixel, the header and the end */
u64 qoi_max(int w, int h) { return (u64)w * (u64)h * 5 + 14 + 8; }

/* in (0xAARRGGBB) as QOI into out (qoi_max bytes): its length */
u64 qoi_encode(const surface *in, u8 *out)
{
    u8 *o = out;
    o[0] = 'q', o[1] = 'o', o[2] = 'i', o[3] = 'f';
    put32be(o + 4, (u32)in->w), put32be(o + 8, (u32)in->h);
    o[12] = 4, o[13] = 0; /* RGBA, sRGB */
    u64 n = 14;
    u32 index[64];
    __builtin_memset(index, 0, sizeof(index));
    u32 prev = 0xFF000000u;
    int run = 0;
    for (int y = 0; y < in->h; y++) {
        const u32 *row = in->px + (u64)y * (u64)in->stride;
        for (int x = 0; x < in->w; x++) {
            u32 c = row[x];
            int last = y == in->h - 1 && x == in->w - 1;
            if (c == prev) {
                if (++run == 62 || last)
                    out[n++] = (u8)(0xC0 | (run - 1)), run = 0;
                continue;
            }
            if (run)
                out[n++] = (u8)(0xC0 | (run - 1)), run = 0;
            u32 a = c >> 24, r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
            int h = (int)((r * 3 + g * 5 + b * 7 + a * 11) & 63);
            if (index[h] == c) {
                out[n++] = (u8)h;
            } else {
                index[h] = c;
                if (a == prev >> 24) {
                    int dr = (int)r - (int)((prev >> 16) & 255), dg = (int)g - (int)((prev >> 8) & 255),
                        db = (int)b - (int)(prev & 255);
                    dr = (signed char)dr, dg = (signed char)dg, db = (signed char)db; /* wrapped, as the decoder adds */
                    int dr_dg = dr - dg, db_dg = db - dg;
                    if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1) {
                        out[n++] = (u8)(0x40 | (dr + 2) << 4 | (dg + 2) << 2 | (db + 2));
                    } else if (dg >= -32 && dg <= 31 && dr_dg >= -8 && dr_dg <= 7 && db_dg >= -8 && db_dg <= 7) {
                        out[n++] = (u8)(0x80 | (dg + 32));
                        out[n++] = (u8)((dr_dg + 8) << 4 | (db_dg + 8));
                    } else {
                        out[n++] = 0xFE, out[n++] = (u8)r, out[n++] = (u8)g, out[n++] = (u8)b;
                    }
                } else {
                    out[n++] = 0xFF, out[n++] = (u8)r, out[n++] = (u8)g, out[n++] = (u8)b, out[n++] = (u8)a;
                }
            }
            prev = c;
        }
    }
    for (int i = 0; i < 7; i++)
        out[n++] = 0;
    out[n++] = 1;
    return n;
}

/* A whole meta.bin (host/gfs_meta.py's layout) into out (cap bytes): the
 * header's fields from mi (flags, name and its source, version), title's ID,
 * then the three sections' bytes (sect[i], len[i]; NULL: none) with mi's
 * sizes, formats and sources.  Its length, or 0 when it doesn't fit. */
u64 meta_build(const struct meta_info *mi, const char *title, u64 written, const u8 *const sect[META_SECTIONS],
               const u32 len[META_SECTIONS], u8 *out, u64 cap)
{
    u64 n = META_HEADER;
    for (int s = 0; s < META_SECTIONS; s++)
        n += sect[s] ? (len[s] + 15) & ~15u : 0;
    if (n > cap || n > 0xFFFFFFFFu)
        return 0;
    __builtin_memset(out, 0, META_HEADER);
    out[0] = 'G', out[1] = 'F', out[2] = 'S', out[3] = 'M';
    put32le(out + 4, 1), put32le(out + 8, META_HEADER), put32le(out + 0x0C, mi->flags);
    for (int i = 0; i < 15 && title[i]; i++)
        out[0x10 + i] = (u8)title[i];
    put32le(out + 0x20, (u32)written), put32le(out + 0x24, (u32)(written >> 32));
    put32le(out + 0x30, (u32)n);
    out[0x34] = mi->name_source;
    for (int i = 0; i < 127 && mi->name[i]; i++)
        out[0x40 + i] = (u8)mi->name[i];
    for (int i = 0; i < 15 && mi->version[i]; i++)
        out[0xF0 + i] = (u8)mi->version[i];
    u64 at = META_HEADER;
    u32 sum = 0x811C9DC5u; /* the content id: FNV-1a over what follows the header (changes with it) */
    for (int s = 0; s < META_SECTIONS; s++) {
        u8 *e = out + 0xC0 + 16 * s;
        if (!sect[s])
            continue;
        __builtin_memcpy(out + at, sect[s], len[s]);
        __builtin_memset(out + at + len[s], 0, ((len[s] + 15) & ~15u) - len[s]);
        for (u32 i = 0; i < len[s]; i++)
            sum = (sum ^ sect[s][i]) * 0x01000193u;
        put32le(e, (u32)at), put32le(e + 4, len[s]);
        e[8] = (u8)mi->s[s].w, e[9] = (u8)(mi->s[s].w >> 8), e[10] = (u8)mi->s[s].h, e[11] = (u8)(mi->s[s].h >> 8);
        e[12] = mi->s[s].format, e[13] = mi->s[s].source;
        at += (len[s] + 15) & ~15u;
    }
    put32le(out + 0x28, sum), put32le(out + 0x2C, (u32)n);
    return n;
}
