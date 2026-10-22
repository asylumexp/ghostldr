/* PNG for the offline harnesses (on the console the selector uses libScePngDec). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "png_host.h"

typedef unsigned char u8;
typedef unsigned u32;

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

int png_host_decode(const u8 *d, unsigned long n, unsigned *pw, unsigned *ph, unsigned *out)
{
    if (n < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8))
        return -1;
    u32 w = 0, h = 0;
    int depth = 0, ctype = 0, inter = 0;
    u8 pal[256][4];
    memset(pal, 255, sizeof(pal));
    u8 *idat = NULL;
    unsigned long ilen = 0;
    for (unsigned long p = 8; p + 12 <= n;) {
        u32 len = be32(d + p);
        const u8 *type = d + p + 4, *body = d + p + 8;
        if (p + 12 + len > n)
            break;
        if (!memcmp(type, "IHDR", 4)) {
            w = be32(body), h = be32(body + 4), depth = body[8], ctype = body[9], inter = body[12];
            if (!out) {
                *pw = w, *ph = h;
                return w && h && !inter ? 0 : -1;
            }
        } else if (!memcmp(type, "PLTE", 4)) {
            for (u32 i = 0; i < len / 3 && i < 256; i++)
                pal[i][0] = body[3 * i], pal[i][1] = body[3 * i + 1], pal[i][2] = body[3 * i + 2];
        } else if (!memcmp(type, "tRNS", 4) && ctype == 3) {
            for (u32 i = 0; i < len && i < 256; i++)
                pal[i][3] = body[i];
        } else if (!memcmp(type, "IDAT", 4)) {
            idat = realloc(idat, ilen + len);
            memcpy(idat + ilen, body, len);
            ilen += len;
        }
        p += 12 + len;
    }
    int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!w || !h || !ch || inter || (depth != 8 && depth != 16 && !(ctype == 3 && depth < 8))) {
        free(idat);
        return -1;
    }
    unsigned long bpp = (unsigned long)ch * depth / 8, stride = ((unsigned long)w * ch * depth + 7) / 8;
    if (!bpp)
        bpp = 1;
    uLongf rawlen = (uLongf)((stride + 1) * h);
    u8 *raw = malloc(rawlen);
    if (uncompress(raw, &rawlen, idat, (uLong)ilen) != Z_OK) {
        free(raw), free(idat);
        return -1;
    }
    free(idat);
    for (u32 y = 0; y < h; y++) {
        u8 *r = raw + y * (stride + 1), f = r[0], *c = r + 1, *prev = y ? raw + (y - 1) * (stride + 1) + 1 : NULL;
        for (unsigned long i = 0; i < stride; i++) {
            int a = i >= bpp ? c[i - bpp] : 0, b = prev ? prev[i] : 0, cc = prev && i >= bpp ? prev[i - bpp] : 0;
            c[i] += f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : f == 4 ? paeth(a, b, cc) : 0;
        }
    }
    for (u32 y = 0; y < h; y++) {
        const u8 *c = raw + y * (stride + 1) + 1;
        for (u32 x = 0; x < w; x++) {
            u32 r, g, b, a = 255;
            int step = depth == 16 ? 2 : 1;
            if (ctype == 3) {
                int bits = depth, idx = (c[x * bits / 8] >> (8 - bits - (x * bits) % 8)) & ((1 << bits) - 1);
                r = pal[idx][0], g = pal[idx][1], b = pal[idx][2], a = pal[idx][3];
            } else {
                const u8 *q = c + (unsigned long)x * ch * step;
                if (ch <= 2)
                    r = g = b = q[0];
                else
                    r = q[0], g = q[step], b = q[2 * step];
                if (ch == 2)
                    a = q[step];
                if (ch == 4)
                    a = q[3 * step];
            }
            out[(unsigned long)y * w + x] = a << 24 | r << 16 | g << 8 | b;
        }
    }
    free(raw);
    *pw = w, *ph = h;
    return 0;
}

int png_host_write(const char *path, const unsigned *px, int w, int h, int pitch)
{
    unsigned long stride = (unsigned long)w * 3 + 1, rawlen = stride * h;
    u8 *raw = malloc(rawlen);
    for (int y = 0; y < h; y++) {
        u8 *r = raw + y * stride;
        r[0] = 0;
        for (int x = 0; x < w; x++) {
            u32 c = px[(unsigned long)y * pitch + x];
            r[1 + 3 * x] = (u8)(c >> 16), r[2 + 3 * x] = (u8)(c >> 8), r[3 + 3 * x] = (u8)c;
        }
    }
    uLongf zlen = compressBound((uLong)rawlen);
    u8 *z = malloc(zlen);
    compress2(z, &zlen, raw, (uLong)rawlen, 6);
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(raw), free(z);
        return -1;
    }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    u8 ihdr[13] = {0};
    u32 W = (u32)w, H = (u32)h;
    ihdr[0] = W >> 24, ihdr[1] = W >> 16, ihdr[2] = W >> 8, ihdr[3] = W;
    ihdr[4] = H >> 24, ihdr[5] = H >> 16, ihdr[6] = H >> 8, ihdr[7] = H;
    ihdr[8] = 8, ihdr[9] = 2;
    const char *types[3] = {"IHDR", "IDAT", "IEND"};
    const u8 *data[3] = {ihdr, z, NULL};
    u32 lens[3] = {13, (u32)zlen, 0};
    for (int i = 0; i < 3; i++) {
        u8 hdr[8] = {lens[i] >> 24, lens[i] >> 16, lens[i] >> 8, lens[i]};
        memcpy(hdr + 4, types[i], 4);
        fwrite(hdr, 1, 8, f);
        if (lens[i])
            fwrite(data[i], 1, lens[i], f);
        uLong crc = crc32(crc32(0, NULL, 0), (const u8 *)types[i], 4);
        if (lens[i])
            crc = crc32(crc, data[i], lens[i]);
        u8 c[4] = {crc >> 24, crc >> 16, crc >> 8, crc};
        fwrite(c, 1, 4, f);
    }
    fclose(f);
    free(raw), free(z);
    return 0;
}
