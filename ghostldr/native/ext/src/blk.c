/*
 * A read cache over a block device, sized for streaming large files.
 *
 * Lines of `line` bytes (a power of two, 16 KiB on the console) in two
 * pools, each least-recently-used: metadata (FAT, directories) and file
 * data, so a title streaming a large file never pushes the directory and
 * FAT lines out.
 *
 *   - A data miss fetches, in one device read, every missing line the
 *     request still needs; when the request continues the previous one (it
 *     starts in or right after the line where that one ended) it also reads
 *     ahead, a window that doubles while requests keep following each other,
 *     up to ra_max lines in all.  They land in a staging buffer, then each
 *     in a line.  A random request reads only its own lines.
 *   - A data read that covers at least direct_min whole uncached lines goes
 *     straight from the device into the caller's buffer, in device reads of
 *     up to max_sectors: large reads cost no copy and leave the cache alone.
 *   - blk_peek hands out a pointer into a line for small metadata reads
 *     (a FAT entry, a directory entry).
 *
 * Lines are clean copies of the device (nothing is ever written), so the
 * direct path needs no coherency.  One caller at a time: usbfs holds its
 * volume lock around every call.
 */
#include "usbstore.h"

static u32 log2_u32(u32 v)
{
    u32 s = 0;
    while ((1u << s) < v)
        s++;
    return s;
}

int blk_init(struct blk *b, const struct blkdev *dev, u32 line_bytes, u32 meta_lines, u32 data_lines, u32 ra_max)
{
    memset(b, 0, sizeof(*b));
    if (!dev->read || !dev->sector || (dev->sector & (dev->sector - 1)) || line_bytes < dev->sector ||
        (line_bytes & (line_bytes - 1)) || !meta_lines || !data_lines)
        return US_EINVAL;
    b->dev = *dev;
    if (!b->dev.max_sectors)
        b->dev.max_sectors = (1u << 20) / dev->sector;
    b->line = line_bytes;
    b->line_shift = log2_u32(line_bytes);
    b->size = dev->sectors * dev->sector;
    b->ra_max = ra_max ? US_MIN(ra_max, data_lines) : 1;
    b->ra = 1;
    b->prev_last = ~1ull;
    b->direct_min = 2;
    u32 lines = meta_lines + data_lines;
    u64 meta = ALIGN_UP((u64)lines * sizeof(struct blk_line), 64);
    b->mem_size = meta + (u64)(lines + b->ra_max) * line_bytes;
    b->mem = us_alloc(b->mem_size);
    if (!b->mem)
        return US_ENOMEM;
    struct blk_line *ln = b->mem;
    u8 *data = (u8 *)b->mem + meta;
    b->pool[BLK_META] = (struct blk_pool){ln, data, meta_lines};
    b->pool[BLK_DATA] = (struct blk_pool){ln + meta_lines, data + (u64)meta_lines * line_bytes, data_lines};
    b->stage = data + (u64)lines * line_bytes;
    for (u32 i = 0; i < lines; i++)
        ln[i].tag = ~0ull;
    return 0;
}

void blk_free(struct blk *b)
{
    if (b->mem)
        us_free(b->mem, b->mem_size);
    b->mem = NULL;
}

/* bytes at a sector-aligned device offset, in reads of at most max_sectors */
static int dev_read(struct blk *b, u64 off, u64 n, void *buf)
{
    u32 ss = b->dev.sector;
    u64 lba = off / ss, count = (n + ss - 1) / ss;
    u8 *p = buf;
    while (count) {
        u32 k = (u32)US_MIN(count, (u64)b->dev.max_sectors);
        int rc = b->dev.read(b->dev.ctx, lba, k, p);
        if (rc < 0)
            return rc;
        b->dev_reads++;
        b->dev_bytes += (u64)k * ss;
        p += (u64)k * ss, lba += k, count -= k;
    }
    return 0;
}

static struct blk_line *find(struct blk *b, u64 tag, u8 **data)
{
    for (int p = 0; p < 2; p++) {
        struct blk_pool *pool = &b->pool[p];
        for (u32 i = 0; i < pool->n; i++)
            if (pool->line[i].tag == tag) {
                *data = pool->data + ((u64)i << b->line_shift);
                return &pool->line[i];
            }
    }
    return NULL;
}

static u32 victim(struct blk_pool *pool)
{
    u32 v = 0;
    for (u32 i = 1; i < pool->n; i++)
        if (pool->line[i].used < pool->line[v].used)
            v = i;
    return v;
}

/* bytes of line `tag` that are on the device */
static u64 line_bytes(struct blk *b, u64 tag)
{
    u64 start = tag << b->line_shift;
    return US_MIN((u64)b->line, b->size - start);
}

/* the cached copy of line `tag`, read on a miss with the lines after it the
 * request needs (upto: its last line) and, for a stream, the read-ahead */
static const u8 *line_get(struct blk *b, u64 tag, u64 upto, int hint)
{
    u8 *data;
    struct blk_line *l = find(b, tag, &data);
    if (l) {
        b->hits++;
        l->used = ++b->clock;
        return data;
    }
    b->misses++;
    struct blk_pool *pool = &b->pool[hint == BLK_META ? BLK_META : BLK_DATA];
    u32 k = 1;
    if (hint != BLK_META) {
        u64 last = (b->size - 1) >> b->line_shift, want = upto - tag + 1 + (b->seq ? b->ra : 0);
        while (k < want && k < b->ra_max && tag + k <= last && !find(b, tag + k, &data))
            k++;
        if (tag + k - 1 > upto)
            b->ra_lines += tag + k - 1 - upto;
    }
    u64 bytes = (u64)(k - 1) * b->line + line_bytes(b, tag + k - 1);
    if (dev_read(b, tag << b->line_shift, bytes, b->stage) != 0)
        return NULL;
    const u8 *first = NULL;
    for (u32 i = 0; i < k; i++) {
        u32 v = victim(pool);
        pool->line[v].tag = tag + i;
        pool->line[v].used = ++b->clock;
        u8 *dst = pool->data + ((u64)v << b->line_shift);
        memcpy(dst, b->stage + ((u64)i << b->line_shift), line_bytes(b, tag + i));
        if (!i)
            first = dst;
    }
    return first;
}

const u8 *blk_peek(struct blk *b, u64 off, u32 n, int hint)
{
    u32 in = (u32)(off & (b->line - 1));
    if (off >= b->size || n > b->size - off || in + n > b->line)
        return NULL;
    const u8 *l = line_get(b, off >> b->line_shift, off >> b->line_shift, hint);
    return l ? l + in : NULL;
}

s64 blk_read(struct blk *b, u64 off, void *buf, u64 n, int hint)
{
    if (off >= b->size)
        return 0;
    n = US_MIN(n, b->size - off);
    u8 *out = buf;
    u64 done = 0, first = off >> b->line_shift, upto = (off + n - 1) >> b->line_shift;
    if (hint == BLK_DATA) {
        b->seq = first == b->prev_last || first == b->prev_last + 1;
        b->ra = b->seq ? US_MIN(b->ra * 2, b->ra_max) : 1;
        b->prev_last = upto;
    }
    while (done < n) {
        u64 pos = off + done, left = n - done, tag = pos >> b->line_shift;
        u32 in = (u32)(pos & (b->line - 1));
        u8 *data;
        if (hint == BLK_DATA && !in && left >= (u64)b->direct_min * b->line && !find(b, tag, &data)) {
            /* whole uncached lines: straight into the caller's buffer */
            u64 k = 1, most = left >> b->line_shift;
            while (k < most && k < 1024 && !find(b, tag + k, &data))
                k++;
            u64 bytes = US_MIN(k << b->line_shift, b->size - pos);
            int rc = dev_read(b, pos, bytes, out + done);
            if (rc < 0)
                return done ? (s64)done : rc;
            b->direct_bytes += bytes;
            done += bytes;
            continue;
        }
        const u8 *src = line_get(b, tag, upto, hint);
        if (!src)
            return done ? (s64)done : US_EIO;
        u64 take = US_MIN((u64)b->line - in, left);
        memcpy(out + done, src + in, take);
        done += take;
    }
    return (s64)done;
}
