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
