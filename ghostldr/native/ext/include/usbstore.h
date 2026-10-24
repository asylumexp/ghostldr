/*
 * The USB storage stack (docs/usb.md, route B): a title's /app0 read from a
 * USB drive the extension drives itself, through SWRR's libSceUsbd.
 *
 *   usb_bot.c   USB Mass Storage Bulk-Only Transport and the SCSI commands
 *               a drive needs, over a small "usbd ops" interface
 *   blk.c       a block cache with read-ahead for streaming
 *   part.c      MBR and GPT partition tables (or none: a "superfloppy")
 *   fsutil.c    what exFAT and FAT share: cluster chains, names, times
 *   exfat.c     read-only exFAT
 *   fat32.c     read-only FAT32 (and FAT16) with long file names
 *   usbfs.c     the app-root backend: open/read/stat/getdents as ghostfs
 *               gives them, behind struct appfs_ops
 *   usb_sce.c   the console side: libSceUsbd calls and this environment
 *
 * Everything but usb_sce.c is plain freestanding C with no use of the core:
 * the host tests (ghostldr/tests/test_usb.py) build it natively against
 * disk images and a fake BOT device.  The environment below is all it needs.
 */
#pragma once
#include "ghost.h"
#include "ghost_api.h"

/* ---- environment (usb_sce.c on the console, the harness on the host) ---- */
void *us_alloc(u64 size);          /* zeroed memory; NULL when there is none */
void us_free(void *p, u64 size);
void us_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void us_sleep_us(u32 us);
s32 us_placeholder_fd(void);       /* a real fd for the title to hold for a usbfs file (< 0: error) */
void us_placeholder_close(s32 fd);

/* SCE kernel errors (0x80020000 + errno), negative in s64 results */
#define US_ENOENT ((s32)0x80020002)
#define US_EIO ((s32)0x80020005)
#define US_EBADF ((s32)0x80020009)
#define US_ENOMEM ((s32)0x8002000c)
#define US_ENOTDIR ((s32)0x80020014)
#define US_EISDIR ((s32)0x80020015)
#define US_EINVAL ((s32)0x80020016)
#define US_EMFILE ((s32)0x80020018)
#define US_EROFS ((s32)0x8002001e)
#define US_ENAMETOOLONG ((s32)0x8002003f)
#define US_ENODEV ((s32)0x80020013)

#define US_MIN(a, b) ((a) < (b) ? (a) : (b))

/* ==== USB transport (usb_bot.c) ==== */

/* results of struct usbd_ops calls */
#define USBD_OK 0
#define USBD_STALL (-1)   /* the endpoint halted (libusb PIPE) */
#define USBD_TIMEOUT (-2)
#define USBD_ERROR (-3)   /* anything else: the device is gone, I/O error, ... */

/* What the transport needs from a USB host stack: libusb's synchronous calls
 * on one opened device whose mass-storage interface is claimed. */
struct usbd_ops {
    void *ctx;
    /* one bulk transfer on ep (bit 7: IN); *done = bytes moved, also on errors */
    int (*bulk)(void *ctx, u8 ep, void *buf, u32 len, u32 *done, u32 timeout_ms);
    /* a control transfer on endpoint 0: bytes moved, or a USBD_* error */
    int (*control)(void *ctx, u8 type, u8 request, u16 value, u16 index, void *data, u16 len, u32 timeout_ms);
    /* CLEAR_FEATURE(ENDPOINT_HALT) and the host side's data toggle (libusb_clear_halt) */
    int (*clear_halt)(void *ctx, u8 ep);
};

struct scsi_sense {
    u8 key, asc, ascq;
};

struct bot {
    const struct usbd_ops *u;
    u8 iface, ep_in, ep_out, lun, max_lun;
    u32 tag;
    u32 block;             /* bytes per logical block (READ CAPACITY) */
    u64 blocks;
    u32 max_xfer;          /* bytes per READ command */
    bool read16;           /* READ(16) needed: blocks past 2^32 */
    char vendor[9], product[17];
    u8 removable, devtype;
    struct scsi_sense sense; /* of the last failed command */
    /* counters (logged, and checked by the tests) */
    u32 commands, stalls, resets, retries, short_reads, phase_errors, bad_csw;
};

/* Status of a command (bot_command): */
#define BOT_PASSED 0
#define BOT_FAILED 1       /* CSW status 1: REQUEST SENSE says why (b->sense) */
#define BOT_ERROR (-1)     /* transport failure even after reset recovery */

int bot_init(struct bot *b, const struct usbd_ops *u, u8 iface, u8 ep_in, u8 ep_out);
/* GET MAX LUN, INQUIRY, TEST UNIT READY until ready, READ CAPACITY(10/16) */
int bot_attach(struct bot *b, u8 lun);
/* one SCSI command: data in (in) or out of buf, len bytes; *xfer = bytes moved */
int bot_command(struct bot *b, const u8 *cdb, u8 cdb_len, void *buf, u32 len, bool in, u32 *xfer);
int bot_request_sense(struct bot *b);
/* count blocks from lba into buf (READ(10) or READ(16), split by max_xfer,
 * short reads reissued, unit attentions retried): 0 or < 0 */
int bot_read(struct bot *b, u64 lba, u32 count, void *buf);
void bot_reset_recovery(struct bot *b);

/* ==== block devices and the cache (blk.c) ==== */

struct blkdev {
    void *ctx;
    u32 sector;            /* bytes per logical block: 512 or 4096 */
    u64 sectors;
    u32 max_sectors;       /* per read call */
    int (*read)(void *ctx, u64 lba, u32 count, void *buf); /* all of it, or < 0 */
};
void bot_blkdev(struct bot *b, struct blkdev *out);

/* hints for blk_read: metadata (FAT, directories) has a pool of its own, so
 * streaming a large file never pushes it out */
#define BLK_DATA 0
#define BLK_META 1

struct blk_line {
    u64 tag;               /* line number (device byte offset / line size); ~0: empty */
    u64 used;              /* LRU clock */
};
struct blk_pool {
    struct blk_line *line;
    u8 *data;
    u32 n;
};
struct blk {
    struct blkdev dev;
    u32 line;              /* bytes per cache line: a power of two, a multiple of the sector */
    u32 line_shift;
    u64 size;              /* device bytes */
    struct blk_pool pool[2];
    u8 *stage;             /* read-ahead lands here first */
    u32 ra_max;            /* lines per device read at most (the staging buffer) */
    u32 ra;                /* read-ahead past a request: grows while requests follow each other */
    u64 prev_last;         /* the last line the previous data request touched */
    bool seq;              /* this request continues the previous one */
    u64 clock;
    u32 direct_min;        /* whole lines a read must cover to bypass the cache */
    void *mem;
    u64 mem_size;
    /* counters */
    u64 hits, misses, dev_reads, dev_bytes, direct_bytes, ra_lines;
};

/* line_bytes: power of two >= the sector; meta/data: lines per pool; ra_max: read-ahead lines */
int blk_init(struct blk *b, const struct blkdev *dev, u32 line_bytes, u32 meta_lines, u32 data_lines, u32 ra_max);
void blk_free(struct blk *b);
/* n bytes at device byte offset off: n, or < 0 */
s64 blk_read(struct blk *b, u64 off, void *buf, u64 n, int hint);
/* n bytes inside one cache line, in place (until the next blk call); NULL on errors */
const u8 *blk_peek(struct blk *b, u64 off, u32 n, int hint);

/* ==== partitions (part.c) ==== */

#define PART_MAX 16
struct part {
    u64 start, sectors;    /* in device sectors */
    u8 mbr_type;           /* MBR type byte; 0 for GPT and whole-disk */
    u8 gpt;                /* from a GPT */
    u8 basic_data;         /* GPT type is Microsoft basic data (exFAT/FAT) */
    u8 whole;              /* no partition table: the volume starts at sector 0 */
    u8 esp;                /* an EFI system partition (FAT, but never the user's files) */
};
/* the partitions in table order (at most max): count, or < 0 */
int part_scan(struct blk *b, struct part *out, int max);
u32 crc32_le(u32 crc, const void *p, u64 n);

/* ==== file systems (fsutil.c, exfat.c, fat32.c) ==== */

struct fs_node {
    u64 size;              /* bytes; ~0 for a FAT directory: as far as its chain goes */
    u64 valid;             /* bytes with data: past it a file reads as zeros (exFAT ValidDataLength) */
    u32 first;             /* first cluster; 0: none (empty file) */
    u32 ino;
    u8 dir;
    u8 contig;             /* exFAT NoFatChain: clusters first, first+1, ... */
    u8 fixed_root;         /* FAT16's root directory: a fixed region, no clusters */
    u8 attr;               /* FAT attribute bits (0x01 read-only, 0x02 hidden, 0x04 system, 0x10 dir, 0x20 archive) */
    s64 mtime, atime, btime; /* Unix seconds; 0: unknown */
    u32 mtime_ns, btime_ns;
};

#define FS_NAME_MAX 1024   /* UTF-8 bytes of one name (255 UTF-16 units), NUL included */
struct fs_dirent {
    char name[FS_NAME_MAX];
    u32 len;
    struct fs_node node;
};

/* the clusters of one chain, as runs: a file's own walk keeps sequential
 * and nearby reads from walking the FAT again */
#define FS_RUNS 24
struct fs_run {
    u32 vcn, lcn, len;     /* file cluster, disk cluster, clusters */
};
struct fs_walk {
    u32 first;             /* the chain these runs belong to */
    u32 nrun;
    struct fs_run run[FS_RUNS];
    u32 next;              /* the FAT's entry for the last run's last cluster */
    u8 end;                /* the chain ends after the last run */
    u8 bad;                /* the chain is broken there */
};

struct vol;
struct fs_ops {
    const char *name;
    /* the child of dir called name (len bytes of UTF-8): 0, US_ENOENT, or < 0 */
    int (*lookup)(struct vol *v, const struct fs_node *dir, const char *name, u32 len, struct fs_node *out);
    /* the entry at *cookie or after (0: the first), *cookie moved past it: 1, 0 at the end, < 0.
     * w: the directory's walk, kept between calls */
    int (*readdir)(struct vol *v, const struct fs_node *dir, struct fs_walk *w, u64 *cookie, struct fs_dirent *out);
};

struct vol {
    struct blk *blk;
    u64 off;               /* device byte offset of the partition */
    u64 size;              /* partition bytes */
    u32 sector;            /* bytes per FS sector */
    u32 cluster_shift;     /* log2 of bytes per cluster */
    u64 heap;              /* volume byte offset of cluster 2 */
    u32 clusters;          /* clusters in the heap */
    u64 fat;               /* volume byte offset of the FAT in use */
    u8 fat_bits;           /* 16 or 32 (exFAT: 32 bits, FAT32: 28) */
    u8 exfat;
    u64 root_off, root_size; /* FAT16's fixed root directory */
    const struct fs_ops *ops;
    struct fs_node root;
    u16 *upcase;           /* exFAT's up-case table, 65536 entries; NULL: built-in folding */
    u64 upcase_size;
    char label[48];        /* UTF-8 volume label */
    u32 serial;
    u32 chain_errors;
};

/* the next cluster after cl: 0 at the chain's end, 1 for a broken chain */
u32 fs_fat_next(struct vol *v, u32 cl);
/* bytes of node at off (directories too): the bytes read (short at the end
 * of a chain), or < 0.  w may be NULL (a walk on the stack) */
s64 fs_read(struct vol *v, const struct fs_node *n, struct fs_walk *w, u64 off, void *buf, u64 len, int hint);
/* the disk cluster holding file cluster vcn: 0 past the chain's end, 1 broken */
u32 fs_map(struct vol *v, const struct fs_node *n, struct fs_walk *w, u32 vcn);
/* a whole chain's length in clusters (at most limit) */
u32 fs_chain_len(struct vol *v, u32 first, u32 limit);

/* names: UTF-8 <-> UTF-16, case folding */
u32 utf16_to_utf8(const u16 *s, u32 n, char *out, u32 cap); /* bytes written (NUL added), 0 if it does not fit */
int utf8_next(const char **p, const char *end);              /* a code point, or -1 for bad UTF-8 */
u32 utf8_to_utf16(const char *s, u32 len, u16 *out, u32 cap); /* units, 0 for bad UTF-8 or too long */
u16 fs_upcase(const struct vol *v, u16 c);
/* FAT and exFAT timestamps -> Unix seconds.  utc_min: minutes east of UTC
 * the time was written in (exFAT's offset field; FAT: 0, read as UTC) */
s64 fs_dos_time(u16 date, u16 time, s32 utc_min);

/* mount the file system on the partition (exFAT, then FAT32/16): 0, or US_ENODEV when neither is there */
int exfat_mount(struct vol *v, struct blk *b, u64 off, u64 size);
int fat_mount(struct vol *v, struct blk *b, u64 off, u64 size);
int fs_mount(struct vol *v, struct blk *b, const struct part *p);
void fs_unmount(struct vol *v);

/* ==== usb_sce.c: the console's USB drive (route B) ==== */
/* find a USB mass-storage device through SWRR's libSceUsbd, mount its
 * exFAT or FAT32 volume: the provider serving it as /usb0, or NULL */
const struct appfs_ops *usb_attach(u32 ctx_flags);

/* ==== usbfs.c: the app root ==== */

/* paths it answers: /usb0/...  the drive's volume; /app0/...  the title's
 * app0 on it once usbfs_set_root has named it */
int usbfs_mount(struct blk *b, int part_index);   /* -1: the first partition with a file system */
int usbfs_set_root(const char *path);             /* "/ghostldr/CUSA12345/app0": 0, or US_ENOENT */
const struct appfs_ops *usbfs_ops(void);
void usbfs_stats(char *out, u32 size);            /* one line for the log */
void usbfs_unmount(void);
/* diagnostics and tests: the node at a usbfs path, and the mounted volume */
int usbfs_node(const char *path, struct fs_node *out);
struct vol *usbfs_vol(void);
int usbfs_part(struct part *out); /* the partition mounted: its index */
