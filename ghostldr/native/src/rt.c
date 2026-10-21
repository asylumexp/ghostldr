/* Minimal freestanding runtime: memory/string helpers and a small formatter. */
#include "ghost.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;
    if (d == s || n == 0)
        return dst;
    if (d < s || d >= s + n)
        return memcpy(dst, src, n);
    while (n--)
        d[n] = s[n];
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    u8 *d = dst;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return x[i] - y[i];
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (u8)*a - (u8)*b;
        if (!*a)
            return 0;
    }
    return 0;
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}

size_t strlcpy(char *d, const char *s, size_t n)
{
    size_t len = strlen(s);
    if (n) {
        size_t c = len < n - 1 ? len : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return len;
}

size_t strlcat(char *d, const char *s, size_t n)
{
    size_t dl = strlen(d);
    if (dl >= n)
        return dl + strlen(s);
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

struct out {
    char *buf;
    size_t size, len;
};

static void put(struct out *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void put_num(struct out *o, u64 v, int base, int is_signed, int width, char pad, int upper)
{
    char tmp[32];
    int n = 0, neg = 0;
    if (is_signed && (s64)v < 0) {
        neg = 1;
        v = (u64)(-(s64)v);
    }
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    if (neg)
        tmp[n++] = '-';
    while (n < width)
        tmp[n++] = pad;
    while (n)
        put(o, tmp[--n]);
}

int vfmt(char *buf, size_t size, const char *f, __builtin_va_list ap)
{
    struct out o = {buf, size, 0};
    for (; *f; f++) {
        if (*f != '%') {
            put(&o, *f);
            continue;
        }
        f++;
        char pad = ' ';
        int width = 0, lng = 0, left = 0;
        if (*f == '-')
            left = 1, f++;
        if (*f == '0')
            pad = '0', f++;
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (*f++ - '0');
        int prec = -1; /* %.Ns / %.*s: at most N chars */
        if (*f == '.') {
            f++, prec = 0;
            if (*f == '*')
                prec = __builtin_va_arg(ap, int), f++;
            else
                while (*f >= '0' && *f <= '9')
                    prec = prec * 10 + (*f++ - '0');
        }
        while (*f == 'l' || *f == 'z' || *f == 'j')
            lng = 1, f++;
        switch (*f) {
        case 'd':
        case 'i':
            put_num(&o, lng ? __builtin_va_arg(ap, s64) : (s64)__builtin_va_arg(ap, int), 10, 1, width, pad, 0);
            break;
        case 'u':
            put_num(&o, lng ? __builtin_va_arg(ap, u64) : __builtin_va_arg(ap, u32), 10, 0, width, pad, 0);
            break;
        case 'x':
        case 'X':
            put_num(&o, lng ? __builtin_va_arg(ap, u64) : __builtin_va_arg(ap, u32), 16, 0, width, pad, *f == 'X');
            break;
        case 'p':
            put(&o, '0'), put(&o, 'x');
            put_num(&o, (u64)__builtin_va_arg(ap, void *), 16, 0, width, pad, 0);
            break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int n = 0;
            while (s[n] && (prec < 0 || n < prec))
                n++;
            if (!left)
                for (int w = n; w < width; w++)
                    put(&o, ' ');
            for (int i = 0; i < n; i++)
                put(&o, s[i]);
            if (left)
                for (int w = n; w < width; w++)
                    put(&o, ' ');
            break;
        }
        case 'c':
            put(&o, (char)__builtin_va_arg(ap, int));
            break;
        case '%':
            put(&o, '%');
            break;
        default:
            put(&o, '%');
            put(&o, *f);
        }
    }
    if (o.size)
        o.buf[o.len < o.size ? o.len : o.size - 1] = 0;
    return (int)o.len;
}

int fmt(char *buf, size_t size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int r = vfmt(buf, size, f, ap);
    __builtin_va_end(ap);
    return r;
}

char *strstr(const char *h, const char *n)
{
    size_t ln = strlen(n);
    for (; *h; h++)
        if (strncmp(h, n, ln) == 0)
            return (char *)h;
    return ln ? NULL : (char *)h;
}
