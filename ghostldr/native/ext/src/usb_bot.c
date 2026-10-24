/*
 * USB Mass Storage Bulk-Only Transport (USB MSC BOT 1.0) and the SCSI block
 * commands a USB drive needs: INQUIRY, TEST UNIT READY, READ CAPACITY (10
 * and 16), READ (10 and 16) and REQUEST SENSE.
 *
 * One command is three bulk transfers: a 31-byte CBW on the OUT endpoint,
 * the data, and a 13-byte CSW on the IN endpoint, matched by tag.  What the
 * host does when they go wrong (BOT 5.3, 6.7):
 *   - the CBW is refused: reset recovery, the command fails;
 *   - the data phase stalls: clear the endpoint's halt, read the CSW;
 *   - the CSW read stalls: clear the halt and read it once more;
 *   - a CSW that is not 13 bytes, has another signature or tag, or says
 *     "phase error": reset recovery (Bulk-Only Mass Storage Reset, then
 *     clear both endpoints' halts), the command fails;
 *   - the data phase is short: the CSW's residue says how much was moved.
 * A failed transport is retried once.  A command whose CSW says "failed"
 * is followed by REQUEST SENSE; reads retry unit attentions and drives that
 * are becoming ready.  Some devices answer with the CSW in place of data
 * they don't have: a 13-byte IN data phase that is our CSW counts as one.
 *
 * Everything goes through struct usbd_ops: libSceUsbd on the console
 * (usb_sce.c), a fake device in the host tests.
 */
#include "usbstore.h"

#define CBW_SIG 0x43425355u /* "USBC" */
#define CSW_SIG 0x53425355u /* "USBS" */
#define T_CMD 5000          /* ms for CBW, CSW and control transfers */
#define T_DATA 20000        /* ms for a data phase */
#define DEFAULT_XFER (128u << 10)

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u64 be64(const u8 *p) { return (u64)be32(p) << 32 | be32(p + 4); }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static void put_be32(u8 *p, u32 v) { p[0] = (u8)(v >> 24), p[1] = (u8)(v >> 16), p[2] = (u8)(v >> 8), p[3] = (u8)v; }
static void put_le32(u8 *p, u32 v) { p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24); }

int bot_init(struct bot *b, const struct usbd_ops *u, u8 iface, u8 ep_in, u8 ep_out)
{
    memset(b, 0, sizeof(*b));
    b->u = u, b->iface = iface, b->ep_in = ep_in | 0x80, b->ep_out = ep_out & 0x7F;
    b->tag = 0x47480000; /* "GH" */
    b->max_xfer = DEFAULT_XFER;
    return 0;
}

static int bulk(struct bot *b, u8 ep, void *buf, u32 len, u32 *done, u32 timeout)
{
    *done = 0;
    int rc = b->u->bulk(b->u->ctx, ep, buf, len, done, timeout);
    if (rc == USBD_STALL)
        b->stalls++;
    return rc;
}

void bot_reset_recovery(struct bot *b)
{
    b->resets++;
    /* Bulk-Only Mass Storage Reset: class request to the interface */
    int rc = b->u->control(b->u->ctx, 0x21, 0xFF, 0, b->iface, NULL, 0, T_CMD);
    int ri = b->u->clear_halt(b->u->ctx, b->ep_in);
    int ro = b->u->clear_halt(b->u->ctx, b->ep_out);
    if (b->resets <= 8)
        us_log("usb: reset recovery (reset %d, clear halt in %d, out %d)", rc, ri, ro);
}

/* the CSW of tag: 0 with status and residue, or -1 (reset recovery needed) */
static int read_csw(struct bot *b, u32 tag, u8 *status, u32 *residue)
{
    u8 csw[16];
    u32 got = 0;
    int rc = USBD_ERROR;
    for (int tries = 0; tries < 2; tries++) {
        rc = bulk(b, b->ep_in, csw, 13, &got, T_CMD);
        if (rc != USBD_STALL)
            break;
        b->u->clear_halt(b->u->ctx, b->ep_in);
    }
    if (rc != USBD_OK || got != 13 || le32(csw) != CSW_SIG || le32(csw + 4) != tag || csw[12] > 2) {
        b->bad_csw++;
        if (b->bad_csw <= 8)
            us_log("usb: bad CSW for tag %x (rc %d, %u bytes, signature %x, tag %x, status %u)", tag, rc, got,
                   got >= 4 ? le32(csw) : 0, got >= 8 ? le32(csw + 4) : 0, got >= 13 ? csw[12] : 0);
        return -1;
    }
    *status = csw[12];
    *residue = le32(csw + 8);
    return 0;
}

static int command_once(struct bot *b, const u8 *cdb, u8 cdb_len, void *buf, u32 len, bool in, u32 *xfer)
{
    u8 cbw[31];
    u32 tag = ++b->tag, got = 0;
    memset(cbw, 0, sizeof(cbw));
    put_le32(cbw, CBW_SIG);
    put_le32(cbw + 4, tag);
    put_le32(cbw + 8, len);
    cbw[12] = in ? 0x80 : 0;
    cbw[13] = b->lun & 0x0F;
    cbw[14] = cdb_len;
    memcpy(cbw + 15, cdb, cdb_len);
    *xfer = 0;
    b->commands++;
    int rc = bulk(b, b->ep_out, cbw, 31, &got, T_CMD);
    if (rc != USBD_OK || got != 31) {
        bot_reset_recovery(b);
        return BOT_ERROR;
    }
    u32 moved = 0;
    if (len) {
        rc = bulk(b, in ? b->ep_in : b->ep_out, buf, len, &moved, T_DATA);
        if (rc == USBD_STALL) {
            b->u->clear_halt(b->u->ctx, in ? b->ep_in : b->ep_out);
        } else if (rc != USBD_OK) {
            bot_reset_recovery(b);
            return BOT_ERROR;
        } else if (in && moved == 13 && len != 13 && le32(buf) == CSW_SIG && le32((u8 *)buf + 4) == tag &&
                   ((u8 *)buf)[12] <= 1) {
            /* the device skipped the data and sent its CSW */
            u8 status = ((u8 *)buf)[12];
            return status == 0 ? BOT_PASSED : BOT_FAILED;
        }
    }
    u8 status;
    u32 residue;
    if (read_csw(b, tag, &status, &residue) != 0) {
        bot_reset_recovery(b);
        return BOT_ERROR;
    }
    if (status == 2) {
        b->phase_errors++;
        bot_reset_recovery(b);
        return BOT_ERROR;
    }
    /* what moved: the transfer's count, less what the device says it did not use */
    u32 used = residue <= len ? len - residue : 0;
    *xfer = US_MIN(moved, used);
    return status == 0 ? BOT_PASSED : BOT_FAILED;
}

int bot_request_sense(struct bot *b)
{
    u8 cdb[6] = {0x03, 0, 0, 0, 18, 0}, d[18];
    u32 got = 0;
    memset(d, 0, sizeof(d));
    memset(&b->sense, 0, sizeof(b->sense));
    int st = BOT_ERROR;
    for (int tries = 0; tries < 2 && st == BOT_ERROR; tries++)
        st = command_once(b, cdb, 6, d, 18, true, &got);
    if (st != BOT_PASSED || got < 8)
        return -1;
    if ((d[0] & 0x7F) == 0x72 || (d[0] & 0x7F) == 0x73) /* descriptor format */
        b->sense = (struct scsi_sense){(u8)(d[1] & 0x0F), d[2], d[3]};
    else if (got >= 14)
        b->sense = (struct scsi_sense){(u8)(d[2] & 0x0F), d[12], d[13]};
    else
        b->sense = (struct scsi_sense){(u8)(d[2] & 0x0F), 0, 0};
    return 0;
}

int bot_command(struct bot *b, const u8 *cdb, u8 cdb_len, void *buf, u32 len, bool in, u32 *xfer)
{
    int st = command_once(b, cdb, cdb_len, buf, len, in, xfer);
    if (st == BOT_ERROR) {
        b->retries++;
        st = command_once(b, cdb, cdb_len, buf, len, in, xfer);
    }
    if (st == BOT_FAILED)
        bot_request_sense(b);
    return st;
}

/* a failed command worth repeating: unit attention, or a drive becoming ready */
static bool transient(const struct scsi_sense *s)
{
    return s->key == 6 || (s->key == 2 && s->asc == 0x04 && (s->ascq == 0x01 || s->ascq == 0x00)) ||
           (s->key == 0 && s->asc == 0);
}

static void ascii(char *out, const u8 *in, u32 n)
{
    u32 k = 0;
    for (u32 i = 0; i < n; i++)
        out[k++] = in[i] >= 0x20 && in[i] < 0x7F ? (char)in[i] : '?';
    while (k && out[k - 1] == ' ')
        k--;
    out[k] = 0;
}

int bot_attach(struct bot *b, u8 lun)
{
    u8 d[36];
    u32 got = 0;
    /* GET MAX LUN: a stall means a single LUN */
    u8 max = 0;
    int rc = b->u->control(b->u->ctx, 0xA1, 0xFE, 0, b->iface, &max, 1, T_CMD);
    b->max_lun = rc == 1 && max <= 15 ? max : 0;
    b->lun = lun <= b->max_lun ? lun : 0;

    u8 inq[6] = {0x12, 0, 0, 0, 36, 0};
    memset(d, 0, sizeof(d));
    if (bot_command(b, inq, 6, d, 36, true, &got) == BOT_PASSED && got >= 32) {
        b->devtype = d[0] & 0x1F;
        b->removable = d[1] >> 7;
        ascii(b->vendor, d + 8, 8);
        ascii(b->product, d + 16, 16);
    } else {
        us_log("usb: INQUIRY failed (sense %x/%x/%x)", b->sense.key, b->sense.asc, b->sense.ascq);
    }
    if (b->devtype != 0 && b->devtype != 0x0E) /* direct access (or simplified) block device */
        us_log("usb: device type %x is not a disk; trying anyway", b->devtype);

    /* TEST UNIT READY until ready: about 10 s for a drive spinning up */
    u8 tur[6] = {0, 0, 0, 0, 0, 0};
    int ready = 0;
    for (int tries = 0; tries < 50 && !ready; tries++) {
        int st = bot_command(b, tur, 6, NULL, 0, true, &got);
        if (st == BOT_PASSED) {
            ready = 1;
        } else if (st == BOT_FAILED && b->sense.key == 2 && b->sense.asc == 0x3A) {
            us_log("usb: no medium");
            return US_ENODEV;
        } else if (st == BOT_FAILED && b->sense.key == 6) {
            continue; /* unit attention: power on, medium changed */
        } else {
            us_sleep_us(200000);
        }
    }
    if (!ready)
        us_log("usb: TEST UNIT READY never passed (sense %x/%x/%x); reading anyway", b->sense.key, b->sense.asc,
               b->sense.ascq);

    /* READ CAPACITY (10), then (16) when the drive is past 2 TiB of 512-byte blocks */
    u64 last = 0;
    u32 block = 0;
    for (int tries = 0; tries < 5 && !block; tries++) {
        u8 rc10[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        memset(d, 0, sizeof(d));
        int st = bot_command(b, rc10, 10, d, 8, true, &got);
        if (st == BOT_PASSED && got >= 8) {
            last = be32(d), block = be32(d + 4);
        } else if (st == BOT_FAILED && transient(&b->sense)) {
            us_sleep_us(100000);
            continue;
        } else {
            break;
        }
        if (last == 0xFFFFFFFF) {
            u8 rc16[16] = {0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0};
            memset(d, 0, sizeof(d));
            if (bot_command(b, rc16, 16, d, 32, true, &got) == BOT_PASSED && got >= 12)
                last = be64(d), block = be32(d + 8);
            else
                block = 0, last = 0;
        }
    }
    if (block < 512 || block > 4096 || (block & (block - 1)) || !last) {
        us_log("usb: READ CAPACITY failed (block %u, last %lx, sense %x/%x/%x)", block, last, b->sense.key,
               b->sense.asc, b->sense.ascq);
        return US_EIO;
    }
    b->block = block;
    b->blocks = last + 1;
    b->read16 = last > 0xFFFFFFFFull;
    b->max_xfer = ALIGN_DOWN(US_MIN(b->max_xfer, 0xFFFFu * block), block);
    us_log("usb: %s %s, LUN %u of %u, %lu blocks of %u bytes (%lu MiB)%s", b->vendor, b->product, b->lun,
           b->max_lun + 1, b->blocks, block, (b->blocks * block) >> 20, b->removable ? ", removable" : "");
    return 0;
}

int bot_read(struct bot *b, u64 lba, u32 count, void *buf)
{
    u8 *p = buf;
    u32 tries = 0;
    if (!b->block || lba >= b->blocks || count > b->blocks - lba)
        return US_EINVAL;
    while (count) {
        u32 n = US_MIN(count, b->max_xfer / b->block), got = 0;
        u8 cdb[16];
        memset(cdb, 0, sizeof(cdb));
        u8 len;
        if (b->read16 || lba + n - 1 > 0xFFFFFFFFull) {
            cdb[0] = 0x88;
            put_be32(cdb + 2, (u32)(lba >> 32));
            put_be32(cdb + 6, (u32)lba);
            put_be32(cdb + 10, n);
            len = 16;
        } else {
            cdb[0] = 0x28;
            put_be32(cdb + 2, (u32)lba);
            cdb[7] = (u8)(n >> 8), cdb[8] = (u8)n;
            len = 10;
        }
        int st = bot_command(b, cdb, len, p, n * b->block, true, &got);
        u32 blocks = got / b->block;
        if (st == BOT_PASSED && blocks < n)
            b->short_reads++;
        if (blocks) {
            /* keep what arrived whole; reissue the rest */
            p += (u64)blocks * b->block, lba += blocks, count -= blocks;
            tries = 0;
            continue;
        }
        if (++tries > 5 || (st == BOT_FAILED && !transient(&b->sense))) {
            us_log("usb: READ of %u blocks at %lx failed (status %d, sense %x/%x/%x)", n, lba, st, b->sense.key,
                   b->sense.asc, b->sense.ascq);
            return US_EIO;
        }
        if (st == BOT_FAILED)
            us_sleep_us(50000);
    }
    return 0;
}

static int bot_blk_read(void *ctx, u64 lba, u32 count, void *buf) { return bot_read(ctx, lba, count, buf); }

void bot_blkdev(struct bot *b, struct blkdev *out)
{
    out->ctx = b;
    out->sector = b->block;
    out->sectors = b->blocks;
    out->max_sectors = b->max_xfer / b->block;
    out->read = bot_blk_read;
}
