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
