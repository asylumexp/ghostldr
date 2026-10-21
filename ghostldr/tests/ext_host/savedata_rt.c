/* The few rt.c functions the save data harness needs that the host's C
 * library lacks, so it builds for the host's own architecture (rt.c's
 * memcpy/memset are x86-64 assembly).  memcpy, strcmp and the rest come from
 * the C library. */
#include <stdarg.h>
#include <stdio.h>

int vfmt(char *buf, size_t size, const char *f, va_list ap) { return vsnprintf(buf, size, f, ap); }

int fmt(char *buf, size_t size, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(buf, size, f, ap);
    va_end(ap);
    return n;
}

size_t strlcpy(char *d, const char *s, size_t n)
{
    size_t len = 0;
    while (s[len])
        len++;
    if (n) {
        size_t c = len < n - 1 ? len : n - 1;
        for (size_t i = 0; i < c; i++)
            d[i] = s[i];
        d[c] = 0;
    }
    return len;
}

size_t strlcat(char *d, const char *s, size_t n)
{
    size_t dl = 0;
    while (dl < n && d[dl])
        dl++;
    return dl + strlcpy(d + dl, s, n - dl);
}

const char *strrchr_c(const char *s, int c)
{
    const char *r = NULL;
    for (; *s; s++)
        if (*s == c)
            r = s;
    return r;
}
