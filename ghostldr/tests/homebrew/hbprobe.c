/*
 * hbprobe: a small PS4 homebrew that checks what ghostldr gives OpenOrbis
 * homebrew, and says so on stdout (ghostldr's log, "ghost1:" lines):
 *
 *   - stdout and stderr, line by line;
 *   - files under /app0: fopen/fread/fseek, stat, opendir/readdir, errors;
 *   - malloc far past SWRR's ~38 MiB of flexible memory, calloc, realloc;
 *   - threads (pthread_create) with thread-local variables;
 *   - direct memory, VideoOut (two 1920x1080 framebuffers) and the pad:
 *     each pad button lights its own box, the bar at the bottom counts
 *     frames, the top band is green when every check passed (red if not).
 *     Holding OPTIONS for two seconds calls exit(3).
 *
 * Built with the OpenOrbis PS4 toolchain (Makefile here).  Built with
 * SYSMALLOC=1 its malloc family comes from the system's libSceLibcInternal
 * instead of musl's, as in homebrew without a libc of its own.
 *
 * MIT license, like the rest of ghostldr.  Only standard C headers: the
 * PS4 functions are declared below.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int sceKernelAllocateDirectMemory(long start, long end, size_t len, size_t align, int type, long *out);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, long off, size_t align);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelUsleep(unsigned int usec);
int sceKernelCreateEqueue(void **eq, const char *name);
int sceKernelWaitEqueue(void *eq, void *ev, int num, int *out, unsigned int *usec);
int sceVideoOutOpen(int user, int bus, int index, const void *param);
void sceVideoOutSetBufferAttribute(void *attr, unsigned fmt, unsigned tiling, unsigned aspect, unsigned w,
                                   unsigned h, unsigned pitch);
int sceVideoOutRegisterBuffers(int handle, int start, void *const *addrs, int num, const void *attr);
int sceVideoOutSetFlipRate(int handle, int rate);
int sceVideoOutAddFlipEvent(void *eq, int handle, void *udata);
int sceVideoOutSubmitFlip(int handle, int index, unsigned mode, long arg);
int sceVideoOutGetFlipStatus(int handle, void *status);
int scePadInit(void);
int scePadOpen(int user, int type, int index, const void *param);
int scePadReadState(int handle, void *data);
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int *user);

#define W 1920
#define H 1080
#define DATA_DIR "/app0/hbprobe"
#define DATA_FILE DATA_DIR "/data.txt"
#define DATA_LINES 4000
#define DATA_SIZE 38890 /* "line 0\n" .. "line 3999\n" */

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    printf("hbprobe: %s %s\n", ok ? "PASS" : "FAIL", what);
    if (ok)
        g_pass++;
    else
        g_fail++;
}

/* ---- files: data.txt holds "line N\n" for N = 0..DATA_LINES-1 ---- */
static const char want_bytes[12345] = "hbprobe";

static void test_files(void)
{
    FILE *f = fopen(DATA_FILE, "r");
    check(f != NULL, "fopen " DATA_FILE);
    if (f) {
        char line[64], want[64];
        int n = 0, ok = 1;
        while (fgets(line, sizeof(line), f)) {
            snprintf(want, sizeof(want), "line %d\n", n);
            ok &= strcmp(line, want) == 0;
            n++;
        }
        check(ok && n == DATA_LINES, "fgets reads every line of data.txt");
        fseek(f, 0, SEEK_END);
        long end = ftell(f);
        fseek(f, -5, SEEK_END);
        char tail[8] = {0};
        size_t got = fread(tail, 1, 5, f);
        check(end == DATA_SIZE && got == 5 && strcmp(tail, "3999\n") == 0, "fseek/ftell/fread at the end of data.txt");
        fclose(f);
    }
    /* OpenOrbis's musl declares struct stat with 32-bit mode_t and nlink_t, so
     * its fields from st_atim on sit 8 bytes past the kernel's (FreeBSD's,
     * what sceKernelStat and ghostfs fill): st_size reads the kernel's
     * st_blocks.  Only the type bits are checked; the sizes are shown. */
    struct stat st;
    check(stat(DATA_FILE, &st) == 0 && S_ISREG(st.st_mode), "stat data.txt is a file");
    printf("hbprobe: note: stat(data.txt).st_size %lld (the file has %d bytes)\n", (long long)st.st_size, DATA_SIZE);
    check(stat(DATA_DIR, &st) == 0 && S_ISDIR(st.st_mode), "stat " DATA_DIR " is a directory");
    DIR *d = opendir(DATA_DIR);
    int seen = 0, files = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            files++;
            seen |= strcmp(e->d_name, "data.txt") == 0;
        }
        closedir(d);
    }
    check(d && seen, "opendir/readdir finds data.txt");
    errno = 0;
    check(fopen(DATA_DIR "/missing.txt", "r") == NULL && errno == ENOENT, "a missing file fails with ENOENT");
    int fd = open(DATA_FILE, O_RDONLY);
    char b[6] = {0};
    check(fd >= 0 && pread(fd, b, 5, 7) == 5 && strcmp(b, "line ") == 0, "open/pread");
    check(fd >= 0 && lseek(fd, 0, SEEK_END) == DATA_SIZE, "lseek to the end");
    if (fd >= 0)
        close(fd);
    printf("hbprobe: %d entries in " DATA_DIR "\n", files);
    /* a file of SWRR's kernel (the title's /temp0): written, stat'ed, removed */
    FILE *t = fopen("/temp0/hbprobe.txt", "w");
    int wrote = t && fwrite(want_bytes, 1, sizeof(want_bytes), t) == sizeof(want_bytes);
    if (t)
        fclose(t);
    check(wrote, "fopen/fwrite in /temp0");
    if (wrote && stat("/temp0/hbprobe.txt", &st) == 0)
        printf("hbprobe: note: stat of a %d-byte /temp0 file: st_size %lld, st_blocks %lld\n", (int)sizeof(want_bytes),
               (long long)st.st_size, (long long)st.st_blocks);
    unlink("/temp0/hbprobe.txt");
}

/* ---- memory: well past what SWRR's own heap could give ---- */
#define CHUNKS 48
#define CHUNK (4u << 20)

static void test_memory(void)
{
    static unsigned char *p[CHUNKS];
    int got = 0, ok = 1;
    for (int i = 0; i < CHUNKS; i++) {
        p[i] = malloc(CHUNK);
        if (!p[i])
            break;
        memset(p[i], i + 1, CHUNK);
        got++;
    }
    for (int i = 0; i < got; i++)
        ok &= p[i][0] == (unsigned char)(i + 1) && p[i][CHUNK - 1] == (unsigned char)(i + 1);
    printf("hbprobe: malloc'd %d MiB in %d blocks\n", got * (CHUNK >> 20), got);
    check(got == CHUNKS && ok, "malloc 192 MiB in 4 MiB blocks, all writable");
    for (int i = 0; i < got; i++)
        free(p[i]);
    void *small[1000];
    int sok = 1;
    for (int i = 0; i < 1000; i++) {
        small[i] = malloc(16 + i * 24);
        sok &= small[i] && ((uintptr_t)small[i] & 15) == 0;
        if (small[i])
            memset(small[i], 0x5A, 16 + i * 24);
    }
    for (int i = 0; i < 1000; i += 2)
        free(small[i]);
    for (int i = 1; i < 1000; i += 2)
        sok &= ((unsigned char *)small[i])[15 + i * 24] == 0x5A, free(small[i]);
    check(sok, "1000 small blocks, 16-byte aligned");
    unsigned char *z = calloc(1 << 20, 3);
    int zero = z != NULL;
    for (size_t i = 0; z && i < (3u << 20); i += 4093)
        zero &= z[i] == 0;
    free(z);
    check(zero, "calloc 3 MiB is zero");
    char *r = malloc(100);
    strcpy(r, "realloc keeps the start");
    r = realloc(r, 64u << 20);
    check(r && strcmp(r, "realloc keeps the start") == 0, "realloc 100 bytes -> 64 MiB keeps the data");
    free(r);
    void *a = NULL;
    check(posix_memalign(&a, 4096, 10000) == 0 && ((uintptr_t)a & 4095) == 0, "posix_memalign 4096");
    free(a);
    void *big = malloc(160u << 20);
    if (big)
        memset(big, 1, 160u << 20);
    check(big != NULL, "one 160 MiB block");
    free(big);
}

/* ---- threads and thread-local variables ----
 * Zero-initialized ones only: create-fself gives the TLS segment an
 * alignment of 32 whatever lld laid it out with, so a loader that follows
 * the segment (as a PS4's does) can place initial values a few bytes off.
 * t_init shows what happens to one. */
static _Thread_local int t_counter;
static _Thread_local char t_name[16];
static _Thread_local int t_init = 7;

static void *worker(void *arg)
{
    intptr_t id = (intptr_t)arg;
    snprintf(t_name, sizeof(t_name), "worker %d", (int)id);
    for (int i = 0; i < 1000; i++)
        t_counter++;
    sceKernelUsleep(1000 * (int)id);
    char want[16];
    snprintf(want, sizeof(want), "worker %d", (int)id);
    if (id == 0)
        printf("hbprobe: note: an initialized thread-local variable starts at %d in a new thread (7 in the "
               "source)\n", t_init);
    return (void *)(intptr_t)(t_counter == 1000 && strcmp(t_name, want) == 0);
}

/* thread-specific data, as libc++abi keeps its exception globals (the POSIX
 * functions; inside SWRR, SWRR's own fault on a homebrew thread, and
 * ghostldr answers these) */
static pthread_key_t g_key;
static void tsd_make_key(void) { pthread_key_create(&g_key, NULL); }

static int tsd_check(void)
{
    static int a;
    int b;
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, tsd_make_key);
    return pthread_getspecific(g_key) == NULL && pthread_setspecific(g_key, &b) == 0 &&
           pthread_getspecific(g_key) == &b && pthread_setspecific(g_key, &a) == 0 && pthread_getspecific(g_key) == &a;
}

static void *tsd_worker(void *arg) { return (void *)(intptr_t)tsd_check(); }

static void test_threads(void)
{
    check(tsd_check(), "thread-specific data on the main thread");
    pthread_t tt;
    void *tret = NULL;
    check(pthread_create(&tt, NULL, tsd_worker, NULL) == 0 && pthread_join(tt, &tret) == 0 && tret == (void *)1,
          "thread-specific data on a new thread");
    pthread_t th[4];
    int created = 0, ok = 1;
    t_counter = 42;
    for (intptr_t i = 0; i < 4; i++)
        created += pthread_create(&th[i], NULL, worker, (void *)i) == 0;
    for (int i = 0; i < created; i++) {
        void *ret = NULL;
        pthread_join(th[i], &ret);
        ok &= ret == (void *)1;
    }
    check(created == 4, "pthread_create x4");
    check(ok, "each thread has its own thread-local variables");
    check(t_counter == 42 && t_name[0] == 0, "the main thread's thread-local variables are untouched");
}

/* ---- video and pad ---- */
static uint32_t *g_fb[2];

static void fill(uint32_t *fb, int x, int y, int w, int h, uint32_t c)
{
    for (int j = y; j < y + h && j < H; j++)
        for (int i = x; i < x + w && i < W; i++)
            fb[j * W + i] = c;
}

static int video_open(void **eq)
{
    int v = sceVideoOutOpen(0xFF, 0, 0, NULL);
    printf("hbprobe: sceVideoOutOpen -> %x\n", v);
    if (v < 0)
        return v;
    long off = 0;
    void *mem = NULL;
    size_t size = 32u << 20;
    int rc = sceKernelAllocateDirectMemory(0, (long)sceKernelGetDirectMemorySize(), size, 2u << 20, 3, &off);
    if (rc == 0)
        rc = sceKernelMapDirectMemory(&mem, size, 0x33, 0, off, 2u << 20);
    check(rc == 0, "direct memory for the framebuffers");
    if (rc)
        return -1;
    g_fb[0] = mem, g_fb[1] = (uint32_t *)((char *)mem + W * H * 4);
    unsigned char attr[0x40] = {0};
    sceVideoOutSetBufferAttribute(attr, 0x80000000u, 1, 0, W, H, W);
    rc = sceVideoOutRegisterBuffers(v, 0, (void *const *)g_fb, 2, attr);
    check(rc == 0, "sceVideoOutRegisterBuffers");
    sceVideoOutSetFlipRate(v, 0);
    sceKernelCreateEqueue(eq, "hbprobe flip");
    sceVideoOutAddFlipEvent(*eq, v, NULL);
    return v;
}

static void wait_flip(int v, void *eq, long frame)
{
    unsigned char st[0x40], ev[0x20];
    for (int tries = 0; tries < 8; tries++) {
        sceVideoOutGetFlipStatus(v, st);
        if (*(long *)(st + 0x18) == frame)
            return;
        int n = 0;
        unsigned int usec = 100000;
        sceKernelWaitEqueue(eq, ev, 1, &n, &usec);
    }
}

int main(int argc, char **argv)
{
    printf("hbprobe: start, argc %d argv[0] %s\n", argc, argc > 0 && argv[0] ? argv[0] : "-");
    fprintf(stderr, "hbprobe: this line is stderr\n");
    test_files();
    test_memory();
    test_threads();
    void *eq = NULL;
    int v = video_open(&eq);
    scePadInit();
    sceUserServiceInitialize(NULL);
    int user = -1;
    sceUserServiceGetInitialUser(&user);
    int pad = scePadOpen(user, 0, 0, NULL);
    check(pad >= 0, "scePadOpen");
    printf("hbprobe: SUMMARY %d passed, %d failed\n", g_pass, g_fail);
    fflush(stdout);
    if (v < 0) {
        for (;;)
            sceKernelUsleep(1000000);
    }
    uint32_t last = 0xFFFFFFFFu;
    long frame = 0;
    int options_frames = 0;
    for (;; frame++) {
        unsigned char pd[0x80] = {0};
        uint32_t buttons = 0;
        if (pad >= 0 && scePadReadState(pad, pd) == 0)
            buttons = *(uint32_t *)pd;
        if (buttons != last) {
            printf("hbprobe: pad buttons %08x sticks %u,%u %u,%u\n", buttons, pd[4], pd[5], pd[6], pd[7]);
            last = buttons;
        }
        options_frames = (buttons & 0x8) ? options_frames + 1 : 0;
        if (options_frames == 120) {
            printf("hbprobe: OPTIONS held: exit(3)\n");
            exit(3);
        }
        uint32_t *fb = g_fb[frame & 1];
        fill(fb, 0, 0, W, H, 0xFF101828u);
        fill(fb, 0, 0, W, 120, g_fail ? 0xFFC02020u : 0xFF20A040u);
        for (int b = 0; b < 16; b++)
            fill(fb, 160 + (b % 8) * 200, 300 + (b / 8) * 220, 160, 160,
                 (buttons >> b) & 1 ? 0xFFFFD040u : 0xFF404858u);
        fill(fb, 160 + pd[4] * 3, 800, 24, 24, 0xFF40C0FFu);  /* left stick x */
        fill(fb, 160 + pd[6] * 3, 860, 24, 24, 0xFFFF80C0u);  /* right stick x */
        fill(fb, 0, H - 60, (int)(frame % 600) * W / 600, 60, 0xFFE0E0E0u);
        sceVideoOutSubmitFlip(v, (int)(frame & 1), 1, frame);
        wait_flip(v, eq, frame);
        if (frame == 120)
            printf("hbprobe: 120 frames flipped\n");
    }
}
