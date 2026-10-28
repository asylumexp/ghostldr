/*
 * The failure page (dlg_overlay.c ov_page) drawn on this machine into a
 * 1920x1080 buffer, written as a PPM to look at (test_diag.py).
 *
 *     page_host OUT.ppm HEAD BODY FOOT
 *
 * Prints how many pixels hold text (neither the background nor the panel).
 */
#include "hle_dialog.h"

long write(int fd, const void *buf, unsigned long n);
int open(const char *path, int flags, ...);
int close(int fd);
int printf(const char *fmt, ...);
void *calloc(unsigned long n, unsigned long size);
int vsnprintf(char *buf, unsigned long size, const char *fmt, __builtin_va_list ap);

const struct ghost_api *G;
#undef logf
#undef spin_lock
#undef spin_unlock
int vfmt(char *buf, size_t size, const char *f, __builtin_va_list ap) { return vsnprintf(buf, size, f, ap); }
int fmt(char *buf, size_t size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(buf, size, f, ap);
    __builtin_va_end(ap);
    return n;
}
/* what dlg_overlay.c links that this doesn't reach */
u64 dlg_now(void) { return 0; }
void dlg_user_answer(struct dlg *d, u32 answer) {}
void harness_family(const struct ext_family *f) {}
static void spin(volatile int *l) {}
static void hlog(const char *f, ...) {}

void ov_page(u32 *px, u32 w, u32 h, u32 pitch, const char *head, const char *body, const char *foot) HIDDEN;

int main(int argc, char **argv)
{
    static struct ghost_api api;
    api.spin_lock = spin, api.spin_unlock = spin, api.logf = hlog;
    G = &api;
    u32 *px = calloc(1920 * 1080, 4);
    ov_page(px, 1920, 1080, 1920, argv[2], argv[3], argc > 4 ? argv[4] : NULL);
    u32 text = 0;
    for (u32 i = 0; i < 1920 * 1080; i++)
        text += px[i] != 0xFF0E1116u && px[i] != 0xFF20262Eu && px[i] != 0xFF5A6A80u && px[i] != 0xFF4A90E2u;
    printf("text pixels %u\n", text);
    int fd = open(argv[1], 0x601, 0644);
    char hdr[32];
    int n = fmt(hdr, sizeof(hdr), "P6\n1920 1080\n255\n");
    write(fd, hdr, (unsigned long)n);
    u8 *rgb = calloc(1920 * 1080, 3);
    for (u32 i = 0; i < 1920 * 1080; i++)
        rgb[3 * i] = (u8)(px[i] >> 16), rgb[3 * i + 1] = (u8)(px[i] >> 8), rgb[3 * i + 2] = (u8)px[i];
    write(fd, rgb, 1920 * 1080 * 3);
    close(fd);
    return text > 1000 ? 0 : 1;
}
