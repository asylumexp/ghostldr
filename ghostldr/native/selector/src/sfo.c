/* param.sfo: "\0PSF", version, key table, data table, count; then entries
 * {u16 key offset, u16 format (0x0204 utf-8, 0x0404 int32), u32 length,
 * u32 max length, u32 data offset}. */
#include "sel.h"

static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* Copy the value of key into out (strings) or *num (integers); 0 when found. */
int sfo_get(const u8 *sfo, u64 size, const char *key, char *out, int cap, s32 *num)
{
    if (size < 20 || sfo[0] != 0 || sfo[1] != 'P' || sfo[2] != 'S' || sfo[3] != 'F')
        return -1;
    u32 keys = le32(sfo + 8), data = le32(sfo + 12), n = le32(sfo + 16);
    if (keys >= size || data >= size || n > 256 || 20 + 16ull * n > size)
        return -1;
    for (u32 i = 0; i < n; i++) {
        const u8 *e = sfo + 20 + 16 * i;
        u32 ko = keys + (u32)(e[0] | e[1] << 8), fmt = (u32)(e[2] | e[3] << 8), len = le32(e + 4);
        u32 doff = data + le32(e + 12);
        if (ko >= size || doff > size || len > size - doff)
            continue;
        const char *k = (const char *)sfo + ko;
        int j = 0;
        while (key[j] && ko + (u32)j < size && k[j] == key[j])
            j++;
        if (key[j] || ko + (u32)j >= size || k[j])
            continue;
        if (fmt == 0x0404 && len >= 4) {
            if (num)
                *num = (s32)le32(sfo + doff);
            return 0;
        }
        if (!out || cap <= 0)
            return 0;
        int m = 0;
        while ((u32)m < len && m + 1 < cap && sfo[doff + m])
            out[m] = (char)sfo[doff + m], m++;
        out[m] = 0;
        return 0;
    }
    return -1;
}

/* ---- a game's dump, or homebrew? ---- */

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int digit(int c) { return c >= '0' && c <= '9'; }

/* s starts with a retail title ID: a product prefix of PS4 (and PS5) games
 * and five digits, then nothing more of the ID */
static int retail_id(const char *s)
{
    static const char prefixes[] = "CUSA" "PCAS" "PCJS" "PCKS" "PLAS" "PLJM" "PLJS" "PLKS" "PPSA";
    if (!s)
        return 0;
    for (int i = 0; i < 9; i++)
        if (!s[i])
            return 0;
    for (int i = 4; i < 9; i++)
        if (!digit(s[i]))
            return 0;
    if (digit(s[9]) || (upper(s[9]) >= 'A' && upper(s[9]) <= 'Z'))
        return 0;
    for (u64 p = 0; p < sizeof(prefixes) - 1; p += 4) {
        int k = 0;
        while (k < 4 && upper(s[k]) == prefixes[p + k])
            k++;
        if (k == 4)
            return 1;
    }
    return 0;
}

int title_is_dump(const char *id, const char *sfo_id, const char *content_id)
{
    const char *c = content_id ? content_id : "";
    int cid = c[0] && c[1] && digit(c[2]) && digit(c[3]) && digit(c[4]) && digit(c[5]) && c[6] == '-';
    if (cid && upper(c[0]) == 'I' && upper(c[1]) == 'V')
        return 0; /* IV0000-BREW00083_00-...: homebrew's publisher code */
    return retail_id(id) || retail_id(sfo_id) || (cid && retail_id(c + 7));
}
