/* NID = custom-base64(reverse(sha1(name || suffix)[0:8]))[0:11] */
#include "ghost.h"

static u32 rol(u32 x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1(const u8 *msg, size_t len, u8 out[20])
{
    u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    u8 block[64];
    size_t total = len + 9;
    size_t nblocks = (total + 63) / 64;
    for (size_t b = 0; b < nblocks; b++) {
        for (int i = 0; i < 64; i++) {
            size_t pos = b * 64 + i;
            u8 v;
            if (pos < len)
                v = msg[pos];
            else if (pos == len)
                v = 0x80;
            else if (pos >= nblocks * 64 - 8)
                v = (u8)(((u64)len * 8) >> (8 * (nblocks * 64 - 1 - pos)));
            else
                v = 0;
            block[i] = v;
        }
        u32 w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (u32)block[i * 4] << 24 | (u32)block[i * 4 + 1] << 16 | (u32)block[i * 4 + 2] << 8 | block[i * 4 + 3];
        for (int i = 16; i < 80; i++)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        u32 a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            u32 f, k;
            if (i < 20)
                f = (bb & c) | (~bb & d), k = 0x5A827999;
            else if (i < 40)
                f = bb ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60)
                f = (bb & c) | (bb & d) | (c & d), k = 0x8F1BBCDC;
            else
                f = bb ^ c ^ d, k = 0xCA62C1D6;
            u32 t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(bb, 30), bb = a, a = t;
        }
        h[0] += a, h[1] += bb, h[2] += c, h[3] += d, h[4] += e;
    }
    for (int i = 0; i < 5; i++)
        out[i * 4] = h[i] >> 24, out[i * 4 + 1] = h[i] >> 16, out[i * 4 + 2] = h[i] >> 8, out[i * 4 + 3] = h[i];
}

void compute_nid(const char *name, char out[12])
{
    static const u8 suffix[16] = {0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde, 0xd8, 0xc1,
                                  0xe6, 0xb0, 0x39, 0xb1, 0xc3, 0xe5, 0x52, 0x30};
    static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    u8 msg[300], digest[20];
    size_t n = strlen(name);
    if (n > sizeof(msg) - 16)
        n = sizeof(msg) - 16;
    memcpy(msg, name, n);
    memcpy(msg + n, suffix, 16);
    sha1(msg, n + 16, digest);
    u8 le[9];
    for (int i = 0; i < 8; i++)
        le[i] = digest[7 - i];
    le[8] = 0;
    /* base64 of 8 bytes -> first 11 chars */
    for (int i = 0, o = 0; o < 11; i += 3) {
        u32 v = (u32)le[i] << 16 | (u32)(i + 1 < 9 ? le[i + 1] : 0) << 8 | (u32)(i + 2 < 9 ? le[i + 2] : 0);
        for (int j = 0; j < 4 && o < 11; j++)
            out[o++] = alpha[(v >> (18 - 6 * j)) & 63];
    }
    out[11] = 0;
}
