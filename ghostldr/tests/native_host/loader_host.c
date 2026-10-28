/*
 * Runs the core's elf.c and loader.c in a host process on real files: an
 * app0 directory's eboot.bin and the named sce_module prx are parsed, placed in a
 * buffer, relocated against a fake resolver (every import gets its own
 * address) and their text written, as boot.c does.  test_loader.py builds
 * and drives it:
 *
 *     loader_host APP0 [PRX...]
 *
 * Prints one line per module: its entry and DT_INIT, relocations (missing,
 * unsupported), TLS sites patched, and what its text starts with.
 */
#include "ghost.h"

long write(int fd, const void *buf, unsigned long n);
int open(const char *path, int flags, ...);
long read(int fd, void *buf, unsigned long n);
long lseek(int fd, long off, int whence);
int close(int fd);
int stat(const char *path, void *st);
int vsnprintf(char *buf, unsigned long size, const char *fmt, __builtin_va_list ap);
void *calloc(unsigned long n, unsigned long size);
void free(void *p);
char *strcat(char *d, const char *s);
char *strchr(const char *s, int c);

struct kfuncs K;
struct boot_ctx *g_ctx;
struct module *g_main_mod;
u64 g_reported_dmem;

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
    write(1, line, n);
}

const char *strrchr_c(const char *s, int c)
{
    const char *r = NULL;
    for (; *s; s++)
        if (*s == c)
            r = s;
    return r;
}

/* ---- what loader.c and elf.c use ---- */
void *page_alloc(size_t size) { return calloc(1, size ? size : 1); }
void page_free(void *addr, size_t size)
{
    memset(addr, 0x5A, size); /* what the core frees is gone: reading it shows */
    free(addr);
}
void *arena_alloc(size_t size) { return calloc(1, size ? size : 1); }
char *arena_strdup(const char *s)
{
    char *d = calloc(1, strlen(s) + 1);
    strcpy(d, s);
    return d;
}
int vfs_open(const char *path, int flags, int mode) { return open(path, flags); }
s64 vfs_read(int fd, void *buf, u64 n) { return read(fd, buf, n); }
s64 vfs_lseek(int fd, s64 off, int whence) { return lseek(fd, off, whence); }
int vfs_close(int fd) { return close(fd); }
void profile_bind(const u8 *fp) {}
const void *profile_record(u32 kind, u32 n) { return NULL; } /* no profile: no TLS site lists */
void stubs_flush(void) {}
bool module_has_tpoff(const struct module *m) { return false; }
u64 g_punched;
/* large executables (bigexec.c): none here, so the whole text is scanned */
int bx_place(struct module *m) { return -1; }
int bx_write_text(struct module *m, const u8 *text) { return -1; }
u32 bx_code(const struct module *m, const struct bx_range **runs) { return 0; }
int vfs_stat(const char *path, void *st) { return stat(path, st); }
int budget_patch_text(struct module *m, u8 *text) { return 0; }
void hooks_patch_text(struct module *m, u8 *text) {}
void ext_patch_text(struct module *m, u8 *text, u64 size) {}

/* one buffer stands in for the code region; all of it counts as RX */
#define REGION (128UL << 20)
static u8 *g_region;
static u64 g_cursor;
u64 arena_code_alloc(u64 size)
{
    u64 at = ALIGN_UP(g_cursor, 0x10000);
    g_cursor = at + size;
    return at;
}
bool arena_is_rx(u64 addr, u64 size) { return addr >= (u64)g_region && addr + size <= (u64)g_region + REGION; }
int arena_punch_rw(u64 addr, u64 size) { return arena_is_rx(addr, size) ? 0 : -1; }
int arena_write(u64 dst, const void *src, u64 size)
{
    memcpy((void *)dst, src, size);
    return 0;
}

/* every import its own fake address */
static u32 g_imports;
u64 resolve_import(struct module *m, const char *encoded, bool weak, int kind)
{
    return 0x100000000UL + 16UL * g_imports++;
}

int module_place(struct module *m, u64 base);
int module_finish(struct module *m);
int module_relocate(struct module *m);
int module_write_text(struct module *m);
extern struct module *g_modules;

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    g_region = calloc(1, REGION);
    g_cursor = (u64)g_region;
    char path[600];
    for (int i = 2; i < argc; i++) {
        fmt(path, sizeof(path), "%s/sce_module/%s", argv[1], argv[i]);
        struct module *m = module_open(path, false);
        if (!m)
            return 3;
        module_publish(m);
    }
    fmt(path, sizeof(path), "%s/eboot.bin", argv[1]);
    g_main_mod = module_open(path, true);
    if (!g_main_mod)
        return 4;
    module_publish(g_main_mod);
    for (struct module *m = g_modules; m; m = m->next)
        if (!m->is_main && module_place(m, arena_code_alloc(m->image_size)) != 0)
            return 5;
    if (module_place(g_main_mod, arena_code_alloc(g_main_mod->image_size)) != 0)
        return 5;
    /* the executable's static TLS block, as tls.c lays it out (variant II) */
    if (g_main_mod->tls_memsz)
        g_main_mod->tls_offset = ALIGN_UP(g_main_mod->tls_memsz, g_main_mod->tls_align > 16 ? g_main_mod->tls_align : 16);
    for (struct module *m = g_modules; m; m = m->next)
        module_finish(m);
    int rc = 0;
    for (struct module *m = g_modules; m; m = m->next)
        rc |= module_relocate(m);
    for (struct module *m = g_modules; m; m = m->next) {
        if (module_write_text(m) != 0)
            return 6;
        /* what a module start would jump to (boot.c: DT_INIT, else e_entry) */
        const u8 *start = (const u8 *)(m->base + (m->init ? m->init : m->entry));
        logf("module %s main %d entry %lx init %lx start bytes %02x%02x%02x%02x proc param %s tls %lx/%lx",
             m->name, m->is_main, m->entry, m->init, start[0], start[1], start[2], start[3],
             m->proc_param ? "yes" : "no", m->tls_filesz, m->tls_memsz);
    }
    const u8 *t = (const u8 *)g_main_mod->base + 0x40;
    char hex[0x81];
    for (u32 i = 0; i < 0x40; i++)
        fmt(hex + 2 * i, 3, "%02x", t[i]);
    logf("main text 40..80 %s", hex);
    char libs[256] = "";
    for (u32 i = 0; i < g_main_mod->n_import_libs; i++)
        strcat(libs, g_main_mod->import_libs[i].name), strcat(libs, " ");
    logf("main module %s imports %s", g_main_mod->modname, libs);
    logf("imports %u, relocation rc %d", g_imports, rc);
    return 0;
}
