/* Map, relocate, and publish Orbis modules inside the host process.
 *
 * Code (the executable PT_LOAD, including its rodata) lives in a JIT block
 * created by the ps2emu compiler process and mapped RX at the module base;
 * everything else is flexible memory mapped RW at its fixed offset. */
#include "ghost.h"

#define R_X86_64_64 1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_DTPMOD64 16
#define R_X86_64_DTPOFF64 17
#define R_X86_64_TPOFF64 18
#define STB_WEAK 2

const char *module_import_lib(struct module *m, const char *encoded, char *nid, const char **modname);
const char *module_export_lib(struct module *m, const char *encoded);

struct module *g_modules;
static u32 g_next_tls_id = 0x1000;
static u32 g_next_handle = 0x7000;

struct module *module_load_file(const char *path, bool is_main)
{
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0) {
        logf("open %s failed %x", path, fd);
        return NULL;
    }
    off_t size = (off_t)vfs_lseek(fd, 0, 2);
    vfs_lseek(fd, 0, 0);
    u8 *buf = page_alloc((size_t)size);
    if (!buf) {
        vfs_close(fd);
        return NULL;
    }
    off_t got = 0;
    while (got < size) {
        ssize_t n = vfs_read(fd, buf + got, (size_t)(size - got) > 0x1000000 ? 0x1000000 : (size_t)(size - got));
        if (n <= 0) {
            logf("read %s failed at %lx: %lx", path, got, n);
            vfs_close(fd);
            return NULL;
        }
        got += n;
    }
    vfs_close(fd);
    struct module *m = arena_alloc(sizeof(*m));
    const char *slash = strrchr_c(path, '/');
    m->name = arena_strdup(slash ? slash + 1 : path);
    m->is_main = is_main;
    m->fake_handle = g_next_handle++;
    if (module_parse(m, buf, (u64)size) != 0)
        return NULL;
    if (m->tls_memsz && !is_main)
        m->tls_id = g_next_tls_id++;
    /* append, keeping load order */
    struct module **pp = &g_modules;
    while (*pp)
        pp = &(*pp)->next;
    *pp = m;
    return m;
}

int module_place(struct module *m, u64 base)
{
    m->base = base;
    if (!arena_is_rx(base, m->text_size)) {
        logf("%s: text %lx+%lx not inside the RX arena", m->name, base, m->text_size);
        return -1;
    }
    for (u32 i = 0; i < m->phnum; i++) {
        struct elf_phdr *p = &m->phdrs[i];
        if (p->type == PT_SCE_RELRO) {
            m->relro_lo = ALIGN_DOWN(p->vaddr, 0x4000);
            m->relro_size = ALIGN_UP(p->vaddr + p->memsz, 0x4000) - m->relro_lo;
            if (!arena_is_rx(base + m->relro_lo, m->relro_size)) {
                logf("%s: relro not inside the RX arena", m->name);
                return -1;
            }
            m->relro_shadow = page_alloc(m->relro_size);
            if (!m->relro_shadow)
                return -1;
            if (m->seg_data[i])
                memcpy(m->relro_shadow + (p->vaddr - m->relro_lo), m->seg_data[i], p->filesz);
        } else if (p->type == PT_LOAD && !(p->flags & 1)) {
            u64 lo = ALIGN_DOWN(p->vaddr, 0x4000), hi = ALIGN_UP(p->vaddr + p->memsz, 0x4000);
            if (arena_punch_rw(base + lo, hi - lo) != 0)
                return -1;
            if (p->filesz && m->seg_data[i])
                memcpy((void *)(base + p->vaddr), m->seg_data[i], p->filesz);
        }
    }
    if (m->proc_param)
        m->proc_param += m->base;
    logf("%s: placed at %lx (text %lx, relro %lx+%lx)", m->name, m->base, m->text_size, m->relro_lo,
         m->relro_size);
    return 0;
}

/* Export table, sorted by NID for binary search. */
static void sort_exports(struct export_ent *e, u32 n)
{
    for (u32 gap = n / 2; gap; gap /= 2)
        for (u32 i = gap; i < n; i++) {
            struct export_ent t = e[i];
            u32 j = i;
            for (; j >= gap && strcmp(e[j - gap].nid, t.nid) > 0; j -= gap)
                e[j] = e[j - gap];
            e[j] = t;
        }
}

static void build_exports(struct module *m)
{
    u32 n = 0;
    for (u64 i = 0; i < m->nsyms; i++) {
        const struct elf_sym *s = &m->symtab[i];
        if (s->shndx && (s->info >> 4) != 0 && s->name)
            n++;
    }
    m->exports = arena_alloc(sizeof(struct export_ent) * (n + 1));
    for (u64 i = 0; i < m->nsyms; i++) {
        const struct elf_sym *s = &m->symtab[i];
        if (!s->shndx || (s->info >> 4) == 0 || !s->name)
            continue;
        const char *enc = module_sym_name(m, s);
        struct export_ent *e = &m->exports[m->nexports++];
        u32 k = 0;
        for (; k < 11 && enc[k] && enc[k] != '#'; k++)
            e->nid[k] = enc[k];
        e->nid[k] = 0;
        /* copied: the executable's file buffer is freed after loading */
        const char *lib = module_export_lib(m, enc);
        e->lib = (n > 0 && m->nexports > 1 && e[-1].lib && lib && strcmp(e[-1].lib, lib) == 0) ? e[-1].lib
                 : lib ? arena_strdup(lib) : NULL;
        e->addr = m->base + s->value;
    }
    sort_exports(m->exports, m->nexports);
}

u64 module_export(struct module *m, const char *nid)
{
    u32 lo = 0, hi = m->nexports;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        int c = strcmp(m->exports[mid].nid, nid);
        if (c == 0)
            return m->exports[mid].addr;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

static u64 sym_value(struct module *m, u32 index, u32 *missing, bool plt)
{
    if (index == 0 || index >= m->nsyms)
        return 0;
    const struct elf_sym *s = &m->symtab[index];
    if (s->shndx)
        return m->base + s->value;
    /* only PLT slots of function symbols may be routed through stubs */
    u64 v = resolve_import(m, module_sym_name(m, s), (s->info >> 4) == STB_WEAK, plt && (s->info & 0xf) == 2);
    if (!v)
        (*missing)++;
    return v;
}

static int apply(struct module *m, const struct elf_rela *r, u64 n, u32 *missing, u32 *unsupported)
{
    for (u64 i = 0; i < n; i++) {
        u32 type = (u32)r[i].info, sym = (u32)(r[i].info >> 32);
        u64 *where;
        if (r[i].offset < m->text_size) {
            (*unsupported)++;
            continue;
        }
        if (m->relro_shadow && r[i].offset >= m->relro_lo && r[i].offset < m->relro_lo + m->relro_size)
            where = (u64 *)(m->relro_shadow + (r[i].offset - m->relro_lo));
        else
            where = (u64 *)(m->base + r[i].offset);
        switch (type) {
        case R_X86_64_RELATIVE:
            *where = m->base + r[i].addend;
            break;
        case R_X86_64_64:
            *where = sym_value(m, sym, missing, false) + r[i].addend;
            break;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            *where = sym_value(m, sym, missing, type == R_X86_64_JUMP_SLOT);
            break;
        case R_X86_64_DTPMOD64:
            *where = m->tls_id;
            break;
        case R_X86_64_DTPOFF64:
            *where = (sym ? m->symtab[sym].value : 0) + r[i].addend;
            break;
        default:
            (*unsupported)++;
            logf("%s: unsupported reloc type %u at %lx", m->name, type, r[i].offset);
        }
    }
    return 0;
}

int module_relocate(struct module *m)
{
    u32 missing = 0, unsupported = 0;
    apply(m, m->rela, m->nrela, &missing, &unsupported);
    apply(m, m->jmprel, m->njmprel, &missing, &unsupported);
    logf("%s: relocated (%lu rela, %lu plt) missing %u unsupported %u", m->name, m->nrela, m->njmprel,
         missing, unsupported);
    return unsupported ? -1 : 0;
}

/* Rewrite each relaxed local-exec TLS load "mov rax, fs:[0]" in the main
 * executable to "mov rax, fs:[-8]": the host's 8-byte main-module TLS slot
 * then points at a per-thread Ghost TLS block (see tls.c). */
static u32 patch_tls(u8 *code, u64 size)
{
    static const u8 pat[12] = {0x66, 0x66, 0x66, 0x64, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
    u32 n = 0;
    for (u64 i = 0; i + sizeof(pat) <= size; i++) {
        if (code[i] == 0x66 && memcmp(code + i, pat, sizeof(pat)) == 0) {
            *(u32 *)(code + i + 8) = 0xFFFFFFF8u;
            n++;
            i += sizeof(pat) - 1;
        }
    }
    return n;
}

int module_finish(struct module *m)
{
    build_exports(m);
    return 0;
}

int module_write_text(struct module *m)
{
    u8 *buf = page_alloc(m->text_size);
    if (!buf)
        return -1;
    for (u32 i = 0; i < m->phnum; i++) {
        struct elf_phdr *p = &m->phdrs[i];
        if (p->type == PT_LOAD && (p->flags & 1) && m->seg_data[i])
            memcpy(buf + p->vaddr, m->seg_data[i], p->filesz);
    }
    if (m->is_main && m->tls_memsz) {
        u32 n = patch_tls(buf, m->text_size);
        logf("%s: patched %u TLS sites", m->name, n);
    }
    if (m->is_main && m->file) {
        /* everything that needed the file (relocation, exports, TLS image,
         * text) is done: give its ~26 MiB back before the memory plan */
        page_free(m->file, m->file_size);
        m->file = NULL;
        m->strtab = NULL, m->symtab = NULL, m->dynlib = NULL, m->nsyms = 0;
        for (u32 i = 0; i < ARRAY_SIZE(m->seg_data); i++)
            m->seg_data[i] = NULL;
    }
    if (m->is_main) {
        if (budget_patch_text(m, buf) != 0) { /* the title cannot fit: stop before it starts */
            page_free(buf, m->text_size);
            return -1;
        }
        hooks_patch_text(m, buf);
    }
    int rc = arena_write(m->base, buf, m->text_size);
    page_free(buf, m->text_size);
    if (rc == 0 && m->relro_shadow) {
        rc = arena_write(m->base + m->relro_lo, m->relro_shadow, m->relro_size);
        page_free(m->relro_shadow, m->relro_size);
        m->relro_shadow = NULL;
    }
    u64 probe = *(volatile u64 *)m->base;
    logf("%s: text written rc=%d first qword %lx", m->name, rc, probe);
    return rc;
}

void module_protect_relro(struct module *m)
{
    (void)m; /* RELRO lives in the RX arena: already read-only here */
}
