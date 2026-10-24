/*
 * Route B's console side (docs/usb.md): the USB storage stack's environment
 * inside the extension, and SWRR's libSceUsbd as the transport's struct
 * usbd_ops.  approot.c calls usb_attach before the title's modules load,
 * when the app root is on a USB drive that AutoMounter could not mount
 * into the sandbox.
 *
 * libSceUsbd is libusb 1.0 under other names: sceUsbdInit, GetDeviceList,
 * Open, KernelDriverActive, DetachKernelDriver, ClaimInterface,
 * BulkTransfer, ControlTransfer, ClearHalt, ...  The title's own imports of
 * it are virtual (hle_peripheral.c: no devices); these calls go to SWRR's
 * module directly.
 *
 * Built only with USB_RAW=1 (docs/usb.md): on a 13.60 PS5, SWRR's
 * libSceUsbd initializes but lists no devices, so this finds no drive.  The
 * sandbox's /dev/ugenB.A nodes are world read-write and FreeBSD's libusb20
 * calls on them (descriptors, USB_DO_REQUEST, USB_FS_INIT/OPEN/START/
 * COMPLETE on endpoint 0) work on a stock console: a struct usbd_ops over
 * those nodes is the way to revive this.  Bulk transfers to a drive the
 * system's umass driver holds have not been tried.  Still unchecked here:
 * the libusb signatures (k_usbd), the error codes (usbd_rc), and USB_XFER.
 * The device's descriptors are parsed from raw bytes (GET_DESCRIPTOR), so
 * no libusb struct layout is assumed beyond the 18-byte device descriptor.
 */
#include "ext.h"
#include "usbstore.h"

/* bytes per READ (unchecked: the largest bulk transfer the transport takes).
 * On a modelled bus (test_usb.py) 128 KiB streams at ~87% of the bus, 512 KiB
 * at ~96% */
#define USB_XFER (128u << 10)
/* 16 KiB lines: a random read brings in little it does not need; streams
 * are read ahead up to CACHE_RA lines per device read */
#define CACHE_LINE (16u << 10)
#define CACHE_META 64         /* lines: 1 MiB for FAT and directories */
#define CACHE_DATA 192        /* lines: 3 MiB of file data */
#define CACHE_RA 32           /* lines per device read at most: 512 KiB */

/* ---- the environment usbstore.h asks for ---- */

void *us_alloc(u64 size) { return page_alloc(size); } /* zeroed */
void us_free(void *p, u64 size) { page_free(p, size); }
void us_sleep_us(u32 us) { K.usleep(us); }

void us_log(const char *f, ...)
{
    char line[400];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    vfmt(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    logf("%s", line);
}

/* as ghostfs's: SWRR's own /app0 directory (its eboot.bin if the sandbox
 * refuses the directory); the path is copied to the stack, since the
 * kernel reads it and literals live in JIT memory */
s32 us_placeholder_fd(void)
{
    static int dir_ok = -1;
    char p[32];
    strlcpy(p, dir_ok ? "/app0" : "/app0/eboot.bin", sizeof(p));
    int fd = K.open(p, 0, 0);
    if (dir_ok == -1) {
        dir_ok = fd >= 0;
        if (!dir_ok) {
            strlcpy(p, "/app0/eboot.bin", sizeof(p));
            fd = K.open(p, 0, 0);
        }
    }
    return fd;
}

void us_placeholder_close(s32 fd) { K.close(fd); }

/* ---- SWRR's libSceUsbd ---- */

static struct {
    s32 (*init)(void);
    s64 (*get_list)(void ***list);
    void (*free_list)(void **list, s32 unref);
    s32 (*device_desc)(void *dev, u8 desc[18]);
    s32 (*open)(void *dev, void **handle);
    void (*close)(void *handle);
    s32 (*driver_active)(void *handle, s32 iface);
    s32 (*detach)(void *handle, s32 iface);
    s32 (*attach)(void *handle, s32 iface);
    s32 (*claim)(void *handle, s32 iface);
    s32 (*release)(void *handle, s32 iface);
    s32 (*bulk)(void *handle, u8 ep, u8 *data, s32 len, s32 *done, u32 timeout_ms);
    s32 (*control)(void *handle, u8 type, u8 req, u16 value, u16 index, u8 *data, u16 len, u32 timeout_ms);
    s32 (*clear_halt)(void *handle, u8 ep);
} S;

static const char *const k_usbd[] = {
    "sceUsbdInit", "sceUsbdGetDeviceList", "sceUsbdFreeDeviceList", "sceUsbdGetDeviceDescriptor", "sceUsbdOpen",
    "sceUsbdClose", "sceUsbdKernelDriverActive", "sceUsbdDetachKernelDriver", "sceUsbdAttachKernelDriver",
    "sceUsbdClaimInterface", "sceUsbdReleaseInterface", "sceUsbdBulkTransfer", "sceUsbdControlTransfer",
    "sceUsbdClearHalt",
};

static struct {
    void *handle;
    u8 iface, ep_in, ep_out, detached;
    u16 vid, pid;
} D;

static struct bot g_bot;
static struct blk g_blk;

static int usbd_bind(void)
{
    void **fn = (void **)&S;
    for (u32 i = 0; i < ARRAY_SIZE(k_usbd); i++) {
        fn[i] = host_real_in("libSceUsbd", k_usbd[i]);
        /* the kernel driver calls are optional: without them the drive may still be claimable */
        if (!fn[i] && i != 6 && i != 7 && i != 8) {
            logf("usb: libSceUsbd has no %s: no raw USB access", k_usbd[i]);
            return -1;
        }
    }
    return 0;
}

/* libSceUsbd's result as a USBD_* (its error codes unchecked) */
static int usbd_rc(s32 rc)
{
    if (rc >= 0)
        return USBD_OK;
    if (rc == (s32)0x80240009 || rc == -9) /* PIPE: the endpoint stalled */
        return USBD_STALL;
    if (rc == (s32)0x80240007 || rc == -7) /* TIMEOUT */
        return USBD_TIMEOUT;
    return USBD_ERROR;
}

static int o_bulk(void *ctx, u8 ep, void *buf, u32 len, u32 *done, u32 timeout_ms)
{
    s32 n = 0;
    s32 rc = S.bulk(D.handle, ep, buf, (s32)len, &n, timeout_ms);
    *done = n > 0 ? (u32)n : 0;
    return usbd_rc(rc);
}

static int o_control(void *ctx, u8 type, u8 req, u16 value, u16 index, void *data, u16 len, u32 timeout_ms)
{
    s32 rc = S.control(D.handle, type, req, value, index, data, len, timeout_ms);
    return rc >= 0 ? rc : usbd_rc(rc);
}

static int o_clear_halt(void *ctx, u8 ep) { return usbd_rc(S.clear_halt(D.handle, ep)); }

static const struct usbd_ops k_ops = {NULL, o_bulk, o_control, o_clear_halt};

/* the Bulk-Only mass-storage interface in a configuration descriptor: 0 with iface and endpoints */
static int msc_interface(const u8 *cfg, u32 n)
{
    int cur = -1;
    u8 in = 0, out = 0;
    for (u32 at = 0; at + 2 <= n && cfg[at] >= 2 && at + cfg[at] <= n; at += cfg[at]) {
        const u8 *d = cfg + at;
        if (d[1] == 4 && d[0] >= 9) { /* interface */
            if (cur >= 0 && in && out)
                break;
            /* class 8 mass storage, subclass 6 SCSI transparent, protocol 0x50 Bulk-Only */
            cur = d[5] == 8 && d[6] == 6 && d[7] == 0x50 && d[3] == 0 ? d[2] : -1;
            in = out = 0;
        } else if (d[1] == 5 && d[0] >= 7 && cur >= 0 && (d[3] & 3) == 2) { /* a bulk endpoint */
            if (d[2] & 0x80)
                in = in ? in : d[2];
            else
                out = out ? out : d[2];
        }
    }
    if (cur < 0 || !in || !out)
        return -1;
    D.iface = (u8)cur, D.ep_in = in, D.ep_out = out;
    return 0;
}

static int find_msc(void)
{
    void **list = NULL;
    s64 n = S.get_list(&list);
    if (n <= 0 || !list) {
        logf("usb: sceUsbdGetDeviceList -> %lx: no USB devices", (u64)n);
        return -1;
    }
    int found = -1;
    for (s64 i = 0; i < n && found; i++) {
        u8 dd[18];
        memset(dd, 0, sizeof(dd));
        if (S.device_desc(list[i], dd) != 0)
            continue;
        void *h = NULL;
        s32 rc = S.open(list[i], &h);
        if (rc != 0 || !h) {
            logf("usb: device %04x:%04x: sceUsbdOpen -> %x", dd[8] | dd[9] << 8, dd[10] | dd[11] << 8, (u32)rc);
            continue;
        }
        D.handle = h;
        u8 cfg[512];
        memset(cfg, 0, sizeof(cfg));
        int got = o_control(NULL, 0x80, 6, 0x0200, 0, cfg, 9, 1000); /* GET_DESCRIPTOR(CONFIGURATION) */
        u32 total = got >= 4 ? (u32)(cfg[2] | cfg[3] << 8) : 0;
        if (total > sizeof(cfg))
            total = sizeof(cfg);
        if (total >= 9)
            got = o_control(NULL, 0x80, 6, 0x0200, 0, cfg, (u16)total, 1000);
        if (total < 9 || got < 9 || msc_interface(cfg, (u32)got) != 0) {
            S.close(h);
            D.handle = NULL;
            continue;
        }
        D.vid = (u16)(dd[8] | dd[9] << 8), D.pid = (u16)(dd[10] | dd[11] << 8);
        if (S.driver_active && S.detach && S.driver_active(h, D.iface) == 1) {
            rc = S.detach(h, D.iface);
            D.detached = rc == 0;
            logf("usb: %04x:%04x: the system's driver let go of interface %u -> %x", D.vid, D.pid, D.iface, (u32)rc);
        }
        rc = S.claim(h, D.iface);
        if (rc != 0) {
            logf("usb: %04x:%04x: sceUsbdClaimInterface(%u) -> %x", D.vid, D.pid, D.iface, (u32)rc);
            if (D.detached && S.attach)
                S.attach(h, D.iface);
            S.close(h);
            D.handle = NULL, D.detached = 0;
            continue;
        }
        logf("usb: %04x:%04x: mass storage on interface %u, bulk in %02x, out %02x", D.vid, D.pid, D.iface, D.ep_in,
             D.ep_out);
        found = 0;
    }
    S.free_list(list, 1);
    return found;
}

/* give the drive back as it was found */
static void usb_release(void)
{
    if (!D.handle)
        return;
    S.release(D.handle, D.iface);
    if (D.detached && S.attach)
        S.attach(D.handle, D.iface);
    S.close(D.handle);
    D.handle = NULL, D.detached = 0;
}

const struct appfs_ops *usb_attach(u32 ctx_flags)
{
    if (usbd_bind() != 0)
        return NULL;
    s32 rc = S.init();
    logf("usb: sceUsbdInit -> %x", (u32)rc);
    if (find_msc() != 0) {
        logf("usb: no USB mass-storage drive (Bulk-Only, SCSI) found");
        return NULL;
    }
    bot_init(&g_bot, &k_ops, D.iface, D.ep_in, D.ep_out);
    g_bot.max_xfer = USB_XFER;
    struct blkdev dev;
    if (bot_attach(&g_bot, 0) != 0)
        goto fail;
    bot_blkdev(&g_bot, &dev);
    if (blk_init(&g_blk, &dev, CACHE_LINE, CACHE_META, CACHE_DATA, CACHE_RA) != 0 || usbfs_mount(&g_blk, -1) != 0)
        goto fail;
    return usbfs_ops();
fail:
    blk_free(&g_blk);
    usb_release();
    return NULL;
}
