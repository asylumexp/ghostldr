/*
 * The extension's USB storage stack (ext/src/usb_bot.c, blk.c, part.c,
 * fsutil.c, exfat.c, fat32.c, usbfs.c) in a host process, against a raw
 * disk image: read straight from the file (a block device), or through a
 * fake USB mass-storage device that answers BOT commands from the image and
 * can stall, send short data, corrupt CSWs, report phase errors and unit
 * attentions on a schedule.  test_usb.py builds it and checks what it
 * prints against the tree it copied onto the image.
 *
 *     usb_host [options] IMAGE walk [--hash-max N] [--dents N]
 *     usb_host [options] IMAGE read PATH OFF LEN
 *     usb_host [options] IMAGE bench PATH CHUNK [RANDOM_READS]
 *     usb_host [options] IMAGE app0 ROOT
 *     usb_host [options] IMAGE parts
 *
 * options: --bot FAULTS (k=n,... every n-th event; "none"), --block N (the
 * fake device's block size), --part N, --line N, --meta N, --data N, --ra N,
 * --xfer N (bytes per READ), --mmap (the image mapped: no system call per read)
 */
#include "usbstore.h"

/* libc, declared here: its headers clash with ghost.h's freestanding ones */
long write(int fd, const void *buf, unsigned long n);
int open(const char *path, int flags, ...);
int close(int fd);
long pread(int fd, void *buf, unsigned long n, long off);
long lseek(int fd, long off, int whence);
void *calloc(unsigned long n, unsigned long size);
void free(void *p);
int vsnprintf(char *buf, unsigned long size, const char *fmt, __builtin_va_list ap);
long strtol(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
void exit(int code);
int usleep(unsigned us);
void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off);
struct timespec_ {
    long sec, nsec;
};
int clock_gettime(int clock, struct timespec_ *ts);
#ifdef __APPLE__
#define CLOCK_MONO 6
#else
#define CLOCK_MONO 1
#endif

int vfmt(char *buf, size_t size, const char *f, __builtin_va_list ap) { return vsnprintf(buf, size, f, ap); }
int fmt(char *buf, size_t size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(buf, size, f, ap);
    __builtin_va_end(ap);
    return n;
}

static void out(int fd, const char *f, ...)
{
    char line[2400];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(line, sizeof(line) - 1, f, ap);
    __builtin_va_end(ap);
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    write(fd, line, (unsigned long)n);
}
#define say(...) out(1, __VA_ARGS__)
#define die(...) (out(2, __VA_ARGS__), exit(2))

static u64 g_live, g_live_peak;
static int g_quiet;
void *us_alloc(u64 size)
{
    g_live += size;
    if (g_live > g_live_peak)
        g_live_peak = g_live;
    return calloc(1, size ? size : 1);
}
void us_free(void *p, u64 size)
{
    g_live -= size;
    free(p);
}
void us_log(const char *f, ...)
{
    if (g_quiet)
        return;
    char line[1200];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    vfmt(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    out(1, "log: %s", line);
}
void us_sleep_us(u32 us) { (void)us; }
static int g_next_fd = 1000, g_open_fds;
s32 us_placeholder_fd(void)
{
    g_open_fds++;
    return g_next_fd++;
}
void us_placeholder_close(s32 fd) { g_open_fds--; }

static u64 now_ns(void)
{
    struct timespec_ ts;
    clock_gettime(CLOCK_MONO, &ts);
    return (u64)ts.sec * 1000000000ull + (u64)ts.nsec;
}

/* ---- the image ---- */
static int g_img = -1;
static const u8 *g_map;
static u64 g_img_size;

static int img_read(u64 off, void *buf, u64 n)
{
    if (off > g_img_size || n > g_img_size - off)
        return -1;
    if (g_map) {
        memcpy(buf, g_map + off, n);
        return 0;
    }
    for (u64 done = 0; done < n;) {
        long r = pread(g_img, (u8 *)buf + done, n - done, (long)(off + done));
        if (r <= 0)
            return -1;
        done += (u64)r;
    }
    return 0;
}

/* a block device straight on the image */
static u32 g_block = 512;
static int raw_read(void *ctx, u64 lba, u32 count, void *buf)
{
    return img_read(lba * g_block, buf, (u64)count * g_block) ? US_EIO : 0;
}

/* ---- a fake USB mass-storage device (BOT) ---- */
enum { EV_STALL_DATA, EV_STALL_CSW, EV_SHORT, EV_BAD_CSW, EV_PHASE, EV_UA, EV_UA_READ, EV_RC16, EV_NOLUN,
       EV_CSW_IN_DATA, EV_BAD_CBW, EV_COUNT };
static const char *const k_ev[EV_COUNT] = {"stall_data", "stall_csw", "short",   "bad_csw", "phase",      "ua",
                                           "ua_read",    "rc16",      "nolun",   "csw_in_data", "bad_cbw"};
static u32 g_every[EV_COUNT], g_seen[EV_COUNT], g_fired[EV_COUNT];

/* every n-th event; "ua" instead fails the first n TEST UNIT READYs */
static bool fire(int ev)
{
    if (!g_every[ev])
        return false;
    bool hit = ev == EV_UA ? ++g_seen[ev] <= g_every[ev] : ++g_seen[ev] % g_every[ev] == 0;
    g_fired[ev] += hit;
    return hit;
}

static struct {
    enum { S_CBW, S_DATA, S_CSW, S_DEAD } state;
    bool halt_in, halt_out, need_reset;
    u32 tag, residue;
    u8 status;
    u8 resp[64];
    const u8 *data;       /* resp, or NULL: image bytes from data_off */
    u64 data_off;
    u32 data_len;         /* what the device will send */
    u32 want;             /* the CBW's length */
    struct scsi_sense sense;
    u32 ua_pending;
    u32 commands, resets, clears;
    u32 skipped_data;     /* the data phase was skipped: the next IN is the CSW */
} D;

static u32 rd32le(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u32 rd32be(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void wr32be(u8 *p, u32 v) { p[0] = (u8)(v >> 24), p[1] = (u8)(v >> 16), p[2] = (u8)(v >> 8), p[3] = (u8)v; }
static void wr32le(u8 *p, u32 v) { p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24); }

static void fail(u8 key, u8 asc, u8 ascq)
{
    D.status = 1;
    D.sense = (struct scsi_sense){key, asc, ascq};
}

static void scsi(const u8 *cdb, u32 want)
{
    u64 blocks = g_img_size / g_block;
    D.status = 0, D.data = D.resp, D.data_len = 0;
    memset(D.resp, 0, sizeof(D.resp));
    switch (cdb[0]) {
    case 0x12: /* INQUIRY */
        D.resp[1] = 0x80, D.resp[2] = 6, D.resp[3] = 2, D.resp[4] = 31;
        memcpy(D.resp + 8, "GHOSTLDR", 8);
        memcpy(D.resp + 16, "FAKE BOT DRIVE  ", 16);
        memcpy(D.resp + 32, "0001", 4);
        D.data_len = 36;
        break;
    case 0x00: /* TEST UNIT READY */
        if (fire(EV_UA))
            fail(6, 0x29, 0);
        break;
    case 0x03: /* REQUEST SENSE */
        D.resp[0] = 0x70, D.resp[2] = D.sense.key, D.resp[7] = 10, D.resp[12] = D.sense.asc, D.resp[13] = D.sense.ascq;
        D.data_len = 18;
        D.sense = (struct scsi_sense){0, 0, 0};
        break;
    case 0x25: /* READ CAPACITY (10) */
        if (D.ua_pending) {
            D.ua_pending--;
            fail(6, 0x28, 0);
            break;
        }
        wr32be(D.resp, blocks > 0xFFFFFFFFull || fire(EV_RC16) ? 0xFFFFFFFFu : (u32)(blocks - 1));
        wr32be(D.resp + 4, g_block);
        D.data_len = 8;
        break;
    case 0x9E: /* READ CAPACITY (16) */
        if ((cdb[1] & 0x1F) != 0x10) {
            fail(5, 0x24, 0);
            break;
        }
        wr32be(D.resp, (u32)((blocks - 1) >> 32));
        wr32be(D.resp + 4, (u32)(blocks - 1));
        wr32be(D.resp + 8, g_block);
        D.data_len = 32;
        break;
    case 0x28:   /* READ (10) */
    case 0x88: { /* READ (16) */
        u64 lba = cdb[0] == 0x28 ? rd32be(cdb + 2) : (u64)rd32be(cdb + 2) << 32 | rd32be(cdb + 6);
        u32 n = cdb[0] == 0x28 ? (u32)(cdb[7] << 8 | cdb[8]) : rd32be(cdb + 10);
        if (fire(EV_UA_READ)) {
            fail(6, 0x28, 0);
            break;
        }
        if (lba >= blocks || n > blocks - lba) {
            fail(5, 0x21, 0);
            break;
        }
        D.data = NULL, D.data_off = lba * g_block, D.data_len = n * g_block;
        break;
    }
    default:
        fail(5, 0x20, 0);
    }
    if (D.data_len > want)
        D.data_len = want;
    D.residue = want - D.data_len;
}

static int dev_bulk(void *ctx, u8 ep, void *buf, u32 len, u32 *done, u32 timeout)
{
    *done = 0;
    bool in = ep & 0x80;
    if ((in && D.halt_in) || (!in && D.halt_out))
        return USBD_STALL;
    if (!in) {
        if (D.state != S_CBW || len != 31 || rd32le(buf) != 0x43425355u || fire(EV_BAD_CBW)) {
            /* an invalid CBW: both endpoints stall until a reset recovery */
            D.halt_in = D.halt_out = D.need_reset = true;
            D.state = S_DEAD;
            *done = len;
            return USBD_OK;
        }
        const u8 *cbw = buf;
        D.commands++;
        D.tag = rd32le(cbw + 4);
        D.want = rd32le(cbw + 8);
        D.skipped_data = 0;
        *done = 31;
        scsi(cbw + 15, D.want);
        bool dir_in = cbw[12] & 0x80;
        if (D.want && dir_in) {
            D.state = S_DATA;
            if (D.status && fire(EV_CSW_IN_DATA)) /* no data to give: the CSW comes in its place */
                D.skipped_data = 1;
        } else {
            D.state = S_CSW;
        }
        return USBD_OK;
    }
    if (D.state == S_DATA && D.skipped_data) {
        u8 csw[13];
        wr32le(csw, 0x53425355u), wr32le(csw + 4, D.tag), wr32le(csw + 8, D.want), csw[12] = D.status;
        u32 k = len < 13 ? len : 13;
        memcpy(buf, csw, k);
        *done = k;
        D.state = S_CBW;
        return USBD_OK;
    }
    if (D.state == S_DATA) {
        u32 n = D.data_len < len ? D.data_len : len;
        if (n && fire(EV_STALL_DATA)) {
            /* some of the data, then a stall: the CSW's residue tells the rest */
            n = (n / 2) & ~511u;
            D.halt_in = true;
            D.residue = D.want - n;
        } else if (n > 1024 && fire(EV_SHORT)) {
            n = (n / 3) & ~511u;
            D.residue = D.want - n;
        }
        if (n && (D.data ? (memcpy(buf, D.data, n), 0) : img_read(D.data_off, buf, n)))
            return USBD_ERROR;
        *done = n;
        D.state = S_CSW;
        return D.halt_in ? USBD_STALL : USBD_OK;
    }
    if (D.state == S_CSW) {
        if (fire(EV_STALL_CSW)) {
            D.halt_in = true;
            return USBD_STALL;
        }
        u8 csw[13];
        wr32le(csw, 0x53425355u), wr32le(csw + 4, D.tag), wr32le(csw + 8, D.residue);
        csw[12] = D.status;
        if (fire(EV_BAD_CSW))
            wr32le(csw + 4, D.tag ^ 0x5A5A);
        else if (fire(EV_PHASE))
            csw[12] = 2;
        u32 k = len < 13 ? len : 13;
        memcpy(buf, csw, k);
        *done = k;
        D.state = S_CBW;
        return USBD_OK;
    }
    return USBD_TIMEOUT; /* nothing to send: the device NAKs until the host gives up */
}

static int dev_control(void *ctx, u8 type, u8 req, u16 value, u16 index, void *data, u16 len, u32 timeout)
{
    if (type == 0x21 && req == 0xFF) { /* Bulk-Only Mass Storage Reset */
        D.resets++;
        D.state = S_CBW, D.need_reset = false;
        return 0;
    }
    if (type == 0xA1 && req == 0xFE) { /* GET MAX LUN */
        if (fire(EV_NOLUN))
            return USBD_STALL;
        if (len >= 1)
            *(u8 *)data = 0;
        return 1;
    }
    return USBD_STALL;
}

static int dev_clear_halt(void *ctx, u8 ep)
{
    D.clears++;
    if (D.need_reset)
        return 0; /* stays halted until the reset */
    if (ep & 0x80)
        D.halt_in = false;
    else
        D.halt_out = false;
    return 0;
}

static const struct usbd_ops k_dev = {NULL, dev_bulk, dev_control, dev_clear_halt};

/* --usb-model US,MBPS: what the same transfers would take on a real bus, a
 * fixed cost per command (CBW, CSW, turnaround) and a bandwidth */
static u64 g_model_cmd_ns, g_model_bps, g_vtime;
static int model_bulk(void *ctx, u8 ep, void *buf, u32 len, u32 *done, u32 timeout)
{
    int rc = dev_bulk(ctx, ep, buf, len, done, timeout);
    if (!(ep & 0x80) && len == 31)
        g_vtime += g_model_cmd_ns;
    else if (D.state != S_CBW || len != 13)
        g_vtime += (u64)*done * 1000000000ull / g_model_bps;
    return rc;
}
static const struct usbd_ops k_model = {NULL, model_bulk, dev_control, dev_clear_halt};

static void parse_faults(const char *s)
{
    while (s && *s && strcmp(s, "none") != 0) {
        const char *eq = s;
        while (*eq && *eq != '=')
            eq++;
        int ev = -1;
        for (int i = 0; i < EV_COUNT; i++)
            if (strlen(k_ev[i]) == (size_t)(eq - s) && strncmp(k_ev[i], s, (size_t)(eq - s)) == 0)
                ev = i;
        if (ev < 0 || !*eq)
            die("unknown fault in %s", s);
        char *end;
        g_every[ev] = (u32)strtol(eq + 1, &end, 10);
        s = *end == ',' ? end + 1 : end;
    }
}

/* ---- walking the volume through usbfs's own calls ---- */
static const struct appfs_ops *F;
static u64 g_hash_max = ~0ull;
static int g_dents = 4096;
static u64 g_files, g_dirs, g_bytes;

/* how many pieces the file's clusters are in on disk, and whether it is NoFatChain */
static u32 fragments(const char *path, u32 *contig)
{
    struct fs_node n;
    struct fs_walk w = {0};
    struct vol *v = usbfs_vol();
    *contig = 0;
    if (usbfs_node(path, &n) != 0 || n.dir || !n.first)
        return 0;
    *contig = n.contig;
    u32 frags = 0, prev = 0, count = (u32)((n.size + (1ull << v->cluster_shift) - 1) >> v->cluster_shift);
    for (u32 k = 0; k < count; k++) {
        u32 lcn = fs_map(v, &n, &w, k);
        if (lcn < 2)
            die("%s: cluster %u of %u is missing (%u)", path, k, count, lcn);
        frags += !k || lcn != prev + 1;
        prev = lcn;
    }
    return frags;
}

static void stat_line(const char *kind, const char *path, const u8 *st, const char *crc_text)
{
    u32 contig = 0, frags = kind[0] == 'F' ? fragments(path, &contig) : 0;
    say("%s\t%s\t%lu\t%s\t%ld\t%ld\t%ld\t%ld\t%ld\t%x\t%u\t%u\t%u", kind, path + 5, *(const s64 *)(st + 0x48),
        crc_text, *(const s64 *)(st + 0x28), *(const s64 *)(st + 0x30), *(const s64 *)(st + 0x68),
        *(const s64 *)(st + 0x18), *(const s64 *)(st + 0x38), *(const u16 *)(st + 8), *(const u32 *)(st + 4), frags,
        contig);
}

static void walk(const char *dir)
{
    int fd;
    if (F->open(dir, 0, &fd) != 0 || fd < 0)
        die("open %s -> %x", dir, fd);
    u8 *buf = calloc(1, (unsigned long)g_dents);
    for (;;) {
        int n = F->getdents(fd, buf, g_dents);
        if (n < 0)
            die("getdents %s -> %x", dir, n);
        if (!n)
            break;
        for (int off = 0; off < n;) {
            const u8 *d = buf + off;
            u16 reclen = *(const u16 *)(d + 4);
            const char *name = (const char *)d + 8;
            if (reclen < 12 || off + reclen > n || strlen(name) != d[7])
                die("bad dirent in %s at %d", dir, off);
            char path[2048];
            fmt(path, sizeof(path), "%s/%s", dir, name);
            u8 st[0x78], st2[0x78];
            int rc;
            if (F->stat(path, st, &rc) != 0 || rc)
                die("stat %s -> %x", path, rc);
            bool is_dir = (*(u16 *)(st + 8) & 0xF000) == 0x4000;
            if (is_dir != (d[6] == 4))
                die("%s: d_type %u, mode %x", path, d[6], *(u16 *)(st + 8));
            if (is_dir) {
                g_dirs++;
                stat_line("D", path, st, "-");
                walk(path);
            } else {
                g_files++;
                int ffd;
                if (F->open(path, 0, &ffd) != 0 || ffd < 0)
                    die("open %s -> %x", path, ffd);
                if (F->fstat(ffd, st2) != 0 || memcmp(st, st2, 0x78) != 0)
                    die("fstat of %s differs from stat", path);
                u64 size = *(u64 *)(st + 0x48);
                char crc_text[16] = "-";
                if (size <= g_hash_max) {
                    /* reads of varied sizes, so every path through the cache runs */
                    static const u32 k_chunks[] = {1, 4095, 65536, 1u << 20, 3, 200000, 8u << 20};
                    static u8 chunk[8u << 20];
                    u32 crc = 0, k = 0;
                    u64 got = 0;
                    for (;;) {
                        u32 want = k_chunks[k++ % (sizeof(k_chunks) / sizeof(k_chunks[0]))];
                        s64 r = F->read(ffd, chunk, want);
                        if (r < 0)
                            die("read %s at %lu -> %lx", path, got, (u64)r);
                        if (!r)
                            break;
                        crc = crc32_le(crc, chunk, (u64)r);
                        got += (u64)r;
                    }
                    if (got != size)
                        die("%s: read %lu of %lu bytes", path, got, size);
                    g_bytes += got;
                    fmt(crc_text, sizeof(crc_text), "%08x", crc);
                }
                F->close(ffd);
                stat_line("F", path, st, crc_text);
            }
            off += reclen;
        }
    }
    free(buf);
    F->close(fd);
}

/* ---- app0: the title's view ---- */
static int expect_n;
#define EXPECT(cond, ...)                                                                                     \
    do {                                                                                                      \
        expect_n++;                                                                                           \
        if (!(cond))                                                                                          \
            die("FAILED: " __VA_ARGS__);                                                                      \
    } while (0)

static void app0_checks(const char *root)
{
    int rc, fd, fd2;
    u8 st[0x78], st2[0x78], buf[4096];
    EXPECT(F->open("/app0/eboot.bin", 0, &fd) == 1, "/app0 before a root is set is not usbfs's");
    EXPECT(F->set_root("/no/such/folder") < 0, "a missing root is refused");
    EXPECT(F->set_root(root) == 0, "set_root %s", root);
    EXPECT(F->open("/app0/eboot.bin", 0, &fd) == 0 && fd >= 0, "open eboot.bin");
    EXPECT(F->owns(fd), "owns its fd");
    EXPECT(!F->owns(3), "does not own fd 3");
    EXPECT(F->fstat(fd, st) == 0 && (*(u16 *)(st + 8) & 0xF000) == 0x8000, "eboot is a file");
    u64 size = *(u64 *)(st + 0x48);
    EXPECT(*(s64 *)(st + 0x28) > 315532800, "mtime is set (%ld)", *(s64 *)(st + 0x28));
    EXPECT(F->stat("/app0//sce_sys/../eboot.bin", st2, &rc) == 0 && rc == 0 && !memcmp(st, st2, 0x78),
           "'//' and '..' resolve");
    EXPECT(F->stat("/app0/./EBOOT.BIN", st2, &rc) == 0 && rc == 0 && !memcmp(st, st2, 0x78),
           "names compare case-insensitively");
    EXPECT(F->stat("/usb0/../../ghostldr", st2, &rc) == 0 && rc == 0, "'..' stops at the volume root");
    EXPECT(F->stat("/app0/../../../../eboot.bin", st2, &rc) == 0 && rc == 0 && !memcmp(st, st2, 0x78),
           "'..' stops at /app0's root");
    EXPECT(F->stat("/app0/missing.bin", st2, &rc) == 0 && rc == US_ENOENT, "a missing file is ENOENT");
    EXPECT(F->stat("/app0/eboot.bin/x", st2, &rc) == 0 && rc == US_ENOTDIR, "a file is not a directory");
    EXPECT(F->stat("/app01/x", st2, &rc) == 1, "/app01 is not usbfs's");
    EXPECT(F->stat("/data/x", st2, &rc) == 1, "/data is not usbfs's");
    EXPECT(F->open("/app0/eboot.bin", 2, &fd2) == 0 && fd2 == US_EROFS, "writing is EROFS");
    EXPECT(F->open("/app0/eboot.bin", 0x20000, &fd2) == 0 && fd2 == US_ENOTDIR, "O_DIRECTORY on a file");
    EXPECT(F->pread(fd, buf, 16, -1) == US_EINVAL, "a negative offset is EINVAL");
    EXPECT(F->pread(fd, buf, 16, (s64)size) == 0, "a read at the end gives 0");
    EXPECT(F->pread(fd, buf, 16, (s64)size + 100) == 0, "a read past the end gives 0");
    EXPECT(F->lseek(fd, 0, 2) == (s64)size, "SEEK_END is the size");
    EXPECT(F->read(fd, buf, 16) == 0, "read at the end gives 0");
    EXPECT(F->lseek(fd, -1, 0) == US_EINVAL, "seeking before 0 is EINVAL");
    EXPECT(F->lseek(fd, 4, 0) == 4 && F->lseek(fd, 4, 1) == 8, "SEEK_SET and SEEK_CUR");
    s64 r = F->read(fd, buf, 8);
    EXPECT(r == (s64)US_MIN(8, size > 8 ? size - 8 : 0), "read after seeking (%ld)", r);
    EXPECT(F->lseek(fd, 0, 1) == 8 + r, "read moves the position");
    EXPECT(F->close(fd) == 0 && !F->owns(fd), "close");
    EXPECT(F->close(fd) == 1, "a closed fd is not usbfs's");
    EXPECT(F->open("/app0/sce_module", 0, &fd) == 0 && fd >= 0, "open a directory");
    EXPECT(F->pread(fd, buf, 16, 0) == US_EISDIR, "reading a directory is EISDIR");
    EXPECT(F->getdents(fd, buf, 8) == US_EINVAL, "a buffer too small for one entry is EINVAL");
    /* the listing in small buffers equals one big call */
    char names[4096] = "", names2[4096] = "";
    int total = 0;
    for (;;) {
        int n = F->getdents(fd, buf, 40);
        EXPECT(n >= 0, "getdents in small steps");
        if (!n)
            break;
        for (int off = 0; off < n; off += *(u16 *)(buf + off + 4))
            strlcat(names, (char *)buf + off + 8, sizeof(names)), strlcat(names, "\n", sizeof(names)), total++;
    }
    EXPECT(F->lseek(fd, 0, 0) == 0, "rewind a directory");
    int n = F->getdents(fd, buf, sizeof(buf));
    for (int off = 0; off < n; off += *(u16 *)(buf + off + 4))
        strlcat(names2, (char *)buf + off + 8, sizeof(names2)), strlcat(names2, "\n", sizeof(names2));
    EXPECT(strcmp(names, names2) == 0, "listing in steps == listing at once");
    EXPECT(F->getdents(fd, buf, sizeof(buf)) == 0, "the end stays the end");
    F->close(fd);
    say("sce_module\t%d entries", total);
    write(1, names, strlen(names));
    EXPECT(F->open("/usb0", 0, &fd) == 0 && fd >= 0, "the volume root opens");
    F->close(fd);
    say("app0 checks: %d passed", expect_n);
}

static u64 num(const char *s) { return strtoull(s, NULL, 0); }

int main(int argc, char **argv)
{
    const char *faults = NULL;
    int part = -1, use_bot = 0, use_map = 0;
    u32 line = 16 << 10, meta = 64, data = 192, ra = 32, xfer = 128 << 10; /* the console's (usb_sce.c) */
    int a = 1;
    for (; a < argc && argv[a][0] == '-' && argv[a][1] == '-'; a++) {
        const char *o = argv[a];
        if (!strcmp(o, "--mmap")) {
            use_map = 1;
            continue;
        }
        if (!strcmp(o, "--quiet")) {
            g_quiet = 1;
            continue;
        }
        if (a + 1 >= argc)
            die("%s needs a value", o);
        const char *v = argv[++a];
        if (!strcmp(o, "--bot"))
            use_bot = 1, faults = v;
        else if (!strcmp(o, "--block"))
            g_block = (u32)num(v);
        else if (!strcmp(o, "--part"))
            part = (int)strtol(v, NULL, 0);
        else if (!strcmp(o, "--line"))
            line = (u32)num(v);
        else if (!strcmp(o, "--meta"))
            meta = (u32)num(v);
        else if (!strcmp(o, "--data"))
            data = (u32)num(v);
        else if (!strcmp(o, "--ra"))
            ra = (u32)num(v);
        else if (!strcmp(o, "--xfer"))
            xfer = (u32)num(v);
        else if (!strcmp(o, "--usb-model")) {
            char *end;
            g_model_cmd_ns = (u64)strtol(v, &end, 10) * 1000;
            g_model_bps = *end == ',' ? (u64)strtol(end + 1, NULL, 10) * 1000000 : 0;
            if (!g_model_bps)
                die("--usb-model US,MBPS");
            use_bot = 1;
            faults = faults ? faults : "none";
        }
        else
            die("unknown option %s", o);
    }
    if (a + 1 >= argc)
        die("usage: usb_host [options] IMAGE walk|read|bench|app0|parts ...");
    const char *image = argv[a++], *cmd = argv[a++];
    g_img = open(image, 0);
    if (g_img < 0)
        die("cannot open %s", image);
    g_img_size = (u64)lseek(g_img, 0, 2);
    g_img_size -= g_img_size % g_block;
    if (use_map && !(g_map = mmap(NULL, g_img_size, PROT_READ, MAP_PRIVATE, g_img, 0)))
        die("mmap");
    if (g_map == (void *)-1)
        g_map = NULL;

    struct blkdev dev;
    struct bot bot;
    if (use_bot) {
        parse_faults(faults);
        D.ua_pending = g_every[EV_UA] ? 1 : 0;
        bot_init(&bot, g_model_bps ? &k_model : &k_dev, 0, 0x81, 0x02);
        bot.max_xfer = xfer;
        if (bot_attach(&bot, 0) != 0)
            die("bot_attach failed");
        bot_blkdev(&bot, &dev);
    } else {
        dev = (struct blkdev){NULL, g_block, g_img_size / g_block, (1u << 20) / g_block, raw_read};
    }
    struct blk *b = calloc(1, sizeof(*b));
    if (blk_init(b, &dev, line, meta, data, ra) != 0)
        die("blk_init");
    if (!strcmp(cmd, "parts")) {
        struct part p[PART_MAX];
        int n = part_scan(b, p, PART_MAX);
        for (int i = 0; i < n; i++)
            say("part\t%d\t%lu\t%lu\t%x\t%s\t%u\t%u", i, p[i].start, p[i].sectors, p[i].mbr_type,
                p[i].whole ? "whole" : p[i].gpt ? "gpt" : "mbr", p[i].basic_data, p[i].esp);
    }
    if (usbfs_mount(b, part) != 0)
        die("usbfs_mount failed");
    struct part mp;
    int mi = usbfs_part(&mp);
    say("mounted\t%d\t%lu\t%lu", mi, mp.start, mp.start * dev.sector);
    F = usbfs_ops();
    u64 t0 = now_ns();
    if (!strcmp(cmd, "walk")) {
        for (; a + 1 < argc; a += 2) {
            if (!strcmp(argv[a], "--hash-max"))
                g_hash_max = num(argv[a + 1]);
            else if (!strcmp(argv[a], "--dents"))
                g_dents = (int)num(argv[a + 1]);
        }
        walk("/usb0");
        say("walked %lu files, %lu directories, %lu bytes", g_files, g_dirs, g_bytes);
    } else if (!strcmp(cmd, "read") && a + 2 < argc) {
        const char *path = argv[a];
        u64 off = num(argv[a + 1]), len = num(argv[a + 2]);
        int fd;
        if (F->open(path, 0, &fd) != 0 || fd < 0)
            die("open %s -> %x", path, fd);
        u8 *buf = calloc(1, len ? len : 1);
        s64 r = F->pread(fd, buf, len, (s64)off);
        if (r < 0)
            die("pread -> %lx", (u64)r);
        char head[40] = "", tail[40] = "";
        for (int i = 0; i < 16 && i < r; i++)
            fmt(head + 2 * i, 3, "%02x", buf[i]);
        for (int i = 0; i < 16 && i < r; i++)
            fmt(tail + 2 * i, 3, "%02x", buf[r - (r < 16 ? r : 16) + i]);
        say("read\t%ld\t%08x\t%s\t%s", r, crc32_le(0, buf, (u64)r), head, tail);
        F->close(fd);
    } else if (!strcmp(cmd, "bench") && a + 1 < argc) {
        const char *path = argv[a];
        u64 chunk = num(argv[a + 1]), randoms = a + 2 < argc ? num(argv[a + 2]) : 0;
        int fd;
        if (F->open(path, 0, &fd) != 0 || fd < 0)
            die("open %s -> %x", path, fd);
        u8 *buf = calloc(1, chunk);
        u64 got = 0, t = now_ns(), v0 = g_vtime;
        u32 c0 = D.commands;
        s64 r;
        while ((r = F->read(fd, buf, chunk)) > 0)
            got += (u64)r;
        u64 ns = now_ns() - t;
        say("bench\tsequential\t%lu\t%lu\t%lu\t%lu\t%u", chunk, got, ns, g_vtime - v0, D.commands - c0);
        if (randoms) {
            u8 st[0x78];
            F->fstat(fd, st);
            u64 size = *(u64 *)(st + 0x48), x = 0x9E3779B97F4A7C15ull, t2 = now_ns(), bytes = 0, v1 = g_vtime;
            u32 c1 = D.commands;
            for (u64 i = 0; i < randoms; i++) {
                x ^= x << 13, x ^= x >> 7, x ^= x << 17;
                u64 off = size > chunk ? x % (size - chunk) : 0;
                r = F->pread(fd, buf, chunk, (s64)off);
                if (r < 0)
                    die("random pread -> %lx", (u64)r);
                bytes += (u64)r;
            }
            say("bench\trandom\t%lu\t%lu\t%lu\t%lu\t%u", chunk, bytes, now_ns() - t2, g_vtime - v1, D.commands - c1);
        }
        F->close(fd);
    } else if (!strcmp(cmd, "app0") && a < argc) {
        app0_checks(argv[a]);
    } else if (strcmp(cmd, "parts") != 0) {
        die("unknown command %s", cmd);
    }
    u64 ns = now_ns() - t0;
    char line_buf[1024];
    usbfs_stats(line_buf, sizeof(line_buf));
    say("stats\t%s", line_buf);
    say("time\t%lu", ns);
    if (use_bot) {
        say("bot\tcommands %u stalls %u resets %u retries %u short %u phase %u bad_csw %u", bot.commands, bot.stalls,
            bot.resets, bot.retries, bot.short_reads, bot.phase_errors, bot.bad_csw);
        for (int i = 0; i < EV_COUNT; i++)
            if (g_every[i])
                say("fault\t%s\t%u", k_ev[i], g_fired[i]);
        say("device\tcommands %u resets %u clears %u", D.commands, D.resets, D.clears);
    }
    say("fds\t%d", g_open_fds);
    usbfs_unmount();
    blk_free(b);
    say("memory\tpeak %lu live %lu", g_live_peak, g_live);
    return 0;
}
