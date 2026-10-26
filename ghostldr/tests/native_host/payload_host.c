/*
 * The ELF loader's payload.c (and elf.c, for Orbis tables) in a host
 * process: test_elfldr.py builds and drives it.
 *
 *     payload_host FILE BASE_HEX IMAGE_OUT
 *
 * Parses FILE as the console would, then builds its image for BASE (a
 * number only: nothing runs) with a resolver that gives every import its
 * own address, and writes the image to IMAGE_OUT.  Prints what it found:
 *
 *     parse ok abi orbis sce 0 exec_end 4000 image 8000 relro 0-0 tls 8/10/8 entry 1040
 *     needed libkernel
 *     import NAME weak 0 kind 1 -> 7000000000
 *     build ok tls_offset 10 tls_id 1000 info "3 TLS sites"
 *
 * or "parse fail: WHY" / "build fail: WHY".  An import named "missing_*"
 * resolves to 0, as an absent one does.
 */
#include "ghost.h"
#include "elfldr.h"

long write(int fd, const void *buf, unsigned long n);
int open(const char *path, int flags, ...);
long read(int fd, void *buf, unsigned long n);
int close(int fd);
int vsnprintf(char *buf, unsigned long size, const char *fmt, __builtin_va_list ap);
void *calloc(unsigned long n, unsigned long size);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);

struct boot_ctx *g_ctx;

int vfmt(char *buf, size_t size, const char *f, __builtin_va_list ap) { return vsnprintf(buf, size, f, ap); }

int fmt(char *buf, size_t size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(buf, size, f, ap);
    __builtin_va_end(ap);
    return n;
}

void logf(const char *f, ...)
{
    char line[1200];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(line, sizeof(line) - 1, f, ap);
    __builtin_va_end(ap);
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    write(2, line, n);
}

static void out(const char *f, ...)
{
    char line[1200];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(line, sizeof(line) - 1, f, ap);
    __builtin_va_end(ap);
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    write(1, line, n);
}

/* elf.c's needs */
void profile_bind(const u8 *fp) {}
int pl_parse_sce(struct module *m, u8 *file, u64 size) { return module_parse(m, file, size); }

static u32 g_imports;

static u64 resolve(void *ctx, struct module *m, const char *name, bool weak, int kind)
{
    u64 a = strncmp(name, "missing_", 8) == 0 ? 0 : 0x7000000000UL + 0x10UL * g_imports++;
    out("import %s weak %d kind %d -> %lx", name, weak, kind, a);
    return a;
}

int main(int argc, char **argv)
{
    if (argc < 4)
        return 2;
    int fd = open(argv[1], 0);
    if (fd < 0)
        return 3;
    u64 cap = 64UL << 20, size = 0;
    u8 *file = calloc(1, cap);
    for (long r; (r = read(fd, file + size, cap - size)) > 0;)
        size += (u64)r;
    close(fd);
    struct module *m = calloc(1, sizeof(*m));
    m->name = "payload";
    m->payload = true;
    struct pl_info info;
    char why[300];
    if (pl_parse(m, file, size, &info, why, sizeof(why)) != 0) {
        out("parse fail: %s", why);
        return 0;
    }
    out("parse ok abi %s sce %d exec_end %lx image %lx relro %lx-%lx tls %lx/%lx/%lx entry %lx",
        info.abi == PL_ORBIS ? "orbis" : "free", info.sce, info.exec_end, info.image_size, info.relro_lo,
        info.relro_hi, m->tls_filesz, m->tls_memsz, m->tls_align, m->entry);
    for (u32 i = 0; i < m->n_needed_files; i++)
        out("needed %s", m->needed_files[i]);
    m->base = strtoul(argv[2], NULL, 16);
    if (m->tls_memsz)
        m->tls_offset = ALIGN_UP(m->tls_memsz, m->tls_align > 16 ? m->tls_align : 16);
    m->tls_id = 0x1000;
    u8 *img = calloc(1, info.image_size);
    if (pl_build(m, &info, img, resolve, NULL, why, sizeof(why)) != 0) {
        out("build fail: %s", why);
        return 0;
    }
    out("build ok tls_offset %lx tls_id %x info \"%s\"", m->tls_offset, m->tls_id, why);
    int o = open(argv[3], 0x601 /* O_WRONLY | O_CREAT | O_TRUNC */, 0644);
    if (o < 0)
        return 4;
    write(o, img, info.image_size);
    close(o);
    return 0;
}
