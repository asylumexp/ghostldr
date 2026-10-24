/*
 * Runs the core's arena.c and ps4.c in a host process (built for this
 * machine) against a fake console: this process's executable regions, laid
 * out as a PS5 (13.60) and a PS4 (12.02) boot logged them (rebased to free
 * memory here, same offsets within 64 KiB; addresses print relative to the
 * IPU buffer), and a fake compiler process with its own map.  A compiler
 * write where that map has nothing writable prints FAULT and exits 3, as the
 * PS4's compiler crashed.  test_ps4.py builds and drives it:
 *
 *     arena_host SCENARIO [small]
 *
 * SCENARIO: ps5 (no PS4 flag: today's path), ps4-header (the compiler writes
 * each buffer past its first 16 KiB), ps4-none (nothing past the IPU buffer),
 * ps4-other (the EE buffer writable there, but other memory), ps4-vu1 (only
 * VU1's buffer), ps4-noblock (nothing, and no new JIT block either),
 * ps4-noflag (ps4-header without the PS4 flag), and old-rule (ps4-header,
 * the first module where v0.1.1 put it).  small: the title's modules
 * fit in 16 MiB (default: Ghost's sizes from the PS4 log).  ps4-report:
 * ps4-header with the core of the second PS4 report (12.02, text 176 KiB,
 * image 224 KiB: core at IPU+16 KiB, small area 914b14000-914b1c000);
 * ps4-nocheck: that layout, with the compiler checks (jit_ps4_check) failing.
 * Also: arena_host heap -- ps4_keep_heap's cases; arena_host layout -- the
 * boot context's offsets that ghostldr.lua writes; arena_host ps5 drop -- the
 * buffers bigexec.c can take, and the arena after it took two.
 */
#include "ghost.h"

int printf(const char *f, ...);
int vsnprintf(char *buf, unsigned long size, const char *f, __builtin_va_list ap);
void exit(int code);
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);

#define MAP_FAILED_ ((void *)-1)
void *malloc(size_t n);
void *calloc(size_t n, size_t m);

struct kfuncs K;
struct boot_ctx *g_ctx;
struct module *g_modules, *g_main_mod;
static struct boot_ctx ctx;

void logf(const char *f, ...)
{
    char line[1000];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    vsnprintf(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    printf("log: %s\n", line);
}

/* ---- this process's map: executable regions (prot 5) ---- */
struct range {
    u64 start, end;
    int prot;
    u8 *host; /* compiler entries: where its bytes live here (NULL: unmapped) */
};
static struct range g_main[16], g_comp[32];
static u32 g_nmain, g_ncomp;
static bool g_block_ok = true, g_check_ok = true;
static u64 g_ipu; /* addresses print relative to it: the consoles' own, rebased here */

static int fake_qmp(void *addr, void **st, void **en, int *prot)
{
    for (u32 i = 0; i < g_nmain; i++)
        if ((u64)addr >= g_main[i].start && (u64)addr < g_main[i].end) {
            *st = (void *)g_main[i].start, *en = (void *)g_main[i].end, *prot = g_main[i].prot;
            return 0;
        }
    return (int)0x80020016;
}

void *host_dlsym(u32 handle, const char *name)
{
    return strcmp(name, "sceKernelQueryMemoryProtection") == 0 ? (void *)fake_qmp : NULL;
}

/* ---- the fake compiler process ---- */
static struct range *comp_at(u64 addr, u64 len, int need)
{
    for (u32 i = 0; i < g_ncomp; i++)
        if (addr >= g_comp[i].start && addr + len <= g_comp[i].end && (g_comp[i].prot & need) == need)
            return &g_comp[i];
    return NULL;
}

/* the probes: EFAULT (-1) where the compiler may not read or write */
int jit_peek(u64 addr, void *out, u32 n)
{
    struct range *r = comp_at(addr, n, PROT_READ);
    if (!r)
        return -1;
    memcpy(out, r->host + (addr - r->start), n);
    return 0;
}

int jit_stamp(u64 addr)
{
    static u64 clock = 1000;
    struct range *r = comp_at(addr, 16, PROT_WRITE);
    if (!r)
        return -1;
    u64 ts[2] = {clock++, 123456789};
    memcpy(r->host + (addr - r->start), ts, 16);
    return 0;
}

int jit_copy_to(u64 dst, const void *src, size_t len)
{
    struct range *r = comp_at(dst, len, PROT_WRITE);
    if (!r) {
        printf("FAULT compiler write at +%lx\n", dst - g_ipu);
        exit(3);
    }
    memcpy(r->host + (dst - r->start), src, len);
    return 0;
}

int jit_block_create(struct jit_block *b, u64 size, u64 want_rx)
{
    memset(b, 0, sizeof(*b));
    b->base_fd = b->rw_fd = b->rx_fd_compiler = b->rx_fd = -1;
    b->size = ALIGN_UP(size, 0x4000);
    if (!g_block_ok)
        return -1;
    b->rw = 0x7700000000UL;
    b->rx = want_rx;
    g_comp[g_ncomp++] = (struct range){b->rw, b->rw + b->size, 3, (u8 *)want_rx};
    printf("block rx +%lx size %lx\n", b->rx - g_ipu, b->size);
    return 0;
}

void jit_block_destroy(struct jit_block *b) { memset(b, 0, sizeof(*b)); }

int jit_ps4_check(void)
{
    printf("compiler check %s\n", g_check_ok ? "ok" : "FAILED");
    return g_check_ok ? 0 : -1;
}

/* ---- layouts (from the PS5 13.60 and PS4 12.02 logs; core image 0x34000, text 0x28000,
 * or as given: the second PS4 report's core is 0x38000 and 0x2c000) ---- */
static void add_main(u64 s, u64 e) { g_main[g_nmain++] = (struct range){s, e, 5, NULL}; }

/* a JIT program buffer: here [s, e) executable; the compiler per model */
static void buffer(u64 s, u64 e, const char *model, bool ee)
{
    add_main(s, e);
    if (!strcmp(model, "ps4-none") || !strcmp(model, "ps4-noblock") || (!strcmp(model, "ps4-vu1") && e - s != 0x1000000))
        return;
    g_comp[g_ncomp++] = (struct range){s, s + JIT_BUF_HEADER, 5, (u8 *)s}; /* the header: not writable */
    u8 *host = (u8 *)(s + JIT_BUF_HEADER);
    if (ee && !strcmp(model, "ps4-other"))
        host = calloc(1, e - s); /* writable there, but not this memory */
    g_comp[g_ncomp++] = (struct range){s + JIT_BUF_HEADER, e, 3, host};
}

static u64 g_ee; /* the EE buffer: the largest */

/* the consoles' layout at an address this machine has free, keeping its
 * position within 64 KiB (all that placement depends on) */
static void layout(u64 console_ipu, const char *model, u64 core_text, u64 core_image)
{
    u8 *p = mmap(NULL, 0x4058000 + 0x10000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED_) {
        printf("SKIP no memory\n");
        exit(77);
    }
    u64 ipu = g_ipu = ALIGN_UP((u64)p, 0x10000) + (console_ipu & 0xffff), core = ipu + 0x4000;
    if (ipu + 0x4058000 > (u64)p + 0x4058000 + 0x10000)
        ipu = g_ipu = ipu - 0x10000, core = ipu + 0x4000;
    ctx.reserved[2] = ipu, ctx.reserved[3] = core + core_image;
    printf("ipu %lx\n", ipu);
    add_main(ipu, core);                         /* the IPU buffer's header (16 KiB: not a region) */
    add_main(core, core + core_text);            /* the core's text; its data (RW) follows */
    add_main(core + core_image, ipu + 0x44000);  /* the small area (48 or 32 KiB) */
    g_comp[g_ncomp++] = (struct range){ipu, core, 5, (u8 *)ipu};
    g_comp[g_ncomp++] = (struct range){core, ipu + 0x44000, 3, (u8 *)core};
    u64 ee = g_ee = ipu + 0x48000;
    buffer(ee, ee + 0x2000000, model, true);
    buffer(ee + 0x2004000, ee + 0x2804000, model, false);
    buffer(ee + 0x2808000, ee + 0x3008000, model, false);
    buffer(ee + 0x300c000, ee + 0x400c000, model, false);
}

/* ---- the title's modules: Ghost's sizes from the PS4 log, or a small title ---- */
static struct elf_phdr g_relro = {PT_SCE_RELRO, 4, 0, 0x1400000, 0, 0x180000, 0x180000, 0x4000};
static struct module g_mods[4];

static void modules(bool small)
{
    static const struct {
        const char *name;
        u64 image, text;
    } k[] = {{"libSceFios2.prx", 0x190000, 0x60000}, {"libc.prx", 0x120000, 0x114000},
             {"libSceNpToolkit2.prx", 0xbc000, 0xa8000}, {"eboot.bin", 0x4144000, 0x133c000}};
    for (u32 i = 0; i < 4; i++) {
        g_mods[i].name = k[i].name;
        g_mods[i].image_size = small && i == 3 ? 0x900000 : k[i].image;
        g_mods[i].text_size = small && i == 3 ? 0x600000 : k[i].text;
        g_mods[i].is_main = i == 3;
        g_mods[i].next = i < 3 ? &g_mods[i + 1] : NULL;
    }
    if (!small)
        g_mods[3].phdrs = &g_relro, g_mods[3].phnum = 1;
}

static u64 place(struct module *m)
{
    u64 base = arena_code_alloc(m->image_size);
    bool rx = arena_is_rx(base, m->text_size) && (!m->phnum || arena_is_rx(base + 0x1400000, 0x180000));
    u8 text[64];
    memset(text, 0x90, sizeof(text));
    int rc = rx ? arena_write(base, text, sizeof(text)) : -9;
    printf("module %s at +%lx rx %d write %d\n", m->name, base - g_ipu, rx, rc);
    return base;
}

int main(int argc, char **argv)
{
    g_ctx = &ctx;
    if (!strcmp(argv[1], "layout")) { /* what ghostldr.lua must write where */
        printf("ctx size %lx flags %lx jit_syscall %lx reserved %lx sel_crc %lx fw %lx jit_sysw %lx\n",
               sizeof(struct boot_ctx), __builtin_offsetof(struct boot_ctx, flags),
               __builtin_offsetof(struct boot_ctx, jit_syscall), __builtin_offsetof(struct boot_ctx, reserved),
               __builtin_offsetof(struct boot_ctx, sel_crc), __builtin_offsetof(struct boot_ctx, fw),
               __builtin_offsetof(struct boot_ctx, jit_sysw));
        return 0;
    }
    if (!strcmp(argv[1], "heap")) {
        u64 s = 0x201014000, e = 0x211014000, h = 0x201014040;
        printf("heap %d %d %d %d\n", ps4_keep_heap(s, e, h, false), ps4_keep_heap(s, e, h, true),
               ps4_keep_heap(e, e + 0x1000000, h, false), ps4_keep_heap(s, e, 0, false));
        return 0;
    }
    const char *model = argv[1];
    bool ps5 = !strcmp(model, "ps5"), noflag = !strcmp(model, "ps4-noflag");
    bool report = !strcmp(model, "ps4-report") || !strcmp(model, "ps4-nocheck");
    g_check_ok = strcmp(model, "ps4-nocheck") != 0;
    ctx.flags = ps5 || noflag || !strcmp(model, "old-rule") ? 0 : CTX_FLAG_PS4;
    g_block_ok = strcmp(model, "ps4-noblock") != 0;
    layout(ps5 ? 0x9009d4000 : 0x914ad8000, ps5 || model[0] == 'o' || noflag || report ? "ps4-header" : model,
           report ? 0x2c000 : 0x28000, report ? 0x38000 : 0x34000);
    modules(argc > 2 && !strcmp(argv[2], "small"));
    if (model[0] == 'o') { /* v0.1.1 put the first module at the EE buffer's 64 KiB-aligned start */
        u8 text[64] = {0};
        printf("old first module +%lx\n", ALIGN_UP(g_ee, 0x10000) - g_ipu);
        jit_copy_to(ALIGN_UP(g_ee, 0x10000), text, sizeof(text));
        return 0;
    }

    int init = arena_init();
    printf("init %d\n", init);
    if (init != 0) /* boot.c, sel.c and diag.c stop here */
        return 0;
    u64 lo = 0, hi = 0;
    int sel = arena_code_region(&lo, &hi);
    printf("selector region %d +%lx-+%lx\n", sel, sel ? 0 : lo - g_ipu, sel ? 0 : hi - g_ipu);
    if (argc > 2 && !strcmp(argv[2], "drop")) { /* bigexec.c takes the EE and VU1 buffers */
        u64 st[8], en[8];
        u32 n = arena_buffers(st, en, 8);
        for (u32 i = 0; i < n; i++)
            printf("buffer +%lx-+%lx\n", st[i] - g_ipu, en[i] - g_ipu);
        arena_drop(g_ee);
        arena_drop(g_ee + 0x300c000);
        n = arena_buffers(st, en, 8);
        printf("left %u\n", n);
        g_modules = &g_mods[0];
        int rc = arena_code_region(&lo, &hi);
        printf("code region %d +%lx-+%lx\n", rc, rc ? 0 : lo - g_ipu, rc ? 0 : hi - g_ipu);
        g_mods[3].next = NULL, g_mods[2].next = NULL; /* libc and Fios2 only */
        g_mods[1].next = NULL;
        for (struct module *m = g_modules; m; m = m->next)
            place(m);
        return 0;
    }
    g_modules = &g_mods[0];
    int rc = arena_code_region(&lo, &hi);
    printf("code region %d +%lx-+%lx\n", rc, rc ? 0 : lo - g_ipu, rc ? 0 : hi - g_ipu);
    if (rc != 0)
        return 0;
    for (struct module *m = g_modules; m; m = m->next)
        place(m);
    u64 stubs = arena_small_alloc(512 * 32);
    u8 code[32] = {0xC3};
    printf("stubs at +%lx write %d\n", stubs ? stubs - g_ipu : 0, stubs ? arena_write(stubs, code, sizeof(code)) : -9);
    u64 hw = arena_wr(g_ee, 8);
    printf("header write %s\n", !hw ? "refused" : hw == g_ee ? "same address" : "elsewhere");
    printf("need %lx\n", jv_code_need(g_modules));
    return 0;
}
