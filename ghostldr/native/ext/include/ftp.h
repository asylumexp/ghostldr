/*
 * ghostldr's FTP server (docs/ftp.md): the console's files, a USB drive's
 * above all, over FTP while a title runs.
 *
 *   ftp.c       the protocol: sessions, paths, listings, transfers.  Plain
 *               freestanding C over the environment below.
 *   ftp_sys.c   the console side: libkernel's sockets, files and threads,
 *               the USB drive's mount (approot.c), started as the title's
 *               main thread is about to run.
 *
 * The host tests (ghostldr/tests/test_ftp.py) build ftp.c natively against
 * ext_native/ftp_host.c and drive it with a real FTP client.  This header
 * uses plain C types only: the harness includes it beside the system's
 * headers, ftp.c beside ghost.h.
 */
#pragma once

/* ---- environment (ftp_sys.c on the console, the harness on the host) ----
 * Calls that can fail return a negative errno (FreeBSD's numbers). */
void *fv_alloc(unsigned long size); /* zeroed; NULL when there is none */
void fv_free(void *p, unsigned long size);
void fv_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fv_thread(void *(*fn)(void *), void *arg, const char *name); /* a detached thread: 0, or < 0 */
void fv_sleep_ms(unsigned ms);
long fv_now(void);      /* seconds since 1970, UTC */
long fv_clock_ms(void); /* a monotonic clock in milliseconds */

/* files: paths as the FTP client names them, absolute and normalized */
struct fv_stat {
    unsigned mode; /* S_IFDIR 0x4000 or S_IFREG 0x8000, and the permission bits */
    long size;
    long mtime; /* seconds since 1970, UTC */
};
int fv_open(const char *path, int flags, int mode); /* flags: O_* below */
long fv_read(int fd, void *buf, unsigned long n);
long fv_write(int fd, const void *buf, unsigned long n);
long fv_seek(int fd, long off, int whence);
int fv_close(int fd);
int fv_stat(const char *path, struct fv_stat *st);
int fv_mkdir(const char *path);
int fv_rmdir(const char *path);
int fv_unlink(const char *path);
int fv_rename(const char *from, const char *to);

/* a directory's names, one at a time; "." and ".." are left out */
struct fv_dir {
    long h;
    unsigned pos, len;
    int virt; /* the environment's own: names that aren't in the directory itself */
    unsigned char buf[4096];
};
int fv_opendir(const char *path, struct fv_dir *d);
int fv_readdir(struct fv_dir *d, char *name, unsigned size); /* 1: a name, 0: the end, or < 0 */
void fv_closedir(struct fv_dir *d);

/* TCP over IPv4; addresses and ports in host order, address 0 is any.
 * fv_listen is called by ftp_start, and by ftp_relisten after rest mode
 * (on the console, new sockets come from the JIT compiler, while Luac0re's
 * Lua is idle); fv_connect may answer -FV_ENOSYS at any time. */
int fv_listen(unsigned addr, unsigned short port, int backlog);         /* a listening socket */
int fv_accept(int fd, int timeout_ms, unsigned *addr, unsigned short *port); /* -1 ms: no limit, 0: no wait */
int fv_connect(unsigned addr, unsigned short port);
int fv_sockname(int fd, unsigned *addr, unsigned short *port);
int fv_timeouts(int fd, int seconds); /* a receive or send that waits longer fails */
long fv_send(int fd, const void *buf, unsigned long n); /* all of it: n, or < 0 */
long fv_recv(int fd, void *buf, unsigned long n);       /* 0: the peer closed it */
void fv_sclose(int fd);
void fv_shutdown(int fd); /* both directions: wakes a thread waiting on it, without closing it */

/* the directory a client starts in */
const char *fv_home(void);
/* the listener's tick, about twice a second */
void fv_idle(void);

#define FV_O_RDONLY 0
#define FV_O_WRONLY 1
#define FV_O_APPEND 0x8
#define FV_O_CREAT 0x200
#define FV_O_TRUNC 0x400
#define FV_S_IFMT 0xF000
#define FV_S_IFDIR 0x4000
#define FV_S_IFREG 0x8000

#define FV_EPERM 1
#define FV_ENOENT 2
#define FV_EINTR 4
#define FV_EIO 5
#define FV_EBADF 9
#define FV_ENOMEM 12
#define FV_EACCES 13
#define FV_EBUSY 16
#define FV_EEXIST 17
#define FV_EXDEV 18
#define FV_ENOTDIR 20
#define FV_EISDIR 21
#define FV_EINVAL 22
#define FV_ENOSPC 28
#define FV_EROFS 30
#define FV_EAGAIN 35
#define FV_ETIMEDOUT 60
#define FV_ENAMETOOLONG 63
#define FV_ENOTEMPTY 66
#define FV_ENOSYS 78

/* ---- the server (ftp.c) ---- */
#define FTP_SESSIONS 10         /* clients at once: gfs_pack.py --usb's 8 connections and a browser */
#define FTP_BUF (128u << 10)    /* each session's transfer buffer */

/* Listen on port and serve clients from a thread of its own: 0, or < 0
 * (the port is taken, no memory).  Once per process. */
int ftp_start(unsigned short port);
/* Stop it and free all it made, waiting at most max_ms for its threads:
 * 0, or < 0 when one is still running.  ftp_start may then start it again. */
int ftp_stop(unsigned max_ms);
int ftp_running(void);
