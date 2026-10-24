/*
 * Partition tables: GPT (behind a protective MBR, the primary header and
 * its entries checked by CRC-32, else the backup at the disk's end), MBR
 * primary partitions, or none at all: a volume boot sector at sector 0
 * ("superfloppy", as many USB sticks come formatted).
 *
 * Logical partitions inside an MBR extended partition are not listed (no
 * drive formatted by Windows, macOS or a camera puts its only volume there).
 */
#include "usbstore.h"

static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 le64(const u8 *p) { return le32(p) | (u64)le32(p + 4) << 32; }

u32 crc32_le(u32 crc, const void *data, u64 n)
{
    static const u32 k_nib[16] = {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
                                  0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
                                  0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
    const u8 *p = data;
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ k_nib[crc & 15];
        crc = (crc >> 4) ^ k_nib[crc & 15];
    }
    return ~crc;
}

/* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, as stored */
static const u8 k_basic_data[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                                    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};

/* C12A7328-F81F-11D2-BA4B-00A0C93EC93B: the EFI system partition macOS and
 * Windows put first on a GPT drive */
static const u8 k_esp[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                             0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};

/* a FAT or exFAT boot sector (not an MBR) */
static bool is_vbr(const u8 *s)
{
    if (memcmp(s + 3, "EXFAT   ", 8) == 0)
        return true;
    if (s[0] != 0xEB && s[0] != 0xE9)
        return false;
    u16 bps = le16(s + 11);
    u8 spc = s[13];
    bool sane = bps >= 512 && bps <= 4096 && !(bps & (bps - 1)) && spc && !(spc & (spc - 1)) && le16(s + 14) &&
                s[16] >= 1 && s[16] <= 4;
    return sane && (memcmp(s + 82, "FAT32   ", 8) == 0 || memcmp(s + 54, "FAT16   ", 8) == 0 ||
                    memcmp(s + 54, "FAT12   ", 8) == 0 || memcmp(s + 54, "FAT     ", 8) == 0);
}

/* the GPT whose header is at lba: partitions found, or -1 when it is not valid */
static int gpt_at(struct blk *b, u64 lba, struct part *out, int max)
{
    u32 ss = b->dev.sector;
    u8 h[512];
    if (blk_read(b, lba * ss, h, 512, BLK_META) != 512 || memcmp(h, "EFI PART", 8) != 0)
        return -1;
    u32 hsize = le32(h + 12);
    if (hsize < 92 || hsize > 512)
        return -1;
    u32 crc = le32(h + 16);
    memset(h + 16, 0, 4);
    if (crc32_le(0, h, hsize) != crc) {
        us_log("usb: GPT header at %lx: bad CRC", lba);
        return -1;
    }
    u64 ents = le64(h + 72);
    u32 n = le32(h + 80), esize = le32(h + 84), ecrc = le32(h + 88);
    if (esize < 128 || (esize & 7) || n > 1024 || (u64)n * esize > (1u << 20) || ents >= b->dev.sectors)
        return -1;
    /* the entry array, read once for its CRC and once more entry by entry */
    u32 c = 0;
    u8 e[512];
    for (u64 at = 0, total = (u64)n * esize; at < total;) {
        u32 take = (u32)US_MIN((u64)sizeof(e), total - at);
        if (blk_read(b, ents * ss + at, e, take, BLK_META) != take)
            return -1;
        c = crc32_le(c, e, take);
        at += take;
    }
    if (c != ecrc) {
        us_log("usb: GPT entries at %lx: bad CRC", ents);
        return -1;
    }
    int found = 0;
    for (u32 i = 0; i < n && found < max; i++) {
        if (blk_read(b, ents * ss + (u64)i * esize, e, 128, BLK_META) != 128)
            return -1;
        static const u8 zero[16];
        u64 first = le64(e + 32), last = le64(e + 40);
        if (memcmp(e, zero, 16) == 0 || last < first || last >= b->dev.sectors)
            continue;
        out[found++] = (struct part){first, last - first + 1, 0, 1, (u8)(memcmp(e, k_basic_data, 16) == 0), 0,
                                     (u8)(memcmp(e, k_esp, 16) == 0)};
    }
    return found;
}

int part_scan(struct blk *b, struct part *out, int max)
{
    u8 s[512];
    if (max < 1)
        return 0;
    if (blk_read(b, 0, s, 512, BLK_META) != 512)
        return US_EIO;
    if (is_vbr(s)) {
        out[0] = (struct part){0, b->dev.sectors, 0, 0, 0, 1, 0};
        return 1;
    }
    if (s[510] != 0x55 || s[511] != 0xAA)
        return 0;
    bool protective = false;
    for (int i = 0; i < 4; i++)
        protective |= s[446 + 16 * i + 4] == 0xEE;
    if (protective) {
        int n = gpt_at(b, 1, out, max);
        if (n < 0) {
            us_log("usb: primary GPT unusable: trying the backup");
            n = gpt_at(b, b->dev.sectors - 1, out, max);
        }
        if (n >= 0)
            return n;
    }
    int found = 0;
    for (int i = 0; i < 4 && found < max; i++) {
        const u8 *e = s + 446 + 16 * i;
        u8 type = e[4];
        u32 start = le32(e + 8), count = le32(e + 12);
        if (!type || type == 0xEE || type == 0x05 || type == 0x0F || type == 0x85 || !count || !start ||
            (u64)start + count > b->dev.sectors)
            continue;
        out[found++] = (struct part){start, count, type, 0, 0, 0, (u8)(type == 0xEF)};
    }
    return found;
}
