/*
 * A QR code for the network installer's address (setui.c), so a phone can
 * open it by pointing its camera at the TV: version 3 (29 x 29 modules),
 * error correction level M, byte mode, which holds 42 bytes
 * ("http://255.255.255.255:65535/" is 29).  After ISO/IEC 18004: the data
 * and its Reed-Solomon code words (26, one block), the function patterns,
 * the mask with the lowest penalty, the format bits.  No tables: GF(256)
 * products are computed (the module has no writable data or relocations).
 */
#include "sel.h"

#define V 3
#define DATA_CW 44 /* version 3-M: 70 code words, 26 of them error correction */
#define ECC_CW 26

static u8 gf_mul(u8 x, u8 y)
{
    u32 z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((u32)(y >> i) & 1) * x;
    }
    return (u8)z;
}

/* the code words: data (mode, count, bytes, terminator, padding) and its ECC */
static int codewords(const char *text, u8 cw[DATA_CW + ECC_CW])
{
    int n = str_len(text);
    if (n > DATA_CW - 2)
        return -1;
    for (int i = 0; i < DATA_CW + ECC_CW; i++)
        cw[i] = 0;
    int bit = 0;
#define PUT(v, len)                                                         \
    for (int k = (len)-1; k >= 0; k--, bit++)                               \
        if (((u32)(v) >> k) & 1)                                            \
            cw[bit >> 3] |= (u8)(0x80 >> (bit & 7));
    PUT(4, 4); /* byte mode */
    PUT(n, 8);
    for (int i = 0; i < n; i++)
        PUT((u8)text[i], 8);
#undef PUT
    bit += 4;                 /* the terminator (there is room: 42 bytes at most) */
    int at = (bit + 7) >> 3;  /* to a byte */
    for (int pad = 0; at < DATA_CW; at++, pad ^= 1)
        cw[at] = pad ? 0x11 : 0xEC;
    /* Reed-Solomon: the divisor's coefficients, then the remainder */
    u8 div[ECC_CW], rem[ECC_CW];
    for (int i = 0; i < ECC_CW; i++)
        div[i] = 0, rem[i] = 0;
    div[ECC_CW - 1] = 1;
    u8 root = 1;
    for (int i = 0; i < ECC_CW; i++) {
        for (int j = 0; j < ECC_CW; j++) {
            div[j] = gf_mul(div[j], root);
            if (j + 1 < ECC_CW)
                div[j] ^= div[j + 1];
        }
        root = gf_mul(root, 2);
    }
    for (int i = 0; i < DATA_CW; i++) {
        u8 f = cw[i] ^ rem[0];
        for (int j = 0; j + 1 < ECC_CW; j++)
            rem[j] = rem[j + 1];
        rem[ECC_CW - 1] = 0;
        for (int j = 0; j < ECC_CW; j++)
            rem[j] ^= gf_mul(div[j], f);
    }
    for (int i = 0; i < ECC_CW; i++)
        cw[DATA_CW + i] = rem[i];
    return 0;
}

/* m: QR_N * QR_N modules, row by row; fn: which are function modules */
static void set(u8 *m, u8 *fn, int x, int y, int dark)
{
    m[y * QR_N + x] = (u8)(dark != 0);
    fn[y * QR_N + x] = 1;
}

static void finder(u8 *m, u8 *fn, int cx, int cy)
{
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            int x = cx + dx, y = cy + dy, d = dx < 0 ? -dx : dx, e = dy < 0 ? -dy : dy;
            int r = d > e ? d : e;
            if (x >= 0 && x < QR_N && y >= 0 && y < QR_N)
                set(m, fn, x, y, r != 2 && r != 4);
        }
}

static void format_bits(u8 *m, u8 *fn, int mask)
{
    int data = 0 << 3 | mask; /* level M: 00 */
    int rem = data;
    for (int i = 0; i < 10; i++)
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = (data << 10 | rem) ^ 0x5412;
#define B(i) ((bits >> (i)) & 1)
    for (int i = 0; i <= 5; i++)
        set(m, fn, 8, i, B(i));
    set(m, fn, 8, 7, B(6));
    set(m, fn, 8, 8, B(7));
    set(m, fn, 7, 8, B(8));
    for (int i = 9; i < 15; i++)
        set(m, fn, 14 - i, 8, B(i));
    for (int i = 0; i < 8; i++)
        set(m, fn, QR_N - 1 - i, 8, B(i));
    for (int i = 8; i < 15; i++)
        set(m, fn, 8, QR_N - 15 + i, B(i));
#undef B
    set(m, fn, 8, QR_N - 8, 1); /* the dark module */
}

static int masked(int mask, int x, int y)
{
    switch (mask) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

/* rules 1 (runs of five or more), 2 (2x2 blocks) and 4 (balance) of the
 * standard's penalty; any mask reads, this only picks a tidy one */
static int penalty(const u8 *m)
{
    int p = 0, dark = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int a = 0; a < QR_N; a++) {
            int run = 1;
            for (int b = 1; b < QR_N; b++) {
                int c = pass ? m[b * QR_N + a] : m[a * QR_N + b], d = pass ? m[(b - 1) * QR_N + a] : m[a * QR_N + b - 1];
                if (c == d) {
                    run++;
                    p += run == 5 ? 3 : run > 5 ? 1 : 0;
                } else {
                    run = 1;
                }
            }
        }
    for (int y = 0; y + 1 < QR_N; y++)
        for (int x = 0; x + 1 < QR_N; x++) {
            int c = m[y * QR_N + x];
            p += 3 * (c == m[y * QR_N + x + 1] && c == m[(y + 1) * QR_N + x] && c == m[(y + 1) * QR_N + x + 1]);
        }
    for (int i = 0; i < QR_N * QR_N; i++)
        dark += m[i];
    int k = (dark * 20 - QR_N * QR_N * 10);
    k = k < 0 ? -k : k;
    return p + (k / (QR_N * QR_N)) * 10;
}

int qr_encode(const char *text, u8 m[QR_N * QR_N])
{
    u8 cw[DATA_CW + ECC_CW], fn[QR_N * QR_N], best[QR_N * QR_N];
    if (codewords(text, cw) != 0)
        return -1;
    for (int i = 0; i < QR_N * QR_N; i++)
        m[i] = 0, fn[i] = 0;
    /* function patterns */
    for (int i = 0; i < QR_N; i++)
        set(m, fn, 6, i, i % 2 == 0), set(m, fn, i, 6, i % 2 == 0); /* timing */
    finder(m, fn, 3, 3);
    finder(m, fn, QR_N - 4, 3);
    finder(m, fn, 3, QR_N - 4);
    for (int dy = -2; dy <= 2; dy++) /* the alignment pattern at (22, 22) */
        for (int dx = -2; dx <= 2; dx++) {
            int d = dx < 0 ? -dx : dx, e = dy < 0 ? -dy : dy;
            set(m, fn, 22 + dx, 22 + dy, (d > e ? d : e) != 1);
        }
    format_bits(m, fn, 0); /* reserves the format areas */
    /* the code words, up and down two columns at a time from the right */
    int i = 0;
    for (int right = QR_N - 1; right >= 1; right -= 2) {
        if (right == 6)
            right = 5;
        for (int vert = 0; vert < QR_N; vert++)
            for (int j = 0; j < 2; j++) {
                int x = right - j, up = ((right + 1) & 2) == 0, y = up ? QR_N - 1 - vert : vert;
                if (!fn[y * QR_N + x] && i < (DATA_CW + ECC_CW) * 8) {
                    m[y * QR_N + x] = (u8)((cw[i >> 3] >> (7 - (i & 7))) & 1);
                    i++;
                }
            }
    }
    /* the mask with the lowest penalty */
    int best_p = 1 << 30;
    for (int k = 0; k < 8; k++) {
        u8 t[QR_N * QR_N];
        for (int a = 0; a < QR_N * QR_N; a++)
            t[a] = m[a] ^ (u8)(!fn[a] && masked(k, a % QR_N, a / QR_N));
        format_bits(t, fn, k);
        int p = penalty(t);
        if (p < best_p) {
            best_p = p;
            for (int a = 0; a < QR_N * QR_N; a++)
                best[a] = t[a];
        }
    }
    for (int a = 0; a < QR_N * QR_N; a++)
        m[a] = best[a];
    return 0;
}

/* the code, dark on white with its quiet zone (4 modules), size px square at (x, y) */
void qr_draw(surface *s, int x, int y, int size, const u8 *m, u32 a)
{
    int q = QR_N + 8, px = size / q;
    size = px * q;
    draw_rect_blend(s, x, y, size, size, (a << 24) | 0xFFFFFF);
    for (int r = 0; r < QR_N; r++)
        for (int c = 0; c < QR_N; c++)
            if (m[r * QR_N + c])
                draw_rect_blend(s, x + (c + 4) * px, y + (r + 4) * px, px, px, (a << 24) | 0x000000);
}
