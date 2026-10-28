/*
 * Internet sockets for the title on a PS5 at 8.00 or later (a PS4 at 14.00
 * or later), where SWRR's own socket() answers ENOSYS for them.
 *
 * As for the FTP server (ftp_sys.c), the emulator's JIT compiler process
 * makes such sockets and hands them to SWRR (jit_socket), but only the boot
 * thread may use the compiler's channel.  So just before the title starts
 * (ext_exports.title_starting, on the boot thread) a pool is made: NTCP
 * stream and NUDP datagram sockets.  When SWRR's socket() or sceNetSocket()
 * answers ENOSYS for an AF_INET socket, the title gets one from the pool, as
 * if it had just been made (each is used once: a closed socket isn't
 * returned to the pool).  A title that runs out gets ENOSYS, as it would
 * have.  Where SWRR's own socket() works, there is no pool and nothing
 * changes.
 */
#include "kmod.h"

int jit_socket(int domain, int type, int proto) HIDDEN; /* ftp_sys.c */
int net_setup(void) HIDDEN;                              /* ftp_sys.c: the socket calls jit_socket uses */

#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define ENOSYS_ 78
#define NTCP 24
#define NUDP 8

static int g_tcp[NTCP], g_udp[NUDP];
static u32 g_ntcp, g_nudp;
static volatile int g_lock;
static bool g_pooled;
static u64 g_given[16]; /* fds below 1024 handed to the title from the pool */

static bool given(s64 fd) { return fd >= 0 && fd < 1024 && (g_given[fd >> 6] >> (fd & 63) & 1); }

typedef s64 (*fn6)(u64, u64, u64, u64, u64, u64);

void net_pool_fill(void) HIDDEN;
void net_pool_fill(void)
{
    int (*sock)(int, int, int) = host_real("socket");
    int (*close_)(int) = host_real("close");
    if (!sock || !close_ || !g_ctx->jit_syscall || net_setup() != 0)
        return;
    int fd = sock(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) { /* SWRR makes its own: nothing to do */
        close_(fd);
        return;
    }
    if (*K.errno_fn() != ENOSYS_)
        return;
    for (u32 i = 0; i < NTCP; i++)
        if ((fd = jit_socket(AF_INET, SOCK_STREAM, 0)) >= 0)
            g_tcp[g_ntcp++] = fd;
    for (u32 i = 0; i < NUDP; i++)
        if ((fd = jit_socket(AF_INET, SOCK_DGRAM, 0)) >= 0)
            g_udp[g_nudp++] = fd;
    g_pooled = true;
    logf("net: SWRR makes no internet sockets here: %u stream and %u datagram sockets from the JIT compiler wait "
         "for the title's socket() calls",
         g_ntcp, g_nudp);
}

/* a pooled socket of type, or -1 */
static int take(int domain, int type)
{
    if (!g_pooled || domain != AF_INET)
        return -1;
    int fd = -1;
    spin_lock(&g_lock);
    if ((type & 0xF) == SOCK_STREAM && g_ntcp)
        fd = g_tcp[--g_ntcp];
    else if ((type & 0xF) == SOCK_DGRAM && g_nudp)
        fd = g_udp[--g_nudp];
    if (fd >= 0 && fd < 1024)
        g_given[fd >> 6] |= 1ull << (fd & 63);
    u32 left_tcp = g_ntcp, left_udp = g_nudp;
    spin_unlock(&g_lock);
    static u32 said;
    if (fd >= 0 && said++ < 8)
        logf("net: the title's socket(%d, %d) gets pooled fd %d (%u stream, %u datagram left)", domain, type, fd,
             left_tcp, left_udp);
    else if (fd < 0)
        logf("net: the title's socket(%d, %d): none left in the pool: ENOSYS", domain, type);
    return fd;
}

static int net_socket(int domain, int type, int proto)
{
    static fn6 real;
    if (!real)
        real = (fn6)host_real("socket");
    int fd = real ? (int)real((u64)domain, (u64)type, (u64)proto, 0, 0, 0) : -1;
    if (fd < 0 && (!real || *K.errno_fn() == ENOSYS_)) {
        fd = take(domain, type);
        if (fd < 0)
            *K.errno_fn() = ENOSYS_;
    }
    return fd;
}

/* int sceNetSocket(const char *name, int family, int type, int protocol): SCE errors 0x804101xx */
static int net_sce_socket(const char *name, int family, int type, int proto)
{
    static fn6 real;
    if (!real)
        real = (fn6)host_real_in("libSceNet", "sceNetSocket");
    int fd = real ? (int)real((u64)name, (u64)family, (u64)type, (u64)proto, 0, 0) : (int)(0x80410100u | ENOSYS_);
    if (fd == (int)(0x80410100u | ENOSYS_)) {
        int p = take(family, type);
        if (p >= 0)
            fd = p;
    }
    return fd;
}

/* "a.b.c.d" -> its address as the network stores it (first octet first) */
static bool ipv4(const char *s, u32 *out)
{
    u32 v[4], k = 0;
    for (; k < 4; k++) {
        u32 n = 0, digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 4)
            n = n * 10 + (u32)(*s++ - '0'), digits++;
        if (!digits || n > 255 || (k < 3 && *s++ != '.'))
            return false;
        v[k] = n;
    }
    *out = v[0] | v[1] << 8 | v[2] << 16 | v[3] << 24;
    return true;
}

/* int sceNetGetDnsInfo(SceNetDnsInfo *info, int flags): the DNS servers
 * (up to two IPv4 addresses) and how many there are, as OpenOrbis's resolver
 * reads it (0-2; more is an error).  Where SWRR's answer has none, the
 * network's own settings (sceNetCtlGetInfo PRIMARY_DNS, SECONDARY_DNS) stand
 * in. */
static s32 net_get_dns_info(u32 *info, s32 flags)
{
    static fn6 real, ctl;
    static u32 said;
    if (!real)
        real = (fn6)host_real_in("libSceNet", "sceNetGetDnsInfo");
    if (!ctl)
        ctl = (fn6)host_real_in("libSceNetCtl", "sceNetCtlGetInfo");
    s32 rc = real ? (s32)real((u64)info, (u64)flags, 0, 0, 0, 0) : (s32)0x80410106;
    if (rc > 0 && rc <= 2 && info) {
        if (!said++)
            logf("net: sceNetGetDnsInfo -> %d: DNS %u.%u.%u.%u", rc, info[0] & 255, (info[0] >> 8) & 255,
                 (info[0] >> 16) & 255, info[0] >> 24);
        return rc;
    }
    if (!info || !ctl)
        return rc;
    char buf[0x200];
    u32 dns[2] = {0, 0};
    for (u32 k = 0; k < 2; k++) {
        memset(buf, 0, sizeof(buf));
        if ((s32)ctl(17 + k /* PRIMARY_DNS, SECONDARY_DNS */, (u64)buf, 0, 0, 0, 0) == 0)
            ipv4(buf, &dns[k]);
    }
    if (!said++)
        logf("net: sceNetGetDnsInfo -> %x with no server; the network settings say %u.%u.%u.%u", (u32)rc,
             dns[0] & 255, (dns[0] >> 8) & 255, (dns[0] >> 16) & 255, dns[0] >> 24);
    if (!dns[0])
        return rc;
    info[0] = dns[0], info[1] = dns[1];
    return dns[1] ? 2 : 1;
}

/* Calls on pooled sockets go to SWRR's own libkernel as they would; the
 * first failures (and the first datagrams) are logged, since the sockets
 * come from another process. */
static u32 g_said_io;

static void io_log(const char *what, s64 fd, s64 rc, const void *sa)
{
    if (g_said_io >= 24)
        return;
    g_said_io++;
    const u8 *a = sa;
    if (a && a[1] == AF_INET)
        logf("net: %s(fd %ld, %u.%u.%u.%u:%u) -> %ld (errno %d)", what, fd, a[4], a[5], a[6], a[7],
             (u32)a[2] << 8 | a[3], rc, rc < 0 ? *K.errno_fn() : 0);
    else
        logf("net: %s(fd %ld) -> %ld (errno %d)", what, fd, rc, rc < 0 ? *K.errno_fn() : 0);
}

static s64 net_bind(s64 fd, const void *sa, u32 len)
{
    static fn6 real;
    if (!real)
        real = (fn6)host_real("bind");
    s64 rc = real ? (s32)real((u64)fd, (u64)sa, len, 0, 0, 0) : -1;
    if (given(fd) && rc < 0)
        io_log("bind", fd, rc, sa);
    return rc;
}

static s64 net_connect(s64 fd, const void *sa, u32 len)
{
    static fn6 real;
    if (!real)
        real = (fn6)host_real("connect");
    s64 rc = real ? (s32)real((u64)fd, (u64)sa, len, 0, 0, 0) : -1;
    if (given(fd))
        io_log("connect", fd, rc, sa);
    return rc;
}

static s64 net_sendto(s64 fd, const void *buf, u64 n, s32 flags, const void *sa, u32 len)
{
    static fn6 real;
    if (!real)
        real = (fn6)host_real("sendto");
    s64 rc = real ? real((u64)fd, (u64)buf, n, (u64)(u32)flags, (u64)sa, len) : -1;
    if (given(fd) && (rc < 0 || sa))
        io_log("sendto", fd, rc, sa);
    return rc;
}

static s64 net_recvfrom(s64 fd, void *buf, u64 n, s32 flags, void *sa, u32 *len)
{
    static fn6 real;
    if (!real)
        real = (fn6)host_real("recvfrom");
    s64 rc = real ? real((u64)fd, (u64)buf, n, (u64)(u32)flags, (u64)sa, (u64)len) : -1;
    if (given(fd) && (rc < 0 || sa))
        io_log("recvfrom", fd, rc, sa);
    return rc;
}

static s32 net_poll(void *fds, u32 nfds, s32 timeout)
{
    static fn6 real;
    if (!real)
        real = (fn6)host_real("poll");
    s32 rc = real ? (s32)real((u64)fds, nfds, (u64)(s64)timeout, 0, 0, 0) : -1;
    struct pfd {
        s32 fd;
        s16 events, revents;
    } *p = fds;
    if (p && nfds && given(p[0].fd) && rc <= 0 && g_said_io < 24) {
        g_said_io++;
        logf("net: poll(fd %d events %x, %u fds, %d ms) -> %d (revents %x, errno %d)", p[0].fd, p[0].events, nfds,
             timeout, rc, p[0].revents, rc < 0 ? *K.errno_fn() : 0);
    }
    return rc;
}

static const struct ext_hle_ent k_hle[] = {
    {"libSceNet", "sceNetGetDnsInfo", net_get_dns_info, NULL},
    {"libkernel", "socket", net_socket, NULL},
    {"libSceNet", "sceNetSocket", net_sce_socket, NULL},
    {"libkernel", "bind", net_bind, NULL},
    {"libkernel", "connect", net_connect, NULL},
    {"libkernel", "sendto", net_sendto, NULL},
    {"libkernel", "recvfrom", net_recvfrom, NULL},
    {"libkernel", "poll", net_poll, NULL},
};

EXT_FAMILY("net", k_hle, NULL);
